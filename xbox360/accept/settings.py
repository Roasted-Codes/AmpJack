#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""SETTINGS acceptance (docs/ROADMAP.md, SETTINGS): what a player changes on the panel is
saved to Hdd:\\Plugins\\xw.settings by itself and is back after a cold reboot.

  XBOX=10.0.0.13 python3 xbox360/accept/settings.py [--pad N] [--hands] [--no-reboot]

  1. Changes BALANCE, VOLUME and NOISE GATE one or two steps each over XWPN inject (the panel,
     exactly as a thumb would), waits 3 s, and checks the save: XWST saves, the save time, and
     that the earpiece did not starve and missed no frames during it.
  2. Reads the file back over xbdm and checks the active profile's record: the three values,
     its CRC, the header.
  3. Cold reboot (magicboot cold), waits for the console, and checks all three are back.
  4. Puts the starting values back (saved too) and the gate as it was.

The XWST gate is off by default: if it is off, the script switches it on before step 1 and
again after the reboot. Switching it on runs the same path as a boot (the file is loaded
once and the active profile restored), so step 3 checks the boot restore; with a build where
the gate defaults on, nothing is switched.

Without --hands, leave the pad off: the profile is then "nobody signed in" (XUID 0) before and
after the reboot (the pad stays dark after a cold boot until its Xbox button is pressed).
With --hands, someone at the console switches the pad on after the reboot and signs in to the
same profile as before; the script waits for that profile.

Audio counters move only while a headset is streaming: with no headset, "no starving" is
trivially true and the script says so.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# xbox360/ first: Python put this script's own folder first, and this file is also called
# settings.py; `import settings` must find xbox360/settings.py
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from accept import Checks, read_only  # noqa: E402

import settings as st  # noqa: E402  (xbox360/settings.py: XWST and the file)
import wp1_3 as wp  # noqa: E402  (Console: XWPN inject and the mix / diag reads)
from xa import Xbdm  # noqa: E402

SAVE_WAIT_S = 3.0      # 2 s after the last change, a 250 ms snapshot, the write
BOOT_WAIT_S = 240      # magicboot cold to xbdm answering (it sometimes switches off instead)
RESTORE_WAIT_S = 15    # xbdm up to the plugin's restore
HANDS_WAIT_S = 180     # someone switches the pad on and signs in

c = Checks()


def flow(con):
    """the headset's flow reports (XWDG): they move only while a headset is attached"""
    d = con.diag()
    return d["flow_below"] + d["flow_equal"] + d["flow_above"]


def three(con):
    """(balance, volume %, gate) as applied now"""
    m, d = con.mix(), con.diag()
    return m["balance"], m["volume"], d["applied_gate"]


def gate_on(x, addr):
    """switches the XWST gate on if it is off; waits for READY and the restore. The block."""
    g = st.read_block(x, addr)
    if not g["enable"]:
        st.setctl(x, addr, "enable", 1)
    return st.wait_for(x, addr, lambda g: g["state"] == 2 and g["restores"] > 0, RESTORE_WAIT_S)


def change_three(con, b0, v0, g0):
    """BALANCE two steps, VOLUME one, NOISE GATE one, with Share held; the targets"""
    con.inject(0)
    time.sleep(wp.SETTLE_S)
    con.inject(wp.SHARE)
    time.sleep(0.3)  # a hold shows the panel, on BALANCE
    c.check("panel shown by a 300 ms Share hold", con.panel()["visible"] == wp.PANEL)
    db = -1 if b0 > -3 else 1
    for _ in range(2):
        con.press(wp.LEFT if db < 0 else wp.RIGHT)
    wp.walk_to(con, wp.VOLUME)
    con.press(wp.LEFT if v0 > 0 else wp.RIGHT)
    wp.walk_to(con, wp.GATE)
    # the panel's order is AUTO, OFF, LOW, HIGH: Right from anything but HIGH, else Left
    con.press(wp.RIGHT if g0 != wp.G_HIGH else wp.LEFT)
    con.inject(0)
    time.sleep(wp.SETTLE_S)
    return three(con)


def file_record(xuid):
    data = Xbdm().getfile(st.CONSOLE_FILE)
    hdr, recs, problems = st.parse_file(data)
    return hdr, st.record_for(recs, xuid), problems


def wait_console():
    """True once xbdm answers again and the XWST block can be read"""
    until = time.time() + BOOT_WAIT_S
    while time.time() < until:
        try:
            st.read()
            return True
        except (OSError, SystemExit):
            time.sleep(3)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pad", type=int, default=0, choices=range(4))
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--no-reboot", action="store_true")
    a = ap.parse_args()

    x, addr = st.connect()
    g = st.read_block(x, addr)
    gate0 = g["enable"]
    print(f"INFO XWST gate {'on' if gate0 else 'off'} at the start", flush=True)
    if read_only():
        st.print_status(g)
        try:
            st.print_file(Xbdm().getfile(st.CONSOLE_FILE))
        except SystemExit as e:
            print(f"INFO {e}")
        c.check("read-only: XWST readable", g["magic"] == st.XS_MAGIC)
        c.done()

    g = gate_on(x, addr)
    c.check("gate on: file loaded and the active profile restored",
            g["state"] == 2 and g["restores"] > 0, st.STATES[g["state"]])
    xuid = g["xuid"]
    print(f"INFO active profile {xuid:016x}" + (" (nobody signed in)" if not xuid else ""),
          flush=True)

    con = wp.Console(a.pad)
    con.set_pn("inject_pad", a.pad)
    con.set_pn("notify", 0)
    b0, v0, g0 = three(con)
    print(f"INFO start: balance {b0}, volume {v0}, gate {g0}", flush=True)
    saves0, flow0 = g["saves"], flow(con)
    try:
        # 1. three changes, then the save
        b1, v1, g1 = change_three(con, b0, v0, g0)
        c.check("three settings changed on the panel", b1 != b0 and v1 != v0 and g1 != g0,
                f"balance {b0}->{b1}, volume {v0}->{v1}, gate {g0}->{g1}")
        time.sleep(SAVE_WAIT_S)
        g = st.read_block(x, addr)
        c.check("saved once, 3 s after the last change", g["saves"] == saves0 + 1,
                f"saves +{g['saves'] - saves0}")
        print(f"INFO save took {g['save_ms_last']} ms (max {g['save_ms_max']} ms); written in "
              f"place {g['inplace']}x", flush=True)
        c.check("the save took under 500 ms", g["save_ms_last"] < 500, f"{g['save_ms_last']} ms")
        streaming = flow(con) != flow0
        c.check("the earpiece did not starve during the save",
                g["save_starved"] == 0 and g["save_missed"] == 0,
                f"starved {g['save_starved']}, missed {g['save_missed']}"
                + ("" if streaming else " (no headset streaming: trivially)"))
        c.check("no settings errors", g["errors"] == 0,
                f"{g['errors']}, last {st.ERRORS.get(st.s32(g['last_error']), '?')}")

        # 2. the file on the disk
        hdr, r, problems = file_record(xuid)
        c.check("file header and every record's CRC good", hdr is not None and not problems,
                "; ".join(problems))
        c.check("the profile's record holds the three new values",
                r is not None and (r["balance"], r["volume"] * 10, r["gate"]) == (b1, v1, g1),
                st.describe(r) if r else "no record for this profile")

        if a.no_reboot:
            print("INFO --no-reboot: the restore after a reboot is not checked", flush=True)
        else:
            # 3. cold reboot, all three back
            print("INFO magicboot cold:", Xbdm().cmd("magicboot cold")[0], flush=True)
            time.sleep(10)
            back = wait_console()
            if not c.check("console back after the cold reboot", back,
                           "" if back else f"no xbdm / XWST within {BOOT_WAIT_S} s: if it is "
                           "off, switch it on and run this again with --no-reboot"):
                c.done()
            x, addr = st.connect()
            g = gate_on(x, addr)
            if xuid and a.hands:
                print("INFO switch the pad on (its Xbox button) and sign in to the same profile",
                      flush=True)
                g = st.wait_for(x, addr, lambda g: g["xuid"] == xuid, HANDS_WAIT_S)
            c.check("after the reboot: the same profile is active", g["xuid"] == xuid,
                    f"{g['xuid']:016x}, want {xuid:016x}"
                    + ("" if a.hands or not xuid else " (signed in before: use --hands)"))
            con = wp.Console(a.pad)
            after = three(con)
            c.check("after the reboot: balance, volume and gate are back", after == (b1, v1, g1),
                    f"{after}, want {(b1, v1, g1)}")
    finally:
        # 4. the starting values back (as wp1_3.py restore does), saved, the gate as it was
        con = wp.Console(a.pad)
        con.inject(0)
        con.set_ga("balance", b0)
        con.set_ga("master", v0)
        con.set_dg("gate", g0)
        time.sleep(SAVE_WAIT_S)
        c.check("starting values put back", three(con) == (b0, v0, g0), str(three(con)))
        x, addr = st.connect()
        if not gate0:
            st.setctl(x, addr, "enable", 0)

    print("OWNER CHECK: change VOLUME and BALANCE on the panel, restart the console, and check "
          "they are where you left them; then sign in as a second profile on the same pad and "
          "check it starts from the normal settings and keeps its own.", flush=True)
    c.done()


if __name__ == "__main__":
    main()
