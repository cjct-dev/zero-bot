#!/usr/bin/env python3
"""A pretend Fire TV on 127.0.0.1, for testing the driver. Python 3 standard library only.

usage: mock_firetv.py [--state STATE] [--caps VARIANT] [--cert a|b] [--mac MAC]
                      [--wake-delay S] [--boot-delay S] [--max-seconds S]
       mock_firetv.py --selftest

It listens on ports the system picks (port 0) and prints them on its first line:
    MOCK listening api=<port> dial=<port> wol=<port> control=<port> pin=<sha256//...>
  api      HTTPS, self-signed certificate (test_cert_a.pem or _b.pem): the remote-app API
  dial     HTTP: the DIAL wake, POST /apps/FireTVRemote
  wol      UDP: Wake-on-LAN magic packets
  control  HTTP, for the test: GET /stats, POST /reset {config}, POST /set {config}
  pin      the key pin of the certificate in use, computed here from the certificate's DER with
           hashlib (independently of the C++ code, which uses libcrypto)

It exits when its standard input is closed (the test's end), after --max-seconds, or when killed.

States (what a real TV does, from firehose-remote docs/protocol.md section 3):
  awake           both ports answer
  idle            the API port refuses connections, DIAL answers; a DIAL wake opens the API
                  port after --wake-delay seconds (0.9 by default, as in the work package)
  hung            the API port accepts, completes TLS and never answers; DIAL answers, waking
                  does not help
  hung_tls        the API port accepts TCP and never starts TLS
  deep            both ports refuse until a magic packet for --mac arrives; the TV then boots
                  for --boot-delay seconds (7.0 by default: firehose section 3 measured 6.5-7.4 s
                  from the first packet) and answers on both
  deep_blackhole  as deep, but connection attempts are dropped (they time out)
  gone            both ports refuse and nothing wakes it (unplugged)
  gone_blackhole  as gone, but connection attempts time out
  starting        a TV coming on: DIAL answers (201, no new wake), the API port accepts and says
                  nothing (config starting_mode: "silent", TLS completes, the default; or
                  "silent_tls", TLS never starts). A wake from idle or deep passes through it for
                  starting_s seconds before awake when starting_s > 0 (0 by default), as the
                  owner's TV did after a DIAL wake on 2026-11-18 (its API accepted connections
                  without answering for longer than 2.5 s; whether TLS completed is not known).

Config keys for /reset and /set (all optional):
  state, caps ("stick" | "tv" | "tv_power" | "levels_only" | "empty"), cert ("a" | "b"), mac,
  wake_delay, boot_delay, pin ("1234": the code shown; random otherwise), ok_first (how many
  "OK" replies before the token), wrong_reply ("400" | "200text"), verify ("normal" |
  "always_ok" | "unreadable" | "silent"), revoked (every token refused), apps (a JSON list
  served by appsV2), faults ({"GET /v1/FireTV": mode, ...}; modes: garbage, huge, drip, deep,
  notjson, wrongtypes, truncated, silent, close, 500, 400, 404, 405, badutf8, hugeheader; the
  DIAL wake is "POST /apps/FireTVRemote", and a fault there means the TV did not take the wake),
  fault_after ({"POST /v1/FireTV": 2}: that many requests to the endpoint are answered normally
  first), dial_late (seconds: the DIAL wake is taken at once but answered only this much later, as
  the owner's TV did from deep standby), starting_s and starting_mode (see the starting state), late (seconds: every command -
  a key, a media action, an app launch - is carried out at once and answered only after this
  long, like a busy TV), slow_tls_every (N: every Nth TLS handshake on the API port waits
  slow_tls_s seconds, 0.3 by default, before it starts).
  The late and slow_tls behaviours, the DIAL faults and the 404 body are invented, for tests: no
  source records a TV doing them (the 400 body is the forum's, the 405 body firehose's).
/stats counts, besides the requests: tls_connections (API handshakes completed), and
silent_connections (connections the API port accepted in a silent mode: hung, hung_tls, starting).
/reset also clears the counters and the request log and hides any code shown; tokens issued
before stay valid unless "forget_tokens" is true.
"""
import base64
import hashlib
import json
import os
import random
import socket
import ssl
import string
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
API_KEY = "0987654321"                       # the phone app's constant (firehose section 1)
DEFAULT_MAC = "00:00:5e:00:53:01"            # documentation range (RFC 7042)

# Replies as recorded by the sources.
STATUS_BODY = {"osVersion": "0036005356164", "platformType": "android", "turnstileVersion": "0.1.147068"}  # firehose, .22
CAPS = {
    # firehose section 2, measured on a Fire TV Stick (.22), 2026-09-18
    "stick": {"isEpgSupported": True, "isInternalIntentSupported": True, "isPropertiesApiSupported": True,
              "isVolumeControlsSupported": False, "powerCapabilityLevelOfSupport": "NotSupported",
              "ringableRemoteCount": 0, "volumeCapabilityLevelOfSupport": "NotSupported"},
    # firehose section 2, read from the device whose volume and mute worked (.25), 2026-09-20
    "tv": {"isVolumeControlsSupported": True},
    # NOT from any source: a TV that also reports power control. "Supported" is invented here;
    # no source records the spelling a device uses for support.
    "tv_power": {"isVolumeControlsSupported": True, "powerCapabilityLevelOfSupport": "Supported",
                 "volumeCapabilityLevelOfSupport": "Supported"},
    "levels_only": {"volumeCapabilityLevelOfSupport": "Supported"},   # invented, as above
    "empty": {},
}
DEFAULT_APPS = [
    {"appId": "com.netflix.ninja", "name": "Netflix", "tvIconArt": "https://example.invalid/n.png", "isInstalled": True},
    {"appId": "com.amazon.firetv.youtube", "name": "YouTube", "tvIconArt": "", "isInstalled": True},
    {"appId": "com.amazon.cloud9", "name": "Prime Video", "isInstalled": True},
    {"appId": "com.amazon.bueller.music", "name": "Amazon Prime Music", "isInstalled": True},
    {"appId": "uk.co.bbc.iplayer", "name": "BBC iPlayer", "isInstalled": True},
    {"appId": "com.example.removed", "name": "Removed App", "isInstalled": False},
    {"appId": "com.example.tile", "name": "Shortcut Tile", "isShortcutApp": True},
    {"appId": "../../etc/passwd", "name": "Path Trick"},
    {"name": "No Package"},
    7,
    {"appId": "com.example.longname", "name": "A very long application name \u0007 that goes on and on and on"},
]
OK_REPLY = {"description": "OK"}
BAD_ARGS = {"description": "Bad arguments supplied. Please check inputs."}   # forum thread, post 3
KEYED_ACTIONS = {"dpad_up", "dpad_down", "dpad_left", "dpad_right", "select"}
PLAIN_ACTIONS = {"home", "back", "menu", "sleep", "volume_up", "volume_down", "mute", "power", "epg"}


# ------------------------------------------------------------------------------------------
# Small helpers
# ------------------------------------------------------------------------------------------
def der_items(data, start, end):
    """Yields (tag, item_start, content_start, item_end) for the DER items between start and end."""
    i = start
    while i < end:
        item_start = i
        tag = data[i]
        n = data[i + 1]
        i += 2
        if n & 0x80:
            count = n & 0x7F
            n = int.from_bytes(data[i:i + count], "big")
            i += count
        yield tag, item_start, i, i + n
        i += n


def spki_pin(pem_path):
    """sha256//<base64> of the certificate's SubjectPublicKeyInfo, read from the DER by hand."""
    with open(pem_path) as f:
        der = ssl.PEM_cert_to_DER_cert(f.read())
    _, _, cert_body, cert_end = next(der_items(der, 0, len(der)))
    _, _, tbs_body, tbs_end = next(der_items(der, cert_body, cert_end))
    fields = list(der_items(der, tbs_body, tbs_end))
    if fields[0][0] == 0xA0:            # explicit version
        fields = fields[1:]
    # serialNumber, signature, issuer, validity, subject, subjectPublicKeyInfo
    _, spki_start, _, spki_end = fields[5]
    return "sha256//" + base64.b64encode(hashlib.sha256(der[spki_start:spki_end]).digest()).decode()


def parse_magic(data):
    """The address a magic packet wakes, or None when it is not one (6 x FF, then 16 x the MAC)."""
    if len(data) < 102 or data[:6] != b"\xff" * 6:
        return None
    mac = data[6:12]
    if any(data[6 + 6 * i:12 + 6 * i] != mac for i in range(16)):
        return None
    return ":".join("%02x" % b for b in mac)


def jdump(obj):
    return json.dumps(obj, separators=(",", ":")).encode()


# ------------------------------------------------------------------------------------------
# A TCP port whose behaviour can change while keeping its number
# ------------------------------------------------------------------------------------------
class Slot:
    """modes: closed (bound, not listening: refuses), open, silent (accepts, completes TLS, never
    answers), silent_tls (accepts, never starts TLS), blackhole (backlog full: SYNs are dropped)."""

    def __init__(self, name, serve, tls):
        self.name, self.serve, self.tls = name, serve, tls
        self.lock = threading.Lock()
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.mode = "closed"
        self.filler = None
        self.generation = 0

    def set_mode(self, mode):
        with self.lock:
            if mode == self.mode:
                return
            self.generation += 1
            self._close()
            s = socket.socket()
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind(("127.0.0.1", self.port))
            except OSError as e:
                print("MOCK cannot keep port %d (%s): exiting" % (self.port, e), flush=True)
                os._exit(3)
            self.sock, self.mode = s, mode
            if mode == "blackhole":
                s.listen(0)
                self.filler = socket.create_connection(("127.0.0.1", self.port), timeout=2)
            elif mode != "closed":
                s.listen(64)
                threading.Thread(target=self._accept, args=(s, self.generation), daemon=True).start()

    def _close(self):
        # shutdown() first: a thread blocked in accept() keeps a closed listening socket alive
        # (still listening) on Linux; shutdown() wakes it and ends the listening state.
        for s in (self.filler, self.sock):
            if s is not None:
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    s.close()
                except OSError:
                    pass
        self.filler = None

    def _accept(self, s, generation):
        while True:
            try:
                conn, _ = s.accept()
            except OSError:
                return
            if generation != self.generation:
                conn.close()
                return
            threading.Thread(target=self._handle, args=(conn, self.mode), daemon=True).start()

    def _handle(self, conn, mode):
        conn.settimeout(10)
        try:
            if mode == "silent_tls":
                self.serve.note_silent()
                conn.settimeout(60)
                while conn.recv(4096):
                    pass
                return
            if self.tls is not None:
                delay = self.serve.tls_delay()
                if delay:
                    time.sleep(delay)            # a slow handshake (config slow_tls_every)
                conn = self.tls().wrap_socket(conn, server_side=True)
                self.serve.note_tls()
            if mode == "silent":
                self.serve.note_silent()
                conn.settimeout(60)
                while conn.recv(4096):
                    pass
                return
            self.serve.connection(self.name, conn)
        except (OSError, ssl.SSLError, ValueError):
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


# ------------------------------------------------------------------------------------------
# The TV
# ------------------------------------------------------------------------------------------
class Tv:
    def __init__(self, args):
        self.lock = threading.RLock()
        self.timers = []
        self.cfg = {}
        self.tokens = set()
        self.started = time.time()
        self.api = Slot("api", self, self.tls_context)
        self.dial = Slot("dial", self, None)
        self.wol = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.wol.bind(("127.0.0.1", 0))
        threading.Thread(target=self._wol_loop, daemon=True).start()
        self.contexts = {}
        self.reset({"state": args.state, "caps": args.caps, "cert": args.cert, "mac": args.mac,
                    "wake_delay": args.wake_delay, "boot_delay": args.boot_delay})

    # ---- configuration
    def tls_context(self):
        cert = self.cfg.get("cert", "a")
        if cert not in self.contexts:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(os.path.join(HERE, "test_cert_%s.pem" % cert), os.path.join(HERE, "test_key_%s.pem" % cert))
            self.contexts[cert] = ctx
        return self.contexts[cert]

    def pin(self):
        return spki_pin(os.path.join(HERE, "test_cert_%s.pem" % self.cfg.get("cert", "a")))

    def reset(self, cfg):
        with self.lock:
            for t in self.timers:
                t.cancel()
            self.timers = []
            keep = {k: self.cfg[k] for k in ("cert", "mac", "wake_delay", "boot_delay") if k in self.cfg}
            self.cfg = {"state": "awake", "caps": "tv", "cert": "a", "mac": DEFAULT_MAC, "wake_delay": 0.9,
                        "boot_delay": 7.0, "ok_first": 0, "wrong_reply": "400", "verify": "normal",
                        "revoked": False, "faults": {}, "apps": DEFAULT_APPS, "late": 0, "slow_tls_every": 0,
                        "slow_tls_s": 0.3, "starting_s": 0, "starting_mode": "silent"}
            self.cfg.update(keep)
            if cfg.get("forget_tokens"):
                self.tokens = set()
            self.stats = {"requests": [], "tls_connections": 0, "silent_connections": 0, "dial_posts": 0, "dial_gets": 0,
                          "magic": [],
                          "pin_display": 0, "pin_verify": 0, "shown_pin": None, "wakes": 0, "boots": 0,
                          "display": "on", "playing": False, "volume": 20, "muted": False, "launched": [],
                          "keys": [], "ignored": [], "scans": [], "issued": []}
            self.oks_left = 0
            self.pin_shown_at = 0
            self.counts = {}
            self.handshakes = 0
            self.apply_cfg(cfg)

    def apply_cfg(self, cfg):
        with self.lock:
            for k, v in cfg.items():
                if k != "forget_tokens":
                    self.cfg[k] = v
            if "ok_first" in cfg:
                self.oks_left = int(cfg["ok_first"])
            self.set_state(self.cfg["state"])

    def set_state(self, state):
        starting_mode = self.cfg.get("starting_mode", "silent")
        if starting_mode not in ("silent", "silent_tls"):
            raise ValueError("unknown starting_mode " + str(starting_mode))
        modes = {"awake": ("open", "open"), "idle": ("closed", "open"), "hung": ("silent", "open"),
                 "hung_tls": ("silent_tls", "open"), "deep": ("closed", "closed"),
                 "deep_blackhole": ("blackhole", "blackhole"), "gone": ("closed", "closed"),
                 "gone_blackhole": ("blackhole", "blackhole"), "starting": (starting_mode, "open")}
        if state not in modes:
            raise ValueError("unknown state " + state)
        with self.lock:
            self.cfg["state"] = state
            self.stats["state"] = state
            self.api.set_mode(modes[state][0])
            self.dial.set_mode(modes[state][1])

    def later(self, seconds, fn):
        t = threading.Timer(float(seconds), fn)
        t.daemon = True
        self.timers.append(t)
        t.start()

    def note_tls(self):
        with self.lock:
            self.stats["tls_connections"] += 1

    def note_silent(self):
        with self.lock:
            self.stats["silent_connections"] += 1

    def come_on(self):
        """The TV has come on (a DIAL wake from idle, or a boot from deep standby): awake at once,
        or after cfg["starting_s"] seconds in the starting state when that is set."""
        with self.lock:
            starting = float(self.cfg.get("starting_s") or 0)
            if starting <= 0:
                self.set_state("awake")
                return
            self.set_state("starting")

            def started():
                with self.lock:
                    if self.cfg["state"] == "starting":
                        self.set_state("awake")
            self.later(starting, started)

    def tls_delay(self):
        """Seconds to wait before this TLS handshake on the API port (config slow_tls_every)."""
        with self.lock:
            self.handshakes += 1
            every = int(self.cfg.get("slow_tls_every") or 0)
            return float(self.cfg.get("slow_tls_s", 0.3)) if every > 0 and self.handshakes % every == 0 else 0.0

    def done(self, conn, status, payload):
        """The answer to a command that has been carried out: after cfg["late"] seconds when set
        (a busy TV; invented, to show that a command answered late is never sent twice)."""
        late = float(self.cfg.get("late") or 0)
        if late > 0:
            time.sleep(late)
        self.send(conn, status, payload)

    # ---- Wake-on-LAN
    def _wol_loop(self):
        while True:
            try:
                data, _ = self.wol.recvfrom(2048)
            except OSError:
                return
            mac = parse_magic(data)
            with self.lock:
                self.stats["magic"].append({"valid": mac is not None, "mac": mac, "length": len(data),
                                            "t": round(time.time() - self.started, 3)})
                if mac and mac == self.cfg["mac"].lower() and self.cfg["state"] in ("deep", "deep_blackhole") \
                        and not self.stats.get("booting"):
                    self.stats["booting"] = True
                    self.later(self.cfg["boot_delay"], self._boot)

    def _boot(self):
        with self.lock:
            self.stats["booting"] = False
            self.stats["boots"] += 1
            self.stats["display"] = "on"
            self.come_on()

    # ---- HTTP on the api and dial ports
    def connection(self, port_name, conn):
        conn.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(65536)
            if not chunk:
                return
            data += chunk
            if len(data) > 256 * 1024:
                return
        head, _, body = data.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        try:
            method, target, _ = lines[0].split(" ", 2)
        except ValueError:
            return
        headers = {}
        for line in lines[1:]:
            k, _, v = line.partition(":")
            headers[k.strip().lower()] = v.strip()
        length = int(headers.get("content-length", "0") or 0)
        if length > 1024 * 1024:
            return
        while len(body) < length:
            chunk = conn.recv(65536)
            if not chunk:
                break
            body += chunk
        url = urllib.parse.urlsplit(target)
        query = dict(urllib.parse.parse_qsl(url.query))
        if port_name == "dial":
            self.serve_dial(conn, method, url.path)
        else:
            self.serve_api(conn, method, url.path, query, headers, body)

    @staticmethod
    def send(conn, status, payload=None, extra=(), reason=None):
        body = b"" if payload is None else (payload if isinstance(payload, bytes) else jdump(payload))
        reasons = {200: "OK", 201: "Created", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
                   405: "Method Not Allowed", 500: "Internal Server Error"}
        lines = ["HTTP/1.1 %d %s" % (status, reason or reasons.get(status, "Status")),
                 "Content-Type: application/json", "Content-Length: %d" % len(body), "Connection: close"]
        lines += list(extra)
        conn.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + body)

    def serve_dial(self, conn, method, path):
        if path != "/apps/FireTVRemote":
            self.send(conn, 404)
            return
        if method == "GET":    # firehose section 3: DIAL XML with <state>; "stopped" is normal
            with self.lock:
                self.stats["dial_gets"] += 1
            xml = b'<?xml version="1.0" encoding="UTF-8"?><service xmlns="urn:dial-multiscreen-org:schemas:dial">' \
                  b'<name>FireTVRemote</name><options allowStop="false"/><state>stopped</state></service>'
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
                         % len(xml) + xml)
            return
        if method != "POST":
            self.send(conn, 405)
            return
        endpoint = "POST /apps/FireTVRemote"
        with self.lock:
            self.stats["dial_posts"] += 1
            self.counts[endpoint] = count = self.counts.get(endpoint, 0) + 1
            fault = self.cfg["faults"].get(endpoint)
            spared = int(self.cfg.get("fault_after", {}).get(endpoint, 0))
        if fault and count > spared:
            self.fault(conn, fault, endpoint)    # the TV does not take the wake
            return
        with self.lock:
            state = self.cfg["state"]
            if state == "idle" and not self.stats.get("waking"):
                self.stats["waking"] = True
                self.stats["wakes"] += 1

                def wake():
                    with self.lock:
                        self.stats["waking"] = False
                        self.stats["display"] = "on"
                        self.come_on()
                self.later(self.cfg["wake_delay"], wake)
            elif state == "awake":
                # Assumed, not documented: a DIAL wake also lights a screen that action=sleep darkened.
                self.stats["display"] = "on"
        # The owner's TV from deep standby (2026-11-18): it takes the wake at once but answers late.
        late = float(self.cfg.get("dial_late") or 0)
        if late > 0:
            time.sleep(late)
        # 201 with a Location, as captured in the forum thread (post 25).
        conn.sendall(b"HTTP/1.1 201 Created\r\nContent-Type: text/plain\r\nLocation: http://127.0.0.1:%d/apps/FireTVRemote/run"
                     b"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" % self.dial.port)

    def fault(self, conn, mode, endpoint):
        if mode == "garbage":
            conn.sendall(b"\x16\x03\x01 this is not HTTP \x00\xff\r\n\r\n")
        elif mode == "huge":
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1000000000\r\n\r\n")
            chunk = b"[" * 65536
            for _ in range(2000):          # stops when the client hangs up
                conn.sendall(chunk)
        elif mode == "drip":
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1000\r\n\r\n")
            for _ in range(200):
                conn.sendall(b" ")
                time.sleep(0.3)
        elif mode == "deep":
            self.send(conn, 200, b"[" * 200000 + b"]" * 200000)
        elif mode == "notjson":
            self.send(conn, 200, b"hello, world")
        elif mode == "wrongtypes":
            bodies = {"GET /v1/FireTV": {"isVolumeControlsSupported": "yes", "powerCapabilityLevelOfSupport": 5},
                      "GET /v1/FireTV/appsV2": {"apps": 5}, "POST /v1/FireTV/pin/verify": {"description": ["zBMBFhY"]}}
            self.send(conn, 200, bodies.get(endpoint, [1, 2, 3]))
        elif mode == "truncated":
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1000\r\n\r\n{\"isVolum")
        elif mode == "silent":
            conn.settimeout(60)
            while conn.recv(4096):
                pass
        elif mode == "close":
            pass
        elif mode == "500":
            self.send(conn, 500, {"description": "Internal error"})
        elif mode == "400":
            self.send(conn, 400, BAD_ARGS)
        elif mode in ("404", "405"):
            # firehose section 2 records 405 {"description":"Method not allowed"} for a method a TV does
            # not serve (POST /v1/FireTV/properties, 2026-09-20); the 404 body is invented.
            self.send(conn, int(mode), {"description": "Not found" if mode == "404" else "Method not allowed"})
        elif mode == "badutf8":
            self.send(conn, 200, b'[{"appId":"com.example.bad","name":"Bad \xff\xfe name","isInstalled":true},'
                                 b'{"appId":"com.netflix.ninja","name":"Netflix","isInstalled":true}]')
        elif mode == "hugeheader":
            conn.sendall(b"HTTP/1.1 200 OK\r\nX-Junk: " + b"a" * 300000 + b"\r\nContent-Length: 2\r\n\r\n{}")
        else:
            self.send(conn, 500, {"description": "unknown fault " + mode})

    def serve_api(self, conn, method, path, query, headers, body):
        endpoint = "%s %s" % (method, path)
        token = headers.get("x-client-token")
        entry = {"method": method, "path": path, "query": query, "body": body.decode("utf-8", "replace"),
                 "api_key": headers.get("x-api-key"), "token": token, "content_type": headers.get("content-type"),
                 "t": round(time.time() - self.started, 3)}
        with self.lock:
            self.stats["requests"].append(entry)
            self.counts[endpoint] = count = self.counts.get(endpoint, 0) + 1
            fault = self.cfg["faults"].get(endpoint)
            spared = int(self.cfg.get("fault_after", {}).get(endpoint, 0))
        if fault and count > spared:
            self.fault(conn, fault, endpoint)
            return
        # Auth first, then routing (firehose section 2: a 403 names the missing header).
        if headers.get("x-api-key") != API_KEY:
            self.send(conn, 403, {"description": "Request unauthorized, missing api key"})
            return
        if method == "POST" and body and not headers.get("content-type", "").startswith("application/json"):
            # Forum thread post 9: a scan body sent as a form (curl -d) got this 400, and the same
            # body with "application/json; charset=utf-8" was accepted. Post 3: a body-less
            # media?action=play worked with no Content-Type at all, so body-less POSTs are not
            # checked. (firehose calls application/json "required"; the driver always sends it.)
            self.send(conn, 400, BAD_ARGS)
            return
        if path == "/v1/FireTV/pin/display" and method == "POST":
            return self.pin_display(conn, body)
        if path == "/v1/FireTV/pin/verify" and method == "POST":
            return self.pin_verify(conn, body)
        if not token:
            self.send(conn, 403, {"description": "Request unauthorized, missing client token"})
            return
        with self.lock:
            valid = token in self.tokens and not self.cfg["revoked"]
        if not valid:
            # The wording for a wrong token is not recorded anywhere; the forum only says it is an error.
            self.send(conn, 403, {"description": "Request unauthorized, invalid client token"})
            return
        if method == "GET" and path == "/v1/FireTV/status":
            return self.send(conn, 200, STATUS_BODY)
        if method == "GET" and path in ("/v1/FireTV", "/v1/FireTV2"):
            return self.send(conn, 200, CAPS[self.cfg["caps"]])
        if method == "GET" and path == "/v1/FireTV/appsV2":
            return self.send(conn, 200, self.cfg["apps"])
        if method == "POST" and path == "/v1/FireTV":
            return self.key(conn, query.get("action", ""), body)
        if method == "POST" and path == "/v1/media":
            return self.media(conn, query.get("action", ""), body)
        if method == "POST" and path.startswith("/v1/FireTV/app/"):
            package = path[len("/v1/FireTV/app/"):]
            with self.lock:
                installed = any(isinstance(a, dict) and a.get("appId") == package and a.get("isInstalled", True)
                                for a in self.cfg["apps"])
                if installed:
                    self.stats["launched"].append(package)
            if installed:
                return self.done(conn, 200, OK_REPLY)
            return self.send(conn, 400, BAD_ARGS)
        self.send(conn, 404, {"description": "Not found"})

    def pin_display(self, conn, body):
        try:
            name = json.loads(body.decode() or "{}").get("friendlyName")
        except ValueError:
            name = None
        if not isinstance(name, str):
            return self.send(conn, 400, BAD_ARGS)
        with self.lock:
            self.stats["pin_display"] += 1
            self.stats["friendly_name"] = name
            self.stats["shown_pin"] = self.cfg.get("pin") or "%04d" % random.randrange(10000)
            self.pin_shown_at = time.time()
            self.oks_left = int(self.cfg.get("ok_first", 0))
        self.send(conn, 200, OK_REPLY)

    def pin_verify(self, conn, body):
        with self.lock:
            self.stats["pin_verify"] += 1
            mode = self.cfg["verify"]
            shown = self.stats["shown_pin"]
            fresh = time.time() - self.pin_shown_at < 300
        try:
            pin = json.loads(body.decode() or "{}").get("pin")
        except ValueError:
            pin = None
        if mode == "silent":
            conn.settimeout(60)
            while conn.recv(4096):
                pass
            return
        if mode == "unreadable":
            return self.send(conn, 200, {"somethingElse": 1})
        if not shown or not fresh or pin != shown:
            # No source records what a TV answers to a wrong code; two plausible shapes.
            if self.cfg["wrong_reply"] == "200text":
                return self.send(conn, 200, {"description": "Invalid PIN"})
            return self.send(conn, 400, {"description": "Invalid PIN"})
        with self.lock:
            if mode == "always_ok" or self.oks_left > 0:
                self.oks_left -= 1
                return self.send(conn, 200, OK_REPLY)       # accepted, token not minted yet
            token = "".join(random.choice(string.ascii_letters + string.digits) for _ in range(7))
            self.tokens.add(token)
            self.stats["issued"].append(token)
            self.stats["shown_pin"] = None
        self.send(conn, 200, {"description": token})

    def key(self, conn, action, body):
        with self.lock:
            if action not in KEYED_ACTIONS | PLAIN_ACTIONS:
                return self.send(conn, 400, BAD_ARGS)
            kind = "bodyless"
            if body:
                try:
                    kind = json.loads(body.decode()).get("keyActionType", "?")
                except (ValueError, AttributeError):
                    return self.send(conn, 400, BAD_ARGS)
            self.stats["keys"].append({"action": action, "form": kind})
            caps = CAPS[self.cfg["caps"]]
            flag = caps.get("isVolumeControlsSupported")
            volume = flag if isinstance(flag, bool) else \
                caps.get("volumeCapabilityLevelOfSupport", "NotSupported") != "NotSupported"
            power = caps.get("powerCapabilityLevelOfSupport", "NotSupported") != "NotSupported"
            if action == "sleep":
                self.stats["display"] = "off"           # display off; the API keeps answering (firehose section 3)
            elif action in ("volume_up", "volume_down", "mute"):
                if not volume:
                    self.stats["ignored"].append(action)   # answered 200 all the same (WP-5 trap note)
                elif action == "volume_up":
                    self.stats["volume"] = min(100, self.stats["volume"] + 1)
                elif action == "volume_down":
                    self.stats["volume"] = max(0, self.stats["volume"] - 1)
                else:
                    self.stats["muted"] = not self.stats["muted"]
            elif action == "power":
                if not power:
                    self.stats["ignored"].append(action)
                else:
                    self.stats["display"] = "off" if self.stats["display"] == "on" else "on"
        self.done(conn, 200, OK_REPLY)

    def media(self, conn, action, body):
        with self.lock:
            if action == "play":
                self.stats["playing"] = not self.stats["playing"]   # one toggle (firehose section 2, Media)
                self.stats["keys"].append({"action": "media:play", "form": "bodyless" if not body else "body"})
                carried_out = True
            elif action == "scan":
                try:
                    direction = json.loads(body.decode()).get("direction")
                except (ValueError, AttributeError):
                    direction = None
                carried_out = direction in ("back", "forward")
                if carried_out:
                    self.stats["scans"].append(direction)
            else:
                carried_out = False
        if carried_out:
            return self.done(conn, 200, OK_REPLY)
        self.send(conn, 400, BAD_ARGS)

    def snapshot(self):
        with self.lock:
            out = json.loads(json.dumps(self.stats))
            out["config"] = {k: v for k, v in self.cfg.items() if k != "apps"}
            out["tokens"] = sorted(self.tokens)
            out["pin"] = self.pin()
            return out


# ------------------------------------------------------------------------------------------
# The control port
# ------------------------------------------------------------------------------------------
def control_server(tv):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, obj, status=200):
            data = jdump(obj)
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/stats":
                return self.reply(tv.snapshot())
            self.reply({"error": "no such path"}, 404)

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0") or 0)
            try:
                cfg = json.loads(self.rfile.read(length).decode() or "{}")
                if self.path == "/reset":
                    tv.reset(cfg)
                elif self.path == "/set":
                    tv.apply_cfg(cfg)
                else:
                    return self.reply({"error": "no such path"}, 404)
            except (ValueError, KeyError) as e:
                return self.reply({"error": str(e)}, 400)
            self.reply({"ok": True, "pin": tv.pin()})

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def watch_stdin_and_clock(max_seconds):
    def stdin_closed():
        try:
            while sys.stdin.buffer.read(4096):
                pass
        except (OSError, ValueError):
            pass
        os._exit(0)

    def too_long():
        time.sleep(max_seconds)
        print("MOCK ran for %d s: exiting" % max_seconds, flush=True)
        os._exit(0)

    threading.Thread(target=stdin_closed, daemon=True).start()
    threading.Thread(target=too_long, daemon=True).start()


# ------------------------------------------------------------------------------------------
# Self-test: the mock against Python's own HTTP and TLS clients
# ------------------------------------------------------------------------------------------
def selftest():
    import http.client

    class Args:
        state, caps, cert, mac, wake_delay, boot_delay = "awake", "tv", "a", DEFAULT_MAC, 0.4, 0.4

    runs = fails = 0

    def check(name, ok, detail=""):
        nonlocal runs, fails
        runs += 1
        fails += not ok
        print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok or not detail else "  -> %r" % (detail,)), flush=True)

    tv = Tv(Args())
    client_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    client_ctx.check_hostname = False
    client_ctx.verify_mode = ssl.CERT_NONE

    def api(method, path, body=None, headers=None, timeout=2.0):
        c = http.client.HTTPSConnection("127.0.0.1", tv.api.port, timeout=timeout, context=client_ctx)
        try:
            c.request(method, path, body=body, headers=headers or {})
            r = c.getresponse()
            return r.status, r.read()
        finally:
            c.close()

    def dial_post():
        c = http.client.HTTPConnection("127.0.0.1", tv.dial.port, timeout=2)
        try:
            c.request("POST", "/apps/FireTVRemote", body=b"")
            r = c.getresponse()
            r.read()
            return r.status, r.getheader("Location")
        finally:
            c.close()

    def refused(port):
        s = socket.socket()
        s.settimeout(1)
        try:
            s.connect(("127.0.0.1", port))
            return False
        except ConnectionRefusedError:
            return True
        except OSError:
            return False
        finally:
            s.close()

    def times_out(port):
        s = socket.socket()
        s.settimeout(0.8)
        try:
            s.connect(("127.0.0.1", port))
            return False
        except socket.timeout:
            return True
        except OSError:
            return False
        finally:
            s.close()

    key = {"X-Api-Key": API_KEY, "Content-Type": "application/json"}
    check("key pin of certificate a matches the openssl command line",
          spki_pin(os.path.join(HERE, "test_cert_a.pem")) == "sha256//Yl4ObLO+rHpqTkvNi9gikpx97WZiO3FYB4dI1dwOcTQ=")
    check("key pin of certificate b matches the openssl command line",
          spki_pin(os.path.join(HERE, "test_cert_b.pem")) == "sha256//I56XVFF1iO0r9UdST+fiYqhH33DUkLIo0m2fJPgMMaU=")
    mac = bytes.fromhex("00005e005301")
    check("magic packet parser accepts a good packet", parse_magic(b"\xff" * 6 + mac * 16) == DEFAULT_MAC)
    check("magic packet parser refuses a short one", parse_magic(b"\xff" * 6 + mac * 15) is None)
    check("magic packet parser refuses mixed addresses", parse_magic(b"\xff" * 6 + mac * 15 + b"\x00" * 6) is None)
    st, body = api("GET", "/v1/FireTV/status")
    check("no headers: 403 naming the api key", st == 403 and b"missing api key" in body, (st, body))
    st, body = api("GET", "/v1/FireTV/status", headers=key)
    check("api key without token: 403 naming the client token", st == 403 and b"missing client token" in body, (st, body))
    tv.apply_cfg({"pin": "4321", "ok_first": 2})
    st, _ = api("POST", "/v1/FireTV/pin/display", body=b'{"friendlyName":"Self test"}', headers=key)
    check("pin/display shows a code", st == 200 and tv.stats["shown_pin"] == "4321")
    st, body = api("POST", "/v1/FireTV/pin/verify", body=b'{"pin":"0000"}', headers=key)
    check("a wrong code is refused", st == 400, (st, body))
    replies = [api("POST", "/v1/FireTV/pin/verify", body=b'{"pin":"4321"}', headers=key) for _ in range(3)]
    token = json.loads(replies[2][1]).get("description")
    check("the right code: OK twice, then a token", [json.loads(b)["description"] for _, b in replies[:2]] == ["OK", "OK"]
          and token in tv.tokens, replies)
    st, body = api("GET", "/v1/FireTV/status", headers=dict(key, **{"X-Client-Token": token}))
    check("the token opens the status call", st == 200 and json.loads(body) == STATUS_BODY, (st, body))
    st, body = api("GET", "/v1/FireTV/status", headers=dict(key, **{"X-Client-Token": "nottoken"}))
    check("another token is refused", st == 403, (st, body))
    st, body = api("POST", "/v1/FireTV?action=home", headers={"X-Api-Key": API_KEY, "X-Client-Token": token})
    check("a body-less POST without a Content-Type is taken (forum post 3)", st == 200, (st, body))
    st, body = api("POST", "/v1/media?action=scan", body=b'{"direction":"forward","durationInSeconds":"10","speed":"1"}',
                   headers={"X-Api-Key": API_KEY, "X-Client-Token": token,
                            "Content-Type": "application/x-www-form-urlencoded"})
    check("a body sent as a form is refused with the forum's 400 (post 9)", st == 400 and b"Bad arguments" in body,
          (st, body))
    tv.set_state("idle")
    check("idle: the API port refuses", refused(tv.api.port))
    st, loc = dial_post()
    check("idle: DIAL answers 201 with a Location", st == 201 and loc and loc.endswith("/apps/FireTVRemote/run"), (st, loc))
    time.sleep(0.6)
    st, _ = api("GET", "/v1/FireTV/status", headers=dict(key, **{"X-Client-Token": token}))
    check("idle: the API answers after the wake delay", st == 200, st)
    for mode in ("silent", "silent_tls"):
        tv.apply_cfg({"state": "idle", "starting_s": 0.7, "starting_mode": mode})
        before = tv.stats["silent_connections"]
        st, _ = dial_post()                      # the API starts at 0.4 s, answers from 1.1 s
        time.sleep(0.6)
        try:
            api("GET", "/v1/FireTV/status", headers=key, timeout=0.3)
            check("starting (%s): the API accepts and says nothing for starting_s after the wake" % mode, False)
        except (socket.timeout, TimeoutError):
            check("starting (%s): the API accepts and says nothing for starting_s after the wake" % mode,
                  st == 201 and tv.stats["state"] == "starting" and tv.stats["silent_connections"] == before + 1,
                  (st, tv.stats["state"], tv.stats["silent_connections"]))
        st, _ = dial_post()
        check("starting (%s): DIAL answers 201 meanwhile, and wakes nothing more" % mode, st == 201, st)
        time.sleep(0.5)
        st, _ = api("GET", "/v1/FireTV/status", headers=key)
        check("starting (%s): then the API answers" % mode, st == 403 and tv.stats["state"] == "awake", st)
    tv.apply_cfg({"state": "idle", "starting_s": 0, "starting_mode": "silent", "faults": {"POST /apps/FireTVRemote": "500"}})
    wakes = tv.stats["wakes"]
    st, _ = dial_post()
    time.sleep(0.6)
    check("a DIAL fault: answered 500, and the TV stays idle", st == 500 and tv.stats["wakes"] == wakes and
          refused(tv.api.port), st)
    tv.apply_cfg({"faults": {}})
    tv.set_state("deep")
    check("deep: both ports refuse", refused(tv.api.port) and refused(tv.dial.port))
    w = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    w.sendto(b"\xff" * 6 + bytes.fromhex("00005e0053ff") * 16, ("127.0.0.1", tv.wol.getsockname()[1]))
    time.sleep(0.6)
    check("deep: a packet for another address wakes nothing", refused(tv.api.port))
    w.sendto(b"\xff" * 6 + mac * 16, ("127.0.0.1", tv.wol.getsockname()[1]))
    w.close()
    time.sleep(0.7)
    st, loc = dial_post()
    check("deep: the right packet boots the TV", st == 201 and not refused(tv.api.port), st)
    tv.set_state("deep_blackhole")
    check("deep_blackhole: connections time out", times_out(tv.api.port) and times_out(tv.dial.port))
    tv.set_state("hung")
    try:
        api("GET", "/v1/FireTV/status", headers=key, timeout=0.8)
        check("hung: TLS completes and nothing answers", False)
    except (socket.timeout, TimeoutError):
        check("hung: TLS completes and nothing answers", True)
    tv.set_state("awake")
    tv.apply_cfg({"caps": "stick"})
    st, body = api("GET", "/v1/FireTV", headers=dict(key, **{"X-Client-Token": token}))
    check("stick: the capability object as measured", st == 200 and json.loads(body) == CAPS["stick"], body)
    st, _ = api("POST", "/v1/FireTV?action=volume_up", headers=dict(key, **{"X-Client-Token": token}))
    check("stick: volume_up answers 200 and is ignored", st == 200 and tv.stats["ignored"] == ["volume_up"], tv.stats["ignored"])
    tv.apply_cfg({"caps": "tv", "late": 0.6})
    before = len(tv.stats["keys"])
    try:
        api("POST", "/v1/FireTV?action=home", headers=dict(key, **{"X-Client-Token": token}), timeout=0.3)
        answered_early = True
    except (socket.timeout, TimeoutError):
        answered_early = False
    check("late: a command is carried out at once and answered later",
          not answered_early and len(tv.stats["keys"]) == before + 1 and tv.stats["keys"][-1]["action"] == "home",
          tv.stats["keys"][before:])
    tv.apply_cfg({"late": 0, "faults": {"POST /v1/FireTV": "405"}})
    st, _ = api("POST", "/v1/FireTV?action=home", headers=dict(key, **{"X-Client-Token": token}))
    check("fault 405: answered 405, nothing carried out", st == 405 and len(tv.stats["keys"]) == before + 1, st)
    tv.apply_cfg({"faults": {}, "slow_tls_every": 1, "slow_tls_s": 0.4})
    t0 = time.time()
    st, _ = api("GET", "/v1/FireTV/status", headers=dict(key, **{"X-Client-Token": token}))
    check("slow_tls_every 1: the handshake waits", st == 200 and time.time() - t0 >= 0.35, time.time() - t0)
    tv.apply_cfg({"slow_tls_every": 0})
    print("%d checks, %d failed" % (runs, fails))
    return 1 if fails else 0


def main():
    import argparse
    p = argparse.ArgumentParser(description="pretend Fire TV on 127.0.0.1")
    p.add_argument("--state", default="awake")
    p.add_argument("--caps", default="tv")
    p.add_argument("--cert", default="a")
    p.add_argument("--mac", default=DEFAULT_MAC)
    p.add_argument("--wake-delay", type=float, default=0.9)
    p.add_argument("--boot-delay", type=float, default=7.0)
    p.add_argument("--max-seconds", type=float, default=600)
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args()
    if args.selftest:
        sys.exit(selftest())
    tv = Tv(args)
    control = control_server(tv)
    print("MOCK listening api=%d dial=%d wol=%d control=%d pin=%s" % (
        tv.api.port, tv.dial.port, tv.wol.getsockname()[1], control.server_address[1], tv.pin()), flush=True)
    watch_stdin_and_clock(args.max_seconds)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
