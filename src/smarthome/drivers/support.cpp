// Helpers the device drivers share (ZB-038): see support.h. Each body is one driver's copy as it
// was, the others having been checked to behave the same on every input.
#include "smarthome/drivers/support.h"

#include <unistd.h>

#include <cctype>

#include <openssl/bio.h>
#include <openssl/pem.h>

#include "core/util.h"

namespace support {

// ---------------------------------------------------------------- JSON from devices
bool json_too_deep(const std::string &text, int max) {
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
            --depth;
        }
    }
    return false;
}

bool parse_reply(const std::string &body, size_t max_bytes, int max_depth, json &out) {
    if (body.size() > max_bytes || json_too_deep(body, max_depth)) return false;
    out = json::parse(body, nullptr, false);
    return !out.is_discarded();
}

// ---------------------------------------------------------------- text from devices
size_t utf8_at(const unsigned char *p, size_t n, size_t i, uint32_t &cp) {
    const unsigned char c = p[i];
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    size_t len;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F, min = 0x80;
    else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F, min = 0x800;
    else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07, min = 0x10000;
    else return 0;
    if (n - i < len) return 0;
    for (size_t k = 1; k < len; k++) {
        if ((p[i + k] & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (p[i + k] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

std::string sanitize_utf8(const std::string &raw, BadUtf8 bad) {
    const char *const replacement = bad == BadUtf8::QuestionMark ? "?" : "\xEF\xBF\xBD";
    std::string out;
    out.reserve(raw.size());
    const unsigned char *p = reinterpret_cast<const unsigned char *>(raw.data());
    for (size_t i = 0; i < raw.size();) {
        uint32_t cp = 0;
        const size_t len = utf8_at(p, raw.size(), i, cp);
        if (!len) {
            out += replacement;
            i++;
            continue;
        }
        out.append(raw, i, len);
        i += len;
    }
    return out;
}

bool unicode_space(uint32_t cp) {
    return cp == ' ' || (cp >= 0x09 && cp <= 0x0D) || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

std::string utf8_prefix(const std::string &text, size_t max) {
    if (text.size() <= max) return text;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut);
}

// ---------------------------------------------------------------- MAC addresses
std::string mac_digits(const std::string &raw) {
    std::string hex;
    for (const unsigned char c : raw) {
        if (c == ':' || c == '-') continue;
        if (!isxdigit(c) || hex.size() >= 12) return "";
        hex += static_cast<char>(tolower(c));
    }
    return hex.size() == 12 ? hex : "";
}

// ---------------------------------------------------------------- bytes
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string hex(const std::string &bytes) {
    static const char *const d = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (const unsigned char c : bytes) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

bool unhex(const std::string &hex, std::string &out) {
    if (hex.size() % 2) return false;
    std::string r;
    r.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_digit(hex[i]), lo = hex_digit(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        r += static_cast<char>(hi << 4 | lo);
    }
    out.swap(r);
    return true;
}

std::string digest(const EVP_MD *md, const void *data, size_t len) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (EVP_Digest(data, len, out, &n, md, nullptr) != 1) return "";
    return std::string(reinterpret_cast<const char *>(out), n);
}

std::string digest(const EVP_MD *md, const std::string &data) { return digest(md, data.data(), data.size()); }

std::string hmac(const char *digest_name, const std::string &key, const std::string &data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    size_t n = 0;
    if (!EVP_Q_mac(nullptr, "HMAC", nullptr, digest_name, nullptr, key.data(), key.size(),
                   reinterpret_cast<const unsigned char *>(data.data()), data.size(), out, sizeof out, &n))
        return "";
    return std::string(reinterpret_cast<const char *>(out), n);
}

// ---------------------------------------------------------------- certificates
Certificate certificate_from_pem(const std::string &pem) {
    if (pem.empty() || pem.size() > 64 * 1024) return nullptr;
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return nullptr;
    X509 *x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return Certificate(x);
}

std::string sha256_pin(const unsigned char *der, size_t len) {
    const std::string d = digest(EVP_sha256(), der, len);
    if (d.size() != 32) return "";
    return "sha256//" + util::base64(d);
}

// ---------------------------------------------------------------- sockets
Socket::~Socket() {
    if (fd >= 0) close(fd);
}

}  // namespace support
