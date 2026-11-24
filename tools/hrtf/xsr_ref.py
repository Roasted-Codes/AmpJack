# SPDX-License-Identifier: GPL-3.0-or-later
"""Integer reference model of xbox360/surround_dsp.h, bit-exact with the C.

Every step below mirrors one line of the C kernel; if one is changed, the other must be, and
tests/test_surround.c (which checks the C against vectors made here) will say so.

The C runs block by block with a history; here the whole signal is convolved at once from a
zero history. The two are identical because the integer sums cannot overflow (the proof is in
surround_dsp.h) and integer addition does not depend on order.
"""
import re
import zlib

import numpy as np

K_FRONT = 6786  # game_360.c's plain downmix, Q14
K_SIDE = 4799
FILTERS = 5     # front-sum, front-diff, surround-sum, surround-diff, centre
FL, FR, C, LFE, SL, SR = range(6)


def clamp16(v):
    return np.clip(v, -32768, 32767)


def table_crc(h):
    """CRC-32 (zlib) of the coefficients as big-endian s16, filter after filter."""
    return zlib.crc32(np.asarray(h, dtype=">i2").tobytes()) & 0xFFFFFFFF


def parse_tab(path):
    """Read every table variant back out of the generated surround_tab.h, so the model runs
    on exactly the committed numbers. Returns {taps: dict(lfe_q14, out_q14, crc, h)}."""
    text = open(path).read()
    out = {}
    for m in re.finditer(r"#(?:el)?if XSR_TAPS == (\d+)(.*?)(?=#elif|#else)", text, re.S):
        taps = int(m.group(1))
        body = m.group(2)
        lfe = int(re.search(r"\.lfe_q14 = (-?\d+)", body).group(1))
        outq = int(re.search(r"\.out_q14 = (-?\d+)", body).group(1))
        crc = int(re.search(r"\.crc = (0x[0-9a-fA-F]+)", body).group(1), 16)
        rows = re.findall(r"\{([-\d,\s]+)\}", body.split(".h =", 1)[1])
        h = np.array([[int(x) for x in r.replace("\n", " ").split(",") if x.strip()] for r in rows],
                     dtype=np.int64)
        assert h.shape == (FILTERS, taps), h.shape
        assert table_crc(h) == crc
        out[taps] = dict(lfe_q14=lfe, out_q14=outq, crc=crc, h=h)
    return out


def derive(x):
    """6 planar s16 channels -> the 5 filter inputs (s16), as the C does per sample."""
    x = np.asarray(x, dtype=np.int64)
    return np.stack([(x[FL] + x[FR]) >> 1, (x[FL] - x[FR]) >> 1,
                     (x[SL] + x[SR]) >> 1, (x[SL] - x[SR]) >> 1, x[C]])


def trim(v, q):
    """(v * q + 8192) >> 14 without a 64-bit product: the C's split multiply."""
    return ((v >> 4) * q + (((v & 15) * q + 8192) >> 4)) >> 10


def process(tab, x, raw=False):
    """x: (6, n) s16 planar (FL FR C LFE SL SR). Returns ((2, n) s16 L/R, clamp count);
    raw: the output before the clamp."""
    d = derive(x)
    n = d.shape[1]
    y = np.empty((FILTERS, n), dtype=np.int64)
    for f in range(FILTERS):
        acc = np.convolve(d[f], tab["h"][f])[:n]
        y[f] = (acc + 8192) >> 14
    ls = y[0] + y[2] + y[4]
    ld = y[1] + y[3]
    lf = (np.asarray(x[LFE], dtype=np.int64) * tab["lfe_q14"] + 8192) >> 14
    out = np.stack([trim(ls + ld + lf, tab["out_q14"]), trim(ls - ld + lf, tab["out_q14"])])
    clamps = int(np.count_nonzero((out > 32767) | (out < -32768)))
    return (out if raw else clamp16(out)), clamps


def downmix(x):
    """game_360.c's plain downmix, applied to s16 input (the OFF path, LFE left out)."""
    x = np.asarray(x, dtype=np.int64)
    l = (x[FL] * K_FRONT + (x[C] + x[SL]) * K_SIDE) >> 14
    r = (x[FR] * K_FRONT + (x[C] + x[SR]) * K_SIDE) >> 14
    return clamp16(np.stack([l, r]))


def crossfade(frm, to):
    """One block from `frm` to `to` ((2, n) each), the C's xsr_crossfade."""
    frm = np.asarray(frm, dtype=np.int64)
    to = np.asarray(to, dtype=np.int64)
    n = frm.shape[1]
    step = 65536 // n
    g = ((np.arange(n, dtype=np.int64) + 1) * step) >> 1
    return frm + (((to - frm) * g) >> 15)


def xorshift(seed, n):
    """xorshift32, as the C test's; returns n raw u32 values."""
    out = np.empty(n, dtype=np.uint32)
    s = seed & 0xFFFFFFFF
    for i in range(n):
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        out[i] = s
    return out


def noise16(seed, n):
    """full-scale s16 noise: the top 16 bits of xorshift32."""
    v = (xorshift(seed, n) >> 16).astype(np.int64)
    return np.where(v >= 32768, v - 65536, v)


def interleave(lr):
    """(2, n) -> [L0, R0, L1, R1, ...] as s16"""
    return np.asarray(lr, dtype=np.int64).T.reshape(-1).astype(np.int16)


def crc16s(v):
    """CRC-32 of s16 values as big-endian bytes (the C test's out_crc)."""
    return zlib.crc32(np.asarray(v, dtype=">i2").tobytes()) & 0xFFFFFFFF


# ---- the tap around the kernel (xbox360/game_360.c), for tests/test_game_surround.c ----------

def f2q15(bits):
    """game_360.c's f2q15: float32 raw bits (u32 array) -> value x 32768, toward zero, |x|
    clamped to 2.0 (65536), NaN -> 0. Returns (values, any NaN)."""
    b = np.asarray(bits, dtype=np.uint64)
    e = (b >> 23) & 0xFF
    mant = (b & 0x7FFFFF) | 0x800000
    shift = np.clip(135 - e.astype(np.int64), 0, 63).astype(np.uint64)
    v = (mant >> shift).astype(np.int64)
    v = np.where(e >= 128, 65536, v)
    nan = (e == 255) & ((b & 0x7FFFFF) != 0)
    v = np.where(nan | (e < 112), 0, v)
    v = np.where(b & 0x80000000, -v, v)
    return v, bool(np.any(nan))


def downmix_q15(q):
    """game_360.c's convert_frame on f2q15 values (|q| <= 65536, before any clamp)"""
    q = np.asarray(q, dtype=np.int64)
    l = (q[FL] * K_FRONT + (q[C] + q[SL]) * K_SIDE) >> 14
    r = (q[FR] * K_FRONT + (q[C] + q[SR]) * K_SIDE) >> 14
    return clamp16(np.stack([l, r]))
