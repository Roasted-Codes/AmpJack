// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The virtual 360 headset (see voice_360.h and research/AUDIO.md, sections 1.2, 1.3, 2, 5, 6).
 *
 * A jack headset on one of our pads is presented to the 360 as that player's headset:
 *
 *   - three kernel exports are detoured: XVoicedHeadsetPresent (481), XVoicedSubmitPacket
 *     (482) and XVoicedClose (483). They act only when ctx[0] is one of our device tags
 *     (0x10000005 + slot, xinput_360.c's CONTEXT_BASE); every other tag runs the kernel's
 *     own code. Our tags are NEVER handed to the kernel once the hooks are in: the kernel
 *     would route 0x1000000n to wireless slot n, which for n = 5..8 lies past the wireless
 *     table (AUDIO.md 1.3: "present" read from a float, then a float used as a spinlock).
 *   - the headset is bound in XAM category 1 with the pad's controller id and tag, as the
 *     kernel's own wired driver does (0x801064A4), on the XAM thread (xinput_360.c's
 *     request queue, so it is ordered after the pad's bind and before the pad's unbind).
 *   - packets (caller-owned: [0] status, [4] bytes done, [8] buffer, [12] length, [16],
 *     [20] XHV's "mode") wait in per-pad capture/render rings; the driver thread fills
 *     capture packets with converted mic PCM and drains render packets into the earpiece,
 *     writing pkt[4], lwsync, then pkt[0] (the order the kernel's completion uses).
 *
 * g_xv.stage (xbdm-writable, voice.py stage N) is 4 at boot; the hooks go in on the driver
 * thread 20 s after start, once no title has a voice context open:
 *   0  nothing installed
 *   1  detours installed (on the driver thread, at the moment stage >= 1 is seen), pure
 *      pass-through for stock tags, with counters and, for a stock capture stream, its
 *      packet sizes, byte rate and first bytes (the R2 measurement); our tags answer
 *      "no headset" (they can't occur here: nothing is bound)
 *   2  the same: act for our tags, report no headset (R3)
 *   3  virtual headset: bound in category 1 while the pad reports a headset (or the
 *      pad's bit is set in g_xv.force), packets served by an internal 8 ms clock with
 *      silence (or the render stream looped back, g_xv.loopback), no PCM from USB (V1)
 *   4  real PCM both ways (xv_mic_samples / xv_earpiece_fill); the internal clock
 *      takes over a direction whose USB stream stops for more than 64 ms
 * Lowering the stage never removes the hooks (a title thread could be inside one): below
 * 3, bound headsets are unbound and their packets failed; the hooks keep shielding our tags.
 *
 * Locking: the hooks run on title/system threads, the service on the driver thread. One
 * spinlock (KfAcquireSpinLock, the call XVoicedSubmitPacket itself uses on its device
 * lock) covers the rings and every packet write, held only for copies (no calls inside).
 *
 * No floating point, no .bss (build.sh), every kernel entry through an NV64 thunk (below).
 * The sidetone, the mic's processing and the mic test are in side_360.c, micdsp_360.c and
 * mictest_360.c (voice_priv.h).
 */
#include <stdlib.h>
#include <xecore/xboxkrnl.h>

#include "../core/gip.h" /* GIP_VOL_*: the headset's own volume settings */
#include "../core/platform.h"
#include "detour.h"
#include "eq_360.h"
#include "diag_360.h"
#include "game_360.h"
#include "platform_360.h"
#include "team_360.h"
#include "voice_360.h"
#include "voice_priv.h"
#include "xinput_360.h"

/* ---- kernel 17559 ------------------------------------------------------------------------ */
#define ORD_XVoicedHeadsetPresent 481
#define ORD_XVoicedSubmitPacket 482
#define ORD_XVoicedClose 483
#define ORD_XVoicedIsActiveProcess 860
#define ADDR_XVoicedHeadsetPresent 0x80101C40
#define ADDR_XVoicedSubmitPacket 0x80102048
#define ADDR_XVoicedClose 0x80102230
#define ADDR_XVoicedIsActiveProcess 0x80101DE8
/* first four instructions of each, from the 17559 dump: checked before anything is patched */
static const u32 SIG_PRESENT[4] = { 0x7d8802a6, 0x9181fff8, 0x9421ffa0, 0x80630000 };
static const u32 SIG_SUBMIT[4] = { 0x7d8802a6, 0x4800ab71, 0x9421ff70, 0x7c7c1b78 };
static const u32 SIG_CLOSE[4] = { 0x7d8802a6, 0x4800a995, 0x9421ff80, 0x7c7d1b78 };
static const u32 SIG_ACTIVE[4] = { 0x7d8802a6, 0x9181fff8, 0x9421ffa0, 0x4bf6fc75 };

/* statuses, as XVoicedSubmitPacket and its device code write them */
#define ST_PENDING 0x103
#define ST_NO_HEADSET 0xD000009D  /* no device, or its present byte is 0 */
#define ST_NO_NODE 0x800700AA     /* no free node */
#define ST_INACTIVE_CAPTURE 0x80000011 /* inactive process, capture packet */
#define ST_CANCELLED 0xC0000001   /* close (0x80101ED8) and removal (0x801065C8) */
/* what the wired headset driver writes to a capture packet's +16 when it starts it
 * (0x80107078..0x80107094); its +20 then holds XHV's requested mode, unchanged */
#define CAP_FLAGS_DEFAULT 0x20000000

/* our tags and controller ids: xinput_360.c's CONTEXT_BASE (hiddriver360's choice) */
#ifdef XA_XINPUT_CONTEXT_BASE
#define XV_TAG_BASE XA_XINPUT_CONTEXT_BASE
#else
#define XV_TAG_BASE 0x10000005
#endif

#define BLOCK_MS 8        /* the GIP audio block, and the internal clock's tick */
#define USB_STALE_MS 64   /* stage 4: a USB direction silent this long -> internal clock */
#define EVENTS 32         /* hook events, logged by the driver thread */
#define EVENTS_LOGGED 64  /* the first this many are logged, then only counters */
#define WATCH 4           /* stock capture packets watched for completion (R2) */

/* sample formats on the 360 side (g_xv.cap_fmt / ren_fmt) */
enum { FMT_S16LE, FMT_S16BE, FMT_U8, FMT_ULAW, FMT_COUNT };

/* ---- status and control block, read and written over xbdm (voice.py) --------------------
 * All u32, in this order: voice.py's field lists mirror it. */
struct xv_pad_status {
	u32 usb_present, forced, bound, serving;
	u32 rate_in, ch_in, rate_out, ch_out;
	u32 cap_submit, ren_submit, cap_done, ren_done;
	u32 cap_full, ren_full, cancelled, inactive;
	u32 cap_bytes, ren_bytes, cap_queued, ren_queued;
	u32 mic_calls, mic_bytes, mic_dropped, ear_calls;
	u32 ear_underrun, vticks, cap_len, ren_len;
	u32 cap_mode, ren_mode, mic_peak, ear_peak;
	u32 present_calls, close_calls, bind_requests, unbind_requests;
	u32 bind_errors, loop_samples, cap_first_ms, cap_last_ms;
};

struct xv_status {
	u32 magic, version;
	/* controls (xbdm writes) */
	volatile u32 stage, cap_rate, cap_fmt, ren_rate;
	volatile u32 ren_fmt, loopback, force, cap_flags;
	volatile u32 debug;
	/* the running title (XexExecutableModuleHandle), packets dropped unwritten because
	 * their title is gone, title switches seen */
	u32 title, stale_dropped, title_switches;
	/* what is in effect */
	u32 applied_stage, applied_cap_rate, applied_cap_fmt, applied_ren_rate;
	u32 applied_ren_fmt, hooks, hook_error, heartbeat;
	u32 log_addr, now_ms, calls_present, calls_submit;
	u32 calls_close, our_present, our_submit, our_close;
	/* stock devices (R2): the last stock tag seen and its streams */
	u32 stock_tag, stock_cap_submit, stock_ren_submit, stock_cap_len;
	u32 stock_ren_len, stock_cap_done, stock_cap_bytes, stock_cap_first_ms;
	u32 stock_cap_last_ms, stock_ren_bytes, stock_ren_first_ms, stock_ren_last_ms;
	u32 stock_cap_mode, stock_cap_flags, stock_first_len, stock_first_status;
	u32 stock_first[8];  /* the first 32 bytes of the first completed stock capture */
	u32 stock_ren_mode, stock_ren_flags, stock_status_last, stock_close;
	struct xv_pad_status pads[PADS];
};

/* voice.py's GLOBAL and PAD lists are these, word for word */
_Static_assert(sizeof(struct xv_pad_status) == 40 * 4, "voice.py PAD");
_Static_assert(sizeof(struct xv_status) == (58 + PADS * 40) * 4, "voice.py GLOBAL");

#define XV_VERSION 1
#define HOOK_PRESENT 1
#define HOOK_SUBMIT 2
#define HOOK_CLOSE 4
#define HOOK_ALL 7
#define HOOK_FAILED 0x80000000u
#define HOOK_NO_BIND 0x40000000u /* built without xinput_360's headset requests */

__attribute__((used)) struct xv_status g_xv = {
	.magic = 0x58563336, /* "XV36", found by voice.py the way xw.py finds its block */
	.version = XV_VERSION,
	.stage = 4, /* the virtual headset on (voice.py stage 0 = off) */
	/* best estimate of the 360 side (AUDIO.md 1.1; see voice.py and the report): 8 kHz,
	 * 16-bit, little-endian. xbdm-writable; validated before use. */
	.cap_rate = 8000,
	.cap_fmt = FMT_S16LE,
	.ren_rate = 8000,
	.ren_fmt = FMT_S16LE,
	.cap_flags = CAP_FLAGS_DEFAULT,
};

/* ---- per-pad state ------------------------------------------------------------------------ */

/* A queued packet belongs to the process that submitted it: OWNER_SYSTEM for the system
 * process (the Guide's XHV, which outlives titles), else the title's executable module
 * handle at submit time. A packet whose title is no longer the running one is dropped
 * without a single write (its memory may already belong to the next title). */
#define OWNER_SYSTEM 0xFFFFFFFFu

struct xv_pad g_pad[PADS];

/* the spinlock, alone on its 128-byte line (the reservation granule) */
static u32 g_lock_line[32] __attribute__((aligned(128)));
#define g_lock (&g_lock_line[0])

/* hook events for the log: written under g_lock, read by the driver thread */
struct xv_event {
	u32 kind; /* 1 present, 2 submit, 3 close; | 0x100 = one of ours */
	u32 tag, dir, len, status, mode;
};
static struct xv_event g_ev[EVENTS];
static volatile u32 g_ev_head;
static u32 g_ev_tail, g_ev_logged, g_ev_lost;

/* stock capture packets in flight, for the R2 measurement (under g_lock) */
static u32 *g_watch_pkt[WATCH], *g_watch_ctx[WATCH], g_watch_owner[WATCH];

static struct detour d_present, d_submit, d_close;
static u32 (*pIsActiveProcess)(void);
/* XexExecutableModuleHandle (kernel 403): a variable holding the running title's module */
#define ORD_XexExecutableModuleHandle 403
static HANDLE *volatile pExeModule;
static u32 g_title_seen; /* driver thread: the title the rings were last checked against */
static u32 g_hooks_tried;
u32 g_quiesced; /* hot reload (xv_quiesce): nothing is bound or served again */
static u64 g_last_summary, g_install_retry;
static u32 g_install_deferred;

/* ---- the NV64 thunks: the kernel calls our hooks with its 64-bit r14-r31 live ---------------
 * kernel_360.h's NV64_THUNK_ASM: the same frame as thunks_common.s's NV64_THUNK, kept here so
 * this file links without a line in the shared thunk files.
 * (XV_HOST_TEST: the logic compiled natively for an offline test, without them.) */
#ifndef XV_HOST_TEST
__asm__(NV64_THUNK_ASM(XvPresentHook, XvPresentHookC)
	NV64_THUNK_ASM(XvSubmitHook, XvSubmitHookC)
	NV64_THUNK_ASM(XvCloseHook, XvCloseHookC));
#endif

u32 XvPresentHook(u32 *ctx);
long XvSubmitHook(u32 *ctx, u32 dir, u32 *pkt);
void XvCloseHook(u32 *ctx);

typedef u32 (*present_t)(u32 *ctx);
typedef long (*submit_t)(u32 *ctx, u32 dir, u32 *pkt);
typedef void (*close_t)(u32 *ctx);

/* ---- small helpers -------------------------------------------------------------------------- */

static int pad_of_tag(u32 tag)
{
	u32 i = tag - XV_TAG_BASE;

	return i < PADS ? (int)i : -1;
}

static u32 lock(void)
{
	return (u32)KfAcquireSpinLock(g_lock);
}

static void unlock(u32 irql)
{
	KfReleaseSpinLock(g_lock, irql);
}

static u32 ring_count(const struct ring *r)
{
	return r->head - r->tail;
}

static u32 now32(void)
{
	return (u32)xa_now_ms();
}

/* the running title's identity (0 while none is loaded) */
static u32 cur_title(void)
{
	return pExeModule ? (u32)*pExeModule : 0;
}

/* who is submitting: called in the submitter's context */
static u32 owner_now(void)
{
	return KeGetCurrentProcessType() == 1 /* title */ ? cur_title() : OWNER_SYSTEM;
}

/* whether memory [p, p + n) is mapped (MmIsAddressValid: a page-table walk, no locks) */
static bool mapped(const void *p, u32 n)
{
	const u8 *a = p;

	if (!a || !n)
		return a != NULL;
	for (u32 off = 0; off < n; off += 0x1000 - ((u32)(a + off) & 0xfff))
		if (!MmIsAddressValid((void *)(a + off)))
			return false;
	return MmIsAddressValid((void *)(a + n - 1));
}

/* whether a queued packet may still be written: its title is the running one, and the
 * packet and its buffer are mapped (under g_lock) */
static bool entry_ok(const struct ring *r, u32 s)
{
	u32 *pkt = r->pkt[s], owner = r->owner[s];

	if (owner != OWNER_SYSTEM && (owner == 0 || owner != cur_title()))
		return false;
	return mapped(pkt, 24) && mapped((const void *)pkt[2], pkt[3]);
}

/* under g_lock */
static void event(u32 kind, u32 tag, u32 dir, u32 len, u32 status, u32 mode)
{
	u32 h = g_ev_head;
	struct xv_event *e;

	if (g_ev_logged + (h - g_ev_tail) >= EVENTS_LOGGED || h - g_ev_tail >= EVENTS)
		return;
	e = &g_ev[h % EVENTS];
	e->kind = kind;
	e->tag = tag;
	e->dir = dir;
	e->len = len;
	e->status = status;
	e->mode = mode;
	BARRIER();
	g_ev_head = h + 1;
}

/* completes a packet: data was written before, then pkt[4], then the status (under g_lock) */
static void complete(u32 *pkt, u32 done, u32 status)
{
	BARRIER();
	pkt[1] = done;
	BARRIER();
	pkt[0] = status;
}

/* fails every queued packet of the ring (ctx == NULL) or of one ctx, and drops those whose
 * title is gone without writing them; the count failed (under g_lock) */
static u32 cancel_ring(struct ring *r, const u32 *ctx)
{
	u32 kept = 0, n = 0, count = ring_count(r);
	u32 *keep_pkt[RING], *keep_ctx[RING], keep_own[RING];

	for (u32 i = 0; i < count; i++) {
		u32 s = (r->tail + i) % RING;

		if (!entry_ok(r, s)) {
			g_xv.stale_dropped++; /* its title is gone: dropped, never written */
		} else if (!ctx || r->ctx[s] == ctx) {
			BARRIER();
			r->pkt[s][0] = ST_CANCELLED; /* status only, as 0x80101ED8 / 0x801065C8 */
			n++;
		} else {
			keep_pkt[kept] = r->pkt[s];
			keep_ctx[kept] = r->ctx[s];
			keep_own[kept++] = r->owner[s];
		}
	}
	r->tail = r->head = 0;
	for (u32 i = 0; i < kept; i++) {
		r->pkt[i] = keep_pkt[i];
		r->ctx[i] = keep_ctx[i];
		r->owner[i] = keep_own[i];
	}
	r->head = kept;
	return n;
}

/* drops (unwritten) every queued packet whose title is gone (under g_lock) */
static void purge_stale(struct ring *r)
{
	u32 kept = 0, count = ring_count(r);
	u32 *keep_pkt[RING], *keep_ctx[RING], keep_own[RING];

	for (u32 i = 0; i < count; i++) {
		u32 s = (r->tail + i) % RING;

		if (!entry_ok(r, s)) {
			g_xv.stale_dropped++;
			continue;
		}
		keep_pkt[kept] = r->pkt[s];
		keep_ctx[kept] = r->ctx[s];
		keep_own[kept++] = r->owner[s];
	}
	for (u32 i = 0; i < kept; i++) {
		r->pkt[i] = keep_pkt[i];
		r->ctx[i] = keep_ctx[i];
		r->owner[i] = keep_own[i];
	}
	r->tail = 0;
	r->head = kept;
}

/* G.711 mu-law, integer */
static u8 ulaw_encode(s16 s)
{
	s32 v = s, sign = 0, exp = 7;

	if (v < 0) {
		v = -v;
		sign = 0x80;
	}
	if (v > 32635)
		v = 32635;
	v += 0x84;
	while (exp > 0 && !(v & (0x4000 >> (7 - exp))))
		exp--;
	return (u8)~(sign | exp << 4 | ((v >> (exp + 3)) & 0x0f));
}

static s16 ulaw_decode(u8 u)
{
	s32 v;

	u = (u8)~u;
	v = ((u & 0x0f) << 3) + 0x84;
	v <<= (u >> 4) & 7;
	v -= 0x84;
	return (s16)(u & 0x80 ? -v : v);
}

static u32 fmt_bytes(u32 fmt)
{
	return fmt <= FMT_S16BE ? 2 : 1;
}

static void put_sample(u8 *b, u32 fmt, s16 v)
{
	switch (fmt) {
	case FMT_S16BE:
		b[0] = (u8)((u16)v >> 8);
		b[1] = (u8)v;
		break;
	case FMT_U8:
		b[0] = (u8)(((u16)v >> 8) ^ 0x80);
		break;
	case FMT_ULAW:
		b[0] = ulaw_encode(v);
		break;
	default:
		b[0] = (u8)v;
		b[1] = (u8)((u16)v >> 8);
	}
}

static s16 get_sample(const u8 *b, u32 fmt)
{
	switch (fmt) {
	case FMT_S16BE:
		return (s16)(b[0] << 8 | b[1]);
	case FMT_U8:
		return (s16)((b[0] ^ 0x80) << 8);
	case FMT_ULAW:
		return ulaw_decode(b[0]);
	default:
		return (s16)(b[0] | b[1] << 8);
	}
}

static u32 peak_of(s32 v)
{
	return (u32)(v < 0 ? -v : v);
}

/* ---- the hooks -------------------------------------------------------------------------------- */

HOOK u32 XvPresentHookC(u32 *ctx)
{
	u32 tag = ctx[0]; /* the kernel reads it unconditionally too */
	int pad = pad_of_tag(tag);
	u32 irql, r;

	g_xv.calls_present++;
	if (pad < 0)
		return ((present_t)d_present.trampoline)(ctx);
	irql = lock();
	g_xv.our_present++;
	g_xv.pads[pad].present_calls++;
	r = g_pad[pad].serving ? 1 : 0;
	event(0x101, tag, 0, 0, r, 0);
	unlock(irql);
	return r;
}

/* stock capture packets: completed ones since the last call of this ctx (under g_lock) */
static void watch_stock(u32 *ctx, u32 now)
{
	for (int i = 0; i < WATCH; i++) {
		u32 *p = g_watch_pkt[i];
		u32 st, done;

		if (!p || g_watch_ctx[i] != ctx)
			continue;
		if (g_watch_owner[i] != OWNER_SYSTEM &&
		    (g_watch_owner[i] != cur_title() || !g_watch_owner[i])) {
			g_watch_pkt[i] = NULL; /* from a title that is gone: never read */
			continue;
		}
		if (!mapped(p, 24)) {
			g_watch_pkt[i] = NULL;
			continue;
		}
		st = p[0];
		if (st == ST_PENDING)
			continue;
		BARRIER();
		done = p[1];
		g_xv.stock_cap_done++;
		g_xv.stock_cap_bytes += done;
		g_xv.stock_cap_last_ms = now;
		g_xv.stock_status_last = st;
		g_xv.stock_cap_mode = p[5];
		g_xv.stock_cap_flags = p[4];
		if (!g_xv.stock_first_len && st == 0 && done) {
			u8 *dst = (u8 *)g_xv.stock_first;
			const u8 *src = (const u8 *)p[2];
			u32 n = done < 32 ? done : 32;

			if (!mapped(src, n))
				n = 0;
			for (u32 k = 0; k < n; k++)
				dst[k] = src[k];
			g_xv.stock_first_status = st;
			BARRIER();
			g_xv.stock_first_len = n;
		}
		g_watch_pkt[i] = NULL;
	}
}

HOOK long XvSubmitHookC(u32 *ctx, u32 dir, u32 *pkt)
{
	u32 tag = ctx[0];
	int pad = pad_of_tag(tag);
	struct xv_pad *p;
	struct xv_pad_status *ps;
	struct ring *r;
	u32 irql, active, owner;
	long st;

	g_xv.calls_submit++;
	if (pad < 0) {
		u32 now = now32(), owner = owner_now();
		int i;

		/* stock device: the kernel's code, observed (R2) */
		irql = lock();
		g_xv.stock_tag = tag;
		watch_stock(ctx, now);
		if (dir) {
			g_xv.stock_cap_submit++;
			g_xv.stock_cap_len = pkt[3];
			if (!g_xv.stock_cap_first_ms)
				g_xv.stock_cap_first_ms = now;
		} else {
			g_xv.stock_ren_submit++;
			g_xv.stock_ren_len = pkt[3];
			g_xv.stock_ren_bytes += pkt[3];
			g_xv.stock_ren_mode = pkt[5];
			g_xv.stock_ren_flags = pkt[4];
			if (!g_xv.stock_ren_first_ms)
				g_xv.stock_ren_first_ms = now;
			g_xv.stock_ren_last_ms = now;
		}
		unlock(irql);
		st = ((submit_t)d_submit.trampoline)(ctx, dir, pkt);
		irql = lock();
		if (dir && st == 0 && pkt[0] == ST_PENDING) {
			for (i = 0; i < WATCH && g_watch_pkt[i]; i++)
				;
			if (i < WATCH) {
				g_watch_pkt[i] = pkt;
				g_watch_ctx[i] = ctx;
				g_watch_owner[i] = owner;
			}
		}
		event(2, tag, dir, pkt[3], (u32)st, pkt[5]);
		unlock(irql);
		return st;
	}

	p = &g_pad[pad];
	ps = &g_xv.pads[pad];
	g_xv.our_submit++;
	/* the kernel's order: device and present byte first, then the active process */
	if (!p->serving) {
		irql = lock();
		event(0x102, tag, dir, pkt[3], ST_NO_HEADSET, pkt[5]);
		unlock(irql);
		return (long)ST_NO_HEADSET;
	}
	active = pIsActiveProcess ? pIsActiveProcess() : 1;
	owner = owner_now();
	irql = lock();
	if (!p->serving) {
		st = (long)ST_NO_HEADSET;
	} else if (owner == 0) {
		st = (long)ST_NO_HEADSET; /* a title process with no title module: switching */
	} else if (!active) {
		/* 0x801021E0: completed at once, full length, capture "busy" */
		ps->inactive++;
		pkt[4] = 0;
		complete(pkt, pkt[3], dir ? ST_INACTIVE_CAPTURE : 0);
		st = 0;
	} else {
		r = dir ? &p->cap : &p->ren;
		if (ring_count(r) >= RING) {
			if (dir)
				ps->cap_full++;
			else
				ps->ren_full++;
			st = (long)ST_NO_NODE;
		} else {
			pkt[0] = ST_PENDING;
			pkt[1] = 0;
			pkt[4] = dir ? g_xv.cap_flags : 0;
			r->pkt[r->head % RING] = pkt;
			r->ctx[r->head % RING] = ctx;
			r->owner[r->head % RING] = owner;
			r->head++;
			if (dir) {
				ps->cap_submit++;
				ps->cap_len = pkt[3];
				ps->cap_mode = pkt[5];
				ps->cap_queued = ring_count(r);
			} else {
				ps->ren_submit++;
				ps->ren_len = pkt[3];
				ps->ren_mode = pkt[5];
				ps->ren_queued = ring_count(r);
			}
			st = 0;
		}
	}
	event(0x102, tag, dir, pkt[3], (u32)st, pkt[5]);
	unlock(irql);
	return st;
}

HOOK void XvCloseHookC(u32 *ctx)
{
	u32 tag = ctx[0];
	int pad = pad_of_tag(tag);
	u32 irql, n = 0;

	g_xv.calls_close++;
	/* our packets of this ctx, whatever its tag says now: XAM zeroes ctx[0] when the
	 * headset is unbound, while packets may still be queued here */
	irql = lock();
	for (int i = 0; i < PADS; i++) {
		u32 c = cancel_ring(&g_pad[i].cap, ctx) + cancel_ring(&g_pad[i].ren, ctx);

		g_xv.pads[i].cancelled += c;
		g_xv.pads[i].cap_queued = ring_count(&g_pad[i].cap);
		g_xv.pads[i].ren_queued = ring_count(&g_pad[i].ren);
		n += c;
	}
	for (int i = 0; i < WATCH; i++)
		if (g_watch_ctx[i] == ctx)
			g_watch_pkt[i] = NULL;
	if (pad >= 0) {
		g_xv.our_close++;
		g_xv.pads[pad].close_calls++;
	} else {
		g_xv.stock_close++;
	}
	event(pad >= 0 ? 0x103 : 3, tag, 0, n, 0, 0);
	unlock(irql);
	/* ours: nothing of XAM's nodes or event is used, nothing to wait for */
	if (pad < 0)
		((close_t)d_close.trampoline)(ctx);
}

/* ---- installing the hooks (driver thread, once stage >= 1) ------------------------------ */

static bool code_is(u32 addr, const u32 *sig)
{
	const volatile u32 *c = (const volatile u32 *)addr;

	return c[0] == sig[0] && c[1] == sig[1] && c[2] == sig[2] && c[3] == sig[3];
}

/* XAM's eight voice contexts (XamVoiceCreate 0x816FC098: 0x81AACA28 + (user + 4 * not
 * title) * 56; +8 = its node array, non-zero from create to close). Patching XVoiced while
 * a thread is inside one of its first four instructions would crash that thread, so the
 * hooks only go in while no context is open, i.e. nothing is using voice. 1 = in use,
 * 0 = idle, -1 = not the XAM we read (no hooks then). */
#define XAM_VOICE_CTX 0x81AACA28
#define XAM_VOICE_CTX_SIZE 56
static int voice_in_use(void)
{
	const volatile u32 *code = (const volatile u32 *)0x816FC0C0;

	/* lis r10, 0x81AB / addi r10, r10, -0x35D8 in XamVoiceCreate */
	if (code[0] != 0x3d4081ab || code[2] != 0x394aca28)
		return -1;
	for (int i = 0; i < 8; i++)
		if (((const volatile u32 *)(XAM_VOICE_CTX + i * XAM_VOICE_CTX_SIZE))[2])
			return 1;
	return 0;
}

static void install_hooks(void)
{
	HANDLE krnl;
	void *a_present = 0, *a_submit = 0, *a_close = 0, *a_active = 0;
	int err = 0;

	g_hooks_tried = 1;
	xa_log("voice: installing hooks with title %08x running (do this from the dashboard or "
	       "Aurora, never while a game uses voice)\n", cur_title());
	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0) {
		err = -XA_ENODEV;
		goto out;
	}
	XexGetProcedureAddress(krnl, ORD_XVoicedHeadsetPresent, &a_present);
	XexGetProcedureAddress(krnl, ORD_XVoicedSubmitPacket, &a_submit);
	XexGetProcedureAddress(krnl, ORD_XVoicedClose, &a_close);
	XexGetProcedureAddress(krnl, ORD_XVoicedIsActiveProcess, &a_active);
	xa_log("voice: XVoiced present %08x submit %08x close %08x active %08x\n", (u32)a_present,
	       (u32)a_submit, (u32)a_close, (u32)a_active);
	if ((u32)a_present != ADDR_XVoicedHeadsetPresent || (u32)a_submit != ADDR_XVoicedSubmitPacket ||
	    (u32)a_close != ADDR_XVoicedClose || (u32)a_active != ADDR_XVoicedIsActiveProcess) {
		err = -XA_ENODEV;
		goto out;
	}
	/* a function an earlier build of this plugin patched (hot reload) starts with that
	 * build's jump: it checked the code then, and detour_install adopts its hook */
	if ((!code_is(ADDR_XVoicedHeadsetPresent, SIG_PRESENT) &&
	     !detour_known((void *)ADDR_XVoicedHeadsetPresent)) ||
	    (!code_is(ADDR_XVoicedSubmitPacket, SIG_SUBMIT) &&
	     !detour_known((void *)ADDR_XVoicedSubmitPacket)) ||
	    (!code_is(ADDR_XVoicedClose, SIG_CLOSE) && !detour_known((void *)ADDR_XVoicedClose)) ||
	    !code_is(ADDR_XVoicedIsActiveProcess, SIG_ACTIVE)) {
		xa_log("voice: XVoiced code is not the 17559 code read offline: no hooks\n");
		err = -XA_EINVAL;
		goto out;
	}
	pIsActiveProcess = a_active;
	/* Close and Submit first: nothing can reach them with our tags before a category-1
	 * bind, but the order costs nothing. A failure leaves the ones already in place
	 * (they only pass through until all three are in) rather than unpatching under a
	 * possible caller. */
	err = detour_install(&d_close, "XVoicedClose", a_close, (void *)XvCloseHook);
	if (!err)
		g_xv.hooks |= HOOK_CLOSE;
	if (!err)
		err = detour_install(&d_submit, "XVoicedSubmitPacket", a_submit, (void *)XvSubmitHook);
	if (!err)
		g_xv.hooks |= HOOK_SUBMIT;
	if (!err)
		err = detour_install(&d_present, "XVoicedHeadsetPresent", a_present,
				     (void *)XvPresentHook);
	if (!err)
		g_xv.hooks |= HOOK_PRESENT;
out:
	if (err) {
		g_xv.hooks |= HOOK_FAILED;
		g_xv.hook_error = (u32)err;
	}
	xa_log("voice: hooks %x (error %d)\n", g_xv.hooks, err);
}

/* hot reload: all three hooks are an earlier build's, to be adopted (one store each, no
 * patching): that is safe at any time, even while a title uses voice, and it should happen
 * at once, as until then the earlier build's hooks answer "no headset" */
static bool hooks_inherited(void)
{
	return detour_known((void *)ADDR_XVoicedHeadsetPresent) &&
	       detour_known((void *)ADDR_XVoicedSubmitPacket) &&
	       detour_known((void *)ADDR_XVoicedClose);
}

static bool hooks_ready(void)
{
	return (g_xv.hooks & (HOOK_ALL | HOOK_FAILED)) == HOOK_ALL;
}

/* ---- category-1 bind (XAM thread, through xinput_360.c's queue) ------------------------- */

#ifdef XA_XINPUT_HEADSET
static bool pad_active(int i)
{
	return xa_xinput_active(i);
}

static int bind_headset(int i, bool connect)
{
	return xa_xinput_bind_headset(i, connect);
}
#else
/* without xinput_360's headset requests the headset is never bound (stage 3/4 stay off) */
static bool pad_active(int i)
{
	return false;
}

static int bind_headset(int i, bool connect)
{
	return -1;
}
#endif

/* ---- packet service (driver thread) ---------------------------------------------------- */

static void reset_pad_dsp(struct xv_pad *p)
{
	rs_init(&p->rs_mic, p->rate_in, g_xv.applied_cap_rate);
	rs_init(&p->rs_loop, p->rate_out, g_xv.applied_cap_rate);
	rs_init(&p->rs_ear, g_xv.applied_ren_rate, p->rate_out);
	p->ef_n = p->ef_pos = 0;
}

/* converted samples into the capture packets of pad i; the rest is dropped */
static void cap_write(int i, const s16 *s, int n)
{
	struct xv_pad *p = &g_pad[i];
	struct xv_pad_status *ps = &g_xv.pads[i];
	u32 fmt = g_xv.applied_cap_fmt, bps = fmt_bytes(fmt), irql;

	irql = lock();
	while (n > 0 && p->serving && ring_count(&p->cap)) {
		u32 slot = p->cap.tail % RING;
		u32 *pkt = p->cap.pkt[slot];
		u8 *buf;
		u32 len, done, start;

		if (!entry_ok(&p->cap, slot)) {
			p->cap.tail++; /* its title is gone: dropped, never written */
			g_xv.stale_dropped++;
			continue;
		}
		buf = (u8 *)pkt[2];
		len = pkt[3];
		done = start = pkt[1];

		while (n > 0 && done + bps <= len) {
			put_sample(buf + done, fmt, *s++);
			done += bps;
			n--;
		}
		if (done < len && done + bps > len)
			buf[done++] = 0; /* an odd tail byte */
		ps->cap_bytes += done - start;
		if (done >= len) {
			u32 now = now32();

			complete(pkt, done, 0);
			p->cap.tail++;
			ps->cap_done++;
			if (!ps->cap_first_ms)
				ps->cap_first_ms = now;
			ps->cap_last_ms = now;
		} else {
			BARRIER();
			pkt[1] = done; /* progress, as the kernel's per-chunk completion shows it */
		}
	}
	ps->cap_queued = ring_count(&p->cap);
	unlock(irql);
	ps->mic_dropped += (u32)n;
}

/* s16le frames (ch 1 or 2) through resampler r into the capture packets */
void mic_feed(int i, struct rs *r, const u8 *d, int frames, u32 ch)
{
	struct xv_pad_status *ps = &g_xv.pads[i];
	s16 out[256 + 16];
	int n = 0;

	for (int f = 0; f < frames; f++, d += 2 * ch) {
		s32 x = (s16)get_le16(d);

		if (ch == 2)
			x = (x + (s16)get_le16(d + 2)) >> 1;
		if (peak_of(x) > ps->mic_peak)
			ps->mic_peak = peak_of(x);
		n += rs_push(r, x, out + n);
		if (n >= 256) {
			cap_write(i, out, n);
			n = 0;
		}
	}
	if (n)
		cap_write(i, out, n);
}

/* silence into the capture packets: one 8 ms block at the capture rate */
static void cap_silence(int i)
{
	static const s16 zeros[48000 / 125]; /* 384: the largest block */
	int n = (int)(g_xv.applied_cap_rate / 125);

	cap_write(i, zeros, n);
}

/* the next 360-side render sample of pad i into *v, completing drained packets;
 * false = nothing queued (an underrun: the caller plays silence). Under g_lock. */
static bool ren_next(int i, s32 *v)
{
	struct xv_pad *p = &g_pad[i];
	struct xv_pad_status *ps = &g_xv.pads[i];
	u32 fmt = g_xv.applied_ren_fmt, bps = fmt_bytes(fmt);

	while (p->serving && ring_count(&p->ren)) {
		u32 slot = p->ren.tail % RING;
		u32 *pkt = p->ren.pkt[slot];
		u32 len, done, start;
		bool got = false;

		if (!entry_ok(&p->ren, slot)) {
			p->ren.tail++;
			g_xv.stale_dropped++;
			continue;
		}
		len = pkt[3];
		done = start = pkt[1];

		if (done + bps <= len) {
			*v = get_sample((const u8 *)pkt[2] + done, fmt);
			done += bps;
			got = true;
		}
		if (done + bps > len) {
			/* drained (an odd tail byte counts as played) */
			ps->ren_bytes += len - start;
			complete(pkt, len, 0);
			p->ren.tail++;
			ps->ren_done++;
			ps->ren_queued = ring_count(&p->ren);
		} else {
			ps->ren_bytes += done - start;
			pkt[1] = done;
		}
		if (got)
			return true;
	}
	return false;
}

/* one earpiece block: len bytes of s16le at rate_out x ch_out from the render packets */
static void ear_fill(int i, u8 *out, int len)
{
	struct xv_pad *p = &g_pad[i];
	struct xv_pad_status *ps = &g_xv.pads[i];
	u32 ch = p->ch_out, frame = 2 * ch, irql;
	int frames = len / (int)frame, f;
	u32 under = 0;

	irql = lock();
	for (f = 0; f < frames; f++) {
		s16 v;

		while (p->ef_pos >= p->ef_n) {
			s32 x = 0;

			if (!ren_next(i, &x))
				under++;
			p->ef_n = rs_push(&p->rs_ear, x, p->ef);
			p->ef_pos = 0;
		}
		v = p->ef[p->ef_pos++];
		if (peak_of(v) > ps->ear_peak)
			ps->ear_peak = peak_of(v);
		put_le16(out + f * frame, (u16)v);
		if (ch == 2)
			put_le16(out + f * frame + 2, (u16)v);
	}
	unlock(irql);
	memset(out + frames * frame, 0, (size_t)(len - frames * (int)frame));
	ps->ear_underrun += under;
}

/* loopback: an earpiece block (s16le, rate_out x ch_out) back into the capture packets */
static void loop_feed(int i, const u8 *d, int len)
{
	struct xv_pad *p = &g_pad[i];
	int frames = len / (int)(2 * p->ch_out);

	g_xv.pads[i].loop_samples += (u32)frames;
	mic_feed(i, &p->rs_loop, d, frames, p->ch_out);
}

/* the internal 8 ms clock (stage 3, and stage 4 for a direction USB isn't pacing) */
static void vclock(int i, u64 now)
{
	static u8 scratch[48000 / 125 * 4]; /* 8 ms of 48 kHz stereo */
	struct xv_pad *p = &g_pad[i];
	bool pcm = g_xv.applied_stage >= 4;

	if (!p->vclock || now - p->vclock > 8 * BLOCK_MS)
		p->vclock = now; /* start, or too far behind (the thread was held up): resync */
	for (int n = 0; n < 8 && now - p->vclock >= BLOCK_MS; n++) {
		bool ear_usb = pcm && now - p->last_ear_ms < USB_STALE_MS;
		bool mic_usb = pcm && now - p->last_mic_ms < USB_STALE_MS;

		p->vclock += BLOCK_MS;
		g_xv.pads[i].vticks++;
		if (!ear_usb) {
			int len = (int)(p->rate_out / 125 * p->ch_out * 2);

			ear_fill(i, scratch, len);
			if (g_xv.loopback)
				loop_feed(i, scratch, len);
		}
		if (!g_xv.loopback && !mic_usb)
			cap_silence(i);
	}
}

/* stops serving pad i's headset, fails its packets and queues the category-1 unbind */
static void unbind_now(int i)
{
	struct xv_pad *p = &g_pad[i];
	struct xv_pad_status *ps = &g_xv.pads[i];
	u32 irql = lock(), n;

	p->serving = 0; /* the hooks say "no headset" from here on */
	n = cancel_ring(&p->cap, NULL) + cancel_ring(&p->ren, NULL);
	ps->cancelled += n;
	ps->cap_queued = ps->ren_queued = 0;
	unlock(irql);
	bind_headset(i, false);
	p->bound = 0;
	ps->unbind_requests++;
	ps->bound = 0;
	ps->serving = 0;
	xa_log("voice: pad %d: headset unbound (%u packets failed)\n", i, n);
}

/* binds or unbinds pad i's headset to match the stage, the USB state and g_xv.force */
static void reconcile(int i)
{
	struct xv_pad *p = &g_pad[i];
	struct xv_pad_status *ps = &g_xv.pads[i];
	bool forced = (g_xv.force >> i) & 1;
	bool want = !g_quiesced && g_xv.applied_stage >= 3 && hooks_ready() &&
		    (p->usb_present || forced) && pad_active(i);
	u64 now = xa_now_ms();

	ps->forced = forced;
	if (want && !p->bound) {
		if (now < p->bind_retry_ms)
			return;
		if (!p->usb_present) {
			/* forced (V1 without a headset): the headset GIP would report */
			p->rate_in = p->rate_out = 24000;
			p->ch_in = 1;
			p->ch_out = 2;
		}
		reset_pad_dsp(p);
		{
			u32 irql = lock();

			p->serving = 1; /* before XAM tells anyone the tag has a headset */
			unlock(irql);
		}
		if (bind_headset(i, true) < 0) {
			u32 irql = lock();

			p->serving = 0;
			unlock(irql);
			ps->bind_errors++;
			p->bind_retry_ms = now + 1000;
			xa_log("voice: pad %d: headset bind not queued\n", i);
			return;
		}
		p->bound = 1;
		p->vclock = 0;
		ps->bind_requests++;
		xa_log("voice: pad %d: headset bound (%s, mic %u Hz x%u, earpiece %u Hz x%u)\n", i,
		       p->usb_present ? "jack" : "forced", p->rate_in, p->ch_in, p->rate_out,
		       p->ch_out);
	} else if (!want && p->bound) {
		unbind_now(i);
	}
	ps->bound = p->bound;
	ps->serving = p->serving;
}

/* validates and applies the rate/format controls */
static void apply_config(void)
{
	u32 cr = g_xv.cap_rate, cf = g_xv.cap_fmt, rr = g_xv.ren_rate, rf = g_xv.ren_fmt;

	if (cr == g_xv.applied_cap_rate && cf == g_xv.applied_cap_fmt &&
	    rr == g_xv.applied_ren_rate && rf == g_xv.applied_ren_fmt)
		return;
	if (!rate_ok(cr) || !rate_ok(rr) || cf >= FMT_COUNT || rf >= FMT_COUNT) {
		xa_log("voice: rejected capture %u Hz fmt %u, render %u Hz fmt %u (rates 4000..48000 "
		       "in steps of 1000, formats 0..3)\n", cr, cf, rr, rf);
		g_xv.cap_rate = g_xv.applied_cap_rate;
		g_xv.cap_fmt = g_xv.applied_cap_fmt;
		g_xv.ren_rate = g_xv.applied_ren_rate;
		g_xv.ren_fmt = g_xv.applied_ren_fmt;
		return;
	}
	g_xv.applied_cap_rate = cr;
	g_xv.applied_cap_fmt = cf;
	g_xv.applied_ren_rate = rr;
	g_xv.applied_ren_fmt = rf;
	for (int i = 0; i < PADS; i++)
		if (g_pad[i].rate_in)
			reset_pad_dsp(&g_pad[i]);
	xa_log("voice: 360 side: capture %u Hz fmt %u, render %u Hz fmt %u\n", cr, cf, rr, rf);
}

/* a title switch: every packet of the old title is dropped unwritten (entry_ok also checks
 * each packet before every write), and stock packets watched from it are forgotten */
static void check_title(void)
{
	u32 t = cur_title(), irql;

	g_xv.title = t;
	if (t == g_title_seen)
		return;
	irql = lock();
	for (int i = 0; i < PADS; i++) {
		purge_stale(&g_pad[i].cap);
		purge_stale(&g_pad[i].ren);
		g_xv.pads[i].cap_queued = ring_count(&g_pad[i].cap);
		g_xv.pads[i].ren_queued = ring_count(&g_pad[i].ren);
	}
	for (int i = 0; i < WATCH; i++)
		if (g_watch_owner[i] != OWNER_SYSTEM)
			g_watch_pkt[i] = NULL;
	unlock(irql);
	xa_log("voice: title %08x -> %08x (%u stale packets dropped so far)\n", g_title_seen, t,
	       g_xv.stale_dropped);
	g_title_seen = t;
	g_xv.title_switches++;
}

static void log_events(void)
{
	static const char *const kinds[] = { "?", "present", "submit", "close" };

	while (g_ev_tail != g_ev_head) {
		struct xv_event e;

		BARRIER();
		e = g_ev[g_ev_tail % EVENTS];
		g_ev_tail++;
		g_ev_logged++;
		xa_log("voice: %s %s tag %08x dir %u len %u -> %08x mode %u\n", kinds[e.kind & 3],
		       e.kind & 0x100 ? "(ours)" : "(stock)", e.tag, e.dir, e.len, e.status, e.mode);
		if (g_ev_logged == EVENTS_LOGGED)
			xa_log("voice: further calls are only counted (voice.py status)\n");
	}
}

/* ---- the interface (voice_360.h) -------------------------------------------------------------- */

void xv_init(void)
{
	g_xv.log_addr = (u32)&xa_log_ring;
	{
		HANDLE krnl;
		void *p = 0;

		if (XexGetModuleHandle("xboxkrnl.exe", &krnl) >= 0)
			XexGetProcedureAddress(krnl, ORD_XexExecutableModuleHandle, &p);
		pExeModule = p;
		g_title_seen = cur_title();
		g_xv.title = g_title_seen;
	}
	g_xv.applied_cap_rate = 8000;
	g_xv.applied_ren_rate = 8000;
	g_xv.applied_cap_fmt = g_xv.applied_ren_fmt = FMT_S16LE;
	/* stage 4 by default: let the boot and the dashboard settle before patching XVoiced */
	g_install_retry = xa_now_ms() + 20000;
	apply_config();
#ifndef XA_XINPUT_HEADSET
	g_xv.hooks |= HOOK_NO_BIND;
#endif
	xa_log("voice: ready, stage %u (voice.py stage N)\n", g_xv.stage);
	xg_init();
}

void xv_headset_changed(int pad, bool present, u32 rate_in, u32 ch_in, u32 rate_out,
			u32 ch_out)
{
	struct xv_pad *p;
	struct xv_pad_status *ps;

	if (pad < 0 || pad >= PADS)
		return;
	p = &g_pad[pad];
	ps = &g_xv.pads[pad];
	if (present) {
		if (!rate_ok(rate_in) || ch_in < 1 || ch_in > 2 || !rate_ok(rate_out) || ch_out < 1 ||
		    ch_out > 2) {
			xa_log("voice: pad %d: headset format %u x%u / %u x%u not supported\n", pad,
			       rate_in, ch_in, rate_out, ch_out);
			present = false;
		} else {
			if (p->bound && (p->rate_in != rate_in || p->ch_in != ch_in ||
					 p->rate_out != rate_out || p->ch_out != ch_out)) {
				/* new format while bound: stop serving through the change */
				p->usb_present = 0;
				reconcile(pad);
			}
			p->rate_in = rate_in;
			p->ch_in = ch_in;
			p->rate_out = rate_out;
			p->ch_out = ch_out;
		}
	}
	p->usb_present = present;
	if (present)
		hs_format(pad); /* the sidetone follows the (new) format */
	else
		hs_headset_lost(pad);
	ps->usb_present = present;
	ps->rate_in = p->rate_in;
	ps->ch_in = p->ch_in;
	ps->rate_out = p->rate_out;
	ps->ch_out = p->ch_out;
	xa_log("voice: pad %d: headset %s\n", pad, present ? "present" : "gone");
	/* now, not on the next poll: an unbind must be queued before the pad's own */
	reconcile(pad);
}

/* the pad is about to be unbound (xw_plugin.c's pad_release, before xa_xinput_unbind):
 * its headset goes first, whatever kept it bound (USB or g_xv.force). Not in voice_360.h
 * yet: see the report. */
void xv_pad_releasing(int pad)
{
	if (pad < 0 || pad >= PADS)
		return;
	hs_headset_lost(pad);
	if (!g_pad[pad].bound)
		return;
	unbind_now(pad); /* the pad is then inactive, so reconcile won't bind it again */
}

/* Hot reload (docs/HOT-RELOAD.md), driver thread, before the pads' unbinds are waited for:
 * every headset still bound (the jack's, or voice.py's force mask) is unbound and its queued
 * packets are failed, and nothing is bound again. The three XVoiced hooks stay exactly where
 * they are and keep answering "no headset" for our tags, as they do below stage 3: our tags
 * must never reach the kernel's own code (see the top of this file), so the shield only ends
 * when the next build takes the hooks' slots over (detour.h), one store each. Everything the
 * hooks touch (g_pad, the lock, the event ring) stays valid: this image is never unloaded. */
void xv_quiesce(void)
{
	g_quiesced = 1;
	for (int i = 0; i < PADS; i++) {
		hs_headset_lost(i);
		g_pad[i].usb_present = 0;
		g_xv.pads[i].usb_present = 0;
		if (g_pad[i].bound)
			unbind_now(i);
		else if (g_pad[i].serving) { /* never: serving is only set with bound */
			u32 irql = lock();

			g_pad[i].serving = 0;
			unlock(irql);
		}
	}
	xd_mictest_quiesce(); /* the headsets' loss above ended any test */
	xa_log("voice: quiesced (hooks %x stay in place and answer \"no headset\" for our tags)\n",
	       g_xv.hooks);
}

/* a refused hot reload: back to normal (headsets are bound again as they are announced) */
void xv_resume(void)
{
	g_quiesced = 0;
	xa_log("voice: resumed\n");
}

static const u8 g_silence[64]; /* -fno-zero-initialized-in-bss keeps it in .data */

/* ---- the 360-rate capture path ------------------------------------------------------------- */

/* voice_priv.h: MIC-AUTO's decimator delivers the mic here, already at the capture rate */
void xv_cap_feed(int pad, const s16 *s, int n)
{
	cap_write(pad, s, n);
}

/* ---- the mic and the earpiece, in the frozen order (docs/ROADMAP.md 2.4) --------------------- */

void xv_mic_samples(int pad, const u8 *data, int len)
{
	struct xv_pad *p;

	if (pad < 0 || pad >= PADS || len <= 0)
		return;
	p = &g_pad[pad];
	g_xv.pads[pad].mic_calls++;
	g_xv.pads[pad].mic_bytes += (u32)len;
	if (!hs_format_ok(p))
		return;
	/* XD_CHUNK frames at a time, cleaned (mono, DC removed: mic_clean), through every
	 * consumer in turn; a mic packet is 1 ms (24 frames at 24 kHz): one chunk */
	for (int frames = len / (int)(2 * p->ch_in); frames > 0;) {
		int n = frames < XD_CHUNK ? frames : XD_CHUNK;
		u8 mono[2 * XD_CHUNK];

		mic_clean(pad, data, n, p->ch_in, mono);
		data += n * 2 * (int)p->ch_in;
		frames -= n;
		mic_analyse(pad, mono, n);                  /* gate, meter */
		st_mic(pad, mono, n, 1);                    /* software sidetone */
		xt_mic(pad, mono, 2 * n, p->rate_in, 1, xd_gate_open(pad)); /* team voice */
		mt_record(pad, mono, n);                    /* the mic test */
		if (g_xv.applied_stage < 4 || !p->serving || g_xv.loopback)
			continue;
		p->last_mic_ms = xa_now_ms();
		if (g_mic_mute[pad] || !xt_mic_to_game())
			/* muted (or MIC TO TEAM): the same amount of silence, so capture packets
			 * keep completing on time */
			mic_to_360(pad, g_silence, n);
		else
			mic_shaped(pad, mono, n);
	}
}

void xv_earpiece_fill(int pad, u8 *out, int len)
{
	struct xv_pad *p;

	if (len <= 0)
		return;
	if (pad < 0 || pad >= PADS) {
		memset(out, 0, (size_t)len);
		return;
	}
	p = &g_pad[pad];
	g_xv.pads[pad].ear_calls++;
	if (g_xv.applied_stage < 4 || !p->serving) {
		memset(out, 0, (size_t)len);
	} else {
		p->last_ear_ms = xa_now_ms();
		ear_fill(pad, out, len);
		/* loopback before the mix: the 360 gets back what it sent, not our sidetone */
		if (g_xv.loopback)
			loop_feed(pad, out, len);
	}
	if (hs_format_ok(p)) {
		/* after the loopback: the 360 gets back what it sent, never the game or our gains.
		 * earpiece = master x (chat x chat level + game x game level) + master x
		 * (sidetone x mic, the centre tick); every gain <= 1, every sum clamped */
		scale(out, len, xg_chat_gain());
		xg_hold(tone_active(pad)); /* the test tone takes game audio's place */
		xg_mix(pad, out, len, p->rate_out, p->ch_out);
		xt_mix(pad, out, len, p->rate_out, p->ch_out, xg_chat_gain() * xt_level() / 10);
		tone_mix(pad, out, len, xg_master_gain());
		st_mix(pad, out, len, xg_master_gain());
		tick_mix(pad, out, len, xg_master_gain());
		mt_mix(pad, out, len, xg_master_gain()); /* the mic test's playback */
		xq_process(pad, out, len, p->rate_out, p->ch_out); /* EQ over the whole mix */
		/* (reserved: a limiter / night mode goes here, after the EQ, before the scan) */
		/* something to play in this block? (dev_quiet: the amplifier follows) */
		for (int i = 0; i + 1 < len; i += 2) {
			s32 s = (s16)get_le16(out + i);

			if (s > QUIET_PEAK || s < -QUIET_PEAK) {
				g_ear_loud_ms[pad] = xa_now_ms();
				break;
			}
		}
	}
}

void xv_poll(void)
{
	u32 tb_work = xd_timebase(); /* diag: this pass's work (xd_time_work) */
	u64 now = xa_now_ms();
	u32 stage = g_xv.stage;

	g_xv.heartbeat++;
	g_xv.now_ms = (u32)now;
	xg_poll();
	xd_poll();
	if (stage > 4)
		stage = 4;
	if (stage >= 1 && !g_hooks_tried && (now >= g_install_retry || hooks_inherited())) {
		/* here, on the driver thread, never at boot; only while nothing uses voice
		 * (unless nothing is patched: hooks_inherited) */
		int use = hooks_inherited() ? 0 : voice_in_use();

		if (use == 0) {
			install_hooks();
		} else if (use < 0) {
			g_hooks_tried = 1;
			g_xv.hooks |= HOOK_FAILED;
			g_xv.hook_error = (u32)-XA_EINVAL;
			xa_log("voice: XamVoiceCreate is not the 17559 code read offline: no hooks\n");
		} else {
			if (!g_install_deferred++)
				xa_log("voice: a voice context is open (a title uses voice): hooks deferred "
				       "until it closes\n");
			g_install_retry = now + 1000;
		}
	}
	check_title();
	if (stage != g_xv.applied_stage) {
		xa_log("voice: stage %u -> %u%s\n", g_xv.applied_stage, stage,
		       stage >= 3 && !hooks_ready() ? " (hooks not ready: no headset is bound)" : "");
		g_xv.applied_stage = stage;
	}
	apply_config();
	for (int i = 0; i < PADS; i++) {
		reconcile(i);
		if (g_pad[i].serving)
			vclock(i, now);
	}
	log_events();
	if (g_ev_head - g_ev_tail > EVENTS)
		g_ev_lost++;
	if (g_xv.applied_stage && now - g_last_summary >= 10000) {
		struct xv_pad_status *ps = &g_xv.pads[0];

		g_last_summary = now;
		if (g_xv.calls_submit || g_xv.calls_present)
			xa_log("voice: calls present %u submit %u close %u (ours %u/%u/%u); pad 0: "
			       "capture %u done %u bytes %u, render %u done %u bytes %u, dropped %u, "
			       "underrun %u\n", g_xv.calls_present, g_xv.calls_submit, g_xv.calls_close,
			       g_xv.our_present, g_xv.our_submit, g_xv.our_close, ps->cap_submit,
			       ps->cap_done, ps->cap_bytes, ps->ren_submit, ps->ren_done,
			       ps->ren_bytes, ps->mic_dropped, ps->ear_underrun);
	}
	xd_time_work(tb_work);
}
