#!/usr/bin/env python3
"""A local peer for tests/unit/http_test.cpp. Standard library only, plus the openssl command
to make two throwaway self-signed certificates. Everything binds 127.0.0.1 on ports the system
picks, and stops when the test program has finished.

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
        elif self.path == "/hello":
            self.reply(200, b"tls ok")
        elif self.path == "/who":
            self.reply(200, b"client ok")
        else:
            self.reply(404)


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
        servers = [plain, tls, tls_client]
        for s in servers:
            serve(s)
        args = [sys.argv[1], str(plain.server_address[1]), str(tls.server_address[1]), str(tls_client.server_address[1]),
                pin_of(cert), client_cert, client_key]
        rc = subprocess.run(args, timeout=60).returncode
    finally:
        for s in servers:
            s.shutdown()
            s.server_close()
        shutil.rmtree(folder, ignore_errors=True)
    sys.exit(rc)


if __name__ == "__main__":
    main()
