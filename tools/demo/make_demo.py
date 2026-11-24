#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The surround demo clip, xw.demo: what SURROUND DEMO plays through the headset (surround_360.c).

    python3 tools/demo/make_demo.py               writes xbox360/out/xw.demo (about 7.9 MB)
    python3 tools/demo/make_demo.py --voice       the same, a spoken speaker name before each
                                                  chime (macOS `say`; not reproducible elsewhere)
    python3 tools/demo/make_demo.py --check FILE  parse FILE, check every CRC and that no channel
                                                  peaks above -6 dBFS (the console's own maths)

Python standard library only. Everything is synthesised here (no recorded audio), from a fixed
seed, so the default clip is the same bytes on every run of the same Python on the same
machine, and free of any licence question.

What it plays (40 s):
  0.5 s   each speaker in turn, front-left, centre, front-right, back-right, back-left: a short
          rising chime (three soft bell notes and their octave), a different key per speaker
 14.0 s   a soft plucked arpeggio with a little air under it, travelling twice around the head
          (centre, right, behind, left, centre), panned between neighbouring speakers at
          constant power
 26.5 s   a scene: a warm pad and light wind in front; footsteps behind left, a weapon-like
          click-clack behind right, footsteps behind right, a tick behind left, and a whoosh
          passing behind from right to left
Peaks stay under -7 dBFS in every channel, so the virtualizer (which can raise a near-ear
peak by a few dB) never clamps.

Why this layout (the numbers, for 48 kHz, 16 bit):
  six channels as they are would be 576 KB a second (23 MB for 40 s). The tap may hold at
  most 64 KB while it plays, which would be 111 ms of read-ahead, and the file worker that
  refills it is a system thread on hardware thread 3, which (like the driver thread) only runs
  in the kernel's ~1 ms window once per video frame: one read job takes one to a few frames
  (17-70 ms) whatever its size. 111 ms is too thin for that.
  So the clip is two mono "stems" plus, per 256-frame block, each stem's gain into each of
  the six channels (Q15, at the block's end; the tap ramps from the previous block's gains
  across the block). Every speaker feed is a stem times a smoothly moving gain, which is all
  this content needs (at most two independent sounds at once; a moving sound is a gain that
  moves). That is 192 KB a second at full 48 kHz / 16-bit quality: 299 ms of read-ahead in
  7 pages of 8392 bytes (58.7 KB), and a 7.9 MB file. What it cannot do: two decorrelated
  channels of one sound (a wide stereo bed); the scene's bed is phantom-centred instead.

The file (big-endian, the console's order; surround_demo.h reads it, keep them in step):
  header, 64 bytes: magic "XWDM", u16 version 1, u16 header bytes 64, u32 rate 48000,
    u16 channels 6, u16 stems 2, u16 block frames 256, u16 blocks per page 8,
    u32 page bytes 8392, u32 pages, u32 blocks (the clip; the last page is padded with
    silence), u32 frames (blocks x 256), u32 peak (the largest |sample| of any channel
    after expansion), 20 bytes zero, u32 CRC-32 (zlib) of bytes 0..59
  pages, 8392 bytes each: u32 CRC-32 of the page's bytes 4.., u32 page index, then 8 block
    records of 1048 bytes: s16 gain[2 stems][6 channels FL FR C LFE SL SR] (Q15, 0..32767),
    s16 stem A[256], s16 stem B[256]
  The tap's sample for channel c, frame i (0..255) of a block, prev = the previous block's
  gains (0 before the first block):
    g = prev + ((gain - prev) * (i + 1) >> 8)                     for each stem
    v = clamp16((A[i] * gA + B[i] * gB) >> 15)
"""
import argparse
import array
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import wave
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "xbox360", "out", "xw.demo")

RATE = 48000
BLOCK = 256
STEMS = 2
CHANNELS = 6
PAGE_BLOCKS = 8
REC = 2 * STEMS * CHANNELS + 2 * STEMS * BLOCK  # 1048
PAGE = 8 + PAGE_BLOCKS * REC                    # 8392
HDR = 64
MAGIC = b"XWDM"
VERSION = 1
SECONDS = 40.0
PEAK_LIMIT = 16422   # -6.0 dBFS: the brief's ceiling, checked on the console's own maths
PEAK_TARGET = -7.0   # dBFS: what the mastering aims at (1 dB of margin)
LOUD = PEAK_LIMIT // 2  # -12 dBFS: blocks whose bound is under this need no exact peak
FL, FR, C, LFE, SL, SR = range(6)
NAMES = ["FL", "FR", "C", "LFE", "SL", "SR"]
SEED = 0x58574D44
TAU = 2 * math.pi


def db(x):
    return 10 ** (x / 20)


# ---- synthesis (floats, full scale 1.0) ------------------------------------------------------

def bell(freq, dur, amp=1.0):
    """a soft struck bell / celesta note: four partials, each an exponentially decaying sine
    made by a complex rotation (fast in pure Python), 4 ms attack"""
    n = int(dur * RATE)
    out = array.array("d", bytes(8 * n))
    for mult, a, tau in ((1.0, 1.0, 0.9), (2.0, 0.32, 0.45), (3.01, 0.10, 0.25), (4.2, 0.05, 0.12)):
        f = freq * mult
        if f > 15000:
            continue
        w = complex(math.cos(TAU * f / RATE), math.sin(TAU * f / RATE)) * math.exp(-1 / (tau * RATE))
        z = complex(0, a * amp)
        for i in range(n):
            out[i] += z.real
            z *= w
    return env(out, 0.004, 0.3)


def pluck(freq, dur, amp=1.0, bright=0.35):
    """a soft plucked note: a few harmonics, the higher ones dying faster"""
    n = int(dur * RATE)
    out = array.array("d", bytes(8 * n))
    for k in range(1, 6):
        f = freq * k
        if f > 12000:
            break
        a = amp * (bright ** (k - 1))
        tau = 0.55 / k
        w = complex(math.cos(TAU * f / RATE), math.sin(TAU * f / RATE)) * math.exp(-1 / (tau * RATE))
        z = complex(0, a)
        for i in range(n):
            out[i] += z.real
            z *= w
    return env(out, 0.006, 0.2)


def noise(n, rng):
    return array.array("d", (rng.random() * 2 - 1 for _ in range(n)))


def lowpass(x, fc):
    """one pole"""
    a = math.exp(-TAU * fc / RATE)
    y, out = 0.0, array.array("d", bytes(8 * len(x)))
    for i, v in enumerate(x):
        y = (1 - a) * v + a * y
        out[i] = y
    return out


def bandpass(x, f0, q, f1=None):
    """RBJ biquad band-pass (constant peak gain), its centre gliding from f0 to f1"""
    out = array.array("d", bytes(8 * len(x)))
    x1 = x2 = y1 = y2 = 0.0
    n = len(x)
    for i, v in enumerate(x):
        if i % 64 == 0:
            f = f0 if f1 is None else f0 * (f1 / f0) ** (i / max(1, n - 1))
            w0 = TAU * f / RATE
            al = math.sin(w0) / (2 * q)
            a0 = 1 + al
            b0, b2 = al / a0, -al / a0
            a1, a2 = -2 * math.cos(w0) / a0, (1 - al) / a0
        y = b0 * v + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, v
        y2, y1 = y1, y
        out[i] = y
    return out


def env(x, att, rel):
    """linear fade in over att s and out over the last rel s"""
    n = len(x)
    a, r = int(att * RATE), int(rel * RATE)
    for i in range(min(a, n)):
        x[i] *= i / a
    for i in range(min(r, n)):
        x[n - 1 - i] *= i / r
    return x


def peak(x):
    return max((abs(v) for v in x), default=0.0)


def norm(x, dbfs):
    p = peak(x)
    if p:
        g = db(dbfs) / p
        for i in range(len(x)):
            x[i] *= g
    return x


def add(dst, src, at):
    for i, v in enumerate(src):
        if 0 <= at + i < len(dst):
            dst[at + i] += v


def chime(root):
    """a rising triad and the octave, 110 ms apart, the last ringing"""
    total = int(2.4 * RATE)
    out = array.array("d", bytes(8 * total))
    for k, (ratio, a) in enumerate(((1.0, 0.8), (1.25, 0.75), (1.5, 0.75), (2.0, 0.6))):
        add(out, bell(root * ratio, 2.4 - 0.11 * k, a), int(0.11 * k * RATE))
    return env(out, 0.0, 0.6)


def footstep(rng, heavy=1.0):
    """a low thump (a falling sine) and a short scuff of filtered noise"""
    n = int(0.25 * RATE)
    out = array.array("d", bytes(8 * n))
    ph = 0.0
    for i in range(n):
        t = i / RATE
        f = 55 + 45 * math.exp(-t / 0.03)
        ph += TAU * f / RATE
        out[i] = heavy * math.sin(ph) * math.exp(-t / 0.06)
    sc = bandpass(noise(int(0.05 * RATE), rng), 1800, 0.8)
    sc = env(sc, 0.002, 0.04)
    add(out, norm(sc, -6), int(0.004 * RATE))
    return env(out, 0.0, 0.05)


def click(rng, f, dur=0.02):
    x = bandpass(noise(int(dur * RATE), rng), f, 6)
    return env(norm(x, 0), 0.0005, dur * 0.9)


def speak(text):
    """`say` into 48 kHz mono s16 (macOS only); floats"""
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "v.wav")
        subprocess.run(["say", "-v", "Samantha", "--file-format=WAVE", "--data-format=LEI16@48000",
                        "-o", p, text], check=True)
        with wave.open(p) as w:
            assert w.getframerate() == RATE and w.getsampwidth() == 2
            raw = w.readframes(w.getnframes())
            ch = w.getnchannels()
    v = array.array("h", raw)
    if sys.byteorder == "big":
        v.byteswap()
    return array.array("d", (v[i] / 32768 for i in range(0, len(v), ch)))


# ---- the clip: two stems and their gains per block ---------------------------------------------

class Clip:
    def __init__(self, seconds):
        self.blocks = int(seconds * RATE) // BLOCK
        n = self.blocks * BLOCK
        self.stem = [array.array("d", bytes(8 * n)) for _ in range(STEMS)]
        # end-of-block gains per stem, floats 0..1, six channels
        self.gain = [[(0.0,) * CHANNELS for _ in range(self.blocks)] for _ in range(STEMS)]
        self.busy = [0] * STEMS   # the first block after the stem's last event
        self.held = [(0.0,) * CHANNELS] * STEMS

    def place(self, stem, t, sig, gains):
        """sig on `stem` from t seconds (rounded down to a block). gains: a 6-tuple, or a
        function of the block's end time in seconds (from the event's start) -> 6-tuple. The
        stem must be silent from the block before (its gains move there, unheard)."""
        b0 = int(t * RATE) // BLOCK
        nb = (len(sig) + BLOCK - 1) // BLOCK
        assert b0 >= self.busy[stem] + 1 or (b0 == 0 and self.busy[stem] == 0), \
            f"stem {stem} busy at {t:.2f} s"
        add(self.stem[stem], sig, b0 * BLOCK)
        g = gains if callable(gains) else (lambda _t, gg=gains: gg)
        if b0 > 0:
            self.gain[stem][b0 - 1] = g(0.0)
        for k in range(nb):
            if b0 + k < self.blocks:
                self.gain[stem][b0 + k] = g((k + 1) * BLOCK / RATE)
        self.busy[stem] = b0 + nb
        self.held[stem] = g(nb * BLOCK / RATE)
        # idle blocks keep the last gains (nothing moves while the stem is silent)
        for k in range(b0 + nb, self.blocks):
            self.gain[stem][k] = self.held[stem]


def one(ch, g=1.0):
    v = [0.0] * CHANNELS
    v[ch] = g
    return tuple(v)


# around the head: (channel, angle in degrees), clockwise from the centre
RING = [(C, 0), (FR, 30), (SR, 110), (SL, 250), (FL, 330)]


def ring_gains(deg):
    """constant-power pan between the two speakers either side of `deg` on RING"""
    deg %= 360
    pts = RING + [(C, 360)]
    for (a, da), (b, db_) in zip(pts, pts[1:]):
        if da <= deg <= db_:
            x = (deg - da) / (db_ - da)
            v = [0.0] * CHANNELS
            v[a] += math.cos(x * math.pi / 2)
            v[b] += math.sin(x * math.pi / 2)
            return tuple(v)
    raise AssertionError(deg)


def compose(voice=False):
    rng = random.Random(SEED)
    clip = Clip(SECONDS + (3.5 if voice else 0.0))  # the names make the tour 3.5 s longer

    # (a) each speaker in turn: a chime, a different key each (C major-ish, so it sounds
    # like one piece); with --voice its name first. Chimes alternate stems so one's tail
    # never moves with the next one's gains.
    tour = [(FL, 392.00, "Front left"), (C, 523.25, "Centre"), (FR, 659.26, "Front right"),
            (SR, 440.00, "Back right"), (SL, 349.23, "Back left")]
    t = 0.5
    slot = 3.4 if voice else 2.7
    for k, (ch, root, name) in enumerate(tour):
        if voice:
            v = norm(env(speak(name), 0.005, 0.02), -10)
            clip.place(1, t, v, one(ch))
            clip.place(0, t + 1.0, norm(chime(root), -9), one(ch))
        else:
            clip.place(k % 2, t, norm(chime(root), -9), one(ch))
        t += slot
    tour_end = t

    # (b) twice around: a plucked arpeggio (Am9 - Fmaj7 - C - G, eight notes a second) with
    # soft air under it, at constant angular speed (6 s a lap), fading in and out
    t0 = max(14.0, tour_end + 0.3)
    dur = 12.0
    n = int(dur * RATE)
    src = array.array("d", bytes(8 * n))
    chords = [(220.0, 261.63, 329.63, 392.0, 493.88), (174.61, 220.0, 261.63, 329.63, 440.0),
              (196.0, 261.63, 329.63, 392.0, 523.25), (196.0, 246.94, 293.66, 392.0, 587.33)]
    pattern = [0, 2, 4, 3, 1, 3, 4, 2]
    step = 0.125
    for i in range(int(dur / step)):
        notes = chords[(i // 16) % len(chords)]
        add(src, pluck(notes[pattern[i % 8]], 0.9, 0.9 if i % 8 == 0 else 0.6), int(i * step * RATE))
    air = bandpass(noise(n, rng), 900, 0.7, 2400)
    air = lowpass(air, 5000)
    norm(air, -26)
    for i in range(n):
        src[i] += air[i]
    norm(env(src, 0.6, 0.9), -9)
    clip.place(0, t0, src, lambda s: ring_gains(360 * 2 * s / dur))

    # (c) the scene: bed in front (a slow pad and a little wind), events behind on stem B
    s0 = t0 + dur + 0.5
    dur = 13.0
    n = int(dur * RATE)
    bed = array.array("d", bytes(8 * n))
    for f, a in ((220.0, 1.0), (329.63, 0.7), (493.88, 0.45), (523.25, 0.4), (164.81, 0.6)):
        ph = rng.random() * TAU
        for i in range(n):
            tt = i / RATE
            bed[i] += a * math.sin(ph + TAU * f * tt + 0.004 * math.sin(TAU * 4.5 * tt)) * \
                (0.75 + 0.25 * math.sin(TAU * 0.13 * tt + f))
    wind = lowpass(bandpass(noise(n, rng), 500, 0.5), 1500)
    norm(wind, -6)
    for i in range(n):
        bed[i] += wind[i] * (0.6 + 0.4 * math.sin(TAU * 0.21 * i / RATE))
    norm(env(bed, 1.2, 1.8), -16)
    front = (0.62, 0.62, 0.45, 0.0, 0.0, 0.0)
    clip.place(0, s0, bed, front)

    def steps(count, gap, heavy=1.0):
        out = array.array("d", bytes(8 * int((count * gap + 0.3) * RATE)))
        for k in range(count):
            add(out, footstep(rng, heavy * (0.85 + 0.15 * (k % 2))), int(k * gap * RATE))
        return norm(out, -8)

    def clack():
        out = array.array("d", bytes(8 * int(0.6 * RATE)))
        add(out, click(rng, 3200), 0)
        add(out, click(rng, 2100, 0.03), int(0.16 * RATE))
        add(out, click(rng, 1400, 0.05), int(0.42 * RATE))
        return norm(out, -8)

    clip.place(1, s0 + 1.5, steps(4, 0.42), one(SL))
    clip.place(1, s0 + 4.0, clack(), one(SR))
    clip.place(1, s0 + 5.5, steps(4, 0.42, 0.9), one(SR))
    clip.place(1, s0 + 8.0, norm(click(rng, 4200, 0.015), -10), one(SL))
    wn = int(1.6 * RATE)
    wh = env(bandpass(noise(wn, rng), 600, 1.2, 3000), 0.5, 0.7)
    norm(wh, -9)
    # behind, from back-right (110) through straight behind (180) to back-left (250)
    clip.place(1, s0 + 9.0, wh, lambda s: ring_gains(110 + 140 * min(1.0, s / 1.6)))
    return clip


# ---- quantising and the file ------------------------------------------------------------------

def quantise(clip):
    """stems -> s16 with TPDF dither (seeded), gains -> Q15"""
    rng = random.Random(SEED + 1)
    stems = []
    for x in clip.stem:
        q = array.array("h", bytes(2 * len(x)))
        for i, v in enumerate(x):
            d = rng.random() - rng.random()
            q[i] = max(-32767, min(32767, int(math.floor(v * 32767 + d + 0.5))))
        stems.append(q)
    gains = [[tuple(max(0, min(32767, int(round(g * 32767)))) for g in blk) for blk in s]
             for s in clip.gain]
    return stems, gains


def expand_block(a, b, ga, gb, pa, pb):
    """the tap's maths for one block (surround_demo.h xdm_expand): 6 lists of 256 ints"""
    out = []
    for c in range(CHANNELS):
        a0, a1, b0, b1 = pa[c], ga[c], pb[c], gb[c]
        if not (a0 or a1 or b0 or b1):
            out.append([0] * BLOCK)
            continue
        ch = []
        for i in range(BLOCK):
            g1 = a0 + (((a1 - a0) * (i + 1)) >> 8)
            g2 = b0 + (((b1 - b0) * (i + 1)) >> 8)
            v = (a[i] * g1 + b[i] * g2) >> 15
            ch.append(max(-32768, min(32767, v)))
        out.append(ch)
    return out


def block_bound(a, b, ga, gb, pa, pb):
    """per channel, an upper bound of |sample| in the block (fast; the check uses the exact)"""
    ma, mb = max(map(abs, a)), max(map(abs, b))
    return [(ma * max(pa[c], ga[c]) + mb * max(pb[c], gb[c])) >> 15 for c in range(CHANNELS)]


def block_peaks(a, b, ga, gb, pa, pb):
    """per channel: the exact peak (the console's maths) where it could be loud, else the
    bound, which is then under -12 dBFS (most blocks: the exact maths in Python is slow)"""
    bound = block_bound(a, b, ga, gb, pa, pb)
    if max(bound) <= LOUD:
        return bound
    return [max(map(abs, ch)) for ch in expand_block(a, b, ga, gb, pa, pb)]


def write(path, stems, gains, peak_value):
    blocks = len(gains[0])
    pages = (blocks + PAGE_BLOCKS - 1) // PAGE_BLOCKS
    hdr = struct.pack(">4sHHIHHHHIIIII20x", MAGIC, VERSION, HDR, RATE, CHANNELS, STEMS, BLOCK,
                      PAGE_BLOCKS, PAGE, pages, blocks, blocks * BLOCK, peak_value)
    hdr += struct.pack(">I", zlib.crc32(hdr) & 0xFFFFFFFF)
    assert len(hdr) == HDR
    silent = (0,) * CHANNELS
    with open(path + ".tmp", "wb") as f:
        f.write(hdr)
        for p in range(pages):
            body = bytearray(struct.pack(">I", p))
            for k in range(PAGE_BLOCKS):
                blk = p * PAGE_BLOCKS + k
                if blk < blocks:
                    body += struct.pack(">12h", *gains[0][blk], *gains[1][blk])
                    for s in range(STEMS):
                        seg = stems[s][blk * BLOCK:(blk + 1) * BLOCK]
                        body += struct.pack(f">{BLOCK}h", *seg)
                else:
                    held = (gains[0][blocks - 1], gains[1][blocks - 1]) if blocks else (silent, silent)
                    body += struct.pack(">12h", *held[0], *held[1]) + bytes(2 * STEMS * BLOCK)
            assert len(body) == PAGE - 4
            f.write(struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))
            f.write(body)
    os.replace(path + ".tmp", path)
    return pages


def parse(data):
    """the header as a dict; ValueError on anything the console would refuse"""
    if len(data) < HDR:
        raise ValueError("shorter than the header")
    f = struct.unpack(">4sHHIHHHHIIIII20xI", data[:HDR])
    h = dict(zip("magic version hdr rate channels stems block page_blocks page_bytes pages "
                 "blocks frames peak crc".split(), f))
    if h["magic"] != MAGIC or h["version"] != VERSION or h["hdr"] != HDR:
        raise ValueError(f"not a version {VERSION} xw.demo: {h['magic']!r} v{h['version']}")
    if zlib.crc32(data[:HDR - 4]) & 0xFFFFFFFF != h["crc"]:
        raise ValueError("header CRC")
    if (h["rate"], h["channels"], h["stems"], h["block"], h["page_blocks"], h["page_bytes"]) != \
            (RATE, CHANNELS, STEMS, BLOCK, PAGE_BLOCKS, PAGE):
        raise ValueError(f"format {h}")
    if h["frames"] != h["blocks"] * BLOCK or h["pages"] != (h["blocks"] + PAGE_BLOCKS - 1) // PAGE_BLOCKS:
        raise ValueError("lengths")
    if len(data) != HDR + h["pages"] * PAGE:
        raise ValueError(f"file is {len(data)} bytes, header says {HDR + h['pages'] * PAGE}")
    return h


def check(path, quiet=False):
    """everything the console checks, and the peak limit on the console's own maths"""
    data = open(path, "rb").read()
    h = parse(data)
    pa = pb = (0,) * CHANNELS
    peaks = [0] * CHANNELS
    for p in range(h["pages"]):
        pg = data[HDR + p * PAGE:HDR + (p + 1) * PAGE]
        crc, idx = struct.unpack(">II", pg[:8])
        if zlib.crc32(pg[4:]) & 0xFFFFFFFF != crc or idx != p:
            raise ValueError(f"page {p}: CRC or index")
        for k in range(PAGE_BLOCKS):
            if p * PAGE_BLOCKS + k >= h["blocks"]:
                break
            r = pg[8 + k * REC:8 + (k + 1) * REC]
            g = struct.unpack(">12h", r[:24])
            ga, gb = g[:6], g[6:]
            if min(g) < 0:
                raise ValueError(f"page {p} block {k}: negative gain")
            a = struct.unpack(f">{BLOCK}h", r[24:24 + 2 * BLOCK])
            b = struct.unpack(f">{BLOCK}h", r[24 + 2 * BLOCK:])
            peaks = [max(p_, m) for p_, m in zip(peaks, block_peaks(a, b, ga, gb, pa, pb))]
            pa, pb = ga, gb
    top = max(peaks)
    if not quiet:
        print(f"{path}: {h['blocks']} blocks ({h['frames'] / RATE:.2f} s), {h['pages']} pages, "
              f"{len(data)} bytes; peaks " +
              ", ".join(f"{NAMES[c]} {dbfs(peaks[c])}" for c in range(CHANNELS)))
    if top > PEAK_LIMIT:
        raise ValueError(f"a channel peaks at {top} ({dbfs(top)} dBFS), over -6 dBFS")
    if top != h["peak"]:
        raise ValueError(f"header peak {h['peak']} but the clip peaks at {top}")
    return h, peaks


def dbfs(v):
    return "-inf" if not v else f"{20 * math.log10(v / 32768):+.1f}"


def make(path, voice=False, quiet=False):
    clip = compose(voice)
    stems, gains = quantise(clip)
    # master: scale both stems so the worst channel bound sits at PEAK_TARGET
    pa = pb = (0,) * CHANNELS
    worst = 0
    for blk in range(clip.blocks):
        sl = slice(blk * BLOCK, (blk + 1) * BLOCK)
        worst = max(worst, max(block_bound(stems[0][sl], stems[1][sl], gains[0][blk], gains[1][blk], pa, pb)))
        pa, pb = gains[0][blk], gains[1][blk]
    limit = int(32768 * db(PEAK_TARGET))
    if worst > limit:
        k = limit / worst
        for s in stems:
            for i in range(len(s)):
                s[i] = int(s[i] * k)
    # the peak, as the console computes it
    pa = pb = (0,) * CHANNELS
    top = 0
    for blk in range(clip.blocks):
        sl = slice(blk * BLOCK, (blk + 1) * BLOCK)
        top = max(top, max(block_peaks(stems[0][sl], stems[1][sl], gains[0][blk], gains[1][blk], pa, pb)))
        pa, pb = gains[0][blk], gains[1][blk]
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    pages = write(path, stems, gains, top)
    if not quiet:
        print(f"wrote {path}: {clip.blocks} blocks ({clip.blocks * BLOCK / RATE:.2f} s), "
              f"{pages} pages, {HDR + pages * PAGE} bytes, peak {dbfs(top)} dBFS"
              + (" (with spoken names)" if voice else ""))
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", default=OUT)
    ap.add_argument("--voice", action="store_true", help="speak each speaker's name (macOS say)")
    ap.add_argument("--check", metavar="FILE")
    ap.add_argument("-q", "--quiet", action="store_true")
    a = ap.parse_args()
    if a.check:
        try:
            check(a.check, a.quiet)
        except ValueError as e:
            raise SystemExit(f"{a.check}: {e}")
        return
    make(a.out, a.voice, a.quiet)
    check(a.out, a.quiet)


if __name__ == "__main__":
    main()
