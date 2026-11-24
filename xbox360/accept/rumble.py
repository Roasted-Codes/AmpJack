#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RUMBLE acceptance (rumble_360.c): the Guide's Vibration setting is honoured on wired pads,
and every stop reaches the controller quickly.

  XBOX=10.0.0.13 python3 xbox360/accept/rumble.py [--pad N] [--hands] [--compare]

Needs a controller bound on pad slot N (default 0) and no game running that sets rumble
every frame (at the dashboard is right).

  1. (read only) XWRB is there, gate ON, XAM's Vibration table recognised (0x81AAC2DC on
     17559), and the pad's setting in XWRB is XAM's own byte for the pad's user.
  2. (writes) A test request 50/50 through the title's path:
       Vibration ON:  the pad is sent 50/50 (it rumbles for ~0.5 s); then 0/0: the pad is sent
                      0 within 0.5 s, the stop is re-sent once, its latency is under 500 ms.
                      Then 30/30 left alone: it stops by itself after 3 s.
       Vibration OFF: the pad is sent 0/0 and stays there (no rumble), "silenced" goes up.
  3. --compare (writes; Vibration OFF only): the gate off for one second shows the old
     behaviour (the pad IS sent 50 and rumbles briefly despite the setting), then on again.
  4. --hands: someone at the console switches Vibration in the Guide (Settings > Preferences
     > Vibration), first to the other value, then back; each time the script sees XAM's byte
     change and re-runs step 2. Then, in a game, while the pad rumbles, they open the Guide
     and quit the game: the pad must be sent 0 within 0.5 s (reason "the title asked" or
     "the title changed").

What "fixed" means: with Vibration OFF the pad's `sent` stays 0/0 whatever the title asks and
`silenced` counts the requests; with it ON a stop is out to the pad in well under 500 ms
(`stop_lat_ms`, typically one or two 16 ms passes) and `re-sent` goes up by one per stop.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# xbox360/ first: this file is also called rumble.py; `import rumble` must find xbox360/rumble.py
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from accept import Checks, read_only  # noqa: E402

import rumble as xr  # noqa: E402

STOP_S = 0.5           # a stop must be out to the pad within this
HANDS_WAIT_S = 120     # someone changes the setting in the Guide
GAME_WAIT_S = 600      # someone starts a game, makes the pad rumble, quits from the Guide

c = Checks()


def pad(g, n):
    return g["pads"][n]


def setting_matches_xam(x, g, n):
    p = pad(g, n)
    xt = xr.xam_table(x, g)
    if not p["known"] or not xt:
        return False, "not known"
    user = 0 if p["user"] is None else p["user"]
    ok = (xt[user] != 0) == p["on"]
    return ok, f"user {user}: XAM byte {xt[user]}, XWRB {'on' if p['on'] else 'off'}"


def step_test(x, addr, n, label):
    """step 2 for the setting as it is now"""
    g0 = xr.read_block(x, addr)
    on = pad(g0, n)["on"]
    if not c.check(f"{label}: test request taken", xr.test(x, addr, n, 50, 50)):
        return
    if on:
        g = xr.wait_for(x, addr, lambda g: pad(g, n)["sent"] == (50, 50), 0.5)
        c.check(f"{label}: Vibration ON: the pad is sent 50/50", pad(g, n)["sent"] == (50, 50),
                f"sent {pad(g, n)['sent']}")
        time.sleep(0.3)
        t0 = time.time()
        xr.test(x, addr, n, 0, 0)
        g = xr.wait_for(x, addr, lambda g: pad(g, n)["sent"] == (0, 0), STOP_S + 0.5)
        dt = time.time() - t0
        c.check(f"{label}: the stop is out to the pad within {STOP_S} s",
                pad(g, n)["sent"] == (0, 0) and dt <= STOP_S,
                f"{dt * 1000:.0f} ms seen over xbdm; pad's own latency {g['stop_lat_ms']} ms")
        c.check(f"{label}: stop latency on the console under 500 ms",
                g["stop_asked"] > g0["stop_asked"] and g["stop_lat_ms"] < 500,
                f"stop_lat_ms {g['stop_lat_ms']}, stops asked {g0['stop_asked']} -> "
                f"{g['stop_asked']}")
        g = xr.wait_for(x, addr, lambda g: g["restops"] > g0["restops"], 1.0)
        c.check(f"{label}: the stop is re-sent once", g["restops"] == g0["restops"] + 1,
                f"re-sent {g0['restops']} -> {g['restops']}")
        g1 = g
        xr.test(x, addr, n, 30, 30)
        g = xr.wait_for(x, addr, lambda g: g["stop_test"] > g1["stop_test"],
                        xr.TEST_MS / 1000 + 1.0, step=0.1)
        c.check(f"{label}: a test left running stops by itself after 3 s",
                g["stop_test"] > g1["stop_test"] and pad(g, n)["sent"] in ((0, 0), (255, 255)),
                f"sent {pad(g, n)['sent']}")
    else:
        g = xr.wait_for(x, addr, lambda g: g["zeroed"] > g0["zeroed"], 0.5)
        c.check(f"{label}: Vibration OFF: the request is silenced", g["zeroed"] > g0["zeroed"],
                f"silenced {g0['zeroed']} -> {g['zeroed']}")
        worst = (0, 0)
        for _ in range(10):
            s = pad(xr.read_block(x, addr), n)["sent"]
            worst = max(worst, s)
            time.sleep(0.1)
        c.check(f"{label}: Vibration OFF: the pad is sent nothing but 0 for 1 s",
                worst == (0, 0), f"highest sent {worst}")
        xr.test(x, addr, n, 0, 0)


def step_compare(x, addr, n):
    g0 = xr.read_block(x, addr)
    if pad(g0, n)["on"]:
        print("--compare: Vibration is ON for this pad; nothing to compare")
        return
    xr.setctl(x, addr, "enable", 0)
    try:
        xr.test(x, addr, n, 50, 50)
        g = xr.wait_for(x, addr, lambda g: pad(g, n)["sent"] == (50, 50), 0.5)
        c.check("gate off (old behaviour): the pad IS sent 50/50 despite Vibration OFF",
                pad(g, n)["sent"] == (50, 50), f"sent {pad(g, n)['sent']}")
        time.sleep(0.3)
        xr.test(x, addr, n, 0, 0)
    finally:
        xr.setctl(x, addr, "enable", 1)
    g = xr.wait_for(x, addr, lambda g: pad(g, n)["sent"] == (0, 0), 1.0)
    c.check("gate back on, the pad stopped", g["enable"] == 1 and pad(g, n)["sent"] == (0, 0))


def step_hands_setting(x, addr, n):
    for k in range(2):
        g0 = xr.read_block(x, addr)
        was = pad(g0, n)["on"]
        want = "OFF" if was else "ON"
        print(f"HANDS: in the Guide, Settings > Preferences > Vibration: switch it {want} "
              f"(waiting up to {HANDS_WAIT_S} s)", flush=True)
        g = xr.wait_for(x, addr, lambda g: pad(g, n)["on"] not in (None, was), HANDS_WAIT_S,
                        step=0.2)
        if not c.check(f"the Guide's switch to {want} is seen", pad(g, n)["on"] != was):
            return
        ok, d = setting_matches_xam(x, g, n)
        c.check(f"Vibration {want}: XWRB matches XAM's byte", ok, d)
        step_test(x, addr, n, f"after switching {want}")


def step_hands_game(x, addr, n):
    g0 = xr.read_block(x, addr)
    if not pad(g0, n)["on"]:
        print("HANDS (game): Vibration is OFF for this pad, switch it ON first; skipped")
        return
    print("HANDS: start a game, make the pad rumble, then (while it rumbles) press the Xbox "
          f"button and quit the game (waiting up to {GAME_WAIT_S} s)", flush=True)
    xr.wait_for(x, addr, lambda g: max(pad(g, n)["req"]) > 0 and g["tests"] == g0["tests"],
                GAME_WAIT_S, step=0.05)
    g1 = xr.wait_for(x, addr, lambda g: (g["stop_asked"] + g["stop_title"]
                                         > g0["stop_asked"] + g0["stop_title"])
                     and pad(g, n)["sent"] == (0, 0) and max(pad(g, n)["req"]) == 0,
                     GAME_WAIT_S, step=0.05)
    c.check("leaving the game stops the pad", pad(g1, n)["sent"] == (0, 0),
            f"last stop: {xr.WHY[g1['last_why']]}, out in {g1['stop_lat_ms']} ms")
    c.check("... within 0.5 s of the stop", g1["stop_lat_ms"] < 500)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pad", type=int, default=0, choices=range(4))
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--compare", action="store_true")
    a = ap.parse_args()
    n = a.pad
    x, addr = xr.connect()
    g = xr.read_block(x, addr)
    xr.print_status(x, g)
    c.check("XWRB gate ON (the default)", g["enable"] == 1)
    c.check("XAM's Vibration table recognised", g["table"] != 0, f"{g['table']:08x}")
    c.check("... at the 17559 address", g["table"] == xr.TABLE_17559, f"{g['table']:08x}")
    if not c.check(f"a controller is bound on pad {n}", pad(g, n)["known"]):
        c.done()
    ok, d = setting_matches_xam(x, g, n)
    c.check("the pad's setting is XAM's byte for its user", ok, d)
    if read_only():
        print("ACCEPT_READ_ONLY: the test requests are skipped")
        c.done()
    step_test(x, addr, n, "now")
    if a.compare:
        step_compare(x, addr, n)
    if a.hands:
        step_hands_setting(x, addr, n)
        step_hands_game(x, addr, n)
    c.done()


if __name__ == "__main__":
    main()
