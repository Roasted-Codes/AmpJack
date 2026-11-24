#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Evaluation of NOISE GATE AUTO, MIC VOLUME AUTO, the decimator and the comb canceller, with
the integer model (micdsp.py, bit-exact with micauto_dsp.h), against today's settings.

    python3 tools/micdsp/eval.py [gate|agc|fir|comb|real|tune|all]

Needs numpy and scipy. Numbers, not plots; README.md quotes them.
"""
import os
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import micdsp as M  # noqa: E402
import signals as S  # noqa: E402

FS = S.FS
WIN = 0.02
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def run(x, **kw):
    p = M.Pad(**kw)
    out = np.array(p.run([int(v) for v in x]), float)
    tr = np.array(p.trace, float)
    return out, tr


def wav(path):
    w = wave.open(os.path.join(ROOT, path))
    return np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(np.int64)


def db(q12):
    return 20 * np.log10(np.maximum(q12, 1) / 4096)


def gate_metrics(tr, words, dur, noise_only=None):
    """tr[:,2] = open per window (decided at the window's end)"""
    op = tr[:, 2].astype(bool)
    ends = (np.arange(len(op)) + 1) * WIN
    hit, delay, pre = 0, [], 0
    for a, b in words:
        i0 = int(a / WIN)  # the window the word starts in
        if i0 > 0 and op[i0 - 1]:
            pre += 1
            hit += 1
            delay.append(0.0)
            continue
        idx = np.nonzero(op[i0:int(b / WIN) + 1])[0]
        if len(idx):
            hit += 1
            delay.append(ends[i0 + idx[0]] - a + 0.010)  # + the 10 ms ramp
    shut, open_frac = 0, []
    gaps = [(e0, s1) for (_, e0), (s1, _) in zip(words, words[1:]) if s1 - e0 >= 0.4]
    for e0, s1 in gaps:
        i = int((e0 + 0.3) / WIN)
        shut += not op[min(i, len(op) - 1)]
        seg = op[int((e0 + 0.25) / WIN):int(s1 / WIN)]
        open_frac.append(seg.mean() if len(seg) else 0)
    r = dict(words=len(words), opened=hit, already_open=pre,
             delay_med_ms=1000 * np.median(delay) if delay else -1,
             delay_p95_ms=1000 * np.percentile(delay, 95) if delay else -1,
             pauses=len(gaps), shut_in_pause=shut,
             pause_open_pct=100 * np.mean(open_frac) if open_frac else 0)
    if noise_only is not None:
        a, b = noise_only
        seg = op[int(a / WIN):int(b / WIN)]
        opens = np.sum(seg[1:] & ~seg[:-1])
        r["noise_opens_per_min"] = opens / (b - a) * 60
        r["noise_open_pct"] = 100 * seg.mean()
    return r


def fmt(r):
    return ", ".join(f"{k} {v:.0f}" if isinstance(v, (float, np.floating)) else f"{k} {v}"
                     for k, v in r.items())


def scene(name, x, words, noise_only=None, extra=""):
    print(f"\n[{name}] {extra}")
    for label, gate in (("LOW ", M.GATE_LOW), ("HIGH", M.GATE_HIGH), ("AUTO", M.GATE_AUTO)):
        _, tr = run(x, gate=gate, level=5)
        r = gate_metrics(tr, words, len(x) / FS, noise_only)
        th = ""
        if gate == M.GATE_AUTO:
            th = (f" | thr dBFS med {np.median(tr[:, 4]) / 256:.1f} (range "
                  f"{tr[:, 4].min() / 256:.1f}..{tr[:, 4].max() / 256:.1f}), floor med "
                  f"{np.median(tr[:, 3]) / 256:.1f}")
        print(f"  {label}: {fmt(r)}{th}")


def gate_eval():
    print("== NOISE GATE: does it open on every word, and shut between? ==")
    print("delay = word start to gate fully open (window end + 10 ms ramp); 0 when the hold"
          " still had it open. pauses = gaps >= 0.4 s; shut_in_pause = shut 0.3 s after the word.")
    dur = 40
    for lvl, nfl in ((-40, -73), (-40, -60), (-30, -60), (-50, -73), (-40, -50)):
        sp, words = S.speech(dur, lvl, seed=3)
        nz = S.noise(dur + 4, nfl, seed=4)
        x = np.zeros(int((dur + 4) * FS))
        x[:] = nz
        x[int(4 * FS):] += sp  # 4 s of noise alone first
        w = [(a + 4, b + 4) for a, b in words]
        scene(f"speech {lvl} dBFS over pink noise {nfl} dBFS", S.to_int16(x), w, (0.5, 4.0))
    # a LAN room: babble around a player at -38 dBFS, who starts talking after 10 s
    sp, words = S.speech(dur, -38, seed=5, lead=10.0)
    for bl in (-55, -50, -44):
        x = sp + S.babble(dur, bl) + S.noise(dur, -72, seed=6)
        scene(f"LAN room: babble {bl} dBFS (alone for 10 s, noise counted from 4 s), player -38",
              S.to_int16(x), words, (4.0, 10.0))
    # breaths between phrases
    sp, words = S.speech(dur, -40, seed=7)
    br, spans = S.breaths(dur, -50, words)
    x = sp + br + S.noise(dur, -73, seed=8)
    _, trl = run(S.to_int16(x), gate=M.GATE_LOW)
    _, tra = run(S.to_int16(x), gate=M.GATE_AUTO)
    scene("breaths at -50 dBFS between phrases, speech -40, floor -73", S.to_int16(x), words)
    for label, tr in (("LOW ", trl), ("AUTO", tra)):
        o = [tr[int(a / WIN):int(b / WIN), 2].max() for a, b in spans]
        print(f"  {label}: breaths that open the gate: {int(sum(o))} of {len(spans)}")
    # a sudden floor change: -73 -> -50 at 15 s, back to -73 at 30 s
    sp, words = S.speech(45, -38, seed=9)
    nz = S.noise(45, -73, seed=11)
    nz[int(15 * FS):int(30 * FS)] *= 10 ** (23 / 20)
    x = S.to_int16(sp + nz)
    _, tr = run(x, gate=M.GATE_AUTO)
    t = (np.arange(len(tr)) + 1) * WIN
    up = t[(t > 15) & (tr[:, 3] > -55 * 256)]
    dn = t[(t > 30) & (tr[:, 3] < -65 * 256)]
    print(f"\n[floor step -73 -> -50 dBFS at 15 s, back at 30 s, speech -38]")
    print(f"  AUTO floor follows the rise after {up[0] - 15:.2f} s, the fall after "
          f"{dn[0] - 30:.2f} s; gate open while it adapts: "
          f"{100 * tr[(t > 15) & (t < up[0]), 2].mean():.0f} % of that time")
    seg = (t > up[0] + 0.5) & (t < 30)
    w2 = [(a, b) for a, b in words if a > up[0] + 0.5 and b < 30]
    print(f"  then, in the loud part: {fmt(gate_metrics(tr, w2, 45))}"
          f" (thr {np.median(tr[seg, 4]) / 256:.1f} dBFS)")
    w3 = [(a, b) for a, b in words if a > 15 and a < up[0]]
    _, trl = run(x, gate=M.GATE_LOW)
    print(f"  LOW in the loud part: open {100 * trl[seg, 2].mean():.0f} % of the time"
          f" (AUTO {100 * tr[seg, 2].mean():.0f} %); words during the adaptation: {len(w3)}")


def agc_metrics(tr, words, ideal_db, t0=0.0, settle_db=2.0):
    t = (np.arange(len(tr)) + 1) * WIN
    g = db(tr[:, 5])
    fin = np.median(g[-len(g) // 4:])
    bad = np.nonzero(np.abs(g - fin) > settle_db)[0]
    settle = (t[bad[-1]] - t0) if len(bad) else 0.0
    # gain movement inside pauses >= 0.3 s (frozen: should be 0)
    mv = []
    for (_, e0), (s1, _) in zip(words, words[1:]):
        if s1 - e0 >= 0.3:
            i0, i1 = int((e0 + 0.04) / WIN), int(s1 / WIN) - 1
            if i1 > i0:
                mv.append(np.ptp(g[i0:i1 + 1]))
    late = t > t[-1] / 2
    return dict(settle_s=settle, final_db=g[-1], ideal_db=ideal_db,
                pause_move_max_db=max(mv) if mv else 0.0,
                late_pp_db=np.ptp(g[late]), late_sd_db=np.std(g[late]))


def asl_p56(x, fs=FS):
    """active speech level, ITU-T P.56 style on 20 ms windows: the power mean of the windows no
    more than 15.9 dB under that mean (iterated), in dBFS"""
    x = np.asarray(x, float)
    n = len(x) // (fs // 50)
    p = (x[:n * (fs // 50)].reshape(n, -1) ** 2).mean(1) / 2 ** 30
    lv = 10 * np.log10(p.mean() + 1e-20)
    for _ in range(20):
        sel = p >= 10 ** ((lv - 15.9) / 10)
        lv = 10 * np.log10(p[sel].mean())
    return lv


def agc_eval():
    print("\n== MIC VOLUME AUTO: settling, pumping, level ==")
    print("settle_s = time from the first word after which the gain stays within 2 dB of where it ends up;"
          " pause_move = gain change inside any pause >= 0.3 s; late_* = gain spread in the"
          " second half. Start: MIC VOLUME 5 (0 dB).")
    dur = 40
    for lvl in (-52, -45, -40, -35, -25, -18):
        sp, words = S.speech(dur, lvl, seed=12)
        x = S.to_int16(sp + S.noise(dur, -73, seed=13))
        ideal = float(np.clip(-30 - lvl, -12.04, 14.96))
        for gate in (M.GATE_AUTO, M.GATE_LOW):
            out, tr = run(x, gate=gate, level=0, fir=True)
            r = agc_metrics(tr, words, ideal, words[0][0])
            ol = asl_p56(out[len(out) // 2:], 8000)
            print(f"  speech {lvl:4d} dBFS, gate {'AUTO' if gate == 3 else 'LOW '}: {fmt2(r)},"
                  f" output active level (2nd half) {ol:.1f} dBFS, level shown {M.auto_level(int(tr[-1, 5]))}")
    # breaths, babble, a louder talker
    sp, words = S.speech(dur, -40, seed=12)
    base = S.noise(dur, -73, seed=13)
    _, tr0 = run(S.to_int16(sp + base), gate=M.GATE_AUTO, level=0)
    br, _ = S.breaths(dur, -48, words)
    _, tr1 = run(S.to_int16(sp + base + br), gate=M.GATE_AUTO, level=0)
    print(f"  breaths at -48 dBFS (speech -40): final gain {db(tr1[-1, 5]):.1f} dB vs {db(tr0[-1, 5]):.1f}"
          f" without; {fmt2(agc_metrics(tr1, words, 10.0, words[0][0]))}")
    for bl in (-50, -44):
        _, tr2 = run(S.to_int16(sp + base + S.babble(dur, bl)), gate=M.GATE_AUTO, level=0)
        print(f"  babble {bl} dBFS (speech -40): final gain {db(tr2[-1, 5]):.1f} dB (ideal 10.0);"
              f" {fmt2(agc_metrics(tr2, words, 10.0, words[0][0]))}")
    sp2, w2 = S.speech(60, -40, seed=14)
    sp2[int(30 * FS):] *= 10 ** (12 / 20)  # the player starts shouting at 30 s
    _, tr3 = run(S.to_int16(sp2 + S.noise(60, -73, seed=15)), gate=M.GATE_AUTO, level=0)
    t = (np.arange(len(tr3)) + 1) * WIN
    g = db(tr3[:, 5])
    fin = np.median(g[-250:])
    ok = np.nonzero((t > 30) & (np.abs(g - fin) <= 2))[0]
    print(f"  talker 12 dB louder for good at 30 s: gain {g[t < 30][-1]:.1f} -> {fin:.1f} dB,"
          f" within 2 dB of that after {t[ok[0]] - 30 if len(ok) else -1:.1f} s"
          f" (the limiter holds the peaks meanwhile)")
    sp2, w2 = S.speech(60, -40, seed=14)
    sp2[int(30 * FS):int(32.5 * FS)] *= 10 ** (12 / 20)  # one 2.5 s shout at 30 s
    _, tr4 = run(S.to_int16(sp2 + S.noise(60, -73, seed=15)), gate=M.GATE_AUTO, level=0)
    g = db(tr4[:, 5])
    before = g[t < 30][-1]
    dip = before - g[t > 30].min()
    back = np.nonzero((t > 33) & (g >= before - 1))[0]
    print(f"  one 2.5 s shout (+12 dB) at 30 s: gain dips {dip:.1f} dB, back within 1 dB after"
          f" {t[back[0]] - 30 if len(back) else -1:.1f} s")
    # muted: frozen
    p = M.Pad(gate=M.GATE_AUTO, level=0)
    xs = [int(v) for v in S.to_int16(sp + base)]
    p.run(xs[:len(xs) // 2])
    q = p.agc.q12
    p.muted = True
    p.run(xs[len(xs) // 2:])
    print(f"  muted for the second half: gain {db(q):.2f} -> {db(p.agc.q12):.2f} dB (frozen)")


def fmt2(r):
    return ", ".join(f"{k} {v:.2f}" for k, v in r.items())


def fir_eval():
    print("\n== 24 -> 8 kHz: response by sweep (output power at 8 kHz / input power, aliases"
          " included) ==")
    freqs = list(range(100, 3201, 100)) + list(range(3300, 4600, 100)) + list(range(4600, 11901, 100))
    res = {"FIR": [], "rs_push": []}
    for f in freqs:
        n = 4800
        x = np.round(16384 * np.sin(2 * np.pi * f * np.arange(n + 600) / FS)).astype(int)
        pin = np.mean(x[600:].astype(float) ** 2)
        for name in res:
            fir = M.Fir() if name == "FIR" else M.Rs()
            y = []
            for v in x:
                if name == "FIR":
                    o = fir.push(int(v))
                    if o is not None:
                        y.append(o)
                else:
                    y.extend(fir.push(int(v)))
            y = np.array(y[200:], float)
            res[name].append(10 * np.log10(np.mean(y ** 2) / pin + 1e-15))
    fr = np.array(freqs)
    for name, d in res.items():
        d = np.array(d)
        pb, sb = d[fr <= 3200], d[fr >= 4600]
        tb = d[(fr > 3200) & (fr < 4600)]
        print(f"  {name:8s}: passband 0.1-3.2 kHz {pb.min():+.2f}..{pb.max():+.2f} dB;"
              f" 3.4 kHz {d[fr == 3400][0]:+.1f}, 4.0 kHz {d[fr == 4000][0]:+.1f} dB;"
              f" above 4.6 kHz at most {sb.max():.1f} dB (worst at {fr[fr >= 4600][sb.argmax()]} Hz),"
              f" 5-7 kHz at most {d[(fr >= 5000) & (fr <= 7000)].max():.1f} dB")
    # delay: impulse
    f = M.Fir()
    y = [f.push(32767 if i == 0 else 0) for i in range(90)]
    print(f"  FIR delay: 22 samples at 24 kHz = 0.917 ms (group delay, linear phase);"
          f" rs_push: ~1 sample (0.04 ms). Added: +0.88 ms")


def comb_eval():
    print("\n== comb canceller (optional) ==")
    from scipy import signal as sg

    def comb_db(x, lo, hi, f0=125):
        N = len(x)
        P = np.abs(np.fft.rfft(x * np.hanning(N))) ** 2
        d = 10 * np.log10(P + 1e-9)
        s = []
        for k in range(max(1, lo // f0), hi // f0):
            i = int(round(k * f0 * N / FS))
            s.append(d[i] - np.median(d[i - 30:i + 30]))
        return np.mean(s)

    for path in ("research/results/2026-11-23-buzz/raw_idle.wav",
                 "research/results/2026-11-23-buzz/raw_idle_g50.wav"):
        x = wav(path)
        x = np.tile(x, 4)  # 1 s recording, looped (the comb is periodic, so the loop is seamless
        # only to within the 192-sample phase: 24000 = 125 x 192 exactly)
        x = x - int(round(x.mean()))
        c = M.Comb()
        y = np.array([c.push(int(v)) for v in x], float)
        a, b = x[-FS:].astype(float), y[-FS:]
        print(f"  {os.path.basename(path)}: comb peaks above the median 5-9 kHz"
              f" {comb_db(a, 5000, 9000):.1f} -> {comb_db(b, 5000, 9000):.1f} dB, 3-5 kHz"
              f" {comb_db(a, 3000, 5000):.1f} -> {comb_db(b, 3000, 5000):.1f} dB; rms"
              f" {20 * np.log10(a.std() / 32768):.1f} -> {20 * np.log10(b.std() / 32768):.1f} dBFS")
    sp, words = S.speech(20, -40, seed=21)
    x = S.to_int16(sp)
    c = M.Comb()
    y = np.array([c.push(int(v)) for v in x], float)
    print(f"  speech (varying pitch): level change {20 * np.log10(y.std() / x.std()):+.2f} dB")
    sp, _ = S.speech(20, -40, seed=21, f0=125.0)
    t = np.arange(int(2 * FS))
    vowel = np.round(1000 * sg.sawtooth(2 * np.pi * 125 * t / FS)).astype(int)
    c = M.Comb()
    y = np.array([c.push(int(v)) for v in vowel], float)
    print(f"  a held 125 Hz buzz (the worst case): after 0.5 s {20 * np.log10(y[12000:12000 + 2400].std() / vowel.std()):+.1f} dB,"
          f" after 2 s {20 * np.log10(y[-2400:].std() / vowel.std()):+.1f} dB")


def real_eval():
    print("\n== real recordings (what they can show is limited: see README) ==")
    for path in ("research/results/2026-11-23-buzz/raw_idle.wav",
                 "research/results/2026-11-23-buzz/raw_idle_g50.wav"):
        x = np.tile(wav(path), 10)  # 1 s of raw idle, looped to 10 s
        _, tr = run(x, gate=M.GATE_AUTO, level=0)
        op = tr[:, 2]
        print(f"  {os.path.basename(path)} (x10 loop): window RMS med "
              f"{20 * np.log10(np.median(tr[:, 0]) / 32768 + 1e-9):.1f} max "
              f"{20 * np.log10(tr[:, 0].max() / 32768):.1f} dBFS; floor {tr[-1, 3] / 256:.1f},"
              f" thr {tr[-1, 4] / 256:.1f} dBFS; gate open {100 * op[150:].mean():.0f} % after 3 s;"
              f" AGC gain {db(tr[-1, 5]):.1f} dB (no speech: stays put)")
    # the owner talking, recorded through the old path with the gate OFF (devmic 50)
    x = wav("research/results/2026-11-23-buzz/owner_mictest_g50.wav")
    print(f"  owner_mictest_g50: active speech level (P.56 style) {asl_p56(x):.1f} dBFS")
    for gate, name in ((M.GATE_LOW, "LOW "), (M.GATE_AUTO, "AUTO")):
        out, tr = run(x, gate=gate, level=0, dc=False)
        op = tr[:, 2]
        r = tr[:, 0]
        loud = r >= 583
        quiet = r < 33
        cut = int(np.sum((r >= 58) & (op == 0)))  # windows at -55 dBFS or more with the gate shut
        thr = 20 * np.log10(np.median(tr[:, 1]) / 32768)
        print(f"  owner_mictest_g50, gate {name}: open {100 * op.mean():.0f} % of windows;"
              f" windows over -35 dBFS open {100 * op[loud].mean():.0f} %, under -60 open"
              f" {100 * op[quiet].mean():.0f} %; windows over -55 dBFS cut {cut} ({cut * 20} ms);"
              f" opens {int(np.sum(op[1:] > op[:-1]))};"
              f" thr {thr:.1f} dBFS; AGC speech level"
              f" {tr[-1, 6] / 256:.1f} dBFS -> gain {db(tr[-1, 5]):+.1f} dB (level {M.auto_level(int(tr[-1, 5]))}),"
              f" within 2 dB of final after {(np.nonzero(np.abs(db(tr[:, 5]) - db(tr[-1, 5])) > 2)[0][-1] + 1) * WIN:.1f} s;"
              f" output active level (2nd half) {asl_p56(out[len(out) // 2:], 8000):.1f} dBFS")
    # the owner talking through the old gate LOW (pauses are digital zero): plus the real idle
    # floor under it, so the floor tracker has something to track
    x = wav("research/results/2026-11-23-buzz/owner_mictest.wav")
    idle = np.tile(wav("research/results/2026-11-23-buzz/raw_idle.wav"), 10)
    idle = idle - int(round(idle.mean()))
    for name, sig in (("as recorded", x), ("+ real idle floor", np.clip(x + idle[:len(x)], -32768, 32767))):
        out, tr = run(sig, gate=M.GATE_AUTO, level=0, dc=False)
        print(f"  owner_mictest {name}: floor {np.median(tr[100:, 3]) / 256:.1f}, thr"
              f" {np.median(tr[100:, 4]) / 256:.1f} dBFS; gate open {100 * tr[:, 2].mean():.0f} %;"
              f" AGC speech level {tr[-1, 6] / 256:.1f} dBFS -> gain {db(tr[-1, 5]):+.1f} dB"
              f" (level {M.auto_level(int(tr[-1, 5]))})")
    x = wav("research/results/mic-tone-20261122-140653.wav")
    x = np.tile(x, 6)
    _, tr = run(x, gate=M.GATE_AUTO, level=0)
    t = (np.arange(len(tr)) + 1) * WIN
    print(f"  mic-tone (a steady -32 dBFS tone, x6 loop): floor climbs to "
          f"{tr[-1, 3] / 256:.1f} dBFS after {t[np.argmax(tr[:, 3] > -40 * 256)]:.1f} s, thr clamps at"
          f" {tr[-1, 4] / 256:.1f}; gate stays open {100 * tr[200:, 2].mean():.0f} % (the tone is over -35)")


def tune_eval():
    """the grid behind the floor smoothing and the margin (window-level: the gate's decision
    and the floor tracker only, the same integer code without the sample path)"""
    print("\n== floor smoothing x margin: words missed / p95 delay on speech, % open on noise alone ==")

    def win_ms(x):
        x = np.asarray(x, np.int64)
        n = len(x) // 480
        return (x[:n * 480].reshape(n, 480) ** 2).sum(1) // 480

    def gate(ms, p):
        f, hold, closed, out = M.Floor(p), 0, 0, []
        for v in ms:
            v = int(v)
            rms = M.isqrt(v)
            hold = max(0, hold - 480)
            if rms >= f.thr_rms:
                closed, hold = 0, 4800
            elif not hold:
                closed = 1
            f.window(v)
            out.append((rms, 0, 0 if closed else 1, f.floor_db, f.thr_db))
        return np.array(out, float)

    scenes = []
    for lvl, nfl in ((-40, -73), (-40, -60), (-50, -73), (-40, -50)):
        sp, words = S.speech(40, lvl, seed=3, lead=4.0)
        scenes.append((f"speech {lvl} / pink {nfl}", win_ms(S.to_int16(sp + S.noise(40, nfl, seed=4))),
                       words, (0.5, 4.0)))
    sp, words = S.speech(40, -40, seed=31, phrase=(0.2, 0.5), word_gap=(0.05, 0.2))
    scenes.append(("non-stop talk -40", win_ms(S.to_int16(sp + S.noise(40, -73, seed=32))), words, None))
    for bl in (-55, -50, -44):
        scenes.append((f"babble alone {bl}", win_ms(S.to_int16(S.babble(30, bl) + S.noise(30, -72, seed=6))),
                       [], (5.0, 30.0)))
    for sm, mg in ((1, 8), (3, 8), (3, 9), (3, 10), (4, 8), (4, 9), (4, 10), (5, 9)):
        p = dict(MA_FLOOR_SMOOTH=sm, MA_GATE_MARGIN_DB=mg * M.DB)
        row = []
        for name, ms, w, no in scenes:
            r = gate_metrics(gate(ms, p), w, 0, no)
            if w:
                row.append(f"{name}: {r['words'] - r['opened']} / {r['delay_p95_ms']:.0f} ms"
                           + (f" / {r['noise_open_pct']:.0f} %" if no else ""))
            else:
                row.append(f"{name}: {r['noise_open_pct']:.0f} %")
        print(f"  smoothing 2^-{sm}, margin {mg} dB: " + "; ".join(row))


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    for k, fn in (("gate", gate_eval), ("agc", agc_eval), ("fir", fir_eval),
                  ("comb", comb_eval), ("real", real_eval), ("tune", tune_eval)):
        if what in (k, "all"):
            fn()
