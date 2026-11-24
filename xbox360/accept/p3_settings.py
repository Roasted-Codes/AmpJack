#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""WP3.4 acceptance (docs/PLAN.md): the user's settings survive a hot reload. Sets balance -3
and the noise gate high on the running build, reloads (in place), and checks that the new
build has both, and the headset's format; then puts the old values back.

  XBOX=10.0.0.13 XW_DIR='Hdd:\\Plugins' python3 xbox360/accept/run.py p3_settings
  XBOX=... python3 xbox360/accept/p3_settings.py [--hands] [-- RELOAD ARGS]

  --hands   someone is at the pad: they mute the mic with a Share tap before the reload, and
            the script checks the new build has it muted (then they unmute it)
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from accept import Checks, read_only, xw  # noqa: E402

import voice  # noqa: E402
import xw as xwpy  # noqa: E402
from xa import Xbdm  # noqa: E402

args = sys.argv[1:]
extra = args[args.index("--") + 1:] if "--" in args else []
hands = "--hands" in args
c = Checks()


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def state():
    x = Xbdm()
    g = voice.read_ga(x, voice.ga_addr())
    d = xwpy.read_diag(x)
    h = voice.read_hs(x)
    return g, d, h


g, d, h = state()
c.check("blocks readable (XVGA, XWDG)", bool(g and d), "")
if read_only() or not (g and d):
    c.done()
old_bal, old_gate = s32(g["balance"]), d["gate"]
x = Xbdm()
x.setu32(voice.ga_addr() + 4 * voice.GA_FIELDS.index("balance"), -3 & 0xFFFFFFFF)
x.setu32(xwpy.diag_addr() + 4 * xwpy.DIAG_FIELDS.index("gate"), 2)
time.sleep(1)
g, d, h = state()
c.check("before: balance -3, gate high", s32(g["applied_balance"]) == -3 and d["applied_gate"] == 2,
        f"balance {s32(g['applied_balance'])}, gate {d['applied_gate']}")
if hands:
    input("OWNER: tap Share once (the mic mutes), then Enter ")
    g, d, h = state()
muted_before = bool(h and h["pads"][0]["muted"])

rc, out = xw("reload", *extra, timeout=600)
c.check("xw.py reload", rc == 0 and "done:" in out, (out.strip().splitlines() or [""])[-1])
time.sleep(5)
g, d, h = state()
c.check("after: balance still -3", g and s32(g["applied_balance"]) == -3,
        g and f"balance {s32(g['applied_balance'])}")
c.check("after: gate still high", d and d["applied_gate"] == 2, d and f"gate {d['applied_gate']}")
if hands:
    c.check("after: mic still muted", bool(h and h["pads"][0]["muted"]) == muted_before,
            f"muted {h and h['pads'][0]['muted']}, before {muted_before}")
x = Xbdm()
name, base, size = xwpy.xw_builds(x)[-1][1:]
ho = xwpy.find_ho(x, base, size)
fmt = ho and ho["ports"][0].get("hs_fmt", 0)
print(f"(headset format handed to the next build: {fmt:#x})" if ho else "")

# put the old values back
x.setu32(voice.ga_addr() + 4 * voice.GA_FIELDS.index("balance"), old_bal & 0xFFFFFFFF)
x.setu32(xwpy.diag_addr() + 4 * xwpy.DIAG_FIELDS.index("gate"), old_gate)
if hands and muted_before:
    print("OWNER: tap Share once more to unmute")
c.done()
