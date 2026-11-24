/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Headphone surround: the integer virtualizer (docs/ROADMAP.md 2.7 and 6.2).
 *
 * Six channels in (FL FR C LFE SL SR, s16), stereo out: each of the five main channels is
 * heard as a speaker at 0, +/-30 or +/-110 degrees through measured head-related responses
 * (the table, surround_tab.h, made by tools/hrtf/gen.py). The head is treated as left/right
 * symmetric, so each speaker pair runs as sum and difference, five FIRs in all:
 *   S = (L+R)/2 through front-sum (near+far ear), D = (L-R)/2 through front-diff (near-far),
 *   left ear = S' + D', right ear = S' - D'; the same for the surround pair; the centre goes
 *   through one filter into both ears; LFE goes into both ears at lfe_q14; then the output
 *   trim out_q14, then the clamp to s16.
 *
 * Threads and locking: none here. Pure functions, no globals, no floating point, no kernel
 * includes: the plugin and the host test (tests/test_surround.c) compile the same code, and
 * tools/hrtf/xsr_ref.py models it bit for bit. Each caller owns one struct xsr_state
 * (malloc'd: 7.5 KB at 64 taps, 10.2 KB at 128), loads it with xsr_init() and calls from one
 * thread. Include core/platform.h (for u16 / s16 / u32 / s32) and this file, then
 * surround_tab.h for the table.
 *
 * Why nothing can overflow (keep this true when changing anything below):
 *   - inputs are s16: |x| <= 2^15. The sum and difference inputs are (a+b)>>1 and (a-b)>>1,
 *     also within s16.
 *   - every filter has sum|h| <= 32768 (2.0 in Q14); xsr_table_ok() refuses a table that
 *     does not. So |acc| <= 2^15 * 2^15 = 2^30 for one FIR (also for any part of its sum).
 *   - each FIR result y = (acc + 2^13) >> 14 is within [-2^16, 2^16]; the LFE term
 *     (lfe * lfe_q14 + 2^13) >> 14 too. One ear is 3 + 2 FIR results + LFE: |v| <= 6 * 2^16.
 *   - the trim multiplies v by out_q14 (|q| <= 2^15) in two parts, (v >> 4) * q
 *     (<= 24576 * 32768 < 2^30) and (v & 15) * q (< 2^19), which is exactly
 *     (v * q + 2^13) >> 14 without a 64-bit product (nested floor division).
 *   So an s32 never overflows, and the order of the additions does not change the result.
 *
 * Input conversion (for the caller, not done here): the tap frame is float32 raw bits.
 * game_360.c already converts them with integers (f2q15, then clamp16); reuse those,
 * do not copy them. Values beyond full scale are clamped to the s16 range before this
 * kernel, and stored as s32 words (the input buffer is 6 KB for a 256-frame block).
 *
 * Cost: 5 * XSR_TAPS multiply-adds per frame (81920 per 256-frame block at 64 taps).
 * Delay: none added by the structure; the responses are minimum phase.
 */
#pragma once

#define XSR_MAGIC 0x58574852u /* 'XWHR': not 'XWSR', which is surround_360.c's status block
			       * (the tools find a block by the first aligned copy of its magic) */
#define XSR_VERSION 1
#define XSR_RATE 48000
#define XSR_FILTERS 5
#define XSR_BLOCK 256         /* most frames per xsr_process call (the tap's frame) */
#ifndef XSR_TAPS
#define XSR_TAPS 64           /* 32 / 64 / 128: surround_tab.h has one table for each */
#endif
#if XSR_TAPS < 4 || XSR_TAPS > 128 || XSR_TAPS % 4
#error "XSR_TAPS must be a multiple of 4, 4..128"
#endif
#define XSR_HIST (XSR_TAPS - 1)

/* rows of h[] */
enum { XSR_FRONT_SUM, XSR_FRONT_DIFF, XSR_SURR_SUM, XSR_SURR_DIFF, XSR_CENTRE };
/* input channels, the tap frame's order */
enum { XSR_FL, XSR_FR, XSR_C, XSR_LFE, XSR_SL, XSR_SR, XSR_CHANNELS };

/* The table: generated into surround_tab.h, or the same bytes (big-endian) in a file. */
struct xsr_table {
	u32 magic;        /* XSR_MAGIC */
	u16 version;      /* XSR_VERSION */
	u16 taps;         /* XSR_TAPS */
	u32 rate;         /* 48000 */
	s16 lfe_q14;      /* LFE into each ear, before the trim */
	s16 out_q14;      /* the output trim */
	u32 crc;          /* CRC-32 (zlib's) of h as big-endian s16, row after row */
	s16 h[XSR_FILTERS][XSR_TAPS]; /* Q14, sum|h| <= 2.0 per row */
};
_Static_assert(__builtin_offsetof(struct xsr_table, h) == 20, "the file layout");

/* Per stream. Everything the inner loop reads is a 32-bit word: the Xenon core (like the
 * Cell PPE) microcodes the sign-extending halfword load (lha), which would cost several
 * times the multiply-add it feeds. So the coefficients are copied in as s32, time-reversed
 * (the loop then walks both arrays forwards), and the history is s32. x holds each input's
 * last XSR_HIST samples, then room for one block, contiguous so the loop never wraps. */
struct xsr_state {
	s32 h[XSR_FILTERS][XSR_TAPS];
	s32 x[XSR_FILTERS][XSR_HIST + XSR_BLOCK];
	s32 lfe_q14, out_q14;
};

enum { XSR_OK, XSR_BAD_MAGIC, XSR_BAD_VERSION, XSR_BAD_TAPS, XSR_BAD_RATE, XSR_BAD_GAIN,
       XSR_BAD_SUM, XSR_BAD_CRC };

static inline u32 xsr_crc32(const s16 *h, u32 n)
{
	u32 crc = 0xffffffffu;

	for (u32 i = 0; i < n; i++) {
		u32 v = (u16)h[i];

		for (int b = 8; b >= 0; b -= 8) {
			crc ^= (v >> b) & 0xff;
			for (int k = 0; k < 8; k++)
				crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
		}
	}
	return ~crc;
}

/* XSR_OK, or why the table may not be used. The sum bound is what keeps the arithmetic
 * from overflowing, so it is checked here, not trusted. */
static inline int xsr_table_ok(const struct xsr_table *t)
{
	if (t->magic != XSR_MAGIC)
		return XSR_BAD_MAGIC;
	if (t->version != XSR_VERSION)
		return XSR_BAD_VERSION;
	if (t->taps != XSR_TAPS)
		return XSR_BAD_TAPS;
	if (t->rate != XSR_RATE)
		return XSR_BAD_RATE;
	if (t->out_q14 < 0 || t->lfe_q14 < 0)
		return XSR_BAD_GAIN;
	for (int f = 0; f < XSR_FILTERS; f++) {
		u32 sum = 0;

		for (int k = 0; k < XSR_TAPS; k++)
			sum += (u32)(t->h[f][k] < 0 ? -t->h[f][k] : t->h[f][k]);
		if (sum > 32768)
			return XSR_BAD_SUM;
	}
	if (xsr_crc32(&t->h[0][0], XSR_FILTERS * XSR_TAPS) != t->crc)
		return XSR_BAD_CRC;
	return XSR_OK;
}

/* Forget the history (as if silence came before), e.g. when surround is switched on again. */
static inline void xsr_clear(struct xsr_state *st)
{
	for (int f = 0; f < XSR_FILTERS; f++)
		for (int k = 0; k < XSR_HIST + XSR_BLOCK; k++)
			st->x[f][k] = 0;
}

/* Take a table that passed xsr_table_ok() (the overflow proof relies on it), and clear. */
static inline void xsr_init(struct xsr_state *st, const struct xsr_table *t)
{
	for (int f = 0; f < XSR_FILTERS; f++)
		for (int k = 0; k < XSR_TAPS; k++)
			st->h[f][k] = t->h[f][XSR_HIST - k];
	st->lfe_q14 = t->lfe_q14;
	st->out_q14 = t->out_q14;
	xsr_clear(st);
}

/* Two consecutive outputs of one FIR, each rounded back to the input's scale. h is
 * time-reversed, so output 0 is sum h[k] x[k] (x[0] the oldest input it sees, x[XSR_HIST]
 * the newest) and output 1 the same one sample on. Each coefficient load feeds two
 * multiply-adds; reads x[0..XSR_TAPS]. Not inlined: five inlined copies cost about 1 KB of
 * image for a call overhead of about 2 us a block. */
__attribute__((noinline)) static void xsr_fir2(const s32 *h, const s32 *x, s32 *y)
{
	s32 a0 = 0, a1 = 0, xa = x[0];

	for (int k = 0; k < XSR_TAPS; k++) {
		s32 xb = x[k + 1];

		a0 += h[k] * xa;
		a1 += h[k] * xb;
		xa = xb;
	}
	y[0] = (a0 + 8192) >> 14;
	y[1] = (a1 + 8192) >> 14;
}

/* (v * q + 2^13) >> 14 in s32 (see the top), then the clamp, counted */
static inline s16 xsr_trim(s32 v, s32 q, u32 *clamps)
{
	s32 r = ((v >> 4) * q + (((v & 15) * q + 8192) >> 4)) >> 10;

	if (r > 32767) {
		(*clamps)++;
		return 32767;
	}
	if (r < -32768) {
		(*clamps)++;
		return -32768;
	}
	return (s16)r;
}

/* n frames (at most XSR_BLOCK; more are ignored). in: s16 values (-32768..32767; the
 * overflow proof needs that range) held in s32 words, so they are read with word loads (see
 * struct xsr_state); planar, channel c at in + c * n, in XSR_FL.. order. out: interleaved
 * L R, 2n values. Returns how many output samples were clamped. */
static inline u32 xsr_process(struct xsr_state *st, const s32 *in, u32 n, s16 *out)
{
	const s32 *fl = in, *fr = in + n, *c = in + 2 * n, *lfe = in + 3 * n;
	const s32 *sl = in + 4 * n, *sr = in + 5 * n;
	s32 *xfs = st->x[XSR_FRONT_SUM], *xfd = st->x[XSR_FRONT_DIFF];
	s32 *xss = st->x[XSR_SURR_SUM], *xsd = st->x[XSR_SURR_DIFF], *xc = st->x[XSR_CENTRE];
	u32 clamps = 0;

	if (n > XSR_BLOCK)
		n = XSR_BLOCK;
	for (u32 i = 0; i < n; i++) {
		xfs[XSR_HIST + i] = (fl[i] + fr[i]) >> 1;
		xfd[XSR_HIST + i] = (fl[i] - fr[i]) >> 1;
		xss[XSR_HIST + i] = (sl[i] + sr[i]) >> 1;
		xsd[XSR_HIST + i] = (sl[i] - sr[i]) >> 1;
		xc[XSR_HIST + i] = c[i];
	}
	/* in pairs; with an odd n the last pair's second output reads one slot past the block
	 * (inside the buffer, an old s16-range sample, so still within the bound) and is unused */
	for (u32 i = 0; i < n; i += 2) {
		s32 fs[2], fd[2], ss[2], sd[2], cc[2];

		xsr_fir2(st->h[XSR_FRONT_SUM], xfs + i, fs);
		xsr_fir2(st->h[XSR_FRONT_DIFF], xfd + i, fd);
		xsr_fir2(st->h[XSR_SURR_SUM], xss + i, ss);
		xsr_fir2(st->h[XSR_SURR_DIFF], xsd + i, sd);
		xsr_fir2(st->h[XSR_CENTRE], xc + i, cc);
		for (u32 j = 0; j < 2 && i + j < n; j++) {
			s32 s = fs[j] + ss[j] + cc[j], d = fd[j] + sd[j];
			s32 low = (lfe[i + j] * st->lfe_q14 + 8192) >> 14;

			out[2 * (i + j)] = xsr_trim(s + d + low, st->out_q14, &clamps);
			out[2 * (i + j) + 1] = xsr_trim(s - d + low, st->out_q14, &clamps);
		}
	}
	/* keep the newest XSR_HIST inputs at the front (forward copy: the source is higher) */
	for (int f = 0; f < XSR_FILTERS; f++)
		for (int k = 0; k < XSR_HIST; k++)
			st->x[f][k] = st->x[f][n + k];
	return clamps;
}


/* Switching surround on or off: compute both outputs for one block and fade from one to
 * the other across it (linear, reaching `to` on the last frame when n divides 65536, as
 * 256 does). Interleaved stereo, n frames; out may be from or to. */
static inline void xsr_crossfade(s16 *out, const s16 *from, const s16 *to, u32 n)
{
	u32 step;

	if (!n)
		return;
	step = 65536u / n;
	for (u32 i = 0; i < 2 * n; i++) {
		s32 g = (s32)((((i >> 1) + 1) * step) >> 1); /* Q15, up to 32768 */

		out[i] = (s16)(from[i] + (((to[i] - from[i]) * g) >> 15));
	}
}
