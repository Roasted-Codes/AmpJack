// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * What the wired driver's files share: xw_plugin.c (the class driver, the transfers, headset
 * audio, the driver thread), reload_360.c (hot reload, both sides) and selftest_360.c (the
 * audio self-test, the mic dump, usbreset / hsrestart / mecreset). The last two are
 * development machinery, compiled only into XW_DEV builds; a release build gets the stubs at
 * the end of this file instead, each of which does what the real one does when nothing asks
 * for it (no reload requested, no self-test stage, no xbdm command).
 */
#ifndef XW_PRIV_H
#define XW_PRIV_H

#include <xecore/xboxkrnl.h>

#include "../core/gip.h"
#include "../core/platform.h"
#include "handover.h"
#include "kernel_360.h"
#include "platform_360.h"

#ifndef XW_SLOT
#define XW_SLOT 0 /* build.sh XW_SLOT=n: xw<n>.xex at its own base, for a hot reload */
#endif

#define PORTS 4
#define PKT_MAX 64     /* GIP packets on a cable are at most 64 bytes */
/* outstanding IN transfers per controller: one, as hiddriver360 keeps on its interrupt
 * endpoints. GIP pads are full-speed devices (the 360's OHCI companion controllers, not EHCI
 * like the wireless adapter); with two queued on one endpoint the console froze. */
#define IN_COUNT 1
#define RX_SLOTS 64    /* received packets queued per controller (a power of two) */
#define OUT_SLOTS 32   /* packets waiting to be sent per controller (a power of two) */

static __attribute__((unused)) u8 *current_kthread(void)
{
	u32 pcr;

	__asm__ volatile("mr %0, 13" : "=r"(pcr));
	return *(u8 **)(pcr + 0x100);
}

struct xw_port;

/*
 * A transfer block. Usbd owns more of it than struct usbd_transfer declares: the word at
 * 0x1C (the "reserved" bytes before a control transfer's setup packet) is written by
 * Usbd, and the first build kept its port pointer there; the IN completion then followed
 * a clobbered pointer on Usbd's DPC and froze the console. So every block, as xa_xfer
 * does, is a full control-transfer block (0x28 bytes) with nothing of ours inside it, and
 * the completions find their port from the block's address (port_of), never from it.
 */
struct xw_xfer {
	struct usbd_control_transfer c;
	u32 spare[4];      /* margin in case Usbd uses more of a block on OHCI */
	volatile int busy; /* queued on Usbd, not completed yet */
};

/* the default endpoint's block: the same, and the same rule */
struct xw_ctrl {
	struct usbd_control_transfer c;
	u32 spare[4];
};

struct xw_port {
	/* set on Usbd's DPC (AddDevice/RemoveDevice), read by the thread */
	void *volatile claimed; /* the device we took at AddDevice, usable or not */
	void *volatile node;    /* the same once its endpoints are open: 0 = not usable */
	volatile u32 session; /* bumped on each AddDevice: a new controller in this port */
	volatile int gone;
	u16 vid, pid;
	u8 ep_in, ep_out;
	u16 max_in, max_out;
	u8 interval_in, interval_out;
	long open_status[3]; /* in, out, default (control) */
	/* interface 1 alternate 1 (headset audio), from the configuration descriptor at
	 * AddDevice; aud_desc: 1 = read from it, 2 = (partly) the known 0x83/0x03 defaults */
	u8 aud_ep_in, aud_ep_out, aud_desc;
	u16 aud_max_in, aud_max_out;
	u32 gep_in, gep_out; /* what UsbdGetEndpointDescriptor(type 1) returned (diagnostic) */

	struct xw_xfer in[IN_COUNT];
	struct xw_xfer out;
	struct xw_ctrl ctrl;   /* default endpoint: SET_CONFIGURATION, SET_INTERFACE */
	u32 setup_step;
	struct xw_xfer close[3];
	u32 close_step;
	void *removing;

	/* received packets: written on the DPC, read by the thread */
	u8 rx[RX_SLOTS][PKT_MAX];
	u8 rx_len[RX_SLOTS];
	volatile u32 rx_write, rx_read;
	volatile u32 rx_dropped, rx_errors, rx_count;

	/* packets to send: appended by the thread, sent one at a time from the DPC */
	u8 tx[OUT_SLOTS][128] __attribute__((aligned(128))); /* DMA: a cache line each */
	u8 tx_len[OUT_SLOTS];
	volatile u32 tx_head, tx_tail;
	volatile int tx_inflight;
	volatile u32 tx_dropped, tx_errors, tx_count;

	/* the thread's */
	u32 seen_session;
	struct gip_client *gc;
	int pad; /* the xa_xinput slot, or -1 */
	int in_started;
	int fb_mark;  /* the start fallback: 1 = seen (counting IN from fb_rx), 2 = reported */
	u32 fb_rx;
	u64 fb_time;

	/* hot reload (handover.h). DPC state: */
	volatile int handed;  /* released: the device is on offer in g_ho.ports[] (or adopted by
			       * now); nothing is queued on it by this build any more */
	volatile int in_wait; /* adopted while an earlier build's IN transfer is still queued:
			       * ours is only queued once that one has completed (XwInForwardC) */
	struct xw_ho_port *in_src; /* with in_wait: the entry of the build that queued it */
	u32 adopt_session;    /* the session that began with an adoption, not an AddDevice */
	u32 ho_flags;         /* HO_PF_*: adopted with them / what a refused release puts back */
	u32 ho_hs_fmt;        /* xw_ho_port.hs_fmt: handed over / adopted with it */
	u32 trace_first;      /* adopted: 1 = trace our first IN queue, 2 = our first OUT */
	/* the thread's */
	u64 probe_at;         /* adopted: when to ask for the headset (0 none, 1 once ready) */
	int adopt_pending;    /* adopted: gip_adopt still to send (after IN is started, T6) */
};

#define AUD_FRAMES 8         /* one 8 ms block per transfer (xone), one OHCI iTD */
#define AUD_OUT_BLOCKS 5     /* OUT blocks kept on Usbd: 40 ms queued. Fixed: with 5 the
				* earpiece stream had no gaps on the console (3 left ~5 gaps/s
				* from driver-thread latency, 2 left ~60/s). 3 IN + 5 OUT + 2
				* endpoint tails = 10 of the kernel's 12 iTDs */
#define AUD_BLOCKS 3         /* IN blocks in rotation */
#define AUD_TB_HZ 50000000u  /* the timebase (mftb): 50 MHz, checked by XWDG tb_per_ms */
#define AUD_BUF 0x1000       /* one block per page: one iTD page, PSW offsets < 0x1000 */
#define AUD_RX_SLOTS 64      /* received audio packets queued for the thread (power of two) */
#define AUD_FRAME_MAX 256    /* the largest audio packet kept (48 kHz stereo is 198 bytes) */
#define AUD_DEF_IN 0x83      /* the pads' interface 1 alt 1 (desktop/xwired, xone) */
#define AUD_DEF_OUT 0x03
#define AUD_DEF_MAX_IN 64
#define AUD_DEF_MAX_OUT 228  /* guess (not read from a descriptor) */
#define AUD_STAGE_MAX 5
#define AUD_VOLUME_MS 50     /* volume messages to the headset (its sidetone): at most 20/s */

struct xw_iso {
	struct usbd_isoch_transfer t;
	u32 spare[8];        /* margin, as for the async blocks */
	volatile int busy;   /* OUT: owned by a pending send (thread sets, completion clears) */
	volatile int queued; /* on Usbd (set and cleared on its DPC) */
	volatile int staged; /* never set since WP1.4 (the DPC refill is gone); the hot-reload
			      * checks (commit_check, aud_drain) still read it */
	u32 gen;             /* g_audio.open_gen when it was queued */
};

struct xw_audio {
	struct xw_port *volatile owner; /* the port with audio enabled, NULL = none */
	u32 stage;                      /* g_xw.audio.stage when it was enabled */
	u8 ep_in, ep_out;
	u16 max_in, max_out;

	/* DPC state; the thread reads it */
	volatile u32 opened;   /* bit 0 IN, bit 1 OUT endpoint open */
	volatile int streaming; /* transfers may be queued (the thread sets/clears it) */
	volatile int closing;   /* the endpoints are being closed */
	volatile int close_waiting; /* closing waits for queued blocks to complete */
	volatile int force;     /* close with blocks still queued (watchdog) */
	struct xw_port *close_then_remove; /* RemoveDevice waits for the audio close */
	/* the endpoints as opened (DPC): closed against these, whatever owner says by then */
	struct xw_port *volatile open_port;
	void *open_node;
	volatile u32 open_gen;  /* bumped by each successful open; blocks carry it */
	u32 trace_first;        /* trace: 1 = the first iso IN completion after an open, 2 = OUT */
	/* thread -> DPC calls: each is posted with call_gen as its argument and runs only if
	 * that is still current (a call the thread gave up on is invalidated, never run late) */
	volatile u32 call_gen;
	struct xw_port *volatile call_port;
	u32 enable_gen;         /* the thread's: bumped by each enable */
	volatile int broken;    /* a control transfer or close never completed */
	volatile int recorded, recorded_bytes; /* first completion captured */
	u16 alt;                /* SET_INTERFACE value for aud_alt_dpc */
	volatile u32 call_done;
	volatile long call_status;
	struct xw_iso in[AUD_BLOCKS], out[AUD_OUT_BLOCKS];
	struct xw_xfer close[2];

	/* received packets: completion writes, thread reads */
	u8 rx[AUD_RX_SLOTS][AUD_FRAME_MAX];
	u16 rx_len[AUD_RX_SLOTS];
	volatile u32 rx_write, rx_read;

	/* the thread's */
	int hs_up;              /* gip reported the headset ready */
	int announced, announced_pad;
	u32 ann_rate_in, ann_ch_in, ann_rate_out, ann_ch_out;
	u64 next_block, next_rate;
	u32 rate_tb;            /* the timebase at the start of the mic_pps / ear_bps window */
	u32 last_mic, last_ear;
	u32 tone_phase;
	u32 dump_want;          /* xw.py dump: bytes to record (dump_rate x channels x 1.5 s) */
};

/* ---- status block, read over xbdm (magic "XW36") ------------------------------------------ */

enum xw_state {
	XW_STARTED = 1, XW_RESOLVE_FAILED, XW_CODE_MISMATCH, XW_WAITING, XW_RUNNING,
	XW_RELEASED, /* hot reload: handed over to the next build (g_ho) */
	XW_IDLE,     /* a slot build that couldn't take over: it hooks and claims nothing */
};

struct xw_pad_status {
	u32 connected;
	u16 vid, pid;
	u32 rx, tx, rx_dropped, rx_errors, tx_dropped, tx_errors;
	u32 first_length_after; /* what Usbd left in an IN block's length field (diagnostic) */
	u16 buttons;
	u8 guide, reserved;
	u16 trigger_left, trigger_right;
	s16 stick_left_x, stick_left_y, stick_right_x, stick_right_y;
};

/*
 * Headset audio, USB reset and the self-test. The first five words are written by xbdm
 * (xw.py audio / tone / usbreset / micdump), and dump; the rest is read-only. Order and types
 * must match AUDIO_FIELDS in xw.py. `version` (AUD_STATUS_VERSION) is the layout: 3 since
 * WP1.4 (85 words, as before: the removed clock-fix and game-dump words became version,
 * mic_skips, dump_rate, dump_channels and reserved, so what follows the block stays put).
 */
#define AUD_STATUS_VERSION 3
struct xw_audio_status {
	u32 magic;              /* "XWAU" */
	volatile u32 stage;     /* xbdm: 0 refuse (H0), 1 open, 2 one IN block, 3 IN, 4 OUT, 5 test */
	volatile u32 tone_hz;   /* xbdm: stage 5's tone */
	volatile u32 usbreset;  /* xbdm: port + 1 = re-enumerate that controller (thread clears) */
	volatile u32 micdump;   /* xbdm: 1 = record ~1 s of mic PCM; 2 recording, 3 done */
	u32 micdump_addr;       /* struct xw_micdump */
	u32 active_stage;       /* the stage latched when audio was enabled */
	u32 port;               /* the port whose headset has audio, 0xFFFFFFFF = none */
	u32 flags;              /* AUD_F_* */
	u32 enables, refusals, disables;
	u32 alt_value;          /* the last SET_INTERFACE(1, n) */
	u32 alt_status;
	u32 ep_in, ep_out, max_in, max_out, desc_source;
	u32 gep_in, gep_out;
	u32 opens, open_in, open_out, closes, close_status, watchdog;
	u32 in_queued, in_done, in_frames_ok, in_frames_empty, in_frames_missed, in_frames_err;
	u32 in_last_err, in_bytes, in_dropped, in_cancelled;
	u32 out_queued, out_done, out_frames_err, out_last_err, out_dropped, out_refused;
	u32 out_inflight;
	u32 mic_packets, mic_bytes, mic_pps, ear_blocks, ear_bps;
	/* the first IN completion after enabling: per-frame status and length, and the first
	 * bytes of the first frame that carried data */
	u32 first_status[8];
	u16 first_len[8];
	u8 first_bytes[16];
	/* mic analysis, every 100 ms window: the tone's amplitude (sample units), the same at
	 * an off-tone frequency, RMS, peak */
	u32 tone_amp, off_amp, off_hz, mic_rms, mic_peak, windows;
	u32 resets;             /* UsbdResetDevice calls (usbreset) */
	u32 fallback_rx[PORTS]; /* IN packets since the start fallback powered the pad on;
				 * 0xFFFFFFFF = no fallback start */
	u32 version;            /* AUD_STATUS_VERSION (this word was ear_fb, 0 by default) */
	/* 1 ms frames the streaming mic skipped (an IN frame with no packet, 1 or 2 in a block:
	 * ~5/s on the SteelSeries, its clock is slower than USB's). Counted only */
	u32 mic_skips;
	/* xw.py dump: writing 1 records ~1.5 s of the final earpiece PCM (s16le, dump_rate x
	 * dump_channels), allocated at the first dump; the thread sets 2 while recording, 3 done */
	volatile u32 dump;
	u32 dump_ear_addr, dump_ear_bytes, dump_rate, dump_channels;
	u32 release;            /* 1: a release build (no XW_DEV): xw.py refuses the development
				 * tools (reload, micdump, usbreset, ...) instead of hanging */
	u32 reserved[1];
};

enum {
	AUD_F_KERNEL = 1,    /* isochronous code checked */
	AUD_F_IN_OPEN = 2,
	AUD_F_OUT_OPEN = 4,
	AUD_F_STREAMING = 8,
	AUD_F_ANNOUNCED = 16, /* voice_360 told: headset present */
	AUD_F_BROKEN = 32,    /* a control/close never completed: audio stays off */
	AUD_F_HS_UP = 64,     /* gip: headset ready */
};

struct xw_status {
	u32 magic;
	u32 version;
	volatile u32 state;
	s32 last_error;
	u32 log_addr;
	u32 match_calls, adds, removes, declined;
	u32 heartbeat;
	volatile u32 debug; /* xbdm writes the log level (xw.py debug): copied into xa_debug */
	/* start-up gates (XW_STAGED builds): the thread runs step n only once go >= n, and
	 * stage says how far it got, so a hang can be pinned to one step (xw.py go N) */
	volatile u32 go;
	volatile u32 stage;
	/* IN completion diagnostics: count, last status, how far the callback got, and the
	 * first packet's bytes */
	volatile u32 in_done, in_last_status, in_returning, in_returned;
	volatile u8 in_first[16];
	volatile u32 stray;            /* completions for a block outside g_ports (never expected) */
	volatile u32 in_block_after[12]; /* the IN block as Usbd left it at the first completion */
	volatile u32 in_block_now[12];   /* port 0's IN block, refreshed by the thread */
	volatile u32 mute;               /* xbdm writes 1: test mode (xa_xinput_mute) */
	volatile u8 last_input[64];      /* port 0's last raw GIP input packet (command 0x20) */
	struct xw_pad_status pads[PORTS];
	struct xw_audio_status audio; /* after pads, so older xw.py versions still read the rest */
	/* the driver loop: the last pass's period (start to start, sleep included) and the
	 * longest since start; xbdm may write loop_max_us 0 to measure from now */
	u32 loop_us;
	volatile u32 loop_max_us;
	/* version 3: the system window (docs/INTERNALS.md). On hardware threads 2-5 the kernel
	 * lets priorities 0-9 run only in a short window after each vertical refresh (1 ms by
	 * default), so this thread (9, hardware thread 2) has that much CPU a frame. A pass that
	 * is still running when the window closes is stopped until the next one, a whole frame. */
	u32 win_us;              /* the window's length now; 0 = not the 17559 code read offline */
	u32 run_us;              /* the last pass's own time: loop top to its sleep */
	volatile u32 run_max_us; /* the longest since start; xbdm may write 0 to measure from now */
	u32 win_cuts;            /* passes stopped by the window's end (run over WIN_CUT_US) */
	volatile u32 win_boost;  /* xbdm: 1 = a pass nearing the window's end goes on at
				  * WIN_PRIO instead of stopping (default 0) */
	u32 win_boosts;          /* passes that did */
};

/* xw.py reads these at fixed places (HDR_WORDS, PAD_FMT, OFF_AUDIO, AUDIO_FIELDS) */
_Static_assert(__builtin_offsetof(struct xw_status, pads) == 63 * 4, "xw.py HDR_WORDS");
_Static_assert(sizeof(struct xw_pad_status) == 52, "xw.py PAD_FMT");
_Static_assert(__builtin_offsetof(struct xw_status, audio) == 63 * 4 + 4 * 52, "xw.py OFF_AUDIO");
_Static_assert(sizeof(struct xw_audio_status) == 85 * 4, "xw.py AUDIO_FIELDS");
_Static_assert(__builtin_offsetof(struct xw_status, loop_us) == 63 * 4 + 4 * 52 + 85 * 4,
	       "accept/wp0_1.py OFF_LOOP");
_Static_assert(sizeof(struct xw_status) == 63 * 4 + 4 * 52 + 85 * 4 + 8 * 4, "xw.py LOOP_FIELDS");
_Static_assert(__builtin_offsetof(struct usbd_isoch_transfer, buffer) == 0x10 &&
	       __builtin_offsetof(struct usbd_isoch_transfer, length) == 0x14 &&
	       __builtin_offsetof(struct usbd_isoch_transfer, frames) == 0x18 &&
	       __builtin_offsetof(struct usbd_isoch_transfer, frame_length) == 0x1C,
	       "isochronous block layout (usbd.h)");

extern struct xw_status g_xw;
extern struct xw_port g_ports[PORTS];
extern struct xw_audio g_audio;
extern struct usbd_class_driver g_driver;
extern struct xw_handover g_ho;
extern struct xw_trace g_trace;

enum {
	/* the released build: do_release, commit_dpc */
	TR_REL_BEGIN = 0x100, TR_REL_CHECKED, TR_REL_AUDIO_DRAINED, TR_REL_AUDIO_OFF,
	TR_REL_GAME_QUIESCED, TR_REL_PADS_RELEASED, TR_REL_QUIESCED, TR_REL_XAM_DRAINED,
	TR_REL_TX_DRAINED, TR_REL_COMMIT, TR_REL_COMMITTED, TR_REL_XINPUT_QUIESCED, TR_REL_DONE,
	TR_REL_LOAD_NEXT, TR_REL_REFUSED, TR_REL_RESUMED, TR_REL_LOADED,
	TR_COMMIT_BEGIN = 0x180, TR_COMMIT_REFUSED, TR_COMMIT_UNLINKED, TR_COMMIT_DONE,
	/* the new build: takeover_import, XwThreadC, adopt_dpc, takeover_adopt */
	TR_TAKE_WAIT = 0x200, TR_TAKE_CMD, TR_TAKE_IMPORTED, TR_TAKE_HOOKS, TR_TAKE_HOOKED,
	TR_TAKE_LINK, TR_TAKE_LINKED, TR_TAKE_ADOPT, TR_TAKE_ADOPTED, TR_TAKE_RUNNING,
	TR_TAKE_FAILED, TR_TAKE_NOTIFY,
	TR_ADOPT_BEGIN = 0x280, TR_ADOPT_PORT, TR_ADOPT_DONE, TR_ADOPT_SKIP, TR_ADOPT_RESET,
	/* Usbd's DPC, any build (argument: the port) */
	TR_LINK_DPC = 0x300, TR_IN_FWD, TR_IN_FWD_QUEUED, TR_FIRST_IN, TR_FIRST_OUT, TR_IN_DOUBLE,
	TR_REMOVE, TR_REMOVE_OTHER, TR_AUD_OPEN, TR_AUD_OPENED,
	TR_IN_HANDED, /* a released build's last IN transfer completed (argument: the port) */
	/* headset audio, any build (after the freeze on xw2, Nov 22): the iso life cycle */
	TR_AUD_CLOSE, TR_AUD_CLOSED, TR_AUD_FORCE, TR_ISO_IN_FIRST, TR_ISO_OUT_FIRST,
	TR_ISO_STRAY, TR_AUD_ALT, TR_MEC_RESET, TR_MEC_RESUMED,
	/* driver thread, an adopted controller (argument: the port / the pad) */
	TR_PORT_GC = 0x380, TR_PORT_ADOPT_SENT, TR_PAD_BIND, TR_PAD_BOUND,
	/* gates (argument: 'R' or 'T' << 8 | n) */
	TR_GATE_HOLD = 0x400, TR_GATE_OPEN, TR_GATE_TIMEOUT,
};

#ifdef XW_DEV
static __attribute__((unused)) void trace(u32 code)
{
	u32 n = __sync_fetch_and_add((u32 *)&g_ho.trace_n, 1) % XW_TRACE_RING, tb;

	__asm__ __volatile__("mftb %0" : "=r"(tb));
	g_trace.e[n][1] = tb;
	g_trace.e[n][0] = code;
	g_ho.trace = code;
}
#else
/* a release build traces nothing (the ring stays empty; xw.py tee still reads it) */
static __attribute__((unused)) void trace(u32 code)
{
	(void)code;
}
#endif
#define TR_PORT(code, p) ((code) | (u32)((p) - g_ports) << 16)

/* xw_plugin.c */
#define STATUS_CANCELLED 0xC0000120
void queue_in(struct xw_port *p, int i);
u32 aud_live_stage(void);
int aud_queued(void);
void aud_start_in_dpc(void *arg);
void aud_disable(struct xw_port *p, const char *why);
void pad_ready(struct gip_client *gc);
void pad_release(struct xw_port *p);
void ho_title(void);
void wait_ms(unsigned int ms);

#ifdef XW_DEV
/* reload_360.c */
void XwInForwardC(void *node, const u8 *data, u32 len, long status);
/* WP3.1 gates: holds the step that follows until xw.py raises cmd_gate */
void ho_gate(u32 side, u32 n, const char *what);
bool takeover_import(void);
void takeover_adopt(void);
void do_release(u32 cmd);
extern struct xw_handover *g_pred; /* the released build's block, once checked */

/* selftest_360.c */
void aud_tone(u8 *out, u32 bytes, u32 rate, u32 ch);
void aud_analyse(const u8 *s, int len, u32 rate, u32 ch);
void aud_dump(const u8 *s, int len, u32 rate, u32 ch);
void aud_service_commands(void);
#else
/* ---- a release build: no hot reload, no self-test, no xbdm USB commands ---- */

/* every gate open, as cmd_gate 99 (its value at load) makes them in a development build */
static __attribute__((unused)) void ho_gate(u32 side, u32 n, const char *what)
{
	(void)side, (void)n, (void)what;
}

/* not a slot build (those need XW_DEV): start at once, nothing taken over */
static __attribute__((unused)) bool takeover_import(void)
{
	return true;
}

static __attribute__((unused)) void takeover_adopt(void)
{
}

#define g_pred ((struct xw_handover *)0)

/* xbdm asked a release build to hand over: it can't, and keeps running (xw.py reload refuses
 * a release build before it gets here) */
static __attribute__((unused)) void do_release(u32 cmd)
{
	xa_log("reload: release %u asked for, but this is a release build (hot reload needs "
	       "XW_DEV): nothing done\n", cmd);
}

/* audio stage 5 (the self-test's tone) plays silence */
static __attribute__((unused)) void aud_tone(u8 *out, u32 bytes, u32 rate, u32 ch)
{
	(void)rate, (void)ch;
	memset(out, 0, bytes);
}

static __attribute__((unused)) void aud_analyse(const u8 *s, int len, u32 rate, u32 ch)
{
	(void)s, (void)len, (void)rate, (void)ch;
}

static __attribute__((unused)) void aud_dump(const u8 *s, int len, u32 rate, u32 ch)
{
	(void)s, (void)len, (void)rate, (void)ch;
}

/* usbreset, hsrestart, mecreset and micdump: ignored (xw.py refuses them first) */
static __attribute__((unused)) void aud_service_commands(void)
{
}
#endif

#endif
