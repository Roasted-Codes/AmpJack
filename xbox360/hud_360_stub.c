// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Placeholder for the on-screen headset bar (hud_360.h): accepts the snapshots and draws
 * nothing. hud_360.c replaces it (build.sh, XW_HUD).
 */
#include "hud_360.h"

void hud_publish(const struct hud_state *s)
{
	(void)s;
}

void hud_init(void)
{
}

void hud_poll(void)
{
}

void hud_quiesce(void)
{
}

void hud_resume(void)
{
}
