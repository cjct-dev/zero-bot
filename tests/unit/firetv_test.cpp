// Known-answer and offline tests for firetv.cpp. Nothing here opens a connection except to
// 127.0.0.1 port 1, where nothing listens, to show that refused input never reaches the network.
// Each check prints one line; the exit status is non-zero when any fails.
//
//   firetv_test [folder holding test_cert_a.pem and test_cert_b.pem]   (default: .)
//
// Where the expected values come from:
//   firehose   docs/protocol.md and FireTvClientTest.kt of firehose-remote (MIT): the token
//              examples, the reply shapes and the measured capability objects
//   forum      the Home Assistant forum thread 901314 (the token example "Wj9PP8I")
//   openssl    the openssl command line, independently of this code:
//              openssl x509 -in test_cert_X.pem -pubkey -noout | openssl pkey -pubin -outform der |
//              openssl dgst -sha256 -binary | openssl enc -base64 -A
//   spec       the Wake-on-LAN magic packet layout (6 x FF, then the address 16 times)
#include "smarthome/drivers/firetv.h"

#include <arpa/inet.h>
#include <curl/curl.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

using namespace firetv;
using namespace firetv::detail;

// The words src/smarthome/control.cpp unreachable() looks for (lower case) to decide that a
// device may have moved; the app then scans the network for 2.5 s and may apply the change again.
static bool reads_as_unreachable(std::string err) {
    for (char &c : err) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (const char *w : {"connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable"})
        if (err.find(w) != std::string::npos) return true;
    return false;
}

static uint32_t ip4(const char *text) {
    in_addr a{};
    inet_pton(AF_INET, text, &a);
    return ntohl(a.s_addr);
}

static std::string ip4_text(uint32_t host_order) {
    in_addr a{};
    a.s_addr = htonl(host_order);
    char buf[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

// "to<-from;..." for a list of magic-packet destinations ("any" for an unbound socket).
static std::string sends(const std::vector<WakeSend> &list) {
    std::string out;
    for (const WakeSend &w : list) out += ip4_text(w.to) + "<-" + (w.from ? ip4_text(w.from) : "any") + ";";
    return out;
}

static Ipv4Interface iface(const char *addr, const char *mask, const char *bcast, bool up = true, bool loopback = false) {
    Ipv4Interface i;
    i.addr = ip4(addr);
    i.netmask = ip4(mask);
    i.broadcast = bcast ? ip4(bcast) : 0;
    i.can_broadcast = bcast != nullptr;
    i.up = up;
    i.loopback = loopback;
    return i;
}

static home::Device at(const char *ip, int port, std::map<std::string, std::string> info = {}) {
    home::Device d;
    d.kind = "fire_tv";
    d.ip = ip;
    d.port = port;
    d.info = std::move(info);
    return d;
}

static int g_failed = 0, g_run = 0;

static void check(const std::string &name, bool ok, const std::string &detail = "") {
    g_run++;
    if (!ok) g_failed++;
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name.c_str(), ok || detail.empty() ? "" : "  -> ", ok ? "" : detail.c_str());
}

static void eq(const std::string &name, const std::string &got, const std::string &want) {
    check(name, got == want, "got \"" + got + "\", want \"" + want + "\"");
}

static std::string hex(const std::string &b) {
    static const char d[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : b) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

static std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static const char *reply_name(PinReply r) {
    switch (r) {
    case PinReply::Token: return "token";
    case PinReply::NotYet: return "not-yet";
    case PinReply::Refused: return "refused";
    case PinReply::Unreadable: return "unreadable";
    }
    return "?";
}

static void pin_case(const std::string &name, long status, const std::string &body, PinReply want, const std::string &want_token = "") {
    std::string token = "stale";
    const PinReply got = read_pin_reply(status, body, token);
    check("pin/verify reply: " + name, got == want && token == want_token,
          std::string("got ") + reply_name(got) + " \"" + token + "\", want " + reply_name(want) + " \"" + want_token + "\"");
}

static std::string kinds(const std::vector<Action> &acts) {
    std::string out;
    for (const Action &a : acts) {
        if (!out.empty()) out += ",";
        switch (a.kind) {
        case Action::Wake: out += "wake"; break;
        case Action::Key: out += "key:" + a.what + (a.repeat > 1 ? "x" + std::to_string(a.repeat) : ""); break;
        case Action::Media: out += "media:" + a.what + (a.body.empty() ? "" : "+body"); break;
        case Action::Launch: out += "launch:" + a.what; break;
        }
    }
    return out;
}

// Plans `c` on a thing with the given ref; returns the actions as text, or "ERR(<fault>)".
static std::string planned(const home::Change &c, const json &ref = json::object()) {
    home::Thing t;
    t.id = "tv";
    t.ref = ref;
    std::vector<Action> acts{Action()};
    std::string err;
    if (!plan(t, c, acts, err)) return std::string("ERR(") + fault_name(fault_of(err)) + (acts.empty() ? "" : ",not-cleared") + ")";
    return kinds(acts);
}

static home::Device offline_device() {
    home::Device d;
    d.kind = "fire_tv";
    d.ip = "127.0.0.1";
    d.port = 1;                  // nothing listens there
    d.info["dial_port"] = "1";   // nor there
    return d;
}

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";

    // ---- error sentences
    eq("fault name: no answer", fault_name(fault_of(std::string(kNoAnswer) + " It may be off.")), "no_answer");
    eq("fault name: already off", fault_name(fault_of(std::string(kAlreadyOff) + " It did not answer.")), "already_off");
    eq("fault name: not serving", fault_name(fault_of(kNotServing)), "not_serving");
    eq("fault name: waking", fault_name(fault_of(std::string(kWaking) + " Try again in a moment.")), "waking");
    eq("fault name: starting", fault_name(fault_of(std::string(kStarting) + " Try again in a moment.")), "starting");
    eq("fault name: not paired", fault_name(fault_of(kNotPaired)), "not_paired");
    eq("fault name: pair again", fault_name(fault_of(kPairAgain)), "pair_again");
    eq("fault name: key changed", fault_name(fault_of(kKeyChanged)), "key_changed");
    eq("fault name: unsupported", fault_name(fault_of(std::string(kUnsupported) + " volume.")), "unsupported");
    eq("fault name: anything else", fault_name(fault_of("The TV refused that command.")), "other");
    eq("fault name: no error", fault_name(fault_of("")), "none");
    // Only "nothing answered" may read as unreachable to the app (it then searches the network
    // and may apply the change again at a new address): never a TV that was there.
    for (const char *s : {kNotServing, kWaking, kStarting, kNotPaired, kPairAgain, kKeyChanged, kUnsupported, kAlreadyOff})
        check(std::string("app word list: \"") + s + "\" does not read as unreachable", !reads_as_unreachable(s));
    check("app word list: \"" + std::string(kNoAnswer) + "\" does (a new address may help)", reads_as_unreachable(kNoAnswer));

    // ---- a TV that is starting up (ZB-060): what a wake comes to, and the memory of one
    {
        using std::chrono::milliseconds;
        using std::chrono::seconds;
        // on=true after the TV accepted the DIAL wake: power alone is done whatever the API does,
        // unless its key changed and could not be accepted; anything more needs the API.
        check("on alone, the TV took the wake, API silent: done", power_on_done(Woken::NotServing, true));
        check("on alone, the TV took the wake, API still refusing: done", power_on_done(Woken::Waking, true));
        check("on alone, the TV took the wake, no time left to wait for the API: done", power_on_done(Woken::NoTime, true));
        check("on alone, API up: done", power_on_done(Woken::Up, true));
        check("on alone, the API's key changed and could not be accepted: not done", !power_on_done(Woken::KeyChanged, true));
        check("on with more, API up: the rest can go", power_on_done(Woken::Up, false));
        for (const Woken w : {Woken::NotServing, Woken::Waking, Woken::KeyChanged, Woken::NoTime, Woken::NoAnswer})
            check("on with more, API not up (" + std::to_string(static_cast<int>(w)) + "): not done", !power_on_done(w, false));

        // A DIAL request with no HTTP answer that counts as a wake taken late (on=true's and a
        // command's wake alike, ZB-070): only one that connected, went out and was still waiting
        // when its time ran out. What libcurl reports for each, measured in the review of 0.4.4
        // with this PC's libcurl and the device's: refused 7, closed 52, reset 56, not HTTP 1.
        check("DIAL unanswered, connected, sent, timed out: a wake taken late",
              wake_taken_late(true, true, CURLE_OPERATION_TIMEDOUT));
        check("DIAL refused: not taken", !wake_taken_late(false, false, CURLE_COULDNT_CONNECT));
        check("DIAL connection never made before the time ran out: not taken",
              !wake_taken_late(false, false, CURLE_OPERATION_TIMEDOUT));
        check("DIAL connected, the request not sent when the time ran out: not taken",
              !wake_taken_late(true, false, CURLE_OPERATION_TIMEDOUT));
        check("DIAL closed without an answer: not taken", !wake_taken_late(true, true, CURLE_GOT_NOTHING));
        check("DIAL reset: not taken", !wake_taken_late(true, true, CURLE_RECV_ERROR));
        check("DIAL answered in something that is not HTTP: not taken", !wake_taken_late(true, true, CURLE_UNSUPPORTED_PROTOCOL));

        // A silent API reads as "still starting up" only while the TV is starting.
        eq("silent API, not starting: not serving, as before", woken_error(Woken::NotServing, false), kNotServing);
        const std::string s = woken_error(Woken::NotServing, true);
        eq("silent API while starting: still starting up", s, "The TV is still starting up. Try again in a moment.");
        check("... fault starting, and nothing that sends the app looking elsewhere",
              fault_of(s) == Fault::Starting && !reads_as_unreachable(s), s);
        eq("refused while starting: waking, as before", woken_error(Woken::Waking, true), woken_error(Woken::Waking, false));
        check("refused while starting: fault waking", fault_of(woken_error(Woken::Waking, true)) == Fault::Waking);
        eq("nothing on either port while starting: no answer, as before (a new address may help)",
           woken_error(Woken::NoAnswer, true), woken_error(Woken::NoAnswer, false));
        check("... fault no_answer", fault_of(woken_error(Woken::NoAnswer, true)) == Fault::NoAnswer);
        eq("a key that changed while starting: as before", woken_error(Woken::KeyChanged, true), woken_error(Woken::KeyChanged, false));
        check("... fault key_changed", fault_of(woken_error(Woken::KeyChanged, true)) == Fault::KeyChanged);
        eq("no time while starting: as before", woken_error(Woken::NoTime, true), "There was no time left to finish that. Try again in a moment.");
        eq("up: no error", woken_error(Woken::Up, true), "");

        // A request that went out and got no answer.
        eq("unanswered command, not starting: as before", unconfirmed_error(true, false),
           std::string(kNotServing) + " The command may still have been carried out.");
        eq("unanswered read, not starting: as before", unconfirmed_error(false, false), kNotServing);
        const std::string acted = unconfirmed_error(true, true), read = unconfirmed_error(false, true);
        eq("unanswered command while starting: may have been carried out, check first", acted,
           "The TV is still starting up. The command may still have been carried out, so check the TV before trying again.");
        eq("unanswered read while starting: try again", read, "The TV is still starting up. Try again in a moment.");
        check("... both fault starting, neither reads as unreachable",
              fault_of(acted) == Fault::Starting && fault_of(read) == Fault::Starting && !reads_as_unreachable(acted) &&
                  !reads_as_unreachable(read));

        // The memory: per device, the earliest start kept, forgotten after its time or when told.
        Recall r(60000);
        const auto t0 = Recall::Clock::now();
        check("memory: nothing noted, nothing kept", !r.get("tv1", t0));
        r.note("tv1", t0, t0);
        check("memory: kept within the minute", r.get("tv1", t0 + seconds(59)) == t0);
        check("memory: per device", !r.get("tv2", t0 + seconds(1)));
        check("memory: a later wake does not move the start (a stuck API is not 'starting' for ever)",
              r.note("tv1", t0 + seconds(30), t0 + seconds(30)) == t0 && r.get("tv1", t0 + seconds(31)) == t0);
        check("memory: gone after the minute", !r.get("tv1", t0 + seconds(60) + milliseconds(1)));
        check("memory: a new wake after that starts a new minute",
              r.note("tv1", t0 + seconds(61), t0 + seconds(61)) == t0 + seconds(61) &&
                  r.get("tv1", t0 + seconds(62)) == t0 + seconds(61));
        r.note("tv2", t0 + seconds(61), t0 + seconds(61));
        r.forget("tv1");
        check("memory: forgotten when told (its API answered), the other device's kept",
              !r.get("tv1", t0 + seconds(62)) && r.get("tv2", t0 + seconds(62)) == t0 + seconds(61));
    }

    // ---- untrusted text
    eq("UTF-8 kept as it is", sanitize_utf8("Mock TV \xe2\x98\x95"), "Mock TV \xe2\x98\x95");
    eq("a stray byte becomes U+FFFD", hex(sanitize_utf8("a\xff" "b")), hex("a\xef\xbf\xbd" "b"));
    eq("an overlong encoding is refused", hex(sanitize_utf8("\xc0\xaf")), hex("\xef\xbf\xbd\xef\xbf\xbd"));
    eq("a UTF-16 surrogate is refused", hex(sanitize_utf8("\xed\xa0\x80")), hex("\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"));
    eq("a sequence cut off at the end is refused", hex(sanitize_utf8("x\xe2\x98")), hex("x\xef\xbf\xbd\xef\xbf\xbd"));
    eq("names: trimmed, controls removed, words kept apart", clean_name("  Living\tRoom\n\x01TV \x7f "), "Living Room TV");
    eq("names: bidirectional override and zero-width space removed",
       clean_name("Net\xe2\x80\xae" "flix\xe2\x80\x8b"), "Netflix");
    eq("names: C1 control removed", clean_name("A\xc2\x86" "B"), "AB");
    eq("names: NEL (white space) keeps words apart", clean_name("A\xc2\x85" "B"), "A B");
    eq("names: Arabic letter mark (a bidi control) removed", hex(clean_name("Net\xd8\x9c" "flix")), hex("Netflix"));
    eq("names: soft hyphen, combining grapheme joiner and Mongolian vowel separator removed",
       clean_name("Ne\xc2\xad" "t\xcd\x8f" "fl\xe1\xa0\x8e" "ix"), "Netflix");
    eq("names: tag characters (hidden text) removed", hex(clean_name("Net\xf3\xa0\x81\x81" "flix")), hex("Netflix"));
    eq("names: interlinear annotation controls removed", hex(clean_name("Net\xef\xbf\xb9" "fl\xef\xbf\xbb" "ix")), hex("Netflix"));
    eq("names: a variation selector removed", hex(clean_name("TV\xef\xb8\x8f")), hex("TV"));
    eq("names: a name of Hangul fillers only is empty", clean_name("\xe1\x85\x9f\xe1\x85\xa0\xe3\x85\xa4\xef\xbe\xa0"), "");
    eq("names: no-break spaces trimmed and collapsed", hex(clean_name("\xc2\xa0\xc2\xa0Living\xc2\xa0\xc2\xa0Room\xc2\xa0")),
       hex("Living Room"));
    eq("names: ideographic and typographic spaces are gaps", clean_name("\xe3\x80\x80TV\xe2\x80\x83" "1\xe3\x80\x80"), "TV 1");
    eq("names: a line separator keeps words apart", clean_name("Living\xe2\x80\xa8Room"), "Living Room");
    eq("names: capped at 40 bytes", clean_name(std::string(60, 'x')), std::string(40, 'x'));
    eq("names: the cap does not cut a UTF-8 sequence", clean_name(std::string(39, 'x') + "\xc3\xa9"), std::string(39, 'x'));
    eq("names: invalid UTF-8 replaced, not dropped", clean_name("TV\xff"), "TV\xef\xbf\xbd");
    check("nesting: 32 levels pass", !too_deep(std::string(32, '[') + std::string(32, ']')));
    check("nesting: 33 levels are refused", too_deep(std::string(33, '[') + std::string(33, ']')));
    check("nesting: brackets inside strings do not count", !too_deep("[\"" + std::string(100, '[') + "\\\"[[\"]"));

    // ---- wake addresses and the magic packet
    eq("MAC: upper case normalised", normalise_mac("00:00:5E:00:53:01"), "00:00:5e:00:53:01");
    eq("MAC: dashes accepted", normalise_mac("00-00-5e-00-53-01"), "00:00:5e:00:53:01");
    eq("MAC: mixed separators refused", normalise_mac("00:00-5e:00:53:01"), "");
    eq("MAC: a multicast address refused", normalise_mac("01:00:5e:00:00:01"), "");
    eq("MAC: the broadcast address refused", normalise_mac("ff:ff:ff:ff:ff:ff"), "");
    eq("MAC: all zeros refused", normalise_mac("00:00:00:00:00:00"), "");
    eq("MAC: not hex refused", normalise_mac("zz:00:5e:00:53:01"), "");
    eq("MAC: too short refused", normalise_mac("00:00:5e:00:53"), "");
    eq("WAKEUP header (firehose section 5)", mac_from_wakeup("MAC=00:00:5e:00:53:01;Timeout=20"), "00:00:5e:00:53:01");
    eq("WAKEUP header: other case and spacing", mac_from_wakeup("mac=00:00:5E:00:53:02 ; Timeout=35"), "00:00:5e:00:53:02");
    eq("WAKEUP header: a doubled MAC takes the first", mac_from_wakeup("MAC=00:00:5e:00:53:03;MAC=00:00:5e:00:53:04"),
       "00:00:5e:00:53:03");
    eq("WAKEUP header: junk gives nothing", mac_from_wakeup("MAC=zzz;Timeout=20"), "");
    eq("WAKEUP header: no MAC gives nothing", mac_from_wakeup("Timeout=20"), "");
    eq("announced address: WAKEUP preferred over the TXT key",
       announced_wake_mac({{"wakeup", "MAC=00:00:5e:00:53:01;Timeout=20"}, {"c", "00:00:5e:00:53:04"}}), "00:00:5e:00:53:01");
    eq("announced address: the TXT key alone", announced_wake_mac({{"c", "00:00:5e:00:53:04"}}), "00:00:5e:00:53:04");
    eq("announced address: a bad WAKEUP falls back to the TXT key",
       announced_wake_mac({{"wakeup", "Timeout=20"}, {"c", "00:00:5e:00:53:04"}}), "00:00:5e:00:53:04");
    eq("announced address: none", announced_wake_mac({{"n", "Living Room"}}), "");
    {
        std::string want = "ffffffffffff";
        for (int i = 0; i < 16; i++) want += "00005e005301";
        const std::string p = magic_packet("00:00:5E:00:53:01");
        check("magic packet: 102 bytes", p.size() == 102, std::to_string(p.size()));
        eq("magic packet: 6 x FF then the address 16 times (spec)", hex(p), want);
        eq("magic packet: none for a bad address", magic_packet("nonsense"), "");
    }
    check("Wake-on-LAN goes to UDP port 9 unless a test sets another (firehose WakeOnLan.PORT)", wake_port() == 9,
          std::to_string(wake_port()));
    {
        // Documentation addresses (RFC 5737). Nothing is sent: this only plans the packets.
        const std::vector<Ipv4Interface> ifs = {
            iface("127.0.0.1", "255.0.0.0", nullptr, true, true),
            iface("192.0.2.5", "255.255.255.0", "192.0.2.255"),
            iface("198.51.100.7", "255.255.255.0", "198.51.100.255"),
            iface("203.0.113.9", "255.255.255.0", "203.0.113.255", false),   // down
        };
        eq("wake packets: the TV's subnet broadcast and 255.255.255.255, both from that interface (firehose)",
           sends(wake_destinations(ifs, ip4("192.0.2.40"))), "192.0.2.255<-192.0.2.5;255.255.255.255<-192.0.2.5;");
        eq("wake packets: the interface is chosen by its netmask",
           sends(wake_destinations(ifs, ip4("198.51.100.200"))), "198.51.100.255<-198.51.100.7;255.255.255.255<-198.51.100.7;");
        eq("wake packets: never a unicast to the TV (its ARP entry is gone in deep standby)",
           std::to_string(sends(wake_destinations(ifs, ip4("192.0.2.40"))).find("192.0.2.40")), std::to_string(std::string::npos));
        eq("wake packets: none through an interface that is down", sends(wake_destinations(ifs, ip4("203.0.113.50"))), "");
        eq("wake packets: none when no interface shares the TV's subnet", sends(wake_destinations(ifs, ip4("10.1.2.3"))), "");
        eq("wake packets: none through an interface that cannot broadcast",
           sends(wake_destinations({iface("192.0.2.5", "255.255.255.0", nullptr)}, ip4("192.0.2.40"))), "");
        eq("wake packets: a TV on a loopback address (a test) gets one packet to its own address",
           sends(wake_destinations(ifs, ip4("127.0.0.1"))), "127.0.0.1<-any;");
    }

    // ---- the key pin
    eq("key pin of the RSA test certificate (openssl)", spki_pin_from_pem(slurp(dir + "/test_cert_a.pem")),
       "sha256//Yl4ObLO+rHpqTkvNi9gikpx97WZiO3FYB4dI1dwOcTQ=");
    eq("key pin of the P-256 test certificate (openssl)", spki_pin_from_pem(slurp(dir + "/test_cert_b.pem")),
       "sha256//I56XVFF1iO0r9UdST+fiYqhH33DUkLIo0m2fJPgMMaU=");
    eq("key pin: nothing from text that is not a certificate", spki_pin_from_pem("-----BEGIN CERTIFICATE-----\nAAAA\n"), "");
    eq("key pin: nothing from nothing", spki_pin_from_pem(""), "");

    // ---- pairing replies
    check("token shape: the protocol write-up's example (firehose)", looks_like_token("zBMBFhY"));
    check("token shape: the forum thread's example (forum)", looks_like_token("Wj9PP8I"));
    check("token shape: OKAY is a token, only OK is the sentinel (firehose)", looks_like_token("OKAY"));
    check("token shape: empty is not", !looks_like_token(""));
    check("token shape: a sentence is not (firehose)", !looks_like_token("Invalid PIN"));
    check("token shape: 513 characters are not", !looks_like_token(std::string(513, 'a')));
    check("token shape: 512 characters are", looks_like_token(std::string(512, 'a')));
    check("token shape: a control character is not", !looks_like_token("ab\x7f"));
    check("token shape: non-ASCII is not (it would go into a header)", !looks_like_token("tok\xc3\xa9"));
    pin_case("a token", 200, R"({"description":"zBMBFhY"})", PinReply::Token, "zBMBFhY");
    pin_case("OK means not yet", 200, R"({"description":"OK"})", PinReply::NotYet);
    pin_case("OKAY is a token", 200, R"({"description":"OKAY"})", PinReply::Token, "OKAY");
    pin_case("a sentence is a refusal, never stored", 200, R"({"description":"Invalid PIN"})", PinReply::Refused);
    pin_case("an empty description is a refusal", 200, R"({"description":""})", PinReply::Refused);
    pin_case("HTTP 500 is a refusal", 500, "", PinReply::Refused);
    pin_case("a token-shaped body with HTTP 403 is a refusal", 403, R"({"description":"zBMBFhY"})", PinReply::Refused);
    pin_case("no description", 200, R"({"somethingElse":1})", PinReply::Unreadable);
    pin_case("a description that is not a string", 200, R"({"description":42})", PinReply::Unreadable);
    pin_case("a string that never ends", 200, R"({"description":"never closed)", PinReply::Unreadable);
    pin_case("two descriptions are not guessed between", 200, R"({"description":"a1","description":"b2"})", PinReply::Unreadable);
    pin_case("a value spelled like the field is not the field", 200, R"({"x":"description","description":"zBMBFhY"})",
             PinReply::Token, "zBMBFhY");
    pin_case("a nested description does not count", 200, R"({"x":{"description":"zBMBFhY"}})", PinReply::Unreadable);
    // firehose FireTvClientTest.tokenRoundTripsThroughEscape: A is "A" and \\ one backslash.
    pin_case("escapes are decoded (firehose)", 200, R"({"description":"zBMA\\FhY"})", PinReply::Token, "zBMA\\FhY");
    pin_case("a short escape is not decoded into a token", 200, R"({"description":"\u004"})", PinReply::Unreadable);
    pin_case("a signed escape is not decoded into a token", 200, R"({"description":"\u+041"})", PinReply::Unreadable);
    pin_case("an oversized token is refused", 200, "{\"description\":\"" + std::string(600, 'a') + "\"}", PinReply::Refused);
    pin_case("deep nesting is unreadable", 200, std::string(5000, '[') + std::string(5000, ']'), PinReply::Unreadable);
    pin_case("an array is unreadable", 200, "[]", PinReply::Unreadable);
    pin_case("invalid UTF-8 is unreadable", 200, "{\"description\":\"tok\xff\"}", PinReply::Unreadable);

    // ---- capabilities
    {
        Capabilities c;
        c.volume = c.power = true;
        // The object measured on a Fire TV Stick (firehose section 2, 2026-09-18).
        check("capabilities: the stick's object parses",
              parse_capabilities(R"({"isEpgSupported":true,"isInternalIntentSupported":true,"isPropertiesApiSupported":true,)"
                                 R"("isVolumeControlsSupported":false,"powerCapabilityLevelOfSupport":"NotSupported",)"
                                 R"("ringableRemoteCount":0,"volumeCapabilityLevelOfSupport":"NotSupported"})", c));
        check("capabilities: the stick has neither volume nor power (firehose)", !c.volume && !c.power);
        check("capabilities: the one-field object of the device whose volume worked (firehose)",
              parse_capabilities(R"({"isVolumeControlsSupported":true})", c) && c.volume && !c.power);
        check("capabilities: the level field speaks when the flag is absent",
              parse_capabilities(R"({"volumeCapabilityLevelOfSupport":"Supported"})", c) && c.volume);
        check("capabilities: the flag wins over the level field",
              parse_capabilities(R"({"isVolumeControlsSupported":false,"volumeCapabilityLevelOfSupport":"Supported"})", c) && !c.volume);
        check("capabilities: power from its level field",
              parse_capabilities(R"({"powerCapabilityLevelOfSupport":"FullySupported"})", c) && c.power && !c.volume);
        check("capabilities: wrong types say nothing",
              parse_capabilities(R"({"isVolumeControlsSupported":"yes","powerCapabilityLevelOfSupport":5})", c) && !c.volume && !c.power);
        check("capabilities: an empty object says nothing", parse_capabilities("{}", c) && !c.volume && !c.power);
        check("capabilities: an array is refused", !parse_capabilities("[]", c));
        check("capabilities: text that is not JSON is refused", !parse_capabilities("hello", c));
        check("capabilities: deep nesting is refused", !parse_capabilities(std::string(100000, '['), c));
    }

    // ---- apps
    check("package: com.netflix.ninja", valid_package("com.netflix.ninja"));
    check("package: air.ITVMobilePlayer", valid_package("air.ITVMobilePlayer"));
    check("package: uk.co.bbc.iplayer", valid_package("uk.co.bbc.iplayer"));
    check("package: a path is refused", !valid_package("com.x/../y"));
    check("package: a query is refused", !valid_package("com.x?y=1"));
    check("package: a space is refused", !valid_package("com.x y"));
    check("package: empty segment refused", !valid_package("com..x"));
    check("package: leading dot refused", !valid_package(".com.x"));
    check("package: trailing dot refused", !valid_package("com.x."));
    check("package: no dot refused", !valid_package("netflix"));
    check("package: over 128 refused", !valid_package("com." + std::string(125, 'a')));
    {
        std::vector<App> apps;
        const std::string body =
            R"([{"appId":"com.netflix.ninja","name":"Netflix","tvIconArt":"http://x/y.png","isInstalled":true},)"
            R"({"appId":"com.amazon.firetv.youtube","name":"YouTube","isInstalled":true},)"
            R"({"appId":"com.example.gone","name":"Removed","isInstalled":false},)"
            R"({"appId":"com.example.tile","name":"Tile","isShortcutApp":true},)"
            R"({"name":"No id"},)"
            R"({"appId":"../../etc","name":"Path"},)"
            R"(5, "text", null, [],)"
            R"({"appId":"com.netflix.ninja","name":"Netflix again"},)"
            R"({"appId":"com.example.ctl","name":"Bad\u0007Name\n"},)"
            R"({"appId":"com.example.noname","name":""},)"
            R"({"appId":"com.example.typed","name":42,"isInstalled":"yes"},)"
            R"({"appId":"com.example.plain","name":"Plain"}])";
        const bool ok = parse_apps(body, apps);
        std::string got;
        for (const App &a : apps) got += a.name + "=" + a.id + ";";
        eq("apps: only installed packages, cleaned and de-duplicated", ok ? got : "parse failed",
           "Netflix=com.netflix.ninja;YouTube=com.amazon.firetv.youtube;BadName=com.example.ctl;"
           "com.example.noname=com.example.noname;Plain=com.example.plain;");
        check("apps: a wrapper object is accepted (hms-firetv)",
              parse_apps(R"({"apps":[{"appId":"com.netflix.ninja","name":"Netflix"}]})", apps) && apps.size() == 1);
        check("apps: a name that is not UTF-8 is repaired, not lost",
              parse_apps("[{\"appId\":\"com.a.b\",\"name\":\"Bad\xff\"}]", apps) && apps.size() == 1 &&
                  apps[0].name == "Bad\xef\xbf\xbd");
        check("apps: a list of the wrong type is refused", !parse_apps(R"({"apps":5})", apps));
        check("apps: text that is not JSON is refused", !parse_apps("<html>", apps));
        check("apps: deep nesting is refused", !parse_apps(std::string(200000, '[') + std::string(200000, ']'), apps));
        std::string many = "[";
        for (int i = 0; i < 300; i++) many += std::string(i ? "," : "") + "{\"appId\":\"com.a.p" + std::to_string(i) + "\",\"name\":\"P\"}";
        check("apps: at most 200 are kept", parse_apps(many + "]", apps) && apps.size() == 200, std::to_string(apps.size()));
    }
    {
        const std::vector<App> apps = {{"Netflix", "com.netflix.ninja"},  {"YouTube", "com.amazon.firetv.youtube"},
                                       {"Prime Video", "com.amazon.cloud9"}, {"Amazon Prime Music", "com.amazon.bueller.music"},
                                       {"BBC iPlayer", "uk.co.bbc.iplayer"}, {"Disney+", "com.disney.disneyplus"}};
        std::vector<std::string> cand;
        check("app match: case and spaces ignored", match_app(apps, "you tube", cand) == 1);
        check("app match: exact name", match_app(apps, "NETFLIX", cand) == 0);
        check("app match: the only name containing it", match_app(apps, "iplayer", cand) == 4);
        check("app match: punctuation ignored", match_app(apps, "disney", cand) == 5);
        check("app match: a package name as is", match_app(apps, "com.amazon.cloud9", cand) == 2);
        const int ambiguous = match_app(apps, "prime", cand);
        check("app match: two candidates are reported, not guessed between",
              ambiguous == -1 && cand.size() == 2 && cand[0] == "Prime Video" && cand[1] == "Amazon Prime Music");
        check("app match: nothing like it", match_app(apps, "hulu", cand) == -1 && cand.empty());
        check("app match: empty", match_app(apps, "  ", cand) == -1);
    }

    // ---- plan: what a change becomes, before anything is sent
    {
        home::Change c;
        const auto with_key = [](const char *k) { home::Change x; x.key = k; return x; };
        eq("plan: home", planned(with_key("home")), "key:home");
        eq("plan: up is dpad_up, sent body-less", planned(with_key("up")), "key:dpad_up");
        eq("plan: down", planned(with_key("down")), "key:dpad_down");
        eq("plan: left", planned(with_key("left")), "key:dpad_left");
        eq("plan: right", planned(with_key("right")), "key:dpad_right");
        eq("plan: ok is select", planned(with_key("ok")), "key:select");
        eq("plan: back", planned(with_key("back")), "key:back");
        eq("plan: menu", planned(with_key("menu")), "key:menu");
        eq("plan: volume_up without reported support", planned(with_key("volume_up")), "ERR(unsupported)");
        eq("plan: mute without reported support", planned(with_key("mute")), "ERR(unsupported)");
        eq("plan: power without reported support", planned(with_key("power")), "ERR(unsupported)");
        eq("plan: input is not offered", planned(with_key("input")), "ERR(unsupported)");
        eq("plan: channel keys are not offered", planned(with_key("channel_up")), "ERR(unsupported)");
        eq("plan: digits are not offered", planned(with_key("7")), "ERR(unsupported)");
        eq("plan: an unknown key", planned(with_key("launch_missiles")), "ERR(other)");
        eq("plan: play_pause (extension)", planned(with_key("play_pause")), "media:play");
        eq("plan: rewind (extension)", planned(with_key("rewind")), "media:scan+body");
        eq("plan: fast_forward (extension)", planned(with_key("fast_forward")), "media:scan+body");
        const json volume = {{"volume", true}, {"caps_read", true}}, power = {{"power", true}};
        eq("plan: volume_up with support", planned(with_key("volume_up"), volume), "key:volume_up");
        eq("plan: volume_down with support", planned(with_key("volume_down"), volume), "key:volume_down");
        eq("plan: mute key with support", planned(with_key("mute"), volume), "key:mute");
        eq("plan: power with support", planned(with_key("power"), power), "key:power");
        eq("plan: volume support does not bring power", planned(with_key("power"), volume), "ERR(unsupported)");
        c = home::Change();
        c.on = true;
        eq("plan: on is a wake", planned(c), "wake");
        c.on = false;
        eq("plan: off is sleep", planned(c), "key:sleep");
        c.key = "home";
        eq("plan: sleep comes last", planned(c), "key:home,key:sleep");
        c = home::Change();
        c.on = true;
        c.source = "Netflix";
        eq("plan: the wake comes first", planned(c), "wake,launch:Netflix");
        c = home::Change();
        c.transport = "pause";
        eq("plan: pause is the play/pause toggle", planned(c), "media:play");
        c.transport = "play";
        eq("plan: play is the same toggle", planned(c), "media:play");
        c.transport = "stop";
        eq("plan: stop is not offered", planned(c), "ERR(unsupported)");
        c.transport = "next";
        eq("plan: next is not offered", planned(c), "ERR(unsupported)");
        c.transport = "dance";
        eq("plan: an unknown transport", planned(c), "ERR(other)");
        c = home::Change();
        c.volume_step = 3;
        eq("plan: volume steps without support", planned(c), "ERR(unsupported)");
        eq("plan: three steps up", planned(c, volume), "key:volume_upx3");
        c.volume_step = -2;
        eq("plan: two steps down", planned(c, volume), "key:volume_downx2");
        c.volume_step = 11;
        eq("plan: more than 10 steps refused", planned(c, volume), "ERR(other)");
        c = home::Change();
        c.mute = true;
        eq("plan: a chosen mute state is refused even with volume support (only a toggle)", planned(c, volume),
           "ERR(unsupported)");
        c.mute = false;
        eq("plan: so is unmute (the toggle would mute an unmuted TV)", planned(c, volume), "ERR(unsupported)");
        eq("plan: and mute without volume support", planned(c), "ERR(unsupported)");
        c = home::Change();
        c.volume = 30;
        eq("plan: an absolute volume is not offered", planned(c, volume), "ERR(unsupported)");
        c = home::Change();
        c.brightness = 50;
        eq("plan: brightness is not a TV's", planned(c), "ERR(unsupported)");
        c = home::Change();
        c.source = "  ";
        eq("plan: an empty app name", planned(c), "ERR(other)");
        eq("plan: an empty change", planned(home::Change()), "ERR(other)");
        c = home::Change();
        c.key = "home";
        c.volume_step = 2;
        eq("plan: one unsupported part stops the whole change", planned(c), "ERR(unsupported)");

        // Inputs (review round 2): the smart_home tool offers "HDMI 1" as a source; this API has
        // no recorded input switch, so the change is unsupported and nothing is sent.
        for (const char *in : {"HDMI 1", "hdmi2", "HDMI-3", "AV", "Component", "Input 2", "TV", "Live TV", "Antenna"})
            check(std::string("input name: \"") + in + "\" reads as an input", reads_as_input(in));
        for (const char *app : {"Netflix", "HDMI Switcher", "TV Player", "Pluto TV", "AV Club", "hdmi123", "", "  "})
            check(std::string("input name: \"") + app + "\" does not", !reads_as_input(app));
        c = home::Change();
        c.source = "HDMI 1";
        eq("plan: an input as the source is unsupported, before anything is sent", planned(c), "ERR(unsupported)");
        {
            home::Thing t;
            t.id = "tv";
            std::vector<Action> acts;
            std::string err;
            plan(t, c, acts, err);
            eq("plan: ... and says so", err, "This TV does not offer that over the network: switching inputs.");
        }
        eq("plan: unless an installed app is called exactly that",
           planned(c, {{"apps", json::array({json::array({"HDMI 1", "com.example.hdmi"})})}}), "launch:HDMI 1");
        c.source = "TV Player";
        eq("plan: an app name that only contains an input word is an app", planned(c), "launch:TV Player");
        c = home::Change();
        c.on = true;
        c.source = "hdmi 2";
        eq("plan: on=true with an input: unsupported as a whole, so the TV is not woken first", planned(c), "ERR(unsupported)");

        // Messages that quote what was asked for (review round 2): the quote can bring in a word
        // control.cpp's unreachable() list matches; the fixed words never do, and fault_of() puts
        // them all in Other, whatever the name.
        for (const char *name : {"connect", "Reach", "respond", "timeout", "zzz"}) {
            home::Thing t;
            t.id = "tv";
            std::vector<Action> acts;
            std::string err;
            home::Change k;
            k.key = name;
            plan(t, k, acts, err);
            check(std::string("app word list: \"There is no TV key called \"") + name + "\".\" is fault other",
                  fault_of(err) == Fault::Other, err);
            if (std::string(name) == "zzz") check("app word list: its fixed words do not read as unreachable", !reads_as_unreachable(err), err);
            if (std::string(name) == "Reach") check("app word list: the quote can (so the app must use fault_of())", reads_as_unreachable(err), err);
            k = home::Change();
            k.transport = name;
            plan(t, k, acts, err);
            check(std::string("app word list: \"There is no playback control called \"") + name + "\".\" is fault other",
                  fault_of(err) == Fault::Other, err);
        }
    }

    // ---- the API port (nothing is contacted: this only computes it)
    {
        const auto port = [](const home::Device &d) { return std::to_string(api_port_of(d)); };
        eq("API port: Device::port on a loopback address (a test pointing at a mock)", port(at("127.0.0.1", 41000)), "41000");
        eq("API port: Device::port on IPv6 loopback", port(at("::1", 41000)), "41000");
        eq("API port: discovery's SRV (Whisperlink) port is not followed on a real network",
           port(at("192.0.2.10", 41000)), "8080");
        eq("API port: nor on IPv6", port(at("2001:db8::10", 41000)), "8080");
        eq("API port: 8080 when none is given", port(at("192.0.2.10", 0)), "8080");
        eq("API port: info api_port wins on a real network", port(at("192.0.2.10", 41000, {{"api_port", "8443"}})), "8443");
        eq("API port: and on loopback", port(at("127.0.0.1", 41000, {{"api_port", "5000"}})), "5000");
        eq("API port: an info api_port that is not a port is ignored",
           port(at("192.0.2.10", 0, {{"api_port", "70000"}})) + port(at("192.0.2.10", 0, {{"api_port", "8o8o"}})) +
               port(at("192.0.2.10", 0, {{"api_port", "0"}})),
           "808080808080");
        eq("API port: none for a host name (no DNS)", port(at("fire-tv.local", 0)), "0");
    }

    // ---- the driver, offline
    {
        auto driver = make_firetv_driver();
        eq("driver: kind", driver->kind(), "fire_tv");
        check("driver: claims what discovery calls fire_tv", driver->claims("fire_tv", {}));
        check("driver: does not claim an Android TV", !driver->claims("android_tv", {}));
        check("driver: does not claim an AirPlay or unknown device", !driver->claims("airplay", {}) && !driver->claims("other", {}));

        home::Device d = offline_device();
        home::PairStep st = driver->next_step(d);
        check("pairing step 1: asks for the code to be shown, once (wait 0)",
              st.kind == home::Pairing::AcceptPrompt && st.wait_s == 0 && st.prompt.find("4-digit code") != std::string::npos);
        check("pairing step 1: names the settings page's button for AcceptPrompt (there is no Continue)",
              st.prompt.find("\"I have accepted it\"") != std::string::npos && st.prompt.find("Continue") == std::string::npos,
              st.prompt);
        const long long now = static_cast<long long>(time(nullptr));
        d.auth["pending"] = {{"key_pin", "sha256//Yl4ObLO+rHpqTkvNi9gikpx97WZiO3FYB4dI1dwOcTQ="}, {"asked", now - 10}};
        st = driver->next_step(d);
        check("pairing step 2: the code field while the code is fresh",
              st.kind == home::Pairing::EnterCode && !st.field.empty() && st.prompt.find("5 minutes") != std::string::npos);
        d.auth["pending"]["asked"] = now - 301;
        check("pairing: after 5 minutes it starts again", driver->next_step(d).kind == home::Pairing::AcceptPrompt);
        d.auth["pending"]["asked"] = now + 3600;
        check("pairing: a time in the future is not trusted", driver->next_step(d).kind == home::Pairing::AcceptPrompt);
        // An edited saved-devices file: no stored time may overflow the age arithmetic (undefined
        // behaviour; the sanitizer build of this test stops on it).
        for (const char *t : {"-9223372036854775808", "9223372036854775807", "18446744073709551615", "0", "-5"}) {
            d.auth["pending"]["asked"] = json::parse(t);
            check(std::string("pairing: a stored time of ") + t + " is not a fresh code",
                  driver->next_step(d).kind == home::Pairing::AcceptPrompt);
        }
        d.auth["pending"]["asked"] = now;
        std::string unused_err;
        check("pairing: a hostile stored time does not break step 2 either (LLONG_MIN through pair())",
              [&] {
                  home::Device x = offline_device();
                  x.auth["pending"] = {{"key_pin", "sha256//Yl4ObLO+rHpqTkvNi9gikpx97WZiO3FYB4dI1dwOcTQ="}, {"asked", LLONG_MIN}};
                  return !driver->pair(x, "1234", unused_err) && unused_err.find("expired") != std::string::npos;
              }(),
              unused_err);
        d.auth = {{"token", "zBMBFhY"}, {"key_pin", "sha256//Yl4ObLO+rHpqTkvNi9gikpx97WZiO3FYB4dI1dwOcTQ="}};
        check("pairing: none needed once paired", driver->next_step(d).kind == home::Pairing::None);
        d.auth.erase("key_pin");
        check("pairing: a token without a key pin does not count", driver->next_step(d).kind == home::Pairing::AcceptPrompt);

        std::string err;
        d = offline_device();
        d.auth["pending"] = {{"key_pin", "sha256//Yl4ObLO+rHpqTkvNi9gikpx97WZiO3FYB4dI1dwOcTQ="}, {"asked", now}};
        auto t0 = std::chrono::steady_clock::now();
        check("pairing: a code that is not 4 digits is refused before sending", !driver->pair(d, "12a4", err) &&
              err == "Type the 4 digits the TV is showing." && d.auth.contains("pending"), err);
        check("pairing: so is a 5-digit code", !driver->pair(d, "12345", err));
        d.auth.erase("pending");
        check("pairing: a code with no code shown is refused before sending",
              !driver->pair(d, "1234", err) && err.find("expired") != std::string::npos, err);
        d.ip = "fire-tv.local";
        check("pairing: a host name is refused (no DNS)", !driver->pair(d, "", err) && err == "The TV's address is not valid.", err);
        const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        check("pairing: none of the refusals above waited on the network", ms < 200, std::to_string(ms) + " ms");

        d = offline_device();
        home::Thing tv;
        tv.id = "tv";
        home::Change c;
        c.key = "home";
        check("apply: refused when not paired", !driver->apply(d, tv, c, 2500, err) && fault_of(err) == Fault::NotPaired, err);
        std::vector<home::Thing> things;
        check("enumerate: refused when not paired", !driver->enumerate(d, things, err) && fault_of(err) == Fault::NotPaired, err);
        check("read: refused when not paired", !driver->read(d, things, err) && fault_of(err) == Fault::NotPaired, err);
        d.auth = {{"token", "zBMBFhY"}, {"key_pin", "sha256//Yl4ObLO+rHpqTkvNi9gikpx97WZiO3FYB4dI1dwOcTQ="}};
        t0 = std::chrono::steady_clock::now();
        c = home::Change();
        c.key = "input";
        check("apply: an unsupported key fails as unsupported", !driver->apply(d, tv, c, 2500, err) && fault_of(err) == Fault::Unsupported, err);
        home::Thing other;
        other.id = "lamp";
        c.key = "home";
        check("apply: a thing this TV does not have", !driver->apply(d, other, c, 2500, err) && fault_of(err) == Fault::Other, err);
        check("apply: the refusals above sent nothing", std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));
        c = home::Change();
        c.key = "home";
        t0 = std::chrono::steady_clock::now();
        const bool sent = driver->apply(d, tv, c, 800, err);
        const long took = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        check("apply: nothing listening gives no_answer within the deadline (800 ms)",
              !sent && fault_of(err) == Fault::NoAnswer && took < 1000, err + " after " + std::to_string(took) + " ms");
        c = home::Change();
        c.on = false;
        check("apply: off with nothing listening is already_off, never no_answer (an infrared toggle would switch it on)",
              !driver->apply(d, tv, c, 800, err) && fault_of(err) == Fault::AlreadyOff, err);
        c.on = true;   // no wake address known: no packet is sent
        check("apply: on with nothing listening and no wake address is no_answer (infrared may switch it on)",
              !driver->apply(d, tv, c, 800, err) && fault_of(err) == Fault::NoAnswer, err);

        // A wake address, but no packet can go out (review round 2): Wake-on-LAN here is IPv4
        // broadcast, so a TV on an IPv6 address gets none. Nothing is waking it: no_answer, not
        // "waking ... A wake-up signal was sent". Nothing listens on [::1]:1.
        {
            home::Device v6 = offline_device();
            v6.ip = "::1";
            v6.auth = {{"token", "zBMBFhY"}, {"key_pin", "sha256//Yl4ObLO+rHpqTkvNi9gikpx97WZiO3FYB4dI1dwOcTQ="},
                       {"wake_mac", "00:00:5e:00:53:01"}};
            home::Change on;
            on.on = true;
            check("apply: on for an IPv6 TV with a wake address sends no packet, so no_answer, never 'waking'",
                  !driver->apply(v6, tv, on, 800, err) && fault_of(err) == Fault::NoAnswer &&
                      err.find("off or unplugged") != std::string::npos, err);
        }

        // Too little time to wake the TV after a request that could not be delivered (review
        // round 2): nothing is concluded from a DIAL request that would get under 200 ms, so not
        // no_answer (which hands the change to infrared).
        c = home::Change();
        c.key = "home";
        check("apply: with 150 ms there is no time to try the wake, and that is not no_answer",
              !driver->apply(d, tv, c, 150, err) && fault_of(err) == Fault::Other &&
                  err == "There was no time left to finish that. Try again in a moment." && !reads_as_unreachable(err), err);
        driver->unpair(d);
        check("unpair: forgets every pairing detail", d.auth.is_object() && d.auth.empty());
    }

    printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
