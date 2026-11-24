// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of the mic test's state machine (xbox360/mictest_360.c, compiled in whole) on a
 * fake clock, with fake mic chunks (1 ms, 24 kHz mono) and fake earpiece pulls (1 ms, 48 kHz
 * stereo), one of each per driver pass, then mt_poll as xd_poll calls it:
 *
 *   for dev in "" -DXW_DEV; do
 *     cc -O2 -Wall -Wextra -Itests/host $dev -o /tmp/test_mictest tests/test_mictest.c &&
 *     /tmp/test_mictest || break
 *   done
 *
 * Checks: every transition of docs/ROADMAP.md Part 3 item 2 (start, stop, play, pause,
 * resume, cancel, panel closed, through the functions and through XWMT cmd), the auto-stop
 * (30 s and a short rec_ms), both allocation fallbacks (malloc failures injected by size),
 * mute during recording, the pad leaving in each state, the stall watchdog, panel-closed in
 * each state (and a clip that finishes while the panel is hidden), pause / resume continuity
 * (the earpiece output compared sample for sample with the resampled clip times the fade
 * gains: nothing skipped or repeated, fades exactly MT_FADE_MS), the duck per state, what the
 * panel is shown, and the buffer freed on every path to IDLE (allocations counted).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- malloc / free, counted, with failures injected by size --------------------------------- */
static unsigned g_allocs, g_frees;
static int g_live;
static int g_fail_big, g_fail_small;

static void *fake_malloc(size_t n)
{
	if ((n == 1440000 && g_fail_big) || (n == 480000 && g_fail_small))
		return NULL;
	g_allocs++;
	g_live++;
	return malloc(n);
}

static void fake_free(void *p)
{
	if (p) {
		g_frees++;
		g_live--;
	}
	free(p);
}

/* kernel_360.h's sig_mismatch keeps 32-bit kernel addresses in u32: fine on the console */
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#define malloc fake_malloc
#define free fake_free
#include "../xbox360/mictest_360.c"
#undef malloc
#undef free

/* ---- fakes for the rest of the plugin --------------------------------------------------------- */
struct xv_pad g_pad[PADS];
u32 g_quiesced;
u8 g_mic_mute[PADS];
struct xd_mic g_dmic[PADS];
const u16 XD_MIC_GAINS[XD_MIC_LEVELS] = { 25, 35, 50, 70, 100, 140, 200, 280, 400, 560 };
struct xd_status g_xd = { .applied_mic_gain = 100, .applied_gate = 1 };

static u64 g_ms = 1000;
static u32 g_duck = 256;
static u32 g_gain = XD_Q12, g_thr;
static int g_verbose;

u64 xa_now_ms(void) { return g_ms; }
u32 xd_timebase(void) { return (u32)(g_ms * 50000); }
u32 gain_index(u32 pct) { (void)pct; return 4; }
u32 mic_gain_target(int pad) { (void)pad; return g_gain; } /* 100 % unless a test moves it */
u32 mic_gate_threshold(int pad) { (void)pad; return g_thr; } /* gate off unless a test sets it */
void xg_duck(u32 q8) { g_duck = q8; }

void xa_log(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	va_start(ap, fmt);
	printf("     log: ");
	vprintf(fmt, ap);
	va_end(ap);
}

/* ---- the driver passes ------------------------------------------------------------------------ */
#define EAR_FRAMES 48               /* 1 ms at 48 kHz */
#define FADE (48000 / 1000 * MT_FADE_MS) /* 240 */
#define MAX_OUT (2 * MT_BIG + 100000)

static int g_fail;
static char g_what[200];
static s16 *g_out;   /* left channel of every earpiece frame pulled while capturing */
static int g_out_n, g_capture;
static u32 g_master = 256;
static s16 g_mic_level = 3000;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		g_fail = 1;
}

static void pad_up(int pad)
{
	memset(&g_pad[pad], 0, sizeof(g_pad[pad]));
	g_pad[pad].usb_present = 1;
	g_pad[pad].rate_in = 24000;
	g_pad[pad].ch_in = 1;
	g_pad[pad].rate_out = 48000;
	g_pad[pad].ch_out = 2;
}

/* one driver pass of 1 ms on pad 0: a mic chunk (if mic), an earpiece block (if ear), mt_poll */
static void pass(int mic, int ear)
{
	u8 chunk[2 * 24], blk[EAR_FRAMES * 4];

	g_ms++;
	if (mic) {
		for (int i = 0; i < 24; i++)
			put_le16(chunk + 2 * i, (u16)(i & 1 ? g_mic_level : -g_mic_level));
		mt_record(0, chunk, 24);
	}
	if (ear) {
		memset(blk, 0, sizeof(blk));
		mt_mix(0, blk, sizeof(blk), g_master);
		for (int f = 0; g_capture && f < EAR_FRAMES && g_out_n < MAX_OUT; f++)
			g_out[g_out_n++] = (s16)get_le16(blk + 4 * f);
	}
	mt_poll();
}

static void passes(int n, int mic, int ear)
{
	while (n-- > 0)
		pass(mic, ear);
}

static u32 st(void)
{
	u32 s, p;

	return xd_mictest_state(&s, &p);
}

static u32 secs(void)
{
	u32 s, p;

	xd_mictest_state(&s, &p);
	return s;
}

static u32 pct(void)
{
	u32 s, p;

	xd_mictest_state(&s, &p);
	return p;
}

/* IDLE with nothing allocated and nothing claimed */
static int idle_ok(void)
{
	return st() == XD_MT_IDLE && g_live == 0 && g_mt.buf == NULL && g_xd_mt.heap_bytes == 0 &&
	       g_xd_mt.buf_addr == 0 && g_xd_mt.clip_ms == 0;
}

/* from IDLE: a recording of `ms` ms, stopped: PLAYING */
static void record(u32 ms)
{
	xd_mictest_start(0);
	passes((int)ms, 1, 1);
	xd_mictest_stop();
}

/* playback through to READY (at most `ms` passes) */
static void play_out(int ms)
{
	for (int i = 0; i < ms && st() != XD_MT_READY && st() != XD_MT_IDLE; i++)
		pass(1, 1);
}

static void reset_all(void)
{
	xd_mictest_cancel("test reset");
	passes(20, 1, 1);
	g_fail_big = g_fail_small = 0;
	g_mic_mute[0] = 0;
	g_xd_mt.rec_ms = XD_MT_MAX_MS;
	pad_up(0);
	passes(5, 1, 1);
}

/* ---- the clip and the gains the playback must apply ---------------------------------------- */

/* a clip of nonzero positive samples (every resampled value and every faded one is nonzero,
 * except the resampler's very first output, 0, so a zero in the output means "nothing mixed") */
static void fill_clip(void)
{
	for (u32 i = 0; i < g_mt.n; i++)
		g_mt.buf[i] = (s16)(1000 + (i * 7919u) % 19000u);
}

/* the clip as the playback must hear it: the live mic path's steps (voice_priv.h gate_*, then
 * shape()) 1 ms at a time, the gate starting shut, at the threshold and volume the fakes
 * give (g_thr, g_gain); the order mic_analyse / mic_shaped use them in */
static void gated(s16 *y)
{
	struct xd_mic m;
	struct xd_shape sh;
	u32 win = gate_win(MT_RATE);

	memset(&m, 0, sizeof(m));
	memset(&sh, 0, sizeof(sh));
	m.closed = 1;
	sh.shut = g_thr ? 32768 : 0;
	sh.gain = g_gain;
	for (u32 c = 0; c < g_mt.n; c += MT_GCHUNK) {
		u32 n = g_mt.n - c < MT_GCHUNK ? g_mt.n - c : MT_GCHUNK, shut_to;

		gate_chunk_start(&m, g_thr);
		for (u32 i = 0; i < n; i++)
			if (gate_push(&m, g_mt.buf[c + i], win))
				gate_window_end(&m, isqrt((u32)(m.sum / m.n)), g_thr, MT_RATE);
		gate_chunk_end(&m, g_thr, win);
		shut_to = gate_shut(&m) ? 32768 : 0;
		for (u32 i = 0; i < n; i++)
			y[c + i] = shape(&sh, g_mt.buf[c + i], g_gain, shut_to, MT_RATE);
	}
}

/* the clip at the earpiece rate, as mt_mix's gate and resampler produce it */
static int resampled(s16 *r)
{
	static s16 y[MT_BIG];
	struct rs rs;
	int n = 0;

	gated(y);
	rs_init(&rs, MT_RATE, 48000);
	for (u32 i = 0; i < g_mt.n; i++)
		n += rs_push(&rs, y[i], r + n);
	return n;
}

struct ev { int k, down; };

/* the gain of each mixed frame k (0..nk-1) given direction changes at mixed-frame indices */
static void gains(u32 *g, int nk, const struct ev *ev, int nev)
{
	u32 env = 0;
	int down = 0, e = 0;

	for (int k = 0; k < nk; k++) {
		while (e < nev && ev[e].k == k)
			down = ev[e++].down;
		if (down) {
			g[k] = env ? env-- : 0;
		} else {
			if (env < FADE)
				env++;
			g[k] = env;
		}
	}
}

static int nonzero(const s16 *a, int n, s16 *z)
{
	int m = 0;

	for (int i = 0; i < n; i++)
		if (a[i])
			z[m++] = a[i];
	return m;
}

/* frames mixed so far in this capture: every nonzero one, plus the resampler's first (0) */
static int mixed_so_far(void)
{
	int m = 0;

	for (int i = 0; i < g_out_n; i++)
		m += g_out[i] != 0;
	return m ? m + 1 : 0;
}

/* compares the captured playback with the clip x the gains: returns 1 if identical */
static int compare_playback(const struct ev *ev, int nev, const char *what)
{
	static s16 r[MAX_OUT], z[MAX_OUT], e[MAX_OUT];
	static u32 g[MAX_OUT];
	int nr = resampled(r), nz = nonzero(g_out, g_out_n, z), ne = 0, bad = -1;

	gains(g, nr, ev, nev);
	for (int k = 0; k < nr; k++) {
		s32 y = (s32)r[k] * (s32)g_master >> 8;

		y = y * (s32)g[k] / FADE;
		if (y)
			e[ne++] = (s16)y;
	}
	for (int i = 0; i < ne && i < nz && bad < 0; i++)
		if (e[i] != z[i])
			bad = i;
	if (bad >= 0 || ne != nz)
		printf("     %s: %d frames expected, %d mixed, first difference at %d\n", what, ne,
		       nz, bad);
	return bad < 0 && ne == nz;
}

/* ---- the tests -------------------------------------------------------------------------------- */

static void t_defaults(void)
{
	check(g_xd_mt.magic == 0x58574D54 && g_xd_mt.version == 3 && MT_WORDS == 43,
	      "XWMT v3, 43 words");
	check(g_xd_mt.enable == 1 && g_xd_mt.rec_ms == 30000 && g_xd_mt.duck_pct == 25,
	      "defaults: on, auto-stop 30 s, duck 25 %");
	check(idle_ok() && xd_mictest_max_secs() == 30, "IDLE, nothing allocated, max 30 s");
	check(offsetof(struct xd_mt_status, alloc_fails) == 31 * 4 &&
	      offsetof(struct xd_mt_status, clip_ms) == 32 * 4 &&
	      offsetof(struct xd_mt_status, alloc_us) == 40 * 4, "v1 positions kept, v2 appended");
}

static void t_record_play_ready(void)
{
	u32 a0 = g_allocs;

	check(xd_mictest_start(0) == XD_MT_OK && st() == XD_MT_RECORDING, "start: RECORDING");
	check(g_allocs == a0 + 1 && g_xd_mt.heap_bytes == 1440000 && g_xd_mt.buf_bytes == 1440000 &&
	      g_xd_mt.fallback == 0, "start: 1.44 MB allocated");
	passes(2500, 1, 1);
	check(secs() == 2 && pct() == 8 && g_xd_mt.clip_ms == 2500,
	      "RECORDING counts up: 2 s, 8 % of 30 s, clip_ms 2500");
	check(g_duck == 256, "RECORDING: the game is not ducked");
	xd_mictest_stop();
	check(st() == XD_MT_PLAYING && g_xd_mt.plays == 1 && g_mt.n == 60000,
	      "stop: PLAYING the 2.5 s recorded");
	passes(10, 1, 1);
	check(g_duck == 64, "PLAYING: the game ducked to 25 %");
	passes(490, 1, 1);
	check(secs() == 3 && pct() >= 19 && pct() <= 20, "PLAYING: the clip's length, % played (20)");
	play_out(4000);
	check(st() == XD_MT_READY && g_xd_mt.done == 1 && g_xd_mt.played == g_mt.n,
	      "end of clip: READY, all of it played");
	check(secs() == 3 && g_xd_mt.clip_ms == 2500 && g_live == 1, "READY: the clip is held");
	passes(10, 1, 1);
	check(g_duck == 256, "READY: the game back at 100 %");
	check(xd_mictest_play() == XD_MT_OK && st() == XD_MT_PLAYING && g_xd_mt.plays == 2 &&
	      g_allocs == a0 + 1, "play: PLAYING again, same buffer");
	play_out(4000);
	check(st() == XD_MT_READY && g_xd_mt.done == 2, "played again to the end: READY");
	check(xd_mictest_play() == XD_MT_OK, "and again");
	play_out(4000);
	check(st() == XD_MT_READY && g_xd_mt.done == 3, "as many times as wanted");
	/* start from READY: the held clip is dropped, the full-size buffer reused */
	check(xd_mictest_start(0) == XD_MT_OK && st() == XD_MT_RECORDING && g_allocs == a0 + 1 &&
	      g_mt.n == 0, "start from READY: RECORDING, clip dropped, buffer reused");
	xd_mictest_cancel("test");
	check(idle_ok() && g_frees == g_allocs, "Left while recording: IDLE, freed");
}

static void t_continuity(void)
{
	struct ev ev[8];
	int nev = 0, before;

	record(1200);
	fill_clip();
	g_out_n = 0;
	g_capture = 1;
	/* a pause at full gain, a long silence, a resume; a pause and a resume mid-fade */
	passes(300, 1, 1);
	ev[nev++] = (struct ev){ mixed_so_far(), 1 };
	xd_mictest_pause();
	check(st() == XD_MT_PAUSED && g_xd_mt.pauses == 1, "pause: PAUSED");
	before = g_out_n;
	passes(200, 1, 1);
	{
		int fadeout = 0;

		for (int i = before; i < g_out_n; i++)
			fadeout += g_out[i] != 0;
		check(fadeout == FADE, "pause: faded out over exactly 5 ms (240 frames), then silence");
	}
	check(g_duck == 256, "PAUSED: the game not ducked");
	check(secs() == 1 && pct() > 0 && pct() < 100, "PAUSED: the clip's length, % played");
	ev[nev++] = (struct ev){ mixed_so_far(), 0 };
	xd_mictest_resume();
	check(st() == XD_MT_PLAYING, "resume: PLAYING");
	passes(400, 1, 1);
	ev[nev++] = (struct ev){ mixed_so_far(), 1 };
	xd_mictest_pause();
	pass(1, 1); /* one block of the fade-out: 48 of 240 frames */
	ev[nev++] = (struct ev){ mixed_so_far(), 0 };
	xd_mictest_resume();
	play_out(4000);
	g_capture = 0;
	check(st() == XD_MT_READY, "the paused playback ends: READY");
	check(compare_playback(ev, nev, "pauses"),
	      "pause / resume: every sample once, in order, fades 1..240 / 240..1");
	/* a playback with no pause: the fade-in of the first 5 ms, then the clip as it is */
	g_out_n = 0;
	g_capture = 1;
	xd_mictest_play();
	play_out(4000);
	g_capture = 0;
	check(compare_playback(NULL, 0, "plain"), "play again: fade-in over 5 ms, the clip exact");
	xd_mictest_cancel("test");
	check(idle_ok(), "Left when READY: IDLE, freed");
}

static void t_cancels(void)
{
	record(800);
	passes(50, 1, 1);
	xd_mictest_cancel("test");
	check(st() == XD_MT_PLAYING && g_mt.stopping, "Left while playing: fading out first");
	pass(1, 1);
	check(st() == XD_MT_PLAYING, "  still fading after 1 ms");
	passes(5, 1, 1);
	check(idle_ok(), "  then IDLE, freed (within 6 ms)");
	passes(10, 1, 1);
	check(g_duck == 256, "  the game back at 100 %");

	record(800);
	passes(50, 1, 1);
	xd_mictest_pause();
	passes(10, 1, 1);
	xd_mictest_cancel("test");
	check(idle_ok(), "Left while PAUSED (faded): IDLE at once, freed");

	record(800);
	passes(50, 1, 1);
	xd_mictest_pause();
	pass(1, 1);
	xd_mictest_cancel("test");
	check(st() == XD_MT_PAUSED && g_mt.stopping, "Left while a pause is fading: the fade ends");
	xd_mictest_resume();
	check(st() == XD_MT_PAUSED, "  and a resume then does nothing");
	passes(6, 1, 1);
	check(idle_ok(), "  then IDLE, freed");

	xd_mictest_cancel("test");
	check(idle_ok(), "Left when IDLE: nothing");
}

static void t_panel_closed(void)
{
	u32 pc;

	xd_mictest_panel_closed();
	check(idle_ok(), "panel closed when IDLE: nothing");

	xd_mictest_start(0);
	passes(700, 1, 1);
	xd_mictest_panel_closed();
	check(st() == XD_MT_RECORDING, "panel closed while RECORDING: it carries on");
	xd_mictest_stop();
	passes(50, 1, 1);
	xd_mictest_panel_closed();
	check(st() == XD_MT_PLAYING && !g_mt.stopping, "panel closed while PLAYING: it carries on");
	play_out(2000);
	check(st() == XD_MT_READY, "  it finishes while the panel is hidden: READY, kept");
	passes(500, 1, 1);
	check(st() == XD_MT_READY, "  still held 0.5 s later");
	pc = g_xd_mt.panel_closes;
	xd_mictest_panel_closed(); /* the panel opened again, and closed */
	check(idle_ok() && g_xd_mt.panel_closes == pc + 1,
	      "  the panel opened and closed again: IDLE, freed");

	record(800);
	passes(50, 1, 1);
	xd_mictest_pause();
	passes(10, 1, 1);
	xd_mictest_panel_closed();
	check(idle_ok(), "panel closed while PAUSED: IDLE, freed");

	record(800);
	play_out(2000);
	xd_mictest_panel_closed();
	check(idle_ok(), "panel closed while READY: IDLE, freed");

	record(800);
	passes(50, 1, 1);
	xd_mictest_pause();
	pass(1, 1);
	xd_mictest_panel_closed();
	check(st() == XD_MT_PAUSED && g_mt.stopping, "panel closed while a pause fades: fade first");
	passes(6, 1, 1);
	check(idle_ok(), "  then IDLE, freed");
}

static void t_autostop(void)
{
	xd_mictest_start(0);
	passes(29999, 1, 1);
	check(st() == XD_MT_RECORDING && secs() == 29, "29.999 s: still recording");
	pass(1, 1);
	check(st() == XD_MT_PLAYING && g_mt.n == MT_BIG && g_xd_mt.recorded == MT_BIG,
	      "30 s: auto-stop, then it plays");
	play_out(31000);
	check(st() == XD_MT_READY && g_xd_mt.played == MT_BIG && secs() == 30,
	      "  plays once to the end: READY, 30 s");
	xd_mictest_cancel("test");
	check(idle_ok(), "  Left: IDLE, freed");

	g_xd_mt.rec_ms = 1000;
	passes(2, 1, 1);
	check(xd_mictest_max_secs() == 1, "rec_ms 1000: the auto-stop is 1 s");
	xd_mictest_start(0);
	passes(1000, 1, 1);
	check(st() == XD_MT_PLAYING && g_mt.n == 24000, "  stops at 1 s and plays");
	xd_mictest_cancel("test");
	passes(10, 1, 1);
	g_xd_mt.rec_ms = 500;
	passes(1, 1, 1);
	check(g_xd_mt.rec_ms == 30000, "rec_ms 500: written back as 30000");
	g_xd_mt.rec_ms = 40000;
	passes(1, 1, 1);
	check(g_xd_mt.rec_ms == 30000 && xd_mictest_max_secs() == 30,
	      "rec_ms 40000: written back as 30000");
	check(idle_ok(), "IDLE, freed");
}

static void t_alloc(void)
{
	u32 fb = g_xd_mt.fallback, af = g_xd_mt.alloc_fails, rf = g_xd_mt.refused;

	g_fail_big = 1;
	check(xd_mictest_start(0) == XD_MT_OK && g_xd_mt.heap_bytes == 480000 &&
	      g_xd_mt.fallback == fb + 1 && g_xd_mt.alloc_fails == af + 1,
	      "1.44 MB refused: the 480 KB buffer, fallback counted");
	check(xd_mictest_max_secs() == 10 && g_mt.want == MT_SMALL, "  the auto-stop is 10 s");
	passes(10000, 1, 1);
	check(st() == XD_MT_PLAYING && g_mt.n == MT_SMALL, "  auto-stop at 10 s, then it plays");
	xd_mictest_cancel("test");
	passes(10, 1, 1);
	check(idle_ok() && xd_mictest_max_secs() == 10, "  IDLE, freed; the panel is told 10 s");
	g_fail_big = 0;
	xd_mictest_start(0);
	check(g_xd_mt.heap_bytes == 1440000 && xd_mictest_max_secs() == 30,
	      "the big buffer available again: 30 s");
	xd_mictest_cancel("test");
	passes(10, 1, 1);

	g_fail_big = g_fail_small = 1;
	check(xd_mictest_start(0) == XD_MT_NO_MEMORY && g_xd_mt.refused == rf + 1 &&
	      g_xd_mt.last_refusal == XD_MT_NO_MEMORY, "both refused: NO_MEMORY");
	check(idle_ok(), "  IDLE, nothing held");
	/* READY with a held clip, then a start that cannot get memory: the clip is gone too */
	g_fail_big = g_fail_small = 0;
	g_fail_big = 1;
	record(800);
	play_out(2000);
	check(st() == XD_MT_READY && g_mt.cap == MT_SMALL, "a 10 s clip held");
	g_fail_small = 1;
	check(xd_mictest_start(0) == XD_MT_NO_MEMORY && idle_ok(),
	      "start from READY with no memory: refused, IDLE, freed");
	g_fail_big = g_fail_small = 0;
	passes(5, 1, 1);
}

static void t_mute(void)
{
	u32 ms = g_xd_mt.mute_stops, d = g_xd_mt.discards, p = g_xd_mt.plays;

	xd_mictest_start(0);
	passes(800, 1, 1);
	g_mic_mute[0] = 1;
	pass(1, 1);
	check(st() == XD_MT_PLAYING && g_xd_mt.mute_stops == ms + 1 && g_mt.n == 19200,
	      "mute after 0.8 s: the recording ends and plays");
	play_out(2000);
	check(st() == XD_MT_READY, "  then READY");
	check(xd_mictest_start(0) == XD_MT_MUTED && st() == XD_MT_READY,
	      "muted: a start is refused, the clip kept");
	g_mic_mute[0] = 0;
	xd_mictest_cancel("test");

	xd_mictest_start(0);
	passes(300, 1, 1);
	g_mic_mute[0] = 1;
	pass(1, 1);
	check(idle_ok() && g_xd_mt.plays == p + 1 && g_xd_mt.discards == d + 2,
	      "mute after 0.3 s: too short, discarded, IDLE, freed");
	g_mic_mute[0] = 0;
	xd_mictest_start(0);
	passes(300, 1, 1);
	xd_mictest_stop();
	check(idle_ok(), "stop after 0.3 s: too short, IDLE, freed");
}

static void t_pad_gone(void)
{
	static const char *const names[] = { "", "RECORDING", "PLAYING", "PAUSED", "READY" };

	for (u32 s = XD_MT_RECORDING; s <= XD_MT_READY; s++) {
		char what[80];

		xd_mictest_start(0);
		passes(700, 1, 1);
		if (s >= XD_MT_PLAYING)
			xd_mictest_stop();
		if (s == XD_MT_PAUSED) {
			passes(50, 1, 1);
			xd_mictest_pause();
			passes(10, 1, 1);
		}
		if (s == XD_MT_READY)
			play_out(2000);
		mt_pad_gone(1, "another pad");
		check(st() == s, "another pad leaving: nothing");
		mt_pad_gone(0, "test");
		snprintf(what, sizeof(what), "the pad leaving while %s: IDLE, freed", names[s]);
		check(idle_ok(), what);
		passes(10, 1, 1);
		check(g_duck == 256, "  the game not ducked");
	}
}

static void t_stalls(void)
{
	u32 s0 = g_xd_mt.stalls;

	xd_mictest_start(0);
	passes(100, 1, 1);
	passes(2000, 0, 1);
	check(st() == XD_MT_RECORDING, "no mic for 2 s: still recording");
	passes(2, 0, 1);
	check(idle_ok() && g_xd_mt.stalls == s0 + 1, "no mic for 2.002 s: IDLE, freed");

	record(800);
	passes(10, 1, 1);
	passes(2002, 1, 0);
	check(idle_ok() && g_xd_mt.stalls == s0 + 2, "no earpiece while PLAYING: IDLE, freed");

	record(800);
	passes(10, 1, 1);
	xd_mictest_pause();
	passes(10, 1, 1);
	passes(5000, 1, 0);
	check(st() == XD_MT_PAUSED, "no earpiece while PAUSED: kept");
	xd_mictest_resume();
	passes(1000, 1, 1);
	check(st() == XD_MT_READY, "  resumed: plays to the end");
	passes(5000, 0, 0);
	check(st() == XD_MT_READY, "nothing at all while READY: kept");
	xd_mictest_cancel("test");
	check(idle_ok(), "IDLE, freed");
}

static void t_refusals(void)
{
	check(xd_mictest_play() == XD_MT_NO_CLIP, "play when IDLE: NO_CLIP");
	g_xd_mt.enable = 0;
	check(xd_mictest_start(0) == XD_MT_OFF && idle_ok(), "enable 0: start refused (OFF)");
	g_xd_mt.enable = 1;
	check(xd_mictest_start(5) == XD_MT_NO_HEADSET, "no such pad: NO_HEADSET");
	g_pad[0].usb_present = 0;
	check(xd_mictest_start(0) == XD_MT_NO_HEADSET && idle_ok(), "no headset: NO_HEADSET");
	pad_up(0);
	record(800);
	check(xd_mictest_start(0) == XD_MT_BUSY && st() == XD_MT_PLAYING,
	      "start while PLAYING: BUSY, nothing changes");
	check(xd_mictest_play() == XD_MT_BUSY, "play while PLAYING: BUSY");
	xd_mictest_stop();
	check(st() == XD_MT_PLAYING, "stop while PLAYING: nothing");
	xd_mictest_resume();
	check(st() == XD_MT_PLAYING, "resume while PLAYING: nothing");
	xd_mictest_cancel("test");
	passes(10, 1, 1);
	check(idle_ok(), "IDLE, freed");
	xd_mictest_pause();
	xd_mictest_stop();
	check(idle_ok(), "pause / stop when IDLE: nothing");
}

static void t_xbdm(void)
{
	u32 c0 = g_xd_mt.cancels;

	g_xd_mt.cmd_pad = 0;
	g_xd_mt.cmd = MT_CMD_START;
	pass(1, 1);
	check(g_xd_mt.cmd == 0 && st() == XD_MT_RECORDING, "cmd 1: start");
	passes(700, 1, 1);
	g_xd_mt.cmd = MT_CMD_STOP;
	pass(1, 1);
	check(st() == XD_MT_PLAYING, "cmd 3: stop and play");
	g_xd_mt.cmd = MT_CMD_PAUSE;
	pass(1, 1);
	check(st() == XD_MT_PAUSED, "cmd 5: pause");
	g_xd_mt.cmd = MT_CMD_RESUME;
	pass(1, 1);
	check(st() == XD_MT_PLAYING, "cmd 6: resume");
	play_out(2000);
	g_xd_mt.cmd = MT_CMD_PLAY;
	pass(1, 1);
	check(st() == XD_MT_PLAYING, "cmd 4: play again");
	play_out(2000);
	g_xd_mt.cmd = MT_CMD_CLOSED;
	pass(1, 1);
	check(idle_ok(), "cmd 7: the panel closed (READY: IDLE, freed)");
	g_xd_mt.cmd = MT_CMD_START;
	pass(1, 1);
	g_xd_mt.cmd = MT_CMD_CANCEL;
	pass(1, 1);
	check(idle_ok() && g_xd_mt.cancels == c0 + 1, "cmd 2: cancel");
	g_xd_mt.cmd = MT_CMD_START_SMALL;
	pass(1, 1);
#ifdef XW_DEV
	check(st() == XD_MT_RECORDING && g_mt.cap == MT_SMALL, "cmd 8 (XW_DEV): the 10 s buffer");
	xd_mictest_cancel("test");
	g_xd_mt.cmd = MT_CMD_START_NOMEM;
	pass(1, 1);
	check(idle_ok() && g_xd_mt.last_refusal == XD_MT_NO_MEMORY, "cmd 9 (XW_DEV): NO_MEMORY");
#else
	check(idle_ok() && g_xd_mt.cmd == 0, "cmd 8 (release): ignored");
#endif
	passes(5, 1, 1);

	/* version 3: held recordings (never played) and keep */
	{
		u32 plays = g_xd_mt.plays, pc = g_xd_mt.panel_closes;
		int heard = 0;

		g_xd_mt.cmd = MT_CMD_START_HOLD;
		pass(1, 1);
		passes(700, 1, 1);
		g_xd_mt.cmd = MT_CMD_STOP_HOLD;
		g_capture = 1;
		g_out_n = 0;
		passes(50, 1, 1);
		for (int i = 0; i < g_out_n; i++)
			heard |= g_out[i] != 0;
		g_capture = 0;
		check(st() == XD_MT_READY && g_xd_mt.plays == plays && !heard && g_mt.n > 0 &&
		      g_xd_mt.buf_addr, "cmd 11: stop, held READY, nothing played into the ear");
		g_xd_mt.keep = 60;
		pass(1, 1);
		g_xd_mt.cmd = MT_CMD_CLOSED;
		pass(1, 1);
		check(st() == XD_MT_READY && g_xd_mt.kept == 1 && g_xd_mt.panel_closes == pc &&
		      g_xd_mt.keep == 0, "keep 60: the panel closing leaves the clip (kept 1)");
		g_ms += 61000;
		g_xd_mt.cmd = MT_CMD_CLOSED;
		pass(1, 1);
		check(idle_ok() && g_xd_mt.panel_closes == pc + 1,
		      "61 s later the panel closing discards it as before");
		g_xd_mt.rec_ms = 1000;
		g_xd_mt.cmd = MT_CMD_START_HOLD;
		pass(1, 1);
		g_capture = 1;
		g_out_n = 0;
		passes(1100, 1, 1);
		heard = 0;
		for (int i = 0; i < g_out_n; i++)
			heard |= g_out[i] != 0;
		g_capture = 0;
		check(st() == XD_MT_READY && g_xd_mt.plays == plays && !heard &&
		      g_mt.n == MT_PER_MS * 1000, "cmd 10: start held, the auto-stop keeps it READY "
		      "unplayed (1000 ms)");
		g_xd_mt.cmd = MT_CMD_CANCEL;
		pass(1, 1);
		check(idle_ok() && g_mt.keep_until == 0, "cancel: IDLE, freed");
		g_xd_mt.rec_ms = XD_MT_MAX_MS;
		g_xd_mt.cmd = MT_CMD_START;
		pass(1, 1);
		passes(700, 1, 1);
		g_xd_mt.cmd = MT_CMD_STOP;
		pass(1, 1);
		check(st() == XD_MT_PLAYING && g_xd_mt.plays == plays + 1,
		      "a plain start after a held one plays as before");
		xd_mictest_cancel("test");
		passes(10, 1, 1);
	}
}

/* version 3: the clip is the mic before MIC VOLUME and the gate; the playback applies the
 * ones in effect, with the live path's steps, and follows a change at once */
static void t_gate_playback(void)
{
	int raw = 1, changed = 0, quiet_after = 1, before = 0;

	/* recorded with the gate shut tight and the volume at 200 %: the clip is raw anyway */
	g_thr = 30000;
	g_gain = 2 * XD_Q12;
	record(600);
	for (u32 i = 1; i + 1 < g_mt.n - MT_PER_MS * MT_FADE_MS; i++) /* rs_rec: 1 sample late */
		raw &= g_mt.buf[i] == (i & 1 ? g_mic_level : -g_mic_level) ||
		       g_mt.buf[i] == (i & 1 ? -g_mic_level : g_mic_level);
	check(raw, "the clip is the mic before MIC VOLUME and the gate");
	xd_mictest_cancel("test");
	passes(20, 1, 1);

	/* words and pauses, played at step 2: exactly the live steps' result */
	g_thr = 58; /* step 2 */
	g_gain = XD_Q12;
	record(1500);
	for (u32 i = 0; i < g_mt.n; i++) {
		u32 ms = i / MT_PER_MS;
		s32 a = ms % 500 < 250 ? 2000 : 30; /* 250 ms "words" at -24 dBFS, pauses at -61 */

		g_mt.buf[i] = (s16)(i & 1 ? a : -a);
	}
	g_out_n = 0;
	g_capture = 1;
	play_out(4000);
	g_capture = 0;
	check(compare_playback(NULL, 0, "gated"), "played through the gate (step 2) exactly as the "
	      "live path's steps give it: words pass, the pauses go after the hold");

	/* a step change while it plays is heard at once: from OFF to a threshold above the clip */
	g_thr = 0;
	xd_mictest_play();
	g_out_n = 0;
	g_capture = 1;
	passes(300, 1, 1);
	for (int i = 0; i < g_out_n; i++)
		before += g_out[i] != 0;
	changed = g_out_n;
	g_thr = 30000;
	passes(100, 1, 1);
	for (int i = changed + 48 * 35; i < g_out_n; i++)
		quiet_after &= g_out[i] == 0;
	g_capture = 0;
	check(before > 48 * 100 && quiet_after, /* (+a, -a at 24 kHz: every other 48 kHz frame 0) */
	      "NOISE GATE changed mid-playback: shut within 35 ms "
	      "(the next window's end, then the 10 ms close), the playback goes on");
	check(st() == XD_MT_PLAYING, "  still PLAYING");
	g_thr = 0;
	xd_mictest_cancel("test");
	passes(20, 1, 1);
}

static void t_levels(void)
{
	/* a full-scale clip on a full-scale earpiece with master > 1: never above 1 (the clip
	 * plays through the 360's limiter, -1 dBFS, then the master at most 1) */
	record(800);
	for (u32 i = 0; i < g_mt.n; i++)
		g_mt.buf[i] = 32767;
	g_master = 400;
	play_out(2000);
	g_master = 256;
	snprintf(g_what, sizeof(g_what), "master above 1 is taken as 1: peak %u, the limiter's "
		 "-1 dBFS (%u)", g_xd_mt.play_peak, XD_LIMIT);
	check(g_xd_mt.play_peak < XD_LIMIT + 32 && g_xd_mt.play_peak > XD_LIMIT - 32, g_what);
	xd_mictest_quiesce();
	check(idle_ok() && g_xd_mt.duck_now == 256 && g_duck == 256, "quiesce: IDLE, freed, no duck");
}

int main(int argc, char **argv)
{
	g_verbose = argc > 1 && !strcmp(argv[1], "-v");
	g_out = calloc(MAX_OUT, sizeof(s16));
	pad_up(0);
	pad_up(1);
	passes(5, 1, 1);
	t_defaults();
	t_record_play_ready();
	t_continuity();
	t_cancels();
	t_panel_closed();
	t_autostop();
	t_alloc();
	t_mute();
	t_pad_gone();
	t_stalls();
	t_refusals();
	t_xbdm();
	t_gate_playback();
	t_levels();
	reset_all();
	check(g_live == 0 && g_allocs == g_frees, "every allocation freed");
	printf("%s (%u allocations, %u frees)\n", g_fail ? "FAILED" : "all passed", g_allocs,
	       g_frees);
	free(g_out);
	return g_fail;
}
