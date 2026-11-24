// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The headset panel's picture (docs/PLAN.md 1.0.2, WP1.2): a pure function from the published
 * state (hud_360.h, struct hud_state v7) to a small canvas of palette indices. hud_360.c
 * (WP1.1) owns everything else: where the canvas goes on screen, its scale, the colours'
 * texel values, the sentinels' altered colours, the blit and the flush.
 *
 * No globals written, no kernel calls, integer only: host-testable, and safe on any thread.
 *
 * Layouts (canvas units; 2 px each at 720p, 3 at 1080p):
 *
 *   HUD_PANEL, headset connected (HP_W x hp_panel_h(rows): 124 for 9 rows, 144 for 11, 184
 *   for all 15):
 *
 *       HEADSET                                              [MIC MUTED]
 *       ----------------------------------------------------------------
 *     | BALANCE       |-----+---o----|             <  CHAT 2  >
 *       VOLUME        [][][][][][][][][][]               7
 *       SIDETONE      [][][][][][][][][][]              OFF
 *       MIC VOLUME    [][][][][][][][][][]  (live mic)  AUTO
 *       NOISE GATE    o  (lit while the gate passes)     LOW
 *       PRESET        CUSTOM               (the name across both columns)
 *         BASS        [][][][][][]|[][][][][][]          +3    (only while CUSTOM is
 *         MIDS        [][][][][][]|[][][][][][]           0     selected: steps -6..+6
 *         TREBLE      [][][][][][]|[][][][][][]          -2     lit out from the centre)
 *       SURROUND      o  (lit while 5.1 is arriving)     OFF
 *       SURROUND DEMO         <       PLAYING 12 SEC      >  (with SURROUND and the clip's
 *                                       (idle: PLAY)          file only; wide, as PRESET)
 *       TEST TONE                                        OFF
 *       MIC TEST                                        READY
 *       ----------------------------------------------------------------
 *       HOW LOUD TEAMMATES HEAR YOU          (the selected row's help line)
 *
 *     Rows are drawn in hud_row_order (hud_360.h), only those whose bit is set in
 *     hud_rows(), HP_ROW_PITCH apart. '|' on the left is the accent bar of the selected row
 *     (hud_state.cursor): its label and value are bright and its value sits between '<' and
 *     '>' (an arrow is dim at the end of the row's range). MIC MUTED is white on a HP_WARN
 *     box. PRESET's '<' sits left of the name (HP_NAME_X - 8).
 *     MIC TEST: idle READY; recording: label RECORDING (HP_WARN), the live mic meter, the
 *     seconds so far (7 SEC); playing: PLAYING (HP_ACCENT), a progress bar, the per cent
 *     played; paused: PAUSED, the bar, the per cent; a clip held: YOUR CLIP and its length.
 *     SURROUND DEMO: its label is long, so its value is centred in HP_DEMO_X .. + HP_DEMO_W
 *     with its '<' at HP_DEMO_X - 8: PLAY when ready, PLAYING and the seconds (label in
 *     HP_ACCENT) while it plays; both arrows lit then (Left and Right both stop it).
 *     The help line under the rows says what the selected row does (for MIC TEST: what
 *     Right and Left do now). On PRESET it says what the selected preset sounds like: its
 *     description (hud_state.desc) when there is one, cut to HP_HELP_COLS characters at a
 *     word (hp_help_cut); the shipped descriptions all fit whole (tools/presets/convert.py
 *     refuses a longer one). One line only: the panel's height depends on the rows alone.
 *
 *   HUD_PANEL, no headset (HP_SMALL_W x HP_SMALL_H): NO HEADSET, centred.
 *   HUD_TOAST (HP_SMALL_W x HP_SMALL_H): a dot (HP_WARN muted, HP_ACCENT on) and MIC MUTED
 *     or MIC ON, centred. Without `flash` while a mic test records or plays (the panel was
 *     let go of mid-test): a dot (HP_WARN recording, HP_ACCENT playing) and REC 9 SEC or
 *     PLAYING 40%.
 *   HUD_BADGE (HP_BADGE_W x HP_BADGE_H, anchor HP_ANCHOR_BADGE), with HUD_BADGE_MUTED: a
 *     dark-red dot and MUTED in grey, each with a one-unit dark shadow, on a transparent
 *     canvas (no box); badge_shift moves both one unit around a 2 x 2 square.
 *   HUD_HIDDEN (or an unknown value): an empty canvas (w = h = 0).
 *
 * The canvas: px[y * HP_W + x] for x < w, y < h, whatever w is (the stride is always HP_W).
 * HP_CLEAR (0xFF) = transparent: the panel's one-unit rounded corners, and the rest of each
 * row past w. Nothing past row h is written.
 */
#ifndef HUD_PANEL_H
#define HUD_PANEL_H

#include "hud_360.h"

_Static_assert(sizeof(struct hud_state) == 124, "hud_panel.c needs hud_360.h version 7");
_Static_assert(HUD_ROWS == 15, "hp_panel_h, HP_H_MAX: 15 rows at most");

#define HP_W 240          /* canvas units; drawn at integer scale 1/2/3 */
/* all 15 rows and the help line: 184. hud_360.c geometry() keeps scale 2 at 1152x640 (Halo 3)
 * up to 256 units and at 720p up to 288, scale 3 at 1080p up to 288, and 640x480 (scale 1)
 * takes up to 384: tests/test_panel.c proves each. Two canvases of HP_W x HP_H_MAX are
 * allocated at boot (hud_360.c struct hud_mem): every unit here is 480 bytes of heap. */
#define HP_H_MAX 184
/* HP_MARK, HP_MARK_TEXT: the mute mark only. Dim on purpose: it stays up for as long as the
 * mic is muted, over the game, and a bright mark would burn into a plasma or OLED screen. */
enum { HP_BG, HP_LABEL, HP_BRIGHT, HP_DIM, HP_ACCENT, HP_WARN, HP_MARK, HP_MARK_TEXT, HP_COLOURS };
#define HP_CLEAR 0xFF     /* transparent: leave the picture */
/* hp_canvas.anchor: where hud_360.c's geometry() places the canvas. BOTTOM_CENTRE is 0 so a
 * zeroed canvas is placed as before: bottom centre, its bottom edge on the 10 % margin.
 * BADGE: the persistent mute mark's spot (XH36 badge_pos picks it). */
enum { HP_ANCHOR_BOTTOM_CENTRE = 0, HP_ANCHOR_BADGE = 1 };
struct hp_canvas {
	u16 w, h;
	u8 anchor;                /* HP_ANCHOR_*, set by hp_build */
	u8 px[HP_W * HP_H_MAX];   /* palette indices; 0xFF = transparent */
};

/* pure function: no globals written, no kernel calls, integer only; host-testable */
void hp_build(struct hp_canvas *c, const struct hud_state *s);
/* the 4 sentinel positions (canvas units) hud_360.c uses for its repaint check: the first
 * four lit pixels, in scan order, of the layout's anchor text (HEADSET, NO HEADSET, the
 * toast's text, or the mark's MUTED). Each is a HP_LABEL or HP_BRIGHT unit (HP_MARK_TEXT for
 * the mark: shot.py's PANEL_TEXT does not list it; accept/badge.py checks XH36 sent_rgb)
 * that nothing else draws over. All zero for HUD_HIDDEN. */
void hp_sentinels(const struct hud_state *s, u16 xy[4][2]);

/* The suggested colours (0xRRGGBB) for each palette index. hud_360.c may use its own; if it
 * changes HP_LABEL or HP_BRIGHT, shot.py's PANEL_TEXT (the sentinels' base colours) must
 * follow. */
extern const u32 hp_palette[HP_COLOURS];

/* ---- layout constants (canvas units), mirrored by accept/p1_panel.py --------------------- */
#define HP_SMALL_W 120    /* toast and NO HEADSET */
#define HP_SMALL_H 15
#define HP_TEXT_H 7       /* the 5 x 7 font, 6 units per character */
#define HP_TITLE_X 12     /* HEADSET */
#define HP_TITLE_Y 5
#define HP_RULE_Y 16      /* the line under the title */
#define HP_ROW0_Y 20      /* the first row's text */
#define HP_ROW_PITCH 10
#define HP_BAR_X 4        /* the selected row's accent bar: 2 wide, from text y - 1, 9 tall */
#define HP_BAR_W 2
#define HP_BAR_H 9
#define HP_LABEL_X 12
#define HP_CTRL_X 80      /* slider, meter or dot */
#define HP_CTRL_W 79
#define HP_LT_X 174       /* '<' */
#define HP_VAL_X 182      /* the value, centred in HP_VAL_W */
#define HP_VAL_W 35
#define HP_GT_X 221       /* '>' */
#define HP_MUTE_W 59      /* the MIC MUTED box, right-aligned to HP_W - HP_TITLE_X */
#define HP_MUTE_H 11
#define HP_PAD_BOTTOM 5
#define HP_HELP_RULE 8    /* the help line's rule: this far below the last row's text */
#define HP_HELP_GAP 12    /* the help line's text: this far below the last row's text */
#define HP_NAME_X HP_CTRL_X /* PRESET: the name spans the control and value columns, */
#define HP_NAME_W 136     /* x 80..215: 22 characters, clear of the '>' at HP_GT_X */
#define HP_SUB_X 18       /* the CUSTOM rows' labels: one character in, under PRESET's */
#define HP_DEMO_X 102     /* SURROUND DEMO's value (PLAYING 12 SEC), centred in HP_DEMO_W, */
#define HP_DEMO_W 114     /* x 102..215; its '<' at HP_DEMO_X - 8, clear of the label */
/* the CUSTOM rows' bar: a centre tick at HP_CBAR_MID and 6 segments each side (5 wide, 1
 * apart), lit from the centre out to the step; x 83..155, inside the control column */
#define HP_CBAR_MID (HP_CTRL_X + HP_CTRL_W / 2)
#define HP_CBAR_SEG 5
#define HP_CBAR_STEPS 6
/* the help line holds this many characters from HP_LABEL_X (6 units each, the last 5: 215
 * units, inside the title margins). tools/presets/convert.py HELP_COLS and the 36 in
 * eq_360.c's FLAT / CUSTOM descriptions are this number: change them together. */
#define HP_HELP_COLS ((HP_W - 2 * HP_TITLE_X + 1) / 6)
_Static_assert(HP_HELP_COLS == 36 && HP_LABEL_X == HP_TITLE_X,
	       "convert.py HELP_COLS, eq_360.c: a description is one help line");
_Static_assert(HP_CBAR_MID - 2 - (HP_CBAR_STEPS - 1) * 6 - HP_CBAR_SEG + 1 >= HP_CTRL_X &&
	       HP_CBAR_MID + 2 + (HP_CBAR_STEPS - 1) * 6 + HP_CBAR_SEG <= HP_CTRL_X + HP_CTRL_W,
	       "the CUSTOM bar fits the control column");
_Static_assert(HP_SUB_X + 6 * 6 - 1 < HP_CTRL_X - 4, "TREBLE runs into its bar");
#define HP_BADGE_W 46     /* the mute mark (HP_ANCHOR_BADGE): a dot and MUTED, no box */
#define HP_BADGE_H 11
#define HP_BADGE_X 3      /* the mark's dot and text start here (badge_shift 0), its text at */
#define HP_BADGE_Y 1      /* + HP_BADGE_TEXT; shadow and shift take one unit each way */
#define HP_BADGE_TEXT 8
_Static_assert(HP_BADGE_X + HP_BADGE_TEXT + 5 * 6 - 1 + 2 <= HP_BADGE_W &&
	       HP_BADGE_Y + HP_TEXT_H + 2 <= HP_BADGE_H, "the mark, its shadow and its shift fit");
_Static_assert(HP_NAME_X + HP_NAME_W <= HP_GT_X - 5, "PRESET name runs into '>'");
_Static_assert(HP_LABEL_X + 13 * 6 - 1 + 4 <= HP_DEMO_X - 8 && HP_DEMO_X + HP_DEMO_W <= HP_GT_X - 5 &&
	       14 * 6 - 1 <= HP_DEMO_W, "SURROUND DEMO: label, '<', PLAYING 99 SEC and '>' apart");

/* the panel's height with these rows shown (hud_rows()): HP_ROW0_Y + n x HP_ROW_PITCH + 14
 * (the help line and the padding: HP_HELP_GAP - HP_ROW_PITCH + HP_TEXT_H + HP_PAD_BOTTOM);
 * 104 for 7 rows, 124 for 9, 144 for 11, 184 for all 15 */
static inline u16 hp_panel_h(u32 rows)
{
	int n = 0;

	for (int i = 0; i < HUD_ROWS; i++)
		n += rows >> i & 1;
	return (u16)(n ? HP_ROW0_Y + (n - 1) * HP_ROW_PITCH + HP_HELP_GAP + HP_TEXT_H + HP_PAD_BOTTOM
		       : HP_RULE_Y + 1 + HP_PAD_BOTTOM);
}
_Static_assert(HP_ROW0_Y + (HUD_ROWS - 1) * HP_ROW_PITCH + HP_HELP_GAP + HP_TEXT_H + HP_PAD_BOTTOM
		       == HP_H_MAX, "HP_H_MAX");

/* A description (at most HUD_DESC_LEN - 1 characters of s, or up to a NUL) as the help line
 * shows it: into out (HP_HELP_COLS + 1 bytes, NUL-terminated) whole if it fits, else cut
 * after the last whole word that does (a first word longer than the line is cut at
 * HP_HELP_COLS), with no trailing space. Returns the length; 0 for an empty or all-space
 * text (the row's own help is shown then). Pure: hp_build uses it, tests/test_panel.c
 * checks it. */
int hp_help_cut(const char *s, char *out);

#endif
