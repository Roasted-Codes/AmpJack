#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for the panel renderer (hud_panel.c): what it draws, checked by pixel
in xbdm screenshots, for hud_360.h version 6 (all 14 rows: the AUTO values, PRESET, CUSTOM's
BASS / MIDS / TREBLE, SURROUND, the team rows; the mic test's states; the preset description
in the help line while the cursor is on PRESET; and the persistent mute mark).

Run it with drawing allowed (hud.py stage 3), at a 720p (or 1080p) picture: Aurora or a
Halo 3 menu. No hands needed.

  XBOX=10.0.0.13 python3 xbox360/accept/p1_panel.py [--keep] [--only NAME,...]

  --keep   leave the last test state on screen (default: hud.py hide at the end)
  --only   run only the named states

For each state (each shown row selected at 8 rows, 9 and 11 rows, the AUTO values, a preset
name, both extremes, muted, the four mic test states, no headset, four toasts, and the mute
mark at each badge_pos) it runs `hud.py show` with the whole state, takes a screenshot
(shot.py), saves it as out/accept/p1_panel/<state>.png and checks:
  - the 4 sentinels are found together at scale 2 or more, at the positions hp_sentinels
    gives for that layout (this also fixes the canvas's origin and scale);
  - panel states: the accent bar is on the selected row's line (rows in hud_row_order) and
    on no other line; muted: the MIC MUTED box (HP_WARN) is there; not muted: it isn't;
    the help line is the panel's last line (its bottom padding is the panel's background,
    and no rule under it: the description area is gone) and nothing is drawn right of its
    36th character, even for a description too long for it ("desc_long"); CUSTOM: its bar's
    centre tick on each of its rows ("everything": all 14 rows at once);
  - the toasts: the dot is HP_WARN (muted, recording) or HP_ACCENT (on, playing); no
    headset: no dot;
  - the mute mark: its sentinels are on its grey text (shot.py searched with HP_MARK_TEXT),
    its dot is HP_MARK, and it sits where hud_360.c geometry() puts it for that badge_pos
    (the band between the 4 % and 10 % margins, centred or 5 % in from a side).
One PASS or FAIL line each; the exit status is 1 if any failed.

The layout numbers mirror hud_panel.h (HP_*); the colours mirror hud_panel.c's hp_palette;
ROW_ORDER mirrors hud_360.h's hud_row_order. If one changes, change it here too.

the owner check (not checked here): one look at the full panel in Halo 3, from his seat: "can
you read every row, and is the selected row obvious?"; at "desc_typical": "can you read
what the preset does in the bottom line?"; and at the mark: "is it there, steady, and out of
your way?"
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import hud  # noqa: E402

try:
    import shot  # noqa: E402
except ImportError:
    shot = None

# ---- hud_panel.h / hud_panel.c / hud_360.h ------------------------------------------------------
HP_W, HP_SMALL_W, HP_SMALL_H = 240, 120, 15
HP_TITLE_X, HP_TITLE_Y = 12, 5
HP_ROW0_Y, HP_ROW_PITCH = 20, 10
HP_BAR_X, HP_BAR_W, HP_BAR_H = 4, 2, 9
HP_MUTE_W, HP_MUTE_H = 59, 11
HP_TEXT_H = 7
HP_HELP_GAP, HP_PAD_BOTTOM, HP_HELP_COLS = 12, 5, 36
OLD_DESC_RULE = 20  # panel 5's description rule (under the help line): must not be drawn
HP_CBAR_MID = 80 + 79 // 2  # HP_CTRL_X + HP_CTRL_W / 2
HP_BADGE_W, HP_BADGE_H, HP_BADGE_X, HP_BADGE_Y, HP_BADGE_TEXT = 46, 11, 3, 1, 8
BADGE_SHIFT = [(0, 0), (1, 0), (1, 1), (0, 1)]
BG, LABEL, BRIGHT, DIM, ACCENT, WARN = 0x141414, 0xc8c8c8, 0xf4f4f4, 0x4a4a4a, 0x107c10, 0xc42b1c
MARK, MARK_TEXT = 0x8c1a10, 0x9a9a9a

HUD_TOAST, HUD_PANEL, HUD_BADGE = 1, 2, 3
(BALANCE, VOLUME, SIDETONE, MIC, GATE, TONE, TEAM, CHANNEL, MICTEST, PRESET,
 SURROUND, BASS, MIDS, TREBLE) = range(14)
ROW_ORDER = [BALANCE, VOLUME, SIDETONE, MIC, GATE, MICTEST, PRESET, BASS, MIDS, TREBLE, SURROUND,
             TEAM, CHANNEL, TONE]
CUSTOM_ROWS = (BASS, MIDS, TREBLE)
ROWS8 = 0x3F, 0x3   # rows, rows_hi: BALANCE .. TEST TONE; MIC TEST, PRESET
MT_IDLE, MT_REC, MT_PLAY, MT_PAUSED, MT_READY = range(5)


def text_w(s):
    return len(s) * 6 - 1


# hp_sentinels: the anchor text's first four lit pixels in scan order. In row 0 of the 5x7
# font: H = 10001, E = 11111, N = 10001, O = 01110, M = 10001, I = 01110, R = 11110,
# P = 11110, U = 10001.
SENT_OFFSETS = {"HEADSET": [(0, 0), (4, 0), (6, 0), (7, 0)],
                "NO HEADSET": [(0, 0), (4, 0), (7, 0), (8, 0)],
                "MIC MUTED": [(0, 0), (4, 0), (7, 0), (8, 0)],
                "MIC ON": [(0, 0), (4, 0), (7, 0), (8, 0)],
                "REC": [(0, 0), (1, 0), (2, 0), (3, 0)],
                "PLAYING": [(0, 0), (1, 0), (2, 0), (3, 0)],
                "MUTED": [(0, 0), (4, 0), (6, 0), (10, 0)]}


def toast_text(st):
    """hud_panel.c layout(): a toast shows the mic test only while it records or plays"""
    if not st["flash"] and st["mictest"] == MT_REC:
        return f"REC {st['mictest_secs']} SEC"
    if not st["flash"] and st["mictest"] == MT_PLAY:
        return f"PLAYING {st['mictest_pct']}%"
    return "MIC MUTED" if st["muted"] else "MIC ON"


def anchor(st):
    """(text, sentinel key, x, y) of the layout's anchor: hud_panel.c layout()"""
    if st["visible"] == HUD_BADGE:
        dx, dy = BADGE_SHIFT[st["badge_shift"] & 3]
        return "MUTED", "MUTED", HP_BADGE_X + dx + HP_BADGE_TEXT, HP_BADGE_Y + dy
    if st["visible"] == HUD_PANEL and st["headset"]:
        return "HEADSET", "HEADSET", HP_TITLE_X, HP_TITLE_Y
    dw = 9 if st["visible"] == HUD_TOAST else 0
    s = "NO HEADSET" if st["visible"] == HUD_PANEL else toast_text(st)
    return s, s.split()[0] if s.split()[0] in SENT_OFFSETS else s, \
        (HP_SMALL_W - dw - text_w(s)) // 2 + dw, (HP_SMALL_H - HP_TEXT_H) // 2


def badge_place(W, H, f, pos):
    """hud_360.c geometry() for HP_ANCHOR_BADGE: the canvas's top-left pixel"""
    w, h = HP_BADGE_W * f, HP_BADGE_H * f
    m4 = H * 4 // 100
    band = H // 10 - m4
    pos = 0 if pos > 4 else pos
    x0 = W // 20 if pos in (1, 3) else W - W // 20 - w if pos in (2, 4) else (W - w) // 2
    y0 = m4 + (band - h) // 2 if h < band else m4
    if pos < 3:
        y0 = H - y0 - h
    return x0, y0


# ---- the states ---------------------------------------------------------------------------------
# hud.py show keys = struct hud_state v6 field names; every state sets all of them
SHOW_FIELDS = ("visible", "muted", "flash", "headset", "game_on", "cursor", "rows", "rows_hi",
               "balance", "volume", "sidetone", "mic_level", "mic_auto_level", "gate", "tone",
               "mic_vu", "gate_open", "team_level", "channel", "team_peers", "team_talking",
               "mictest", "mictest_secs", "mictest_pct", "mictest_max", "preset",
               "preset_count", "preset_name", "surround", "surround_live", "badge",
               "badge_shift", "preset_flags", "custom_bass", "custom_mids", "custom_treble",
               "desc")
BASE = dict(visible=HUD_PANEL, muted=0, flash=0, headset=1, game_on=1, cursor=BALANCE,
            rows=ROWS8[0], rows_hi=ROWS8[1], balance=-2, volume=7, sidetone=3, mic_level=5,
            mic_auto_level=0, gate=1, tone=0, mic_vu=4, gate_open=1, team_level=0, channel=0,
            team_peers=0, team_talking=0, mictest=MT_IDLE, mictest_secs=0, mictest_pct=0,
            mictest_max=30, preset=0, preset_count=0, preset_name="", surround=0,
            surround_live=0, badge=0, badge_shift=0, preset_flags=0, custom_bass=0,
            custom_mids=0, custom_treble=0, desc="")
ALL11 = dict(rows=ROWS8[0] | 1 << TEAM | 1 << CHANNEL, rows_hi=0x7, surround=1, surround_live=1,
             team_level=6, channel=3, team_peers=3, team_talking=2)
# CUSTOM selected (its three rows); a shipped description, and one too long for the line in
# the widest letter (tests/test_panel.c WORST_WORDS: drawn cut to its first word)
WORST_DESC = "W" * 18 + " " + "W" * 18 + " " + "W" * 25
TYPICAL_DESC = "ALL-ROUNDER: PUNCH AND CLEAR STEPS"
CUSTOM = dict(rows_hi=ROWS8[1] | 0x38, preset_flags=1, preset=30, preset_count=29,
              preset_name="CUSTOM", custom_bass=3, custom_mids=0, custom_treble=-2)


def rows_of(st):
    return st["rows"] | st["rows_hi"] << 8


STATES = [(f"row{r}", dict(BASE, cursor=r)) for r in ROW_ORDER if rows_of(BASE) >> r & 1]
STATES += [
    ("rows9_surround", dict(BASE, rows_hi=0x7, surround=1, surround_live=1, cursor=SURROUND)),
    ("rows11_team", dict(BASE, **ALL11, cursor=TEAM)),
    ("auto_values", dict(BASE, **ALL11, cursor=MIC, mic_level=0, mic_auto_level=6, gate=3)),
    ("preset_name", dict(BASE, **ALL11, cursor=PRESET, preset=3, preset_count=29,
                         preset_name="HALO FOOTSTEPS")),
    ("desc_typical", dict(BASE, rows_hi=0x7, cursor=PRESET, preset=2, preset_count=9,
                          preset_name="OMNIVOX", desc=TYPICAL_DESC)),
    ("custom", dict(BASE, **CUSTOM, cursor=BASS)),
    # all 14 rows (rows_hi: MIC TEST, PRESET, SURROUND, BASS, MIDS, TREBLE)
    ("everything", {**BASE, **ALL11, **CUSTOM, "rows_hi": 0x3F, "cursor": TREBLE}),
    ("desc_long", {**BASE, **ALL11, **CUSTOM, "rows_hi": 0x3F, "cursor": PRESET,
                   "desc": WORST_DESC}),
    ("min", dict(BASE, cursor=BALANCE, balance=-5, volume=0, sidetone=0, mic_level=1, gate=0,
                 mic_vu=0, gate_open=0)),
    ("max", dict(BASE, cursor=TONE, balance=5, volume=10, sidetone=10, mic_level=10, gate=2,
                 tone=2, mic_vu=10)),
    ("muted", dict(BASE, cursor=SIDETONE, muted=1)),
    ("mt_recording", dict(BASE, cursor=MICTEST, mictest=MT_REC, mictest_secs=7, mic_vu=6)),
    ("mt_playing", dict(BASE, cursor=MICTEST, mictest=MT_PLAY, mictest_secs=12, mictest_pct=40)),
    ("mt_paused", dict(BASE, cursor=MICTEST, mictest=MT_PAUSED, mictest_secs=12, mictest_pct=40)),
    ("mt_ready", dict(BASE, cursor=MICTEST, mictest=MT_READY, mictest_secs=12)),
    ("no_headset", dict(BASE, headset=0)),
    ("toast_muted", dict(BASE, visible=HUD_TOAST, flash=1, muted=1)),
    ("toast_on", dict(BASE, visible=HUD_TOAST, flash=1, muted=0)),
    ("toast_rec", dict(BASE, visible=HUD_TOAST, mictest=MT_REC, mictest_secs=7)),
    ("toast_playing", dict(BASE, visible=HUD_TOAST, mictest=MT_PLAY, mictest_pct=40)),
]
# the mute mark at each badge_pos (the hud.py control), and one burn-in step
BADGES = [(f"badge_pos{p}", p, dict(BASE, visible=HUD_BADGE, badge=1, muted=1)) for p in range(5)]
BADGES += [("badge_shift2", 0, dict(BASE, visible=HUD_BADGE, badge=1, muted=1, badge_shift=2))]

SETTLE_S = 0.6   # hud.py show -> the next drawn frame
TRIES = 3        # screenshots per state before the sentinels count as missing
TOL = 8          # per 8-bit lane

failed = []


def result(ok, name, detail=""):
    print(f"{'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""), flush=True)
    if not ok:
        failed.append(name)
    return ok


def hud_py(*args):
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "hud.py"), *map(str, args)],
                       cwd=TOOLS, capture_output=True, text=True, timeout=60)
    return r.returncode, (r.stdout + r.stderr).strip()


def colour_is(fr, x, y, rgb):
    """pixel (x, y) is rgb, within TOL per lane"""
    if not (0 <= x < fr.width and 0 <= y < fr.height):
        return False
    shift = fr.bits - 8
    got = fr.rgb(x, y)
    want = (rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb & 0xff)
    return all(abs((g >> shift) - w) <= TOL for g, w in zip(got, want))


def find_canvas(fr, st):
    """(origin x, origin y, scale) from the sentinels, or (None, reason)"""
    bases = (MARK_TEXT,) if st["visible"] == HUD_BADGE else shot.PANEL_TEXT
    found = shot.find_sentinels(fr, bases=bases)
    pick = shot.best_set(found)
    if not pick:
        return None, "sentinels not found (" + ", ".join(f"{k}: {len(v)}" for k, v in
                                                         found.items()) + " candidates)"
    f = pick[0][2]
    if f < 2:
        return None, f"sentinels only at scale 1 ({pick}): not conclusive"
    text, key, ax, ay = anchor(st)
    offs = SENT_OFFSETS[key]
    ox, oy = pick[0][0] - (ax + offs[0][0]) * f, pick[0][1] - (ay + offs[0][1]) * f
    for k, (x, y, _) in enumerate(pick):
        ex, ey = ox + (ax + offs[k][0]) * f, oy + (ay + offs[k][1]) * f
        if (x, y) != (ex, ey):
            return None, (f"sentinel {k} at ({x},{y}), expected ({ex},{ey}) for {text!r}: "
                          f"another layout?")
    return (ox, oy, f), None


def unit_px(o, ux, uy):
    """the centre pixel of canvas unit (ux, uy) (never a sentinel's top-left pixel)"""
    ox, oy, f = o
    return ox + ux * f + f // 2, oy + uy * f + f // 2


def check_panel(name, fr, o, st):
    rows = rows_of(st)
    lines = [r for r in ROW_ORDER if rows >> r & 1]
    bar = []
    for i, r in enumerate(lines):
        y = HP_ROW0_Y + i * HP_ROW_PITCH + 3
        x, py = unit_px(o, HP_BAR_X, y)
        if colour_is(fr, x, py, ACCENT):
            bar.append(r)
        elif not colour_is(fr, x, py, BG):
            result(False, f"{name} accent bar", f"line {i} ({x},{py}) is {fr.rgb(x, py)}, "
                   "neither the accent nor the background")
            return
    result(bar == [st["cursor"]], f"{name} accent bar",
           f"on row(s) {bar}, selected {st['cursor']} ({len(lines)} rows)")
    y_last = HP_ROW0_Y + (len(lines) - 1) * HP_ROW_PITCH
    for i, r in enumerate(lines):
        if r in CUSTOM_ROWS:
            x, y = unit_px(o, HP_CBAR_MID, HP_ROW0_Y + i * HP_ROW_PITCH + 3)
            result(colour_is(fr, x, y, LABEL), f"{name} {('BASS', 'MIDS', 'TREBLE')[r - BASS]} "
                   "bar centre", f"({x},{y}) is {fr.rgb(x, y)}")
    # the help line is the last line: the panel's background in its bottom padding, no
    # rule under it (panel 5's description area), nothing right of its 36th character
    x, y = unit_px(o, HP_TITLE_X - 2, y_last + HP_HELP_GAP + HP_TEXT_H + HP_PAD_BOTTOM - 2)
    result(colour_is(fr, x, y, BG), f"{name} help line is the panel's last",
           f"bottom padding at ({x},{y}) is {fr.rgb(x, y)}")
    x, y = unit_px(o, HP_TITLE_X + 1, y_last + OLD_DESC_RULE)
    result(not colour_is(fr, x, y, DIM), f"{name} no description area",
           f"({x},{y}) is {fr.rgb(x, y)}")
    lit = [ux for ux in range(HP_TITLE_X + 6 * HP_HELP_COLS, HP_W - 1) for uy in range(HP_TEXT_H)
           if not colour_is(fr, *unit_px(o, ux, y_last + HP_HELP_GAP + uy), BG)]
    result(not lit, f"{name} help line inside the panel",
           f"{len(lit)} units lit right of character {HP_HELP_COLS}")
    x, y = unit_px(o, HP_W - HP_TITLE_X - HP_MUTE_W + 1, HP_TITLE_Y - 1)
    box = colour_is(fr, x, y, WARN)
    result(box == bool(st["muted"]), f"{name} mute box",
           f"{'shown' if box else 'not shown'} at ({x},{y}), muted {st['muted']}")


def check_small(name, fr, o, st):
    text, _key, ax, ay = anchor(st)
    x, y = unit_px(o, ax - 7, ay + 3)
    if st["visible"] == HUD_TOAST:
        if text.startswith("REC"):
            want = WARN
        elif text.startswith("PLAYING"):
            want = ACCENT
        else:
            want = WARN if st["muted"] else ACCENT
        result(colour_is(fr, x, y, want), f"{name} dot",
               f"({x},{y}) is {fr.rgb(x, y)}, want {want:06x} ({text})")
    else:
        result(colour_is(fr, x, y, BG), f"{name} no dot", f"({x},{y}) is {fr.rgb(x, y)}")


def check_badge(name, fr, o, st, pos):
    ox, oy, f = o
    dx, dy = BADGE_SHIFT[st["badge_shift"] & 3]
    x, y = unit_px(o, HP_BADGE_X + dx + 2, HP_BADGE_Y + dy + 3)
    result(colour_is(fr, x, y, MARK), f"{name} dot", f"({x},{y}) is {fr.rgb(x, y)}, want "
           f"{MARK:06x}")
    ex, ey = badge_place(fr.width, fr.height, f, pos)
    result((ox, oy) == (ex, ey), f"{name} position",
           f"canvas at ({ox},{oy}), geometry() puts it at ({ex},{ey}) for badge_pos {pos} on "
           f"{fr.width}x{fr.height} at scale {f}")


def run_state(name, st, save, pos=None):
    args = [f"{k}={st[k]}" for k in SHOW_FIELDS]
    rc, out = hud_py("show", *args)
    if not result(rc == 0, f"{name} show", "" if rc == 0 else out.splitlines()[-1]):
        return
    o = why = fr = None
    for _ in range(TRIES):
        time.sleep(SETTLE_S)
        fr, _hdr = shot.grab()
        o, why = find_canvas(fr, st)
        if o:
            break
    fr.png(os.path.join(save, f"{name}.png"))
    if not result(o is not None, f"{name} sentinels",
                  why or f"origin ({o[0]},{o[1]}), scale {o[2]}"):
        return
    if st["visible"] == HUD_BADGE:
        check_badge(name, fr, o, st, pos)
    elif st["visible"] == HUD_PANEL and st["headset"]:
        check_panel(name, fr, o, st)
    else:
        check_small(name, fr, o, st)


def main():
    keep = "--keep" in sys.argv[1:]
    only = None
    if "--only" in sys.argv[1:]:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    if shot is None:
        result(False, "shot.py", "not found")
        return
    missing = [k for k in SHOW_FIELDS if k not in getattr(hud, "SHOW_KEYS", {})]
    if not result(not missing, "hud.py show keys",
                  f"missing {', '.join(missing)}" if missing else ""):
        return
    rc, out = hud_py("status")
    if not result(rc == 0 and "VdSwap hooked" in out, "hud hooked", out.splitlines()[0] if out
                  else f"exit {rc}"):
        return
    pos0 = next((ln.split("badge_pos ")[1].split(")")[0] for ln in out.splitlines()
                 if "badge_pos " in ln), "0")
    save = os.path.join(TOOLS, "out", "accept", "p1_panel")
    os.makedirs(save, exist_ok=True)
    try:
        for name, st in STATES:
            if not only or name in only:
                run_state(name, st, save)
        for name, pos, st in BADGES:
            if only and name not in only:
                continue
            rc, out = hud_py("set", "badge_pos", pos)
            if result(rc == 0, f"{name} badge_pos {pos}", "" if rc == 0 else out):
                run_state(name, st, save, pos)
    finally:
        hud_py("set", "badge_pos", pos0)
        if not keep:
            hud_py("hide")
    print(f"screenshots: {save}")


if __name__ == "__main__":
    main()
    print(f"{'FAIL' if failed else 'PASS'} p1_panel" + (f": {', '.join(failed)}" if failed else ""))
    sys.exit(1 if failed else 0)
