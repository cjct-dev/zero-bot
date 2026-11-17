// Amazon Fire TV driver over the Fire TV phone app's local REST API. See firetv.h.
//
// Ported from (both MIT):
//   firehose-remote, github.com/AustrianCanvasSociety/firehose-remote (docs/protocol.md and
//   protocol/FireTvClient.kt, WakeOnLan.kt, Ssdp.kt as pushed on 2026-10-05): the API key
//   constant and headers; the PIN exchange with its "OK" retry (3 attempts, 1 s apart) and the
//   token shape check; the wake shape (DIAL on a command that got no answer, then GET
//   /v1/FireTV/status every 250 ms for at most 3 s, where any HTTP answer counts, then one
//   resend; wake attempts of 2 s, for connecting and for the answer alike, started 2.5 s apart,
//   a magic packet with each); the WAKEUP header parsing and the magic packet; the scan bodies;
//   the capability fields.
//   hms-firetv, github.com/hms-homelab/hms-firetv (src/clients/LightningClient.cpp,
//   src/services/AppSyncService.cpp): the appsV2 entry filter (appId, name, isInstalled,
//   isShortcutApp, object wrappers) and the app launch path.
// Nothing was taken from uc-intg-firetv (MPL-2.0) or from the Home Assistant forum thread; they
// were only read to compare behaviour. Where this file departs from those sources it says so.
//
// The two ported sources carry these notices (LICENSE of each repository), reproduced as their
// licence requires:
//
//   firehose-remote: MIT License, Copyright (c) 2026 AustrianCanvasSociety
//   hms-firetv:      MIT License, Copyright (c) 2026 HMS Homelab
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this
//   software and associated documentation files (the "Software"), to deal in the Software
//   without restriction, including without limitation the rights to use, copy, modify, merge,
//   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
//   to whom the Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
//   INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
//   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
//   FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
#include "firetv.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <unistd.h>

namespace firetv {

// Only kNoAnswer and kAlreadyOff messages may use the words control.cpp's unreachable() looks
// for (connect, reach, answer, respond, timed out, timeout, no route, unreachable): the app then
// searches the network for the device, which helps only when nothing answered at this address.
// (A message that quotes what the user asked for, an app, key or playback control name, can
// carry those words in the quote: fault_of() is the reliable classifier.)
const char *const kNoAnswer = "The TV did not answer.";
const char *const kAlreadyOff = "The TV seems to be off already.";
const char *const kNotServing = "The TV is on the network but its remote control service is not replying.";
const char *const kWaking = "The TV is waking up.";
const char *const kNotPaired = "The TV is not paired yet.";
const char *const kPairAgain = "The TV no longer accepts this pairing.";
const char *const kKeyChanged = "The TV's security key has changed.";
const char *const kUnsupported = "This TV does not offer that over the network:";

namespace {
bool starts_with(const std::string &s, const char *prefix) {
    const size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}
}  // namespace

Fault fault_of(const std::string &err) {
    if (err.empty()) return Fault::None;
    const struct {
        const char *const *text;
        Fault fault;
    } table[] = {{&kNoAnswer, Fault::NoAnswer},     {&kAlreadyOff, Fault::AlreadyOff}, {&kNotServing, Fault::NotServing},
                 {&kWaking, Fault::Waking},         {&kNotPaired, Fault::NotPaired},   {&kPairAgain, Fault::PairAgain},
                 {&kKeyChanged, Fault::KeyChanged}, {&kUnsupported, Fault::Unsupported}};
    for (const auto &t : table)
        if (starts_with(err, *t.text)) return t.fault;
    return Fault::Other;
}

const char *fault_name(Fault f) {
    switch (f) {
    case Fault::None: return "none";
    case Fault::NoAnswer: return "no_answer";
    case Fault::AlreadyOff: return "already_off";
    case Fault::NotServing: return "not_serving";
    case Fault::Waking: return "waking";
    case Fault::NotPaired: return "not_paired";
    case Fault::PairAgain: return "pair_again";
    case Fault::KeyChanged: return "key_changed";
    case Fault::Unsupported: return "unsupported";
    case Fault::Other: return "other";
    }
    return "other";
}

// ---------------------------------------------------------------------------------------------
// Building blocks (and the small helpers a shared net/text library would hold later: UTF-8
// cleaning, the nesting check, the magic packet, the key pin). Listed in STATUS.md.
// ---------------------------------------------------------------------------------------------
namespace detail {

namespace {
std::atomic<int> g_wake_port{9};   // the conventional Wake-on-LAN port (firehose WakeOnLan.PORT)

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n\f\v";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string lower_ascii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes one code point of valid UTF-8 at s[i]; returns its length (s must be valid UTF-8).
size_t decode(const std::string &s, size_t i, uint32_t &cp) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    if (i + len > s.size()) len = 1;
    cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    return len;
}

// Characters that display as nothing, and so can make a name look like another one or like
// nothing at all: C0 and C1 controls, DEL, and Unicode's Default_Ignorable_Code_Point set (soft
// hyphen, combining grapheme joiner, Arabic letter mark, Hangul fillers, Khmer inherent vowels,
// Mongolian variation selectors and vowel separator, zero-width characters, the bidirectional
// marks, overrides and isolates, word joiner and invisible operators, variation selectors, the
// byte-order mark, halfwidth Hangul filler, shorthand format controls, musical symbol
// formatting, tags), plus the interlinear annotation controls.
bool invisible(uint32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x00AD || cp == 0x034F || cp == 0x061C ||
           cp == 0x115F || cp == 0x1160 || cp == 0x17B4 || cp == 0x17B5 || (cp >= 0x180B && cp <= 0x180F) ||
           (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
           cp == 0x3164 || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xFFA0 ||
           (cp >= 0xFFF0 && cp <= 0xFFFB) || (cp >= 0x1BCA0 && cp <= 0x1BCA3) || (cp >= 0x1D173 && cp <= 0x1D17A) ||
           (cp >= 0xE0000 && cp <= 0xE0FFF);
}

// Unicode White_Space: a gap between words, wherever it comes from (tab, line breaks, NEL,
// no-break and ideographic spaces, the typographic spaces, line and paragraph separators).
bool white_space(uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000;
}
}  // namespace

void set_wake_port(int port) {
    if (port > 0 && port < 65536) g_wake_port = port;
}

int wake_port() { return g_wake_port.load(); }

std::vector<WakeSend> wake_destinations(const std::vector<Ipv4Interface> &interfaces, uint32_t tv) {
    std::vector<WakeSend> out;
    if ((tv >> 24) == 127) {   // only ever a test: never a broadcast
        out.push_back({tv, 0});
        return out;
    }
    for (const Ipv4Interface &i : interfaces) {
        if (!i.up || i.loopback || !i.can_broadcast || i.netmask == 0) continue;
        if ((i.addr & i.netmask) != (tv & i.netmask)) continue;   // the TV is not on this link
        out.push_back({i.broadcast, i.addr});
        out.push_back({0xFFFFFFFFu, i.addr});
    }
    return out;
}

std::string sanitize_utf8(const std::string &raw) {
    static const char kReplacement[] = "\xEF\xBF\xBD";
    std::string out;
    out.reserve(raw.size());
    const size_t n = raw.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
            i++;
            continue;
        }
        size_t len;
        uint32_t cp, min;
        if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1Fu, min = 0x80;
        else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0Fu, min = 0x800;
        else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07u, min = 0x10000;
        else {
            out += kReplacement;
            i++;
            continue;
        }
        bool ok = i + len <= n;
        for (size_t k = 1; ok && k < len; k++) {
            const unsigned char d = static_cast<unsigned char>(raw[i + k]);
            if ((d & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (d & 0x3Fu);
        }
        if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
        if (!ok) {
            out += kReplacement;
            i++;
            continue;
        }
        out.append(raw, i, len);
        i += len;
    }
    return out;
}

std::string clean_name(const std::string &raw, size_t max) {
    const std::string s = sanitize_utf8(raw.size() > 4096 ? raw.substr(0, 4096) : raw);
    std::string out;
    bool space = false;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp;
        const size_t len = decode(s, i, cp);
        if (white_space(cp)) {
            space = true;   // keep words apart
        } else if (invisible(cp)) {
            // dropped: what is on either side joins up
        } else {
            if (space && !out.empty()) out += ' ';
            space = false;
            out.append(s, i, len);
        }
        i += len;
    }
    if (out.size() > max) {
        size_t cut = max;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;   // not inside a sequence
        out.erase(cut);
        out = trim(out);
    }
    return out;
}

bool too_deep(const std::string &text, int max) {
    int depth = 0;
    bool in_string = false, escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            if (++depth > max) return true;
        } else if (c == ']' || c == '}') {
            depth--;
        }
    }
    return false;
}

std::string normalise_mac(const std::string &text) {
    const std::string t = trim(text);
    if (t.size() != 17) return "";
    unsigned char b[6];
    for (int i = 0; i < 6; i++) {
        const int hi = hex_digit(t[i * 3]), lo = hex_digit(t[i * 3 + 1]);
        if (hi < 0 || lo < 0) return "";
        if (i < 5 && t[i * 3 + 2] != ':' && t[i * 3 + 2] != '-') return "";
        if (i < 5 && t[i * 3 + 2] != t[2]) return "";   // one separator throughout
        b[i] = static_cast<unsigned char>(hi << 4 | lo);
    }
    // A group address (multicast or broadcast) or all zeros is not a network card's address.
    if (b[0] & 1) return "";
    if (!(b[0] | b[1] | b[2] | b[3] | b[4] | b[5])) return "";
    static const char d[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 6; i++) {
        if (i) out += ':';
        out += d[b[i] >> 4];
        out += d[b[i] & 15];
    }
    return out;
}

// After firehose Ssdp.macFromWakeup: the first ';'-delimited field after "MAC=", and only if it
// is shaped like an address.
std::string mac_from_wakeup(const std::string &value) {
    if (value.size() > 512) return "";
    const size_t at = lower_ascii(value).find("mac=");
    if (at == std::string::npos) return "";
    const size_t from = at + 4;
    return normalise_mac(value.substr(from, value.find(';', from) - from));
}

std::string announced_wake_mac(const std::map<std::string, std::string> &info) {
    const auto w = info.find("wakeup");
    if (w != info.end()) {
        const std::string mac = mac_from_wakeup(w->second);
        if (!mac.empty()) return mac;
    }
    const auto c = info.find("c");
    if (c != info.end()) return normalise_mac(c->second);
    return "";
}

// Six 0xFF bytes, then the address sixteen times (firehose WakeOnLan.magicPacket).
std::string magic_packet(const std::string &mac) {
    const std::string m = normalise_mac(mac);
    if (m.empty()) return "";
    std::string addr;
    for (int i = 0; i < 6; i++) addr += static_cast<char>(hex_digit(m[i * 3]) << 4 | hex_digit(m[i * 3 + 1]));
    std::string out(6, '\xff');
    for (int i = 0; i < 16; i++) out += addr;
    return out;
}

std::string spki_pin_from_pem(const std::string &pem) {
    if (pem.empty() || pem.size() > 64 * 1024) return "";
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return "";
    X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert) return "";
    std::string out;
    const EVP_PKEY *key = X509_get0_pubkey(cert);
    unsigned char *der = nullptr;
    const int len = key ? i2d_PUBKEY(key, &der) : -1;
    if (len > 0 && der) {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int md_len = 0;
        if (EVP_Digest(der, static_cast<size_t>(len), md, &md_len, EVP_sha256(), nullptr) && md_len == 32) {
            unsigned char b64[64];
            const int n = EVP_EncodeBlock(b64, md, 32);
            if (n > 0) out = "sha256//" + std::string(reinterpret_cast<char *>(b64), static_cast<size_t>(n));
        }
    }
    OPENSSL_free(der);
    X509_free(cert);
    return out;
}

// firehose FireTvClient.looksLikeToken (non-empty, at most 512, no whitespace or control
// characters), narrowed to printable ASCII because the token goes into a request header.
bool looks_like_token(const std::string &description) {
    if (description.empty() || description.size() > 512) return false;
    for (const char c : description)
        if (c < 0x21 || c > 0x7E) return false;
    return true;
}

PinReply read_pin_reply(long http_status, const std::string &body, std::string &token) {
    token.clear();
    if (http_status < 200 || http_status > 299) return PinReply::Refused;
    if (body.size() > 64 * 1024 || too_deep(body)) return PinReply::Unreadable;
    // Exactly one top-level "description" (a duplicate is refused, not guessed between, and one
    // nested deeper is not the TV's answer: firehose readDescription).
    int count = 0;
    const json::parser_callback_t count_keys = [&count](int depth, json::parse_event_t event, json &parsed) {
        if (event == json::parse_event_t::key && depth == 1 && parsed.is_string() &&
            parsed.get_ref<const std::string &>() == "description")
            count++;
        return true;
    };
    const json j = json::parse(body, count_keys, false);
    if (j.is_discarded() || !j.is_object() || count != 1) return PinReply::Unreadable;
    const auto it = j.find("description");
    if (it == j.end() || !it->is_string()) return PinReply::Unreadable;
    const std::string &d = it->get_ref<const std::string &>();
    if (d == "OK") return PinReply::NotYet;   // the code was accepted, the token is not minted yet ("OKAY" is a token)
    if (!looks_like_token(d)) return PinReply::Refused;
    token = d;
    return PinReply::Token;
}

bool parse_capabilities(const std::string &body, Capabilities &out) {
    if (body.size() > 64 * 1024 || too_deep(body)) return false;
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    Capabilities c;
    const auto level_supported = [&j](const char *key) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_string()) return false;
        const std::string &v = it->get_ref<const std::string &>();
        return !v.empty() && v != "NotSupported";
    };
    const auto flag = j.find("isVolumeControlsSupported");
    if (flag != j.end() && flag->is_boolean()) c.volume = flag->get<bool>();
    else c.volume = level_supported("volumeCapabilityLevelOfSupport");
    c.power = level_supported("powerCapabilityLevelOfSupport");
    out = c;
    return true;
}

bool valid_package(const std::string &id) {
    if (id.size() < 3 || id.size() > 128 || !std::isalpha(static_cast<unsigned char>(id[0]))) return false;
    if (id.back() == '.' || id.find("..") != std::string::npos || id.find('.') == std::string::npos) return false;
    for (const char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.') return false;
    return true;
}

bool parse_apps(const std::string &body, std::vector<App> &out) {
    out.clear();
    if (body.size() > 512 * 1024 || too_deep(body)) return false;
    const json j = json::parse(sanitize_utf8(body), nullptr, false);
    if (j.is_discarded()) return false;
    const json *list = &j;
    if (j.is_object()) {   // hms-firetv AppSyncService: tolerate a wrapper object
        for (const char *key : {"apps", "appsV2", "result"}) {
            const auto it = j.find(key);
            if (it != j.end() && it->is_array()) {
                list = &*it;
                break;
            }
        }
    }
    if (!list->is_array()) return false;
    std::set<std::string> seen;
    for (const json &item : *list) {
        if (out.size() >= 200) break;
        if (!item.is_object()) continue;
        const auto id = item.find("appId");
        if (id == item.end() || !id->is_string() || !valid_package(id->get_ref<const std::string &>())) continue;
        const auto shortcut = item.find("isShortcutApp");   // launcher tiles, not packages
        if (shortcut != item.end() && !(shortcut->is_boolean() && !shortcut->get<bool>())) continue;
        const auto installed = item.find("isInstalled");
        if (installed != item.end() && !(installed->is_boolean() && installed->get<bool>())) continue;
        const std::string &package = id->get_ref<const std::string &>();
        if (!seen.insert(package).second) continue;
        const auto name = item.find("name");
        std::string label = name != item.end() && name->is_string() ? clean_name(name->get_ref<const std::string &>()) : "";
        if (label.empty()) label = clean_name(package);
        out.push_back({label, package});
    }
    return true;
}

namespace {
// What is compared when a spoken app name is matched: letters and digits, lower case. Bytes
// outside ASCII are kept as they are, so names in other scripts still compare.
std::string match_key(const std::string &s) {
    std::string k;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x80) k += ch;
        else if (std::isalnum(c)) k += static_cast<char>(std::tolower(c));
    }
    return k;
}
}  // namespace

int match_app(const std::vector<App> &apps, const std::string &wanted, std::vector<std::string> &candidates) {
    candidates.clear();
    const std::string w = match_key(wanted);
    if (w.empty()) return -1;
    for (size_t i = 0; i < apps.size(); i++)
        if (match_key(apps[i].name) == w) return static_cast<int>(i);
    const std::string as_is = trim(wanted);
    for (size_t i = 0; i < apps.size(); i++)
        if (apps[i].id == as_is) return static_cast<int>(i);
    std::vector<size_t> hits;
    for (size_t i = 0; i < apps.size(); i++)
        if (match_key(apps[i].name).find(w) != std::string::npos) hits.push_back(i);
    if (hits.size() == 1) return static_cast<int>(hits[0]);
    for (size_t i = 0; i < hits.size() && candidates.size() < 5; i++) candidates.push_back(apps[hits[i]].name);
    return -1;
}

bool reads_as_input(const std::string &name) {
    const std::string k = match_key(name);
    // A connector, alone or with its number: "HDMI 1", "hdmi2", "HDMI-3", "AV", "Component", "Input 2".
    for (const char *port : {"hdmi", "av", "component", "composite", "scart", "vga", "dvi", "input"}) {
        const size_t n = strlen(port);
        if (k.size() >= n && k.size() <= n + 2 && k.compare(0, n, port) == 0 &&
            std::all_of(k.begin() + static_cast<std::ptrdiff_t>(n), k.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return true;
    }
    // The television's own tuner.
    for (const char *whole : {"tv", "livetv", "antenna", "aerial", "tuner"})
        if (k == whole) return true;
    return false;
}

namespace {
// Whether the app list kept in thing.ref holds an app called exactly `name` (ignoring case,
// spaces and punctuation, as match_app does), or one whose package name is `name`.
bool stored_app_named(const json &ref, const std::string &name) {
    if (!ref.is_object()) return false;
    const auto it = ref.find("apps");
    if (it == ref.end() || !it->is_array()) return false;
    const std::string wanted = match_key(name);
    for (const json &a : *it) {
        if (!a.is_array() || a.size() != 2 || !a[0].is_string() || !a[1].is_string()) continue;
        if (match_key(a[0].get_ref<const std::string &>()) == wanted || a[1].get_ref<const std::string &>() == trim(name))
            return true;
    }
    return false;
}

bool ref_flag(const json &ref, const char *key) {
    if (!ref.is_object()) return false;
    const auto it = ref.find(key);
    return it != ref.end() && it->is_boolean() && it->get<bool>();
}

// The shuttle bodies the vendor app sent (firehose FireTvClient.SCAN_BACK_BODY, SCAN_FORWARD_BODY).
const char kScanBack[] = R"({"direction":"back","durationInSeconds":"10","speed":"1"})";
const char kScanForward[] = R"({"direction":"forward","durationInSeconds":"10","speed":"1"})";

std::string join(const std::vector<std::string> &parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) out += (i ? ", " : "") + parts[i];
    return out;
}
}  // namespace

bool plan(const home::Thing &thing, const home::Change &change, std::vector<Action> &out, std::string &err) {
    out.clear();
    err.clear();
    const bool volume = ref_flag(thing.ref, "volume"), power = ref_flag(thing.ref, "power");
    std::vector<std::string> missing;   // what this TV does not offer over the network
    std::string bad;                    // a request that makes no sense for any TV
    const auto key = [&out](const char *action, int repeat = 1) {
        Action a;
        a.kind = Action::Key;
        a.what = action;
        a.repeat = repeat;
        out.push_back(a);
    };
    const auto media = [&out](const char *what, const char *body) {
        Action a;
        a.kind = Action::Media;
        a.what = what;
        a.body = body;
        out.push_back(a);
    };

    if (change.brightness) missing.push_back("brightness");
    if (change.kelvin) missing.push_back("colour temperature");
    if (change.hs) missing.push_back("colour");
    if (change.target_c || change.mode) missing.push_back("heating");
    if (change.position) missing.push_back("position");
    if (change.fan) missing.push_back("fan speed");
    if (change.volume) missing.push_back("a volume level (only volume up and down)");

    if (change.on && *change.on) {
        Action a;
        a.kind = Action::Wake;
        out.push_back(a);
    }
    if (change.key) {
        std::string k = lower_ascii(trim(*change.key));
        std::replace(k.begin(), k.end(), ' ', '_');
        std::replace(k.begin(), k.end(), '-', '_');
        if (k == "up") key("dpad_up");
        else if (k == "down") key("dpad_down");
        else if (k == "left") key("dpad_left");
        else if (k == "right") key("dpad_right");
        else if (k == "ok" || k == "select") key("select");
        else if (k == "back") key("back");
        else if (k == "home") key("home");
        else if (k == "menu") key("menu");
        else if (k == "volume_up" || k == "volume_down" || k == "mute") {
            if (volume) key(k == "volume_up" ? "volume_up" : k == "volume_down" ? "volume_down" : "mute");
            else missing.push_back(k == "mute" ? "mute" : "volume");
        } else if (k == "power") {
            if (power) key("power");
            else missing.push_back("the power key");
        } else if (k == "play_pause" || k == "play" || k == "pause") media("play", "");
        else if (k == "rewind") media("scan", kScanBack);
        else if (k == "fast_forward" || k == "forward") media("scan", kScanForward);
        else if (k == "channel_up" || k == "channel_down") missing.push_back("the channel keys");
        else if (k == "input") missing.push_back("the input key");
        else if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') missing.push_back("the number keys");
        else bad = "There is no TV key called \"" + clean_name(*change.key, 24) + "\".";
    }
    if (change.transport) {
        const std::string t = lower_ascii(trim(*change.transport));
        if (t == "play" || t == "pause" || t == "play_pause") media("play", "");   // one toggle on this API
        else if (t == "rewind") media("scan", kScanBack);
        else if (t == "fast_forward") media("scan", kScanForward);
        else if (t == "stop" || t == "next" || t == "previous") missing.push_back(t);
        else bad = "There is no playback control called \"" + clean_name(*change.transport, 24) + "\".";
    }
    if (change.volume_step && *change.volume_step != 0) {
        const int n = *change.volume_step;
        if (!volume) missing.push_back("volume");
        else if (n > 10 || n < -10) bad = "At most 10 volume steps can be made at once.";
        else key(n > 0 ? "volume_up" : "volume_down", n > 0 ? n : -n);
    }
    // A chosen mute state cannot be set: the TV's only mute control is a key that toggles, and
    // the API cannot read which way it is (an "unmute" sent as the toggle would mute an unmuted
    // TV). The mute key itself is Change::key "mute".
    if (change.mute) missing.push_back(volume ? "muting or unmuting (it only has a mute key that toggles)" : "mute");
    if (change.source) {
        const std::string s = trim(*change.source);
        if (s.empty()) bad = "Say which app to open.";
        // "HDMI 1" and the like name an input, which this API is not known to select (no source
        // records an input switch; survey 5.2: action=epg, the remote's TV/source button, has no
        // recorded effect): unsupported before anything is sent, so the caller can use infrared,
        // unless an installed app is called exactly that.
        else if (reads_as_input(s) && !stored_app_named(thing.ref, s)) missing.push_back("switching inputs");
        else {
            Action a;
            a.kind = Action::Launch;
            a.what = s.substr(0, 200);
            out.push_back(a);
        }
    }
    if (change.on && !*change.on) key("sleep");   // last, after anything else asked for

    if (!missing.empty()) {
        err = std::string(kUnsupported) + " " + join(missing) + ".";
    } else if (!bad.empty()) {
        err = bad;
    } else if (out.empty()) {
        err = "Nothing was asked of the TV.";
    }
    if (!err.empty()) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------
// Talking to the TV
// ---------------------------------------------------------------------------------------------
namespace {
using detail::App;
using detail::Capabilities;
using Clock = std::chrono::steady_clock;

const char kApiKey[] = "0987654321";   // the app's constant, the same on every TV: not a secret (firehose section 4)
const int kApiPort = 8080, kDialPort = 8009;
const char kFriendlyName[] = "Zero Bot";   // shown on the TV while it displays the code
const char kJson[] = "application/json";
const char kJsonUtf8[] = "application/json; charset=utf-8";   // what the forum thread found scan needs

const long kPinValidS = 300;        // the code expires after 5 minutes (firehose section 1, hms PairingController)
const int kPinAttempts = 3;         // "OK" means not yet: up to 3 attempts, 1 s apart (firehose, hms)
const long kPinRetryGapMs = 1000;
const long kPollEveryMs = 250;      // firehose WAKE_POLL_INTERVAL_MS
const long kWakeSettleMs = 3000;    // firehose WAKE_SETTLE_MS: the API answered within 1.84 s of a wake in 10 samples
const long kWakeAttemptMs = 2000;   // firehose WAKE_ATTEMPT_TIMEOUT_MS
const long kWakeEveryMs = 2500;     // firehose WAKE_ATTEMPT_INTERVAL_MS
const long kPressEveryMs = 220;     // the vendor app's fastest repeat (firehose section 2): never faster,
                                    // counted from the answer to one press to the sending of the next
const long kConnectMs = 1000;       // connecting to the API (TCP and TLS) at most, and a DIAL wake for a command
const long kResendReserveMs = 300;  // kept back from a wake so that what follows it can still be sent
const long kMinTryMs = 200;         // a request after the first of a change is not started with less, and a
                                    // request given less proves nothing about the TV when it gets no answer
// A TV in deep standby answered 6.5-7.4 s after the first magic packet (firehose section 3, four
// runs). A DIAL request that starts this long after the first packet and gets nothing shows that
// the TV is not coming on; before that, silence means "still resuming", not "no answer".
const long kDeepResumeMs = 8000;
const long kFinalTryMs = 300;       // on=true's last DIAL request starts this long before the deadline,
const long kWakeMinGapMs = 500;     // and not sooner than this after the request before it
const long kWakeMemoryMs = 60000;   // how long an unanswered wake packet is remembered across calls
const long kEnumerateMs = 4000, kReadMs = 2500, kShowCodeMs = 6000, kVerifyMs = 9000;
const size_t kSmallBody = 64 * 1024, kAppsBody = 512 * 1024;

// Words shown during pairing (README-snippet.md lists them). Step 1 is shown by the settings
// page as an AcceptPrompt step, whose button reads "I have accepted it" (settings_pages.cpp);
// the prompt names that button, since driver.h has no step kind with a plain "Continue".
const char kShowCodePrompt[] =
    "Turn on the Fire TV, then choose \"I have accepted it\" and the TV will show a 4-digit code.";
const char kEnterCodePrompt[] = "Type the 4-digit code the TV is showing. It stays valid for 5 minutes.";
const char kCodeField[] = "Code shown on the TV";
const char kCodeFormat[] = "Type the 4 digits the TV is showing.";
const char kStartAgain[] = " Start again and the TV will show a new code.";
const char kCodeExpired[] = "That code has expired.";
const char kCodeRefused[] = "The TV did not accept that code.";
const char kCodeUnreadable[] = "The TV's answer to the code was not understood.";
const char kCodeUnfinished[] = "The TV took the code but did not finish pairing.";
const char kDisplayRefused[] = "The TV refused to show a pairing code.";
const char kNoKey[] = "The TV's security key could not be read.";

const char kRefused[] = "The TV refused that command.";
// A request that had too little time left to prove anything (fault other: no infrared, no search).
const char kNoTime[] = "There was no time left to finish that. Try again in a moment.";
// on=false while a wake-up signal from here may still be bringing the TV up (after kWaking).
const char kNotSwitchedOff[] = " A wake-up signal was sent a moment ago, so it was not switched off. Ask again in a few seconds.";
const char kInternal[] = "Something went wrong while talking to the TV.";
const char kCannotPin[] = "This system cannot check the TV's security key, so nothing was sent.";

class Deadline {
public:
    explicit Deadline(long ms) : end_(Clock::now() + std::chrono::milliseconds(ms > 0 ? ms : 0)) {}
    long left() const {
        const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(end_ - Clock::now()).count());
        return ms > 0 ? ms : 0;
    }
    void sleep(long ms) const {
        const long s = std::min(ms, left());
        if (s > 0) std::this_thread::sleep_for(std::chrono::milliseconds(s));
    }

private:
    Clock::time_point end_;
};

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n\f\v";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// Where the TV is. Only an IP literal is accepted: a host name would need a DNS lookup, which
// libcurl cannot bound in time without its threaded resolver.
struct Target {
    std::string ip, host;   // host as it goes in a URL ("[...]" for IPv6)
    bool v6 = false, loopback = false;
    int api_port = kApiPort, dial_port = kDialPort;
};

// A port kept in Device::info as 1 to 5 digits; 0 when absent or not a port.
int info_port(const std::map<std::string, std::string> &info, const char *key) {
    const auto it = info.find(key);
    if (it == info.end() || it->second.empty() || it->second.size() > 5 ||
        it->second.find_first_not_of("0123456789") != std::string::npos)
        return 0;
    const int p = std::stoi(it->second);
    return p > 0 && p < 65536 ? p : 0;
}

bool target_of(const home::Device &dev, Target &t, std::string &err) {
    in_addr a4{};
    in6_addr a6{};
    if (dev.ip.size() < 64 && inet_pton(AF_INET, dev.ip.c_str(), &a4) == 1) {
        t.ip = t.host = dev.ip;
        t.loopback = (ntohl(a4.s_addr) >> 24) == 127;
    } else if (dev.ip.size() < 64 && inet_pton(AF_INET6, dev.ip.c_str(), &a6) == 1) {
        t.ip = dev.ip;
        t.host = "[" + dev.ip + "]";
        t.v6 = true;
        t.loopback = IN6_IS_ADDR_LOOPBACK(&a6);
    } else {
        err = "The TV's address is not valid.";
        return false;
    }
    if (dev.port < 0 || dev.port > 65535) {
        err = "The TV's port is not valid.";
        return false;
    }
    // The API port. Device::port is honoured only on a loopback address, i.e. a test pointing at
    // a mock (the common brief requires that). On a real network discovery fills Device::port
    // with the _amzn-wplay SRV port, which belongs to Amazon's Whisperlink service, not to this
    // API ("the sp/SRV ports are per-service and rotate. For the REST API on this class of
    // device, 8080 was correct and stable"; "Port 8080 does not speak Whisperlink": firehose
    // sections 5 and 7), so following it would send pairing and every command to the wrong
    // service. info "api_port" overrides both.
    t.api_port = dev.port && t.loopback ? dev.port : kApiPort;
    if (const int api = info_port(dev.info, "api_port")) t.api_port = api;
    t.dial_port = kDialPort;
    if (const int dial = info_port(dev.info, "dial_port")) t.dial_port = dial;
    return true;
}

struct Reply {
    long status = 0;           // HTTP status; 0 when nothing was answered
    std::string body;
    CURLcode rc = CURLE_OK;
    bool connected = false;    // a TCP connection was made
    bool sent = false;         // the request went out (TLS completed and it was written): the TV may have
                               // acted on it even when no answer came. 0 bytes when refused or TLS never
                               // completed (measured with this libcurl: CURLINFO_REQUEST_SIZE)
    bool too_big = false;      // the body was cut off at the size limit
    bool unpinned = false;     // the HTTPS library could not be told to check the key: nothing was sent
    std::string spki;          // the pin of the TV's certificate, when asked for
    bool answered() const { return status > 0; }
    bool ok() const { return status >= 200 && status <= 299; }
    bool complete() const { return ok() && rc == CURLE_OK && !too_big; }
    bool key_changed() const { return rc == CURLE_SSL_PINNEDPUBKEYNOTMATCH; }
    bool blocked() const { return key_changed() || unpinned; }   // stopped on our side: never a reason to wake or resend
};

struct Request {
    bool post = false;
    std::string url, body;
    std::vector<std::string> headers;
    std::string pin;           // "sha256//..." to require that key, "" for none (first contact only)
    bool want_spki = false;
    size_t max_body = kSmallBody;
    long connect_ms = 1000, total_ms = 2000;
};

struct Sink {
    std::string *body;
    size_t max;
    bool over = false;
};

size_t on_data(char *p, size_t size, size_t n, void *user) {
    Sink *s = static_cast<Sink *>(user);
    const size_t len = size * n;
    if (len > s->max || s->body->size() > s->max - len) {
        s->over = true;
        return 0;   // stops the transfer
    }
    s->body->append(p, len);
    return len;
}

// One HTTP exchange on a connection of its own. Every phase (connect, TLS, transfer) is bounded
// by total_ms; connecting (TCP and TLS) by connect_ms.
Reply perform(const Request &r) {
    Reply out;
    if (r.total_ms <= 0) {
        out.rc = CURLE_OPERATION_TIMEDOUT;
        return out;
    }
    CURL *h = curl_easy_init();
    if (!h) {
        out.rc = CURLE_FAILED_INIT;
        return out;
    }
    Sink sink{&out.body, r.max_body};
    curl_slist *headers = nullptr;
    const auto add = [&headers](const char *line) {
        if (curl_slist *grown = curl_slist_append(headers, line)) headers = grown;
    };
    for (const std::string &line : r.headers) add(line.c_str());
    add("Expect:");
    const bool https = starts_with(r.url, "https://");
    curl_easy_setopt(h, CURLOPT_URL, r.url.c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_PROXY, "");   // a TV is on the local network
    curl_easy_setopt(h, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, std::max(1L, std::min(r.connect_ms, r.total_ms)));
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, r.total_ms);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, https ? "https" : "http");
#else
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, static_cast<long>(https ? CURLPROTO_HTTPS : CURLPROTO_HTTP));
#endif
    if (https) {
        // The certificate is self-signed for a host name while the TV is reached by address, so
        // there is nothing to verify it against: the key is pinned at pairing instead, and a
        // different key ends the connection before anything is sent (CURLE_SSL_PINNEDPUBKEYNOTMATCH).
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
        if (!r.pin.empty() && curl_easy_setopt(h, CURLOPT_PINNEDPUBLICKEY, r.pin.c_str()) != CURLE_OK) {
            out.unpinned = true;   // a TLS backend without pinning: the token must not go out unchecked
            curl_slist_free_all(headers);
            curl_easy_cleanup(h);
            return out;
        }
        if (r.want_spki) curl_easy_setopt(h, CURLOPT_CERTINFO, 1L);
    }
    if (r.post) {
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, r.body.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
    } else {
        curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);

    out.rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out.status);
    curl_off_t connect_us = 0;
    if (curl_easy_getinfo(h, CURLINFO_CONNECT_TIME_T, &connect_us) == CURLE_OK) out.connected = connect_us > 0;
    // The device's libcurl (8.14) reports no connect time when TCP was accepted but TLS never
    // finished; the number of connections made says it either way.
    long connects = 0;
    if (curl_easy_getinfo(h, CURLINFO_NUM_CONNECTS, &connects) == CURLE_OK && connects > 0) out.connected = true;
    long request_bytes = 0;
    // When the size cannot be read, assume the request went out: then it is never sent twice.
    out.sent = curl_easy_getinfo(h, CURLINFO_REQUEST_SIZE, &request_bytes) != CURLE_OK || request_bytes > 0;
    out.too_big = sink.over;
    if (r.want_spki && out.answered()) {
        curl_certinfo *info = nullptr;
        if (curl_easy_getinfo(h, CURLINFO_CERTINFO, &info) == CURLE_OK && info && info->num_of_certs > 0) {
            for (curl_slist *item = info->certinfo[0]; item; item = item->next)   // [0] is the TV's own
                if (item->data && strncmp(item->data, "Cert:", 5) == 0) out.spki = detail::spki_pin_from_pem(item->data + 5);
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    return out;
}

// Wake-on-LAN, best effort (firehose UdpWakeOnLan): the subnet broadcast of every interface the
// TV is on, and 255.255.255.255 sent from that interface's address (detail::wake_destinations
// decides; this only sends). Nothing for an IPv6 TV, nor when no interface shares the TV's
// subnet. Returns how many packets the system took to send: 0 means no wake-up signal went out.
int send_magic(const Target &t, const std::string &mac) {
    const std::string packet = detail::magic_packet(mac);
    if (packet.empty() || t.v6) return 0;
    in_addr tv{};
    if (inet_pton(AF_INET, t.ip.c_str(), &tv) != 1) return 0;
    std::vector<detail::Ipv4Interface> interfaces;
    if (!t.loopback) {   // a loopback TV (a test) needs no interface list
        ifaddrs *list = nullptr;
        if (getifaddrs(&list) != 0) return 0;
        for (const ifaddrs *i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
            detail::Ipv4Interface f;
            f.addr = ntohl(reinterpret_cast<const sockaddr_in *>(i->ifa_addr)->sin_addr.s_addr);
            f.netmask = ntohl(reinterpret_cast<const sockaddr_in *>(i->ifa_netmask)->sin_addr.s_addr);
            f.up = i->ifa_flags & IFF_UP;
            f.loopback = i->ifa_flags & IFF_LOOPBACK;
            f.can_broadcast = (i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr;
            if (f.can_broadcast) f.broadcast = ntohl(reinterpret_cast<const sockaddr_in *>(i->ifa_broadaddr)->sin_addr.s_addr);
            interfaces.push_back(f);
        }
        freeifaddrs(list);
    }
    const int port = detail::wake_port();
    int sent = 0;
    for (const detail::WakeSend &w : detail::wake_destinations(interfaces, ntohl(tv.s_addr))) {
        const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) continue;
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        if (w.from) {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(w.from);
            if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof local) != 0) {
                close(fd);
                continue;
            }
        }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<uint16_t>(port));
        to.sin_addr.s_addr = htonl(w.to);
        if (sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof to) ==
            static_cast<ssize_t>(packet.size()))
            sent++;
        close(fd);
    }
    return sent;
}

// ---- what is kept in Device::auth
struct Pairing {
    std::string token, pin;
};

std::string auth_string(const json &auth, const char *key) {
    if (!auth.is_object()) return "";
    const auto it = auth.find(key);
    return it != auth.end() && it->is_string() ? it->get<std::string>() : "";
}

bool valid_pin(const std::string &pin) { return starts_with(pin, "sha256//") && pin.size() == 52; }

bool paired(const home::Device &dev, Pairing &p) {
    p.token = auth_string(dev.auth, "token");
    p.pin = auth_string(dev.auth, "key_pin");
    // Never send the token without the key it was issued over.
    return detail::looks_like_token(p.token) && valid_pin(p.pin);
}

bool pending_code(const json &auth, std::string &pin) {
    pin.clear();
    if (!auth.is_object()) return false;
    const auto it = auth.find("pending");
    if (it == auth.end() || !it->is_object()) return false;
    const auto asked = it->find("asked");
    const auto key = it->find("key_pin");
    if (asked == it->end() || !asked->is_number_integer() || key == it->end() || !key->is_string()) return false;
    // The stored time comes from a file that may have been edited: compare before subtracting,
    // so that no value (LLONG_MIN, a huge unsigned number) can overflow.
    const long long now = static_cast<long long>(time(nullptr));
    if (asked->is_number_unsigned() && asked->get<unsigned long long>() > static_cast<unsigned long long>(now)) return false;
    const long long at = asked->get<long long>();
    if (at <= 0 || at > now || now - at >= kPinValidS || !valid_pin(key->get<std::string>())) return false;
    pin = key->get<std::string>();
    return true;
}

// The stored wake address, else the one discovery heard announced (never stored over one).
std::string wake_mac_of(const home::Device &dev) {
    const std::string stored = detail::normalise_mac(auth_string(dev.auth, "wake_mac"));
    return stored.empty() ? detail::announced_wake_mac(dev.info) : stored;
}

void learn_wake_mac(home::Device &dev) {
    if (!dev.auth.is_object()) return;
    if (!detail::normalise_mac(auth_string(dev.auth, "wake_mac")).empty()) return;   // never replaced
    const std::string mac = detail::announced_wake_mac(dev.info);
    if (!mac.empty()) dev.auth["wake_mac"] = mac;
}

// ---- one conversation with the TV, within one deadline
// NoTime: there was too little time left to try a wake, so nothing is known.
enum class Woken { Up, NoAnswer, NotServing, Waking, KeyChanged, NoTime };

// What a request does, for judging its outcome.
struct What {
    const char *name = nullptr;             // for kUnsupported when the TV does not know it ("the home key")
    bool bad_request_unsupported = false;   // a 400 means the TV does not know this key or media action
    bool acts = true;                       // it makes the TV do something (a list read does not)
};

class Session {
public:
    Session(const Target &t, const Pairing &p, const Deadline &dl) : t_(t), p_(p), dl_(dl) {}

    const Deadline &deadline() const { return dl_; }
    // Whether the TV was heard from in this session: an HTTP answer, or a connection it accepted,
    // on either port. Then it is on the network.
    bool heard() const { return heard_; }
    // A wake in this session brought the API up (power_on, or the wake before a resend).
    void set_woken_up() { woken_up_ = true; }

    // One request to the API with both headers (the token when paired).
    Reply api(bool post, const std::string &path, const std::string &body = "", const char *type = kJson,
              size_t max_body = kSmallBody, long cap_ms = 1500, bool want_spki = false) const {
        Request r;
        r.post = post;
        r.url = "https://" + t_.host + ":" + std::to_string(t_.api_port) + path;
        r.body = body;
        r.headers = {std::string("X-Api-Key: ") + kApiKey, std::string("Content-Type: ") + type};
        if (!p_.token.empty()) r.headers.push_back("X-Client-Token: " + p_.token);
        r.pin = p_.pin;
        r.want_spki = want_spki;
        r.max_body = max_body;
        r.total_ms = std::min(cap_ms, dl_.left());
        r.connect_ms = std::min(kConnectMs, r.total_ms);
        return note(perform(r));
    }

    // The DIAL launch of FireTVRemote that wakes the TV. Sent like the official app sends it
    // (empty body, text/plain), as captured in the forum thread. `connect_ms` bounds connecting;
    // on=true's wake attempts pass their whole length, as firehose does (its 2 s limit is set for
    // connecting and for the answer alike: HttpUrlTransport), because a TV resuming from deep
    // standby may first answer a retried connection attempt a second or more in.
    Reply dial(long cap_ms, long connect_ms = kConnectMs) const {
        Request r;
        r.post = true;
        r.url = "http://" + t_.host + ":" + std::to_string(t_.dial_port) + "/apps/FireTVRemote";
        r.headers = {"Content-Type: text/plain; charset=utf-8"};
        r.total_ms = std::min(cap_ms, dl_.left());
        r.connect_ms = std::min(connect_ms, r.total_ms);
        r.max_body = 16 * 1024;
        return note(perform(r));
    }

    // Waits for the API to answer anything at all (a 403 for the missing headers is an answer:
    // firehose awaitCommandApi sends none). `silent` says the API was already seen accepting a
    // connection without answering; with `reserve` milliseconds kept back for a resend.
    Woken await_api(bool silent, long reserve) const {
        const Deadline window(std::min(kWakeSettleMs, dl_.left()));
        for (;;) {
            const long left = std::min(window.left(), dl_.left() - reserve);
            if (left <= 0) break;
            const auto started = Clock::now();
            Request r;
            r.url = "https://" + t_.host + ":" + std::to_string(t_.api_port) + "/v1/FireTV/status";
            r.pin = p_.pin;
            r.total_ms = std::min(1000L, left);
            r.connect_ms = r.total_ms;
            const Reply probe = note(perform(r));
            if (probe.blocked()) return Woken::KeyChanged;
            if (probe.answered()) return Woken::Up;
            if (probe.connected) silent = true;   // accepted, then said nothing
            const long spent = static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
            if (spent < kPollEveryMs) dl_.sleep(std::min(kPollEveryMs - spent, std::max(0L, window.left())));
        }
        return silent ? Woken::NotServing : Woken::Waking;
    }

    // A DIAL wake, then the wait for the API. `silent`: the API had accepted without answering.
    // With too little time left for the DIAL request it is not sent: a request that has no time
    // to be answered says nothing about the TV, and must not be read as "no answer".
    Woken wake(bool silent) {
        if (dl_.left() < kMinTryMs) return Woken::NoTime;
        const Reply d = dial(std::min(1500L, dl_.left()));
        if (!d.answered()) return silent || d.connected ? Woken::NotServing : Woken::NoAnswer;
        if (!d.ok()) return Woken::NotServing;
        const Woken w = await_api(silent, kResendReserveMs);
        if (w == Woken::Up) woken_up_ = true;
        return w;
    }

    // Sends one request. When it could not be delivered at all (the connection was refused, or
    // TCP was accepted and TLS never completed, so no byte of it left), wakes the TV once per
    // session and sends it again: an idle TV refuses its API port until woken, and that is how
    // the official app and firehose (withWakeRecovery) recover. A request that went out but got
    // no answer in time is never sent again: a TV that is slow to answer has already carried it
    // out, and a toggle (play/pause, mute, power) sent twice undoes itself, a volume step or a
    // D-pad move doubles. Nor is the TV woken then: an API that accepts and stays silent is
    // stuck, not asleep, and waking does not help (firehose section 3). (Departure: firehose and
    // the vendor app also wake and resend after a timeout.) An HTTP answer of any kind is final.
    // A request that is not the first of the session is not started with less than kMinTryMs
    // left (an earlier part of the change used the time): it could only fail for lack of time,
    // and that failure would read as a TV that is not there.
    bool command(const std::function<Reply()> &send, const What &what, std::string &err) {
        if (used_ && dl_.left() < kMinTryMs) return starved(err);
        const long window = std::min(kConnectMs, dl_.left());   // the time the request has to connect
        Reply r = send();
        if (!r.answered() && !r.blocked() && !r.sent) {
            if (woke_) return no_answer(r, err);
            woke_ = true;
            const Woken w = wake(r.connected);
            if (w == Woken::NoTime) {
                // No time left to try the wake. A request that could not even connect in a fair
                // window still shows that nothing answered (unless a wake just brought the TV up
                // in this session); anything less proves nothing.
                if (!woken_up_ && !r.connected && window >= kMinTryMs) return woken_error(Woken::NoAnswer, err);
                return starved(err);
            }
            if (w != Woken::Up) return woken_error(w, err);
            r = send();
        }
        if (!r.answered() && !r.blocked()) return r.sent ? unconfirmed(what, err) : no_answer(r, err);
        return judge(r, err, what);
    }

    // What a request that had no time left reports. After a wake in this session the TV is
    // coming up: kWaking. Otherwise nothing is known: kNoTime. Never kNoAnswer, which would hand
    // the change to infrared (whose power key toggles the TV that was just switched on).
    bool starved(std::string &err) const {
        err = woken_up_ ? std::string(kWaking) + " Try again in a moment." : kNoTime;
        return false;
    }

    static bool judge(const Reply &r, std::string &err, const What &what = What()) {
        if (r.unpinned) {
            err = kCannotPin;
            return false;
        }
        if (r.key_changed()) {
            err = std::string(kKeyChanged) + " Nothing was sent to it. If the TV was reset, pair it again in the settings.";
            return false;
        }
        if (r.ok()) return true;   // the TV took the command, whatever came with the answer
        if (r.status == 401 || r.status == 403) {
            err = std::string(kPairAgain) + " Pair it again in the settings.";
        } else if (what.name && (r.status == 404 || r.status == 405 || (r.status == 400 && what.bad_request_unsupported))) {
            // A route or method the TV does not serve (firehose section 2 records 405 for those),
            // or arguments it does not take (the forum's 400 "Bad arguments supplied"): this TV,
            // or a changed API, does not offer it, so the caller may use infrared.
            err = std::string(kUnsupported) + " " + what.name + ".";
        } else {
            err = kRefused;
        }
        return false;
    }

    // A request that went out and got no answer within the deadline.
    static bool unconfirmed(const What &what, std::string &err) {
        err = kNotServing;
        if (what.acts) err += " The command may still have been carried out.";
        return false;
    }

    static bool woken_error(Woken w, std::string &err) {
        switch (w) {
        case Woken::Up: return true;
        case Woken::NoAnswer: err = std::string(kNoAnswer) + " It may be off or unplugged."; break;
        case Woken::NotServing: err = kNotServing; break;
        case Woken::Waking: err = std::string(kWaking) + " Try again in a moment."; break;
        case Woken::KeyChanged:
            err = std::string(kKeyChanged) + " Nothing was sent to it. If the TV was reset, pair it again in the settings.";
            break;
        case Woken::NoTime: err = kNoTime; break;
        }
        return false;
    }

    static bool no_answer(const Reply &r, std::string &err) {
        // A request that could not be delivered after a wake that worked: a connection accepted
        // without TLS is a stuck service; a refusal is a TV going back to sleep.
        return woken_error(r.connected ? Woken::NotServing : Woken::Waking, err);
    }

    const Target &target() const { return t_; }

private:
    // Every reply passes through here: a request was made, and whether the TV was heard.
    Reply note(Reply r) const {
        used_ = true;
        if (r.answered() || r.connected) heard_ = true;
        return r;
    }

    const Target &t_;
    const Pairing &p_;
    const Deadline &dl_;
    bool woke_ = false;               // a wake was tried for a command (once per session)
    bool woken_up_ = false;           // a wake in this session brought the API up
    mutable bool used_ = false;       // a request went to the TV in this session
    mutable bool heard_ = false;      // the TV answered, or accepted a connection, in this session
};

// ---- the thing
const char kThingId[] = "tv";

home::Thing base_thing(const home::Device &dev) {
    home::Thing t;
    for (const home::Thing &old : dev.things)
        if (old.id == kThingId) {
            t.ref = old.ref;
            t.state = old.state;
        }
    t.id = kThingId;
    t.name = detail::clean_name(dev.name);
    if (t.name.empty()) t.name = "Fire TV";
    t.type = "tv";
    if (!t.ref.is_object()) t.ref = json::object();
    if (!t.state.is_object()) t.state = json::object();
    return t;
}

std::vector<App> apps_of(const json &ref) {
    std::vector<App> apps;
    if (!ref.is_object()) return apps;
    const auto it = ref.find("apps");
    if (it == ref.end() || !it->is_array()) return apps;
    for (const json &a : *it) {
        if (!a.is_array() || a.size() != 2 || !a[0].is_string() || !a[1].is_string()) continue;
        const std::string &id = a[1].get_ref<const std::string &>();
        if (!detail::valid_package(id)) continue;
        apps.push_back({detail::clean_name(a[0].get_ref<const std::string &>()), id});
    }
    return apps;
}

void set_apps(home::Thing &t, const std::vector<App> &apps) {
    json pairs = json::array(), names = json::array();
    for (const App &a : apps) {
        pairs.push_back(json::array({a.name, a.id}));
        names.push_back(a.name);
    }
    t.ref["apps"] = pairs;
    t.state["apps"] = names;
}

void set_caps(home::Thing &t) {
    t.caps = home::kPower | home::kKeys | home::kTransport | home::kSource;
    if (detail::ref_flag(t.ref, "volume")) t.caps |= home::kVolumeStep;
}

bool four_digits(const std::string &s) {
    return s.size() == 4 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// ---------------------------------------------------------------------------------------------
// The driver
// ---------------------------------------------------------------------------------------------
class FireTvDriver : public home::Driver {
public:
    const char *kind() const override { return "fire_tv"; }

    home::PairStep next_step(const home::Device &dev) const override {
        home::PairStep step;
        try {
            Pairing p;
            std::string pin;
            if (paired(dev, p)) return step;   // None: ready
            if (pending_code(dev.auth, pin)) {
                step.kind = home::Pairing::EnterCode;
                step.prompt = kEnterCodePrompt;
                step.field = kCodeField;
                return step;
            }
        } catch (...) {
        }
        step.kind = home::Pairing::AcceptPrompt;   // driver.h has no plain "continue" step: see STATUS.md
        step.prompt = kShowCodePrompt;
        step.wait_s = 0;
        return step;
    }

    bool pair(home::Device &dev, const std::string &input, std::string &err) override {
        try {
            err.clear();
            if (!dev.auth.is_object()) dev.auth = json::object();
            Target t;
            if (!target_of(dev, t, err)) return false;
            const std::string code = trim(input);
            if (code.empty()) return show_code(dev, t, err);
            if (!four_digits(code)) {   // nothing is sent for a code that cannot be right
                err = kCodeFormat;
                return false;
            }
            return verify(dev, t, code, err);
        } catch (...) {
            err = kInternal;
            return false;
        }
    }

    // The API has no call to forget a client: the TV is not told, the pairing is dropped here.
    void unpair(home::Device &dev) override { dev.auth = json::object(); }

    bool enumerate(home::Device &dev, std::vector<home::Thing> &out, std::string &err) override {
        try {
            err.clear();
            out.clear();
            Target t;
            Pairing p;
            if (!target_of(dev, t, err)) return false;
            if (!paired(dev, p)) {
                err = std::string(kNotPaired) + " Pair it in the settings first.";
                return false;
            }
            const Deadline dl(kEnumerateMs);
            const Session s(t, p, dl);
            home::Thing thing = base_thing(dev);
            const Reply caps = s.api(false, "/v1/FireTV", "", kJson, kSmallBody, 1500);
            if (caps.blocked() || caps.status == 401 || caps.status == 403) {
                Session::judge(caps, err);
                return false;
            }
            Capabilities c;
            if (caps.complete() && detail::parse_capabilities(caps.body, c)) {
                thing.ref["volume"] = c.volume;
                thing.ref["power"] = c.power;
                thing.ref["caps_read"] = true;
            }   // otherwise what was known before stands (nothing, on a first read)
            if (caps.ok()) {
                const Reply list = s.api(false, "/v1/FireTV/appsV2", "", kJson, kAppsBody, 2500);
                std::vector<App> apps;
                if (list.complete() && detail::parse_apps(list.body, apps)) set_apps(thing, apps);
            }
            thing.state["reachable"] = caps.ok();
            if (!thing.state.contains("apps")) thing.state["apps"] = json::array();
            set_caps(thing);
            learn_wake_mac(dev);
            out.push_back(thing);
            return true;
        } catch (...) {
            err = kInternal;
            out.clear();
            return false;
        }
    }

    bool read(const home::Device &dev, std::vector<home::Thing> &things, std::string &err) override {
        try {
            err.clear();
            Target t;
            Pairing p;
            if (!target_of(dev, t, err)) return false;
            if (!paired(dev, p)) {
                err = std::string(kNotPaired) + " Pair it in the settings first.";
                return false;
            }
            if (things.empty()) things.push_back(base_thing(dev));
            const Deadline dl(kReadMs);
            const Session s(t, p, dl);
            const Reply r = s.api(false, "/v1/FireTV/status", "", kJson, kSmallBody, 1500);
            if (r.blocked() || r.status == 401 || r.status == 403) {
                Session::judge(r, err);
                return false;
            }
            for (home::Thing &thing : things) {
                if (thing.id != kThingId) continue;
                if (!thing.state.is_object()) thing.state = json::object();
                thing.state["reachable"] = r.ok();
            }
            return true;
        } catch (...) {
            err = kInternal;
            return false;
        }
    }

    bool apply(const home::Device &dev, const home::Thing &thing, const home::Change &change, int deadline_ms,
               std::string &err) override {
        try {
            err.clear();
            const Deadline dl(std::max(1, std::min(deadline_ms, 120000)));
            Target t;
            Pairing p;
            if (!target_of(dev, t, err)) return false;
            if (!paired(dev, p)) {
                err = std::string(kNotPaired) + " Pair it in the settings first.";
                return false;
            }
            if (thing.id != kThingId) {
                err = "The TV has nothing by that name.";
                return false;
            }
            std::vector<detail::Action> actions;
            if (!detail::plan(thing, change, actions, err)) return false;   // nothing has been sent
            const std::string who = dev.id.empty() ? dev.ip : dev.id;   // the key of the wake memory
            Session s(t, p, dl);
            bool ok = true;
            for (size_t i = 0; ok && i < actions.size(); i++) {
                const detail::Action &a = actions[i];
                switch (a.kind) {
                case detail::Action::Wake:   // keeps time back when more of the change follows it
                    ok = power_on(s, dev, who, i + 1 < actions.size() ? kResendReserveMs : 0, err);
                    break;
                case detail::Action::Key: ok = press(s, a, who, err); break;
                case detail::Action::Media: ok = media(s, a, err); break;
                case detail::Action::Launch: ok = launch(s, thing, a.what, err); break;
                }
            }
            // The TV answered something: it is on, so an earlier unanswered wake-up signal no
            // longer says anything about it.
            if (s.heard()) forget_wake(who);
            return ok;
        } catch (...) {
            err = kInternal;
            return false;
        }
    }

private:
    // Pairing step 1: the TV shows a code; the key it presented is remembered for step 2.
    bool show_code(home::Device &dev, const Target &t, std::string &err) {
        const Deadline dl(kShowCodeMs);
        const Pairing none;   // first contact: no token, nothing to pin yet
        Session s(t, none, dl);
        const std::string body = json{{"friendlyName", kFriendlyName}}.dump();
        const auto send = [&] { return s.api(true, "/v1/FireTV/pin/display", body, kJson, kSmallBody, dl.left(), true); };
        Reply r = send();
        if (!r.answered() && !r.sent) {   // never delivered: an idle TV refuses until woken (firehose requestPin)
            const Woken w = s.wake(r.connected);
            if (w != Woken::Up) return pairing_error(w, err);
            r = send();
        }
        // Delivered and unanswered: not sent again (the TV may be showing a code already).
        if (!r.answered()) return pairing_error(r.sent || r.connected ? Woken::NotServing : Woken::Waking, err);
        if (!r.ok()) {
            err = kDisplayRefused;
            return false;
        }
        if (!valid_pin(r.spki)) {
            err = kNoKey;
            return false;
        }
        dev.auth["pending"] = {{"key_pin", r.spki}, {"asked", static_cast<long long>(time(nullptr))}};
        return true;
    }

    static bool pairing_error(Woken w, std::string &err) {
        switch (w) {
        case Woken::NoAnswer: err = std::string(kNoAnswer) + " Check that it is on and connected to this network."; break;
        case Woken::NotServing: err = std::string(kNotServing) + " Try again later."; break;
        case Woken::Waking: err = std::string(kWaking) + " Try again in a moment."; break;
        case Woken::KeyChanged: err = std::string(kKeyChanged) + kStartAgain; break;
        case Woken::NoTime: err = kNoTime; break;
        case Woken::Up: return true;
        }
        return false;
    }

    // Pairing step 2. The code goes to the TV once; it is sent again only after the literal
    // "OK" (the TV took it but has no token yet), or when it could not be delivered at all (the
    // connection was refused, or TLS never completed: then the TV is woken first, as firehose
    // does, because reading the code off the screen gives an idle TV time to stop listening).
    // Anything else ends this code: the user starts again for a new one. The request is pinned
    // to the key the TV presented in step 1, so a different TV never receives the code.
    bool verify(home::Device &dev, const Target &t, const std::string &code, std::string &err) {
        std::string pending_pin;
        if (!pending_code(dev.auth, pending_pin)) {
            dev.auth.erase("pending");
            err = std::string(kCodeExpired) + kStartAgain;
            return false;
        }
        dev.auth.erase("pending");   // whatever happens now, this code is used up
        const Deadline dl(kVerifyMs);
        Pairing pp;
        pp.pin = pending_pin;   // the key the TV showed the code over
        Session s(t, pp, dl);
        const std::string body = json{{"pin", code}}.dump();
        bool woke = false;
        for (int attempt = 0; attempt < kPinAttempts; attempt++) {
            if (attempt) dl.sleep(kPinRetryGapMs);
            Reply r = s.api(true, "/v1/FireTV/pin/verify", body, kJson, kSmallBody, 2000);
            if (r.blocked()) {
                if (r.unpinned) err = kCannotPin;
                else pairing_error(Woken::KeyChanged, err);
                return false;
            }
            if (!r.answered() && !r.sent && !woke) {
                woke = true;
                const Woken w = s.wake(r.connected);
                if (w != Woken::Up) {
                    err = std::string(w == Woken::NotServing   ? kNotServing
                                      : w == Woken::Waking     ? kWaking
                                      : w == Woken::KeyChanged ? kKeyChanged
                                      : w == Woken::NoTime     ? "There was no time left to finish that."
                                                               : kNoAnswer) + kStartAgain;
                    return false;
                }
                r = s.api(true, "/v1/FireTV/pin/verify", body, kJson, kSmallBody, 2000);
                if (r.blocked()) {
                    if (r.unpinned) err = kCannotPin;
                    else pairing_error(Woken::KeyChanged, err);
                    return false;
                }
            }
            if (!r.answered()) {   // delivered (it may have counted it): never sent twice
                err = std::string(r.sent || r.connected ? kNotServing : kNoAnswer) + kStartAgain;
                return false;
            }
            std::string token;
            switch (detail::read_pin_reply(r.status, r.body, token)) {
            case detail::PinReply::Token:
                dev.auth["token"] = token;
                dev.auth["key_pin"] = pp.pin;
                dev.auth["paired_at"] = static_cast<long long>(time(nullptr));
                learn_wake_mac(dev);
                return true;
            case detail::PinReply::NotYet: continue;
            case detail::PinReply::Refused: err = std::string(kCodeRefused) + kStartAgain; return false;
            case detail::PinReply::Unreadable: err = std::string(kCodeUnreadable) + kStartAgain; return false;
            }
        }
        err = std::string(kCodeUnfinished) + kStartAgain;
        return false;
    }

    // on=true (firehose wake()): a magic packet, when the TV announced a wake address, and the
    // DIAL request, again every 2.5 s until the DIAL request is answered; then the wait for the
    // API. `keep_ms` is kept back for whatever the change asks after the wake.
    //
    // When nothing answers, the verdict rests on the latest DIAL request, never on the clock
    // alone. A TV in deep standby comes on 6.5-7.4 s after the first magic packet (measured), so
    // only a request that started kDeepResumeMs or more after that packet (sent in this call, or
    // in an earlier on=true for this device that got no answer) and got nothing shows that the
    // TV is not coming on: kNoAnswer, and the caller may use infrared. Before that the answer is
    // kWaking, so that nothing toggles power by infrared while the TV may be resuming. For the
    // latest evidence, when the next 2.5 s slot does not fit before the deadline one more DIAL
    // request (no packet) goes out kFinalTryMs before it, instead of waiting idle. Without a
    // packet that actually left (no wake address, an IPv6 TV, no interface on the TV's subnet)
    // nothing is waking the TV: kNoAnswer.
    bool power_on(Session &s, const home::Device &dev, const std::string &who, long keep_ms, std::string &err) {
        using std::chrono::milliseconds;
        const Deadline &dl = s.deadline();
        const long keep = std::max(0L, std::min(keep_ms, dl.left()));
        const auto started = Clock::now();
        const auto end = started + milliseconds(dl.left() - keep);   // the wake's share of the deadline
        const auto last_call = end - milliseconds(kFinalTryMs);
        const auto ms_to = [](Clock::time_point t) {
            const long n = static_cast<long>(std::chrono::duration_cast<milliseconds>(t - Clock::now()).count());
            return n > 0 ? n : 0L;
        };
        const std::string mac = wake_mac_of(dev);
        std::optional<Clock::time_point> origin = remembered_wake(who);   // the first packet of this attempt to wake it
        std::optional<Clock::time_point> last, evidence;   // the latest request; the latest that fairly showed nothing
        bool dial_ok = false, dial_refused_by_tv = false, connected = false;
        for (int k = 0;; k++) {
            auto at = started + milliseconds(k * kWakeEveryMs);
            bool final = false;
            if (k > 0 && at + milliseconds(kMinTryMs) > end) {
                // The next slot does not fit: one last request, when the verdict depends on it.
                if (!origin || !last || last_call < *last + milliseconds(kWakeMinGapMs)) break;
                at = last_call;
                final = true;
            }
            dl.sleep(ms_to(at));
            const auto begun = Clock::now();
            if (k > 0 && begun >= end) break;
            if (!final && !mac.empty() && send_magic(s.target(), mac) > 0 && !origin) origin = remember_wake(who, begun);
            // An attempt has kWakeAttemptMs to connect and be answered, and ends in time for the
            // last request when that is to follow.
            auto until = std::min(begun + milliseconds(kWakeAttemptMs), end);
            if (!final && origin && started + milliseconds((k + 1) * kWakeEveryMs + kMinTryMs) > end &&
                last_call >= begun + milliseconds(kWakeMinGapMs))
                until = std::min(until, last_call);
            const long budget = std::max(1L, static_cast<long>(std::chrono::duration_cast<milliseconds>(until - begun).count()));
            const Reply d = s.dial(budget, budget);
            last = begun;
            connected = connected || d.connected;
            if (d.ok()) {
                dial_ok = true;
                break;
            }
            if (d.answered()) {
                dial_refused_by_tv = true;
                break;
            }
            if (budget >= kMinTryMs) evidence = begun;   // a request with almost no time proves nothing
            if (final) break;
        }
        if (dial_ok || dial_refused_by_tv || connected) forget_wake(who);   // it is on the network
        if (!dial_ok) {
            if (dial_refused_by_tv || connected)
                err = kNotServing;
            else if (!origin)
                err = std::string(kNoAnswer) + " It may be off or unplugged.";
            else if (evidence && *evidence >= *origin + milliseconds(kDeepResumeMs))
                err = std::string(kNoAnswer) + " It did not come on after a wake-up signal.";
            else
                err = std::string(kWaking) + " A wake-up signal was sent, and it can take about 10 seconds to come on.";
            return false;
        }
        const Woken w = s.await_api(false, keep);
        if (w == Woken::Up) s.set_woken_up();
        return Session::woken_error(w, err);
    }

    // The wake memory: when the first magic packet of a still unanswered attempt to switch on a
    // device went out, so that a repeated on=true, or an on=false soon after, counts from that
    // packet. A small per-device cache (driver.h allows one), keyed by Device::id (the address
    // when there is none), kept for kWakeMemoryMs and dropped as soon as the TV answers anything.
    std::optional<Clock::time_point> remembered_wake(const std::string &who) {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        prune_wakes();
        const auto it = wake_started_.find(who);
        if (it == wake_started_.end()) return std::nullopt;
        return it->second;
    }

    Clock::time_point remember_wake(const std::string &who, Clock::time_point first) {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        prune_wakes();
        return wake_started_.emplace(who, first).first->second;   // an earlier start is kept
    }

    void forget_wake(const std::string &who) {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_started_.erase(who);
    }

    void prune_wakes() {   // with wake_mutex_ held
        const auto now = Clock::now();
        for (auto it = wake_started_.begin(); it != wake_started_.end();)
            it = now - it->second > std::chrono::milliseconds(kWakeMemoryMs) ? wake_started_.erase(it) : std::next(it);
    }

    // What a key is called in a message when the TV turns out not to offer it.
    static const char *key_name(const std::string &action) {
        static const std::map<std::string, const char *> names = {
            {"dpad_up", "the up key"},       {"dpad_down", "the down key"}, {"dpad_left", "the left key"},
            {"dpad_right", "the right key"}, {"select", "the OK key"},      {"back", "the back key"},
            {"home", "the home key"},        {"menu", "the menu key"},      {"sleep", "switching off"},
            {"volume_up", "volume up"},      {"volume_down", "volume down"}, {"mute", "the mute key"},
            {"power", "the power key"}};
        const auto it = names.find(action);
        return it == names.end() ? "that key" : it->second;
    }

    bool press(Session &s, const detail::Action &a, const std::string &who, std::string &err) {
        const bool switching_off = a.what == "sleep";
        const auto asked = Clock::now();
        const std::string path = "/v1/FireTV?action=" + a.what;   // a.what is one of plan()'s literals
        const What what{key_name(a.what), true, true};
        // Part of a run of presses got to the TV: not a reason to send them all again another
        // way. Worded without the words the app reads as "unreachable" (the TV was there).
        const auto partial = [&](int done, const char *why) {
            err = "Only " + std::to_string(done) + " of " + std::to_string(a.repeat) + " presses were confirmed by the TV. " + why;
            return false;
        };
        Clock::time_point last;
        for (int i = 0; i < a.repeat; i++) {
            if (i) {
                // Paced from the answer to the previous press, so presses arrive at the TV at least
                // kPressEveryMs apart however long a connection takes (firehose likewise measures its
                // key gap from the response).
                const long since = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last).count());
                if (since < kPressEveryMs) s.deadline().sleep(kPressEveryMs - since);
                if (s.deadline().left() < 200) return partial(i, "There was no time for the rest.");
            }
            // Body-less, one complete press: nothing can be left held down (see STATUS.md).
            if (!s.command([&] { return s.api(true, path, "", kJson, kSmallBody, s.deadline().left()); }, what, err)) {
                if (i) return partial(i, "Then it stopped taking them.");
                // Nothing answered on either port: the TV is most likely off already, and an
                // infrared power toggle would switch it on. Unless a wake-up signal from here went
                // out less than kDeepResumeMs before this was asked and nothing has answered since:
                // then the TV may be coming on right now, and "off already" would be wrong.
                if (switching_off && fault_of(err) == Fault::NoAnswer) {
                    const auto origin = remembered_wake(who);
                    if (origin && asked < *origin + std::chrono::milliseconds(kDeepResumeMs))
                        err = std::string(kWaking) + kNotSwitchedOff;
                    else
                        err = std::string(kAlreadyOff) + " It did not answer.";
                }
                return false;
            }
            last = Clock::now();
        }
        return true;
    }

    bool media(Session &s, const detail::Action &a, std::string &err) {
        const std::string path = "/v1/media?action=" + a.what;
        const char *type = a.body.empty() ? kJson : kJsonUtf8;
        const char *name = a.what == "play" ? "play and pause" : a.body.find("\"back\"") != std::string::npos ? "rewind" : "fast-forward";
        return s.command([&] { return s.api(true, path, a.body, type, kSmallBody, s.deadline().left()); },
                         What{name, true, true}, err);
    }

    bool launch(Session &s, const home::Thing &thing, const std::string &wanted, std::string &err) {
        std::vector<App> apps = apps_of(thing.ref);
        std::vector<std::string> candidates;
        int at = detail::match_app(apps, wanted, candidates);
        if (at < 0 && candidates.empty()) {   // perhaps installed since the list was read
            Reply list;
            if (!s.command([&] { return list = s.api(false, "/v1/FireTV/appsV2", "", kJson, kAppsBody, 2000); },
                           What{"opening apps", false, false}, err))
                return false;
            if (list.complete() && detail::parse_apps(list.body, apps)) at = detail::match_app(apps, wanted, candidates);
        }
        if (at < 0) {
            const std::string said = detail::clean_name(wanted);
            if (candidates.empty()) err = "No app called \"" + said + "\" is on the TV.";
            else err = "\"" + said + "\" could mean any of: " + detail::join(candidates) + ". Say which one.";
            return false;
        }
        const std::string path = "/v1/FireTV/app/" + apps[static_cast<size_t>(at)].id;   // checked by valid_package
        // A 400 here is not "unsupported": the TV may not have that package (any longer).
        return s.command([&] { return s.api(true, path, "", kJson, kSmallBody, s.deadline().left()); },
                         What{"opening apps", false, true}, err);
    }

    std::mutex wake_mutex_;
    std::map<std::string, Clock::time_point> wake_started_;
};

}  // namespace

namespace detail {
int api_port_of(const home::Device &dev) {
    Target t;
    std::string err;
    return target_of(dev, t, err) ? t.api_port : 0;
}
}  // namespace detail

}  // namespace firetv

std::unique_ptr<home::Driver> make_firetv_driver() { return std::make_unique<firetv::FireTvDriver>(); }
