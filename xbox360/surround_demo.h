/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The surround demo clip (Hdd:\Plugins\xw.demo, made by tools/demo/make_demo.py, whose top
 * explains the layout and why): the file format and the tap's maths. surround_360.c streams
 * the file and calls these; tools/demo/make_demo.py (and tools/demo/demo_vectors.py for the
 * host test) model them bit for bit.
 *
 * Pure: no globals, no floating point, no kernel includes. Include core/platform.h first.
 *
 * The clip is two mono stems and, per 256-frame block, each stem's gain into each of the six
 * channels (Q15, 0..32767, reached at the block's last frame; the tap ramps from the previous
 * block's gains). Channel c, frame i of a block:
 *   g = prev + ((gain - prev) * (i + 1) >> 8)                       (per stem)
 *   v = clamp16((A[i] * gA + B[i] * gB) >> 15)
 * Why nothing overflows: |A|, |B| <= 32768 and 0 <= g <= 32767 (gains are clamped on read,
 * whatever the file says), so each product is under 2^30 and their sum under 2^31. A fade
 * multiplies an s16 by at most 256.
 *
 * Big-endian, the console's order: on the console a word load reads two samples at once
 * (the sign-extending halfword load is microcoded on the Xenon, surround_dsp.h).
 */
#pragma once

#define XDM_MAGIC 0x5857444Du /* "XWDM" */
#define XDM_VERSION 1
#define XDM_HDR 64            /* header bytes, the last 4 its CRC */
#define XDM_RATE 48000
#define XDM_CHANNELS 6        /* FL FR C LFE SL SR, the tap's order */
#define XDM_STEMS 2
#define XDM_BLOCK 256         /* frames: the tap's block */
#define XDM_PAGE_BLOCKS 8
#define XDM_GAINS (XDM_STEMS * XDM_CHANNELS)
#define XDM_REC (2 * XDM_GAINS + 2 * XDM_STEMS * XDM_BLOCK)   /* a block's record: 1048 */
#define XDM_PAGE (8 + XDM_PAGE_BLOCKS * XDM_REC)              /* CRC, index, records: 8392 */
#define XDM_MAX_BLOCKS (XDM_RATE / XDM_BLOCK * 600)           /* 10 minutes: anything longer
								* is not this tool's file */
_Static_assert(XDM_REC % 8 == 0 && XDM_PAGE % 8 == 0, "records stay word aligned in a page");

/* xdm_header results */
enum { XDM_OK, XDM_E_MAGIC = -1, XDM_E_CRC = -2, XDM_E_FORMAT = -3, XDM_E_LENGTH = -4 };
/* xdm_expand modes: the block replaces the game, or is crossfaded with the game's block
 * already in `in` (from the game to the clip over the block, or back) */
enum { XDM_REPLACE, XDM_FADE_IN, XDM_FADE_OUT };

typedef u32 __attribute__((may_alias)) xdm_u32_alias;

static inline u32 xdm_be32(const u8 *p)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return *(const xdm_u32_alias *)p; /* word aligned: see XDM_REC */
#else
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
#endif
}

static inline u32 xdm_be16(const u8 *p)
{
	return (u32)p[0] << 8 | p[1];
}

/* CRC-32 (zlib's), four bits at a time: a 64-byte table instead of 1 KB, about 2 table steps
 * a byte (a page is checked once, on the file worker) */
static inline u32 xdm_crc32(const u8 *p, u32 n)
{
	static const u32 t[16] = { 0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
				   0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
				   0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
				   0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c };
	u32 crc = 0xffffffffu;

	for (u32 i = 0; i < n; i++) {
		crc ^= p[i];
		crc = (crc >> 4) ^ t[crc & 15];
		crc = (crc >> 4) ^ t[crc & 15];
	}
	return ~crc;
}

/* the header (XDM_HDR bytes): XDM_OK and the clip's pages and blocks, or why not */
static inline int xdm_header(const u8 *h, u32 *pages, u32 *blocks)
{
	u32 p = xdm_be32(h + 24), b = xdm_be32(h + 28);

	if (xdm_be32(h) != XDM_MAGIC || xdm_be16(h + 4) != XDM_VERSION || xdm_be16(h + 6) != XDM_HDR)
		return XDM_E_MAGIC;
	if (xdm_crc32(h, XDM_HDR - 4) != xdm_be32(h + XDM_HDR - 4))
		return XDM_E_CRC;
	if (xdm_be32(h + 8) != XDM_RATE || xdm_be16(h + 12) != XDM_CHANNELS ||
	    xdm_be16(h + 14) != XDM_STEMS || xdm_be16(h + 16) != XDM_BLOCK ||
	    xdm_be16(h + 18) != XDM_PAGE_BLOCKS || xdm_be32(h + 20) != XDM_PAGE)
		return XDM_E_FORMAT;
	if (!b || b > XDM_MAX_BLOCKS || p != (b + XDM_PAGE_BLOCKS - 1) / XDM_PAGE_BLOCKS ||
	    xdm_be32(h + 32) != b * XDM_BLOCK)
		return XDM_E_LENGTH;
	*pages = p;
	*blocks = b;
	return XDM_OK;
}

/* a page (XDM_PAGE bytes) is whole and is page `index` */
static inline bool xdm_page_ok(const u8 *pg, u32 index)
{
	return xdm_be32(pg + 4) == index && xdm_crc32(pg + 4, XDM_PAGE - 4) == xdm_be32(pg);
}

/* block `k` (0..XDM_PAGE_BLOCKS - 1) of a page */
static inline const u8 *xdm_record(const u8 *pg, u32 k)
{
	return pg + 8 + k * XDM_REC;
}

static inline s32 xdm_gain(const u8 *rec, u32 k)
{
	s32 g = (s16)xdm_be16(rec + 2 * k);

	return g < 0 ? 0 : g; /* the overflow bound at the top needs 0..32767 */
}

/* One block of the clip into the tap's input `in` (6 x 256 s32 holding s16, planar FL FR C
 * LFE SL SR). rec: the block's record, or NULL for silence (not loaded in time, or the clip
 * is over; the gains then stay). prev: the 12 gains the last block ended on (0 at the start),
 * updated. XDM_REPLACE writes the clip; XDM_FADE_IN / XDM_FADE_OUT mix it with the game's
 * block already in `in` (s16 values), the clip's weight rising (n of 256 at frame n - 1) or
 * falling over the block, so a start or a stop never clicks. */
static inline void xdm_expand(const u8 *rec, s32 *prev, s32 *in, u32 mode)
{
	for (u32 c = 0; c < XDM_CHANNELS; c++) {
		s32 *o = in + c * XDM_BLOCK;
		s32 a0 = prev[c], b0 = prev[XDM_CHANNELS + c], da = 0, db = 0;

		if (rec) {
			da = xdm_gain(rec, c) - a0;
			db = xdm_gain(rec, XDM_CHANNELS + c) - b0;
		}
		if (!rec || !(a0 | b0 | da | db)) {
			/* this channel of the clip is silent */
			for (u32 i = 0; i < XDM_BLOCK; i++) {
				s32 w = mode == XDM_FADE_IN ? (s32)i + 1 : 256 - (s32)i - 1;

				o[i] = mode == XDM_REPLACE ? 0 : (o[i] * (256 - w)) >> 8;
			}
			continue;
		}
		for (u32 i = 0; i < XDM_BLOCK; i += 2) {
			u32 wa = xdm_be32(rec + 2 * XDM_GAINS + 2 * i);
			u32 wb = xdm_be32(rec + 2 * XDM_GAINS + 2 * XDM_BLOCK + 2 * i);

			for (u32 j = 0; j < 2; j++) {
				s32 n = (s32)(i + j) + 1;
				s32 x = j ? (s16)wa : (s32)wa >> 16, y = j ? (s16)wb : (s32)wb >> 16;
				s32 v = (x * (a0 + ((da * n) >> 8)) + y * (b0 + ((db * n) >> 8))) >> 15;

				v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
				if (mode != XDM_REPLACE) {
					s32 w = mode == XDM_FADE_IN ? n : 256 - n;

					v = (o[i + j] * (256 - w) + v * w) >> 8;
				}
				o[i + j] = v;
			}
		}
	}
	if (rec)
		for (u32 k = 0; k < XDM_GAINS; k++)
			prev[k] = xdm_gain(rec, k);
}
