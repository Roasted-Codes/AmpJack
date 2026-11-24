// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Console-to-console team voice, the MixAmp daisy chain (team_360.h; docs/PLAN.md Phase 4,
 * protocol in docs/TEAM.md). Consoles on the same LAN and the same channel hear each other;
 * no PC or server, no pairing. Plain 16 kHz PCM over UDP broadcast, port 3660.
 *
 * Built: WP4.0 (the socket, HELLO / BYE, discovery, counters), WP4.1 (the receive path:
 * talkers, jitter buffers, drift-steered resampling, xt_mix) and WP4.2 (the send path: xt_mic
 * resamples the gated mic to 16 kHz on the driver thread and cuts 10 ms packets into a ring the
 * net thread sends; xt_mic_to_game gives the 360 silence on MIC TO TEAM).
 *
 * Threads:
 *   - the driver thread calls everything in team_360.h. It never calls NetDll: it starts the
 *     net thread, reads what it publishes, and mixes the talkers' rings into the earpiece;
 *   - the net thread (XtNetThread, a system thread on its own hardware thread, below the
 *     driver thread's priority) owns the socket (net_360.c): non-blocking, polled every 2 ms;
 *     it parses packets, keeps the peer table and writes the talkers' rings. Its log lines
 *     go through a small event ring the driver thread prints (xa_log isn't thread-safe).
 *   Each shared word has one writer (noted per field). A talker ring is written by the net
 *   thread only ahead of what the driver thread has published as read, so a race can at worst
 *   cost a glitch, never memory outside a ring (every index is masked).
 *
 * Gate: XWTV.stage, 0 at boot (nothing opened, no thread). 1 = socket only (datagrams counted,
 * not parsed), 2 = HELLO / BYE and discovery, 3 = receive audio into the earpiece, 4 = send
 * the mic (or a test tone: test_tone 1 from the net thread, 2 a synthetic mic through the
 * whole send path). team.py drives it; docs/TEAM.md has the steps.
 */
#include <stdlib.h>

#ifndef XT_HOST_TEST /* tests/test_team.c compiles this file on a PC with stand-ins */
#include <xecore/xboxkrnl.h>
#endif

#include "../core/platform.h"
#include "net_360.h"
#include "team_360.h"
#ifndef XT_HOST_TEST
#include "diag_360.h"  /* the mic level in effect (g_xd), read-only */
#include "voice_360.h" /* xv_mic_muted */
#endif

/* build.sh (WP0.1's) compiles one team file, team_360.c, when it exists; net_360.c is part of
 * this unit until build.sh lists it (then drop this line: XW_TEAM="team_360.c net_360.c") */
#ifndef XT_HOST_TEST
#include "net_360.c"
#endif

#ifndef XT_HOST_TEST
#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#else
#define BARRIER() __sync_synchronize()
#endif
#define BODY __attribute__((used, noinline))

/* ---- the wire format (docs/TEAM.md; team.py mirrors it). Big-endian, PCM s16 big-endian ---- */
#define XT_MAGIC 0x58575456 /* "XWTV" */
#define XT_VERSION 1
#define XT_PORT 3660
enum { XT_HELLO = 1, XT_AUDIO = 2, XT_BYE = 3 };
#define XT_F_TALKING 1
#define XT_F_MUTED 2
#define XT_F_PC 4      /* a PC or phone peer, not a console */
#define XT_F_ALLCALL 8 /* heard on every channel (admin talk-back) */
#define XT_CHANNELS 8
#define XT_RATE 16000
#define XT_PACKET 160          /* samples per AUDIO packet we send (10 ms) */
#define XT_MAX_SAMPLES 480     /* the most we accept in one packet (30 ms, a PC peer) */
#define XT_NAME 16

struct xt_hdr {          /* 20 bytes */
	u32 magic;
	u8 version, type, channel, flags;
	u32 sender;
	u16 seq;
	u16 samples;
	u32 timestamp;
};
struct xt_hello {        /* after the header: 20 bytes */
	char name[XT_NAME];
	u8 level, reserved[3];
};
_Static_assert(sizeof(struct xt_hdr) == 20, "wire header");
_Static_assert(sizeof(struct xt_hello) == 20, "wire hello");

/* ---- timing ------------------------------------------------------------------------------- */
#define HELLO_MS 1000
#define PEER_TIMEOUT_MS 5000
#define POLL_MS 2              /* the net thread's sleep between passes */
#define RX_PER_PASS 64
#define REOPEN_AFTER_FAILS 100 /* consecutive send or receive errors: the socket is reopened */
#define RETRY_MS 5000          /* a failed open is tried again this often */
#define PUBLISH_MS 100
#define QUIESCE_WAIT_MS 1000
#define TB_PER_US 50           /* the timebase: 50 MHz */
#define TB_PER_SAMPLE 3125     /* 50 MHz / 16 kHz */

/* ---- the jitter buffers ------------------------------------------------------------------- */
#define TALKERS 4                /* mixed at once (a 4v4 team has 3 others) */
#define RING 4096                /* samples per talker (256 ms at 16 kHz), a power of two */
#define GUARD 144                /* a late packet may land no nearer the reader than this
                                  * (the reader's published position lags by a block: <= 131) */
#define SPURT_MS 80              /* no packet this long: the talker stopped (not an underrun) */
#define RESTART_GAP 3200         /* a timestamp jump over 200 ms: the stream starts over */
#define TARGET_MIN_MS 30
#define TARGET_MAX_MS 80
#define HIGH_OVER (120 * 16)     /* this far above the target: drop to the target */
#define DRIFT_MAX_PPM 1000
#define FADE 128                 /* output frames of fade-in / fade-out */
#define ONE (1u << 20)           /* resampler phase, Q20 */
#define ACC_MAX 1024             /* output frames per earpiece block we mix (8 ms at 48 kHz = 384) */
#define DUMP_MAX 48000           /* team.py dump: 1 s at up to 48 kHz, mono */

#define TONE_PEAK 3277           /* -20 dBFS */
#define TONE_INC 118111601u      /* 440 Hz at 16 kHz, phase Q32 */

/* ---- the send path (WP4.2) ---------------------------------------------------------------- */
#define TXQ 32                   /* mic packets queued for the net thread (320 ms), power of 2 */
#define TX_RAMP 160              /* samples of the gate / level ramp (10 ms, as voice_360.c) */
#define TX_STEP ((32768 + TX_RAMP - 1) / TX_RAMP)
#define MIC_IDLE_MS 200          /* another pad's mic may take over after this long */
enum { TX_SENDING, TX_OFF_STAGE, TX_OFF_CHANNEL, TX_OFF_ROUTE, TX_OFF_TONE, TX_OFF_SOCKET,
       TX_OFF_QUIESCED, TX_OFF_PAD, TX_OFF_FORMAT };   /* xt_tx_status.why */

/* ---- the xbdm block "XWTV" (team.py FIELDS mirrors it; all u32, append only) ------------- */
#define PEERS 16
struct xt_peer_status {    /* net thread */
	u32 id, ip, chan;      /* chan: channel | flags << 8 | level << 16 */
	u32 name[4];
	u32 age_ms, rx_hello, rx_audio, lost, late, jitter_us, talker; /* talker: slot + 1, 0 none */
};
struct xt_talker_status {
	u32 id, state;                     /* id: net; state: driver (C_*) */
	u32 fill_ms, target_ms, jitter_us; /* fill: driver (average); target, jitter: net */
	u32 underruns, overruns, trim_ppm, starts, spurts; /* driver */
	u32 concealed, recovered, late, restarts;          /* net (concealed: packets) */
	u32 mixed_ms, peak;                                /* driver */
};
struct xt_status {
	u32 magic, version;
	/* controls (xbdm; the panel writes channel, level and mic_route through the setters) */
	volatile u32 stage;     /* 0 off, 1 socket, 2 hello, 3 receive, 4 send */
	volatile u32 channel;   /* 0 off, 1..8 */
	volatile u32 level;     /* TEAM VOL 0..10 */
	volatile u32 mic_route; /* 0 TEAM, 1 GAME, 2 BOTH */
	volatile u32 test_tone; /* stage 4: 1 = 440 Hz at -20 dBFS from the net thread instead of
				 * the mic; 2 = a synthetic mic (440 Hz, -20 dBFS, gated 1 s on /
				 * 1 s off, mute obeyed) through the whole send path */
	volatile u32 dest;      /* where packets go: 0 = 255.255.255.255, else this IPv4 address */
	volatile u32 port;      /* 0 = 3660 */
	volatile u32 opts;      /* XN_OPT_* (net_360.h): insecure 1, broadcast 2, reuseaddr 4 */
	volatile u32 startup;   /* XN_START_* : XNet 1, WSA 2 */
	volatile u32 loop_own;  /* 1: our own packets, if the stack loops them back, are played */
	volatile u32 jb_ms;     /* 0 = adaptive jitter target (30..80 ms), else fixed */
	volatile u32 hw_thread; /* the net thread's hardware thread (read when it is created) */
	volatile u32 priority;  /* the net thread's priority */
	volatile u32 debug;     /* 1: log every new peer and talker event */
	/* state */
	u32 applied_stage, init_error, init_detail, thread;       /* driver */
	u32 net_state, heartbeat, now_ms, sender, ip, mac_hi;     /* net */
	u32 xnaddr_flags, link, socket, opens, reopens;           /* net */
	u32 title_switches, quiesce;                              /* driver */
	/* the last raw error per call and failure counts (net) */
	u32 err_xnet, err_wsa, err_open, open_step, err_send, send_fails, err_recv, recv_fails;
	/* traffic (net) */
	u32 tx_hello, tx_audio, tx_bye, tx_bytes, tx_us_last, tx_us_max, tx_us_sum, tx_calls;
	u32 rx_raw, rx_bytes, rx_hello, rx_audio, rx_bye, rx_bad, rx_own, rx_other_ch;
	u32 rx_no_talker, rx_us_max;
	u32 peers_known, team_peers, talking_mask, tone_skips;    /* net */
	/* the mix (driver) */
	u32 mix_calls, mix_pad, rate_out, ch_out, gain, out_peak;
	volatile u32 dump;      /* team.py dump: 1 arm -> 2 recording -> 3 done (driver) */
	u32 dump_addr, dump_frames, dump_rate;
	struct xt_peer_status peer[PEERS];
	struct xt_talker_status talker[TALKERS];
	/* version 2: the send path (WP4.2), team.py TX_FIELDS */
	struct xt_tx_status {
		volatile u32 wake;   /* control: 1 = the net thread waits on an event each queued
				      * packet sets (0 = it only polls, every POLL_MS) */
		/* driver thread */
		u32 why;             /* TX_SENDING (0), else why the mic isn't sent (TX_OFF_*) */
		u32 mic_calls, mic_pad, mic_rate, mic_ch;
		u32 mic_open, mic_muted, gain_q15;   /* the gate, the mute, the level ramp's target */
		u32 talking, spurts, queued, dropped; /* dropped: the ring was full */
		u32 depth_max;        /* the most packets waiting for the net thread */
		u32 peak, clock;      /* peak of what was queued; the 16 kHz sample clock */
		u32 close_ms, close_ms_max; /* the gate closing to the end of the last packet */
		u32 game_silenced;    /* xt_mic_to_game() calls that said "silence" */
		/* net thread */
		u32 sent, discarded, wakes;
	} tx;
};
#define TX_WORDS 22
_Static_assert(sizeof(struct xt_tx_status) == TX_WORDS * 4, "team.py TX_FIELDS");
_Static_assert(sizeof(struct xt_status) == (73 + PEERS * 14 + TALKERS * 16 + TX_WORDS) * 4,
	       "team.py FIELDS");

enum { NS_CLOSED, NS_OPEN, NS_FAILED, NS_REFUSED };
enum { C_IDLE, C_BUFFERING, C_RUNNING };

__attribute__((used)) struct xt_status g_xt = {
	.magic = XT_MAGIC,
	.version = 2,    /* 2: the tx tail (WP4.2); everything before it is version 1's layout */
	.stage = 0,      /* off at boot: no thread, no socket (PLAN.md section 0) */
	.channel = 0,    /* TEAM CH off until the panel (or team.py) picks one */
	.level = 10,
	.mic_route = 0,  /* TEAM (PLAN.md 4.1): at stage 4 with a channel, the 360 gets silence */
	.opts = XN_OPT_INSECURE | XN_OPT_BROADCAST | XN_OPT_REUSEADDR,
	.startup = XN_START_XNET | XN_START_WSA, /* as HaloLAN */
	.hw_thread = 5,  /* not 2 (driver, Usbd) or 4 (XAM thread, audio render) */
	.priority = 8,   /* below the driver thread's 9 */
	.tx = { .wake = 1, .why = TX_OFF_STAGE, .mic_pad = 0xFFFFFFFFu },
};

/* ---- the net thread's entry: an NV64 thunk (docs/INTERNALS.md), as thunks_common.s's macro.
 * Kept here so team voice adds no line to the shared thunk files. -------------------------- */
#ifndef XT_HOST_TEST
__asm__(
	"	.text\n"
	"	.globl XtNetThread\n"
	"	.p2align 2\n"
	"XtNetThread:\n"
	"	mflr 0\n"
	"	stwu 1, -176(1)\n"
	"	stw 0, 168(1)\n"
	"	.irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
	"	std \\r, (16 + (\\r - 14) * 8)(1)\n"
	"	.endr\n"
	"	bl XtNetThreadC\n"
	"	.irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
	"	ld \\r, (16 + (\\r - 14) * 8)(1)\n"
	"	.endr\n"
	"	lwz 0, 168(1)\n"
	"	mtlr 0\n"
	"	addi 1, 1, 176\n"
	"	blr\n");
#endif
u32 XtNetThread(void *context);

#define NET_STACK 0x8000
#define KTHREAD_PRIORITY_BASE 0xc9
#define ORD_XexExecutableModuleHandle 403

/* ---- shared state ------------------------------------------------------------------------- */
struct talker {
	/* net thread (producer) */
	volatile u32 gen;        /* a new stream: the reader starts over at `start` */
	volatile u32 start;      /* stream position of this generation's first sample */
	volatile u32 hi;         /* stream position after the last sample written */
	volatile u32 target;     /* jitter target, samples */
	volatile u32 ended;      /* BYE or the peer is gone: play out, no underrun */
	volatile u32 last_rx_ms;
	u32 used, id, base;      /* base: the sender timestamp at stream position 0 */
	u32 last_n, conceal_run;
	s32 jit_q4;              /* RFC 3550 interarrival jitter, samples Q4 */
	u32 transit, have_transit;
	s16 last[XT_MAX_SAMPLES]; /* the last packet, repeated (faded) for a lost one */
	/* driver thread (consumer) */
	volatile u32 rgen;       /* the generation it reads */
	volatile u32 rpos;       /* the next sample it reads (published after each block) */
	u32 state, frac, fade;
	u32 pending;             /* ran dry while packets were recent: an underrun, unless the
				  * talker turns out to have stopped (then a talk spurt's end) */
	s32 a, b, prev, o;       /* the two samples the output lies between; prev: filter; o: last */
	s32 avg_q4, ppm;
	u32 mixed_frames;
	s16 ring[RING];
};
static struct talker g_tk[TALKERS];

struct peer {            /* net thread only */
	u32 used, id, ip, chan, flags, level, last_ms;
	u32 rx_hello, rx_audio, lost, late;
	int talker;          /* -1 none */
	char name[XT_NAME];
};
static struct peer g_peer[PEERS];

/* the mic packets, driver thread -> net thread (one producer, one consumer) */
struct txpkt {
	u32 ts, flags;
	s16 pcm[XT_PACKET];
};
static struct txpkt g_txq[TXQ];
static volatile u32 g_txq_w;      /* driver: packets queued */
static volatile u32 g_txq_r;      /* net: packets taken */
static volatile u32 g_tx_flags;   /* driver -> net: XT_F_TALKING / XT_F_MUTED for HELLO */
/* The SDK's KEVENT is an opaque 4-byte type, but KeInitializeEvent writes 16 bytes (type,
 * state, the wait list at +8 / +12): declaring a KEVENT overwrites what follows it (the
 * loopwake freeze, fe3c085). The real layout, as xw_plugin.c's struct xw_kevent; cast to
 * KEVENT * only at the calls. */
struct xt_kevent {
	u8 type, absolute, size, inserted;
	volatile s32 signal_state;
	u32 wait_flink, wait_blink;
};
_Static_assert(sizeof(struct xt_kevent) == 16, "KEVENT on 17559 (KeInitializeEvent)");
#ifndef XT_HOST_TEST
static struct xt_kevent g_net_wake __attribute__((aligned(16))); /* set per queued packet */
#endif
static volatile u32 g_net_wake_ready;

static volatile u32 g_quiesced;   /* driver -> net: close, send BYE, park */
static volatile u32 g_parked;     /* net -> driver: socket closed while quiesced */
static u32 g_thread_started;      /* driver */

/* net thread -> driver thread log events */
#define EVENTS 32
enum { EV_OPEN = 1, EV_OPEN_FAIL, EV_CLOSE, EV_PEER, EV_PEER_GONE, EV_TALKER, EV_TALKER_END,
       EV_REOPEN, EV_ADDR, EV_REFUSED, EV_PRIORITY };
struct ev { u32 code, a, b, c; };
static struct ev g_ev[EVENTS];
static volatile u32 g_ev_w;
static u32 g_ev_r;

static void ev(u32 code, u32 a, u32 b, u32 c)
{
	u32 w = g_ev_w;
	struct ev *e = &g_ev[w % EVENTS];

	e->code = code;
	e->a = a;
	e->b = b;
	e->c = c;
	BARRIER();
	g_ev_w = w + 1;
}

#ifdef XT_HOST_TEST
u64 xt_host_tb(void);
#define tb64 xt_host_tb
u8 *xt_host_kthread(void);
#define current_kthread xt_host_kthread
#else
static u64 tb64(void)
{
	u32 hi, lo, hi2;

	do {
		__asm__ __volatile__("mftbu %0" : "=r"(hi));
		__asm__ __volatile__("mftb %0" : "=r"(lo));
		__asm__ __volatile__("mftbu %0" : "=r"(hi2));
	} while (hi != hi2);
	return (u64)hi << 32 | lo;
}

static u8 *current_kthread(void)
{
	u32 pcr;

	__asm__ volatile("mr %0, 13" : "=r"(pcr));
	return *(u8 **)(pcr + 0x100);
}
#endif

static s32 clamp16(s32 v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static u32 abs32(s32 v)
{
	return v < 0 ? (u32)-v : (u32)v;
}

/* ---- net thread: sending ---------------------------------------------------------------- */
static u32 g_sock = 0xFFFFFFFFu;
static u16 g_port;
static u32 g_opened_opts, g_opened_startup;
static u32 g_sender;
static u16 g_seq[4];              /* per type */
static u32 g_send_fail_run, g_recv_fail_run;
static u8 g_tx[sizeof(struct xt_hdr) + 2 * XT_PACKET] __attribute__((aligned(8)));
static u8 g_rx[1536] __attribute__((aligned(8)));
static u32 g_hello_chan;          /* the channel the last HELLO announced */

static u32 dest_ip(void)
{
	u32 d = g_xt.dest;

	return d ? d : XN_BROADCAST;
}

static void put_hdr(u8 *b, u32 type, u32 flags, u32 samples, u32 ts)
{
	struct xt_hdr h;

	h.magic = XT_MAGIC;
	h.version = XT_VERSION;
	h.type = (u8)type;
	h.channel = (u8)(g_xt.channel <= XT_CHANNELS ? g_xt.channel : 0);
	h.flags = (u8)flags;
	h.sender = g_sender;
	h.seq = g_seq[type & 3]++;
	h.samples = (u16)samples;
	h.timestamp = ts;
	memcpy(b, &h, sizeof(h)); /* big-endian on the 360: the wire order */
}

static void send_buf(const u8 *b, int len)
{
	u64 t0 = tb64();
	u32 err = xn_send_to(g_sock, dest_ip(), g_port, b, len);
	u32 us = (u32)(tb64() - t0) / TB_PER_US;

	g_xt.tx_calls++;
	g_xt.tx_us_last = us;
	g_xt.tx_us_sum += us;
	if (us > g_xt.tx_us_max)
		g_xt.tx_us_max = us;
	if (err) {
		g_xt.err_send = err;
		g_xt.send_fails++;
		g_send_fail_run++;
		return;
	}
	g_send_fail_run = 0;
	g_xt.tx_bytes += (u32)len;
}

static void send_hello(void)
{
	struct xt_hello h;
	static const char hex[] = "0123456789ABCDEF";

	memset(&h, 0, sizeof(h));
	/* the pad user's gamertag needs a XAM call (the XAM thread): later. "XBOX-" + id */
	memcpy(h.name, "XBOX-", 5);
	for (int i = 0; i < 4; i++)
		h.name[5 + i] = hex[(g_sender >> (12 - 4 * i)) & 15];
	h.level = 0; /* reserved */
	put_hdr(g_tx, XT_HELLO, g_tx_flags & (XT_F_TALKING | XT_F_MUTED), 0, 0);
	memcpy(g_tx + sizeof(struct xt_hdr), &h, sizeof(h));
	send_buf(g_tx, sizeof(struct xt_hdr) + sizeof(h));
	g_xt.tx_hello++;
	g_hello_chan = g_xt.channel;
}

static void send_bye(void)
{
	put_hdr(g_tx, XT_BYE, 0, 0, 0);
	send_buf(g_tx, sizeof(struct xt_hdr));
	g_xt.tx_bye++;
}

/* one AUDIO packet of XT_PACKET samples at stream time ts (16 kHz sample clock) */
static void send_audio(const s16 *pcm, u32 ts, u32 flags)
{
	put_hdr(g_tx, XT_AUDIO, flags | XT_F_TALKING, XT_PACKET, ts);
	memcpy(g_tx + sizeof(struct xt_hdr), pcm, 2 * XT_PACKET); /* s16 BE = native */
	send_buf(g_tx, sizeof(g_tx));
	g_xt.tx_audio++;
}

/* ---- the test tone (stage 4, test_tone): paced by the timebase -------------------------- */
static const s16 SIN_Q[65] = {
	0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179,
	7962, 8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732,
	15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403,
	22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
	27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571,
	30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
	32609, 32678, 32728, 32757, 32767,
};

/* sine of a Q32 phase, Q15 */
static s32 isin(u32 ph)
{
	u32 q = ph >> 30, i = (ph >> 24) & 63, f = (ph >> 8) & 0xFFFF;
	s32 a, b, v;

	if (q & 1) { /* falling quarter: mirror */
		a = SIN_Q[64 - i];
		b = SIN_Q[63 - i];
		v = a + (((b - a) * (s32)f) >> 16);
	} else {
		a = SIN_Q[i];
		b = SIN_Q[i + 1];
		v = a + (((b - a) * (s32)f) >> 16);
	}
	return q & 2 ? -v : v;
}

static struct {
	u32 on, phase, ts;
	u64 tb0, sent;      /* the timebase at the start; samples sent since */
} g_tone;

static void tone_poll(void)
{
	s16 pcm[XT_PACKET];
	u64 due;

	if (!g_tone.on) {
		g_tone.on = 1;
		g_tone.tb0 = tb64();
		g_tone.sent = 0;
		xa_random(&g_tone.ts, sizeof(g_tone.ts));
	}
	due = (tb64() - g_tone.tb0) / TB_PER_SAMPLE;
	if (due > g_tone.sent + 10 * XT_PACKET) { /* the thread was held up 100 ms: skip ahead */
		g_xt.tone_skips++;
		g_tone.ts += (u32)(due - g_tone.sent);
		g_tone.sent = due;
	}
	while (g_tone.sent + XT_PACKET <= due) {
		for (int i = 0; i < XT_PACKET; i++) {
			pcm[i] = (s16)(isin(g_tone.phase) * TONE_PEAK >> 15);
			g_tone.phase += TONE_INC;
		}
		send_audio(pcm, g_tone.ts, 0);
		g_tone.ts += XT_PACKET;
		g_tone.sent += XT_PACKET;
	}
}

/* ---- send half (WP4.2): the mic packets xt_mic queued, sent in order. Anything queued while
 * sending isn't wanted (a lower stage, test tone 1, the socket closed) is discarded, so the
 * ring never fills with stale audio. ----------------------------------------------------- */
static void tx_drain(u32 want)
{
	u32 w = g_txq_w;

	BARRIER(); /* the count before the packets it covers */
	while (g_txq_r != w) {
		struct txpkt *q = &g_txq[g_txq_r & (TXQ - 1)];

		if (want >= 4 && g_xt.net_state == NS_OPEN && g_xt.test_tone != 1) {
			send_audio(q->pcm, q->ts, q->flags);
			g_xt.tx.sent++;
		} else {
			g_xt.tx.discarded++;
		}
		BARRIER(); /* done with the slot before the driver may reuse it */
		g_txq_r++;
	}
}

/* the net thread's wait between passes: ms, or less when the driver queues a mic packet */
static void net_wait(u32 ms)
{
#ifndef XT_HOST_TEST
	if (g_xt.tx.wake && g_net_wake_ready) {
		int64_t timeout = -(int64_t)ms * 10000; /* relative, 100 ns units */

		/* Executive, KernelMode, not alertable */
		if (KeWaitForSingleObject((KEVENT *)&g_net_wake, 0, 0, 0, &timeout) == 0)
			g_xt.tx.wakes++; /* set, not timed out (STATUS_TIMEOUT is 0x102) */
		return;
	}
#endif
	xa_sleep_ms(ms);
}

/* ---- net thread: the socket ------------------------------------------------------------- */
static void net_close(int bye)
{
	if (g_sock != 0xFFFFFFFFu) {
		if (bye)
			send_bye();
		xn_close(g_sock);
		g_sock = 0xFFFFFFFFu;
	}
	xn_cleanup();
	g_xt.socket = 0xFFFFFFFFu;
	g_xt.net_state = NS_CLOSED;
	g_tone.on = 0;
}

static void read_addr(void)
{
	u8 mac[6];
	u32 ip = 0;

	g_xt.xnaddr_flags = xn_xnaddr(mac, &ip);
	g_xt.ip = ip;
	g_xt.link = xn_link();
	if (!g_sender) {
		u32 id = (u32)mac[2] << 24 | (u32)mac[3] << 16 | (u32)mac[4] << 8 | mac[5];

		if (!id)
			xa_random(&id, sizeof(id));
		g_sender = id ? id : 1;
		g_xt.sender = g_sender;
		g_xt.mac_hi = (u32)mac[0] << 8 | mac[1];
		ev(EV_ADDR, ip, g_sender, g_xt.xnaddr_flags);
	}
}

static void net_open(void)
{
	u32 xnet = 0, wsa = 0, step = 0, err;
	u32 opts = g_xt.opts, startup = g_xt.startup;

	g_xt.opens++;
	g_port = (u16)(g_xt.port ? g_xt.port : XT_PORT);
	g_opened_opts = opts;
	g_opened_startup = startup;
	err = xn_startup(startup, &xnet, &wsa);
	g_xt.err_xnet = xnet;
	g_xt.err_wsa = wsa;
	if (!err)
		err = xn_open_udp(g_port, opts, 0x20000 /* 128 KB, as HaloLAN */, &g_sock, &step);
	else
		step = 0x80; /* the startup itself */
	g_xt.err_open = err;
	g_xt.open_step = step;
	if (err) {
		xn_cleanup();
		g_xt.net_state = NS_FAILED;
		ev(EV_OPEN_FAIL, err, step, xnet << 16 | (wsa & 0xFFFF));
		return;
	}
	g_xt.socket = g_sock;
	g_xt.net_state = NS_OPEN;
	g_send_fail_run = g_recv_fail_run = 0;
	read_addr();
	ev(EV_OPEN, g_port, opts, startup);
}

/* ---- net thread: peers and talkers ------------------------------------------------------ */
static void talker_release(struct peer *p)
{
	if (p->talker >= 0) {
		struct talker *k = &g_tk[p->talker];

		k->ended = 1;
		k->used = 0;
		if (g_xt.debug)
			ev(EV_TALKER_END, p->id, (u32)p->talker, 0);
		p->talker = -1;
	}
}

static struct peer *peer_of(u32 id, u32 now, bool make)
{
	struct peer *free_p = 0, *oldest = 0;

	for (int i = 0; i < PEERS; i++) {
		struct peer *p = &g_peer[i];

		if (p->used && p->id == id)
			return p;
		if (!p->used && !free_p)
			free_p = p;
		if (p->used && (!oldest || p->last_ms < oldest->last_ms))
			oldest = p;
	}
	if (!make)
		return 0;
	if (!free_p) {
		free_p = oldest; /* full: the one heard least recently goes */
		talker_release(free_p);
	}
	memset(free_p, 0, sizeof(*free_p));
	free_p->used = 1;
	free_p->id = id;
	free_p->talker = -1;
	free_p->last_ms = now;
	if (g_xt.debug)
		ev(EV_PEER, id, 0, 0);
	return free_p;
}

static void age_peers(u32 now)
{
	for (int i = 0; i < PEERS; i++) {
		struct peer *p = &g_peer[i];

		if (p->used && now - p->last_ms > PEER_TIMEOUT_MS) {
			talker_release(p);
			p->used = 0;
			ev(EV_PEER_GONE, p->id, p->rx_hello, p->rx_audio);
		}
	}
	/* a talker whose peer stopped sending audio frees its slot after the talk spurt */
	for (int t = 0; t < TALKERS; t++) {
		struct talker *k = &g_tk[t];

		if (k->used && now - k->last_rx_ms > 2000) {
			for (int i = 0; i < PEERS; i++)
				if (g_peer[i].used && g_peer[i].talker == t)
					talker_release(&g_peer[i]);
			k->used = 0;
			k->ended = 1;
		}
	}
}

/* a talker slot for this peer: its own, a free one, or the one idle longest (over 200 ms) */
static int talker_for(struct peer *p, u32 now)
{
	int best = -1;

	if (p->talker >= 0)
		return p->talker;
	for (int t = 0; t < TALKERS; t++) {
		struct talker *k = &g_tk[t];

		if (!k->used) {
			best = t;
			break;
		}
		if (now - k->last_rx_ms > 200 &&
		    (best < 0 || k->last_rx_ms < g_tk[best].last_rx_ms))
			best = t;
	}
	if (best < 0)
		return -1;
	for (int i = 0; i < PEERS; i++)
		if (g_peer[i].used && g_peer[i].talker == best)
			talker_release(&g_peer[i]);
	g_tk[best].used = 1;
	g_tk[best].id = p->id;
	g_tk[best].ended = 1; /* the first packet starts a stream */
	g_xt.talker[best].id = p->id;
	p->talker = best;
	if (g_xt.debug)
		ev(EV_TALKER, p->id, (u32)best, 0);
	return best;
}

static u32 target_of(struct talker *k)
{
	u32 j = (u32)(k->jit_q4 >> 4), t;

	if (g_xt.jb_ms)
		return (g_xt.jb_ms > 200 ? 200 : g_xt.jb_ms) * 16;
	t = 15 * 16 + 3 * j; /* 15 ms + 3 x jitter */
	if (t < TARGET_MIN_MS * 16)
		t = TARGET_MIN_MS * 16;
	if (t > TARGET_MAX_MS * 16)
		t = TARGET_MAX_MS * 16;
	return t;
}

static void ring_put(struct talker *k, u32 pos, const s16 *s, u32 n, s32 gain_q8)
{
	for (u32 i = 0; i < n; i++)
		k->ring[(pos + i) & (RING - 1)] = (s16)(gain_q8 == 256 ? s[i] : s[i] * gain_q8 >> 8);
}

static void new_stream(struct talker *k, struct xt_status *st, int t, u32 ts, const s16 *s,
		       u32 n)
{
	u32 start = k->hi + RING; /* well past anything the reader may still be on */

	k->base = ts - start;
	ring_put(k, start, s, n, 256);
	k->start = start;
	k->hi = start + n;
	k->ended = 0;
	k->conceal_run = 0;
	k->have_transit = 0;
	BARRIER(); /* samples and positions before the generation */
	k->gen++;
	st->talker[t].restarts++;
}

/* an AUDIO packet's samples into its talker's ring */
static void talker_audio(struct peer *p, int t, const struct xt_hdr *h, const s16 *s, u32 now)
{
	struct talker *k = &g_tk[t];
	struct xt_talker_status *ts = &g_xt.talker[t];
	u32 n = h->samples, rd, pos, arrival;
	s32 d;

	/* jitter (RFC 3550), in 16 kHz samples */
	arrival = (u32)(tb64() / TB_PER_SAMPLE);
	if (k->have_transit) {
		u32 dd = abs32((s32)((arrival - h->timestamp) - k->transit));

		if (dd > XT_RATE)
			dd = XT_RATE; /* a pause or a restart, not jitter */
		k->jit_q4 += ((s32)(dd << 4) - k->jit_q4) >> 4;
	}
	k->transit = arrival - h->timestamp;
	k->have_transit = 1;
	k->target = target_of(k);
	ts->target_ms = k->target / 16;
	ts->jitter_us = (u32)(k->jit_q4 >> 4) * 1000 / 16;

	if (k->ended || now - k->last_rx_ms > SPURT_MS) {
		/* the first packet, or the talker paused: a new stream */
		k->last_rx_ms = now;
		new_stream(k, &g_xt, t, h->timestamp, s, n);
		goto keep;
	}
	k->last_rx_ms = now;
	pos = h->timestamp - k->base;
	d = (s32)(pos - k->hi);
	rd = k->rgen == k->gen ? k->rpos : k->start; /* where the reader is (or will start) */
	if (d > RESTART_GAP || d < -RESTART_GAP || (s32)(pos + n - rd) > RING - GUARD ||
	    (s32)(k->hi - rd) > RING - GUARD) {
		/* a jump, or nobody is reading (no headset): start over at this packet */
		new_stream(k, &g_xt, t, h->timestamp, s, n);
		goto keep;
	}
	if (d >= 0) {
		/* in order, or after a loss: repeat the last packet, fading, over the gap */
		u32 gap = (u32)d;

		if (gap) {
			u32 lost = (gap + k->last_n - 1) / (k->last_n ? k->last_n : 1);

			p->lost += lost;
			ts->concealed += lost;
			for (u32 at = k->hi; at < pos;) {
				u32 m = pos - at < k->last_n ? pos - at : k->last_n;
				s32 g = k->conceal_run < 3 ? 256 >> (k->conceal_run + 1) : 0;

				if (!m) { /* no last packet (can't happen after the first): silence */
					for (; at < pos; at++)
						k->ring[at & (RING - 1)] = 0;
					break;
				}
				ring_put(k, at, k->last, m, g);
				at += m;
				k->conceal_run++;
			}
		} else {
			k->conceal_run = 0;
		}
		ring_put(k, pos, s, n, 256);
		BARRIER(); /* the samples before the position that publishes them */
		k->hi = pos + n;
	} else if ((s32)(pos - rd) >= GUARD) {
		/* late, but not yet played: the real samples replace the concealment */
		ring_put(k, pos, s, n, 256);
		ts->recovered++;
		if (p->lost)
			p->lost--;
		return;
	} else {
		p->late++;
		ts->late++;
		return;
	}
keep:
	if (n <= XT_MAX_SAMPLES) {
		memcpy(k->last, s, 2 * n);
		k->last_n = n;
	}
}

/* ---- net thread: receiving -------------------------------------------------------------- */
static void handle_packet(const u8 *b, int len, u32 ip, u32 now, u32 want)
{
	struct xt_hdr h;
	struct peer *p;
	u32 ch = g_xt.channel;

	if (len < (int)sizeof(h)) {
		g_xt.rx_bad++;
		return;
	}
	memcpy(&h, b, sizeof(h));
	if (h.magic != XT_MAGIC || h.version != XT_VERSION || h.type < XT_HELLO || h.type > XT_BYE ||
	    h.channel > XT_CHANNELS) {
		g_xt.rx_bad++;
		return;
	}
	if (h.sender == g_sender) {
		g_xt.rx_own++;
		if (!g_xt.loop_own)
			return;
		h.sender ^= 0x80000000u; /* our own packets as another talker (loopback test) */
	}
	if (h.type == XT_HELLO) {
		struct xt_hello hl;

		if (len < (int)(sizeof(h) + sizeof(hl))) {
			g_xt.rx_bad++;
			return;
		}
		memcpy(&hl, b + sizeof(h), sizeof(hl));
		g_xt.rx_hello++;
		p = peer_of(h.sender, now, true);
		p->ip = ip;
		p->chan = h.channel;
		p->flags = h.flags;
		p->level = hl.level;
		p->last_ms = now;
		p->rx_hello++;
		memcpy(p->name, hl.name, XT_NAME);
		p->name[XT_NAME - 1] = 0;
		return;
	}
	if (h.type == XT_BYE) {
		g_xt.rx_bye++;
		p = peer_of(h.sender, now, false);
		if (p) {
			talker_release(p);
			p->used = 0;
			ev(EV_PEER_GONE, p->id, p->rx_hello, p->rx_audio);
		}
		return;
	}
	/* AUDIO */
	if (h.samples == 0 || h.samples > XT_MAX_SAMPLES ||
	    len != (int)(sizeof(h) + 2u * h.samples)) {
		g_xt.rx_bad++;
		return;
	}
	g_xt.rx_audio++;
	p = peer_of(h.sender, now, true);
	p->ip = ip;
	p->chan = h.channel;
	p->flags = h.flags;
	p->last_ms = now;
	p->rx_audio++;
	if (want < 3 || ch == 0 || (h.channel != ch && !(h.flags & XT_F_ALLCALL))) {
		if (ch && h.channel != ch)
			g_xt.rx_other_ch++;
		return;
	}
	{
		int t = talker_for(p, now);

		if (t < 0) {
			g_xt.rx_no_talker++;
			return;
		}
		talker_audio(p, t, &h, (const s16 *)(b + sizeof(h)), now);
	}
}

static void rx_drain(u32 now, u32 want)
{
	for (int i = 0; i < RX_PER_PASS; i++) {
		int len = 0;
		u32 ip = 0;
		u16 port = 0;
		u64 t0 = tb64();
		u32 err = xn_recv(g_sock, g_rx, sizeof(g_rx), &len, &ip, &port);
		u32 us = (u32)(tb64() - t0) / TB_PER_US;

		if (us > g_xt.rx_us_max)
			g_xt.rx_us_max = us;
		if (err == XN_WOULDBLOCK) {
			g_recv_fail_run = 0;
			return;
		}
		if (err) {
			g_xt.err_recv = err;
			g_xt.recv_fails++;
			g_recv_fail_run++;
			return;
		}
		g_recv_fail_run = 0;
		g_xt.rx_raw++;
		g_xt.rx_bytes += (u32)len;
		if (want >= 2)
			handle_packet(g_rx, len, ip, now, want);
	}
}

static void publish(u32 now)
{
	u32 team = 0, known = 0, talking = 0;

	for (int i = 0; i < PEERS; i++) {
		struct peer *p = &g_peer[i];
		struct xt_peer_status *s = &g_xt.peer[i];

		if (!p->used) {
			s->id = 0;
			continue;
		}
		known++;
		if (g_xt.channel && p->chan == g_xt.channel && !(p->flags & XT_F_PC))
			team++;
		s->id = p->id;
		s->ip = p->ip;
		s->chan = p->chan | p->flags << 8 | p->level << 16;
		memcpy(s->name, p->name, XT_NAME);
		s->age_ms = now - p->last_ms;
		s->rx_hello = p->rx_hello;
		s->rx_audio = p->rx_audio;
		s->lost = p->lost;
		s->late = p->late;
		s->jitter_us = p->talker >= 0 ? g_xt.talker[p->talker].jitter_us : 0;
		s->talker = p->talker >= 0 ? (u32)p->talker + 1 : 0;
	}
	for (int t = 0; t < TALKERS; t++)
		if (g_tk[t].used && now - g_tk[t].last_rx_ms < 200)
			talking |= 1u << t;
	g_xt.peers_known = known;
	g_xt.team_peers = team;
	g_xt.talking_mask = talking;
}

static void forget_peers(void)
{
	for (int i = 0; i < PEERS; i++) {
		talker_release(&g_peer[i]);
		g_peer[i].used = 0;
	}
}

BODY u32 XtNetThreadC(void *context)
{
	u32 next_hello = 0, next_publish = 0, next_retry = 0, next_addr = 0, prio = 0;
	u32 was_hello = 0;

	(void)context;
	for (;;) {
		u32 now = (u32)xa_now_ms();
		u32 want = g_quiesced ? 0 : g_xt.stage;

		g_xt.heartbeat++;
		g_xt.now_ms = now;
		if (want > 4)
			want = 4;
		if (g_xt.priority != prio && g_xt.priority >= 1 && g_xt.priority <= 9) {
			u8 *kt = current_kthread();

			prio = g_xt.priority;
			KeSetBasePriorityThread(kt, (u32)((s32)prio - (s32)kt[KTHREAD_PRIORITY_BASE]));
			ev(EV_PRIORITY, prio, kt[KTHREAD_PRIORITY_BASE], 0);
		}

		if (g_xt.net_state == NS_OPEN &&
		    (want == 0 || g_xt.opts != g_opened_opts || g_xt.startup != g_opened_startup ||
		     (g_xt.port ? g_xt.port : XT_PORT) != g_port)) {
			net_close(was_hello);
			was_hello = 0;
			forget_peers();
			ev(EV_CLOSE, want, 0, 0);
		}
		if (want >= 1 && g_xt.net_state != NS_OPEN &&
		    (g_xt.net_state == NS_CLOSED || now >= next_retry)) {
			net_open();
			next_retry = now + RETRY_MS;
			next_hello = now;
		}
		if (g_xt.net_state == NS_OPEN) {
			if (g_send_fail_run >= REOPEN_AFTER_FAILS || g_recv_fail_run >= REOPEN_AFTER_FAILS) {
				g_xt.reopens++;
				ev(EV_REOPEN, g_xt.err_send, g_xt.err_recv, 0);
				net_close(0);
				net_open();
			}
		}
		if (g_xt.net_state == NS_OPEN) {
			rx_drain(now, want);
			if (want >= 2 && (now >= next_hello || g_xt.channel != g_hello_chan)) {
				send_hello();
				was_hello = 1;
				next_hello = now + HELLO_MS;
			} else if (want < 2 && was_hello) {
				send_bye();
				was_hello = 0;
			}
			if (want >= 4 && g_xt.test_tone == 1)
				tone_poll();
			else
				g_tone.on = 0;
			if (now >= next_addr) {
				read_addr();
				next_addr = now + 5000;
			}
			age_peers(now);
		}
		tx_drain(want); /* sends only while open at stage 4; otherwise empties the ring */
		if (now >= next_publish) {
			publish(now);
			next_publish = now + PUBLISH_MS;
		}
		g_parked = g_quiesced && g_xt.net_state != NS_OPEN;
		net_wait(g_xt.net_state == NS_OPEN ? POLL_MS : 20);
	}
}

/* ---- driver thread ---------------------------------------------------------------------- */
static HANDLE *pExecutableModule;
static u32 g_title_seen;
static u32 g_applied_gain = 256;
static s32 g_acc[ACC_MAX];
static s16 *g_dump;
static int g_mix_pad = -1;
static u64 g_mix_last_ms;

static void log_events(void)
{
	while (g_ev_r != g_ev_w) {
		struct ev e = g_ev[g_ev_r % EVENTS];

		BARRIER();
		g_ev_r++;
		switch (e.code) {
		case EV_OPEN:
			xa_log("team: socket open, UDP %u (options %x, startup %x)\n", e.a, e.b, e.c);
			break;
		case EV_OPEN_FAIL:
			xa_log("team: socket failed: error %u at step %u (XNetStartup %u, WSAStartup %u); "
			       "retrying every 5 s\n", e.a, e.b, e.c >> 16, e.c & 0xFFFF);
			break;
		case EV_CLOSE:
			xa_log("team: socket closed (stage %u)\n", e.a);
			break;
		case EV_ADDR:
			xa_log("team: this console is %u.%u.%u.%u, id %08x (xnaddr flags %x)\n",
			       e.a >> 24, (e.a >> 16) & 255, (e.a >> 8) & 255, e.a & 255, e.b, e.c);
			break;
		case EV_PEER:
			xa_log("team: new peer %08x\n", e.a);
			break;
		case EV_PEER_GONE:
			xa_log("team: peer %08x gone (%u hellos, %u audio packets)\n", e.a, e.b, e.c);
			break;
		case EV_TALKER:
			xa_log("team: talker %u = %08x\n", e.b, e.a);
			break;
		case EV_TALKER_END:
			xa_log("team: talker %u (%08x) released\n", e.b, e.a);
			break;
		case EV_REOPEN:
			xa_log("team: socket errors (send %u, receive %u): reopened\n", e.a, e.b);
			break;
		case EV_PRIORITY:
			xa_log("team: net thread priority %u (base was %u)\n", e.a, e.b);
			break;
		}
	}
}

void xt_init(void)
{
	/* nothing opened and no thread: XWTV.stage is 0 at boot */
}

static void start_thread(void)
{
	HANDLE thread = 0;
	u32 tid = 0, hw = g_xt.hw_thread;
	int err = xn_init();

	g_thread_started = 1;
	g_xt.init_error = (u32)err;
	g_xt.init_detail = xn_init_detail();
	if (err) {
		g_xt.net_state = NS_REFUSED;
		xa_log("team: network exports refused (%d, %08x): team voice stays off\n", err,
		       xn_init_detail());
		return;
	}
	if (hw > 5 || hw == 2 || hw == 4)
		hw = 5;
#ifndef XT_HOST_TEST
	/* synchronization: a set while the net thread is busy is kept for its next wait */
	KeInitializeEvent((KEVENT *)&g_net_wake, 1, 0);
	BARRIER();
	g_net_wake_ready = 1;
#endif
	ExCreateThread(&thread, NET_STACK, &tid, 0, (void *)XtNetThread, 0,
		       0x2 | (1u << hw) << 24);
	g_xt.thread = thread ? 1 : 0;
	xa_log("team: net thread %s on hardware thread %u\n", thread ? "started" : "NOT created", hw);
}

void xt_poll(void)
{
	u32 stage = g_xt.stage;

	g_xt.applied_stage = stage > 4 ? 4 : stage;
	if (stage >= 1 && !g_thread_started && !g_quiesced)
		start_thread();
	log_events();
	if (!pExecutableModule) {
		HANDLE krnl;
		void *p = 0;

		if (XexGetModuleHandle("xboxkrnl.exe", &krnl) >= 0)
			XexGetProcedureAddress(krnl, ORD_XexExecutableModuleHandle, &p);
		pExecutableModule = p;
	}
	if (pExecutableModule) {
		u32 title = (u32)(uintptr_t)*pExecutableModule;

		if (title != g_title_seen) {
			if (g_title_seen)
				g_xt.title_switches++;
			g_title_seen = title;
		}
	}
}

void xt_quiesce(void)
{
	u64 until;

	g_quiesced = 1;
	if (!g_xt.thread) {
		g_xt.quiesce = 1;
		return;
	}
	until = xa_now_ms() + QUIESCE_WAIT_MS;
	while (!g_parked && xa_now_ms() < until)
		xa_sleep_ms(5);
	g_xt.quiesce = g_parked ? 1 : 2; /* 2: the net thread didn't close in time */
	xa_log("team: quiesced (%s)\n", g_parked ? "socket closed, BYE sent" : "TIMED OUT");
	log_events();
}

void xt_resume(void)
{
	g_quiesced = 0;
	g_xt.quiesce = 0;
}

/* one talker's samples, resampled to rate_out, added to g_acc[0..frames) */
static void mix_talker(int t, u32 frames, u32 rate_out, u64 now)
{
	struct talker *k = &g_tk[t];
	struct xt_talker_status *ts = &g_xt.talker[t];
	u32 gen = k->gen, hi, avail, need, step, target, rpos;
	bool filter;

	BARRIER(); /* the generation before what it covers */
	if (gen != k->rgen) {
		if (k->pending) /* it paused and started over: the end of a talk spurt */
			ts->spurts++;
		k->pending = 0;
		k->rgen = gen;
		k->rpos = k->start;
		k->state = C_BUFFERING;
		k->avg_q4 = (s32)(k->target << 4);
		k->ppm = 0;
		ts->starts++;
	}
	if (k->state == C_IDLE)
		return;
	rpos = k->rpos;
	hi = k->hi;
	BARRIER(); /* the position before the samples it covers */
	avail = (s32)(hi - rpos) > 0 ? hi - rpos : 0;
	target = k->target ? k->target : TARGET_MIN_MS * 16;
	step = (16u << 20) / (rate_out / 1000); /* input samples per output frame, Q20 */
	filter = step >= ONE + ONE / 2;
	ts->state = k->state;
	if (k->state == C_BUFFERING) {
		if (avail < target + 2) {
			if (k->ended || now - k->last_rx_ms > SPURT_MS) {
				k->state = C_IDLE;
				if (k->pending)
					ts->spurts++;
				k->pending = 0;
			}
			ts->state = k->state;
			return;
		}
		if (k->pending) /* it ran dry and the packets kept coming: a real underrun */
			ts->underruns++;
		k->pending = 0;
		if (avail > target + 2 + XT_PACKET) {
			/* more than the target is waiting (nobody read it: no earpiece yet): start
			 * at the target, not with old audio and a long delay */
			rpos = hi - target - 2;
			avail = target + 2;
		}
		k->prev = 0;
		k->a = k->ring[rpos++ & (RING - 1)];
		k->b = k->ring[rpos++ & (RING - 1)];
		k->frac = 0;
		k->fade = 0;
		k->state = C_RUNNING;
		avail -= 2;
	}
	if (avail > target + HIGH_OVER) {
		/* far too full (this thread was held up, or a burst): skip to the target */
		ts->overruns++;
		rpos = hi - target;
		avail = target;
	}
	/* the drift trim, as game_360.c: the fill average (~0.5 s) steers the ratio */
	k->avg_q4 += ((s32)(avail << 4) - k->avg_q4) >> 6;
	{
		s32 err = (k->avg_q4 >> 4) - (s32)target;
		s32 ppm = err * DRIFT_MAX_PPM / (s32)(target / 3);

		k->ppm = ppm > DRIFT_MAX_PPM ? DRIFT_MAX_PPM : ppm < -DRIFT_MAX_PPM ? -DRIFT_MAX_PPM : ppm;
		ts->fill_ms = (u32)(k->avg_q4 >> 4) / 16;
		ts->trim_ppm = (u32)k->ppm;
	}
	step = (u32)((s32)step + (s32)(step >> 10) * k->ppm / 977);
	need = (k->frac + frames * step) >> 20;
	if (avail < need) {
		/* empty: the talker stopped (play out, then idle) or the packets are late */
		u32 n = frames < FADE ? frames : FADE;

		for (u32 f = 0; f < n; f++)
			g_acc[f] += k->o * (s32)(n - f) / (s32)n;
		k->o = 0;
		if (k->ended || now - k->last_rx_ms > SPURT_MS) {
			k->state = C_IDLE;
			ts->spurts++;
		} else {
			k->state = C_BUFFERING; /* counted once it is clear which it was */
			k->pending = 1;
		}
		ts->state = k->state;
		k->rpos = rpos;
		return;
	}
	for (u32 f = 0; f < frames; f++) {
		s32 x = k->a + (((k->b - k->a) * (s32)(k->frac >> 6)) >> 14);

		if (k->fade < FADE) {
			x = x * (s32)k->fade / FADE;
			k->fade++;
		}
		k->o = x;
		g_acc[f] += x;
		k->frac += step;
		while (k->frac >= ONE) {
			s32 y = k->ring[rpos++ & (RING - 1)];

			k->frac -= ONE;
			if (filter) { /* decimating: a 2-tap average first */
				s32 fy = (y + k->prev) >> 1;

				k->prev = y;
				y = fy;
			}
			k->a = k->b;
			k->b = y;
		}
	}
	BARRIER(); /* done reading before the net thread may reuse the slots */
	k->rpos = rpos;
	k->mixed_frames += frames;
	ts->mixed_ms = (u32)((u64)k->mixed_frames * 1000 / rate_out);
}

void xt_mix(int pad, u8 *out, int len, u32 rate_out, u32 ch_out, u32 gain)
{
	u64 now = xa_now_ms();
	u32 frames, peak = 0;
	s32 from;

	if (g_xt.applied_stage < 3 || !g_xt.channel || g_quiesced || len <= 0 || ch_out < 1 ||
	    ch_out > 2 || rate_out < 8000 || rate_out > 48000 || rate_out % 1000)
		return;
	/* one earpiece at a time (xw.xex streams one headset): the first that asks */
	if (pad != g_mix_pad) {
		if (g_mix_pad >= 0 && now - g_mix_last_ms < 200)
			return;
		g_mix_pad = pad;
	}
	g_mix_last_ms = now;
	g_xt.mix_calls++;
	g_xt.mix_pad = (u32)pad;
	g_xt.rate_out = rate_out;
	g_xt.ch_out = ch_out;
	if (gain > 256)
		gain = 256;
	g_xt.gain = gain;
	frames = (u32)len / (2 * ch_out);
	if (frames > ACC_MAX)
		frames = ACC_MAX;
	memset(g_acc, 0, frames * sizeof(g_acc[0]));
	for (int t = 0; t < TALKERS; t++)
		mix_talker(t, frames, rate_out, now);

	if (g_xt.dump == 1) {
		if (!g_dump)
			g_dump = malloc(DUMP_MAX * sizeof(s16));
		if (g_dump) {
			g_xt.dump_addr = (u32)(uintptr_t)g_dump;
			g_xt.dump_frames = 0;
			g_xt.dump_rate = rate_out;
			g_xt.dump = 2;
		}
	}
	from = (s32)g_applied_gain;
	g_applied_gain = gain;
	for (u32 f = 0; f < frames; f++) {
		s32 g = from + ((s32)gain - from) * (s32)f / (s32)frames; /* a level change ramps */
		s32 v = clamp16(g_acc[f]) * g >> 8;
		u8 *o = out + f * 2 * ch_out;

		if (abs32(v) > peak)
			peak = abs32(v);
		put_le16(o, (u16)clamp16((s16)get_le16(o) + v));
		if (ch_out == 2)
			put_le16(o + 2, (u16)clamp16((s16)get_le16(o + 2) + v));
		if (g_xt.dump == 2 && g_dump) {
			g_dump[g_xt.dump_frames++] = (s16)v;
			if (g_xt.dump_frames >= DUMP_MAX || g_xt.dump_frames >= rate_out)
				g_xt.dump = 3;
		}
	}
	if (peak > g_xt.out_peak)
		g_xt.out_peak = peak;
	for (int t = 0; t < TALKERS; t++)
		if (abs32(g_tk[t].o) > g_xt.talker[t].peak)
			g_xt.talker[t].peak = abs32(g_tk[t].o);
}

/* ---- the mic (WP4.2): driver thread ------------------------------------------------------
 * xt_mic gets every mic packet (1 ms, s16le at the headset's rate) with the gate's decision.
 * It does what voice_360.c does on the way to the 360 (mono, the mic level, the gate, both
 * ramped over 10 ms), resamples to 16 kHz and cuts packets of 160 samples on a running 16 kHz
 * sample clock (the AUDIO timestamp: a pause shows as a jump). A packet goes out if any of its
 * samples had a gain above 0, so a talk spurt ends 10 ms (the ramp) after the gate closes;
 * the gate's own 200 ms hold is the plan's hang. Muted, the gate is closed. The packets go to
 * the net thread through g_txq; the driver thread never calls NetDll. */

/* integer resampler, as voice_360.c's rs (static there): linear interpolation, a box
 * pre-filter when decimating (rates multiples of 1000 in 8000..48000) */
struct rs {
	u32 in, out, t, n, bi;
	s32 prev, sum;
	s32 box[8];
};

static u32 gcd32(u32 a, u32 b)
{
	while (b) {
		u32 t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static void rs_init(struct rs *r, u32 in_hz, u32 out_hz)
{
	u32 g = gcd32(in_hz, out_hz);

	memset(r, 0, sizeof(*r));
	r->in = in_hz / g;
	r->out = out_hz / g;
	r->n = r->in > r->out ? (r->in + r->out - 1) / r->out : 1;
	if (r->n > 8)
		r->n = 8;
}

/* one input sample in; 0..2 output samples to y (16 kHz out, at least 8 kHz in) */
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
		y[k++] = (s16)clamp16(r->prev + (x - r->prev) * (s32)r->t / (s32)r->out);
		r->t += r->in;
	}
	r->t -= r->out;
	r->prev = x;
	return k;
}

static struct {
	u32 on;              /* set up for this rate and channel count */
	u32 rate, ch;
	struct rs rs;
	u32 clock;           /* the 16 kHz sample clock: the timestamp of the next sample */
	u32 g, target;       /* the ramped gain, Q15, and where it goes */
	u32 n, ts, any;      /* the packet being cut: samples, its timestamp, a sample with g > 0 */
	u32 talking;         /* the last packet went out */
	u32 open, close_at;  /* the gate as last seen; the clock when it closed */
	u32 syn_n, syn_ph, syn_step; /* test_tone 2: mic frames since the start; the sine, Q32 */
	s16 pkt[XT_PACKET];
} g_mt;
static int g_mic_pad = -1;
static u64 g_mic_last_ms;

#ifdef XT_HOST_TEST
u32 xt_host_mic_gain = 100; /* tests/: the mic level in % (g_xd.applied_mic_gain) */
bool xt_host_muted;
#define MIC_GAIN_PCT() xt_host_mic_gain
#define MIC_MUTED(pad) xt_host_muted
#else
#define MIC_GAIN_PCT() g_xd.applied_mic_gain
#define MIC_MUTED(pad) xv_mic_muted(pad)
#endif

/* the mic level in effect as Q15, as voice_360.c applies it to the 360's mic */
static u32 mic_gain_q15(void)
{
	u32 pct = MIC_GAIN_PCT();

	if (pct > 100)
		pct = 100;
	return pct * 32768 / 100;
}

/* why the mic isn't sent now (TX_SENDING = it is) */
static u32 tx_why(void)
{
	if (g_quiesced)
		return TX_OFF_QUIESCED;
	if (g_xt.applied_stage < 4)
		return TX_OFF_STAGE;
	if (!xt_channel())
		return TX_OFF_CHANNEL;
	if (xt_mic_route() == 1)
		return TX_OFF_ROUTE;
	if (g_xt.test_tone == 1)
		return TX_OFF_TONE;
	if (g_xt.net_state != NS_OPEN)
		return TX_OFF_SOCKET;
	return TX_SENDING;
}

static void tx_queue(void)
{
	u32 w = g_txq_w, depth = w - g_txq_r;
	struct txpkt *q;

	if (depth >= TXQ) {
		g_xt.tx.dropped++; /* the net thread is held up: this packet is lost */
		return;
	}
	if (depth + 1 > g_xt.tx.depth_max)
		g_xt.tx.depth_max = depth + 1;
	q = &g_txq[w & (TXQ - 1)];
	q->ts = g_mt.ts;
	q->flags = 0;
	memcpy(q->pcm, g_mt.pkt, sizeof(q->pcm));
	BARRIER(); /* the packet before the count that publishes it */
	g_txq_w = w + 1;
	g_xt.tx.queued++;
#ifndef XT_HOST_TEST
	if (g_xt.tx.wake && g_net_wake_ready)
		KeSetEvent((KEVENT *)&g_net_wake, 0, 0);
#endif
}

/* one 16 kHz sample (before the gain) into the packet being cut */
static void tx_sample(s32 x)
{
	s32 y;

	if (g_mt.g < g_mt.target)
		g_mt.g = g_mt.target - g_mt.g > TX_STEP ? g_mt.g + TX_STEP : g_mt.target;
	else if (g_mt.g > g_mt.target)
		g_mt.g = g_mt.g - g_mt.target > TX_STEP ? g_mt.g - TX_STEP : g_mt.target;
	y = x * (s32)g_mt.g >> 15;
	if (!g_mt.n)
		g_mt.ts = g_mt.clock;
	if (g_mt.g)
		g_mt.any = 1;
	if (abs32(y) > g_xt.tx.peak)
		g_xt.tx.peak = abs32(y);
	g_mt.pkt[g_mt.n++] = (s16)y;
	g_mt.clock++;
	if (g_mt.n < XT_PACKET)
		return;
	if (g_mt.any) {
		if (!g_mt.talking) {
			g_mt.talking = 1;
			g_xt.tx.spurts++;
		}
		tx_queue();
	} else if (g_mt.talking) {
		/* the spurt ended with the packet before this one: the gate's close to its end */
		g_mt.talking = 0;
		g_xt.tx.close_ms = (g_mt.ts - g_mt.close_at) / 16;
		if (g_xt.tx.close_ms > g_xt.tx.close_ms_max)
			g_xt.tx.close_ms_max = g_xt.tx.close_ms;
	}
	g_mt.n = 0;
	g_mt.any = 0;
}

static void tx_stop(u32 why)
{
	g_xt.tx.why = why;
	g_mt.on = 0;
	g_mt.talking = 0;
	g_xt.tx.talking = 0;
	g_tx_flags &= ~XT_F_TALKING;
}

void xt_mic(int pad, const u8 *pcm, int len, u32 rate, u32 ch, bool open)
{
	u64 now;
	u32 why, frames, muted;
	s16 y[8];

	if (len <= 0)
		return;
	now = xa_now_ms();
	/* one mic at a time (xw.xex streams one headset): the first that sends */
	if (pad != g_mic_pad) {
		if (g_mic_pad >= 0 && now - g_mic_last_ms < MIC_IDLE_MS)
			return;
		g_mic_pad = pad;
		g_xt.tx.mic_pad = (u32)pad;
		g_mt.on = 0;
	}
	g_mic_last_ms = now;
	g_xt.tx.mic_calls++;
	muted = MIC_MUTED(pad) ? 1 : 0;
	g_xt.tx.mic_muted = muted;
	g_tx_flags = (g_tx_flags & ~XT_F_MUTED) | (muted ? XT_F_MUTED : 0);
	if (ch < 1 || ch > 2 || rate < 8000 || rate > 48000 || rate % 1000) {
		tx_stop(TX_OFF_FORMAT);
		return;
	}
	why = tx_why();
	if (why != TX_SENDING) {
		tx_stop(why);
		return;
	}
	g_xt.tx.why = TX_SENDING;
	if (!g_mt.on || g_mt.rate != rate || g_mt.ch != ch) {
		/* a fresh start; the sample clock runs on (from a random point the first time) */
		u32 clock = g_mt.clock;

		if (!clock)
			xa_random(&clock, sizeof(clock));
		memset(&g_mt, 0, sizeof(g_mt));
		rs_init(&g_mt.rs, rate, XT_RATE);
		g_mt.on = 1;
		g_mt.rate = rate;
		g_mt.ch = ch;
		g_mt.syn_step = (u32)(((u64)440 << 32) / rate);
		g_mt.clock = clock ? clock : 1;
		g_xt.tx.mic_rate = rate;
		g_xt.tx.mic_ch = ch;
	}
	frames = (u32)len / (2 * ch);
	for (u32 f = 0; f < frames; f++, pcm += 2 * ch) {
		s32 x;
		u32 gate;
		int k;

		if (g_xt.test_tone == 2) {
			/* the synthetic mic: 440 Hz at -20 dBFS, the gate 1 s on and 1 s off by the mic's
			 * own clock, at unity level so the far end can check it exactly; mute obeyed */
			gate = g_mt.syn_n % (2 * rate) < rate && !muted;
			x = isin(g_mt.syn_ph) * TONE_PEAK >> 15;
			g_mt.syn_ph += g_mt.syn_step;
			g_mt.syn_n++;
			g_mt.target = gate ? 32768 : 0;
		} else {
			x = (s16)get_le16(pcm);
			if (ch == 2)
				x = (x + (s16)get_le16(pcm + 2)) >> 1; /* as voice_360.c mixes it */
			gate = open ? 1 : 0;
			g_mt.target = gate ? mic_gain_q15() : 0;
		}
		if (g_mt.open && !gate)
			g_mt.close_at = g_mt.clock;
		g_mt.open = gate;
		k = rs_push(&g_mt.rs, x, y);
		for (int i = 0; i < k; i++)
			tx_sample(y[i]);
	}
	g_xt.tx.mic_open = g_mt.open;
	g_xt.tx.gain_q15 = g_mt.target;
	g_xt.tx.talking = g_mt.talking;
	g_xt.tx.clock = g_mt.clock;
	g_tx_flags = (g_tx_flags & ~XT_F_TALKING) | (g_mt.talking ? XT_F_TALKING : 0);
}

/* MIC TO: TEAM gives the 360 silence while the team gets the mic (stage 4, a channel, the
 * socket open); GAME and BOTH leave the 360's mic alone */
bool xt_mic_to_game(void)
{
	if (xt_mic_route() != 0 || tx_why() != TX_SENDING)
		return true;
	g_xt.tx.game_silenced++;
	return false;
}

/* ---- the panel ---------------------------------------------------------------------------- */
u32 xt_channel(void)
{
	return g_xt.channel <= XT_CHANNELS ? g_xt.channel : 0;
}

void xt_set_channel(u32 ch)
{
	g_xt.channel = ch <= XT_CHANNELS ? ch : 0;
}

u32 xt_level(void)
{
	return g_xt.level <= 10 ? g_xt.level : 10;
}

void xt_set_level(u32 level)
{
	g_xt.level = level <= 10 ? level : 10;
}

u32 xt_mic_route(void)
{
	return g_xt.mic_route <= 2 ? g_xt.mic_route : 0;
}

void xt_set_mic_route(u32 r)
{
	g_xt.mic_route = r <= 2 ? r : 0;
}

u32 xt_peers(void)
{
	return g_xt.team_peers;
}

u32 xt_talking_mask(void)
{
	return g_xt.talking_mask;
}
