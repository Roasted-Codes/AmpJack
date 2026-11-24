# SPDX-License-Identifier: GPL-3.0-or-later
"""The CUSTOM preset's tables for xw.presets (used by convert.py; standard library only).

CUSTOM is three plain controls, BASS / MIDS / TREBLE, each -6..+6 steps (eq_360.h). The kernel
has no floating point, so every step of every control is designed here, quantised to Q3.28,
and stored (3 x 13 biquads); the console only picks rows. The shapes (decided for a player
who "doesn't speak kHz": broad, gentle, impossible to make sound broken):
  BASS    a low shelf, half its gain at 200 Hz: rumble, booms, the weight of shots
  MIDS    a bell at 1 kHz, Q 0.7 (about two octaves): voices and the body of most sounds
  TREBLE  a high shelf, half its gain at 4 kHz: footsteps' crunch, clicks, sparkle
1.5 dB a step, so +-9 dB at the ends: enough to hear each step, small enough that no setting
is harsh or boomy beyond repair. Shelves at the gentlest RBJ slope (S = 1, no overshoot), so a
cut never boosts anywhere and a boost never overshoots its end value.

The preamp of every one of the 13^3 = 2197 combinations is tabled (u16 Q15, 4394 bytes): the
level of a cascade on K-weighted pink noise is not the sum of its sections' levels (that sum is
up to 3.3 dB off here), so no small formula keeps them in line. Per combination:
  preamp = min(as loud as FLAT, the peak-safe value): the same rule as the file's presets, so
  the generator checks every combination's peak after the preamp is <= 0 dB (a dense grid plus
  golden-section refinement of every near-maximum, on the quantised coefficients), and that
  none is louder than FLAT. A combination that boosts must turn down by its peak, so it can be
  quieter than FLAT ("as far as headroom allows"); the summary says how many and by how much.
  0 0 0 is FLAT's own level, flat_q15 (and eq_360.c plays FLAT itself for it).
CUSTOM is not part of the common level (convert.py LOUDNESS): every +6 at once needs about
-9 dB, and taking that as the common level would turn every preset down with it.

Range proof (the kernel's no-wrap guarantee, as convert.l1_bounds for a file preset): a band's
output is at most the L1 norm of its impulse response times its input's bound, so with each
control's largest L1 over its 13 steps and the largest preamp of the table, the chained bound
covers every combination at once, the worst one included. The corners (every control at -6
or +6) are also run through convert.l1_bounds as a cross-check.
"""
import math
import struct

import convert as cv
import eq_model as em

STEP_DB = 1.5
# (type, frequency Hz, Q for a peak / slope for a shelf); BASS, MIDS, TREBLE
SHAPES = (("lowshelf", 200.0, 1.0), ("peak", 1000.0, 0.7), ("highshelf", 4000.0, 1.0))
STEPS = range(em.CSTEP_MIN, em.CSTEP_MAX + 1)
UNITY = [1 << cv.QBITS, 0, 0, 0, 0]


def section(k, step):
    kind, f0, q = SHAPES[k]
    return [1.0, 0.0, 0.0, 0.0, 0.0] if step == 0 else cv.section(kind, f0, step * STEP_DB, q)


def _resp(row, z1):
    b0, b1, b2, a1, a2 = row
    return (b0 + b1 * z1 + b2 * z1 * z1) / (1 + a1 * z1 + a2 * z1 * z1)


def _l1(row):
    """L1 norm of one quantised section's impulse response"""
    b0, b1, b2, a1, a2 = (c / (1 << cv.QBITS) for c in row)
    x1 = x2 = y1 = y2 = 0.0
    total = 0.0
    for i in range(16384):
        x = 1.0 if i == 0 else 0.0
        y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, x, y1, y
        total += abs(y)
    if abs(y) > 1e-12:
        cv.fail("custom: an impulse response has not decayed")
    return total


def build(flat_q15):
    """the tables, checked: dict(c, pre, cdb, stats)"""
    qc = [[cv.quantise([section(k, s)])[0] for s in STEPS] for k in range(3)]
    for k in range(3):
        if qc[k][em.CMID] != UNITY:
            cv.fail("custom: step 0 is not unity")
    qf = [[[v / (1 << cv.QBITS) for v in row] for row in ctl] for ctl in qc]

    # |H| in dB of every section on the peak grid, |H|^2 on the loudness grid
    pgrid = [0.0] + [5 * (cv.FS / 2 / 5) ** (i / 3999) for i in range(4000)]
    lgrid = [10 * 2399 ** (i / 2399) for i in range(2400)]
    zp = [complex(math.cos(-2 * math.pi * f / cv.FS), math.sin(-2 * math.pi * f / cv.FS))
          for f in pgrid]
    zl = [complex(math.cos(-2 * math.pi * f / cv.FS), math.sin(-2 * math.pi * f / cv.FS))
          for f in lgrid]
    mag = [[[cv.db(abs(_resp(row, z))) for z in zp] for row in ctl] for ctl in qf]
    pw = [[[abs(_resp(row, z)) ** 2 for z in zl] for row in ctl] for ctl in qf]
    k2 = [cv.response(cv.K_WEIGHT, f) ** 2 for f in lgrid]
    den = sum(k2)
    flat_db = cv.db(flat_q15 / 32768)

    def exact_db(rows, f):
        z1 = complex(math.cos(-2 * math.pi * f / cv.FS), math.sin(-2 * math.pi * f / cv.FS))
        h = 1.0
        for row in rows:
            h *= _resp(row, z1)
        return cv.db(abs(h))

    pre = [0] * em.CSTEPS ** 3
    quieter, worst_q, worst_after, louder = 0, 0.0, -99.0, 0
    for b in STEPS:
        mb = mag[0][b + em.CMID]
        pb = [x * y for x, y in zip(pw[0][b + em.CMID], k2)]
        for m in STEPS:
            mbm = [x + y for x, y in zip(mb, mag[1][m + em.CMID])]
            pbm = [x * y for x, y in zip(pb, pw[1][m + em.CMID])]
            for t in STEPS:
                i = em.custom_index(b, m, t)
                if (b, m, t) == (0, 0, 0):
                    pre[i] = flat_q15
                    continue
                rows = [qf[0][b + em.CMID], qf[1][m + em.CMID], qf[2][t + em.CMID]]
                tot = [x + y for x, y in zip(mbm, mag[2][t + em.CMID])]
                top = max(tot)
                pk = top
                for j in range(1, len(tot) - 1):  # refine every near-maximum
                    if tot[j] >= top - 0.5 and tot[j] >= tot[j - 1] and tot[j] >= tot[j + 1]:
                        lo, hi = pgrid[j - 1], pgrid[j + 1]
                        for _ in range(50):
                            m1, m2 = lo + (hi - lo) * 0.382, lo + (hi - lo) * 0.618
                            if exact_db(rows, m1) < exact_db(rows, m2):
                                lo = m1
                            else:
                                hi = m2
                        pk = max(pk, exact_db(rows, (lo + hi) / 2))
                loud = 10 * math.log10(sum(x * y for x, y in zip(pbm, pw[2][t + em.CMID])) / den)
                safe = 32768 if pk <= 0 else int(math.floor(32768 * 10 ** (-pk / 20)))
                match = int(round(32768 * 10 ** ((flat_db - loud) / 20)))
                p = max(1, min(match, safe, 32768))
                pre[i] = p
                after = pk + cv.db(p / 32768)
                worst_after = max(worst_after, after)
                if after > 1e-9:
                    cv.fail("custom %+d %+d %+d: peak after preamp %+.4f dB" % (b, m, t, after))
                lvl = loud + cv.db(p / 32768) - flat_db
                if lvl > 0.01:
                    louder += 1
                if lvl < -0.05:
                    quieter += 1
                    worst_q = min(worst_q, lvl)
    if louder:
        cv.fail("custom: %d combinations louder than FLAT" % louder)

    # range proof, every combination at once (see the top)
    x_bound = (32768 * max(pre) + 64) >> 7
    proof = []
    for k in range(3):
        l1 = max(_l1(row) for row in qc[k])
        y_bound = l1 * x_bound * 1.01 + 1024
        acc = max(sum(abs(c) for c in row[:3]) * x_bound + sum(abs(c) for c in row[3:]) * y_bound
                  for row in qc[k]) + (1 << 27)
        if y_bound >= 2 ** 31 - 1 or acc >= 2 ** 63 - 1:
            cv.fail("custom: a band output or accumulator can overflow")
        proof.append((l1, y_bound, acc))
        x_bound = y_bound
    for b in (-6, 6):  # cross-check on the corners
        for m in (-6, 6):
            for t in (-6, 6):
                bands = em.custom_bands(dict(c=qc, pre=pre), b, m, t)
                xm = (32768 * bands.preamp_q15 + 64) >> 7
                for l1, yb, acc in cv.l1_bounds(bands.c, bands.active, xm):
                    if yb > proof[-1][1] or acc > max(p[2] for p in proof):
                        cv.fail("custom: the corner %+d %+d %+d exceeds the chained bound"
                                % (b, m, t))
    cdb = [int(round(STEP_DB * 100))] * 3
    return dict(c=qc, pre=pre, cdb=cdb,
                stats=dict(quieter=quieter, worst_quieter=worst_q, worst_after=worst_after,
                           max_y=proof[-1][1], max_acc=max(p[2] for p in proof),
                           l1=[p[0] for p in proof]))


def pack(tab):
    out = struct.pack(">4I", em.CMAGIC, em.CUSTOM_BYTES, em.CONTROLS, em.CSTEPS)
    out += struct.pack(">3I", *tab["cdb"])
    out += struct.pack(">%di" % (em.CONTROLS * em.CSTEPS * 5),
                       *[v for ctl in tab["c"] for row in ctl for v in row])
    out += struct.pack(">%dH" % em.CSTEPS ** 3, *tab["pre"])
    out += b"\0\0"
    assert len(out) == em.CUSTOM_BYTES
    return out


def summary(tab, flat_q15):
    s = tab["stats"]
    return ("CUSTOM: %s, %.1f dB a step; %d of 2196 combinations quieter than FLAT (they boost "
            "and need the headroom), the worst by %.2f dB; none louder; peak after preamp <= "
            "0 dB (highest %+.4f); every band output < 2^31 (bound %.4f of it), every "
            "accumulator < 2^63 (bound 2^%.1f)" % (
                " / ".join("%s %g Hz" % (kind, f) for kind, f, _ in SHAPES), STEP_DB,
                s["quieter"], -s["worst_quieter"], s["worst_after"], s["max_y"] / 2 ** 31,
                math.log2(s["max_acc"])))
