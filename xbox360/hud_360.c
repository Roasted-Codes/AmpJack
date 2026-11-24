// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The on-screen headset panel (hud_360.h; research/HUD.md; docs/PLAN.md WP1.1).
 *
 * While the controls say so (hud_state.visible), a panel is written straight into the picture
 * the console is about to show: the game's, Aurora's or the dashboard's front buffer. What
 * the panel looks like is hud_panel.c's (hp_build: a canvas of palette indices, WP1.2); this
 * file owns the hook, the swap records, the fetch decode, the tiling, the blit, the flush and
 * the status block.
 *
 * Where the picture comes from. D3D presents a frame by calling the kernel's VdSwap (17559:
 * ordinal 603, 0x800C8B90) with the front buffer's texture fetch constant (r4: address,
 * pitch, size, format, endian, tiling) and a writeback fence (r5 = address, r7 = value: the
 * GPU writes the value there once it has processed the swap, 0x800C8CA4). VdSwap only queues
 * GPU commands: when it runs, the GPU usually hasn't resolved that frame into the front
 * buffer yet, so pixels written then would be overwritten. So:
 *
 *   - the VdSwap detour (the title's render thread) only records the call: fetch constant,
 *     fence, title, timebase; a few dozen stores into a ring, no calls, no locks (seqlock
 *     per slot);
 *   - the driver thread (hud_poll, once per pass of its loop: ~16.6 ms on the console, the
 *     kernel's clock tick, research/HUD.md 6) picks the newest recorded swap whose fence the
 *     GPU has written: that frame is resolved and on screen (or will be at the next vblank)
 *     and nothing renders into it until the next swap. It draws the panel into it once, then
 *     flushes the touched cache lines (dcbf) for the display to read.
 *
 * The canvas (up to HP_W x HP_H_MAX units; its height depends on the rows shown) is rebuilt
 * when the published state changes and drawn at an integer scale (2 at 720p, 3 at 1080p,
 * stepped down to fit the 80 % title-safe area, 1 at the least) where its anchor says
 * (geometry()): the panel and the toasts at the bottom centre, bottom edge on the 10 % margin;
 * the persistent mute mark (HUD_BADGE, no background) in the band below that, or a corner
 * (badge_pos). The canvases live in one allocation made by hud_init (struct hud_mem); without
 * it nothing is drawn. The background is opaque by default (keep = 0): the only
 * reads of the front buffer are the 4 sentinel texels. With keep > 0 it is blended with
 * integer math (a read per background pixel). Four text pixels (hp_sentinels) carry slightly
 * altered colours: if they are already there, the frame was swapped again without a new
 * resolve, and the panel is not drawn again (keep 0) or repainted opaque (keep > 0).
 *
 * Formats: k_8_8_8_8 and k_2_10_10_10 (and their AS_16_16_16_16 variants), any of the four
 * endian modes, channel order from the fetch swizzle, tiled or linear. Anything else is
 * skipped and counted. Every page the panel touches is checked with MmIsAddressValid
 * before the first write, and every offset is bounded by the buffer's size.
 *
 * Stages (g_xh.enable, written over xbdm: hud.py stage N), 0 at boot until WP1.1's console
 * bring-up has passed:
 *   0  nothing: VdSwap is not looked at (once hooked: the hook passes straight through)
 *   1  check only: resolve ordinal 603 and compare 23 code words with the 17559 dump
 *      (sig_ok); nothing is patched
 *   2  hook VdSwap (if sig_ok) and record swaps; never draw
 *   3  draw allowed (when the controls, or hud.py show, make the panel visible)
 * Lowering the stage never unpatches: below 2 the hook only returns the trampoline.
 *
 * Hot reload (hud_360.h, docs/HOT-RELOAD.md): hud_quiesce makes the hook record nothing and
 * only return the trampoline, and hud_poll do nothing, until hud_resume. The hook stays
 * patched; the next build adopts its slot (detour_install).
 *
 * No floating point, no .bss (build.sh), the kernel enters only through the asm stub below.
 */
#ifndef HUD_HOST_TEST
#include <xecore/xboxkrnl.h>
#else
typedef void *HANDLE; /* tests/test_panel.c: no kernel; pExeModule stays NULL (title 0) */
#endif
#include <stdlib.h>

#include "../core/platform.h"
#include "detour.h"
#include "hud_360.h"
#include "hud_panel.h"

#ifndef HUD_HOST_TEST
#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#define SYNC() __asm__ __volatile__("sync" ::: "memory")
#define DCBF(p) __asm__ __volatile__("dcbf 0, %0" : : "r"(p) : "memory")
/* SMT thread priority hints (no-ops where unsupported): low while spinning, then medium */
#define SMT_LOW() __asm__ __volatile__("or 1, 1, 1")
#define SMT_MEDIUM() __asm__ __volatile__("or 2, 2, 2")
static inline u32 ticks(void)
{
	u32 t;

	__asm__ __volatile__("mftb %0" : "=r"(t));
	return t;
}
#define PTR(va) ((u8 *)(va))
#define LD(p) (*(p))
#define ST(p, v) (*(p) = (v))
#else
#define BARRIER() __sync_synchronize()
#define SYNC() __sync_synchronize()
#define DCBF(p) ((void)(p))
#define SMT_LOW() ((void)0)
#define SMT_MEDIUM() ((void)0)
extern u32 hud_host_ticks;
static u32 ticks(void) { return hud_host_ticks; }
/* the host test maps a fake "physical" range onto its own buffer */
extern u8 *hud_host_mem;
#define HUD_HOST_VA 0xC0000000u
#define PTR(va) (hud_host_mem + ((va) - HUD_HOST_VA))
/* the 360 is big-endian: the host test reads and writes words the way it does */
#define LD(p) __builtin_bswap32(*(p))
#define ST(p, v) (*(p) = __builtin_bswap32(v))
#endif
#define HOOK __attribute__((used, noinline))
#define TB_PER_US 50 /* the timebase: 50 MHz */

/* ---- kernel 17559 ---------------------------------------------------------------------------- */
#define ORD_VdSwap 603
#define ORD_XexExecutableModuleHandle 403
#define ADDR_VdSwap 0x800C8B90
struct sig {
	u32 addr, n;
	const u32 *w;
};
/* the prologue the detour replaces (mflr r12; bl __savegprlr; stwu; lis r11) and the code
 * whose meaning the HUD relies on, word for word from the 17559 dump */
static const u32 SIG_ENTRY[4] = { 0x7d8802a6, 0x48044021, 0x9421ff40, 0x3d608017 };
/* mr r30, r6 (the swap info) ... mr r31, r3 / mr r28, r5 (fence address) / mr r24, r7
 * (fence value) / mr r27, r8 */
static const u32 SIG_ARGS[7] = { 0x7cde3378, 0x3bab0e70, 0x39600000, 0x7c7f1b78,
				 0x7cbc2b78, 0x7cf83b78, 0x7d1b4378 };
/* lwz r11, 0(r30); ...; cmplwi r11, 0; beq (no fetch); addi r3, r1, 0x50; li r5, 0x18
 * (memcpy of the 6-word fetch constant from r4) */
static const u32 SIG_INFO[1] = { 0x817e0000 };
static const u32 SIG_COPY[4] = { 0x2b0b0000, 0x419a00d0, 0x38610050, 0x38a00018 };
/* the fence packet: physical address of r28, EVENT_WRITE_SHD (type 3, opcode 0x58) */
static const u32 SIG_FENCE[7] = { 0x578b653e, 0x931f000c, 0x578a00fe, 0x396b0200,
				  0x3d20c002, 0x556b04e6, 0x61295800 };
static const struct sig SIGS[] = {
	{ 0x800C8B90, 4, SIG_ENTRY }, { 0x800C8BA0, 7, SIG_ARGS }, { 0x800C8BC4, 1, SIG_INFO },
	{ 0x800C8BD0, 4, SIG_COPY },  { 0x800C8CA4, 7, SIG_FENCE },
};

#define STALE_MS 250        /* a recorded swap older than this is never drawn */
#define STAGE_MAX 3

/* ---- the status and control block, read and written over xbdm (hud.py) -----------------------
 * All u32, in this order: hud.py's FIELDS mirror it. Append only. The first 62 words keep
 * version 1's positions (xw.py tee reads enable 2, force 3, hooked 14, error 15, heartbeat
 * 17, swaps 20, draws 23; shot.py reads the d_* fields by name). */
struct xh_status {
	u32 magic, version;
	/* controls (xbdm writes) */
	volatile u32 enable;   /* the stage, 0..3 (above) */
	volatile u32 force;    /* 0 normal, 1 show the test_* state, 2 never draw */
	volatile u32 mode;     /* 0 draw once the GPU fence passes, 1 at once, 2 the previous swap */
	volatile u32 keep;     /* background kept by the panel, /256; 0 = opaque, write-only */
	volatile u32 swap_rb;  /* 1: swap red and blue (if the accent comes out wrong) */
	/* force 1: the state shown (struct hud_state v2 fields; the rest are at the end) */
	volatile u32 test_muted, test_balance, test_volume, test_sidetone;
	volatile u32 test_game_on, test_headset, test_flash;
	/* state */
	u32 hooked, error, now_ms, heartbeat;
	u32 publishes, pub_visible, swaps, swaps_fetch;
	u32 fence_same, draws, repaints, skip_format;
	u32 skip_title, skip_unmapped, skip_stale, fence_waits;
	u32 last_skip;
	/* the last recorded swap */
	u32 sw_fetch[6];
	u32 sw_info[7];
	u32 sw_fence_addr, sw_fence_val, sw_fence_now, sw_title;
	/* the last draw */
	u32 d_base, d_width, d_height, d_pitch;
	u32 d_format, d_endian, d_tiled, d_swizzle;
	u32 d_scale, d_x0, d_y0, d_ticks;
	volatile u32 d_ticks_max; /* xbdm may write 0: measure from now */
	u32 d_pixels;
	/* ---- version 2 (WP1.1) ---- */
	u32 stage;             /* the stage acted on (enable as last seen by hud_poll) */
	u32 sig_ok;            /* 0 not checked, 1 the 23 words match, 2 refused (error = where) */
	u32 sig_addr;          /* VdSwap's address from ordinal 603 */
	u32 install_ms;        /* ms after hud_init that the hook went in; 0 = not hooked */
	u32 quiesced;          /* hot reload: handed over (hud_quiesce) */
	u32 swaps_per_s, draws_per_s; /* over the last whole second */
	u32 cover_pct;         /* draws / swaps x 100 over the last second the panel was up */
	u32 vis_swaps, vis_draws; /* the same, cumulative: swaps with a picture while the panel
				   * was up at stage 3, and those drawn (or found intact) */
	u32 fence_late_us;     /* the fence seen passed -> draw start, last draw */
	volatile u32 fence_late_max_us;
	u32 swap_late_us;      /* VdSwap called -> draw start, last draw */
	volatile u32 swap_late_max_us;
	u32 d_hist[4];         /* draw time (d_ticks): < 0.25, < 0.5, < 1, >= 1 ms */
	u32 titles_seen;       /* title changes seen in the swaps (the first title counts) */
	u32 tt_next;           /* (title, format) pairs seen; the newest is tt_*[(tt_next-1)&3] */
	u32 tt_title[4];       /* title module handle */
	u32 tt_fmt[4];         /* format | endian << 8 | tiled << 10 | pitch (texels) << 16 */
	u32 tt_size[4];        /* width << 16 | height */
	u32 sent_n;            /* sentinels in the last draw (4, or 0 if the canvas had none) */
	u32 sent_x[4], sent_y[4]; /* their screen pixels (the top-left pixel of the unit) */
	u32 sent_rgb[4];       /* their colours, native depth (8 or 10 bits a lane), r g b */
	u32 canvas_w, canvas_h; /* the last canvas (units) */
	u32 d_w, d_h;          /* the last draw's panel size (pixels) */
	u32 intact;            /* draws found already there (sentinels): not drawn again */
	u32 skip_canvas;       /* nothing to draw (an empty or oversized canvas, or no fit) */
	u32 poll_us;           /* hud_poll's own time, last pass */
	volatile u32 poll_max_us;
	/* force 1: the rest of the test state (controls) */
	volatile u32 test_visible;  /* HUD_TOAST, or HUD_PANEL (anything else) */
	volatile u32 test_cursor, test_rows, test_mic_level, test_gate, test_tone;
	volatile u32 test_mic_vu, test_gate_open, test_team_level, test_channel;
	volatile u32 test_team_peers, test_team_talking;
	/* ---- version 3 (second console run): where the front buffer comes from ----
	 * Most VdSwap calls carry no picture (info[0] = 0: the kernel only queues the fence;
	 * seen at Aurora and in Halo 3), so the buffer on screen after a swap is taken from
	 * (FS_OWN) the swap's own fetch constant, else (FS_PTR) the 6 words its r4 points to,
	 * read by the driver thread (D3D passes it on every call; VdSwap only reads it with a
	 * picture), else (FS_LAST) the last swap with a picture of the same title. */
	volatile u32 src;      /* 0 auto (own, r4, last), 1 own or last, 2 own or r4, 3 own or r4 with
				* the base from *r8, 4 auto with the base from the scan-out register */
	u32 sw_src;            /* FS_* of the last swap */
	u32 sw_ptr[4];         /* the last swap's r4 (fetch), r8, r9, r10 (out pointers) */
	u32 sw_out[3];         /* *r8, *r9, *r10, copied by the hook (0 if unmapped) */
	u32 src_n[4];          /* swaps by source: none, own, pointer, last picture */
	u32 pic_fetch[6];      /* the last swap with a picture (its own fetch constant) */
	u32 pic_title;
	/* ---- version 4 (third console run) ----
	 * r4 and r8 point into the caller's stack (D3D builds the fetch constant there per
	 * call): the hook copies them at call time; read later they are another frame's. */
	volatile u32 order;    /* tiled blit: 0 in memory order (whole 128-byte lines), 1 by rows */
	volatile u32 noflush;  /* 1: no dcbf after the blit */
	u32 ptr_agree, ptr_differ; /* swaps whose *r8 equals / differs from the r4 fetch's base */
	u32 d_blit_us, d_flush_us; /* the last draw: the stores, and the dcbf + sync */
	/* ---- version 5 (fourth console run): the surface the display scans out ----
	 * The display controller's D1GRPH_PRIMARY_SURFACE_ADDRESS (0x7FC86110: physical), read
	 * and mapped the way xbdm's screenshot does (the first valid of +0xC0000000,
	 * +0xA0000000, +0xE0000000 - 0x1000 whose MmGetPhysicalAddress comes back the same). */
	volatile u32 alias;    /* the CPU mapping drawn through: 0 the swap's own address (src 4: as
				* xbdm, the first of C, A, E), 1 E window (4 KB pages), 2 A (64 KB),
				* 3 C (16 MB); same physical memory, maybe other caching */
	u32 scan_phys, scan_va, scan_ctl; /* the register, its mapping, D1GRPH_CONTROL's top byte */
	u32 scan_agree, scan_differ; /* swaps whose buffer is / isn't (physically) the scan-out */
	u32 d_gap_max_us;      /* the last draw: the longest pause between two lines' stores */
	u32 d_gaps;            /* lines started more than 1 ms after the one before (all draws) */
	volatile u32 wide;     /* tiled blit: 1 = two texels per 8-byte store (default), 0 = one */
	u32 d_va;              /* the last draw's CPU address (d_base after the alias choice) */
	u32 skip_alias;        /* draws skipped: the chosen alias isn't mapped */
	/* ---- version 6 (fifth console run): the draw thread (docs/PLAN.md WP1.1 fallback) ----
	 * The driver thread lost hardware thread 2 for ~15.6 ms in the middle of draws, so the
	 * panel landed after the title had moved on. With thread 1 a system thread of its own
	 * (XhDrawThread, on hardware thread thr_hw) does the swap ingest, the pick and the draw;
	 * the VdSwap hook wakes it (KeSetEvent) at every swap; hud_poll keeps the stages. */
	volatile u32 thread;   /* 0 draw on the driver thread (hud_poll), 1 on the draw thread */
	volatile u32 thr_hw;   /* its hardware thread, read when it is created: 0, 1, 3 or 5 */
	volatile u32 thr_prio; /* its priority (1..15; the driver thread runs at 9) */
	volatile u32 spin_us;  /* after a wake with no swap ready: watch the fence this long */
	u32 thr_state;         /* 0 not created, 1 running (thr_hw_used), 2 creation failed */
	u32 thr_hw_used, thr_prio_now;
	u32 thr_wakes;         /* wakes that did work */
	u32 thr_heartbeat;     /* every wake or 100 ms timeout */
	u32 thr_spins, thr_spin_hits; /* spins, and those that saw a fence pass */
	u32 thr_spin_us_max;
	/* ---- version 7 (sixth console run: half a panel, the draw thread still paused ~15.6 ms
	 * mid-draw at priority 9; asking 15 left it at 9) ---- */
	u32 thr_prio_base, thr_prio_max; /* its KTHREAD base priority (+0xB9) and ceiling (+0xCA) */
	volatile u32 bg;       /* panel background: 1 opaque (default), 0 none (text and shapes
				* only), 2 only within one unit of them (a halo) */
	volatile u32 retry_us; /* a pause longer than this mid-draw restarts the draw (up to 3
				* times; 0 = never) */
	u32 d_restarts;        /* draws restarted after a pause (all draws) */
	u32 d_tries;           /* the last draw's attempts */
	u32 d_paused_us;       /* the last draw: time lost in pauses (> 200 us between lines) */
	u32 d_work_us;         /* the last draw: the stores themselves (d_blit_us - d_paused_us) */
	u32 d_ink_px;          /* the last draw: pixels written */
	/* ---- version 8 (seventh console run: fast, whole draws, but the panel in only 15-30 %
	 * of screenshots). A swap's r4 buffer is the one on screen when VdSwap is called (98.5 %
	 * of swaps, scan_agree); its fence passes ~33 ms later. Drawing only at swap wakes
	 * misses the scan-out's display period, and when xbdm stops the title for a screenshot
	 * no more swaps wake the thread. ---- */
	volatile u32 ensure;   /* 1 (default): every pass, if the buffer being scanned out is one
				* of the title's front buffers and lacks this panel, draw it there */
	volatile u32 idle_ms;  /* the draw thread's wait when no swap wakes it (default 20) */
	u32 ens_checks, ens_draws, ens_skips; /* ensure: looked, drew, scan-out not a known buffer */
	volatile u32 trace;    /* xbdm writes N (1..64): record the next N events into tr[];
				* + 0x80: wakes too */
	u32 tr_n;              /* events recorded since trace was armed */
	u32 tr[64 * 4];        /* {timebase, kind, a, b}: hud.py trace */
	/* ---- version 9 (eighth run: the owner sees it, with flicker). One scan-out buffer, a
	 * swap every 33 ms, the title resolving into the buffer on screen; the panel is gone
	 * from that resolve until our next draw. ensure now samples, at every pass (idle_ms
	 * default 2), whether the panel is in the scan-out, and redraws it at once if not. */
	u32 pres_yes, pres_no; /* ensure samples: the panel there / not there (then redrawn) */
	u32 pres_pct;          /* the share of samples with the panel there, last second */
	u32 gap_last_us;       /* the last gap: last sample with the panel -> redrawn */
	volatile u32 gap_max_us;
	/* ---- version 10: hud.py show's keys for hud_360.h version 3 (the MIC TEST row) */
	volatile u32 test_rows_hi, test_mictest, test_mictest_secs, test_mictest_pct;
	/* ---- version 11: hud.py show's keys for hud_360.h version 4, and where the mute mark
	 * goes ---- */
	volatile u32 test_mic_auto_level, test_preset, test_preset_count, test_surround;
	volatile u32 test_surround_live, test_badge, test_badge_shift, test_mictest_max;
	volatile u32 test_preset_name[5]; /* hud_state.preset_name's 20 bytes as they lie in
					   * memory: big-endian words, first character first */
	volatile u32 badge_pos;  /* the mute mark (HP_ANCHOR_BADGE): 0 bottom centre, between the
				  * 10 % and 4 % margins (default); 1 bottom left, 2 bottom right,
				  * 3 top left, 4 top right; anything else as 0 */
	/* ---- version 12: the draw thread's wait while only the mute mark is up (in place of
	 * idle_ms; the mark may stay up for a whole match, so S2 can trade its repaint gap
	 * against the wakes on hardware thread 1: accept/badge.py) ---- */
	volatile u32 badge_idle_ms;
	/* ---- version 13: hud.py show's keys for hud_360.h version 5 (CUSTOM, the description) */
	volatile u32 test_preset_flags, test_custom_bass, test_custom_mids, test_custom_treble;
	volatile u32 test_unused476; /* version 13: test_desc_lines; version 14 reads nothing here
				      * (no description area: desc is the help line on PRESET) */
	volatile u32 test_desc[HUD_DESC_LEN / 4]; /* hud_state.desc's bytes as they lie in memory:
						   * big-endian words, first character first */
	/* ---- version 15: hud.py show's keys for hud_360.h version 7 (SURROUND DEMO) */
	volatile u32 test_demo, test_demo_secs;
};
_Static_assert(sizeof(struct xh_status) == (239 + 256) * 4, "hud.py FIELDS");
_Static_assert(__builtin_offsetof(struct xh_status, test_demo) == 493 * 4, "hud.py FIELDS v15");
_Static_assert(__builtin_offsetof(struct xh_status, test_mic_auto_level) == 457 * 4 &&
	       __builtin_offsetof(struct xh_status, badge_pos) == 470 * 4 &&
	       __builtin_offsetof(struct xh_status, badge_idle_ms) == 471 * 4, "hud.py FIELDS v12");
_Static_assert(__builtin_offsetof(struct xh_status, test_preset_flags) == 472 * 4 &&
	       __builtin_offsetof(struct xh_status, test_desc) == 477 * 4, "hud.py FIELDS v13");
_Static_assert(__builtin_offsetof(struct xh_status, hooked) == 14 * 4, "xw.py tee XH_WATCH");
_Static_assert(__builtin_offsetof(struct xh_status, heartbeat) == 17 * 4, "xw.py tee XH_HB");
_Static_assert(__builtin_offsetof(struct xh_status, draws) == 23 * 4, "xw.py tee XH_DRAWS");
_Static_assert(__builtin_offsetof(struct xh_status, d_pixels) == 61 * 4, "v1 prefix");

enum { HK_NONE, HK_INSTALLED, HK_REFUSED, HK_FAILED };
enum { FS_NONE, FS_OWN, FS_PTR, FS_LAST };
enum { SK_NONE, SK_TYPE, SK_FORMAT, SK_SIZE, SK_ADDR, SK_SWIZZLE, SK_UNMAPPED, SK_TITLE,
       SK_CANVAS };

__attribute__((used)) struct xh_status g_xh = {
	.magic = 0x58483336, /* "XH36" */
	.version = 15,       /* 2: 127 words (306a69a); 3: 147 (c65287a); 4: 153 (71863c1); 5: 164
			      * (6455fd5); 6: 176 (d90b9de); 7: 185 (6672dbb); 8: 448 (08c0023);
			      * 9: 453 (c02ff87); 10: 457; 11: 471; 12: 472; 13: 493; 14: 493,
			      * word 476 unused (hud_state version 6); 15: 495 (version 7) */
	.wide = 1,
	/* the draw thread, as verified on the console (Nov 22-23 2026: panel present in 100 % of
	 * samples at Aurora and in Halo 3): on, hardware thread 1, priority 15 */
	.thread = 1,
	.thr_hw = 1,         /* not 2 (driver thread, Usbd), 4 (XAM thread, audio), 5 (team net) */
	.thr_prio = 15,
	.bg = 1,
	.retry_us = 2000,
	.ensure = 1,
	.idle_ms = 2,
	/* as idle_ms: the thread wakes every idle_ms whatever is shown, so the mark adds only
	 * ensure's 5 texel reads per wake and a ~2000-pixel redraw per title frame; a longer
	 * wait makes it cheaper still but leaves it missing longer after each resolve */
	.badge_idle_ms = 2,
	.enable = 3,         /* on at boot: no computer needed. Acted on BOOT_HOLD_MS after start
			      * (stage_poll); hud.py stage 0 before then keeps it off */
	.keep = 0,           /* opaque (docs/PLAN.md 1.0) */
	/* the test state hud.py show starts from: the panel as it boots (BALANCE .. TEST TONE,
	 * PRESET on FLAT, MIC TEST) */
	.test_volume = 10,
	.test_game_on = 1,
	.test_headset = 1,
	.test_visible = HUD_PANEL,
	.test_rows = 0x3F,
	.test_rows_hi = 3,
	.test_mic_level = 5,
	.test_gate = 1,
};

/* ---- recorded swaps: the VdSwap hook writes, the driver thread reads ----------------------- */
#define RECS 8
struct swap_rec {
	volatile u32 seq; /* ticket + 1 once complete, 0 while written */
	u32 title;
	u32 tb;           /* timebase when VdSwap was called */
	u32 fetch[6];     /* with a picture: the fetch constant (driver thread: the effective one) */
	u32 info[7];
	u32 fence_addr, fence_val;
	u32 ptr[4];       /* r4 (fetch), r8, r9, r10 */
	u32 sfetch[6];    /* what r4 pointed to at the call (copies: bit 0 of copied) */
	u32 sout[3];      /* what r8, r9, r10 pointed to (bits 1-3 of copied) */
	u32 copied;
};
static struct swap_rec g_rec[RECS];
static volatile u32 g_rec_head; /* tickets handed out */

/* driver thread: copies of recent swaps */
struct pending {
	struct swap_rec r;
	u64 seen_ms;
	u32 fence_tb;   /* when the fence was first seen passed (or the swap seen, modes 1 and 2) */
	u32 src;        /* FS_*: where r.fetch (the buffer on screen after this swap) came from */
	u32 valid, done;
};
/* the last swap with a picture: its fetch constant and title */
static u32 g_pic[6], g_pic_title, g_pic_valid;
static struct pending g_pend[RECS];
static u32 g_seen;      /* tickets consumed */
static u32 g_last_fence_val, g_last_title;

static struct detour d_swap;
static HANDLE *volatile pExeModule;
static u64 g_init_ms;
static u32 g_inited, g_stage = 0;
/* hot reload: this build is handing over (hud_quiesce) */
static volatile u32 g_quiesced;
/* the per-second window */
static u32 g_win_tb, g_win_swaps, g_win_draws, g_win_vs, g_win_vd, g_win_open = 0;
static u32 g_win_py, g_win_pn;     /* ensure samples this second: panel there / not */

/* the published state (seqlock: hud_publish may run on any thread) */
static struct {
	volatile u32 seq;
	struct hud_state s;
} g_pub;

/* ---- the draw thread's wake-up: a KEVENT as the 17559 kernel lays it out (16 bytes). The
 * SDK's KEVENT is a 4-byte `void *`: never declare one (KeInitializeEvent would overwrite the
 * next 12 bytes); cast to KEVENT * only at the calls. --------------------------------------- */
struct xh_kevent {
	u8 type, absolute, size, inserted;
	volatile s32 signal_state;
	u32 wait_flink, wait_blink;
};
_Static_assert(sizeof(struct xh_kevent) == 16, "KEVENT on 17559 (KeInitializeEvent)");
static struct xh_kevent g_draw_wake __attribute__((aligned(16)));
static volatile u32 g_wake_ready;   /* g_draw_wake initialised and the thread exists */
static volatile u32 g_thr_active;   /* the draw thread is working (not waiting) */
static volatile u32 g_poll_active;  /* hud_poll is working */
#define D1GRPH_CONTROL 0x7FC86104
#define D1GRPH_PRIMARY_SURFACE_ADDRESS 0x7FC86110
#define KTHREAD_PRIORITY 0x70
#define KTHREAD_BASE 0xb9          /* the thread's base priority */
#define KTHREAD_PRIORITY_BASE 0xc9 /* the process's (KeSetBasePriorityThread adds to it) */
#define KTHREAD_PRIORITY_MAX 0xca  /* the ceiling KeSetBasePriorityThread clamps to */
#define DRAW_STACK 0x8000

/* ---- the VdSwap hook ---------------------------------------------------------------------------
 * VdSwap takes ten arguments; the 9th and 10th are in the caller's frame, so the original
 * must run on the caller's own stack. The stub saves r3-r10 and the 64-bit r14-r31, calls
 * XhSwapC (which returns the trampoline), restores everything and jumps to the trampoline
 * with the caller's LR and stack untouched. */
#define XH_SAVE(r) "    std " #r ", " #r "*8-96(1)\n"
#define XH_LOAD(r) "    ld " #r ", " #r "*8-96(1)\n"
#define XH_REGS(M) M(14) M(15) M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23) M(24) M(25) \
	M(26) M(27) M(28) M(29) M(30) M(31)
#define XH_SAVEA(r) "    std " #r ", " #r "*8+136(1)\n"
#define XH_LOADA(r) "    ld " #r ", " #r "*8+136(1)\n"
#define XH_ARGS(M) M(3) M(4) M(5) M(6) M(7) M(8) M(9) M(10)
/* frame (256): 0 back chain, 4 callee LR slot, 16..159 r14-r31, 160..223 r3-r10, 248 LR */
#ifndef HUD_HOST_TEST
__asm__("    .text\n"
	"    .globl XhSwapHook\n"
	"    .p2align 2\n"
	"XhSwapHook:\n"
	"    stwu 1, -256(1)\n"
	"    mflr 0\n"
	"    stw 0, 248(1)\n"
	XH_REGS(XH_SAVE)
	XH_ARGS(XH_SAVEA)
	"    bl XhSwapC\n"
	"    mtctr 3\n"
	XH_ARGS(XH_LOADA)
	XH_REGS(XH_LOAD)
	"    lwz 0, 248(1)\n"
	"    mtlr 0\n"
	"    addi 1, 1, 256\n"
	"    bctr\n");
#endif
void XhSwapHook(void);

/* the hook may read n bytes at a: aligned, not near 0, mapped */
static bool hook_readable(u32 a, u32 n)
{
	if (a < 0x10000 || (a & 3))
		return false;
#ifndef HUD_HOST_TEST
	return MmIsAddressValid((void *)a) && MmIsAddressValid((void *)(a + n - 1));
#else
	return true;
#endif
}

static u32 cur_title(void)
{
	return pExeModule ? (u32)*pExeModule : 0;
}

/* the render thread, once per presented frame. Reads exactly what VdSwap itself reads:
 * info[0] always, the fetch constant and info[1..2] if info[0] != 0, info[3..6] if
 * info[2] != 0 (0x800C8BC4..0x800C8C98). Records only at stage 2 and 3. */
HOOK u32 XhSwapC(u32 cmd, const u32 *fetch, u32 fence_addr, const volatile u32 *info,
		 u32 fence_val, u32 out_fb, u32 out_fmt, u32 out_cs)
{
	(void)cmd;
	if (g_xh.enable >= 2 && !g_quiesced && info) {
		u32 t = __sync_fetch_and_add(&g_rec_head, 1);
		struct swap_rec *r = &g_rec[t & (RECS - 1)];

		r->seq = 0;
		BARRIER();
		r->tb = ticks();
		r->title = cur_title();
		r->fence_addr = fence_addr;
		r->fence_val = fence_val;
		r->ptr[0] = (u32)fetch;
		r->ptr[1] = out_fb;
		r->ptr[2] = out_fmt;
		r->ptr[3] = out_cs;
		/* D3D passes r4 and r8 pointing into its own frame: copy them now (a mapped,
		 * aligned pointer only; VdSwap itself reads r4 only with a picture) */
		r->copied = 0;
		if (hook_readable((u32)fetch, 24)) {
			for (int i = 0; i < 6; i++)
				r->sfetch[i] = fetch[i];
			r->copied |= 1;
		}
		for (int i = 0; i < 3; i++) {
			u32 a = r->ptr[1 + i];

			r->sout[i] = 0;
			if (hook_readable(a, 4)) {
				r->sout[i] = *(const volatile u32 *)a;
				r->copied |= 2u << i;
			}
		}
		for (int i = 0; i < 7; i++)
			r->info[i] = 0;
		for (int i = 0; i < 6; i++)
			r->fetch[i] = 0;
		r->info[0] = info[0];
		if (r->info[0]) {
			for (int i = 0; i < 6; i++)
				r->fetch[i] = fetch[i];
			r->info[1] = info[1];
			r->info[2] = info[2];
			if (r->info[2])
				for (int i = 3; i < 7; i++)
					r->info[i] = info[i];
		}
		BARRIER();
		r->seq = t + 1;
#ifndef HUD_HOST_TEST
		if (g_wake_ready && g_xh.thread)
			KeSetEvent((KEVENT *)&g_draw_wake, 0, 0);
#endif
	}
	return (u32)d_swap.trampoline;
}

/* ---- the canvas (hud_panel.h, WP1.2): palette indices, coloured with hud_panel.c's
 * hp_palette (shot.py's PANEL_TEXT lists HP_LABEL and HP_BRIGHT, the sentinels' base colours) */
#define PALETTE hp_palette
#define SENTINELS 4

/* The two canvases and the blit's per-column / per-row tables (about 90 KB) are allocated
 * once, by hud_init: as statics they were image bytes (no .bss, build.sh). If the allocation
 * fails, nothing is ever drawn (skip_canvas counts the tries); the hook and the stages work
 * as usual. g_canvas / g_dcanvas / g_cx ... name the parts. */
struct hud_mem {
	struct hp_canvas canvas, dcanvas;
	/* per-column and per-row offset parts of the last geometry (col_part / row_part), and
	 * the canvas unit of each panel column and row */
	u32 cx[HP_W * 3], cy[HP_H_MAX * 3];
	u8 ux[HP_W * 3], uy[HP_H_MAX * 3];
};
static struct hud_mem *g_mem;
#define g_canvas (g_mem->canvas)
#define g_dcanvas (g_mem->dcanvas)
#define g_cx (g_mem->cx)
#define g_cy (g_mem->cy)
#define g_ux (g_mem->ux)
#define g_uy (g_mem->uy)
static u16 g_sxy[SENTINELS][2];     /* sentinel positions, canvas units */
static u16 g_chk[2];                /* the lowest text or shape unit (already_drawn) */
static u8 g_chk_c = 0xFF;           /* its colour; 0xFF: none */
static u8 g_sent_ok[SENTINELS];     /* inside the canvas, on an opaque unit */
static struct hud_state g_canvas_state;
static u32 g_canvas_valid, g_canvas_gen, g_canvas_bg;
/* what is drawn: g_canvas itself (bg 1), or g_dcanvas with the background removed (bg 0) or
 * kept only next to text and shapes (bg 2); set by rebuild */
static const struct hp_canvas *g_dcv;
/* the canvas this blit writes: g_dcv, or the full g_canvas over an older panel of ours (bg
 * 0 / 2 would leave its text where the new one has none); set by draw */
static const struct hp_canvas *g_blit_cv;
/* the canvas generation last drawn into each recent buffer (physical address) */
static u32 g_drawn_pa[4], g_drawn_gen[4], g_drawn_next;

static bool drawn_here(u32 pa, u32 gen)
{
	for (u32 i = 0; i < 4; i++)
		if (g_drawn_pa[i] == pa && g_drawn_gen[i] == gen)
			return true;
	return false;
}

static void note_drawn(u32 pa, u32 gen)
{
	for (u32 i = 0; i < 4; i++)
		if (g_drawn_pa[i] == pa) {
			g_drawn_gen[i] = gen;
			return;
		}
	g_drawn_next = (g_drawn_next + 1) & 3;
	g_drawn_pa[g_drawn_next] = pa;
	g_drawn_gen[g_drawn_next] = gen;
}


/* ---- the front buffer -------------------------------------------------------------------------- */
struct fb {
	u32 base, width, height, pitch, size;
	u32 format, endian, tiled, swizzle;
	u32 bits, mask;              /* per colour lane */
	u32 sr, sg, sb;              /* lane shifts in the texel value */
	u32 rgb;                     /* the three lanes */
};

/* the texel value as the GPU sees it, from the word the CPU reads (and back: each mode is
 * its own inverse). The GPU reads memory little-endian and then applies the endian swap. */
static u32 to_texel(u32 c, u32 endian)
{
	switch (endian) {
	case 0: /* none */
		return c >> 24 | (c >> 8 & 0xff00) | (c << 8 & 0xff0000) | c << 24;
	case 1: /* 8in16 */
		return c << 16 | c >> 16;
	case 2: /* 8in32 */
		return c;
	default: /* 16in32 */
		return (c & 0x00ff00ff) << 8 | (c >> 8 & 0x00ff00ff);
	}
}

/* fetch constant (xenos.h xe_gpu_texture_fetch_t) -> fb; SK_* */
static int decode_fetch(const u32 *f, struct fb *fb, u32 swap_rb)
{
	u32 dw0 = f[0], dw1 = f[1], dw2 = f[2], dw3 = f[3], src[3], h32;

	if ((dw0 & 3) != 2) /* not a texture fetch constant */
		return SK_TYPE;
	fb->pitch = ((dw0 >> 22) & 0x1ff) << 5;
	fb->tiled = dw0 >> 31;
	fb->format = dw1 & 0x3f;
	fb->endian = (dw1 >> 6) & 3;
	fb->base = dw1 & 0xfffff000;
	fb->width = (dw2 & 0x1fff) + 1;
	fb->height = ((dw2 >> 13) & 0x1fff) + 1;
	fb->swizzle = (dw3 >> 1) & 0xfff;
	switch (fb->format) {
	case 6:  /* k_8_8_8_8 */
	case 50: /* k_8_8_8_8_AS_16_16_16_16 */
		fb->bits = 8;
		break;
	case 7:  /* k_2_10_10_10 */
	case 54: /* k_2_10_10_10_AS_16_16_16_16 */
		fb->bits = 10;
		break;
	default:
		return SK_FORMAT;
	}
	if (fb->width < 320 || fb->width > 2048 || fb->height < 240 || fb->height > 2048 ||
	    fb->pitch < fb->width || fb->pitch > 4096)
		return SK_SIZE;
	h32 = (fb->height + 31) & ~31u;
	fb->size = fb->tiled ? fb->pitch * h32 * 4 : fb->pitch * fb->height * 4;
	/* a physical-memory mapping (0xA0000000..0xFFFFFFFF, VdSwap's own assumption when it
	 * converts the address for the GPU), and no wrap */
#ifndef HUD_HOST_TEST
	if (fb->base < 0xA0000000u || fb->base + fb->size - 1 < fb->base)
		return SK_ADDR;
#endif
	src[0] = fb->swizzle & 7;
	src[1] = (fb->swizzle >> 3) & 7;
	src[2] = (fb->swizzle >> 6) & 7;
	for (int i = 0; i < 3; i++) {
		/* 8888: any of the four components; 2_10_10_10: the 10-bit ones */
		if (src[i] >= (fb->bits == 8 ? 4u : 3u))
			return SK_SWIZZLE;
		for (int j = 0; j < i; j++)
			if (src[j] == src[i])
				return SK_SWIZZLE;
	}
	fb->mask = (1u << fb->bits) - 1;
	fb->sr = src[swap_rb ? 2 : 0] * fb->bits;
	fb->sg = src[1] * fb->bits;
	fb->sb = src[swap_rb ? 0 : 2] * fb->bits;
	fb->rgb = fb->mask << fb->sr | fb->mask << fb->sg | fb->mask << fb->sb;
	return SK_NONE;
}

/* byte offset of texel (x, y) in a 2D tiled 32 bpp surface whose pitch (texels) is a
 * multiple of 32: Xenia's texture_address::Tiled2D (= XGAddress2DTiledOffset x 4), with
 * bytes_per_block_log2 = 2 */
static u32 tiled_offset(u32 x, u32 y, u32 pitch)
{
	u32 outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
	u32 inner = ((y >> 1) & 7) << 3 | (x & 7);
	u32 b = (outer | inner) << 2;
	u32 bank = (y >> 4) & 1;
	u32 pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);

	return (y & 1) << 4 | pipe << 6 | bank << 11 | (b & 15) | ((b >> 4) & 1) << 5 |
	       ((b >> 5) & 7) << 8 | (b >> 8) << 12;
}

static u32 offset_of(const struct fb *fb, u32 x, u32 y)
{
	return fb->tiled ? tiled_offset(x, y, fb->pitch) : (y * fb->pitch + x) * 4;
}

/* The blit's offsets, split per column and per row. Tiled2D (above) puts x's bits at offset
 * bits 2, 3, 5, 6 and (x >> 5) << 12, y's at 4, 8, 9, 10, 11 and (y >> 5) x (pitch >> 5)
 * << 12, and bit 7 = x4 ^ y3. So offset = col(x) + row(y) - 2 x (col & row & 0x80): the
 * two parts' bits are disjoint but for bit 7 (an XOR), and the macro-tile parts add.
 * Linear: col = 4x, row = 4 x pitch x y, offset = col + row (overlap mask 0). */
static u32 col_part(u32 x, u32 tiled)
{
	if (!tiled)
		return x << 2;
	return (x & 3) << 2 | ((x >> 2) & 1) << 5 | ((x >> 3) & 1) << 6 | ((x >> 4) & 1) << 7 |
	       (x >> 5) << 12;
}

static u32 row_part(u32 y, u32 pitch, u32 tiled)
{
	if (!tiled)
		return (y * pitch) << 2;
	return (y & 1) << 4 | ((y >> 3) & 1) << 7 | ((y >> 1) & 7) << 8 | ((y >> 4) & 1) << 11 |
	       ((y >> 5) * (pitch >> 5)) << 12;
}

static inline u32 off_of(u32 col, u32 row, u32 overlap)
{
	return col + row - ((col & row & overlap) << 1);
}

/* where the panel goes */
struct geom {
	u32 f, x0, y0, w, h;
};

/* integer scale 2 at 720p, 3 at 1080p, 1 at 480p, stepped down until the canvas fits the
 * title-safe area (the middle 80 % each way). anchor (hp_canvas.anchor):
 *   HP_ANCHOR_BOTTOM_CENTRE (the panel, the toasts): bottom centre, bottom edge on the 10 %
 *     margin;
 *   HP_ANCHOR_BADGE (the mute mark): in the band between the 4 % and 10 % margins, below
 *     where the panel and the toasts sit, so neither ever lands on it: centred (badge_pos 0,
 *     and anything unknown), or 5 % in from the left / right edge, at the bottom (1, 2) or
 *     the top (3, 4). A band too thin for it (never at 480 lines and up) puts it on the 4 %
 *     margin.
 * false if it doesn't fit even at scale 1 */
static bool geometry(const struct fb *fb, u32 cw, u32 ch, u32 anchor, struct geom *g)
{
	u32 f = (fb->height + 180) / 360;
	u32 sw = fb->width - 2 * (fb->width / 10), sh = fb->height - 2 * (fb->height / 10);
	u32 m4 = fb->height * 4 / 100, band = fb->height / 10 - m4, pos = g_xh.badge_pos;

	if (f < 1)
		f = 1;
	if (f > 3)
		f = 3;
	while (f > 1 && (cw * f > sw || ch * f > sh))
		f--;
	if (cw * f > sw || ch * f > sh)
		return false;
	g->f = f;
	g->w = cw * f;
	g->h = ch * f;
	g->x0 = (fb->width - g->w) / 2;
	g->y0 = fb->height - fb->height / 10 - g->h;
	if (anchor != HP_ANCHOR_BADGE)
		return true;
	if (pos > 4)
		pos = 0;
	if (pos == 1 || pos == 3)
		g->x0 = fb->width / 20;
	else if (pos == 2 || pos == 4)
		g->x0 = fb->width - fb->width / 20 - g->w;
	/* top: the band's mirror image; bottom: centred in the band (or on the 4 % margin) */
	g->y0 = g->h < band ? m4 + (band - g->h) / 2 : m4;
	if (pos < 3)
		g->y0 = fb->height - g->y0 - g->h;
	return true;
}

/* every page the panel touches is mapped */
static bool panel_mapped(const struct fb *fb, const struct geom *g)
{
#ifndef HUD_HOST_TEST
	if (fb->tiled) {
		/* 32 x 32 texels of 4 bytes = one 4 KB page per macro tile */
		for (u32 ty = g->y0 >> 5; ty <= (g->y0 + g->h - 1) >> 5; ty++)
			for (u32 tx = g->x0 >> 5; tx <= (g->x0 + g->w - 1) >> 5; tx++)
				if (!MmIsAddressValid(PTR(fb->base + ((ty * (fb->pitch >> 5) + tx) << 12))))
					return false;
	} else {
		for (u32 y = g->y0; y < g->y0 + g->h; y++)
			if (!MmIsAddressValid(PTR(fb->base + offset_of(fb, g->x0, y))) ||
			    !MmIsAddressValid(PTR(fb->base + offset_of(fb, g->x0 + g->w - 1, y))))
				return false;
	}
#endif
	return true;
}

/* an 8-bit-per-channel colour as a texel value, alpha (and unused bits) all ones */
static u32 texel_of(const struct fb *fb, u32 rgb)
{
	u32 r = rgb >> 16 & 0xff, gr = rgb >> 8 & 0xff, b = rgb & 0xff;

	if (fb->bits == 10) {
		r = r << 2 | r >> 6;
		gr = gr << 2 | gr >> 6;
		b = b << 2 | b >> 6;
	}
	return ~fb->rgb | r << fb->sr | gr << fb->sg | b << fb->sb;
}

static u32 lane(u32 t, u32 s, u32 mask)
{
	return t >> s & mask;
}

/* the drawing state the blit uses */
struct ink {
	u32 word[HP_COLOURS];    /* CPU words for each palette entry */
	u32 cx, cy, cword;       /* the bottom check pixel (the lowest text or shape unit) */
	u32 sent[SENTINELS];     /* CPU words of the sentinels */
	u32 sx[SENTINELS], sy[SENTINELS]; /* their screen pixels */
	u32 n_sent;              /* 4, or 0: no repaint check, no sentinels */
	u32 bg_r, bg_g, bg_b;    /* the blended colour x (256 - keep), per lane */
	u32 keep;
};

static void make_ink(const struct fb *fb, const struct geom *g, struct ink *k, u32 keep)
{
	u32 t;

	for (int i = 0; i < HP_COLOURS; i++)
		k->word[i] = to_texel(texel_of(fb, PALETTE[i]), fb->endian);
	k->n_sent = SENTINELS;
	for (int i = 0; i < SENTINELS; i++) {
		u8 c;

		if (!g_sent_ok[i]) {
			k->n_sent = 0;
			break;
		}
		c = g_canvas.px[g_sxy[i][1] * HP_W + g_sxy[i][0]];
		/* the unit's colour with blue's low bit flipped and red's / green's carrying k
		 * (shot.py sentinel_pattern) */
		t = texel_of(fb, PALETTE[c]) ^ (1u << fb->sb) ^ ((u32)(i & 1) << fb->sr) ^
		    ((u32)(i >> 1 & 1) << fb->sg);
		k->sent[i] = to_texel(t, fb->endian);
		k->sx[i] = g->x0 + g_sxy[i][0] * g->f;
		k->sy[i] = g->y0 + g_sxy[i][1] * g->f;
	}
	k->cx = g->x0 + g_chk[0] * g->f;
	k->cy = g->y0 + g_chk[1] * g->f;
	k->cword = k->word[g_chk_c < HP_COLOURS ? g_chk_c : HP_BG];
	if (keep > 256)
		keep = 256;
	k->keep = keep;
	t = texel_of(fb, PALETTE[HP_BG]);
	k->bg_r = lane(t, fb->sr, fb->mask) * (256 - keep);
	k->bg_g = lane(t, fb->sg, fb->mask) * (256 - keep);
	k->bg_b = lane(t, fb->sb, fb->mask) * (256 - keep);
}

static u32 blend(const struct fb *fb, const struct ink *k, u32 c)
{
	u32 t = to_texel(c, fb->endian), m = fb->mask;
	u32 r = (lane(t, fb->sr, m) * k->keep + k->bg_r) >> 8;
	u32 g = (lane(t, fb->sg, m) * k->keep + k->bg_g) >> 8;
	u32 b = (lane(t, fb->sb, m) * k->keep + k->bg_b) >> 8;

	t = (t & ~fb->rgb) | r << fb->sr | g << fb->sg | b << fb->sb;
	return to_texel(t, fb->endian);
}

/* our panel is already in this frame (swapped again without a new resolve): the only reads
 * of the front buffer with keep 0 */
static bool already_drawn(const struct fb *fb, const struct ink *k)
{
	u32 c;

	if (k->n_sent != SENTINELS)
		return false;
	/* the lowest text unit too: a resolve may have covered the bottom rows only so far */
	if (g_chk_c < HP_COLOURS) {
		c = LD((volatile u32 *)PTR(fb->base + offset_of(fb, k->cx, k->cy)));
		if ((to_texel(c, fb->endian) & fb->rgb) != (to_texel(k->cword, fb->endian) & fb->rgb))
			return false;
	}
	for (int i = 0; i < SENTINELS; i++) {
		u32 c = LD((volatile u32 *)PTR(fb->base + offset_of(fb, k->sx[i], k->sy[i])));

		if ((to_texel(c, fb->endian) & fb->rgb) != (to_texel(k->sent[i], fb->endian) & fb->rgb))
			return false;
	}
	return true;
}

/* the geometry g_cx .. g_uy (struct hud_mem) were made for */
static struct {
	u32 x0, y0, w, h, f, pitch, tiled, valid;
} g_tab;

static void tables(const struct fb *fb, const struct geom *g)
{
	if (g_tab.valid && g_tab.x0 == g->x0 && g_tab.y0 == g->y0 && g_tab.w == g->w &&
	    g_tab.h == g->h && g_tab.f == g->f && g_tab.pitch == fb->pitch && g_tab.tiled == fb->tiled)
		return;
	for (u32 i = 0; i < g->w; i++) {
		g_cx[i] = col_part(g->x0 + i, fb->tiled);
		g_ux[i] = (u8)(i / g->f);
	}
	for (u32 j = 0; j < g->h; j++) {
		g_cy[j] = row_part(g->y0 + j, fb->pitch, fb->tiled);
		g_uy[j] = (u8)(j / g->f);
	}
	g_tab.f = g->f;
	g_tab.x0 = g->x0;
	g_tab.y0 = g->y0;
	g_tab.w = g->w;
	g_tab.h = g->h;
	g_tab.pitch = fb->pitch;
	g_tab.tiled = fb->tiled;
	g_tab.valid = 1;
}

/* two texels in one 8-byte store (p 8-byte aligned; big-endian: a at p, b at p + 4) */
static inline void st2(volatile u32 *p, u32 a, u32 b)
{
#ifndef HUD_HOST_TEST
	u32 lo = b;

	__asm__ __volatile__("rldimi %0, %1, 32, 0\n\tstd %0, 0(%2)"
			     : "+&r"(lo) : "r"(a), "r"(p) : "memory");
#else
	ST(p, a);
	ST(p + 1, b);
#endif
}

/* A tiled panel in memory order: each 128-byte line (16 x 2 texels: offset bits 2-3 x0-1,
 * 4 y0, 5 x2, 6 x3) written whole, ascending, so a write-combined front buffer gets full
 * lines instead of 16-byte pieces. (On the console, writing by rows took 16.7 ms for the
 * 720p panel: ~210 ns a store.) */
static void blit_lines(const struct fb *fb, const struct geom *g, const struct ink *k,
		       bool blend_bg)
{
	u8 *base = PTR(fb->base);
	u32 x0 = g->x0, y0 = g->y0, x1 = g->x0 + g->w, y1 = g->y0 + g->h;
	u32 last = ticks(), gap = 0, paused = 0, px = 0, wide = g_xh.wide, tries = 0;
	u32 retry = g_xh.retry_us * TB_PER_US;
	const u8 *cv = g_blit_cv->px;

again:
	tries++;
	for (u32 by = y0 & ~1u; by < y1; by += 2)
		for (u32 bx = x0 & ~15u; bx < x1; bx += 16) {
			u32 lo = tiled_offset(bx, by, fb->pitch), now = ticks();
			volatile u32 *line = (volatile u32 *)(base + lo);

			/* a pause here = the thread lost the CPU, not slow stores */
			if (now - last > gap)
				gap = now - last;
			if (now - last > 50000)
				g_xh.d_gaps++;
			if (now - last > 200 * TB_PER_US)
				paused += now - last;
			/* the title may have rendered over what is drawn so far: start again, whole */
			if (retry && now - last > retry && tries < 4) {
				last = now;
				g_xh.d_restarts++;
				goto again;
			}
			last = now;

			if (lo > fb->size - 128)
				continue; /* can't happen (geometry is inside the buffer) */
			/* texels n and n + 1 (n even) are x and x + 1 on one row: 8 bytes together */
			for (u32 n = 0; n < 32; n += 2) {
				u32 x = bx + ((n >> 4) & 1) * 8 + ((n >> 3) & 1) * 4 + (n & 3);
				u32 y = by + ((n >> 2) & 1);
				u32 c[2];

				for (u32 j = 0; j < 2; j++) {
					c[j] = HP_CLEAR;
					if (x + j >= x0 && x + j < x1 && y >= y0 && y < y1)
						c[j] = cv[g_uy[y - y0] * HP_W + g_ux[x + j - x0]];
					if (c[j] >= HP_COLOURS)
						c[j] = HP_CLEAR;
				}
				if (wide && c[0] != HP_CLEAR && c[1] != HP_CLEAR &&
				    !(blend_bg && (c[0] == HP_BG || c[1] == HP_BG))) {
					st2(&line[n], k->word[c[0]], k->word[c[1]]);
					px += 2;
					continue;
				}
				for (u32 j = 0; j < 2; j++) {
					if (c[j] == HP_CLEAR)
						continue;
					px++;
					if (c[j] == HP_BG && blend_bg)
						ST(&line[n + j], blend(fb, k, LD(&line[n + j])));
					else
						ST(&line[n + j], k->word[c[j]]);
				}
			}
		}
	for (u32 i = 0; i < k->n_sent; i++)
		ST((volatile u32 *)(base + offset_of(fb, k->sx[i], k->sy[i])), k->sent[i]);
	g_xh.d_gap_max_us = gap / TB_PER_US;
	g_xh.d_paused_us = paused / TB_PER_US;
	g_xh.d_tries = tries;
	g_xh.d_ink_px = px;
}

/* the panel into the frame: opaque (keep 0, or a repaint: stores only), or the background
 * blended (a read per background pixel); then the sentinels */
static void blit(const struct fb *fb, const struct geom *g, const struct ink *k, bool blend_bg)
{
	u8 *base = PTR(fb->base);
	u32 f = g->f, cw = g_canvas.w, ch = g_canvas.h, lim = fb->size - 4;
	u32 ov = fb->tiled ? 0x80 : 0;

	if (fb->tiled && !g_xh.order) {
		blit_lines(fb, g, k, blend_bg);
		return;
	}

	for (u32 uy = 0; uy < ch; uy++) {
		const u8 *row = &g_blit_cv->px[uy * HP_W];

		for (u32 dy = 0; dy < f; dy++) {
			u32 ry = g_cy[uy * f + dy];
			const u32 *cx = g_cx;

			for (u32 ux = 0; ux < cw; ux++, cx += f) {
				u32 c = row[ux], w;

				if (c == HP_CLEAR || c >= HP_COLOURS) /* transparent (or unknown) */
					continue;
				w = k->word[c];
				for (u32 dx = 0; dx < f; dx++) {
					u32 off = off_of(cx[dx], ry, ov);
					volatile u32 *p;

					if (off > lim)
						continue; /* can't happen (geometry is inside the buffer) */
					p = (volatile u32 *)(base + off);
					if (c == HP_BG && blend_bg)
						ST(p, blend(fb, k, LD(p)));
					else
						ST(p, w);
				}
			}
		}
	}
	for (u32 i = 0; i < k->n_sent; i++)
		ST((volatile u32 *)(base + offset_of(fb, k->sx[i], k->sy[i])), k->sent[i]);
}

/* write the touched lines back (and drop them: the GPU writes the next frame to memory) */
static void flush(const struct fb *fb, const struct geom *g)
{
	u8 *base = PTR(fb->base);

	if (fb->tiled) {
		/* a 128-byte line holds 16 x 2 texels (offset bits 0-3: x0-1, 4: y0, 5: x2, 6: x3) */
		for (u32 y = g->y0 & ~1u; y < g->y0 + g->h; y += 2)
			for (u32 x = g->x0 & ~15u; x < g->x0 + g->w; x += 16)
				DCBF(base + tiled_offset(x, y, fb->pitch));
	} else {
		for (u32 y = g->y0; y < g->y0 + g->h; y++) {
			u32 a = offset_of(fb, g->x0, y) & ~127u, e = offset_of(fb, g->x0 + g->w - 1, y);

			for (; a <= e; a += 128)
				DCBF(base + a);
		}
	}
	SYNC();
}

/* ---- the driver thread ------------------------------------------------------------------------- */

static void get_state(struct hud_state *s)
{
	if (g_xh.force == 1) {
		memset(s, 0, sizeof(*s));
		s->visible = g_xh.test_visible == HUD_TOAST || g_xh.test_visible == HUD_BADGE
				     ? (u8)g_xh.test_visible : HUD_PANEL;
		s->muted = g_xh.test_muted != 0;
		s->flash = g_xh.test_flash != 0;
		s->headset = g_xh.test_headset != 0;
		s->game_on = g_xh.test_game_on != 0;
		s->cursor = (u8)g_xh.test_cursor;
		s->rows = (u8)g_xh.test_rows;
		s->balance = (s8)g_xh.test_balance;
		s->volume = (u8)g_xh.test_volume;
		s->sidetone = (u8)g_xh.test_sidetone;
		s->mic_level = (u8)g_xh.test_mic_level;
		s->gate = (u8)g_xh.test_gate;
		s->tone = (u8)g_xh.test_tone;
		s->mic_vu = (u8)g_xh.test_mic_vu;
		s->gate_open = g_xh.test_gate_open != 0;
		s->team_level = (u8)g_xh.test_team_level;
		s->channel = (u8)g_xh.test_channel;
		s->team_peers = (u8)g_xh.test_team_peers;
		s->team_talking = (u8)g_xh.test_team_talking;
		s->rows_hi = (u8)g_xh.test_rows_hi;
		s->mictest = (u8)g_xh.test_mictest;
		s->mictest_secs = (u8)g_xh.test_mictest_secs;
		s->mictest_pct = (u8)g_xh.test_mictest_pct;
		s->mic_auto_level = (u8)g_xh.test_mic_auto_level;
		s->preset = (u8)g_xh.test_preset;
		s->preset_count = (u8)g_xh.test_preset_count;
		s->surround = (u8)g_xh.test_surround;
		s->surround_live = (u8)g_xh.test_surround_live;
		s->badge = (u8)g_xh.test_badge;
		s->badge_shift = (u8)g_xh.test_badge_shift;
		s->mictest_max = (u8)g_xh.test_mictest_max;
		for (int i = 0; i < HUD_NAME_LEN; i++)
			s->preset_name[i] = (char)(g_xh.test_preset_name[i / 4] >> (24 - 8 * (i % 4)));
		s->preset_flags = (u8)g_xh.test_preset_flags;
		s->custom[0] = (s8)g_xh.test_custom_bass;
		s->custom[1] = (s8)g_xh.test_custom_mids;
		s->custom[2] = (s8)g_xh.test_custom_treble;
		for (int i = 0; i < HUD_DESC_LEN; i++)
			s->desc[i] = (char)(g_xh.test_desc[i / 4] >> (24 - 8 * (i % 4)));
		s->demo = (u8)g_xh.test_demo;
		s->demo_secs = (u8)g_xh.test_demo_secs;
		return;
	}
	for (int tries = 0; tries < 8; tries++) {
		u32 s1 = g_pub.seq;

		if (s1 & 1)
			continue;
		BARRIER();
		*s = g_pub.s;
		BARRIER();
		if (g_pub.seq == s1)
			break;
	}
	if (g_xh.force == 2)
		s->visible = 0;
}

static void skip(u32 *counter, u32 why)
{
	(*counter)++;
	g_xh.last_skip = why;
}

static void rebuild(const struct hud_state *s)
{
	hp_build(&g_canvas, s);
	hp_sentinels(s, g_sxy);
	for (int i = 0; i < SENTINELS; i++) {
		u32 x = g_sxy[i][0], y = g_sxy[i][1];

		g_sent_ok[i] = x < g_canvas.w && y < g_canvas.h && g_canvas.w <= HP_W &&
			       g_canvas.h <= HP_H_MAX && g_canvas.px[y * HP_W + x] < HP_COLOURS;
	}
	g_chk_c = 0xFF;
	for (u32 y = g_canvas.h; y-- > 0 && g_chk_c == 0xFF && g_canvas.w <= HP_W && g_canvas.h <= HP_H_MAX;)
		for (u32 x = 0; x < g_canvas.w; x++) {
			u8 c = g_canvas.px[y * HP_W + x];

			if (c != HP_BG && c < HP_COLOURS) {
				g_chk[0] = (u16)x;
				g_chk[1] = (u16)y;
				g_chk_c = c;
				break;
			}
		}
	g_canvas_bg = g_xh.bg;
	g_dcv = &g_canvas;
	if (g_canvas_bg != 1 && g_canvas.w <= HP_W && g_canvas.h <= HP_H_MAX) {
		u32 w = g_canvas.w, h = g_canvas.h;

		g_dcanvas.w = (u16)w;
		g_dcanvas.h = (u16)h;
		for (u32 y = 0; y < h; y++)
			for (u32 x = 0; x < w; x++) {
				u8 c = g_canvas.px[y * HP_W + x];

				if (c == HP_BG) {
					c = HP_CLEAR;
					/* bg 2: kept if a text or shape unit is within one unit */
					for (u32 yy = y ? y - 1 : 0; g_canvas_bg == 2 && yy <= y + 1 && yy < h; yy++)
						for (u32 xx = x ? x - 1 : 0; xx <= x + 1 && xx < w; xx++) {
							u8 n = g_canvas.px[yy * HP_W + xx];

							if (n != HP_BG && n < HP_COLOURS)
								c = HP_BG;
						}
				}
				g_dcanvas.px[y * HP_W + x] = c;
			}
		g_dcv = &g_dcanvas;
	}
	g_canvas_state = *s;
	g_canvas_valid = 1;
	g_canvas_gen++;
	g_xh.canvas_w = g_canvas.w;
	g_xh.canvas_h = g_canvas.h;
}

static u32 phys_of(u32 va);
static void trace_ev(u32 kind, u32 a, u32 b);
static u32 scan_now(void);
enum { TK_WAKE = 1, TK_SWAP, TK_FENCE, TK_DRAW, TK_DRAWN, TK_SCAN, TK_ENSURE };

/* alias 1-3: draw through that window's mapping of the same physical memory; false if it
 * isn't mapped there (MmGetPhysicalAddress must agree) */
static bool alias_of(struct fb *fb)
{
#ifndef HUD_HOST_TEST
	static const u32 WIN[4] = { 0, 0xE0000000u - 0x1000, 0xA0000000u, 0xC0000000u };
	u32 a = g_xh.alias, pa, va;

	if (a < 1 || a > 3)
		return true;
	pa = phys_of(fb->base);
	if (!pa || pa >= 0x20000000)
		return false;
	va = pa + WIN[a];
	if (va == fb->base)
		return true;
	if (!MmIsAddressValid((void *)va) || !MmIsAddressValid((void *)(va + fb->size - 1)) ||
	    MmGetPhysicalAddress((void *)va) != pa)
		return false;
	fb->base = va;
#endif
	return true;
}

/* ensure: a re-draw into the buffer being scanned out, not a swap's own draw (no swap
 * statistics; counted in ens_draws) */
enum { DR_SKIP, DR_INTACT, DR_DREW };

/* the last mapped-page check that passed: repeated for the same buffer, title and place
 * only every 250 ms (ensure looks every pass) */
static struct {
	u32 pa, title, x0, y0, w, h, tb;
} g_mapped;

static int draw(struct pending *pd, const struct hud_state *s, bool ensure)
{
	const struct swap_rec *r = &pd->r;
	struct fb fb;
	struct geom g;
	struct ink k;
	int why;
	u32 t0, t1, d;
	bool again;

	if (r->title != cur_title()) {
		skip(&g_xh.skip_title, SK_TITLE);
		return DR_SKIP;
	}
	why = decode_fetch(r->fetch, &fb, g_xh.swap_rb);
	if (why) {
		skip(&g_xh.skip_format, (u32)why);
		return DR_SKIP;
	}
	if (!alias_of(&fb)) {
		skip(&g_xh.skip_alias, SK_UNMAPPED);
		return DR_SKIP;
	}
	g_xh.d_va = fb.base;
	if (!g_mem) {
		skip(&g_xh.skip_canvas, SK_CANVAS); /* hud_init could not allocate it */
		return DR_SKIP;
	}
	if (!g_canvas_valid || memcmp(&g_canvas_state, s, sizeof(*s)) || g_canvas_bg != g_xh.bg)
		rebuild(s);
	if (!g_canvas.w || !g_canvas.h || g_canvas.w > HP_W || g_canvas.h > HP_H_MAX ||
	    !geometry(&fb, g_canvas.w, g_canvas.h, g_canvas.anchor, &g)) {
		skip(&g_xh.skip_canvas, SK_CANVAS);
		return DR_SKIP;
	}
	{
		u32 pa = phys_of(fb.base), now = ticks();

		if (!(g_mapped.pa == pa && pa && g_mapped.title == r->title && g_mapped.x0 == g.x0 &&
		      g_mapped.y0 == g.y0 && g_mapped.w == g.w && g_mapped.h == g.h &&
		      now - g_mapped.tb < 250000 * TB_PER_US / 1000)) {
			if (!panel_mapped(&fb, &g)) {
				g_mapped.pa = 0;
				skip(&g_xh.skip_unmapped, SK_UNMAPPED);
				return DR_SKIP;
			}
			g_mapped.pa = pa;
			g_mapped.title = r->title;
			g_mapped.x0 = g.x0;
			g_mapped.y0 = g.y0;
			g_mapped.w = g.w;
			g_mapped.h = g.h;
			g_mapped.tb = now;
		}
	}
	make_ink(&fb, &g, &k, g_xh.keep);
	tables(&fb, &g);
	/* the fence was read before any pixel: keep it that way */
	SYNC();
	t0 = ticks();
	again = already_drawn(&fb, &k);
	if (again && drawn_here(phys_of(fb.base), g_canvas_gen)) {
		/* this canvas is already in this buffer: nothing to do */
		if (ensure)
			return DR_INTACT;
		g_xh.intact++;
	} else {
		/* (sentinels there but an older canvas, or another buffer: drawn again, opaque) */
		u32 tb = ticks();

		g_blit_cv = again ? &g_canvas : g_dcv;
		trace_ev(TK_DRAW, ensure ? 0 : r->seq, phys_of(fb.base));
		blit(&fb, &g, &k, !again && k.keep);
		g_xh.d_blit_us = (ticks() - tb) / TB_PER_US;
		g_xh.d_work_us = g_xh.d_blit_us > g_xh.d_paused_us ? g_xh.d_blit_us - g_xh.d_paused_us : 0;
		tb = ticks();
		if (!g_xh.noflush)
			flush(&fb, &g);
		g_xh.d_flush_us = (ticks() - tb) / TB_PER_US;
		g_xh.draws++;
		if (again)
			g_xh.repaints++;
		note_drawn(phys_of(fb.base), g_canvas_gen);
		trace_ev(TK_DRAWN, ensure ? 0 : r->seq, scan_now());
		if (ensure)
			g_xh.ens_draws++;
	}
	if (ensure)
		return DR_DREW;
	t1 = ticks();
	g_xh.vis_draws++;
	g_win_vd++;
	d = t1 - t0;
	g_xh.d_ticks = d;
	if (d > g_xh.d_ticks_max)
		g_xh.d_ticks_max = d;
	g_xh.d_hist[d < 12500 ? 0 : d < 25000 ? 1 : d < 50000 ? 2 : 3]++;
	g_xh.fence_late_us = (t0 - pd->fence_tb) / TB_PER_US;
	if (g_xh.fence_late_us > g_xh.fence_late_max_us)
		g_xh.fence_late_max_us = g_xh.fence_late_us;
	g_xh.swap_late_us = (t0 - r->tb) / TB_PER_US;
	if (g_xh.swap_late_us > g_xh.swap_late_max_us)
		g_xh.swap_late_max_us = g_xh.swap_late_us;
	g_xh.d_base = fb.base;
	g_xh.d_width = fb.width;
	g_xh.d_height = fb.height;
	g_xh.d_pitch = fb.pitch;
	g_xh.d_format = fb.format;
	g_xh.d_endian = fb.endian;
	g_xh.d_tiled = fb.tiled;
	g_xh.d_swizzle = fb.swizzle;
	g_xh.d_scale = g.f;
	g_xh.d_x0 = g.x0;
	g_xh.d_y0 = g.y0;
	g_xh.d_w = g.w;
	g_xh.d_h = g.h;
	g_xh.d_pixels = g.w * g.h;
	g_xh.sent_n = k.n_sent;
	for (u32 i = 0; i < SENTINELS; i++) {
		u32 t = to_texel(k.sent[i], fb.endian);

		g_xh.sent_x[i] = i < k.n_sent ? k.sx[i] : 0;
		g_xh.sent_y[i] = i < k.n_sent ? k.sy[i] : 0;
		g_xh.sent_rgb[i] = i < k.n_sent ? lane(t, fb.sr, fb.mask) << (2 * fb.bits) |
						  lane(t, fb.sg, fb.mask) << fb.bits |
						  lane(t, fb.sb, fb.mask) : 0;
	}
	return DR_DREW;
}

/* the GPU has processed this swap (its fence value is in place) */
static bool fence_passed(const struct swap_rec *r, u32 *now_val)
{
	const volatile u32 *p = (const volatile u32 *)r->fence_addr;

#ifndef HUD_HOST_TEST
	if (!p || (r->fence_addr & 3) || !MmIsAddressValid((void *)p))
		return false;
#endif
	*now_val = *p;
	return (s32)(*now_val - r->fence_val) >= 0;
}

/* a swap with a picture: one log line and a tt_* slot per new (title, format) pair */
static void note_title(const struct swap_rec *r)
{
	u32 f0 = r->fetch[0], f1 = r->fetch[1], f2 = r->fetch[2];
	u32 fmt = (f1 & 0x3f) | ((f1 >> 6) & 3) << 8 | (f0 >> 31) << 10 | (((f0 >> 22) & 0x1ff) << 5) << 16;
	u32 size = ((f2 & 0x1fff) + 1) << 16 | (((f2 >> 13) & 0x1fff) + 1);
	u32 n;

	if (r->title != g_last_title) {
		g_last_title = r->title;
		g_xh.titles_seen++;
	}
	for (int i = 0; i < 4; i++)
		if (g_xh.tt_title[i] == r->title && g_xh.tt_fmt[i] == fmt && g_xh.tt_size[i] == size &&
		    (u32)i < g_xh.tt_next)
			return;
	n = g_xh.tt_next & 3;
	g_xh.tt_title[n] = r->title;
	g_xh.tt_fmt[n] = fmt;
	g_xh.tt_size[n] = size;
	g_xh.tt_next++;
	xa_log("hud: title %08x: %ux%u format %u endian %u %s pitch %u base %08x\n", r->title,
	       size >> 16, size & 0xffff, fmt & 0x3f, (fmt >> 8) & 3, fmt & 0x400 ? "tiled" : "linear",
	       fmt >> 16, f1 & 0xfffff000);
}

/* ---- the scan-out surface (xbdm's screenshot handler, 0x9100D5D0 in xbdm.xex) ---- */

static u32 g_scan_p = 0xffffffff, g_scan_alias;

/* the scan-out surface's CPU address (0: none), recomputed when the register or alias change */
static u32 scan_va(void)
{
#ifndef HUD_HOST_TEST
	static const u32 WIN[3] = { 0xC0000000u, 0xA0000000u, 0xE0000000u - 0x1000 };
	u32 p = *(volatile u32 *)D1GRPH_PRIMARY_SURFACE_ADDRESS, va = 0;

	if (p == g_scan_p && g_scan_alias == g_xh.alias)
		return g_xh.scan_va;
	g_scan_p = p;
	g_scan_alias = g_xh.alias;
	g_xh.scan_phys = p;
	g_xh.scan_ctl = *(volatile u8 *)D1GRPH_CONTROL;
	if (p >= 0x1000 && p < 0x20000000 && !(p & 0xfff))
		for (u32 i = 0; i < 3; i++) {
			u32 a = p + WIN[i];

			if (g_xh.alias && g_xh.alias != 3 - i)
				continue;
			if (MmIsAddressValid((void *)a) && MmGetPhysicalAddress((void *)a) == p) {
				va = a;
				break;
			}
		}
	g_xh.scan_va = va;
	return va;
#else
	return 0;
#endif
}

/* the physical address of a front buffer's CPU address (one-entry cache; 0: unmapped) */
static u32 phys_of(u32 va)
{
#ifndef HUD_HOST_TEST
	static u32 cva[4], cpa[4], next;

	for (u32 i = 0; i < 4; i++)
		if (cva[i] == va && va)
			return cpa[i];
	next = (next + 1) & 3;
	cva[next] = va;
	cpa[next] = MmIsAddressValid((void *)va) ? MmGetPhysicalAddress((void *)va) : 0;
	return cpa[next];
#else
	return va;
#endif
}

/* ---- the event trace (hud.py trace): armed by writing N to g_xh.trace ---- */

static void trace_ev(u32 kind, u32 a, u32 b)
{
	u32 n = g_xh.trace & 0x7f, i = g_xh.tr_n;

	/* trace bit 7: wakes too (with idle_ms 2 they would fill the ring) */
	if (!n || i >= n || i >= 64 || (kind == TK_WAKE && !(g_xh.trace & 0x80)))
		return;
	g_xh.tr[i * 4] = ticks();
	g_xh.tr[i * 4 + 1] = kind;
	g_xh.tr[i * 4 + 2] = a;
	g_xh.tr[i * 4 + 3] = b;
	g_xh.tr_n = i + 1;
}

/* the display's scan-out register, raw (0 on the host); a change is a trace event */
static u32 scan_now(void)
{
#ifndef HUD_HOST_TEST
	static u32 last;
	u32 p = *(volatile u32 *)D1GRPH_PRIMARY_SURFACE_ADDRESS;

	if (p != last) {
		last = p;
		trace_ev(TK_SCAN, 0, p);
	}
	return p;
#else
	return 0;
#endif
}

/* the title's recent front buffers (distinct bases), with their layouts: ensure draws into
 * the scan-out only when it is one of these */
struct front {
	u32 title, pa;
	u32 fetch[6];
};
static struct front g_fronts[4];
static u32 g_front_next;

static void note_front(const struct pending *p)
{
	u32 va = p->r.fetch[1] & 0xfffff000, pa = phys_of(va);

	for (u32 i = 0; i < 4; i++)
		if (g_fronts[i].pa == pa && g_fronts[i].title == p->r.title) {
			for (int j = 0; j < 6; j++)
				g_fronts[i].fetch[j] = p->r.fetch[j];
			return;
		}
	g_front_next = (g_front_next + 1) & 3;
	g_fronts[g_front_next].title = p->r.title;
	g_fronts[g_front_next].pa = pa;
	for (int j = 0; j < 6; j++)
		g_fronts[g_front_next].fetch[j] = p->r.fetch[j];
}

/* the buffer on screen after swap p: p->r.fetch and p->src */
static void front_of(struct pending *p)
{
	struct fb fb;
	u32 f[6], src = g_xh.src, sva;

	p->src = FS_NONE;
	if (p->r.info[0] && (p->r.fetch[0] & 3) == 2) {
		p->src = FS_OWN;
		for (int i = 0; i < 6; i++)
			g_pic[i] = p->r.fetch[i];
		g_pic_title = p->r.title;
		g_pic_valid = 1;
	}
	for (int i = 0; i < 6; i++)
		f[i] = p->r.sfetch[i];
	if (src == 3 && (p->r.copied & 2) && p->r.sout[0] >= 0xA0000000u && !(p->r.sout[0] & 0xfff))
		f[1] = (f[1] & 0xfff) | p->r.sout[0];
	if (!p->src && src != 1 && (p->r.copied & 1) && decode_fetch(f, &fb, 0) == SK_NONE) {
		p->src = FS_PTR;
		for (int i = 0; i < 6; i++)
			p->r.fetch[i] = f[i];
		if (p->r.copied & 2) {
			if (p->r.sout[0] == (p->r.sfetch[1] & 0xfffff000))
				g_xh.ptr_agree++;
			else
				g_xh.ptr_differ++;
		}
	}
	if (!p->src && src != 2 && src != 3 && g_pic_valid && g_pic_title == p->r.title) {
		p->src = FS_LAST;
		for (int i = 0; i < 6; i++)
			p->r.fetch[i] = g_pic[i];
	}
	if (!p->src)
		return;
	/* is it what the display scans? (physical addresses: the mappings may differ) */
	sva = scan_va();
	if (g_xh.scan_phys) {
		if (phys_of(p->r.fetch[1] & 0xfffff000) == g_xh.scan_phys)
			g_xh.scan_agree++;
		else
			g_xh.scan_differ++;
	}
	note_front(p);
	trace_ev(TK_SWAP, p->r.seq, phys_of(p->r.fetch[1] & 0xfffff000));
	if (src == 4 && sva)
		p->r.fetch[1] = (p->r.fetch[1] & 0xfff) | sva;
}

/* new swaps from the ring into g_pend */
static void ingest(u64 now, bool counting)
{
	u32 head = g_rec_head;

	if (head - g_seen > RECS)
		g_seen = head - RECS;
	for (; g_seen != head; g_seen++) {
		struct swap_rec *r = &g_rec[g_seen & (RECS - 1)];
		struct pending *p = &g_pend[g_seen & (RECS - 1)];
		u32 s1 = r->seq;

		if (s1 != g_seen + 1) {
			if ((s32)(s1 - (g_seen + 1)) > 0) {
				p->valid = 0;
				continue; /* already overwritten by a newer swap */
			}
			break; /* still being written: the next poll takes it */
		}
		p->valid = 0;
		BARRIER();
		p->r = *r;
		BARRIER();
		if (r->seq != s1)
			continue;
		p->r.seq = s1;
		p->seen_ms = now;
		p->fence_tb = ticks();
		p->valid = 1;
		p->done = 0;
		g_xh.swaps++;
		front_of(p);
		g_xh.src_n[p->src]++;
		g_xh.sw_src = p->src;
		if (p->src == FS_OWN) {
			g_xh.swaps_fetch++;
			for (int i = 0; i < 6; i++)
				g_xh.pic_fetch[i] = p->r.fetch[i];
			g_xh.pic_title = p->r.title;
		}
		if (p->src != FS_NONE) {
			note_title(&p->r);
			if (counting && p->r.title == cur_title()) {
				g_xh.vis_swaps++;
				g_win_vs++;
			}
		}
		for (int i = 0; i < 4; i++)
			g_xh.sw_ptr[i] = p->r.ptr[i];
		for (int i = 0; i < 3; i++)
			g_xh.sw_out[i] = p->r.sout[i];
		if (p->r.fence_val == g_last_fence_val)
			g_xh.fence_same++;
		g_last_fence_val = p->r.fence_val;
		for (int i = 0; i < 6; i++)
			g_xh.sw_fetch[i] = p->r.fetch[i];
		for (int i = 0; i < 7; i++)
			g_xh.sw_info[i] = p->r.info[i];
		g_xh.sw_fence_addr = p->r.fence_addr;
		g_xh.sw_fence_val = p->r.fence_val;
		g_xh.sw_title = p->r.title;
	}
}

/* the swap to draw now, or NULL; everything older than it is marked done */
static u32 g_pick_quiet; /* the draw thread's spin: don't count fence waits */

static struct pending *pick(u64 now)
{
	struct pending *best = NULL, *newest = NULL, *prev = NULL;
	u32 mode = g_xh.mode;

	/* newest first: tickets g_seen - 1, g_seen - 2, ... */
	for (u32 i = 1; i <= RECS; i++) {
		struct pending *p = &g_pend[(g_seen - i) & (RECS - 1)];

		if (!p->valid || p->r.seq != g_seen - i + 1 || p->done)
			continue;
		if (now - p->seen_ms > STALE_MS) {
			p->done = 1;
			g_xh.skip_stale++;
			continue;
		}
		if (p->src == FS_NONE || p->r.title != cur_title()) {
			p->done = 1; /* the buffer on screen isn't known, or its title is gone */
			continue;
		}
		if (!newest) {
			newest = p;
		} else if (!prev) {
			prev = p;
		}
		if (mode == 0 && !best) {
			u32 v = 0;

			if (fence_passed(&p->r, &v)) {
				best = p;
				best->fence_tb = ticks();
				g_xh.sw_fence_now = v;
				trace_ev(TK_FENCE, p->r.seq, v);
			}
		}
	}
	if (mode == 0 && !best && newest)
		g_xh.fence_waits += !g_pick_quiet; /* the GPU hasn't reached it yet (normal) */
	if (mode == 1)
		best = newest;
	else if (mode == 2)
		best = prev;
	if (!best)
		return NULL;
	/* it and everything older: done */
	for (u32 i = 1; i <= RECS; i++) {
		struct pending *p = &g_pend[(g_seen - i) & (RECS - 1)];

		if (p == best) {
			for (u32 j = i; j <= RECS; j++)
				g_pend[(g_seen - j) & (RECS - 1)].done = 1;
			break;
		}
	}
	return best;
}

static void *vdswap_addr(void)
{
	void *a = 0;
#ifndef HUD_HOST_TEST
	HANDLE krnl;

	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0)
		return 0;
	XexGetProcedureAddress(krnl, ORD_VdSwap, &a);
#endif
	return a;
}

/* stage 1: VdSwap is the 17559 code read offline (sig_ok 1), or not (2, error = where) */
static void check_sig(void)
{
	void *a = vdswap_addr();
	/* a hot-reload slot build: an earlier build's patch is in the first four words */
	bool ours = a && detour_known(a);

	g_xh.sig_addr = (u32)a;
	if ((u32)a != ADDR_VdSwap) {
		g_xh.sig_ok = 2;
		g_xh.error = (u32)a;
		xa_log("hud: stage 1: VdSwap (603) at %08x, not the 17559 address: refused\n", (u32)a);
		return;
	}
	for (u32 i = ours ? 1 : 0; i < ARRAY_SIZE(SIGS); i++) {
		const volatile u32 *c = (const volatile u32 *)SIGS[i].addr;

		for (u32 j = 0; j < SIGS[i].n; j++)
			if (c[j] != SIGS[i].w[j]) {
				g_xh.sig_ok = 2;
				g_xh.error = SIGS[i].addr + 4 * j;
				xa_log("hud: stage 1: VdSwap code differs at %08x (%08x, want %08x): "
				       "refused\n", SIGS[i].addr + 4 * j, c[j], SIGS[i].w[j]);
				return;
			}
	}
	g_xh.sig_ok = 1;
	g_xh.error = 0;
	xa_log("hud: stage 1: VdSwap at %08x, %s code words match\n", (u32)a,
	       ours ? "19 (the prologue is an earlier build's hook)" : "23");
}

/* stage 2: the hook */
static void install(void)
{
	int err = detour_install(&d_swap, "VdSwap", (void *)g_xh.sig_addr, (void *)XhSwapHook);

	if (err) {
		g_xh.hooked = HK_FAILED;
		g_xh.error = (u32)err;
		xa_log("hud: stage 2: VdSwap hook failed (%d)\n", err);
		return;
	}
	g_xh.hooked = HK_INSTALLED;
	g_xh.install_ms = (u32)(xa_now_ms() - g_init_ms);
	if (!g_xh.install_ms)
		g_xh.install_ms = 1;
	xa_log("hud: stage 2: VdSwap hooked%s, %u ms after start\n",
	       d_swap.adopted ? " (adopted from an earlier build)" : "", g_xh.install_ms);
}

#define BOOT_HOLD_MS 20000

/* the stage the xbdm control asks for, acted on */
static void stage_poll(void)
{
	u32 want = g_xh.enable;

	if (want > STAGE_MAX)
		want = STAGE_MAX;
	/* at boot nothing is checked or hooked for BOOT_HOLD_MS: the dashboard is up first, and
	 * a bad build can still be replaced over xbdm in that time */
	if (xa_now_ms() - g_init_ms < BOOT_HOLD_MS)
		return;
	if (want != g_stage) {
		xa_log("hud: stage %u -> %u%s\n", g_stage, want,
		       want < 2 && g_xh.hooked == HK_INSTALLED ? " (the hook stays in, passing through)"
							       : "");
		g_stage = want;
		g_xh.stage = want;
		if (want >= 1 && g_xh.sig_ok != 1 && g_xh.hooked != HK_INSTALLED)
			check_sig();
	}
	if (g_stage >= 2 && g_xh.sig_ok == 1 && g_xh.hooked == HK_NONE)
		install();
}

/* the per-second rates */
static void window(void)
{
	u32 now = ticks(), d = now - g_win_tb;

	if (!g_win_open) {
		g_win_open = 1;
		g_win_tb = now;
		g_win_swaps = g_xh.swaps;
		g_win_draws = g_xh.draws;
		g_win_vs = g_win_vd = 0;
		return;
	}
	if (d < 50000000u)
		return;
	g_xh.swaps_per_s = (u32)((u64)(g_xh.swaps - g_win_swaps) * 50000000u / d);
	g_xh.draws_per_s = (u32)((u64)(g_xh.draws - g_win_draws) * 50000000u / d);
	if (g_win_vs)
		g_xh.cover_pct = g_win_vd >= g_win_vs ? 100 : g_win_vd * 100 / g_win_vs;
	if (g_win_py + g_win_pn)
		g_xh.pres_pct = g_win_py * 100 / (g_win_py + g_win_pn);
	g_win_py = g_win_pn = 0;
	g_win_tb = now;
	g_win_swaps = g_xh.swaps;
	g_win_draws = g_xh.draws;
	g_win_vs = g_win_vd = 0;
}

/* ---- the interface (hud_360.h) ----------------------------------------------------------------- */

void hud_publish(const struct hud_state *s)
{
	g_pub.seq++;
	BARRIER();
	g_pub.s = *s;
	BARRIER();
	g_pub.seq++;
	g_xh.publishes++;
	g_xh.pub_visible = s->visible;
}

void hud_init(void)
{
	void *p = 0;

	if (g_inited)
		return;
	g_inited = 1;
#ifndef HUD_HOST_TEST
	{
		HANDLE krnl;

		if (XexGetModuleHandle("xboxkrnl.exe", &krnl) >= 0)
			XexGetProcedureAddress(krnl, ORD_XexExecutableModuleHandle, &p);
	}
#endif
	pExeModule = p;
	g_init_ms = xa_now_ms();
	g_mem = malloc(sizeof(*g_mem));
	if (g_mem)
		memset(g_mem, 0, sizeof(*g_mem));
	else
		xa_log("hud: no memory for the canvases (%u bytes): nothing will be drawn\n",
		       (u32)sizeof(*g_mem));
	xa_log("hud: ready, stage %u (hud.py stage N: 1 check, 2 hook, 3 draw)\n", g_xh.enable);
}

/* one pass of the drawing side: new swaps in, the rates, and the draw if the panel is up.
 * Runs on the driver thread (hud_poll) or the draw thread, never both (work_begin). spin_us
 * (draw thread only): with no swap ready, watch the newest one's fence this long. */
/* the buffer the display scans now: if it is one of the title's recent front buffers and
 * lacks this canvas, draw it there (4 sentinel reads when it has it) */
static u32 g_present_tb;           /* the last ensure sample that found the panel, or its redraw */
static u32 g_work_vis;             /* hud_state.visible as the last pass saw it */

static void ensure_scanout(const struct hud_state *s)
{
	u32 pa = scan_now();
	struct pending e;

	g_xh.ens_checks++;
	for (u32 i = 0; i < 4; i++)
		if (pa && g_fronts[i].pa == pa && g_fronts[i].title == cur_title()) {
			u32 t = ticks();
			int r;

			memset(&e, 0, sizeof(e));
			e.r.title = g_fronts[i].title;
			for (int j = 0; j < 6; j++)
				e.r.fetch[j] = g_fronts[i].fetch[j];
			r = draw(&e, s, true);
			if (r == DR_INTACT) {
				g_xh.pres_yes++;
				g_win_py++;
				g_present_tb = t;
			} else if (r == DR_DREW) {
				/* gone (the title resolved over it): from the last sample that had it to
				 * the end of this redraw */
				u32 gap = (ticks() - g_present_tb) / TB_PER_US;

				g_xh.pres_no++;
				g_win_pn++;
				trace_ev(TK_ENSURE, gap, pa);
				if (g_present_tb && gap < 1000000) {
					g_xh.gap_last_us = gap;
					if (gap > g_xh.gap_max_us)
						g_xh.gap_max_us = gap;
				}
				g_present_tb = ticks();
			}
			return;
		}
	g_xh.ens_skips++;
}

static void work(u32 spin_us)
{
	u64 now = xa_now_ms();
	struct hud_state s;
	struct pending *p;
	bool up;

	get_state(&s);
	g_work_vis = s.visible;
	up = g_stage >= 3 && s.visible;
	trace_ev(TK_WAKE, g_rec_head, scan_now());
	ingest(now, up);
	window();
	if (!up) {
		/* nothing pending is drawn later */
		for (int i = 0; i < RECS; i++)
			g_pend[i].done = 1;
		return;
	}
	p = pick(now);
	if (!p && spin_us) {
		u32 t0 = ticks(), d = 0;

		g_xh.thr_spins++;
		g_pick_quiet = 1;
		SMT_LOW(); /* SMT priority low while it spins */
		while (!p && (d = ticks() - t0) < spin_us * TB_PER_US) {
			ingest(now, up);
			p = pick(now);
		}
		SMT_MEDIUM();
		g_pick_quiet = 0;
		if (p)
			g_xh.thr_spin_hits++;
		if (d / TB_PER_US > g_xh.thr_spin_us_max)
			g_xh.thr_spin_us_max = d / TB_PER_US;
	}
	if (p)
		draw(p, &s, false);
	if (g_xh.ensure)
		ensure_scanout(&s);
}

/* the drawing side may run on this thread now: mine = g_thr_active or g_poll_active, set
 * first, then the other one and the control checked (each side's store before its loads) */
static bool work_begin(volatile u32 *mine, const volatile u32 *other, bool want)
{
	*mine = 1;
	SYNC();
	if (!want || *other) {
		*mine = 0;
		return false;
	}
	return true;
}

static void work_end(volatile u32 *mine)
{
	BARRIER();
	*mine = 0;
}

/* ---- the draw thread: an NV64 thunk (docs/INTERNALS.md), as thunks_common.s's macro, kept
 * here so the HUD adds no line to the shared thunk files ------------------------------------ */
#ifndef HUD_HOST_TEST
__asm__("    .text\n"
	"    .globl XhDrawThread\n"
	"    .p2align 2\n"
	"XhDrawThread:\n"
	"    mflr 0\n"
	"    stwu 1, -176(1)\n"
	"    stw 0, 168(1)\n"
	"    .irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
	"    std \\r, (16 + (\\r - 14) * 8)(1)\n"
	"    .endr\n"
	"    bl XhDrawThreadC\n"
	"    .irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
	"    ld \\r, (16 + (\\r - 14) * 8)(1)\n"
	"    .endr\n"
	"    lwz 0, 168(1)\n"
	"    mtlr 0\n"
	"    addi 1, 1, 176\n"
	"    blr\n");
u32 XhDrawThread(void *context);

static u8 *current_kthread(void)
{
	u32 pcr;

	__asm__ volatile("mr %0, 13" : "=r"(pcr));
	return *(u8 **)(pcr + 0x100);
}

/* woken by the hook at every swap (or every 100 ms); after hud_quiesce it does nothing and
 * stays parked here: its image stays loaded (docs/HOT-RELOAD.md) */
HOOK u32 XhDrawThreadC(void *context)
{
	u32 prio = 0;

	(void)context;
	g_xh.thr_heartbeat++; /* it ran (on THR_BOOT_HW, at the default priority) */
	for (;;) {
		/* only the mute mark up (it may be there for a whole match): its own wait */
		u32 idle = g_work_vis == HUD_BADGE ? g_xh.badge_idle_ms : g_xh.idle_ms;

		idle = idle < 1 ? 1 : idle > 1000 ? 1000 : idle;
		int64_t timeout = -(int64_t)idle * 10000; /* relative, 100 ns units */
		u32 want;

		/* priority first, then the hardware thread: it must be able to run where it lands
		 * (created on hw 1 at the default priority, it never ran: whatever runs there at
		 * a higher priority kept it off) */
		want = g_xh.thr_prio;
		if (want != prio && want >= 1 && want <= 15) {
			u8 *kt = current_kthread();

			prio = want;
			/* KeSetBasePriorityThread (0x80070A38) clamps the base to this thread's
			 * ceiling at +0xCA (9 for a system thread): above 9, the ceiling is raised
			 * first (this thread's own KTHREAD) */
			if (prio > kt[KTHREAD_PRIORITY_MAX])
				kt[KTHREAD_PRIORITY_MAX] = (u8)prio;
			KeSetBasePriorityThread(kt, (u32)((s32)prio - (s32)kt[KTHREAD_PRIORITY_BASE]));
			g_xh.thr_prio_now = kt[KTHREAD_PRIORITY];
			g_xh.thr_prio_base = kt[KTHREAD_BASE];
			g_xh.thr_prio_max = kt[KTHREAD_PRIORITY_MAX];
			xa_log("hud: draw thread priority %u, base %u, ceiling %u (asked %u)\n",
			       kt[KTHREAD_PRIORITY], kt[KTHREAD_BASE], kt[KTHREAD_PRIORITY_MAX], prio);
		}
		want = g_xh.thr_hw;
		if (want != g_xh.thr_hw_used && want <= 5 && want != 2 && want != 4) {
			u32 old = 0;

			KeSetAffinityThread(current_kthread(), 1u << want, &old);
			g_xh.thr_hw_used = want;
			xa_log("hud: draw thread moved to hardware thread %u (affinity was %02x)\n", want, old);
		}
		/* Executive, KernelMode, not alertable */
		KeWaitForSingleObject((KEVENT *)&g_draw_wake, 0, 0, 0, &timeout);
		g_xh.thr_heartbeat++;
		if (!work_begin(&g_thr_active, &g_poll_active,
				g_xh.thread && !g_quiesced && g_xh.hooked == HK_INSTALLED))
			continue;
		g_xh.thr_wakes++;
		work(g_xh.spin_us > 8000 ? 8000 : g_xh.spin_us);
		work_end(&g_thr_active);
	}
}

/* The thread is always created on THR_BOOT_HW (where the first build's thread ran at the
 * default priority); its loop then sets thr_prio and only then moves to thr_hw. */
#define THR_BOOT_HW 3

static void thread_start(void)
{
	HANDLE thread = 0;
	u32 tid, hw = THR_BOOT_HW;

	g_xh.thr_hw_used = hw;
	/* synchronization: a set while the thread works is kept for its next wait */
	KeInitializeEvent((KEVENT *)&g_draw_wake, 1, 0);
	ExCreateThread(&thread, DRAW_STACK, &tid, 0, (void *)XhDrawThread, 0,
		       0x2 | (1u << hw) << 24);
	g_xh.thr_state = thread ? 1 : 2;
	BARRIER();
	g_wake_ready = thread ? 1 : 0;
	xa_log("hud: draw thread %s on hardware thread %u (then priority %u, hardware thread %u)\n",
	       thread ? "started" : "NOT created", hw, g_xh.thr_prio, g_xh.thr_hw);
}
#endif

void hud_poll(void)
{
	u32 t0, us;

	if (!g_inited || g_quiesced)
		return;
	t0 = ticks();
	g_xh.heartbeat++;
	g_xh.now_ms = (u32)xa_now_ms();
	stage_poll();
	if (g_xh.hooked != HK_INSTALLED)
		goto out;
#ifndef HUD_HOST_TEST
	if (g_xh.thread && !g_xh.thr_state && g_stage >= 3)
		thread_start();
#endif
	if (work_begin(&g_poll_active, &g_thr_active, !(g_xh.thread && g_xh.thr_state == 1))) {
		work(0);
		work_end(&g_poll_active);
	}
out:
	us = (ticks() - t0) / TB_PER_US;
	g_xh.poll_us = us;
	if (us > g_xh.poll_max_us)
		g_xh.poll_max_us = us;
}

/* driver thread (hud_poll runs on it too, so no draw is in progress here) */
void hud_quiesce(void)
{
	g_quiesced = 1;
	g_xh.quiesced = 1;
	SYNC();
	/* the draw thread finishes its pass (a draw: a few ms) and then only waits */
	for (u32 ms = 0; g_thr_active && ms < 500; ms += 17)
		xa_sleep_ms(1);
	xa_log("hud: quiesced (the VdSwap hook passes through%s)\n",
	       g_thr_active ? "; the draw thread is STILL drawing" : "");
}

void hud_resume(void)
{
	g_quiesced = 0; /* (swaps recorded before the quiesce age out as usual: STALE_MS) */
	g_xh.quiesced = 0;
	xa_log("hud: resumed\n");
}
