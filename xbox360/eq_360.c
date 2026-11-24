// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The headset equalizer (eq_360.h): the presets of Hdd:\Plugins\xw.presets, run by the
 * integer kernel in eq_dsp.h on every earpiece block.
 *
 * Threads: everything here runs on the driver thread except load_job, which runs on the file
 * worker thread (file_360.h xf_post): it reads and checks the file, builds the whole table in
 * one malloc'd block and hands it over through g_job: table pointer and result first, an
 * lwsync, then `done`. The driver thread (xq_poll) reads `done`, issues the matching lwsync,
 * then takes the table; from then on only the driver thread touches it. One job at a time, so
 * no lock is needed (and the kernel rules allow no lock that could block the driver thread).
 * A job that never finishes (xf_quiesce drops queued jobs) only leaves the EQ at FLAT.
 *
 * Gate: XWEQ `enable`, off by default. Off: xq_count() is 0 (the panel offers FLAT only) and
 * xq_process touches nothing. The file is still loaded at start-up, so switching it on is
 * immediate.
 *
 * Loudness: the file's flat_q15 is FLAT's level while a file is loaded; every preset is
 * matched to it (tools/presets/convert.py LOUDNESS), so switching never jumps in loudness.
 * FLAT at 0 dB (no file, or a file without it) passes the samples through untouched.
 *
 * CUSTOM: the file's tables (eqd_custom) give every step of BASS / MIDS / TREBLE and the
 * preamp of every combination; a change builds the new cascade (eqd_custom_bands) into a
 * slot no stream is playing or fading in, and the streams fade to it as to any preset. A
 * slot a stream still plays is never written: that would change a running filter's
 * coefficients under its history (a click, or a burst from a low shelf). 2 x XQ_PADS + 2
 * slots: each pad holds at most two (playing, fading in), the current combination one, so a
 * free one always exists, however fast the steps come (a held direction repeats 8 times a
 * second; a fade takes 20 ms, and a newer request waits for it, so in-between combinations
 * are simply skipped). 0 0 0 is FLAT itself: the same pointer, so selecting it is FLAT bit
 * for bit and starts no fade. The steps are kept without a file too (settings_360.c restores
 * them at boot), and the cascade is built when the file arrives.
 *
 * State per pad (each headset is its own stream); the selected preset is one for all.
 */
#include <stdlib.h>

#include "../core/platform.h"
#include "eq_360.h"
#include "eq_dsp.h"
#include "file_360.h"

#ifndef XQ_HOST_TEST
#define XQ_BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
/* the timebase (50 MHz, as micdsp_360.c's xd_timebase): the EQ's own time per pass */
static inline u32 XQ_TB(void)
{
	u32 t;

	__asm__ __volatile__("mftb %0" : "=r"(t));
	return t;
}
#else
#define XQ_BARRIER() __sync_synchronize()
static u32 g_host_tb, g_host_tb_step; /* tests/test_eq360.c: each read advances it a step */
#define XQ_TB() (g_host_tb += g_host_tb_step)
#endif
#define XQ_TB_PER_US 50

#define XQ_PADS 4          /* the plugin's pad slots (voice_360.c PADS) */
#define XQ_CHUNK 192       /* frames per kernel call: the work buffer is 2 x this s32 */
#define XQ_FILE_MAX (EQD_HDR + XQ_MAX_PRESETS * EQD_REC + EQD_TAIL_MAX) /* else XF_E_TOOBIG */
#define XQ_POST_TRIES 10   /* xf_post refused (queue full, or no worker): retries */
#define XQ_POST_RETRY_MS 1000
#define XQ_NO_SELECT 0xFFFFFFFFu
#define XQ_SEL_CUSTOM 0xFFFFFFFEu /* g_sel: CUSTOM, whatever its index (it moves with count) */
#define XQ_CUSTOM_SLOTS (2 * XQ_PADS + 2) /* see the top */
#define XQ_CUSTOM_SET 0x01000000u /* XWEQ custom_set: this bit and the packed steps */
/* Built for size: everything here but eqd_filter (eq_dsp.h, out of line at -O2), which is
 * where the time goes (about 95 % of a block). The rest runs a few times a second, or is a
 * byte swap / store pass per block that costs a few microseconds either way; clang's -O2
 * unrolls and specialises those loops into about 2 KB more image. */
#define XQ_SMALL __attribute__((minsize))

_Static_assert(XQ_MAGIC == EQD_MAGIC && XQ_VERSION == EQD_VERSION && XQ_RATE == EQD_RATE &&
		       XQ_BANDS == EQD_BANDS && sizeof(struct xq_file_preset) == EQD_REC &&
		       sizeof(struct xq_file_header) == EQD_HDR && XQ_DESC_LEN == EQD_DESC &&
		       XQ_CUSTOM_MAGIC == EQD_CMAGIC && XQ_CUSTOM_BANDS == EQD_CBANDS &&
		       XQ_CUSTOM_STEPS == EQD_CSTEPS && -XQ_CUSTOM_MIN == EQD_CMID &&
		       sizeof(struct xq_file_custom) == EQD_CUSTOM_BYTES &&
		       __builtin_offsetof(struct xq_file_preset, desc) == 160,
	       "eq_360.h and eq_dsp.h describe the same file");

/* XWEQ load_state */
enum { XQ_LOAD_IDLE, XQ_LOAD_POSTED, XQ_LOAD_OK, XQ_LOAD_FAILED, XQ_LOAD_NO_WORKER };
/* XWEQ load_error: file_360.h's XF_E_* (-1..-4), or these */
enum {
	XQ_E_SHORT = -11,   /* eqd_check -1 */
	XQ_E_MAGIC = -12,   /* -2: magic or version */
	XQ_E_FORMAT = -13,  /* -3: rate, bands or record size */
	XQ_E_LENGTH = -14,  /* -4 */
	XQ_E_CRC = -15,     /* -5 */
	XQ_E_COUNT = -16,   /* more than XQ_MAX_PRESETS */
	XQ_E_RECORD = -17,  /* a record's id is not FNV-1a of its name */
	XQ_E_NOMEM = -18,
	XQ_E_OLD = -19,     /* -6: a version-1 file (eq.py install puts the new one in place) */
};
/* XWEQ custom_error: why CUSTOM is not offered (eqd_custom_read), or 0 */
enum { XQ_C_NOFILE = -10 }; /* no preset file loaded (yet) */

/* FLAT's and CUSTOM's descriptions: the panel shows a description in its one help line, 36
 * characters (hud_panel.h HP_HELP_COLS; tools/presets/convert.py HELP_COLS checks the file's) */
static const char g_flat_desc[] = "THE GAME AS IT WAS MIXED. NO CHANGE";
static const char g_custom_desc[] = "YOUR OWN SOUND: SET THE ROWS BELOW";
_Static_assert(sizeof(g_flat_desc) <= 36 + 1 && sizeof(g_custom_desc) <= 36 + 1,
	       "xq_desc: one help line (hud_panel.h HP_HELP_COLS)");

struct xq_preset {
	struct eqd_bands b;
	u32 id;
	char name[EQD_NAME + 1];
	char desc[EQD_DESC];
};

struct xq_custom {
	struct eqd_custom t;
	struct eqd_bands slot[XQ_CUSTOM_SLOTS];
	const struct eqd_bands *cur;    /* the current combination: a slot, or FLAT for 0 0 0 */
};

/* everything the EQ allocates, in one block (heap_bytes) */
struct xq_table {
	u32 count, crc, bytes;
	s32 custom_err;                 /* eqd_custom_read: 0 = CUSTOM offered */
	struct eqd_bands flat;          /* FLAT while loaded: no bands, the file's flat_q15 */
	const struct eqd_bands *flatp;  /* &flat, or NULL when that is 0 dB (pass-through) */
	struct eqd_run run[XQ_PADS];
	s32 work[2 * XQ_CHUNK];
	struct xq_custom *cust;         /* after p[], or NULL: no CUSTOM */
	struct xq_preset p[];           /* count */
};

/* the hand-over from the file worker (see the top) */
static struct {
	struct xq_table *tab;
	s32 result;
	u32 version;                    /* the file's header version, 0 if unread */
	volatile u32 done;
} g_job;

/* xbdm: the equalizer block ("XWEQ"), read and written by eq.py and accept/eq.py.
 * All u32, in this order: eq.py's FIELDS mirrors it. Append only. */
struct xq_status {
	u32 magic, version;
	/* controls (xbdm writes) */
	volatile u32 enable;   /* the gate: 0 off (default), 1 on */
	volatile u32 select;   /* write an index to select it (as the panel does); taken -> ~0 */
	volatile u32 reload;   /* write 1: read the file again (after eq.py install) */
	/* state */
	u32 preset, preset_id, count;
	u32 load_state, load_error, file_crc, flat_q15;
	u32 fade;              /* the last pad processed: 0 steady, 1 warming up, 2 fading */
	u32 clamps;            /* output samples clamped to s16 (expected 0 on a test tone) */
	u32 blocks, skipped;   /* earpiece blocks processed / passed through (rate, pad) */
	u32 heap_bytes, quiesced;
	/* version 2 */
	u32 file_version;      /* the last file's header version (1: refused as too old), 0 none */
	u32 custom_index;      /* xq_custom_index(): 0 not offered */
	u32 custom;            /* the steps, s8 each: BASS bits 0-7, MIDS 8-15, TREBLE 16-23 */
	volatile u32 custom_set; /* write XQ_CUSTOM_SET | packed steps: set all three; -> 0 */
	u32 custom_error;      /* s32: 0 offered, eqd_custom_read's -1..-3, XQ_C_NOFILE */
	/* version 3 */
	/* the EQ's own time in the last driver-loop pass (every xq_process of it, timebase),
	 * microseconds: bits 0-15 the last pass, 16-31 the longest since start (xbdm may write
	 * 0 to measure from now). The driver thread has about 1 ms per frame in all
	 * (docs/INTERNALS.md "the system window"): this is the EQ's share of it. */
	volatile u32 pass_us;
};
_Static_assert(sizeof(struct xq_status) == 24 * 4, "eq.py FIELDS");

__attribute__((used)) struct xq_status g_xq = {
	.magic = 0x58574551, /* "XWEQ" */
	.version = 3,
	.enable = 1, /* the equalizer is offered at boot (eq.py off switches it off) */
	.select = XQ_NO_SELECT,
	.flat_q15 = 32768,
	.custom_error = (u32)XQ_C_NOFILE,
};

static struct xq_table *g_tab; /* driver thread only, once taken from g_job */
static u32 g_sel;              /* selected index, 0 = FLAT, or XQ_SEL_CUSTOM */
static s32 g_cust[XQ_CUSTOM_BANDS]; /* CUSTOM's steps, kept with or without a file */
static u32 g_enabled;          /* the gate as applied */
#define XQ_DEFAULT_ID 0x0fe47caeu /* HALO MCC (tools/presets): selected at boot; FLAT if absent */
static u32 g_defaulted;
static u32 g_tries;
static u64 g_post_ms;
static u32 g_pass_tb;          /* xq_process's time since the last xq_poll (timebase) */

/* ---- file worker thread ---------------------------------------------------------------- */

XQ_SMALL static s32 check_code(int e)
{
	static const s8 map[] = { 0,	       XQ_E_SHORT, XQ_E_MAGIC, XQ_E_FORMAT,
				  XQ_E_LENGTH, XQ_E_CRC,   XQ_E_OLD };

	return e >= -6 ? map[-e] : XQ_E_LENGTH;
}

XQ_SMALL static struct xq_table *build(const u8 *f, u32 len, s32 *err)
{
	int n = eqd_check(f, len);
	char author[EQD_AUTHOR + 1];
	s16 gains[EQD_BANDS];

	if (n < 0) {
		*err = check_code(n);
		return NULL;
	}
	if (n > XQ_MAX_PRESETS) {
		*err = XQ_E_COUNT;
		return NULL;
	}
	/* room for CUSTOM only when the tables are there (a wrong one wastes it, harmlessly) */
	u32 at = (sizeof(struct xq_table) + (u32)n * sizeof(struct xq_preset) + 7) & ~7u;
	u32 has = len - EQD_HDR - (u32)n * EQD_REC == EQD_CUSTOM_BYTES;
	u32 bytes = at + (has ? sizeof(struct xq_custom) : 0);
	struct xq_table *t = malloc(bytes);
	if (!t) {
		*err = XQ_E_NOMEM;
		return NULL;
	}
	memset(t, 0, bytes);
	t->count = (u32)n;
	t->crc = eqd_be32(f + 24);
	t->bytes = bytes;
	for (u32 i = 0; i < t->count; i++) {
		struct xq_preset *p = &t->p[i];
		p->id = eqd_record(f, i, &p->b, p->name, author, p->desc, gains);
		/* xq_find and xw.settings rely on the id naming the preset */
		if (p->id == 0 || p->id != eqd_fnv1a(p->name)) {
			free(t);
			*err = XQ_E_RECORD;
			return NULL;
		}
	}
	t->flat.preamp_q15 = eqd_flat_q15(f);
	t->flatp = t->flat.preamp_q15 == 32768 ? NULL : &t->flat;
	if (has) {
		t->cust = (struct xq_custom *)((u8 *)t + at);
		t->custom_err = eqd_custom_read(f, len, t->count, t->flat.preamp_q15, &t->cust->t);
		if (t->custom_err)
			t->cust = NULL;
	} else { /* absent or the wrong size: the reason, read before ct would be touched */
		t->custom_err = eqd_custom_read(f, len, t->count, t->flat.preamp_q15, NULL);
	}
	*err = 0;
	return t;
}

XQ_SMALL static void load_job(void *arg)
{
	u8 *f = malloc(XQ_FILE_MAX);
	struct xq_table *t = NULL;
	u32 len = 0;
	s32 err;

	(void)arg;
	if (!f) {
		err = XQ_E_NOMEM;
	} else {
		err = xf_read(XF_DIR XQ_FILE, f, XQ_FILE_MAX, &len);
		g_job.version = err == XF_OK && len >= 8 ? eqd_be32(f + 4) : 0;
		if (err == XF_OK)
			t = build(f, len, &err);
		free(f);
	}
	g_job.tab = t;
	g_job.result = err;
	XQ_BARRIER(); /* table and result are visible before `done` */
	g_job.done = 1;
}

/* ---- driver thread ---------------------------------------------------------------------- */

XQ_SMALL static void post_load(void)
{
	g_post_ms = xa_now_ms();
	if (xf_post(load_job, NULL)) {
		g_xq.load_state = XQ_LOAD_POSTED;
		return;
	}
	if (++g_tries >= XQ_POST_TRIES) {
		g_xq.load_state = XQ_LOAD_NO_WORKER;
		g_xq.load_error = (u32)XF_E_NOWORKER;
		xa_log("eq: no file worker, presets not loaded (FLAT only)\n");
	}
}

XQ_SMALL void xq_init(void)
{
	g_tries = 0;
	g_xq.load_state = XQ_LOAD_IDLE;
	post_load();
}

static const struct eqd_bands *target(const struct xq_table *t)
{
	if (g_sel == XQ_SEL_CUSTOM)
		return t->cust ? t->cust->cur : t->flatp;
	return g_sel ? &t->p[g_sel - 1].b : t->flatp;
}

/* CUSTOM's current combination into a slot no stream plays or fades in (see the top) */
XQ_SMALL static void custom_build(struct xq_table *t)
{
	struct xq_custom *c = t->cust;

	if (!c)
		return;
	if (!g_cust[0] && !g_cust[1] && !g_cust[2]) {
		c->cur = t->flatp; /* FLAT itself */
		return;
	}
	for (int s = 0; s < XQ_CUSTOM_SLOTS; s++) {
		const struct eqd_bands *b = &c->slot[s];
		int busy = b == c->cur;

		for (int p = 0; p < XQ_PADS && !busy; p++)
			busy = t->run[p].pc == b || (t->run[p].fading && t->run[p].pn == b);
		if (busy)
			continue;
		eqd_custom_bands(&c->t, g_cust[0], g_cust[1], g_cust[2], &c->slot[s]);
		c->cur = b;
		return;
	}
	/* unreachable (more slots than a stream can hold): the previous combination plays on */
}

/* every pad back to FLAT at 0 dB with no history: the next block fades in what is selected */
static void reset_runs(struct xq_table *t)
{
	memset(t->run, 0, sizeof(t->run));
}

XQ_SMALL static void take_job(void)
{
	struct xq_table *t;
	s32 err;

	XQ_BARRIER(); /* pairs with load_job's: `done` was seen, so the rest is */
	t = g_job.tab;
	err = g_job.result;
	g_job.tab = NULL;
	g_job.done = 0;
	g_xq.file_version = g_job.version;
	if (!t) {
		g_xq.load_state = XQ_LOAD_FAILED;
		g_xq.load_error = (u32)err;
		xa_log("eq: " XQ_FILE " not loaded (error %d%s): FLAT only\n", (int)err,
		       err == XQ_E_OLD ? ", an older file: eq.py install" : "");
		return;
	}
	if (g_tab)
		free(g_tab); /* a reload: nothing else holds it (driver thread only) */
	g_tab = t;
	if (g_sel == XQ_SEL_CUSTOM ? !t->cust : g_sel > t->count)
		g_sel = 0;
	custom_build(t);
	if (!g_defaulted) { /* the first table of this boot: the preset a new player starts on */
		g_defaulted = 1;
		for (u32 i = 0; !g_sel && i < t->count; i++)
			if (t->p[i].id == XQ_DEFAULT_ID)
				g_sel = i + 1;
	}
	g_xq.load_state = XQ_LOAD_OK;
	g_xq.load_error = 0;
	g_xq.file_crc = t->crc;
	g_xq.flat_q15 = (u32)t->flat.preamp_q15;
	g_xq.heap_bytes = t->bytes;
	g_xq.custom_error = (u32)t->custom_err;
	xa_log("eq: %u presets loaded, CUSTOM %s (%d), FLAT at %d/32768\n", t->count,
	       t->cust ? "offered" : "not offered", (int)t->custom_err, (int)t->flat.preamp_q15);
}

/* once a pass (xq_poll runs once per driver-loop pass, after audio_poll's earpiece fills):
 * the EQ's time in that pass into pass_us */
XQ_SMALL static void publish_time(void)
{
	u32 us = g_pass_tb / XQ_TB_PER_US, max = g_xq.pass_us >> 16;

	g_pass_tb = 0;
	us = us > 0xFFFF ? 0xFFFF : us;
	g_xq.pass_us = (us > max ? us : max) << 16 | us;
}

XQ_SMALL void xq_poll(void)
{
	publish_time();
	if (g_xq.quiesced)
		return;
	if (g_job.done)
		take_job();
	if (g_xq.reload) {
		g_xq.reload = 0;
		if (g_xq.load_state != XQ_LOAD_POSTED) {
			g_tries = 0;
			post_load();
		}
	}
	if (g_xq.load_state == XQ_LOAD_IDLE && xa_now_ms() - g_post_ms >= XQ_POST_RETRY_MS)
		post_load();
	if (g_xq.custom_set & XQ_CUSTOM_SET) {
		u32 v = g_xq.custom_set;

		g_xq.custom_set = 0;
		for (u32 k = 0; k < XQ_CUSTOM_BANDS; k++)
			xq_set_custom(k, (s8)(v >> (8 * k)));
	}
	if (g_xq.select != XQ_NO_SELECT) {
		xq_set_preset(g_xq.select);
		g_xq.select = XQ_NO_SELECT;
	}
	u32 en = g_xq.enable ? 1 : 0;
	if (g_xq.enable != en)
		g_xq.enable = en;
	if (en != g_enabled) {
		g_enabled = en;
		if (g_tab)
			reset_runs(g_tab);
		xa_log("eq: %s\n", en ? "on" : "off");
	}
	g_xq.count = xq_count();
	g_xq.preset = xq_preset();
	g_xq.preset_id = xq_preset_id(g_xq.preset);
	g_xq.custom_index = xq_custom_index();
	g_xq.custom = (u32)(u8)g_cust[0] | (u32)(u8)g_cust[1] << 8 | (u32)(u8)g_cust[2] << 16;
	if (!g_tab)
		g_xq.custom_error = (u32)XQ_C_NOFILE;
}

void xq_quiesce(void)
{
	g_xq.quiesced = 1;
}

void xq_resume(void)
{
	g_xq.quiesced = 0;
}

/* the earpiece is s16 little-endian; the kernel works on native s16 */
XQ_SMALL static void to_native(s16 *s, int n)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	u16 *u = (u16 *)s;
	for (int i = 0; i < n; i++)
		u[i] = __builtin_bswap16(u[i]);
#else
	(void)s;
	(void)n;
#endif
}

XQ_SMALL void xq_process(int pad, u8 *out, int len, u32 rate_out, u32 ch_out)
{
	struct xq_table *t = g_tab;

	if (g_xq.quiesced || !g_enabled || !t || len <= 0)
		return;
	if (pad < 0 || pad >= XQ_PADS || rate_out != XQ_RATE || (ch_out != 1 && ch_out != 2) ||
	    ((uintptr_t)out & 1)) {
		g_xq.skipped++;
		return;
	}
	struct eqd_run *r = &t->run[pad];
	const struct eqd_bands *want = target(t);
	int ch = (int)ch_out, frames = len / (2 * ch);
	u32 tb0 = XQ_TB();

	if (r->want != want)
		eqd_request(r, want);
	g_xq.blocks++;
	if (!r->fading && r->want == r->pc && !r->pc) {
		g_xq.fade = 0;
		return; /* FLAT at 0 dB: the samples go out untouched */
	}
	/* chunk by chunk, as eqd_block decides fades at chunk starts: a steady stereo chunk takes
	 * the fast path in the earpiece's byte order, anything else (a fade, mono) the general
	 * one between two byte swaps. Both give the same samples. */
	for (int i = 0; i < frames; i += XQ_CHUNK) {
		int n = frames - i < XQ_CHUNK ? frames - i : XQ_CHUNK;
		u8 *blk = out + i * 2 * ch;

		if (ch == 2 && eqd_steady(r)) {
			g_xq.clamps += eqd_steady_le2(r->pc, r->cur, blk, n, t->work);
		} else {
			to_native((s16 *)blk, n * ch);
			g_xq.clamps += eqd_block(r, (s16 *)blk, n, ch, t->work);
			to_native((s16 *)blk, n * ch);
		}
	}
	g_xq.fade = !r->fading ? 0 : r->xpos < 0 ? 1 : 2;
	g_pass_tb += XQ_TB() - tb0;
}

u32 xq_count(void)
{
	return g_enabled && g_tab ? g_tab->count : 0;
}

u32 xq_preset(void)
{
	if (!xq_count())
		return 0;
	return g_sel == XQ_SEL_CUSTOM ? xq_custom_index() : g_sel;
}

void xq_set_preset(u32 idx)
{
	if (idx && idx == xq_custom_index())
		g_sel = XQ_SEL_CUSTOM;
	else
		g_sel = idx <= xq_count() ? idx : 0;
}

XQ_SMALL u32 xq_preset_id(u32 idx)
{
	return idx && idx <= xq_count() ? g_tab->p[idx - 1].id : 0;
}

XQ_SMALL u32 xq_find(u32 id)
{
	for (u32 i = 1; id && i <= xq_count(); i++)
		if (g_tab->p[i - 1].id == id)
			return i;
	return 0;
}

XQ_SMALL const char *xq_name(u32 idx)
{
	if (idx && idx == xq_custom_index())
		return "CUSTOM";
	return idx && idx <= xq_count() ? g_tab->p[idx - 1].name : "FLAT";
}

XQ_SMALL const char *xq_desc(u32 idx)
{
	if (!idx)
		return g_flat_desc;
	if (idx == xq_custom_index())
		return g_custom_desc;
	return idx <= xq_count() ? g_tab->p[idx - 1].desc : "";
}

bool xq_custom_offered(void)
{
	return xq_count() && g_tab->cust;
}

u32 xq_custom_index(void)
{
	return xq_custom_offered() ? g_tab->count + 1 : 0;
}

int xq_custom(u32 band)
{
	return band < XQ_CUSTOM_BANDS ? (int)g_cust[band] : 0;
}

XQ_SMALL void xq_set_custom(u32 band, int step)
{
	if (band >= XQ_CUSTOM_BANDS)
		return;
	step = step < XQ_CUSTOM_MIN ? XQ_CUSTOM_MIN : step > XQ_CUSTOM_MAX ? XQ_CUSTOM_MAX : step;
	if (g_cust[band] == step)
		return; /* a held direction at the end: no new cascade, no fade */
	g_cust[band] = step;
	if (g_tab)
		custom_build(g_tab);
}
