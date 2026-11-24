#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""MIC-AUTO (micdsp_360.c): NOISE GATE AUTO, MIC VOLUME AUTO and the 24 -> 8 kHz filter, over
xbdm. At boot NOISE GATE AUTO is on (and gate auto is the default gate); the others are off.

  micauto.py status          the switches, the floor / threshold / speech level / AUTO gain of
                             the first headset, the counters, the memory in use
  micauto.py watch [S]       one line a second for S seconds (default 30): what AUTO sees
  micauto.py gate on|off     offer NOISE GATE AUTO: the panel shows it, and a gate set to auto
                             (xw.py diag gate auto) follows the room. Off: auto runs as low
  micauto.py volume on|off   offer MIC VOLUME AUTO: the panel shows AUTO left of 1. Off: AUTO is
                             never in effect, whatever was picked
  micauto.py auto on|off     pick MIC VOLUME AUTO, as the panel does (needs `volume on`)
  micauto.py fir on|off      the 360's mic through the new 24 -> 8 kHz low-pass instead of the
                             old resampler (a cleaner voice for teammates; +0.88 ms)
  micauto.py comb on|off     XW_DEV builds: the 125 Hz comb canceller on the mic

XBOX=host[:port] as for every tool. The block's layout is micdsp_360.c's struct ma_status
(its magic is MAGIC below).
"""
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MAGIC = 0x58574D41
VERSION = 1
FIELDS = ("magic version gate_auto mic_auto fir comb auto_on pad floor_rms thr_rms speech_rms "
          "agc_q12 agc_level agc_state speech_windows auto_opens fir_samples fir_bypass "
          "heap_bytes nomem").split()
assert len(FIELDS) == 20  # _Static_assert in micdsp_360.c
PADS = 4
SWITCHES = {"gate": "gate_auto", "volume": "mic_auto", "auto": "auto_on", "fir": "fir",
            "comb": "comb"}
FROZEN, ACQUIRING, MOVING = 1, 2, 4
SPEECH_WINDOWS = 256   # MA_AGC_AVG_SHIFT: the AGC's average is full
TARGET_DB = -31.5      # MA_AGC_TARGET_DB: AUTO aims the speech level (dB mean) here
GATE_MIN_DB, GATE_MAX_DB, GATE_MARGIN_DB = -60, -35, 9


def block_addr():
    from xa import xw_block
    return xw_block(MAGIC)


def connect():
    from xa import Xbdm
    return Xbdm(), block_addr()


def read_block(x, addr):
    w = x.u32s(addr, len(FIELDS))
    if not w or w[0] != MAGIC:
        raise SystemExit(f"mic-auto block not found at {addr:08x} (an xw build with MIC-AUTO?)")
    if w[1] != VERSION:
        raise SystemExit(f"mic-auto block version {w[1]}: this micauto.py reads version {VERSION}")
    return dict(zip(FIELDS, w))


def read(x=None, addr=None):
    if x is None:
        x, addr = connect()
    return read_block(x, addr)


def setctl(x, addr, name, value):
    return x.setu32(addr + 4 * FIELDS.index(name), value & 0xFFFFFFFF)


def db(rms):
    """a window RMS (full scale 32768) in dBFS; None for 0"""
    return 20 * math.log10(rms / 32768) if rms else None


def q12_db(q12):
    return 20 * math.log10(q12 / 4096) if q12 else None


def fmt_db(v):
    return "-" if v is None else f"{v:.1f} dBFS"


def state_text(s):
    return ", ".join(n for b, n in ((FROZEN, "frozen"), (ACQUIRING, "acquiring"),
                                    (MOVING, "moving")) if s & b) or "adapting"


def print_status(g):
    sw = lambda v: "on" if v else "off"
    print(f"switches: noise gate auto {sw(g['gate_auto'])}, mic volume auto {sw(g['mic_auto'])}"
          f" (picked: {'yes' if g['auto_on'] else 'no'}), 8 kHz filter {sw(g['fir'])}, comb "
          f"{sw(g['comb'])}; memory {g['heap_bytes']} bytes"
          + (f"; {g['nomem']} failed allocations" if g["nomem"] else ""))
    if g["pad"] >= PADS:
        print("no headset (or every switch off: nothing is tracked then)")
    elif not g["heap_bytes"]:
        print(f"pad {g['pad']}: every switch off, nothing is tracked")
    else:
        print(f"pad {g['pad']}: floor {fmt_db(db(g['floor_rms']))}, AUTO's gate threshold "
              f"{fmt_db(db(g['thr_rms']))} (rms {g['thr_rms']}), speech level "
              f"{fmt_db(db(g['speech_rms']))} over {g['speech_windows']} windows")
        print(f"  AUTO's gain {q12_db(g['agc_q12']):+.1f} dB ({g['agc_q12'] * 100 // 4096} %, "
              f"level {g['agc_level']}), {state_text(g['agc_state'])}")
    print(f"counters: gate opened {g['auto_opens']} times on AUTO; filter made "
          f"{g['fir_samples']} samples, {g['fir_bypass']} chunks at another rate")


def watch(x, addr, secs):
    t_end = time.time() + secs
    while time.time() < t_end:
        g = read_block(x, addr)
        print(f"{time.strftime('%H:%M:%S')} floor {fmt_db(db(g['floor_rms'])):>11} thr "
              f"{fmt_db(db(g['thr_rms'])):>11} speech {fmt_db(db(g['speech_rms'])):>11} gain "
              f"{q12_db(g['agc_q12']) or 0:+5.1f} dB level {g['agc_level']:2d} "
              f"{state_text(g['agc_state'])}", flush=True)
        time.sleep(1)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    args = sys.argv[2:]
    if cmd in SWITCHES and args and args[0] in ("on", "off"):
        if cmd == "comb":
            import xw
            xw.need_dev("micauto.py comb")
        x, addr = connect()
        print(SWITCHES[cmd], "=", int(args[0] == "on"),
              setctl(x, addr, SWITCHES[cmd], int(args[0] == "on")))
        return
    if cmd not in ("status", "watch"):
        raise SystemExit(__doc__)
    x, addr = connect()
    if cmd == "status":
        print_status(read_block(x, addr))
    else:
        watch(x, addr, float(args[0]) if args else 30)


if __name__ == "__main__":
    main()
