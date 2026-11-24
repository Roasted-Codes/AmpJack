#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for WP1.4, "Audio settings, defaults, knob removal" (docs/PLAN.md).

Run it after a reboot with the WP1.4 xw.xex installed and NO xw.py / voice.py command sent
since (the first checks are the compiled defaults), once the pad is on (press its Xbox
button) and its jack headset is streaming. For the stream check, be in a Halo 3 match (or at
least have game audio playing) for the --seconds window. Run it from anywhere; it uses
xbox360/out/ for the block addresses:

  XBOX=10.0.0.13 python3 xbox360/accept/wp1_4.py [--seconds N] [--hands] [--no-probe]

  --seconds N   how long the stream is watched (default 300: the plan's 5 minutes)
  --hands       also run the gate check that needs someone at the mic: 10 s talking, then
                10 s silence. Without it, a OWNER CHECK line is printed instead.
  --no-probe    skip the driver-loop timing probes at the end

Checks (one PASS or FAIL line each; the exit status is 1 if any failed):
  - the XWDG block is version 3 and the XWAU block version 3 (this build; xw.py reads them);
  - the compiled defaults, untouched: mic volume 100 % (level 5), noise gate auto, test tone
    off, sidetone 0 (XWDG version 3, branch mictest: was mic level 50 %);
  - the removed knobs are gone from out/xw.exe (no clock-fix / DPC-refill / game-dump code);
  - xw.py diag micgain 70 applies (applied_mic_gain 70), a raw 120 is rounded to the nearest
    accepted volume and written back as 140, the volume is put back to 100 afterwards;
  - over N seconds: out_starved +0, out_frames_missed +0 (XWDG), mic_pps 1000 +/- 2 in every
    sample (XWAU), game audio underruns +0 (XVGA);
  - with --hands: gate_opens rises while the owner talks, gate_open is 0 after 10 s of silence,
    and the mic meter (mic_vu) moves while he talks;
  - xw.py micdump and xw.py dump each write a WAV with audio in it (out/accept/);
  - the driver loop's timing (INFO, then a verdict line; judged only for consistency):
    pass_us / work_us / pass histogram, timebase vs system clock, loop passes per second by
    the console clock and by the XW36 heartbeat over the Mac's wall clock, and probes of
    xa_sleep_ms(0), (1), (2) and of the system clock's step (16 passes each).

Standalone: it imports only xa.py, xw.py and voice.py (next to this folder), so accept/run.py
(WP0.2) can run it as a script.
"""
import argparse
import os
import sys
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import voice  # noqa: E402
import xw  # noqa: E402
from xa import OUT, Xbdm  # noqa: E402

MIC_PPS, MIC_PPS_TOL = 1000, 2
SAMPLE_S = 2
HANDS_S = 10
PROBE_WAIT_S = 10
DEFAULT_GAIN, DEFAULT_GATE = 100, 3  # gate auto (diag_360.h XD_GATE_DEFAULT)
# log strings and symbols of the removed knobs: none may be left in the image
GONE = [b"earpiece feed", b"refill from the completion DPC", b"OUT blocks in flight: %u",
        b"frames per mic skip"]

failed = []


def result(ok, name, detail=""):
    print(f"{'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""), flush=True)
    if not ok:
        failed.append(name)
    return ok


def info(text):
    print(f"INFO {text}", flush=True)


def diag(x):
    return xw.read_diag(x)


def audio(x, st):
    s = xw.read_status(x, st)
    return s and s.get("audio")


def set_diag_word(x, field, value):
    x.setu32(xw.diag_addr() + 4 * xw.DIAG_FIELDS.index(field), value)


def wait_for(x, pred, secs):
    until = time.time() + secs
    while time.time() < until:
        d = diag(x)
        if d and pred(d):
            return d
        time.sleep(0.2)
    return diag(x)


def check_versions(x, st):
    d = diag(x)
    a = audio(x, st)
    ok = result(d is not None, f"XWDG version {xw.DIAG_VERSION}",
                "read" if d else "missing or another version")
    ok &= result(a is not None and a["version"] == xw.AUDIO_VERSION, "XWAU version 3",
                 f"version {a['version']}" if a else "missing or another version")
    return ok, d, a


def check_defaults(x, d):
    ga = voice.read_ga(x, voice.ga_addr()) if voice.ga_addr() is not None else None
    result(d["mic_gain"] == DEFAULT_GAIN and d["applied_mic_gain"] == DEFAULT_GAIN,
           f"default mic volume {DEFAULT_GAIN} %",
           f"mic_gain {d['mic_gain']}, applied {d['applied_mic_gain']} (if not {DEFAULT_GAIN}: a command "
           "was sent since boot?)")
    result(d["gate"] == DEFAULT_GATE and d["applied_gate"] == DEFAULT_GATE,
           "default noise gate auto", f"gate {d['gate']}, applied {d['applied_gate']}, "
           f"threshold {d['gate_threshold']}")
    result(d["applied_tone"] == 0, "default test tone off", f"tone {d['applied_tone']}")
    result(ga is not None and ga["applied_sidetone"] == 5, "default sidetone 5",
           f"sidetone {ga['applied_sidetone'] if ga else '-'}")


def check_removed():
    path = os.path.join(OUT, "xw.exe")
    try:
        img = open(path, "rb").read()
    except OSError as e:
        return result(False, "removed knobs gone from out/xw.exe", str(e))
    left = [g.decode() for g in GONE if g in img]
    return result(not left, "removed knobs gone from out/xw.exe",
                  "none of their strings left" if not left else f"still there: {left}")


def check_micgain(x):
    set_diag_word(x, "mic_gain", 70)
    d = wait_for(x, lambda d: d["applied_mic_gain"] == 70, 3)
    result(d is not None and d["applied_mic_gain"] == 70, "xw.py diag micgain 70 applies",
           f"applied {d['applied_mic_gain'] if d else '-'}, gain now "
           f"{d['mic_gain_q15'] * 100 // 32768 if d else '-'}%")
    set_diag_word(x, "mic_gain", 120)
    d = wait_for(x, lambda d: d["mic_gain"] != 120, 3)
    result(d is not None and d["mic_gain"] == 140 and d["applied_mic_gain"] == 140,
           "a raw 120 is rounded to 140 and written back",
           f"mic_gain {d['mic_gain'] if d else '-'}, applied {d['applied_mic_gain'] if d else '-'}")
    set_diag_word(x, "mic_gain", DEFAULT_GAIN)
    d = wait_for(x, lambda d: d["applied_mic_gain"] == DEFAULT_GAIN, 3)
    result(d is not None and d["applied_mic_gain"] == DEFAULT_GAIN,
           f"mic volume back to {DEFAULT_GAIN} %", f"applied {d['applied_mic_gain'] if d else '-'}")


def check_stream(x, st, seconds):
    d0, a0 = diag(x), audio(x, st)
    ga_at = voice.ga_addr()
    g0 = voice.read_ga(x, ga_at) if ga_at is not None else None
    streaming = bool(a0) and a0["mic_pps"] > 0
    result(streaming, "headset streaming at the start",
           f"mic_pps {a0['mic_pps'] if a0 else '-'}, active stage "
           f"{a0['active_stage'] if a0 else '-'}" +
           ("" if streaming else " (pad on? headset plugged in? xw.py audio 4?)"))
    if g0:
        info(f"game audio: state {g0['state']} ({voice.GA_STATES.get(g0['state'], '?')}), "
             f"enable {g0['applied_enable']}, consumer pad {g0['consumer_pad']}")
    pps, bad = [], []
    info(f"watching the stream for {seconds:.0f} s (be in a Halo 3 match)")
    until = time.time() + seconds
    while time.time() < until:
        time.sleep(SAMPLE_S)
        a = audio(x, st)
        v = a["mic_pps"] if a else None
        pps.append(v)
        if v is None or abs(v - MIC_PPS) > MIC_PPS_TOL:
            bad.append(v)
    d1 = diag(x)
    g1 = voice.read_ga(x, ga_at) if ga_at is not None else None
    for f in ("out_starved", "out_frames_missed"):
        result(d1 is not None and d0 is not None and d1[f] == d0[f], f"{f} +0",
               f"{d0[f] if d0 else '-'} -> {d1[f] if d1 else '-'}")
    vals = [p for p in pps if p is not None]
    result(bool(pps) and not bad, f"mic_pps {MIC_PPS} +/- {MIC_PPS_TOL}",
           f"{len(pps)} samples, min {min(vals, default='-')}, max {max(vals, default='-')}"
           + (f", out of range: {bad[:8]}" if bad else ""))
    result(g0 is not None and g1 is not None and g1["underruns"] == g0["underruns"],
           "game audio underruns +0",
           f"{g0['underruns'] if g0 else '-'} -> {g1['underruns'] if g1 else '-'}")
    a1 = audio(x, st)
    if a0 and a1:
        info(f"mic frames skipped by the headset: +{a1['mic_skips'] - a0['mic_skips']}")


def check_gate_hands(x):
    d0 = diag(x)
    print(f"DO   talk normally into the headset mic for {HANDS_S} s, starting now", flush=True)
    vu_max, until = 0, time.time() + HANDS_S
    while time.time() < until:
        d = diag(x)
        if d:
            vu_max = max(vu_max, d["mic_vu"])
        time.sleep(0.2)
    d1 = diag(x)
    result(d1["gate_opens"] > d0["gate_opens"], "gate_opens rises while talking",
           f"{d0['gate_opens']} -> {d1['gate_opens']}")
    result(vu_max > 0, "mic meter moves while talking", f"mic_vu max {vu_max}/10")
    print(f"DO   now stay quiet for {HANDS_S} s", flush=True)
    time.sleep(HANDS_S)
    d2 = diag(x)
    result(d2["gate_open"] == 0, "gate_open 0 in silence",
           f"gate_open {d2['gate_open']}, rms {d2['gate_rms']} / threshold {d2['gate_threshold']}")


def wav_has_audio(path):
    with wave.open(path, "rb") as w:
        n, data, width = w.getnframes(), w.readframes(w.getnframes()), w.getsampwidth()
    peak = 0
    for i in range(0, len(data) - 1, width):
        v = int.from_bytes(data[i:i + 2], "little", signed=True)
        peak = max(peak, abs(v))
    return n, peak


def check_dumps():
    d = os.path.join(OUT, "accept")
    os.makedirs(d, exist_ok=True)
    for name, fn, file in (("micdump", xw.micdump, "wp1_4_mic.wav"),
                           ("dump", xw.dump, "wp1_4_ear.wav")):
        path = os.path.join(d, file)
        try:
            fn(path)
            n, peak = wav_has_audio(path)
            result(n > 0, f"xw.py {name} writes a WAV", f"{path}: {n} frames, peak {peak}")
        except (SystemExit, OSError, wave.Error) as e:
            result(False, f"xw.py {name} writes a WAV", str(e))


def run_probe(x, value):
    runs = diag(x)["probe_runs"]
    set_diag_word(x, "probe", value)
    d = wait_for(x, lambda d: d["probe_runs"] > runs and d["probe"] == 0, PROBE_WAIT_S)
    if not d or d["probe_runs"] <= runs:
        return None
    return d


def check_timing(x, st, probe):
    """measure, don't guess: where the ~16.6 ms loop pass goes"""
    d0 = diag(x)
    s0 = xw.read_status(x, st)
    t0 = time.time()
    time.sleep(5)
    d1 = diag(x)
    s1 = xw.read_status(x, st)
    t1 = time.time()
    hb_rate = (s1["heartbeat"] - s0["heartbeat"]) / (t1 - t0)
    hist = ", ".join(f"{n} ms: {c1 - c0}" for n, c0, c1 in
                     zip(xw.PASS_HIST, d0["pass_hist"], d1["pass_hist"]))
    info(f"loop pass {d1['pass_us']} us (max since boot {d1['pass_max_us']}) = work in "
         f"audio_poll + xv_poll {d1['work_us']} us (max {d1['work_max_us']}) + the rest of the "
         f"pass (the 2 ms sleep, panel/hud/xt polls, service_port) {d1['gap_us']} us (max "
         f"{d1['gap_max_us']})")
    info(f"passes over 5 s by length: {hist}")
    info(f"passes/s: {d1['passes_per_s']} by the console clock, {hb_rate:.1f} by the XW36 "
         f"heartbeat over the Mac's clock; timebase {d1['tb_per_ms']} ticks/ms (50000 = the "
         f"50 MHz the _us fields assume)")
    result(45000 <= d1["tb_per_ms"] <= 55000, "timebase is ~50 MHz (the _us fields are right)",
           f"{d1['tb_per_ms']} ticks/ms")
    if hb_rate > 0 and d1["passes_per_s"]:
        result(abs(hb_rate - d1["passes_per_s"]) <= max(3, 0.1 * hb_rate),
               "loop rate agrees: console clock vs Mac clock",
               f"{d1['passes_per_s']} vs {hb_rate:.1f} passes/s")
    if not probe:
        return
    out = {}
    for label, value in (("sleep0", xw.PROBE_SLEEP + 0), ("sleep1", xw.PROBE_SLEEP + 1),
                         ("sleep2", xw.PROBE_SLEEP + 2), ("tick", xw.PROBE_TICK)):
        d = run_probe(x, value)
        if not result(d is not None, f"probe {label} ran", "" if d else "no result in 10 s"):
            continue
        out[label] = d
        extra = f", step {d['probe_step_100ns'] / 10:.0f} us of system time" \
            if value == xw.PROBE_TICK else ""
        info(f"probe {label}: avg {d['probe_avg_us']} us, min {d['probe_min_us']}, max "
             f"{d['probe_max_us']} ({d['probe_n']} passes){extra}")
    d2 = diag(x)
    info(f"out_starved during the probes: +{d2['out_starved'] - d1['out_starved']} (a probe "
         "pass is longer than a normal one)")
    if {"sleep0", "sleep1", "sleep2"} <= out.keys():
        s0_, s1_, s2_ = (out[k]["probe_avg_us"] for k in ("sleep0", "sleep1", "sleep2"))
        tick = out["tick"]["probe_avg_us"] if "tick" in out else None
        work, gap = d1["work_us"], d1["gap_us"]
        if s1_ >= 5000 and abs(s2_ - s1_) <= max(1500, s1_ // 5):
            verdict = (f"SLEEP GRANULARITY: xa_sleep_ms(1) takes {s1_} us and (2) {s2_} us, "
                       f"about the same: a sleep rounds up to the scheduler's timer tick"
                       + (f" (system clock steps every {tick} us)" if tick else "") +
                       f"; the loop's own work is {work} us, the rest of the pass {gap} us. "
                       "The pass time is the sleep.")
        elif s2_ < 4000 and gap > s2_ + 4000:
            verdict = (f"NOT THE SLEEP: xa_sleep_ms(2) takes {s2_} us, but the rest of the pass "
                       f"is {gap} us (audio/voice work {work} us): about {gap - s2_} us per pass "
                       "goes to panel_poll / hud_poll / xt_poll / service_port, or the thread "
                       "waits to be run after its sleep")
        elif s2_ < 4000 and d1["pass_us"] > 10000:
            verdict = (f"NOT THE SLEEP: xa_sleep_ms(2) takes {s2_} us, yet a pass is "
                       f"{d1['pass_us']} us with {work} us of audio/voice work")
        else:
            verdict = (f"MIXED: sleep(0) {s0_} us, sleep(1) {s1_} us, sleep(2) {s2_} us, pass "
                       f"{d1['pass_us']} us, work {work} us")
        print(f"VERDICT {verdict}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, default=300)
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--no-probe", action="store_true")
    a = ap.parse_args()
    x = Xbdm()
    st = xw.status_addr()

    ok, d, _ = check_versions(x, st)
    if not ok:
        print("FAILED (not the WP1.4 build, or out/ isn't the installed build)", flush=True)
        return 1
    check_defaults(x, d)
    check_removed()
    check_micgain(x)
    if a.hands:
        check_gate_hands(x)
    else:
        print("OWNER CHECK talk for 10 s, then stay quiet for 10 s, while the coordinator "
              "watches `xw.py diag`: gate opened N times rises, the meter moves, then the gate "
              "reads closed", flush=True)
    check_stream(x, st, a.seconds)
    check_dumps()
    check_timing(x, st, not a.no_probe)
    print("OWNER CHECK in a Halo 3 match with no setup after the reboot: teammates no longer "
          "hear a hot mic, and still hear you when you talk", flush=True)

    print(f"{'FAILED' if failed else 'ALL PASSED'}"
          + (f" ({len(failed)}: {', '.join(failed)})" if failed else ""), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
