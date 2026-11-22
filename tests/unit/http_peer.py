#!/usr/bin/env python3
"""A local peer for tests/unit/http_test.cpp. Standard library only, plus the openssl command
to make two throwaway self-signed certificates. Everything binds 127.0.0.1 (and one listener
127.0.0.2, "another host") on ports the system picks, and stops when the test program has
finished. What the other host and the pretend proxy received is reported at /seen.

usage: http_peer.py path/to/http_test
"""
import base64
import hashlib
import os
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

USER, PASSWORD, REALM, NONCE = "user", "pass word", "device", "dcd98b7102dd2f0e8b11d0f600bfb0c093"
SEEN = {"away": [], "proxy": []}   # requests that reached the other host, and the pretend proxy
AWAY = {"port": 0}
# For the retry checks: how many requests reached each /flaky, /status, /hangup and /slowcount
# address, by its key; /tries/KEY reports the count.
TRIES = {}
TRIES_LOCK = threading.Lock()


def count(key):
    with TRIES_LOCK:
        TRIES[key] = TRIES.get(key, 0) + 1
        return TRIES[key]


def md5(text):
    return hashlib.md5(text.encode()).hexdigest()


def digest_ok(header, method):
    """RFC 7616 digest with MD5 and qop=auth, as curl sends it."""
    if not header or not header.startswith("Digest "):
        return False
    fields = dict((k, v1 or v2) for k, v1, v2 in re.findall(r'(\w+)=(?:"([^"]*)"|([^,\s]*))', header[7:]))
    if fields.get("username") != USER or fields.get("realm") != REALM or fields.get("nonce") != NONCE:
        return False
    ha1 = md5("%s:%s:%s" % (USER, REALM, PASSWORD))
    ha2 = md5("%s:%s" % (method, fields.get("uri", "")))
    if fields.get("qop") == "auth":
        want = md5("%s:%s:%s:%s:auth:%s" % (ha1, NONCE, fields.get("nc", ""), fields.get("cnonce", ""), ha2))
    else:
        want = md5("%s:%s:%s" % (ha1, NONCE, ha2))
    return fields.get("response") == want


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", headers=None):
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def cut(self, status, headers):
        """An answer whose body stops a tenth of the way through, with the connection closed. It is sent as HTTP/1.1
        and says nothing of closing: curl then reads on for the rest (told the connection closes, it leaves a
        challenge's body unread and sends the login on a new connection)."""
        self.protocol_version = "HTTP/1.1"
        self.send_response(status)
        for k, v in headers.items():
            self.send_header(k, v)
        self.send_header("Content-Length", "100")
        self.end_headers()
        self.wfile.write(b"x" * 10)
        self.close_connection = True

    def do_GET(self):
        if self.path == "/redirect":
            self.reply(302, b"moved", {"Location": "/target"})
        elif self.path == "/target":
            self.reply(200, b"target")
        elif self.path == "/slow":
            time.sleep(2)
            self.reply(200, b"late")
        elif self.path == "/digest":
            if digest_ok(self.headers.get("Authorization"), "GET"):
                self.reply(200, b"digest ok")
            else:
                self.reply(401, b"who are you", {"WWW-Authenticate": 'Digest realm="%s", qop="auth", nonce="%s", algorithm=MD5' % (REALM, NONCE)})
        elif self.path in ("/digest-cut", "/challenge-cut"):   # a 401 cut short: to the login, or the challenge itself
            challenge = {"WWW-Authenticate": 'Digest realm="%s", qop="auth", nonce="%s", algorithm=MD5' % (REALM, NONCE)}
            login = (self.headers.get("Authorization") or "").startswith("Digest ")
            if not login and self.path == "/digest-cut":
                self.reply(401, b"who are you", challenge)
            elif not login:
                self.cut(401, challenge)
            elif self.path == "/digest-cut":
                self.cut(401, challenge)
            else:
                self.reply(200, b"the login was sent after all")
        elif self.path == "/hello":
            self.reply(200, b"tls ok")
        elif self.path == "/who":
            self.reply(200, b"client ok")
        elif self.path == "/redirect-away":
            self.reply(307, b"moved", {"Location": "http://127.0.0.2:%d/stolen" % AWAY["port"]})
        elif self.path == "/seen":
            body = ("away=%d proxy=%d" % (len(SEEN["away"]), len(SEEN["proxy"]))).encode()
            self.reply(200, body)
        elif self.path.startswith("/flaky/"):            # /flaky/KEY/N: 503 for the first N requests, then 200
            _, _, key, n = self.path.split("/")
            if count(key) <= int(n):
                self.reply(503, b"<html>busy</html>")
            else:
                self.reply(200, b"fine")
        elif self.path.startswith("/status/"):           # /status/CODE/KEY: always CODE
            _, _, code, key = self.path.split("/")
            count(key)
            self.reply(int(code), b"status")
        elif self.path.startswith("/hangup/"):           # /hangup/KEY: the connection closes with no answer
            count(self.path.split("/")[2])
            self.close_connection = True
            self.connection.shutdown(2)
        elif self.path.startswith("/slowcount/"):        # /slowcount/KEY: answers after 2 s, by when the client has gone
            count(self.path.split("/")[2])
            time.sleep(2)
            try:
                self.reply(200, b"late")
            except OSError:
                pass
        elif self.path.startswith("/tries/"):
            with TRIES_LOCK:
                self.reply(200, str(TRIES.get(self.path.split("/")[2], 0)).encode())
        else:
            self.reply(404)


class Away(Handler):
    """Another host: whatever reaches it is counted, with the headers it carried."""
    def do_GET(self):
        SEEN["away"].append(dict(self.headers))
        self.reply(200, b"stolen")


class Proxy(Handler):
    """A pretend proxy: counts every request and refuses it."""
    def do_GET(self):
        SEEN["proxy"].append(self.requestline)
        self.reply(502, b"proxy")

    def do_CONNECT(self):
        SEEN["proxy"].append(self.requestline)
        self.reply(502, b"proxy")


def self_signed(folder, name):
    key, cert = os.path.join(folder, name + "-key.pem"), os.path.join(folder, name + "-cert.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                    "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=" + name],
                   check=True, capture_output=True)
    return cert, key


def pin_of(cert):
    """curl's pinned-key form: sha256// and the base64 SHA-256 of the certificate's public key (SPKI DER)."""
    pem = subprocess.run(["openssl", "x509", "-in", cert, "-pubkey", "-noout"], check=True, capture_output=True, text=True).stdout
    der = base64.b64decode("".join(l for l in pem.splitlines() if l and not l.startswith("-----")))
    return "sha256//" + base64.b64encode(hashlib.sha256(der).digest()).decode()


def serve(server):
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return t


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    folder = tempfile.mkdtemp(prefix="zb-http-peer-")
    servers = []
    try:
        cert, key = self_signed(folder, "device")
        client_cert, client_key = self_signed(folder, "client")
        plain = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        tls = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        tls.socket = ctx.wrap_socket(tls.socket, server_side=True)
        tls_client = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cctx.load_cert_chain(cert, key)
        cctx.verify_mode = ssl.CERT_REQUIRED
        cctx.load_verify_locations(client_cert)
        tls_client.socket = cctx.wrap_socket(tls_client.socket, server_side=True)
        away = ThreadingHTTPServer(("127.0.0.2", 0), Away)
        AWAY["port"] = away.server_address[1]
        proxy = ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
        servers = [plain, tls, tls_client, away, proxy]
        for s in servers:
            serve(s)
        args = [sys.argv[1], str(plain.server_address[1]), str(tls.server_address[1]), str(tls_client.server_address[1]),
                pin_of(cert), client_cert, client_key, str(proxy.server_address[1])]
        rc = subprocess.run(args, timeout=60).returncode
    finally:
        for s in servers:
            s.shutdown()
            s.server_close()
        shutil.rmtree(folder, ignore_errors=True)
    sys.exit(rc)


if __name__ == "__main__":
    main()
