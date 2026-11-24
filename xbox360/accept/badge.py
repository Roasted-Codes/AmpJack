#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for the persistent mute mark: while the mic is muted and nothing else
is on screen, a small dark-red dot and a grey MUTED (panel_360.c XWPN `badge`, hud_360.c
HUD_BADGE). The question is cost: the mark can stay up for a whole match, so it must not
take frame rate from the game or flicker (docs/ROADMAP.md Part 1, PANEL risks).

Run it with a headset in pad N's jack (the mark only shows for a live headset), drawing
allowed (hud.py stage 3), and best in a Halo 3 match (the soak is about that title's frame
rate). No hands needed: the Share taps are injected (XWPN inject).

  XBOX=10.0.0.13 python3 xbox360/accept/badge.py [--pad N] [--secs 600] [--idle 2,8]
                                                  [--no-shots]

  --secs S     the soak with the mark up (default 600: ten minutes)
  --idle A,B   XH36 badge_idle_ms values: the soak runs at A; each further value gets 60 s
               for comparison (INFO only: the owner picks)
  --no-shots   skip the screenshots of the five positions

Checks (one PASS or FAIL line each; exit status 1 if any failed):
  1. XWPN version 5 with badge 1 and badge_mode 0 (the defaults); XH36 version 12 or later.
  2. Baseline: 20 s with nothing shown and the mic on: swaps_per_s each second.
  3. A Share tap mutes: the toast, then within 2 s the mark (XWPN visible 3, XH36
     pub_visible 3), badge_shows + 1.
  4. Each badge_pos 0..4: a screenshot with the mark's sentinels at the place hud_360.c
     geometry() gives (accept/p1_panel.py badge_place); out/accept/badge/pos<N>.png.
  5. The soak: mean swaps_per_s at least the baseline's minus 1; pres_pct (the mark found in
     the picture being shown) mean 95 % or more; draws under 0.25 ms (XH36 d_ticks_max, and
     d_blit_us + d_flush_us of the redraws); the burn-in shift moves the mark by exactly one
     unit at a time, at least once a minute.
  6. badge_mode 1 (the fallback): up 2 s out of every 15 (between 8 and 20 % of samples).
  7. badge 0 hides it at once.
  8. A second tap unmutes: no mark. Everything put back as it was.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)

import hud  # noqa: E402
import p1_panel  # noqa: E402  (the mark's layout and geometry, mirrored)
import wp1_3 as wp  # noqa: E402  (Console: XWPN inject and reads)
from xa import xw_block  # noqa: E402

try:
    import shot  # noqa: E402
except ImportError:
    shot = None

BASE_S = 20
TB_PER_US = 50
failed = []


def result(ok, name, detail=""):
    print(f"{'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""), flush=True)
    if not ok:
        failed.append(name)
    return ok


def info(text):
    print(f"INFO {text}", flush=True)


class Mark(wp.Console):
    def __init__(self, pad):
        super().__init__(pad)
        self.xh = xw_block(hud.MAGIC)

    def hud(self):
        return hud.read_block(self.x, self.xh)

    def set_xh(self, name, v):
        hud.setctl(self.x, self.xh, name, v, quiet=True)


def sample(c, secs, every=1.0):
    """[XH36 dict] once a second for secs"""
    out = []
    end = time.time() + secs
    while time.time() < end:
        time.sleep(every)
        out.append(c.hud())
    return out


def mean(v):
    return sum(v) / len(v) if v else 0


def soak_numbers(rows):
    fps = [g["swaps_per_s"] for g in rows]
    pres = [g["pres_pct"] for g in rows]
    redraw = [g["d_blit_us"] + g["d_flush_us"] for g in rows]
    return fps, pres, redraw


def burn_in(rows):
    """the mark's sentinel 0 over the soak: (moves, bad moves, scale)"""
    moves, bad, last = 0, [], None
    f = max((g["d_scale"] for g in rows), default=0)
    for g in rows:
        p = (g["sent_x0"], g["sent_y0"])
        if g["sent_n"] != 4 or not p[0]:
            continue
        if last and p != last:
            moves += 1
            d = abs(p[0] - last[0]) + abs(p[1] - last[1])
            if d != f:
                bad.append((last, p))
        last = p
    return moves, bad, f


def shots(c, save):
    st0 = dict(p1_panel.BASE, visible=p1_panel.HUD_BADGE, badge=1, muted=1)
    pos0 = c.hud()["badge_pos"]
    try:
        for pos in range(5):
            c.set_xh("badge_pos", pos)
            o = why = fr = None
            for _ in range(3):
                time.sleep(0.6)
                fr, _hdr = shot.grab()
                o, why = p1_panel.find_canvas(fr, st0)
                if o:
                    break
            fr.png(os.path.join(save, f"pos{pos}.png"))
            if not result(o is not None, f"badge_pos {pos}: the mark found", why or ""):
                continue
            ex, ey = p1_panel.badge_place(fr.width, fr.height, o[2], pos)
            result((o[0], o[1]) == (ex, ey), f"badge_pos {pos}: where geometry() puts it",
                   f"at ({o[0]},{o[1]}), want ({ex},{ey}), scale {o[2]}, "
                   f"{fr.width}x{fr.height}")
    finally:
        c.set_xh("badge_pos", pos0)


def run(c, a):
    p, g = c.panel(), c.hud()
    result(p["version"] == wp.PN_VERSION and p["badge"] == 1 and p["badge_mode"] == 0,
           f"XWPN version {wp.PN_VERSION}, badge 1, badge_mode 0 (defaults)",
           f"version {p['version']}, badge {p['badge']}, badge_mode {p['badge_mode']}")
    if not result(g["version"] >= 12, "XH36 version 12 or later", str(g["version"])):
        return
    idles = [int(v) for v in a.idle.split(",") if v]
    info(f"title {g['sw_title']:08x}, {g['d_width']}x{g['d_height']} at the last draw; "
         f"badge_pos {g['badge_pos']}, badge_idle_ms {g['badge_idle_ms']}, idle_ms {g['idle_ms']}")

    # 2. baseline: nothing on screen
    base = [r["swaps_per_s"] for r in sample(c, BASE_S)]
    info(f"baseline, nothing shown: swaps_per_s mean {mean(base):.1f}, min {min(base)}, "
         f"max {max(base)}")

    # 3. mute: toast, then the mark
    s0 = c.panel()["badge_shows"]
    c.tap()
    result(c.muted() and c.panel()["visible"] == wp.TOAST, "Share tap: muted, toast",
           f"muted {c.muted()}, visible {c.panel()['visible']}")
    time.sleep(wp.TOAST_S + 0.5)
    p, g = c.panel(), c.hud()
    if not result(p["visible"] == wp.BADGE and g["pub_visible"] == wp.BADGE and
                  p["badge_shows"] == s0 + 1, "after the toast: the mark",
                  f"visible {p['visible']}, pub_visible {g['pub_visible']}, badge_shows "
                  f"+{p['badge_shows'] - s0} (0 with no live headset)"):
        return

    # 4. positions
    if not a.no_shots and shot is not None:
        save = os.path.join(TOOLS, "out", "accept", "badge")
        os.makedirs(save, exist_ok=True)
        shots(c, save)
        info(f"screenshots: {save}")

    # 5. the soak
    c.set_xh("badge_idle_ms", idles[0])
    c.set_xh("d_ticks_max", 0)
    time.sleep(1.5)
    rows = sample(c, a.secs)
    g = c.hud()
    fps, pres, redraw = soak_numbers(rows)
    result(mean(fps) >= mean(base) - 1, f"soak {a.secs} s at badge_idle_ms {idles[0]}: frame "
           "rate unchanged", f"swaps_per_s mean {mean(fps):.1f} (min {min(fps)}), baseline "
           f"{mean(base):.1f}")
    result(mean(pres) >= 95, "the mark is in the picture shown (pres_pct mean >= 95)",
           f"mean {mean(pres):.1f} %, min {min(pres)} %; gap max {g['gap_max_us']} us")
    dmax = g["d_ticks_max"] / TB_PER_US
    result(dmax < 250 and max(redraw) < 250, "draws under 0.25 ms",
           f"swap draws max {dmax:.0f} us; redraws (stores + flush) max {max(redraw)} us")
    moves, bad, f = burn_in(rows)
    result(not bad and moves >= max(1, a.secs // 60 - 1), "burn-in: one unit at a time, about "
           "once a minute", f"{moves} moves at scale {f}" + (f", bad {bad[:3]}" if bad else ""))
    for ms in idles[1:]:
        c.set_xh("badge_idle_ms", ms)
        time.sleep(1.5)
        fps2, pres2, redraw2 = soak_numbers(sample(c, 60))
        info(f"badge_idle_ms {ms}: swaps_per_s mean {mean(fps2):.1f}, pres_pct mean "
             f"{mean(pres2):.1f} % (min {min(pres2)}), redraw max {max(redraw2)} us")
    c.set_xh("badge_idle_ms", idles[0])

    # 6. 2 s of every 15
    c.set_pn("badge_mode", 1)
    up = n = 0
    end = time.time() + 32
    while time.time() < end:
        time.sleep(0.25)
        n += 1
        up += c.panel()["visible"] == wp.BADGE
    c.set_pn("badge_mode", 0)
    result(0.08 <= up / n <= 0.20, "badge_mode 1: up 2 s of every 15", f"{up} of {n} samples")

    # 7. off
    c.set_pn("badge", 0)
    time.sleep(0.2)
    result(c.panel()["visible"] == wp.HIDDEN, "badge 0: no mark", f"visible {c.panel()['visible']}")
    c.set_pn("badge", 1)
    time.sleep(0.2)

    # 8. unmute
    c.tap()
    time.sleep(wp.TOAST_S + 0.5)
    result(not c.muted() and c.panel()["visible"] == wp.HIDDEN, "second tap: unmuted, no mark",
           f"muted {c.muted()}, visible {c.panel()['visible']}")
    print("OWNER CHECK Tap Share in a Halo 3 match: is the small red MUTED mark there, steady, "
          "and not in your way? Which corner would you like it in (hud.py set badge_pos N: "
          "0 bottom centre, 1 bottom left, 2 bottom right, 3 top left, 4 top right)?", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pad", type=int, default=0, choices=range(4))
    ap.add_argument("--secs", type=int, default=600)
    ap.add_argument("--idle", default="2,8")
    ap.add_argument("--no-shots", action="store_true")
    a = ap.parse_args()
    try:
        c = Mark(a.pad)
    except SystemExit as e:
        result(False, "XWPN / XH36 blocks on the console", str(e))
        return 1
    p, g = c.panel(), c.hud()
    keep = {k: p[k] for k in ("badge", "badge_mode", "inject_pad", "notify")}
    keep_xh = {k: g[k] for k in ("badge_pos", "badge_idle_ms") if k in g}
    muted0 = c.muted()
    c.inject(0)
    c.set_pn("inject_pad", a.pad)
    c.set_pn("notify", 0)
    c.set_pn("badge", 1)
    c.set_pn("badge_mode", 0)
    if muted0:  # start from "mic on"
        c.tap()
    time.sleep(wp.TOAST_S + 0.3)
    try:
        run(c, a)
    finally:
        c.inject(0)
        for k, v in keep.items():
            c.set_pn(k, v)
        for k, v in keep_xh.items():
            c.set_xh(k, v)
        time.sleep(wp.TOAST_S + 0.3)
        if c.muted() != muted0:
            c.tap()
    print(f"{'FAILED' if failed else 'ALL PASSED'}"
          + (f" ({len(failed)}: {', '.join(failed)})" if failed else ""), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
