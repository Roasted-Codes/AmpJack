// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The sidetone (software, or the headset's own), the headset's volume settings, hiss control,
 * the centre tick, the chat gain and the mic mute: the "XVHS" block (voice.py status).
 * Driver thread only; they work on the headset's own PCM from xv_mic_samples /
 * xv_earpiece_fill (voice_360.c), whatever the 360 does with the headset. voice_priv.h lists
 * what the other audio files use from here.
 */
#include "../core/gip.h" /* GIP_VOL_*: the headset's own volume settings */
#include "game_360.h"
#include "voice_360.h"
#include "voice_priv.h"

/* hiss control (dev_quiet): when the mic last heard a voice (a 20 ms window at or above the
 * low gate's threshold, whatever the gate is set to), and when the earpiece last had a sample
 * above QUIET_PEAK to play. Driver thread. */
u64 g_voice_ms[PADS], g_ear_loud_ms[PADS];

u8 g_mic_mute[PADS];

/* ---- sidetone and the centre tick (driver thread only) -------------------------------------
 * Both work on the headset's own PCM in xv_mic_samples / xv_earpiece_fill, whatever the 360
 * does with the headset (serving or not), and never touch the 360-side streams.
 *
 * Sidetone: the own mic in the earpiece at xg_sidetone() x 10 % x master (not balanced),
 * through a FIFO with no deliberate delay. The mic arrives in 8 ms blocks (one IN transfer)
 * and the earpiece is filled 8 ms at a time on the same USB clock, so the FIFO only waits
 * for ST_PRIME_MS (one block and a margin) before reading. A gap (it ran dry) fades the held
 * sample out over ST_FADE_MS and waits for ST_PRIME_MS again; more than ST_HIGH_MS buffered
 * (the earpiece stalled) fades out and skips back to ST_PRIME_MS: no click either way.
 * Muted or level 0: the gain ramps to 0 over one block, then the FIFO stops.
 * Feedback guard (the mic sits inside the ear cup): the output (after every gain) is measured
 * in ST_WIN_MS windows; an RMS that keeps climbing for ST_TRIP_MS, one window above
 * ST_LOUD_RMS, or a level that never drops under ST_STEADY_RMS for ST_STEADY_MS (speech has
 * pauses, a howl doesn't) pulls the sidetone gain down 4x (12 dB, ramped over a block) and
 * holds it there for ST_DUCK_MS; it then comes back gently (ST_RECOVER: about 1.4 s for
 * 12 dB), but only to half of where it tripped (6 dB under the howl), until the sidetone
 * level or the master volume is changed (or a mute, or the headset reconnecting). A howl that comes back anyway is
 * caught earlier and at a lower level each time; 1/256 at the least.
 *
 * The centre tick (xv_tick): TICK_MS of TICK_HZ under a raised-cosine envelope, peak
 * TICK_PEAK (-30 dBFS) times master, from an integer sine table. The only generated sound.
 *
 * Safety: no gain above 1 anywhere (sidetone <= 1, tick 1000/32768); every mixed sample is
 * clamped. Off at boot (sidetone 0), never saved. */
#define ST_RING 2048         /* mono mic samples (power of two): 42 ms at 48 kHz */
#define ST_PRIME_MS 10       /* buffered before the first read: one 8 ms mic block + 2 ms */
#define ST_HIGH_MS 26        /* buffered beyond this: skip back to ST_PRIME_MS */
#define ST_FADE_MS 2         /* fade out at a gap or skip, in after it */
#define ST_WIN_MS 50
#define ST_TRIP_MS 300
#define ST_DUCK_MS 1000
#define ST_GUARD_ONE 32768   /* the guard factor, Q15: 1 = not pulled down */
#define ST_GUARD_MIN 128     /* 1/256 */
#define ST_RECOVER 128       /* after the hold: guard += guard / 128 per 8 ms block */
#define ST_SETTLE_MS 200     /* after a trip, the level needs this long to fall: no new trip */
#define ST_FLOOR_RMS 300     /* "climbing" only counts above this (room noise wanders) */
#define ST_LOUD_RMS 8000     /* -12 dBFS in the ear */
#define ST_STEADY_RMS 2000   /* -24 dBFS */
#define ST_STEADY_MS 2000
#define TICK_HZ 2000
#define TICK_MS 15
#define TICK_PEAK 1000       /* -30.3 dBFS, before the master volume */

struct xv_side {
	s16 ring[ST_RING];
	u32 w, r;              /* written / read (mic-rate samples) */
	u32 prime, high, fade; /* ST_PRIME_MS, ST_HIGH_MS, ST_FADE_MS in mic-rate samples */
	u32 on, primed, skip;
	s32 last;              /* the last sample read (held while fading out) */
	u32 env;               /* 0..fade: the gap fade */
	struct rs rs;          /* mic rate -> earpiece rate */
	s16 f[16];             /* resampled, not yet used */
	int f_n, f_pos;
	u32 gain;              /* Q15 (32768 = 1), in effect at the end of the last block */
	u32 guard, ceil;       /* Q15: the feedback guard's factor on the gain, and its limit */
	u64 trip_at;           /* the last trip: held until + ST_DUCK_MS, next trip + ST_SETTLE_MS */
	u32 setting;           /* level and master the ceiling belongs to */
	u32 win_sum, win_n, win_len, last_rms, rise_ms, steady_ms;
	u32 tick_pos, tick_len, tick_phase, tick_step, tick_env_step;
};

static struct xv_side g_side[PADS];

/* xbdm: sidetone and mute state, read by voice.py status ("XVHS"; its own block, so the XV36
 * block keeps its layout). All u32, in this order: voice.py's HS_GLOBAL / HS_PAD mirror it.
 * The sidetone level itself is a mix control in the XVGA block (game_360.c). */
struct xv_hs_pad_status {
	u32 muted, gain, rms, ducks, guard, gaps, skips, fill, mixed, ticks;
};
struct xv_hs_status {
	u32 magic, version;
	struct xv_hs_pad_status pads[PADS];
	/* version 3 (appended): which path makes the sidetone, and the headset's own volume
	 * settings (MS-GIPUSB 3.2.5.1.1, core/gip.h GIP_VOL_*). voice.py's HS_TAIL mirrors it.
	 * Controls (xbdm, validated and written back by xv_headset_volume): */
	volatile u32 st_mode;     /* ST_MODE_*: 0 software, 1 the headset's own, 2 auto (default:
				   * the headset's own when its report marks sidetone writable) */
	volatile u32 dev_max;     /* the headset's sidetone % at SIDETONE 10 (default 100) */
	volatile u32 dev_speaker; /* the headset's speaker volume % asked for (default 100) */
	volatile u32 dev_mic;     /* the headset's mic volume % asked for (default 100) */
	volatile u32 dev_bit7;    /* 1: set bit 7 on the fields we write (default 0, as xone) */
	/* state */
	u32 st_path;              /* ST_PATH_*: 0 off (level 0 / muted), 1 software, 2 headset */
	u32 dev_pad;              /* the pad slot the dev_* fields are about, 0xFFFFFFFF = none */
	u32 dev_reports;          /* volume reports from that headset */
	u32 dev_report[2];        /* the last: bytes flags speaker balance mic | sidetone r1 r2
				   * length, first byte in the high bits */
	u32 dev_writable;         /* bit n = report field n (GIP_VOL_*) has bit 7 set */
	u32 dev_sends;            /* volume messages sent to it */
	u32 dev_sent[2];          /* the last: flags speaker balance mic | sidetone 0 0 0 */
	u32 dev_sidetone;         /* the headset's sidetone % asked for now (after the guard) */
	u32 dev_trips;            /* its feedback guard pulled it down */
	u32 side_delay_ms;        /* software path: mouth-to-ear estimate (FIFO + mic block +
				   * OUT queue), computed */
	volatile u32 dev_vol;     /* 1 (default): VOLUME turns the headset's amplifier (speaker %
				   * = dev_speaker x VOLUME) while its report marks the speaker
				   * field writable, and the samples go out at full level; 0: the
				   * samples are scaled, the amplifier stays at dev_speaker */
	u32 dev_vol_on;           /* that is in effect now */
	volatile u32 dev_quiet;   /* hiss control (default 3). bit 0: the headset's sidetone is on
				   * only while the mic hears a voice (else it plays the mic
				   * input's noise all the time); bit 1: with nothing to play (the
				   * earpiece samples silent, no sidetone) the headset's amplifier
				   * is turned to 0, and back up before the next sound */
};
#define XV_HS_TAIL 20 /* u32s after pads[] */
_Static_assert(sizeof(struct xv_hs_pad_status) == 10 * 4, "voice.py HS_PAD");
_Static_assert(sizeof(struct xv_hs_status) == (2 + PADS * 10 + XV_HS_TAIL) * 4,
	       "voice.py HS_GLOBAL / HS_TAIL");

enum { ST_MODE_SOFTWARE, ST_MODE_DEVICE, ST_MODE_AUTO, ST_MODES };
enum { ST_PATH_OFF, ST_PATH_SOFTWARE, ST_PATH_DEVICE };

__attribute__((used)) struct xv_hs_status g_xv_hs = {
	.magic = 0x58564853, /* "XVHS" */
	.version = 3,
	.st_mode = ST_MODE_AUTO,
	.dev_max = 100,
	.dev_speaker = 100,
	.dev_vol = 1,
	.dev_quiet = 3,
	.dev_mic = 100,
	.dev_pad = 0xFFFFFFFF,
};

/* the headset's own sidetone (xv_headset_volume): in use for the pad (st_mix stays
 * silent), and its feedback guard (st_dev_watch) */
struct xv_side_dev {
	u32 on;           /* the headset makes the sidetone */
	u32 pct;          /* asked for now */
	u32 cap;          /* the guard's limit (100 = none) until the setting changes */
	u32 setting;      /* level, max, mute the cap belongs to */
	u64 duck_until;   /* the guard holds it at a quarter until then */
	u64 trip_at;
	u32 steady_ms;
};
static struct xv_side_dev g_sdev[PADS];


/* the FIFO empty and idle at the pad's (new) format, the guard cleared */
static void st_idle(int pad)
{
	struct xv_side *s = &g_side[pad];
	struct xv_pad *p = &g_pad[pad];
	u32 in = rate_ok(p->rate_in) ? p->rate_in : 8000;
	u32 out = rate_ok(p->rate_out) ? p->rate_out : 8000;

	s->w = s->r = 0;
	s->prime = in / 1000 * ST_PRIME_MS;
	s->high = in / 1000 * ST_HIGH_MS;
	s->fade = in / 1000 * ST_FADE_MS;
	s->on = s->primed = s->skip = 0;
	s->last = 0;
	s->env = 0;
	rs_init(&s->rs, in, out);
	s->f_n = s->f_pos = 0;
	s->gain = 0;
	s->guard = s->ceil = ST_GUARD_ONE;
	s->trip_at = 0;
	s->setting = 0;
	s->win_sum = s->win_n = s->last_rms = s->rise_ms = s->steady_ms = 0;
	s->win_len = out / 1000 * ST_WIN_MS;
	g_xv_hs.pads[pad].gain = g_xv_hs.pads[pad].rms = 0;
	g_xv_hs.pads[pad].guard = ST_GUARD_ONE;
	g_xv_hs.pads[pad].fill = 0;
}

/* the headset went away, or came with a (possibly new) format: the sidetone starts over and
 * a tick in progress stops */
void hs_headset_lost(int pad)
{
	st_idle(pad);
	xg_master_device(false); /* until the next headset's volume report */
	g_xv_hs.dev_vol_on = 0;
	memset(&g_sdev[pad], 0, sizeof(g_sdev[pad])); /* software until its next volume report */
	g_side[pad].tick_pos = g_side[pad].tick_len = 0;
	xd_reset_pad(pad);
}

void hs_format(int pad)
{
	hs_headset_lost(pad);
}

/* the mic (s16le frames at rate_in x ch_in) into the FIFO while the sidetone is on */
void st_mic(int pad, const u8 *d, int frames, u32 ch)
{
	struct xv_side *s = &g_side[pad];

	if (!s->on)
		return;
	for (int f = 0; f < frames; f++, d += 2 * ch) {
		s32 x = (s16)get_le16(d);

		if (ch == 2)
			x = (x + (s16)get_le16(d + 2)) >> 1;
		s->ring[s->w % ST_RING] = (s16)x;
		s->w++;
	}
	if (s->w - s->r > ST_RING)
		s->r = s->w - ST_RING; /* a ring behind (the earpiece stopped): st_next skips */
}

/* the next mic sample at the earpiece rate, with the gap fade */
static s32 st_next(int pad)
{
	struct xv_side *s = &g_side[pad];
	struct xv_hs_pad_status *hs = &g_xv_hs.pads[pad];

	while (s->f_pos >= s->f_n) {
		u32 avail = s->w - s->r;

		if (s->primed && !s->skip && avail > s->high) {
			s->skip = 1; /* too far behind: fade out, then jump to the newest samples */
			hs->skips++;
		}
		if (!s->primed && !s->skip && avail >= s->prime)
			s->primed = 1;
		if (s->primed && !s->skip && avail) {
			s->last = s->ring[s->r % ST_RING];
			s->r++;
			if (s->env < s->fade)
				s->env++;
		} else {
			if (s->primed && !s->skip) {
				s->primed = 0; /* ran dry: fade out, wait for ST_PRIME_MS again */
				hs->gaps++;
			}
			if (s->env)
				s->env--;
			if (!s->env && s->skip) {
				s->r = s->w - s->prime;
				s->skip = 0;
			}
		}
		hs->fill = s->w - s->r;
		s->f_n = rs_push(&s->rs, s->last * (s32)s->env / (s32)s->fade, s->f);
		s->f_pos = 0;
	}
	return s->f[s->f_pos++];
}

/* the guard, per output sample: why to pull the gain down now, or NULL */
static const char *st_watch(int pad, s32 y)
{
	struct xv_side *s = &g_side[pad];
	struct xv_hs_pad_status *hs = &g_xv_hs.pads[pad];
	const char *why;
	u32 rms;

	s->win_sum += (u32)(y * y) >> 12; /* <= 2^18 each; a 48 kHz window is 2400 of them */
	if (++s->win_n < s->win_len)
		return NULL;
	rms = isqrt((s->win_sum / s->win_n) << 12);
	s->win_sum = s->win_n = 0;
	hs->rms = rms;
	s->rise_ms = rms > ST_FLOOR_RMS && rms > s->last_rms + s->last_rms / 16
			     ? s->rise_ms + ST_WIN_MS
			     : 0;
	s->steady_ms = rms > ST_STEADY_RMS ? s->steady_ms + ST_WIN_MS : 0;
	s->last_rms = rms;
	why = rms > ST_LOUD_RMS ? "too loud" : s->rise_ms >= ST_TRIP_MS ? "kept rising"
	      : s->steady_ms >= ST_STEADY_MS ? "steady" : NULL;
	if (why)
		s->rise_ms = s->steady_ms = 0;
	return why;
}

/* the sidetone mixed into an earpiece block (s16le, rate_out x ch_out); master: Q8 */
void st_mix(int pad, u8 *out, int len, u32 master)
{
	struct xv_side *s = &g_side[pad];
	struct xv_hs_pad_status *hs = &g_xv_hs.pads[pad];
	u32 ch = g_pad[pad].ch_out, frame = 2 * ch, level = xg_sidetone();
	int frames = len / (int)frame;
	s32 from = (s32)s->gain, to;
	u64 now = xa_now_ms();

	if (g_sdev[pad].on)
		level = 0; /* the headset mixes it itself (xv_headset_volume): ramp out, stop */
	else if (level && !g_mic_mute[pad] && master && g_pad[pad].rate_in)
		/* the mic block (8 ms), half a driver loop pass on average before the thread
		 * sees it (~16.6 ms passes), the FIFO, and the OUT blocks queued ahead of this
		 * one (xw_plugin.c's AUD_OUT_BLOCKS = 5, 8 ms each): what this sidetone lags the
		 * voice by, as an estimate (not measured) */
		g_xv_hs.side_delay_ms = 8 + 8 + hs->fill * 1000 / g_pad[pad].rate_in + 5 * 8;
	if (level > XG_SIDETONE_MAX)
		level = XG_SIDETONE_MAX;
	if (master > 256)
		master = 256;
	to = g_mic_mute[pad] ? 0 : (s32)(level * master * 128 / XG_SIDETONE_MAX); /* <= 32768 */
	if (s->setting != (level << 16 | master)) {
		s->setting = level << 16 | master;
		s->ceil = ST_GUARD_ONE; /* a new setting: the user decides again */
	}
	if (s->guard < s->ceil && now >= s->trip_at + ST_DUCK_MS) {
		s->guard += s->guard / ST_RECOVER + 1; /* the hold is over: back up, gently */
		if (s->guard > s->ceil)
			s->guard = s->ceil;
		hs->guard = s->guard;
	}
	to = (s32)((u32)to * s->guard >> 15);
	if (!to && !from) {
		if (s->on)
			st_idle(pad); /* off (level 0, muted or master 0): the FIFO stops */
		return;
	}
	s->on = 1; /* the FIFO fills from the next mic block; silence until it's primed */
	for (int f = 0; f < frames; f++) {
		s32 g = from + (to - from) * f / frames; /* a gain change ramps over the block */
		s32 y = st_next(pad) * g >> 15;
		const char *why = st_watch(pad, y);

		if (why && (!s->trip_at || now >= s->trip_at + ST_SETTLE_MS)) {
			s->ceil = s->guard / 2 > ST_GUARD_MIN ? s->guard / 2 : ST_GUARD_MIN;
			s->guard >>= 2; /* heard from the next block (ramped) */
			if (s->guard < ST_GUARD_MIN)
				s->guard = ST_GUARD_MIN;
			s->trip_at = now;
			hs->ducks++;
			hs->guard = s->guard;
			xa_log("voice: pad %d: sidetone pulled down to %u/32768 for %u ms (level %u: %s)\n",
			       pad, s->guard, ST_DUCK_MS, hs->rms, why);
		}
		if (!y)
			continue;
		hs->mixed++;
		for (u32 c = 0; c < ch; c++) {
			u8 *o = out + f * (int)frame + 2 * (int)c;

			put_le16(o, (u16)clamp16((s16)get_le16(o) + y));
		}
	}
	s->gain = (u32)to;
	hs->gain = (u32)to;
}

/* ---- the headset's own sidetone (driver thread only) ----------------------------------------
 * MS-GIPUSB 3.2.5.1.1 (Audio Control Volume Extended): the headset reports flags, speaker,
 * game/chat balance, mic and SIDETONE (mic monitoring, default 0), each with bit 7 = writable
 * by the host, and applies what the host sends, answering with a report of what it did. Its
 * DSP mixes the mic into the earpiece with no USB round trip: a MixAmp's mic monitor, where
 * the software path above lags by ~60 ms (the mic block, a loop pass, the FIFO and the 40 ms
 * of OUT blocks queued on Usbd: side_delay_ms).
 *
 * xv_headset_volume (from xw_plugin.c, every loop pass while the headset is ready) picks the
 * path (st_mode: auto = the headset's own when its report marks the sidetone field writable,
 * else software) and says what the headset should be set to: sidetone = SIDETONE x 10 % of
 * dev_max (not scaled by the master volume: a MixAmp's monitor has its own knob), 0 when
 * muted or at 0. The software sidetone is then silent (st_mix). Speaker and mic volume are
 * xbdm settings (dev_speaker / dev_mic, 100 %: the mic level, the gate and the meter work on
 * the mic as the headset sends it, so the headset's mic gain is not a panel row).
 *
 * Feedback guard for this path (st_dev_watch, per 20 ms window of the mic as it arrives):
 * the level in the ear is estimated as the mic RMS x the sidetone %; one window above
 * ST_DEV_LOUD_RMS, or never under ST_STEADY_RMS for ST_STEADY_MS (speech pauses, a howl
 * doesn't), sets it to a quarter for ST_DUCK_MS, then to at most half of where it tripped
 * until the level is changed (or a mute, or the headset reconnecting). "Too loud" is 6 dB
 * above the software guard's: the jack mic runs hot (Halo 3 needed mic level 50 %), and a
 * shout shouldn't take the monitor away; a howl is caught as "steady" within 2 s. */
#define ST_DEV_LOUD_RMS (2 * ST_LOUD_RMS) /* -6 dBFS */
static void dev_watch_reset(struct xv_side_dev *d, u32 setting)
{
	d->setting = setting;
	d->cap = 100;
	d->duck_until = 0;
	d->steady_ms = 0;
}

/* a 20 ms window of pad's mic (its RMS as it arrives), from mic_analyse */
void st_dev_watch(int pad, u32 rms, u32 win_ms)
{
	struct xv_side_dev *d = &g_sdev[pad];
	u64 now = xa_now_ms();
	u32 ear = rms * d->pct / 100;
	const char *why;

	if (!d->on || !d->pct) {
		d->steady_ms = 0;
		return;
	}
	d->steady_ms = ear > ST_STEADY_RMS ? d->steady_ms + win_ms : 0;
	why = ear > ST_DEV_LOUD_RMS ? "too loud" : d->steady_ms >= ST_STEADY_MS ? "steady" : NULL;
	if (!why || (d->trip_at && now < d->trip_at + ST_SETTLE_MS))
		return;
	d->cap = d->pct / 2;
	d->duck_until = now + ST_DUCK_MS;
	d->trip_at = now;
	d->steady_ms = 0;
	g_xv_hs.dev_trips++;
	xa_log("voice: pad %d: the headset's sidetone pulled down from %u%% (mic rms %u: %s): "
	       "%u%% for %u ms, then at most %u%%\n", pad, d->pct, rms, why, d->pct / 4,
	       ST_DUCK_MS, d->cap);
}

static u32 pack4(const u8 *b, u32 n)
{
	u32 v = 0;

	for (u32 i = 0; i < 4; i++)
		v = v << 8 | (i < n ? b[i] : 0);
	return v;
}

static u32 clamp_ctl(volatile u32 *v, u32 max, u32 dflt)
{
	if (*v > max)
		*v = dflt; /* written back: xbdm sees what is in effect */
	return *v;
}

void xv_headset_volume(int pad, const u8 *report, u32 report_len, u32 reports, const u8 *sent,
		       u32 sends, u8 *want)
{
	struct xv_hs_status *h = &g_xv_hs;
	struct xv_side_dev *d;
	u32 mode = clamp_ctl(&h->st_mode, ST_MODES - 1, ST_MODE_AUTO);
	u32 max = clamp_ctl(&h->dev_max, 100, 100), spk = clamp_ctl(&h->dev_speaker, 100, 100);
	u32 mic = clamp_ctl(&h->dev_mic, 100, 100), b7 = clamp_ctl(&h->dev_bit7, 1, 0) << 7;
	u32 level = xg_sidetone(), setting, pct, writable = 0;
	bool on, vol;
	u32 quiet = clamp_ctl(&h->dev_quiet, 3, 3);
	u64 now = xa_now_ms();

	memcpy(want, sent, GIP_VOL_FIELDS);
	if (pad < 0 || pad >= PADS)
		return;
	d = &g_sdev[pad];
	for (u32 i = 0; i < GIP_VOL_FIELDS && report && i < report_len; i++)
		if (report[i] & GIP_VOL_WRITABLE)
			writable |= 1u << i;
	on = mode == ST_MODE_DEVICE || (mode == ST_MODE_AUTO && (writable & 1u << GIP_VOL_SIDETONE));
	if (on != (d->on != 0))
		xa_log("voice: pad %d: sidetone by %s (mode %u; the headset %s)\n", pad,
		       on ? "the headset itself" : "software", mode,
		       !reports ? "sent no volume report yet"
		       : writable & 1u << GIP_VOL_SIDETONE ? "marks it writable"
							 : "marks it read-only");
	d->on = on;
	/* VOLUME by the headset's amplifier: its noise then drops with the volume */
	vol = clamp_ctl(&h->dev_vol, 1, 1) && reports && (writable & 1u << GIP_VOL_SPEAKER);
	xg_master_device(vol);
	h->dev_vol_on = vol;
	if (vol)
		spk = spk * xg_master() / 100;
	if (level > XG_SIDETONE_MAX)
		level = XG_SIDETONE_MAX;
	setting = level << 16 | max << 1 | g_mic_mute[pad];
	if (setting != d->setting)
		dev_watch_reset(d, setting); /* a new setting: the user decides again */
	pct = on && !g_mic_mute[pad] ? (level * max + XG_SIDETONE_MAX / 2) / XG_SIDETONE_MAX : 0;
	if (pct > d->cap)
		pct = d->cap;
	if (now < d->duck_until && pct > d->cap / 2)
		pct = d->cap / 2; /* the hold: a quarter of where it tripped */
	d->pct = pct; /* the feedback guard works on the level asked for */
	/* hiss control: no sidetone while nobody talks; no amplifier while nothing plays */
	if ((quiet & 1) && now - g_voice_ms[pad] >= QUIET_TALK_MS)
		pct = 0;
	if ((quiet & 2) && (writable & 1u << GIP_VOL_SPEAKER) && !pct &&
	    now - g_ear_loud_ms[pad] >= QUIET_AMP_MS)
		spk = 0;

	want[GIP_VOL_FLAGS] = GIP_VOL_F_HEADSET;
	want[GIP_VOL_SPEAKER] = (u8)(spk | b7);
	want[GIP_VOL_BALANCE] = 50;
	want[GIP_VOL_MIC] = (u8)(mic | b7);
	/* off: 0 whatever the mode, so a headset left in auto never keeps a stale sidetone */
	want[GIP_VOL_SIDETONE] = (u8)(pct | b7);

	h->st_path = !level || g_mic_mute[pad] ? ST_PATH_OFF
		     : on			     ? ST_PATH_DEVICE
						     : ST_PATH_SOFTWARE;
	h->dev_pad = (u32)pad;
	h->dev_reports = reports;
	h->dev_report[0] = report ? pack4(report, report_len) : 0;
	h->dev_report[1] = report && report_len > 4
				   ? (pack4(report + 4, report_len - 4) & 0xFFFFFF00u) | report_len
				   : report_len;
	h->dev_writable = writable;
	h->dev_sends = sends;
	h->dev_sent[0] = pack4(sent, 4);
	h->dev_sent[1] = (u32)sent[GIP_VOL_SIDETONE] << 24;
	h->dev_sidetone = pct;
}

/* the centre tick mixed into an earpiece block; master: Q8 */
void tick_mix(int pad, u8 *out, int len, u32 master)
{
	struct xv_side *s = &g_side[pad];
	u32 ch = g_pad[pad].ch_out, frame = 2 * ch;
	int frames = len / (int)frame;

	if (master > 256)
		master = 256;
	for (int f = 0; f < frames && s->tick_pos < s->tick_len; f++) {
		/* raised cosine: sin^2(pi n / len), from the first half of the same table */
		s32 e = isin(s->tick_pos * s->tick_env_step);
		s32 y = isin(s->tick_phase) * (e * e >> 15) >> 15; /* Q15 */

		y = (y * TICK_PEAK >> 15) * (s32)master >> 8;
		s->tick_phase += s->tick_step;
		s->tick_pos++;
		for (u32 c = 0; c < ch; c++) {
			u8 *o = out + f * (int)frame + 2 * (int)c;

			put_le16(o, (u16)clamp16((s16)get_le16(o) + y));
		}
	}
}

/* s16le samples times a Q8 gain (<= 256) */
static u32 g_chat_gain_cur = 256; /* the chat gain last applied: a change ramps over a block */

void scale(u8 *out, int len, u32 gain)
{
	s32 from = (s32)g_chat_gain_cur, n = len / 2;

	if (gain > 256)
		gain = 256;
	g_chat_gain_cur = gain;
	if (gain == 256 && from == 256)
		return;
	for (s32 i = 0; i < n; i++) {
		s32 g = from + ((s32)gain - from) * i / n; /* no zipper click on a balance step */

		put_le16(out + 2 * i, (u16)(s16)((s16)get_le16(out + 2 * i) * g >> 8));
	}
}

void xv_tick(void)
{
	for (int i = 0; i < PADS; i++) {
		struct xv_side *s = &g_side[i];
		struct xv_pad *p = &g_pad[i];

		if (!hs_format_ok(p))
			continue;
		s->tick_len = p->rate_out / 1000 * TICK_MS;
		s->tick_pos = 0;
		s->tick_phase = 0;
		s->tick_step = 0xFFFFFFFFu / p->rate_out * TICK_HZ;
		s->tick_env_step = 0x80000000u / s->tick_len; /* half a turn (pi) over the tick */
		g_xv_hs.pads[i].ticks++;
	}
}

bool xv_mic_muted(int pad)
{
	return pad >= 0 && pad < PADS && g_mic_mute[pad];
}

bool xv_toggle_mic_mute(int pad)
{
	if (pad < 0 || pad >= PADS)
		return false;
	g_mic_mute[pad] = !g_mic_mute[pad];
	g_xv_hs.pads[pad].muted = g_mic_mute[pad];
	xa_log("voice: pad %d: mic %s\n", pad, g_mic_mute[pad] ? "muted" : "on");
	return g_mic_mute[pad];
}
