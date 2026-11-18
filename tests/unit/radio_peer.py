#!/usr/bin/env python3
"""A pretend radio server for tests/unit/radio_test.cpp. Standard library only. It listens on
127.0.0.1 on a port the system picks, runs the test program with its address and a port that
nothing listens on, and stops when the program has finished.

Answers, by path:
  /stream.mp3   an endless MP3-like stream (audio/mpeg), as an Icecast server sends it
  /trickle.aac  a stream that sends 200 bytes and then nothing for a long while
  /live.m3u8    an HLS media playlist
  /list.pls     a PLS playlist naming /stream.mp3;  /list.m3u  a plain M3U naming /stream.mp3
  /loop.pls     a PLS playlist naming itself
  /nothing.pls  a PLS playlist that names no stream
  /mixed.pls / /mixed.m3u  playlists whose first entries are a local file, an option for the
                player and other ffmpeg protocols, and only then /stream.mp3
  /geo 403, /gone 404, /fault 500, /busy 429
  /page         a web page (text/html)
  /empty        audio/mpeg with no body at all
  /silent       takes the request and answers nothing at all for a long while

usage: radio_peer.py path/to/radio_test
"""
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = {"port": 0}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        pass

    def send(self, code, ctype, body=b""):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        base = "http://127.0.0.1:%d" % PORT["port"]
        path = self.path.split("?")[0]
        try:
            if path == "/stream.mp3":
                self.send_response(200)
                self.send_header("Content-Type", "audio/mpeg")
                self.send_header("icy-br", "128")
                self.end_headers()
                frame = b"\xff\xfb\x90\x64" + b"\x00" * 413
                for _ in range(2000):          # about 800 KB, or until the client hangs up
                    self.wfile.write(frame * 4)
                    time.sleep(0.002)
            elif path == "/trickle.aac":
                self.send_response(200)
                self.send_header("Content-Type", "audio/aac")
                self.end_headers()
                self.wfile.write(b"\xff\xf1" + b"\x00" * 198)
                self.wfile.flush()
                time.sleep(20)
            elif path == "/live.m3u8":
                self.send(200, "application/vnd.apple.mpegurl",
                          b"#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:7\n#EXTINF:6.4,\nseg7.ts\n")
            elif path == "/list.pls":
                self.send(200, "audio/x-scpls", ("[playlist]\nNumberOfEntries=1\nFile1=%s/stream.mp3\nTitle1=Test\n" % base).encode())
            elif path == "/list.m3u":
                self.send(200, "audio/x-mpegurl", ("#EXTM3U\n#EXTINF:-1,Test\n%s/stream.mp3\n" % base).encode())
            elif path == "/loop.pls":
                self.send(200, "audio/x-scpls", ("[playlist]\nFile1=%s/loop.pls\n" % base).encode())
            elif path in ("/mixed.pls", "/mixed.m3u"):
                hostile = ["file:///etc/passwd", "-i http://127.0.0.1:9/x.mp3", "concat:%s/a.mp3|%s/b.mp3" % (base, base),
                           "subfile,,start,0,end,99,,:%s/stream.mp3" % base, "data:audio/mpeg;base64,AAAA",
                           "/stream.mp3", "%s/a b.mp3" % base, "%s/stream.mp3" % base]
                if path.endswith(".pls"):
                    body = "[playlist]\n" + "".join("File%d=%s\n" % (i + 1, u) for i, u in enumerate(hostile))
                    self.send(200, "audio/x-scpls", body.encode())
                else:
                    self.send(200, "audio/x-mpegurl", ("#EXTM3U\n" + "\n".join(hostile) + "\n").encode())
            elif path == "/nothing.pls":
                self.send(200, "audio/x-scpls", b"[playlist]\nNumberOfEntries=0\n")
            elif path == "/geo":
                self.send(403, "text/plain", b"not in your country")
            elif path == "/gone":
                self.send(404, "text/plain", b"no such mount")
            elif path == "/fault":
                self.send(500, "text/html", b"<html>oops</html>")
            elif path == "/busy":
                self.send(429, "text/plain", b"slow down")
            elif path == "/page":
                self.send(200, "text/html; charset=utf-8", b"<!DOCTYPE html><html><body>Listen live!</body></html>")
            elif path == "/empty":
                self.send(200, "audio/mpeg", b"")
            elif path == "/silent":
                time.sleep(20)
            else:
                self.send(404, "text/plain", b"unknown")
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    PORT["port"] = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    # A port nothing listens on: bound and closed again at once.
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    closed = s.getsockname()[1]
    s.close()
    code = subprocess.call([sys.argv[1], "http://127.0.0.1:%d" % PORT["port"], "http://127.0.0.1:%d" % closed])
    server.shutdown()
    sys.exit(code)


if __name__ == "__main__":
    main()
