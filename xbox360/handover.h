/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Hot reload of xw.xex (docs/HOT-RELOAD.md): what a running build leaves for the build that
 * replaces it, and the commands xw.py gives the two of them over xbdm.
 *
 * A loaded module can't be loaded again, and code another thread may be inside can't be
 * freed, so a build is never unloaded: it is asked to release (it quiesces: no threads, no
 * XAM calls, no USB traffic of its own), publishes this block, and stays in memory as dead
 * weight plus a few passive entry points (its hooks, until their slots are taken over; its
 * USB callbacks, for a controller nobody adopted or an IN transfer still on the wire). The
 * next build, loaded under its own name and base (build.sh XW_SLOT), is given this block's
 * address and takes over: hooks by one store each (detour.h), controllers by copying the
 * open endpoint handles and making itself the device's class driver, with no USB reset, so
 * a controller that is switched on stays on.
 *
 * This layout is shared between builds, which may be different versions of this file: only
 * append fields, never reorder, and bump XW_HO_VERSION if a field's meaning changes. A
 * build only takes over from a block of its own version. Everything is a u32 on purpose.
 */
#pragma once

#include "../core/compat.h"
#include "detour.h"

#define XW_HO_MAGIC 0x5857484F /* "XWHO": xw.py finds the block by it, like the others */
#define XW_HO_VERSION 1
#define XW_HO_PORTS 4

/* xw_handover.state */
enum {
	HO_STARTING = 1, /* not in the main loop yet: a release isn't looked at */
	HO_RUNNING,      /* normal operation */
	HO_RELEASING,    /* a release is in progress (it may still be refused) */
	HO_RELEASED,     /* quiesced for good; ports[] and the detour registry are final */
	HO_IDLE,         /* a slot build that was never told what to take over: it does nothing */
};

/* xw_handover.refuse: why the last release was refused (the log has a line too) */
enum {
	HO_R_NONE,
	HO_R_NOT_RUNNING,  /* still starting up */
	HO_R_AUDIO_BROKEN, /* an audio USB request never completed: its blocks may be on Usbd */
	HO_R_AUDIO_OPEN,   /* the headset's endpoints didn't close, or blocks are still queued */
	HO_R_REMOVING,     /* a controller is being removed right now */
	HO_R_SETUP,        /* a controller is being set up (a control transfer is in flight) */
	HO_R_XAM_BUSY,     /* the XAM thread didn't finish the unbinds (it is inside XAM) */
	HO_R_TX,           /* a controller's OUT queue didn't drain */
	HO_R_DPC,          /* the commit never ran on Usbd's DPC */
	HO_R_NOT_OWNER,    /* Usbd's device record doesn't name this build's class driver */
};

/* xw_handover.take_state (the new build) */
enum {
	HO_T_NONE,      /* no take-over asked for (yet) */
	HO_T_IMPORTED,  /* the previous build's block checked, its hooks inherited */
	HO_T_DONE,      /* controllers adopted (or reset); `adopted` says how many */
	HO_T_FAILED,    /* take_error says why; this build stays idle and hooks nothing */
	HO_T_ALONE,     /* told there is no previous build: a normal start */
};

/* xw_handover.take_error */
enum {
	HO_E_NONE,
	HO_E_UNMAPPED = 1, /* the address given isn't mapped */
	HO_E_MAGIC,        /* no handover block there */
	HO_E_VERSION,      /* the previous build's block is another version */
	HO_E_NOT_RELEASED, /* the previous build is still running */
	HO_E_TIMEOUT,      /* xw.py never said what to take over */
	HO_E_DPC,          /* the adoption never ran on Usbd's DPC */
};

/* xw_ho_port.flags */
#define HO_PF_HEADSET 1 /* a headset had announced itself: the next build asks for it */
#define HO_PF_READY 2   /* the controller was bound as a pad */
#define HO_PF_MUTED 4   /* its pad's mic was muted (Share tap): the next build mutes it too */

/*
 * One controller. `node` != 0 means: claimed, configured, its interrupt IN/OUT pair and its
 * default endpoint open, the audio endpoints closed, nothing of the releasing build queued
 * on it except possibly one IN transfer (in_entry). Written on Usbd's DPC by the releasing
 * build at the commit, and from then on only on Usbd's DPC by whoever touches it.
 */
struct xw_ho_port {
	volatile u32 node; /* Usbd's device record; 0 = nothing to adopt (or adopted, or gone) */
	u32 driver;        /* the class driver Usbd has for it (the word at node + 4) */
	u32 vid, pid;
	u32 ep_in, ep_out, max_in, max_out, interval_in, interval_out;
	u32 h_in, h_out, h_ctrl; /* endpoint handles (a transfer block's first word) */
	u32 aud_ep_in, aud_ep_out, aud_max_in, aud_max_out, aud_desc;
	u32 flags;         /* HO_PF_* */
	/*
	 * The interrupt IN transfer. One is always queued, and it is never cancelled: an idle
	 * controller sends nothing, so it may stay queued for hours, inside the image of the
	 * build that queued it. in_entry = the xw_ho_port (in that build's block, possibly an
	 * older one than the releasing build's) whose transfer it is; 0 = none is queued. While
	 * that entry's in_busy is set, nobody queues another one (two on a full-speed interrupt
	 * endpoint froze the console). When it completes, its owner's completion clears in_busy
	 * and calls in_fwd, if set, on Usbd's DPC:
	 *     void in_fwd(void *node, const u8 *data, u32 len, long status)
	 * a plain C function of the adopting build (the 32-bit ABI; the caller is already
	 * behind an NV64 thunk), which takes the packet and queues its own transfer.
	 */
	u32 in_entry;
	volatile u32 in_busy;
	volatile u32 in_fwd;
	/* the audio format the jack headset last accepted: in | out << 8 | HO_HS_FMT_VALID (0 =
	 * none known, or a build from before it). The next build restarts the headset with it
	 * (gip_headset_resume): a running headset ignores an identify request */
	u32 hs_fmt;
	u32 reserved[3];
};
#define HO_HS_FMT_VALID 0x10000

struct xw_handover {
	u32 magic, version, size;
	volatile u32 state;        /* HO_* */
	/* xbdm (xw.py reload) -> a running build: 1 = release; 2 = release, then load the
	 * module named in load_path from this build's own thread (no loader title) */
	volatile u32 cmd_release;
	/* xbdm -> a slot build, which waits for it before it installs anything: the address of
	 * the released build's block, or 0xFFFFFFFF = there is none (a normal start) */
	volatile u32 cmd_takeover;
	/* xbdm, before cmd_takeover: 0 = adopt the controllers as they are; 1 = don't adopt,
	 * re-enumerate them with a USB reset instead (the old build closes them); 2 = adopt,
	 * but without a second security handshake (a diagnostic) */
	volatile u32 cmd_mode;
	u32 slot;                  /* build.sh XW_SLOT: 0 = xw.xex, n = xw<n>.xex */
	u32 refuse;                /* HO_R_*: the last refused release */
	u32 refusals, releases;    /* counters: xw.py watches them for the outcome */
	volatile u32 take_state;   /* HO_T_* */
	u32 take_error;            /* HO_E_* */
	u32 adopted, resets, skipped; /* controllers taken over / reset / left with the old build */
	u32 inherited;             /* hooks inherited */
	u32 detours;               /* struct detour_registry *: every hook known to this build */
	u32 load_status;           /* cmd_release 2: XexLoadImage's status (0xEEEEEEEE: in it) */
	/*
	 * Instrumentation (docs/PLAN.md WP3.1), in what were reserved[0..6]; a build from before
	 * it has zeros here (trace_ring 0 = no trace, no gates). Nothing a take-over reads.
	 */
	volatile u32 trace;        /* the last reload step reached: TR_* (| argument << 16) */
	volatile u32 trace_n;      /* trace codes written so far; the ring has the last 64 */
	u32 trace_ring;            /* struct xw_trace * (magic XWTR) */
	/* xbdm (xw.py reload --step): the reload steps run while cmd_gate >= their number,
	 * releasing gates R1..R6 and taking-over gates T1..T7 (docs/HOT-RELOAD.md). 99 at
	 * load = no gate holds */
	volatile u32 cmd_gate;
	volatile u32 gate_at;      /* 'R' or 'T' << 8 | n while held at that gate; 0 none */
	volatile u32 title;        /* XexExecutableModuleHandle's value, as this build's driver
				    * thread last saw it (every 10 ms while it waits, every pass
				    * of its loop; frozen once a release has ended the thread) */
	volatile u32 in_double;    /* guard: interrupt IN transfers refused because one was
				    * queued on the endpoint already (must stay 0) */
	u32 reserved[1];
	char load_path[128];       /* cmd_release 2: e.g. \Device\Harddisk0\Partition1\Plugins\xw1.xex */
	struct xw_ho_port ports[XW_HO_PORTS];
};

/* xw.py reads and writes these at fixed places (HO_* in xw.py) */
_Static_assert(sizeof(struct xw_ho_port) == 26 * 4, "xw.py HO_PORT_WORDS");
_Static_assert(__builtin_offsetof(struct xw_handover, load_path) == 27 * 4, "xw.py HO_OFF_PATH");
_Static_assert(__builtin_offsetof(struct xw_handover, ports) == 27 * 4 + 128, "xw.py HO_OFF_PORTS");
_Static_assert(__builtin_offsetof(struct xw_handover, trace) == 19 * 4, "xw.py HO_EXT");

/* xw_handover.trace_ring: the last 64 trace codes with the timebase (50 MHz, low word) at
 * each, written by whoever reaches a step (driver thread, XAM-free; Usbd's DPC), slot
 * (trace_n - 1) % 64 last. xw.py tee and xw.py trace read it. */
#define XW_TRACE_MAGIC 0x58575452 /* "XWTR" */
#define XW_TRACE_RING 64
struct xw_trace {
	u32 magic, version, entries, spare;
	u32 e[XW_TRACE_RING][2]; /* code, timebase */
};
_Static_assert(sizeof(struct xw_trace) == 16 + 8 * XW_TRACE_RING, "xw.py TRACE_*");
