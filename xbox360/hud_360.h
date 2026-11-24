// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The contract between the headset controls (xw_plugin.c / voice_360.c / game_360.c: Share +
 * D-pad / bumpers, the mix) and the on-screen headset bar (hud_360.c: drawn into the game's
 * picture at present time). The control side publishes a snapshot with hud_publish() on the
 * driver thread; the overlay side reads the latest snapshot whenever it draws, from whatever
 * thread presents. hud_360.c owns the synchronisation (seqlock or double buffer).
 */
#ifndef HUD_360_H
#define HUD_360_H

#include "../core/compat.h"

/* hud_state.visible. HUD_BADGE: only the small persistent mute mark (hud_state.badge) */
enum { HUD_HIDDEN = 0, HUD_TOAST = 1, HUD_PANEL = 2, HUD_BADGE = 3 };

/* The rows. The numbers are bit positions in hud_rows() and indexes of per-row tables and
 * XWPN counters, so they never move; rows 8 and up have their bit in rows_hi (rows is a u8):
 * MIC TEST bit 0, PRESET bit 1, SURROUND bit 2, BASS bit 3, MIDS bit 4, TREBLE bit 5,
 * SURROUND DEMO bit 6.
 * BASS, MIDS and TREBLE are the CUSTOM preset's three controls (eq_360.h XQ_CUSTOM_*, in that
 * order): shown only while CUSTOM is the selected preset. SURROUND DEMO plays a clip through
 * the surround path (game_360.h xg_demo_*): shown only with SURROUND and the clip's file. */
enum { HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE, HUD_ROW_MIC, HUD_ROW_GATE,
       HUD_ROW_TONE, HUD_ROW_TEAM, HUD_ROW_CHANNEL, HUD_ROW_MICTEST, HUD_ROW_PRESET,
       HUD_ROW_SURROUND, HUD_ROW_BASS, HUD_ROW_MIDS, HUD_ROW_TREBLE, HUD_ROW_DEMO, HUD_ROWS };
#define HUD_ROWS_CUSTOM (1u << HUD_ROW_BASS | 1u << HUD_ROW_MIDS | 1u << HUD_ROW_TREBLE)
/* The order the rows are drawn in and the cursor walks. hud_panel.c and panel_360.c must both
 * use it, or Up / Down would land on a row other than the one drawn above / below. A row whose
 * bit is clear is skipped; every row has a place here, so turning one on never reorders the
 * others. The CUSTOM rows sit directly under PRESET: they belong to its value. MIC TEST sits
 * directly under NOISE GATE: a clip plays back through the gate in effect, so a player
 * records once and steps NOISE GATE (one Up) while it plays. SURROUND DEMO sits directly under
 * SURROUND for the same reason: one Up flips SURROUND while the demo plays. */
static const u8 hud_row_order[HUD_ROWS] = {
	HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE, HUD_ROW_MIC, HUD_ROW_GATE,
	HUD_ROW_MICTEST, HUD_ROW_PRESET, HUD_ROW_BASS, HUD_ROW_MIDS, HUD_ROW_TREBLE,
	HUD_ROW_SURROUND, HUD_ROW_DEMO, HUD_ROW_TEAM, HUD_ROW_CHANNEL, HUD_ROW_TONE,
};

/* hud_state.mictest: the same numbers as diag_360.h's XD_MT_* (panel_360.c asserts it).
 * IDLE: no clip. PAUSED: playback stopped part way, clip held. READY: a whole clip held,
 * nothing playing. */
enum { HUD_MT_IDLE, HUD_MT_RECORDING, HUD_MT_PLAYING, HUD_MT_PAUSED, HUD_MT_READY };
/* hud_state.gate: the same numbers as diag_360.h's XD_GATE_* (OFF, steps 1..4, AUTO). The
 * stored numbers never move (xbdm tools and scripts write them); the panel shows them in its
 * own order. */
enum { HUD_GATE_OFF, HUD_GATE_1, HUD_GATE_2, HUD_GATE_AUTO, HUD_GATE_3, HUD_GATE_4 };
/* hud_state.mic_level: MIC VOLUME AUTO (the level in effect is then mic_auto_level) */
#define HUD_MIC_AUTO 0
/* hud_state.badge bits */
#define HUD_BADGE_MUTED 0x01
#define HUD_NAME_LEN 20 /* preset_name */
#define HUD_DESC_LEN 64 /* desc: eq_360.h XQ_DESC_LEN (panel_360.c asserts it) */
/* hud_state.preset_flags bits */
#define HUD_PF_CUSTOM 0x01 /* CUSTOM is offered: the PRESET row's last value, preset_count + 1 */

/* Version 7. The first 24 bytes keep version 3's positions and meanings except where noted;
 * a zero in any later byte means what version 3 showed (no PRESET or SURROUND row, no mark,
 * no CUSTOM, no description, the demo idle). Version 5 put preset_flags, custom[] and
 * desc_lines into version 4's reserved bytes and appended desc[]; version 6 shows desc in the
 * help line and leaves byte 55 (desc_lines, the separate description area's height) unused,
 * 0; version 7 appends the demo's two bytes and two reserved. Grow it
 * only into reserved bytes or at the end, and keep XH36's test_* words (hud_360.c, hud.py
 * show) in step: hud.py show is how the panel is checked without hands on the pad. */
struct hud_state {
	u8 visible;      /* [0] HUD_* */
	u8 muted, flash, headset, game_on; /* [1..4] flash: see below */
	u8 cursor;       /* [5] HUD_ROW_*: the selected row */
	u8 rows;         /* [6] bit n = row n is shown, rows 0..7 */
	s8 balance;      /* [7] -5..+5 */
	u8 volume, sidetone;        /* [8, 9] 0..10 */
	u8 mic_level;    /* [10] HUD_MIC_AUTO (0) or 1..10 */
	u8 gate;         /* [11] HUD_GATE_* */
	u8 tone;         /* [12] 0 off, 1 440 Hz, 2 1 kHz */
	u8 mic_vu;       /* [13] 0..10: the mic's level now (after gain, before the gate) */
	u8 gate_open;    /* [14] 1 = the gate is passing the mic */
	u8 team_level;   /* [15] 0..10 */
	u8 channel;      /* [16] 0 off, 1..8 */
	u8 team_peers;   /* [17] consoles heard on this channel */
	u8 team_talking; /* [18] bit n = peer n is talking */
	u8 rows_hi;      /* [19] bit n = row 8 + n is shown */
	u8 mictest;      /* [20] HUD_MT_* */
	u8 mictest_secs; /* [21] RECORDING: seconds recorded so far (count-up); PLAYING, PAUSED,
			  * READY: the clip's length in seconds */
	u8 mictest_pct;  /* [22] RECORDING: 0..100 % of mictest_max; PLAYING / PAUSED: played */
	u8 mic_auto_level; /* [23] MIC VOLUME AUTO's level in effect now, 1..10 (0 unknown) */
	u8 preset;       /* [24] 0 FLAT, 1..preset_count, preset_count + 1 CUSTOM (HUD_PF_CUSTOM) */
	u8 preset_count; /* [25] 0 = no preset file (the row shows FLAT, and CUSTOM if offered) */
	u8 surround;     /* [26] 0 OFF, 1 ON */
	u8 surround_live; /* [27] 1 = centre / surround channels carry sound now (the dot) */
	u8 badge;        /* [28] with HUD_BADGE: HUD_BADGE_* bits (other bits free) */
	u8 badge_shift;  /* [29] 0..3: moves the mark one unit now and then (burn-in) */
	u8 mictest_max;  /* [30] the auto-stop in seconds (xd_mictest_max_secs) */
	u8 preset_flags; /* [31] HUD_PF_* (version 4: reserved, 0) */
	char preset_name[HUD_NAME_LEN]; /* [32..51] NUL padded, panel font characters only */
	s8 custom[3];    /* [52..54] CUSTOM's BASS, MIDS, TREBLE steps, -6..+6 (XQ_CUSTOM_*) */
	u8 reserved55;   /* [55] 0 (version 5: desc_lines; nothing reads it) */
	/* [56..119] the selected preset's description (xq_desc), shown in the help line while the
	 * cursor is on PRESET (panel_360.c publishes it only then, and only with presets or
	 * CUSTOM): NUL padded, panel font characters only, at most HUD_DESC_LEN - 1 characters
	 * (a 64th is ignored); longer than the help line (HP_HELP_COLS), it is cut at a word */
	char desc[HUD_DESC_LEN];
	u8 demo;         /* [120] SURROUND DEMO: 0 ready (or the row hidden), 1 playing */
	u8 demo_secs;    /* [121] while playing: seconds played */
	u8 reserved122[2]; /* [122, 123] 0 */
};
_Static_assert(sizeof(struct hud_state) == 124, "hud_360.h version 7 (XH36 test_*, hud.py)");
_Static_assert(__builtin_offsetof(struct hud_state, mictest_pct) == 22 &&
	       __builtin_offsetof(struct hud_state, preset_name) == 32 &&
	       __builtin_offsetof(struct hud_state, custom) == 52 &&
	       __builtin_offsetof(struct hud_state, desc) == 56 &&
	       __builtin_offsetof(struct hud_state, demo) == 120, "hud_360.h version 7 layout");
/* `flash`: a HUD_TOAST is the mute toast (MIC MUTED / MIC ON). A toast without it, while a mic
 * test records or plays, shows the test instead (the panel was let go of). */

/* every row shown: rows, and rows_hi above it */
static inline u32 hud_rows(const struct hud_state *s)
{
	return (u32)s->rows | (u32)s->rows_hi << 8;
}

/* driver thread: the new state (copied) */
void hud_publish(const struct hud_state *s);

/* driver thread, once at start-up (installs nothing by itself; gated and delayed like the
 * other hooks) */
void hud_init(void);

/* driver thread, every loop iteration: delayed install, housekeeping */
void hud_poll(void);

/* Hot reload (docs/HOT-RELOAD.md), driver thread: this build is handing over to the next
 * one and its image stays loaded. hud_quiesce: from its return on, the overlay's hook (the
 * VdSwap detour) must draw nothing and touch no state the driver thread owned: a pure
 * pass-through to the original, for good, until the next build adopts the hook's slot. It
 * must NOT unpatch (a thread may be inside the hook or its trampoline). So hud_360.c must
 * install its hook with detour_install (detour.h): then the next build takes it over with
 * one store. hud_resume: a refused reload; draw again. */
void hud_quiesce(void);
void hud_resume(void);

#endif
