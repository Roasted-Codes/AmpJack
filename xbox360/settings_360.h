// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Per-profile settings (settings_360.c): the panel's settings saved to Hdd:\Plugins\xw.settings
 * for whoever is signed in on the headset's pad, and restored at boot, when a headset is
 * bound, and when that pad's profile changes.
 *
 * No feature calls into this module: xs_poll reads every setting through its owner's getter
 * (game_360.h, diag_360.h, eq_360.h, team_360.h) every 250 ms, saves 2 s after the last change,
 * and restores through the setters. File work goes through file_360.h's worker, never on the
 * driver thread. A missing or corrupt file means the compiled defaults; it never blocks boot.
 * A profile with no record gets the compiled defaults, not the previous player's values.
 */
#ifndef SETTINGS_360_H
#define SETTINGS_360_H

#include "../core/compat.h"

#define XS_FILE "xw.settings" /* in XF_DIR (file_360.h) */

/* driver thread, once, after xf_init and after the modules whose settings it restores */
void xs_init(void);
/* driver thread, every pass: profile changes, change detection, the delayed save */
void xs_poll(void);
/* Hot reload, driver thread: a pending save is written now (bounded wait), then nothing more
 * is saved. xs_resume: a refused reload; saving resumes. */
void xs_quiesce(void);
void xs_resume(void);

/* ---- the settings file, big-endian (the console reads it natively). Loaded once into RAM
 * and rewritten whole on a save (under 3.1 KB). Not saved: mute, test tone, mic test, and
 * the engineering knobs (buffer_ms, dev_*). ------------------------------------------------ */
#define XS_MAGIC 0x58575354 /* "XWST" */
#define XS_VERSION 1
#define XS_CAPACITY 64      /* records; when full, the lowest seq is replaced */
#define XS_XUID_NOBODY 0ull /* nobody signed in on the headset's pad */
#define XS_XUID_EMPTY 0xFFFFFFFFFFFFFFFFull /* an unused slot */
/* xs_record.flags */
#define XS_F_MIC_AUTO 0x01  /* MIC VOLUME AUTO on; mic_level is then the manual step to return to */
#define XS_F_CUSTOM 0x02    /* the EQ's CUSTOM is selected (preset_id is then 0) */
struct xs_file_header {
	u32 magic;
	u16 version;
	u16 rec_size;           /* sizeof(struct xs_record): a reader refuses any other */
	u16 count;              /* records in use */
	u16 capacity;           /* XS_CAPACITY */
	u32 reserved;
};
struct xs_record {
	u64 xuid;               /* XS_XUID_NOBODY, XS_XUID_EMPTY, or the profile's XUID */
	u32 seq;                /* bumped on every save of this record */
	u8 ver;                 /* 1 */
	s8 balance;             /* -5..+5 */
	u8 volume;              /* 0..10 */
	u8 sidetone;            /* 0..10 */
	u8 mic_level;           /* 1..10, the manual step (AUTO is XS_F_MIC_AUTO) */
	u8 gate;                /* XD_GATE_* */
	u8 surround;            /* 0, 1 */
	u8 flags;               /* XS_F_* */
	u32 preset_id;          /* xq_preset_id; 0 FLAT; an unknown id loads as FLAT */
	u8 team_level, team_channel, mic_route;
	/* CUSTOM's BASS, MIDS, TREBLE steps (eq_360.h, -6..6). These were reserved bytes, zero
	 * in every older file: an older record reads as CUSTOM at 0 0 0 and not selected, so the
	 * record version stays 1 and nothing is refused. An older build ignores them and the
	 * flag (it plays the saved preset_id 0, FLAT) and, saving, writes them back as zero. */
	s8 custom[3];
	u8 reserved[14];        /* zero */
	u32 crc32;              /* of the 44 bytes before it (IEEE 802.3, as zlib.crc32) */
};
_Static_assert(sizeof(struct xs_file_header) == 16, "xw.settings header");
_Static_assert(sizeof(struct xs_record) == 48 &&
	       __builtin_offsetof(struct xs_record, crc32) == 44, "xw.settings record");

#endif
