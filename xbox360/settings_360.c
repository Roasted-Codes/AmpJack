// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Per-profile settings (settings_360.h): the panel's settings of whoever is signed in on the
 * headset's pad, kept in Hdd:\Plugins\xw.settings and put back at boot, when a headset is
 * bound, and when that pad's profile changes. No setup and no save button: a change is saved
 * 2 s after the last step.
 *
 * Threads:
 *   - Driver thread: xs_init, xs_poll, xs_quiesce, xs_resume, and everything that reads or
 *     writes a setting (through the owners' getters and setters, as the panel does, so their
 *     validation stays in one place). No feature calls in here.
 *   - File worker (file_360.h): load_job, save_job (the disk) and signin_job (XAM). A job's
 *     result is handed back as in eq_360.c: fields, lwsync, then `done`; the driver thread
 *     reads `done`, lwsync, then the fields. No lock: nothing may block the driver thread.
 *   - g_mem (the file image, malloc'd) belongs to the worker while a load or save job is out
 *     (g_io_busy) and to the driver thread otherwise; during a save the driver thread may
 *     still read it (both only read), never write it.
 *
 * Gate: XWST `enable`, off by default. Off: nothing is read, written or applied, and XAM is
 * not asked. Switching it on is like a boot: the file is loaded once and the active profile
 * restored.
 *
 * Who is "the active profile": the offline XUID signed in on the headset's pad, asked of XAM
 * on the file worker about once a second while a headset is there. Never on the driver
 * thread: a sign-in in progress can hold XAM's user lock. Not on xinput_360.c's XAM thread
 * either, although every other XAM call is made there: that thread carries the Guide button
 * and the binds, and a wait on the user lock would delay them, where on the worker it only
 * delays a save. The question is one read-only export (XamUserGetSigninInfo), not a
 * notification, so PR #6's process-type finding (XNotify needs the driver thread's process
 * type) does not apply to it; whether 17559's XAM answers a system thread is UNVERIFIED (the
 * XW_DEV log shows each answer). Nobody signed in, no answer, or a pad XAM
 * gave no user index: XUID 0, one shared record. Without a headset the profile stays as it is
 * (XUID 0 at boot).
 */
#include <stdlib.h>

#include "../core/platform.h"
#include "diag_360.h"
#include "eq_360.h"
#include "file_360.h"
#include "game_360.h"
#include "kernel_360.h"
#include "panel_360.h"
#include "settings_360.h"
#include "team_360.h"
#include "xinput_360.h"

#ifndef XS_HOST_TEST
#include <xecore/xboxkrnl.h>
#endif

/* Built for size, the whole file and settings_tab.h: it runs every 250 ms at most (a
 * snapshot is ten getter calls), a save a few times a minute, and clang's -O2 inlining and
 * unrolling of it came to about twice the SETTINGS image budget. A pass of xs_poll stays a
 * few microseconds either way. */
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#include "settings_tab.h"

#define XS_PATH XF_DIR XS_FILE
#define XS_SNAP_MS 250      /* how often the settings are compared with the last look */
#define XS_SAVE_MS 2000     /* saved this long after the last change */
#define XS_PROFILE_MS 1000  /* who is signed in: asked this often while a headset is there */
#define XS_RETRY_MS 5000    /* after a refused post or a failed write */
#define XS_QUIESCE_MS 1000  /* xs_quiesce's longest wait for a write */
#define XS_TB_PER_MS 50000  /* the timebase is 50 MHz (diag_360.h, tb_per_ms) */
#define XS_NO_SLOT 0xFFFFFFFFu
#define XS_NO_USER 0xFFu

_Static_assert(sizeof(struct xs_file_header) == XS_HDR && sizeof(struct xs_record) == XS_REC,
	       "settings_360.h describes this layout");

/* XWST state */
enum { XS_ST_OFF, XS_ST_LOADING, XS_ST_READY, XS_ST_NO_WORKER, XS_ST_NO_MEMORY, XS_ST_QUIESCED };
/* XWST last_error: file_360.h's XF_E_* (-1..-4), or these */
enum { XS_E_NOMEM = -23, XS_E_POST = -24, XS_E_NOXAM = -25 }; /* -21, -22: settings_tab.h */

/* xbdm: the settings block ("XWST"), read and written by settings.py and accept/settings.py.
 * All u32, in this order: settings.py's FIELDS mirrors it. Append only (24 words at most). */
struct xs_status {
	u32 magic, version;
	/* controls (xbdm writes) */
	volatile u32 enable;  /* the gate: 0 off (default), 1 on */
	volatile u32 wipe;    /* write 1: forget every profile (the file is rewritten empty) */
	volatile u32 probe;   /* XW_DEV: write 1 to write the file now, unchanged (timed) */
	/* state */
	u32 state;            /* XS_ST_* */
	u32 xuid_hi, xuid_lo; /* the active profile (0 = nobody signed in) */
	u32 slot;             /* its record, or XS_NO_SLOT (no record yet: the defaults) */
	u32 user;             /* the headset pad's XAM user index (bits 0-7, 0xFF none) and the
			       * sign-in state XAM gave last (bits 8-15: 0 none, 1 local, 2 Live) */
	u32 inplace;          /* writes where the rename was refused (file_360.h XF_NEW_SUFFIX) */
	u32 records, loads, saves;
	u32 save_ms_last, save_ms_max;  /* a write on the worker, start to end */
	u32 errors, crc_rejects;
	u32 last_error;       /* s32: XF_E_* or XS_E_* */
	u32 heap_bytes, restores;
	u32 xam_status;       /* XAM's last answer that was neither success nor "no such user" */
	u32 save_starved;     /* XWDG out_starved and out_frames_missed during the last write: */
	u32 save_missed;      /* the earpiece must not notice a save */
};
_Static_assert(sizeof(struct xs_status) == 24 * 4, "settings.py FIELDS");

__attribute__((used)) struct xs_status g_xs = {
	.magic = XS_MAGIC,
	.version = 1,
	.slot = XS_NO_SLOT,
	.user = XS_NO_USER | XS_NO_USER << 8,
};

/* ---- the worker's jobs ------------------------------------------------------------------- */

enum { IO_LOAD = 1, IO_SAVE };
static u8 *g_mem;                     /* the file image, XS_FILE_BYTES */
static struct {
	volatile u32 done;
	u32 kind, rejects, ms;
	s32 result;
} g_io;
static struct {
	volatile u32 done;
	u32 state;
	s32 status;
	u64 xuid;
} g_sj;

static void load_job(void *arg)
{
	u32 len = 0, rej = 0;
	int e = xf_read(XS_PATH, g_mem, XS_FILE_BYTES, &len);
	int c = e == XF_OK ? table_check(g_mem, len, &rej) : e;

	(void)arg;
	g_io.rejects = rej;
	if (c != 0) {
		/* a cut during the rename fallback leaves the temporary copy whole */
		int e2 = xf_read(XS_PATH XF_NEW_SUFFIX, g_mem, XS_FILE_BYTES, &len);

		if (e2 == XF_OK && table_check(g_mem, len, &rej) == 0) {
			g_io.rejects += rej;
			c = 0;
		} else {
			table_init(g_mem);
			if (c == XF_E_NOFILE && e2 == XF_E_NOFILE)
				c = 0; /* never saved: no error */
		}
	}
	g_io.result = c;
	g_io.kind = IO_LOAD;
	BARRIER();
	g_io.done = 1;
}

static void save_job(void *arg)
{
	u32 tb = xd_timebase();

	(void)arg;
	g_io.result = xf_write(XS_PATH, g_mem, XS_FILE_BYTES);
	g_io.ms = (xd_timebase() - tb) / XS_TB_PER_MS;
	g_io.kind = IO_SAVE;
	BARRIER();
	g_io.done = 1;
}

#ifndef XS_HOST_TEST
/* XamUserGetSigninInfo (xam 551 = 0x227: Xenia's export table, which matches this console's
 * 402 / 656 / 685; research/dumps/xam_815f0000.bin is the image only, without the XEX export
 * table, so the ordinal is not proven on 17559): (user index, flags, XUSER_SIGNIN_INFO *) ->
 * 0 or "no such user". The info is 40 bytes (Xenia's X_USER_SIGNIN_INFO): u64 xuid, u32,
 * u32 state (0 none, 1 local, 2 Live), ... Flag 2 (the SDK's XUSER_GET_SIGNIN_INFO_OFFLINE_
 * XUID_ONLY, UNVERIFIED) asks for the offline XUID, which every profile has and which does
 * not change when the profile goes on or off Live; if the flag means something else, a
 * signed-in player may read as XUID 0 and shares the "nobody" record: wrong, never a hang.
 * Resolved on the worker, once. */
#define ORD_XamUserGetSigninInfo 551
#define SIGNIN_OFFLINE_XUID 2
typedef u32 (*signin_info_t)(u32 user, u32 flags, void *info);
static signin_info_t g_signin_fn;
static u32 g_signin_tried;

static s32 xam_signin(u32 user, u64 *xuid, u32 *state)
{
	u8 info[64] __attribute__((aligned(8))); /* 40 used; room if a build writes more */
	u32 st;

	if (!g_signin_tried) {
		HANDLE xam;
		void *fn = 0;

		g_signin_tried = 1;
		if (XexGetModuleHandle("xam.xex", &xam) >= 0)
			XexGetProcedureAddress(xam, ORD_XamUserGetSigninInfo, &fn);
		g_signin_fn = (signin_info_t)fn;
	}
	if (!g_signin_fn)
		return XS_E_NOXAM;
	memset(info, 0, sizeof(info));
	st = g_signin_fn(user, SIGNIN_OFFLINE_XUID, info);
	*xuid = be64(info);
	*state = eqd_be32(info + 12);
	return (s32)st;
}
#else
s32 xam_signin(u32 user, u64 *xuid, u32 *state); /* the test's */
#endif

static void signin_job(void *arg)
{
	u64 xuid = 0;
	u32 state = 0;

	g_sj.status = xam_signin((u32)(uintptr_t)arg, &xuid, &state);
	g_sj.xuid = xuid;
	g_sj.state = state;
	BARRIER();
	g_sj.done = 1;
}

/* ---- driver thread ---------------------------------------------------------------------- */

/* "nobody signed in on that index": Win32 ERROR_NO_SUCH_USER, or the same as an HRESULT
 * (XAM's X_E_NO_SUCH_USER); which one 17559's XAM returns is not known offline */
#define XS_NO_SUCH_USER 0x525
#define XS_NO_SUCH_USER_HR 0x80070525u
static bool no_such_user(s32 st)
{
	return st == XS_NO_SUCH_USER || (u32)st == XS_NO_SUCH_USER_HR;
}

static struct xs_vals g_defaults;  /* the compiled defaults, read at xs_init */
static struct xs_vals g_saved;     /* the active profile's record (or the defaults) */
static struct xs_vals g_last;      /* the settings at the last look */
static u64 g_active;               /* the active profile's XUID */
static u64 g_want;                 /* the profile asked for (g_want_set) */
/* Milliseconds as u32, compared only as differences (wrap-safe for the seconds involved):
 * 64-bit compares cost several instructions each on this 32-bit build. */
static u32 g_snap_ms, g_change_ms, g_profile_ms;
static u32 g_retry_ms;              /* no load or save is posted before this */
static u32 g_inited, g_quiesced, g_loaded, g_restored, g_dirty, g_need_write, g_want_set;
static u32 g_io_busy, g_sj_busy;
static int g_hs_last = -2;         /* the headset pad at the last look (-2: never looked) */
static u32 g_preset_wait;          /* the preset to select is not settled (no presets yet) */
static u32 g_preset_want;          /* its id */
static u32 g_custom_want;          /* or CUSTOM (XS_F_CUSTOM), which has no id */
static u32 g_starved0, g_missed0;
#ifdef XW_DEV
static u64 g_dev_xuid;
static u32 g_dev_state = 0xFFFFFFFFu;
#endif

#ifdef XS_HOST_TEST
/* tests/test_settings.c: a new boot (every static above, back to how the image starts) */
static void xs_host_reset(void)
{
	free(g_mem);
	g_mem = NULL;
	memset(&g_io, 0, sizeof(g_io));
	memset(&g_sj, 0, sizeof(g_sj));
	memset(&g_defaults, 0, sizeof(g_defaults));
	memset(&g_saved, 0, sizeof(g_saved));
	memset(&g_last, 0, sizeof(g_last));
	g_active = g_want = g_snap_ms = g_change_ms = g_profile_ms = g_retry_ms = 0;
	g_inited = g_quiesced = g_loaded = g_restored = g_dirty = g_need_write = g_want_set = 0;
	g_io_busy = g_sj_busy = g_preset_wait = g_preset_want = g_starved0 = g_missed0 = 0;
	g_custom_want = 0;
	g_hs_last = -2;
	memset(&g_xs, 0, sizeof(g_xs));
	g_xs.magic = XS_MAGIC;
	g_xs.version = 1;
	g_xs.slot = XS_NO_SLOT;
	g_xs.user = XS_NO_USER | XS_NO_USER << 8;
}
#endif

/* `at` has come (u32 milliseconds that may wrap) */
static bool due(u32 now, u32 at)
{
	return (s32)(now - at) >= 0;
}

static void note_error(s32 e)
{
	g_xs.errors++;
	g_xs.last_error = (u32)e;
}

/* The preset can only be selected once the EQ has its presets: the file loads in the
 * background on the file worker, and xq_count() is 0 until it has and while the XWEQ gate is
 * off. Until then the profile's preset id (or CUSTOM) is kept as it is: snapshot reports it,
 * so another change saves it unchanged, never as FLAT, and it is selected on the first
 * snapshot (every 250 ms) that sees xq_count() above 0. eq_360.h has no "the load failed"
 * getter, so a failed load looks the same as a slow one: the id is kept (a missing preset
 * file for one boot must not cost the player his preset), at the price of one xq_count()
 * call per snapshot. With no presets the panel offers no PRESET row, so nothing else can
 * change it meanwhile. CUSTOM's steps need no wait: eq_360.c keeps them without a file
 * (apply sets them at once). *custom: CUSTOM is (or is to be) selected. */
static bool custom_selected(void)
{
	u32 sel = xq_preset();

	return sel && sel == xq_custom_index();
}

static u32 preset_now(u32 *custom)
{
	if (!xq_count()) {
		g_preset_wait = 1;
		*custom = g_custom_want;
		return g_preset_want;
	}
	if (g_preset_wait) {
		g_preset_wait = 0;
		/* CUSTOM in a file without its tables, or an id the file lacks: FLAT */
		xq_set_preset(g_custom_want ? xq_custom_index() : xq_find(g_preset_want));
		/* settling is not a change, and a choice that became FLAT is not saved by itself */
		g_last.preset_id = g_saved.preset_id = xq_preset_id(xq_preset());
		g_last.flags &= ~XS_F_CUSTOM;
		g_saved.flags &= ~XS_F_CUSTOM;
		if (custom_selected()) {
			g_last.flags |= XS_F_CUSTOM;
			g_saved.flags |= XS_F_CUSTOM;
		}
	}
	*custom = g_custom_want = custom_selected();
	return g_preset_want = xq_preset_id(xq_preset());
}

static void snapshot(struct xs_vals *v)
{
	u32 custom;

	memset(v, 0, sizeof(*v));
	v->balance = xg_balance();
	v->volume = xg_master() / XG_MASTER_STEP;
	v->sidetone = xg_sidetone();
	v->mic_level = xd_mic_level(g_xd.mic_gain);
	v->gate = g_xd.gate < XD_GATES ? g_xd.gate : XD_GATE_DEFAULT;
	v->surround = xg_surround() ? 1 : 0;
	v->preset_id = preset_now(&custom);
	v->flags = (xd_mic_auto() ? XS_F_MIC_AUTO : 0) | (custom ? XS_F_CUSTOM : 0);
	for (u32 k = 0; k < XQ_CUSTOM_BANDS; k++)
		v->custom[k] = xq_custom(k);
	v->team_level = xt_level();
	v->team_channel = xt_channel();
}

/* through the setters the panel uses (panel_360.c row_adjust), then xd_poll as it does */
static void apply(const struct xs_vals *v)
{
	xg_adjust(v->balance - xg_balance(), (int)v->volume - (int)(xg_master() / XG_MASTER_STEP),
		  (int)v->sidetone - (int)xg_sidetone());
	g_xd.mic_gain = xd_mic_gain_step(XD_MIC_GAIN_MIN, (int)v->mic_level - 1);
	g_xd.gate = v->gate;
	xd_set_mic_auto(v->flags & XS_F_MIC_AUTO);
	xg_set_surround(v->surround);
	xt_set_level(v->team_level);
	xt_set_channel(v->team_channel);
	for (u32 k = 0; k < XQ_CUSTOM_BANDS; k++)
		xq_set_custom(k, v->custom[k]);
	g_preset_want = v->preset_id;
	g_custom_want = v->flags & XS_F_CUSTOM ? 1 : 0;
	g_preset_wait = 1;
	xd_poll();
}

static void restore(u64 xuid)
{
	int slot = table_find(g_mem, xuid);
	struct xs_vals v = g_defaults;

	if (slot >= 0)
		rec_get(rec_at(g_mem, (u32)slot), &v);
	g_active = xuid;
	apply(&v);
	g_saved = v;
	snapshot(&g_last);
	g_dirty = 0;
	g_restored = 1;
	g_xs.restores++;
	g_xs.xuid_hi = (u32)(xuid >> 32);
	g_xs.xuid_lo = (u32)xuid;
	g_xs.slot = slot >= 0 ? (u32)slot : XS_NO_SLOT;
	xa_log("settings: profile %08x %s\n", (u32)xuid,
	       slot >= 0 ? "restored" : "has no record: defaults");
}

/* the settings now into the active profile's record, if they differ from it (driver thread,
 * g_mem not out with the worker) */
static void commit(void)
{
	u32 slot;

	if (!memcmp(&g_last, &g_saved, sizeof(g_saved)))
		return;
	slot = table_slot_for(g_mem, g_active);
	rec_put(rec_at(g_mem, slot), g_active, table_max_seq(g_mem) + 1, &g_last);
	hdr_put(g_mem);
	g_saved = g_last;
	g_need_write = 1;
	g_xs.slot = slot;
	g_xs.records = eqd_be32(g_mem + 8) >> 16;
}

static bool post_io(void (*job)(void *))
{
	if (!xf_post(job, NULL)) {
		note_error(XS_E_POST);
		g_retry_ms = (u32)xa_now_ms() + XS_RETRY_MS;
		return false;
	}
	g_io_busy = 1;
	return true;
}

static void post_save(void)
{
	g_starved0 = g_xd.out_starved;
	g_missed0 = g_xd.out_frames_missed;
	if (post_io(save_job))
		g_need_write = 0;
}

static void take_io(void)
{
	s32 r;

	if (!g_io.done)
		return;
	BARRIER(); /* pairs with the job's: `done` was seen, so the rest is */
	g_io.done = 0;
	g_io_busy = 0;
	r = g_io.result;
	if (g_io.kind == IO_LOAD) {
		g_loaded = 1;
		g_xs.loads++;
		g_xs.crc_rejects += g_io.rejects;
		g_xs.records = eqd_be32(g_mem + 8) >> 16;
		if (r)
			note_error(r);
		xa_log("settings: " XS_FILE " loaded: %u profiles, %u rejected, error %d\n",
		       g_xs.records, g_io.rejects, (int)r);
		return;
	}
	g_xs.inplace = xf_inplace_writes();
	g_xs.save_starved = g_xd.out_starved - g_starved0;
	g_xs.save_missed = g_xd.out_frames_missed - g_missed0;
	if (r) {
		note_error(r);
		g_need_write = 1;
		g_retry_ms = (u32)xa_now_ms() + XS_RETRY_MS;
		xa_log("settings: " XS_FILE " not written (error %d)\n", (int)r);
		return;
	}
	g_xs.saves++;
	g_xs.save_ms_last = g_io.ms;
	if (g_io.ms > g_xs.save_ms_max)
		g_xs.save_ms_max = g_io.ms;
}

static void take_signin(void)
{
	s32 st;
	u64 xuid;

	if (!g_sj.done)
		return;
	BARRIER();
	g_sj.done = 0;
	g_sj_busy = 0;
	st = g_sj.status;
	xuid = g_sj.xuid;
	g_xs.user = (g_xs.user & 0xFF) | (g_sj.state & 0xFF) << 8;
	if (st != 0 && !no_such_user(st))
		g_xs.xam_status = (u32)st;
	if (st != 0 || !g_sj.state || xuid == XS_XUID_EMPTY)
		xuid = XS_XUID_NOBODY; /* no answer, or nobody signed in: the shared record */
#ifdef XW_DEV
	if (xuid != g_dev_xuid || g_sj.state != g_dev_state) {
		xa_log("settings: user %u: sign-in %u, xuid %08x%08x (XAM %08x)\n",
		       g_xs.user & 0xFF, g_sj.state, (u32)(g_sj.xuid >> 32), (u32)g_sj.xuid, (u32)st);
		g_dev_xuid = xuid;
		g_dev_state = g_sj.state;
	}
#endif
	g_want = xuid;
	g_want_set = 1;
}

/* who should be active: the headset's pad's profile (asked of XAM on the worker) */
static void profile_poll(u32 now)
{
	int hs = xw_headset_pad(), user;

	if (g_sj_busy || (hs == g_hs_last && now - g_profile_ms < XS_PROFILE_MS))
		return;
	g_profile_ms = now;
	g_hs_last = hs;
	if (hs < 0) {
		g_xs.user = XS_NO_USER | XS_NO_USER << 8;
		if (!g_restored) {
			g_want = XS_XUID_NOBODY; /* at boot the pad is dark: the shared record */
			g_want_set = 1;
		}
		return;
	}
	user = xa_xinput_user(hs);
	g_xs.user = (g_xs.user & ~0xFFu) | (user < 0 ? XS_NO_USER : (u32)user);
	if (user < 0) {
		g_want = XS_XUID_NOBODY;
		g_want_set = 1;
	} else if (xf_post(signin_job, (void *)(uintptr_t)user)) {
		g_sj_busy = 1;
	} else {
		note_error(XS_E_POST);
	}
}

/* switch to g_want once the old profile's last change is in RAM */
static void profile_switch(void)
{
	if (!g_want_set)
		return;
	if (g_restored && g_want == g_active) {
		g_want_set = 0;
		return;
	}
	if (g_restored) {
		snapshot(&g_last);
		if (memcmp(&g_last, &g_saved, sizeof(g_saved))) {
			if (g_io_busy)
				return; /* the image is being written: next pass */
			commit();
		}
	}
	g_want_set = 0;
	restore(g_want);
}

void xs_init(void)
{
	g_inited = 1;
	g_retry_ms = (u32)xa_now_ms(); /* nothing to wait for (0 is not "long ago" after 24 days) */
	snapshot(&g_defaults); /* before anything is restored: the compiled defaults */
}

void xs_poll(void)
{
	u32 now;

	if (!g_inited || g_quiesced)
		return;
	take_io();
	take_signin();
	if (!g_xs.enable) {
		g_xs.state = XS_ST_OFF;
		g_restored = 0; /* switched on again: restored again, like a boot */
		g_dirty = 0;
		g_want_set = 0;
		g_hs_last = -2;
		return;
	}
	g_xs.enable = 1;
	now = (u32)xa_now_ms();
	if (!g_mem) {
		g_mem = malloc(XS_FILE_BYTES);
		if (!g_mem) {
			if (g_xs.state != XS_ST_NO_MEMORY)
				note_error(XS_E_NOMEM);
			g_xs.state = XS_ST_NO_MEMORY;
			return;
		}
		g_xs.heap_bytes = XS_FILE_BYTES;
	}
	if (!g_loaded) {
		if (!g_io_busy && due(now, g_retry_ms))
			g_xs.state = post_io(load_job) ? XS_ST_LOADING : XS_ST_NO_WORKER;
		return;
	}
	g_xs.state = XS_ST_READY;
	if (g_xs.wipe && !g_io_busy) {
		g_xs.wipe = 0;
		table_init(g_mem);
		g_need_write = 1;
		g_xs.records = 0;
		xa_log("settings: every profile forgotten\n");
		if (g_restored)
			restore(g_active); /* no record now: the defaults */
	}
#ifdef XW_DEV
	if (g_xs.probe && !g_io_busy) {
		g_xs.probe = 0;
		g_need_write = 1; /* the same bytes again, timed (save_ms_*, save_starved) */
	}
#endif
	profile_poll(now);
	profile_switch();
	if (!g_restored)
		return; /* nothing is the active profile's yet */
	if (now - g_snap_ms >= XS_SNAP_MS) {
		struct xs_vals v;

		g_snap_ms = now;
		snapshot(&v);
		if (memcmp(&v, &g_last, sizeof(v))) {
			g_last = v;
			g_change_ms = now;
			g_dirty = 1;
		}
	}
	if (g_dirty && now - g_change_ms >= XS_SAVE_MS && !g_io_busy) {
		g_dirty = 0;
		commit();
	}
	if (g_need_write && !g_io_busy && due(now, g_retry_ms))
		post_save();
}

/* a load / save (and a sign-in question) still out with the worker, for at most
 * XS_QUIESCE_MS: xf_quiesce, which follows, drops whatever is still queued */
static void wait_jobs(void)
{
	for (u32 ms = 0; (g_io_busy || g_sj_busy) && ms < XS_QUIESCE_MS; ms += 5) {
		xa_sleep_ms(5);
		take_io();
		take_signin();
	}
}

void xs_quiesce(void)
{
	if (!g_inited || g_quiesced)
		return;
	wait_jobs();
	if (g_xs.enable && g_loaded && g_restored && !g_io_busy) {
		snapshot(&g_last);
		commit();
		if (g_need_write)
			post_save();
		wait_jobs();
	}
	g_quiesced = 1;
	g_xs.state = XS_ST_QUIESCED;
	xa_log("settings: quiesced%s\n", g_io_busy ? " (a write is STILL running)" : "");
}

/* A sign-in question still out was perhaps dropped by xf_quiesce (it never answers): ask
 * again. Harmless if it does answer later: the answer is taken like any other. A load or save
 * still out is left alone: the worker may still be using g_mem, and only its `done` may hand
 * g_mem back. */
void xs_resume(void)
{
	g_sj_busy = 0;
	g_quiesced = 0;
}

#pragma clang attribute pop
