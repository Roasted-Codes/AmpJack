#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for WP4.1, the team voice receive path (docs/PLAN.md 4.3, docs/TEAM.md).

The Mac plays a teammate: this script sends a 440 Hz tone (-20 dBFS, 10 ms packets) on channel
1 from UDP 3660 here, while the console runs stage 3 on channel 1 and mixes it into the
headset's earpiece. Needs the pad on and its jack headset streaming (xw.py audio 4, as for
WP0.1); game audio may play.

  XBOX=10.0.0.13 python3 xbox360/accept/p4_rx.py [--seconds 300] [--hands] [--keep]

  --seconds N   how long the tone runs (default 300, the plan's 5 minutes)
  --hands       the owner is at the headset: the level steps are announced and he confirms
  --keep        leave stage 3 and channel 1 on at the end

Checks (PASS / FAIL, exit 1 if any failed):
  - the headset is streaming (XWAU mic_pps > 0) and the XWTV block is there;
  - over the run, for the Mac's talker: lost 0, underruns +0 after the first second, the
    fill (average) within 20..50 ms in every sample after 5 s, the trim within +/- 500 ppm in
    every sample after 30 s;
  - the team mix dump (1 s, after the gain) has 440 Hz at the expected level (-20 dBFS x
    gain, +/- 2 dB);
  - xw.py dump's earpiece capture has 440 Hz (at least half the expected level: game audio
    and chat are in it too);
  - out_starved +0 (XWDG) over the run.
OWNER CHECK: a clean steady tone under the game audio; team.py set level 3 makes it quieter.
"""
import argparse
import math
import os
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import team  # noqa: E402
import xw  # noqa: E402
from accept import Checks, read_only, xw as run_xw  # noqa: E402

SAMPLE_S = 5


def info(text):
    print(f"INFO {text}", flush=True)


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def ear_440(prefix):
    """440 Hz amplitude in the left channel of xw.py dump's earpiece capture (s16le stereo),
    at the earpiece rate the team block reported"""
    try:
        d = open(prefix + ".ear", "rb").read()
    except OSError:
        return None
    n = len(d) // 4
    left = struct.unpack(f"<{2 * n}h", d[:4 * n])[0::2]
    return left


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, default=300)
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    c = Checks()
    try:
        x, addr = team.connect()
        g = team.read_block(x, addr)
    except SystemExit as e:
        c.check("XWTV block", False, str(e))
        c.done()
    c.check("XWTV block", True, f"at {addr:08x}")
    st = xw.status_addr()
    au = xw.read_status(x, st).get("audio")
    streaming = bool(au) and au["mic_pps"] > 0
    c.check("headset streaming", streaming,
            f"mic_pps {au['mic_pps'] if au else '-'}" +
            ("" if streaming else " (pad on? headset in? xw.py audio 4?)"))
    if read_only() or not streaming:
        team.print_status(g)
        c.done()
    stage0, channel0, level0 = g["stage"], g["channel"], g["level"]
    d0 = xw.read_diag(x)
    team.setctl(x, addr, "channel", 1)
    team.setctl(x, addr, "level", 10)
    team.setctl(x, addr, "stage", 3)
    for name in team.COUNTERS:
        team.setctl(x, addr, name, 0)

    bg = team.Background(channel=1, tone=True, name="TONE").start()
    t0 = time.time()
    samples = []
    under_1s = None
    talker = None
    dump = None
    ear = None
    try:
        while time.time() - t0 < a.seconds:
            time.sleep(1 if under_1s is None else SAMPLE_S)
            g = team.read_block(x, addr)
            el = time.time() - t0
            peer = next((p for p in g["peers"] if p["id"] == bg.peer.sender), None)
            if peer and peer["talker"]:
                talker = peer["talker"] - 1
            if talker is None:
                continue
            t = g["talkers"][talker]
            if under_1s is None and el >= 1.5:
                under_1s = t["underruns"]
            samples.append((el, t["fill_ms"], s32(t["trim_ppm"]), t["underruns"],
                            peer["lost"] if peer else None, t["state"]))
            if dump is None and el >= min(30, a.seconds / 2):
                try:
                    s, rate, amp, rms = team.console_dump(None)
                    dump = (amp, rms, rate, team.read_block(x, addr)["gain"])
                except SystemExit as e:
                    dump = (None, None, None, str(e))
                prefix = os.path.join(tempfile.mkdtemp(prefix="p4rx"), "dump")
                run_xw("dump", prefix)
                left = ear_440(prefix)
                ear = (team.goertzel(left, g["rate_out"], 440) if left else None, g["rate_out"])
            if a.hands and el >= 60 and not getattr(main, "asked", False):
                main.asked = True
                print("DO   the owner: listen for a steady tone under the game; it gets quieter "
                      "now (level 3) and back in 10 s", flush=True)
                team.setctl(x, addr, "level", 3)
                time.sleep(10)
                team.setctl(x, addr, "level", 10)
    finally:
        bg.stop()
    g = team.read_block(x, addr)
    d1 = xw.read_diag(x)
    c.check("the Mac's tone got a talker slot", talker is not None,
            f"slot {talker}" if talker is not None else "no talker (channel? stage?)")
    if talker is None:
        team.print_status(g)
        c.done()
    t = g["talkers"][talker]
    peer = next((p for p in g["peers"] if p["id"] == bg.peer.sender), None)
    info(f"sent {bg.sent_audio} packets ({bg.skips} pacing skips here); console rx_audio "
         f"{peer['rx_audio'] if peer else '-'}")
    c.check("lost 0", peer is not None and peer["lost"] == 0,
            f"lost {peer['lost'] if peer else '-'}, late {peer['late'] if peer else '-'}, "
            f"concealed {t['concealed']}, recovered {t['recovered']}")
    c.check("underruns +0 after the first second", under_1s is not None and
            t["underruns"] == under_1s, f"{under_1s} at 1.5 s, {t['underruns']} at the end; "
            f"overruns {t['overruns']}, restarts {t['restarts']}, starts {t['starts']}")
    fills = [f for el, f, *_ in samples if el >= 5]
    c.check("fill within 20..50 ms", bool(fills) and all(20 <= f <= 50 for f in fills),
            f"{min(fills, default='-')}..{max(fills, default='-')} ms over {len(fills)} samples "
            f"(target {t['target_ms']} ms, jitter {t['jitter_us'] / 1000:.1f} ms)")
    trims = [p for el, _, p, *_ in samples if el >= 30]
    c.check("trim within +/- 500 ppm", bool(trims) and all(abs(p) <= 500 for p in trims),
            f"{min(trims, default='-')}..{max(trims, default='-')} ppm")
    if dump and dump[0] is not None:
        amp, rms, rate, gain = dump
        want = 10 ** (-20 / 20) * gain / 256
        db = 20 * math.log10(amp / want) if amp > 0 else -99
        c.check("team mix dump: 440 Hz at the expected level", abs(db) <= 2,
                f"{amp:.4f} (expected {want:.4f} = -20 dBFS x gain {gain}/256, {db:+.1f} dB) at "
                f"{rate} Hz")
    else:
        c.check("team mix dump", False, str(dump[3]) if dump else "not taken")
    if ear and ear[0] is not None and dump and dump[0] is not None:
        want = 10 ** (-20 / 20) * dump[3] / 256
        c.check("earpiece (xw.py dump) has the 440 Hz tone", ear[0] >= want / 2,
                f"{ear[0]:.4f} (expected about {want:.4f})")
    else:
        c.check("earpiece (xw.py dump)", False, "no capture")
    c.check("out_starved +0", d0 is not None and d1 is not None and
            d1["out_starved"] == d0["out_starved"],
            f"{d0['out_starved'] if d0 else '-'} -> {d1['out_starved'] if d1 else '-'}")
    info(f"team mix: {g['mix_calls']} blocks at {g['rate_out']} Hz x {g['ch_out']}, peak "
         f"{g['out_peak']}; talker peak {t['peak']}, mixed {t['mixed_ms'] / 1000:.0f} s")
    if not a.hands:
        print("OWNER CHECK a clean steady tone under the game audio in the headset; "
              "`team.py set level 3` makes it clearly quieter, `level 10` brings it back",
              flush=True)
    if not a.keep:
        team.setctl(x, addr, "stage", stage0)
        team.setctl(x, addr, "channel", channel0)
        team.setctl(x, addr, "level", level0)
        info(f"stage back to {stage0}, channel {channel0}, level {level0}")
    c.done()


if __name__ == "__main__":
    main()
