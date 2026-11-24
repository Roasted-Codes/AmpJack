// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of the surround demo clip's reader (xbox360/surround_demo.h) on a real file made
 * by tools/demo/make_demo.py: tests/test_demo_file.py runs it, or
 *
 *   python3 tools/demo/make_demo.py -o /tmp/xw.demo &&
 *   cc -O2 -Wall -o /tmp/test_demo tests/test_demo.c && /tmp/test_demo /tmp/xw.demo
 *
 * Checks, with the console's code: the header is accepted and says what the file holds; every
 * page passes its CRC and index check; every block expanded (XDM_REPLACE, as the tap plays it)
 * stays at or under -6 dBFS in every channel and peaks exactly where the header says; a
 * damaged header, a damaged page and a page in the wrong place are refused; the fades start
 * from the game and end on it; a missing block is silence and keeps the gains.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/platform.h"
#include "../xbox360/surround_demo.h"

#define PEAK_LIMIT 16422 /* -6.0 dBFS */

static int g_fail;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		g_fail = 1;
}

int main(int argc, char **argv)
{
	static s32 in[XDM_CHANNELS * XDM_BLOCK];
	s32 prev[XDM_GAINS] = { 0 };
	u32 pages = 0, blocks = 0, peak[XDM_CHANNELS] = { 0 }, top = 0, bad_pages = 0, used = 0;
	char msg[200];
	FILE *f;
	long size;
	u8 *d;

	if (argc < 2) {
		fprintf(stderr, "usage: test_demo FILE.demo\n");
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc((size_t)size);
	if (!d || fread(d, 1, (size_t)size, f) != (size_t)size)
		return 2;
	fclose(f);

	check(xdm_header(d, &pages, &blocks) == XDM_OK &&
		      (long)XDM_HDR + (long)pages * XDM_PAGE == size,
	      "header accepted, pages x XDM_PAGE + header = the file's size");
	snprintf(msg, sizeof msg, "%u blocks (%u.%02u s), %u pages", blocks, blocks * 256 / 48000,
		 blocks * 256 % 48000 * 100 / 48000, pages);
	check(blocks >= 40 * 48000 / 256 && blocks <= 45 * 48000 / 256, msg);
	for (u32 p = 0; p < pages; p++) {
		const u8 *pg = d + XDM_HDR + (size_t)p * XDM_PAGE;

		if (!xdm_page_ok(pg, p)) {
			bad_pages++;
			continue;
		}
		for (u32 k = 0; k < XDM_PAGE_BLOCKS && p * XDM_PAGE_BLOCKS + k < blocks; k++) {
			xdm_expand(xdm_record(pg, k), prev, in, XDM_REPLACE);
			for (u32 c = 0; c < XDM_CHANNELS; c++)
				for (u32 i = 0; i < XDM_BLOCK; i++) {
					u32 a = (u32)(in[c * XDM_BLOCK + i] < 0 ? -in[c * XDM_BLOCK + i]
										: in[c * XDM_BLOCK + i]);

					if (a > peak[c])
						peak[c] = a;
				}
		}
	}
	snprintf(msg, sizeof msg, "every page whole and in its place (%u bad)", bad_pages);
	check(!bad_pages, msg);
	for (u32 c = 0; c < XDM_CHANNELS; c++) {
		if (peak[c] > top)
			top = peak[c];
		used += peak[c] > 0;
	}
	snprintf(msg, sizeof msg, "peaks FL %u FR %u C %u LFE %u SL %u SR %u: <= %u (-6 dBFS), "
		 "the header's %u, five speakers used, no LFE", peak[0], peak[1], peak[2], peak[3],
		 peak[4], peak[5], PEAK_LIMIT, xdm_be32(d + 36));
	check(top <= PEAK_LIMIT && top == xdm_be32(d + 36) && used == 5 && !peak[3], msg);

	/* refusals */
	{
		u8 h[XDM_HDR];
		u32 a, b;

		memcpy(h, d, XDM_HDR);
		h[0] ^= 1;
		check(xdm_header(h, &a, &b) == XDM_E_MAGIC, "a wrong magic is refused");
		memcpy(h, d, XDM_HDR);
		h[30] ^= 1;
		check(xdm_header(h, &a, &b) == XDM_E_CRC, "a damaged header is refused");
		check(!xdm_page_ok(d + XDM_HDR, 1), "page 0 read where page 1 should be is refused");
		d[XDM_HDR + 100] ^= 0x10;
		check(!xdm_page_ok(d + XDM_HDR, 0), "a damaged page is refused");
		d[XDM_HDR + 100] ^= 0x10;
	}
	/* fades and silence, on a block that has sound in FL */
	{
		const u8 *rec = NULL;
		s32 g[XDM_GAINS], keep[XDM_GAINS];
		int ok = 1;

		for (u32 p = 0; p < pages && !rec; p++)
			for (u32 k = 0; k < XDM_PAGE_BLOCKS && !rec; k++) {
				const u8 *r = xdm_record(d + XDM_HDR + (size_t)p * XDM_PAGE, k);

				if (xdm_gain(r, 0) > 16000 && xdm_be32(r + 24 + 200) != 0)
					rec = r;
			}
		for (u32 k = 0; k < XDM_GAINS; k++)
			g[k] = xdm_gain(rec, k);
		memcpy(keep, g, sizeof(g));
		for (u32 k = 0; k < XDM_CHANNELS * XDM_BLOCK; k++)
			in[k] = 1000;
		xdm_expand(rec, g, in, XDM_FADE_IN);
		{
			static s32 ref[XDM_CHANNELS * XDM_BLOCK];
			s32 g2[XDM_GAINS];

			memcpy(g2, keep, sizeof(g2));
			xdm_expand(rec, g2, ref, XDM_REPLACE);
			/* frame 0: almost all game; the last frame: all clip */
			ok &= in[0] == (1000 * 255 + ref[0]) >> 8 && in[255] == ref[255] &&
			      in[XDM_BLOCK * 3] /* LFE: no clip */ == (1000 * 255) >> 8;
			for (u32 k = 0; k < XDM_CHANNELS * XDM_BLOCK; k++)
				in[k] = 1000;
			memcpy(g2, keep, sizeof(g2));
			xdm_expand(rec, g2, in, XDM_FADE_OUT);
			ok &= in[0] == (1000 + ref[0] * 255) >> 8 && in[255] == 1000;
		}
		check(ok, "fade in: from the game to the clip; fade out: back to the game exactly");
		for (u32 k = 0; k < XDM_CHANNELS * XDM_BLOCK; k++)
			in[k] = 1234;
		xdm_expand(NULL, g, in, XDM_REPLACE);
		ok = !memcmp(g, keep, sizeof(g));
		for (u32 k = 0; k < XDM_CHANNELS * XDM_BLOCK; k++)
			ok &= in[k] == 0;
		check(ok, "a missing block is silence and keeps the gains");
	}
	printf("%s\n", g_fail ? "FAILED" : "all passed");
	free(d);
	return g_fail;
}
