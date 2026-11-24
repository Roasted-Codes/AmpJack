// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The mic test (the panel's MIC TEST row; diag_360.h xd_mictest_*) and its xbdm block "XWMT"
 * (voice.py mictest, accept/mictest.py). Record / play / pause, and the clip never outlives
 * the panel (docs/ROADMAP.md Part 3 item 2).
 *
 * Threads and locking: everything here runs on the driver thread (the panel's panel_poll,
 * xd_poll -> mt_poll, the mic chain -> mt_record, the earpiece mix -> mt_mix), so nothing is
 * locked. xbdm writes only the volatile controls; mt_poll takes them.
 * tests/test_mictest.c compiles this file on the Mac against fakes of the rest.
 *
 * States (XD_MT_*):
 *   IDLE        nothing held; no buffer (it is freed on every way back here).
 *   RECORDING   every mic chunk of the test's pad as it leaves mic_clean (DC removed; before
 *               MIC VOLUME and the noise gate), resampled to MT_RATE into the buffer. Ends at the auto-stop
 *               (want), on xd_mictest_stop or on a mute: then it plays once if at least
 *               MT_MIN_MS was recorded, else it is discarded. No mic audio for MT_STALL_MS
 *               ends the test.
 *   PLAYING     the clip through the noise gate, MIC VOLUME and the limiter in effect NOW, 1 ms
 *               at a time, with the live path's own steps (voice_priv.h gate_*, shape(); the
 *               threshold is mic_gate_threshold's: NOISE GATE AUTO's of the moment, not
 *               re-learnt from the clip): what teammates would hear with today's settings.
 *               A NOISE GATE or MIC VOLUME change while it plays is heard at once, so a
 *               player records once and tries every step on the same sentence. Then
 *               mixed into that pad's earpiece blocks after everything else, at the master
 *               volume (Q8 <= 256: never above 1), every sample clamped, faded in over
 *               MT_FADE_MS; game audio ducked to duck_pct % (xg_duck, ramped in mt_poll). The
 *               end of the clip -> READY. No earpiece block for MT_STALL_MS ends the test.
 *   PAUSED      faded out over MT_FADE_MS (the samples under the fade are the next ones of the
 *               clip, so a pause skips and repeats nothing), then nothing is taken from the
 *               clip; resume fades in from where it stopped. The game is not ducked.
 *   READY       the clip is held; play starts it again from the beginning.
 * Left (xd_mictest_cancel) discards whatever there is; a playback (or a pause still fading)
 * fades out first (`stopping`), then the clip goes. The panel closing with nothing recording
 * or playing (xd_mictest_panel_closed) discards a paused or held clip.
 *
 * For tools over xbdm (voice.py mictest record / save), XWMT version 3: a recording started
 * with MT_CMD_START_HOLD, or ended with MT_CMD_STOP_HOLD, ends READY without being played
 * (nothing reaches the player's ear); and `keep` N protects the clip held from the panel
 * closing for N seconds (at most MT_KEEP_MAX_S), so a player using the panel while a tool
 * reads the clip does not free the buffer under it (that is how `voice.py mictest save` lost
 * a 30 s clip: getmem read freed pages). Left, a new recording, the headset going and a hot
 * reload still discard it.
 *
 * The buffer: XD_MT_MAX_MS of MT_RATE s16 (1.44 MB) is allocated when a recording starts;
 * if that fails, XD_MT_MS (480 KB), and the auto-stop shrinks to fit (xd_mictest_max_secs,
 * XWMT max_ms / fallback); if that fails too the start is refused (XD_MT_NO_MEMORY).
 * malloc is ExAllocatePool (platform_360.c). Read in the 17559 dump: ExAllocatePool (ord 9,
 * 0x80064A68) is ExAllocatePoolTypeWithTag(size, 'None', 0); from a system thread such as
 * the driver thread that is the system pool, and anything over 0xFD8 bytes goes to the page
 * allocator (0x80081250 -> 0x80080D28 on the system region 0x80170880): whole 4 KB pages, no
 * contiguous physical memory needed. Whether 1.44 MB of system pages is free while a game
 * runs is UNVERIFIED (XWMT fallback / alloc_fails / alloc_us on the console tell); the
 * 480 KB size is the one the mic test has always allocated. Nothing else in the plugin
 * depends on the allocation succeeding.
 */
#include "game_360.h"
#include "voice_360.h"
#include "voice_priv.h"

#define MT_RATE 24000
#define MT_PER_MS (MT_RATE / 1000)
#define MT_BIG (MT_PER_MS * XD_MT_MAX_MS) /* 720000 samples: 1.44 MB */
#define MT_SMALL (MT_PER_MS * XD_MT_MS)   /* 240000: 480 KB, the fallback */
#define MT_MIN_MS XD_MT_MIN_MS
#define MT_STALL_MS 2000
#define MT_FADE_MS 5
#define MT_DUCK_STEP 32  /* Q8 per driver loop pass: ~130 ms from 100 % to 25 % */
#define MT_KEEP_MAX_S 300
#define MT_GCHUNK MT_PER_MS /* the clip is gated 1 ms at a time, as the live mic arrives */

/* XWMT cmd: what xbdm asks for (the panel's actions, so a script needs no pad). The numbers
 * never move: voice.py and accept/mictest.py write them. */
enum { MT_CMD_NONE, MT_CMD_START, MT_CMD_CANCEL, MT_CMD_STOP, MT_CMD_PLAY, MT_CMD_PAUSE,
       MT_CMD_RESUME, MT_CMD_CLOSED,
       MT_CMD_START_SMALL, /* XW_DEV: start as if the 1.44 MB allocation had failed */
       MT_CMD_START_NOMEM, /* XW_DEV: start as if both allocations had failed */
       MT_CMD_START_HOLD,  /* start; the recording ends READY, never played */
       MT_CMD_STOP_HOLD    /* end the recording now, READY, never played */ };

/* xbdm: "XWMT", all u32, append only; voice.py's MT_FIELDS and accept/mictest.py mirror it */
struct xd_mt_status {
	u32 magic, version;
	/* controls */
	volatile u32 enable;   /* 1: the MIC TEST row is shown and works */
	volatile u32 cmd;      /* xbdm: MT_CMD_* (on cmd_pad for a start); written back 0 when taken */
	volatile u32 cmd_pad;
	volatile u32 rec_ms;   /* the auto-stop: 1000..30000 ms (default 30000; shorter for tests) */
	volatile u32 duck_pct; /* game audio during the playback: 0..100 % (default 25) */
	/* state */
	u32 state;             /* XD_MT_* */
	u32 pad;               /* the test's pad slot, 0xFFFFFFFF = none yet */
	u32 secs, pct;         /* what the panel shows (xd_mictest_state) */
	u32 want, recorded, played; /* MT_RATE samples: the auto-stop, the clip (so far), played */
	u32 starts, done, cancels, refused, last_refusal, mute_stops, stalls;
	/* the last recording: its peak and RMS (sample units, MT_RATE; version 3: before MIC
	 * VOLUME and the gate); version 3: rec_shut / rec_gain / rec_gate belong to the playback
	 * (the clip's samples the gate shut in the last one, the volume in per cent and the gate
	 * it is played with; at a start, the ones in effect). Version 2: at the recording. */
	u32 rec_peak, rec_rms, rec_shut, rec_gain, rec_gate;
	u32 play_peak;         /* the largest playback sample mixed in (after the master) */
	u32 duck_now;          /* game audio's share now, Q8 */
	u32 buf_addr;          /* the buffer now, 0 = none */
	u32 buf_bytes;         /* the last allocation's size (kept after it is freed) */
	u32 allocs, alloc_fails;
	/* version 2 */
	u32 clip_ms;           /* the clip held (recorded so far while recording), 0 = none */
	u32 max_ms;            /* the auto-stop in effect (the next recording's while idle) */
	u32 plays, pauses;     /* playbacks started (the one after a recording included); pauses */
	u32 discards;          /* returns to IDLE: every one frees the buffer */
	u32 panel_closes;      /* clips discarded because the panel closed */
	u32 fallback;          /* recordings that got the 10 s buffer */
	u32 heap_bytes;        /* held now */
	u32 alloc_us;          /* the last allocation (both tries) took this long */
	/* version 3 */
	volatile u32 keep;     /* xbdm: protect the clip from the panel closing for N s (<= 300);
				* written back 0 when taken */
	u32 kept;              /* panel closes that left a clip because of keep */
};
#define MT_WORDS 43
_Static_assert(sizeof(struct xd_mt_status) == MT_WORDS * 4, "voice.py MT_FIELDS");

__attribute__((used)) struct xd_mt_status g_xd_mt = {
	.magic = 0x58574D54, /* "XWMT" */
	.version = 3,        /* 2: record / play / pause, 30 s, cmd 3..9, clip_ms .. alloc_us;
			      * 3: cmd 10 / 11 (held recordings), keep, kept */
	.enable = 1,         /* the MIC TEST row is on (accept/mictest.py passed) */
	.rec_ms = XD_MT_MAX_MS,
	.duck_pct = 25,
	.pad = 0xFFFFFFFF,
	.duck_now = 256,
	.max_ms = XD_MT_MAX_MS,
};

static struct {
	s16 *buf;              /* cap samples, from the start of a recording until IDLE */
	u32 cap;               /* MT_BIG or MT_SMALL; 0 = no buffer */
	u32 next_ms;           /* what the next recording can hold, as far as is known */
	u32 state;             /* XD_MT_* */
	int pad;
	u32 want, n, pos;      /* MT_RATE samples: the auto-stop, recorded, taken for playback */
	u32 stopping;          /* fading out, then discarded (Left / panel closed mid-fade) */
	struct xd_mic gm;      /* playback: the gate's state (closed, hold, window) */
	struct xd_shape sh;    /* playback: MIC VOLUME, the gate's ramp, the limiter */
	s16 g[MT_GCHUNK];      /* playback: the clip's next 1 ms through them */
	u32 g_n, g_pos;
	struct rs rs_rec, rs_play;
	s16 f[16];             /* playback: resampled, not yet mixed */
	int f_n, f_pos;
	u32 env, fade;         /* playback gain env / fade, earpiece-rate samples */
	u64 sum, last_ms;
	u32 hold_end;          /* the recording ends READY, not played (MT_CMD_*_HOLD) */
	u64 keep_until;        /* the panel closing leaves the clip until then (xa_now_ms) */
} g_mt = { .pad = -1, .next_ms = XD_MT_MAX_MS };

static void mt_free(void)
{
	free(g_mt.buf);
	g_mt.buf = NULL;
	g_mt.cap = 0;
	g_xd_mt.buf_addr = 0;
	g_xd_mt.heap_bytes = 0;
}

/* back to IDLE from any other state: the clip and its buffer go */
static void mt_discard(u32 *counter, const char *why)
{
	if (g_mt.state == XD_MT_IDLE)
		return;
	xa_log("voice: pad %d: mic test %s (recorded %u ms, played %u ms)\n", g_mt.pad, why,
	       g_mt.n / MT_PER_MS, g_mt.pos / MT_PER_MS);
	if (counter)
		(*counter)++;
	g_xd_mt.discards++;
	g_mt.state = XD_MT_IDLE;
	g_mt.stopping = 0;
	g_mt.keep_until = 0; /* it protected this clip, not the next */
	g_xd_mt.state = XD_MT_IDLE;
	mt_free();
}

/* PLAYING from the clip's start, fading in */
static void mt_start_play(const char *why)
{
	u32 rate = g_pad[g_mt.pad].rate_out;

	rs_init(&g_mt.rs_play, MT_RATE, rate);
	g_mt.pos = 0;
	g_mt.f_n = g_mt.f_pos = 0;
	/* the gate starts shut, as after a pause, and MIC VOLUME at its value (the fade-in
	 * below already keeps the start from clicking; a 10 ms volume ramp from 0 would not be
	 * what teammates hear mid-conversation) */
	memset(&g_mt.gm, 0, sizeof(g_mt.gm));
	memset(&g_mt.sh, 0, sizeof(g_mt.sh));
	g_mt.gm.closed = 1;
	g_mt.sh.shut = mic_gate_threshold(g_mt.pad) ? 32768 : 0;
	g_mt.sh.gain = mic_gain_target(g_mt.pad);
	g_mt.g_n = g_mt.g_pos = 0;
	g_xd_mt.rec_shut = 0;
	g_mt.fade = rate / 1000 * MT_FADE_MS; /* rate_ok: >= 20 */
	g_mt.env = 0;
	g_mt.stopping = 0;
	g_mt.state = XD_MT_PLAYING;
	g_mt.last_ms = xa_now_ms();
	g_xd_mt.state = XD_MT_PLAYING;
	g_xd_mt.played = 0;
	g_xd_mt.play_peak = 0;
	g_xd_mt.plays++;
	xa_log("voice: pad %d: mic test: %s; playing %u ms\n", g_mt.pad, why, g_mt.n / MT_PER_MS);
}

/* the recording ends: what there is is played once (or, under MT_MIN_MS, discarded); a
 * held one (hold_end) is kept READY instead, unplayed */
static void mt_end_recording(const char *why)
{
	u32 fade = MT_PER_MS * MT_FADE_MS;
	u64 rms = g_mt.n ? g_mt.sum / g_mt.n : 0;

	g_xd_mt.rec_rms = isqrt((u32)(rms > 0xFFFFFFFFu ? 0xFFFFFFFFu : rms));
	if (g_mt.n < MT_PER_MS * MT_MIN_MS) {
		mt_discard(NULL, "ended: too short to play back");
		return;
	}
	/* the clip's last MT_FADE_MS faded out in place: no click at its end */
	for (u32 i = 0; i < fade && i < g_mt.n; i++)
		g_mt.buf[g_mt.n - 1 - i] = (s16)(g_mt.buf[g_mt.n - 1 - i] * (s32)i / (s32)fade);
	xa_log("voice: pad %d: mic test: recording %s (rms %u, peak %u)%s\n", g_mt.pad, why,
	       g_xd_mt.rec_rms, g_xd_mt.rec_peak, g_mt.hold_end ? "; held, not played" : "");
	if (g_mt.hold_end) {
		g_mt.state = XD_MT_READY;
		g_xd_mt.state = XD_MT_READY;
		return;
	}
	mt_start_play("recorded");
}

/* the clip goes, after a fade-out if it is being heard (PLAYING, or a pause still fading) */
static void mt_drop(u32 *counter, const char *why)
{
	if (g_mt.state == XD_MT_PLAYING || (g_mt.state == XD_MT_PAUSED && g_mt.env)) {
		if (!g_mt.stopping) {
			if (counter)
				(*counter)++;
			g_mt.stopping = 1; /* mt_mix fades it out, then discards it */
			xa_log("voice: pad %d: mic test: playback stopping (%s)\n", g_mt.pad, why);
		}
		return;
	}
	mt_discard(counter, why);
}

/* a mic chunk of pad `pad` (mono s16le, after mic_clean: before MIC VOLUME and the gate,
 * which the playback applies) into the recording */
void mt_record(int pad, const u8 *d, int frames)
{
	if (g_mt.state != XD_MT_RECORDING || pad != g_mt.pad)
		return;
	if (g_mic_mute[pad]) {
		g_xd_mt.mute_stops++;
		mt_end_recording("ended by the mute");
		return;
	}
	for (int i = 0; i < frames && g_mt.n < g_mt.want; i++) {
		s16 out[16];
		int k = rs_push(&g_mt.rs_rec, (s16)get_le16(d + 2 * i), out);

		for (int j = 0; j < k && g_mt.n < g_mt.want; j++) {
			s32 v = out[j];
			u32 a = (u32)(v < 0 ? -v : v);

			g_mt.buf[g_mt.n++] = out[j];
			g_mt.sum += a * a;
			if (a > g_xd_mt.rec_peak)
				g_xd_mt.rec_peak = a;
		}
	}
	g_mt.last_ms = xa_now_ms();
	g_xd_mt.recorded = g_mt.n;
	if (g_mt.n >= g_mt.want)
		mt_end_recording("reached the auto-stop");
}

/* the clip's next MT_GCHUNK samples through the gate, MIC VOLUME and the limiter in effect
 * now, in mic_analyse's then mic_shaped's order (voice_priv.h); 0 at the clip's end.
 * rec_shut counts the samples the gate shut in this playback; rec_gain / rec_gate are the
 * volume and gate it is played with. */
static u32 mt_gate_chunk(void)
{
	struct xd_mic *m = &g_mt.gm;
	const s16 *x = g_mt.buf + g_mt.pos;
	u32 n = g_mt.n - g_mt.pos, thr = mic_gate_threshold(g_mt.pad), win = gate_win(MT_RATE);
	u32 gain_to = mic_gain_target(g_mt.pad), shut_to;

	if (n > MT_GCHUNK)
		n = MT_GCHUNK;
	if (!n)
		return 0;
	gate_chunk_start(m, thr);
	for (u32 i = 0; i < n; i++)
		if (gate_push(m, x[i], win))
			gate_window_end(m, isqrt((u32)(m->sum / m->n)), thr, MT_RATE);
	gate_chunk_end(m, thr, win);
	shut_to = gate_shut(m) ? 32768 : 0;
	for (u32 i = 0; i < n; i++)
		g_mt.g[i] = shape(&g_mt.sh, x[i], gain_to, shut_to, MT_RATE);
	if (shut_to)
		g_xd_mt.rec_shut += n;
	g_xd_mt.rec_gain = gain_to * 100 / XD_Q12;
	g_xd_mt.rec_gate = g_xd.applied_gate;
	g_mt.pos += n;
	g_mt.g_n = n;
	g_mt.g_pos = 0;
	return n;
}

/* the playback mixed into an earpiece block of pad `pad` (s16le, rate_out x ch_out);
 * master: Q8. The gain of each sample is env / fade: fading in it rises 1, 2 .. fade, fading
 * out it falls fade .. 1, so every sample taken from the clip is heard (none at gain 0) and a
 * pause continues exactly where it stopped. */
void mt_mix(int pad, u8 *out, int len, u32 master)
{
	u32 ch = g_pad[pad].ch_out, frame = 2 * ch;
	int frames = len / (int)frame;
	bool end = false, down;

	if (pad != g_mt.pad ||
	    (g_mt.state != XD_MT_PLAYING && !(g_mt.state == XD_MT_PAUSED && g_mt.env)))
		return;
	if (master > 256)
		master = 256;
	down = g_mt.stopping || g_mt.state == XD_MT_PAUSED;
	for (int f = 0; f < frames; f++) {
		s32 y;
		u32 a, gain;

		if (down && !g_mt.env)
			break; /* faded out: paused, or about to be discarded */
		while (g_mt.f_pos >= g_mt.f_n) {
			if (g_mt.g_pos >= g_mt.g_n && !mt_gate_chunk())
				break;
			g_mt.f_n = rs_push(&g_mt.rs_play, g_mt.g[g_mt.g_pos++], g_mt.f);
			g_mt.f_pos = 0;
		}
		if (g_mt.f_pos >= g_mt.f_n) {
			end = true; /* all of it played */
			break;
		}
		if (down) {
			gain = g_mt.env--;
		} else {
			if (g_mt.env < g_mt.fade)
				g_mt.env++;
			gain = g_mt.env;
		}
		y = (s32)g_mt.f[g_mt.f_pos++] * (s32)master >> 8;
		y = y * (s32)gain / (s32)g_mt.fade;
		a = (u32)(y < 0 ? -y : y);
		if (a > g_xd_mt.play_peak)
			g_xd_mt.play_peak = a;
		for (u32 c = 0; c < ch; c++) {
			u8 *o = out + f * (int)frame + 2 * (int)c;

			put_le16(o, (u16)clamp16((s16)get_le16(o) + y));
		}
	}
	g_mt.last_ms = xa_now_ms();
	g_xd_mt.played = g_mt.pos;
	if (g_mt.stopping && (end || !g_mt.env)) {
		mt_discard(NULL, "stopped");
	} else if (end) {
		g_mt.state = XD_MT_READY;
		g_xd_mt.state = XD_MT_READY;
		g_mt.env = 0;
		g_xd_mt.done++;
		xa_log("voice: pad %d: mic test: played to the end; clip held\n", g_mt.pad);
	}
}

/* the test's pad lost its headset (or its format changed): the test ends, the clip goes */
void mt_pad_gone(int pad, const char *why)
{
	if (pad == g_mt.pad)
		mt_discard(&g_xd_mt.cancels, why);
}

bool xd_mictest_enabled(void)
{
	return g_xd_mt.enable == 1;
}

static u32 mt_refuse(int pad, u32 why)
{
	static const char *const whys[] = { "", "switched off (XWMT enable 0)", "busy",
					    "no headset", "the mic is muted", "out of memory",
					    "no clip held" };

	g_xd_mt.refused++;
	g_xd_mt.last_refusal = why;
	xa_log("voice: pad %d: mic test: refused: %s\n", pad, whys[why]);
	return why;
}

/* the clip's length in whole seconds as the panel shows it (at least 1 once there is one) */
static u32 mt_clip_secs(void)
{
	u32 s = (g_mt.n + MT_RATE / 2) / MT_RATE;

	return s ? s : g_mt.n ? 1 : 0;
}

/* rec_ms, clamped (a bad value is written back: xbdm sees what is in effect) */
static u32 mt_rec_ms(void)
{
	u32 ms = g_xd_mt.rec_ms;

	if (ms < 1000 || ms > XD_MT_MAX_MS)
		g_xd_mt.rec_ms = ms = XD_MT_MAX_MS;
	return ms;
}

/* a buffer for a recording: a held full-size one is reused; else MT_BIG, else MT_SMALL.
 * refuse (XW_DEV tests): 1 = act as if MT_BIG failed, 2 = as if both did. */
static bool mt_buffer(u32 refuse)
{
	u32 tb = xd_timebase();

	if (g_mt.buf && g_mt.cap == MT_BIG && !refuse)
		return true;
	mt_free();
	for (u32 i = 0; i < 2 && !g_mt.buf; i++) {
		u32 cap = i ? MT_SMALL : MT_BIG;

		g_mt.buf = refuse > i ? NULL : malloc(cap * sizeof(s16));
		if (!g_mt.buf) {
			g_xd_mt.alloc_fails++;
			continue;
		}
		g_mt.cap = cap;
		g_xd_mt.allocs++;
		g_xd_mt.buf_addr = (u32)(uintptr_t)g_mt.buf;
		g_xd_mt.buf_bytes = g_xd_mt.heap_bytes = cap * sizeof(s16);
	}
	g_xd_mt.alloc_us = (xd_timebase() - tb) / 50; /* the timebase is 50 MHz */
	if (!g_mt.buf)
		return false;
	g_mt.next_ms = g_mt.cap / MT_PER_MS;
	if (g_mt.cap != MT_BIG) {
		g_xd_mt.fallback++;
		xa_log("voice: mic test: no 1.44 MB for the buffer: recordings stop at %u s\n",
		       XD_MT_MS / 1000);
	}
	return true;
}

static u32 mt_start(int pad, u32 refuse)
{
	struct xv_pad *p;
	u32 ms = mt_rec_ms();

	if (!xd_mictest_enabled())
		return mt_refuse(pad, XD_MT_OFF);
	if (g_mt.state != XD_MT_IDLE && g_mt.state != XD_MT_READY)
		return mt_refuse(pad, XD_MT_BUSY);
	if (pad < 0 || pad >= PADS || !hs_format_ok(&g_pad[pad]) || g_quiesced)
		return mt_refuse(pad, XD_MT_NO_HEADSET);
	if (g_mic_mute[pad])
		return mt_refuse(pad, XD_MT_MUTED);
	if (!mt_buffer(refuse)) {
		mt_discard(NULL, "discarded: no memory for a new recording");
		return mt_refuse(pad, XD_MT_NO_MEMORY);
	}
	if (ms > g_mt.cap / MT_PER_MS)
		ms = g_mt.cap / MT_PER_MS;
	p = &g_pad[pad];
	rs_init(&g_mt.rs_rec, p->rate_in, MT_RATE);
	g_mt.pad = pad;
	g_mt.want = MT_PER_MS * ms;
	g_mt.n = g_mt.pos = 0;
	g_mt.sum = 0;
	g_mt.stopping = 0;
	g_mt.hold_end = 0; /* the panel's recordings play; MT_CMD_START_HOLD sets it after */
	g_mt.env = 0;
	g_mt.last_ms = xa_now_ms();
	g_mt.state = XD_MT_RECORDING;
	g_xd_mt.state = XD_MT_RECORDING;
	g_xd_mt.pad = (u32)pad;
	g_xd_mt.want = g_mt.want;
	g_xd_mt.max_ms = ms;
	g_xd_mt.recorded = g_xd_mt.played = 0;
	g_xd_mt.rec_peak = g_xd_mt.rec_rms = g_xd_mt.rec_shut = g_xd_mt.play_peak = 0;
	g_xd_mt.rec_gain = XD_MIC_GAINS[gain_index(g_xd.applied_mic_gain)];
	g_xd_mt.rec_gate = g_xd.applied_gate;
	g_xd_mt.starts++;
	xa_log("voice: pad %d: mic test: recording, at most %u ms (mic %u Hz x%u, volume %u%%, "
	       "gate %u)\n", pad, ms, p->rate_in, p->ch_in, g_xd_mt.rec_gain, g_xd_mt.rec_gate);
	return XD_MT_OK;
}

u32 xd_mictest_start(int pad)
{
	return mt_start(pad, 0);
}

void xd_mictest_stop(void)
{
	if (g_mt.state == XD_MT_RECORDING)
		mt_end_recording("stopped");
}

u32 xd_mictest_play(void)
{
	if (g_mt.state == XD_MT_IDLE)
		return mt_refuse(g_mt.pad, XD_MT_NO_CLIP);
	if (g_mt.state != XD_MT_READY)
		return mt_refuse(g_mt.pad, XD_MT_BUSY);
	mt_start_play("played again");
	return XD_MT_OK;
}

void xd_mictest_pause(void)
{
	if (g_mt.state != XD_MT_PLAYING || g_mt.stopping)
		return;
	g_mt.state = XD_MT_PAUSED; /* mt_mix fades it out, then takes nothing more */
	g_xd_mt.state = XD_MT_PAUSED;
	g_xd_mt.pauses++;
	xa_log("voice: pad %d: mic test: paused at %u ms\n", g_mt.pad, g_mt.pos / MT_PER_MS);
}

void xd_mictest_resume(void)
{
	if (g_mt.state != XD_MT_PAUSED || g_mt.stopping)
		return;
	g_mt.state = XD_MT_PLAYING; /* mt_mix fades in from where the fade-out left it */
	g_xd_mt.state = XD_MT_PLAYING;
	g_mt.last_ms = xa_now_ms(); /* the stall check starts again */
	xa_log("voice: pad %d: mic test: playing on from %u ms\n", g_mt.pad, g_mt.pos / MT_PER_MS);
}

void xd_mictest_cancel(const char *why)
{
	mt_drop(&g_xd_mt.cancels, why);
}

/* The panel calls this each time it closes with nothing recording or playing (and a call
 * while something is recording or playing is ignored here too). A clip that finishes while
 * the panel is hidden is READY with no call pending: the next call can only come from the
 * next time the panel closes, i.e. after it has been opened again. So the panel needs no
 * state of its own for "kept until opened and closed". While a tool reads the clip (`keep`)
 * the close leaves it, the same way. */
void xd_mictest_panel_closed(void)
{
	if (g_mt.state != XD_MT_PAUSED && g_mt.state != XD_MT_READY)
		return;
	if (xa_now_ms() < g_mt.keep_until) {
		g_xd_mt.kept++;
		xa_log("voice: pad %d: mic test: the panel closed; clip kept (xbdm keep)\n",
		       g_mt.pad);
		return;
	}
	mt_drop(&g_xd_mt.panel_closes, "discarded: the panel closed");
}

/* hot reload (xv_quiesce): no test, no buffer (this image stays loaded), the game not ducked */
void xd_mictest_quiesce(void)
{
	mt_discard(&g_xd_mt.cancels, "stopped: hot reload");
	g_xd_mt.duck_now = 256;
	xg_duck(256);
}

/* what the panel shows: RECORDING counts up (seconds recorded) with pct of the auto-stop;
 * PLAYING / PAUSED the clip's length and pct played; READY the clip's length */
static void mt_show(void)
{
	u32 secs = 0, pct = 0, clip = 0;

	switch (g_mt.state) {
	case XD_MT_RECORDING:
		secs = g_mt.n / MT_RATE;
		pct = g_mt.want ? g_mt.n * 100 / g_mt.want : 0;
		clip = g_mt.n;
		break;
	case XD_MT_PLAYING:
	case XD_MT_PAUSED:
		pct = g_mt.n ? g_mt.pos * 100 / g_mt.n : 0;
		/* fall through */
	case XD_MT_READY:
		secs = mt_clip_secs();
		clip = g_mt.n;
		break;
	}
	g_xd_mt.secs = secs;
	g_xd_mt.pct = pct;
	g_xd_mt.clip_ms = clip / MT_PER_MS;
}

u32 xd_mictest_state(u32 *secs, u32 *pct)
{
	mt_show();
	*secs = g_xd_mt.secs;
	*pct = g_xd_mt.pct;
	return g_mt.state;
}

u32 xd_mictest_max_secs(void)
{
	return g_xd_mt.max_ms / 1000;
}

/* driver thread, from xd_poll: xbdm's commands, the stall check, what the panel shows, the
 * auto-stop in effect and the game's duck */
void mt_poll(void)
{
	u32 cmd = g_xd_mt.cmd, duck_to = 256, d = g_xd_mt.duck_pct;

	if (g_xd_mt.enable > 1)
		g_xd_mt.enable = 1;
	if (g_xd_mt.keep) {
		u32 s = g_xd_mt.keep < MT_KEEP_MAX_S ? g_xd_mt.keep : MT_KEEP_MAX_S;

		g_xd_mt.keep = 0;
		g_mt.keep_until = xa_now_ms() + (u64)s * 1000;
	}
	if (cmd) {
		int pad = (int)g_xd_mt.cmd_pad;

		g_xd_mt.cmd = 0;
		switch (cmd) {
		case MT_CMD_START:
			mt_start(pad, 0);
			break;
		case MT_CMD_CANCEL:
			xd_mictest_cancel("xbdm");
			break;
		case MT_CMD_STOP:
			xd_mictest_stop();
			break;
		case MT_CMD_PLAY:
			xd_mictest_play();
			break;
		case MT_CMD_PAUSE:
			xd_mictest_pause();
			break;
		case MT_CMD_RESUME:
			xd_mictest_resume();
			break;
		case MT_CMD_CLOSED:
			xd_mictest_panel_closed();
			break;
		case MT_CMD_START_HOLD:
			if (mt_start(pad, 0) == XD_MT_OK)
				g_mt.hold_end = 1;
			break;
		case MT_CMD_STOP_HOLD:
			if (g_mt.state == XD_MT_RECORDING) {
				g_mt.hold_end = 1;
				mt_end_recording("stopped (xbdm)");
			}
			break;
#ifdef XW_DEV
		case MT_CMD_START_SMALL:
		case MT_CMD_START_NOMEM:
			mt_start(pad, cmd - MT_CMD_START_SMALL + 1);
			break;
#endif
		}
	}
	if ((g_mt.state == XD_MT_RECORDING || g_mt.state == XD_MT_PLAYING || g_mt.stopping) &&
	    xa_now_ms() - g_mt.last_ms > MT_STALL_MS)
		mt_discard(&g_xd_mt.stalls, g_mt.state == XD_MT_RECORDING
						    ? "stopped: no mic audio"
						    : "stopped: no earpiece audio");
	if (g_mt.state == XD_MT_IDLE) {
		u32 ms = mt_rec_ms();

		g_xd_mt.max_ms = ms < g_mt.next_ms ? ms : g_mt.next_ms;
	}
	mt_show();
	if (d > 100)
		g_xd_mt.duck_pct = d = 100;
	if (g_mt.state == XD_MT_PLAYING && !g_mt.stopping)
		duck_to = d * 256 / 100;
	g_xd_mt.duck_now = ramp_to(g_xd_mt.duck_now, duck_to, MT_DUCK_STEP);
	xg_duck(g_xd_mt.duck_now);
}
