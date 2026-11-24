// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The contract between the wired driver (xw_plugin.c: USB, GIP, the headset's PCM) and the
 * virtual 360 headset (voice_360.c: XAM category-1 bind, XVoiced detours, packet service,
 * rate conversion). Every function here is called from the driver thread only (never from a
 * Usbd DPC or a callback), so the two sides need no locking between them.
 *
 * PCM on this interface is the headset's own format, as GIP carries it: s16 little-endian,
 * interleaved, at the negotiated rate. voice_360.c converts to and from what the 360 expects.
 *
 * Wireless (docs/WIRELESS-NOTES.md): this hooks XVoiced* in the kernel. Upstream's wireless
 * headset work hooks XamVoice* in XAM instead; one plugin doing both must pick one route.
 */
#ifndef VOICE_360_H
#define VOICE_360_H

#include "../core/compat.h"

/* the jack headset on pad slot `pad` (0-3, the XInput slot) became usable / went away.
 * rate_*: Hz; ch_*: 1 or 2. Called after the pad itself is bound, and before it is unbound. */
void xv_headset_changed(int pad, bool present, u32 rate_in, u32 ch_in, u32 rate_out,
			u32 ch_out);

/* pad slot `pad` is about to be unbound (call before xa_xinput_unbind): its headset is
 * unbound first, whatever kept it bound (the jack or voice.py's force mask) */
void xv_pad_releasing(int pad);

/* microphone samples from the headset, `len` bytes of s16le at rate_in x ch_in */
void xv_mic_samples(int pad, const u8 *data, int len);

/* the next earpiece block: fills exactly `len` bytes of s16le at rate_out x ch_out (silence
 * when the 360 has nothing to play). Called once per 8 ms block, at the USB stream's pace. */
void xv_earpiece_fill(int pad, u8 *out, int len);

/* The headset's own volume settings and sidetone (MS-GIPUSB 3.2.5.1.1, core/gip.h GIP_VOL_*)
 * for pad slot `pad`, every loop pass while its headset is ready: `report` (`report_len`
 * bytes from the flags on, NULL before the first) is the headset's last volume report and
 * `reports` how many it sent; `sent` (GIP_VOL_FIELDS bytes) is what was last sent to it and
 * `sends` how many times. Fills `want` (GIP_VOL_FIELDS bytes); the caller sends it
 * (gip_headset_set_volume) when it differs from `sent`. Picks the sidetone's path: while
 * the headset makes it, the software sidetone in the earpiece fill is silent. */
void xv_headset_volume(int pad, const u8 *report, u32 report_len, u32 reports, const u8 *sent,
		       u32 sends, u8 *want);

/* the Share button: toggles pad slot `pad`'s mic mute (the 360 gets silence while muted).
 * Returns the new state: true = muted. */
bool xv_toggle_mic_mute(int pad);

/* pad slot `pad`'s mic is muted (xv_toggle_mic_mute) */
bool xv_mic_muted(int pad);

/* the centre tick: one short, soft 2 kHz tick in every streaming earpiece (the balance just
 * landed on centre), at master volume */
void xv_tick(void);

/* driver thread, every loop iteration: completes due packets, housekeeping */
void xv_poll(void);

/* Hot reload (docs/HOT-RELOAD.md). xv_quiesce: unbinds every headset, fails its packets and
 * binds nothing again; the XVoiced hooks stay in place and keep shielding our tags until the
 * next build adopts them. xv_resume: undoes it after a refused reload. (Game audio has its
 * own pair, xg_quiesce / xg_resume in game_360.h.) */
void xv_quiesce(void);
void xv_resume(void);

/* once at start-up from the driver thread, before any of the above (installs nothing by
 * itself: hooks and binds are gated, see voice_360.c) */
void xv_init(void);

#endif
