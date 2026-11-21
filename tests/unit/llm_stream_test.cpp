// Unit tests for agent/llm_stream.h: the event stream split into lines across network chunks, and the
// answer cleared of thinking markers however the pieces fall. Header-only and pure: nothing linked.
// Each check prints one line on failure; the exit status is non-zero when any fails.
#include "agent/llm_stream.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_passed = 0, g_failed = 0;

void check(bool ok, const std::string &what) {
    if (ok) ++g_passed;
    else { ++g_failed; printf("FAIL %s\n", what.c_str()); }
}

using Lines = std::vector<std::string>;

// The lines of a stream handed over in these chunks, then ended.
Lines split(const std::vector<std::string> &chunks, bool finish = true) {
    llm::LineSplitter s;
    Lines out;
    auto on_line = [&](const std::string &l) { out.push_back(l); };
    for (const std::string &c : chunks) s.feed(c.data(), c.size(), on_line);
    if (finish) s.finish(on_line);
    return out;
}

// What reaches the caller of an answer streamed in these pieces, then ended; `pieces_out` gets what
// each piece let through (before the end).
std::string clear(const std::vector<std::string> &pieces, std::vector<std::string> *pieces_out = nullptr) {
    llm::ThinkFilter f;
    std::string out, now;
    auto emit = [&](const std::string &t) { out += t; now += t; };
    for (const std::string &p : pieces) {
        now.clear();
        f.feed(p, false, emit);
        if (pieces_out) pieces_out->push_back(now);
    }
    f.feed("", true, emit);
    return out;
}

}  // namespace

int main() {
    // The event stream's lines.
    check(split({"data: a\n"}) == Lines{"data: a"}, "one line");
    check(split({"dat", "a: b\r", "\n"}) == Lines{"data: b"}, "a line cut between chunks, its \\r dropped");
    check(split({"x\ny\n\n"}) == Lines{"x", "y", ""}, "several lines in one chunk, an empty one kept");
    check(split({"data: [DONE]"}) == Lines{"data: [DONE]"}, "a last line without its newline comes out at the end");
    check(split({"tail\r"}) == Lines{"tail\r"}, "the last partial line is handed over as it came");
    check(split({"a\nb"}, false) == Lines{"a"}, "a partial line waits for its newline");
    {
        llm::LineSplitter s;
        Lines out;
        auto on_line = [&](const std::string &l) { out.push_back(l); };
        s.feed("half", 4, on_line);
        s.reset();
        s.feed("new\n", 4, on_line);
        s.finish(on_line);
        s.finish(on_line);
        check(out == Lines{"new"}, "reset drops a partial line; finish hands the rest over once");
    }

    // Thinking markers.
    check(clear({"Hello ", "there."}) == "Hello there.", "plain text passes as it is");
    std::vector<std::string> each;
    clear({"Hello ", "there."}, &each);
    check(each == std::vector<std::string>{"Hello ", "there."}, "plain text is passed on as it comes, not held");
    check(clear({"<think>let me see</think>Lisbon."}) == "Lisbon.", "a thinking block is dropped");
    check(clear({"<thi", "nk>reasoning</th", "ink>Lis", "bon."}) == "Lisbon.", "markers cut between pieces");
    check(clear({"Lisbon.</think>Lisbon, the capital."}) == "Lisbon.", "a stray close ends the answer: the repeat is dropped");
    check(clear({"Lisbon.</thi", "nk> and again"}) == "Lisbon.", "a stray close cut between pieces");
    each.clear();
    check(clear({"x <", "think>y</think>z"}, &each) == "x z" && each.size() == 2 && each[0] == "x ",
          "a tail that may start a marker is kept back until it is known");
    check(clear({"<think>never closed"}) == "", "an unclosed block says nothing");
    check(clear({"Done <"}) == "Done <", "at the end a kept-back tail that was no marker goes out");
    check(clear({"a <b", "old> c"}) == "a <bold> c", "a tag that is not a marker passes");
    {
        llm::ThinkFilter f;
        std::string out;
        auto emit = [&](const std::string &t) { out += t; };
        f.feed("one.</think>", false, emit);
        f.reset();
        f.feed("<think>x</think>two.", true, emit);
        check(out == "one.two.", "reset starts afresh after an answer that ended");
    }

    printf("llm stream: %d checks, %d failed\n", g_passed + g_failed, g_failed);
    return g_failed ? 1 : 0;
}
