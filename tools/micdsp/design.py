#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Designs the integer tables in xbox360/micauto_dsp.h and checks the header against them.

    python3 tools/micdsp/design.py           # check: the header's tables are what this designs
    python3 tools/micdsp/design.py --print   # print the C tables (paste into the header)
    python3 tools/micdsp/design.py --interp  # the 8 -> 48 kHz interpolator (design note only)

Needs numpy and scipy (a venv outside the repo). The kernels themselves need neither: the
header is integer C, and micdsp.py models it with Python integers.

Decimator 24 -> 8 kHz: 45-tap equiripple (Parks-McClellan) low-pass, passband 0..3.2 kHz,
stopband 4.6..12 kHz, stopband weighted 30x (the passband may ripple +/-1 dB, the stopband
must be 50 dB down: their ratio, 0.12 / 0.0032, is about 37). Scaled to unity DC gain before
rounding to Q15. A centre-tap fix-up to force the sum to exactly 32768 is NOT used: it adds a
flat offset to the whole response and costs about 20 dB of stopband.
"""
import os
import re
import sys

import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "..", "xbox360", "micauto_dsp.h")

FS = 24000
TAPS = 45
PASS_HZ, STOP_HZ, STOP_WEIGHT = 3200, 4600, 30


def fir_dec3():
    h = signal.remez(TAPS, [0, PASS_HZ, STOP_HZ, FS / 2], [1, 0], weight=[1, STOP_WEIGHT],
                     fs=FS, maxiter=1000)
    h = h / h.sum()
    return [int(v) for v in np.round(h * 32768)]


def log2_tab():
    return [int(round(4096 * np.log2(1 + i / 32))) for i in range(33)]


def exp2_tab():
    return [int(round(32768 * 2 ** (i / 32))) for i in range(33)]


def interp6():
    """8 -> 48 kHz: zero-stuff by 6, then this low-pass at 48 kHz (gain 6 in Q15 terms: each
    of the six phases sums to about 32768). Passband 0..3.2 kHz, stopband from 4.6 kHz (the
    first image of 3.4 kHz). 6 phases x 14 taps."""
    n = 84
    h = signal.remez(n, [0, 3200, 4600, 24000], [1, 0], weight=[1, 30], fs=48000, maxiter=1000)
    h = h / h.sum() * 6
    return [int(v) for v in np.round(h * 32768)]


def response(q, fs, n=1 << 15):
    w, H = signal.freqz(np.array(q) / 32768.0, worN=n, fs=fs)
    return w, 20 * np.log10(np.abs(H) + 1e-12)


def c_table(name, ctype, vals, per=11):
    rows = [", ".join(str(v) for v in vals[i:i + per]) for i in range(0, len(vals), per)]
    return f"static const {ctype} {name}[{len(vals)}] = {{\n\t" + ",\n\t".join(rows) + ",\n};"


def header_table(name):
    txt = open(HEADER).read()
    m = re.search(name + r"\[[^\]]*\]\s*=\s*\{([^}]*)\}", txt)
    if not m:
        return None
    return [int(v) for v in re.findall(r"-?\d+", m.group(1))]


def main():
    fir = fir_dec3()
    w, d = response(fir, FS)
    pb, sb = d[w <= PASS_HZ], d[w >= STOP_HZ]
    print(f"decimator: {TAPS} taps, sum {sum(fir)} (32768 = 0 dB), sum|h| {sum(abs(v) for v in fir)}"
          f" (< 65536: no s32 overflow), passband {pb.min():+.2f}..{pb.max():+.2f} dB,"
          f" stopband max {sb.max():.1f} dB, delay {(TAPS - 1) // 2} samples"
          f" = {(TAPS - 1) / 2 / FS * 1000:.3f} ms")
    assert fir == fir[::-1], "symmetric"
    assert sum(abs(v) for v in fir) < 65536
    if "--print" in sys.argv:
        print(c_table("ma_fir_dec3", "s16", fir))
        print(c_table("ma_log2_tab", "u16", log2_tab()))
        print(c_table("ma_exp2_tab", "u32", exp2_tab()))
    if "--interp" in sys.argv:
        q = interp6()
        w, d = response(q, 48000)
        d -= 20 * np.log10(6)
        pb, sb = d[w <= 3200], d[w >= 4600]
        print(f"interpolator x6: {len(q)} taps, phases sum to "
              f"{[sum(q[p::6]) for p in range(6)]}, passband {pb.min():+.2f}..{pb.max():+.2f} dB,"
              f" stopband max {sb.max():.1f} dB, delay {(len(q) - 1) / 2 / 48:.3f} ms")
        print(c_table("interp6", "s16", q, 12))
    bad = 0
    for name, want in (("ma_fir_dec3", fir), ("ma_log2_tab", log2_tab()),
                       ("ma_exp2_tab", exp2_tab())):
        got = header_table(name)
        ok = got == want
        bad += not ok
        print(f"{'ok  ' if ok else 'FAIL'} header {name} matches the design")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
