#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for the headset equalizer (eq_360.c, docs/ROADMAP.md EQ).

Needs: an xw build with eq_360.c and the real file_360.c, Hdd:\\Plugins\\xw.presets installed
(xbox360/eq.py install), the pad on and the jack headset streaming (audio stage 4), any title.

  XBOX=10.0.0.13 python3 xbox360/accept/eq.py [--hands] [--dwell S]

  --hands    also run the the owner check (y / n); without it, it is printed as a TODO line
  --dwell S  seconds on each preset while stepping through them (default 0.5)

ACCEPT_READ_ONLY=1 (accept/run.py --read-only): only steps 1-2, nothing written.

Side effects, all put back at the end: the EQ gate, the selected preset, CUSTOM's steps, the
test tone (1 kHz, quiet) and the XWDG max counters (zeroed to measure).

Checks (one PASS / FAIL line each; exit status 1 if any failed):
  1. the running build has the XWEQ version 2 block;
  2. the preset file loaded: no error, file version 2, the count and CRC of the local
     tools/presets file, CUSTOM's tables accepted, memory within the 16 KB budget;
  3. gate on: the panel sees the count, CUSTOM offered right after the presets, and
     xq_process runs on 48 kHz blocks (blocks rise, none passed through for the rate);
  4. every preset and CUSTOM selects (index, id; CUSTOM has id 0) and its fade ends; the
     clamp counter stays 0 on the test tone;
  5. driver-thread cost: XWDG work_max_us with the EQ stepping through presets (each switch
     runs two presets for 20 ms) rises less than 300 us over the EQ-off baseline, measured the
     same way overhead.py reads XWDG (xw.read_diag);
  5b. a preset with all 5 bands active, steady for 6 s, adds no driver-loop pass of 25 ms or
     more (XWDG pass_hist) and none stopped by the window (XW36 win_cuts, a build that has
     it) over 6 s with the EQ off (1 allowed): the driver thread has about
     1 ms a frame (docs/INTERNALS.md, the system window), and an EQ that took a pass past
     it cost a whole frame; the EQ's longest time per pass (XWEQ pass_us) and the loop's
     window counters (XW36) are printed with it;
  6. the EQ is really in the earpiece: the 1 kHz tone's level in the earpiece dump (xw.py
     dump, taken after every mix) with each of three presets, against FLAT, matches what the
     file's filters give at 1 kHz within 0.5 dB, on both channels;
  7. CUSTOM in the earpiece: at 0 0 0 the tone is FLAT's level exactly; at four settings its
     level matches the file's tables within 0.5 dB; a held direction (a step every 125 ms for
     4 s, every control end to end) ends steady with no clamped sample;
  8. settings put back.
the owner checks (--hands):
  J1 In a game, open the panel and flip through a few presets: each sounds different, none is
     distorted, none is louder or quieter than FLAT, and switching makes no click.
  J2 On CUSTOM, hold BASS up then down, then the same for MIDS and TREBLE: each step is
     heard, nothing clicks or crackles, and all three back at 0 sounds just like FLAT.
"""
import argparse
import cmath
import math
import os
import sys
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from accept import Checks, read_only  # noqa: E402
import eq  # noqa: E402
import xw  # noqa: E402
from xa import Xbdm  # noqa: E402

BUDGET_US = 300
HEAP_BUDGET = 16384
FADE_S = 0.05      # warm-up + fade = 20 ms, then one 8 ms block to see it
TONE_1K = 2        # XWDG tone: 1 kHz
LEVEL_QUIET = 1    # -30 dBFS peak
MEASURE_S = 10
SLOW_S = 6           # step 5b: seconds per side (the bug gave ~100 slow passes in that time)
SLOW_TOLERANCE = 1   # step 5b: slow passes allowed beyond the EQ-off baseline


def diag_off(name):
    return 4 * xw.DIAG_FIELDS.index(name)


def response_db(bands, f, flat_q15):
    """the level at f of a preset relative to FLAT (both with their preamps), from the file"""
    z1 = cmath.exp(-2j * math.pi * f / 48000)
    h = complex(bands.preamp_q15 / 32768)
    for k in range(5):
        if bands.active >> k & 1:
            b0, b1, b2, a1, a2 = (c / (1 << 28) for c in bands.c[k])
            h *= (b0 + b1 * z1 + b2 * z1 * z1) / (1 + a1 * z1 + a2 * z1 * z1)
    return 20 * math.log10(abs(h) / (flat_q15 / 32768))


def custom_db(tab, steps, f, flat_q15):
    """CUSTOM at steps, the level at f against FLAT (from the file's tables)"""
    import eq_model
    if not any(steps):
        return 0.0
    return response_db(eq_model.custom_bands(tab, *steps), f, flat_q15)


def slow_passes(x, addr, d_addr, idx):
    """>= 25 ms driver-loop passes (XWDG pass_hist[5]) over SLOW_S seconds with the EQ off,
    then with preset idx steady; the EQ's longest time per pass (XWEQ pass_us) meanwhile, and
    the loop's window counters (XW36 version 3) as text. Leaves the gate on and idx selected."""
    def hist():
        return xw.read_diag(x, quiet=True)["pass_hist"][5]

    def loop():
        return xw.read_loop(x)

    def cuts(lp):
        return lp["win_cuts"] if lp else 0

    v3 = eq.read_block(x, addr)["version"] >= 3  # pass_us exists (never write past a block)
    eq.setctl(x, addr, "enable", 0)
    time.sleep(0.5)
    h0, l0 = hist(), loop()
    time.sleep(SLOW_S)
    off, off_cuts = hist() - h0, cuts(loop()) - cuts(l0)
    eq.setctl(x, addr, "enable", 1)
    eq.setctl(x, addr, "select", idx)
    time.sleep(0.5)  # the fade, then steady
    if v3:
        eq.setctl(x, addr, "pass_us", 0)
    h0, l0 = hist(), loop()
    time.sleep(SLOW_S)
    on, l1 = hist() - h0, loop()
    eq_us = eq.pass_times(eq.read_block(x, addr))[1]
    text = "no XW36 window counters (older build)"
    on_cuts = 0
    if l0 and l1:
        on_cuts = cuts(l1) - cuts(l0)
        text = (f"window {l1['win_us']} us, the loop's own time longest {l1['run_max_us']} us, "
                f"passes stopped by the window EQ off +{off_cuts}, preset +{on_cuts}, boosted "
                f"+{l1['win_boosts'] - l0['win_boosts']} (boost {'on' if l1['win_boost'] else 'off'})")
    # a stopped pass is followed at once by the next one, so it may not show as a 25 ms pass:
    # the window counter is the direct reading, counted like the slow passes
    return dict(off=off + off_cuts, eq=on + on_cuts, eq_us=eq_us, loop=text)


def tone_level(path, f):
    """least-squares amplitude of a sine at f per channel of a WAV (the middle second)"""
    with wave.open(path, "rb") as w:
        ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    s = [int.from_bytes(raw[i:i + 2], "little", signed=True) for i in range(0, len(raw), 2)]
    start, stop = n // 4, n // 4 + min(rate, n // 2)
    out = []
    for c in range(ch):
        ss = cc = sc = ys = yc = 0.0
        for i in range(start, stop):
            a = 2 * math.pi * f * i / rate
            si, co, y = math.sin(a), math.cos(a), s[i * ch + c]
            ss += si * si
            cc += co * co
            sc += si * co
            ys += y * si
            yc += y * co
        det = ss * cc - sc * sc
        p, q = (ys * cc - yc * sc) / det, (yc * ss - ys * sc) / det
        out.append(math.hypot(p, q))
    return out, rate


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--dwell", type=float, default=0.5)
    a = ap.parse_args()
    c = Checks()

    x, addr = eq.connect()
    try:
        g = eq.read_block(x, addr)
    except SystemExit as e:
        c.check("1 XWEQ block", False, str(e))
        c.done()
    c.check("1 XWEQ block", True, f"version {g['version']} at {addr:08x}")

    presets, flat_q15, crc = eq.local_presets()
    err = eq.s32(g["load_error"])
    ctab = eq.local_custom()
    c.check("2 preset file loaded",
            g["load_state"] == 2 and err == 0 and g["file_crc"] == crc and g["file_version"] == 2
            and g["flat_q15"] == flat_q15 and 0 < g["heap_bytes"] <= HEAP_BUDGET
            and ctab is not None and eq.s32(g["custom_error"]) == 0,
            f"state {g['load_state']}, error {err} ({eq.LOAD_ERRORS.get(err, '?')}), version "
            f"{g['file_version']}, crc {g['file_crc']:08x} (local {crc:08x}), CUSTOM "
            f"{eq.CUSTOM_ERRORS.get(eq.s32(g['custom_error']), g['custom_error'])}, "
            f"{g['heap_bytes']} bytes")
    if read_only():
        print("INFO read-only: steps 3-7 skipped")
        c.done()

    d_addr = xw.diag_addr()
    d0 = xw.read_diag(x, quiet=True)
    if not d0:
        c.check("XWDG readable", False, "no XWDG block this xw.py knows")
        c.done()
    saved = dict(enable=g["enable"], preset=g["preset"], custom=eq.custom_steps(g),
                 tone=d0["tone"],
                 tone_level=d0["tone_level"])
    logs = os.path.join(xw.OUT, "logs")
    os.makedirs(logs, exist_ok=True)
    try:
        # baseline: EQ off, tone on
        eq.setctl(x, addr, "enable", 0)
        x.setu32(d_addr + diag_off("tone"), TONE_1K)
        x.setu32(d_addr + diag_off("tone_level"), LEVEL_QUIET)
        time.sleep(1)
        x.setu32(d_addr + diag_off("work_max_us"), 0)
        time.sleep(MEASURE_S)
        base = xw.read_diag(x, quiet=True)["work_max_us"]
        tone_hz = xw.read_diag(x, quiet=True)["tone_hz"] or 1000

        # 3. gate on
        eq.setctl(x, addr, "enable", 1)
        time.sleep(0.5)
        g1 = eq.read_block(x, addr)
        time.sleep(2)
        g2 = eq.read_block(x, addr)
        cidx = len(presets) + 1
        c.check("3 gate on: count, CUSTOM and 48 kHz blocks",
                g2["count"] == len(presets) and g2["custom_index"] == cidx
                and g2["blocks"] - g1["blocks"] >= 100 and g2["skipped"] == g1["skipped"],
                f"count {g2['count']}, CUSTOM at {g2['custom_index']}, "
                f"{g2['blocks'] - g1['blocks']} blocks in 2 s, "
                f"{g2['skipped'] - g1['skipped']} passed through")

        # 4 + 5. every preset, with the cost measured meanwhile
        x.setu32(d_addr + diag_off("work_max_us"), 0)
        clamps0 = g2["clamps"]
        bad = []
        t_end = time.time() + MEASURE_S
        rounds = 0
        while rounds == 0 or time.time() < t_end:
            for i in range(len(presets) + 2):  # FLAT, the presets, CUSTOM
                eq.setctl(x, addr, "select", i)
                time.sleep(max(a.dwell, FADE_S))
                s = eq.read_block(x, addr)
                want_id = presets[i - 1]["id"] if 0 < i <= len(presets) else 0
                if s["preset"] != i or s["preset_id"] != want_id or s["fade"] != 0:
                    bad.append(f"{i}: preset {s['preset']} id {s['preset_id']:08x} fade "
                               f"{s['fade']}")
            rounds += 1
        eq_max = xw.read_diag(x, quiet=True)["work_max_us"]
        s = eq.read_block(x, addr)
        c.check("4 every preset selects, fades end, no clamping",
                not bad and s["clamps"] == clamps0,
                f"{rounds} rounds of {len(presets) + 2}; clamps +{s['clamps'] - clamps0}"
                + (f"; wrong: {bad[:3]}" if bad else ""))
        c.check("5 driver work_max_us rise under 300 us", eq_max - base < BUDGET_US,
                f"EQ off {base} us, stepping presets {eq_max} us, rise {eq_max - base} us")

        # 5b. a 5-band preset steady: no slow passes (docs/INTERNALS.md, the system window)
        five = [i + 1 for i, p in enumerate(presets) if p["bands"].active == 0x1F]
        if not five:
            c.check("5b 5-band preset: no slow passes", False, "no preset with 5 bands in the file")
        else:
            slow = slow_passes(x, addr, d_addr, five[0])
            c.check("5b 5-band preset: no slow passes (>= 25 ms, or stopped by the window) "
                    "beyond the EQ-off baseline",
                    slow["eq"] <= slow["off"] + SLOW_TOLERANCE,
                    f"{presets[five[0] - 1]['name']}, {SLOW_S} s each: slow or stopped passes "
                    f"EQ off +{slow['off']}, preset +{slow['eq']}; EQ time per pass longest "
                    f"{slow['eq_us']} us; {slow['loop']}")

        # 6. the EQ in the earpiece: the tone's level per preset against FLAT
        def level(idx):
            eq.setctl(x, addr, "select", idx)
            time.sleep(0.3)
            path = os.path.join(logs, f"eq-{idx}.wav")
            xw.dump(path)
            return tone_level(path, tone_hz)[0]

        flat = level(0)
        picks = sorted(range(1, len(presets) + 1),
                       key=lambda i: -abs(response_db(presets[i - 1]["bands"], tone_hz,
                                                      flat_q15)))[:3]
        errs = []
        for i in picks:
            got = level(i)
            want = response_db(presets[i - 1]["bands"], tone_hz, flat_q15)
            for ch in range(len(got)):
                errs.append((abs(20 * math.log10(got[ch] / flat[ch]) - want), i, ch, want))
        worst = max(errs)
        c.check("6 the earpiece follows the preset at the test tone (0.5 dB)",
                worst[0] < 0.5,
                f"presets {picks}; worst {worst[0]:.2f} dB off (preset {worst[1]}, channel "
                f"{worst[2]}, expected {worst[3]:+.2f} dB against FLAT)")

        # 7. CUSTOM in the earpiece
        def custom_level(steps):
            eq.setctl(x, addr, "custom_set", eq.pack_custom(steps))
            eq.setctl(x, addr, "select", cidx)
            time.sleep(0.3)
            path = os.path.join(logs, "eq-custom-%+d%+d%+d.wav" % tuple(steps))
            xw.dump(path)
            return tone_level(path, tone_hz)[0]

        errs = []
        zero = custom_level([0, 0, 0])
        zero_off = max(abs(20 * math.log10(zero[ch] / flat[ch])) for ch in range(len(zero)))
        for steps in ([0, 6, 0], [0, -6, 0], [6, -3, -6], [-6, 3, 6]):
            got = custom_level(steps)
            want = custom_db(ctab, steps, tone_hz, flat_q15)
            for ch in range(len(got)):
                errs.append((abs(20 * math.log10(got[ch] / flat[ch]) - want), steps, want))
        worst = max(errs)
        s0 = eq.read_block(x, addr)
        for k in range(3):  # a held direction: every control end to end, 8 steps a second
            for v in list(range(1, 7)) + list(range(5, -7, -1)) + list(range(-5, 1)):
                st = [0, 0, 0]
                st[k] = v
                eq.setctl(x, addr, "custom_set", eq.pack_custom(st))
                time.sleep(0.125)
        time.sleep(FADE_S)
        s1 = eq.read_block(x, addr)
        c.check("7 CUSTOM: 0 0 0 is FLAT, settings follow the tables (0.5 dB), a held direction "
                "is clean",
                zero_off < 0.05 and worst[0] < 0.5 and s1["clamps"] == s0["clamps"]
                and s1["fade"] == 0 and eq.custom_steps(s1) == [0, 0, 0],
                f"0 0 0 vs FLAT {zero_off:.2f} dB; worst {worst[0]:.2f} dB off at "
                f"{worst[1]} (expected {worst[2]:+.2f} dB); clamps +{s1['clamps'] - s0['clamps']},"
                f" fade {s1['fade']}")
    finally:
        # 8. put back
        eq.setctl(x, addr, "custom_set", eq.pack_custom(saved["custom"]))
        eq.setctl(x, addr, "select", saved["preset"])
        eq.setctl(x, addr, "enable", saved["enable"])
        x.setu32(d_addr + diag_off("tone"), saved["tone"])
        x.setu32(d_addr + diag_off("tone_level"), saved["tone_level"])
    time.sleep(0.5)
    g = eq.read_block(Xbdm(), addr)
    d = xw.read_diag(Xbdm(), quiet=True)
    c.check("8 settings put back", g["enable"] == saved["enable"]
            and eq.custom_steps(g) == saved["custom"]
            and d["tone"] == saved["tone"] and d["tone_level"] == saved["tone_level"])

    j1 = ("J1 In a game, open the panel and flip through a few presets: each sounds different, "
          "none is distorted, none is louder or quieter than FLAT, and switching makes no click.")
    if a.hands:
        eq.setctl(Xbdm(), addr, "enable", 1)
        ans = input(j1 + " [y/n] ").strip().lower()
        eq.setctl(Xbdm(), addr, "enable", saved["enable"])
        c.check("J1 the owner: presets sound right", ans.startswith("y"))
    else:
        print("TODO " + j1)
    j2 = ("J2 On CUSTOM, hold BASS up then down, then the same for MIDS and TREBLE: each step is "
          "heard, nothing clicks or crackles, and all three back at 0 sounds just like FLAT.")
    if a.hands:
        eq.setctl(Xbdm(), addr, "enable", 1)
        ans = input(j2 + " [y/n] ").strip().lower()
        eq.setctl(Xbdm(), addr, "enable", saved["enable"])
        c.check("J2 the owner: CUSTOM sounds right", ans.startswith("y"))
    else:
        print("TODO " + j2)
    c.done()


if __name__ == "__main__":
    main()
