// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of the plugin side of the equalizer (xbox360/eq_360.c, compiled in whole) with a
 * fake file worker (file_360.h): the load and its hand-over, every refusal (missing file, bad
 * CRC, wrong rate, too many presets, a version-1 file, no worker), the gate, quiesce, the XWEQ
 * controls, the descriptions, and a preset switch in the middle of a little-endian stereo
 * stream, compared sample for sample with the kernel (eq_dsp.h) driven directly. CUSTOM:
 * offered with the tables, index / name / description, steps set before the file arrives,
 * every change crossfaded (a stream of changes faster than a held direction, bit-exact with
 * the kernel fed a fresh cascade per change), 0 0 0 = FLAT bit for bit, the XWEQ custom_set
 * control, and a bad table refusing CUSTOM while the presets load. XWEQ pass_us: the EQ's
 * time between two xq_poll, and the longest kept. A steady stereo stream takes eq_360.c's
 * fast path (eqd_steady_le2), so the stream comparisons check it too.
 *
 *   python3 tools/presets/convert.py
 *   cc -O2 -Wall -o test_eq360 tests/test_eq360.c -lm && ./test_eq360 [tools/presets/xw.presets]
 */
#define XQ_HOST_TEST 1
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include "../xbox360/eq_360.c"

static int g_fail;
static u64 g_ms;
static int g_logs;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		g_fail = 1;
}

/* ---- fakes for platform.h and file_360.h ----------------------------------------------- */

u64 xa_now_ms(void)
{
	return g_ms;
}

void xa_log(const char *fmt, ...)
{
	va_list ap;

	g_logs++;
	va_start(ap, fmt);
	printf("     log: ");
	vprintf(fmt, ap);
	va_end(ap);
}

static const u8 *g_file; /* what Hdd:\Plugins\xw.presets holds; NULL = missing */
static u32 g_flen;
static int g_worker = 1; /* 0: xf_post refuses, as the stub file_360.c does */
static void (*g_fn)(void *);
static void *g_arg;
static int g_path_ok = 1;

int xf_read(const char *path, void *buf, u32 max, u32 *len)
{
	if (strcmp(path, XF_DIR "xw.presets"))
		g_path_ok = 0;
	*len = 0;
	if (!g_file)
		return XF_E_NOFILE;
	if (g_flen > max)
		return XF_E_TOOBIG;
	memcpy(buf, g_file, g_flen);
	*len = g_flen;
	return XF_OK;
}

bool xf_post(void (*fn)(void *), void *arg)
{
	if (!g_worker || g_fn)
		return false;
	g_fn = fn;
	g_arg = arg;
	return true;
}

/* the worker thread runs the queued job */
static void worker(void)
{
	if (g_fn) {
		void (*fn)(void *) = g_fn;
		g_fn = NULL;
		fn(g_arg);
	}
}

/* a fresh plugin start */
static void reset(void)
{
	free(g_tab);
	g_tab = NULL;
	memset(&g_job, 0, sizeof(g_job));
	g_sel = g_enabled = g_tries = 0;
	memset(g_cust, 0, sizeof(g_cust));
	g_post_ms = 0;
	g_fn = NULL;
	u32 en = g_xq.enable;
	memset(&g_xq, 0, sizeof(g_xq));
	g_xq.magic = 0x58574551;
	g_xq.version = 2;
	g_xq.select = XQ_NO_SELECT;
	g_xq.flat_q15 = 32768;
	g_xq.custom_error = (u32)XQ_C_NOFILE;
	g_xq.enable = en;
}

/* init, run the worker, poll: the load as it happens at start-up */
static void boot(const u8 *f, u32 len, u32 enable)
{
	g_xq.enable = enable;
	reset();
	g_file = f;
	g_flen = len;
	xq_init();
	worker();
	xq_poll();
}

static void put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

/* a copy of the file with n records (the real ones repeated), the CUSTOM tables, a fresh CRC */
static u8 *with_count(const u8 *f, u32 flen, u32 n, u32 *len)
{
	u32 have = eqd_be32(f + 12), tail = flen - EQD_HDR - have * EQD_REC;
	*len = EQD_HDR + n * EQD_REC + tail;
	u8 *g = malloc(*len);
	memcpy(g, f, EQD_HDR);
	for (u32 i = 0; i < n; i++)
		memcpy(g + EQD_HDR + i * EQD_REC, f + EQD_HDR + (i % have) * EQD_REC, EQD_REC);
	memcpy(g + EQD_HDR + n * EQD_REC, f + EQD_HDR + have * EQD_REC, tail);
	put_be32(g + 12, n);
	put_be32(g + 24, eqd_crc32(g + EQD_HDR, *len - EQD_HDR));
	return g;
}

static void fix_crc(u8 *f, u32 len)
{
	put_be32(f + 24, eqd_crc32(f + EQD_HDR, len - EQD_HDR));
}

/* ---- the stream -------------------------------------------------------------------------- */

enum { FRAMES = 384, BYTES = FRAMES * 4 };

static void make_block(u8 *b, u32 k)
{
	static u32 seed = 99;
	for (int i = 0; i < FRAMES; i++) {
		double t = (double)(k * FRAMES + i) / EQD_RATE;
		for (int c = 0; c < 2; c++) {
			seed = seed * 1664525u + 1013904223u;
			int v = (int)lrint(12000 * sin(2 * M_PI * (c ? 3000 : 220) * t)) +
				(int)((s32)seed >> 20);
			b[4 * i + 2 * c] = (u8)v;
			b[4 * i + 2 * c + 1] = (u8)(v >> 8);
		}
	}
}

/* The same stream through eq_360.c (bytes) and through eqd_block directly (native s16, in
 * XQ_CHUNK pieces as eq_360.c calls it): requests at the same block starts. Returns the samples that differ. */
static u64 stream(const u8 *f, const u32 *sel, int nblocks, u32 *untouched_flat)
{
	struct eqd_bands b[XQ_MAX_PRESETS];
	char name[EQD_NAME + 1], author[EQD_AUTHOR + 1], desc[EQD_DESC];
	s16 g[EQD_BANDS], pcm[2 * FRAMES];
	s32 work[2 * FRAMES];
	u8 blk[BYTES], orig[BYTES];
	struct eqd_run ref;
	struct eqd_bands flat;
	const struct eqd_bands *flatp;
	u64 differ = 0;

	for (u32 i = 0; i < eqd_be32(f + 12); i++)
		eqd_record(f, i, &b[i], name, author, desc, g);
	memset(&flat, 0, sizeof(flat));
	flat.preamp_q15 = eqd_flat_q15(f);
	flatp = flat.preamp_q15 == 32768 ? NULL : &flat;
	memset(&ref, 0, sizeof(ref));
	*untouched_flat = 0;
	for (int k = 0; k < nblocks; k++) {
		if (sel[k] != XQ_NO_SELECT) {
			g_xq.select = sel[k]; /* as eq.py select does; xq_poll takes it */
			xq_poll();
		}
		make_block(blk, (u32)k);
		memcpy(orig, blk, BYTES);
		xq_process(1, blk, BYTES, 48000, 2);
		u32 s = xq_preset();
		eqd_request(&ref, s ? &b[s - 1] : flatp);
		for (int i = 0; i < 2 * FRAMES; i++)
			pcm[i] = (s16)(orig[2 * i] | orig[2 * i + 1] << 8);
		for (int i = 0; i < FRAMES; i += XQ_CHUNK) /* a queued fade starts at a chunk */
			eqd_block(&ref, pcm + 2 * i, XQ_CHUNK, 2, work);
		for (int i = 0; i < 2 * FRAMES; i++)
			differ += (s16)(blk[2 * i] | blk[2 * i + 1] << 8) != pcm[i];
		if (!flatp && s == 0 && !ref.fading && !memcmp(blk, orig, BYTES))
			(*untouched_flat)++;
	}
	return differ;
}

/* CUSTOM selected, its steps changed at block starts (chg[k]: 1 = the next step of a walk
 * over every control), through eq_360.c and through the kernel with a fresh cascade per
 * change (0 0 0 = FLAT itself), requested at the same block starts. Returns the samples that
 * differ; *fades counts blocks the plugin spent fading. */
static u64 custom_stream(const u8 *f, u32 flen, const u8 *chg, int nblocks, u32 *fades)
{
	static struct eqd_custom ct;
	static struct eqd_bands ref_b[256];
	s16 pcm[2 * FRAMES];
	s32 work[2 * FRAMES];
	u8 blk[BYTES], orig[BYTES];
	struct eqd_run ref;
	struct eqd_bands flat;
	const struct eqd_bands *flatp, *want;
	u64 differ = 0;
	int nref = 0, st[3] = { xq_custom(0), xq_custom(1), xq_custom(2) }, walk = 0;

	memset(&flat, 0, sizeof(flat));
	flat.preamp_q15 = eqd_flat_q15(f);
	flatp = flat.preamp_q15 == 32768 ? NULL : &flat;
	if (eqd_custom_read(f, flen, eqd_be32(f + 12), flat.preamp_q15, &ct))
		return ~0ull;
	*fades = 0;
	want = flatp;
	if (st[0] || st[1] || st[2]) {
		eqd_custom_bands(&ct, st[0], st[1], st[2], &ref_b[nref]);
		want = &ref_b[nref++];
	}
	/* the reference starts where pad 2 is once settled: same history, its own pointers */
	for (int k = 0; k < 10 && (g_tab->run[2].fading || g_tab->run[2].pc != target(g_tab)); k++) {
		make_block(blk, 1000u + (u32)k);
		xq_process(2, blk, BYTES, 48000, 2);
	}
	memcpy(&ref, &g_tab->run[2], sizeof(ref));
	ref.pc = ref.want = g_tab->run[2].pc == NULL ? NULL : g_tab->run[2].pc == &g_tab->flat ? flatp : want;
	ref.pn = NULL;
	for (int k = 0; k < nblocks; k++) {
		if (chg[k]) { /* bass up to +6, down to -6, back to 0, then mids, then treble */
			int c = (walk / 24) % 3, n = walk % 24;
			int v = n < 6 ? n + 1 : n < 18 ? 11 - n : n - 23;
			walk++;
			st[c] = v;
			xq_set_custom((u32)c, v);
			if (!st[0] && !st[1] && !st[2]) {
				want = flatp;
			} else {
				eqd_custom_bands(&ct, st[0], st[1], st[2], &ref_b[nref % 256]);
				want = &ref_b[nref++ % 256];
			}
		}
		make_block(blk, (u32)k);
		memcpy(orig, blk, BYTES);
		xq_process(2, blk, BYTES, 48000, 2);
		*fades += g_xq.fade != 0;
		eqd_request(&ref, want);
		for (int i = 0; i < 2 * FRAMES; i++)
			pcm[i] = (s16)(orig[2 * i] | orig[2 * i + 1] << 8);
		for (int i = 0; i < FRAMES; i += XQ_CHUNK)
			eqd_block(&ref, pcm + 2 * i, XQ_CHUNK, 2, work);
		for (int i = 0; i < 2 * FRAMES; i++)
			differ += (s16)(blk[2 * i] | blk[2 * i + 1] << 8) != pcm[i];
	}
	return differ;
}

static int untouched(int pad, u32 rate, u32 ch)
{
	u8 blk[BYTES], orig[BYTES];
	make_block(blk, 7);
	memcpy(orig, blk, BYTES);
	xq_process(pad, blk, BYTES, rate, ch);
	return !memcmp(blk, orig, BYTES);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "tools/presets/xw.presets";
	FILE *fp = fopen(path, "rb");
	static u8 file[XQ_FILE_MAX + EQD_REC];
	u32 len, l2;

	if (!fp) {
		fprintf(stderr, "cannot open %s (python3 tools/presets/convert.py)\n", path);
		return 2;
	}
	len = (u32)fread(file, 1, sizeof(file), fp);
	fclose(fp);
	u32 n = eqd_be32(file + 12);

	/* good file, gate off: loaded, but the panel sees nothing and audio is untouched */
	boot(file, len, 0);
	check(g_xq.load_state == XQ_LOAD_OK && g_xq.load_error == 0 && g_path_ok,
	      "good file: loaded from " XF_DIR "xw.presets");
	check(xq_count() == 0 && g_xq.count == 0 && !strcmp(xq_name(1), "FLAT"),
	      "gate off: count 0, names FLAT");
	check(untouched(0, 48000, 2), "gate off: audio untouched");
	printf("     heap %u bytes (budget 16384), FLAT %d/32768, crc %08x\n", g_xq.heap_bytes,
	       (int)g_xq.flat_q15, g_xq.file_crc);
	check(g_xq.heap_bytes > 0 && g_xq.heap_bytes <= 16384, "heap within 16 KB");

	/* gate on */
	g_xq.enable = 1;
	xq_poll();
	u32 id5 = xq_preset_id(5);
	check(xq_count() == n && g_xq.count == n && xq_find(id5) == 5 && xq_find(0) == 0 &&
		      xq_find(0x12345678) == 0 && xq_preset_id(0) == 0 && xq_preset_id(n + 1) == 0 &&
		      !strcmp(xq_name(0), "FLAT") && !strcmp(xq_name(n + 2), "FLAT") &&
		      strlen(xq_name(1)) <= 20,
	      "gate on: count, ids, find, names");
	printf("     preset 1 \"%s\", preset %u \"%s\"\n", xq_name(1), n, xq_name(n));
	xq_set_preset(n + 5);
	check(xq_preset() == 0, "an index above the count selects FLAT");

	/* descriptions */
	{
		char want[EQD_DESC];
		memcpy(want, file + EQD_HDR + 160, EQD_DESC);
		want[EQD_DESC - 1] = 0;
		check(!strcmp(xq_desc(1), want) && xq_desc(1)[0] && strlen(xq_desc(n)) < XQ_DESC_LEN &&
			      !strcmp(xq_desc(0), g_flat_desc) &&
			      !strcmp(xq_desc(n + 1), g_custom_desc) && !strcmp(xq_desc(n + 2), ""),
		      "descriptions: FLAT's, each record's, CUSTOM's, \"\" out of range");
		printf("     FLAT: %s\n     1: %s\n     CUSTOM: %s\n", xq_desc(0), xq_desc(1),
		       xq_desc(n + 1));
	}

	/* CUSTOM: offered, index, name, selection, steps, the XWEQ control */
	check(xq_custom_offered() && xq_custom_index() == n + 1 && !strcmp(xq_name(n + 1), "CUSTOM") &&
		      g_xq.file_version == 2 && (s32)g_xq.custom_error == 0,
	      "custom: offered as index count + 1, named CUSTOM, file version 2");
	xq_set_preset(n + 1);
	xq_poll();
	check(xq_preset() == n + 1 && xq_preset_id(n + 1) == 0 && g_xq.preset == n + 1 &&
		      g_xq.custom_index == n + 1,
	      "custom: selected by its index (no id)");
	xq_set_custom(0, 9);
	xq_set_custom(1, -9);
	xq_set_custom(2, 3);
	xq_set_custom(3, 1);
	check(xq_custom(0) == 6 && xq_custom(1) == -6 && xq_custom(2) == 3 && xq_custom(3) == 0,
	      "custom: steps clamped to -6..6, a fourth band ignored");
	g_xq.custom_set = XQ_CUSTOM_SET | (u8)(s8)-2 | 1 << 8 | 0 << 16;
	xq_poll();
	check(xq_custom(0) == -2 && xq_custom(1) == 1 && xq_custom(2) == 0 && g_xq.custom_set == 0 &&
		      g_xq.custom == ((u8)(s8)-2 | 1u << 8),
	      "custom: XWEQ custom_set sets all three, custom reads them back");
	xq_set_custom(0, 0);
	xq_set_custom(1, 0);
	xq_set_preset(0);

	/* a switch mid-stream, a queued request during a fade, back to FLAT */
	u32 sel[40];
	for (int i = 0; i < 40; i++)
		sel[i] = XQ_NO_SELECT;
	sel[0] = 0;
	sel[5] = 3;
	sel[6] = 9; /* while 3 is still warming up: waits for that fade */
	sel[20] = 0;
	sel[30] = n;
	u32 flat_untouched;
	u64 d = stream(file, sel, 40, &flat_untouched);
	printf("     40 blocks of 384 stereo frames, 4 switches: %llu samples differ, clamps %u, "
	       "blocks %u\n",
	       (unsigned long long)d, g_xq.clamps, g_xq.blocks);
	check(d == 0, "stream through eq_360.c equals the kernel, bit for bit (s16le in place)");

	/* other rates, pads, layouts: untouched */
	u32 sk = g_xq.skipped;
	check(untouched(0, 44100, 2) && untouched(0, 24000, 1) && untouched(4, 48000, 2) &&
		      untouched(-1, 48000, 2) && untouched(0, 48000, 6) && g_xq.skipped == sk + 5,
	      "other rate, pad or channel count: untouched, counted as skipped");

	/* CUSTOM: a change every block (12x faster than a held direction), every step of every
	 * control, crossfaded like a preset change, against the kernel fed a fresh cascade per
	 * change; slots are reused all along (a playing one never written) */
	{
		static u8 chg[400];
		u32 fades;
		xq_set_preset(0);
		for (int i = 0; i < 4; i++)
			untouched(2, 48000, 2); /* pad 2 settles on FLAT */
		xq_set_preset(xq_custom_index());
		memset(chg, 1, 72);
		u64 dc = custom_stream(file, len, chg, 72, &fades);
		printf("     72 blocks, a step each: %llu samples differ, %u blocks fading\n",
		       (unsigned long long)dc, fades);
		check(dc == 0 && fades > 30, "custom: every change crossfaded, bit-exact with the kernel");
		memset(chg, 0, sizeof(chg));
		for (int i = 0; i < 384; i += 16)
			chg[i] = 1; /* a held direction: 8 a second = every 16th 8 ms block; 24 steps
				     * take BASS up to +6, down to -6 and back to 0 */
		dc = custom_stream(file, len, chg, 400, &fades);
		check(dc == 0, "custom: a held direction (8 steps a second), bit-exact");
		check(!xq_custom(0) && !xq_custom(1) && !xq_custom(2) &&
			      target(g_tab) == g_tab->flatp,
		      "custom: back at 0 0 0 plays FLAT itself");
		memset(chg, 0, sizeof(chg));
		xq_set_preset(xq_custom_index());
		u64 d0 = custom_stream(file, len, chg, 20, &fades);
		check(d0 == 0 && fades == 0, "custom: 0 0 0 is FLAT bit for bit, no fade to it");
	}

	/* quiesce / resume */
	xq_set_preset(2);
	xq_quiesce();
	check(untouched(0, 48000, 2), "quiesced: audio untouched");
	xq_resume();
	untouched(0, 48000, 2); /* the new preset warms up unheard first */
	untouched(0, 48000, 2);
	check(!untouched(0, 48000, 2), "resumed: processing again");

	/* XWEQ pass_us: the EQ's time between two xq_poll (one driver-loop pass) */
	g_host_tb_step = 100 * XQ_TB_PER_US; /* each timebase read advances 100 us */
	xq_poll();
	g_xq.pass_us = 0; /* as xbdm writes it: measure from now */
	untouched(0, 48000, 2);
	untouched(0, 48000, 2);
	xq_poll();
	u32 pu1 = g_xq.pass_us;
	xq_poll();
	u32 pu2 = g_xq.pass_us;
	g_host_tb_step = 0;
	check(pu1 == (200u << 16 | 200u) && pu2 == 200u << 16,
	      "pass_us: two blocks' time in the pass, the longest kept after an idle pass");

	/* the gate off again: untouched at once */
	g_xq.enable = 0;
	xq_poll();
	check(untouched(0, 48000, 2) && xq_count() == 0 && xq_preset() == 0,
	      "gate off while a preset plays: untouched, FLAT");

	/* reload control */
	g_xq.enable = 1;
	xq_poll();
	xq_set_preset(xq_custom_index());
	xq_set_custom(2, 4);
	u8 *f64 = with_count(file, len, 64, &l2);
	g_file = f64;
	g_flen = l2;
	g_xq.reload = 1;
	xq_poll();
	worker();
	xq_poll();
	check(g_xq.load_state == XQ_LOAD_OK && xq_count() == 64 && xq_custom_offered() &&
		      xq_preset() == 65 && xq_custom(2) == 4 && target(g_tab) == g_tab->cust->cur &&
		      g_tab->cust->cur != g_tab->flatp,
	      "reload: a 64-preset file loads; CUSTOM stays selected (now index 65), steps kept");
	free(f64);
	xq_set_custom(2, 0);

	/* refusals: FLAT only, never a crash, audio untouched */
	g_xq.enable = 1;
	boot(NULL, 0, 1);
	check(g_xq.load_state == XQ_LOAD_FAILED && (s32)g_xq.load_error == XF_E_NOFILE &&
		      xq_count() == 0 && untouched(0, 48000, 2),
	      "missing file: FLAT only, error -1");

	u8 *bad = malloc(len);
	memcpy(bad, file, len);
	bad[EQD_HDR + 77] ^= 0x10;
	boot(bad, len, 1);
	check(g_xq.load_state == XQ_LOAD_FAILED && (s32)g_xq.load_error == XQ_E_CRC &&
		      xq_count() == 0 && untouched(0, 48000, 2),
	      "bad CRC: FLAT only, error -15");

	memcpy(bad, file, len);
	put_be32(bad + 8, 44100);
	boot(bad, len, 1);
	check((s32)g_xq.load_error == XQ_E_FORMAT && xq_count() == 0, "wrong rate: refused, -13");

	memcpy(bad, file, len);
	put_be32(bad + 4, 3);
	boot(bad, len, 1);
	check((s32)g_xq.load_error == XQ_E_MAGIC && xq_count() == 0, "wrong version: refused, -12");

	memcpy(bad, file, len);
	boot(bad, EQD_HDR + n * EQD_REC - 1, 1);
	check((s32)g_xq.load_error == XQ_E_LENGTH && xq_count() == 0,
	      "truncated in the records: refused, -14");
	boot(bad, len - 1, 1);
	check((s32)g_xq.load_error == XQ_E_CRC && xq_count() == 0,
	      "truncated in the CUSTOM tables: refused (CRC), -15");

	memcpy(bad, file, len);
	bad[EQD_HDR + 4] = 'Z'; /* record 0's name no longer hashes to its id */
	put_be32(bad + 24, eqd_crc32(bad + EQD_HDR, len - EQD_HDR));
	boot(bad, len, 1);
	check((s32)g_xq.load_error == XQ_E_RECORD && xq_count() == 0,
	      "a record whose id is not its name's: refused, -17");
	free(bad);

	u8 *f65 = with_count(file, len, 65, &l2);
	boot(f65, l2, 1);
	check((s32)g_xq.load_error == XQ_E_COUNT && xq_count() == 0,
	      "65 presets (over XQ_MAX_PRESETS): refused, -16");
	free(f65);
	f65 = calloc(1, XQ_FILE_MAX + 1);
	memcpy(f65, file, len);
	boot(f65, XQ_FILE_MAX + 1, 1);
	check((s32)g_xq.load_error == XF_E_TOOBIG && xq_count() == 0,
	      "a file longer than the buffer: refused, -3");
	free(f65);
	f65 = with_count(file, len, 65, &l2);
	/* the same count in a file that would fit the buffer: the count check itself */
	put_be32(f65 + 12, 65);
	boot(f65, XQ_FILE_MAX, 1);
	check(xq_count() == 0 && g_xq.load_state == XQ_LOAD_FAILED,
	      "a header claiming 65 presets: refused");
	free(f65);

	/* a version-1 file (the one on the console's disk before this build): refused, FLAT only,
	 * logged once */
	bad = malloc(len);
	memcpy(bad, file, len);
	put_be32(bad + 4, 1);
	g_logs = 0;
	boot(bad, len, 1);
	check((s32)g_xq.load_error == XQ_E_OLD && g_xq.file_version == 1 && xq_count() == 0 &&
		      !xq_custom_offered() && untouched(0, 48000, 2) && g_logs == 2,
	      "version-1 file: refused (-19, file_version 1), FLAT only, one log (plus the gate's)");

	/* a bad CUSTOM table: the presets load, CUSTOM is not offered */
	memcpy(bad, file, len);
	{
		u32 at = EQD_HDR + n * EQD_REC + EQD_CHDR + EQD_CBANDS * EQD_CSTEPS * 20;
		bad[at + 10] = 0;
		bad[at + 11] = 0; /* a preamp of 0 */
	}
	fix_crc(bad, len);
	g_cust[1] = 2; /* a saved CUSTOM, as settings_360.c restores it */
	boot(bad, len, 1);
	g_cust[1] = 2;
	xq_set_preset(n + 1);
	check(g_xq.load_state == XQ_LOAD_OK && xq_count() == n && !xq_custom_offered() &&
		      xq_custom_index() == 0 && (s32)g_xq.custom_error == -3 && xq_preset() == 0 &&
		      !strcmp(xq_name(n + 1), "FLAT") && !strcmp(xq_desc(n + 1), ""),
	      "bad CUSTOM table: presets loaded, CUSTOM not offered (-3), its index selects FLAT");
	boot(file, EQD_HDR + n * EQD_REC, 1); /* a file without the tables (CRC fixed below) */
	memcpy(bad, file, EQD_HDR + n * EQD_REC);
	fix_crc(bad, EQD_HDR + n * EQD_REC);
	boot(bad, EQD_HDR + n * EQD_REC, 1);
	check(xq_count() == n && !xq_custom_offered() && (s32)g_xq.custom_error == -1,
	      "no CUSTOM tables: presets loaded, CUSTOM not offered (-1)");
	free(bad);

	/* steps set before the file arrives (settings_360.c at boot): built once it loads */
	g_xq.enable = 1;
	reset();
	g_file = file;
	g_flen = len;
	xq_set_custom(0, -3);
	xq_set_custom(2, 5);
	xq_init();
	xq_poll();
	check(!xq_custom_offered() && xq_custom(0) == -3, "custom: steps kept before the file loads");
	worker();
	xq_poll();
	xq_set_preset(xq_custom_index());
	{
		static u8 chg[30];
		u32 fades;
		u64 dc = custom_stream(file, len, chg, 30, &fades);
		check(xq_preset() == n + 1 && dc == 0 && g_tab->cust->cur != g_tab->flatp &&
			      target(g_tab) == g_tab->cust->cur,
		      "custom: the saved steps play once the file is there (bit-exact)");
	}

	/* no worker (the stub file_360.c): retried for 10 s, then FLAT only, logged once */
	g_worker = 0;
	g_logs = 0;
	boot(file, len, 1);
	for (int i = 0; i < 30; i++) {
		g_ms += 500;
		xq_poll();
	}
	check(g_xq.load_state == XQ_LOAD_NO_WORKER && (s32)g_xq.load_error == XF_E_NOWORKER &&
		      g_tries == XQ_POST_TRIES && g_logs == 2 && xq_count() == 0 &&
		      untouched(0, 48000, 2),
	      "no file worker: 10 tries, then FLAT only (one log, plus the gate's)");
	g_worker = 1;

	/* a file with FLAT at 0 dB: FLAT passes the bytes through exactly */
	u8 *f0 = malloc(len);
	memcpy(f0, file, len);
	put_be32(f0 + 28, 0);
	boot(f0, len, 1);
	for (int i = 0; i < 40; i++)
		sel[i] = XQ_NO_SELECT;
	sel[0] = 0;
	sel[10] = 4;
	sel[20] = 0;
	d = stream(f0, sel, 40, &flat_untouched);
	check(d == 0 && flat_untouched >= 25, "FLAT at 0 dB: untouched bytes, switches bit-exact");
	printf("     %u of 40 blocks went out untouched on FLAT\n", flat_untouched);
	free(f0);

	reset();
	printf(g_fail ? "FAIL\n" : "all ok\n");
	return g_fail;
}
