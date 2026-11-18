#!/usr/bin/env python3
"""A scripted stand-in for the chat-completions service the app talks to, for tests of the
request loop.

It binds 127.0.0.1 on a port the system picks, prints "CHAT listening PORT", and exits when its
standard input closes. Every request body is appended to LOG (one JSON object per line). The
reply to each request follows SCRIPT, a JSON file:

  {"user":  [["words in the user's message", {"tool": "name", "args": {...}} | {"text": "..."}], ...],
   "after": [["start of a tool result", {"text": "..."} | {"tool": ...}], ...],
   "note":  [["words in one of the app's automatic notes", {"text": "..."} | {"tool": ...}], ...],
   "default": {"text": "OK."}}

The newest message decides: an automatic note from the app (a system message, or a user message
carrying "(Automatic note") by the first "note" rule whose words it contains, then a user message
by the first "user" rule whose words it contains (case does not matter), a tool result by the
first "after" rule it starts with. A rule's value may also be a list, used one entry per match in
turn (the last one repeats). A question about a photo (the look tool) arrives as a user message
whose content is a list of parts; its text parts are what the rules see.

Besides text and a tool call (whose arguments may be given as "raw" text, to send a malformed
call; {"tools": [{"tool": ..., "args": ...}, ...]} makes several calls in one reply), a reply may be {"status": 503}: that HTTP status with a JSON error, or {"sse_error": "..."}:
an error inside the stream, as a service that is overloaded sends one.

usage: mock_chat.py SCRIPT LOG
"""
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SCRIPT = json.load(open(sys.argv[1]))
LOG = sys.argv[2]
LOCK = threading.Lock()
USED = {}   # rule key -> times used
CALLS = [0]


def said(message):
    """The user's words without the time stamp the app puts in front; for a photo, its question."""
    text = message.get("content") or ""
    if isinstance(text, list):
        text = " ".join(p.get("text", "") for p in text if isinstance(p, dict) and p.get("type") == "text")
    return text.split("] ", 1)[1] if text.startswith("[") and "] " in text else text


def pick(kind, key, value):
    if not isinstance(value, list):
        return value
    n = USED.get((kind, key), 0)
    USED[(kind, key)] = n + 1
    return value[min(n, len(value) - 1)]


def decide(messages):
    last = messages[-1] if messages else {}
    text = said(last) if last.get("role") in ("system", "user") else ""
    if last.get("role") == "system" or "(Automatic note" in text:
        for words, value in SCRIPT.get("note", []):
            if words.lower() in text.lower():
                return pick("note", words, value)
    if last.get("role") == "tool":
        result = last.get("content") or ""
        for prefix, value in SCRIPT.get("after", []):
            if result.startswith(prefix):
                return pick("after", prefix, value)
    elif last.get("role") == "user":
        text = said(last).lower()
        for words, value in SCRIPT.get("user", []):
            if words.lower() in text:
                return pick("user", words, value)
    return SCRIPT.get("default", {"text": "OK."})


def events(reply):
    if "tool" in reply or "tools" in reply:
        calls = []
        for i, one in enumerate(reply.get("tools") or [reply]):
            CALLS[0] += 1
            args = one["raw"] if "raw" in one else json.dumps(one.get("args", {}), ensure_ascii=False)
            calls.append({"index": i, "id": "call_%d" % CALLS[0], "type": "function",
                          "function": {"name": one["tool"], "arguments": args}})
        return [{"choices": [{"index": 0, "delta": {"role": "assistant", "tool_calls": calls}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}]
    if "sse_error" in reply:
        return [{"error": {"message": reply["sse_error"]}}]
    return [{"choices": [{"index": 0, "delta": {"role": "assistant", "content": reply.get("text", "")}}]},
            {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        with LOCK:
            with open(LOG, "a") as f:
                f.write(json.dumps(body, ensure_ascii=False) + "\n")
            reply = decide(body.get("messages", []))
            if "status" in reply:
                raw = json.dumps({"error": {"message": "scripted failure %d" % reply["status"]}}).encode()
            else:
                raw = ("".join("data: %s\n\n" % json.dumps(e, ensure_ascii=False) for e in events(reply)) + "data: [DONE]\n\n").encode()
        self.send_response(reply.get("status", 200))
        self.send_header("Content-Type", "application/json" if "status" in reply else "text/event-stream")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)
        self.close_connection = True


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=lambda: (sys.stdin.read(), os._exit(0)), daemon=True).start()
    print("CHAT listening %d" % server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
