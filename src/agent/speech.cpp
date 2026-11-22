#include "agent/speech.h"

#include <unistd.h>
#include <vector>

#include "agent/llm.h"
#include "agent/prompt.h"
#include "platform/audio.h"

namespace {
audio::Speaker &speaker() {
    static audio::Speaker s;
    return s;
}
}  // namespace

SpeechQueue &SpeechQueue::get() {
    static SpeechQueue q;
    return q;
}

SpeechQueue::SpeechQueue() { thread_ = std::thread(&SpeechQueue::run, this); thread_.detach(); }

void SpeechQueue::enqueue(const std::string &sentence) {
    const std::string clean = prompt::for_speech(sentence);
    if (clean.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (ended_) {               // the first sentence of a new answer
        ++answer_;
        if (observer_) observer_(answer_, false);
    }
    queue_.push_back({clean, answer_});
    ended_ = false;
    working_ = true;
    cv_.notify_one();
}

void SpeechQueue::end_of_answer() {
    std::lock_guard<std::mutex> lk(mtx_);
    ended_ = true;
    cv_.notify_one();
}

void SpeechQueue::stop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
        ended_ = true;
        ++generation_;
        stop_flag_ = true;
    }
    speaker().stop();
    playing_ = false;
    cv_.notify_one();
}

bool SpeechQueue::active() { return working_; }

void SpeechQueue::failed(int answer, const std::string &reason) {
    if (answer == reported_ || !on_failure_) return;   // one note per answer, not one per sentence
    reported_ = answer;
    on_failure_(reason);
}

// The open stream ends: its last sound plays out, unless stop() cuts it short. Speech thread, lock not held.
void SpeechQueue::close_stream(int gen) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stream_open_ = false;
    }
    speaker().finish();
    while (speaker().playing() && generation_ == gen) usleep(30 * 1000);
    playing_ = false;
}

// The newest answer with nothing left in the queue (lock held).
int SpeechQueue::last_unqueued() const { return queue_.empty() ? answer_ : queue_.front().answer - 1; }

// The answers up to `answer` are over: each is told to the observer once, in order (lock held). An answer
// whose next one came before it had played out shares its stream, and the player cannot say where in it
// one ends: both are over when the stream is, the first told late.
void SpeechQueue::played_out(int answer) {
    for (; over_ < answer; ++over_)
        if (observer_) observer_(over_ + 1, true);
}

void SpeechQueue::run() {
    for (;;) {
        Sentence next;
        int gen;
        if (take(next, gen)) speak(next, gen);
    }
}

// Waits for something to do: a stop to deal with, an answer to finish, or the next sentence, which it
// takes (true) with the generation it belongs to.
bool SpeechQueue::take(Sentence &next, int &gen) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [&] { return !queue_.empty() || stop_flag_ || (ended_ && working_); });
    gen = generation_;
    if (stop_flag_) {
        stop_flag_ = false;
        stream_open_ = false;
        playing_ = false;
        played_out(last_unqueued());   // cut short, or never begun
        if (queue_.empty()) {
            working_ = false;
            audio::Music::get().hold(audio::Music::kSpeech | audio::Music::kExchange, false);
            return false;
        }
    }
    if (queue_.empty()) {
        // The answer is complete: close the stream and wait for the last audio to play out.
        if (stream_open_) {
            lk.unlock();
            close_stream(gen);
            lk.lock();
        }
        played_out(last_unqueued());
        if (queue_.empty()) {
            working_ = false;
            audio::Music::get().hold(audio::Music::kSpeech | audio::Music::kExchange, false);
        }
        return false;
    }
    next = queue_.front();
    queue_.pop_front();
    return true;
}

// One sentence synthesized and written to the stream (opened for its answer if need be), unless stop()
// comes first.
void SpeechQueue::speak(const Sentence &next, int gen) {
    std::string pcm, err;
    std::atomic<bool> cancel{false};
    // Synthesis is a blocking HTTP call; poll the generation so stop() takes effect quickly.
    std::thread watcher([&] {
        while (!cancel && generation_ == gen) usleep(20 * 1000);
        cancel = true;
    });
    const bool ok = llm::synthesize(next.text, pcm, err, &cancel);
    const bool stale = generation_ != gen;
    cancel = true;
    watcher.join();
    if (stale) return;
    if (!ok || pcm.empty()) {
        failed(next.answer, ok ? "the speech service sent no audio" : err);
        return;
    }
    if (!stream_open_) {
        audio::Music::get().hold(audio::Music::kSpeech, true);
        if (!speaker().begin(llm::kTtsRate)) { failed(next.answer, "the speaker could not be opened"); return; }
        std::lock_guard<std::mutex> lk(mtx_);
        stream_open_ = true;
        playing_ = true;
    }
    speaker().write(pcm.data(), pcm.size());
    // A short pause between sentences sounds more natural than back-to-back audio.
    static const std::vector<char> gap(static_cast<size_t>(llm::kTtsRate) * 2 * 120 / 1000, 0);
    speaker().write(gap.data(), gap.size());
}

std::vector<std::string> SpeechQueue::Chunker::feed(const std::string &piece) {
    std::vector<std::string> out;
    buf_ += piece;
    for (;;) {
        size_t cut = std::string::npos;
        for (size_t i = 0; i < buf_.size(); ++i) {
            const char c = buf_[i];
            const bool last = i + 1 == buf_.size();
            if (c == '\n' && i >= 2) { cut = i + 1; break; }
            if ((c == '.' || c == '!' || c == '?') && !last && buf_[i + 1] == ' ' && i >= 24) {
                // Do not split decimals or initials such as "3.5" or "U.S. dollars".
                if (c == '.' && i > 0 && isdigit(static_cast<unsigned char>(buf_[i - 1])) && i + 2 < buf_.size() &&
                    isdigit(static_cast<unsigned char>(buf_[i + 2])))
                    continue;
                cut = i + 1;
                break;
            }
            if (i >= 240 && (c == ',' || c == ';' || c == ' ')) { cut = i + 1; break; }
        }
        if (cut == std::string::npos) break;
        out.push_back(buf_.substr(0, cut));
        buf_.erase(0, cut);
    }
    return out;
}

std::string SpeechQueue::Chunker::flush() {
    std::string rest;
    rest.swap(buf_);
    return rest;
}
