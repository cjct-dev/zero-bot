// Unit tests for the helpers the device drivers share (src/smarthome/drivers/support.h). No network.
// Each check prints one line; the exit status is non-zero when any fails.
//
//   support_test MOCKS    MOCKS: tests/mocks (for hue/certs/a.pem, a test certificate)
//
// Where the vectors come from: SHA-256 and SHA-512 of "abc" (FIPS 180-2, appendix B and C), HMAC
// test cases 1 and 2 of RFC 4231; HMAC-SHA-256 with an empty key and message, and the base64 of
// SHA-256("abc"), computed with Python's hashlib, hmac and base64, which share no code with these.
#include "smarthome/drivers/support.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using support::json;

static int g_failed = 0, g_run = 0;

static void check(const std::string &name, bool ok, const std::string &detail = "") {
    g_run++;
    if (!ok) g_failed++;
    printf("%s  %s%s\n", ok ? "ok  " : "FAIL", name.c_str(), ok || detail.empty() ? "" : ("  -> " + detail).c_str());
}

static void same(const std::string &name, const std::string &got, const std::string &want) {
    check(name, got == want, "got \"" + got + "\", want \"" + want + "\"");
}

static std::string nested(int depth) { return std::string(depth, '[') + std::string(depth, ']'); }

static void test_json() {
    check("json_too_deep: 32 levels under a limit of 32", !support::json_too_deep(nested(32), 32));
    check("json_too_deep: 33 levels over it", support::json_too_deep(nested(33), 32));
    check("json_too_deep: objects count as arrays do", support::json_too_deep(R"({"a":{"b":{"c":1}}})", 2));
    check("json_too_deep: brackets inside a string are not counted", !support::json_too_deep(R"(["[[[[{{{{"])", 1));
    check("json_too_deep: an escaped quote does not end the string", !support::json_too_deep(R"(["\"[[[["])", 1));
    check("json_too_deep: an escaped backslash does", support::json_too_deep(R"(["\\",[[1]]])", 2));
    // A closing bracket with nothing open counts below zero (HomeKit's own check does not; it stays its own).
    check("json_too_deep: stray closing brackets count below zero", !support::json_too_deep("]]][[[[", 2));
    check("json_too_deep: empty text", !support::json_too_deep("", 0));

    json out = json::array({1});
    check("parse_reply: longer than the limit is refused", !support::parse_reply("[1,2,3]", 6, 32, out) && out == json::array({1}));
    check("parse_reply: deeper than the limit is refused", !support::parse_reply(nested(4), 100, 3, out) && out == json::array({1}));
    check("parse_reply: text that is not JSON", !support::parse_reply("{\"a\":", 100, 32, out));
    check("parse_reply: a reply within both limits", support::parse_reply(R"({"a":[1,2]})", 11, 2, out) && out["a"][1] == 2);

    // The same scanner checks, then parses, everything else that came from another machine (core/util.h, ZB-093).
    check("parse_untrusted_json: 32 levels, the shared limit, are read", util::parse_untrusted_json(nested(32)).is_array());
    check("parse_untrusted_json: 33 are not, though they are JSON", util::parse_untrusted_json(nested(33)).is_discarded() &&
                                                                       !json::parse(nested(33), nullptr, false).is_discarded());
    check("parse_untrusted_json: a megabyte of brackets is refused unread",
          util::parse_untrusted_json(std::string(1 << 20, '[') + std::string(1 << 20, ']')).is_discarded());
    check("parse_untrusted_json: another limit", util::parse_untrusted_json(nested(4), 3).is_discarded() &&
                                                     util::parse_untrusted_json(nested(3), 3).is_array());
    check("parse_untrusted_json: text that is not JSON", util::parse_untrusted_json("{\"a\":").is_discarded());
    check("parse_untrusted_json: brackets inside a string are not counted",
          util::parse_untrusted_json("{\"a\":\"" + nested(100) + "\"}").value("a", "") == nested(100));
    check("support::json_too_deep is that scanner", support::json_too_deep(nested(33), util::kMaxJsonDepth) &&
                                                        !support::json_too_deep(nested(32), util::kMaxJsonDepth));
}

static void test_text() {
    using support::BadUtf8;
    const std::string good = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";   // é, €, an emoji
    same("sanitize_utf8: well-formed text is kept", support::sanitize_utf8(good, BadUtf8::QuestionMark), good);
    same("sanitize_utf8: a stray byte becomes '?'", support::sanitize_utf8("a\xFF" "b", BadUtf8::QuestionMark), "a?b");
    same("sanitize_utf8: or U+FFFD", support::sanitize_utf8("a\xFF" "b", BadUtf8::ReplacementCharacter), "a\xEF\xBF\xBD" "b");
    same("sanitize_utf8: an overlong form, one per byte", support::sanitize_utf8("\xC0\xAF", BadUtf8::QuestionMark), "??");
    same("sanitize_utf8: a surrogate", support::sanitize_utf8("\xED\xA0\x80", BadUtf8::QuestionMark), "???");
    same("sanitize_utf8: above U+10FFFF", support::sanitize_utf8("\xF4\x90\x80\x80", BadUtf8::QuestionMark), "????");
    same("sanitize_utf8: a sequence cut short, then ASCII", support::sanitize_utf8("\xE2\x82" "a", BadUtf8::QuestionMark), "??a");

    const auto *p = reinterpret_cast<const unsigned char *>(good.data());
    uint32_t cp = 0;
    check("utf8_at: ASCII", support::utf8_at(p, good.size(), 0, cp) == 1 && cp == 'c');
    check("utf8_at: two bytes", support::utf8_at(p, good.size(), 3, cp) == 2 && cp == 0xE9);
    check("utf8_at: three bytes", support::utf8_at(p, good.size(), 6, cp) == 3 && cp == 0x20AC);
    check("utf8_at: four bytes", support::utf8_at(p, good.size(), 10, cp) == 4 && cp == 0x1F600);
    check("utf8_at: inside a sequence", support::utf8_at(p, good.size(), 4, cp) == 0);
    check("utf8_at: a sequence longer than what is left", support::utf8_at(p, 12, 10, cp) == 0);

    bool all = true;
    for (const uint32_t c : {0x09u, 0x0Au, 0x0Du, 0x20u, 0x85u, 0xA0u, 0x1680u, 0x2000u, 0x200Au, 0x2028u, 0x2029u, 0x202Fu, 0x205Fu, 0x3000u})
        all = all && support::unicode_space(c);
    check("unicode_space: every White_Space code point checked", all);
    bool none = false;
    for (const uint32_t c : {0x08u, 0x0Eu, 0x41u, 0x180Eu, 0x200Bu, 0x2060u, 0xFEFFu})
        none = none || support::unicode_space(c);
    check("unicode_space: not the zero-width and format characters", !none);

    same("utf8_prefix: short text is kept", support::utf8_prefix("abc", 5), "abc");
    same("utf8_prefix: cut between characters", support::utf8_prefix("a\xC3\xA9" "b", 3), "a\xC3\xA9");
    same("utf8_prefix: not inside one", support::utf8_prefix("a\xC3\xA9" "b", 2), "a");
    same("utf8_prefix: a character longer than the limit", support::utf8_prefix("\xF0\x9F\x98\x80", 3), "");
}

static void test_mac() {
    same("mac_digits: colons", support::mac_digits("AA:bb:CC:00:11:22"), "aabbcc001122");
    same("mac_digits: dashes", support::mac_digits("aa-bb-cc-00-11-22"), "aabbcc001122");
    same("mac_digits: nothing between", support::mac_digits("AABBCC001122"), "aabbcc001122");
    same("mac_digits: separators anywhere are skipped", support::mac_digits("aa:bb-cc0011::22-"), "aabbcc001122");
    same("mac_digits: eleven digits", support::mac_digits("aabbcc00112"), "");
    same("mac_digits: thirteen", support::mac_digits("aabbcc0011223"), "");
    same("mac_digits: not a hex digit", support::mac_digits("aa:bb:cc:00:11:2g"), "");
    same("mac_digits: a space", support::mac_digits(" aa:bb:cc:00:11:22"), "");
    same("mac_digits: empty", support::mac_digits(""), "");
}

static void test_bytes() {
    check("hex_digit", support::hex_digit('0') == 0 && support::hex_digit('9') == 9 && support::hex_digit('a') == 10 &&
                           support::hex_digit('F') == 15 && support::hex_digit('g') == -1 && support::hex_digit(' ') == -1);
    same("hex: lower case", support::hex(std::string("\x00\xFF\x10\xAB", 4)), "00ff10ab");
    std::string out = "kept";
    check("unhex: either case", support::unhex("00Ff10aB", out) && out == std::string("\x00\xFF\x10\xAB", 4));
    out = "kept";
    check("unhex: an odd length leaves out untouched", !support::unhex("abc", out) && out == "kept");
    check("unhex: a character that is not hex leaves out untouched", !support::unhex("a0zz", out) && out == "kept");
    check("unhex: empty", support::unhex("", out) && out.empty());

    same("digest: SHA-256 of \"abc\"", support::hex(support::digest(EVP_sha256(), "abc")),
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    same("digest: SHA-512 of \"abc\"", support::hex(support::digest(EVP_sha512(), std::string("abc"))),
         "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    same("hmac: RFC 4231 case 1, SHA-256", support::hex(support::hmac("SHA256", std::string(20, '\x0b'), "Hi There")),
         "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    same("hmac: RFC 4231 case 1, SHA-512", support::hex(support::hmac("SHA512", std::string(20, '\x0b'), "Hi There")),
         "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
    same("hmac: RFC 4231 case 2", support::hex(support::hmac("SHA256", "Jefe", "what do ya want for nothing?")),
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    same("hmac: an empty key and message", support::hex(support::hmac("SHA256", "", "")),
         "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    same("hmac: a digest libcrypto does not know", support::hmac("NO-SUCH-DIGEST", "k", "m"), "");
}

static void test_certificates(const std::string &mocks) {
    check("certificate_from_pem: empty", !support::certificate_from_pem(""));
    check("certificate_from_pem: not PEM", !support::certificate_from_pem("-----BEGIN CERTIFICATE-----\nnot base64\n"));
    std::ifstream f(mocks + "/hue/certs/a.pem");
    std::stringstream pem;
    pem << f.rdbuf();
    check("certificate_from_pem: a test certificate", f.good() && support::certificate_from_pem(pem.str()) != nullptr,
          "could not read " + mocks + "/hue/certs/a.pem");
    std::string big = pem.str();
    big.append(64 * 1024 + 1 - std::min<size_t>(big.size(), 64 * 1024 + 1), '\n');
    check("certificate_from_pem: over 64 KiB is refused, whatever it holds", !support::certificate_from_pem(big));
    const std::string abc = "abc";
    same("sha256_pin: \"sha256//\" and the base64 SHA-256",
         support::sha256_pin(reinterpret_cast<const unsigned char *>(abc.data()), abc.size()),
         "sha256//ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=");
}

static void test_socket() {
    int fd = -1;
    {
        support::Socket s(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        fd = s.fd;
        check("Socket: open inside its scope", fd >= 0 && fcntl(fd, F_GETFD) != -1);
    }
    errno = 0;
    check("Socket: closed when it goes out of scope", fd >= 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    { support::Socket none; }
    check("Socket: an empty one closes nothing", true);
}

int main(int argc, char **argv) {
    const std::string mocks = argc > 1 ? argv[1] : "tests/mocks";
    test_json();
    test_text();
    test_mac();
    test_bytes();
    test_certificates(mocks);
    test_socket();
    printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
