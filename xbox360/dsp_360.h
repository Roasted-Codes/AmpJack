// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Integer DSP helpers shared by the audio files (no floating point anywhere in the plugin:
 * docs/INTERNALS.md). Pure functions, no globals: they compile on the Mac for host tests.
 * Add to this file instead of copying a helper into another one.
 *
 * They are `static` (not `static inline`), as they were in voice_360.c: `inline` would raise
 * clang's inlining threshold and change where they are inlined, which the file split was
 * proven not to do. `unused` keeps -Wall quiet in a file that calls only some of them.
 *
 * Not the same as these, so not replaced yet (their owners should look when they next edit):
 * game_360.c and team_360.c have a clamp16 that returns s32; team_360.c's isin interpolates
 * with a 16-bit fraction (this one: 8 bits). xw_plugin.c's self-test, echo.c and voicetest.c
 * have an isqrt64.
 */
#ifndef DSP_360_H
#define DSP_360_H

#include "../core/compat.h"

static __attribute__((unused)) s16 clamp16(s32 v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v;
}

/* sin(2 pi i / 256), Q15, i = 0..64: a quarter wave */
static const s16 SIN_Q[65] = {
	0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179,
	7962, 8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732,
	15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403,
	22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
	27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571,
	30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
	32609, 32678, 32728, 32757, 32767,
};

/* sin(2 pi ph / 2^32), Q15, linearly interpolated */
static __attribute__((unused)) s32 isin(u32 ph)
{
	u32 q = ph >> 30, i = (ph >> 24) & 63, fr = (ph >> 16) & 255;
	s32 a = (q & 1) ? SIN_Q[64 - i] : SIN_Q[i];
	s32 b = (q & 1) ? SIN_Q[63 - i] : SIN_Q[i + 1];
	s32 v = a + ((b - a) * (s32)fr >> 8);

	return (q & 2) ? -v : v;
}

static __attribute__((unused)) u32 isqrt(u32 v)
{
	u32 r = 0, b = 1u << 30;

	while (b > v)
		b >>= 2;
	while (b) {
		if (v >= r + b) {
			v -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
		b >>= 2;
	}
	return r;
}

#endif
