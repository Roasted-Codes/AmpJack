// SPDX-License-Identifier: GPL-3.0-or-later
/* XInput injection: see xinput_360.h. */
#include <xecore/xboxkrnl.h>

#include "../core/platform.h"
#include "detour.h"
#include "xinput_360.h"

#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")

/* ordinals */
#define ORD_XInputdReadState 486          /* xboxkrnl.exe */
/* XInputdGetDeviceStats(device, ?, stats, out): XAM's turn-off dialog (Guide held) asks it
 * about the device first and drops the dialog if it fails; for our tags it would, as the
 * kernel routes 0x1000000n to wireless slot n. The dialog reads stats+12 only:
 * 0x10000000 = a wireless pad (adds "turn off controller"). Retail 17559 address. */
#define ORD_XInputdGetDeviceStats 492
#define ADDR_XInputdGetDeviceStats 0x800F6D50
#define ORD_XamInputGetCapabilitiesEx 685 /* xam.xex */
#define ORD_XamInputSetState 402          /* xam.xex */

/* XamInputSetState (17559, 0x816F1740) hands the kernel a scale from XAM's per-user table of
 * the profile's Vibration setting (3 on, 0 off; rumble_360.h), instead of the title's flags:
 *   +0x3C lis r11, hi; +0x40 addi r11, r11, lo; +0x44 lbzx r4, r31, r11   (r31 = user)
 * Our hook replaces the call into the kernel for our pads, so it has to apply that byte
 * itself. The table's address is taken from those three instructions (after the four our
 * detour rewrites), so it is only used where the code is exactly this; anything else leaves
 * the setting unknown and rumble as the title asks. */
#define SETSTATE_VIB_OFF 0x3C
#define SETSTATE_VIB_LBZX 0x7c9f58ae
#define XAM_LO 0x815F0000u
#define XAM_HI 0x81C00000u
static volatile const u8 *g_vib_table;

/* XamUserBindDeviceCallback is not exported; retail 17559 address, checked before use.
 * prologue: 7c8b2378 7ca42b78 54ca063f 41820010 */
#define ADDR_XamUserBind 0x816D9060
static const u32 XAM_BIND_SIG[2] = { 0x7c8b2378, 0x7ca42b78 };

/* The kernel's table of XAM input callbacks (kernel 17559): XAM registers them at boot.
 * +0x0c is the Guide button, +0x10 XamUserBindDeviceCallback. The console's own
 * wireless driver (WgcWirelessOnPacketIn) calls the Guide entry, through 0x8012BB88,
 * with (device context, 0, 0) when Guide is released after a short press, from Usbd's
 * DPC. So it only queues the work and is safe from any context, unlike
 * XamInputSendXenonButtonPress, which opens the guide synchronously and can block its caller. */
#define K_XAM_INPUT_CALLBACKS 0x80189ea0
/* Held this long: the turn-off menu. The console's wired-controller driver starts a 2000 ms
 * timer when Guide goes down (kernel 0x800F9508); if it's still held when the timer fires
 * (0x800F8DA8) it calls the Guide entry with (device, 0, 1), and the release then sends
 * nothing; released earlier, it sends (device, 0, 0) (0x800F9544). Same here. */
#define GUIDE_LONG_PRESS_MS 2000
#define GUIDE_DEBOUNCE_MS 700    /* the controller repeats its Guide message: one press, one call */

/* After a Guide press, XAM's follow-up work blocked our driver thread in a kernel wait,
 * even with the Guide call made from another thread, and that thread is the one that
 * feeds XAM the input: a deadlock. The likely link is that the driver thread bound the
 * pads. So every XAM call (bind, unbind, Guide) goes through a thread of its own, on hw
 * thread 4, away from the driver thread (2); the driver thread only queues requests. */
#define XAM_THREAD_FLAGS (0x2 | (1u << 4) << 24)
#define XAM_THREAD_STACK 0x4000
#define XAM_QUEUE 16 /* requests: driver thread -> XAM thread (single producer/consumer) */
enum { REQ_BIND = 1, REQ_UNBIND, REQ_GUIDE, REQ_NOTIFY, REQ_GUIDE_LONG, REQ_HEADSET,
	REQ_HEADSET_OFF };

/* TODO (the user, Nov 2026): status notices such as "Wired controller connected" are to go
 * behind an "advanced / troubleshooting" setting, off by default. Not built yet.
 *
 * On-screen notifications (XNotifyQueueUI, xam ord 656 in every XAM version hiddriver360 supports).
 * Its prototype is (type, user index, u64 areas, text, context): the u64 is the third
 * argument, so it's declared u32 here to keep text and context in r6/r7 as XAM expects
 * (a ppc32 u64 would take a register pair). Type 14 shows text with the review icon. */
#define ORD_XNotifyQueueUI 656
#define NOTIFY_TYPE 14
#define NOTIFY_USER_ANY 0xff
#define NOTIFY_AREAS 1
#ifdef XA_WIRED /* xw.xex: wired controllers */
static const char *const notify_text[XA_NOTIFY_COUNT] = {
	[XA_NOTIFY_STARTING] = "Wired Xbox controllers: starting",
	[XA_NOTIFY_READY] = "Wired Xbox controllers: ready",
	[XA_NOTIFY_NO_FIRMWARE] = "",
	[XA_NOTIFY_NO_FIRMWARE_02FE] = "",
	[XA_NOTIFY_NO_FIRMWARE_02E6] = "",
	[XA_NOTIFY_PAD_ON] = "Wired controller connected",
	[XA_NOTIFY_PAD_FULL] = "Wired controller: all 4 slots in use",
	[XA_NOTIFY_MIC_MUTED] = "Headset: mic muted",
	[XA_NOTIFY_MIC_ON] = "Headset: mic on",
};
#else
static const char *const notify_text[XA_NOTIFY_COUNT] = {
	[XA_NOTIFY_STARTING] = "Wireless adapter: starting",
	[XA_NOTIFY_READY] = "Wireless adapter: ready",
	[XA_NOTIFY_NO_FIRMWARE] = "Wireless adapter: firmware missing",
	[XA_NOTIFY_NO_FIRMWARE_02FE] = "Wireless adapter: 02fe firmware missing",
	[XA_NOTIFY_NO_FIRMWARE_02E6] = "Wireless adapter: 02e6 firmware missing",
	[XA_NOTIFY_PAD_ON] = "",
	[XA_NOTIFY_PAD_FULL] = "",
	[XA_NOTIFY_MIC_MUTED] = "",
	[XA_NOTIFY_MIC_ON] = "",
};
#endif
static u16 notify_wide[XA_NOTIFY_COUNT][40]; /* kept alive: the queue may hold the pointer */

/* our device contexts and controller ids, as hiddriver360 picks them */
#define CONTEXT_BASE XA_XINPUT_CONTEXT_BASE
#define CONTROLLER_ID_BASE 0xa7553952

/* XINPUT_GAMEPAD (big-endian native, 12 bytes) */
struct xinput_gamepad {
	u16 buttons;
	u8 trigger_left, trigger_right;
	s16 stick_lx, stick_ly, stick_rx, stick_ry;
};

/* XINPUT button bits */
#define XI_DPAD_UP 0x0001
#define XI_DPAD_DOWN 0x0002
#define XI_DPAD_LEFT 0x0004
#define XI_DPAD_RIGHT 0x0008
#define XI_START 0x0010
#define XI_BACK 0x0020
#define XI_THUMB_L 0x0040
#define XI_THUMB_R 0x0080
#define XI_SHOULDER_L 0x0100
#define XI_SHOULDER_R 0x0200
#define XI_A 0x1000
#define XI_B 0x2000
#define XI_X 0x4000
#define XI_Y 0x8000

#define XI_DEVTYPE_GAMEPAD 1
#define XI_DEVSUBTYPE_GAMEPAD 1
/* ERROR_SUCCESS, ERROR_DEVICE_NOT_CONNECTED, ERROR_INVALID_PARAMETER: from xboxkrnl_types.h */

struct xpad {
	volatile u32 active;
	u32 context;
	u32 user_index;              /* 0 if XAM gave none (0xff): the hooks still need one */
	u32 user_bound;              /* XAM gave a real index (xa_xinput_user) */
	volatile u32 packet;
	struct xinput_gamepad state; /* written by the driver thread, read by the hook */
	u64 guide_down_ms;           /* driver thread: when Guide went down, 0 = up */
	u64 guide_sent_ms;           /* driver thread: when we last asked for the guide */
	u32 guide_long_sent;         /* driver thread: this press already opened the turn-off menu */
	volatile u32 rumble_pending; /* XamInputSetState -> driver thread */
	volatile u8 rumble_left, rumble_right;
};

static struct xpad g_xpad[4];

/* Debug switches, writable over xbdm (addresses: llvm-nm out/xa.exe): a per-hook enable bit
 * (1 = our behaviour, 0 = pass through) and call counters, to narrow a problem down to one
 * hook without rebuilding. The enable bits (read, caps, set) also decide which hooks
 * xa_xinput_init installs: build.sh installs all three by default (XA_HOOKS=read for input
 * only); without its defines, only the input hook. */
#ifndef XA_HOOK_CAPS
#define XA_HOOK_CAPS 0
#endif
#ifndef XA_HOOK_STATS
#define XA_HOOK_STATS 0
#endif
#ifndef XA_HOOK_SET
#define XA_HOOK_SET 0
#endif
__attribute__((used)) volatile u32 xa_xi_on[4] = { 1, XA_HOOK_CAPS, XA_HOOK_SET, XA_HOOK_STATS };
__attribute__((used)) volatile u32 xa_xi_calls[3];                  /* read, caps, set */
__attribute__((used)) volatile u32 xa_xi_read_ctx[4];              /* distinct read contexts seen */
__attribute__((used)) volatile u32 xa_guide = 1;                   /* 0 = Guide does nothing */

/* resolved kernel/XAM functions */
static int (*pXamUserBind)(u32 controller_id, u32 context, u8 category, int disconnect,
			   u8 *user_index);
static void (*pGuideCallback)(u32 context, u32 zero1, u32 zero2);
static void (*pXNotifyQueueUI)(u32 type, u32 user, u32 areas, const u16 *text, void *context);
/* Notify TLS bind (see docs/INVESTIGATION.md): XAM's create gate reads a per-thread
 * notify context via TlsGetValue, and nothing ever sets that slot for our XAM thread
 * (the only binder runs inside XAM's init windows and clears the slot again when the
 * window ends). We bind it ourselves right before each XNotifyQueueUI call: pair[0] =
 * the notify singleton (its +0xc is the context, +4 a refcount, +8 flags), which
 * satisfies the gate chain V -> *V -> context. After the first successful create the
 * TLS-free merge path takes over. */
static int (*pTlsSetValue)(u32 index, const void *value); /* xboxkrnl ord 341 */
static u32 xa_notify_pair[2];
#define XAM_TLS_INDEX_GLOB 0x81A82570u  /* TlsAlloc result of XAM's notify init */
#define XAM_SINGLETON_GLOB 0x81AA07A8u  /* lazily created notify singleton */
#define ORD_TlsSetValue 341
static volatile u32 xam_queue[XAM_QUEUE]; /* (request << 8) | slot */
static volatile u32 xam_head, xam_tail;
static volatile u32 xam_stop, xam_running;
static volatile u32 xam_busy;     /* the XAM thread is inside a request (it sets and clears it) */
static volatile u32 xi_quiesced;  /* hot reload: this build has handed over; the hooks only
				   * pass through (xa_xinput_quiesce) */

/* originals, reached through the trampolines */
typedef long (*read_state_t)(u32 ctx, u32 *packet, struct xinput_gamepad *out, int *unk);
typedef u32 (*caps_t)(u32 unk, u32 user, u32 flags, void *caps);
typedef u32 (*set_state_t)(u32 user, u32 flags, void *state, u8 amp, u8 freq, u8 off);

static struct detour d_read, d_caps, d_set, d_stats;

/* thunks (thunks.s) */
u32 XaXamThread(void *context);
long XaReadStateHook(u32 ctx, u32 *packet, struct xinput_gamepad *out, int *unk);
u32 XaCapsHook(u32 unk, u32 user, u32 flags, void *caps);
u32 XaSetStateHook(u32 user, u32 flags, void *state, u8 amp, u8 freq, u8 off);
typedef long (*stats_t)(u32 device, u32 arg, u32 *stats, u32 *out);
long XaStatsHook(u32 device, u32 arg, u32 *stats, u32 *out);

#define HOOK __attribute__((used, noinline))

static struct xpad *pad_by_context(u32 ctx)
{
	for (int i = 0; i < 4; i++)
		if (g_xpad[i].active && g_xpad[i].context == ctx)
			return &g_xpad[i];
	return NULL;
}

static struct xpad *pad_by_user(u32 user)
{
	if ((user & 0xff) == 0xff)
		user = 0;
	for (int i = 0; i < 4; i++)
		if (g_xpad[i].active && g_xpad[i].user_index == user)
			return &g_xpad[i];
	return NULL;
}

/* ---- the hooks (C bodies behind the NV64 thunks) --------------------------------------- */

HOOK long XaReadStateHookC(u32 ctx, u32 *packet, struct xinput_gamepad *out, int *unk)
{
	struct xpad *p = ctx >= CONTEXT_BASE ? pad_by_context(ctx) : NULL;

	xa_xi_calls[0]++;
	for (int i = 0; i < 4; i++) {
		if (xa_xi_read_ctx[i] == ctx)
			break;
		if (!xa_xi_read_ctx[i]) {
			xa_xi_read_ctx[i] = ctx;
			break;
		}
	}
	if (!p || !xa_xi_on[0] || xi_quiesced)
		return ((read_state_t)d_read.trampoline)(ctx, packet, out, unk);
	if (!out)
		return ERROR_INVALID_PARAMETER;

	BARRIER();
	*out = p->state;
	if (packet)
		*packet = ++p->packet;
	if (unk)
		*unk = 0;
	/* no XAM calls here: XAM holds its input lock while it polls us (the Guide button is
	 * handled by the driver thread, see xa_xinput_update) */
	return 0;
}

HOOK u32 XaCapsHookC(u32 unk, u32 user, u32 flags, void *caps)
{
	u32 status = ((caps_t)d_caps.trampoline)(unk, user, flags, caps);
	struct xpad *p;
	u8 *c = caps;

	xa_xi_calls[1]++;
	if (status != ERROR_DEVICE_NOT_CONNECTED || !caps || !xa_xi_on[1] || xi_quiesced)
		return status;
	p = pad_by_user(user);
	if (!p)
		return status;

	/* XINPUT_CAPABILITIES_EX: Type, SubType, Flags (u16), then XINPUT_GAMEPAD */
	memset(c, 0, 4 + sizeof(struct xinput_gamepad) + 4);
	c[0] = XI_DEVTYPE_GAMEPAD;
	c[1] = XI_DEVSUBTYPE_GAMEPAD;
	return ERROR_SUCCESS;
}

HOOK u32 XaSetStateHookC(u32 user, u32 flags, void *state, u8 amp, u8 freq, u8 off)
{
	u32 status = ((set_state_t)d_set.trampoline)(user, flags, state, amp, freq, off);
	struct xpad *p = pad_by_user(user);
	u8 *s = state;

	xa_xi_calls[2]++;
	if (!p || !xa_xi_on[2] || xi_quiesced)
		return status;
	/* XINPUT_VIBRATION: wLeftMotorSpeed, wRightMotorSpeed (u16, 0..65535) */
	if (s) {
		p->rumble_left = (u8)(((u32)(s[0] << 8 | s[1]) * 100) / 65535);
		p->rumble_right = (u8)(((u32)(s[2] << 8 | s[3]) * 100) / 65535);
		BARRIER();
		p->rumble_pending = 1;
	}
	return ERROR_SUCCESS;
}

/* ---- interface ------------------------------------------------------------------------- */

/* our pads: success, as a wired pad (not 0x10000000), so the turn-off dialog appears */
HOOK long XaStatsHookC(u32 device, u32 arg, u32 *stats, u32 *out)
{
	struct xpad *p = device >= CONTEXT_BASE ? pad_by_context(device) : NULL;

	if (!p || !p->active || !xa_xi_on[3] || xi_quiesced)
		return ((stats_t)d_stats.trampoline)(device, arg, stats, out);
	if (out)
		*out = 0;
	if (stats)
		memset(stats, 0, 16); /* +12, the device kind, is all the dialog reads */
	return 0;
}

int xa_xinput_init(void)
{
	HANDLE krnl, xam;
	void *read_state = 0, *caps = 0, *set_state = 0, *stats = 0;
	u32 *bind = (u32 *)ADDR_XamUserBind;
	int err = 0;

	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0 ||
	    XexGetModuleHandle("xam.xex", &xam) < 0) {
		xa_log("xinput: no module handle\n");
		return -XA_ENODEV;
	}
	XexGetProcedureAddress(krnl, ORD_XInputdReadState, &read_state);
	XexGetProcedureAddress(krnl, ORD_XInputdGetDeviceStats, &stats);
	if ((u32)stats != ADDR_XInputdGetDeviceStats)
		xa_xi_on[3] = 0; /* not the 17559 routine we read: leave it alone */
	XexGetProcedureAddress(xam, ORD_XamInputGetCapabilitiesEx, &caps);
	XexGetProcedureAddress(xam, ORD_XamInputSetState, &set_state);
	XexGetProcedureAddress(xam, ORD_XNotifyQueueUI, (void *)&pXNotifyQueueUI); /* optional */
	XexGetProcedureAddress(krnl, ORD_TlsSetValue, (void *)&pTlsSetValue); /* notify tls bind */
	if (!read_state || !caps || !set_state) {
		xa_log("xinput: read=%08x caps=%08x set=%08x\n", (u32)read_state, (u32)caps,
		       (u32)set_state);
		return -XA_ENODEV;
	}
	if (bind[0] != XAM_BIND_SIG[0] || bind[1] != XAM_BIND_SIG[1]) {
		xa_log("xinput: XamUserBindDeviceCallback signature mismatch at %08x (%08x %08x)\n",
		       (u32)bind, bind[0], bind[1]);
		return -XA_EINVAL;
	}
	pXamUserBind = (void *)bind;

	/* XAM's Vibration table, read out of XamInputSetState's own code */
	{
		const u32 *w = (const u32 *)((u32)set_state + SETSTATE_VIB_OFF);
		u32 table = (w[0] & 0xFFFF) << 16;

		table += (u32)(s32)(s16)(w[1] & 0xFFFF);
		if ((w[0] & 0xFFFF0000u) == 0x3D600000u && (w[1] & 0xFFFF0000u) == 0x396B0000u &&
		    w[2] == SETSTATE_VIB_LBZX && table >= XAM_LO && table + 4 <= XAM_HI)
			g_vib_table = (const u8 *)table;
		xa_log("xinput: vibration setting table %08x%s\n", table,
		       g_vib_table ? "" : " NOT RECOGNISED: the Vibration setting is not applied");
	}

	/* the Guide entry, if the table is the one we know (its bind entry is ours) */
	{
		u32 *cb = (u32 *)K_XAM_INPUT_CALLBACKS;

		if (cb[4] == ADDR_XamUserBind && (cb[3] >> 24) == 0x81)
			pGuideCallback = (void *)cb[3];
		xa_log("xinput: guide callback %08x (bind entry %08x)\n", cb[3], cb[4]);
	}

	/* the XAM thread makes every XAM call from here on */
	{
		HANDLE thread;
		u32 tid;

		ExCreateThread(&thread, XAM_THREAD_STACK, &tid, 0, (void *)XaXamThread, 0,
			       XAM_THREAD_FLAGS);
	}

	if (xa_xi_on[0])
		err = detour_install(&d_read, "XInputdReadState", read_state, (void *)XaReadStateHook);
	if (!err && xa_xi_on[1])
		err = detour_install(&d_caps, "XamInputGetCapabilitiesEx", caps, (void *)XaCapsHook);
	if (!err && xa_xi_on[2])
		err = detour_install(&d_set, "XamInputSetState", set_state, (void *)XaSetStateHook);
	if (!err && xa_xi_on[3])
		err = detour_install(&d_stats, "XInputdGetDeviceStats", stats, (void *)XaStatsHook);
	xa_log("xinput: installed read=%d caps=%d set=%d stats=%d\n", xa_xi_on[0], xa_xi_on[1],
	       xa_xi_on[2], xa_xi_on[3]);
	if (err) {
		/* leave no half-patched kernel behind */
		detour_remove(&d_stats);
		detour_remove(&d_set);
		detour_remove(&d_caps);
		detour_remove(&d_read);
		xa_log("xinput: hook install failed: %d\n", err);
		return err;
	}
	xa_log("xinput: hooks installed\n");
	return 0;
}

void xa_xinput_shutdown(void)
{
	/* stop our hook logic first, then let any in-flight hook call drain before we restore
	 * the original bytes (a thread caught mid-trampoline as it's rewritten would crash) */
	xa_xi_on[0] = xa_xi_on[1] = xa_xi_on[2] = xa_xi_on[3] = 0;
	xam_stop = 1;
	BARRIER();
	for (int i = 0; i < 4; i++)
		g_xpad[i].active = 0;
	xa_sleep_ms(150); /* let any in-flight hook call return before rewriting the code */
	for (int i = 0; i < 50 && xam_running; i++)
		xa_sleep_ms(20);
	if (xam_running)
		xa_log("xinput: XAM thread still busy in XAM\n");
	detour_remove(&d_set);
	detour_remove(&d_caps);
	detour_remove(&d_read);
	detour_remove(&d_stats);
	xa_log("xinput: hooks removed\n");
}

/* Notify requests run INLINE on the calling (driver) thread: XAM's dispatcher only
 * takes its direct path when KeGetCurrentProcessType()==2. The driver thread has
 * proc=0x02 (verified live via xbdm threadinfo), the XAM-hook thread proc=0x04 -
 * queueing the notify made every call take the indirect path, whose insert rejects
 * our message by default (no create, the pending counter stays 0, no toast ever).
 * The caller sets its payload fields before this runs, so ordering is preserved. */
static void do_notify(int msg);

/* driver thread: queue a request for the XAM thread; false if the queue is full */
static bool xam_request(u32 req, int slot)
{
	u32 head = xam_head;

	if (req == REQ_NOTIFY) {
		if (!pXNotifyQueueUI)
			return false;
		do_notify(slot);
		return true;
	}
	if (!xam_running || head - xam_tail >= XAM_QUEUE)
		return false;
	xam_queue[head % XAM_QUEUE] = req << 8 | (u32)slot;
	BARRIER();
	xam_head = head + 1;
	return true;
}

/* hot reload, step 1 (reversible): every request queued so far (the pads' and headsets'
 * unbinds) has been carried out by the XAM thread and it is idle; false = it is still busy
 * inside XAM after `ms` (nothing was changed: the caller refuses the reload) */
bool xa_xinput_drain(unsigned int ms)
{
	for (unsigned int i = 0; i < ms; i += 5) {
		BARRIER();
		if (!xam_running || (xam_tail == xam_head && !xam_busy))
			return true;
		xa_sleep_ms(5);
	}
	BARRIER();
	return !xam_running || (xam_tail == xam_head && !xam_busy);
}

/* hot reload, step 2 (final): this build serves no pad any more and makes no XAM call. The
 * hooks stay where they are, as pure pass-throughs, until the next build takes their slots
 * over (detour.h): nothing is unpatched, so a thread inside a hook or a trampoline just
 * finishes there. The image must stay loaded. false = the XAM thread didn't exit in time
 * (it is inside XAM; it still exits when that call returns). */
bool xa_xinput_quiesce(void)
{
	xi_quiesced = 1;
	for (int i = 0; i < 4; i++)
		g_xpad[i].active = 0;
	BARRIER();
	xam_stop = 1;
	BARRIER();
	for (int i = 0; i < 100 && xam_running; i++)
		xa_sleep_ms(10);
	xa_log("xinput: quiesced (XAM thread %s; hooks left in place as pass-throughs)\n",
	       xam_running ? "STILL INSIDE XAM" : "stopped");
	return !xam_running;
}

/* before handing over to the next build: unbind every pad in XAM, wait for the XAM
 * thread to do it, then remove the hooks */
void xa_xinput_release(void)
{
	for (int i = 0; i < 4; i++)
		xa_xinput_unbind(i);
	for (int i = 0; i < 100 && xam_running && xam_tail != xam_head; i++)
		xa_sleep_ms(10);
	xa_xinput_shutdown();
}

int xa_xinput_bind(int slot)
{
	if (!pXamUserBind || !xam_request(REQ_BIND, slot)) {
		xa_log("xinput: pad %d not bound (XAM thread not ready)\n", slot);
		return -1;
	}
	return 0;
}

void xa_xinput_unbind(int slot)
{
	struct xpad *p = &g_xpad[slot];

	if (!p->active)
		return;
	p->active = 0; /* the read hook stops serving it now; XAM hears it on the XAM thread */
	BARRIER();
	xam_request(REQ_UNBIND, slot);
}

/* XAM thread: the bodies of the requests */
static void do_bind(int slot)
{
	struct xpad *p = &g_xpad[slot];
	u8 user = 0xff;

	memset(p, 0, sizeof(*p));
	p->context = CONTEXT_BASE + slot;
	pXamUserBind(CONTROLLER_ID_BASE + slot, p->context, 0, 0, &user);
	p->user_index = user == 0xff ? 0 : user;
	p->user_bound = user != 0xff;
	BARRIER();
	p->active = 1;
	xa_log("xinput: pad %d bound as user %d\n", slot, p->user_index);
}

static void do_unbind(int slot)
{
	pXamUserBind(CONTROLLER_ID_BASE + slot, CONTEXT_BASE + slot, 0, 1 /* disconnect */, 0);
	xa_log("xinput: pad %d unbound\n", slot);
}

/* category 1, user index NULL: XAM puts it on the user holding this controller id */
static void do_headset(int slot, bool connect)
{
	int st = pXamUserBind(CONTROLLER_ID_BASE + slot, CONTEXT_BASE + slot, 1, !connect, 0);

	xa_log("xinput: pad %d headset %s (%08x)\n", slot, connect ? "bound" : "unbound",
	       (u32)st);
}

int xa_xinput_bind_headset(int slot, bool connect)
{
	if (slot < 0 || slot > 3 || !pXamUserBind ||
	    !xam_request(connect ? REQ_HEADSET : REQ_HEADSET_OFF, slot)) {
		xa_log("xinput: pad %d headset %s not queued\n", slot, connect ? "bind" : "unbind");
		return -1;
	}
	return 0;
}

bool xa_xinput_active(int slot)
{
	return slot >= 0 && slot < 4 && g_xpad[slot].active;
}

int xa_xinput_user(int slot)
{
	const struct xpad *p;

	if (slot < 0 || slot > 3)
		return -1;
	p = &g_xpad[slot];
	if (!p->active)
		return -1;
	BARRIER(); /* pairs with do_bind's: active was seen, so the index is */
	return p->user_bound ? (int)p->user_index : -1;
}

void xa_xinput_notify(int msg)
{
	if (msg >= 0 && msg < XA_NOTIFY_COUNT && !xam_request(REQ_NOTIFY, msg))
		xa_log("xinput: notification %d not queued\n", msg);
}

/* Bind the calling thread to XAM's notify TLS slot (see the block at the function
 * pointers above). Index and singleton are re-read on every call, which covers the
 * lazy TlsAlloc/singleton timing (XAM's notify init can run minutes after boot).
 * Without a singleton the first call is allowed to fail; from the second call on the
 * bind works and the merge path takes over. */
static void xa_bind_notify_tls(void)
{
	u32 idx, single;

	if (!pTlsSetValue)
		return;
	idx = *(volatile u32 *)XAM_TLS_INDEX_GLOB;
	single = *(volatile u32 *)XAM_SINGLETON_GLOB;
	if (!single)
		return;
	xa_notify_pair[0] = single;
	pTlsSetValue(idx, xa_notify_pair);
}

static void do_notify(int msg)
{
	u16 *w = notify_wide[msg];
	const char *s = notify_text[msg];
	int i;

	if (!pXNotifyQueueUI || !*s)
		return;
	for (i = 0; s[i] && i < (int)ARRAY_SIZE(notify_wide[0]) - 1; i++)
		w[i] = (u8)s[i];
	w[i] = 0;
	xa_bind_notify_tls();
	pXNotifyQueueUI(NOTIFY_TYPE, NOTIFY_USER_ANY, NOTIFY_AREAS, w, 0);
	xa_log("xinput: notified \"%s\"\n", s);
}

/* the XAM thread: runs the driver thread's requests in order */
HOOK u32 XaXamThreadC(void *context)
{
	(void)context;
	xam_running = 1;
	do_notify(XA_NOTIFY_STARTING); /* at boot this is while the other plugins still load */
	while (!xam_stop) {
		while (xam_tail != xam_head) {
			u32 r = xam_queue[xam_tail % XAM_QUEUE];
			int slot = r & 3;

			xam_busy = 1; /* before the request leaves the queue (xa_xinput_drain) */
			BARRIER();
			xam_tail++;
			if (r >> 8 == REQ_BIND)
				do_bind(slot);
			else if (r >> 8 == REQ_UNBIND)
				do_unbind(slot);
			else if (r >> 8 == REQ_GUIDE && pGuideCallback)
				pGuideCallback(CONTEXT_BASE + slot, 0, 0);
			else if (r >> 8 == REQ_GUIDE_LONG && pGuideCallback)
				pGuideCallback(CONTEXT_BASE + slot, 0, 1);
			else if (r >> 8 == REQ_NOTIFY)
				do_notify((int)(r & 0xff));
			else if (r >> 8 == REQ_HEADSET || r >> 8 == REQ_HEADSET_OFF)
				do_headset(slot, r >> 8 == REQ_HEADSET);
			BARRIER();
			xam_busy = 0;
		}
		xa_sleep_ms(10);
	}
	xam_running = 0;
	BARRIER();
	ExTerminateThread(0);
	return 0;
}

volatile u32 xa_xinput_mute;

void xa_xinput_update(int slot, const struct gip_gamepad_state *s)
{
	struct xpad *p = &g_xpad[slot];
	struct xinput_gamepad g;
	u16 b = s->buttons;
	u16 x = 0;

	if (!p->active)
		return;
	if (xa_xinput_mute) {
		memset(&p->state, 0, sizeof(p->state));
		p->guide_down_ms = 0;
		BARRIER();
		return;
	}
	if (b & GIP_GP_BTN_A) x |= XI_A;
	if (b & GIP_GP_BTN_B) x |= XI_B;
	if (b & GIP_GP_BTN_X) x |= XI_X;
	if (b & GIP_GP_BTN_Y) x |= XI_Y;
	if (b & GIP_GP_BTN_MENU) x |= XI_START;
	if (b & GIP_GP_BTN_VIEW) x |= XI_BACK;
	if (b & GIP_GP_BTN_DPAD_U) x |= XI_DPAD_UP;
	if (b & GIP_GP_BTN_DPAD_D) x |= XI_DPAD_DOWN;
	if (b & GIP_GP_BTN_DPAD_L) x |= XI_DPAD_LEFT;
	if (b & GIP_GP_BTN_DPAD_R) x |= XI_DPAD_RIGHT;
	if (b & GIP_GP_BTN_BUMPER_L) x |= XI_SHOULDER_L;
	if (b & GIP_GP_BTN_BUMPER_R) x |= XI_SHOULDER_R;
	if (b & GIP_GP_BTN_STICK_L) x |= XI_THUMB_L;
	if (b & GIP_GP_BTN_STICK_R) x |= XI_THUMB_R;

	g.buttons = x;
	g.trigger_left = (u8)((u32)s->trigger_left * 255 / 1023);
	g.trigger_right = (u8)((u32)s->trigger_right * 255 / 1023);
	g.stick_lx = s->stick_left_x;
	g.stick_ly = s->stick_left_y;
	g.stick_rx = s->stick_right_x;
	g.stick_ry = s->stick_right_y;

	p->state = g;
	BARRIER();

	/* Guide: a short press on release, a long one at 2 s while held (xa_xinput_poll) */
	if (s->guide) {
		if (!p->guide_down_ms) {
			p->guide_down_ms = xa_now_ms();
			p->guide_long_sent = 0;
		}
	} else if (p->guide_down_ms) {
		u64 held = xa_now_ms() - p->guide_down_ms;

		p->guide_down_ms = 0;
		if (!p->guide_long_sent && held < GUIDE_LONG_PRESS_MS && xa_guide && pGuideCallback &&
		    xa_now_ms() - p->guide_sent_ms >= GUIDE_DEBOUNCE_MS) {
			p->guide_sent_ms = xa_now_ms();
			xa_log("xinput: pad %d guide\n", slot);
			xam_request(REQ_GUIDE, slot);
		}
		p->guide_long_sent = 0;
	}
}

void xa_xinput_poll(int slot)
{
	struct xpad *p = &g_xpad[slot];

	/* held to 2 s with no report in between: the controller only reports changes */
	if (!p->active || xa_xinput_mute || !p->guide_down_ms || p->guide_long_sent ||
	    xa_now_ms() - p->guide_down_ms < GUIDE_LONG_PRESS_MS || !xa_guide || !pGuideCallback)
		return;
	p->guide_long_sent = 1;
	p->guide_sent_ms = xa_now_ms();
	xa_log("xinput: pad %d guide held: turn-off menu\n", slot);
	xam_request(REQ_GUIDE_LONG, slot);
}

int xa_xinput_vibration(int slot)
{
	const struct xpad *p;

	if (!g_vib_table || slot < 0 || slot > 3)
		return -1;
	p = &g_xpad[slot];
	if (!p->active || p->user_index > 3)
		return -1;
	/* a plain byte load: XAM writes it from its work item, a stale value is one pass old */
	return g_vib_table[p->user_index] ? 1 : 0;
}

u32 xa_xinput_vibration_table(void)
{
	return (u32)g_vib_table;
}

bool xa_xinput_take_rumble(int slot, u8 *left, u8 *right)
{
	struct xpad *p = &g_xpad[slot];

	if (!p->rumble_pending)
		return false;
	p->rumble_pending = 0;
	BARRIER();
	*left = p->rumble_left;
	*right = p->rumble_right;
	return true;
}
