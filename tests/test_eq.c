// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of the equalizer kernel (xbox360/eq_dsp.h) against the preset file and the
 * vectors tools/presets/convert.py exports from its Python twin (eq_model.py):
 *   1. tools/presets/xw.presets: header, CRC, ids (FNV-1a of the name), names and descriptions
 *      in the panel font, a corrupted, truncated or version-1 copy refused; the CUSTOM tables
 *      read, and a bad table refused (CUSTOM only);
 *   2. the decoded coefficients equal the converter's;
 *   3. bit-exact output against the Python model (fades, queued requests, odd block sizes,
 *      mono and stereo);
 *   4. frequency response of the integer kernel, measured with sines, within 0.2 dB of the
 *      floating-point design for every preset;
 *   5. no wrap: a shadow cascade with 128-bit accumulators checks every band output fits
 *      s32 under full-scale noise and the worst-case input for every band, and matches the
 *      kernel sample for sample;
 *   6. the 10 ms crossfade is click-free: no sample step beyond the two presets' own and the
 *      fade's slope;
 *   7. loudness: every preset as loud as FLAT (the file's flat_q15) on pink noise, K-weighted
 *      (an own-level one never louder); CUSTOM 0 0 0 is FLAT bit for bit; no CUSTOM
 *      combination louder than FLAT;
 *   8. the stereo fast path eq_360.c uses in the steady state (eqd_steady_le2: little-endian
 *      in place, both channels in one loop) equals eqd_block bit for bit: every preset, the
 *      CUSTOM combinations and FLAT, quiet and full-scale noise, odd block sizes, history;
 *   9. host time per 384-frame stereo block (information only).
 * CUSTOM runs through 3 (every single-control step, the corners and a random sample, bit-exact
 * with the model, coefficients and preamps included), 5 (the corners and every single step at
 * +-6) and 6 (a held direction: every step to the next, on each control).
 * Floating point is used only by this checking code; eq_dsp.h has none.
 *
 *   python3 tools/presets/convert.py --vectors
 *   cc -O2 -Wall -o test_eq tests/test_eq.c -lm && ./test_eq [tools/presets/xw.presets dist/eq_vectors.bin]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../xbox360/eq_dsp.h"

#define MAXP 64
#define BLOCK 384

static int g_fail;
static struct eqd_bands g_b[MAXP];
static char g_name[MAXP][EQD_NAME + 1];
static u32 g_n;
/* FLAT as eq_360.c plays it while this file is loaded: no bands, the file's flat_q15 gain */
static struct eqd_bands g_flat;
static const struct eqd_bands *g_flatp;
static char g_desc[MAXP][EQD_DESC];
static u32 g_flags[MAXP];
static struct eqd_custom g_ct;
#define NCOMBO 128
static struct eqd_bands g_cb[NCOMBO]; /* the vectors' CUSTOM combinations */
static int g_combo[NCOMBO][3];
static u32 g_ncombo;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		g_fail = 1;
}

static u8 *load(const char *path, u32 *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s (run python3 tools/presets/convert.py --vectors)\n",
			path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *p = malloc((size_t)n);
	if (fread(p, 1, (size_t)n, f) != (size_t)n)
		exit(2);
	fclose(f);
	*len = (u32)n;
	return p;
}

/* a cursor over the big-endian vectors file */
static const u8 *g_v;
static u32 rd32(void)
{
	u32 v = eqd_be32(g_v);
	g_v += 4;
	return v;
}
static s16 rd16(void)
{
	s16 v = (s16)(g_v[0] << 8 | g_v[1]);
	g_v += 2;
	return v;
}

/* ---- 1. the preset file --------------------------------------------------------------- */

static void test_file(u8 *f, u32 len)
{
	static const char glyphs[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-%+:/.<> ";
	int n = eqd_check(f, len);
	char author[EQD_AUTHOR + 1];
	s16 g[EQD_BANDS];
	int ids_ok = 1, names_ok = 1, pre_ok = 1;

	printf("xw.presets: %u bytes, %d presets\n", len, n);
	/* whatever set ships (tools/presets/names.txt): the vectors carry the same count */
	check(n >= 1 && n <= 64 && len == EQD_HDR + (u32)n * EQD_REC + EQD_CUSTOM_BYTES,
	      "file: 1..64 presets, 32 + 224 bytes each + the CUSTOM tables");
	if (n <= 0)
		exit(1);
	g_n = (u32)n;
	g_flat.preamp_q15 = eqd_flat_q15(f);
	g_flatp = g_flat.preamp_q15 == 32768 ? NULL : &g_flat;
	printf("FLAT while loaded: %.3f dB\n", 20 * log10(g_flat.preamp_q15 / 32768.0));
	for (u32 i = 0; i < g_n; i++) {
		u32 id = eqd_record(f, i, &g_b[i], g_name[i], author, g_desc[i], g);
		g_flags[i] = eqd_be32(f + EQD_HDR + i * EQD_REC + 44);
		if (id != eqd_fnv1a(g_name[i]) || id == 0)
			ids_ok = 0;
		for (u32 j = 0; j < i; j++)
			if (eqd_fnv1a(g_name[j]) == id)
				ids_ok = 0;
		if (!g_name[i][0] || strspn(g_name[i], glyphs) != strlen(g_name[i]) ||
		    (strcmp(author, "ZaliaS") && strcmp(author, "Xbox360-GIP")) || !g_desc[i][0] ||
		    strspn(g_desc[i], glyphs) != strlen(g_desc[i]) || strlen(g_desc[i]) > 63)
			names_ok = 0;
		if (g_b[i].preamp_q15 <= 0 || g_b[i].preamp_q15 > 32768)
			pre_ok = 0;
	}
	check(ids_ok, "file: every id is FNV-1a of its name, unique and non-zero");
	check(names_ok, "file: names and descriptions (1..63) drawable by the panel font, author "
			"ZaliaS or Xbox360-GIP");
	printf("     1 %s: %s\n     %u %s: %s\n", g_name[0], g_desc[0], g_n, g_name[g_n - 1],
	       g_desc[g_n - 1]);
	check(pre_ok, "file: preamp in 1..32768");
	f[EQD_HDR + 100] ^= 1;
	check(eqd_check(f, len) == -5, "file: a flipped bit is refused (CRC)");
	f[EQD_HDR + 100] ^= 1;
	check(eqd_check(f, len - 1) == -5 && eqd_check(f, EQD_HDR + g_n * EQD_REC - 1) == -4,
	      "file: truncated in the CUSTOM tables (CRC) or in the records (length): refused");
	f[7] = 1;
	check(eqd_check(f, len) == -6, "file: version 1 refused as an older file");
	f[7] = 3;
	check(eqd_check(f, len) == -2, "file: version 3 refused");
	f[7] = 2;

	/* CUSTOM */
	int ce = eqd_custom_read(f, len, g_n, g_flat.preamp_q15, &g_ct);
	check(ce == 0, "custom: the tables read");
	static struct eqd_custom bad;
	u8 *t = f + EQD_HDR + g_n * EQD_REC;
	u32 pre0 = EQD_CHDR + EQD_CBANDS * EQD_CSTEPS * 20;
	check(eqd_custom_read(f, EQD_HDR + g_n * EQD_REC, g_n, g_flat.preamp_q15, &bad) == -1,
	      "custom: absent -> -1");
	check(eqd_custom_read(f, len - 2, g_n, g_flat.preamp_q15, &bad) == -2,
	      "custom: wrong size -> -2");
	t[3] ^= 1;
	check(eqd_custom_read(f, len, g_n, g_flat.preamp_q15, &bad) == -2, "custom: magic -> -2");
	t[3] ^= 1;
	t[pre0 + 100] = 0x80;
	t[pre0 + 101] = 1; /* 32769 */
	check(eqd_custom_read(f, len, g_n, g_flat.preamp_q15, &bad) == -3,
	      "custom: a preamp above 0 dB -> -3");
	t[pre0 + 100] = (u8)(g_ct.pre[50] >> 8);
	t[pre0 + 101] = (u8)g_ct.pre[50];
	check(eqd_custom_read(f, len, g_n, g_flat.preamp_q15 - 1, &bad) == -3,
	      "custom: 0 0 0 not at FLAT's level -> -3");
	t[EQD_CHDR + (0 * EQD_CSTEPS + EQD_CMID) * 20 + 7] ^= 1; /* BASS step 0's b0 */
	check(eqd_custom_read(f, len, g_n, g_flat.preamp_q15, &bad) == -3,
	      "custom: step 0 not unity -> -3");
	t[EQD_CHDR + (0 * EQD_CSTEPS + EQD_CMID) * 20 + 7] ^= 1;
	check(eqd_custom_read(f, len, g_n, g_flat.preamp_q15, &bad) == 0 &&
		      !memcmp(&bad, &g_ct, sizeof(bad)),
	      "custom: restored, reads the same");
	struct eqd_bands z;
	eqd_custom_bands(&g_ct, 0, 0, 0, &z);
	check(z.active == 0 && z.preamp_q15 == g_flat.preamp_q15,
	      "custom: 0 0 0 is FLAT (no band, FLAT's level)");
}

/* ---- 2 / 3. coefficients and bit-exact vectors ------------------------------------------ */

static void run_cases(u32 nc, u64 *total_out, u64 *bad_out);

static void test_vectors(u32 *nf_out, const u8 **freqs_out)
{
	if (rd32() != 0x58574556u || rd32() != 2) {
		check(0, "vectors: header");
		exit(1);
	}
	u32 np = rd32(), nf = rd32(), nc = rd32();
	int same = np == g_n;

	for (u32 i = 0; i < np && same; i++) {
		same &= (s32)rd32() == g_b[i].preamp_q15;
		same &= rd32() == g_b[i].active;
		for (int k = 0; k < 25; k++)
			same &= (s32)rd32() == g_b[i].c[k / 5][k % 5];
	}
	check(same, "vectors: decoded coefficients equal the converter's");
	*nf_out = nf;
	*freqs_out = g_v;
	g_v += np * nf * 8;

	u64 total = 0, bad = 0;
	run_cases(nc, &total, &bad);
	printf("vectors: %u cases, %llu samples, %llu differ\n", nc, (unsigned long long)total,
	       (unsigned long long)bad);
	check(bad == 0 && total > 0, "bit-exact with the Python model");

	/* CUSTOM: the combinations as eqd_custom_bands builds them from the file, against the
	 * model's, then streams that request them */
	u32 nk = rd32(), cbad = 0;
	g_ncombo = nk < NCOMBO ? nk : NCOMBO;
	for (u32 i = 0; i < nk; i++) {
		struct eqd_bands want;
		int st[3];
		for (int k = 0; k < 3; k++)
			st[k] = (s32)rd32();
		want.preamp_q15 = (s32)rd32();
		want.active = rd32();
		for (int k = 0; k < 25; k++)
			want.c[k / 5][k % 5] = (s32)rd32();
		if (i >= NCOMBO)
			continue;
		memcpy(g_combo[i], st, sizeof(st));
		eqd_custom_bands(&g_ct, st[0], st[1], st[2], &g_cb[i]);
		cbad += memcmp(&want, &g_cb[i], sizeof(want)) != 0;
	}
	check(nk == g_ncombo && nk >= 100 && cbad == 0,
	      "custom: every single-control step, the corners and a sample of combinations: "
	      "coefficients and preamp equal the model's");
	nc = rd32();
	total = bad = 0;
	run_cases(nc, &total, &bad);
	printf("custom vectors: %u combinations, %u cases, %llu samples, %llu differ\n", nk, nc,
	       (unsigned long long)total, (unsigned long long)bad);
	check(bad == 0 && total > 0, "custom: streams bit-exact with the Python model (steps, a held "
				     "direction, mixed with presets)");
}

/* the vectors' cases at g_v: request -1 none, 0 FLAT, k preset k, 1000 + i combination i */
static void run_cases(u32 nc, u64 *total_out, u64 *bad_out)
{
	u64 total = 0, bad = 0;
	for (u32 c = 0; c < nc; c++) {
		u32 ch = rd32(), nb = rd32();
		u32 *fr = malloc(nb * 4);
		s32 *rq = malloc(nb * 4);
		u32 frames = 0;
		for (u32 b = 0; b < nb; b++) {
			fr[b] = rd32();
			rq[b] = (s32)rd32();
			frames += fr[b];
		}
		s16 *pcm = malloc(frames * ch * 2);
		for (u32 i = 0; i < frames * ch; i++)
			pcm[i] = rd16();
		struct eqd_run r;
		memset(&r, 0, sizeof(r));
		s32 *work = malloc(2 * BLOCK * 4);
		u32 pos = 0;
		for (u32 b = 0; b < nb; b++) {
			if (rq[b] >= 1000)
				eqd_request(&r, &g_cb[rq[b] - 1000]);
			else if (rq[b] >= 0)
				eqd_request(&r, rq[b] ? &g_b[rq[b] - 1] : g_flatp);
			eqd_block(&r, pcm + pos * ch, (int)fr[b], (int)ch, work);
			pos += fr[b];
		}
		for (u32 i = 0; i < frames * ch; i++) {
			s16 want = rd16();
			bad += pcm[i] != want;
			total++;
		}
		free(fr);
		free(rq);
		free(pcm);
		free(work);
	}
	*total_out += total;
	*bad_out += bad;
}

/* ---- 4. frequency response ------------------------------------------------------------- */

/* Least-squares amplitude of a sine of known frequency in y[0..n-1] (sin / cos fit). */
static double amplitude(const double *y, int n, double w, int start)
{
	double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
	for (int i = 0; i < n; i++) {
		double s = sin(w * (start + i)), c = cos(w * (start + i));
		ss += s * s;
		cc += c * c;
		sc += s * c;
		ys += y[i] * s;
		yc += y[i] * c;
	}
	double det = ss * cc - sc * sc;
	double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
	return sqrt(a * a + b * b);
}

static void test_response(u32 nf, const u8 *tab)
{
	enum { SETTLE = 12000, MEAS = 9600, N = SETTLE + MEAS };
	s16 *x = malloc(N * 2), *y = malloc(N * 2);
	double *xi = malloc(MEAS * sizeof(double)), *yo = malloc(MEAS * sizeof(double));
	s32 work[BLOCK];
	double worst = 0;
	u32 worst_p = 0, points = 0, skipped = 0;
	double worst_f = 0;

	for (u32 p = 0; p < g_n; p++) {
		for (u32 j = 0; j < nf; j++) {
			const u8 *e = tab + (p * nf + j) * 8;
			double f = eqd_be32(e) / 1000.0, want = (s32)eqd_be32(e + 4) / 1e6;
			double w = 2 * M_PI * f / EQD_RATE;
			struct eqd_state st;

			/* a band-pass's stop band: the s16 output of a -6 dBFS sine is a few LSB
			 * there, so its level cannot be measured to 0.2 dB */
			if (want < -40) {
				skipped++;
				continue;
			}
			memset(&st, 0, sizeof(st));
			for (int i = 0; i < N; i++)
				x[i] = (s16)lrint(16384 * sin(w * i));
			for (int i = 0; i < N; i += BLOCK) {
				int n = N - i < BLOCK ? N - i : BLOCK;
				eqd_filter(&g_b[p], &st, x + i, 1, work, n);
				eqd_store(work, y + i, 1, n);
			}
			for (int i = 0; i < MEAS; i++) {
				xi[i] = x[SETTLE + i];
				yo[i] = y[SETTLE + i];
			}
			double got = 20 * log10(amplitude(yo, MEAS, w, SETTLE) /
						amplitude(xi, MEAS, w, SETTLE));
			double err = fabs(got - want);
			points++;
			if (err > worst) {
				worst = err;
				worst_p = p;
				worst_f = f;
			}
		}
	}
	printf("response: %u presets x %u sines (20 Hz..20 kHz, 1/6 octave + band centres; %u "
	       "points below -40 dB skipped): worst %.4f dB (%s at %.0f Hz)\n",
	       g_n, nf, skipped, worst, g_name[worst_p], worst_f);
	check(points > 0 && worst < 0.2, "frequency response within 0.2 dB of the float design");
	free(x);
	free(y);
	free(xi);
	free(yo);
}

/* ---- 5. no wrap ------------------------------------------------------------------------- */

/* The kernel's arithmetic with 128-bit accumulators and no narrowing: records the largest
 * band output and accumulator, and the samples whose band output would not fit s32. */
struct shadow {
	__int128 h[EQD_BANDS][4];
	double max_y, max_acc;
	u64 wraps;
};

static s16 shadow_sample(struct shadow *s, const struct eqd_bands *b, s16 in, u32 *clamps)
{
	__int128 x = (in * b->preamp_q15 + 64) >> 7;
	for (int k = 0; k < EQD_BANDS; k++) {
		if (!(b->active & (1u << k)))
			continue;
		const s32 *c = b->c[k];
		__int128 *h = s->h[k];
		__int128 acc = c[0] * x + c[1] * h[0] + c[2] * h[1] - c[3] * h[2] - c[4] * h[3];
		__int128 y = (acc + (1 << 27)) >> 28;
		double da = fabs((double)acc), dy = fabs((double)y);
		if (da > s->max_acc)
			s->max_acc = da;
		if (dy > s->max_y)
			s->max_y = dy;
		if (y > INT32_MAX || y < INT32_MIN)
			s->wraps++;
		h[1] = h[0];
		h[0] = x;
		h[3] = h[2];
		h[2] = y;
		x = y;
	}
	return eqd_round16((s32)x, clamps);
}

/* Runs x through the kernel and the shadow; returns the samples that differ. */
static u64 run_both(const struct eqd_bands *b, const s16 *x, int n, struct shadow *s,
		    u32 *clamps)
{
	struct eqd_state st;
	s16 y[BLOCK];
	s32 work[BLOCK];
	u64 differ = 0;

	memset(&st, 0, sizeof(st));
	memset(s->h, 0, sizeof(s->h));
	for (int i = 0; i < n; i += BLOCK) {
		int m = n - i < BLOCK ? n - i : BLOCK;
		eqd_filter(b, &st, x + i, 1, work, m);
		eqd_store(work, y, 1, m);
		for (int j = 0; j < m; j++)
			differ += shadow_sample(s, b, x[i + j], clamps) != y[j];
	}
	return differ;
}

/* The input that drives the output of bands 0..last to its largest value at the end:
 * full scale with the signs of the time-reversed impulse response (double precision). */
static void worst_input(const struct eqd_bands *b, int last, s16 *x, int n)
{
	double *h = calloc((size_t)n, sizeof(double));
	h[0] = 1;
	for (int k = 0; k <= last; k++) {
		if (!(b->active & (1u << k)))
			continue;
		double c[5], x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		for (int j = 0; j < 5; j++)
			c[j] = b->c[k][j] / 268435456.0;
		for (int i = 0; i < n; i++) {
			double v = c[0] * h[i] + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
			x2 = x1;
			x1 = h[i];
			y2 = y1;
			y1 = v;
			h[i] = v;
		}
	}
	for (int i = 0; i < n; i++)
		x[i] = h[n - 1 - i] >= 0 ? 32767 : -32768;
	free(h);
}

static void test_wrap(void)
{
	enum { N = 48000, W = 4096 };
	s16 *x = malloc(N * 2);
	double max_y = 0, max_acc = 0;
	u64 wraps = 0, differ = 0;
	u32 clamps_noise = 0, clamps_worst = 0;
	u32 seed = 12345;

	for (u32 p = 0; p < g_n + g_ncombo; p++) {
		/* then the CUSTOM combinations: every single step, the corners, the sample */
		const struct eqd_bands *b = p < g_n ? &g_b[p] : &g_cb[p - g_n];
		struct shadow s;
		memset(&s, 0, sizeof(s));
		for (int kind = 0; kind < 2; kind++) {
			for (int i = 0; i < N; i++) {
				seed = seed * 1664525u + 1013904223u;
				x[i] = kind ? ((seed >> 31) ? 32767 : -32768) : (s16)(seed >> 16);
			}
			differ += run_both(b, x, p < g_n ? N : N / 8, &s, &clamps_noise);
		}
		for (int last = 0; last < EQD_BANDS; last++) {
			worst_input(b, last, x, W);
			differ += run_both(b, x, W, &s, &clamps_worst);
		}
		if (s.max_y > max_y)
			max_y = s.max_y;
		if (s.max_acc > max_acc)
			max_acc = s.max_acc;
		wraps += s.wraps;
	}
	printf("wrap: full-scale uniform + binary noise and the worst-case input per band, all "
	       "presets and %u CUSTOM combinations: largest band output %.4f of s32, largest accumulator 2^%.1f, "
	       "%llu would-wrap, %llu kernel/shadow differences; output clamps: noise %u, "
	       "worst-case %u\n",
	       g_ncombo, max_y / 2147483648.0, log2(max_acc), (unsigned long long)wraps,
	       (unsigned long long)differ, clamps_noise, clamps_worst);
	check(wraps == 0 && differ == 0 && max_y < 2147483648.0 && max_acc < 9.2e18,
	      "no overflow or wrap (outputs beyond s16 are clamped, not wrapped)");
	free(x);
}

/* ---- 6. crossfade ----------------------------------------------------------------------- */

/* Stereo sine (both channels) through a run that plays `from` warm from frame 0 and, if
 * request, is asked for `to` at frame at (a block start). out: n frames, left channel. */
static void run_sine(const struct eqd_bands *from, const struct eqd_bands *to, int request,
		     double f, int at, int n, double *out)
{
	s16 *pcm = malloc((size_t)n * 4);
	s32 work[2 * BLOCK];
	struct eqd_run r;
	double w = 2 * M_PI * f / EQD_RATE;

	for (int i = 0; i < n; i++)
		pcm[2 * i] = pcm[2 * i + 1] = (s16)lrint(16000 * sin(w * i + 0.3));
	memset(&r, 0, sizeof(r));
	r.pc = r.want = from;
	for (int i = 0; i < n; i += BLOCK) {
		if (request && i == at)
			eqd_request(&r, to);
		eqd_block(&r, pcm + 2 * i, n - i < BLOCK ? n - i : BLOCK, 2, work);
	}
	for (int i = 0; i < n; i++)
		out[i] = pcm[2 * i];
	free(pcm);
}

/* Every switch FLAT -> 1, 1 -> 2, ..., last -> FLAT, then CUSTOM as a held direction moves it
 * (each control from 0 to +6, down to -6 and back to 0, one step a switch), at six sine
 * frequencies. In the fade
 * window, the largest step between output samples is compared with the larger of the two
 * presets' own largest steps (each played alone, warm, over the same window):
 *   ratio   that step / own: the brief's "no step larger than the signals' own";
 *   excess  that step - (own + max|b - a| / EQD_XFADE + 2): what a linear fade between the two
 *           warm signals cannot produce by itself (a cut, or the new filter's cold start);
 *           a click shows here, the fade's own slope does not.
 * A hard switch (b's sample right after a's, less own) is reported for scale. */
static void test_xfade(void)
{
	static const double freqs[] = { 60, 100, 440, 1000, 3000, 8000 };
	enum { AT = 25 * BLOCK, SPAN = EQD_WARM + EQD_XFADE + 2 * BLOCK, N = AT + SPAN + BLOCK };
	double *ya = malloc(N * sizeof(double)), *yb = malloc(N * sizeof(double));
	double *yx = malloc(N * sizeof(double));
	double worst = 0, worst_excess = -1e9, worst_hard = 0, worst_step = 0;
	char where[64] = "";
	int pairs = 0;
	/* the held direction: steps 0 1 .. 6 5 .. -6 .. -1 0 per control */
	static struct eqd_bands held[3][25];
	int nheld = 0;
	for (int k = 0; k < 3; k++) {
		int st = 0, i = 0;
		for (int dir = 1, n = 0; n < 24; n++) {
			st += dir;
			if (st == 6 || st == -6)
				dir = -dir;
			int s3[3] = { 0, 0, 0 };
			s3[k] = st;
			eqd_custom_bands(&g_ct, s3[0], s3[1], s3[2], &held[k][i++]);
		}
		nheld = i;
	}

	for (u32 p = 0; p <= g_n + 3 * (u32)nheld; p++) {
		const struct eqd_bands *a, *b;
		char an[24], bn[24];
		if (p <= g_n) {
			a = p ? &g_b[p - 1] : g_flatp;
			b = p < g_n ? &g_b[p] : g_flatp;
			snprintf(an, sizeof(an), "%s", p ? g_name[p - 1] : "FLAT");
			snprintf(bn, sizeof(bn), "%s", p < g_n ? g_name[p] : "FLAT");
		} else { /* CUSTOM, one control at a time, from FLAT (0 0 0) step by step */
			u32 q = p - g_n - 1, k = q / (u32)nheld, i = q % (u32)nheld;
			a = i ? &held[k][i - 1] : g_flatp;
			b = i + 1 < (u32)nheld ? &held[k][i] : g_flatp; /* the last step is back to 0 */
			snprintf(an, sizeof(an), "CUSTOM %u step %u", k, i);
			snprintf(bn, sizeof(bn), "step %u", i + 1);
		}
		for (unsigned k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
			double sa = 0, sb = 0, sx = 0, diff = 0;
			run_sine(a, a, 0, freqs[k], AT, N, ya);
			run_sine(b, b, 0, freqs[k], AT, N, yb);
			run_sine(a, b, 1, freqs[k], AT, N, yx);
			for (int i = AT; i < AT + SPAN; i++) {
				sa = fmax(sa, fabs(ya[i] - ya[i - 1]));
				sb = fmax(sb, fabs(yb[i] - yb[i - 1]));
				sx = fmax(sx, fabs(yx[i] - yx[i - 1]));
				diff = fmax(diff, fabs(yb[i] - ya[i]));
			}
			double own = fmax(sa, sb);
			double excess = sx - (own + diff / EQD_XFADE + 2);
			double hard = fabs(yb[AT] - ya[AT - 1]) - own;
			pairs++;
			worst_excess = fmax(worst_excess, excess);
			worst_hard = fmax(worst_hard, hard);
			worst_step = fmax(worst_step, sx);
			if (sx / own > worst) {
				worst = sx / own;
				snprintf(where, sizeof(where), "%s -> %s at %.0f Hz", an, bn,
					 freqs[k]);
			}
		}
	}
	printf("xfade: %d switches, -6 dBFS sines: worst step ratio %.3f (%s); worst excess over "
	       "a linear fade's own slope %+.1f LSB; largest step in any fade %.0f; a hard switch "
	       "would exceed the signals' own step by up to %.0f LSB\n",
	       pairs, worst, where, worst_excess, worst_step, worst_hard);
	check(worst_excess <= 0, "10 ms crossfade: no step beyond the two signals' own and the "
				 "fade's slope (no click)");
	free(ya);
	free(yb);
	free(yx);
}

/* ---- 7. loudness ----------------------------------------------------------------------- */

/* biquad in double, for the K-weighting of the checking code only */
static void dbiquad(const double *c, double *st, double *x, int n)
{
	for (int i = 0; i < n; i++) {
		double y = c[0] * x[i] + c[1] * st[0] + c[2] * st[1] - c[3] * st[2] - c[4] * st[3];
		st[1] = st[0];
		st[0] = x[i];
		st[3] = st[2];
		st[2] = y;
		x[i] = y;
	}
}

/* K-weighted mean square (ITU-R BS.1770) of pink noise through bands b, in dB, after 0.5 s */
static double k_loudness(const struct eqd_bands *b, const s16 *x, int n)
{
	static const double k1[5] = { 1.53512485958697, -2.69169618940638, 1.19839281085285,
				      -1.69065929318241, 0.73248077421585 };
	static const double k2[5] = { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };
	s16 *y = malloc((size_t)n * 2);
	double *d = malloc((size_t)n * sizeof(double)), s1[4] = { 0 }, s2[4] = { 0 }, sum = 0;
	s32 work[BLOCK];
	struct eqd_state st;

	memset(&st, 0, sizeof(st));
	for (int i = 0; i < n; i += BLOCK) {
		int m = n - i < BLOCK ? n - i : BLOCK;
		eqd_filter(b, &st, x + i, 1, work, m);
		eqd_store(work, y + i, 1, m);
	}
	for (int i = 0; i < n; i++)
		d[i] = y[i];
	dbiquad(k1, s1, d, n);
	dbiquad(k2, s2, d, n);
	for (int i = EQD_RATE / 2; i < n; i++)
		sum += d[i] * d[i];
	free(y);
	free(d);
	return 10 * log10(sum / (n - EQD_RATE / 2));
}

/* Pink noise (Paul Kellet's filter on white noise, within 0.05 dB of 1/f above 10 Hz) at
 * about -20 dBFS RMS, so nothing clamps; the same noise for FLAT and every preset. */
static void test_loudness(void)
{
	enum { N = 4 * EQD_RATE };
	s16 *x = malloc(N * 2);
	double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, worst = 0;
	u32 seed = 777, wp = 0;

	for (int i = 0; i < N; i++) {
		seed = seed * 1664525u + 1013904223u;
		double w = ((double)(seed >> 8) / 16777216.0 - 0.5) * 2;
		b0 = 0.99886 * b0 + w * 0.0555179;
		b1 = 0.99332 * b1 + w * 0.0750759;
		b2 = 0.96900 * b2 + w * 0.1538520;
		b3 = 0.86650 * b3 + w * 0.3104856;
		b4 = 0.55000 * b4 + w * 0.5329522;
		b5 = -0.7616 * b5 - w * 0.0168980;
		double v = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
		b6 = w * 0.115926;
		x[i] = (s16)lrint(v * 1300);
	}
	double ref = k_loudness(g_flatp, x, N), in = k_loudness(NULL, x, N), own_max = -99;
	for (u32 p = 0; p < g_n; p++) {
		double d = k_loudness(&g_b[p], x, N) - ref;
		if (g_flags[p] & 0x100) { /* XQ_F_OWN_LEVEL: never louder, may be quieter */
			printf("     %s (own level): %+.2f dB against FLAT\n", g_name[p], d);
			own_max = fmax(own_max, d);
			continue;
		}
		if (fabs(d) > fabs(worst)) {
			worst = d;
			wp = p;
		}
	}
	printf("loudness: FLAT %.2f dB under the input; every preset within %+.3f dB of FLAT (%s), "
	       "K-weighted pink noise\n",
	       in - ref, worst, g_name[wp]);
	check(fabs(worst) < 0.1 && own_max < 0.1,
	      "presets loudness-matched to FLAT within 0.1 dB; own-level ones never louder");

	/* CUSTOM: 0 0 0 is FLAT bit for bit; no combination louder than FLAT */
	struct eqd_bands z;
	eqd_custom_bands(&g_ct, 0, 0, 0, &z);
	s16 *y1 = malloc(N * 2), *y2 = malloc(N * 2);
	s32 work[BLOCK];
	struct eqd_state s1, s2;
	memset(&s1, 0, sizeof(s1));
	memset(&s2, 0, sizeof(s2));
	for (int i = 0; i < N; i += BLOCK) {
		eqd_filter(&z, &s1, x + i, 1, work, BLOCK);
		eqd_store(work, y1 + i, 1, BLOCK);
		eqd_filter(g_flatp, &s2, x + i, 1, work, BLOCK);
		eqd_store(work, y2 + i, 1, BLOCK);
	}
	check(!memcmp(y1, y2, N * 2), "custom: 0 0 0 sounds exactly as FLAT (bit for bit)");
	double cmax = -99, cmin = 99;
	u32 matched = 0;
	for (u32 i = 0; i < g_ncombo; i++) {
		double d = k_loudness(&g_cb[i], x, N) - ref;
		cmax = fmax(cmax, d);
		cmin = fmin(cmin, d);
		matched += fabs(d) < 0.1;
	}
	printf("custom loudness: %u combinations, %u within 0.1 dB of FLAT, the rest quieter (they "
	       "boost and need the headroom); range %+.2f .. %+.2f dB\n",
	       g_ncombo, matched, cmin, cmax);
	check(cmax < 0.1, "custom: no combination louder than FLAT");
	free(y1);
	free(y2);
	free(x);
}

/* ---- 8. the stereo fast path (eqd_steady_le2) is eqd_block's steady branch ---------------- */

static u32 g_rng = 12345;

static s16 noise16(int full)
{
	g_rng = g_rng * 1103515245u + 12345u;
	s32 v = (s32)(g_rng >> 8 & 0xFFFF) - 32768;
	return (s16)(full ? v : v / 8);
}

/* one preset through both paths for several blocks of odd sizes: same bytes, same clamps,
 * same history; full = full-scale noise (clamps), else -18 dB noise */
static u64 steady_one(const struct eqd_bands *b, int full, u32 *clamps)
{
	static const int sizes[] = { 192, 1, 7, 191, 192, 64, 3, 192 };
	s16 a[2 * 192];
	u8 le[4 * 192];
	s32 work[2 * 192];
	struct eqd_run r;
	struct eqd_state st[2];
	u64 bad = 0;

	memset(&r, 0, sizeof(r));
	r.pc = r.want = b;
	memset(st, 0, sizeof(st));
	for (u32 s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		int n = sizes[s];
		for (int i = 0; i < 2 * n; i++) {
			a[i] = noise16(full);
			le[2 * i] = (u8)a[i];
			le[2 * i + 1] = (u8)((u16)a[i] >> 8);
		}
		u32 ca = eqd_block(&r, a, n, 2, work);
		u32 cb = eqd_steady_le2(b, st, le, n, work);
		*clamps += cb;
		bad += ca != cb;
		for (int i = 0; i < 2 * n; i++)
			bad += (s16)(u16)(le[2 * i] | le[2 * i + 1] << 8) != a[i];
	}
	bad += memcmp(st, r.cur, sizeof(st)) != 0;
	return bad;
}

static void test_steady(void)
{
	u64 bad = 0;
	u32 clamps = 0, runs = 0;

	for (int full = 0; full < 2; full++) {
		for (u32 i = 0; i < g_n; i++, runs++)
			bad += steady_one(&g_b[i], full, &clamps);
		for (u32 i = 0; i < g_ncombo; i++, runs++)
			bad += steady_one(&g_cb[i], full, &clamps);
		if (g_flatp) {
			bad += steady_one(g_flatp, full, &clamps);
			runs++;
		}
	}
	printf("fast path: %u runs (every preset, %u CUSTOM combinations, FLAT; quiet and "
	       "full-scale noise, blocks of 1..192 frames), %u clamps, %llu differences\n",
	       runs, g_ncombo, clamps, (unsigned long long)bad);
	check(bad == 0 && clamps > 0, "stereo fast path bit-exact with eqd_block (samples, "
	      "clamps, history)");
}

/* ---- 9. host timing --------------------------------------------------------------------- */

static void test_time(void)
{
	s16 pcm[2 * BLOCK];
	s32 work[2 * BLOCK];
	struct eqd_run r;
	struct eqd_state st[2];
	u32 p = 0;
	for (u32 i = 0; i < g_n; i++)
		if (g_b[i].active == 0x1F)
			p = i;
	memset(&r, 0, sizeof(r));
	memset(st, 0, sizeof(st));
	r.pc = r.want = &g_b[p];
	for (int i = 0; i < 2 * BLOCK; i++)
		pcm[i] = (s16)(i * 37);
	enum { REPS = 20000 };
	clock_t t0 = clock();
	for (int k = 0; k < REPS; k++)
		eqd_block(&r, pcm, BLOCK, 2, work);
	double us = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / REPS;
	t0 = clock();
	for (int k = 0; k < REPS; k++)
		for (int i = 0; i < BLOCK; i += 192)
			eqd_steady_le2(&g_b[p], st, (u8 *)pcm + 4 * i, 192, work);
	double us2 = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / REPS;
	printf("time: %.2f us per 384-frame stereo block on this host (5 bands, %s); fast path "
	       "%.2f us (information only: the console's core is in order, measure there)\n",
	       us, g_name[p], us2);
}

int main(int argc, char **argv)
{
	const char *fp = argc > 1 ? argv[1] : "tools/presets/xw.presets";
	const char *vp = argc > 2 ? argv[2] : "dist/eq_vectors.bin";
	u32 flen, vlen, nf;
	const u8 *freqs;
	u8 *f = load(fp, &flen);
	u8 *v = load(vp, &vlen);

	test_file(f, flen);
	g_v = v;
	test_vectors(&nf, &freqs);
	test_response(nf, freqs);
	test_wrap();
	test_xfade();
	test_loudness();
	test_steady();
	test_time();
	printf(g_fail ? "FAIL\n" : "all ok\n");
	return g_fail;
}
