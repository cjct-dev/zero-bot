#!/usr/bin/env python3
"""A pretend Tapo plug for testing tapo.cpp. Python standard library only.

Used by tests/tapo_e2e.py (the app end to end); it was first written beside the Tapo module's own
known-answer tests.

It plays the device side of both logins for one known account and answers a few commands:

  KLAP v2   /app/handshake1, /app/handshake2, /app/request?seq=N       (AES-128-CBC)
  TPAP      POST / {"method":"login",...}: discover, pake_register, pake_share
            (SPAKE2+ verifier, RFC 9383, P-256), then /stok=<token>/ds  (AES-128-CCM)

Written from the RFCs and the protocol description, not from tapo.cpp: AES, CBC, CCM and the
P-256 arithmetic are implemented here in plain Python, so that the C++ (which uses OpenSSL) and
this file agreeing means two independent implementations agree. `--selftest` checks those parts
against published vectors.

Test hooks (never present on a real plug): GET /mock/stats, and POST /mock/reset with any of
{"budget":[1,0], "lock_after":1, "no_energy_usage":true, "page":3, "refuse_set":true}, which also clears all
state. The budget is how many requests each successive session serves before the plug forgets it;
refuse_set answers every switch command with error -1001 (the plug says no), for tests of what the
app does after a command fails.

It binds 127.0.0.1 only.
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import socket
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --------------------------------------------------------------------------------------------
# AES-128 (FIPS 197), CBC with PKCS#7, CCM (RFC 3610)
# --------------------------------------------------------------------------------------------

def _rotl8(x, n):
    return ((x << n) | (x >> (8 - n))) & 0xFF


def _make_sbox():
    sbox = [0] * 256
    p = q = 1
    while True:
        p = (p ^ (p << 1) ^ (0x1B if p & 0x80 else 0)) & 0xFF          # p * 3
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09                                                   # q / 3
        sbox[p] = q ^ _rotl8(q, 1) ^ _rotl8(q, 2) ^ _rotl8(q, 3) ^ _rotl8(q, 4) ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


SBOX = _make_sbox()
INV_SBOX = [0] * 256
for _i, _v in enumerate(SBOX):
    INV_SBOX[_v] = _i


def _gmul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1
        b >>= 1
    return r


class AES128:
    def __init__(self, key):
        assert len(key) == 16
        w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
        rcon = 1
        for i in range(4, 44):
            t = list(w[i - 1])
            if i % 4 == 0:
                t = [SBOX[b] for b in t[1:] + t[:1]]
                t[0] ^= rcon
                rcon = _gmul(rcon, 2)
            w.append([a ^ b for a, b in zip(w[i - 4], t)])
        self.rk = [sum(w[4 * r:4 * r + 4], []) for r in range(11)]

    def encrypt(self, block):
        s = [b ^ k for b, k in zip(block, self.rk[0])]
        for rnd in range(1, 11):
            s = [SBOX[b] for b in s]
            s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]                      # ShiftRows
            if rnd < 10:
                m = []
                for c in range(0, 16, 4):
                    a = s[c:c + 4]
                    m += [_gmul(a[i], 2) ^ _gmul(a[(i + 1) % 4], 3) ^ a[(i + 2) % 4] ^ a[(i + 3) % 4] for i in range(4)]
                s = m
            s = [b ^ k for b, k in zip(s, self.rk[rnd])]
        return bytes(s)

    def decrypt(self, block):
        s = [b ^ k for b, k in zip(block, self.rk[10])]
        for rnd in range(9, -1, -1):
            s = [s[(i - 4 * (i % 4)) % 16] for i in range(16)]                      # InvShiftRows
            s = [INV_SBOX[b] for b in s]
            s = [b ^ k for b, k in zip(s, self.rk[rnd])]
            if rnd > 0:
                m = []
                for c in range(0, 16, 4):
                    a = s[c:c + 4]
                    m += [_gmul(a[i], 14) ^ _gmul(a[(i + 1) % 4], 11) ^ _gmul(a[(i + 2) % 4], 13) ^ _gmul(a[(i + 3) % 4], 9)
                          for i in range(4)]
                s = m
        return bytes(s)


def _xor(a, b):
    return bytes(x ^ y for x, y in zip(a, b))


def cbc_encrypt(key, iv, plain):
    aes = AES128(key)
    pad = 16 - len(plain) % 16
    plain += bytes([pad]) * pad
    out, prev = b"", iv
    for i in range(0, len(plain), 16):
        prev = aes.encrypt(_xor(plain[i:i + 16], prev))
        out += prev
    return out


def cbc_decrypt(key, iv, data):
    if not data or len(data) % 16:
        raise ValueError("ciphertext is not whole blocks")
    aes = AES128(key)
    out, prev = b"", iv
    for i in range(0, len(data), 16):
        out += _xor(aes.decrypt(data[i:i + 16]), prev)
        prev = data[i:i + 16]
    pad = out[-1]
    if not 1 <= pad <= 16 or out[-pad:] != bytes([pad]) * pad:
        raise ValueError("bad padding")
    return out[:-pad]


def _ccm(key, nonce, aad, text, tag_len, sealing):
    """Returns (output text, tag over the plaintext)."""
    aes = AES128(key)
    ell = 15 - len(nonce)

    def ctr_block(i):
        return aes.encrypt(bytes([ell - 1]) + nonce + i.to_bytes(ell, "big"))

    out = b""
    for i in range(0, len(text), 16):
        out += _xor(text[i:i + 16], ctr_block(i // 16 + 1))
    plain = text if sealing else out

    flags = (0x40 if aad else 0) | ((tag_len - 2) // 2) << 3 | (ell - 1)
    blocks = bytes([flags]) + nonce + len(plain).to_bytes(ell, "big")
    if aad:
        a = struct.pack(">H", len(aad)) + aad
        blocks += a + bytes(-len(a) % 16)
    blocks += plain + bytes(-len(plain) % 16)
    mac = bytes(16)
    for i in range(0, len(blocks), 16):
        mac = aes.encrypt(_xor(mac, blocks[i:i + 16]))
    return out, _xor(mac, ctr_block(0))[:tag_len]


def ccm_seal(key, nonce, aad, plain, tag_len=16):
    out, tag = _ccm(key, nonce, aad, plain, tag_len, True)
    return out + tag


def ccm_open(key, nonce, aad, sealed, tag_len=16):
    if len(sealed) < tag_len:
        raise ValueError("too short")
    plain, tag = _ccm(key, nonce, aad, sealed[:-tag_len], tag_len, False)
    if not hmac.compare_digest(tag, sealed[-tag_len:]):
        raise ValueError("tag mismatch")
    return plain


# --------------------------------------------------------------------------------------------
# P-256 and the SPAKE2+ verifier (RFC 9383)
# --------------------------------------------------------------------------------------------

P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
ORDER = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
CURVE_B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def pt_add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0]:
        if (a[1] + b[1]) % P == 0:
            return None
        lam = (3 * a[0] * a[0] - 3) * pow(2 * a[1], -1, P) % P
    else:
        lam = (b[1] - a[1]) * pow(b[0] - a[0], -1, P) % P
    x = (lam * lam - a[0] - b[0]) % P
    return x, (lam * (a[0] - x) - a[1]) % P


def pt_neg(a):
    return None if a is None else (a[0], -a[1] % P)


def pt_mul(k, a):
    r = None
    while k:
        if k & 1:
            r = pt_add(r, a)
        a = pt_add(a, a)
        k >>= 1
    return r


def pt_decode(data):
    """SEC1 to a point; raises for anything that is not a point of the curve."""
    if len(data) == 65 and data[0] == 4:
        x, y = int.from_bytes(data[1:33], "big"), int.from_bytes(data[33:], "big")
    elif len(data) == 33 and data[0] in (2, 3):
        x = int.from_bytes(data[1:], "big")
        y = pow((x * x * x - 3 * x + CURVE_B) % P, (P + 1) // 4, P)
        if y & 1 != data[0] & 1:
            y = P - y
    else:
        raise ValueError("not a SEC1 point")
    if x >= P or y >= P or (y * y - (x * x * x - 3 * x + CURVE_B)) % P:
        raise ValueError("not on the curve")
    return x, y


def pt_encode(a):
    return b"\x04" + a[0].to_bytes(32, "big") + a[1].to_bytes(32, "big")


M_POINT = pt_decode(bytes.fromhex("02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f"))
N_POINT = pt_decode(bytes.fromhex("03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49"))


def hkdf(ikm, salt, info, length):
    prk = hmac.new(salt or bytes(32), ikm, hashlib.sha256).digest()
    okm, t, i = b"", b"", 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        okm += t
        i += 1
    return okm[:length]


class Verifier:
    """The device's side: it holds w0 and L = w1*G, never w1 or the password."""

    def __init__(self, w0, big_l, context, id_prover=b"", id_verifier=b"", y=None):
        self.w0, self.big_l, self.context = w0, big_l, context
        self.ids = (id_prover, id_verifier)
        self.y = y or (int.from_bytes(os.urandom(40), "big") % (ORDER - 1) + 1)
        self.share = pt_encode(pt_add(pt_mul(self.y, G), pt_mul(w0, N_POINT)))       # Y = y*G + w0*N

    def finish(self, prover_share, prover_confirm):
        """Returns (device confirmation, shared key), or None when the prover does not know the password."""
        try:
            x = pt_decode(prover_share)
        except ValueError:
            return None
        unmasked = pt_add(x, pt_neg(pt_mul(self.w0, M_POINT)))                       # X - w0*M
        z, v = pt_mul(self.y, unmasked), pt_mul(self.y, self.big_l)
        if z is None or v is None:
            return None
        tt = b""
        for item in (self.context, self.ids[0], self.ids[1], pt_encode(M_POINT), pt_encode(N_POINT), pt_encode(x),
                     self.share, pt_encode(z), pt_encode(v), self.w0.to_bytes(32, "big")):
            tt += struct.pack("<Q", len(item)) + item
        main = hashlib.sha256(tt).digest()
        keys = hkdf(main, b"", b"ConfirmationKeys", 64)
        if not hmac.compare_digest(hmac.new(keys[:32], self.share, hashlib.sha256).digest(), prover_confirm):
            return None
        return hmac.new(keys[32:], pt_encode(x), hashlib.sha256).digest(), hkdf(main, b"", b"SharedKey", 32)


# --------------------------------------------------------------------------------------------
# The pretend device
# --------------------------------------------------------------------------------------------

def b64(text):
    return base64.b64encode(text.encode()).decode()


class Mock:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.auth_hash = hashlib.sha256(hashlib.sha1(args.email.encode()).digest() +
                                        hashlib.sha1(args.password.encode()).digest()).digest()
        self.credential = hashlib.sha1(args.password.encode()).hexdigest().encode()   # password_shadow id 2
        self.salt = os.urandom(16)
        self.udp_probes = 0
        self.reset({})

    def reset(self, cfg):
        self.budget = cfg.get("budget") or []
        self.lock_after = cfg.get("lock_after", self.args.lock_after)
        self.no_energy_usage = cfg.get("no_energy_usage", self.args.no_energy_usage)
        self.page = cfg.get("page", self.args.page)
        self.refuse_set = cfg.get("refuse_set", False)
        self.sessions = {}       # cookie or token -> session dict
        self.pending = None      # TPAP: the handshake between pake_register and pake_share
        self.failed_logins = 0
        self.stats = {k: 0 for k in ("discover", "handshake1", "handshake2", "pake_register", "pake_share",
                                     "login_ok", "login_rejected", "requests", "gone")}
        self.methods, self.bad = [], []
        self.on = False
        self.children = [{"device_id": "8000000000000000000000000000000000000304%02d" % i, "position": i + 1,
                          "nickname": b64("Socket %d" % (i + 1)), "device_on": i == 1,
                          "category": "plug.powerstrip.sub-plug", "model": "P304M", "is_usb": False}
                         for i in range(4)]

    def snapshot(self):
        d = dict(self.stats)
        d.update(methods=self.methods, bad=self.bad, udp_probes=self.udp_probes, on=self.on,
                 children_on=[c["device_on"] for c in self.children], sessions=len(self.sessions))
        return d

    def new_session(self, sid, **fields):
        n = self.stats["login_ok"]
        left = self.budget[min(n, len(self.budget) - 1)] if self.budget else None
        self.sessions[sid] = dict(fields, left=left)
        self.stats["login_ok"] += 1

    def spend(self, sid):
        """The session for one more request, or None when the plug does not (or no longer) know it."""
        s = self.sessions.get(sid)
        if s is not None and s.get("confirmed", True) and s["left"] != 0:
            if s["left"] is not None:
                s["left"] -= 1
            return s
        self.sessions.pop(sid, None)
        self.stats["gone"] += 1
        return None

    # --- commands ---------------------------------------------------------------------------

    def command(self, req, child=None):
        method = req.get("method")
        params = req.get("params") or {}
        strip = self.args.model == "p304m"
        target = child if child is not None else self
        if method == "get_device_info":
            if child is not None:
                return 0, child
            info = {"device_id": "8000000000000000000000000000000000000110", "fw_ver": "1.4.8 Build 260804 Rel.045837",
                    "hw_ver": "1.0", "mac": self.args.mac, "model": "P304M" if strip else "P110",
                    "nickname": b64(self.args.nickname or ("Mock Strip" if strip else "Mock Plug ☕")),
                    "ssid": base64.b64encode(b"Mock Wi-Fi \xff").decode(),      # not UTF-8: must not break the client
                    "rssi": -51, "type": "SMART.TAPOPLUG"}
            if not strip:
                info["device_on"] = self.on
            return 0, info
        if method == "set_device_info":
            if self.refuse_set:
                return -1001, None
            if (strip and child is None) or not isinstance(params.get("device_on"), bool):
                return -1008, None
            if child is not None:
                child["device_on"] = params["device_on"]
            else:
                self.on = params["device_on"]
            return 0, {}
        if method == "get_child_device_list":
            if not strip:
                return -1002, None
            start = params.get("start_index", 0)
            page = self.children[start:start + self.page]
            return 0, {"child_device_list": page, "start_index": start, "sum": len(self.children)}
        if method == "control_child":
            inner = params.get("requestData") or {}
            kid = next((c for c in self.children if c["device_id"] == params.get("device_id")), None)
            if not strip or kid is None or inner.get("method") != "multipleRequest":
                return -1008, None
            responses = []
            for r in (inner.get("params") or {}).get("requests") or []:
                code, result = self.command(r, kid)
                responses.append({"method": r.get("method"), "error_code": code, "result": result or {}})
            return 0, {"responseData": {"result": {"responses": responses}}}
        is_on = target["device_on"] if child is not None else self.on
        if strip and child is None:
            return -1002, None
        if method == "get_energy_usage" and not self.no_energy_usage:
            usage = {"today_energy": 321, "month_energy": 4567, "today_runtime": 12, "month_runtime": 345,
                     "local_time": "2026-11-17 12:00:00", "electricity_charge": [0, 0, 0]}
            if child is None:                # a strip's socket leaves the live power out
                usage["current_power"] = 23456 if is_on else 0
            return 0, usage
        if method == "get_current_power" and not self.no_energy_usage:
            return 0, {"current_power": 23 if is_on else 0}
        if method == "get_emeter_data":
            return 0, {"current_ma": 98, "voltage_mv": 240695, "power_mw": 23456 if is_on else 0, "energy_wh": 8725}
        if method == "get_device_usage":
            return 0, {"power_usage": {"today": 321, "past7": 2000, "past30": 9000}, "time_usage": {"today": 12}}
        if method == "multipleRequest":
            self.bad.append("top-level multipleRequest")
            return -1008, None
        return -1002, None

    def answer(self, text):
        try:
            req = json.loads(text)
            assert isinstance(req, dict)
        except (ValueError, AssertionError):
            self.bad.append("request is not a JSON object")
            return json.dumps({"error_code": -1003})
        self.stats["requests"] += 1
        self.methods.append(req.get("method"))
        code, result = self.command(req)
        reply = {"error_code": code}
        if result is not None:
            reply["result"] = result
        return json.dumps(reply)

    # --- KLAP -------------------------------------------------------------------------------

    def klap(self, path, query, cookie, body):
        """Returns (status, headers, body)."""
        if self.args.mode != "klap":
            return 403, {}, b""          # a plug in TPAP mode turns KLAP down like this
        if cookie is not None and "TIMEOUT" in cookie:
            self.bad.append("the TIMEOUT cookie was sent back")
            return 400, {}, b""
        sid = cookie.split("=", 1)[1] if cookie and cookie.startswith("TP_SESSIONID=") else None
        if path == "/app/handshake1":
            self.stats["handshake1"] += 1
            if len(body) != 16:
                self.bad.append("handshake1 seed is not 16 bytes")
                return 400, {}, b""
            remote = os.urandom(16)
            seeds = body + remote + self.auth_hash
            ivh = hashlib.sha256(b"iv" + seeds).digest()
            sid = os.urandom(16).hex().upper()
            for k in [k for k, s in self.sessions.items() if not s["confirmed"]]:
                del self.sessions[k]     # one handshake at a time
            self.sessions[sid] = {"confirmed": False, "left": None, "local": body, "remote": remote,
                                  "key": hashlib.sha256(b"lsk" + seeds).digest()[:16], "iv": ivh[:12],
                                  "seq": struct.unpack(">i", ivh[28:])[0],
                                  "sig": hashlib.sha256(b"ldk" + seeds).digest()[:28]}
            return 200, {"Set-Cookie": "TP_SESSIONID=%s;TIMEOUT=1440" % sid}, remote + hashlib.sha256(seeds).digest()
        if path == "/app/handshake2":
            self.stats["handshake2"] += 1
            s = self.sessions.get(sid)
            if s is None or s["confirmed"]:
                return 403, {}, b""
            if hmac.compare_digest(body, hashlib.sha256(s["remote"] + s["local"] + self.auth_hash).digest()):
                del self.sessions[sid]
                self.new_session(sid, **{k: v for k, v in s.items() if k != "left"})
                self.sessions[sid]["confirmed"] = True
                return 200, {}, b""
            del self.sessions[sid]
            self.stats["login_rejected"] += 1
            return 403, {}, b""
        if path == "/app/request":
            s = self.spend(sid)
            if s is None:
                return 403, {}, b""
            try:
                seq = int(query.split("seq=", 1)[1])
            except (IndexError, ValueError):
                self.bad.append("request without seq")
                return 400, {}, b""
            want = (s["seq"] + 1 + 2 ** 31) % 2 ** 32 - 2 ** 31
            seqb = struct.pack(">i", seq)
            if seq != want:
                self.bad.append("sequence number %d, expected %d" % (seq, want))
                return 400, {}, b""
            if len(body) < 48 or hashlib.sha256(s["sig"] + seqb + body[32:]).digest() != body[:32]:
                self.bad.append("bad request signature")
                return 400, {}, b""
            s["seq"] = seq
            try:
                text = cbc_decrypt(s["key"], s["iv"] + seqb, body[32:]).decode()
            except ValueError:
                self.bad.append("request does not decrypt")
                return 400, {}, b""
            ct = cbc_encrypt(s["key"], s["iv"] + seqb, self.answer(text).encode())
            return 200, {}, hashlib.sha256(s["sig"] + seqb + ct).digest() + ct
        return 404, {}, b""

    # --- TPAP -------------------------------------------------------------------------------

    def tpap_info(self):
        return {"tls": 0, "dac": 0, "noc": 0, "pake": [2], "port": self.args.port}

    def login(self, body):
        try:
            params = json.loads(body)["params"]
            sub = params["sub_method"]
        except (ValueError, KeyError, TypeError):
            return {"error_code": -1003}
        locked = {"error_code": -2101, "error_info": {"lockedMinute": 30, "remainAttempts": 0}}
        is_locked = self.lock_after and self.failed_logins >= self.lock_after
        if sub == "discover":
            self.stats["discover"] += 1
            if is_locked:
                return locked
            if self.args.mode != "tpap":
                return {"error_code": 0, "result": {"sub_method": "discover", "tpap_preferred": False}}
            return {"error_code": 0, "result": {"sub_method": "discover", "tpap_preferred": True,
                                                "mac": self.args.mac.replace("-", ""), "tpap": self.tpap_info()}}
        if self.args.mode != "tpap":
            return {"error_code": -1002}
        if sub == "pake_register":
            self.stats["pake_register"] += 1
            if is_locked:
                return locked
            if params.get("username") != hashlib.md5(b"admin").hexdigest():
                self.bad.append("pake_register user is not md5(admin)")
                return {"error_code": -1008}
            if 1 not in (params.get("cipher_suites") or []) or "aes_128_ccm" not in (params.get("encryption") or []):
                return {"error_code": -2201}
            if params.get("passcode_type") != "userpw":
                return {"error_code": -2202}
            try:
                user_random = base64.b64decode(params["user_random"], validate=True)
                assert len(user_random) == 32
            except (KeyError, ValueError, AssertionError):
                self.bad.append("pake_register user_random is not 32 bytes of base64")
                return {"error_code": -1008}
            iterations = 3000
            d = hashlib.pbkdf2_hmac("sha256", self.credential, self.salt, iterations, 80)
            w0, w1 = int.from_bytes(d[:40], "big") % ORDER, int.from_bytes(d[40:], "big") % ORDER
            dev_random = os.urandom(32)
            context = hashlib.sha256(b"PAKE V1" + user_random + dev_random).digest()
            self.pending = Verifier(w0, pt_mul(w1, G), context)
            result = {"sub_method": "pake_register", "dev_random": base64.b64encode(dev_random).decode(),
                      "dev_salt": base64.b64encode(self.salt).decode(),
                      "dev_share": base64.b64encode(self.pending.share).decode(),
                      "cipher_suites": self.args.suite, "iterations": iterations, "encryption": "aes_128_ccm",
                      "extra_crypt": {"type": "password_shadow", "params": {"passwd_id": self.args.passwd_id}}}
            return {"error_code": 0, "result": result}
        if sub == "pake_share":
            self.stats["pake_share"] += 1
            if is_locked:
                return locked
            verifier, self.pending = self.pending, None
            if verifier is None:
                self.bad.append("pake_share without pake_register")
                return {"error_code": -1008}
            try:
                done = verifier.finish(base64.b64decode(params["user_share"], validate=True),
                                       base64.b64decode(params["user_confirm"], validate=True))
            except (KeyError, ValueError):
                done = None
            if done is None:
                self.stats["login_rejected"] += 1
                self.failed_logins += 1
                return {"error_code": -2203}
            confirm, shared = done
            token = os.urandom(16).hex()
            start_seq = struct.unpack(">i", os.urandom(4))[0]
            self.new_session(token, seq=start_seq,
                             key=hkdf(shared, b"tp-kdf-salt-aes128-key", b"tp-kdf-info-aes128-key", 16),
                             nonce=hkdf(shared, b"tp-kdf-salt-aes128-iv", b"tp-kdf-info-aes128-iv", 12))
            return {"error_code": 0, "result": {"sub_method": "pake_share", "dev_confirm": base64.b64encode(confirm).decode(),
                                                "stok": token, "start_seq": start_seq, "expired": 604800}}
        return {"error_code": -1002}

    def tpap_request(self, token, body):
        """Returns (content type, body). A plug that does not know the session says so in plain JSON."""
        s = self.spend(token)
        if s is None:
            return "application/json", json.dumps({"error_code": -2402}).encode()
        if len(body) < 20 or struct.unpack(">i", body[:4])[0] != s["seq"]:
            self.bad.append("TPAP sequence number is not the expected %d" % s["seq"])
            return "application/json", json.dumps({"error_code": -1008}).encode()
        nonce = s["nonce"][:8] + body[:4]
        try:
            text = ccm_open(s["key"], nonce, b"", body[4:]).decode()
        except ValueError:
            self.bad.append("TPAP request does not decrypt")
            return "application/json", json.dumps({"error_code": -1008}).encode()
        s["seq"] = (s["seq"] + 1 + 2 ** 31) % 2 ** 32 - 2 ** 31
        return "application/octet-stream", body[:4] + ccm_seal(s["key"], nonce, b"", self.answer(text).encode())


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    mock = None

    def log_message(self, fmt, *args):
        if self.mock.args.verbose:
            sys.stderr.write("mock: " + fmt % args + "\n")

    def send(self, status, body, ctype="application/octet-stream", headers=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        if self.path == "/mock/stats":
            with self.mock.lock:
                self.send(200, json.dumps(self.mock.snapshot()).encode(), "application/json")
        else:
            self.send(404, b"")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        path, _, query = self.path.partition("?")
        m = self.mock
        with m.lock:
            if path == "/mock/reset":
                m.reset(json.loads(body or b"{}"))
                self.send(200, b"{}", "application/json")
            elif path == "/":
                self.send(200, json.dumps(m.login(body)).encode(), "application/json")
            elif path.startswith("/app/"):
                status, headers, out = m.klap(path, query, self.headers.get("Cookie"), body)
                self.send(status, out, headers=headers)
            elif path.startswith("/stok=") and path.endswith("/ds") and m.args.mode == "tpap":
                ctype, out = m.tpap_request(path[6:-3], body)
                self.send(200, out, ctype)
            else:
                self.send(404, b"")


# --------------------------------------------------------------------------------------------
# Self-test against published vectors
# --------------------------------------------------------------------------------------------

def selftest():
    h = bytes.fromhex
    failed = 0

    def check(name, ok):
        nonlocal failed
        failed += not ok
        print("%s  mock: %s" % ("ok  " if ok else "FAIL", name))

    k = AES128(bytes(range(16)))
    ct = k.encrypt(h("00112233445566778899aabbccddeeff"))
    check("AES-128 block encrypt (FIPS 197 C.1)", ct == h("69c4e0d86a7b0430d8cdb78070b4c55a"))
    check("AES-128 block decrypt (FIPS 197 C.1)", k.decrypt(ct) == h("00112233445566778899aabbccddeeff"))
    key, iv = h("2b7e151628aed2a6abf7158809cf4f3c"), bytes(range(16))
    plain = h("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51")
    ct = cbc_encrypt(key, iv, plain)
    check("AES-128-CBC (NIST SP 800-38A F.2.1)", ct[:32] == h("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2")
          and cbc_decrypt(key, iv, ct) == plain)
    key, nonce = h("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"), h("00000003020100a0a1a2a3a4a5")
    sealed = ccm_seal(key, nonce, bytes(range(8)), bytes(range(8, 31)), 8)
    check("AES-128-CCM (RFC 3610 packet vector 1)", sealed == h("588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0")
          and ccm_open(key, nonce, bytes(range(8)), sealed, 8) == bytes(range(8, 31)))

    shared = bytes(range(32))
    key = hkdf(shared, b"tp-kdf-salt-aes128-key", b"tp-kdf-info-aes128-key", 16)
    base = hkdf(shared, b"tp-kdf-salt-aes128-iv", b"tp-kdf-info-aes128-iv", 12)
    seq = h("01020304")
    body = seq + ccm_seal(key, base[:8] + seq, b"", b'{"method":"get_device_info"}')
    check("TPAP request body (tapo crate, tpap_cipher.rs)", body.hex() ==
          "01020304c0331c1c36a5df4ce0c68e5a27b2667ebf842dc994a3fd24b9e860a60fab76d4b66b9b669d95cca10c012f13")

    # RFC 9383 appendix C, P256-SHA256: the verifier's side, with the RFC's y.
    w0 = int("bb8e1bbcf3c48f62c08db243652ae55d3e5586053fca77102994f23ad95491b3", 16)
    w1 = int("7e945f34d78785b8a3ef44d0df5a1a97d6b3b460409a345ca7830387a74b1dba", 16)
    big_l = pt_mul(w1, G)
    check("P-256: L = w1*G (RFC 9383)", pt_encode(big_l).hex() ==
          "04eb7c9db3d9a9eb1f8adab81b5794c1f13ae3e225efbe91ea487425854c7fc00f00bfedcbd09b2400142d40a14f2064ef31dfaa903b91d1faea7093d835966efd")
    v = Verifier(w0, big_l, b"SPAKE2+-P256-SHA256-HKDF-SHA256-HMAC-SHA256 Test Vectors", b"client", b"server",
                 y=int("717a72348a182085109c8d3917d6c43d59b224dc6a7fc4f0483232fa6516d8b3", 16))
    check("SPAKE2+ shareV (RFC 9383)", v.share.hex() ==
          "04c0f65da0d11927bdf5d560c69e1d7d939a05b0e88291887d679fcadea75810fb5cc1ca7494db39e82ff2f50665255d76173e09986ab46742c798a9a68437b048")
    share_p = h("04ef3bd051bf78a2234ec0df197f7828060fe9856503579bb1733009042c15c0c1"
                "de127727f418b5966afadfdd95a6e4591d171056b333dab97a79c7193e341727")
    done = v.finish(share_p, h("926cc713504b9b4d76c9162ded04b5493e89109f6d89462cd33adc46fda27527"))
    check("SPAKE2+ verifier accepts the prover's confirmation (RFC 9383)", done is not None)
    check("SPAKE2+ HMAC(K_confirmV, shareP) and K_shared (RFC 9383)", done is not None and
          done[0].hex() == "9747bcc4f8fe9f63defee53ac9b07876d907d55047e6ff2def2e7529089d3e68" and
          done[1].hex() == "0c5f8ccd1413423a54f6c1fb26ff01534a87f893779c6e68666d772bfd91f3e7")
    check("SPAKE2+ verifier rejects a wrong confirmation", v.finish(share_p, bytes(32)) is None)
    d = hashlib.pbkdf2_hmac("sha256", b"f3bbbd66a63d4bf1747940578ec3d0103530e21d", b"0123456789abcdef", 3000, 80)
    check("w0 from PBKDF2 (tapo crate, tpap_spake2p.rs)", "%064x" % (int.from_bytes(d[:40], "big") % ORDER) ==
          "f01588c03f935783a7c530a83ab1ade1a3261b13005521b6946dc5c7ad259f2c")
    print("mock self-test: %s" % ("all passed" if not failed else "%d FAILED" % failed))
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8080, help="0 picks a free port; the one in use is printed")
    ap.add_argument("--mode", choices=("klap", "tpap"), default="tpap")
    ap.add_argument("--model", choices=("p110", "p304m"), default="p110")
    ap.add_argument("--email", default="user@example.com")
    ap.add_argument("--password", default="hunter2")
    ap.add_argument("--mac", default="AA-BB-CC-DD-EE-FF", help="the plug's MAC, as get_device_info gives it")
    ap.add_argument("--nickname", default="", help="the plug's name (default: Mock Plug ☕ or Mock Strip)")
    ap.add_argument("--page", type=int, default=10, help="sockets per get_child_device_list answer")
    ap.add_argument("--lock-after", type=int, default=0, help="TPAP: answer -2101 after this many wrong passwords")
    ap.add_argument("--no-energy-usage", action="store_true", help="answer get_energy_usage and get_current_power with -1002")
    ap.add_argument("--suite", type=int, default=1, help="TPAP: the cipher suite pake_register answers with")
    ap.add_argument("--passwd-id", type=int, default=2, help="TPAP: the password_shadow id pake_register asks for")
    ap.add_argument("--lazy", action="store_true",
                    help="open the HTTP port only after a discovery query arrives on UDP 127.0.0.1:20002")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--stdin-lifeline", action="store_true",
                    help="exit as soon as standard input closes, i.e. when the test that started it has ended")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(1 if selftest() else 0)

    if args.stdin_lifeline:
        def lifeline():
            sys.stdin.read()
            os._exit(0)
        threading.Thread(target=lifeline, daemon=True).start()
    mock = Mock(args)
    Handler.mock = mock
    if args.lazy:
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            udp.bind(("127.0.0.1", 20002))
        except OSError as e:
            print("MOCK cannot-bind-udp %s" % e, flush=True)
            sys.exit(3)
        print("MOCK waiting-for-probe", flush=True)
        while True:
            data, _ = udp.recvfrom(2048)
            if data == bytes.fromhex("020000010000000000000000463cb5d3"):
                mock.udp_probes += 1
                break
        udp.close()
    ThreadingHTTPServer.allow_reuse_address = True
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    args.port = server.server_address[1]   # the one the system picked when asked for port 0
    print("MOCK listening %d" % args.port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
