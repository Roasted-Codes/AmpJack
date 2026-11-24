#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for WP4.0, the team voice network spike (docs/PLAN.md 4.3, docs/TEAM.md).

The Mac is the second peer: this script runs one in-process (UDP 3660 on this machine, HELLO
once a second on channel 1) while the console's xw.xex (with team_360.c) runs stage 2.

  XBOX=10.0.0.13 python3 xbox360/accept/p4_net.py [--seconds 60] [--switches 2] [--spike]
                                                  [--dest A.B.C.D] [--keep]

  --seconds N   HELLOs both ways for at least N s (default 60)
  --switches K  ... and until the console has seen K title switches (default 2: Aurora ->
                Halo 3 -> Aurora). The coordinator switches titles while it runs; it prompts
                every 15 s and gives up 4 minutes after --seconds. 0 = no title switches.
  --spike       first the spike matrix: the startup calls (XNet + WSA, WSA only, none), the
                insecure option on and off, and broadcast / subnet broadcast / unicast to this
                Mac, a few seconds each, printed as a table (INFO lines, not judged)
  --dest        where the console sends during the main run (default 255.255.255.255)
  --keep        leave stage 2 on at the end (default: back to what it was)

Checks (PASS / FAIL lines, exit 1 if any failed):
  - the XWTV block is there and its exports passed the 17559 check;
  - the socket opens (stage 2) with no startup or open error;
  - over the run: the Mac hears the console's HELLOs (>= 90 % of the seconds, no gap over
    3 s) and the console hears the Mac's (its peer entry, >= 90 %);
  - zero send and receive errors, no reopen, no second open;
  - the title switches asked for happened while it ran.
Answers printed as INFO: XNetStartup / WSAStartup results, the cost of a sendto, whether the
console's stack loops its own broadcasts back (rx_own), its address and id.
OWNER CHECK lines: Halo 3 system link still works with the socket open.
"""
import argparse
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import team  # noqa: E402
from accept import Checks, read_only  # noqa: E402


def info(text):
    print(f"INFO {text}", flush=True)


def mac_ip(console_ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((console_ip, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def wait_open(x, addr, opens_before, secs=10):
    end = time.time() + secs
    g = team.read_block(x, addr)
    while time.time() < end:
        g = team.read_block(x, addr)
        if g["opens"] > opens_before and g["net_state"] != 0:
            break
        time.sleep(0.25)
    time.sleep(0.5)
    return team.read_block(x, addr)


def spike(x, addr, console_ip):
    """the matrix; leaves the defaults set"""
    me = mac_ip(console_ip)
    subnet = ".".join(console_ip.split(".")[:3] + ["255"])
    rows = []
    variants = [
        ("XNet+WSA, insecure, broadcast", 3, 7, "0"),
        ("WSA only", 2, 7, "0"),
        ("no startup", 0, 7, "0"),
        ("subnet broadcast " + subnet, 3, 7, subnet),
        ("unicast to this Mac " + me, 3, 7, me),
        ("NOT insecure (no 0x5801)", 3, 6, "0"),
    ]
    bg = team.Background(channel=1, name="SPIKE").start()
    try:
        for name, startup, opts, dest in variants:
            g0 = team.read_block(x, addr)
            team.setctl(x, addr, "dest", team.ip_int(dest))
            team.setctl(x, addr, "startup", startup)
            team.setctl(x, addr, "opts", opts)
            if g0["startup"] == startup and g0["opts"] == opts:
                pass  # no reopen needed
            g1 = wait_open(x, addr, g0["opens"] if (g0["startup"], g0["opts"]) != (startup, opts)
                           else g0["opens"] - 1)
            with bg.lock:
                mine0 = sum(len(v) for k, v in bg.hellos.items() if k == g1["sender"])
            time.sleep(5)
            g2 = team.read_block(x, addr)
            with bg.lock:
                mine1 = sum(len(v) for k, v in bg.hellos.items() if k == g2["sender"])
            peer = next((p for p in g2["peers"] if p["id"] == bg.peer.sender), None)
            ph = [p for p in g1["peers"] if p["id"] == bg.peer.sender]
            heard = (peer["rx_hello"] if peer else 0) - (ph[0]["rx_hello"] if ph else 0)
            avg = ((g2["tx_us_sum"] - g1["tx_us_sum"]) / (g2["tx_calls"] - g1["tx_calls"])
                   if g2["tx_calls"] > g1["tx_calls"] else 0)
            rows.append((name, g2["net_state"], g2["err_xnet"], g2["err_wsa"], g2["err_open"],
                         team.STEPS.get(g2["open_step"], g2["open_step"]), mine1 - mine0, heard,
                         g2["send_fails"] - g1["send_fails"], g2["err_send"], avg,
                         g2["rx_own"] - g1["rx_own"]))
    finally:
        bg.stop()
        team.setctl(x, addr, "dest", 0)
        team.setctl(x, addr, "startup", 3)
        team.setctl(x, addr, "opts", 7)
    info("spike matrix (5 s each): state, XNetStartup, WSAStartup, open error @ step, console "
         "HELLOs heard here, our HELLOs heard there, send failures (last error), sendto us, "
         "own packets looped back")
    for r in rows:
        info(f"  {r[0]:36s} {team.NET_STATES.get(r[1], r[1]):8s} xnet {r[2]} wsa {r[3]} "
             f"open {r[4]} @ {r[5]}; here {r[6]}, there {r[7]}; send fails {r[8]} ({r[9]}); "
             f"{r[10]:.0f} us; own {r[11]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--switches", type=int, default=2)
    ap.add_argument("--spike", action="store_true")
    ap.add_argument("--dest", default="0")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    c = Checks()
    try:
        x, addr = team.connect()
        g = team.read_block(x, addr)
    except SystemExit as e:
        c.check("XWTV block", False, str(e))
        c.done()
    c.check("XWTV block", True, f"at {addr:08x}, version {g['version']}")
    if read_only():
        info("ACCEPT_READ_ONLY=1: nothing written; status only")
        team.print_status(g)
        c.done()
    stage0, channel0 = g["stage"], g["channel"]
    team.setctl(x, addr, "channel", 1)
    team.setctl(x, addr, "dest", team.ip_int(a.dest))
    team.setctl(x, addr, "stage", 2)
    g = wait_open(x, addr, g["opens"] if g["net_state"] != 1 else g["opens"] - 1)
    c.check("network exports are the 17559 code", g["init_error"] == 0,
            team.INIT_ERRORS.get(g["init_error"], str(g["init_error"])) +
            f" ({g['init_detail']:08x})")
    ok = c.check("socket open at stage 2", g["net_state"] == 1,
                 f"{team.NET_STATES.get(g['net_state'])}; XNetStartup {g['err_xnet']}, "
                 f"WSAStartup {g['err_wsa']}, open error {g['err_open']} at "
                 f"{team.STEPS.get(g['open_step'], g['open_step'])}")
    info(f"console {team.ip_str(g['ip'])}, id {g['sender']:08x}, xnaddr flags "
         f"{g['xnaddr_flags']:x}, link {g['link']:x}, net thread on hw thread {g['hw_thread']}")
    if not ok:
        team.print_status(g)
        c.done()
    if a.spike:
        spike(x, addr, team.ip_str(g["ip"]))
        team.setctl(x, addr, "dest", team.ip_int(a.dest))
        g = wait_open(x, addr, g["opens"], 3)

    # the main run
    for name in team.COUNTERS:
        team.setctl(x, addr, name, 0)
    g0 = team.read_block(x, addr)
    bg = team.Background(channel=1, name="ACCEPT").start()
    t0 = time.time()
    last_prompt = 0
    until_max = t0 + a.seconds + 240
    try:
        while True:
            time.sleep(2)
            g = team.read_block(x, addr)
            switched = g["title_switches"] - g0["title_switches"]
            if time.time() - t0 >= a.seconds and switched >= a.switches:
                break
            if time.time() >= until_max:
                break
            if switched < a.switches and time.time() - last_prompt >= 15:
                print(f"DO   coordinator: switch titles ({switched} of {a.switches} so far: "
                      f"Aurora -> Halo 3 -> Aurora; a system-link lobby counts as a bonus)",
                      flush=True)
                last_prompt = time.time()
    finally:
        bg.stop()
    secs = time.time() - t0
    g = team.read_block(x, addr)
    with bg.lock:
        times = bg.hellos.get(g["sender"], [])
    gaps = [b - a_ for a_, b in zip(times, times[1:])]
    c.check("Mac hears the console's HELLOs", len(times) >= 0.9 * secs - 2,
            f"{len(times)} in {secs:.0f} s, longest gap {max(gaps, default=0):.1f} s")
    c.check("no HELLO gap over 3 s at the Mac", bool(times) and max(gaps, default=0) <= 3.0,
            f"longest {max(gaps, default=0):.1f} s")
    peer = next((p for p in g["peers"] if p["id"] == bg.peer.sender), None)
    c.check("console hears the Mac's HELLOs", bool(peer) and peer["rx_hello"] >= 0.9 * secs - 2,
            f"{peer['rx_hello'] if peer else 0} of {bg.sent_hello} sent")
    errs = {k: g[k] for k in ("send_fails", "recv_fails", "reopens", "rx_bad")}
    c.check("zero socket errors", not any(errs.values()) and g["net_state"] == 1,
            " ".join(f"{k} {v}" for k, v in errs.items()) +
            f"; last errors send {g['err_send']} receive {g['err_recv']}")
    c.check("one socket over the run (no reopen)", g["opens"] == g0["opens"],
            f"opens {g0['opens']} -> {g['opens']}")
    switched = g["title_switches"] - g0["title_switches"]
    c.check(f"{a.switches} title switches survived", switched >= a.switches,
            f"{switched} title switches while the socket stayed open")
    avg = g["tx_us_sum"] / g["tx_calls"] if g["tx_calls"] else 0
    info(f"sendto: {avg:.0f} us average, {g['tx_us_max']} us max over {g['tx_calls']} calls; "
         f"recvfrom max {g['rx_us_max']} us")
    info(f"own broadcasts looped back to the console: {g['rx_own']} (of {g['tx_hello']} sent)")
    info(f"peers known {g['peers_known']}, consoles on channel 1: {g['team_peers']}")
    print("OWNER CHECK with stage 2 on, start a Halo 3 system-link lobby on this console: it "
          "behaves as always (the lobby opens, a second console or XLink peer sees it)",
          flush=True)
    if not a.keep:
        team.setctl(x, addr, "stage", stage0)
        team.setctl(x, addr, "channel", channel0)
        info(f"stage back to {stage0}, channel {channel0}")
    c.done()


if __name__ == "__main__":
    main()
