// SPDX-License-Identifier: GPL-3.0-or-later
/* Host simulation of team voice's send path (xbox360/team_360.c, WP4.2): mic packets of 1 ms
 * at 24 kHz go into xt_mic as voice_360.c gives them, the net thread's tx_drain runs every
 * ~16 ms (the console's sleep granularity), and every datagram sent is checked as the Mac
 * peer (accept/p4_tx.py) checks it: 160-sample packets, contiguous timestamps within a talk
 * spurt, the level, the fades, the end of a spurt after the gate closes, nothing while muted,
 * on MIC TO GAME or below stage 4, and MIC TO TEAM's silence for the 360.
 *
 *   cc -O2 -Wall -o test_team_tx tests/test_team_tx.c -lm && ./test_team_tx
 *
 * Packets are in host byte order here (team_360.c writes the header natively; on the console
 * that is the wire's big-endian order).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XT_HOST_TEST 1
#include "../core/platform.h"

typedef void *HANDLE;
static u64 g_now_us;
static u8 g_kthread[0x100];

u64 xt_host_tb(void) { return g_now_us * 50; }
u8 *xt_host_kthread(void) { return g_kthread; }
long ExCreateThread(HANDLE *h, u32 a, u32 *b, void *c, void *d, void *e, u32 f) { *h = 0; return 0; }
long KeSetBasePriorityThread(void *t, u32 i) { return 0; }
long XexGetModuleHandle(const char *n, HANDLE *h) { *h = 0; return -1; }
long XexGetProcedureAddress(HANDLE m, u32 o, void **p) { *p = 0; return -1; }
u64 xa_now_ms(void) { return g_now_us / 1000; }
void xa_sleep_ms(unsigned int ms) { g_now_us += ms * 1000; }
int xa_random(void *b, int n) { memset(b, 0xff, (size_t)n); return 0; } /* the 16 kHz clock wraps at once */
void xa_log(const char *fmt, ...) { (void)fmt; }
int xn_init(void) { return 0; }
u32 xn_init_detail(void) { return 0; }
u32 xn_startup(u32 m, u32 *a, u32 *b) { return 0; }
void xn_cleanup(void) {}
u32 xn_open_udp(u16 p, u32 o, u32 r, u32 *s, u32 *st) { *s = 1; return 0; }
void xn_close(u32 s) {}
u32 xn_recv(u32 s, void *b, int c, int *l, u32 *ip, u16 *p) { return 10035; }
u32 xn_xnaddr(u8 m[6], u32 *ip) { memset(m, 0, 6); m[5] = 7; *ip = 0; return 4; }
u32 xn_link(void) { return 1; }

/* every AUDIO datagram sent, with its send time */
#define MAXP 8192
struct sent { u64 at_us; struct xt_hdr_host { u32 magic; u8 version, type, channel, flags;
	u32 sender; u16 seq, samples; u32 ts; } h; int len; s16 pcm[160]; };
static struct sent g_s[MAXP];
static int g_n, g_other;

u32 xn_send_to(u32 s, u32 ip, u16 p, const void *b, int l)
{
	struct sent *x;

	if (l < 20)
		return 0;
	if (((const u8 *)b)[5] != 2) { /* HELLO / BYE */
		g_other++;
		return 0;
	}
	if (g_n >= MAXP)
		return 0;
	x = &g_s[g_n++];
	x->at_us = g_now_us;
	memcpy(&x->h, b, 20);
	x->len = l;
	memcpy(x->pcm, (const u8 *)b + 20, l - 20 > 320 ? 320 : (size_t)(l - 20));
	return 0;
}

#include "../xbox360/team_360.c"

/* ---- the simulation --------------------------------------------------------------------- */
#define RATE_IN 24000
#define PASS_US 16667 /* the driver loop and the net thread wake this often (sleep granularity) */

static int g_fail;
static double g_ph;

static void check(int ok, const char *name, const char *fmt, ...)
{
	va_list ap;

	printf("%s %-34s ", ok ? "ok  " : "FAIL", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	if (!ok)
		g_fail = 1;
}

static void reset(void)
{
	memset(&g_mt, 0, sizeof(g_mt));
	memset(&g_xt.tx, 0, sizeof(g_xt.tx));
	g_xt.tx.wake = 1;
	g_txq_w = g_txq_r = 0;
	g_mic_pad = -1;
	g_n = g_other = 0;
	g_xt.stage = 4;
	g_xt.channel = 1;
	g_xt.mic_route = 0;
	g_xt.test_tone = 0;
	xt_host_muted = false;
	xt_host_mic_gain = 100;
	g_quiesced = 0;
	g_ph = 0;
	xt_poll();             /* applied_stage */
	g_xt.net_state = NS_OPEN;
	g_sender = 0x12345678;
}

/* runs `ms` of mic: amp(t) gives the tone's amplitude (0..1) and open(t) the gate, per 1 ms
 * mic packet; the driver delivers them in bursts of a pass, the net thread drains after */
typedef double (*amp_fn)(double t);
typedef int (*open_fn)(double t);

static void run(double from_s, int ms, amp_fn amp, open_fn open, int drain)
{
	u8 pkt[2 * 24];
	u64 next_pass = g_now_us + PASS_US;
	int pending = 0;
	static u8 burst[64][2 * 24];
	static int burst_open[64];

	for (int m = 0; m < ms; m++) {
		double t = from_s + m / 1000.0;
		double a = amp ? amp(t) : 0;

		for (int i = 0; i < 24; i++) {
			s16 v = (s16)lrint(a * 32767 * sin(g_ph));

			g_ph += 2 * M_PI * 440 / RATE_IN;
			pkt[2 * i] = (u8)v;
			pkt[2 * i + 1] = (u8)(v >> 8);
		}
		memcpy(burst[pending], pkt, sizeof(pkt));
		burst_open[pending++] = open ? open(t) : 0;
		g_now_us += 1000;
		if (g_now_us >= next_pass || pending == 64) {
			next_pass += PASS_US;
			for (int b = 0; b < pending; b++)
				xt_mic(0, burst[b], sizeof(pkt), RATE_IN, 1, burst_open[b]);
			pending = 0;
			if (drain)
				tx_drain(g_xt.applied_stage);
		}
	}
	for (int b = 0; b < pending; b++)
		xt_mic(0, burst[b], sizeof(pkt), RATE_IN, 1, burst_open[b]);
	if (drain)
		tx_drain(g_xt.applied_stage);
}

struct spurt { int first, last; u32 samples; };

/* the received packets split into talk spurts by timestamp; checks the packets themselves */
static int spurts(struct spurt *sp, int max, int *bad_len, int *bad_seq, int *gaps)
{
	int n = 0;

	*bad_len = *bad_seq = *gaps = 0;
	for (int i = 0; i < g_n; i++) {
		struct sent *s = &g_s[i];

		if (s->h.samples != 160 || s->len != 340 || s->h.magic != XT_MAGIC || s->h.channel != 1 ||
		    !(s->h.flags & XT_F_TALKING) || s->h.sender != 0x12345678)
			(*bad_len)++;
		if (i && (u16)(s->h.seq - g_s[i - 1].h.seq) != 1)
			(*bad_seq)++;
		if (i && s->h.ts - g_s[i - 1].h.ts == 160) {
			sp[n - 1].last = i;
			sp[n - 1].samples += 160;
			continue;
		}
		if (i && (s32)(s->h.ts - g_s[i - 1].h.ts) < 160 * 5)
			(*gaps)++; /* a hole inside a spurt (lost or misplaced packets) */
		if (n < max) {
			sp[n].first = sp[n].last = i;
			sp[n].samples = 160;
			n++;
		}
	}
	return n;
}

static double goertzel(int from, int to)
{
	double k = 2 * cos(2 * M_PI * 440 / 16000), s1 = 0, s2 = 0, p;
	int n = 0;

	for (int i = from; i <= to; i++)
		for (int j = 0; j < 160; j++, n++) {
			double s0 = g_s[i].pcm[j] + k * s1 - s2;

			s2 = s1;
			s1 = s0;
		}
	p = s1 * s1 + s2 * s2 - k * s1 * s2;
	return n ? 2 * sqrt(p > 0 ? p : 0) / n / 32768 : 0;
}

static double tone20(double t) { return 0.1; }
static double quiet(double t) { return 0.0; }
static int open_1s(double t) { return t < 1.0; }
static int always(double t) { return 1; }

int main(void)
{
	struct spurt sp[64];
	int n, bl, bs, gaps;

	/* 1. the synthetic mic (test_tone 2): 10 s = 5 spurts of 1 s */
	reset();
	g_xt.test_tone = 2;
	run(0, 10000, quiet, 0, 1);
	n = spurts(sp, 64, &bl, &bs, &gaps);
	{
		int len_ok = 1, edge_ok = 1, gap_ok = 1;

		for (int i = 0; i < n; i++) {
			u32 len = sp[i].samples;
			s16 *a = g_s[sp[i].first].pcm, *z = g_s[sp[i].last].pcm;

			if (len < 16000 || len > 16000 + 2 * 160)
				len_ok = 0;
			if (abs(a[0]) > 400 || abs(z[159]) > 400)
				edge_ok = 0;
			if (i && (g_s[sp[i].first].h.ts - g_s[sp[i - 1].last].h.ts - 160 < 15500 ||
				  g_s[sp[i].first].h.ts - g_s[sp[i - 1].last].h.ts - 160 > 16500))
				gap_ok = 0;
		}
		check(n == 5 && !bl && !bs && !gaps, "synthetic: 5 spurts, packets well formed",
		      "%d spurts, %d packets, bad %d, seq breaks %d, holes %d", n, g_n, bl, bs, gaps);
		check(len_ok, "synthetic: each spurt 1 s (+ <= 20 ms)", "first %u samples", sp[0].samples);
		check(gap_ok, "synthetic: 1 s between spurts", "");
		check(edge_ok, "synthetic: spurts start and end faded", "first %d, last %d",
		      g_s[sp[0].first].pcm[0], g_s[sp[0].last].pcm[159]);
		{
			double a = goertzel(sp[1].first + 2, sp[1].last - 2);

			check(fabs(20 * log10(a / 0.1)) < 0.5, "synthetic: 440 Hz at -20 dBFS",
			      "%.4f (%.2f dB)", a, 20 * log10(a / 0.1));
		}
		check(g_xt.tx.close_ms_max <= 20 && g_xt.tx.spurts == 5 && !g_xt.tx.dropped,
		      "synthetic: counters", "spurts %u queued %u sent %u dropped %u close_ms max %u "
		      "depth max %u", g_xt.tx.spurts, g_xt.tx.queued, g_xt.tx.sent, g_xt.tx.dropped,
		      g_xt.tx.close_ms_max, g_xt.tx.depth_max);
	}

	/* 2. the mic: speech-level tone, gate open 1 s then closed; mic level 50 % */
	reset();
	xt_host_mic_gain = 50;
	run(0, 3000, tone20, open_1s, 1);
	n = spurts(sp, 64, &bl, &bs, &gaps);
	{
		double a = n ? goertzel(sp[0].first + 2, sp[0].last - 2) : 0;
		u32 end = n ? g_s[sp[0].last].h.ts + 160 - g_s[0].h.ts : 0;

		check(n == 1 && !bl && !bs && !gaps, "mic: one spurt", "%d spurts, %d packets", n, g_n);
		check(fabs(20 * log10(a / 0.05)) < 0.5, "mic: the level x 50 %", "%.4f (%.2f dB vs "
		      "-26 dBFS)", a, 20 * log10(a / 0.05));
		check(end >= 16000 && end <= 16000 + 2 * 160 && g_xt.tx.close_ms <= 20,
		      "mic: stops <= 20 ms after the gate", "%.1f ms of audio, close_ms %u", end / 16.0,
		      g_xt.tx.close_ms);
		check(g_xt.tx.why == TX_SENDING && !g_xt.tx.talking, "mic: idle after", "why %u",
		      g_xt.tx.why);
	}

	/* 3. muted: the gate (xd_gate_open) is closed by voice_360.c, and MUTED goes in HELLO */
	reset();
	xt_host_muted = true;
	run(0, 2000, tone20, 0, 1);
	check(g_n == 0 && (g_tx_flags & XT_F_MUTED), "muted: nothing sent", "%d packets, flags %x",
	      g_n, g_tx_flags);
	reset();
	g_xt.test_tone = 2;
	xt_host_muted = true;
	run(0, 3000, quiet, 0, 1);
	check(g_n == 0, "muted: synthetic mic sends nothing", "%d packets", g_n);

	/* 4. MIC TO */
	reset();
	g_xt.mic_route = 1;
	run(0, 2000, tone20, always, 1);
	check(g_n == 0 && g_xt.tx.why == TX_OFF_ROUTE && xt_mic_to_game(), "GAME: nothing sent, "
	      "the 360 keeps the mic", "%d packets, why %u", g_n, g_xt.tx.why);
	reset();
	g_xt.mic_route = 2;
	run(0, 2000, tone20, always, 1);
	check(g_n >= 195 && xt_mic_to_game(), "BOTH: sent, the 360 keeps the mic", "%d packets", g_n);
	reset();
	run(0, 2000, tone20, always, 1);
	check(g_n >= 195 && !xt_mic_to_game(), "TEAM: sent, the 360 gets silence", "%d packets", g_n);
	g_xt.net_state = NS_FAILED;
	check(xt_mic_to_game(), "TEAM, socket down: the 360 keeps the mic", "");
	g_xt.net_state = NS_OPEN;
	g_xt.channel = 0;
	check(xt_mic_to_game(), "TEAM, no channel: the 360 keeps the mic", "");

	/* 5. below stage 4, or test tone 1: nothing from the mic */
	reset();
	g_xt.stage = 3;
	xt_poll();
	run(0, 2000, tone20, always, 1);
	check(g_n == 0 && g_xt.tx.why == TX_OFF_STAGE, "stage 3: nothing sent", "%d packets", g_n);
	reset();
	g_xt.test_tone = 1;
	run(0, 1000, tone20, always, 1);
	check(g_n == 0 && g_xt.tx.why == TX_OFF_TONE, "test tone 1: no mic packets", "%d", g_n);

	/* 6. the net thread held up 500 ms: the ring overflows, then recovers */
	reset();
	run(0, 500, tone20, always, 0);
	check(g_xt.tx.dropped > 0 && g_xt.tx.depth_max == TXQ, "held-up net thread: drops counted",
	      "dropped %u, depth max %u", g_xt.tx.dropped, g_xt.tx.depth_max);
	run(0.5, 1000, tone20, always, 1);
	check(g_xt.tx.sent == g_xt.tx.queued, "held-up net thread: recovers", "queued %u sent %u",
	      g_xt.tx.queued, g_xt.tx.sent);

	/* 7. a stage drop discards what is queued (not sent later, stale) */
	reset();
	run(0, 300, tone20, always, 0);
	g_xt.stage = 3;
	xt_poll();
	tx_drain(g_xt.applied_stage);
	check(g_n == 0 && g_xt.tx.discarded == g_xt.tx.queued, "stage drop: queue discarded",
	      "queued %u discarded %u", g_xt.tx.queued, g_xt.tx.discarded);

	printf(g_fail ? "FAILED\n" : "all ok\n");
	return g_fail;
}
