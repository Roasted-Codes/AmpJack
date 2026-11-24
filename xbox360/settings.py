#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-profile settings (settings_360.c): its XWST block over xbdm, and the settings file.

  settings.py status          the XWST block: gate, state, the active profile, records, loads
                              and saves, save time, audio counters during the last save, errors
  settings.py on | off        the gate (off at boot). Off: nothing is read, written or applied
                              and XAM is not asked. On: like a boot (the file is loaded once and
                              the active profile's settings are put back)
  settings.py dump [--file PATH]
                              Hdd:\\Plugins\\xw.settings (or a local copy) as text, one line per
                              profile, every record's CRC checked
  settings.py wipe            forget every profile: the file is rewritten empty and whoever is
                              active gets the compiled defaults (needs the gate on)
  settings.py probe           XW_DEV builds: write the file now, unchanged, and report how long
                              the write took and whether the earpiece starved meanwhile

XBOX=host[:port] as for every tool. The block's layout is settings_360.c's struct xs_status,
the file's is settings_360.h (big-endian).
"""
import os
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

XS_MAGIC = 0x58575354  # "XWST", the block and the file
XS_VERSION = 1         # the block's version (struct xs_status)
FIELDS = ("magic version enable wipe probe state xuid_hi xuid_lo slot user inplace records "
          "loads saves save_ms_last save_ms_max errors crc_rejects last_error heap_bytes "
          "restores xam_status save_starved save_missed").split()
assert len(FIELDS) == 24  # _Static_assert in settings_360.c
STATES = ["off", "loading the file", "ready", "no file worker (retrying)", "out of memory",
          "quiesced (hot reload)"]
ERRORS = {0: "none", -1: "no file", -2: "disk error", -3: "file too big",
          -4: "no file worker", -21: "file too short", -22: "wrong magic, version or size",
          -23: "out of memory", -24: "file worker busy or absent", -25: "XAM export not found"}
NO_SLOT = 0xFFFFFFFF
SIGNIN = ["not signed in", "signed in locally", "signed in to Live"]
CONSOLE_FILE = "Hdd:\\Plugins\\xw.settings"

# the file (settings_360.h)
FILE_VERSION, REC_SIZE, CAPACITY, HDR_SIZE, CRC_LEN = 1, 48, 64, 16, 44
FILE_BYTES = HDR_SIZE + CAPACITY * REC_SIZE  # 3088: always written whole
XUID_EMPTY = 0xFFFFFFFFFFFFFFFF
F_MIC_AUTO = 0x01
F_CUSTOM = 0x02  # the EQ's CUSTOM selected; its steps are bytes 27-29 (zero in older files)
GATES = ["off", "low", "high", "auto"]  # diag_360.h XD_GATE_*


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


# ---- the file ----------------------------------------------------------------------------

def parse_file(data):
    """(header, records, problems) of an xw.settings image. records: one dict per used slot
    (`slot`, `xuid`, `seq`, the values, `crc_ok`); problems: why the plugin would refuse the
    whole file or a record (it then uses the compiled defaults for it)"""
    problems = []
    if len(data) < HDR_SIZE:
        return None, [], [f"{len(data)} bytes: shorter than the header"]
    magic, ver, rec_size, count, cap, _ = struct.unpack_from(">IHHHHI", data, 0)
    hdr = dict(magic=magic, version=ver, rec_size=rec_size, count=count, capacity=cap,
               length=len(data))
    if magic != XS_MAGIC or ver != FILE_VERSION or rec_size != REC_SIZE or cap != CAPACITY:
        problems.append(f"header {magic:08x} v{ver} rec {rec_size} cap {cap}: not this "
                        f"plugin's (it starts an empty table)")
        return hdr, [], problems
    if len(data) != FILE_BYTES:
        problems.append(f"{len(data)} bytes, not {FILE_BYTES}: records past the end are lost")
    recs = []
    for i in range(CAPACITY):
        off = HDR_SIZE + i * REC_SIZE
        if off + REC_SIZE > len(data):
            break
        r = data[off:off + REC_SIZE]
        xuid, seq = struct.unpack_from(">QI", r, 0)
        if xuid == XUID_EMPTY:
            continue
        (rver, bal, vol, side, mic, gate, sur, flags, preset, tl, tc, route) = \
            struct.unpack_from(">BbBBBBBBIBBB", r, 12)
        custom = list(struct.unpack_from(">3b", r, 27))
        crc = struct.unpack_from(">I", r, CRC_LEN)[0]
        ok = crc == zlib.crc32(r[:CRC_LEN]) & 0xFFFFFFFF and rver == 1
        if not ok:
            problems.append(f"slot {i}: bad CRC or record version (the plugin drops it)")
        recs.append(dict(slot=i, xuid=xuid, seq=seq, ver=rver, balance=bal, volume=vol,
                         sidetone=side, mic_level=mic, mic_auto=bool(flags & F_MIC_AUTO),
                         gate=gate, surround=sur, preset_id=preset, team_level=tl,
                         team_channel=tc, mic_route=route, crc_ok=ok,
                         custom_selected=bool(flags & F_CUSTOM), custom=custom))
    if count != len(recs):
        problems.append(f"header says {count} records, {len(recs)} slots are used")
    return hdr, recs, problems


def record_for(recs, xuid):
    """the record of xuid in parse_file's list, or None"""
    for r in recs:
        if r["xuid"] == xuid and r["crc_ok"]:
            return r
    return None


def describe(r):
    who = "nobody signed in" if r["xuid"] == 0 else f"xuid {r['xuid']:016x}"
    gate = GATES[r["gate"]] if r["gate"] < len(GATES) else r["gate"]
    return (f"slot {r['slot']:2d} seq {r['seq']:5d}  {who:24s} balance {r['balance']:+d} "
            f"volume {r['volume']} sidetone {r['sidetone']} mic {r['mic_level']}"
            f"{' AUTO' if r['mic_auto'] else ''} gate {gate} surround {r['surround']} "
            f"preset {'CUSTOM' if r['custom_selected'] else '%08x' % r['preset_id']} "
            f"(custom {r['custom'][0]:+d} {r['custom'][1]:+d} {r['custom'][2]:+d}) "
            f"team {r['team_level']}/ch {r['team_channel']}"
            f"{'' if r['crc_ok'] else '  BAD CRC'}")


def print_file(data):
    hdr, recs, problems = parse_file(data)
    if hdr:
        print(f"{hdr['length']} bytes, version {hdr['version']}, {hdr['count']} of "
              f"{hdr['capacity']} profiles")
    for r in sorted(recs, key=lambda r: -r["seq"]):
        print(describe(r))
    for p in problems:
        print("PROBLEM", p)


# ---- the block ---------------------------------------------------------------------------

def block_addr():
    from xa import xw_block
    return xw_block(XS_MAGIC)


def connect():
    from xa import Xbdm
    return Xbdm(), block_addr()


def read_block(x, addr):
    w = x.u32s(addr, len(FIELDS))
    if not w or w[0] != XS_MAGIC:
        raise SystemExit(f"XWST block not found at {addr:08x} (an xw build with settings_360.c?)")
    if w[1] != XS_VERSION:
        raise SystemExit(f"XWST version {w[1]}: this settings.py reads version {XS_VERSION}")
    g = dict(zip(FIELDS, w))
    g["xuid"] = g["xuid_hi"] << 32 | g["xuid_lo"]
    return g


def read(x=None, addr=None):
    if x is None:
        x, addr = connect()
    return read_block(x, addr)


def setctl(x, addr, name, value):
    return x.setu32(addr + 4 * FIELDS.index(name), value & 0xFFFFFFFF)


def print_status(g):
    st = STATES[g["state"]] if g["state"] < len(STATES) else g["state"]
    user, signin = g["user"] & 0xFF, (g["user"] >> 8) & 0xFF
    who = "nobody signed in (the shared record)" if g["xuid"] == 0 else f"xuid {g['xuid']:016x}"
    slot = "no record yet (the defaults)" if g["slot"] == NO_SLOT else f"record {g['slot']}"
    print(f"gate {'ON' if g['enable'] else 'off'}; {st}; active: {who}, {slot}")
    print(f"headset pad's user index {'none' if user == 0xFF else user}, "
          f"{'not asked' if signin == 0xFF else SIGNIN[signin] if signin < 3 else signin}")
    err = s32(g["last_error"])
    print(f"file: {g['records']} profiles; loads {g['loads']}, saves {g['saves']}, restores "
          f"{g['restores']}; written in place (no rename) {g['inplace']}; memory "
          f"{g['heap_bytes']} bytes")
    print(f"last save {g['save_ms_last']} ms (max {g['save_ms_max']} ms); during it the "
          f"earpiece starved {g['save_starved']}x, missed {g['save_missed']} frames")
    print(f"errors {g['errors']} (last {err}: {ERRORS.get(err, '?')}); CRC rejects "
          f"{g['crc_rejects']}; XAM's last odd answer {g['xam_status']:08x}")


def wait_for(x, addr, pred, secs):
    """the block once pred(block) is true, or the last one read after secs"""
    until = time.time() + secs
    while True:
        g = read_block(x, addr)
        if pred(g) or time.time() >= until:
            return g
        time.sleep(0.1)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    args = sys.argv[2:]
    if cmd == "dump":
        if args[:1] == ["--file"] and len(args) > 1:
            data = open(args[1], "rb").read()
        else:
            from xa import Xbdm
            data = Xbdm().getfile(CONSOLE_FILE)
        print_file(data)
        return
    x, addr = connect()
    if cmd == "status":
        print_status(read_block(x, addr))
    elif cmd in ("on", "off"):
        print(setctl(x, addr, "enable", 1 if cmd == "on" else 0))
        if cmd == "on":
            g = wait_for(x, addr, lambda g: g["state"] != 1 and g["restores"], 5)
            print_status(g)
    elif cmd == "wipe":
        g = read_block(x, addr)
        if not g["enable"]:
            raise SystemExit("the gate is off: settings.py on first (a wipe is done by the plugin)")
        saves = g["saves"]
        setctl(x, addr, "wipe", 1)
        g = wait_for(x, addr, lambda g: g["saves"] > saves, 5)
        print("every profile forgotten" if g["saves"] > saves else "no save seen in 5 s")
        print_status(g)
    elif cmd == "probe":
        import xw
        xw.need_dev("settings.py probe")
        g = read_block(x, addr)
        if g["state"] != 2:
            raise SystemExit("the settings are not ready (settings.py on, then wait a moment)")
        saves = g["saves"]
        setctl(x, addr, "probe", 1)
        g = wait_for(x, addr, lambda g: g["saves"] > saves, 5)
        if g["saves"] == saves:
            raise SystemExit("no save within 5 s: " + ERRORS.get(s32(g["last_error"]), "?"))
        print(f"write: {g['save_ms_last']} ms; the earpiece starved {g['save_starved']}x and "
              f"missed {g['save_missed']} frames during it")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
