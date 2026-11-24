// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Headphone surround's side of the game-audio tap (surround_360.c), for game_360.c only.
 * The public controls are xg_surround / xg_set_surround / xg_surround_live in game_360.h.
 *
 * Threads: xsv_init, xsv_poll, xg_surround*, xg_surround_offered and xsv_dev_driver_load run
 * on the driver thread; xsv_tap_input, xsv_tap_run, xsv_tap_done and xsv_dev_inject on the
 * kernel's audio render thread (game_360.c's XgTapC), and only while g_xsv_tap is nonzero.
 * They share single-writer words only (surround_360.c says which side writes which).
 */
#ifndef SURROUND_360_H
#define SURROUND_360_H

#include "../core/compat.h"

/* The gate (XWSR enable) as the tap sees it. 0: the tap is today's code, untouched. Nonzero:
 * the tap converts the six channels for the virtualizer, times itself and keeps the channel
 * peaks, and xsv_tap_run decides what goes into the ring. Written by the driver thread only;
 * it returns to 0 only after the tap has faded surround out. */
extern volatile u32 g_xsv_tap;

/* what xsv_tap_run tells the tap to put into the ring for this block */
enum {
	XSV_PLAIN,    /* today's downmix (convert_frame) */
	XSV_VIRTUAL,  /* the virtualizer's stereo */
	XSV_FADE_IN,  /* switching on: from today's downmix to the virtualizer, over the block */
	XSV_FADE_OUT, /* switching off: the other way */
};

/* the block's input: 6 x 256 s32 holding s16 values, planar FL FR C LFE SL SR */
s32 *xsv_tap_input(void);
/* After the input is filled: keeps the peaks, runs the virtualizer when it is needed and
 * returns XSV_*. *lr: its 256 stereo frames (interleaved s16). *plain: for a fade, scratch for
 * 512 s16 (the input's memory, no longer needed) that the tap fills with today's downmix. */
u32 xsv_tap_run(s16 **lr, s16 **plain);
/* the end of a block the surround path handled; t0 = xd_timebase() at the tap's start */
void xsv_tap_done(u32 t0);

/* The demo (xg_demo_* in game_360.h), in the tap while the gate is on. Per block:
 * xsv_demo_begin says what the block does with it; for anything but XSV_DM_NONE the tap then
 * calls xsv_demo_fill once the input holds what it needs (nothing for XSV_DM_PLAY: the clip
 * replaces it; the game's six channels converted, for a fade), and xsv_demo_end once the
 * block is in the ring. A block the tap drops (no capture, the ring full) skips both, and the
 * next block begins the same way again. */
enum {
	XSV_DM_NONE, /* no demo: the game, as without it */
	XSV_DM_IN,   /* the first block: from the game into the clip (needs the game's block) */
	XSV_DM_PLAY, /* the clip alone (the game is not needed: no capture) */
	XSV_DM_OUT,  /* the last: from the clip back to the game (needs the game's block) */
};
u32 xsv_demo_begin(void);
void xsv_demo_fill(u32 dm, s32 *in);
void xsv_demo_end(u32 dm);

/* from xg_init: checks the table and allocates the state (failure: surround unavailable) */
void xsv_init(void);
/* from xg_poll, every pass: the gate, the setting, the live flag, the demo */
void xsv_poll(void);
/* from xg_quiesce / xg_resume (hot reload): the demo stops and takes no new start; its ring
 * is freed when nothing can be using it (tap_idle: the tap will not run again) */
void xsv_quiesce(bool tap_idle);
void xsv_resume(void);

#ifdef XW_DEV
/* test_ch: noise bursts into one channel of the raw frame (float bits), the others silent */
void xsv_dev_inject(u32 *frame);
/* load_site 1: the dummy load on the driver thread, once per earpiece block of `frames` */
void xsv_dev_driver_load(u32 frames);
#endif

#endif
