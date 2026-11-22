// Local control of TP-Link Tapo smart plugs over KLAP v2 and TPAP. See tapo.h.
//
// Written from public sources: KLAP and the commands as python-kasa (GPL-3.0, read for protocol
// facts only) and the `tapo` crate use them, SPAKE2+ from RFC 9383. The TPAP login follows
// the MIT-licensed `tapo` Rust crate (github.com/mihai-dinculescu/tapo, v0.13.0): the SPAKE2+
// prover is a port of tapo/src/api/protocol/tpap_spake2p.rs, and the checks on the device's
// answers follow tapo/src/api/protocol/tpap_protocol.rs (its notice is in
// licenses/THIRD-PARTY-NOTICES.md). No code is taken from python-kasa.
#include "smarthome/tapo.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <curl/curl.h>
#include <netdb.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/util.h"
#include "smarthome/drivers/support.h"

namespace tapo {

// ---------------------------------------------------------------------------------------------
// Building blocks
// ---------------------------------------------------------------------------------------------
namespace detail {

const unsigned char kDiscoveryQuery[16] = {0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00, 0x46, 0x3c, 0xb5, 0xd3};

namespace {
const unsigned char *u8(const Bytes &b) { return reinterpret_cast<const unsigned char *>(b.data()); }
unsigned char *u8(Bytes &b) { return reinterpret_cast<unsigned char *>(&b[0]); }

Bytes be32(int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    const char b[4] = {char(u >> 24), char(u >> 16), char(u >> 8), char(u)};
    return Bytes(b, 4);
}

bool same(const Bytes &a, const Bytes &b) {   // constant-time for equal lengths
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

template <class T, void (*F)(T *)> struct Del {
    void operator()(T *p) const { F(p); }
};
using Bn = std::unique_ptr<BIGNUM, Del<BIGNUM, BN_clear_free>>;
using BnCtx = std::unique_ptr<BN_CTX, Del<BN_CTX, BN_CTX_free>>;
using Group = std::unique_ptr<EC_GROUP, Del<EC_GROUP, EC_GROUP_free>>;
using Point = std::unique_ptr<EC_POINT, Del<EC_POINT, EC_POINT_clear_free>>;
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, Del<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>>;
}  // namespace

std::string hex(const Bytes &b) { return support::hex(b); }

// On failure `out` is left as it was (it used to be cleared and part filled): every caller gives up
// on a false and never reads it.
bool unhex(const std::string &h, Bytes &out) { return support::unhex(h, out); }

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64(const Bytes &b) { return util::base64(b); }

bool unb64(const std::string &s, Bytes &out) {
    out.clear();
    size_t len = s.size();
    while (len && s[len - 1] == '=') len--;
    if (s.size() - len > 2 || len % 4 == 1) return false;
    uint32_t v = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        const char *p = s[i] ? strchr(kB64, s[i]) : nullptr;
        if (!p) return false;
        v = v << 6 | static_cast<uint32_t>(p - kB64);
        if ((bits += 6) >= 8) out += char(v >> (bits -= 8) & 255);
    }
    return true;
}

Bytes md5(const Bytes &b) { return support::digest(EVP_md5(), b); }
Bytes sha1(const Bytes &b) { return support::digest(EVP_sha1(), b); }
Bytes sha256(const Bytes &b) { return support::digest(EVP_sha256(), b); }

Bytes hmac_sha256(const Bytes &key, const Bytes &data) { return support::hmac("SHA256", key, data); }

// RFC 5869. An empty salt stands for a block of zeros.
Bytes hkdf_sha256(const Bytes &ikm, const Bytes &salt, const Bytes &info, size_t len) {
    const Bytes prk = hmac_sha256(salt.empty() ? Bytes(32, '\0') : salt, ikm);
    Bytes okm, t;
    for (unsigned char i = 1; okm.size() < len && i < 255 && !prk.empty(); i++) {
        t = hmac_sha256(prk, t + info + char(i));
        okm += t;
    }
    okm.resize(len);
    return okm;
}

Bytes pbkdf2_sha256(const Bytes &pass, const Bytes &salt, int iterations, size_t len) {
    Bytes out(len, '\0');
    if (!PKCS5_PBKDF2_HMAC(pass.data(), static_cast<int>(pass.size()), u8(salt), static_cast<int>(salt.size()),
                           iterations, EVP_sha256(), static_cast<int>(len), u8(out)))
        out.clear();
    return out;
}

bool aes128_cbc(bool encrypt, const Bytes &key, const Bytes &iv, const Bytes &in, Bytes &out) {
    out.clear();
    Cipher c(EVP_CIPHER_CTX_new());
    if (!c || key.size() != 16 || iv.size() != 16) return false;
    Bytes buf(in.size() + 16, '\0');
    int n = 0, m = 0;
    if (!EVP_CipherInit_ex(c.get(), EVP_aes_128_cbc(), nullptr, u8(key), u8(iv), encrypt ? 1 : 0) ||
        !EVP_CipherUpdate(c.get(), u8(buf), &n, u8(in), static_cast<int>(in.size())) ||
        !EVP_CipherFinal_ex(c.get(), u8(buf) + n, &m))   // fails on bad PKCS#7 padding
        return false;
    out.assign(buf, 0, static_cast<size_t>(n + m));
    return true;
}

// CCM in OpenSSL: the nonce and tag lengths go in before the key, and the total length of the
// text is announced with an output-less update before any data.
static bool ccm(bool seal, const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &in,
                const Bytes &tag_in, size_t tag_len, Bytes &out, Bytes &tag_out) {
    Cipher c(EVP_CIPHER_CTX_new());
    if (!c || key.size() != 16 || in.empty()) return false;
    int n = 0;
    Bytes tag = tag_in;
    if (!EVP_CipherInit_ex(c.get(), EVP_aes_128_ccm(), nullptr, nullptr, nullptr, seal ? 1 : 0) ||
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) <= 0 ||
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tag_len), seal ? nullptr : u8(tag)) <= 0 ||
        !EVP_CipherInit_ex(c.get(), nullptr, nullptr, u8(key), u8(nonce), seal ? 1 : 0) ||
        !EVP_CipherUpdate(c.get(), nullptr, &n, nullptr, static_cast<int>(in.size())))
        return false;
    if (!aad.empty() && !EVP_CipherUpdate(c.get(), nullptr, &n, u8(aad), static_cast<int>(aad.size()))) return false;
    out.assign(in.size(), '\0');
    // When opening, this update is where the tag is checked.
    if (EVP_CipherUpdate(c.get(), u8(out), &n, u8(in), static_cast<int>(in.size())) <= 0 ||
        static_cast<size_t>(n) != in.size()) {
        out.clear();
        return false;
    }
    if (!seal) return true;
    tag_out.assign(tag_len, '\0');
    int m = 0;
    return EVP_CipherFinal_ex(c.get(), u8(out) + n, &m) &&
           EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(tag_len), u8(tag_out)) > 0;
}

bool aes128_ccm_seal(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &plain, size_t tag_len, Bytes &out) {
    Bytes tag;
    if (!ccm(true, key, nonce, aad, plain, Bytes(), tag_len, out, tag)) return false;
    out += tag;
    return true;
}

bool aes128_ccm_open(const Bytes &key, const Bytes &nonce, const Bytes &aad, const Bytes &sealed, size_t tag_len, Bytes &plain) {
    plain.clear();
    if (sealed.size() <= tag_len) return false;
    Bytes unused;
    return ccm(false, key, nonce, aad, sealed.substr(0, sealed.size() - tag_len), sealed.substr(sealed.size() - tag_len),
               tag_len, plain, unused);
}

// --- KLAP v2 ---------------------------------------------------------------------------------
// The account is known to both sides as SHA256(SHA1(email) || SHA1(password)), raw digests.

Bytes klap_auth_hash(const std::string &email, const Bytes &password_sha1) { return sha256(sha1(email) + password_sha1); }

Bytes klap_handshake1_hash(const Bytes &local_seed, const Bytes &remote_seed, const Bytes &auth_hash) {
    return sha256(local_seed + remote_seed + auth_hash);   // what the plug proves itself with
}

Bytes klap_handshake2_hash(const Bytes &local_seed, const Bytes &remote_seed, const Bytes &auth_hash) {
    return sha256(remote_seed + local_seed + auth_hash);   // what the client proves itself with
}

KlapKeys klap_derive(const Bytes &local_seed, const Bytes &remote_seed, const Bytes &auth_hash) {
    const Bytes s = local_seed + remote_seed + auth_hash;
    const Bytes ivh = sha256("iv" + s);
    KlapKeys k;
    k.key = sha256("lsk" + s).substr(0, 16);
    k.iv = ivh.substr(0, 12);
    k.sig = sha256("ldk" + s).substr(0, 28);
    uint32_t seq = 0;   // the last four bytes of the IV hash, big-endian and signed
    for (size_t i = 28; i < 32 && i < ivh.size(); i++) seq = seq << 8 | static_cast<unsigned char>(ivh[i]);
    k.seq = static_cast<int32_t>(seq);
    return k;
}

// body = SHA256(sig || seq || ciphertext) || ciphertext, with the IV ending in the sequence number.
Bytes klap_seal(const KlapKeys &k, int32_t seq, const std::string &plain) {
    Bytes ct;
    if (!aes128_cbc(true, k.key, k.iv + be32(seq), plain, ct)) return Bytes();
    return sha256(k.sig + be32(seq) + ct) + ct;
}

// The plug answers in the same framing under the request's sequence number. Its signature is
// not checked: neither reference client does, so how the firmware computes it is unconfirmed.
bool klap_open(const KlapKeys &k, int32_t seq, const Bytes &body, std::string &plain) {
    plain.clear();
    if (body.size() < 48 || (body.size() - 32) % 16) return false;
    return aes128_cbc(false, k.key, k.iv + be32(seq), body.substr(32), plain);
}

// --- TPAP ------------------------------------------------------------------------------------

namespace {
// The points M and N that RFC 9383 assigns to P-256, compressed.
const char kM[] = "02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f";
const char kN[] = "03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49";

// A point of the curve from its SEC1 encoding; null for the identity or anything off the curve.
Point point_from(const EC_GROUP *g, const Bytes &b, BN_CTX *ctx) {
    Point p(EC_POINT_new(g));
    if (!p || b.empty() || !EC_POINT_oct2point(g, p.get(), u8(b), b.size(), ctx) ||
        EC_POINT_is_at_infinity(g, p.get()) || EC_POINT_is_on_curve(g, p.get(), ctx) != 1)
        return nullptr;
    return p;
}

Bytes uncompressed(const EC_GROUP *g, const EC_POINT *p, BN_CTX *ctx) {
    unsigned char buf[65];
    const size_t n = EC_POINT_point2oct(g, p, POINT_CONVERSION_UNCOMPRESSED, buf, sizeof buf, ctx);
    return n == sizeof buf ? Bytes(reinterpret_cast<char *>(buf), n) : Bytes();
}

// One transcript item: its length as 8 little-endian bytes, then the item.
Bytes item(const Bytes &v) {
    Bytes out;
    for (int i = 0; i < 8; i++) out += char(static_cast<uint64_t>(v.size()) >> (8 * i) & 255);
    return out + v;
}
}  // namespace

// d = PBKDF2-HMAC-SHA256(credential, salt, iterations, 80); each 40-byte half reduced modulo the
// group order gives w0 and w1.
bool spake2p_w0_w1(const std::string &credential, const Bytes &salt, int iterations, Bytes &w0, Bytes &w1) {
    Bytes d = pbkdf2_sha256(credential, salt, iterations, 80);
    Group g(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
    BnCtx ctx(BN_CTX_new());
    if (d.size() != 80 || !g || !ctx) return false;
    Bytes *out[2] = {&w0, &w1};
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        Bn v(BN_bin2bn(u8(d) + 40 * i, 40, nullptr));
        out[i]->assign(32, '\0');
        ok = v && BN_nnmod(v.get(), v.get(), EC_GROUP_get0_order(g.get()), ctx.get()) &&
             BN_bn2binpad(v.get(), u8(*out[i]), 32) == 32;
    }
    OPENSSL_cleanse(&d[0], d.size());
    return ok;
}

// Ported from prove_with() in the tapo crate's tpap_spake2p.rs. Names follow RFC 9383:
//   X = x*G + w0*M   (the prover's share)       Y = the verifier's share
//   Z = x*(Y - w0*N)                            V = w1*(Y - w0*N)
// Every multiplication takes a single scalar, which keeps OpenSSL on its constant-time ladder.
bool spake2p_prove(const Bytes &w0b, const Bytes &w1b, const Bytes &xb, const Bytes &context,
                   const Bytes &id_prover, const Bytes &id_verifier, const Bytes &verifier_share,
                   SpakeProof &out, std::string &err) {
    err = "the key exchange could not be computed";
    Group grp(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
    BnCtx ctx(BN_CTX_new());
    if (!grp || !ctx || w0b.size() != 32 || w1b.size() != 32) return false;
    const EC_GROUP *g = grp.get();
    const BIGNUM *order = EC_GROUP_get0_order(g);
    Bn w0(BN_bin2bn(u8(w0b), 32, nullptr)), w1(BN_bin2bn(u8(w1b), 32, nullptr));
    Bn x(xb.empty() ? BN_new() : BN_bin2bn(u8(xb), static_cast<int>(xb.size()), nullptr));
    if (!w0 || !w1 || !x) return false;
    if (xb.empty()) {
        do {
            if (!BN_priv_rand_range(x.get(), order)) return false;
        } while (BN_is_zero(x.get()));
    } else if (BN_is_zero(x.get()) || BN_cmp(x.get(), order) >= 0) {
        return false;
    }

    Bytes mb, nb;
    unhex(kM, mb);
    unhex(kN, nb);
    Point M = point_from(g, mb, ctx.get()), N = point_from(g, nb, ctx.get());
    Point a(EC_POINT_new(g)), b(EC_POINT_new(g)), X(EC_POINT_new(g)), Z(EC_POINT_new(g)), V(EC_POINT_new(g));
    if (!M || !N || !a || !b || !X || !Z || !V) return false;

    if (!EC_POINT_mul(g, a.get(), x.get(), nullptr, nullptr, ctx.get()) ||       // x*G
        !EC_POINT_mul(g, b.get(), nullptr, M.get(), w0.get(), ctx.get()) ||      // w0*M
        !EC_POINT_add(g, X.get(), a.get(), b.get(), ctx.get()))
        return false;

    Point Y = point_from(g, verifier_share, ctx.get());
    if (!Y) {
        err = "the plug's key share is not a point on the P-256 curve";
        return false;
    }
    if (!EC_POINT_mul(g, b.get(), nullptr, N.get(), w0.get(), ctx.get()) ||      // w0*N
        !EC_POINT_invert(g, b.get(), ctx.get()) ||
        !EC_POINT_add(g, a.get(), Y.get(), b.get(), ctx.get()))                  // Y - w0*N
        return false;
    if (EC_POINT_is_at_infinity(g, a.get())) {
        err = "the plug's key share leads to a key that is not secret";
        return false;
    }
    if (!EC_POINT_mul(g, Z.get(), nullptr, a.get(), x.get(), ctx.get()) ||
        !EC_POINT_mul(g, V.get(), nullptr, a.get(), w1.get(), ctx.get()))
        return false;
    if (EC_POINT_is_at_infinity(g, Z.get()) || EC_POINT_is_at_infinity(g, V.get())) {
        err = "the plug's key share leads to a key that is not secret";
        return false;
    }

    const Bytes share = uncompressed(g, X.get(), ctx.get()), yb = uncompressed(g, Y.get(), ctx.get());
    // The transcript holds every point uncompressed and w0 as the full 32 bytes.
    Bytes tt = item(context) + item(id_prover) + item(id_verifier) + item(uncompressed(g, M.get(), ctx.get())) +
               item(uncompressed(g, N.get(), ctx.get())) + item(share) + item(yb) +
               item(uncompressed(g, Z.get(), ctx.get())) + item(uncompressed(g, V.get(), ctx.get())) + item(w0b);
    Bytes main_key = sha256(tt);
    const Bytes confirm_keys = hkdf_sha256(main_key, Bytes(), "ConfirmationKeys", 64);
    out.share = share;
    out.confirm = hmac_sha256(confirm_keys.substr(0, 32), yb);
    out.verifier_confirm_key = confirm_keys.substr(32);
    out.shared_key = hkdf_sha256(main_key, Bytes(), "SharedKey", 32);
    OPENSSL_cleanse(&tt[0], tt.size());
    if (!main_key.empty()) OPENSSL_cleanse(&main_key[0], main_key.size());
    if (share.size() != 65 || yb.size() != 65 || out.confirm.size() != 32 || out.shared_key.size() != 32) return false;
    err.clear();
    return true;
}

bool spake2p_verify(const SpakeProof &proof, const Bytes &verifier_confirm) {
    const Bytes want = hmac_sha256(proof.verifier_confirm_key, proof.share);
    return want.size() == 32 && same(want, verifier_confirm);
}

Bytes tpap_context(const Bytes &user_random, const Bytes &dev_random) {
    return sha256("PAKE V1" + user_random + dev_random);
}

TpapKeys tpap_derive(const Bytes &shared_key) {
    TpapKeys k;
    k.key = hkdf_sha256(shared_key, "tp-kdf-salt-aes128-key", "tp-kdf-info-aes128-key", 16);
    k.nonce = hkdf_sha256(shared_key, "tp-kdf-salt-aes128-iv", "tp-kdf-info-aes128-iv", 12);
    return k;
}

// body = seq || AES-128-CCM(plain) || tag, under the nonce base[0:8] || seq. No associated data.
Bytes tpap_seal(const TpapKeys &k, int32_t seq, const std::string &plain) {
    Bytes ct;
    if (k.nonce.size() != 12 || !aes128_ccm_seal(k.key, k.nonce.substr(0, 8) + be32(seq), Bytes(), plain, 16, ct))
        return Bytes();
    return be32(seq) + ct;
}

// The plug answers under the request's sequence number; anything else is not the answer to it.
bool tpap_open(const TpapKeys &k, int32_t seq, const Bytes &body, std::string &plain) {
    plain.clear();
    if (k.nonce.size() != 12 || body.size() <= 4 + 16 || body.compare(0, 4, be32(seq)) != 0) return false;
    return aes128_ccm_open(k.key, k.nonce.substr(0, 8) + be32(seq), Bytes(), body.substr(4), 16, plain);
}

std::string describe_error(int code) {
    switch (code) {
        case -1002: return "the plug does not know this command";
        case -1003: return "the plug could not read the request";
        case -1008: return "the plug rejected the parameters of the request";
        case -1010: return "the plug rejected the key of an older login scheme";
        case -1501: return "the plug rejected the login: wrong e-mail or password";
        case 9999: return "the session with the plug has timed out";
        case -2101: return "the plug is locked after too many failed logins";
        case -2201: return "the plug does not support the offered cipher suite";
        case -2202: return "the plug does not take the account password for this login";
        case -2203: return "wrong password for this plug";
        case -2402: return "the plug no longer knows this session";
        case -40401: return "the session with the plug has expired";
        case 1003: return "the plug's firmware does not speak TPAP";
        default: return "the plug answered with error code " + std::to_string(code);
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------
namespace {
using detail::Bytes;

const int kWrongPassword = -2203, kLocked = -2101;
const char kNoRetry[] = " (not tried again, so that the plug does not lock itself; correct the account details first)";

struct Http {
    long status = 0;
    Bytes body;
    std::string cookie;       // TP_SESSIONID of a KLAP handshake
    bool unreached = false;   // no connection was made, so the plug saw nothing of the request
};

size_t on_body(char *p, size_t size, size_t n, void *user) {
    auto *h = static_cast<Http *>(user);
    if (h->body.size() + size * n > (1u << 20)) return 0;   // no plug answers with a megabyte
    h->body.append(p, size * n);
    return size * n;
}

// "Set-Cookie: TP_SESSIONID=<id>;TIMEOUT=1440". Only the id is kept: the plug does not want
// TIMEOUT sent back, which is why curl's own cookie handling is not used.
size_t on_header(char *p, size_t size, size_t n, void *user) {
    std::string line(p, size * n);
    static const char name[] = "set-cookie:", key[] = "TP_SESSIONID=";
    if (line.size() > sizeof name && strncasecmp(line.c_str(), name, sizeof name - 1) == 0) {
        const size_t at = line.find(key);
        if (at != std::string::npos) {
            const size_t from = at + sizeof key - 1;
            std::string id = line.substr(from, line.find_first_of("; \r\n", from) - from);
            bool clean = !id.empty() && id.size() <= 128;
            for (unsigned char c : id) clean = clean && c > 0x20 && c < 0x7f && c != ',' && c != '"';
            if (clean) static_cast<Http *>(user)->cookie = id;
        }
    }
    return size * n;
}

// Some P110 firmware starts its HTTP server only once it has seen a discovery query on UDP 20002.
void wake(const std::string &host) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), "20002", &hints, &res) != 0 || !res) return;
    const int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0) {
        (void)!sendto(fd, detail::kDiscoveryQuery, sizeof detail::kDiscoveryQuery, 0, res->ai_addr, res->ai_addrlen);
        close(fd);
    }
    freeaddrinfo(res);
}

// One POST. Connections are not kept: these plugs close them anyway.
bool http_post(const std::string &host, const std::string &url, const Bytes &body, const char *content_type,
               const std::string &cookie, Http &out, std::string &err) {
    for (int attempt = 0;; attempt++) {
        out = Http();
        CURL *c = curl_easy_init();
        if (!c) {
            err = "the HTTP client could not be started";
            return false;
        }
        curl_slist *headers = curl_slist_append(nullptr, (std::string("Content-Type: ") + content_type).c_str());
        headers = curl_slist_append(headers, "Expect:");   // no "100-continue" round trip
        if (!cookie.empty()) headers = curl_slist_append(headers, ("Cookie: TP_SESSIONID=" + cookie).c_str());
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &out);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 6000L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);          // called from worker threads
        curl_easy_setopt(c, CURLOPT_PROXY, "");             // a plug is on the local network
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http");
#else
        curl_easy_setopt(c, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP));
#endif
        const CURLcode rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out.status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        if (rc == CURLE_OK) return true;
        out.unreached = rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST;
        if (rc == CURLE_COULDNT_CONNECT && attempt == 0) {
            wake(host);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }
        // The URL is left out of the message: it can carry the session token.
        err = rc == CURLE_COULDNT_CONNECT ? "cannot connect to the plug"
              : rc == CURLE_OPERATION_TIMEDOUT ? "the plug did not answer in time"
              : std::string("the request to the plug failed: ") + curl_easy_strerror(rc);
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------------------------
struct Session {
    bool tpap = false;
    std::string fp;                 // fingerprint of the credentials the session was opened with
    std::string base;               // http://host:port
    std::string cookie, stok;       // KLAP session cookie / TPAP session token
    detail::KlapKeys klap;
    detail::TpapKeys keys;
    int32_t seq = 0;                // TPAP: sequence number of the next request
    std::string mac;                // the saved MAC the plug in this session has been checked against ("" until then)
};

struct State {
    std::mutex mu;                                  // one request at a time: sequence numbers must not interleave
    std::unique_ptr<Session> session;
    std::map<std::string, std::string> rejected;    // credential fingerprint -> what the plug said
    std::chrono::steady_clock::time_point locked_until{};
    json tpap;                                      // the plug's own answer to "discover", when discovery gave none
};

std::mutex g_mu;
std::map<std::string, std::shared_ptr<State>> g_states;   // by "host:port"

int32_t next_seq(int32_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v) + 1u); }

bool get_int(const json &j, const char *key, long long &out) {
    if (!j.is_object()) return false;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    out = it->get<long long>();
    return true;
}

std::string get_str(const json &j, const char *key) {
    if (!j.is_object()) return std::string();
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const json &get_obj(const json &j, const char *key) {
    static const json none;
    if (!j.is_object()) return none;
    const auto it = j.find(key);
    return it != j.end() ? *it : none;
}

// A locked plug answers every login step with -2101 and may say for how long. Nothing is sent to
// it until then (ten minutes when it does not say).
bool locked(State &st, const json &resp, std::string &err) {
    const json &info = get_obj(resp, "error_info");
    long long minutes = 0;
    if (!get_int(info, "lockedMinute", minutes)) minutes = atoll(get_str(info, "lockedMinute").c_str());
    const bool known = minutes > 0;
    if (!known) minutes = 10;
    if (minutes > 24 * 60) minutes = 24 * 60;
    st.locked_until = std::chrono::steady_clock::now() + std::chrono::minutes(minutes);
    err = detail::describe_error(kLocked) + (known ? "; it says to wait " + std::to_string(minutes) + " min" : "") +
          ". Do not try again before then";
    return false;
}

// --- KLAP login --------------------------------------------------------------------------------
// handshake1: client seed -> plug seed + SHA256(seeds, account). handshake2: the client's own proof.
// A wrong account shows in the plug's proof, before the client has proved or sent anything.
bool login_klap(const std::string &host, const std::string &base, const std::string &email, const Bytes &pw_sha1,
                State &st, const std::string &fp, std::string &err) {
    Bytes local(16, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(&local[0]), 16) != 1) {
        err = "no random numbers available";
        return false;
    }
    const Bytes auth = detail::klap_auth_hash(email, pw_sha1);
    const std::string url = base + "/app";
    Http h;
    if (!http_post(host, url + "/handshake1", local, "application/octet-stream", "", h, err)) return false;
    if (h.status == 403) {
        err = "the plug refuses the KLAP login; it has probably switched to TPAP (run discovery again)";
        return false;
    }
    if (h.status != 200 || h.body.size() != 48) {
        err = "the plug answered the first login step unexpectedly (HTTP " + std::to_string(h.status) + ")";
        return false;
    }
    const Bytes remote = h.body.substr(0, 16);
    if (!detail::same(h.body.substr(16), detail::klap_handshake1_hash(local, remote, auth))) {
        st.rejected[fp] = err = "the plug does not accept this e-mail and password (both are case-sensitive)";
        return false;
    }
    if (h.cookie.empty()) {
        err = "the plug sent no session cookie";
        return false;
    }
    const std::string cookie = h.cookie;
    if (!http_post(host, url + "/handshake2", detail::klap_handshake2_hash(local, remote, auth),
                   "application/octet-stream", cookie, h, err))
        return false;
    if (h.status != 200) {
        if (h.status == 403) st.rejected[fp] = "the plug rejected the login (KLAP handshake)";
        err = "the plug rejected the second login step (HTTP " + std::to_string(h.status) + ")";
        return false;
    }
    auto s = std::make_unique<Session>();
    s->fp = fp;
    s->base = url;
    s->cookie = cookie;
    s->klap = detail::klap_derive(local, remote, auth);
    st.session = std::move(s);
    return true;
}

// --- TPAP login --------------------------------------------------------------------------------

// One step of the login: a JSON POST to "/", answered with {"error_code":..,"result":{..}}.
bool login_step(const std::string &host, const std::string &base, const json &params, json &resp, std::string &err) {
    Http h;
    const json body = {{"method", "login"}, {"params", params}};
    if (!http_post(host, base + "/", body.dump(), "application/json", "", h, err)) return false;
    resp = h.status == 200 ? json::parse(h.body, nullptr, false) : json();
    long long code = 0;
    if (!get_int(resp, "error_code", code)) {
        err = "the plug answered the login unexpectedly (HTTP " + std::to_string(h.status) + ")";
        return false;
    }
    return true;
}

// Only the variant the owner's P110s announce is spoken: plain HTTP and the account password.
// `dac` and `noc` are not looked at; with pake [2] they change nothing in the login (a P304M on
// TPAP firmware announces both as 1 and logs in the same way).
bool check_variant(const json &tpap, int &port, std::string &err) {
    if (!tpap.is_object()) {
        err = "the plug's TPAP announcement cannot be read";
        return false;
    }
    long long tls = 0, p = 0;
    if (tpap.contains("tls") && (!get_int(tpap, "tls", tls) || tls != 0)) {
        err = "this plug wants TPAP over TLS, which is not supported";
        return false;
    }
    bool account = false, setup = false, readable = tpap.contains("pake") && tpap["pake"].is_array();
    if (readable)
        for (const json &v : tpap["pake"]) {
            readable = readable && v.is_number_integer();
            account = account || v == 2;
            setup = setup || v == 0;
        }
    if (!readable || !account || setup) {   // as the crate: [0] is a plug in setup mode, [0,2] was never seen
        err = "this plug does not take the account password over TPAP (unsupported passcode types)";
        return false;
    }
    if (get_int(tpap, "port", p) && p > 0 && p < 65536 && port == 80) port = static_cast<int>(p);
    return true;
}

// Asks the plug how it wants to be logged in to. This proves nothing and is not a login attempt.
bool discover_tpap(const std::string &host, const std::string &base, State &st, json &tpap, std::string &err) {
    json resp;
    if (!login_step(host, base, {{"sub_method", "discover"}}, resp, err)) return false;
    long long code = 0;
    get_int(resp, "error_code", code);
    if (code == kLocked) return locked(st, resp, err);
    tpap = get_obj(get_obj(resp, "result"), "tpap");
    if (code != 0 || !tpap.is_object()) {
        err = "the plug does not offer the TPAP login; it may have switched to KLAP (run discovery again)";
        return false;
    }
    return true;
}

// What pake_register gives back: the plug's random, the salt and iterations for the password, its share.
struct Registered {
    Bytes dev_random, salt, dev_share;
    long long iterations = 0;
};

// Step 1, pake_register: the plug names salt, iterations and its share.
bool pake_register(const std::string &host, const std::string &base, State &st, const Bytes &user_random, Registered &out,
                   std::string &err) {
    json resp;
    const json reg = {{"sub_method", "pake_register"},
                      {"username", detail::hex(detail::md5("admin"))},   // always "admin", never the e-mail
                      {"user_random", detail::b64(user_random)},
                      {"cipher_suites", {1}},
                      {"encryption", {"aes_128_ccm"}},
                      {"passcode_type", "userpw"}};
    if (!login_step(host, base, reg, resp, err)) return false;
    long long code = 0, suite = 0;
    long long &iterations = out.iterations;
    get_int(resp, "error_code", code);
    if (code == kLocked) return locked(st, resp, err);
    if (code != 0) {
        err = detail::describe_error(static_cast<int>(code));
        return false;
    }
    const json &r = get_obj(resp, "result");
    Bytes &dev_random = out.dev_random, &salt = out.salt, &dev_share = out.dev_share;
    if (!get_int(r, "cipher_suites", suite) || suite != 1 || get_str(r, "encryption") != "aes_128_ccm") {
        err = "the plug picked a cipher suite or encryption that is not supported";
        return false;
    }
    if (!get_int(r, "iterations", iterations) || iterations < 1 || iterations > 100000 ||
        !detail::unb64(get_str(r, "dev_random"), dev_random) || dev_random.size() != 32 ||
        !detail::unb64(get_str(r, "dev_salt"), salt) || salt.empty() ||
        !detail::unb64(get_str(r, "dev_share"), dev_share)) {
        err = "the plug's answer to the login cannot be used";
        return false;
    }
    // Which form of the password the plug holds. Only id 2, the hex SHA-1, can be made from what is stored.
    const json &extra = get_obj(r, "extra_crypt");
    long long passwd_id = 0;
    if (get_str(extra, "type") != "password_shadow" || !get_int(get_obj(extra, "params"), "passwd_id", passwd_id) ||
        passwd_id != 2) {
        err = "the plug asks for a form of the password that is not supported";
        return false;
    }
    return true;
}

// Step 2: prove knowledge of the password without sending it.
bool prove_password(const std::string &pw_hex, const Bytes &user_random, const Registered &reg, detail::SpakeProof &proof,
                    std::string &err) {
    Bytes w0, w1;
    const bool proved = detail::spake2p_w0_w1(pw_hex, reg.salt, static_cast<int>(reg.iterations), w0, w1) &&
                        detail::spake2p_prove(w0, w1, Bytes(), detail::tpap_context(user_random, reg.dev_random), Bytes(),
                                              Bytes(), reg.dev_share, proof, err);
    if (!w0.empty()) OPENSSL_cleanse(&w0[0], w0.size());
    if (!w1.empty()) OPENSSL_cleanse(&w1[0], w1.size());
    if (!proved) {
        if (err.empty()) err = "the key exchange could not be computed";
        return false;
    }
    return true;
}

// Step 3, pake_share. From here on the plug counts a failure towards its lock, so whatever
// it rejects is remembered and never sent again. On success `resp` holds the plug's answer, whose
// proof of the password has been checked.
bool pake_share(const std::string &host, const std::string &base, State &st, const std::string &fp,
                const detail::SpakeProof &proof, json &resp, std::string &err) {
    const json share = {{"sub_method", "pake_share"},
                        {"user_share", detail::b64(proof.share)},
                        {"user_confirm", detail::b64(proof.confirm)}};
    if (!login_step(host, base, share, resp, err)) return false;
    long long code = 0;
    get_int(resp, "error_code", code);
    if (code == kLocked) return locked(st, resp, err);
    if (code != 0) {
        err = code == kWrongPassword ? detail::describe_error(kWrongPassword)
                                     : "the plug rejected the login (error code " + std::to_string(code) + ")";
        long long left = 0;
        if (get_int(get_obj(resp, "error_info"), "remainAttempts", left))
            err += "; it allows " + std::to_string(left) + " more attempts before it locks";
        st.rejected[fp] = err;
        return false;
    }
    const json &sr = get_obj(resp, "result");
    Bytes dev_confirm;
    if (!detail::unb64(get_str(sr, "dev_confirm"), dev_confirm) || !detail::spake2p_verify(proof, dev_confirm)) {
        err = "the plug did not prove that it knows the password; it is not trusted";
        return false;
    }
    return true;
}

// The session the plug opened: its token and first sequence number, checked before they are used.
bool open_session(const json &sr, const std::string &base, const std::string &fp, const detail::SpakeProof &proof,
                  State &st, std::string &err) {
    long long start_seq = 0;
    const std::string stok = get_str(sr, "stok");
    bool clean = !stok.empty() && stok.size() <= 256;
    for (unsigned char c : stok) clean = clean && (isalnum(c) || strchr("-_.~=+%", c));
    if (!clean || !get_int(sr, "start_seq", start_seq) || start_seq < INT32_MIN || start_seq > INT32_MAX) {
        err = "the plug's session details cannot be used";
        return false;
    }
    auto s = std::make_unique<Session>();
    s->tpap = true;
    s->fp = fp;
    s->base = base;
    s->stok = stok;
    s->keys = detail::tpap_derive(proof.shared_key);
    s->seq = static_cast<int32_t>(start_seq);
    st.session = std::move(s);
    return true;
}

bool login_tpap(const std::string &host, const Device &dev, const std::string &pw_hex, State &st,
                const std::string &fp, std::string &err) {
    json info = dev.tpap.is_null() ? st.tpap : dev.tpap;
    if (info.is_null()) {
        if (!discover_tpap(host, "http://" + host + ":" + std::to_string(dev.port), st, info, err)) return false;
        st.tpap = info;
    }
    int port = dev.port;
    if (!check_variant(info, port, err)) return false;
    const std::string base = "http://" + host + ":" + std::to_string(port);

    Bytes user_random(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(&user_random[0]), 32) != 1) {
        err = "no random numbers available";
        return false;
    }
    Registered reg;
    if (!pake_register(host, base, st, user_random, reg, err)) return false;
    detail::SpakeProof proof;
    if (!prove_password(pw_hex, user_random, reg, proof, err)) return false;
    json resp;
    if (!pake_share(host, base, st, fp, proof, resp, err)) return false;
    return open_session(get_obj(resp, "result"), base, fp, proof, st, err);
}

// --- One request on an open session ------------------------------------------------------------

enum class Sent { ok, lost, failed };

// Sends `plain` and fills `resp` with the decrypted {"error_code":..,"result":..}. `lost` means the
// plug no longer knows the session. `keep` tells whether the session is still usable afterwards.
Sent send(const std::string &host, Session &s, const std::string &plain, json &resp, bool &keep, std::string &err) {
    Http h;
    std::string text;
    keep = false;
    bool opened = false;
    if (s.tpap) {
        const int32_t seq = s.seq;
        s.seq = next_seq(seq);
        if (!http_post(host, s.base + "/stok=" + s.stok + "/ds", detail::tpap_seal(s.keys, seq, plain),
                       "application/octet-stream", "", h, err)) {
            if (h.unreached) s.seq = seq, keep = true;
            return Sent::failed;
        }
        // Decrypt first and look for JSON second (a ciphertext can start with '{'). A plug that
        // has dropped the session says so in plain JSON, which is never taken as an answer.
        opened = h.status == 200 && detail::tpap_open(s.keys, seq, h.body, text);
        if (!opened) {
            long long code = 0;
            if ((get_int(json::parse(h.body, nullptr, false), "error_code", code) && code != 0) || h.status == 401 ||
                h.status == 403) {
                err = detail::describe_error(-2402);
                return Sent::lost;
            }
        }
    } else {
        const int32_t seq = next_seq(s.klap.seq);   // KLAP counts up before each request
        if (!http_post(host, s.base + "/request?seq=" + std::to_string(seq), detail::klap_seal(s.klap, seq, plain),
                       "application/octet-stream", s.cookie, h, err)) {
            keep = h.unreached;
            return Sent::failed;
        }
        s.klap.seq = seq;
        if (h.status == 401 || h.status == 403) {
            err = detail::describe_error(-2402);
            return Sent::lost;
        }
        opened = h.status == 200 && detail::klap_open(s.klap, seq, h.body, text);
    }
    if (!opened) {
        err = h.status != 200 ? "the plug answered with HTTP " + std::to_string(h.status)
                              : "the plug's answer could not be decrypted";
        return Sent::failed;
    }
    resp = json::parse(text, nullptr, false);
    long long code = 0;
    if (!get_int(resp, "error_code", code)) {
        err = "the plug's answer cannot be read";
        return Sent::failed;
    }
    if (code == 9999 || code == -40401 || code == -2402) {   // session timed out, said inside the session
        err = detail::describe_error(static_cast<int>(code));
        return Sent::lost;
    }
    keep = true;
    return Sent::ok;
}

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// Whether the plug in this session is the one saved (Device::mac, as `want`): another plug of the
// same account given this one's address would accept the same login, and would then be switched
// in its place. Asked once per session and per saved MAC (Session::mac), before anything else is
// sent in it. A plug that names no MAC at all cannot be told apart and is taken as it is; one that
// names a different MAC, or an unreadable one, gets nothing more (Sent::failed, the session
// dropped). Sent::lost and failures on the way are send()'s own.
Sent check_plug(const std::string &host, Session &s, const std::string &want, bool &keep, std::string &err) {
    keep = true;
    if (want.empty() || s.mac == want) return Sent::ok;
    json resp;
    const Sent sent = send(host, s, R"({"method":"get_device_info","params":null})", resp, keep, err);
    if (sent != Sent::ok) return sent;
    const json &result = get_obj(resp, "result");
    if (result.is_object() && result.contains("mac") && support::mac_digits(get_str(result, "mac")) != want) {
        keep = false;
        err = "another plug answered at this plug's saved address, so nothing was sent to it";
        return Sent::failed;
    }
    s.mac = want;
    return Sent::ok;
}

// What the plug would check: KLAP hashes e-mail and password, TPAP the password alone.
std::string fingerprint(bool tpap, const std::string &email, const std::string &pw_hex) {
    return detail::hex(detail::sha256((tpap ? std::string("TPAP") : "KLAP" + email) + '\0' + pw_hex));
}

// request(), also reporting the plug's error code (0 when the failure was not the plug saying no).
bool exchange(const Device &dev, const Credentials &cred, const json &req, json &reply, std::string &err, int &code) {
    code = 0;
    err.clear();
    reply = json::object();
    const bool tpap = dev.scheme == "TPAP";
    if (!tpap && dev.scheme != "KLAP") {
        err = "this plug's login scheme is not supported (only KLAP and TPAP are)";
        return false;
    }
    bool host_ok = !dev.ip.empty() && dev.ip.size() < 254 && dev.port > 0 && dev.port < 65536;
    for (unsigned char c : dev.ip) host_ok = host_ok && (isalnum(c) || c == '.' || c == '-');
    if (!host_ok) {
        err = "the plug's address is not usable";
        return false;
    }
    Bytes pw_sha1;
    if (cred.password_sha1_hex.size() != 40 || !detail::unhex(cred.password_sha1_hex, pw_sha1)) {
        err = "the stored password hash is not a SHA-1 in hex";
        return false;
    }
    if (!tpap && cred.email.empty()) {
        err = "this plug needs the account e-mail to log in";
        return false;
    }
    const std::string pw_hex = detail::hex(pw_sha1);
    const std::string fp = fingerprint(tpap, cred.email, pw_hex);

    std::shared_ptr<State> st;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto &slot = g_states[dev.ip + ":" + std::to_string(dev.port)];
        if (!slot) slot = std::make_shared<State>();
        st = slot;
    }
    std::lock_guard<std::mutex> lock(st->mu);

    const auto rejected = st->rejected.find(fp);
    if (rejected != st->rejected.end()) {
        err = rejected->second + kNoRetry;
        return false;
    }
    if (std::chrono::steady_clock::now() < st->locked_until) {
        err = detail::describe_error(kLocked) + "; not contacted again until the lock should be over";
        return false;
    }
    if (!owned_by(dev, cred.email)) {
        err = "this e-mail does not match the plug's owner, so no login was attempted";
        return false;
    }

    const std::string host = dev.ip;
    const std::string want_mac = support::mac_digits(dev.mac);
    const auto login = [&]() {
        st->session.reset();
        return tpap ? login_tpap(host, dev, pw_hex, *st, fp, err)
                    : login_klap(host, "http://" + host + ":" + std::to_string(dev.port), cred.email, pw_sha1, *st, fp, err);
    };
    bool fresh = false;   // at most one login per call
    if (!st->session || st->session->fp != fp || st->session->tpap != tpap) {
        if (!login()) return false;
        fresh = true;
    }
    const std::string plain = req.dump();
    json resp;
    for (;;) {
        bool keep = false;
        // The plug is asked who it is first (once per session), so that the request goes to no other.
        Sent sent = check_plug(host, *st->session, want_mac, keep, err);
        if (sent == Sent::ok) sent = send(host, *st->session, plain, resp, keep, err);
        if (!keep) st->session.reset();
        if (sent == Sent::ok) break;
        if (sent == Sent::failed) return false;
        if (fresh) {
            err = "the plug dropped a session it had only just opened; giving up";
            return false;
        }
        if (!login()) return false;
        fresh = true;
    }
    long long c = 0;
    get_int(resp, "error_code", c);
    code = static_cast<int>(c);
    if (code != 0) {
        err = detail::describe_error(code);
        return false;
    }
    if (get_obj(resp, "result").is_object()) reply = resp["result"];
    return true;
}

bool exchange_safe(const Device &dev, const Credentials &cred, const json &req, json &reply, std::string &err, int &code) {
    try {
        return exchange(dev, cred, req, reply, err, code);
    } catch (const std::exception &) {   // json: a request that is not valid UTF-8, or no memory
        code = 0;
        err = "the request could not be built or its answer could not be read";
        return false;
    }
}

// A getter as the tapo crate sends it, with "params": null.
json getter(const char *method) { return {{"method", method}, {"params", nullptr}}; }

json set_on(bool on) { return {{"method", "set_device_info"}, {"params", {{"device_on", on}}}}; }

// A name as the owner typed it: base64 on the wire. Bytes that are not UTF-8 are replaced, since
// a json value holding them could not be written out again.
std::string decoded(const json &j, const char *key) {
    Bytes out;
    const std::string raw = get_str(j, key);
    if (!detail::unb64(raw, out)) return raw;
    return json::parse(json(out).dump(-1, ' ', false, json::error_handler_t::replace)).get<std::string>();
}

// The sockets of a strip, as the plug lists them (pages of get_child_device_list).
bool children(const Device &dev, const Credentials &cred, json &list, std::string &err, int &code) {
    list = json::array();
    for (int page = 0; page < 8; page++) {
        json r;
        const json req = {{"method", "get_child_device_list"}, {"params", {{"start_index", list.size()}}}};
        if (!exchange_safe(dev, cred, req, r, err, code)) return false;
        const json &got = get_obj(r, "child_device_list");
        if (!got.is_array() || got.empty()) break;
        for (const json &child : got) list.push_back(child);
        long long sum = 0;
        if (!get_int(r, "sum", sum) || static_cast<long long>(list.size()) >= sum) break;
    }
    return true;
}

bool is_on(const json &j) {
    const json &v = get_obj(j, "device_on");
    return v.is_boolean() && v.get<bool>();
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Interface
// ---------------------------------------------------------------------------------------------

std::string sha1_hex(const std::string &text) { return detail::hex(detail::sha1(text)); }

bool owned_by(const Device &dev, const std::string &email) {
    if (dev.owner.empty()) return true;
    const std::string owner = lower(dev.owner);
    return !email.empty() && (owner == detail::hex(detail::md5(email)) || owner == detail::hex(detail::md5(lower(email))));
}

bool request(const Device &dev, const Credentials &cred, const json &req, json &reply, std::string &err) {
    int code = 0;
    return exchange_safe(dev, cred, req, reply, err, code);
}

bool login_refused(const Device &dev, const Credentials &cred) {
    Bytes pw_sha1;
    if (cred.password_sha1_hex.size() != 40 || !detail::unhex(cred.password_sha1_hex, pw_sha1)) return false;
    const std::string fp = fingerprint(dev.scheme == "TPAP", cred.email, detail::hex(pw_sha1));
    std::shared_ptr<State> st;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const auto it = g_states.find(dev.ip + ":" + std::to_string(dev.port));
        if (it == g_states.end()) return false;
        st = it->second;
    }
    std::lock_guard<std::mutex> lock(st->mu);
    return st->rejected.count(fp) > 0 || std::chrono::steady_clock::now() < st->locked_until;
}

bool get_info(const Device &dev, const Credentials &cred, json &info, std::string &err) {
    int code = 0;
    if (!exchange_safe(dev, cred, getter("get_device_info"), info, err, code)) return false;
    for (const char *key : {"nickname", "ssid"})
        if (info.contains(key)) info[key] = decoded(info, key);
    info["name"] = get_str(info, "nickname");
    info["model"] = get_str(info, "model");
    info["fw"] = get_str(info, "fw_ver");
    info["sockets"] = 0;
    info["on"] = is_on(info);
    if (!info.contains("device_on")) {   // a strip has no switch of its own: it is on when any socket is
        json list;
        if (children(dev, cred, list, err, code)) {
            info["sockets"] = list.size();
            for (const json &child : list) info["on"] = info["on"].get<bool>() || is_on(child);
        } else if (code == 0) {
            return false;   // not the plug saying it has no sockets
        }
        err.clear();
    }
    return true;
}

bool set_power(const Device &dev, const Credentials &cred, bool on, std::string &err) {
    // As the tapo crate sends it: set_device_info carries the time and a terminal id.
    json req = set_on(on), reply;
    req["requestTimeMilis"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch()).count();
    req["terminalUUID"] = "00-00-00-00-00-00";
    return request(dev, cred, req, reply, err);
}

// One request in the usual case. Firmware differs in what it reports where, so what is missing
// is asked for separately: get_energy_usage gives today's energy and, on most plugs, the power in
// mW; get_emeter_data and get_current_power give the power; get_device_usage gives today's energy.
bool get_energy(const Device &dev, const Credentials &cred, json &energy, std::string &err) {
    energy = json::object();
    json raw = json::object(), r;
    std::string first_err;
    int code = 0;
    long long v = 0;
    const auto ask = [&](const char *method) {
        if (exchange_safe(dev, cred, getter(method), r, err, code)) {
            raw[method] = r;
            return true;
        }
        if (first_err.empty()) first_err = err;
        return false;
    };
    if (ask("get_energy_usage")) {
        if (get_int(r, "today_energy", v)) energy["today_wh"] = v;
        if (get_int(r, "month_energy", v)) energy["month_wh"] = v;
        if (get_int(r, "current_power", v)) energy["power_w"] = v / 1000.0;
    } else if (code == 0) {
        return false;   // unreachable or not logged in: asking again would not help
    }
    if (!energy.contains("power_w")) {
        if (ask("get_emeter_data")) {
            if (get_int(r, "power_mw", v)) energy["power_w"] = v / 1000.0;
        } else if (code == 0) {
            return false;
        } else if (ask("get_current_power")) {
            if (get_int(r, "current_power", v)) energy["power_w"] = static_cast<double>(v);   // whole watts
        } else if (code == 0) {
            return false;
        }
    }
    if (!energy.contains("today_wh")) {
        if (ask("get_device_usage")) {
            if (get_int(get_obj(r, "power_usage"), "today", v)) energy["today_wh"] = v;
        } else if (code == 0) {
            return false;
        }
    }
    if (energy.empty()) {
        err = first_err.empty() ? "the plug reports no energy readings" : first_err;
        return false;
    }
    energy["raw"] = raw;
    err.clear();
    return true;
}

bool list_sockets(const Device &dev, const Credentials &cred, json &sockets, std::string &err) {
    json list;
    int code = 0;
    sockets = json::array();
    if (!children(dev, cred, list, err, code)) return false;
    for (const json &child : list) {
        json s = {{"id", get_str(child, "device_id")}, {"name", decoded(child, "nickname")}, {"on", is_on(child)}};
        long long position = 0;
        if (get_int(child, "position", position)) s["position"] = position;
        sockets.push_back(s);
    }
    return true;
}

// A strip takes the request for a socket inside a multipleRequest inside a control_child, and
// answers with an error code for the envelope and one for the socket's own request.
bool set_socket_power(const Device &dev, const Credentials &cred, const std::string &child_id, bool on, std::string &err) {
    const json inner = {{"method", "multipleRequest"}, {"params", {{"requests", {set_on(on)}}}}};
    const json req = {{"method", "control_child"}, {"params", {{"device_id", child_id}, {"requestData", inner}}}};
    json reply;
    if (!request(dev, cred, req, reply, err)) return false;
    const json &responses = get_obj(get_obj(get_obj(reply, "responseData"), "result"), "responses");
    long long code = 0;
    if (!responses.is_array() || responses.empty() || !get_int(responses[0], "error_code", code)) {
        err = "the strip did not say whether the socket was switched";
        return false;
    }
    if (code != 0) {
        err = "the socket: " + detail::describe_error(static_cast<int>(code));
        return false;
    }
    return true;
}

void forget_sessions() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_states.clear();   // a request under way keeps its own state alive until it returns
}

}  // namespace tapo
