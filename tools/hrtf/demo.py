#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render headphone-surround demos with the integer reference model, for listening on a Mac.

    python3 -I tools/hrtf/demo.py [--taps 64] [--out research/results/surround-demo]

Uses exactly the committed table (xbox360/surround_tab.h) through xsr_ref.py, which matches
the console's C bit for bit, so what you hear is what the plugin will produce from the same
six channels. "plain" files are today's downmix (game_360.c, LFE left out).
"""
import argparse
import os
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # xsr_ref (this directory only)

import numpy as np  # noqa: E402
from scipy import signal  # noqa: E402

import xsr_ref  # noqa: E402

RATE = 48000
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WALK = (("front-left", xsr_ref.FL), ("centre", xsr_ref.C), ("front-right", xsr_ref.FR),
        ("surround-right", xsr_ref.SR), ("surround-left", xsr_ref.SL))
rng = np.random.default_rng(2026)


def pink(n):
    spec = np.fft.rfft(rng.standard_normal(n))
    p = np.fft.irfft(spec / np.sqrt(np.maximum(np.arange(n // 2 + 1), 1)), n)
    p = signal.sosfilt(signal.butter(2, 60, "hp", fs=RATE, output="sos"), p)
    return p / np.sqrt(np.mean(p ** 2))


def ramp(x, ms=10):
    k = int(RATE * ms / 1000)
    w = np.ones(len(x))
    w[:k] = np.sin(np.linspace(0, np.pi / 2, k)) ** 2
    w[-k:] = w[:k][::-1]
    return x * w


def vowel(n, f0, formants):
    """voice-like: a glottal pulse train with vibrato through formant resonators"""
    t = np.arange(n) / RATE
    f = f0 * (1 + 0.02 * np.sin(2 * np.pi * 5.5 * t))
    ph = np.cumsum(f) / RATE
    src = (ph % 1.0) ** 3 - 0.25          # a sharp-edged pulse, rich in harmonics
    src = np.diff(src, prepend=src[0])
    y = np.zeros(n)
    for fc, bw, g in formants:
        r = np.exp(-np.pi * bw / RATE)
        y += g * signal.lfilter([1 - r], [1, -2 * r * np.cos(2 * np.pi * fc / RATE), r * r], src)
    return y / np.sqrt(np.mean(y ** 2))


VOWELS = {  # (formant Hz, bandwidth Hz, gain)
    "ah": [(730, 90, 1.0), (1090, 110, 0.5), (2440, 160, 0.25)],
    "ee": [(270, 60, 1.0), (2290, 100, 0.4), (3010, 120, 0.3)],
    "oo": [(300, 60, 1.0), (870, 80, 0.4), (2240, 120, 0.1)],
    "eh": [(530, 70, 1.0), (1840, 100, 0.5), (2480, 140, 0.3)],
    "oh": [(570, 80, 1.0), (840, 90, 0.6), (2410, 140, 0.15)],
}


def dbfs(x, db):
    return x * 32767 * 10 ** (db / 20)


def to16(x):
    return np.clip(np.round(x), -32768, 32767).astype(np.int64)


def write(path, lr):
    data = xsr_ref.interleave(lr).astype("<i2").tobytes()
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)
    return len(data) + 44


def walk(sound, on_s, gap_s, level_db):
    """one sound per speaker in WALK order; returns the six channels"""
    on, gap = int(on_s * RATE), int(gap_s * RATE)
    x = np.zeros((6, len(WALK) * (on + gap)))
    for k, (_, ch) in enumerate(WALK):
        x[ch, k * (on + gap):k * (on + gap) + on] = dbfs(ramp(sound(k, on)), level_db)
    return to16(x)


def scene(seconds):
    """a 5.1 test scene: something different on every channel, and a sound circling the room"""
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    x = np.zeros((6, n))
    # front-left: plucked notes (Karplus-Strong)
    for i, note in enumerate([220, 277, 330, 440, 330, 277, 220, 165] * 2):
        start = int(i * 0.37 * RATE)
        if start >= n:
            break
        period = int(RATE / note)
        buf = rng.uniform(-1, 1, period)
        out = np.zeros(min(int(0.9 * RATE), n - start))
        for j in range(len(out)):
            out[j] = buf[j % period]
            buf[j % period] = 0.497 * (buf[j % period] + buf[(j + 1) % period])
        x[xsr_ref.FL, start:start + len(out)] += dbfs(out, -14)
    # front-right: ticks (short filtered noise bursts)
    sos = signal.butter(2, [5000, 12000], "bp", fs=RATE, output="sos")
    for start in range(int(0.1 * RATE), n - 2400, int(0.25 * RATE)):
        b = signal.sosfilt(sos, rng.standard_normal(2400)) * np.exp(-np.arange(2400) / 300)
        x[xsr_ref.FR, start:start + 2400] += dbfs(b / np.max(np.abs(b)), -16)
    # centre: a voice-like vowel sequence
    seg = int(0.6 * RATE)
    for i, v in enumerate(["ah", "ee", "oo", "eh", "oh", "ah", "ee", "oo", "eh", "oh"]):
        s = i * seg
        if s + seg > n:
            break
        x[xsr_ref.C, s:s + seg - 2400] += dbfs(ramp(vowel(seg - 2400, 120 + 10 * (i % 3), VOWELS[v])), -18)
    # surrounds: rain-like ambience, different on each side
    for ch, fc in ((xsr_ref.SL, 3000), (xsr_ref.SR, 3600)):
        r = signal.sosfilt(signal.butter(2, [fc / 3, fc], "bp", fs=RATE, output="sos"), rng.standard_normal(n))
        mod = 0.6 + 0.4 * np.sin(2 * np.pi * (0.3 if ch == xsr_ref.SL else 0.37) * t)
        x[ch] += dbfs(r / np.sqrt(np.mean(r ** 2)) * mod, -30)
    # a noise source circling the room once (pairwise constant-power pan around the 5 speakers)
    ring = [(xsr_ref.C, 0), (xsr_ref.FR, -30), (xsr_ref.SR, -110), (xsr_ref.SL, 110), (xsr_ref.FL, 30)]
    az = (-360 * t / seconds) % 360  # clockwise, seen from above
    whoosh = pink(n) * (0.7 + 0.3 * np.sin(2 * np.pi * 7 * t))
    angles = [a % 360 for _, a in ring] + [360]
    chans = [c for c, _ in ring] + [ring[0][0]]
    order = np.argsort(angles[:-1])
    angles = [angles[i] for i in order] + [angles[order[0]] + 360]
    chans = [chans[i] for i in order] + [chans[order[0]]]
    for k in range(len(angles) - 1):
        a0, a1 = angles[k], angles[k + 1]
        sel = (az >= a0) & (az < a1)
        p = (az[sel] - a0) / (a1 - a0)
        x[chans[k], sel] += dbfs(whoosh[sel] * np.cos(p * np.pi / 2), -20)
        x[chans[k + 1], sel] += dbfs(whoosh[sel] * np.sin(p * np.pi / 2), -20)
    # LFE: a low thump every 1.5 s
    for start in range(0, n - RATE // 2, int(1.5 * RATE)):
        k = np.arange(RATE // 2)
        x[xsr_ref.LFE, start:start + len(k)] += dbfs(np.sin(2 * np.pi * 45 * k / RATE) * np.exp(-k / 6000), -10)
    return to16(x)


def switched(x, tab, every_s):
    """plain and virtual alternating, each switch a one-block crossfade (xsr_crossfade)"""
    on, _ = xsr_ref.process(tab, x)
    off = xsr_ref.downmix(x)
    out = off.copy()
    blk = 256
    per = int(every_s * RATE) // blk
    for b in range(x.shape[1] // blk):
        s = slice(b * blk, (b + 1) * blk)
        phase = (b // per) % 2          # 0 plain, 1 virtual
        out[:, s] = on[:, s] if phase else off[:, s]
        if b and b % per == 0:          # the first block of a new phase fades into it
            src, dst = (off, on) if phase else (on, off)
            out[:, s] = xsr_ref.crossfade(src[:, s], dst[:, s])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--taps", type=int, default=64)
    ap.add_argument("--out", default=os.path.join(ROOT, "research", "results", "surround-demo"))
    args = ap.parse_args()
    tab = xsr_ref.parse_tab(os.path.join(ROOT, "xbox360", "surround_tab.h"))[args.taps]
    os.makedirs(args.out, exist_ok=True)
    files = {}

    x = walk(lambda k, n: pink(n), 0.55, 0.2, -16)
    files["walk-noise-surround.wav"] = xsr_ref.process(tab, x)
    files["walk-noise-plain.wav"] = (xsr_ref.downmix(x), 0)
    names = list(VOWELS)
    x = walk(lambda k, n: vowel(n, 120, VOWELS[names[k]]), 0.8, 0.25, -16)
    files["walk-voice-surround.wav"] = xsr_ref.process(tab, x)
    x = scene(6.0)
    files["scene-plain.wav"] = (xsr_ref.downmix(x), 0)
    files["scene-surround.wav"] = xsr_ref.process(tab, x)
    files["scene-switching.wav"] = (switched(x, tab, 1.5), 0)

    for name, (lr, clamps) in files.items():
        size = write(os.path.join(args.out, name), lr)
        peak = 20 * np.log10(max(1, np.max(np.abs(lr))) / 32767)
        print(f"{name}: {lr.shape[1] / RATE:.2f} s, {size / 1e6:.2f} MB, peak {peak:+.1f} dBFS, "
              f"clamped {clamps}")


if __name__ == "__main__":
    main()
