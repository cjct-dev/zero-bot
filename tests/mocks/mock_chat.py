#!/usr/bin/env python3
"""A scripted stand-in for the chat-completions service the app talks to, for tests of the
request loop.

It binds 127.0.0.1 on a port the system picks, prints "CHAT listening PORT", and exits when its
standard input closes. Every request body is appended to LOG (one JSON object per line). The
reply to each request follows SCRIPT, a JSON file:

  {"user":  [["words in the user's message", {"tool": "name", "args": {...}} | {"text": "..."}], ...],
   "after": [["start of a tool result", {"text": "..."} | {"tool": ...}], ...],
   "default": {"text": "OK."}}

The newest message decides: a user message is matched by the first "user" rule whose words it
contains (case does not matter), a tool result by the first "after" rule it starts with. A rule's
value may also be a list, used one entry per match in turn (the last one repeats).

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
    """The user's words without the time stamp the app puts in front."""
    text = message.get("content") or ""
    return text.split("] ", 1)[1] if text.startswith("[") and "] " in text else text


def pick(kind, key, value):
    if not isinstance(value, list):
        return value
    n = USED.get((kind, key), 0)
    USED[(kind, key)] = n + 1
    return value[min(n, len(value) - 1)]


def decide(messages):
    last = messages[-1] if messages else {}
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
    if "tool" in reply:
        CALLS[0] += 1
        call = {"index": 0, "id": "call_%d" % CALLS[0], "type": "function",
                "function": {"name": reply["tool"], "arguments": json.dumps(reply.get("args", {}), ensure_ascii=False)}}
        return [{"choices": [{"index": 0, "delta": {"role": "assistant", "tool_calls": [call]}}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}]
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
    print("CHAT listening %d" % server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
