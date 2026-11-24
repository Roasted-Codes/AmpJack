#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for headphone surround (surround_360.c, docs/ROADMAP.md section 6).

Needs an XW_DEV build for the channel walk and the level match (they inject noise into one
channel at a time ahead of the virtualizer); on a release build those checks FAIL with a
message saying so, and the rest still runs. The pad on with the jack headset streaming, any
title (Aurora is fine; a 5.1 game for J3). Run:

  XBOX=10.0.0.13 python3 xbox360/accept/surround.py [--hands]

  --hands   also run the the owner checks: the script pauses at each and asks y / n (without it
            they are printed as TODO lines and not counted)
  --demo-full  let the demo play to its end (D5: 41 s more) instead of stopping it

ACCEPT_READ_ONLY=1: only step 1. Everything written is put back at the end.

Checks:
  1. the build has the XWSR block; the table passed its check and the state was allocated
     (state not "no memory" / "bad table"), within the 16 KB heap budget;
  2. gate on: state "off"; game audio is streaming (the tap is copying);
  3. SURROUND on: state "on", blocks go through the virtualizer, one switch counted; the
     tap time stays within the budget (last and max <= 300 / 500 us);
  4. SURROUND off: one more switch, no more virtualized blocks;
  5. (XW_DEV) the channel walk: noise in FL, C, FR, SR, SL in turn; only that channel's
     peak shows each time;
  6. (XW_DEV) level match: the same front-left noise with SURROUND off and on; the ring's
     peak differs by what the model says (+4.6 dB for white noise on FL: the near ear's
     resonance; equal loudness for pink noise is gen.py's check), within 2 dB;
  7. no output clamped during the test;
  8. gate off: the gate closes (the tap back to today's code).
The demo (needs Hdd:\\Plugins\\xw.demo: tools/demo/make_demo.py, then xw.py install):
  D1 the file is found with the gate on (XWSR version 2);
  D2 demo start: playing within 1 s, its memory at most 64 KB, game audio still streaming;
  D3 3 s later: the clip advanced at the tap's rate (187.5 blocks a second, within 10 %), no
     underrun, the read-ahead never below a quarter of the ring, the tap within its budget;
  D4 SURROUND on while it plays: it keeps playing, one switch, blocks virtualized; off again;
  D5 stopped (or, --demo-full, played to the end): idle within 0.5 s, memory back to 0.
the owner checks (--hands), each one sentence:
  J1 Walk: the hiss moves front-left, centre, front-right, back-right, back-left, and the
     centre is in front of you, not inside your head.
  J2 SURROUND on and off in a game: the sound changes (wider, outside your head) but not
     its loudness, and there is no click.
  J3 Halo 3 with SURROUND on: a sound behind you (footsteps, a shot) sounds behind you.
  J4 The demo: chimes front-left, centre, front-right, back-right, back-left; a sound circling
     your head twice; footsteps behind you. With SURROUND OFF it all sounds flat and inside
     your head; ON, around you.
"""
import argparse
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XBOX360 = os.path.dirname(HERE)
sys.path.insert(0, XBOX360)

from accept import Checks, read_only  # noqa: E402

HEAP_BUDGET = 16384
DEMO_HEAP_BUDGET = 65536
FL_ON_OFF_DB = 4.6  # tools/hrtf model, 64 taps: the white test noise on FL, ring peak ON - OFF
SETTLE_S = 1.3      # a peak window is one second; one more to be sure the switch is in it


def tool():
    import importlib.util
    spec = importlib.util.spec_from_file_location("surround_tool", os.path.join(XBOX360, "surround.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def ask(hands, c, name, text):
    if not hands:
        print(f"TODO {name}: {text}")
        return
    ans = input(f"{name}: {text} [y/n] ").strip().lower()
    c.check(name, ans.startswith("y"), "the owner")


def run_demo(c, st, x, addr, args):
    """D1-D5 (the gate is on, SURROUND off); leaves the demo stopped and SURROUND off"""
    d = st.read(x, addr)
    if d["version"] < 2:
        c.check("D1 demo file found", False, f"XWSR version {d['version']}: a build without the demo")
        return
    for _ in range(40):
        if d["demo_file"] != 0:
            break
        time.sleep(0.05)
        d = st.read(x, addr)
    ok = d["demo_file"] == 1
    c.check("D1 demo file found", ok, st.name_of(st.DEMO_FILES, d["demo_file"]) +
            ("" if ok else ": python3 tools/demo/make_demo.py, then xw.py install --no-reboot"))
    if not ok:
        return
    st.write(x, addr, "demo_cmd", 1, d["dev"])
    t0 = time.time()
    while time.time() - t0 < 1.0:
        d = st.read(x, addr)
        if d["demo_state"] == 2:
            break
        time.sleep(0.05)
    import voice
    ga = voice.read_ga(x, voice.ga_addr()) or {}
    c.check("D2 demo playing, memory in budget, game audio streaming",
            d["demo_state"] == 2 and 0 < d["demo_heap"] <= DEMO_HEAP_BUDGET and ga.get("tap_on") == 1,
            f"state {st.name_of(st.DEMO_STATES, d['demo_state'])} after {time.time() - t0:.2f} s, "
            f"memory {d['demo_heap']} bytes, tap {'copying' if ga.get('tap_on') else 'idle'}")
    if d["demo_state"] != 2:
        st.write(x, addr, "demo_cmd", 2, d["dev"])
        return
    p0, u0, t0 = d["demo_pos"], d["demo_underruns"], time.time()
    time.sleep(3.0)
    d = st.read(x, addr)
    rate = (d["demo_pos"] - p0) / (time.time() - t0)
    c.check("D3 the clip runs at the tap's rate, no underrun, read-ahead kept, tap in budget",
            abs(rate - st.DEMO_BLOCKS_PER_S) <= 0.1 * st.DEMO_BLOCKS_PER_S
            and d["demo_underruns"] == u0 and d["demo_fill_min"] >= st.DEMO_RING_BLOCKS // 4
            and d["tap_us"] <= 300,
            f"{rate:.1f} blocks/s, underruns +{d['demo_underruns'] - u0}, fewest ahead "
            f"{d['demo_fill_min']} of {st.DEMO_RING_BLOCKS}, longest read {d['demo_read_max_us']} us, "
            f"tap {d['tap_us']} us (max {d['tap_max_us']})")
    s0, b0 = d["switches"], d["blocks"]
    st.write(x, addr, "surround", 1)
    time.sleep(1.0)
    d = st.read(x, addr)
    ok = d["demo_state"] == 2 and d["switches"] - s0 == 1 and d["blocks"] - b0 >= 150
    st.write(x, addr, "surround", 0)
    time.sleep(0.5)
    d = st.read(x, addr)
    c.check("D4 SURROUND on and off while the demo plays: it keeps playing",
            ok and d["demo_state"] == 2 and d["switches"] - s0 == 2,
            f"switches +{d['switches'] - s0}, blocks virtualized +{d['blocks'] - b0}, "
            f"tap max {d['tap_max_us']} us")
    if args.demo_full:
        left = (d["demo_blocks"] - d["demo_pos"]) / st.DEMO_BLOCKS_PER_S + 1.0
        time.sleep(max(0.0, left))
        want = 1
    else:
        st.write(x, addr, "demo_cmd", 2, d["dev"])
        want = 2
    time.sleep(0.5)
    d = st.read(x, addr)
    c.check("D5 demo " + ("played to the end" if want == 1 else "stopped") + ": idle, memory freed",
            d["demo_state"] == 0 and d["demo_end"] == want and d["demo_heap"] == 0,
            f"state {st.name_of(st.DEMO_STATES, d['demo_state'])}, ended: "
            f"{st.name_of(st.DEMO_ENDS, d['demo_end'])}, memory {d['demo_heap']}, underruns "
            f"{d['demo_underruns']}, bad pages {d['demo_bad_pages']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--demo-full", action="store_true")
    args = ap.parse_args()
    import voice
    st = tool()
    c = Checks()
    x, addr, d = st.connect()
    ok = d["state"] not in (3, 4)
    c.check("1 surround present, table checked, state allocated", ok and d["heap_bytes"] <= HEAP_BUDGET,
            f"state {st.STATES[d['state']]}, {d['taps']} taps, memory {d['heap_bytes']} bytes "
            f"(budget {HEAP_BUDGET}), {'XW_DEV' if d['dev'] else 'release'} build")
    if read_only() or not ok:
        c.done()
    saved = {k: d[k] for k in ("enable", "surround")}
    if d["dev"]:
        saved.update(test_ch=d["test_ch"], load_convs=d["load_convs"])
    clamps0 = d["clamps"]
    dev = d["dev"]
    try:
        st.write(x, addr, "surround", 0)
        st.write(x, addr, "enable", 1)
        time.sleep(0.5)
        d = st.read(x, addr)
        ga = voice.read_ga(x, voice.ga_addr()) or {}
        c.check("2 gate on: state off, game audio streaming", d["state"] == 1 and ga.get("tap_on") == 1,
                f"state {st.STATES[d['state']]}, tap {'copying' if ga.get('tap_on') else 'idle'}")
        b0, s0 = d["blocks"], d["switches"]
        st.write(x, addr, "surround", 1)
        time.sleep(SETTLE_S)
        d = st.read(x, addr)
        c.check("3 SURROUND on: virtualizing, one switch, tap time in budget",
                d["state"] == 2 and d["blocks"] - b0 >= 150 and d["switches"] - s0 == 1
                and d["tap_us"] <= 300 and d["tap_max_us"] <= 500,
                f"blocks +{d['blocks'] - b0}, switches +{d['switches'] - s0}, tap {d['tap_us']} us "
                f"(max {d['tap_max_us']})")
        st.write(x, addr, "surround", 0)
        time.sleep(0.5)
        b1 = st.read(x, addr)["blocks"]
        time.sleep(0.5)
        d = st.read(x, addr)
        c.check("4 SURROUND off: one more switch, nothing virtualized", d["state"] == 1
                and d["switches"] - s0 == 2 and d["blocks"] == b1,
                f"switches +{d['switches'] - s0}, blocks after the switch +{d['blocks'] - b1}")
        run_demo(c, st, x, addr, args)
        d = st.read(x, addr)
        if not d["dev"]:
            c.check("5 channel walk", False, "needs an XW_DEV build (it injects test noise)")
            c.check("6 level match", False, "needs an XW_DEV build (it injects test noise)")
        else:
            seen = st.walk(2.0)
            bad = []
            for n, peaks in seen.items():
                others = [p for i, p in enumerate(peaks) if i != n - 1]
                if peaks[n - 1] < 3000 or max(others) > 33:  # the burst; others under -60 dBFS
                    bad.append(f"{st.CHANNELS[n - 1]}: " + " ".join(st.dbfs(p) for p in peaks))
            c.check("5 channel walk: only the walked channel carries the noise", not bad,
                    "; ".join(bad) or "FL, C, FR, SR, SL each alone")
            ask(args.hands, c, "J1", "Walk: the hiss moves front-left, centre, front-right, "
                "back-right, back-left, and the centre is in front of you, not inside your head.")
            peaks = {}
            st.write(x, addr, "test_ch", 1)
            for on in (0, 1):
                st.write(x, addr, "surround", on)
                time.sleep(2 * SETTLE_S)
                ga = voice.read_ga(x, voice.ga_addr()) or {}
                peaks[on] = ga.get("in_peak", 0)
            st.write(x, addr, "test_ch", 0)
            st.write(x, addr, "surround", 0)
            diff = 20 * math.log10(max(peaks[1], 1) / max(peaks[0], 1))
            c.check("6 level match: front-left noise, ring peak ON - OFF as the model says",
                    abs(diff - FL_ON_OFF_DB) <= 2.0,
                    f"OFF {peaks[0]}, ON {peaks[1]}: {diff:+.1f} dB (model {FL_ON_OFF_DB:+.1f})")
        if args.hands and st.read(x, addr).get("demo_file") == 1:
            st.demo(x, addr, st.read(x, addr), ["start"])
        ask(args.hands, c, "J4", "The demo (playing now; surround.py on|off flips it): chimes "
            "front-left, centre, front-right, back-right, back-left; a sound circling your head "
            "twice; footsteps behind you. SURROUND OFF sounds flat, ON sounds around you.")
        if args.hands:
            st.write(x, addr, "demo_cmd", 2, dev)
        ask(args.hands, c, "J2", "SURROUND on and off in a game: the sound changes (wider, "
            "outside your head) but not its loudness, and there is no click.")
        ask(args.hands, c, "J3", "Halo 3 with SURROUND on: a sound behind you (footsteps, a "
            "shot) sounds behind you.")
        d = st.read(x, addr)
        c.check("7 nothing clamped", d["clamps"] == clamps0, f"clamped samples +{d['clamps'] - clamps0}")
        st.write(x, addr, "enable", 0)
        time.sleep(0.5)
        d = st.read(x, addr)
        c.check("8 gate off: closed", d["state"] == 0, f"state {st.STATES[d['state']]}")
    finally:
        if d.get("version", 1) >= 2:
            st.write(x, addr, "demo_cmd", 2, dev)  # stopped if it plays (nothing otherwise)
        for k, v in saved.items():
            st.write(x, addr, k, v)
    c.done()


if __name__ == "__main__":
    main()
