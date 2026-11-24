#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for WP1.1, "HUD hook bring-up on hardware" (docs/PLAN.md): the staged
steps H0-H8, one per run, in order. Every step but H0 writes to the console (XH36 controls).

  XBOX=10.0.0.13 python3 xbox360/accept/wp1_1.py STEP [--seconds N] [--modes] [--tag NAME]
  WP1_1_STEP=h2 XBOX=10.0.0.13 python3 xbox360/accept/run.py wp1_1     (the same, with a tee)

Keep `xw.py tee` running in another terminal for every step (run.py starts one), and wait
about 30 s between steps.

  h0  after boot, read-only: this build (XH36 version 9), stage 0, VdSwap not hooked
  h1  stage 1 (check only): sig_ok 1, VdSwap at 800c8b90, still not hooked
  h2  at idle Aurora: stage 2 (hook, record, never draw): hooked; for --seconds (60) the
      console and the driver thread stay alive, swaps grow ~60/s, nothing is drawn; xw.py
      hooks calls VdSwap ours
  h3  three status reads 2 s apart: the front buffer's format is one hud_360.c draws into;
      tiled; 1280x720 (at Aurora); fence behaviour printed
  h4  stage 3 and hud.py show (the full panel): it is drawn; a screenshot (saved under
      out/logs/) has the 4 sentinels at the positions the HUD reports
  h5  20 screenshots: the sentinels in 19 or more. --modes: the same for mode 0, 1 and 2
      (the mode is left at the best one)
  h6  in Halo 3 (menu, then a match: run it twice, --tag menu / --tag match): h3 + h4 + h5
      checks, plus over --seconds (30) with the panel up: cover >= 95 % of swaps drawn, draw
      time max < 1 ms (d_ticks_max < 50,000)
  h7  in a match, panel up for --seconds (60): out_starved +0, mic_pps 1000 +/- 2 in every
      sample, out_frames_missed +0; driver loop and hud_poll maxima printed
  h8  panel forced on, then switch titles by hand (Halo 3 -> Aurora -> Halo 3) within
      --seconds (180): the console never goes silent, the HUD's heartbeat keeps moving, the
      title changes are seen; skip_title printed
  pres the panel shown for --seconds (10): the share of the draw thread's ~2 ms samples
      that found it in the scan-out (>= 95 %), the gaps, and a the owner check for flicker
  off hud.py hide and stage 0 (the hook, if in, only passes through)

One PASS or FAIL line per check (INFO lines are measurements, OWNER CHECK lines need eyes);
the exit status is 1 if anything failed. ACCEPT_READ_ONLY=1 (run.py --read-only) skips every
write and only reads.
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from accept import Checks, read_only, tee_log, tee_mark, tee_wait  # noqa: E402

import hud  # noqa: E402
import xw as xwpy  # noqa: E402
from xa import OUT, Xbdm  # noqa: E402

ADDR_VDSWAP = 0x800C8B90
HK_INSTALLED = 1
# the panel state h4-h8 show (hud.py show keys = struct hud_state v2 fields)
SHOW = dict(visible=2, muted=0, headset=1, game_on=1, cursor=0, rows=0x3F, balance=0,
            volume=10, sidetone=0, mic_level=5, gate=1, tone=0, mic_vu=4, gate_open=1)
OFF_LOOP = 63 * 4 + 4 * 52 + 85 * 4   # XW36 loop_us, loop_max_us (accept/wp0_1.py)
MIC_PPS, MIC_PPS_TOL = 1000, 2
DRAW_MAX_TICKS = 50000                # 1 ms at 50 MHz
SHOTS, SHOTS_NEED = 20, 19

c = Checks()


def info(text):
    print(f"INFO {text}", flush=True)


def owner(text):
    print(f"OWNER CHECK {text}", flush=True)


class Hud:
    def __init__(self):
        self.x = Xbdm(timeout=10)
        self.addr = hud.block_addr()

    def read(self):
        return hud.read_block(self.x, self.addr)

    def set(self, name, value):
        if read_only():
            info(f"read-only: not setting {name} = {value}")
            return
        hud.setctl(self.x, self.addr, name, value, quiet=True)
        info(f"{name} = {value}")

    def show(self, **kv):
        if read_only():
            info("read-only: not showing the panel")
            return
        for k, v in dict(SHOW, **kv).items():
            hud.setctl(self.x, self.addr, hud.SHOW_KEYS[k], v, quiet=True)
        hud.setctl(self.x, self.addr, "force", 1, quiet=True)
        info("hud.py show (the full Phase 1 panel)")

    def wait(self, pred, secs):
        """the block once pred(block) holds, polled every 0.25 s for up to secs; else the last"""
        until = time.time() + secs
        while True:
            g = self.read()
            if pred(g) or time.time() >= until:
                return g
            time.sleep(0.25)


def print_status():
    """hud.py status, indented"""
    try:
        r = subprocess.run([sys.executable, os.path.join(os.path.dirname(OUT), "hud.py"),
                            "status"], capture_output=True, text=True, timeout=60)
        out = (r.stdout + r.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        out = f"hud.py status: {e}"
    for line in out.splitlines():
        print(f"     | {line}", flush=True)


def tee_line(mark, text, secs):
    if not tee_log():
        info(f"no tee log (XW_TEE_LOG): not looking for {text!r} (run through accept/run.py, "
             "or check your own xw.py tee)")
        return
    line = tee_wait(mark, lambda l: text in l, secs)
    c.check(f"tee: {text!r} logged", bool(line), line or f"not within {secs} s")


# ---- the steps ----------------------------------------------------------------------------------

def h0(h, a):
    g = h.read()
    c.check("XH36 version 9 (this build is running)", g["version"] == 9, f"version {g['version']}")
    if g["version"] < 9:
        return
    c.check("stage 0 at boot", g["enable"] == 0 and g["stage"] == 0,
            f"enable {g['enable']}, stage {g['stage']}")
    c.check("VdSwap not hooked", g["hooked"] != HK_INSTALLED,
            hud.HOOKED.get(g["hooked"], str(g["hooked"])))
    c.check("signature not checked yet", g["sig_ok"] == 0, f"sig_ok {g['sig_ok']}")
    hb = g["heartbeat"]
    g = h.wait(lambda g: g["heartbeat"] != hb, 2)
    c.check("hud_poll runs (heartbeat moves)", g["heartbeat"] != hb, f"{hb} -> {g['heartbeat']}")


def h1(h, a):
    mark = tee_mark()
    h.set("enable", 1)
    g = h.wait(lambda g: g["stage"] == 1 and g["sig_ok"], 3)
    c.check("stage 1", g["stage"] == 1, f"stage {g['stage']}")
    ok = c.check("sig_ok 1 (23 code words match)", g["sig_ok"] == 1,
                 f"sig_ok {g['sig_ok']}, error {g['error']:08x}" +
                 ("" if g["sig_ok"] == 1 else ": STOP, not the code read offline; do not go on"))
    c.check("VdSwap at 800c8b90", g["sig_addr"] == ADDR_VDSWAP, f"{g['sig_addr']:08x}")
    c.check("nothing patched", g["hooked"] != HK_INSTALLED,
            hud.HOOKED.get(g["hooked"], str(g["hooked"])))
    if ok:
        tee_line(mark, "hud: stage 1: VdSwap at", 5)


def h2(h, a):
    g = h.read()
    if not c.check("precondition: stage 1 passed (sig_ok 1)", g["sig_ok"] == 1,
                   f"sig_ok {g['sig_ok']}: run h1 first"):
        return
    print("     (this step patches VdSwap: be at idle Aurora, nothing launching)", flush=True)
    mark = tee_mark()
    h.set("enable", 2)
    g = h.wait(lambda g: g["hooked"] != 0, 5)
    if not c.check("VdSwap hooked", g["hooked"] == HK_INSTALLED,
                   f"{hud.HOOKED.get(g['hooked'], g['hooked'])}, error {g['error']:08x}, "
                   f"install_ms {g['install_ms']}"):
        return
    tee_line(mark, "VdSwap hooked", 5)
    s0, hb0, t0 = g["swaps"], g["heartbeat"], time.time()
    alive, rates, draws0 = True, [], g["draws"]
    info(f"watching {a.seconds:.0f} s")
    while time.time() - t0 < a.seconds:
        time.sleep(2)
        try:
            g = h.read()
        except (OSError, SystemExit) as e:
            alive = False
            c.check("console answers", False, f"xbdm: {e} (a freeze here = the patch or the "
                    "stub: reboot, then compare the stub's frame with VdSwap's stack args 9, 10)")
            return
        rates.append(g["swaps_per_s"])
    dt = time.time() - t0
    rate = (g["swaps"] - s0) / dt
    c.check("console alive for the whole window", alive, f"{dt:.0f} s")
    c.check("driver thread alive (HUD heartbeat moves)", g["heartbeat"] > hb0,
            f"{hb0} -> {g['heartbeat']}")
    c.check("swaps grow 30-60/s (the title's frame rate)", 20 <= rate <= 75,
            f"{rate:.1f}/s over the window")
    c.check("swaps_per_s agrees with the raw count", bool(rates) and
            all(abs(r - rate) <= max(5, rate / 5) for r in rates[1:]),
            f"samples {rates[:8]}")
    c.check("nothing drawn at stage 2", g["draws"] == draws0 and g["vis_draws"] == 0,
            f"draws {draws0} -> {g['draws']}")
    res = {r["name"]: r for r in xwpy.hooks_check(h.x)}
    vd = res.get("VdSwap")
    c.check("xw.py hooks: VdSwap is ours", bool(vd) and vd["kind"] == "ours",
            vd and f"{vd['kind']}: {vd['detail']}")
    print_status()


def fetch_checks(g, where, expect_720=True):
    """the buffer on screen after the last swap (its own fetch constant, the one r4 points to,
    or the last picture swap's: hud.py set src)"""
    f = [g[f"sw_fetch{i}"] for i in range(6)]
    n = [g[f"src_n{i}"] for i in range(4)]
    info(f"{where}: swaps by front-buffer source: " +
         ", ".join(f"{hud.SOURCES[i]} {n[i]}" for i in range(4)) +
         f"; r4 {g['sw_ptr0']:08x}, r8 {g['sw_ptr1']:08x} -> {g['sw_out0']:08x}, r9 "
         f"{g['sw_ptr2']:08x} -> {g['sw_out1']:08x}, r10 {g['sw_ptr3']:08x} -> {g['sw_out2']:08x}"
         f"; *r8 = the r4 fetch's base in {g['ptr_agree']} swaps, differs in {g['ptr_differ']}")
    info(f"{where}: scan-out register {g['scan_phys']:08x} -> {g['scan_va']:08x}; swaps whose "
         f"buffer is the scan-out: {g['scan_agree']}, is not: {g['scan_differ']}" +
         ("  <- the swaps' buffers are not what the display shows: use `hud.py set src 4`"
          if g["scan_differ"] > g["scan_agree"] else ""))
    if g["pic_title"]:
        info(f"{where}: last swap with a picture: " +
             hud.decode([g[f"pic_fetch{i}"] for i in range(6)]))
    if not c.check(f"{where}: the buffer on screen is known", g["sw_src"] != 0 and f[0] & 3 == 2,
                   f"last swap: {hud.SOURCES[g['sw_src']]}, fetch {f[0]:08x} {f[1]:08x}" +
                   ("" if g["sw_src"] else "; no picture swap seen and r4 doesn't point to a "
                    "fetch constant: try a title switch (a picture swap) and rerun h3")):
        return
    info(f"{where}: on screen: " + hud.decode(f))
    fmt, tiled = f[1] & 0x3f, f[0] >> 31
    w, ht = (f[2] & 0x1fff) + 1, ((f[2] >> 13) & 0x1fff) + 1
    c.check(f"{where}: format is one the panel draws into", fmt in hud.FORMATS,
            f"format {fmt} {hud.FORMATS.get(fmt, '(unknown: add the decode; no drawing yet)')}, "
            f"endian {hud.ENDIAN[(f[1] >> 6) & 3]}, swizzle {(f[3] >> 1) & 0xfff:03x}")
    c.check(f"{where}: tiled", bool(tiled), "tiled" if tiled else "linear")
    if expect_720:
        c.check(f"{where}: 1280x720", (w, ht) == (1280, 720), f"{w}x{ht}")
    else:
        info(f"{where}: {w}x{ht}")


def h3(h, a, where="Aurora", expect_720=True):
    reads = []
    for i in range(3):
        g = h.read()
        reads.append(g)
        info(f"read {i + 1}: swaps {g['swaps']} with a picture {g['swaps_fetch']}, "
             f"{g['swaps_per_s']}/s, same fence "
             f"value {g['fence_same']}, waiting for the GPU {g['fence_waits']}, fence "
             f"{g['sw_fence_addr']:08x} wanted {g['sw_fence_val']:08x} read {g['sw_fence_now']:08x}")
        if i < 2:
            time.sleep(2)
    d_sw = reads[2]["swaps"] - reads[0]["swaps"]
    d_same = reads[2]["fence_same"] - reads[0]["fence_same"]
    info(f"fence_same: {d_same} of {d_sw} swaps reused the previous fence value"
         + (" (the fence value doesn't advance per swap: mode 0 may not work; see h5 --modes)"
            if d_sw and d_same * 2 > d_sw else ""))
    c.check(f"{where}: hooked and recording", reads[2]["hooked"] == HK_INSTALLED and d_sw > 0,
            f"{d_sw} swaps in 4 s")
    fetch_checks(reads[2], where, expect_720)
    print_status()


def grab(g):
    import shot  # WP0.2
    return shot.grab(swap_rb=g["swap_rb"])  # the HUD's own lane order


def sentinel_hits(fr, g):
    """how many of the 4 sentinels are at the HUD's reported positions, with its colours"""
    if g.get("sent_n") != 4:
        return 0
    n, bits = 0, fr.bits
    for i in range(4):
        x, y, want = g[f"sent_x{i}"], g[f"sent_y{i}"], g[f"sent_rgb{i}"]
        if 0 <= x < fr.width and 0 <= y < fr.height:
            r, gg, b = fr.rgb(x, y)
            if (r << 2 * bits | gg << bits | b) == want:
                n += 1
    return n


def h4(h, a, where="Aurora"):
    g = h.read()
    if not c.check("precondition: VdSwap hooked (h2)", g["hooked"] == HK_INSTALLED,
                   hud.HOOKED.get(g["hooked"], str(g["hooked"]))):
        return False
    h.set("enable", 3)
    if a.thread is not None:
        h.set("thread", a.thread)
    h.show()
    d0 = g["draws"] + g["intact"]
    g = h.wait(lambda g: g["draws"] + g["intact"] > d0 + 3, 3)
    drawn = c.check(f"{where}: the panel is drawn (draws grow)", g["draws"] + g["intact"] > d0,
                    f"draws {g['draws']}, intact {g['intact']}, skipped: format "
                    f"{g['skip_format']} unmapped {g['skip_unmapped']} canvas {g['skip_canvas']}"
                    f" stale {g['skip_stale']}, fence waits {g['fence_waits']}, last reason "
                    f"{hud.SKIPS.get(g['last_skip'], g['last_skip'])}")
    if not drawn:
        return False
    time.sleep(0.5)
    fr, hdr = grab(h.read())
    g = h.read()
    os.makedirs(os.path.join(OUT, "logs"), exist_ok=True)
    png = os.path.join(OUT, "logs", f"wp1_1_{a.step}{'_' + a.tag if a.tag else ''}.png")
    fr.png(png)
    info(f"screenshot {fr.width}x{fr.height} -> {png}; panel {g['d_w']}x{g['d_h']} at "
         f"({g['d_x0']},{g['d_y0']}) scale {g['d_scale']}")
    c.check(f"{where}: the screenshot is the buffer drawn into (same size)",
            (fr.width, fr.height) == (g["d_width"], g["d_height"]),
            f"screenshot {fr.width}x{fr.height}, drawn {g['d_width']}x{g['d_height']}")
    n = sentinel_hits(fr, g)
    ok = c.check(f"{where}: 4 sentinels at the HUD's positions", n == 4,
                 f"{n} of 4 at " + " ".join(f"({g[f'sent_x{i}']},{g[f'sent_y{i}']})"
                                            for i in range(4)) +
                 ("" if n == 4 else "; draws grow but no pixels: try `hud.py set mode 1`, then "
                  "2, and rerun h4; wrong colours in the PNG: `hud.py set swap_rb 1`; the "
                  "panel in the wrong buffer: `hud.py set src 4` (the scan-out register; h3 "
                  "says whether the swaps' buffers are it), then `src 3`, `src 1`; rerun h4 "
                  "after each"))
    info(f"{where}: last draw: stores {g['d_blit_us']} us, flush {g['d_flush_us']} us "
         f"(d_ticks {g['d_ticks'] / 50:.0f} us), longest pause between two lines "
         f"{g['d_gap_max_us']} us, pauses > 1 ms so far {g['d_gaps']} (a pause of ~16.7 ms = "
         f"the driver thread lost its hardware thread, not slow stores); drawn through "
         f"{g['d_va']:08x}; draw thread: control {g['thread']}, state {g['thr_state']} on hw "
         f"{g['thr_hw_used']} prio {g['thr_prio_now']} (base {g['thr_prio_base']}, ceiling "
         f"{g['thr_prio_max']}), wakes {g['thr_wakes']}, spins {g['thr_spins']}/"
         f"{g['thr_spin_hits']}; last draw: paused {g['d_paused_us']} us, storing "
         f"{g['d_work_us']} us for {g['d_ink_px']} pixels, attempts {g['d_tries']} (restarts so "
         f"far {g['d_restarts']}). Pauses: --thread 1, then `hud.py set thr_prio 15`, "
         f"`thr_hw 1` / `0` / `5` (live), `bg 2` / `bg 0` (fewer pixels); ensure "
         f"{g['ensure']}: checks {g['ens_checks']}, drew {g['ens_draws']}, scan-out unknown "
         f"{g['ens_skips']}")
    owner(f"{where}: look at the TV: is the panel steady, readable, and inside the picture?")
    return ok


def shots(h, n=SHOTS):
    hits, t0 = 0, time.time()
    for _ in range(n):
        fr, _ = grab(h.read())
        if sentinel_hits(fr, h.read()) == 4:
            hits += 1
    return hits, time.time() - t0


def h5(h, a, where="Aurora"):
    g = h.read()
    if not c.check("precondition: drawing (h4)", g["stage"] == 3 and g["force"] == 1,
                   f"stage {g['stage']}, force {g['force']}"):
        return
    if a.modes:
        best = None
        for m in (0, 1, 2):
            h.set("mode", m)
            time.sleep(1)
            hits, dt = shots(h)
            g = h.read()
            info(f"mode {m}: sentinels in {hits} of {SHOTS} screenshots ({dt:.1f} s); swap -> "
                 f"draw {g['swap_late_us']} us (max {g['swap_late_max_us']})")
            if best is None or hits > best[1]:
                best = (m, hits)
        h.set("mode", best[0])
        c.check(f"{where}: best mode {best[0]}: sentinels in >= {SHOTS_NEED} of {SHOTS}",
                best[1] >= SHOTS_NEED, f"{best[1]} of {SHOTS}")
        return
    hits, dt = shots(h)
    g = h.read()
    c.check(f"{where}: sentinels in >= {SHOTS_NEED} of {SHOTS} screenshots", hits >= SHOTS_NEED,
            f"{hits} of {SHOTS} over {dt:.1f} s (mode {g['mode']})" +
            ("" if hits >= SHOTS_NEED else "; flicker: compare modes with h5 --modes"))
    info(f"VdSwap -> draw {g['swap_late_us']} us (max {g['swap_late_max_us']}); fence seen -> "
         f"draw {g['fence_late_us']} us (max {g['fence_late_max_us']}); last second "
         f"{g['cover_pct']} % of swaps drawn")


def budget(h, a, where):
    """over a.seconds with the panel up: cover and draw time"""
    for m in ("d_ticks_max", "fence_late_max_us", "swap_late_max_us", "poll_max_us"):
        h.set(m, 0)
    g0 = h.read()
    info(f"{where}: panel up for {a.seconds:.0f} s")
    time.sleep(a.seconds)
    g = h.read()
    vs, vd = g["vis_swaps"] - g0["vis_swaps"], g["vis_draws"] - g0["vis_draws"]
    pct = 100 * vd / vs if vs else 0
    c.check(f"{where}: cover >= 95 % (swaps drawn while the panel is up)", vs > 0 and pct >= 95,
            f"{vd} of {vs} = {pct:.1f} %")
    c.check(f"{where}: draw time max < 1 ms (d_ticks_max < {DRAW_MAX_TICKS})",
            0 < g["d_ticks_max"] < DRAW_MAX_TICKS,
            f"{g['d_ticks_max']} ticks = {g['d_ticks_max'] / 50:.0f} us; histogram " +
            ", ".join(f"{hud.HIST[i]} +{g[f'd_hist{i}'] - g0[f'd_hist{i}']}" for i in range(4)))
    info(f"{where}: hud_poll max {g['poll_max_us']} us; VdSwap -> draw max "
         f"{g['swap_late_max_us']} us; fence seen -> draw max {g['fence_late_max_us']} us")


def h6(h, a):
    where = f"Halo 3 {a.tag}" if a.tag else "Halo 3"
    g = h.read()
    info(f"titles seen {g['titles_seen']}; current swap title {g['sw_title']:08x}")
    h3(h, a, where, expect_720=False)
    if h4(h, a, where):
        h5(h, a, where)
    budget(h, a, where)


def h7(h, a):
    g = h.read()
    if not c.check("precondition: drawing allowed (stage 3)", g["stage"] == 3,
                   f"stage {g['stage']}"):
        return
    h.show()
    x = h.x
    st = xwpy.status_addr()
    d0 = xwpy.read_diag(x)
    if not c.check("XWDG block", d0 is not None, "read" if d0 else "missing"):
        return
    if not read_only():
        x.setu32(st + OFF_LOOP + 4, 0)  # XW36 loop_max_us: measure from now
    h.set("poll_max_us", 0)
    h.set("d_ticks_max", 0)
    g0 = h.read()
    pps, bad = [], []
    info(f"panel up, watching {a.seconds:.0f} s")
    until = time.time() + a.seconds
    while time.time() < until:
        time.sleep(2)
        s = xwpy.read_status(x, st)
        v = s and s.get("audio") and s["audio"]["mic_pps"]
        pps.append(v)
        if v is None or abs(v - MIC_PPS) > MIC_PPS_TOL:
            bad.append(v)
    d1 = xwpy.read_diag(x)
    g = h.read()
    loop = x.u32s(st + OFF_LOOP, 2)
    c.check("out_starved +0", d1 is not None and d1["out_starved"] == d0["out_starved"],
            f"{d0['out_starved']} -> {d1['out_starved'] if d1 else '-'}")
    c.check(f"mic_pps {MIC_PPS} +/- {MIC_PPS_TOL} in every sample", bool(pps) and not bad,
            f"{len(pps)} samples" + (f", out of range: {bad[:8]}" if bad else ""))
    c.check("out_frames_missed +0", d1 is not None and
            d1["out_frames_missed"] == d0["out_frames_missed"],
            f"{d0['out_frames_missed']} -> {d1['out_frames_missed'] if d1 else '-'}")
    c.check("the panel was drawn throughout", g["vis_draws"] > g0["vis_draws"],
            f"{g['vis_draws'] - g0['vis_draws']} draws of {g['vis_swaps'] - g0['vis_swaps']} swaps")
    info(f"driver loop: last {loop[0] if loop else '-'} us, max {loop[1] if loop else '-'} us; "
         f"hud_poll max {g['poll_max_us']} us; draw max {g['d_ticks_max'] / 50:.0f} us")


def h8(h, a):
    g = h.read()
    if not c.check("precondition: drawing allowed (stage 3)", g["stage"] == 3,
                   f"stage {g['stage']}"):
        return
    h.show()
    g0 = h.read()
    print(f"DO   switch titles now: Halo 3 -> Aurora -> Halo 3 (waiting up to {a.seconds:.0f} s "
          "for 2 title changes)", flush=True)
    titles, last, silent, hb = [g0["sw_title"]], g0["sw_title"], None, g0["heartbeat"]
    until = time.time() + a.seconds
    while time.time() < until:
        time.sleep(0.5)
        try:
            g = h.read()
        except (OSError, SystemExit) as e:
            silent = silent or f"{time.strftime('%H:%M:%S')}: {e}"
            try:
                h = Hud()
            except (OSError, SystemExit):
                pass
            continue
        if g["sw_title"] != last and g["sw_title"]:
            last = g["sw_title"]
            titles.append(last)
            info(f"title {last:08x} ({len(titles) - 1} changes)")
        if len(titles) >= 3:
            time.sleep(5)
            break
    g = h.read()
    c.check("the console never went silent", not silent, silent or "")
    c.check("HUD heartbeat still moving", g["heartbeat"] > hb, f"{hb} -> {g['heartbeat']}")
    c.check("two title changes seen", len(titles) >= 3,
            " -> ".join(f"{t:08x}" for t in titles) + f"; titles_seen {g0['titles_seen']} -> "
            f"{g['titles_seen']}")
    info(f"skip_title {g0['skip_title']} -> {g['skip_title']}, skip_unmapped "
         f"{g0['skip_unmapped']} -> {g['skip_unmapped']}, skip_stale {g0['skip_stale']} -> "
         f"{g['skip_stale']}")
    h.set("force", 0)


def pres(h, a):
    """the panel shown for --seconds (10): the share of ensure samples (every ~2 ms on the
    draw thread) that found it in the scan-out, and the gaps"""
    g = h.read()
    if not c.check("precondition: drawing allowed (stage 3)", g["stage"] == 3,
                   f"stage {g['stage']}"):
        return
    if a.thread is not None:
        h.set("thread", a.thread)
    h.show()
    h.set("gap_max_us", 0)
    time.sleep(1)
    g0 = h.read()
    time.sleep(a.seconds)
    g = h.read()
    y, n = g["pres_yes"] - g0["pres_yes"], g["pres_no"] - g0["pres_no"]
    pct = 100 * y / (y + n) if y + n else 0
    info(f"samples {y + n} in {a.seconds:.0f} s ({(y + n) / a.seconds:.0f}/s): panel there in "
         f"{y} ({pct:.1f} %), redrawn {n}; gaps last {g['gap_last_us']} us, max "
         f"{g['gap_max_us']} us; draws {g['draws'] - g0['draws']}, ensure draws "
         f"{g['ens_draws'] - g0['ens_draws']}; thread {g['thread']} hw {g['thr_hw_used']} prio "
         f"{g['thr_prio_now']}, idle {g['idle_ms']} ms")
    c.check("samples are frequent (>= 200/s: the draw thread wakes every ~2 ms)",
            (y + n) / a.seconds >= 200, f"{(y + n) / a.seconds:.0f}/s")
    c.check("panel present in >= 95 % of samples", pct >= 95, f"{pct:.1f} %")
    owner("is the panel steady now (no flicker)?")


def off(h, a):
    h.set("force", 0)
    h.set("enable", 0)
    g = h.read()
    c.check("off: force 0, stage 0", g["force"] == 0 and g["stage"] == 0,
            f"force {g['force']}, stage {g['stage']} (hook {hud.HOOKED.get(g['hooked'])})")


STEPS = dict(h0=h0, h1=h1, h2=h2, h3=h3, h4=h4, h5=h5, h6=h6, h7=h7, h8=h8, pres=pres, off=off)
DEFAULT_SECONDS = dict(h2=60, h6=30, h7=60, h8=180, pres=10)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("step", nargs="?", default=os.environ.get("WP1_1_STEP", "h0"),
                    choices=sorted(STEPS))
    ap.add_argument("--seconds", type=float)
    ap.add_argument("--modes", action="store_true")
    ap.add_argument("--tag", default=os.environ.get("WP1_1_TAG", ""))
    ap.add_argument("--thread", type=int, choices=(0, 1),
                    help="h4/h6: set the draw-thread control first (1 = its own thread)")
    a = ap.parse_args()
    if a.seconds is None:
        a.seconds = DEFAULT_SECONDS.get(a.step, 30)
    print(f"WP1.1 step {a.step}", flush=True)
    h = Hud()
    g = h.read()
    if a.step != "h0" and g["version"] < 9:
        c.check("XH36 version 9 (this build is running)", False, f"version {g['version']}")
        c.done()
    STEPS[a.step](h, a)
    c.done()


if __name__ == "__main__":
    main()
