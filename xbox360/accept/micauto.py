#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for MIC-AUTO (micdsp_360.c; docs/ROADMAP.md MIC-AUTO, Part 2 A).

Needs: an xw build with MIC-AUTO, the pad on and its jack headset sending the mic to a title
that listens (audio stage 4: a Halo 3 lobby, a party), the room quiet while step 4 runs.

  XBOX=10.0.0.13 python3 xbox360/accept/micauto.py [--fresh] [--hands] [--voicetest]

  --fresh      the build has just booted and nothing was written since: check the defaults
  --hands      also run the the owner checks (y / n); without it they are printed as TODO lines
  --voicetest  also run voice.py test (a title that records what the 360 gets from the mic)
               with the 8 kHz filter off and on, and compare the 1 kHz tone's level. It
               leaves the running game for about a minute (Aurora comes back).

ACCEPT_READ_ONLY=1 (accept/run.py --read-only): steps 1-2 only, nothing written.

Side effects, all put back at the end: the mic-auto switches, XWDG gate / mic_gain, XWDG
work_max_us (zeroed to measure).

Checks (one PASS / FAIL line each; exit status 1 if any failed):
  1. the mic-auto block (version 1) and XWDG version 5;
  2. --fresh: NOISE GATE AUTO switched on with its memory, every other switch off, gate auto,
     MIC VOLUME 100 % (level 5);
  3. the manual settings are today's numbers, with every switch off and again with both
     AUTOs offered (not picked): gate steps 1..4 at 33 58 104 184 (-60 -55 -50 -45), a gate set to auto
     with NOISE GATE AUTO off 58 (step 2); mic_gain_q15 per MIC VOLUME step 8192 .. 183496 (accept/wp1_4.py's);
  4. both AUTOs offered: memory within 4 KB, the headset's pad tracked; after 5 s of quiet the
     threshold is the floor + 9 dB within -60 .. -35 dBFS, and with the gate on auto XWDG
     uses it (gate_threshold = thr_rms);
  5. the 8 kHz filter on: it makes 8000 samples a second, nothing bypassed, and the 360 gets
     the mic at the same pace as with it off (XV36 cap_bytes, +-2 %, no new mic_dropped);
  6. cost: XWDG work_max_us with everything on rises less than 100 us over everything off;
  7. (--voicetest) the 1 kHz tone reaches the 360 at the same level with the filter on and
     off (0.5 dB);
  8. put back.
the owner checks (--hands):
  J1 NOISE GATE AUTO: talk for 10 s, then stay quiet: the gate opened on AUTO while you
     talked and is shut 1 s after you stop. "Did the gate dot on the panel go out when you
     stopped talking?"
  J2 MIC VOLUME AUTO: count slowly to ten in your normal voice: the AUTO level moves toward
     the target (-30 dBFS for your voice). "Did MIC VOLUME AUTO's level on the panel change
     while you counted, and does it settle?"
  J3 the 8 kHz filter, A/B: needs someone listening on another console (party chat): the
     filter switches every 10 s, A = on, B = off. "Which sounded clearer?" (The mic test
     cannot show it: it records the 24 kHz mic before the filter.)
"""
import argparse
import math
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from accept import Checks, read_only  # noqa: E402
import micauto  # noqa: E402
import voice  # noqa: E402
import xw  # noqa: E402
from xa import OUT, Xbdm  # noqa: E402

GAINS = xw.MIC_GAINS
Q15 = {g: g * 4096 // 100 * 8 for g in GAINS}  # mic_gain_q15 per step (Q12 ramp target x 8)
STEP1, STEP2, AUTO, STEP3, STEP4 = 1, 2, 3, 4, 5  # diag_360.h XD_GATE_*
HEAP_BUDGET = 4096
COST_BUDGET_US = 100
MEASURE_S = 10
QUIET_S = 5
HANDS_S = 10


def diag_off(name):
    return 4 * xw.DIAG_FIELDS.index(name)


def wait_for(fn, secs, step=0.1):
    """fn() until it returns something true, or the last value after secs"""
    t_end, v = time.time() + secs, None
    while True:
        v = fn()
        if v or time.time() > t_end:
            return v
        time.sleep(step)


def rms_db(rms):
    return 20 * math.log10(rms / 32768) if rms else -100.0


def tone_db(path, f, rate):
    """least-squares level of a sine at f in a raw s16le mono file (dBFS of its amplitude)"""
    data = open(path, "rb").read()
    s = [int.from_bytes(data[i:i + 2], "little", signed=True) for i in range(0, len(data) - 1, 2)]
    s = s[len(s) // 4:]  # past the start-up
    ss = cc = sc = ys = yc = 0.0
    for i, y in enumerate(s):
        a = 2 * math.pi * f * i / rate
        si, co = math.sin(a), math.cos(a)
        ss, cc, sc, ys, yc = ss + si * si, cc + co * co, sc + si * co, ys + y * si, yc + y * co
    det = ss * cc - sc * sc
    amp = math.hypot((ys * cc - yc * sc) / det, (yc * ss - ys * sc) / det)
    return 20 * math.log10(amp / 32768) if amp else -100.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--voicetest", action="store_true")
    a = ap.parse_args()
    c = Checks()

    x, addr = micauto.connect()
    try:
        g = micauto.read_block(x, addr)
    except SystemExit as e:
        c.check("1 mic-auto block", False, str(e))
        c.done()
    d0 = xw.read_diag(x, quiet=True)
    c.check("1 mic-auto block version 1, XWDG version 5", d0 is not None and d0["version"] == 5,
            f"at {addr:08x}; XWDG {'read' if d0 else 'missing (xw.py of another build?)'}")
    if not d0:
        c.done()
    if a.fresh:
        c.check("2 defaults: NOISE GATE AUTO on (memory held), other switches off, gate auto, "
                "MIC VOLUME 5",
                g["gate_auto"] and not (g["mic_auto"] or g["fir"] or g["comb"] or g["auto_on"])
                and 0 < g["heap_bytes"] <= HEAP_BUDGET and g["nomem"] == 0
                and d0["applied_gate"] == AUTO and d0["applied_mic_gain"] == 100,
                f"switches {g['gate_auto']} {g['mic_auto']} {g['fir']} {g['comb']} "
                f"{g['auto_on']}, {g['heap_bytes']} bytes (nomem {g['nomem']}), gate "
                f"{d0['applied_gate']}, mic "
                f"{d0['applied_mic_gain']} %")
    if read_only():
        print("INFO read-only: steps 3-8 skipped")
        c.done()

    d_addr = xw.diag_addr()
    v_addr = voice.block_addr()
    put = lambda name, v: micauto.setctl(x, addr, name, v)
    put_d = lambda name, v: x.setu32(d_addr + diag_off(name), v)
    diag = lambda: xw.read_diag(x, quiet=True)
    ma = lambda: micauto.read_block(x, addr)
    saved = {k: g[k] for k in ("gate_auto", "mic_auto", "fir", "comb", "auto_on")}
    saved_d = {k: d0[k] for k in ("gate", "mic_gain")}

    def switches(gate=0, mic=0, fir=0, auto_on=0):
        put("gate_auto", gate)
        put("mic_auto", mic)
        put("fir", fir)
        put("auto_on", auto_on)
        put("comb", 0)
        time.sleep(0.2)

    try:
        # 3. the manual settings: today's numbers, whatever is offered
        for label, gate_on in (("switches off", 0), ("AUTOs offered", 1)):
            switches(gate=gate_on, mic=gate_on)
            bad = []
            for gate, want in ((STEP1, 33), (STEP2, 58), (STEP3, 104), (STEP4, 184),
                               (AUTO, 58 if not gate_on else None)):
                if want is None:
                    continue
                put_d("gate", gate)
                d = wait_for(lambda: (lambda d: d if d["applied_gate"] == gate and
                                      d["gate_threshold"] == want else None)(diag()), 1)
                if not d:
                    bad.append(f"gate {gate}: threshold {diag()['gate_threshold']}, want {want}")
            put_d("gate", saved_d["gate"])
            for pct in GAINS:
                put_d("mic_gain", pct)
                d = wait_for(lambda: (lambda d: d if d["mic_gain_q15"] == Q15[pct] else None)(
                    diag()), 1)
                if not d:
                    bad.append(f"mic {pct} %: q15 {diag()['mic_gain_q15']}, want {Q15[pct]}")
            put_d("mic_gain", saved_d["mic_gain"])
            c.check(f"3 manual settings as today ({label})", not bad,
                    "; ".join(bad[:4]) + (" (mic_gain_q15 moves only while the mic goes to a "
                                          "title: stage 4, not muted)" if bad else ""))

        # 4. the floor tracker on the real headset, in quiet
        switches(gate=1, mic=1)
        print(f"DO   stay quiet for {QUIET_S} s", flush=True)
        time.sleep(QUIET_S)
        g = ma()
        floor, thr = rms_db(g["floor_rms"]), rms_db(g["thr_rms"])
        want = min(max(floor + micauto.GATE_MARGIN_DB, micauto.GATE_MIN_DB), micauto.GATE_MAX_DB)
        c.check("4a memory and pad", 0 < g["heap_bytes"] <= HEAP_BUDGET and g["pad"] < micauto.PADS,
                f"{g['heap_bytes']} bytes, pad {g['pad']}")
        c.check("4b threshold = floor + 9 dB within -60 .. -35 dBFS",
                g["floor_rms"] > 0 and abs(thr - want) < 1.5 and 33 <= g["thr_rms"] <= 583,
                f"floor {floor:.1f} dBFS, threshold {thr:.1f} dBFS (rms {g['thr_rms']})")
        put_d("gate", AUTO)
        d = wait_for(lambda: (lambda d, g: d["gate_threshold"] == g["thr_rms"])(diag(), ma()), 1)
        c.check("4c gate auto uses it", bool(d),
                f"XWDG gate_threshold {diag()['gate_threshold']}, thr_rms {ma()['thr_rms']}")
        put_d("gate", saved_d["gate"])

        # 5. the 8 kHz filter in the 360's mic path
        pad = g["pad"] if g["pad"] < micauto.PADS else 0

        def pace(secs=2.0):
            v0, m0, t0 = voice.read_block(x, v_addr)["pads"][pad], ma(), time.time()
            time.sleep(secs)
            v1, m1, t1 = voice.read_block(x, v_addr)["pads"][pad], ma(), time.time()
            return ((v1["cap_bytes"] - v0["cap_bytes"]) / (t1 - t0),
                    v1["mic_dropped"] - v0["mic_dropped"],
                    (m1["fir_samples"] - m0["fir_samples"]) / (t1 - t0),
                    m1["fir_bypass"] - m0["fir_bypass"])
        switches(fir=0)
        off = pace()
        switches(fir=1)
        on = pace()
        c.check("5 filter: 8000 samples a second, the 360 gets the same pace",
                abs(on[2] - 8000) < 400 and on[3] == 0 and off[0] > 0
                and abs(on[0] - off[0]) <= 0.02 * off[0] and on[1] <= off[1],
                f"{on[2]:.0f} samples/s, {on[3]} bypassed; capture {off[0]:.0f} -> "
                f"{on[0]:.0f} B/s, "
                f"mic dropped +{off[1]} / +{on[1]}" + ("" if off[0] else
                                                      " (no capture: is a title listening?)"))

        # 6. cost on the driver thread
        switches()
        put_d("work_max_us", 0)
        time.sleep(MEASURE_S)
        base = diag()["work_max_us"]
        switches(gate=1, mic=1, fir=1, auto_on=1)
        put_d("gate", AUTO)
        put_d("work_max_us", 0)
        time.sleep(MEASURE_S)
        full = diag()["work_max_us"]
        put_d("gate", saved_d["gate"])
        c.check(f"6 driver work_max_us rise under {COST_BUDGET_US} us",
                full - base < COST_BUDGET_US,
                f"everything off {base} us, everything on {full} us")

        # 7. the tone through the voicetest title, filter off and on
        if a.voicetest:
            levels = []
            put_d("gate", 0)
            for fir in (0, 1):
                switches(fir=fir)
                subprocess.run([sys.executable, os.path.join(xw.HERE, "voice.py"), "test",
                                "--freq", "1000", "--seconds", "6"]
                               + (["--no-upload"] if fir else []), check=False)
                levels.append(tone_db(os.path.join(OUT, "voicetest_8000.raw"), 1000, 8000))
                x, addr = micauto.connect()  # the title switch: reconnect
            put_d = lambda name, v: Xbdm().setu32(d_addr + diag_off(name), v)
            put_d("gate", saved_d["gate"])
            c.check("7 voicetest: 1 kHz tone level, filter on vs off (0.5 dB)",
                    abs(levels[1] - levels[0]) < 0.5,
                    f"off {levels[0]:.2f} dBFS, on {levels[1]:.2f} dBFS")
    finally:
        x, addr = micauto.connect()
        for k, v in saved.items():
            micauto.setctl(x, addr, k, v)
        Xbdm().setu32(d_addr + diag_off("gate"), saved_d["gate"])
        Xbdm().setu32(d_addr + diag_off("mic_gain"), saved_d["mic_gain"])
    time.sleep(0.5)
    g, d = micauto.read_block(Xbdm(), addr), xw.read_diag(Xbdm(), quiet=True)
    c.check("8 settings put back", all(g[k] == v for k, v in saved.items())
            and d["gate"] == saved_d["gate"] and d["mic_gain"] == saved_d["mic_gain"])

    j1 = ("J1 NOISE GATE AUTO: talk normally for 10 s, then stay quiet. Did the gate dot on the "
          "panel go out when you stopped talking?")
    j2 = ("J2 MIC VOLUME AUTO: count slowly to ten in your normal voice. Did MIC VOLUME AUTO's "
          "level on the panel change while you counted, and does it settle?")
    j3 = ("J3 the 8 kHz filter: someone on another console listens in party chat while it "
          "switches every 10 s (A = on, B = off). Which sounded clearer?")
    if not a.hands:
        for j in (j1, j2, j3):
            print("TODO " + j)
        c.done()
    x = Xbdm()
    try:
        # J1
        switches(gate=1, mic=1)
        put_d("gate", AUTO)
        opens0 = ma()["auto_opens"]
        print(f"DO   talk normally into the mic for {HANDS_S} s, starting now", flush=True)
        time.sleep(HANDS_S)
        opens1 = ma()["auto_opens"]
        print("DO   now stay quiet", flush=True)
        time.sleep(1.5)
        d = diag()
        c.check("J1a gate on AUTO opened while talking, shut 1.5 s after",
                opens1 > opens0 and d["gate_open"] == 0,
                f"opened {opens1 - opens0} times; gate_open {d['gate_open']}, rms "
                f"{d['gate_rms']} / threshold {d['gate_threshold']}")
        c.check("J1 the owner: the gate dot went out", input(j1 + " [y/n] ").strip().lower()
                .startswith("y"))
        # J2: AUTO from MIC VOLUME 5
        put_d("mic_gain", 100)
        time.sleep(0.2)
        put("auto_on", 1)
        g0 = ma()
        print("DO   count slowly to ten in your normal voice, now", flush=True)
        trail = []
        for _ in range(12):
            time.sleep(1)
            trail.append(ma()["agc_level"])
        g1 = ma()
        err = lambda g: abs(micauto.TARGET_DB - rms_db(g["speech_rms"]) -
                            micauto.q12_db(g["agc_q12"]))
        c.check("J2a AUTO learnt the voice and moved toward the target",
                g1["speech_windows"] > g0["speech_windows"] and g1["speech_rms"] > 0
                and (err(g1) < 2.0 or err(g1) < err(g0)),
                f"speech {rms_db(g1['speech_rms']):.1f} dBFS over {g1['speech_windows']} windows; "
                f"gain {micauto.q12_db(g0['agc_q12']):+.1f} -> {micauto.q12_db(g1['agc_q12']):+.1f}"
                f" dB, levels {trail}")
        c.check("J2 the owner: the AUTO level moved and settled", input(j2 + " [y/n] ").strip()
                .lower().startswith("y"))
        put("auto_on", saved["auto_on"])
        # J3: A/B every 10 s for a minute
        print("DO   ask the listener to say which part sounds clearer", flush=True)
        for i in range(6):
            fir = int(i % 2 == 0)
            put("fir", fir)
            print(f"     {'A (filter on)' if fir else 'B (filter off)'}", flush=True)
            time.sleep(10)
        ans = input(j3 + " [A/B/same] ").strip().lower()
        print(f"INFO J3 answer: {ans}")
        c.check("J3 the owner: an answer was given", ans in ("a", "b", "same"))
    finally:
        x, addr = micauto.connect()
        for k, v in saved.items():
            micauto.setctl(x, addr, k, v)
        Xbdm().setu32(d_addr + diag_off("gate"), saved_d["gate"])
        Xbdm().setu32(d_addr + diag_off("mic_gain"), saved_d["mic_gain"])
    c.done()


if __name__ == "__main__":
    main()
