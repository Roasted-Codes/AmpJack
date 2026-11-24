#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for WP0.1, "Integration base" (docs/PLAN.md).

Run it after one reboot with the WP0.1 xw.xex installed, once the pad is on (press its Xbox
button) and its jack headset is streaming. Run it from anywhere; it uses xbox360/out/ for the
block addresses:

  XBOX=10.0.0.13 python3 xbox360/accept/wp0_1.py [--seconds N] [--hands]

  --seconds N   how long the driver-thread budget is watched (default 60)
  --hands       also run the checks that need someone at the pad: a Share tap must toggle
                the mic mute (and a second tap toggle it back), and Share + D-pad Right / Left
                must move the balance one step and back. Each step prints what to do and waits
                up to 30 s. Without --hands they are printed as OWNER CHECK lines.

Checks (one PASS or FAIL line each; the exit status is 1 if any failed):
  - xw.py status, voice.py status and hud.py status each run and read their blocks;
  - the running build is this one (XW36 has the WP0.1 loop timing words, and they move);
  - hud: enable 0, VdSwap not hooked;
  - over N seconds: out_starved +0 (XWDG), mic_pps 1000 +/- 2 in every sample (XWAU);
    loop_max_us (the driver loop's longest pass, reset at the start) is printed, not judged.

Standalone: it imports only xa.py, xw.py, voice.py and hud.py (next to this folder), so
accept/run.py (WP0.2) can run it as a script.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import hud  # noqa: E402
import voice  # noqa: E402
import xw  # noqa: E402
from xa import Xbdm, xw_block  # noqa: E402

# struct xw_status (xw_plugin.c): loop_us, loop_max_us after the audio block (_Static_assert)
OFF_LOOP = 63 * 4 + 4 * 52 + 85 * 4
MIC_PPS, MIC_PPS_TOL = 1000, 2
SAMPLE_S = 2
HANDS_WAIT_S = 30
HK_INSTALLED = 1

failed = []


def result(ok, name, detail=""):
    print(f"{'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""), flush=True)
    if not ok:
        failed.append(name)
    return ok


def info(text):
    print(f"INFO {text}", flush=True)


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def tool_status(script):
    """runs `script status` as the coordinator would; ok = exit 0 with some output"""
    try:
        r = subprocess.run([sys.executable, os.path.join(TOOLS, script), "status"], cwd=TOOLS,
                           capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return result(False, f"{script} status", "timed out")
    out = (r.stdout + r.stderr).strip()
    last = out.splitlines()[-1] if out else "no output"
    return result(r.returncode == 0 and bool(r.stdout.strip()), f"{script} status",
                  f"exit {r.returncode}" + ("" if r.returncode == 0 else f"; {last}"))


def read_loop(x, addr):
    w = x.u32s(addr + OFF_LOOP, 2)
    return (w[0], w[1]) if w else (None, None)


def read_audio(x, addr):
    s = xw.read_status(x, addr)
    return s and s.get("audio")


def wait_change(prompt, read, old, secs=HANDS_WAIT_S):
    """prints what to do, then polls read() until it differs from old; the new value, or old
    if nothing changed within secs"""
    print(f"DO   {prompt} (waiting up to {secs} s)", flush=True)
    until = time.time() + secs
    while time.time() < until:
        v = read()
        if v != old:
            time.sleep(0.5)  # let a second change (a double press) show up too
            return read()
        time.sleep(0.25)
    return old


def hands_checks(x):
    def mutes():
        h = voice.read_hs(x)
        return [p["muted"] for p in h["pads"]] if h else None

    ga = voice.ga_addr()

    def balance():
        return s32(voice.read_ga(x, ga)["balance"])

    m0 = mutes()
    if m0 is None:
        result(False, "Share tap toggles mute", "no XVHS block")
    else:
        m1 = wait_change("tap Share once (release within 1 s)", mutes, m0)
        if result(m1 != m0 and sum(a != b for a, b in zip(m0, m1)) == 1,
                  "Share tap toggles mute", f"muted {m0} -> {m1}"):
            m2 = wait_change("tap Share once more", mutes, m1)
            result(m2 == m0, "second Share tap toggles it back", f"muted {m1} -> {m2}")

    # one step away from b0 and back (Left first if the balance is already at +5)
    b0 = balance()
    first, back, step = ("Right", "Left", 1) if b0 < 5 else ("Left", "Right", -1)
    b1 = wait_change(f"hold Share, press D-pad {first} once, release both", balance, b0)
    if result(b1 == b0 + step, f"Share + {first}: balance {step:+d}", f"balance {b0} -> {b1}"):
        b2 = wait_change(f"hold Share, press D-pad {back} once, release both", balance, b1)
        result(b2 == b0, f"Share + {back}: balance back", f"balance {b1} -> {b2}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--hands", action="store_true")
    a = ap.parse_args()
    x = Xbdm()

    # 1. the three tools read their blocks
    for script in ("xw.py", "voice.py", "hud.py"):
        tool_status(script)

    # 2. this build: the loop timing words exist and move
    st = xw.status_addr()
    l0 = read_loop(x, st)
    time.sleep(0.5)
    l1 = read_loop(x, st)
    # (a pass is at least the 2 ms sleep; an older build has other data there)
    result(l1[0] is not None and 1500 <= l1[0] <= 1000000 and l1[1] >= l1[0],
           "WP0.1 build running (XW36 loop_us)",
           f"loop_us {l1[0]}, loop_max_us {l1[1]} (before: {l0[0]}, {l0[1]})")

    # 3. the HUD is off
    try:
        g = hud.read_block(x, xw_block(hud.MAGIC))
        result(g["enable"] == 0, "hud enable 0", f"enable {g['enable']}")
        result(g["hooked"] != HK_INSTALLED, "hud: VdSwap not hooked",
               hud.HOOKED.get(g["hooked"], str(g["hooked"])))
    except SystemExit as e:
        result(False, "hud block", str(e))

    # 4. the driver thread's budget
    d0 = xw.read_diag(x)
    au = read_audio(x, st)
    if not result(d0 is not None, "XWDG block", "read" if d0 else "missing"):
        return 1
    streaming = bool(au) and au["mic_pps"] > 0
    result(streaming, "headset streaming at the start",
           f"mic_pps {au['mic_pps'] if au else '-'}, active stage "
           f"{au['active_stage'] if au else '-'}" +
           ("" if streaming else " (pad on? headset plugged in? xw.py audio 4?)"))
    x.setu32(st + OFF_LOOP + 4, 0)  # loop_max_us: measure from now
    pps, bad = [], []
    info(f"watching for {a.seconds:.0f} s")
    until = time.time() + a.seconds
    while time.time() < until:
        time.sleep(SAMPLE_S)
        au = read_audio(x, st)
        v = au["mic_pps"] if au else None
        pps.append(v)
        if v is None or abs(v - MIC_PPS) > MIC_PPS_TOL:
            bad.append(v)
    d1 = xw.read_diag(x)
    loop_us, loop_max = read_loop(x, st)
    result(d1 is not None and d1["out_starved"] == d0["out_starved"], "out_starved +0",
           f"{d0['out_starved']} -> {d1['out_starved'] if d1 else '-'}")
    result(bool(pps) and not bad, f"mic_pps {MIC_PPS} +/- {MIC_PPS_TOL}",
           f"{len(pps)} samples, min {min((p for p in pps if p is not None), default='-')}, "
           f"max {max((p for p in pps if p is not None), default='-')}"
           + (f", out of range: {bad[:8]}" if bad else ""))
    if d1:
        info(f"out_frames_missed +{d1['out_frames_missed'] - d0['out_frames_missed']}")
    info(f"driver loop: last pass {loop_us} us, longest over the window {loop_max} us")

    # 5. what needs a person
    if a.hands:
        hands_checks(x)
    else:
        for c in ("Share tap mutes the mic and shows MIC MUTED / MIC ON as before",
                  "Share + D-pad changes balance / volume live, and the D-pad doesn't reach "
                  "the title while Share is held",
                  "Halo 3: teammates hear you, you hear them, and game audio is in the headset, "
                  "as on 5eabd0a"):
            print(f"OWNER CHECK {c}", flush=True)

    print(f"{'FAILED' if failed else 'ALL PASSED'}"
          + (f" ({len(failed)}: {', '.join(failed)})" if failed else ""), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
