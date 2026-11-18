// Local control of TP-Link Tapo smart plugs: no cloud, only the plug's own HTTP server.
//
// Two logins are spoken, chosen per device from what its discovery reply announces:
//   KLAP v2  seeds + SHA-256 of the account, then AES-128-CBC per request      (P304M strip)
//   TPAP     SPAKE2+ (RFC 9383, P-256/SHA-256), then AES-128-CCM per request   (P110, fw 1.4.x)
// Only the TPAP variant these plugs announce is supported: plain HTTP (tls 0), account password
// (pake [2]), cipher suite 1, aes_128_ccm, password_shadow id 2. Anything else is refused.
//
// A plug counts rejected logins and locks itself after too many, so this module never retries
// one: a rejection is remembered for that plug and credential until forget_sessions().
//
// Depends on the C++ standard library, libcurl, OpenSSL libcrypto and nlohmann/json.
// The application should have called curl_global_init() before the first request.
#pragma once
#include <cstdint>
#include <string>

#include "nlohmann/json.hpp"

namespace tapo {
using json = nlohmann::json;

struct Credentials {
    std::string email;              // Tapo account e-mail, exactly as registered (KLAP hashes it; TPAP only checks ownership with it)
    std::string password_sha1_hex;  // hex SHA-1 of the account password; the password itself is never stored
};

struct Device {
    std::string ip;
    int port = 80;          // discovery's mgt_encrypt_schm.http_port. For TPAP, tpap.port is used instead while this is left at 80.
    std::string scheme;     // "KLAP" or "TPAP", from discovery's mgt_encrypt_schm.encrypt_type
    json tpap;              // discovery's "tpap" object when present. If null on a TPAP device, the plug is asked for it (not a login).
    std::string owner;      // discovery's "owner" field (MD5 of the account e-mail), may be empty
    // The MAC the plug was saved under (any notation), "" for none. An address is not an identity:
    // after a router hands this plug's address to another plug of the same account, that plug
    // accepts the same login. So each new session is asked get_device_info first, and a plug that
    // names another MAC gets nothing else (request() fails, saying another plug answered).
    std::string mac;
};

// Lower-case hex SHA-1, for turning a typed password into the stored form.
std::string sha1_hex(const std::string &text);

// Whether the plug's owner field matches this e-mail (as typed, or lower-cased). True when the
// plug announced no owner.
bool owned_by(const Device &dev, const std::string &email);

// Sends one request such as {"method":"get_device_info"} and fills `reply` with the device's
// "result" object ({} when it sent none). Logs in when there is no session, and logs in again
// once if the plug no longer knows the session; at most one login is made per call. Returns
// false with a human-readable `err` otherwise. No login is attempted when owned_by() is false,
// or after the plug rejected these credentials, or while it reports itself locked.
// With Device::mac set, a session is used only once the plug has named that MAC (one extra
// request per login).
// Thread-safe; requests to one plug (one ip and port) are serialised and share one session.
// Each HTTP exchange is limited to 3 s to connect and 6 s in total; a call that has to log in
// first makes three of them (four when it must also ask a TPAP plug how to log in). A refused
// connection is tried once more, 300 ms after a discovery query to the plug's UDP port 20002.
bool request(const Device &dev, const Credentials &cred, const json &req, json &reply, std::string &err);

// Whether this plug has rejected these credentials, or locked itself, since forget_sessions().
// The caller can keep that beyond the life of the program, so that it is never tried again.
bool login_refused(const Device &dev, const Credentials &cred);

// Convenience wrappers built on request().

// Fills {"name": the plug's name, "on": true/false, "model", "fw", "sockets": number of child
// sockets (0 for a plain plug)}, followed by everything else get_device_info returned ("nickname"
// and "ssid" decoded from base64). A strip has no switch of its own: "on" is true when any of its
// sockets is, which costs a second request.
bool get_info(const Device &dev, const Credentials &cred, json &info, std::string &err);

bool set_power(const Device &dev, const Credentials &cred, bool on, std::string &err);

// P110. Fills {"power_w": power now in watts, "today_wh": today's energy in watt-hours}, leaving
// out what the plug does not report, plus "month_wh" when known and "raw" (the plug's answers).
bool get_energy(const Device &dev, const Credentials &cred, json &energy, std::string &err);

// P304M. Fills an array of {"id": child device id for set_socket_power(), "name", "on", "position"}.
bool list_sockets(const Device &dev, const Credentials &cred, json &sockets, std::string &err);

bool set_socket_power(const Device &dev, const Credentials &cred, const std::string &child_id, bool on,
                      std::string &err);

// Drops every cached session and every remembered rejection or lock, e.g. after the credentials
// were changed.
void forget_sessions();

// The protocol's building blocks, exposed so that tapo_test.cpp can check each against published
// vectors. Not part of the interface. Byte strings are std::string holding raw bytes.
namespace detail {
using Bytes = std::string;

std::string hex(const Bytes &b);
bool unhex(const std::string &h, Bytes &out);
std::string b64(const Bytes &b);
bool unb64(const std::string &s, Bytes &out);

Bytes md5(const Bytes &b);
Bytes sha1(const Bytes &b);
Bytes sha256(const Bytes &b);
Bytes hmac_sha256(const Bytes &key, const Bytes &data);
Bytes hkdf_sha256(const Bytes &ikm, const Bytes &salt, const Bytes &info, size_t len);
Bytes pbkdf2_sha256(const Bytes &pass, const Bytes &salt, int iterations, size_t len);
bool aes128_cbc(bool encrypt, const Bytes &key, const Bytes &iv, const Bytes &in, Bytes &out);
// CCM with a 12-byte nonce and 16-byte tag as TPAP uses it; other sizes are for the RFC 3610 vectors.
// The tag follows the ciphertext.
bool aes128_ccm_seal(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &plain, size_t tag_len, Bytes &out);
bool aes128_ccm_open(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &sealed, size_t tag_len, Bytes &plain);

// KLAP v2.
struct KlapKeys {
    Bytes key, iv, sig;    // 16, 12 and 28 bytes
    int32_t seq = 0;       // the next request is sent under seq + 1
};
Bytes klap_auth_hash(const std::string &email, const Bytes &password_sha1);
Bytes klap_handshake1_hash(const Bytes &local_seed, const Bytes &remote_seed, const Bytes &auth_hash);
Bytes klap_handshake2_hash(const Bytes &local_seed, const Bytes &remote_seed, const Bytes &auth_hash);
KlapKeys klap_derive(const Bytes &local_seed, const Bytes &remote_seed, const Bytes &auth_hash);
Bytes klap_seal(const KlapKeys &k, int32_t seq, const std::string &plain);
bool klap_open(const KlapKeys &k, int32_t seq, const Bytes &body, std::string &plain);

// TPAP.
struct SpakeProof {
    Bytes share;                  // the prover's share, 65 bytes uncompressed
    Bytes confirm;                // proves knowledge of the password to the verifier
    Bytes shared_key;             // what the session keys are derived from
    Bytes verifier_confirm_key;   // checks the verifier's confirmation
};
// w0 and w1 (32 bytes each) from the credential string the device asks for.
bool spake2p_w0_w1(const std::string &credential, const Bytes &salt, int iterations, Bytes &w0, Bytes &w1);
// The prover of RFC 9383 on P-256. `x` is the prover's random scalar (32 bytes); empty picks one.
bool spake2p_prove(const Bytes &w0, const Bytes &w1, const Bytes &x, const Bytes &context,
                   const Bytes &id_prover, const Bytes &id_verifier, const Bytes &verifier_share,
                   SpakeProof &out, std::string &err);
bool spake2p_verify(const SpakeProof &proof, const Bytes &verifier_confirm);
Bytes tpap_context(const Bytes &user_random, const Bytes &dev_random);
struct TpapKeys {
    Bytes key, nonce;      // 16 and 12 bytes
};
TpapKeys tpap_derive(const Bytes &shared_key);
Bytes tpap_seal(const TpapKeys &k, int32_t seq, const std::string &plain);
bool tpap_open(const TpapKeys &k, int32_t seq, const Bytes &body, std::string &plain);

// The 16-byte discovery query (UDP 20002) that carries no key.
extern const unsigned char kDiscoveryQuery[16];
// Plain words for a device error code.
std::string describe_error(int code);
}  // namespace detail

}  // namespace tapo
