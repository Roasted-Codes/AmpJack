// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The PC bridge's arithmetic (bridge_360.c; tests/test_bridge.c runs it on the Mac): the
 * jitter buffer for a stream from the PC, the ring for a stream to the PC, and the mix into
 * a block of s16le samples. Integer only, no globals: every function works on the struct it
 * is given, so the host test drives exactly the code the console runs.
 *
 * Threads (on the console): a jitter buffer has one writer (the bridge's net thread, xb_jb_put)
 * and one reader (the driver thread, xb_jb_read); a send ring has one writer (the driver
 * thread, xb_txr_push) and one reader (the net thread, xb_txr_take). Each shared word has one
 * writer; positions are published after the samples they cover (XB_BARRIER) and every index is
 * masked, so a race costs at most a glitch, never memory outside a ring.
 *
 * The jitter buffer (as team_360.c's talkers, with game_360.c's drift trim):
 *   - positions are frames of the sender's stream: a packet's timestamp minus a base set at
 *     the stream's first packet. A gap (lost packets) is filled with the last audio fading to
 *     silence over 5 ms, and the packet after it fades in, so a loss is a dip, not a click; a
 *     late packet that is not played yet replaces that fill; one already played is dropped.
 *   - a new format, a jump of more than 200 ms, or a ring nobody reads (no headset) starts the
 *     stream over (a new generation the reader picks up).
 *   - the reader waits for `target` ms, then plays through a linear resampler from the
 *     stream's rate to the output's (a 2-tap pre-average when it halves the rate or more). Its
 *     ratio is trimmed by up to 1000 ppm so the fill, averaged over about half a second, stays
 *     at the target: the PC's clock and the headset's are different clocks.
 *   - empty: the last output fades to silence over 5 ms (an underrun, unless the PC stopped
 *     sending), then it waits for the target again and fades in over 5 ms.
 */
#ifndef BRIDGE_DSP_H
#define BRIDGE_DSP_H

#include "../core/compat.h"

#ifdef __PPC__
#define XB_BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#else
#define XB_BARRIER() __sync_synchronize()
#endif

#define XB_ONE (1u << 20)          /* resampler phase, Q20 */
#define XB_DRIFT_MAX_PPM 1000
#define XB_FADE_DIV 200            /* fades: rate / 200 frames = 5 ms */
#define XB_HIGH_OVER_MS 80         /* this far above the target: skip back to it */
#define XB_RESTART_MS 200          /* a timestamp jump larger than this starts over */
#define XB_STOPPED_MS 250          /* no packet this long: the PC stopped (not an underrun). Well
                                    * above the console's net thread pass (one per video frame).
                                    * Compared signed: the reader's clock may lag the writer's */
#define XB_GUARD 512               /* frames between the reader and anything the writer writes */
#define XB_AVG_DIV 125             /* the fill is sampled once per rate / 125 frames (8 ms) */
#define XB_MAX_BLOCK 512           /* output frames per xb_jb_read: frames x step fits 32 bits */

enum { XB_IDLE, XB_BUFFERING, XB_RUNNING };

static __attribute__((unused)) s32 xb_clamp16(s32 v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static __attribute__((unused)) u32 xb_abs(s32 v)
{
	return v < 0 ? (u32)-v : (u32)v;
}

static __attribute__((unused)) s32 xb_get_be16(const u8 *p)
{
	return (s16)(u16)(p[0] << 8 | p[1]);
}

static __attribute__((unused)) void xb_put_be16(u8 *p, s32 v)
{
	p[0] = (u8)((u32)v >> 8);
	p[1] = (u8)v;
}

/* s16 little-endian load and store: one byte-reversed halfword access on the console (lhbrx /
 * sthbrx) instead of two byte accesses and a shift */
static inline s32 xb_ld_le16(const u8 *p)
{
	u16 v;

	memcpy(&v, p, 2);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	v = __builtin_bswap16(v);
#endif
	return (s16)v;
}

static inline void xb_st_le16(u8 *p, s32 x)
{
	u16 v = (u16)x;

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	v = __builtin_bswap16(v);
#endif
	memcpy(p, &v, 2);
}

/* a rate the bridge takes: 8000..48000 Hz in steps of 1000 (the resampler's step is in kHz) */
static __attribute__((unused)) bool xb_rate_ok(u32 hz)
{
	return hz >= 8000 && hz <= 48000 && hz % 1000 == 0;
}

/* ---- a stream from the PC ------------------------------------------------------------------ */

struct xb_jb {
	/* set once (xb_jb_init) */
	s16 *ring;               /* cap frames of ch samples */
	u32 cap, ch;             /* cap: a power of two; ch: what the reader gets (1 or 2) */
	/* writer */
	volatile u32 gen;        /* a new stream: the reader starts over at `start` */
	volatile u32 start;      /* this generation's first position */
	volatile u32 hi;         /* the position after the last frame written */
	volatile u32 rate;       /* this generation's rate (Hz) */
	volatile u32 ended;      /* the PC said bye or went away: play out, then idle */
	volatile u32 last_ms;    /* when the last packet came (the writer's clock) */
	u32 base, in_ch, have, seq_next;
	u32 gap_from;            /* the end of the real audio before the last filled gap (0 none) */
	u32 packets, lost, late, recovered, restarts;  /* the writer's counters */
	/* reader */
	volatile u32 rgen;       /* the generation it reads */
	volatile u32 rpos;       /* the next position it reads (published after each call) */
	u32 state, frac, fade, fadeout, fade_n, pending, out_rate;
	s32 a[2], b[2], pv[2], o[2];
	s32 avg_q4, ppm;
	u32 acc_fill, acc_n;
	/* the reader's per-format values, worked out once (divisions are slow on the console) */
	u32 c_rate, c_out, c_tms, c_khz, c_step, c_stepp, c_target, c_high, c_full, c_avgn;
	u32 underruns, overruns, starts, fill_ms, peak; /* the reader's counters */
};

/* ring: cap x ch s16 (cap a power of two, at least 4 x XB_GUARD) */
static __attribute__((unused)) void xb_jb_init(struct xb_jb *j, s16 *ring, u32 cap, u32 ch)
{
	memset(j, 0, sizeof(*j));
	j->ring = ring;
	j->cap = cap;
	j->ch = ch;
	j->ended = 1;
}

/* n frames of in_ch big-endian s16 into the ring at pos, as the ring's channels; gain Q8 ramps
 * from g0 to g1 over the n frames (256 = as is) */
static __attribute__((unused)) void xb_jb_write(struct xb_jb *j, u32 pos, const u8 *be, u32 n,
						u32 in_ch, s32 g0, s32 g1)
{
	for (u32 f = 0; f < n; f++, be += 2 * in_ch) {
		s16 *d = &j->ring[((pos + f) & (j->cap - 1)) * j->ch];
		s32 l = xb_get_be16(be), r = in_ch == 2 ? xb_get_be16(be + 2) : l;
		s32 g = g0 == g1 ? g0 : g0 + (g1 - g0) * (s32)f / (s32)n;

		if (g != 256) {
			l = l * g >> 8;
			r = r * g >> 8;
		}
		if (j->ch == 2) {
			d[0] = (s16)l;
			d[1] = (s16)r;
		} else {
			d[0] = (s16)((l + r) >> 1);
		}
	}
}

/* the gap [from, to) filled with the last frame written, fading to silence over rate /
 * XB_FADE_DIV frames (then silence): the level moves on from where the audio stopped, with no
 * step, and the packet after the gap fades in from silence */
static void xb_jb_conceal(struct xb_jb *j, u32 from, u32 to)
{
	u32 fade = j->rate / XB_FADE_DIV;
	const s16 *s = &j->ring[((from - 1) & (j->cap - 1)) * j->ch];
	s32 v[2] = { s[0], j->ch == 2 ? s[1] : 0 };

	for (u32 at = from, k = 0; at != to; at++, k++) {
		s16 *d = &j->ring[(at & (j->cap - 1)) * j->ch];
		s32 g = k < fade ? (s32)(fade - k - 1) * 256 / (s32)fade : 0;

		for (u32 c = 0; c < j->ch; c++)
			d[c] = (s16)(v[c] * g >> 8);
	}
}

/* n frames at pos, fading in over the first m and out over the last m (m: 5 ms, at most n / 2):
 * a packet dropped into a filled gap meets silence at both ends */
static void xb_jb_write_faded(struct xb_jb *j, u32 pos, const u8 *be, u32 n, u32 in_ch,
			      bool in, bool out)
{
	u32 m = j->rate / XB_FADE_DIV, mid;

	if (m > n / 2)
		m = n / 2;
	mid = n - (in ? m : 0) - (out ? m : 0);
	if (in)
		xb_jb_write(j, pos, be, m, in_ch, 0, 256);
	xb_jb_write(j, pos + (in ? m : 0), be + 2 * in_ch * (in ? m : 0), mid, in_ch, 256, 256);
	if (out)
		xb_jb_write(j, pos + n - m, be + 2 * in_ch * (n - m), m, in_ch, 256, 0);
}

static void xb_jb_restart(struct xb_jb *j, u32 ts, u32 rate, u32 in_ch, const u8 *be, u32 n)
{
	u32 start = j->hi + j->cap; /* well past anything the reader may still be on */

	j->rate = rate;
	j->in_ch = in_ch;
	j->base = ts - start;
	xb_jb_write(j, start, be, n, in_ch, 0, 256); /* from silence */
	j->start = start;
	j->hi = start + n;
	j->ended = 0;
	j->have = 1;
	j->gap_from = 0;
	XB_BARRIER(); /* samples and positions before the generation that publishes them */
	j->gen++;
	j->restarts++;
}

/* Writer: one audio packet (seq, its first frame's timestamp, its format, n frames of in_ch
 * big-endian s16). The caller has checked xb_rate_ok, in_ch 1..2 and the length. */
static __attribute__((unused)) void xb_jb_put(struct xb_jb *j, u32 seq, u32 ts, u32 rate,
					      u32 in_ch, const u8 *be, u32 n, u32 now_ms)
{
	u32 pos, rd, lim = j->cap - XB_GUARD;
	s32 d, gap_max = (s32)(rate / 1000 * XB_RESTART_MS);
	s32 ds = (s32)(seq - j->seq_next);

	j->packets++;
	if (!j->have || ds > 1000 || ds < -1000) {
		j->seq_next = seq + 1; /* the first packet, or the PC started a new sequence */
	} else if (ds >= 0) {
		j->lost += (u32)ds;    /* packets the sequence skipped */
		j->seq_next = seq + 1;
	} else if (j->lost) {
		j->lost--;             /* an earlier packet after all: reordered, not lost */
	}
	if (n > lim / 2)
		return; /* can't happen with the sizes the caller allows */
	if (!j->have || j->ended || rate != j->rate || in_ch != j->in_ch ||
	    (s32)(now_ms - j->last_ms) > XB_STOPPED_MS) {
		j->last_ms = now_ms;
		xb_jb_restart(j, ts, rate, in_ch, be, n);
		return;
	}
	j->last_ms = now_ms;
	pos = ts - j->base;
	d = (s32)(pos - j->hi);
	rd = j->rgen == j->gen ? j->rpos : j->start; /* where the reader is, or will start */
	if (d > gap_max || d < -gap_max || (s32)(pos + n - rd) > (s32)lim ||
	    (s32)(j->hi - rd) > (s32)lim) {
		/* a jump, or nobody reads (no headset): start over at this packet */
		xb_jb_restart(j, ts, rate, in_ch, be, n);
		return;
	}
	if (d >= 0) {
		if (d > 0) {
			xb_jb_conceal(j, j->hi, pos); /* lost packets: a fade to silence */
			j->gap_from = j->hi;
		}
		/* after a gap the packet fades in */
		xb_jb_write_faded(j, pos, be, n, in_ch, d > 0, false);
		XB_BARRIER(); /* the samples before the position that publishes them */
		j->hi = pos + n;
	} else if ((s32)(pos - rd) >= XB_GUARD) {
		/* late, not played yet: the real samples replace part of the fill. Right where the
		 * audio broke off it continues it; elsewhere it fades in from the fill's silence.
		 * It fades out to meet what follows (silence, or a packet that faded in). */
		xb_jb_write_faded(j, pos, be, n, in_ch, pos != j->gap_from, true);
		j->gap_from = 0;
		j->recovered++;
		return;
	} else {
		j->late++;
		return;
	}
}

/* the PC is gone (bye, or no keep-alive): the reader plays out what it has, then idles */
static __attribute__((unused)) void xb_jb_end(struct xb_jb *j)
{
	j->ended = 1;
}

/* one input frame into b (a moves up), with the pre-average when decimating */
static void xb_jb_pop(struct xb_jb *j, u32 *rpos, bool filter)
{
	const s16 *s = &j->ring[((*rpos)++ & (j->cap - 1)) * j->ch];

	for (u32 c = 0; c < j->ch; c++) {
		s32 y = s[c];

		if (filter) {
			s32 fy = (y + j->pv[c]) >> 1;

			j->pv[c] = y;
			y = fy;
		}
		j->a[c] = j->b[c];
		j->b[c] = y;
	}
}

/* frames of the fade-out of the last output (or silence) into out */
static void xb_jb_tail(struct xb_jb *j, s32 *out, u32 frames)
{
	u32 ch = j->ch, fo = j->fadeout, fn = j->fade_n;
	s32 o0 = j->o[0], o1 = j->o[1];

	for (u32 f = 0; f < frames; f++, out += ch) {
		s32 v0 = 0, v1 = 0;

		if (fo) {
			v0 = o0 * (s32)fo / (s32)fn;
			v1 = o1 * (s32)fo / (s32)fn;
			fo--;
		}
		out[0] = v0;
		if (ch == 2)
			out[1] = v1;
	}
	j->fadeout = fo;
	if (!fo)
		j->o[0] = j->o[1] = 0;
}

static void xb_jb_stop(struct xb_jb *j, bool underrun, u32 now_ms)
{
	if (j->state == XB_RUNNING) {
		j->fadeout = j->fade_n; /* from the last output, over 5 ms */
		if (underrun && !j->ended && (s32)(now_ms - j->last_ms) <= XB_STOPPED_MS)
			j->pending = 1; /* counted once it is clear the PC is still sending */
	}
	j->state = XB_BUFFERING;
}

/* the values that depend only on the formats and the target */
static void xb_jb_format(struct xb_jb *j, u32 rate, u32 rate_out, u32 target_ms)
{
	j->c_rate = rate;
	j->c_out = rate_out;
	j->c_tms = target_ms;
	j->c_khz = rate / 1000;
	j->c_step = (j->c_khz << 20) / (rate_out / 1000); /* input frames per output frame, Q20 */
	j->c_stepp = (u32)((s32)j->c_step + (s32)(j->c_step >> 10) * j->ppm / 977);
	j->c_target = j->c_khz * target_ms;
	j->c_high = j->c_target + j->c_khz * XB_HIGH_OVER_MS;
	j->c_full = j->c_target / 3 ? j->c_target / 3 : 1;
	j->c_avgn = rate_out / XB_AVG_DIV;
	j->fade_n = rate_out / XB_FADE_DIV;
	if (j->fadeout > j->fade_n)
		j->fadeout = j->fade_n;
}

/* Reader: `frames` output frames (ch samples each, s16 values in s32) at rate_out into out.
 * target_ms: the fill to keep. Returns false when everything written is silence (the caller
 * then mixes nothing). rate_out: xb_rate_ok; frames <= XB_MAX_BLOCK (the caller splits).
 * The loop keeps the state in locals: stores to `out` would otherwise make the compiler reload
 * every field after each one (a load-hit-store stall per sample on the console). */
static __attribute__((unused)) bool xb_jb_read(struct xb_jb *j, s32 *out, u32 frames,
					       u32 rate_out, u32 target_ms, u32 now_ms)
{
	u32 gen = j->gen, rpos, hi, avail, target, step, need, rate;
	bool filter;

	XB_BARRIER(); /* the generation before what it covers */
	if (gen != j->rgen) {
		xb_jb_stop(j, false, now_ms); /* a new stream: the old one fades out first */
		j->rgen = gen;
		j->rpos = j->start;
		j->pending = 0;
		j->acc_fill = j->acc_n = 0;
	}
	if (j->state == XB_IDLE && !j->fadeout)
		return false;
	rate = j->rate;
	rpos = j->rpos;
	hi = j->hi;
	XB_BARRIER(); /* the position before the samples it covers */
	if (!xb_rate_ok(rate)) {
		xb_jb_tail(j, out, frames);
		j->state = XB_IDLE;
		return true;
	}
	if (rate != j->c_rate || rate_out != j->c_out || target_ms != j->c_tms)
		xb_jb_format(j, rate, rate_out, target_ms);
	avail = (s32)(hi - rpos) > 0 ? hi - rpos : 0;
	target = j->c_target;
	step = j->c_step;
	filter = step >= XB_ONE + XB_ONE / 2;
	if (j->state != XB_RUNNING) {
		bool stopped = j->ended || (s32)(now_ms - j->last_ms) > XB_STOPPED_MS;

		if (avail < target + 2 || stopped) {
			bool any = j->fadeout != 0;

			if (stopped) {
				j->state = XB_IDLE;
				j->pending = 0;
			}
			xb_jb_tail(j, out, frames);
			return any; /* the fade's last frames, or nothing to mix */
		}
		if (j->pending) /* it ran dry while packets kept coming */
			j->underruns++;
		j->pending = 0;
		if (avail > target + 2 + rate / 100)
			rpos = hi - target - 2; /* far more than the target waited: start at it */
		j->pv[0] = j->pv[1] = 0;
		xb_jb_pop(j, &rpos, filter);
		xb_jb_pop(j, &rpos, filter);
		j->frac = 0;
		j->fade = 0;
		j->fadeout = 0;
		j->avg_q4 = (s32)(target << 4);
		j->acc_fill = j->acc_n = 0;
		j->state = XB_RUNNING;
		j->starts++;
		avail = hi - rpos;
	}
	if (avail > j->c_high) {
		/* far too full (the reader was held up, or a burst): back to the target */
		j->overruns++;
		rpos = hi - target;
		avail = target;
	}
	/* the drift trim (game_360.c's): the fill, sampled per 8 ms of output and averaged over
	 * ~0.5 s, steers the ratio by at most 1000 ppm; full trim at a third of the target off */
	j->acc_fill += avail * frames;
	j->acc_n += frames;
	if (j->acc_n >= j->c_avgn) {
		s32 fill = (s32)(j->acc_fill / j->acc_n), err, ppm;

		j->acc_fill = j->acc_n = 0;
		j->avg_q4 += ((fill << 4) - j->avg_q4) >> 6;
		err = (j->avg_q4 >> 4) - (s32)target;
		ppm = err * XB_DRIFT_MAX_PPM / (s32)j->c_full;
		j->ppm = ppm > XB_DRIFT_MAX_PPM ? XB_DRIFT_MAX_PPM
			 : ppm < -XB_DRIFT_MAX_PPM ? -XB_DRIFT_MAX_PPM : ppm;
		j->c_stepp = (u32)((s32)step + (s32)(step >> 10) * j->ppm / 977);
		j->fill_ms = (u32)(j->avg_q4 >> 4) / j->c_khz;
	}
	step = j->c_stepp;
	need = (j->frac + frames * step) >> 20;
	if (avail < need) {
		/* empty: the PC stopped, or its packets are late */
		xb_jb_stop(j, true, now_ms);
		xb_jb_tail(j, out, frames);
		j->rpos = rpos;
		return true;
	}
	{
		const s16 *ring = j->ring;
		u32 mask = j->cap - 1, frac = j->frac, fade = j->fade, fn = j->fade_n, peak = j->peak;
		s32 a0 = j->a[0], a1 = j->a[1], b0 = j->b[0], b1 = j->b[1];
		s32 p0 = j->pv[0], p1 = j->pv[1], x0 = 0, x1 = 0;
		bool st = j->ch == 2;

		for (u32 f = 0; f < frames; f++) {
			s32 t = (s32)(frac >> 6); /* Q14 */

			x0 = a0 + (((b0 - a0) * t) >> 14);
			x1 = st ? a1 + (((b1 - a1) * t) >> 14) : 0;
			if (fade < fn) {
				x0 = x0 * (s32)fade / (s32)fn;
				x1 = x1 * (s32)fade / (s32)fn;
				fade++;
			}
			if (xb_abs(x0) > peak)
				peak = xb_abs(x0);
			if (st) {
				if (xb_abs(x1) > peak)
					peak = xb_abs(x1);
				out[2 * f] = x0;
				out[2 * f + 1] = x1;
			} else {
				out[f] = x0;
			}
			frac += step;
			while (frac >= XB_ONE) {
				const s16 *sp = &ring[(rpos++ & mask) << st];
				s32 y0 = sp[0], y1 = st ? sp[1] : 0;

				frac -= XB_ONE;
				if (filter) { /* decimating: a 2-tap average first */
					s32 f0 = (y0 + p0) >> 1, f1 = (y1 + p1) >> 1;

					p0 = y0;
					p1 = y1;
					y0 = f0;
					y1 = f1;
				}
				a0 = b0;
				a1 = b1;
				b0 = y0;
				b1 = y1;
			}
		}
		j->frac = frac;
		j->fade = fade;
		j->peak = peak;
		j->a[0] = a0;
		j->a[1] = a1;
		j->b[0] = b0;
		j->b[1] = b1;
		j->pv[0] = p0;
		j->pv[1] = p1;
		j->o[0] = x0;
		j->o[1] = x1;
	}
	XB_BARRIER(); /* done reading the slots before the writer may reuse them */
	j->rpos = rpos;
	return true;
}

/* ---- the mix -------------------------------------------------------------------------------- */

/* src (frames x src_ch, s16 values in s32) at a gain ramping from g0 to g1 (Q8, <= 256) over the
 * block, added to frames of s16le out (ch_out 1 or 2), every sum clamped; returns the peak of
 * what was added */
static __attribute__((unused)) u32 xb_mix_into(u8 *out, u32 frames, u32 ch_out, const s32 *src,
					       u32 src_ch, s32 g0, s32 g1)
{
	u32 peak = 0;

	for (u32 f = 0; f < frames; f++, out += 2 * ch_out) {
		s32 g = g0 == g1 ? g0 : g0 + (g1 - g0) * (s32)f / (s32)frames;
		s32 l = src[f * src_ch], r = src_ch == 2 ? src[f * src_ch + 1] : l;

		l = xb_clamp16(l) * g >> 8;
		r = xb_clamp16(r) * g >> 8;
		if (xb_abs(l) > peak)
			peak = xb_abs(l);
		if (xb_abs(r) > peak)
			peak = xb_abs(r);
		if (ch_out == 1) {
			xb_st_le16(out, xb_clamp16(xb_ld_le16(out) + ((l + r) >> 1)));
		} else {
			/* both loads before either store: a load just behind a store to the same
			 * doubleword stalls the Xenon for dozens of cycles (load-hit-store) */
			s32 ol = xb_ld_le16(out), orr = xb_ld_le16(out + 2);

			xb_st_le16(out, xb_clamp16(ol + l));
			xb_st_le16(out + 2, xb_clamp16(orr + r));
		}
	}
	return peak;
}

/* ---- a stream to the PC --------------------------------------------------------------------- */

struct xb_txr {
	s16 *buf;                /* cap mono samples */
	u32 cap;                 /* a power of two */
	volatile u32 w;          /* writer: samples written (the stream position) */
	volatile u32 r;          /* reader: samples taken */
	volatile u32 rate;       /* writer: the rate of what it writes */
	u32 skipped;             /* reader: samples lost because it fell behind */
	u32 peak;                /* writer */
};

static __attribute__((unused)) void xb_txr_init(struct xb_txr *t, s16 *buf, u32 cap)
{
	memset(t, 0, sizeof(*t));
	t->buf = buf;
	t->cap = cap;
}

/* Writer: frames samples of s16le, `stride` bytes apart (2 = mono, 4 = the left of stereo) */
static __attribute__((unused)) void xb_txr_push(struct xb_txr *t, const u8 *le, u32 frames,
						u32 stride, u32 rate)
{
	u32 w = t->w, peak = t->peak, mask = t->cap - 1;
	s16 *buf = t->buf;

	if (rate != t->rate)
		t->rate = rate;
	for (u32 f = 0; f < frames; f++, le += stride) {
		s32 v = xb_ld_le16(le);

		if (xb_abs(v) > peak)
			peak = xb_abs(v);
		buf[(w + f) & mask] = (s16)v;
	}
	t->peak = peak;
	XB_BARRIER(); /* the samples before the position that publishes them */
	t->w = w + frames;
}

/* Reader: starts at what is written now (a new session hears nothing old) */
static __attribute__((unused)) void xb_txr_reset(struct xb_txr *t)
{
	t->r = t->w;
}

/* Reader: up to max samples as big-endian s16 into be; *ts gets the stream position of the
 * first. Fell more than half the ring behind: skips to the newest half (counted; the gap shows
 * in the timestamps). Returns the count (0: nothing new). */
static __attribute__((unused)) u32 xb_txr_take(struct xb_txr *t, u8 *be, u32 max, u32 *ts)
{
	u32 w = t->w, r = t->r, n;

	XB_BARRIER(); /* the position before the samples it covers */
	if (w - r > t->cap / 2) {
		t->skipped += w - r - t->cap / 4;
		r = w - t->cap / 4;
	}
	n = w - r < max ? w - r : max;
	for (u32 i = 0; i < n; i++)
		xb_put_be16(be + 2 * i, t->buf[(r + i) & (t->cap - 1)]);
	*ts = r;
	XB_BARRIER(); /* done reading before the slots are reused */
	t->r = r + n;
	return n;
}

#endif
