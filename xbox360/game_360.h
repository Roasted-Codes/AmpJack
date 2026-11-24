// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Game audio in the headset and the earpiece mix (game_360.c; research/AUDIO.md, "Game
 * audio"): the console's final mix, copied from the kernel's XAudio render driver, mixed
 * into the earpiece with chat. The TV output is untouched (the kernel copies the mix to us;
 * nothing is patched).
 *
 * The mix, Astro A50 style (attenuation only, never a gain above 1):
 *   earpiece = master x (game x game_level + chat x chat_level) + master x sidetone x mic
 *   balance -5..+5 (0: both 100 %); each step toward game (negative) cuts chat by 20 %
 *   (-5: chat 0 %), each step toward chat cuts game the same way. master 0..100 % in 10s.
 *   sidetone 0..10 (x10 %, 0 = off): the own mic, mixed by voice_360.c (not balanced).
 * The settings are kept in memory here; settings_360.c saves and restores them per profile
 * through the getters and setters below.
 *
 * Every function here runs on the driver thread (voice_360.c and xw_plugin.c call them),
 * except the tap, which runs on the kernel's audio render thread (game_360.c).
 *
 * Wireless (docs/WIRELESS-NOTES.md): the tap uses one of the kernel's two MEC client slots
 * (ordinals 800 / 802). Upstream's wireless-headset work uses the same tap: one plugin that
 * does both must share this registration, not add a second one.
 */
#ifndef GAME_360_H
#define GAME_360_H

#include "../core/compat.h"

#define XG_BALANCE_MAX 5
#define XG_MASTER_STEP 10
#define XG_SIDETONE_MAX 10

/* once, from xv_init: nothing is registered yet (xg_poll does that after the boot delay) */
void xg_init(void);

/* every driver-thread pass, from xv_poll: controls, the delayed registration, the tap gate */
void xg_poll(void);

/* Hot reload (docs/HOT-RELOAD.md): the tap off and the MEC client unregistered (waits for a
 * running callback; left registered and idle if the kernel's unregister isn't the code read
 * offline). xg_resume: after a refused reload, registers again. */
void xg_quiesce(void);
void xg_resume(void);

/* the game mix added to an earpiece block of pad `pad` (s16le, rate_out x ch_out, `len`
 * bytes), at master x game level, clamped. Does nothing while game audio is off. */
void xg_mix(int pad, u8 *out, int len, u32 rate_out, u32 ch_out);

/* the ring target the ratio trim steers to: 30, 45 (default), 60 or 90 ms; false = not one
 * of those (nothing changes). The target in effect ramps there at 0.75 ms a second while
 * audio plays (never a jump); the rebuffer limit (target + 75 ms, 150 ms at most) and the
 * full-trim distance (target / 3) follow it. Getters: in effect now, in 48 kHz frames. */
bool xg_set_buffer(u32 ms);
u32 xg_buffer_target(void);
u32 xg_buffer_high(void);
u32 xg_buffer_drift_full(void);

/* hold: xg_mix adds nothing (what was playing fades out over one block; game audio starts
 * over, buffered and faded in, when the hold ends). For the diagnostics test tone. */
void xg_hold(bool on);

/* duck: game audio at q8 / 256 of its level (256 = not ducked; never above), from the next
 * earpiece block, ramped over that block. For the mic test's playback (voice_360.c). */
void xg_duck(u32 q8);

/* Q8 gains (256 = 1): master x chat level, and master alone (sidetone, the centre tick) */
u32 xg_chat_gain(void);
u32 xg_master_gain(void);
/* true: VOLUME is applied by the headset's amplifier (voice_360.c sends it VOLUME x its
 * speaker setting); the Q8 gains above then carry the balance only (0 at VOLUME 0) */
void xg_master_device(bool on);

/* game audio is reaching the earpiece now: on, registered, and an earpiece is being fed */
bool xg_routed(void);

/* balance (-5 = all game .. +5 = all chat), master (0..100 %), sidetone (0..10) */
int xg_balance(void);
u32 xg_master(void);
u32 xg_sidetone(void);
/* moves balance by dbal steps, master by dmaster steps of 10 % and sidetone by dside steps,
 * clamped; heard from the next earpiece block on (Share + D-pad / bumpers; the HUD shows it) */
void xg_adjust(int dbal, int dmaster, int dside);

/* SURROUND (surround_360.c): 5.1 game audio virtualized for headphones instead of the stereo
 * downmix (five virtual speakers; a stereo game gets two virtual front speakers).
 * xg_surround: the setting, 0 OFF (default: today's downmix), 1 ON. It is kept even while
 * surround is unavailable (its xbdm gate XWSR enable is off, the default, or its memory could
 * not be allocated), and is then not applied. xg_surround_offered: surround can be heard (the
 * gate is on and its state was allocated); the panel shows the SURROUND row only then. xg_set_surround switches it (a one-block crossfade in the tap, within a driver pass
 * and a 5.33 ms block; anything above 1 is 1). xg_surround_live: the centre or surround
 * channels carried sound in the last second (the panel's dot; it changes nothing; false while
 * the gate is off or no game audio plays). */
u32 xg_surround(void);
void xg_set_surround(u32 on);
bool xg_surround_live(void);
bool xg_surround_offered(void);

/* SURROUND DEMO (surround_360.c): a 40 s clip from Hdd:\Plugins\xw.demo played in place of the
 * game's sound, through the same path, so SURROUND ON / OFF can be compared on it while it
 * plays (tools/demo/make_demo.py makes the file). It plays only while game audio reaches the
 * headset (with or without a title running), ends by itself, and stops on xg_demo_stop, a
 * title change, surround's gate going off, or 2 s without game audio; memory (58.7 KB) only
 * while it plays.
 * xg_demo_offered: surround is offered and the file is there (looked for while the gate is
 * on: at the gate's first pass, then every 30 s while missing). xg_demo_playing: starting or
 * playing; *secs = seconds played. xg_demo_start: ignored unless offered and idle; the sound
 * starts within a few frames (the first 299 ms are read first). */
bool xg_demo_offered(void);
bool xg_demo_playing(u32 *secs);
void xg_demo_start(void);
void xg_demo_stop(void);

#endif
