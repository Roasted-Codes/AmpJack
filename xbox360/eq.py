#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The headset equalizer (eq_360.c): its XWEQ block over xbdm, and the preset file.

  eq.py status            the XWEQ block: gate, preset, CUSTOM's steps, load state, file
                          version, fade, clamps, memory, the EQ's time per driver-loop pass
  eq.py on | off          the gate (off at boot): off = the panel offers FLAT only and the
                          earpiece is untouched
  eq.py select N|NAME     select a preset (as the panel does): 0 or FLAT, an index, CUSTOM, or
                          a name or the start of one ("omni")
  eq.py custom B M T      CUSTOM's BASS, MIDS and TREBLE steps (-6..6 each), and select it
  eq.py list              the presets in the local file with their descriptions, the
                          console's selection marked (the local file must be the one
                          installed: its CRC is compared)
  eq.py install [FILE]    copy the preset file (default out/xw.presets, written by
                          tools/presets/convert.py) to Hdd:\\Plugins\\xw.presets, then have a
                          running build read it again (the sound restarts its fade)
  eq.py reload            read Hdd:\\Plugins\\xw.presets again

XBOX=host[:port] as for every tool. The block's layout is eq_360.c's struct xq_status.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools", "presets"))

import eq_model  # noqa: E402

XQ_MAGIC = 0x58574551  # "XWEQ"
XQ_VERSION = 3
FIELDS = ("magic version enable select reload preset preset_id count "
          "load_state load_error file_crc flat_q15 fade clamps blocks skipped "
          "heap_bytes quiesced "
          "file_version custom_index custom custom_set custom_error "  # version 2
          "pass_us").split()  # version 3
assert len(FIELDS) == 24  # _Static_assert in eq_360.c
V1_FIELDS = 18  # a build with the version-1 block (no CUSTOM)
V2_FIELDS = 23  # a build with the version-2 block (no time per pass)
CUSTOM_SET = 0x01000000
CUSTOM_ERRORS = {0: "offered", -1: "the file has no CUSTOM tables",
                 -2: "the CUSTOM tables have the wrong shape", -3: "a CUSTOM table value is wrong",
                 -10: "no preset file loaded"}
FLAT_DESC = "THE GAME AS IT WAS MIXED. NO CHANGE"  # eq_360.c g_flat_desc, for list
CUSTOM_DESC = "YOUR OWN SOUND: SET THE ROWS BELOW"  # eq_360.c g_custom_desc
NO_SELECT = 0xFFFFFFFF
LOAD_STATES = ["not posted yet", "loading", "loaded", "failed", "no file worker"]
LOAD_ERRORS = {0: "none", -1: "no file", -2: "disk error", -3: "file too big (over 64 presets)",
               -4: "no file worker", -11: "too short", -12: "wrong magic or version",
               -13: "wrong rate, bands or record size", -14: "wrong length",
               -15: "CRC mismatch", -16: "too many presets", -17: "a record's id is not its name's",
               -18: "out of memory", -19: "an older preset file (version 1): eq.py install"}
FADES = ["steady", "warming up", "fading"]
CONSOLE_FILE = "Hdd:\\Plugins\\xw.presets"
LOCAL_FILE = os.path.join(HERE, "out", "xw.presets")


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def block_addr():
    from xa import xw_block
    return xw_block(XQ_MAGIC)


def connect():
    from xa import Xbdm
    return Xbdm(), block_addr()


def read_block(x, addr):
    w = x.u32s(addr, V1_FIELDS)
    if not w or w[0] != XQ_MAGIC:
        raise SystemExit(f"XWEQ block not found at {addr:08x} (an xw build with eq_360.c?)")
    if w[1] == 1:  # an older build: no CUSTOM fields (they read as absent)
        g = dict(zip(FIELDS, w))
        g.update(file_version=0, custom_index=0, custom=0, custom_set=0, custom_error=-10)
        return g
    if w[1] == 2:  # no time per pass (reads as None)
        g = dict(zip(FIELDS, x.u32s(addr, V2_FIELDS)))
        g["pass_us"] = None
        return g
    if w[1] != XQ_VERSION:
        raise SystemExit(f"XWEQ version {w[1]}: this eq.py reads versions 1 to {XQ_VERSION}")
    return dict(zip(FIELDS, x.u32s(addr, len(FIELDS))))


def pass_times(g):
    """the EQ's time in the last driver-loop pass and the longest since start (or since
    pass_us was written 0), microseconds; (None, None) before XWEQ version 3"""
    v = g.get("pass_us")
    return (None, None) if v is None else (v & 0xFFFF, v >> 16)


def custom_steps(g):
    """the CUSTOM steps (bass, mids, treble) from the block's packed word"""
    return [((g["custom"] >> (8 * k)) & 0xFF) - (256 if (g["custom"] >> (8 * k)) & 0x80 else 0)
            for k in range(3)]


def pack_custom(steps):
    return CUSTOM_SET | sum((s & 0xFF) << (8 * k) for k, s in enumerate(steps))


def read(x=None, addr=None):
    if x is None:
        x, addr = connect()
    return read_block(x, addr)


def setctl(x, addr, name, value):
    return x.setu32(addr + 4 * FIELDS.index(name), value & 0xFFFFFFFF)


def local_presets(path=LOCAL_FILE):
    """(presets, flat_q15, crc) of a local file; the committed copy if out/ has none"""
    if not os.path.exists(path):
        path = os.path.join(os.path.dirname(HERE), "tools", "presets", "xw.presets")
    data = open(path, "rb").read()
    return eq_model.read_presets(data), eq_model.read_flat_q15(data), \
        struct.unpack(">I", data[24:28])[0]


def local_custom(path=LOCAL_FILE):
    """the CUSTOM tables of the local file (eq_model.read_custom), or None"""
    if not os.path.exists(path):
        path = os.path.join(os.path.dirname(HERE), "tools", "presets", "xw.presets")
    data = open(path, "rb").read()
    return eq_model.read_custom(data, eq_model.read_flat_q15(data))[0]


def print_status(g):
    import math
    err = s32(g["load_error"])
    sel = "CUSTOM" if g["preset"] and g["preset"] == g["custom_index"] else \
        f"id {g['preset_id']:08x}"
    print(f"gate {'ON' if g['enable'] else 'off'}{' (quiesced)' if g['quiesced'] else ''}; "
          f"preset {g['preset']} of {g['count']} ({sel}); "
          f"{FADES[g['fade']] if g['fade'] < 3 else g['fade']}")
    b, m, t = custom_steps(g)
    cerr = s32(g["custom_error"])
    print(f"CUSTOM: {'index %d' % g['custom_index'] if g['custom_index'] else 'not offered'} "
          f"({CUSTOM_ERRORS.get(cerr, cerr)}); bass {b:+d} mids {m:+d} treble {t:+d}")
    print(f"file: {LOAD_STATES[g['load_state']] if g['load_state'] < 5 else g['load_state']}"
          f"{'' if not err else ', error %d (%s)' % (err, LOAD_ERRORS.get(err, '?'))}; version "
          f"{g['file_version']}; crc {g['file_crc']:08x}; FLAT {20 * math.log10(g['flat_q15'] / 32768):+.2f} dB; "
          f"memory {g['heap_bytes']} bytes")
    print(f"blocks {g['blocks']} processed, {g['skipped']} passed through (not 48 kHz); "
          f"clamped samples {g['clamps']}")
    last, most = pass_times(g)
    if last is not None:
        print(f"time per driver-loop pass: last {last} us, longest {most} us (of the driver "
              f"thread's ~1 ms a frame: docs/INTERNALS.md, the system window)")


def find_index(arg, presets):
    if arg.isdigit():
        return int(arg)
    if arg.upper() == "FLAT":
        return 0
    if arg.upper() == "CUSTOM":
        return len(presets) + 1  # the console refuses it (FLAT) when CUSTOM is not offered
    want = arg.upper()
    hits = [i + 1 for i, p in enumerate(presets) if p["name"].startswith(want)]
    if len(hits) != 1:
        raise SystemExit(f"{arg!r} matches {len(hits)} presets (eq.py list)")
    return hits[0]


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    args = sys.argv[2:]
    if cmd == "list":
        presets, flat, crc = local_presets()
        g = None
        try:
            g = read()
        except (SystemExit, OSError) as e:
            print(f"(console not read: {e})")
        if g and g["file_crc"] != crc:
            print(f"the console's file (crc {g['file_crc']:08x}) is not this one ({crc:08x}): "
                  f"eq.py install")
        sel = g["preset"] if g else None
        print(f"{'*' if sel == 0 else ' '}  0  {'FLAT':20s}  {'':11s}  {FLAT_DESC}")
        for i, p in enumerate(presets):
            print(f"{'*' if sel == i + 1 else ' '} {i + 1:2d}  {p['name']:20s}  id {p['id']:08x}"
                  f"  {p['desc']}")
        n = len(presets) + 1
        if local_custom():
            steps = " (bass %+d mids %+d treble %+d)" % tuple(custom_steps(g)) if g else ""
            print(f"{'*' if sel == n else ' '} {n:2d}  {'CUSTOM':20s}  {'':11s}  "
                  f"{CUSTOM_DESC}{steps}")
        return
    if cmd == "install":
        path = args[0] if args else LOCAL_FILE
        if not os.path.exists(path):
            raise SystemExit(f"{path} is missing: python3 tools/presets/convert.py")
        data = open(path, "rb").read()
        eq_model.read_presets(data)  # refuse to send a file the plugin would refuse
        from xa import Xbdm
        print(CONSOLE_FILE, len(data), Xbdm().sendfile(CONSOLE_FILE, data))
        try:
            x, addr = connect()
        except SystemExit:
            print("no running build with XWEQ: it is read at the next boot")
            return
        setctl(x, addr, "reload", 1)
        print("reload asked")
        return
    x, addr = connect()
    if cmd == "status":
        print_status(read_block(x, addr))
    elif cmd in ("on", "off"):
        print(setctl(x, addr, "enable", 1 if cmd == "on" else 0))
    elif cmd == "select" and args:
        presets, _, _ = local_presets()
        idx = find_index(" ".join(args), presets)
        print(setctl(x, addr, "select", idx))
    elif cmd == "custom" and len(args) == 3:
        steps = [int(v) for v in args]
        if any(not -6 <= v <= 6 for v in steps):
            raise SystemExit("custom B M T: each -6..6")
        g = read_block(x, addr)
        if g["version"] < 2:
            raise SystemExit("this build has no CUSTOM (XWEQ version 1)")
        print(setctl(x, addr, "custom_set", pack_custom(steps)))
        g = read_block(x, addr)
        if not g["custom_index"]:
            raise SystemExit(f"steps set; CUSTOM is not offered: "
                             f"{CUSTOM_ERRORS.get(s32(g['custom_error']), g['custom_error'])}"
                             f"{'' if g['enable'] else ' (the gate is off: eq.py on)'}")
        print(setctl(x, addr, "select", g["custom_index"]))
    elif cmd == "reload":
        print(setctl(x, addr, "reload", 1))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
