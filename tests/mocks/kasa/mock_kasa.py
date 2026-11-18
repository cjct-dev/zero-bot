#!/usr/bin/env python3
"""Simulated TP-Link Kasa devices that speak the older local protocol, on 127.0.0.1 only.

usage: mock_kasa.py [--max-seconds N]

Each device listens on its own TCP port (the control protocol: a four-byte big-endian length,
then JSON scrambled with the autokey XOR whose first key is 171) and its own UDP port (the same
scrambled JSON with no length, as discovery broadcasts it to port 9999). Every port is one the
system picks. "moved" is a device whose firmware moved it to the newer login: its TCP port
refuses connections, as port 9999 of such a device does, and its probe port answers TP-Link's
16-byte discovery query (normally UDP 20002) with a reply naming that login.

On start one line is printed:
    MOCK listening control=PORT devices={"plug":{"tcp":P,"udp":P,"probe":P},...}
Control, HTTP on 127.0.0.1:control:
    GET  /stats    per device: "connections", "requests" (every JSON request received over TCP,
                   decoded), "udp" (the same for discovery datagrams), "probes" (16-byte queries),
                   "bad" (inputs that could not be decoded), "state" (relay, light and children)
    POST /reset    every device back to how it started, counters cleared, modes normal
    POST /set      {"device": NAME, "mode": MODE} changes how a device answers (below)
The process ends after --max-seconds (default 600), or when its standard input closes.

Modes (all but normal are for tests of the client's defences):
    normal        answers like the device
    silent        reads the request and never answers
    close         closes each connection as soon as it is accepted
    garbage       answers a well-framed reply that decodes to bytes that are not JSON
    huge          announces a 2 GB reply, sends 100 bytes and waits
    oversize      sends a well-framed 70000-byte reply
    partial       announces 500 bytes, sends 100 and closes
    split         sends a correct reply one byte at a time, 2 ms apart
    crawl         sends a correct reply one byte every 300 ms (slower than any deadline)
    deep          answers JSON nested 30000 deep
    array         answers a JSON array instead of an object
    names         hostile names, socket ids and values in get_sysinfo
    many          a strip with 600 sockets
    no_sockets    a strip whose get_sysinfo lists no sockets (an empty "children")
    refuse        every set_* method answers err_code -3
    unsupported   every method answers module not support (err_code -1)
    energy        meter readings that are not plausible (strings, negative, enormous)
    probe_tapo    ("moved" only) the probe answer is a Tapo (SMART.) device's
    probe_silent  ("moved" only) the probe is not answered
"""
import argparse
import copy
import datetime
import json
import os
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOCK = threading.Lock()


def scramble(data: bytes) -> bytes:
    key, out = 171, bytearray()
    for b in data:
        key ^= b
        out.append(key)
    return bytes(out)


def unscramble(data: bytes) -> bytes:
    key, out = 171, bytearray()
    for b in data:
        out.append(b ^ key)
        key = b
    return bytes(out)


def plug_sysinfo(alias, model, mac, dev_name, hw="1.0", feature="TIM", typekey="type"):
    return {"sw_ver": "1.0.0 Build 000000 Rel.000000", "hw_ver": hw, typekey: "IOT.SMARTPLUGSWITCH", "model": model,
            "mac": mac, "dev_name": dev_name, "alias": alias, "relay_state": 0, "on_time": 0, "active_mode": "none",
            "feature": feature, "updating": 0, "icon_hash": "", "rssi": -50, "led_off": 0, "longitude_i": 0,
            "latitude_i": 0, "hwId": "0" * 32, "fwId": "0" * 32, "deviceId": mac.replace(":", "") + "0" * 28,
            "oemId": "0" * 32, "next_action": {"type": -1}, "err_code": 0}


def bulb_sysinfo(alias, model, mac, color, variable, strip=False):
    info = {"sw_ver": "1.0.0 Build 000000 Rel.000000", "hw_ver": "1.0", "model": model, "description": "Smart Wi-Fi LED",
            "alias": alias, "mic_type": "IOT.SMARTBULB", "dev_state": "normal", "mic_mac": mac.replace(":", ""),
            "deviceId": mac.replace(":", "") + "0" * 28, "oemId": "0" * 32, "hwId": "0" * 32, "is_factory": False,
            "disco_ver": "1.0", "ctrl_protocols": {"name": "Linkie", "version": "1.0"}, "active_mode": "none",
            "is_dimmable": 1, "is_color": 1 if color else 0, "is_variable_color_temp": 1 if variable else 0,
            "light_state": {"on_off": 0, "dft_on_state": {"mode": "normal", "hue": 0, "saturation": 0,
                                                           "color_temp": 2700, "brightness": 50}},
            "preferred_state": [], "rssi": -50, "err_code": 0}
    if strip:
        info["length"] = 16
        info["lighting_effect_state"] = {"enable": 0, "name": "", "brightness": 100, "custom": 0, "id": ""}
    return info


def make_devices():
    """The devices, each with its sysinfo and its behaviour."""
    devs = {}
    devs["plug"] = {"sysinfo": plug_sysinfo("Kettle plug", "HS110(UK)", "50:C7:BF:00:00:01",
                                            "Wi-Fi Smart Plug With Energy Monitoring", feature="TIM:ENE"),
                    "meter": "v1"}
    devs["plug2"] = {"sysinfo": plug_sysinfo("Desk fan", "KP115(UK)", "C0:06:C3:00:00:02", "Smart Wi-Fi Plug Mini",
                                             feature="TIM:ENE", typekey="mic_type"), "meter": "v2"}
    devs["switch"] = {"sysinfo": plug_sysinfo("Porch light", "HS200(UK)", "50:C7:BF:00:00:03",
                                              "Smart Wi-Fi Light Switch")}
    dimmer = plug_sysinfo("Hall dimmer", "HS220(US)", "50:C7:BF:00:00:04", "Smart Wi-Fi Dimmer", typekey="mic_type")
    dimmer["brightness"] = 25
    devs["dimmer"] = {"sysinfo": dimmer, "dimmer": True}
    strip = plug_sysinfo("Desk strip", "HS300(US)", "C0:06:C3:00:00:05", "Smart Wi-Fi Power Strip", hw="2.0",
                         feature="TIM:ENE", typekey="mic_type")
    del strip["relay_state"]
    base = strip["deviceId"]
    strip["children"] = [{"id": base + "%02d" % i, "state": 1 if i == 1 else 0, "alias": "Socket %s" % "ABCDEF"[i],
                          "on_time": 0, "next_action": {"type": -1}} for i in range(6)]
    strip["child_num"] = 6
    devs["strip"] = {"sysinfo": strip, "meter": "v2"}
    devs["bulb"] = {"sysinfo": bulb_sysinfo("Reading lamp", "KL130(EU)", "1C:3B:F3:00:00:06", True, True),
                    "light": "smartlife.iot.smartbulb.lightingservice", "set": "transition_light_state",
                    "kelvin": (2500, 9000)}
    devs["lightstrip"] = {"sysinfo": bulb_sysinfo("Shelf strip", "KL430(US)", "E8:48:B8:00:00:07", True, True,
                                                  strip=True),
                          "light": "smartlife.iot.lightStrip", "set": "set_light_state", "kelvin": (2500, 9000)}
    devs["edison"] = {"sysinfo": bulb_sysinfo("Edison bulb", "KL50(US)", "1C:3B:F3:00:00:08", False, False),
                      "light": "smartlife.iot.smartbulb.lightingservice", "set": "transition_light_state",
                      "kelvin": None}
    cam = plug_sysinfo("Garden camera", "KC100(UK)", "50:C7:BF:00:00:09", "Kasa Cam")
    cam["type"] = "IOT.IPCAMERA"
    devs["camera"] = {"sysinfo": cam}
    devs["moved"] = {"sysinfo": plug_sysinfo("Moved plug", "HS100(UK)", "CC:32:E5:00:00:0A", "Smart Wi-Fi Plug"),
                     "moved": True}
    for d in devs.values():
        d["initial"] = copy.deepcopy(d["sysinfo"])
    return devs


class Device:
    def __init__(self, name, spec):
        self.name, self.spec = name, spec
        self.sysinfo = spec["sysinfo"]
        self.mode = "normal"
        self.clear()

    def clear(self):
        self.connections, self.requests, self.udp, self.probes, self.bad = 0, [], [], 0, []

    def reset(self):
        self.sysinfo = self.spec["sysinfo"] = copy.deepcopy(self.spec["initial"])
        self.mode = "normal"
        self.clear()

    # ---------------------------------------------------------------- answers
    def view(self):
        """get_sysinfo as the device would give it now (the hostile modes alter it)."""
        info = copy.deepcopy(self.sysinfo)
        if self.mode == "names":
            info["alias"] = "\u0007Kettle\n\"}{<b>`plug​" + "x" * 300
            if "children" in info:
                info["children"][0]["alias"] = "Sock\u0000et \"one\"" + "y" * 200
                info["children"][1]["id"] = "bad\"id"
                info["children"][2]["id"] = "z" * 200
                info["children"][3]["id"] = 12345
                info["children"][4]["id"] = info["children"][5]["id"]
            if "brightness" in info:
                info["brightness"] = 9999
            if "light_state" in info:
                info["light_state"] = {"on_off": 1, "brightness": -40, "hue": "red", "saturation": 1e300,
                                       "color_temp": 0, "mode": "normal"}
        if self.mode == "no_sockets" and "children" in info:
            info["children"] = []
        if self.mode == "many" and "children" in info:
            base = info["deviceId"]
            info["children"] = [{"id": base + "%04d" % i, "state": i % 2, "alias": "S%d" % i} for i in range(600)]
        return info

    def child_ids(self, request):
        ctx = request.get("context")
        if not isinstance(ctx, dict):
            return None
        ids = ctx.get("child_ids")
        return ids if isinstance(ids, list) else []

    def children_by_id(self):
        return {c["id"]: c for c in self.sysinfo.get("children", [])}

    def meter(self, method, args, child):
        kind = self.spec.get("meter")
        if not kind:
            return {"err_code": -1, "err_msg": "module not support"}
        if self.mode == "energy":
            return {"get_realtime": {"err_code": 0, "power_mw": "lots", "power": -50, "total_wh": 1e300},
                    "get_daystat": {"err_code": 0, "day_list": [{"year": 0, "month": 0, "day": d, "energy_wh": -5}
                                                                  for d in range(1, 32)]}}.get(method, {"err_code": -2})
        relay = (child["state"] if child else self.sysinfo.get("relay_state", 0)) == 1
        if method == "get_realtime":
            if kind == "v1":
                return {"current": 0.4 if relay else 0.0, "voltage": 240.1, "power": 95.5 if relay else 0.0,
                        "total": 12.345, "err_code": 0}
            out = {"current_ma": 400 if relay else 0, "voltage_mv": 240100, "power_mw": 95500 if relay else 0,
                   "total_wh": 12345, "err_code": 0}
            if child:
                out["slot_id"] = list(self.children_by_id()).index(child["id"])
            return out
        if method == "get_daystat":
            if not isinstance(args, dict) or not isinstance(args.get("year"), int) or not isinstance(args.get("month"), int):
                return {"err_code": -3, "err_msg": "invalid argument"}
            today = datetime.date.today()
            current = args["year"] == today.year and args["month"] == today.month
            days = []
            for day in range(1, (today.day if current else 28) + 1):
                wh = 250 + day
                days.append({"year": args["year"], "month": args["month"], "day": day,
                             **({"energy": wh / 1000.0} if kind == "v1" else {"energy_wh": wh})})
            return {"day_list": days, "err_code": 0}
        return {"err_code": -2, "err_msg": "member not support"}

    def light(self, params):
        """transition_light_state / set_light_state: the bulb's rules for what it accepts."""
        if not isinstance(params, dict):
            return {"err_code": -3, "err_msg": "invalid argument"}
        info = self.sysinfo
        ls = info["light_state"]
        settings = dict(ls.get("dft_on_state") or {k: ls[k] for k in ("mode", "hue", "saturation", "color_temp",
                                                                     "brightness") if k in ls})
        for key in params:
            if key not in ("on_off", "brightness", "hue", "saturation", "color_temp", "ignore_default",
                           "transition_period", "mode"):
                return {"err_code": -3, "err_msg": "invalid argument"}
        for key, lo, hi in (("brightness", 0, 100), ("hue", 0, 360), ("saturation", 0, 100)):
            if key in params and (not isinstance(params[key], int) or not lo <= params[key] <= hi):
                return {"err_code": -3, "err_msg": "invalid argument"}
        if "color_temp" in params:
            ct, rng = params["color_temp"], self.spec.get("kelvin")
            if not isinstance(ct, int) or (ct != 0 and (rng is None or not rng[0] <= ct <= rng[1])):
                return {"err_code": -3, "err_msg": "invalid argument"}
            if ct == 0 and not info["is_color"]:
                return {"err_code": -3, "err_msg": "invalid argument"}
        if ("hue" in params or "saturation" in params) and not info["is_color"]:
            return {"err_code": -3, "err_msg": "invalid argument"}
        on = params.get("on_off", 1) == 1
        for key in ("brightness", "hue", "saturation", "color_temp"):
            if key in params:
                settings[key] = params[key]
        if on:
            info["light_state"] = dict(settings, on_off=1)
        else:
            info["light_state"] = {"on_off": 0, "dft_on_state": settings}
        return dict(copy.deepcopy(info["light_state"]), err_code=0)

    def handle(self, request):
        """The reply to one decoded request object: one answer per module and method."""
        if not isinstance(request, dict):
            return {"err_code": -1, "err_msg": "module not support"}
        reply = {}
        ids = self.child_ids(request)
        for module, calls in request.items():
            if module == "context":
                continue
            if self.mode == "unsupported" or not isinstance(calls, dict):
                reply[module] = {"err_code": -1, "err_msg": "module not support"}
                continue
            out = {}
            for method, args in calls.items():
                out[method] = self.call(module, method, args, ids)
            reply[module] = out
        return reply

    def call(self, module, method, args, ids):
        info = self.sysinfo
        if self.mode == "refuse" and method.startswith("set") or self.mode == "refuse" and method.startswith("trans"):
            return {"err_code": -3, "err_msg": "invalid argument"}
        children = self.children_by_id()
        if module == "system" and method == "get_sysinfo":
            return self.view()
        if module == "system" and method == "set_relay_state":
            if not isinstance(args, dict) or args.get("state") not in (0, 1) or "light_state" in info:
                return {"err_code": -3, "err_msg": "invalid argument"}
            if children:
                targets = ids if ids is not None else list(children)
                if not targets or any(not isinstance(i, str) or i not in children for i in targets):
                    return {"err_code": -14, "err_msg": "entry not exist"}
                for i in targets:
                    children[i]["state"] = args["state"]
            else:
                if ids:
                    return {"err_code": -14, "err_msg": "entry not exist"}
                info["relay_state"] = args["state"]
            return {"err_code": 0}
        if module == "smartlife.iot.dimmer" and self.spec.get("dimmer"):
            if method == "set_brightness":
                b = args.get("brightness") if isinstance(args, dict) else None
                if not isinstance(b, int) or not 1 <= b <= 100:   # a dimmer has no 0: that is off
                    return {"err_code": -3, "err_msg": "invalid argument"}
                info["brightness"] = b
                return {"err_code": 0}
            return {"err_code": -2, "err_msg": "member not support"}
        if module == self.spec.get("light"):
            if method == self.spec["set"]:
                return self.light(args)
            if method == "get_light_state":
                return dict(copy.deepcopy(info["light_state"]), err_code=0)
            return {"err_code": -2, "err_msg": "member not support"}
        if module == "emeter":
            child = None
            if children:
                if not ids or len(ids) != 1 or ids[0] not in children:
                    return {"err_code": -14, "err_msg": "entry not exist"}
                child = children[ids[0]]
            return self.meter(method, args, child)
        return {"err_code": -1, "err_msg": "module not support"}

    def stats(self):
        state = {k: self.sysinfo[k] for k in ("relay_state", "brightness", "light_state") if k in self.sysinfo}
        if "children" in self.sysinfo:
            state["children"] = {c["id"]: c["state"] for c in self.sysinfo["children"]}
        return {"connections": self.connections, "requests": self.requests, "udp": self.udp, "probes": self.probes,
                "bad": self.bad, "state": state, "mode": self.mode}

    def probe_reply(self):
        mac = self.sysinfo["mac"].replace(":", "-")
        tapo = self.mode == "probe_tapo"
        result = {"device_id": "0" * 32, "device_model": "P110(UK)" if tapo else "HS100(UK)",
                  "device_type": "SMART.TAPOPLUG" if tapo else "IOT.SMARTPLUGSWITCH", "factory_default": False,
                  "hw_ver": "4.1", "ip": "127.0.0.1", "mac": mac, "owner": "0" * 32,
                  "mgt_encrypt_schm": {"encrypt_type": "KLAP", "http_port": 80, "is_support_https": False, "lv": 2}}
        body = json.dumps({"error_code": 0, "result": result}).encode()
        header = bytes([2, 0, 0, 1]) + struct.pack(">H", len(body)) + bytes([0x11, 0]) + os.urandom(4) + bytes(4)
        return header + body


# ---------------------------------------------------------------- serving
def read_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def framed(obj_bytes):
    return struct.pack(">I", len(obj_bytes)) + scramble(obj_bytes)


def serve_connection(dev, conn):
    with conn:
        conn.settimeout(30)
        with LOCK:
            dev.connections += 1
            mode = dev.mode
        if mode == "close":
            return
        try:
            while True:
                head = read_exact(conn, 4)
                if head is None:
                    return
                (n,) = struct.unpack(">I", head)
                if n == 0 or n > 1 << 20:
                    with LOCK:
                        dev.bad.append("length %d" % n)
                    return
                body = read_exact(conn, n)
                if body is None:
                    return
                try:
                    request = json.loads(unscramble(body).decode())
                except ValueError:
                    with LOCK:
                        dev.bad.append("not JSON")
                    return
                with LOCK:
                    dev.requests.append(request)
                    mode = dev.mode
                    reply = dev.handle(request)
                plain = json.dumps(reply, separators=(",", ":")).encode()
                if mode == "silent":
                    time.sleep(20)
                    return
                if mode == "garbage":
                    conn.sendall(struct.pack(">I", 64) + os.urandom(64))
                elif mode == "huge":
                    conn.sendall(struct.pack(">I", 0x7FFFFFFF) + scramble(b"{" * 100))
                    time.sleep(20)
                    return
                elif mode == "oversize":
                    big = b'{"system":{"get_sysinfo":{"alias":"' + b"x" * 69950 + b'"}}}'
                    conn.sendall(framed(big))
                elif mode == "partial":
                    conn.sendall(struct.pack(">I", 500) + scramble(plain)[:100])
                    return
                elif mode in ("split", "crawl"):
                    data = framed(plain)
                    for i in range(len(data)):
                        conn.sendall(data[i:i + 1])
                        time.sleep(0.002 if mode == "split" else 0.3)
                elif mode == "deep":
                    conn.sendall(framed(b"[" * 30000 + b"]" * 30000))
                elif mode == "array":
                    conn.sendall(framed(b"[1,2,3]"))
                else:
                    conn.sendall(framed(plain))
        except OSError:
            return


def serve_tcp(dev, sock):
    while True:
        try:
            conn, _ = sock.accept()
        except OSError:
            return
        threading.Thread(target=serve_connection, args=(dev, conn), daemon=True).start()


def serve_udp(dev, sock):
    while True:
        try:
            data, peer = sock.recvfrom(9000)
        except OSError:
            return
        try:
            request = json.loads(unscramble(data).decode())
        except ValueError:
            with LOCK:
                dev.bad.append("udp: not JSON")
            continue
        with LOCK:
            dev.udp.append(request)
            reply = dev.handle(request)
        if dev.spec.get("moved"):
            continue   # a moved device no longer answers the older protocol at all
        sock.sendto(scramble(json.dumps(reply, separators=(",", ":")).encode()), peer)


def serve_probe(dev, sock):
    while True:
        try:
            data, peer = sock.recvfrom(9000)
        except OSError:
            return
        with LOCK:
            dev.probes += 1
            mode = dev.mode
            reply = dev.probe_reply()
        # Only a device that moved to the newer login answers TP-Link's 20002 query.
        if len(data) != 16 or mode == "probe_silent" or not dev.spec.get("moved"):
            continue
        sock.sendto(reply, peer)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-seconds", type=float, default=600)
    args = ap.parse_args()
    devices = {name: Device(name, spec) for name, spec in make_devices().items()}
    ports = {}
    for name, dev in devices.items():
        tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp.bind(("127.0.0.1", 0))
        if not dev.spec.get("moved"):
            tcp.listen(16)
            threading.Thread(target=serve_tcp, args=(dev, tcp), daemon=True).start()
        # A moved device's port is bound but never listened on: connecting to it is refused.
        dev.tcp_socket = tcp
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.bind(("127.0.0.1", 0))
        threading.Thread(target=serve_udp, args=(dev, udp), daemon=True).start()
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.bind(("127.0.0.1", 0))
        threading.Thread(target=serve_probe, args=(dev, probe), daemon=True).start()
        dev.sockets = (udp, probe)
        ports[name] = {"tcp": tcp.getsockname()[1], "udp": udp.getsockname()[1], "probe": probe.getsockname()[1]}

    class Control(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def answer(self, obj, status=200):
            body = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path != "/stats":
                return self.answer({"error": "not found"}, 404)
            with LOCK:
                self.answer({name: dev.stats() for name, dev in devices.items()})

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            try:
                cfg = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                return self.answer({"error": "bad json"}, 400)
            with LOCK:
                if self.path == "/reset":
                    for dev in devices.values():
                        dev.reset()
                    return self.answer({"ok": True})
                if self.path == "/set" and cfg.get("device") in devices:
                    devices[cfg["device"]].mode = cfg.get("mode", "normal")
                    return self.answer({"ok": True})
            self.answer({"error": "not found"}, 404)

    control = ThreadingHTTPServer(("127.0.0.1", 0), Control)
    threading.Thread(target=control.serve_forever, daemon=True).start()
    print("MOCK listening control=%d devices=%s" % (control.server_address[1], json.dumps(ports, separators=(",", ":"))),
          flush=True)

    def lifeline():
        try:
            sys.stdin.read()
        except Exception:
            pass
        os._exit(0)

    threading.Thread(target=lifeline, daemon=True).start()
    time.sleep(args.max_seconds)
    os._exit(0)


if __name__ == "__main__":
    main()
