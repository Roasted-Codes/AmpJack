// SPDX-License-Identifier: GPL-3.0-or-later
/* Host simulation of team voice's receive path (xbox360/team_360.c, WP4.1): packets from a
 * simulated peer go through handle_packet into a talker's jitter buffer, and xt_mix pulls
 * earpiece blocks at the headset's pace, on a simulated clock. Each scenario checks what the
 * console acceptance (accept/p4_rx.py) checks: no underruns after the first second, fill and
 * trim in range, the tone at the expected level, and the loss / reorder / talk-spurt counters.
 *
 *   cc -O2 -Wall -o test_team tests/test_team.c -lm && ./test_team
 *
 * The packets are built in host byte order (team_360.c reads the header natively; on the
 * console that is the wire's big-endian order).
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
int xa_random(void *b, int n) { memset(b, 0x5a, (size_t)n); return 0; }
static int g_quiet = 1;
void xa_log(const char *fmt, ...)
{
	va_list ap;

	if (g_quiet)
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
int xn_init(void) { return 0; }
u32 xn_init_detail(void) { return 0; }
u32 xn_startup(u32 m, u32 *a, u32 *b) { return 0; }
void xn_cleanup(void) {}
u32 xn_open_udp(u16 p, u32 o, u32 r, u32 *s, u32 *st) { *s = 1; return 0; }
void xn_close(u32 s) {}
u32 xn_send_to(u32 s, u32 ip, u16 p, const void *b, int l) { return 0; }
u32 xn_recv(u32 s, void *b, int c, int *l, u32 *ip, u16 *p) { return 10035; }
u32 xn_xnaddr(u8 m[6], u32 *ip) { memset(m, 0, 6); *ip = 0; return 1; }
u32 xn_link(void) { return 1; }

#include "../xbox360/team_360.c"

/* ---- the simulation --------------------------------------------------------------------- */
#define RATE_OUT 24000
#define BLOCK 192 /* 8 ms at 24 kHz */
#define MAXQ 4096

struct pkt { u64 at; u32 seq, ts; s16 pcm[160]; };
static struct pkt g_q[MAXQ];
static int g_qn;

static double frand(void) { return rand() / (double)RAND_MAX; }

static void queue(u64 at, u32 seq, u32 ts, double *phase, double hz, double amp)
{
	struct pkt *p;

	if (g_qn >= MAXQ)
		return;
	p = &g_q[g_qn++];
	p->at = at;
	p->seq = seq;
	p->ts = ts;
	for (int i = 0; i < 160; i++) {
		p->pcm[i] = (s16)lrint(amp * sin(*phase));
		*phase += 2 * M_PI * hz / 16000;
	}
}

static void deliver(const struct pkt *p, u32 sender)
{
	u8 b[20 + 320];
	struct xt_hdr h = { XT_MAGIC, XT_VERSION, XT_AUDIO, 1, XT_F_TALKING, sender,
			    (u16)p->seq, 160, p->ts };

	memcpy(b, &h, 20);
	memcpy(b + 20, p->pcm, 320);
	handle_packet(b, sizeof(b), 0x0A000002, (u32)xa_now_ms(), 4);
}

struct scenario {
	const char *name;
	double secs, sender_ppm, consumer_ppm, jitter_ms, loss, reorder;
	double talk_s, pause_s;  /* 0: continuous */
	int expect_spurts;
	double mix_from_s;       /* no earpiece (no headset streaming) before this */
};

static double goertzel(const s16 *x, int n, double rate, double hz)
{
	double k = 2 * cos(2 * M_PI * hz / rate), s1 = 0, s2 = 0;

	for (int i = 0; i < n; i++) {
		double s0 = x[i] + k * s1 - s2;

		s2 = s1;
		s1 = s0;
	}
	return 2 * sqrt(fmax(s1 * s1 + s2 * s2 - k * s1 * s2, 0)) / n / 32768;
}

static int run(const struct scenario *sc)
{
	static s16 out[BLOCK * 2];
	static s16 tail[RATE_OUT]; /* the last second of the left channel */
	u64 end = (u64)(sc->secs * 1e6), next_send = 0, next_mix = 3000, next_age = 0;
	double send_period = 10000.0 / (1 + sc->sender_ppm * 1e-6);
	double mix_period = 8000.0 / (1 + sc->consumer_ppm * 1e-6), phase = 0, tsend = 0,
	       tmix = 3000;
	u32 seq = 0, ts = 123456, under_1s = 0, fill_min = 999, fill_max = 0;
	s32 trim_min = 1 << 30, trim_max = -(1 << 30);
	int tail_n = 0, ok = 1;

	memset(&g_tk, 0, sizeof(g_tk));
	memset(&g_peer, 0, sizeof(g_peer));
	memset(g_xt.talker, 0, sizeof(g_xt.talker));
	g_xt.stage = 3;
	g_xt.applied_stage = 3;
	g_xt.channel = 1;
	g_xt.jb_ms = 0;
	g_sender = 0x11111111;
	g_qn = 0;
	g_now_us = 0;
	srand(1);
	while (g_now_us < end) {
		u64 t = next_send < next_mix ? next_send : next_mix;
		int qi = -1;

		for (int i = 0; i < g_qn; i++)
			if (g_q[i].at <= t && (qi < 0 || g_q[i].at < g_q[qi].at))
				qi = i;
		if (qi >= 0) { /* a packet arrives first */
			g_now_us = g_q[qi].at;
			deliver(&g_q[qi], 0x22222222);
			g_q[qi] = g_q[--g_qn];
			continue;
		}
		g_now_us = t;
		if (t == next_send) {
			double in_spurt = sc->talk_s == 0 ? 1 :
				fmod(t / 1e6, sc->talk_s + sc->pause_s) < sc->talk_s;

			if (in_spurt && frand() >= sc->loss) {
				u64 at = t + (u64)(frand() * sc->jitter_ms * 1000);

				if (sc->reorder && frand() < sc->reorder)
					at += 12000; /* arrives after the next one */
				queue(at, seq, ts, &phase, 440, 3277);
			} else if (in_spurt) {
				for (int i = 0; i < 160; i++)
					phase += 2 * M_PI * 440 / 16000;
			}
			if (in_spurt) {
				seq++;
				ts += 160;
			} else {
				ts += 160; /* the sender's clock runs on while it is silent */
			}
			tsend += send_period;
			next_send = (u64)tsend;
		} else {
			memset(out, 0, sizeof(out));
			if (t >= (u64)(sc->mix_from_s * 1e6))
				xt_mix(0, (u8 *)out, sizeof(out), RATE_OUT, 2, 256);
			for (int i = 0; i < BLOCK; i++)
				tail[(tail_n + i) % RATE_OUT] = out[2 * i];
			tail_n += BLOCK;
			if (t > 1000000 && sc->talk_s == 0) {
				struct xt_talker_status *s = &g_xt.talker[0];

				if (t < 1100000 + (u64)(sc->mix_from_s * 1e6))
					under_1s = s->underruns;
				if (t > 5000000) {
					if (s->fill_ms < fill_min)
						fill_min = s->fill_ms;
					if (s->fill_ms > fill_max)
						fill_max = s->fill_ms;
					if (t > 30000000) {
						if ((s32)s->trim_ppm < trim_min)
							trim_min = (s32)s->trim_ppm;
						if ((s32)s->trim_ppm > trim_max)
							trim_max = (s32)s->trim_ppm;
					}
				}
			}
			tmix += mix_period;
			next_mix = (u64)tmix;
		}
		if (g_now_us >= next_age) {
			age_peers((u32)xa_now_ms());
			publish((u32)xa_now_ms());
			next_age = g_now_us + 100000;
		}
	}
	{
		struct xt_talker_status *s = &g_xt.talker[0];
		double amp = goertzel(tail, RATE_OUT, RATE_OUT, 440);

		printf("%-28s underruns %u (at 1 s %u) overruns %u spurts %u starts %u restarts %u "
		       "concealed %u recovered %u late %u; fill %u..%u ms (target %u), trim %d..%d "
		       "ppm, jitter %.1f ms; 440 Hz in the last second %.4f\n",
		       sc->name, s->underruns, under_1s, s->overruns, s->spurts, s->starts,
		       s->restarts, s->concealed, s->recovered, s->late, fill_min, fill_max,
		       s->target_ms, trim_min, trim_max, s->jitter_us / 1000.0, amp);
		if (sc->talk_s == 0) {
			ok &= s->underruns == under_1s;
			ok &= fill_min >= 20 && fill_max <= 50;
			ok &= trim_min >= -500 && trim_max <= 500;
			if (sc->loss == 0)
				ok &= amp > 0.09 && amp < 0.11; /* -20 dBFS sent, gain 1 */
		} else {
			ok &= s->underruns == 0;
			ok &= (int)s->spurts >= sc->expect_spurts - 1;
		}
		if (sc->loss > 0)
			ok &= s->concealed > 0;
		if (sc->reorder > 0)
			ok &= s->recovered + s->late > 0;
	}
	printf("%s %s\n", ok ? "ok  " : "FAIL", sc->name);
	return ok;
}

int main(void)
{
	static const struct scenario S[] = {
		{ "steady, same clocks", 300, 0, 0, 1, 0, 0, 0, 0, 0 },
		{ "steady, sender +200 ppm", 300, 200, 0, 3, 0, 0, 0, 0, 0 },
		{ "steady, sender -300 ppm", 300, -300, 0, 3, 0, 0, 0, 0, 0 },
		{ "jitter 10 ms", 120, 50, -50, 10, 0, 0, 0, 0, 0 },
		{ "1 % loss", 120, 0, 0, 2, 0.01, 0, 0, 0, 0 },
		{ "2 % reordered", 120, 0, 0, 2, 0, 0.02, 0, 0, 0 },
		{ "talk spurts 2 s on 1 s off", 60, 0, 0, 2, 0, 0, 2, 1, 20 },
		{ "no headset for 3 s", 60, 100, 0, 2, 0, 0, 0, 0, 0, 3 },
	};
	int ok = 1;

	for (unsigned i = 0; i < sizeof(S) / sizeof(S[0]); i++)
		ok &= run(&S[i]);
	return ok ? 0 : 1;
}
