/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The xw.settings image in RAM (settings_360.h has the layout): building, checking and
 * searching it. Pure C on a caller's buffer, no globals and no kernel includes, so the plugin
 * (settings_360.c) and the host test (tests/test_settings.c) run the same code. Every field
 * is read and written byte by byte as big-endian, so the host (little-endian) and the console
 * agree on the bytes.
 *
 * Threads: none here; the caller owns the buffer (settings_360.c: the driver thread, or the
 * file worker while a load or save job has it).
 */
#ifndef SETTINGS_TAB_H
#define SETTINGS_TAB_H

#include "../core/compat.h"
#include "diag_360.h"
#include "eq_360.h" /* XQ_CUSTOM_* */
#include "eq_dsp.h" /* eqd_crc32, eqd_be32: the same CRC as xw.presets (zlib's) */
#include "game_360.h"
#include "settings_360.h"

#define XS_HDR 16
#define XS_REC 48
#define XS_CRC_LEN 44
#define XS_FILE_BYTES (XS_HDR + XS_CAPACITY * XS_REC) /* 3088: always written whole */
enum { XS_E_SHORT = -21, XS_E_HEADER = -22 }; /* table_check; XWST last_error */

/* one profile's settings, native order (u32 only: compared with memcmp) */
struct xs_vals {
	s32 balance;
	u32 volume, sidetone, mic_level, gate, surround, flags, preset_id;
	u32 team_level, team_channel;
	s32 custom[XQ_CUSTOM_BANDS]; /* CUSTOM's steps; XS_F_CUSTOM in flags: it is selected */
};

static void put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static u64 be64(const u8 *p)
{
	return (u64)eqd_be32(p) << 32 | eqd_be32(p + 4);
}

static u8 *rec_at(u8 *f, u32 i)
{
	return f + XS_HDR + i * XS_REC;
}

static u32 clampu(u32 v, u32 lo, u32 hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void rec_put(u8 *r, u64 xuid, u32 seq, const struct xs_vals *v)
{
	memset(r, 0, XS_REC);
	put_be32(r, (u32)(xuid >> 32));
	put_be32(r + 4, (u32)xuid);
	put_be32(r + 8, seq);
	r[12] = 1;
	r[13] = (u8)(s8)v->balance;
	r[14] = (u8)v->volume;
	r[15] = (u8)v->sidetone;
	r[16] = (u8)v->mic_level;
	r[17] = (u8)v->gate;
	r[18] = (u8)v->surround;
	r[19] = (u8)v->flags;
	put_be32(r + 20, v->preset_id);
	r[24] = (u8)v->team_level;
	r[25] = (u8)v->team_channel;
	/* r[26] mic_route: not restored (team voice is unpublished); zero, as reserved[] */
	for (int k = 0; k < XQ_CUSTOM_BANDS; k++)
		r[27 + k] = (u8)(s8)v->custom[k];
	put_be32(r + XS_CRC_LEN, eqd_crc32(r, XS_CRC_LEN));
}

static void rec_empty(u8 *r)
{
	memset(r, 0, XS_REC);
	memset(r, 0xFF, 8);
	put_be32(r + XS_CRC_LEN, eqd_crc32(r, XS_CRC_LEN));
}

static bool rec_used(const u8 *r)
{
	return be64(r) != XS_XUID_EMPTY;
}

/* a used record's values, clamped to what the setters take (a hand-edited file stays sane) */
static void rec_get(const u8 *r, struct xs_vals *v)
{
	s32 b = (s8)r[13];

	memset(v, 0, sizeof(*v));
	v->balance = b < -XG_BALANCE_MAX ? -XG_BALANCE_MAX : b > XG_BALANCE_MAX ? XG_BALANCE_MAX : b;
	v->volume = clampu(r[14], 0, 100 / XG_MASTER_STEP);
	v->sidetone = clampu(r[15], 0, XG_SIDETONE_MAX);
	v->mic_level = clampu(r[16], 1, XD_MIC_LEVELS);
	v->gate = r[17] < XD_GATES ? r[17] : XD_GATE_DEFAULT;
	v->surround = r[18] ? 1 : 0;
	v->flags = r[19] & (XS_F_MIC_AUTO | XS_F_CUSTOM);
	v->preset_id = eqd_be32(r + 20);
	v->team_level = clampu(r[24], 0, 10);
	v->team_channel = clampu(r[25], 0, 8);
	for (int k = 0; k < XQ_CUSTOM_BANDS; k++) {
		s32 c = (s8)r[27 + k];
		v->custom[k] = c < XQ_CUSTOM_MIN ? XQ_CUSTOM_MIN : c > XQ_CUSTOM_MAX ? XQ_CUSTOM_MAX : c;
	}
}

static void hdr_put(u8 *f)
{
	u32 n = 0;

	for (u32 i = 0; i < XS_CAPACITY; i++)
		n += rec_used(rec_at(f, i));
	put_be32(f, XS_MAGIC);
	put_be32(f + 4, (u32)XS_VERSION << 16 | XS_REC);
	put_be32(f + 8, n << 16 | XS_CAPACITY);
	put_be32(f + 12, 0);
}

static void table_init(u8 *f)
{
	for (u32 i = 0; i < XS_CAPACITY; i++)
		rec_empty(rec_at(f, i));
	hdr_put(f);
}

/* Checks a file of len bytes read into f (XS_FILE_BYTES long) and makes it a clean table:
 * records past the end, with a bad CRC or version, and the older of two with one XUID become
 * empty slots (*rejects counts the bad ones). 0, or XS_E_SHORT / XS_E_HEADER (f is then not
 * a table: the caller starts an empty one). */
static int table_check(u8 *f, u32 len, u32 *rejects)
{
	*rejects = 0;
	if (len < XS_HDR)
		return XS_E_SHORT;
	if (eqd_be32(f) != XS_MAGIC || eqd_be32(f + 4) != ((u32)XS_VERSION << 16 | XS_REC) ||
	    (eqd_be32(f + 8) & 0xFFFF) != XS_CAPACITY)
		return XS_E_HEADER;
	for (u32 i = 0; i < XS_CAPACITY; i++) {
		u8 *r = rec_at(f, i);

		if (XS_HDR + (i + 1) * XS_REC > len) {
			rec_empty(r); /* cut short: a torn or truncated write */
			continue;
		}
		if (!rec_used(r))
			continue;
		if (eqd_crc32(r, XS_CRC_LEN) != eqd_be32(r + XS_CRC_LEN) || r[12] != 1) {
			rec_empty(r);
			(*rejects)++;
			continue;
		}
		for (u32 j = 0; j < i; j++) {
			u8 *o = rec_at(f, j);

			if (rec_used(o) && be64(o) == be64(r)) {
				rec_empty(eqd_be32(o + 8) < eqd_be32(r + 8) ? o : r);
				break;
			}
		}
	}
	hdr_put(f);
	return 0;
}

static int table_find(u8 *f, u64 xuid)
{
	for (u32 i = 0; i < XS_CAPACITY; i++)
		if (rec_used(rec_at(f, i)) && be64(rec_at(f, i)) == xuid)
			return (int)i;
	return -1;
}

static u32 table_max_seq(u8 *f)
{
	u32 m = 0;

	for (u32 i = 0; i < XS_CAPACITY; i++)
		if (rec_used(rec_at(f, i)) && eqd_be32(rec_at(f, i) + 8) > m)
			m = eqd_be32(rec_at(f, i) + 8);
	return m;
}

/* where xuid's record goes: its own, else an empty slot, else the lowest seq (the profile
 * saved longest ago is forgotten) */
static u32 table_slot_for(u8 *f, u64 xuid)
{
	int i = table_find(f, xuid);
	u32 best = 0, best_seq = 0xFFFFFFFFu;

	if (i >= 0)
		return (u32)i;
	for (u32 k = 0; k < XS_CAPACITY; k++) {
		u8 *r = rec_at(f, k);

		if (!rec_used(r))
			return k;
		if (eqd_be32(r + 8) < best_seq) {
			best_seq = eqd_be32(r + 8);
			best = k;
		}
	}
	return best;
}

#endif
