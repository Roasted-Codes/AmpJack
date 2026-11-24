// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Rumble (rumble_360.h): each pass, per pad, the title's request (xinput_360.c's
 * XamInputSetState hook) and the system's Vibration setting go through xr_step, and the
 * result to the controller (gip_set_rumble). Driver thread only; a pass is a few loads and
 * compares per pad.
 *
 * xbdm block "XWRB" (rumble.py, accept/rumble.py). Gate `enable`, ON by default: this is the
 * console's own behaviour (a 360 pad honours the setting), not an optional feature; 0 brings
 * back the old path (the title's levels as asked, nothing forced) for comparison.
 * `test` stands in for a title, so the path can be checked with no game running: write
 * XR_TEST_GO | pad << 24 | left << 8 | right (0..100); the driver thread takes it (the word
 * goes back to 0) and handles it exactly like a XamInputSetState call, the setting included.
 * A test level stops by itself after XR_TEST_MS.
 */
#include "../core/gip.h"
#include "../core/platform.h"
#include "rumble_360.h"
#include "xinput_360.h"

#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#define XR_TEST_GO 0x10000u
#define PADS 4

/* All u32, in this order: rumble.py FIELDS mirrors it. Append only (24 words at most). The
 * per-pad bytes are packed pad 0 in the top byte. */
struct xr_status {
	u32 magic, version;
	volatile u32 enable;    /* the gate: 1 on (default), 0 = the old path */
	volatile u32 test;      /* write a request (above); 0 once taken */
	u32 table;              /* XAM's Vibration table, 0 = not recognised (setting not applied) */
	u32 vib;                /* per pad: 0xFF not known / no pad, else XAM user << 4 | 1 on / 0 off */
	u32 req_l, req_r;       /* per pad: the last request, 0..100 */
	u32 sent_l, sent_r;     /* per pad: what the controller was last sent (0xFF: a re-send due) */
	u32 requests;           /* requests taken (titles' and tests') */
	u32 zeroed;             /* non-zero requests silenced by the setting */
	u32 stop_asked;         /* running motors stopped: the title asked */
	u32 stop_setting;       /* ... the setting was switched off */
	u32 stop_title;         /* ... the title changed */
	u32 stop_test;          /* ... a test request timed out */
	u32 restops;            /* stops sent a second time */
	u32 last_why;           /* XR_WHY_* of the last stop */
	u32 last_stop_ms;       /* when (xa_now_ms, low 32 bits) */
	u32 now_ms;             /* now, for the age of the above */
	u32 stop_lat_ms;        /* last stop: decision to the zero going out to the controller */
	u32 stop_lat_max_ms;
	u32 title;              /* the running title as the driver thread sees it */
	u32 tests;              /* test requests taken */
};
_Static_assert(sizeof(struct xr_status) == 24 * 4, "rumble.py FIELDS");

__attribute__((used)) struct xr_status g_xr = {
	.magic = XR_MAGIC,
	.version = 1,
	.enable = 1,
	.vib = 0xFFFFFFFFu,
};

static struct xr_pad g_pad[PADS];
static u32 g_stop_at[PADS];   /* a stop not yet seen going out (xa_now_ms + 1; 0 none) */

static void put_byte(u32 *w, int pad, u32 v)
{
	u32 sh = (u32)(3 - pad) * 8;

	*w = (*w & ~(0xFFu << sh)) | (v & 0xFF) << sh;
}

void xr_pad_reset(int pad)
{
	if (pad < 0 || pad >= PADS)
		return;
	xr_reset(&g_pad[pad]);
	g_stop_at[pad] = 0;
}

void xr_service(int pad, struct gip_client *gc, u32 title)
{
	struct xr_pad *s;
	struct xr_in in;
	struct xr_out o;
	u32 now = (u32)xa_now_ms(), t;

	if (pad < 0 || pad >= PADS || !gc)
		return;
	s = &g_pad[pad];
	in.now = now;
	in.title = title;
	in.gate = g_xr.enable != 0;
	in.vib = xa_xinput_vibration(pad);
	in.test = 0;
	in.l = in.r = 0;
	in.took = xa_xinput_take_rumble(pad, &in.l, &in.r);
	t = g_xr.test;
	if (!in.took && (t & XR_TEST_GO) && (int)(t >> 24 & 3) == pad) {
		in.took = in.test = 1;
		in.l = (u8)MIN(t >> 8 & 0xFF, 100);
		in.r = (u8)MIN(t & 0xFF, 100);
		g_xr.test = 0;
		g_xr.tests++;
	}
	xr_step(s, &in, &o);

	if (in.took)
		g_xr.requests++;
	g_xr.zeroed += (u32)o.zeroed;
	if (o.act == XR_ACT_SEND) {
		gip_set_rumble(gc, o.l, o.r, o.l, o.r);
	} else if (o.act == XR_ACT_RESEND) {
		gip_set_rumble(gc, 0, 0, 0, 0);
		gip_rumble_resend(gc);
		g_xr.restops++;
	}
	if (o.why != XR_WHY_NONE) {
		g_xr.last_why = (u32)o.why;
		g_xr.last_stop_ms = now;
		g_stop_at[pad] = now + 1;
		if (o.why == XR_WHY_ASKED)
			g_xr.stop_asked++;
		else if (o.why == XR_WHY_SETTING)
			g_xr.stop_setting++;
		else if (o.why == XR_WHY_TITLE)
			g_xr.stop_title++;
		else
			g_xr.stop_test++;
		if (o.why != XR_WHY_ASKED)
			xa_log("rumble: pad %d stopped (%s)\n", pad,
			       o.why == XR_WHY_SETTING ? "Vibration switched off" :
			       o.why == XR_WHY_TITLE ? "title changed" : "test ended");
	}
	/* gip_poll sends after this (same pass, paced 10 ms): the stop's latency is counted
	 * from the decision to the pass that sees it sent */
	if (g_stop_at[pad] && !gc->rumble_sent[0] && !gc->rumble_sent[1] &&
	    !gc->rumble_sent[2] && !gc->rumble_sent[3]) {
		g_xr.stop_lat_ms = now + 1 - g_stop_at[pad];
		if (g_xr.stop_lat_ms > g_xr.stop_lat_max_ms)
			g_xr.stop_lat_max_ms = g_xr.stop_lat_ms;
		g_stop_at[pad] = 0;
	}

	g_xr.table = xa_xinput_vibration_table();
	/* user 0xF: XAM gave the pad no index (the hooks then use 0) */
	put_byte(&g_xr.vib, pad,
		 in.vib < 0 ? 0xFF : ((u32)xa_xinput_user(pad) & 0xF) << 4 | (u32)in.vib);
	put_byte(&g_xr.req_l, pad, s->req_l);
	put_byte(&g_xr.req_r, pad, s->req_r);
	put_byte(&g_xr.sent_l, pad, gc->rumble_sent[0]);
	put_byte(&g_xr.sent_r, pad, gc->rumble_sent[1]);
	g_xr.now_ms = now;
	g_xr.title = title;
	BARRIER();
}
