#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for the sidetone (docs/PLAN.md WP1.5 / WP1.6, branch `sidetone`): the
headset's own sidetone (MS-GIPUSB 3.2.5.1.1 Audio Control Volume, its SIDETONE field) driven by
the panel's SIDETONE row, with our software mix as the fallback.

Run it with the `sidetone` xw.xex loaded, the pad on and the jack headset plugged in and
streaming (any title; Aurora is fine):

  XBOX=10.0.0.13 python3 xbox360/accept/sidetone.py [--pad N] [--hands]

  --pad N   the pad slot of the headset (default 0)
  --hands   also run the the owner checks: the script pauses at each and asks for y / n
            (without it they are printed as TODO lines and not counted)

ACCEPT_READ_ONLY=1 (accept/run.py --read-only): only the reads (steps 1-3), nothing written.

Side effects, all undone at the end (sidetone level, path mode, bit7 and the mute are put back
as they were): the sidetone is set to 5, 10, 0; the mic is muted and unmuted once with an
injected Share tap (the bar shows MIC MUTED / MIC ON); the software path is on for ~2 s.

Checks (one PASS or FAIL line each; the exit status is 1 if any failed):
  1. the running build has the XVHS version 3 block (the sidetone path, the headset's volume);
  2. the headset is up and has sent a volume report; prints it (which fields are writable);
  3. the report marks the SIDETONE field writable (else auto mode stays on software);
  4. sidetone 5 in auto: the path is the headset's own, we sent sidetone 50 %, and the
     headset's answering report says 50 % (it applied it); if it doesn't, the same again
     with bit 7 set on the written fields (`voice.py sidetone bit7 1`);
  5. the software sidetone is silent meanwhile (no samples mixed);
  6. sidetone 10 -> 100 %, 0 -> 0 % (sent and reported);
  7. a Share tap (mute) sends 0 % and the path says off; a second tap brings 50 % back;
  8. mode software: the headset gets 0 %, our mix runs (samples mixed rise), and its delay
     estimate is printed (side_delay_ms: what WP1.6 would cut);
  9. settings put back.
the owner checks (--hands), each one sentence:
  J1 SIDETONE 5: talk. You hear your own voice in the headset at once, with no echo.
  J2 Share + RB (x5) then Share + LB (x5): your voice gets louder, then quieter, then gone.
  J3 Share tap (muted): your voice is gone from the headset; tap again: it is back.
  J4 (comparison) the software path for 5 s: your voice comes back late, like an echo.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import voice  # noqa: E402
from xa import Xbdm, xw_block  # noqa: E402

PN_MAGIC = 0x5857504E  # XWPN (panel_360.c)
PN_INJECT = 2          # word index of `inject` (accept/wp1_3.py PN_FIELDS)
SHARE = 1
STEP_S = 0.08
SETTLE_S = 0.4         # a level change -> xv_headset_volume (next pass) -> USB -> its answer
ANSWER_S = 1.5         # how long to wait for the headset's answering report
TOAST_S = 1.6

failed = []
passed = []


def result(ok, name, detail=""):
    print(f"{'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""), flush=True)
    (passed if ok else failed).append(name)
    return ok


def info(text):
    print(f"INFO {text}", flush=True)


class Console:
    def __init__(self, pad):
        self.x = Xbdm()
        self.pad = pad
        self.ga = voice.ga_addr()
        self.pn = xw_block(PN_MAGIC)

    def hs(self):
        h = voice.read_hs(self.x)
        if not h:
            raise SystemExit("XVHS block unreadable (xw.xex loaded?)")
        return h

    def tail(self):
        return self.hs()["tail"]

    def level(self):
        return voice.read_ga(self.x, self.ga)["applied_sidetone"]

    def set_level(self, v):
        self.x.setu32(self.ga + 4 * voice.GA_FIELDS.index("sidetone"), v)

    def set_tail(self, name, v):
        self.x.setu32(voice.hs_addr_of(name), v)

    def muted(self):
        return self.hs()["pads"][self.pad]["muted"]

    def tap(self):
        """an injected Share tap: the mic mute toggles"""
        self.x.setu32(self.pn + 4 * (PN_INJECT + 1), self.pad)  # inject_pad
        self.x.setu32(self.pn + 4 * PN_INJECT, SHARE)
        time.sleep(STEP_S)
        self.x.setu32(self.pn + 4 * PN_INJECT, 0)
        time.sleep(SETTLE_S)

    def sent(self, t=None):
        t = t or self.tail()
        return voice.vol_bytes(t["dev_sent0"], t["dev_sent1"], 5)

    def report(self, t=None):
        t = t or self.tail()
        n = t["dev_report1"] & 0xFF
        return voice.vol_bytes(t["dev_report0"], t["dev_report1"], min(n, 7))

    def wait_report(self, pct, since):
        """the headset's answer: a report after `since` reports with sidetone == pct"""
        until = time.time() + ANSWER_S
        while True:
            t = self.tail()
            r = self.report(t)
            if t["dev_reports"] > since and len(r) >= 5 and r[4] & 0x7F == pct:
                return t, r
            if time.time() >= until:
                return t, r
            time.sleep(0.1)


def ask(hands, text):
    """a the owner check: with --hands, wait for y / n; without, a TODO line"""
    if not hands:
        print(f"TODO the owner check: {text}", flush=True)
        return None
    while True:
        a = input(f"OWNER CHECK: {text}\n  y / n? ").strip().lower()
        if a in ("y", "n"):
            return result(a == "y", "the owner: " + text.split(":")[0])


def set_and_check(c, level, want_pct, name):
    t0 = c.tail()
    c.set_level(level)
    time.sleep(SETTLE_S)
    t, r = c.wait_report(want_pct, t0["dev_reports"])
    s = c.sent(t)
    ok_sent = s[4] & 0x7F == want_pct
    ok_rep = len(r) >= 5 and r[4] & 0x7F == want_pct and t["dev_reports"] > t0["dev_reports"]
    result(ok_sent, f"{name}: sent sidetone {want_pct} %",
           f"sent {s[4]:02x} ({t['dev_sends'] - t0['dev_sends']} messages)")
    result(ok_rep, f"{name}: the headset reports sidetone {want_pct} % back",
           f"reports +{t['dev_reports'] - t0['dev_reports']}, last: {voice.vol_text(r)}")
    return ok_sent and ok_rep


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pad", type=int, default=0, choices=range(4))
    ap.add_argument("--hands", action="store_true")
    a = ap.parse_args()
    c = Console(a.pad)

    # 1. the build
    h = c.hs()
    if not result(h["version"] >= 3 and h["tail"] is not None, "XVHS version 3 (sidetone path)",
                  f"version {h['version']}"):
        info("install the `sidetone` build (xbox360/out/xw.xex) and run again")
        return finish()
    t = h["tail"]

    # 2. the headset and its report
    if not result(t["dev_pad"] == a.pad and t["dev_reports"] > 0,
                  "headset up, volume report seen",
                  f"pad {t['dev_pad'] if t['dev_pad'] != 0xFFFFFFFF else 'none'}, "
                  f"{t['dev_reports']} reports"):
        info("is the pad on and the headset plugged in? (xw.py status: 'headset ready')")
        return finish()
    rep = c.report(t)
    info(f"headset volume report ({t['dev_report1'] & 0xFF} bytes): {voice.vol_text(rep)}")
    info(f"we sent {t['dev_sends']}x, last {' '.join('%02x' % b for b in c.sent(t))}; "
         f"mode {voice.ST_MODE_NAMES.get(t['st_mode'])}, path "
         f"{voice.ST_PATHS.get(t['st_path'])}")

    # 3. writable?
    writable = len(rep) >= 5 and bool(rep[4] & 0x80)
    result(writable, "the headset marks SIDETONE writable",
           "auto mode will use the headset's own sidetone" if writable else
           "auto mode stays on software; step 4 forces the headset path to try it anyway")
    if os.environ.get("ACCEPT_READ_ONLY") == "1":
        info("read-only: stopping before any write")
        return finish()

    s0 = dict(level=c.level(), mode=t["st_mode"], bit7=t["dev_bit7"], muted=c.muted())
    if s0["muted"]:
        c.tap()
    try:
        # 4. the headset's own, level 5
        c.set_tail("st_mode", voice.ST_MODES["auto" if writable else "headset"])
        c.set_level(0)
        time.sleep(SETTLE_S)
        ok = set_and_check(c, 5, 50, "SIDETONE 5")
        result(c.tail()["st_path"] == 2, "path: the headset itself",
               voice.ST_PATHS.get(c.tail()["st_path"]))
        if not ok:
            info("not applied as sent: trying again with bit 7 set on the written fields")
            c.set_tail("dev_bit7", 1)
            c.set_level(0)
            time.sleep(SETTLE_S)
            if set_and_check(c, 5, 50, "SIDETONE 5 with bit 7"):
                info("the headset wants bit 7 set: make dev_bit7 = 1 the default (voice_360.c)")
            else:
                c.set_tail("dev_bit7", s0["bit7"])
                info("the headset ignores our sidetone field: keep mode software, or WP1.6")

        # 5. software silent meanwhile
        m0 = c.hs()["pads"][a.pad]["mixed"]
        time.sleep(1.0)
        p = c.hs()["pads"][a.pad]
        result(p["mixed"] == m0 and p["gain"] == 0, "software sidetone silent on the headset path",
               f"mixed +{p['mixed'] - m0}, gain {p['gain']}")

        ask(a.hands, "SIDETONE 5: talk. You hear your own voice in the headset at once, with "
                     "no echo (like a MixAmp's mic monitor)")

        # 6. 10 and 0
        set_and_check(c, 10, 100, "SIDETONE 10")
        set_and_check(c, 0, 0, "SIDETONE 0")
        result(c.tail()["st_path"] == 0, "level 0: path off", voice.ST_PATHS.get(c.tail()["st_path"]))

        # 7. mute
        c.set_level(5)
        time.sleep(SETTLE_S)
        t0 = c.tail()
        c.tap()
        time.sleep(SETTLE_S)
        t1 = c.tail()
        result(c.muted() and c.sent(t1)[4] & 0x7F == 0 and t1["st_path"] == 0,
               "muted: sidetone 0 % sent, path off",
               f"muted {c.muted()}, sent {c.sent(t1)[4]:02x}, path {t1['st_path']}")
        time.sleep(TOAST_S)
        c.tap()
        time.sleep(SETTLE_S)
        t2 = c.tail()
        result(not c.muted() and c.sent(t2)[4] & 0x7F == 50, "unmuted: 50 % again",
               f"sent {c.sent(t2)[4]:02x} ({t2['dev_sends'] - t0['dev_sends']} messages)")

        ask(a.hands, "Share + RB five times, then Share + LB five times: your voice gets "
                     "louder, then quieter, then gone (the panel shows the SIDETONE row)")
        c.set_level(5)
        time.sleep(SETTLE_S)
        ask(a.hands, "Share tap (MIC MUTED): your voice is gone from the headset; tap again "
                     "(MIC ON): it is back")
        if c.muted():
            time.sleep(TOAST_S)
            c.tap()

        # 8. the software path, for comparison
        c.set_tail("st_mode", voice.ST_MODES["software"])
        time.sleep(SETTLE_S)
        m0 = c.hs()["pads"][a.pad]["mixed"]
        time.sleep(1.5)
        t = c.tail()
        p = c.hs()["pads"][a.pad]
        result(c.sent(t)[4] & 0x7F == 0 and t["st_path"] == 1 and p["mixed"] > m0,
               "mode software: headset 0 %, our mix runs",
               f"sent {c.sent(t)[4]:02x}, path {t['st_path']}, mixed +{p['mixed'] - m0}, "
               f"gain {p['gain']}/32768, gaps {p['gaps']}, skips {p['skips']}, guard trips "
               f"{p['ducks']}")
        info(f"software sidetone delay estimate: {t['side_delay_ms']} ms (mic block 8 + loop "
             f"pass ~8 + FIFO + 5 OUT blocks 40); the headset's own: no USB delay")
        ask(a.hands, "for comparison, our software sidetone: talk for 5 s. Your voice comes "
                     "back late, like an echo")
    finally:
        # 9. put back
        c.set_tail("st_mode", s0["mode"])
        c.set_tail("dev_bit7", s0["bit7"])
        c.set_level(s0["level"])
        time.sleep(SETTLE_S)
        if c.muted() != s0["muted"]:
            time.sleep(TOAST_S)
            c.tap()
        t = c.tail()
        result(c.level() == s0["level"] and t["st_mode"] == s0["mode"] and
               c.muted() == s0["muted"], "settings put back",
               f"level {c.level()}, mode {t['st_mode']}, muted {c.muted()}")
    return finish()


def finish():
    print(f"{len(passed)} passed, {len(failed)} failed", flush=True)
    sys.exit(1 if failed or not passed else 0)


if __name__ == "__main__":
    main()
