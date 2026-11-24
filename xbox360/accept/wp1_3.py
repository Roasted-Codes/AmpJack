#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for WP1.3, "Controls and XAM menu removal" (docs/PLAN.md).

No hands needed: it drives the Share button and the D-pad over xbdm through the panel's
`inject` word (XWPN block, panel_360.c) and reads XWPN, XVGA (game mix), XWDG (mic level, gate,
tone), XVHS (mute) and XH36 (what the bar was told). Run it with the WP1.3 xw.xex loaded, at
idle Aurora; a pad does not have to be on (if one is, the title also gets the injected input):

  XBOX=10.0.0.13 python3 xbox360/accept/wp1_3.py [--pad N] [--offer] [--offer-custom]

  --pad N   the pad slot the input is injected into (default 0)
  --offer   XW_DEV builds: XWPN features_force = all, so the SURROUND, TEAM VOL / TEAM CH rows
            and the AUTO values are walked even before their features land (put back after)
  --offer-custom  XW_DEV builds: also features_force 0x40, so CUSTOM's BASS / MIDS / TREBLE
            rows are walked (presses counted) whatever the equalizer offers

Side effects, all undone at the end (balance, volume, sidetone, mic level, gate, tone and mute
are put back as they were): the mic mute toggles twice; the mix moves a step and back; the
test tone plays for about half a second (very quiet, x master); and with Share up an injected
D-pad Up then Down reaches the title (Aurora's selection moves up and back).

Checks (one PASS or FAIL line each; the exit status is 1 if any failed):
  - out/xw.exe has no XamShowMessageBoxUI (the XAM menu is gone);
  - the running build has the XWPN block, version 5 (the mute mark is switched off for the
    run and put back: accept/badge.py tests it);
  - a Share tap toggles the mute (XVHS follows), shows the toast, and a second tap undoes it;
  - a 300 ms hold shows the panel (XWPN visible 2, cursor on BALANCE; XH36 pub_visible set);
  - Left x3 gives balance -3 and Right x3 brings it back to 0;
  - Down moves the cursor to VOLUME and the D-pad is kept from the title while Share is down;
  - the rows shown follow XWPN `features`, and Down walks them in hud_row_order (BALANCE,
    VOLUME, SIDETONE, MIC VOLUME, NOISE GATE, PRESET, [BASS, MIDS, TREBLE], [SURROUND], [TEAM
    VOL, TEAM CH], TEST TONE, MIC TEST), stopping on the last;
  - volume, sidetone and mic level move one step and back; NOISE GATE steps in the panel's
    order (AUTO when offered, OFF, LOW, HIGH); PRESET held Right repeats about 8 a second
    after 0.4 s; SURROUND / TEAM rows count their presses; MIC VOLUME AUTO (when offered)
    leaves the manual gain alone;
  - Up / Down move the cursor; Right on TEST TONE turns the tone on;
  - releasing Share hides the panel, switches the tone off and tells the mic test once
    (mictest_closes);
  - with Share up, the D-pad reaches the title;
  - the next Share press starts on BALANCE again;
  - a 2 s hold and release changes nothing (no mute, no setting).

Standalone: it imports only xa.py, xw.py, voice.py and hud.py (next to this folder), so
accept/run.py (WP0.2) can run it as a script.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import hud  # noqa: E402
import voice  # noqa: E402
import xw  # noqa: E402
from xa import Xbdm, xw_block  # noqa: E402

# struct xw_panel_status (panel_360.c), all u32; _Static_assert there: 46 words
PN_MAGIC = 0x5857504E  # "XWPN"
PN_VERSION = 5  # 2: adjusts[9] (MIC TEST, row 8); 3: rows, features, the mute mark; 4: CUSTOM;
# 5: no description area (the help line shows the description): the last word unused, 0
PN_FIELDS = ("magic version inject inject_pad notify tap_ms hold_ms visible cursor share_down "
             "taps holds " + " ".join(f"adjusts{i}" for i in range(9)) + " "
             "masked_presses tone_auto_offs title_buttons0 title_buttons1 title_buttons2 "
             "title_buttons3 panel_pad publishes empty_holds inject_timeouts bumpers "
             "adjusts9 adjusts10 rows features features_force badge badge_mode badge_shows "
             "repeats mictest_closes "
             "adjusts11 adjusts12 adjusts13 unused45").split()  # adjusts_custom[3]
assert len(PN_FIELDS) == 46
HIDDEN, TOAST, PANEL, BADGE = 0, 1, 2, 3
ROWS = ["balance", "volume", "sidetone", "mic volume", "noise gate", "test tone", "team vol",
        "team ch", "mic test", "preset", "surround", "bass", "mids", "treble"]
(BALANCE, VOLUME, SIDETONE, MIC, GATE, TONE, TEAM, CHANNEL, MICTEST, PRESET, SURROUND, BASS, MIDS,
 TREBLE) = range(14)
CUSTOM_ROWS = {BASS, MIDS, TREBLE}
# hud_360.h hud_row_order: the order rows are drawn and the cursor walks
ROW_ORDER = [BALANCE, VOLUME, SIDETONE, MIC, GATE, MICTEST, PRESET, BASS, MIDS, TREBLE, SURROUND,
             TEAM, CHANNEL, TONE]
# panel_360.c PN_F_*: XWPN features (what the rows offer), features_force (XW_DEV builds);
# F_CUSTOM_ROWS is features_force only (0x80, version 4's test description, is ignored)
F_SURROUND, F_MIC_AUTO, F_GATE_AUTO, F_TEAM, F_PRESETS = 1, 2, 4, 8, 16
F_CUSTOM, F_CUSTOM_ROWS = 0x20, 0x40
# diag_360.h XD_GATE_* and the panel's order for them (panel_360.c GATE_ORDER)
G_OFF, G_1, G_2, G_AUTO, G_3, G_4 = range(6)
GATE_ORDER = [G_AUTO, G_OFF, G_1, G_2, G_3, G_4]
SHARE, UP, DOWN, LEFT, RIGHT = 1, 2, 4, 8, 16
# GIP buttons (core/gip.h)
GIP_DPAD_U, GIP_DPAD_D, GIP_DPAD_L, GIP_DPAD_R = 0x100, 0x200, 0x400, 0x800
GIP_DPAD = GIP_DPAD_U | GIP_DPAD_D | GIP_DPAD_L | GIP_DPAD_R

STEP_S = 0.08    # each injected press and release (the driver thread looks every ~2 ms)
SETTLE_S = 0.15  # after a change, for xd_poll / xg_poll to apply it
TOAST_S = 1.5

failed = []


def result(ok, name, detail=""):
    print(f"{'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""), flush=True)
    if not ok:
        failed.append(name)
    return ok


def info(text):
    print(f"INFO {text}", flush=True)


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


class Console:
    def __init__(self, pad):
        self.x = Xbdm()
        self.pad = pad
        self.pn = xw_block(PN_MAGIC)
        self.ga = voice.ga_addr()
        self.dg = xw.diag_addr()

    # -- reads
    def panel(self):
        w = self.x.u32s(self.pn, len(PN_FIELDS))
        if not w or w[0] != PN_MAGIC:
            raise SystemExit("XWPN block unreadable")
        return dict(zip(PN_FIELDS, w))

    def mix(self):
        g = voice.read_ga(self.x, self.ga)
        if not g:
            raise SystemExit("XVGA block unreadable")
        return dict(balance=s32(g["applied_balance"]), volume=g["applied_master"],
                    sidetone=g["applied_sidetone"])

    def diag(self):
        d = xw.read_diag(self.x)
        if not d:
            raise SystemExit("XWDG block unreadable (or a version xw.py doesn't know)")
        return d

    def muted(self):
        h = voice.read_hs(self.x)
        if not h:
            raise SystemExit("XVHS block unreadable")
        return h["pads"][self.pad]["muted"]

    def settings(self):
        m, d = self.mix(), self.diag()
        return dict(m, mic=d["applied_mic_gain"], gate=d["applied_gate"], tone=d["applied_tone"],
                    muted=self.muted())

    def pub_visible(self):
        return hud.read_block(self.x, xw_block(hud.MAGIC))["pub_visible"]

    # -- writes
    def set_pn(self, name, v):
        self.x.setu32(self.pn + 4 * PN_FIELDS.index(name), v & 0xFFFFFFFF)

    def set_ga(self, name, v):
        self.x.setu32(self.ga + 4 * voice.GA_FIELDS.index(name), v & 0xFFFFFFFF)

    def set_dg(self, name, v):
        self.x.setu32(self.dg + 4 * xw.DIAG_FIELDS.index(name), v & 0xFFFFFFFF)

    def inject(self, bits):
        self.set_pn("inject", bits)

    # -- gestures
    def tap(self):
        self.inject(SHARE)
        time.sleep(STEP_S)
        self.inject(0)
        time.sleep(SETTLE_S)

    def press(self, bits):
        """one D-pad press with Share held (Share stays down after it)"""
        self.inject(SHARE | bits)
        time.sleep(STEP_S)
        self.inject(SHARE)
        time.sleep(STEP_S)


def row_value(c, row):
    if row == VOLUME:
        return c.mix()["volume"]
    if row == SIDETONE:
        return c.mix()["sidetone"]
    d = c.diag()
    return {MIC: d["applied_mic_gain"], GATE: d["applied_gate"], TONE: d["applied_tone"]}[row]


ROW_MAX = {VOLUME: 100, SIDETONE: 10, MIC: 560, TONE: 2}
ROW_STEP = {VOLUME: 10, SIDETONE: 1}  # MIC: the next value xd_poll accepts


def adjusts(c, row):
    return c.panel()["adjusts%d" % row]


def walk_to(c, row):
    """Up / Down (Share held) until the cursor is on `row` (hud_row_order); True if it got there"""
    for _ in range(len(ROW_ORDER)):
        cur = c.panel()["cursor"]
        if cur == row:
            return True
        if cur not in ROW_ORDER or row not in ROW_ORDER:
            return False
        c.press(DOWN if ROW_ORDER.index(row) > ROW_ORDER.index(cur) else UP)
    return c.panel()["cursor"] == row


def gate_check(c, auto_offered):
    """NOISE GATE: Left / Right step through AUTO, OFF, 1, 2, 3, 4 (AUTO only when offered);
    the stored numbers stay XD_GATE_*. Puts the gate back."""
    g0 = c.diag()["applied_gate"]
    for _ in range(6):
        c.press(LEFT)
    time.sleep(SETTLE_S)
    seen = [c.diag()["applied_gate"]]
    for _ in range(5):
        c.press(RIGHT)
        time.sleep(SETTLE_S)
        seen.append(c.diag()["applied_gate"])
    want = GATE_ORDER if auto_offered else GATE_ORDER[1:] + [G_4]
    result(seen == want, "noise gate: Left to the end, then Right x5, in the panel's order"
           + (" (AUTO offered)" if auto_offered else ""), f"{seen}, want {want}")
    while c.diag()["applied_gate"] != g0 and len(seen) < 12:
        c.press(LEFT)
        time.sleep(SETTLE_S)
        seen.append(c.diag()["applied_gate"])
    result(c.diag()["applied_gate"] == g0, "noise gate put back", str(c.diag()["applied_gate"]))


def preset_check(c, f):
    """PRESET: one press, then holding Right repeats ~8 a second after 0.4 s (adjusts and
    repeats counted; the preset itself is eq.py's to read). Back to FLAT at the end."""
    a0, r0 = adjusts(c, PRESET), c.panel()["repeats"]
    c.inject(SHARE | RIGHT)
    time.sleep(1.0)
    c.inject(SHARE)
    time.sleep(STEP_S)
    p = c.panel()
    reps = p["repeats"] - r0
    result(3 <= reps <= 7, "preset: Right held 1 s repeats (8 a second after 0.4 s)",
           f"{reps} repeats")
    result(p["adjusts9"] - a0 == reps + 1, "preset: every step counted",
           f"adjusts +{p['adjusts9'] - a0}")
    time.sleep(0.5)
    result(c.panel()["repeats"] == p["repeats"], "preset: released, repeats stop")
    # back to FLAT: Left held long enough for any number of presets (clamped at FLAT)
    c.inject(SHARE | LEFT)
    time.sleep(0.4 + (64 / 8 if f & F_PRESETS else 0.3))
    c.inject(SHARE)
    time.sleep(STEP_S)
    if f & F_PRESETS:
        info("presets loaded: PRESET left on FLAT")


def step_back_counted(c, row):
    """a row whose value has no reader here yet (SURROUND, TEAM VOL, TEAM CH): Right, Left,
    both counted"""
    a0 = adjusts(c, row)
    c.press(RIGHT)
    c.press(LEFT)
    result(adjusts(c, row) - a0 == 2, f"{ROWS[row]}: Right and Left counted",
           f"+{adjusts(c, row) - a0}")


def mic_auto_check(c):
    """MIC VOLUME AUTO (offered): Left from 1 goes to AUTO without touching mic_gain (Right
    from AUTO then returns to it). Whether AUTO is on is MIC-AUTO's block to say (micauto.py);
    tests/test_panel.c checks the panel's side of it."""
    m0 = c.diag()["applied_mic_gain"]
    c.set_dg("mic_gain", 25)
    time.sleep(SETTLE_S)
    a0 = adjusts(c, MIC)
    c.press(LEFT)
    time.sleep(SETTLE_S)
    m1 = c.diag()["applied_mic_gain"]
    result(m1 == 25 and adjusts(c, MIC) == a0 + 1, "mic volume: Left from 1 (AUTO) keeps the "
           "manual gain", f"mic_gain {m1}")
    c.press(RIGHT)
    time.sleep(SETTLE_S)
    info(f"mic volume: Right after it: mic_gain {c.diag()['applied_mic_gain']} (25 once AUTO "
         "is implemented: the last manual step; 35 with the stub, which never enters AUTO)")
    c.set_dg("mic_gain", m0)
    time.sleep(SETTLE_S)


def step_and_back(c, row):
    """with the cursor on `row`: one step (Right, or Left at the top) and back"""
    a0 = c.panel()["adjusts%d" % row]
    v0 = row_value(c, row)
    up = v0 < ROW_MAX[row]
    first, back = (RIGHT, LEFT) if up else (LEFT, RIGHT)
    c.press(first)
    time.sleep(SETTLE_S)
    v1 = row_value(c, row)
    want = v0 + (1 if up else -1) * ROW_STEP[row] if row in ROW_STEP else None
    ok = v1 != v0 and (want is None or v1 == want) and (v1 > v0) == up
    result(ok, f"{ROWS[row]}: {'Right' if up else 'Left'} steps it",
           f"{v0} -> {v1}" + (f" (expected {want})" if want is not None else ""))
    c.press(back)
    time.sleep(SETTLE_S)
    v2 = row_value(c, row)
    result(v2 == v0, f"{ROWS[row]}: {'Left' if up else 'Right'} steps it back", f"{v1} -> {v2}")
    a1 = c.panel()["adjusts%d" % row]
    result(a1 - a0 == 2, f"{ROWS[row]}: adjusts counted", f"+{a1 - a0}")


def restore(c, s0):
    c.inject(0)
    time.sleep(SETTLE_S)
    c.set_ga("balance", s0["balance"])
    c.set_ga("master", s0["volume"])
    c.set_ga("sidetone", s0["sidetone"])
    c.set_dg("mic_gain", s0["mic"])
    c.set_dg("gate", s0["gate"])
    c.set_dg("tone", s0["tone"])
    time.sleep(TOAST_S + 0.3)  # let any toast run out, so a mute fix-up tap is a fresh press
    if c.muted() != s0["muted"]:
        c.tap()
    time.sleep(SETTLE_S)
    s1 = c.settings()
    result(s1 == s0, "settings put back", f"{s1}" if s1 != s0 else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pad", type=int, default=0, choices=range(4))
    ap.add_argument("--offer", action="store_true",
                    help="XW_DEV builds: offer every row and value (XWPN features_force) so "
                         "SURROUND, TEAM and the AUTO values are walked against the stubs")
    ap.add_argument("--offer-custom", action="store_true",
                    help="XW_DEV builds: CUSTOM's rows shown (features_force 0x40) whatever "
                         "the equalizer offers")
    a = ap.parse_args()
    pad = a.pad

    # 1. the build: no XAM menu
    exe = os.path.join(TOOLS, "out", "xw.exe")
    with open(exe, "rb") as f:
        img = f.read()
    result(b"XamShowMessageBoxUI" not in img, "no XamShowMessageBoxUI in out/xw.exe")
    result(b"Headset diagnostics" not in img and b"diag: menu" not in img,
           "no diagnostics menu strings in out/xw.exe")

    try:
        c = Console(pad)
    except SystemExit as e:
        result(False, "XWPN block on the console", str(e))
        return 1
    p = c.panel()
    if not result(p["version"] == PN_VERSION, "XWPN version", str(p["version"])):
        return 1
    c.inject(0)
    c.set_pn("inject_pad", pad)
    c.set_pn("notify", 0)
    # the mute mark has its own script (accept/badge.py): off here, so "hidden" means hidden
    badge0, force0 = p["badge"], p["features_force"]
    c.set_pn("badge", 0)
    force = (0x1F if a.offer else 0) | (F_CUSTOM_ROWS if a.offer_custom else 0)
    if force:
        c.set_pn("features_force", force)
        time.sleep(SETTLE_S)
        f = c.panel()["features"]
        result(f & force == force, "--offer / --offer-custom: forced features in effect (XW_DEV "
               "build)", f"features {f:02x}, forced {force:02x}")
    time.sleep(TOAST_S + 0.3)
    s0 = c.settings()
    info(f"start: {s0}; tap {p['tap_ms']} ms, hold {p['hold_ms']} ms")
    if c.panel()["share_down"]:
        info("Share is down on some pad (someone holding it?): results may be off")

    try:
        run(c, pad, s0)
    finally:
        restore(c, s0)
        c.set_pn("features_force", force0)
        c.set_pn("badge", badge0)

    print(f"{'FAILED' if failed else 'ALL PASSED'}"
          + (f" ({len(failed)}: {', '.join(failed)})" if failed else ""), flush=True)
    return 1 if failed else 0


def run(c, pad, s0):
    # 2. tap: mute toggles, toast, no panel; a second tap undoes it
    p0 = c.panel()
    c.tap()
    p1 = c.panel()
    m1 = c.muted()
    result(m1 != s0["muted"] and p1["taps"] == p0["taps"] + 1, "Share tap toggles mute (XVHS)",
           f"muted {s0['muted']} -> {m1}, taps +{p1['taps'] - p0['taps']}")
    result(p1["visible"] == TOAST, "tap shows the toast", f"visible {p1['visible']}")
    result(p1["holds"] == p0["holds"], "tap doesn't show the panel",
           f"holds +{p1['holds'] - p0['holds']}")
    time.sleep(TOAST_S + 0.2)
    result(c.panel()["visible"] == HIDDEN, "toast gone after 1.5 s")
    c.tap()
    m2 = c.muted()
    result(m2 == s0["muted"], "second tap toggles it back", f"muted {m1} -> {m2}")
    time.sleep(TOAST_S + 0.2)

    # 3. a 300 ms hold shows the panel on BALANCE
    c.set_ga("balance", 0)
    time.sleep(SETTLE_S)
    p0 = c.panel()
    c.inject(SHARE)
    time.sleep(0.3)
    p1 = c.panel()
    result(p1["visible"] == PANEL and p1["holds"] == p0["holds"] + 1,
           "300 ms hold shows the panel", f"visible {p1['visible']}, holds +"
           f"{p1['holds'] - p0['holds']}")
    result(p1["cursor"] == BALANCE, "cursor starts on balance", f"cursor {p1['cursor']}")
    pv = c.pub_visible()
    result(pv != 0, "hud.py status: pub_visible", str(pv))

    # 4. balance: Left x3, Right x3
    b0 = c.mix()["balance"]
    for _ in range(3):
        c.press(LEFT)
    time.sleep(SETTLE_S)
    b1 = c.mix()["balance"]
    result(b1 == b0 - 3, "Left x3: balance -3", f"{b0} -> {b1}")
    for _ in range(3):
        c.press(RIGHT)
    time.sleep(SETTLE_S)
    b2 = c.mix()["balance"]
    result(b2 == b0, "Right x3: balance back", f"{b1} -> {b2}")

    # 5. Down to VOLUME, with the D-pad held: kept from the title
    m0 = c.panel()["masked_presses"]
    c.inject(SHARE | DOWN)
    time.sleep(STEP_S)
    p = c.panel()
    tb = p["title_buttons%d" % pad]
    result(not tb & GIP_DPAD, "D-pad masked from the title while Share is down",
           f"title buttons {tb:04x}")
    result(p["masked_presses"] > m0, "masked_presses counts it", f"+{p['masked_presses'] - m0}")
    c.inject(SHARE)
    time.sleep(STEP_S)
    result(c.panel()["cursor"] == VOLUME, "Down: cursor on volume", f"cursor {c.panel()['cursor']}")

    # 6. Down through every shown row in hud_row_order, each one stepped and back
    p = c.panel()
    f, rows = p["features"], p["rows"]
    order = [r for r in ROW_ORDER if rows >> r & 1]
    m = voice.read_mt(c.x)
    want = {BALANCE, VOLUME, SIDETONE, MIC, GATE, PRESET, TONE}
    want |= {MICTEST} if m and m["enable"] == 1 else set()
    want |= {SURROUND} if f & F_SURROUND else set()
    want |= {TEAM, CHANNEL} if f & F_TEAM else set()
    want |= CUSTOM_ROWS if f & F_CUSTOM_ROWS else set()
    if f & F_CUSTOM and not f & F_CUSTOM_ROWS and CUSTOM_ROWS & set(order):
        want |= CUSTOM_ROWS  # CUSTOM is the selected preset (eq.py says which)
        info("CUSTOM is selected: its rows are walked")
    result(set(order) == want, "rows shown follow the features",
           f"rows {rows:04x} = {[ROWS[r] for r in order]}, features {f:02x}")
    info(f"features offered {f:02x} (surround 1, mic auto 2, gate auto 4, team 8, presets 16, "
         f"custom 32; forced only: custom rows 64)")
    result(p["unused45"] == 0 and not f & 0x80, "no description area (XWPN word 45 is 0, "
           "features_force 0x80 ignored)", f"word 45 {p['unused45']}, features {f:02x}")
    i = 1
    while i < len(order):
        row = order[i]
        cur = c.panel()["cursor"]
        if not result(cur == row, f"cursor on {ROWS[row]} (hud_row_order)", f"cursor {cur}"):
            break
        if row in (VOLUME, SIDETONE, MIC):
            step_and_back(c, row)
            if row == MIC and f & F_MIC_AUTO:
                mic_auto_check(c)
        elif row == GATE:
            gate_check(c, bool(f & F_GATE_AUTO))
        elif row == PRESET:
            preset_check(c, f)
            # leaving CUSTOM (it ends on FLAT) hides its rows unless they are forced
            rows = c.panel()["rows"]
            order = [r for r in ROW_ORDER if rows >> r & 1]
        elif row in (SURROUND, TEAM, CHANNEL) or row in CUSTOM_ROWS:
            step_back_counted(c, row)
        if row != order[-1]:
            c.press(DOWN)
        i += 1
    last = order[-1]
    c.press(DOWN)
    result(c.panel()["cursor"] == last, f"Down on the last row ({ROWS[last]}): stays")
    walk_to(c, GATE)
    result(c.panel()["cursor"] == GATE, "Up: cursor on noise gate", f"cursor {c.panel()['cursor']}")
    walk_to(c, TONE)
    result(c.panel()["cursor"] == TONE, "Down: cursor on test tone", f"cursor {c.panel()['cursor']}")
    t0 = c.panel()["tone_auto_offs"]
    if c.diag()["applied_tone"] == 0:
        c.press(RIGHT)
        time.sleep(SETTLE_S)
    tone = c.diag()["applied_tone"]
    result(tone != 0, "Right on test tone: tone on", f"tone {tone}")

    # 7. release: hidden, tone off, the mic test told (nothing recording or playing)
    mc0 = c.panel()["mictest_closes"]
    c.inject(0)
    time.sleep(SETTLE_S)
    p = c.panel()
    tone = c.diag()["applied_tone"]
    result(p["visible"] == HIDDEN, "Share released: panel hidden", f"visible {p['visible']}")
    result(p["mictest_closes"] == mc0 + 1, "panel closed: xd_mictest_panel_closed called once",
           f"+{p['mictest_closes'] - mc0}")
    result(tone == 0 and p["tone_auto_offs"] == t0 + 1, "tone off when the panel hides",
           f"tone {tone}, tone_auto_offs +{p['tone_auto_offs'] - t0}")
    pv = c.pub_visible()
    result(pv == 0, "hud.py status: pub_visible cleared", str(pv))
    s = c.settings()
    result(s["muted"] == s0["muted"], "panel use didn't toggle the mute", f"muted {s['muted']}")

    # 8. Share up: the D-pad reaches the title
    m0 = c.panel()["masked_presses"]
    c.inject(UP)
    time.sleep(STEP_S)
    p = c.panel()
    tb = p["title_buttons%d" % pad]
    result(bool(tb & GIP_DPAD_U) and p["masked_presses"] == m0,
           "D-pad reaches the title with Share up", f"title buttons {tb:04x}")
    c.inject(0)
    time.sleep(STEP_S)
    c.inject(DOWN)  # back where Aurora was
    time.sleep(STEP_S)
    c.inject(0)
    time.sleep(STEP_S)
    result(not c.panel()["title_buttons%d" % pad] & GIP_DPAD, "released: title D-pad clear")

    # 9. the next press starts on BALANCE; a 2 s hold and release changes nothing
    s1 = c.settings()
    p0 = c.panel()
    c.inject(SHARE)
    time.sleep(0.3)
    result(c.panel()["cursor"] == BALANCE, "next Share press: cursor on balance again",
           f"cursor {c.panel()['cursor']}")
    time.sleep(1.7)
    c.inject(0)
    time.sleep(SETTLE_S)
    p1 = c.panel()
    s2 = c.settings()
    result(s2 == s1 and p1["taps"] == p0["taps"] and p1["visible"] == HIDDEN,
           "2 s hold and release changes nothing",
           f"taps +{p1['taps'] - p0['taps']}, visible {p1['visible']}"
           + ("" if s2 == s1 else f", {s1} -> {s2}"))
    result(p1["empty_holds"] == p0["empty_holds"] + 1, "empty hold counted",
           f"+{p1['empty_holds'] - p0['empty_holds']}")

    print("OWNER CHECK 2 minutes in Halo 3 with the real Share button: a tap mutes (MIC MUTED "
          "on the bar), holding shows the panel, D-pad Up/Down picks a row and Left/Right "
          "changes it, and the D-pad doesn't move the in-game menu while Share is held",
          flush=True)


if __name__ == "__main__":
    sys.exit(main())
