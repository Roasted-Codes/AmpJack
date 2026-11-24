/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Rumble (rumble_360.c): what the pad's motors are told, from the title's requests
 * (XamInputSetState, xinput_360.c) and the system's per-profile Vibration setting.
 *
 * How a real 360 pad is silenced (17559, read in research/dumps; docs/INTERNALS.md, Rumble):
 *   - XamInputSetState (xam 402, 0x816F1740) ignores the title's flags and passes the kernel a
 *     scale byte from XAM's per-user table (0x81AAC2DC, decoded from the function's own code
 *     at +0x3C): 3 = Vibration on, 0 = off. The wired-pad driver (kernel 0x800FA178) sends
 *     (speed / 3) x scale >> 8, so 0 means motors off. XAM refreshes the table from profile
 *     setting 0x10040003 (XPROFILE_OPTION_CONTROLLER_VIBRATION) at sign-in and on every
 *     "profile setting changed" notification; 3 for an index nobody is signed in on.
 *     Our hook took the title's levels and dropped that byte: the Guide's Vibration switch
 *     did nothing on our pads.
 *   - When a title ends, XAM (message 0x80000006, 0x816F4660) calls XamInputSetState with
 *     zero for users 0..3: our hook sees that like any request. Nothing in XAM or the kernel
 *     stops the motors when the Guide opens: the title does that (it keeps calling
 *     XamInputSetState), and our pads follow it the same way.
 *
 * What we add on top, cheaply, because a GIP pad's motor command runs for about ten minutes
 * (duration 0xFF x 10 ms, 0xEB repeats, as xone and xpad send it) where a 360 pad's level is
 * only held: a stop is sent a second time XR_RESTOP_MS later (a stop the pad's firmware
 * dropped would otherwise leave it rumbling), and a title switch that the plugin sees stops
 * the motors even if XAM's own stop never reached us.
 *
 * This header is the decision, a pure function of its inputs (no globals), host-tested in
 * tests/test_rumble.c. Driver thread only.
 */
#ifndef RUMBLE_360_H
#define RUMBLE_360_H

#include "../core/compat.h"

#define XR_MAGIC 0x58575242u  /* "XWRB" */
#define XR_RESTOP_MS 100      /* a stop is sent again this much later */
#define XR_TEST_MS 3000       /* a test request (xbdm) stops by itself after this */

/* why the motors were last stopped (XWRB last_why; rumble.py WHY mirrors it) */
enum { XR_WHY_NONE, XR_WHY_ASKED, XR_WHY_SETTING, XR_WHY_TITLE, XR_WHY_TEST };
/* what to do with the pad this pass */
enum { XR_ACT_NONE, XR_ACT_SEND, XR_ACT_RESEND };

/* one pad's rumble state (driver thread) */
struct xr_pad {
	u8 req_l, req_r;   /* the last request (title or test), 0..100 */
	u8 out_l, out_r;   /* what the pad was last told to do */
	u8 restop;         /* a stop is to be sent again at restop_ms */
	u8 test;           /* the last request was the xbdm test's */
	u8 seen_title;     /* `title` holds a title seen before */
	u8 pad_;
	u32 restop_ms, test_ms;
	u32 title;
};

/* one pass's inputs */
struct xr_in {
	u32 now;      /* milliseconds (wrap-safe differences) */
	u32 title;    /* anything that changes when the running title does */
	int gate;     /* XWRB enable: 0 = the title's levels as asked, nothing forced */
	int took;     /* a new request in l, r */
	int test;     /* ... that came from the xbdm test control */
	int vib;      /* XAM's Vibration setting for the pad's user: 1 on, 0 off, -1 not known */
	u8 l, r;
};

struct xr_out {
	int act;      /* XR_ACT_*: SEND = set the motors to l, r; RESEND = send the stop again */
	u8 l, r;
	int why;      /* XR_WHY_* when this pass stops motors that were running, else NONE */
	int zeroed;   /* a new non-zero request was silenced by the setting */
};

static inline void xr_reset(struct xr_pad *s)
{
	*s = (struct xr_pad){ 0 };
}

static inline void xr_step(struct xr_pad *s, const struct xr_in *in, struct xr_out *o)
{
	int switched = 0, timed_out = 0;
	u8 l, r;

	o->act = XR_ACT_NONE;
	o->l = o->r = 0;
	o->why = XR_WHY_NONE;
	o->zeroed = 0;

	/* a new title: whatever the old one asked for is over (before this pass's request,
	 * which may already be the new title's) */
	if (in->gate && (!s->seen_title || s->title != in->title)) {
		if (s->seen_title) {
			switched = s->req_l || s->req_r;
			s->req_l = s->req_r = 0;
			s->test = 0;
		}
		s->title = in->title;
		s->seen_title = 1;
	}
	if (in->took) {
		s->req_l = in->l;
		s->req_r = in->r;
		s->test = (u8)(in->test != 0);
		s->test_ms = in->now;
		switched = 0;
	}
	/* a test request nobody ended (the script died), gate or not */
	if (s->test &&(s->req_l || s->req_r) && in->now - s->test_ms >= XR_TEST_MS) {
		s->req_l = s->req_r = 0;
		s->test = 0;
		timed_out = 1;
	}

	l = s->req_l;
	r = s->req_r;
	if (in->gate && in->vib == 0) {
		o->zeroed = in->took && (l || r);
		l = r = 0;
	}

	if (l != s->out_l || r != s->out_r) {
		if (!l && !r) {
			o->why = in->gate && in->vib == 0 && (s->req_l || s->req_r) ? XR_WHY_SETTING
			       : switched ? XR_WHY_TITLE
			       : timed_out ? XR_WHY_TEST
			       : XR_WHY_ASKED;
			s->restop = (u8)(in->gate != 0);
			s->restop_ms = in->now + XR_RESTOP_MS;
		} else {
			s->restop = 0;
		}
		s->out_l = l;
		s->out_r = r;
		o->act = XR_ACT_SEND;
		o->l = l;
		o->r = r;
		return;
	}
	/* the same levels again: still passed on (gip_set_rumble keeps only changes), so a pad
	 * that restarted with its motors off is told again by the title's next request */
	if (in->took) {
		o->act = XR_ACT_SEND;
		o->l = l;
		o->r = r;
	}
	/* checked even while a title repeats its zero every frame (those send nothing) */
	if (s->restop && (s32)(in->now - s->restop_ms) >= 0) {
		s->restop = 0;
		if (!s->out_l && !s->out_r)
			o->act = XR_ACT_RESEND;
	}
}

/* rumble_360.c (driver thread): a new controller in pad slot `pad`; one pass for pad `pad`
 * on its GIP client, `title` as in struct xr_in */
struct gip_client;
void xr_pad_reset(int pad);
void xr_service(int pad, struct gip_client *gc, u32 title);

#endif
