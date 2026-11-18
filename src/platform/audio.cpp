#include "platform/audio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <sstream>
#include <unistd.h>

namespace audio {

namespace {
constexpr float kBaseMicGain = 5.f;                 // amplification at the 100% setting
std::atomic<float> g_mic_gain{kBaseMicGain};
}  // namespace

// ---------------------------------------------------------------- Recorder
Recorder::~Recorder() { cancel(); }

namespace {
std::atomic<bool> g_mic_builtin{true};
std::mutex g_mic_name_mtx;
std::string g_mic_name;                 // the built-in microphone's source name, once found
std::atomic<bool> g_mic_lookup{false};  // a lookup is running

// The built-in microphone is the sound card soldered to the board ("alsa_input.platform-…");
// failing that, any wired input rather than a Bluetooth one.
std::string find_builtin_mic() {
    const auto res = proc::run({"pactl", "list", "short", "sources"}, "", 3000);
    std::string fallback;
    std::istringstream lines(res.out);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream cols(line);
        std::string id, name;
        cols >> id >> name;
        if (name.compare(0, 11, "alsa_input.") != 0) continue;
        if (name.compare(0, 20, "alsa_input.platform-") == 0) return name;
        if (fallback.empty()) fallback = name;
    }
    return fallback;
}

std::string builtin_mic() {
    {
        std::lock_guard<std::mutex> lk(g_mic_name_mtx);
        if (!g_mic_name.empty()) return g_mic_name;
    }
    const std::string name = find_builtin_mic();
    std::lock_guard<std::mutex> lk(g_mic_name_mtx);
    g_mic_name = name;
    return name;
}
}  // namespace

void set_mic_builtin(bool builtin) {
    g_mic_builtin = builtin;
    // Look the name up ahead of time, so the first press of the talk key does not wait for it.
    if (builtin && !g_mic_lookup.exchange(true)) std::thread([] { builtin_mic(); g_mic_lookup = false; }).detach();
}

bool Recorder::start() {
    cancel();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pcm_.clear();
    }
    samples_seen_ = 0;
    level_ = 0;
    std::vector<std::string> argv = {"pw-cat", "--record", "--raw", "--format", "s16", "--rate", std::to_string(kMicRate),
                                     "--channels", "1", "--latency", "20ms"};
    if (g_mic_builtin) {
        const std::string mic = builtin_mic();
        if (!mic.empty()) { argv.push_back("--target"); argv.push_back(mic); }
    }
    argv.push_back("-");
    child_ = proc::spawn(argv, false, true);
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
            float lvl = static_cast<float>((db + 62.0) / 40.0);
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

void Speaker::kill() {
    std::lock_guard<std::mutex> lk(mtx_);
    proc::signal_group(child_, SIGKILL);   // not reaped, nor its pipe closed, here: its writer does that
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

void set_volume_async(int percent) {
    static std::atomic<int> wanted{-1};
    static std::atomic<bool> worker{false};
    wanted = std::max(0, std::min(100, percent));
    if (worker.exchange(true)) return;
    std::thread([] {
        for (;;) {
            const int v = wanted.exchange(-1);
            if (v < 0) {
                worker = false;
                if (wanted < 0 || worker.exchange(true)) return;   // nothing arrived meanwhile
                continue;
            }
            set_volume(v);
        }
    }).detach();
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

// ---------------------------------------------------------------- timer alarm
std::vector<int16_t> alarm_burst(int rate) {
    // Four quick beeps high in the range the small speaker plays loudest, then a pause: the
    // pattern of an alarm clock, and nothing like the soft two-note chime of a reminder. A little
    // third harmonic makes each beep buzz rather than whistle.
    const double freq = 2093;                             // C7
    const int beep = rate * 100 / 1000, gap = rate * 50 / 1000, edge = rate * 5 / 1000;
    std::vector<int16_t> pcm;
    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < beep; ++i) {
            const int from_end = beep - 1 - i;
            const double env = i < edge ? 0.5 - 0.5 * std::cos(M_PI * i / edge)
                             : from_end < edge ? 0.5 - 0.5 * std::cos(M_PI * from_end / edge) : 1.0;
            const double x = 2 * M_PI * freq * i / rate;
            pcm.push_back(static_cast<int16_t>(16000 * env * (std::sin(x) + 0.25 * std::sin(3 * x))));
        }
        pcm.insert(pcm.end(), static_cast<size_t>(gap), 0);
    }
    pcm.insert(pcm.end(), static_cast<size_t>(rate * 500 / 1000), 0);
    return pcm;
}

// One ring: its player, and what its thread is told. Shared by the thread and AlarmSound until
// both are done with it, so a ring that is cut off can finish in its own time.
struct AlarmSound::Ring {
    Speaker speaker;
    std::atomic<bool> stop{false};
    std::function<bool()> wait;
    int max_ms = 0, give_way_ms = 0;
};

namespace {
std::atomic<int> g_alarm_rings{0};   // ring threads still running (static storage: they may outlive a ring)
}  // namespace

AlarmSound &AlarmSound::get() {
    static AlarmSound a;
    return a;
}

void AlarmSound::start(std::function<bool()> wait, int max_ms, int give_way_ms) {
    stop();                            // a ring under way is cut off; it is not waited for
    auto ring = std::make_shared<Ring>();
    ring->wait = std::move(wait);
    ring->max_ms = max_ms;
    ring->give_way_ms = give_way_ms;
    ring_ = ring;
    ++g_alarm_rings;
    std::thread(&AlarmSound::run, std::move(ring)).detach();
}

void AlarmSound::stop() {
    if (!ring_) return;
    ring_->stop = true;
    // Silent there and then. This also frees the ring's thread should it be stuck writing to a
    // player that has stopped reading; the thread reaps the player as it leaves.
    ring_->speaker.kill();
    ring_.reset();
}

void AlarmSound::stop_and_wait() {
    stop();
    // Each player has been ended already; the threads notice within a chunk and reap them.
    for (int i = 0; i < 100 && g_alarm_rings > 0; ++i) usleep(10 * 1000);
}

bool AlarmSound::active() const { return g_alarm_rings > 0; }

void AlarmSound::run(std::shared_ptr<Ring> r) {
    // stop() may end the player while this thread writes to it: the write then fails, and must not
    // raise SIGPIPE, which would end the whole process wherever it is not ignored.
    sigset_t pipe_signal;
    sigemptyset(&pipe_signal);
    sigaddset(&pipe_signal, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_signal, nullptr);
    const int rate = 24000;
    const std::vector<int16_t> burst = alarm_burst(rate);
    const size_t chunk = static_cast<size_t>(rate / 20);   // handed over 50 ms at a time
    const std::vector<int16_t> quiet(chunk, 0);
    if (!r->stop && r->speaker.begin(rate)) {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        auto elapsed_ms = [&t0] {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        };
        long long queued_ms = 0, waited_ms = 0;
        bool first = true;
        while (!r->stop && elapsed_ms() < r->max_ms) {
            // Quiet while the assistant's voice is coming out of the speaker (the timer's name, most
            // likely), so the words can be heard; but after give_way_ms of that in all, it rings on.
            const bool give_way = !first && waited_ms < r->give_way_ms && r->wait && r->wait();
            const std::vector<int16_t> &part = give_way ? quiet : burst;
            first = false;
            for (size_t at = 0; at < part.size() && !r->stop; at += chunk) {
                // Stay at most 200 ms ahead of real time, so that a key press silences it at once.
                const long long ahead = queued_ms - elapsed_ms();
                if (ahead > 200) usleep(static_cast<useconds_t>((ahead - 200) * 1000));
                const size_t n = std::min(chunk, part.size() - at);
                r->speaker.write(part.data() + at, n * sizeof(int16_t));
                queued_ms += static_cast<long long>(n) * 1000 / rate;
            }
            if (give_way) waited_ms += static_cast<long long>(part.size()) * 1000 / rate;
        }
    }
    r->speaker.stop();                                    // cut off what is still queued, and reap it
    --g_alarm_rings;
}

// ---------------------------------------------------------------- Music
Music &Music::get() {
    static Music m;
    return m;
}

void Music::play(std::vector<std::string> paths) {
    std::lock_guard<std::mutex> ctl(ctl_);
    stop_locked();
    if (paths.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_ = std::move(paths);
        index_ = 0;
    }
    running_ = true;
    holds_ &= ~kUser;   // asking for music ends a pause the user set earlier
    const int gen = ++generation_;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, gen]() {
        while (running_ && generation_ == gen) {
            // Wait for quiet before starting a track: the reply is still to be spoken, or the
            // microphone is open.
            if (holds_ != 0) { usleep(50 * 1000); continue; }
            std::string path;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (holds_ != 0) continue;
                if (index_ >= queue_.size()) break;
                path = queue_[index_++];
                current_ = path;
                // The player's own audio buffer is about 12 ms, which any busy moment elsewhere on a
                // small processor outruns: measured on a CardputerZero, playback skipped under load
                // with the default and held steady with 300 ms. Music does not need low latency.
                child_ = proc::spawn({"env", "PULSE_LATENCY_MSEC=300", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", path},
                                     false, false);
                stopped_ = false;
            }
            skip_ = false;
            for (;;) {
                usleep(100 * 1000);
                std::lock_guard<std::mutex> lk(mtx_);
                if (!running_ || generation_ != gen || skip_) {
                    if (stopped_) { proc::signal_group(child_, SIGCONT); stopped_ = false; }
                    proc::stop(child_, SIGTERM);
                    break;
                }
                if (!proc::alive(child_)) break;
            }
        }
        std::lock_guard<std::mutex> lk(mtx_);
        if (generation_ == gen) { current_.clear(); running_ = false; }
    });
}

void Music::stop() {
    std::lock_guard<std::mutex> ctl(ctl_);
    stop_locked();
}

void Music::stop_locked() {
    running_ = false;
    ++generation_;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(mtx_);
    if (stopped_) { proc::signal_group(child_, SIGCONT); stopped_ = false; }
    proc::stop(child_, SIGTERM);
    queue_.clear();
    current_.clear();
    title_.clear();
    live_ = false;
    holds_ &= ~kUser;
}

void Music::play_live(const std::string &url, const std::string &title) {
    std::lock_guard<std::mutex> ctl(ctl_);
    stop_locked();
    if (url.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        current_ = url;
        title_ = title;
        problem_.clear();
    }
    live_ = true;
    running_ = true;
    holds_ &= ~kUser;   // asking for a station ends a pause the user set earlier
    const int gen = ++generation_;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&Music::run_live, this, gen, url);
}

void Music::run_live(int gen, std::string url) {
    using clock = std::chrono::steady_clock;
    auto wanted = [this, gen] { return running_ && generation_ == gen; };
    int failures = 0;   // starts in a row that ended by themselves
    while (wanted()) {
        // Like a track, the stream waits for quiet: the reply is still to be spoken, the microphone
        // is open, or the user paused it.
        if (holds_ != 0) { usleep(50 * 1000); continue; }
        clock::time_point started;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (holds_ != 0 || !wanted()) continue;
            // The same player and output buffer as music. rw_timeout ends a player whose stream has
            // gone silent (the Wi-Fi dropped) after 15 s, so that it is joined again rather than
            // left waiting for ever; -autoexit ends it when the station closes the stream. A stream
            // never ends by itself, so the player goes with this thread, which outlives it: an app
            // that crashes or is killed does not leave a station playing that nothing can stop.
            child_ = proc::spawn({"env", "PULSE_LATENCY_MSEC=300", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
                                  "-rw_timeout", "15000000", url},
                                 false, false, false, true);
            stopped_ = false;
            started = clock::now();
        }
        bool ended_itself = false;
        for (;;) {
            usleep(100 * 1000);
            std::lock_guard<std::mutex> lk(mtx_);
            if (!wanted() || holds_ != 0) {
                // Stopped, replaced, or held: hold() has silenced it already. Nothing of a live stream
                // is worth keeping, so the player is ended at once, suspended or not.
                proc::stop(child_, SIGKILL);
                stopped_ = false;
                break;
            }
            if (!proc::alive(child_)) { ended_itself = true; break; }
        }
        if (!ended_itself) continue;   // the top of the loop waits for the holds, or leaves
        // A stream that played for a while and dropped starts the count again; one that will not
        // start at all (off the air, refused, a wrong address) is given up after three tries. "A
        // while" is well past the time a stall takes to end the player: a server that sends its
        // opening burst and then nothing was measured ending it after 31 s (about twice the 15 s
        // read timeout), and with 30 s here it was joined again every half-minute for ever.
        failures = clock::now() - started > std::chrono::seconds(75) ? 1 : failures + 1;
        if (failures >= 3) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (generation_ == gen)
                problem_ = (title_.empty() ? std::string("The radio") : title_) +
                           " stopped: the stream could not be played. The station may be off the air or not available in "
                           "this country, or its address may have changed.";
            break;
        }
        for (int i = 0; i < failures * 20 - 10 && wanted(); ++i) usleep(100 * 1000);   // 1 s, then 3 s
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (generation_ == gen) { current_.clear(); title_.clear(); running_ = false; live_ = false; }
}

std::string Music::title() {
    std::lock_guard<std::mutex> lk(mtx_);
    return live_ ? title_ : std::string();
}

std::string Music::take_problem() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string out;
    out.swap(problem_);
    return out;
}

void Music::next() { skip_ = true; }

void Music::previous() {
    {
        // index_ already points past the track that is playing.
        std::lock_guard<std::mutex> lk(mtx_);
        index_ = index_ >= 2 ? index_ - 2 : 0;
    }
    skip_ = true;
}

void Music::hold(int reasons, bool on) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (on) holds_ |= reasons; else holds_ &= ~reasons;
    if (!child_.running()) return;
    const bool want_stopped = holds_ != 0;
    if (want_stopped == stopped_) return;
    proc::signal_group(child_, want_stopped ? SIGSTOP : SIGCONT);
    stopped_ = want_stopped;
}

bool Music::playing() { return running_; }

std::string Music::current() {
    std::lock_guard<std::mutex> lk(mtx_);
    return current_;
}

}  // namespace audio
