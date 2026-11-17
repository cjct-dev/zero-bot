// HomeKit accessories over IP (the HomeKit Accessory Protocol, "HAP") as Zero Bot sees them: a
// controller of its own that pairs with an accessory or bridge using the accessory's 8-digit
// setup code, then switches lights and plugs, sets heating and reads sensors on the local network.
// No Apple account, no Apple device and no internet are involved.
//
// One short conversation per call: TCP to the accessory's port, pair-verify (a fresh X25519 key,
// both long-term Ed25519 keys checked), one or a few HTTP requests inside the encrypted session,
// close. No connection is kept, nothing is subscribed to, no thread is started.
//
// What is stored:
//   Secrets, family "homekit"   this controller's identity, created once at the first pairing and
//                               used for every accessory: {"controller_id": UUID string,
//                               "ltsk": hex Ed25519 private key (seed), "ltpk": hex public key}
//   Device::auth                {"accessory_id": the accessory's pairing id, "accessory_ltpk":
//                               hex Ed25519 public key, "controller_id": the id it was paired with}
// Nothing secret is ever put in an error string.
//
// kind() is "homekit". claims() takes devices of that kind except categories that can only be
// locks, garage doors, alarms, doors, windows, cameras, doorbells, sprinklers or taps, those with
// nothing on the allow-list (buttons, range extenders, routers, remotes, air purifiers,
// humidifiers, dehumidifiers) or a driver of their own (televisions, set-top boxes, streaming
// sticks, speakers, audio receivers), and Apple's own devices. Pairing is one Pairing::EnterCode
// step: the 8-digit code ("123-45-678", "12345678") or the text of the accessory's QR code
// ("X-HM://..."). A code the accessory refused is never sent to it again by this driver object,
// and nothing is ever retried automatically.
// An accessory that no longer knows this app (its HomeKit pairing was reset, or it was unpaired
// elsewhere: after proving its identity in pair-verify M2 with the saved key, it answered M4
// with an Authentication error) gets an EnterCode step again from next_step (for the life of the
// driver object), and pair() then makes a new pairing ("already paired with another controller"
// in the step once pair-setup said so). pair() on a device already paired with this app first
// asks the accessory (pair-verify): it does nothing more when the accessory still knows this app,
// and sends nothing more (no pair-setup, so no code) when pair-verify failed any other way: no
// answer, another identity, a bad signature, an unsigned refusal in M2, garbage.
// A reset to factory settings gives an accessory a new identity: pair-verify then says "A
// different HomeKit accessory answered at this address", next_step offers nothing, and the way on
// is a scan (which lists it as a new device), pairing that one, and removing the old one.
//
// Things (one per allowed service; id "aid.iid" of the service; room left empty, HAP has none):
//   light       Lightbulb        kPower, kBrightness, kColorTemp, kColor (as the bulb has them)
//   plug        Outlet           kPower
//   switch      Switch           kPower
//   thermostat  Thermostat       kTargetTemp (target and mode off / heat / auto), kSensor
//   thermostat  HeaterCooler     kPower, kTargetTemp (heating threshold and mode; only when the
//                                threshold is writable and the unit can heat: a cooling-only unit
//                                is kPower only), kSensor; ask_first is set
// (kSensor only when the current temperature can be read; each cap only when its
// characteristic is writable, so a light without colour has no kColor.)
//   cover       WindowCovering   kPosition
//   fan         Fan, Fan v2      kPower, kFanSpeed
//   sensor      Temperature, Humidity, Motion and Contact sensors     kSensor (read only)
// An accessory that also has a lock, lock management, garage door, security system, door, window,
// valve, tap, irrigation, camera, microphone, doorbell or access-control service is skipped
// entirely, and so is one with a service whose type is not a well-formed HAP type (not a string
// of 1-8 hex digits or a UUID: it cannot be checked) and one whose aid is listed more than once.
// Everything else (unknown or vendor services, televisions, smoke sensors...) is ignored.
//
// Thing::state after enumerate or read: only what the accessory reported then. A value that
// could not be read is left out, never kept from an earlier read; when nothing at all could be
// read, read() fails with the accessory's reason ("<name> is switched off, or out of range of its
// bridge."). The keys the app shows (src/smarthome/home.h):
//   on             bool
//   brightness     0..100
//   kelvin         colour temperature (from mireds)
//   hue            0..360          saturation   0..100
//   current_c      current temperature, degrees C (thermostats, heaters, temperature sensors)
//   target_c       heating target, degrees C
//   mode           "off", "heat" or "auto"; also "cool" when the device was set to cool elsewhere
//                  (shown, never set: Change::mode has no cool)
//   humidity       0..100 percent
//   position       0 closed .. 100 open (current)
//   fan            0..100 percent of the fan's speed range
//   motion         bool
//   contact        "open" or "closed" (contact sensors)
// and keys of this driver's own, which the app does not show:
//   in_use         bool (outlets that report it)
//   heating        bool, the thermostat or heater is heating right now
//   target_position  where a cover is heading
//   reachable      false when some value could not be read on the last read (its bridged
//                  accessory is switched off, say); see STATUS.md
// Readings no thermometer gives (outside -100..200 C) are left out.
//
// Change mapping: brightness 0 switches a light off; brightness, kelvin or hs given without `on`
// also switch the light on. `on` on a thermostat sets mode heat / off. Heating targets are clamped
// to 5..30 degrees C and to the thermostat's own range and step. Unsupported fields are refused
// with an error before anything is sent.
//
// Before every write, inside the verified session, the ids about to be written are read back
// with their types, with the accessory's model, serial number and firmware revision; when the
// firmware or discovery's c# changed, or the accessory gives no types, the whole database is read
// again. Anything that no longer matches what enumerate saw refuses the write ("... has changed on
// the accessory ... Refresh the device in settings."), so ids an accessory reused for something
// else (a lock, say) are never written. apply_many returns false with an empty err when its
// things fared differently, so that the app applies each one alone (see apply_many). A write
// answered 207 (Multi-Status) with no body, no list of items or a list about none of the written
// ones is "The accessory's reply was not understood.", never success.
//
// Depends on the C++ standard library, POSIX sockets and OpenSSL libcrypto (non-deprecated
// interfaces only), and nlohmann/json.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "smarthome/driver.h"

std::unique_ptr<home::Driver> make_homekit_driver();

namespace homekit {
namespace detail {
// The building blocks, exposed so that homekit_test.cpp can check each one against published
// vectors and homekit_cli.cpp can time pair-verify. Not part of the interface. Byte strings are
// std::string holding raw bytes.
using json = nlohmann::json;
using Bytes = std::string;

std::string hex(const Bytes &b);
bool unhex(const std::string &h, Bytes &out);

// ---- primitives (libcrypto)
Bytes sha512(const Bytes &b);
Bytes hmac_sha512(const Bytes &key, const Bytes &data);
Bytes hkdf_sha512(const Bytes &ikm, const Bytes &salt, const Bytes &info, size_t len);
// ChaCha20-Poly1305 (RFC 8439), 32-byte key, 12-byte nonce. `sealed` is ciphertext then 16-byte tag.
bool chacha20_poly1305_seal(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &plain,
                            Bytes &sealed);
bool chacha20_poly1305_open(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &sealed,
                            Bytes &plain);
Bytes label_nonce(const char *label8);   // 4 zero bytes, then an 8-character label such as "PS-Msg05"
Bytes counter_nonce(uint64_t n);         // 4 zero bytes, then n as 64-bit little-endian
bool x25519_public(const Bytes &priv, Bytes &pub);
bool x25519_shared(const Bytes &priv, const Bytes &peer_pub, Bytes &shared);   // refuses an all-zero result
bool ed25519_public(const Bytes &seed, Bytes &pub);
bool ed25519_sign(const Bytes &seed, const Bytes &msg, Bytes &sig);
bool ed25519_verify(const Bytes &pub, const Bytes &msg, const Bytes &sig);
bool random_bytes(size_t n, Bytes &out);

// ---- TLV8: type byte, length byte, value; longer values split into 255-byte fragments of the
// same type; distinct neighbours of one type are separated by an empty item of type 0xFF.
enum TlvType : uint8_t {
    kTlvMethod = 0, kTlvIdentifier = 1, kTlvSalt = 2, kTlvPublicKey = 3, kTlvProof = 4,
    kTlvEncryptedData = 5, kTlvState = 6, kTlvError = 7, kTlvRetryDelay = 8, kTlvCertificate = 9,
    kTlvSignature = 10, kTlvPermissions = 11, kTlvSeparator = 255,
};
struct Tlv {
    uint8_t type = 0;
    Bytes value;
};
Bytes tlv_encode(const std::vector<Tlv> &items);
bool tlv_decode(const Bytes &data, std::vector<Tlv> &items);   // false on a truncated item
const Bytes *tlv_find(const std::vector<Tlv> &items, uint8_t type);

// ---- SRP-6a as HAP uses it: RFC 5054 3072-bit group, g = 5, SHA-512, user "Pair-Setup",
// password the setup code with its dashes. A, B and S are left-padded to 384 bytes wherever they
// are hashed; B may arrive with its leading zero bytes stripped.
struct SrpProof {
    Bytes A;    // 384 bytes, sent in M3
    Bytes M1;   // 64 bytes, sent in M3
    Bytes K;    // session key, 64 bytes
    Bytes M2;   // what the accessory must answer in M4
};
// `a` is the client's secret exponent (big-endian); empty picks 32 random bytes.
bool srp_client(const std::string &user, const std::string &pass, const Bytes &salt, const Bytes &B,
                const Bytes &a, SrpProof &out);
Bytes srp_modulus();   // N, 384 bytes
Bytes srp_k();         // k = H(N | PAD(g)), 64 bytes

// ---- setup codes: "123-45-678", "12345678", "123 45 678" or "X-HM://" + 9 base-36 characters
// (+ an optional 4-character setup id). On success `code` is "XXX-XX-XXX".
bool parse_setup_code(const std::string &input, std::string &code, std::string &err);

// ---- the encrypted session: 2-byte little-endian length (also the associated data), at most
// 1024 bytes of ChaCha20-Poly1305 ciphertext, 16-byte tag; nonce from a per-direction counter.
struct Cipher {
    Bytes key;
    uint64_t counter = 0;
};
Bytes seal_frames(Cipher &c, const Bytes &plain);
// Decrypts every complete frame of `in` from `pos` on, appending to `out` and advancing `pos`.
// Returns false on a frame longer than 1024 bytes or one that fails authentication.
bool open_frames(Cipher &c, const Bytes &in, size_t &pos, Bytes &out);

// ---- HTTP/1.1 responses (and EVENT/1.0 messages), read incrementally.
struct HttpResponse {
    bool event = false;   // an EVENT/1.0 notification rather than a response
    int status = 0;
    std::map<std::string, std::string> headers;   // names lower-cased
    Bytes body;
};
class HttpReader {
public:
    explicit HttpReader(size_t max_body) : max_body_(max_body) {}
    // Consumes bytes of `buf` from `pos`. Returns 1 when `msg` is complete, 0 when more bytes are
    // needed, -1 when the message is malformed or larger than allowed.
    int feed(const Bytes &buf, size_t &pos);
    HttpResponse msg;

private:
    enum class St { kStatus, kHeader, kBody, kChunkSize, kChunkData, kChunkEnd, kTrailer, kDone };
    int line(const Bytes &buf, size_t &pos, std::string &out);
    St st_ = St::kStatus;
    size_t max_body_, line_bytes_ = 0, want_ = 0;
    int headers_ = 0;
    bool chunked_ = false;
};

// ---- text from accessories
Bytes sanitize_utf8(const Bytes &in);           // invalid sequences become U+FFFD
// Valid UTF-8, no control, format or invisible characters, Unicode spaces collapsed and trimmed,
// at most 40 characters. May be empty: callers fall back to a name of their own.
std::string clean_name(const std::string &raw);
bool json_depth_ok(const Bytes &text, int max_depth);
// False when the text has more than max_values '[', '{', ',' and ':' outside strings (what a
// JSON tree's size grows with).
bool json_values_ok(const Bytes &text, size_t max_values);
// Inside JSON strings, every \uD800-\uDBFF escape not followed by a \uDC00-\uDFFF escape, and every
// lone \uDC00-\uDFFF escape, becomes � (the JSON parser would refuse the whole text).
Bytes repair_surrogates(const Bytes &text);
// A JSON reply from an accessory: invalid UTF-8 replaced, lone surrogate escapes repaired,
// nesting and size checked before parsing.
bool parse_reply_json(const Bytes &body, size_t max_values, json &out);
// GET /accessories as enumerate reads it: at most 1 MiB and 150,000 values.
bool parse_database(const Bytes &body, json &out);

// ---- the accessory database
std::string short_type(const std::string &uuid);   // "43", "0000004A-0000-1000-8000-0026BB765291" -> "4A"
// Turns the JSON of GET /accessories into things (allow-list only; see the top of this file).
// Thing::ref: {"aid", "svc": the service's iid, "k": its kind, "c": {key: {"i": iid, "t": short
// type, "f": format, "r", "w", "min", "max", "step", "vv"}}, "ais": {"21" model, "30" serial
// number, "52" firmware: [iid, fingerprint]}, and from enumerate "cn": discovery's c#}.
bool things_from_database(const json &db, std::vector<home::Thing> &out);
// One characteristic write.
struct Write {
    int64_t aid = 0, iid = 0;
    json value;
};
// The writes a change needs on one thing, every value in the characteristic's format, clamped to
// its range and rounded to its step. False with a sentence in `err` when the thing cannot do it.
bool writes_for(const home::Thing &thing, const home::Change &change, std::vector<Write> &out, std::string &err);
// Makes the thing's state from values read back ({(aid, iid): value}). `failed` lists the
// characteristics the accessory reported an error for: their values are left out (not kept from
// before) and the state gets "reachable": false.
void apply_values(home::Thing &thing, const std::map<std::pair<int64_t, int64_t>, json> &values,
                  const std::vector<std::pair<int64_t, int64_t>> &failed);
// Words for a HAP status code such as -70402, about the named thing.
std::string status_text(long status, const std::string &name);

// Every error sentence for a problem on this side or from an accessory that did answer (all but
// "could not be reached", "did not answer in time", "closed the connection" and "a different
// accessory answered", which may mean it moved). None may read as "it did not answer" to the app.
std::vector<std::string> answered_error_texts();

// Connects to the paired accessory and runs pair-verify only, `runs` times, filling `ms` with the
// time each took. For measuring; homekit_cli's "time-verify" uses it.
bool time_pair_verify(const home::Device &dev, home::Secrets *secrets, int runs, std::vector<double> &ms,
                      std::string &err);
}  // namespace detail
}  // namespace homekit
