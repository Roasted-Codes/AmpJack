#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drives xw.xex on the real console over xbdm: is it up, what is running, a screenshot, the
Share panel opened and walked without hands, a slow-pass measurement, and the safe way to boot
a new build. A thin layer over the project's own tools in xbox360/ (xa.py, xw.py, hud.py,
shot.py, accept/wp1_3.py); run it from the repo root:

  XBOX=10.0.0.13 python3 .claude/skills/run-xbox360-gip-wired/driver.py COMMAND

  up [SECS]            is xbdm answering (waits up to SECS, default 0); exit status 1 if not
  status               one screen: title running, panel build (XH36 version, stage, hook), the
                       driver loop's passes, starved audio blocks, EQ state
  shot FILE.png        the screen as the console scans it out
  panel FILE.png [down N] [right N] [left N]
                       hold Share (injected), press the D-pad, screenshot the frame the panel
                       is drawn into (about 8 s, so moving backgrounds tear), let go. Holds
                       for at least 0.6 s: letting go inside 0.4 s toggles the mic mute
  slow [SECS]          how many driver-loop passes took 25 ms or more in SECS (default 6): the
                       A/B number for "does this setting stall the audio loop" (0 is healthy)
  keep                 out/xw.xex (and out/xw.presets, out/xw.demo) onto the console's disk,
                       no reboot
  boottest FALLBACK.xex [--max SECS]
                       the only safe way to try a build that has never booted: install
                       out/xw.xex, cold boot, and the moment xbdm answers put FALLBACK back on
                       disk (so a freeze costs one power-cycle, not a dead console), watch the
                       new build for 2 minutes, then put it back on disk if it stayed up.
                       Says so after 150 s without the console (it has probably switched
                       itself off: someone must press its power button) and gives up after
                       --max (default 3600) with the new build still on the disk
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
XB = os.path.join(ROOT, "xbox360")
sys.path[:0] = [XB, os.path.join(XB, "accept")]
PLUGIN = "Hdd:\\Plugins\\xw.xex"


def host():
    if not os.environ.get("XBOX"):
        raise SystemExit("set XBOX=10.0.0.13 (the console's address)")
    h, _, p = os.environ["XBOX"].partition(":")
    return h, int(p or 730)


def is_up():
    try:
        socket.create_connection(host(), timeout=1).close()
        return True
    except OSError:
        return False


def tool(*args):
    """one of the project's tools, its output as text"""
    r = subprocess.run([sys.executable, *args], cwd=XB, capture_output=True, text=True)
    return r.stdout + r.stderr


def slow_count():
    for line in tool("xw.py", "status").splitlines():
        if "passes:" in line and ">=25 ms" in line:
            return int(line.rsplit(">=25 ms", 1)[1].split()[0])
    raise SystemExit("no pass histogram in xw.py status (is xw.xex loaded?)")


def cmd_up(args):
    end = time.time() + (float(args[0]) if args else 0)
    while not is_up():
        if time.time() >= end:
            print("down: xbdm at %s:%d does not answer" % host())
            raise SystemExit(1)
        time.sleep(1)
    print("up")


def cmd_status(_):
    from xa import Xbdm
    print("title:", Xbdm().cmd("xbeinfo running")[1][-1])
    print(tool("hud.py", "status").splitlines()[0])
    for line in tool("xw.py", "status").splitlines():
        if any(k in line for k in ("passes:", "driver loop:", "starved completions",
                                   "earpiece 0 blocks", "blocks (")):
            print(line.strip()[:200])
    print("\n".join(tool("eq.py", "status").splitlines()[:3]))


def cmd_shot(args):
    print(tool("shot.py", os.path.abspath(args[0])).strip())


def cmd_panel(args):
    import wp1_3 as w
    bits = {"down": w.DOWN, "up": w.UP, "left": w.LEFT, "right": w.RIGHT}
    c = w.Console(0)
    c.inject(w.SHARE)
    time.sleep(0.6)
    try:
        for name, n in zip(args[1::2], args[2::2]):
            for _ in range(int(n)):
                c.press(bits[name])
        time.sleep(0.3)
        # --mem: the frame the panel was drawn into, read with getmem (about 8 s). xbdm's own
        # screenshot returns a different, smaller buffer at Aurora that the panel is not in.
        print(tool("hud.py", "shot", os.path.abspath(args[0]), "--mem", "--find").strip()[-300:])
        p = c.panel()
        print("panel: visible %u, cursor %u" % (p["visible"], p["cursor"]))
    finally:
        c.inject(0)


def cmd_slow(args):
    secs = float(args[0]) if args else 6
    a = slow_count()
    time.sleep(secs)
    print("slow passes (>=25 ms) in %g s: +%d" % (secs, slow_count() - a))


def cmd_keep(_):
    print(tool("xw.py", "install", "--no-reboot").strip())


def cmd_boottest(args):
    from xa import Xbdm
    good = open(args[0], "rb").read()
    limit = float(args[args.index("--max") + 1]) if "--max" in args else 3600
    new = open(os.path.join(XB, "out", "xw.xex"), "rb").read()
    out = tool("xw.py", "install")
    print(out.strip())
    if "magicboot cold" not in out:
        raise SystemExit("install failed: nothing rebooted")
    t0 = time.time()
    while is_up() and time.time() - t0 < 60:
        time.sleep(1)
    told = False
    while not is_up():
        waited = time.time() - t0
        if waited > 150 and not told:
            told = True
            print("NOT BACK after 150 s: the console has probably switched itself off "
                  "(magicboot cold does that) or hangs at boot. Someone must press its power "
                  "button; if it hangs, power-cycle once more.", flush=True)
        if waited > limit:
            raise SystemExit("gave up after %d s: the NEW build is still on the disk" % limit)
        time.sleep(0.5)
    for _ in range(60):
        try:
            print("fallback back on disk:", Xbdm().sendfile(PLUGIN, good), flush=True)
            break
        except Exception:
            time.sleep(0.3)
    else:
        raise SystemExit("could not write the fallback: the NEW build is still on the disk")
    alive = 0
    for _ in range(60):
        alive += is_up()
        time.sleep(2)
    print("after 2 min: xbdm answered %d/60 polls" % alive)
    print(tool("hud.py", "status").splitlines()[0])
    if alive < 58:
        raise SystemExit("the new build did not stay up: the fallback stays on the disk")
    st = Xbdm().sendfile(PLUGIN, new)
    ok = Xbdm().getfile(PLUGIN) == new
    print("new build back on disk:", st, "(read back ok)" if ok else "(READ BACK DIFFERS)")


COMMANDS = {"up": cmd_up, "status": cmd_status, "shot": cmd_shot, "panel": cmd_panel,
            "slow": cmd_slow, "keep": cmd_keep, "boottest": cmd_boottest}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        raise SystemExit(__doc__)
    host()
    COMMANDS[sys.argv[1]](sys.argv[2:])
