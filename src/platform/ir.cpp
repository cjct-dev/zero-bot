#include "platform/ir.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/lirc.h>

#include "agent/experiment.h"
#include "core/util.h"

namespace ir {

namespace {

using Pulses = std::vector<uint32_t>;  // alternating mark, space, mark ... in microseconds (odd count)

enum class Proto { Nec, Samsung, Sony12, Kaseikyo, Rc6, Rc5 };

struct Code { const char *key; uint32_t cmd; };
struct Brand {
    const char *name;
    Proto proto;
    uint32_t address;
    std::vector<Code> codes;
};

const std::vector<Brand> &table() {
    static const std::vector<Brand> t = {
        {"samsung", Proto::Samsung, 0x07,
         {{"power", 0x02}, {"volume_up", 0x07}, {"volume_down", 0x0B}, {"mute", 0x0F}, {"channel_up", 0x12},
          {"channel_down", 0x10}, {"input", 0x01}, {"up", 0x60}, {"down", 0x61}, {"left", 0x65}, {"right", 0x62},
          {"ok", 0x68}, {"menu", 0x1A}, {"back", 0x58}, {"home", 0x79}, {"1", 0x04}, {"2", 0x05}, {"3", 0x06},
          {"4", 0x08}, {"5", 0x09}, {"6", 0x0A}, {"7", 0x0C}, {"8", 0x0D}, {"9", 0x0E}, {"0", 0x11}}},
        {"lg", Proto::Nec, 0x04,
         {{"power", 0x08}, {"volume_up", 0x02}, {"volume_down", 0x03}, {"mute", 0x09}, {"channel_up", 0x00},
          {"channel_down", 0x01}, {"input", 0x0B}, {"up", 0x40}, {"down", 0x41}, {"left", 0x07}, {"right", 0x06},
          {"ok", 0x44}, {"menu", 0x43}, {"back", 0x28}, {"home", 0x7C}, {"0", 0x10}, {"1", 0x11}, {"2", 0x12},
          {"3", 0x13}, {"4", 0x14}, {"5", 0x15}, {"6", 0x16}, {"7", 0x17}, {"8", 0x18}, {"9", 0x19}}},
        {"sony", Proto::Sony12, 0x01,
         {{"power", 21}, {"volume_up", 18}, {"volume_down", 19}, {"mute", 20}, {"channel_up", 16},
          {"channel_down", 17}, {"input", 37}, {"up", 116}, {"down", 117}, {"left", 52}, {"right", 51},
          {"ok", 101}, {"menu", 96}, {"home", 96}, {"1", 0}, {"2", 1}, {"3", 2}, {"4", 3}, {"5", 4}, {"6", 5},
          {"7", 6}, {"8", 7}, {"9", 8}, {"0", 9}}},
        {"panasonic", Proto::Kaseikyo, 0x0080,
         {{"power", 0x3D}, {"volume_up", 0x20}, {"volume_down", 0x21}, {"mute", 0x32}, {"channel_up", 0x34},
          {"channel_down", 0x35}, {"input", 0x05}, {"up", 0x4A}, {"down", 0x4B}, {"left", 0x4E}, {"right", 0x4F},
          {"ok", 0x49}, {"menu", 0x52}, {"1", 0x10}, {"2", 0x11}, {"3", 0x12}, {"4", 0x13}, {"5", 0x14},
          {"6", 0x15}, {"7", 0x16}, {"8", 0x17}, {"9", 0x18}, {"0", 0x19}}},
        {"philips", Proto::Rc6, 0x00,
         {{"power", 0x0C}, {"volume_up", 0x10}, {"volume_down", 0x11}, {"mute", 0x0D}, {"channel_up", 0x20},
          {"channel_down", 0x21}, {"input", 0x38}, {"up", 0x58}, {"down", 0x59}, {"left", 0x5A}, {"right", 0x5B},
          {"ok", 0x5C}, {"menu", 0x54}, {"back", 0x0A}, {"0", 0}, {"1", 1}, {"2", 2}, {"3", 3}, {"4", 4},
          {"5", 5}, {"6", 6}, {"7", 7}, {"8", 8}, {"9", 9}}},
        {"philips_rc5", Proto::Rc5, 0x00,
         {{"power", 12}, {"volume_up", 16}, {"volume_down", 17}, {"mute", 13}, {"channel_up", 32},
          {"channel_down", 33}, {"input", 56}, {"0", 0}, {"1", 1}, {"2", 2}, {"3", 3}, {"4", 4}, {"5", 5},
          {"6", 6}, {"7", 7}, {"8", 8}, {"9", 9}}},
        {"toshiba", Proto::Nec, 0x40,
         {{"power", 0x12}, {"volume_up", 0x1A}, {"volume_down", 0x1E}, {"mute", 0x10}, {"channel_up", 0x1B},
          {"channel_down", 0x1F}, {"input", 0x0F}}},
        {"vizio", Proto::Nec, 0x04,
         {{"power", 0x08}, {"volume_up", 0x02}, {"volume_down", 0x03}, {"mute", 0x09}, {"channel_up", 0x00},
          {"channel_down", 0x01}, {"input", 0x2F}}},
    };
    return t;
}

void add_bits_lsb(Pulses &p, uint32_t value, int bits, uint32_t mark, uint32_t zero, uint32_t one) {
    for (int i = 0; i < bits; ++i) {
        p.push_back(mark);
        p.push_back((value >> i) & 1 ? one : zero);
    }
}

Pulses encode_nec(uint32_t address, uint32_t cmd, bool samsung) {
    Pulses p;
    p.push_back(samsung ? 4500 : 9000);
    p.push_back(4500);
    const uint32_t a = address & 0xFF, c = cmd & 0xFF;
    // Samsung repeats the address byte; NEC sends its complement.
    const uint32_t frame = a | ((samsung ? a : (~a & 0xFF)) << 8) | (c << 16) | ((~c & 0xFF) << 24);
    add_bits_lsb(p, frame, 32, 560, 560, 1690);
    p.push_back(560);  // stop bit
    return p;
}

Pulses encode_sony12(uint32_t device, uint32_t cmd) {
    Pulses one;
    one.push_back(2400);
    const uint32_t frame = (cmd & 0x7F) | ((device & 0x1F) << 7);
    for (int i = 0; i < 12; ++i) {
        one.push_back(600);
        one.push_back((frame >> i) & 1 ? 1200 : 600);
    }
    // Sony sets expect the frame at least three times, 45 ms apart.
    Pulses p;
    for (int rep = 0; rep < 3; ++rep) {
        uint32_t used = 0;
        for (uint32_t v : one) used += v;
        p.insert(p.end(), one.begin(), one.end());
        if (rep < 2) p.push_back(used < 45000 ? 45000 - used : 10000);
    }
    return p;
}

Pulses encode_kaseikyo(uint32_t address, uint32_t cmd) {
    // Panasonic: vendor 0x2002, then device, sub-device, command and an XOR checksum.
    const uint8_t bytes[6] = {0x02, 0x20, static_cast<uint8_t>(address & 0xFF), static_cast<uint8_t>(address >> 8),
                              static_cast<uint8_t>(cmd),
                              static_cast<uint8_t>((address & 0xFF) ^ (address >> 8) ^ cmd)};
    Pulses p;
    p.push_back(3456);
    p.push_back(1728);
    for (uint8_t b : bytes) add_bits_lsb(p, b, 8, 432, 432, 1296);
    p.push_back(432);
    return p;
}

// Manchester helpers: append a level for `us` microseconds, merging with the previous run.
void push_level(Pulses &p, bool &last_mark, bool mark, uint32_t us) {
    if (p.empty()) {
        if (!mark) return;  // leading space carries no information
        p.push_back(us);
    } else if (last_mark == mark) {
        p.back() += us;
    } else {
        p.push_back(us);
    }
    last_mark = mark;
}

Pulses encode_rc5(uint32_t address, uint32_t cmd, bool toggle) {
    Pulses p;
    bool last = false;
    const uint32_t t = 889;
    const uint32_t frame = (1u << 13) | (((cmd & 0x40) ? 0u : 1u) << 12) | ((toggle ? 1u : 0u) << 11) |
                           ((address & 0x1F) << 6) | (cmd & 0x3F);
    for (int i = 13; i >= 0; --i) {
        const bool bit = (frame >> i) & 1;  // RC5: 1 = space then mark
        push_level(p, last, !bit, t);
        push_level(p, last, bit, t);
    }
    if (!last) p.pop_back();
    return p;
}

Pulses encode_rc6(uint32_t address, uint32_t cmd, bool toggle) {
    Pulses p;
    bool last = false;
    const uint32_t t = 444;
    push_level(p, last, true, 6 * t);   // leader
    push_level(p, last, false, 2 * t);
    auto bit = [&](bool one, uint32_t unit) {  // RC6: 1 = mark then space
        push_level(p, last, one, unit);
        push_level(p, last, !one, unit);
    };
    bit(true, t);                               // start bit
    bit(false, t); bit(false, t); bit(false, t);  // mode 0
    bit(toggle, 2 * t);                         // double-width toggle bit
    for (int i = 7; i >= 0; --i) bit((address >> i) & 1, t);
    for (int i = 7; i >= 0; --i) bit((cmd >> i) & 1, t);
    if (!last) p.pop_back();
    return p;
}

int carrier_for(Proto proto) {
    switch (proto) {
        case Proto::Sony12: return 40000;
        case Proto::Kaseikyo: return 37000;
        case Proto::Rc5: case Proto::Rc6: return 36000;
        default: return 38000;
    }
}

Pulses encode(const Brand &b, uint32_t cmd) {
    static bool toggle = false;
    toggle = !toggle;
    switch (b.proto) {
        case Proto::Nec: return encode_nec(b.address, cmd, false);
        case Proto::Samsung: return encode_nec(b.address, cmd, true);
        case Proto::Sony12: return encode_sony12(b.address, cmd);
        case Proto::Kaseikyo: return encode_kaseikyo(b.address, cmd);
        case Proto::Rc5: return encode_rc5(b.address, cmd, toggle);
        case Proto::Rc6: return encode_rc6(b.address, cmd, toggle);
    }
    return {};
}

int open_lirc(bool want_send) {
    for (int i = 0; i < 4; ++i) {
        const std::string path = "/dev/lirc" + std::to_string(i);
        const int fd = open(path.c_str(), (want_send ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
        if (fd < 0) continue;
        uint32_t features = 0;
        if (ioctl(fd, LIRC_GET_FEATURES, &features) == 0) {
            if (want_send && (features & LIRC_CAN_SEND_PULSE)) return fd;
            if (!want_send && (features & LIRC_CAN_REC_MODE2)) return fd;
        }
        close(fd);
    }
    return -1;
}

std::string transmit(Pulses pulses, int carrier) {
    if (pulses.empty()) return "nothing to send";
    if (pulses.size() % 2 == 0) pulses.pop_back();  // must start and end with a mark
    if (getenv("ZB_DRY_RUN")) return "";
    // A test run (ZB_EXP_NO_HARDWARE, experiment builds only) never sends a code into the room.
    if (std::string off = experiment::no_infrared(); !off.empty()) return off;
    const int fd = open_lirc(true);
    if (fd < 0) return "no infrared transmitter available";
    uint32_t mode = LIRC_MODE_PULSE, freq = static_cast<uint32_t>(carrier);
    ioctl(fd, LIRC_SET_SEND_MODE, &mode);
    ioctl(fd, LIRC_SET_SEND_CARRIER, &freq);
    const ssize_t want = static_cast<ssize_t>(pulses.size() * sizeof(uint32_t));
    const ssize_t n = write(fd, pulses.data(), static_cast<size_t>(want));
    const int err = errno;
    close(fd);
    return n == want ? "" : std::string("infrared send failed: ") + strerror(err);
}

std::string learned_path() { return util::data_dir() + "/ir_learned.json"; }

std::string norm(const std::string &s) {
    std::string out = util::lower(util::trim(s));
    for (char &c : out)
        if (c == ' ' || c == '-') c = '_';
    return out;
}

const Brand *find_brand(const std::string &name) {
    for (auto &b : table())
        if (name == b.name) return &b;
    return nullptr;
}

bool find_code(const Brand &b, const std::string &key, uint32_t &cmd) {
    for (auto &c : b.codes)
        if (key == c.key) { cmd = c.cmd; return true; }
    return false;
}

}  // namespace

std::vector<std::string> brands() {
    std::vector<std::string> out = {"auto"};
    for (auto &b : table()) out.push_back(b.name);
    return out;
}

std::vector<std::string> keys() {
    return {"power", "volume_up", "volume_down", "mute", "channel_up", "channel_down", "input", "up", "down",
            "left", "right", "ok", "menu", "back", "home", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
}

std::string send(const std::string &brand_in, const std::string &key_in, int repeat, const std::atomic<bool> *cancel) {
    const std::string key = norm(key_in), brand = norm(brand_in);
    if (repeat < 1) repeat = 1;
    if (repeat > kMaxRepeat) repeat = kMaxRepeat;
    auto stopped = [cancel] { return cancel && *cancel; };

    // A learned button always wins: it came from the user's actual remote.
    const json learned_db = util::load_json(learned_path(), json::object());
    if (learned_db.contains(key) && learned_db[key].is_object()) {
        Pulses p = learned_db[key].value("pulses", Pulses{});
        const int carrier = learned_db[key].value("carrier", 38000);
        for (int i = 0; i < repeat; ++i) {
            if (stopped()) return "stopped: the request was cancelled";
            const std::string err = transmit(p, carrier);
            if (!err.empty()) return err;
            usleep(110 * 1000);
        }
        return "";
    }

    std::vector<const Brand *> targets;
    if (brand.empty() || brand == "auto") {
        for (auto &b : table()) targets.push_back(&b);
    } else if (const Brand *b = find_brand(brand)) {
        targets.push_back(b);
    } else {
        return "unknown TV brand '" + brand_in + "'";
    }

    bool sent_any = false;
    for (int i = 0; i < repeat; ++i) {
        for (const Brand *b : targets) {
            uint32_t cmd;
            if (!find_code(*b, key, cmd)) continue;
            if (stopped()) return "stopped: the request was cancelled";
            const std::string err = transmit(encode(*b, cmd), carrier_for(b->proto));
            if (!err.empty()) return err;
            sent_any = true;
            usleep(targets.size() > 1 ? 60 * 1000 : 110 * 1000);
        }
    }
    return sent_any ? "" : "no code for '" + key_in + "' on " + (brand.empty() ? "auto" : brand);
}

std::string learn(const std::string &name_in, int timeout_ms) {
    const std::string name = norm(name_in);
    if (name.empty()) return "a button name is needed";
    const int fd = open_lirc(false);
    if (fd < 0) return "no infrared receiver available";
    uint32_t mode = LIRC_MODE_MODE2;
    ioctl(fd, LIRC_SET_REC_MODE, &mode);

    Pulses pulses;
    int remaining = timeout_ms;
    bool started = false;
    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        // Once a signal has started, a 150 ms gap means the button press is over.
        const int r = poll(&pfd, 1, started ? 150 : remaining);
        if (r <= 0) break;
        uint32_t buf[64];
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(uint32_t); ++i) {
            const uint32_t kind = buf[i] & LIRC_MODE2_MASK, value = buf[i] & LIRC_VALUE_MASK;
            if (kind == LIRC_MODE2_PULSE) { pulses.push_back(value); started = true; }
            else if (kind == LIRC_MODE2_SPACE && started) {
                if (value > 30000 && pulses.size() > 20) goto done;  // first full frame captured
                pulses.push_back(value);
            } else if (kind == LIRC_MODE2_TIMEOUT && started && pulses.size() > 20) goto done;
        }
        if (pulses.size() > 600) break;
    }
done:
    close(fd);
    if (pulses.size() < 12) return "no signal received; point the remote at the top edge and try again";
    if (pulses.size() % 2 == 0) pulses.pop_back();
    json db = util::load_json(learned_path(), json::object());
    db[name] = {{"pulses", pulses}, {"carrier", 38000}};
    util::save_json(learned_path(), db);
    return "";
}

std::vector<std::string> learned() {
    std::vector<std::string> out;
    const json db = util::load_json(learned_path(), json::object());
    for (auto it = db.begin(); it != db.end(); ++it) out.push_back(it.key());
    return out;
}

bool forget(const std::string &name) {
    json db = util::load_json(learned_path(), json::object());
    if (!db.erase(norm(name))) return false;
    util::save_json(learned_path(), db);
    return true;
}

void forget_all() { util::save_json(learned_path(), json::object()); }

}  // namespace ir
