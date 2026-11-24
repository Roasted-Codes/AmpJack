// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Game audio in the headset (game_360.h; research/AUDIO.md, "Game audio").
 *
 * The tap. The kernel's XAudio render driver (17559: object 0x80195F38) runs one thread
 * (priority 25, hardware thread 4, PASSIVE_LEVEL inside a critical region). Per frame it
 * clears a 6144-byte mix buffer, calls every render client (titles' XAudio2, XAM), each of
 * which adds its frame to the buffer with XAudioSubmitRenderDriverFrame, then calls its
 * "MEC" clients (the echo-cancellation reference: Kinect), and only then hands the buffer
 * to the hardware. A MEC client may copy the finished mix with
 * XAudioCaptureRenderDriverFrame (ordinal 802), which only works from inside its callback.
 * So game_360 registers as a MEC client (ordinal 800, two slots) and copies the mix in its
 * callback: no kernel code is patched, the TV mix is only read.
 *
 * Frame: 256 samples x 6 channels, big-endian float32, channel after channel (FL FR C LFE
 * SL SR, as Xenia reads it). The callback converts it with integer operations on the raw
 * bits (no floating point: build.sh, check_xex.py), downmixes to stereo, and appends it to
 * a single-producer ring; the driver thread drains the ring into the earpiece through a
 * linear resampler whose ratio is trimmed (at most 0.1 %) to keep the ring ~45 ms full:
 * the audio chip's 48 kHz and the USB frame clock are different clocks. The 45 ms is a
 * run-time setting (xg_set_buffer: the diagnostics menu); a change ramps, never jumps.
 *
 * Gates: registered on the driver thread, 20 s after boot, only if the kernel code matches
 * what was read offline; the callback copies nothing unless game audio is on and an
 * earpiece is being fed (g_tap_on), otherwise it returns at once. Game audio defaults on.
 *
 * Surround (surround_360.c): with its gate on (g_xsv_tap) the tap converts the six channels
 * to s16 for the virtualizer and puts its stereo, the downmix or a crossfade into the ring
 * (surround_in / surround_out). With the gate off (the default) the tap is the code above,
 * unchanged. Its demo replaces the six channels (and skips the capture) while it plays.
 */
#ifndef XG_HOST_TEST
#include <xecore/xboxkrnl.h>
#endif

#include "../core/platform.h"
#include <stdlib.h>
#include "diag_360.h"
#include "game_360.h"
#include "surround_360.h"
#include "surround_dsp.h"

#ifndef XG_HOST_TEST
#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#else
#define BARRIER() __sync_synchronize()
#endif
#define HOOK __attribute__((used, noinline))

/* ---- kernel 17559 --------------------------------------------------------------------------- */
#define ORD_XAudioRegisterRenderDriverMECClient 800
#define ORD_XAudioUnregisterRenderDriverMECClient 801
#define ADDR_UNREGISTER_MEC 0x8012E2A8 /* export: loads the driver object, b 0x8012DBA0 */
#define ORD_XAudioCaptureRenderDriverFrame 802
#define ADDR_REGISTER_MEC 0x8012E288 /* export: loads the driver object, b 0x8012D828 */
#define ADDR_CAPTURE 0x8012D5B8      /* export: loads the driver object, b 0x8012CFD0 */
#define MEC_SLOTS 0x801960AC         /* driver + 0x174: two 16-byte MEC client entries */
#define MEC_SLOT_SIZE 16
#define MEC_SLOT_COUNT 2
#define PROCESS_TITLE 1              /* KeGetCurrentProcessType */

struct sig {
	u32 addr, n;
	const u32 *w;
};
/* the code the tap relies on, word for word from the 17559 dump */
static const u32 SIG_REG_EXPORT[7] = { 0x3d608019, 0x7c852378, 0x396ba3d0, 0x7c641b78,
				       0x3d6b0001, 0x386bbb68, 0x4bfff588 };
static const u32 SIG_CAP_EXPORT[7] = { 0x3d608019, 0x7c852378, 0x396ba3d0, 0x7c641b78,
				       0x3d6b0001, 0x386bbb68, 0x4bfffa00 };
/* MEC register: prologue; the slot scan (driver + 0x174, 2 slots); the slot fill */
static const u32 SIG_REG_BODY[4] = { 0x7d8802a6, 0x4bfdf38d, 0x9421ff60, 0x7c7e1b78 };
static const u32 SIG_REG_SCAN[1] = { 0x395e0174 };
static const u32 SIG_REG_COUNT[1] = { 0x2b0b0002 };
static const u32 SIG_REG_FILL[6] = { 0x556a2036, 0x813b0000, 0x390b0018, 0x7d6af214,
				     0x55082036, 0x394b0174 };
/* capture: prologue and handle range check (0x174..0x194); the copy length (0x600 x 4) */
static const u32 SIG_CAP_BODY[13] = { 0x7d8802a6, 0x9181fff8, 0xfbe1fff0, 0x9421ffa0,
				      0x39630174, 0x7cbf2b78, 0x7f045840, 0x41980018,
				      0x39630194, 0x7f045840, 0x4098000c, 0x2b040000,
				      0x409a0020 };
static const u32 SIG_CAP_LEN[1] = { 0x2b090600 };
/* the render thread's MEC call: driver+0x194 = entry; callback(&{entry->arg}) */
static const u32 SIG_LOOP_CUR[1] = { 0x939a0194 };
static const u32 SIG_LOOP_CALL[7] = { 0x817a0194, 0x38610050, 0x814b0004, 0x91410050,
				      0x816b0000, 0x7d6903a6, 0x4e800421 };
/* MEC unregister (hot reload only): the export stub, then the body up to its handle range
 * check (driver + 0x174 .. + 0x194), word for word from the 17559 dump */
static const u32 SIG_UNREG_EXPORT[6] = { 0x3d608019, 0x7c641b78, 0x396ba3d0, 0x3d6b0001,
					 0x386bbb68, 0x4bfff8e4 };
static const u32 SIG_UNREG_BODY[10] = { 0x7d8802a6, 0x4bfdf019, 0x9421ff70, 0x7c7d1b78,
					0x7c9b2378, 0x397d0174, 0x7f1b5840, 0x41980018,
					0x3b3d0194, 0x7f1bc840 };
static const struct sig SIGS[] = {
	{ ADDR_REGISTER_MEC, 7, SIG_REG_EXPORT }, { ADDR_CAPTURE, 7, SIG_CAP_EXPORT },
	{ 0x8012D828, 4, SIG_REG_BODY },          { 0x8012D8E4, 1, SIG_REG_SCAN },
	{ 0x8012D904, 1, SIG_REG_COUNT },         { 0x8012D96C, 6, SIG_REG_FILL },
	{ 0x8012CFD0, 13, SIG_CAP_BODY },         { 0x8012D0A4, 1, SIG_CAP_LEN },
	{ 0x8012E5E8, 1, SIG_LOOP_CUR },          { 0x8012E638, 7, SIG_LOOP_CALL },
};

/* ---- the frame and the ring ------------------------------------------------------------------ */
#define FRAME_SAMPLES 256              /* per channel */
#define FRAME_CHANNELS 6
#define FRAMES_PER_S 188               /* 48000 / 256 = 187.5 */
#define RING 8192                      /* stereo frames at 48 kHz: 170 ms */
#define RATE 48000
#define MS_FRAMES (RATE / 1000)
#define BUFFER_MS_DEFAULT 45           /* the fill the ratio trim steers to (xg_set_buffer) */
#define HIGH_OVER (75 * MS_FRAMES)     /* this far above the target: start over (rebuffer) */
#define HIGH_MAX (150 * MS_FRAMES)     /* ... but never nearer the ring's end than this */
#define TARGET_RAMP_Q8 9216            /* a new target ramps at 36 frames (0.75 ms) a second:
					* x output frames / output rate, in frames Q8 */
#define DRIFT_MAX_PPM 1000
#define FADE 256                       /* frames: fade-in at a start, fade-out at a stop */
#define CONSUMER_IDLE_MS 200           /* no earpiece block this long: the tap stops copying */
#define INSTALL_DELAY_MS 20000
#define RETRY_MS 10000
#define ONE (1u << 20)                 /* resampler phase, Q20 */

/* downmix, Q14: L = a FL + b (C + SL), R = a FR + b (C + SR), b = 0.707 a, and
 * a (1 + 2 x 0.707) = 1 so a full-scale input can't clip; LFE is left out */
#define K_FRONT 6786 /* 0.4142 */
#define K_SIDE 4799  /* 0.2929 */

/* xbdm: the game audio block ("XVGA"), read by voice.py status / written by voice.py game.
 * All u32, in this order: voice.py's GA_FIELDS mirrors it. */
struct xg_status {
	u32 magic, version;
	/* controls (xbdm writes; Share + D-pad / bumpers write them too). balance is signed
	 * (-5 all game .. +5 all chat), master is a percentage (0..100, steps of 10), sidetone
	 * the own-mic level in the earpiece (0 off .. 10, x10 %; voice_360.c mixes it) */
	volatile u32 enable, balance, master, sidetone;
	/* in effect, and the Q8 gains they give (256 = 1) */
	u32 applied_enable, applied_balance, applied_master, applied_sidetone, game_gain, chat_gain;
	u32 state, error, handle, process_type, register_tries, tap_on;
	/* the render thread (the tap) */
	u32 tap_calls, tap_frames, capture_errors, overflow_frames;
	u32 bad_floats, in_peak, in_peak_max, ring_write;
	/* the driver thread (the earpiece) */
	u32 mix_calls, mix_frames, ring_read, ring_fill;
	u32 fill_avg, target, drift_ppm, drift_changes;
	u32 underruns, overruns, starts, out_peak;
	u32 tap_rate, consumer_pad, rate_out, ch_out;
};
_Static_assert(sizeof(struct xg_status) == 42 * 4, "voice.py GA_FIELDS");

enum { XG_IDLE, XG_REGISTERED, XG_RETRY, XG_REFUSED };

__attribute__((used)) struct xg_status g_xg = {
	.magic = 0x58564741, /* "XVGA" */
	.version = 3,
	.enable = 1,   /* game audio on at boot (voice.py game off turns it off) */
	.balance = 0,  /* both at 100 % */
	/* the owner's settings on the SCUF Instinct Pro, Nov 23 2026 (the defaults he asked for) */
	.master = 80,  /* percent: VOLUME 8 */
	.sidetone = 7, /* SIDETONE 7 (heard only while talking: voice_360.c dev_quiet) */
	.target = BUFFER_MS_DEFAULT * MS_FRAMES,
	.consumer_pad = 0xffffffff,
};

struct mec_client {
	u32 callback, arg; /* the kernel copies both into its slot */
};

static u32 g_frame[FRAME_SAMPLES * FRAME_CHANNELS] __attribute__((aligned(128)));
/* (L << 16) | (u16)R, s16. 32 KB, so allocated by xg_init (a static costs image bytes: no
 * .bss); nothing reads it before the MEC client is registered, which needs it */
static u32 *g_ring;
static volatile u32 g_w;          /* written by the tap only */
static volatile u32 g_r;          /* written by the driver thread only */
static volatile u32 g_tap_on;     /* driver thread -> tap */
static volatile u32 g_handle;
static long (*pCapture)(u32 handle, void *buf);
static long (*pRegister)(const struct mec_client *client, u32 *handle);
static struct mec_client g_client;
static u32 g_peak_acc, g_peak_frames; /* the tap's */

/* driver thread */
static struct {
	u32 state;            /* C_* */
	int pad;
	u64 last_ms;          /* the last earpiece block (valid once fed) */
	u32 fed;
	u32 rate_out, step, frac;
	s32 aL, aR, bL, bR;   /* the two input frames the output lies between */
	s32 pL, pR;           /* the previous input (the decimation pre-filter) */
	s32 oL, oR;           /* the last output (pre volume), where a fade-out starts */
	u32 fade;             /* fade-in progress, frames */
	s32 avg_q4;           /* fill average, Q4 */
	s32 ppm, ppm_logged;
	s32 vol;              /* the game gain of the last block (a change ramps from it) */
} g_c;
enum { C_IDLE, C_BUFFERING, C_RUNNING };

static u64 g_install_at, g_next_try, g_last_rate_ms, g_last_summary;
static u32 g_last_tap_calls, g_failures_logged, g_resolved;
static u32 g_logged_drift, g_logged_under, g_logged_over;
static u64 g_last_event_log;
static u32 g_applied;   /* the gains were computed once */
static u32 g_quiesced;  /* hot reload (xg_quiesce): no registration, the tap stays off */

/* the ring target (driver thread): the setting in ms, and the target in effect in frames Q8,
 * which ramps to the setting while audio runs (a jump would be heard as a rebuffer). The
 * rebuffer limit and the full-trim distance follow the target in effect: at the default
 * 45 ms they are the 120 ms and 15 ms they always were. */
static u32 g_buffer_ms = BUFFER_MS_DEFAULT;
static u32 g_target_q8 = (BUFFER_MS_DEFAULT * MS_FRAMES) << 8;
static u32 g_hold;      /* xg_hold: the earpiece gets no game audio (the test tone) */
static u32 g_duck = 256; /* xg_duck: game audio's share now, Q8 (the mic test's playback) */

static u32 target_now(void)
{
	return g_target_q8 >> 8;
}

static u32 high_of(u32 target)
{
	return target + HIGH_OVER < HIGH_MAX ? target + HIGH_OVER : HIGH_MAX;
}

static u32 drift_full_of(u32 target)
{
	return target / 3 ? target / 3 : 1;
}

/* ---- the NV64 thunk: the render thread calls us with its 64-bit r14-r31 live -------------
 * Same frame as voice_360.c's XV_THUNK / thunks_common.s's NV64_THUNK. */
#define XG_SAVE(r) "    std " #r ", " #r "*8-96(1)\n"
#define XG_LOAD(r) "    ld " #r ", " #r "*8-96(1)\n"
#define XG_REGS(M) M(14) M(15) M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23) M(24) M(25) \
	M(26) M(27) M(28) M(29) M(30) M(31)
#ifndef XG_HOST_TEST
__asm__("    .text\n"
	"    .globl XgTapHook\n"
	"    .p2align 2\n"
	"XgTapHook:\n"
	"    mflr 0\n"
	"    stwu 1, -176(1)\n"
	"    stw 0, 168(1)\n"
	XG_REGS(XG_SAVE)
	"    bl XgTapC\n"
	XG_REGS(XG_LOAD)
	"    lwz 0, 168(1)\n"
	"    mtlr 0\n"
	"    addi 1, 1, 176\n"
	"    blr\n");
#endif
void XgTapHook(u32 *argp);

/* ---- conversion (integer only) ---------------------------------------------------------------- */

/* IEEE single (raw bits) -> value x 32768, |value| clamped to 2.0; NaN -> 0 (*bad set) */
static s32 f2q15(u32 b, u32 *bad)
{
	u32 e = (b >> 23) & 0xff;
	s32 v;

	if (e < 112) /* |x| < 2^-15 (and zero, denormals) */
		return 0;
	if (e == 255) {
		if (b & 0x7fffff) { /* NaN */
			*bad = 1;
			return 0;
		}
		v = 65536; /* infinity: clamped */
	} else if (e >= 128) {
		v = 65536; /* |x| >= 2 */
	} else {
		v = (s32)(((b & 0x7fffff) | 0x800000) >> (135 - e)); /* e 112..127: shift 23..8 */
	}
	return (b & 0x80000000u) ? -v : v;
}

static s32 clamp16(s32 v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static u32 abs32(s32 v)
{
	return (u32)(v < 0 ? -v : v);
}

/* one frame of the plain downmix into the ring at slot w; the larger |L| or |R| into *peak */
static inline void downmix_put(u32 w, s32 fl, s32 fr, s32 c, s32 sl, s32 sr, u32 *peak)
{
	s32 l = clamp16((fl * K_FRONT + (c + sl) * K_SIDE) >> 14);
	s32 r = clamp16((fr * K_FRONT + (c + sr) * K_SIDE) >> 14);
	u32 m = abs32(l) > abs32(r) ? abs32(l) : abs32(r);

	if (m > *peak)
		*peak = m;
	g_ring[w & (RING - 1)] = (u32)l << 16 | (u16)r;
}

/* the frame in g_frame, downmixed, into the ring at w (the tap) */
static u32 convert_frame(u32 w)
{
	const u32 *fl = g_frame, *fr = fl + FRAME_SAMPLES, *c = fr + FRAME_SAMPLES;
	const u32 *sl = c + 2 * FRAME_SAMPLES, *sr = sl + FRAME_SAMPLES; /* c + 1 = LFE */
	u32 bad = 0, peak = 0;

	for (int i = 0; i < FRAME_SAMPLES; i++)
		downmix_put(w + (u32)i, f2q15(fl[i], &bad), f2q15(fr[i], &bad), f2q15(c[i], &bad),
			    f2q15(sl[i], &bad), f2q15(sr[i], &bad), &peak);
	if (bad)
		g_xg.bad_floats++;
	return peak;
}

/* the demo's blocks (surround_360.c): the same downmix of the virtualizer's input, which then
 * holds the clip (s16 values), not the game */
__attribute__((noinline)) static u32 convert_in(u32 w)
{
	const s32 *fl = xsv_tap_input(), *fr = fl + FRAME_SAMPLES, *c = fr + FRAME_SAMPLES;
	const s32 *sl = c + 2 * FRAME_SAMPLES, *sr = sl + FRAME_SAMPLES;
	u32 peak = 0;

	for (u32 i = 0; i < FRAME_SAMPLES; i++)
		downmix_put(w + i, fl[i], fr[i], c[i], sl[i], sr[i], &peak);
	return peak;
}

/* two s16 (L, R) read as one word: the ring's (L << 16) | R on the console (big-endian) */
typedef u32 __attribute__((may_alias)) u32_alias;

static u32 lr_word(const s16 *lr, u32 i)
{
	u32 v = ((const u32_alias *)lr)[i];

#if __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
	v = v << 16 | v >> 16; /* the host test */
#endif
	return v;
}

static s16 *g_lr, *g_plain; /* the tap's: surround_in -> surround_out */

/* Surround's gate on, step 1: the frame converted for the virtualizer (surround_360.c),
 * which runs it when it is needed and says what the ring gets (XSV_*). While the demo plays
 * (dm, XSV_DM_*) the clip takes the frame's place: alone (the game's frame is not even
 * captured), or crossfaded with it on the first and last block. */
__attribute__((noinline)) static u32 surround_in(u32 dm)
{
	s32 *in = xsv_tap_input();
	u32 bad = 0, how;

	if (dm != XSV_DM_PLAY) {
#ifdef XW_DEV
		xsv_dev_inject(g_frame);
#endif
		for (u32 k = 0; k < FRAME_SAMPLES * FRAME_CHANNELS; k++)
			in[k] = clamp16(f2q15(g_frame[k], &bad));
	}
	if (dm != XSV_DM_NONE)
		xsv_demo_fill(dm, in);
	how = xsv_tap_run(&g_lr, &g_plain);
	if (bad && (how == XSV_VIRTUAL || dm != XSV_DM_NONE))
		g_xg.bad_floats++; /* otherwise convert_frame runs and counts them */
	return how;
}

/* Step 2: the virtualizer's stereo into the ring at w; for a switch, crossfaded with the
 * downmix convert_frame has just put there (from it when switching on, to it when off). */
__attribute__((noinline)) static u32 surround_out(u32 w, u32 how)
{
	s16 *lr = g_lr, *plain = g_plain;
	u32 peak = 0;

	if (how != XSV_VIRTUAL) {
		bool in = how == XSV_FADE_IN;

		for (u32 i = 0; i < FRAME_SAMPLES; i++) {
			u32 v = g_ring[(w + i) & (RING - 1)];

			plain[2 * i] = (s16)(v >> 16);
			plain[2 * i + 1] = (s16)v;
		}
		xsr_crossfade(lr, in ? plain : lr, in ? lr : plain, FRAME_SAMPLES);
	}
	for (u32 i = 0; i < FRAME_SAMPLES; i++) {
		u32 v = lr_word(lr, i), m;
		s32 l = (s32)v >> 16, r = (s16)v; /* word loads: lha is microcoded on the Xenon */

		m = abs32(l) > abs32(r) ? abs32(l) : abs32(r);
		if (m > peak)
			peak = m;
		g_ring[(w + i) & (RING - 1)] = v;
	}
	return peak;
}

/* ---- the tap: the render driver thread, once per 256-sample frame ------------------------------ */

HOOK void XgTapC(u32 *argp)
{
	u32 w, r, peak = 0, xs, t0 = 0, how, dm = XSV_DM_NONE;

	(void)argp;
	g_xg.tap_calls++;
	if (!g_tap_on)
		return; /* off, or no headset being fed: nothing else happens */
	BARRIER(); /* g_handle and pCapture were set before g_tap_on */
	xs = g_xsv_tap; /* surround's gate: 0 = everything below as it was without surround */
	if (xs) {
		BARRIER(); /* surround's state was ready before its gate opened */
		t0 = xd_timebase();
		dm = xsv_demo_begin();
	}
	/* the demo alone needs nothing from the game */
	if (dm != XSV_DM_PLAY && pCapture(g_handle, g_frame) != 0) {
		g_xg.capture_errors++;
		return;
	}
	w = g_w;
	r = g_r;
	BARRIER(); /* the reader's position before any slot is overwritten */
	if (RING - (w - r) < FRAME_SAMPLES) {
		g_xg.overflow_frames += FRAME_SAMPLES; /* the reader stalled: this frame is dropped */
		return;
	}
	how = xs ? surround_in(dm) : XSV_PLAIN;
	if (how != XSV_VIRTUAL) /* with the gate off: convert_frame, always, as before surround */
		peak = dm != XSV_DM_NONE ? convert_in(w) : convert_frame(w);
	if (how != XSV_PLAIN)
		peak = surround_out(w, how);
	BARRIER(); /* the samples before the position that publishes them */
	g_w = w + FRAME_SAMPLES;
	g_xg.ring_write = w + FRAME_SAMPLES;
	g_xg.tap_frames++;
	if (peak > g_peak_acc)
		g_peak_acc = peak;
	if (++g_peak_frames >= FRAMES_PER_S) {
		g_xg.in_peak = g_peak_acc;
		if (g_peak_acc > g_xg.in_peak_max)
			g_xg.in_peak_max = g_peak_acc;
		g_peak_acc = g_peak_frames = 0;
	}
	if (xs) {
		if (dm != XSV_DM_NONE)
			xsv_demo_end(dm);
		xsv_tap_done(t0);
	}
}

/* ---- the earpiece side (driver thread) ---------------------------------------------------------- */

static void pop(u32 *r, s32 *l, s32 *rr, bool filter)
{
	u32 v = g_ring[*r & (RING - 1)];
	s32 x = (s16)(v >> 16), y = (s16)v;

	(*r)++;
	if (filter) { /* decimating: a 2-tap average first (a zero at 24 kHz) */
		s32 fx = (x + g_c.pL) >> 1, fy = (y + g_c.pR) >> 1;

		g_c.pL = x;
		g_c.pR = y;
		x = fx;
		y = fy;
	}
	*l = x;
	*rr = y;
}

/* volume (Q8, <= 256) applied, then added to `frames` frames of out, clamped */
static void add_frame(u8 *o, u32 ch, s32 l, s32 r, s32 vol)
{
	s32 gl = l * vol >> 8, gr = r * vol >> 8;
	u32 m = abs32(gl) > abs32(gr) ? abs32(gl) : abs32(gr);

	if (m > g_xg.out_peak)
		g_xg.out_peak = m;
	if (ch == 1) {
		put_le16(o, (u16)clamp16((s16)get_le16(o) + ((gl + gr) >> 1)));
		return;
	}
	put_le16(o, (u16)clamp16((s16)get_le16(o) + gl));
	put_le16(o + 2, (u16)clamp16((s16)get_le16(o + 2) + gr));
}

/* the last output ramped to silence over the block (no click), then a fresh start */
static void fade_out(u8 *out, int frames, u32 ch, s32 vol)
{
	int n = frames < FADE ? frames : FADE;

	for (int f = 0; f < n; f++) {
		s32 g = (s32)(FADE - (u32)f * FADE / (u32)n); /* 256 .. >0 */

		add_frame(out + f * 2 * (int)ch, ch, g_c.oL * g >> 8, g_c.oR * g >> 8, vol);
	}
	g_c.oL = g_c.oR = 0;
	g_c.state = C_IDLE;
}

void xg_mix(int pad, u8 *out, int len, u32 rate_out, u32 ch_out)
{
	u64 now = xa_now_ms();
	/* master x game level x the duck, Q8, <= 256; a change ramps over the block (g_c.vol) */
	s32 vol = (s32)(g_xg.game_gain * (g_duck <= 256 ? g_duck : 256) >> 8);
	u32 w, r, avail, need, frames, target;
	bool filter;

#ifdef XW_DEV
	if (ch_out == 1 || ch_out == 2)
		xsv_dev_driver_load((u32)len / (2 * ch_out)); /* surround's probe, load_site 1 */
#endif
	if (!g_xg.applied_enable || g_xg.state != XG_REGISTERED || len <= 0 || ch_out < 1 ||
	    ch_out > 2 || rate_out < 4000 || rate_out > 48000 || rate_out % 1000) {
		g_c.state = C_IDLE;
		return;
	}
	if (g_hold) {
		/* the test tone has the earpiece: what was playing fades out over this block,
		 * and the stream starts over (buffering, fade-in) when the hold ends. The tap
		 * idles meanwhile (xg_poll: no block taken for CONSUMER_IDLE_MS) */
		if (g_c.state == C_RUNNING && pad == g_c.pad && rate_out == g_c.rate_out)
			fade_out(out, len / (int)(2 * ch_out), ch_out, vol);
		g_c.state = C_IDLE;
		return;
	}
	/* one earpiece at a time (xw.xex streams one headset): the first that asks */
	if (pad != g_c.pad) {
		if (g_c.fed && now - g_c.last_ms < CONSUMER_IDLE_MS)
			return;
		g_c.pad = pad;
		g_c.state = C_IDLE;
	}
	if (rate_out != g_c.rate_out) {
		g_c.rate_out = rate_out;
		g_c.step = (48u << 20) / (rate_out / 1000); /* input frames per output frame, Q20 */
		g_c.state = C_IDLE;
	}
	g_c.last_ms = now;
	g_c.fed = 1;
	g_xg.mix_calls++;
	g_xg.consumer_pad = (u32)pad;
	g_xg.rate_out = rate_out;
	g_xg.ch_out = ch_out;
	frames = (u32)len / (2 * ch_out);
	/* frac + frames x step must fit in 32 bits (an 8 ms block is ~384 x 2^20 at any rate) */
	if (frames > 0x70000000u / (g_c.step + g_c.step / 512))
		frames = 0x70000000u / (g_c.step + g_c.step / 512);
	filter = g_c.step >= ONE + ONE / 2;

	w = g_w;
	BARRIER(); /* the position before the samples it covers */
	if (g_c.state == C_IDLE) {
		/* (re)start with an empty ring: whatever is in it may be stale */
		g_r = w;
		g_c.state = C_BUFFERING;
		g_target_q8 = (g_buffer_ms * MS_FRAMES) << 8; /* nothing is playing: no ramp */
		g_c.avg_q4 = (s32)(target_now() << 4);
		g_xg.starts++;
	}
	r = g_r;
	avail = w - r;
	g_xg.ring_fill = avail;
	if (avail > RING) { /* can't happen (the tap never overruns r); start over */
		g_c.state = C_IDLE;
		return;
	}
	if (g_c.state == C_BUFFERING) {
		g_xg.target = target_now();
		if (avail < target_now() + 2)
			return;
		g_c.pL = g_c.pR = 0;
		pop(&r, &g_c.aL, &g_c.aR, filter);
		pop(&r, &g_c.bL, &g_c.bR, filter);
		g_c.frac = 0;
		g_c.fade = 0;
		g_c.state = C_RUNNING;
		avail -= 2;
	}
	{
		/* a new buffer setting: the target moves there slowly, and the trim follows */
		u32 want = (g_buffer_ms * MS_FRAMES) << 8;
		u32 step = frames * TARGET_RAMP_Q8 / rate_out + 1;

		if (g_target_q8 < want)
			g_target_q8 = want - g_target_q8 > step ? g_target_q8 + step : want;
		else if (g_target_q8 > want)
			g_target_q8 = g_target_q8 - want > step ? g_target_q8 - step : want;
		target = target_now();
		g_xg.target = target;
	}
	if (avail > high_of(target)) {
		/* far too full (the driver thread was held up): fade out, then start over */
		g_xg.overruns++;
		fade_out(out, (int)frames, ch_out, vol);
		return;
	}

	/* the ratio trim: slow (the average follows over ~0.5 s), at most DRIFT_MAX_PPM */
	g_c.avg_q4 += ((s32)(avail << 4) - g_c.avg_q4) >> 6;
	{
		s32 err = (g_c.avg_q4 >> 4) - (s32)target;
		s32 ppm = err * DRIFT_MAX_PPM / (s32)drift_full_of(target);

		if (ppm > DRIFT_MAX_PPM)
			ppm = DRIFT_MAX_PPM;
		if (ppm < -DRIFT_MAX_PPM)
			ppm = -DRIFT_MAX_PPM;
		g_c.ppm = ppm;
		if (abs32(ppm - g_c.ppm_logged) >= 100) { /* counted (and logged by xg_poll) */
			g_c.ppm_logged = ppm;
			g_xg.drift_changes++;
		}
		g_xg.fill_avg = (u32)(g_c.avg_q4 >> 4);
		g_xg.drift_ppm = (u32)ppm;
	}
	{
		u32 step = (u32)((s32)g_c.step + (s32)(g_c.step >> 10) * g_c.ppm / 977);

		need = (g_c.frac + frames * step) >> 20;
		if (avail < need) {
			g_xg.underruns++;
			fade_out(out, (int)frames, ch_out, vol);
			return;
		}
		for (u32 f = 0; f < frames; f++) {
			s32 t = (s32)(g_c.frac >> 6); /* Q14 */
			s32 l = g_c.aL + (((g_c.bL - g_c.aL) * t) >> 14);
			s32 rr = g_c.aR + (((g_c.bR - g_c.aR) * t) >> 14);

			if (g_c.fade < FADE) {
				l = l * (s32)g_c.fade >> 8;
				rr = rr * (s32)g_c.fade >> 8;
				g_c.fade++;
			}
			g_c.oL = l;
			g_c.oR = rr;
			/* a gain change ramps over the block (no zipper click on a balance step) */
			add_frame(out + f * 2 * ch_out, ch_out, l, rr,
				  g_c.vol + (vol - g_c.vol) * (s32)f / (s32)frames);
			g_c.frac += step;
			while (g_c.frac >= ONE) {
				g_c.frac -= ONE;
				g_c.aL = g_c.bL;
				g_c.aR = g_c.bR;
				pop(&r, &g_c.bL, &g_c.bR, filter);
			}
		}
	}
	BARRIER(); /* done reading the slots before the tap may reuse them */
	g_r = r;
	g_xg.ring_read = r;
	g_xg.mix_frames += frames;
	g_c.vol = vol;
}

/* ---- registration and control (driver thread) ------------------------------------------------ */

static bool code_ok(void)
{
	for (u32 i = 0; i < sizeof(SIGS) / sizeof(SIGS[0]); i++) {
		const volatile u32 *c = (const volatile u32 *)SIGS[i].addr;

		for (u32 k = 0; k < SIGS[i].n; k++)
			if (c[k] != SIGS[i].w[k]) {
				xa_log("game: kernel code at %08x is %08x, not %08x (not 17559's render "
				       "driver): no game audio\n", SIGS[i].addr + 4 * k, c[k], SIGS[i].w[k]);
				return false;
			}
	}
	return true;
}

static bool resolve(void)
{
	HANDLE krnl;
	void *reg = 0, *cap = 0;

	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0)
		return false;
	XexGetProcedureAddress(krnl, ORD_XAudioRegisterRenderDriverMECClient, &reg);
	XexGetProcedureAddress(krnl, ORD_XAudioCaptureRenderDriverFrame, &cap);
	xa_log("game: XAudioRegisterRenderDriverMECClient %08x, XAudioCaptureRenderDriverFrame "
	       "%08x\n", (u32)reg, (u32)cap);
	if ((u32)reg != ADDR_REGISTER_MEC || (u32)cap != ADDR_CAPTURE) {
		xa_log("game: not the 17559 exports: no game audio\n");
		return false;
	}
	if (!code_ok())
		return false;
	pRegister = reg;
	pCapture = cap;
	return true;
}

static void try_register(u64 now)
{
	u32 handle = 0, type;
	long st;

	if (!g_resolved) {
		g_resolved = 1;
		if (!resolve()) {
			g_xg.state = XG_REFUSED;
			g_xg.error = (u32)-XA_EINVAL;
			return;
		}
	}
	type = (u32)KeGetCurrentProcessType();
	g_xg.process_type = type;
	if (type == PROCESS_TITLE) {
		/* the kernel removes a title's MEC clients when the title exits */
		xa_log("game: driver thread runs as a title process: no game audio\n");
		g_xg.state = XG_REFUSED;
		return;
	}
	g_client.callback = (u32)XgTapHook;
	g_client.arg = 0;
	g_xg.register_tries++;
	st = pRegister(&g_client, &handle);
	if (st != 0) {
		g_xg.state = XG_RETRY;
		g_xg.error = (u32)st;
		g_next_try = now + RETRY_MS;
		if (g_failures_logged++ < 3)
			xa_log("game: MEC client not registered: %08x (both slots in use? Kinect "
			       "uses one); retrying every %u s\n", (u32)st, RETRY_MS / 1000);
		return;
	}
	g_xg.handle = handle;
	if (handle < MEC_SLOTS || handle >= MEC_SLOTS + MEC_SLOT_COUNT * MEC_SLOT_SIZE ||
	    (handle - MEC_SLOTS) % MEC_SLOT_SIZE) {
		/* registered, but not where the capture check expects: never captured (the
		 * callback stays a counter) */
		xa_log("game: MEC handle %08x outside the slots at %08x: no game audio\n", handle,
		       MEC_SLOTS);
		g_xg.state = XG_REFUSED;
		return;
	}
	g_handle = handle;
	g_xg.error = 0;
	BARRIER();
	g_xg.state = XG_REGISTERED;
	xa_log("game: MEC client registered (handle %08x, process type %u): game audio %s\n",
	       handle, type, g_xg.applied_enable ? "on" : "off");
}

/* VOLUME by the headset's own amplifier (xg_master_device, from voice_360.c's
 * xv_headset_volume): the samples then go out at full level (g_dig_master 100, or 0 at VOLUME
 * 0) and the amplifier is turned down instead, so its own noise (a buzz at its full gain,
 * heard on the console Nov 23 2026 with digital silence going out) drops with the volume. */
static u32 g_master_dev, g_dig_master = 100;

void xg_master_device(bool on)
{
	if (g_master_dev == (u32)on)
		return;
	g_master_dev = on;
	g_applied = 0; /* the gains again, on the next apply_controls */
	xa_log("game: VOLUME by %s\n", on ? "the headset's amplifier (samples at full level)"
					  : "the samples (software)");
}

/* the controls, validated (a bad xbdm value is clamped and written back), and the gains */
static void apply_controls(void)
{
	u32 dm;

	u32 en = g_xg.enable ? 1 : 0, m = g_xg.master, st = g_xg.sidetone;
	s32 bal = (s32)g_xg.balance;
	u32 game_pct, chat_pct;

	if (bal < -XG_BALANCE_MAX)
		bal = -XG_BALANCE_MAX;
	if (bal > XG_BALANCE_MAX)
		bal = XG_BALANCE_MAX;
	if (m > 100)
		m = 100;
	m -= m % XG_MASTER_STEP;
	if (st > XG_SIDETONE_MAX)
		st = XG_SIDETONE_MAX;
	if (g_xg.enable != en)
		g_xg.enable = en;
	if (g_xg.balance != (u32)bal)
		g_xg.balance = (u32)bal;
	if (g_xg.master != m)
		g_xg.master = m;
	if (g_xg.sidetone != st)
		g_xg.sidetone = st;
	if (en == g_xg.applied_enable && (u32)bal == g_xg.applied_balance &&
	    m == g_xg.applied_master && st == g_xg.applied_sidetone && g_applied)
		return;
	g_applied = 1;
	/* each balance step away from a side cuts that side by 20 % */
	game_pct = bal > 0 ? 100 - 20 * (u32)bal : 100;
	chat_pct = bal < 0 ? 100 - 20 * (u32)-bal : 100;
	g_xg.applied_enable = en;
	g_xg.applied_balance = (u32)bal;
	g_xg.applied_master = m;
	g_xg.applied_sidetone = st;
	dm = g_master_dev && m ? 100 : m;
	g_dig_master = dm;
	g_xg.game_gain = dm * game_pct * 256 / 10000; /* <= 256 */
	g_xg.chat_gain = dm * chat_pct * 256 / 10000;
	xa_log("game: game audio %s, balance %d (game %u%%, chat %u%%), master %u%%, sidetone "
	       "%u/10\n", en ? "on" : "off", (int)bal, game_pct, chat_pct, m, st);
}

/* Hot reload (docs/HOT-RELOAD.md), driver thread. The tap stops copying at once; then the
 * MEC client is unregistered, so the next build finds its slot free. The kernel's unregister
 * (0x8012DBA0) takes the render driver's lock and, called off the render thread, yields
 * while the render thread is inside this very client's callback: when it returns, XgTapC is
 * not running and is never called again. It is only called if its code is the 17559 code
 * read offline. If it can't be called, or fails, the client simply stays registered: XgTapC
 * then returns at its first test for good (g_tap_on = 0, and this image is never unloaded),
 * at the price of one of the two MEC slots until the console reboots. */
#ifndef XG_HOST_TEST
void xg_quiesce(void)
{
	static const struct sig unreg[2] = { { ADDR_UNREGISTER_MEC, 6, SIG_UNREG_EXPORT },
					     { 0x8012DBA0, 10, SIG_UNREG_BODY } };
	long (*unregister)(u32 handle) = 0;
	HANDLE krnl;
	void *p = 0;
	long st;

	g_quiesced = 1;
	g_tap_on = 0;
	g_xg.tap_on = 0;
	BARRIER();
	if (g_xg.state != XG_REGISTERED) {
		xsv_quiesce(true); /* no client: the tap never runs */
		xa_log("game: quiesced (no MEC client registered)\n");
		return;
	}
	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) >= 0)
		XexGetProcedureAddress(krnl, ORD_XAudioUnregisterRenderDriverMECClient, &p);
	if ((u32)p == ADDR_UNREGISTER_MEC) {
		unregister = p;
		for (u32 i = 0; i < 2 && unregister; i++) {
			const volatile u32 *c = (const volatile u32 *)unreg[i].addr;

			for (u32 k = 0; k < unreg[i].n; k++)
				if (c[k] != unreg[i].w[k])
					unregister = 0;
		}
	}
	if (!unregister) {
		xsv_quiesce(false); /* the tap may be inside a block now (it stops at g_tap_on) */
		xa_log("game: quiesced; XAudioUnregisterRenderDriverMECClient (%08x) is not the 17559 "
		       "code: the MEC client stays registered and idle (one of the two slots is "
		       "lost until the console reboots)\n", (u32)p);
		return;
	}
	st = unregister(g_handle);
	if (st != 0) {
		xsv_quiesce(false);
		xa_log("game: quiesced; MEC unregister(%08x) failed: %08x: the client stays "
		       "registered and idle\n", (u32)g_handle, (u32)st);
		return;
	}
	g_xg.state = XG_IDLE;
	g_xg.handle = 0;
	xsv_quiesce(true); /* the unregister waited for a running callback: the tap never runs again */
	xa_log("game: quiesced, MEC client %08x unregistered\n", (u32)g_handle);
}

/* a refused hot reload: register again (xg_poll does, from the next pass on) */
void xg_resume(void)
{
	g_quiesced = 0;
	g_install_at = xa_now_ms();
	xsv_resume();
	xa_log("game: resumed\n");
}
#endif

void xg_init(void)
{
	g_c.pad = -1;
	g_install_at = xa_now_ms() + INSTALL_DELAY_MS;
	apply_controls();
	g_ring = malloc(RING * sizeof(*g_ring));
	if (!g_ring) {
		/* never registered (xg_poll registers only from XG_IDLE / XG_RETRY): no game audio */
		g_xg.state = XG_REFUSED;
		g_xg.error = (u32)-XA_ENOMEM;
		xa_log("game: no memory for the ring (%u bytes): no game audio\n",
		       (u32)(RING * sizeof(*g_ring)));
	}
	xsv_init();
	xa_log("game: ready, game audio %s (registered %u s after start)\n",
	       g_xg.applied_enable ? "on" : "off", INSTALL_DELAY_MS / 1000);
}

void xg_poll(void)
{
	u64 now = xa_now_ms();
	u32 on;

	apply_controls();
	xsv_poll();
	if (g_quiesced)
		return;
	if (g_xg.applied_enable && now >= g_install_at &&
	    (g_xg.state == XG_IDLE || (g_xg.state == XG_RETRY && now >= g_next_try)))
		try_register(now);

	/* the tap copies only while an earpiece is being fed with game audio on */
	if (g_c.state != C_IDLE && (!g_c.fed || now - g_c.last_ms >= CONSUMER_IDLE_MS))
		g_c.state = C_IDLE; /* the stream stopped: start over when it comes back */
	on = g_xg.applied_enable && g_xg.state == XG_REGISTERED && g_c.fed &&
	     now - g_c.last_ms < CONSUMER_IDLE_MS;
	if (on != g_tap_on) {
		g_tap_on = on;
		g_xg.tap_on = on;
		xa_log("game: tap %s\n", on ? "copying" : "idle");
	}
	if (now - g_last_rate_ms >= 1000) {
		u32 calls = g_xg.tap_calls;

		g_xg.tap_rate = calls - g_last_tap_calls;
		g_last_tap_calls = calls;
		g_last_rate_ms = now;
	}
	/* corrections and stream events, at most one line a second */
	if ((g_xg.drift_changes != g_logged_drift || g_xg.underruns != g_logged_under ||
	     g_xg.overruns != g_logged_over) && now - g_last_event_log >= 1000) {
		g_last_event_log = now;
		xa_log("game: trim %d ppm (fill avg %u, target %u; %u trim changes), underruns %u, "
		       "overruns %u, starts %u\n", (int)g_xg.drift_ppm, g_xg.fill_avg, target_now(),
		       g_xg.drift_changes, g_xg.underruns, g_xg.overruns, g_xg.starts);
		g_logged_drift = g_xg.drift_changes;
		g_logged_under = g_xg.underruns;
		g_logged_over = g_xg.overruns;
	}
	if (on && now - g_last_summary >= 10000) {
		g_last_summary = now;
		xa_log("game: tap %u/s, frames %u, fill %u (avg %u, target %u), trim %d ppm, "
		       "underruns %u overruns %u overflow %u, peak in %u out %u\n", g_xg.tap_rate,
		       g_xg.tap_frames, g_xg.ring_fill, g_xg.fill_avg, target_now(), (int)g_xg.drift_ppm,
		       g_xg.underruns, g_xg.overruns, g_xg.overflow_frames, g_xg.in_peak,
		       g_xg.out_peak);
		g_xg.out_peak = 0;
	}
}

bool xg_set_buffer(u32 ms)
{
	if (ms != 30 && ms != 45 && ms != 60 && ms != 90)
		return false;
	g_buffer_ms = ms; /* xg_mix ramps the target there (or starts with it) */
	return true;
}

u32 xg_buffer_target(void)
{
	return target_now();
}

u32 xg_buffer_high(void)
{
	return high_of(target_now());
}

u32 xg_buffer_drift_full(void)
{
	return drift_full_of(target_now());
}

void xg_duck(u32 q8)
{
	g_duck = q8 <= 256 ? q8 : 256;
}

void xg_hold(bool on)
{
	g_hold = on;
}

bool xg_routed(void)
{
	return g_tap_on != 0;
}

int xg_balance(void)
{
	return (s32)g_xg.applied_balance;
}

u32 xg_master(void)
{
	return g_xg.applied_master;
}

u32 xg_chat_gain(void)
{
	return g_xg.chat_gain;
}

u32 xg_master_gain(void)
{
	return g_dig_master * 256 / 100;
}

u32 xg_sidetone(void)
{
	return g_xg.applied_sidetone;
}

void xg_adjust(int dbal, int dmaster, int dside)
{
	s32 bal = (s32)g_xg.applied_balance + dbal;
	s32 m = (s32)g_xg.applied_master + dmaster * XG_MASTER_STEP;
	s32 st = (s32)g_xg.applied_sidetone + dside;

	g_xg.balance = (u32)(bal < -XG_BALANCE_MAX ? -XG_BALANCE_MAX
				: bal > XG_BALANCE_MAX ? XG_BALANCE_MAX : bal);
	g_xg.master = (u32)(m < 0 ? 0 : m > 100 ? 100 : m);
	g_xg.sidetone = (u32)(st < 0 ? 0 : st > XG_SIDETONE_MAX ? XG_SIDETONE_MAX : st);
	apply_controls(); /* heard from the next earpiece block on */
}
