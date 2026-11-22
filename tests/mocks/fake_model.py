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
  "usage": true|false      the request asks for token counts ("stream_options": {"include_usage": true},
                           which the app sends only for the experiments' trace, ZB_EXP_TRACE)

REPLY:

  {"text": "..."}                       one piece of text
  {"chunks": ["..", ".."]}              text in several pieces (each its own event)
  {"calls": [{"name": "..", "args": {..} or "raw text"}], "text": ".."}   tool calls (text optional); a call
                                        with "args_chunks": ["..", ".."] sends its arguments in those pieces
  {"status": 500, "body": "..."}        an HTTP error instead of a stream
  {"stream_error": "message"}           an error event inside a 200 stream
  "delay_ms": N                         wait this long before answering (any reply)
  "chunk_ms": N                         wait this long before each piece of text after the first, so the app
                                        has the first piece well before the next is sent
  "no_ids": true                        tool calls without ids (some services omit them)
  "pad_depth": N                        every event of the stream gets "pad": [[...]] nested N levels as its last
                                        member, so the event nests N + 1 deep (the app reads no more than 32)

A request that asks for token counts gets them in a last event with no choices, as the services do:
prompt_tokens about a quarter of the body's bytes, completion_tokens a quarter of the reply's, and
prompt_tokens_details.cached_tokens from the script's top-level "cached_tokens" (default 0).

POST .../audio/speech (the speech service) answers 200 with 0.1 s of silent 24 kHz PCM, logged
with "rule": "speech".
POST .../audio/transcriptions (speech to text) answers {"text": ...} with the script's top-level "heard"
(a string, or a list used one per recording in turn, the last repeating; default "hello"), logged with
"rule": "transcription" and the size of what was sent in place of its body. The script's top-level
"heard_pad_depth": N adds a "pad" nested N levels to that answer, as "pad_depth" does to a stream's events.
The script's top-level "speech_error" and "transcription_error" ({"status": N, "body": "..."}, with
"delay_ms" if wanted) make every request to that service fail so instead; each is still logged.

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
HEARD = [0]      # recordings transcribed


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
    if "usage" in when and bool((body.get("stream_options") or {}).get("include_usage")) != when["usage"]:
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


def json_text(value, pad):
    """An object's JSON text; with `pad`, "pad" nested that many levels is added as its last member."""
    text = json.dumps(value, ensure_ascii=False)
    return text[:-1] + ',"pad":' + "[" * pad + "]" * pad + "}" if pad else text


def events(reply, body):
    out = []
    pieces = reply.get("chunks") or ([reply["text"]] if reply.get("text") else [])
    for piece in pieces:
        out.append({"choices": [{"index": 0, "delta": {"role": "assistant", "content": piece}}]})
    calls = reply.get("calls") or []
    for k, call in enumerate(calls):
        COUNT[1] += 1
        args = call.get("args", {})
        pieces_of_args = call.get("args_chunks") or [args if isinstance(args, str) else json.dumps(args, ensure_ascii=False)]
        tc = {"index": k, "type": "function", "function": {"name": call["name"], "arguments": pieces_of_args[0]}}
        if not reply.get("no_ids"):
            tc["id"] = "call_%d" % COUNT[1]
        out.append({"choices": [{"index": 0, "delta": {"tool_calls": [tc]}}]})
        for more in pieces_of_args[1:]:
            out.append({"choices": [{"index": 0, "delta": {"tool_calls": [{"index": k, "function": {"arguments": more}}]}}]})
    if reply.get("stream_error"):
        out.append({"error": {"message": reply["stream_error"]}})
    out.append({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls" if calls else "stop"}]})
    if (body.get("stream_options") or {}).get("include_usage"):
        sent = len(json.dumps(body, ensure_ascii=False).encode()) // 4
        made = len("".join(pieces).encode()) // 4 + 20 * len(calls) + 1
        out.append({"choices": [], "usage": {"prompt_tokens": sent, "completion_tokens": made, "total_tokens": sent + made,
                                             "prompt_tokens_details": {"cached_tokens": SCRIPT.get("cached_tokens", 0)}}})
    return out


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_whole(self, status, content_type, raw):
        """Answers with `raw` in one piece, and closes the connection."""
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)
        self.close_connection = True

    def speech_service(self, failure, content_type, raw):
        """Answers a request to the speech or transcription service: with the script's `failure` ("speech_error",
        "transcription_error") when it has one, else with `raw`."""
        fail = SCRIPT.get(failure)
        if not fail:
            self.send_whole(200, content_type, raw)
            return
        if fail.get("delay_ms"):
            time.sleep(fail["delay_ms"] / 1000.0)
        self.send_whole(fail["status"], "application/json", fail.get("body", "").encode())

    def do_POST(self):
        raw_body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        if self.path.endswith("/audio/transcriptions"):   # multipart: the recording is not read
            heard = SCRIPT.get("heard", "hello")
            with LOCK:
                COUNT[0] += 1
                if isinstance(heard, list):
                    heard = heard[min(HEARD[0], len(heard) - 1)]
                HEARD[0] += 1
                with open(LOG, "a") as f:
                    f.write(json.dumps({"n": COUNT[0], "rule": "transcription", "body": {"bytes": len(raw_body)}}) + "\n")
            self.speech_service("transcription_error", "application/json",
                                json_text({"text": heard}, SCRIPT.get("heard_pad_depth")).encode())
            return
        body = json.loads(raw_body or b"{}")
        if self.path.endswith("/audio/speech"):
            with LOCK:
                COUNT[0] += 1
                with open(LOG, "a") as f:
                    f.write(json.dumps({"n": COUNT[0], "rule": "speech", "body": body}, ensure_ascii=False) + "\n")
            self.speech_service("speech_error", "audio/pcm", b"\0" * 4800)
            return
        with LOCK:
            COUNT[0] += 1
            rule, reply = decide(body)
            with open(LOG, "a") as f:
                f.write(json.dumps({"n": COUNT[0], "rule": rule, "body": body}, ensure_ascii=False) + "\n")
        if reply.get("delay_ms"):
            time.sleep(reply["delay_ms"] / 1000.0)
        parts = []   # the stream's events, each with whether it carries text
        if reply.get("status"):
            raw = reply.get("body", "").encode()
            self.send_response(reply["status"])
            self.send_header("Content-Type", reply.get("content_type", "application/json"))
        else:
            parts = [(("data: %s\n\n" % json_text(e, reply.get("pad_depth"))).encode(),
                      bool(e.get("choices") and e["choices"][0].get("delta", {}).get("content"))) for e in events(reply, body)]
            parts.append((b"data: [DONE]\n\n", False))
            raw = b"".join(p for p, _ in parts)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        if reply.get("chunk_ms") and parts:
            texts = 0
            for piece, text in parts:
                if text:
                    if texts:
                        time.sleep(reply["chunk_ms"] / 1000.0)
                    texts += 1
                self.wfile.write(piece)
                self.wfile.flush()
        else:
            self.wfile.write(raw)
        self.close_connection = True


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=lambda: (sys.stdin.read(), os._exit(0)), daemon=True).start()
    print("MODEL listening %d" % server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
