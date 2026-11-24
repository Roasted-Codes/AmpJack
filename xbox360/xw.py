#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test helper for the wired plugin (xw.xex) over xbdm (port 730). Run next to out/.

  xw.py install [--no-reboot]
                        install out/xw.xex (and out/xw.presets, out/xw.demo, if there) in
                        Hdd:\\Plugins: the build there now is kept as xw.xex.prev, the new file
                        is read back and compared, then the console reboots (magicboot cold; it sometimes
                        switches off instead: switch it on). --no-reboot: copy and verify only
  xw.py upload          copy out/xw.xex and out/xw_loader.xex to Usb0:\\Apps\\xwired
  xw.py load            start the loader (it loads xw.xex, then launches Aurora)
  xw.py boot on|off     load xw.xex at every boot: a pluginN line in Usb0:\\launch.ini
                        (the old file is backed up first)
  xw.py status          one status dump: driver state, log, controllers
  xw.py watch [SECONDS] follow the log and the controllers
  xw.py window [on|off|reset]
                        the driver loop against the system window (docs/INTERNALS.md): its
                        own time per pass, the window's length, passes the window's end
                        stopped (each cost a frame); on/off: a pass nearing the window's
                        end goes on at priority 20 instead (default off); reset: the
                        longest-pass counters from now
  xw.py debug 0|1|2|3   log level: 1 = every GIP message but input and audio, 2 = with
                        input, 3 = with headset audio packets (1000 a second: floods)
  xw.py audio N         headset audio stage (0 at boot; read when the headset identifies
                        itself, so replug the headset after raising it; lowering takes
                        effect at once): 0 refuse, 1 SET_INTERFACE + open the isochronous
                        endpoints, 2 + one IN block, 3 + continuous mic, 4 + earpiece
                        (voice side), 5 self-test: tone out, mic analysed, no voice side
  xw.py tone HZ         stage 5's tone (default 1000)
  xw.py usbreset [PORT] re-enumerate controller PORT (default 0) as if replugged
  xw.py mecreset        game audio's MEC client unregistered and registered again, as a
                        reload does across two builds (diagnostic)
  xw.py hsrestart [PORT [IN OUT]]
                        restart the headset's audio (picks up a new audio stage): with the
                        format it last accepted (this build's, or handed over by a reload),
                        or IN OUT given (hex: 9 10 = 24 kHz mono mic / 48 kHz stereo
                        earpiece); with neither it is asked to identify itself, which a
                        running headset ignores
  xw.py micdump FILE    record ~1 s of the headset mic into a WAV (needs audio stage >= 3,
                        or 5, and a headset streaming)
  xw.py dump [FILE]     record ~1.5 s of the earpiece as it goes out (after every mix) into
                        a WAV (default ear.wav; needs audio stage 4 and a headset streaming)
  xw.py diag            the headset settings, diagnostics and loop timing (also in status)
  xw.py diag NAME VALUE set one (the panel's MIC VOLUME / NOISE GATE rows write the same):
                        tone off|440|1k         test tone in place of game audio
                        level veryquiet|quiet   its level (-36 / -30 dBFS peak x volume)
                        buffer 30|45|60|90      game audio buffer, ms (ramps)
                        micgain 25|35|50|70|100|140|200|280|400|560
                                                MIC VOLUME (how loud teammates hear you), per
                                                cent: the panel's 1..10, 3 dB a step (default
                                                100 = 5)
                        micdc on|off            remove the mic's DC offset first (default on)
                        gate off|1|2|3|4|auto|low|high
                                                mic noise gate: steps 1..4 open at -60 -55 -50
                                                -45 dBFS (higher cuts more), held 400 ms; low =
                                                2, high = 4 (on an older build its own low /
                                                high); default auto: the threshold follows the
                                                room's noise while `micauto.py gate on`, the
                                                default; otherwise as 2
                        probe sleep0..sleep20|tick
                                                time one xa_sleep_ms(N), or the system clock's
                                                step, on the driver thread, once per pass for
                                                16 passes (results in xw.py diag)
  xw.py go N            XW_STAGED builds: let start-up step N run (1 hooks, 2 USB driver,
                        3 re-offer connected controllers and run, 4 receive one packet,
                        5 receive continuously, 6 process packets, 7 start sending)
  xw.py test            button test: the console ignores the controller meanwhile (test
                        mode), and a checklist ticks off each control as it's used.
                        Ctrl-C ends it (and turns test mode off)
  xw.py raw [SECONDS]   print the extra report bytes (Share, paddles) as they change
  xw.py aurora          go back to Aurora
  xw.py reload [--loader] [--reset] [--no-auth] [--no-build] [--settle] [--step] [--hold [S]]
               [--release-only] [--no-hooks-check]
                        hot reload (docs/HOT-RELOAD.md): build the next slot (xw<n>.xex), have
                        the running build release (it quiesces and stays loaded), load the
                        new one and have it take over the hooks and the controllers, with no
                        reboot and no USB reset, so a controller that is on stays on.
                        The old build loads the new one itself (in place: no title switch,
                        the title on screen keeps running). --loader: the loader title loads
                        it and then launches Aurora (the old way; a title switch while the old
                        build is released froze the console in E1). --reset: re-enumerate the
                        controllers with a USB
                        reset instead of adopting them. --no-build: load out/ as it is.
                        XW_DIR is the console folder (default Usb0:\\Apps\\xwired; with the
                        plugin on the hard disk: XW_DIR='Hdd:\\Plugins')
                        The new build starts with the old one's settings: voice and audio
                        stage, game audio, HUD, balance, master, sidetone, mic gain, noise
                        gate, game buffer, team stage / channel / level, and the mic mute.
                        It stops first if another module patched one of our functions.
                        --settle: take over only once the title has been the same for 10 s
                        (the loader title launches Aurora right after the load).
                        --step: one step at a time through the gates (docs/HOT-RELOAD.md,
                        "Experiments"): R1-R6 in the old build, T1-T11 in the new one; at each
                        it prints the trace and the new log lines and waits for Enter.
                        --hold [S]: the same, but each gate holds S seconds (default 15).
                        --no-auth: adopt, but skip the second security handshake (diagnostic;
                        a Microsoft pad then shows no headset).
                        --release-only: build, upload and release, then stop (diagnostic: the
                        released state alone); xw.py reload --no-build goes on from there
  xw.py handover        the hot-reload state of every xw build that is loaded
  xw.py trace [NAME]    the reload trace ring of every loaded xw build (or NAME, e.g. xw2):
                        the last 64 steps reached, with the time between them
  xw.py gate N [NAME]   raise the reload gate of every loaded xw build (or NAME) to N; 99 = no
                        gate holds (lets a build that an interrupted --step left holding go on)
  xw.py tee [DIR]       run this through every console session: every 50 ms it reads each
                        loaded xw build's log ring and its XW36 / XWAU / XWHO (with the trace
                        words) / XH36 / XV36 blocks, and appends the log, state changes and a
                        heartbeat line a second to DIR/<date-time>.log (default out/logs).
                        Prints "CONSOLE SILENT since <time>" when xbdm stops answering (the
                        last time it did), and "CONSOLE BACK" when it answers again. Ctrl-C ends
  xw.py hooks           the first 4 words of each function xw.xex hooks (4 XInput, 3 XVoiced,
                        VdSwap): original code, our slot jump, or a foreign patch, and the
                        modules that own the slot and the hook; exit status 1 if any is foreign

XBOX=host[:port] is the console's address (required). Shares xbdm code with xa.py.

Development tools need an XW_DEV build (XW_DEV=1 bash toolchain/build.sh; docs/DEVELOPMENT.md):
reload, trace, gate, micdump, usbreset, mecreset, hsrestart, audio 5, tone and diag probe.
On a release build they stop with a message instead of waiting for something that never
happens.
"""
import math
import os
import struct
import sys
import time

from xa import (BUTTONS, HERE, OUT, Xbdm, boot, name_file, pe_va, read_log, scan_block,
                scan_blocks, xw_block, xw_slot)

B = "\\"
FOLDER = "Usb0:" + B + "Apps" + B + "xwired"
BOOT_PATH = "Usb:" + B + "Apps" + B + "xwired" + B + "xw.xex"
MAGIC = 0x58573336  # XW36
STATES = {1: "started", 2: "resolve failed", 3: "kernel code mismatch (not 17559?)",
          4: "waiting (boot)", 5: "running", 6: "released to the next build (hot reload)",
          7: "idle: a slot build that took nothing over"}
HDR_WORDS = 63  # struct xw_status up to pads (static asserts in xw_plugin.c)
OFF_MUTE = 184
OFF_DEBUG, OFF_GO = 40, 44
STAGES = {0: "not loaded / DllMain not reached", 1: "DllMain done", 2: "thread running",
          3: "XInput hooks installed", 4: "USB driver linked", 5: "connected controllers re-offered",
          6: "running"}
PAD_FMT = ">IHHIIIIIIIHBBHHhhhh"  # struct xw_pad_status (xw_plugin.c)
PAD_LEN = struct.calcsize(PAD_FMT)
# struct xw_audio_status, right after the pads (its own version word: AUDIO_VERSION)
AUDIO_FIELDS = (
    "magic stage tone_hz usbreset micdump micdump_addr active_stage port flags "
    "enables refusals disables alt_value alt_status ep_in ep_out max_in max_out desc_source "
    "gep_in gep_out opens open_in open_out closes close_status watchdog "
    "in_queued in_done in_frames_ok in_frames_empty in_frames_missed in_frames_err "
    "in_last_err in_bytes in_dropped in_cancelled "
    "out_queued out_done out_frames_err out_last_err out_dropped out_refused out_inflight "
    "mic_packets mic_bytes mic_pps ear_blocks ear_bps").split()
# then first_status[8] first_len[8] first_bytes[16], tone_amp off_amp off_hz mic_rms mic_peak
# windows, resets, fallback_rx[4], version mic_skips dump dump_ear_addr dump_ear_bytes
# dump_rate dump_channels, release, reserved[1]
AUDIO_FMT = ">" + "I" * len(AUDIO_FIELDS) + "8I8H16B6II4I7I2I"
AUDIO_LEN = struct.calcsize(AUDIO_FMT)
assert AUDIO_LEN == 85 * 4
AUDIO_VERSION = 3  # xw_plugin.c AUD_STATUS_VERSION (the word was ear_fb in older builds)
OFF_AUDIO = 4 * HDR_WORDS + 4 * PAD_LEN
AUDIO_MAGIC = 0x58574155  # XWAU
OFF_AUD_STAGE, OFF_AUD_TONE, OFF_AUD_RESET, OFF_AUD_MICDUMP = (OFF_AUDIO + 4, OFF_AUDIO + 8,
                                                               OFF_AUDIO + 12, OFF_AUDIO + 16)
OFF_AUD_DUMP = OFF_AUDIO + 78 * 4
# struct xw_status after the audio block: the driver loop (loop_us, loop_max_us), then
# (XW36 version 3) the system window (docs/INTERNALS.md; xw_priv.h)
OFF_LOOP = OFF_AUDIO + AUDIO_LEN
LOOP_FIELDS = "loop_us loop_max_us win_us run_us run_max_us win_cuts win_boost win_boosts".split()
AUD_FLAGS = [(1, "kernel ok"), (2, "IN open"), (4, "OUT open"), (8, "streaming"),
             (16, "voice told"), (32, "BROKEN"), (64, "headset ready")]
MICDUMP_MAGIC = 0x58574D44  # XWMD
# struct xd_status (diag_360.h): its own block, found by its magic like the XW36 block
DIAG_MAGIC = 0x58574447  # XWDG
DIAG_VERSION = 5  # diag_360.h XD_VERSION (5: gate low -55 / high -45, default auto)
DIAG_VERSIONS = (4, 5)  # read alike (layout as 3); only the gate's numbers differ
DIAG_FIELDS = (
    "magic version tone tone_level buffer_ms mic_gain gate "
    "applied_tone applied_tone_level applied_buffer_ms applied_mic_gain applied_gate "
    "tone_hz tone_peak target_frames high_frames drift_full_frames "
    "mic_gain_q15 gate_open gate_rms gate_threshold gate_opens mic_vu "
    "out_frames_missed out_last_status out_starved "
    "flow_below flow_equal flow_above flow_min flow_max flow_last flow_fragment "
    "pass_us pass_max_us work_us work_max_us "
    "pass_hist0 pass_hist1 pass_hist2 pass_hist3 pass_hist4 pass_hist5 "
    "tb_per_ms passes_per_s probe probe_kind probe_n probe_min_us probe_avg_us probe_max_us "
    "probe_step_100ns probe_runs gap_us gap_max_us mic_dc mic_dc_now").split()
DIAG_WORDS = 57  # XD_WORDS, _Static_assert in diag_360.h
assert len(DIAG_FIELDS) == DIAG_WORDS
MIC_GAINS = (25, 35, 50, 70, 100, 140, 200, 280, 400, 560)  # MIC VOLUME 1..10 (diag_360.h)
MIC_DB = (-12, -9, -6, -3, 0, 3, 6, 9, 12, 15)
PASS_HIST = ["<4", "4-8", "8-12", "12-17", "17-25", ">=25"]  # ms
DIAG_TONES = ["off", "440 Hz", "1 kHz"]
DIAG_LEVELS = ["very quiet (-36 dBFS)", "quiet (-30 dBFS)"]
DIAG_GATES = ["off", "1 (-60 dBFS)", "2 (-55 dBFS)", "auto", "3 (-50 dBFS)", "4 (-45 dBFS)"]
DIAG_GATES_V4 = ["off", "low (-45 dBFS)", "high (-35 dBFS)", "auto"]  # XWDG version 4 builds
DIAG_GATE_SET_V4 = {"off": 0, "low": 1, "high": 2, "auto": 3}
PROBE_SLEEP, PROBE_TICK = 0x100, 0x200
# xw.py diag NAME VALUE: the control and its accepted values
DIAG_SET = {
    "tone": ("tone", {"off": 0, "440": 1, "1k": 2, "1000": 2}),
    "level": ("tone_level", {"veryquiet": 0, "quiet": 1}),
    "buffer": ("buffer_ms", {"30": 30, "45": 45, "60": 60, "90": 90}),
    "micgain": ("mic_gain", {str(v): v for v in MIC_GAINS}),
    "micdc": ("mic_dc", {"on": 1, "off": 0}),
    # the steps by stored number (diag_360.h); low / high: the -55 / -45 dBFS this package gave
    # them before the steps came (on a version 4 build: its own low / high)
    "gate": ("gate", {"off": 0, "1": 1, "2": 2, "3": 4, "4": 5, "auto": 3, "low": 2, "high": 5}),
    "probe": ("probe", dict([(f"sleep{n}", PROBE_SLEEP + n) for n in range(21)] +
                            [("tick", PROBE_TICK)])),
}
DIAG_APPLIED = ("tone", "tone_level", "buffer_ms", "mic_gain", "gate")  # have applied_*


def status_addr():
    return xw_block(MAGIC)


def diag_addr():
    return xw_block(DIAG_MAGIC)


def need_dev(what):
    """stops `what` on a release build of xw.xex: its development machinery (hot reload, the
    audio self-test, micdump, usbreset / hsrestart / mecreset, the timing probe) is not in it,
    and a command for it would only wait for an answer that never comes"""
    s = read_status(Xbdm(), status_addr())
    if s and s["audio"] and s["audio"]["release"]:
        raise SystemExit(f"xw.py {what} needs an XW_DEV build (XW_DEV=1 bash toolchain/build.sh); "
                         "the build running on the console is a release build")


def read_diag(x, quiet=False):
    """the XWDG block as a dict; None if absent or of a version this xw.py doesn't know
    (it is never read with another layout)"""
    try:
        addr = diag_addr()
    except SystemExit:
        return None
    w = x.u32s(addr, DIAG_WORDS)
    if not w or w[0] != DIAG_MAGIC:
        return None
    if w[1] not in DIAG_VERSIONS:
        if not quiet:
            print(f"diagnostics: XWDG block version {w[1]}, this xw.py reads versions "
                  f"{', '.join(map(str, DIAG_VERSIONS))} only (use the xw.py of that build)")
        return None
    d = dict(zip(DIAG_FIELDS, w))
    d["pass_hist"] = [d.pop(f"pass_hist{i}") for i in range(6)]
    return d


def pick(names, i):
    return names[i] if i < len(names) else f"?{i}"


def print_diag(d):
    if not d:
        print(f"diagnostics: no XWDG version {DIAG_VERSION} block (other build, or out/xw.exe "
              "isn't the loaded one)")
        return
    g = d["applied_mic_gain"]
    lvl = f"level {MIC_GAINS.index(g) + 1}, {MIC_DB[MIC_GAINS.index(g)]:+d} dB" \
        if g in MIC_GAINS else "?"
    dc = d["mic_dc_now"] - (1 << 32) if d["mic_dc_now"] & 0x80000000 else d["mic_dc_now"]
    print(f"settings: mic volume {g}% ({lvl}; now {d['mic_gain_q15'] * 100 // 32768}%), DC removal "
          f"{'on (offset ' + str(dc) + ')' if d['mic_dc'] else 'off'}, "
          f"noise gate {pick(DIAG_GATES if d['version'] >= 5 else DIAG_GATES_V4, d['applied_gate'])}; test tone "
          f"{pick(DIAG_TONES, d['applied_tone'])}, level {pick(DIAG_LEVELS, d['applied_tone_level'])} "
          f"(now {d['tone_hz']} Hz, peak {d['tone_peak']})")
    vu = min(d["mic_vu"], 10)
    thr = d["gate_threshold"]
    thr_db = f" ({20 * math.log10(thr / 32768):.0f} dBFS)" if thr else ""
    print(f"  mic: gate {'open' if d['gate_open'] else 'closed'}, rms {d['gate_rms']} / threshold "
          f"{thr}{thr_db}, opened {d['gate_opens']} times; meter {d['mic_vu']}/10 "
          f"[{'#' * vu}{'.' * (10 - vu)}]")
    if d["applied_gate"] == 3:
        print("  (gate auto runs as low unless NOISE GATE AUTO is in effect: micauto.py status)")
    print(f"  buffer: game {d['applied_buffer_ms']} ms (target now {d['target_frames']} frames = "
          f"{d['target_frames'] / 48:.1f} ms, rebuffer above {d['high_frames']}, full trim at "
          f"{d['drift_full_frames']}); OUT blocks in flight: 5 (fixed)")
    print(f"  OUT stream: frames missed (not accessed) {d['out_frames_missed']}, last non-zero "
          f"frame status {d['out_last_status']:08x}, starved completions {d['out_starved']}")
    print(f"  mic packet flow field vs {d['flow_fragment']} bytes per earpiece packet: below "
          f"{d['flow_below']}, equal {d['flow_equal']}, above {d['flow_above']}; min "
          f"{d['flow_min']}, max {d['flow_max']}, last {d['flow_last']}")
    hist = ", ".join(f"{n} ms {c}" for n, c in zip(PASS_HIST, d["pass_hist"]))
    print(f"  driver loop: pass {d['pass_us']} us (max {d['pass_max_us']}) = work in audio_poll + "
          f"xv_poll {d['work_us']} us (max {d['work_max_us']}) + the rest (sleep, panel, hud, "
          f"ports) {d['gap_us']} us (max {d['gap_max_us']}); {d['passes_per_s']} passes/s by "
          f"the system clock; timebase {d['tb_per_ms']} ticks/ms (50000 expected)")
    print(f"  passes: {hist}")
    if d["probe_runs"]:
        k = d["probe_kind"]
        what = "system time step" if k == PROBE_TICK else f"xa_sleep_ms({k & 0xFF})"
        extra = f", last step {d['probe_step_100ns']} x 100 ns" if k == PROBE_TICK else ""
        print(f"  probe ({d['probe_runs']} run(s)): {what}: avg {d['probe_avg_us']} us, min "
              f"{d['probe_min_us']}, max {d['probe_max_us']} over {d['probe_n']}{extra}"
              + (" (another running)" if d["probe"] else ""))
    elif d["probe"]:
        print(f"  probe {d['probe']:x}: running")
    pending = [n for n in DIAG_APPLIED if d[n] != d["applied_" + n]]
    if pending:
        print("  not applied yet: " + ", ".join(f"{n} = {d[n]}" for n in pending))


def diag_usage():
    short = {"micgain": "25|35|..|560", "probe": "sleep0..sleep20|tick"}
    return "usage: xw.py diag " + " | ".join(
        f"{k} {short.get(k) or '|'.join(v[1])}" for k, v in DIAG_SET.items())


def set_diag(args):
    if args and args[0] == "probe":
        need_dev("diag probe")
    if len(args) != 2 or args[0] not in DIAG_SET or args[1] not in DIAG_SET[args[0]][1]:
        raise SystemExit(diag_usage())
    field, values = DIAG_SET[args[0]]
    x = Xbdm()
    d = read_diag(x)
    if not d:
        raise SystemExit(f"no XWDG version {DIAG_VERSION} block (plugin loaded? out/xw.exe the "
                         "loaded build?)")
    if field == "gate" and d["version"] < 5:
        values = DIAG_GATE_SET_V4
        if args[1] not in values:
            raise SystemExit(f"gate {args[1]}: an XWDG version {d['version']} build has only "
                             + "|".join(values))
    print(field, "=", values[args[1]],
          x.setu32(diag_addr() + 4 * DIAG_FIELDS.index(field), values[args[1]]))


def read_status(x, addr):
    w = x.u32s(addr, HDR_WORDS)
    if not w or w[0] != MAGIC:
        return None
    raw = x.mem(addr + 4 * HDR_WORDS, PAD_LEN * 4)
    pads = []
    for i in range(4):
        (conn, vid, pid, rx, tx, rxd, rxe, txd, txe, lafter, buttons, guide, _, lt, rt,
         lx, ly, rx_, ry) = struct.unpack_from(PAD_FMT, raw, PAD_LEN * i)
        pads.append(dict(conn=conn, vid=vid, pid=pid, rx=rx, tx=tx, rxd=rxd, rxe=rxe, txd=txd,
                         txe=txe, lafter=lafter, buttons=buttons, guide=guide, lt=lt, rt=rt,
                         lx=lx, ly=ly, rx_=rx_, ry=ry))
    s32 = lambda v: v - (1 << 32) if v & 0x80000000 else v
    audio = None
    if w[1] >= 2:
        raw = x.mem(addr + OFF_AUDIO, AUDIO_LEN)
        if raw and len(raw) == AUDIO_LEN:
            v = struct.unpack(AUDIO_FMT, raw)
            n = len(AUDIO_FIELDS)
            audio = dict(zip(AUDIO_FIELDS, v[:n]))
            audio["first_status"] = v[n:n + 8]
            audio["first_len"] = v[n + 8:n + 16]
            audio["first_bytes"] = bytes(v[n + 16:n + 32])
            (audio["tone_amp"], audio["off_amp"], audio["off_hz"], audio["mic_rms"],
             audio["mic_peak"], audio["windows"]) = v[n + 32:n + 38]
            audio["resets"] = v[n + 38]
            audio["fallback_rx"] = v[n + 39:n + 43]
            (audio["version"], audio["mic_skips"], audio["dump"], audio["dump_ear_addr"],
             audio["dump_ear_bytes"], audio["dump_rate"], audio["dump_channels"]) = v[n + 43:n + 50]
            audio["release"] = v[n + 50]  # 1: a release build (0 in every build before it)
            if audio["magic"] != AUDIO_MAGIC:
                audio = None
            elif audio["version"] != AUDIO_VERSION:
                # never read with another layout: an older build has ear_fb (0) here
                print(f"audio: XWAU block version {audio['version']}, this xw.py reads version "
                      f"{AUDIO_VERSION} only (use the xw.py of that build)")
                audio = None
    return dict(audio=audio, state=w[2], err=s32(w[3]), log=w[4], matches=w[5], adds=w[6], removes=w[7],
                declined=w[8], heartbeat=w[9], go=w[11], stage=w[12], in_done=w[13],
                in_status=w[14], in_returning=w[15], in_returned=w[16],
                in_first=" ".join(f"{b:02x}" for b in struct.pack(">4I", *w[17:21])), stray=w[21],
                block_after=w[22:34], block_now=w[34:46], mute=w[46],
                last_input=struct.pack(">16I", *w[47:63]), pads=pads)


def read_loop(x, addr=None):
    """the driver loop's timing and the system window (XW36 version 3), or None for an older
    build (it has loop_us / loop_max_us only)"""
    addr = status_addr() if addr is None else addr
    w = x.u32s(addr, 2)
    if not w or w[0] != MAGIC or w[1] < 3:
        return None
    return dict(zip(LOOP_FIELDS, x.u32s(addr + OFF_LOOP, len(LOOP_FIELDS))))


def print_loop(lp):
    if not lp:
        print("driver loop: no window counters (XW36 before version 3)")
        return
    win = f"{lp['win_us']} us window" if lp["win_us"] else "window not read (not 17559's code)"
    print(f"driver loop: pass {lp['loop_us']} us (longest {lp['loop_max_us']}); its own time "
          f"{lp['run_us']} us (longest {lp['run_max_us']}) of the {win} a frame; stopped by the "
          f"window's end {lp['win_cuts']} times; boost {'ON' if lp['win_boost'] else 'off'} "
          f"({lp['win_boosts']} passes went on at priority 20)")


def pad_line(i, p):
    names = [n for bit, n in BUTTONS if p["buttons"] & bit] + (["Guide"] if p["guide"] else [])
    return (f"port {i} {p['vid']:04x}:{p['pid']:04x}: {' '.join(names) or '-':<24} "
            f"LT {p['lt']:4} RT {p['rt']:4} L {p['lx']:6},{p['ly']:6} R {p['rx_']:6},{p['ry']:6}")


def print_status(s):
    print(f"state {s['state']} ({STATES.get(s['state'], '?')}), last error {s['err']}, "
          f"match calls {s['matches']}, adds {s['adds']}, removes {s['removes']}, "
          f"declined {s['declined']}, heartbeat {s['heartbeat']}")
    print(f"start-up: stage {s['stage']} ({STAGES.get(s['stage'], '?')}), gate open to {s['go']}")
    print(f"IN completions {s['in_done']} (last status {s['in_status']:08x}, callback returning "
          f"{s['in_returning']}, returned {s['in_returned']}), first packet: {s['in_first']}")
    blk = lambda ws: " ".join(f"{v:08x}" for v in ws)
    print(f"stray completions {s['stray']}")
    print(f"IN block now:   {blk(s['block_now'])}")
    print(f"IN block after: {blk(s['block_after'])}  (first completion)")
    for i, p in enumerate(s["pads"]):
        if p["conn"]:
            print(pad_line(i, p))
            print(f"         usb in {p['rx']} (dropped {p['rxd']}, errors {p['rxe']}), "
                  f"out {p['tx']} (dropped {p['txd']}, errors {p['txe']}), "
                  f"IN length field after completion {p['lafter']}")
    if s["audio"]:
        print_audio(s["audio"])


def print_audio(a):
    port = "none" if a["port"] == 0xFFFFFFFF else str(a["port"])
    flags = ", ".join(n for bit, n in AUD_FLAGS if a["flags"] & bit) or "-"
    src = {1: "config descriptor", 2: "defaults"}.get(a["desc_source"], "?")
    print(f"audio: stage {a['stage']} (active {a['active_stage']}), port {port}, {flags}; "
          f"enables {a['enables']}, refused {a['refusals']}, disables {a['disables']}")
    print(f"  SET_INTERFACE 1/{a['alt_value']}: {a['alt_status']:08x}; endpoints IN "
          f"{a['ep_in']:02x} ({a['max_in']} bytes) {a['open_in']:08x}, OUT {a['ep_out']:02x} "
          f"({a['max_out']} bytes) {a['open_out']:08x}, from {src}; opens {a['opens']}, "
          f"closes {a['closes']} (last {a['close_status']:08x}), forced {a['watchdog']}; "
          f"UsbdGetEndpointDescriptor(type 1) {a['gep_in']:08x}/{a['gep_out']:08x}")
    print(f"  IN: queued {a['in_queued']}, done {a['in_done']}, frames ok {a['in_frames_ok']} "
          f"empty {a['in_frames_empty']} missed {a['in_frames_missed']} errors "
          f"{a['in_frames_err']} (last {a['in_last_err']:08x}), {a['in_bytes']} bytes, "
          f"dropped {a['in_dropped']}, cancelled blocks {a['in_cancelled']}")
    print(f"  OUT: queued {a['out_queued']}, done {a['out_done']}, in flight {a['out_inflight']}, "
          f"frame errors {a['out_frames_err']} (last {a['out_last_err']:08x}), dropped "
          f"{a['out_dropped']}, refused {a['out_refused']}")
    print(f"  mic {a['mic_packets']} packets ({a['mic_pps']}/s, {a['mic_bytes']} bytes), "
          f"earpiece {a['ear_blocks']} blocks ({a['ear_bps']}/s)")
    print("  first IN block: " + " ".join(f"{st:08x}/{ln}" for st, ln in
                                          zip(a["first_status"], a["first_len"])) +
          "  bytes " + a["first_bytes"].hex(" "))
    ratio = a["tone_amp"] / a["off_amp"] if a["off_amp"] else float("inf")
    print(f"  self-test ({a['tone_hz']} Hz): tone {a['tone_amp']}, off-tone ({a['off_hz']} Hz) "
          f"{a['off_amp']} (ratio {ratio:.1f}), rms {a['mic_rms']}, peak {a['mic_peak']}, "
          f"windows {a['windows']}")
    fb = ["-" if v == 0xFFFFFFFF else str(v) for v in a["fallback_rx"]]
    print(f"  usb resets {a['resets']}; IN packets after a blind start, per port: {' '.join(fb)}")
    print(f"  mic frames skipped by the headset (no packet in a 1 ms frame): {a['mic_skips']}")


def dump(path):
    """~1.5 s of the earpiece as it goes out, after every mix (game, chat, sidetone, tone),
    into a WAV (s16le at the earpiece's rate and channels)"""
    import wave
    addr = status_addr()
    s = read_status(Xbdm(), addr)
    if not s or not s["audio"]:
        raise SystemExit("no XWAU block this xw.py can read (plugin loaded? this build?)")
    Xbdm().setu32(addr + OFF_AUD_DUMP, 1)
    end = time.time() + 6
    a = None
    while time.time() < end:
        time.sleep(0.3)
        a = read_status(Xbdm(), addr)["audio"]
        if a["dump"] == 3:
            break
    else:
        Xbdm().setu32(addr + OFF_AUD_DUMP, 0)
        raise SystemExit(f"earpiece dump not done within 6 s (state {a['dump'] if a else '?'}, "
                         f"{a['dump_ear_bytes'] if a else 0} bytes): audio stage 4 and a "
                         "headset streaming?")
    n, at = a["dump_ear_bytes"], a["dump_ear_addr"]
    data = b""
    while len(data) < n:
        chunk = Xbdm().mem(at + len(data), min(0x1000, n - len(data)))
        if not chunk:
            raise SystemExit("getmem failed")
        data += chunk
    Xbdm().setu32(addr + OFF_AUD_DUMP, 0)
    with wave.open(path, "wb") as f:
        f.setnchannels(a["dump_channels"] or 2)
        f.setsampwidth(2)
        f.setframerate(a["dump_rate"] or 24000)
        f.writeframes(data)  # s16 little-endian, as WAV wants
    print(f"{path}: {n} bytes, {a['dump_rate']} Hz x{a['dump_channels']}")


def micdump(path):
    import wave
    need_dev("micdump")
    addr = status_addr()
    x = Xbdm()
    w = x.u32s(addr, 2)
    if not w or w[0] != MAGIC or w[1] < 2:
        raise SystemExit("plugin not found, or too old for micdump")
    print(x.setu32(addr + OFF_AUD_MICDUMP, 1), "-> recording (needs audio stage >= 3 and "
          "a headset streaming)")
    for _ in range(100):
        time.sleep(0.2)
        st = Xbdm().u32s(addr + OFF_AUD_MICDUMP, 2)
        if st and st[0] == 3:
            break
    else:
        raise SystemExit("no mic data within 20 s (xw.py status: mic packets?)")
    dump = st[1]
    x = Xbdm()
    magic, rate, ch, n, _ = x.u32s(dump, 5)
    if magic != MICDUMP_MAGIC:
        raise SystemExit(f"no mic dump at {dump:08x}")
    data = b""
    while len(data) < n:
        chunk = x.mem(dump + 20 + len(data), min(0x1000, n - len(data)))
        if not chunk:
            raise SystemExit("getmem failed")
        data += chunk
    with wave.open(path, "wb") as f:
        f.setnchannels(ch or 1)
        f.setsampwidth(2)
        f.setframerate(rate or 24000)
        f.writeframes(data)  # the headset's PCM is s16 little-endian, as WAV wants
    print(f"{path}: {n} bytes, {rate} Hz x{ch}")


CHECKS = ["A", "B", "X", "Y", "Up", "Down", "Left", "Right", "LB", "RB", "LT", "RT", "LS", "RS",
          "View", "Menu", "Guide", "L stick full circle", "R stick full circle"]


def button_test():
    """live checklist of every control on port 0, with the console's view of it muted"""
    addr = status_addr()
    seen, base, extra = set(), None, {}
    quadrants = {"L": set(), "R": set()}
    Xbdm().setu32(addr + OFF_MUTE, 1)
    print("test mode on: the console ignores the controller. Press everything; Ctrl-C ends.")
    try:
        last = None
        while True:
            try:
                st = read_status(Xbdm(), addr)
            except OSError as e:
                print(f"xbdm: {e}")
                time.sleep(1)
                continue
            if not st:
                raise SystemExit("plugin not found (loaded?)")
            p = st["pads"][0]
            if not p["conn"]:
                time.sleep(0.2)
                continue
            for bit, name in BUTTONS:
                if p["buttons"] & bit:
                    seen.add(name)
            if p["guide"]:
                seen.add("Guide")
            if p["lt"] > 900:
                seen.add("LT")
            if p["rt"] > 900:
                seen.add("RT")
            for side, x, y in (("L", p["lx"], p["ly"]), ("R", p["rx_"], p["ry"])):
                if abs(x) > 20000 or abs(y) > 20000:
                    quadrants[side].add((x > 0, y > 0))
                if len(quadrants[side]) == 4:
                    seen.add(f"{side} stick full circle")
            # bytes past the standard report (4-byte header + 14 bytes): extra buttons
            raw = st["last_input"]
            if raw[0] == 0x20:
                if base is None:
                    base = raw
                for i in range(18, 64):
                    if raw[i] != base[i]:
                        extra.setdefault(i, set()).add(raw[i])
            line = ("  ".join(("[x] " if c in seen else "[ ] ") + c for c in CHECKS) +
                    ("\n  extra report bytes that changed: " +
                     ", ".join(f"byte {i}: {sorted(f'{v:02x}' for v in vals)}"
                               for i, vals in sorted(extra.items())) if extra else ""))
            if line != last:
                print(f"\n{time.strftime('%H:%M:%S')}  {len(seen)}/{len(CHECKS)}\n  {line}", flush=True)
                last = line
            if len(seen) == len(CHECKS):
                print("\nall controls seen")
                break
            time.sleep(0.15)
    except KeyboardInterrupt:
        pass
    finally:
        print("test mode", Xbdm().setu32(addr + OFF_MUTE, 0), "-> off")
        missing = [c for c in CHECKS if c not in seen]
        print("not seen:", ", ".join(missing) or "none")
        if extra:
            print("extra bytes (centre button / paddles?):",
                  {i: sorted(f"{v:02x}" for v in vals) for i, vals in extra.items()})


NOISY = set(range(40, 47)) - {43}  # counters/sensor data in the SCUF's report, not buttons


def raw_test(secs):
    """print each byte past the standard report as it changes (test mode on meanwhile)"""
    addr = status_addr()
    Xbdm().setu32(addr + OFF_MUTE, 1)
    print("test mode on. Press the extra buttons one at a time, a second apart.", flush=True)
    last, until = None, time.time() + secs
    try:
        while time.time() < until:
            try:
                st = read_status(Xbdm(), addr)
            except OSError:
                time.sleep(0.5)
                continue
            raw = st["last_input"]
            if raw[0] != 0x20:
                time.sleep(0.1)
                continue
            cur = {i: raw[i] for i in range(18, 64) if i not in NOISY}
            if last is not None:
                diff = {i: (last[i], cur[i]) for i in cur if cur[i] != last[i]}
                if diff:
                    print(time.strftime("%H:%M:%S"), "  ".join(
                        f"byte {i}: {a:02x}->{b:02x}" for i, (a, b) in sorted(diff.items())), flush=True)
            last = cur
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        print("test mode", Xbdm().setu32(addr + OFF_MUTE, 0), "-> off", flush=True)


# ---- hot reload (docs/HOT-RELOAD.md; struct xw_handover in handover.h) ----------------------
HO_MAGIC = 0x5857484F  # XWHO
HO_VERSION = 1
HO_FIELDS = ("magic version size state cmd_release cmd_takeover cmd_mode slot refuse refusals "
             "releases take_state take_error adopted resets skipped inherited detours "
             "load_status").split()
# WP3.1 (handover.h): in what were reserved[0..6]; zeros in a build from before it
HO_EXT = "trace trace_n trace_ring cmd_gate gate_at title in_double".split()
HO_FIELDS += HO_EXT
HO_OFF = {f: 4 * i for i, f in enumerate(HO_FIELDS)}
assert HO_OFF["trace"] == 19 * 4  # _Static_assert in handover.h
HO_OFF_PATH = 27 * 4   # static asserts in handover.h
HO_OFF_PORTS = 27 * 4 + 128
HO_PORT_WORDS = 26
HO_PORT_FIELDS = ("node driver vid pid ep_in ep_out max_in max_out interval_in interval_out "
                  "h_in h_out h_ctrl aud_ep_in aud_ep_out aud_max_in aud_max_out aud_desc flags "
                  "in_entry in_busy in_fwd hs_fmt").split()
HO_STATES = {1: "starting", 2: "running", 3: "releasing", 4: "released", 5: "idle (took nothing over)"}
HO_RUNNING, HO_RELEASED, HO_IDLE = 2, 4, 5
HO_REFUSE = {1: "the driver is still starting up",
             2: "headset audio is broken (a USB request never completed): only a reboot is safe",
             3: "the headset's audio endpoints didn't close, or audio blocks are still queued",
             4: "a controller is being removed right now (try again)",
             5: "a controller is still being set up (try again)",
             6: "the XAM thread is still inside XAM (a system dialog open? try again)",
             7: "a controller's outgoing packets didn't drain",
             8: "the hand-over routine never ran on Usbd's DPC",
             9: "Usbd's record of a controller doesn't name the running build's driver"}
HO_TAKE = {0: "none", 1: "hooks inherited", 2: "done", 3: "FAILED", 4: "no previous build"}
HO_T_DONE, HO_T_FAILED = 2, 3
HO_TAKE_ERR = {1: "the address given isn't mapped", 2: "no handover block at that address",
               3: "the previous build's handover block is another version",
               4: "the previous build hasn't released", 5: "xw.py never said what to take over",
               6: "the adoption never ran on Usbd's DPC (controllers stay with the old build)"}
# ---- the reload trace and gates (WP3.1; g_trace and TR_* in xw_plugin.c, struct xw_trace) ----
TRACE_MAGIC, TRACE_RING = 0x58575452, 64  # "XWTR"; _Static_assert in handover.h
TRACE_NAMES = {}
for _base, _names in (
        (0x100, "REL_BEGIN REL_CHECKED REL_AUDIO_DRAINED REL_AUDIO_OFF REL_GAME_QUIESCED "
                "REL_PADS_RELEASED REL_QUIESCED REL_XAM_DRAINED REL_TX_DRAINED REL_COMMIT "
                "REL_COMMITTED REL_XINPUT_QUIESCED REL_DONE REL_LOAD_NEXT REL_REFUSED REL_RESUMED "
                "REL_LOADED"),
        (0x180, "COMMIT_BEGIN COMMIT_REFUSED COMMIT_UNLINKED COMMIT_DONE"),
        (0x200, "TAKE_WAIT TAKE_CMD TAKE_IMPORTED TAKE_HOOKS TAKE_HOOKED TAKE_LINK TAKE_LINKED "
                "TAKE_ADOPT TAKE_ADOPTED TAKE_RUNNING TAKE_FAILED TAKE_NOTIFY"),
        (0x280, "ADOPT_BEGIN ADOPT_PORT ADOPT_DONE ADOPT_SKIP ADOPT_RESET"),
        (0x300, "LINK_DPC IN_FWD IN_FWD_QUEUED FIRST_IN FIRST_OUT IN_DOUBLE REMOVE REMOVE_OTHER "
                "AUD_OPEN AUD_OPENED IN_HANDED AUD_CLOSE AUD_CLOSED AUD_FORCE "
                "ISO_IN_FIRST ISO_OUT_FIRST ISO_STRAY AUD_ALT MEC_RESET MEC_RESUMED"),
        (0x380, "PORT_GC PORT_ADOPT_SENT PAD_BIND PAD_BOUND"),
        (0x400, "GATE_HOLD GATE_OPEN GATE_TIMEOUT")):
    for _i, _n in enumerate(_names.split()):
        TRACE_NAMES[_base + _i] = _n
# what each gate lets run (the same text the build logs); R = the releasing build, T = the
# build taking over. T8-T11 are xw.py's: the new build's own knobs, held at 0 until then
GATES_R = {1: "drain the headset's audio blocks",
           2: "headset off: audio endpoints closed, interface 1 to alternate 0",
           3: "game audio off: MEC client unregistered",
           4: "pads and headsets unbound in XAM, XAM thread drained",
           5: "commit on Usbd's DPC (class driver unlinked, controllers on offer)",
           6: "the XAM thread and the driver thread end"}
GATES_T = {1: "check the previous build's block, inherit its hooks",
           2: "XInput: XAM thread started, the 4 hook slots stored",
           3: "link the USB class driver (Usbd's DPC)",
           4: "adopt the controllers (adopt_dpc on Usbd's DPC)",
           5: "serve the adopted controller: gip client, IN started",
           6: "ask the adopted controller to identify itself (gip_adopt)",
           7: "bind the pad in XAM",
           8: "XVoiced: voice stage back to {v} (the 3 hook slots are stored at stage >= 1)",
           9: "headset audio stage back to {v}, headset asked again (SET_INTERFACE, iso open, "
              "stream)",
           10: "game audio enable back to {v} (MEC registered)",
           11: "HUD enable back to {v} (VdSwap slot stored)"}


def gate_name(v):
    return f"{chr(v >> 8)}{v & 0xFF}" if v and chr(v >> 8) in "RT" else (f"{v:#x}" if v else "-")


def trace_text(code):
    name = TRACE_NAMES.get(code & 0xFFFF, f"{code & 0xFFFF:#06x}")
    arg = code >> 16
    if name.startswith("GATE_"):
        return f"{name} {gate_name(arg)}"
    if name == "TAKE_FAILED":
        return f"{name} ({HO_TAKE_ERR.get(arg, arg)})"
    if name.startswith(("ADOPT_P", "ADOPT_S", "ADOPT_R", "IN_", "FIRST_", "REMOVE", "AUD_", "PORT_")):
        return f"{name} port {arg}" if arg != 0xFF else f"{name} (no port)"
    if name.startswith("PAD_"):
        return f"{name} pad {arg}"
    return name + (f" {arg}" if arg else "")


def read_trace(x, h, seen=None):
    """the trace entries of the build whose handover block is h (read_ho) written after the
    first `seen` (None = all the ring still has): (entries, trace_n, lost), entries =
    [(index, code, timebase)] oldest first"""
    n, ring = h.get("trace_n", 0), h.get("trace_ring", 0)
    if not ring:
        return [], n, 0
    first = max(n - TRACE_RING, 0 if seen is None else seen)
    lost = max(0, n - TRACE_RING - (seen or 0)) if seen is not None else 0
    if first >= n:
        return [], n, lost
    w = x.u32s(ring, 4 + 2 * TRACE_RING)
    if not w or w[0] != TRACE_MAGIC:
        return [], n, lost
    e = w[4:]
    return [(i, e[2 * (i % TRACE_RING)], e[2 * (i % TRACE_RING) + 1]) for i in range(first, n)], n, lost


def trace_lines(entries, prev_tb=None):
    """the entries as text, each with the time since the one before it (timebase 50 MHz)"""
    out = []
    for i, code, tb in entries:
        dt = "" if prev_tb is None else f" (+{((tb - prev_tb) & 0xFFFFFFFF) / 50000:.1f} ms)"
        out.append(f"trace {i}: {code:08x} {trace_text(code)}{dt}")
        prev_tb = tb
    return out, prev_tb


LOG_MAGIC = b"XLOG"
DEVICES = {"usb0:": B + "Device" + B + "Mass0", "hdd:": B + "Device" + B + "Harddisk0" + B + "Partition1"}


def console_dir():
    return os.environ.get("XW_DIR", FOLDER).rstrip(B)


def read_ho(x, addr):
    w = x.u32s(addr, len(HO_FIELDS))
    if not w or w[0] != HO_MAGIC:
        return None
    h = dict(zip(HO_FIELDS, w))
    h["addr"] = addr
    h["ports"] = []
    raw = x.u32s(addr + HO_OFF_PORTS, 4 * HO_PORT_WORDS)
    for i in range(4):
        h["ports"].append(dict(zip(HO_PORT_FIELDS, raw[i * HO_PORT_WORDS:])) if raw else {})
    return h


def find_ho(x, base, size):
    """a loaded xw build's handover block, found in its memory (never through out/, which may
    have been rebuilt since that build was loaded); None = a build without hot reload"""
    def ok(addr):
        w = x.u32s(addr, 3)
        return bool(w) and w[1] in range(1, 100) and 0x100 <= w[2] <= 0x2000
    addr = scan_block(x, base, size, struct.pack(">I", HO_MAGIC), ok)
    return read_ho(x, addr) if addr else None


def xw_builds(x):
    """[(slot, name, base, size)] of the loaded xw builds, oldest first"""
    out = [(xw_slot(n), n, b, s) for n, (b, s) in x.module_map().items() if xw_slot(n) is not None]
    return sorted(out)


def log_tail(x, base, size, lines=25):
    ring = scan_block(x, base, size, LOG_MAGIC)
    if not ring:
        return ""
    text = read_log(x, ring, 0)[1]
    return "\n".join("    " + l for l in text.splitlines()[-lines:])


def print_ho(name, h):
    print(f"{name}: handover v{h['version']} at {h['addr']:08x}, slot {h['slot']}, "
          f"{HO_STATES.get(h['state'], h['state'])}; releases {h['releases']}, refused "
          f"{h['refusals']}" + (f" (last: {HO_REFUSE.get(h['refuse'], h['refuse'])})" if h["refusals"] else "")
          + f"; take-over {HO_TAKE.get(h['take_state'], h['take_state'])}"
          + (f" ({HO_TAKE_ERR.get(h['take_error'], h['take_error'])})" if h["take_error"] else "")
          + f": {h['adopted']} adopted, {h['resets']} reset, {h['skipped']} left alone, "
          f"{h['inherited']} hooks inherited")
    if h.get("trace_ring"):
        print(f"  trace: last step {trace_text(h['trace'])} ({h['trace_n']} steps); gate "
              f"{h['cmd_gate']}" + (f", holding at {gate_name(h['gate_at'])}" if h["gate_at"] else "")
              + f"; in_double {h['in_double']}" + (" (MUST be 0)" if h["in_double"] else ""))
    for i, p in enumerate(h["ports"]):
        if p and p["node"]:
            print(f"  port {i}: device {p['node']:08x} {p['vid']:04x}:{p['pid']:04x} on offer, "
                  f"IN {p['ep_in']:02x} OUT {p['ep_out']:02x}"
                  + (", an IN transfer still queued" if p["in_entry"] else "")
                  + (", headset" if p["flags"] & 1 else ""))


def handover_status():
    x = Xbdm()
    builds = xw_builds(x)
    if not builds:
        raise SystemExit("no xw build is loaded")
    for slot, name, base, size in builds:
        h = find_ho(x, base, size)
        if h:
            print_ho(name, h)
        else:
            print(f"{name}: no handover block (built before hot reload existed)")
    print(f"memory held by {len(builds)} build(s): {sum(b[3] for b in builds) // 1024} KB "
          f"(a released build stays loaded until the console reboots)")


def trace_status(args):
    """xw.py trace [NAME]"""
    need_dev("trace")
    x = Xbdm()
    want = args[0].lower().removesuffix(".xex") + ".xex" if args else None
    shown = 0
    for slot, name, base, size in xw_builds(x):
        if want and name != want:
            continue
        h = find_ho(x, base, size)
        if not h or not h.get("trace_ring"):
            print(f"{name}: no trace (built before WP3.1)")
            continue
        entries, n, _ = read_trace(x, h)
        print(f"{name}: {n} steps traced, last {trace_text(h['trace'])}; gate {h['cmd_gate']}"
              + (f", holding at {gate_name(h['gate_at'])}" if h["gate_at"] else "")
              + f"; in_double {h['in_double']}; title {h['title']:08x}")
        for l in trace_lines(entries)[0]:
            print("  " + l)
        shown += 1
    if not shown:
        raise SystemExit("no such xw build is loaded" if want else "no xw build is loaded")


def set_gate(args):
    """xw.py gate N [NAME]"""
    need_dev("gate")
    if not args or not args[0].isdigit():
        raise SystemExit("usage: xw.py gate N [NAME]   (99 = no gate holds)")
    n = int(args[0])
    want = args[1].lower().removesuffix(".xex") + ".xex" if len(args) > 1 else None
    x = Xbdm()
    for slot, name, base, size in xw_builds(x):
        if want and name != want:
            continue
        h = find_ho(x, base, size)
        if h and h.get("trace_ring"):
            print(f"{name}: gate {h['cmd_gate']} -> {n}"
                  + (f" (was holding at {gate_name(h['gate_at'])})" if h["gate_at"] else ""),
                  x.setu32(h["addr"] + HO_OFF["cmd_gate"], n))


def wait_ho(what, secs, fn):
    until = time.time() + secs
    while True:
        try:
            v = fn()
            if v:
                return v
        except OSError:
            pass
        if time.time() > until:
            return None
        time.sleep(0.2)


def build_blocks(x, name, base, size, magics):
    """{magic: address} of these status blocks in a loaded build: from out/<name>.exe if the
    console has the magic there, else found in the module's memory"""
    plausible = lambda a: (lambda w: bool(w) and 1 <= w[1] <= 100)(x.u32s(a, 2))
    found = {}
    exe = os.path.join(OUT, name[:-4] + ".exe")
    if os.path.exists(exe):
        for m in magics:
            try:
                a = pe_va(exe, struct.pack(">I", m))
            except SystemExit:
                continue
            if base <= a < base + size and x.u32s(a, 1) == (m,) and plausible(a):
                found[m] = a
    missing = {m: plausible for m in magics if m not in found}
    if missing:
        found.update(scan_blocks(x, base, size, missing))
    return found


def knob_specs():
    """(name, block magic, offset in the block, gate): the settings a reload carries over.
    Gate 8-11: the stages (held at 0 with --step until that gate); None: the user's settings
    (WP3.4), written before the take-over in every mode. The mic mute goes with the handover
    block instead (HO_PF_MUTED): it has no xbdm word."""
    import team
    import voice
    ga = lambda f: 4 * voice.GA_FIELDS.index(f)
    xd = lambda f: 4 * DIAG_FIELDS.index(f)
    xt = lambda f: 4 * team.HEAD.index(f)
    return [("voice stage", voice.MAGIC, 4 * voice.GLOBAL.index("stage"), 8),
            ("audio stage", MAGIC, OFF_AUD_STAGE, 9),
            ("game audio enable", voice.GA_MAGIC, ga("enable"), 10),
            ("HUD enable", XH36_MAGIC, 4 * 2, 11),  # XH_WATCH: enable is word 2
            ("balance", voice.GA_MAGIC, ga("balance"), None),
            ("master", voice.GA_MAGIC, ga("master"), None),
            ("sidetone", voice.GA_MAGIC, ga("sidetone"), None),
            ("mic gain", DIAG_MAGIC, xd("mic_gain"), None),
            ("noise gate", DIAG_MAGIC, xd("gate"), None),
            ("game buffer", DIAG_MAGIC, xd("buffer_ms"), None),
            ("team stage", team.XT_MAGIC, xt("stage"), None),
            ("team channel", team.XT_MAGIC, xt("channel"), None),
            ("team level", team.XT_MAGIC, xt("level"), None)]


def knob_text(v):
    return "-" if v is None else str(v - (1 << 32) if v & 0x80000000 else v)


def old_knobs(x, name, base, size):
    """{knob name: value} in the build being replaced (None: it has no such block)"""
    specs = knob_specs()
    blocks = build_blocks(x, name, base, size, {m for _, m, _, _ in specs})
    out = {}
    for k, m, off, _ in specs:
        w = blocks.get(m) and x.u32s(blocks[m] + off, 1)
        out[k] = w[0] if w else None
    print(f"{name}: " + ", ".join(f"{k} {knob_text(v)}" for k, v in out.items()))
    return out


def new_knobs(x, exe, carry, hold_all):
    """writes the old build's settings into the new build before it takes over (its main
    loop hasn't run yet), or 0 for each with --step (gates T8-T11 raise them). Returns
    [(name, address, value to end with, gate)]"""
    out = []
    for k, m, off, g in knob_specs():
        try:
            a = pe_va(exe, struct.pack(">I", m)) + off
        except SystemExit:
            continue
        w = x.u32s(a, 1)
        if not w:
            continue
        want = w[0] if carry.get(k) is None else carry[k]
        hold = hold_all and g is not None
        if hold or want != w[0]:
            x.setu32(a, 0 if hold else want)
        out.append((k, a, want, g))
    print("new build starts with: " + ", ".join(
        f"{k} {0 if hold_all and g is not None else knob_text(v)}" +
        (f" (then {v})" if hold_all and g is not None and v else "") for k, _, v, g in out))
    return out


def settle_title(name, ho, need_switch, stable=10.0, timeout=40.0):
    """--settle: the take-over waits until the title has been the same for `stable` s (the
    loader title launches Aurora about 1 s after it loaded the new build; boot waits 8 s for
    the same reason). The slot build publishes the title while it waits (g_ho.title). Stays
    under the 60 s the slot build waits for its take-over word."""
    t0 = time.time()
    last, since, switched = None, t0, False
    print(f"{name}: --settle: waiting until the title has been stable {stable:.0f} s", flush=True)
    while True:
        try:
            h = read_ho(Xbdm(), ho)
        except OSError:
            h = None
        now = time.time()
        t = h and h.get("title")
        if t:
            if t != last:
                if last is not None:
                    switched = True
                    print(f"  {now - t0:5.1f} s: title {last:08x} -> {t:08x}", flush=True)
                else:
                    print(f"  {now - t0:5.1f} s: title {t:08x}", flush=True)
                last, since = t, now
            elif now - since >= stable and (switched or not need_switch or now - t0 > 30):
                print(f"  {now - t0:5.1f} s: title {t:08x} stable {now - since:.1f} s: settled"
                      + ("" if switched or not need_switch else " (no switch seen)"), flush=True)
                return True
        if now - t0 > timeout:
            print(f"  {now - t0:5.1f} s: NOT settled (title {last or 0:08x}); taking over anyway",
                  flush=True)
            return False
        time.sleep(0.25)


class Stepper:
    """xw.py reload --step: the reload one gate at a time. At each gate it prints the trace and
    the log lines since the last one, says what the next step does, and waits for Enter (or
    `hold` seconds: --hold). The builds hold at a C gate until cmd_gate reaches it (300 s at
    most, then they go on by themselves); T8-T11 are the new build's own settings."""

    def __init__(self, hold):
        if hold is None and not sys.stdin.isatty():
            hold = 15.0
        self.hold = hold
        self.builds = {}  # name -> dict(ho, ring, log_since, trace_seen, tb)

    def follow(self, name, ho, base, size, history=False):
        """from now on show() prints this build's new trace entries and log lines (history:
        also what its trace ring and log already have)"""
        x = Xbdm()
        ring = scan_block(x, base, size, LOG_MAGIC)
        since = 0 if history or not ring else read_log(x, ring, 0)[0]
        h = read_ho(x, ho)
        self.builds[name] = dict(ho=ho, ring=ring, log_since=since,
                                 trace_seen=None if history or not h else h["trace_n"], tb=None)

    def show(self, name):
        b = self.builds.get(name)
        if not b:
            return
        try:
            x = Xbdm()
            h = read_ho(x, b["ho"])
            if h:
                entries, n, lost = read_trace(x, h, b["trace_seen"])
                if lost:
                    print(f"  [{name}] ({lost} trace entries overwritten)")
                lines, b["tb"] = trace_lines(entries, b["tb"])
                for l in lines:
                    print(f"  [{name}] {l}")
                b["trace_seen"] = n
            if b["ring"]:
                b["log_since"], text = read_log(x, b["ring"], b["log_since"])
                for l in text.splitlines():
                    print(f"  [{name}] {l}")
        except OSError as e:
            print(f"  [{name}] xbdm: {e}")
        sys.stdout.flush()

    def pause(self, label, what):
        print(f"--- {label} next: {what}", flush=True)
        try:
            if self.hold is None:
                input("    Enter runs it (Ctrl-C stops here) ")
            else:
                print(f"    holding {self.hold:g} s", flush=True)
                time.sleep(self.hold)
        except (KeyboardInterrupt, EOFError):
            print(f"\nstopped before {label}. The build holds there for up to 300 s, then goes "
                  f"on by itself; `xw.py gate 99` lets every held build go on now.")
            raise SystemExit(1)

    @staticmethod
    def raise_gate(ho, n):
        Xbdm().setu32(ho + HO_OFF["cmd_gate"], n)

    def wait_gate(self, name, ho, side, n, stop, secs=30):
        """True once the build holds at gate side n; else stop(h)'s reason, or None (timeout)"""
        want = ord(side) << 8 | n
        until = time.time() + secs
        while time.time() < until:
            try:
                h = read_ho(Xbdm(), ho)
            except OSError:
                h = None
            if h:
                if h["gate_at"] == want:
                    return True
                r = stop(h)
                if r:
                    return r
            time.sleep(0.1)
        return None

    def not_reached(self, name, label):
        raise SystemExit(f"{name} did not reach gate {label} within the time allowed. The trace "
                         f"above names the last step it reached (xw.py trace). It may be stuck "
                         f"in that step; `xw.py gate 99` lets a held build go on, or reboot")

    def release_gates(self, name, ho, refusals):
        def stop(h):
            if h["refusals"] != refusals:
                return "refused"
            return "released" if h["state"] == HO_RELEASED else None
        for n in range(1, 7):
            r = self.wait_gate(name, ho, "R", n, stop)
            self.show(name)
            if r is None:
                self.not_reached(name, f"R{n}")
            if r is not True:
                return  # the caller reports the refusal (or the release)
            self.pause(f"{name} R{n}", GATES_R[n])
            self.raise_gate(ho, n)

    def takeover_gates(self, name, ho, knobs):
        failed = lambda h: "failed" if h["take_state"] == HO_T_FAILED else None
        adopted = 0
        for n in range(1, 8):
            if n == 5:
                h = wait_ho("the adoption", 10, lambda: (lambda h: h if h and h["take_state"] in (
                    HO_T_DONE, HO_T_FAILED) else None)(read_ho(Xbdm(), ho)))
                adopted = h["adopted"] if h else 0
                if not adopted:
                    print(f"--- {name} T5-T7: no controller was adopted: nothing to hold")
                    break
            # T2 can take up to 30 s (XInput retries), T7 waits for the pad's identify answer
            r = self.wait_gate(name, ho, "T", n, failed, secs=45 if n in (2, 7) else 30)
            self.show(name)
            if r is None:
                self.not_reached(name, f"T{n}")
            if r is not True:
                return  # take-over failed: the caller reports it
            self.pause(f"{name} T{n}", GATES_T[n])
            self.raise_gate(ho, n)
        if adopted:
            # the pad's bind (PAD_BOUND in the trace), then no C gate holds any more
            until = time.time() + 10
            while time.time() < until:
                h = read_ho(Xbdm(), ho)
                if h and TRACE_NAMES.get(h["trace"] & 0xFFFF) == "PAD_BOUND":
                    break
                time.sleep(0.2)
        self.raise_gate(ho, 99)
        time.sleep(1)
        self.show(name)
        for k, addr, want, g in sorted((k for k in knobs if k[3] is not None),
                                       key=lambda k: k[3]):
            if not want:
                print(f"--- {name} T{g}: {k} stays 0 (it was 0 in the build replaced)")
                continue
            self.pause(f"{name} T{g}", GATES_T[g].format(v=want))
            print(f"    {k} = {want}", Xbdm().setu32(addr, want))
            if g == 9:
                self.hsrestart(addr)
            time.sleep(3)
            self.show(name)

    @staticmethod
    def hsrestart(stage_addr):
        """after the audio stage: the headset identifies itself again on the first connected
        port (at stage 0 it was refused)"""
        x = Xbdm()
        st = stage_addr - OFF_AUD_STAGE
        for i in range(4):
            w = x.u32s(st + 4 * HDR_WORDS + i * PAD_LEN, 1)
            if w and w[0]:
                print(f"    hsrestart port {i}:", x.setu32(st + OFF_AUD_RESET, 0x100 | (i + 1)))
                return
        print("    no controller connected: no hsrestart")


def reload(args):
    """Replace the running xw build with a new one: no reboot, no replug, no USB reset."""
    args, hold = list(args), None
    if "--hold" in args:
        i = args.index("--hold")
        hold = 15.0
        if i + 1 < len(args) and args[i + 1].replace(".", "", 1).isdigit():
            hold = float(args.pop(i + 1))
    bad = [a for a in args if a not in ("--in-place", "--reset", "--no-build", "--step", "--settle",
                                        "--hold", "--no-auth", "--release-only", "--loader",
                                        "--no-hooks-check")]
    if bad:
        raise SystemExit(f"xw.py reload: unknown option {bad[0]}")
    need_dev("reload")
    # in place by default (WP3.3: E1 froze around the loader title's exit; E1' passed):
    # the old build loads the new one from its own thread, no title switch. --loader: the
    # loader title, which then launches Aurora (the old way)
    loader_path = "--loader" in args
    if loader_path and "--in-place" in args:
        raise SystemExit("xw.py reload: --in-place or --loader, not both")
    in_place, reset, build = not loader_path, "--reset" in args, "--no-build" not in args
    settle, no_auth = "--settle" in args, "--no-auth" in args
    release_only = "--release-only" in args
    if release_only and "--in-place" in args:
        raise SystemExit("xw.py reload: --release-only loads nothing, so not with --in-place")
    if release_only:
        in_place = False  # the release only (cmd_release 1); the load comes later
    if reset and no_auth:
        raise SystemExit("xw.py reload: --reset or --no-auth, not both")
    step = Stepper(hold) if "--step" in args or "--hold" in args else None
    folder = console_dir()
    x = Xbdm()
    builds = xw_builds(x)
    if not builds:
        raise SystemExit("no xw build is loaded: nothing to reload (xw.py load, or reboot with "
                         "xw.xex as a boot plugin)")
    # the build to replace: the newest that isn't an idle leftover of a failed reload
    old = None
    for slot, name, base, size in reversed(builds):
        h = find_ho(x, base, size)
        if h is None:
            raise SystemExit(f"{name} has no hot-reload support (it was built before this "
                             f"feature): install a build from this branch as the boot plugin "
                             f"and reboot once; every update after that can be a reload")
        if h["version"] != HO_VERSION:
            raise SystemExit(f"{name} has handover version {h['version']}, this xw.py knows "
                             f"{HO_VERSION}: reboot to update")
        if h["state"] != HO_IDLE:
            old = (name, base, size, h)
            break
        print(f"{name}: idle (it took nothing over); looking at the build before it")
    if not old:
        raise SystemExit("every loaded xw build is idle: reboot")
    old_name, old_base, old_size, oh = old
    new_slot = builds[-1][0] + 1
    new = f"xw{new_slot}"
    print(f"replacing {old_name} ({HO_STATES.get(oh['state'], oh['state'])}) with {new}.xex")
    if oh["state"] not in (HO_RUNNING, HO_RELEASED):
        raise SystemExit(f"{old_name} is {HO_STATES.get(oh['state'], oh['state'])}: try again "
                         f"in a moment")
    if in_place and oh["state"] == HO_RELEASED:
        if "--in-place" in args:
            raise SystemExit(f"{old_name} has released already (its thread is gone), so it "
                             f"can't load the next build itself: run reload --loader")
        print(f"{old_name} has released already (its thread is gone): the loader title loads "
              f"the new build (as --loader)")
        in_place = False
    # cause 4 (docs/PLAN.md 3.1): a function another plugin patched over our jump would be
    # patched again by the new build: stop before anything is changed
    if "--no-hooks-check" not in args:
        bad_hooks = [r for r in hooks_check(Xbdm()) if r["kind"] == "foreign"]
        if bad_hooks:
            raise SystemExit("hooks: " + "; ".join(f"{r['name']}: {r['detail']}" for r in bad_hooks)
                             + "\nanother module patched a function xw.xex hooks: nothing was "
                             "changed (xw.py hooks; --no-hooks-check goes ahead anyway)")

    # 1. build the slot (nothing on the console has changed yet)
    if build:
        import subprocess
        env = dict(os.environ, XW_SLOT=str(new_slot))
        cmd, cwd = (["bash", "build.sh"], HERE) if os.environ.get("OXC") else (
            ["bash", os.path.join(HERE, "..", "toolchain", "build.sh")], HERE)
        print(f"building slot {new_slot} ...", flush=True)
        r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
        if r.returncode or name_file("xw_built_slot", "") != new:
            raise SystemExit("build failed:\n" + r.stdout[-3000:] + r.stderr[-3000:])
    exe, xex = os.path.join(OUT, new + ".exe"), os.path.join(OUT, new + ".xex")
    if not (os.path.exists(exe) and os.path.exists(xex)):
        raise SystemExit(f"out/{new}.xex isn't built (XW_SLOT={new_slot} bash toolchain/build.sh)")
    new_ho = pe_va(exe, struct.pack(">I", HO_MAGIC))

    # 2. the files (still nothing changed for the running build)
    hdd = folder.lower() == "hdd:" + B + "plugins"
    usb = folder.lower() == FOLDER.lower()
    loader = "xw_slot_loader_hdd.xex" if hdd else "xw_slot_loader.xex"
    if not in_place and not (hdd or usb):
        raise SystemExit(f"the loader title only knows {FOLDER} and Hdd:{B}Plugins: use "
                         f"--in-place for XW_DIR={folder}")
    dev = next((d for p, d in DEVICES.items() if folder.lower().startswith(p)), None)
    if in_place and not dev:
        raise SystemExit(f"--in-place: no device path known for {folder} (Usb0: or Hdd:)")
    files = [new + ".xex"] + ([] if in_place else [loader])
    for f in files:
        data = open(os.path.join(OUT, f), "rb").read()
        st = Xbdm().sendfile(folder + B + f, data)
        print(f"upload {folder}{B}{f}: {len(data)} bytes, {st}")
        if not st.startswith("2"):
            raise SystemExit("upload failed: nothing was changed on the console")

    # the old build's settings that the new one starts with (read before its release)
    carry = old_knobs(Xbdm(), old_name, old_base, old_size)

    # 3. release the running build
    if oh["state"] == HO_RELEASED:
        print(f"{old_name}: already released (an earlier reload got this far)")
    else:
        addr, before = oh["addr"], oh["refusals"]
        gated = step and oh.get("trace_ring")
        if step and not gated:
            print(f"{old_name} has no gates (built before WP3.1): it releases in one go")
        if gated:
            Xbdm().setu32(addr + HO_OFF["cmd_gate"], 0)
            step.follow(old_name, oh["addr"], old_base, old_size)
        if in_place:
            path = (dev + folder[folder.index(":") + 1:] + B + new + ".xex").encode() + b"\0"
            if len(path) > 128:
                raise SystemExit("module path too long")
            Xbdm().setbytes(addr + HO_OFF_PATH, path)
        print(f"{old_name}: release", Xbdm().setu32(addr + HO_OFF["cmd_release"], 2 if in_place else 1))
        if gated:
            step.release_gates(old_name, addr, before)

        def released():
            h = read_ho(Xbdm(), addr)
            return h if h and (h["state"] == HO_RELEASED or h["refusals"] != before) else None
        h = wait_ho("the release", 20, released)
        if not h or h["state"] != HO_RELEASED:
            print(log_tail(Xbdm(), old_base, old_size))
            if h:
                raise SystemExit(f"{old_name} REFUSED to release: "
                                 f"{HO_REFUSE.get(h['refuse'], h['refuse'])}.\n"
                                 f"Nothing was handed over; it keeps running as before.")
            raise SystemExit(f"{old_name} didn't answer the release within 20 s (xw.py handover)")
        print(f"{old_name}: released")
        if gated:
            step.show(old_name)
            Xbdm().setu32(addr + HO_OFF["cmd_gate"], 99)
        oh = h
    if release_only:
        print(f"--release-only: {old_name} has released; nothing is loaded. It holds the "
              f"controllers idle (no input reaches the console now). Go on with: xw.py reload "
              f"--no-build [--settle] [--step|--hold] (it picks up from here), or reboot")
        return

    # 4. load the new build
    if in_place:
        def loaded():
            h = read_ho(Xbdm(), oh["addr"])
            return h if h and h["load_status"] not in (0, 0xEEEEEEEE) or (
                new + ".xex") in Xbdm().modules() else None
        wait_ho("XexLoadImage", 20, loaded)
        h = read_ho(Xbdm(), oh["addr"])
        print(f"{old_name}: XexLoadImage status {h['load_status']:08x}")
    else:
        print("load", Xbdm().cmd(f'magicboot title="{folder}{B}{loader}" directory="{folder}"')[0])
    if not wait_ho(f"{new}.xex", 40, lambda: (new + ".xex") in Xbdm().modules()):
        raise SystemExit(f"{new}.xex did not load. {old_name} has released and holds the "
                         f"controllers idle: fix the problem and run xw.py reload again (it "
                         f"picks up from here), or reboot")
    if not wait_ho(f"{new}'s handover block", 15,
                   lambda: (Xbdm().u32s(new_ho, 1) or [0])[0] == HO_MAGIC):
        raise SystemExit(f"{new}.xex is loaded, but out/{new}.exe doesn't match it")

    # 5. the old build's settings into the new one (all held at 0 with --step: gates T8-T11)
    knobs = new_knobs(Xbdm(), exe, carry, hold_all=bool(step))
    if settle:
        settle_title(new, new_ho, need_switch=not in_place)

    # 6. take over: the mode first, then the word the new build is waiting for
    Xbdm().setu32(new_ho + HO_OFF["cmd_mode"], 1 if reset else 2 if no_auth else 0)
    if step:
        Xbdm().setu32(new_ho + HO_OFF["cmd_gate"], 0)
        step.follow(new, new_ho, *Xbdm().module_map()[new + ".xex"], history=True)
    print(f"{new}: take over from {oh['addr']:08x}" + (" (USB reset)" if reset else " (no second handshake)" if no_auth else ""),
          Xbdm().setu32(new_ho + HO_OFF["cmd_takeover"], oh["addr"]))
    if step:
        step.takeover_gates(new, new_ho, knobs)
    h = wait_ho("the take-over", 30, lambda: (lambda h: h if h and h["take_state"] in (
        HO_T_DONE, HO_T_FAILED) else None)(read_ho(Xbdm(), new_ho)))
    x = Xbdm()
    mods = x.module_map()
    if step:
        step.show(new)
    elif new + ".xex" in mods:
        print(log_tail(x, *mods[new + ".xex"], lines=40))
    if not h:
        raise SystemExit(f"{new} didn't finish the take-over within 30 s (xw.py handover)")
    print_ho(new, h)
    if h["take_state"] == HO_T_FAILED:
        raise SystemExit(f"{new} did NOT take over: {HO_TAKE_ERR.get(h['take_error'], h['take_error'])}")
    # only now do status / watch / voice.py follow the new build
    open(os.path.join(OUT, "xw_loaded_name"), "w").write(new + "\n")
    if h["skipped"] or h["take_error"]:
        print(f"*** WARNING: {h['skipped']} controller(s) were NOT taken over"
              + (f" ({HO_TAKE_ERR.get(h['take_error'], h['take_error'])})" if h["take_error"] else "")
              + f". They stay with {old_name}, which no longer serves them, and a later reload "
              f"won't pick them up: replug them (or reboot). ***")
    print(f"done: {new}.xex is the running build. xw.py status / watch follow it from now on.\n"
          f"{len(mods and [m for m in mods if xw_slot(m) is not None])} xw builds are loaded; "
          f"the old ones stay in memory until the console reboots.")


# ---- xw.py hooks: who owns the start of each function xw.xex hooks ---------------------------
# (name, 17559 address, its first four instructions). The XInput and XVoiced ones were read
# from xw.xex's detour registry on the console (Nov 22 2026); VdSwap from hud_360.c.
HOOK_TARGETS = [
    ("XInputdReadState", 0x800F6C20, (0x546b0006, 0x5463073e, 0x2b060000, 0x419a000c)),
    ("XamInputGetCapabilitiesEx", 0x816F12B8, (0x7d8802a6, 0x4803c101, 0x9421ff60, 0x546b043e)),
    ("XamInputSetState", 0x816F1740, (0x7d8802a6, 0x4803bc81, 0x9421ff70, 0x7cbe2b78)),
    ("XInputdGetDeviceStats", 0x800F6D50, (0x546b0006, 0x5463073e, 0x2b060000, 0x419a000c)),
    ("XVoicedHeadsetPresent", 0x80101C40, (0x7d8802a6, 0x9181fff8, 0x9421ffa0, 0x80630000)),
    ("XVoicedSubmitPacket", 0x80102048, (0x7d8802a6, 0x4800ab71, 0x9421ff70, 0x7c7c1b78)),
    ("XVoicedClose", 0x80102230, (0x7d8802a6, 0x4800a995, 0x9421ff80, 0x7c7d1b78)),
    ("VdSwap", 0x800C8B90, (0x7d8802a6, 0x48044021, 0x9421ff40, 0x3d608017)),
]
REG_ENTRY_WORDS, REG_MAX = 7, 16  # struct detour_entry, DETOUR_REG_MAX (detour.h)


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode_jump(w):
    """the first four instructions -> ("slot", slot address) for the slot jump detour.c
    writes (lis rN; lwz rN, lo(rN); mtctr rN; bctr), ("far", target) for lis/ori/mtctr/bctr
    (older xw/xa builds, iMoD1998-style detours), ("branch", target) for a plain b, or None"""
    op = lambda v: v >> 26
    rd = lambda v: v >> 21 & 31
    ra = lambda v: v >> 16 & 31
    if op(w[0]) == 15 and ra(w[0]) == 0 and w[3] == 0x4e800420 and \
            w[2] == 0x7c0903a6 | rd(w[0]) << 21:
        r = rd(w[0])
        if op(w[1]) == 32 and rd(w[1]) == r and ra(w[1]) == r:  # lwz r, lo(r)
            return "slot", ((w[0] & 0xffff) << 16) + s16(w[1] & 0xffff) & 0xffffffff
        if op(w[1]) == 24 and ra(w[1]) == r and rd(w[1]) == r:  # ori r, r, lo (rS = rA = r)
            return "far", (w[0] & 0xffff) << 16 | (w[1] & 0xffff)
    if op(w[0]) == 18:  # b / ba
        li = w[0] & 0x03fffffc
        li = li - 0x04000000 if li & 0x02000000 else li
        return "branch", li & 0xffffffff  # relative: the caller adds the address unless AA
    return None


def module_of(mods, addr):
    for name, (base, size) in mods.items():
        if base <= addr < base + size:
            return name
    return f"no module ({addr:08x})"


def read_registries(x, mods):
    """{target: entry} from every loaded xw build's detour registry (newest build last, so
    its entries win), entry = dict(target, slot, trampoline, original, build)"""
    out = {}
    for slot, name, base, size in xw_builds(x):
        h = find_ho(x, base, size)
        if not h or not h.get("detours"):
            continue
        w = x.u32s(h["detours"], 1 + REG_ENTRY_WORDS * REG_MAX)
        if not w:
            continue
        for i in range(min(w[0], REG_MAX)):
            e = w[1 + REG_ENTRY_WORDS * i:1 + REG_ENTRY_WORDS * (i + 1)]
            if e[1]:
                out[e[0]] = dict(target=e[0], slot=e[1], trampoline=e[2], original=tuple(e[3:7]),
                                 build=name)
    return out


def hooks_check(x):
    """[dict(name, addr, words, kind, detail)], kind = "ours" (our slot jump, the slot in an xw
    build), "original" (the function's own code), "foreign" (anything else), "unreadable"."""
    mods = x.module_map()
    xw_mods = {n for n in mods if xw_slot(n) is not None}
    reg = read_registries(x, mods)
    known = {a for _, a, _ in HOOK_TARGETS}
    targets = list(HOOK_TARGETS) + [(f"(registry entry of {e['build']})", a, e["original"])
                                    for a, e in reg.items() if a not in known]
    out = []
    for name, addr, original in targets:
        w = x.u32s(addr, 4)
        r = dict(name=name, addr=addr, words=w, kind="foreign", detail="")
        if not w:
            r.update(kind="unreadable", detail="getmem failed")
        elif tuple(w) == original:
            r.update(kind="original", detail="the function's own code (not hooked)")
        else:
            j = decode_jump(w)
            if j and j[0] == "slot":
                slot = j[1]
                v = x.u32s(slot, 1)
                hook = v[0] if v else None
                owner, howner = module_of(mods, slot), hook is not None and module_of(mods, hook)
                r["detail"] = (f"jumps through slot {slot:08x} in {owner} to " +
                               (f"{hook:08x} in {howner}" if hook is not None else "(unreadable)"))
                if owner in xw_mods and howner in xw_mods:
                    r["kind"] = "ours"
                elif owner in xw_mods:
                    r["detail"] += " (our slot, but the hook isn't in an xw build)"
            elif j:
                to = j[1] if j[0] == "far" or w[0] & 2 else (addr + j[1]) & 0xffffffff
                r["detail"] = (f"{'far jump' if j[0] == 'far' else 'branch'} to {to:08x} in "
                               f"{module_of(mods, to)}")
            else:
                r["detail"] = "code that is neither the original nor a jump we know"
            if r["kind"] == "foreign" and addr in reg and reg[addr]["original"] != original:
                r["detail"] += f" ({reg[addr]['build']}'s registry has other original words)"
        out.append(r)
    return out


def hooks():
    """xw.py hooks; exit status 1 if any function is patched by someone else"""
    res = hooks_check(Xbdm())
    for r in res:
        words = " ".join(f"{v:08x}" for v in r["words"]) if r["words"] else "-" * 35
        print(f"{r['name']:<26} {r['addr']:08x}  {words}  {r['kind'].upper():<10} {r['detail']}")
    n = {k: sum(r["kind"] == k for r in res) for k in ("ours", "original", "foreign", "unreadable")}
    print(f"{n['ours']} ours, {n['original']} original, {n['foreign']} foreign"
          + (f", {n['unreadable']} unreadable" if n["unreadable"] else ""))
    if n["foreign"] or n["unreadable"]:
        sys.exit(1)


# ---- xw.py tee: a log that survives a freeze -----------------------------------------------------
TEE_POLL = 0.05
TEE_SILENT = 1.0      # seconds without an answer before the console counts as silent
XW36_WATCH = {2: "state", 3: "last_error", 6: "adds", 7: "removes", 8: "declined", 12: "stage"}
AU_WATCH = ("stage active_stage port flags enables refusals disables opens closes watchdog "
            "in_cancelled out_refused").split()
HO_WATCH = ("state refuse refusals releases take_state take_error adopted resets skipped "
            "inherited load_status cmd_gate title in_double").split()
XH36_MAGIC, XV36_MAGIC = 0x58483336, 0x58563336
# struct xh_status (hud_360.c, 4266a69: append-only): enable 2, force 3, hooked 14, error 15,
# heartbeat 17, swaps 20, draws 23
XH_WATCH = {2: "enable", 3: "force", 14: "hooked", 15: "error"}
XH_HB, XH_SWAPS, XH_DRAWS, XH_WORDS = 17, 20, 23, 24
XV_HB = 21  # struct xv_status heartbeat (voice.py GLOBAL)


class TeeBuild:
    """one loaded xw build: where its blocks are and what was last seen in them"""

    def __init__(self, name, base, size):
        self.name, self.base, self.size = name, base, size
        self.st = self.ring = self.au = self.ho = self.xh = self.xv = None
        self.since, self.partial = 0, ""
        self.last = {}
        self.hb = {}      # heartbeat name -> (value, time it last changed, reported stopped)
        self.trace_seen, self.trace_tb = None, None  # the WP3.1 trace ring: read up to here


def tee_mem(x, addr, n):
    """getmem that tells a dead connection (OSError) from an unmapped address (None)"""
    st, lines = x.cmd(f"getmem addr=0x{addr:08x} length=0x{n:x}")
    if not st:
        raise OSError("xbdm closed the connection")
    h = "".join(lines)
    return None if not h or "?" in h else bytes.fromhex(h)


def tee_u32s(x, addr, n):
    b = tee_mem(x, addr, 4 * n)
    return b and struct.unpack(f">{n}I", b)


def tee_find(x, b):
    """the build's blocks: XW36 from out/<name>.exe if it matches, else by a scan of the
    module; the log ring and XWAU through XW36; XWHO, XH36, XV36 by the same scan"""
    plausible = lambda a: (lambda w: bool(w) and 1 <= w[1] <= 100)(x.u32s(a, 2))
    want = {MAGIC: plausible, HO_MAGIC: lambda a: (lambda w: bool(w) and w[1] in range(1, 100)
                                                    and 0x100 <= w[2] <= 0x2000)(x.u32s(a, 3)),
            XH36_MAGIC: plausible, XV36_MAGIC: plausible}
    found = {}
    exe = os.path.join(OUT, b.name[:-4] + ".exe")
    if os.path.exists(exe):
        for m in list(want):
            try:
                a = pe_va(exe, struct.pack(">I", m))
            except SystemExit:
                continue
            w = x.u32s(a, 1)
            if b.base <= a < b.base + b.size and w and w[0] == m and want[m](a):
                found[m] = a
    missing = {m: c for m, c in want.items() if m not in found}
    if missing:  # out/ is another build, or a block this build doesn't have (one pass)
        found.update(scan_blocks(x, b.base, b.size, missing))
    b.st, b.ho, b.xh, b.xv = (found.get(m) for m in (MAGIC, HO_MAGIC, XH36_MAGIC, XV36_MAGIC))
    if b.st:
        w = x.u32s(b.st, 5)
        if w and x.u32s(w[4], 1) == (0x584C4F47,):
            b.ring = w[4]
        if w and w[1] >= 2 and x.u32s(b.st + OFF_AUDIO, 1) == (AUDIO_MAGIC,):
            b.au = b.st + OFF_AUDIO


class Tee:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "a", buffering=1)
        self.x = None
        self.builds = {}
        self.mods = None
        self.next_mods = 0
        self.next_hb = 0
        self.last_ok = self.since_ok = time.time()
        self.silent = self.refind = False

    @staticmethod
    def stamp(t=None):
        t = time.time() if t is None else t
        return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"

    def out(self, text, echo=True):
        """an event: the log file and the screen"""
        for line in text.splitlines():
            self.f.write(f"{self.stamp()} {line}\n")
            if echo:
                print(f"{self.stamp()} {line}", flush=True)

    def run(self):
        self.out(f"tee: {os.environ.get('XBOX')} -> {self.path}")
        try:
            while True:
                t0 = time.time()
                try:
                    if self.x is None:
                        self.x = Xbdm(timeout=TEE_SILENT)
                    self.poll()
                    self.last_ok = time.time()
                    if self.silent:
                        self.silent = False
                        self.out(f"CONSOLE BACK (silent since {self.stamp(self.since_ok)}, "
                                 f"{time.time() - self.since_ok:.1f} s)")
                except (OSError, ValueError) as e:
                    if self.x:
                        self.x.close()
                    self.x = None
                    if not self.silent:
                        self.silent, self.since_ok = True, self.last_ok
                        for b in self.builds.values():
                            self.flush_partial(b)
                        self.out(f"CONSOLE SILENT since {self.stamp(self.last_ok)} ({e})")
                    self.refind = True  # the blocks again once it answers (a reboot?)
                    time.sleep(0.25)
                time.sleep(max(0.0, TEE_POLL - (time.time() - t0)))
        except KeyboardInterrupt:
            pass
        finally:
            import signal
            signal.signal(signal.SIGINT, signal.SIG_IGN)  # a second Ctrl-C: still close the log
            self.f.write(f"{self.stamp()} tee: stopped\n")
            self.f.close()
            print("tee: stopped; log in " + self.path)

    def poll(self):
        x, now = self.x, time.time()
        if self.mods is None or now >= self.next_mods:
            self.next_mods = now + 1.0
            st, lines = x.cmd("modules")
            if not st:
                raise OSError("xbdm closed the connection")
            mods = {}
            for l in lines:
                f = dict(kv.split("=", 1) for kv in l.split() if "=" in kv)
                try:
                    mods[f["name"].strip('"').lower()] = (int(f["base"], 0), int(f["size"], 0))
                except (KeyError, ValueError):
                    pass
            if self.mods is None:
                self.out("modules: " + " ".join(sorted(mods)))
            else:
                gone = sorted(set(self.mods) - set(mods))
                new = sorted(set(mods) - set(self.mods))
                if gone or new:
                    self.out("modules:" + "".join(f" +{n}" for n in new) +
                             "".join(f" -{n}" for n in gone))
            self.mods = mods
            for n in list(self.builds):
                if n not in mods or mods[n] != (self.builds[n].base, self.builds[n].size):
                    self.flush_partial(self.builds.pop(n))
                elif self.refind and tee_u32s(x, self.builds[n].st or 0x80000000, 1) != (MAGIC,):
                    tee_find(x, self.builds[n])  # same place after a silence, another build
            self.refind = False
            for n, (base, size) in sorted(mods.items(), key=lambda m: xw_slot(m[0]) or 0):
                if xw_slot(n) is not None and n not in self.builds:
                    b = TeeBuild(n, base, size)
                    tee_find(x, b)
                    self.builds[n] = b
                    self.out(f"{n}: base {base:08x}, blocks " + ", ".join(
                        f"{k} {v:08x}" if v else f"{k} -" for k, v in (
                            ("XW36", b.st), ("XLOG", b.ring), ("XWAU", b.au), ("XWHO", b.ho),
                            ("XH36", b.xh), ("XV36", b.xv))))
        for b in self.builds.values():
            self.poll_build(x, b, time.time())  # not `now`: finding blocks takes seconds
        if now >= self.next_hb:
            self.next_hb = time.time() + 1.0
            self.out(" | ".join(self.hb_line(b) for b in self.builds.values()) or "no xw build",
                     echo=False)

    def changed(self, b, block, names, values):
        diff = []
        for n, v in zip(names, values):
            key = (block, n)
            if key in b.last and b.last[key] != v:
                diff.append(f"{n} {b.last[key]:#x} -> {v:#x}" if v > 9 or b.last[key] > 9
                            else f"{n} {b.last[key]} -> {v}")
            b.last[key] = v
        if diff:
            self.out(f"{b.name} {block}: " + ", ".join(diff))

    def beat(self, b, which, value, now, quiet=False):
        old = b.hb.get(which)
        if old is None or old[0] != value:
            if old and old[2]:
                self.out(f"{b.name}: {which} moving again ({value})")
            b.hb[which] = (value, now, False)
        elif not old[2] and now - old[1] > 2.0 and not quiet:
            self.out(f"{b.name}: {which} stopped at {value} {now - old[1]:.1f} s ago")
            b.hb[which] = (old[0], old[1], True)

    def poll_build(self, x, b, now):
        released = False
        if b.st:
            w = tee_u32s(x, b.st, 13)
            if w and w[0] == MAGIC:
                hb = b.hb.get("XW36 heartbeat")
                if hb and w[9] < hb[0]:
                    self.flush_partial(b)
                    self.out(f"{b.name}: started again (heartbeat {hb[0]} -> {w[9]}): reboot?")
                    b.since, b.hb = 0, {}
                self.changed(b, "XW36", XW36_WATCH.values(), [w[i] for i in XW36_WATCH])
                released = w[2] in (6, 7)
                self.beat(b, "XW36 heartbeat", w[9], now, quiet=released)
        if b.ring:
            self.read_ring(x, b)
        if b.ho:
            w = tee_u32s(x, b.ho, len(HO_FIELDS))
            if w and w[0] == HO_MAGIC:
                h = dict(zip(HO_FIELDS, w))
                self.changed(b, "XWHO", HO_WATCH, [h[n] for n in HO_WATCH])
                if b.last.get("gate_at", 0) != h["gate_at"]:
                    self.out(f"{b.name} XWHO: " + (f"holding at gate {gate_name(h['gate_at'])}"
                                                   if h["gate_at"] else "gate passed"))
                b.last["gate_at"] = h["gate_at"]
                if h["trace_ring"] and h["trace_n"] != b.trace_seen:
                    self.read_trace(x, b, h)
                elif not h["trace_ring"]:  # a build before WP3.1: the word only (0)
                    self.changed(b, "XWHO", ["trace"], [h["trace"]])
                released = released or w[3] in (HO_RELEASED, HO_IDLE)
        if released:
            return  # a released build: its log ring and handover block only
        if b.au:
            raw = tee_mem(x, b.au, 4 * len(AUDIO_FIELDS))
            if raw:
                a = dict(zip(AUDIO_FIELDS, struct.unpack(f">{len(AUDIO_FIELDS)}I", raw)))
                if a["magic"] == AUDIO_MAGIC:
                    self.changed(b, "XWAU", AU_WATCH, [a[n] for n in AU_WATCH])
                    b.last["au"] = a
        if b.xh:
            w = tee_u32s(x, b.xh, XH_WORDS)
            if w and w[0] == XH36_MAGIC:
                self.changed(b, "XH36", XH_WATCH.values(), [w[i] for i in XH_WATCH])
                self.beat(b, "XH36 heartbeat", w[XH_HB], now)
                b.last["xh"] = w
        if b.xv:
            w = tee_u32s(x, b.xv, XV_HB + 1)
            if w and w[0] == XV36_MAGIC:
                self.beat(b, "XV36 heartbeat", w[XV_HB], now)

    def read_trace(self, x, b, h):
        """the WP3.1 trace ring's new entries (all it has the first time)"""
        entries, n, lost = read_trace(x, h, b.trace_seen)
        if lost:
            self.out(f"{b.name}: [trace ring overran: {lost} entries lost]")
        lines, b.trace_tb = trace_lines(entries, b.trace_tb)
        for l in lines:
            self.out(f"{b.name} {l}")
        b.trace_seen = n

    def read_ring(self, x, b):
        hdr = tee_u32s(x, b.ring, 3)
        if not hdr or hdr[0] != 0x584C4F47:
            return
        size, written = hdr[1], hdr[2]
        if written < b.since:  # the ring started again (a reboot that kept the address?)
            b.since = 0
        if written - b.since > size:
            if b.since:
                self.out(f"{b.name}: [log ring overran: {written - b.since - size} bytes lost]")
            b.since = written - size
            b.partial = "[...]"  # the oldest text in the ring starts mid-line
        text = b""
        while b.since < written:
            start = b.since % size
            n = min(written - b.since, size - start, 0x1000)
            chunk = tee_mem(x, b.ring + 12 + start, n)
            if chunk is None:
                break
            text += chunk
            b.since += n
        if text:
            lines = (b.partial + text.decode(errors="replace")).split("\n")
            b.partial = lines.pop()
            for l in lines:
                self.out(f"[{b.name[:-4]}] {l.rstrip()}")

    def flush_partial(self, b):
        if b.partial:
            self.out(f"[{b.name[:-4]}] {b.partial} [no newline yet]")
            b.partial = ""

    def hb_line(self, b):
        parts = [b.name]
        for k, (v, _, stopped) in sorted(b.hb.items()):
            parts.append(f"{k.split()[0]} hb {v}" + (" STOPPED" if stopped else ""))
        a = b.last.get("au")
        if a:
            parts.append(f"mic {a['mic_packets']} ({a['mic_pps']}/s) ear {a['ear_blocks']} "
                         f"out_done {a['out_done']} in_done {a['in_done']}")
        w = b.last.get("xh")
        if w:
            parts.append(f"swaps {w[XH_SWAPS]} draws {w[XH_DRAWS]}")
        return " ".join(parts)


def install(args):
    """xw.py install [--no-reboot]: out/xw.xex, and out/xw.presets and out/xw.demo (the
    surround demo clip, tools/demo/make_demo.py) if they are there, into Hdd:\\Plugins (where
    the boot loads the plugin from), each checked by reading it back"""
    if [a for a in args if a != "--no-reboot"]:
        raise SystemExit("usage: xw.py install [--no-reboot]")
    folder = "Hdd:" + B + "Plugins" + B
    files = [("xw.xex", os.path.join(OUT, "xw.xex"))]
    for extra in ("xw.presets", "xw.demo"):
        if os.path.exists(os.path.join(OUT, extra)):
            files.append((extra, os.path.join(OUT, extra)))
    if not os.path.exists(files[0][1]):
        raise SystemExit(f"{files[0][1]} not found: bash toolchain/build.sh first")
    for name, path in files:
        new = open(path, "rb").read()
        try:
            old = Xbdm().getfile(folder + name)
        except SystemExit:
            old = None  # not installed yet
        # no .prev for the demo clip: 7.9 MB of disk for a file make_demo.py remakes exactly
        if old is not None and old != new and name != "xw.demo":
            # the build there now, as .prev (a reinstall of the same file keeps the older .prev)
            st = Xbdm().sendfile(folder + name + ".prev", old)
            if not st.startswith("200") or Xbdm().getfile(folder + name + ".prev") != old:
                raise SystemExit(f"{folder}{name}.prev: not written ({st}); nothing changed")
        st = Xbdm().sendfile(folder + name, new)
        if Xbdm().getfile(folder + name) != new:
            raise SystemExit(f"{folder}{name}: the copy read back differs ({st})"
                             + (f"; the previous file is {name}.prev"
                                 if old is not None and name != "xw.demo" else ""))
        print(f"{folder}{name}: {len(new)} bytes, read back ok"
              + (", previous kept as .prev" if old is not None and old != new and name != "xw.demo"
                 else ", unchanged" if old == new else ""))
    if "--no-reboot" in args:
        print("not rebooted (--no-reboot): the new build loads at the next boot")
        return
    print("magicboot cold:", Xbdm().cmd("magicboot cold")[0])
    print("rebooting. If the console is not back (ping, xbdm) in about 2 minutes it may have "
          "switched off instead: switch it on. The controller stays dark until its Xbox button "
          "is pressed.")


def tee(args):
    d = args[0] if args else os.path.join(OUT, "logs")
    os.makedirs(d, exist_ok=True)
    Tee(os.path.join(d, time.strftime("%Y%m%d-%H%M%S") + ".log")).run()


def main():
    step = sys.argv[1] if len(sys.argv) > 1 else "status"
    if step in ("tone", "usbreset", "mecreset", "hsrestart") or (step == "audio" and
                                                                  sys.argv[2:3] == ["5"]):
        need_dev(" ".join(sys.argv[1:3]) if step == "audio" else step)
    if step == "install":
        install(sys.argv[2:])
    elif step == "upload":
        # XW_DIR=Hdd:\Plugins: the plugin on the hard disk, with the loader built for that path
        folder = console_dir()
        hdd = folder.lower() == "hdd:" + B + "plugins"
        for name in ("xw.xex", "xw_loader_hdd.xex" if hdd else "xw_loader.xex"):
            data = open(os.path.join(OUT, name), "rb").read()
            print(folder + B + name, len(data), Xbdm().sendfile(folder + B + name, data))
    elif step == "load":
        if "xw.xex" in Xbdm().modules():
            raise SystemExit("xw.xex is already loaded (reboot to load a new build)")
        print(Xbdm().cmd(f'magicboot title="{FOLDER}{B}xw_loader.xex" directory="{FOLDER}"')[0])
    elif step == "boot":
        if sys.argv[2:3] not in (["on"], ["off"]):
            raise SystemExit("usage: xw.py boot on|off")
        boot(sys.argv[2] == "on", BOOT_PATH)
    elif step == "aurora":
        # Aurora where the loader titles expect it: Hdd:\Aurora with the plugin on the hard disk
        a = os.environ.get("XW_AURORA") or ("Hdd:" + B + "Aurora" if console_dir().lower().startswith(
            "hdd:") else "Usb0:" + B + "Apps" + B + "Aurora")
        print(Xbdm().cmd(f'magicboot title="{a}{B}Aurora.xex" directory="{a}"')[0])
    elif step == "reload":
        reload(sys.argv[2:])
    elif step == "handover":
        handover_status()
    elif step == "tee":
        tee(sys.argv[2:])
    elif step == "hooks":
        hooks()
    elif step == "trace":
        trace_status(sys.argv[2:])
    elif step == "gate":
        set_gate(sys.argv[2:])
    elif step == "raw":
        raw_test(float(sys.argv[2]) if len(sys.argv) > 2 else 60)
    elif step == "test":
        button_test()
    elif step == "go":
        print(Xbdm().setu32(status_addr() + OFF_GO, int(sys.argv[2])))
    elif step == "audio":
        if len(sys.argv) < 3 or not sys.argv[2].isdigit() or int(sys.argv[2]) > 5:
            raise SystemExit("usage: xw.py audio 0..5")
        print(Xbdm().setu32(status_addr() + OFF_AUD_STAGE, int(sys.argv[2])),
              "(replug the headset for a higher stage to take effect)")
    elif step == "tone":
        print(Xbdm().setu32(status_addr() + OFF_AUD_TONE, int(sys.argv[2])))
    elif step == "usbreset":
        port = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        print(Xbdm().setu32(status_addr() + OFF_AUD_RESET, port + 1))
    elif step == "mecreset":
        print(Xbdm().setu32(status_addr() + OFF_AUD_RESET, 0x200),
              "(game audio's MEC client: unregistered, then registered again)")
    elif step == "hsrestart":
        # PORT [IN OUT]: restart the headset with that format (hex, e.g. 9 10 = 24 kHz mono
        # mic / 48 kHz stereo earpiece); without: the format it last accepted, if known
        port = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        fmt = 0
        if len(sys.argv) > 4:
            fi, fo = int(sys.argv[3], 16), int(sys.argv[4], 16)
            if not (1 <= fi <= 16 and 1 <= fo <= 16):
                raise SystemExit("usage: xw.py hsrestart [PORT [IN OUT]]   (formats 1..10 hex)")
            fmt = fi << 16 | fo << 24
        print(Xbdm().setu32(status_addr() + OFF_AUD_RESET, 0x100 | (port + 1) | fmt))
    elif step == "diag":
        if len(sys.argv) > 2:
            set_diag(sys.argv[2:])
        else:
            print_diag(read_diag(Xbdm()))
    elif step == "dump":
        dump(sys.argv[2] if len(sys.argv) > 2 else "ear.wav")
    elif step == "micdump":
        micdump(sys.argv[2] if len(sys.argv) > 2 else "mic.wav")
    elif step == "window":
        x, addr = Xbdm(), status_addr()
        if read_loop(x, addr) is None:
            raise SystemExit("this build has no window counters (XW36 before version 3)")
        arg = sys.argv[2] if len(sys.argv) > 2 else ""
        if arg in ("on", "off"):
            x.setu32(addr + OFF_LOOP + 4 * LOOP_FIELDS.index("win_boost"), int(arg == "on"))
        elif arg == "reset":
            x.setu32(addr + OFF_LOOP + 4 * LOOP_FIELDS.index("loop_max_us"), 0)
            x.setu32(addr + OFF_LOOP + 4 * LOOP_FIELDS.index("run_max_us"), 0)
        elif arg:
            raise SystemExit("usage: xw.py window [on|off|reset]")
        print_loop(read_loop(x, addr))
    elif step == "debug":
        if sys.argv[2:3] not in (["0"], ["1"], ["2"], ["3"]):
            raise SystemExit("usage: xw.py debug 0|1|2|3")
        print(Xbdm().setu32(status_addr() + OFF_DEBUG, int(sys.argv[2])))
    elif step in ("watch", "status"):
        addr = status_addr()
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else (0 if step == "status" else 120)
        since, last_pads, until = 0, None, time.time() + secs
        while True:
            try:
                x = Xbdm()
                s = read_status(x, addr)
                if s is None:
                    print(f"xw status block not found at {addr:08x} (plugin loaded?)")
                else:
                    since, text = read_log(x, s["log"], since)
                    if text:
                        print(text, end="", flush=True)
                    pads = [pad_line(i, p) for i, p in enumerate(s["pads"]) if p["conn"]]
                    if step == "status":
                        print_status(s)
                        print_loop(read_loop(x, addr))
                        print_diag(read_diag(x))
                    elif pads != last_pads:
                        print("\n".join(pads) or "(no controllers)", flush=True)
                        last_pads = pads
            except OSError as e:
                print(f"xbdm: {e}", flush=True)
            if time.time() >= until:
                break
            time.sleep(0.3)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
