#include "platform/codec.h"

#include <algorithm>

#include <dlfcn.h>

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
