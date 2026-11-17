#include "platform/audio.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstring>
#include <unistd.h>

namespace audio {

namespace {
constexpr float kBaseMicGain = 5.f;                 // amplification at the 100% setting
std::atomic<float> g_mic_gain{kBaseMicGain};
}  // namespace

// ---------------------------------------------------------------- Recorder
Recorder::~Recorder() { cancel(); }

bool Recorder::start() {
    cancel();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pcm_.clear();
    }
    samples_seen_ = 0;
    level_ = 0;
    child_ = proc::spawn({"pw-cat", "--record", "--raw", "--format", "s16", "--rate", std::to_string(kMicRate),
                          "--channels", "1", "--latency", "20ms", "-"},
                         false, true);
    if (!child_.running()) return false;
    active_ = true;
    thread_ = std::thread(&Recorder::reader, this);
    return true;
}

void Recorder::reader() {
    int16_t buf[1024];
    size_t carry = 0;  // odd byte left over between reads
    char *raw = reinterpret_cast<char *>(buf);
    for (;;) {
        ssize_t n = read(child_.out, raw + carry, sizeof(buf) - carry);
        if (n <= 0) break;
        size_t total = carry + static_cast<size_t>(n);
        size_t count = total / 2;
        if (count) {
            // The built-in microphone is quiet, so samples are amplified here (with clipping
            // protection) rather than by changing system mixer levels other apps rely on.
            const float gain = g_mic_gain.load();
            double sum = 0;
            for (size_t i = 0; i < count; ++i) {
                float v = static_cast<float>(buf[i]) * gain;
                if (v > 32767.f) v = 32767.f;
                if (v < -32768.f) v = -32768.f;
                buf[i] = static_cast<int16_t>(v);
                sum += static_cast<double>(v) * v;
            }
            // Map RMS to 0..1 on a log scale so quiet speech still moves the meter.
            const double rms = std::sqrt(sum / static_cast<double>(count));
            double db = rms > 1 ? 20.0 * std::log10(rms / 32768.0) : -90.0;
            float lvl = static_cast<float>((db + 58.0) / 40.0);
            if (lvl < 0) lvl = 0;
            if (lvl > 1) lvl = 1;
            level_ = level_ * 0.6f + lvl * 0.4f;
            std::lock_guard<std::mutex> lk(mtx_);
            pcm_.insert(pcm_.end(), buf, buf + count);
            samples_seen_ = pcm_.size();
        }
        carry = total - count * 2;
        if (carry) raw[0] = raw[count * 2];
    }
}

std::vector<int16_t> Recorder::stop() {
    std::vector<int16_t> out;
    if (!active_) return out;
    proc::signal_group(child_, SIGINT);
    if (thread_.joinable()) thread_.join();
    proc::stop(child_, SIGTERM);
    active_ = false;
    std::lock_guard<std::mutex> lk(mtx_);
    out.swap(pcm_);
    return out;
}

void Recorder::cancel() {
    if (!active_) return;
    (void)stop();
}

// ---------------------------------------------------------------- Speaker
Speaker::~Speaker() { stop(); }

bool Speaker::begin(int rate) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (child_.running()) return true;
    child_ = proc::spawn({"pw-cat", "--playback", "--raw", "--format", "s16", "--rate", std::to_string(rate),
                          "--channels", "1", "--latency", "80ms", "-"},
                         true, false);
    return child_.running();
}

void Speaker::write(const void *data, size_t bytes) {
    int fd;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fd = child_.in;
    }
    if (fd < 0) return;
    const char *p = static_cast<const char *>(data);
    while (bytes) {
        ssize_t n = ::write(fd, p, bytes);
        if (n <= 0) return;  // player was stopped
        p += n;
        bytes -= static_cast<size_t>(n);
    }
}

void Speaker::finish() {
    std::lock_guard<std::mutex> lk(mtx_);
    proc::close_in(child_);
}

void Speaker::stop() {
    std::lock_guard<std::mutex> lk(mtx_);
    proc::stop(child_, SIGKILL);
}

bool Speaker::playing() {
    std::lock_guard<std::mutex> lk(mtx_);
    return proc::alive(child_);
}

// ---------------------------------------------------------------- helpers
std::string wav_from_pcm(const std::vector<int16_t> &pcm, int rate) {
    const uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * 2);
    std::string out;
    out.reserve(44 + data_bytes);
    auto u32 = [&](uint32_t v) { out.append(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.append(reinterpret_cast<const char *>(&v), 2); };
    out += "RIFF"; u32(36 + data_bytes); out += "WAVEfmt ";
    u32(16); u16(1); u16(1); u32(static_cast<uint32_t>(rate)); u32(static_cast<uint32_t>(rate) * 2); u16(2); u16(16);
    out += "data"; u32(data_bytes);
    out.append(reinterpret_cast<const char *>(pcm.data()), data_bytes);
    return out;
}

void set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", percent / 100.0);
    proc::run({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", buf}, "", 3000);
}

void set_mic_gain(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 300) percent = 300;
    g_mic_gain = static_cast<float>(percent) / 100.f * kBaseMicGain;
}

void normalize(std::vector<int16_t> &pcm) {
    int peak = 1;
    for (int16_t v : pcm) peak = std::max(peak, std::abs(static_cast<int>(v)));
    // Bring the loudest moment to about 70% of full scale; never amplify noise by more than 20x.
    const float gain = std::min(20.f, 23000.f / static_cast<float>(peak));
    if (gain < 1.15f) return;
    for (int16_t &v : pcm) v = static_cast<int16_t>(static_cast<float>(v) * gain);
}

void chime(int kind) {
    // Two short sine blips, synthesized so no sound files are needed.
    const int rate = 24000;
    const double freqs[3][2] = {{660, 880}, {880, 660}, {988, 1319}};
    std::vector<int16_t> pcm;
    for (int part = 0; part < 2; ++part) {
        const int n = rate * (kind == 2 ? 140 : 70) / 1000;
        for (int i = 0; i < n; ++i) {
            const double env = std::sin(M_PI * i / n);
            pcm.push_back(static_cast<int16_t>(9000 * env * std::sin(2 * M_PI * freqs[kind % 3][part] * i / rate)));
        }
    }
    std::thread([pcm = std::move(pcm), rate]() {
        Speaker sp;
        if (!sp.begin(rate)) return;
        sp.write(pcm.data(), pcm.size() * 2);
        sp.finish();
        for (int i = 0; i < 100 && sp.playing(); ++i) usleep(20 * 1000);
    }).detach();
}

// ---------------------------------------------------------------- Music
Music &Music::get() {
    static Music m;
    return m;
}

void Music::play(std::vector<std::string> paths) {
    stop();
    if (paths.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_ = std::move(paths);
        index_ = 0;
    }
    running_ = true;
    paused_ = false;
    const int gen = ++generation_;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, gen]() {
        while (running_ && generation_ == gen) {
            std::string path;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (index_ >= queue_.size()) break;
                path = queue_[index_++];
                current_ = path;
                child_ = proc::spawn({"ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", path}, false, false);
            }
            skip_ = false;
            for (;;) {
                usleep(100 * 1000);
                std::lock_guard<std::mutex> lk(mtx_);
                if (!running_ || generation_ != gen || skip_) { proc::stop(child_, SIGTERM); break; }
                if (!proc::alive(child_)) break;
            }
        }
        std::lock_guard<std::mutex> lk(mtx_);
        if (generation_ == gen) { current_.clear(); running_ = false; }
    });
}

void Music::stop() {
    running_ = false;
    ++generation_;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(mtx_);
    proc::stop(child_, SIGTERM);
    queue_.clear();
    current_.clear();
    paused_ = false;
}

void Music::next() { skip_ = true; }

void Music::pause(bool on) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!child_.running() || paused_ == on) return;
    proc::signal_group(child_, on ? SIGSTOP : SIGCONT);
    paused_ = on;
}

bool Music::playing() { return running_; }

std::string Music::current() {
    std::lock_guard<std::mutex> lk(mtx_);
    return current_;
}

}  // namespace audio
