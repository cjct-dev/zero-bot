// Amazon Fire TV (Fire OS televisions, sticks and cubes): the local REST API the official Fire TV
// phone app uses. HTTPS on port 8080 with a self-signed certificate, pairing by a 4-digit code the
// TV shows, wake by a DIAL request on port 8009 and, where the TV announced a wake address, a
// Wake-on-LAN packet.
//
// Amazon does not document this API. Everything here follows two MIT-licensed implementations of
// it (see firetv.cpp for what was ported from where, and STATUS.md for what is corroborated).
//
// What a caller needs to know:
//  - One thing per device: type "tv", id "tv". It takes keys (Change::key: up, down, left, right,
//    ok, back, home, menu), play/pause (Change::transport "play" and "pause" both send the TV's
//    one play/pause toggle, so a caller must not record "playing" or "paused" from them), app
//    launch by name (Change::source), sleep (on=false) and wake (on=true). The keys volume_up,
//    volume_down and mute, and Change::volume_step, exist only when the TV's own capability
//    report says it controls volume (kVolumeStep); the "power" key only when it reports power
//    control. Change::mute (a chosen mute state) is never offered: the TV has only a mute key
//    that toggles, and cannot say which way it is. Everything else (channels, input, digits,
//    absolute volume, stop, next, previous) fails with kUnsupported so the caller can use
//    infrared instead; so does Change::source naming an input ("HDMI 1", "AV", "antenna",
//    "TV": detail::reads_as_input) unless an installed app has exactly that name ("switching
//    inputs"), and then nothing at all is sent. Extra keys outside tv_remote's list:
//    "play_pause", "rewind", "fast_forward" (rewind and fast-forward start a shuttle that runs
//    until play/pause).
//  - The API cannot report whether the screen is on, the volume, or what is playing.
//    Thing::state is {"reachable": bool, "apps": [installed app names]}; there is no "on".
//    Thing::ref (private) keeps {"volume": bool, "power": bool, "caps_read": bool,
//    "apps": [[name, package], ...]}.
//  - enumerate() and read() never wake the TV: a sleeping TV is listed with "reachable": false.
//    apply() wakes an idle TV with the DIAL request when a command could not be delivered at all
//    (the connection was refused, or TLS never completed), then sends it once more. A command
//    that went out but was not answered within the deadline is never sent again, because the TV
//    may have carried it out (a toggle sent twice undoes itself): that is kNotServing. Only
//    on=true sends the Wake-on-LAN packet.
//  - on=true on a TV in deep standby takes 7 s or more after the first Wake-on-LAN packet
//    (6.5-7.4 s measured). on=true reports kNoAnswer only when a DIAL request that started 8 s
//    or more after that packet got nothing (the packet counted across on=true calls for the
//    same device within a minute); until then it reports kWaking, so that nothing toggles power
//    by infrared while the TV may be resuming. The verdict rests on a request, not on the clock:
//    each call's last DIAL request goes out 300 ms before its deadline. Give on=true about
//    9000 ms to settle it in one call; with the usual 2500 ms the first call says kWaking, and a
//    repeat says kNoAnswer only once its own requests reach past 8 s and still get nothing. A
//    wake address with no packet sent (an IPv6 TV, no interface on the TV's subnet) is no wake.
//  - on=true alone is done once the TV has accepted the DIAL wake (a 2xx answer): the TV is
//    coming on, even when its API, which starts some seconds later, is still refusing or
//    accepting connections without answering when the deadline comes (the owner's TV did that
//    for longer than 2.5 s). It still fails when the API presents a key that changed and could
//    not be accepted (kKeyChanged), and when the TV did not accept the wake (no answer, an answer
//    that is not 2xx: as above). Anything else in the change needs the API.
//  - When on=true is combined with other fields, the wake keeps 300 ms back for them. A later
//    part of a change that would start with less than 200 ms left is not sent: kWaking ("Try
//    again in a moment.") after a wake, else "There was no time left to finish that. Try again
//    in a moment." (Fault::Other). Never kNoAnswer: infrared would toggle the TV just woken.
//  - The TV is "starting" for up to a minute after it accepted a DIAL wake from this driver
//    (on=true's, or the one sent for a command while its API refused connections), until its API
//    answers anything. Then a command that finds the API silent (it accepts and does not answer)
//    fails with kStarting, not kNotServing: "The TV is still starting up. Try again in a moment.",
//    or, when the request went out, "... The command may still have been carried out, so check
//    the TV before trying again." So does an on=true then whose DIAL request is accepted and not
//    answered (an HTTP answer that refuses the wake stays kNotServing). The minute counts from the
//    first accepted wake of a run with no answer from the API, so an API that is stuck reads as
//    kNotServing again after it.
//  - on=false when nothing answers on either port reports kAlreadyOff: the TV is most likely
//    off (deep standby) already. Never answer it with an infrared power toggle. Exception: when
//    this driver sent the TV a wake-up signal less than 8 s before on=false was asked and nothing
//    has answered since, the TV may be coming on right now: kWaking ("... so it was not switched
//    off. Ask again in a few seconds.").
//  - Every error begins with one of the sentences below; fault_of() tells them apart. Fall back
//    to infrared only on Fault::NoAnswer (nothing answered: off, unplugged or moved) and
//    Fault::Unsupported (the TV does not offer the control over the network). Of the fixed
//    wording, only the no_answer and already_off messages contain a word that the app's
//    control.cpp unreachable() list matches ("answer"), which is right: only then can a
//    re-discovery at a new address help (a TV that is starting up has not moved: kStarting's
//    messages have none of those words). But a message that quotes what was asked for (an app,
//    key or playback control name: "No app called "Reach" is on the TV.") can bring such a word
//    in with the quote. Classify with fault_of(), not with words.
//  - Ports. The API port is Device::info "api_port" when present (digits), else Device::port
//    when not 0 and the TV is on a loopback address (a test pointing at a mock, as the common
//    brief asks), else 8080. A non-zero Device::port is NOT honoured on a real network, because
//    discovery fills it with the _amzn-wplay SRV (Whisperlink) port, which is not the API
//    (DISCOVERY.md, change 1). Device::info "dial_port", when present, is the DIAL port (default
//    8009). Device::info "wakeup" (the SSDP WAKEUP header) or "c" (the mDNS TXT key) give the
//    wake address; the app's discovery.cpp records neither today, so until it does (DISCOVERY.md,
//    changes 2 and 3) no magic packet is ever sent and on=true cannot wake a TV from deep
//    standby. Device::mac (the ARP-table address discovery fills in) is deliberately not used
//    as a wake address: the TV never announced it as one. Device::auth holds {"token",
//    "key_pin", "paired_at", "wake_mac"} once paired and {"pending": {...}} between the two
//    pairing steps.
//
// Depends on the C++ standard library, POSIX sockets, libcurl, OpenSSL libcrypto and
// nlohmann/json. The application should have called curl_global_init() first.
#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "smarthome/driver.h"

std::unique_ptr<home::Driver> make_firetv_driver();

namespace firetv {
using home::json;

// Why a call failed, recognised from the start of the error sentence.
enum class Fault {
    None,         // no error text
    NoAnswer,     // nothing answered on either port: off, deeply asleep, unplugged or moved. Infrared is the fallback.
    AlreadyOff,   // on=false and nothing answered on either port: most likely off already. NOT an infrared
                  // fallback: the infrared power key toggles, so it would switch the TV on.
    NotServing,   // the TV is on the network but its control service did not reply: it accepted and stayed
                  // silent (waking does not help), or a command went out and its answer did not come in time
                  // (it may have been carried out, so it is not repeated). Not an infrared fallback. (In the
                  // minute after the TV accepted a wake from this driver, that is Starting instead.)
    Waking,       // a wake is under way: acknowledged by the TV, or a Wake-on-LAN packet went out and no DIAL
                  // request made 8 s or more after it has gone unanswered yet (also on=false then, and a
                  // later part of a change left without time after a wake). Not an infrared fallback (a
                  // power toggle would race the TV coming on): try again.
    Starting,     // the TV accepted a wake from this driver less than a minute ago and its API, accepting
                  // connections, has not answered since: it is coming on. Not an infrared fallback: try again.
    NotPaired,    // no pairing stored
    PairAgain,    // the TV rejected the stored pairing
    KeyChanged,   // the TV presented a different TLS key from the one pinned at pairing; nothing was sent
    Unsupported,  // this TV, or this API, does not offer the requested control over the network (also when
                  // the TV answers a control with 404 or 405, or a key or media action with 400)
    Other,        // anything else (a refused command, a reply that was not understood, bad input)
};
Fault fault_of(const std::string &err);
const char *fault_name(Fault f);   // "no_answer", "already_off", "not_serving", ... for logs and tests

// The opening sentences. An error may continue after them with one more sentence.
extern const char *const kNoAnswer;      // "The TV did not answer."
extern const char *const kAlreadyOff;    // "The TV seems to be off already."
extern const char *const kNotServing;    // "The TV is on the network but its remote control service is not replying."
extern const char *const kWaking;        // "The TV is waking up."
extern const char *const kStarting;      // "The TV is still starting up."
extern const char *const kNotPaired;     // "The TV is not paired yet."
extern const char *const kPairAgain;     // "The TV no longer accepts this pairing."
extern const char *const kKeyChanged;    // "The TV's security key has changed."
extern const char *const kUnsupported;   // "This TV does not offer that over the network:"

// Building blocks, exposed so that firetv_test.cpp can check each one. Not part of the interface.
namespace detail {

// Where Wake-on-LAN packets go: UDP port 9 on the subnet broadcast address of the interface the
// TV is on, and 255.255.255.255 bound to that interface's address. For a TV on a loopback
// address (only ever a test) the packet goes to that address instead. Tests against a mock set
// the port here; the app never calls this.
void set_wake_port(int port);
int wake_port();   // 9 unless set_wake_port() changed it

// One IPv4 interface as getifaddrs() reports it. Addresses in host byte order.
struct Ipv4Interface {
    uint32_t addr = 0, netmask = 0, broadcast = 0;
    bool up = false, loopback = false, can_broadcast = false;
};
// One magic packet to send: to `to`, from a socket bound to `from` (0: not bound). Host order.
struct WakeSend {
    uint32_t to = 0, from = 0;
};
// Where the magic packets for a TV at `tv` go (firehose UdpWakeOnLan): for every interface that
// is up, can broadcast, is not loopback and has the TV on its subnet, that subnet's broadcast
// address and 255.255.255.255, each sent from the interface's own address. A TV on a loopback
// address (only ever a test) gets one packet to its own address. Nothing when no interface
// shares the TV's subnet.
std::vector<WakeSend> wake_destinations(const std::vector<Ipv4Interface> &interfaces, uint32_t tv);

// The HTTPS port of the API for this device (see "Ports" above): info "api_port", else
// Device::port on a loopback address, else 8080. 0 when the address is not an IP literal.
int api_port_of(const home::Device &dev);

// Replaces every byte sequence that is not UTF-8 with U+FFFD.
std::string sanitize_utf8(const std::string &raw);
// Removes characters that display as nothing (C0 and C1 controls, DEL, Unicode's
// Default_Ignorable_Code_Point set: soft hyphen, zero-width characters, bidirectional marks,
// overrides and isolates, Hangul fillers, variation selectors, tags; and the interlinear
// annotation controls), turns every Unicode White_Space character into a word gap, collapses and
// trims the gaps, and caps at `max` bytes without cutting a UTF-8 sequence. Invalid UTF-8 is
// replaced first.
std::string clean_name(const std::string &raw, size_t max = 40);
// Whether a JSON text nests arrays and objects deeper than `max` (strings are skipped). Checked
// before parsing anything the TV sends.
bool too_deep(const std::string &text, int max = 32);

// "aa:bb:cc:dd:ee:ff" (lower case) from six hex pairs separated by ':' or '-', "" for anything else.
std::string normalise_mac(const std::string &text);
// The address out of an SSDP "WAKEUP" header value such as "MAC=00:00:5e:00:53:01;Timeout=20".
std::string mac_from_wakeup(const std::string &value);
// The wake address a device announced, from what discovery kept in Device::info:
// "wakeup" (the SSDP header value) is preferred over "c" (the mDNS TXT key). "" when neither.
std::string announced_wake_mac(const std::map<std::string, std::string> &info);
// The 102-byte magic packet (raw bytes), "" when `mac` is not an address.
std::string magic_packet(const std::string &mac);

// "sha256//<base64>" of the certificate's SubjectPublicKeyInfo, as libcurl's pinned-key option
// expects it. "" when `pem` holds no certificate.
std::string spki_pin_from_pem(const std::string &pem);

// Whether a pin/verify description is shaped like a token rather than a message: 1 to 512
// printable ASCII characters, no spaces.
bool looks_like_token(const std::string &description);

enum class PinReply { Token, NotYet, Refused, Unreadable };
// Reads a pin/verify answer. `token` is filled only for PinReply::Token. A 2xx answer whose
// top-level "description" is exactly "OK" is NotYet; a token-shaped one is Token; any other
// description, or any other status, is Refused; a 2xx body without exactly one top-level string
// "description" is Unreadable.
PinReply read_pin_reply(long http_status, const std::string &body, std::string &token);

struct Capabilities {
    bool volume = false;   // volume up/down and the mute key act on this device
    bool power = false;    // the "power" action is reported as supported
};
// From the body of GET /v1/FireTV. Only a positive statement counts: "isVolumeControlsSupported"
// true (or, when that is absent, a "volumeCapabilityLevelOfSupport" other than "NotSupported"),
// and a "powerCapabilityLevelOfSupport" other than "NotSupported". False when the body is not a
// JSON object.
bool parse_capabilities(const std::string &body, Capabilities &out);

struct App {
    std::string name;   // cleaned display name
    std::string id;     // package name
};
// Whether `id` is shaped like an Android package name (letters, digits, '_' and '.', at most 128).
bool valid_package(const std::string &id);
// From the body of GET /v1/FireTV/appsV2 (an array, or an object holding one under "apps",
// "appsV2" or "result"). Entries that are not installed apps, shortcuts, or that carry an
// invalid package name are left out; at most 200 are kept. False when no list was found.
bool parse_apps(const std::string &body, std::vector<App> &out);
// The app called `wanted`, ignoring case, spaces and punctuation: an exact name, else a package
// name given as is, else the only app whose name contains it. Returns its index, or -1 with
// `candidates` holding up to five names that contain what was asked for (never a guess).
int match_app(const std::vector<App> &apps, const std::string &wanted, std::vector<std::string> &candidates);
// Whether a source name reads as a TV input rather than an app, ignoring case, spaces and
// punctuation: hdmi, av, component, composite, scart, vga, dvi or input, alone or followed by
// one or two digits ("HDMI 1", "hdmi2", "AV"), or tv, live tv, antenna, aerial, tuner.
bool reads_as_input(const std::string &name);

// One request (or a run of identical ones) the driver will send for a change.
struct Action {
    enum Kind { Wake, Key, Media, Launch } kind = Key;
    std::string what;     // Key: the action= value. Media: "play" or "scan". Launch: the name asked for.
    std::string body;     // Media scan: the JSON body
    int repeat = 1;       // Key: how many presses (volume steps), 220 ms apart
};
// Turns a change into the requests to send, in order (a wake first, sleep last), without
// contacting anything. Capabilities come from thing.ref. False with an error sentence when any
// part of the change is not available on this thing; then nothing at all is sent.
bool plan(const home::Thing &thing, const home::Change &change, std::vector<Action> &out, std::string &err);

// What a wake came to: the API answered (Up); nothing answered on either port (NoAnswer); the TV
// was there but its API did not answer: it accepted connections and stayed silent, or DIAL did not
// accept the wake (NotServing); the API kept refusing after an accepted wake (Waking); the API
// presented a key that could not be accepted (KeyChanged); there was no time to try (NoTime).
enum class Woken { Up, NoAnswer, NotServing, Waking, KeyChanged, NoTime };
// The error a wake that did not bring the API up reports ("" for Up). `starting`: the TV accepted
// a wake from this driver less than a minute ago and its API has not answered since, so an API
// that is silent is starting up (kStarting), not stuck (kNotServing).
std::string woken_error(Woken w, bool starting);
// The error for a request that went out and got no answer in time. `acts`: it makes the TV do
// something, so it may have been carried out (never sent again by itself).
std::string unconfirmed_error(bool acts, bool starting);
// Whether on=true is done once the TV has accepted the DIAL wake (a 2xx answer), given what the
// wait for the API found (`api`) and whether the change asks for nothing else (`alone`). Power
// alone is done unless the API presented a key that could not be accepted; anything more needs Up.
bool power_on_done(Woken api, bool alone);

// When something began, per device (Device::id, or the address when there is none), kept for
// `keep_ms` and then forgotten: the first magic packet of a still unanswered attempt to switch
// the TV on, and the first DIAL wake the TV accepted whose API has not answered since. Safe to
// use from several threads. `now` is a parameter for the tests.
class Recall {
public:
    using Clock = std::chrono::steady_clock;
    explicit Recall(long keep_ms) : keep_(keep_ms) {}
    std::optional<Clock::time_point> get(const std::string &who, Clock::time_point now = Clock::now());
    // Keeps `at` for `who`, unless an earlier time is still kept; returns the time kept.
    Clock::time_point note(const std::string &who, Clock::time_point at, Clock::time_point now = Clock::now());
    void forget(const std::string &who);

private:
    void prune(Clock::time_point now);   // with mutex_ held
    const std::chrono::milliseconds keep_;
    std::mutex mutex_;
    std::map<std::string, Clock::time_point> at_;
};

}  // namespace detail
}  // namespace firetv
