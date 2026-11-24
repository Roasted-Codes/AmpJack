# SPDX-License-Identifier: GPL-3.0-or-later
"""Bit-exact Python twin of xbox360/eq_dsp.h: the integer equalizer and the xw.presets reader.

Every operation below is the C one, line for line: Python's >> on negative ints is the same
arithmetic shift clang emits, and every value is proven by convert.py to stay inside the C
types (s32 samples and history, 64-bit accumulators), so unbounded Python ints give the same
numbers. Change this file and eq_dsp.h together; tests/test_eq.c compares the two through the
vectors convert.py exports.
"""
import struct
import zlib

BANDS = 5
RATE = 48000
XFADE = 480  # EQD_XFADE
WARM = 480  # EQD_WARM
MAGIC = b"XWPQ"
VERSION = 2
VERSION_OLD = 1  # eqd_check: -6
HDR = 32
REC = 224
NAME = 20
AUTHOR = 16
DESC = 64  # 63 characters and a NUL
F_OWN_LEVEL = 0x100  # record flags: not loudness-matched (convert.py Level = own)
# the CUSTOM tables after the records (eq_dsp.h struct eqd_custom)
CMAGIC = 0x58575043  # "XWPC"
CONTROLS = 3
CSTEP_MIN, CSTEP_MAX = -6, 6
CSTEPS = CSTEP_MAX - CSTEP_MIN + 1  # 13
CMID = -CSTEP_MIN  # the index of step 0
CHDR = 28
CUSTOM_BYTES = CHDR + CONTROLS * CSTEPS * 20 + CSTEPS ** 3 * 2 + 2  # 5204


TAIL_MAX = 8192  # eqd_check: bytes allowed after the records (the CUSTOM tables)


def fnv1a(name):
    h = 0x811C9DC5
    for ch in name.encode("ascii"):
        h = ((h ^ ch) * 0x01000193) & 0xFFFFFFFF
    return h


class Bands:
    """struct eqd_bands: c[5][5] (b0 b1 b2 a1 a2, Q3.28), preamp_q15, active mask."""

    def __init__(self, c, preamp_q15, active):
        self.c = [list(r) for r in c]
        self.preamp_q15 = preamp_q15
        self.active = active


def flat_bands(flat_q15):
    """FLAT as eq_360.c plays it: no bands, the file's flat_q15 gain (None when that is 0 dB,
    the exact pass-through)."""
    return None if flat_q15 == 32768 else Bands([[1 << 28, 0, 0, 0, 0]] * BANDS, flat_q15, 0)


def read_flat_q15(data):
    """eqd_flat_q15: the header's last word; 0 (files without it) or anything above 32768
    is 0 dB."""
    v = struct.unpack(">I", data[28:32])[0]
    return 32768 if v == 0 or v > 32768 else v


def new_state():
    return [[0, 0, 0, 0] for _ in range(BANDS)]  # x1 x2 y1 y2 per band


def filt(b, st, x16):
    """eqd_filter: list of s16 -> list of Q23. b None = FLAT."""
    pre = b.preamp_q15 if b else 32768
    work = [(x * pre + 64) >> 7 for x in x16]
    if not b:
        return work
    for k in range(BANDS):
        if not b.active & (1 << k):
            continue
        b0, b1, b2, a1, a2 = b.c[k]
        x1, x2, y1, y2 = st[k]
        for i, x in enumerate(work):
            acc = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            y = (acc + (1 << 27)) >> 28
            work[i] = y
            x2, x1, y2, y1 = x1, x, y1, y
        st[k] = [x1, x2, y1, y2]
    return work


def round16(y):
    v = (y + 128) >> 8
    return 32767 if v > 32767 else -32768 if v < -32768 else v


def store(work):
    return [round16(y) for y in work]


def xfade(a, b, pos, length):
    out = []
    for i in range(len(a)):
        w = 0 if pos <= 0 else 32768 if pos >= length else (pos << 15) // length
        out.append(round16(a[i] + (((b[i] - a[i]) * w) >> 15)))
        pos += 1
    return out


class Run:
    """struct eqd_run with eqd_request / eqd_block."""

    def __init__(self, ch):
        self.ch = ch
        self.cur = [new_state() for _ in range(ch)]
        self.nxt = [new_state() for _ in range(ch)]
        self.pc = None
        self.pn = None
        self.want = None
        self.fading = False
        self.xpos = 0

    def request(self, to):
        self.want = to

    def block(self, pcm):
        """pcm: interleaved s16 list (len = n * ch); returns the processed copy."""
        ch = self.ch
        n = len(pcm) // ch
        if not self.fading and self.want is not self.pc:
            self.pn = self.want
            self.fading = True
            self.xpos = -WARM
            self.nxt = [new_state() for _ in range(ch)]
        if not self.fading and self.pc is None:
            return list(pcm)
        out = list(pcm)
        for c in range(ch):
            x = pcm[c::ch]
            a = filt(self.pc, self.cur[c], x)
            if self.fading:
                b = filt(self.pn, self.nxt[c], x)
                y = xfade(a, b, self.xpos, XFADE)
            else:
                y = store(a)
            out[c::ch] = y
        if self.fading:
            self.xpos += n
            if self.xpos >= XFADE:
                self.pc = self.pn
                self.cur = self.nxt
                self.nxt = [new_state() for _ in range(ch)]
                self.fading = False
        return out


def read_presets(data):
    """Parses a whole xw.presets image (checks as eqd_check); returns a list of dicts."""
    if len(data) < HDR:
        raise ValueError("short")
    magic, ver, rate, count, rec, bands, crc, _ = struct.unpack(">4s7I", data[:HDR])
    if magic == MAGIC and ver == VERSION_OLD:
        raise ValueError("an older file version (1): build and install a new one")
    if magic != MAGIC or ver != VERSION:
        raise ValueError("magic / version")
    if rate != RATE or rec != REC or bands != BANDS:
        raise ValueError("rate / bands / record size")
    if count > 1000 or len(data) < HDR + count * REC or len(data) - HDR - count * REC > TAIL_MAX:
        raise ValueError("length")
    if zlib.crc32(data[HDR:]) != crc:
        raise ValueError("crc")
    out = []
    for i in range(count):
        r = data[HDR + i * REC:HDR + (i + 1) * REC]
        pid = struct.unpack(">I", r[0:4])[0]
        name = r[4:24].rstrip(b"\0").decode("ascii")
        author = r[24:40].rstrip(b"\0").decode("ascii")
        pre, flags = struct.unpack(">iI", r[40:48])
        flat = struct.unpack(">25i", r[48:148])
        gains = struct.unpack(">5h", r[148:158])
        desc = r[160:160 + DESC - 1].split(b"\0")[0].decode("ascii")
        c = [flat[5 * k:5 * k + 5] for k in range(BANDS)]
        out.append(dict(id=pid, name=name, author=author, gain_cdb=list(gains), desc=desc,
                        flags=flags, bands=Bands(c, pre, flags & 0x1F)))
    return out


def read_custom(data, flat_q15):
    """eqd_custom_read: (table, 0) or (None, reason): -1 absent, -2 header / size, -3 values.
    table: dict c[control][step index] = [b0 b1 b2 a1 a2], pre[index] (Q15),
    cdb[control] (the step size in 0.01 dB). Call after read_presets accepted the image."""
    count = struct.unpack(">I", data[12:16])[0]
    t = HDR + count * REC
    tail = data[t:]
    if not tail:
        return None, -1
    if len(tail) != CUSTOM_BYTES:
        return None, -2
    magic, size, controls, steps = struct.unpack(">4I", tail[:16])
    if magic != CMAGIC or size != CUSTOM_BYTES or controls != CONTROLS or steps != CSTEPS:
        return None, -2
    cdb = list(struct.unpack(">3I", tail[16:28]))
    co = struct.unpack(">%di" % (CONTROLS * CSTEPS * 5), tail[CHDR:CHDR + CONTROLS * CSTEPS * 20])
    c = [[list(co[(k * CSTEPS + j) * 5:(k * CSTEPS + j) * 5 + 5]) for j in range(CSTEPS)]
         for k in range(CONTROLS)]
    o = CHDR + CONTROLS * CSTEPS * 20
    pre = list(struct.unpack(">%dH" % CSTEPS ** 3, tail[o:o + 2 * CSTEPS ** 3]))
    if any(not 1 <= v <= 32768 for v in pre) or pre[custom_index(0, 0, 0)] != flat_q15:
        return None, -3
    if any(c[k][CMID] != [1 << 28, 0, 0, 0, 0] for k in range(CONTROLS)):
        return None, -3
    return dict(c=c, pre=pre, cdb=cdb), 0


def custom_index(b, m, t):
    """the preamp table's index of a combination (steps -6..6 each)"""
    return ((b + CMID) * CSTEPS + (m + CMID)) * CSTEPS + (t + CMID)


def custom_bands(tab, b, m, t):
    """eqd_custom_bands: the cascade of a combination (bands 0-2 = BASS, MIDS, TREBLE; 3-4
    unity). The caller plays FLAT itself for 0 0 0 (eq_360.c), never this."""
    steps = (b, m, t)
    c = [list(tab["c"][k][steps[k] + CMID]) for k in range(CONTROLS)]
    c += [[1 << 28, 0, 0, 0, 0]] * (BANDS - CONTROLS)
    active = sum(1 << k for k in range(CONTROLS) if steps[k])
    return Bands(c, tab["pre"][custom_index(b, m, t)], active)
