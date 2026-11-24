// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * What the headset's audio files share (driver thread unless a line says otherwise):
 *
 *   voice_360.c    the virtual 360 headset (XVoiced hooks, packet rings) and the xv_*
 *                  entry points the USB side calls: xv_mic_samples, xv_earpiece_fill, xv_poll
 *   side_360.c     sidetone (software and the headset's own), headset volume, hiss control,
 *                  the centre tick, the mic mute ("XVHS")
 *   micdsp_360.c   the mic's DC removal, gate, meter, volume and limiter, the test tone, the
 *                  driver loop's timing ("XWDG", xd_poll)
 *   mictest_360.c  the mic test and its contract adapters ("XWMT")
 *
 * The order of the earpiece mix and of the mic chain is frozen (docs/ROADMAP.md 2.4): it is
 * written out in xv_earpiece_fill and xv_mic_samples, the only places that call across these
 * files on the audio path. A feature adds its stage at the call site prepared for it there
 * (xq_process; mic_to_360 in micdsp_360.c for the decimator; the PC bridge's xb_* calls), not
 * inside another file.
 *
 * The helpers below are `static` in this header, as they were in the one file they came
 * from, so each file inlines them exactly as before (see dsp_360.h for why not `inline`).
 */
#ifndef VOICE_PRIV_H
#define VOICE_PRIV_H

#include <stdlib.h>
#include <xecore/xboxkrnl.h>

#include "../core/platform.h"
#include "diag_360.h"
#include "dsp_360.h"
#include "kernel_360.h"

#define PADS 4
#define RING 16           /* packets per pad and direction (the "no free node" limit) */

struct ring {
	u32 *pkt[RING];
	u32 *ctx[RING];
	u32 owner[RING];
	u32 head, tail; /* tail = oldest */
};

/* integer resampler: linear interpolation with a box pre-filter when decimating */
struct rs {
	u32 in, out, t, n, bi;
	s32 prev, sum;
	s32 box[8];
};

struct xv_pad {
	volatile u32 serving;  /* the hooks act as a present headset (under g_lock) */
	u32 bound;             /* category-1 bind requested and not yet undone */
	u32 usb_present;
	u32 rate_in, ch_in, rate_out, ch_out;
	struct ring cap, ren;  /* under g_lock */
	struct rs rs_mic, rs_ear, rs_loop;
	s16 ef[16];            /* earpiece: resampled samples not yet used */
	int ef_n, ef_pos;
	u64 vclock, last_mic_ms, last_ear_ms, bind_retry_ms;
};

extern struct xv_pad g_pad[PADS]; /* voice_360.c */
extern u32 g_quiesced;            /* voice_360.c: hot reload, nothing is bound or served again */

/* hiss control (side_360.c) */
#define QUIET_PEAK 16         /* -66 dBFS */
#define QUIET_TALK_MS 800     /* the sidetone stays this long after the last voice window */
#define QUIET_AMP_MS 1000     /* the amplifier stays up this long after the last sound */
extern u64 g_voice_ms[PADS], g_ear_loud_ms[PADS];

extern u8 g_mic_mute[PADS]; /* side_360.c: the Share button's mic mute */

/* micdsp_360.c: the mic's chunking, ramps and the volume / gate / limiter state */
#define XD_FADE_DIV 100  /* 10 ms: rate / 100 samples (the tone's fades, the mic's ramps) */
/* 2 ms: the gate opening (rate / 500 samples). With the 10 ms of XD_FADE_DIV a word's first
 * 5 ms came through at under half level; a gate's attack is usually 1 ms or less. Closing
 * keeps XD_FADE_DIV: it only happens after the hold, in a pause. */
#define XD_GATE_ATTACK_DIV 500
#define XD_Q12 4096         /* the mic's volume in effect: Q12 (4096 = 100 %, <= 22937) */
#define XD_LIMIT 29204      /* the mic's peak limiter: -1 dBFS */
#define XD_CHUNK 32         /* mic frames handled at a time */

/* the mic on its way out (the 360's, the mic test's recording): zero = a fresh state, which
 * fades in over 10 ms. gain: MIC VOLUME in effect, Q12, ramping; shut: how far the gate is
 * closed, Q15 (0 = open), ramping; lim: the limiter's factor, Q12 (0 = not started: 4096) */
struct xd_shape {
	u32 gain, shut, lim, rel;
};
/* zero = a fresh state. closed / hold: the gate's decision (mic_analyse); opening: the window
 * under way has already reached the threshold (the gate passes it before the window ends);
 * sum / n: the 20 ms window; vu, vu_ms: the meter; dc / dc_on: the DC tracker (Q12) and
 * whether it has started */
struct xd_mic {
	struct xd_shape sh;
	u32 closed, hold, n;
	u64 sum;
	u32 vu;
	u64 vu_ms;
	s32 dc;
	u32 dc_on;
	u32 opening;
};
extern struct xd_mic g_dmic[PADS];

/* the gate shuts this mic now: closed at the last window, and the window under way has not
 * reached the threshold yet (what the 360's mic and the mic test's playback follow) */
static __attribute__((unused)) bool gate_shut(const struct xd_mic *m)
{
	return m->closed && !m->opening;
}

/* ---- the noise gate's steps, shared by the live mic (micdsp_360.c mic_analyse) and the mic
 * test's playback (mictest_360.c), so a clip played back is gated exactly as teammates get
 * it. Per chunk: gate_chunk_start, gate_push for each sample (at a window's end the caller
 * reads m->sum / m->n, then gate_window_end), gate_chunk_end; then shape() each sample with
 * shut_to = gate_shut(m) ? 32768 : 0. thr: RMS (full scale 32768), 0 = no gate. */
#define XD_GATE_WIN_DIV 50 /* 20 ms windows (the gate and the meter) */
/* The gate stays open this long after the last window at the threshold. 200 ms shut it in
 * the short dips inside a sentence (the owner's reading at -45: 13 closures in 30 s, 7 of
 * them gaps of 20-120 ms); 400 ms bridges them (tools/micdsp/README.md section 7). Longer
 * lets more room noise through after each phrase. */
#define XD_GATE_HOLD_MS 400

static __attribute__((unused)) u32 gate_win(u32 rate)
{
	return rate / XD_GATE_WIN_DIV ? rate / XD_GATE_WIN_DIV : 1;
}

static __attribute__((unused)) void gate_chunk_start(struct xd_mic *m, u32 thr)
{
	if (!thr)
		m->closed = m->hold = m->opening = 0;
}

/* one sample into the window; true when it ends one */
static __attribute__((unused)) bool gate_push(struct xd_mic *m, s32 x, u32 win)
{
	m->sum += (u32)(x * x);
	if (m->hold)
		m->hold--;
	return ++m->n >= win;
}

/* a window's end (its RMS): open at the threshold and held XD_GATE_HOLD_MS, shut once the
 * hold has run out; the window restarts. True: it opened a shut gate. */
static __attribute__((unused)) bool gate_window_end(struct xd_mic *m, u32 rms, u32 thr,
						    u32 rate)
{
	bool opened = false;

	m->opening = 0; /* this window's end decides, as it always did */
	if (thr && rms >= thr) {
		opened = m->closed != 0;
		m->closed = 0;
		m->hold = rate / 1000 * XD_GATE_HOLD_MS;
	} else if (thr && !m->hold) {
		m->closed = 1;
	}
	m->sum = 0;
	m->n = 0;
	return opened;
}

/* a chunk's end: opening early (micdsp_360.c mic_analyse says why). thr: the threshold the
 * window under way will be judged by. */
static __attribute__((unused)) void gate_chunk_end(struct xd_mic *m, u32 thr, u32 win)
{
	if (thr && m->closed && !m->opening && m->sum >= (u64)thr * thr * win)
		m->opening = 1;
}
extern const u16 XD_MIC_GAINS[XD_MIC_LEVELS]; /* MIC VOLUME 1..10 in per cent */

/* ---- helpers (from voice_360.c) ---- */

static __attribute__((unused)) u32 gcd(u32 a, u32 b)
{
	while (b) {
		u32 t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static __attribute__((unused)) bool rate_ok(u32 hz)
{
	return hz >= 4000 && hz <= 48000 && hz % 1000 == 0;
}

static __attribute__((unused)) void rs_init(struct rs *r, u32 in_hz, u32 out_hz)
{
	u32 g = gcd(in_hz, out_hz);

	memset(r, 0, sizeof(*r));
	r->in = in_hz / g;
	r->out = out_hz / g;
	r->n = r->in > r->out ? (r->in + r->out - 1) / r->out : 1;
	if (r->n > 8)
		r->n = 8;
}

/* one input sample in; 0..12 output samples to y (rates are multiples of 1000 in
 * 4000..48000, so in/out reduce to at most 48 and the products stay in 32 bits) */
static __attribute__((unused)) int rs_push(struct rs *r, s32 x, s16 *y)
{
	int k = 0;

	if (r->n > 1) {
		r->sum += x - r->box[r->bi];
		r->box[r->bi] = x;
		if (++r->bi == r->n)
			r->bi = 0;
		x = r->sum / (s32)r->n;
	}
	while (r->t < r->out) {
		y[k++] = clamp16(r->prev + (x - r->prev) * (s32)r->t / (s32)r->out);
		r->t += r->in;
	}
	r->t -= r->out;
	r->prev = x;
	return k;
}

static __attribute__((unused)) bool hs_format_ok(const struct xv_pad *p)
{
	return p->usb_present && rate_ok(p->rate_in) && rate_ok(p->rate_out) && p->ch_in >= 1 &&
	       p->ch_in <= 2 && p->ch_out >= 1 && p->ch_out <= 2;
}

static __attribute__((unused)) u32 ramp_to(u32 v, u32 to, u32 step)
{
	if (v < to)
		return to - v > step ? v + step : to;
	return v - to > step ? v - step : to;
}

/* one mic sample (after mic_clean) through MIC VOLUME, the gate (closed: shut_to 32768)
 * and the peak limiter, the ramps advanced by a sample (rate: the mic's): the volume and the
 * gate closing over 10 ms, the gate opening over 2 ms (only while it moves: no division a
 * sample otherwise) */
static __attribute__((unused)) s16 shape(struct xd_shape *sh, s32 x, u32 gain_to, u32 shut_to, u32 rate)
{
	u32 ramp = rate / XD_FADE_DIV ? rate / XD_FADE_DIV : 1;
	u32 g;
	s32 y, a;

	if (!sh->lim)
		sh->lim = XD_Q12;
	if (sh->shut > shut_to) {
		u32 att = rate / XD_GATE_ATTACK_DIV ? rate / XD_GATE_ATTACK_DIV : 1;

		sh->shut = ramp_to(sh->shut, shut_to, (32768 + att - 1) / att);
	} else if (sh->shut < shut_to) {
		sh->shut = ramp_to(sh->shut, shut_to, (32768 + ramp - 1) / ramp);
	}
	sh->gain = ramp_to(sh->gain, gain_to, (XD_Q12 * 6 + ramp - 1) / ramp);
	g = sh->gain * (32768 - sh->shut) >> 15;     /* Q12, <= 22937 */
	y = x * (s32)g >> 12;                         /* |y| <= 32768 x 5.6 */
	a = y < 0 ? -y : y;
	/* the limiter: a peak over XD_LIMIT pulls the factor down at once (no overshoot), and
	 * it comes back by ~1/256 every 8 samples (~85 ms at 24 kHz) */
	if ((u32)a * sh->lim >> 12 > XD_LIMIT) {
		sh->lim = (u32)XD_LIMIT * XD_Q12 / (u32)a;
	} else if (sh->lim < XD_Q12 && !(++sh->rel & 7)) {
		sh->lim += ((XD_Q12 - sh->lim) >> 8) + 1;
		if (sh->lim > XD_Q12)
			sh->lim = XD_Q12;
	}
	return clamp16(y * (s32)sh->lim >> 12);
}

/* ---- across the files ---- */

/* voice_360.c */
void mic_feed(int i, struct rs *r, const u8 *d, int frames, u32 ch);
/* the 360-rate capture path: n samples (already at g_xv's capture rate, mono) into pad's
 * capture packets; what does not fit is dropped (counted). For MIC-AUTO's decimator. */
void xv_cap_feed(int pad, const s16 *s, int n);

/* side_360.c */
void hs_headset_lost(int pad);
void hs_format(int pad);
void st_mic(int pad, const u8 *d, int frames, u32 ch);
void st_mix(int pad, u8 *out, int len, u32 master);
void st_dev_watch(int pad, u32 rms, u32 win_ms);
void tick_mix(int pad, u8 *out, int len, u32 master);
void scale(u8 *out, int len, u32 gain);

/* micdsp_360.c */
void xd_reset_pad(int pad);
bool tone_active(int pad);
void tone_mix(int pad, u8 *out, int len, u32 master);
u32 gain_index(u32 pct);
/* MIC VOLUME in effect for pad slot `pad` as the Q12 target of the 10 ms ramp (<= 22937):
 * MIC VOLUME AUTO's gain while it is in effect (it is per player), else the manual step.
 * pad -1: the pad micdsp_360.c shows in its status (the first headset). */
u32 mic_gain_target(int pad);
void mic_clean(int pad, const u8 *d, int frames, u32 ch, u8 *out);
void mic_analyse(int pad, const u8 *d, int frames);
/* the noise gate's threshold in effect now for pad slot `pad` (RMS, 0 = gate off): the
 * step's, or NOISE GATE AUTO's of the moment for that pad */
u32 mic_gate_threshold(int pad);
/* MIC VOLUME, the gate and the limiter: frames (<= XD_CHUNK) of mono s16le into out */
void mic_shape(int pad, const u8 *d, int frames, u8 *out);
/* the capture path's decimator hook: shaped mic, or silence while muted (mono s16le) */
void mic_to_360(int pad, const u8 *d, int frames);

/* mictest_360.c */
void mt_record(int pad, const u8 *d, int frames);
void mt_mix(int pad, u8 *out, int len, u32 master);
void mt_pad_gone(int pad, const char *why);
void xd_mictest_quiesce(void);
void mt_poll(void);

#endif
