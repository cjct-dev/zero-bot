#!/usr/bin/env python3
"""A pretend Philips Hue bridge (CLIP v2 over HTTPS), Python standard library only.

    mock_hue.py --state DIR [--max-seconds N]      serve; prints "PORT <n>" once listening
    mock_hue.py --selftest                         check the mock itself
    mock_hue.py --dump                             print the resources as JSON

tests/hue_e2e.py starts it, for build/unit/hue_cli and for the command-line build of the app. The
certificates it presents are in certs/ beside it (test fixtures made by make_certs.sh, not secrets).

Listens on 127.0.0.1, port 0 (the system picks a free one), and exits when its standard input
closes, on SIGTERM, or after --max-seconds (default 600), so no server is left running.

What it serves, as a bridge on firmware 1971060010 does (shapes follow aiohue's fixture recorded
from a real bridge, and Signify's "Get started" page for POST /api):
  GET  /api/<anything>/config                  the public configuration, no key needed
  POST /api   {"devicetype": "..."}            error 101 until DIR/button exists (and is under
                                               30 s old), then {"success":{"username":<new key>}}
  GET  /clip/v2/resource[/<type>[/<id>]]       needs a hue-application-key it issued: 403 otherwise
  PUT  /clip/v2/resource/light/<id>            on, dimming, color_temperature, color, dynamics
  PUT  /clip/v2/resource/grouped_light/<id>    the same, applied to the group's lights
  PUT  /clip/v2/resource/scene/<id>            {"recall":{"action":"active"}}

Control files in DIR, read on every connection or request (the test writes them):
  button        exists: the link button was pressed (valid for 30 s from its modification time)
  cert          which certificate new connections get: a (default), b, rsa, other, san, chain
  fault         "<name> [<path prefix>]": misbehave on matching requests, see FAULTS below
  revoke        exists: every issued key is refused (403), as after removal in the Hue app
  busy          a number N: the next N requests under /clip get 429
  config.json   an object merged into /api/0/config (e.g. an old swversion)
  extra-room    exists: the Hall light's device moves to the Kitchen (membership changed)
  no-camera     exists: the camera (and its floodlight and scene) is not on this bridge
  plug-in-scene exists: the Living room's Relax scene also switches on its smart plug (the
                "Fan heater"), as the Hue app does when a scene is made for a room with a plug
  swap-after-config   a certificate name: once /api/0/config has been answered, new
                connections get that certificate (a certificate changed mid-pairing)
  unreachable   exists: every light reports communication issues on PUT (207)
Writes DIR/requests.jsonl, one JSON line per request: method, path, key ("ok", "wrong" or
"none"), body (parsed when JSON), status, t (monotonic seconds). The key itself is never
written.
"""
import argparse
import copy
import http.client
import http.server
import json
import os
import secrets
import signal
import socket
import socketserver
import ssl
import sys
import threading
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
CERTS = os.path.join(HERE, "certs")
BRIDGE_ID = "00005EFFFE005301"          # IANA documentation MAC range, as a bridge writes its id
MAC = "00:00:5e:00:53:01"
NS = uuid.UUID("6f1c6f3e-9a5d-4d7e-8f43-4f8a0d9b1c2e")

FAULTS = {
    "silent": "accept TCP and never start TLS",
    "silent-http": "complete TLS, read the request, never answer",
    "garbage": "200 with a body that is not JSON",
    "deep": "200 with arrays nested 100000 deep",
    "huge": "200 with a 10 MB body",
    "huge-valid": "resources served as usual, in a well-formed reply padded past 5 MB",
    "huge-length": "200 announcing a 100 GB body",
    "truncated": "Content-Length 1000, 100 bytes, then close",
    "wrong-types": "resources whose fields have the wrong JSON types",
    "nonutf8": "names containing bytes that are not UTF-8",
    "data-object": "\"data\" is an object, not an array",
    "status-500": "500 Internal Server Error",
    "not-hue": "/api/0/config answers like something else",
    "link-refused": "POST /api answers error type 7",
}


def rid(name):
    return str(uuid.uuid5(NS, name))


# ---------------------------------------------------------------- the bridge's resources
def ident(name, rtype):
    return {"rid": rid(name), "rtype": rtype}


def device(name, label, archetype, model, services, product_archetype=None):
    return {
        "id": rid(name), "id_v1": "", "type": "device",
        "identify": {},
        "metadata": {"name": label, "archetype": archetype},
        "product_data": {"model_id": model, "manufacturer_name": "Signify Netherlands B.V.",
                         "product_name": label, "product_archetype": product_archetype or archetype,
                         "certified": True, "software_version": "1.122.2"},
        "services": [ident(s, t) for s, t in services],
    }


GAMUT_C = {"red": {"x": 0.6915, "y": 0.3083}, "green": {"x": 0.17, "y": 0.7}, "blue": {"x": 0.1532, "y": 0.0475}}
GAMUT_A = {"red": {"x": 0.704, "y": 0.296}, "green": {"x": 0.2151, "y": 0.7106}, "blue": {"x": 0.138, "y": 0.08}}


def light(name, owner, label, archetype, on=False, dim=None, ct=None, color=None, n=1):
    """dim: brightness or None; ct: (mirek or None, min, max) or None; color: (x, y, gamut, type) or None."""
    r = {
        "id": rid(name), "id_v1": "/lights/%d" % n, "type": "light",
        "owner": ident(owner, "device"),
        "metadata": {"name": label, "archetype": archetype, "function": "mixed"},
        "product_data": {"function": "mixed"},
        "identify": {}, "service_id": 0, "mode": "normal",
        "on": {"on": on},
        "alert": {"action_values": ["breathe"]},
        "signaling": {"signal_values": ["no_signal", "on_off"]},
        "dynamics": {"speed": 0.0, "speed_valid": False, "status": "none", "status_values": ["none"]},
        "powerup": {"preset": "safety", "configured": True, "on": {"mode": "on", "on": {"on": True}}},
    }
    if dim is not None:
        r["dimming"] = {"brightness": dim, "min_dim_level": 0.2}
        r["dimming_delta"] = {}
    if ct is not None:
        mirek, lo, hi = ct
        r["color_temperature"] = {"mirek": mirek, "mirek_valid": mirek is not None,
                                  "mirek_schema": {"mirek_minimum": lo, "mirek_maximum": hi}}
        r["color_temperature_delta"] = {}
    if color is not None:
        x, y, gamut, gtype = color
        r["color"] = {"xy": {"x": x, "y": y}, "gamut": gamut, "gamut_type": gtype}
    return r


def group(kind, name, label, children, archetype="other", with_light=True, n=0):
    r = {"id": rid(name), "id_v1": "/groups/%d" % n, "type": kind,
         "children": children, "services": []}
    if kind != "bridge_home":
        r["metadata"] = {"name": label, "archetype": archetype}
    if with_light:
        r["services"].append(ident("gl-" + name, "grouped_light"))
    return r


def grouped_light(name, owner_kind):
    return {"id": rid("gl-" + name), "id_v1": "", "type": "grouped_light",
            "owner": ident(name, owner_kind), "on": {"on": False}, "dimming": {"brightness": 0.0},
            "dimming_delta": {}, "color_temperature": {}, "color_temperature_delta": {},
            "alert": {"action_values": ["breathe"]}, "signaling": {"signal_values": ["no_signal", "on_off"]},
            "dynamics": {}}


def scene(name, label, group_name, group_kind, targets):
    return {"id": rid(name), "id_v1": "/scenes/" + name, "type": "scene",
            "metadata": {"name": label, "image": ident("img-" + name, "public_image")},
            "group": ident(group_name, group_kind),
            "actions": [{"target": ident(t, "light"),
                         "action": {"on": {"on": True}, "dimming": {"brightness": 40.0}}} for t in targets],
            "palette": {"color": [], "dimming": [], "color_temperature": [], "effects": [], "effects_v2": []},
            "recall": {}, "speed": 0.6, "auto_dynamic": False,
            "status": {"active": "inactive"}}


def simple(name, rtype, owner, **fields):
    r = {"id": rid(name), "type": rtype, "owner": ident(owner, "device")}
    r.update(fields)
    return r


HOSTILE_NAME = "\u0007Evil\u202e lamp " + "x" * 60 + "\n"


def make_resources(extra_room=False, no_camera=False, plug_scene=False):
    devices = [
        device("bridge", "Hue Bridge", "bridge_v2", "BSB002", [("br", "bridge"), ("zdd", "zigbee_device_discovery")]),
        device("d-sofa", "Sofa lamp", "table_shade", "LCA001", [("l-sofa", "light"), ("zc-sofa", "zigbee_connectivity")], "sultan_bulb"),
        device("d-ceiling", "Ceiling", "ceiling_round", "LTA009", [("l-ceiling", "light")]),
        device("d-plug", "Fan heater", "plug", "LOM007", [("l-plug", "light")]),
        device("d-strip", "Kitchen strip", "hue_lightstrip", "LST001", [("l-strip", "light")]),
        device("d-spots", "Kitchen spots", "single_spot", "LTG002", [("l-spots", "light")]),
        device("d-hall", "Hall light", "classic_bulb", "LWA001", [("l-hall", "light")]),
        device("d-lantern", "Garden lantern", "string_light", "LCX028", [("l-lantern", "light")]),
        device("d-hostile", HOSTILE_NAME, "classic_bulb", "LWA001", [("l-hostile", "light")]),
        device("d-sensor", "Hall sensor", "unknown_archetype", "SML003",
               [("motion-1", "motion"), ("temp-1", "temperature"), ("ll-1", "light_level"), ("dp-1", "device_power")]),
        device("d-camera", "Porch camera", "unknown_archetype", "CMW004", [("l-flood", "light"), ("cm-1", "camera_motion")]),
        device("d-contact", "Back door", "unknown_archetype", "SOC001", [("contact-1", "contact"), ("tamper-1", "tamper")]),
        device("d-chime", "Chime", "hue_chime", "COM001", [("spk-1", "speaker")]),
        device("d-dimmer", "Dimmer switch", "unknown_archetype", "RWL022", [("btn-1", "button"), ("dp-2", "device_power")]),
        device("d-future", "Future thing", "unknown_archetype", "ZZZ001", [("fut-1", "zz_future_service")]),
    ]
    lights = [
        light("l-sofa", "d-sofa", "Sofa lamp", "table_shade", True, 60.0, (None, 153, 500), (0.4573, 0.41, GAMUT_C, "C"), 1),
        light("l-ceiling", "d-ceiling", "Ceiling", "ceiling_round", False, 100.0, (366, 153, 454), None, 2),
        light("l-plug", "d-plug", "Fan heater", "plug", False, None, None, None, 3),
        light("l-strip", "d-strip", "Kitchen strip", "hue_lightstrip", True, 30.0, None, (0.1691, 0.0441, GAMUT_A, "A"), 4),
        light("l-spots", "d-spots", "Kitchen spots", "single_spot", True, 80.0, (250, 153, 454), None, 5),
        light("l-hall", "d-hall", "Hall light", "classic_bulb", False, 50.0, None, None, 6),
        light("l-lantern", "d-lantern", "Garden lantern", "string_light", False, 100.0, (None, 50, 1000),
              (0.5974, 0.36, GAMUT_C, "C"), 7),
        light("l-hostile", "d-hostile", HOSTILE_NAME, "classic_bulb", False, 10.0, None, None, 8),
        light("l-flood", "d-camera", "Porch floodlight", "wall_lantern", False, 100.0, None, None, 9),
    ]
    living = [ident("d-sofa", "device"), ident("d-ceiling", "device"), ident("d-plug", "device")]
    kitchen = [ident("d-strip", "device"), ident("d-spots", "device")]
    if extra_room:
        kitchen.append(ident("d-hall", "device"))
    hallway = [ident("d-hostile", "device"), ident("d-sensor", "device"), ident("d-camera", "device")]
    if not extra_room:
        hallway.insert(0, ident("d-hall", "device"))
    rooms = [
        group("room", "room-living", "Living room", living, "living_room", n=1),
        group("room", "room-kitchen", "Kitchen", kitchen, "kitchen", n=2),
        group("room", "room-hallway", "Hallway", hallway, "hallway", n=3),
        group("room", "room-bedroom", "Bedroom", [], "bedroom", with_light=False, n=4),
    ]
    zones = [
        group("zone", "zone-outside", "Outside", [ident("l-lantern", "light")], "garden", n=5),
        group("zone", "zone-down", "Downstairs",
              [ident(x, "light") for x in ("l-sofa", "l-ceiling", "l-strip", "l-spots")], "downstairs", n=6),
    ]
    home = group("bridge_home", "home", "", [ident(n, "room") for n in ("room-living", "room-kitchen", "room-hallway", "room-bedroom")]
                 + [ident(d, "device") for d in ("d-lantern", "d-contact", "d-chime", "d-dimmer", "d-future", "bridge")], n=0)
    grouped = [grouped_light("room-living", "room"), grouped_light("room-kitchen", "room"), grouped_light("room-hallway", "room"),
               grouped_light("zone-outside", "zone"), grouped_light("zone-down", "zone"), grouped_light("home", "bridge_home")]
    scenes = [
        scene("sc-relax-living", "Relax", "room-living", "room", ["l-sofa", "l-ceiling"] + (["l-plug"] if plug_scene else [])),
        scene("sc-relax-kitchen", "Relax", "room-kitchen", "room", ["l-strip", "l-spots"]),
        scene("sc-bright-down", "Bright", "zone-down", "zone", ["l-sofa", "l-ceiling", "l-strip", "l-spots"]),
        scene("sc-alarm", "Alarm flash", "room-hallway", "room", ["l-flood", "l-hall"]),
    ]
    others = [
        {"id": rid("br"), "type": "bridge", "bridge_id": BRIDGE_ID.lower(), "owner": ident("bridge", "device"),
         "time_zone": {"time_zone": "Europe/Amsterdam"}},
        simple("motion-1", "motion", "d-sensor", enabled=True,
               motion={"motion": False, "motion_valid": True,
                       "motion_report": {"changed": "2026-11-17T08:00:00.000Z", "motion": True}},
               sensitivity={"sensitivity": 2, "sensitivity_max": 4, "status": "set"}),
        simple("temp-1", "temperature", "d-sensor", enabled=True,
               temperature={"temperature": 22.55, "temperature_valid": True,
                            "temperature_report": {"changed": "2026-11-17T08:00:00.000Z", "temperature": 21.37}}),
        simple("ll-1", "light_level", "d-sensor", enabled=True,
               light={"light_level": 0, "light_level_valid": True}),
        simple("dp-1", "device_power", "d-sensor", power_state={"battery_level": 53, "battery_state": "normal"}),
        simple("dp-2", "device_power", "d-dimmer", power_state={"battery_level": 90, "battery_state": "normal"}),
        simple("btn-1", "button", "d-dimmer", metadata={"control_id": 1}, button={"button_report": {"event": "short_release"}}),
        simple("cm-1", "camera_motion", "d-camera", enabled=True, motion={"motion": True, "motion_valid": True}),
        simple("contact-1", "contact", "d-contact", enabled=True,
               contact_report={"changed": "2026-11-17T08:00:00.000Z", "state": "no_contact"}),
        simple("tamper-1", "tamper", "d-contact", tamper_reports=[]),
        simple("spk-1", "speaker", "d-chime", mute={"mute": "unmute"},
               alarm={"sound_values": ["siren", "no_sound"], "status": {"sound": "no_sound"}}),
        simple("zc-sofa", "zigbee_connectivity", "d-sofa", status="connected", mac_address="00:00:5e:00:53:10"),
        simple("fut-1", "zz_future_service", "d-future", whatever={"x": 1}),
        {"id": rid("bs-1"), "type": "behavior_script", "description": "Natural light",
         "configuration_schema": {"$ref": "x.json#"}, "metadata": {"name": "Natural light", "category": "automation"},
         "supported_features": [], "max_number_instances": 10},
        {"id": rid("ent-1"), "type": "entertainment_configuration", "metadata": {"name": "TV area"},
         "status": "inactive", "channels": []},
    ]
    resources = devices + lights + rooms + zones + [home] + grouped + scenes + others
    if no_camera:
        gone = {rid(x) for x in ("d-camera", "l-flood", "cm-1", "sc-alarm")}
        resources = [r for r in resources if r["id"] not in gone]
        for r in resources:
            if r["type"] == "room":
                r["children"] = [c for c in r["children"] if c["rid"] not in gone]
    return {r["id"]: r for r in resources}


# ---------------------------------------------------------------- server
class Bridge:
    def __init__(self, state_dir):
        self.dir = state_dir
        self.lock = threading.Lock()
        self.keys = set()
        self.flags = (False, False, False)
        self.res = make_resources()
        self.contexts = {}
        for name in ("a", "b", "rsa", "other", "san", "chain"):
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(os.path.join(CERTS, name + ".pem"), os.path.join(CERTS, name + ".key"))
            self.contexts[name] = ctx

    # control files
    def path(self, name):
        return os.path.join(self.dir, name)

    def control(self, name):
        try:
            with open(self.path(name)) as f:
                return f.read().strip()
        except OSError:
            return None

    def fault(self, req_path):
        f = self.control("fault")
        if not f:
            return None
        parts = f.split()
        if len(parts) > 1 and not req_path.startswith(parts[1]):
            return None
        return parts[0]

    def cert(self):
        name = self.control("cert") or "a"
        return self.contexts.get(name, self.contexts["a"])

    def take_busy(self):
        with self.lock:
            n = self.control("busy")
            try:
                n = int(n)
            except (TypeError, ValueError):
                return False
            if n <= 0:
                return False
            with open(self.path("busy"), "w") as f:
                f.write(str(n - 1))
            return True

    def log(self, entry):
        entry["t"] = time.monotonic()
        with self.lock:
            with open(self.path("requests.jsonl"), "a") as f:
                f.write(json.dumps(entry) + "\n")

    def sync_membership(self):
        flags = (os.path.exists(self.path("extra-room")), os.path.exists(self.path("no-camera")),
                 os.path.exists(self.path("plug-in-scene")))
        if flags != self.flags:
            self.res = make_resources(*flags)   # set before a test uses it: states start afresh
            self.flags = flags

    def config(self):
        c = {"name": "Hue Bridge", "datastoreversion": "172", "swversion": "1971060010", "apiversion": "1.71.0",
             "mac": MAC, "bridgeid": BRIDGE_ID, "factorynew": False, "replacesbridgeid": None,
             "modelid": "BSB002", "starterkitid": ""}
        extra = self.control("config.json")
        if extra:
            try:
                c.update(json.loads(extra))
            except ValueError:
                pass
        return c

    # members of a grouped_light
    def group_lights(self, gl):
        owner = gl["owner"]
        g = self.res.get(owner["rid"])
        if g is None:
            return []
        if g["type"] == "bridge_home":
            return [r for r in self.res.values() if r["type"] == "light"]
        out = []
        for c in g["children"]:
            if c["rtype"] == "light" and c["rid"] in self.res:
                out.append(self.res[c["rid"]])
            elif c["rtype"] == "device" and c["rid"] in self.res:
                for s in self.res[c["rid"]]["services"]:
                    if s["rtype"] == "light" and s["rid"] in self.res:
                        out.append(self.res[s["rid"]])
        return out

    def apply_light(self, l, body, group=False):
        """Validates and applies a light or grouped_light body. Returns (status, errors)."""
        errors = []
        if "on" in body:
            if not isinstance(body["on"], dict) or not isinstance(body["on"].get("on"), bool):
                return 400, ["invalid value for on"]
        for key in ("dimming", "color_temperature", "color"):
            if key in body and key not in l and not group:
                return 400, ["device (light) does not support %s" % key]
        if "dimming" in body:
            b = body["dimming"].get("brightness") if isinstance(body["dimming"], dict) else None
            if not isinstance(b, (int, float)) or isinstance(b, bool) or not 0 <= b <= 100:
                return 400, ["invalid value for dimming.brightness"]
        if "color_temperature" in body:
            m = body["color_temperature"].get("mirek") if isinstance(body["color_temperature"], dict) else None
            if not isinstance(m, int) or isinstance(m, bool) or not 153 <= m <= 500:
                if not (isinstance(m, int) and "color_temperature" in l and
                        l["color_temperature"]["mirek_schema"]["mirek_minimum"] <= m <= l["color_temperature"]["mirek_schema"]["mirek_maximum"]):
                    return 400, ["invalid value for color_temperature.mirek"]
        if "color" in body:
            xy = body["color"].get("xy") if isinstance(body["color"], dict) else None
            if not isinstance(xy, dict) or not all(isinstance(xy.get(k), (int, float)) and 0 <= xy.get(k) <= 1 for k in ("x", "y")):
                return 400, ["invalid value for color.xy"]
        unknown = set(body) - {"on", "dimming", "color_temperature", "color", "dynamics", "alert", "signaling"}
        if unknown:
            return 400, ["unsupported property %s" % sorted(unknown)[0]]
        if os.path.exists(self.path("unreachable")):
            errors.append("device (%s) has communication issues, command (.on.on) may not have effect" % ("grouped_light" if group else "light"))
            return 207, errors
        targets = self.group_lights(l) if group else [l]
        turning_on = isinstance(body.get("on"), dict) and body["on"].get("on") is True
        for t in targets:
            if not group and not t["on"]["on"] and not turning_on and any(k in body for k in ("dimming", "color", "color_temperature")):
                errors.append('device (light) is "soft off", command (.dimming.brightness) may not have effect')
                continue
            if "on" in body:
                t["on"]["on"] = body["on"]["on"]
            if "dimming" in body and "dimming" in t:
                t["dimming"]["brightness"] = max(t["dimming"].get("min_dim_level", 0.2), float(body["dimming"]["brightness"]))
            if "color_temperature" in body and "color_temperature" in t:
                sch = t["color_temperature"]["mirek_schema"]
                m = min(max(body["color_temperature"]["mirek"], sch["mirek_minimum"]), sch["mirek_maximum"])
                t["color_temperature"]["mirek"] = m
                t["color_temperature"]["mirek_valid"] = True
            if "color" in body and "color" in t:
                t["color"]["xy"] = {"x": body["color"]["xy"]["x"], "y": body["color"]["xy"]["y"]}
                if "color_temperature" in t:
                    t["color_temperature"]["mirek"] = None
                    t["color_temperature"]["mirek_valid"] = False
        return (207 if errors else 200), errors


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 10
    server_version = "nginx"
    sys_version = ""

    def log_message(self, *args):
        pass

    @property
    def bridge(self):
        return self.server.bridge

    def send_json(self, status, obj, raw=None):
        body = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self._status = status

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        data = self.rfile.read(min(n, 1 << 20)) if n > 0 else b""
        try:
            return data, json.loads(data) if data else None
        except ValueError:
            return data, ValueError

    def key_state(self):
        k = self.headers.get("hue-application-key")
        if k is None:
            return "none"
        if os.path.exists(self.bridge.path("revoke")):
            return "wrong"
        return "ok" if k in self.bridge.keys else "wrong"

    def misbehave(self, fault):
        """Carries out a fault that replaces the whole answer. Returns True when it did."""
        if fault == "silent-http":
            time.sleep(30)
            self.close_connection = True
            return True
        if fault == "garbage":
            self.send_json(200, None, raw=b"\x00\xffnot json {{{")
            return True
        if fault == "deep":
            self.send_json(200, None, raw=b"[" * 100000 + b"]" * 100000)
            return True
        if fault == "huge":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(10 * 1024 * 1024))
            self.end_headers()
            try:
                chunk = b" " * 65536
                for _ in range(160):
                    self.wfile.write(chunk)
            except OSError:
                pass
            self.close_connection = True
            self._status = 200
            return True
        if fault == "huge-length":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "100000000000")
            self.end_headers()
            try:
                self.wfile.write(b'{"data":[')
            except OSError:
                pass
            self.close_connection = True
            self._status = 200
            return True
        if fault == "truncated":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "1000")
            self.end_headers()
            self.wfile.write(b'{"errors":[],"data":[' + b" " * 79)
            self.close_connection = True
            self._status = 200
            return True
        if fault == "status-500":
            self.send_json(500, {"errors": [{"description": "Internal error"}], "data": []})
            return True
        if fault == "data-object":
            self.send_json(200, {"errors": [], "data": {"oops": 1}})
            return True
        return False

    def handle_one_request(self):
        self._status = 0
        super().handle_one_request()

    def finish_log(self, body=None):
        entry = {"method": self.command, "path": self.path, "key": self.key_state(), "status": self._status}
        if body is not None and body is not ValueError:
            entry["body"] = body
        self.bridge.log(entry)

    # ---- GET
    def do_GET(self):
        b = self.bridge
        fault = b.fault(self.path)
        try:
            if self.path.startswith("/api/") and self.path.endswith("/config"):
                # Swapped before answering: this connection keeps its certificate, the next gets the new one.
                swap = b.control("swap-after-config")
                if swap:
                    with open(b.path("cert"), "w") as f:
                        f.write(swap)
                    os.remove(b.path("swap-after-config"))
                if fault == "not-hue":
                    self.send_json(200, {"name": "router", "model": "x"})
                elif not self.misbehave(fault):
                    self.send_json(200, b.config())
                return
            if self.path.startswith("/clip/v2/"):
                if self.key_state() != "ok":
                    self.send_json(403, {"errors": [{"description": "unauthorized user"}], "data": []})
                    return
                if b.take_busy():
                    self.send_json(429, {"errors": [{"description": "Too many requests"}], "data": []})
                    return
                if self.misbehave(fault):
                    return
                self.serve_resource(fault)
                return
            self.send_json(404, {"errors": [{"description": "Not Found"}], "data": []})
        finally:
            self.finish_log()

    def serve_resource(self, fault):
        b = self.bridge
        parts = self.path.split("?")[0].strip("/").split("/")   # clip v2 resource [type [id]]
        with b.lock:
            b.sync_membership()
            items = list(b.res.values())
            if len(parts) >= 4:
                items = [r for r in items if r["type"] == parts[3]]
            if len(parts) >= 5:
                items = [r for r in items if r["id"] == parts[4]]
                if not items:
                    self.send_json(404, {"errors": [{"description": "Not Found"}], "data": []})
                    return
            if len(parts) > 5 or (len(parts) >= 4 and parts[3] not in {r["type"] for r in b.res.values()}
                                  and parts[3] not in ("motion", "temperature")):
                self.send_json(404, {"errors": [{"description": "Not Found"}], "data": []})
                return
            items = copy.deepcopy(items)
        if fault == "wrong-types":
            for r in items:
                if r["type"] == "light":
                    r["on"] = "yes"
                    r["metadata"] = 5
                    r["dimming"] = []
                elif r["type"] == "room":
                    r["children"] = {"rid": "x"}
                    r["metadata"] = {"name": ["a"]}
                elif r["type"] == "scene":
                    r["actions"] = "all"
                    r["group"] = None
                elif r["type"] == "device":
                    r["services"] = 42
            items.append(17)
            items.append(None)
        raw = json.dumps({"errors": [], "data": items}).encode()
        if fault == "huge-valid":
            raw = json.dumps({"errors": [], "data": items, "padding": "x" * (5 * 1024 * 1024)}).encode()
        if fault == "nonutf8":
            raw = raw.replace(b"Sofa lamp", b"Sofa \xff\xfe lamp").replace(b"Kitchen", b"Kit\xc0\x80chen")
        self.send_json(200, None, raw=raw)

    # ---- POST /api
    def do_POST(self):
        b = self.bridge
        raw, body = self.body()
        fault = b.fault(self.path)
        try:
            if self.path != "/api":
                self.send_json(404, {"errors": [{"description": "Not Found"}], "data": []})
                return
            if self.misbehave(fault):
                return
            if body is ValueError:
                self.send_json(200, [{"error": {"type": 2, "address": "", "description": "body contains invalid json"}}])
                return
            if not isinstance(body, dict) or not isinstance(body.get("devicetype"), str):
                self.send_json(200, [{"error": {"type": 5, "address": "/", "description": "invalid/missing parameters in body"}}])
                return
            if fault == "link-refused":
                self.send_json(200, [{"error": {"type": 7, "address": "/devicetype", "description": "invalid value"}}])
                return
            try:
                pressed = time.time() - os.path.getmtime(b.path("button")) < 30
            except OSError:
                pressed = False
            if not pressed:
                self.send_json(200, [{"error": {"type": 101, "address": "", "description": "link button not pressed"}}])
                return
            key = secrets.token_urlsafe(30)
            b.keys.add(key)
            self.send_json(200, [{"success": {"username": key}}])
        finally:
            self.finish_log(body)

    # ---- PUT
    def do_PUT(self):
        b = self.bridge
        raw, body = self.body()
        fault = b.fault(self.path)
        try:
            if not self.path.startswith("/clip/v2/resource/"):
                self.send_json(405, {"errors": [{"description": "Method Not Allowed"}], "data": []})
                return
            if self.key_state() != "ok":
                self.send_json(403, {"errors": [{"description": "unauthorized user"}], "data": []})
                return
            if b.take_busy():
                self.send_json(429, {"errors": [{"description": "Too many requests"}], "data": []})
                return
            if self.misbehave(fault):
                return
            parts = self.path.strip("/").split("/")
            if len(parts) != 5:
                self.send_json(405, {"errors": [{"description": "Method Not Allowed"}], "data": []})
                return
            rtype, rid_ = parts[3], parts[4]
            if body is ValueError or not isinstance(body, dict):
                self.send_json(400, {"errors": [{"description": "body contains invalid json"}], "data": []})
                return
            with b.lock:
                b.sync_membership()
                r = b.res.get(rid_)
                if r is None or r["type"] != rtype:
                    self.send_json(404, {"errors": [{"description": "Not Found"}], "data": []})
                    return
                if rtype in ("light", "grouped_light"):
                    status, errors = b.apply_light(r, body, group=rtype == "grouped_light")
                elif rtype == "scene":
                    if body != {"recall": {"action": "active"}}:
                        status, errors = 400, ["invalid value for recall"]
                    else:
                        for a in r["actions"]:
                            t = b.res.get(a["target"]["rid"])
                            if t:
                                t["on"]["on"] = a["action"]["on"]["on"]
                                if "dimming" in t:
                                    t["dimming"]["brightness"] = a["action"]["dimming"]["brightness"]
                        r["status"]["active"] = "static"
                        status, errors = 200, []
                else:
                    status, errors = 405, ["Method Not Allowed"]
            data = [] if status in (400, 405) else [{"rid": rid_, "rtype": rtype}]
            self.send_json(status, {"errors": [{"description": e} for e in errors], "data": data})
        finally:
            self.finish_log(body)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, bridge):
        self.bridge = bridge
        super().__init__(("127.0.0.1", 0), Handler)

    def finish_request(self, request, client_address):
        if self.bridge.fault("") == "silent":
            time.sleep(30)          # accepted, and nothing more ever happens
            return
        request.settimeout(10)
        try:
            tls = self.bridge.cert().wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError):
            return
        try:
            self.RequestHandlerClass(tls, client_address, self)
        except (OSError, ssl.SSLError):
            pass
        finally:
            try:
                tls.close()
            except OSError:
                pass

    def handle_error(self, request, client_address):
        pass


def serve(state_dir, max_seconds):
    os.makedirs(state_dir, exist_ok=True)
    bridge = Bridge(state_dir)
    server = Server(bridge)
    port = server.server_address[1]

    def stop(*_):
        os._exit(0)

    signal.signal(signal.SIGTERM, stop)
    threading.Thread(target=lambda: (sys.stdin.read(), stop()), daemon=True).start()
    threading.Timer(max_seconds, stop).start()
    print("PORT %d" % port, flush=True)
    server.serve_forever(poll_interval=0.2)


# ---------------------------------------------------------------- self-test
def selftest():
    import tempfile
    results = []

    def check(ok, what):
        results.append(ok)
        print("%s %s" % ("ok  " if ok else "FAIL", what))

    d = tempfile.mkdtemp(prefix="mock_hue_")
    bridge = Bridge(d)
    server = Server(bridge)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.1}, daemon=True).start()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    def call(method, path, body=None, key=None):
        c = http.client.HTTPSConnection("127.0.0.1", port, context=ctx, timeout=5)
        headers = {"Content-Type": "application/json"}
        if key:
            headers["hue-application-key"] = key
        c.request(method, path, body=None if body is None else json.dumps(body), headers=headers)
        r = c.getresponse()
        data = r.read()
        der = c.sock.getpeercert(binary_form=True)
        c.close()
        return r.status, json.loads(data), der

    st, cfg, der_a = call("GET", "/api/0/config")
    check(st == 200 and cfg["bridgeid"] == BRIDGE_ID and cfg["swversion"] == "1971060010", "GET /api/0/config")
    st, rep, _ = call("POST", "/api", {"devicetype": "zero_bot#test"})
    check(rep[0]["error"]["type"] == 101, "POST /api before the button: error 101")
    open(os.path.join(d, "button"), "w").close()
    st, rep, _ = call("POST", "/api", {"devicetype": "zero_bot#test"})
    key = rep[0].get("success", {}).get("username", "")
    check(len(key) >= 32, "POST /api after the button: a key")
    st, rep, _ = call("POST", "/api", {"nope": 1})
    check(rep[0]["error"]["type"] == 5, "POST /api without devicetype: error 5")
    st, rep, _ = call("GET", "/clip/v2/resource/light")
    check(st == 403, "no key: 403")
    st, rep, _ = call("GET", "/clip/v2/resource/light", key="wrong")
    check(st == 403, "wrong key: 403")
    st, rep, _ = call("GET", "/clip/v2/resource/light", key=key)
    check(st == 200 and len(rep["data"]) == 9 and rep["errors"] == [], "GET light: 9 lights")
    st, rep, _ = call("GET", "/clip/v2/resource", key=key)
    types = {r["type"] for r in rep["data"]}
    check({"device", "light", "room", "zone", "scene", "grouped_light", "bridge_home", "bridge", "motion",
           "temperature", "camera_motion", "speaker", "zz_future_service"} <= types, "GET resource: every type")
    st, rep, _ = call("GET", "/clip/v2/resource/light/" + rid("nothing"), key=key)
    check(st == 404, "unknown id: 404")
    sofa = rid("l-sofa")
    st, rep, _ = call("PUT", "/clip/v2/resource/light/" + sofa, {"on": {"on": False}}, key=key)
    check(st == 200 and rep["data"] == [{"rid": sofa, "rtype": "light"}], "PUT light: data names it")
    check(bridge.res[sofa]["on"]["on"] is False, "PUT light: switched off")
    st, rep, _ = call("PUT", "/clip/v2/resource/light/" + sofa, {"dimming": {"brightness": 20}}, key=key)
    check(st == 207 and "soft off" in rep["errors"][0]["description"], "dimming a light that is off: 207 soft off")
    st, rep, _ = call("PUT", "/clip/v2/resource/light/" + rid("l-hall"), {"on": {"on": True}, "color": {"xy": {"x": 0.3, "y": 0.3}}}, key=key)
    check(st == 400, "colour to a white light: 400")
    st, rep, _ = call("PUT", "/clip/v2/resource/grouped_light/" + rid("gl-room-kitchen"),
                      {"on": {"on": True}, "color_temperature": {"mirek": 400}}, key=key)
    check(st == 200 and bridge.res[rid("l-spots")]["color_temperature"]["mirek"] == 400 and
          bridge.res[rid("l-strip")]["on"]["on"] is True, "PUT grouped_light reaches the room's lights")
    st, rep, _ = call("PUT", "/clip/v2/resource/scene/" + rid("sc-relax-living"), {"recall": {"action": "active"}}, key=key)
    check(st == 200 and bridge.res[sofa]["on"]["on"] is True and bridge.res[rid("sc-relax-living")]["status"]["active"] == "static",
          "PUT scene recall applies its actions")
    with open(os.path.join(d, "busy"), "w") as f:
        f.write("1")
    st, rep, _ = call("GET", "/clip/v2/resource/light", key=key)
    st2, rep2, _ = call("GET", "/clip/v2/resource/light", key=key)
    check(st == 429 and st2 == 200, "busy: one 429, then served")
    with open(os.path.join(d, "cert"), "w") as f:
        f.write("b")
    st, cfg, der_b = call("GET", "/api/0/config")
    check(der_a != der_b, "the cert file switches the certificate")
    os.remove(os.path.join(d, "cert"))
    open(os.path.join(d, "revoke"), "w").close()
    st, rep, _ = call("GET", "/clip/v2/resource/light", key=key)
    check(st == 403, "revoke: the key is refused")
    os.remove(os.path.join(d, "revoke"))
    open(os.path.join(d, "extra-room"), "w").close()
    st, rep, _ = call("GET", "/clip/v2/resource/room/" + rid("room-kitchen"), key=key)
    check(len(rep["data"][0]["children"]) == 3, "extra-room: the Kitchen gains a device")
    with open(os.path.join(d, "fault"), "w") as f:
        f.write("data-object /clip/v2/resource/light")
    st, rep, _ = call("GET", "/clip/v2/resource/light", key=key)
    st2, rep2, _ = call("GET", "/clip/v2/resource/room", key=key)
    check(isinstance(rep["data"], dict) and isinstance(rep2["data"], list), "a fault applies only under its path")
    with open(os.path.join(d, "requests.jsonl")) as f:
        log = [json.loads(line) for line in f]
    check(all("username" not in json.dumps(e) for e in log) and not any(key in json.dumps(e) for e in log),
          "the request log never holds a key")
    server.shutdown()
    server.server_close()
    import shutil
    shutil.rmtree(d, ignore_errors=True)
    failed = results.count(False)
    print("%d checks, %d failed" % (len(results), failed))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--state")
    ap.add_argument("--max-seconds", type=float, default=600)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--dump", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    if a.dump:
        print(json.dumps(list(make_resources().values()), indent=1))
        return
    if not a.state:
        ap.error("--state is required")
    serve(a.state, a.max_seconds)


if __name__ == "__main__":
    main()
