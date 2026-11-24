// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of the mic path as built: the real xbox360/micdsp_360.c (XV_HOST_TEST), fed the
 * way voice_360.c's xv_mic_samples feeds it (mic_clean, mic_analyse, then mic_shape + mic_to_360, or
 * silence through mic_to_360 while muted), with the capture side (mic_feed's rs_push,
 * xv_cap_feed) hashed instead of packed.
 *
 *   cc -O2 -Wall -Itests/host -o test_micdsp360 tests/test_micdsp360.c && ./test_micdsp360
 *   (and with -DXW_DEV: adds the comb canceller's scenario)
 *
 * 1. MIC-AUTO as wired is bit-exact with the integer model: every scenario of
 *    tools/micdsp/vectors.h (gate AUTO, MIC VOLUME AUTO from 1 s, the decimator, muting)
 *    gives the model's per-window trace and 8 kHz output.
 * 2. The manual settings (gate OFF / LOW / HIGH, a stored gate 3 with NOISE GATE AUTO
 *    switched off, MIC VOLUME 1..10, decimator off) give the same output and the same XWDG
 *    numbers whatever the new switches say. And the same as the code before MIC-AUTO, built
 *    from git (its own headers too):
 *
 *   R=$(mktemp -d) && git archive e1a2220 xbox360 core | tar -x -C $R
 *   cc -O2 -Wall -w -Itests/host -DMICDSP_SRC="\"$R/xbox360/micdsp_360.c\"" -o $R/ref \
 *      tests/test_micdsp360.c && $R/ref --manual > $R/ref.txt
 *   ./test_micdsp360 --manual | diff $R/ref.txt - && echo identical
 *
 * 3. The switches: allocation only while one is on, the offered getters, quiesce / resume,
 *    the decimator switched on and off mid-stream, and its level against rs_push.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XV_HOST_TEST 1
/* kernel_360.h keeps 32-bit kernel addresses in u32: fine on the console, noisy on a 64-bit host */
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#ifndef MICDSP_SRC
#define MICDSP_SRC "../xbox360/micdsp_360.c"
#endif
/* the plugin's allocations, made to fail on demand (MIC-AUTO's state) */
static int g_malloc_fail;
static void *test_malloc(size_t n)
{
	return g_malloc_fail ? NULL : malloc(n);
}
#define malloc(n) test_malloc(n)
#include MICDSP_SRC
#undef malloc
#include "../tools/micdsp/vectors.h"
#include "gate_take.h"

#ifdef XD_GATE_STEPS
/* the gate's four steps: 1 2 AUTO 3 4 by stored number; AUTO not in effect runs step 2 */
#define GT_FALLBACK XD_GATE_FALLBACK
static const char *const GT_NAMES[XD_GATES] = { "OFF ", "1   ", "2   ", "AUTO", "3   ", "4   " };
/* the settings that must keep this talker's sentences whole (3 and 4 are stricter on purpose) */
#define GT_MUST(g) ((g) == XD_GATE_1 || (g) == XD_GATE_2 || (g) == XD_GATE_AUTO)
#else /* a micdsp_360.c from before the steps (--gate against an older build, see the top) */
#define GT_FALLBACK 1
static const char *const GT_NAMES[XD_GATES] = { "OFF ", "LOW ", "HIGH", "AUTO" };
#define GT_MUST(g) ((g) == 1 || (g) == XD_GATE_AUTO)
#endif

#ifdef MA_FIR_TAPS
#define NEW_CODE 1 /* micdsp_360.c with MIC-AUTO (the reference build has none of it) */
#endif

/* ---- what micdsp_360.c needs from the other files ----------------------------------------- */
struct xv_pad g_pad[PADS];
u32 g_quiesced;
u64 g_voice_ms[PADS], g_ear_loud_ms[PADS];
u8 g_mic_mute[PADS];
static u64 g_now = 1000;
static int g_verbose;

u64 xa_now_ms(void) { return g_now; }
void xa_sleep_ms(unsigned int ms) { g_now += ms; }
void xa_log(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
u32 xg_sidetone(void) { return 5; }
u32 xg_master(void) { return 100; }
bool xg_set_buffer(u32 ms) { (void)ms; return true; }
u32 xg_buffer_target(void) { return 2160; }
u32 xg_buffer_high(void) { return 4320; }
u32 xg_buffer_drift_full(void) { return 480; }
bool xd_mictest_enabled(void) { return false; }
void mt_poll(void) {}
void mt_pad_gone(int pad, const char *why) { (void)pad, (void)why; }

static u32 g_windows;
void st_dev_watch(int pad, u32 rms, u32 win_ms) { (void)pad, (void)rms, (void)win_ms; g_windows++; }

static u32 fnv(u32 h, u32 v)
{
	for (int k = 0; k < 4; k++) {
		h ^= (v >> (8 * k)) & 0xff;
		h *= 0x01000193u;
	}
	return h;
}

/* the 8 kHz capture stream, whichever way it went */
static u32 g_out_hash, g_out_n;
static u64 g_out_sq; /* sum of squares, for the level comparison */

static void out_sample(s16 y)
{
	g_out_hash = fnv(g_out_hash, (u32)(s32)y);
	g_out_n++;
	g_out_sq += (u64)((s32)y * (s32)y);
}

/* voice_360.c's mic_feed, its cap_write replaced by the hash */
void mic_feed(int i, struct rs *r, const u8 *d, int frames, u32 ch)
{
	(void)i;
	for (int f = 0; f < frames; f++, d += 2 * ch) {
		s32 x = (s16)get_le16(d);
		s16 out[16];
		int k;

		if (ch == 2)
			x = (x + (s16)get_le16(d + 2)) >> 1;
		k = rs_push(r, x, out);
		for (int j = 0; j < k; j++)
			out_sample(out[j]);
	}
}

void xv_cap_feed(int pad, const s16 *s, int n)
{
	(void)pad;
	for (int j = 0; j < n; j++)
		out_sample(s[j]);
}

/* ---- the harness ----------------------------------------------------------------------------- */
static const u8 g_zero[2 * XD_CHUNK]; /* voice_360.c's g_silence */
static u32 g_clean_hash, g_xd_hash;

/* a 24 kHz mono headset on pad 0, as voice_360.c sets one up (reset_pad_dsp, hs_format) */
static void headset(void)
{
	memset(&g_pad[0], 0, sizeof(g_pad[0]));
	g_pad[0].usb_present = 1;
	g_pad[0].rate_in = 24000;
	g_pad[0].ch_in = 1;
	g_pad[0].rate_out = 48000;
	g_pad[0].ch_out = 2;
	rs_init(&g_pad[0].rs_mic, 24000, 8000);
	xd_reset_pad(0);
}

/* xv_mic_samples for one chunk of raw mono samples; true if a 20 ms window ended in it */
static bool chunk(const s16 *x, int n)
{
	u8 raw[2 * XD_CHUNK], mono[2 * XD_CHUNK];
	u32 w = g_windows;

	for (int i = 0; i < n; i++)
		put_le16(raw + 2 * i, (u16)x[i]);
	mic_clean(0, raw, n, 1, mono);
	mic_analyse(0, mono, n);
	for (int i = 0; i < n; i++)
		g_clean_hash = fnv(g_clean_hash, get_le16(mono + 2 * i));
	if (g_mic_mute[0]) {
		mic_to_360(0, g_zero, n);
	} else {
		u8 shaped[2 * XD_CHUNK];

		mic_shape(0, mono, n, shaped);
		mic_to_360(0, shaped, n);
	}
	if (w == g_windows)
		return false;
	/* what XWDG shows of the mic after the window */
	{
		const u32 v[] = { g_xd.gate_rms, g_xd.gate_threshold, g_xd.gate_open, g_xd.gate_opens,
				  g_xd.mic_vu, g_xd.mic_gain_q15, g_xd.mic_dc_now };

		for (u32 k = 0; k < sizeof(v) / sizeof(v[0]); k++)
			g_xd_hash = fnv(g_xd_hash, v[k]);
	}
	return true;
}

/* vectors.py's input: noise and a buzz, in level segments (as tests/test_micdsp.c) */
static s16 *gen(void)
{
	s16 *x = malloc(MV_SAMPLES * sizeof(s16));
	u32 s = MV_SEED, i = 0;

	for (u32 g = 0; g < sizeof(mv_segs) / sizeof(mv_segs[0]); g++) {
		u32 n = mv_segs[g][0] * 24, a0 = mv_segs[g][1], a1 = mv_segs[g][2];

		for (u32 j = 0; j < n; j++, i++) {
			s32 r, amp, saw;

			s = s * 1664525u + 1013904223u;
			r = (s32)(s >> 16) - 32768;
			amp = (s32)((a0 * (n - j) + a1 * j) / n);
			saw = (s32)(i % 160) * 409 - 32720;
			x[i] = clamp16(((r * amp) >> 16) + ((saw * amp) >> 16) + 60);
		}
	}
	return x;
}

struct run {
	u32 gate, level;  /* g_xd.gate; MIC VOLUME 1..10 (manual) */
	u32 mute;         /* muted from MV_MUTE_FROM to MV_MUTE_TO ms */
	u32 auto_at;      /* MIC VOLUME AUTO chosen at this ms (0: never) */
#ifdef NEW_CODE
	u32 gate_auto, mic_auto, fir, comb, auto_on;
	s32 (*trace)[7];  /* the model's per-window row, if wanted */
	u32 trace_hash;
#endif
};

static int g_fail;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	g_fail |= !ok;
}

/* one run of the whole input from a fresh headset; leaves the switches off and freed */
static void run(struct run *r, const s16 *x, u32 samples)
{
	u32 win = 0;

	g_xd.gate = r->gate;
	g_xd.mic_gain = XD_MIC_GAINS[r->level - 1];
	g_xd.gate_opens = 0;
	g_mic_mute[0] = 0;
#ifdef NEW_CODE
	g_xma.gate_auto = r->gate_auto;
	g_xma.mic_auto = r->mic_auto;
	g_xma.fir = r->fir;
	g_xma.comb = r->comb;
	g_xma.auto_on = r->auto_on;
#endif
	xd_poll();
	headset();
	g_out_hash = g_clean_hash = g_xd_hash = 0x811C9DC5u;
	g_out_n = 0;
	g_out_sq = 0;
	g_windows = 0;
	for (u32 c = 0; c + 24 <= samples; c += 24) {
		u32 ms = c / 24;

		if (r->auto_at && ms == r->auto_at)
			xd_set_mic_auto(true);
		g_mic_mute[0] = r->mute && ms >= MV_MUTE_FROM && ms < MV_MUTE_TO;
		if (!chunk(x + c, 24))
			continue;
#ifdef NEW_CODE
		if (g_ma) {
			const struct ma_pad *s = &g_ma->pad[0];
			s32 row[7] = { (s32)g_xd.gate_rms, (s32)g_xd.gate_threshold,
				       (s32)g_xd.gate_open, s->floor.floor_db, s->floor.thr_db,
				       (s32)s->agc.q12, s->agc.speech_db };

			for (int k = 0; k < 7; k++)
				r->trace_hash = fnv(r->trace_hash, (u32)row[k]);
			if (r->trace && win < mv_scen[0].windows)
				memcpy(r->trace[win], row, sizeof(row));
		}
#endif
		win++;
		if (win % 16 == 0)
			xd_poll(); /* the driver loop's pass: must change nothing */
	}
#ifdef NEW_CODE
	g_xma.gate_auto = g_xma.mic_auto = g_xma.fir = g_xma.comb = g_xma.auto_on = 0;
#endif
	xd_set_mic_auto(false);
	xd_poll();
}

/* ---- 2: the manual settings --------------------------------------------------------------- */
static void manual(const s16 *x, bool print_only)
{
	int same = 1;

	for (u32 gate = XD_GATE_OFF; gate < XD_GATES; gate++) {
		for (u32 level = 1; level <= 10; level++) {
			struct run r = { .gate = gate, .level = level, .mute = 1 };
			u32 out, n, clean, xdh;

			run(&r, x, MV_SAMPLES);
			out = g_out_hash, n = g_out_n, clean = g_clean_hash, xdh = g_xd_hash;
#ifdef NEW_CODE
			/* the new switches, set every way that must not touch a manual setting */
			for (int v = 0; v < 3; v++) {
				struct run q = r;

				q.mic_auto = v == 0;         /* offered, not chosen */
				q.gate_auto = v == 0 && gate != XD_GATE_AUTO;
				q.auto_on = v >= 1;          /* chosen, not offered */
				q.trace_hash = 0x811C9DC5u;
				if (v == 2)
					q.auto_at = 1000;    /* ... and chosen mid-run */
				run(&q, x, MV_SAMPLES);
				if (g_out_hash != out || g_out_n != n || g_clean_hash != clean ||
				    g_xd_hash != xdh) {
					printf("FAIL gate %u level %u: switches variant %d changed it\n",
					       gate, level, v);
					same = 0;
				}
			}
#endif
			printf("%sgate %u level %2u: %u samples at 8 kHz, out %08x, mic %08x, "
			       "XWDG %08x\n", print_only ? "" : "     ", gate, level, n, out, clean, xdh);
		}
	}
	g_fail |= !same;
	if (!print_only)
		check(same, "manual settings: same output and XWDG numbers whatever the switches say "
		      "(compare with the code before MIC-AUTO: --manual, see the top)");
}

#ifdef NEW_CODE
/* ---- 1: against the model ----------------------------------------------------------------- */
static void model(const s16 *x)
{
	for (u32 i = 0; i < sizeof(mv_scen) / sizeof(mv_scen[0]); i++) {
		const struct mv_scen *sc = &mv_scen[i];
		struct run r = { .gate = sc->gate, .level = sc->manual, .mute = sc->mute,
				 .auto_at = sc->level == 0 ? MV_AUTO_AT : 0, .gate_auto = 1,
				 .mic_auto = 1, .fir = sc->fir, .comb = sc->comb,
				 .trace_hash = 0x811C9DC5u };
		char what[160];
		int ok;

#ifndef XW_DEV
		if (sc->comb) {
			printf("skip scenario %-12s (the comb canceller: build with -DXW_DEV)\n",
			       sc->name);
			continue;
		}
#endif
		if (i == 0)
			r.trace = calloc(sc->windows, sizeof(*r.trace));
		run(&r, x, MV_SAMPLES);
		ok = g_windows == sc->windows && r.trace_hash == sc->trace_hash &&
		     g_out_n == sc->out_n && g_out_hash == sc->out_hash;
		if (r.trace) {
			for (u32 w = 0; w < sc->windows; w++)
				if (memcmp(r.trace[w], mv_trace0[w], sizeof(mv_trace0[w]))) {
					printf("     window %u differs (rms %d thr %d open %d floor %d "
					       "thr_db %d q12 %d speech %d; model %d %d %d %d %d %d %d)\n",
					       w, r.trace[w][0], r.trace[w][1], r.trace[w][2],
					       r.trace[w][3], r.trace[w][4], r.trace[w][5],
					       r.trace[w][6], mv_trace0[w][0], mv_trace0[w][1],
					       mv_trace0[w][2], mv_trace0[w][3], mv_trace0[w][4],
					       mv_trace0[w][5], mv_trace0[w][6]);
					ok = 0;
					break;
				}
			free(r.trace);
		}
		snprintf(what, sizeof(what), "scenario %-12s bit-exact with the model (%u windows, "
			 "%u samples at 8 kHz)", sc->name, g_windows, g_out_n);
		check(ok, what);
	}
}

/* ---- 3: the switches ---------------------------------------------------------------------- */
static void switches(const s16 *x)
{
	char what[200];
	int ok;

	/* nothing allocated, nothing offered, gate 3 = LOW, AUTO chosen but not in effect */
	headset();
	xd_set_mic_auto(true);
	g_xd.mic_gain = 100;
	g_xd.gate = XD_GATE_AUTO;
	xd_poll();
	for (int c = 0; c < 40; c++)
		chunk(x + 24 * c, 24);
	ok = !g_ma && !g_xma.heap_bytes && !xd_gate_auto_offered() && !xd_mic_auto_offered() &&
	     !xd_mic_auto() && g_xd.gate_threshold == XD_GATE_RMS[GT_FALLBACK] &&
	     mic_gain_target(0) == 4096 && xd_mic_auto_level() == 5;
	snprintf(what, sizeof(what), "switches off: no memory, nothing offered, gate 3 runs step 2 (%u), "
		 "AUTO chosen but not in effect", XD_GATE_RMS[GT_FALLBACK]);
	check(ok, what);

	/* NOISE GATE AUTO switched on with no memory for it: LOW, not offered, counted, and it
	 * comes into effect on the first pass that gets the memory */
	g_malloc_fail = 1;
	g_xma.gate_auto = 1;
	xd_poll();
	for (int c = 0; c < 40; c++)
		chunk(x + 24 * c, 24);
	ok = !g_ma && !xd_gate_auto_offered() && g_xma.nomem > 0 &&
	     g_xd.gate_threshold == XD_GATE_RMS[GT_FALLBACK];
	g_malloc_fail = 0;
	xd_poll();
	for (int c = 0; c < 40; c++)
		chunk(x + 24 * c, 24);
	snprintf(what, sizeof(what), "gate_auto on, allocation failing: gate 3 runs step 2 (%u), AUTO "
		 "hidden, nomem %u; next pass with memory: AUTO in effect", XD_GATE_RMS[GT_FALLBACK],
		 g_xma.nomem);
	check(ok && g_ma && xd_gate_auto_offered(), what);
	g_xma.gate_auto = 0;
	g_xma.nomem = 0;
	xd_poll();

	/* offered: allocated (within budget), AUTO in effect from the manual step, no jump */
	g_xd.mic_gain = 200;
	g_xma.gate_auto = g_xma.mic_auto = 1;
	xd_poll();
	snprintf(what, sizeof(what), "switches on: offered, AUTO starts at the manual step (level %u, "
		 "q12 %u), %u bytes allocated", xd_mic_auto_level(), mic_gain_target(0),
		 g_xma.heap_bytes);
	ok = g_ma && xd_gate_auto_offered() && xd_mic_auto_offered() && xd_mic_auto() &&
	     mic_gain_target(0) == 200 * 4096 / 100 && xd_mic_auto_level() == 7 &&
	     mic_gain_target(-1) == mic_gain_target(0) && g_xma.pad == 0 &&
	     g_xma.heap_bytes > 0 && g_xma.heap_bytes <= 4096;
	check(ok, what);
	g_xd.mic_gain = 100;

	/* hot reload: quiesce frees and nothing comes back; resume allocates again */
	g_quiesced = 1;
	xd_reset_pad(0);
	xd_poll();
	ok = !g_ma && !g_xma.heap_bytes && !xd_mic_auto() && !xd_gate_auto_offered() &&
	     !g_xma.nomem;
	g_quiesced = 0;
	xd_poll();
	check(ok && g_ma && xd_mic_auto(), "quiesce frees everything and holds; resume comes back");
	g_xma.gate_auto = g_xma.mic_auto = g_xma.auto_on = 0;
	xd_poll();
	check(!g_ma && !g_xma.heap_bytes, "all switches off: memory freed");

	/* every manual step, as the panel shows it */
	ok = 1;
	for (u32 l = 1; l <= 10; l++) {
		g_xd.mic_gain = XD_MIC_GAINS[l - 1];
		xd_poll();
		ok &= xd_mic_auto_level() == l && xd_mic_level(g_xd.applied_mic_gain) == l;
	}
	check(ok, "xd_mic_auto_level is the manual step 1..10 while AUTO is off");
	g_xd.mic_gain = 100;

	/* the decimator switched on and off every 0.5 s: the stream never loses its pace */
	{
		u32 c, flips = 0, made = g_xma.fir_samples, bypass = g_xma.fir_bypass;

		g_xd.gate = XD_GATE_OFF;
		xd_poll();
		headset();
		g_out_n = 0;
		for (c = 0; c + 24 <= MV_SAMPLES; c += 24) {
			if (c % 12000 == 0) {
				g_xma.fir = !g_xma.fir;
				xd_poll();
				flips++;
			}
			chunk(x + c, 24);
		}
		snprintf(what, sizeof(what), "decimator switched %u times mid-stream: %u samples at "
			 "8 kHz for %u in (%u by the decimator, %u chunks bypassed)", flips, g_out_n,
			 c, g_xma.fir_samples - made, g_xma.fir_bypass - bypass);
		ok = g_out_n == c / 3 && g_xma.fir_samples - made > c / 3 / 3 &&
		     g_xma.fir_bypass == bypass;
		check(ok, what);
		g_xma.fir = 0;
		xd_poll();
	}

	/* the decimator at another rate (16 kHz headset): rs_push carries on, counted */
	{
		u32 before = g_xma.fir_bypass;

		g_xma.fir = 1;
		xd_poll();
		headset();
		g_pad[0].rate_in = 16000;
		rs_init(&g_pad[0].rs_mic, 16000, 8000);
		g_out_n = 0;
		for (int c = 0; c < 100; c++)
			chunk(x + 16 * c, 16);
		check(g_xma.fir_bypass > before && g_out_n == 800,
		      "decimator at 16 kHz: left to rs_push (fir_bypass counts), pace unchanged");
		g_xma.fir = 0;
		xd_poll();
	}
}

/* the decimator's level against rs_push: a -12 dBFS sine through the whole path (gate OFF,
 * MIC VOLUME 5), output power in Q8 dB, at frequencies across the voice band */
static void level(void)
{
	static const u32 hz[] = { 100, 300, 500, 1000, 1500, 2000, 2500, 3000, 3200, 3400 };
	enum { N = 24000 };
	s16 *x = malloc(N * sizeof(s16));
	int ok = 1;

	printf("     level, decimator vs rs_push (dB):");
	for (u32 i = 0; i < sizeof(hz) / sizeof(hz[0]); i++) {
		s32 db[2];

		for (int k = 0; k < N; k++)
			x[k] = (s16)(isin((u32)((u64)hz[i] * k * 4294967296ull / 24000)) / 4);
		for (int f = 0; f < 2; f++) {
			struct run r = { .gate = XD_GATE_OFF, .level = 5, .fir = (u32)f };

			run(&r, x, N);
			db[f] = ma_db_ms((u32)(g_out_sq / g_out_n));
		}
		{
			s32 c = (db[1] - db[0]) * 100 / MA_DB; /* hundredths of a dB */

			printf(" %u Hz %s%d.%02d", hz[i], c < 0 ? "-" : "+", abs(c) / 100, abs(c) % 100);
		}
		if (hz[i] <= 1000)
			ok &= db[1] - db[0] <= MA_DB / 2 && db[1] - db[0] >= -MA_DB / 2;
	}
	printf("\n");
	check(ok, "decimator level within 0.5 dB of rs_push up to 1 kHz (above, rs_push droops)");
	free(x);
}
#endif

/* ---- 4: the gate on the owner's reading (tests/gate_take.h) ----------------------------------
 * A signal with exactly the take's 20 ms window levels (constant within a window: A or A + 1,
 * so sum x^2 / 480 is the stored mean square) through the real mic path, DC removal off (the
 * take is after it already). Speech: windows at -58 dBFS or more (16 dB over the room's
 * floor); an utterance: speech windows less than 300 ms apart. Measured per 1 ms chunk: the
 * gate's state (xd_gate_open) and the energy that reached the 8 kHz output. */
#define GT_SPEECH_RMS 41 /* -58 dBFS */
#define GT_GAP_WIN 14    /* < 300 ms between speech windows: the same utterance */
#define GT_CHUNKS (GT_WINDOWS * 20)

struct gate_result {
	u32 speech, lost, closures, words;
	u32 start20, start40; /* mean share of a word's first 20 / 40 ms lost, per cent */
	u32 open_outside_ms, shut_ms;
};

static void gate_take(u32 gate, u32 gate_auto, struct gate_result *res)
{
	static u8 open[GT_CHUNKS], utt[GT_WINDOWS], sp[GT_WINDOWS];
	static u64 out_e[GT_CHUNKS];
	s16 buf[24];
	u32 last = 0, s20 = 0, s40 = 0;

	(void)gate_auto;
	memset(res, 0, sizeof(*res));
	g_xd.gate = gate;
	g_xd.mic_gain = 100;
	g_xd.mic_dc = 0;
	g_mic_mute[0] = 0;
#ifdef NEW_CODE
	g_xma.gate_auto = gate_auto;
	g_xma.mic_auto = g_xma.fir = g_xma.comb = g_xma.auto_on = 0;
#endif
	headset();
	xd_poll();
	for (u32 w = 0; w < GT_WINDOWS; w++) {
		u32 ms = gt_ms[w], a = isqrt(ms);
		u32 k = (u32)(((u64)(ms - a * a) * 480 + a) / (2 * a + 1)); /* samples at a + 1 */

		sp[w] = a >= GT_SPEECH_RMS;
		res->speech += sp[w];
		for (u32 c = 0; c < 20; c++) {
			for (u32 i = 0; i < 24; i++)
				buf[i] = (s16)(c * 24 + i < k ? a + 1 : a);
			g_out_sq = 0;
			chunk(buf, 24);
			out_e[w * 20 + c] = g_out_sq;
			open[w * 20 + c] = xd_gate_open(0);
			if (w * 20 + c && (w * 20 + c) % 320 == 0)
				xd_poll();
		}
	}
	/* utterances */
	for (u32 w = 0; w < GT_WINDOWS; w++) {
		if (!sp[w])
			continue;
		if (last && w - last <= GT_GAP_WIN + 1)
			for (u32 v = last; v <= w; v++)
				utt[v] = 1;
		utt[w] = 1;
		last = w;
	}
	for (u32 w = 0; w < GT_WINDOWS; w++) {
		u64 e = 0, in = (u64)gt_ms[w] * 160; /* 8 kHz samples a window, gain 1 */

		for (u32 c = 0; c < 20; c++) {
			e += out_e[w * 20 + c];
			if (!utt[w] && open[w * 20 + c])
				res->open_outside_ms++;
			if (!open[w * 20 + c])
				res->shut_ms++;
		}
		if (sp[w] && 2 * e < in)
			res->lost++;
		/* a word: speech after >= 100 ms without; its first 20 / 40 ms (window resolution) */
		if (sp[w] && w >= 5 && !sp[w - 1] && !sp[w - 2] && !sp[w - 3] && !sp[w - 4] &&
		    !sp[w - 5] && w + 1 < GT_WINDOWS) {
			u64 e20 = 0, e40;

			for (u32 c = 0; c < 20; c++)
				e20 += out_e[w * 20 + c];
			e40 = e20;
			for (u32 c = 20; c < 40; c++)
				e40 += out_e[w * 20 + c];
			s20 += 100 - (u32)(e20 * 100 / ((u64)gt_ms[w] * 160));
			s40 += 100 - (u32)(e40 * 100 / (((u64)gt_ms[w] + gt_ms[w + 1]) * 160));
			res->words++;
		}
	}
	/* closures inside an utterance: the gate shuts and opens again before it ends */
	for (u32 c = 1; c < GT_CHUNKS; c++) {
		u32 w = c / 20, end = w;

		if (!utt[w] || !open[c - 1] || open[c])
			continue;
		while (end + 1 < GT_WINDOWS && utt[end + 1])
			end++;
		for (u32 d = c; d < (end + 1) * 20; d++)
			if (open[d]) {
				res->closures++;
				break;
			}
	}
	if (res->words) {
		res->start20 = s20 / res->words;
		res->start40 = s40 / res->words;
	}
	g_xd.mic_dc = 1;
#ifdef NEW_CODE
	g_xma.gate_auto = 0;
#endif
	xd_poll();
}

#ifdef XD_GATE_STEPS
/* the mic test's playback order (mictest_360.c mt_gate_chunk) on voice_priv.h's gate steps,
 * against the model's micdsp.playback (what voice.py mictest save --gated computes) */
static void playback_vector(const s16 *x)
{
	struct xd_mic m;
	struct xd_shape sh;
	u32 h = 0x811C9DC5u, thr = 58, gain = 8192, win = gate_win(24000);

	memset(&m, 0, sizeof(m));
	memset(&sh, 0, sizeof(sh));
	m.closed = 1;
	sh.shut = 32768;
	sh.gain = gain;
	for (u32 c = 0; c < 72000; c += 24) {
		u32 shut_to;

		gate_chunk_start(&m, thr);
		for (u32 i = 0; i < 24; i++)
			if (gate_push(&m, x[c + i], win))
				gate_window_end(&m, isqrt((u32)(m.sum / m.n)), thr, 24000);
		gate_chunk_end(&m, thr, win);
		shut_to = gate_shut(&m) ? 32768 : 0;
		for (u32 i = 0; i < 24; i++)
			h = fnv(h, (u32)(s32)shape(&sh, x[c + i], gain, shut_to, 24000));
	}
	check(h == MV_PLAY_HASH, "mic test playback order on the gate's steps: bit-exact with the "
	      "model's playback (voice.py mictest save --gated)");
}
#endif

static void gate_regression(void)
{
	static const char *const *names = GT_NAMES;
	char what[220];

	for (u32 gate = XD_GATE_OFF; gate < XD_GATES; gate++) {
		struct gate_result r;

		gate_take(gate, gate == XD_GATE_AUTO, &r);
		snprintf(what, sizeof(what), "gate %s on the reading: speech lost %u of %u windows "
			 "(%u.%u %%), %u closures inside utterances; word starts lost %u %% of the first "
			 "20 ms, %u %% of 40 ms (%u words); open outside speech %u ms, shut %u ms",
			 names[gate], r.lost, r.speech, r.lost * 1000 / r.speech / 10,
			 r.lost * 1000 / r.speech % 10, r.closures, r.start20, r.start40, r.words,
			 r.open_outside_ms, r.shut_ms);
		if (GT_MUST(gate))
			check(r.closures == 0 && r.lost * 100 <= r.speech, what);
		else
			printf("     %s\n", what);
	}
}

int main(int argc, char **argv)
{
	s16 *x = gen();
	bool manual_only = argc > 1 && !strcmp(argv[1], "--manual");

	g_verbose = argc > 1 && !strcmp(argv[1], "-v");
	if (manual_only) {
		manual(x, true);
		free(x);
		return g_fail;
	}
	if (argc > 1 && !strcmp(argv[1], "--gate")) {
		/* also builds against an older micdsp_360.c (see the top) */
		gate_regression();
		free(x);
		printf(g_fail ? "FAILED\n" : "all passed\n");
		return g_fail;
	}
#ifdef NEW_CODE
	{
		/* the compiled defaults, before anything here writes a control */
		char what[160];

		snprintf(what, sizeof(what), "defaults: gate %u (step 3), NOISE GATE AUTO switch %u, "
			 "MIC VOLUME AUTO %u, fir %u", g_xd.gate, g_xma.gate_auto, g_xma.mic_auto,
			 g_xma.fir);
		check(g_xd.gate == XD_GATE_3 && g_xd.applied_gate == XD_GATE_3 &&
		      g_xma.gate_auto == 1 && !g_xma.mic_auto && !g_xma.fir && !g_xma.auto_on, what);
	}
	model(x);
	switches(x);
	level();
#endif
#ifdef XD_GATE_STEPS
	playback_vector(x);
#endif
	gate_regression();
	manual(x, false);
	free(x);
	printf(g_fail ? "FAILED\n" : "all passed\n");
	return g_fail;
}
