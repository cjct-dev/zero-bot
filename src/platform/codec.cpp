#include "platform/codec.h"

#include <algorithm>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

namespace codec {

namespace {

// Only the handful of entry points needed are declared here, which avoids depending on the
// libraries' development headers at build time.
struct Lame {
    void *lib = nullptr;
    void *(*init)() = nullptr;
    int (*set_in_samplerate)(void *, int) = nullptr;
    int (*set_num_channels)(void *, int) = nullptr;
    int (*set_brate)(void *, int) = nullptr;
    int (*set_mode)(void *, int) = nullptr;
    int (*set_quality)(void *, int) = nullptr;
    int (*set_write_vbr_tag)(void *, int) = nullptr;
    int (*init_params)(void *) = nullptr;
    int (*encode_buffer)(void *, const short *, const short *, int, unsigned char *, int) = nullptr;
    int (*encode_flush)(void *, unsigned char *, int) = nullptr;
    int (*close)(void *) = nullptr;
    bool ok = false;
};

struct OpusFile {
    void *lib = nullptr;
    void *(*open_memory)(const unsigned char *, size_t, int *) = nullptr;
    int (*channel_count)(const void *, int) = nullptr;
    int (*read)(void *, int16_t *, int, int *) = nullptr;
    void (*free)(void *) = nullptr;
    bool ok = false;
};

template <typename Fn>
bool bind(void *lib, const char *name, Fn &out) {
    out = reinterpret_cast<Fn>(dlsym(lib, name));
    return out != nullptr;
}

const Lame &lame() {
    static const Lame l = [] {
        Lame x;
        x.lib = dlopen("libmp3lame.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!x.lib) return x;
        x.ok = bind(x.lib, "lame_init", x.init) && bind(x.lib, "lame_set_in_samplerate", x.set_in_samplerate) &&
               bind(x.lib, "lame_set_num_channels", x.set_num_channels) && bind(x.lib, "lame_set_brate", x.set_brate) &&
               bind(x.lib, "lame_set_mode", x.set_mode) && bind(x.lib, "lame_set_quality", x.set_quality) &&
               bind(x.lib, "lame_set_bWriteVbrTag", x.set_write_vbr_tag) && bind(x.lib, "lame_init_params", x.init_params) &&
               bind(x.lib, "lame_encode_buffer", x.encode_buffer) && bind(x.lib, "lame_encode_flush", x.encode_flush) &&
               bind(x.lib, "lame_close", x.close);
        return x;
    }();
    return l;
}

const OpusFile &opusfile() {
    static const OpusFile o = [] {
        OpusFile x;
        x.lib = dlopen("libopusfile.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!x.lib) return x;
        x.ok = bind(x.lib, "op_open_memory", x.open_memory) && bind(x.lib, "op_channel_count", x.channel_count) &&
               bind(x.lib, "op_read", x.read) && bind(x.lib, "op_free", x.free);
        return x;
    }();
    return o;
}

}  // namespace

namespace {

// libmpg123, used in "feed" mode: the file is read here and handed over in pieces, which keeps the
// library's file and off_t interfaces (whose names vary between builds) out of it.
struct Mpg123 {
    void *lib = nullptr;
    bool ok = false;
    void *(*make)(const char *, int *) = nullptr;
    void (*free)(void *) = nullptr;
    int (*format_none)(void *) = nullptr;
    int (*format)(void *, long, int, int) = nullptr;
    int (*open_feed)(void *) = nullptr;
    int (*feed)(void *, const unsigned char *, size_t) = nullptr;
    int (*decode)(void *, const unsigned char *, size_t, unsigned char *, size_t, size_t *) = nullptr;
    int (*getformat)(void *, long *, int *, int *) = nullptr;
};
constexpr int kMpgOk = 0, kMpgNewFormat = -11, kMpgNeedMore = -10, kMpgDone = -12;
constexpr int kMpgSigned16 = 0xd0, kMpgMonoOrStereo = 1 | 2;

const Mpg123 &mpg123() {
    static const Mpg123 m = [] {
        Mpg123 x;
        x.lib = dlopen("libmpg123.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!x.lib) return x;
        x.ok = bind(x.lib, "mpg123_new", x.make) && bind(x.lib, "mpg123_delete", x.free) &&
               bind(x.lib, "mpg123_format_none", x.format_none) && bind(x.lib, "mpg123_format", x.format) &&
               bind(x.lib, "mpg123_open_feed", x.open_feed) && bind(x.lib, "mpg123_feed", x.feed) &&
               bind(x.lib, "mpg123_decode", x.decode) &&
               bind(x.lib, "mpg123_getformat", x.getformat);
        // Before 1.27 the library had to be set up once; later ones do nothing here.
        int (*init)() = nullptr;
        if (x.ok && bind(x.lib, "mpg123_init", init) && init() != kMpgOk) x.ok = false;
        return x;
    }();
    return m;
}

}  // namespace

Mp3File::~Mp3File() {
    if (mh_) mpg123().free(mh_);
    if (fd_ >= 0) ::close(fd_);
}

bool Mp3File::feed() {
    if (input_done_) return false;
    unsigned char buf[16384];
    const ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n <= 0) { input_done_ = true; return false; }
    return mpg123().feed(mh_, buf, static_cast<size_t>(n)) == kMpgOk;
}

bool Mp3File::open(const std::string &path) {
    const Mpg123 &m = mpg123();
    if (!m.ok || mh_) return false;
    int err = 0;
    mh_ = m.make(nullptr, &err);
    if (!mh_) return false;
    m.format_none(mh_);
    for (long r : {8000L, 11025L, 12000L, 16000L, 22050L, 24000L, 32000L, 44100L, 48000L})
        m.format(mh_, r, kMpgMonoOrStereo, kMpgSigned16);
    if (m.open_feed(mh_) != kMpgOk) return false;
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return false;
    // Read until the decoder knows the format, keeping any samples it made on the way.
    unsigned char out[8192];
    size_t fed = 0;
    while (rate_ == 0 && fed < (1u << 20)) {
        size_t done = 0;
        const int r = m.decode(mh_, nullptr, 0, out, sizeof out, &done);
        if (r == kMpgNewFormat) {
            long rate = 0;
            int channels = 0, encoding = 0;
            m.getformat(mh_, &rate, &channels, &encoding);
            if (encoding != kMpgSigned16 || rate <= 0 || (channels != 1 && channels != 2)) return false;
            rate_ = static_cast<int>(rate);
            channels_ = channels;
        } else if (r == kMpgNeedMore) {
            if (!feed()) return false;
            fed += 16384;
        } else if (r != kMpgOk) {
            return false;
        }
        pending_.insert(pending_.end(), out, out + done);
    }
    return rate_ > 0;
}

long Mp3File::read(unsigned char *out, size_t max) {
    if (!mh_ || max == 0) return -1;
    if (!pending_.empty()) {
        const size_t n = std::min(max, pending_.size());
        std::copy(pending_.begin(), pending_.begin() + static_cast<long>(n), out);
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(n));
        return static_cast<long>(n);
    }
    const Mpg123 &m = mpg123();
    for (;;) {
        size_t done = 0;
        const int r = m.decode(mh_, nullptr, 0, out, max, &done);
        if (done > 0) return static_cast<long>(done);
        if (r == kMpgNeedMore) {
            if (!feed()) return 0;   // the whole file is decoded
            continue;
        }
        if (r == kMpgOk) continue;
        if (r == kMpgDone || r == kMpgNewFormat) return 0;   // a new format mid-file would play at the wrong speed
        return -1;
    }
}

bool can_encode_mp3() { return lame().ok; }
bool can_decode_opus() { return opusfile().ok; }

bool mp3_encode(const std::vector<int16_t> &pcm, int sample_rate, int kbps, std::string &mp3) {
    const Lame &l = lame();
    if (!l.ok || pcm.empty()) return false;
    void *enc = l.init();
    if (!enc) return false;
    l.set_in_samplerate(enc, sample_rate);
    l.set_num_channels(enc, 1);
    l.set_brate(enc, kbps);
    l.set_mode(enc, 3);              // mono
    l.set_quality(enc, 7);           // favour speed; speech at this bitrate gains nothing from slower modes
    l.set_write_vbr_tag(enc, 0);
    if (l.init_params(enc) < 0) { l.close(enc); return false; }

    mp3.clear();
    std::vector<unsigned char> buf(16384);
    const int chunk = 4096;
    for (size_t pos = 0; pos < pcm.size(); pos += chunk) {
        const int n = static_cast<int>(std::min<size_t>(chunk, pcm.size() - pos));
        const int wrote = l.encode_buffer(enc, pcm.data() + pos, pcm.data() + pos, n, buf.data(), static_cast<int>(buf.size()));
        if (wrote < 0) { l.close(enc); return false; }
        mp3.append(reinterpret_cast<const char *>(buf.data()), static_cast<size_t>(wrote));
    }
    const int tail = l.encode_flush(enc, buf.data(), static_cast<int>(buf.size()));
    if (tail > 0) mp3.append(reinterpret_cast<const char *>(buf.data()), static_cast<size_t>(tail));
    l.close(enc);
    return !mp3.empty();
}

bool opus_decode(const std::string &ogg, std::vector<int16_t> &pcm) {
    const OpusFile &o = opusfile();
    if (!o.ok || ogg.size() < 64 || ogg.compare(0, 4, "OggS") != 0) return false;
    int err = 0;
    void *of = o.open_memory(reinterpret_cast<const unsigned char *>(ogg.data()), ogg.size(), &err);
    if (!of) return false;
    pcm.clear();
    std::vector<int16_t> buf(11520);   // 120 ms of stereo at 48 kHz, the largest Opus packet
    for (;;) {
        int link = 0;
        const int samples = o.read(of, buf.data(), static_cast<int>(buf.size()), &link);
        if (samples <= 0) break;
        const int channels = o.channel_count(of, link);
        if (channels <= 1) {
            pcm.insert(pcm.end(), buf.begin(), buf.begin() + samples);
        } else {
            for (int i = 0; i < samples; ++i) {   // mix down to mono
                int sum = 0;
                for (int c = 0; c < channels; ++c) sum += buf[static_cast<size_t>(i * channels + c)];
                pcm.push_back(static_cast<int16_t>(sum / channels));
            }
        }
    }
    o.free(of);
    return !pcm.empty();
}

}  // namespace codec
