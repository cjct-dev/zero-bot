// Helpers the device drivers share (ZB-038). Each of these was written more than once, in drivers
// that all read untrusted answers from devices on the home network. Only copies that behaved the
// same were merged; where one driver's rule differs from another's (a limit, a replacement
// character), the rule is a parameter here, and where more than that differs (which characters a
// device's name may keep, how a wait ends at its deadline) the driver keeps its own, saying why.
//
// Not part of the driver contract (driver.h): a driver may use any of these or none.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include "core/util.h"
#include "nlohmann/json.hpp"
#include "smarthome/driver.h"

namespace support {
using json = nlohmann::json;

// ---------------------------------------------------------------- JSON from devices
// Whether a JSON text nests arrays and objects deeper than `max`: util::json_too_deep (core/util.h),
// the one scanner for everything that came from another machine. Checked before parsing anything a
// device sent, so that a hostile answer cannot make copying the tree recurse without end. Each
// driver keeps its own limit (Hue 48, the others 32, util::kMaxJsonDepth); util::parse_untrusted_json
// checks and parses in one.
using util::json_too_deep;
// A reply parsed without throwing: false, with `out` untouched, when it is longer than `max_bytes`
// or nests deeper than `max_depth`; otherwise `out` is the parse, and false when it is not JSON.
bool parse_reply(const std::string &body, size_t max_bytes, int max_depth, json &out);

// ---------------------------------------------------------------- text from devices
// What a byte that does not begin a well-formed UTF-8 sequence becomes.
enum class BadUtf8 {
    QuestionMark,           // '?' (Hue)
    ReplacementCharacter,   // U+FFFD (Fire TV, HomeKit)
};
// The text with every byte that does not begin a well-formed sequence (RFC 3629: no overlong
// forms, no surrogates, nothing above U+10FFFF) replaced; a bad sequence costs one replacement per
// byte, as the next byte is looked at afresh.
std::string sanitize_utf8(const std::string &raw, BadUtf8 bad);
// The well-formed UTF-8 sequence at p[i] (n bytes in all): its length, with its code point in `cp`;
// 0 when there is none there.
size_t utf8_at(const unsigned char *p, size_t n, size_t i, uint32_t &cp);
// Unicode's White_Space property (PropList.txt): tab, the line breaks, space, NEL, the no-break and
// typographic spaces, the line and paragraph separators, the ideographic space.
bool unicode_space(uint32_t cp);
// The longest start of `text` of at most `max` bytes that does not end inside a UTF-8 sequence.
std::string utf8_prefix(const std::string &text, size_t max);

// ---------------------------------------------------------------- MAC addresses
// The twelve hex digits of a MAC address written with ':', '-' or nothing between them, lower
// case; "" for anything else, and for twelve zeros, which name no device: the one check of a MAC for
// what is saved, compared and controlled (a zero MAC saved over a real one let another device take
// over its record). What counts as the same device (a side with no MAC, a reply with an unreadable
// one) differs between drivers and stays beside each comparison.
std::string mac_digits(const std::string &raw);
// The MAC a device was saved under, as mac_digits gives it: the record's own, else the one in its
// id, for a family whose ids are made from the MAC ("wiz:<MAC>", "tplink:<MAC>"; an earlier version
// saved some records with the MAC in the id alone). "" when neither holds one.
std::string saved_mac(const home::Device &dev, const std::string &id_prefix);

// ---------------------------------------------------------------- bytes
int hex_digit(char c);   // 0 to 15, -1 for a character that is not a hex digit
std::string hex(const std::string &bytes);   // lower case
// Raw bytes from hex digits (either case). False, with `out` untouched, for an odd length or a
// character that is not a hex digit.
bool unhex(const std::string &hex, std::string &out);
// A message digest (EVP_sha256() and the like), raw; "" when libcrypto fails.
std::string digest(const EVP_MD *md, const void *data, size_t len);
std::string digest(const EVP_MD *md, const std::string &data);
// HMAC with the named digest ("SHA256", "SHA512"), raw; "" when libcrypto fails. An empty key is
// passed to libcrypto as it is (HomeKit, whose peers use one, gives one zero byte instead).
std::string hmac(const char *digest_name, const std::string &key, const std::string &data);

// ---------------------------------------------------------------- certificates
struct X509Free {
    void operator()(X509 *x) const { X509_free(x); }
};
using Certificate = std::unique_ptr<X509, X509Free>;
// The first certificate in PEM text; null when there is none, or the text is empty or over 64 KiB.
Certificate certificate_from_pem(const std::string &pem);
// "sha256//" and the base64 SHA-256 of a DER SubjectPublicKeyInfo, as libcurl's pinned public key
// option takes it; "" when the digest fails. How the DER is got from the certificate stays with
// each driver (Hue encodes the certificate's own X509_PUBKEY, as libcurl does; Fire TV re-encodes
// the decoded key).
std::string sha256_pin(const unsigned char *der, size_t len);

// ---------------------------------------------------------------- sockets
// A socket descriptor closed when it goes out of scope. The waits stay in each driver: Kasa's looks
// once more when the deadline has come, HomeKit's does not and tells an error from a timeout, and
// WiZ's runs a schedule of resends.
struct Socket {
    int fd = -1;
    Socket() = default;
    explicit Socket(int descriptor) : fd(descriptor) {}
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    ~Socket();
};

}  // namespace support
