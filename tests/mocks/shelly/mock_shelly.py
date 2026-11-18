#!/usr/bin/env python3
"""A simulated Shelly device on 127.0.0.1, for tests/shelly_e2e.py. Standard library only.

usage: mock_shelly.py --port 0 --model MODEL [--password PW] [--user NAME] [--id ID] [--mac MAC]
       mock_shelly.py --selftest

Prints "MOCK listening on 127.0.0.1:PORT" once it is listening, and exits when its stdin closes.

It follows Shelly's public API documentation (shelly-api-docs.shelly.cloud):
  every generation  GET /shelly, never behind a password
  Gen2 and later    POST /rpc with a JSON-RPC frame ({"id", "src", "method", "params"}), answered with
                    {"id", "src", "dst", "result"} or {"id", "src", "dst", "error": {"code", "message"}};
                    GET /rpc/<Method>?k=v answered with the result alone. With a password: HTTP digest
                    per RFC 7616, SHA-256, qop "auth", user "admin", realm = the device id; a 401 has a
                    WWW-Authenticate challenge and an empty body; each nc must be greater than the
                    last one seen for its nonce. Shelly.GetDeviceInfo needs no password.
  Gen1              GET /settings, /status, /relay/N, /light/N, /color/N, /white/N, /roller/N; with a
                    password (the "restricted login"), HTTP basic on everything but /shelly.
Where the documentation is silent the mock makes a stated guess: an uncalibrated cover answers
Cover.GoToPosition with error -109; a Gen1 /relay in roller mode answers 400.

Everything it is sent is logged; GET /mock/log returns {"requests": [...]}, each with "method",
"path", "query", "rpc" (method and params of a frame), "auth" ("none", "ok", "bad"), "setup" (true for
anything that would change how the device is set up: calibration, settings, firmware, reboot).
POST /mock/reset clears the log; POST /mock/config takes {"password", "user", "throttle": bool,
"stale_once": bool, "hostile": "", "id", "mac"} (see HOSTILE below).
"""
import argparse
import base64
import copy
import hashlib
import json
import os
import re
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOSTILE = {
    "": "behave",
    "long": "every JSON answer is padded past 1 MiB",
    "deep": "status and config are nested 200 levels deep",
    "names": "names carry control characters, quotes and 300 characters",
    "types": "values have the wrong types or impossible numbers",
    "garbage": "answers 200 with text that is not JSON",
    "slow": "waits 6 s before answering anything but /shelly",
    "error500": "answers RPC calls with HTTP 500 and an error frame",
    "redirect": "answers everything but /shelly with a redirect to another host",
}

GEN1_MODELS = {"1pm", "1", "25", "25-roller", "25-roller-uncal", "plugs1", "dimmer2", "rgbw2-color", "rgbw2-white",
               "duo1", "ht1", "gas1"}
GEN2_MODELS = {"plus1pm", "plus1-lights", "plus1-garage", "plugs", "plus2pm", "plus2pm-cover", "plus2pm-cover-uncal",
               "rgbw", "dimmer", "duo", "plusht", "wall-thermostat"}

SETUP_METHODS = {"Cover.Calibrate", "Light.Calibrate", "Shelly.Reboot", "Shelly.FactoryReset", "Shelly.Update",
                 "Shelly.SetAuth", "Shelly.SetProfile", "Shelly.ResetWiFiConfig", "Sys.SetConfig", "WiFi.SetConfig",
                 "Switch.SetConfig", "Cover.SetConfig", "Light.SetConfig", "Script.Create", "Webhook.Create"}


def sha(s):
    return hashlib.sha256(s.encode()).hexdigest()


def digest_response(user, realm, password, method, uri, nonce, nc, cnonce, qop="auth", h=sha):
    """RFC 7616 section 3.4.1: response = H(H(A1):nonce:nc:cnonce:qop:H(A2))."""
    ha1 = h("%s:%s:%s" % (user, realm, password))
    ha2 = h("%s:%s" % (method, uri))
    return h("%s:%s:%s:%s:%s:%s" % (ha1, nonce, nc, cnonce, qop, ha2))


def selftest():
    # RFC 7616 section 3.9.1, the SHA-256 and MD5 examples.
    args = ("Mufasa", "http-auth@example.org", "Circle of Life", "GET", "/dir/index.html",
            "7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v", "00000001", "f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ")
    ok = digest_response(*args) == "753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1"
    ok = ok and digest_response(*args, h=lambda s: hashlib.md5(s.encode()).hexdigest()) == "8ca523f5e9506fed4657c9700eebdbec"
    print("selftest %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


# ---------------------------------------------------------------- the simulated devices
def gen2_model(model):
    """(device info, config, status) for a Gen2+ model."""
    def sw(i, name=None, power=None):
        cfg = {"id": i, "name": name, "in_mode": "follow", "initial_state": "match_input"}
        st = {"id": i, "source": "init", "output": False, "temperature": {"tC": 41.2, "tF": 106.2}}
        if power is not None:
            st.update({"apower": power, "voltage": 230.1, "current": 0.0, "aenergy": {"total": 11.679, "by_minute": [0, 0, 0], "minute_ts": 1}})
        return cfg, st

    info = {"name": None, "id": "", "mac": "A8032AB12345", "slot": 0, "model": "", "gen": 2, "fw_id": "20241011-114455/1.4.4-g6d2a586",
            "ver": "1.4.4", "app": "", "auth_en": False, "auth_domain": None}
    config = {"sys": {"device": {"name": None, "mac": "A8032AB12345", "fw_id": "x", "discoverable": True},
                      "ui_data": {}, "location": {"tz": "Europe/London"}},
              "wifi": {"sta": {"ssid": "ExampleNet", "is_open": False, "enable": True}}}
    status = {"sys": {"mac": "A8032AB12345", "restart_required": False, "uptime": 1000},
              "wifi": {"sta_ip": "127.0.0.1", "status": "got ip", "ssid": "ExampleNet", "rssi": -60},
              "cloud": {"connected": False}, "input:0": {"id": 0, "state": False}}

    def put(key, cfg, st):
        config[key] = cfg
        status[key] = st

    if model in ("plus1pm", "plus1-lights", "plus1-garage"):
        info.update(id="shellyplus1pm-a8032ab12345", model="SNSW-001P16EU", app="Plus1PM")
        name = {"plus1pm": "Desk lamp", "plus1-lights": None, "plus1-garage": "Garage door"}[model]
        put("switch:0", *sw(0, name, 12.5))
        if model == "plus1-lights":
            config["sys"]["ui_data"]["consumption_types"] = ["lights"]
            config["sys"]["device"]["name"] = "Hall"
    elif model == "plugs":
        info.update(id="shellyplusplugs-a8032ab12345", model="SNPL-00112EU", app="PlugS")
        put("switch:0", *sw(0, "Kettle", 0.0))
    elif model == "plus2pm":
        info.update(id="shellyplus2pm-a8032ab12345", model="SNSW-102P16EU", app="Plus2PM", profile="switch")
        config["sys"]["device"]["name"] = "Kitchen"
        put("switch:0", *sw(0, None, 5.0))
        put("switch:1", *sw(1, "Cooker hood light", 0.0))
        config["sys"]["ui_data"]["consumption_types"] = ["", "light"]
    elif model in ("plus2pm-cover", "plus2pm-cover-uncal"):
        info.update(id="shellyplus2pm-a8032ab12345", model="SNSW-102P16EU", app="Plus2PM", profile="cover")
        cal = model == "plus2pm-cover"
        st = {"id": 0, "source": "init", "state": "stopped", "apower": 0, "voltage": 230, "current": 0,
              "pos_control": cal, "last_direction": "open"}
        if cal:
            st["current_pos"] = 40
        put("cover:0", {"id": 0, "name": "Lounge blind", "maxtime_open": 60}, st)
    elif model == "rgbw":
        info.update(id="shellyplusrgbwpm-a8032ab12345", model="SNDC-0D4P10WW", app="PlusRGBWPM", profile="rgb")
        put("rgb:0", {"id": 0, "name": "Shelf strip"},
            {"id": 0, "source": "init", "output": False, "rgb": [255, 0, 0], "brightness": 50, "apower": 0})
    elif model == "dimmer":
        info.update(id="shellyplusdimmer-a8032ab12345", model="SNDM-0013US", app="PlusWallDimmer")
        put("light:0", {"id": 0, "name": "Bedroom light"}, {"id": 0, "source": "init", "output": True, "brightness": 30})
    elif model == "duo":
        info.update(id="shellyduobulbg3-a8032ab12345", model="S3BL-D010009AEU", app="DuoBulbG3", gen=3)
        put("cct:0", {"id": 0, "name": "Reading lamp", "ct_range": [2700, 6500]},
            {"id": 0, "source": "init", "output": False, "brightness": 80, "ct": 4000})
    elif model == "plusht":
        info.update(id="shellyplusht-a8032ab12345", model="SNSN-0013A", app="PlusHT")
        config["sys"]["device"]["name"] = "Nursery"
        put("temperature:0", {"id": 0, "name": None}, {"id": 0, "tC": 21.37, "tF": 70.5})
        put("humidity:0", {"id": 0, "name": None}, {"id": 0, "rh": 48.6})
    elif model == "wall-thermostat":
        # A wall display whose relay its own thermostat drives: nothing of it is offered but the sensor.
        info.update(id="shellywalldisplay-a8032ab12345", model="SAWD-0A1XX10EU1", app="WallDisplay")
        put("switch:0", *sw(0, "Heating", None))
        status["sys"]["relay_in_thermostat"] = True
        put("temperature:0", {"id": 0, "name": "Hall temperature"}, {"id": 0, "tC": 19.5})
        put("thermostat:0", {"id": 0, "enable": True}, {"id": 0, "target_C": 20})
    else:
        raise SystemExit("unknown Gen2 model %r" % model)
    return info, config, status


def gen1_model(model):
    """(/shelly, /settings, /status) for a Gen1 model."""
    mac = "A4CF12F45378"
    shelly = {"type": "", "mac": mac, "auth": False, "fw": "20230913-112003/v1.14.0-gcb84623", "longid": 1, "discoverable": True}
    settings = {"device": {"type": "", "mac": mac, "hostname": ""}, "name": None, "fw": shelly["fw"],
                "wifi_sta": {"enabled": True, "ssid": "ExampleNet", "ipv4_method": "dhcp"},
                "login": {"enabled": False, "unprotected": False, "username": "admin"}}
    status = {"wifi_sta": {"connected": True, "ssid": "ExampleNet", "ip": "127.0.0.1", "rssi": -60},
              "mac": mac, "uptime": 1000, "has_update": False}

    def relay(name, appliance="General"):
        return {"name": name, "appliance_type": appliance, "ison": False, "has_timer": False, "default_state": "off"}

    def relay_status():
        return {"ison": False, "has_timer": False, "timer_started": 0, "timer_duration": 0, "timer_remaining": 0,
                "overpower": False, "source": "http"}

    def meter(power):
        return {"power": power, "overpower": 0.0, "is_valid": True, "timestamp": 0, "counters": [0, 0, 0], "total": 4321}

    if model == "1pm":
        shelly["type"] = "SHSW-PM"
        settings["relays"] = [relay("Porch light", "lights")]
        status.update(relays=[relay_status()], meters=[meter(7.5)], tmp={"tC": 40.1, "tF": 104.2, "is_valid": True})
    elif model == "1":
        shelly["type"] = "SHSW-1"
        settings["relays"] = [relay("Garage door")]
        status.update(relays=[relay_status()], meters=[meter(0)])
    elif model in ("25", "25-roller", "25-roller-uncal"):
        shelly["type"] = "SHSW-25"
        settings["name"] = "Landing"
        if model == "25":
            settings["mode"] = "relay"
            settings["relays"] = [relay("Landing light"), relay(None)]
            status.update(relays=[relay_status(), relay_status()], meters=[meter(3.0), meter(0.0)])
        else:
            settings["mode"] = "roller"
            settings["relays"] = [relay(None), relay(None)]
            settings["rollers"] = [{"maxtime": 20, "default_state": "stop", "positioning": model == "25-roller"}]
            status.update(relays=[relay_status(), relay_status()], meters=[meter(0.0), meter(0.0)],
                          rollers=[{"state": "stop", "power": 0, "is_valid": True, "safety_switch": False,
                                    "overtemperature": False, "stop_reason": "normal", "last_direction": "open",
                                    "current_pos": 70, "calibrating": False, "positioning": model == "25-roller"}])
    elif model == "plugs1":
        shelly["type"] = "SHPLG-S"
        settings["relays"] = [relay("Fan heater")]
        status.update(relays=[relay_status()], meters=[meter(1500.0)])
    elif model == "dimmer2":
        shelly["type"] = "SHDM-2"
        settings["lights"] = [{"name": "Dining light", "ison": False, "brightness": 40}]
        status.update(lights=[{"ison": True, "source": "http", "brightness": 40, "mode": "white", "transition": 0}],
                      meters=[meter(9.0)])
    elif model == "rgbw2-color":
        shelly["type"] = "SHRGBW2"
        settings["mode"] = "color"
        settings["lights"] = [{"name": "Cabinet strip"}]
        status.update(lights=[{"ison": False, "mode": "color", "red": 0, "green": 0, "blue": 255, "white": 0, "gain": 60,
                               "effect": 0}], meters=[meter(0.0)])
    elif model == "rgbw2-white":
        shelly["type"] = "SHRGBW2"
        settings["mode"] = "white"
        settings["lights"] = [{"name": "Strip %d" % (i + 1)} for i in range(4)]
        status.update(lights=[{"ison": i == 0, "mode": "white", "brightness": 10 * (i + 1)} for i in range(4)],
                      meters=[meter(0.0) for _ in range(4)])
    elif model == "duo1":
        shelly["type"] = "SHBDUO-1"
        settings["lights"] = [{"name": None}]
        settings["name"] = "Study lamp"
        status.update(lights=[{"ison": True, "brightness": 70, "white": 50, "temp": 3500}], meters=[meter(6.0)])
    elif model == "ht1":
        shelly["type"] = "SHHT-1"
        settings["name"] = "Cellar"
        status.update(tmp={"value": 12.4, "units": "C", "tC": 12.4, "tF": 54.3, "is_valid": True},
                      hum={"value": 81, "is_valid": True}, bat={"value": 90, "voltage": 2.9})
    elif model == "gas1":
        shelly["type"] = "SHGS-1"
        status.update(gas_sensor={"sensor_state": "normal", "alarm_state": "none"})
    else:
        raise SystemExit("unknown Gen1 model %r" % model)
    settings["device"]["type"] = shelly["type"]
    settings["device"]["hostname"] = "shelly%s-%s" % (model.split("-")[0], mac)
    return shelly, settings, status


class Device:
    def __init__(self, args):
        self.lock = threading.Lock()
        self.model = args.model
        self.gen2 = args.model not in GEN1_MODELS
        if self.gen2:
            self.info, self.config, self.status = gen2_model(args.model)
        else:
            self.shelly, self.settings, self.state = gen1_model(args.model)
        if args.id and self.gen2:
            self.info["id"] = args.id
        if args.mac:
            self.set_mac(args.mac)
        self.password = args.password or ""
        self.user = args.user or "admin"
        self.throttle = False
        self.stale_once = False
        self.hostile = ""
        self.log = []
        self.nonces = {}   # nonce -> last nc seen

    @property
    def realm(self):
        return self.info["id"] if self.gen2 else "shelly"

    def configure(self, cfg):
        if "password" in cfg:
            self.password = cfg["password"] or ""
        if "user" in cfg:
            self.user = cfg["user"] or "admin"
        if "hostile" in cfg:
            self.hostile = cfg["hostile"] or ""
        for k in ("throttle", "stale_once"):
            if k in cfg:
                setattr(self, k, bool(cfg[k]))
        if cfg.get("id") and self.gen2:
            self.info["id"] = cfg["id"]
        if cfg.get("mac"):
            self.set_mac(cfg["mac"])

    def set_mac(self, mac):
        """One MAC everywhere the device reports it."""
        if self.gen2:
            self.info["mac"] = self.status["sys"]["mac"] = self.config["sys"]["device"]["mac"] = mac
        else:
            self.shelly["mac"] = self.state["mac"] = self.settings["device"]["mac"] = mac

    # ---- what /shelly says
    def identity(self):
        if self.gen2:
            out = dict(self.info)
            out["auth_en"] = bool(self.password)
            out["auth_domain"] = self.info["id"] if self.password else None
            return out
        out = dict(self.shelly)
        out["auth"] = bool(self.password)
        return out


DEV = None


def hostile_names(obj):
    bad = "Lamp\x01\x1b[31m \"quoted\" “typeset” <b>{x}</b> " + "y" * 300
    if isinstance(obj, dict):
        for k, v in list(obj.items()):
            if k == "name":
                obj[k] = bad
            else:
                hostile_names(v)
    elif isinstance(obj, list):
        for v in obj:
            hostile_names(v)
    return obj


def hostile_types(obj):
    if isinstance(obj, dict):
        for k, v in list(obj.items()):
            if k in ("output", "ison", "pos_control", "positioning"):
                obj[k] = "yes"
            elif k in ("brightness", "gain"):
                obj[k] = 5000
            elif k in ("apower", "power"):
                obj[k] = "12"
            elif k in ("current_pos",):
                obj[k] = 1e9
            elif k in ("tC", "rh", "value"):
                obj[k] = -1e300
            elif k == "rgb":
                obj[k] = [300, -5, "x"]
            elif k == "ct":
                obj[k] = 99999
            else:
                hostile_types(v)
    elif isinstance(obj, list):
        for v in obj:
            hostile_types(v)
    return obj


def nest(depth):
    v = 1
    for _ in range(depth):
        v = [v]
    return v


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "ShellyMock/1.0"

    def log_message(self, *a):
        pass

    # ---- plumbing
    def send(self, status, body=b"", ctype="application/json", headers=()):
        if isinstance(body, (dict, list)) or body is None:
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        if body or status not in (401,):
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, value):
        if DEV.hostile == "long":
            value = {"pad": "x" * (1100 * 1024), "value": value}
        if DEV.hostile == "garbage":
            return self.send(200, "<html>not json</html>", "text/html")
        self.send(200, value)

    def challenge(self, stale=False):
        nonce = base64.b64encode(os.urandom(24)).decode()
        DEV.nonces[nonce] = 0
        if DEV.gen2:
            h = 'Digest qop="auth", realm="%s", nonce="%s", algorithm=SHA-256' % (DEV.realm, nonce)
            if stale:
                h += ", stale=true"
            self.send(401, b"", headers=[("WWW-Authenticate", h)])
        else:
            self.send(401, "401 Unauthorized", "text/plain", headers=[("WWW-Authenticate", 'Basic realm="%s"' % DEV.realm)])

    def check_auth(self, entry):
        """Returns True when the request may go ahead; otherwise has answered it."""
        header = self.headers.get("Authorization", "")
        if not DEV.password:
            entry["auth"] = "none" if not header else "ok"
            return True
        if DEV.throttle:
            entry["auth"] = "throttled"
            self.send(429, "Too many requests", "text/plain")
            return False
        if not header:
            entry["auth"] = "none"
            self.challenge()
            return False
        if DEV.gen2:
            ok = self.digest_ok(header, entry)
        else:
            ok = False
            if header.startswith("Basic "):
                try:
                    user, _, pw = base64.b64decode(header[6:]).decode().partition(":")
                    ok = user == DEV.user and pw == DEV.password
                except Exception:
                    ok = False
        if ok and DEV.stale_once and DEV.gen2:
            DEV.stale_once = False
            entry["auth"] = "stale"
            self.challenge(stale=True)
            return False
        entry["auth"] = "ok" if ok else "bad"
        if not ok:
            self.challenge()
        return ok

    def digest_ok(self, header, entry):
        if not header.startswith("Digest "):
            return False
        fields = dict((m.group(1).lower(), m.group(2) if m.group(2) is not None else m.group(3))
                      for m in re.finditer(r'(\w+)=(?:"([^"]*)"|([^\s,]+))', header[7:]))
        entry["digest"] = {k: fields.get(k) for k in ("username", "realm", "uri", "algorithm", "qop", "nc")}
        nonce, nc = fields.get("nonce", ""), fields.get("nc", "")
        if fields.get("username") != "admin" or fields.get("realm") != DEV.realm:
            return False
        if fields.get("algorithm", "").upper() != "SHA-256" or fields.get("qop") != "auth":
            return False
        if fields.get("uri") != self.path or nonce not in DEV.nonces or not re.fullmatch(r"[0-9a-fA-F]{8}", nc or ""):
            return False
        if int(nc, 16) <= DEV.nonces[nonce]:
            return False
        expected = digest_response("admin", DEV.realm, DEV.password, self.command, fields["uri"], nonce, nc, fields.get("cnonce", ""))
        if fields.get("response") != expected:
            return False
        DEV.nonces[nonce] = int(nc, 16)
        return True

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n > 0 else b""

    # ---- dispatch
    def do_GET(self):
        self.handle_any("GET", b"")

    def do_POST(self):
        self.handle_any("POST", self.body())

    def handle_any(self, method, raw):
        url = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(url.query, keep_blank_values=True))
        if url.path.startswith("/mock/"):
            return self.control(url.path, raw)
        entry = {"method": method, "path": url.path, "query": query, "auth": "none", "setup": False, "t": time.time()}
        with DEV.lock:
            DEV.log.append(entry)
        if url.path == "/shelly":
            return self.send_json(DEV.identity())
        if DEV.hostile == "redirect":
            return self.send(302, b"", headers=[("Location", "http://192.0.2.1/elsewhere")])
        if DEV.hostile == "slow":
            time.sleep(6)
        if DEV.gen2:
            return self.gen2(method, url.path, query, raw, entry)
        return self.gen1(url.path, query, entry)

    def control(self, path, raw):
        with DEV.lock:
            if path == "/mock/log":
                return self.send(200, {"requests": DEV.log})
            if path == "/mock/reset":
                DEV.log = []
                return self.send(200, {"ok": True})
            if path == "/mock/config":
                DEV.configure(json.loads(raw or b"{}"))
                return self.send(200, {"ok": True})
            if path == "/mock/state":
                return self.send(200, DEV.status if DEV.gen2 else DEV.state)
        self.send(404, {"error": "no such control"})

    # ---- Gen2+
    def gen2(self, method, path, query, raw, entry):
        if path == "/rpc" and method == "POST":
            if not raw and DEV.password:
                # An empty POST is how libcurl draws a digest challenge: no method to read, so no
                # exception for Shelly.GetDeviceInfo.
                entry["rpc"] = {"method": None}
                if not self.check_auth(entry):
                    return
            try:
                frame = json.loads(raw)
                name, params, fid = frame["method"], frame.get("params") or {}, frame.get("id")
                src = frame.get("src", "")
            except Exception:
                entry["rpc"] = {"method": None}
                return self.send(400, {"code": -103, "message": "bad frame"})
        elif path.startswith("/rpc/"):
            name, fid, src = path[5:], None, None
            params = {}
            for k, v in query.items():
                try:
                    params[k] = json.loads(v)
                except ValueError:
                    params[k] = v
        else:
            return self.send(404, "Not Found", "text/plain")
        entry["rpc"] = {"method": name, "params": params}
        entry["setup"] = name in SETUP_METHODS or name.endswith("SetConfig") or name.endswith("Calibrate")
        if name != "Shelly.GetDeviceInfo" and not self.check_auth(entry):
            return
        with DEV.lock:
            result, error = self.call(name, params)
        if DEV.hostile == "error500" and error is None:
            error = (-114, "simulated")
        if fid is None:   # GET /rpc/<method>: the result alone, or the error with a 4xx/5xx status
            return self.send_json(result) if error is None else self.send(500 if error[0] != -103 else 400, {"code": error[0], "message": error[1]})
        reply = {"id": fid, "src": DEV.info["id"], "dst": src}
        if error is None:
            reply["result"] = result
        else:
            reply["error"] = {"code": error[0], "message": error[1]}
        if DEV.hostile == "error500":
            return self.send(500, reply)
        self.send_json(reply)

    def call(self, name, p):
        st, cfg = DEV.status, DEV.config
        if name == "Shelly.GetDeviceInfo":
            return DEV.identity(), None
        if name == "Shelly.GetStatus":
            if DEV.hostile == "deep":
                return {"sys": nest(200)}, None
            out = copy.deepcopy(st)
            if DEV.hostile == "types":
                hostile_types(out)
            return out, None
        if name == "Shelly.GetConfig":
            if DEV.hostile == "deep":
                return {"sys": nest(200)}, None
            out = copy.deepcopy(cfg)
            if DEV.hostile == "names":
                hostile_names(out)
            return out, None
        comp, _, verb = name.partition(".")
        comp = comp.lower()
        if "id" not in p or not isinstance(p["id"], int) or isinstance(p["id"], bool):
            return None, (-103, "Missing required 'id'")
        key = "%s:%d" % (comp, p["id"])
        if key not in st:
            return None, (-105, "Bad id=%s" % p["id"])
        s = st[key]
        extra = set(p) - {"id", "on", "brightness", "rgb", "white", "ct", "pos", "transition_duration", "toggle_after", "tag", "offset"}
        if extra:
            return None, (-103, "Unexpected %s" % sorted(extra))
        if verb == "Set" and comp in ("switch", "light", "rgb", "rgbw", "cct"):
            if comp == "switch" and "on" not in p:
                return None, (-103, "Missing required 'on'")
            if "on" in p:
                if not isinstance(p["on"], bool):
                    return None, (-103, "'on' must be a boolean")
                was = s["output"]
                s["output"] = p["on"]
                if comp == "switch":
                    return {"was_on": was}, None
            if "brightness" in p:
                if comp == "switch" or not isinstance(p["brightness"], int) or not 0 <= p["brightness"] <= 100:
                    return None, (-103, "Bad brightness")
                s["brightness"] = p["brightness"]
            if "rgb" in p:
                v = p["rgb"]
                if comp not in ("rgb", "rgbw") or not (isinstance(v, list) and len(v) == 3 and all(isinstance(x, int) and 0 <= x <= 255 for x in v)):
                    return None, (-103, "Bad rgb")
                s["rgb"] = v
            if "ct" in p:
                lo, hi = cfg[key].get("ct_range", [2700, 6500])
                if comp != "cct" or not isinstance(p["ct"], int) or not lo <= p["ct"] <= hi:
                    return None, (-103, "Bad ct")
                s["ct"] = p["ct"]
            return None, None
        if comp == "cover":
            if verb in ("Open", "Close"):
                s["state"] = "opening" if verb == "Open" else "closing"
                s["last_direction"] = "open" if verb == "Open" else "close"
                if s.get("pos_control"):
                    s["current_pos"] = 100 if verb == "Open" else 0
                return None, None
            if verb == "Stop":
                s["state"] = "stopped"
                return None, None
            if verb == "GoToPosition":
                if not s.get("pos_control"):
                    return None, (-109, "Precondition failed: cover not calibrated!")
                if not isinstance(p.get("pos"), int) or not 0 <= p["pos"] <= 100:
                    return None, (-103, "Bad pos")
                s["current_pos"] = p["pos"]
                return None, None
            if verb == "Calibrate":
                s["state"] = "calibrating"
                return None, None
        return None, (404, "No handler for %s" % name)

    # ---- Gen1
    def gen1(self, path, q, entry):
        segs = [x for x in path.split("/") if x]
        entry["setup"] = (bool(segs) and segs[0] == "settings" and (len(segs) > 1 or bool(q))) or \
                         (len(segs) == 3 and segs[2] == "calibrate") or path in ("/reboot", "/ota", "/reset", "/calibrate")
        if not self.check_auth(entry):
            return
        with DEV.lock:
            st, se = DEV.state, DEV.settings
            if path == "/settings" and not q:
                out = copy.deepcopy(se)
                if DEV.hostile == "names":
                    hostile_names(out)
                if DEV.hostile == "deep":
                    out = {"relays": nest(200)}
                return self.send_json(out)
            if path == "/status" and not q:
                out = copy.deepcopy(st)
                if DEV.hostile == "types":
                    hostile_types(out)
                if DEV.hostile == "deep":
                    out = {"relays": nest(200)}
                return self.send_json(out)
            if entry["setup"]:
                return self.send_json({"ok": True})   # a real device would change its setup here
            if len(segs) != 2 or not segs[1].isdigit():
                return self.send(404, "Not Found", "text/plain")
            kind, i = segs[0], int(segs[1])
            if kind == "relay":
                if se.get("mode") == "roller":
                    return self.send(400, "Device mode is not relay!", "text/plain")
                if i >= len(st.get("relays", [])):
                    return self.send(404, "Not Found", "text/plain")
                r = st["relays"][i]
                turn = q.get("turn")
                if turn not in (None, "on", "off", "toggle"):
                    return self.send(400, "Bad turn", "text/plain")
                if turn:
                    r["ison"] = (not r["ison"]) if turn == "toggle" else turn == "on"
                return self.send_json(r)
            if kind in ("light", "color", "white"):
                lights = st.get("lights", [])
                if i >= len(lights):
                    return self.send(404, "Not Found", "text/plain")
                if kind == "color" and se.get("mode") != "color" or kind == "white" and se.get("mode") != "white":
                    return self.send(400, "Bad mode", "text/plain")
                l = lights[i]
                for k, v in q.items():
                    if k == "turn":
                        if v not in ("on", "off", "toggle"):
                            return self.send(400, "Bad turn", "text/plain")
                        l["ison"] = (not l["ison"]) if v == "toggle" else v == "on"
                    elif k in ("brightness", "gain", "red", "green", "blue", "white", "temp"):
                        if not v.isdigit():
                            return self.send(400, "Bad %s" % k, "text/plain")
                        n = int(v)
                        lo, hi = {"brightness": (1 if DEV.model.startswith("dimmer") else 0, 100), "gain": (0, 100),
                                  "temp": (2700 if DEV.model == "duo1" else 3000, 6500)}.get(k, (0, 255))
                        if not lo <= n <= hi:
                            return self.send(400, "Bad %s" % k, "text/plain")
                        l[k] = n
                    else:
                        return self.send(400, "Bad parameter %s" % k, "text/plain")
                return self.send_json(l)
            if kind == "roller":
                if se.get("mode") != "roller" or i >= len(st.get("rollers", [])):
                    return self.send(404, "Not Found", "text/plain")
                r = st["rollers"][i]
                go = q.get("go")
                if go in ("open", "close"):
                    r["state"] = go
                    r["last_direction"] = go
                    if r["positioning"]:
                        r["current_pos"] = 100 if go == "open" else 0
                elif go == "stop":
                    r["state"] = "stop"
                elif go == "to_pos":
                    if not r["positioning"]:
                        return self.send(400, "Roller is not calibrated!", "text/plain")
                    pos = q.get("roller_pos", "")
                    if not pos.isdigit() or not 0 <= int(pos) <= 100:
                        return self.send(400, "Bad roller_pos", "text/plain")
                    r["current_pos"] = int(pos)
                else:
                    return self.send(400, "Bad go", "text/plain")
                return self.send_json(r)
        return self.send(404, "Not Found", "text/plain")


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # A client that stops reading (the driver refusing an answer too long, or giving up on a
        # slow one) is part of the tests, not a fault of the mock.
        if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            return
        super().handle_error(request, client_address)


def main():
    global DEV
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--model", default="plus1pm")
    ap.add_argument("--password", default="")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--id", default="")
    ap.add_argument("--mac", default="")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    DEV = Device(args)
    server = Server(("127.0.0.1", args.port), Handler)
    print("MOCK listening on 127.0.0.1:%d" % server.server_address[1], flush=True)

    def watch_stdin():
        try:
            sys.stdin.read()
        finally:
            os._exit(0)

    threading.Thread(target=watch_stdin, daemon=True).start()
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
