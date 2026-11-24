#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rumble (rumble_360.c): its XWRB block over xbdm.

  rumble.py status            the gate, XAM's Vibration table and each pad's setting, the
                              last request and what the controller was sent, stops and why
  rumble.py on | off          the gate (ON at boot). off: the title's levels go to the pad as
                              asked, the Vibration setting is ignored and nothing is forced
                              (the old behaviour, for comparison)
  rumble.py test PAD L R      a request as if a title asked for it (0..100 each, pad slot
                              0..3), through the same path, the setting included. A non-zero
                              test stops by itself after 3 s; `test PAD 0 0` stops it now
  rumble.py watch [SECONDS]   one line per change

XBOX=host[:port] as for every tool. The layout is rumble_360.c's struct xr_status.
"""
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

XR_MAGIC = 0x58575242  # "XWRB"
XR_VERSION = 1
FIELDS = ("magic version enable test table vib req_l req_r sent_l sent_r requests zeroed "
          "stop_asked stop_setting stop_title stop_test restops last_why last_stop_ms now_ms "
          "stop_lat_ms stop_lat_max_ms title tests").split()
assert len(FIELDS) == 24  # _Static_assert in rumble_360.c
WHY = ["none", "the title asked", "Vibration switched off", "the title changed",
       "test request ended"]  # rumble_360.h XR_WHY_*
TEST_GO = 0x10000
TEST_MS = 3000          # rumble_360.h XR_TEST_MS
TABLE_17559 = 0x81AAC2DC  # XAM's per-user Vibration bytes on 17559 (docs/INTERNALS.md)


def block_addr():
    from xa import xw_block
    return xw_block(XR_MAGIC)


def connect():
    from xa import Xbdm
    return Xbdm(), block_addr()


def byte(w, pad):
    return (w >> (8 * (3 - pad))) & 0xFF


def read_block(x, addr):
    w = x.u32s(addr, len(FIELDS))
    if not w or w[0] != XR_MAGIC:
        raise SystemExit(f"XWRB block not found at {addr:08x} (an xw build with rumble_360.c?)")
    if w[1] != XR_VERSION:
        raise SystemExit(f"XWRB version {w[1]}: this rumble.py reads version {XR_VERSION}")
    g = dict(zip(FIELDS, w))
    g["pads"] = []
    for i in range(4):
        v = byte(g["vib"], i)
        g["pads"].append(dict(
            known=v != 0xFF, on=bool(v & 1) if v != 0xFF else None,
            user=None if v == 0xFF or (v >> 4) == 0xF else v >> 4,
            req=(byte(g["req_l"], i), byte(g["req_r"], i)),
            sent=(byte(g["sent_l"], i), byte(g["sent_r"], i))))
    return g


def read(x=None, addr=None):
    if x is None:
        x, addr = connect()
    return read_block(x, addr)


def setctl(x, addr, name, value):
    return x.setu32(addr + 4 * FIELDS.index(name), value & 0xFFFFFFFF)


def test(x, addr, pad, left, right):
    """a test request for pad slot `pad`; True once the driver thread has taken it"""
    setctl(x, addr, "test", TEST_GO | (pad & 3) << 24 | (left & 0xFF) << 8 | (right & 0xFF))
    until = time.time() + 1.0
    while time.time() < until:
        if read_block(x, addr)["test"] == 0:
            return True
        time.sleep(0.02)
    return False


def xam_table(x, g):
    """XAM's own four Vibration bytes (users 0..3), read directly, or None"""
    if not g["table"]:
        return None
    b = x.mem(g["table"], 4)
    return list(b) if b else None


def wait_for(x, addr, pred, secs, step=0.02):
    """the block once pred(block) is true, or the last one read after secs"""
    until = time.time() + secs
    while True:
        g = read_block(x, addr)
        if pred(g) or time.time() >= until:
            return g
        time.sleep(step)


def pad_line(i, p):
    if not p["known"]:
        return f"pad {i}: no controller bound (or the setting is not known)"
    who = "no XAM user index" if p["user"] is None else f"user {p['user']}"
    return (f"pad {i}: {who}, Vibration {'ON' if p['on'] else 'OFF'}; asked "
            f"{p['req'][0]}/{p['req'][1]}, sent {p['sent'][0]}/{p['sent'][1]}"
            f"{' (a re-sent stop is due)' if 0xFF in p['sent'] else ''}")


def print_status(x, g):
    t = g["table"]
    note = "" if t == TABLE_17559 else " (not the 17559 address)" if t else ""
    print(f"gate {'ON' if g['enable'] else 'off (the old behaviour)'}; XAM's Vibration "
          f"table {t:08x}{note}" if t else
          f"gate {'ON' if g['enable'] else 'off'}; XAM's Vibration table NOT RECOGNISED: "
          f"the setting is not applied")
    xt = xam_table(x, g)
    if xt:
        print("XAM's bytes, users 0-3: " +
              " ".join(f"{u}:{'on' if v else 'OFF'}" for u, v in enumerate(xt)))
    for i, p in enumerate(g["pads"]):
        if p["known"] or i == 0:
            print(pad_line(i, p))
    age = (g["now_ms"] - g["last_stop_ms"]) & 0xFFFFFFFF
    print(f"requests {g['requests']} (tests {g['tests']}), silenced by the setting "
          f"{g['zeroed']}")
    print(f"stops: asked {g['stop_asked']}, setting {g['stop_setting']}, title changed "
          f"{g['stop_title']}, test ended {g['stop_test']}; re-sent {g['restops']}")
    if g["last_why"]:
        print(f"last stop: {WHY[g['last_why']] if g['last_why'] < len(WHY) else g['last_why']}"
              f", {age / 1000:.1f} s ago, out to the pad in {g['stop_lat_ms']} ms "
              f"(longest {g['stop_lat_max_ms']} ms)")
    print(f"title {g['title']:08x}")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    x, addr = connect()
    if cmd == "status":
        print_status(x, read_block(x, addr))
    elif cmd in ("on", "off"):
        setctl(x, addr, "enable", 1 if cmd == "on" else 0)
        print_status(x, read_block(x, addr))
    elif cmd == "test" and len(sys.argv) == 5:
        pad, left, right = (int(v) for v in sys.argv[2:5])
        if not test(x, addr, pad, left, right):
            raise SystemExit("not taken: is a controller bound on that pad slot?")
        time.sleep(0.1)
        print(pad_line(pad, read_block(x, addr)["pads"][pad]))
    elif cmd == "watch":
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else 60
        until, last = time.time() + secs, None
        while time.time() < until:
            g = read_block(x, addr)
            line = " | ".join(pad_line(i, p) for i, p in enumerate(g["pads"]) if p["known"])
            key = (line, g["stop_asked"], g["stop_setting"], g["stop_title"], g["restops"])
            if key != last:
                last = key
                print(f"{time.strftime('%H:%M:%S')} {line or 'no pad'}; stops "
                      f"{g['stop_asked']}/{g['stop_setting']}/{g['stop_title']}/"
                      f"{g['stop_test']} re-sent {g['restops']}", flush=True)
            time.sleep(0.05)
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
