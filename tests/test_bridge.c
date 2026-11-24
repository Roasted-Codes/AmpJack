// SPDX-License-Identifier: GPL-3.0-or-later
/* Host simulation of the PC bridge's arithmetic (xbox360/bridge_dsp.h): a PC on its own clock
 * sends packets of a sine; the console's net thread drains them once per video frame (as the
 * system window lets it, docs/INTERNALS.md) into a jitter buffer; the driver thread reads it
 * at the headset's pace and mixes it into a block. Scenarios: clean, the PC's clock 200 ppm
 * fast and slow, loss, reordering, a network stall (an underrun: it must fade to silence and
 * back without a click), the PC stopping, the 1 ms mic-chunk reader, the send ring, the mix.
 *
 *   cc -O2 -Wall -Wextra -Itests/host -o /tmp/t tests/test_bridge.c -lm && /tmp/t
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../xbox360/bridge_dsp.h"

static int g_fail;

#define CHECK(cond, ...)                                                                   \
	do {                                                                               \
		if (!(cond)) {                                                             \
			g_fail++;                                                          \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                       \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
		}                                                                          \
	} while (0)

/* ---- the simulation ---------------------------------------------------------------------- */
struct pkt {
	u64 deliver_us;   /* when the console's stack has it */
	u32 seq, ts, n;
	u8 pcm[2 * 2 * 480];
};

#define QMAX 4096
struct sim {
	/* the scenario */
	u32 in_rate, in_ch, pkt_frames;   /* the PC's stream */
	u32 out_rate, out_ch, block;      /* the reader: block frames per call */
	u32 block_us;                     /* how often the reader is called (the headset's clock) */
	s32 pc_ppm;                       /* the PC's clock against the console's */
	u32 net_us;                       /* the net thread's period */
	u32 target_ms;
	u32 secs;
	double loss;                      /* fraction of packets dropped */
	u32 reorder_every;                /* swap two packets' delivery every N (0 none) */
	u32 stall_at_ms, stall_ms;        /* a network stall: packets held, then delivered */
	u32 stop_at_ms;                   /* the PC stops sending (0 never) */
	double amp;
	u32 reader_lag_ms;                /* the reader's clock is this far behind the writer's */
	/* state */
	struct pkt q[QMAX];
	u32 qn;
	u32 sent, dropped, swaps;
	double phase;
	/* results */
	s32 *out;                         /* every output sample (left channel) */
	u32 out_n;
	u32 max_delta_after_start;
	u32 settled_underruns, settled_overruns;
	s32 fill_min, fill_max;
	s32 ppm_last;
};

static u32 rnd_state = 12345;
static double rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return (double)(rnd_state >> 8) / (double)(1u << 24);
}

static void make_packet(struct sim *s, struct pkt *p, u32 seq, u32 ts)
{
	p->seq = seq;
	p->ts = ts;
	p->n = s->pkt_frames;
	for (u32 f = 0; f < p->n; f++) {
		double v = s->amp * sin(s->phase);
		s32 x = (s32)lrint(v);

		s->phase += 2 * M_PI * 440.0 / s->in_rate;
		for (u32 c = 0; c < s->in_ch; c++)
			xb_put_be16(p->pcm + 2 * (f * s->in_ch + c), x);
	}
}

static void run(struct sim *s, struct xb_jb *j)
{
	u64 t_end = (u64)s->secs * 1000000, t_send = 0, t_net = 0, t_read = 0;
	/* the PC's packet period in console microseconds: its clock runs pc_ppm fast */
	double pkt_us = (double)s->pkt_frames * 1e6 / s->in_rate / (1.0 + s->pc_ppm * 1e-6);
	double next_send = 0;
	u32 seq = 1000, ts = 777;
	s32 prev = 0;
	int started = 0;
	s32 *blk = malloc(sizeof(s32) * XB_MAX_BLOCK * 2);
	u8 *mix = malloc(4 * XB_MAX_BLOCK);

	s->out = malloc(sizeof(s32) * (size_t)(s->secs + 1) * s->out_rate);
	s->out_n = 0;
	s->fill_min = 1 << 30;
	s->fill_max = -(1 << 30);
	for (u64 t = 0; t < t_end; t++) {
		/* the PC */
		if (t >= (u64)next_send && (!s->stop_at_ms || t < (u64)s->stop_at_ms * 1000)) {
			struct pkt p;
			u64 at = t + 300 + (u64)(rnd() * 700); /* LAN jitter, under 1 ms */

			next_send += pkt_us;
			make_packet(s, &p, seq++, ts);
			ts += p.n;
			s->sent++;
			if (s->stall_ms && t >= (u64)s->stall_at_ms * 1000 &&
			    t < (u64)(s->stall_at_ms + s->stall_ms) * 1000)
				at = (u64)(s->stall_at_ms + s->stall_ms) * 1000 + 100;
			if (s->loss > 0 && rnd() < s->loss) {
				s->dropped++;
			} else if (s->qn < QMAX) {
				p.deliver_us = at;
				s->q[s->qn++] = p;
				if (s->reorder_every && s->sent % s->reorder_every == 0 && s->qn >= 2) {
					/* this one overtakes the one before it */
					s->swaps++;
					u64 d = s->q[s->qn - 2].deliver_us;

					s->q[s->qn - 2].deliver_us = s->q[s->qn - 1].deliver_us;
					s->q[s->qn - 1].deliver_us = d;
					struct pkt tmp = s->q[s->qn - 2];
					s->q[s->qn - 2] = s->q[s->qn - 1];
					s->q[s->qn - 1] = tmp;
				}
			}
		}
		(void)t_send;
		/* the net thread: everything delivered so far, in arrival order */
		if (t >= t_net) {
			u32 k = 0;

			t_net += s->net_us;
			for (u32 i = 0; i < s->qn; i++) {
				if (s->q[i].deliver_us <= t)
					xb_jb_put(j, s->q[i].seq, s->q[i].ts, s->in_rate, s->in_ch,
						  s->q[i].pcm, s->q[i].n, (u32)(t / 1000));
				else
					s->q[k++] = s->q[i];
			}
			s->qn = k;
		}
		/* the driver thread */
		if (t >= t_read) {
			u32 under = j->underruns, over = j->overruns;
			bool any;

			t_read += s->block_us;
			any = xb_jb_read(j, blk, s->block, s->out_rate, s->target_ms,
					 (u32)(t / 1000) - s->reader_lag_ms);
			memset(mix, 0, 4 * s->block);
			if (any)
				xb_mix_into(mix, s->block, s->out_ch, blk, j->ch, 256, 256);
			for (u32 f = 0; f < s->block; f++) {
				s32 v = (s16)get_le16(mix + 2 * s->out_ch * f);

				if (started) {
					u32 d = xb_abs(v - prev);

					if (d > s->max_delta_after_start)
						s->max_delta_after_start = d;
				}
				prev = v;
				s->out[s->out_n++] = v;
			}
			if (j->state == XB_RUNNING)
				started = 1;
			if (t > 3000000 && (!s->stall_ms || t < (u64)s->stall_at_ms * 1000) &&
			    (!s->stop_at_ms || t < (u64)s->stop_at_ms * 1000)) {
				s->settled_underruns += j->underruns - under;
				s->settled_overruns += j->overruns - over;
				if (j->state == XB_RUNNING && j->fill_ms) {
					if ((s32)j->fill_ms < s->fill_min)
						s->fill_min = (s32)j->fill_ms;
					if ((s32)j->fill_ms > s->fill_max)
						s->fill_max = (s32)j->fill_ms;
				}
				s->ppm_last = j->ppm;
			}
		}
	}
	free(blk);
	free(mix);
}

/* the largest step between neighbouring samples of a clean sine at this rate and amplitude */
static u32 sine_step(double amp, u32 rate)
{
	return (u32)ceil(amp * 2 * sin(M_PI * 440.0 / rate)) + 2;
}

/* RMS of the last `secs` seconds of the output (left), against the sine's */
static double rms_tail(const struct sim *s, double secs)
{
	u32 n = (u32)(secs * s->out_rate), from = s->out_n > n ? s->out_n - n : 0;
	double sum = 0;

	for (u32 i = from; i < s->out_n; i++)
		sum += (double)s->out[i] * s->out[i];
	return sqrt(sum / (s->out_n - from));
}

static struct sim *new_sim(void)
{
	struct sim *s = calloc(1, sizeof(*s));

	s->in_rate = 48000;
	s->in_ch = 2;
	s->pkt_frames = 240;   /* 5 ms */
	s->out_rate = 24000;
	s->out_ch = 2;
	s->block = 192;        /* the earpiece's 8 ms block */
	s->block_us = 8000;
	s->net_us = 16683;     /* one pass per 60 Hz frame */
	s->target_ms = 50;
	s->secs = 60;
	s->amp = 8000;
	return s;
}

static struct xb_jb *new_jb(u32 cap, u32 ch)
{
	struct xb_jb *j = malloc(sizeof(*j));

	xb_jb_init(j, calloc(cap * ch, sizeof(s16)), cap, ch);
	return j;
}

static void free_all(struct sim *s, struct xb_jb *j)
{
	free(s->out);
	free(j->ring);
	free(j);
	free(s);
}

static void report(const char *name, const struct sim *s, const struct xb_jb *j)
{
	printf("%-34s sent %5u lost %3u (dropped %3u) late %2u recovered %2u restarts %u underruns %u "
	       "(settled %u) overruns %u fill %d..%d ms trim %+d ppm step %u (sine %u)\n",
	       name, s->sent, j->lost, s->dropped, j->late, j->recovered, j->restarts, j->underruns,
	       s->settled_underruns, j->overruns, s->fill_min, s->fill_max, s->ppm_last,
	       s->max_delta_after_start, sine_step(s->amp / 2 * 2, s->out_rate));
}

/* a clean run at a clock offset: no underruns or overruns once settled, the fill near the
 * target, the trim tracking the offset, the tone at full level, no step above a sine's */
static void steady(const char *name, s32 ppm, u32 in_rate, u32 in_ch, u32 pkt, u32 out_rate,
		   u32 out_ch, u32 block, u32 block_us, u32 cap, u32 jb_ch)
{
	struct sim *s = new_sim();
	struct xb_jb *j = new_jb(cap, jb_ch);
	double rms;

	/* the console's driver thread reads a clock it took at its last pass (up to a frame old) */
	s->reader_lag_ms = 17;
	s->pc_ppm = ppm;
	s->in_rate = in_rate;
	s->in_ch = in_ch;
	s->pkt_frames = pkt;
	s->out_rate = out_rate;
	s->out_ch = out_ch;
	s->block = block;
	s->block_us = block_us;
	run(s, j);
	report(name, s, j);
	rms = rms_tail(s, 10);
	CHECK(j->restarts == 1, "%s: restarts %u", name, j->restarts);
	CHECK(j->lost == 0 && j->late == 0, "%s: lost %u late %u", name, j->lost, j->late);
	CHECK(s->settled_underruns == 0 && s->settled_overruns == 0, "%s: underruns %u overruns %u",
	      name, s->settled_underruns, s->settled_overruns);
	CHECK(s->fill_min >= (s32)s->target_ms - 12 && s->fill_max <= (s32)s->target_ms + 12,
	      "%s: fill %d..%d ms", name, s->fill_min, s->fill_max);
	/* the trim follows the PC's clock (the console reads that much faster or slower) */
	CHECK(abs(s->ppm_last - ppm) <= 120, "%s: trim %d ppm for a %d ppm offset", name,
	      s->ppm_last, ppm);
	CHECK(fabs(rms - s->amp / sqrt(2)) < s->amp * 0.03, "%s: rms %.0f, sine %.0f", name, rms,
	      s->amp / sqrt(2));
	CHECK(s->max_delta_after_start <= sine_step(s->amp, s->out_rate) * 11 / 10,
	      "%s: largest step %u (a clean sine's %u)", name, s->max_delta_after_start,
	      sine_step(s->amp, s->out_rate));
	free_all(s, j);
}

static void loss_and_reorder(void)
{
	struct sim *s = new_sim();
	struct xb_jb *j = new_jb(8192, 2);

	s->loss = 0.01;
	run(s, j);
	report("1% loss", s, j);
	CHECK(j->lost == s->dropped, "loss: counted %u, dropped %u", j->lost, s->dropped);
	CHECK(s->dropped > 50, "loss: only %u dropped", s->dropped);
	CHECK(s->settled_underruns == 0, "loss: underruns %u", s->settled_underruns);
	CHECK(j->restarts == 1, "loss: restarts %u", j->restarts);
	/* a lost packet is a dip to silence and back: never a step above a clean sine's */
	CHECK(s->max_delta_after_start <= sine_step(s->amp, s->out_rate) * 11 / 10,
	      "loss: largest step %u (a sine's %u)", s->max_delta_after_start,
	      sine_step(s->amp, s->out_rate));
	free_all(s, j);

	s = new_sim();
	j = new_jb(8192, 2);
	s->reorder_every = 37;
	run(s, j);
	report("reordered every 37th", s, j);
	CHECK(j->lost == 0, "reorder: %u counted lost", j->lost);
	CHECK(j->recovered + j->late == s->swaps, "reorder: recovered %u late %u of %u",
	      j->recovered, j->late, s->swaps);
	CHECK(j->recovered > 0, "reorder: none recovered");
	CHECK(s->settled_underruns == 0, "reorder: underruns %u", s->settled_underruns);
	CHECK(s->max_delta_after_start <= sine_step(s->amp, s->out_rate) * 11 / 10,
	      "reorder: largest step %u", s->max_delta_after_start);
	free_all(s, j);
}

/* a network stall longer than the target: the earpiece runs dry while the PC keeps sending.
 * It must fade to silence, stay silent, and fade back in, with no step above a sine's. */
static void stall(void)
{
	struct sim *s = new_sim();
	struct xb_jb *j = new_jb(8192, 2);
	u32 from, to, zeros = 0;

	s->stall_at_ms = 20000;
	s->stall_ms = 90;
	s->secs = 25;
	run(s, j);
	report("stall 90 ms (underrun)", s, j);
	CHECK(j->underruns == 1, "stall: underruns %u (want 1)", j->underruns);
	CHECK(j->restarts == 1, "stall: restarts %u (the stream goes on)", j->restarts);
	CHECK(s->max_delta_after_start <= sine_step(s->amp, s->out_rate) * 11 / 10,
	      "stall: largest step %u (a sine's %u): a click", s->max_delta_after_start,
	      sine_step(s->amp, s->out_rate));
	/* silence in the hole */
	from = 20000 * s->out_rate / 1000;
	to = (20000 + 200) * s->out_rate / 1000;
	for (u32 i = from; i < to && i < s->out_n; i++)
		zeros += s->out[i] == 0;
	CHECK(zeros > s->out_rate / 50, "stall: only %u silent samples in the hole", zeros);
	CHECK(fabs(rms_tail(s, 2) - s->amp / sqrt(2)) < s->amp * 0.03, "stall: no full level after");
	free_all(s, j);
}

/* the PC stops: the earpiece plays out and fades; no underrun is counted; then it starts again */
static void stop(void)
{
	struct sim *s = new_sim();
	struct xb_jb *j = new_jb(8192, 2);
	u32 tail_nonzero = 0;

	s->stop_at_ms = 10000;
	s->secs = 12;
	run(s, j);
	report("PC stops", s, j);
	CHECK(j->underruns == 0, "stop: underruns %u", j->underruns);
	CHECK(j->state == XB_IDLE, "stop: state %u, not idle", j->state);
	for (u32 i = s->out_n - s->out_rate; i < s->out_n; i++)
		tail_nonzero += s->out[i] != 0;
	CHECK(tail_nonzero == 0, "stop: %u samples after the end", tail_nonzero);
	CHECK(s->max_delta_after_start <= sine_step(s->amp, s->out_rate) * 11 / 10,
	      "stop: largest step %u", s->max_delta_after_start);
	/* the end (bye) */
	xb_jb_end(j);
	free_all(s, j);
}

/* the send ring: in order, the position as the timestamp, a reader far behind skips ahead */
static void txr(void)
{
	struct xb_txr t;
	s16 *buf = calloc(1024, sizeof(s16));
	u8 le[4 * 100], be[2 * 600];
	u32 ts = 0, n, total = 0, bad = 0;

	xb_txr_init(&t, buf, 1024);
	for (u32 round = 0; round < 50; round++) {
		for (u32 f = 0; f < 100; f++) {
			put_le16(le + 4 * f, (u16)(round * 100 + f));
			put_le16(le + 4 * f + 2, 0xBEEF); /* the right channel: not sent */
		}
		xb_txr_push(&t, le, 100, 4, 24000);
		while ((n = xb_txr_take(&t, be, 300, &ts)) != 0) {
			for (u32 i = 0; i < n; i++)
				bad += (u32)xb_get_be16(be + 2 * i) != (u32)(s16)(ts + i);
			total += n;
		}
	}
	CHECK(total == 5000 && bad == 0 && t.skipped == 0, "txr: %u taken, %u wrong", total, bad);
	/* fall behind by 3000 (the ring holds 1024): skips to the newest quarter */
	for (u32 round = 0; round < 30; round++)
		xb_txr_push(&t, le, 100, 4, 24000);
	n = xb_txr_take(&t, be, 600, &ts);
	CHECK(t.skipped == 3000 - 256 && ts == 5000 + 3000 - 256 && n == 256,
	      "txr: skipped %u, ts %u, n %u", t.skipped, ts, n);
	CHECK(t.rate == 24000, "txr: rate %u", t.rate);
	xb_txr_reset(&t);
	CHECK(xb_txr_take(&t, be, 600, &ts) == 0, "txr: reset left something");
	free(buf);
}

/* the mix: gains, channel mapping, clamping */
static void mix(void)
{
	u8 out[4 * 4];
	s32 src[8] = { 30000, -30000, 1000, 2000, -32768, 32767, 100, 100 };

	for (int i = 0; i < 4; i++) {
		put_le16(out + 4 * i, (u16)20000);
		put_le16(out + 4 * i + 2, (u16)(s16)-20000);
	}
	xb_mix_into(out, 4, 2, src, 2, 256, 256);
	CHECK((s16)get_le16(out) == 32767 && (s16)get_le16(out + 2) == -32768,
	      "mix: no clamp: %d %d", (s16)get_le16(out), (s16)get_le16(out + 2));
	CHECK((s16)get_le16(out + 4) == 21000 && (s16)get_le16(out + 6) == -18000, "mix: sum");
	/* mono out from stereo: the average; half gain */
	put_le16(out, 0);
	xb_mix_into(out, 1, 1, (s32[]){ 1000, 3000 }, 2, 128, 128);
	CHECK((s16)get_le16(out) == 1000, "mix: mono %d", (s16)get_le16(out));
	/* stereo out from mono: both */
	put_le16(out, 0);
	put_le16(out + 2, 0);
	xb_mix_into(out, 1, 2, (s32[]){ -4000 }, 1, 256, 256);
	CHECK((s16)get_le16(out) == -4000 && (s16)get_le16(out + 2) == -4000, "mix: dup");
}

int main(void)
{
	/* stream 3: 48 kHz stereo from the PC into a 24 kHz stereo earpiece (8 ms blocks) */
	steady("headset 48k->24k, clocks equal", 0, 48000, 2, 240, 24000, 2, 192, 8000, 8192, 2);
	steady("headset 48k->24k, PC +200 ppm", 200, 48000, 2, 240, 24000, 2, 192, 8000, 8192, 2);
	steady("headset 48k->24k, PC -200 ppm", -200, 48000, 2, 240, 24000, 2, 192, 8000, 8192, 2);
	/* stream 3 at the earpiece's own rate (what the helper sends when asked) */
	steady("headset 24k->24k, PC +200 ppm", 200, 24000, 2, 240, 24000, 2, 192, 8000, 8192, 2);
	steady("headset 48k->48k, PC -200 ppm", -200, 48000, 2, 240, 48000, 2, 384, 8000, 8192, 2);
	/* stream 4: 24 kHz mono into the mic path, read 24 frames (1 ms) at a time */
	steady("chat 24k mono, 1 ms reads, +200", 200, 24000, 1, 240, 24000, 1, 24, 1000, 8192, 1);
	steady("chat 24k mono, 1 ms reads, -200", -200, 24000, 1, 240, 24000, 1, 24, 1000, 8192, 1);
	steady("chat 48k stereo into mono", 0, 48000, 2, 240, 24000, 1, 24, 1000, 8192, 1);
	loss_and_reorder();
	stall();
	stop();
	txr();
	mix();
	printf("%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
	return g_fail ? 1 : 0;
}
