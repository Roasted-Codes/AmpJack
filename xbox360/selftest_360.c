// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Headset audio diagnostics for development sessions: the self-test (audio stage 5: a tone
 * out, the mic analysed in), the mic dump (xw.py micdump), and the xbdm commands usbreset,
 * hsrestart and mecreset (XWAU usbreset word). Compiled into XW_DEV builds only (a release
 * build gets xw_priv.h's stubs). Driver thread, except reset_dpc (Usbd's DPC).
 */
#include <stdlib.h>
#include <xecore/xboxkrnl.h>

#include "../core/gip.h"
#include "../core/platform.h"
#include "game_360.h"
#include "xw_priv.h"

#ifdef XW_DEV

/* ~1 s of raw mic PCM for xw.py micdump (xbdm getmem) */
#define MICDUMP_MAX 0x10000
struct xw_micdump {
	u32 magic; /* "XWMD" */
	u32 rate, channels;
	volatile u32 bytes;
	u32 size;  /* bytes wanted */
	u8 data[MICDUMP_MAX];
};
/* allocated when xw.py micdump first arms it: 64 KB only in a session that asks for it */
static struct xw_micdump *g_micdump;

/* usbreset: re-enumerate, as an unplug and replug. arg: the node the thread saw; only reset
 * if a port still holds exactly that device */
static void reset_dpc(void *arg)
{
	for (int i = 0; i < PORTS; i++) {
		if (arg && g_ports[i].claimed == arg && !g_ports[i].removing) {
			g_xw.audio.resets++;
			usbd.reset_device(arg, 1);
			return;
		}
	}
}

/* ---- self-test: tone out, analysis in (integer only) ---------------------------------------- */

static const s16 sine256[256] = {
	0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
	9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
	18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811,
	25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
	30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
	32609, 32678, 32728, 32757, 32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
	32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571, 30273, 29956, 29621, 29268,
	28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
	23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151,
	15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
	6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0, -804, -1608, -2410,
	-3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
	-12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
	-20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
	-27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
	-31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
	-32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
	-31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
	-27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
	-20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
	-12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011,
	-3212, -2410, -1608, -804,
};

#define TONE_AMPLITUDE 8192 /* about -12 dBFS */

/* the phase step of hz at rate, in 2^-32 cycles */
static u32 phase_step(u32 hz, u32 rate)
{
	return rate ? (u32)(((u64)hz << 32) / rate) : 0;
}

static u32 tone_hz(u32 rate)
{
	u32 hz = g_xw.audio.tone_hz;

	if (hz < 100)
		hz = 100;
	if (rate && hz > rate / 2 - 500)
		hz = rate / 2 - 500;
	return hz;
}

void aud_tone(u8 *out, u32 bytes, u32 rate, u32 ch)
{
	u32 step = phase_step(tone_hz(rate), rate), n = ch ? bytes / (2 * ch) : 0;

	for (u32 i = 0; i < n; i++) {
		s16 v = (s16)((sine256[g_audio.tone_phase >> 24] * TONE_AMPLITUDE) >> 15);

		g_audio.tone_phase += step;
		for (u32 c = 0; c < ch; c++)
			put_le16(out + (i * ch + c) * 2, (u16)v);
	}
	memset(out + n * ch * 2, 0, bytes - n * ch * 2);
}

static u32 isqrt64(u64 v)
{
	u64 r = 0, bit = (u64)1 << 62;

	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (u32)r;
}

/* I/Q correlation of the mic with the tone and with an off-tone frequency, plus RMS and
 * peak, over 100 ms windows (first channel only) */
static struct {
	int64_t i, q, oi, oq;
	u64 sq;
	u32 n, peak, ph, oph;
} an;

static u32 amp_of(int64_t i, int64_t q, u32 n)
{
	int64_t a = i >> 15, b = q >> 15;

	return n ? (u32)(2 * (u64)isqrt64((u64)(a * a) + (u64)(b * b)) / n) : 0;
}

void aud_analyse(const u8 *s, int len, u32 rate, u32 ch)
{
	struct xw_audio_status *as = &g_xw.audio;
	u32 hz = tone_hz(rate), off = hz + 437 < rate / 2 ? hz + 437 : hz - 437;
	u32 step = phase_step(hz, rate), ostep = phase_step(off, rate);
	u32 window = rate / 10;

	if (!rate || !ch)
		return;
	for (int k = 0; k + 2 * (int)ch <= len; k += 2 * (int)ch) {
		s32 x = (s16)get_le16(s + k);
		u32 m = (u32)(x < 0 ? -x : x);

		an.i += (int64_t)(x * sine256[((an.ph >> 24) + 64) & 255]);
		an.q += (int64_t)(x * sine256[an.ph >> 24]);
		an.oi += (int64_t)(x * sine256[((an.oph >> 24) + 64) & 255]);
		an.oq += (int64_t)(x * sine256[an.oph >> 24]);
		an.ph += step;
		an.oph += ostep;
		an.sq += (u64)(x * x);
		if (m > an.peak)
			an.peak = m;
		if (++an.n < window)
			continue;
		as->tone_amp = amp_of(an.i, an.q, an.n);
		as->off_amp = amp_of(an.oi, an.oq, an.n);
		as->off_hz = off;
		as->mic_rms = isqrt64(an.sq / an.n);
		as->mic_peak = an.peak;
		as->windows++;
		an.i = an.q = an.oi = an.oq = 0;
		an.sq = 0;
		an.n = an.peak = 0;
	}
}

void aud_dump(const u8 *s, int len, u32 rate, u32 ch)
{
	struct xw_audio_status *as = &g_xw.audio;
	u32 room;

	if (as->micdump != 2)
		return;
	if (!g_micdump->bytes) {
		g_micdump->rate = rate;
		g_micdump->channels = ch;
		g_micdump->size = rate * ch * 2;
		if (!g_micdump->size || g_micdump->size > MICDUMP_MAX)
			g_micdump->size = MICDUMP_MAX;
	}
	room = g_micdump->size - g_micdump->bytes;
	if ((u32)len > room)
		len = (int)room;
	memcpy(g_micdump->data + g_micdump->bytes, s, (size_t)len);
	BARRIER();
	g_micdump->bytes += (u32)len;
	if (g_micdump->bytes >= g_micdump->size) {
		as->micdump = 3;
		xa_log("xw: mic dump done: %u bytes at %u Hz x%u (%08x)\n", g_micdump->bytes,
		       g_micdump->rate, g_micdump->channels, (u32)g_micdump);
	}
}

void aud_service_commands(void)
{
	struct xw_audio_status *as = &g_xw.audio;
	u32 r = as->usbreset;

	if (as->micdump == 1 && !g_micdump) {
		g_micdump = malloc(sizeof(*g_micdump));
		if (g_micdump) {
			g_micdump->magic = 0x58574D44; /* "XWMD" */
			as->micdump_addr = (u32)g_micdump;
		} else {
			as->micdump = 0;
			xa_log("xw: mic dump: out of memory\n");
		}
	}
	if (as->micdump == 1) {
		g_micdump->bytes = 0;
		BARRIER();
		as->micdump = 2;
		xa_log("xw: mic dump armed\n");
	}
	if (!r)
		return;
	as->usbreset = 0;
	/* 0x200: game audio's MEC client unregistered and registered again, as a reload does
	 * across two builds (a diagnostic: xw.py mecreset) */
	if (r == 0x200) {
		trace(TR_MEC_RESET);
		xa_log("xw: MEC reset: unregistering the game audio client, then registering again\n");
		xg_quiesce();
		xg_resume();
		trace(TR_MEC_RESUMED);
		return;
	}
	/* 0x100 | port + 1: the headset identifies itself again (the stage is latched then),
	 * as if replugged, without touching USB */
	if (r & 0x100) {
		u32 port = (r & 0xff) - 1;
		int err;

		if (port >= PORTS || !g_ports[port].gc || !g_ports[port].node ||
		    g_ports[port].removing) {
			xa_log("xw: headset restart: no controller in port %u\n", port);
			return;
		}
		/* xw.py hsrestart PORT IN OUT: with that format (r >> 16: in, r >> 24: out) */
		if (r >> 16)
			err = gip_headset_resume(g_ports[port].gc, (u8)(r >> 16), (u8)(r >> 24));
		else
			err = gip_headset_reidentify(g_ports[port].gc);
		xa_log("xw: headset restart, port %u: %d%s\n", port, err,
		       err == -XA_ENODEV ? " (no headset format known and none announced in this "
					   "session: xw.py hsrestart PORT IN OUT, e.g. 0 9 10)" : "");
		return;
	}
	if (r > PORTS) {
		xa_log("xw: usbreset: no port %u\n", r - 1);
		return;
	}
	{
		struct xw_port *p = &g_ports[r - 1];

		void *node;

		if (!p->claimed || p->removing) {
			xa_log("xw: usbreset: no device in port %u\n", r - 1);
			return;
		}
		aud_disable(p, "USB reset");
		node = p->claimed; /* reset_dpc only resets this exact device */
		if (!node || p->removing) {
			xa_log("xw: usbreset: port %u's device went away meanwhile\n", r - 1);
			return;
		}
		xa_log("xw: usbreset: port %u, device %08x: re-enumerating\n", r - 1, (u32)node);
		xa_usbd_run(reset_dpc, node);
	}
}

#endif
