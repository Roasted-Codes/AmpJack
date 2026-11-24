# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic test signals for the mic DSP evaluation (numpy; floats here are fine, only the
model under test is integer). Everything is seeded, so a run is repeatable.

speech(): words of 1-3 voiced syllables, high-passed at 80 Hz like a headset mic (a jittered glottal pulse train through two formant
resonators and a tilt), 30 % of words starting with a fricative 12-18 dB below the vowel (the
"soft starts" a gate chops first), syllable peaks varying +/-5 dB, gaps of 20-60 ms inside a
word, 120-500 ms between words and 0.8-2.0 s between phrases. Returns the samples and the
ground truth: the word intervals. Scaled so its active level (mean power inside the words) is
the requested dBFS.
"""
import numpy as np
from scipy import signal

FS = 24000


def _db(x):
    return 10 * np.log10(np.mean(np.asarray(x, float) ** 2) + 1e-20) - 20 * np.log10(32768)


def _scale_to(x, db, mask=None):
    p = _db(x[mask] if mask is not None else x)
    return x * 10 ** ((db - p) / 20)


def _resonator(x, f, bw):
    r = np.exp(-np.pi * bw / FS)
    a = [1, -2 * r * np.cos(2 * np.pi * f / FS), r * r]
    return signal.lfilter([1 - r], a, x)


def _syllable(rng, n, f0):
    t = np.arange(n)
    f = f0 * (1 + 0.06 * np.sin(2 * np.pi * rng.uniform(3, 6) * t / FS + rng.uniform(0, 6)))
    ph = np.cumsum(f / FS) + rng.normal(0, 0.002, n).cumsum() * 0.01
    src = np.diff(np.floor(ph), prepend=0).astype(float)  # one pulse per period
    src = signal.lfilter([1], [1, -0.95], src)  # glottal tilt
    v = _resonator(src, rng.uniform(300, 800), 90) + 0.6 * _resonator(src, rng.uniform(900, 2500), 140)
    v += 0.2 * _resonator(src, rng.uniform(2500, 3500), 200)
    a = int(rng.uniform(0.015, 0.04) * FS)
    r = int(rng.uniform(0.04, 0.08) * FS)
    env = np.ones(n)
    env[:a] = 0.5 - 0.5 * np.cos(np.pi * np.arange(a) / a)
    env[-r:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(r) / r)
    v = v * env
    return v / (np.sqrt(np.mean(v ** 2)) + 1e-12)


def _fricative(rng, n):
    x = rng.normal(0, 1, n)
    b, a = signal.butter(2, [3000, 8000], "bandpass", fs=FS)
    x = signal.lfilter(b, a, x)
    env = np.sin(np.pi * np.arange(n) / n) ** 0.5
    x = x * env
    return x / (np.sqrt(np.mean(x ** 2)) + 1e-12)


def speech(dur, level_db, seed=1, f0=None, lead=0.6, phrase=(0.8, 2.0), word_gap=(0.12, 0.5)):
    rng = np.random.default_rng(seed)
    n = int(dur * FS)
    x = np.zeros(n)
    words = []
    f0 = f0 or rng.uniform(100, 200)
    t = lead
    while t < dur - 1.0:
        for _ in range(rng.integers(3, 9)):
            if t > dur - 0.8:
                break
            w0 = t
            if rng.uniform() < 0.3:
                m = int(rng.uniform(0.06, 0.12) * FS)
                i = int(t * FS)
                x[i:i + m] += _fricative(rng, m)[:n - i] * 10 ** (-rng.uniform(12, 18) / 20)
                t += m / FS * 0.8
            for _ in range(rng.integers(1, 4)):
                m = int(rng.uniform(0.12, 0.3) * FS)
                i = int(t * FS)
                s = _syllable(rng, m, f0 * rng.uniform(0.85, 1.2)) * 10 ** (rng.uniform(-5, 5) / 20)
                x[i:i + m] += s[:max(0, n - i)]
                t += m / FS + rng.uniform(0.02, 0.06)
            words.append((w0, t))
            t += rng.uniform(*word_gap)
        t += rng.uniform(*phrase)
    # a headset mic passes nothing under ~80 Hz (and the glottal tilt above has a DC part, which
    # the plugin's DC tracker would turn into slow tails after every syllable)
    b, a = signal.butter(2, 80, "highpass", fs=FS)
    x = signal.lfilter(b, a, x)
    mask = np.zeros(n, bool)
    for a, b in words:
        mask[int(a * FS):int(b * FS)] = True
    return _scale_to(x, level_db, mask), words


def noise(dur, level_db, seed=2, color="pink"):
    rng = np.random.default_rng(seed)
    n = int(dur * FS)
    w = rng.normal(0, 1, n)
    if color == "pink":
        X = np.fft.rfft(w)
        f = np.fft.rfftfreq(n, 1 / FS)
        f[0] = f[1]
        w = np.fft.irfft(X / np.sqrt(f), n)
    return _scale_to(w, level_db)


def babble(dur, level_db, talkers=8, seed=10):
    x = np.zeros(int(dur * FS))
    for k in range(talkers):
        s, _ = speech(dur, -30, seed=seed + k, lead=0.05 * k, phrase=(0.2, 0.8), word_gap=(0.05, 0.25))
        x += s * 10 ** (np.random.default_rng(seed + 100 + k).uniform(-6, 0) / 20)
    b, a = signal.butter(2, 4000, fs=FS)  # across a room: the highs are gone
    return _scale_to(signal.lfilter(b, a, x), level_db)


def breaths(dur, level_db, words, seed=20, n_max=None):
    """a 0.4-0.6 s breath in each pause over 0.9 s, ending 0.15 s before the next word"""
    rng = np.random.default_rng(seed)
    n = int(dur * FS)
    x = np.zeros(n)
    b, a = signal.butter(2, [300, 3000], "bandpass", fs=FS)
    spans = []
    for (_, e0), (s1, _) in zip(words, words[1:]):
        if s1 - e0 < 0.9:
            continue
        m = int(rng.uniform(0.4, 0.6) * FS)
        i = int((s1 - 0.15) * FS) - m
        br = signal.lfilter(b, a, rng.normal(0, 1, m)) * np.sin(np.pi * np.arange(m) / m) ** 2
        br /= np.sqrt(np.mean(br ** 2))
        x[i:i + m] += br * 10 ** (level_db / 20) * 32768
        spans.append((i / FS, (i + m) / FS))
        if n_max and len(spans) >= n_max:
            break
    return x, spans


def to_int16(x):
    return np.clip(np.round(x), -32768, 32767).astype(np.int64)
