// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The PC bridge (bridge_360.h; the wire format is mirrored in tools/bridge/xbox-bridge.py, the
 * status block in xbox360/bridge.py). The arithmetic is bridge_dsp.h's (tests/test_bridge.c).
 *
 * Threads:
 *   - the driver thread calls everything in bridge_360.h. It never calls NetDll: it writes the
 *     mic (stream 1) and the 360's chat (stream 2) into send rings, and reads streams 3 and 4
 *     from jitter buffers into the earpiece and the mic, inside the audio paths' own calls.
 *   - the net thread (XbNetThread: a system thread on hardware thread 5, priority 8, below the
 *     driver thread's 9; as team voice's) owns the socket (net_360.c, non-blocking): it takes
 *     the PC's hellos and audio, fills the jitter buffers, cuts the send rings into packets.
 *     It runs only in the system window after each video frame (docs/INTERNALS.md), so it
 *     handles about a frame's worth of packets per pass; the jitter target (50 ms) covers that.
 *   Each shared word has one writer (noted per field). Nothing here logs from the net thread
 *   (xa_log is not thread-safe): xb_poll logs what changed.
 *
 * Wire (big-endian, a 24-byte header on every datagram; UDP 3670 on the console):
 *   magic "XBRG", version 1, type, stream, channels, rate (Hz), seq, ts (the stream's frame
 *   position of the first frame), frames, flags
 *   HELLO  (PC -> console, type 1): u32 want (bit 0 stream 1, bit 1 stream 2). The console
 *          answers each with a STATUS (type 2): the formats it wants for streams 3 and 4 and
 *          its counters (xbox-bridge.py STATUS_FIELDS).
 *   AUDIO  (type 3): frames x channels s16 big-endian. Console -> PC: streams 1 and 2, mono at
 *          the headset's own rates, at most 600 frames (1224 bytes, one Ethernet frame). PC ->
 *          console: streams 3 and 4 in the format the STATUS asked for (any rate 8..48 kHz in
 *          kHz steps and 1 or 2 channels is taken), at most 1440 bytes of samples.
 *   BYE    (type 4): the PC is leaving.
 * Rates are chosen so the console resamples nothing it doesn't have to: the PC side converts.
 */
#include <stdlib.h>
#include <xecore/xboxkrnl.h>

#include "../core/platform.h"
#include "bridge_360.h"
#include "bridge_dsp.h"
#include "diag_360.h" /* xd_timebase */
#include "kernel_360.h"
#include "net_360.h"

/* ---- the wire ------------------------------------------------------------------------------ */
#define W_MAGIC 0x58425247u      /* "XBRG" (not the status block's letters) */
#define W_VERSION 1
#define W_PORT 3670
#define W_HDR 24
enum { W_HELLO = 1, W_STATUS = 2, W_AUDIO = 3, W_BYE = 4 };
#define TX_MAX_FRAMES 600        /* console -> PC: 1200 bytes of mono samples */
#define RX_MAX_BYTES 1440        /* PC -> console: samples per packet */
#define PKT_BUF 1536

/* ---- timing ---------------------------------------------------------------------------------- */
#define START_DELAY_MS 20000     /* after xb_init: the boot and the dashboard settle first */
#define PEER_TIMEOUT_MS 3000     /* no hello this long: the session ends */
#define POLL_MS 2                /* the net thread's sleep (it runs once per window anyway) */
#define IDLE_MS 50               /* ... with the socket closed */
#define RETRY_MS 5000            /* a failed open is tried again this often */
#define RX_PER_PASS 48
#define TX_PER_PASS 6            /* packets per stream and pass (a pass covers ~17 ms) */
#define REOPEN_AFTER_FAILS 100
#define PUBLISH_MS 100
#define QUIESCE_WAIT_MS 1000
#define TB_PER_US 50
#define NET_STACK 0x8000
#define NET_HW_THREAD 5          /* not 2 (driver, Usbd), 3 (file worker), 4 (XAM, audio render) */
#define NET_PRIORITY 8
#define KTHREAD_PRIORITY_BASE 0xc9

/* ---- the rings (one allocation, xb_init) -------------------------------------------------- */
#define JB_FRAMES 16384          /* 341 ms at 48 kHz; streams 3 (stereo) and 4 (mono) */
#define TX_FRAMES 8192           /* 341 ms at 24 kHz; streams 1 and 2 */
#define JB_MS_MIN 20
#define JB_MS_MAX 150
#define JB_MS_DEFAULT 50

/* ---- the xbdm block (bridge.py FIELDS mirrors it; all u32, append only) ---------------------- */
#define XB_MAGIC 0x58574252u     /* "XWBR" */
struct xb_stream_status {
	u32 packets;    /* 1, 2: sent; 3, 4: received */
	u32 lost;       /* 1, 2: samples skipped (the net thread fell behind); 3, 4: packets lost */
	u32 late;       /* 3, 4: packets too late to play (+ recovered << 16) */
	u32 underruns;  /* 3, 4: ran dry while the PC was sending */
	u32 restarts;   /* 3, 4: the stream started over (a new format, a jump, nobody reading) */
	u32 fill_ms;    /* 1, 2: queued now; 3, 4: the jitter buffer, averaged */
	u32 trim_ppm;   /* 3, 4: the drift trim (signed) */
	u32 peak;       /* the last second's largest sample */
	u32 rate, ch;   /* the format on the wire */
};
#define STREAM_WORDS 10
struct xb_status {
	u32 magic, version;
	/* controls (xbdm; bridge.py) */
	volatile u32 enable;        /* 1 (default): listen for a PC; 0: socket closed, nothing runs */
	volatile u32 level_headset; /* stream 3 in the earpiece, percent 0..100 */
	volatile u32 level_chat;    /* stream 4 into the 360's mic, percent 0..100 */
	volatile u32 jb_ms;         /* the jitter buffers' target, 20..150 */
	volatile u32 zero;          /* 1: the maxima start over (driver) */
	/* net thread */
	u32 state;                  /* XS_* */
	u32 port, peer_ip, peer_port, peer_age_ms, sessions, hellos, rx_bad, rx_other;
	u32 net_err, net_step, opens, heartbeat, net_us_max;
	/* driver thread: the added work in the audio paths */
	u32 mic_calls, ear_calls, mic_us_max, ear_us_max, pass_us, pass_us_max, pass_us_avg;
	struct xb_stream_status s[4];
};
#define XB_WORDS (28 + 4 * STREAM_WORDS)
_Static_assert(sizeof(struct xb_stream_status) == STREAM_WORDS * 4, "bridge.py STREAM_FIELDS");
_Static_assert(sizeof(struct xb_status) == XB_WORDS * 4, "bridge.py FIELDS");

enum { XS_OFF, XS_STARTING, XS_LISTENING, XS_CONNECTED, XS_FAILED, XS_REFUSED, XS_QUIESCED };

__attribute__((used)) struct xb_status g_xb = {
	.magic = XB_MAGIC,
	.version = 1,
	.enable = 1,            /* on: it does nothing until a PC says hello */
	.level_headset = 100,
	.level_chat = 100,
	.jb_ms = JB_MS_DEFAULT,
	.state = XS_STARTING,
	.port = W_PORT,
};

/* ---- shared state ----------------------------------------------------------------------- */
static struct xb_jb g_jb[2];      /* streams 3, 4: net thread writes, driver thread reads */
static struct xb_txr g_txr[2];    /* streams 1, 2: driver thread writes, net thread reads */
static s32 *g_acc;                /* driver: a block of stream 3 or 4 (XB_MAX_BLOCK x 2) */
static u8 *g_rx, *g_tx;           /* net: one datagram each */
static volatile u32 g_live;       /* net -> driver: bit 0 / 1 = send stream 1 / 2 (a PC wants it) */
static volatile u32 g_ear_rate, g_mic_rate; /* driver -> net: the headset's rates, for STATUS */
static volatile u32 g_ear_ms;     /* driver: the last earpiece block (a headset is streaming) */
static volatile u32 g_quiesced;   /* driver -> net: close the socket, park */
static volatile u32 g_parked;     /* net -> driver */
static u32 g_thread, g_ready;     /* driver */
static u32 g_now;                 /* driver: xa_now_ms() at the last xb_poll (a 64-bit division
				   * per audio call otherwise; a pass old at most, far below 250 ms) */
static u64 g_start_at;            /* driver */

/* the timebase's low word (50 MHz), inline: the audio calls read it twice each */
static inline u32 tb(void)
{
	u32 t;

	__asm__ __volatile__("mftb %0" : "=r"(t));
	return t;
}

static u8 *current_kthread(void)
{
	u32 pcr;

	__asm__ volatile("mr %0, 13" : "=r"(pcr));
	return *(u8 **)(pcr + 0x100);
}

static u32 be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static void put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

/* ---- the net thread's entry: an NV64 thunk (docs/INTERNALS.md) -------------------------------- */
__asm__(NV64_THUNK_ASM(XbNetThread, XbNetThreadC));
u32 XbNetThread(void *context);

/* ---- net thread ------------------------------------------------------------------------------ */
static u32 g_sock = 0xFFFFFFFFu;
static u32 g_fail_run;
static struct {
	u32 ip, last_ms, want;
	u16 port;
	u32 seq[2];
} g_peer;

static void put_hdr(u8 *b, u32 type, u32 stream, u32 ch, u32 rate, u32 seq, u32 ts, u32 frames)
{
	put_be32(b, W_MAGIC);
	b[4] = W_VERSION;
	b[5] = (u8)type;
	b[6] = (u8)stream;
	b[7] = (u8)ch;
	put_be32(b + 8, rate);
	put_be32(b + 12, seq);
	put_be32(b + 16, ts);
	b[20] = (u8)(frames >> 8);
	b[21] = (u8)frames;
	b[22] = b[23] = 0;
}

static void send_buf(int len)
{
	u32 err = xn_send_to(g_sock, g_peer.ip, g_peer.port, g_tx, len);

	if (err) {
		g_xb.net_err = err;
		g_fail_run++;
	} else {
		g_fail_run = 0;
	}
}

/* the STATUS answer: what the console wants for streams 3 and 4, and its counters */
static void send_status(void)
{
	u32 w[28], n = 0;
	u32 er = g_ear_rate, mr = g_mic_rate;

	w[n++] = g_xb.enable;
	w[n++] = xb_rate_ok(er) ? er : 48000; /* stream 3: the earpiece's rate, stereo */
	w[n++] = 2;
	w[n++] = xb_rate_ok(mr) ? mr : 24000; /* stream 4: the mic's rate, mono */
	w[n++] = 1;
	w[n++] = (u32)xa_now_ms() - g_ear_ms < 500 ? 1 : 0; /* a headset is streaming */
	for (int k = 2; k < 4; k++) {
		const struct xb_stream_status *s = &g_xb.s[k];

		w[n++] = s->packets;
		w[n++] = s->lost;
		w[n++] = s->late;
		w[n++] = s->underruns;
		w[n++] = s->restarts;
		w[n++] = s->fill_ms;
		w[n++] = s->trim_ppm;
	}
	for (int k = 0; k < 2; k++) {
		w[n++] = g_xb.s[k].packets;
		w[n++] = g_xb.s[k].lost;
	}
	w[n++] = g_xb.pass_us_max;
	w[n++] = g_xb.mic_us_max;
	w[n++] = g_xb.ear_us_max;
	w[n++] = g_xb.net_us_max;
	put_hdr(g_tx, W_STATUS, 0, 0, 0, 0, 0, 0);
	for (u32 i = 0; i < n; i++)
		put_be32(g_tx + W_HDR + 4 * i, w[i]);
	send_buf(W_HDR + 4 * (int)n);
}

static void session_end(void)
{
	g_live = 0;
	g_peer.ip = 0;
	xb_jb_end(&g_jb[0]);
	xb_jb_end(&g_jb[1]);
	g_xb.peer_ip = 0;
	g_xb.peer_port = 0;
	g_xb.state = XS_LISTENING;
}

static void net_close(void)
{
	if (g_sock != 0xFFFFFFFFu) {
		xn_close(g_sock);
		g_sock = 0xFFFFFFFFu;
	}
	xn_cleanup_as(XN_USER_BRIDGE);
	if (g_peer.ip)
		session_end();
}

static void net_open(void)
{
	u32 xnet = 0, wsa = 0, step = 0x80, err;

	g_xb.opens++;
	err = xn_startup_as(XN_USER_BRIDGE, XN_START_XNET | XN_START_WSA, &xnet, &wsa);
	if (!err)
		err = xn_open_udp(W_PORT, XN_OPT_INSECURE | XN_OPT_REUSEADDR, 0x10000, &g_sock, &step);
	if (err) {
		xn_cleanup_as(XN_USER_BRIDGE);
		g_xb.net_err = err;
		g_xb.net_step = step;
		g_xb.state = XS_FAILED;
		return;
	}
	g_fail_run = 0;
	g_xb.state = XS_LISTENING;
}

static void hello(const u8 *b, int len, u32 ip, u16 port, u32 now)
{
	u32 want = len >= W_HDR + 4 ? be32(b + W_HDR) & 3 : 3;

	g_xb.hellos++;
	/* one PC at a time: another is answered only once this one has gone quiet (the same
	 * machine on a new port, a restarted helper, takes over at once) */
	if (g_peer.ip && ip != g_peer.ip && now - g_peer.last_ms <= PEER_TIMEOUT_MS) {
		g_xb.rx_other++;
		return;
	}
	if (ip != g_peer.ip || port != g_peer.port) {
		g_xb.sessions++;
		g_peer.seq[0] = g_peer.seq[1] = 0;
		xb_txr_reset(&g_txr[0]); /* nothing from before this session */
		xb_txr_reset(&g_txr[1]);
		xb_jb_end(&g_jb[0]);
		xb_jb_end(&g_jb[1]);
	}
	g_peer.ip = ip;
	g_peer.port = port;
	g_peer.last_ms = now;
	g_peer.want = want;
	g_live = want;
	g_xb.peer_ip = ip;
	g_xb.peer_port = port;
	g_xb.state = XS_CONNECTED;
	send_status();
}

static void audio_in(const u8 *b, int len, u32 now)
{
	u32 stream = b[6], ch = b[7], rate = be32(b + 8), seq = be32(b + 12), ts = be32(b + 16);
	u32 frames = (u32)b[20] << 8 | b[21];

	if ((stream != 3 && stream != 4) || ch < 1 || ch > 2 || !xb_rate_ok(rate) || !frames ||
	    frames * ch * 2 > RX_MAX_BYTES || len != (int)(W_HDR + frames * ch * 2)) {
		g_xb.rx_bad++;
		return;
	}
	xb_jb_put(&g_jb[stream - 3], seq, ts, rate, ch, b + W_HDR, frames, now);
}

static void rx_drain(u32 now)
{
	for (int i = 0; i < RX_PER_PASS; i++) {
		int len = 0;
		u32 ip = 0;
		u16 port = 0;
		u32 err = xn_recv(g_sock, g_rx, PKT_BUF, &len, &ip, &port);

		if (err == XN_WOULDBLOCK)
			return;
		if (err) {
			g_xb.net_err = err;
			g_fail_run++;
			return;
		}
		if (len < W_HDR || be32(g_rx) != W_MAGIC || g_rx[4] != W_VERSION) {
			g_xb.rx_bad++;
			continue;
		}
		if (g_rx[5] == W_HELLO) {
			hello(g_rx, len, ip, port, now);
		} else if (!g_peer.ip || ip != g_peer.ip) {
			g_xb.rx_other++; /* audio or a bye from a PC that isn't the one connected */
		} else if (g_rx[5] == W_AUDIO) {
			audio_in(g_rx, len, now);
		} else if (g_rx[5] == W_BYE) {
			session_end();
		} else {
			g_xb.rx_bad++;
		}
	}
}

/* streams 1 and 2: what the driver thread queued, in packets of up to 600 frames */
static void tx_streams(void)
{
	for (u32 k = 0; k < 2; k++) {
		struct xb_txr *t = &g_txr[k];
		u32 rate = t->rate;

		if (!(g_peer.want & (1u << k)) || !xb_rate_ok(rate))
			continue;
		for (int p = 0; p < TX_PER_PASS; p++) {
			u32 ts, n;

			/* at least 5 ms a packet: a pass usually has a frame's worth (~17 ms) */
			if (t->w - t->r < rate / 200)
				break;
			n = xb_txr_take(t, g_tx + W_HDR, TX_MAX_FRAMES, &ts);
			if (!n)
				break;
			put_hdr(g_tx, W_AUDIO, k + 1, 1, rate, g_peer.seq[k]++, ts, n);
			send_buf(W_HDR + 2 * (int)n);
			g_xb.s[k].packets++;
		}
	}
}

BODY u32 XbNetThreadC(void *context)
{
	u32 next_retry = 0, failed = 0;

	(void)context;
	{
		u8 *kt = current_kthread();

		KeSetBasePriorityThread(kt, (u32)(NET_PRIORITY - (s32)kt[KTHREAD_PRIORITY_BASE]));
	}
	for (;;) {
		u32 now = (u32)xa_now_ms(), t0 = xd_timebase(), us;
		bool want = g_xb.enable && !g_quiesced;

		g_xb.heartbeat++;
		if (g_sock != 0xFFFFFFFFu && !want) {
			net_close();
			g_xb.state = g_quiesced ? XS_QUIESCED : XS_OFF;
		}
		if (!want && g_sock == 0xFFFFFFFFu)
			g_xb.state = g_quiesced ? XS_QUIESCED : XS_OFF;
		if (want && g_sock == 0xFFFFFFFFu && (!failed || now - next_retry < 0x80000000u)) {
			net_open();
			failed = g_sock == 0xFFFFFFFFu;
			next_retry = now + RETRY_MS;
		}
		if (g_sock != 0xFFFFFFFFu && g_fail_run >= REOPEN_AFTER_FAILS) {
			net_close(); /* errors on every call: start the socket again */
			net_open();
			failed = g_sock == 0xFFFFFFFFu;
			next_retry = now + RETRY_MS;
		}
		if (g_sock != 0xFFFFFFFFu) {
			rx_drain(now);
			if (g_peer.ip && now - g_peer.last_ms > PEER_TIMEOUT_MS)
				session_end(); /* the PC went quiet */
			if (g_peer.ip) {
				tx_streams();
				g_xb.peer_age_ms = now - g_peer.last_ms;
			}
		}
		g_parked = g_quiesced && g_sock == 0xFFFFFFFFu;
		us = (xd_timebase() - t0) / TB_PER_US;
		if (us > g_xb.net_us_max)
			g_xb.net_us_max = us;
		xa_sleep_ms(g_sock != 0xFFFFFFFFu ? POLL_MS : IDLE_MS);
	}
	return 0;
}

/* ---- driver thread ------------------------------------------------------------------------ */
static u32 g_pass_tb, g_sec_tb, g_sec_passes;
static u64 g_sec_ms, g_pub_ms;
static u32 g_gain_hs = 256, g_gain_chat = 256; /* the gains of the last block (a change ramps) */
static s32 *g_chat;               /* stream 4 read ahead for the mic chunks (XB_MAX_BLOCK) */
static u32 g_chat_n, g_chat_pos, g_chat_any, g_chat_rate;
static u32 g_logged_state = 0xFFFFFFFFu, g_logged_ip, g_logged_err;

void xb_init(void)
{
	u32 jb3 = JB_FRAMES * 2, jb4 = JB_FRAMES, tx = TX_FRAMES;
	u32 bytes = 2 * (jb3 + jb4 + 2 * tx) + 4 * 3 * XB_MAX_BLOCK + 2 * PKT_BUF;
	u8 *m = malloc(bytes);

	g_start_at = xa_now_ms() + START_DELAY_MS;
	if (!m) {
		g_xb.state = XS_REFUSED;
		xa_log("bridge: no memory (%u bytes): the PC bridge stays off\n", bytes);
		return;
	}
	memset(m, 0, bytes);
	xb_jb_init(&g_jb[0], (s16 *)m, JB_FRAMES, 2);
	m += 2 * jb3;
	xb_jb_init(&g_jb[1], (s16 *)m, JB_FRAMES, 1);
	m += 2 * jb4;
	xb_txr_init(&g_txr[0], (s16 *)m, TX_FRAMES);
	m += 2 * tx;
	xb_txr_init(&g_txr[1], (s16 *)m, TX_FRAMES);
	m += 2 * tx;
	g_acc = (s32 *)m;
	m += 4 * 2 * XB_MAX_BLOCK;
	g_chat = (s32 *)m;
	m += 4 * XB_MAX_BLOCK;
	g_rx = m;
	g_tx = m + PKT_BUF;
	g_ready = 1;
	xa_log("bridge: ready (%u bytes); listening for a PC on UDP %u from %u s on\n", bytes, W_PORT,
	       START_DELAY_MS / 1000);
}

static void start_thread(void)
{
	HANDLE thread = 0;
	u32 tid = 0;
	int err = xn_init();

	g_thread = 1;
	if (err) {
		g_xb.state = XS_REFUSED;
		g_xb.net_err = xn_init_detail();
		xa_log("bridge: network exports refused (%d, %08x): the PC bridge stays off\n", err,
		       xn_init_detail());
		return;
	}
	ExCreateThread(&thread, NET_STACK, &tid, 0, (void *)XbNetThread, 0,
		       0x2 | (1u << NET_HW_THREAD) << 24);
	xa_log("bridge: net thread %s on hardware thread %u\n", thread ? "started" : "NOT created",
	       NET_HW_THREAD);
	if (!thread)
		g_xb.state = XS_REFUSED;
}

static u32 level_q8(u32 pct)
{
	return (pct > 100 ? 100 : pct) * 256 / 100;
}

static void publish(void)
{
	for (int k = 0; k < 2; k++) {
		struct xb_jb *j = &g_jb[k];
		struct xb_stream_status *s = &g_xb.s[2 + k];
		struct xb_txr *t = &g_txr[k];
		struct xb_stream_status *o = &g_xb.s[k];

		s->packets = j->packets;
		s->lost = j->lost;
		s->late = j->late | j->recovered << 16;
		s->underruns = j->underruns;
		s->restarts = j->restarts;
		s->fill_ms = j->state == XB_RUNNING ? j->fill_ms : 0;
		s->trim_ppm = (u32)j->ppm;
		s->rate = j->rate;
		s->ch = j->in_ch;
		o->lost = t->skipped;
		o->rate = t->rate;
		o->ch = 1;
		o->fill_ms = xb_rate_ok(t->rate) ? (t->w - t->r) / (t->rate / 1000) : 0;
	}
}

void xb_poll(void)
{
	u64 now = xa_now_ms();
	u32 us = g_pass_tb / TB_PER_US;

	g_now = (u32)now;
	/* the added work in this pass's audio calls */
	g_pass_tb = 0;
	g_xb.pass_us = us;
	if (us > g_xb.pass_us_max)
		g_xb.pass_us_max = us;
	g_sec_tb += us;
	g_sec_passes++;
	if (g_xb.zero) {
		g_xb.pass_us_max = g_xb.mic_us_max = g_xb.ear_us_max = g_xb.net_us_max = 0;
		g_xb.zero = 0;
	}
	if (now - g_sec_ms >= 1000) {
		g_xb.pass_us_avg = g_sec_passes ? g_sec_tb / g_sec_passes : 0;
		g_sec_tb = g_sec_passes = 0;
		g_sec_ms = now;
		for (int k = 0; k < 2; k++) {
			g_xb.s[2 + k].peak = g_jb[k].peak;
			g_jb[k].peak = 0;
			g_xb.s[k].peak = g_txr[k].peak;
			g_txr[k].peak = 0;
		}
	}
	if (g_xb.jb_ms < JB_MS_MIN || g_xb.jb_ms > JB_MS_MAX)
		g_xb.jb_ms = JB_MS_DEFAULT;
	if (g_ready && !g_thread && !g_quiesced && g_xb.enable && now >= g_start_at)
		start_thread();
	if (g_ready && now - g_pub_ms >= PUBLISH_MS) {
		g_pub_ms = now;
		publish();
	}
	/* what changed, from the net thread's words */
	if (g_xb.state != g_logged_state || g_xb.peer_ip != g_logged_ip) {
		u32 ip = g_xb.peer_ip, st = g_xb.state;

		if (st == XS_CONNECTED && ip)
			xa_log("bridge: PC %u.%u.%u.%u:%u connected (session %u)\n", ip >> 24,
			       (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, g_xb.peer_port, g_xb.sessions);
		else if (st == XS_LISTENING)
			xa_log("bridge: listening on UDP %u%s\n", W_PORT,
			       g_logged_state == XS_CONNECTED ? " (the PC left)" : "");
		else if (st == XS_FAILED && g_xb.net_err != g_logged_err)
			xa_log("bridge: socket failed: error %u at step %u; retrying every %u s\n",
			       g_xb.net_err, g_xb.net_step, RETRY_MS / 1000);
		else if (st == XS_OFF)
			xa_log("bridge: off (bridge.py on)\n");
		g_logged_state = st;
		g_logged_ip = ip;
		g_logged_err = g_xb.net_err;
	}
}

void xb_quiesce(void)
{
	u64 until;

	g_quiesced = 1;
	g_live = 0;
	if (!g_thread) {
		g_xb.state = XS_QUIESCED;
		return;
	}
	until = xa_now_ms() + QUIESCE_WAIT_MS;
	while (!g_parked && xa_now_ms() < until)
		xa_sleep_ms(5);
	xa_log("bridge: quiesced (%s)\n", g_parked ? "socket closed" : "TIMED OUT");
}

void xb_resume(void)
{
	g_quiesced = 0;
}

static void spent(u32 t0, u32 *max_us)
{
	u32 dt = tb() - t0, us = dt / TB_PER_US;

	g_pass_tb += dt;
	if (us > *max_us)
		*max_us = us;
}

bool xb_mic_wanted(void)
{
	return (g_live & 1) != 0;
}

void xb_mic(int pad, const u8 *mono, int frames, u32 rate)
{
	u32 t0;

	(void)pad;
	g_mic_rate = rate;
	if (!(g_live & 1) || frames <= 0 || !g_ready)
		return;
	t0 = tb();
	xb_txr_push(&g_txr[0], mono, (u32)frames, 2, rate);
	spent(t0, &g_xb.mic_us_max);
}

void xb_players(int pad, const u8 *out, int len, u32 rate, u32 ch)
{
	u32 t0;

	(void)pad;
	if (!(g_live & 2) || len <= 0 || ch < 1 || ch > 2 || !g_ready)
		return;
	t0 = tb();
	xb_txr_push(&g_txr[1], out, (u32)len / (2 * ch), 2 * ch, rate); /* the left channel */
	spent(t0, &g_xb.ear_us_max);
}

/* one stream from the PC into a block: read in pieces of at most XB_MAX_BLOCK frames, mixed at
 * a gain that ramps from the last block's (a level change is never a click) */
static void mix_from(struct xb_jb *j, u8 *out, u32 frames, u32 rate, u32 ch, u32 *gain_last,
		     u32 gain, u32 *max_us)
{
	u32 t0 = tb(), done = 0, g0 = *gain_last;

	if (!g_ready || ch < 1 || ch > 2 || !xb_rate_ok(rate))
		return;
	if (j->state == XB_IDLE && !j->fadeout && j->gen == j->rgen) {
		*gain_last = gain;
		return; /* nothing from the PC: no cost but this test */
	}
	while (done < frames) {
		u32 n = frames - done < XB_MAX_BLOCK ? frames - done : XB_MAX_BLOCK;
		s32 ga = (s32)(g0 + ((s32)gain - (s32)g0) * (s32)done / (s32)frames);
		s32 gb = (s32)(g0 + ((s32)gain - (s32)g0) * (s32)(done + n) / (s32)frames);

		if (xb_jb_read(j, g_acc, n, rate, g_xb.jb_ms, g_now))
			xb_mix_into(out + 2 * ch * done, n, ch, g_acc, j->ch, ga, gb);
		done += n;
	}
	*gain_last = gain;
	spent(t0, max_us);
}

void xb_mix_headset(int pad, u8 *out, int len, u32 rate, u32 ch, u32 master_q8)
{
	u32 gain;

	(void)pad;
	g_ear_rate = rate;
	g_ear_ms = g_now;
	g_xb.ear_calls++;
	if (len <= 0 || ch < 1 || ch > 2)
		return;
	gain = (master_q8 > 256 ? 256 : master_q8) * level_q8(g_xb.level_headset) >> 8;
	mix_from(&g_jb[0], out, (u32)len / (2 * ch), rate, ch, &g_gain_hs, gain, &g_xb.ear_us_max);
}

/* Stream 4 goes into mic chunks of 1 ms (24 frames), 17 a pass: it is read from its jitter
 * buffer in pieces of 4 ms kept in g_chat, so the reader's fixed cost is paid a quarter as
 * often (4 ms more delay to the Xbox players) */
#define CHAT_PIECE_DIV 250

void xb_mix_chat(int pad, u8 *mono, int frames, u32 rate)
{
	struct xb_jb *j = &g_jb[1];
	u32 t0, done = 0, g0 = g_gain_chat, gain = level_q8(g_xb.level_chat);

	(void)pad;
	g_mic_rate = rate;
	g_xb.mic_calls++;
	if (frames <= 0 || !g_ready || !xb_rate_ok(rate))
		return;
	if (j->state == XB_IDLE && !j->fadeout && j->gen == j->rgen && g_chat_pos >= g_chat_n) {
		g_gain_chat = gain;
		return; /* nothing from the PC: no cost but this test */
	}
	t0 = tb();
	if (rate != g_chat_rate) {
		g_chat_rate = rate;
		g_chat_pos = g_chat_n = 0; /* a new mic format: what was read is for the old one */
	}
	while (done < (u32)frames) {
		u32 m;

		if (g_chat_pos >= g_chat_n) {
			g_chat_n = rate / CHAT_PIECE_DIV;
			g_chat_any = xb_jb_read(j, g_chat, g_chat_n, rate, g_xb.jb_ms, g_now);
			g_chat_pos = 0;
		}
		m = (u32)frames - done < g_chat_n - g_chat_pos ? (u32)frames - done
							       : g_chat_n - g_chat_pos;
		if (g_chat_any) {
			s32 ga = (s32)g0 + ((s32)gain - (s32)g0) * (s32)done / frames;
			s32 gb = (s32)g0 + ((s32)gain - (s32)g0) * (s32)(done + m) / frames;

			xb_mix_into(mono + 2 * done, m, 1, g_chat + g_chat_pos, 1, ga, gb);
		}
		done += m;
		g_chat_pos += m;
	}
	g_gain_chat = gain;
	spent(t0, &g_xb.mic_us_max);
}
