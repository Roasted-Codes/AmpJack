#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The on-screen headset panel in xw.xex (hud_360.c, research/HUD.md) over xbdm. Run next to out/.

  hud.py status               stage, hook, swaps and draws, rates, timing, titles, last draw
  hud.py stage N              the bring-up stage (docs/PLAN.md WP1.1), 0 at boot:
                                0 nothing (once hooked: the hook only passes through)
                                1 check only: VdSwap's address and 23 code words (sig_ok)
                                2 hook VdSwap and record swaps, never draw
                                3 drawing allowed
  hud.py show [k=v ...]       dev: show the panel now with a test state, without the controls
                              (force = 1; needs stage 3). Keys are struct hud_state v7's fields
                              (hud_360.h):
                                visible (1 toast, 2 panel, 3 badge) muted flash headset
                                game_on cursor rows balance volume sidetone mic_level (0 auto)
                                gate (0 off, 3 auto, steps 1 2 4 5) tone mic_vu gate_open
                                team_level channel team_peers team_talking
                                rows_hi (bit 0 MIC TEST, 1 PRESET, 2 SURROUND, 6 DEMO) mictest (0 idle,
                                1 recording, 2 playing, 3 paused, 4 ready) mictest_secs
                                mictest_pct
                                version 11 builds also: mic_auto_level preset preset_count
                                surround surround_live badge badge_shift mictest_max
                                preset_name (text, up to 20 characters)
                                version 13 builds also: preset_flags (1 = CUSTOM offered, the
                                PRESET row's last value) custom_bass custom_mids custom_treble
                                (-6..6; rows_hi bits 3-5 show their rows) desc (text, up to 63
                                characters: the help line while cursor=9, PRESET; version 14
                                builds show it there, cut at a word to 36 characters)
                                version 15 builds also: demo (1 playing) demo_secs; rows_hi
                                bit 6 shows SURROUND DEMO (row 14, under SURROUND)
                              Unnamed keys keep their last value (at boot: the panel as it
                              boots, PRESET on FLAT and MIC TEST, volume 10, mic 5, gate
                              auto). e.g.
                                hud.py show visible=badge badge=1 badge_shift=2
                                hud.py show cursor=2 balance=-3 muted=1
                                hud.py show cursor=8 mictest=1 mictest_secs=7 mic_vu=5
                                hud.py show visible=toast flash=0 mictest=2 mictest_pct=40
                                hud.py show rows_hi=3 cursor=9 preset=3 preset_count=29 \\
                                  "preset_name=HALO FOOTSTEPS"
                                hud.py show rows_hi=0x3b cursor=11 preset=30 preset_count=29 \\
                                  preset_flags=1 preset_name=CUSTOM custom_bass=3
                                hud.py show rows_hi=3 cursor=9 preset=2 preset_count=9 \\
                                  "preset_name=OMNIVOX" "desc=ALL-ROUNDER: PUNCH AND CLEAR STEPS"
  hud.py hide                 back to normal: the controls decide (force = 0)
  hud.py never                never draw, whatever the controls publish (force = 2)
  hud.py set NAME VALUE       any control: enable force mode keep swap_rb src order noflush alias
                              wide thread thr_hw thr_prio spin_us bg retry_us ensure
                              idle_ms test_* badge_pos (the mute mark: 0 bottom centre,
                              1 bottom left, 2 bottom right, 3 top left, 4 top right)
                              badge_idle_ms, or reset a
                              maximum: d_ticks_max fence_late_max_us swap_late_max_us poll_max_us
                                mode: 0 draw once the GPU fence passes (default), 1 draw when
                                  the swap is first seen, 2 draw the previous swap's buffer
                                keep: background kept, /256 (default 0 = opaque, write-only;
                                  > 0 reads every background pixel)
                                swap_rb: 1 if the green accent comes out red or blue
                                src: where the buffer on screen comes from when a swap
                                  carries no picture: 0 auto (the fetch r4 points to, copied
                                  in the hook, else the last picture swap's), 1 last picture
                                  only, 2 r4 only, 3 r4 with its base replaced by *r8,
                                  4 auto with the base from the display's scan-out register
                                alias: the CPU mapping drawn through (same physical memory):
                                  0 the swap's own address (src 4: as xbdm, the first valid
                                  of +C0000000, +A0000000, +E0000000-1000), 1 E, 2 A, 3 C
                                wide: tiled blit with 8-byte stores (1, default) or 4-byte (0)
                                thread: 1 = draw on a thread of its own (created at stage 3,
                                  on hardware thread thr_hw (default 3; 0, 1, 3 or 5), at
                                  priority thr_prio (default 9), woken by every swap); 0 =
                                  draw on the driver thread
                                spin_us: draw thread, a wake with no swap ready: watch the
                                  newest swap's fence up to this long (default 0, max 8000)
                                thr_hw / thr_prio take effect at the thread's next wake (above
                                  9 the thread's priority ceiling is raised first)
                                bg: panel background 1 opaque (default), 0 none, 2 only next
                                  to text and shapes (fewer pixels: a faster draw)
                                retry_us: a pause longer than this in the middle of a draw
                                  restarts it, up to 3 times (default 2000; 0 = never)
                                ensure: 1 (default) = every pass, draw into the buffer being
                                  scanned out if it is the title's and lacks the panel
                                idle_ms: the draw thread's wake without a swap (default 2:
                                  ensure then samples the scan-out every ~2 ms)
                                badge_idle_ms: the same while only the mute mark is up
                                  (default 2; longer = fewer wakes, a longer gap after the
                                  title redraws the buffer: accept/badge.py measures both)
                                order: tiled blit 0 by whole 128-byte lines (default), 1 by rows
                                noflush: 1 = no dcbf after drawing
  hud.py trace [N] [--wakes]  record the next N (default 64, max 64) events: swaps, fences
                              seen, draws, panel found missing, scan-out changes (and the
                              draw thread's wakes with --wakes); then print them with times
                              in us and the buffers as letters
  hud.py shot FILE.png [--find] [--mem] [--swap-rb] | shot --find
                              the front buffer as a PNG (shot.py); --find looks for the
                              panel's sentinels at the positions the last draw reports

XBOX=host[:port] is the console's address (required). Shares xbdm code with xa.py.
"""
import sys

from xa import Xbdm, xw_block

MAGIC = 0x58483336  # "XH36"
# struct xh_status (hud_360.c), all u32, in order. Version 1's 62 words are a prefix.
V1_FIELDS = ("magic version enable force mode keep swap_rb test_muted test_balance test_volume "
             "test_sidetone test_game_on test_headset test_flash "
             "hooked error now_ms heartbeat publishes pub_visible swaps swaps_fetch fence_same "
             "draws repaints skip_format skip_title skip_unmapped skip_stale fence_waits "
             "last_skip " +
             " ".join(f"sw_fetch{i}" for i in range(6)) + " " +
             " ".join(f"sw_info{i}" for i in range(7)) +
             " sw_fence_addr sw_fence_val sw_fence_now sw_title "
             "d_base d_width d_height d_pitch d_format d_endian d_tiled d_swizzle d_scale d_x0 "
             "d_y0 d_ticks d_ticks_max d_pixels").split()
FIELDS = V1_FIELDS + (
    "stage sig_ok sig_addr install_ms quiesced swaps_per_s draws_per_s cover_pct vis_swaps "
    "vis_draws fence_late_us fence_late_max_us swap_late_us swap_late_max_us " +
    " ".join(f"d_hist{i}" for i in range(4)) + " titles_seen tt_next " +
    " ".join(f"tt_title{i}" for i in range(4)) + " " +
    " ".join(f"tt_fmt{i}" for i in range(4)) + " " +
    " ".join(f"tt_size{i}" for i in range(4)) + " sent_n " +
    " ".join(f"sent_x{i}" for i in range(4)) + " " +
    " ".join(f"sent_y{i}" for i in range(4)) + " " +
    " ".join(f"sent_rgb{i}" for i in range(4)) +
    " canvas_w canvas_h d_w d_h intact skip_canvas poll_us poll_max_us "
    "test_visible test_cursor test_rows test_mic_level test_gate test_tone test_mic_vu "
    "test_gate_open test_team_level test_channel test_team_peers test_team_talking "
    "src sw_src " + " ".join(f"sw_ptr{i}" for i in range(4)) + " " +
    " ".join(f"sw_out{i}" for i in range(3)) + " " +
    " ".join(f"src_n{i}" for i in range(4)) + " " +
    " ".join(f"pic_fetch{i}" for i in range(6)) + " pic_title "
    "order noflush ptr_agree ptr_differ d_blit_us d_flush_us "
    "alias scan_phys scan_va scan_ctl scan_agree scan_differ d_gap_max_us d_gaps "
    "wide d_va skip_alias "
    "thread thr_hw thr_prio spin_us thr_state thr_hw_used thr_prio_now thr_wakes thr_heartbeat "
    "thr_spins thr_spin_hits thr_spin_us_max "
    "thr_prio_base thr_prio_max bg retry_us d_restarts d_tries d_paused_us d_work_us "
    "d_ink_px "
    "ensure idle_ms ens_checks ens_draws ens_skips trace tr_n " +
    " ".join(f"tr{i}" for i in range(256)) +
    " pres_yes pres_no pres_pct gap_last_us gap_max_us "
    "test_rows_hi test_mictest test_mictest_secs test_mictest_pct "
    "test_mic_auto_level test_preset test_preset_count test_surround test_surround_live "
    "test_badge test_badge_shift test_mictest_max " +
    " ".join(f"test_preset_name{i}" for i in range(5)) + " badge_pos "
    "badge_idle_ms "
    "test_preset_flags test_custom_bass test_custom_mids test_custom_treble test_unused476 " +
    " ".join(f"test_desc{i}" for i in range(16)) + " "
    "test_demo test_demo_secs").split()
assert len(V1_FIELDS) == 62 and len(FIELDS) == 495  # _Static_assert in hud_360.c
VERSIONS = {1: V1_FIELDS, 2: FIELDS[:127], 3: FIELDS[:147], 4: FIELDS[:153], 5: FIELDS[:164],
            6: FIELDS[:176], 7: FIELDS[:185], 8: FIELDS[:448], 9: FIELDS[:453], 10: FIELDS[:457],
            11: FIELDS[:471], 12: FIELDS[:472], 13: FIELDS[:493], 14: FIELDS[:493], 15: FIELDS}
# version 14: the same words; test_unused476 (version 13's test_desc_lines, the description
# area's lines) is read by nothing: the description is the help line on PRESET
# what xbdm may write: the controls, and the maxima (0 = measure from now)
CONTROLS = (FIELDS[2:14] + [f for f in FIELDS if f.startswith("test_")] + ["src", "order", "noflush", "alias", "wide", "thread", "thr_hw", "thr_prio",
                                     "spin_us", "bg", "retry_us", "ensure", "idle_ms", "trace",
                                     "tr_n", "gap_max_us", "badge_pos",
                                     "badge_idle_ms"] +
            ["d_ticks_max", "fence_late_max_us", "swap_late_max_us", "poll_max_us"])
# badge_pos: where the persistent mute mark goes (hud_360.c)
BADGE_POS = {0: "bottom centre", 1: "bottom left", 2: "bottom right", 3: "top left",
             4: "top right"}
HOOKED = {0: "not hooked", 1: "VdSwap hooked", 2: "refused (not the 17559 code)",
          3: "hook failed", 4: "disabled (a version 1 block)"}
SIG = {0: "not checked", 1: "ok", 2: "REFUSED"}
STAGES = {0: "off", 1: "check only", 2: "hooked, recording, no drawing", 3: "drawing allowed"}
SKIPS = {0: "-", 1: "not a texture fetch", 2: "format", 3: "size", 4: "address", 5: "swizzle",
         6: "unmapped page", 7: "title changed", 8: "canvas (empty, too big, or no fit)"}
FORMATS = {6: "8_8_8_8", 50: "8_8_8_8_AS_16_16_16_16", 7: "2_10_10_10",
           54: "2_10_10_10_AS_16_16_16_16"}
ENDIAN = {0: "none", 1: "8in16", 2: "8in32", 3: "16in32"}
VIS = {"toast": 1, "panel": 2, "badge": 3}
# hud.py show key -> its control: struct hud_state v7's field names (accept/p1_panel.py,
# accept/mictest.py); rows_hi .. mictest_pct need a version 10 block, mic_auto_level ..
# preset_name version 11, preset_flags .. desc version 13, demo and demo_secs version 15.
# preset_name and desc are text, packed into the test_preset_name / test_desc words
# (text_words).
SHOW_KEYS = {k: "test_" + k for k in (
    "visible muted flash headset game_on cursor rows balance volume sidetone mic_level gate "
    "tone mic_vu gate_open team_level channel team_peers team_talking "
    "rows_hi mictest mictest_secs mictest_pct "
    "mic_auto_level preset preset_count surround surround_live badge badge_shift "
    "mictest_max preset_flags custom_bass custom_mids custom_treble demo demo_secs").split()}
SHOW_KEYS["preset_name"] = "test_preset_name0"
SHOW_KEYS["desc"] = "test_desc0"
NAME_LEN = 20  # hud_360.h HUD_NAME_LEN
DESC_LEN = 64  # hud_360.h HUD_DESC_LEN: 63 characters and a NUL
TEXT_KEYS = {"preset_name": ("test_preset_name", NAME_LEN), "desc": ("test_desc", DESC_LEN)}
HIST = ("<0.25 ms", "<0.5 ms", "<1 ms", ">=1 ms")
THR = {0: "not created", 1: "running", 2: "CREATION FAILED"}
# where the buffer on screen after a swap comes from (hud_360.c FS_*); the src control
SOURCES = ("unknown", "own fetch (a picture swap)", "r4 pointer", "last picture swap")
SRC_MODES = {0: "auto: own, r4 pointer, last picture", 1: "own or last picture only",
             2: "own or r4 pointer only", 3: "own or r4 pointer with the base from *r8",
             4: "auto, with the base from the scan-out register (what xbdm screenshots)"}
ALIASES = {0: "the swap's own address (src 4: as xbdm, C, A, E)", 1: "E window (4 KB pages)",
           2: "A window (64 KB pages)", 3: "C window (16 MB pages)"}


def block_addr():
    """XH36 in the xw build that is running (from out/, or by a scan of the module)"""
    return xw_block(MAGIC)


def read_block(x, addr):
    """the block as a dict; refuses a version it doesn't know"""
    w = x.u32s(addr, 2)
    if not w or w[0] != MAGIC:
        raise SystemExit(f"hud block not found at {addr:08x} (xw.xex loaded? this build?)")
    names = VERSIONS.get(w[1])
    if not names:
        raise SystemExit(f"hud block version {w[1]}: this hud.py knows {sorted(VERSIONS)}")
    return dict(zip(names, x.u32s(addr, len(names))))


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def decode(f):
    """a fetch constant, in words"""
    return (f"base {f[1] & 0xfffff000:08x} {(f[2] & 0x1fff) + 1}x"
            f"{((f[2] >> 13) & 0x1fff) + 1} pitch {((f[0] >> 22) & 0x1ff) << 5} format "
            f"{FORMATS.get(f[1] & 0x3f, f[1] & 0x3f)} endian {ENDIAN[(f[1] >> 6) & 3]} "
            f"{'tiled' if f[0] >> 31 else 'linear'} swizzle {(f[3] >> 1) & 0xfff:03x}")


def fmt_word(f):
    """tt_fmt: format | endian << 8 | tiled << 10 | pitch << 16"""
    return (f"{FORMATS.get(f & 0x3f, f'format {f & 0x3f}')} {ENDIAN[(f >> 8) & 3]} "
            f"{'tiled' if f & 0x400 else 'linear'} pitch {f >> 16}")


def sentinels(g):
    """the last draw's 4 sentinel screen pixels [(x, y)] (for shot --find), or None"""
    if g.get("version", 1) < 2 or g.get("sent_n") != 4 or not g.get("draws") and not g.get("intact"):
        return None
    return [(g[f"sent_x{i}"], g[f"sent_y{i}"]) for i in range(4)]


def print_status(g):
    v2 = g["version"] >= 2
    if v2:
        print(f"stage {g['stage']} ({STAGES.get(g['stage'], '?')}; control enable "
              f"{g['enable']}); signature {SIG.get(g['sig_ok'], g['sig_ok'])}, VdSwap at "
              f"{g['sig_addr']:08x}; hook: {HOOKED.get(g['hooked'], g['hooked'])}"
              + (f" {g['install_ms']} ms after start" if g["install_ms"] else "")
              + f" (error {g['error']:08x}){'; QUIESCED (hot reload)' if g['quiesced'] else ''}")
    else:
        print(f"(a version 1 block: an older build) hook: "
              f"{HOOKED.get(g['hooked'], g['hooked'])}, enable {g['enable']}")
    print(f"controls: force {g['force']} mode {g['mode']} keep {g['keep']} swap_rb "
          f"{g['swap_rb']}; heartbeat {g['heartbeat']}; published {g['publishes']} times, "
          f"visible {g['pub_visible']}")
    if v2:
        print("test state: " + " ".join(
            f"{k}={text_of(g, k) if k in TEXT_KEYS else s32(g[c])}"
            for k, c in SHOW_KEYS.items() if c in g))
    if "badge_pos" in g:
        print(f"mute mark position: {BADGE_POS.get(g['badge_pos'], 'bottom centre')} "
              f"(badge_pos {g['badge_pos']})"
              + (f"; draw thread wait while only the mark is up {g['badge_idle_ms']} ms"
                 if "badge_idle_ms" in g else ""))
    print(f"swaps {g['swaps']} (with a picture {g['swaps_fetch']}, same fence value as the one "
          f"before {g['fence_same']}); draws {g['draws']} (repainted {g['repaints']}"
          + (f", found intact {g['intact']}" if v2 else "") + f"); waiting for the GPU "
          f"{g['fence_waits']}")
    if v2:
        print(f"rates: {g['swaps_per_s']} swaps/s, {g['draws_per_s']} draws/s; panel up: "
              f"{g['vis_draws']} of {g['vis_swaps']} swaps drawn, last second "
              f"{g['cover_pct']} %")
        print(f"timing: draw {g['d_ticks'] / 50:.0f} us (max {g['d_ticks_max'] / 50:.0f}; "
              + ", ".join(f"{h} {g[f'd_hist{i}']}" for i, h in enumerate(HIST))
              + (f"; last: stores {g['d_blit_us']} us, flush {g['d_flush_us']} us (order "
                 f"{g['order']}, noflush {g['noflush']})" if "order" in g else "")
              + (f", longest pause between lines {g['d_gap_max_us']} us, pauses > 1 ms "
                 f"{g['d_gaps']}" if "d_gaps" in g else "")
              + (f"; of which paused {g['d_paused_us']} us, storing {g['d_work_us']} us for "
                 f"{g['d_ink_px']} pixels, attempts {g['d_tries']} (restarts so far "
                 f"{g['d_restarts']}; bg {g['bg']}, retry {g['retry_us']} us)" if "bg" in g else "")
              + f"); fence seen -> draw {g['fence_late_us']} us (max {g['fence_late_max_us']}); "
              f"VdSwap -> draw {g['swap_late_us']} us (max {g['swap_late_max_us']}); "
              f"hud_poll {g['poll_us']} us (max {g['poll_max_us']})")
    if "ensure" in g:
        print(f"ensure scan-out: control {g['ensure']} (idle wake {g['idle_ms']} ms); checks "
              f"{g['ens_checks']}, drew {g['ens_draws']}, scan-out not a known front buffer "
              f"{g['ens_skips']}")
        if "pres_pct" in g:
            n = g["pres_yes"] + g["pres_no"]
            print(f"panel present in the scan-out: last second {g['pres_pct']} % of samples; "
                  f"all {g['pres_yes']} of {n}; gaps (gone -> redrawn) last {g['gap_last_us']} us, "
                  f"max {g['gap_max_us']} us")
    if "thread" in g:
        print(f"draw thread: control {g['thread']} (hw {g['thr_hw']}, prio {g['thr_prio']}, spin "
              f"{g['spin_us']} us); state {THR.get(g['thr_state'], g['thr_state'])} on hw "
              f"{g['thr_hw_used']} at priority {g['thr_prio_now']}"
              + (f" (base {g['thr_prio_base']}, ceiling {g['thr_prio_max']})" if "thr_prio_max" in g
                 else "") + f"; wakes that drew/checked "
              f"{g['thr_wakes']}, heartbeat {g['thr_heartbeat']}; spins {g['thr_spins']} "
              f"(fence seen {g['thr_spin_hits']}, longest {g['thr_spin_us_max']} us)")
    print(f"skipped: format {g['skip_format']} title {g['skip_title']} unmapped "
          f"{g['skip_unmapped']} stale {g['skip_stale']}"
          + (f" canvas {g['skip_canvas']}" if v2 else "")
          + f"; last reason {SKIPS.get(g['last_skip'], g['last_skip'])}")
    f = [g[f"sw_fetch{i}"] for i in range(6)]
    print("last swap: fetch " + " ".join(f"{v:08x}" for v in f) + "; info " +
          " ".join(f"{g[f'sw_info{i}']:08x}" for i in range(7)))
    print(f"  fence at {g['sw_fence_addr']:08x}: wanted {g['sw_fence_val']:08x}, last read "
          f"{g['sw_fence_now']:08x}; title {g['sw_title']:08x}")
    if v2 and "src" in g:
        n = [g[f"src_n{i}"] for i in range(4)]
        print(f"  buffer on screen from: {SOURCES[g['sw_src']] if g['sw_src'] < 4 else g['sw_src']}"
              f" (src {g['src']}: {SRC_MODES.get(g['src'], '?')}); swaps by source: "
              + ", ".join(f"{SOURCES[i]} {n[i]}" for i in range(4)))
        if "scan_phys" in g:
            print(f"  scan-out register {g['scan_phys']:08x} -> {g['scan_va']:08x} (alias "
                  f"{g['alias']}: {ALIASES.get(g['alias'], '?')}; control byte "
                  f"{g['scan_ctl']:02x}); last draw through {g.get('d_va', 0):08x} (wide "
                  f"{g.get('wide', '-')}, alias skips {g.get('skip_alias', '-')}); swaps whose "
                  f"buffer is the scan-out "
                  f"{g['scan_agree']}, is not {g['scan_differ']}")
        if "ptr_agree" in g:
            print(f"  *r8 equals the r4 fetch's base in {g['ptr_agree']} swaps, differs in "
                  f"{g['ptr_differ']}")
        print(f"  r4 {g['sw_ptr0']:08x}; out pointers r8 {g['sw_ptr1']:08x} -> {g['sw_out0']:08x}, "
              f"r9 {g['sw_ptr2']:08x} -> {g['sw_out1']:08x}, r10 {g['sw_ptr3']:08x} -> "
              f"{g['sw_out2']:08x}")
    if f[0] & 3 == 2:
        print("  decoded: " + decode(f))
    if v2 and "pic_title" in g and g["pic_title"]:
        pf = [g[f"pic_fetch{i}"] for i in range(6)]
        print(f"last swap with a picture (title {g['pic_title']:08x}): " + decode(pf))
    if v2 and g["tt_next"]:
        n = g["tt_next"]
        print(f"titles: {g['titles_seen']} title changes; (title, format) pairs seen {n}:")
        for k in range(min(n, 4)):
            i = (n - 1 - k) & 3
            sz = g[f"tt_size{i}"]
            print(f"  {g[f'tt_title{i}']:08x}: {sz >> 16}x{sz & 0xffff} {fmt_word(g[f'tt_fmt{i}'])}")
    if g["draws"] or (v2 and g["intact"]):
        print(f"last draw: base {g['d_base']:08x} {g['d_width']}x{g['d_height']} pitch "
              f"{g['d_pitch']} {FORMATS.get(g['d_format'], g['d_format'])} "
              f"{ENDIAN.get(g['d_endian'])} {'tiled' if g['d_tiled'] else 'linear'} swizzle "
              f"{g['d_swizzle']:03x}; scale {g['d_scale']} at ({g['d_x0']}, {g['d_y0']})"
              + (f", {g['d_w']}x{g['d_h']} px (canvas {g['canvas_w']}x{g['canvas_h']})"
                 if v2 else "") + f", {g['d_pixels']} pixels")
        if v2 and g["sent_n"] == 4:
            print("  sentinels at " + " ".join(f"({g[f'sent_x{i}']},{g[f'sent_y{i}']})"
                                               for i in range(4)))


TRACE_KINDS = {1: "wake", 2: "swap", 3: "fence", 4: "draw", 5: "drawn", 6: "scan", 7: "gone"}


def trace(x, addr, n=64, wakes=False):
    """arm the trace for n events, wait for them (or 2 s), print them"""
    import time
    n = max(1, min(64, n))
    setctl(x, addr, "trace", 0, quiet=True)
    setctl(x, addr, "tr_n", 0, quiet=True)
    setctl(x, addr, "trace", n | (0x80 if wakes else 0), quiet=True)
    until = time.time() + 2
    while time.time() < until:
        g = read_block(x, addr)
        if g["tr_n"] >= n:
            break
        time.sleep(0.1)
    g = read_block(x, addr)
    ev = [[g[f"tr{4 * i + j}"] for j in range(4)] for i in range(min(g["tr_n"], 64))]
    if not ev:
        print("no events (stage 3 and the draw thread or hud_poll working?)")
        return
    letters, t0 = {}, ev[0][0]

    def buf(pa):
        if not pa:
            return "-"
        if pa not in letters:
            letters[pa] = "ABCDEFGH"[len(letters) % 8]
        return letters[pa]
    for tb, kind, a, b in ev:
        k = TRACE_KINDS.get(kind, str(kind))
        t = ((tb - t0) & 0xffffffff) / 50
        if k == "wake":
            d = f"head {a}, scan-out {buf(b)}"
        elif k == "swap":
            d = f"swap #{a} taken in, buffer {buf(b)}"
        elif k == "fence":
            d = f"swap #{a}: fence passed (value {b:08x})"
        elif k == "draw":
            d = f"{'ensure' if not a else f'swap #{a}'}: draw into {buf(b)}"
        elif k == "drawn":
            d = f"{'ensure' if not a else f'swap #{a}'}: done, scan-out now {buf(b)}"
        elif k == "gone":
            d = f"panel missing from the scan-out {buf(b)} (last seen {a} us before): redrawn"
        elif k == "scan":
            d = f"scan-out -> {buf(b)}"
        else:
            d = f"{a:08x} {b:08x}"
        print(f"{t:10.0f} us  {k:6s} {d}")
    print("buffers: " + ", ".join(f"{v} = physical {k:08x}" for k, v in letters.items()))


def setctl(x, addr, name, value, quiet=False):
    if name not in CONTROLS:
        raise SystemExit(f"unknown control {name}; one of {', '.join(CONTROLS)}")
    ver = x.u32s(addr, 2)
    if not ver or name not in VERSIONS.get(ver[1], ()):
        # never past the end of an older build's block
        raise SystemExit(f"{name}: not in the running build's hud block (version "
                         f"{ver[1] if ver else '?'})")
    st = x.setu32(addr + 4 * FIELDS.index(name), value & 0xffffffff)
    if not quiet:
        print(name, "=", s32(value & 0xffffffff), st)
    if not st.startswith("200"):
        raise SystemExit(f"setmem {name}: {st}")
    return st


def text_words(text, n):
    """text -> the n // 4 words of test_preset_name / test_desc (big-endian, NUL padded; desc
    keeps its last byte NUL: hud_state.desc holds at most 63 characters)"""
    keep = NAME_LEN if n == NAME_LEN else n - 1
    b = text.upper().encode("ascii", "replace")[:keep].ljust(n, b"\0")
    return [int.from_bytes(b[i:i + 4], "big") for i in range(0, n, 4)]


def text_of(g, key):
    """the test_preset_name / test_desc words of a block dict -> text"""
    pre, n = TEXT_KEYS[key]
    b = b"".join(g[f"{pre}{i}"].to_bytes(4, "big") for i in range(n // 4))
    return repr(b.split(b"\0")[0].decode("ascii", "replace"))


def show(x, addr, args):
    """hud.py show k=v ...: the named test_* controls, then force 1"""
    for a in args:
        k, _, v = a.partition("=")
        if k not in SHOW_KEYS or not v and k not in TEXT_KEYS:
            raise SystemExit(f"show: keys are {', '.join(SHOW_KEYS)} (k=v)")
        if k in TEXT_KEYS:
            pre, n = TEXT_KEYS[k]
            for i, w in enumerate(text_words(v, n)):
                setctl(x, addr, f"{pre}{i}", w, quiet=True)
            continue
        setctl(x, addr, SHOW_KEYS[k], VIS[v] if k == "visible" and v in VIS else int(v, 0),
               quiet=True)
    setctl(x, addr, "force", 1)


def main():
    step = sys.argv[1] if len(sys.argv) > 1 else "status"
    addr = block_addr()
    x = Xbdm()
    g = read_block(x, addr)
    if step == "shot":
        import shot  # WP0.2
        return shot.cli(sys.argv[2:], lambda: read_block(Xbdm(), addr),
                        lambda: sentinels(read_block(Xbdm(), addr)))
    if step != "status" and g["version"] < 3:
        raise SystemExit(f"the running xw.xex has an older hud block (version "
                         f"{g['version']}): only status works")
    if step == "status":
        print_status(g)
    elif step == "stage":
        n = int(sys.argv[2])
        if not 0 <= n <= 3:
            raise SystemExit("stage: 0..3")
        setctl(x, addr, "enable", n)
    elif step == "show":
        show(x, addr, sys.argv[2:])
    elif step == "trace":
        nums = [a for a in sys.argv[2:] if not a.startswith("--")]
        trace(x, addr, int(nums[0], 0) if nums else 64, "--wakes" in sys.argv)
    elif step == "hide":
        setctl(x, addr, "force", 0)
    elif step == "never":
        setctl(x, addr, "force", 2)
    elif step == "set":
        setctl(x, addr, sys.argv[2], int(sys.argv[3], 0))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
