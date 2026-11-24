// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The contract between xw_plugin.c (the wired class driver and its driver thread) and the
 * headset controls (panel_360.c: the Share button's gestures, the headset panel's cursor and
 * rows driven by Share + D-pad, and what the on-screen headset bar shows, hud_360.h; xbdm
 * block "XWPN"). docs/PLAN.md sections 1.0 and 1.0.3.
 *
 * `pad` is the controller's xa_xinput slot (0..3: the player), the same number xv_* and
 * xa_xinput_* take; xw_plugin.c calls these only for a controller that has one.
 */
#ifndef PANEL_360_H
#define PANEL_360_H

#include "../core/compat.h"
#include "../core/gip.h"

/* all on the driver thread */
void panel_init(void);
/* a raw GIP input packet (command 0x20) of port/pad, before gip_rx parses it */
void panel_raw_input(int pad, u16 vid, u16 pid, const u8 *pkt, int len);
/* the parsed state, before xa_xinput_update: masks the controls the panel consumed */
void panel_filter(int pad, struct gip_gamepad_state *st);
void panel_pad_reset(int pad);   /* a new controller in this slot */
void panel_poll(void);           /* every pass: timers, tone auto-off, hud_publish */

/* ---- what xw_plugin.c tells the panel (driver thread) ---- */
/* the pad whose headset is on the voice side (xv_headset_changed), or -1 */
int xw_headset_pad(void);
/* that headset is on the voice side and its audio is streaming */
bool xw_headset_live(void);

#endif
