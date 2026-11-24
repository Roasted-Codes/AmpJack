// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Placeholder for team voice (team_360.h): opens nothing, mixes nothing, and the 360 keeps
 * the mic. team_360.c replaces it (build.sh, XW_TEAM).
 */
#include "team_360.h"

void xt_init(void)
{
}

void xt_poll(void)
{
}

void xt_quiesce(void)
{
}

void xt_resume(void)
{
}

void xt_mic(int pad, const u8 *pcm, int len, u32 rate, u32 ch, bool open)
{
	(void)pad;
	(void)pcm;
	(void)len;
	(void)rate;
	(void)ch;
	(void)open;
}

void xt_mix(int pad, u8 *out, int len, u32 rate_out, u32 ch_out, u32 gain)
{
	(void)pad;
	(void)out;
	(void)len;
	(void)rate_out;
	(void)ch_out;
	(void)gain;
}

u32 xt_channel(void)
{
	return 0;
}

void xt_set_channel(u32 ch)
{
	(void)ch;
}

u32 xt_level(void)
{
	return 0;
}

void xt_set_level(u32 level)
{
	(void)level;
}

u32 xt_mic_route(void)
{
	return 1; /* GAME: the mic only goes to the 360 */
}

void xt_set_mic_route(u32 r)
{
	(void)r;
}

u32 xt_peers(void)
{
	return 0;
}

u32 xt_talking_mask(void)
{
	return 0;
}

bool xt_mic_to_game(void)
{
	return true;
}
