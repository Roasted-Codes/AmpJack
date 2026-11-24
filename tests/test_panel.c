// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of the headset panel: the picture (xbox360/hud_panel.c), where it lands in a
 * front buffer (xbox360/hud_360.c, HUD_HOST_TEST) and the controls behind it
 * (xbox360/panel_360.c, against stubs of the audio side below).
 *
 *   1. Pictures: hp_build for every layout (each row selected, 8 / 9 / 11 rows, the AUTO
 *      values, the preset names, every mic test state, the toasts, no headset, the mute mark
 *      at each burn-in step): the canvas height, nothing drawn outside w x h, no text
 *      overlapping other text or a control (hud_panel.c's HP_HOST_TEST boxes), the
 *      sentinels on lit units. Each is written to OUT/<name>.ppm (palette colours, 3 px a
 *      unit; transparent units as a grey check). The version 4 pictures must hash as before
 *      CUSTOM and the description (GOLDEN, test_panel_golden.h).
 *   1b. The description in the help line: hp_help_cut (cases, the two worst 63-character
 *      texts, 200000 random texts checked as properties: at most 36 characters, cut after
 *      the last whole word that fits); PRESET's help line is the description, every other
 *      row keeps its own, CUSTOM's rows say what they change, the CUSTOM bars (v6_*); every
 *      description that ships (tools/presets/xw.presets, FLAT's and CUSTOM's in eq_360.c)
 *      fits the line whole.
 *   2b. Fit: all 14 rows + help, a too-long description on PRESET, drawn at 640x480,
 *      1280x720, 1920x1080 and 1152x640: inside the safe area at scale 1, 2, 3, 2
 *      (frame6_*).
 *   2. Placement: hud_360.c's draw() into a fake 1280x720 linear 8_8_8_8 and a 1152x640
 *      tiled 2_10_10_10 front buffer (Halo 3's): the panel bottom centre in the safe area,
 *      the mark in the band under it (badge_pos 0) or in each corner, never overlapping the
 *      panel's place, transparent around its ink. Frames to OUT/frame_*.ppm.
 *   3. Controls: panel_360.c driven through XWPN inject on a fake clock: cursor order and
 *      skipping, the gate order table, MIC VOLUME AUTO, the mic test's Right / Left
 *      machine and xd_mictest_panel_closed, PRESET hold-to-repeat timing, the mute mark
 *      (steady, off, 2 s of 15, the burn-in shift), what is published.
 *   3b. CUSTOM and the description (fakes of eq_360.h's xq_desc / xq_custom_*): nothing new
 *      published while neither is offered; the description published only with the cursor
 *      on PRESET, the height fixed while stepping presets;
 *      CUSTOM after the last preset, its rows appearing / hiding, the cursor walking and
 *      skipping them and moving to PRESET when they go, hold-to-repeat on them; the XWPN
 *      features_force bits 0x20 / 0x40, and 0x80 ignored.
 *
 *   cc -O1 -Wall -w -DHP_HOST_TEST -DHUD_HOST_TEST -DXW_DEV -o test_panel tests/test_panel.c \
 *      xbox360/hud_panel.c xbox360/panel_360.c && ./test_panel [OUT]
 * (OUT defaults to out/test_panel; -w: hud_360.c's 32-bit pointer casts.)
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../xbox360/hud_360.c" /* static draw(), geometry(), g_pub, g_xh */
#include "../xbox360/diag_360.h"
#include "../xbox360/panel_360.h"
#include "../xbox360/xinput_360.h"

static int g_fail, g_checks;
static const char *g_out = "out/test_panel";

static void check(int ok, const char *fmt, ...)
{
	va_list ap;

	g_checks++;
	if (ok)
		return;
	g_fail++;
	printf("FAIL ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* ---- what hud_360.c needs on the host ---------------------------------------------------- */
u32 hud_host_ticks;
u8 *hud_host_mem;
static u64 g_now = 1000000;

u64 xa_now_ms(void) { return g_now; }
void xa_sleep_ms(unsigned int ms) { g_now += ms; }
void xa_log(const char *fmt, ...) { (void)fmt; }
int detour_install(struct detour *d, const char *name, void *target, const void *hook)
{
	return -1;
}
bool detour_known(const void *target) { return false; }

/* ---- 1. pictures ------------------------------------------------------------------------- */
#define MAXBOX 128
static struct box {
	int kind, x, y, w, h;
	char what[40]; /* a help line: up to 36 */
} g_box[MAXBOX];
static int g_nbox;

void hp_test_box(int kind, int x, int y, int w, int h, const char *what)
{
	if (g_nbox < MAXBOX) {
		struct box *b = &g_box[g_nbox++];

		b->kind = kind;
		b->x = x;
		b->y = y;
		b->w = w;
		b->h = h;
		snprintf(b->what, sizeof(b->what), "%s", what);
	}
}

enum { HP_T_TEXT, HP_T_SHAPE, HP_T_BOX }; /* hud_panel.c */

static int meets(const struct box *a, const struct box *b, int gap)
{
	return a->x < b->x + b->w + gap && b->x < a->x + a->w + gap && a->y < b->y + b->h &&
	       b->y < a->y + a->h;
}

static int inside(const struct box *a, const struct box *b)
{
	return a->x >= b->x && a->y >= b->y && a->x + a->w <= b->x + b->w &&
	       a->y + a->h <= b->y + b->h;
}

static struct hp_canvas g_cv;

/* a checker for transparent units: shows what the mark leaves to the game */
static void unit_rgb(u8 c, int x, int y, u8 *rgb)
{
	u32 v = c < HP_COLOURS ? hp_palette[c] : ((x ^ y) & 1) ? 0x707070 : 0x909090;

	rgb[0] = (u8)(v >> 16);
	rgb[1] = (u8)(v >> 8);
	rgb[2] = (u8)v;
}

static void write_ppm(const char *name, const u8 *rgb, int w, int h)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s.ppm", g_out, name);
	f = fopen(path, "wb");
	if (!f) {
		check(0, "write %s", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(rgb, 3, (size_t)w * h, f);
	fclose(f);
}

static void canvas_ppm(const char *name, const struct hp_canvas *c)
{
	enum { S = 3 };
	int w = c->w, h = c->h;
	u8 *rgb = malloc((size_t)w * h * S * S * 3);

	for (int y = 0; y < h * S; y++)
		for (int x = 0; x < w * S; x++)
			unit_rgb(c->px[(y / S) * HP_W + x / S], x / S, y / S, &rgb[(y * w * S + x) * 3]);
	write_ppm(name, rgb, w * S, h * S);
	free(rgb);
}

/* The pictures of hud_state version 4 (no CUSTOM, no description), as hud_panel.c drew them
 * before both existed: FNV-1a of w, h and the w x h units. A state whose version 5 bytes are
 * zero must still draw exactly these (TEST_PANEL_GOLDEN=1 prints the table). */
static const struct golden {
	const char *name;
	u32 hash;
} GOLDEN[] = {
#include "test_panel_golden.h"
};

static u32 canvas_hash(const struct hp_canvas *c)
{
	u32 h = 2166136261u;

	h = (h ^ c->w) * 16777619u;
	h = (h ^ c->h) * 16777619u;
	for (int y = 0; y < c->h; y++)
		for (int x = 0; x < c->w; x++)
			h = (h ^ c->px[y * HP_W + x]) * 16777619u;
	return h;
}

static void golden_check(const char *name, const struct hp_canvas *c)
{
	u32 h = canvas_hash(c);

	if (getenv("TEST_PANEL_GOLDEN")) {
		printf("\t{ \"%s\", 0x%08x },\n", name, h);
		return;
	}
	for (u32 i = 0; i < sizeof(GOLDEN) / sizeof(GOLDEN[0]); i++)
		if (!strcmp(GOLDEN[i].name, name)) {
			check(GOLDEN[i].hash == h, "%s: picture %08x differs from before CUSTOM and the "
			      "description (%08x)", name, h, GOLDEN[i].hash);
			return;
		}
}

/* hp_build + the checks; `name` also names the picture */
static void render(const char *name, const struct hud_state *s, int want_h)
{
	u16 sxy[4][2];
	struct box all;
	int w, h;

	g_nbox = 0;
	memset(&g_cv, 0xAB, sizeof(g_cv));
	hp_build(&g_cv, s);
	golden_check(name, &g_cv);
	w = g_cv.w;
	h = g_cv.h;
	if (want_h >= 0)
		check(h == want_h, "%s: canvas height %d, want %d", name, h, want_h);
	check(w <= HP_W && h <= HP_H_MAX, "%s: canvas %dx%d too big", name, w, h);
	check(g_cv.anchor == (s->visible == HUD_BADGE ? HP_ANCHOR_BADGE : HP_ANCHOR_BOTTOM_CENTRE),
	      "%s: anchor %d", name, g_cv.anchor);
	/* every unit of the h rows is set; nothing past w */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < HP_W; x++) {
			u8 c = g_cv.px[y * HP_W + x];

			if (x >= w && c != HP_CLEAR) {
				check(0, "%s: unit (%d,%d) past w = %d is %d", name, x, y, w, c);
				return;
			}
			if (c != HP_CLEAR && c >= HP_COLOURS) {
				check(0, "%s: unit (%d,%d) is %d, not a colour", name, x, y, c);
				return;
			}
		}
	/* boxes: inside the canvas (one unit clear of its edge: rounded corners), and no text
	 * on text or on a control (the mark's shadow is meant to sit under its ink) */
	all.x = all.y = 1;
	all.w = w - 2;
	all.h = h - 2;
	if (w == HP_W) {
		/* the panel: everything within the title's margin on both sides (help lines of
		 * at most 36 characters) */
		all.x = HP_BAR_X;
		all.w = HP_W - HP_TITLE_X - HP_BAR_X;
	}
	for (int i = 0; i < g_nbox; i++) {
		struct box *a = &g_box[i];

		if (a->kind == HP_T_TEXT && !*a->what)
			continue;
		check(inside(a, &all), "%s: %s '%s' at (%d,%d) %dx%d leaves the %dx%d canvas", name,
		      a->kind == HP_T_TEXT ? "text" : "shape", a->what, a->x, a->y, a->w, a->h, w, h);
		if (s->visible == HUD_BADGE)
			continue;
		for (int j = i + 1; j < g_nbox; j++) {
			struct box *b = &g_box[j];

			if (b->kind == HP_T_TEXT && !*b->what)
				continue;
			if (a->kind == HP_T_SHAPE && b->kind == HP_T_SHAPE)
				continue; /* shapes are placed by hand per row: checked by eye */
			if ((a->kind == HP_T_BOX && inside(b, a)) || (b->kind == HP_T_BOX && inside(a, b)))
				continue;
			check(!meets(a, b, a->kind == HP_T_TEXT && b->kind == HP_T_TEXT ? 1 : 0),
			      "%s: '%s' (%d,%d %dx%d) overlaps '%s' (%d,%d %dx%d)", name, a->what, a->x,
			      a->y, a->w, a->h, b->what, b->x, b->y, b->w, b->h);
		}
	}
	/* the sentinels: lit units, in the canvas */
	hp_sentinels(s, sxy);
	if (w)
		for (int k = 0; k < 4; k++) {
			u8 c = g_cv.px[sxy[k][1] * HP_W + sxy[k][0]];

			check(sxy[k][0] < w && sxy[k][1] < h && c < HP_COLOURS && c != HP_BG,
			      "%s: sentinel %d at (%d,%d) on colour %d", name, k, sxy[k][0], sxy[k][1], c);
		}
	if (w)
		canvas_ppm(name, &g_cv);
}

static struct hud_state base_state(void)
{
	struct hud_state s;

	memset(&s, 0, sizeof(s));
	s.visible = HUD_PANEL;
	s.headset = 1;
	s.game_on = 1;
	s.rows = 0x3F;      /* BALANCE .. TEST TONE */
	s.rows_hi = 1 | 2;  /* MIC TEST, PRESET */
	s.balance = -2;
	s.volume = 7;
	s.sidetone = 3;
	s.mic_level = 5;
	s.gate = HUD_GATE_2;
	s.mic_vu = 4;
	s.gate_open = 1;
	s.mictest_max = 30;
	return s;
}

static void name_set(struct hud_state *s, const char *n)
{
	memset(s->preset_name, 0, HUD_NAME_LEN);
	memcpy(s->preset_name, n, strlen(n) < HUD_NAME_LEN ? strlen(n) : HUD_NAME_LEN);
}

static void test_pictures(void)
{
	struct hud_state s = base_state();
	char name[64];

	/* 8 rows (no team, no surround): every row selected */
	for (int i = 0; i < HUD_ROWS; i++) {
		int r = hud_row_order[i];

		if (!(hud_rows(&s) >> r & 1))
			continue;
		s.cursor = (u8)r;
		snprintf(name, sizeof(name), "rows8_sel%02d", r);
		render(name, &s, 114);
	}
	/* 9 rows: SURROUND too, live dot */
	s.rows_hi |= 4;
	s.surround = 1;
	s.surround_live = 1;
	s.cursor = HUD_ROW_SURROUND;
	render("rows9_surround", &s, 124);
	/* 11 rows: team voice; every row selected */
	s.rows |= 1 << HUD_ROW_TEAM | 1 << HUD_ROW_CHANNEL;
	s.team_level = 6;
	s.channel = 3;
	s.team_peers = 3;
	s.team_talking = 2;
	for (int i = 0; i < HUD_ROWS; i++) {
		s.cursor = hud_row_order[i];
		snprintf(name, sizeof(name), "rows11_sel%02d", s.cursor);
		render(name, &s, 144);
	}
	/* AUTO values, presets with names (the longest is 20 characters), muted */
	s.mic_level = HUD_MIC_AUTO;
	s.mic_auto_level = 6;
	s.gate = HUD_GATE_AUTO;
	s.preset = 3;
	s.preset_count = 29;
	name_set(&s, "HALO FOOTSTEPS");
	s.cursor = HUD_ROW_MIC;
	render("auto_mic", &s, 144);
	s.cursor = HUD_ROW_GATE;
	render("auto_gate", &s, 144);
	s.cursor = HUD_ROW_PRESET;
	render("preset_name", &s, 144);
	name_set(&s, "WWWWWWWWWWWWWWWWWWWW"); /* 20 wide letters: the widest name */
	s.muted = 1;
	render("preset_20_muted", &s, 144);
	s.preset = 29;
	name_set(&s, "TWENTY CHARS NAME 29");
	render("preset_last", &s, 144);
	s.preset = 0;
	s.preset_count = 0;
	render("preset_flat_only", &s, 144);
	/* the mic test, each state, selected */
	s = base_state();
	s.cursor = HUD_ROW_MICTEST;
	render("mt_idle", &s, 114);
	s.muted = 1;
	render("mt_idle_muted", &s, 114);
	s.muted = 0;
	s.mictest = HUD_MT_RECORDING;
	s.mictest_secs = 27;
	s.mic_vu = 6;
	render("mt_recording", &s, 114);
	s.mictest = HUD_MT_PLAYING;
	s.mictest_secs = 27;
	s.mictest_pct = 40;
	render("mt_playing", &s, 114);
	s.mictest_pct = 100;
	render("mt_playing_100", &s, 114);
	s.mictest = HUD_MT_PAUSED;
	s.mictest_pct = 40;
	render("mt_paused", &s, 114);
	s.mictest = HUD_MT_READY;
	s.mictest_secs = 30;
	render("mt_ready", &s, 114);
	s.mictest = 9; /* unknown: as idle */
	render("mt_unknown", &s, 114);
	/* the toasts */
	s = base_state();
	s.visible = HUD_TOAST;
	s.flash = 1;
	s.muted = 1;
	render("toast_muted", &s, HP_SMALL_H);
	s.muted = 0;
	render("toast_on", &s, HP_SMALL_H);
	s.flash = 0;
	s.mictest = HUD_MT_RECORDING;
	s.mictest_secs = 12;
	render("toast_rec", &s, HP_SMALL_H);
	s.mictest = HUD_MT_PLAYING;
	s.mictest_pct = 100;
	render("toast_playing", &s, HP_SMALL_H);
	s.mictest = HUD_MT_PAUSED; /* no mic test toast for a paused or held clip */
	render("toast_paused", &s, HP_SMALL_H);
	check(g_box[0].kind == HP_T_SHAPE && strcmp(g_box[1].what, "MIC ON") == 0,
	      "paused clip: the toast is the mute toast, not the mic test (%s)", g_box[1].what);
	s.visible = HUD_PANEL;
	s.headset = 0;
	render("no_headset", &s, HP_SMALL_H);
	/* the mute mark at each burn-in step; off without its bit */
	s = base_state();
	s.visible = HUD_BADGE;
	s.badge = HUD_BADGE_MUTED;
	for (int k = 0; k < 4; k++) {
		s.badge_shift = (u8)k;
		snprintf(name, sizeof(name), "badge_shift%d", k);
		render(name, &s, HP_BADGE_H);
		check(g_cv.w == HP_BADGE_W, "badge width %d", g_cv.w);
	}
	s.badge = 0;
	render("badge_off", &s, 0);
	check(g_cv.w == 0, "badge without HUD_BADGE_MUTED: empty");
	s.visible = HUD_HIDDEN;
	render("hidden", &s, 0);
	s.visible = 7;
	render("unknown", &s, 0);
}

/* ---- 1b. the description in the help line, and the CUSTOM rows' pictures ------------------- */

/* the worst cases: 63 characters of the widest glyph whose second word ends one past the line,
 * and one 63-character word */
static const char WORST_WORDS[] = "WWWWWWWWWWWWWWWWWW WWWWWWWWWWWWWWWWWW WWWWWWWWWWWWWWWWWWWWWWWWW";
static const char WORST_WORD[] = "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW";
_Static_assert(sizeof(WORST_WORDS) == 64 && sizeof(WORST_WORD) == 64, "63 characters");
#define W36 "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"
#define A33 "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
_Static_assert(sizeof(W36) == 37 && sizeof(A33) == 34, "W36, A33");

static const char *cut(const char *s, char *out)
{
	hp_help_cut(s, out);
	return out;
}

/* the cut is the longest prefix of at most HP_HELP_COLS characters that ends at a word (the
 * text's end, or a space after it), without trailing spaces; a first word longer than the
 * line is cut at HP_HELP_COLS. Checked as properties, not by a second copy of the code. */
static int cut_ok(const char *s, const char *out, int r)
{
	int n = 0, ends = 0;

	while (n < HUD_DESC_LEN - 1 && s[n])
		n++;
	if (r < 0 || r > HP_HELP_COLS || (int)strlen(out) != r || strncmp(s, out, (size_t)r) ||
	    (r && out[r - 1] == ' '))
		return 0;
	/* where a word ends inside the line: after s[j - 1], at j <= HP_HELP_COLS */
	for (int j = HP_HELP_COLS < n ? HP_HELP_COLS : n; j > 0 && !ends; j--)
		if (s[j - 1] != ' ' && (j == n || s[j] == ' '))
			ends = j;
	if (ends)
		return r == ends;
	/* no word ends inside the line: one long first word, cut at the line's end (spaces before
	 * it kept, none after: nothing at all for a line of spaces) */
	ends = HP_HELP_COLS < n ? HP_HELP_COLS : n;
	while (ends > 0 && s[ends - 1] == ' ')
		ends--;
	return r == ends;
}

static void test_help_cut(void)
{
	static const struct { const char *in, *want; } T[] = {
		{ "", "" },
		{ "   ", "" },
		{ "ALL-ROUNDER: PUNCH AND CLEAR STEPS", "ALL-ROUNDER: PUNCH AND CLEAR STEPS" },
		{ "TRAILING  ", "TRAILING" },
		/* 36 fit whole; at 37 the last word goes, whole */
		{ A33 " BB", A33 " BB" },
		{ A33 " BBB", A33 },
		/* a space right after the 36th character: all 36 kept */
		{ W36 " CC", W36 },
		/* two spaces before the word that does not fit: neither kept */
		{ "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA  BBBB", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
		/* a panel 5 description (two lines then): its first line */
		{ "FOOTSTEPS AND RELOADS STAND OUT. LESS BOOM.", "FOOTSTEPS AND RELOADS STAND OUT." },
		{ WORST_WORDS, "WWWWWWWWWWWWWWWWWW" },
		/* a first word longer than the line: cut at the line's end */
		{ WORST_WORD, W36 },
		{ " " W36 "W", " " "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW" },
	};
	char out[HP_HELP_COLS + 1], buf[HUD_DESC_LEN + 8];
	u32 seed = 12345;

	for (u32 i = 0; i < sizeof(T) / sizeof(T[0]); i++)
		check(!strcmp(cut(T[i].in, out), T[i].want), "cut '%s': '%s', want '%s'", T[i].in, out,
		      T[i].want);
	check(hp_help_cut("   ", out) == 0 && hp_help_cut("", out) == 0, "nothing to show: 0");
	/* only 63 characters are read (hud_state.desc's 64th byte may not be a NUL) */
	memset(buf, 'X', sizeof(buf));
	buf[sizeof(buf) - 1] = 0;
	for (int i = 40; i < (int)sizeof(buf) - 1; i += 2)
		buf[i] = ' ';
	check(!strcmp(cut(buf, out), "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"),
	      "64+ characters, spaces only past the line: the first 36 ('%s')", out);
	/* random texts, some with no NUL in the first 64 bytes */
	for (int t = 0; t < 200000; t++) {
		int len, r;

		seed = seed * 1103515245u + 12345u;
		len = (int)(seed >> 16) % (HUD_DESC_LEN + 4);
		for (int i = 0; i < len; i++) {
			seed = seed * 1103515245u + 12345u;
			/* spaces often (short words), and now and then a long run without */
			buf[i] = (seed >> 16) % (t & 1 ? 3 : 12) ? "ABCDEFGHIJKLMNOPQRSTUVWXYZ.:-"[(seed >> 8) % 29]
								 : ' ';
		}
		buf[len] = 0;
		memset(out, 0x55, sizeof(out));
		r = hp_help_cut(buf, out);
		if (!cut_ok(buf, out, r)) {
			check(0, "cut of random '%s': '%s' (%d)", buf, out, r);
			break;
		}
	}
}

/* a state with every row that can exist at once: 15 rows (team, SURROUND, CUSTOM, SURROUND
 * DEMO playing) */
static struct hud_state state_all(void)
{
	struct hud_state s = base_state();

	s.rows |= 1 << HUD_ROW_TEAM | 1 << HUD_ROW_CHANNEL;
	s.rows_hi = (u8)((1u << (HUD_ROW_MICTEST - 8)) | (1u << (HUD_ROW_PRESET - 8)) |
			 (1u << (HUD_ROW_SURROUND - 8)) | (HUD_ROWS_CUSTOM >> 8) |
			 (1u << (HUD_ROW_DEMO - 8)));
	s.surround = 1;
	s.surround_live = 1;
	s.demo = 1;
	s.demo_secs = 99;
	s.team_level = 6;
	s.channel = 3;
	s.team_peers = 3;
	s.team_talking = 2;
	s.preset_count = 29;
	s.preset_flags = HUD_PF_CUSTOM;
	s.preset = 30;
	name_set(&s, "CUSTOM");
	s.custom[0] = 3;
	s.custom[1] = 0;
	s.custom[2] = -2;
	return s;
}

static void desc_set(struct hud_state *s, const char *d)
{
	memset(s->desc, 0, HUD_DESC_LEN);
	memcpy(s->desc, d, strlen(d) < HUD_DESC_LEN ? strlen(d) : HUD_DESC_LEN);
}

/* the help line's text in the last picture rendered ("" if none was drawn) */
static const char *help_text(const struct hud_state *s)
{
	int n = 0;

	for (int i = 0; i < HUD_ROWS; i++)
		n += hud_rows(s) >> i & 1;
	for (int i = 0; i < g_nbox; i++)
		if (g_box[i].kind == HP_T_TEXT &&
		    g_box[i].y == HP_ROW0_Y + (n - 1) * HP_ROW_PITCH + HP_HELP_GAP)
			return g_box[i].what;
	return "";
}

/* SURROUND DEMO under SURROUND: ready (PLAY, Right lit, Left dim) and playing (the label in
 * the accent, PLAYING and the seconds, both arrows lit), its help line for each; the seconds
 * clamped; and hidden (the rows bit clear) it changes nothing: the same picture as a state
 * that never had it, whatever demo / demo_secs say. */
static void test_pictures_demo(void)
{
	struct hud_state s = base_state(), t;
	int lt = -1, val = -1, gt = -1, lab = -1;

	s.rows_hi |= 1 << (HUD_ROW_SURROUND - 8) | 1 << (HUD_ROW_DEMO - 8);
	s.cursor = HUD_ROW_DEMO;
	render("v7_demo_ready", &s, 134);
	check(!strcmp(help_text(&s), "> PLAY A 40 SECOND SURROUND SAMPLE"), "demo ready: help '%s'",
	      help_text(&s));
	for (int i = 0; i < g_nbox; i++) {
		if (g_box[i].kind != HP_T_TEXT || g_box[i].y != HP_ROW0_Y + 8 * HP_ROW_PITCH)
			continue;
		if (!strcmp(g_box[i].what, "SURROUND DEMO"))
			lab = i;
		else if (!strcmp(g_box[i].what, "PLAY"))
			val = i;
		else if (!strcmp(g_box[i].what, "<"))
			lt = i;
		else if (!strcmp(g_box[i].what, ">"))
			gt = i;
	}
	check(lab >= 0 && val >= 0 && lt >= 0 && gt >= 0 && g_box[lab].x + g_box[lab].w < g_box[lt].x &&
		      g_box[lt].x + g_box[lt].w < g_box[val].x && g_box[val].x + g_box[val].w < g_box[gt].x,
	      "demo row: the label, '<', PLAY, '>' left to right, apart (the 9th line, under SURROUND)");
	/* '<' (5 x 7: its point, row 3, is its first column) */
	check(g_cv.px[(HP_ROW0_Y + 8 * HP_ROW_PITCH + 3) * HP_W + HP_DEMO_X - 8] == HP_DIM,
	      "demo ready: '<' dim (Left does nothing)");
	s.demo = 1;
	s.demo_secs = 12;
	render("v7_demo_playing", &s, 134);
	check(!strcmp(help_text(&s), "GO UP TO FLIP SURROUND.  > STOPS IT"), "demo playing: help '%s'",
	      help_text(&s));
	val = -1;
	for (int i = 0; i < g_nbox; i++)
		if (g_box[i].kind == HP_T_TEXT && !strcmp(g_box[i].what, "PLAYING 12 SEC"))
			val = i;
	check(val >= 0 && g_box[val].x >= HP_DEMO_X && g_box[val].x + g_box[val].w <= HP_DEMO_X + HP_DEMO_W,
	      "demo playing: PLAYING 12 SEC inside its value area");
	s.demo_secs = 250;
	render("v7_demo_clamped", &s, 134);
	val = -1;
	for (int i = 0; i < g_nbox; i++)
		if (g_box[i].kind == HP_T_TEXT && !strcmp(g_box[i].what, "PLAYING 99 SEC"))
			val = i;
	check(val >= 0, "demo seconds clamped to 99");
	s.cursor = HUD_ROW_SURROUND;
	render("v7_demo_playing_on_surround", &s, 134);
	/* hidden: identical to a state without it */
	s = base_state();
	s.rows_hi |= 1 << (HUD_ROW_SURROUND - 8);
	s.cursor = HUD_ROW_SURROUND;
	t = s;
	render("v7_demo_hidden_ref", &t, 124);
	{
		static struct hp_canvas ref;

		ref = g_cv;
		s.demo = 1;
		s.demo_secs = 30;
		render("v7_demo_hidden", &s, 124);
		check(ref.w == g_cv.w && ref.h == g_cv.h && !memcmp(ref.px, g_cv.px, HP_W * ref.h),
		      "demo row hidden: the picture is the one without it");
	}
}

static void test_pictures_v6(void)
{
	struct hud_state s = state_all();
	char name[64], want[HP_HELP_COLS + 1];

	/* all 15 rows and a description longer than the line: every row selected; on PRESET the
	 * help line is the description cut at a word, elsewhere the row's own help */
	desc_set(&s, WORST_WORDS);
	for (int i = 0; i < HUD_ROWS; i++) {
		s.cursor = hud_row_order[i];
		snprintf(name, sizeof(name), "v7_rows15_sel%02d", s.cursor);
		render(name, &s, HP_H_MAX);
		if (s.cursor == HUD_ROW_PRESET)
			check(!strcmp(help_text(&s), "WWWWWWWWWWWWWWWWWW"), "15 rows, PRESET: the "
			      "description cut at a word ('%s')", help_text(&s));
		else
			check(*help_text(&s) && strcmp(help_text(&s), "WWWWWWWWWWWWWWWWWW"),
			      "15 rows, row %u: its own help ('%s')", s.cursor, help_text(&s));
	}
	check(HP_H_MAX == 184 && hp_panel_h(hud_rows(&s)) == 184, "all 15 rows: 184 (174 + SURROUND DEMO)");
	test_pictures_demo();
	/* the CUSTOM rows' help: what Left / Right change, in plain words */
	s.cursor = HUD_ROW_BASS;
	render("v6_help_bass", &s, HP_H_MAX);
	check(!strcmp(help_text(&s), "MORE OR LESS THUMP AND RUMBLE"), "BASS help '%s'", help_text(&s));
	s.cursor = HUD_ROW_MIDS;
	render("v6_help_mids", &s, HP_H_MAX);
	check(!strcmp(help_text(&s), "MORE OR LESS VOICE AND BODY"), "MIDS help '%s'", help_text(&s));
	s.cursor = HUD_ROW_TREBLE;
	render("v6_help_treble", &s, HP_H_MAX);
	check(!strcmp(help_text(&s), "MORE OR LESS SPARKLE AND CRUNCH"), "TREBLE help '%s'",
	      help_text(&s));
	/* the CUSTOM rows: the bar at each end and in between, the arrows dim at the ends */
	for (int v = -6; v <= 6; v += 3) {
		s.custom[0] = s.custom[1] = s.custom[2] = (s8)v;
		s.cursor = HUD_ROW_MIDS;
		snprintf(name, sizeof(name), "v6_custom_%+d", v);
		render(name, &s, HP_H_MAX);
	}
	s.custom[0] = 7; /* out of range: clamped */
	s.custom[2] = -100;
	s.cursor = HUD_ROW_BASS;
	render("v6_custom_clamped", &s, HP_H_MAX);
	/* one 63-character word: cut at the line's end, inside the panel (render checks) */
	s = state_all();
	s.cursor = HUD_ROW_PRESET;
	desc_set(&s, WORST_WORD);
	render("v6_desc_word63", &s, HP_H_MAX);
	check(!strcmp(help_text(&s), W36), "63-character word: its first 36 ('%s')", help_text(&s));
	/* typical: 9 rows (FLAT .. presets, SURROUND), a preset and its description */
	s = base_state();
	s.rows_hi |= 1 << (HUD_ROW_SURROUND - 8);
	s.preset_count = 18;
	s.preset = 1;
	name_set(&s, "OMNIVOX");
	s.cursor = HUD_ROW_PRESET;
	desc_set(&s, "ALL-ROUNDER: PUNCH AND CLEAR STEPS");
	render("v6_typical", &s, 124);
	check(!strcmp(help_text(&s), "ALL-ROUNDER: PUNCH AND CLEAR STEPS"), "typical: the description "
	      "is the help line ('%s')", help_text(&s));
	s.cursor = HUD_ROW_VOLUME;
	render("v6_typical_volume", &s, 124);
	check(!strcmp(help_text(&s), "EVERYTHING YOU HEAR"), "another row: its own help ('%s')",
	      help_text(&s));
	/* PRESET without a description: the row's help */
	s.cursor = HUD_ROW_PRESET;
	desc_set(&s, "");
	render("v6_preset_no_desc", &s, 124);
	check(!strcmp(help_text(&s), "EQUALIZER FOR EVERYTHING YOU HEAR"), "no description: '%s'",
	      help_text(&s));
	desc_set(&s, "    ");
	render("v6_preset_blank_desc", &s, 124);
	check(!strcmp(help_text(&s), "EQUALIZER FOR EVERYTHING YOU HEAR"), "blank description: '%s'",
	      help_text(&s));
	/* no presets, no CUSTOM: says so, whatever desc holds (hud.py show can set it) */
	s.preset_count = 0;
	s.preset = 0;
	desc_set(&s, "THE GAME AS IT WAS MIXED. NO CHANGE");
	render("v6_no_presets", &s, 124);
	check(!strcmp(help_text(&s), "NO PRESETS INSTALLED"), "no presets: '%s'", help_text(&s));
	/* CUSTOM offered and selected, 8 + 3 rows, its arrows: '>' dim on the last value */
	s = base_state();
	s.preset_count = 29;
	s.preset_flags = HUD_PF_CUSTOM;
	s.preset = 30;
	name_set(&s, "CUSTOM");
	s.rows_hi |= HUD_ROWS_CUSTOM >> 8;
	s.cursor = HUD_ROW_PRESET;
	s.custom[0] = 2;
	s.custom[2] = -4;
	desc_set(&s, "YOUR OWN SOUND: SET THE ROWS BELOW");
	render("v6_custom_selected", &s, 114 + 30);
	check(!strcmp(help_text(&s), "YOUR OWN SOUND: SET THE ROWS BELOW"), "CUSTOM: '%s'",
	      help_text(&s));
	/* CUSTOM offered with no presets: PRESET's help is not "NO PRESETS INSTALLED" */
	s.preset_count = 0;
	s.preset = 1;
	render("v6_custom_no_presets", &s, -1);
	check(!strcmp(help_text(&s), "YOUR OWN SOUND: SET THE ROWS BELOW"),
	      "CUSTOM offered, no presets: its description ('%s')", help_text(&s));
}

/* Every description that ships fits the help line whole: the presets in the committed
 * tools/presets/xw.presets (big-endian; eq_360.h struct xq_file_preset, desc at 160) and
 * FLAT's and CUSTOM's in xbox360/eq_360.c. Each is also drawn on the 9-row panel. */
static void read_rel(const char *rel, u8 *buf, size_t cap, size_t *len)
{
	const char *f = __FILE__, *slash = strrchr(f, '/');
	char path[512];
	FILE *fp;

	snprintf(path, sizeof(path), "%.*s%s", slash ? (int)(slash - f + 1) : 0, f, rel);
	fp = fopen(path, "rb");
	*len = fp ? fread(buf, 1, cap, fp) : 0;
	if (fp)
		fclose(fp);
	check(*len > 0 && *len < cap, "read %s", path);
}

static u32 be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static void shipped_one(const char *what, const char *d, int n)
{
	static const char FONT[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -%+:/.<>";
	struct hud_state s = base_state();
	char out[HP_HELP_COLS + 1], name[64];
	int ok = n >= 1 && n <= HP_HELP_COLS && d[0] != ' ' && d[n - 1] != ' ';

	for (int i = 0; i < n; i++)
		ok &= d[i] && strchr(FONT, d[i]) != NULL;
	check(ok, "%s: description '%.*s' (%d characters): 1..%d panel-font characters", what, n, d,
	      n, HP_HELP_COLS);
	s.rows_hi |= 1 << (HUD_ROW_SURROUND - 8);
	s.preset_count = 18;
	s.preset = 1;
	s.cursor = HUD_ROW_PRESET;
	memset(s.desc, 0, HUD_DESC_LEN);
	memcpy(s.desc, d, (size_t)(n < HUD_DESC_LEN ? n : HUD_DESC_LEN - 1));
	snprintf(name, sizeof(name), "v6_shipped_%.40s", what);
	for (char *c = name; *c; c++)
		if (*c == ' ' || *c == ':')
			*c = '_';
	render(name, &s, 124);
	check(hp_help_cut(s.desc, out) == n && !strcmp(help_text(&s), out), "%s: shown whole ('%s')",
	      what, help_text(&s));
}

static void test_shipped(void)
{
	static u8 file[65536];
	static char src[65536];
	size_t len, slen;
	u32 count;

	read_rel("../tools/presets/xw.presets", file, sizeof(file), &len);
	count = len >= 32 ? be32(file + 12) : 0;
	check(len >= 32 && be32(file) == 0x58575051 && be32(file + 4) == 2 && be32(file + 16) == 224 &&
	      count >= 1 && count <= 64 && 32 + count * 224 <= len,
	      "xw.presets: version 2, %u records of 224 bytes", count);
	for (u32 i = 0; i < count && 32 + (i + 1) * 224 <= len; i++) {
		const u8 *r = file + 32 + i * 224;
		char nm[21], d[HUD_DESC_LEN];

		memcpy(nm, r + 4, 20);
		nm[20] = 0;
		memcpy(d, r + 160, HUD_DESC_LEN);
		check(d[HUD_DESC_LEN - 1] == 0, "%s: description NUL-terminated", nm);
		d[HUD_DESC_LEN - 1] = 0;
		shipped_one(nm, d, (int)strlen(d));
	}
	/* FLAT and CUSTOM: the strings in eq_360.c (its static assert holds them to 36 too) */
	read_rel("../xbox360/eq_360.c", (u8 *)src, sizeof(src) - 1, &slen);
	src[slen] = 0;
	for (int k = 0; k < 2; k++) {
		const char *key = k ? "g_custom_desc[] = \"" : "g_flat_desc[] = \"";
		const char *p = strstr(src, key), *e;

		check(p != NULL, "eq_360.c has %s", key);
		if (!p)
			continue;
		p += strlen(key);
		e = strchr(p, '"');
		shipped_one(k ? "CUSTOM" : "FLAT", p, e ? (int)(e - p) : 0);
	}
}

/* ---- 2. placement (hud_360.c draw) -------------------------------------------------------- */

#define VA HUD_HOST_VA
static struct fb g_fb;
static u32 g_fetch[6];

/* a fake front buffer at VA: fetch constant, then a game-like picture (a bright sky above, a
 * bright patch and a dark floor below, so the mark is seen on both) */
static void make_fb(u32 w, u32 h, u32 tiled, u32 format)
{
	u32 pitch = (w + 31) & ~31u, size, endian = 2;

	g_fetch[0] = 2 | (pitch >> 5) << 22 | tiled << 31;
	g_fetch[1] = format | endian << 6 | VA;
	g_fetch[2] = (w - 1) | (h - 1) << 13;
	g_fetch[3] = (0 | 1 << 3 | 2 << 6) << 1; /* R G B from components 0 1 2 */
	g_fetch[4] = g_fetch[5] = 0;
	check(decode_fetch(g_fetch, &g_fb, 0) == SK_NONE, "decode_fetch %ux%u", w, h);
	size = g_fb.size;
	free(hud_host_mem);
	hud_host_mem = calloc(1, size);
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++) {
			u32 r, g, b;

			if (y < h / 2) {
				r = 120 + y * 100 / h;
				g = 160 + y * 80 / h;
				b = 230;
			} else if (x > w / 2 && x < w / 2 + w / 5) {
				r = g = b = 235; /* snow */
			} else {
				r = 40 + x * 40 / w;
				g = 60;
				b = 30;
			}
			ST((volatile u32 *)PTR(VA + offset_of(&g_fb, x, y)), texel_of(&g_fb, r << 16 | g << 8 | b));
		}
}

static u32 px_rgb(u32 x, u32 y)
{
	u32 t = to_texel(LD((volatile u32 *)PTR(VA + offset_of(&g_fb, x, y))), g_fb.endian);
	u32 sh = g_fb.bits - 8;

	return (lane(t, g_fb.sr, g_fb.mask) >> sh) << 16 | (lane(t, g_fb.sg, g_fb.mask) >> sh) << 8 |
	       lane(t, g_fb.sb, g_fb.mask) >> sh;
}

static void frame_ppm(const char *name, u32 y0, u32 y1)
{
	u32 w = g_fb.width, h = y1 - y0;
	u8 *rgb = malloc((size_t)w * h * 3);

	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++) {
			u32 v = px_rgb(x, y0 + y);

			rgb[(y * w + x) * 3] = (u8)(v >> 16);
			rgb[(y * w + x) * 3 + 1] = (u8)(v >> 8);
			rgb[(y * w + x) * 3 + 2] = (u8)v;
		}
	write_ppm(name, rgb, (int)w, (int)h);
	free(rgb);
}

/* draw state s into the fake buffer through hud_360.c; the drawn box (screen px) */
static int draw_state(const struct hud_state *s, struct geom *g)
{
	struct pending pd;
	int r;

	memset(&pd, 0, sizeof(pd));
	memcpy(pd.r.fetch, g_fetch, sizeof(g_fetch));
	g_canvas_valid = 0;
	g_mapped.pa = 0;
	r = draw(&pd, s, false);
	g->x0 = g_xh.d_x0;
	g->y0 = g_xh.d_y0;
	g->w = g_xh.d_w;
	g->h = g_xh.d_h;
	g->f = g_xh.d_scale;
	return r;
}

/* the screen box of pixels that differ from the picture under it, given a copy of it */
static void changed_box(const u8 *before, u32 *x0, u32 *y0, u32 *x1, u32 *y1)
{
	*x0 = *y0 = 0xffffffff;
	*x1 = *y1 = 0;
	for (u32 y = 0; y < g_fb.height; y++)
		for (u32 x = 0; x < g_fb.width; x++) {
			u32 o = offset_of(&g_fb, x, y);

			if (memcmp(before + o, hud_host_mem + o, 4)) {
				if (x < *x0)
					*x0 = x;
				if (y < *y0)
					*y0 = y;
				if (x > *x1)
					*x1 = x;
				if (y > *y1)
					*y1 = y;
			}
		}
}

static void test_placement(void)
{
	static const char *const POS[5] = { "bottom centre", "bottom left", "bottom right",
					    "top left", "top right" };
	struct { u32 w, h, tiled, fmt; const char *name; } modes[] = {
		{ 1280, 720, 0, 6, "720p linear 8888" },
		{ 1152, 640, 1, 7, "1152x640 tiled 2_10_10_10" },
		{ 1920, 1080, 1, 6, "1080p tiled 8888" },
		{ 640, 480, 0, 6, "480p linear 8888" },
	};
	struct hud_state panel = base_state(), mark;
	struct geom gp, gb;

	hud_init();
	check(g_mem != NULL, "hud_init allocated the canvases");
	g_xh.enable = 3;
	panel.rows |= 1 << HUD_ROW_TEAM | 1 << HUD_ROW_CHANNEL;
	panel.rows_hi = 7;
	memset(&mark, 0, sizeof(mark));
	mark.visible = HUD_BADGE;
	mark.badge = HUD_BADGE_MUTED;

	for (u32 m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		u32 W = modes[m].w, H = modes[m].h, sw = W - 2 * (W / 10), sh = H - 2 * (H / 10);
		u32 x0, y0, x1, y1;
		u8 *before;
		char name[64];

		make_fb(W, H, modes[m].tiled, modes[m].fmt);
		before = malloc(g_fb.size);
		memcpy(before, hud_host_mem, g_fb.size);
		/* the 11-row panel */
		check(draw_state(&panel, &gp) == DR_DREW, "%s: panel drawn (skip %u)", modes[m].name,
		      g_xh.last_skip);
		check(gp.x0 >= W / 10 && gp.x0 + gp.w <= W - W / 10 && gp.y0 >= H / 10 &&
		      gp.y0 + gp.h == H - H / 10 && gp.w <= sw && gp.h <= sh,
		      "%s: panel at (%u,%u) %ux%u is not bottom centre in the safe area", modes[m].name,
		      gp.x0, gp.y0, gp.w, gp.h);
		check(gp.h == 144 * gp.f, "%s: panel height %u at scale %u", modes[m].name, gp.h, gp.f);
		changed_box(before, &x0, &y0, &x1, &y1);
		check(x0 >= gp.x0 && x1 < gp.x0 + gp.w && y0 >= gp.y0 && y1 < gp.y0 + gp.h,
		      "%s: panel pixels (%u,%u)-(%u,%u) outside its box", modes[m].name, x0, y0, x1, y1);
		if (m < 2) {
			snprintf(name, sizeof(name), "frame_panel_%ux%u", W, H);
			frame_ppm(name, 0, H);
		}
		/* the mark at each position, over a fresh picture */
		for (u32 pos = 0; pos < 5; pos++) {
			u32 band_top = H - H / 10, band_bot = H - H * 4 / 100;

			memcpy(hud_host_mem, before, g_fb.size);
			g_xh.badge_pos = pos;
			check(draw_state(&mark, &gb) == DR_DREW, "%s: mark drawn at %s", modes[m].name,
			      POS[pos]);
			changed_box(before, &x0, &y0, &x1, &y1);
			check(x0 >= gb.x0 && x1 < gb.x0 + gb.w && y0 >= gb.y0 && y1 < gb.y0 + gb.h,
			      "%s %s: mark pixels outside its box", modes[m].name, POS[pos]);
			/* transparent: the corners of its box keep the picture */
			check(!memcmp(before + offset_of(&g_fb, gb.x0, gb.y0),
				      hud_host_mem + offset_of(&g_fb, gb.x0, gb.y0), 4) &&
			      !memcmp(before + offset_of(&g_fb, gb.x0 + gb.w - 1, gb.y0 + gb.h - 1),
				      hud_host_mem + offset_of(&g_fb, gb.x0 + gb.w - 1, gb.y0 + gb.h - 1), 4),
			      "%s %s: the mark has a box", modes[m].name, POS[pos]);
			check(gb.w == HP_BADGE_W * gb.f && gb.h == HP_BADGE_H * gb.f && gb.f == gp.f,
			      "%s %s: mark %ux%u at scale %u (panel's %u)", modes[m].name, POS[pos], gb.w,
			      gb.h, gb.f, gp.f);
			if (pos < 3) {
				check(gb.y0 >= band_top && gb.y0 + gb.h <= band_bot,
				      "%s %s: mark y %u..%u not in the band %u..%u", modes[m].name, POS[pos],
				      gb.y0, gb.y0 + gb.h, band_top, band_bot);
				check(gb.y0 >= gp.y0 + gp.h, "%s %s: mark overlaps the panel's place",
				      modes[m].name, POS[pos]);
			} else {
				check(gb.y0 >= H * 4 / 100 && gb.y0 + gb.h <= H / 10,
				      "%s %s: mark y %u..%u not in the top band", modes[m].name, POS[pos],
				      gb.y0, gb.y0 + gb.h);
			}
			if (pos == 0)
				check(gb.x0 == (W - gb.w) / 2, "%s: mark not centred", modes[m].name);
			else
				check(pos & 1 ? gb.x0 == W / 20 : gb.x0 + gb.w == W - W / 20,
				      "%s %s: mark x %u", modes[m].name, POS[pos], gb.x0);
			if (m == 0 && pos == 0)
				frame_ppm("frame_badge_720p_bottom", H * 3 / 4, H);
		}
		/* all five at once, for a look (they never overlap) */
		if (m == 0) {
			memcpy(hud_host_mem, before, g_fb.size);
			for (u32 pos = 0; pos < 5; pos++) {
				g_xh.badge_pos = pos;
				draw_state(&mark, &gb);
			}
			frame_ppm("frame_badge_720p_all", 0, H);
		}
		g_xh.badge_pos = 0;
		free(before);
	}
}

/* Everything at once fits: all 14 rows and the help line, with the cursor on PRESET and a
 * description longer than the line (both worst cases, cut) or on TREBLE, at 480p, 720p, 1080p
 * and Halo 3's 1152x640, in the title-safe area, bottom edge on the 10 % margin, at the scale
 * the 11-row panel had (1, 2, 3, 2), not one pixel outside the panel. */
static void test_fit(void)
{
	struct { u32 w, h, tiled, fmt, f; const char *name; } modes[] = {
		{ 640, 480, 0, 6, 1, "480p" },
		{ 1280, 720, 0, 6, 2, "720p" },
		{ 1920, 1080, 1, 6, 3, "1080p" },
		{ 1152, 640, 1, 7, 2, "1152x640" },
	};
	const char *const worst[3] = { WORST_WORDS, WORST_WORD, WORST_WORDS };
	struct hud_state s;
	struct geom g;

	g_xh.enable = 3;
	g_xh.badge_pos = 0;
	for (u32 m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		u32 W = modes[m].w, H = modes[m].h, x0, y0, x1, y1;
		u8 *before;
		char name[64];

		make_fb(W, H, modes[m].tiled, modes[m].fmt);
		before = malloc(g_fb.size);
		memcpy(before, hud_host_mem, g_fb.size);
		for (int k = 0; k < 3; k++) {
			memcpy(hud_host_mem, before, g_fb.size);
			s = state_all();
			s.cursor = k < 2 ? HUD_ROW_PRESET : HUD_ROW_TREBLE;
			desc_set(&s, worst[k]);
			check(draw_state(&s, &g) == DR_DREW, "%s: everything drawn (skip %u)", modes[m].name,
			      g_xh.last_skip);
			check(g.f == modes[m].f && g.h == HP_H_MAX * g.f && g.w == HP_W * g.f,
			      "%s: %ux%u at scale %u, want scale %u", modes[m].name, g.w, g.h, g.f, modes[m].f);
			check(g.x0 >= W / 10 && g.x0 + g.w <= W - W / 10 && g.y0 >= H / 10 &&
			      g.y0 + g.h == H - H / 10, "%s: at (%u,%u) %ux%u: not in the safe area",
			      modes[m].name, g.x0, g.y0, g.w, g.h);
			changed_box(before, &x0, &y0, &x1, &y1);
			check(x0 >= g.x0 && x1 < g.x0 + g.w && y0 >= g.y0 && y1 < g.y0 + g.h,
			      "%s: pixels (%u,%u)-(%u,%u) outside the panel", modes[m].name, x0, y0, x1, y1);
			if (k == 1 && (m == 0 || m == 3)) {
				snprintf(name, sizeof(name), "frame6_all_%ux%u", W, H);
				frame_ppm(name, 0, H);
			}
		}
		/* typical at 720p: 9 rows, a preset and its description in the help line */
		if (m == 1) {
			memcpy(hud_host_mem, before, g_fb.size);
			s = base_state();
			s.rows_hi |= 1 << (HUD_ROW_SURROUND - 8);
			s.preset_count = 18;
			s.preset = 2;
			name_set(&s, "HALO MCC");
			s.cursor = HUD_ROW_PRESET;
			desc_set(&s, "SHARP: STEPS UP FRONT. LESS BASS");
			check(draw_state(&s, &g) == DR_DREW && g.f == 2 && g.h == 124 * 2,
			      "720p typical drawn at scale 2, 124 units tall");
			frame_ppm("frame6_typical_1280x720", 0, H);
		}
		free(before);
	}
	/* the margin left at each: units of height still free at that scale */
	check(HP_H_MAX <= (480 - 2 * 48) / 1 && HP_H_MAX <= (640 - 2 * 64) / 2 &&
	      HP_H_MAX <= (720 - 2 * 72) / 2 && HP_H_MAX <= (1080 - 2 * 108) / 3,
	      "HP_H_MAX %d within 384 / 256 / 288 / 288", HP_H_MAX);
	printf("canvases: struct hud_mem %zu bytes (HP_H_MAX %d)\n", sizeof(struct hud_mem), HP_H_MAX);
}

/* ---- 3. controls (panel_360.c against these stubs) ------------------------------------------ */

struct xd_status g_xd = { .mic_gain = 100, .gate = XD_GATE_2 };
static const u32 GAINS[10] = { 25, 35, 50, 70, 100, 140, 200, 280, 400, 560 };
static int g_balance, g_muted, g_mic_auto, g_surround, g_team_level, g_channel;
static u32 g_master = 50, g_side = 5, g_preset, g_preset_count, g_mt_enabled = 1;
/* the fake mic test: state, and the last call made */
static u32 g_mt, g_mt_ms, g_mt_pos, g_mt_len;
static char g_mt_call[32];

static int gain_idx(u32 pct)
{
	int best = 0;

	for (int i = 1; i < 10; i++)
		if ((pct > GAINS[i] ? pct - GAINS[i] : GAINS[i] - pct) <
		    (pct > GAINS[best] ? pct - GAINS[best] : GAINS[best] - pct))
			best = i;
	return best;
}

u32 xd_mic_gain_step(u32 pct, int steps)
{
	int i = gain_idx(pct) + steps;

	return GAINS[i < 0 ? 0 : i > 9 ? 9 : i];
}
u32 xd_mic_level(u32 pct) { return (u32)gain_idx(pct) + 1; }
void xd_poll(void) {}
bool xd_mic_auto(void) { return g_mic_auto; }
void xd_set_mic_auto(bool on) { g_mic_auto = on; }
u32 xd_mic_auto_level(void) { return 7; }
u32 xd_mic_vu(int pad) { return 4; }
bool xd_gate_open(int pad) { return true; }
bool xd_mictest_enabled(void) { return g_mt_enabled; }
static void mt_call(const char *c) { snprintf(g_mt_call, sizeof(g_mt_call), "%s", c); }
u32 xd_mictest_start(int pad)
{
	mt_call("start");
	if (g_mt != XD_MT_IDLE && g_mt != XD_MT_READY)
		return XD_MT_BUSY;
	g_mt = XD_MT_RECORDING;
	g_mt_ms = 0;
	return XD_MT_OK;
}
void xd_mictest_stop(void)
{
	mt_call("stop");
	if (g_mt == XD_MT_RECORDING) {
		g_mt = XD_MT_PLAYING;
		g_mt_len = g_mt_ms;
		g_mt_pos = 0;
	}
}
u32 xd_mictest_play(void)
{
	mt_call("play");
	if (g_mt != XD_MT_READY)
		return XD_MT_NO_CLIP;
	g_mt = XD_MT_PLAYING;
	g_mt_pos = 0;
	return XD_MT_OK;
}
void xd_mictest_pause(void)
{
	mt_call("pause");
	if (g_mt == XD_MT_PLAYING)
		g_mt = XD_MT_PAUSED;
}
void xd_mictest_resume(void)
{
	mt_call("resume");
	if (g_mt == XD_MT_PAUSED)
		g_mt = XD_MT_PLAYING;
}
void xd_mictest_cancel(const char *why)
{
	mt_call("cancel");
	g_mt = XD_MT_IDLE;
}
static int g_closed;
void xd_mictest_panel_closed(void)
{
	g_closed++;
	if (g_mt == XD_MT_PAUSED || g_mt == XD_MT_READY)
		g_mt = XD_MT_IDLE;
}
u32 xd_mictest_state(u32 *secs, u32 *pct)
{
	*secs = g_mt == XD_MT_RECORDING ? g_mt_ms / 1000 : g_mt_len / 1000;
	*pct = g_mt_len ? g_mt_pos * 100 / g_mt_len : 0;
	return g_mt;
}
u32 xd_mictest_max_secs(void) { return 30; }
/* the fake clock moves the mic test along */
static void mt_tick(u32 ms)
{
	if (g_mt == XD_MT_RECORDING && (g_mt_ms += ms) >= 30000) {
		g_mt = XD_MT_PLAYING;
		g_mt_len = 30000;
		g_mt_pos = 0;
	} else if (g_mt == XD_MT_PLAYING && (g_mt_pos += ms) >= g_mt_len) {
		g_mt_pos = g_mt_len;
		g_mt = XD_MT_READY;
	}
}

bool xv_toggle_mic_mute(int pad) { return g_muted = !g_muted; }
bool xv_mic_muted(int pad) { return g_muted; }
void xv_tick(void) {}
bool xg_routed(void) { return true; }
int xg_balance(void) { return g_balance; }
u32 xg_master(void) { return g_master; }
u32 xg_sidetone(void) { return g_side; }
void xg_adjust(int db, int dm, int ds)
{
	g_balance += db;
	g_balance = g_balance < -5 ? -5 : g_balance > 5 ? 5 : g_balance;
	g_master = (u32)((int)g_master + dm * 10) > 100 ? (dm < 0 ? 0 : 100) : g_master + dm * 10;
	g_side = (u32)((int)g_side + ds) > 10 ? (ds < 0 ? 0 : 10) : g_side + ds;
}
u32 xg_surround(void) { return g_surround; }
void xg_set_surround(u32 on) { g_surround = on != 0; }
bool xg_surround_live(void) { return g_surround; }
bool xg_surround_offered(void) { return false; } /* rows are forced with XWPN features_force */
/* the demo (game_360.h xg_demo_*): offered when g_demo_offered (and SURROUND, forced) */
static int g_demo_offered, g_demo_on, g_demo_starts, g_demo_stops;
static u32 g_demo_secs;
bool xg_demo_offered(void) { return g_demo_offered; }
bool xg_demo_playing(u32 *secs)
{
	*secs = g_demo_secs;
	return g_demo_on;
}
void xg_demo_start(void)
{
	g_demo_starts++;
	g_demo_on = 1;
}
void xg_demo_stop(void)
{
	g_demo_stops++;
	g_demo_on = 0;
}
bool xd_mic_auto_offered(void) { return false; }
bool xd_gate_auto_offered(void) { return false; }
/* the equalizer (eq_360.h): g_custom_on offers CUSTOM after the presets; g_descs[i] is preset
 * i's description (NULL: ""). With both off it is today's stand-in. */
static int g_custom_on, g_custom[3], g_custom_sets;
static const char *g_descs[64];
u32 xq_count(void) { return g_preset_count; }
u32 xq_preset(void) { return g_preset; }
void xq_set_preset(u32 i) { g_preset = i > g_preset_count + (g_custom_on ? 1 : 0) ? 0 : i; }
const char *xq_name(u32 i)
{
	static char b[24];

	if (g_custom_on && i == g_preset_count + 1)
		return "CUSTOM";
	if (!i || i > g_preset_count)
		return "FLAT";
	snprintf(b, sizeof(b), "PRESET NUMBER %u", i);
	return b;
}
const char *xq_desc(u32 i) { return i < 64 && g_descs[i] ? g_descs[i] : ""; }
bool xq_custom_offered(void) { return g_custom_on; }
u32 xq_custom_index(void) { return g_custom_on ? g_preset_count + 1 : 0; }
int xq_custom(u32 b) { return b < 3 ? g_custom[b] : 0; }
void xq_set_custom(u32 b, int step)
{
	g_custom_sets++;
	if (b < 3)
		g_custom[b] = step < -6 ? -6 : step > 6 ? 6 : step;
}
u32 xt_level(void) { return (u32)g_team_level; }
void xt_set_level(u32 l) { g_team_level = (int)l; }
u32 xt_channel(void) { return (u32)g_channel; }
void xt_set_channel(u32 c) { g_channel = (int)c; }
u32 xt_peers(void) { return 0; }
u32 xt_talking_mask(void) { return 0; }
void xa_xinput_update(int slot, const struct gip_gamepad_state *s) {}
void xa_xinput_notify(int msg) {}
int xw_headset_pad(void) { return 0; }
bool xw_headset_live(void) { return true; }

/* XWPN, as xbdm sees it */
extern struct {
	u32 magic, version, inject, inject_pad, notify, tap_ms, hold_ms, visible, cursor,
	    share_down, taps, holds, adjusts[9], masked_presses, tone_auto_offs, title_buttons[4],
	    panel_pad, publishes, empty_holds, inject_timeouts, bumpers, adjusts_hi[2], rows,
	    features, features_force, badge, badge_mode, badge_shows, repeats, mictest_closes,
	    adjusts_custom[3], unused45;
} g_pn;
_Static_assert(sizeof(g_pn) == 46 * 4, "XWPN v5");

enum { SHARE = 1, UP = 2, DOWN = 4, LEFT = 8, RIGHT = 16 };

/* the driver loop: a pass every 2 ms */
static void run(u32 ms)
{
	for (u32 t = 0; t < ms; t += 2) {
		g_now += 2;
		mt_tick(2);
		panel_poll();
	}
}

static void inject(u32 bits)
{
	g_pn.inject = bits;
	run(2);
}

static void press(u32 bits)
{
	inject(SHARE | bits);
	run(60);
	inject(SHARE);
	run(60);
}

static void open_panel(void)
{
	inject(SHARE);
	run(450); /* past tap_ms too: a release before it is a mute tap even with the panel up */
	check(g_pn.visible == HUD_PANEL, "Share held: panel (visible %u)", g_pn.visible);
}

static void close_panel(void)
{
	inject(0);
	run(20);
}

static struct hud_state published(void)
{
	return g_pub.s;
}

static void walk(const u8 *want, int n, const char *what)
{
	open_panel();
	for (int i = 0; i < n; i++) {
		check(g_pn.cursor == want[i], "%s: step %d cursor %u, want %u", what, i, g_pn.cursor,
		      want[i]);
		press(DOWN);
	}
	check(g_pn.cursor == want[n - 1], "%s: Down on the last row stays (%u)", what, g_pn.cursor);
	for (int i = n - 1; i > 0; i--)
		press(UP);
	check(g_pn.cursor == want[0], "%s: Up back to the first row (%u)", what, g_pn.cursor);
	press(UP);
	check(g_pn.cursor == want[0], "%s: Up on the first row stays", what);
	close_panel();
}

static void cursor_to(u32 row)
{
	for (int i = 0; i < HUD_ROWS && g_pn.cursor != row; i++)
		press(DOWN);
	check(g_pn.cursor == row, "cursor to row %u (at %u)", row, g_pn.cursor);
}

static void test_controls(void)
{
	static const u8 ORDER8[] = { HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE, HUD_ROW_MIC,
				     HUD_ROW_GATE, HUD_ROW_MICTEST, HUD_ROW_PRESET, HUD_ROW_TONE };
	static const u8 ORDER11[] = { HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE,
				      HUD_ROW_MIC, HUD_ROW_GATE, HUD_ROW_MICTEST, HUD_ROW_PRESET,
				      HUD_ROW_SURROUND, HUD_ROW_TEAM, HUD_ROW_CHANNEL, HUD_ROW_TONE };
	static const u8 ORDER10[] = { HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE,
				      HUD_ROW_MIC, HUD_ROW_GATE, HUD_ROW_PRESET, HUD_ROW_SURROUND,
				      HUD_ROW_TEAM, HUD_ROW_CHANNEL, HUD_ROW_TONE };
	struct hud_state h;
	u32 a0, r0, shows;

	panel_init();
	g_pn.inject_pad = 0;
	run(100);
	check(g_pn.version == 5 && g_pn.badge == 1 && g_pn.badge_mode == 0, "XWPN v5 defaults");

	/* cursor order: the default rows, then everything offered */
	walk(ORDER8, 8, "8 rows");
	check(g_pn.rows == (0x3F | 3 << 8), "rows %03x", g_pn.rows);
	g_pn.features_force = 0x1F;
	run(4);
	walk(ORDER11, 11, "11 rows");
	g_mt_enabled = 0;
	run(4);
	walk(ORDER10, 10, "MIC TEST off: 10 rows");
	g_mt_enabled = 1;

	/* NOISE GATE: AUTO, OFF, 1, 2, 3, 4 (stored 3, 0, 1, 2, 4, 5) */
	open_panel();
	cursor_to(HUD_ROW_GATE);
	g_xd.gate = XD_GATE_2;
	press(LEFT);
	check(g_xd.gate == XD_GATE_1, "gate: Left from 2 -> 1 (%u)", g_xd.gate);
	press(LEFT);
	check(g_xd.gate == XD_GATE_OFF, "gate: Left from 1 -> OFF (%u)", g_xd.gate);
	press(LEFT);
	check(g_xd.gate == XD_GATE_AUTO, "gate: Left from OFF -> AUTO (%u)", g_xd.gate);
	press(LEFT);
	check(g_xd.gate == XD_GATE_AUTO, "gate: Left on AUTO stays (%u)", g_xd.gate);
	press(RIGHT);
	check(g_xd.gate == XD_GATE_OFF, "gate: Right from AUTO -> OFF (%u)", g_xd.gate);
	press(RIGHT);
	check(g_xd.gate == XD_GATE_1, "gate: Right from OFF -> 1 (%u)", g_xd.gate);
	press(RIGHT);
	press(RIGHT);
	check(g_xd.gate == XD_GATE_3, "gate: Right x2 -> 3, stored 4 (%u)", g_xd.gate);
	press(RIGHT);
	check(g_xd.gate == XD_GATE_4, "gate: Right -> 4, stored 5 (%u)", g_xd.gate);
	press(RIGHT);
	check(g_xd.gate == XD_GATE_4, "gate: Right on 4 stays (%u)", g_xd.gate);
	g_pn.features_force = 0x1F & ~0x04; /* no gate AUTO */
	g_xd.gate = XD_GATE_OFF;
	press(LEFT);
	check(g_xd.gate == XD_GATE_OFF, "gate without AUTO: Left on OFF stays (%u)", g_xd.gate);
	g_xd.gate = XD_GATE_AUTO; /* set over xbdm */
	press(LEFT);
	check(g_xd.gate == XD_GATE_AUTO, "gate AUTO not offered: Left stays (%u)", g_xd.gate);
	press(RIGHT);
	check(g_xd.gate == XD_GATE_OFF, "gate AUTO not offered: Right -> OFF (%u)", g_xd.gate);
	g_xd.gate = XD_GATE_2;

	/* MIC VOLUME: AUTO left of 1, Right from AUTO back to the manual level */
	g_pn.features_force = 0x1F;
	press(UP);
	check(g_pn.cursor == HUD_ROW_MIC, "cursor on MIC VOLUME");
	g_xd.mic_gain = 35;
	press(LEFT);
	check(g_xd.mic_gain == 25 && !g_mic_auto, "mic: Left 2 -> 1 (%u)", g_xd.mic_gain);
	press(LEFT);
	check(g_mic_auto && g_xd.mic_gain == 25, "mic: Left from 1 -> AUTO, gain kept (%u)",
	      g_xd.mic_gain);
	run(60);
	h = published();
	check(h.mic_level == HUD_MIC_AUTO && h.mic_auto_level == 7, "published AUTO (%u, %u)",
	      h.mic_level, h.mic_auto_level);
	press(LEFT);
	check(g_mic_auto, "mic: Left on AUTO stays");
	press(RIGHT);
	check(!g_mic_auto && g_xd.mic_gain == 25, "mic: Right from AUTO -> 1 (%u)", g_xd.mic_gain);
	g_xd.mic_gain = 140;
	g_mic_auto = 1; /* AUTO switched on over xbdm while at 6 */
	press(RIGHT);
	check(!g_mic_auto && g_xd.mic_gain == 140, "mic: Right from AUTO -> the last manual 6");
	g_pn.features_force = 0x1F & ~0x02;
	g_xd.mic_gain = 25;
	press(LEFT);
	check(!g_mic_auto && g_xd.mic_gain == 25, "mic without AUTO: Left on 1 stays");
	g_xd.mic_gain = 100;
	g_pn.features_force = 0x1F;
	close_panel();

	/* PRESET: hold-to-repeat, clamped */
	g_preset_count = 29;
	g_preset = 0;
	open_panel();
	cursor_to(HUD_ROW_PRESET);
	r0 = g_pn.repeats;
	inject(SHARE | RIGHT);
	run(1000); /* 1 press, then repeats at 400, 525, 650, 775, 900 ms */
	check(g_preset == 6 && g_pn.repeats - r0 == 5, "preset: Right held 1 s -> 6 (%u, %u repeats)",
	      g_preset, g_pn.repeats - r0);
	run(4000);
	check(g_preset == 29, "preset: held on -> clamped at 29 (%u)", g_preset);
	h = published();
	check(h.preset == 29 && h.preset_count == 29 && !strncmp(h.preset_name, "PRESET NUMBER 29", 20),
	      "published preset %u/%u '%.20s'", h.preset, h.preset_count, h.preset_name);
	inject(SHARE);
	r0 = g_pn.repeats;
	run(1000);
	check(g_pn.repeats == r0, "preset: released, no more repeats");
	inject(SHARE | LEFT);
	run(398);
	check(g_preset == 28, "preset: Left press -> 28 at once, no repeat before 400 ms (%u)",
	      g_preset);
	inject(SHARE);
	press(DOWN); /* off PRESET: SURROUND */
	inject(SHARE | LEFT);
	r0 = g_pn.repeats;
	run(1000);
	check(g_pn.repeats == r0, "no repeat on other rows");
	inject(SHARE);
	press(RIGHT);
	check(g_surround == 1, "surround: Right -> ON");
	press(RIGHT);
	check(g_surround == 1, "surround: Right on ON stays");
	press(LEFT);
	check(g_surround == 0, "surround: Left -> OFF");
	a0 = g_pn.adjusts_hi[0];
	close_panel();
	check(a0 >= 3, "adjusts_hi[0] counts PRESET presses (%u)", a0);

	/* MIC TEST: Right record -> stop and play -> pause -> play on; finished -> play again;
	 * Left discards */
	g_pn.features_force = 0;
	open_panel();
	cursor_to(HUD_ROW_MICTEST);
	press(LEFT);
	check(g_mt == XD_MT_IDLE && strcmp(g_mt_call, "cancel"), "mic test idle: Left does nothing");
	press(RIGHT);
	check(g_mt == XD_MT_RECORDING && !strcmp(g_mt_call, "start"), "Right: record (%s)", g_mt_call);
	run(5000);
	h = published();
	check(h.mictest == HUD_MT_RECORDING && h.mictest_secs == 5, "published REC %u s",
	      h.mictest_secs);
	press(RIGHT);
	check(g_mt == XD_MT_PLAYING && !strcmp(g_mt_call, "stop"), "Right: stop and play (%s)",
	      g_mt_call);
	run(1000);
	press(RIGHT);
	check(g_mt == XD_MT_PAUSED && !strcmp(g_mt_call, "pause"), "Right: pause (%s)", g_mt_call);
	run(60);
	h = published();
	check(h.mictest == HUD_MT_PAUSED && h.mictest_pct > 0, "published PAUSED %u%%",
	      h.mictest_pct);
	press(RIGHT);
	check(g_mt == XD_MT_PLAYING && !strcmp(g_mt_call, "resume"), "Right: play on (%s)", g_mt_call);
	run(6000);
	check(g_mt == XD_MT_READY, "played to the end: clip held");
	press(RIGHT);
	check(g_mt == XD_MT_PLAYING && !strcmp(g_mt_call, "play"), "Right: play again (%s)",
	      g_mt_call);
	press(LEFT);
	check(g_mt == XD_MT_IDLE && !strcmp(g_mt_call, "cancel"), "Left while playing: discard");
	press(RIGHT);
	press(LEFT);
	check(g_mt == XD_MT_IDLE, "Left while recording: discard");
	/* let go while playing: it carries on, the toast shows it, the clip is kept */
	press(RIGHT);
	run(2000);
	press(RIGHT);
	g_closed = 0;
	close_panel();
	check(g_mt == XD_MT_PLAYING && g_pn.visible == HUD_TOAST && !g_closed,
	      "let go while playing: plays on, toast (visible %u, closed %d)", g_pn.visible, g_closed);
	run(3000);
	check(g_mt == XD_MT_READY && g_pn.visible == HUD_HIDDEN, "finished hidden: kept, no toast");
	/* the next press starts on BALANCE (nothing running); closing deletes the held clip */
	open_panel();
	check(g_pn.cursor == HUD_ROW_BALANCE, "next press: cursor on BALANCE");
	close_panel();
	check(g_mt == XD_MT_IDLE && g_closed == 1, "closed with a held clip: discarded (%d)",
	      g_closed);
	/* paused + let go = gone */
	open_panel();
	cursor_to(HUD_ROW_MICTEST);
	press(RIGHT);
	run(2000);
	press(RIGHT);
	press(RIGHT);
	check(g_mt == XD_MT_PAUSED, "paused");
	close_panel();
	check(g_mt == XD_MT_IDLE && g_closed == 2 && g_pn.mictest_closes >= 2,
	      "closed while paused: discarded");
	/* a press while recording starts on MIC TEST */
	open_panel();
	cursor_to(HUD_ROW_MICTEST);
	press(RIGHT);
	close_panel();
	check(g_pn.visible == HUD_TOAST, "let go while recording: toast");
	open_panel();
	check(g_pn.cursor == HUD_ROW_MICTEST, "press while recording: cursor on MIC TEST");
	press(LEFT);
	close_panel();

	/* the mute mark */
	shows = g_pn.badge_shows;
	inject(SHARE);
	inject(0);
	check(g_muted && g_pn.visible == HUD_TOAST, "tap: muted, toast (muted %d, visible %u, taps %u)",
	      g_muted, g_pn.visible, g_pn.taps);
	run(1600);
	check(g_pn.visible == HUD_BADGE && g_pn.badge_shows == shows + 1, "after the toast: mark");
	h = published();
	check(h.visible == HUD_BADGE && h.badge == HUD_BADGE_MUTED && h.mic_vu == 0 && h.rows == 0,
	      "published mark: minimal state");
	open_panel();
	close_panel();
	check(g_pn.visible == HUD_BADGE, "panel shown and let go: mark back");
	g_pn.badge = 0;
	run(10);
	check(g_pn.visible == HUD_HIDDEN, "badge 0: no mark");
	g_pn.badge = 1;
	run(10);
	/* the burn-in shift: one step a minute */
	{
		u8 s0 = published().badge_shift, s1;

		run(60000);
		s1 = published().badge_shift;
		check(s1 == ((s0 + 1) & 3), "mark moved after 60 s (%u -> %u)", s0, s1);
	}
	/* badge_mode 1: 2 s out of every 15 */
	g_pn.badge_mode = 1;
	{
		u32 on = 0;

		run(10);
		for (u32 t = 0; t < 30000; t += 10) {
			run(10);
			on += g_pn.visible == HUD_BADGE;
		}
		check(on >= 390 && on <= 410, "badge_mode 1: up %u of 3000 samples (want ~400)", on);
	}
	g_pn.badge_mode = 0;
	inject(SHARE);
	inject(0);
	run(1600);
	check(!g_muted && g_pn.visible == HUD_HIDDEN, "unmuted: no mark");
}

static int all_zero(const void *p, size_t n)
{
	const u8 *b = p;

	for (size_t i = 0; i < n; i++)
		if (b[i])
			return 0;
	return 1;
}

/* the published state's description is preset p's */
static int desc_is(const struct hud_state *h, u32 p)
{
	return !strncmp(h->desc, xq_desc(p), HUD_DESC_LEN - 1);
}

static void test_custom_controls(void)
{
	static const u8 ORDER14[] = { HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE, HUD_ROW_MIC,
				      HUD_ROW_GATE, HUD_ROW_MICTEST, HUD_ROW_PRESET, HUD_ROW_BASS,
				      HUD_ROW_MIDS, HUD_ROW_TREBLE, HUD_ROW_SURROUND, HUD_ROW_TEAM,
				      HUD_ROW_CHANNEL, HUD_ROW_TONE };
	struct hud_state h;
	u32 r0, a0;
	int same = 1;

	g_pn.features_force = 0;
	g_custom_on = 0;
	memset(g_descs, 0, sizeof(g_descs));
	memset(g_custom, 0, sizeof(g_custom));
	check(g_pn.version == 5 && g_pn.unused45 == 0, "XWPN version 5 (%u), its last word 0",
	      g_pn.version);

	/* 1. nothing offered (today's default): PRESET stops at the last file preset, no CUSTOM
	 * rows, no description (FLAT's is not published without a file), and the published
	 * state's version 5 bytes are zero (so the pictures are the GOLDEN ones) */
	g_descs[0] = "THE GAME AS IT WAS MIXED. NO CHANGE";
	for (u32 count = 0; count <= 29; count += 29) {
		g_preset_count = count;
		g_preset = 0;
		open_panel();
		cursor_to(HUD_ROW_PRESET);
		run(60);
		h = published();
		check(count || all_zero(h.desc, HUD_DESC_LEN), "no file: FLAT's description not "
		      "published ('%.63s')", h.desc);
		inject(SHARE | RIGHT);
		run(5000);
		inject(SHARE);
		check(g_preset == count, "not offered, %u presets: Right held stops at %u (%u)", count,
		      count, g_preset);
		run(60);
		h = published();
		check(h.preset_flags == 0 && h.reserved55 == 0 && all_zero(h.custom, 3) &&
		      all_zero(h.desc, HUD_DESC_LEN) && !(hud_rows(&h) & HUD_ROWS_CUSTOM) &&
		      g_pn.unused45 == 0 && g_pn.rows == (0x3F | 3 << 8),
		      "not offered, %u presets: published as version 4 (flags %u, rows %03x)", count,
		      h.preset_flags, hud_rows(&h));
		press(DOWN);
		check(g_pn.cursor == HUD_ROW_TONE, "not offered: Down from PRESET to TEST TONE (%u)",
		      g_pn.cursor);
		close_panel();
	}

	/* 2. descriptions: published only while the cursor is on PRESET (the help line shows
	 * it); the panel's height never changes with them */
	g_preset_count = 29;
	g_preset = 0;
	g_descs[1] = "ALL-ROUNDER: PUNCH AND CLEAR STEPS";
	g_descs[3] = "VOICES FIRST";
	open_panel();
	h = published();
	check(all_zero(h.desc, HUD_DESC_LEN), "cursor on BALANCE: no description published");
	cursor_to(HUD_ROW_PRESET);
	h = published();
	check(desc_is(&h, 0), "on PRESET: FLAT's description ('%.63s')", h.desc);
	for (u32 p = 1; p <= 29; p++) {
		press(RIGHT);
		h = published();
		same &= g_preset == p && desc_is(&h, p) && hp_panel_h(hud_rows(&h)) == 114 &&
			h.reserved55 == 0;
	}
	check(same, "stepping through 29 presets: each one's description, the panel 114 tall");
	press(LEFT);
	press(LEFT);
	press(LEFT);
	press(LEFT);
	press(LEFT);
	press(LEFT);
	h = published();
	check(g_preset == 23, "Left x6: preset 23 (%u)", g_preset);
	press(DOWN); /* off PRESET: the row's own help, nothing published */
	h = published();
	check(g_pn.cursor == HUD_ROW_TONE && all_zero(h.desc, HUD_DESC_LEN),
	      "cursor on TEST TONE: no description published");
	g_preset = 1; /* the preset changed from elsewhere (saved settings) while off PRESET */
	run(100);
	check(all_zero(published().desc, HUD_DESC_LEN), "a preset change off PRESET: still none");
	press(UP);
	check(desc_is(&g_pub.s, 1), "back on PRESET: preset 1's ('%.63s')", g_pub.s.desc);
	close_panel();
	/* a description longer than the line (an older file): published whole, cut when drawn */
	g_descs[7] = WORST_WORDS;
	g_preset = 7;
	open_panel();
	cursor_to(HUD_ROW_PRESET);
	h = published();
	check(desc_is(&h, 7) && h.desc[HUD_DESC_LEN - 1] == 0 && hp_panel_h(hud_rows(&h)) == 114,
	      "a 63-character description: published, the panel 114 tall");
	close_panel();
	g_descs[7] = NULL;

	/* 3. CUSTOM: right after FLAT in the walk (its index stays after the file's presets); its
	 * rows under PRESET while it is selected */
	g_custom_on = 1;
	g_descs[30] = "YOUR OWN SOUND: SET THE ROWS BELOW";
	g_preset = 29;
	open_panel();
	cursor_to(HUD_ROW_PRESET);
	press(RIGHT);
	check(g_preset == 29, "Right on the last preset stays (%u)", g_preset);
	g_preset = 0;
	press(RIGHT);
	h = published();
	check(g_preset == 30 && h.preset == 30 && h.preset_flags == HUD_PF_CUSTOM &&
	      !strcmp(h.preset_name, "CUSTOM") && (hud_rows(&h) & HUD_ROWS_CUSTOM) == HUD_ROWS_CUSTOM &&
	      desc_is(&h, 30), "Right from FLAT: CUSTOM, its rows shown (preset %u, rows %03x)",
	      h.preset, hud_rows(&h));
	press(RIGHT);
	check(g_preset == 1, "Right on CUSTOM: the first preset (%u)", g_preset);
	press(LEFT);
	check(g_preset == 30, "Left from the first preset: CUSTOM (%u)", g_preset);
	press(LEFT);
	check(g_preset == 0, "Left on CUSTOM: FLAT (%u)", g_preset);
	press(LEFT);
	check(g_preset == 0, "Left on FLAT stays");
	press(RIGHT);
	press(DOWN);
	h = published();
	check(g_pn.cursor == HUD_ROW_BASS && all_zero(h.desc, HUD_DESC_LEN),
	      "on BASS: its own help, no description published");
	close_panel();
	g_pn.features_force = 0x1F;
	run(4);
	walk(ORDER14, 14, "14 rows (CUSTOM selected)");
	/* a step at a time, counted */
	open_panel();
	cursor_to(HUD_ROW_BASS);
	a0 = g_pn.adjusts_custom[0];
	press(RIGHT);
	press(RIGHT);
	press(LEFT);
	check(g_custom[0] == 1 && g_pn.adjusts_custom[0] == a0 + 3, "BASS: Right x2, Left: +1 (%d, "
	      "%u counted)", g_custom[0], g_pn.adjusts_custom[0] - a0);
	/* held: repeats like PRESET (a press, then at 400, 525, 650, 775, 900 ms), clamped */
	g_custom[0] = -6;
	r0 = g_pn.repeats;
	inject(SHARE | RIGHT);
	run(1000);
	check(g_custom[0] == 0 && g_pn.repeats - r0 == 5, "BASS: Right held 1 s from -6 -> 0 (%d, %u "
	      "repeats)", g_custom[0], g_pn.repeats - r0);
	run(2000);
	check(g_custom[0] == 6, "BASS: held on -> clamped at +6 (%d)", g_custom[0]);
	inject(SHARE);
	run(60);
	h = published();
	check(h.custom[0] == 6 && h.custom[1] == 0 && h.custom[2] == 0, "published custom %d %d %d",
	      h.custom[0], h.custom[1], h.custom[2]);
	press(DOWN);
	press(DOWN);
	inject(SHARE | LEFT);
	run(3000);
	inject(SHARE);
	r0 = g_pn.repeats;
	run(1000);
	check(g_pn.cursor == HUD_ROW_TREBLE && g_custom[2] == -6 && g_pn.repeats == r0,
	      "TREBLE: Left held -> -6, released: no more repeats (%d)", g_custom[2]);
	/* the rows go when another preset is picked on PRESET */
	press(UP);
	press(UP);
	press(UP);
	check(g_pn.cursor == HUD_ROW_PRESET, "Up x3 from TREBLE: PRESET (%u)", g_pn.cursor);
	press(LEFT);
	h = published();
	check(g_preset == 0 && !(hud_rows(&h) & HUD_ROWS_CUSTOM), "Left from CUSTOM: FLAT, rows "
	      "hidden (%03x)", hud_rows(&h));
	press(DOWN);
	check(g_pn.cursor == HUD_ROW_SURROUND, "Down from PRESET skips the hidden rows (%u)",
	      g_pn.cursor);
	/* the cursor on one when they go (preset changed by xbdm or saved settings): to PRESET */
	press(UP);
	press(RIGHT);
	press(DOWN);
	press(DOWN);
	check(g_pn.cursor == HUD_ROW_MIDS, "back on CUSTOM, cursor on MIDS (%u)", g_pn.cursor);
	inject(SHARE | RIGHT); /* a repeat running on MIDS stops with it */
	g_preset = 3;
	run(10);
	r0 = g_pn.repeats;
	run(1000);
	h = published();
	check(g_pn.cursor == HUD_ROW_PRESET && h.cursor == HUD_ROW_PRESET &&
	      !(hud_rows(&h) & HUD_ROWS_CUSTOM) && g_pn.repeats == r0 && g_preset == 3,
	      "rows gone under the cursor: it moves to PRESET, the repeat stops (cursor %u, preset %u)",
	      g_pn.cursor, g_preset);
	check(desc_is(&h, 3), "moved to PRESET: preset 3's description ('%.63s')", h.desc);
	inject(SHARE);
	/* ... and when CUSTOM stops being offered */
	press(RIGHT);
	press(RIGHT);
	g_preset = 30;
	press(DOWN);
	check(g_pn.cursor == HUD_ROW_BASS, "on BASS (%u)", g_pn.cursor);
	g_custom_on = 0;
	run(10);
	check(g_pn.cursor == HUD_ROW_PRESET && !(g_pn.rows & HUD_ROWS_CUSTOM),
	      "CUSTOM no longer offered: cursor to PRESET (%u)", g_pn.cursor);
	close_panel();

	/* 4. the features_force bits for the console tests (XW_DEV); 0x80 (the test description
	 * of panel 5) means nothing now */
	g_preset = 0;
	memset(g_descs, 0, sizeof(g_descs));
	g_pn.features_force = 0x20;
	run(4);
	h = published();
	check(h.preset_flags == HUD_PF_CUSTOM && !(g_pn.rows & HUD_ROWS_CUSTOM),
	      "force 0x20: CUSTOM offered, not selected");
	g_pn.features_force = 0x40;
	run(4);
	check((g_pn.rows & HUD_ROWS_CUSTOM) == HUD_ROWS_CUSTOM, "force 0x40: CUSTOM rows shown");
	g_pn.features_force = 0x80;
	open_panel();
	cursor_to(HUD_ROW_PRESET);
	h = published();
	check(!(g_pn.features & 0x80) && all_zero(h.desc, HUD_DESC_LEN) && g_pn.unused45 == 0 &&
	      h.reserved55 == 0, "force 0x80: ignored (features %02x, '%.63s')", g_pn.features, h.desc);
	close_panel();
	g_pn.features_force = 0;
}

static u32 adjusts_sum(void)
{
	u32 n = 0;

	for (int i = 0; i < 9; i++)
		n += g_pn.adjusts[i];
	return n + g_pn.adjusts_hi[0] + g_pn.adjusts_hi[1] + g_pn.adjusts_custom[0] +
	       g_pn.adjusts_custom[1] + g_pn.adjusts_custom[2];
}

/* SURROUND DEMO: shown only with SURROUND and the file; directly under SURROUND; Right plays,
 * Right or Left stops; Up to SURROUND while it plays leaves it playing (SURROUND flips);
 * closing the panel leaves it playing, and the next Share press starts on it; the test tone
 * goes off when it starts; the row going away moves the cursor to SURROUND; no XWPN word
 * moves (its layout is kept). */
static void test_demo_controls(void)
{
	static const u8 ORDER[] = { HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE, HUD_ROW_MIC,
				    HUD_ROW_GATE, HUD_ROW_MICTEST, HUD_ROW_PRESET, HUD_ROW_SURROUND,
				    HUD_ROW_DEMO, HUD_ROW_TONE };
	struct hud_state h;
	u32 adj;

	g_custom_on = 0;
	g_preset = 0;
	g_surround = 0;
	g_mt = 0;
	g_xd.tone = 0;
	g_pn.features_force = 0;
	g_demo_offered = 1;
	run(10);
	check(!(g_pn.rows >> HUD_ROW_DEMO & 1) && g_pn.features & 0x100,
	      "demo: the file offered, hidden without SURROUND (rows %x)", g_pn.rows);
	g_demo_offered = 0;
	g_pn.features_force = 0x01;
	run(10);
	check(!(g_pn.rows >> HUD_ROW_DEMO & 1), "demo: hidden without the file (rows %x)", g_pn.rows);
	g_demo_offered = 1;
	run(10);
	check(g_pn.rows >> HUD_ROW_DEMO & 1 && g_pn.features & 0x100,
	      "demo: shown with SURROUND and the file (rows %x, features %x)", g_pn.rows, g_pn.features);
	walk(ORDER, sizeof(ORDER), "demo order");
	adj = adjusts_sum();
	open_panel();
	cursor_to(HUD_ROW_DEMO);
	g_xd.tone = 1;
	press(LEFT);
	check(!g_demo_on && !g_demo_starts && !g_demo_stops, "demo: Left while ready does nothing");
	press(RIGHT);
	check(g_demo_on && g_demo_starts == 1 && g_xd.tone == 0,
	      "demo: Right plays (the test tone off first: tone %u)", g_xd.tone);
	g_demo_secs = 7;
	run(100);
	h = published();
	check(h.demo == 1 && h.demo_secs == 7 && h.cursor == HUD_ROW_DEMO &&
		      hud_rows(&h) >> HUD_ROW_DEMO & 1,
	      "demo: published playing, 7 s (demo %u secs %u)", h.demo, h.demo_secs);
	press(UP);
	check(g_pn.cursor == HUD_ROW_SURROUND && g_demo_on && !g_demo_stops,
	      "demo: Up to SURROUND, still playing");
	press(RIGHT);
	check(g_surround == 1 && g_demo_on, "demo: SURROUND flipped ON while it plays");
	press(LEFT);
	check(g_surround == 0 && g_demo_on, "demo: SURROUND flipped OFF while it plays");
	press(DOWN);
	press(RIGHT);
	check(!g_demo_on && g_demo_stops == 1, "demo: Right while playing stops it");
	press(RIGHT);
	press(LEFT);
	check(!g_demo_on && g_demo_starts == 2 && g_demo_stops == 2, "demo: Left while playing stops it");
	press(RIGHT);
	close_panel();
	run(100);
	check(g_demo_on && g_pn.visible != HUD_PANEL, "demo: the panel closed, it keeps playing");
	open_panel();
	check(g_pn.cursor == HUD_ROW_DEMO, "demo: Share while it plays starts on its row (%u)",
	      g_pn.cursor);
	g_demo_offered = 0;
	run(10);
	check(g_pn.cursor == HUD_ROW_SURROUND, "demo: the row went: cursor to SURROUND (%u)",
	      g_pn.cursor);
	close_panel();
	g_demo_on = 0;
	g_demo_secs = 0;
	check(adjusts_sum() == adj + 2, "demo: no XWPN counter moved for its row (only SURROUND's "
	      "Right and Left: +%u)", adjusts_sum() - adj);
	g_pn.features_force = 0;
	run(10);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		g_out = argv[1];
	mkdir("out", 0755);
	mkdir(g_out, 0755);
	test_pictures();
	test_help_cut();
	test_pictures_v6();
	test_shipped();
	test_placement();
	test_fit();
	test_controls();
	test_custom_controls();
	test_demo_controls();
	printf("%s: %d checks, %d failed; pictures in %s\n", g_fail ? "FAIL" : "ok", g_checks, g_fail,
	       g_out);
	return g_fail != 0;
}
