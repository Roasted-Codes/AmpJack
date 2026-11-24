// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Headset settings and audio diagnostics (voice_360.c): the mic level and noise gate (rows of
 * the on-screen panel, docs/PLAN.md 1.0), the test tone, game audio's buffer, the earpiece
 * stream's counters and the driver loop's timing. One block, "XWDG", all u32, in this order:
 * xw.py's DIAG_FIELDS mirrors it (version 3; xw.py refuses any other). Nothing is saved: the
 * compiled defaults below need no setup after a reboot.
 *
 * The mic, in the order it is processed (every mic packet, muted or not):
 *   1. DC removed (mic_dc, default on): the headset's mic carries a constant offset of about
 *      +60 (read on the console, Nov 23 2026: idle mic = 60 +/- 3, i.e. -55 dBFS of DC on
 *      top of -80 dBFS of hiss). Without this the meter showed that offset as a level and
 *      any gain above 100 % would push it into the teammates' audio. A one-pole DC tracker,
 *      ~4 Hz: nothing audible is touched.
 *   2. The 20 ms RMS: the gate's decision (before the volume, so MIC VOLUME never changes
 *      when the gate opens) and the meter (after the volume).
 *   3. MIC VOLUME (mic_gain) and the gate, ramped: the 360's mic, the mic test's recording.
 *      Team voice (team_360.c) applies the same volume, capped at 100 %.
 *
 *   tone         a test tone in the earpiece in place of game audio (chat and sidetone stay):
 *                0 off, 1 440 Hz, 2 1 kHz. A pure integer sine, continuous in phase, 10 ms
 *                fades. tone_level 0 = very quiet (peak 519, -36 dBFS), 1 = quiet (peak 1036,
 *                -30 dBFS), times the master volume: never louder. Default off.
 *   buffer_ms    game audio's ring target (game_360.c): 30, 45 (default), 60 or 90 ms. A
 *                change ramps (0.75 ms a second: the ratio trim follows it), never jumps.
 *   mic_gain     MIC VOLUME, how loud teammates hear you, in per cent: one of the 10 values
 *                25 35 50 70 100 140 200 280 400 560 (the panel's 1..10: 3 dB a step, from
 *                -12 to +15 dB; default 100 = level 5 = the mic as the headset sends it).
 *                Ramped over 10 ms; above 100 % a soft limiter keeps peaks under full scale.
 *                Any other value is rounded to the nearest and written back. (Version 2 had
 *                10..100 in steps of 10: the top half of that range was under 3 dB, and
 *                there was no way up for a quiet mic.)
 *   mic_dc       1 (default): remove the mic's DC offset first (see above); 0: as version 2.
 *   gate         noise gate on the mic as the 360 gets it: 0 off, the steps 1 (opens at RMS
 *                33, -60 dBFS), 2 (58, -55), 4 = step 3 (104, -50), 5 = step 4 (184, -45),
 *                measured before the gain over 20 ms
 *                windows; open from the 1 ms chunk in which the window's sum reaches the
 *                threshold, held open 400 ms after the last such window; opens over 2 ms,
 *                closes over 10 ms; silence when closed. No delay is added to the mic.
 *                3 auto (XD_GATE_AUTO, the default): the threshold follows the room, 9 dB
 *                over its noise floor (-60 .. -35 dBFS), while NOISE GATE AUTO is in effect
 *                (the mic-auto block's gate_auto, on by default, and its memory there,
 *                micdsp_360.c); otherwise exactly step 2. gate_threshold then shows AUTO's
 *                threshold of the moment, as RMS. Out of range: step 4. (Version 4 and
 *                before: 1 low -45, 2 high -35, 200 ms, 10 ms to open, default low; they cut
 *                a quiet talker's words.)
 *   probe        xbdm: time one driver-thread call per loop pass, 16 passes (see below).
 *
 * The earpiece's OUT blocks in flight are fixed (xw_plugin.c AUD_OUT_BLOCKS = 5: no gaps on
 * the console); the clock-fix experiment, the DPC refill and the game tap / resampler dumps
 * are gone (PLAN.md WP1.4).
 *
 * The controls are written by xbdm (xw.py diag) and by the panel; xd_poll (driver thread)
 * clamps them, writes a bad value back corrected, applies and logs each change. The sidetone
 * is not touched by the mic gain or the gate (it is the user's own monitor).
 *
 * Loop timing, measured from audio_poll (xw_plugin.c) and xv_poll, both once per driver loop
 * pass, with the timebase (mftb, 50 MHz):
 *   pass_us      audio_poll's start to its next start (= the loop's period, sleep included);
 *                pass_max_us the longest (xbdm may write 0 to measure from now);
 *                pass_hist[6] passes under 4, 8, 12, 17, 25 ms and the rest.
 *   work_us      the time spent inside audio_poll + xv_poll in the last pass (max: work_max_us).
 *   gap_us       the end of xv_poll to the next audio_poll start: the rest of the pass (the
 *                main loop's xa_sleep_ms(2), panel_poll, hud_poll, xt_poll, service_port);
 *                gap_max_us the longest. pass = work + gap.
 *   tb_per_ms    timebase ticks per millisecond of system time (KeQuerySystemTime), over the
 *                last >= 1 s: 50000 if the timebase is the 50 MHz the _us fields assume.
 *   passes_per_s loop passes in that second, by the system clock (a cross-check of pass_us).
 *   probe        written by xbdm: 0x100 + ms = one xa_sleep_ms(ms) (0..20) per pass;
 *                0x200 = the system time's step (spin from one KeQuerySystemTime change to the
 *                next, at most 40 ms). Each of the next 16 passes runs it once and times it;
 *                then the thread writes probe 0 and bumps probe_runs. Results: probe_kind
 *                (what ran), probe_n, probe_min_us / probe_avg_us / probe_max_us, and for
 *                0x200 the step in 100 ns units (probe_step_100ns, the last one seen).
 *                A sleep that lasts ~16 ms each time while work_us is small is the sleep's
 *                granularity, not the loop's work; a short sleep with a long gap is other
 *                work in the gap, or the thread not being run.
 * (Read offline in the 17559 dump: the clock interrupt at 0x800771F8 adds 10000 x 100 ns to
 * the interrupt and system time of KeTimeStampBundle, 0x801B0EE0, and 1 to its tick count;
 * timers are due against that interrupt time. Measured on the console, Nov 23 2026: the
 * system time steps every ~5.4 ms, xa_sleep_ms(1) takes 14.9 ms and (2) 15.9 ms: the probe's
 * verdict was SLEEP GRANULARITY. 17559 exports no timer-resolution call (xboxkrnl.def has
 * KeSetTimer/Ex, NtSetTimerEx, KeQueryInterruptTime, nothing like NtSetTimerResolution).)
 */
#ifndef DIAG_360_H
#define DIAG_360_H

#include "../core/compat.h"

/* 3: mic_gain's 10 values, mic_dc (was reserved[0]). 4: gate 3 is NOISE GATE AUTO (when
 * switched on). 5: the gate's four steps (1, 2, 4, 5: -60 -55 -50 -45 dBFS), 400 ms hold,
 * opening early, default AUTO. Same size and layout since 3. */
#define XD_VERSION 5

struct xd_status {
	u32 magic, version;
	/* controls */
	volatile u32 tone, tone_level, buffer_ms, mic_gain, gate;
	/* in effect */
	u32 applied_tone, applied_tone_level, applied_buffer_ms, applied_mic_gain, applied_gate;
	/* the tone: its frequency and its peak now (sample units, after master and fade) */
	u32 tone_hz, tone_peak;
	/* game audio's ring: the target in effect (ramping to buffer_ms), the rebuffer limit
	 * and the distance for the full trim, in 48 kHz frames */
	u32 target_frames, high_frames, drift_full_frames;
	/* the mic: the gain in effect (Q15: 32768 = 100 %, more above), the gate (1 = open),
	 * the last 20 ms window's RMS (after the DC removal, before the gain), the threshold,
	 * times it opened, and the meter (0..10: that RMS after the gain, xd_mic_vu) */
	u32 mic_gain_q15, gate_open, gate_rms, gate_threshold, gate_opens, mic_vu;
	/* the earpiece's OUT stream (Usbd's DPC counts): frames that completed "not accessed"
	 * (0xC005100E: the frame had passed), the last non-zero frame status, completions that
	 * left no other OUT block queued (a gap: the next block starts late) */
	u32 out_frames_missed, out_last_status, out_starved;
	/* the flow field (le16) of the headset's audio packets this session (core/gip.c):
	 * packets with it below / equal to / above the earpiece's bytes per packet
	 * (flow_fragment), the smallest, the largest and the last value */
	u32 flow_below, flow_equal, flow_above, flow_min, flow_max, flow_last, flow_fragment;
	/* the driver loop's timing (see above) */
	u32 pass_us;
	volatile u32 pass_max_us;
	u32 work_us, work_max_us;
	u32 pass_hist[6];
	u32 tb_per_ms, passes_per_s;
	volatile u32 probe;
	u32 probe_kind, probe_n, probe_min_us, probe_avg_us, probe_max_us, probe_step_100ns;
	u32 probe_runs;
	u32 gap_us;
	volatile u32 gap_max_us;
	/* version 3, in what was reserved[0] (loop_wake, removed Nov 23 2026) */
	volatile u32 mic_dc;   /* 1 (default): the mic's DC offset is removed first */
	u32 mic_dc_now;        /* the offset being removed now (signed, sample units) */
};
#define XD_WORDS 57
_Static_assert(sizeof(struct xd_status) == XD_WORDS * 4, "xw.py DIAG_FIELDS / DIAG_WORDS");

extern struct xd_status g_xd; /* voice_360.c */

#define XD_TONES 3       /* off, 440 Hz, 1 kHz */
#define XD_TONE_LEVELS 2 /* very quiet, quiet */
/* g_xd.gate: OFF, AUTO and the four manual steps 1..4 (-60 -55 -50 -45 dBFS: higher cuts
 * more). The numbers are what xbdm tools, scripts and saved settings hold; 0 = OFF and 3 =
 * AUTO never moved, the steps take 1, 2, 4, 5 (the panel shows them in its own order: AUTO,
 * OFF, 1, 2, 3, 4). A value stored before the steps existed reads as: 1 (LOW) -> step 1,
 * 2 (HIGH) -> step 2. While NOISE GATE AUTO is not in effect (xd_gate_auto_offered: switched
 * off, or no memory for it), AUTO runs exactly step XD_GATE_FALLBACK's threshold. */
enum { XD_GATE_OFF, XD_GATE_1, XD_GATE_2, XD_GATE_AUTO, XD_GATE_3, XD_GATE_4 };
#define XD_GATES 6
#define XD_GATE_STEPS 4
#define XD_GATE_FALLBACK XD_GATE_2 /* -55 dBFS */
#define XD_MIC_LEVELS 10       /* MIC VOLUME 1..10 */
#define XD_MIC_GAIN_MIN 25     /* level 1, -12 dB */
#define XD_MIC_GAIN_MAX 560    /* level 10, +15 dB */
#define XD_MIC_GAIN_DEFAULT 140 /* level 6 (level 5, 100, is as the headset sends it) */
/* Step 3 (-50 dBFS), the owner's choice after trying AUTO and the steps in Halo 3 chat (Nov 23
 * 2026). A profile with a saved gate keeps it (settings_360.c restores the stored number). */
#define XD_GATE_DEFAULT XD_GATE_3

/* driver thread, every pass (from xv_poll): validates the controls, applies, logs; runs
 * the mic test (xd_mictest_*) */
void xd_poll(void);

/* The mic volumes the audio code accepts (MIC VOLUME 1..10, per cent: see mic_gain above).
 * xd_mic_gain_step: the accepted value `steps` steps away from `pct` (rounded to the nearest
 * accepted value first), clamped at both ends; the panel writes it to g_xd.mic_gain, xd_poll
 * applies it. xd_mic_level: the panel's 1..10 for `pct` (rounded the same way). */
u32 xd_mic_gain_step(u32 pct, int steps);
u32 xd_mic_level(u32 pct);

/* MIC VOLUME AUTO (driver thread; micdsp_360.c). While it is on, the gain follows the
 * speech level (one gain per player), and g_xd.mic_gain keeps the player's manual value, so
 * turning AUTO off returns to his last step. xd_set_mic_auto stores the choice and applies it
 * at once; xd_mic_auto is whether it is in effect: never while MIC VOLUME AUTO is not
 * switched on (xd_mic_auto_offered), whatever was chosen. xd_mic_auto_level: the 1..10 step
 * in effect now for the first headset (AUTO's gain as the nearest step, or the manual one). */
bool xd_mic_auto(void);
void xd_set_mic_auto(bool on);
u32 xd_mic_auto_level(void);

/* Whether the panel may offer AUTO (driver thread): each is its own switch in the mic-auto
 * status block (micauto.py): NOISE GATE AUTO on at boot, MIC VOLUME AUTO off until the owner
 * has heard it. Off (or, for the gate, its 1120 bytes not allocated), a stored gate 3 runs
 * LOW and MIC VOLUME AUTO is never in effect; the panel then hides the AUTO values. */
bool xd_gate_auto_offered(void);
bool xd_mic_auto_offered(void);

/* the mic meter of pad slot `pad` (driver thread): 0..10, the last 20 ms window's RMS after
 * the mic volume and before the gate, 4 dB a segment where speech is, 6 near full scale:
 * segment 1 lights at -48 dBFS, then -44 -40 -36 -32 -28 -24 -20, -14 and -8. Normal
 * speech, as teammates should get it (about -30 dBFS, the level VoIP aims for), lights 5;
 * 9 and 10 mean "too loud: it will distort". 0 with no headset, or when no mic audio
 * arrived for 100 ms. Muted or not: it shows what the mic hears. */
u32 xd_mic_vu(int pad);

/* ---- the mic test (the panel's MIC TEST row; mictest_360.c, xbdm block "XWMT") ---------------
 * Records the mic as teammates would hear it (after the DC removal, MIC VOLUME, the noise gate
 * and the limiter; muted = no recording) for as long as the player likes, up to the auto-stop
 * (XWMT rec_ms, default XD_MT_MAX_MS), then plays it once into that headset's earpiece at the
 * earpiece's own level (x master volume, never above 1, clamped), with game audio ducked to
 * 25 % while it plays (not while paused or held). The clip can be played again, paused and
 * resumed as often as wanted. One test at a time. Driver thread only.
 *
 * The contract (the panel drives it; the owner's model): Right = start recording, stop and
 * play, pause, resume, play again; Left = discard. The clip never outlives the panel: when
 * the panel closes with nothing recording or playing, a held clip is deleted and its memory
 * freed (xd_mictest_panel_closed). A clip that finishes while the panel is hidden is kept
 * until the panel has next been opened and closed. Every return to IDLE frees the buffer. */
#define XD_MT_MS 10000      /* the fallback: the longest clip when 1.44 MB cannot be had */
#define XD_MT_MAX_MS 30000  /* the auto-stop: the longest clip (XD_MT_MS if the big
			     * allocation fails; xd_mictest_max_secs says which) */
#define XD_MT_MIN_MS 500    /* shorter recordings are discarded, not played */
/* the state; = HUD_MT_* (panel_360.c asserts it). IDLE/RECORDING/PLAYING keep their numbers:
 * XWMT `state` and accept/mictest.py read them. */
enum { XD_MT_IDLE, XD_MT_RECORDING, XD_MT_PLAYING, XD_MT_PAUSED, XD_MT_READY };
/* why xd_mictest_start / xd_mictest_play refused (XWMT last_refusal: numbers never move) */
enum { XD_MT_OK, XD_MT_OFF, XD_MT_BUSY, XD_MT_NO_HEADSET, XD_MT_MUTED, XD_MT_NO_MEMORY,
       XD_MT_NO_CLIP };
/* the MIC TEST row is shown (XWMT `enable`) */
bool xd_mictest_enabled(void);
/* IDLE or READY -> RECORDING on pad slot `pad` (a held clip is dropped): XD_MT_OK, or why
 * not. RECORDING ends by itself at the auto-stop and then plays, as xd_mictest_stop. */
u32 xd_mictest_start(int pad);
/* RECORDING -> PLAYING (at least XD_MT_MIN_MS recorded; else discarded -> IDLE). A playback
 * that reaches the end -> READY (clip held). Anything else: nothing. */
void xd_mictest_stop(void);
/* READY -> PLAYING from the start: XD_MT_OK, or XD_MT_NO_CLIP / XD_MT_BUSY / ... */
u32 xd_mictest_play(void);
/* PLAYING -> PAUSED (faded out, position kept); PAUSED -> PLAYING (faded in). Anything else:
 * nothing. */
void xd_mictest_pause(void);
void xd_mictest_resume(void);
/* Left on the row: RECORDING -> discarded; PLAYING -> faded out, then discarded; PAUSED,
 * READY -> discarded. Always ends in IDLE with the clip's memory freed; the game comes
 * back. */
void xd_mictest_cancel(const char *why);
/* panel_360.c, each time the panel closes: PAUSED or READY -> discarded (IDLE). RECORDING
 * and PLAYING carry on (the toast shows them; a call then does nothing); a clip they leave
 * is kept until the panel has been opened and closed again, with no state in the panel: the
 * next call can only come from that next close. */
void xd_mictest_panel_closed(void);
/* XD_MT_* now. *secs and *pct: as hud_state.mictest_secs / mictest_pct (hud_360.h):
 * RECORDING seconds recorded so far (counting up) and % of the auto-stop; PLAYING / PAUSED
 * the clip's length and % played; READY the clip's length. */
u32 xd_mictest_state(u32 *secs, u32 *pct);
/* the auto-stop in effect, seconds (the next recording's while idle: 10 after a fallback) */
u32 xd_mictest_max_secs(void);
/* the noise gate is passing pad slot `pad`'s mic to the 360 now (always, with the gate off);
 * false when muted or with no headset */
bool xd_gate_open(int pad);

/* the loop timing (xw_plugin.c): xd_time_pass at audio_poll's start, once per driver loop
 * pass; xd_time_work(tb) at the end of audio_poll and of xv_poll with the timebase their
 * work started at (xd_timebase) */
u32 xd_timebase(void);
void xd_time_pass(void);
void xd_time_work(u32 tb_start);

#endif
