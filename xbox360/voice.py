#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The virtual 360 headset in xw.xex (voice_360.c) over xbdm. Run next to out/.

  voice.py status             the voice block: stage, hooks, controls, counters per pad; then
                              mute and sidetone (level, feedback guard) and game audio (the
                              tap, the ring, drift trim, levels)
  voice.py game [on|off|balance N|master N|sidetone N]
                              game audio in the headset on/off; the mix: balance -5 (all
                              game) .. 0 (both 100 %) .. +5 (all chat), master 0..100 % (steps
                              of 10), sidetone 0 (off) .. 10 (own mic, x10 %); without an
                              argument, the game audio status
  voice.py sidetone N         the same as voice.py game sidetone N
  voice.py sidetone mode software|headset|auto
                              who makes the sidetone: our mix (~60 ms late), the headset
                              itself (its own DSP, MS-GIPUSB volume message: no delay), or
                              auto (default: the headset when its report marks sidetone
                              writable, else software)
  voice.py sidetone devmax|devspeaker|devmic N
                              the headset's sidetone % at SIDETONE 10 / its speaker / its mic
                              volume, 0..100 (default 100 each)
  voice.py sidetone devvol 0|1  1 (default): the panel's VOLUME turns the headset's own amplifier
                              (its buzz drops with the volume); 0: the samples are scaled
  voice.py sidetone quiet 0..3  hiss control (default 3): +1 the headset's sidetone only while you
                              talk, +2 its amplifier off while there is nothing to play
  voice.py sidetone bit7 0|1  set bit 7 ("writable") on the fields we send (default 0)
  voice.py mictest [on|off|start [PAD]|stop|play|pause|resume|cancel|closed|length MS|
                   duck PCT|save FILE.wav [--gated]|record SECS FILE.wav [PAD] [--gated]|keep SECS|
                   start-hold [PAD]|stop-hold]
                              the mic test (the panel's MIC TEST row, XWMT block): without an
                              argument its state; on / off shows / hides the row (on by
                              default); start records on pad slot PAD (default 0), stop ends
                              the recording and plays it, play plays a held clip again,
                              pause / resume, cancel = Left (discard), closed = the panel
                              closing (discards a paused or held clip), as the panel's Right
                              and Left do; length 1000..30000 ms is the auto-stop (default
                              30000; shorter for tests), duck 0..100 % is game audio during the
                              playback; save copies the clip held now (24 kHz mono; XWMT
                              version 3: the mic before MIC VOLUME and the noise gate, which the
                              playback applies, --gated: as the playback would sound with the
                              settings now; version 2: as teammates heard it) into a WAV,
                              retrying failed reads, verified, and
                              says why if the console discarded the clip meanwhile (a panel
                              close is the usual reason). record SECS FILE.wav [PAD]: records
                              SECS (1..30) s, never plays it into the player's ear, saves and
                              verifies it, then discards it on the console (an older build
                              without XWMT version 3: the game's volume is 0 for ~0.5 s at the
                              end instead). Version 3: keep SECS protects the clip held from
                              the panel closing for SECS s (save does it itself); start-hold /
                              stop-hold record without the playback. XW_DEV builds also take
                              start-small [PAD] / start-nomem [PAD]: a start as if the 1.44 MB
                              (or both) allocations had failed
  voice.py watch [SECONDS]    follow the plugin log and the pad 0 counters
  voice.py stage N            0 off | 1 hooks, pass-through + counters (R2) | 2 act for our tags,
                              report no headset (R3) | 3 virtual headset, silence or loopback
                              (V1) | 4 real PCM. Lowering never removes the hooks.
  voice.py set NAME VALUE     any control: stage cap_rate cap_fmt ren_rate ren_fmt loopback
                              force cap_flags (fmt: 0 s16le, 1 s16be, 2 u8, 3 mu-law;
                              force: bitmask of pads bound as having a headset without one;
                              loopback 1: the render stream comes back as capture)
  voice.py rate HZ [FMT]      the 360-side rate (and format) for both directions
  voice.py upload             copy out/voicetest.xex to Hdd:\\voicetest
  voice.py test [options]     run voicetest.xex (XamVoice* round trip, as a game's XHV) once per
                              rate and print its results. Options:
                                --rates 8000,16000,24000   (default: the current capture rate)
                                --fmt N --seconds S --freq HZ --packet-ms MS --user U
                                --stage N   set the stage first (e.g. 3)
                                --force M   set the force mask first (e.g. 1 = pad 0)
                                --loopback 0|1
                                --no-upload --no-aurora
                              voicetest relaunches Hdd:\\Aurora\\Aurora.xex when done.

XBOX=host[:port] is the console's address (required). Shares xbdm code with xa.py.
"""
import os
import struct
import sys
import time

from xa import OUT, Xbdm, pe_va, read_log, xw_block

B = "\\"
MAGIC = 0x58563336
TEST_DIR = "Hdd:" + B + "voicetest"
TEST_XEX = TEST_DIR + B + "voicetest.xex"
TEST_INI = "Hdd:" + B + "voicetest.ini"
TEST_TXT = "Hdd:" + B + "voicetest.txt"
TEST_RAW = "Hdd:" + B + "voicetest.raw"

# struct xv_status / xv_pad_status (voice_360.c), all u32, in order
GLOBAL = ("magic version stage cap_rate cap_fmt ren_rate ren_fmt loopback force cap_flags debug "
          "title stale_dropped title_switches applied_stage applied_cap_rate applied_cap_fmt "
          "applied_ren_rate applied_ren_fmt hooks hook_error heartbeat log_addr now_ms "
          "calls_present calls_submit calls_close our_present our_submit our_close stock_tag "
          "stock_cap_submit stock_ren_submit stock_cap_len stock_ren_len stock_cap_done "
          "stock_cap_bytes stock_cap_first_ms stock_cap_last_ms stock_ren_bytes "
          "stock_ren_first_ms stock_ren_last_ms stock_cap_mode stock_cap_flags stock_first_len "
          "stock_first_status " + " ".join(f"stock_first{i}" for i in range(8)) +
          " stock_ren_mode stock_ren_flags stock_status_last stock_close").split()
PAD = ("usb_present forced bound serving rate_in ch_in rate_out ch_out cap_submit ren_submit "
       "cap_done ren_done cap_full ren_full cancelled inactive cap_bytes ren_bytes cap_queued "
       "ren_queued mic_calls mic_bytes mic_dropped ear_calls ear_underrun vticks cap_len ren_len "
       "cap_mode ren_mode mic_peak ear_peak present_calls close_calls bind_requests "
       "unbind_requests bind_errors loop_samples cap_first_ms cap_last_ms").split()
assert len(GLOBAL) == 58 and len(PAD) == 40  # _Static_assert in voice_360.c
# struct xv_hs_status / xv_hs_pad_status (voice_360.c): mute and sidetone, its own block
HS_MAGIC = 0x58564853
HS_GLOBAL = "magic version".split()
HS_PAD = "muted gain rms ducks guard gaps skips fill mixed ticks".split()
# version 3, after pads[4]: the sidetone's path and the headset's own volume settings
HS_TAIL = ("st_mode dev_max dev_speaker dev_mic dev_bit7 st_path dev_pad dev_reports "
           "dev_report0 dev_report1 dev_writable dev_sends dev_sent0 dev_sent1 dev_sidetone "
           "dev_trips side_delay_ms dev_vol dev_vol_on dev_quiet").split()
assert len(HS_GLOBAL) == 2 and len(HS_PAD) == 10 and len(HS_TAIL) == 20  # XV_HS_TAIL
ST_MODES = {"software": 0, "headset": 1, "auto": 2}
ST_MODE_NAMES = {0: "software", 1: "headset", 2: "auto"}
ST_PATHS = {0: "off (level 0 or muted)", 1: "software (our mix)", 2: "the headset itself"}
VOL_NAMES = ("flags", "speaker", "balance", "mic", "sidetone")
# struct xg_status (game_360.c): game audio, its own block
GA_MAGIC = 0x58564741
GA_FIELDS = ("magic version enable balance master sidetone applied_enable applied_balance "
             "applied_master applied_sidetone game_gain chat_gain state error handle process_type register_tries "
             "tap_on tap_calls tap_frames capture_errors "
             "overflow_frames bad_floats in_peak in_peak_max ring_write mix_calls mix_frames "
             "ring_read ring_fill fill_avg target drift_ppm drift_changes underruns overruns "
             "starts out_peak tap_rate consumer_pad rate_out ch_out").split()
assert len(GA_FIELDS) == 42  # _Static_assert in game_360.c
GA_STATES = {0: "not registered yet (20 s after start, while on)", 1: "registered",
             2: "registration failed, retrying", 3: "refused (kernel code differs)"}
CONTROLS = ("stage", "cap_rate", "cap_fmt", "ren_rate", "ren_fmt", "loopback", "force", "cap_flags")
FMTS = {0: "s16le", 1: "s16be", 2: "u8", 3: "mu-law"}
STAGES = {0: "off", 1: "hooks, pass-through + counters", 2: "our tags: no headset",
          3: "virtual headset (silence/loopback)", 4: "real PCM"}


def block_addr():
    return xw_block(MAGIC)


def read_block(x, addr):
    n = len(GLOBAL) + 4 * len(PAD)
    w = x.u32s(addr, n)
    if not w or w[0] != MAGIC:
        return None
    g = dict(zip(GLOBAL, w[:len(GLOBAL)]))
    g["pads"] = [dict(zip(PAD, w[len(GLOBAL) + i * len(PAD):len(GLOBAL) + (i + 1) * len(PAD)]))
                 for i in range(4)]
    return g


def read_hs(x):
    """the mute / sidetone block, or None (an older build without it)"""
    try:
        addr = xw_block(HS_MAGIC)
    except SystemExit:
        return None
    w = x.u32s(addr, len(HS_GLOBAL) + 4 * len(HS_PAD))
    if not w or w[0] != HS_MAGIC or w[1] < 2:
        return None
    h = dict(zip(HS_GLOBAL, w[:len(HS_GLOBAL)]))
    h["pads"] = [dict(zip(HS_PAD, w[len(HS_GLOBAL) + i * len(HS_PAD):
                                    len(HS_GLOBAL) + (i + 1) * len(HS_PAD)])) for i in range(4)]
    h["tail"] = None
    if w[1] >= 3:
        t = x.u32s(addr + 4 * (len(HS_GLOBAL) + 4 * len(HS_PAD)), len(HS_TAIL))
        if t:
            h["tail"] = dict(zip(HS_TAIL, t))
    return h


def hs_addr_of(name):
    """the address of an XVHS version 3 tail field"""
    return xw_block(HS_MAGIC) + 4 * (len(HS_GLOBAL) + 4 * len(HS_PAD) + HS_TAIL.index(name))


def vol_bytes(w0, w1, n=None):
    b = list(w0.to_bytes(4, "big") + w1.to_bytes(4, "big"))
    return b[:n] if n is not None else b


def vol_text(b):
    """the headset's volume fields: W = writable by the host, ro = read-only"""
    if len(b) < 5:
        return "(none)"
    out = [f"flags {b[0]:02x}"]
    for i in range(1, 5):
        out.append(f"{VOL_NAMES[i]} {'W' if b[i] & 0x80 else 'ro'} {b[i] & 0x7F}")
    return ", ".join(out)


def print_st_path(t):
    if t is None:
        print("  sidetone path: no XVHS version 3 tail (older build)")
        return
    print(f"  sidetone path: {ST_PATHS.get(t['st_path'], t['st_path'])}; mode "
          f"{ST_MODE_NAMES.get(t['st_mode'], t['st_mode'])}, software delay estimate "
          f"{t['side_delay_ms']} ms (0 = never on)")
    if t["dev_pad"] == 0xFFFFFFFF:
        print("  headset volume: no headset seen yet")
        return
    n = t["dev_report1"] & 0xFF
    rep = vol_bytes(t["dev_report0"], t["dev_report1"], min(n, 7))
    sent = vol_bytes(t["dev_sent0"], t["dev_sent1"], 5)
    print(f"  headset volume (pad {t['dev_pad']}): {t['dev_reports']} reports, last "
          f"({n} bytes): {vol_text(rep)}")
    print(f"    sent {t['dev_sends']}x, last: flags {sent[0]:02x}, speaker {sent[1]:02x}, "
          f"balance {sent[2]:02x}, mic {sent[3]:02x}, sidetone {sent[4]:02x}; asking sidetone "
          f"{t['dev_sidetone']} %, guard trips {t['dev_trips']}; settings: max {t['dev_max']} %, "
          f"speaker {t['dev_speaker']} %, mic {t['dev_mic']} %, bit7 {t['dev_bit7']}")


def print_hs(h, rates):
    if h is None:
        print("mute / sidetone: no XVHS version 2 block (older build?)")
        return
    print("mute / sidetone:")
    for i, p in enumerate(h["pads"]):
        if not (p["muted"] or p["gain"] or p["mixed"] or p["ducks"] or p["ticks"]):
            continue
        rate = rates[i] or 1
        print(f"  pad {i}: mic {'muted' if p['muted'] else 'on'}; sidetone gain {p['gain']}/32768"
              f" (guard {p['guard']}/32768, pulled down {p['ducks']} times), level in the ear "
              f"(rms) {p['rms']}, buffered {p['fill']} samples ({p['fill'] * 1000 / rate:.1f} ms), "
              f"gaps {p['gaps']}, skips {p['skips']}, mixed samples {p['mixed']}; centre ticks "
              f"{p['ticks']}")
    print_st_path(h.get("tail"))


def ga_addr():
    """the game audio block's address, or None (an older build without it)"""
    try:
        return xw_block(GA_MAGIC)
    except SystemExit:
        return None


def read_ga(x, addr):
    if addr is None:
        return None
    w = x.u32s(addr, len(GA_FIELDS))
    if not w or w[0] != GA_MAGIC:
        return None
    return dict(zip(GA_FIELDS, w))


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def print_ga(g):
    if g is None:
        print("game audio: no XVGA block (older build?)")
        return
    ms = lambda frames: f"{frames / 48:.1f} ms"
    bal = s32(g["applied_balance"])
    bar = ("-" * max(bal, 0) + "#" * (5 - max(bal, 0)) + "-" + "#" * (5 + min(bal, 0)) +
           "-" * -min(bal, 0))
    print(f"mix: Game [{bar}] Chat, balance {bal} (asked {s32(g['balance'])}), master "
          f"{g['applied_master']}% (asked {g['master']}), sidetone {g['applied_sidetone']}/10 "
          f"(asked {g['sidetone']}); gains game {g['game_gain']}/256 chat {g['chat_gain']}/256")
    print(f"game audio: {'on' if g['applied_enable'] else 'off'} (asked {g['enable']}); "
          f"{GA_STATES.get(g['state'], g['state'])}, error {g['error']:08x}, handle "
          f"{g['handle']:08x}, process type {g['process_type']}, tries {g['register_tries']}")
    print(f"  tap: {'copying' if g['tap_on'] else 'idle'}; calls {g['tap_calls']} "
          f"({g['tap_rate']}/s, 187-188 expected), frames copied {g['tap_frames']}, capture errors "
          f"{g['capture_errors']}, frames dropped (ring full) {g['overflow_frames']}, frames with "
          f"NaN {g['bad_floats']}; input peak {g['in_peak']} (last second), max {g['in_peak_max']}")
    print(f"  earpiece: pad {s32(g['consumer_pad'])} at {g['rate_out']} Hz x{g['ch_out']}; mix calls "
          f"{g['mix_calls']}, frames {g['mix_frames']}; ring fill {g['ring_fill']} "
          f"({ms(g['ring_fill'])}), average {g['fill_avg']} ({ms(g['fill_avg'])}), target "
          f"{g['target']} ({ms(g['target'])})")
    print(f"  drift trim {s32(g['drift_ppm'])} ppm ({g['drift_changes']} changes); starts "
          f"{g['starts']}, underruns {g['underruns']}, overruns {g['overruns']}; output peak "
          f"{g['out_peak']} (since the last 10 s log line)")


def set_game(args):
    addr = ga_addr()
    if addr is None:
        raise SystemExit("no game audio block in out/xw.exe (older build?)")
    x = Xbdm()
    if read_ga(x, addr) is None:
        raise SystemExit(f"game audio block not found at {addr:08x} (xw.xex loaded? this build?)")
    if not args:
        print_ga(read_ga(x, addr))
    elif args[0] in ("on", "off"):
        print("enable =", int(args[0] == "on"),
              x.setu32(addr + 4 * GA_FIELDS.index("enable"), int(args[0] == "on")))
    elif args[0] == "balance" and len(args) > 1 and -5 <= int(args[1]) <= 5:
        v = int(args[1])
        print("balance =", v, x.setu32(addr + 4 * GA_FIELDS.index("balance"), v & 0xFFFFFFFF))
    elif args[0] == "master" and len(args) > 1 and 0 <= int(args[1]) <= 100 and int(args[1]) % 10 == 0:
        v = int(args[1])
        print("master =", v, x.setu32(addr + 4 * GA_FIELDS.index("master"), v))
    elif args[0] == "sidetone" and len(args) > 1 and 0 <= int(args[1]) <= 10:
        v = int(args[1])
        print("sidetone =", v, x.setu32(addr + 4 * GA_FIELDS.index("sidetone"), v))
    else:
        raise SystemExit("voice.py game [on | off | balance -5..5 | master 0..100 (steps of 10) | "
                         "sidetone 0..10]")


def set_sidetone(what, value):
    """voice.py sidetone mode|devmax|devspeaker|devmic|bit7 VALUE (XVHS version 3)"""
    x = Xbdm()
    h = read_hs(x)
    if h is None or h.get("tail") is None:
        raise SystemExit("no XVHS version 3 block (xw.xex loaded? a build with the headset's "
                         "own sidetone?)")
    if what == "mode":
        if value not in ST_MODES:
            raise SystemExit("voice.py sidetone mode software|headset|auto")
        name, v = "st_mode", ST_MODES[value]
    else:
        name = {"devmax": "dev_max", "devspeaker": "dev_speaker", "devmic": "dev_mic",
                "bit7": "dev_bit7", "devvol": "dev_vol", "quiet": "dev_quiet"}[what]
        v = int(value)
        if not 0 <= v <= (1 if what in ("bit7", "devvol") else 3 if what == "quiet" else 100):
            raise SystemExit(f"voice.py sidetone {what}: out of range")
    print(f"{name} =", v, x.setu32(hs_addr_of(name), v))


def field_addr(addr, name):
    return addr + 4 * GLOBAL.index(name)


def hooks_text(h):
    if not h:
        return "none"
    parts = [n for bit, n in ((1, "present"), (2, "submit"), (4, "close")) if h & bit]
    if h & 0x80000000:
        parts.append("FAILED")
    if h & 0x40000000:
        parts.append("no-bind-build")
    return ",".join(parts)


def print_block(g):
    print(f"stage {g['applied_stage']} ({STAGES.get(g['applied_stage'], '?')}), asked {g['stage']}; "
          f"hooks {hooks_text(g['hooks'])} (error {g['hook_error']:08x}); heartbeat {g['heartbeat']}")
    print(f"360 side: capture {g['applied_cap_rate']} Hz {FMTS.get(g['applied_cap_fmt'], '?')}, "
          f"render {g['applied_ren_rate']} Hz {FMTS.get(g['applied_ren_fmt'], '?')}; "
          f"loopback {g['loopback']}, force {g['force']:x}, capture +16 = {g['cap_flags']:08x}")
    print(f"title {g['title']:08x}, title switches {g['title_switches']}, packets of a gone title "
          f"dropped unwritten {g['stale_dropped']}")
    print(f"calls: present {g['calls_present']} submit {g['calls_submit']} close {g['calls_close']}"
          f" (ours {g['our_present']}/{g['our_submit']}/{g['our_close']})")
    if g["stock_tag"]:
        first = struct.pack(">8I", *[g[f"stock_first{i}"] for i in range(8)])[:g["stock_first_len"]]
        span = g["stock_cap_last_ms"] - g["stock_cap_first_ms"]
        rspan = g["stock_ren_last_ms"] - g["stock_ren_first_ms"]
        print(f"stock tag {g['stock_tag']:08x}: capture submits {g['stock_cap_submit']} "
              f"(len {g['stock_cap_len']}), done {g['stock_cap_done']} = {g['stock_cap_bytes']} "
              f"bytes over {span} ms ({g['stock_cap_bytes'] * 1000 // span if span else 0} B/s), "
              f"+16 {g['stock_cap_flags']:08x} +20 {g['stock_cap_mode']}, last status "
              f"{g['stock_status_last']:08x}")
        print(f"  render submits {g['stock_ren_submit']} (len {g['stock_ren_len']}), "
              f"{g['stock_ren_bytes']} bytes over {rspan} ms "
              f"({g['stock_ren_bytes'] * 1000 // rspan if rspan else 0} B/s), +16 "
              f"{g['stock_ren_flags']:08x} +20 {g['stock_ren_mode']}; closes {g['stock_close']}")
        print(f"  first capture bytes ({g['stock_first_len']}): {first.hex(' ')}")
    for i, p in enumerate(g["pads"]):
        if not (p["usb_present"] or p["bound"] or p["forced"] or p["mic_calls"] or p["ear_calls"]
                or p["cap_submit"] or p["present_calls"]):
            continue
        span = p["cap_last_ms"] - p["cap_first_ms"]
        print(f"pad {i}: usb headset {p['usb_present']} (mic {p['rate_in']} x{p['ch_in']}, earpiece "
              f"{p['rate_out']} x{p['ch_out']}), forced {p['forced']}, bound {p['bound']}, "
              f"serving {p['serving']}; binds {p['bind_requests']} unbinds "
              f"{p['unbind_requests']} errors {p['bind_errors']}")
        print(f"  capture: submits {p['cap_submit']} done {p['cap_done']} bytes {p['cap_bytes']} "
              f"({p['cap_bytes'] * 1000 // span if span else 0} B/s) queued {p['cap_queued']} full "
              f"{p['cap_full']} len {p['cap_len']} mode {p['cap_mode']}; mic dropped "
              f"{p['mic_dropped']} peak {p['mic_peak']}")
        print(f"  render: submits {p['ren_submit']} done {p['ren_done']} bytes {p['ren_bytes']} "
              f"queued {p['ren_queued']} full {p['ren_full']} len {p['ren_len']} mode "
              f"{p['ren_mode']}; underrun samples {p['ear_underrun']} peak {p['ear_peak']}")
        print(f"  usb: mic calls {p['mic_calls']} ({p['mic_bytes']} bytes), earpiece calls "
              f"{p['ear_calls']}; internal ticks {p['vticks']}, loopback samples "
              f"{p['loop_samples']}; cancelled {p['cancelled']}, inactive-process "
              f"{p['inactive']}, present calls {p['present_calls']}, closes {p['close_calls']}")


def need_block(x, addr):
    g = read_block(x, addr)
    if g is None:
        raise SystemExit(f"voice block not found at {addr:08x} (xw.xex loaded? this build?)")
    return g


def setctl(name, value):
    if name not in CONTROLS:
        raise SystemExit(f"unknown control {name}; one of {', '.join(CONTROLS)}")
    addr = block_addr()
    x = Xbdm()
    need_block(x, addr)
    print(name, "=", value, x.setu32(field_addr(addr, name), value))


def getfile_or_none(name):
    try:
        return Xbdm().getfile(name)
    except (SystemExit, OSError):
        return None


def run_test(args):
    opts = {"--rates": None, "--fmt": None, "--seconds": "10", "--freq": "1000", "--packet-ms": "20",
            "--user": "-1", "--stage": None, "--force": None, "--loopback": None}
    flags = set()
    it = iter(args)
    for a in it:
        if a in ("--no-upload", "--no-aurora"):
            flags.add(a)
        elif a in opts:
            opts[a] = next(it, None)
        else:
            raise SystemExit(f"unknown option {a}\n" + __doc__)
    addr = block_addr()
    x = Xbdm()
    g = need_block(x, addr)
    for name, opt in (("stage", "--stage"), ("force", "--force"), ("loopback", "--loopback")):
        if opts[opt] is not None:
            print(f"{name} = {opts[opt]}", x.setu32(field_addr(addr, name), int(opts[opt], 0)))
    fmt = int(opts["--fmt"]) if opts["--fmt"] is not None else g["applied_cap_fmt"]
    rates = ([int(r) for r in opts["--rates"].split(",")] if opts["--rates"]
             else [g["applied_cap_rate"]])
    if "--no-upload" not in flags:
        data = open(os.path.join(OUT, "voicetest.xex"), "rb").read()
        Xbdm().cmd(f'mkdir name="{TEST_DIR}"')
        print("voicetest.xex", len(data), Xbdm().sendfile(TEST_XEX, data))
    seconds = int(opts["--seconds"])
    results = []
    for rate in rates:
        x = Xbdm()
        x.setu32(field_addr(addr, "cap_rate"), rate)
        x.setu32(field_addr(addr, "ren_rate"), rate)
        x.setu32(field_addr(addr, "cap_fmt"), fmt)
        x.setu32(field_addr(addr, "ren_fmt"), fmt)
        time.sleep(0.5)
        g = need_block(Xbdm(), addr)
        if g["applied_cap_rate"] != rate or g["applied_cap_fmt"] != fmt:
            print(f"rate {rate} fmt {fmt}: rejected by the plugin (see its log); skipped")
            continue
        before = g["pads"]
        ini = (f"rate={rate}\nfmt={fmt}\nfreq={opts['--freq']}\nseconds={seconds}\n"
               f"packet_ms={opts['--packet-ms']}\nuser={opts['--user']}\nraw=1\n"
               f"aurora={0 if '--no-aurora' in flags else 1}\n")
        Xbdm().cmd(f'delete name="{TEST_TXT}"')
        print(f"--- rate {rate} Hz, {FMTS.get(fmt, fmt)}: ini", Xbdm().sendfile(TEST_INI, ini.encode()))
        print("launch", Xbdm().cmd(f'magicboot title="{TEST_XEX}" directory="{TEST_DIR}"')[0])
        time.sleep(seconds + 5)
        text = None
        for _ in range(60):
            try:
                text = getfile_or_none(TEST_TXT)
            except OSError:
                text = None
            if text and text.rstrip().endswith(b"end"):
                break
            time.sleep(2)
        if not text:
            print("no result file (title didn't run, or hung: check xbdm, then voice.py status)")
            continue
        text = text.decode(errors="replace")
        print(text, end="")
        res = dict(l.split("=", 1) for l in text.splitlines() if "=" in l)
        res["rate"] = str(rate)
        results.append(res)
        raw = getfile_or_none(TEST_RAW)
        if raw:
            path = os.path.join(OUT, f"voicetest_{rate}.raw")
            open(path, "wb").write(raw)
            print(f"raw capture: {path} ({len(raw)} bytes, s16le mono {rate} Hz)")
        try:
            g = read_block(Xbdm(), addr)
            if g:
                for i, (a, b) in enumerate(zip(before, g["pads"])):
                    if b["bound"] or a["cap_submit"] != b["cap_submit"]:
                        print(f"plugin pad {i}: capture done +{b['cap_done'] - a['cap_done']} "
                              f"render done +{b['ren_done'] - a['ren_done']} mic dropped "
                              f"+{b['mic_dropped'] - a['mic_dropped']} underrun "
                              f"+{b['ear_underrun'] - a['ear_underrun']} cancelled "
                              f"+{b['cancelled'] - a['cancelled']}")
        except OSError as e:
            print(f"xbdm: {e}")
        time.sleep(8)  # Aurora coming back before the next launch
    if len(results) > 1:
        print("--- summary")
        for r in results:
            print(f"rate {r['rate']:>5}: detected {r.get('detected', '?'):3} tone {r.get('tone_pct', '?'):>4}% "
                  f"off {r.get('offtone_pct', '?'):>4}% rms {r.get('rms', '?'):>6} capture "
                  f"{r.get('capture_bytes_per_s', '?'):>7} B/s render {r.get('render_bytes_per_s', '?'):>7} B/s")


# struct xd_mt_status (mictest_360.c, "XWMT"): the mic test. All u32, in order; _Static_assert
# there: MT_WORDS 43. Version 2: record / play / pause; secs counts up while recording.
# Version 3 appends keep, kept and takes cmd 10 / 11 (held recordings, never played).
MT_MAGIC = 0x58574D54
MT_VERSION = 3
MT_VERSIONS = {2: 41, 3: 43}  # version: words (a version 2 block reads as the first 41)
MT_FIELDS = ("magic version enable cmd cmd_pad rec_ms duck_pct state pad secs pct want "
             "recorded played starts done cancels refused last_refusal mute_stops stalls "
             "rec_peak rec_rms rec_shut rec_gain rec_gate play_peak duck_now buf_addr buf_bytes "
             "allocs alloc_fails clip_ms max_ms plays pauses discards panel_closes fallback "
             "heap_bytes alloc_us keep kept").split()
MT_WORDS = 43
assert len(MT_FIELDS) == MT_WORDS
MT_RATE = 24000
MT_MAX_MS, MT_SMALL_MS = 30000, 10000
IDLE, RECORDING, PLAYING, PAUSED, READY = range(5)
MT_STATES = ["idle", "RECORDING", "PLAYING", "PAUSED", "READY (clip held)"]
MT_REFUSALS = ["-", "switched off (enable 0)", "busy", "no headset", "mic muted",
               "out of memory", "no clip held"]
# XWMT cmd (mictest_360.c MT_CMD_*); 8 and 9 only in XW_DEV builds, 10 and 11 from version 3
MT_CMDS = {"start": 1, "cancel": 2, "stop": 3, "play": 4, "pause": 5, "resume": 6, "closed": 7,
           "start-small": 8, "start-nomem": 9, "start-hold": 10, "stop-hold": 11}
MT_CMD_V3 = ("start-hold", "stop-hold")


def mt_addr():
    return xw_block(MT_MAGIC)


def read_mt(x, quiet=True, addr=None):
    """the XWMT block as a dict (version 2 without the version 3 fields); None if absent or of
    a version this voice.py does not know"""
    try:
        addr = addr if addr is not None else mt_addr()
    except SystemExit:
        return None
    w = x.u32s(addr, 2)
    if not w or w[0] != MT_MAGIC or w[1] not in MT_VERSIONS:
        if not quiet:
            print(f"mic test: no XWMT version {' / '.join(map(str, MT_VERSIONS))} block (an "
                  "older build?)")
        return None
    n = MT_VERSIONS[w[1]]
    w = x.u32s(addr, n)
    return dict(zip(MT_FIELDS[:n], w)) if w else None


def mt_state(v):
    return MT_STATES[v] if v < len(MT_STATES) else f"?{v}"


def print_mt(m):
    if not m:
        print("mic test: no XWMT block (older build)")
        return
    import math
    db = lambda v: f"{20 * math.log10(v / 32768):.1f} dBFS" if v else "silence"
    st = m["state"]
    print(f"mic test: {'on' if m['enable'] else 'OFF (voice.py mictest on)'}; {mt_state(st)}"
          + (f", {m['secs']} s so far ({m['pct']}% of {m['max_ms'] // 1000} s)"
             if st == RECORDING else "")
          + (f", {m['pct']}% of {m['secs']} s played" if st in (PLAYING, PAUSED) else "")
          + (f", {m['clip_ms']} ms" if st == READY else "")
          + f"; auto-stop {m['rec_ms']} ms (in effect {m['max_ms']} ms), game ducked to "
          f"{m['duck_pct']}% while playing (now {m['duck_now'] * 100 // 256}%)")
    print(f"  recordings {m['starts']}, playbacks {m['plays']} (to the end {m['done']}), pauses "
          f"{m['pauses']}; discarded {m['discards']} (Left {m['cancels']}, panel closed "
          f"{m['panel_closes']}, mute stops {m['mute_stops']}, stalled {m['stalls']}); refused "
          f"{m['refused']} (last: {MT_REFUSALS[m['last_refusal']] if m['last_refusal'] < 7 else m['last_refusal']})")
    if m["starts"]:
        print(f"  last: pad {m['pad']}, recorded {m['recorded'] * 1000 // MT_RATE} of "
              f"{m['want'] * 1000 // MT_RATE} ms at volume {m['rec_gain']}% gate "
              f"{m['rec_gate']}: peak {m['rec_peak']} ({db(m['rec_peak'])}), rms {m['rec_rms']} "
              f"({db(m['rec_rms'])}), gate shut for {m['rec_shut'] * 1000 // MT_RATE} ms; "
              f"played {m['played'] * 1000 // MT_RATE} ms, peak in the ear {m['play_peak']}")
    print(f"  buffer {m['buf_addr']:08x}, holding {m['heap_bytes']} bytes (last allocation "
          f"{m['buf_bytes']} bytes in {m['alloc_us']} us); allocations {m['allocs']}, failed "
          f"{m['alloc_fails']}, 10 s fallbacks {m['fallback']}"
          + (f"; panel closes that kept a clip for xbdm {m['kept']}" if "kept" in m else ""))


MT_CHUNK = 0x1000   # bytes a getmem (8 KB of hex back; ~0.5 MB/s, a 30 s clip in ~3 s)
MT_TRIES = 4        # a getmem that fails while the clip is still held is tried again
MT_KEEP_S = 120     # XWMT keep while a tool reads (version 3)


class ClipGone(Exception):
    pass


def mt_gone_why(m0, m1):
    """why the clip held at m0 is no longer there at m1 (XWMT counters)"""
    if not m1:
        return "the XWMT block is gone (the plugin was reloaded?)"
    d = lambda k: m1[k] - m0[k]
    whys = [w for k, w in (("panel_closes", "the panel closed (Share released)"),
                           ("cancels", "Left on MIC TEST, xbdm cancel, the headset going or a "
                                       "hot reload"),
                           ("stalls", "the stall watchdog"),
                           ("starts", "a new recording started")) if d(k) > 0]
    return " / ".join(whys) or f"state {mt_state(m1['state'])}, buffer {m1['buf_addr']:08x}"


def mt_same_clip(m0, m1):
    return (m1 is not None and m1["state"] in (PLAYING, PAUSED, READY) and
            m1["buf_addr"] == m0["buf_addr"] and m1["starts"] == m0["starts"] and
            m1["recorded"] == m0["recorded"])


def mt_put(x, addr, name, v):
    x.setu32(addr + 4 * MT_FIELDS.index(name), v & 0xFFFFFFFF)


def read_clip(m, addr):
    """the clip described by m (held: PLAYING / PAUSED / READY), read over xbdm in MT_CHUNK
    pieces. A failed piece is retried on a new connection while the clip is still held;
    ClipGone (with the reason) once it is not. Returns the raw big-endian bytes."""
    n = 2 * m["recorded"]
    data = bytearray()
    x = Xbdm()
    while len(data) < n:
        want = min(MT_CHUNK, n - len(data))
        for tries in range(MT_TRIES):
            try:
                chunk = x.mem(m["buf_addr"] + len(data), want)
            except OSError:
                chunk = None
            if chunk and len(chunk) == want:
                break
            # freed pages read back as "??": was the clip discarded, or was it xbdm?
            x.close()
            time.sleep(0.2 * (tries + 1))
            x = Xbdm()
            m1 = read_mt(x, addr=addr)
            if not mt_same_clip(m, m1):
                raise ClipGone(f"the clip was discarded on the console after {len(data)} of "
                               f"{n} bytes: {mt_gone_why(m, m1)}")
        else:
            raise SystemExit(f"getmem at {m['buf_addr'] + len(data):08x} failed {MT_TRIES} "
                             f"times although the clip is still held: nothing saved")
        data += chunk
    m1 = read_mt(x, addr=addr)
    x.close()
    if not mt_same_clip(m, m1):
        raise ClipGone(f"the clip was discarded on the console while it was read: "
                       f"{mt_gone_why(m, m1)}")
    return bytes(data)


def write_clip(path, data):
    """big-endian s16 from the console -> a 24 kHz mono WAV, read back to verify"""
    import wave
    pcm = b"".join(struct.pack("<h", v) for (v,) in struct.iter_unpack(">h", data))
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(MT_RATE)
        f.writeframes(pcm)
    with wave.open(path, "rb") as f:
        ok = (f.getnframes() == len(data) // 2 and f.getframerate() == MT_RATE and
              f.readframes(f.getnframes()) == pcm)
    if not ok:
        raise SystemExit(f"{path}: written but does not read back the same")


def gate_clip(data, x):
    """a version 3 clip (before MIC VOLUME and the gate) as its playback would sound now: the
    gate's threshold and the volume in effect (XWDG gate_threshold, mic_gain_q15) through
    tools/micdsp/micdsp.py's playback, bit-exact with mictest_360.c's. Returns (bytes, text)."""
    import xw
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools",
                                    "micdsp"))
    import micdsp
    d = xw.read_diag(x)
    if not d:
        raise SystemExit("--gated: no XWDG block to read the gate and the volume from")
    thr, q12 = d["gate_threshold"], d["mic_gain_q15"] // (32768 // 4096)
    ys = micdsp.playback([v for (v,) in struct.iter_unpack(">h", data)], thr, q12)
    return (struct.pack(f">{len(ys)}h", *ys),
            f"gated as played now: threshold {thr}, volume {q12 * 100 // 4096} %")


def save_mt(path, gated=False):
    """the clip held now into a WAV, read over xbdm. Version 3 builds hold the mic before MIC
    VOLUME and the noise gate (the playback applies them): saved as it is, or with gated
    as the playback would sound with the settings in effect now. Version 2 builds hold it
    as teammates heard it when it was made. On a version 3 build the clip is first
    protected from the panel closing (keep)."""
    x = Xbdm()
    addr = mt_addr()
    m = read_mt(x, quiet=False, addr=addr)
    if not m or m["state"] not in (PLAYING, PAUSED, READY) or not m["buf_addr"]:
        raise SystemExit("no clip held on the console (record one first; it is deleted when "
                         "the panel closes)")
    if "keep" in m:
        mt_put(x, addr, "keep", MT_KEEP_S)
    else:
        if gated:
            raise SystemExit("--gated: this build's clip is already gated (XWMT version 2)")
        print("note: this build discards the clip if the panel closes during the read (about "
              f"{m['recorded'] * 2 / 500000:.0f} s): keep Share released", flush=True)
    t0 = time.time()
    try:
        data = read_clip(m, addr)
    except ClipGone as e:
        raise SystemExit(f"{e}: nothing saved")
    how = "before MIC VOLUME and the gate" if "keep" in m else "as teammates heard it"
    if gated:
        data, how = gate_clip(data, x)
    x.close()
    write_clip(path, data)
    print(f"{path}: {m['recorded'] / MT_RATE:.2f} s, {MT_RATE} Hz mono, {how} ({len(data)} bytes "
          f"read in {time.time() - t0:.1f} s, verified)")
    return path


def wait_mt(x, addr, until, timeout, what):
    """polls XWMT every 50 ms until until(m) is true; the last block read"""
    end = time.time() + timeout
    while True:
        m = read_mt(x, addr=addr)
        if m and until(m):
            return m
        if time.time() > end:
            raise SystemExit(f"timed out waiting for {what} (state "
                             f"{mt_state(m['state']) if m else '-'})")
        time.sleep(0.05)


def record_mt(secs, path, pad=0, gated=False):
    """voice.py mictest record SECS FILE.wav: record SECS seconds of the mic as teammates get
    it, never play it into the player's ear, save it (verified), then discard it on the
    console. Version 3 builds: a held recording (cmd 10) with keep. Version 2 builds: the
    recording ends in a playback, so the game's master volume is 0 from 0.3 s before the
    auto-stop until the playback is paused (the ear gets ~0.5 s of silence instead of game
    audio), and the panel must stay closed until the save is done."""
    ms = int(round(secs * 1000))
    if not 1000 <= ms <= MT_MAX_MS:
        raise SystemExit(f"record: 1 .. {MT_MAX_MS // 1000} seconds")
    x = Xbdm()
    addr = mt_addr()
    m0 = read_mt(x, quiet=False, addr=addr)
    if not m0:
        raise SystemExit(1)
    if m0["state"] not in (IDLE, READY):
        raise SystemExit(f"the mic test is busy ({mt_state(m0['state'])}): voice.py mictest "
                         "cancel first")
    v3 = "keep" in m0
    rec_ms0 = m0["rec_ms"]
    ga, master0 = ga_addr(), None
    put = lambda name, v: mt_put(x, addr, name, v)
    try:
        put("rec_ms", ms)
        if v3:
            put("keep", MT_KEEP_S + ms // 1000)
        put("cmd_pad", pad)
        put("cmd", MT_CMDS["start-hold" if v3 else "start"])
        m = wait_mt(x, addr, lambda m: m["starts"] > m0["starts"] or m["refused"] > m0["refused"],
                    2, "the recording to start")
        if m["refused"] > m0["refused"]:
            raise SystemExit("the console refused the recording: "
                             f"{MT_REFUSALS[m['last_refusal']] if m['last_refusal'] < 7 else m['last_refusal']}")
        print(f"recording {ms} ms on pad {pad}" + ("" if v3 else " (older build: the game's "
              "volume drops to 0 for about half a second at the end; keep the panel closed)"),
              flush=True)
        if not v3:
            g = read_ga(x, ga)
            if g is None:
                raise SystemExit("older build without the game audio block: cannot keep the "
                                 "playback out of the ear; use the panel instead")
            want = m["want"]
            wait_mt(x, addr, lambda m: m["recorded"] >= want - MT_RATE * 3 // 10 or
                    m["state"] != RECORDING, ms / 1000 + 3, "the end of the recording")
            master0 = g["master"]
            x.setu32(ga + 4 * GA_FIELDS.index("master"), 0)
            m = wait_mt(x, addr, lambda m: m["state"] != RECORDING, 3, "the auto-stop")
            if m["state"] == PLAYING:
                put("cmd", MT_CMDS["pause"])
                m = wait_mt(x, addr, lambda m: m["state"] != PLAYING, 2, "the pause")
            time.sleep(0.1)  # the pause's 5 ms fade, at master 0
            x.setu32(ga + 4 * GA_FIELDS.index("master"), master0)
            master0 = None
        else:
            m = wait_mt(x, addr, lambda m: m["state"] != RECORDING, ms / 1000 + 3,
                        "the end of the recording")
        if m["state"] not in (PAUSED, READY) or m["starts"] != m0["starts"] + 1:
            raise SystemExit(f"the recording ended {mt_state(m['state'])}: "
                             f"{mt_gone_why(m0, m)}")
        if v3 and m["plays"] != m0["plays"]:
            print("warning: the console played the clip (an older mictest_360.c?)")
        x.close()
        try:
            data = read_clip(m, addr)
        except ClipGone as e:
            raise SystemExit(f"{e}: nothing saved")
        x = Xbdm()
        how = "before MIC VOLUME and the gate" if v3 else "as teammates heard it"
        if gated:
            if not v3:
                raise SystemExit("--gated: this build's clip is already gated (XWMT version 2)")
            data, how = gate_clip(data, x)
        write_clip(path, data)
        print(f"{path}: {m['recorded'] / MT_RATE:.2f} s, {MT_RATE} Hz mono, {how}, verified; "
              f"rms {m['rec_rms']}, peak {m['rec_peak']}", flush=True)
        put = lambda name, v: mt_put(x, addr, name, v)
        put("cmd", MT_CMDS["cancel"])
        wait_mt(x, addr, lambda m: m["state"] == IDLE, 2, "the clip to be discarded")
        print("discarded on the console")
        return path
    finally:
        try:
            if master0 is not None:
                x.setu32(ga + 4 * GA_FIELDS.index("master"), master0)
            put("rec_ms", rec_ms0)
        except OSError:
            print(f"warning: could not put back rec_ms {rec_ms0}" +
                  (f" and master {master0}" if master0 is not None else ""))


MT_USAGE = ("usage: voice.py mictest on|off|start [PAD]|stop|play|pause|resume|cancel|closed|"
            "length MS|duck PCT|save FILE.wav [--gated]|record SECS FILE.wav [PAD] [--gated]|"
            "keep SECS|"
            "start-hold [PAD]|stop-hold  (XW_DEV: start-small [PAD]|start-nomem [PAD])")


def set_mt(args):
    """voice.py mictest on|off|start [PAD]|stop|play|pause|resume|cancel|closed|length MS|
    duck PCT|keep SECS|start-hold [PAD]|stop-hold|start-small [PAD]|start-nomem [PAD]"""
    x = Xbdm()
    m = read_mt(x, quiet=False)
    if not m:
        raise SystemExit(1)
    addr = mt_addr()
    put = lambda name, v: print(name, "=", v, x.setu32(addr + 4 * MT_FIELDS.index(name), v))
    if (args[0] in MT_CMD_V3 or args[0] == "keep") and "keep" not in m:
        raise SystemExit(f"{args[0]}: needs an XWMT version 3 build (this one is "
                         f"{m['version']})")
    if args[:1] == ["on"]:
        put("enable", 1)
    elif args[:1] == ["off"]:
        put("enable", 0)
    elif args[:1] == ["keep"] and len(args) == 2:
        put("keep", int(args[1]))
    elif args[0] in MT_CMDS and (len(args) == 1 or args[0].startswith("start") and len(args) == 2):
        if args[0].startswith("start"):
            put("cmd_pad", int(args[1]) if len(args) > 1 else 0)
        put("cmd", MT_CMDS[args[0]])
    elif args[:1] == ["length"] and len(args) == 2:
        put("rec_ms", int(args[1]))
    elif args[:1] == ["duck"] and len(args) == 2:
        put("duck_pct", int(args[1]))
    else:
        raise SystemExit(MT_USAGE)


def main():
    step = sys.argv[1] if len(sys.argv) > 1 else "status"
    if step == "status":
        x = Xbdm()
        g = need_block(x, block_addr())
        print_block(g)
        print_hs(read_hs(x), [p["rate_in"] for p in g["pads"]])
        print_ga(read_ga(x, ga_addr()))
        print_mt(read_mt(x))
    elif step == "mictest" and sys.argv[2:3] in (["save"], ["record"]):
        args = [a for a in sys.argv[3:] if a != "--gated"]
        gated = "--gated" in sys.argv[3:]
        if sys.argv[2] == "save":
            save_mt(args[0] if args else "mictest.wav", gated)
        elif len(args) in (2, 3):
            record_mt(float(args[0]), args[1], int(args[2]) if len(args) > 2 else 0, gated)
        else:
            raise SystemExit(MT_USAGE)
    elif step == "mictest" and len(sys.argv) > 2:
        set_mt(sys.argv[2:])
    elif step == "mictest":
        print_mt(read_mt(Xbdm(), quiet=False))
    elif step == "watch":
        addr = block_addr()
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else 120
        since, until, last = 0, time.time() + secs, None
        while time.time() < until:
            try:
                x = Xbdm()
                g = read_block(x, addr)
                if g:
                    since, text = read_log(x, g["log_addr"], since)
                    if text:
                        print(text, end="", flush=True)
                    p = g["pads"][0]
                    cur = (g["applied_stage"], p["bound"], p["cap_done"], p["ren_done"],
                           g["calls_submit"])
                    if cur != last:
                        print(f"[voice] stage {cur[0]} pad0 bound {cur[1]} capture done {cur[2]} "
                              f"render done {cur[3]} submits {cur[4]}", flush=True)
                        last = cur
            except OSError as e:
                print(f"xbdm: {e}", flush=True)
            time.sleep(0.5)
    elif step == "game":
        set_game(sys.argv[2:])
    elif step == "sidetone" and len(sys.argv) > 3 and sys.argv[2] in (
            "mode", "devmax", "devspeaker", "devmic", "bit7", "devvol", "quiet"):
        set_sidetone(sys.argv[2], sys.argv[3])
    elif step == "sidetone":
        set_game(sys.argv[1:])
    elif step == "stage":
        setctl("stage", int(sys.argv[2]))
    elif step == "set":
        setctl(sys.argv[2], int(sys.argv[3], 0))
    elif step == "rate":
        hz = int(sys.argv[2])
        setctl("cap_rate", hz)
        setctl("ren_rate", hz)
        if len(sys.argv) > 3:
            setctl("cap_fmt", int(sys.argv[3]))
            setctl("ren_fmt", int(sys.argv[3]))
    elif step == "upload":
        data = open(os.path.join(OUT, "voicetest.xex"), "rb").read()
        Xbdm().cmd(f'mkdir name="{TEST_DIR}"')
        print("voicetest.xex", len(data), Xbdm().sendfile(TEST_XEX, data))
    elif step == "test":
        run_test(sys.argv[2:])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
