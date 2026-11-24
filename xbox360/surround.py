#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Headphone surround (surround_360.c) over xbdm: the XWSR block. Run next to out/.

  surround.py status            gate, setting, state, live, channel peaks, clamps, memory, tap time
  surround.py gate on|off       the feature's gate (off at boot): off = the tap is today's code
  surround.py on|off            SURROUND itself (what the panel row sets); needs the gate on
  surround.py watch [SECONDS]   status once a second
  surround.py demo [status]     the demo (SURROUND DEMO): state, the file, position, underruns,
                                reads, memory, why the last play ended
  surround.py demo start        play Hdd:\\Plugins\\xw.demo (40 s) in place of the game's sound;
                                turns the gate on first if it is off. Needs the headset
                                streaming (any title, or the dashboard). Flip SURROUND with
                                surround.py on|off while it plays.
  surround.py demo stop         stop it (it also ends by itself)
  The file: python3 tools/demo/make_demo.py, then xw.py install (or the run skill's keep).

XW_DEV builds only (the measurement probe, docs/ROADMAP.md 6.1):
  surround.py load CONVS TAPS [SITE]   dummy load: CONVS (0..10) FIRs of TAPS (0..128) per
                                block, SITE 0 the tap (default), 1 the driver thread; load 0 0 off
  surround.py zero              reset tap_max_us, the histogram, the tap period, the peak maxima
  surround.py testch N          noise bursts into channel N only (1 FL, 2 FR, 3 C, 4 LFE, 5 SL,
                                6 SR; 0 off): the game is silenced meanwhile
  surround.py walk [SECS]       the channel walk: FL, C, FR, SR, SL, SECS (default 2) each,
                                with the gate on; puts everything back after

XBOX=host[:port] is the console's address (required).
"""
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from xa import Xbdm, xw_block  # noqa: E402

MAGIC = 0x58575352  # "XWSR", struct xsv_status in surround_360.c
CHANNELS = ["FL", "FR", "C", "LFE", "SL", "SR"]
FIELDS = ("magic version enable surround state live build "
          + " ".join(f"ch_peak{i}" for i in range(6))
          + " clamps heap_bytes tap_us tap_max_us blocks switches").split()
assert len(FIELDS) == 19  # XSV_RELEASE_WORDS, _Static_assert in surround_360.c
DEV_FIELDS = ("load_convs load_taps load_site test_ch zero "
              + " ".join(f"tap_hist{i}" for i in range(6)) + " tap_period_max_us "
              + " ".join(f"ch_peak_max{i}" for i in range(6)) + " load_blocks sink").split()
assert len(DEV_FIELDS) == 20
# version 2: the demo's words, after DEV_FIELDS in an XW_DEV build, after FIELDS otherwise
DEMO_FIELDS = ("demo_cmd demo_state demo_file demo_end demo_plays demo_blocks demo_pos demo_secs "
               "demo_underruns demo_reads demo_bad_pages demo_read_max_us demo_fill_min "
               "demo_heap").split()
assert len(DEMO_FIELDS) == 14  # XSV_DEMO_WORDS
DEMO_STATES = ["idle", "loading", "playing", "stopping"]
DEMO_FILES = ["not looked for yet (the gate is off)", "present", "missing", "not a version 1 xw.demo",
              "unreadable"]
DEMO_ENDS = ["-", "played to the end", "stopped", "title changed", "surround's gate off",
             "no game audio (headset not streaming)", "file failed", "no memory", "quiesced"]
DEMO_BLOCKS_PER_S = 187.5  # 48000 / 256
DEMO_RING_BLOCKS = 56      # surround_360.c DM_SLOTS x XDM_PAGE_BLOCKS
BUILD_DEV = 0x100
STATES = ["gate off", "off", "on", "no memory", "bad table"]
TAP_HIST = ["<50", "50-100", "100-200", "200-400", "400-800", ">=800"]  # us
WALK = [1, 3, 2, 6, 5]  # test_ch: FL, C, FR, SR, SL (the demo's order)
LIVE_PEAK = 104  # -50 dBFS, XSV_LIVE_PEAK


def dbfs(peak):
    return "-inf" if not peak else f"{20 * math.log10(peak / 32768):+.1f}"


def block_addr():
    """the XWSR block's address in the running build (SystemExit if it has none)"""
    return xw_block(MAGIC)


def read(x, addr):
    """the block as a dict (dev fields too on an XW_DEV build), or None"""
    w = x.u32s(addr, len(FIELDS))
    if not w or w[0] != MAGIC:
        return None
    d = dict(zip(FIELDS, w))
    d["ch_peak"] = [d[f"ch_peak{i}"] for i in range(6)]
    d["taps"] = d["build"] & 0xFF
    d["dev"] = bool(d["build"] & BUILD_DEV)
    if d["dev"]:
        v = x.u32s(addr + 4 * len(FIELDS), len(DEV_FIELDS))
        if v:
            d.update(zip(DEV_FIELDS, v))
            d["tap_hist"] = [d[f"tap_hist{i}"] for i in range(6)]
            d["ch_peak_max"] = [d[f"ch_peak_max{i}"] for i in range(6)]
    if d["version"] >= 2:
        v = x.u32s(addr + offset(DEMO_FIELDS[0], d["dev"]), len(DEMO_FIELDS))
        if v:
            d.update(zip(DEMO_FIELDS, v))
    return d


def need_dev(d):
    if not d.get("dev"):
        raise SystemExit("this needs an XW_DEV build (XW_DEV=1 bash toolchain/build.sh): the "
                         "running xw.xex is a release build, which has no measurement probe")


def offset(name, dev=False):
    if name in FIELDS:
        return 4 * FIELDS.index(name)
    if name in DEMO_FIELDS:
        return 4 * (len(FIELDS) + (len(DEV_FIELDS) if dev else 0) + DEMO_FIELDS.index(name))
    return 4 * (len(FIELDS) + DEV_FIELDS.index(name))


def write(x, addr, name, value, dev=False):
    """dev: the running build is XW_DEV (the demo's words sit after the probe's)"""
    return x.setu32(addr + offset(name, dev), value & 0xFFFFFFFF)


def connect():
    """(Xbdm, address, block) or SystemExit"""
    addr = block_addr()
    x = Xbdm()
    d = read(x, addr)
    if d is None:
        raise SystemExit(f"no XWSR block at {addr:08x} (xw.xex loaded? a build with surround?)")
    return x, addr, d


def show(d):
    print(f"surround: gate {'on' if d['enable'] else 'off'}, setting "
          f"{'ON' if d['surround'] else 'OFF'}, state {STATES[d['state']] if d['state'] < 5 else d['state']}, "
          f"{d['taps']} taps, {'XW_DEV' if d['dev'] else 'release'} build, memory {d['heap_bytes']} bytes")
    print("  channel peaks (last second, dBFS): " +
          ", ".join(f"{c} {dbfs(p)}" for c, p in zip(CHANNELS, d["ch_peak"])) +
          f"; 5.1 live: {'yes' if d['live'] else 'no'}")
    print(f"  tap: {d['tap_us']} us a block now, {d['tap_max_us']} us max (budget 300 us p99); "
          f"blocks virtualized {d['blocks']}, switches {d['switches']}, clamped samples {d['clamps']}")
    if d["dev"] and "tap_hist" in d:
        print("  probe: load %u x %u taps at %s; test_ch %u; tap period max %u us; dummy blocks %u" % (
            d["load_convs"], d["load_taps"], "the driver thread" if d["load_site"] else "the tap",
            d["test_ch"], d["tap_period_max_us"], d["load_blocks"]))
        print("  tap time histogram: " + ", ".join(f"{k} {v}" for k, v in zip(TAP_HIST, d["tap_hist"])))
        print("  peak maxima: " + ", ".join(f"{c} {dbfs(p)}" for c, p in zip(CHANNELS, d["ch_peak_max"])))
    if "demo_state" in d:
        show_demo(d)


def name_of(names, v):
    return names[v] if v < len(names) else str(v)


def show_demo(d):
    total = d["demo_blocks"] / DEMO_BLOCKS_PER_S
    print(f"  demo: {name_of(DEMO_STATES, d['demo_state'])}, {d['demo_secs']} of {total:.0f} s "
          f"(block {d['demo_pos']} of {d['demo_blocks']}); file {name_of(DEMO_FILES, d['demo_file'])}; "
          f"plays {d['demo_plays']}, last ended: {name_of(DEMO_ENDS, d['demo_end'])}")
    print(f"        underruns {d['demo_underruns']} (silent blocks), bad pages {d['demo_bad_pages']}, "
          f"reads {d['demo_reads']} (longest {d['demo_read_max_us']} us), fewest blocks ahead "
          f"{d['demo_fill_min']} of {DEMO_RING_BLOCKS} ({d['demo_fill_min'] * 1000 / DEMO_BLOCKS_PER_S:.0f} ms), "
          f"memory {d['demo_heap']} bytes")


def demo(x, addr, d, args, log=print):
    """surround.py demo [status|start|stop]; returns the block after"""
    if d["version"] < 2:
        raise SystemExit(f"the running build's XWSR block is version {d['version']}: no demo")
    cmd = args[0] if args else "status"
    if cmd == "start":
        if not d["enable"]:
            log("the gate is off: switching it on (surround.py gate off puts it back)")
            write(x, addr, "enable", 1)
        for _ in range(30):  # the file is looked for on the gate's first pass
            d = read(x, addr)
            if d["demo_file"] != 0:
                break
            time.sleep(0.05)
        if d["demo_file"] != 1:
            raise SystemExit(f"xw.demo: {name_of(DEMO_FILES, d['demo_file'])}. Make it with "
                             f"python3 tools/demo/make_demo.py, then xw.py install --no-reboot")
        write(x, addr, "demo_cmd", 1, d["dev"])
        for _ in range(40):
            time.sleep(0.05)
            d = read(x, addr)
            if d["demo_state"] in (0, 2):
                break
        log(f"demo: {name_of(DEMO_STATES, d['demo_state'])}"
            + (f" (last end: {name_of(DEMO_ENDS, d['demo_end'])})" if d["demo_state"] == 0 else "")
            + f"; SURROUND is {'ON' if d['surround'] else 'OFF'} (surround.py on|off flips it)")
    elif cmd == "stop":
        write(x, addr, "demo_cmd", 2, d["dev"])
        time.sleep(0.2)
        d = read(x, addr)
        log(f"demo: {name_of(DEMO_STATES, d['demo_state'])}")
    elif cmd != "status":
        raise SystemExit(__doc__)
    show_demo(d)
    return d


def walk(secs=2.0, log=print):
    """the channel walk (XW_DEV); returns {channel: peaks seen} and puts things back"""
    x, addr, d = connect()
    need_dev(d)
    gate, ch = d["enable"], d["test_ch"]
    seen = {}
    try:
        write(x, addr, "enable", 1)
        for n in WALK:
            write(x, addr, "test_ch", n)
            log(f"walk: {CHANNELS[n - 1]}")
            time.sleep(secs)
            seen[n] = read(x, addr)["ch_peak"]
    finally:
        write(x, addr, "test_ch", ch)
        write(x, addr, "enable", gate)
    return seen


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return
    cmd, args = argv[0], argv[1:]
    x, addr, d = connect()
    if cmd == "status":
        show(d)
    elif cmd == "gate" and args[:1] in (["on"], ["off"]):
        print("enable =", int(args[0] == "on"), write(x, addr, "enable", int(args[0] == "on")))
    elif cmd in ("on", "off"):
        if not d["enable"] and cmd == "on":
            print("note: the gate is off, so the setting is kept but not heard (surround.py gate on)")
        print("surround =", int(cmd == "on"), write(x, addr, "surround", int(cmd == "on")))
    elif cmd == "demo":
        demo(x, addr, d, args)
    elif cmd == "watch":
        end = time.time() + float(args[0]) if args else None
        while end is None or time.time() < end:
            show(read(x, addr))
            time.sleep(1)
    elif cmd == "load" and len(args) >= 2:
        need_dev(d)
        convs, taps, site = int(args[0]), int(args[1]), int(args[2]) if len(args) > 2 else 0
        if not (0 <= convs <= 10 and 0 <= taps <= 128 and site in (0, 1)):
            raise SystemExit("load CONVS (0..10) TAPS (0..128) [SITE 0|1]")
        for k, v in (("load_site", site), ("load_taps", taps), ("load_convs", convs)):
            write(x, addr, k, v)
        print(f"dummy load {convs} x {taps} taps at {'the driver thread' if site else 'the tap'}")
    elif cmd == "zero":
        need_dev(d)
        print("zero", write(x, addr, "zero", 1))
    elif cmd == "testch" and args:
        need_dev(d)
        n = int(args[0])
        if not 0 <= n <= 6:
            raise SystemExit("testch 0..6")
        print("test_ch =", n, write(x, addr, "test_ch", n))
    elif cmd == "walk":
        for n, peaks in walk(float(args[0]) if args else 2.0).items():
            print(f"  {CHANNELS[n - 1]}: " + ", ".join(f"{c} {dbfs(p)}" for c, p in zip(CHANNELS, peaks)))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
