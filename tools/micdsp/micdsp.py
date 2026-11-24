#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Integer reference model of xbox360/micauto_dsp.h and of today's mic path in voice_360.c.

Python integers only (no numpy), so every result is bit-exact with the C. The constants and
tables are read from the header itself: there is one source of truth. `params` overrides let
eval.py try other constants without editing the header.

Today's path (mic_clean, mic_analyse, shape, rs_push) is transcribed from voice_360.c so the
evaluation can compare AUTO with NOISE GATE LOW and MIC VOLUME 5, and so tests/test_micdsp.c
can prove the manual settings do not touch the new code. Keep it in step with voice_360.c.

Notes on bit-exactness: C's >> on a negative s32 is arithmetic (clang, gcc), like Python's;
C's / truncates where Python's // floors, so every division here has non-negative operands.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "..", "xbox360", "micauto_dsp.h")


def _parse_header():
    txt = open(HEADER).read()
    consts = {}
    for name, expr in re.findall(r"#define (MA_\w+)[ \t]+([^\n]+)", txt):
        expr = re.sub(r"/\*.*?\*/", "", expr).strip()
        expr = re.sub(r"\bMA_\w+\b", lambda m: str(consts[m.group(0)]), expr)
        v = eval(expr, {}, {})  # integer expressions like (-54 * 256), (-63 * 256 / 2)
        assert v == int(v), (name, expr)
        consts[name] = int(v)
    tabs = {}
    for name in ("ma_fir_dec3", "ma_log2_tab", "ma_exp2_tab"):
        m = re.search(name + r"\[[^\]]*\]\s*=\s*\{([^}]*)\}", txt)
        tabs[name] = [int(v) for v in re.findall(r"-?\d+", m.group(1))]
    return consts, tabs


C, T = _parse_header()
FIR = T["ma_fir_dec3"]
LOG2 = T["ma_log2_tab"]
EXP2 = T["ma_exp2_tab"]
DB = C["MA_DB"]
SILENT = C["MA_DB_SILENT"]
U32 = 0xFFFFFFFF


def clamp16(v):
    return 32767 if v > 32767 else -32768 if v < -32768 else v


def log2_q12(v):
    e = v.bit_length() - 1
    m = v >> (e - 15) if e >= 15 else v << (15 - e)
    f = m - 32768
    i, r = f >> 10, f & 1023
    return e * 4096 + LOG2[i] + (((LOG2[i + 1] - LOG2[i]) * r + 512) >> 10)


def db_of_log2(l2):
    return (l2 * 12330 + 32768) >> 16


def db_ms(ms):
    if not ms:
        return SILENT
    d = db_of_log2(log2_q12(ms) - 30 * 4096)
    return max(d, SILENT)


def db_q12(q12):
    return db_of_log2(log2_q12(q12 * q12) - 24 * 4096)


def lin(db, shift):
    v = ((db * 21771) >> 13) + shift * 4096
    ip = v >> 12
    fr = v & 4095
    i, r = fr >> 7, fr & 127
    m = EXP2[i] + (((EXP2[i + 1] - EXP2[i]) * r + 64) >> 7)
    if ip < -1:
        return 0
    if ip >= 16:
        return 0x80000000
    return m << (ip - 15) if ip >= 15 else (m + (1 << (14 - ip))) >> (15 - ip)


class Floor:
    def __init__(self, p=None):
        self.p = dict(C, **(p or {}))
        self.blk = [self.p["MA_FLOOR_INIT_DB"]] * self.p["MA_FLOOR_BLOCKS"]
        self.cur = self.floor_db = self.p["MA_FLOOR_INIT_DB"]
        self.sm = self.n = self.bi = self.started = 0
        self._thr()

    def _thr(self):
        p = self.p
        t = self.floor_db + p["MA_GATE_MARGIN_DB"]
        self.thr_db = min(max(t, p["MA_GATE_MIN_DB"]), p["MA_GATE_MAX_DB"])
        self.thr_rms = lin(self.thr_db, 15)

    def window(self, ms):
        p = self.p
        d = db_ms(ms)
        if not self.started:
            self.sm, self.started = d, 1
        else:
            self.sm += (d - self.sm) >> p["MA_FLOOR_SMOOTH"]
        if self.n == 0 or self.sm < self.cur:
            self.cur = self.sm
        self.n += 1
        if self.n == p["MA_FLOOR_BLOCK_WIN"]:
            self.blk[self.bi] = self.cur
            self.bi = (self.bi + 1) % p["MA_FLOOR_BLOCKS"]
            self.n = 0
        self.floor_db = min([self.cur] + self.blk)
        self._thr()


class Agc:
    def __init__(self, manual_q12, p=None):
        self.p = dict(C, **(p or {}))
        self.lvl = self.n = 0
        self.speech_db = SILENT
        self.start(manual_q12)

    def start(self, manual_q12):
        p = self.p
        self.q12 = min(max(manual_q12, p["MA_AGC_MIN_Q12"]), p["MA_AGC_MAX_Q12"])
        self.g = db_q12(self.q12)
        self.fast = p["MA_AGC_ACQUIRE_WIN"]
        self.moving = 0

    def window(self, ms, floor, gate_open, learn, adapt):
        p = self.p
        d = db_ms(ms)
        if not learn or d < floor.floor_db + p["MA_AGC_SNR_DB"] or d < floor.thr_db:
            return self.q12
        k = 0
        while k < p["MA_AGC_AVG_SHIFT"] and (1 << (k + 1)) <= self.n + 1:
            k += 1
        if self.n >= p["MA_AGC_CLIP_AFTER"]:
            lv = self.lvl >> 8
            d = min(max(d, lv - p["MA_AGC_CLIP_DOWN_DB"]), lv + p["MA_AGC_CLIP_UP_DB"])
        self.lvl += (d * 256 - self.lvl) >> k
        if self.n < (1 << p["MA_AGC_AVG_SHIFT"]):
            self.n += 1
        self.speech_db = self.lvl >> 8
        if not adapt or not gate_open or self.n < p["MA_AGC_MIN_WIN"]:
            return self.q12
        lo, hi = db_q12(p["MA_AGC_MIN_Q12"]), db_q12(p["MA_AGC_MAX_Q12"])
        want = min(max(p["MA_AGC_TARGET_DB"] - self.speech_db, lo), hi)
        err = want - self.g
        step = p["MA_AGC_FAST"] if self.fast else p["MA_AGC_SLOW"]
        if self.fast:
            self.fast -= 1
        if not self.moving and abs(err) > p["MA_AGC_DEAD_DB"]:
            self.moving = 1
        if not self.moving:
            return self.q12
        if -step <= err <= step:
            self.g, self.moving = want, 0
        else:
            self.g += step if err > 0 else -step
        self.q12 = min(max(lin(self.g, 12), p["MA_AGC_MIN_Q12"]), p["MA_AGC_MAX_Q12"])
        return self.q12


class Fir:
    N = C["MA_FIR_TAPS"]

    def __init__(self):
        self.x = [0] * (2 * self.N)
        self.pos = self.ph = 0

    def push(self, x):
        N = self.N
        self.pos = self.pos - 1 if self.pos else N - 1
        x = clamp16(x)
        self.x[self.pos] = self.x[self.pos + N] = x
        self.ph += 1
        if self.ph < 3:
            return None
        self.ph = 0
        w = self.x[self.pos:self.pos + N]
        acc = 16384 + FIR[N // 2] * w[N // 2]
        for k in range(N // 2):
            acc += FIR[k] * (w[k] + w[N - 1 - k])
        return clamp16(acc >> 15)


class Comb:
    def __init__(self, p=None):
        self.p = dict(C, **(p or {}))
        self.avg = [0] * self.p["MA_COMB_PER"]
        self.k = 0

    def push(self, x):
        a = self.avg[self.k]
        a += (x * 256 - a) >> self.p["MA_COMB_SHIFT"]
        self.avg[self.k] = a
        self.k = (self.k + 1) % self.p["MA_COMB_PER"]
        return clamp16(x - ((a + 128) >> 8))


# ---- today's path (voice_360.c), transcribed ------------------------------------------------
# by stored number (micdsp_360.c): off, step 1 -60, step 2 -55, AUTO (as step 2 when not in
# effect), step 3 -50, step 4 -45 dBFS
XD_GATE_RMS = [0, 33, 58, 58, 104, 184]
XD_GATE_HOLD_MS = 400
XD_GATE_ATTACK_DIV = 500  # the gate opens over 2 ms (voice_priv.h)
XD_MIC_GAINS = [25, 35, 50, 70, 100, 140, 200, 280, 400, 560]
XD_Q12, XD_LIMIT, XD_DC_SHIFT = 4096, 29204, 10
GATE_OFF, GATE_1, GATE_2, GATE_AUTO, GATE_3, GATE_4 = 0, 1, 2, 3, 4, 5
GATE_LOW, GATE_HIGH = GATE_2, GATE_4  # eval.py's labels: what LOW / HIGH were before the steps


def isqrt(v):
    r, b = 0, 1 << 30
    while b > v:
        b >>= 2
    while b:
        if v >= r + b:
            v -= r + b
            r = (r >> 1) + b
        else:
            r >>= 1
        b >>= 2
    return r


def gain_index(pct):
    i = 0
    pct = min(pct, 10000)
    while i + 1 < 10 and pct * pct > XD_MIC_GAINS[i] * XD_MIC_GAINS[i + 1]:
        i += 1
    return i


def manual_q12(level):
    """MIC VOLUME 1..10 as mic_gain_target() returns it"""
    return XD_MIC_GAINS[level - 1] * XD_Q12 // 100


def auto_level(q12):
    """the 1..10 the panel shows for an AUTO gain: gain_index of its per cent"""
    return gain_index(q12 * 100 // XD_Q12) + 1


def ramp_to(v, to, step):
    if v < to:
        return v + step if to - v > step else to
    return v - step if v - to > step else to


class Shape:
    def __init__(self):
        self.gain = self.shut = self.lim = self.rel = 0

    def push(self, x, gain_to, shut_to, rate=24000):
        ramp = rate // 100 or 1
        if not self.lim:
            self.lim = XD_Q12
        if self.shut > shut_to:  # opening: 2 ms
            att = rate // XD_GATE_ATTACK_DIV or 1
            self.shut = ramp_to(self.shut, shut_to, (32768 + att - 1) // att)
        elif self.shut < shut_to:  # closing: 10 ms
            self.shut = ramp_to(self.shut, shut_to, (32768 + ramp - 1) // ramp)
        self.gain = ramp_to(self.gain, gain_to, (XD_Q12 * 6 + ramp - 1) // ramp)
        g = self.gain * (32768 - self.shut) >> 15
        y = x * g >> 12
        a = abs(y)
        if (a * self.lim >> 12) > XD_LIMIT:
            self.lim = XD_LIMIT * XD_Q12 // a
        elif self.lim < XD_Q12:
            self.rel = (self.rel + 1) & U32
            if not (self.rel & 7):
                self.lim = min(self.lim + ((XD_Q12 - self.lim) >> 8) + 1, XD_Q12)
        return clamp16(y * self.lim >> 12)


class Rs:
    """rs_push for 24 -> 8 kHz (in 3, out 1): a 3-sample box, then every third sample"""

    def __init__(self, in_hz=24000, out_hz=8000):
        from math import gcd
        g = gcd(in_hz, out_hz)
        self.i, self.o = in_hz // g, out_hz // g
        self.n = min((self.i + self.o - 1) // self.o, 8) if self.i > self.o else 1
        self.t = self.bi = self.prev = self.sum = 0
        self.box = [0] * 8

    def push(self, x):
        out = []
        if self.n > 1:
            self.sum += x - self.box[self.bi]
            self.box[self.bi] = x
            self.bi = (self.bi + 1) % self.n
            s = self.sum
            x = s // self.n if s >= 0 else -((-s) // self.n)  # C truncation
        while self.t < self.o:
            v = (x - self.prev) * self.t
            q = v // self.o if v >= 0 else -((-v) // self.o)
            out.append(clamp16(self.prev + q))
            self.t += self.i
        self.t -= self.o
        self.prev = x
        return out


def playback(xs, thr, gain_q12, rate=24000, chunk=24):
    """the mic test's playback (mictest_360.c mt_gate_chunk): a clip recorded before MIC VOLUME
    and the gate, through the gate at threshold thr (window RMS, 0 = off) and the volume
    gain_q12, `chunk` samples at a time in the live path's order (voice_priv.h gate_*, then
    shape), the gate starting shut and the volume at its value. voice.py mictest save
    --gated uses it; tests/test_micdsp360.c checks it against the C (MV_PLAY_HASH)."""
    closed, hold, n, sm, opening = 1, 0, 0, 0, 0
    sh = Shape()
    sh.shut, sh.gain = (32768 if thr else 0), gain_q12
    win = rate // 50 or 1
    out = []
    for c in range(0, len(xs), chunk):
        part = xs[c:c + chunk]
        if not thr:
            closed = hold = opening = 0
        for x in part:
            sm += x * x
            if hold:
                hold -= 1
            n += 1
            if n < win:
                continue
            rms = isqrt(sm // n)
            opening = 0
            if thr and rms >= thr:
                closed, hold = 0, rate // 1000 * XD_GATE_HOLD_MS
            elif thr and not hold:
                closed = 1
            sm = n = 0
        if thr and closed and not opening and sm >= thr * thr * win:
            opening = 1
        shut = 32768 if closed and not opening else 0
        out.extend(sh.push(x, gain_q12, shut, rate) for x in part)
    return out


class Pad:
    """one pad's mic path, chunk by chunk as xv_mic_samples runs it, with the new hooks:
    gate 0..3 (3 = AUTO), level 1..10 or 0 = AUTO, fir / comb on or off, muted.
    Records a per-window trace for the evaluation and the test vectors."""

    def __init__(self, gate=GATE_LOW, level=5, fir=False, comb=False, dc=True, params=None,
                 manual_level=5):
        self.gate, self.level, self.fir_on, self.comb_on, self.dc_on = gate, level, fir, comb, dc
        self.manual = level if level else manual_level
        self.dc = self.dc_started = 0
        self.closed = self.hold = self.n = self.sum = self.opening = 0
        self.sh = Shape()
        self.rs = Rs()
        self.fir = Fir()
        self.comb = Comb(params)
        self.floor = Floor(params)
        self.agc = Agc(manual_q12(self.manual), params)
        self.muted = False
        self.trace = []  # per window: (rms, thr, open, floor_db, thr_db, q12, speech_db)

    def set_level(self, level):
        """the panel's MIC VOLUME: AUTO (0) starts from the manual level in effect"""
        if level == 0 and self.level != 0:
            self.manual = self.level
            self.agc.start(manual_q12(self.level))
        self.level = level

    def gain_to(self):
        return self.agc.q12 if self.level == 0 else manual_q12(self.level)

    def thr(self):
        if self.gate == GATE_AUTO:
            return self.floor.thr_rms
        return XD_GATE_RMS[self.gate] if self.gate < len(XD_GATE_RMS) else 0

    def chunk(self, xs):
        """xs: raw mono samples of one chunk (<= 32); returns the 8 kHz chat samples"""
        mono = []
        for x in xs:
            if self.dc_on:
                if not self.dc_started:
                    self.dc, self.dc_started = x * 4096, 1
                self.dc += (x * 4096 - self.dc) >> XD_DC_SHIFT
                x = clamp16(x - (self.dc >> 12))
            if self.comb_on:
                x = self.comb.push(x)
            mono.append(x)
        thr = self.thr()
        if not thr:
            self.closed = self.hold = self.opening = 0
        for x in mono:
            self.sum += x * x
            if self.hold:
                self.hold -= 1
            self.n += 1
            if self.n < 480:
                continue
            ms = self.sum // self.n
            rms = isqrt(ms)
            if self.gate == GATE_AUTO:
                thr = self.floor.thr_rms
            self.opening = 0
            if thr and rms >= thr:
                self.closed = 0
                self.hold = 24000 // 1000 * XD_GATE_HOLD_MS
            elif thr and not self.hold:
                self.closed = 1
            self.agc.window(ms, self.floor, not thr or rms >= thr, not self.muted,
                            self.level == 0 and not self.muted)
            self.floor.window(ms)
            self.trace.append((rms, thr, 0 if self.closed else 1, self.floor.floor_db,
                               self.floor.thr_db, self.agc.q12, self.agc.speech_db))
            self.sum = self.n = 0
        # opening early: the window so far already reaches the threshold (mic_analyse), the
        # one it will be judged by (AUTO's moved at a window end in this chunk)
        if self.gate == GATE_AUTO:
            thr = self.floor.thr_rms
        if thr and self.closed and not self.opening and self.sum >= thr * thr * 480:
            self.opening = 1
        out = []
        if self.muted:
            ys = [0] * len(mono)
        else:
            shut_now = self.closed and not self.opening
            g, shut = self.gain_to(), 32768 if shut_now else 0
            ys = [self.sh.push(x, g, shut) for x in mono]
        for y in ys:
            if self.fir_on:
                v = self.fir.push(y)
                if v is not None:
                    out.append(v)
            else:
                out.extend(self.rs.push(y))
        return out

    def run(self, xs, chunk=24):
        out = []
        for i in range(0, len(xs), chunk):
            out.extend(self.chunk(xs[i:i + chunk]))
        return out
