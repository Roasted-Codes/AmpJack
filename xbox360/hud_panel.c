// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The headset panel's picture (hud_panel.h): hud_state in, palette indices out. Plain, still
 * and instant: every setting at once, the selected row marked, nothing animated. The layouts
 * and their constants are described in hud_panel.h.
 *
 * Pure: writes only the caller's canvas, reads only its arguments and const tables; no FP, no
 * kernel calls, no writable globals (so no .bss or .data at all). Out-of-range state values
 * are clamped, never trusted (hud.py show writes them straight from xbdm).
 */
#include "hud_panel.h"

/* tests/test_panel.c: every text and shape's box, to prove nothing overlaps or leaves the
 * canvas. Nothing in the plugin build. */
#ifdef HP_HOST_TEST
enum { HP_T_TEXT, HP_T_SHAPE, HP_T_BOX };
void hp_test_box(int kind, int x, int y, int w, int h, const char *what);
#define TRACE(kind, x, y, w, h, what) hp_test_box(kind, x, y, w, h, what)
#else
#define TRACE(kind, x, y, w, h, what) ((void)0)
#endif

const u32 hp_palette[HP_COLOURS] = {
	0x141414, /* HP_BG: the panel, opaque by default */
	0xc8c8c8, /* HP_LABEL: labels and values */
	0xf4f4f4, /* HP_BRIGHT: the title, the selected row, the MIC MUTED text */
	0x4a4a4a, /* HP_DIM: unlit segments, the slider track, the rule, a spent arrow */
	0x107c10, /* HP_ACCENT: Xbox green; lit segments, the knob, the selection bar */
	0xc42b1c, /* HP_WARN: the MIC MUTED box, the toast's muted dot */
	0x8c1a10, /* HP_MARK: the mute mark's dot (dark red) */
	0x9a9a9a, /* HP_MARK_TEXT: the mute mark's MUTED (grey) */
};

/* ---- the font: 5 x 7, the hud_360.c set plus '<' and '>' ------------------------------------ */
static const char FONT_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-%+:/.<>";
static const u8 FONT[][HP_TEXT_H] = {
	{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* A */
	{ 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, /* B */
	{ 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, /* C */
	{ 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }, /* D */
	{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, /* E */
	{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, /* F */
	{ 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, /* G */
	{ 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* H */
	{ 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* I */
	{ 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, /* J */
	{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, /* K */
	{ 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, /* L */
	{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, /* M */
	{ 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, /* N */
	{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* O */
	{ 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, /* P */
	{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, /* Q */
	{ 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, /* R */
	{ 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, /* S */
	{ 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* T */
	{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* U */
	{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, /* V */
	{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, /* W */
	{ 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, /* X */
	{ 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 }, /* Y */
	{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, /* Z */
	{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 0 */
	{ 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 1 */
	{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, /* 2 */
	{ 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, /* 3 */
	{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 4 */
	{ 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, /* 5 */
	{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 6 */
	{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
	{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, /* 8 */
	{ 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, /* 9 */
	{ 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 }, /* - */
	{ 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 }, /* % */
	{ 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 }, /* + */
	{ 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 }, /* : */
	{ 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 }, /* / */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C }, /* . */
	{ 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 }, /* < */
	{ 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 }, /* > */
};
_Static_assert(sizeof(FONT) / sizeof(FONT[0]) == sizeof(FONT_CHARS) - 1, "font table");

static int glyph(char ch)
{
	for (int i = 0; FONT_CHARS[i]; i++)
		if (FONT_CHARS[i] == ch)
			return i;
	return -1; /* space and anything unknown: blank */
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static int text_w(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n ? n * 6 - 1 : 0;
}

/* ---- drawing into the canvas (clipped to w x h) ---------------------------------------------- */

static void fill(struct hp_canvas *c, int x0, int y0, int w, int h, u8 colour)
{
	int x1 = x0 + w, y1 = y0 + h;

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > c->w)
		x1 = c->w;
	if (y1 > c->h)
		y1 = c->h;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			c->px[y * HP_W + x] = colour;
}

static void text(struct hp_canvas *c, int x, int y, const char *s, u8 colour)
{
	TRACE(HP_T_TEXT, x, y, text_w(s), HP_TEXT_H, s);
	for (int n = 0; s[n]; n++) {
		int g = glyph(s[n]);

		if (g < 0)
			continue;
		for (int r = 0; r < HP_TEXT_H; r++)
			for (int b = 0; b < 5; b++)
				if (FONT[g][r] & (0x10 >> b))
					fill(c, x + n * 6 + b, y + r, 1, 1, colour);
	}
}

/* a 5 x 5 disc centred on (cx, cy) */
static void dot(struct hp_canvas *c, int cx, int cy, u8 colour)
{
	TRACE(HP_T_SHAPE, cx - 2, cy - 2, 5, 5, "dot");
	fill(c, cx - 2, cy - 1, 5, 3, colour);
	fill(c, cx - 1, cy - 2, 3, 5, colour);
}

/* 10 segments across the control column, 7 x 5 units, 1 apart; `lit` of them accented */
static void meter(struct hp_canvas *c, int y, int lit)
{
	TRACE(HP_T_SHAPE, HP_CTRL_X, y + 1, 79, 5, "meter");
	lit = clampi(lit, 0, 10);
	for (int k = 0; k < 10; k++)
		fill(c, HP_CTRL_X + k * 8, y + 1, 7, 5, k < lit ? HP_ACCENT : HP_DIM);
}

/* -5 (all game) .. +5 (all chat): a track with end ticks, a taller centre tick, the knob */
static void slider(struct hp_canvas *c, int y, int pos)
{
	TRACE(HP_T_SHAPE, HP_CTRL_X, y, HP_CTRL_W, HP_TEXT_H, "slider");
	int mid = HP_CTRL_X + HP_CTRL_W / 2, cx = mid + clampi(pos, -5, 5) * 7;

	fill(c, HP_CTRL_X, y + 3, HP_CTRL_W, 1, HP_DIM);
	fill(c, HP_CTRL_X, y + 2, 1, 3, HP_DIM);
	fill(c, HP_CTRL_X + HP_CTRL_W - 1, y + 2, 1, 3, HP_DIM);
	fill(c, mid, y, 1, HP_TEXT_H, HP_LABEL);
	dot(c, cx, y + 3, HP_ACCENT);
}

/* up to 8 small squares: consoles heard on the team channel, accented while talking */
static void peers(struct hp_canvas *c, int y, int n, u8 talking)
{
	TRACE(HP_T_SHAPE, HP_CTRL_X, y + 2, 8 * 8 - 5, 3, "peers");
	n = clampi(n, 0, 8);
	for (int k = 0; k < n; k++)
		fill(c, HP_CTRL_X + k * 8, y + 2, 3, 3, talking >> k & 1 ? HP_ACCENT : HP_LABEL);
}

/* a small unsigned number as text; buf holds at least 4 */
static const char *num(char *buf, int v)
{
	int i = 0;

	v = clampi(v, 0, 999);
	if (v >= 100)
		buf[i++] = (char)('0' + v / 100);
	if (v >= 10)
		buf[i++] = (char)('0' + v / 10 % 10);
	buf[i++] = (char)('0' + v % 10);
	buf[i] = 0;
	return buf;
}

/* ---- the rows ------------------------------------------------------------------------------- */

static const char *const ROW_LABEL[HUD_ROWS] = {
	[HUD_ROW_BALANCE] = "BALANCE",   [HUD_ROW_VOLUME] = "VOLUME",
	[HUD_ROW_SIDETONE] = "SIDETONE", [HUD_ROW_MIC] = "MIC VOLUME",
	[HUD_ROW_GATE] = "NOISE GATE",   [HUD_ROW_TONE] = "TEST TONE",
	[HUD_ROW_TEAM] = "TEAM VOL",     [HUD_ROW_CHANNEL] = "TEAM CH",
	[HUD_ROW_MICTEST] = "MIC TEST",  [HUD_ROW_PRESET] = "PRESET",
	[HUD_ROW_SURROUND] = "SURROUND", [HUD_ROW_BASS] = "BASS",
	[HUD_ROW_MIDS] = "MIDS",         [HUD_ROW_TREBLE] = "TREBLE",
	[HUD_ROW_DEMO] = "SURROUND DEMO",
};

/* the help line: what the selected row does, in words a player knows (at most HP_HELP_COLS,
 * 36 characters; the font has no comma). PRESET's is shown only when the selected preset has
 * no description (row_help). */
static const char *const ROW_HELP[HUD_ROWS] = {
	[HUD_ROW_BALANCE] = "< MORE GAME            MORE CHAT >",
	[HUD_ROW_VOLUME] = "EVERYTHING YOU HEAR",
	[HUD_ROW_SIDETONE] = "YOUR OWN VOICE IN YOUR EARS",
	[HUD_ROW_MIC] = "HOW LOUD TEAMMATES HEAR YOU",
	[HUD_ROW_GATE] = "HIGHER CUTS MORE BACKGROUND NOISE",
	[HUD_ROW_TONE] = "A STEADY BEEP TO CHECK YOUR EARS",
	[HUD_ROW_TEAM] = "TEAM CHAT VOLUME",
	[HUD_ROW_CHANNEL] = "TEAM CHAT CHANNEL",
	[HUD_ROW_MICTEST] = "> RECORD THEN TRY NOISE GATE ON IT",
	[HUD_ROW_PRESET] = "EQUALIZER FOR EVERYTHING YOU HEAR",
	[HUD_ROW_SURROUND] = "GAME 5.1 SOUND AROUND YOUR HEAD",
	/* CUSTOM's controls: what Left / Right change, as a player hears it (no frequencies) */
	[HUD_ROW_BASS] = "MORE OR LESS THUMP AND RUMBLE",
	[HUD_ROW_MIDS] = "MORE OR LESS VOICE AND BODY",
	[HUD_ROW_TREBLE] = "MORE OR LESS SPARKLE AND CRUNCH",
	[HUD_ROW_DEMO] = "> PLAY A 40 SECOND SURROUND SAMPLE",
};

static int mictest_of(const struct hud_state *s)
{
	return s->mictest <= HUD_MT_READY ? s->mictest : HUD_MT_IDLE;
}

/* the AUTO values (MIC VOLUME and NOISE GATE) can be picked: panel_360.c publishes
 * mic_auto_level 0 ("unknown") while they cannot. Only the arrows depend on it. */
static int auto_offered(const struct hud_state *s)
{
	return s->mic_auto_level != 0;
}

/* the help line's text for the selected row (buf: HP_HELP_COLS + 1, for a description) */
static const char *row_help(const struct hud_state *s, int row, char *buf)
{
	switch (row) {
	case HUD_ROW_MICTEST:
		switch (mictest_of(s)) {
		case HUD_MT_RECORDING:
			return "TALK NOW.   > PLAY IT   < DISCARD";
		case HUD_MT_PLAYING:
			return "THIS IS YOU.   > PAUSE   < DISCARD";
		case HUD_MT_PAUSED:
			return "> PLAY ON              < DISCARD";
		case HUD_MT_READY:
			return "> PLAY AGAIN           < DISCARD";
		}
		if (s->muted)
			return "TAP SHARE TO UNMUTE FIRST";
		break;
	case HUD_ROW_MIC:
		if (s->mic_level == HUD_MIC_AUTO)
			return "AUTO: KEEPS YOUR VOICE AT ONE LEVEL";
		break;
	case HUD_ROW_DEMO:
		if (s->demo)
			return "GO UP TO FLIP SURROUND.  > STOPS IT";
		break;
	case HUD_ROW_PRESET:
		if (!s->preset_count && !(s->preset_flags & HUD_PF_CUSTOM))
			return "NO PRESETS INSTALLED";
		/* what the selected preset sounds like, in place of what the row is */
		if (hp_help_cut(s->desc, buf))
			return buf;
		break;
	}
	return ROW_HELP[row];
}

/* row `row`'s label and its colour (the selected row's is bright) */
static const char *row_label(const struct hud_state *s, int row, int sel, u8 *ink)
{
	*ink = sel ? HP_BRIGHT : HP_LABEL;
	if (row == HUD_ROW_DEMO && s->demo)
		*ink = HP_ACCENT; /* playing, as MIC TEST's PLAYING */
	if (row != HUD_ROW_MICTEST)
		return ROW_LABEL[row];
	switch (mictest_of(s)) {
	case HUD_MT_RECORDING:
		*ink = HP_WARN;
		return "RECORDING";
	case HUD_MT_PLAYING:
		*ink = HP_ACCENT;
		return "PLAYING";
	case HUD_MT_PAUSED:
		return "PAUSED";
	case HUD_MT_READY:
		return "YOUR CLIP";
	}
	return ROW_LABEL[row];
}

/* a thin bar across the control column, `pct` % of it accented */
static void progress(struct hp_canvas *c, int y, int pct)
{
	TRACE(HP_T_SHAPE, HP_CTRL_X, y + 2, HP_CTRL_W, 3, "progress");
	fill(c, HP_CTRL_X, y + 2, HP_CTRL_W, 3, HP_DIM);
	fill(c, HP_CTRL_X, y + 2, HP_CTRL_W * clampi(pct, 0, 100) / 100, 3, HP_ACCENT);
}

/* CUSTOM's -6..+6: 6 segments each side of a centre tick, lit from the centre out to v (a
 * bar that grows left for a cut and right for a boost reads at a glance, even at 480p) */
static void cbar(struct hp_canvas *c, int y, int v)
{
	TRACE(HP_T_SHAPE, HP_CTRL_X, y, HP_CTRL_W, HP_TEXT_H, "custom bar");
	v = clampi(v, -HP_CBAR_STEPS, HP_CBAR_STEPS);
	fill(c, HP_CBAR_MID, y, 1, HP_TEXT_H, HP_LABEL);
	for (int k = 1; k <= HP_CBAR_STEPS; k++) {
		int off = 2 + (k - 1) * 6;

		fill(c, HP_CBAR_MID - off - HP_CBAR_SEG + 1, y + 1, HP_CBAR_SEG, 5,
		     v <= -k ? HP_ACCENT : HP_DIM);
		fill(c, HP_CBAR_MID + off, y + 1, HP_CBAR_SEG, 5, v >= k ? HP_ACCENT : HP_DIM);
	}
}

/* v followed by `tail` (buf: at least 12) */
static const char *num_with(char *buf, int v, const char *tail)
{
	int n = 0;

	num(buf, v);
	while (buf[n])
		n++;
	for (int i = 0; tail[i] && n < 11; i++)
		buf[n++] = tail[i];
	buf[n] = 0;
	return buf;
}

#define VAL_BUF (HUD_NAME_LEN + 4) /* row_draw's text: a preset name, or at most 11 */

/* row `row`'s control, at text line y; its value as text (buf: VAL_BUF) and where it stands
 * in its range (for the arrows) */
static const char *row_draw(struct hp_canvas *c, const struct hud_state *s, int row, int y,
			    char *buf, int *at_min, int *at_max)
{
	/* by HUD_GATE_* (stored number): OFF, 1, 2, AUTO, 3, 4 */
	static const char *const GATE[6] = { "OFF", "1", "2", "AUTO", "3", "4" };
	static const char *const TONE[3] = { "OFF", "440 HZ", "1 KHZ" };
	int v, hi;

	switch (row) {
	case HUD_ROW_BALANCE:
		v = clampi(s->balance, -5, 5);
		slider(c, y, v);
		*at_min = v == -5;
		*at_max = v == 5;
		if (!v)
			return "EVEN";
		/* the side that is louder, and by how many steps: GAME 3, CHAT 5 */
		for (int i = 0; i < 4; i++)
			buf[i] = (v < 0 ? "GAME" : "CHAT")[i];
		buf[4] = ' ';
		buf[5] = (char)('0' + (v < 0 ? -v : v));
		buf[6] = 0;
		return buf;
	case HUD_ROW_VOLUME:
		v = clampi(s->volume, 0, 10);
		meter(c, y, v);
		*at_min = v == 0;
		*at_max = v == 10;
		return num(buf, v);
	case HUD_ROW_SIDETONE:
		v = clampi(s->sidetone, 0, 10);
		meter(c, y, v);
		*at_min = v == 0;
		*at_max = v == 10;
		return v ? num(buf, v) : "OFF";
	case HUD_ROW_MIC:
		/* MIC VOLUME: the control is the live mic meter (after this volume, before the
		 * noise gate: what the teammates get while they hear you), in AUTO too; the
		 * value is the setting. AUTO sits left of 1. */
		meter(c, y, s->mic_vu);
		if (s->mic_level == HUD_MIC_AUTO) {
			*at_min = 1;
			*at_max = 0;
			return "AUTO";
		}
		v = clampi(s->mic_level, 1, 10);
		*at_min = v == 1 && !auto_offered(s);
		*at_max = v == 10;
		return num(buf, v);
	case HUD_ROW_GATE:
		/* the dot is lit while the gate passes the mic. The panel's order is AUTO, OFF,
		 * 1, 2, 3, 4 (panel_360.c); the stored numbers are HUD_GATE_*. */
		dot(c, HP_CTRL_X + 2, y + 3, s->gate_open ? HP_ACCENT : HP_DIM);
		v = clampi(s->gate, 0, 5);
		*at_min = v == HUD_GATE_AUTO || (v == HUD_GATE_OFF && !auto_offered(s));
		*at_max = v == HUD_GATE_4;
		return GATE[v];
	case HUD_ROW_PRESET:
		/* FLAT, CUSTOM if offered (its index is after the presets, its place in the walk
		 * right after FLAT: panel_360.c preset_pos), then the presets by name; drawn
		 * across both columns (build_panel) */
		hi = s->preset_count + (s->preset_flags & HUD_PF_CUSTOM ? 1 : 0);
		v = clampi(s->preset, 0, hi);
		*at_min = v == 0;
		*at_max = s->preset_flags & HUD_PF_CUSTOM ? v == (s->preset_count ? s->preset_count : hi)
							  : v >= hi;
		if (!v || !s->preset_name[0])
			return "FLAT";
		for (v = 0; v < HUD_NAME_LEN && s->preset_name[v]; v++)
			buf[v] = s->preset_name[v]; /* not NUL-terminated when all 20 are used */
		buf[v] = 0;
		return buf;
	case HUD_ROW_SURROUND:
		/* the dot is lit while the game sends sound on its centre / surround channels */
		dot(c, HP_CTRL_X + 2, y + 3, s->surround_live ? HP_ACCENT : HP_DIM);
		v = s->surround != 0;
		*at_min = !v;
		*at_max = v;
		return v ? "ON" : "OFF";
	case HUD_ROW_DEMO:
		/* Right plays; while it plays, Left and Right both stop it */
		*at_min = !s->demo;
		*at_max = 0;
		if (!s->demo)
			return "PLAY";
		for (v = 0; v < 8; v++)
			buf[v] = "PLAYING "[v];
		num_with(buf + 8, clampi(s->demo_secs, 0, 99), " SEC");
		return buf;
	case HUD_ROW_BASS:
	case HUD_ROW_MIDS:
	case HUD_ROW_TREBLE:
		/* CUSTOM's steps: the bar, and the step signed (+3, 0, -2) */
		v = clampi(s->custom[row - HUD_ROW_BASS], -HP_CBAR_STEPS, HP_CBAR_STEPS);
		cbar(c, y, v);
		*at_min = v == -HP_CBAR_STEPS;
		*at_max = v == HP_CBAR_STEPS;
		if (!v)
			return "0";
		buf[0] = v < 0 ? '-' : '+';
		num(buf + 1, v < 0 ? -v : v);
		return buf;
	case HUD_ROW_TONE:
		v = clampi(s->tone, 0, 2);
		*at_min = v == 0;
		*at_max = v == 2;
		return TONE[v];
	case HUD_ROW_TEAM:
		v = clampi(s->team_level, 0, 10);
		meter(c, y, v);
		*at_min = v == 0;
		*at_max = v == 10;
		return num(buf, v);
	case HUD_ROW_MICTEST:
		/* Right: record, stop and play, pause, play on, play again (not while muted and
		 * idle); Left: discard (nothing to discard while idle) */
		*at_min = *at_max = 0;
		switch (mictest_of(s)) {
		case HUD_MT_RECORDING:
			meter(c, y, s->mic_vu); /* the live mic, as on MIC VOLUME */
			return num_with(buf, clampi(s->mictest_secs, 0, 99), " SEC");
		case HUD_MT_PLAYING:
		case HUD_MT_PAUSED:
			progress(c, y, s->mictest_pct);
			return num_with(buf, clampi(s->mictest_pct, 0, 100), "%");
		case HUD_MT_READY:
			return num_with(buf, clampi(s->mictest_secs, 0, 99), " SEC");
		}
		*at_min = 1;
		*at_max = s->muted != 0;
		return s->muted ? "MUTED" : "READY";
	default: /* HUD_ROW_CHANNEL */
		v = clampi(s->channel, 0, 8);
		if (v)
			peers(c, y, s->team_peers, s->team_talking);
		*at_min = v == 0;
		*at_max = v == 8;
		return v ? num(buf, v) : "OFF";
	}
}

/* ---- the description in the help line ------------------------------------------------------ */

int hp_help_cut(const char *s, char *out)
{
	int n = 0, end = 0;

	while (n < HUD_DESC_LEN - 1 && s[n])
		n++;
	if (n > HP_HELP_COLS) {
		/* the last space that ends a whole word inside the line; none (one long first
		 * word): cut it at the line's end, so nothing is ever drawn past the panel */
		for (int i = HP_HELP_COLS; i > 0 && !end; i--)
			if (s[i] == ' ' && s[i - 1] != ' ')
				end = i;
		n = end ? end : HP_HELP_COLS;
	}
	while (n > 0 && s[n - 1] == ' ')
		n--;
	for (int i = 0; i < n; i++)
		out[i] = s[i];
	out[n] = 0;
	return n;
}

static void build_panel(struct hp_canvas *c, const struct hud_state *s)
{
	int line = 0, cur = s->cursor, y_last = -1;
	u32 rows = hud_rows(s);
	char help[HP_HELP_COLS + 1];

	text(c, HP_TITLE_X, HP_TITLE_Y, "HEADSET", HP_BRIGHT);
	if (s->muted) {
		int x = HP_W - HP_TITLE_X - HP_MUTE_W;

		TRACE(HP_T_BOX, x, HP_TITLE_Y - 2, HP_MUTE_W, HP_MUTE_H, "mute box");
		fill(c, x, HP_TITLE_Y - 2, HP_MUTE_W, HP_MUTE_H, HP_WARN);
		text(c, x + (HP_MUTE_W - text_w("MIC MUTED")) / 2, HP_TITLE_Y, "MIC MUTED", HP_BRIGHT);
	}
	fill(c, HP_TITLE_X, HP_RULE_Y, HP_W - 2 * HP_TITLE_X, 1, HP_DIM);

	for (int i = 0; i < HUD_ROWS; i++) {
		int row = hud_row_order[i];
		int y = HP_ROW0_Y + line * HP_ROW_PITCH, sel = row == cur, lo = 0, hi = 0;
		int lt = HP_LT_X, vx = HP_VAL_X, vw = HP_VAL_W;
		u8 ink = sel ? HP_BRIGHT : HP_LABEL, lab;
		char buf[VAL_BUF];
		const char *val, *name;

		if (!(rows >> row & 1))
			continue;
		line++;
		y_last = y;
		if (sel)
			fill(c, HP_BAR_X, y - 1, HP_BAR_W, HP_BAR_H, HP_ACCENT);
		name = row_label(s, row, sel, &lab);
		/* CUSTOM's rows one character in: they are PRESET's, not rows of their own */
		text(c, HUD_ROWS_CUSTOM >> row & 1 ? HP_SUB_X : HP_LABEL_X, y, name, lab);
		val = row_draw(c, s, row, y, buf, &lo, &hi);
		if (row == HUD_ROW_PRESET) {
			/* the name is wide (up to 20 characters): across the control and value
			 * columns, its '<' moved left of it */
			lt = HP_NAME_X - 8;
			vx = HP_NAME_X;
			vw = HP_NAME_W;
		} else if (row == HUD_ROW_DEMO) {
			/* the label runs into the control column: the value sits right of it */
			lt = HP_DEMO_X - 8;
			vx = HP_DEMO_X;
			vw = HP_DEMO_W;
		}
		text(c, vx + (vw - text_w(val)) / 2, y, val, ink);
		if (sel) {
			text(c, lt, y, "<", lo ? HP_DIM : HP_BRIGHT);
			text(c, HP_GT_X, y, ">", hi ? HP_DIM : HP_BRIGHT);
		}
	}
	if (y_last < 0)
		return;
	/* the help line: what the selected row does (blank if it isn't shown) */
	fill(c, HP_TITLE_X, y_last + HP_HELP_RULE, HP_W - 2 * HP_TITLE_X, 1, HP_DIM);
	if (cur >= 0 && cur < HUD_ROWS && (rows >> cur & 1))
		text(c, HP_LABEL_X, y_last + HP_HELP_GAP, row_help(s, cur, help), HP_LABEL);
}

/* ---- the anchor text: the layout's first text, which carries the sentinels ------------------ */

struct anchor {
	const char *s;
	int x, y;
	char buf[16];
};

/* a toast that shows the mic test (the panel was let go of while one records or plays)
 * instead of the mute: no `flash` (hud_360.h). Paused or held: no toast (the clip is gone
 * once the panel has closed, xd_mictest_panel_closed). */
static int toast_mictest(const struct hud_state *s)
{
	return s->visible == HUD_TOAST && !s->flash &&
	       (mictest_of(s) == HUD_MT_RECORDING || mictest_of(s) == HUD_MT_PLAYING);
}

/* the mic test toast's text: REC 9 SEC / PLAYING 40% (buf: at least 16) */
static const char *mictest_toast(const struct hud_state *s, char *buf)
{
	bool rec = mictest_of(s) == HUD_MT_RECORDING;
	const char *pre = rec ? "REC " : "PLAYING ";
	int n = 0;

	while (pre[n]) {
		buf[n] = pre[n];
		n++;
	}
	if (rec)
		num_with(buf + n, clampi(s->mictest_secs, 0, 99), " SEC");
	else
		num_with(buf + n, clampi(s->mictest_pct, 0, 100), "%");
	return buf;
}

/* the mute mark's burn-in steps (badge_shift): one unit each, around a 2 x 2 square */
static const u8 BADGE_SHIFT[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

/* 0 = nothing to show; else the canvas size and its anchor text */
static int layout(const struct hud_state *s, int *w, int *h, struct anchor *a)
{
	if (s->visible == HUD_PANEL && s->headset) {
		*w = HP_W;
		*h = hp_panel_h(hud_rows(s));
		a->s = "HEADSET";
		a->x = HP_TITLE_X;
		a->y = HP_TITLE_Y;
		return 1;
	}
	if (s->visible == HUD_PANEL || s->visible == HUD_TOAST) {
		/* the toast: a dot, a 4-unit gap, the text; centred together */
		int dw = s->visible == HUD_TOAST ? 9 : 0;

		*w = HP_SMALL_W;
		*h = HP_SMALL_H;
		if (toast_mictest(s))
			a->s = mictest_toast(s, a->buf);
		else
			a->s = s->visible == HUD_PANEL ? "NO HEADSET" : s->muted ? "MIC MUTED" : "MIC ON";
		a->x = (HP_SMALL_W - dw - text_w(a->s)) / 2 + dw;
		a->y = (HP_SMALL_H - HP_TEXT_H) / 2;
		return 1;
	}
	if (s->visible == HUD_BADGE && (s->badge & HUD_BADGE_MUTED)) {
		const u8 *d = BADGE_SHIFT[s->badge_shift & 3];

		*w = HP_BADGE_W;
		*h = HP_BADGE_H;
		a->s = "MUTED";
		a->x = HP_BADGE_X + d[0] + HP_BADGE_TEXT;
		a->y = HP_BADGE_Y + d[1];
		return 1;
	}
	*w = *h = 0;
	return 0;
}

/* the mute mark: a one-unit shadow first (right and down), then the ink over it, so it
 * reads on a bright picture as well as a dark one without a box */
static void build_badge(struct hp_canvas *c, const struct anchor *a)
{
	int x = a->x - HP_BADGE_TEXT, y = a->y;

	dot(c, x + 3, y + 4, HP_BG);
	text(c, a->x + 1, y + 1, a->s, HP_BG);
	dot(c, x + 2, y + 3, HP_MARK);
	text(c, a->x, y, a->s, HP_MARK_TEXT);
}

void hp_build(struct hp_canvas *c, const struct hud_state *s)
{
	struct anchor a;
	int w, h, badge = s->visible == HUD_BADGE;

	layout(s, &w, &h, &a);
	c->w = (u16)w;
	c->h = (u16)h;
	c->anchor = badge ? HP_ANCHOR_BADGE : HP_ANCHOR_BOTTOM_CENTRE;
	/* the mark has no background: transparent but for its ink and shadow */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < HP_W; x++)
			c->px[y * HP_W + x] = x < w && !badge ? HP_BG : HP_CLEAR;
	if (!w)
		return;
	if (badge) {
		build_badge(c, &a);
		return;
	}
	/* rounded corners */
	c->px[0] = c->px[w - 1] = HP_CLEAR;
	c->px[(h - 1) * HP_W] = c->px[(h - 1) * HP_W + w - 1] = HP_CLEAR;

	if (s->visible == HUD_PANEL && s->headset) {
		build_panel(c, s);
	} else if (s->visible == HUD_PANEL) {
		text(c, a.x, a.y, a.s, HP_LABEL);
	} else if (toast_mictest(s)) {
		dot(c, a.x - 7, a.y + 3, mictest_of(s) == HUD_MT_RECORDING ? HP_WARN : HP_ACCENT);
		text(c, a.x, a.y, a.s, HP_BRIGHT);
	} else {
		dot(c, a.x - 7, a.y + 3, s->muted ? HP_WARN : HP_ACCENT);
		text(c, a.x, a.y, a.s, HP_BRIGHT);
	}
}

void hp_sentinels(const struct hud_state *s, u16 xy[4][2])
{
	struct anchor a;
	int w, h, k = 0;

	for (int i = 0; i < 4; i++)
		xy[i][0] = xy[i][1] = 0;
	if (!layout(s, &w, &h, &a))
		return;
	/* the anchor text's first four lit pixels in scan order (each string has more) */
	for (int r = 0; r < HP_TEXT_H && k < 4; r++)
		for (int n = 0; a.s[n] && k < 4; n++) {
			int g = glyph(a.s[n]);

			if (g < 0)
				continue;
			for (int b = 0; b < 5 && k < 4; b++)
				if (FONT[g][r] & (0x10 >> b)) {
					xy[k][0] = (u16)(a.x + n * 6 + b);
					xy[k][1] = (u16)(a.y + r);
					k++;
				}
		}
}
