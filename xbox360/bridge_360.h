// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The PC bridge (bridge_360.c; the PC side is tools/bridge/xbox-bridge.py): four audio streams
 * over UDP on the LAN between the plugin and a helper on a PC.
 *
 *   1 HEADSET MIC   console -> PC: the headset mic after MIC VOLUME and the noise gate
 *   2 XBOX PLAYERS  console -> PC: the chat the 360 plays to this headset
 *   3 TO HEADSET    PC -> console: mixed into the earpiece with game and chat
 *   4 TO XBOX CHAT  PC -> console: mixed into the mic the 360 gets (never into 1 or 2)
 *
 * The PC starts it: it sends a hello to UDP 3670 about once a second, and the console streams
 * to that address while hellos keep coming (3 s without one ends the session). Nothing to set
 * on the console; XWBR.enable (bridge.py on|off, default on) is the only gate.
 *
 * Every function here is the driver thread's, and none of them touches the network: they
 * write to and read from rings the bridge's own net thread empties and fills. With no PC
 * connected each costs one test.
 */
#ifndef BRIDGE_360_H
#define BRIDGE_360_H

#include "../core/compat.h"

void xb_init(void);    /* at start: the rings; the net thread starts 20 s later (xb_poll) */
void xb_poll(void);    /* every pass: starts the net thread, logs, publishes, timing */
void xb_quiesce(void); /* hot reload: the socket closed, nothing streamed */
void xb_resume(void);

/* stream 1: does a PC want the headset mic now? (voice_360.c then shapes the mic for it even
 * when the 360 doesn't take it) */
bool xb_mic_wanted(void);
/* stream 1: the mic after MIC VOLUME and the gate (silence while muted), mono s16le */
void xb_mic(int pad, const u8 *mono, int frames, u32 rate);
/* stream 2: an earpiece block as the 360's chat fills it, before any gain (s16le, ch 1..2) */
void xb_players(int pad, const u8 *out, int len, u32 rate, u32 ch);
/* stream 3: added to an earpiece block at master (Q8) x the PC-to-headset level, clamped */
void xb_mix_headset(int pad, u8 *out, int len, u32 rate, u32 ch, u32 master_q8);
/* stream 4: added to the mic block the 360 gets (mono s16le) at the PC-to-chat level, clamped */
void xb_mix_chat(int pad, u8 *mono, int frames, u32 rate);

#endif
