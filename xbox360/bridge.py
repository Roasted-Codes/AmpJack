#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The PC bridge (bridge_360.c) over xbdm: the XWBR block. Run next to out/.

  bridge.py status              on/off, the PC connected, per-stream counters, the added time
  bridge.py on|off              the bridge's gate (on at boot): off closes its socket
  bridge.py headset PERCENT     the PC's sound in the headset, 0..100 (default 100)
  bridge.py chat PERCENT        the PC's voices to the Xbox players, 0..100 (default 100)
  bridge.py buffer MS           the jitter buffers' target, 20..150 ms (default 50)
  bridge.py zero                the maxima (timing) start over
  bridge.py watch [SECONDS]     status once a second

The PC side: tools/bridge/xbox-bridge.py (on Linux: python3 xbox-bridge.py 10.0.0.13; from
the Mac: --test). XBOX=host[:port] is the console's address (required).
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from xa import Xbdm, xw_block  # noqa: E402

MAGIC = 0x58574252  # struct xb_status in bridge_360.c
FIELDS = ("magic version enable level_headset level_chat jb_ms zero state port peer_ip "
          "peer_port peer_age_ms sessions hellos rx_bad rx_other net_err net_step opens "
          "heartbeat net_us_max mic_calls ear_calls mic_us_max ear_us_max pass_us pass_us_max "
          "pass_us_avg").split()
assert len(FIELDS) == 28
STREAM_FIELDS = "packets lost late underruns restarts fill_ms trim_ppm peak rate ch".split()
assert len(STREAM_FIELDS) == 10  # STREAM_WORDS
STATES = ["off", "starting (20 s after boot)", "listening (no PC)", "PC connected",
          "socket failed (retrying)", "refused (not this kernel's network code, or no memory)",
          "quiesced (hot reload)"]
STREAMS = {1: "headset mic -> PC", 2: "Xbox players -> PC", 3: "PC -> headset",
           4: "PC -> Xbox chat"}


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


def ip_str(v):
    return ".".join(str((v >> s) & 255) for s in (24, 16, 8, 0))


def read(x, addr):
    w = x.u32s(addr, len(FIELDS) + 4 * len(STREAM_FIELDS))
    if not w or w[0] != MAGIC:
        return None
    d = dict(zip(FIELDS, w))
    d["s"] = {}
    for k in range(4):
        v = w[len(FIELDS) + k * len(STREAM_FIELDS):len(FIELDS) + (k + 1) * len(STREAM_FIELDS)]
        s = dict(zip(STREAM_FIELDS, v))
        s["trim_ppm"] = signed(s["trim_ppm"])
        s["recovered"] = s["late"] >> 16
        s["late"] &= 0xFFFF
        d["s"][k + 1] = s
    return d


def write(x, addr, name, value):
    return x.setu32(addr + 4 * FIELDS.index(name), value & 0xFFFFFFFF)


def connect():
    """(Xbdm, address, block) or SystemExit"""
    addr = xw_block(MAGIC)
    x = Xbdm()
    d = read(x, addr)
    if d is None:
        raise SystemExit(f"no PC bridge block at {addr:08x} (xw.xex loaded? a build with the bridge?)")
    return x, addr, d


def name_of(names, v):
    return names[v] if v < len(names) else str(v)


def show(d, log=print):
    log(f"bridge: {'on' if d['enable'] else 'OFF'}, {name_of(STATES, d['state'])}, UDP {d['port']}"
        + (f", PC {ip_str(d['peer_ip'])}:{d['peer_port']} (hello {d['peer_age_ms']} ms ago)"
           if d["peer_ip"] else "")
        + f"; sessions {d['sessions']}, hellos {d['hellos']}, bad {d['rx_bad']}, other PCs "
          f"{d['rx_other']}")
    if d["net_err"] or d["state"] == 4:
        log(f"  last socket error {d['net_err']} (step {d['net_step']}), opens {d['opens']}")
    log(f"  levels: PC -> headset {d['level_headset']} %, PC -> chat {d['level_chat']} %; "
        f"buffer {d['jb_ms']} ms")
    for k, s in d["s"].items():
        fmt = f"{s['rate']} Hz x{s['ch']}" if s["rate"] else "-"
        if k <= 2:
            log(f"  {k} {STREAMS[k]:20s} {fmt:12s} packets {s['packets']}, skipped samples "
                f"{s['lost']}, queued {s['fill_ms']} ms, peak {s['peak']}")
        else:
            log(f"  {k} {STREAMS[k]:20s} {fmt:12s} packets {s['packets']}, lost {s['lost']}, late "
                f"{s['late']} (recovered {s['recovered']}), underruns {s['underruns']}, restarts "
                f"{s['restarts']}, fill {s['fill_ms']} ms, trim {s['trim_ppm']:+d} ppm, peak "
                f"{s['peak']}")
    log(f"  added work: {d['pass_us']} us this pass, {d['pass_us_avg']} us average, "
        f"{d['pass_us_max']} us max; per call max: mic {d['mic_us_max']} us, earpiece "
        f"{d['ear_us_max']} us; net thread pass max {d['net_us_max']} us; calls: mic "
        f"{d['mic_calls']}, earpiece {d['ear_calls']}; net heartbeat {d['heartbeat']}")


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return
    cmd, args = argv[0], argv[1:]
    x, addr, d = connect()
    if cmd == "status":
        show(d)
    elif cmd in ("on", "off"):
        print("enable =", int(cmd == "on"), write(x, addr, "enable", int(cmd == "on")))
    elif cmd in ("headset", "chat") and args:
        v = int(args[0])
        if not 0 <= v <= 100:
            raise SystemExit("a percentage, 0..100")
        print(f"level_{cmd} =", v, write(x, addr, f"level_{cmd}", v))
    elif cmd == "buffer" and args:
        v = int(args[0])
        if not 20 <= v <= 150:
            raise SystemExit("20..150 ms")
        print("jb_ms =", v, write(x, addr, "jb_ms", v))
    elif cmd == "zero":
        print("zero", write(x, addr, "zero", 1))
    elif cmd == "watch":
        end = time.time() + float(args[0]) if args else None
        while end is None or time.time() < end:
            show(read(x, addr))
            time.sleep(1)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
