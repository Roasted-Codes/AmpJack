// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The mic's processing (DC removal, the 20 ms analysis, MIC VOLUME, the noise gate, the
 * limiter), the test tone and the driver loop's timing: the "XWDG" block (diag_360.h, xw.py
 * diag) and xd_poll. MIC-AUTO's glue (NOISE GATE AUTO, MIC VOLUME AUTO, the 24 -> 8 kHz
 * decimator) and its own status block: see "MIC-AUTO" below. Driver thread only, so nothing
 * here locks. voice_priv.h lists what the other audio files use from here. The loop-timing
 * probe and the comb canceller are development machinery: XW_DEV builds only.
 */
#include "game_360.h"
/* MIC-AUTO's per-window helpers once each, out of line and built for size (micauto_dsp.h) */
#define MICAUTO_SLOW static __attribute__((minsize, noinline, unused))
#include "micauto_dsp.h"
#include "voice_360.h"
#include "voice_priv.h"

/* ---- headset settings and diagnostics (diag_360.h; driver thread only) -------------------------
 * The test tone, the mic's DC removal, volume / noise gate / limiter and its meter, the mic
 * test (mictest_360.c), and the driver loop's timing. Like the sidetone they work on the
 * headset's own PCM.
 * Safety: the tone's peak is at most 1036 (-30 dBFS) x master; the mic's volume goes up to
 * +15 dB only on the way to the 360 / the recording, through a peak limiter at -1 dBFS; the
 * mic test plays back at the master volume (<= 1); every mixed sample is clamped. */
__attribute__((used)) struct xd_status g_xd = {
	.magic = 0x58574447, /* "XWDG", found by xw.py the way voice.py finds XVHS */
	.version = XD_VERSION,
	.buffer_ms = 45,
	.mic_gain = XD_MIC_GAIN_DEFAULT,
	.gate = XD_GATE_DEFAULT,
	.applied_buffer_ms = 45,
	.applied_mic_gain = XD_MIC_GAIN_DEFAULT,
	.applied_gate = XD_GATE_DEFAULT,
	.mic_gain_q15 = 32768,
	.gate_open = 1,
	.mic_dc = 1,
};

#define XD_VU_STALE_MS 100 /* no mic window for this long: the meter reads 0 */
/* "someone is talking" for the sidetone's hiss control (g_voice_ms, side_360.c): -45 dBFS,
 * the old LOW threshold, kept apart from the gate so a gate change does not move it */
#define XD_VOICE_RMS 184
static const u16 XD_TONE_HZ[XD_TONES] = { 0, 440, 1000 };
static const u16 XD_TONE_PEAK[XD_TONE_LEVELS] = { 519, 1036 }; /* -36, -30 dBFS */
/* by stored number (diag_360.h): off, step 1 -60, step 2 -55, AUTO, step 3 -50, step 4 -45
 * dBFS (20 ms RMS, full scale 32768). The owner's speech windows lie between -60 and -30,
 * most -45..-35, a fifth under -45: the old LOW -45 / HIGH -35 cut 4 % / most of it; steps 1
 * and 2 cut none of his sentences (tools/micdsp/README.md section 7), 3 and 4 are for louder
 * rooms and voices. AUTO: step 2's threshold while NOISE GATE AUTO is not in effect
 * (switched off, or its memory not there); in effect, the floor tracker's instead. */
static const u16 XD_GATE_RMS[XD_GATES] = { 0, 33, 58, 58, 104, 184 };
_Static_assert(XD_GATE_FALLBACK == XD_GATE_2, "XD_GATE_RMS[XD_GATE_AUTO] is step 2's");
/* the meter's segments: RMS (full scale 32768) at -48 -44 -40 -36 -32 -28 -24 -20 -14 -8 dBFS
 * (version 2: -57 -51 -45 -39 -33 -28 -23 -18 -12 -6; its first segment was the mic's DC) */
static const u16 XD_VU_RMS[10] = { 130, 207, 328, 519, 823, 1305, 2068, 3277, 6538, 13045 };
/* MIC VOLUME 1..10 in per cent: -12 dB .. +15 dB in 3 dB steps (rounded to 5 % or so) */
const u16 XD_MIC_GAINS[XD_MIC_LEVELS] = { 25, 35, 50, 70, 100, 140, 200, 280, 400, 560 };
#define XD_DC_SHIFT 10      /* the DC tracker: 1/1024 a sample (~3.7 Hz at 24 kHz) */

struct xd_tone {
	u32 phase, step; /* 2^-32 turns; never reset while it sounds (continuous across blocks) */
	u32 hz, rate;    /* what step belongs to */
	u32 env;         /* 0..rate / 100: the fade */
	s32 peak;        /* at the end of the last block (a level or master change ramps) */
};
static struct xd_tone g_tone[PADS];
struct xd_mic g_dmic[PADS];

/* the tone has pad's earpiece (game audio is held meanwhile): on, or still fading out */
bool tone_active(int pad)
{
	return g_xd.applied_tone != 0 || g_tone[pad].env != 0;
}

/* the test tone mixed into an earpiece block (s16le, rate_out x ch_out); master: Q8 */
void tone_mix(int pad, u8 *out, int len, u32 master)
{
	struct xd_tone *t = &g_tone[pad];
	u32 ch = g_pad[pad].ch_out, frame = 2 * ch, rate = g_pad[pad].rate_out;
	u32 sel = g_xd.applied_tone, lvl = g_xd.applied_tone_level, fade = rate / XD_FADE_DIV;
	int frames = len / (int)frame;
	s32 from = t->peak, to;

	if (sel >= XD_TONES)
		sel = 0;
	if (lvl >= XD_TONE_LEVELS)
		lvl = 0;
	if ((!sel && !t->env) || !fade || frames <= 0)
		return;
	if (master > 256)
		master = 256;
	if (t->env > fade)
		t->env = fade;
	if (sel) {
		if (!t->env)
			t->phase = 0; /* a start, from a zero crossing */
		if (t->hz != XD_TONE_HZ[sel] || t->rate != rate) {
			/* a new pitch keeps the phase: no click */
			t->hz = XD_TONE_HZ[sel];
			t->rate = rate;
			t->step = (u32)(((u64)t->hz << 32) / rate);
		}
		to = (s32)(XD_TONE_PEAK[lvl] * master >> 8); /* <= 1036 */
		if (!t->env)
			from = to; /* the fade alone shapes a start */
	} else {
		to = from; /* fading out at the level it had */
	}
	for (int f = 0; f < frames; f++) {
		s32 peak, y;

		if (sel) {
			if (t->env < fade)
				t->env++;
		} else if (!t->env) {
			break; /* faded out: the phase stops here */
		} else {
			t->env--;
		}
		peak = from + (to - from) * f / frames;
		y = ((isin(t->phase) * peak + 16384) >> 15) * (s32)t->env / (s32)fade;
		t->phase += t->step;
		for (u32 c = 0; c < ch; c++) {
			u8 *o = out + f * (int)frame + 2 * (int)c;

			put_le16(o, (u16)clamp16((s16)get_le16(o) + y));
		}
	}
	t->peak = to;
	g_xd.tone_hz = t->env ? t->hz : 0;
	g_xd.tone_peak = (u32)to * t->env / fade;
}

/* MIC VOLUME's index (0..9) nearest to pct (the nearest in dB: the geometric midpoints) */
u32 gain_index(u32 pct)
{
	u32 i = 0;

	if (pct > 10000)
		pct = 10000; /* no overflow below */
	while (i + 1 < XD_MIC_LEVELS &&
	       pct * pct > (u32)XD_MIC_GAINS[i] * XD_MIC_GAINS[i + 1])
		i++;
	return i;
}

/* MIC VOLUME's manual step in effect as Q12 (<= 22937). Out of line: the mic path calls it
 * once per chunk, not per sample. */
static __attribute__((minsize, noinline)) u32 manual_q12(void)
{
	return (u32)XD_MIC_GAINS[gain_index(g_xd.applied_mic_gain)] * XD_Q12 / 100;
}

/* ---- MIC-AUTO: NOISE GATE AUTO, MIC VOLUME AUTO, the 24 -> 8 kHz decimator -------------------
 * The kernels are micauto_dsp.h (pure integer C, bit-exact with the model in tools/micdsp/,
 * whose README has the design and the measurements); this is their glue.
 *
 * Status block (magic 0x58574D41; micauto.py and accept/micauto.py mirror it; all u32, append
 * only). Controls, switches written over xbdm (micauto.py; any value but 0 is on) and applied
 * and logged by xd_poll:
 *   gate_auto  1 (default): NOISE GATE AUTO is offered (xd_gate_auto_offered) and gate 3
 *              (the default gate) runs it. 0: gate 3 runs LOW's threshold.
 *   mic_auto   1: MIC VOLUME AUTO is offered (xd_mic_auto_offered) and runs when chosen.
 *              0 (default): it is never in effect, whatever was chosen.
 *   fir        1: the 360's mic goes 24 -> 8 kHz through the 45-tap low-pass instead of
 *              rs_push, whose box filter lets 4..12 kHz fold back into the voice band only
 *              ~5 dB down. Adds 0.88 ms. Other rates keep rs_push (fir_bypass counts them).
 *   comb       XW_DEV builds: the 125 Hz comb canceller after the DC removal, for every
 *              consumer. A release build writes it back to 0.
 *   auto_on    MIC VOLUME AUTO chosen (the panel and the settings: xd_set_mic_auto).
 * State of pad `pad` (the first headset; PADS = none), RMS in sample units (full scale
 * 32768, as XWDG's gate_rms): floor_rms, thr_rms (AUTO's threshold), speech_rms (the AGC's
 * speech level, 0 before any speech); agc_q12 / agc_level (the gain AUTO wants, as Q12 and as
 * the nearest 1..10); agc_state (bit 0 frozen: the last window left the gain where it was, as
 * in a pause, with the gate shut, muted, within 1 dB of target or AUTO off; bit 1 acquiring
 * fast; bit 2 moving); speech_windows (the AGC's count, stops at 256). Counters: auto_opens
 * (the gate opened while on AUTO), fir_samples (8 kHz samples the decimator made), fir_bypass
 * (mic chunks that took rs_push with fir on), nomem (failed allocations: tried every pass);
 * heap_bytes.
 *
 * Memory: one malloc for the four pads (floor tracker, AGC, decimator: 1120 bytes), made by
 * xd_poll on the driver thread when a control is on and freed when all are off. gate_auto is
 * on at boot, so that is the driver loop's first pass, with the plugin's other start-up
 * allocations (small pool: under 0xFD8 bytes). If it fails, every control acts as off (gate 3
 * runs LOW, the panel hides AUTO), nomem counts, the log says so once, and the next pass tries
 * again. With every control off nothing is allocated and the manual settings run alone
 * (tests/test_micdsp360.c checks they give the same output whatever the switches say). With
 * state, the floor and the speech level are learnt in every mode, so AUTO is right the moment
 * it is picked; the manual settings never read them. A pad's part restarts when its headset
 * comes or goes (xd_reset_pad). Hot reload's xv_quiesce resets every pad with g_quiesced set:
 * that frees it all, and nothing is allocated again until xv_resume clears it. */
#define MA_GATE_AUTO_DEFAULT 1 /* NOISE GATE AUTO, the default gate */
#define MA_FIR_DEFAULT 0 /* the owner decides after an A/B (accept/micauto.py J3) */
/* Built for size and kept out of line: everything below runs once per 20 ms window or per
 * loop pass, where a few hundred ns do not matter, and clang's -O2 otherwise inlines the
 * dB / floor / AGC helpers into every caller (about 2.5 KB more image). The per-sample
 * work (the decimator in mic_to_360, the comb in mic_clean) stays at -O2. */
#define MA_SMALL __attribute__((minsize, noinline))

struct ma_status {
	u32 magic, version;
	volatile u32 gate_auto, mic_auto, fir, comb, auto_on;
	u32 pad, floor_rms, thr_rms, speech_rms, agc_q12, agc_level, agc_state, speech_windows;
	u32 auto_opens, fir_samples, fir_bypass, heap_bytes, nomem;
};
_Static_assert(sizeof(struct ma_status) == 20 * 4, "micauto.py FIELDS");

__attribute__((used)) struct ma_status g_xma = {
	.magic = 0x58574D41,
	.version = 1,
	.gate_auto = MA_GATE_AUTO_DEFAULT,
	.fir = MA_FIR_DEFAULT,
	.pad = PADS,
};

enum { MA_FROZEN = 1, MA_ACQUIRING = 2, MA_MOVING = 4 }; /* agc_state */

struct ma_pad {
	struct ma_floor floor;
	struct ma_agc agc;
	struct ma_fir fir;
	u32 fir_on; /* the decimator has this pad's stream; its rs_mic is idle meanwhile */
	u32 state;  /* agc_state after its last window */
};
struct ma_mem {
	struct ma_pad pad[PADS];
#ifdef XW_DEV
	struct ma_comb *comb; /* PADS of them: 3 KB, only once comb has been switched on */
#endif
};
static struct ma_mem *g_ma;
/* the controls in effect: only ever 1 while g_ma exists */
static u32 g_ma_gate, g_ma_mic, g_ma_auto, g_ma_fir, g_ma_comb;

MA_SMALL static void ma_pad_reset(int pad)
{
	struct ma_pad *s = &g_ma->pad[pad];

	ma_floor_init(&s->floor);
	ma_agc_init(&s->agc, manual_q12());
	ma_fir_init(&s->fir);
	s->fir_on = 0;
	s->state = MA_FROZEN;
#ifdef XW_DEV
	if (g_ma->comb)
		ma_comb_init(&g_ma->comb[pad]);
#endif
}

/* the decimator lets go of pad's stream: rs_mic takes it over from a fresh state (its own
 * is as old as the decimator's run; a stale sample would click) */
MA_SMALL static void ma_fir_release(int pad)
{
	if (!g_ma->pad[pad].fir_on)
		return;
	g_ma->pad[pad].fir_on = 0;
	{
		/* rs_init's result without its gcd: fir_on is only ever set for 24 -> 8 kHz, so
		 * in / out / n are 3 / 1 / 3 already; the rest is the running state */
		struct rs *r = &g_pad[pad].rs_mic;

		r->t = r->bi = 0;
		r->prev = r->sum = 0;
		memset(r->box, 0, sizeof(r->box));
	}
}

MA_SMALL static void ma_free(void)
{
	for (int i = 0; i < PADS; i++)
		ma_fir_release(i);
	g_ma_gate = g_ma_mic = g_ma_auto = g_ma_fir = g_ma_comb = 0;
#ifdef XW_DEV
	free(g_ma->comb);
#endif
	free(g_ma);
	g_ma = NULL;
	g_xma.heap_bytes = 0;
}

/* false: no memory (tried again next pass; nomem counts the tries) or quiesced */
MA_SMALL static bool ma_alloc(void)
{
	if (g_ma)
		return true;
	if (g_quiesced)
		return false;
	g_ma = malloc(sizeof(*g_ma));
	if (!g_ma) {
		if (!g_xma.nomem++)
			xa_log("diag: mic auto: no memory\n");
		return false;
	}
	memset(g_ma, 0, sizeof(*g_ma));
	for (int i = 0; i < PADS; i++)
		ma_pad_reset(i);
	g_xma.heap_bytes = sizeof(*g_ma);
	return true;
}

/* MIC VOLUME AUTO in effect = offered and chosen. Starting it starts every pad's gain at the
 * manual step in effect, so nothing jumps; leaving it needs nothing (g_xd.mic_gain still
 * holds the manual step, and shape()'s 10 ms ramp goes there). */
MA_SMALL static void ma_apply_auto(void)
{
	u32 on = g_ma_mic && g_xma.auto_on;

	if (on && !g_ma_auto)
		for (int i = 0; i < PADS; i++)
			ma_agc_start(&g_ma->pad[i].agc, manual_q12());
	if (on != g_ma_auto)
		xa_log("diag: mic volume auto %s\n", on ? "on" : "off");
	g_ma_auto = on;
}

/* one 20 ms window of pad's mic (its mean square, before any gain) once the gate has decided
 * on it: the AGC learns from it (and moves, if AUTO is in effect) against the floor as it was,
 * then the floor tracker takes it, so the threshold it sets applies from the next window.
 * open: the gate in effect passed the window (always with the gate off). */
MA_SMALL static void ma_window(int pad, u32 ms, bool open)
{
	struct ma_pad *s = &g_ma->pad[pad];
	bool learn = !g_mic_mute[pad];
	u32 g = s->agc.g;

	ma_agc_window(&s->agc, ms, &s->floor, open, learn, g_ma_auto && learn);
	ma_floor_window(&s->floor, ms);
	s->state = ((u32)s->agc.g == g ? MA_FROZEN : 0) | (s->agc.fast ? MA_ACQUIRING : 0) |
		   (s->agc.moving ? MA_MOVING : 0);
}

/* MIC VOLUME in effect (voice_priv.h): AUTO's gain for this pad, or the manual step */
u32 mic_gain_target(int pad)
{
	if (pad < 0)
		pad = (int)g_xma.pad;
	if (g_ma_auto && pad < PADS)
		return g_ma->pad[pad].agc.q12;
	return manual_q12();
}

void xd_reset_pad(int pad)
{
	memset(&g_tone[pad], 0, sizeof(g_tone[pad]));
	memset(&g_dmic[pad], 0, sizeof(g_dmic[pad]));
	if (g_ma && g_quiesced)
		ma_free(); /* hot reload: nothing of ours outlives the hand-over */
	else if (g_ma)
		ma_pad_reset(pad);
	mt_pad_gone(pad, "the headset went away (or changed format)");
}

static u32 vu_of(u32 rms)
{
	u32 n = 0;

	while (n < 10 && rms >= XD_VU_RMS[n])
		n++;
	return n;
}

/* the mic of pad slot `pad` as every consumer gets it: `frames` (<= XD_CHUNK) frames of
 * s16le at rate_in x ch into mono s16le `out`, the DC offset removed (g_xd.mic_dc) */
void mic_clean(int pad, const u8 *d, int frames, u32 ch, u8 *out)
{
	struct xd_mic *m = &g_dmic[pad];
	bool dc = g_xd.mic_dc != 0;
#ifdef XW_DEV
	/* the comb is learnt on the USB blocks' 192-sample period: 24 kHz only */
	struct ma_comb *comb = g_ma_comb && g_pad[pad].rate_in == 24000 ? &g_ma->comb[pad] : NULL;
#endif

	for (int i = 0; i < frames; i++, d += 2 * ch) {
		s32 x = (s16)get_le16(d);

		if (ch == 2)
			x = (x + (s16)get_le16(d + 2)) >> 1;
		if (dc) {
			/* dc follows the input's mean with a 1024-sample time constant, in Q12 */
			if (!m->dc_on) {
				m->dc = x * (1 << 12); /* start where the offset is: no step */
				m->dc_on = 1;
			}
			m->dc += (x * (1 << 12) - m->dc) >> XD_DC_SHIFT;
			x = clamp16(x - (m->dc >> 12));
		}
#ifdef XW_DEV
		if (comb)
			x = ma_comb_push(comb, x);
#endif
		put_le16(out + 2 * i, (u16)(s16)x);
	}
	g_xd.mic_dc_now = dc ? (u32)(m->dc >> 12) : 0;
}

/* every mic chunk of pad slot `pad` (mono s16le, after mic_clean), muted or not: the 20 ms
 * RMS (before the volume), from which the gate decides (open at the threshold, held
 * XD_GATE_HOLD_MS) and the meter reads (after the volume). It runs whatever the 360 does with
 * the mic, so the meter and the gate's state are right in the dashboard too. With MIC-AUTO's
 * state each window then feeds the floor tracker and the AGC (ma_window); on NOISE GATE AUTO
 * the threshold is the tracker's, read again at every window (the last one may have moved it).
 *
 * Opening early: a shut gate used to open only at the end of the window that reached the
 * threshold, so that window (up to 20 ms, a word's start) was lost. Now, at the end of every
 * chunk, a window whose sum so far already guarantees its RMS reaches the threshold (sum >=
 * thr^2 x window, whatever the rest of it holds) sets `opening`, and the gate passes from that
 * chunk on: no delay is added to the mic, and nothing opens that the window's end would not
 * open (it confirms it: the hold and the counters are taken there, as before).
 * The gate's steps are voice_priv.h's gate_*: the mic test's playback runs the same ones. */
void mic_analyse(int pad, const u8 *d, int frames)
{
	struct xv_pad *p = &g_pad[pad];
	struct xd_mic *m = &g_dmic[pad];
	u32 rate = p->rate_in;
	bool autog = g_ma_gate && g_xd.applied_gate == XD_GATE_AUTO;
	u32 thr = mic_gate_threshold(pad);
	u32 win = gate_win(rate);

	gate_chunk_start(m, thr);
	g_xd.gate_threshold = thr;
	for (int i = 0; i < frames; i++, d += 2) {
		if (!gate_push(m, (s16)get_le16(d), win))
			continue;
		{
			u32 ms = (u32)(m->sum / m->n), rms = isqrt(ms);

			if (autog) {
				thr = g_ma->pad[pad].floor.thr_rms;
				g_xd.gate_threshold = thr;
			}
			g_xd.gate_rms = rms;
			if (rms >= XD_VOICE_RMS)
				g_voice_ms[pad] = xa_now_ms();
			st_dev_watch(pad, rms, 1000 / XD_GATE_WIN_DIV);
			m->vu = vu_of(rms * mic_gain_target(pad) / XD_Q12); /* <= 32768 x 5.6 */
			m->vu_ms = xa_now_ms();
			g_xd.mic_vu = m->vu;
			if (gate_window_end(m, rms, thr, rate)) {
				g_xd.gate_opens++;
				if (autog)
					g_xma.auto_opens++;
			}
			if (g_ma)
				ma_window(pad, ms, !thr || rms >= thr);
		}
	}
	/* the threshold the window under way will be judged by: AUTO's moved at the window end
	 * above (ma_window), after it decided that window */
	if (autog)
		thr = g_ma->pad[pad].floor.thr_rms;
	gate_chunk_end(m, thr, win);
	g_xd.gate_open = !gate_shut(m);
}

/* the gate's threshold in effect now for pad slot `pad` (voice_priv.h) */
u32 mic_gate_threshold(int pad)
{
	u32 gate = g_xd.applied_gate;

	if (g_ma_gate && gate == XD_GATE_AUTO && pad >= 0 && pad < PADS)
		return g_ma->pad[pad].floor.thr_rms;
	return gate < XD_GATES ? XD_GATE_RMS[gate] : 0;
}

/* The mic after shape() on its way to the 360 (mono s16le at the headset's rate), and the
 * silence sent in its place while muted (xv_mic_samples), so the decimator's filter sees one
 * continuous stream. With fir on and the capture path set for 24 -> 8 kHz (rs_init reduces
 * the rates to 3 / 1), MIC-AUTO's decimator: one output per three inputs like rs_push, so the
 * capture packets fill at the same pace. Otherwise voice_360.c's linear resampler. (XV36's
 * mic_peak is mic_feed's: it does not move while the decimator has the stream.) */
void mic_to_360(int pad, const u8 *d, int frames)
{
	const struct xv_pad *p = &g_pad[pad];
	struct ma_pad *s;
	s16 out[16];
	int n = 0;

	if (!g_ma_fir || p->rate_in != 24000 || p->rs_mic.in != 3 || p->rs_mic.out != 1) {
		if (g_ma_fir)
			g_xma.fir_bypass++;
		mic_feed(pad, &g_pad[pad].rs_mic, d, frames, 1);
		return;
	}
	s = &g_ma->pad[pad];
	if (!s->fir_on) {
		ma_fir_init(&s->fir); /* from silence: ~1 ms of fade-in */
		s->fir_on = 1;
	}
	for (int i = 0; i < frames; i++) {
		n += ma_fir_push(&s->fir, (s16)get_le16(d + 2 * i), out + n);
		if (n == 16 || (n && i == frames - 1)) { /* out[] full, or the chunk's last */
			xv_cap_feed(pad, out, n);
			g_xma.fir_samples += (u32)n;
			n = 0;
		}
	}
}

/* the mic on its way out (mono s16le, after mic_clean): MIC VOLUME, the gate (decided by
 * mic_analyse) and the limiter, into out (frames <= XD_CHUNK). xv_mic_samples hands the result
 * to the 360 (mic_to_360) and to the PC bridge. */
void mic_shape(int pad, const u8 *d, int frames, u8 *out)
{
	struct xv_pad *p = &g_pad[pad];
	struct xd_mic *m = &g_dmic[pad];
	u32 gain_to = mic_gain_target(pad), shut_to = gate_shut(m) ? 32768 : 0;

	if (frames > XD_CHUNK)
		frames = XD_CHUNK;
	for (int i = 0; i < frames; i++)
		put_le16(out + 2 * i,
			 (u16)shape(&m->sh, (s16)get_le16(d + 2 * i), gain_to, shut_to, p->rate_in));
	g_xd.mic_gain_q15 = m->sh.gain * (32768 / XD_Q12);
}

u32 xd_mic_vu(int pad)
{
	if (pad < 0 || pad >= PADS || !hs_format_ok(&g_pad[pad]))
		return 0;
	if (xa_now_ms() - g_dmic[pad].vu_ms > XD_VU_STALE_MS)
		return 0;
	return g_dmic[pad].vu;
}

bool xd_gate_open(int pad)
{
	if (pad < 0 || pad >= PADS || !hs_format_ok(&g_pad[pad]) || g_mic_mute[pad])
		return false;
	return !gate_shut(&g_dmic[pad]);
}

/* ---- the driver loop's timing (diag_360.h) ---- */

#define XD_TB_PER_US 50 /* the timebase: 50 MHz (tb_per_ms checks it) */
#define XD_PROBE_SLEEP 0x100
#define XD_PROBE_TICK 0x200
#define XD_PROBE_PASSES 16

u32 xd_timebase(void)
{
#if defined(__PPC__) && !defined(XV_HOST_TEST)
	u32 t;

	__asm__ __volatile__("mftb %0" : "=r"(t));
	return t;
#else
	return 0;
#endif
}

static struct {
	u32 tb_prev, work;            /* this pass: its start, its work so far (ticks) */
	u32 tb_end;                   /* the end of the last xd_time_work (xv_poll's) */
	u32 win_tb, win_passes;       /* the tb_per_ms window */
	u64 win_ms;
	u32 probe, n, min, max;       /* the probe running, its samples so far */
	u64 sum;
	u32 step;
} g_xt;

#ifdef XW_DEV
static u64 sys_100ns(void)
{
#ifndef XV_HOST_TEST
	int64_t t;

	KeQuerySystemTime(&t);
	return (u64)t;
#else
	return xa_now_ms() * 10000;
#endif
}

/* one probe sample: the time the call takes, in timebase ticks */
static u32 probe_once(u32 kind)
{
	u32 t0 = xd_timebase();

	if ((kind & 0xF00) == XD_PROBE_SLEEP) {
		xa_sleep_ms(kind & 0xFF);
	} else {
		/* from one system time change to the next: the clock's step. 40 ms at most */
		u64 s0 = sys_100ns(), s1;
		u32 limit = 40000 * XD_TB_PER_US;

		while ((s1 = sys_100ns()) == s0 && xd_timebase() - t0 < limit)
			;
		t0 = xd_timebase();
		s0 = s1;
		while ((s1 = sys_100ns()) == s0 && xd_timebase() - t0 < limit)
			;
		g_xt.step = (u32)(s1 - s0);
	}
	return xd_timebase() - t0;
}

static void probe_poll(void)
{
	u32 cmd = g_xd.probe, t;

	if (!g_xt.probe) {
		if (!cmd)
			return;
		if (!((cmd & 0xF00) == XD_PROBE_SLEEP && (cmd & 0xFF) <= 20) && cmd != XD_PROBE_TICK) {
			xa_log("diag: probe %x: unknown (0x100 + ms, 0x200)\n", cmd);
			g_xd.probe = 0;
			return;
		}
		g_xt.probe = cmd;
		g_xt.n = g_xt.max = 0;
		g_xt.min = 0xFFFFFFFFu;
		g_xt.sum = 0;
		g_xt.step = 0;
	}
	t = probe_once(g_xt.probe);
	g_xt.work -= t; /* the probe isn't the pass's work (xv_poll's xd_time_work adds it back) */
	g_xt.n++;
	g_xt.sum += t;
	if (t < g_xt.min)
		g_xt.min = t;
	if (t > g_xt.max)
		g_xt.max = t;
	if (g_xt.n < XD_PROBE_PASSES)
		return;
	g_xd.probe_kind = g_xt.probe;
	g_xd.probe_n = g_xt.n;
	g_xd.probe_min_us = g_xt.min / XD_TB_PER_US;
	g_xd.probe_avg_us = (u32)(g_xt.sum / g_xt.n) / XD_TB_PER_US;
	g_xd.probe_max_us = g_xt.max / XD_TB_PER_US;
	g_xd.probe_step_100ns = g_xt.step;
	if (g_xt.probe == XD_PROBE_TICK)
		xa_log("diag: probe: system time steps every %u us (min %u, max %u; last step %u x "
		       "100 ns)\n", g_xd.probe_avg_us, g_xd.probe_min_us, g_xd.probe_max_us,
		       g_xt.step);
	else
		xa_log("diag: probe: xa_sleep_ms(%u) takes %u us (min %u, max %u, %u passes)\n",
		       g_xt.probe & 0xFF, g_xd.probe_avg_us, g_xd.probe_min_us, g_xd.probe_max_us,
		       g_xt.n);
	g_xt.probe = 0;
	BARRIER();
	g_xd.probe = 0;
	g_xd.probe_runs++;
}
#else
/* a release build has no probe: xbdm's probe word is left as written, nothing runs */
static void probe_poll(void)
{
}
#endif

void xd_time_pass(void)
{
	static const u32 edges_ms[5] = { 4, 8, 12, 17, 25 };
	u32 tb = xd_timebase();
	u64 now = xa_now_ms();

	if (g_xt.tb_prev) {
		u32 us = (tb - g_xt.tb_prev) / XD_TB_PER_US, b = 0;

		g_xd.pass_us = us;
		if (us > g_xd.pass_max_us)
			g_xd.pass_max_us = us;
		while (b < 5 && us >= edges_ms[b] * 1000)
			b++;
		g_xd.pass_hist[b]++;
		g_xd.work_us = g_xt.work / XD_TB_PER_US;
		if (g_xd.work_us > g_xd.work_max_us)
			g_xd.work_max_us = g_xd.work_us;
		if (g_xt.tb_end) {
			g_xd.gap_us = (tb - g_xt.tb_end) / XD_TB_PER_US;
			if (g_xd.gap_us > g_xd.gap_max_us)
				g_xd.gap_max_us = g_xd.gap_us;
		}
	}
	g_xt.tb_prev = tb;
	g_xt.work = 0;
	if (!g_xt.win_ms) {
		g_xt.win_ms = now;
		g_xt.win_tb = tb;
		g_xt.win_passes = 0;
	} else if (now - g_xt.win_ms >= 1000) {
		u32 ms = (u32)(now - g_xt.win_ms);

		g_xd.tb_per_ms = (tb - g_xt.win_tb) / ms;
		g_xd.passes_per_s = g_xt.win_passes * 1000 / ms;
		g_xt.win_ms = now;
		g_xt.win_tb = tb;
		g_xt.win_passes = 0;
	}
	g_xt.win_passes++;
}

void xd_time_work(u32 tb_start)
{
	u32 tb = xd_timebase();

	g_xt.work += tb - tb_start;
	g_xt.tb_end = tb;
}

/* the accepted MIC VOLUME nearest to v */
static u32 round_gain(u32 v)
{
	return XD_MIC_GAINS[gain_index(v)];
}

u32 xd_mic_gain_step(u32 pct, int steps)
{
	s32 i = (s32)gain_index(pct) + steps;

	return XD_MIC_GAINS[i < 0 ? 0 : i >= XD_MIC_LEVELS ? XD_MIC_LEVELS - 1 : i];
}

u32 xd_mic_level(u32 pct)
{
	return gain_index(pct) + 1;
}

/* MIC VOLUME AUTO and the panel's "offered" getters (diag_360.h) */
bool xd_mic_auto(void)
{
	return g_ma_auto != 0;
}

MA_SMALL void xd_set_mic_auto(bool on)
{
	g_xma.auto_on = on;
	ma_apply_auto(); /* at once: the panel shows the result before the next xd_poll */
}

MA_SMALL u32 xd_mic_auto_level(void)
{
	return gain_index(mic_gain_target(-1) * 100 / XD_Q12) + 1;
}

bool xd_gate_auto_offered(void)
{
	return g_ma_gate != 0;
}

bool xd_mic_auto_offered(void)
{
	return g_ma_mic != 0;
}

MA_SMALL static const char *gate_name(u32 gate)
{
	static const char *const gates[XD_GATES + 1] = {
		"off", "1 (-60 dBFS)", "2 (-55 dBFS)", "auto", "3 (-50 dBFS)", "4 (-45 dBFS)",
		"auto (as 2: not in effect)" };

	return gates[gate == XD_GATE_AUTO && !g_ma_gate ? XD_GATES : gate < XD_GATES ? gate : 0];
}

/* MIC-AUTO's controls (driver thread, every pass): (de)allocate, apply, log, and the state
 * of pad slot `pad` (-1: no headset) into the status block. Each control is a switch: any
 * value but 0 is on. */
MA_SMALL static void ma_poll(int pad)
{
	u32 gate = g_xma.gate_auto != 0, mic = g_xma.mic_auto != 0, fir = g_xma.fir != 0;
	u32 comb = g_xma.comb != 0;
	const struct ma_pad *s;

#ifndef XW_DEV
	if (comb)
		g_xma.comb = comb = 0; /* development machinery: not in a release build */
#endif
	if ((gate | mic | fir | comb) && !ma_alloc())
		gate = mic = fir = comb = 0; /* as if all were off until memory comes */
#ifdef XW_DEV
	if (comb && !g_ma->comb) {
		g_ma->comb = malloc(PADS * sizeof(struct ma_comb));
		if (!g_ma->comb) {
			g_xma.nomem++;
			g_xma.comb = comb = 0;
		} else {
			g_xma.heap_bytes += PADS * sizeof(struct ma_comb);
		}
	}
	if (comb && !g_ma_comb)
		for (int i = 0; i < PADS; i++)
			ma_comb_init(&g_ma->comb[i]); /* learns afresh (0.5 s) */
#endif
	if (g_ma_fir && !fir)
		for (int i = 0; i < PADS; i++)
			ma_fir_release(i);
	if (gate != g_ma_gate || mic != g_ma_mic || fir != g_ma_fir || comb != g_ma_comb) {
		bool gate_now = gate != g_ma_gate && g_xd.applied_gate == XD_GATE_AUTO;

		xa_log("diag: mic auto: gate_auto %u mic_auto %u fir %u comb %u\n", gate, mic, fir,
		       comb);
		g_ma_gate = gate;
		if (gate_now) /* the gate is on AUTO: its threshold changes now */
			xa_log("diag: mic noise gate %s\n", gate_name(XD_GATE_AUTO));
	}
	g_ma_mic = mic;
	g_ma_fir = fir;
	g_ma_comb = comb;
	ma_apply_auto();
	if (g_ma && !(gate | mic | fir | comb))
		ma_free();

	g_xma.pad = pad >= 0 ? (u32)pad : PADS;
	if (!g_ma || pad < 0) {
		g_xma.floor_rms = g_xma.thr_rms = g_xma.speech_rms = g_xma.agc_q12 = 0;
		g_xma.agc_level = g_xma.agc_state = g_xma.speech_windows = 0;
		return;
	}
	s = &g_ma->pad[pad];
	g_xma.floor_rms = ma_lin(s->floor.floor_db, 15);
	g_xma.thr_rms = s->floor.thr_rms;
	g_xma.speech_rms = ma_lin(s->agc.speech_db, 15); /* 0 for MA_DB_SILENT (none yet) */
	g_xma.agc_q12 = s->agc.q12;
	g_xma.agc_level = xd_mic_level(s->agc.q12 * 100 / XD_Q12);
	g_xma.agc_state = s->state;
	g_xma.speech_windows = s->agc.n;
}

void xd_poll(void)
{
	static const char *const tones[XD_TONES] = { "off", "440 Hz", "1 kHz" };
	static const char *const levels[XD_TONE_LEVELS] = { "very quiet (-36 dBFS)",
							    "quiet (-30 dBFS)" };
	static u32 told;
	u32 tone = g_xd.tone, lvl = g_xd.tone_level, buf = g_xd.buffer_ms;
	u32 mg = round_gain(g_xd.mic_gain), gate = g_xd.gate;

	if (!told) {
		told = 1;
		xa_log("diag: defaults: mic volume %u%% (level %u), DC removal %s, noise gate %s, "
		       "sidetone %u; mic test %s\n", g_xd.applied_mic_gain,
		       xd_mic_level(g_xd.applied_mic_gain), g_xd.mic_dc ? "on" : "off",
		       gate_name(g_xd.applied_gate), xg_sidetone(),
		       xd_mictest_enabled() ? "on" : "off (XWMT enable)");
	}
	if (g_xd.mic_dc > 1)
		g_xd.mic_dc = 1;
	/* a bad xbdm value is clamped and written back: an unknown tone is no tone */
	if (tone >= XD_TONES)
		tone = 0;
	if (lvl >= XD_TONE_LEVELS)
		lvl = 0;
	buf = buf <= 37 ? 30 : buf <= 52 ? 45 : buf <= 75 ? 60 : 90;
	if (gate >= XD_GATES)
		gate = XD_GATE_4; /* out of range is the strongest */
	if (g_xd.tone != tone)
		g_xd.tone = tone;
	if (g_xd.tone_level != lvl)
		g_xd.tone_level = lvl;
	if (g_xd.buffer_ms != buf)
		g_xd.buffer_ms = buf;
	if (g_xd.mic_gain != mg)
		g_xd.mic_gain = mg;
	if (g_xd.gate != gate)
		g_xd.gate = gate;

	if (tone != g_xd.applied_tone || lvl != g_xd.applied_tone_level) {
		g_xd.applied_tone = tone;
		g_xd.applied_tone_level = lvl;
		xa_log("diag: test tone %s, level %s x master %u%%%s\n", tones[tone], levels[lvl],
		       xg_master(), tone ? " (in place of game audio)" : "");
	}
	if (buf != g_xd.applied_buffer_ms && xg_set_buffer(buf)) {
		xa_log("diag: game buffer %u -> %u ms (ramping from %u frames)\n",
		       g_xd.applied_buffer_ms, buf, xg_buffer_target());
		g_xd.applied_buffer_ms = buf;
	}
	if (mg != g_xd.applied_mic_gain) {
		g_xd.applied_mic_gain = mg;
		xa_log("diag: mic volume %u%% (level %u)\n", mg, xd_mic_level(mg));
	}
	if (gate != g_xd.applied_gate) {
		g_xd.applied_gate = gate;
		xa_log("diag: mic noise gate %s\n", gate_name(gate));
	}
	g_xd.target_frames = xg_buffer_target();
	g_xd.high_frames = xg_buffer_high();
	g_xd.drift_full_frames = xg_buffer_drift_full();
	{
		/* the meter falls to 0 when the mic stops */
		int pad = -1;

		for (int i = 0; i < PADS && pad < 0; i++)
			if (hs_format_ok(&g_pad[i]))
				pad = i;
		g_xd.mic_vu = pad >= 0 ? xd_mic_vu(pad) : 0;
		ma_poll(pad);
	}
	mt_poll();
	probe_poll();
}
