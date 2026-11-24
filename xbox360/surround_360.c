// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Headphone surround (game_360.h xg_surround*; docs/ROADMAP.md section 6, design A): the six
 * channels of the game-audio tap through the binaural virtualizer (surround_dsp.h, table
 * surround_tab.h) instead of the plain stereo downmix, in the tap itself, before the ring.
 * Everything after the ring (xg_mix, the drift trim, the buffer) is unchanged.
 *
 * The gate. XWSR `enable` (xbdm, surround.py gate on|off) is 0 at boot: the tap is then
 * today's code (game_360.c tests g_xsv_tap and takes its old path). With the gate on the tap
 * converts the six channels for the virtualizer, keeps their peaks and its own time, and the
 * SURROUND setting picks plain or virtual; with the setting off, the ring gets exactly what
 * it gets today (convert_frame's downmix), only measured.
 *
 * Threads. Driver thread: xsv_init, xsv_poll, xg_surround*. Render thread (the tap, priority
 * 25, never preempted): xsv_tap_*. They share words with one writer each:
 *   driver -> tap: g_xsv_tap (the gate), g_want (surround should be heard)
 *   tap -> driver: g_cur (surround is in the ring now), and the status counters
 * Switching (on or off) is one 256-frame crossfade in the tap. The gate only goes off after
 * the tap has faded out (g_cur 0), so turning the gate off never clicks. The state is
 * allocated once at init, never freed (the image is never unloaded) and never touched by the
 * tap while g_xsv_tap is 0.
 *
 * Cost: 5 x XSR_TAPS multiply-adds per frame (81920 per block at 64 taps; estimated
 * 95-185 us on the Xenon, budget 300 us p99), measured by tap_us. Added sound delay: none
 * beyond the filters' own (minimum phase).
 *
 * The demo (SURROUND DEMO; XWSR demo_*, surround.py demo): Hdd:\Plugins\xw.demo (format and
 * maths in surround_demo.h, made by tools/demo/make_demo.py) played in place of the game's six
 * channels, ahead of everything above, so SURROUND ON / OFF act on it as on a game. Only with
 * the gate on and the file there. Three threads:
 *   - the file worker (dm_job, file_360.h) reads whole pages into a ring of DM_SLOTS pages
 *     (58.7 KB, malloc'd at the start and freed at the end: 0 bytes while idle) and checks
 *     each page's CRC (a bad page plays as silence);
 *   - the driver thread (dm_poll, every pass) posts the reads, publishes the pages loaded
 *     (g_dm_avail), starts and stops, and frees;
 *   - the tap takes one block per tap block (so the clip runs at the render driver's own
 *     rate), fading from the game into the clip over the first block and back over the last.
 *     A block not loaded in time is silence (counted), never a wait.
 * Driver <-> tap, one writer each: g_dm_run (driver: play), g_dm_cur (tap: it is using the
 * ring), g_dm_over (tap: it faded out, the play is over), g_dm_pos (tap: blocks taken). The
 * driver writes g_dm_pos / g_dm_over only at a start, and g_dm_cur only when the tap has not
 * run for DM_TAP_GONE_MS: the tap cannot be touching them then (it writes them only while
 * g_dm_cur is set, and sets g_dm_cur only after seeing g_dm_run, Dekker style: each side
 * stores, syncs, then loads the other's word). The ring is freed only once g_dm_cur is 0 and
 * no read is in flight.
 */
#ifndef XG_HOST_TEST
#include <xecore/xboxkrnl.h>
#endif

#include "../core/platform.h"
#include <stdlib.h>
#include "diag_360.h"
#include "file_360.h"
#include "game_360.h"
#include "surround_360.h"
#include "surround_demo.h"
#include "surround_dsp.h"
#include "surround_tab.h"

#ifndef XG_HOST_TEST
#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#define FULL_BARRIER() __asm__ __volatile__("sync" ::: "memory")
#else
#define BARRIER() __sync_synchronize()
#define FULL_BARRIER() __sync_synchronize()
#endif

#define XSV_BLOCK 256        /* the tap's frame: game_360.c FRAME_SAMPLES */
#define XSV_BLOCKS_PER_S 188 /* 48000 / 256 */
#define XSV_LIVE_PEAK 104    /* -50 dBFS (ROADMAP 6.1 M1): C, SL or SR above it = 5.1 audio */
#define XSV_TB_PER_US 50     /* the timebase: 50 MHz (diag_360.h tb_per_ms checks it) */

_Static_assert(XSV_BLOCK == XSR_BLOCK, "the kernel's block is the tap's frame");

enum { XSV_ST_GATE_OFF, XSV_ST_OFF, XSV_ST_ON, XSV_ST_NO_MEMORY, XSV_ST_BAD_TABLE };

/* xbdm: the surround block ("XWSR"), read and written by surround.py (its FIELDS mirror this
 * order). All u32. Release: the first 19 words. XW_DEV builds append the measurement probe
 * (build bit 8 says so). The tap writes ch_peak, clamps, tap_*, blocks, switches and the
 * probe's results; the driver thread the rest. */
struct xsv_status {
	u32 magic, version;
	volatile u32 enable;    /* the gate: 0 (default) surround unavailable, 1 available */
	volatile u32 surround;  /* the setting: 0 off (default), 1 on (xg_set_surround) */
	u32 state;              /* XSV_ST_* */
	u32 live;               /* C, SL or SR above -50 dBFS in the last second (the panel's dot) */
	u32 build;              /* bits 0-7 XSR_TAPS, bit 8 an XW_DEV build */
	u32 ch_peak[6];         /* the last second's peak per input channel, FL FR C LFE SL SR */
	u32 clamps;             /* output samples clamped since boot (the protection working) */
	u32 heap_bytes;         /* allocated at init */
	u32 tap_us, tap_max_us; /* the tap's time per block with the gate on: last, max */
	u32 blocks, switches;   /* blocks through the virtualizer; crossfades */
#ifdef XW_DEV
	/* the measurement probe (ROADMAP 6.1): a dummy load of load_convs FIRs of load_taps
	 * (the kernel's inner loop, over real tap data, result discarded) at load_site 0 (the
	 * tap) or 1 (the driver thread, per earpiece block); test_ch 1..6 puts noise bursts
	 * into that channel (FL FR C LFE SL SR) ahead of everything, the others silent;
	 * zero = 1 resets the maxima and the histogram (the tap clears it) */
	volatile u32 load_convs, load_taps, load_site, test_ch, zero;
	u32 tap_hist[6];        /* blocks by tap time: < 50, < 100, < 200, < 400, < 800 us, more */
	u32 tap_period_max_us;  /* the longest gap between two tap blocks */
	u32 ch_peak_max[6];
	u32 load_blocks;        /* blocks that ran the dummy load */
	u32 sink;               /* the dummy load's result, kept so it isn't optimised away */
#endif
	/* ---- version 2: the demo (after the probe in XW_DEV builds: surround.py DEMO_FIELDS) */
	volatile u32 demo_cmd;  /* xbdm: 1 start, 2 stop; taken -> 0 */
	u32 demo_state;         /* DM_ST_* */
	u32 demo_file;          /* DM_FILE_*: xw.demo as last looked at */
	u32 demo_end;           /* DM_END_*: why the last play ended */
	u32 demo_plays;         /* starts */
	u32 demo_blocks;        /* the clip's length (256-frame blocks), from its header */
	u32 demo_pos;           /* blocks played in this (or the last) play */
	u32 demo_secs;          /* seconds played */
	u32 demo_underruns;     /* tap: blocks with nothing loaded in time, played as silence */
	u32 demo_reads;         /* read jobs done */
	u32 demo_bad_pages;     /* pages that failed their check or were not read: silence */
	u32 demo_read_max_us;   /* the longest read job (open to close, on the worker) */
	u32 demo_fill_min;      /* the fewest blocks loaded ahead of the tap while playing */
	u32 demo_heap;          /* bytes allocated now (0 while idle) */
};
#define XSV_RELEASE_WORDS 19
#define XSV_DEMO_WORDS 14
_Static_assert(__builtin_offsetof(struct xsv_status, blocks) == 17 * 4, "surround.py FIELDS");
#ifdef XW_DEV
_Static_assert(sizeof(struct xsv_status) == (XSV_RELEASE_WORDS + 20 + XSV_DEMO_WORDS) * 4,
	       "surround.py DEV_FIELDS, DEMO_FIELDS");
#else
_Static_assert(sizeof(struct xsv_status) == (XSV_RELEASE_WORDS + XSV_DEMO_WORDS) * 4,
	       "surround.py FIELDS, DEMO_FIELDS");
#endif

/* XWSR demo_state, demo_file, demo_end (surround.py mirrors the names) */
enum { DM_ST_IDLE, DM_ST_LOADING, DM_ST_PLAYING, DM_ST_STOPPING };
enum { DM_FILE_UNKNOWN, DM_FILE_PRESENT, DM_FILE_MISSING, DM_FILE_BAD, DM_FILE_READ_ERROR };
enum { DM_END_NONE, DM_END_DONE, DM_END_STOPPED, DM_END_TITLE, DM_END_GATE, DM_END_NO_AUDIO,
       DM_END_FILE, DM_END_NO_MEMORY, DM_END_QUIESCED };

#ifdef XW_DEV
#define XSV_BUILD_DEV 0x100
#else
#define XSV_BUILD_DEV 0
#endif

__attribute__((used)) struct xsv_status g_xsv_st = {
	.magic = 0x58575352, /* "XWSR" */
	.version = 2,        /* 2: the demo words */
	.enable = 1,         /* offered at boot (surround.py gate off) */
	.surround = 1,       /* and on: the owner's setting, Nov 23 2026 */
	.build = XSR_TAPS | XSV_BUILD_DEV,
};

#define DEV_TAPS 128
struct xsv_mem {
	struct xsr_state k;           /* the kernel: coefficients and history */
	s32 in[XSV_BLOCK * XSR_CHANNELS]; /* the tap's converted input; a fade's scratch after */
	s16 lr[2 * XSV_BLOCK];         /* the kernel's output */
#ifdef XW_DEV
	s32 dh[DEV_TAPS];             /* the dummy load's coefficients and input */
	s32 dx[DEV_TAPS + XSV_BLOCK + 1];
#endif
};
_Static_assert(sizeof(s16) * 2 * XSV_BLOCK <= sizeof(s32) * XSV_BLOCK * XSR_CHANNELS,
	       "a fade's scratch fits in the input");

volatile u32 g_xsv_tap;          /* driver -> tap: the gate */
static volatile u32 g_want;     /* driver -> tap: surround should be heard */
static volatile u32 g_cur;      /* tap -> driver: surround is in the ring */
static struct xsv_mem *g_m;
static u32 g_logged_state = 0xffffffff;
/* the tap's own */
static u32 g_pacc[XSR_CHANNELS], g_pblocks;
#ifdef XW_DEV
static u32 g_prev_t0, g_noise = 0x2545f491, g_burst;
#endif

/* ---- the demo's state (see the top) ------------------------------------------------------- */
#define DM_SLOTS 7            /* ring pages: 7 x 8392 = 58.7 KB, 56 blocks = 299 ms ahead */
#define DM_PATH XF_DIR "xw.demo"
#define DM_READ_MIN 2         /* a read waits until this many pages are free (fewer opens: the
			       * worker, like this thread, runs once a frame), unless at the end */
#define DM_NO_AUDIO_MS 2000   /* the tap not running this long while playing: stopped */
#define DM_TAP_GONE_MS 100    /* ... this long: it cannot be inside a block any more */
#define DM_RECHECK_MS 30000   /* no usable file: looked for again this often (gate on) */
#define ORD_XEX_EXECUTABLE_MODULE 403 /* XexExecutableModuleHandle: the running title */

struct dm_mem {
	u8 page[DM_SLOTS][XDM_PAGE];
	s32 prev[XDM_GAINS];  /* the tap's: the gains the last block ended on */
};
_Static_assert(sizeof(struct dm_mem) <= 65536, "the demo may hold 64 KB while it plays");

/* driver <-> tap */
static struct dm_mem *volatile g_dm; /* driver; the tap reads it only while g_dm_cur is set */
static volatile u32 g_dm_run;        /* driver -> tap: play */
static volatile u32 g_dm_avail;      /* driver -> tap: pages loaded and checked */
static volatile u32 g_dm_blocks;     /* driver -> tap: the clip's length */
static volatile u32 g_dm_cur;        /* tap -> driver: the tap is using the ring */
static volatile u32 g_dm_over;       /* tap -> driver: faded out, this play is over */
static volatile u32 g_dm_pos;        /* tap -> driver: blocks taken */
static u32 g_dm_faded, g_dm_took;    /* the tap's own: faded in; this block took a record */

/* driver <-> file worker: one job at a time, posted by the driver, `busy` cleared last */
enum { DM_JOB_HEADER, DM_JOB_PAGES };
static struct {
	volatile u32 busy;
	u32 kind, first, n; /* pages first .. first + n - 1, into dst */
	u8 *dst;
	s32 err;            /* XF_* */
	u32 got, bad, us;
	u8 hdr[XDM_HDR];
} g_job;

/* the driver thread's */
static struct {
	u32 pages;          /* the clip's, from the header */
	u32 next;           /* the next page to read */
	u32 posted;         /* a job is out (taken back by dm_poll once busy is 0) */
	u32 need_header;    /* a start waits for the header */
	u32 title;          /* the title when the play started */
	u32 quiesced, resolved;
	u64 recheck_ms;     /* the next look for a missing or unusable file */
	u64 tap_seen_ms;    /* the last pass the tap was running */
} g_dmd;
static volatile u32 *g_title; /* XexExecutableModuleHandle's value: the running title */

/* ---- render thread (the tap) ------------------------------------------------------------- */

s32 *xsv_tap_input(void)
{
	return g_m->in;
}

#ifdef XW_DEV
/* two outputs of a FIR of `taps` (the kernel's xsr_fir2 with the length at run time) */
static void dev_fir2(const s32 *h, const s32 *x, u32 taps, s32 *y)
{
	s32 a0 = 0, a1 = 0, xa = x[0];

	for (u32 k = 0; k < taps; k++) {
		s32 xb = x[k + 1];

		a0 += h[k] * xa;
		a1 += h[k] * xb;
		xa = xb;
	}
	y[0] = a0;
	y[1] = a1;
}

/* load_convs FIRs of load_taps over `frames` frames of real tap input. The values stay in
 * s16 range (the casts) and |dh| sums to at most 2.0, so nothing overflows whatever the tap
 * is writing into `in` meanwhile (the driver-thread site reads it unsynchronised). */
static void dev_load(u32 frames)
{
	struct xsv_mem *m = g_m;
	u32 convs = g_xsv_st.load_convs, taps = g_xsv_st.load_taps;
	s32 sink = 0;

	if (convs > 10)
		convs = 10;
	if (taps > DEV_TAPS)
		taps = DEV_TAPS;
	if (!convs || !taps)
		return;
	for (u32 c = 0; c < convs; c++) {
		const s32 *src = m->in + (c % XSR_CHANNELS) * XSV_BLOCK;

		for (u32 i = 0; i < XSV_BLOCK; i++)
			m->dx[DEV_TAPS + i] = (s16)src[i];
		for (u32 i = 0; i < frames; i += 2) {
			s32 y[2];

			dev_fir2(m->dh, m->dx + DEV_TAPS - taps + (i & (XSV_BLOCK - 1)), taps, y);
			sink += (y[0] >> 14) + (y[1] >> 14);
		}
	}
	g_xsv_st.sink = (u32)sink;
	g_xsv_st.load_blocks++;
}

void xsv_dev_driver_load(u32 frames)
{
	if (g_m && g_xsv_st.load_site == 1)
		dev_load(frames);
}

/* an s16-range value as the float bits the tap frame holds (value / 32768), integers only */
static u32 q15_bits(s32 v)
{
	u32 a = (u32)(v < 0 ? -v : v), e;

	if (!a)
		return 0;
	e = 31 - (u32)__builtin_clz(a); /* a in [2^e, 2^(e+1)), e <= 15 */
	return (v < 0 ? 0x80000000u : 0) | (e + 112) << 23 | ((a << (23 - e)) & 0x7fffff);
}

void xsv_dev_inject(u32 *frame)
{
	u32 ch = g_xsv_st.test_ch, on;

	if (ch < 1 || ch > XSR_CHANNELS)
		return;
	/* 0.5 s of noise (-18 dBFS peak), 0.25 s of silence, so each burst is heard as one */
	on = g_burst++ % 141 < 94;
	for (u32 c = 0; c < XSR_CHANNELS; c++)
		for (u32 i = 0; i < XSV_BLOCK; i++) {
			s32 v = 0;

			if (c == ch - 1 && on) {
				g_noise ^= g_noise << 13;
				g_noise ^= g_noise >> 17;
				g_noise ^= g_noise << 5;
				v = (s32)g_noise >> 19;
			}
			frame[c * XSV_BLOCK + i] = q15_bits(v);
		}
}
#endif

u32 xsv_tap_run(s16 **lr, s16 **plain)
{
	struct xsv_mem *m = g_m;
	u32 want = g_want, how;

	for (u32 c = 0; c < XSR_CHANNELS; c++) {
		const s32 *x = m->in + c * XSV_BLOCK;
		u32 pk = g_pacc[c];

		for (u32 i = 0; i < XSV_BLOCK; i++) {
			u32 a = (u32)(x[i] < 0 ? -x[i] : x[i]);

			if (a > pk)
				pk = a;
		}
		g_pacc[c] = pk;
	}
	if (++g_pblocks >= XSV_BLOCKS_PER_S) {
		for (u32 c = 0; c < XSR_CHANNELS; c++) {
			g_xsv_st.ch_peak[c] = g_pacc[c];
#ifdef XW_DEV
			if (g_pacc[c] > g_xsv_st.ch_peak_max[c])
				g_xsv_st.ch_peak_max[c] = g_pacc[c];
#endif
			g_pacc[c] = 0;
		}
		g_pblocks = 0;
	}
#ifdef XW_DEV
	if (g_xsv_st.load_site == 0)
		dev_load(XSV_BLOCK);
#endif
	if (!want && !g_cur)
		return XSV_PLAIN;
	if (want && !g_cur) {
		xsr_clear(&m->k); /* whatever played before is not this stream's history */
		how = XSV_FADE_IN;
	} else if (!want) {
		how = XSV_FADE_OUT;
	} else {
		how = XSV_VIRTUAL;
	}
	g_xsv_st.clamps += xsr_process(&m->k, m->in, XSV_BLOCK, m->lr);
	g_xsv_st.blocks++;
	if (how != XSV_VIRTUAL) {
		g_xsv_st.switches++;
		g_cur = how == XSV_FADE_IN;
	}
	*lr = m->lr;
	*plain = (s16 *)m->in; /* the kernel has consumed it */
	return how;
}

void xsv_tap_done(u32 t0)
{
	u32 us = (xd_timebase() - t0) / XSV_TB_PER_US;

	g_xsv_st.tap_us = us;
	if (us > g_xsv_st.tap_max_us)
		g_xsv_st.tap_max_us = us;
#ifdef XW_DEV
	{
		static const u32 edge[5] = { 50, 100, 200, 400, 800 };
		u32 b = 0, period = (t0 - g_prev_t0) / XSV_TB_PER_US;

		if (g_xsv_st.zero) {
			g_xsv_st.zero = 0;
			g_xsv_st.tap_max_us = us;
			g_xsv_st.tap_period_max_us = 0;
			for (u32 k = 0; k < 6; k++)
				g_xsv_st.tap_hist[k] = g_xsv_st.ch_peak_max[k] = 0;
			period = 0;
		}
		while (b < 5 && us >= edge[b])
			b++;
		g_xsv_st.tap_hist[b]++;
		/* a gap over 100 ms is the tap restarting (no headset), not a late block */
		if (g_prev_t0 && period < 100000 && period > g_xsv_st.tap_period_max_us)
			g_xsv_st.tap_period_max_us = period;
		g_prev_t0 = t0;
	}
#endif
}

/* The demo, at the start of a tap block (the gate is on): what this block does with it. The
 * first block of a play fades from the game into the clip; the last (stopped, or the clip
 * over) fades back. */
u32 xsv_demo_begin(void)
{
	if (!g_dm_cur) {
		if (!g_dm_run || g_dm_over)
			return XSV_DM_NONE;
		g_dm_cur = 1;
		FULL_BARRIER(); /* cur before run is read again (dm_stop: run before cur) */
		if (!g_dm_run) {
			g_dm_cur = 0;
			return XSV_DM_NONE;
		}
		g_dm_faded = 0;
	}
	if (!g_dm_faded)
		return XSV_DM_IN;
	return g_dm_run && g_dm_pos < g_dm_blocks ? XSV_DM_PLAY : XSV_DM_OUT;
}

/* the clip's block into the tap's input (replacing it, or faded with the game's there) */
void xsv_demo_fill(u32 dm, s32 *in)
{
	struct dm_mem *m = g_dm;
	u32 pos = g_dm_pos, pg = pos / XDM_PAGE_BLOCKS;
	const u8 *rec = NULL;

	if (dm == XSV_DM_IN)
		for (u32 k = 0; k < XDM_GAINS; k++)
			m->prev[k] = 0; /* every play ramps up from silence */
	if (pos < g_dm_blocks && pg < g_dm_avail) {
		BARRIER(); /* the page's bytes after the count that published them */
		rec = xdm_record(m->page[pg % DM_SLOTS], pos % XDM_PAGE_BLOCKS);
	} else if (dm == XSV_DM_PLAY) {
		g_xsv_st.demo_underruns++; /* silence this block; the clip waits (never the tap) */
	}
	g_dm_took = rec != NULL;
	xdm_expand(rec, m->prev, in, dm == XSV_DM_IN ? XDM_FADE_IN
				    : dm == XSV_DM_OUT ? XDM_FADE_OUT : XDM_REPLACE);
}

/* the block is in the ring */
void xsv_demo_end(u32 dm)
{
	if (g_dm_took)
		g_dm_pos = g_dm_pos + 1;
	if (dm == XSV_DM_IN) {
		g_dm_faded = 1;
	} else if (dm == XSV_DM_OUT) {
		BARRIER(); /* done with the ring before saying so */
		g_dm_over = 1;
		g_dm_cur = 0;
	}
}

/* ---- driver thread -------------------------------------------------------------------------- */

/* ---- the demo: file worker ---- */

/* the header, or g_job.n pages from g_job.first; a page that fails its check (or was not
 * read) is zeroed: silence, and its gains ramp to 0 and back, so no click */
static void dm_job(void *arg)
{
	u32 t0 = xd_timebase(), got = 0, bad = 0;
	s32 err;

	(void)arg;
	if (g_job.kind == DM_JOB_HEADER) {
		err = xf_read_at(DM_PATH, 0, g_job.hdr, XDM_HDR, &got);
	} else {
		err = xf_read_at(DM_PATH, XDM_HDR + g_job.first * XDM_PAGE, g_job.dst,
				 g_job.n * XDM_PAGE, &got);
		for (u32 k = 0; k < g_job.n; k++) {
			u8 *pg = g_job.dst + k * XDM_PAGE;

			if ((k + 1) * XDM_PAGE > got || !xdm_page_ok(pg, g_job.first + k)) {
				memset(pg, 0, XDM_PAGE);
				bad++;
			}
		}
	}
	g_job.err = err;
	g_job.got = got;
	g_job.bad = bad;
	g_job.us = (xd_timebase() - t0) / XSV_TB_PER_US;
	BARRIER(); /* the results and the pages before busy */
	g_job.busy = 0;
}

/* ---- the demo: driver thread ---- */

static const char *const DM_END_NAME[] = { "-", "played to the end", "stopped", "title changed",
					   "surround's gate off", "no game audio", "file failed",
					   "no memory", "quiesced" };

static u32 title_now(void)
{
	return g_title ? *g_title : 0;
}

static bool dm_post(u32 kind, u32 first, u32 n)
{
	g_job.kind = kind;
	g_job.first = first;
	g_job.n = n;
	g_job.dst = kind == DM_JOB_PAGES ? g_dm->page[first % DM_SLOTS] : NULL;
	g_job.busy = 1;
	BARRIER(); /* the job before the worker may take it */
	if (!xf_post(dm_job, NULL)) {
		g_job.busy = 0; /* no worker yet, or its queue is full: next pass */
		return false;
	}
	g_dmd.posted = 1;
	return true;
}

/* the ring goes once the tap is done with it and no read is writing it (see the top) */
static void dm_free(u64 now)
{
	if (g_dmd.posted)
		return; /* dm_poll takes the job back first */
	if (g_dm_cur) {
		if (now - g_dmd.tap_seen_ms < DM_TAP_GONE_MS)
			return; /* the tap fades out on its next block */
		g_dm_cur = 0; /* the tap has stopped running: it is not inside a block */
	}
	free(g_dm);
	g_dm = NULL;
	g_dm_avail = 0;
	g_xsv_st.demo_heap = 0;
	g_xsv_st.demo_state = DM_ST_IDLE;
	xa_log("surround: demo ended (%s) after %u s; underruns %u, bad pages %u, longest read %u us, "
	       "fewest blocks ahead %u\n", DM_END_NAME[g_xsv_st.demo_end], g_xsv_st.demo_secs,
	       g_xsv_st.demo_underruns, g_xsv_st.demo_bad_pages, g_xsv_st.demo_read_max_us,
	       g_xsv_st.demo_fill_min);
}

static void dm_stop(u32 why)
{
	u32 st = g_xsv_st.demo_state;

	if (st == DM_ST_IDLE || st == DM_ST_STOPPING)
		return;
	g_dm_run = 0;
	FULL_BARRIER(); /* run before cur is read (xsv_demo_begin: cur before run) */
	g_xsv_st.demo_end = why;
	g_xsv_st.demo_state = DM_ST_STOPPING;
	g_dmd.need_header = 0;
	dm_free(xa_now_ms());
}

/* the header has been read for a start: the ring, then the first reads (dm_poll) */
static void dm_load(void)
{
	struct dm_mem *m = malloc(sizeof(*m));

	if (!m) {
		xa_log("surround: demo: no memory for its ring (%u bytes)\n", (u32)sizeof(*m));
		dm_stop(DM_END_NO_MEMORY);
		return;
	}
	g_dmd.next = 0;
	g_dm_avail = 0;
	g_dm_blocks = g_xsv_st.demo_blocks;
	g_dm = m;
	g_xsv_st.demo_heap = sizeof(*m);
	g_xsv_st.demo_fill_min = DM_SLOTS * XDM_PAGE_BLOCKS;
}

static void dm_job_done(u64 now)
{
	g_dmd.posted = 0;
	if (g_job.kind == DM_JOB_HEADER) {
		u32 pages = 0, blocks = 0, f;

		f = g_job.err == XF_E_NOFILE			    ? DM_FILE_MISSING
		    : g_job.err || g_job.got != XDM_HDR		    ? DM_FILE_READ_ERROR
		    : xdm_header(g_job.hdr, &pages, &blocks) != XDM_OK ? DM_FILE_BAD
								    : DM_FILE_PRESENT;
		if (f != g_xsv_st.demo_file)
			xa_log("surround: demo file %s (%u blocks)\n",
			       f == DM_FILE_PRESENT ? "found" : f == DM_FILE_MISSING ? "missing"
			       : f == DM_FILE_BAD ? "not a version 1 xw.demo" : "unreadable", blocks);
		g_xsv_st.demo_file = f;
		g_dmd.recheck_ms = now + DM_RECHECK_MS;
		if (f == DM_FILE_PRESENT) {
			g_dmd.pages = pages;
			g_xsv_st.demo_blocks = blocks;
		}
		if (g_dmd.need_header) {
			g_dmd.need_header = 0;
			if (f == DM_FILE_PRESENT)
				dm_load();
			else
				dm_stop(DM_END_FILE);
		}
		return;
	}
	g_xsv_st.demo_reads++;
	g_xsv_st.demo_bad_pages += g_job.bad;
	if (g_job.us > g_xsv_st.demo_read_max_us)
		g_xsv_st.demo_read_max_us = g_job.us;
	if (g_job.err) {
		g_xsv_st.demo_file = DM_FILE_READ_ERROR; /* the file went away under the play */
		dm_stop(DM_END_FILE);
		return;
	}
	g_dmd.next = g_job.first + g_job.n;
	BARRIER(); /* the pages (the worker's, seen through busy) before the count */
	g_dm_avail = g_dmd.next;
}

/* every pass, after the gate: commands, the file, the reads, starting, stopping */
static void dm_poll(u64 now, bool gate)
{
	u32 cmd = g_xsv_st.demo_cmd, st;

	if (cmd) {
		g_xsv_st.demo_cmd = 0;
		if (cmd == 1)
			xg_demo_start();
		else
			xg_demo_stop();
	}
	if (g_dmd.posted && !g_job.busy) {
		BARRIER(); /* busy before the results */
		dm_job_done(now);
	}
	if (xg_routed())
		g_dmd.tap_seen_ms = now;
	st = g_xsv_st.demo_state;
	if (st == DM_ST_IDLE) {
		/* look for the file while the gate is on (the panel offers the row only then) */
		if (gate && !g_dmd.posted && !g_dmd.quiesced &&
		    (g_xsv_st.demo_file == DM_FILE_UNKNOWN ||
		     (g_xsv_st.demo_file != DM_FILE_PRESENT && now >= g_dmd.recheck_ms)))
			dm_post(DM_JOB_HEADER, 0, 0);
		return;
	}
	if (st == DM_ST_STOPPING) {
		dm_free(now);
		return;
	}
	if (!gate)
		dm_stop(DM_END_GATE);
	else if (title_now() != g_dmd.title)
		dm_stop(DM_END_TITLE);
	else if (now - g_dmd.tap_seen_ms >= DM_NO_AUDIO_MS)
		dm_stop(DM_END_NO_AUDIO);
	else if (g_dm_over)
		dm_stop(DM_END_DONE);
	if (g_xsv_st.demo_state != st)
		return;
	if (!g_dm) {
		if (g_dmd.need_header && !g_dmd.posted)
			dm_post(DM_JOB_HEADER, 0, 0);
		return;
	}
	{
		u32 pos = g_dm_pos, q = g_dmd.next, free_ = DM_SLOTS - (q - pos / XDM_PAGE_BLOCKS), n;
		u32 avail = g_dm_avail, ahead;

		n = free_ < g_dmd.pages - q ? free_ : g_dmd.pages - q;
		if (n > DM_SLOTS - q % DM_SLOTS)
			n = DM_SLOTS - q % DM_SLOTS; /* one read fills slots that follow each other */
		if (!g_dmd.posted && n &&
		    (st == DM_ST_LOADING || free_ >= DM_READ_MIN || q + n == g_dmd.pages))
			dm_post(DM_JOB_PAGES, q, n);
		if (st == DM_ST_LOADING && avail >= (g_dmd.pages < DM_SLOTS ? g_dmd.pages : DM_SLOTS)) {
			BARRIER(); /* the ring and the counts before the tap may start */
			g_dm_run = 1;
			g_xsv_st.demo_state = DM_ST_PLAYING;
			xa_log("surround: demo playing (%u s, %u ms read ahead)\n",
			       g_dm_blocks * 2 / 375, DM_SLOTS * XDM_PAGE_BLOCKS * 256 / 48);
		}
		ahead = (avail * XDM_PAGE_BLOCKS < g_dm_blocks ? avail * XDM_PAGE_BLOCKS : g_dm_blocks) - pos;
		if (st == DM_ST_PLAYING && q < g_dmd.pages && ahead < g_xsv_st.demo_fill_min)
			g_xsv_st.demo_fill_min = ahead; /* the margin, while there is more to read */
		g_xsv_st.demo_pos = pos;
		g_xsv_st.demo_secs = pos * 2 / 375; /* 187.5 blocks a second */
	}
}

void xg_demo_start(void)
{
	if (!xg_surround_offered() || g_dmd.quiesced || g_xsv_st.demo_state != DM_ST_IDLE) {
		xa_log("surround: demo not started (%s)\n", g_dmd.quiesced ? "quiesced"
			: g_xsv_st.demo_state != DM_ST_IDLE ? "already playing" : "surround unavailable");
		return;
	}
#ifndef XG_HOST_TEST
	if (!g_dmd.resolved) {
		HANDLE k;
		void *p = 0;

		g_dmd.resolved = 1;
		if (XexGetModuleHandle("xboxkrnl.exe", &k) >= 0)
			XexGetProcedureAddress(k, ORD_XEX_EXECUTABLE_MODULE, &p);
		g_title = p;
	}
#endif
	g_xsv_st.demo_plays++;
	g_xsv_st.demo_state = DM_ST_LOADING;
	g_xsv_st.demo_end = DM_END_NONE;
	g_xsv_st.demo_pos = g_xsv_st.demo_secs = 0;
	/* the tap's words of the last play: it is not using them (idle: g_dm_cur and g_dm_run 0) */
	g_dm_pos = 0;
	g_dm_over = 0;
	g_dmd.need_header = 1; /* read again: the file may have been replaced since it was found */
	g_dmd.title = title_now();
	g_dmd.tap_seen_ms = xa_now_ms();
	xa_log("surround: demo starting (surround %s)\n", g_xsv_st.surround ? "on" : "off");
}

void xg_demo_stop(void)
{
	dm_stop(DM_END_STOPPED);
}

bool xg_demo_offered(void)
{
	return xg_surround_offered() && g_xsv_st.demo_file == DM_FILE_PRESENT;
}

bool xg_demo_playing(u32 *secs)
{
	u32 st = g_xsv_st.demo_state;

	*secs = g_xsv_st.demo_secs;
	return st == DM_ST_LOADING || st == DM_ST_PLAYING;
}

/* Hot reload: stopped; the ring is freed if nothing can be using it (tap_idle: the tap will
 * never run again). Otherwise it stays allocated (logged): a read in flight, or a tap that may
 * be inside a block. */
void xsv_quiesce(bool tap_idle)
{
	g_dmd.quiesced = 1;
	if (g_xsv_st.demo_state == DM_ST_IDLE)
		return;
	dm_stop(DM_END_QUIESCED);
	if (g_xsv_st.demo_state != DM_ST_IDLE && tap_idle && !g_dmd.posted) {
		g_dm_cur = 0;
		dm_free(xa_now_ms());
	}
	if (g_xsv_st.demo_state != DM_ST_IDLE)
		xa_log("surround: demo quiesced with a %s: its %u bytes stay allocated\n",
		       g_dmd.posted ? "read in flight" : "tap still running", g_xsv_st.demo_heap);
}

void xsv_resume(void)
{
	g_dmd.quiesced = 0;
}

static const char *const STATE_NAME[] = { "gate off", "off", "on", "no memory", "bad table" };

void xsv_init(void)
{
	int bad = xsr_table_ok(&xsr_tab);

	if (bad) {
		g_xsv_st.state = XSV_ST_BAD_TABLE;
		xa_log("surround: the filter table fails its check (%d): unavailable\n", bad);
		return;
	}
	g_m = malloc(sizeof(*g_m));
	if (!g_m) {
		g_xsv_st.state = XSV_ST_NO_MEMORY;
		xa_log("surround: no memory for its state (%u bytes): unavailable\n",
		       (u32)sizeof(*g_m));
		return;
	}
	xsr_init(&g_m->k, &xsr_tab);
#ifdef XW_DEV
	/* the table's rows one after the other up to DEV_TAPS, each divided by how many fit:
	 * sum|dh| <= 2.0, the kernel's own bound, at any load_taps */
	for (u32 k = 0; k < DEV_TAPS; k++)
		g_m->dh[k] = xsr_tab.h[k / XSR_TAPS % XSR_FILTERS][k % XSR_TAPS] /
			     (DEV_TAPS > XSR_TAPS ? DEV_TAPS / XSR_TAPS : 1);
	for (u32 k = 0; k < DEV_TAPS + XSV_BLOCK + 1; k++)
		g_m->dx[k] = 0;
#endif
	g_xsv_st.heap_bytes = sizeof(*g_m);
	xa_log("surround: ready, %u taps, %u bytes (gate %s)\n", XSR_TAPS, (u32)sizeof(*g_m),
	       g_xsv_st.enable ? "on" : "off");
}

void xsv_poll(void)
{
	u32 en = g_xsv_st.enable ? 1 : 0, on = g_xsv_st.surround ? 1 : 0, ok = g_m != 0, st;

	if (g_xsv_st.enable != en)
		g_xsv_st.enable = en; /* a bad xbdm value, written back as it is taken */
	if (g_xsv_st.surround != on)
		g_xsv_st.surround = on;
	if (ok && en && !g_xsv_tap) {
		g_want = 0;
		BARRIER(); /* g_want before the tap may read it */
		g_xsv_tap = 1;
	}
	g_want = ok && en && on;
	if (g_xsv_tap && !en && !g_cur && !g_dm_cur)
		g_xsv_tap = 0; /* the tap has faded out (or never faded in), the demo too */
	dm_poll(xa_now_ms(), ok && en);

	st = g_xsv_st.state == XSV_ST_BAD_TABLE ? XSV_ST_BAD_TABLE
	     : !ok			      ? XSV_ST_NO_MEMORY
	     : !en			      ? XSV_ST_GATE_OFF
	     : on			      ? XSV_ST_ON
					      : XSV_ST_OFF;
	g_xsv_st.state = st;
	if (st != g_logged_state) {
		g_logged_state = st;
		xa_log("surround: %s\n", STATE_NAME[st]);
	}
	{
		const u32 *p = g_xsv_st.ch_peak;
		u32 m = p[XSR_C] > p[XSR_SL] ? p[XSR_C] : p[XSR_SL];

		if (p[XSR_SR] > m)
			m = p[XSR_SR];
		g_xsv_st.live = g_xsv_tap && xg_routed() && m > XSV_LIVE_PEAK;
	}
}

u32 xg_surround(void)
{
	return g_xsv_st.surround ? 1 : 0;
}

void xg_set_surround(u32 on)
{
	g_xsv_st.surround = on ? 1 : 0; /* xsv_poll applies it, the tap crossfades */
}

bool xg_surround_live(void)
{
	return g_xsv_st.live != 0;
}

bool xg_surround_offered(void)
{
	return g_m && g_xsv_st.enable;
}
