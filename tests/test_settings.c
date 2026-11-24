// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of per-profile settings (xbox360/settings_360.c, compiled in whole) with a fake
 * file worker and file system (file_360.h), fake feature getters / setters (game_360.h,
 * diag_360.h, eq_360.h, team_360.h), a fake sign-in answer (XAM) and a fake clock.
 *
 *   cc -O2 -Wall -o test_settings tests/test_settings.c && ./test_settings
 *   XS_DUMP=/tmp/x.settings ./test_settings && python3 xbox360/settings.py dump --file /tmp/x.settings
 *
 * Checks: the gate (off: no file, no XAM), record round trip through a "reboot", the bytes of
 * the file (big-endian, zlib CRC), CRC reject, torn / truncated file, wrong version, the .new
 * copy, the 65th profile evicting the oldest, an unknown preset id loading as FLAT, the
 * preset waiting for the EQ's file, CUSTOM (selected and its steps saved, steps put back at
 * once, the selection waiting for the file, FLAT when the file has no CUSTOM, an older record
 * reading as CUSTOM 0 0 0 unselected), profile switch (own values, defaults for a new profile,
 * the old profile's last change kept), debounce timing, no write when nothing changed,
 * quiesce flushing, wipe, and a failed XAM answer falling back to the shared record.
 */
#define XS_HOST_TEST 1
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../xbox360/settings_360.c"

static int g_fail;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		g_fail = 1;
}

/* ---- clock and log ---------------------------------------------------------------------- */
static u64 g_ms = 1000;
static int g_verbose;

u64 xa_now_ms(void)
{
	return g_ms;
}

static void run_worker(void);

void xa_sleep_ms(unsigned int ms)
{
	g_ms += ms;
	run_worker(); /* the worker runs meanwhile */
}

void xa_log(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* ---- file worker and file system -------------------------------------------------------- */
#define FS_MAX 8192
static struct {
	u8 main[FS_MAX], tmp[FS_MAX];
	int main_len, tmp_len; /* -1: missing */
} g_fs = { .main_len = -1, .tmp_len = -1 };
static struct {
	void (*fn)(void *);
	void *arg;
} g_jobs[8];
static int g_njobs, g_posts, g_reads, g_writes, g_worker_ok = 1, g_auto = 1;

bool xf_post(void (*fn)(void *), void *arg)
{
	if (!g_worker_ok || g_njobs == 8)
		return false;
	g_jobs[g_njobs].fn = fn;
	g_jobs[g_njobs].arg = arg;
	g_njobs++;
	g_posts++;
	return true;
}

static void run_worker(void)
{
	for (int i = 0; i < g_njobs; i++)
		g_jobs[i].fn(g_jobs[i].arg);
	g_njobs = 0;
}

int xf_read(const char *path, void *buf, u32 max, u32 *len)
{
	int tmp = strstr(path, XF_NEW_SUFFIX) != NULL;
	int n = tmp ? g_fs.tmp_len : g_fs.main_len;

	g_reads++;
	*len = 0;
	if (n < 0)
		return XF_E_NOFILE;
	if ((u32)n > max)
		return XF_E_TOOBIG;
	memcpy(buf, tmp ? g_fs.tmp : g_fs.main, (size_t)n);
	*len = (u32)n;
	return XF_OK;
}

int xf_write(const char *path, const void *buf, u32 len)
{
	if (strcmp(path, XF_DIR "xw.settings"))
		check(0, "writes go to Hdd:\\Plugins\\xw.settings");
	g_writes++;
	memcpy(g_fs.main, buf, len);
	g_fs.main_len = (int)len;
	return XF_OK;
}

u32 xf_inplace_writes(void)
{
	return 0;
}

/* ---- the features ------------------------------------------------------------------------ */
struct xd_status g_xd;
static struct {
	int balance;
	u32 master, sidetone, mic_auto, surround, team_level, team_channel;
	u32 preset, count, ids[4];
	u32 custom_ok;   /* the EQ's file carries the CUSTOM tables */
	int custom[3];
} g_f;
static const u32 GAINS[10] = { 25, 35, 50, 70, 100, 140, 200, 280, 400, 560 };

/* the EQ's preset file: n presets (0 = not loaded, or the XWEQ gate off) */
static void eq_file(u32 n)
{
	g_f.count = n;
	g_f.custom_ok = 1; /* with the CUSTOM tables, as convert.py writes it */
	g_f.ids[0] = 111;
	g_f.ids[1] = 222;
	g_f.ids[2] = 333;
}

static void features_default(void)
{
	memset(&g_f, 0, sizeof(g_f));
	g_f.master = 50;
	g_f.sidetone = 5;
	g_f.team_level = 10;
	g_xd.mic_gain = 100;
	g_xd.gate = XD_GATE_2;
}

int xg_balance(void) { return g_f.balance; }
u32 xg_master(void) { return g_f.master; }
u32 xg_sidetone(void) { return g_f.sidetone; }
void xg_adjust(int db, int dm, int ds)
{
	int b = g_f.balance + db, m = (int)g_f.master + dm * 10, s = (int)g_f.sidetone + ds;

	g_f.balance = b < -5 ? -5 : b > 5 ? 5 : b;
	g_f.master = (u32)(m < 0 ? 0 : m > 100 ? 100 : m);
	g_f.sidetone = (u32)(s < 0 ? 0 : s > 10 ? 10 : s);
}
u32 xg_surround(void) { return g_f.surround; }
void xg_set_surround(u32 on) { g_f.surround = on ? 1 : 0; }

static u32 gain_index(u32 pct)
{
	u32 best = 0;

	for (u32 i = 1; i < 10; i++)
		if ((pct > GAINS[i] ? pct - GAINS[i] : GAINS[i] - pct) <
		    (pct > GAINS[best] ? pct - GAINS[best] : GAINS[best] - pct))
			best = i;
	return best;
}
u32 xd_mic_gain_step(u32 pct, int steps)
{
	int i = (int)gain_index(pct) + steps;

	return GAINS[i < 0 ? 0 : i > 9 ? 9 : i];
}
u32 xd_mic_level(u32 pct) { return gain_index(pct) + 1; }
bool xd_mic_auto(void) { return g_f.mic_auto; }
void xd_set_mic_auto(bool on) { g_f.mic_auto = on; }
static int g_xd_polls;
void xd_poll(void) { g_xd_polls++; }
u32 xd_timebase(void) { return (u32)(g_ms * 50000); }

u32 xq_count(void) { return g_f.count; }
u32 xq_preset(void) { return g_f.count ? g_f.preset : 0; }
bool xq_custom_offered(void) { return g_f.count && g_f.custom_ok; }
u32 xq_custom_index(void) { return xq_custom_offered() ? g_f.count + 1 : 0; }
void xq_set_preset(u32 i)
{
	g_f.preset = i <= g_f.count || (i && i == xq_custom_index()) ? i : 0;
}
int xq_custom(u32 b) { return b < 3 ? g_f.custom[b] : 0; }
void xq_set_custom(u32 b, int s)
{
	if (b < 3)
		g_f.custom[b] = s < -6 ? -6 : s > 6 ? 6 : s;
}
u32 xq_preset_id(u32 i) { return i && i <= g_f.count ? g_f.ids[i - 1] : 0; }
u32 xq_find(u32 id)
{
	for (u32 i = 1; id && i <= g_f.count; i++)
		if (g_f.ids[i - 1] == id)
			return i;
	return 0;
}

u32 xt_level(void) { return g_f.team_level; }
void xt_set_level(u32 l) { g_f.team_level = l; }
u32 xt_channel(void) { return g_f.team_channel; }
void xt_set_channel(u32 c) { g_f.team_channel = c; }

/* ---- who is on the headset's pad -------------------------------------------------------- */
static int g_hs = -1, g_user = 0, g_xam_calls;
static u64 g_xuid;
static u32 g_state; /* 0 signed out, 1 local */
static s32 g_xam_status;

int xw_headset_pad(void) { return g_hs; }
int xa_xinput_user(int slot) { return slot == g_hs ? g_user : -1; }
s32 xam_signin(u32 user, u64 *xuid, u32 *state)
{
	(void)user;
	g_xam_calls++;
	*xuid = g_state ? g_xuid : 0;
	*state = g_state;
	return g_xam_status ? g_xam_status : g_state ? 0 : XS_NO_SUCH_USER;
}

static void sign_in(u64 xuid)
{
	g_hs = 0;
	g_xuid = xuid;
	g_state = xuid ? 1 : 0;
}

/* ---- helpers ----------------------------------------------------------------------------- */

/* ms of driver-thread passes, 2 ms apart, the worker running between them */
static void run(u32 ms)
{
	for (u32 t = 0; t < ms; t += 2) {
		g_ms += 2;
		xs_poll();
		if (g_auto)
			run_worker();
	}
}

static void boot(int gate)
{
	xs_host_reset();
	g_njobs = 0;
	features_default();
	xs_init();
	g_xs.enable = (u32)gate;
	run(20);
}

static u32 crc32_ref(const u8 *p, u32 n)
{
	u32 c = 0xFFFFFFFFu;

	for (u32 i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
	}
	return ~c;
}

static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* the record of xuid in the fake disk's file, or NULL */
static const u8 *disk_rec(u64 xuid)
{
	for (int i = 0; i < 64; i++) {
		const u8 *r = g_fs.main + 16 + 48 * i;

		if (16 + 48 * (i + 1) <= g_fs.main_len &&
		    ((u64)rd32(r) << 32 | rd32(r + 4)) == xuid)
			return r;
	}
	return NULL;
}

static void wipe_disk(void)
{
	g_fs.main_len = g_fs.tmp_len = -1;
}

/* ---- scenarios --------------------------------------------------------------------------- */

static void t_gate_off(void)
{
	wipe_disk();
	g_posts = g_xam_calls = 0;
	sign_in(0x0009000000000001ull);
	boot(0);
	g_f.balance = -2;
	run(10000);
	check(g_posts == 0 && g_xam_calls == 0 && g_fs.main_len < 0 && !g_mem,
	      "gate off: no file read or written, no XAM call, no memory");
	check(g_xs.state == XS_ST_OFF, "gate off: state OFF");
	g_hs = -1;
}

static void t_roundtrip(void)
{
	const u8 *r;

	wipe_disk();
	boot(1);
	check(g_xs.state == XS_ST_READY && g_xs.loads == 1 && (s32)g_xs.last_error == 0,
	      "no file: loaded empty, no error");
	check(g_restored && g_active == 0 && g_xs.slot == XS_NO_SLOT,
	      "no headset at boot: the shared record (XUID 0), defaults");
	g_f.balance = -3;
	xg_adjust(0, -2, 0); /* volume 3 */
	g_xd.mic_gain = 200; /* level 7 */
	g_xd.gate = XD_GATE_4;
	g_f.surround = 1;
	g_f.mic_auto = 1;
	run(2600);
	check(g_writes == 1 && g_fs.main_len == 3088, "one save, the whole 3088-byte file");
	r = disk_rec(0);
	check(rd32(g_fs.main) == 0x58575354 && rd32(g_fs.main + 4) == (1u << 16 | 48) &&
		      rd32(g_fs.main + 8) == (1u << 16 | 64),
	      "header: XWST, version 1, 48-byte records, 1 of 64");
	check(r && r[12] == 1 && (s8)r[13] == -3 && r[14] == 3 && r[15] == 5 && r[16] == 7 &&
		      r[17] == XD_GATE_4 && r[18] == 1 && r[19] == XS_F_MIC_AUTO &&
		      rd32(r + 8) == 1,
	      "record bytes: balance -3, volume 3, sidetone 5, mic 7 + AUTO, gate high, surround");
	check(r && rd32(r + 44) == crc32_ref(r, 44), "record CRC is zlib's CRC-32 of 44 bytes");
	if (getenv("XS_DUMP")) {
		/* for xbox360/settings.py dump --file: the Python reader on the C writer's bytes */
		FILE *f = fopen(getenv("XS_DUMP"), "wb");

		if (f) {
			fwrite(g_fs.main, 1, (size_t)g_fs.main_len, f);
			fclose(f);
		}
	}

	/* reboot: everything back */
	boot(1);
	check(g_f.balance == -3 && g_f.master == 30 && g_xd.mic_gain == 200 &&
		      g_xd.gate == XD_GATE_4 && g_f.surround == 1 && g_f.mic_auto == 1,
	      "after a reboot: all of it restored");
	g_writes = 0;
	run(10000);
	check(g_writes == 0, "restore alone writes nothing");
}

static void t_debounce(void)
{
	wipe_disk();
	boot(1);
	g_writes = 0;
	g_f.balance = 1;
	run(1500);
	g_f.balance = 2; /* another step before 2 s */
	run(1900);
	check(g_writes == 0, "debounce: nothing written 1.9 s after the last change");
	run(400);
	check(g_writes == 1 && (s8)disk_rec(0)[13] == 2, "debounce: written once, 2 s after the last change");
	g_f.balance = 3;
	run(1000);
	g_f.balance = 2; /* back before the save */
	run(5000);
	check(g_writes == 1, "changed and changed back: no write");
}

/* the u32 millisecond clock wraps (about 49.7 days up) in the middle of a debounce */
static void t_clock_wrap(void)
{
	u64 ms0 = g_ms;

	wipe_disk();
	g_ms = 0x100000000ull - 1500;
	boot(1);
	g_writes = 0;
	g_f.balance = 3;
	run(1900);
	check(g_writes == 0, "clock wrap: nothing written before 2 s");
	run(1000);
	check(g_writes == 1 && (s8)disk_rec(0)[13] == 3, "clock wrap: written once, on time");
	g_ms = ms0;
}

static void t_crc_reject_and_torn(void)
{
	u8 *r;

	wipe_disk();
	boot(1);
	/* three profiles: 0 (nobody), A, B */
	g_f.balance = 1;
	run(2600);
	sign_in(0xA);
	run(1100);
	g_f.balance = 4;
	run(2600);
	sign_in(0xB);
	run(1100);
	g_f.balance = -4;
	run(2600);
	check(disk_rec(0) && disk_rec(0xA) && disk_rec(0xB), "three profiles on disk");

	/* CRC: A's record is damaged */
	r = (u8 *)disk_rec(0xA);
	r[13] ^= 0x10;
	sign_in(0xA);
	boot(1);
	run(1100);
	check(g_xs.crc_rejects == 1 && g_active == 0xA && g_f.balance == 0,
	      "CRC reject: that profile gets the defaults");
	check(disk_rec(0xB) != NULL, "CRC reject: the others survive");
	r[13] ^= 0x10;

	/* torn: the file cut in the middle of the third record */
	g_fs.main_len = 16 + 48 * 2 + 20;
	sign_in(0xB);
	boot(1);
	run(1100);
	{
		int a = table_find(g_mem, 0xA), b = table_find(g_mem, 0xB), n = table_find(g_mem, 0);
		int lost = (a < 0) + (b < 0) + (n < 0);

		check(lost == 1 && g_xs.crc_rejects == 0, "torn file: the cut record is lost, no reject");
	}
	g_fs.main_len = 3088;

	/* the header itself unreadable (a cut before it): the .new copy is used */
	memcpy(g_fs.tmp, g_fs.main, 3088);
	g_fs.tmp_len = 3088;
	g_fs.main_len = 10;
	sign_in(0xB);
	boot(1);
	run(1100);
	check(g_active == 0xB && g_f.balance == -4 && !g_xs.errors, "short file: read from .new");
	g_fs.tmp_len = -1;
	g_fs.main_len = 3088;
}

static void t_version(void)
{
	sign_in(0);
	g_hs = -1;
	wipe_disk();
	boot(1);
	g_f.balance = 5;
	run(2600);
	g_fs.main[5] = 2; /* version 2 */
	boot(1);
	check(g_f.balance == 0 && (s32)g_xs.last_error == XS_E_HEADER && g_xs.errors == 1,
	      "wrong version: defaults, error counted");
	g_f.balance = -1;
	run(2600);
	check(g_fs.main[5] == 1 && (s8)disk_rec(0)[13] == -1, "wrong version: rewritten as version 1");
}

static void t_evict(void)
{
	u8 f[3088];
	struct xs_vals v = { 0 };

	/* the table directly */
	table_init(f);
	for (u32 i = 0; i < 64; i++) {
		u32 s = table_slot_for(f, 100 + i);

		rec_put(rec_at(f, s), 100 + i, i == 10 ? 1 : 1000 + i, &v); /* 110 is the oldest */
	}
	{
		u32 s = table_slot_for(f, 999);

		check(be64(rec_at(f, s)) == 110, "table: full, the lowest seq is replaced");
	}

	/* the whole flow: 65 profiles in turn */
	wipe_disk();
	boot(1);
	for (u64 x = 1; x <= 65; x++) {
		sign_in(x);
		run(1100);
		g_f.balance = (int)(x % 5) + 1;
		run(2600);
	}
	check(!disk_rec(1) && disk_rec(2) && disk_rec(65), "65th profile: the first one saved is gone");
	check(rd32(g_fs.main + 8) >> 16 == 64, "65th profile: 64 records");
	sign_in(1);
	run(1100);
	check(g_f.balance == 0, "the evicted profile gets the defaults");
}

static void t_presets(void)
{
	u8 *r;

	g_hs = -1;
	wipe_disk();
	boot(1);
	g_writes = 0;
	eq_file(3);
	run(300); /* the EQ's file arrives: the profile's preset (FLAT, no record) is applied */
	check(g_f.preset == 0 && g_writes == 0, "EQ loaded: the saved preset applied, nothing written");
	g_f.preset = 2; /* the player picks one */
	run(2600);
	check(disk_rec(0) && rd32(disk_rec(0) + 20) == 222, "preset saved by id");

	/* boot with the EQ file not loaded yet: the preset waits, and is not saved as FLAT */
	g_f.count = 0;
	boot(1);
	g_f.balance = 2;
	run(2600);
	check(rd32(disk_rec(0) + 20) == 222, "EQ not loaded: another change keeps the preset id");
	eq_file(3);
	run(300);
	check(g_f.preset == 2, "EQ loaded later: the preset is selected then");

	/* the EQ's file never loads (missing, or the XWEQ gate off) */
	g_f.count = 0;
	boot(1);
	g_f.balance = -2;
	run(2600);
	g_f.balance = -1;
	run(2600);
	check(rd32(disk_rec(0) + 20) == 222 && (s8)disk_rec(0)[13] == -1,
	      "EQ never loads: saves keep the preset id, never FLAT");
	eq_file(3);
	run(300);
	check(g_f.preset == 2, "EQ switched on much later: the preset is selected then");
	g_f.preset = 0;

	/* an id the file no longer has */
	r = (u8 *)disk_rec(0);
	put_be32(r + 20, 0xDEAD);
	put_be32(r + 44, crc32_ref(r, 44));
	g_writes = 0;
	boot(1);
	g_f.preset = 1; /* whatever was selected before */
	eq_file(3);
	run(300);
	check(g_f.preset == 0, "unknown preset id: FLAT");
	run(5000);
	check(g_writes == 0, "unknown preset id: not written back by itself");
	g_f.count = 0;
}

static void t_custom(void)
{
	u8 *r;

	g_hs = -1;
	wipe_disk();
	boot(1);
	eq_file(3);
	run(300);
	g_f.preset = 4; /* the player picks CUSTOM (index count + 1) and shapes it */
	g_f.custom[0] = 2;
	g_f.custom[1] = -1;
	g_f.custom[2] = 6;
	run(2600);
	r = (u8 *)disk_rec(0);
	check(r && r[19] == XS_F_CUSTOM && rd32(r + 20) == 0 && (s8)r[27] == 2 && (s8)r[28] == -1 &&
		      (s8)r[29] == 6 && !r[30] && !r[43],
	      "custom: saved as the flag, preset id 0 and the three steps (bytes 27-29)");

	/* boot with the EQ file not loaded yet: the steps come back at once, the selection waits */
	memset(g_f.custom, 0, sizeof(g_f.custom));
	g_f.count = 0;
	g_f.preset = 0;
	boot(1);
	check(g_f.custom[0] == 2 && g_f.custom[1] == -1 && g_f.custom[2] == 6,
	      "custom: steps put back before the EQ's file loads");
	g_f.balance = 3;
	run(2600);
	r = (u8 *)disk_rec(0);
	check(r[19] == XS_F_CUSTOM && (s8)r[13] == 3,
	      "custom: EQ not loaded, another change keeps CUSTOM selected");
	eq_file(3);
	run(300);
	check(g_f.preset == 4, "custom: the EQ loads later, CUSTOM is selected then");

	/* a step changed is saved */
	g_f.custom[1] = -4;
	run(2600);
	check((s8)disk_rec(0)[28] == -4, "custom: a step change is saved");

	/* a file without CUSTOM: FLAT, not written back by itself */
	g_writes = 0;
	boot(1);
	eq_file(3);
	g_f.custom_ok = 0;
	run(300);
	check(g_f.preset == 0, "custom: a file without CUSTOM selects FLAT");
	run(5000);
	check(g_writes == 0 && disk_rec(0)[19] == XS_F_CUSTOM,
	      "custom: that FLAT is not written back by itself (CUSTOM kept on disk)");

	/* an older record: reserved bytes zero, no flag: CUSTOM 0 0 0, not selected */
	r = (u8 *)disk_rec(0);
	r[19] = 0;
	memset(r + 27, 0, 3);
	put_be32(r + 44, crc32_ref(r, 44));
	boot(1);
	eq_file(3);
	run(300);
	check(!g_f.custom[0] && !g_f.custom[1] && !g_f.custom[2] && g_f.preset == 0,
	      "custom: an older record reads as CUSTOM 0 0 0, not selected");

	/* out-of-range steps in a hand-edited record are clamped */
	r = (u8 *)disk_rec(0);
	r[27] = 0x7F;
	r[28] = 0x80;
	put_be32(r + 44, crc32_ref(r, 44));
	boot(1);
	check(g_f.custom[0] == 6 && g_f.custom[1] == -6, "custom: steps clamped to -6..6");
	g_f.count = 0;
}

static void t_switch(void)
{
	wipe_disk();
	sign_in(0xA);
	boot(1);
	run(1100);
	check(g_active == 0xA && g_xam_calls > 0, "headset at boot: its profile");
	g_f.balance = -3;
	run(2600);
	sign_in(0xB);
	run(1100);
	check(g_active == 0xB && g_f.balance == 0, "new profile: the defaults, not the previous player's");
	xg_adjust(0, 3, 0); /* volume 8 */
	run(500);           /* not saved yet when A comes back */
	sign_in(0xA);
	run(1100);
	check(g_f.balance == -3 && g_f.master == 50, "back to A: A's values");
	run(3000);
	check(disk_rec(0xB) && disk_rec(0xB)[14] == 8, "B's last change was kept at the switch");
	sign_in(0); /* signs out */
	run(1100);
	check(g_active == 0 && g_f.balance == 0, "signed out: the shared record");

	/* XAM refuses: the shared record, never stuck */
	g_xam_status = 0x80004005;
	sign_in(0xB);
	run(1100);
	check(g_active == 0 && g_xs.xam_status == 0x80004005, "XAM error: the shared record");
	g_xam_status = 0;
	run(1100);
	check(g_active == 0xB && g_f.master == 80, "XAM answers again: B");

	/* "no such user" as an HRESULT (XAM's own form): nobody, and not an XAM error */
	g_xs.xam_status = 0;
	g_xam_status = (s32)0x80070525;
	run(1100);
	check(g_active == 0 && g_xs.xam_status == 0, "no such user (HRESULT): the shared record, no error");
	g_xam_status = 0;
	run(1100);
	check(g_active == 0xB, "and B again");

	/* the pad without a user index (bind not done) */
	g_user = -1;
	run(1100);
	check(g_active == 0, "pad not bound in XAM: the shared record");
	g_user = 0;
	run(1100);
	check(g_active == 0xB, "pad bound again: B");
	g_hs = -1;
	run(3000);
	check(g_active == 0xB, "headset gone: the profile stays");
}

static void t_quiesce_wipe_worker(void)
{
	g_hs = -1;
	wipe_disk();
	boot(1);
	g_writes = 0;
	g_f.balance = 4;
	run(300); /* seen, not yet saved */
	g_auto = 0; /* the worker runs only inside xa_sleep_ms from here */
	xs_quiesce();
	g_auto = 1;
	check(g_writes == 1 && (s8)disk_rec(0)[13] == 4 && g_xs.state == XS_ST_QUIESCED,
	      "quiesce: the pending change is written now");
	g_f.balance = -4;
	run(5000);
	check(g_writes == 1, "quiesced: nothing more is saved");
	xs_resume();
	run(5000);
	check(g_writes == 2, "resumed: saving again");

	/* a sign-in question queued at quiesce and dropped (xf_quiesce): asked again on resume */
	sign_in(0xC);
	g_auto = 0;
	run(1100);       /* posted, the worker not run */
	g_worker_ok = 0; /* the worker is stalled: quiesce's wait runs out */
	xs_quiesce();
	g_njobs = 0;     /* xf_quiesce drops the queue */
	g_worker_ok = 1;
	g_auto = 1;
	xs_resume();
	run(1100);
	check(g_active == 0xC, "quiesce dropped a sign-in question: asked again after resume");
	g_hs = -1;
	run(3000);

	g_xs.wipe = 1;
	run(100);
	check(g_f.balance == 0 && g_xs.records == 0 && rd32(g_fs.main + 8) >> 16 == 0,
	      "wipe: every profile forgotten, defaults applied");

	/* no worker: never blocks, retries */
	wipe_disk();
	g_worker_ok = 0;
	boot(1);
	run(3000);
	check(g_xs.state == XS_ST_NO_WORKER && !g_loaded, "no worker: state says so, nothing applied");
	g_worker_ok = 1;
	run(6000);
	check(g_xs.state == XS_ST_READY, "worker back: loaded after the retry");
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	g_verbose = argc > 1 && !strcmp(argv[1], "-v");
	t_gate_off();
	t_roundtrip();
	t_debounce();
	t_clock_wrap();
	t_crc_reject_and_torn();
	t_version();
	t_evict();
	t_presets();
	t_custom();
	t_switch();
	t_quiesce_wipe_worker();
	printf(g_fail ? "FAILED\n" : "all passed\n");
	return g_fail;
}
