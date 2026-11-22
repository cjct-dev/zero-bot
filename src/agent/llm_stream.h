// The pieces of reading a streamed chat reply that keep state from one network chunk to the next
// (llm::chat): the event stream split into lines, the answer's text cleared of thinking markers, and
// tool calls put together from their pieces. Header-only and pure, so tests/unit/llm_stream_test.cpp
// checks them without a service.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "agent/llm.h"

namespace llm {

// The most tool calls one reply may carry. The stream numbers each call ("index") and a slot is kept
// for every number up to it, so a number out of range is a fault in the stream, not a call: a negative
// one once wrapped round to a slot far past the end, and a huge one would have asked for gigabytes.
constexpr int kMaxToolCalls = 16;

// One piece of a streamed tool call (an element of an event's "delta.tool_calls"), added to the call
// its index names; a piece without one is the first call's. False, with `error` set to a sentence for
// the user and nothing added, when the index is not a whole number from 0 to kMaxToolCalls - 1.
inline bool add_tool_delta(std::vector<ToolCall> &calls, const json &tc, std::string &error) {
    size_t idx = 0;
    const auto it = tc.is_object() ? tc.find("index") : tc.end();
    if (tc.is_object() && it != tc.end() && !it->is_null()) {
        const double n = it->is_number() ? it->get<double>() : -1;
        if (!(n >= 0 && n < kMaxToolCalls) || n != std::floor(n)) {
            error = "The AI service sent a reply this app could not read (a tool call numbered " +
                    it->dump(-1, ' ', true, json::error_handler_t::replace).substr(0, 24) + ").";
            return false;
        }
        idx = static_cast<size_t>(n);
    }
    if (calls.size() <= idx) calls.resize(idx + 1);
    if (!tc.is_object()) return true;
    ToolCall &call = calls[idx];
    const auto id = tc.find("id");
    if (id != tc.end() && id->is_string() && !id->get<std::string>().empty()) call.id = id->get<std::string>();
    const auto fn = tc.find("function");
    if (fn != tc.end() && fn->is_object()) {
        const auto name = fn->find("name");
        if (name != fn->end() && name->is_string() && !name->get<std::string>().empty()) call.name = name->get<std::string>();
        const auto args = fn->find("arguments");
        if (args != fn->end() && args->is_string()) call.args += args->get<std::string>();
    }
    return true;
}

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
