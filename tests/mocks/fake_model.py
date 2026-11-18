#!/usr/bin/env python3
"""A scripted stand-in for the chat-completions service, for tests of the request loop itself
(tests/agent_loop_test.py). Richer than mock_chat.py: rules match on more than the newest
message, a reply can be split into pieces, carry several tool calls, fail with an HTTP status,
or arrive late, and every request is logged with the rule that answered it.

It binds 127.0.0.1 on a port the system picks, prints "MODEL listening PORT", and exits when its
standard input closes. Standard library only.

usage: fake_model.py SCRIPT LOG

SCRIPT is a JSON file:

  {"rules": [{"when": {CONDITIONS}, "reply": REPLY or [REPLY, ...], "times": N}, ...],
   "default": REPLY}

The first rule whose conditions all hold answers. A list of replies is used one per match in
turn (the last one repeats); "times" retires a rule after that many uses. CONDITIONS (all
optional):

  "said": "words"          the newest user message contains these words (any case; the time stamp
                           the app puts in front is ignored, notes it appends are included)
  "last": "user"|"tool"|"assistant"|"system"   the role of the newest message
  "result": "prefix"       the newest message is a tool result that starts with this
  "results": N             exactly N tool results follow the newest user message
  "tools": true|false      the request offers tools (the loop's last round offers none)
  "effort": true|false     the request carries "reasoning_effort"

REPLY:

  {"text": "..."}                       one piece of text
  {"chunks": ["..", ".."]}              text in several pieces (each its own event)
  {"calls": [{"name": "..", "args": {..} or "raw text"}], "text": ".."}   tool calls (text optional)
  {"status": 500, "body": "..."}        an HTTP error instead of a stream
  {"stream_error": "message"}           an error event inside a 200 stream
  "delay_ms": N                         wait this long before answering (any reply)
  "no_ids": true                        tool calls without ids (some services omit them)

LOG gets one JSON object per request: {"n": 1, "rule": index or "default", "body": {...}}.
"""
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SCRIPT = json.load(open(sys.argv[1]))
LOG = sys.argv[2]
LOCK = threading.Lock()
USED = {}
COUNT = [0, 0]   # requests, tool calls


def text_of(message):
    content = message.get("content") or ""
    if isinstance(content, list):
        content = " ".join(p.get("text", "") for p in content if isinstance(p, dict) and p.get("type") == "text")
    return content


def spoken(message):
    """A user message without the "[Wed 18 Nov 2026, 10:00] " stamp the app puts in front."""
    text = text_of(message)
    return text.split("] ", 1)[1] if text.startswith("[") and "] " in text else text


def matches(when, body):
    messages = body.get("messages", [])
    last = messages[-1] if messages else {}
    users = [i for i, m in enumerate(messages) if m.get("role") == "user"]
    newest_user = messages[users[-1]] if users else {}
    if "said" in when and when["said"].lower() not in spoken(newest_user).lower():
        return False
    if "last" in when and last.get("role") != when["last"]:
        return False
    if "result" in when and not (last.get("role") == "tool" and text_of(last).startswith(when["result"])):
        return False
    if "results" in when:
        after = messages[users[-1] + 1:] if users else messages
        if sum(1 for m in after if m.get("role") == "tool") != when["results"]:
            return False
    if "tools" in when and bool(body.get("tools")) != when["tools"]:
        return False
    if "effort" in when and ("reasoning_effort" in body) != when["effort"]:
        return False
    return True


def decide(body):
    for i, rule in enumerate(SCRIPT.get("rules", [])):
        used = USED.get(i, 0)
        if "times" in rule and used >= rule["times"]:
            continue
        if not matches(rule.get("when", {}), body):
            continue
        USED[i] = used + 1
        reply = rule["reply"]
        if isinstance(reply, list):
            reply = reply[min(used, len(reply) - 1)]
        return i, reply
    return "default", SCRIPT.get("default", {"text": "OK."})


def events(reply):
    out = []
    pieces = reply.get("chunks") or ([reply["text"]] if reply.get("text") else [])
    for piece in pieces:
        out.append({"choices": [{"index": 0, "delta": {"role": "assistant", "content": piece}}]})
    calls = reply.get("calls") or []
    for k, call in enumerate(calls):
        COUNT[1] += 1
        args = call.get("args", {})
        tc = {"index": k, "type": "function",
              "function": {"name": call["name"], "arguments": args if isinstance(args, str) else json.dumps(args, ensure_ascii=False)}}
        if not reply.get("no_ids"):
            tc["id"] = "call_%d" % COUNT[1]
        out.append({"choices": [{"index": 0, "delta": {"tool_calls": [tc]}}]})
    if reply.get("stream_error"):
        out.append({"error": {"message": reply["stream_error"]}})
    out.append({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls" if calls else "stop"}]})
    return out


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        with LOCK:
            COUNT[0] += 1
            rule, reply = decide(body)
            with open(LOG, "a") as f:
                f.write(json.dumps({"n": COUNT[0], "rule": rule, "body": body}, ensure_ascii=False) + "\n")
        if reply.get("delay_ms"):
            time.sleep(reply["delay_ms"] / 1000.0)
        if reply.get("status"):
            raw = reply.get("body", "").encode()
            self.send_response(reply["status"])
            self.send_header("Content-Type", reply.get("content_type", "application/json"))
        else:
            raw = ("".join("data: %s\n\n" % json.dumps(e, ensure_ascii=False) for e in events(reply)) + "data: [DONE]\n\n").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)
        self.close_connection = True


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=lambda: (sys.stdin.read(), os._exit(0)), daemon=True).start()
    print("MODEL listening %d" % server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
