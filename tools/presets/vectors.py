# SPDX-License-Identifier: GPL-3.0-or-later
"""Test vectors for tests/test_eq.c (convert.py --vectors writes them to dist/eq_vectors.bin):
the Python model's output (eq_model.py) for streams the C kernel (eq_dsp.h) must reproduce
bit for bit, and the float design's response for the kernel's measured one.

'XWEV' v2, big-endian:
  header: magic, version, presets, freqs per preset, cases
  per preset: preamp, active, c[25]
  per preset: (freq mHz, design response in udB) x freqs: the float design with the Q15 preamp
  the cases, then the CUSTOM part:
    combos; per combo: bass, mids, treble (s32), then preamp, active, c[25] as
      eq_model.custom_bands builds them (eqd_custom_bands must give the same);
    cases.
  A case: ch, nblocks, per block (frames, request), s16 input, s16 expected output.
  request: -1 none, 0 FLAT, k (1..presets) preset k, 1000 + i the custom combo i. FLAT is the
  file's flat_q15 gain (eq_model.flat_bands), as eq_360.c plays it; CUSTOM 0 0 0 is FLAT
  itself (the same object), as eq_360.c plays it.
"""
import math
import random
import struct

import convert as cv
import eq_model as em

CUSTOM_REQ = 1000


def test_freqs(p):
    grid = [20 * 2 ** (i / 6) for i in range(60)] + [20000.0]
    return sorted(grid + list(p["freqs"]))


def _run_cases(cases, resolve):
    out = []
    for ch, blocks, pcm in cases:
        run = em.Run(ch)
        res, pos = [], 0
        for n, req in blocks:
            if req >= 0:
                run.request(resolve(req))
            res += run.block(pcm[pos:pos + n * ch])
            pos += n * ch
        out.append(struct.pack(">2I", ch, len(blocks)))
        out.append(b"".join(struct.pack(">Ii", n, r) for n, r in blocks))
        out.append(struct.pack(">%dh" % len(pcm), *pcm))
        out.append(struct.pack(">%dh" % len(res), *res))
    return out


def build(presets, back, flat_q15, ctab):
    rnd = random.Random(0x5eed)
    nf = len(test_freqs(presets[0]))
    out = [struct.pack(">4s4I", b"XWEV", 2, len(presets), nf, 0)]
    for p in presets:
        out.append(struct.pack(">iI25i", p["pre"], p["active"], *[v for r in p["qc"] for v in r]))
    for p in presets:
        g = cv.db(p["pre"] / 32768)
        for f in test_freqs(p):
            out.append(struct.pack(">Ii", int(round(f * 1000)),
                                   int(round((cv.db(cv.response(p["fc"], f)) + g) * 1e6))))
    bands = [b["bands"] for b in back]
    flat = em.flat_bands(flat_q15)

    def noise(n, ch, amp):
        return [int(rnd.uniform(-amp, amp)) for _ in range(n * ch)]

    def tones(n, ch, amp, f1, f2):
        ph = rnd.uniform(0, 6.28)
        return [int(round(amp * 0.5 * (math.sin(2 * math.pi * f1 * i / cv.FS + ph + c)
                                       + math.sin(2 * math.pi * f2 * i / cv.FS))))
                for i in range(n) for c in range(ch)]

    cases = []
    sizes = [384, 384, 255, 1, 128, 384]
    for k in range(len(presets)):  # each preset: fade in from FLAT, then steady, full-scale noise
        blocks = [(s, k + 1 if i == 0 else -1) for i, s in enumerate(sizes)]
        cases.append((2, blocks, noise(sum(sizes), 2, 32767)))
    # switching while fading (requests queue), back to FLAT, mono, and tones
    m = len(presets)

    def pick(i):  # preset i of whatever ships (1..m, wrapping): no set is assumed
        return (i - 1) % m + 1

    seq = [pick(1), pick(2), -1, pick(3), 0, -1, m, pick(5), 0, pick(m - 1), -1, -1]
    cases.append((2, [(384, r) for r in seq], tones(384 * len(seq), 2, 16000, 97, 1000)))
    cases.append((1, [(384, r) for r in seq], tones(384 * len(seq), 1, 16000, 3000, 8000)))
    seq2 = [m, 0, pick(7), pick(8), pick(9), pick(10), -1, -1, 0, -1]
    cases.append((2, [(240, r) for r in seq2], noise(240 * len(seq2), 2, 32767)))
    out += _run_cases(cases, lambda r: flat if r == 0 else bands[r - 1])
    ncases = len(cases)

    # CUSTOM: every single-control step, the eight corners, a random sample
    combos = []
    for k in range(3):
        for s in range(em.CSTEP_MIN, em.CSTEP_MAX + 1):
            if s:
                combos.append(tuple(s if j == k else 0 for j in range(3)))
    combos += [(b, mm, t) for b in (-6, 6) for mm in (-6, 6) for t in (-6, 6)]
    while len(combos) < 36 + 8 + 60:
        c = tuple(rnd.randint(-6, 6) for _ in range(3))
        if c != (0, 0, 0) and c not in combos:
            combos.append(c)
    cb = [em.custom_bands(ctab, *c) for c in combos]
    out.append(struct.pack(">I", len(combos)))
    for c, b in zip(combos, cb):
        out.append(struct.pack(">3i", *c))
        out.append(struct.pack(">iI25i", b.preamp_q15, b.active, *[v for r in b.c for v in r]))

    def resolve(r):
        return cb[r - CUSTOM_REQ] if r >= CUSTOM_REQ else flat if r == 0 else bands[r - 1]

    ccases = []
    # each single step faded in and played (5 blocks of 240 = warm-up, fade and two steady)
    seq = []
    for i in range(36):
        seq += [CUSTOM_REQ + i, -1, -1, -1, -1]
    ccases.append((2, [(240, r) for r in seq], noise(240 * len(seq), 2, 32767)))
    # a held direction: a new combination every block, so requests queue during fades
    seq = [CUSTOM_REQ + i for i in range(36, len(combos))] + [-1] * 4
    ccases.append((1, [(240, r) for r in seq], tones(240 * len(seq), 1, 16000, 3000, 8000)))
    # mixed with presets and FLAT (0 0 0 is FLAT), stereo tones
    seq = [CUSTOM_REQ + 3, -1, pick(2), -1, -1, CUSTOM_REQ + 40, -1, 0, -1, -1, CUSTOM_REQ + 43, -1,
           -1, CUSTOM_REQ + 44, 0, CUSTOM_REQ + 50, -1, -1, -1]
    ccases.append((2, [(384, r) for r in seq], tones(384 * len(seq), 2, 16000, 97, 4000)))
    out.append(struct.pack(">I", len(ccases)))
    out += _run_cases(ccases, resolve)
    blob = b"".join(out)
    return blob[:16] + struct.pack(">I", ncases) + blob[20:]
