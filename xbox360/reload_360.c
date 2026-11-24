// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Hot reload (handover.h, docs/HOT-RELOAD.md): the running build releasing the controllers
 * and the hooks, and a slot build taking them over, with the trace points and gates that
 * xw.py reload --step drives. Development machinery: compiled into XW_DEV builds only (a
 * release build gets xw_priv.h's stubs). Driver thread, except where a function says it runs
 * on Usbd's DPC.
 */
#include <xecore/xboxkrnl.h>

#include "../core/gip.h"
#include "../core/platform.h"
#include "detour.h"
#include "eq_360.h"
#include "file_360.h"
#include "game_360.h"
#include "hud_360.h"
#include "settings_360.h"
#include "team_360.h"
#include "voice_360.h"
#include "xinput_360.h"
#include "xw_priv.h"

#ifdef XW_DEV

/* ---- hot reload (handover.h, docs/HOT-RELOAD.md) --------------------------------------------
 *
 * Releasing (do_release, the driver thread of the running build): everything that can fail is
 * done first and can be undone (release_resume); the hand-over itself is one routine on
 * Usbd's DPC (commit_dpc) that re-checks every condition there, where nothing of ours can
 * change under it, and either does all of it or nothing. After it this build has no thread
 * and calls nothing; its image stays loaded.
 *
 * Taking over (takeover_import before the hooks, takeover_adopt after the class driver is
 * linked): the next build inherits the hooks and adopts each controller on Usbd's DPC.
 */

#define HO_XAM_WAIT_MS 2000       /* the XAM thread carries out the unbinds */
#define HO_TX_WAIT_MS 300         /* the controllers' OUT queues empty */
#define HO_AUDIO_DRAIN_MS 300     /* the queued isochronous blocks complete (40 ms of them) */
#define HO_DPC_WAIT_MS 1000       /* a routine posted to Usbd's DPC runs */
#define HO_TAKEOVER_WAIT_MS 60000 /* a slot build waits this long for xw.py's word */
#define ORD_XexLoadImage 409
#define KTHREAD_PROCESS_TYPE 0x73 /* as loader.c: 2 = system */

/* A routine posted to Usbd's DPC that the thread waits for, with a way to give up that can't
 * race the routine: whoever swaps `claim` from 0 first owns the outcome. The routine runs
 * only if it wins (once_enter); a thread that wins instead knows the routine will never run,
 * however late its DPC comes. */
struct dpc_once {
	volatile u32 claim; /* 0 free, 1 the routine took it, 2 the thread gave up */
	volatile u32 done;
	u32 result;
};

static bool once_enter(struct dpc_once *o)
{
	return __sync_bool_compare_and_swap((u32 *)&o->claim, 0, 1);
}

static void once_leave(struct dpc_once *o, u32 result)
{
	o->result = result;
	BARRIER();
	o->done = 1;
}

/* 1 = the routine ran (o->result); 0 = it didn't within ms and never will */
static int once_run(void (*fn)(void *), struct dpc_once *o, unsigned int ms)
{
	u64 until = xa_now_ms() + ms;

	o->done = 0;
	o->claim = 0;
	BARRIER();
	xa_usbd_run(fn, o);
	while (!o->done) {
		if (xa_now_ms() > until &&
		    __sync_bool_compare_and_swap((u32 *)&o->claim, 0, 2))
			return 0;
		xa_sleep_ms(1);
	}
	BARRIER();
	return 1;
}

/* Staged gates (docs/PLAN.md WP3.1, docs/HOT-RELOAD.md "Experiments"): the step that follows
 * a gate runs only once xbdm has raised g_ho.cmd_gate to the gate's number, so xw.py reload
 * --step can run a reload one step at a time and the trace names the step a freeze is in.
 * cmd_gate is 99 at load: nothing holds. Releasing gates (R) hold only during a release,
 * taking-over gates (T) only in a build that is taking over; a refused release's
 * release_resume never holds. A gate nobody raises opens by itself after HO_GATE_MAX_MS.
 * Driver thread only. */
#define HO_GATE_MAX_MS 300000
static int g_taking; /* this build was told to take over from a previous one */

/* whether gate n of that side lets its step run now */
static bool ho_gate_open(u32 side, u32 n)
{
	if (g_ho.cmd_gate >= n)
		return true;
	return side == 'R' ? g_ho.state != HO_RELEASING : !g_taking || g_ho.state == HO_RELEASING;
}

void ho_gate(u32 side, u32 n, const char *what)
{
	unsigned int ms;

	if (ho_gate_open(side, n))
		return;
	xa_log("reload: gate %c%u (%s): holding (xw.py reload --step raises it)\n", side, n, what);
	trace(TR_GATE_HOLD | (side << 8 | n) << 16);
	g_ho.gate_at = side << 8 | n;
	for (ms = 0; g_ho.cmd_gate < n && ms < HO_GATE_MAX_MS; ms += 10)
		wait_ms(10);
	g_ho.gate_at = 0;
	if (g_ho.cmd_gate < n) {
		trace(TR_GATE_TIMEOUT | (side << 8 | n) << 16);
		xa_log("reload: gate %c%u: nobody raised it in %u s: going on\n", side, n,
		       HO_GATE_MAX_MS / 1000);
	} else {
		trace(TR_GATE_OPEN | (side << 8 | n) << 16);
		xa_log("reload: gate %c%u: %s\n", side, n, what);
	}
}

static const char *const refuse_text[] = {
	[HO_R_NONE] = "?",
	[HO_R_NOT_RUNNING] = "the driver is still starting up",
	[HO_R_AUDIO_BROKEN] = "headset audio is broken (a USB request never completed, so its "
			      "blocks may still be on Usbd): only a reboot is safe",
	[HO_R_AUDIO_OPEN] = "the headset's audio endpoints didn't close, or audio blocks are "
			    "still queued",
	[HO_R_REMOVING] = "a controller is being removed right now (try again)",
	[HO_R_SETUP] = "a controller is still being set up (try again)",
	[HO_R_XAM_BUSY] = "the XAM thread is still inside XAM (the unbinds aren't done; is a "
			  "system dialog open? try again)",
	[HO_R_TX] = "a controller's outgoing packets didn't drain",
	[HO_R_DPC] = "the hand-over routine never ran on Usbd's DPC",
	[HO_R_NOT_OWNER] = "Usbd's record of a controller doesn't name this build's driver",
};

/* Whether the controllers can be handed over as they are; a HO_R_* reason if not. `final`
 * adds what must hold at the commit itself (audio closed and drained, OUT queues empty). On
 * the thread it is a preview; commit_dpc runs it again on Usbd's DPC, where it is exact. */
static u32 commit_check(int final)
{
	if (g_audio.broken)
		return HO_R_AUDIO_BROKEN;
	if (final) {
		if (g_audio.opened || g_audio.closing || g_audio.close_waiting || aud_queued())
			return HO_R_AUDIO_OPEN;
		for (int i = 0; i < AUD_OUT_BLOCKS; i++)
			if (g_audio.out[i].busy || g_audio.out[i].staged)
				return HO_R_AUDIO_OPEN;
	}
	for (int i = 0; i < PORTS; i++) {
		struct xw_port *p = &g_ports[i];
		void *node = p->node;

		if (p->removing)
			return HO_R_REMOVING;
		if (!p->claimed)
			continue;
		if (p->setup_step)
			return HO_R_SETUP; /* a control transfer of ours is on the default endpoint */
		if (!node)
			continue; /* claimed, but its endpoints never opened: it stays with us */
		if (p->gone)
			return HO_R_REMOVING;
		if (((const u32 *)node)[1] != (u32)&g_driver)
			return HO_R_NOT_OWNER;
		if (final && (p->tx_inflight || p->out.busy || p->tx_head != p->tx_tail))
			return HO_R_TX;
	}
	return 0;
}

static struct dpc_once g_commit;
_Static_assert(PORTS == XW_HO_PORTS, "g_ho.ports[] is indexed by port");

/* The headset's isochronous blocks, before its endpoints are closed: no block is re-queued
 * or submitted any more (streaming = 0: aud_can_queue), and the ones on Usbd are left to
 * complete by themselves, which they do at their frames whatever the device does. They are
 * never cancelled, and here not even aborted by a close: if they don't drain, the streams
 * are started again and the release is refused. 0 = nothing is queued. Driver thread. */
static int aud_drain(void)
{
	u64 until = xa_now_ms() + HO_AUDIO_DRAIN_MS;
	int was = g_audio.streaming;

	g_audio.streaming = 0;
	BARRIER();
	for (;;) {
		int busy = aud_queued();

		for (int i = 0; i < AUD_OUT_BLOCKS; i++)
			busy |= g_audio.out[i].busy || g_audio.out[i].staged;
		if (!busy)
			return 0;
		if (xa_now_ms() > until)
			break;
		xa_sleep_ms(1);
	}
	xa_log("reload: audio blocks still queued after %u ms (opened %u, closing %d)\n",
	       HO_AUDIO_DRAIN_MS, g_audio.opened, g_audio.closing);
	if (was && g_audio.owner && !g_audio.closing) {
		g_audio.streaming = 1;
		BARRIER();
		if (aud_live_stage() >= 3)
			xa_usbd_run(aud_start_in_dpc, NULL);
	}
	return -1;
}

/* The hand-over, on Usbd's DPC: nothing of Usbd's, no completion and no AddDevice or
 * RemoveDevice can run between the check and the last store. */
static void commit_dpc(void *arg)
{
	struct dpc_once *o = arg;
	u32 r;

	if (!once_enter(o))
		return;
	trace(TR_COMMIT_BEGIN);
	r = commit_check(1);
	if (r) {
		trace(TR_COMMIT_REFUSED);
		once_leave(o, r); /* nothing was changed */
		return;
	}
	/* no device is offered to this build again (as xa_plugin.c's unlink_driver) */
	((struct usbd_class_driver *)g_driver.blink)->flink = g_driver.flink;
	((struct usbd_class_driver *)g_driver.flink)->blink = g_driver.blink;
	BARRIER();
	trace(TR_COMMIT_UNLINKED);
	for (int i = 0; i < PORTS; i++) {
		struct xw_port *p = &g_ports[i];
		struct xw_ho_port *e = &g_ho.ports[i];

		e->node = 0;
		if (!p->node)
			continue;
		e->driver = (u32)&g_driver;
		e->vid = p->vid;
		e->pid = p->pid;
		e->ep_in = p->ep_in;
		e->ep_out = p->ep_out;
		e->max_in = p->max_in;
		e->max_out = p->max_out;
		e->interval_in = p->interval_in;
		e->interval_out = p->interval_out;
		e->h_in = p->in[0].c.t.endpoint;
		e->h_out = p->out.c.t.endpoint;
		e->h_ctrl = p->ctrl.c.t.endpoint;
		e->aud_ep_in = p->aud_ep_in;
		e->aud_ep_out = p->aud_ep_out;
		e->aud_max_in = p->aud_max_in;
		e->aud_max_out = p->aud_max_out;
		e->aud_desc = p->aud_desc;
		e->flags = p->ho_flags;
		e->hs_fmt = p->ho_hs_fmt;
		/* the one IN transfer that may still be queued: ours, or (we adopted this
		 * controller ourselves and it has been silent since) an earlier build's */
		e->in_fwd = 0;
		e->in_busy = p->in[0].busy ? 1 : 0;
		e->in_entry = p->in_wait ? (u32)p->in_src : p->in[0].busy ? (u32)e : 0;
		p->handed = 1; /* completions stop re-queueing; nothing new is submitted */
		BARRIER();
		e->node = (u32)p->node;
	}
	trace(TR_COMMIT_DONE);
	once_leave(o, 0);
}

static void ho_refuse(u32 code)
{
	g_ho.refuse = code;
	g_ho.refusals++;
	xa_log("reload: REFUSED: %s. Nothing was handed over; this build keeps running\n",
	       code < ARRAY_SIZE(refuse_text) && refuse_text[code] ? refuse_text[code] : "?");
}

/* a release refused after the pads and the headset were taken down: put them back */
static void release_resume(void)
{
	xf_resume();
	xq_resume();
	xs_resume();
	xt_resume();
	hud_resume();
	xv_resume();
	xg_resume();
	for (int i = 0; i < PORTS; i++) {
		struct xw_port *p = &g_ports[i];

		if (!p->gc || !p->node || p->gone)
			continue;
		if (p->pad < 0 && (p->ho_flags & HO_PF_READY) && p->gc->ready)
			pad_ready(p->gc);
		if (p->ho_flags & HO_PF_HEADSET)
			gip_headset_reidentify(p->gc);
	}
	xa_log("reload: pads and headset restored\n");
}

/* cmd_release 2: the next build is loaded by this thread (a system thread), so no loader
 * title runs and the title on screen keeps running. As loader.c: XexLoadImage is resolved at
 * run time, and the thread's process type is 2 (system) around the call. */
static void load_next(void)
{
	NTSTATUS (*load)(const char *, u32, u32, HANDLE *) = 0;
	HANDLE krnl, module = 0;
	u8 *type = current_kthread() + KTHREAD_PROCESS_TYPE, old;
	NTSTATUS st;

	g_ho.load_path[sizeof(g_ho.load_path) - 1] = 0;
	if (!g_ho.load_path[0]) {
		xa_log("reload: no module path given: nothing loaded\n");
		return;
	}
	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) >= 0)
		XexGetProcedureAddress(krnl, ORD_XexLoadImage, (void **)&load);
	if (!load) {
		xa_log("reload: XexLoadImage not found: nothing loaded\n");
		g_ho.load_status = 0xC0000139;
		return;
	}
	old = *type;
	xa_log("reload: loading %s from this thread (process type %u)\n", g_ho.load_path, old);
	g_ho.load_status = 0xEEEEEEEE; /* still there = XexLoadImage never returned */
	*type = 2;
	st = load(g_ho.load_path, 8 /* system DLL */, 0, &module);
	*type = old;
	g_ho.load_status = (u32)st;
	xa_log("reload: XexLoadImage: %08x\n", (u32)st);
}

/* xw.py reload wrote cmd_release. Returns only if the release was refused. */
void do_release(u32 cmd)
{
	int touched = 0;
	u64 until;
	u32 r;

	g_ho.state = HO_RELEASING;
	trace(TR_REL_BEGIN);
	xa_log("reload: release asked for (%u): handing over to the next build\n", cmd);
	r = commit_check(0);
	if (r) {
		ho_refuse(r);
		goto refused;
	}
	trace(TR_REL_CHECKED);
	ho_gate('R', 1, "drain the headset's audio blocks");
	for (int i = 0; i < PORTS; i++) {
		struct xw_port *p = &g_ports[i];

		p->ho_hs_fmt = p->gc && p->gc->hs_known_in && p->gc->hs_known_out ?
			       p->gc->hs_known_in | p->gc->hs_known_out << 8 | HO_HS_FMT_VALID : 0;
		p->ho_flags = (p->gc && (p->gc->hs_seen || p->ho_hs_fmt) ? HO_PF_HEADSET : 0) |
			      (p->gc && p->pad >= 0 ? HO_PF_READY : 0) |
			      (p->gc && p->pad >= 0 && xv_mic_muted(p->pad) ? HO_PF_MUTED : 0);
	}

	/* 1. headset audio: the isochronous blocks left to complete (never cancelled), then off
	 * for the 360, the endpoints closed, the interface back on alternate 0 */
	if (g_audio.owner && aud_drain()) {
		ho_refuse(HO_R_AUDIO_OPEN);
		goto refused; /* nothing else was touched: the streams run again */
	}
	trace(TR_REL_AUDIO_DRAINED);
	touched = 1;
	ho_gate('R', 2, "headset off: audio endpoints closed, interface 1 to alternate 0");
	if (g_audio.owner)
		aud_disable(g_audio.owner, "handing over to the next build");
	trace(TR_REL_AUDIO_OFF);
	if (g_audio.broken) {
		ho_refuse(HO_R_AUDIO_BROKEN);
		goto refused;
	}
	if (g_audio.owner || g_audio.opened || g_audio.closing) {
		ho_refuse(HO_R_AUDIO_OPEN);
		goto refused;
	}

	/* 2. game audio, the pads and their headsets in XAM, the virtual headset, the HUD.
	 * The unbinds are XAM calls: queued for the XAM thread, then waited for. */
	ho_gate('R', 3, "game audio off: MEC client unregistered");
	xg_quiesce();
	trace(TR_REL_GAME_QUIESCED);
	ho_gate('R', 4, "pads and headsets unbound in XAM, XAM thread drained");
	for (int i = 0; i < PORTS; i++)
		if (g_ports[i].gc)
			pad_release(&g_ports[i]);
	trace(TR_REL_PADS_RELEASED);
	xv_quiesce();
	hud_quiesce();
	xt_quiesce();
	xs_quiesce();
	xq_quiesce();
	xf_quiesce();
	trace(TR_REL_QUIESCED);
	if (!xa_xinput_drain(HO_XAM_WAIT_MS)) {
		ho_refuse(HO_R_XAM_BUSY);
		goto refused;
	}
	trace(TR_REL_XAM_DRAINED);

	/* 3. what the above sent to the controllers has left (nothing new is sent: the thread
	 * is here, not in its loop) */
	until = xa_now_ms() + HO_TX_WAIT_MS;
	while ((r = commit_check(1)) == HO_R_TX && xa_now_ms() < until)
		xa_sleep_ms(2);
	if (r) {
		ho_refuse(r);
		goto refused;
	}
	trace(TR_REL_TX_DRAINED);

	/* 4. the hand-over itself */
	ho_gate('R', 5, "commit on Usbd's DPC (the class driver unlinked, controllers on offer)");
	trace(TR_REL_COMMIT);
	if (!once_run(commit_dpc, &g_commit, HO_DPC_WAIT_MS)) {
		ho_refuse(HO_R_DPC);
		goto refused;
	}
	if (g_commit.result) {
		ho_refuse(g_commit.result);
		goto refused;
	}

	/* released: no way back from here, and nothing below can fail */
	trace(TR_REL_COMMITTED);
	ho_gate('R', 6, "the XAM thread and the driver thread end");
	xa_xinput_quiesce();
	trace(TR_REL_XINPUT_QUIESCED);
	g_xw.state = XW_RELEASED;
	for (int i = 0; i < PORTS; i++) {
		const struct xw_ho_port *e = &g_ho.ports[i];

		if (e->node)
			xa_log("reload: port %d: device %08x (%04x:%04x) on offer%s%s\n", i, e->node,
			       e->vid, e->pid, e->in_entry ? ", an IN transfer still queued" : "",
			       e->flags & HO_PF_HEADSET ? ", headset" : "");
		else if (g_ports[i].claimed)
			xa_log("reload: port %d: device %08x was never usable: it stays with this "
			       "build until it is unplugged\n", i, (u32)g_ports[i].claimed);
	}
	g_ho.releases++;
	BARRIER();
	g_ho.state = HO_RELEASED;
	xa_log("reload: released. This build is quiescent and stays loaded; its thread ends\n");
	trace(TR_REL_DONE);
	if (cmd == 2) {
		trace(TR_REL_LOAD_NEXT);
		load_next();
		trace(TR_REL_LOADED);
	}
	ExTerminateThread(0);
	for (;;) /* not reached */
		xa_sleep_ms(1000);

refused:
	trace(TR_REL_REFUSED);
	if (touched)
		release_resume();
	trace(TR_REL_RESUMED);
	g_ho.state = HO_RUNNING;
}

/* ---- hot reload: the new build's side ------------------------------------------------------- */

struct xw_handover *g_pred; /* the released build's block, once checked */
static struct dpc_once g_adopt;

static bool ho_mapped(const void *p, u32 n)
{
	const u8 *a = p;

	if ((u32)a < 0x80000000u || ((u32)a & 3))
		return false;
	for (u32 off = 0; off < n; off += 0x1000)
		if (!MmIsAddressValid((void *)(a + off)))
			return false;
	return MmIsAddressValid((void *)(a + n - 1));
}

static bool take_fail(u32 code, const char *why)
{
	g_ho.take_error = code;
	trace(TR_TAKE_FAILED | code << 16);
	BARRIER(); /* xw.py reads the reason once it sees the state */
	g_ho.take_state = HO_T_FAILED;
	g_ho.state = HO_IDLE;
	xa_log("reload: NOT taking over: %s. This build stays idle: it hooks nothing and claims "
	       "no controller (the previous build, if released, still holds them: run xw.py "
	       "reload again, or reboot)\n", why);
	return false;
}

/* Before anything is hooked. A slot build (XW_SLOT > 0) exists only to replace a running
 * build, so it waits for xw.py to name that build's handover block, checks it and inherits
 * its hooks; false = it must not start (it would patch functions the previous build has
 * already patched, and fight it for the controllers). xw.xex itself (slot 0) starts at once. */
bool takeover_import(void)
{
	struct xw_handover *pred;
	unsigned int ms;
	u32 cmd = 0;

	if (!XW_SLOT)
		return true;
	xa_log("reload: slot %u build: waiting for xw.py to say what it replaces\n", XW_SLOT);
	trace(TR_TAKE_WAIT);
	/* the title as this thread sees it: xw.py reload --settle waits for it to be stable
	 * (the loader title launches Aurora about 1 s after it loaded this build) */
	for (ms = 0; !(cmd = g_ho.cmd_takeover) && ms < HO_TAKEOVER_WAIT_MS; ms += 10)
		wait_ms(10);
	ho_title();
	if (!cmd)
		return take_fail(HO_E_TIMEOUT, "xw.py never said what to take over");
	trace(TR_TAKE_CMD);
	xa_log("reload: take over from %08x asked for, title %08x\n", cmd, g_ho.title);
	if (cmd == 0xFFFFFFFF) {
		g_ho.take_state = HO_T_ALONE;
		xa_log("reload: no previous build: a normal start\n");
		return true;
	}
	pred = (struct xw_handover *)cmd;
	g_taking = 1;
	ho_gate('T', 1, "check the previous build's block, inherit its hooks");
	if (!ho_mapped(pred, sizeof(*pred)))
		return take_fail(HO_E_UNMAPPED, "the address given isn't a mapped block");
	if (pred->magic != XW_HO_MAGIC)
		return take_fail(HO_E_MAGIC, "there is no handover block at the address given");
	if (pred->version != XW_HO_VERSION || pred->size != sizeof(*pred))
		return take_fail(HO_E_VERSION, "the previous build's handover block is another "
					       "version");
	if (pred->state != HO_RELEASED)
		return take_fail(HO_E_NOT_RELEASED, "the previous build hasn't released");
	g_pred = pred;
	if (ho_mapped((const void *)pred->detours, sizeof(struct detour_registry)))
		g_ho.inherited = (u32)detour_inherit((const struct detour_registry *)pred->detours);
	xa_log("reload: taking over from slot %u (block %08x): %u hook(s) inherited\n", pred->slot,
	       cmd, g_ho.inherited);
	g_ho.take_state = HO_T_IMPORTED;
	trace(TR_TAKE_IMPORTED);
	return true;
}

/* The IN transfer an earlier build left queued on an adopted controller has completed
 * (handover.h: in_fwd). Called by that build's completion, on Usbd's DPC, as plain C. */
BODY void XwInForwardC(void *node, const u8 *data, u32 len, long status)
{
	struct xw_port *p = NULL;

	for (int i = 0; i < PORTS; i++)
		if (g_ports[i].claimed == node && g_ports[i].in_wait)
			p = &g_ports[i];
	if (!p) {
		trace(TR_IN_FWD | 0xFF0000);
		return; /* removed meanwhile (RemoveDevice clears in_wait) */
	}
	trace(TR_PORT(TR_IN_FWD, p));
	p->in_wait = 0;
	p->in_src = NULL;
	g_xw.in_done++;
	g_xw.in_last_status = (u32)status;
	if (status >= 0 && data) {
		if (len > p->max_in)
			len = p->max_in;
		if (len > PKT_MAX)
			len = PKT_MAX;
		p->rx_count++;
		if (p->rx_write - p->rx_read >= RX_SLOTS) {
			p->rx_dropped++;
		} else {
			u32 s = p->rx_write % RX_SLOTS;

			memset(p->rx[s], 0, PKT_MAX);
			memcpy(p->rx[s], data, len);
			p->rx_len[s] = (u8)len;
			BARRIER();
			p->rx_write++;
		}
	} else if ((u32)status != STATUS_CANCELLED) {
		p->rx_errors++;
	}
	/* our own transfer from here on, exactly when XwInDoneC would re-queue one. If we
	 * have released as well meanwhile, the next build sees the entry's in_busy clear.
	 * Before gate T5 (reload --step) the thread's start_port queues it instead. */
	if (!p->handed && (u32)status != STATUS_CANCELLED && p->node && !p->gone &&
	    !p->removing && g_xw.go >= 5 && !p->in[0].busy && ho_gate_open('T', 5)) {
		queue_in(p, 0);
		trace(TR_PORT(TR_IN_FWD_QUEUED, p));
	}
}

/* On Usbd's DPC: each controller the previous build left on offer becomes ours. Nothing is
 * sent to the device and nothing is opened or closed: the endpoint handles are copied, and
 * Usbd's record is pointed at our class driver, so its removal comes to us. With cmd_mode 1
 * the device is reset instead: Usbd then has the previous build close it (its callbacks
 * still work), and it re-enumerates into our driver like a replug. */
static void adopt_dpc(void *arg)
{
	struct dpc_once *o = arg;
	struct xw_handover *pred = g_pred;

	if (!once_enter(o))
		return;
	trace(TR_ADOPT_BEGIN);
	for (int i = 0; i < XW_HO_PORTS; i++) {
		struct xw_ho_port *e = &pred->ports[i], *ie;
		void *node = (void *)e->node;
		struct xw_port *p = NULL;

		if (!node)
			continue;
		if (!ho_mapped(node, 16) || ((const u32 *)node)[1] != e->driver) {
			xa_log("reload: device %08x: Usbd's record doesn't name the previous build's "
			       "driver (%08x): left alone\n", (u32)node, e->driver);
			trace(TR_ADOPT_SKIP | (u32)i << 16);
			g_ho.skipped++;
			continue;
		}
		if (g_ho.cmd_mode == 1) {
			xa_log("reload: device %08x: USB reset (re-enumerating)\n", (u32)node);
			g_ho.resets++;
			trace(TR_ADOPT_RESET | (u32)i << 16);
			usbd.reset_device(node, 1);
			continue;
		}
		for (int k = 0; k < PORTS; k++) {
			if (!g_ports[k].claimed && !g_ports[k].removing) {
				p = &g_ports[k];
				break;
			}
		}
		if (!p || !e->h_in || !e->h_out || !e->h_ctrl || !e->max_in || e->max_in > PKT_MAX ||
		    !e->max_out || e->max_out > PKT_MAX) {
			xa_log("reload: device %08x: %s: left with the previous build\n", (u32)node,
			       p ? "its endpoint data makes no sense" : "no port free");
			trace(TR_ADOPT_SKIP | (u32)i << 16);
			g_ho.skipped++;
			continue;
		}
		p->claimed = node;
		p->vid = (u16)e->vid;
		p->pid = (u16)e->pid;
		p->ep_in = (u8)e->ep_in;
		p->ep_out = (u8)e->ep_out;
		p->max_in = (u16)e->max_in;
		p->max_out = (u16)e->max_out;
		p->interval_in = (u8)e->interval_in;
		p->interval_out = (u8)e->interval_out;
		p->aud_ep_in = (u8)e->aud_ep_in;
		p->aud_ep_out = (u8)e->aud_ep_out;
		p->aud_max_in = (u16)e->aud_max_in;
		p->aud_max_out = (u16)e->aud_max_out;
		p->aud_desc = (u8)e->aud_desc;
		p->gep_in = p->gep_out = 0;
		memset(p->in, 0, sizeof(p->in));
		memset(&p->out, 0, sizeof(p->out));
		memset(&p->ctrl, 0, sizeof(p->ctrl));
		for (int k = 0; k < IN_COUNT; k++)
			p->in[k].c.t.endpoint = e->h_in;
		p->out.c.t.endpoint = e->h_out;
		p->ctrl.c.t.endpoint = e->h_ctrl;
		p->open_status[0] = p->open_status[1] = p->open_status[2] = 0; /* open: ours to close */
		p->setup_step = 0;
		p->close_step = 0;
		p->handed = 0;
		p->ho_flags = e->flags;
		p->ho_hs_fmt = (e->hs_fmt & HO_HS_FMT_VALID) ? e->hs_fmt : 0;
		p->rx_write = p->rx_read = 0;
		p->tx_head = p->tx_tail = 0;
		p->tx_inflight = 0;
		p->gone = 0;
		p->trace_first = 3;
		/* an IN transfer of an earlier build still on the endpoint: ours waits for it */
		ie = (struct xw_ho_port *)e->in_entry;
		p->in_wait = 0;
		p->in_src = NULL;
		if (ie && ho_mapped(ie, sizeof(*ie)) && ie->in_busy) {
			p->in_src = ie;
			p->in_wait = 1;
			BARRIER();
			ie->in_fwd = (u32)XwInForwardC;
		}
		((u32 *)node)[1] = (u32)&g_driver; /* its RemoveDevice is ours from here on */
		e->node = 0;                       /* and the previous build's offer is taken */
		BARRIER();
		p->node = node;
		p->session++;
		p->adopt_session = p->session;
		g_ho.adopted++;
		trace(TR_PORT(TR_ADOPT_PORT, p));
		xa_log("reload: port %d: device %08x (%04x:%04x) adopted, no USB reset%s\n",
		       (int)(p - g_ports), (u32)node, p->vid, p->pid,
		       p->in_wait ? "; waiting for the previous build's IN transfer" : "");
		xa_log("reload: port %d: handles copied: IN %02x %08x (max %u, every %u ms), OUT %02x "
		       "%08x (max %u), default %08x; IN transfer of entry %08x %s\n",
		       (int)(p - g_ports), p->ep_in, e->h_in, p->max_in, p->interval_in, p->ep_out,
		       e->h_out, p->max_out, e->h_ctrl, (u32)ie,
		       p->in_wait ? "still queued" : ie ? "already completed" : "(none)");
	}
	trace(TR_ADOPT_DONE);
	once_leave(o, 0);
}

/* after the class driver is linked: new devices come to us already */
void takeover_adopt(void)
{
	if (!g_pred)
		return;
	trace(TR_TAKE_ADOPT);
	if (!once_run(adopt_dpc, &g_adopt, HO_DPC_WAIT_MS)) {
		g_ho.take_error = HO_E_DPC;
		xa_log("reload: the adoption never ran on Usbd's DPC: the controllers stay with "
		       "the previous build (replug them)\n");
	}
	xa_log("reload: take-over done: %u controller(s) adopted, %u reset, %u left alone\n",
	       g_ho.adopted, g_ho.resets, g_ho.skipped);
	BARRIER();
	g_ho.take_state = HO_T_DONE;
	trace(TR_TAKE_ADOPTED);
}

#endif
