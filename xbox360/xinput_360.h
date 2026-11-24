/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Presents the paired controllers to the console as XInput gamepads (the XInputd route).
 *
 * A virtual controller is bound per pad with XamUserBindDeviceCallback, and three
 * functions are hooked so the console reads our state: XInputdReadState (kernel),
 * XamInputGetCapabilitiesEx and XamInputSetState (XAM, the last for rumble). This is
 * hiddriver360's route (EinTim23, GPL-3.0); it leaves the console's own wireless slots
 * alone, so the built-in 360 pads keep working.
 *
 * Wireless (docs/WIRELESS-NOTES.md): xw.xex and xa.xex both install these hooks, so they
 * never load together. One plugin for wired and wireless pads would share these 4 slots.
 */
#pragma once

#include "../core/compat.h"
#include "../core/gip.h"

/* resolve the hooked functions and install the hooks; 0 or a negative error */
int xa_xinput_init(void);

/* remove the hooks (restore the original kernel/XAM code). Call only when the dashboard
 * is not polling them (e.g. not in Aurora), after a short quiesce. */
void xa_xinput_shutdown(void);

/* unbind all pads in XAM, then xa_xinput_shutdown(): for handing over to a new build */
void xa_xinput_release(void);

/* Hot reload (xw.xex, docs/HOT-RELOAD.md), driver thread. xa_xinput_drain: the XAM thread
 * has carried out everything queued so far and is idle (false after `ms`: still inside XAM;
 * nothing was changed). xa_xinput_quiesce: final; no pad is served, the XAM thread exits, the
 * hooks stay in place as pass-throughs for the next build to adopt (never unpatched). */
bool xa_xinput_drain(unsigned int ms);
bool xa_xinput_quiesce(void);

/* bind pad `slot` (0..3) as a virtual controller, on the XAM thread (asynchronous):
 * 0 = queued, -1 = not possible */
int xa_xinput_bind(int slot);

/* unbind pad `slot` (stops serving it at once; XAM is told on the XAM thread) */
void xa_xinput_unbind(int slot);

/* the jack headset (voice_360.c): bind (connect) or unbind pad `slot`'s headset in XAM
 * category 1 with the pad's controller id and tag, as the kernel's wired driver does
 * (0x801064A4), on the XAM thread and in order with the pad's own requests.
 * 0 = queued, -1 = not possible */
#define XA_XINPUT_HEADSET 1
#define XA_XINPUT_CONTEXT_BASE 0x10000005 /* pad slot n's device tag is this + n */
int xa_xinput_bind_headset(int slot, bool connect);

/* whether pad `slot` is bound (category 0) and served */
bool xa_xinput_active(int slot);

/* the XAM user index (0..3) pad `slot` was bound as, for whoever asks who is signed in on
 * that pad (settings_360.c); -1 while it is not bound, or if XAM gave it no index. Any
 * thread: it only reads what the XAM thread published with the bind. */
int xa_xinput_user(int slot);

/* test mode (xw.py test): pads report nothing to the console and the Guide button does
 * nothing, so a button test can't launch or open anything; their state is still logged */
extern volatile u32 xa_xinput_mute;

/* store a pad's current state, translated to XInput (called from the driver thread) */
void xa_xinput_update(int slot, const struct gip_gamepad_state *s);

/* timers (the Guide long press); call every few milliseconds from the driver thread */
void xa_xinput_poll(int slot);

/* a rumble asked for by the console since the last call; false = none.
 * left/right are 0..100 (gip_set_rumble's scale). */
bool xa_xinput_take_rumble(int slot, u8 *left, u8 *right);

/* the system's Vibration setting (Guide > Settings > Preferences) for the profile on pad
 * `slot`'s user index, as XAM's own XamInputSetState applies it (rumble_360.h): 1 on (also
 * nobody signed in), 0 off, -1 not known (pad not bound, or XAM's table not recognised at
 * init). Any thread: one byte load. xa_xinput_vibration_table: the table's address, 0 if
 * not recognised (for the XWRB status block). */
int xa_xinput_vibration(int slot);
u32 xa_xinput_vibration_table(void);

/* on-screen notifications, shown by the XAM thread; "starting" is shown when it starts. The
 * mic ones are sent only when panel_360.c's xbdm `notify` is set (the headset bar shows the
 * mute state instead). */
enum {
	XA_NOTIFY_STARTING, XA_NOTIFY_READY, XA_NOTIFY_NO_FIRMWARE, XA_NOTIFY_NO_FIRMWARE_02FE,
	XA_NOTIFY_NO_FIRMWARE_02E6, XA_NOTIFY_PAD_ON, XA_NOTIFY_PAD_FULL, XA_NOTIFY_MIC_MUTED,
	XA_NOTIFY_MIC_ON, XA_NOTIFY_COUNT
};
void xa_xinput_notify(int msg);

/* log the hook counters when they change (throttled); called from the driver thread */
void xa_xinput_debug_log(void);
