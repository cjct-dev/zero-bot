// The two pieces of reading a streamed chat reply that keep state from one network chunk to the
// next (llm::chat): the event stream split into lines, and the answer's text cleared of thinking
// markers. Header-only and pure, so tests/unit/llm_stream_test.cpp checks them without a service.
#pragma once
#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>

namespace llm {

// Splits the event stream into its lines as the network hands it over: a line cut between chunks is
// carried over to the next, and the "\r" before a "\n" is dropped.
class LineSplitter {
public:
    void feed(const char *data, size_t n, const std::function<void(const std::string &)> &on_line) {
        pending_.append(data, n);
        size_t pos;
        while ((pos = pending_.find('\n')) != std::string::npos) {
            std::string line = pending_.substr(0, pos);
            pending_.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            on_line(line);
        }
    }
    // The stream has ended: what came after its last "\n", if anything, as it came.
    void finish(const std::function<void(const std::string &)> &on_line) {
        if (!pending_.empty()) on_line(pending_);
        pending_.clear();
    }
    void reset() { pending_.clear(); }

private:
    std::string pending_;   // a partial line carried between network chunks
};

// With reasoning switched off, some models still leak their thinking markers into the answer: a
// "<think>...</think>" block, or the answer, a stray "</think>", then the answer again. Neither may
// reach the speaker, so text is passed on only once it is known to be clear of a marker.
class ThinkFilter {
public:
    // The next piece of the answer: what is clear of markers goes to emit (which may be handed "").
    // final: no more is coming, so a tail kept back for a marker split across chunks goes out too.
    void feed(const std::string &piece, bool final, const std::function<void(const std::string &)> &emit) {
        static const std::string open = "<think>", close = "</think>";
        if (answer_over_) return;
        held_ += piece;
        for (;;) {
            const size_t c = held_.find(close);
            if (thinking_) {
                if (c == std::string::npos) { if (held_.size() > close.size()) held_.erase(0, held_.size() - close.size()); break; }
                held_.erase(0, c + close.size());
                thinking_ = false;
                continue;
            }
            const size_t o = held_.find(open);
            if (c != std::string::npos && (o == std::string::npos || c < o)) {
                emit(held_.substr(0, c));
                held_.clear();
                answer_over_ = true;
                break;
            }
            if (o != std::string::npos) {
                emit(held_.substr(0, o));
                held_.erase(0, o + open.size());
                thinking_ = true;
                continue;
            }
            // Keep back a tail that could be the start of a marker split across chunks.
            size_t keep = 0;
            for (size_t n = std::min(held_.size(), close.size() - 1); n > 0 && !final; --n)
                if (close.compare(0, n, held_, held_.size() - n, n) == 0 || open.compare(0, n, held_, held_.size() - n, n) == 0) { keep = n; break; }
            emit(held_.substr(0, held_.size() - keep));
            held_.erase(0, held_.size() - keep);
            break;
        }
        if (final && thinking_) held_.clear();
    }
    void reset() {
        held_.clear();
        thinking_ = answer_over_ = false;
    }

private:
    std::string held_;
    bool thinking_ = false, answer_over_ = false;
};

}  // namespace llm
