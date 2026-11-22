// HomeKit Accessory Protocol (HAP) controller over IP. See homekit.h for what it does and stores.
//
// Licences. This file is a C++ re-write (over libcrypto), not a copy, of parts of two projects
// under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0); their
// notices are kept here as section 4 of that licence asks:
//   aiohomekit:  "Copyright 2019 aiohomekit team. Licensed under the Apache License, Version 2.0"
//   HomeKit ADK: "Copyright (c) 2015-2019 The HomeKit ADK Contributors. Licensed under the Apache
//                License, Version 2.0"
// Neither project has a NOTICE file. HAP-NodeJS (Apache-2.0) was only read, to check behaviour.
// Distributing the app with this file means shipping the Apache-2.0 text and crediting both
// projects: licenses/THIRD-PARTY-NOTICES.md and licenses/Apache-2.0.txt, which the package installs.
//
// Ported from (all Apache-2.0), re-written in C++ over libcrypto:
//   Jc2k/aiohomekit       protocol/__init__.py: the pair-setup M1..M6 and pair-verify M1..M4
//                         messages, the HKDF salts and infos, the "PS-Msg05" style nonces;
//                         crypto/srp.py: the SRP client (A, B and S padded to 384 bytes when hashed,
//                         k = H(N | PAD(g)), M1 = H(H(N) xor H(g) | H(user) | salt | A | B | K) with
//                         H(g) over the single byte 0x05, salt padded to 16 bytes, B accepted with
//                         its leading zeros stripped); protocol/tlv.py: TLV8 fragments and
//                         separators; controller/ip/connection.py: session frames (1024-byte
//                         blocks, length as associated data, nonce = 4 zero bytes + 64-bit
//                         little-endian counter per direction), the bare "Host: <address>" header,
//                         one write per request; http/response.py: chunked bodies, EVENT/1.0;
//                         controller/ip/pairing.py: /characteristics and Remove Pairing.
//   apple/HomeKitADK      HAP/HAPPairingPairSetup.c (the accessory strips leading zeros from B;
//                         which error each step can answer; Method 1 needs the authentication
//                         chip), HAP/HAPPairing.h (RetryDelay: seconds, little-endian integer),
//                         HAP/HAPAccessorySetup.c (layout of the X-HM:// setup payload),
//                         HAP/HAPIPAccessoryProtocol.c (GET /characteristics takes type=1).
//   homebridge/HAP-NodeJS src/lib/HAPServer.ts, src/lib/util/hapCrypto.ts: the accessory side, read
//                         to check item order, status codes and framing.
// Service and characteristic numbers were checked against aiohomekit's model/*/types.py.
#include "homekit.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <set>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "smarthome/drivers/support.h"

namespace homekit {

namespace {
using detail::Bytes;
using detail::json;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// Limits and deadlines
// ---------------------------------------------------------------------------------------------
constexpr int kConnectMs = 2000;          // a TCP connect that takes longer than this will not succeed
constexpr int kPairMs = 30000;            // pair-setup: slow accessories spend seconds on SRP
constexpr int kEnumerateMs = 4000;
constexpr int kReadMs = 4000;
constexpr int kApplyMs = 2500;
constexpr int kMaxApplyMs = 10000;
constexpr int kUnpairMs = 8000;
constexpr size_t kMaxTlvBody = 64 * 1024;
// A bridge of 400 accessories (the most that become things) is about 550 KB of JSON with about
// 80,000 values; a JSON tree costs about 12 times its text. The value caps bound what a hostile
// reply under the byte cap can cost (a body of nothing but "{}," would otherwise cost 29 times).
constexpr size_t kMaxDatabase = 1024 * 1024;
constexpr size_t kMaxDatabaseValues = 150000;
constexpr size_t kMaxCharBody = 256 * 1024;
constexpr size_t kMaxCharValues = 50000;
constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxLine = 8 * 1024;
constexpr int kMaxHeaders = 100;
constexpr int kMaxChunks = 100000;
constexpr int kMaxEvents = 16;            // EVENT/1.0 messages skipped while waiting for a response
constexpr int kMaxJsonDepth = 32;
constexpr size_t kMaxAccessories = 400;
constexpr size_t kMaxThings = 400;
constexpr size_t kIdsPerRead = 48;        // characteristics per GET, to keep each request small
constexpr double kMinTargetC = 5.0, kMaxTargetC = 30.0;

// ---------------------------------------------------------------------------------------------
// Words shown to the user. README-snippet.md quotes the pairing ones; keep them in step.
// ---------------------------------------------------------------------------------------------
const char *const kPromptCode =
    "Type the accessory's HomeKit setup code: the 8 digits on its label, its box or in its maker's app, "
    "like 123-45-678. If there is only a QR code, scan it with a phone and type the text that starts with X-HM://.";
const char *const kPromptPairedElsewhere =
    "This accessory is already paired with another controller, such as Apple Home. Remove it there first, "
    "scan again, then type its setup code here.";
const char *const kPromptKeyChanged =
    "This accessory was paired with a key this app no longer has. Reset the accessory's HomeKit pairing "
    "(see its maker's instructions), scan again, then type its setup code.";
// "Reset" here is a reset of the accessory's HomeKit pairings, which keeps its identity. A reset
// to factory settings gives it a new identity (HomeKit ADK HAPRestoreFactorySettings purges the
// configuration domain, so HAPDeviceIDGet and the server make a new Device ID and key): that is
// kErrWrongAccessory, and a scan lists it as a new device.
const char *const kPromptNotKnown =
    "This accessory no longer recognises this app (its HomeKit pairing may have been reset, or it was unpaired "
    "elsewhere). Type its setup code to pair it again.";
const char *const kField = "Setup code";

const char *const kErrNotCode =
    "That is not a setup code. Type the 8 digits from the accessory's label, like 123-45-678, or the text "
    "from its QR code that starts with X-HM://.";
const char *const kErrQrNoCode =
    "That QR code text has no setup code in it. Type the 8 digits from the accessory's label instead.";
const char *const kErrPairedElsewhere =
    "This accessory is already paired with another controller, such as Apple Home. Remove it there first, "
    "then scan again.";
const char *const kErrWrongCode =
    "The accessory did not accept that setup code. Check the code and type it again: it is not retried "
    "automatically, because accessories stop pairing after too many wrong codes.";
const char *const kErrRefusedBefore =
    "The accessory already refused that setup code, so it is not sent again. Check the code on the label "
    "and type the correct one.";
const char *const kErrLocked =
    "The accessory has stopped pairing after too many wrong codes. Its HomeKit pairing must be reset (see "
    "its maker's instructions) before it can be paired.";
const char *const kErrBusy = "The accessory is busy pairing with something else. Try again in a minute.";
const char *const kErrMaxPeers =
    "The accessory cannot take another controller. Remove one in its maker's app or in Apple Home first.";
const char *const kErrPairFailed = "The accessory refused to pair.";
const char *const kErrFakeAccessory =
    "The accessory could not prove that it knows the setup code, so pairing was stopped.";
const char *const kErrBadIdentity = "The accessory's identity could not be checked, so pairing was stopped.";
const char *const kErrNoStore = "This app could not save its HomeKit key, so pairing was not started.";
// No sentence for a problem on this side, or from an accessory that did answer, may contain a word
// of the app's "it did not answer" list (src/smarthome/control.cpp unreachable(): connect, reach,
// answer, respond, timed out, timeout, no route, unreachable): the app rescans the network for
// those. Checked by the unit test over answered_error_texts(). kErrUnreachable, kErrTimeout,
// kErrDropped and kErrWrongAccessory use them on purpose: the accessory may have moved.
const char *const kErrBadStore =
    "The HomeKit key saved by this app is damaged, so this app cannot use its HomeKit accessories.";
const char *const kErrNotPaired = "This accessory is not paired yet. Pair it in settings with its setup code.";
const char *const kErrKeyChanged =
    "This accessory was paired with a key this app no longer has. Reset its HomeKit pairing and pair it again.";
const char *const kErrNoAddress = "No address is known for this accessory. Scan the network again.";
const char *const kErrUnreachable =
    "The accessory could not be reached. Check that it is switched on and on this network.";
const char *const kErrTimeout = "The accessory did not answer in time.";
const char *const kErrDropped = "The accessory closed the connection.";
const char *const kErrGarbled = "The accessory's reply was not understood.";
const char *const kErrDecrypt = "The accessory's reply could not be decrypted.";
const char *const kErrNotKnown =
    "The accessory no longer recognises this app (its HomeKit pairing may have been reset, or it was unpaired "
    "elsewhere). Type its setup code in settings to pair it again.";
const char *const kErrWrongAccessory =
    "A different HomeKit accessory answered at this address. Scan the network again. If this accessory was reset "
    "to factory settings, it now has a new HomeKit identity: pair the one the scan finds, and remove this one.";
const char *const kErrVerify = "The accessory's identity check failed, so nothing was sent to it.";
const char *const kErrRefusedSession = "The accessory turned down the session. Try again in a moment.";
const char *const kErrInternal = "Something went wrong talking to the accessory.";
const char *const kErrNothing = "Nothing to change.";

// ---------------------------------------------------------------------------------------------
// Helpers this driver alone needs. Hex, digests, HMAC and UTF-8 decoding come from
// drivers/support.h (ZB-038).
// ---------------------------------------------------------------------------------------------
const unsigned char *u8(const Bytes &b) { return reinterpret_cast<const unsigned char *>(b.data()); }
unsigned char *u8(Bytes &b) { return reinterpret_cast<unsigned char *>(&b[0]); }

template <typename T, void (*F)(T *)>
struct Del {
    void operator()(T *p) const { F(p); }
};
using Pkey = std::unique_ptr<EVP_PKEY, Del<EVP_PKEY, EVP_PKEY_free>>;
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, Del<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, Del<EVP_MD_CTX, EVP_MD_CTX_free>>;
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, Del<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>>;
using Bn = std::unique_ptr<BIGNUM, Del<BIGNUM, BN_clear_free>>;
using BnCtx = std::unique_ptr<BN_CTX, Del<BN_CTX, BN_CTX_free>>;

struct Wipe {   // clears a secret when it goes out of scope
    Bytes &b;
    ~Wipe() {
        if (!b.empty()) OPENSSL_cleanse(&b[0], b.size());
    }
};

int remaining_ms(Clock::time_point deadline) {
    const auto left = deadline - Clock::now();
    if (left <= Clock::duration::zero()) return 0;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
    return static_cast<int>(std::min<long long>(std::max<long long>(ms, 1), INT_MAX));
}

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

Bytes one(uint8_t v) { return Bytes(1, static_cast<char>(v)); }

// ---- careful JSON access: accessories send whatever they like
const json *member(const json &j, const char *key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}
const json *array_of(const json &j, const char *key) {
    const json *m = member(j, key);
    return m && m->is_array() ? m : nullptr;
}
std::string string_of(const json &j, const char *key) {
    const json *m = member(j, key);
    return m && m->is_string() ? m->get<std::string>() : std::string();
}
bool number_of(const json &v, double &out) {
    if (v.is_boolean()) {
        out = v.get<bool>() ? 1 : 0;
        return true;
    }
    if (!v.is_number()) return false;
    out = v.get<double>();
    return std::isfinite(out);
}
// A HAP instance id: a positive integer (some accessories send 3.0).
bool id_of(const json &j, const char *key, int64_t &out) {
    const json *m = member(j, key);
    if (!m) return false;
    if (m->is_number_integer()) {
        out = m->get<int64_t>();
    } else if (m->is_number_float()) {
        const double d = m->get<double>();
        if (!std::isfinite(d) || d != std::floor(d) || d < 1 || d > 9007199254740992.0) return false;
        out = static_cast<int64_t>(d);
    } else {
        return false;
    }
    return out >= 1 && out <= 9007199254740992LL;
}
}  // namespace

namespace detail {

std::string hex(const Bytes &b) { return support::hex(b); }

bool unhex(const std::string &h, Bytes &out) { return support::unhex(h, out); }

// ---------------------------------------------------------------------------------------------
// Primitives over libcrypto
// ---------------------------------------------------------------------------------------------
Bytes sha512(const Bytes &b) {
    const Bytes d = support::digest(EVP_sha512(), b);
    return d.size() == 64 ? d : Bytes();
}

Bytes hmac_sha512(const Bytes &key, const Bytes &data) {
    // HMAC pads its key with zeros to the block size, so an empty key is the same as one zero byte.
    const Bytes mac = support::hmac("SHA512", key.empty() ? Bytes(1, '\0') : key, data);
    return mac.size() == 64 ? mac : Bytes();
}

// RFC 5869 with SHA-512.
Bytes hkdf_sha512(const Bytes &ikm, const Bytes &salt, const Bytes &info, size_t len) {
    if (len == 0 || len > 255 * 64) return Bytes();
    Bytes prk = hmac_sha512(salt, ikm);
    Wipe w{prk};
    if (prk.size() != 64) return Bytes();
    Bytes okm, t;
    for (unsigned i = 1; okm.size() < len; i++) {
        t = hmac_sha512(prk, t + info + static_cast<char>(i));
        if (t.size() != 64) return Bytes();
        okm += t;
    }
    okm.resize(len);
    return okm;
}

namespace {
bool aead(bool seal, const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &in, Bytes &out) {
    if (key.size() != 32 || nonce.size() != 12) return false;
    if (!seal && in.size() < 16) return false;
    const size_t text = seal ? in.size() : in.size() - 16;
    if (text > static_cast<size_t>(INT_MAX) - 64 || aad.size() > static_cast<size_t>(INT_MAX)) return false;
    CipherCtx c(EVP_CIPHER_CTX_new());
    if (!c) return false;
    Bytes buf(text + 16, '\0');
    int n = 0, m = 0;
    if (!EVP_CipherInit_ex(c.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr, seal ? 1 : 0) ||
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) <= 0 ||
        !EVP_CipherInit_ex(c.get(), nullptr, nullptr, u8(key), u8(nonce), seal ? 1 : 0))
        return false;
    if (!aad.empty() && !EVP_CipherUpdate(c.get(), nullptr, &n, u8(aad), static_cast<int>(aad.size()))) return false;
    n = 0;
    if (text && !EVP_CipherUpdate(c.get(), u8(buf), &n, u8(in), static_cast<int>(text))) return false;
    if (!seal) {
        Bytes tag = in.substr(text);
        if (EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_AEAD_SET_TAG, 16, u8(tag)) <= 0) return false;
    }
    if (!EVP_CipherFinal_ex(c.get(), u8(buf) + n, &m)) {   // a wrong tag fails here
        OPENSSL_cleanse(u8(buf), buf.size());
        return false;
    }
    if (seal && EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_AEAD_GET_TAG, 16, u8(buf) + text) <= 0) return false;
    buf.resize(seal ? text + 16 : text);
    out.swap(buf);
    return true;
}

Pkey raw_key(int type, bool priv, const Bytes &b) {
    if (b.size() != 32) return Pkey();
    return Pkey(priv ? EVP_PKEY_new_raw_private_key(type, nullptr, u8(b), 32)
                     : EVP_PKEY_new_raw_public_key(type, nullptr, u8(b), 32));
}

bool public_of(int type, const Bytes &priv, Bytes &pub) {
    Pkey k = raw_key(type, true, priv);
    unsigned char out[32];
    size_t n = sizeof out;
    if (!k || EVP_PKEY_get_raw_public_key(k.get(), out, &n) != 1 || n != 32) return false;
    pub.assign(reinterpret_cast<char *>(out), 32);
    return true;
}
}  // namespace

bool chacha20_poly1305_seal(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &plain, Bytes &sealed) {
    return aead(true, key, nonce, aad, plain, sealed);
}

bool chacha20_poly1305_open(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &sealed, Bytes &plain) {
    return aead(false, key, nonce, aad, sealed, plain);
}

Bytes label_nonce(const char *label8) {
    Bytes n(4, '\0');
    n += Bytes(label8, strnlen(label8, 8));
    n.resize(12, '\0');
    return n;
}

Bytes counter_nonce(uint64_t c) {
    Bytes n(12, '\0');
    for (int i = 0; i < 8; i++) n[4 + i] = static_cast<char>((c >> (8 * i)) & 0xff);
    return n;
}

bool x25519_public(const Bytes &priv, Bytes &pub) { return public_of(EVP_PKEY_X25519, priv, pub); }

bool x25519_shared(const Bytes &priv, const Bytes &peer_pub, Bytes &shared) {
    Pkey k = raw_key(EVP_PKEY_X25519, true, priv), p = raw_key(EVP_PKEY_X25519, false, peer_pub);
    if (!k || !p) return false;
    PkeyCtx ctx(EVP_PKEY_CTX_new_from_pkey(nullptr, k.get(), nullptr));
    unsigned char out[32];
    size_t n = sizeof out;
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) != 1 || EVP_PKEY_derive_set_peer(ctx.get(), p.get()) != 1 ||
        EVP_PKEY_derive(ctx.get(), out, &n) != 1 || n != 32)
        return false;
    unsigned char any = 0;
    for (const unsigned char c : out) any |= c;
    if (!any) return false;   // the peer sent a point of small order
    shared.assign(reinterpret_cast<char *>(out), 32);
    OPENSSL_cleanse(out, sizeof out);
    return true;
}

bool ed25519_public(const Bytes &seed, Bytes &pub) { return public_of(EVP_PKEY_ED25519, seed, pub); }

bool ed25519_sign(const Bytes &seed, const Bytes &msg, Bytes &sig) {
    Pkey k = raw_key(EVP_PKEY_ED25519, true, seed);
    MdCtx md(EVP_MD_CTX_new());
    unsigned char out[64];
    size_t n = sizeof out;
    if (!k || !md || EVP_DigestSignInit(md.get(), nullptr, nullptr, nullptr, k.get()) != 1 ||
        EVP_DigestSign(md.get(), out, &n, u8(msg), msg.size()) != 1 || n != 64)
        return false;
    sig.assign(reinterpret_cast<char *>(out), 64);
    return true;
}

bool ed25519_verify(const Bytes &pub, const Bytes &msg, const Bytes &sig) {
    if (sig.size() != 64) return false;
    Pkey k = raw_key(EVP_PKEY_ED25519, false, pub);
    MdCtx md(EVP_MD_CTX_new());
    return k && md && EVP_DigestVerifyInit(md.get(), nullptr, nullptr, nullptr, k.get()) == 1 &&
           EVP_DigestVerify(md.get(), u8(sig), sig.size(), u8(msg), msg.size()) == 1;
}

bool random_bytes(size_t n, Bytes &out) {
    if (n > 4096) return false;
    Bytes b(n, '\0');
    if (n && RAND_bytes(u8(b), static_cast<int>(n)) != 1) return false;
    out.swap(b);
    return true;
}

// ---------------------------------------------------------------------------------------------
// TLV8
// ---------------------------------------------------------------------------------------------
Bytes tlv_encode(const std::vector<Tlv> &items) {
    Bytes out;
    for (const Tlv &t : items) {
        if (t.value.empty()) {   // an empty item, such as a separator
            out += static_cast<char>(t.type);
            out += '\0';
            continue;
        }
        for (size_t off = 0; off < t.value.size(); off += 255) {
            const size_t n = std::min<size_t>(255, t.value.size() - off);
            out += static_cast<char>(t.type);
            out += static_cast<char>(n);
            out.append(t.value, off, n);
        }
    }
    return out;
}

bool tlv_decode(const Bytes &data, std::vector<Tlv> &items) {
    items.clear();
    size_t pos = 0;
    bool joinable = false;   // the previous item was a full 255-byte fragment
    while (pos < data.size()) {
        if (data.size() - pos < 2) return false;
        const uint8_t type = static_cast<uint8_t>(data[pos]);
        const size_t len = static_cast<uint8_t>(data[pos + 1]);
        if (data.size() - pos - 2 < len) return false;
        // A value over 255 bytes arrives as consecutive items of one type; the separator rule
        // keeps two distinct items of one type apart, so a neighbour of the same type continues.
        if (!items.empty() && items.back().type == type && joinable)
            items.back().value.append(data, pos + 2, len);
        else
            items.push_back(Tlv{type, data.substr(pos + 2, len)});
        joinable = len == 255;
        pos += 2 + len;
    }
    return true;
}

const Bytes *tlv_find(const std::vector<Tlv> &items, uint8_t type) {
    for (const Tlv &t : items)
        if (t.type == type) return &t.value;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// SRP-6a, 3072-bit group of RFC 5054 appendix A, generator 5, SHA-512
// ---------------------------------------------------------------------------------------------
namespace {
const char *const kN3072 =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
constexpr size_t kSrpLen = 384;

BIGNUM *bn(const Bytes &b) { return BN_bin2bn(u8(b), static_cast<int>(b.size()), nullptr); }

bool padded(const BIGNUM *v, Bytes &out) {
    out.assign(kSrpLen, '\0');
    return BN_bn2binpad(v, u8(out), static_cast<int>(kSrpLen)) == static_cast<int>(kSrpLen);
}
}  // namespace

Bytes srp_modulus() {
    Bytes n;
    unhex(kN3072, n);
    return n;
}

Bytes srp_k() {
    Bytes g(kSrpLen, '\0');
    g[kSrpLen - 1] = 5;
    return sha512(srp_modulus() + g);
}

bool srp_client(const std::string &user, const std::string &pass, const Bytes &salt_in, const Bytes &B_in,
                const Bytes &a_in, SrpProof &out) {
    if (B_in.empty() || B_in.size() > kSrpLen || salt_in.empty() || salt_in.size() > 64) return false;
    // aiohomekit pads a short salt to 16 bytes and B to 384 (the ADK strips B's leading zeros).
    const Bytes salt = salt_in.size() < 16 ? Bytes(16 - salt_in.size(), '\0') + salt_in : salt_in;
    const Bytes Bb = Bytes(kSrpLen - B_in.size(), '\0') + B_in;
    const Bytes Nb = srp_modulus();
    Bytes a = a_in;
    Wipe wa{a};
    if (a.empty() && !random_bytes(32, a)) return false;

    BnCtx ctx(BN_CTX_new());
    Bn N(bn(Nb)), g(BN_new()), B(bn(Bb)), av(bn(a)), A(BN_new()), k(bn(srp_k())), v(BN_new()), t(BN_new()),
        base(BN_new()), e(BN_new()), S(BN_new());
    if (!ctx || !N || !g || !B || !av || !A || !k || !v || !t || !base || !e || !S || !BN_set_word(g.get(), 5))
        return false;
    // RFC 5054 2.6: the client aborts when B % N is zero.
    if (!BN_nnmod(t.get(), B.get(), N.get(), ctx.get()) || BN_is_zero(t.get())) return false;
    BN_set_flags(av.get(), BN_FLG_CONSTTIME);
    Bytes Ab;
    if (!BN_mod_exp(A.get(), g.get(), av.get(), N.get(), ctx.get()) || !padded(A.get(), Ab)) return false;
    Bn u(bn(sha512(Ab + Bb)));
    if (!u || BN_is_zero(u.get())) return false;
    Bytes xb = sha512(salt + sha512(user + ":" + pass));
    Wipe wx{xb};
    Bn x(bn(xb));
    if (!x) return false;
    BN_set_flags(x.get(), BN_FLG_CONSTTIME);
    // S = (B - k * g^x) ^ (a + u * x) mod N
    if (!BN_mod_exp(v.get(), g.get(), x.get(), N.get(), ctx.get()) ||
        !BN_mod_mul(t.get(), k.get(), v.get(), N.get(), ctx.get()) ||
        !BN_mod_sub(base.get(), B.get(), t.get(), N.get(), ctx.get()) || !BN_mul(e.get(), u.get(), x.get(), ctx.get()) ||
        !BN_add(e.get(), e.get(), av.get()))
        return false;
    BN_set_flags(e.get(), BN_FLG_CONSTTIME);
    Bytes Sb;
    Wipe ws{Sb};
    if (!BN_mod_exp(S.get(), base.get(), e.get(), N.get(), ctx.get()) || !padded(S.get(), Sb)) return false;
    const Bytes K = sha512(Sb);
    Bytes group = sha512(Nb);
    const Bytes hg = sha512(Bytes(1, '\x05'));
    if (group.size() != 64 || hg.size() != 64 || K.size() != 64) return false;
    for (size_t i = 0; i < 64; i++) group[i] = static_cast<char>(group[i] ^ hg[i]);
    out.A = Ab;
    out.K = K;
    out.M1 = sha512(group + sha512(user) + salt + Ab + Bb + K);
    out.M2 = sha512(Ab + out.M1 + K);
    return out.M1.size() == 64 && out.M2.size() == 64;
}

// ---------------------------------------------------------------------------------------------
// Setup codes
// ---------------------------------------------------------------------------------------------
bool parse_setup_code(const std::string &input, std::string &code, std::string &err) {
    const std::string s = trim(input.substr(0, 256));
    std::string digits;
    std::string up;
    for (const char c : s) up += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (up.compare(0, 7, "X-HM://") == 0) {
        // Layout from HomeKitADK HAPAccessorySetup.c: 9 base-36 characters encode a 46-bit value
        // whose low 27 bits are the code as a number; a 4-character setup id may follow.
        const std::string p = up.substr(7);
        if (p.size() != 9 && p.size() != 13) {
            err = kErrNotCode;
            return false;
        }
        uint64_t v = 0;
        for (size_t i = 0; i < p.size(); i++) {
            const char c = p[i];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : -1;
            if (d < 0) {
                err = kErrNotCode;
                return false;
            }
            if (i < 9) v = v * 36 + static_cast<uint64_t>(d);
        }
        const uint64_t n = v & ((uint64_t(1) << 27) - 1);
        if (n == 0 || n > 99999999) {
            err = kErrQrNoCode;
            return false;
        }
        digits = std::to_string(n);
        digits.insert(0, 8 - digits.size(), '0');
    } else {
        for (const char c : s) {
            if (c >= '0' && c <= '9') {
                digits += c;
            } else if (c != '-' && c != ' ') {
                err = kErrNotCode;
                return false;
            }
        }
        if (digits.size() != 8) {
            err = kErrNotCode;
            return false;
        }
    }
    code = digits.substr(0, 3) + "-" + digits.substr(3, 2) + "-" + digits.substr(5, 3);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Session frames
// ---------------------------------------------------------------------------------------------
Bytes seal_frames(Cipher &c, const Bytes &plain) {
    Bytes out;
    for (size_t off = 0; off < plain.size();) {
        const size_t n = std::min<size_t>(1024, plain.size() - off);
        Bytes len(2, '\0');
        len[0] = static_cast<char>(n & 0xff);
        len[1] = static_cast<char>(n >> 8);
        Bytes sealed;
        if (!chacha20_poly1305_seal(c.key, counter_nonce(c.counter), len, plain.substr(off, n), sealed)) return Bytes();
        c.counter++;
        out += len;
        out += sealed;
        off += n;
    }
    return out;
}

bool open_frames(Cipher &c, const Bytes &in, size_t &pos, Bytes &out) {
    while (in.size() - pos >= 2) {
        const size_t n = static_cast<size_t>(u8(in)[pos]) | static_cast<size_t>(u8(in)[pos + 1]) << 8;
        if (n > 1024) return false;
        if (in.size() - pos < 2 + n + 16) return true;   // the rest of this frame has not arrived
        Bytes plain;
        if (!chacha20_poly1305_open(c.key, counter_nonce(c.counter), in.substr(pos, 2), in.substr(pos + 2, n + 16),
                                    plain))
            return false;
        c.counter++;
        out += plain;
        pos += 2 + n + 16;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// HTTP responses
// ---------------------------------------------------------------------------------------------
namespace {
// One line ending in LF (a CR before it is dropped). 1 with the line, 0 when it has not all
// arrived, -1 when it is longer than `limit`.
int take_line(const Bytes &buf, size_t &pos, size_t limit, std::string &out) {
    const size_t avail = buf.size() - pos;
    const char *start = buf.data() + pos;
    const void *nl = avail ? memchr(start, '\n', std::min(avail, limit + 2)) : nullptr;
    if (!nl) return avail > limit + 1 ? -1 : 0;
    const size_t len = static_cast<size_t>(static_cast<const char *>(nl) - start);
    out.assign(start, len);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    if (out.size() > limit) return -1;
    pos += len + 1;
    return 1;
}
}  // namespace

int HttpReader::line(const Bytes &buf, size_t &pos, std::string &out) {
    const int r = take_line(buf, pos, kMaxLine, out);
    if (r > 0) {
        line_bytes_ += out.size() + 2;
        if (line_bytes_ > kMaxHeaderBytes) return -1;
    }
    return r;
}

int HttpReader::feed(const Bytes &buf, size_t &pos) {
    std::string l;
    for (;;) {
        switch (st_) {
        case St::kStatus: {
            const int r = line(buf, pos, l);
            if (r <= 0) return r;
            if (l.empty()) {   // tolerate a stray CRLF between messages
                if (line_bytes_ > 8) return -1;
                break;
            }
            const size_t sp = l.find(' ');
            if (sp == std::string::npos || l.size() < sp + 4) return -1;
            const std::string version = l.substr(0, sp);
            if (version.compare(0, 7, "HTTP/1.") == 0) msg.event = false;
            else if (version.compare(0, 8, "EVENT/1.") == 0) msg.event = true;
            else return -1;
            int status = 0;
            for (size_t i = sp + 1; i < sp + 4; i++) {
                if (l[i] < '0' || l[i] > '9') return -1;
                status = status * 10 + (l[i] - '0');
            }
            if (l.size() > sp + 4 && l[sp + 4] != ' ') return -1;
            msg.status = status;
            st_ = St::kHeader;
            break;
        }
        case St::kHeader: {
            const int r = line(buf, pos, l);
            if (r <= 0) return r;
            if (!l.empty()) {
                if (++headers_ > kMaxHeaders) return -1;
                const size_t colon = l.find(':');
                if (colon == std::string::npos || colon == 0) return -1;
                const std::string name = lower(trim(l.substr(0, colon))), value = trim(l.substr(colon + 1));
                const auto old = msg.headers.find(name);
                if (name == "content-length" && old != msg.headers.end() && old->second != value) return -1;
                msg.headers[name] = value;
                break;
            }
            const auto te = msg.headers.find("transfer-encoding");
            const auto cl = msg.headers.find("content-length");
            if (te != msg.headers.end() && lower(te->second).find("chunked") != std::string::npos) {
                chunked_ = true;
                st_ = St::kChunkSize;
            } else if (cl != msg.headers.end()) {
                const std::string &v = cl->second;
                if (v.empty() || v.size() > 18 || v.find_first_not_of("0123456789") != std::string::npos) return -1;
                const unsigned long long n = std::stoull(v);
                if (n > max_body_) return -1;
                want_ = static_cast<size_t>(n);
                st_ = want_ ? St::kBody : St::kDone;
            } else {
                st_ = St::kDone;   // 204 and the like: no body
            }
            break;
        }
        case St::kBody:
        case St::kChunkData: {
            const size_t take = std::min(want_, buf.size() - pos);
            msg.body.append(buf, pos, take);
            pos += take;
            want_ -= take;
            if (want_) return 0;
            st_ = st_ == St::kBody ? St::kDone : St::kChunkEnd;
            break;
        }
        case St::kChunkSize: {
            const int r = take_line(buf, pos, 64, l);
            if (r <= 0) return r;
            if (++headers_ > kMaxChunks) return -1;
            size_t n = 0, i = 0;
            for (; i < l.size() && isxdigit(static_cast<unsigned char>(l[i])); i++) {
                if (n > (max_body_ >> 4)) return -1;   // larger than anything allowed, and no overflow
                const char c = static_cast<char>(tolower(static_cast<unsigned char>(l[i])));
                n = n * 16 + static_cast<size_t>(c <= '9' ? c - '0' : c - 'a' + 10);
            }
            if (i == 0 || (i < l.size() && l[i] != ';' && l[i] != ' ' && l[i] != '\t')) return -1;
            if (n == 0) {
                st_ = St::kTrailer;
                break;
            }
            if (n > max_body_ - msg.body.size()) return -1;
            want_ = n;
            st_ = St::kChunkData;
            break;
        }
        case St::kChunkEnd: {
            const int r = take_line(buf, pos, 2, l);
            if (r <= 0) return r;
            if (!l.empty()) return -1;
            st_ = St::kChunkSize;
            break;
        }
        case St::kTrailer: {
            const int r = line(buf, pos, l);
            if (r <= 0) return r;
            if (l.empty()) st_ = St::kDone;
            break;
        }
        case St::kDone:
            return 1;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Text from accessories
// ---------------------------------------------------------------------------------------------
Bytes sanitize_utf8(const Bytes &in) { return support::sanitize_utf8(in, support::BadUtf8::ReplacementCharacter); }

namespace {
// What a name must not keep: control characters; format and default-ignorable characters, which
// are invisible or reorder text (bidi controls, zero-width characters, tags, variation selectors);
// the fillers that render as blank; non-characters; and U+FFFD, which stands for bytes that were
// not UTF-8. Nobody can say any of them.
bool invisible_char(uint32_t cp) {
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || cp == 0xAD || cp == 0x34F || cp == 0x61C ||
           cp == 0x115F || cp == 0x1160 || cp == 0x17B4 || cp == 0x17B5 || (cp >= 0x180B && cp <= 0x180F) ||
           (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
           cp == 0x3164 || (cp >= 0xFDD0 && cp <= 0xFDEF) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF ||
           cp == 0xFFA0 || (cp >= 0xFFF0 && cp <= 0xFFFD) || (cp & 0xFFFE) == 0xFFFE || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
           (cp >= 0x1D173 && cp <= 0x1D17A) || (cp >= 0xE0000 && cp <= 0xE0FFF);
}
}  // namespace

std::string clean_name(const std::string &raw) {
    const Bytes s = sanitize_utf8(raw.substr(0, 1024));
    const unsigned char *p = u8(s);
    std::string out;
    size_t count = 0;
    bool space = false;
    for (size_t i = 0; i < s.size() && count < 40;) {
        uint32_t cp = 0;
        const size_t len = support::utf8_at(p, s.size(), i, cp);
        const size_t at = i;
        i += len ? len : 1;
        if (len && support::unicode_space(cp)) {
            space = !out.empty();
            continue;
        }
        if (!len || invisible_char(cp)) continue;
        if (space) {
            if (count + 1 >= 40) break;
            out += ' ';
            count++;
            space = false;
        }
        out.append(s, at, len);
        count++;
    }
    return out;
}

namespace {
bool hex4_at(const Bytes &s, size_t at, unsigned &v) {
    if (at > s.size() || s.size() - at < 4) return false;
    v = 0;
    for (size_t k = 0; k < 4; k++) {
        const int d = support::hex_digit(s[at + k]);
        if (d < 0) return false;
        v = v * 16 + static_cast<unsigned>(d);
    }
    return true;
}
}  // namespace

// nlohmann::json refuses a \uD800-\uDBFF escape that no \uDC00-\uDFFF escape follows, and a lone
// \uDC00-\uDFFF: one such name would make a whole bridge unreadable. HAP-NodeJS writes them (a
// JavaScript string cut through an emoji, which JSON.stringify writes as "\ud83d"). Each lone one
// becomes �, which clean_name drops. Same length, so the size and value counts do not change.
Bytes repair_surrogates(const Bytes &text) {
    Bytes out = text;
    bool in_string = false;
    for (size_t i = 0; i < out.size(); i++) {
        const char c = out[i];
        if (!in_string) {
            if (c == '"') in_string = true;
            continue;
        }
        if (c == '"') {
            in_string = false;
            continue;
        }
        if (c != '\\' || i + 1 >= out.size()) continue;
        unsigned v = 0;
        if (out[i + 1] != 'u' || !hex4_at(out, i + 2, v)) {
            i++;   // a two-character escape (\" \\ \n ...), or a broken one the parser will refuse
            continue;
        }
        if (v >= 0xD800 && v <= 0xDBFF) {
            unsigned w = 0;
            if (i + 7 < out.size() && out[i + 6] == '\\' && out[i + 7] == 'u' && hex4_at(out, i + 8, w) && w >= 0xDC00 &&
                w <= 0xDFFF) {
                i += 11;   // a proper pair: kept
                continue;
            }
            out.replace(i + 2, 4, "FFFD");
        } else if (v >= 0xDC00 && v <= 0xDFFF) {
            out.replace(i + 2, 4, "FFFD");
        }
        i += 5;
    }
    return out;
}

bool parse_reply_json(const Bytes &body, size_t max_values, json &out) {
    out = json();
    const Bytes text = repair_surrogates(sanitize_utf8(body));
    if (support::json_too_deep(text, kMaxJsonDepth) || !json_values_ok(text, max_values)) return false;
    out = json::parse(text, nullptr, false);
    return !out.is_discarded();
}

bool parse_database(const Bytes &body, json &out) {
    if (body.size() > kMaxDatabase) {
        out = json();
        return false;
    }
    return parse_reply_json(body, kMaxDatabaseValues, out);
}

// Counts what makes a JSON tree large: every '[', '{', ',' and ':' outside strings (about one per
// value or member; a real accessory database has about one in seven bytes).
bool json_values_ok(const Bytes &text, size_t max_values) {
    size_t n = 0;
    bool in_string = false, escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if ((c == '[' || c == '{' || c == ',' || c == ':') && ++n > max_values) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The accessory database
// ---------------------------------------------------------------------------------------------
std::string short_type(const std::string &uuid) {
    std::string u;
    for (const char c : uuid.substr(0, 64)) u += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    static const std::string kBase = "-0000-1000-8000-0026BB765291";
    std::string head;
    if (u.size() == 36 && u.compare(8, std::string::npos, kBase) == 0) head = u.substr(0, 8);
    else if (!u.empty() && u.size() <= 8) head = u;
    else return u;   // a maker's own type: matches nothing below
    if (head.find_first_not_of("0123456789ABCDEF") != std::string::npos) return u;
    const size_t nz = head.find_first_not_of('0');
    return nz == std::string::npos ? "0" : head.substr(nz);
}

namespace {
enum class Kind { kNone, kLight, kPlug, kSwitch, kThermostat, kHeater, kCover, kFan, kFan2, kTemp, kHumidity, kMotion, kContact };

struct ServiceRule {
    const char *type;
    Kind kind;
    const char *ref;     // Thing::ref "k"
    const char *thing;   // Thing::type
};
const ServiceRule kServices[] = {
    {"43", Kind::kLight, "light", "light"},       {"47", Kind::kPlug, "plug", "plug"},
    {"49", Kind::kSwitch, "switch", "switch"},    {"4A", Kind::kThermostat, "thermostat", "thermostat"},
    {"BC", Kind::kHeater, "heater", "thermostat"}, {"8C", Kind::kCover, "cover", "cover"},
    {"40", Kind::kFan, "fan", "fan"},             {"B7", Kind::kFan2, "fan2", "fan"},
    {"8A", Kind::kTemp, "temp", "sensor"},        {"82", Kind::kHumidity, "humidity", "sensor"},
    {"85", Kind::kMotion, "motion", "sensor"},    {"80", Kind::kContact, "contact", "sensor"},
};

// An accessory with any of these is skipped as a whole: lock mechanism and management, garage
// door, security system, door, window, valve, faucet, irrigation, camera stream / control /
// recording / operating mode, microphone, doorbell, camera audio, access code, NFC access, access
// control. A switch or light inside such an accessory may well drive the lock or the door.
const char *const kDenied[] = {"45", "44", "41", "7E", "81", "8B", "D0", "D7", "CF", "110", "111",
                               "204", "21A", "112", "121", "127", "260", "266", "DA"};

struct CharRule {
    Kind kind;
    const char *type;
    const char *key;
};
const CharRule kChars[] = {
    {Kind::kLight, "25", "on"},       {Kind::kLight, "8", "bri"},        {Kind::kLight, "13", "hue"},
    {Kind::kLight, "2F", "sat"},      {Kind::kLight, "CE", "ct"},        {Kind::kPlug, "25", "on"},
    {Kind::kPlug, "26", "inuse"},     {Kind::kSwitch, "25", "on"},       {Kind::kThermostat, "11", "cur"},
    {Kind::kThermostat, "35", "target"}, {Kind::kThermostat, "F", "curst"}, {Kind::kThermostat, "33", "mode"},
    {Kind::kThermostat, "10", "hum"}, {Kind::kHeater, "B0", "active"},   {Kind::kHeater, "11", "cur"},
    {Kind::kHeater, "12", "heat"},    {Kind::kHeater, "B1", "curst"},    {Kind::kHeater, "B2", "mode"},
    {Kind::kCover, "6D", "pos"},      {Kind::kCover, "7C", "target"},    {Kind::kFan, "25", "on"},
    {Kind::kFan, "29", "speed"},      {Kind::kFan2, "B0", "active"},     {Kind::kFan2, "29", "speed"},
    {Kind::kTemp, "11", "temp"},      {Kind::kHumidity, "10", "hum"},    {Kind::kMotion, "22", "motion"},
    {Kind::kContact, "6A", "contact"},
};

const ServiceRule *service_rule(const std::string &type) {
    for (const ServiceRule &r : kServices)
        if (type == r.type) return &r;
    return nullptr;
}
const ServiceRule *rule_by_ref(const std::string &ref) {
    for (const ServiceRule &r : kServices)
        if (ref == r.ref) return &r;
    return nullptr;
}
bool denied(const std::string &type) {
    for (const char *d : kDenied)
        if (type == d) return true;
    return false;
}

// A service type as HAP writes it: a string of 1 to 8 hexadecimal digits (Apple's short form) or a
// full 8-4-4-4-12 UUID. Anything else (a number, a stray space, no type at all, a service that is
// not an object) cannot be checked against kDenied, so its accessory is skipped: the deny rule
// fails closed.
bool well_formed_service_type(const json &service) {
    const json *t = member(service, "type");
    if (!t || !t->is_string()) return false;
    const std::string &u = t->get_ref<const std::string &>();
    auto hexd = [](char c) { return support::hex_digit(c) >= 0; };
    if (!u.empty() && u.size() <= 8) return std::all_of(u.begin(), u.end(), hexd);
    if (u.size() != 36) return false;
    for (size_t i = 0; i < u.size(); i++)
        if (i == 8 || i == 13 || i == 18 || i == 23 ? u[i] != '-' : !hexd(u[i])) return false;
    return true;
}
const char *char_key(Kind kind, const std::string &type) {
    for (const CharRule &r : kChars)
        if (r.kind == kind && type == r.type) return r.key;
    return nullptr;
}

bool numeric_format(const std::string &f) {
    return f == "bool" || f == "uint8" || f == "uint16" || f == "uint32" || f == "uint64" || f == "int" ||
           f == "float";
}

// A short fingerprint of an accessory-information value (model, serial number, firmware).
std::string info_hash(const std::string &value) { return hex(sha512(sanitize_utf8(value.substr(0, 4096)))).substr(0, 16); }

// One characteristic as Thing::ref keeps it: {"i": iid, "t": short type, "f": format, "r": readable,
// "w": writable, "min", "max", "step", "vv": valid values}. `value` is its current value when it
// has one. (The caller adds "t".)
bool parse_characteristic(const json &c, json &spec, json &value) {
    int64_t iid = 0;
    if (!id_of(c, "iid", iid)) return false;
    std::string f = lower(string_of(c, "format"));
    const json *v = member(c, "value");
    if (f.empty() && v) f = v->is_boolean() ? "bool" : v->is_number_integer() ? "int" : v->is_number() ? "float" : "";
    if (!numeric_format(f)) return false;
    bool r = false, w = false;
    if (const json *perms = array_of(c, "perms")) {
        for (const json &p : *perms) {
            if (!p.is_string()) continue;
            r = r || p.get<std::string>() == "pr";
            w = w || p.get<std::string>() == "pw";
        }
    } else {
        r = true;   // no permissions listed: assume it can at least be read
    }
    spec = {{"i", iid}, {"f", f}, {"r", r}, {"w", w}};
    double d;
    for (const auto &k : {std::make_pair("minValue", "min"), std::make_pair("maxValue", "max"),
                          std::make_pair("minStep", "step")}) {
        const json *m = member(c, k.first);
        if (m && m->is_number() && number_of(*m, d)) spec[k.second] = d;
    }
    if (const json *vv = array_of(c, "valid-values")) {
        json list = json::array();
        for (const json &e : *vv) {
            if (list.size() >= 32) break;
            if (e.is_number() && number_of(e, d) && d == std::floor(d) && std::fabs(d) < 1e15)
                list.push_back(static_cast<int64_t>(d));
        }
        if (!list.empty()) spec["vv"] = list;
    }
    if (v && (v->is_boolean() || (v->is_number() && number_of(*v, d)))) value = *v;
    return true;
}

bool can(const json &ch, const char *key, const char *perm) {
    const json *s = member(ch, key);
    const json *p = s ? member(*s, perm) : nullptr;
    return p && p->is_boolean() && p->get<bool>();
}

double spec_num(const json &spec, const char *key, double fallback) {
    const json *m = member(spec, key);
    double d;
    return m && number_of(*m, d) ? d : fallback;
}

// The values a characteristic accepts out of `all`: its valid values when it lists them, else
// those within its range.
std::set<int64_t> allowed_values(const json &spec, std::initializer_list<int64_t> all) {
    std::set<int64_t> out;
    if (const json *vv = array_of(spec, "vv")) {
        for (const json &e : *vv)
            if (e.is_number_integer()) out.insert(e.get<int64_t>());
        return out;
    }
    const double lo = spec_num(spec, "min", -1e18), hi = spec_num(spec, "max", 1e18);
    for (const int64_t v : all)
        if (v >= lo && v <= hi) out.insert(v);
    return out;
}

// A heater-cooler can take a heating target only through its heating threshold, and only when it
// can be set to heat (1) or auto (0): one that only cools has no heating target to offer.
bool heater_heats(const json &ch) {
    if (!can(ch, "heat", "w")) return false;
    const json *mode = member(ch, "mode");
    if (!mode) return true;   // the mode characteristic is required; without one, the threshold is all there is
    const std::set<int64_t> modes = allowed_values(*mode, {0, 1, 2});
    return modes.count(0) || modes.count(1);
}

uint32_t caps_of(Kind k, const json &ch) {
    uint32_t c = 0;
    switch (k) {
    case Kind::kLight:
        if (can(ch, "on", "w")) c |= home::kPower;
        if (can(ch, "bri", "w")) c |= home::kBrightness;
        if (can(ch, "ct", "w")) c |= home::kColorTemp;
        if (can(ch, "hue", "w") && can(ch, "sat", "w")) c |= home::kColor;
        break;
    case Kind::kPlug:
    case Kind::kSwitch:
        if (can(ch, "on", "w")) c |= home::kPower;
        break;
    case Kind::kThermostat: {
        // A cool-only thermostat (an air conditioner: modes off and cool) has no heating target;
        // its target is a cooling setpoint, so it is not offered as heating.
        const json *mode = member(ch, "mode");
        const std::set<int64_t> modes = mode ? allowed_values(*mode, {0, 1, 2, 3}) : std::set<int64_t>{1};
        const bool heats = modes.count(1) || modes.count(3);
        if (heats && (can(ch, "target", "w") || can(ch, "mode", "w"))) c |= home::kTargetTemp;
        if (can(ch, "cur", "r")) c |= home::kSensor;
        break;
    }
    case Kind::kHeater:
        if (can(ch, "active", "w")) c |= home::kPower;
        if (heater_heats(ch)) c |= home::kTargetTemp;
        if (can(ch, "cur", "r")) c |= home::kSensor;
        break;
    case Kind::kCover:
        if (can(ch, "target", "w")) c |= home::kPosition;
        break;
    case Kind::kFan:
        if (can(ch, "on", "w")) c |= home::kPower;
        if (can(ch, "speed", "w")) c |= home::kFanSpeed;
        break;
    case Kind::kFan2:
        if (can(ch, "active", "w")) c |= home::kPower;
        if (can(ch, "speed", "w")) c |= home::kFanSpeed;
        break;
    case Kind::kTemp:
        if (can(ch, "temp", "r")) c |= home::kSensor;
        break;
    case Kind::kHumidity:
        if (can(ch, "hum", "r")) c |= home::kSensor;
        break;
    case Kind::kMotion:
        if (can(ch, "motion", "r")) c |= home::kSensor;
        break;
    case Kind::kContact:
        if (can(ch, "contact", "r")) c |= home::kSensor;
        break;
    case Kind::kNone:
        break;
    }
    return c;
}

double round_to(double v, double unit) { return std::round(v / unit) * unit; }

// A temperature reading in degrees C, to 0.1; false for a value no thermometer gives (a hostile
// 1e308 would otherwise round to infinity).
bool celsius(double v, double &out) {
    if (!(v >= -100 && v <= 200)) return false;
    out = round_to(v, 0.1);
    return std::isfinite(out);
}

// Percent of a characteristic's range (fan speeds: some go 0..100, some 0..3).
double percent_of(const json &spec, double v) {
    const double lo = spec_num(spec, "min", 0), hi = spec_num(spec, "max", 100);
    if (hi <= lo) return 0;
    return std::min(100.0, std::max(0.0, (v - lo) * 100.0 / (hi - lo)));
}

// Folds `vals` (ref key -> value) into the thing's state, under the keys the app's smart-home
// layer shows (src/smarthome/home.h): current_c, target_c, mode, humidity, contact "open" /
// "closed"... Keys of this driver's own (heating, in_use, target_position, reachable) never reach
// the model there.
void merge_state(home::Thing &t, const std::map<std::string, json> &vals, bool any_failed) {
    const json *k = member(t.ref, "k");
    const ServiceRule *rule = k && k->is_string() ? rule_by_ref(k->get<std::string>()) : nullptr;
    const json *ch = member(t.ref, "c");
    if (!rule || !ch) return;
    if (!t.state.is_object()) t.state = json::object();
    json &st = t.state;
    auto num = [&](const char *key, double &v) {
        const auto it = vals.find(key);
        return it != vals.end() && number_of(it->second, v);
    };
    auto spec = [&](const char *key) -> const json & {
        static const json empty = json::object();
        const json *s = member(*ch, key);
        return s ? *s : empty;
    };
    double v, c;
    auto humidity = [&](double h) { return round_to(std::min(100.0, std::max(0.0, h)), 0.1); };
    switch (rule->kind) {
    case Kind::kLight:
        if (num("on", v)) st["on"] = v != 0;
        if (num("bri", v)) st["brightness"] = static_cast<int>(std::lround(std::min(100.0, std::max(0.0, v))));
        if (num("hue", v)) st["hue"] = round_to(std::min(360.0, std::max(0.0, v)), 0.1);
        if (num("sat", v)) st["saturation"] = round_to(std::min(100.0, std::max(0.0, v)), 0.1);
        if (num("ct", v) && v >= 1) st["kelvin"] = static_cast<int>(std::lround(1e6 / v));
        break;
    case Kind::kPlug:
    case Kind::kSwitch:
        if (num("on", v)) st["on"] = v != 0;
        if (num("inuse", v)) st["in_use"] = v != 0;
        break;
    case Kind::kThermostat:
        if (num("cur", v) && celsius(v, c)) st["current_c"] = c;
        if (num("target", v) && celsius(v, c)) st["target_c"] = c;
        if (num("curst", v)) st["heating"] = v == 1;
        if (num("mode", v)) {
            // "cool" is outside the app's off / heat / auto, but true: it is shown, never set.
            static const char *const modes[] = {"off", "heat", "cool", "auto"};
            if (v >= 0 && v <= 3 && v == std::floor(v)) st["mode"] = modes[static_cast<int>(v)];
        }
        if (num("hum", v)) st["humidity"] = humidity(v);
        break;
    case Kind::kHeater: {
        double active = -1, target = -1;
        if (num("active", active)) st["on"] = active == 1;
        if (num("cur", v) && celsius(v, c)) st["current_c"] = c;
        if (num("heat", v) && celsius(v, c)) st["target_c"] = c;
        if (num("curst", v)) st["heating"] = v == 2;
        num("mode", target);
        if (active == 0) {
            st["mode"] = "off";
        } else if (target >= 0) {
            static const char *const modes[] = {"auto", "heat", "cool"};
            if (target <= 2 && target == std::floor(target)) st["mode"] = modes[static_cast<int>(target)];
        }
        break;
    }
    case Kind::kCover:
        if (num("pos", v)) st["position"] = static_cast<int>(std::lround(std::min(100.0, std::max(0.0, v))));
        if (num("target", v)) st["target_position"] = static_cast<int>(std::lround(std::min(100.0, std::max(0.0, v))));
        break;
    case Kind::kFan:
    case Kind::kFan2:
        if (num(rule->kind == Kind::kFan ? "on" : "active", v)) st["on"] = v != 0;
        if (num("speed", v)) st["fan"] = static_cast<int>(std::lround(percent_of(spec("speed"), v)));
        break;
    case Kind::kTemp:
        if (num("temp", v) && celsius(v, c)) st["current_c"] = c;
        break;
    case Kind::kHumidity:
        if (num("hum", v)) st["humidity"] = humidity(v);
        break;
    case Kind::kMotion:
        if (num("motion", v)) st["motion"] = v != 0;
        break;
    case Kind::kContact:
        // ContactSensorState: 0 = contact detected (closed), 1 = not detected (open).
        if (num("contact", v)) st["contact"] = v != 0 ? "open" : "closed";
        break;
    case Kind::kNone:
        break;
    }
    if (any_failed) st["reachable"] = false;
    else if (!vals.empty()) st.erase("reachable");
}

// things_from_database() step by step.

// How many listings each aid has, among the first kMaxAccessories.
std::map<int64_t, int> listings_of(const json &accessories) {
    std::map<int64_t, int> listings;
    size_t seen = 0;
    for (const json &acc : accessories) {
        if (++seen > kMaxAccessories) break;
        int64_t aid = 0;
        if (id_of(acc, "aid", aid)) listings[aid]++;
    }
    return listings;
}

// An accessory's own name and its information fingerprints, from its information service ("3E").
// False when one of its services is malformed or denied: then the accessory gives no thing at all.
bool accessory_info(const json &services, std::string &acc_name, json &ais) {
    bool skip = false, seen_info = false;
    for (const json &s : services) {
        const std::string type = short_type(string_of(s, "type"));
        if (!well_formed_service_type(s) || denied(type)) skip = true;
        if (type != "3E" || seen_info) continue;
        seen_info = true;
        if (const json *chars = array_of(s, "characteristics"))
            for (const json &c : *chars) {
                const std::string ct = short_type(string_of(c, "type"));
                if (ct == "23") acc_name = string_of(c, "value");
                // Model, serial number and firmware revision: what each write checks again
                // (see write_check), so that ids reused by another accessory are noticed.
                int64_t iid = 0;
                const json *v = member(c, "value");
                if ((ct == "21" || ct == "30" || ct == "52") && !ais.contains(ct) && id_of(c, "iid", iid) && v &&
                    v->is_string())
                    ais[ct] = json::array({iid, info_hash(v->get<std::string>())});
            }
    }
    return !skip;
}

struct Candidate {
    const ServiceRule *rule;
    int64_t iid;
    std::string own_name;
    json ch;
    std::map<std::string, json> vals;
};

// The services this driver can do something with, each with the characteristics it understands.
std::vector<Candidate> candidates_of(const json &services) {
    std::vector<Candidate> found;
    for (const json &s : services) {
        const ServiceRule *rule = service_rule(short_type(string_of(s, "type")));
        const json *hidden = member(s, "hidden");
        const json *chars = array_of(s, "characteristics");
        int64_t iid = 0;
        if (!rule || (hidden && hidden->is_boolean() && hidden->get<bool>()) || !chars || !id_of(s, "iid", iid))
            continue;
        Candidate c{rule, iid, std::string(), json::object(), {}};
        std::string name, configured;
        for (const json &chj : *chars) {
            const std::string type = short_type(string_of(chj, "type"));
            if (type == "23") name = string_of(chj, "value");
            if (type == "E3") configured = string_of(chj, "value");
            const char *key = char_key(rule->kind, type);
            json spec, value;
            if (!key || c.ch.contains(key) || !parse_characteristic(chj, spec, value)) continue;
            spec["t"] = type;
            c.ch[key] = spec;
            if (!value.is_null()) c.vals[key] = value;
        }
        if (!caps_of(rule->kind, c.ch)) continue;   // nothing it could do or report
        c.own_name = clean_name(configured).empty() ? name : configured;
        found.push_back(std::move(c));
    }
    return found;
}
}  // namespace

bool things_from_database(const json &db, std::vector<home::Thing> &out) {
    out.clear();
    const json *accessories = array_of(db, "accessories");
    if (!accessories) return false;
    // An aid listed more than once is skipped in every listing: the deny rule must see all of an
    // accessory's services at once (a garage door in one listing, its light in another).
    std::map<int64_t, int> listings = listings_of(*accessories);
    size_t seen = 0;
    for (const json &acc : *accessories) {
        if (++seen > kMaxAccessories || out.size() >= kMaxThings) break;
        int64_t aid = 0;
        const json *services = array_of(acc, "services");
        if (!id_of(acc, "aid", aid) || !services || listings[aid] > 1) continue;

        std::string acc_name;
        json ais = json::object();
        if (!accessory_info(*services, acc_name, ais)) continue;

        std::vector<Candidate> found = candidates_of(*services);

        // Names: an accessory with one thing is called by the accessory's name (what the user
        // named it in the maker's app). With several, each takes its service's own name, prefixed
        // by the accessory's unless it already contains it: "Strip Left", "Hall Sensor Humidity".
        std::set<std::string> used;
        for (size_t i = 0; i < found.size() && out.size() < kMaxThings; i++) {
            Candidate &c = found[i];
            // A malformed accessory database can list one service twice, or give two services the
            // same iid: only the first becomes a thing, so that a thing's id names one service.
            const std::string id = std::to_string(aid) + "." + std::to_string(c.iid);
            if (std::any_of(out.begin(), out.end(), [&](const home::Thing &x) { return x.id == id; })) continue;
            std::string name;
            const std::string own = clean_name(c.own_name), whole = clean_name(acc_name);
            if (found.size() == 1) name = !whole.empty() ? whole : own;
            else if (own.empty()) name = !whole.empty() ? whole + " " + std::to_string(i + 1) : std::string();
            else if (whole.empty() || lower(own).find(lower(whole)) != std::string::npos) name = own;
            else name = whole + " " + own;
            if (name.empty()) name = "Accessory " + std::to_string(aid);
            name = clean_name(name);
            const std::string base = name;
            for (int n = 2; used.count(name) && n < 1000; n++) {
                const std::string suffix = " " + std::to_string(n);
                // keep the result within 40 characters without cutting a character in half
                std::string cut;
                size_t chars = 0;
                for (size_t k = 0; k < base.size(); k++) {
                    const unsigned char b = static_cast<unsigned char>(base[k]);
                    if ((b & 0xC0) != 0x80 && ++chars > 40 - suffix.size()) break;
                    cut += base[k];
                }
                name = clean_name(trim(cut) + suffix);
            }
            used.insert(name);

            home::Thing t;
            t.id = id;
            t.name = name;
            t.type = c.rule->thing;
            t.caps = caps_of(c.rule->kind, c.ch);
            t.ask_first = c.rule->kind == Kind::kHeater;
            t.ref = {{"aid", aid}, {"svc", c.iid}, {"k", c.rule->ref}, {"c", c.ch}, {"ais", ais}};
            t.state = json::object();
            merge_state(t, c.vals, false);
            out.push_back(std::move(t));
        }
    }
    return true;
}

void apply_values(home::Thing &t, const std::map<std::pair<int64_t, int64_t>, json> &values,
                  const std::vector<std::pair<int64_t, int64_t>> &failed) {
    int64_t aid = 0;
    const json *ch = member(t.ref, "c");
    if (!id_of(t.ref, "aid", aid) || !ch || !ch->is_object()) return;
    std::map<std::string, json> vals;
    bool any_failed = false;
    for (auto it = ch->begin(); it != ch->end(); ++it) {
        int64_t iid = 0;
        if (!id_of(it.value(), "i", iid)) continue;
        const auto v = values.find({aid, iid});
        if (v != values.end()) vals[it.key()] = v->second;
        if (std::find(failed.begin(), failed.end(), std::make_pair(aid, iid)) != failed.end()) any_failed = true;
    }
    // The state is what this read saw. A value that could not be read now is left out rather than
    // kept from an earlier read: the app would show a kept value as the current one.
    t.state = json::object();
    merge_state(t, vals, any_failed);
}

// No word here may read as "the accessory did not answer" (the app rescans the network for those,
// control.cpp unreachable()): a status always comes from an accessory that did answer.
std::string status_text(long status, const std::string &name) {
    const std::string who = name.empty() ? std::string("The accessory") : name;
    switch (status == LONG_MIN ? 0 : status < 0 ? -status : status) {
    case 70401: return "This app is not allowed to do that to " + who + ".";
    case 70402: return who + " is switched off, or out of range of its bridge.";
    case 70403: return who + " is busy. Try again in a moment.";
    case 70404: return who + " cannot be changed.";
    case 70405: return who + " cannot be read.";
    case 70407: return who + " is out of resources. Try again in a moment.";
    case 70408: return who + " was too slow to do that. Try again in a moment.";
    case 70409: return who + " no longer exists on the accessory. Refresh the device in settings.";
    case 70410: return who + " refused that value.";
    case 70411: return who + " needs an authorisation this app cannot give.";
    case 70412: return who + " cannot do that right now.";
    default: return who + " reported an error.";
    }
}

// ---------------------------------------------------------------------------------------------
// Writes for a change
// ---------------------------------------------------------------------------------------------
namespace {
// Clamps to the characteristic's range and rounds to its step, in its format.
json format_value(const json &spec, double v) {
    const json *fj = member(spec, "f");
    const std::string f = fj && fj->is_string() ? fj->get<std::string>() : "float";
    if (f == "bool") return json(v != 0);
    double flo = -std::numeric_limits<double>::infinity(), fhi = std::numeric_limits<double>::infinity();
    if (f == "uint8") flo = 0, fhi = 255;
    else if (f == "uint16") flo = 0, fhi = 65535;
    else if (f == "uint32") flo = 0, fhi = 4294967295.0;
    else if (f == "uint64") flo = 0, fhi = 9007199254740992.0;
    else if (f == "int") flo = -2147483648.0, fhi = 2147483647.0;
    double lo = std::max(flo, spec_num(spec, "min", flo)), hi = std::min(fhi, spec_num(spec, "max", fhi));
    // A declared range outside the format's (a uint8 from 1e16) or upside down: only the format's
    // own range holds (found by the safety reviewer's fuzz harness, rerun on 2026-11-17).
    if (hi < lo) lo = flo, hi = fhi;
    v = std::min(hi, std::max(lo, v));
    const double step = spec_num(spec, "step", 0);
    if (step > 0) {
        const double base = std::isfinite(lo) ? lo : 0;
        v = base + std::round((v - base) / step) * step;
        if (v > hi) v -= step;
        if (v < lo) v += step;
        v = std::min(hi, std::max(lo, v));
    }
    v = std::min(9e15, std::max(-9e15, v));   // whatever the ranges said, a value llround can hold
    if (f == "float") return json(std::round(v * 1e6) / 1e6);
    return json(static_cast<int64_t>(std::llround(v)));
}
}  // namespace

namespace {
// writes_for() part by part: the writes by ref key, where explicit fields win over implied ones ("on"
// implied by a brightness). Each part is false, with `err` set, when the thing cannot take it.
struct WritePlan {
    const home::Change &c;
    const json *ch;
    Kind kind;
    const std::string &name, &cannot;
    const char *power;   // what switches this kind on and off
    std::string &err;
    std::map<std::string, json> explicit_w, implied_w;

    const json *writable(const char *key) const {
        const json *s = member(*ch, key);
        return s && can(*ch, key, "w") ? s : nullptr;
    }
    const json *need(const char *key) {
        const json *s = writable(key);
        if (!s) err = cannot;
        return s;
    }
    void imply_on() {
        if (c.on) return;
        if (const json *s = writable(power)) implied_w[power] = format_value(*s, 1);
    }

    bool on_off() {
        if (c.on) {
            if (kind == Kind::kThermostat) {
                if (!c.mode) {
                    const json *s = need("mode");
                    if (!s) return false;
                    const int64_t want = *c.on ? 1 : 0;
                    if (!allowed_values(*s, {0, 1, 2, 3}).count(want)) {
                        err = cannot;
                        return false;
                    }
                    explicit_w["mode"] = format_value(*s, static_cast<double>(want));
                }
            } else if (kind == Kind::kCover) {
                err = cannot;
                return false;
            } else {
                const json *s = need(power);
                if (!s) return false;
                explicit_w[power] = format_value(*s, *c.on ? 1 : 0);
            }
        }
        return true;
    }
    bool brightness() {
        if (c.brightness) {
            if (kind != Kind::kLight) {
                err = cannot;
                return false;
            }
            if (*c.brightness <= 0) {
                const json *s = need("on");
                if (!s) return false;
                if (!c.on) explicit_w["on"] = format_value(*s, 0);
            } else {
                const json *s = need("bri");
                if (!s) return false;
                explicit_w["bri"] = format_value(*s, std::min(100, *c.brightness));
                imply_on();
            }
        }
        return true;
    }
    bool kelvin() {
        if (c.kelvin) {
            const json *s = kind == Kind::kLight ? need("ct") : nullptr;
            if (!s || *c.kelvin <= 0) {
                err = cannot;
                return false;
            }
            json spec = *s;   // HAP's default range when the bulb gives none: 140..500 mireds
            if (!member(spec, "min")) spec["min"] = 140.0;
            if (!member(spec, "max")) spec["max"] = 500.0;
            explicit_w["ct"] = format_value(spec, 1e6 / *c.kelvin);
            imply_on();
        }
        return true;
    }
    bool colour() {
        if (c.hs) {
            const json *h = kind == Kind::kLight ? need("hue") : nullptr;
            const json *s = h ? need("sat") : nullptr;
            if (!h || !s) {
                err = cannot;
                return false;
            }
            explicit_w["hue"] = format_value(*h, std::min(360, std::max(0, c.hs->first)));
            explicit_w["sat"] = format_value(*s, std::min(100, std::max(0, c.hs->second)));
            imply_on();
        }
        return true;
    }
    bool target() {
        if (c.target_c) {
            const char *key = kind == Kind::kThermostat ? "target" : kind == Kind::kHeater ? "heat" : nullptr;
            const json *s = key ? need(key) : nullptr;
            if (!s || !std::isfinite(*c.target_c)) {
                err = cannot;
                return false;
            }
            const double want = std::min(kMaxTargetC, std::max(kMinTargetC, *c.target_c));
            json v = format_value(*s, want);
            double got;
            if (!number_of(v, got) || got < kMinTargetC - 0.5 || got > kMaxTargetC + 0.5) {
                err = "That temperature is outside what " + name + " accepts.";
                return false;
            }
            explicit_w[key] = v;
        }
        return true;
    }
    bool mode() {
        if (c.mode) {
            const std::string m = lower(*c.mode);
            if (m != "off" && m != "heat" && m != "auto") {
                err = "That is not a heating mode.";
                return false;
            }
            if (kind == Kind::kThermostat) {
                const json *s = need("mode");
                if (!s) return false;
                const int64_t want = m == "off" ? 0 : m == "heat" ? 1 : 3;
                if (!allowed_values(*s, {0, 1, 2, 3}).count(want)) {
                    err = name + " has no " + m + " mode.";
                    return false;
                }
                explicit_w["mode"] = format_value(*s, static_cast<double>(want));
            } else if (kind == Kind::kHeater) {
                const json *a = need("active");
                if (!a) return false;
                explicit_w["active"] = format_value(*a, m == "off" ? 0 : 1);
                if (m != "off") {
                    const json *s = writable("mode");
                    const int64_t want = m == "heat" ? 1 : 0;
                    if (!s || !allowed_values(*s, {0, 1, 2}).count(want)) {
                        err = name + " has no " + m + " mode.";
                        return false;
                    }
                    explicit_w["mode"] = format_value(*s, static_cast<double>(want));
                }
            } else {
                err = cannot;
                return false;
            }
        }
        return true;
    }
    bool position() {
        if (c.position) {
            const json *s = kind == Kind::kCover ? need("target") : nullptr;
            if (!s) {
                err = cannot;
                return false;
            }
            explicit_w["target"] = format_value(*s, std::min(100, std::max(0, *c.position)));
        }
        return true;
    }
    bool fan() {
        if (c.fan) {
            if (kind != Kind::kFan && kind != Kind::kFan2) {
                err = cannot;
                return false;
            }
            if (*c.fan <= 0) {
                const json *s = need(power);
                if (!s) return false;
                if (!c.on) explicit_w[power] = format_value(*s, 0);
            } else {
                const json *s = need("speed");
                if (!s) return false;
                const double lo = spec_num(*s, "min", 0), hi = spec_num(*s, "max", 100);
                explicit_w["speed"] = format_value(*s, lo + std::min(100, *c.fan) * (hi - lo) / 100.0);
                imply_on();
            }
        }
        return true;
    }

    // The writes in the order they are sent.
    bool ordered(int64_t aid, std::vector<Write> &out) {
        for (const auto &w : implied_w) explicit_w.insert(w);   // insert keeps an explicit value
        if (explicit_w.empty()) {
            err = kErrNothing;
            return false;
        }
        // Power first, then the mode, then the rest: some accessories apply the items in order.
        std::vector<std::string> order;
        for (const char *first : {"on", "active", "mode"})
            if (explicit_w.count(first)) order.push_back(first);
        for (const auto &w : explicit_w)
            if (std::find(order.begin(), order.end(), w.first) == order.end()) order.push_back(w.first);
        for (const std::string &key : order) {
            int64_t iid = 0;
            const json *spec = member(*ch, key.c_str());
            if (!spec || !id_of(*spec, "i", iid)) {
                err = cannot;
                return false;
            }
            out.push_back(Write{aid, iid, explicit_w[key]});
        }
        return true;
    }
};
}  // namespace

bool writes_for(const home::Thing &t, const home::Change &c, std::vector<Write> &out, std::string &err) {
    out.clear();
    const std::string name = clean_name(t.name).empty() ? std::string("That device") : clean_name(t.name);
    const json *k = member(t.ref, "k");
    const ServiceRule *rule = k && k->is_string() ? rule_by_ref(k->get<std::string>()) : nullptr;
    const json *ch = member(t.ref, "c");
    int64_t aid = 0;
    if (!rule || !ch || !ch->is_object() || !id_of(t.ref, "aid", aid)) {
        err = name + " needs to be refreshed in settings.";
        return false;
    }
    const Kind kind = rule->kind;
    const std::string cannot = name + " cannot do that.";
    if (c.volume || c.volume_step || c.mute || c.transport || c.source || c.key) {
        err = cannot;
        return false;
    }
    if (kind == Kind::kTemp || kind == Kind::kHumidity || kind == Kind::kMotion || kind == Kind::kContact) {
        err = name + " is a sensor: it can only be read.";
        return false;
    }

    // Writes by ref key; explicit fields win over implied ones ("on" implied by a brightness).
    WritePlan w{c, ch, kind, name, cannot, kind == Kind::kFan2 || kind == Kind::kHeater ? "active" : "on", err, {}, {}};
    if (!w.on_off() || !w.brightness() || !w.kelvin() || !w.colour() || !w.target() || !w.mode() || !w.position() ||
        !w.fan())
        return false;
    return w.ordered(aid, out);
}
}  // namespace detail

// =============================================================================================
// Transport: one TCP connection with a deadline, plain HTTP until pair-verify, then frames.
// =============================================================================================
namespace {
using namespace detail;

Bytes http_request(const std::string &method, const std::string &path, const std::string &host,
                   const std::string &ctype, const Bytes &body) {
    std::string r = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
    if (!ctype.empty()) r += "Content-Type: " + ctype + "\r\n";
    if (method != "GET") r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "\r\n";
    r += body;
    return r;
}

class Link {
public:
    explicit Link(Clock::time_point deadline) : deadline_(deadline) {}
    ~Link() { close(); }
    Link(const Link &) = delete;
    Link &operator=(const Link &) = delete;

    bool open(const home::Device &dev, std::string &err) {
        if (dev.ip.empty() || dev.ip.size() > 64 || dev.port <= 0 || dev.port > 65535) {
            err = kErrNoAddress;
            return false;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;   // never a name lookup
        addrinfo *res = nullptr;
        const std::string port = std::to_string(dev.port);
        if (getaddrinfo(dev.ip.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
            err = kErrNoAddress;
            return false;
        }
        std::unique_ptr<addrinfo, void (*)(addrinfo *)> guard(res, freeaddrinfo);
        fd_ = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            err = kErrUnreachable;
            return false;
        }
        const int yes = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        if (connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
            if (errno != EINPROGRESS) {
                err = kErrUnreachable;
                close();
                return false;
            }
            const Clock::time_point until = std::min(deadline_, Clock::now() + std::chrono::milliseconds(kConnectMs));
            const int w = wait(POLLOUT, until);
            int so = 0;
            socklen_t len = sizeof so;
            if (w <= 0 || getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so, &len) != 0 || so != 0) {
                err = w == 0 && remaining_ms(deadline_) == 0 ? kErrTimeout : kErrUnreachable;
                close();
                return false;
            }
        }
        host_ = res->ai_family == AF_INET6 ? "[" + dev.ip + "]" : dev.ip;
        return true;
    }

    void secure(const Bytes &write_key, const Bytes &read_key) {
        enc_ = true;
        out_ = Cipher{write_key, 0};
        in_ = Cipher{read_key, 0};
        raw_.clear();
        raw_pos_ = 0;
        plain_.clear();   // nothing sent in the clear after pair-verify M4 belongs to the session
        plain_pos_ = 0;
    }

    // Sends one request and reads its response, skipping EVENT/1.0 messages. `sent` tells a
    // failure before the request left from one while waiting for the answer.
    bool exchange(const std::string &method, const std::string &path, const std::string &ctype, const Bytes &body,
                  size_t max_body, HttpResponse &resp, std::string &err, bool *sent = nullptr) {
        if (sent) *sent = false;
        if (fd_ < 0) {
            err = kErrDropped;
            return false;
        }
        const Bytes req = http_request(method, path, host_, ctype, body);
        const Bytes wire = enc_ ? seal_frames(out_, req) : req;
        if (wire.empty()) {
            err = kErrInternal;
            return false;
        }
        if (!send_all(wire, err)) return false;   // one write per request (see aiohomekit)
        if (sent) *sent = true;
        const size_t cap = max_body + 2 * kMaxHeaderBytes + 4096;
        for (int events = 0;;) {
            HttpReader reader(max_body);
            int r;
            while ((r = reader.feed(plain_, plain_pos_)) == 0)
                if (!fill(cap, err)) return false;
            if (r < 0) {
                err = kErrGarbled;
                return false;
            }
            if (reader.msg.event || (reader.msg.status >= 100 && reader.msg.status < 200)) {
                if (++events > kMaxEvents) {
                    err = kErrGarbled;
                    return false;
                }
                continue;
            }
            resp = std::move(reader.msg);
            return true;
        }
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        if (!out_.key.empty()) OPENSSL_cleanse(u8(out_.key), out_.key.size());
        if (!in_.key.empty()) OPENSSL_cleanse(u8(in_.key), in_.key.size());
    }

private:
    // 1 when ready, 0 when `until` passed, -1 on error.
    int wait(short events, Clock::time_point until) {
        for (;;) {
            const int ms = remaining_ms(until);
            if (ms <= 0) return 0;
            pollfd p{fd_, events, 0};
            const int r = poll(&p, 1, ms);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) return -1;
            if (r > 0) return 1;
        }
    }

    bool send_all(const Bytes &data, std::string &err) {
        size_t off = 0;
        while (off < data.size()) {
            const ssize_t n = send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                const int w = wait(POLLOUT, deadline_);
                if (w > 0) continue;
                err = w == 0 ? kErrTimeout : kErrDropped;
                return false;
            }
            err = kErrDropped;
            return false;
        }
        return true;
    }

    // Reads what has arrived (waiting until the deadline) into plain_, decrypting when secure.
    bool fill(size_t cap, std::string &err) {
        char buf[16384];
        ssize_t n;
        for (;;) {
            n = recv(fd_, buf, sizeof buf, 0);
            if (n > 0) break;
            if (n == 0) {
                err = kErrDropped;
                return false;
            }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                err = kErrDropped;
                return false;
            }
            const int w = wait(POLLIN, deadline_);
            if (w <= 0) {
                err = w == 0 ? kErrTimeout : kErrDropped;
                return false;
            }
        }
        if (plain_pos_ == plain_.size()) {
            plain_.clear();
            plain_pos_ = 0;
        } else if (plain_pos_ > 65536) {
            plain_.erase(0, plain_pos_);
            plain_pos_ = 0;
        }
        if (!enc_) {
            plain_.append(buf, static_cast<size_t>(n));
        } else {
            raw_.append(buf, static_cast<size_t>(n));
            if (!open_frames(in_, raw_, raw_pos_, plain_)) {
                err = kErrDecrypt;
                return false;
            }
            raw_.erase(0, raw_pos_);
            raw_pos_ = 0;
        }
        if (plain_.size() - plain_pos_ > cap) {
            err = kErrGarbled;
            return false;
        }
        return true;
    }

    int fd_ = -1;
    Clock::time_point deadline_;
    std::string host_;
    bool enc_ = false;
    Cipher out_, in_;
    Bytes raw_, plain_;
    size_t raw_pos_ = 0, plain_pos_ = 0;
};

// =============================================================================================
// Identities and pairing
// =============================================================================================
struct Identity {   // this controller
    std::string id;
    Bytes seed, pub;
    ~Identity() {
        if (!seed.empty()) OPENSSL_cleanse(u8(seed), seed.size());
    }
};
struct Peer {       // the accessory, as pair-setup learnt it
    std::string id;
    Bytes ltpk;
};

bool valid_pairing_id(const std::string &s) {
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s)
        if (c < 0x21 || c > 0x7e) return false;
    return true;
}

// 1 loaded, 0 nothing stored yet, -1 something stored but unusable.
int load_identity(home::Secrets *secrets, Identity &me) {
    if (!secrets) return 0;
    const json j = secrets->get("homekit");
    if (j.is_null() || (j.is_object() && j.empty())) return 0;
    const std::string id = string_of(j, "controller_id"), sk = string_of(j, "ltsk"), pk = string_of(j, "ltpk");
    Bytes seed, pub, derived;
    if (!valid_pairing_id(id) || !unhex(sk, seed) || seed.size() != 32 || !unhex(pk, pub) || pub.size() != 32 ||
        !ed25519_public(seed, derived) || derived != pub) {
        if (!seed.empty()) OPENSSL_cleanse(u8(seed), seed.size());
        return -1;
    }
    me.id = id;
    me.seed = seed;
    me.pub = pub;
    OPENSSL_cleanse(u8(seed), seed.size());
    return 1;
}

bool create_identity(home::Secrets *secrets, Identity &me) {
    if (!secrets) return false;
    Bytes seed, pub, u;
    if (!random_bytes(32, seed) || !ed25519_public(seed, pub) || !random_bytes(16, u)) return false;
    u[6] = static_cast<char>((u[6] & 0x0f) | 0x40);   // a version 4 UUID, as iOS uses
    u[8] = static_cast<char>((u[8] & 0x3f) | 0x80);
    std::string id;
    for (const char c : hex(u)) id += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    for (const size_t at : {20u, 16u, 12u, 8u}) id.insert(at, "-");
    std::string sk = hex(seed);
    secrets->set("homekit", {{"controller_id", id}, {"ltsk", sk}, {"ltpk", hex(pub)}});
    OPENSSL_cleanse(&sk[0], sk.size());
    OPENSSL_cleanse(u8(seed), seed.size());
    // Read it back: an accessory paired to a key that was never stored could not be used again.
    return load_identity(secrets, me) == 1 && me.id == id;
}

bool load_peer(const home::Device &dev, Peer &peer, std::string &controller) {
    const std::string id = string_of(dev.auth, "accessory_id"), pk = string_of(dev.auth, "accessory_ltpk");
    controller = string_of(dev.auth, "controller_id");
    Bytes ltpk;
    if (!valid_pairing_id(id) || !unhex(pk, ltpk) || ltpk.size() != 32 || controller.empty()) return false;
    peer.id = id;
    peer.ltpk = ltpk;
    return true;
}

void forget_pairing(home::Device &dev) {
    if (!dev.auth.is_object()) {
        dev.auth = json::object();
        return;
    }
    for (const char *k : {"accessory_id", "accessory_ltpk", "controller_id"}) dev.auth.erase(k);
}

long info_number(const std::map<std::string, std::string> &info, const char *key, long fallback) {
    const auto it = info.find(key);
    if (it == info.end() || it->second.empty() || it->second.size() > 9 ||
        it->second.find_first_not_of("0123456789") != std::string::npos)
        return fallback;
    return std::stol(it->second);
}

// Discovery's TXT "sf": bit 0 set while the accessory has no controller.
bool paired_elsewhere(const home::Device &dev) {
    const long sf = info_number(dev.info, "sf", -1);
    return sf >= 0 && !(sf & 1);
}

// A HAP status as a whole number; anything outside the codes' range becomes -1 (no conversion
// of an out-of-range double, which would be undefined).
long status_code(double s) { return std::fabs(s) < 1e9 ? static_cast<long>(s) : -1; }

std::string http_error(int status) {
    if (status == 470) return kErrRefusedSession;
    if (status >= 500) return "The accessory reported an internal error.";
    if (status == 400) return "The accessory did not understand the request.";
    return "The accessory refused the request.";
}

bool post_tlv(Link &link, const char *path, const std::vector<Tlv> &req, std::vector<Tlv> &resp, std::string &err,
              bool *sent = nullptr) {
    HttpResponse r;
    if (!link.exchange("POST", path, "application/pairing+tlv8", tlv_encode(req), kMaxTlvBody, r, err, sent))
        return false;
    // Pairing errors come back as an Error item, with status 200 or (some accessories) 4xx. Any
    // other status counts only with an Error item in it: a JSON body that happened to decode as
    // TLV must not pass for success.
    if (r.body.empty() || !tlv_decode(r.body, resp) || (r.status != 200 && !tlv_find(resp, kTlvError))) {
        err = r.status == 200 ? kErrGarbled : http_error(r.status);
        return false;
    }
    return true;
}

// 0 when the reply is at `expected` without an error, the error code when there is one, -1 when
// it is at another state. A missing State item is tolerated (aiohomekit issue 20).
int step_status(const std::vector<Tlv> &items, int expected) {
    if (const Bytes *s = tlv_find(items, kTlvState))
        if (s->size() != 1 || static_cast<uint8_t>((*s)[0]) != expected) return -1;
    if (const Bytes *e = tlv_find(items, kTlvError)) {
        const int code = e->empty() ? 1 : static_cast<uint8_t>((*e)[0]);
        return code ? code : 1;
    }
    return 0;
}

enum class SetupFail { kNone, kWrongCode, kBackoff, kMaxTries, kUnavailable, kBusy, kMaxPeers, kOther };

bool setup_error(int code, int stage, const std::vector<Tlv> &items, SetupFail &why, uint64_t &retry_s,
                 std::string &err) {
    switch (code) {
    case 2:   // Authentication: at M4 the proof did not match, that is, the code was wrong
        why = stage == 4 ? SetupFail::kWrongCode : SetupFail::kOther;
        err = stage == 4 ? kErrWrongCode : kErrPairFailed;
        break;
    case 3: {   // Backoff, with the seconds to wait in RetryDelay
        // HomeKit ADK HAP/HAPPairing.h: kHAPPairingTLVType_RetryDelay = 0x08, "Seconds to delay
        // until retrying a setup code. integer. Obsolete since R3"; TLV integers are little-endian
        // and as short as the value allows (HAPPairingPairSetup.c, HAPExpandLittleUInt32 and
        // HAPPairingGetNumBytes for kTLVType_Flags).
        why = SetupFail::kBackoff;
        retry_s = 0;
        if (const Bytes *d = tlv_find(items, kTlvRetryDelay))
            for (size_t i = 0; i < d->size() && i < 8; i++) retry_s |= static_cast<uint64_t>(static_cast<uint8_t>((*d)[i])) << (8 * i);
        retry_s = std::min<uint64_t>(retry_s, 86400);
        err = retry_s ? "The accessory asks to wait " + std::to_string(retry_s) + " seconds before the next try."
                      : std::string("The accessory asks to wait before the next try. Try again in a few minutes.");
        break;
    }
    case 4: why = SetupFail::kMaxPeers; err = kErrMaxPeers; break;
    case 5: why = SetupFail::kMaxTries; err = kErrLocked; break;
    case 6: why = SetupFail::kUnavailable; err = kErrPairedElsewhere; break;
    case 7: why = SetupFail::kBusy; err = kErrBusy; break;
    default: why = SetupFail::kOther; err = kErrPairFailed; break;
    }
    return false;
}

// Pair-setup M1..M6 on one connection. The code is sent (as an SRP proof) exactly once.
bool pair_setup(Link &link, const std::string &code, int method, const Identity &me, Peer &peer, SetupFail &why,
                uint64_t &retry_s, std::string &err) {
    why = SetupFail::kOther;
    std::vector<Tlv> r;
    if (!post_tlv(link, "/pair-setup", {{kTlvState, one(1)}, {kTlvMethod, one(static_cast<uint8_t>(method))}}, r, err))
        return false;
    int st = step_status(r, 2);
    if (st) {
        if (st < 0) err = kErrGarbled;
        return st < 0 ? false : setup_error(st, 2, r, why, retry_s, err);
    }
    const Bytes *salt = tlv_find(r, kTlvSalt), *B = tlv_find(r, kTlvPublicKey);
    SrpProof p;
    if (!salt || !B || !srp_client("Pair-Setup", code, *salt, *B, Bytes(), p)) {
        err = kErrGarbled;
        return false;
    }
    Wipe wk{p.K};
    if (!post_tlv(link, "/pair-setup", {{kTlvState, one(3)}, {kTlvPublicKey, p.A}, {kTlvProof, p.M1}}, r, err))
        return false;
    st = step_status(r, 4);
    if (st) {
        if (st < 0) err = kErrGarbled;
        return st < 0 ? false : setup_error(st, 4, r, why, retry_s, err);
    }
    const Bytes *m2 = tlv_find(r, kTlvProof);
    if (!m2 || m2->size() != 64 || CRYPTO_memcmp(m2->data(), p.M2.data(), 64) != 0) {
        err = kErrFakeAccessory;
        return false;
    }
    Bytes key = hkdf_sha512(p.K, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", 32);
    Wipe wkey{key};
    const Bytes x = hkdf_sha512(p.K, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", 32);
    Bytes sig, sealed;
    if (key.size() != 32 || x.size() != 32 || !ed25519_sign(me.seed, x + me.id + me.pub, sig) ||
        !chacha20_poly1305_seal(key, label_nonce("PS-Msg05"), Bytes(),
                                tlv_encode({{kTlvIdentifier, me.id}, {kTlvPublicKey, me.pub}, {kTlvSignature, sig}}),
                                sealed)) {
        err = kErrInternal;
        return false;
    }
    if (!post_tlv(link, "/pair-setup", {{kTlvState, one(5)}, {kTlvEncryptedData, sealed}}, r, err)) return false;
    st = step_status(r, 6);
    if (st) {
        if (st < 0) err = kErrGarbled;
        return st < 0 ? false : setup_error(st, 6, r, why, retry_s, err);
    }
    const Bytes *enc = tlv_find(r, kTlvEncryptedData);
    Bytes plain;
    std::vector<Tlv> sub;
    if (!enc || !chacha20_poly1305_open(key, label_nonce("PS-Msg06"), Bytes(), *enc, plain) || !tlv_decode(plain, sub)) {
        err = kErrBadIdentity;
        return false;
    }
    const Bytes *id = tlv_find(sub, kTlvIdentifier), *ltpk = tlv_find(sub, kTlvPublicKey), *asig = tlv_find(sub, kTlvSignature);
    const Bytes ax = hkdf_sha512(p.K, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", 32);
    if (!id || !ltpk || !asig || !valid_pairing_id(*id) || ltpk->size() != 32 || ax.size() != 32 ||
        !ed25519_verify(*ltpk, ax + *id + *ltpk, *asig)) {
        err = kErrBadIdentity;
        return false;
    }
    peer.id = *id;
    peer.ltpk = *ltpk;
    why = SetupFail::kNone;
    return true;
}

// Pair-verify M1..M4; on success the link is encrypted. `not_known` is set only when the accessory
// answered M4 with an Authentication error after it had proved, in M2, that it holds the saved
// key: then it does not know this controller any more. An error in M2 proves nothing (anything at
// the address can send one) and IP accessories never send Authentication there: the controller's
// identity only arrives in M3 (HomeKit ADK HAPPairingPairVerify.c sets it before M2 only for BLE
// Pair Resume, and while processing M3 for IP; HAP-NodeJS HAPServer.ts handlePairVerifyM1 has no
// error answer, its AUTHENTICATION answers are all M4).
bool pair_verify(Link &link, const Identity &me, const Peer &peer, bool &not_known, std::string &err) {
    not_known = false;
    Bytes priv, pub;
    Wipe wp{priv};
    if (!random_bytes(32, priv) || !x25519_public(priv, pub)) {
        err = kErrInternal;
        return false;
    }
    std::vector<Tlv> r;
    if (!post_tlv(link, "/pair-verify", {{kTlvState, one(1)}, {kTlvPublicKey, pub}}, r, err)) return false;
    int st = step_status(r, 2);
    if (st) {   // unsigned: never "not known", whatever the error code says
        err = st < 0 ? kErrGarbled : kErrRefusedSession;
        return false;
    }
    const Bytes *theirs = tlv_find(r, kTlvPublicKey), *enc = tlv_find(r, kTlvEncryptedData);
    if (!theirs || theirs->size() != 32 || !enc) {
        err = kErrGarbled;
        return false;
    }
    const Bytes their_pub = *theirs;
    Bytes shared, plain;
    Wipe ws{shared};
    if (!x25519_shared(priv, their_pub, shared)) {
        err = kErrVerify;
        return false;
    }
    Bytes key = hkdf_sha512(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", 32);
    Wipe wk{key};
    std::vector<Tlv> sub;
    if (key.size() != 32 || !chacha20_poly1305_open(key, label_nonce("PV-Msg02"), Bytes(), *enc, plain) ||
        !tlv_decode(plain, sub)) {
        err = kErrVerify;
        return false;
    }
    const Bytes *id = tlv_find(sub, kTlvIdentifier), *sig = tlv_find(sub, kTlvSignature);
    if (!id || !sig) {
        err = kErrVerify;
        return false;
    }
    if (*id != peer.id) {
        err = kErrWrongAccessory;
        return false;
    }
    if (!ed25519_verify(peer.ltpk, their_pub + *id + pub, *sig)) {
        err = kErrVerify;
        return false;
    }
    Bytes mine, sealed;
    if (!ed25519_sign(me.seed, pub + me.id + their_pub, mine) ||
        !chacha20_poly1305_seal(key, label_nonce("PV-Msg03"), Bytes(),
                                tlv_encode({{kTlvIdentifier, me.id}, {kTlvSignature, mine}}), sealed)) {
        err = kErrInternal;
        return false;
    }
    if (!post_tlv(link, "/pair-verify", {{kTlvState, one(3)}, {kTlvEncryptedData, sealed}}, r, err)) return false;
    st = step_status(r, 4);
    if (st) {   // after a verified M2: Authentication (2) means this controller is not known
        not_known = st == 2;
        err = st < 0 ? kErrGarbled : st == 2 ? kErrNotKnown : kErrRefusedSession;
        return false;
    }
    const Bytes write_key = hkdf_sha512(shared, "Control-Salt", "Control-Write-Encryption-Key", 32);
    const Bytes read_key = hkdf_sha512(shared, "Control-Salt", "Control-Read-Encryption-Key", 32);
    if (write_key.size() != 32 || read_key.size() != 32) {
        err = kErrInternal;
        return false;
    }
    link.secure(write_key, read_key);
    return true;
}

bool parse_json(const Bytes &body, size_t max_values, json &out, std::string &err) {
    if (!parse_reply_json(body, max_values, out)) {
        err = kErrGarbled;
        return false;
    }
    return true;
}

// What a /characteristics answer said about each (aid, iid).
using Id = std::pair<int64_t, int64_t>;
struct CharReply {
    std::map<Id, json> values;          // numbers and booleans
    std::map<Id, std::string> strings;  // text values (accessory information)
    std::map<Id, long> failed;          // a status other than 0
    std::map<Id, std::string> types;    // short types, when asked for with type=1
    std::set<Id> seen;                  // every item with a sound aid and iid
};

// False when the reply has no list of items.
bool collect_values(const json &j, CharReply &out) {
    const json *list = array_of(j, "characteristics");
    if (!list) return false;
    size_t n = 0;
    for (const json &c : *list) {
        if (++n > 4096) break;
        int64_t aid = 0, iid = 0;
        if (!id_of(c, "aid", aid) || !id_of(c, "iid", iid)) continue;
        const Id id{aid, iid};
        out.seen.insert(id);
        const json *type = member(c, "type");
        if (type && type->is_string()) out.types[id] = short_type(type->get<std::string>());
        const json *status = member(c, "status");
        double s = 0;
        if (status && !status->is_null() && !number_of(*status, s)) {   // a status that cannot be read is no success
            out.failed[id] = -1;
            continue;
        }
        if (status && s != 0) {
            out.failed[id] = status_code(s);
            continue;
        }
        const json *v = member(c, "value");
        double d;
        if (v && (v->is_boolean() || (v->is_number() && number_of(*v, d)))) out.values[id] = *v;
        else if (v && v->is_string()) out.strings[id] = v->get<std::string>().substr(0, 4096);
    }
    return true;
}

// GETs the characteristics `ids` in batches; `typed` adds type=1 (each item then names its type).
// False with `err` when the link or the reply fails. `http_status` is the first status other than
// 200 or 207 (the batches stop there).
bool get_characteristics(Link &link, const std::vector<Id> &ids, bool typed, CharReply &out, int &http_status,
                         std::string &err) {
    http_status = 0;
    for (size_t i = 0; i < ids.size(); i += kIdsPerRead) {
        std::string path = "/characteristics?id=";
        for (size_t k = i; k < ids.size() && k < i + kIdsPerRead; k++)
            path += (k > i ? "," : "") + std::to_string(ids[k].first) + "." + std::to_string(ids[k].second);
        if (typed) path += "&type=1";
        HttpResponse r;
        if (!link.exchange("GET", path, "", "", kMaxCharBody, r, err)) return false;
        if (r.status != 200 && r.status != 207) {
            http_status = r.status;
            return true;
        }
        json j;
        if (!parse_json(r.body, kMaxCharValues, j, err)) return false;
        double s = 0;
        const json *global = member(j, "status");
        if (global && number_of(*global, s) && s != 0)   // one error for the whole request
            for (size_t k = i; k < ids.size() && k < i + kIdsPerRead; k++) out.failed[ids[k]] = status_code(s);
        collect_values(j, out);
    }
    return true;
}

bool gone_status(long s) { return s == -70409 || s == 70409; }

std::string display_name(const home::Thing &t) {
    const std::string n = clean_name(t.name);
    return n.empty() ? std::string("That device") : n;
}

std::string changed_text(const std::string &name) {
    return name + " has changed on the accessory since it was last read, so nothing was sent to it. Refresh the "
                  "device in settings.";
}

// The writes for one thing.
struct Planned {
    const home::Thing *thing;
    std::vector<Write> writes;
};

enum class Check { kOk, kStale, kThingFailed, kError };

namespace {
// write_check() stage by stage (see below). What is compared: each id about to be written, with the
// type it was listed with and whether it can be read, and each accessory's model, serial number and
// firmware fingerprints; `full` when the whole database is to be read again.
struct WriteCheck {
    struct Want {
        std::string type;
        size_t thing;
        bool readable;
    };
    struct Info {
        std::string key, hash;
        size_t thing;
    };
    const home::Device &dev;
    const std::vector<Planned> &plan;
    std::string &err;
    std::map<Id, Want> want;
    std::map<Id, Info> infos;
    bool full = false;

    // What each thing's writes and its accessory's information were listed as. kOk to go on.
    Check collect() {
        for (size_t k = 0; k < plan.size(); k++) {
            const home::Thing &t = *plan[k].thing;
            const json *ch = member(t.ref, "c");
            int64_t aid = 0;
            if (!ch || !ch->is_object() || !id_of(t.ref, "aid", aid)) {
                err = display_name(t) + " needs to be refreshed in settings.";
                return Check::kStale;
            }
            for (const Write &w : plan[k].writes) {
                std::string type;
                bool readable = false;
                for (auto it = ch->begin(); it != ch->end(); ++it) {
                    int64_t iid = 0;
                    if (!id_of(it.value(), "i", iid) || iid != w.iid) continue;
                    type = string_of(it.value(), "t");
                    const json *r = member(it.value(), "r");
                    readable = r && r->is_boolean() && r->get<bool>();
                    break;
                }
                if (type.empty()) {   // listed before types were kept
                    err = display_name(t) + " needs to be refreshed in settings.";
                    return Check::kStale;
                }
                want[{w.aid, w.iid}] = Want{type, k, readable};
            }
            if (const json *ais = member(t.ref, "ais"))
                for (const char *key : {"21", "30", "52"}) {
                    const json *e = member(*ais, key);
                    int64_t iid = 0;
                    if (!e || !e->is_array() || e->size() != 2 || !(*e)[1].is_string()) continue;
                    const json pos = {{"i", (*e)[0]}};
                    if (!id_of(pos, "i", iid)) continue;
                    infos[{aid, iid}] = Info{key, (*e)[1].get<std::string>(), k};
                }
            const std::string cn = string_of(t.ref, "cn");
            const auto now_cn = dev.info.find("c#");
            if (!cn.empty() && now_cn != dev.info.end() && !now_cn->second.empty() && now_cn->second != cn) full = true;
        }

        return Check::kOk;
    }

    // 1. The ids read again with their types, and the accessory information. kOk to go on.
    Check typed_read(Link &link) {
        std::vector<Id> ids;
        for (const auto &w : want) ids.push_back(w.first);
        for (const auto &i : infos)
            if (!want.count(i.first)) ids.push_back(i.first);
        CharReply reply;
        int http_status = 0;
        if (!get_characteristics(link, ids, true, reply, http_status, err)) return Check::kError;
        if (http_status == 400) {
            full = true;   // an accessory that does not take type=1: the database tells instead
        } else if (http_status) {
            err = http_error(http_status);
            return Check::kError;
        } else {
            long failed_status = 0;
            size_t failed_thing = 0;
            auto cannot_check = [&](size_t thing, long status) {
                if (!failed_status) failed_status = status, failed_thing = thing;
            };
            for (const auto &w : want) {
                const std::string name = display_name(*plan[w.second.thing].thing);
                const auto ty = reply.types.find(w.first);
                const auto f = reply.failed.find(w.first);
                if (f != reply.failed.end() && gone_status(f->second)) {
                    err = status_text(f->second, name);
                    return Check::kStale;
                }
                if (ty != reply.types.end()) {
                    if (ty->second != w.second.type) {
                        err = changed_text(name);
                        return Check::kStale;
                    }
                    continue;
                }
                if (f == reply.failed.end()) full = true;   // answered without a type
                else if (!w.second.readable && (f->second == -70405 || f->second == 70405)) full = true;   // write-only
                else cannot_check(w.second.thing, f->second);
            }
            for (const auto &i : infos) {
                const std::string name = display_name(*plan[i.second.thing].thing);
                const auto ty = reply.types.find(i.first);
                const auto s = reply.strings.find(i.first);
                const auto f = reply.failed.find(i.first);
                if ((ty != reply.types.end() && ty->second != i.second.key) || (f != reply.failed.end() && gone_status(f->second))) {
                    err = changed_text(name);
                    return Check::kStale;
                }
                if (s != reply.strings.end()) {
                    if (info_hash(s->second) == i.second.hash) continue;
                    if (i.second.key == std::string("52")) {
                        full = true;   // new firmware: the database may have changed with it
                        continue;
                    }
                    err = changed_text(name);   // another accessory now has this place on the bridge
                    return Check::kStale;
                }
                if (f != reply.failed.end()) cannot_check(i.second.thing, f->second);
                else full = true;
            }
            if (failed_status) {
                err = status_text(failed_status, display_name(*plan[failed_thing].thing));
                return Check::kThingFailed;
            }
        }
        return Check::kOk;
    }

    // 2. The whole database read again, and each thing checked against it.
    Check database(Link &link) {
        HttpResponse r;
        if (!link.exchange("GET", "/accessories", "", "", kMaxDatabase, r, err)) return Check::kError;
        if (r.status != 200) {
            err = http_error(r.status);
            return Check::kError;
        }
        json db;
        std::vector<home::Thing> now;
        if (!parse_database(r.body, db) || !things_from_database(db, now)) {
            err = kErrGarbled;
            return Check::kError;
        }
        for (const Planned &p : plan) {
            const home::Thing &t = *p.thing;
            const std::string name = display_name(t);
            const auto cur = std::find_if(now.begin(), now.end(), [&](const home::Thing &x) { return x.id == t.id; });
            if (cur == now.end() || cur->ref.value("k", json()) != t.ref.value("k", json())) {
                err = changed_text(name);
                return Check::kStale;
            }
            const json *was = member(t.ref, "c"), *is = member(cur->ref, "c");
            if (!was || !was->is_object() || !is) {
                err = changed_text(name);
                return Check::kStale;
            }
            for (const Write &w : p.writes)
                for (auto it = was->begin(); it != was->end(); ++it) {
                    int64_t iid = 0;
                    if (!id_of(it.value(), "i", iid) || iid != w.iid) continue;
                    const json *spec = member(*is, it.key().c_str());
                    int64_t now_iid = 0;
                    if (!spec || !id_of(*spec, "i", now_iid) || now_iid != iid ||
                        string_of(*spec, "t") != string_of(it.value(), "t")) {
                        err = changed_text(name);
                        return Check::kStale;
                    }
                }
            const json *was_ais = member(t.ref, "ais"), *is_ais = member(cur->ref, "ais");
            for (const char *key : {"21", "30"}) {
                const json *a = was_ais ? member(*was_ais, key) : nullptr, *b = is_ais ? member(*is_ais, key) : nullptr;
                if (a && a->is_array() && a->size() == 2 && (!b || !b->is_array() || b->size() != 2 || (*a)[1] != (*b)[1])) {
                    err = changed_text(name);
                    return Check::kStale;
                }
            }
        }
        return Check::kOk;
    }
};
}  // namespace

// Before anything is written: is every id still what the thing was made from? An accessory whose
// database changed (a firmware update, an accessory replaced on a bridge) may reuse ids, and a
// value written to an id that now means something else could unlock a lock. So, inside the
// verified session:
//  1. GET each id about to be written with type=1, and each accessory's model, serial number and
//     firmware revision. A different type, a vanished id (-70409) or another model or serial
//     number refuses the whole write: kStale.
//  2. The whole database is read again and each thing checked against it (still listed, so its
//     accessory has no denied service; same kind; same characteristic at every id written; same
//     model and serial number) when the firmware revision changed, when discovery's c# differs
//     from the one enumerate saw, or when the accessory gave no types.
// kThingFailed: one thing could not be checked now (its bridged accessory is off, say); `err`
// says so about it. kError: the link or a reply failed.
Check write_check(Link &link, const home::Device &dev, const std::vector<Planned> &plan, std::string &err) {
    WriteCheck w{dev, plan, err, {}, {}, false};
    Check check = w.collect();
    if (check == Check::kOk) check = w.typed_read(link);
    if (check != Check::kOk) return check;
    if (!w.full) return Check::kOk;
    return w.database(link);
}

// =============================================================================================
// The driver
// =============================================================================================
class HomekitDriver final : public home::Driver {
public:
    const char *kind() const override { return "homekit"; }

    bool claims(const std::string &device_kind, const std::map<std::string, std::string> &info) const override {
        if (device_kind != "homekit") return false;
        // Categories (aiohomekit model/categories.py, HAP-NodeJS Accessory.ts) that can only be
        // things this app never exposes, or that offer no service on the allow-list (pairing one
        // would use up its single HomeKit pairing for nothing), and Apple's own devices: garage
        // door 4, lock 6, security system 11, door 12, window 13, programmable switch 15 (buttons),
        // range extender 16, camera 17, doorbell 18, air purifier 19 (its service, BB, is not on
        // the allow-list), humidifier 22 and dehumidifier 23 (HumidifierDehumidifier, BD, is not
        // either; its humidity reading is a characteristic inside it, not a HumiditySensor
        // service), Apple TV 24, HomePod 25, speaker 26, AirPort 27, sprinkler 28, faucet 29,
        // shower head 30, television 31, remote 32, router 33, audio receiver 34, set-top box
        // 35, streaming stick 36. TVs and speakers have their own drivers (Android TV, Roku,
        // Cast, Sonos), which discovery would otherwise never reach: it ranks homekit above them
        // (DISCOVERY.md). The same devices behind a bridge (category 2) are filtered by service.
        const long ci = info_number(info, "ci", 0);
        for (const long no : {4L, 6L, 11L, 12L, 13L, 15L, 16L, 17L, 18L, 19L, 22L, 23L, 24L, 25L, 26L, 27L, 28L, 29L,
                              30L, 31L, 32L, 33L, 34L, 35L, 36L})
            if (ci == no) return false;
        // A Hue Bridge (model BSB001, BSB002, BSB003: "md" in its HomeKit announcement) has a driver
        // of its own, which also offers its scenes; discovery lists it under that kind whenever its
        // own announcement was heard, and this keeps a scan that heard only HomeKit from offering it.
        const auto md = info.find("md");
        if (md != info.end() && md->second.size() == 6) {
            std::string model = md->second;
            for (char &c : model) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (model.compare(0, 5, "BSB00") == 0) return false;
        }
        return true;
    }

    home::PairStep next_step(const home::Device &dev) const override {
        home::PairStep step;
        try {
            Identity me;
            Peer peer;
            std::string controller;
            const bool have_peer = load_peer(dev, peer, controller);
            const int have_me = load_identity(secrets_, me);
            if (have_peer && have_me == 1 && controller == me.id) {
                bool elsewhere = false;
                if (!forgotten_by_accessory(dev, &elsewhere)) return step;   // paired with this app
                step.kind = home::Pairing::EnterCode;   // ... but the accessory has since forgotten it
                step.field = kField;
                // and when a new pairing was tried, another controller had it by then
                step.prompt = elsewhere ? kPromptPairedElsewhere : kPromptNotKnown;
                return step;
            }
            step.kind = home::Pairing::EnterCode;
            step.field = kField;
            if (have_peer) step.prompt = kPromptKeyChanged;
            else if (paired_elsewhere(dev)) step.prompt = kPromptPairedElsewhere;
            else step.prompt = kPromptCode;
        } catch (...) {
            step.kind = home::Pairing::EnterCode;
            step.field = kField;
            step.prompt = kPromptCode;
        }
        return step;
    }

    bool pair(home::Device &dev, const std::string &input, std::string &err) override {
        try {
            err.clear();
            std::string code;
            if (!parse_setup_code(input, code, err)) return false;
            Identity me;
            Peer peer;
            std::string controller;
            bool repairing = false;   // a new pairing for one the accessory forgot
            int have = load_identity(secrets_, me);
            if (have < 0) {
                err = kErrBadStore;
                return false;
            }
            if (have == 1 && load_peer(dev, peer, controller) && controller == me.id) {
                // Paired with this app already. Ask the accessory: when it still knows this app,
                // there is nothing to do; when it answers, after proving its identity, that it
                // does not (its pairing was reset, or it was unpaired elsewhere), a new pairing is
                // made below. Any other failure (no answer, another identity, a bad signature, an
                // unsigned refusal, garbage) sends nothing more: the code goes to no one. discovery's
                // sf=0 is not trusted here (it was this app's own pairing): pair-setup M2 says
                // Unavailable if another controller has the accessory, before the code is sent.
                std::string verr;
                bool forgotten = false;
                {
                    Link link(Clock::now() + std::chrono::milliseconds(kReadMs));
                    if (link.open(dev, verr) && pair_verify(link, me, peer, forgotten, verr)) {
                        clear_forgotten(dev);
                        return true;
                    }
                }
                if (!forgotten) {
                    err = verr;
                    return false;
                }
                set_forgotten(dev);
                repairing = true;
            } else if (paired_elsewhere(dev)) {
                err = kErrPairedElsewhere;
                return false;
            }
            const std::string key = memory_key(dev);
            const std::string code_id = hex(sha512(key + "\n" + code)).substr(0, 32);
            if (refused_locally(key, code_id, err)) return false;
            if (have == 0) {
                std::lock_guard<std::mutex> lock(identity_mu_);   // two pairings at once make one key
                have = load_identity(secrets_, me);
                if (have == 0 && !create_identity(secrets_, me)) {
                    err = kErrNoStore;
                    return false;
                }
                if (have < 0) {
                    err = kErrBadStore;
                    return false;
                }
            }
            Link link(Clock::now() + std::chrono::milliseconds(kPairMs));
            if (!link.open(dev, err)) return false;
            SetupFail why = SetupFail::kOther;
            uint64_t retry_s = 0;
            const int method = info_number(dev.info, "ff", 0) & 1 ? 1 : 0;   // Pair Setup with Auth for MFi chips
            if (!pair_setup(link, code, method, me, peer, why, retry_s, err)) {
                // Another controller has the accessory now: next_step says so until a pairing works.
                if (repairing) set_taken_elsewhere(dev, why == SetupFail::kUnavailable);
                std::lock_guard<std::mutex> lock(mu_);
                Memory &m = memory_[key];
                if (why == SetupFail::kWrongCode) m.refused.insert(code_id);
                if (why == SetupFail::kMaxTries) m.locked = true;
                if (why == SetupFail::kBackoff) m.wait_until = Clock::now() + std::chrono::seconds(retry_s ? retry_s : 60);
                return false;
            }
            clear_forgotten(dev);
            if (!dev.auth.is_object()) dev.auth = json::object();
            dev.auth["accessory_id"] = peer.id;
            dev.auth["accessory_ltpk"] = hex(peer.ltpk);
            dev.auth["controller_id"] = me.id;
            std::lock_guard<std::mutex> lock(mu_);
            memory_.erase(key);
            return true;
        } catch (...) {
            err = kErrInternal;
            return false;
        }
    }

    void unpair(home::Device &dev) override {
        try {
            Identity me;
            Peer peer;
            std::string controller;
            if (!load_peer(dev, peer, controller)) {
                forget_pairing(dev);
                return;
            }
            if (load_identity(secrets_, me) != 1 || controller != me.id) {
                forget_pairing(dev);   // this app cannot speak for that pairing any more
                return;
            }
            const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(kUnpairMs);
            std::string err;
            bool removed = false, unsure = false;
            {
                Link link(deadline);
                bool not_known = false, sent = false;
                if (link.open(dev, err)) {
                    if (pair_verify(link, me, peer, not_known, err)) {
                        std::vector<Tlv> r;
                        if (post_tlv(link, "/pairings",
                                     {{kTlvState, one(1)}, {kTlvMethod, one(4)}, {kTlvIdentifier, me.id}}, r, err, &sent))
                            removed = step_status(r, 2) == 0;
                        else
                            unsure = sent;
                    } else {
                        removed = not_known;   // it already forgot this controller
                    }
                }
            }
            if (unsure) {
                // An accessory may close the connection as soon as the pairing is gone, before it
                // answers: a fresh pair-verify tells.
                Link link(deadline);
                bool not_known = false;
                if (link.open(dev, err) && !pair_verify(link, me, peer, not_known, err)) removed = not_known;
            }
            // Not removed (the accessory could not be reached): the pairing is kept in dev.auth so
            // that Unpair can be tried again. (The app stores dev.auth as left here: control.cpp,
            // home::unpair.)
            if (removed) {
                forget_pairing(dev);
                clear_forgotten(dev);
                // It has no controller now: until the next scan says so, the saved discovery
                // record must not still say "paired" (that would block pairing it again).
                if (dev.info.count("sf")) {
                    int sf = 0;
                    try { sf = std::stoi(dev.info["sf"]); } catch (...) {}
                    dev.info["sf"] = std::to_string(sf | 1);
                }
            }
        } catch (...) {
        }
    }

    bool enumerate(home::Device &dev, std::vector<home::Thing> &out, std::string &err) override {
        try {
            err.clear();
            json db;
            if (!session(dev, kEnumerateMs, err, [&](Link &link, std::string &e) {
                    HttpResponse r;
                    if (!link.exchange("GET", "/accessories", "", "", kMaxDatabase, r, e)) return false;
                    if (r.status != 200) {
                        e = http_error(r.status);
                        return false;
                    }
                    if (!parse_database(r.body, db)) {
                        e = kErrGarbled;
                        return false;
                    }
                    return true;
                }))
                return false;
            std::vector<home::Thing> things;
            if (!things_from_database(db, things)) {
                err = kErrGarbled;
                return false;
            }
            // Discovery's configuration number (TXT c#) as this list was made: a write checks the
            // whole database again when a later scan reports another one (write_check).
            const auto cn = dev.info.find("c#");
            if (cn != dev.info.end() && !cn->second.empty() && cn->second.size() <= 16)
                for (home::Thing &t : things) t.ref["cn"] = cn->second;
            out = std::move(things);
            return true;
        } catch (...) {
            err = kErrInternal;
            return false;
        }
    }

    bool read(const home::Device &dev, std::vector<home::Thing> &things, std::string &err) override {
        try {
            err.clear();
            if (things.empty()) things = dev.things;
            std::vector<std::pair<int64_t, int64_t>> ids;
            for (const home::Thing &t : things) {
                int64_t aid = 0;
                const json *ch = member(t.ref, "c");
                if (!id_of(t.ref, "aid", aid) || !ch || !ch->is_object()) continue;
                for (auto it = ch->begin(); it != ch->end(); ++it) {
                    int64_t iid = 0;
                    const json *r = member(it.value(), "r");
                    if (id_of(it.value(), "i", iid) && r && r->is_boolean() && r->get<bool>() &&
                        std::find(ids.begin(), ids.end(), std::make_pair(aid, iid)) == ids.end())
                        ids.emplace_back(aid, iid);
                }
            }
            if (ids.empty()) return true;
            CharReply reply;
            if (!session(dev, kReadMs, err, [&](Link &link, std::string &e) {
                    int http_status = 0;
                    if (!get_characteristics(link, ids, false, reply, http_status, e)) return false;
                    if (http_status) {
                        e = http_error(http_status);
                        return false;
                    }
                    return true;
                }))
                return false;
            std::vector<Id> failed;
            for (const auto &f : reply.failed) failed.push_back(f.first);
            for (home::Thing &t : things) apply_values(t, reply.values, failed);
            // Nothing at all could be read: an error, so that a status asked for by name says why,
            // instead of an empty state.
            if (std::none_of(ids.begin(), ids.end(), [&](const Id &id) { return reply.values.count(id) > 0; })) {
                const auto first = std::find_if(ids.begin(), ids.end(), [&](const Id &id) { return reply.failed.count(id) > 0; });
                err = first == ids.end() ? std::string(kErrGarbled)
                                         : status_text(reply.failed[*first], things.size() == 1 ? clean_name(things[0].name) : "");
                return false;
            }
            return true;
        } catch (...) {
            err = kErrInternal;
            return false;
        }
    }

    bool apply(const home::Device &dev, const home::Thing &thing, const home::Change &change, int deadline_ms,
               std::string &err) override {
        return write(dev, {&thing}, change, deadline_ms, err);
    }

    // One PUT for every thing. False with a sentence in `err` when none of them was changed (the
    // same reason holds for all: the accessory refused the whole request, it has changed since
    // it was listed, it could not be reached...). False with an EMPTY `err` when the things fared
    // differently (one bridged lamp is switched off at the wall, say): the app then applies the
    // change to each thing on its own (control.cpp apply_device), and each gets its own result.
    // That repeats writes that already worked, which is harmless: every value is absolute.
    bool apply_many(const home::Device &dev, const std::vector<const home::Thing *> &things, const home::Change &change,
                    int deadline_ms, std::string &err) override {
        return write(dev, things, change, deadline_ms, err);
    }

private:
    struct Memory {
        std::set<std::string> refused;   // hashes of codes the accessory rejected
        Clock::time_point wait_until{};
        bool locked = false;
    };

    static std::string memory_key(const home::Device &dev) {
        return !dev.id.empty() ? dev.id : dev.ip + ":" + std::to_string(dev.port);
    }

    // Accessories that answered pair-verify M4 with an Authentication error, after proving their
    // identity in M2: they no longer know this app (their pairing was reset, or it was unpaired
    // elsewhere). next_step then asks for the setup code again, and pair() makes a new pairing.
    // Kept per device for the life of the driver object, and only while the device's saved
    // pairing is the one that was refused. `elsewhere`: the last new pairing tried was answered
    // Unavailable (another controller has the accessory now), so next_step says that instead.
    struct Forgotten {
        std::string ltpk;   // the refused pairing's accessory_ltpk
        bool elsewhere = false;
    };
    bool forgotten_by_accessory(const home::Device &dev, bool *elsewhere = nullptr) const {
        std::lock_guard<std::mutex> lock(forgotten_mu_);
        const auto it = forgotten_.find(memory_key(dev));
        const bool yes = it != forgotten_.end() && it->second.ltpk == string_of(dev.auth, "accessory_ltpk");
        if (elsewhere) *elsewhere = yes && it->second.elsewhere;
        return yes;
    }
    void set_forgotten(const home::Device &dev) const {   // keeps what is known if it was already
        std::lock_guard<std::mutex> lock(forgotten_mu_);
        const std::string ltpk = string_of(dev.auth, "accessory_ltpk");
        Forgotten &f = forgotten_[memory_key(dev)];
        if (f.ltpk != ltpk) f = Forgotten{ltpk, false};
    }
    void set_taken_elsewhere(const home::Device &dev, bool yes) const {
        std::lock_guard<std::mutex> lock(forgotten_mu_);
        const auto it = forgotten_.find(memory_key(dev));
        if (it != forgotten_.end() && it->second.ltpk == string_of(dev.auth, "accessory_ltpk")) it->second.elsewhere = yes;
    }
    void clear_forgotten(const home::Device &dev) const {
        std::lock_guard<std::mutex> lock(forgotten_mu_);
        forgotten_.erase(memory_key(dev));
    }

    bool refused_locally(const std::string &key, const std::string &code_id, std::string &err) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = memory_.find(key);
        if (it == memory_.end()) return false;
        const Memory &m = it->second;
        if (m.locked) {
            err = kErrLocked;
            return true;
        }
        const auto now = Clock::now();
        if (m.wait_until > now) {
            const long long s = std::chrono::duration_cast<std::chrono::seconds>(m.wait_until - now).count() + 1;
            err = "The accessory asked to wait before another try. Try again in " + std::to_string(s) + " seconds.";
            return true;
        }
        if (m.refused.count(code_id)) {
            err = kErrRefusedBefore;
            return true;
        }
        return false;
    }

    // Opens a verified session with a paired accessory and runs `work` in it.
    bool session(const home::Device &dev, int ms, std::string &err,
                 const std::function<bool(Link &, std::string &)> &work) const {
        Identity me;
        Peer peer;
        std::string controller;
        const int have = load_identity(secrets_, me);
        if (have < 0) {
            err = kErrBadStore;
            return false;
        }
        if (!load_peer(dev, peer, controller)) {
            err = kErrNotPaired;
            return false;
        }
        if (have == 0 || controller != me.id) {
            err = kErrKeyChanged;
            return false;
        }
        Link link(Clock::now() + std::chrono::milliseconds(ms));
        bool forgotten = false;
        if (!link.open(dev, err)) return false;
        if (!pair_verify(link, me, peer, forgotten, err)) {
            if (forgotten) set_forgotten(dev);
            return false;
        }
        clear_forgotten(dev);
        return work(link, err);
    }

    bool write(const home::Device &dev, const std::vector<const home::Thing *> &things, const home::Change &change,
               int deadline_ms, std::string &err) {
        try {
            err.clear();
            std::vector<Planned> plan;
            std::map<Id, std::string> owner;
            json body = {{"characteristics", json::array()}};
            for (const home::Thing *t : things) {
                if (!t) continue;
                Planned p{t, {}};
                if (!writes_for(*t, change, p.writes, err)) {
                    // One of several cannot take this change: an empty error hands them back to
                    // the app one by one, so the others are still changed and this one says why.
                    if (things.size() > 1) err.clear();
                    return false;
                }
                for (const Write &x : p.writes) {
                    owner[{x.aid, x.iid}] = display_name(*t);
                    body["characteristics"].push_back({{"aid", x.aid}, {"iid", x.iid}, {"value", x.value}});
                }
                plan.push_back(std::move(p));
            }
            if (body["characteristics"].empty()) {
                err = kErrNothing;
                return false;
            }
            const bool several = plan.size() > 1;
            bool each_alone = false;   // the things fared differently: apply_many hands them back one by one
            const int ms = deadline_ms > 0 ? std::min(deadline_ms, kMaxApplyMs) : kApplyMs;
            const bool ok = session(dev, ms, err, [&](Link &link, std::string &e) {
                const Check check = write_check(link, dev, plan, e);
                if (check == Check::kThingFailed && several) each_alone = true;
                if (check != Check::kOk) return false;
                HttpResponse r;
                if (!link.exchange("PUT", "/characteristics", "application/hap+json", body.dump(), kMaxCharBody, r, e))
                    return false;
                if (r.status == 204) return true;
                if (r.status != 200 && r.status != 207) {
                    e = http_error(r.status);
                    return false;
                }
                // A Multi-Status (207) must say how the written items fared. One with no body, no
                // list of items, or a list about none of them is not understood: never success,
                // and never an empty err (which the app reads as "apply each thing alone").
                if (r.body.empty()) {
                    if (r.status == 200) return true;
                    e = kErrGarbled;
                    return false;
                }
                json j;
                if (!parse_json(r.body, kMaxCharValues, j, e)) return false;
                double s = 0;
                const json *global = member(j, "status");
                if (global && number_of(*global, s) && s != 0) {   // one error for the whole request
                    e = status_text(status_code(s), several ? "" : display_name(*plan[0].thing));
                    return false;
                }
                CharReply reply;
                const bool listed = collect_values(j, reply);
                if (r.status == 207 &&
                    (!listed || std::none_of(reply.seen.begin(), reply.seen.end(), [&](const Id &id) { return owner.count(id) > 0; }))) {
                    e = kErrGarbled;
                    return false;
                }
                if (reply.failed.empty()) return true;
                if (several) {
                    each_alone = true;
                    return false;
                }
                const auto first = reply.failed.begin();
                const auto o = owner.find(first->first);
                e = status_text(first->second, o == owner.end() ? "" : o->second);
                return false;
            });
            if (!ok && each_alone) err.clear();
            return ok;
        } catch (...) {
            err = kErrInternal;
            return false;
        }
    }

    std::mutex mu_;            // memory_
    std::map<std::string, Memory> memory_;
    std::mutex identity_mu_;   // creating this controller's key
    mutable std::mutex forgotten_mu_;   // forgotten_
    mutable std::map<std::string, Forgotten> forgotten_;   // by memory_key
};
}  // namespace

namespace detail {
std::vector<std::string> answered_error_texts() {
    std::vector<std::string> out = {kErrNotCode, kErrQrNoCode, kErrPairedElsewhere, kErrWrongCode, kErrRefusedBefore,
                                    kErrLocked, kErrBusy, kErrMaxPeers, kErrPairFailed, kErrFakeAccessory,
                                    kErrBadIdentity, kErrNoStore, kErrBadStore, kErrNotPaired, kErrKeyChanged,
                                    kErrNoAddress, kErrGarbled, kErrDecrypt, kErrNotKnown, kErrVerify,
                                    kErrRefusedSession, kErrInternal, kErrNothing, changed_text("Hall Lamp")};
    for (const int status : {400, 403, 404, 470, 500, 503}) out.push_back(http_error(status));
    return out;
}

bool time_pair_verify(const home::Device &dev, home::Secrets *secrets, int runs, std::vector<double> &ms, std::string &err) {
    try {
        Identity me;
        Peer peer;
        std::string controller;
        if (load_identity(secrets, me) != 1 || !load_peer(dev, peer, controller) || controller != me.id) {
            err = kErrNotPaired;
            return false;
        }
        for (int i = 0; i < runs; i++) {
            const auto t0 = Clock::now();
            Link link(t0 + std::chrono::milliseconds(kReadMs));
            bool not_known = false;
            if (!link.open(dev, err) || !pair_verify(link, me, peer, not_known, err)) return false;
            ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        }
        return true;
    } catch (...) {
        err = kErrInternal;
        return false;
    }
}
}  // namespace detail
}  // namespace homekit

std::unique_ptr<home::Driver> make_homekit_driver() { return std::make_unique<homekit::HomekitDriver>(); }
