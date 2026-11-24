/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The integer DSP behind NOISE GATE AUTO, MIC VOLUME AUTO, the 24 -> 8 kHz chat decimator and
 * the optional 8 ms comb canceller. Pure functions on state the caller owns: no globals, no
 * kernel calls, no floating point (the plugin is built -msoft-float and checked for FP
 * instructions; a float here faults on the console). No locking: the state belongs to the
 * thread that calls (the driver thread in the plugin).
 *
 * Bit-exact with the Python model tools/micdsp/micdsp.py, which reads its constants and tables
 * from this file; tests/test_micdsp.c checks the two against each other. Change a constant
 * here, then rerun tools/micdsp/vectors.py and the test. tools/micdsp/README.md says where each
 * kernel hooks into the mic path and why the constants are what they are.
 *
 * Units: levels are "Q8 dB" (MA_DB = 1 dB) relative to full scale. A window is 20 ms of the mic
 * after the DC removal and before any gain, given as its mean square (sum x^2 / n; full scale
 * 32768^2 = 2^30): the same measurement, at the same point, as the manual gate's.
 *
 * Purely additive: nothing in here is used by the manual settings (NOISE GATE OFF / steps 1..4,
 * MIC VOLUME 1..10) or with the decimator and comb switched off.
 */
#ifndef MICAUTO_DSP_H
#define MICAUTO_DSP_H

#include "../core/compat.h"

/* The functions that run once per 20 ms window (levels, floor tracker, AGC) are MICAUTO_SLOW:
 * by default `static inline`, as everything here. A caller that wants them once, out of line
 * and small (the plugin: they run 50 times a second, where size matters more than a few ns)
 * defines MICAUTO_SLOW before including this file. The per-sample kernels (decimator, comb)
 * stay inline. Either way the arithmetic is the same. (Not MA_*: micdsp.py reads every MA_*
 * define as a number.) */
#ifndef MICAUTO_SLOW
#define MICAUTO_SLOW static inline
#endif

#define MA_DB 256                 /* 1 dB in Q8 dB */
#define MA_DB_SILENT (-100 * MA_DB) /* the level of an all-zero window */

/* ---- NOISE GATE AUTO: minimum-statistics noise floor -------------------------------------
 * Each window's level is smoothed (sm += (level - sm) / 2^MA_FLOOR_SMOOTH: ~320 ms), the lowest
 * smoothed level is kept per block of MA_FLOOR_BLOCK_WIN windows, and the floor is the lowest of
 * the last MA_FLOOR_BLOCKS blocks and the block being filled: it follows a quieter room at once
 * and a louder one after 3.5 .. 4 s. Talk always has pauses of a few hundred ms within 3 s, so
 * the floor stays in them while someone talks.
 * The threshold is floor + MA_GATE_MARGIN_DB, clamped. Smoothing and margin are a pair: the
 * minimum of raw 20 ms levels of a fluctuating noise (pink noise, a room's babble) sits 6..9 dB
 * under its mean, so floor + 8 dB without smoothing opened on such noise 30..90 % of the time;
 * smoothed, + 9 dB lands just above the noise's peaks (tools/micdsp/README.md has the numbers).
 * MA_GATE_MIN_DB: the headset's own idle noise peaks at -63 dBFS (1 s raw recording), and quiet
 * speech starts at -55 (the owner's reading: none of it under -60 was lost at -60). In a quiet
 * room the threshold sits on this clamp. MA_GATE_MAX_DB: a room this loud (floor -44) is
 * shut out at the cost of a quiet talker's words; above it normal speech is cut. The gate
 * itself (hold, ramps, opening early) is the manual one (micdsp_360.c). */
#define MA_FLOOR_SMOOTH 4
#define MA_FLOOR_BLOCK_WIN 25     /* 0.5 s */
#define MA_FLOOR_BLOCKS 6         /* x 0.5 s = 3 s of history */
/* before any history: threshold -45 dBFS (= step 4). A quieter room pulls the floor down at its
 * first window, so this only matters in a loud one, which is then gated from the start: at
 * -64 (step 2's -55) AUTO stood open on a -50 dBFS room for its first 3.5 s. */
#define MA_FLOOR_INIT_DB (-54 * MA_DB)
#define MA_GATE_MARGIN_DB (9 * MA_DB)
#define MA_GATE_MIN_DB (-60 * MA_DB)
#define MA_GATE_MAX_DB (-35 * MA_DB)

/* ---- MIC VOLUME AUTO: slow feed-forward AGC ------------------------------------------------
 * Which windows count as speech: at least MA_AGC_SNR_DB over the floor and over NOISE GATE
 * AUTO's threshold, whatever the gate is set to (with step 4, a quiet talker's syllables
 * under -45 dBFS still count, so his level is not overestimated).
 * The speech level is the mean of their levels in dB over the last 2^MA_AGC_AVG_SHIFT of them
 * (a plain running mean while fewer have been seen), each clipped to MA_AGC_CLIP_UP_DB above and
 * MA_AGC_CLIP_DOWN_DB below the level so far. The clip is what keeps it still: a 2.5 s shout at
 * +12 dB moved a plain power mean by ~10 dB (the gain then dipped 4 dB and took ~30 s to come
 * back); clipped, it moves it ~1 dB, inside the dead band. A dB mean sits ~1.5 dB under the
 * active speech level, hence MA_AGC_TARGET_DB: speech comes out at about -30 dBFS.
 * It is measured before the gain, so the gain cannot feed back into it: no loop to oscillate.
 * The gain heads for MA_AGC_TARGET_DB - speech level, within the manual range (MIC VOLUME
 * 1 .. 10, 25 .. 560 %). It moves only on a speech window during which the gate in effect is
 * open: frozen in pauses, while the gate is shut and while muted, so nothing swells up in a
 * pause. It starts when the error exceeds MA_AGC_DEAD_DB and then goes all the way
 * (hysteresis: no hunting). Speed per speech window: MA_AGC_FAST for the first
 * MA_AGC_ACQUIRE_WIN after AUTO starts (a new player is right within a few seconds), then
 * MA_AGC_SLOW (~1 dB/s). The clip makes a lasting change of level slow to follow (+12 dB: the
 * gain is 6 dB lower after 30 s); the limiter guards the peaks meanwhile. */
#define MA_AGC_TARGET_DB (-63 * MA_DB / 2) /* -31.5 */
#define MA_AGC_SNR_DB (12 * MA_DB)
#define MA_AGC_AVG_SHIFT 8        /* 256 speech windows: 5 s of speech */
#define MA_AGC_CLIP_UP_DB (6 * MA_DB)
#define MA_AGC_CLIP_DOWN_DB (10 * MA_DB)
#define MA_AGC_CLIP_AFTER 8       /* the first windows set the level unclipped */
#define MA_AGC_MIN_WIN 25         /* no move before 0.5 s of speech has been heard */
#define MA_AGC_DEAD_DB (1 * MA_DB)
#define MA_AGC_SLOW 5             /* Q8 dB per speech window: 0.98 dB/s */
#define MA_AGC_FAST 25            /* 4.9 dB/s */
#define MA_AGC_ACQUIRE_WIN 150    /* 3 s of speech */
#define MA_AGC_MIN_Q12 1024       /* 25 %: MIC VOLUME 1 */
#define MA_AGC_MAX_Q12 22937      /* 560 %: MIC VOLUME 10 */

/* ---- 24 -> 8 kHz decimator ---------------------------------------------------------------
 * 45-tap linear-phase low-pass (tools/micdsp/design.py): passband 0..3.2 kHz +0.00..+0.35 dB,
 * at least 61 dB down from 4.6 kHz, delay 22 samples (0.92 ms). Symmetric; sum |h| = 57776, so
 * a full-scale input cannot overflow the s32 sum (needs < 65536). */
#define MA_FIR_TAPS 45
static const s16 ma_fir_dec3[45] = {
	30, 29, -28, -146, -245, -206, 13, 279, 343, 62, -404,
	-628, -274, 517, 1078, 709, -613, -1926, -1783, 676, 4855, 8820,
	10448, 8820, 4855, 676, -1783, -1926, -613, 709, 1078, 517, -274,
	-628, -404, 62, 343, 279, 13, -206, -245, -146, -28, 29,
	30,
};

/* ---- comb canceller (optional, off by default) --------------------------------------------
 * The raw mic carries a faint comb at 125 Hz spacing (-75 dBFS): something repeating every
 * USB block, 192 samples at 24 kHz. A per-phase running average over the last ~2^MA_COMB_SHIFT
 * periods (0.5 s) learns it and subtracts it. It only works at 24 kHz with the mic's samples
 * in step with the USB blocks; a lost packet shifts the phase and it re-learns in ~0.5 s. A
 * voice holding exactly a multiple of 125 Hz for half a second would be dented. */
#define MA_COMB_PER 192
#define MA_COMB_SHIFT 6

/* log2(1 + i/32) in Q12, and 2^(i/32) in Q15 (design.py) */
static const u16 ma_log2_tab[33] = {
	0, 182, 358, 530, 696, 858, 1016, 1169, 1319, 1465, 1607,
	1746, 1882, 2015, 2145, 2272, 2396, 2518, 2637, 2754, 2869, 2982,
	3092, 3200, 3307, 3412, 3514, 3615, 3715, 3812, 3908, 4003, 4096,
};
static const u32 ma_exp2_tab[33] = {
	32768, 33486, 34219, 34968, 35734, 36516, 37316, 38133, 38968, 39821, 40693,
	41584, 42495, 43425, 44376, 45348, 46341, 47356, 48393, 49452, 50535, 51642,
	52773, 53928, 55109, 56316, 57549, 58809, 60097, 61413, 62757, 64132, 65536,
};

/* log2(v) in Q12, v > 0 (error < 0.0005) */
MICAUTO_SLOW s32 ma_log2_q12(u32 v)
{
	s32 e = 31 - __builtin_clz(v);
	u32 m = e >= 15 ? v >> (e - 15) : v << (15 - e); /* 32768..65535 */
	u32 f = m - 32768, i = f >> 10, r = f & 1023;

	return e * 4096 + ma_log2_tab[i] +
	       (s32)(((ma_log2_tab[i + 1] - ma_log2_tab[i]) * r + 512) >> 10);
}

/* Q12 log2 (relative to a reference, |l2| < 2^17) to Q8 dB of a power: x 10 log10(2) x 256 /
 * 4096 = 0.188144 (12330 / 65536) */
MICAUTO_SLOW s32 ma_db_of_log2(s32 l2)
{
	return (l2 * 12330 + 32768) >> 16;
}

/* a mean square (full scale 2^30) in Q8 dBFS */
MICAUTO_SLOW s32 ma_db_ms(u32 ms)
{
	s32 d;

	if (!ms)
		return MA_DB_SILENT;
	d = ma_db_of_log2(ma_log2_q12(ms) - 30 * 4096);
	return d < MA_DB_SILENT ? MA_DB_SILENT : d;
}

/* a Q12 gain (4096 = 0 dB) in Q8 dB */
MICAUTO_SLOW s32 ma_db_q12(u32 q12)
{
	return ma_db_of_log2(ma_log2_q12(q12 * q12) - 24 * 4096); /* q12 <= 65535 */
}

/* round(2^shift x 10^(db / 20)): an amplitude, db in Q8 dB (shift 15: an RMS, full scale
 * 32768; shift 12: a Q12 gain). 0 below 2^-16 of the unit; saturates at 2^31. */
MICAUTO_SLOW u32 ma_lin(s32 db, u32 shift)
{
	s32 v = ((db * 21771) >> 13) + (s32)(shift * 4096); /* log2 in Q12: x log2(10) / 20 / 256 x 4096 */
	s32 ip = v >> 12;
	u32 fr = (u32)v & 4095, i = fr >> 7, r = fr & 127;
	u32 m = ma_exp2_tab[i] + (((ma_exp2_tab[i + 1] - ma_exp2_tab[i]) * r + 64) >> 7);

	if (ip < -1)
		return 0;
	if (ip >= 16)
		return 0x80000000u;
	return ip >= 15 ? m << (ip - 15) : (m + (1u << (14 - ip))) >> (15 - ip);
}

/* ---- the floor tracker ------------------------------------------------------------------ */
struct ma_floor {
	s32 sm;                   /* the smoothed window level */
	s32 cur;                  /* the lowest sm of the block being filled */
	s32 blk[MA_FLOOR_BLOCKS]; /* the lowest sm of each of the last blocks */
	u32 n, bi;                /* windows in the block being filled; the oldest block's slot */
	s32 floor_db, thr_db;     /* the floor and NOISE GATE AUTO's threshold, Q8 dBFS */
	u32 thr_rms;              /* thr_db as a window RMS (full scale 32768): what the gate uses */
	u32 started;
};

MICAUTO_SLOW void ma_floor_set_thr(struct ma_floor *f)
{
	s32 t = f->floor_db + MA_GATE_MARGIN_DB;

	f->thr_db = t < MA_GATE_MIN_DB ? MA_GATE_MIN_DB : t > MA_GATE_MAX_DB ? MA_GATE_MAX_DB : t;
	f->thr_rms = ma_lin(f->thr_db, 15);
}

MICAUTO_SLOW void ma_floor_init(struct ma_floor *f)
{
	memset(f, 0, sizeof(*f));
	for (int i = 0; i < MA_FLOOR_BLOCKS; i++)
		f->blk[i] = MA_FLOOR_INIT_DB;
	f->cur = f->floor_db = MA_FLOOR_INIT_DB;
	ma_floor_set_thr(f);
}

/* one 20 ms window's mean square. The threshold it sets applies from the next window. */
MICAUTO_SLOW void ma_floor_window(struct ma_floor *f, u32 ms)
{
	s32 d = ma_db_ms(ms), lo;

	if (!f->started) {
		f->sm = d; /* start where the level is, not from the initial guess */
		f->started = 1;
	} else {
		f->sm += (d - f->sm) >> MA_FLOOR_SMOOTH;
	}
	if (f->n == 0 || f->sm < f->cur)
		f->cur = f->sm;
	if (++f->n == MA_FLOOR_BLOCK_WIN) {
		f->blk[f->bi] = f->cur;
		if (++f->bi == MA_FLOOR_BLOCKS)
			f->bi = 0;
		f->n = 0;
	}
	lo = f->cur;
	for (int i = 0; i < MA_FLOOR_BLOCKS; i++)
		if (f->blk[i] < lo)
			lo = f->blk[i];
	f->floor_db = lo;
	ma_floor_set_thr(f);
}

/* ---- the AGC ---------------------------------------------------------------------------- */
struct ma_agc {
	s32 lvl;        /* the speech level, Q16 dBFS (Q8 dB x 256: the average's fraction) */
	u32 n;          /* speech windows learnt (stops at 2^MA_AGC_AVG_SHIFT) */
	s32 speech_db;  /* the level of e, Q8 dBFS (MA_DB_SILENT before any speech) */
	s32 g;          /* the gain, Q8 dB */
	u32 q12;        /* the same as the Q12 target of shape()'s 10 ms ramp */
	u32 fast;       /* speech windows left at MA_AGC_FAST */
	u32 moving;     /* heading for the target (until within one step of it) */
};

/* (re)start AUTO from the manual gain in effect (Q12): no jump; the speech level learnt so far
 * is kept (it does not depend on the gain) */
MICAUTO_SLOW void ma_agc_start(struct ma_agc *a, u32 manual_q12)
{
	a->q12 = manual_q12 < MA_AGC_MIN_Q12 ? MA_AGC_MIN_Q12 :
		 manual_q12 > MA_AGC_MAX_Q12 ? MA_AGC_MAX_Q12 : manual_q12;
	a->g = ma_db_q12(a->q12);
	a->fast = MA_AGC_ACQUIRE_WIN;
	a->moving = 0;
}

MICAUTO_SLOW void ma_agc_init(struct ma_agc *a, u32 manual_q12)
{
	memset(a, 0, sizeof(*a));
	a->speech_db = MA_DB_SILENT;
	ma_agc_start(a, manual_q12);
}

/* one 20 ms window's mean square, with the floor tracker as it was before this window.
 * gate_open: the window's RMS reached the threshold of the gate in effect (always, gate OFF);
 * learn: not muted; adapt: MIC VOLUME AUTO and not muted. Returns the Q12 gain (a->q12). */
MICAUTO_SLOW u32 ma_agc_window(struct ma_agc *a, u32 ms, const struct ma_floor *f,
				bool gate_open, bool learn, bool adapt)
{
	s32 d = ma_db_ms(ms), want, err, step, lo, hi;
	u32 k = 0;

	if (!learn || d < f->floor_db + MA_AGC_SNR_DB || d < f->thr_db)
		return a->q12;
	while (k < MA_AGC_AVG_SHIFT && (1u << (k + 1)) <= a->n + 1)
		k++; /* floor(log2(n + 1)): a running mean at first (n = 0: the level is d) */
	if (a->n >= MA_AGC_CLIP_AFTER) {
		s32 l = a->lvl >> 8;

		d = d > l + MA_AGC_CLIP_UP_DB ? l + MA_AGC_CLIP_UP_DB :
		    d < l - MA_AGC_CLIP_DOWN_DB ? l - MA_AGC_CLIP_DOWN_DB : d;
	}
	a->lvl += (d * 256 - a->lvl) >> k;
	if (a->n < (1u << MA_AGC_AVG_SHIFT))
		a->n++;
	a->speech_db = a->lvl >> 8;
	if (!adapt || !gate_open || a->n < MA_AGC_MIN_WIN)
		return a->q12;
	lo = ma_db_q12(MA_AGC_MIN_Q12);
	hi = ma_db_q12(MA_AGC_MAX_Q12);
	want = MA_AGC_TARGET_DB - a->speech_db;
	want = want < lo ? lo : want > hi ? hi : want;
	err = want - a->g;
	step = a->fast ? MA_AGC_FAST : MA_AGC_SLOW;
	if (a->fast)
		a->fast--;
	if (!a->moving && (err > MA_AGC_DEAD_DB || err < -MA_AGC_DEAD_DB))
		a->moving = 1;
	if (!a->moving)
		return a->q12;
	if (err <= step && err >= -step) {
		a->g = want;
		a->moving = 0;
	} else {
		a->g += err > 0 ? step : -step;
	}
	a->q12 = ma_lin(a->g, 12);
	if (a->q12 < MA_AGC_MIN_Q12)
		a->q12 = MA_AGC_MIN_Q12;
	if (a->q12 > MA_AGC_MAX_Q12)
		a->q12 = MA_AGC_MAX_Q12;
	return a->q12;
}

/* ---- the decimator: polyphase, one output per three inputs ------------------------------ */
struct ma_fir {
	s16 x[2 * MA_FIR_TAPS]; /* each input twice (pos and pos + TAPS): a straight window */
	u32 pos, ph;
};

static inline void ma_fir_init(struct ma_fir *f)
{
	memset(f, 0, sizeof(*f));
}

/* one 24 kHz sample in; returns 1 with an 8 kHz sample in *y every third call, else 0 */
static inline int ma_fir_push(struct ma_fir *f, s32 x, s16 *y)
{
	const s16 *w;
	s32 acc;

	f->pos = f->pos ? f->pos - 1 : MA_FIR_TAPS - 1;
	x = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
	f->x[f->pos] = f->x[f->pos + MA_FIR_TAPS] = (s16)x;
	if (++f->ph < 3)
		return 0;
	f->ph = 0;
	w = f->x + f->pos; /* w[0] the newest .. w[TAPS - 1] the oldest */
	acc = 16384 + ma_fir_dec3[MA_FIR_TAPS / 2] * w[MA_FIR_TAPS / 2];
	for (int k = 0; k < MA_FIR_TAPS / 2; k++)
		acc += ma_fir_dec3[k] * (w[k] + w[MA_FIR_TAPS - 1 - k]); /* symmetric: one multiply */
	acc >>= 15;
	*y = (s16)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
	return 1;
}

/* ---- the comb canceller ----------------------------------------------------------------- */
struct ma_comb {
	s32 avg[MA_COMB_PER]; /* per phase of the 8 ms block, Q8 */
	u32 k;
};

static inline void ma_comb_init(struct ma_comb *c)
{
	memset(c, 0, sizeof(*c));
}

static inline s32 ma_comb_push(struct ma_comb *c, s32 x)
{
	s32 *a = &c->avg[c->k], y;

	*a += (x * 256 - *a) >> MA_COMB_SHIFT;
	if (++c->k == MA_COMB_PER)
		c->k = 0;
	y = x - ((*a + 128) >> 8);
	return y > 32767 ? 32767 : y < -32768 ? -32768 : y;
}

#endif /* MICAUTO_DSP_H */
