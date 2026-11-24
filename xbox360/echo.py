#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""echo.xex (echo.c) over xbdm: record / play / delayed echo through the headset. Run next to out/.

  echo.py launch              upload out/echo.xex to Hdd:\\echo, write Hdd:\\echo.ini with
                              xw.xex's 360-side rate and format, start it (this ends the
                              running game)
  echo.py record [SECS]       record up to SECS (1..10, default 10) of mic; waits, then
                              prints Hdd:\\echo.txt
  echo.py stop                stop recording / playback / echo
  echo.py play                play the last recording into the earpiece once; waits for the end
  echo.py echo on|off         mic to earpiece ~0.5 s later, gain 1/2, with a howl limiter
  echo.py status              the title's status block
  echo.py pull [FILE.wav]     fetch Hdd:\\echo.raw and write a WAV at the rate in Hdd:\\echo.txt
                              (default research/results/echo-<time>.wav)
  echo.py exit                stop audio and go back to Aurora

On the pad (user 0): A record (A again stops), B play, X echo on/off, Back exit.
XBOX=host[:port] is the console's address (required). Shares xbdm code with xa.py/voice.py.
"""
import os
import struct
import sys
import time
import wave

from xa import OUT, Xbdm, pe_va
import voice

B = "\\"
MAGIC = 0x4543484F  # "ECHO"
TITLE_DIR = "Hdd:" + B + "echo"
TITLE_XEX = TITLE_DIR + B + "echo.xex"
INI = "Hdd:" + B + "echo.ini"
TXT = "Hdd:" + B + "echo.txt"
RAW = "Hdd:" + B + "echo.raw"
RESULTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "research", "results")

# g_ec in echo.c, all u32, in order (enum EC_*)
FIELDS = ("magic phase heartbeat req ack mode rate fmt user ctx tag rec_samples rec_peak rec_rms "
          "saves save_status plays_done play_pos mic_rms mic_peak lim_trips muted cap_done ren_done "
          "submit_errors first_error buttons fifo other_status first_other create_status "
          "present").split()
assert len(FIELDS) == 32
OPS = {"record": 1, "stop": 2, "play": 3, "echo on": 4, "echo off": 5, "exit": 6}
MODES = {0: "idle", 1: "recording", 2: "playing", 3: "echo"}
PHASES = {1: "starting", 2: "exports resolved", 3: "waiting for a headset (no voice context)",
          4: "running", 5: "stopping", 6: "closed", 7: "launching Aurora"}


def block_addr():
    path = os.path.join(OUT, "echo.exe")
    tag = struct.pack(">I", MAGIC)
    if open(path, "rb").read().count(tag) != 1:
        raise SystemExit(f"magic not unique in {path}")
    return pe_va(path, tag)


def read_status(x, addr):
    w = x.u32s(addr, len(FIELDS))
    if not w or w[0] != MAGIC:
        return None
    return dict(zip(FIELDS, w))


def need_status(x, addr):
    s = read_status(x, addr)
    if s is None:
        raise SystemExit("echo.xex isn't running (or this isn't its build): echo.py launch")
    return s


def send(op, arg=0):
    addr = block_addr()
    x = Xbdm()
    s = need_status(x, addr)
    seq = ((s["req"] >> 16) + 1) & 0xFFFF or 1
    word = seq << 16 | (arg & 0xFF) << 8 | op
    st = x.setu32(addr + 4 * FIELDS.index("req"), word)
    if not st.startswith("200"):
        raise SystemExit(f"setmem: {st}")
    for _ in range(40):
        time.sleep(0.05)
        s = read_status(Xbdm(), addr)
        if s and s["ack"] == word:
            return addr, s
    raise SystemExit("the title didn't take the request (hung? echo.py status)")


def print_status(s):
    fmt = voice.FMTS.get(s["fmt"], "?")
    print(f"phase {s['phase']} ({PHASES.get(s['phase'], '?')}), mode {MODES.get(s['mode'], s['mode'])}, "
          f"heartbeat {s['heartbeat']}")
    print(f"360 side {s['rate']} Hz {fmt}; user {s['user']} ctx {s['ctx']:08x} tag {s['tag']:08x} "
          f"present {s['present']} (create status {s['create_status']:08x})")
    secs = s["rec_samples"] / s["rate"] if s["rate"] else 0
    print(f"recording: {s['rec_samples']} samples ({secs:.2f} s), peak {s['rec_peak']} rms "
          f"{s['rec_rms']}; saved {s['saves']}x (status {s['save_status']:08x})")
    print(f"playback: position {s['play_pos']}, finished {s['plays_done']}x")
    print(f"mic now: rms {s['mic_rms']} peak {s['mic_peak']}; echo delay line {s['fifo']} samples, "
          f"muted {s['muted']}, limiter trips {s['lim_trips']}")
    print(f"packets: capture done {s['cap_done']} render done {s['ren_done']}; submit errors "
          f"{s['submit_errors']} (first {s['first_error']:08x}), other status {s['other_status']} "
          f"(first {s['first_other']:08x}); buttons {s['buttons']:04x}")


def get_text():
    try:
        return Xbdm().getfile(TXT).decode(errors="replace")
    except (SystemExit, OSError):
        return None


def launch():
    x = Xbdm()
    g = voice.read_block(x, voice.block_addr())
    if g is None:
        print("warning: xw.xex's voice block not found; assuming 8000 Hz s16le")
        rate, fmt = 8000, 0
    else:
        rate, fmt = g["applied_cap_rate"], g["applied_cap_fmt"]
        if (g["applied_ren_rate"], g["applied_ren_fmt"]) != (rate, fmt):
            raise SystemExit(f"xw.xex capture ({rate} Hz fmt {fmt}) and render "
                             f"({g['applied_ren_rate']} Hz fmt {g['applied_ren_fmt']}) differ; "
                             f"echo.xex needs one rate: voice.py rate HZ [FMT]")
        if g["applied_stage"] < 4:
            print(f"warning: voice stage {g['applied_stage']} (< 4: no real PCM; voice.py stage 4)")
    data = open(os.path.join(OUT, "echo.xex"), "rb").read()
    Xbdm().cmd(f'mkdir name="{TITLE_DIR}"')
    print("echo.xex", len(data), Xbdm().sendfile(TITLE_XEX, data))
    ini = f"rate={rate}\nfmt={fmt}\nuser=-1\naurora=1\n"
    print(f"ini ({rate} Hz {voice.FMTS.get(fmt, fmt)})", Xbdm().sendfile(INI, ini.encode()))
    print("launch", Xbdm().cmd(f'magicboot title="{TITLE_XEX}" directory="{TITLE_DIR}"')[0])
    addr = block_addr()
    for _ in range(30):
        time.sleep(1)
        try:
            s = read_status(Xbdm(), addr)
        except OSError:
            continue
        if s and s["phase"] >= 3:
            print_status(s)
            return
    print("no status block yet (still starting? echo.py status)")


def record(secs):
    addr, s = send(OPS["record"], secs)
    saves = s["saves"]
    print(f"recording up to {secs} s (A or echo.py stop ends it early)")
    deadline = time.time() + secs + 15
    while time.time() < deadline:
        time.sleep(0.5)
        s = read_status(Xbdm(), addr)
        if s and s["saves"] != saves:
            break
    else:
        raise SystemExit("not saved in time: echo.py status")
    text = get_text()
    print(text if text else "no Hdd:\\echo.txt", end="")


def play():
    addr, s = send(OPS["play"])
    if not s["rec_samples"]:
        raise SystemExit("nothing recorded yet (in this run of echo.xex)")
    done = s["plays_done"]
    secs = s["rec_samples"] / s["rate"] if s["rate"] else 10
    deadline = time.time() + secs + 10
    while time.time() < deadline:
        time.sleep(0.25)
        s = read_status(Xbdm(), addr)
        if s and (s["plays_done"] != done or s["mode"] != 2):
            print("finished" if s["plays_done"] != done else f"stopped ({MODES.get(s['mode'])})")
            return
    print("still playing? echo.py status")


def pull(path):
    text = get_text()
    if not text:
        raise SystemExit("no Hdd:\\echo.txt (nothing recorded yet?)")
    info = dict(l.split("=", 1) for l in text.splitlines() if "=" in l)
    rate = int(info["rate"])
    raw = Xbdm().getfile(RAW)
    if len(raw) != int(info.get("bytes", len(raw))):
        print(f"warning: echo.raw has {len(raw)} bytes, echo.txt says {info.get('bytes')}")
    if not path:
        os.makedirs(RESULTS, exist_ok=True)
        path = os.path.join(RESULTS, time.strftime("echo-%Y%m%d-%H%M%S.wav"))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(raw[:len(raw) & ~1])  # echo.raw is s16le, as WAV
    print(f"{os.path.normpath(path)}: {len(raw) // 2} samples, {rate} Hz s16le mono, "
          f"peak {info.get('peak')} rms {info.get('rms')}")


def main():
    a = sys.argv[1:]
    step = a[0] if a else "status"
    if step == "launch":
        launch()
    elif step == "record":
        secs = int(a[1]) if len(a) > 1 else 10
        if not 1 <= secs <= 10:
            raise SystemExit("record: 1..10 seconds")
        record(secs)
    elif step == "stop":
        send(OPS["stop"])
    elif step == "play":
        play()
    elif step == "echo" and len(a) > 1 and a[1] in ("on", "off"):
        _, s = send(OPS["echo " + a[1]])
        print("echo", a[1])
    elif step == "status":
        print_status(need_status(Xbdm(), block_addr()))
    elif step == "pull":
        pull(a[1] if len(a) > 1 else None)
    elif step == "exit":
        send(OPS["exit"])
        print("exiting to Aurora")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
