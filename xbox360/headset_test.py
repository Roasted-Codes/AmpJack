#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One-shot headset audio test on the console, from a switched-on pad to a XamVoice round trip.

  XBOX=10.0.0.13 python3 headset_test.py [--keep]

Needs: the pad on port 0 switched on (Xbox button lit: a cold reboot leaves it dark and only
the button wakes it), a headset in its jack, the console at the dashboard, out/ from the build
that is installed. Each USB stage is entered with `hsrestart` (the headset identifies itself
again and the new stage is latched), checked, and the run stops at the first failure with audio
switched back off. The last step runs voicetest through the real headset: the tone it plays
into the earpiece must come back on the in-ear mic. --keep leaves audio stage 4 + voice stage 4
on afterwards (the headset usable in games); otherwise both go back off.
"""
import os
import subprocess
import sys
import time

import xw
from xa import Xbdm, read_log

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "..", "research", "results")
F_IN, F_OUT, F_STREAM, F_BROKEN, F_READY = 2, 4, 8, 32, 64
OK_FRAME = (0, 0xC0050003)  # short packets complete with "data underrun": data valid


def status():
    s = xw.read_status(Xbdm(), ADDR)
    if not s or not s["audio"]:
        raise SystemExit("xw.xex status (version 2 with audio) not found: is this build installed?")
    return s


def log_since(since):
    x = Xbdm()
    s = xw.read_status(x, ADDR)
    since, text = read_log(x, s["log"], since)
    return since, text


def set_word(off, value):
    Xbdm().setu32(ADDR + off, value)


def wait_for(cond, secs, what):
    end = time.time() + secs
    while time.time() < end:
        a = status()["audio"]
        if cond(a):
            return a
        time.sleep(0.25)
    print(f"  timed out after {secs} s waiting for {what}")
    return None


def audio_off():
    set_word(xw.OFF_AUD_STAGE, 0)
    set_word(xw.OFF_AUD_RESET, 0x101)  # hsrestart 0: the headset re-identifies, stage 0 refuses


def enter(stage):
    set_word(xw.OFF_AUD_STAGE, stage)
    set_word(xw.OFF_AUD_RESET, 0x101)
    print(f"\n== audio stage {stage}")


def fail(why):
    print(f"\nFAILED: {why}")
    audio_off()
    time.sleep(1)
    _, text = log_since(LOG0)
    print("--- plugin log since the test started ---")
    print(text[-6000:])
    xw.print_audio(status()["audio"])
    sys.exit(1)


def main():
    global ADDR, LOG0
    keep = "--keep" in sys.argv
    ADDR = xw.status_addr()
    s = status()
    LOG0, _ = log_since(0)
    pad = s["pads"][0]
    if not pad["conn"]:
        raise SystemExit("no controller on port 0")
    # an idle pad only sends on input changes: any traffic since it was bound means it's awake
    if not pad["rx"]:
        raise SystemExit("the pad on port 0 sends nothing: press its Xbox button (it must be lit)")
    a = s["audio"]
    if a["flags"] & F_BROKEN:
        raise SystemExit("audio is marked BROKEN (an earlier USB request never completed): reboot")
    print(f"pad 0 {pad['vid']:04x}:{pad['pid']:04x} is talking; audio endpoints IN {a['ep_in']:02x} "
          f"({a['max_in']} B) OUT {a['ep_out']:02x} ({a['max_out']} B)")

    # 1: SET_INTERFACE(1,1) and both iso endpoints open, nothing queued
    enter(1)
    a = wait_for(lambda a: a["active_stage"] == 1 and a["flags"] & F_IN and a["flags"] & F_OUT,
                 10, "both audio endpoints open")
    if not a or a["alt_status"] or a["open_in"] or a["open_out"]:
        fail("stage 1: SET_INTERFACE 1/1 or the endpoint opens didn't succeed")
    print(f"  ok: SET_INTERFACE 1/1 {a['alt_status']:08x}, opens IN {a['open_in']:08x} "
          f"OUT {a['open_out']:08x}")

    # 2: exactly one IN block, its per-frame status and lengths
    enter(2)
    a = wait_for(lambda a: a["active_stage"] == 2 and a["in_done"] >= 1, 12, "one IN block")
    if not a:
        fail("stage 2: the IN block never completed")
    frames = list(zip(a["first_status"], a["first_len"]))
    print("  first IN block: " + " ".join(f"{st:08x}/{ln}" for st, ln in frames) +
          "  bytes " + a["first_bytes"].hex(" "))
    if any(st not in OK_FRAME and st != 0xC005100E for st, _ in frames):
        fail("stage 2: an IN frame completed with an error")
    if not any(ln for _, ln in frames):
        print("  note: all 8 frames empty (the headset may not have been streaming yet)")

    # 3: continuous mic
    enter(3)
    a = wait_for(lambda a: a["active_stage"] == 3 and a["mic_pps"] >= 500, 15, "~1000 mic packets/s")
    if not a:
        fail("stage 3: the mic stream didn't reach 500 packets/s")
    time.sleep(3)
    a = status()["audio"]
    print(f"  ok: mic {a['mic_pps']}/s, IN frames ok {a['in_frames_ok']} errors "
          f"{a['in_frames_err']}, dropped {a['in_dropped']}")
    if a["flags"] & F_BROKEN:
        fail("stage 3: audio went BROKEN")

    # 5: tone into the earpiece, detected on the in-ear mic; record 1 s of mic
    set_word(xw.OFF_AUD_TONE, 1000)
    enter(5)
    a = wait_for(lambda a: a["active_stage"] == 5 and a["ear_bps"] >= 60 and a["windows"] >= 20,
                 15, "earpiece blocks and 2 s of mic analysis")
    if not a:
        fail("stage 5: the earpiece stream didn't start")
    time.sleep(2)
    a = status()["audio"]
    ratio = a["tone_amp"] / a["off_amp"] if a["off_amp"] else float("inf")
    print(f"  earpiece {a['ear_bps']} blocks/s (OUT frame errors {a['out_frames_err']}); mic: "
          f"1 kHz {a['tone_amp']} vs off-tone {a['off_amp']} (ratio {ratio:.1f}), rms "
          f"{a['mic_rms']}, peak {a['mic_peak']}")
    os.makedirs(RESULTS, exist_ok=True)
    wav = os.path.join(RESULTS, time.strftime("mic-tone-%Y%m%d-%H%M%S.wav"))
    try:
        xw.micdump(wav)
    except SystemExit as e:
        print(f"  mic recording failed: {e}")
    if a["out_frames_err"] > a["out_done"] // 10:
        fail("stage 5: too many earpiece frame errors")
    acoustic = ratio > 3
    print("  ok: the tone comes back on the mic" if acoustic else
          "  WARNING: tone not clearly on the mic (headset on the desk? listen to " + wav + ")")

    # 4 + voice 4: the whole chain, voicetest's tone through XamVoice -> earpiece -> mic -> XamVoice
    enter(4)
    a = wait_for(lambda a: a["active_stage"] == 4 and a["ear_bps"] >= 60 and a["flags"] & 16,
                 15, "stage 4 streaming and the voice side told")
    if not a:
        fail("stage 4: streaming or the hand-over to the voice side didn't happen")
    print("\n== voice stage 4 + voicetest through the real headset")
    r = subprocess.run([sys.executable, os.path.join(HERE, "voice.py"), "test", "--stage", "4",
                        "--force", "0", "--loopback", "0", "--rates", "8000,16000,24000"],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    path = os.path.join(RESULTS, time.strftime("voicetest-headset-%Y%m%d-%H%M%S.txt"))
    open(path, "w").write(out)
    summary = out[out.find("--- summary"):] if "--- summary" in out else out[-3000:]
    print(summary)
    print(f"(full output: {path})")

    if keep:
        print("\naudio stage 4 and voice stage 4 left on: the headset is the pad's 360 headset now")
    else:
        subprocess.run([sys.executable, os.path.join(HERE, "voice.py"), "stage", "2"])
        audio_off()
        print("\naudio and voice switched back off (use --keep to leave them on)")


if __name__ == "__main__":
    main()
