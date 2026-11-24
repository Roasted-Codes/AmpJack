// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The headset equalizer (eq_360.c): presets from Hdd:\Plugins\xw.presets applied to
 * everything the earpiece plays.
 *
 * Where it runs: xq_process is called from xv_earpiece_fill (driver thread) after the whole
 * mix (chat, game, team, tone, sidetone, tick, mic-test playback) and before the "anything to
 * play" scan and USB, so one preset shapes all of it (ROADMAP 2.4). Everything else here is
 * driver thread too. The file is loaded through file_360.h (xf_post), never on the driver
 * thread. No file, or a bad one: FLAT only, and the plugin works as without the equalizer.
 *
 * Index 0 is always FLAT (no processing); presets are 1..xq_count() in file order. A preset
 * also has a stable id (FNV-1a 32 of its name; 0 = FLAT) that settings_360.c saves, so a
 * reordered or extended file still restores the same preset; an id the file no longer has
 * loads as FLAT. CUSTOM has no id (xq_preset_id gives 0): settings_360.c saves "CUSTOM is
 * selected" and its three steps instead.
 */
#ifndef EQ_360_H
#define EQ_360_H

#include "../core/compat.h"

#define XQ_FILE "xw.presets" /* in XF_DIR (file_360.h) */

/* driver thread, once, after xf_init: posts the preset load */
void xq_init(void);
/* driver thread, every pass: takes in a finished load, status */
void xq_poll(void);
/* Hot reload: xq_process passes audio through untouched from return on; xq_resume undoes it */
void xq_quiesce(void);
void xq_resume(void);

/* in place on an earpiece block of pad `pad` (s16le, rate_out x ch_out, len bytes): the
 * selected preset, clamped. FLAT is the file's flat_q15 gain only (none without a file).
 * Does nothing while the XWEQ gate is off, on FLAT at 0 dB, or when rate_out is not 48000
 * (the presets are designed for 48 kHz). Adds no delay. */
void xq_process(int pad, u8 *out, int len, u32 rate_out, u32 ch_out);

u32 xq_count(void);            /* presets loaded; 0 = no file (FLAT only) */
u32 xq_preset(void);           /* the selected index: 0 FLAT, 1..xq_count() */
void xq_set_preset(u32 idx);   /* crossfaded over 10 ms; above xq_count(): FLAT */
u32 xq_preset_id(u32 idx);     /* the stable id; 0 for FLAT or an index out of range */
u32 xq_find(u32 id);           /* the index with this id; 0 (FLAT) if none */
/* the name shown on the panel: never NULL, at most 20 characters (HUD_NAME_LEN), only the
 * panel font's characters (A-Z 0-9 space - % + : / . < >); "FLAT" for 0 or out of range */
const char *xq_name(u32 idx);

/* The line of plain words the panel shows at its bottom for a preset ("WHAT IT IS GOOD FOR"):
 * never NULL, "" when there is none, at most XQ_DESC_LEN - 1 characters, panel font only. */
#define XQ_DESC_LEN 64
const char *xq_desc(u32 idx);

/* CUSTOM: one more entry after the file's presets that the player shapes himself with three
 * plain controls, each XQ_CUSTOM_MIN..XQ_CUSTOM_MAX steps (0 = untouched; all 0 sounds as
 * FLAT). Its curve is built from tables in the preset file (no arithmetic the kernel rules
 * forbid). Offered only when the loaded file carries those tables. */
#define XQ_CUSTOM_BASS 0
#define XQ_CUSTOM_MIDS 1
#define XQ_CUSTOM_TREBLE 2
#define XQ_CUSTOM_BANDS 3
#define XQ_CUSTOM_MIN (-6)
#define XQ_CUSTOM_MAX 6
bool xq_custom_offered(void);
u32 xq_custom_index(void);              /* xq_count() + 1 when offered (xq_set_preset takes it,
                                         * xq_name says "CUSTOM"), else 0 */
int xq_custom(u32 band);                /* the step of XQ_CUSTOM_BASS / MIDS / TREBLE */
void xq_set_custom(u32 band, int step); /* clamped; crossfaded like a preset change */

/* ---- the preset file, big-endian (the console reads it natively; a host reads it with
 * byte swaps). tools/presets writes it; the generator proves every preset's peak gain after
 * the preamp is 0 dB or less, and the same for every CUSTOM combination.
 * Layout: the header, `count` records, then (optional) the CUSTOM tables. A version-1 file
 * (records without a description, no CUSTOM) is refused: FLAT only, logged once, XWEQ
 * load_error XQ_E_OLD; eq.py install puts the new file in place. -------------------------- */
#define XQ_MAGIC 0x58575051 /* "XWPQ" */
#define XQ_VERSION 2
#define XQ_RATE 48000
#define XQ_BANDS 5
#define XQ_MAX_PRESETS 64
struct xq_file_header {
	u32 magic, version;
	u32 rate;       /* XQ_RATE */
	u32 count;      /* records that follow, at most XQ_MAX_PRESETS */
	u32 rec_size;   /* sizeof(struct xq_file_preset): a reader refuses any other */
	u32 bands;      /* XQ_BANDS */
	u32 crc32;      /* of everything after this header, the CUSTOM tables included (IEEE
			 * 802.3, as zlib.crc32) */
	/* FLAT's gain while this file is loaded, Q15 (0 = 32768 = 0 dB): the presets are
	 * loudness-matched to it (pink noise, K-weighted) so that switching never jumps in
	 * loudness; tools/presets/convert.py LOUDNESS. A reader that ignores it plays FLAT at
	 * 0 dB, louder than the presets by up to the file's headroom. */
	u32 flat_q15;
};
/* xq_file_preset.flags */
#define XQ_F_BANDS 0x1F      /* bit i: band i is not unity (skipped exactly when clear) */
#define XQ_F_OWN_LEVEL 0x100 /* not loudness-matched (a band-pass): never louder than FLAT,
			      * possibly quieter; information only, the reader ignores it */
/* One preset: XQ_BANDS biquads in series after the preamp; direct form 1, s16 in, 64-bit
 * accumulate, per channel. */
struct xq_file_preset {
	u32 id;                     /* FNV-1a 32 of name (as stored, NUL padding excluded) */
	char name[20];              /* panel name, NUL padded (no NUL when 20 long) */
	char author[16];            /* credit, NUL padded */
	s32 preamp_q15;             /* <= 32768 (0 dB) */
	u32 flags;                  /* XQ_F_* */
	s32 coef[XQ_BANDS][5];      /* b0 b1 b2 a1 a2, Q3.28 */
	s16 gain_cdb[XQ_BANDS];     /* each band's gain in 0.01 dB (for display and checks) */
	u8 pad[2];
	char desc[XQ_DESC_LEN];     /* xq_desc: at most 63 characters, NUL padded */
};
/* CUSTOM (xq_custom_*): after the records, exactly this long, or absent (no CUSTOM). A table
 * that is there but wrong (magic, size, a preamp outside 1..32768, step 0 not unity, 0 0 0
 * not at flat_q15) refuses CUSTOM only; the presets still load. The cascade of a combination
 * is coef[BASS][b+6], coef[MIDS][m+6], coef[TREBLE][t+6] (a band at step 0 is skipped) after
 * preamp_q15[b+6][m+6][t+6]; 0 0 0 plays as FLAT itself. tools/presets/custom.py: the
 * shapes, and the proofs over all 2197 combinations. */
#define XQ_CUSTOM_MAGIC 0x58575043 /* "XWPC" */
#define XQ_CUSTOM_STEPS (XQ_CUSTOM_MAX - XQ_CUSTOM_MIN + 1)
struct xq_file_custom {
	u32 magic;
	u32 size;                   /* sizeof(struct xq_file_custom) */
	u32 controls;               /* XQ_CUSTOM_BANDS */
	u32 steps;                  /* XQ_CUSTOM_STEPS */
	u32 cdb_step[XQ_CUSTOM_BANDS]; /* dB per step x 100 (information) */
	s32 coef[XQ_CUSTOM_BANDS][XQ_CUSTOM_STEPS][5]; /* Q3.28 */
	u16 preamp_q15[XQ_CUSTOM_STEPS][XQ_CUSTOM_STEPS][XQ_CUSTOM_STEPS]; /* 1..32768 */
	u8 pad[2];
};
_Static_assert(sizeof(struct xq_file_header) == 32, "xw.presets header");
_Static_assert(sizeof(struct xq_file_preset) == 224, "xw.presets record");
_Static_assert(sizeof(struct xq_file_custom) == 5204, "xw.presets CUSTOM tables");

#endif
