// Known-answer and unit tests for homekit.cpp. No network is used.
// Each check prints one line; the exit status is non-zero when any fails.
//
// Where the vectors come from:
//   FIPS / RFC   the named standard (FIPS 180-2, RFC 4231, RFC 5869, RFC 7748, RFC 8032, RFC 8439)
//   ADK          apple/HomeKitADK (Apache-2.0) Tests/HAPCryptoTest.c (SRP: the RFC 5054 appendix B
//                inputs on the 3072-bit group with SHA-512, cited there from wolfSSL issue 18;
//                HKDF-SHA-512 from kullo.net) and Tests/HAPAccessorySetupGetSetupPayloadTest.c
//                (X-HM:// payloads). The ADK lists no client secret `a`: RFC 5054's `a`
//                reproduces the ADK's `A`, so it is used here.
//   aiohomekit   Jc2k/aiohomekit (Apache-2.0) tests: test_protocol_tlv.py (TLV8),
//                test_crypto_srp.py (zero salt with b = 1336 gives B with a leading zero byte),
//                crypto/srp.py CLIENT_K_VALUE (k)
//   Wycheproof   C2SP/wycheproof (Apache-2.0) hkdf_sha512_test.json, cases 1 and 10
//   mock         computed by mock_homekit.py's own pure-Python ChaCha20-Poly1305 and HKDF, which
//                share no code with this module (the session frame known answers)
#include "smarthome/drivers/homekit.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <set>
#include <string>

#include <openssl/bn.h>

using namespace homekit::detail;
using home::json;

// ---- a counting allocator, so that the memory a hostile reply costs can be measured in-process
// (only C++ allocations are counted: nlohmann::json and std containers, not OpenSSL's)
static std::atomic<size_t> g_heap_now{0}, g_heap_peak{0};
static void *counted_new(size_t n) {
    void *p = std::malloc(n + 16);
    if (!p) return nullptr;
    *static_cast<size_t *>(p) = n;
    const size_t now = g_heap_now += n;
    size_t peak = g_heap_peak.load();
    while (now > peak && !g_heap_peak.compare_exchange_weak(peak, now)) {
    }
    return static_cast<char *>(p) + 16;
}
static void counted_delete(void *p) noexcept {
    if (!p) return;
    char *base = static_cast<char *>(p) - 16;
    g_heap_now -= *reinterpret_cast<size_t *>(base);
    std::free(base);
}
void *operator new(size_t n) {
    if (void *p = counted_new(n)) return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n) {
    if (void *p = counted_new(n)) return p;
    throw std::bad_alloc();
}
void *operator new(size_t n, const std::nothrow_t &) noexcept { return counted_new(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return counted_new(n); }
void operator delete(void *p) noexcept { counted_delete(p); }
void operator delete[](void *p) noexcept { counted_delete(p); }
void operator delete(void *p, size_t) noexcept { counted_delete(p); }
void operator delete[](void *p, size_t) noexcept { counted_delete(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { counted_delete(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { counted_delete(p); }

static int g_failed = 0, g_run = 0;

static void check(const std::string &name, bool ok, const std::string &detail = "") {
    g_run++;
    if (!ok) g_failed++;
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name.c_str(), ok || detail.empty() ? "" : "  -> ", ok ? "" : detail.c_str());
}

static void eq(const std::string &name, const std::string &got, const std::string &want) {
    check(name, got == want, "got " + got + ", want " + want);
}

static Bytes H(const std::string &hexstr) {
    Bytes out;
    if (!unhex(hexstr, out)) {
        printf("FAIL  bad hex in the test itself: %.40s\n", hexstr.c_str());
        g_failed++;
    }
    return out;
}

static Bytes range(int from, int count) {
    Bytes b;
    for (int i = 0; i < count; i++) b += static_cast<char>(from + i);
    return b;
}

// ---------------------------------------------------------------------------------------------
static void test_primitives() {
    eq("SHA-512 of \"abc\" (FIPS 180-2)", hex(sha512("abc")),
       "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
       "454d4423643ce80e2a9ac94fa54ca49f");
    eq("HMAC-SHA-512 (RFC 4231 case 2)", hex(hmac_sha512("Jefe", "what do ya want for nothing?")),
       "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fd"
       "caeab1a34d4a6b4b636e070a38bce737");
    eq("HKDF-SHA-512 (ADK, from kullo.net)",
       hex(hkdf_sha512(Bytes(22, '\x0b'), H("000102030405060708090a0b0c"), H("f0f1f2f3f4f5f6f7f8f9"), 42)),
       "832390086cda71fb47625bb5ceb168e4c8e26a1a16ed34d9fc7fe92c1481579338da362cb8d9f925d7cb");
    eq("HKDF-SHA-512, empty salt and info (Wycheproof 1)", hex(hkdf_sha512(H("24aeff2645e3e0f5494a9a102778c43a"), "", "", 20)),
       "dd2599840b09699c6200b5cba79002b3aa75c61b");
    eq("HKDF-SHA-512 (Wycheproof 10)",
       hex(hkdf_sha512(H("c360e16084cfd13cb44b0dc02d8665de"), H("685ac7df93701d6c78babd847861bb3c"),
                       H("e0ddfaaaa7afb53f59a007a205c7149b5b5a72be"), 20)),
       "17408c6f8dd7eb8423758ce39a91b59020f7debe");
    check("HKDF-SHA-512 refuses a length of 0 or over 255 blocks",
          hkdf_sha512("k", "s", "i", 0).empty() && hkdf_sha512("k", "s", "i", 255 * 64 + 1).empty() &&
              hkdf_sha512("k", "s", "i", 255 * 64).size() == 255 * 64);

    // RFC 8439 2.8.2
    const Bytes key = range(0x80, 32), nonce = H("070000004041424344454647"), aad = H("50515253c0c1c2c3c4c5c6c7");
    const Bytes pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                     "sunscreen would be it.";
    Bytes sealed, back;
    const bool sealed_ok = chacha20_poly1305_seal(key, nonce, aad, pt, sealed);
    check("ChaCha20-Poly1305 seal (RFC 8439 2.8.2)",
          sealed_ok && hex(sealed) ==
                           "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b"
                           "1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
                           "3ff4def08e4b7a9de576d26586cec64b6116"
                           "1ae10b594f09e26a7e902ecbd0600691",
          hex(sealed));
    check("ChaCha20-Poly1305 open (RFC 8439 2.8.2)", chacha20_poly1305_open(key, nonce, aad, sealed, back) && back == pt);
    Bytes bad = sealed;
    bad[0] ^= 1;
    check("ChaCha20-Poly1305 refuses a changed ciphertext", !chacha20_poly1305_open(key, nonce, aad, bad, back));
    bad = sealed;
    bad[bad.size() - 1] ^= 1;
    check("ChaCha20-Poly1305 refuses a changed tag", !chacha20_poly1305_open(key, nonce, aad, bad, back));
    check("ChaCha20-Poly1305 refuses changed associated data", !chacha20_poly1305_open(key, nonce, aad + "x", sealed, back));
    check("ChaCha20-Poly1305 refuses input shorter than a tag", !chacha20_poly1305_open(key, nonce, aad, Bytes(15, 'a'), back));
    // RFC 8439 A.5
    const Bytes a5 = H("64a0861575861af460f062c79be643bd5e805cfd345cf389f108670ac76c8cb24c6cfc18755d43eea09ee94e382d26b0"
                       "bdb7b73c321b0100d4f03b7f355894cf332f830e710b97ce98c8a84abd0b948114ad176e008d33bd60f982b1ff37c855"
                       "9797a06ef4f0ef61c186324e2b3506383606907b6a7c02b0f9f6157b53c867e4b9166c767b804d46a59b5216cde7a4e9"
                       "9040c5a40433225ee282a1b0a06c523eaf4534d7f83fa1155b0047718cbc546a0d072b04b3564eea1b422273f548271a"
                       "0bb2316053fa76991955ebd63159434ecebb4e466dae5a1073a6727627097a1049e617d91d361094fa68f0ff77987130"
                       "305beaba2eda04df997b714d6c6f2c29a6ad5cb4022b02709b"
                       "eead9d67890cbb22392336fea1851f38");
    check("ChaCha20-Poly1305 open (RFC 8439 A.5)",
          chacha20_poly1305_open(H("1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc207075c0"),
                                 H("000000000102030405060708"), H("f33388860000000000004e91"), a5, back) &&
              back.size() == 265 && back.compare(0, 35, "Internet-Drafts are draft documents") == 0 &&
              back.compare(back.size() - 12, 12, "rogress./\xe2\x80\x9d") == 0);
    check("ChaCha20-Poly1305 with an empty text gives only a tag, and opens",
          chacha20_poly1305_seal(key, nonce, "", "", sealed) && sealed.size() == 16 &&
              chacha20_poly1305_open(key, nonce, "", sealed, back) && back.empty());
    eq("nonce for a message label: 4 zero bytes and \"PS-Msg05\"", hex(label_nonce("PS-Msg05")), "0000000050532d4d73673035");
    eq("nonce for frame 258: 4 zero bytes and the counter little-endian", hex(counter_nonce(258)), "000000000201000000000000");
    eq("ChaCha20-Poly1305 under a label nonce (mock)",
       [&] {
           Bytes s;
           chacha20_poly1305_seal(range(0, 32), label_nonce("PS-Msg05"), "", "hello", s);
           return hex(s);
       }(),
       "5c9fee01061a743ae7b4aca3e424c8143a5092f000");

    // RFC 7748
    Bytes out, pub;
    check("X25519 (RFC 7748 5.2 vector 1)",
          x25519_shared(H("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                        H("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"), out) &&
              hex(out) == "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    check("X25519 (RFC 7748 5.2 vector 2)",
          x25519_shared(H("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
                        H("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"), out) &&
              hex(out) == "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
    const Bytes alice = H("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Bytes bob = H("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    Bytes alice_pub, bob_pub, k1, k2;
    check("X25519 public keys and shared secret, both ways (RFC 7748 6.1)",
          x25519_public(alice, alice_pub) && x25519_public(bob, bob_pub) &&
              hex(alice_pub) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a" &&
              hex(bob_pub) == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f" &&
              x25519_shared(alice, bob_pub, k1) && x25519_shared(bob, alice_pub, k2) && k1 == k2 &&
              hex(k1) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    check("X25519 refuses a peer key of small order (all-zero secret)", !x25519_shared(alice, Bytes(32, '\0'), out));
    check("X25519 refuses keys of the wrong length", !x25519_shared(alice, Bytes(31, 'a'), out) && !x25519_public("x", pub));

    // RFC 8032 7.1
    struct Ed {
        const char *name, *sk, *pk, *msg, *sig;
    } const eds[] = {
        {"1", "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"2", "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"3", "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (const Ed &e : eds) {
        Bytes p, s;
        check(std::string("Ed25519 public key and signature (RFC 8032 7.1 test ") + e.name + ")",
              ed25519_public(H(e.sk), p) && hex(p) == e.pk && ed25519_sign(H(e.sk), H(e.msg), s) && hex(s) == e.sig);
        Bytes changed = H(e.sig);
        changed[10] ^= 1;
        check(std::string("Ed25519 verify accepts the signature and refuses a changed one or message (test ") + e.name + ")",
              ed25519_verify(H(e.pk), H(e.msg), H(e.sig)) && !ed25519_verify(H(e.pk), H(e.msg), changed) &&
                  !ed25519_verify(H(e.pk), H(e.msg) + "x", H(e.sig)) && !ed25519_verify(H(e.pk), H(e.msg), H(e.sig).substr(1)));
    }
}

// ---------------------------------------------------------------------------------------------
static std::string rep(const char *s, int n) {
    std::string r;
    for (int i = 0; i < n; i++) r += s;
    return r;
}

static void test_tlv() {
    const Bytes vec = H("060103" "09FF" + rep("61", 255) + "092D" + rep("61", 45) + "010568656c6c6f");
    std::vector<Tlv> items;
    check("TLV8 decode joins a 300-byte value split 255 + 45 (aiohomekit)",
          tlv_decode(vec, items) && items.size() == 3 && items[0].type == 6 && items[0].value == "\x03" &&
              items[1].type == 9 && items[1].value == Bytes(300, 'a') && items[2].type == 1 && items[2].value == "hello");
    check("TLV8 encode splits it the same way (aiohomekit)",
          tlv_encode({{6, "\x03"}, {9, Bytes(300, 'a')}, {1, "hello"}}) == vec);
    const Bytes enc510 = tlv_encode({{3, Bytes(510, 'b')}});
    check("TLV8 510 bytes are two full fragments, 255 bytes one",
          enc510.size() == 514 && static_cast<uint8_t>(enc510[1]) == 255 && static_cast<uint8_t>(enc510[258]) == 255 &&
              tlv_encode({{3, Bytes(255, 'b')}}).size() == 257 && tlv_decode(enc510, items) && items.size() == 1 &&
              items[0].value.size() == 510);
    check("TLV8 an empty value is a type and a zero length", tlv_encode({{0xFF, ""}}) == Bytes("\xff\x00", 2));
    check("TLV8 a separator keeps two items of one type apart",
          tlv_decode(tlv_encode({{1, "a"}, {0xFF, ""}, {1, "b"}}), items) && items.size() == 3 && items[0].value == "a" &&
              items[2].value == "b");
    check("TLV8 neighbours of one type join only after a full 255-byte fragment",
          tlv_decode(Bytes("\x01\x01" "a" "\x01\x01" "b", 6), items) && items.size() == 2);
    check("TLV8 a truncated item is refused (aiohomekit)", !tlv_decode(H("060103" "09FF" + std::string(50, '6')), items) &&
                                                               !tlv_decode(Bytes("\x06", 1), items));
    check("TLV8 find returns the first item of a type",
          tlv_decode(tlv_encode({{6, "\x02"}, {7, "\x06"}, {7, "\x02"}}), items) && tlv_find(items, 7) &&
              *tlv_find(items, 7) == "\x06" && !tlv_find(items, 3));
}

// ---------------------------------------------------------------------------------------------
// The server side of SRP, written here with BIGNUM so the client can be checked against it.
struct SrpServerSide {
    Bytes salt, B;   // B without leading zeros, as the ADK sends it
    BIGNUM *v = nullptr, *b = nullptr;
    ~SrpServerSide() {
        BN_free(v);
        BN_free(b);
    }
};

static Bytes pad384(const BIGNUM *x) {
    Bytes out(384, '\0');
    BN_bn2binpad(x, reinterpret_cast<unsigned char *>(&out[0]), 384);
    return out;
}

static BIGNUM *bn_of(const Bytes &b) { return BN_bin2bn(reinterpret_cast<const unsigned char *>(b.data()), static_cast<int>(b.size()), nullptr); }

static void srp_server(const std::string &user, const std::string &pass, const Bytes &salt, unsigned long b_word,
                       SrpServerSide &s) {
    BN_CTX *ctx = BN_CTX_new();
    BIGNUM *N = bn_of(srp_modulus()), *g = BN_new(), *x = bn_of(sha512(salt + sha512(user + ":" + pass))), *k = bn_of(srp_k()),
           *t = BN_new(), *B = BN_new();
    s.salt = salt;
    s.v = BN_new();
    s.b = BN_new();
    BN_set_word(g, 5);
    BN_set_word(s.b, b_word);
    BN_mod_exp(s.v, g, x, N, ctx);
    BN_mod_mul(B, k, s.v, N, ctx);
    BN_mod_exp(t, g, s.b, N, ctx);
    BN_mod_add(B, B, t, N, ctx);
    s.B = Bytes(BN_num_bytes(B), '\0');
    BN_bn2bin(B, reinterpret_cast<unsigned char *>(&s.B[0]));
    BN_free(N), BN_free(g), BN_free(x), BN_free(k), BN_free(t), BN_free(B);
    BN_CTX_free(ctx);
}

// K the server derives from the client's A.
static Bytes srp_server_key(const SrpServerSide &s, const Bytes &A) {
    BN_CTX *ctx = BN_CTX_new();
    BIGNUM *N = bn_of(srp_modulus()), *a = bn_of(A), *u = bn_of(sha512(A + Bytes(384 - s.B.size(), '\0') + s.B)),
           *t = BN_new(), *S = BN_new();
    BN_mod_exp(t, s.v, u, N, ctx);
    BN_mod_mul(t, a, t, N, ctx);
    BN_mod_exp(S, t, s.b, N, ctx);
    const Bytes K = sha512(pad384(S));
    BN_free(N), BN_free(a), BN_free(u), BN_free(t), BN_free(S);
    BN_CTX_free(ctx);
    return K;
}

static void test_srp() {
    const Bytes N = srp_modulus();
    check("SRP modulus is the RFC 5054 3072-bit group", N.size() == 384 && hex(N.substr(0, 8)) == "ffffffffffffffff" &&
                                                            hex(N.substr(8, 8)) == "c90fdaa22168c234" &&
                                                            hex(N.substr(376)) == "ffffffffffffffff");
    eq("SRP k = H(N | PAD(g)) equals aiohomekit's CLIENT_K_VALUE", hex(srp_k()),
       "a9c2e2559bf0ebb53f0cbbf62282906bede7f2182f00678211fbd5bde5b285033a4993503b87397f9be5ec02080fedbc0835587ad039060879b8621e8c3659e0");

    // ADK HAPCryptoTest.c
    const Bytes salt = H("beb25379d1a8581eb5a727673a2441ee");
    const Bytes a = H("60975527035CF2AD1989806F0407210BC81EDC04E2762A56AFD529DDDA2D4393");
    const Bytes B = H(
        "40f57088a482d4c7733384fe0d301fddca9080ad7d4f6fdf09a01006c3cb6d562e41639ae8fa21de3b5dba7585b275589bdb279863c56280"
        "7b2b99083cd1429cdbe89e25bfbd7e3cad3173b2e3c5a0b174da6d5391e6a06e465f037a4006254839a56bf76da84b1c94e0ae208576156f"
        "e5c140a4ba4ffc9e38c3b07b88845fc6f7ddda93381fe0ca6084c4cd2d336e5451c464ccb6ec65e7d16e548a273e826284af2559b6264274"
        "215960fff47bdd63d3aff064d6137af769661c9d4fee47382603c88eaa0980581d07758461b777e4356dda5835198b51feea308d70f75450"
        "b71675c08c7d8302fd7539dd1ff2a11cb4258aa70d234436aa42b6a0615f3f915d55cc3b966b2716b36e4d1a06ce5e5d2ea3bee5a1270e87"
        "51da45b60b997b0ffdb0f9962fee4f03bee780ba0a845b1d9271421783ae6601a61ea2e342e4f2e8bc935a409ead19f221bd1b74e2964dd1"
        "9fc845f60efc09338b60b6b256d8cac889cca306cc370a0b18c8b886e95da0af5235fef4393020d2b7f3056904759042");
    SrpProof p;
    const bool ok = srp_client("alice", "password123", salt, B, a, p);
    check("SRP-6a client runs on the ADK vector", ok);
    eq("SRP-6a A = g^a (ADK)", hex(p.A),
       "fab6f5d2615d1e323512e7991cc37443f487da604ca8c9230fcb04e541dce6280b27ca4680b0374f179dc3bdc7553fe62459798c701ad864"
       "a91390a28c93b644adbf9c00745b942b79f9012a21b9b78782319d83a1f8362866fbd6f46bfc0ddb2e1ab6e4b45a9906b82e37f05d6f97f6"
       "a3eb6e182079759c4f6847837b62321ac1b4fa68641fcb4bb98dd697a0c73641385f4bab25b793584cc39fc8d48d4bd867a9a3c10f8ea121"
       "70268e34fe3bbe6ff89998d60da2f3e4283cbec1393d52af724a57230c604e9fbce583d7613e6bffd67596ad121a8707eec469449570336"
       "86a155f644d5c5863b48f61bdbf19a53eab6dad0a186b8c152e5f5d8cad4b0ef8aa4ea5008834c3cd342e5e0f167ad04592cd8bd2796393"
       "98ef9e114dfaaab919e14e850989224ddd98576d79385d2210902e9f9b1f2d86cfa47ee244635465f71058421a0184be51dd10cc9d079e6f"
       "1604e7aa9b7cf7883c7d4ce12b06ebe16081e23f27a231d18432d7d1bb55c28ae21ffcf005f57528d15a88881bb3bbb7fe");
    eq("SRP-6a session key K = H(S) (ADK)", hex(p.K),
       "5cbc219db052138ee1148c71cd4498963d682549ce91ca24f098468f06015beb6af245c2093f98c3651bca83ab8cab2b580bbf02184fefdf26142f73df95ac50");
    eq("SRP-6a proof M1 (ADK)", hex(p.M1),
       "5f7c14ab57ed0e94fd1d78c6b4dd09ed7e340b7e05d419a9fd760f6b35e523d1310777a1ae1d2826f596f3a85116cc457c7c964d4f44ded5559da818c88b617f");
    eq("SRP-6a expected accessory proof M2 (ADK)", hex(p.M2),
       "2fa0e81f5cb73b88fa0964270f321dd641f2227a5d805c40f1bfe96aaf6a19ffce8e23287965a39eab9d5a02215f89e128177ed2c4f103e655a045531bcbf7ad");

    // aiohomekit: zero salt, b = 1336, code 123-45-678 gives a B one byte short.
    SrpServerSide srv;
    srp_server("Pair-Setup", "123-45-678", Bytes(16, '\0'), 1336, srv);
    check("SRP-6a test accessory: zero salt and b = 1336 give a 383-byte B (aiohomekit)", srv.B.size() == 383);
    SrpProof stripped, full;
    const Bytes ca = H("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    check("SRP-6a client treats B with its leading zero stripped like the padded B",
          srp_client("Pair-Setup", "123-45-678", srv.salt, srv.B, ca, stripped) &&
              srp_client("Pair-Setup", "123-45-678", srv.salt, Bytes(1, '\0') + srv.B, ca, full) && stripped.M1 == full.M1 &&
              stripped.K == full.K);
    check("SRP-6a client and server derive the same key", srp_server_key(srv, stripped.A) == stripped.K);
    SrpProof wrong;
    check("SRP-6a a wrong code gives another proof and key",
          srp_client("Pair-Setup", "123-45-679", srv.salt, srv.B, ca, wrong) && wrong.M1 != stripped.M1 &&
              srp_server_key(srv, wrong.A) != wrong.K);
    SrpProof random1, random2;
    check("SRP-6a picks a fresh secret each time when none is given",
          srp_client("Pair-Setup", "123-45-678", srv.salt, srv.B, "", random1) &&
              srp_client("Pair-Setup", "123-45-678", srv.salt, srv.B, "", random2) && random1.A != random2.A &&
              srp_server_key(srv, random1.A) == random1.K);
    SrpProof none;
    check("SRP-6a refuses B = 0, B = N, B longer than 384 bytes, an empty or huge salt",
          !srp_client("Pair-Setup", "1", salt, Bytes(384, '\0'), a, none) && !srp_client("Pair-Setup", "1", salt, N, a, none) &&
              !srp_client("Pair-Setup", "1", salt, Bytes(385, '\x01'), a, none) &&
              !srp_client("Pair-Setup", "1", "", B, a, none) && !srp_client("Pair-Setup", "1", Bytes(65, 's'), B, a, none));
}

// ---------------------------------------------------------------------------------------------
static void test_setup_codes() {
    struct Case {
        const char *in, *want;   // want "" = refused
    } const cases[] = {
        {"518-08-582", "518-08-582"},
        {"51808582", "518-08-582"},
        {" 518 08 582 ", "518-08-582"},
        {"X-HM://0071WK4SM7OSX", "518-08-582"},     // ADK: category 7, IP
        {"x-hm://0076cdmx27osx", "518-08-582"},     // ADK: BLE flag; lower case as some readers give it
        {"X-HM://0071WK4SM", "518-08-582"},         // without the setup id
        {"X-HM://00711PP1C0000", ""},               // ADK: no setup code in it
        {"X-HM://00739MG3K0000", ""},               // ADK: paired, no code
        {"1234567", ""},
        {"123456789", ""},
        {"12a-45-678", ""},
        {"", ""},
        {"X-HM://0071WK4S", ""},
        {"X-HM://0071WK4S!7OSX", ""},
        {"X-HM://0071WK4SM7OSXEXTRA", ""},
    };
    for (const Case &c : cases) {
        std::string code, err;
        const bool ok = parse_setup_code(c.in, code, err);
        if (*c.want)
            check(std::string("setup code \"") + c.in + "\" reads as " + c.want, ok && code == c.want, ok ? code : err);
        else
            check(std::string("setup code \"") + c.in + "\" is refused with a sentence", !ok && err.size() > 20 && err.back() == '.');
    }
}

// ---------------------------------------------------------------------------------------------
static void test_frames() {
    // Known answers from mock_homekit.py's pure-Python ChaCha20-Poly1305.
    Cipher out{range(0, 32), 0};
    const Bytes req = "GET /accessories HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    const Bytes f0 = seal_frames(out, req), f1 = seal_frames(out, req);
    eq("session frame 0: length, ciphertext, tag (mock)", hex(f0),
       "2e005ffd16118287c5b276122f0edd2a2b54d8f9a7a1b1826a72ddf5f65a4546016652393a5affde126db3b0552e69048fcee049a959fd6806e95d4b4a03eac1");
    eq("session frame 1 uses the next counter (mock)", hex(f1),
       "2e00d812a77f9a4bcc7b4cc5dd3320fcfcf4e535bf5a17445183f269bf13e29ba99ef87be49bcaf8f1bbaa1af995868e43c3574a058935f9e5f588a68223ccdc");
    Cipher big{range(0, 32), 0};
    const Bytes text(2500, 'x');
    const Bytes wire = seal_frames(big, text);
    auto len_at = [&](size_t pos) { return static_cast<size_t>(static_cast<uint8_t>(wire[pos]) | static_cast<uint8_t>(wire[pos + 1]) << 8); };
    check("2500 bytes go as frames of 1024, 1024 and 452, each with a 16-byte tag",
          wire.size() == 2500 + 3 * 18 && len_at(0) == 1024 && len_at(1042) == 1024 && len_at(2084) == 452 && big.counter == 3);
    Cipher in{range(0, 32), 0};
    Bytes plain;
    size_t pos = 0;
    const Bytes partial = wire.substr(0, wire.size() - 1);
    check("frames are opened as they complete; an incomplete one waits",
          open_frames(in, partial, pos, plain) && plain.size() == 2048 && pos == 2084 && in.counter == 2);
    check("the last frame opens once it is complete", open_frames(in, wire, pos, plain) && plain == text && in.counter == 3);
    Cipher replay{range(0, 32), 1};
    size_t p2 = 0;
    check("a frame under the wrong counter is refused", !open_frames(replay, f0, p2, plain));
    Bytes tampered = f0;
    tampered[5] ^= 1;
    Cipher fresh{range(0, 32), 0};
    p2 = 0;
    check("a changed frame is refused", !open_frames(fresh, tampered, p2, plain));
    Bytes lying = f0;
    lying[0] = '\x30';   // claims 48 bytes: the tag no longer matches
    fresh = Cipher{range(0, 32), 0};
    p2 = 0;
    Bytes lying_more = lying + Bytes(4, '\0');
    check("a frame with a changed length is refused (the length is authenticated)", !open_frames(fresh, lying_more, p2, plain));
    Bytes huge("\x01\x04", 2);   // 1025 bytes
    fresh = Cipher{range(0, 32), 0};
    p2 = 0;
    check("a frame longer than 1024 bytes is refused before it arrives", !open_frames(fresh, huge, p2, plain));
}

// ---------------------------------------------------------------------------------------------
static int feed_all(HttpReader &r, const Bytes &buf, size_t &pos) { return r.feed(buf, pos); }

static void test_http() {
    {
        HttpReader r(1000);
        const Bytes b = "HTTP/1.1 200 OK\r\nContent-Type: application/hap+json\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
        size_t pos = 0;
        check("HTTP response with Content-Length; following bytes are left alone",
              feed_all(r, b, pos) == 1 && r.msg.status == 200 && r.msg.body == "hello" && pos == b.size() - 5 &&
                  r.msg.headers["content-type"] == "application/hap+json");
    }
    {
        HttpReader r(1000);
        const Bytes b = "HTTP/1.1 204 No Content\r\n\r\n";
        size_t pos = 0;
        check("HTTP 204 without a body", feed_all(r, b, pos) == 1 && r.msg.status == 204 && r.msg.body.empty());
    }
    {
        HttpReader r(1000);
        const Bytes b = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nX-T: 1\r\n\r\n";
        size_t pos = 0;
        check("HTTP chunked body with an extension and a trailer", feed_all(r, b, pos) == 1 && r.msg.body == "hello world" &&
                                                                       pos == b.size());
    }
    {
        // The same, one byte at a time.
        const Bytes b = "EVENT/1.0 200 OK\r\nContent-Length: 2\r\n\r\n{}HTTP/1.1 207 Multi-Status\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
        Bytes buf;
        size_t pos = 0;
        HttpReader first(1000), second(1000);
        int r1 = 0, r2 = 0;
        size_t i = 0;
        for (; i < b.size() && r1 == 0; i++) {
            buf += b[i];
            r1 = first.feed(buf, pos);
        }
        for (; i < b.size() && r2 == 0; i++) {
            buf += b[i];
            r2 = second.feed(buf, pos);
        }
        check("an EVENT message, then a response, fed one byte at a time",
              r1 == 1 && first.msg.event && first.msg.body == "{}" && r2 == 1 && !second.msg.event && second.msg.status == 207 &&
                  second.msg.body == "abc");
    }
    struct Bad {
        const char *what;
        Bytes in;
    } const bads[] = {
        {"a status line that is not HTTP", "SIP/2.0 200 OK\r\n\r\n"},
        {"a status code that is not three digits", "HTTP/1.1 2x0 OK\r\n\r\n"},
        {"a Content-Length beyond the limit", "HTTP/1.1 200 OK\r\nContent-Length: 1099511627776\r\n\r\n"},
        {"a Content-Length that is not a number", "HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\n"},
        {"two different Content-Lengths", "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"},
        {"a chunk larger than the limit", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFFFF\r\n"},
        {"a chunk size that is not hexadecimal", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"},
        {"a chunk not followed by CRLF", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabXY\r\n"},
        {"a header without a colon", "HTTP/1.1 200 OK\r\nNoColonHere\r\n\r\n"},
        {"a header line of 9000 bytes", "HTTP/1.1 200 OK\r\nX: " + Bytes(9000, 'a') + "\r\n\r\n"},
        {"a status line that never ends", Bytes(9000, 'H')},
    };
    for (const Bad &b : bads) {
        HttpReader r(1000);
        size_t pos = 0;
        check("HTTP refuses " + std::string(b.what), r.feed(b.in, pos) == -1);
    }
    {
        Bytes many = "HTTP/1.1 200 OK\r\n";
        for (int i = 0; i < 101; i++) many += "X-" + std::to_string(i) + ": 1\r\n";
        HttpReader r(1000);
        size_t pos = 0;
        check("HTTP refuses more than 100 headers", r.feed(many + "\r\n", pos) == -1);
    }
    {
        HttpReader r(10);
        size_t pos = 0;
        const Bytes b = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n8\r\n12345678\r\n8\r\n12345678\r\n0\r\n\r\n";
        check("HTTP refuses chunks that add up beyond the limit", r.feed(b, pos) == -1);
    }
}

// ---------------------------------------------------------------------------------------------
static void test_text() {
    eq("invalid UTF-8 bytes become U+FFFD", hex(sanitize_utf8("a\xff" "b")), hex("a\xEF\xBF\xBD" "b"));
    eq("an overlong encoding is refused", hex(sanitize_utf8("\xC0\x80")), hex("\xEF\xBF\xBD\xEF\xBF\xBD"));
    eq("a UTF-16 surrogate is refused", hex(sanitize_utf8("\xED\xA0\x80")), hex("\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));
    eq("valid UTF-8 is kept", sanitize_utf8("Caf\xC3\xA9 \xE2\x98\x95 \xF0\x9F\x92\xA1"), "Caf\xC3\xA9 \xE2\x98\x95 \xF0\x9F\x92\xA1");
    eq("a truncated sequence at the end is replaced", hex(sanitize_utf8("ok\xE2\x98")), hex("ok\xEF\xBF\xBD\xEF\xBF\xBD"));
    eq("names: control characters dropped, tabs and runs of spaces become one space, trimmed",
       clean_name("  \x01Living\t\tRoom \x7f Lamp\r\n "), "Living Room Lamp");
    eq("names: bidirectional overrides, zero-width characters and U+FFFD dropped",
       clean_name("Lamp\xE2\x80\xAE" "1\xE2\x80\x8B" "2\xFF" "3"), "Lamp123");
    std::string accents;
    for (int i = 0; i < 50; i++) accents += "\xC3\xA9";
    eq("names: at most 40 characters, never half a character", clean_name(accents), accents.substr(0, 80));
    eq("names: no space left at the end after cutting", clean_name(std::string(39, 'a') + " bcd"), std::string(39, 'a'));
    check("names: C1 control characters dropped", clean_name("A\xC2\x81" "B") == "AB");
    // Unicode White_Space is a space: collapsed, and trimmed at both ends (U+3000, U+2003, U+1680,
    // U+202F, U+205F, U+0085).
    eq("names: Unicode spaces are trimmed and collapsed",
       clean_name("\xE3\x80\x80Lamp\xE2\x80\x83\xE1\x9A\x80" "One\xE2\x80\xAF\xE2\x81\x9F\xC2\x85\xE3\x80\x80"), "Lamp One");
    // Format and invisible characters: soft hyphen, combining grapheme joiner, Arabic letter mark,
    // Hangul fillers, Mongolian vowel separator, U+206A, variation selector, tags, musical format.
    eq("names: invisible and format characters dropped",
       clean_name("L\xC2\xAD" "a\xCD\x8F" "m\xD8\x9C" "p\xE1\xA0\x8E\xE2\x81\xAA\xEF\xB8\x8F\xF3\xA0\x81\x81\xF3\xA0\x81\x82"
                  "\xF0\x9D\x85\xB3!"),
       "Lamp!");
    check("names: a name of nothing but blank fillers comes out empty (callers then use their own)",
          clean_name("\xE3\x85\xA4") == "" && clean_name("\xE1\x85\x9F\xEF\xBE\xA0 \xE2\x80\x8B") == "" &&
              clean_name("\xEF\xBF\xBE\xEF\xB7\x90") == "");
    eq("names: other scripts and emoji are kept", clean_name("Caf\xC3\xA9 \xE7\x81\xAF \xF0\x9F\x92\xA1"),
       "Caf\xC3\xA9 \xE7\x81\xAF \xF0\x9F\x92\xA1");
    // A reply's nesting is checked with support::json_too_deep before it is parsed. A stray closing bracket
    // counts below zero there, but a text with one is no JSON, and is refused all the same.
    const Bytes deep32 = std::string(32, '[') + std::string(32, ']'), deep33 = "[" + deep32 + "]";
    const Bytes in_string = "{\"a\":\"" + std::string(40, '[') + "\\\"" + std::string(40, '{') + "\"}";
    json reply;
    check("JSON nesting of 32 is accepted, 33 refused", parse_reply_json(deep32, 1000, reply) && !parse_reply_json(deep33, 1000, reply));
    check("JSON brackets inside strings do not count", parse_reply_json(in_string, 1000, reply) && reply.is_object());
    check("a stray closing bracket does not raise the limit",
          !parse_reply_json("]" + deep33, 1000, reply) && !parse_reply_json(deep33 + "]", 1000, reply));
    eq("short type of a full Apple UUID", short_type("0000004A-0000-1000-8000-0026BB765291"), "4A");
    eq("short type in lower case with leading zeros", short_type("0043"), "43");
    eq("short type of zero", short_type("00000000-0000-1000-8000-0026BB765291"), "0");
    eq("a maker's own type stays long", short_type("E863F007-079E-48FF-8F27-9C2605A29F52"), "E863F007-079E-48FF-8F27-9C2605A29F52");
    eq("a short type that is not hexadecimal stays as it is", short_type("4G"), "4G");
}

// ---------------------------------------------------------------------------------------------
static json C(int iid, const char *type, const char *fmt, json perms, json value = nullptr, json extra = json::object()) {
    json c = {{"iid", iid}, {"type", type}, {"format", fmt}, {"perms", perms}};
    if (!value.is_null()) c["value"] = value;
    for (auto it = extra.begin(); it != extra.end(); ++it) c[it.key()] = it.value();
    return c;
}

static json info(const std::string &name) {
    return {{"iid", 1},
            {"type", "3E"},
            {"characteristics", json::array({C(2, "14", "bool", {"pw"}), C(3, "23", "string", {"pr"}, name),
                                             C(4, "21", "string", {"pr"}, "Model " + name), C(5, "30", "string", {"pr"}, "SN-" + name),
                                             C(6, "52", "string", {"pr"}, "1.0")})}};
}

static json acc(int aid, const std::string &name, json services) {
    json s = json::array({info(name)});
    for (const json &x : services) s.push_back(x);
    return {{"aid", aid}, {"services", s}};
}

static const json PRW = {"pr", "pw", "ev"}, PR = {"pr", "ev"};

static json fixture() {
    json accs = json::array();
    accs.push_back(acc(1, "Bridge", json::array()));
    accs.push_back(acc(2, "Hall Lamp", json::array({json{{"iid", 10}, {"type", "00000043-0000-1000-8000-0026BB765291"}, {"characteristics", json::array({
        C(11, "25", "bool", PRW, true), C(12, "8", "int", PRW, 40, {{"minValue", 0}, {"maxValue", 100}, {"minStep", 1}}),
        C(13, "13", "float", PRW, 30, {{"minValue", 0}, {"maxValue", 360}, {"minStep", 1}}),
        C(14, "2F", "float", PRW, 50, {{"minValue", 0}, {"maxValue", 100}, {"minStep", 1}}),
        C(15, "CE", "uint32", PRW, 370, {{"minValue", 140}, {"maxValue", 500}, {"minStep", 1}})})}}})));
    accs.push_back(acc(3, "Kettle", json::array({json{{"iid", 10}, {"type", "47"}, {"characteristics", json::array({
        C(11, "25", "bool", PRW, false), C(12, "26", "bool", PR, false)})}}})));
    accs.push_back(acc(4, "Heating", json::array({json{{"iid", 10}, {"type", "4A"}, {"characteristics", json::array({
        C(11, "F", "uint8", PR, 1), C(12, "33", "uint8", PRW, 1, {{"minValue", 0}, {"maxValue", 3}, {"valid-values", {0, 1, 3}}}),
        C(13, "11", "float", PR, 19.5), C(14, "35", "float", PRW, 20, {{"minValue", 5}, {"maxValue", 32}, {"minStep", 0.5}})})}}})));
    accs.push_back(acc(5, "Lock", json::array({json{{"iid", 10}, {"type", "45"}, {"characteristics", json::array({C(11, "1E", "uint8", PRW, 1)})}},
                                                json{{"iid", 20}, {"type", "49"}, {"characteristics", json::array({C(21, "25", "bool", PRW, false)})}}})));
    accs.push_back(acc(6, "Garage", json::array({json{{"iid", 10}, {"type", "41"}, {"characteristics", json::array({C(11, "32", "uint8", PRW, 1)})}},
                                                  json{{"iid", 20}, {"type", "43"}, {"characteristics", json::array({C(21, "25", "bool", PRW, false)})}}})));
    accs.push_back(acc(7, "Tap", json::array({json{{"iid", 10}, {"type", "000000D0-0000-1000-8000-0026BB765291"}, {"characteristics", json::array({C(11, "B0", "uint8", PRW, 0)})}}})));
    accs.push_back(acc(8, "Gadget", json::array({json{{"iid", 10}, {"type", "E863F007-079E-48FF-8F27-9C2605A29F52"}, {"characteristics", json::array({C(11, "25", "bool", PRW, false)})}}})));
    accs.push_back(acc(9, "Desk", json::array({json{{"iid", 10}, {"type", "43"}, {"hidden", true}, {"characteristics", json::array({C(11, "25", "bool", PRW, false)})}},
                                                json{{"iid", 20}, {"type", "47"}, {"characteristics", json::array({C(21, "25", "bool", PRW, true)})}}})));
    accs.push_back(acc(10, "Heater", json::array({json{{"iid", 10}, {"type", "BC"}, {"characteristics", json::array({
        C(11, "B0", "uint8", PRW, 1), C(12, "B1", "uint8", PR, 2), C(13, "B2", "uint8", PRW, 1, {{"valid-values", {1}}}),
        C(14, "11", "float", PR, 17), C(15, "12", "float", PRW, 18, {{"minValue", 0}, {"maxValue", 25}, {"minStep", 0.5}})})}}})));
    accs.push_back(acc(11, "Fan", json::array({json{{"iid", 10}, {"type", "B7"}, {"characteristics", json::array({
        C(11, "B0", "uint8", PRW, 1), C(12, "29", "float", PRW, 2, {{"minValue", 0}, {"maxValue", 3}, {"minStep", 1}})})}}})));
    accs.push_back(acc(12, "Strip", json::array({
        json{{"iid", 10}, {"type", "47"}, {"characteristics", json::array({C(11, "25", "bool", PRW, false), C(12, "23", "string", {"pr"}, "Left")})}},
        json{{"iid", 20}, {"type", "47"}, {"characteristics", json::array({C(21, "25", "bool", PRW, false), C(22, "23", "string", {"pr"}, "Left")})}},
        json{{"iid", 30}, {"type", "47"}, {"characteristics", json::array({C(31, "25", "bool", PRW, false)})}}})));
    accs.push_back(acc(13, "Hall", json::array({
        json{{"iid", 10}, {"type", "8A"}, {"characteristics", json::array({C(11, "11", "float", PR, 18.25), C(12, "23", "string", {"pr"}, "Temperature")})}},
        json{{"iid", 20}, {"type", "80"}, {"characteristics", json::array({C(21, "6A", "uint8", PR, 0), C(22, "E3", "string", {"pr", "pw"}, "Hall door")})}}})));
    accs.push_back(acc(14, "Broken Light", json::array({json{{"iid", 10}, {"type", "43"}, {"characteristics", json::array({C(11, "8", "int", PR, 3)})}}})));
    accs.push_back(acc(15, "Camera", json::array({json{{"iid", 10}, {"type", "110"}, {"characteristics", json::array()}},
                                                   json{{"iid", 20}, {"type", "85"}, {"characteristics", json::array({C(21, "22", "bool", PR, true)})}}})));
    return {{"accessories", accs}};
}

static const home::Thing *find(const std::vector<home::Thing> &ts, const std::string &id) {
    for (const home::Thing &t : ts)
        if (t.id == id) return &t;
    return nullptr;
}

static std::string ids(const std::vector<home::Thing> &ts) {
    std::string s;
    for (const home::Thing &t : ts) s += (s.empty() ? "" : " ") + t.id;
    return s;
}

static void test_database() {
    std::vector<home::Thing> ts;
    check("database: parsed", things_from_database(fixture(), ts));
    eq("database: only allowed services; lock, garage, tap, gadget, hidden, camera and a light without On left out", ids(ts),
       "2.10 3.10 4.10 9.20 10.10 11.10 12.10 12.20 12.30 13.10 13.20");
    const home::Thing *lamp = find(ts, "2.10"), *kettle = find(ts, "3.10"), *heat = find(ts, "4.10"), *heater = find(ts, "10.10"),
                      *fan = find(ts, "11.10"), *door = find(ts, "13.20");
    check("database: the lamp is a light with power, brightness, white and colour, named after its accessory",
          lamp && lamp->type == "light" && lamp->name == "Hall Lamp" &&
              lamp->caps == (home::kPower | home::kBrightness | home::kColorTemp | home::kColor) && !lamp->ask_first);
    check("database: the lamp's state from the database",
          lamp && lamp->state.value("on", false) && lamp->state.value("brightness", 0) == 40 && lamp->state.value("kelvin", 0) == 2703 &&
              lamp->state.value("hue", 0.0) == 30.0);
    check("database: the plug", kettle && kettle->type == "plug" && kettle->caps == home::kPower &&
                                    kettle->state.value("on", true) == false && kettle->state.value("in_use", true) == false);
    check("database: the thermostat with target, mode and reading (current_c, as the app reads it)",
          heat && heat->type == "thermostat" && heat->caps == (home::kTargetTemp | home::kSensor) &&
              heat->state.value("target_c", 0.0) == 20.0 && heat->state.value("current_c", 0.0) == 19.5 &&
              !heat->state.contains("temperature") && heat->state.value("mode", "") == "heat" &&
              heat->state.value("heating", false),
          heat ? heat->state.dump() : "");
    check("database: a heater-cooler is a thermostat that asks before switching on",
          heater && heater->type == "thermostat" && heater->ask_first &&
              heater->caps == (home::kPower | home::kTargetTemp | home::kSensor) && heater->state.value("mode", "") == "heat" &&
              heater->state.value("heating", false) && heater->state.value("on", false));
    check("database: a three-speed fan reports its speed as a percentage",
          fan && fan->type == "fan" && fan->caps == (home::kPower | home::kFanSpeed) && fan->state.value("fan", 0) == 67);
    check("database: names of a strip's sockets: the accessory's name first, duplicates numbered",
          find(ts, "12.10") && find(ts, "12.10")->name == "Strip Left" && find(ts, "12.20")->name == "Strip Left 2" &&
              find(ts, "12.30")->name == "Strip 3");
    check("database: a sensor named by its service, and a configured name preferred",
          find(ts, "13.10") && find(ts, "13.10")->name == "Hall Temperature" && door && door->name == "Hall door" &&
              door->type == "sensor" && door->caps == home::kSensor);
    check("database: a contact sensor's state is contact \"closed\" or \"open\", a temperature sensor's current_c",
          door && door->state == json({{"contact", "closed"}}) && find(ts, "13.10") &&
              find(ts, "13.10")->state == json({{"current_c", 18.3}}),
          door ? door->state.dump() + " " + find(ts, "13.10")->state.dump() : "");
    // Every key a thing's state can have is one the app shows (src/smarthome/home.h; control.cpp
    // shown_state keeps only those) or one of this driver's own, documented in homekit.h.
    static const std::set<std::string> kAppKeys = {"on", "brightness", "kelvin", "hue", "saturation", "mode", "target_c",
                                                   "current_c", "humidity", "position", "fan", "volume", "muted", "playing",
                                                   "source", "title", "power_w", "energy_today_wh", "motion", "contact"};
    static const std::set<std::string> kOwnKeys = {"in_use", "heating", "target_position", "reachable"};
    std::string strays;
    for (const home::Thing &t : ts)
        for (auto it = t.state.begin(); it != t.state.end(); ++it)
            if (!kAppKeys.count(it.key()) && !kOwnKeys.count(it.key())) strays += t.id + ":" + it.key() + " ";
    check("database: every state key is one the app shows, or one of the driver's own", strays.empty(), strays);
    check("database: each characteristic keeps its type, each thing its accessory's model and serial number",
          lamp && lamp->ref["c"]["on"].value("t", "") == "25" && lamp->ref["c"]["bri"].value("t", "") == "8" &&
              lamp->ref["ais"].contains("21") && lamp->ref["ais"].contains("30") && lamp->ref["ais"]["21"][0] == 4 &&
              lamp->ref["ais"]["21"][1].get<std::string>().size() == 16 &&
              lamp->ref["ais"]["21"][1] != lamp->ref["ais"]["30"][1],
          lamp ? lamp->ref.dump() : "");
    check("database: the plug next to a hidden light is named after the accessory", find(ts, "9.20") && find(ts, "9.20")->name == "Desk");
    check("database: rooms are not on the wire", lamp && lamp->room.empty());

    // Hostile shapes: nothing throws, nothing bogus comes out.
    json wrong = {{"accessories", json::array({
        json{{"aid", "2"}, {"services", json::array({json{{"iid", 10}, {"type", 43}, {"characteristics", "none"}}})}},
        json{{"aid", 3}, {"services", json{{"iid", 10}}}},
        json{{"aid", 4}, {"services", json::array({json{{"iid", "x"}, {"type", "43"}, {"characteristics", json::array()}}})}},
        json{{"aid", 5}, {"services", json::array({json{{"iid", 10}, {"type", "43"}, {"characteristics", json::array({
            json{{"iid", "11"}, {"type", "25"}, {"format", "bool"}, {"perms", {"pr", "pw"}}},
            json{{"iid", 12}, {"type", {"8"}}, {"format", "int"}, {"perms", "pr"}},
            json{{"iid", 13}, {"type", "25"}, {"format", 7}, {"perms", {1, nullptr, "pw"}}, {"value", {{"x", 1}}}},
            json{{"iid", 14}, {"type", "8"}, {"format", "int"}, {"perms", {"pr", "pw"}}, {"minValue", "low"}, {"maxValue", {100}},
                 {"minStep", nullptr}, {"valid-values", "all"}, {"value", "40"}}})}}})}},
        json{{"aid", 1e300}, {"services", json::array()}}, "not an accessory", nullptr, 42})}};
    std::vector<home::Thing> wt;
    bool ok = false;
    try {
        ok = things_from_database(wrong, wt);
    } catch (...) {
        ok = false;
    }
    check("database: wrong types everywhere are skipped without an exception", ok && wt.size() <= 1);
    check("database: a light whose On has a bad id and whose brightness has bad ranges keeps only what is sound",
          wt.empty() || (wt[0].id == "5.10" && wt[0].caps == home::kBrightness));
    json absurd = {{"accessories", json::array({acc(30, "Odd", json::array({json{{"iid", 10}, {"type", "4A"}, {"characteristics", json::array({
        C(11, "33", "uint8", PRW, 1, {{"valid-values", {1e300, -1e300, 1, 3}}}),
        C(12, "35", "float", PRW, 20, {{"minValue", -1e308}, {"maxValue", 1e308}, {"minStep", 1e-300}})})}}}))})}};
    std::vector<home::Thing> odd;
    std::string oerr;
    std::vector<Write> ow;
    home::Change oc;
    oc.target_c = 21;
    bool oparsed = things_from_database(absurd, odd) && odd.size() == 1;
    check("database: valid values too large for an integer are dropped",
          oparsed && odd[0].ref["c"]["mode"]["vv"] == json({1, 3}), oparsed ? odd[0].ref.dump() : "");
    check("writes: absurd ranges and steps give a refusal, not a crash or a wild value",
          oparsed && (!writes_for(odd[0], oc, ow, oerr) || (ow.size() == 1 && ow[0].value.get<double>() >= 5 &&
                                                              ow[0].value.get<double>() <= 30)));
    check("status words for an absurd status code", status_text(LONG_MIN, "X") == "X reported an error.");

    // A heater-cooler that can only cool: no heating target to offer (setting one could never work).
    json cooler = {{"accessories", json::array({acc(31, "Bedroom AC", json::array({json{{"iid", 10}, {"type", "BC"}, {"characteristics", json::array({
        C(11, "B0", "uint8", PRW, 1, {{"valid-values", {0, 1}}}), C(12, "B2", "uint8", PRW, 2, {{"valid-values", {2}}}),
        C(13, "11", "float", PR, 24.0), C(14, "D", "float", PRW, 22.0, {{"minValue", 16}, {"maxValue", 30}})})}}})),
                                                acc(32, "Hall Heater", json::array({json{{"iid", 10}, {"type", "BC"}, {"characteristics", json::array({
        C(11, "B0", "uint8", PRW, 1), C(12, "B2", "uint8", PRW, 0, {{"valid-values", {0, 2}}}),
        C(13, "11", "float", PR, 18.0), C(14, "12", "float", PRW, 20.0, {{"minValue", 10}, {"maxValue", 30}})})}}})),
                                                acc(33, "Odd Heater", json::array({json{{"iid", 10}, {"type", "BC"}, {"characteristics", json::array({
        C(11, "B0", "uint8", PRW, 1), C(12, "B2", "uint8", PRW, 2, {{"valid-values", {2}}}),
        C(14, "12", "float", PRW, 20.0, {{"minValue", 10}, {"maxValue", 30}})})}}}))})}};
    std::vector<home::Thing> coolers;
    const bool cparsed = things_from_database(cooler, coolers) && coolers.size() == 3;
    check("database: a heater-cooler that only cools has power but no heating target",
          cparsed && coolers[0].caps == (home::kPower | home::kSensor) && coolers[0].state.value("mode", "") == "cool",
          cparsed ? std::to_string(coolers[0].caps) + " " + coolers[0].state.dump() : "");
    check("database: one that heats in auto keeps its heating target; a heating threshold alone does not make one",
          cparsed && (coolers[1].caps & home::kTargetTemp) && !(coolers[2].caps & home::kTargetTemp),
          cparsed ? std::to_string(coolers[1].caps) + " " + std::to_string(coolers[2].caps) : "");

    // A thermostat that only cools (an air conditioner) is not heating; one with heat or auto is.
    json thermostats = {{"accessories", json::array({acc(34, "Study AC", json::array({json{{"iid", 10}, {"type", "4A"}, {"characteristics", json::array({
        C(11, "33", "uint8", PRW, 2, {{"valid-values", {0, 2}}}), C(12, "11", "float", PR, 24.0),
        C(13, "35", "float", PRW, 22.0, {{"minValue", 16}, {"maxValue", 30}})})}}})),
                                                     acc(35, "Hall Stat", json::array({json{{"iid", 10}, {"type", "4A"}, {"characteristics", json::array({
        C(11, "33", "uint8", PRW, 1, {{"valid-values", {0, 1, 3}}}), C(12, "11", "float", PR, 19.0),
        C(13, "35", "float", PRW, 20.0, {{"minValue", 5}, {"maxValue", 30}})})}}}))})}};
    std::vector<home::Thing> stat_things;
    const bool stats_parsed = things_from_database(thermostats, stat_things) && stat_things.size() == 2;
    check("database: a thermostat that only cools offers no heating target; one that heats does",
          stats_parsed && !(stat_things[0].caps & home::kTargetTemp) && (stat_things[0].caps & home::kSensor) &&
              (stat_things[1].caps & home::kTargetTemp),
          stats_parsed ? std::to_string(stat_things[0].caps) + " " + std::to_string(stat_things[1].caps) : "");

    // A malformed database listing two services under one iid gives one thing, not two with one id.
    json twice = {{"accessories", json::array({acc(36, "Desk", json::array({
        json{{"iid", 10}, {"type", "43"}, {"characteristics", json::array({C(11, "25", "bool", PRW, false)})}},
        json{{"iid", 10}, {"type", "49"}, {"characteristics", json::array({C(11, "25", "bool", PRW, false)})}}}))})}};
    std::vector<home::Thing> once;
    const bool once_parsed = things_from_database(twice, once);
    check("database: two services sharing an iid give one thing", once_parsed && once.size() == 1 && once[0].id == "36.10",
          once_parsed ? std::to_string(once.size()) : "");

    // Readings no thermometer gives never become infinity (round(1e308 / 0.1) overflows).
    json wild = {{"accessories", json::array({acc(40, "Wild", json::array({json{{"iid", 10}, {"type", "4A"}, {"characteristics", json::array({
        C(11, "33", "uint8", PRW, 1), C(12, "11", "float", PR, 1e308), C(13, "35", "float", PRW, -1e308),
        C(14, "10", "float", PR, 1e308)})}}}))})}};
    std::vector<home::Thing> wt2;
    const bool wparsed = things_from_database(wild, wt2) && wt2.size() == 1;
    bool all_finite = wparsed;
    if (wparsed)
        for (auto it = wt2[0].state.begin(); it != wt2[0].state.end(); ++it)
            if (it->is_number() && !std::isfinite(it->get<double>())) all_finite = false;
    check("database: absurd temperatures are left out and humidity is clamped, never infinite",
          all_finite && !wt2[0].state.contains("current_c") && !wt2[0].state.contains("target_c") &&
              wt2[0].state.value("humidity", 0.0) == 100.0,
          wparsed ? wt2[0].state.dump() : "");
    home::Thing back = wparsed ? wt2[0] : home::Thing();
    apply_values(back, {{{40, 12}, json(-1e308)}, {{40, 14}, json(-5.0)}}, {});
    check("values read back: absurd temperatures left out, humidity clamped",
          wparsed && !back.state.contains("current_c") && back.state.value("humidity", -1.0) == 0.0, back.state.dump());
    // Finite but absurd readings too (they round to finite numbers, so only the range keeps them out).
    apply_values(back, {{{40, 12}, json(5000.0)}, {{40, 13}, json(1e300)}}, {});
    check("values read back: a reading of 5000 C or a target of 1e300 is left out, not shown",
          wparsed && !back.state.contains("current_c") && !back.state.contains("target_c"), back.state.dump());
    apply_values(back, {{{40, 12}, json(-40.0)}, {{40, 13}, json(21.5)}}, {});
    check("values read back: real readings, frost included, are kept",
          wparsed && back.state.value("current_c", 0.0) == -40.0 && back.state.value("target_c", 0.0) == 21.5, back.state.dump());

    // A declared range outside the characteristic's format (a hostile uint8 from 1e16) or upside
    // down: what is written stays within the format (found by the safety reviewer's fuzz harness).
    json odd_fan = {{"accessories", json::array({acc(41, "Odd Fan", json::array({json{{"iid", 10}, {"type", "B7"}, {"characteristics", json::array({
        C(11, "B0", "uint8", PRW, 0, {{"minValue", 1e16}}), C(12, "29", "float", PRW, 0, {{"minValue", 100}, {"maxValue", 0}})})}}}))})}};
    std::vector<home::Thing> oddt;
    std::vector<Write> ow2;
    std::string oerr2;
    home::Change spin;
    spin.on = true;
    const bool oddok = things_from_database(odd_fan, oddt) && oddt.size() == 1 && writes_for(oddt[0], spin, ow2, oerr2) &&
                       ow2.size() == 1;
    check("writes: a uint8 whose declared range lies outside 0..255 is still written within 0..255",
          oddok && ow2[0].value == json(1), oddok ? ow2[0].value.dump() : oerr2);
    // The deny rule fails closed: a lock service whose type is a number, has a stray space, or is
    // missing cannot be recognised, so the accessory goes; and an aid listed twice (the garage door
    // with its light, then the light alone) is skipped in every listing.
    auto lock_with = [&](int aid, json type) {
        json lock_svc = {{"iid", 10}, {"characteristics", json::array({C(11, "1E", "uint8", PRW, 1)})}};
        if (!type.is_null()) lock_svc["type"] = type;
        return acc(aid, "Front Door", json::array({lock_svc, json{{"iid", 20}, {"type", "49"}, {"characteristics", json::array({
            C(21, "25", "bool", PRW, false)})}}}));
    };
    json malformed = {{"accessories", json::array({lock_with(50, 45), lock_with(51, " 45"), lock_with(52, nullptr),
                                                   lock_with(53, "45 "), lock_with(54, "0x45"), lock_with(55, "000000045"),
                                                   lock_with(56, "0000004-50000-1000-8000-0026BB765291"),
                                                   lock_with(57, json::array({"45"})),
                                                   acc(58, "Plain", json::array({json{{"iid", 10}, {"type", "0000004a-0000-1000-8000-0026bb765291"}, {"characteristics", json::array({
                                                       C(11, "33", "uint8", PRW, 1)})}},
                                                       json{{"iid", 20}, {"type", "e863f007-079e-48ff-8f27-9c2605a29f52"}, {"characteristics", json::array()}}}))})}};
    std::vector<home::Thing> mt;
    const bool mparsed = things_from_database(malformed, mt);
    check("database: a service type that is not a well-formed HAP type (a number, \" 45\", none, \"0x45\", 9 digits, a "
          "misplaced dash, a list) skips its accessory; lower-case UUIDs, Apple's and a maker's own, are fine",
          mparsed && ids(mt) == "58.10", ids(mt));
    json dup = fixture();
    json garage_light = dup["accessories"][5];   // aid 6: the garage door with its light
    garage_light["services"].erase(1);           // ... listed again with only the light
    dup["accessories"].push_back(garage_light);
    std::vector<home::Thing> dt;
    bool dparsed = things_from_database(dup, dt);
    check("database: an aid listed twice is skipped in every listing (the garage's light does not come through)",
          dparsed && !find(dt, "6.20") && find(dt, "2.10"), ids(dt));
    json dup2 = fixture();
    dup2["accessories"].push_back(dup2["accessories"][1]);   // the lamp twice
    dparsed = things_from_database(dup2, dt);
    check("database: the same accessory listed twice gives no thing for it (not two things with one id)",
          dparsed && !find(dt, "2.10") && find(dt, "3.10"), ids(dt));

    // A lone UTF-16 surrogate escape in one name (HAP-NodeJS writes one for a JavaScript string cut
    // through an emoji): the reply still parses, the name is cleaned, the rest is untouched.
    eq("surrogates: a lone high one becomes U+FFFD", repair_surrogates("[\"Porch \\ud83d Switch\"]"), "[\"Porch \\uFFFD Switch\"]");
    eq("surrogates: a lone low one, and one at the end of a string", repair_surrogates("[\"a\\uDC00b\",\"c\\uD83D\"]"),
       "[\"a\\uFFFDb\",\"c\\uFFFD\"]");
    eq("surrogates: a proper pair, an escaped backslash before u, and text outside strings are kept",
       repair_surrogates("{\"\\ud83d\\udca1\":\"\\\\ud83d\"} \\ud83d"), "{\"\\ud83d\\udca1\":\"\\\\ud83d\"} \\ud83d");
    eq("surrogates: a high one followed by another high one, then a pair",
       repair_surrogates("[\"\\ud83d\\ud83d\\ude00\"]"), "[\"\\uFFFD\\ud83d\\ude00\"]");
    json surrogate_db = fixture();
    surrogate_db["accessories"][2]["services"][0]["characteristics"][1]["value"] = "PLACEHOLDER";   // the kettle's name
    std::string text = surrogate_db.dump();
    text.replace(text.find("PLACEHOLDER"), 11, "Kettle \\ud83d Plug \\udca1");
    json parsed_db;
    std::vector<home::Thing> st;
    const bool sparsed = parse_database(text, parsed_db) && things_from_database(parsed_db, st);
    check("database: one name with lone surrogate escapes does not make the whole bridge unreadable",
          sparsed && st.size() == ts.size() && find(st, "3.10") && find(st, "3.10")->name == "Kettle Plug",
          sparsed && find(st, "3.10") ? find(st, "3.10")->name : "not parsed");

    check("database: not an object, or no accessories array, is refused",
          !things_from_database(json::array(), wt) && !things_from_database(json{{"accessories", 1}}, wt));
    json many = {{"accessories", json::array()}};
    for (int i = 0; i < 600; i++)
        many["accessories"].push_back(acc(100 + i, "L", json::array({json{{"iid", 10}, {"type", "49"}, {"characteristics", json::array({C(11, "25", "bool", PRW, false)})}}})));
    check("database: at most 400 things from a huge bridge", things_from_database(many, wt) && wt.size() == 400);
}

// ---------------------------------------------------------------------------------------------
static std::string writes(const home::Thing *t, const home::Change &c, std::string &err) {
    std::vector<Write> w;
    err.clear();
    if (!t || !writes_for(*t, c, w, err)) return "!" + err;
    std::string s;
    for (const Write &x : w) s += (s.empty() ? "" : " ") + std::to_string(x.aid) + "." + std::to_string(x.iid) + "=" + x.value.dump();
    return s;
}

static void test_writes() {
    std::vector<home::Thing> ts;
    things_from_database(fixture(), ts);
    const home::Thing *lamp = find(ts, "2.10"), *heat = find(ts, "4.10"), *heater = find(ts, "10.10"), *fan = find(ts, "11.10"),
                      *kettle = find(ts, "3.10"), *door = find(ts, "13.20");
    std::string err;
    home::Change c;
    c.on = true;
    eq("light on", writes(lamp, c, err), "2.11=true");
    c = home::Change();
    c.brightness = 40;
    eq("brightness 40 also switches the light on, power first", writes(lamp, c, err), "2.11=true 2.12=40");
    c.brightness = 0;
    eq("brightness 0 switches the light off", writes(lamp, c, err), "2.11=false");
    c.brightness = 150;
    eq("brightness above 100 is clamped", writes(lamp, c, err), "2.11=true 2.12=100");
    c = home::Change();
    c.on = false;
    c.brightness = 50;
    eq("an explicit off wins over the implied on", writes(lamp, c, err), "2.11=false 2.12=50");
    c = home::Change();
    c.kelvin = 2700;
    eq("2700 K is 370 mireds", writes(lamp, c, err), "2.11=true 2.15=370");
    c.kelvin = 10000;
    eq("10000 K is clamped to the bulb's 140 mireds", writes(lamp, c, err), "2.11=true 2.15=140");
    c = home::Change();
    c.hs = std::make_pair(120, 50);
    eq("hue and saturation as floats", writes(lamp, c, err), "2.11=true 2.13=120.0 2.14=50.0");
    c = home::Change();
    c.target_c = 20.3;
    eq("a target is rounded to the thermostat's 0.5 step", writes(heat, c, err), "4.14=20.5");
    c.target_c = 40;
    eq("a target above 30 C is clamped to 30", writes(heat, c, err), "4.14=30.0");
    c.target_c = 1;
    eq("a target below 5 C is clamped to 5", writes(heat, c, err), "4.14=5.0");
    c = home::Change();
    c.mode = "auto";
    eq("mode auto is 3", writes(heat, c, err), "4.12=3");
    c.mode = "off";
    eq("mode off is 0", writes(heat, c, err), "4.12=0");
    c.mode = "cool";
    eq("mode cool is not a heating mode", writes(heat, c, err), "!That is not a heating mode.");
    c = home::Change();
    c.on = false;
    eq("off on a thermostat sets mode off", writes(heat, c, err), "4.12=0");
    c.on = true;
    eq("on on a thermostat sets mode heat", writes(heat, c, err), "4.12=1");
    c = home::Change();
    c.mode = "heat";
    eq("heat on a heater-cooler: active, then heating", writes(heater, c, err), "10.11=1 10.13=1");
    c.mode = "auto";
    eq("auto on a heater-cooler that only heats is refused", writes(heater, c, err), "!Heater has no auto mode.");
    c = home::Change();
    c.target_c = 22;
    eq("a heater-cooler's target is its heating threshold", writes(heater, c, err), "10.15=22.0");
    c = home::Change();
    c.fan = 50;
    eq("fan 50% of a three-speed fan is speed 2, and switches it on", writes(fan, c, err), "11.11=1 11.12=2.0");
    c.fan = 0;
    eq("fan 0 switches the fan off", writes(fan, c, err), "11.11=0");
    c = home::Change();
    c.volume = 10;
    eq("volume on a light is refused", writes(lamp, c, err), "!Hall Lamp cannot do that.");
    c = home::Change();
    c.brightness = 50;
    eq("brightness on a plug is refused", writes(kettle, c, err), "!Kettle cannot do that.");
    c = home::Change();
    c.on = true;
    eq("a sensor cannot be changed", writes(door, c, err), "!Hall door is a sensor: it can only be read.");
    eq("an empty change is refused", writes(lamp, home::Change(), err), "!Nothing to change.");
    home::Thing stale = *lamp;
    stale.ref = json::object();
    c = home::Change();
    c.on = true;
    eq("a thing without its addressing asks for a refresh", writes(&stale, c, err), "!Hall Lamp needs to be refreshed in settings.");

    eq("status -70402 in words", status_text(-70402, "Hall Lamp"), "Hall Lamp is switched off, or out of range of its bridge.");
    eq("status 70409 without a name", status_text(70409, ""), "The accessory no longer exists on the accessory. Refresh the device in settings.");
    // Every status comes from an accessory that answered: none may read as "it did not answer",
    // or the app would rescan the network for 2.5 s (the words of control.cpp unreachable()).
    std::string trips;
    for (const long code : {-70401L, -70402L, -70403L, -70404L, -70405L, -70406L, -70407L, -70408L, -70409L, -70410L, -70411L,
                            -70412L, -1L})
        for (const char *w : {"connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable"}) {
            std::string s = status_text(code, "Hall Lamp");
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
            if (s.find(w) != std::string::npos) trips += std::to_string(code) + ":" + w + " ";
        }
    check("no status in words reads as \"did not answer\" to the app", trips.empty(), trips);
    // The same for every error sentence about a problem on this side or from an accessory that
    // answered (a damaged key store, a turned-down session, a garbled reply...).
    trips.clear();
    const std::vector<std::string> sentences = answered_error_texts();
    for (std::string s : sentences) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        for (const char *w : {"connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable"})
            if (s.find(w) != std::string::npos) trips += "[" + s.substr(0, 40) + "...]:" + w + " ";
    }
    check("no error from this side or from an accessory that answered reads as \"did not answer\" to the app (" +
              std::to_string(sentences.size()) + " sentences)",
          sentences.size() >= 30 && trips.empty(), trips);

    home::Thing t = *lamp;
    apply_values(t, {{{2, 11}, json(false)}, {{2, 12}, json(75)}, {{2, 13}, json(10)}, {{2, 14}, json(20)}, {{2, 15}, json(370)}}, {});
    check("values read back make the state", t.state.value("on", true) == false && t.state.value("brightness", 0) == 75 &&
                                                 !t.state.contains("reachable"));
    apply_values(t, {{{2, 11}, json(true)}, {{2, 13}, json(10)}, {{2, 14}, json(20)}, {{2, 15}, json(370)}}, {{2, 12}});
    check("a value that could not be read is left out, not kept from before; the thing is marked",
          t.state.value("reachable", true) == false && !t.state.contains("brightness") && t.state.value("on", false),
          t.state.dump());
    apply_values(t, {}, {{2, 11}, {2, 12}, {2, 13}, {2, 14}, {2, 15}});
    check("nothing readable: no value is kept at all", t.state == json({{"reachable", false}}), t.state.dump());
    apply_values(t, {{{2, 11}, json(1)}, {{2, 12}, json(75)}}, {});
    check("a later good read clears the mark; 1 counts as true", !t.state.contains("reachable") && t.state.value("on", false));
    apply_values(t, {{{2, 11}, json(1)}, {{2, 12}, json("bright")}, {{2, 15}, json(0)}}, {});
    check("values of the wrong type or impossible ones are left out",
          !t.state.contains("brightness") && !t.state.contains("kelvin") && t.state.value("on", false), t.state.dump());
}

// ---------------------------------------------------------------------------------------------
class MemSecrets : public home::Secrets {
public:
    json get(const std::string &family) const override { return store.contains(family) ? store[family] : json::object(); }
    void set(const std::string &family, const json &value) override {
        store[family] = value;
        sets++;
    }
    json store = json::object();
    int sets = 0;
};

static void test_driver_offline() {
    std::unique_ptr<home::Driver> d = make_homekit_driver();
    check("kind is homekit", std::string(d->kind()) == "homekit");
    check("claims homekit devices, not other kinds", d->claims("homekit", {}) && d->claims("homekit", {{"ci", "2"}}) &&
                                                          !d->claims("hue_bridge", {}) && !d->claims("matter", {}));
    check("does not claim locks, garage doors, alarms, cameras, doorbells or taps",
          !d->claims("homekit", {{"ci", "6"}}) && !d->claims("homekit", {{"ci", "4"}}) && !d->claims("homekit", {{"ci", "11"}}) &&
              !d->claims("homekit", {{"ci", "17"}}) && !d->claims("homekit", {{"ci", "18"}}) && !d->claims("homekit", {{"ci", "29"}}));
    std::string taken;
    for (const char *ci : {"15", "16", "24", "25", "26", "31", "32", "33", "34", "35", "36"})
        if (d->claims("homekit", {{"ci", ci}})) taken += std::string(ci) + " ";
    check("does not claim TVs, set-top boxes, streaming sticks, speakers, receivers, remotes, routers, buttons or "
          "range extenders (nothing on the allow-list; TVs and speakers have drivers of their own)",
          taken.empty(), taken);
    taken.clear();
    for (const char *ci : {"19", "22", "23"})
        if (d->claims("homekit", {{"ci", ci}})) taken += std::string(ci) + " ";
    check("does not claim air purifiers, humidifiers or dehumidifiers (their services, BB and BD, are not on the "
          "allow-list: pairing one would use up its HomeKit pairing for nothing)", taken.empty(), taken);
    check("still claims fans, heaters and air conditioners (3, 20, 21)",
          d->claims("homekit", {{"ci", "3"}}) && d->claims("homekit", {{"ci", "20"}}) && d->claims("homekit", {{"ci", "21"}}));
    check("claims bridges, thermostats, lights, plugs, heaters and sensors; a garbled category does not matter",
          d->claims("homekit", {{"ci", "2"}}) && d->claims("homekit", {{"ci", "9"}}) && d->claims("homekit", {{"ci", "5"}}) &&
              d->claims("homekit", {{"ci", "7"}}) && d->claims("homekit", {{"ci", "20"}}) && d->claims("homekit", {{"ci", "10"}}) &&
              d->claims("homekit", {{"ci", "x"}}));

    MemSecrets secrets;
    d->attach(&secrets);
    home::Device dev;
    dev.id = "homekit:aa:bb:cc:dd:ee:01";
    dev.kind = "homekit";
    dev.ip = "127.0.0.1";
    dev.port = 0;   // nothing can be contacted
    dev.info = {{"sf", "1"}, {"ci", "2"}};
    home::PairStep s = d->next_step(dev);
    check("an unpaired accessory asks for its setup code",
          s.kind == home::Pairing::EnterCode && s.field == "Setup code" && s.prompt.find("8 digits") != std::string::npos);
    dev.info["sf"] = "0";
    s = d->next_step(dev);
    check("an accessory paired elsewhere says so before anything is tried",
          s.kind == home::Pairing::EnterCode && s.prompt.find("already paired") != std::string::npos);
    std::string err;
    check("pairing an accessory paired elsewhere is refused without contacting it",
          !d->pair(dev, "123-45-678", err) && err.find("already paired") != std::string::npos && secrets.sets == 0);
    dev.info["sf"] = "1";
    check("a malformed code is refused without contacting it", !d->pair(dev, "12-34", err) && err.find("not a setup code") != std::string::npos);
    check("pairing with no address is refused", !d->pair(dev, "123-45-678", err) && err.find("No address") != std::string::npos);
    check("this controller's key was made once and stored", secrets.sets == 1 && secrets.store["homekit"].contains("controller_id") &&
                                                                secrets.store["homekit"]["ltsk"].get<std::string>().size() == 64);
    const json first = secrets.store["homekit"];
    d->pair(dev, "123-45-678", err);
    check("the key is not made again", secrets.sets == 1 && secrets.store["homekit"] == first);
    const std::string id = first["controller_id"].get<std::string>();
    check("the controller id is an upper-case version 4 UUID",
          id.size() == 36 && id[8] == '-' && id[13] == '-' && id[14] == '4' && id[18] == '-' && id[23] == '-' &&
              std::none_of(id.begin(), id.end(), [](char c) { return c >= 'a' && c <= 'z'; }));

    std::vector<home::Thing> out;
    check("enumerating before pairing says to pair", !d->enumerate(dev, out, err) && err.find("not paired") != std::string::npos);

    dev.auth = {{"accessory_id", "AA:BB:CC:DD:EE:01"}, {"accessory_ltpk", std::string(64, '1')}, {"controller_id", id}};
    s = d->next_step(dev);
    check("a paired accessory needs no step", s.kind == home::Pairing::None);
    const json kept = dev.auth;
    check("pairing again when already paired with this app first asks the accessory (here: no address), and keeps "
          "the pairing",
          !d->pair(dev, "123-45-678", err) && err.find("No address") != std::string::npos && dev.auth == kept, err);
    check("enumerating with no address says so", !d->enumerate(dev, out, err) && err.find("No address") != std::string::npos);
    dev.auth["controller_id"] = "SOMEONE-ELSE";
    s = d->next_step(dev);
    check("a pairing made with another key of this app asks for a reset",
          s.kind == home::Pairing::EnterCode && s.prompt.find("no longer has") != std::string::npos);
    check("... and nothing is sent with it", !d->enumerate(dev, out, err) && err.find("no longer has") != std::string::npos);
    d->unpair(dev);
    check("unpair forgets a pairing this app cannot speak for", !dev.auth.contains("accessory_id") && dev.auth.is_object());

    MemSecrets damaged;
    damaged.store["homekit"] = {{"controller_id", "X"}, {"ltsk", std::string(64, 'a')}, {"ltpk", std::string(64, 'b')}};
    std::unique_ptr<home::Driver> d2 = make_homekit_driver();
    d2->attach(&damaged);
    dev.auth = {{"accessory_id", "AA:BB:CC:DD:EE:01"}, {"accessory_ltpk", std::string(64, '1')}, {"controller_id", "X"}};
    check("a damaged stored key is reported and never replaced",
          !d2->enumerate(dev, out, err) && err.find("damaged") != std::string::npos && !d2->pair(dev, "123-45-678", err) &&
              err.find("damaged") != std::string::npos && damaged.sets == 0);

    std::unique_ptr<home::Driver> d3 = make_homekit_driver();
    home::Device dev3 = dev;
    dev3.auth = json::object();
    check("without a secret store pairing is refused before any contact",
          !d3->pair(dev3, "123-45-678", err) && err.find("could not save") != std::string::npos);
    home::Device nothing;
    std::vector<home::Thing> none;
    check("reading a device with no things succeeds and sends nothing", d3->read(nothing, none, err));
    home::Change c;
    c.on = true;
    home::Thing t;
    check("apply_many with no things is refused with a sentence", !d3->apply_many(nothing, {}, c, 100, err) && !err.empty());
}

// ---------------------------------------------------------------------------------------------
// What GET /accessories can cost: enumerate's own steps (parse_database: UTF-8 repair, nesting and
// size checks, parsing; then things_from_database) on bodies just under the 1 MiB cap, measured
// with the counting allocator above.
static size_t peak_bytes(const std::function<void()> &f) {
    const size_t base = g_heap_now.load();
    g_heap_peak = base;
    f();
    return g_heap_peak.load() - base;
}

static std::string listing(const std::string &open, const std::function<std::string(size_t)> &item, const std::string &close,
                           size_t max_bytes) {
    std::string s = open;
    for (size_t i = 0;; i++) {
        const std::string it = (i ? "," : "") + item(i);
        if (s.size() + it.size() + close.size() > max_bytes) break;
        s += it;
    }
    return s + close;
}

static std::string realistic_bridge(int accessories) {
    json accs = json::array();
    for (int i = 0; i < accessories; i++)
        accs.push_back(acc(100 + i, "Light " + std::to_string(i), json::array({json{{"iid", 10}, {"type", "43"}, {"characteristics", json::array({
            C(11, "25", "bool", PRW, true), C(12, "8", "int", PRW, 40, {{"minValue", 0}, {"maxValue", 100}, {"minStep", 1}, {"unit", "percentage"}}),
            C(13, "13", "float", PRW, 30, {{"minValue", 0}, {"maxValue", 360}, {"minStep", 1}, {"unit", "arcdegrees"}}),
            C(14, "2F", "float", PRW, 50, {{"minValue", 0}, {"maxValue", 100}, {"minStep", 1}, {"unit", "percentage"}}),
            C(15, "CE", "uint32", PRW, 370, {{"minValue", 140}, {"maxValue", 500}, {"minStep", 1}}),
            C(16, "23", "string", {"pr"}, "Light")})}}})));
    return json({{"accessories", accs}}).dump();
}

static void test_memory() {
    const size_t kCap = 1024 * 1024 - 64;   // just under parse_database's 1 MiB
    const size_t kBound = 16u * 1024 * 1024;
    struct Bomb {
        const char *name;
        std::string body;
    };
    const std::vector<Bomb> bombs = {
        {"empty objects", listing("[", [](size_t) { return std::string("{}"); }, "]", kCap)},
        {"empty strings", listing("[", [](size_t) { return std::string("\"\""); }, "]", kCap)},
        {"empty arrays", listing("[", [](size_t) { return std::string("[]"); }, "]", kCap)},
        {"numbers", listing("[", [](size_t) { return std::string("0"); }, "]", kCap)},
        {"members", listing("{", [](size_t i) { return "\"k" + std::to_string(i) + "\":0"; }, "}", kCap)},
        {"long-key members", listing("{", [](size_t i) { return "\"key-" + std::to_string(1000000000000 + i) + "\":[]"; }, "}", kCap)},
        {"members under the value cap",
         listing("{", [](size_t i) { return "\"k" + std::to_string(i) + "\":0"; }, "}", 7 * 74000)},   // about 148,000 values
        {"strings under the value cap", listing("[", [](size_t) { return std::string("\"\""); }, "]", 3 * 149000)},
        {"objects under the value cap", listing("[", [](size_t) { return std::string("{}"); }, "]", 3 * 74000)},
    };
    for (const Bomb &b : bombs) {
        bool parsed = false;
        size_t things = 0;
        const size_t peak = peak_bytes([&] {
            json db;
            parsed = parse_database(b.body, db);
            std::vector<home::Thing> ts;
            if (parsed) things_from_database(db, ts);
            things = ts.size();
        });
        char line[200];
        snprintf(line, sizeof line, "a hostile /accessories of %zu bytes (%s) costs at most 16 MB: %s, peak %.1f MB", b.body.size(),
                 b.name, parsed ? "parsed" : "refused", peak / 1048576.0);
        check(line, peak < kBound && things == 0);
    }
    const std::string real = realistic_bridge(400);
    bool parsed = false;
    size_t things = 0;
    const size_t peak = peak_bytes([&] {
        json db;
        parsed = parse_database(real, db);
        std::vector<home::Thing> ts;
        if (parsed) things_from_database(db, ts);
        things = ts.size();
    });
    char line[200];
    snprintf(line, sizeof line, "a bridge of 400 lights (%zu bytes) is read whole: %zu things, peak %.1f MB", real.size(), things,
             peak / 1048576.0);
    check(line, parsed && things == 400 && peak < kBound);
    json db;
    check("a database over 1 MiB is refused", !parse_database(realistic_bridge(800), db));
    check("JSON values are counted outside strings only",
          json_values_ok("[\"a,b,c,d,e:[{\"]", 2) && !json_values_ok("[1,2,3]", 2) && json_values_ok("[1,2]", 2));
}

// ---------------------------------------------------------------------------------------------
// What pair-verify costs this controller, with the accessory's half done in the same process.
static void test_pair_verify_cost() {
    Bytes acc_seed, acc_pub, me_seed, me_pub;
    random_bytes(32, acc_seed);
    random_bytes(32, me_seed);
    ed25519_public(acc_seed, acc_pub);
    ed25519_public(me_seed, me_pub);
    const int runs = 50;
    std::vector<double> ms;
    bool all_ok = true;
    for (int i = 0; i < runs; i++) {
        // accessory side, not timed
        Bytes apriv, apub;
        random_bytes(32, apriv);
        x25519_public(apriv, apub);
        // controller side
        const auto t0 = std::chrono::steady_clock::now();
        Bytes cpriv, cpub, shared, sig, sealed, plain;
        random_bytes(32, cpriv);
        x25519_public(cpriv, cpub);
        x25519_shared(cpriv, apub, shared);
        const auto t1 = std::chrono::steady_clock::now();
        Bytes asig;   // accessory signs, not timed
        ed25519_sign(acc_seed, apub + "AA:BB" + cpub, asig);
        Bytes asealed;
        const Bytes key = hkdf_sha512(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", 32);
        chacha20_poly1305_seal(key, label_nonce("PV-Msg02"), "", tlv_encode({{kTlvIdentifier, "AA:BB"}, {kTlvSignature, asig}}), asealed);
        const auto t2 = std::chrono::steady_clock::now();
        std::vector<Tlv> sub;
        const Bytes k2 = hkdf_sha512(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", 32);
        bool ok = chacha20_poly1305_open(k2, label_nonce("PV-Msg02"), "", asealed, plain) && tlv_decode(plain, sub) &&
                  ed25519_verify(acc_pub, apub + *tlv_find(sub, kTlvIdentifier) + cpub, *tlv_find(sub, kTlvSignature)) &&
                  ed25519_sign(me_seed, cpub + "CTRL" + apub, sig) &&
                  chacha20_poly1305_seal(k2, label_nonce("PV-Msg03"), "", tlv_encode({{kTlvIdentifier, "CTRL"}, {kTlvSignature, sig}}), sealed);
        hkdf_sha512(shared, "Control-Salt", "Control-Write-Encryption-Key", 32);
        hkdf_sha512(shared, "Control-Salt", "Control-Read-Encryption-Key", 32);
        const auto t3 = std::chrono::steady_clock::now();
        all_ok = all_ok && ok;
        ms.push_back(std::chrono::duration<double, std::milli>((t1 - t0) + (t3 - t2)).count());
    }
    std::sort(ms.begin(), ms.end());
    char line[160];
    snprintf(line, sizeof line, "pair-verify, this controller's share of the work: median %.3f ms, worst %.3f ms over %d runs",
             ms[ms.size() / 2], ms.back(), runs);
    check(line, all_ok && ms[ms.size() / 2] < 50);
}

int main() {
    test_primitives();
    test_tlv();
    test_srp();
    test_setup_codes();
    test_frames();
    test_http();
    test_text();
    test_database();
    test_writes();
    test_driver_offline();
    test_memory();
    test_pair_verify_cost();
    printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
