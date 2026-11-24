// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Console-to-console team voice (docs/PLAN.md Phase 4, contract 4.2). team_360.c implements
 * it; until it exists, team_360_stub.c does nothing (build.sh, XW_TEAM).
 */
#ifndef TEAM_360_H
#define TEAM_360_H

#include "../core/compat.h"

/* driver thread unless noted */
void xt_init(void);                 /* nothing opened: gated by XWTV.stage */
void xt_poll(void);                 /* stage changes, peer ageing, status */
void xt_quiesce(void);  void xt_resume(void);
/* voice_360.c -> team: the mic as the 360 gets it (after level and gate), s16le at rate x ch;
 * `open` = the gate is passing it. Called per mic packet. */
void xt_mic(int pad, const u8 *pcm, int len, u32 rate, u32 ch, bool open);
/* voice_360.c's xv_earpiece_fill -> team: adds the team mix to an earpiece block, at `gain`
 * (Q8: master x chat_level x team_level), clamped; like xg_mix */
void xt_mix(int pad, u8 *out, int len, u32 rate_out, u32 ch_out, u32 gain);
/* panel */
u32  xt_channel(void);   void xt_set_channel(u32 ch);     /* 0 off, 1..8 */
u32  xt_level(void);     void xt_set_level(u32 level);    /* 0..10 */
u32  xt_mic_route(void); void xt_set_mic_route(u32 r);    /* 0 TEAM, 1 GAME, 2 BOTH */
u32  xt_peers(void);     u32 xt_talking_mask(void);
bool xt_mic_to_game(void);          /* voice_360.c: false = give the 360 silence */

#endif
