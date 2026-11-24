/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The equalizer's arithmetic and the xw.presets reader: pure integer C, no globals, no kernel
 * includes, so the plugin (eq_360.c) and the host test (tests/test_eq.c) run the same code.
 * tools/presets/eq_model.py is the bit-exact Python twin; change both together or
 * tests/test_eq.c fails its bit-exact check.
 *
 * Threads and locking: none here. Every function works only on what it is handed; the caller
 * owns the state and serialises calls per struct eqd_run.
 *
 * Signal path, per channel (interleaved s16 in, in place):
 *   x23 = (x16 * preamp_q15 + 64) >> 7          s16 -> Q23 (8 bits below the s16 LSB)
 *   per active band, direct form 1, Q3.28 coefficients, 64-bit accumulator:
 *     y = (b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 + 2^27) >> 28
 *   out16 = clamp((y23 + 128) >> 8)
 * The 8 extra bits keep the rounding noise that the low shelves' feedback amplifies (poles
 * close to z = 1) under the s16 LSB; s16 state would put it above. Costs nothing on the
 * 32-bit PowerPC build: a 32 x 32 -> 64-bit product is mullw + mulhw either way.
 *
 * Range proof (done per preset by tools/presets/convert.py, checked by tests/test_eq.c):
 * after the preamp no band's partial cascade has an L1 gain over 256 (48 dB), so a band
 * output always fits in s32 and every accumulator in 64 bits. Only the final store clamps.
 * A hand-made file with valid CRC but wild coefficients can wrap internally: loud noise, never
 * a crash (no index depends on the samples).
 *
 * No floating point anywhere: the plugin is built with -msoft-float and check_xex.py rejects
 * FP instructions.
 */
#pragma once

#include "../core/compat.h"

/* Out of line and built for size: code that runs once per call or once per preset change.
 * Inlined, the fixed-size state copies alone unroll into about 1 KB of stores. */
#if defined(__clang__)
#define EQD_COLD __attribute__((noinline, minsize))
#else
#define EQD_COLD __attribute__((noinline, cold))
#endif

#define EQD_BANDS 5
#define EQD_MAX_CH 2
#define EQD_RATE 48000
#define EQD_XFADE 480 /* frames: 10 ms at 48 kHz; shorter clicks on a busy mix */
/* frames the new preset runs unheard before its fade starts, so the fade blends two settled
 * filters: started from silence, an 80-100 Hz shelf's start-up transient is still about
 * -40 dB of the signal when the fade begins, and shows as a small step. 10 ms is 1.5 to
 * 4 time constants of the slowest bands (a Q = 10 peak at 500 Hz, the low shelves). */
#define EQD_WARM 480

/* xw.presets layout (big-endian): eq_360.h struct xq_file_* */
#define EQD_MAGIC 0x58575051u /* 'XWPQ' */
#define EQD_VERSION 2
#define EQD_VERSION_OLD 1 /* records without a description: eqd_check -6 */
#define EQD_HDR 32
#define EQD_REC 224
#define EQD_NAME 20
#define EQD_AUTHOR 16
#define EQD_DESC 64        /* 63 characters and a NUL */
#define EQD_TAIL_MAX 8192  /* bytes allowed after the records (the CUSTOM tables) */
/* the CUSTOM tables (eq_360.h struct xq_file_custom) */
#define EQD_CMAGIC 0x58575043u /* 'XWPC' */
#define EQD_CBANDS 3
#define EQD_CSTEPS 13
#define EQD_CMID 6          /* the index of step 0 */
#define EQD_CHDR 28
#define EQD_CPRE (EQD_CSTEPS * EQD_CSTEPS * EQD_CSTEPS)
#define EQD_CUSTOM_BYTES (EQD_CHDR + EQD_CBANDS * EQD_CSTEPS * 20 + EQD_CPRE * 2 + 2)

/* One preset as the kernel uses it (native byte order). */
struct eqd_bands {
	s32 c[EQD_BANDS][5]; /* b0 b1 b2 a1 a2, Q3.28, a0 = 1 */
	s32 preamp_q15;      /* <= 32768 */
	u32 active;          /* bit i: band i is not unity (unity bands are skipped, exactly) */
};

/* CUSTOM's tables as the kernel uses them (native byte order): every step of every control,
 * and the preamp of every combination. */
struct eqd_custom {
	s32 c[EQD_CBANDS][EQD_CSTEPS][5]; /* Q3.28; [k][EQD_CMID] is unity */
	u16 pre[EQD_CPRE];                /* Q15, 1..32768; index eqd_custom_index */
};

/* Per channel, per preset: direct form 1 history of every band. */
struct eqd_state {
	s32 h[EQD_BANDS][4]; /* x1 x2 y1 y2, Q23 */
};

/* One output stream: the preset in use, the one being faded to, and the next request.
 * NULL is FLAT. Zero it (memset) for FLAT with no history. */
struct eqd_run {
	struct eqd_state cur[EQD_MAX_CH], nxt[EQD_MAX_CH];
	const struct eqd_bands *pc;   /* playing */
	const struct eqd_bands *pn;   /* warming / fading in; valid while fading */
	const struct eqd_bands *want; /* latest request; applied at a block start */
	u32 fading;                   /* 1 while pc -> pn is in progress */
	s32 xpos;                     /* frames into the fade; -EQD_WARM..-1 = warming */
};

/* ---- file reading ---------------------------------------------------------------------- */

static inline u32 eqd_be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/* zlib / IEEE CRC-32, bitwise: the file is read once at init, a table would cost 1 KB. */
static inline u32 eqd_crc32(const u8 *p, u32 n)
{
	u32 c = 0xFFFFFFFFu;

	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
	}
	return ~c;
}

/* FNV-1a 32 of a NUL-terminated name: the preset's stable id (saved in xw.settings).
 * Renaming a preset changes its id, and a saved choice of it then loads as FLAT. */
static inline u32 eqd_fnv1a(const char *s)
{
	u32 h = 0x811C9DC5u;

	while (*s)
		h = (h ^ (u8)*s++) * 0x01000193u;
	return h;
}

/* Checks a whole xw.presets image; returns the preset count, or a negative reason:
 * -1 short, -2 magic / version, -3 rate / bands / record size, -4 length, -5 CRC, -6 an older
 * version (1). After the records up to EQD_TAIL_MAX bytes may follow (the CUSTOM tables,
 * checked on their own by eqd_custom_read): a newer table layout then costs CUSTOM only. */
static inline int eqd_check(const u8 *f, u32 len)
{
	if (len < EQD_HDR)
		return -1;
	if (eqd_be32(f) == EQD_MAGIC && eqd_be32(f + 4) == EQD_VERSION_OLD)
		return -6;
	if (eqd_be32(f) != EQD_MAGIC || eqd_be32(f + 4) != EQD_VERSION)
		return -2;
	if (eqd_be32(f + 8) != EQD_RATE || eqd_be32(f + 16) != EQD_REC ||
	    eqd_be32(f + 20) != EQD_BANDS)
		return -3;
	u32 count = eqd_be32(f + 12);
	if (count > 1000 || len < EQD_HDR + count * EQD_REC ||
	    len - (EQD_HDR + count * EQD_REC) > EQD_TAIL_MAX)
		return -4;
	if (eqd_crc32(f + EQD_HDR, len - EQD_HDR) != eqd_be32(f + 24))
		return -5;
	return (int)count;
}

/* FLAT's level while this file is loaded (the header's last word), Q15: the presets are
 * loudness-matched to it, so switching between FLAT and a preset does not jump in loudness.
 * 0 (a file without it) or anything above 32768 is 0 dB. */
static inline s32 eqd_flat_q15(const u8 *f)
{
	u32 v = eqd_be32(f + 28);

	return v == 0 || v > 32768 ? 32768 : (s32)v;
}

/* Decodes record i of a checked image. name / author get a terminating NUL (21 / 17 bytes);
 * desc (EQD_DESC bytes) always ends in one (its last byte is forced to NUL). gain_cdb: the
 * band gains in 1/100 dB, for status and the panel. Returns the id. */
static inline u32 eqd_record(const u8 *f, u32 i, struct eqd_bands *b, char *name,
			     char *author, char *desc, s16 *gain_cdb)
{
	const u8 *r = f + EQD_HDR + i * EQD_REC;

	memcpy(name, r + 4, EQD_NAME);
	name[EQD_NAME] = 0;
	memcpy(author, r + 24, EQD_AUTHOR);
	author[EQD_AUTHOR] = 0;
	memcpy(desc, r + 160, EQD_DESC);
	desc[EQD_DESC - 1] = 0;
	b->preamp_q15 = (s32)eqd_be32(r + 40);
	b->active = eqd_be32(r + 44) & ((1u << EQD_BANDS) - 1);
	for (int k = 0; k < EQD_BANDS; k++)
		for (int j = 0; j < 5; j++)
			b->c[k][j] = (s32)eqd_be32(r + 48 + 4 * (5 * k + j));
	for (int k = 0; k < EQD_BANDS; k++)
		gain_cdb[k] = (s16)(r[148 + 2 * k] << 8 | r[149 + 2 * k]);
	if (b->preamp_q15 > 32768 || b->preamp_q15 < 0)
		b->preamp_q15 = 32768;
	return eqd_be32(r);
}

/* ---- CUSTOM ------------------------------------------------------------------------------ */

/* steps -6..6 each -> the index into eqd_custom.pre */
static inline u32 eqd_custom_index(int b, int m, int t)
{
	return ((u32)(b + EQD_CMID) * EQD_CSTEPS + (u32)(m + EQD_CMID)) * EQD_CSTEPS +
	       (u32)(t + EQD_CMID);
}

/* The CUSTOM tables of a checked image (eqd_check gave count) into ct. 0, or why there is no
 * CUSTOM: -1 absent (nothing after the records), -2 magic / size / shape, -3 a value out of
 * range (a preamp outside 1..32768, step 0 not unity, 0 0 0 not at flat_q15: FLAT and
 * CUSTOM 0 0 0 must be the same sound). The presets do not depend on it. */
static inline EQD_COLD int eqd_custom_read(const u8 *f, u32 len, u32 count, s32 flat_q15,
					    struct eqd_custom *ct)
{
	u32 at = EQD_HDR + count * EQD_REC;
	const u8 *t = f + at;

	if (len == at)
		return -1;
	if (len - at != EQD_CUSTOM_BYTES || eqd_be32(t) != EQD_CMAGIC ||
	    eqd_be32(t + 4) != EQD_CUSTOM_BYTES || eqd_be32(t + 8) != EQD_CBANDS ||
	    eqd_be32(t + 12) != EQD_CSTEPS)
		return -2;
	t += EQD_CHDR;
	for (int k = 0; k < EQD_CBANDS; k++)
		for (int s = 0; s < EQD_CSTEPS; s++)
			for (int j = 0; j < 5; j++, t += 4)
				ct->c[k][s][j] = (s32)eqd_be32(t);
	for (u32 i = 0; i < EQD_CPRE; i++, t += 2) {
		ct->pre[i] = (u16)(t[0] << 8 | t[1]);
		if (ct->pre[i] == 0 || ct->pre[i] > 32768)
			return -3;
	}
	for (int k = 0; k < EQD_CBANDS; k++) {
		const s32 *u = ct->c[k][EQD_CMID];
		if (u[0] != 1 << 28 || u[1] || u[2] || u[3] || u[4])
			return -3;
	}
	if (ct->pre[eqd_custom_index(0, 0, 0)] != flat_q15)
		return -3;
	return 0;
}

/* The cascade of steps b m t (each -6..6, clamped by the caller): BASS, MIDS, TREBLE in
 * bands 0-2, a control at 0 inactive (skipped exactly), bands 3-4 unity and inactive. Pure:
 * out is the caller's (eq_360.c builds into a slot no stream is playing). */
static inline EQD_COLD void eqd_custom_bands(const struct eqd_custom *ct, int b, int m, int t,
					     struct eqd_bands *out)
{
	const int st[EQD_CBANDS] = { b, m, t };

	memset(out, 0, sizeof(*out));
	for (int k = 0; k < EQD_BANDS; k++)
		out->c[k][0] = 1 << 28;
	for (int k = 0; k < EQD_CBANDS; k++) {
		if (!st[k])
			continue;
		memcpy(out->c[k], ct->c[k][st[k] + EQD_CMID], sizeof(out->c[k]));
		out->active |= 1u << k;
	}
	out->preamp_q15 = ct->pre[eqd_custom_index(b, m, t)];
}

/* ---- the filter ------------------------------------------------------------------------ */

/* One channel of n frames: s16 at in[0], in[stride], ... -> Q23 in work[0..n-1].
 * b NULL (FLAT) gives x << 8 exactly. Band-outer, sample-inner, so a band's five
 * coefficients and four history words stay in registers for the whole block. Not inlined:
 * eqd_block calls it twice, and one copy saves image; a call per band-block costs nothing
 * next to the loop. */
static inline __attribute__((noinline)) void eqd_filter(const struct eqd_bands *b,
							struct eqd_state *st, const s16 *in,
							int stride, s32 *work, int n)
{
	s32 pre = b ? b->preamp_q15 : 32768;

	for (int i = 0; i < n; i++)
		work[i] = (in[i * stride] * pre + 64) >> 7;
	if (!b)
		return;
	for (int k = 0; k < EQD_BANDS; k++) {
		if (!(b->active & (1u << k)))
			continue;
		const s32 *c = b->c[k];
		int64_t b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
		s32 x1 = st->h[k][0], x2 = st->h[k][1], y1 = st->h[k][2], y2 = st->h[k][3];

		for (int i = 0; i < n; i++) {
			s32 x = work[i];
			int64_t acc = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			s32 y = (s32)((acc + (1 << 27)) >> 28);

			work[i] = y;
			x2 = x1;
			x1 = x;
			y2 = y1;
			y1 = y;
		}
		st->h[k][0] = x1;
		st->h[k][1] = x2;
		st->h[k][2] = y1;
		st->h[k][3] = y2;
	}
}

/* Little-endian s16 (the earpiece's byte order) to s32, and back. Built from bytes so that no
 * sign-extending halfword load (lha) is needed: the Xenon microcodes lha, as the Cell PPE
 * does, and it costs several times the multiply it feeds (surround_dsp.h has the same rule).
 * clang turns these into lhbrx + extsh and sthbrx on the console, plain moves on a host. */
static inline s32 eqd_get_le(const u8 *p)
{
	return (s32)(s16)(u16)(p[0] | p[1] << 8);
}

static inline void eqd_put_le(u8 *p, s16 v)
{
	p[0] = (u8)v;
	p[1] = (u8)((u16)v >> 8);
}

/* eqd_block's steady state for a stereo stream (no fade pending or running, a preset or a
 * non-unity FLAT playing), in place on n interleaved little-endian frames, the earpiece's own
 * format: the same arithmetic as eqd_filter + eqd_store on each channel, bit for bit
 * (tests/test_eq.c compares the two on every preset). Three things make it cheaper per
 * sample than the general path, which the driver thread needs (it has about 1 ms of CPU per
 * frame in all, docs/INTERNALS.md "the system window"):
 *   - both channels' recurrences in one loop: the 360's core is in order, and one channel's
 *     multiply-add chain (y depends on the last y) leaves it waiting on the multiplier; the
 *     other channel's independent chain fills those slots;
 *   - the byte order is handled in the preamp and store loops, so the two whole-block swap
 *     passes of eq_360.c's general path are not needed;
 *   - no lha (eqd_get_le).
 * work: 2 * n s32, the two channels interleaved. Returns the samples clamped. */
static inline __attribute__((noinline)) u32 eqd_steady_le2(const struct eqd_bands *b,
							   struct eqd_state *st, u8 *pcm, int n,
							   s32 *work)
{
	s32 pre = b->preamp_q15;
	u32 clamps = 0;

	for (int i = 0; i < 2 * n; i++)
		work[i] = (eqd_get_le(pcm + 2 * i) * pre + 64) >> 7;
	for (int k = 0; k < EQD_BANDS; k++) {
		if (!(b->active & (1u << k)))
			continue;
		const s32 *c = b->c[k];
		int64_t b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
		s32 *hl = st[0].h[k], *hr = st[1].h[k];
		s32 lx1 = hl[0], lx2 = hl[1], ly1 = hl[2], ly2 = hl[3];
		s32 rx1 = hr[0], rx2 = hr[1], ry1 = hr[2], ry2 = hr[3];

		for (int i = 0; i < n; i++) {
			s32 xl = work[2 * i], xr = work[2 * i + 1];
			int64_t al = b0 * xl + b1 * lx1 + b2 * lx2 - a1 * ly1 - a2 * ly2;
			int64_t ar = b0 * xr + b1 * rx1 + b2 * rx2 - a1 * ry1 - a2 * ry2;
			s32 yl = (s32)((al + (1 << 27)) >> 28);
			s32 yr = (s32)((ar + (1 << 27)) >> 28);

			work[2 * i] = yl;
			work[2 * i + 1] = yr;
			lx2 = lx1;
			lx1 = xl;
			ly2 = ly1;
			ly1 = yl;
			rx2 = rx1;
			rx1 = xr;
			ry2 = ry1;
			ry1 = yr;
		}
		hl[0] = lx1;
		hl[1] = lx2;
		hl[2] = ly1;
		hl[3] = ly2;
		hr[0] = rx1;
		hr[1] = rx2;
		hr[2] = ry1;
		hr[3] = ry2;
	}
	for (int i = 0; i < 2 * n; i++) {
		s32 v = (work[i] + 128) >> 8;

		if (v > 32767 || v < -32768) {
			clamps++;
			v = v > 0 ? 32767 : -32768;
		}
		eqd_put_le(pcm + 2 * i, (s16)v);
	}
	return clamps;
}

static inline s16 eqd_round16(s32 y23, u32 *clamps)
{
	s32 v = (y23 + 128) >> 8;

	if (v > 32767 || v < -32768) {
		(*clamps)++;
		return v > 0 ? 32767 : -32768;
	}
	return (s16)v;
}

/* Q23 -> s16 into out[0], out[stride], ...; returns the samples clamped. */
static inline u32 eqd_store(const s32 *work, s16 *out, int stride, int n)
{
	u32 clamps = 0;

	for (int i = 0; i < n; i++)
		out[i * stride] = eqd_round16(work[i], &clamps);
	return clamps;
}

/* Linear crossfade a -> b; frame i of this call is frame pos + i of a len-frame fade.
 * Weight (pos << 15) / len: an exact 0..32768 ramp, so the last frame is b exactly;
 * pos < 0 is a exactly (the warm-up). */
static inline EQD_COLD u32 eqd_xfade(const s32 *a, const s32 *b, s16 *out, int stride, int n, s32 pos,
			    s32 len)
{
	u32 clamps = 0;

	for (int i = 0; i < n; i++, pos++) {
		s32 w = pos <= 0 ? 0 : pos >= len ? 32768 : (s32)(((u32)pos << 15) / (u32)len);
		s32 y = a[i] + (s32)((((int64_t)b[i] - a[i]) * w) >> 15);

		out[i * stride] = eqd_round16(y, &clamps);
	}
	return clamps;
}

/* ---- one stream, block by block --------------------------------------------------------- */

/* Asks for a preset (NULL = FLAT). Takes effect at the next eqd_block; a request during a
 * fade waits for that fade to end, so fast scrolling never cuts a fade short (a click). */
static inline void eqd_request(struct eqd_run *r, const struct eqd_bands *to)
{
	r->want = to;
}

/* Processes n interleaved frames of ch (1 or 2) channels in place. work: 2 * n s32.
 * A change runs both presets for EQD_WARM + EQD_XFADE frames (20 ms), so those blocks cost
 * twice a steady one. Returns the samples clamped. FLAT with nothing pending returns at
 * once (the output would equal the input exactly anyway). */
static inline EQD_COLD void eqd_fade_start(struct eqd_run *r)
{
	r->pn = r->want;
	r->fading = 1;
	r->xpos = -EQD_WARM;
	memset(r->nxt, 0, sizeof(r->nxt));
}

static inline EQD_COLD void eqd_fade_end(struct eqd_run *r)
{
	r->pc = r->pn;
	memcpy(r->cur, r->nxt, sizeof(r->cur));
	r->fading = 0;
}

/* true when eqd_block would take its steady branch: no fade starts or runs, and something
 * other than FLAT at 0 dB is playing (that returns at once) */
static inline int eqd_steady(const struct eqd_run *r)
{
	return !r->fading && r->want == r->pc && r->pc;
}

static inline u32 eqd_block(struct eqd_run *r, s16 *pcm, int n, int ch, s32 *work)
{
	u32 clamps = 0;

	if (!r->fading && r->want != r->pc)
		eqd_fade_start(r);
	if (!r->fading && !r->pc)
		return 0;
	for (int c = 0; c < ch; c++) {
		eqd_filter(r->pc, &r->cur[c], pcm + c, ch, work, n);
		if (r->fading) {
			eqd_filter(r->pn, &r->nxt[c], pcm + c, ch, work + n, n);
			clamps += eqd_xfade(work, work + n, pcm + c, ch, n, r->xpos, EQD_XFADE);
		} else {
			clamps += eqd_store(work, pcm + c, ch, n);
		}
	}
	if (r->fading) {
		r->xpos += n;
		if (r->xpos >= EQD_XFADE)
			eqd_fade_end(r);
	}
	return clamps;
}
