// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The headset controls (panel_360.h): the Share button's gestures, the headset panel's cursor
 * and rows, and what the on-screen headset bar shows (hud_360.h). All on the driver thread.
 * docs/PLAN.md section 1.0 (the gesture model), docs/ROADMAP.md Part 1 section 4 (the rows)
 * and Part 3 item 2 (the mic test).
 *
 * One control method:
 *   Share tap (released within tap_ms, 400 ms, with no other button pressed meanwhile)
 *       toggles the pad's mic mute, on release; the bar shows MIC MUTED / MIC ON for 1.5 s
 *       (TOAST_MS). No XNotify toast unless xbdm sets `notify` (XWPN).
 *   Share held hold_ms (250 ms), or any D-pad press while Share is down
 *       shows the panel. The cursor starts on BALANCE at every Share press.
 *       D-pad Up / Down: the row above / below in hud_row_order, skipping hidden rows
 *       (stops at the first / last row).
 *       D-pad Left / Right: that row's value down / up (clamped, no wrap). On PRESET and
 *       the CUSTOM rows, holding Left / Right repeats about 8 times a second after 400 ms.
 *       So Share + Left / Right is still balance, and volume is Share, Down, Left / Right.
 *       LB / RB (XWPN `bumpers`, default on): sidetone down / up from any row; the
 *       cursor moves to SIDETONE so the panel shows it.
 *   Share released
 *       hides the panel; the test tone is switched off as it hides. A mic test that records
 *       or plays keeps going: the bar shows a small toast instead (REC 7 SEC, PLAYING 40%);
 *       the mute toast takes its place for its 1.5 s. A paused or held clip is deleted
 *       (xd_mictest_panel_closed).
 *   Share held and released with no other button (longer than a tap): nothing.
 * The D-pad (and LB / RB while `bumpers` is on) is kept from the title while Share is down (a
 * press with Share down stays hidden until it is released, even if Share goes up first; one
 * already held when Share went down stays the title's). Every other button reaches the title
 * as usual; pressing one while Share is down only stops the release from being a mute tap.
 *
 * What is on screen: the panel while Share is held; else a toast (the mute toast, or a mic
 * test that records or plays); else, while the mic is muted and XWPN `badge` is on, the small
 * persistent mute mark (HUD_BADGE); else nothing.
 *
 * Rows (HUD_ROW_*, drawn and walked in hud_row_order):
 *   BALANCE    -5..+5, centre tick (xv_tick)        xg_adjust
 *   VOLUME     0..10 (master x10 %)                 xg_adjust
 *   SIDETONE   0..10                                xg_adjust
 *   MIC VOLUME AUTO, 1..10                          g_xd.mic_gain (xd_mic_gain_step: 3 dB a
 *                                                   step, 25 .. 560 %, 5 = 100 %); AUTO is
 *                                                   xd_set_mic_auto and leaves mic_gain alone,
 *                                                   so Right from AUTO is the last manual step
 *   NOISE GATE AUTO, OFF, LOW, HIGH (GATE_ORDER)    g_xd.gate (stored numbers XD_GATE_*)
 *   PRESET     FLAT, CUSTOM, 1..xq_count()          xq_set_preset (CUSTOM: xq_custom_index,
 *                                                   only while xq_custom_offered)
 *     BASS / MIDS / TREBLE  -6..+6                  xq_set_custom; shown only while CUSTOM
 *                                                   is selected (a cursor on one when they
 *                                                   go moves to PRESET)
 *   SURROUND   OFF / ON                             xg_set_surround
 *     SURROUND DEMO  Right plays, Left or Right     xg_demo_start / xg_demo_stop; shown only
 *                    stops                          with SURROUND and the clip's file
 *                                                   (xg_demo_offered); it keeps playing when
 *                                                   the cursor moves (Up flips SURROUND) and
 *                                                   when the panel closes (it ends by itself)
 *   TEAM VOL   0..10, TEAM CH OFF, 1..8             xt_set_level / xt_set_channel
 *   TEST TONE  off / 440 Hz / 1 kHz                 g_xd.tone; off when the panel hides and
 *                                                   when a pad leaves
 *   MIC TEST   Right: record, stop and play, pause, play on, play again; Left: discard
 *                                                   xd_mictest_* (diag_360.h)
 * Rows for features that are not there yet stay out of the way (features(), rows_shown()):
 * PRESET is always shown and offers FLAT only while no preset is loaded; SURROUND, TEAM VOL /
 * TEAM CH and the AUTO values appear only once their owners say they are available.
 * Under the rows, a help line says what the selected row does. On PRESET, while presets are
 * loaded or CUSTOM is offered, it says what the selected preset sounds like instead: its
 * description (xq_desc), published in hud_state.desc only while the cursor is on PRESET (the
 * panel's height never depends on it: one line, as every other row's help).
 *
 * xbdm block "XWPN" (g_pn): `inject` drives all of it with no hands on the pad (bit 0 Share,
 * 1 Up, 2 Down, 3 Left, 4 Right), OR-ed into pad `inject_pad`'s input as if it came from the
 * controller (the title sees an injected D-pad when Share isn't down). An inject that hasn't
 * changed for INJECT_TIMEOUT_MS is cleared, so a script that dies can't leave Share held.
 * xbox360/accept/wp1_3.py and accept/badge.py use it.
 */
#include <stdlib.h>

#include "../core/gip.h"
#include "../core/platform.h"
#include "diag_360.h"
#include "eq_360.h"
#include "game_360.h"
#include "hud_360.h"
#include "hud_panel.h" /* HP_CBAR_STEPS: the CUSTOM bar's ends are the engine's */
#include "pad_quirks.h"
#include "panel_360.h"
#include "platform_360.h"
#include "team_360.h"
#include "voice_360.h"
#include "xinput_360.h"

#define PADS 4 /* xa_xinput slots */

#define PN_HIDDEN HUD_HIDDEN
#define PN_TOAST HUD_TOAST
#define PN_PANEL HUD_PANEL
#define PN_BADGE HUD_BADGE
/* XWPN version 2's adjusts[] covers rows 0..8 (BALANCE .. MIC TEST, accept/wp1_3.py,
 * accept/mictest.py); PRESET and SURROUND count in adjusts_hi[] (version 3), BASS, MIDS and
 * TREBLE in adjusts_custom[] (version 4) */
#define PN_ROWS (HUD_ROW_MICTEST + 1)
#define PN_ROWS_HI (HUD_ROW_SURROUND + 1 - PN_ROWS)
_Static_assert(PN_ROWS == 9 && PN_ROWS_HI == 2 && HUD_ROW_DEMO == HUD_ROW_TREBLE + 1 &&
	       HUD_ROWS == HUD_ROW_DEMO + 1 &&
	       HUD_ROW_BASS == HUD_ROW_SURROUND + 1, "XWPN adjusts[] + adjusts_hi[] + adjusts_custom[], "
	       "accept/wp1_3.py");
/* the CUSTOM rows are eq_360.h's bands in order; the panel draws the steps the engine takes */
_Static_assert(HUD_ROW_MIDS - HUD_ROW_BASS == XQ_CUSTOM_MIDS - XQ_CUSTOM_BASS &&
	       HUD_ROW_TREBLE - HUD_ROW_BASS == XQ_CUSTOM_TREBLE - XQ_CUSTOM_BASS &&
	       XQ_CUSTOM_BANDS == 3 && XQ_CUSTOM_MIN == -HP_CBAR_STEPS &&
	       XQ_CUSTOM_MAX == HP_CBAR_STEPS, "CUSTOM rows = eq_360.h XQ_CUSTOM_*");
_Static_assert(HUD_DESC_LEN == XQ_DESC_LEN, "hud_state.desc holds an xq_desc");
/* always shown: BALANCE .. NOISE GATE, PRESET (FLAT only without presets), TEST TONE */
#define PN_ROWS_BASE (1u << HUD_ROW_BALANCE | 1u << HUD_ROW_VOLUME | 1u << HUD_ROW_SIDETONE | \
		      1u << HUD_ROW_MIC | 1u << HUD_ROW_GATE | 1u << HUD_ROW_TONE |              \
		      1u << HUD_ROW_PRESET)
_Static_assert((int)HUD_MT_PLAYING == (int)XD_MT_PLAYING && (int)HUD_MT_RECORDING ==
	       (int)XD_MT_RECORDING && (int)HUD_MT_PAUSED == (int)XD_MT_PAUSED &&
	       (int)HUD_MT_READY == (int)XD_MT_READY, "hud_360.h HUD_MT_* = diag_360.h XD_MT_*");
_Static_assert((int)HUD_GATE_AUTO == (int)XD_GATE_AUTO && (int)HUD_GATE_1 == (int)XD_GATE_1 &&
	       (int)HUD_GATE_2 == (int)XD_GATE_2 && (int)HUD_GATE_3 == (int)XD_GATE_3 &&
	       (int)HUD_GATE_4 == (int)XD_GATE_4, "hud_360.h HUD_GATE_* = diag_360.h XD_GATE_*");

#define TAP_MS 400
#define HOLD_MS 250
#define TOAST_MS 1500
#define PUBLISH_MS 50 /* the bar's state, re-published this often while the panel is up */
#define INJECT_TIMEOUT_MS 20000
#define REPEAT_DELAY_MS 400 /* PRESET, CUSTOM: Left / Right held this long repeats ... */
#define REPEAT_MS 125       /* ... this often (8 a second: a 64-preset file in 8 s) */
#define BADGE_SHIFT_MS 60000 /* the mute mark moves one unit this often (burn-in) */
#define BADGE_PERIOD_MS 15000 /* badge_mode 1: shown BADGE_SHOW_MS out of every period */
#define BADGE_SHOW_MS 2000

#define INJ_SHARE 0x01
#define INJ_UP 0x02
#define INJ_DOWN 0x04
#define INJ_LEFT 0x08
#define INJ_RIGHT 0x10
#define INJ_ALL 0x1F

#define DPAD (GIP_GP_BTN_DPAD_U | GIP_GP_BTN_DPAD_D | GIP_GP_BTN_DPAD_L | GIP_GP_BTN_DPAD_R)
#define BUMPERS (GIP_GP_BTN_BUMPER_L | GIP_GP_BTN_BUMPER_R)

/* features(): what the rows may offer now */
#define PN_F_SURROUND 0x01  /* the SURROUND row */
#define PN_F_MIC_AUTO 0x02  /* MIC VOLUME AUTO */
#define PN_F_GATE_AUTO 0x04 /* NOISE GATE AUTO */
#define PN_F_TEAM 0x08      /* the TEAM VOL and TEAM CH rows */
#define PN_F_PRESETS 0x10   /* presets loaded (PRESET offers more than FLAT) */
#define PN_F_CUSTOM 0x20    /* CUSTOM is offered (PRESET's last value; its rows while selected) */
/* features_force only (XW_DEV), to see the new parts on the console before the engine has
 * them (accept/wp1_3.py --offer-custom): */
#define PN_F_CUSTOM_ROWS 0x40 /* the CUSTOM rows shown whatever preset is selected */
/* 0x80 was a 3-line test description for the description area, which is gone (the help line
 * shows the description): ignored, and free for a later use */
/* 0x100: the SURROUND DEMO row (with PN_F_SURROUND: it sits under SURROUND). XWPN keeps its
 * layout (accept/wp1_3.py checks version 5): the row has no adjusts[] counter; XWSR
 * demo_plays and demo_end count what it does. */
#define PN_F_DEMO 0x100
#define PN_F_ALL 0x17F

/* xbdm: "XWPN", all u32, append only; accept/wp1_3.py mirrors the order. Version 2: adjusts[]
 * has a 9th entry (MIC TEST), in what was reserved[1]'s place at the end, so every word after
 * adjusts[] moved down by one. Version 3 appends the rows, features and mute mark words;
 * version 4 the CUSTOM rows' counters and the description's lines; version 5 has no
 * description area (that last word stays, always 0) and ignores features_force 0x80. */
struct xw_panel_status {
	u32 magic, version;
	/* controls */
	volatile u32 inject;     /* INJ_* bits, applied as pad inject_pad's input */
	volatile u32 inject_pad; /* 0..3 */
	volatile u32 notify;     /* 1 = also an XNotify toast on a mute tap (off: the bar shows it) */
	volatile u32 tap_ms;     /* a Share press shorter than this is a tap */
	volatile u32 hold_ms;    /* Share held this long shows the panel */
	/* state */
	u32 visible;             /* PN_HIDDEN / PN_TOAST / PN_PANEL / PN_BADGE */
	u32 cursor;              /* HUD_ROW_* */
	u32 share_down;          /* bit n = pad n's Share is down (the controller's or injected) */
	u32 taps;                /* mute taps */
	u32 holds;               /* Share presses that showed the panel */
	u32 adjusts[PN_ROWS];    /* Left / Right presses per row */
	u32 masked_presses;      /* D-pad presses kept from the title */
	u32 tone_auto_offs;      /* the test tone switched off by the panel hiding / a pad leaving */
	u32 title_buttons[PADS]; /* GIP buttons as handed to xa_xinput_update (what the title gets) */
	u32 panel_pad;           /* the pad whose Share was pressed last, 0xFFFFFFFF = none */
	u32 publishes;           /* hud_publish calls */
	u32 empty_holds;         /* Share held past a tap and released with nothing else: ignored */
	u32 inject_timeouts;     /* injects cleared by INJECT_TIMEOUT_MS */
	volatile u32 bumpers;    /* 1 (default): Share + LB / RB = sidetone - / + (masked from the
				  * title like the D-pad); 0: bumpers pass through (PLAN.md 1.0) */
	/* ---- version 3 ---- */
	u32 adjusts_hi[PN_ROWS_HI]; /* Left / Right presses on PRESET, SURROUND */
	u32 rows;                /* hud_rows() shown now (bit = HUD_ROW_*) */
	u32 features;            /* PN_F_* in effect */
	volatile u32 features_force; /* XW_DEV builds only: PN_F_* bits offered as if the feature
				      * were there (rows and values to test before it lands) */
	volatile u32 badge;      /* 1 (default, the owner's call): the persistent mute mark while
				  * the mic is muted; 0: none */
	volatile u32 badge_mode; /* 0 (default) steady; 1 shown 2 s out of every 15 s (the fallback
				  * if the steady mark costs frame rate or shimmers) */
	u32 badge_shows;         /* times the mark came up */
	u32 repeats;             /* PRESET and CUSTOM steps made by holding Left / Right */
	u32 mictest_closes;      /* xd_mictest_panel_closed calls (the panel closed, no test
				  * recording or playing) */
	/* ---- version 4 ---- */
	u32 adjusts_custom[XQ_CUSTOM_BANDS]; /* Left / Right presses on BASS, MIDS, TREBLE */
	u32 unused45;            /* 0 (version 4: desc_lines, the description area's lines) */
};
_Static_assert(sizeof(struct xw_panel_status) == 46 * 4, "accept/wp1_3.py PN_FIELDS");

__attribute__((used)) struct xw_panel_status g_pn = {
	.magic = 0x5857504E, /* "XWPN" */
	.version = 5,
	.tap_ms = TAP_MS,
	.hold_ms = HOLD_MS,
	.panel_pad = 0xFFFFFFFF,
	.bumpers = 1,
	.badge = 1,
};

/* per pad: the thread's */
struct panel_pad {
	u8 share_real;  /* Share in the controller's last input packet */
	u8 share_down;  /* Share as the gestures see it: the controller's or injected */
	u8 share_used;  /* another button was pressed during this Share press */
	u8 shown;       /* this Share press has shown the panel */
	u8 have_state;  /* `last` holds a parsed input of this controller */
	u8 inj;         /* the INJ_* bits applied to this pad */
	u16 btn_prev;   /* the buttons (GIP) of the last input, injected D-pad included */
	u16 ctl_mask;   /* D-pad directions kept from the title */
	u64 share_down_ms;
	struct gip_gamepad_state last; /* the last parsed input, before panel_filter */
};

static struct panel_pad g_pads[PADS];
static u8 g_cursor;          /* HUD_ROW_*: the selected row */
static u8 g_visible;         /* PN_* */
static u64 g_toast_until;    /* after a mute tap; 0 = none */
static int g_panel_pad = -1; /* the pad whose Share was pressed last (its mute is shown) */
static u32 g_inj_last;       /* inject as last seen, and since when */
static u64 g_inj_since;
/* PRESET / CUSTOM hold-to-repeat: the pad and direction held (0 = none), the next step's
 * time */
static int g_rep_pad;
static int g_rep_dir;
static u8 g_rep_row; /* the row it started on: moving off it stops it */
static u64 g_rep_next;
/* the mute mark: since when it may show (0 = it may not), for badge_mode 1's period */
static u64 g_badge_since;

static struct panel_pad *pad_of(int pad)
{
	return pad >= 0 && pad < PADS ? &g_pads[pad] : NULL;
}

static const char *const row_names[HUD_ROWS] = {
	"balance", "volume", "sidetone", "mic volume", "noise gate", "test tone", "team",
	"channel", "mic test", "preset", "surround", "bass", "mids", "treble", "surround demo"
};

/* ---- what the features offer ---------------------------------------------------------------
 * The owners' "is it available" getters do not exist yet; each stand-in below names the one
 * to call once it does (and until then says no, so the panel looks as before the feature).
 * In XW_DEV builds XWPN `features_force` switches each on, so the rows can be tested over
 * xbdm against the stubs. */
static bool offered_surround(void)
{
	return xg_surround_offered(); /* the XWSR gate is on and its state was allocated */
}

static bool offered_mic_auto(void)
{
	return xd_mic_auto_offered(); /* the XWMA mic_auto switch is on */
}

static bool offered_gate_auto(void)
{
	return xd_gate_auto_offered(); /* the XWMA gate_auto switch is on */
}

static bool offered_demo(void)
{
	return xg_demo_offered(); /* surround offered and the xw.demo file found */
}

static bool offered_team(void)
{
	return false; /* team voice switched on: xt_offered() (team_360.h; the stub: false) */
}

static u32 features(void)
{
	u32 f = (xq_count() ? PN_F_PRESETS : 0) | (offered_surround() ? PN_F_SURROUND : 0) |
		(offered_mic_auto() ? PN_F_MIC_AUTO : 0) | (offered_gate_auto() ? PN_F_GATE_AUTO : 0) |
		(offered_team() ? PN_F_TEAM : 0) | (xq_custom_offered() ? PN_F_CUSTOM : 0) |
		(offered_demo() ? PN_F_DEMO : 0);

#ifdef XW_DEV
	f |= g_pn.features_force & PN_F_ALL;
#endif
	return f;
}

/* CUSTOM's preset index, 0 if it is not offered. Forced (XW_DEV) before the engine offers it,
 * xq_custom_index says 0: then where the contract puts it, after the file's presets. */
static u32 custom_index(u32 f)
{
	u32 i;

	if (!(f & PN_F_CUSTOM))
		return 0;
	i = xq_custom_index();
	return i ? i : xq_count() + 1;
}

/* the PRESET row's last value: CUSTOM, or the file's last preset (0: FLAT only) */
static u32 preset_last(u32 f)
{
	u32 c = custom_index(f);

	return c ? c : xq_count();
}

/* The PRESET row walks FLAT, CUSTOM (when offered), then the file's presets: the player's own
 * sound sits next to FLAT instead of behind the whole list. Only the walk has this order; the
 * engine's indexes stay as they are (CUSTOM after the file's presets; c = custom_index). */
static u32 preset_pos(u32 idx, u32 c)
{
	return !c || !idx ? idx : idx == c ? 1 : idx + 1;
}

static u32 preset_at(u32 pos, u32 c)
{
	return !c || !pos ? pos : pos == 1 ? c : pos - 1;
}

/* the CUSTOM rows are shown: CUSTOM is the selected preset */
static bool custom_rows(u32 f)
{
	u32 c = custom_index(f);

	return (f & PN_F_CUSTOM_ROWS) || (c && xq_preset() == c);
}

/* the rows on the panel now (hud_rows) */
static u32 rows_of(u32 f)
{
	return PN_ROWS_BASE | (xd_mictest_enabled() ? 1u << HUD_ROW_MICTEST : 0) |
	       (f & PN_F_SURROUND ? 1u << HUD_ROW_SURROUND : 0) |
	       ((f & PN_F_SURROUND) && (f & PN_F_DEMO) ? 1u << HUD_ROW_DEMO : 0) |
	       (f & PN_F_TEAM ? 1u << HUD_ROW_TEAM | 1u << HUD_ROW_CHANNEL : 0) |
	       (custom_rows(f) ? HUD_ROWS_CUSTOM : 0);
}

static u32 rows_shown(void)
{
	return rows_of(features());
}

/* a mic test is recording or playing back (it outlives the panel; the toast shows it) */
static bool mictest_running(void)
{
	u32 secs, pct, st = xd_mictest_state(&secs, &pct);

	return st == XD_MT_RECORDING || st == XD_MT_PLAYING;
}

/* the row's XWPN press counter; NULL for SURROUND DEMO (none: XWPN's layout is kept) */
static bool demo_playing(void)
{
	u32 secs;

	return xg_demo_playing(&secs);
}

static u32 *adjust_count(int row)
{
	return row == HUD_ROW_DEMO ? NULL
	       : row < PN_ROWS ? &g_pn.adjusts[row]
	       : row < HUD_ROW_BASS ? &g_pn.adjusts_hi[row - PN_ROWS]
	       : &g_pn.adjusts_custom[row - HUD_ROW_BASS];
}

/* ---- the on-screen headset bar (hud_360.h) ------------------------------------------------ */

static struct hud_state g_hud_last;
static u8 g_hud_force = 1; /* publish on the next pass even if nothing changed */
static u64 g_hud_next;     /* the next periodic publish while the panel is up */

static u8 clamp8(u32 v, u32 max)
{
	return (u8)(v > max ? max : v);
}

/* the state for the bar (hud_360.h version 7) */
static void hud_fill(struct hud_state *h, int pad, u64 now)
{
	u32 vu, f, rows, secs, pct, mt, preset;
	const char *name;

	memset(h, 0, sizeof(*h));
	h->visible = g_visible;
	if (g_visible == PN_BADGE) {
		/* only what the mark shows, so the mic meter and the like do not republish (and
		 * redraw) it while it is the only thing up */
		h->muted = h->headset = 1;
		h->badge = HUD_BADGE_MUTED;
		h->badge_shift = (u8)(now / BADGE_SHIFT_MS & 3);
		return;
	}
	vu = xd_mic_vu(pad);
	f = features();
	rows = rows_of(f);
	mt = xd_mictest_state(&secs, &pct);
	h->flash = g_toast_until != 0; /* the mute toast (else a toast shows the mic test) */
	h->muted = xv_mic_muted(pad);
	h->headset = xw_headset_live();
	h->game_on = xg_routed();
	h->cursor = g_cursor;
	h->rows = (u8)rows;
	h->rows_hi = (u8)(rows >> 8);
	h->balance = (s8)xg_balance();
	h->volume = (u8)(xg_master() / XG_MASTER_STEP);
	h->sidetone = (u8)xg_sidetone();
	h->mic_level = xd_mic_auto() ? HUD_MIC_AUTO : (u8)xd_mic_level(g_xd.mic_gain);
	/* 0 tells the renderer the AUTO values are not offered (its arrows) */
	h->mic_auto_level = f & (PN_F_MIC_AUTO | PN_F_GATE_AUTO) ? clamp8(xd_mic_auto_level(), 10) : 0;
	if ((f & (PN_F_MIC_AUTO | PN_F_GATE_AUTO)) && !h->mic_auto_level)
		h->mic_auto_level = 1;
	h->gate = (u8)(g_xd.gate < XD_GATES ? g_xd.gate : 0);
	h->tone = (u8)(g_xd.tone < XD_TONES ? g_xd.tone : 0);
	h->mic_vu = clamp8(vu, 10);
	h->gate_open = xd_gate_open(pad);
	h->mictest = (u8)(mt <= XD_MT_READY ? mt : XD_MT_IDLE);
	h->mictest_secs = clamp8(secs, 99);
	h->mictest_pct = clamp8(pct, 100);
	h->mictest_max = clamp8(xd_mictest_max_secs(), 255);
	preset = xq_preset();
	h->preset_count = clamp8(xq_count(), 254);
	h->preset_flags = f & PN_F_CUSTOM ? HUD_PF_CUSTOM : 0;
	h->preset = clamp8(preset, h->preset_count + (f & PN_F_CUSTOM ? 1 : 0));
	name = xq_name(preset);
	for (int i = 0; name && name[i] && i < HUD_NAME_LEN; i++)
		h->preset_name[i] = name[i];
	if (rows & HUD_ROWS_CUSTOM)
		for (u32 b = 0; b < XQ_CUSTOM_BANDS; b++) {
			int v = xq_custom(b);

			h->custom[b] = (s8)(v < XQ_CUSTOM_MIN ? XQ_CUSTOM_MIN : v > XQ_CUSTOM_MAX
					    ? XQ_CUSTOM_MAX : v);
		}
	/* the help line's text on PRESET; nothing elsewhere, so stepping presets from another
	 * row (a restored setting) does not repaint, and nothing without a file (the row's own
	 * help says none is installed, though FLAT has a description) */
	if (g_cursor == HUD_ROW_PRESET && (f & (PN_F_PRESETS | PN_F_CUSTOM))) {
		name = xq_desc(preset);
		for (int i = 0; name && name[i] && i < HUD_DESC_LEN - 1; i++)
			h->desc[i] = name[i];
	}
	if (rows & 1u << HUD_ROW_SURROUND) {
		h->surround = xg_surround() != 0;
		h->surround_live = xg_surround_live();
	}
	if (rows & 1u << HUD_ROW_DEMO) {
		h->demo = xg_demo_playing(&secs);
		h->demo_secs = h->demo ? clamp8(secs, 99) : 0;
	}
	if (rows & 1u << HUD_ROW_TEAM) {
		h->team_level = clamp8(xt_level(), 10);
		h->channel = clamp8(xt_channel(), 8);
		h->team_peers = clamp8(xt_peers(), 255);
		h->team_talking = (u8)xt_talking_mask();
	}
}

static void hud_update(u64 now)
{
	struct hud_state h;
	int hs_pad = xw_headset_pad();

	hud_fill(&h, g_panel_pad >= 0 ? g_panel_pad : hs_pad >= 0 ? hs_pad : 0, now);
	if (g_visible == PN_PANEL && now >= g_hud_next)
		g_hud_force = 1; /* 20 Hz while the panel is up (the mic meter) */
	if (!g_hud_force && !memcmp(&h, &g_hud_last, sizeof(h)))
		return;
	g_hud_force = 0;
	g_hud_next = now + PUBLISH_MS;
	g_hud_last = h;
	g_pn.publishes++;
	hud_publish(&h);
}

/* ---- rows ------------------------------------------------------------------------------ */

static void tone_off(const char *why)
{
	if (!g_xd.tone)
		return;
	g_xd.tone = 0;
	xd_poll(); /* applied and logged now */
	g_pn.tone_auto_offs++;
	xa_log("panel: test tone off (%s)\n", why);
}

static u32 step_clamped(u32 v, int d, u32 count)
{
	int n = (int)v + d;

	return n < 0 ? 0 : n >= (int)count ? count - 1 : (u32)n;
}

/* MIC VOLUME: AUTO sits left of 1. Entering or leaving AUTO never touches g_xd.mic_gain, so
 * Right from AUTO lands on the last manual step. Manual steps: the next value the audio code
 * accepts (xd_mic_gain_step: 3 dB, clamped at 25 and 560 %), applied and logged now. */
static void mic_adjust(int d, u32 f)
{
	if (xd_mic_auto()) {
		if (d > 0)
			xd_set_mic_auto(false);
		return;
	}
	if (d < 0 && xd_mic_level(g_xd.mic_gain) <= 1) {
		if (f & PN_F_MIC_AUTO)
			xd_set_mic_auto(true);
		return;
	}
	g_xd.mic_gain = xd_mic_gain_step(g_xd.mic_gain, d);
	xd_poll();
}

/* NOISE GATE: the panel's order is AUTO, OFF, 1, 2, 3, 4 (higher cuts more); the stored
 * numbers (XD_GATE_*) are what xbdm tools, scripts and saved settings hold, so they never
 * move. Without AUTO offered the row starts at OFF; a gate set to AUTO over xbdm still shows
 * (and Right leaves it for OFF). */
static const u8 GATE_ORDER[XD_GATES] = { XD_GATE_AUTO, XD_GATE_OFF, XD_GATE_1, XD_GATE_2,
					 XD_GATE_3,    XD_GATE_4 };

static u32 gate_step(u32 gate, int d, bool auto_ok)
{
	int pos = 0, lo = auto_ok ? 0 : 1;

	while (pos < XD_GATES && GATE_ORDER[pos] != gate)
		pos++;
	if (pos == XD_GATES)
		pos = 3; /* not a gate value (xd_poll corrects it): from step 2 */
	pos += d;
	if (pos < lo)
		return gate;
	return GATE_ORDER[pos >= XD_GATES ? XD_GATES - 1 : pos];
}

/* MIC TEST (docs/ROADMAP.md Part 3 item 2). Right: nothing held -> record; recording -> stop
 * and play; playing -> pause; paused -> play on; a clip held -> play it again. Left: discard,
 * whatever it is doing. The test tone goes off first: it would cover the playback. */
static void mictest_adjust(int pad, int d)
{
	u32 secs, pct, st = xd_mictest_state(&secs, &pct);
	int hs = xw_headset_pad();

	if (d < 0) {
		if (st != XD_MT_IDLE)
			xd_mictest_cancel("Left on MIC TEST");
		return;
	}
	tone_off("mic test");
	switch (st) {
	case XD_MT_IDLE:
		xd_mictest_start(hs >= 0 ? hs : pad);
		break;
	case XD_MT_RECORDING:
		xd_mictest_stop();
		break;
	case XD_MT_PLAYING:
		xd_mictest_pause();
		break;
	case XD_MT_PAUSED:
		xd_mictest_resume();
		break;
	case XD_MT_READY:
		xd_mictest_play();
		break;
	}
}

static void row_adjust(int pad, int d)
{
	u32 f = features();
	int before;

	if (!(rows_of(f) & 1u << g_cursor))
		return; /* the selected row went away (its feature did) */
	switch (g_cursor) {
	case HUD_ROW_BALANCE:
		before = xg_balance();
		xg_adjust(d, 0, 0);
		if (before != 0 && xg_balance() == 0)
			xv_tick(); /* landed on centre */
		break;
	case HUD_ROW_VOLUME:
		xg_adjust(0, d, 0);
		break;
	case HUD_ROW_SIDETONE:
		xg_adjust(0, 0, d);
		break;
	case HUD_ROW_MIC:
		mic_adjust(d, f);
		break;
	case HUD_ROW_GATE:
		g_xd.gate = gate_step(g_xd.gate, d, (f & PN_F_GATE_AUTO) != 0);
		xd_poll();
		break;
	case HUD_ROW_PRESET:
		xq_set_preset(preset_at(step_clamped(preset_pos(xq_preset(), custom_index(f)), d,
						     preset_last(f) + 1),
					custom_index(f)));
		break;
	case HUD_ROW_BASS:
	case HUD_ROW_MIDS:
	case HUD_ROW_TREBLE: {
		u32 band = XQ_CUSTOM_BASS + (u32)(g_cursor - HUD_ROW_BASS);
		int v = xq_custom(band) + d;

		xq_set_custom(band, v < XQ_CUSTOM_MIN ? XQ_CUSTOM_MIN : v > XQ_CUSTOM_MAX ? XQ_CUSTOM_MAX : v);
		break;
	}
	case HUD_ROW_SURROUND:
		xg_set_surround(d > 0);
		break;
	case HUD_ROW_DEMO: {
		u32 secs;

		if (xg_demo_playing(&secs)) {
			xg_demo_stop();
		} else if (d > 0) {
			tone_off("surround demo"); /* the tone holds game audio: it would hide the demo */
			xg_demo_start();
		}
		break;
	}
	case HUD_ROW_TEAM:
		xt_set_level(step_clamped(xt_level(), d, 11));
		break;
	case HUD_ROW_CHANNEL:
		xt_set_channel(step_clamped(xt_channel(), d, 9));
		break;
	case HUD_ROW_TONE:
		g_xd.tone = step_clamped(g_xd.tone < XD_TONES ? g_xd.tone : 0, d, XD_TONES);
		xd_poll();
		break;
	case HUD_ROW_MICTEST:
		mictest_adjust(pad, d);
		break;
	default:
		return;
	}
	if (adjust_count(g_cursor))
		(*adjust_count(g_cursor))++;
	xa_log("panel: %s %s: balance %d, volume %u, sidetone %u, mic %u%%%s, gate %u, preset %u "
	       "(custom %d %d %d), surround %u, tone %u\n", row_names[g_cursor], d > 0 ? "up" : "down",
	       xg_balance(), xg_master(), xg_sidetone(), g_xd.mic_gain, xd_mic_auto() ? " (auto)" : "",
	       g_xd.gate, xq_preset(), xq_custom(XQ_CUSTOM_BASS), xq_custom(XQ_CUSTOM_MIDS),
	       xq_custom(XQ_CUSTOM_TREBLE), xg_surround(), g_xd.tone);
}

/* rows whose Left / Right held repeats: PRESET (up to 64 presets one press each would be a chore)
 * and CUSTOM's (-6 to +6 is 12 presses) */
static bool repeat_row(int row)
{
	return row == HUD_ROW_PRESET || (HUD_ROWS_CUSTOM >> row & 1);
}

/* the row above / below in hud_row_order that is shown; on the first / last: stay */
static void cursor_move(int d)
{
	u32 shown = rows_shown();
	int at = 0;

	while (at < HUD_ROWS && hud_row_order[at] != g_cursor)
		at++;
	for (int i = at + d; i >= 0 && i < HUD_ROWS; i += d)
		if (shown & 1u << hud_row_order[i]) {
			g_cursor = hud_row_order[i];
			g_pn.cursor = g_cursor;
			return;
		}
}

/* PRESET, CUSTOM: Left / Right held repeats (repeat_row); anything else (another row, the
 * button or Share released, the panel gone) stops it */
static void repeat_start(int pad, int d, u64 now)
{
	g_rep_pad = pad;
	g_rep_dir = d;
	g_rep_row = g_cursor;
	g_rep_next = now + REPEAT_DELAY_MS;
}

static void repeat_poll(u64 now)
{
	struct panel_pad *p;
	u16 b = g_rep_dir < 0 ? GIP_GP_BTN_DPAD_L : GIP_GP_BTN_DPAD_R;

	if (!g_rep_dir)
		return;
	p = &g_pads[g_rep_pad];
	if (g_cursor != g_rep_row || g_visible != PN_PANEL || !p->share_down || !(p->btn_prev & b)) {
		g_rep_dir = 0;
		return;
	}
	if (now < g_rep_next)
		return;
	/* a late pass (the driver thread can lose the CPU ~16 ms) is not caught up in a burst */
	g_rep_next = now - g_rep_next > REPEAT_MS ? now + REPEAT_MS : g_rep_next + REPEAT_MS;
	g_pn.repeats++;
	row_adjust(g_rep_pad, g_rep_dir);
	g_hud_force = 1;
}

/* ---- gestures ---------------------------------------------------------------------------- */

static void panel_show(int pad, const char *why)
{
	struct panel_pad *p = &g_pads[pad];

	if (p->shown)
		return;
	p->shown = 1;
	g_pn.holds++;
	xa_log("panel: pad %d: panel shown (%s)\n", pad, why);
}

/* Share as the gestures see it: the controller's or injected; acts on a change */
static void share_sync(int pad)
{
	struct panel_pad *p = &g_pads[pad];
	u8 down = p->share_real || (p->inj & INJ_SHARE);
	u64 now = xa_now_ms();

	if (down == p->share_down)
		return;
	p->share_down = down;
	g_hud_force = 1;
	if (down) {
		p->share_down_ms = now;
		p->share_used = 0;
		p->shown = 0;
		/* every press starts on balance; on MIC TEST while a test records or plays, on
		 * SURROUND DEMO while it plays (Right and Left act on it at once, one Up is
		 * SURROUND) */
		g_cursor = mictest_running() && (rows_shown() & 1u << HUD_ROW_MICTEST)
				   ? HUD_ROW_MICTEST
			   : demo_playing() && (rows_shown() & 1u << HUD_ROW_DEMO) ? HUD_ROW_DEMO
										   : HUD_ROW_BALANCE;
		g_pn.cursor = g_cursor;
		g_toast_until = 0;
		g_panel_pad = pad;
		g_pn.panel_pad = (u32)pad;
		return;
	}
	if (!p->share_used && now - p->share_down_ms < g_pn.tap_ms) {
		bool muted = xv_toggle_mic_mute(pad);

		g_pn.taps++;
		g_toast_until = now + TOAST_MS;
		if (g_pn.notify)
			xa_xinput_notify(muted ? XA_NOTIFY_MIC_MUTED : XA_NOTIFY_MIC_ON);
	} else if (!p->share_used) {
		g_pn.empty_holds++;
		xa_log("panel: pad %d: Share held %u ms and released: nothing\n", pad,
		       (u32)(now - p->share_down_ms));
	}
	p->shown = 0;
}

/* the D-pad while Share is down; what reaches the title (st->buttons) */
static void filter_buttons(int pad, struct gip_gamepad_state *st)
{
	struct panel_pad *p = &g_pads[pad];
	u16 buttons = st->buttons, press;

	if (p->inj & INJ_UP)
		buttons |= GIP_GP_BTN_DPAD_U;
	if (p->inj & INJ_DOWN)
		buttons |= GIP_GP_BTN_DPAD_D;
	if (p->inj & INJ_LEFT)
		buttons |= GIP_GP_BTN_DPAD_L;
	if (p->inj & INJ_RIGHT)
		buttons |= GIP_GP_BTN_DPAD_R;
	press = buttons & (u16)~p->btn_prev;
	if (p->share_down && press) {
		u16 dp = press & DPAD, bp = g_pn.bumpers ? press & BUMPERS : 0;

		p->share_used = 1; /* not a mute tap any more */
		if (bp) {
			/* the hs-controls shortcut, kept because it is what hands reach for (LB/RB
			 * pressed on the panel and the sidetone never moved): sidetone -/+ from
			 * any row, the cursor shown on SIDETONE */
			p->ctl_mask |= bp;
			g_pn.masked_presses += (bp & GIP_GP_BTN_BUMPER_L ? 1 : 0) +
					       (bp & GIP_GP_BTN_BUMPER_R ? 1 : 0);
			panel_show(pad, "LB/RB");
			g_cursor = HUD_ROW_SIDETONE;
			g_pn.cursor = g_cursor;
			if (bp & GIP_GP_BTN_BUMPER_L)
				row_adjust(pad, -1);
			if (bp & GIP_GP_BTN_BUMPER_R)
				row_adjust(pad, 1);
			g_hud_force = 1;
		}
		if (dp) {
			p->ctl_mask |= dp;
			for (u16 b = dp; b; b &= (u16)(b - 1))
				g_pn.masked_presses++;
			panel_show(pad, "D-pad");
			if (dp & GIP_GP_BTN_DPAD_U)
				cursor_move(-1);
			if (dp & GIP_GP_BTN_DPAD_D)
				cursor_move(1);
			if (dp & GIP_GP_BTN_DPAD_L)
				row_adjust(pad, -1);
			if (dp & GIP_GP_BTN_DPAD_R)
				row_adjust(pad, 1);
			if ((dp & (GIP_GP_BTN_DPAD_L | GIP_GP_BTN_DPAD_R)) && repeat_row(g_cursor))
				repeat_start(pad, dp & GIP_GP_BTN_DPAD_L ? -1 : 1, xa_now_ms());
			g_hud_force = 1;
		}
	}
	p->btn_prev = buttons;
	p->ctl_mask &= buttons; /* released: the title gets it again */
	buttons &= (u16)~p->ctl_mask;
	st->buttons = buttons;
	g_pn.title_buttons[pad] = buttons;
}

/* ---- the mute mark ------------------------------------------------------------------------ */

/* XWPN `badge` is on and the headset's mic is muted: the mark may show (badge_mode 1: only
 * BADGE_SHOW_MS of every BADGE_PERIOD_MS from the moment it could first) */
static bool badge_due(int pad, u64 now)
{
	if (!g_pn.badge || !xw_headset_live() || !xv_mic_muted(pad)) {
		g_badge_since = 0;
		return false;
	}
	if (!g_badge_since)
		g_badge_since = now;
	return g_pn.badge_mode != 1 || (now - g_badge_since) % BADGE_PERIOD_MS < BADGE_SHOW_MS;
}

/* ---- the interface (panel_360.h) ---------------------------------------------------------- */

void panel_raw_input(int pad, u16 vid, u16 pid, const u8 *pkt, int len)
{
	struct panel_pad *p = pad_of(pad);
	int down;

	if (!p)
		return;
	down = pq_share_down(vid, pid, pkt, len);
	if (down < 0)
		return;
	p->share_real = (u8)down;
	share_sync(pad);
}

void panel_filter(int pad, struct gip_gamepad_state *st)
{
	struct panel_pad *p = pad_of(pad);

	if (!p)
		return;
	p->last = *st;
	p->have_state = 1;
	filter_buttons(pad, st);
}

/* a new controller in this slot, or the slot's controller left: its presses are not the next
 * one's, and a test tone it left on goes off */
void panel_pad_reset(int pad)
{
	struct panel_pad *p = pad_of(pad);

	if (!p)
		return;
	memset(p, 0, sizeof(*p));
	if (g_panel_pad == pad) {
		g_panel_pad = -1;
		g_pn.panel_pad = 0xFFFFFFFF;
	}
	if (g_rep_dir && g_rep_pad == pad)
		g_rep_dir = 0;
	g_pn.title_buttons[pad] = 0;
	tone_off("pad left");
	g_hud_force = 1;
}

void panel_init(void)
{
	g_hud_force = 1;
}

void panel_poll(void)
{
	u64 now = xa_now_ms();
	u32 inject = g_pn.inject, share = 0;
	int hs_pad = xw_headset_pad(), mark_pad;
	u8 vis, panel = 0;

	/* a stuck inject (the script died) is dropped */
	if (inject != g_inj_last) {
		g_inj_last = inject;
		g_inj_since = now;
	} else if (inject && now - g_inj_since >= INJECT_TIMEOUT_MS) {
		g_pn.inject = inject = g_inj_last = 0;
		g_pn.inject_timeouts++;
		xa_log("panel: inject unchanged for %u s: cleared\n", INJECT_TIMEOUT_MS / 1000);
	}

	for (int i = 0; i < PADS; i++) {
		struct panel_pad *p = &g_pads[i];
		u8 inj = g_pn.inject_pad == (u32)i ? (u8)(inject & INJ_ALL) : 0;

		if (inj != p->inj) {
			struct gip_gamepad_state st;

			p->inj = inj;
			share_sync(i);
			if (p->have_state)
				st = p->last;
			else
				memset(&st, 0, sizeof(st));
			filter_buttons(i, &st);
			if (p->have_state)
				xa_xinput_update(i, &st); /* the controller reports changes only */
		}
		if (p->share_down && !p->shown && now - p->share_down_ms >= g_pn.hold_ms)
			panel_show(i, "held");
		if (p->share_down && p->shown)
			panel = 1;
		if (p->share_down)
			share |= 1u << i;
	}

	if (g_toast_until && now >= g_toast_until)
		g_toast_until = 0;
	/* the panel; else the mute toast or a running mic test's; else the mute mark */
	mark_pad = g_panel_pad >= 0 ? g_panel_pad : hs_pad >= 0 ? hs_pad : 0;
	vis = panel ? PN_PANEL
	      : g_toast_until || mictest_running() ? PN_TOAST
	      : badge_due(mark_pad, now) ? PN_BADGE : PN_HIDDEN;
	if (g_visible == PN_PANEL && vis != PN_PANEL) {
		xa_log("panel: hidden\n");
		tone_off("panel hidden");
		/* a paused or held clip does not outlive the panel (a running test carries on,
		 * its toast showing it) */
		if (!mictest_running()) {
			xd_mictest_panel_closed();
			g_pn.mictest_closes++;
		}
	}
	if (vis == PN_BADGE && g_visible != PN_BADGE)
		g_pn.badge_shows++;
	if (vis != g_visible)
		g_hud_force = 1;
	g_visible = vis;
	g_pn.visible = vis;
	g_pn.share_down = share;
	g_pn.features = features();
	g_pn.rows = rows_of(g_pn.features);
	/* the CUSTOM rows went (another preset was picked, here or by xbdm / saved settings)
	 * with the cursor on one: back to PRESET, the row that brings them back */
	if ((HUD_ROWS_CUSTOM >> g_cursor & 1) && !(g_pn.rows >> g_cursor & 1)) {
		g_cursor = HUD_ROW_PRESET;
		g_pn.cursor = g_cursor;
		g_hud_force = 1;
	}
	/* SURROUND DEMO went (surround's gate off, or the file): back to SURROUND if it is there */
	if (g_cursor == HUD_ROW_DEMO && !(g_pn.rows >> HUD_ROW_DEMO & 1)) {
		g_cursor = g_pn.rows >> HUD_ROW_SURROUND & 1 ? HUD_ROW_SURROUND : HUD_ROW_BALANCE;
		g_pn.cursor = g_cursor;
		g_hud_force = 1;
	}
	repeat_poll(now);
	hud_update(now);
}
