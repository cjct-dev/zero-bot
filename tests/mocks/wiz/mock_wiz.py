#!/usr/bin/env python3
"""A simulated WiZ device: JSON over UDP on 127.0.0.1, as WiZ lights, plugs and fans answer.

usage: mock_wiz.py --port 0 --model MODEL [--key HEX] [--stdin-lifeline]

Prints "MOCK listening udp <port> http <port>" once both sockets are open (ports the system picks
with --port 0). Standard library only.

The device side, UDP: one JSON object per datagram each way, the reply sent back to the address
and port the request came from. Methods: getSystemConfig, getModelConfig, getUserConfig,
getPilot, setPilot, getPower and registration (the discovery query), with the answers and error
codes pywizlight's tests record for real firmware (tests/fake_bulb.py, tests/test_signing.py;
sbidy/pywizlight, MIT): -32601 for a method the firmware lacks, -32602 for bad params, and on
firmware that signs ("signed"), every reply carries "env":"pro", "sigTs" and an "hmac", an
unsigned setPilot is -32602, and one whose HMAC-SHA256 (with the home's key, over the params text
exactly as sent) is wrong, or whose sigTs is more than 30 s away, is -32605.

This mock is stricter than firmware is known to be, so that a driver's mistakes show: a setPilot
with a key it does not know, a dimming below the model's lowest or above 100, a white and a
colour together, a white outside the model's range, or a value the kind of device lacks (a white
on a dimmable bulb, a brightness on a plug, a fan speed on a light) is refused with -32602.

Models (module name, firmware):
  rgb          ESP01_SHRGB_03 1.25.0     colour; getModelConfig without minDimLevel (lowest 10)
  rgb_lowdim   ESP25_SHRGB_01 1.38.0     colour; minDimLevel 1
  tw           ESP21_SHTW_01 1.25.0      tunable white, 2700..5000 K
  dw           ESP05_SHDW_21 1.25.0      dimmable white
  old          ESP01_SHRGB1C_31 1.17.1   no getModelConfig; the white range from getUserConfig
  typeid       (no module name) 1.8.0    typeId 0 only: a dimmable white bulb
  socket       ESP25_SOCKET_01 1.26.2    plug that meters (getPower)
  socket_plain ESP10_SOCKET_06 1.25.0    plug without getPower
  fan          ESP03_FANDIMS_31 1.31.32  ceiling fan (6 speeds) with a dimmable light
  signed       ESP25_SHORGB_01 1.39.0    colour; signs its replies, takes only signed changes (--key)
  camera       ESP99_CAMERA_01 1.30.0    a module of no known kind

The control side, HTTP on 127.0.0.1 (another port):
  GET  /mock/stats    {"requests": [...], "counts": {method: n}, "pilot": {...}, "replies": n}
                      each request: {"method", "params", "raw", "signed", "sig_ok", "port", "dropped"}
  POST /mock/config   {"drop": "none"|"alternate"|"all", "garbage_first": bool, "late_other": bool}
                      alternate: every other datagram is lost (the first is); all: nothing answers,
                      as when "Allow local communication" is off
  POST /mock/reset    forgets the requests seen
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODELS = {
    "rgb": {"module": "ESP01_SHRGB_03", "fw": "1.25.0", "mac": "a8bb5006033d", "cls": "RGB",
            "model_config": {"ps": 1, "pwmFreq": 1000, "pwmRange": [3, 100], "wcr": 30, "nowc": 1,
                             "cctRange": [2200, 2700, 4800, 6500], "renderFactor": [171, 255, 75, 255, 43, 85, 0, 0, 0, 0]}},
    "rgb_lowdim": {"module": "ESP25_SHRGB_01", "fw": "1.38.0", "mac": "a8bb50aa0001", "cls": "RGB",
                   "model_config": {"devTotal": 1, "headTotal": 1, "minDimLevel": 1, "cctRange": [2200, 2700, 6500, 6500]}},
    "tw": {"module": "ESP21_SHTW_01", "fw": "1.25.0", "mac": "6c29905a067c", "cls": "TW",
           "model_config": {"ps": 2, "pwmFreq": 5000, "cctRange": [2700, 2700, 5000, 5000]}},
    "dw": {"module": "ESP05_SHDW_21", "fw": "1.25.0", "mac": "a8bb50060001", "cls": "DW",
           "model_config": {"ps": 1, "pwmRange": [5, 100], "cctRange": [2700, 2700, 2700, 2700]}},
    "old": {"module": "ESP01_SHRGB1C_31", "fw": "1.17.1", "mac": "a8bb50bdf8d7", "cls": "RGB", "model_config": None,
            "user_config": {"fadeIn": 450, "fadeOut": 500, "fadeNight": False, "dftDim": 100, "pwmRange": [0, 100],
                            "whiteRange": [2700, 6500], "extRange": [2200, 6500], "po": False}},
    "typeid": {"module": None, "type_id": 0, "fw": "1.8.0", "mac": "a8bb502054e3", "cls": "DW", "model_config": None},
    "socket": {"module": "ESP25_SOCKET_01", "fw": "1.26.2", "mac": "d8a0119906b7", "cls": "SOCKET", "power_mw": 23456,
               "model_config": {"ps": 1, "cctRange": [2700, 2700, 2700, 2700]}},
    "socket_plain": {"module": "ESP10_SOCKET_06", "fw": "1.25.0", "mac": "a8bb50df7693", "cls": "SOCKET",
                     "model_config": {"ps": 1, "pwmFreq": 200, "cctRange": [2700, 2700, 2700, 2700]}},
    "fan": {"module": "ESP03_FANDIMS_31", "fw": "1.31.32", "mac": "a8bb50fa0001", "cls": "FANDIM",
            "model_config": {"ps": 1, "cctRange": [2700, 2700, 2700, 2700], "fanSpeed": 6}},
    "signed": {"module": "ESP25_SHORGB_01", "fw": "1.39.0", "mac": "a8bb50510001", "cls": "RGB", "signs": True,
               "model_config": {"devTotal": 1, "headTotal": 1, "cctRange": [2700, 2700, 5000, 5000]}},
    "camera": {"module": "ESP99_CAMERA_01", "fw": "1.30.0", "mac": "a8bb50ca0001", "cls": "CAMERA",
               "model_config": {"ps": 1}},
}

KNOWN_PILOT_KEYS = {"state", "dimming", "temp", "r", "g", "b", "c", "w", "sceneId", "speed", "ratio",
                    "fanState", "fanSpeed", "fanMode", "fanRevrs"}


def compact(obj):
    return json.dumps(obj, separators=(",", ":"))


class Device:
    def __init__(self, model, key_hex):
        self.m = MODELS[model]
        self.key = bytes.fromhex(key_hex) if key_hex else None
        self.lock = threading.Lock()
        self.requests = []
        self.replies = 0
        self.config = {"drop": "none", "garbage_first": False, "late_other": False}
        self.datagrams = 0
        cls = self.m["cls"]
        cct = (self.m.get("model_config") or {}).get("cctRange") or (self.m.get("user_config") or {}).get("extRange") or [2200, 6500]
        self.kmin, self.kmax = min(cct), max(cct)
        self.min_dim = (self.m.get("model_config") or {}).get("minDimLevel", 10)
        self.speeds = (self.m.get("model_config") or {}).get("fanSpeed", 0)
        pilot = {"mac": self.m["mac"], "rssi": -62, "src": "udp", "state": False}
        if cls == "RGB":
            pilot.update({"sceneId": 0, "r": 255, "g": 127, "b": 0, "c": 0, "w": 0, "temp": 0, "dimming": 13})
        elif cls == "TW":
            pilot.update({"sceneId": 0, "temp": 4000, "dimming": 60})
        elif cls in ("DW", "FANDIM"):
            pilot.update({"sceneId": 0, "dimming": 40})
        if cls == "FANDIM":
            pilot.update({"fanState": 0, "fanMode": 1, "fanSpeed": 2, "fanRevrs": 0})
        self.pilot = pilot

    # ---- replies
    def reply(self, method, field, body):
        msg = {"method": method, "env": "pro", field: body}
        if self.m.get("signs"):
            body = dict(body, sigTs=int(time.time()))
            msg = {"method": method, "env": "pro", field: body, "hmac": self.sign(compact(body).encode())}
        return compact(msg).encode()

    def sign(self, data):
        return base64.b64encode(hmac.new(self.key or b"\0" * 16, data, hashlib.sha256).digest()).decode()

    def error(self, method, code, message):
        return self.reply(method, "error", {"code": code, "message": message})

    def bad_params(self, method):
        return self.error(method, -32602, "Invalid params")

    def check_pilot(self, params):
        """Whether these setPilot params are ones this kind of device takes (see the module notes)."""
        cls = self.m["cls"]
        light = cls in ("RGB", "TW", "DW", "FANDIM")
        if set(params) - KNOWN_PILOT_KEYS:
            return False
        for k, v in params.items():
            if k == "state" and not isinstance(v, bool):
                return False
            if k != "state" and (isinstance(v, bool) or not isinstance(v, int)):
                return False
        if "dimming" in params and (not light or not self.min_dim <= params["dimming"] <= 100):
            return False
        if "temp" in params and (cls not in ("RGB", "TW") or not self.kmin <= params["temp"] <= self.kmax):
            return False
        colour = {"r", "g", "b", "c", "w"} & set(params)
        if colour and (cls != "RGB" or "temp" in params or any(not 0 <= params[k] <= 255 for k in colour)):
            return False
        fan = {"fanState", "fanSpeed", "fanMode", "fanRevrs"} & set(params)
        if fan and cls != "FANDIM":
            return False
        if "fanSpeed" in params and not 1 <= params["fanSpeed"] <= self.speeds:
            return False
        if "fanState" in params and params["fanState"] not in (0, 1):
            return False
        return True

    def handle(self, raw):
        """Returns the datagrams to send back, and what is recorded about the request."""
        rec = {"raw": raw.decode("utf-8", "replace"), "method": None, "params": None, "signed": False, "sig_ok": None}
        try:
            req = json.loads(raw.decode())
        except (ValueError, UnicodeDecodeError):
            return [b'{"env":"pro","error":{"code":-32700,"message":"Parse error"}}'], rec
        if not isinstance(req, dict) or not isinstance(req.get("method"), str):
            return [b'{"env":"pro","error":{"code":-32600,"message":"Invalid Request"}}'], rec
        method = req["method"]
        params = req.get("params", {})
        rec["method"], rec["params"] = method, params
        rec["signed"] = "hmac" in req
        m = self.m
        if method == "registration":
            return [self.reply(method, "result", {"mac": m["mac"], "success": True})], rec
        if method == "getSystemConfig":
            result = {"mac": m["mac"], "homeId": 653906, "roomId": 989983, "fwVersion": m["fw"], "groupId": 0, "ping": 0}
            if m.get("module"):
                result["moduleName"] = m["module"]
            if "type_id" in m:
                result.update({"typeId": m["type_id"], "homeLock": False, "pairingLock": False})
            return [self.reply(method, "result", result)], rec
        if method == "getModelConfig":
            if m.get("model_config") is None:
                return [self.error(method, -32601, "Method not found")], rec
            return [self.reply(method, "result", m["model_config"])], rec
        if method == "getUserConfig":
            if m.get("user_config") is None:
                return [self.error(method, -32601, "Method not found")], rec
            return [self.reply(method, "result", m["user_config"])], rec
        if method == "getPilot":
            with self.lock:
                pilot = dict(self.pilot)
            return [self.reply(method, "result", pilot)], rec
        if method == "getPower":
            if "power_mw" not in m:
                return [self.error(method, -32601, "Method not found")], rec
            return [self.reply(method, "result", {"power": m["power_mw"]})], rec
        if method != "setPilot":
            return [self.error(method, -32601, "Method not found")], rec
        if not isinstance(params, dict):
            return [self.bad_params(method)], rec
        if m.get("signs"):
            if "sigTs" not in params:
                return [self.bad_params(method)], rec
            # The signature covers the params text exactly as it was sent.
            text = rec["raw"]
            start, end = text.find('"params":'), text.rfind(',"hmac":')
            body = text[start + 9:end] if start >= 0 and end > start else ""
            rec["sig_ok"] = (self.key is not None and hmac.compare_digest(str(req.get("hmac", "")), self.sign(body.encode()))
                             and isinstance(params.get("sigTs"), int) and abs(time.time() - params["sigTs"]) <= 30)
            if not rec["sig_ok"]:
                return [self.error(method, -32605, "Security error")], rec
            params = {k: v for k, v in params.items() if k != "sigTs"}
        if not params or not self.check_pilot(params):
            return [self.bad_params(method)], rec
        with self.lock:
            p = self.pilot
            if "temp" in params:
                p.update({"r": 0, "g": 0, "b": 0, "c": 0, "w": 0} if "r" in p else {})
                p["sceneId"] = 0
            if {"r", "g", "b"} & set(params):
                p["temp"] = 0
                p["sceneId"] = 0
            p.update(params)
        return [self.reply(method, "result", {"success": True})], rec

    def datagram(self, raw, addr, sock):
        with self.lock:
            self.datagrams += 1
            n = self.datagrams
            cfg = dict(self.config)
        out, rec = self.handle(raw)
        rec["port"] = addr[1]
        rec["dropped"] = cfg["drop"] == "all" or (cfg["drop"] == "alternate" and n % 2 == 1)
        with self.lock:
            self.requests.append(rec)
        if rec["dropped"]:
            return
        extra = []
        if cfg["garbage_first"]:
            extra.append(b"\x00\xffnot json")
        if cfg["late_other"]:
            extra.append(self.reply("getSystemConfig", "result", {"mac": self.m["mac"], "late": True}))
            extra.append(b'{"env":"pro","method":"getPilot","error":{"code":-32601,"message":"Method not found"}}')
        for data in extra + out:
            sock.sendto(data, addr)
            with self.lock:
                self.replies += 1

    def stats(self):
        with self.lock:
            counts = {}
            for r in self.requests:
                if r["method"]:
                    counts[r["method"]] = counts.get(r["method"], 0) + 1
            return {"requests": list(self.requests), "counts": counts, "pilot": dict(self.pilot), "replies": self.replies}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--model", choices=sorted(MODELS), required=True)
    ap.add_argument("--key", default="", help="the home's signing key, 32 hex characters (model signed)")
    ap.add_argument("--stdin-lifeline", action="store_true", help="exit when stdin closes")
    args = ap.parse_args()
    dev = Device(args.model, args.key)

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", args.port))

    def serve_udp():
        while True:
            try:
                raw, addr = udp.recvfrom(65536)
            except OSError:
                return
            dev.datagram(raw, addr, udp)

    class Control(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send_json(self, obj):
            body = json.dumps(obj).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/mock/stats":
                self.send_json(dev.stats())
            else:
                self.send_error(404)

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            if self.path == "/mock/config":
                with dev.lock:
                    dev.config.update({k: v for k, v in body.items() if k in dev.config})
                self.send_json({"ok": True})
            elif self.path == "/mock/reset":
                with dev.lock:
                    dev.requests.clear()
                    dev.replies = 0
                    dev.datagrams = 0
                self.send_json({"ok": True})
            else:
                self.send_error(404)

    http = ThreadingHTTPServer(("127.0.0.1", 0), Control)
    threading.Thread(target=serve_udp, daemon=True).start()
    threading.Thread(target=http.serve_forever, daemon=True).start()
    print("MOCK listening udp %d http %d" % (udp.getsockname()[1], http.server_address[1]), flush=True)
    if args.stdin_lifeline:
        sys.stdin.read()
        os._exit(0)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
