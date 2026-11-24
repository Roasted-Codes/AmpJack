// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of the game-audio tap with headphone surround: the real game_360.c and
 * surround_360.c (XG_HOST_TEST), fed float32 tap frames through a fake capture, block by
 * block through the gate and the SURROUND switch, against the Python model
 * (tools/hrtf/game_vectors.py -> tools/hrtf/xsr_game_vectors.h):
 *
 *   for t in 32 64 128; do for dev in "" -DXW_DEV; do
 *     cc -O2 -Wall -DXSR_TAPS=$t $dev -o /tmp/tgs tests/test_game_surround.c && /tmp/tgs || break 2
 *   done; done
 *
 * Checks: every block of the ring bit-exact with the model, which is today's downmix
 * (convert_frame on f2q15) wherever surround is off, so "OFF is the old downmix exactly";
 * the switch fades one block each way; the gate closes only after surround has faded out;
 * the status block's counters; and the tap path never runs the kernel with the gate off.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XG_HOST_TEST 1
#include "../core/platform.h"

typedef void *HANDLE;
static u64 g_now_ms = 1000;
static u32 g_tb;

u64 xa_now_ms(void) { return g_now_ms; }
void xa_log(const char *fmt, ...)
{
	(void)fmt;
}
long KeGetCurrentProcessType(void) { return 0; }
long XexGetModuleHandle(const char *n, HANDLE *h) { (void)n; *h = 0; return -1; }
long XexGetProcedureAddress(HANDLE m, u32 o, void **p) { (void)m; (void)o; *p = 0; return -1; }
u32 xd_timebase(void) { return g_tb += 50 * 40; } /* 40 us a call */

/* game_360.c keeps 32-bit kernel addresses in u32: fine on the console, noisy on a 64-bit host */
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#pragma GCC diagnostic ignored "-Wvoid-pointer-to-int-cast"
#include "../xbox360/game_360.c"
#include "../xbox360/surround_360.c"
#include "../tools/hrtf/xsr_game_vectors.h"
#include "../tools/demo/demo_vectors.h"

static int g_fail;
static u32 g_bits[6 * 256];

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		g_fail = 1;
}

static long fake_capture(u32 handle, void *buf)
{
	(void)handle;
	memcpy(buf, g_bits, sizeof(g_bits));
	return 0;
}

static u32 xorshift(u32 *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

/* ---- the demo: a fake file layer (file_360.h) ---------------------------------------------
 * Jobs wait in a queue until pass() runs them (the worker is "up") or not (stalled); reads
 * come from g_file, the test clip built below. */
static struct {
	void (*fn)(void *);
	void *arg;
} g_jobs[8];
static int g_njobs, g_up = 1, g_missing;
static u8 *g_file;
static u32 g_file_len;
static u32 g_fake_title = 0x1234;

bool xf_post(void (*fn)(void *), void *arg)
{
	if (g_njobs >= 8)
		return false;
	g_jobs[g_njobs].fn = fn;
	g_jobs[g_njobs].arg = arg;
	g_njobs++;
	return true;
}

int xf_read_at(const char *path, u32 offset, void *buf, u32 len, u32 *got)
{
	*got = 0;
	if (g_missing || !g_file || strcmp(path, DM_PATH))
		return XF_E_NOFILE;
	if (offset < g_file_len) {
		*got = len < g_file_len - offset ? len : g_file_len - offset;
		memcpy(buf, g_file + offset, *got);
	}
	return XF_OK;
}

static int run_jobs(void)
{
	int n = g_njobs;

	for (int i = 0; i < n; i++)
		g_jobs[i].fn(g_jobs[i].arg);
	g_njobs = 0;
	return n;
}

/* one driver pass; with the worker up, every job a poll posts runs within the pass */
static void pass(void)
{
	xsv_poll();
	while (g_up && run_jobs())
		xsv_poll();
}

static void be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

/* the test clip, as tools/demo/demo_vectors.py makes it (same draws, same bytes) */
static void build_clip(void)
{
	u32 blocks = XDV_CLIP_BLOCKS, pages = (blocks + XDM_PAGE_BLOCKS - 1) / XDM_PAGE_BLOCKS;
	u32 gs = XDV_SEED_GAIN, ss = XDV_SEED_STEM;
	s16 last[XDM_GAINS] = { 0 };

	g_file_len = XDM_HDR + pages * XDM_PAGE;
	g_file = calloc(1, g_file_len);
	be32(g_file, XDM_MAGIC);
	g_file[5] = XDM_VERSION;
	g_file[7] = XDM_HDR;
	be32(g_file + 8, 48000);
	g_file[13] = 6;
	g_file[15] = 2;
	g_file[16] = 1; /* 256 */
	g_file[19] = XDM_PAGE_BLOCKS;
	be32(g_file + 20, XDM_PAGE);
	be32(g_file + 24, pages);
	be32(g_file + 28, blocks);
	be32(g_file + 32, blocks * 256);
	be32(g_file + 60, xdm_crc32(g_file, 60));
	for (u32 p = 0; p < pages; p++) {
		u8 *pg = g_file + XDM_HDR + p * XDM_PAGE;

		be32(pg + 4, p);
		for (u32 k = 0; k < XDM_PAGE_BLOCKS; k++) {
			u8 *r = pg + 8 + k * XDM_REC;

			if (p * XDM_PAGE_BLOCKS + k >= blocks) {
				for (u32 g = 0; g < XDM_GAINS; g++) {
					r[2 * g] = (u8)(last[g] >> 8);
					r[2 * g + 1] = (u8)last[g];
				}
				continue;
			}
			for (u32 g = 0; g < XDM_GAINS; g++) {
				u32 x = xorshift(&gs);
				s16 v = (s16)(x >> 17);

				if (((x >> 3) & 3) == 0)
					v = 0;
				if (g % 6 == 3)
					v >>= 3;
				last[g] = v;
				r[2 * g] = (u8)(v >> 8);
				r[2 * g + 1] = (u8)v;
			}
			for (u32 i = 0; i < 2 * 256; i++) {
				s16 v = (s16)(s16)(xorshift(&ss) >> 16) >> 1;

				r[24 + 2 * i] = (u8)((u16)v >> 8);
				r[24 + 2 * i + 1] = (u8)v;
			}
		}
		be32(pg, xdm_crc32(pg + 4, XDM_PAGE - 4));
	}
}

static u32 g_game_seed[6];

/* the game's next frame (the model's: s16 noise x 1.5 / 32768, each channel its own stream) */
static void game_frame(void)
{
	for (int c = 0; c < 6; c++)
		for (int i = 0; i < 256; i++) {
			float f = (float)(s16)(xorshift(&g_game_seed[c]) >> 16) * 1.5f / 32768.0f;

			memcpy(&g_bits[c * 256 + i], &f, 4);
		}
}

/* one tap block; the CRC of what it put into the ring (the earpiece keeps up) */
static u32 tap_block(void)
{
	static s16 out[2 * 256];
	u32 w = g_w;

	game_frame();
	g_now_ms += 5;
	XgTapC(0);
	for (int i = 0; i < 256; i++) {
		u32 v = g_ring[(w + (u32)i) & (RING - 1)];

		out[2 * i] = (s16)(v >> 16);
		out[2 * i + 1] = (s16)v;
	}
	g_r = g_w;
	return g_w == w ? 0 : xsr_crc32(out, 512);
}

/* The demo through the real tap, block by block, against tools/demo/demo_vectors.py: a start
 * (from the game into the clip), SURROUND switched on and off on the clip, a stalled file
 * worker (the ring runs dry: silence, counted, never a wait), its return, a damaged page
 * (silence), the clip's end (back to the game), a play stopped by a title change and one by
 * the player; memory only while playing. */
static void test_demo(void)
{
	char msg[200];
	int bad = 0, bad_mode = 0;
	u32 under0, plays0, heap_max = 0, crc;

	build_clip();
	snprintf(msg, sizeof msg, "the test clip: %u bytes, crc %08x (model %08x)", g_file_len,
		 xdm_crc32(g_file, g_file_len), XDV_FILE_CRC);
	check(g_file_len == XDV_FILE_BYTES && xdm_crc32(g_file, g_file_len) == XDV_FILE_CRC, msg);
	g_file[XDM_HDR + XDV_BAD_PAGE * XDM_PAGE + 1000] ^= 0x40; /* page 5: damaged on the disk */
	for (int c = 0; c < 6; c++)
		g_game_seed[c] = XDV_SEED_GAME + (u32)c * 0x9E3779B9u;
	g_title = &g_fake_title;
	g_tap_on = 1;
	g_xsv_st.surround = 0;
	check(!xg_demo_offered() && g_xsv_st.demo_file == DM_FILE_UNKNOWN,
	      "no demo offered before the file has been looked for");
	g_xsv_st.enable = 1;
	under0 = g_xsv_st.demo_underruns;
	plays0 = g_xsv_st.demo_plays;
	for (int b = 0; b < XDV_BLOCKS; b++) {
		u32 a = xdv_act[b], pos0 = g_dm_pos, u0 = g_xsv_st.demo_underruns, m = xdv_mode[b];
		bool ok;

		if (a & XDV_SURR_ON)
			g_xsv_st.surround = 1;
		if (a & XDV_SURR_OFF)
			g_xsv_st.surround = 0;
		if (a & XDV_STALL)
			g_up = 0;
		if (a & XDV_RESUME)
			g_up = 1;
		if (a & XDV_START) {
			xg_demo_start();
			for (int i = 0; i < 10 && g_xsv_st.demo_state != DM_ST_PLAYING; i++)
				pass();
		}
		if (a & XDV_STOP)
			xg_demo_stop();
		if (a & XDV_TITLE)
			g_fake_title++;
		pass();
		if (g_xsv_st.demo_heap > heap_max)
			heap_max = g_xsv_st.demo_heap;
		crc = tap_block();
		if (crc != xdv_crc[b]) {
			if (bad++ < 5)
				printf("  block %d (mode %u, surround %u): crc %08x, model %08x\n", b, m,
				       g_xsv_st.surround, crc, xdv_crc[b]);
		}
		/* what the tap did, from what it left behind */
		ok = m == XSV_DM_NONE ? g_dm_pos == pos0 && g_xsv_st.demo_underruns == u0
		     : m == XSV_DM_IN ? g_dm_cur && g_dm_faded
		     : m == XSV_DM_PLAY ? g_dm_cur && (g_dm_pos == pos0 + 1) != (g_xsv_st.demo_underruns == u0 + 1)
					 : !g_dm_cur && g_dm_over;
		if (!ok && bad_mode++ < 5)
			printf("  block %d: the tap did not do mode %u (pos %u -> %u, cur %u)\n", b, m, pos0,
			       g_dm_pos, g_dm_cur);
	}
	snprintf(msg, sizeof msg, "%d blocks bit-exact with the model (game, start, clip with "
		 "surround off / on / switching, dry ring, damaged page, end, title change, stop): "
		 "%d differ, %d in another mode", XDV_BLOCKS, bad, bad_mode);
	check(!bad && !bad_mode, msg);
	pass();
	snprintf(msg, sizeof msg, "underruns %u (model %u), plays %u (%u), damaged pages %u (3: page 5 is read ahead by every play), "
		 "reads %u, longest read %u us", g_xsv_st.demo_underruns - under0, XDV_UNDERRUNS,
		 g_xsv_st.demo_plays - plays0, XDV_PLAYS, g_xsv_st.demo_bad_pages, g_xsv_st.demo_reads,
		 g_xsv_st.demo_read_max_us);
	check(g_xsv_st.demo_underruns - under0 == XDV_UNDERRUNS &&
		      g_xsv_st.demo_plays - plays0 == XDV_PLAYS && g_xsv_st.demo_bad_pages == 3 &&
		      g_xsv_st.demo_reads > 0, msg);
	snprintf(msg, sizeof msg, "memory: %u bytes while playing (<= 65536), %u after; state %u, "
		 "last end %u (stopped)", heap_max, g_xsv_st.demo_heap, g_xsv_st.demo_state,
		 g_xsv_st.demo_end);
	check(heap_max == sizeof(struct dm_mem) && heap_max <= 65536 && !g_xsv_st.demo_heap && !g_dm &&
		      g_xsv_st.demo_state == DM_ST_IDLE && g_xsv_st.demo_end == DM_END_STOPPED, msg);
	check(xg_demo_offered() && g_xsv_st.demo_file == DM_FILE_PRESENT &&
		      g_xsv_st.demo_blocks == XDV_CLIP_BLOCKS,
	      "the file was found when the gate went on: offered, its length read");
}

/* start .. playing (the worker up); false if it does not get there */
static bool demo_play(void)
{
	xg_demo_start();
	for (int i = 0; i < 10 && g_xsv_st.demo_state != DM_ST_PLAYING; i++)
		pass();
	return g_xsv_st.demo_state == DM_ST_PLAYING;
}

/* the ways a play ends besides the schedule's, and finding the file */
static void test_demo_edges(void)
{
	u32 secs;
	bool ok;

	/* no game audio reaching the headset: stopped after 2 s, freed once the tap is gone */
	g_up = 1;
	ok = demo_play();
	tap_block();
	tap_block();
	g_tap_on = 0;
	for (int i = 0; i < 30; i++) {
		g_now_ms += 100;
		pass();
	}
	check(ok && g_xsv_st.demo_state == DM_ST_IDLE && g_xsv_st.demo_end == DM_END_NO_AUDIO &&
		      !g_dm && !g_dm_cur && !xg_demo_playing(&secs),
	      "no game audio for 2 s: stopped and freed (the tap was inside the clip)");
	g_tap_on = 1;
	pass();

	/* surround's gate off while playing: the clip fades out first, then the gate closes */
	ok = demo_play();
	tap_block();
	tap_block();
	g_xsv_st.enable = 0;
	pass();
	ok &= g_xsv_tap == 1 && g_dm_cur; /* still open: the tap has not faded out yet */
	tap_block();
	pass();
	check(ok && !g_dm_cur && g_xsv_tap == 0 && g_xsv_st.demo_end == DM_END_GATE && !g_dm,
	      "gate off: the clip fades out, then the gate closes, memory freed");
	xg_demo_start();
	check(g_xsv_st.demo_state == DM_ST_IDLE, "no start with the gate off");
	g_xsv_st.enable = 1;
	pass();

	/* xbdm: demo_cmd 1 starts, 2 stops; xg_demo_playing counts the seconds */
	g_xsv_st.demo_cmd = 1;
	for (int i = 0; i < 10 && g_xsv_st.demo_state != DM_ST_PLAYING; i++)
		pass();
	for (int i = 0; i < 60; i++) {
		tap_block();
		pass();
	}
	ok = xg_demo_playing(&secs) && g_xsv_st.demo_pos == 60 && secs == 60 * 2 / 375 &&
	     !g_xsv_st.demo_cmd;
	g_xsv_st.demo_cmd = 2;
	pass();
	tap_block();
	pass();
	check(ok && g_xsv_st.demo_state == DM_ST_IDLE && g_xsv_st.demo_end == DM_END_STOPPED,
	      "xbdm demo_cmd: 1 starts, 2 stops; the blocks played and seconds shown");

	/* hot reload: quiesced mid-play with the tap gone -> freed; no new start until resumed */
	ok = demo_play();
	tap_block();
	xsv_quiesce(true);
	ok &= !g_dm && g_xsv_st.demo_state == DM_ST_IDLE && g_xsv_st.demo_end == DM_END_QUIESCED;
	g_dm_cur = 0; /* (the tap never runs again after a quiesce; this test's tap would) */
	xg_demo_start();
	ok &= g_xsv_st.demo_state == DM_ST_IDLE;
	xsv_resume();
	ok &= demo_play();
	/* ... and with a read in flight: the ring stays (the worker is writing it) */
	g_up = 0;
	for (int i = 0; i < 20; i++)
		tap_block(), pass();
	xsv_quiesce(false);
	ok &= g_dm != NULL && g_xsv_st.demo_state == DM_ST_STOPPING;
	g_up = 1;
	xsv_resume();
	pass();
	tap_block();
	pass();
	check(ok && !g_dm && g_xsv_st.demo_state == DM_ST_IDLE,
	      "quiesce: freed when nothing can use the ring, kept while a read is in flight, freed "
	      "after; no start while quiesced");

	/* the file: missing -> not offered; looked for again after 30 s; a damaged header */
	g_missing = 1;
	g_xsv_st.demo_file = DM_FILE_UNKNOWN;
	pass();
	ok = g_xsv_st.demo_file == DM_FILE_MISSING && !xg_demo_offered();
	g_missing = 0;
	g_now_ms += 10000;
	pass();
	ok &= g_xsv_st.demo_file == DM_FILE_MISSING;
	g_now_ms += 21000;
	pass();
	ok &= g_xsv_st.demo_file == DM_FILE_PRESENT && xg_demo_offered();
	g_file[30] ^= 1;
	xg_demo_start();
	for (int i = 0; i < 5; i++)
		pass();
	ok &= g_xsv_st.demo_file == DM_FILE_BAD && g_xsv_st.demo_end == DM_END_FILE &&
	      g_xsv_st.demo_state == DM_ST_IDLE && !xg_demo_offered();
	g_file[30] ^= 1;
	check(ok, "file: missing is not offered and is looked for again after 30 s; a damaged "
		  "header is refused at the start");
}

int main(void)
{
	static u32 seeds[6];
	static s16 out[2 * 256];
	char msg[160];
	int bad_blocks = 0;

	for (int c = 0; c < 6; c++)
		seeds[c] = XGV_SEED + (u32)c * 0x9E3779B9u;
	xg_init();
	check(g_xsv_st.state == XSV_ST_GATE_OFF && g_m && g_xsv_st.heap_bytes == sizeof(*g_m),
	      "init: state allocated, gate off");
	pCapture = fake_capture;
	g_handle = 0x801960ac;
	g_tap_on = 1;
	for (int b = 0; b < XGV_BLOCKS; b++) {
		u32 blocks_before = g_xsv_st.blocks, crc, w = g_w;

		/* the frame: s16 noise x 1.5 / 32768 as float32, specials in block 0 */
		for (int c = 0; c < 6; c++)
			for (int i = 0; i < 256; i++) {
				float f = (float)(s16)(xorshift(&seeds[c]) >> 16) * 1.5f / 32768.0f;

				memcpy(&g_bits[c * 256 + i], &f, 4);
			}
		if (b == 0)
			memcpy(g_bits, xgv_special, sizeof(xgv_special));
		g_xsv_st.enable = xgv_gate[b];
		g_xsv_st.surround = xgv_on[b];
		xsv_poll();
		XgTapC(0);
		for (int i = 0; i < 256; i++) {
			u32 v = g_ring[(w + (u32)i) & (RING - 1)];

			out[2 * i] = (s16)(v >> 16);
			out[2 * i + 1] = (s16)v;
		}
		crc = xsr_crc32(out, 512);
		if (crc != xgv_crc[b]) {
			printf("  block %d (gate %d, surround %d): crc %08x, model %08x\n", b, xgv_gate[b],
			       xgv_on[b], crc, xgv_crc[b]);
			bad_blocks++;
		}
		if (!xgv_gate[b] && !g_xsv_tap && g_xsv_st.blocks != blocks_before) {
			printf("  block %d: the kernel ran with the gate off\n", b);
			bad_blocks++;
		}
	}
	snprintf(msg, sizeof msg, "%d blocks bit-exact with the model (gate off / on, surround "
		 "off / on / switching): %d differ", XGV_BLOCKS, bad_blocks);
	check(bad_blocks == 0, msg);
	check(g_w == XGV_BLOCKS * 256 && g_xg.tap_frames == XGV_BLOCKS && g_xg.bad_floats == 1,
	      "the ring advanced one block per call; the NaN was counted once");
	snprintf(msg, sizeof msg, "virtualized %u blocks, %u switches (model: 8 and 4)",
		 g_xsv_st.blocks, g_xsv_st.switches);
	check(g_xsv_st.blocks == 8 && g_xsv_st.switches == 4, msg);
	xsv_poll();
	check(!g_xsv_tap && !g_cur && g_xsv_st.state == XSV_ST_GATE_OFF,
	      "the gate closed after surround faded out");
	check(g_xsv_st.tap_us == 40 && g_xsv_st.tap_max_us == 40, "tap time from the timebase (40 us a call)");
	{
		/* a second of blocks with the gate on: the peaks are published, and 5.1 is live;
		 * then a second with C, SL and SR silent: stereo, not live */
		g_xsv_st.enable = 1;
		g_xsv_st.surround = 0;
		for (int pass = 0; pass < 2; pass++) {
			for (int b = 0; b < 2 * 188; b++) { /* the second is a whole window */
				for (int k = 0; k < 6 * 256; k++) {
					int c = k / 256;
					float f = (float)(s16)(xorshift(&seeds[c]) >> 16) / 32768.0f;

					if (pass && (c == XSR_C || c == XSR_SL || c == XSR_SR))
						f = 0;
					memcpy(&g_bits[k], &f, 4);
				}
				xsv_poll();
				g_r = g_w; /* an earpiece that keeps up */
				XgTapC(0);
			}
			xsv_poll();
			snprintf(msg, sizeof msg, "second %d: peaks C %u SL %u SR %u, live %u", pass + 1,
				 g_xsv_st.ch_peak[XSR_C], g_xsv_st.ch_peak[XSR_SL],
				 g_xsv_st.ch_peak[XSR_SR], g_xsv_st.live);
			check(pass ? !xg_surround_live() && g_xsv_st.ch_peak[XSR_C] == 0
				   : xg_surround_live() && g_xsv_st.ch_peak[XSR_C] > 30000, msg);
		}
		g_xsv_st.enable = 0;
		xsv_poll();
	}
#ifdef XW_DEV
	{
		/* the probe: test_ch 3 puts bursts into C only; the dummy load runs in the tap */
		g_xsv_st.enable = 1;
		g_xsv_st.test_ch = 3;
		g_xsv_st.load_convs = 5;
		g_xsv_st.load_taps = 64;
		for (int b = 0; b < 2 * 188; b++) {
			xsv_poll();
			g_r = g_w;
			XgTapC(0);
		}
		snprintf(msg, sizeof msg, "probe: test_ch 3 -> peaks FL %u C %u SR %u; dummy load ran "
			 "%u blocks", g_xsv_st.ch_peak[XSR_FL], g_xsv_st.ch_peak[XSR_C],
			 g_xsv_st.ch_peak[XSR_SR], g_xsv_st.load_blocks);
		check(g_xsv_st.ch_peak[XSR_FL] == 0 && g_xsv_st.ch_peak[XSR_SR] == 0 &&
			      g_xsv_st.ch_peak[XSR_C] > 3000 && g_xsv_st.ch_peak[XSR_C] <= 4096 &&
			      g_xsv_st.load_blocks == 2 * 188,
		      msg);
		g_xsv_st.test_ch = 0;
		g_xsv_st.load_convs = 0;
		g_xsv_st.enable = 0;
		xsv_poll();
	}
#endif
	xg_set_surround(7);
	check(xg_surround() == 1 && !xg_surround_offered() && !xg_surround_live(),
	      "xg_set_surround clamps to 1; unavailable with the gate off; not live");
	test_demo();
	test_demo_edges();
	printf("%s\n", g_fail ? "FAILED" : "all passed");
	return g_fail;
}
