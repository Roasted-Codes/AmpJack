#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for the PC bridge (bridge_360.c), with this Mac as the PC: it runs
tools/bridge/xbox-bridge.py's test mode (tones to the console, its streams to WAV files) and
reads the XWBR block over xbdm. Run:

  XBOX=10.0.0.13 python3 xbox360/accept/bridge.py [--secs 12] [--dir DIR]

The pad on with the jack headset streaming (press the controller's Xbox button) for the mixing
checks (B4-B6); without it they print SKIP lines and the rest still runs. Nothing to hear:
the tones are -20 dBFS (440 Hz in the headset, 660 Hz to the Xbox chat) for --secs seconds.
ACCEPT_READ_ONLY=1: only B1. Everything written is put back at the end.

Checks:
  B1 the build has the XWBR block, on, listening (or connected), its net thread running;
  B2 a hello is answered within 1 s; the block shows this Mac connected, a new session;
  B3 streams 3 and 4: the console counted what was sent (at least 98 %), none lost, no bad
     datagrams;
  B4 (headset streaming) stream 3 mixed: running at the buffer target (within 15 ms), no
     underrun after the first 2 s, the trim within 1000 ppm, the tone's peak at -20 dBFS;
  B5 (headset and the 360's mic path) stream 4 the same;
  B6 (headset streaming) streams 1 and 2 reach the Mac: at least 90 % of the samples, under
     1 % lost, none skipped on the console;
  B7 the added work: at most 300 us a pass, 150 us per earpiece call, 100 us per mic call;
     the net thread's pass at most 3 ms;
  B8 a bye: listening again within 1.5 s, no PC;
  B9 a PC that goes quiet without a bye: dropped after 3 s (within 3..5 s);
  B10 bridge.py off: the socket closes, a hello gets no answer; on: answered again.
"""
import argparse
import importlib.util
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XBOX360 = os.path.dirname(HERE)
ROOT = os.path.dirname(XBOX360)
sys.path.insert(0, XBOX360)

from accept import Checks, read_only  # noqa: E402

PASS_US_MAX = 300
EAR_US_MAX = 150
MIC_US_MAX = 100
NET_US_MAX = 3000
TONE_PEAK = 3277  # -20 dBFS, the helper's tones


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def my_ip(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 3670))
        return s.getsockname()[0]
    finally:
        s.close()


def wait_for(fn, secs, step=0.1):
    end = time.time() + secs
    while True:
        v = fn()
        if v or time.time() >= end:
            return v
        time.sleep(step)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=float, default=12)
    ap.add_argument("--dir")
    a = ap.parse_args()
    host = os.environ.get("XBOX", "").partition(":")[0]
    if not host:
        raise SystemExit("set XBOX=10.0.0.13")
    br = load("bridge_tool", os.path.join(XBOX360, "bridge.py"))
    pc = load("xbox_bridge", os.path.join(ROOT, "tools", "bridge", "xbox-bridge.py"))
    c = Checks()
    x, addr, d = br.connect()
    hb = d["heartbeat"]
    time.sleep(0.5)
    d = br.read(x, addr)
    c.check("B1 block, on, listening, net thread running",
            d["version"] >= 1 and d["enable"] == 1 and d["state"] in (2, 3) and d["heartbeat"] != hb,
            f"version {d['version']}, enable {d['enable']}, state "
            f"{br.name_of(br.STATES, d['state'])}, heartbeat {hb} -> {d['heartbeat']}")
    if read_only():
        c.done()
    before = d
    mine = my_ip(host)

    # B2-B7: a test session
    seen = {}

    class Args:
        pass

    args = Args()
    args.host, args.port, args.secs, args.tone, args.dir = host, pc.PORT, a.secs, "34", a.dir
    lines = []

    def log(s):
        lines.append(s)
        print("   ", s, flush=True)

    # the block 2 s into the session (after the buffers started), read while the test runs
    import threading

    def mid():
        time.sleep(2.5)
        xm, _, _ = br.connect()
        seen["mid"] = br.read(xm, addr)

    t = threading.Thread(target=mid, daemon=True)
    t.start()
    res = pc.run_test(args, log=log)
    t.join(5)
    after = br.read(x, addr)
    c.check("B2 hello answered within 1 s", res["connected"] and res["connect_s"] < 1.0,
            f"{res.get('connect_s', 0) * 1000:.0f} ms")
    m = seen.get("mid") or after
    c.check("B2 the console shows this Mac connected, a new session",
            m["state"] == 3 and br.ip_str(m["peer_ip"]) == mine and m["sessions"] > before["sessions"],
            f"state {br.name_of(br.STATES, m['state'])}, peer {br.ip_str(m['peer_ip'])} (this Mac "
            f"{mine}), sessions {before['sessions']} -> {m['sessions']}")
    if not res["connected"]:
        c.done()
    first = res["first"]
    for k in (3, 4):
        got = after["s"][k]["packets"] - before["s"][k]["packets"]
        lost = after["s"][k]["lost"] - before["s"][k]["lost"]
        sent = res["sent"][k]
        c.check(f"B3 stream {k} received", sent > 0 and got >= sent * 0.98 and lost == 0,
                f"sent {sent}, counted {got}, lost {lost}")
    c.check("B3 no bad datagrams", after["rx_bad"] == before["rx_bad"],
            f"{after['rx_bad'] - before['rx_bad']}")
    headset = bool(first.get("headset")) and after["ear_calls"] > before["ear_calls"]
    mic = after["mic_calls"] > before["mic_calls"]
    target = after["jb_ms"]
    for k, live, what in ((3, headset, "the headset streaming"), (4, mic, "the 360 taking the mic")):
        name = "B4" if k == 3 else "B5"
        if not live:
            print(f"SKIP {name} stream {k} mixed: needs {what} (press the controller's Xbox "
                  f"button, plug in the headset)")
            continue
        s, sm = after["s"][k], m["s"][k]
        fill = res["during"].get(f"s{k}_fill_ms", 0)  # the console's answer while it played
        c.check(f"{name} stream {k} at the buffer target", abs(fill - target) <= 15,
                f"fill {fill} ms, target {target}")
        c.check(f"{name} stream {k} no underrun after 2 s", s["underruns"] == sm["underruns"],
                f"{sm['underruns']} -> {s['underruns']}; restarts {before['s'][k]['restarts']} -> "
                f"{s['restarts']}")
        c.check(f"{name} stream {k} trim", abs(s["trim_ppm"]) <= 1000, f"{s['trim_ppm']:+d} ppm")
        c.check(f"{name} stream {k} tone level", abs(sm["peak"] - TONE_PEAK) <= TONE_PEAK // 10,
                f"peak {sm['peak']} (want {TONE_PEAK})")
    if headset:
        for k in (1, 2):
            i = res["ins"][k]
            rate = i["rate"] or 1
            frac = i["frames"] / (rate * a.secs)
            skipped = after["s"][k]["lost"] - before["s"][k]["lost"]
            c.check(f"B6 stream {k} reaches the Mac",
                    frac >= 0.9 and i["lost"] <= max(1, i["packets"] // 100) and skipped == 0,
                    f"{i['packets']} packets, {frac * 100:.0f} % of {a.secs:.0f} s at {i['rate']} Hz, "
                    f"lost {i['lost']}, skipped on the console {skipped}; {res['wav'][k][0]}")
    else:
        print("SKIP B6 streams 1 and 2 reach the Mac: needs the headset streaming")
    c.check("B7 added work per pass", after["pass_us_max"] <= PASS_US_MAX,
            f"max {after['pass_us_max']} us, average {after['pass_us_avg']} us")
    c.check("B7 per call", after["ear_us_max"] <= EAR_US_MAX and after["mic_us_max"] <= MIC_US_MAX,
            f"earpiece {after['ear_us_max']} us, mic {after['mic_us_max']} us")
    c.check("B7 net thread pass", after["net_us_max"] <= NET_US_MAX, f"{after['net_us_max']} us")

    # B8: the bye was sent by run_test's close
    d = wait_for(lambda: (lambda v: v if v["state"] == 2 else None)(br.read(x, addr)), 1.5)
    d = d or br.read(x, addr)
    c.check("B8 a bye ends the session", d["state"] == 2 and d["peer_ip"] == 0,
            f"state {br.name_of(br.STATES, d['state'])}")

    # B9: quiet without a bye
    link = pc.Link(host, pc.PORT, log=lambda s: None)
    ok = wait_for(link.connected, 2)
    link.running = False  # no more hellos, no bye
    t0 = time.time()
    d = wait_for(lambda: (lambda v: v if v["state"] == 2 else None)(br.read(x, addr)), 6, 0.2)
    dt = time.time() - t0
    link.s.close()
    c.check("B9 a quiet PC is dropped after 3 s", ok and d is not None and 2.0 <= dt <= 5.0,
            f"dropped {dt:.1f} s after the last hello")

    # B10: off / on
    enable = after["enable"]
    try:
        br.write(x, addr, "enable", 0)
        d = wait_for(lambda: (lambda v: v if v["state"] == 0 else None)(br.read(x, addr)), 1.0)
        link = pc.Link(host, pc.PORT, log=lambda s: None)
        answered = wait_for(link.connected, 2.0)
        link.close()
        c.check("B10 off: socket closed, no answer", d is not None and not answered,
                f"state {br.name_of(br.STATES, br.read(x, addr)['state'])}, answered {bool(answered)}")
        br.write(x, addr, "enable", 1)
        wait_for(lambda: br.read(x, addr)["state"] == 2, 1.0)
        link = pc.Link(host, pc.PORT, log=lambda s: None)
        answered = wait_for(link.connected, 2.0)
        link.close()
        c.check("B10 on: answered again", bool(answered),
                f"state {br.name_of(br.STATES, br.read(x, addr)['state'])}")
    finally:
        br.write(x, addr, "enable", enable)
    c.done()


if __name__ == "__main__":
    main()
