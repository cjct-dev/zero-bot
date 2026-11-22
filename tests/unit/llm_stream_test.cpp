// Unit tests for agent/llm_stream.h: the event stream split into lines across network chunks, the
// answer cleared of thinking markers however the pieces fall, and tool calls put together from their
// pieces (an index out of range refused). Header-only and pure: nothing linked.
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

    // Tool calls, piece by piece: each piece goes to the call its index names, and an index outside 0 to 15 is a
    // fault in the stream, reported as an error, with nothing added (a negative one once wrapped round to a slot
    // far past the end; a huge one would have asked for gigabytes).
    {
        std::vector<llm::ToolCall> calls;
        std::string err;
        bool ok = llm::add_tool_delta(calls, json::parse(R"({"index":0,"id":"a","function":{"name":"timer","arguments":"{\"min"}})"), err);
        ok = ok && llm::add_tool_delta(calls, json::parse(R"({"index":0,"function":{"arguments":"utes\":5}"}})"), err);
        ok = ok && llm::add_tool_delta(calls, json::parse(R"({"index":1,"id":"b","function":{"name":"date_info"}})"), err);
        check(ok && err.empty() && calls.size() == 2 && calls[0].id == "a" && calls[0].name == "timer" && calls[0].args == "{\"minutes\":5}" &&
                  calls[1].id == "b" && calls[1].name == "date_info",
              "a call's pieces are joined by index; a second call gets its own slot");
        calls.clear();
        check(llm::add_tool_delta(calls, json::parse(R"({"id":"c","function":{"name":"timer"}})"), err) && calls.size() == 1 &&
                  calls[0].id == "c",
              "a piece without an index is the first call's");
        check(llm::add_tool_delta(calls, json::parse(R"({"index":15,"id":"z"})"), err) && calls.size() == 16 && calls[15].id == "z",
              "index 15 is the last taken");
        for (const char *bad : {R"({"index":-1,"id":"x"})", R"({"index":16})", R"({"index":4294967295})", R"({"index":18446744073709551615})",
                                R"({"index":1e300})", R"({"index":1.5})", R"({"index":"1"})", R"({"index":[0]})"}) {
            std::vector<llm::ToolCall> some(1);
            std::string why;
            const bool took = llm::add_tool_delta(some, json::parse(bad), why);
            check(!took && some.size() == 1 && some[0].id.empty() && why.find("could not read") != std::string::npos,
                  std::string("an index out of range or not a whole number is an error, nothing added: ") + bad + " -> " + why);
        }
    }

    printf("llm stream: %d checks, %d failed\n", g_passed + g_failed, g_failed);
    return g_failed ? 1 : 0;
}
