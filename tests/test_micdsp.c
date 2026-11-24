// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of xbox360/micauto_dsp.h (NOISE GATE AUTO, MIC VOLUME AUTO, the 24 -> 8 kHz
 * decimator, the comb canceller):
 *  - bit-exact with the integer Python model (tools/micdsp/micdsp.py) on the vectors it
 *    exported (tools/micdsp/vectors.h: rerun tools/micdsp/vectors.py after changing either);
 *  - the manual settings (NOISE GATE OFF / LOW / HIGH, MIC VOLUME 1..10, decimator off) give
 *    exactly today's output whether or not the new kernels run beside them, whatever state
 *    they hold, and today's numbers (gate_threshold 33 58 104 184 for steps 1..4, the per-step gains);
 *  - the decimator cannot overflow; and, with --bench, what each kernel costs per sample.
 *
 *   cc -O2 -Wall -o test_micdsp tests/test_micdsp.c && ./test_micdsp [--bench]
 *
 * The mic path below (DC removal, the 20 ms gate with its 400 ms hold and early opening,
 * shape(), rs_push) is
 * transcribed from voice_360.c, with the hooks placed where tools/micdsp/README.md says they
 * go. Keep it in step with voice_360.c and micdsp.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../xbox360/micauto_dsp.h"
#include "../tools/micdsp/vectors.h"

/* ---- today's mic path (voice_360.c) -------------------------------------------------------- */
enum { GATE_OFF, GATE_1, GATE_2, GATE_AUTO, GATE_3, GATE_4, GATES };
/* by stored number: off, steps 1 2 (-60 -55), AUTO, steps 3 4 (-50 -45 dBFS) */
static const u16 XD_GATE_RMS[GATES] = { 0, 33, 58, 0, 104, 184 };
#define XD_GATE_HOLD_MS 400
#define XD_GATE_ATTACK_DIV 500 /* the gate opens over 2 ms */
static const u16 XD_MIC_GAINS[10] = { 25, 35, 50, 70, 100, 140, 200, 280, 400, 560 };
#define XD_Q12 4096
#define XD_LIMIT 29204
#define XD_DC_SHIFT 10
#define RATE 24000

static s16 clamp16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }

static u32 isqrt(u32 v)
{
	u32 r = 0, b = 1u << 30;

	while (b > v)
		b >>= 2;
	while (b) {
		if (v >= r + b) {
			v -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
		b >>= 2;
	}
	return r;
}

static u32 gain_index(u32 pct)
{
	u32 i = 0;

	if (pct > 10000)
		pct = 10000;
	while (i + 1 < 10 && pct * pct > (u32)XD_MIC_GAINS[i] * XD_MIC_GAINS[i + 1])
		i++;
	return i;
}

static u32 manual_q12(u32 level) { return (u32)XD_MIC_GAINS[level - 1] * XD_Q12 / 100; }

static u32 ramp_to(u32 v, u32 to, u32 step)
{
	if (v < to)
		return to - v > step ? v + step : to;
	return v - to > step ? v - step : to;
}

struct shape_st { u32 gain, shut, lim, rel; };

static s16 shape(struct shape_st *sh, s32 x, u32 gain_to, u32 shut_to, u32 rate)
{
	u32 ramp = rate / 100 ? rate / 100 : 1, g;
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
	g = sh->gain * (32768 - sh->shut) >> 15;
	y = x * (s32)g >> 12;
	a = y < 0 ? -y : y;
	if ((u32)a * sh->lim >> 12 > XD_LIMIT) {
		sh->lim = (u32)XD_LIMIT * XD_Q12 / (u32)a;
	} else if (sh->lim < XD_Q12 && !(++sh->rel & 7)) {
		sh->lim += ((XD_Q12 - sh->lim) >> 8) + 1;
		if (sh->lim > XD_Q12)
			sh->lim = XD_Q12;
	}
	return clamp16(y * (s32)sh->lim >> 12);
}

struct rs { u32 in, out, t, n, bi; s32 prev, sum; s32 box[8]; };

static void rs_init(struct rs *r, u32 in_hz, u32 out_hz)
{
	memset(r, 0, sizeof(*r));
	r->in = 3; /* 24000 -> 8000 reduced */
	r->out = 1;
	r->n = r->in > r->out ? (r->in + r->out - 1) / r->out : 1;
	(void)in_hz, (void)out_hz;
}

static int rs_push(struct rs *r, s32 x, s16 *y)
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

/* ---- one pad's mic: today's code, plus the hooks when `hooks` (the next phase) ------------- */
struct pad {
	int hooks;
	u32 gate, level, manual, fir_on, comb_on, muted;
	s32 dc;
	u32 dc_on, closed, hold, n, opening;
	u64 sum;
	struct shape_st sh;
	struct rs rs;
	/* the new per-pad state (micdsp_360.c next phase) */
	struct ma_floor floor;
	struct ma_agc agc;
	struct ma_fir fir;
	struct ma_comb comb;
	/* results */
	u32 windows, trace_hash, out_n, out_hash;
	s32 (*trace)[7];
	u32 trace_max;
};

static u32 fnv(u32 h, u32 v)
{
	for (int k = 0; k < 4; k++) {
		h ^= (v >> (8 * k)) & 0xff;
		h *= 0x01000193u;
	}
	return h;
}

static void pad_init(struct pad *p, int hooks, u32 gate, u32 level, u32 fir, u32 comb)
{
	memset(p, 0, sizeof(*p));
	p->hooks = hooks;
	p->gate = gate;
	p->level = p->manual = level;
	p->fir_on = fir;
	p->comb_on = comb;
	rs_init(&p->rs, RATE, 8000);
	ma_floor_init(&p->floor);
	ma_agc_init(&p->agc, manual_q12(level));
	ma_fir_init(&p->fir);
	ma_comb_init(&p->comb);
	p->trace_hash = p->out_hash = 0x811C9DC5u;
}

static void pad_set_level(struct pad *p, u32 level)
{
	if (level == 0 && p->level != 0) {
		p->manual = p->level;
		ma_agc_start(&p->agc, manual_q12(p->level));
	}
	p->level = level;
}

/* mic_gain_target(): AUTO's gain, else today's */
static u32 gain_target(struct pad *p)
{
	return p->hooks && p->level == 0 ? p->agc.q12 : manual_q12(p->level);
}

static void out_sample(struct pad *p, s16 y)
{
	p->out_hash = fnv(p->out_hash, (u32)(s32)y);
	p->out_n++;
}

/* xv_mic_samples for one chunk of raw mono samples */
static void pad_chunk(struct pad *p, const s16 *in, int frames)
{
	s16 mono[32];
	u32 thr = p->gate < GATES ? XD_GATE_RMS[p->gate] : 0;

	for (int i = 0; i < frames; i++) { /* mic_clean, then the comb (hook) */
		s32 x = in[i];

		if (!p->dc_on) {
			p->dc = x * (1 << 12);
			p->dc_on = 1;
		}
		p->dc += (x * (1 << 12) - p->dc) >> XD_DC_SHIFT;
		x = clamp16(x - (p->dc >> 12));
		if (p->hooks && p->comb_on)
			x = ma_comb_push(&p->comb, x);
		mono[i] = (s16)x;
	}
	if (p->hooks && p->gate == GATE_AUTO)
		thr = p->floor.thr_rms;
	if (!thr)
		p->closed = p->hold = p->opening = 0;
	for (int i = 0; i < frames; i++) { /* mic_analyse */
		s32 x = mono[i];

		p->sum += (u32)(x * x);
		if (p->hold)
			p->hold--;
		if (++p->n < RATE / 50)
			continue;
		{
			u32 ms = (u32)(p->sum / p->n), rms = isqrt(ms);

			if (p->hooks && p->gate == GATE_AUTO)
				thr = p->floor.thr_rms;
			p->opening = 0;
			if (thr && rms >= thr) {
				p->closed = 0;
				p->hold = RATE / 1000 * XD_GATE_HOLD_MS;
			} else if (thr && !p->hold) {
				p->closed = 1;
			}
			if (p->hooks) {
				ma_agc_window(&p->agc, ms, &p->floor, !thr || rms >= thr, !p->muted,
					      p->level == 0 && !p->muted);
				ma_floor_window(&p->floor, ms);
			}
			{
				s32 row[7] = { (s32)rms, (s32)thr, !p->closed, p->floor.floor_db,
					       p->floor.thr_db, (s32)p->agc.q12, p->agc.speech_db };

				for (int k = 0; k < 7; k++)
					p->trace_hash = fnv(p->trace_hash, (u32)row[k]);
				if (p->trace && p->windows < p->trace_max)
					memcpy(p->trace[p->windows], row, sizeof(row));
			}
			p->windows++;
		}
		p->sum = 0;
		p->n = 0;
	}
	/* opening early: the window so far already reaches the threshold it will be judged by */
	if (p->hooks && p->gate == GATE_AUTO)
		thr = p->floor.thr_rms;
	if (thr && p->closed && !p->opening && p->sum >= (u64)thr * thr * (RATE / 50))
		p->opening = 1;
	for (int i = 0; i < frames; i++) { /* mic_shaped / silence, then the decimation */
		s16 y = 0, o[16];

		if (!p->muted)
			y = shape(&p->sh, mono[i], gain_target(p), p->closed && !p->opening ? 32768 : 0,
				  RATE);
		if (p->hooks && p->fir_on) {
			if (ma_fir_push(&p->fir, y, o))
				out_sample(p, o[0]);
		} else {
			int k = rs_push(&p->rs, y, o);

			for (int j = 0; j < k; j++)
				out_sample(p, o[j]);
		}
	}
}

/* ---- the shared input generator (vectors.py gen) ------------------------------------------- */
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

static void run(struct pad *p, const s16 *x, const struct mv_scen *sc)
{
	for (u32 c = 0; c < MV_SAMPLES; c += 24) {
		u32 ms = c / 24;

		if (sc && sc->level == 0 && ms == MV_AUTO_AT)
			pad_set_level(p, 0);
		p->muted = sc && sc->mute && ms >= MV_MUTE_FROM && ms < MV_MUTE_TO;
		pad_chunk(p, x + c, 24);
	}
}

static int g_fail;

static void check(int ok, const char *fmt, const char *what)
{
	printf("%s %s %s\n", ok ? "ok  " : "FAIL", fmt, what);
	g_fail |= !ok;
}

static void bench(const s16 *x)
{
	enum { N = 24000 * 60 };
	struct ma_fir f;
	struct ma_comb c;
	struct ma_floor fl;
	struct ma_agc a;
	struct rs r;
	s16 o[16];
	volatile s32 sink = 0;
	clock_t t0;

	ma_fir_init(&f);
	t0 = clock();
	for (int i = 0; i < N; i++)
		if (ma_fir_push(&f, x[i % MV_SAMPLES], o))
			sink += o[0];
	printf("bench: ma_fir_push %.2f ns per 24 kHz sample", 1e9 * (clock() - t0) / CLOCKS_PER_SEC / N);
	rs_init(&r, RATE, 8000);
	t0 = clock();
	for (int i = 0; i < N; i++)
		sink += rs_push(&r, x[i % MV_SAMPLES], o);
	printf(" (rs_push %.2f)", 1e9 * (clock() - t0) / CLOCKS_PER_SEC / N);
	ma_comb_init(&c);
	t0 = clock();
	for (int i = 0; i < N; i++)
		sink += ma_comb_push(&c, x[i % MV_SAMPLES]);
	printf("; ma_comb_push %.2f", 1e9 * (clock() - t0) / CLOCKS_PER_SEC / N);
	ma_floor_init(&fl);
	ma_agc_init(&a, 4096);
	t0 = clock();
	for (int i = 0; i < N / 480; i++) {
		u32 ms = (u32)(x[(i * 480) % MV_SAMPLES] * x[(i * 480) % MV_SAMPLES]);

		ma_agc_window(&a, ms, &fl, 1, 1, 1);
		ma_floor_window(&fl, ms);
		sink += (s32)(a.q12 + fl.thr_rms);
	}
	printf("; floor + AGC %.1f ns per 20 ms window\n",
	       1e9 * (clock() - t0) / CLOCKS_PER_SEC / (N / 480));
	(void)sink;
}

int main(int argc, char **argv)
{
	s16 *x = gen();
	char what[160];

	/* the log / exp helpers and the decimator's impulse response, against the model */
	{
		int ok = 1;

		for (u32 i = 0; i < sizeof(mv_ms) / sizeof(mv_ms[0]); i++)
			ok &= ma_db_ms(mv_ms[i]) == mv_db_ms[i];
		for (u32 i = 0; i < sizeof(mv_db) / sizeof(mv_db[0]); i++)
			ok &= ma_lin(mv_db[i], 15) == mv_lin15[i] && ma_lin(mv_db[i], 12) == mv_lin12[i];
		for (u32 i = 0; i < 10; i++)
			ok &= ma_db_q12(manual_q12(i + 1)) == mv_db_q12[i];
		check(ok, "ma_db_ms / ma_lin / ma_db_q12", "match the model");
	}
	{
		struct ma_fir f;
		s16 y, imp[64];
		int n = 0, ok;

		ma_fir_init(&f);
		for (int i = 0; i < 60; i++)
			if (ma_fir_push(&f, i == 0 ? 32767 : 0, &y))
				imp[n++] = y;
		ok = n == (int)(sizeof(mv_fir_imp) / sizeof(mv_fir_imp[0])) &&
		     !memcmp(imp, mv_fir_imp, sizeof(mv_fir_imp));
		check(ok, "decimator impulse response", "matches the model");
	}
	{
		/* worst case for the s32 sum: every tap's input at full scale with the tap's sign
		 * (45 pushes from phase 0: the last one makes an output) */
		struct ma_fir f;
		s16 y = 0;
		long long sum = 16384;
		int ok = 0;

		ma_fir_init(&f);
		for (int k = MA_FIR_TAPS - 1; k >= 0; k--) {
			s32 v = ma_fir_dec3[k] < 0 ? -32768 : 32767;

			sum += (long long)ma_fir_dec3[k] * v;
			ok = ma_fir_push(&f, v, &y);
		}
		snprintf(what, sizeof(what), "(sum %lld < 2^31; output %d)", sum, y);
		check(ok && sum < (1LL << 31) && y == (sum >> 15 > 32767 ? 32767 : sum >> 15),
		      "decimator worst-case input does not overflow", what);
	}
	{
		struct ma_comb c;
		u32 h = 0x811C9DC5u;

		ma_comb_init(&c);
		for (int i = 0; i < 4000; i++)
			h = fnv(h, (u32)ma_comb_push(&c, x[i]));
		check(h == MV_COMB_HASH, "comb canceller", "matches the model");
	}

	/* every scenario through the mic path, against the model */
	for (u32 s = 0; s < sizeof(mv_scen) / sizeof(mv_scen[0]); s++) {
		const struct mv_scen *sc = &mv_scen[s];
		struct pad *p = calloc(1, sizeof(*p));
		int ok;

		pad_init(p, 1, sc->gate, sc->manual, sc->fir, sc->comb);
		if (s == 0) {
			p->trace = calloc(sc->windows, sizeof(*p->trace));
			p->trace_max = sc->windows;
		}
		run(p, x, sc);
		ok = p->windows == sc->windows && p->trace_hash == sc->trace_hash &&
		     p->out_n == sc->out_n && p->out_hash == sc->out_hash;
		if (s == 0) {
			for (u32 w = 0; w < sc->windows; w++)
				if (memcmp(p->trace[w], mv_trace0[w], sizeof(mv_trace0[w]))) {
					printf("     window %u differs: rms %d thr %d open %d floor %d thr_db %d"
					       " q12 %d speech %d, model %d %d %d %d %d %d %d\n", w,
					       p->trace[w][0], p->trace[w][1], p->trace[w][2], p->trace[w][3],
					       p->trace[w][4], p->trace[w][5], p->trace[w][6], mv_trace0[w][0],
					       mv_trace0[w][1], mv_trace0[w][2], mv_trace0[w][3], mv_trace0[w][4],
					       mv_trace0[w][5], mv_trace0[w][6]);
					ok = 0;
					break;
				}
			free(p->trace);
		}
		snprintf(what, sizeof(what), "(%u windows, %u samples at 8 kHz)", p->windows, p->out_n);
		printf("%s scenario %-12s bit-exact with the model %s\n", ok ? "ok  " : "FAIL", sc->name, what);
		g_fail |= !ok;
		free(p);
	}

	/* the manual settings: today's output, with or without the new kernels beside them */
	for (u32 gate = GATE_OFF; gate < GATES; gate++) {
		if (gate == GATE_AUTO)
			continue; /* not a manual setting */
		for (u32 level = 1; level <= 10; level++) {
			struct pad *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
			struct pad *c = calloc(1, sizeof(*c));
			int ok;

			pad_init(a, 0, gate, level, 0, 0);    /* today's code alone */
			pad_init(b, 1, gate, level, 0, 0);    /* with the hooks */
			pad_init(c, 1, gate, level, 0, 0);    /* with the hooks, their state scrambled */
			c->floor.thr_rms = 1;
			c->floor.floor_db = 0;
			c->agc.q12 = MA_AGC_MAX_Q12;
			c->agc.g = 0;
			run(a, x, NULL);
			run(b, x, NULL);
			run(c, x, NULL);
			ok = a->out_hash == b->out_hash && a->out_hash == c->out_hash &&
			     a->out_n == b->out_n && a->trace_hash != 0;
			if (!ok || level == 10) {
				snprintf(what, sizeof(what), "gate %u (stored number), MIC VOLUME 1..%u",
					 gate, level);
				check(ok, "manual settings: today's output, untouched by the new code:", what);
			}
			free(a), free(b), free(c);
		}
	}
	{
		int ok = ma_lin(-60 * MA_DB, 15) == XD_GATE_RMS[GATE_1] &&
			 ma_lin(-55 * MA_DB, 15) == XD_GATE_RMS[GATE_2] &&
			 ma_lin(-50 * MA_DB, 15) == XD_GATE_RMS[GATE_3] &&
			 ma_lin(-45 * MA_DB, 15) == XD_GATE_RMS[GATE_4] &&
			 ma_lin(MA_FLOOR_INIT_DB + MA_GATE_MARGIN_DB, 15) == XD_GATE_RMS[GATE_4];
		static const u32 q15[10] = { 8192, 11464, 16384, 22936, 32768, 45872, 65536, 91744,
					     131072, 183496 };

		for (u32 l = 1; l <= 10; l++)
			ok &= manual_q12(l) * (32768 / XD_Q12) == q15[l - 1] &&
			      gain_index(manual_q12(l) * 100 / XD_Q12) + 1 == l;
		check(ok, "gate_threshold 33 / 58 / 104 / 184 (-60 -55 -50 -45 dBFS; AUTO starts at step 4's), "
		      "mic_gain_q15 per step, level <-> gain", "as today");
	}
	{
		/* AUTO's start: exactly the manual gain in effect, no jump */
		struct ma_agc a;
		int ok = 1;

		for (u32 l = 1; l <= 10; l++) {
			ma_agc_init(&a, 4096);
			ma_agc_start(&a, manual_q12(l));
			ok &= a.q12 == manual_q12(l);
		}
		check(ok, "MIC VOLUME AUTO starts at the manual gain", "for each of 1..10");
	}
	if (argc > 1 && !strcmp(argv[1], "--bench"))
		bench(x);
	free(x);
	printf(g_fail ? "FAILED\n" : "all passed\n");
	return g_fail;
}
