#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 3 acceptance (docs/PLAN.md, "Phase 3 definition of done"): consecutive xw.py reload
runs, each followed by checks: the take-over adopted the pad and left nothing behind, pad
input counters move, the headset streams (mic_pps ~1000), game audio is registered, the HUD
hook is adopted (if the HUD is on), in_double stays 0, and the console never went silent.

  XBOX=10.0.0.13 XW_DIR='Hdd:\\Plugins' python3 xbox360/accept/run.py p3_reload
  XBOX=... python3 xbox360/accept/p3_reload.py [--aurora N] [--in-game N] [--secs S]
                                               [--no-headset] [--no-game] [--hands] [-- ARGS]

  --aurora N     reloads at the Aurora dashboard first (default 5)
  --in-game N    then reloads inside a game (default 5; every reload is in place): it asks for
                 a Halo 3 match first (Enter, or 90 s without a terminal)
  --secs S       how long each reload's checks watch the new build (default 60)
  --no-headset   no headset in the jack: skip the mic_pps check
  --no-game      game audio off on purpose: skip the MEC check
  --hands        someone is at the pad: wait for input (a stick or button) after each reload
                 and judge it. Without it, a still pad is reported, not failed (an idle pad
                 sends nothing)
  -- ARGS        passed to every xw.py reload (e.g. -- --settle)
  P3_ARGS        the same options from the environment (accept/run.py passes no arguments)

Each reload builds the next slot (XW_SLOT=n bash toolchain/build.sh), about 40 s. Reboot
when the slot count gets high (24 at most; ~700 KB each).
"""
import argparse
import os
import shlex
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from accept import Checks, read_only, tee_log, tee_mark, tee_since, xw  # noqa: E402

import voice  # noqa: E402
import xw as xwpy  # noqa: E402
from xa import Xbdm, xw_block  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--aurora", type=int, default=5)
ap.add_argument("--in-game", dest="in_game", type=int, default=5)
ap.add_argument("--secs", type=float, default=60)
ap.add_argument("--no-headset", dest="headset", action="store_false")
ap.add_argument("--no-game", dest="game", action="store_false")
ap.add_argument("--hands", action="store_true")
argv = shlex.split(os.environ.get("P3_ARGS", "")) + sys.argv[1:]
extra = []
if "--" in argv:
    extra = argv[argv.index("--") + 1:]
    argv = argv[:argv.index("--")]
opt = ap.parse_args(argv)
c = Checks()


def newest(x):
    """(name, handover dict) of the newest loaded xw build"""
    slot, name, base, size = xwpy.xw_builds(x)[-1]
    return name, xwpy.find_ho(x, base, size)


def watch(run, mark):
    """the checks after one reload"""
    x = Xbdm()
    name, h = newest(x)
    c.check(f"{run}: {name} took over", h and h["take_state"] == xwpy.HO_T_DONE and h["adopted"] >= 1
            and not h["skipped"] and not h["take_error"],
            h and f"{h['adopted']} adopted, {h['skipped']} left alone, error {h['take_error']}")
    st = xw_block(xwpy.MAGIC)
    s0 = xwpy.read_status(x, st)
    rx0 = s0 and sum(p["rx"] for p in s0["pads"] if p["conn"])
    if opt.hands:
        print(f"OWNER: move a stick or press a button on the pad now ({run})", flush=True)
    until, rx_moved, pps, game, hooked = time.time() + opt.secs, False, [], None, None
    while time.time() < until:
        try:
            x = Xbdm()
            s = xwpy.read_status(x, st)
            if s:
                rx_moved |= sum(p["rx"] for p in s["pads"] if p["conn"]) > (rx0 or 0)
                if s["audio"]:
                    pps.append(s["audio"]["mic_pps"])
            g = voice.read_ga(x, voice.ga_addr())
            game = g and g["state"]
            hw = x.u32s(xw_block(xwpy.XH36_MAGIC), 16)
            hooked = hw and (hw[2], hw[14])  # enable, hooked
        except (OSError, SystemExit):
            pass
        if opt.hands and rx_moved and time.time() > until - opt.secs / 2:
            break
        time.sleep(1)
    pad = s0 and [p for p in s0["pads"] if p["conn"]]
    c.check(f"{run}: a pad is connected", bool(pad), f"{len(pad or [])} connected")
    if opt.hands:
        c.check(f"{run}: pad input arrives", rx_moved, "rx counter moved" if rx_moved else
                "no IN packet while someone moved the pad")
    else:
        print(f"{'ok' if rx_moved else 'note'} {run}: pad rx {'moved' if rx_moved else 'still (idle pad?)'}")
    if opt.headset:
        tail = pps[-10:]
        c.check(f"{run}: headset streams (mic_pps ~1000)", bool(tail) and min(tail) >= 950,
                f"last samples {tail}")
    if opt.game:
        c.check(f"{run}: game audio registered", game == 1,
                voice.GA_STATES.get(game, game) if game is not None else "no XVGA block")
    if hooked and hooked[0]:
        c.check(f"{run}: HUD hook adopted", hooked[1] == 1, f"hooked {hooked[1]}")
    x = Xbdm()
    name, h = newest(x)
    c.check(f"{run}: in_double 0", h and h.get("in_double") == 0, h and f"{h.get('in_double')}")
    if tee_log():
        silent = [l for l in tee_since(mark) if "CONSOLE SILENT" in l]
        c.check(f"{run}: the console never went silent", not silent, silent[0] if silent else "")


if read_only():
    print("(read-only: no reload is run)")
    x = Xbdm()
    name, h = newest(x)
    c.check("read-only: newest build has the WP3.1 trace", bool(h and h.get("trace_ring")), name)
    c.done()

runs = [("aurora", i + 1, []) for i in range(opt.aurora)] + \
       [("in-game", i + 1, []) for i in range(opt.in_game)]
for kind, i, args in runs:
    if kind == "in-game" and i == 1:
        print("OWNER: start a Halo 3 match (a custom game is fine) and stay in it; the "
              "controller will drop and come back once per reload", flush=True)
        if sys.stdin.isatty():
            input("Enter once the match is running ")
        else:
            time.sleep(90)
    run = f"{kind} {i}"
    mark = tee_mark()
    t0 = time.time()
    rc, out = xw("reload", *args, *extra, timeout=600)
    last = [l for l in out.strip().splitlines() if l.strip()][-1:] or [""]
    ok = c.check(f"{run}: xw.py reload", rc == 0 and "done:" in out,
                 f"{time.time() - t0:.0f} s; {last[0]}")
    if not ok:
        print(out[-3000:])
        break
    watch(run, mark)
print("OWNER: did the pad's light ever go out during these reloads? (it must not)")
c.done()
