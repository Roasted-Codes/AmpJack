// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Per-pad quirks the GIP parser doesn't cover. For now only where the Share button is: 360
 * titles never see Share, and it isn't in gip_gamepad_state, so the headset controls
 * (panel_360.c) read it from the raw GIP input packet (command 0x20) themselves.
 *
 * One entry per controller model that differs from the default: the packet byte and bit of Share, the shortest packet that
 * has that byte, and the shortest payload length (packet byte 3) the pad sends Share in.
 * docs/PLAN.md WP1.3 (the table), WP2.1 (more pads: the SCUF Instinct Pro 2e95:0504 once
 * `xw.py raw` has shown its byte on the console).
 *
 * Header-only (static const data: .rdata, no .bss), driver thread.
 *
 * Wireless (docs/WIRELESS-NOTES.md): every entry was read over USB. Whether a pad reports
 * Share at the same byte over the adapter's radio is untested.
 */
#ifndef PAD_QUIRKS_H
#define PAD_QUIRKS_H

#include "../core/compat.h"

struct pq_share {
	u16 vid, pid;
	u8 byte;        /* packet byte (the 4-byte GIP header included) */
	u8 mask;
	u8 min_len;     /* the packet has at least this many bytes */
	u8 min_payload; /* pkt[3] (the payload length) at least this */
};

/* The Series layout: payload byte 18 bit 0. Seen with xw.py raw on the console on the Xbox
 * Series pad (045e:0b12, fw 5.9.2709.0) and the SCUF Instinct Pro (2e95:0504, Nov 23 2026).
 * A pad that isn't in the table below is read this way too: a pad with a Share button copies
 * Microsoft's report, and a pad without one sends shorter packets (-1: no Share). */
static const struct pq_share pq_share_default = { 0, 0, 22, 0x01, 23, 19 };

/* pads whose Share is somewhere else (none known yet) */
static const struct pq_share pq_share_table[] = {
	{ 0x045e, 0x0b12, 22, 0x01, 23, 19 },
	{ 0x2e95, 0x0504, 22, 0x01, 23, 19 },
};

/* the Share button in one raw input packet: 1 down, 0 up, -1 = not known for this pad, or
 * the packet is too short to say (keep the last state) */
static inline int pq_share_down(u16 vid, u16 pid, const u8 *pkt, int len)
{
	for (unsigned int i = 0; i < sizeof(pq_share_table) / sizeof(pq_share_table[0]); i++) {
		const struct pq_share *q = &pq_share_table[i];

		if (q->vid != vid || q->pid != pid)
			continue;
		if (len < q->min_len || len < 4 || pkt[3] < q->min_payload)
			return -1;
		return (pkt[q->byte] & q->mask) != 0;
	}
	if (len < pq_share_default.min_len || len < 4 || pkt[3] < pq_share_default.min_payload)
		return -1;
	return (pkt[pq_share_default.byte] & pq_share_default.mask) != 0;
}

#endif
