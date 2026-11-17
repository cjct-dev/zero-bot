#!/usr/bin/env python3
"""A pretend HomeKit bridge (HAP over IP) for testing homekit.cpp. Python standard library only.

It plays the accessory side of:
  POST /pair-setup    M1..M6: SRP-6a (RFC 5054 3072-bit group, SHA-512), Ed25519, HKDF-SHA-512,
                      ChaCha20-Poly1305. Leading zero bytes of B are stripped, as HomeKitADK does.
  POST /pair-verify   M1..M4: X25519, then the encrypted session (1024-byte frames)
  GET  /accessories   a bridge with lights, plugs, a thermostat, a heater, a blind, fans, sensors,
                      and accessories that must never become things (lock, garage, valve, alarm,
                      doorbell, a vendor service)
  GET/PUT /characteristics, POST /pairings (remove, list)

Written from the RFCs and the protocol description, not from homekit.cpp: ChaCha20-Poly1305,
X25519, Ed25519 (after the reference code in RFC 8032 section 6) and the SRP server are plain
Python here, so the C++ (which uses OpenSSL) and this file agreeing means two independent
implementations agree. `--selftest` checks those parts against published vectors.

Test hooks (never on a real accessory), answered only on a connection that is not encrypted:
  GET  /mock/stats            counters, what the controller sent, protocol violations seen ("bad")
  POST /mock/reset {...}      clears pairings, counters, values and faults, then applies the options
  POST /mock/config {...}     applies options and faults, keeping pairings and counters
Options: code, max_tries, backoff_after, backoff_seconds, salt (hex), b (int), many (extra
lights), faults (list of names, see FAULTS), slow_ms, paired_with_other (bool), unreachable (list
of aids whose bridged accessory is switched off: their characteristics answer -70402, their
accessory information still reads), fail_writes (list of aids whose writes answer -70402 while
reads still work: switched off between the check and the write), auth_chip (bool: has an Apple authentication coprocessor, so
pair-setup Method 1 works; without one Method 1 fails at M4 as in HomeKitADK), drop_pairings
(config only: forget every controller, as after a reset of the accessory's HomeKit pairings,
HomeKitADK HAPRemoveAllPairings: its id and key stay), factory_reset (config only: forget every
controller AND take a new pairing id and long-term key, as HomeKitADK HAPRestoreFactorySettings
does by purging the configuration domain; the setup code stays, it is on the label; /mock/reset
gives back the first identity).

It binds 127.0.0.1 only (port 0 by default: the kernel picks one) and prints
"MOCK listening on 127.0.0.1:PORT" when ready. It exits when its stdin closes, so a test that
dies does not leave it running, and on SIGTERM.
"""
import argparse
import hashlib
import hmac
import json
import os
import selectors
import socket
import struct
import sys
import threading
import time
import urllib.parse

# --------------------------------------------------------------------------------------------
# ChaCha20-Poly1305 (RFC 8439)
# --------------------------------------------------------------------------------------------
M32 = 0xFFFFFFFF


def _rotl(v, n):
    return ((v << n) & M32) | (v >> (32 - n))


def _qr(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & M32
    s[d] = _rotl(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & M32
    s[b] = _rotl(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b]) & M32
    s[d] = _rotl(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & M32
    s[b] = _rotl(s[b] ^ s[c], 7)


def chacha20_block(key, counter, nonce):
    st = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574] + list(struct.unpack("<8L", key)) + \
         [counter & M32] + list(struct.unpack("<3L", nonce))
    w = st[:]
    for _ in range(10):
        _qr(w, 0, 4, 8, 12)
        _qr(w, 1, 5, 9, 13)
        _qr(w, 2, 6, 10, 14)
        _qr(w, 3, 7, 11, 15)
        _qr(w, 0, 5, 10, 15)
        _qr(w, 1, 6, 11, 12)
        _qr(w, 2, 7, 8, 13)
        _qr(w, 3, 4, 9, 14)
    return struct.pack("<16L", *[(w[i] + st[i]) & M32 for i in range(16)])


def chacha20_xor(key, counter, nonce, data):
    out = bytearray()
    for i in range(0, len(data), 64):
        chunk = data[i:i + 64]
        ks = chacha20_block(key, counter + i // 64, nonce)[:len(chunk)]
        out += (int.from_bytes(chunk, "little") ^ int.from_bytes(ks, "little")).to_bytes(len(chunk), "little")
    return bytes(out)


def poly1305(key, msg):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:32], "little")
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(msg), 16):
        acc = (acc + int.from_bytes(msg[i:i + 16] + b"\x01", "little")) * r % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def _pad16(b):
    return b"\x00" * (-len(b) % 16)


def _tag(key, nonce, aad, ct):
    otk = chacha20_block(key, 0, nonce)[:32]
    return poly1305(otk, aad + _pad16(aad) + ct + _pad16(ct) + struct.pack("<QQ", len(aad), len(ct)))


def aead_seal(key, nonce, aad, plain):
    ct = chacha20_xor(key, 1, nonce, plain)
    return ct + _tag(key, nonce, aad, ct)


def aead_open(key, nonce, aad, sealed):
    if len(sealed) < 16:
        return None
    ct, tag = sealed[:-16], sealed[-16:]
    if not hmac.compare_digest(_tag(key, nonce, aad, ct), tag):
        return None
    return chacha20_xor(key, 1, nonce, ct)


def label_nonce(label):
    return b"\x00" * 4 + label


def counter_nonce(n):
    return b"\x00" * 4 + struct.pack("<Q", n)


# --------------------------------------------------------------------------------------------
# X25519 (RFC 7748 section 5)
# --------------------------------------------------------------------------------------------
P25519 = 2 ** 255 - 19


def x25519(k, u):
    k = bytearray(k)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    k = int.from_bytes(k, "little")
    p = P25519
    x1 = int.from_bytes(u, "little") & ((1 << 255) - 1)
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    for t in reversed(range(255)):
        kt = (k >> t) & 1
        swap ^= kt
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = kt
        a = (x2 + z2) % p
        aa = a * a % p
        b = (x2 - z2) % p
        bb = b * b % p
        e = (aa - bb) % p
        c = (x3 + z3) % p
        d = (x3 - z3) % p
        da = d * a % p
        cb = c * b % p
        x3 = (da + cb) ** 2 % p
        z3 = x1 * (da - cb) ** 2 % p
        x2 = aa * bb % p
        z2 = e * (aa + 121665 * e) % p
    if swap:
        x2, x3, z2, z3 = x3, x2, z3, z2
    return (x2 * pow(z2, p - 2, p) % p).to_bytes(32, "little")


X25519_BASE = (9).to_bytes(32, "little")

# --------------------------------------------------------------------------------------------
# Ed25519 (RFC 8032, after the reference code in section 6)
#
# The point arithmetic, recover_x, compress / decompress, sign and verify below follow the Python
# code of RFC 8032 section 6, a code component of that RFC, used under the Simplified BSD License
# as section 4.e of the IETF Trust's Legal Provisions Relating to IETF Documents describes:
#
#   Copyright (c) 2017 IETF Trust and the persons identified as authors of the code. All rights
#   reserved.
#
#   Redistribution and use in source and binary forms, with or without modification, are
#   permitted provided that the following conditions are met:
#   - Redistributions of source code must retain the above copyright notice, this list of
#     conditions and the following disclaimer.
#   - Redistributions in binary form must reproduce the above copyright notice, this list of
#     conditions and the following disclaimer in the documentation and/or other materials
#     provided with the distribution.
#   - Neither the name of Internet Society, IETF or IETF Trust, nor the names of specific
#     contributors, may be used to endorse or promote products derived from this software
#     without specific prior written permission.
#
#   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
#   OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
#   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
#   COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
#   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
#   GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
#   ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
#   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
#   OF THE POSSIBILITY OF SUCH DAMAGE.
#
# The mock is a test tool and is not shipped with the app.
# --------------------------------------------------------------------------------------------
ED_P = 2 ** 255 - 19
ED_D = -121665 * pow(121666, ED_P - 2, ED_P) % ED_P
ED_Q = 2 ** 252 + 27742317777372353535851937790883648493
ED_SQRT_M1 = pow(2, (ED_P - 1) // 4, ED_P)


def _ed_add(P, Q):
    p = ED_P
    a = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    b = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    c = 2 * P[3] * Q[3] * ED_D % p
    d = 2 * P[2] * Q[2] % p
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f, g * h, f * g, e * h)


def _ed_mul(s, P):
    Q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            Q = _ed_add(Q, P)
        P = _ed_add(P, P)
        s >>= 1
    return Q


def _ed_equal(P, Q):
    p = ED_P
    return (P[0] * Q[2] - Q[0] * P[2]) % p == 0 and (P[1] * Q[2] - Q[1] * P[2]) % p == 0


def _ed_recover_x(y, sign):
    p = ED_P
    if y >= p:
        return None
    x2 = (y * y - 1) * pow(ED_D * y * y + 1, p - 2, p)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p != 0:
        x = x * ED_SQRT_M1 % p
    if (x * x - x2) % p != 0:
        return None
    if (x & 1) != sign:
        x = p - x
    return x


_ED_GY = 4 * pow(5, ED_P - 2, ED_P) % ED_P
_ED_GX = _ed_recover_x(_ED_GY, 0)
ED_G = (_ED_GX, _ED_GY, 1, _ED_GX * _ED_GY % ED_P)


def _ed_compress(P):
    zinv = pow(P[2], ED_P - 2, ED_P)
    x, y = P[0] * zinv % ED_P, P[1] * zinv % ED_P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _ed_decompress(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _ed_recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % ED_P)


def _ed_expand(secret):
    h = hashlib.sha512(secret).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def _ed_hq(*parts):
    return int.from_bytes(hashlib.sha512(b"".join(parts)).digest(), "little") % ED_Q


def ed25519_public(secret):
    return _ed_compress(_ed_mul(_ed_expand(secret)[0], ED_G))


def ed25519_sign(secret, msg):
    a, prefix = _ed_expand(secret)
    A = _ed_compress(_ed_mul(a, ED_G))
    r = _ed_hq(prefix, msg)
    R = _ed_compress(_ed_mul(r, ED_G))
    s = (r + _ed_hq(R, A, msg) * a) % ED_Q
    return R + s.to_bytes(32, "little")


def ed25519_verify(public, msg, sig):
    if len(public) != 32 or len(sig) != 64:
        return False
    A = _ed_decompress(public)
    R = _ed_decompress(sig[:32])
    s = int.from_bytes(sig[32:], "little")
    if A is None or R is None or s >= ED_Q:
        return False
    h = _ed_hq(sig[:32], public, msg)
    return _ed_equal(_ed_mul(s, ED_G), _ed_add(R, _ed_mul(h, A)))


# --------------------------------------------------------------------------------------------
# HKDF-SHA-512 (RFC 5869), SRP-6a server (RFC 5054 3072-bit group as HAP uses it), TLV8
# --------------------------------------------------------------------------------------------
def hkdf(ikm, salt, info, n=32):
    prk = hmac.new(salt or b"\x00" * 64, ikm, hashlib.sha512).digest()
    out, t, i = b"", b"", 1
    while len(out) < n:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha512).digest()
        out += t
        i += 1
    return out[:n]


SRP_N = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF", 16)
SRP_G = 5
SRP_LEN = 384


def _H(*parts):
    return hashlib.sha512(b"".join(parts)).digest()


def _pad(i):
    return i.to_bytes(SRP_LEN, "big")


SRP_K = int.from_bytes(_H(_pad(SRP_N), _pad(SRP_G)), "big")


class SrpServer:
    def __init__(self, password, salt=None, b=None, user=b"Pair-Setup"):
        self.user = user
        self.salt = os.urandom(16) if salt is None else salt
        x = int.from_bytes(_H(self.salt, _H(user + b":" + password.encode())), "big")
        self.v = pow(SRP_G, x, SRP_N)
        self.b = int.from_bytes(os.urandom(32), "big") if b is None else b
        self.B = (SRP_K * self.v + pow(SRP_G, self.b, SRP_N)) % SRP_N

    def public_stripped(self):
        return _pad(self.B).lstrip(b"\x00") or b"\x00"

    def check(self, A_bytes, M1):
        """Returns (K, M2) when the client's proof is right, else None."""
        if not A_bytes or len(A_bytes) > SRP_LEN or len(M1) != 64:
            return None
        A = int.from_bytes(A_bytes, "big")
        if A % SRP_N == 0:
            return None
        Ab, Bb = _pad(A), _pad(self.B)
        u = int.from_bytes(_H(Ab, Bb), "big")
        S = pow(A * pow(self.v, u, SRP_N) % SRP_N, self.b, SRP_N)
        K = _H(_pad(S))
        group = bytes(x ^ y for x, y in zip(_H(_pad(SRP_N)), _H(b"\x05")))
        if not hmac.compare_digest(_H(group, _H(self.user), self.salt, Ab, Bb, K), M1):
            return None
        return K, _H(Ab, M1, K)


def tlv_encode(items):
    out = bytearray()
    for t, v in items:
        if not v:
            out += bytes([t, 0])
            continue
        for i in range(0, len(v), 255):
            chunk = v[i:i + 255]
            out += bytes([t, len(chunk)]) + chunk
    return bytes(out)


def tlv_decode(data):
    items, i, join = [], 0, False
    while i < len(data):
        if len(data) - i < 2:
            raise ValueError("truncated TLV")
        t, n = data[i], data[i + 1]
        if len(data) - i - 2 < n:
            raise ValueError("truncated TLV")
        v = bytes(data[i + 2:i + 2 + n])
        if items and items[-1][0] == t and join:
            items[-1] = (t, items[-1][1] + v)
        else:
            items.append((t, v))
        join = n == 255
        i += 2 + n
    return items


def tlv_get(items, t):
    for k, v in items:
        if k == t:
            return v
    return None


T_METHOD, T_ID, T_SALT, T_PUBKEY, T_PROOF, T_ENC, T_STATE, T_ERROR, T_RETRY, T_SIG, T_PERM = \
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11
E_UNKNOWN, E_AUTH, E_BACKOFF, E_MAXPEERS, E_MAXTRIES, E_UNAVAILABLE, E_BUSY = 1, 2, 3, 4, 5, 6, 7

# --------------------------------------------------------------------------------------------
# The accessory database
# --------------------------------------------------------------------------------------------
PRW = ["pr", "pw", "ev"]
PR = ["pr", "ev"]


def ch(iid, type_, fmt, perms, value=None, **meta):
    c = {"iid": iid, "type": type_, "format": fmt, "perms": perms}
    if value is not None:
        c["value"] = value
    names = {"lo": "minValue", "hi": "maxValue", "step": "minStep", "valid": "valid-values", "unit": "unit"}
    for k, v in meta.items():
        c[names[k]] = v
    return c


def info_service(name, model="Mock"):
    return {"iid": 1, "type": "3E", "characteristics": [
        ch(2, "14", "bool", ["pw"]),
        ch(3, "20", "string", ["pr"], "Mock Maker"),
        ch(4, "21", "string", ["pr"], model),
        ch(5, "23", "string", ["pr"], name),
        ch(6, "30", "string", ["pr"], "0001"),
        ch(7, "52", "string", ["pr"], "1.0"),
    ]}


def accessory(aid, name, *services, model="Mock"):
    return {"aid": aid, "services": [info_service(name, model)] + list(services)}


def light(iid, name=None, hidden=False):
    chars = [ch(iid + 1, "25", "bool", PRW, False),
             # one type in the long form, as some accessories send them
             ch(iid + 2, "00000008-0000-1000-8000-0026BB765291", "int", PRW, 40, lo=0, hi=100, step=1, unit="percentage"),
             ch(iid + 3, "13", "float", PRW, 30, lo=0, hi=360, step=1, unit="arcdegrees"),
             ch(iid + 4, "2F", "float", PRW, 50, lo=0, hi=100, step=1, unit="percentage"),
             ch(iid + 5, "CE", "uint32", PRW, 370, lo=140, hi=500, step=1)]
    if name:
        chars.append(ch(iid + 6, "23", "string", ["pr"], name))
    svc = {"iid": iid, "type": "43", "characteristics": chars}
    if hidden:
        svc["hidden"] = True
    return svc


def outlet(iid, name=None, on=False):
    chars = [ch(iid + 1, "25", "bool", PRW, on), ch(iid + 2, "26", "bool", PR, on)]
    if name:
        chars.append(ch(iid + 3, "23", "string", ["pr"], name))
    return {"iid": iid, "type": "47", "characteristics": chars}


BAD_NAME = "@BAD@"   # replaced by raw invalid UTF-8 bytes after the JSON is made


def build_database(many=0):
    accs = [
        {"aid": 1, "services": [info_service("Mock Bridge", "MockBridge1,1"),
                                {"iid": 10, "type": "A2", "characteristics": [ch(11, "37", "string", ["pr"], "1.1.0")]}]},
        accessory(2, "Hall Lamp", light(10, "Lamp")),
        accessory(3, "Kettle Plug", outlet(10, on=True)),
        accessory(4, "Hall Thermostat", {"iid": 10, "type": "4A", "characteristics": [
            ch(11, "F", "uint8", PR, 1, lo=0, hi=2, step=1, valid=[0, 1]),
            ch(12, "33", "uint8", PRW, 1, lo=0, hi=3, step=1, valid=[0, 1, 3]),
            ch(13, "11", "float", PR, 19.5, lo=0, hi=100, step=0.1, unit="celsius"),
            ch(14, "35", "float", PRW, 20.0, lo=5, hi=32, step=0.5, unit="celsius"),
            ch(15, "36", "uint8", PRW, 0, lo=0, hi=1, step=1)]}, model="MockThermostat1,1"),
        accessory(5, "Front Door Lock",
                  {"iid": 10, "type": "45", "characteristics": [ch(11, "1D", "uint8", PR, 1), ch(12, "1E", "uint8", PRW, 1)]},
                  {"iid": 20, "type": "49", "characteristics": [ch(21, "25", "bool", PRW, False), ch(22, "23", "string", ["pr"], "Auto lock")]}),
        accessory(6, "Garage",
                  {"iid": 10, "type": "41", "characteristics": [ch(11, "E", "uint8", PR, 1), ch(12, "32", "uint8", PRW, 1),
                                                                ch(13, "24", "bool", PR, False)]},
                  light(20, "Garage light")),
        accessory(7, "Bedroom Blind", {"iid": 10, "type": "8C", "characteristics": [
            ch(11, "6D", "uint8", PR, 0, lo=0, hi=100, step=1),
            ch(12, "7C", "uint8", PRW, 0, lo=0, hi=100, step=1),
            ch(13, "72", "uint8", PR, 2, lo=0, hi=2)]}),
        accessory(8, "Ceiling Fan", {"iid": 10, "type": "B7", "characteristics": [
            ch(11, "B0", "uint8", PRW, 0, lo=0, hi=1, step=1, valid=[0, 1]),
            ch(12, "29", "float", PRW, 0, lo=0, hi=3, step=1)]}),
        accessory(9, "Hall Sensor",
                  {"iid": 10, "type": "8A", "characteristics": [ch(11, "11", "float", PR, 18.25, lo=-50, hi=100, step=0.1),
                                                                ch(12, "23", "string", ["pr"], "Temperature")]},
                  {"iid": 20, "type": "82", "characteristics": [ch(21, "10", "float", PR, 55, lo=0, hi=100, step=1),
                                                                ch(22, "23", "string", ["pr"], "Humidity")]}),
        accessory(10, "Landing Motion", {"iid": 10, "type": "85", "characteristics": [ch(11, "22", "bool", PR, False)]}),
        accessory(11, "Back Window Contact", {"iid": 10, "type": "80", "characteristics": [
            ch(11, "6A", "uint8", PR, 1, lo=0, hi=1, step=1)]}),
        accessory(12, "Vendor Gadget", {"iid": 10, "type": "E863F007-079E-48FF-8F27-9C2605A29F52", "characteristics": [
            ch(11, "E863F10A-079E-48FF-8F27-9C2605A29F52", "float", PR, 230.0)]}),
        accessory(13, "Panel Heater", {"iid": 10, "type": "BC", "characteristics": [
            ch(11, "B0", "uint8", PRW, 0, lo=0, hi=1, step=1),
            ch(12, "B1", "uint8", PR, 0, lo=0, hi=3, step=1),
            ch(13, "B2", "uint8", PRW, 1, lo=0, hi=2, step=1, valid=[1]),
            ch(14, "11", "float", PR, 17.0, lo=0, hi=100, step=0.1),
            ch(15, "12", "float", PRW, 18.0, lo=0, hi=25, step=0.5)]}),
        accessory(14, "Porch Switch", {"iid": 10, "type": "49", "characteristics": [ch(11, "25", "bool", PRW, False)]}),
        accessory(15, "Garden Tap", {"iid": 10, "type": "D0", "characteristics": [
            ch(11, "B0", "uint8", PRW, 0), ch(12, "D2", "uint8", PR, 0), ch(13, "D5", "uint8", PR, 0)]}),
        accessory(16, "Alarm", {"iid": 10, "type": "7E", "characteristics": [ch(11, "66", "uint8", PR, 3), ch(12, "67", "uint8", PRW, 3)]}),
        accessory(17, "Doorbell",
                  {"iid": 10, "type": "121", "characteristics": [ch(11, "73", "uint8", PR, None)]},
                  {"iid": 20, "type": "85", "characteristics": [ch(21, "22", "bool", PR, False)]}),
        accessory(18, "\u0001Study\tLamp ‮" + BAD_NAME + "\u007f with a name far too long for any speaker to say",
                  light(10)),
        accessory(19, "Desk", light(10, "Hidden light", hidden=True), outlet(20, "Desk socket")),
        accessory(20, "Old Fan", {"iid": 10, "type": "40", "characteristics": [
            ch(11, "25", "bool", PRW, True), ch(12, "29", "float", PRW, 50, lo=0, hi=100, step=10)]}),
        accessory(21, "Strip", outlet(10, "Left"), outlet(20, "Right"), outlet(30)),
    ]
    for i in range(many):
        accs.append(accessory(100 + i, "Extra Light %d" % (i + 1), light(10)))
    return {"accessories": accs}


def short_form(uuid):
    """A type as HAP-NodeJS toShortForm gives it: Apple's base UUID becomes its short hex number."""
    u = uuid.upper()
    if len(u) == 36 and u.endswith("-0000-1000-8000-0026BB765291"):
        return u[:8].lstrip("0") or "0"
    return u


def characteristic_index(db):
    idx = {}
    for acc in db["accessories"]:
        for svc in acc["services"]:
            for c in svc["characteristics"]:
                idx[(acc["aid"], c["iid"])] = c
    return idx


def renumbered(db, offset):
    """The same accessories with new instance ids, as after a firmware update (c# changes)."""
    db = json.loads(json.dumps(db))
    for acc in db["accessories"]:
        for svc in acc["services"]:
            svc["iid"] += offset
            for c in svc["characteristics"]:
                c["iid"] += offset
    return db


def _accessory_of(db, aid):
    return next(a for a in db["accessories"] if a["aid"] == aid)


def _set_info(acc, type_, value):
    for c in acc["services"][0]["characteristics"]:
        if c["type"] == type_:
            c["value"] = value


def reused_ids(db):
    """Accessory 2 (the hall lamp) now has a lock where the lamp was: LockTargetState at the lamp's
    old On (2.11). Same accessory information, so only the characteristic types tell."""
    db = json.loads(json.dumps(db))
    acc = _accessory_of(db, 2)
    acc["services"] = [acc["services"][0],
                       {"iid": 10, "type": "45", "characteristics": [ch(11, "1E", "uint8", PRW, 1, lo=0, hi=1, step=1),
                                                                     ch(12, "1D", "uint8", PR, 1, lo=0, hi=3, step=1)]},
                       light(20, "Lamp")]
    return db


def replaced_accessory(db):
    """The bridge gave aid 2 to another accessory: a lock whose "Auto lock" switch has its On at
    2.11, exactly where the lamp's On was (same type). Its model and serial number differ."""
    db = json.loads(json.dumps(db))
    acc = _accessory_of(db, 2)
    _set_info(acc, "21", "MockLock1,1")
    _set_info(acc, "30", "0002-LOCK")
    _set_info(acc, "23", "Front Door Lock")
    acc["services"] = [acc["services"][0],
                       {"iid": 10, "type": "49", "characteristics": [ch(11, "25", "bool", PRW, False),
                                                                     ch(12, "23", "string", ["pr"], "Auto lock")]},
                       {"iid": 20, "type": "45", "characteristics": [ch(21, "1D", "uint8", PR, 1), ch(22, "1E", "uint8", PRW, 1)]}]
    return db


def firmware_updated(db):
    """Accessory 2 has new firmware, and nothing else changed."""
    db = json.loads(json.dumps(db))
    _set_info(_accessory_of(db, 2), "52", "1.1")
    return db


def firmware_reused(db):
    """Accessory 7 (the bedroom blind) has new firmware that added a window opener (a Window
    service, never to be exposed) where the blind was: its TargetPosition (7C) sits at 7.12, the
    blind's old TargetPosition, with the same type. Only the database's service types tell."""
    db = json.loads(json.dumps(db))
    acc = _accessory_of(db, 7)
    _set_info(acc, "52", "2.0")
    blind = acc["services"][1]
    acc["services"] = [acc["services"][0],
                       {"iid": 10, "type": "8B", "characteristics": [ch(11, "6D", "uint8", PR, 0, lo=0, hi=100, step=1),
                                                                     ch(12, "7C", "uint8", PRW, 0, lo=0, hi=100, step=1),
                                                                     ch(13, "72", "uint8", PR, 2, lo=0, hi=2)]},
                       {"iid": 20, "type": "8C", "characteristics": [dict(c, iid=c["iid"] + 10) for c in blind["characteristics"]]}]
    return db


# Faults that change the accessory database, and how. The first one listed in `faults` applies.
DB_FAULTS = {
    "renumber": lambda db: renumbered(db, 1000),
    "reuse_ids": reused_ids,
    "replace_accessory": replaced_accessory,
    "firmware_update": firmware_updated,
    "firmware_reuse": firmware_reused,
}


# --------------------------------------------------------------------------------------------
# The accessory
# --------------------------------------------------------------------------------------------
FAULTS = {
    "silent",               # HAP requests are read and never answered
    "stall_after_verify",   # pair-verify works, encrypted requests are never answered
    "drop_after_verify",    # the connection is closed when the first encrypted request arrives
    "garbage_plain",        # /pair-verify M1 is answered with bytes that are not HTTP
    "garbage_tlv",          # /pair-setup and /pair-verify answer with a truncated TLV
    "huge_content_length",  # an encrypted answer claims a body of 2^40 bytes
    "huge_frame",           # an encrypted frame claims 65535 bytes
    "truncated_frame",      # half a frame, then the connection closes
    "bad_tag",              # the first answer frame has a corrupted tag
    "wrong_types",          # /accessories has every type wrong
    "deep_json",            # /accessories is nested 100000 deep
    "chunked",              # encrypted answers use chunked transfer encoding
    "event_first",          # an EVENT/1.0 message precedes every encrypted answer
    "impostor",             # pair-verify is signed with a different long-term key
    "verify_unknown",       # pair-verify M4 answers Authentication (controller not known)
    "auth_at_m2",           # pair-verify M1 is answered with State 2 and an Authentication error,
                            # unsigned (no IP accessory does this: anything at the address can)
    "verify_m4_unknown",    # pair-verify M4 answers Error 1 (Unknown), not Authentication
    "type_400",             # GET /characteristics with type=1 answers 400 {"status":-70410}
    "put_207_empty",        # PUT /characteristics answers 207 with no body
    "put_207_no_list",      # PUT /characteristics answers 207 {"characteristics": "x"}
    "lone_surrogate",       # /accessories names the porch switch "Porch \ud83d Switch" (a lone
                            # surrogate escape, as JSON.stringify writes a string cut through an emoji)
    "status_207",           # PUT /characteristics fails the first item with -70402
    "status_huge",          # PUT /characteristics fails the first item with status 1e300
    "renumber",             # instance ids change (as after a firmware update)
    "reuse_ids",            # a lock now sits at the lamp's old ids (see reused_ids); values are
                            # coerced as HAP-NodeJS does (true -> 1), so a stale write would unlock it
    "replace_accessory",    # aid 2 is now another accessory with a switch at the lamp's old On
    "firmware_update",      # accessory 2 reports new firmware; nothing else changed
    "firmware_reuse",       # the blind's new firmware put a window opener at the blind's old ids
    "no_types",             # GET /characteristics ignores type=1
    "http_500",             # encrypted requests get 500
    "remove_no_answer",     # Remove Pairing removes, then closes without answering
    "busy",                 # pair-setup M1 answers Busy
    "max_peers",            # pair-setup M5 answers MaxPeers
    "fake_m4",              # pair-setup M4 carries a wrong proof (an impostor without the code)
    "bad_m6_sig",           # pair-setup M6 carries a bad signature
}


class Accessory:
    def __init__(self, args):
        self.lock = threading.RLock()
        self.other_ltsk = hashlib.sha256(b"some other key").digest()
        self.base = {"code": args.code, "max_tries": args.max_tries, "backoff_after": args.backoff_after,
                     "backoff_seconds": args.backoff_seconds, "salt": args.salt, "b": args.b, "many": args.many,
                     "faults": list(args.fault or []), "slow_ms": 0, "paired_with_other": args.paired_with_other,
                     "unreachable": [], "fail_writes": [], "auth_chip": args.auth_chip}
        self.reset({})

    def identity(self, resets):
        """The pairing id and long-term key: the first ones, or new ones after a factory reset."""
        self.resets = resets
        self.pairing_id = "AA:BB:CC:DD:EE:%02X" % (1 + resets)
        self.ltsk = hashlib.sha256(b"mock accessory key" + (b" after reset %d" % resets if resets else b"")).digest()
        self.ltpk = ed25519_public(self.ltsk)

    def reset(self, cfg):
        with self.lock:
            self.identity(0)
            self.cfg = dict(self.base)
            self.pairings = {}
            self.failed = 0
            self.backoff_until = 0.0
            self.setup_owner = None
            self.stats = {"connections": 0, "requests": [], "setup_m1": 0, "setup_m3": 0, "setup_ok": 0,
                          "setup_methods": [], "setup_m3_methods": [], "auth_failures": 0, "verify_ok": 0, "verify_fail": 0, "removed": [],
                          "puts": [], "gets": [], "frames_in": 0, "max_frame_in": 0, "bad": [], "hosts": set()}
            self.configure(cfg)
            self.db = build_database(int(self.cfg["many"]))
            self.values = {k: c.get("value") for k, c in characteristic_index(self.db).items()}
            self.variants = {}   # database fault -> (database, values)

    def configure(self, cfg):
        with self.lock:
            for k, v in cfg.items():
                if k == "drop_pairings":   # a reset of the HomeKit pairings only (HAPRemoveAllPairings)
                    if v:
                        self.pairings = {}
                    continue
                if k == "factory_reset":   # HAPRestoreFactorySettings: new Device ID and key as well
                    if v:
                        self.pairings = {}
                        self.identity(self.resets + 1)
                    continue
                if k == "faults":
                    unknown = set(v) - FAULTS
                    if unknown:
                        raise ValueError("unknown faults: %s" % sorted(unknown))
                self.cfg[k] = v
            if self.cfg.get("paired_with_other") and "OTHER-CONTROLLER" not in self.pairings:
                self.pairings["OTHER-CONTROLLER"] = ed25519_public(self.other_ltsk)

    def fault(self, name):
        return name in self.cfg["faults"]

    def bad(self, what):
        with self.lock:
            if len(self.stats["bad"]) < 100:
                self.stats["bad"].append(what)

    def db_fault(self):
        return next((f for f in self.cfg["faults"] if f in DB_FAULTS), None)

    def current(self):
        """(database, values) as the accessory has them now. A changed database keeps the values of
        the characteristics that are still there with the same type."""
        with self.lock:
            name = self.db_fault()
            if name is None:
                return self.db, self.values
            if name not in self.variants:
                db = DB_FAULTS[name](self.db)
                old = characteristic_index(self.db)
                shift = 1000 if name == "renumber" else 0
                values = {}
                for (aid, iid), c in characteristic_index(db).items():
                    was = old.get((aid, iid - shift))
                    same = was is not None and was["type"] == c["type"] and was.get("value") == c.get("value")
                    values[(aid, iid)] = self.values.get((aid, iid - shift)) if same else c.get("value")
                self.variants[name] = (db, values)
            return self.variants[name]

    def current_db(self):
        return self.current()[0]

    def info_ids(self, db):
        """The (aid, iid) of every accessory-information characteristic."""
        return {(a["aid"], c["iid"]) for a in db["accessories"] for s in a["services"] if s["type"] == "3E"
                for c in s["characteristics"]}

    def snapshot(self):
        with self.lock:
            st = dict(self.stats)
            st["hosts"] = sorted(st["hosts"])
            st["pairings"] = {k: v.hex() for k, v in self.pairings.items()}
            st["failed_attempts"] = self.failed
            st["accessory_id"] = self.pairing_id
            st["accessory_ltpk"] = self.ltpk.hex()
            st["values"] = {"%d.%d" % k: v for k, v in self.current()[1].items() if v is not None}
            return st


REASONS = {200: "OK", 204: "No Content", 207: "Multi-Status", 400: "Bad Request", 404: "Not Found",
           470: "Connection Authorization Required", 500: "Internal Server Error"}


class Connection:
    def __init__(self, acc, sock):
        self.acc = acc
        self.sock = sock
        self.enc = False
        self.c2a_key = self.a2c_key = None
        self.c2a = self.a2c = 0
        self.raw = b""
        self.plain = b""
        self.srp = None
        self.setup_state = 0
        self.setup_method = None
        self.setup_key = None
        self.verify = None
        self.controller = None
        self.closed = False

    # ---- reading
    def recv_more(self):
        data = self.sock.recv(65536)
        if not data:
            raise EOFError()
        if not self.enc:
            self.plain += data
            return
        self.raw += data
        while len(self.raw) >= 2:
            n = struct.unpack("<H", self.raw[:2])[0]
            if n > 1024:
                self.acc.bad("controller frame of %d bytes" % n)
                raise EOFError()
            if len(self.raw) < 2 + n + 16:
                break
            plain = aead_open(self.c2a_key, counter_nonce(self.c2a), self.raw[:2], self.raw[2:2 + n + 16])
            if plain is None:
                self.acc.bad("controller frame %d failed authentication" % self.c2a)
                raise EOFError()
            self.c2a += 1
            with self.acc.lock:
                self.acc.stats["frames_in"] += 1
                self.acc.stats["max_frame_in"] = max(self.acc.stats["max_frame_in"], n)
            self.plain += plain
            self.raw = self.raw[2 + n + 16:]

    def read_request(self):
        while b"\r\n\r\n" not in self.plain:
            if len(self.plain) > 65536:
                self.acc.bad("request header too long")
                raise EOFError()
            self.recv_more()
        head, rest = self.plain.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        parts = lines[0].split(" ")
        if len(parts) != 3 or parts[2] != "HTTP/1.1":
            self.acc.bad("bad request line %r" % lines[0][:80])
            raise EOFError()
        headers = {}
        for line in lines[1:]:
            if ":" not in line:
                self.acc.bad("bad header line %r" % line[:80])
                raise EOFError()
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
        n = int(headers.get("content-length", "0"))
        while len(rest) < n:
            self.plain = head + b"\r\n\r\n" + rest
            self.recv_more()
            head, rest = self.plain.split(b"\r\n\r\n", 1)
        body, self.plain = rest[:n], rest[n:]
        return parts[0], parts[1], headers, body

    # ---- writing
    def send_raw(self, data):
        self.sock.sendall(data)

    def frames(self, data):
        out = b""
        for i in range(0, len(data), 1024):
            chunk = data[i:i + 1024]
            aad = struct.pack("<H", len(chunk))
            out += aad + aead_seal(self.a2c_key, counter_nonce(self.a2c), aad, chunk)
            self.a2c += 1
        return out

    def message(self, status, body=b"", ctype=None, chunked=False):
        head = "HTTP/1.1 %d %s\r\n" % (status, REASONS.get(status, "OK"))
        if ctype:
            head += "Content-Type: %s\r\n" % ctype
        if chunked:
            head += "Transfer-Encoding: chunked\r\n"
            enc = b""
            for i in range(0, len(body), 100):
                part = body[i:i + 100]
                enc += b"%x\r\n" % len(part) + part + b"\r\n"
            body = enc + b"0\r\n\r\n"
        elif status != 204:
            head += "Content-Length: %d\r\n" % len(body)
        return head.encode() + b"\r\n" + body

    def respond(self, status, body=b"", ctype="application/hap+json"):
        acc = self.acc
        if acc.cfg.get("slow_ms"):
            time.sleep(acc.cfg["slow_ms"] / 1000.0)
        if not self.enc:
            self.send_raw(self.message(status, body, ctype if body else None))
            return
        if acc.fault("http_500"):
            status, body = 500, b'{"status":-70402}'
        data = b""
        if acc.fault("event_first"):
            ev = json.dumps({"characteristics": [{"aid": 2, "iid": 11, "value": True}]}).encode()
            data += self.message(200, ev, "application/hap+json").replace(b"HTTP/1.1", b"EVENT/1.0", 1)
        if acc.fault("huge_content_length"):
            data += b"HTTP/1.1 200 OK\r\nContent-Type: application/hap+json\r\nContent-Length: 1099511627776\r\n\r\n{"
            self.send_raw(self.frames(data))
            return
        data += self.message(status, body, ctype if body else None, chunked=acc.fault("chunked") and status != 204)
        wire = self.frames(data)
        if acc.fault("huge_frame"):
            wire = b"\xff\xff" + os.urandom(2000)
        if acc.fault("bad_tag"):
            wire = wire[:2 + min(1024, len(data))] + bytes([wire[2 + min(1024, len(data))] ^ 1]) + wire[3 + min(1024, len(data)):]
        if acc.fault("truncated_frame"):
            self.send_raw(wire[:len(wire) // 2])
            raise EOFError()
        self.send_raw(wire)

    def respond_tlv(self, items, status=200):
        self.respond(status, tlv_encode(items), "application/pairing+tlv8")

    # ---- the loop
    def run(self):
        acc = self.acc
        counted = False
        try:
            while True:
                method, path, headers, body = self.read_request()
                if not path.startswith("/mock/"):
                    with acc.lock:
                        if not counted:   # the test's own /mock/ requests are not counted
                            acc.stats["connections"] += 1
                            counted = True
                        if len(acc.stats["requests"]) < 1000:
                            acc.stats["requests"].append([method, path.split("?")[0]])
                        acc.stats["hosts"].add(headers.get("host", ""))
                    if "host" not in headers:
                        acc.bad("request without a Host header")
                if path.startswith("/mock/"):
                    if self.enc:
                        acc.bad("mock hook inside an encrypted session")
                        raise EOFError()
                    self.mock_hook(method, path, body)
                    continue
                if acc.fault("silent") or (self.enc and acc.fault("stall_after_verify")):
                    continue
                if self.enc and acc.fault("drop_after_verify"):
                    raise EOFError()
                self.dispatch(method, path, headers, body)
        except (EOFError, ConnectionError, OSError, ValueError):
            pass
        finally:
            with acc.lock:
                if acc.setup_owner is self:
                    acc.setup_owner = None
            try:
                self.sock.close()
            except OSError:
                pass

    def mock_hook(self, method, path, body):
        acc = self.acc
        if method == "GET" and path == "/mock/stats":
            self.respond(200, json.dumps(acc.snapshot()).encode(), "application/json")
        elif method == "POST" and path in ("/mock/reset", "/mock/config"):
            try:
                cfg = json.loads(body or b"{}")
                if path == "/mock/reset":
                    acc.reset(cfg)
                else:
                    acc.configure(cfg)
                self.respond(200, b'{"ok":true}', "application/json")
            except ValueError as e:
                self.respond(400, json.dumps({"error": str(e)}).encode(), "application/json")
        else:
            self.respond(404, b"{}", "application/json")

    def dispatch(self, method, path, headers, body):
        acc = self.acc
        route = path.split("?")[0]
        if route in ("/pair-setup", "/pair-verify", "/pairings") and method == "POST":
            if headers.get("content-type") != "application/pairing+tlv8":
                acc.bad("%s with content type %r" % (route, headers.get("content-type")))
            try:
                items = tlv_decode(body)
            except ValueError:
                acc.bad("%s with a malformed TLV" % route)
                self.respond_tlv([(T_STATE, b"\x02"), (T_ERROR, bytes([E_UNKNOWN]))], 400)
                return
            if route == "/pair-setup":
                self.pair_setup(items)
            elif route == "/pair-verify":
                self.pair_verify(items)
            else:
                self.pairings_request(items)
            return
        if not self.enc:
            self.respond(470, b'{"status":-70401}')
            return
        if route == "/accessories" and method == "GET":
            self.get_accessories()
        elif route == "/characteristics" and method == "GET":
            self.get_characteristics(path)
        elif route == "/characteristics" and method == "PUT":
            if headers.get("content-type") != "application/hap+json":
                acc.bad("PUT /characteristics with content type %r" % headers.get("content-type"))
            self.put_characteristics(body)
        else:
            self.respond(404, b'{"status":-70409}')

    # ---- pair-setup
    def pair_setup(self, items):
        acc = self.acc
        state = tlv_get(items, T_STATE)
        state = state[0] if state else 0
        if acc.fault("garbage_tlv"):
            self.respond(200, b"\x06\x01\x02\x03\xff", "application/pairing+tlv8")
            return
        if state == 1:
            with acc.lock:
                acc.stats["setup_m1"] += 1
                method = tlv_get(items, T_METHOD)
                acc.stats["setup_methods"].append(method[0] if method and len(method) == 1 else -1)
                if method not in (b"\x00", b"\x01"):
                    acc.bad("pair-setup M1 with method %r" % method)
                self.setup_method = method
                if acc.pairings:
                    self.respond_tlv([(T_STATE, b"\x02"), (T_ERROR, bytes([E_UNAVAILABLE]))])
                    return
                if acc.failed >= int(acc.cfg["max_tries"]):
                    self.respond_tlv([(T_STATE, b"\x02"), (T_ERROR, bytes([E_MAXTRIES]))])
                    return
                left = acc.backoff_until - time.time()
                if left > 0:
                    self.respond_tlv([(T_STATE, b"\x02"), (T_ERROR, bytes([E_BACKOFF])),
                                      (T_RETRY, int(left + 0.999).to_bytes(2, "little"))])
                    return
                if acc.fault("busy") or (acc.setup_owner is not None and acc.setup_owner is not self):
                    self.respond_tlv([(T_STATE, b"\x02"), (T_ERROR, bytes([E_BUSY]))])
                    return
                acc.setup_owner = self
            salt = bytes.fromhex(acc.cfg["salt"]) if acc.cfg.get("salt") else None
            b = int(acc.cfg["b"]) if acc.cfg.get("b") else None
            self.srp = SrpServer(acc.cfg["code"], salt, b)
            self.setup_state = 2
            self.respond_tlv([(T_STATE, b"\x02"), (T_PUBKEY, self.srp.public_stripped()), (T_SALT, self.srp.salt)])
        elif state == 3 and self.setup_state == 2:
            with acc.lock:
                acc.stats["setup_m3"] += 1
            A, M1 = tlv_get(items, T_PUBKEY), tlv_get(items, T_PROOF)
            if A is None or M1 is None:
                acc.bad("pair-setup M3 without A or proof")
            if A is not None and len(A) != SRP_LEN:
                acc.bad("pair-setup M3 with A of %d bytes" % len(A))
            with acc.lock:
                acc.stats["setup_m3_methods"].append(
                    self.setup_method[0] if self.setup_method and len(self.setup_method) == 1 else -1)
            ok = self.srp.check(A or b"", M1 or b"")
            if ok is None:
                with acc.lock:
                    acc.failed += 1
                    acc.stats["auth_failures"] += 1
                    after = int(acc.cfg["backoff_after"])
                    if after and acc.failed >= after:
                        acc.backoff_until = time.time() + float(acc.cfg["backoff_seconds"])
                    acc.setup_owner = None
                self.setup_state = 0
                self.respond_tlv([(T_STATE, b"\x04"), (T_ERROR, bytes([E_AUTH]))])
                return
            K, M2 = ok
            with acc.lock:
                acc.failed = 0
            if self.setup_method == b"\x01" and not acc.cfg.get("auth_chip"):
                # HomeKitADK HAPPairingPairSetup.c (GetM4): "Pair Setup with Auth" without an Apple
                # Authentication Coprocessor fails with kHAPError_InvalidState once the proof has
                # been checked, which HAPIPAccessoryServer.c answers with a bare 500.
                with acc.lock:
                    acc.setup_owner = None
                self.setup_state = 0
                self.send_raw(b"HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n")
                return
            self.setup_key = K
            self.setup_state = 4
            if acc.fault("fake_m4"):
                M2 = bytes([M2[0] ^ 1]) + M2[1:]
            m4 = [(T_STATE, b"\x04"), (T_PROOF, M2)]
            if self.setup_method == b"\x01":
                # With the chip, M4 also carries the chip's certificate and signature, encrypted
                # (here stand-ins: the controller only has to cope with the item).
                key = hkdf(K, b"Pair-Setup-Encrypt-Salt", b"Pair-Setup-Encrypt-Info")
                m4.append((T_ENC, aead_seal(key, label_nonce(b"PS-Msg04"), b"",
                                            tlv_encode([(T_SIG, os.urandom(64)), (9, b"\x30" * 300)]))))
            self.respond_tlv(m4)
        elif state == 5 and self.setup_state == 4:
            K = self.setup_key
            key = hkdf(K, b"Pair-Setup-Encrypt-Salt", b"Pair-Setup-Encrypt-Info")
            enc = tlv_get(items, T_ENC) or b""
            plain = aead_open(key, label_nonce(b"PS-Msg05"), b"", enc)
            if plain is None:
                acc.bad("pair-setup M5 did not decrypt")
                self.respond_tlv([(T_STATE, b"\x06"), (T_ERROR, bytes([E_AUTH]))])
                return
            sub = tlv_decode(plain)
            cid, cpk, csig = tlv_get(sub, T_ID), tlv_get(sub, T_PUBKEY), tlv_get(sub, T_SIG)
            x = hkdf(K, b"Pair-Setup-Controller-Sign-Salt", b"Pair-Setup-Controller-Sign-Info")
            if not cid or not cpk or not csig or not ed25519_verify(cpk, x + cid + cpk, csig):
                acc.bad("pair-setup M5 signature did not verify")
                self.respond_tlv([(T_STATE, b"\x06"), (T_ERROR, bytes([E_AUTH]))])
                return
            if acc.fault("max_peers"):
                self.respond_tlv([(T_STATE, b"\x06"), (T_ERROR, bytes([E_MAXPEERS]))])
                return
            ax = hkdf(K, b"Pair-Setup-Accessory-Sign-Salt", b"Pair-Setup-Accessory-Sign-Info")
            aid = self.acc.pairing_id.encode()
            sig = ed25519_sign(acc.ltsk, ax + aid + acc.ltpk)
            if acc.fault("bad_m6_sig"):
                sig = bytes([sig[0] ^ 1]) + sig[1:]
            sealed = aead_seal(key, label_nonce(b"PS-Msg06"), b"",
                               tlv_encode([(T_ID, aid), (T_PUBKEY, acc.ltpk), (T_SIG, sig)]))
            with acc.lock:
                acc.pairings[cid.decode("utf-8", "replace")] = cpk
                acc.stats["setup_ok"] += 1
                acc.setup_owner = None
            self.setup_state = 0
            self.respond_tlv([(T_STATE, b"\x06"), (T_ENC, sealed)])
        else:
            acc.bad("pair-setup message in state %d after %d" % (state, self.setup_state))
            self.respond_tlv([(T_STATE, bytes([state + 1 if state < 255 else 0])), (T_ERROR, bytes([E_UNKNOWN]))], 400)

    # ---- pair-verify
    def pair_verify(self, items):
        acc = self.acc
        state = tlv_get(items, T_STATE)
        state = state[0] if state else 0
        if acc.fault("garbage_plain"):
            self.send_raw(b"\x00\x01 this is not HTTP at all\r\n\r\n")
            raise EOFError()
        if acc.fault("garbage_tlv"):
            self.respond(200, b"\x06\x01\x02\x05\xff\x01", "application/pairing+tlv8")
            return
        if self.enc:
            acc.bad("pair-verify inside an encrypted session")
            raise EOFError()
        if state == 1 and acc.fault("auth_at_m2"):
            with acc.lock:
                acc.stats["verify_fail"] += 1
            self.respond_tlv([(T_STATE, b"\x02"), (T_ERROR, bytes([E_AUTH]))])
            return
        if state == 1:
            cpub = tlv_get(items, T_PUBKEY)
            if cpub is None or len(cpub) != 32:
                acc.bad("pair-verify M1 without a 32-byte key")
                self.respond_tlv([(T_STATE, b"\x02"), (T_ERROR, bytes([E_UNKNOWN]))])
                return
            priv = os.urandom(32)
            pub = x25519(priv, X25519_BASE)
            shared = x25519(priv, cpub)
            key = hkdf(shared, b"Pair-Verify-Encrypt-Salt", b"Pair-Verify-Encrypt-Info")
            aid = acc.pairing_id.encode()
            signer = acc.other_ltsk if acc.fault("impostor") else acc.ltsk
            sig = ed25519_sign(signer, pub + aid + cpub)
            sealed = aead_seal(key, label_nonce(b"PV-Msg02"), b"", tlv_encode([(T_ID, aid), (T_SIG, sig)]))
            self.verify = (shared, cpub, pub, key)
            self.respond_tlv([(T_STATE, b"\x02"), (T_ENC, sealed), (T_PUBKEY, pub)])
        elif state == 3 and self.verify:
            shared, cpub, pub, key = self.verify
            self.verify = None
            plain = aead_open(key, label_nonce(b"PV-Msg03"), b"", tlv_get(items, T_ENC) or b"")
            sub = tlv_decode(plain) if plain is not None else []
            cid = tlv_get(sub, T_ID)
            csig = tlv_get(sub, T_SIG)
            with acc.lock:
                cpk = acc.pairings.get(cid.decode("utf-8", "replace")) if cid else None
            if acc.fault("verify_m4_unknown"):
                with acc.lock:
                    acc.stats["verify_fail"] += 1
                self.respond_tlv([(T_STATE, b"\x04"), (T_ERROR, bytes([E_UNKNOWN]))])
                return
            if acc.fault("verify_unknown") or cpk is None or not csig or not ed25519_verify(cpk, cpub + cid + pub, csig):
                with acc.lock:
                    acc.stats["verify_fail"] += 1
                if plain is None:
                    acc.bad("pair-verify M3 did not decrypt")
                self.respond_tlv([(T_STATE, b"\x04"), (T_ERROR, bytes([E_AUTH]))])
                return
            with acc.lock:
                acc.stats["verify_ok"] += 1
            self.respond_tlv([(T_STATE, b"\x04")])
            self.controller = cid.decode("utf-8", "replace")
            self.c2a_key = hkdf(shared, b"Control-Salt", b"Control-Write-Encryption-Key")
            self.a2c_key = hkdf(shared, b"Control-Salt", b"Control-Read-Encryption-Key")
            self.enc = True
            if self.plain:
                acc.bad("controller sent bytes before pair-verify finished")
                self.plain = b""
        else:
            acc.bad("pair-verify message in state %d" % state)
            self.respond_tlv([(T_STATE, bytes([state + 1 if state < 255 else 0])), (T_ERROR, bytes([E_UNKNOWN]))], 400)

    # ---- /pairings (inside the session)
    def pairings_request(self, items):
        acc = self.acc
        if not self.enc:
            self.respond(470, b'{"status":-70401}')
            return
        method = tlv_get(items, T_METHOD)
        if tlv_get(items, T_STATE) != b"\x01":
            acc.bad("/pairings without State 1")
        if method == b"\x04":
            cid = (tlv_get(items, T_ID) or b"").decode("utf-8", "replace")
            with acc.lock:
                acc.pairings.pop(cid, None)
                acc.stats["removed"].append(cid)
            if acc.fault("remove_no_answer"):
                raise EOFError()
            self.respond_tlv([(T_STATE, b"\x02")])
            if cid == self.controller:
                raise EOFError()   # the spec: close the sessions of a removed controller
        elif method == b"\x05":
            out = [(T_STATE, b"\x02")]
            with acc.lock:
                for i, (k, v) in enumerate(acc.pairings.items()):
                    if i:
                        out.append((0xFF, b""))
                    out += [(T_ID, k.encode()), (T_PUBKEY, v), (T_PERM, b"\x01")]
            self.respond_tlv(out)
        else:
            self.respond_tlv([(T_STATE, b"\x02"), (T_ERROR, bytes([E_UNKNOWN]))])

    # ---- accessories and characteristics
    def get_accessories(self):
        acc = self.acc
        if acc.fault("deep_json"):
            self.respond(200, b"[" * 100000 + b"]" * 100000)
            return
        if acc.fault("wrong_types"):
            body = {"accessories": [
                {"aid": "2", "services": [{"iid": 10, "type": 43, "characteristics": "none"}]},
                {"aid": 3, "services": {"iid": 10}},
                {"aid": 4, "services": [{"iid": "x", "type": "43", "characteristics": []}]},
                {"aid": 5, "services": [{"iid": 10, "type": "43", "characteristics": [
                    {"iid": "11", "type": "25", "format": "bool", "perms": ["pr", "pw"]},
                    {"iid": 12, "type": ["8"], "format": "int", "perms": "pr"},
                    {"iid": 13, "type": "25", "format": 7, "perms": [1, None, "pw"], "value": {"x": 1}},
                    {"iid": 14, "type": "8", "format": "int", "perms": ["pr", "pw"], "minValue": "low",
                     "maxValue": [100], "minStep": None, "valid-values": "all", "value": "40"}]}]},
                {"aid": 1e300, "services": []}, "not an accessory", None, 42]}
            self.respond(200, json.dumps(body).encode())
            return
        body = json.dumps(acc.current_db()).encode()
        body = body.replace(BAD_NAME.encode(), b"\xff\xfe\xc0")   # invalid UTF-8 inside a JSON string
        if acc.fault("lone_surrogate"):
            body = body.replace(b'"Porch Switch"', b'"Porch \\ud83d Switch"')
        self.respond(200, body)

    def get_characteristics(self, path):
        acc = self.acc
        query = urllib.parse.parse_qs(urllib.parse.urlparse(path).query)
        ids = query.get("id", [""])[0].split(",")
        # type=1: each item names its characteristic's type, in the short form, as HAP-NodeJS
        # (toShortForm) and HomeKitADK (HAPUUIDGetDescription) do; an item that failed has none
        # (HAP-NodeJS).
        typed = query.get("type", ["0"])[0] == "1" and not acc.fault("no_types")
        with acc.lock:
            acc.stats["gets"].append(path)
        if query.get("type", ["0"])[0] == "1" and acc.fault("type_400"):   # does not take type=1 at all
            self.respond(400, b'{"status":-70410}')
            return
        db, values = acc.current()
        idx = characteristic_index(db)
        info = acc.info_ids(db)
        off = set(acc.cfg.get("unreachable") or [])
        out, failed = [], False
        for one in ids:
            try:
                aid, iid = (int(x) for x in one.split("."))
            except ValueError:
                acc.bad("GET /characteristics with id %r" % one)
                failed = True
                continue
            c = idx.get((aid, iid))
            if c is None or "pr" not in c["perms"]:
                out.append({"aid": aid, "iid": iid, "status": -70409 if c is None else -70405})
                failed = True
                continue
            if aid in off and (aid, iid) not in info:   # a bridged accessory switched off at the wall
                out.append({"aid": aid, "iid": iid, "status": -70402})
                failed = True
                continue
            with acc.lock:
                value = values.get((aid, iid))
            item = {"aid": aid, "iid": iid, "value": value}
            if typed:
                item["type"] = short_form(c["type"])
            out.append(item)
        if failed:
            for o in out:
                o.setdefault("status", 0)
        self.respond(207 if failed else 200, json.dumps({"characteristics": out}).encode())

    def put_characteristics(self, body):
        acc = self.acc
        try:
            req = json.loads(body)
        except ValueError:
            acc.bad("PUT /characteristics body is not JSON")
            self.respond(400, b'{"status":-70410}')
            return
        with acc.lock:
            acc.stats["puts"].append(json.loads(json.dumps(req)))
        db, values = acc.current()
        idx = characteristic_index(db)
        off = set(acc.cfg.get("unreachable") or []) | set(acc.cfg.get("fail_writes") or [])
        # In the changed databases, a value is taken as HAP-NodeJS takes it (validateClientSuppliedValue
        # turns true into 1 for a number): what makes a write to a reused id dangerous.
        coerce = acc.db_fault() in ("reuse_ids", "replace_accessory", "firmware_reuse")
        results, failed = [], False
        for n, item in enumerate(req.get("characteristics", [])):
            aid, iid = item.get("aid"), item.get("iid")
            status = 0
            c = idx.get((aid, iid))
            if c is not None and coerce and c["format"] != "bool" and isinstance(item.get("value"), bool):
                item["value"] = int(item["value"])
            if c is None:
                status = -70409
            elif "pw" not in c["perms"]:
                status = -70404
                acc.bad("write to read-only %s.%s" % (aid, iid))
            elif "value" not in item:
                status = -70410
                acc.bad("write without a value")
            elif aid in off:
                status = -70402
            else:
                problem = check_value(c, item["value"])
                if problem:
                    acc.bad("%s.%s: %s" % (aid, iid, problem))
                    status = -70410
            if status == 0 and n == 0 and acc.fault("status_207"):
                status = -70402
            if status == 0 and n == 0 and acc.fault("status_huge"):
                status = 1e300
            if status == 0:
                with acc.lock:
                    values[(aid, iid)] = item["value"]
            else:
                failed = True
            results.append({"aid": aid, "iid": iid, "status": status})
        if acc.fault("put_207_empty"):   # the writes were done; the answer says nothing about them
            self.respond(207, b"")
            return
        if acc.fault("put_207_no_list"):
            self.respond(207, b'{"characteristics": "x"}')
            return
        if failed:
            self.respond(207, json.dumps({"characteristics": results}).encode())
        else:
            self.respond(204)


def check_value(c, v):
    """What is wrong with a written value for this characteristic, or None."""
    fmt = c["format"]
    if fmt == "bool":
        return None if isinstance(v, bool) else "bool written as %r" % (v,)
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return "%s written as %r" % (fmt, v)
    if fmt != "float" and not isinstance(v, int):
        return "%s written as a float %r" % (fmt, v)
    lo, hi = c.get("minValue"), c.get("maxValue")
    if lo is not None and v < lo - 1e-9:
        return "%r below minimum %r" % (v, lo)
    if hi is not None and v > hi + 1e-9:
        return "%r above maximum %r" % (v, hi)
    step = c.get("minStep")
    if step:
        k = (v - (lo or 0)) / step
        if abs(k - round(k)) > 1e-6:
            return "%r is not on a step of %r" % (v, step)
    if "valid-values" in c and v not in c["valid-values"]:
        return "%r is not one of %r" % (v, c["valid-values"])
    return None


# --------------------------------------------------------------------------------------------
# Self-test against published vectors
# --------------------------------------------------------------------------------------------
def selftest():
    ok = True

    def check(name, cond):
        nonlocal ok
        ok = ok and bool(cond)
        print("%s  %s" % ("ok  " if cond else "FAIL", name), flush=True)

    h = bytes.fromhex
    # RFC 8439 2.8.2
    key = bytes(range(0x80, 0xA0))
    nonce = h("070000004041424344454647")
    aad = h("50515253c0c1c2c3c4c5c6c7")
    pt = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it."
    sealed = aead_seal(key, nonce, aad, pt)
    check("ChaCha20-Poly1305 seal (RFC 8439 2.8.2)",
          sealed[:16].hex() == "d31a8d34648e60db7b86afbc53ef7ec2" and sealed[-16:].hex() == "1ae10b594f09e26a7e902ecbd0600691")
    check("ChaCha20-Poly1305 open, and a changed byte refused",
          aead_open(key, nonce, aad, sealed) == pt and aead_open(key, nonce, aad, b"\x00" + sealed[1:]) is None)
    # RFC 8439 A.5
    key = h("1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc207075c0")
    ct = h("64a0861575861af460f062c79be643bd5e805cfd345cf389f108670ac76c8cb24c6cfc18755d43eea09ee94e382d26b0"
           "bdb7b73c321b0100d4f03b7f355894cf332f830e710b97ce98c8a84abd0b948114ad176e008d33bd60f982b1ff37c855"
           "9797a06ef4f0ef61c186324e2b3506383606907b6a7c02b0f9f6157b53c867e4b9166c767b804d46a59b5216cde7a4e9"
           "9040c5a40433225ee282a1b0a06c523eaf4534d7f83fa1155b0047718cbc546a0d072b04b3564eea1b422273f548271a"
           "0bb2316053fa76991955ebd63159434ecebb4e466dae5a1073a6727627097a1049e617d91d361094fa68f0ff77987130"
           "305beaba2eda04df997b714d6c6f2c29a6ad5cb4022b02709b")
    got = aead_open(key, h("000000000102030405060708"), h("f33388860000000000004e91"),
                    ct + h("eead9d67890cbb22392336fea1851f38"))
    check("ChaCha20-Poly1305 open (RFC 8439 A.5)", got is not None and got.startswith(b"Internet-Drafts are draft documents"))
    # RFC 7748 5.2 and 6.1
    check("X25519 (RFC 7748 5.2 vector 1)",
          x25519(h("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                 h("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")).hex() ==
          "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")
    a = h("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
    b = h("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
    check("X25519 Diffie-Hellman (RFC 7748 6.1)",
          x25519(a, X25519_BASE).hex() == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a" and
          x25519(a, x25519(b, X25519_BASE)).hex() == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
    # RFC 8032 7.1 tests 1 to 3
    for name, sk, pk, msg, sig in [
            ("1", "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
             "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
             "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
            ("2", "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
             "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
             "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
            ("3", "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
             "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
             "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a")]:
        s = ed25519_sign(h(sk), h(msg))
        check("Ed25519 key, signature and verify (RFC 8032 7.1 test %s)" % name,
              ed25519_public(h(sk)).hex() == pk and s.hex() == sig and ed25519_verify(h(pk), h(msg), s) and
              not ed25519_verify(h(pk), h(msg) + b"x", s))
    # HKDF-SHA-512: the vector HomeKitADK's Tests/HAPCryptoTest.c uses
    check("HKDF-SHA-512 (HomeKitADK HAPCryptoTest.c)",
          hkdf(b"\x0b" * 22, h("000102030405060708090a0b0c"), h("f0f1f2f3f4f5f6f7f8f9"), 42).hex() ==
          "832390086cda71fb47625bb5ceb168e4c8e26a1a16ed34d9fc7fe92c1481579338da362cb8d9f925d7cb")
    # SRP: HomeKitADK HAPCryptoTest.c (the RFC 5054 inputs on the 3072-bit group with SHA-512)
    srp = SrpServer("password123", h("beb25379d1a8581eb5a727673a2441ee"),
                    int("e487cb59d31ac550471e81f00f6928e01dda08e974a004f49e61f5d105284d20", 16), user=b"alice")
    A = pow(SRP_G, int("60975527035CF2AD1989806F0407210BC81EDC04E2762A56AFD529DDDA2D4393", 16), SRP_N).to_bytes(SRP_LEN, "big")
    Ab = A
    Bb = _pad(srp.B)
    u = _H(Ab, Bb)
    S = pow(int.from_bytes(A, "big") * pow(srp.v, int.from_bytes(u, "big"), SRP_N) % SRP_N, srp.b, SRP_N)
    K = _H(_pad(S))
    group = bytes(x ^ y for x, y in zip(_H(_pad(SRP_N)), _H(b"\x05")))
    M1 = _H(group, _H(b"alice"), srp.salt, Ab, Bb, K)
    check("SRP-6a B, u and K (HomeKitADK HAPCryptoTest.c)",
          Bb.hex().startswith("40f57088a482d4c7733384fe0d301fddca9080ad7d4f6fdf09a01006c3cb6d56") and
          u.hex() == "03ae5f3c3fa9eff1a50d7dbb8d2f60a1ea66ea712d50ae976ee34641a1cd0e51c4683da383e8595d6cb56a15d5fbc7543e07fbddd316217e01a391a18ef06dff" and
          K.hex() == "5cbc219db052138ee1148c71cd4498963d682549ce91ca24f098468f06015beb6af245c2093f98c3651bca83ab8cab2b580bbf02184fefdf26142f73df95ac50")
    res = srp.check(A, M1)
    check("SRP-6a M1 accepted, M2 (HomeKitADK HAPCryptoTest.c)",
          M1.hex() == "5f7c14ab57ed0e94fd1d78c6b4dd09ed7e340b7e05d419a9fd760f6b35e523d1310777a1ae1d2826f596f3a85116cc457c7c964d4f44ded5559da818c88b617f" and
          res is not None and res[1].hex() == "2fa0e81f5cb73b88fa0964270f321dd641f2227a5d805c40f1bfe96aaf6a19ffce8e23287965a39eab9d5a02215f89e128177ed2c4f103e655a045531bcbf7ad")
    check("SRP-6a refuses a wrong proof", srp.check(A, bytes(64)) is None)
    # aiohomekit tests/test_crypto_srp.py: zero salt and b = 1336 give a B with a leading zero byte
    z = SrpServer("123-45-678", bytes(16), 1336)
    check("SRP-6a B with a leading zero is sent stripped (aiohomekit test_crypto_srp.py)", len(z.public_stripped()) == 383)
    # TLV8: aiohomekit tests/test_protocol_tlv.py
    data = h("060103" + "09FF" + "61" * 255 + "092D" + "61" * 45 + "010568656c6c6f")
    check("TLV8 fragments joined (aiohomekit test_protocol_tlv.py)",
          tlv_decode(data) == [(6, b"\x03"), (9, b"a" * 300), (1, b"hello")] and
          tlv_encode([(6, b"\x03"), (9, b"a" * 300), (1, b"hello")]) == data)
    check("TLV8 separator keeps two items apart",
          tlv_decode(tlv_encode([(1, b"a"), (0xFF, b""), (1, b"b")])) == [(1, b"a"), (0xFF, b""), (1, b"b")])
    print("%s" % ("mock self-test passed" if ok else "mock self-test FAILED"))
    return ok


# --------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--code", default="123-45-678")
    ap.add_argument("--max-tries", type=int, default=100)
    ap.add_argument("--backoff-after", type=int, default=0, help="answer Backoff after this many wrong codes")
    ap.add_argument("--backoff-seconds", type=float, default=30)
    ap.add_argument("--salt", default="", help="hex; random when empty")
    ap.add_argument("--b", default="", help="SRP secret b as a decimal integer; random when empty")
    ap.add_argument("--many", type=int, default=0, help="extra lights on the bridge")
    ap.add_argument("--fault", action="append", choices=sorted(FAULTS))
    ap.add_argument("--paired-with-other", action="store_true")
    ap.add_argument("--auth-chip", action="store_true", help="has an Apple authentication coprocessor (Method 1 works)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(0 if selftest() else 1)

    acc = Accessory(args)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(16)
    print("MOCK listening on 127.0.0.1:%d" % srv.getsockname()[1], flush=True)
    sel = selectors.DefaultSelector()
    sel.register(srv, selectors.EVENT_READ, "accept")
    stdin_watch = not sys.stdin.isatty()
    if stdin_watch:
        sel.register(sys.stdin, selectors.EVENT_READ, "stdin")
    while True:
        for key, _ in sel.select():
            if key.data == "stdin":
                if not os.read(sys.stdin.fileno(), 4096):
                    return   # the test that started us has gone
            else:
                conn, _ = srv.accept()
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                threading.Thread(target=Connection(acc, conn).run, daemon=True).start()


if __name__ == "__main__":
    main()
