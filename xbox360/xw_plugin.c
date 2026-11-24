// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * xw: wired Xbox One / Series controllers on the Xbox 360, as a system DLL (kernel 17559).
 *
 * Any USB device whose interface 0 is GIP (class/subclass/protocol FF/47/D0, as Linux's
 * xone-wired and xpad match it) is claimed by a class driver linked into the kernel's
 * runtime USB driver list, the same way xa.xex claims the wireless adapter. Up to four
 * controllers, each with its own interrupt IN/OUT endpoint pair. A driver thread runs
 * core/gip.c for each one (announce, identify, power on, input, rumble, Guide) and hands
 * the state to xinput_360.c, which presents it to the system as an XInput controller.
 *
 * No security handshake: on a cable, a GIP controller reports input once it's powered on
 * (xpad has always driven them that way), and there's no radio link to encrypt.
 *
 * Kernel context rules (docs/INTERNALS.md) apply unchanged: every kernel -> plugin call
 * goes through an NV64 thunk (thunks_wired.s), Usbd is only called on Usbd's DPC
 * (completions and xa_usbd_run), and the driver thread never calls Usbd directly.
 *
 * Headset audio (the jack, GIP client 1): interface 1 alternate 1, isochronous IN/OUT, one
 * 8 ms block (8 frames) per transfer, for one headset at a time. Everything is gated by
 * g_xw.audio.stage (xbdm, xw.py audio N), 4 at boot; see the "headset audio" section.
 *
 * Development machinery is in XW_DEV builds only (docs/DEVELOPMENT.md): hot reload
 * (reload_360.c), the audio self-test, the mic dump and the usbreset / hsrestart / mecreset
 * commands (selftest_360.c). xw_priv.h has what the three files share and the release stubs.
 */
#include <stdlib.h>

#include <xecore/xboxkrnl.h>

#include "../core/gip.h"
#include "../core/platform.h"
#include "platform_360.h"
#include "xinput_360.h"
#include "voice_360.h"
#include "game_360.h"
#include "diag_360.h"
#include "hud_360.h"
#include "panel_360.h"
#include "team_360.h"
#include "bridge_360.h"
#include "handover.h"
#include "eq_360.h"
#include "file_360.h"
#include "settings_360.h"
#include "rumble_360.h"
#include "xw_priv.h"

/* development-only switches: never in a release build */
#if !defined(XW_DEV) && (defined(XW_STAGED) || defined(XW_NO_AUTH) || XW_SLOT)
#error "XW_STAGED, XW_NO_AUTH and slot builds (XW_SLOT, hot reload) need XW_DEV=1"
#endif

#ifndef XA_NAME
#define XA_NAME "xw.xex"
#endif

/* the GIP data interface */
#define GIP_CLASS 0xFF
#define GIP_SUBCLASS 0x47
#define GIP_PROTOCOL 0xD0

#define DRAIN_MAX 32   /* packets handed to the core per controller per loop pass */

/* UsbdGetEndpointDescriptor(node, index, type, direction): as hiddriver360 uses it */
#define ORD_UsbdGetEndpointDescriptor 744
#define EP_DIR_IN 1
#define EP_DIR_OUT 0

/* ExCreateThread: system thread pinned to hw thread 2, as xa.xex (docs/INTERNALS.md) */
#define THREAD_FLAGS (0x2 | (1u << 2) << 24)
#define THREAD_STACK 0x10000
#define THREAD_PRIORITY 9
#define KTHREAD_PRIORITY 0x70
#define KTHREAD_PRIORITY_MIN 0xc8
#define KTHREAD_PRIORITY_BASE 0xc9
#define KTHREAD_PRIORITY_MAX 0xca

/* ---- kernel 17559 (the same addresses and checks as xa_plugin.c) ------------------------ */
#define K_DRIVER_LIST 0x801A8048
#define K_USBD_TIMER_DPC 0x801721D0
#define K_WGC_DRIVER 0x80172598
#define K_UNRECOGNIZED 0x801A7FA0
#define K_UNRECOGNIZED_COUNT 20
#define ORD_UsbdTitleDriverResetAllUnrecognizedPorts 890
#define RESCAN_CTX_SIZE 0x1000
#define RESCAN_EVENT 0x1c
#define ORD_XexExecutableModuleHandle 403
#define TITLE_WAIT_MS 60000
#define TITLE_SETTLE_MS 8000
#define RESCAN_WAIT_MS 10000
#define BOOT_RETRY_MS 30000

/* thunks (thunks_wired.s) */
void XwLinkDpc(struct kdpc360 *dpc, void *context, void *arg1, void *arg2);
u32 XwMatch(const u8 *ifdesc, const u8 *devdesc, struct usbd_class_driver *self);
void XwAddDevice(void *node);
void XwRemoveDevice(void *node);
long XwInDone(struct usbd_transfer *t, long status);
long XwOutDone(struct usbd_transfer *t, long status);
long XwCloseDone(struct usbd_transfer *t, long status);
long XwCtrlDone(struct usbd_transfer *t, long status);
long XwIsoInDone(struct usbd_isoch_transfer *t, const long *status, const u16 *length);
long XwIsoOutDone(struct usbd_isoch_transfer *t, const long *status, const u16 *length);
long XwIsoCloseDone(struct usbd_transfer *t, long status);
u32 XwThread(void *context);

struct usbd_api usbd;
static const u8 *(*pUsbdGetEndpointDescriptor)(void *node, u32 index, u32 type, u32 dir);
static void (*pResetAllUnrecognized)(void *context);
static void (*pUsbdQueueIsochTransfer)(void *node, struct usbd_isoch_transfer *t,
				       const u16 *frame_lengths);
static int g_audio_kernel_ok; /* the isochronous code is the 17559 code read in usbd.h */
static HANDLE *pExecutableModule;
static u8 g_rescan_ctx[RESCAN_CTX_SIZE] __attribute__((aligned(16)));

#define HO_PROBE_DELAY_MS 1500 /* adopted pad ready -> ask for its headset */

/* ---- per-controller USB state ------------------------------------------------------------- */

/* DMA buffers: their own cache lines, inside one page each */
static u8 in_buf[PORTS][IN_COUNT][128] __attribute__((aligned(0x1000)));

struct xw_port g_ports[PORTS];

/* the port a transfer block lives in: blocks are only ever inside g_ports[] */
static struct xw_port *port_of(const void *block)
{
	const u8 *b = block;

	for (int i = 0; i < PORTS; i++)
		if (b >= (const u8 *)&g_ports[i] && b < (const u8 *)&g_ports[i + 1])
			return &g_ports[i];
	return NULL;
}
/* ---- headset audio state (one headset at a time) ------------------------------------------ */

/* per-frame statuses (17559's table at 0x800447F8, usbd.h) */
#define ST_UNDERRUN 0xC0050003     /* short IN packet: data valid */
#define ST_NOT_ACCESSED 0xC005100E /* the frame was missed: length 0 */
#define ST_NO_RESPONSE 0xC0051005 /* OHCI "device not responding": no packet that frame */
#define ST_GONE 0xC0000184         /* ours: the device went away before the call ran */
#define ST_BUSY 0xC00000AE         /* ours: the default endpoint was in use */

struct xw_audio g_audio;
#define DUMP_EAR_MS 1500
#define DUMP_EAR_BYTES (48 * 2 * 2 * DUMP_EAR_MS) /* 1.5 s at 48 kHz stereo, at most */
static u8 *g_dump_ear; /* xw.py dump: the final earpiece PCM (allocated at the first dump) */
static u32 g_alt_gen; /* the call generation of the audio SET_INTERFACE in flight */
static u8 aud_in_buf[AUD_BLOCKS][AUD_BUF] __attribute__((aligned(0x1000)));
static u8 aud_out_buf[AUD_OUT_BLOCKS][AUD_BUF] __attribute__((aligned(0x1000)));

static u16 g_offered_vid, g_offered_pid; /* Match -> AddDevice, both on Usbd's DPC */
static volatile u32 g_notify_full;       /* Match declined one: the thread notifies */
struct usbd_class_driver g_driver;
static struct kdpc360 g_link_dpc;

/* hot reload: what this build tells xw.py and leaves for the next build (handover.h) */
__attribute__((used)) struct xw_handover g_ho = {
	.magic = XW_HO_MAGIC,
	.version = XW_HO_VERSION,
	.size = sizeof(struct xw_handover),
	.state = HO_STARTING,
	.slot = XW_SLOT,
	.cmd_gate = 99, /* no gate holds unless xw.py reload --step lowers it */
};
/* Trace points at the reload steps (docs/PLAN.md WP3.1): the last step reached is in
 * g_ho.trace and the last 64 in g_trace with their time, so a freeze names the step that was
 * running (xw.py tee polls both every 50 ms). Safe on Usbd's DPC: a reserved slot (one
 * atomic add) and three stores. Codes: TR_* | argument << 16; xw.py TRACE_NAMES mirrors
 * this list (append only). */
__attribute__((used)) struct xw_trace g_trace = {
	.magic = XW_TRACE_MAGIC,
	.version = 1,
	.entries = XW_TRACE_RING,
};

#ifdef XW_STAGED
#define XW_GO_DEFAULT 0
#else
#define XW_GO_DEFAULT 99
#endif

__attribute__((used)) struct xw_status g_xw = {
	.magic = 0x58573336 /* XW36 */,
	.version = 3,
	.go = XW_GO_DEFAULT,
	.win_boost = 1, /* measured in a Halo 3 match with a 5-band preset: 0 passes stopped */
	.audio = {
		.magic = 0x58574155 /* XWAU */,
		.stage = 4, /* headset audio on: verified on 045e:0b12 + SteelSeries (xw.py audio 0 = off) */
		.tone_hz = 1000,
		.port = 0xFFFFFFFF,
		.fallback_rx = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF },
		.version = AUD_STATUS_VERSION,
#ifndef XW_DEV
		.release = 1,
#endif
	},
};

enum {
	STAGE_DLLMAIN = 1, /* resolved and checked, thread created */
	STAGE_THREAD,      /* thread running, waiting for gate 1 */
	STAGE_HOOKS,       /* gate 1: XInput hooks installed */
	STAGE_LINKED,      /* gate 2: class driver linked into Usbd's list */
	STAGE_GRABBED,     /* gate 3: controllers present at load re-offered */
	STAGE_RUNNING,     /* main loop; gate 4 starts IN transfers, gate 5 lets OUT through */
};

/* ---- class driver (Usbd DPC) ---------------------------------------------------------------- */

static int ports_free(void)
{
	int n = 0;

	for (int i = 0; i < PORTS; i++)
		n += !g_ports[i].claimed && !g_ports[i].removing;
	return n;
}

BODY u32 XwMatchC(const u8 *ifdesc, const u8 *devdesc, struct usbd_class_driver *self)
{
	(void)self;
	g_xw.match_calls++;
	/* interface descriptor: length, type 4, number, alternate, endpoints, class... An
	 * interface-level driver is offered each interface; only the data interface (0) is ours.
	 * GIP pads (the SCUF Instinct Pro at least) also carry FF/47/D0 as the device class: if
	 * Usbd ever offers the device without an interface descriptor, take that instead. */
	if (ifdesc && ifdesc[1] == 4) {
		if (ifdesc[2] != 0 || ifdesc[5] != GIP_CLASS || ifdesc[6] != GIP_SUBCLASS ||
		    ifdesc[7] != GIP_PROTOCOL)
			return 0;
	} else if (!devdesc || devdesc[4] != GIP_CLASS || devdesc[5] != GIP_SUBCLASS ||
		   devdesc[6] != GIP_PROTOCOL) {
		return 0;
	}
	g_offered_vid = (u16)(devdesc[8] | devdesc[9] << 8);
	g_offered_pid = (u16)(devdesc[10] | devdesc[11] << 8);
	xa_log("usb: GIP controller %04x:%04x offered\n", g_offered_vid, g_offered_pid);
	if (!ports_free()) {
		g_xw.declined++;
		g_notify_full = 1;
		xa_log("usb: all %d ports in use, declined\n", PORTS);
		return 0;
	}
	return 1;
}

/* the interrupt endpoint of one direction: address, max packet, interval; its descriptor
 * (inside Usbd's copy of the configuration descriptor), NULL if none */
static const u8 *find_endpoint(void *node, u32 dir, u8 *addr, u16 *maxp, u8 *interval)
{
	const u8 *ed = pUsbdGetEndpointDescriptor(node, 0, USBD_TYPE_INTERRUPT, dir);

	if (!ed || ed[1] != 5)
		return NULL;
	*addr = ed[2];
	*maxp = (u16)((ed[4] | ed[5] << 8) & 0x7FF);
	*interval = ed[6];
	if (*maxp > PKT_MAX)
		*maxp = PKT_MAX;
	return ed;
}

/*
 * The headset's isochronous endpoints: interface 1, alternate setting 1.
 *
 * UsbdGetEndpointDescriptor (0x800D83E0) can't return them: it scans from the node's
 * interface descriptor and stops at the next interface descriptor of any kind, so from
 * interface 0 it never reaches interface 1, and interface 1's alternate 0 has no endpoints.
 * It is still called with type 1 and its answer logged. The configuration descriptor it
 * scans is Usbd's per-controller enumeration copy: table 0x801A8220 indexed by
 * (node[11] >> 1) & 0xC, descriptor at +96 (its wTotalLength at +98, little-endian). The
 * same buffer is walked here, but only if the interrupt endpoint descriptor that
 * UsbdGetEndpointDescriptor just returned lies inside it (proof the table is read right).
 * Read-only, at AddDevice (afterwards the copy is gone). Anything not found keeps the
 * known defaults 0x83/0x03 (and the guessed packet sizes); desc_source says which.
 */
#define K_USBD_CONFIG_TABLE 0x801A8220

static void find_audio_endpoints(struct xw_port *p, void *node, const u8 *known_ed)
{
	const u8 *n = node, *d, *end;
	u32 total, found = 0;
	int in_alt = 0;

	p->aud_ep_in = AUD_DEF_IN;
	p->aud_ep_out = AUD_DEF_OUT;
	p->aud_max_in = AUD_DEF_MAX_IN;
	p->aud_max_out = AUD_DEF_MAX_OUT;
	p->aud_desc = 2;
	p->gep_in = (u32)pUsbdGetEndpointDescriptor(node, 0, USBD_TYPE_ISOCH, EP_DIR_IN);
	p->gep_out = (u32)pUsbdGetEndpointDescriptor(node, 0, USBD_TYPE_ISOCH, EP_DIR_OUT);
	if (!g_audio_kernel_ok || !known_ed)
		goto done;
	d = *(const u8 *const *)(K_USBD_CONFIG_TABLE + ((n[11] >> 1) & 0xC));
	if ((u32)d < 0x80000000u)
		goto done;
	d += 96;
	total = (u32)(d[2] | d[3] << 8);
	if (d[0] != 9 || d[1] != 2 || total < 9 || total > 1024 || known_ed < d ||
	    known_ed >= d + total)
		goto done;
	for (end = d + total; d + 2 <= end && d[0] >= 2 && d + d[0] <= end; d += d[0]) {
		if (d[1] == 4 && d[0] >= 9) {
			in_alt = d[2] == 1 && d[3] == 1;
		} else if (d[1] == 5 && d[0] >= 7 && in_alt && (d[3] & 3) == USBD_TYPE_ISOCH) {
			u16 maxp = (u16)((d[4] | d[5] << 8) & 0x7FF);

			if (d[2] & 0x80) {
				p->aud_ep_in = d[2];
				p->aud_max_in = maxp;
				found |= 1;
			} else {
				p->aud_ep_out = d[2];
				p->aud_max_out = maxp;
				found |= 2;
			}
		}
	}
	if (found == 3)
		p->aud_desc = 1;
done:
	xa_log("usb: port %d: audio (interface 1/1): IN %02x %u bytes, OUT %02x %u bytes, %s; "
	       "UsbdGetEndpointDescriptor(type 1) gave %08x/%08x\n", (int)(p - g_ports),
	       p->aud_ep_in, p->aud_max_in, p->aud_ep_out, p->aud_max_out,
	       p->aud_desc == 1 ? "from the configuration descriptor"
				: found ? "partly defaults" : "defaults (descriptor not read)",
	       p->gep_in, p->gep_out);
}

static u16 swap16(u16 v)
{
	return (u16)(v << 8 | v >> 8);
}

/* a request without data on the default endpoint; completes through XwCtrlDone. Usbd adds a
 * data stage whenever the buffer is set, so it stays NULL (docs/INTERNALS.md) */
static void queue_ctrl(struct xw_port *p, u8 type, u8 request, u16 value, u16 index)
{
	struct usbd_control_transfer *c = &p->ctrl.c;

	c->request_type = type;
	c->request = request;
	c->value = swap16(value);
	c->index = swap16(index);
	c->length = 0;
	c->t.buffer = NULL;
	c->t.length = 0;
	c->t.flags = 1;
	c->t.callback = (void *)XwCtrlDone;
	c->t.saved_endpoint = c->t.endpoint;
	usbd.queue_async_transfer(p->claimed, c);
}

/* the device is configured: open the interrupt pair and hand the port to the thread */
static void open_data_endpoints(struct xw_port *p)
{
	void *node = p->claimed;
	int idx = (int)(p - g_ports);

	/* found in AddDevice: once configured, UsbdGetEndpointDescriptor returns nothing */
	p->open_status[0] = usbd.open_endpoint(node, USBD_TYPE_INTERRUPT, p->ep_in, p->max_in,
					       p->interval_in, &p->in[0].c.t);
	for (int i = 1; p->open_status[0] >= 0 && i < IN_COUNT; i++)
		p->in[i].c.t = p->in[0].c.t;
	p->open_status[1] = usbd.open_endpoint(node, USBD_TYPE_INTERRUPT, p->ep_out, p->max_out,
					       p->interval_out, &p->out.c.t);
	xa_log("usb: port %d: IN %02x (%u bytes, %u) %08x, OUT %02x (%u bytes, %u) %08x\n", idx,
	       p->ep_in, p->max_in, p->interval_in, (u32)p->open_status[0], p->ep_out, p->max_out,
	       p->interval_out, (u32)p->open_status[1]);
	if (p->open_status[0] < 0 || p->open_status[1] < 0)
		return; /* unusable until re-plugged; RemoveDevice closes what did open */

	p->rx_write = p->rx_read = 0;
	p->tx_head = p->tx_tail = 0;
	p->tx_inflight = 0;
	p->gone = 0;
	BARRIER();
	p->node = node;
	p->session++;
}

enum { SETUP_CONFIG = 1, SETUP_INTERFACE, SETUP_AUDIO_ALT };

/* The 360's Usbd leaves a device unconfigured for its class driver (hiddriver360 sends
 * SET_CONFIGURATION itself): until then its interrupt endpoints never deliver anything. So:
 * SET_CONFIGURATION 1, then the audio interface to alternate 0 (xone: "mandatory for
 * certain third party devices"; a pad without one stalls it, which is fine), then open. */
BODY long XwCtrlDoneC(struct usbd_transfer *t, long status)
{
	struct xw_port *p = port_of(t);
	int idx;

	if (!p) {
		g_xw.stray++;
		return 0;
	}
	idx = (int)(p - g_ports);
	if (p->removing || !p->claimed) /* unplugged during set-up: RemoveDevice closes */
		return 0;
	switch (p->setup_step) {
	case SETUP_CONFIG:
		xa_log("usb: port %d: SET_CONFIGURATION 1: %08x\n", idx, (u32)status);
		if (status < 0) {
			/* claimed, never usable: no control transfer is in flight any more (a
			 * hot reload leaves such a port with this build, commit_check) */
			p->setup_step = 0;
			return 0;
		}
		p->setup_step = SETUP_INTERFACE;
		queue_ctrl(p, 0x01 /* host to device, interface */, 0x0B /* SET_INTERFACE */, 0, 1);
		break;
	case SETUP_INTERFACE:
		xa_log("usb: port %d: SET_INTERFACE 1/0: %08x\n", idx, (u32)status);
		p->setup_step = 0;
		open_data_endpoints(p);
		break;
	case SETUP_AUDIO_ALT: /* the thread waits for it (aud_set_alt), unless it gave up */
		p->setup_step = 0;
		if (g_alt_gen == g_audio.call_gen) {
			g_audio.call_status = status;
			BARRIER();
			g_audio.call_done = 1;
		}
		break;
	}
	xa_usbd_drain();
	return 0;
}

BODY void XwAddDeviceC(void *node)
{
	struct xw_port *p = NULL;
	const u8 *ed_in;
	int idx;

	g_xw.adds++;
	usbd.add_device_complete(node, 0);
	for (idx = 0; idx < PORTS; idx++) {
		if (!g_ports[idx].claimed && !g_ports[idx].removing) {
			p = &g_ports[idx];
			break;
		}
	}
	if (!p) {
		/* Match said yes, but a port filled since: nothing opened, so nothing to close */
		xa_log("usb: device %08x added with no port free\n", (u32)node);
		return;
	}
	p->claimed = node;
	p->vid = g_offered_vid;
	p->pid = g_offered_pid;
	p->in_wait = 0; /* a new device: nothing of an adoption applies to it */
	p->in_src = NULL;

	memset(p->in, 0, sizeof(p->in));
	memset(&p->out, 0, sizeof(p->out));
	memset(&p->ctrl, 0, sizeof(p->ctrl));
	p->open_status[0] = p->open_status[1] = p->open_status[2] = -1;
	ed_in = find_endpoint(node, EP_DIR_IN, &p->ep_in, &p->max_in, &p->interval_in);
	if (!ed_in || !find_endpoint(node, EP_DIR_OUT, &p->ep_out, &p->max_out, &p->interval_out)) {
		/* nothing opened: free the port again */
		xa_log("usb: port %d: device %08x has no interrupt IN/OUT pair\n", idx, (u32)node);
		p->claimed = NULL;
		return;
	}
	find_audio_endpoints(p, node, ed_in);
	p->open_status[2] = usbd.open_default_endpoint(node, &p->ctrl.c);
	xa_log("usb: port %d: device %08x, default endpoint %08x\n", idx, (u32)node,
	       (u32)p->open_status[2]);
	if (p->open_status[2] < 0)
		return;
	p->setup_step = SETUP_CONFIG;
	queue_ctrl(p, 0x00 /* host to device, device */, 0x09 /* SET_CONFIGURATION */, 1, 0);
}

static void aud_close_next(void);

/* closes the IN, then the OUT endpoint, then reports the removal done (as xa_plugin.c).
 * Each close completes through XwCloseDone, sometimes from inside the close call. */
static void close_next(struct xw_port *p)
{
	void *node = p->removing;

	for (;;) {
		u32 step = p->close_step++;
		struct xw_xfer *c;

		if (step < 3) {
			if (p->open_status[step] < 0)
				continue;
			c = &p->close[step];
			memset(c, 0, sizeof(*c));
			c->c.t.saved_endpoint = step == 0 ? p->in[0].c.t.endpoint
						: step == 1 ? p->out.c.t.endpoint : p->ctrl.c.t.endpoint;
			c->c.t.callback = (void *)XwCloseDone;
			if (step < 2)
				usbd.queue_close_endpoint(node, c);
			else
				usbd.queue_close_default_endpoint(node, c);
			return;
		}
		g_xw.removes++;
		xa_log("usb: port %d: device %08x removed\n", (int)(p - g_ports), (u32)node);
		p->open_status[0] = p->open_status[1] = p->open_status[2] = -1;
		p->setup_step = 0;
		p->claimed = NULL;
		BARRIER();
		p->removing = NULL;
		usbd.remove_device_complete(node);
		return;
	}
}

BODY long XwCloseDoneC(struct usbd_transfer *t, long status)
{
	(void)status;
	struct xw_port *p = port_of(t);

	if (p)
		close_next(p);
	else
		g_xw.stray++;
	return 0;
}

BODY void XwRemoveDeviceC(void *node)
{
	struct xw_port *p = NULL;

	for (int i = 0; i < PORTS; i++) {
		if (g_ports[i].claimed == node && !g_ports[i].removing) {
			p = &g_ports[i];
			break;
		}
	}
	if (!p) {
		trace(TR_REMOVE_OTHER);
		usbd.remove_device_complete(node);
		return;
	}
	trace(TR_PORT(TR_REMOVE, p));
	xa_log("usb: port %d: device %08x is going away\n", (int)(p - g_ports), (u32)node);
	if (p->handed) /* released and nobody adopted it: no longer on offer to the next build */
		g_ho.ports[p - g_ports].node = 0;
	p->in_wait = 0;
	p->gone = 1;
	BARRIER();
	p->node = 0;
	p->removing = node;
	p->close_step = 0;
	if (g_audio.open_port == p && (g_audio.opened || g_audio.closing)) {
		/* the headset's endpoints first: once their queued blocks have completed (or the
		 * thread's watchdog forces it), aud_close_next carries on with close_next */
		xa_log("usb: port %d: closing the audio endpoints first\n", (int)(p - g_ports));
		g_audio.streaming = 0;
		g_audio.close_then_remove = p;
		if (!g_audio.closing) {
			g_audio.closing = 1;
			aud_close_next();
		}
		return;
	}
	close_next(p);
}

BODY void XwLinkDpcC(struct kdpc360 *dpc, void *context, void *arg1, void *arg2)
{
	struct usbd_class_driver *head = (struct usbd_class_driver *)K_DRIVER_LIST;

	(void)dpc, (void)context, (void)arg1, (void)arg2;
	g_driver.flink = head->flink;
	g_driver.blink = head;
	BARRIER();
	head->flink->blink = &g_driver;
	head->flink = &g_driver;
	trace(TR_LINK_DPC);
	g_xw.state = XW_WAITING;
}

/* ---- transfers (Usbd DPC) ----------------------------------------------------------------- */

#define STATUS_CANCELLED 0xC0000120

void queue_in(struct xw_port *p, int i)
{
	struct xw_xfer *x = &p->in[i];
	u8 *buf = in_buf[p - g_ports][i];

	/* guard (WP3.1): never a second interrupt IN transfer on the endpoint (two froze the
	 * console): ours still queued, or an earlier build's not completed yet */
	if (x->busy || p->in_wait) {
		g_ho.in_double++;
		trace(TR_PORT(TR_IN_DOUBLE, p));
		xa_log("usb: port %d: IN NOT queued: one is queued already (%s); in_double %u\n",
		       (int)(p - g_ports), x->busy ? "ours" : "the previous build's",
		       g_ho.in_double);
		return;
	}
	if (p->trace_first & 1) {
		p->trace_first &= ~1u;
		trace(TR_PORT(TR_FIRST_IN, p));
	}
	/* a short packet leaves the rest of the buffer as it was: zeroed, GIP reads that as
	 * empty padding, never as the tail of an older packet */
	memset(buf, 0, PKT_MAX);
	/* everything past the declared fields as xa_xfer leaves it: zero */
	memset(x->c.reserved, 0, sizeof(x->c) - sizeof(x->c.t));
	x->c.t.buffer = buf;
	x->c.t.length = p->max_in;
	x->c.t.flags = 1;
	x->c.t.callback = (void *)XwInDone;
	x->c.t.saved_endpoint = x->c.t.endpoint;
	x->busy = 1;
	BARRIER();
	usbd.queue_async_transfer(p->node, x);
}

BODY long XwInDoneC(struct usbd_transfer *t, long status)
{
	struct xw_xfer *x = (struct xw_xfer *)t;
	struct xw_port *p = port_of(t);
	int i;

	/* what Usbd left in the block, before anything else (diagnostic) */
	if (!g_xw.in_done)
		memcpy((void *)g_xw.in_block_after, t, sizeof(g_xw.in_block_after));
	if (!p) {
		g_xw.stray++;
		return 0;
	}
	i = (int)(x - p->in);
	if (i < 0 || i >= IN_COUNT) {
		g_xw.stray++;
		return 0;
	}
	g_xw.in_done++;
	g_xw.in_last_status = (u32)status;
	if (p->handed) {
		/* released (hot reload): this was our last IN transfer, queued before the
		 * release and never cancelled. It is not re-queued; the build that adopted the
		 * controller, if any, gets the packet and queues its own (handover.h) */
		struct xw_ho_port *e = &g_ho.ports[p - g_ports];
		void (*fwd)(void *, const u8 *, u32, long);

		trace(TR_PORT(TR_IN_HANDED, p));
		x->busy = 0;
		BARRIER();
		e->in_busy = 0;
		BARRIER();
		fwd = (void (*)(void *, const u8 *, u32, long))e->in_fwd;
		if (fwd)
			fwd(p->claimed, in_buf[p - g_ports][i], p->max_in, status);
		xa_usbd_drain();
		return 0;
	}
	if (status >= 0) {
		struct xw_pad_status *ps = &g_xw.pads[p - g_ports];

		if (!p->rx_count) {
			ps->first_length_after = t->length;
			memcpy((void *)g_xw.in_first, in_buf[p - g_ports][i], sizeof(g_xw.in_first));
		}
		p->rx_count++;
		if (p->rx_write - p->rx_read >= RX_SLOTS) {
			p->rx_dropped++;
		} else {
			u32 s = p->rx_write % RX_SLOTS;

			memcpy(p->rx[s], in_buf[p - g_ports][i], p->max_in);
			p->rx_len[s] = (u8)p->max_in;
			BARRIER();
			p->rx_write++;
		}
	} else if ((u32)status != STATUS_CANCELLED) {
		p->rx_errors++;
	}

	/* XW_STAGED: gate 4 receives one packet only; from gate 5 the transfer is re-queued.
	 * Not busy any more first: queue_in refuses a block that is (in_double) */
	x->busy = 0;
	BARRIER();
	if ((u32)status != STATUS_CANCELLED && p->node && !p->gone && g_xw.go >= 5)
		queue_in(p, i);
	g_xw.in_returning++;
	xa_usbd_drain();
	g_xw.in_returned++;
	return 0;
}

/* the oldest waiting packet, if none is in flight */
static void tx_submit(struct xw_port *p)
{
	struct xw_xfer *x = &p->out;
	u32 s;

	if (p->tx_inflight || p->tx_head == p->tx_tail || !p->node || p->gone || p->handed)
		return;
	if (p->trace_first & 2) {
		p->trace_first &= ~2u;
		trace(TR_PORT(TR_FIRST_OUT, p));
	}
	s = p->tx_tail % OUT_SLOTS;
	memset(x->c.reserved, 0, sizeof(x->c) - sizeof(x->c.t));
	x->c.t.buffer = p->tx[s];
	x->c.t.length = p->tx_len[s];
	x->c.t.flags = 1;
	x->c.t.callback = (void *)XwOutDone;
	x->c.t.saved_endpoint = x->c.t.endpoint;
	x->busy = 1;
	p->tx_inflight = 1;
	BARRIER();
	usbd.queue_async_transfer(p->node, x);
}

BODY long XwOutDoneC(struct usbd_transfer *t, long status)
{
	struct xw_xfer *x = (struct xw_xfer *)t;
	struct xw_port *p = port_of(t);

	if (!p || x != &p->out) {
		g_xw.stray++;
		return 0;
	}
	if (status < 0)
		p->tx_errors++;
	else
		p->tx_count++;
	x->busy = 0;
	p->tx_tail++;
	p->tx_inflight = 0;
	BARRIER();
	tx_submit(p);
	xa_usbd_drain();
	return 0;
}

static void start_port(void *arg) /* xa_usbd_run */
{
	struct xw_port *p = arg;

	if (!p->node || p->gone || p->handed)
		return;
	if (p->in_wait) /* an earlier build's transfer is still queued: XwInForwardC queues ours */
		return;
	for (int i = 0; i < IN_COUNT; i++)
		if (!p->in[i].busy)
			queue_in(p, i);
}

static void kick_port(void *arg) /* xa_usbd_run */
{
	tx_submit(arg);
}

/* ---- headset audio: isochronous transfers (Usbd DPC) ---------------------------------------
 *
 * One headset at a time (g_audio.owner). IN: three 8-frame blocks rotate, each completion
 * copies its frames into g_audio.rx and re-queues itself (stage >= 3; stage 2 queues one
 * block once and never re-queues). OUT: the thread fills a free block with what
 * gip_headset_send produced and posts it; at most AUD_OUT_BLOCKS are on Usbd. Closing: no new
 * queueing (streaming = 0, closing = 1), wait until every queued block has completed (they
 * always do, an isochronous TD retires at its frames whatever the device does), then close
 * IN, then OUT. Never UsbdCancelAsyncTransfer on these blocks (usbd.h).
 */

/* the live stage can only lower the latched one (xw.py audio 0 stops streaming at once) */
u32 aud_live_stage(void)
{
	u32 s = g_xw.audio.stage;

	return s < g_audio.stage ? s : g_audio.stage;
}

static int aud_can_queue(void)
{
	struct xw_port *p = g_audio.owner;

	return p && p == g_audio.open_port && p->node && !p->gone && !p->removing &&
	       g_audio.streaming && !g_audio.closing;
}

int aud_queued(void)
{
	for (int i = 0; i < AUD_BLOCKS; i++)
		if (g_audio.in[i].queued)
			return 1;
	for (int i = 0; i < AUD_OUT_BLOCKS; i++)
		if (g_audio.out[i].queued)
			return 1;
	return 0;
}

static void iso_submit(struct xw_iso *b, void *callback)
{
	b->t.callback = callback;
	b->t.saved_endpoint = b->t.endpoint;
	b->t.cancel_link = 0;
	b->gen = g_audio.open_gen;
	b->queued = 1;
	BARRIER();
	pUsbdQueueIsochTransfer(g_audio.open_node, &b->t, b->t.frame_length);
}

static void iso_in_queue(int i)
{
	struct xw_iso *b = &g_audio.in[i];

	b->t.buffer = aud_in_buf[i];
	b->t.length = (u32)g_audio.max_in * AUD_FRAMES;
	b->t.frames = AUD_FRAMES;
	for (int k = 0; k < USBD_ISOCH_FRAMES_MAX; k++)
		b->t.frame_length[k] = g_audio.max_in;
	g_xw.audio.in_queued++;
	iso_submit(b, (void *)XwIsoInDone);
}

static void aud_close_if_idle(void)
{
	if (g_audio.close_waiting && !aud_queued())
		aud_close_next();
}

BODY long XwIsoInDoneC(struct usbd_isoch_transfer *t, const long *status, const u16 *length)
{
	struct xw_audio_status *as = &g_xw.audio;
	struct xw_iso *b = (struct xw_iso *)t;
	int i = (b >= &g_audio.in[0] && b < &g_audio.in[AUD_BLOCKS]) ? (int)(b - g_audio.in) : -1;
	u32 frames, max = g_audio.max_in;
	int cancelled = 0;

	if (i < 0) {
		g_xw.stray++;
		trace(TR_ISO_STRAY | 1 << 16);
		return 0;
	}
	if (b->gen != g_audio.open_gen) {
		/* from an earlier open (can't happen while aud_open_dpc waits for every queued
		 * block): its queued flag isn't ours to clear any more, and it's never re-queued */
		g_xw.stray++;
		trace(TR_ISO_STRAY | 2 << 16);
		xa_usbd_drain();
		return 0;
	}
	frames = t->frames > AUD_FRAMES ? AUD_FRAMES : t->frames;
	if (g_audio.trace_first & 1) {
		g_audio.trace_first &= ~1u;
		trace(TR_ISO_IN_FIRST | (frames ? (u32)status[0] & 0xFFFF : 0xFFFF) << 16);
	}
	as->in_done++;
	if (!g_audio.recorded) {
		for (u32 k = 0; k < AUD_FRAMES; k++) {
			as->first_status[k] = k < frames ? (u32)status[k] : 0;
			as->first_len[k] = k < frames ? length[k] : 0;
		}
		g_audio.recorded = 1;
	}
	{
		u32 silent = 0;

		for (u32 k = 0; k < frames; k++)
			silent += (u32)status[k] == ST_NO_RESPONSE;
		if (silent && silent <= 2) /* a streaming mic's skip, not a mic that stopped */
			as->mic_skips += silent;
	}
	for (u32 k = 0; k < frames; k++) {
		u32 st = (u32)status[k], n = length[k];
		const u8 *src = (const u8 *)t->buffer + k * max;

		if (st == 0 || st == ST_UNDERRUN) {
			if (!n) {
				as->in_frames_empty++;
				continue;
			}
			as->in_frames_ok++;
			as->in_bytes += n;
			if (n > max || n > AUD_FRAME_MAX ||
			    g_audio.rx_write - g_audio.rx_read >= AUD_RX_SLOTS) {
				as->in_dropped++;
				continue;
			}
			if (!g_audio.recorded_bytes) {
				memcpy(as->first_bytes, src, n < 16 ? n : 16);
				g_audio.recorded_bytes = 1;
			}
			memcpy(g_audio.rx[g_audio.rx_write % AUD_RX_SLOTS], src, n);
			g_audio.rx_len[g_audio.rx_write % AUD_RX_SLOTS] = (u16)n;
			BARRIER();
			g_audio.rx_write++;
		} else if (st == STATUS_CANCELLED) {
			cancelled = 1;
		} else if (st == ST_NOT_ACCESSED) {
			as->in_frames_missed++;
		} else {
			as->in_frames_err++;
			as->in_last_err = st;
		}
	}
	if (cancelled)
		as->in_cancelled++;
	if (!cancelled && aud_can_queue() && (g_audio.opened & 1) && aud_live_stage() >= 3)
		iso_in_queue(i);
	else
		b->queued = 0;
	aud_close_if_idle();
	xa_usbd_drain();
	return 0;
}

BODY long XwIsoOutDoneC(struct usbd_isoch_transfer *t, const long *status, const u16 *length)
{
	struct xw_audio_status *as = &g_xw.audio;
	struct xw_iso *b = (struct xw_iso *)t;
	u32 frames;

	(void)length;
	if (b < &g_audio.out[0] || b >= &g_audio.out[AUD_OUT_BLOCKS]) {
		g_xw.stray++;
		trace(TR_ISO_STRAY | 3 << 16);
		return 0;
	}
	if (b->gen != g_audio.open_gen) { /* from an earlier open: see XwIsoInDoneC */
		g_xw.stray++;
		trace(TR_ISO_STRAY | 4 << 16);
		xa_usbd_drain();
		return 0;
	}
	frames = t->frames > AUD_FRAMES ? AUD_FRAMES : t->frames;
	if (g_audio.trace_first & 2) {
		g_audio.trace_first &= ~2u;
		trace(TR_ISO_OUT_FIRST | (frames ? (u32)status[0] & 0xFFFF : 0xFFFF) << 16);
	}
	as->out_done++;
	for (u32 k = 0; k < frames; k++) {
		u32 st = (u32)status[k];

		if (st && st != ST_NOT_ACCESSED && st != STATUS_CANCELLED) {
			as->out_frames_err++;
			as->out_last_err = st;
		}
		if (st == ST_NOT_ACCESSED)
			g_xd.out_frames_missed++;
		if (st)
			g_xd.out_last_status = st;
	}
	b->queued = 0;
	BARRIER();
	b->busy = 0;
	{
		/* the stream after this block: starved if nothing else is queued (the next
		 * block starts late) */
		int others = 0, live = aud_can_queue() && (g_audio.opened & 2) && aud_live_stage() >= 4;

		for (int i = 0; i < AUD_OUT_BLOCKS; i++)
			others += g_audio.out[i].queued;
		if (!others && live)
			g_xd.out_starved++;
	}
	aud_close_if_idle();
	xa_usbd_drain();
	return 0;
}

/* closes IN, then OUT, each through XwIsoCloseDone (sometimes from inside the call); then,
 * if RemoveDevice is waiting, the rest of the device */
static void aud_close_next(void)
{
	void *node = g_audio.open_node; /* as opened: valid until RemoveDevice completes */

	if (!g_audio.force && aud_queued()) {
		g_audio.close_waiting = 1;
		return;
	}
	g_audio.close_waiting = 0;
	if (node && (g_audio.opened & 3)) {
		int out = !(g_audio.opened & 1);
		struct xw_xfer *c = &g_audio.close[out];

		g_audio.opened &= out ? ~2u : ~1u;
		memset(c, 0, sizeof(*c));
		c->c.t.saved_endpoint = out ? g_audio.out[0].t.endpoint : g_audio.in[0].t.endpoint;
		c->c.t.callback = (void *)XwIsoCloseDone;
		usbd.queue_close_endpoint(node, c);
		return;
	}
	g_audio.opened = 0;
	g_audio.open_port = NULL;
	g_audio.open_node = NULL;
	g_audio.force = 0;
	BARRIER();
	g_audio.closing = 0;
	g_xw.audio.closes++;
	if (g_audio.close_then_remove) {
		struct xw_port *r = g_audio.close_then_remove;

		g_audio.close_then_remove = NULL;
		close_next(r);
	}
}

BODY long XwIsoCloseDoneC(struct usbd_transfer *t, long status)
{
	if (t != &g_audio.close[0].c.t && t != &g_audio.close[1].c.t) {
		g_xw.stray++;
		return 0;
	}
	g_xw.audio.close_status = (u32)status;
	trace(TR_AUD_CLOSED | ((u32)status & 0xFFFF) << 16);
	aud_close_next();
	xa_usbd_drain();
	return 0;
}

/* xa_usbd_run bodies for the thread; each reports through g_audio.call_done if it is waited
 * for */
static void aud_call_finish(long status)
{
	g_audio.call_status = status;
	BARRIER();
	g_audio.call_done = 1;
}

/* the port of a waited-for call posted with generation `arg`; NULL = the thread gave up on
 * it (or audio moved on): don't run it */
static struct xw_port *aud_call_port(void *arg)
{
	struct xw_port *p = g_audio.call_port;

	if ((u32)arg != g_audio.call_gen || !p || p != g_audio.owner)
		return NULL;
	return p;
}

/* g_alt_gen: the call generation of the SET_INTERFACE on the default endpoint */

static void aud_alt_dpc(void *arg)
{
	struct xw_port *p = aud_call_port(arg);

	if (!p)
		return;
	if (!p->claimed || p->gone || p->removing) {
		aud_call_finish((long)ST_GONE);
		return;
	}
	if (p->setup_step) {
		aud_call_finish((long)ST_BUSY);
		return;
	}
	g_alt_gen = (u32)arg;
	trace(TR_PORT(TR_AUD_ALT, p));
	p->setup_step = SETUP_AUDIO_ALT; /* XwCtrlDoneC finishes the call */
	queue_ctrl(p, 0x01 /* host to device, interface */, 0x0B /* SET_INTERFACE */, g_audio.alt,
		   1);
}

static void aud_open_dpc(void *arg)
{
	struct xw_port *p = aud_call_port(arg);
	struct xw_audio_status *as = &g_xw.audio;
	long s0 = (long)ST_GONE, s1 = (long)ST_GONE;

	if (!p)
		return;
	trace(TR_PORT(TR_AUD_OPEN, p));
	/* blocks of an earlier (forced) close may still complete: never reopen under them */
	int out_busy = 0;

	for (int i = 0; i < AUD_OUT_BLOCKS; i++)
		out_busy |= g_audio.out[i].busy;
	if (aud_queued() || out_busy) {
		as->open_in = as->open_out = ST_BUSY;
		aud_call_finish((long)ST_BUSY);
		return;
	}
	if (p->node && !p->gone && !p->removing && !g_audio.opened && !g_audio.closing) {
		/* the blocks' Usbd part only: queued/busy/gen stay as the completions left them */
		for (int i = 0; i < AUD_BLOCKS; i++)
			memset(&g_audio.in[i].t, 0, sizeof(g_audio.in[i].t));
		for (int i = 0; i < AUD_OUT_BLOCKS; i++)
			memset(&g_audio.out[i].t, 0, sizeof(g_audio.out[i].t));
		g_audio.open_gen++;
		g_audio.trace_first = 3;
		g_audio.open_port = p;
		g_audio.open_node = p->node;
		/* interval: OHCI forces 1 ms for isochronous endpoints */
		s0 = usbd.open_endpoint(p->node, USBD_TYPE_ISOCH, g_audio.ep_in, g_audio.max_in, 1,
					(struct usbd_transfer *)&g_audio.in[0].t);
		if (s0 >= 0) {
			for (int i = 1; i < AUD_BLOCKS; i++)
				g_audio.in[i].t.endpoint = g_audio.in[0].t.endpoint;
			g_audio.opened |= 1;
		}
		s1 = usbd.open_endpoint(p->node, USBD_TYPE_ISOCH, g_audio.ep_out, g_audio.max_out,
					1, (struct usbd_transfer *)&g_audio.out[0].t);
		if (s1 >= 0) {
			for (int i = 1; i < AUD_OUT_BLOCKS; i++)
				g_audio.out[i].t.endpoint = g_audio.out[0].t.endpoint;
			g_audio.opened |= 2;
		}
		if (!g_audio.opened) {
			g_audio.open_port = NULL;
			g_audio.open_node = NULL;
		}
		as->opens++;
	}
	as->open_in = (u32)s0;
	as->open_out = (u32)s1;
	trace(TR_PORT(TR_AUD_OPENED, p));
	aud_call_finish(s0 < 0 ? s0 : s1);
}

static void aud_close_dpc(void *arg)
{
	(void)arg;
	g_audio.streaming = 0;
	if (g_audio.closing || !g_audio.opened)
		return;
	g_audio.closing = 1;
	trace(TR_AUD_CLOSE);
	aud_close_next();
}

/* the thread's watchdog: a queued block that never completes would hold the close forever */
static void aud_force_dpc(void *arg)
{
	(void)arg;
	if (!g_audio.close_waiting)
		return;
	g_xw.audio.watchdog++;
	trace(TR_AUD_FORCE);
	g_audio.force = 1;
	aud_close_next();
}

void aud_start_in_dpc(void *arg) /* stage >= 3: both IN blocks */
{
	(void)arg;
	for (int i = 0; i < AUD_BLOCKS; i++)
		if (!g_audio.in[i].queued && aud_can_queue() && (g_audio.opened & 1))
			iso_in_queue(i);
}

static void aud_oneshot_dpc(void *arg) /* stage 2: one IN block, never re-queued */
{
	(void)arg;
	if (!aud_queued() && aud_can_queue() &&
	    (g_audio.opened & 1))
		iso_in_queue(0);
}

static void aud_queue_out_dpc(void *arg)
{
	struct xw_iso *b = arg;

	if (b->queued || !b->busy) /* never: the thread only posts a block it owns */
		return;
	if (!b->t.frames || b->t.frames > AUD_FRAMES || !b->t.length || b->t.length > AUD_BUF) {
		b->busy = 0; /* never queue a malformed block: FC = frames - 1 goes into the iTD */
		return;
	}
	if (!aud_can_queue() || !(g_audio.opened & 2) || aud_live_stage() < 4) {
		g_xw.audio.out_dropped++;
		b->busy = 0;
		aud_close_if_idle();
		return;
	}
	g_xw.audio.out_queued++;
	iso_submit(b, (void *)XwIsoOutDone);
}

/* ---- the GIP transport: one packet per interrupt OUT transfer ------------------------------ */

static int wired_send(void *ctx, const u8 *data, int len)
{
	struct xw_port *p = ctx;
	u32 s;

	if (len > p->max_out)
		return -XA_EINVAL;
	if (!p->node || p->gone)
		return -XA_ENODEV;
	if (g_xw.go < 7) {
		static u32 held;

		if (!held++)
			xa_log("xw: OUT held back until xw.py go 7\n");
		return 0;
	}
	if (p->tx_head - p->tx_tail >= OUT_SLOTS) {
		p->tx_dropped++;
		return 0; /* only under extreme overload; GIP retries what it must */
	}
	s = p->tx_head % OUT_SLOTS;
	memcpy(p->tx[s], data, len);
	p->tx_len[s] = (u8)len;
	BARRIER();
	p->tx_head++;
	xa_usbd_run(kick_port, p);
	return 0;
}

/* ---- headset audio: the transport's enable_audio / send_audio (driver thread) ---------------
 *
 * Stages (g_xw.audio.stage, xw.py audio N; read when gip.c enables audio, i.e. when the
 * headset identifies itself: replug the headset after raising it):
 *   0  the headset is logged, audio refused (gip.c then treats it as no headset)
 *   1  SET_INTERFACE(1,1), open the isochronous endpoints, log; nothing queued
 *   2  + one IN block when the headset is ready, never re-queued (first_* in the status)
 *   3  + continuous IN: mic packets -> gip.c -> voice_360 (xv_mic_samples)
 *   4  + OUT: earpiece blocks from xv_earpiece_fill
 *   5  self-test without voice_360: OUT is a tone (tone_hz), the mic is analysed
 * Lowering the stage at run time stops what the lower stage doesn't do (IN re-queue below
 * 3, OUT below 4) at once; the endpoints stay open until the headset goes.
 */

#define K_USBD_CONTROLLERS 0x801A8230 /* UsbdQueueIsochTransfer's controller table */

#define AUD_WAIT_MS 500
#define AUD_FORCE_MS 100

static int aud_port_idx(const struct xw_port *p)
{
	return (int)(p - g_ports);
}

/* the controller a node is on: OHCI, or EHCI (a full-speed pad behind a high-speed hub,
 * where isochronous transfers would be split transactions: not supported here). The same
 * test as UsbdQueueIsochTransfer's dispatch (0x800D82C0): controller byte 0x96 bit 1 SET
 * branches to the OHCI code (0x800DC9A8), clear to EHCI (0x800DFA48) */
static int aud_on_ehci(void *node)
{
	const u8 *n = node;
	const u8 *ctrl = *(const u8 *const *)(K_USBD_CONTROLLERS + (n[8] & 3) * 4);

	return !ctrl || !(ctrl[150] & 2);
}

/* runs fn on Usbd's DPC and waits for it to call aud_call_finish; 0 = timed out or gone */
static int aud_call(void (*fn)(void *), struct xw_port *p, unsigned int ms)
{
	u64 until = xa_now_ms() + ms;
	u32 gen = g_audio.call_gen + 1;

	g_audio.call_done = 0;
	g_audio.call_port = p;
	g_audio.call_gen = gen;
	BARRIER();
	xa_usbd_run(fn, (void *)gen);
	while (!g_audio.call_done) {
		if (xa_now_ms() > until || p->gone) {
			g_audio.call_gen = gen + 1; /* given up: a late run of fn does nothing */
			BARRIER();
			return 0;
		}
		xa_sleep_ms(1);
	}
	BARRIER();
	return 1;
}

static int aud_set_alt(struct xw_port *p, u16 alt)
{
	struct xw_audio_status *as = &g_xw.audio;
	long st;

	g_audio.alt = alt;
	as->alt_value = alt;
	if (!aud_call(aud_alt_dpc, p, AUD_WAIT_MS)) {
		if (!p->gone) {
			/* the default endpoint's block may still be on Usbd: never reuse it */
			g_audio.broken = 1;
			xa_log("usb: port %d: SET_INTERFACE 1/%u never completed: audio is broken "
			       "until the console is rebooted\n", aud_port_idx(p), alt);
		}
		as->alt_status = ST_GONE;
		return -XA_ETIMEDOUT;
	}
	st = g_audio.call_status;
	as->alt_status = (u32)st;
	xa_log("usb: port %d: SET_INTERFACE 1/%u: %08x\n", aud_port_idx(p), alt, (u32)st);
	return st < 0 ? -XA_EIO : 0;
}

/* closes the isochronous endpoints and waits; 0 when closed */
static int aud_close_wait(struct xw_port *p)
{
	u64 start = xa_now_ms();
	int forced = 0;

	g_audio.streaming = 0;
	BARRIER();
	xa_usbd_run(aud_close_dpc, NULL);
	while (g_audio.opened || g_audio.closing) {
		u64 now = xa_now_ms();

		if (!forced && now > start + AUD_FORCE_MS && g_audio.close_waiting) {
			forced = 1;
			xa_log("usb: port %d: audio blocks still queued after %u ms: closing anyway\n",
			       aud_port_idx(p), AUD_FORCE_MS);
			xa_usbd_run(aud_force_dpc, NULL);
		}
		if (now > start + AUD_WAIT_MS) {
			g_audio.broken = 1;
			xa_log("usb: port %d: audio endpoints didn't close (opened %u, waiting %d): "
			       "audio is broken until the console is rebooted\n", aud_port_idx(p),
			       g_audio.opened, g_audio.close_waiting);
			return -XA_ETIMEDOUT;
		}
		xa_sleep_ms(1);
	}
	return 0;
}

/* voice_360: the headset present (once the pad is bound and gip has the format) or gone */
static void aud_announce(struct xw_port *p, bool on)
{
	if (on) {
		const struct gip_headset *hs;

		if (g_audio.announced || g_audio.owner != p || !g_audio.hs_up || p->pad < 0 ||
		    !p->gc || (g_audio.stage != 3 && g_audio.stage != 4) || aud_live_stage() < 3)
			return;
		hs = &p->gc->hs;
		g_audio.ann_rate_in = hs->rate_in;
		g_audio.ann_ch_in = hs->channels_in;
		g_audio.ann_rate_out = hs->rate_out;
		g_audio.ann_ch_out = hs->channels_out;
		g_audio.announced_pad = p->pad;
		g_audio.announced = 1;
		xa_log("xw: pad %d: headset to the voice side: mic %u Hz x%u, earpiece %u Hz x%u\n",
		       p->pad, hs->rate_in, hs->channels_in, hs->rate_out, hs->channels_out);
		xv_headset_changed(p->pad, true, hs->rate_in, hs->channels_in, hs->rate_out,
				   hs->channels_out);
		return;
	}
	if (!g_audio.announced)
		return;
	g_audio.announced = 0;
	xa_log("xw: pad %d: headset gone (voice side)\n", g_audio.announced_pad);
	xv_headset_changed(g_audio.announced_pad, false, g_audio.ann_rate_in, g_audio.ann_ch_in,
			   g_audio.ann_rate_out, g_audio.ann_ch_out);
}

/* everything off for this port: voice first, then the streams, then alternate 0 */
void aud_disable(struct xw_port *p, const char *why)
{
	if (g_audio.owner != p)
		return;
	aud_announce(p, false);
	g_audio.hs_up = 0;
	xa_log("xw: port %d: audio off (%s)\n", aud_port_idx(p), why);
	g_xw.audio.disables++;
	if (aud_close_wait(p))
		return; /* broken: the owner stays, so nothing streams again */
	if (!p->gone && p->node && !g_audio.broken)
		aud_set_alt(p, 0);
	if (!g_audio.broken)
		g_audio.owner = NULL;
}

static int aud_refuse(struct xw_port *p, const char *why)
{
	g_xw.audio.refusals++;
	xa_log("xw: port %d: headset audio not started: %s\n", aud_port_idx(p), why);
	return -XA_EIO;
}

static int wired_enable_audio(void *ctx, bool on)
{
	struct xw_port *p = ctx;
	struct xw_audio_status *as = &g_xw.audio;
	u32 stage = as->stage;
	int idx = aud_port_idx(p);

	if (!on) {
		aud_disable(p, "headset gone");
		return 0;
	}
	if (!stage || stage > AUD_STAGE_MAX)
		return aud_refuse(p, "audio stage 0 (xw.py audio N, then replug the headset)");
	if (!g_audio_kernel_ok)
		return aud_refuse(p, "this kernel's isochronous code isn't the one checked (17559)");
	if (g_audio.broken)
		return aud_refuse(p, "audio is broken (an earlier USB request never completed) "
				     "until the console is rebooted");
	if (g_audio.owner == p)
		aud_disable(p, "enabled again");
	if (g_audio.broken)
		return aud_refuse(p, "audio is broken until the console is rebooted");
	if (g_audio.owner)
		return aud_refuse(p, "another controller's headset has audio");
	if (!p->node || p->gone)
		return aud_refuse(p, "the controller is going away");
	if (aud_on_ehci(p->node))
		return aud_refuse(p, "the controller is on EHCI (a hub?): plug it into the console");
	if (!p->aud_max_in || p->aud_max_in > AUD_BUF / AUD_FRAMES || !p->aud_max_out ||
	    p->aud_max_out > AUD_BUF / AUD_FRAMES)
		return aud_refuse(p, "unusable isochronous packet sizes");

	g_audio.owner = p;
	g_audio.enable_gen++;
	g_audio.stage = stage;
	g_audio.ep_in = p->aud_ep_in;
	g_audio.ep_out = p->aud_ep_out;
	g_audio.max_in = p->aud_max_in;
	g_audio.max_out = p->aud_max_out;
	g_audio.hs_up = g_audio.announced = 0;
	g_audio.recorded = g_audio.recorded_bytes = 0;
	g_audio.rx_read = g_audio.rx_write = 0;
	g_audio.next_block = 0;
	memset(as->first_status, 0, sizeof(as->first_status));
	memset(as->first_len, 0, sizeof(as->first_len));
	memset(as->first_bytes, 0, sizeof(as->first_bytes));
	as->enables++;
	as->active_stage = stage;
	as->port = (u32)idx;
	as->ep_in = g_audio.ep_in;
	as->ep_out = g_audio.ep_out;
	as->max_in = g_audio.max_in;
	as->max_out = g_audio.max_out;
	as->desc_source = p->aud_desc;
	as->gep_in = p->gep_in;
	as->gep_out = p->gep_out;
	xa_log("xw: port %d: headset audio, stage %u: IN %02x (%u), OUT %02x (%u)\n", idx, stage,
	       g_audio.ep_in, g_audio.max_in, g_audio.ep_out, g_audio.max_out);

	if (aud_set_alt(p, 1)) {
		if (!g_audio.broken)
			g_audio.owner = NULL;
		return -XA_EIO;
	}
	if (!aud_call(aud_open_dpc, p, AUD_WAIT_MS)) {
		/* the open is synchronous on the DPC, and a late run is invalidated; but if the
		 * device is still there, keep the owner (RemoveDevice closes whatever opened) */
		if (p->gone) {
			aud_disable(p, "controller gone while opening");
		} else {
			g_audio.broken = 1;
			g_audio.streaming = 0;
			xa_log("usb: port %d: opening the audio endpoints didn't run: audio is "
			       "broken until the console is rebooted\n", idx);
		}
		return -XA_EIO;
	}
	xa_log("usb: port %d: audio endpoints: IN %02x %08x, OUT %02x %08x\n", idx, g_audio.ep_in,
	       as->open_in, g_audio.ep_out, as->open_out);
	if (g_audio.opened != 3) {
		aud_disable(p, "an endpoint didn't open");
		return -XA_EIO;
	}
	if (stage == 1) {
		xa_log("xw: port %d: audio stage 1: endpoints open, nothing queued\n", idx);
		return 0;
	}
	g_audio.streaming = 1;
	BARRIER();
	if (stage >= 3) {
		xa_log("xw: port %d: audio IN streaming\n", idx);
		xa_usbd_run(aud_start_in_dpc, NULL);
	}
	return 0;
}

/* an OUT block the thread may fill (thread): not all AUD_OUT_BLOCKS are in use */
static int aud_out_free(void)
{
	for (int i = 0; i < AUD_OUT_BLOCKS; i++)
		if (!g_audio.out[i].busy)
			return 1;
	return 0;
}

static int wired_send_audio(void *ctx, const u8 *data, int frame_len, int frames)
{
	struct xw_port *p = ctx;
	struct xw_audio_status *as = &g_xw.audio;
	struct xw_iso *b = NULL;
	int i;

	if (g_audio.owner != p || !g_audio.streaming || aud_live_stage() < 4) {
		as->out_refused++;
		return -XA_ENODEV;
	}
	if (frames < 1 || frames > AUD_FRAMES || frame_len < 1 || frame_len > g_audio.max_out ||
	    frame_len * frames > AUD_BUF) {
		if (!as->out_refused++)
			xa_log("usb: earpiece block of %d x %d bytes doesn't fit the endpoint (%u)\n",
			       frames, frame_len, g_audio.max_out);
		return -XA_EINVAL;
	}
	for (i = 0; i < AUD_OUT_BLOCKS; i++) {
		if (!g_audio.out[i].busy) {
			b = &g_audio.out[i];
			break;
		}
	}
	if (!b) {
		as->out_dropped++; /* behind: drop a block rather than lag */
		return 0;
	}
	memcpy(aud_out_buf[i], data, (size_t)(frame_len * frames));
	b->t.buffer = aud_out_buf[i];
	b->t.length = (u32)(frame_len * frames);
	/* always AUD_FRAMES USB frames: fewer packets leave the last frames empty, so the
	 * block still spans 8 ms */
	b->t.frames = AUD_FRAMES;
	for (int k = 0; k < USBD_ISOCH_FRAMES_MAX; k++)
		b->t.frame_length[k] = k < frames ? (u16)frame_len : 0;
	b->busy = 1;
	BARRIER();
	xa_usbd_run(aud_queue_out_dpc, b);
	return 0;
}

/* ---- headset audio: the driver loop's part ------------------------------------------------- */

static void win_check(void); /* the system window: see the driver thread */

/* every driver loop pass: received audio to gip.c, earpiece blocks, rates, status */
static void audio_poll(void)
{
	static u8 block[1800]; /* gip_headset_send's largest block (8 x 200 bytes) */
	struct xw_audio_status *as = &g_xw.audio;
	struct xw_port *p = g_audio.owner;
	u64 now = xa_now_ms();
	u32 flags = g_audio_kernel_ok ? AUD_F_KERNEL : 0, inflight = 0, tb_work;

	xd_time_pass(); /* diag: the loop's period (one audio_poll per pass) */
	tb_work = xd_timebase();
	aud_service_commands();
	/* a close waiting for blocks that never complete (RemoveDevice included): force it;
	 * and a close that never finishes at all is reported, once */
	{
		static u64 waiting_since, closing_since;
		static int reported;

		if (!g_audio.close_waiting) {
			waiting_since = 0;
		} else if (!waiting_since) {
			waiting_since = now;
		} else if (now > waiting_since + 2 * AUD_FORCE_MS) {
			waiting_since = 0;
			xa_log("usb: audio close still waiting for queued blocks: forcing it\n");
			xa_usbd_run(aud_force_dpc, NULL);
		}
		if (!g_audio.closing) {
			closing_since = 0;
		} else if (!closing_since) {
			closing_since = now;
		} else if (now > closing_since + 1000 && !reported) {
			reported = 1;
			g_audio.broken = 1;
			xa_log("usb: *** AUDIO CLOSE NEVER COMPLETED (1 s: opened %u, waiting %d, "
			       "forced %u, last close status %08x): audio is broken until the console "
			       "is rebooted; if the controller was unplugged, its removal is stuck "
			       "too ***\n", g_audio.opened, g_audio.close_waiting, g_xw.audio.watchdog,
			       g_xw.audio.close_status);
		}
	}
	p = g_audio.owner;
	if (p && p->gc && !p->gone) {
		struct gip_client *gc = p->gc;
		u32 gen = g_audio.enable_gen;

		/* audio packets (one per frame) to gip.c: they are 0x60 messages of client 1 */
		for (int n = 0; n < AUD_RX_SLOTS && g_audio.rx_read != g_audio.rx_write; n++) {
			u32 s = g_audio.rx_read % AUD_RX_SLOTS;

			BARRIER();
			if (aud_live_stage() >= 2)
				gip_rx(gc, g_audio.rx[s], g_audio.rx_len[s]);
			/* gip_rx may have turned audio off, or off and on again (which resets the
			 * ring): this packet is no longer ours to consume */
			if (g_audio.owner != p || g_audio.enable_gen != gen)
				break;
			BARRIER();
			g_audio.rx_read++;
		}
		p = g_audio.owner; /* gip_rx may have turned audio off */
	}
	if (p && p->gc && !p->gone) {
		struct gip_client *gc = p->gc;
		u32 stage = aud_live_stage();

		aud_announce(p, true); /* if it waited for the pad to be bound */
		if (g_audio.announced && g_audio.hs_up && gc->hs.state == GIP_HS_READY) {
			/* the headset's own volume settings and sidetone (voice_360.c picks
			 * them): sent when they change, at most every AUD_VOLUME_MS */
			static u64 next_volume;
			u8 want[GIP_VOL_FIELDS];

			xv_headset_volume(g_audio.announced_pad,
					  gc->hs.vol_reports ? gc->hs.vol_rx : NULL,
					  gc->hs.vol_rx_len, gc->hs.vol_reports, gc->hs.vol_tx,
					  gc->hs.vol_sent, want);
			if (memcmp(want, gc->hs.vol_tx, GIP_VOL_FIELDS) && now >= next_volume) {
				gip_headset_set_volume(gc, want);
				next_volume = now + AUD_VOLUME_MS;
			}
		}
		if (stage >= 4 && g_audio.hs_up && gc->hs.state == GIP_HS_READY &&
		    gc->hs.buffer_out && gc->hs.buffer_out <= sizeof(block) &&
		    ((g_audio.stage == 4 && g_audio.announced) || g_audio.stage == 5)) {
			/* paced by the USB clock, not ours: keep every OUT block on Usbd, refilling
			 * one as soon as it completes. A timer pace drifts against the frame clock
			 * and, with completions reported late, finds no free block (dropped audio,
			 * gaps in the stream: the headset then sends only ~900 mic packets/s) */
			for (int n = 0; n < AUD_OUT_BLOCKS && aud_out_free(); n++) {
				u32 bl = gc->hs.buffer_out;

				win_check(); /* a block's mix and EQ: the pass's largest pieces */
				if (g_audio.stage == 5) {
					aud_tone(block, bl, gc->hs.rate_out, gc->hs.channels_out);
				} else {
					xv_earpiece_fill(g_audio.announced_pad, block, (int)bl);
					if (as->dump == 2 && g_dump_ear &&
					    as->dump_ear_bytes + bl <= g_audio.dump_want) {
						memcpy(g_dump_ear + as->dump_ear_bytes, block, bl);
						as->dump_ear_bytes += bl;
					}
				}
				gip_headset_send(gc, block);
				as->ear_blocks++;
			}
		}
	}

	/* xw.py dump: ~1.5 s of the earpiece as it goes out (after every mix) */
	if (as->dump == 1 && !g_dump_ear)
		g_dump_ear = malloc(DUMP_EAR_BYTES);
	if (as->dump == 1 && g_dump_ear) {
		struct xw_port *o = g_audio.owner;
		u32 rate = o && o->gc ? o->gc->hs.rate_out : 0, ch = o && o->gc ? o->gc->hs.channels_out : 0;
		u32 want = rate * ch * 2 * DUMP_EAR_MS / 1000;

		as->dump_ear_bytes = 0;
		as->dump_ear_addr = (u32)g_dump_ear;
		as->dump_rate = rate;
		as->dump_channels = ch;
		g_audio.dump_want = want && want <= DUMP_EAR_BYTES ? want : DUMP_EAR_BYTES;
		as->dump = 2;
		xa_log("xw: earpiece dump armed: %u Hz x%u, %u bytes\n", rate, ch, g_audio.dump_want);
	} else if (as->dump == 2 && as->dump_ear_bytes + sizeof(block) > g_audio.dump_want) {
		as->dump = 3;
		xa_log("xw: earpiece dump done: %u bytes (%08x)\n", as->dump_ear_bytes,
		       as->dump_ear_addr);
	}
	if (now >= g_audio.next_rate) {
		/* per second of the timebase: the window is a second and up to a loop pass
		 * (~16 ms) longer, which counted 1008 packets for a mic sending 1000 a second */
		u32 tb = xd_timebase(), span = tb - g_audio.rate_tb;
		u32 mic = as->mic_packets - g_audio.last_mic, ear = as->ear_blocks - g_audio.last_ear;

		if (g_audio.rate_tb && span) {
			as->mic_pps = (u32)(((u64)mic * AUD_TB_HZ + span / 2) / span);
			as->ear_bps = (u32)(((u64)ear * AUD_TB_HZ + span / 2) / span);
		} else {
			as->mic_pps = mic;
			as->ear_bps = ear;
		}
		g_audio.rate_tb = tb;
		g_audio.last_mic = as->mic_packets;
		g_audio.last_ear = as->ear_blocks;
		g_audio.next_rate = now + 1000;
	}
	for (int i = 0; i < AUD_OUT_BLOCKS; i++)
		inflight += g_audio.out[i].queued;
	as->out_inflight = inflight;
	p = g_audio.owner;
	if (p && p->gc && !p->gone) {
		const struct gip_headset *hs = &p->gc->hs;

		g_xd.flow_below = hs->flow_below;
		g_xd.flow_equal = hs->flow_equal;
		g_xd.flow_above = hs->flow_above;
		g_xd.flow_min = hs->flow_min;
		g_xd.flow_max = hs->flow_max;
		g_xd.flow_last = hs->flow_last;
		g_xd.flow_fragment = hs->fragment_out;
	}
	if (g_audio.opened & 1)
		flags |= AUD_F_IN_OPEN;
	if (g_audio.opened & 2)
		flags |= AUD_F_OUT_OPEN;
	if (g_audio.streaming)
		flags |= AUD_F_STREAMING;
	if (g_audio.announced)
		flags |= AUD_F_ANNOUNCED;
	if (g_audio.broken)
		flags |= AUD_F_BROKEN;
	if (g_audio.hs_up)
		flags |= AUD_F_HS_UP;
	as->flags = flags;
	as->port = g_audio.owner ? (u32)aud_port_idx(g_audio.owner) : 0xFFFFFFFF;
	xd_time_work(tb_work);
}

static const struct gip_transport wired_transport = {
	.send = wired_send,
#ifdef XW_NO_AUTH
	.no_auth = true,
#else
	.no_auth = false, /* the security handshake: Microsoft pads show the headset only after it */
#endif
	.wired = true,
	.enable_audio = wired_enable_audio,
	.send_audio = wired_send_audio,
};

/* hot reload, cmd_mode 2 (xw.py reload --no-auth, a diagnostic): an adopted controller skips
 * the second security handshake (docs/PLAN.md 3.1, cause 6). A copy made at start */
static struct gip_transport wired_transport_noauth = { .wired = true };

/* ---- controllers ------------------------------------------------------------------------- */

static int pad_slot_used[PORTS];

void pad_ready(struct gip_client *gc)
{
	struct xw_port *p = gc->user;

	for (int i = 0; i < PORTS; i++) {
		if (pad_slot_used[i])
			continue;
		pad_slot_used[i] = 1;
		p->pad = i;
		panel_pad_reset(i); /* a new controller in this slot */
		xr_pad_reset(i);
		xa_log("pad %d: ready (port %d, %04x:%04x)\n", i, gc->id, p->vid, p->pid);
		ho_gate('T', 7, "bind the pad in XAM");
		trace(TR_PAD_BIND | (u32)i << 16);
		xa_xinput_bind(i);
		trace(TR_PAD_BOUND | (u32)i << 16);
		/* adopted (or a refused release putting it back): muted as it was */
		if ((p->ho_flags & HO_PF_MUTED) && p->adopt_session == p->session &&
		    !xv_mic_muted(i))
			xv_toggle_mic_mute(i);
		xa_xinput_notify(XA_NOTIFY_PAD_ON);
		return;
	}
	xa_log("pad: port %d ready, but all pads are taken\n", gc->id);
}

static void pad_input(struct gip_client *gc, const struct gip_gamepad_state *s)
{
	struct xw_port *p = gc->user;
	struct xw_pad_status *ps = &g_xw.pads[p - g_ports];
	bool changed = ps->buttons != s->buttons || ps->guide != s->guide;

	ps->buttons = s->buttons;
	ps->guide = s->guide;
	ps->trigger_left = s->trigger_left;
	ps->trigger_right = s->trigger_right;
	ps->stick_left_x = s->stick_left_x;
	ps->stick_left_y = s->stick_left_y;
	ps->stick_right_x = s->stick_right_x;
	ps->stick_right_y = s->stick_right_y;
	if (p->pad < 0)
		return;
	{
		struct gip_gamepad_state st = *s;

		panel_filter(p->pad, &st); /* Share + D-pad / bumpers: the mix, not the title */
		xa_xinput_update(p->pad, &st);
	}
	if (changed)
		xa_log("pad %d: buttons %04x%s\n", p->pad, s->buttons, s->guide ? " Guide" : "");
}

void pad_release(struct xw_port *p)
{
	if (p->pad < 0)
		return;
	xa_log("pad %d: off\n", p->pad);
	xv_pad_releasing(p->pad); /* the headset (category 1) before the pad itself */
	xa_xinput_unbind(p->pad);
	panel_pad_reset(p->pad);
	pad_slot_used[p->pad] = 0;
	p->pad = -1;
}

static void pad_gone(struct gip_client *gc)
{
	/* turned off: gip.c has already forgotten the headset (without telling us) */
	aud_disable(gc->user, "controller turned off");
	pad_release(gc->user);
}

/* gip.c: the jack headset is ready (format agreed, powered on) or gone */
static void pad_headset(struct gip_client *gc, bool present)
{
	struct xw_port *p = gc->user;

	if (g_audio.owner != p)
		return;
	if (!present) {
		aud_announce(p, false);
		g_audio.hs_up = 0;
		return;
	}
	g_audio.hs_up = 1;
	if (g_audio.stage == 2 && g_audio.streaming) {
		xa_log("xw: port %d: audio stage 2: one IN block\n", aud_port_idx(p));
		xa_usbd_run(aud_oneshot_dpc, NULL);
	}
	aud_announce(p, true);
}

/* gip.c: microphone samples (a 0x60 packet's payload after the flow rate) */
static void pad_audio_in(struct gip_client *gc, const u8 *samples, int len)
{
	struct xw_port *p = gc->user;
	struct xw_audio_status *as = &g_xw.audio;

	if (g_audio.owner != p || len <= 0)
		return;
	as->mic_packets++;
	as->mic_bytes += (u32)len;
	aud_analyse(samples, len, gc->hs.rate_in, gc->hs.channels_in);
	aud_dump(samples, len, gc->hs.rate_in, gc->hs.channels_in);
	if (g_audio.announced && aud_live_stage() >= 3)
		xv_mic_samples(g_audio.announced_pad, samples, len);
}

static const struct gip_ops pad_ops = {
	.ready = pad_ready,
	.input = pad_input,
	.gone = pad_gone,
	.headset = pad_headset,
	.audio_in = pad_audio_in,
};

static void port_drop(struct xw_port *p)
{
	if (!p->gc)
		return;
	aud_disable(p, "controller gone"); /* before the pad is unbound */
	pad_release(p);
	gip_client_free(p->gc);
	p->gc = NULL;
	g_xw.pads[p - g_ports].connected = 0;
}

/* the headset controls and the on-screen bar (panel_360.c) */
int xw_headset_pad(void)
{
	return g_audio.announced ? g_audio.announced_pad : -1;
}

bool xw_headset_live(void)
{
	return g_audio.announced && g_audio.streaming;
}

/* the driver thread's view of one port: new controller, gone, or packets to handle */
static void service_port(struct xw_port *p)
{
	int idx = (int)(p - g_ports);
	struct xw_pad_status *ps = &g_xw.pads[idx];
	u32 session = p->session;

	if (p->gc && (p->gone || !p->node || session != p->seen_session)) {
		xa_log("xw: port %d: controller gone\n", idx);
		port_drop(p);
	}
	if (!p->gc && p->node && !p->gone && session != p->seen_session) {
		if (p->adopt_session == session)
			ho_gate('T', 5, "serve the adopted controller: gip client, IN started");
		p->seen_session = session;
		p->pad = -1;
		p->gc = gip_client_new_transport(p->adopt_session == session && g_ho.cmd_mode == 2 ?
						 &wired_transport_noauth : &wired_transport,
						 p, idx, &pad_ops, p);
		if (!p->gc) {
			xa_log("xw: port %d: out of memory\n", idx);
			return;
		}
		ps->connected = 1;
		p->probe_at = 0;
		p->adopt_pending = 0;
		if (p->adopt_session == session) {
			/* taken over from the previous build with no USB reset: it is switched on
			 * and silent, so ask who it is now; then for the headset it had */
			xa_log("xw: port %d: controller taken over from the previous build\n", idx);
			trace(TR_PORT(TR_PORT_GC, p));
			p->adopt_pending = 1; /* gip_adopt, once IN is started (below) */
			p->probe_at = (p->ho_flags & HO_PF_HEADSET) ? 1 : 0;
		} else {
			xa_log("xw: port %d: controller connected, waiting for it to announce "
			       "itself\n", idx);
		}
		p->in_started = 0;
		p->fb_mark = 0;
		g_xw.audio.fallback_rx[idx] = 0xFFFFFFFF;
	}
	if (!p->gc)
		return;
	if (!p->in_started && g_xw.go >= 4) {
		p->in_started = 1;
		xa_log("xw: port %d: starting IN\n", idx);
		xa_usbd_run(start_port, p);
	}
	if (p->in_started == 1 && g_xw.go >= 5) {
		p->in_started = 2;
		xa_log("xw: port %d: IN continuous\n", idx);
		xa_usbd_run(start_port, p); /* re-queues it if the one-shot transfer has finished */
	}
	if (p->adopt_pending) {
		/* taken over: it is switched on and silent, so ask who it is now */
		ho_gate('T', 6, "ask the adopted controller to identify itself (gip_adopt)");
		p->adopt_pending = 0;
		gip_adopt(p->gc);
		trace(TR_PORT(TR_PORT_ADOPT_SENT, p));
	}

	for (int n = 0; g_xw.go >= 6 && n < DRAIN_MAX && p->rx_read != p->rx_write; n++) {
		u32 s = p->rx_read % RX_SLOTS;

		BARRIER();
		if (xa_debug > 1)
			xa_log("usb: port %d in: %02x %02x %02x %02x\n", idx, p->rx[s][0], p->rx[s][1],
			       p->rx[s][2], p->rx[s][3]);
		if (idx == 0 && p->rx[s][0] == 0x20)
			memcpy((void *)g_xw.last_input, p->rx[s], sizeof(g_xw.last_input));
		if (p->rx[s][0] == 0x20 && p->pad >= 0)
			panel_raw_input(p->pad, p->vid, p->pid, p->rx[s], p->rx_len[s]);
		gip_rx(p->gc, p->rx[s], p->rx_len[s]);
		BARRIER();
		p->rx_read++;
	}
	if (p->pad >= 0)
		xa_xinput_poll(p->pad);
	/* the title's rumble and the Vibration setting (rumble_360.h); before gip_poll sends */
	if (p->pad >= 0)
		xr_service(p->pad, p->gc, pExecutableModule ? (u32)*pExecutableModule : 0);
	gip_poll(p->gc);
	if (p->probe_at && p->gc->ready) {
		/* adopted: the headset announced itself to the previous build only. Give the
		 * controller's start (and its security handshake) a moment, then ask */
		if (p->probe_at == 1) {
			p->probe_at = xa_now_ms() + HO_PROBE_DELAY_MS;
		} else if (xa_now_ms() > p->probe_at) {
			p->probe_at = 0;
			/* with the format the previous build's headset had: restarted as it is (it
			 * ignores an identify); without one, asked to identify itself */
			if (p->gc->hs.state == GIP_HS_NONE && p->ho_hs_fmt)
				gip_headset_resume(p->gc, (u8)p->ho_hs_fmt, (u8)(p->ho_hs_fmt >> 8));
			else if (p->gc->hs.state == GIP_HS_NONE)
				gip_headset_probe(p->gc);
		}
	}

	/* did the start fallback's power-on wake a silent pad? (IN packets since then) */
	if (p->gc && p->gc->fallback && !p->fb_mark) {
		p->fb_mark = 1;
		p->fb_rx = p->rx_count;
		p->fb_time = xa_now_ms();
		xa_log("xw: port %d: started blind (no announce); counting IN traffic\n", idx);
	}
	if (p->fb_mark) {
		g_xw.audio.fallback_rx[idx] = p->rx_count - p->fb_rx;
		if (p->fb_mark == 1 && xa_now_ms() > p->fb_time + 2000) {
			p->fb_mark = 2;
			xa_log("xw: port %d: %u IN packets in the 2 s after the blind start%s\n", idx,
			       p->rx_count - p->fb_rx,
			       p->rx_count == p->fb_rx ? " (still silent: xw.py usbreset?)" : "");
		}
	}

	if (idx == 0)
		memcpy((void *)g_xw.in_block_now, &p->in[0], sizeof(g_xw.in_block_now));
	ps->vid = p->vid;
	ps->pid = p->pid;
	ps->rx = p->rx_count;
	ps->tx = p->tx_count;
	ps->rx_dropped = p->rx_dropped;
	ps->rx_errors = p->rx_errors;
	ps->tx_dropped = p->tx_dropped;
	ps->tx_errors = p->tx_errors;
}

/* ---- boot: controllers plugged in before we loaded (as xa_plugin.c) ------------------------ */

/* 0 = never grab (xbdm-writable); off in XW_STAGED builds, which test one step at a time */
#ifdef XW_STAGED
__attribute__((used)) volatile u32 xw_boot_grab = 0;
#else
__attribute__((used)) volatile u32 xw_boot_grab = 1;
#endif

static int any_port_used(void)
{
	return ports_free() != PORTS;
}

static void rescan_on_usbd_dpc(void *context)
{
	pResetAllUnrecognized(context);
}

static void *find_unrecognized_device(void)
{
	for (int i = 0; i < K_UNRECOGNIZED_COUNT; i++) {
		void *node = *(void *volatile *)(K_UNRECOGNIZED + i * 8);

		if (node)
			return node;
	}
	return NULL;
}

/* the title as the driver thread sees it, for xw.py tee and reload --settle (handover.h) */
void ho_title(void)
{
	g_ho.title = pExecutableModule ? (u32)*pExecutableModule : 0;
}

void wait_ms(unsigned int ms)
{
	for (unsigned int i = 0; i < ms; i += 10) {
		g_xw.heartbeat++;
		ho_title();
		xa_sleep_ms(10);
	}
}

static void wait_for_title_switch(void)
{
	HANDLE title = pExecutableModule ? *pExecutableModule : 0;
	unsigned int ms, stable = 0;
	bool switched = false;

	for (ms = 0; ms < TITLE_WAIT_MS && !any_port_used(); ms += 100) {
		HANDLE now = pExecutableModule ? *pExecutableModule : 0;

		if (now != title) {
			title = now;
			switched = true;
			stable = 0;
		} else if ((stable += 100) >= TITLE_SETTLE_MS && (switched || !pExecutableModule)) {
			break;
		}
		wait_ms(100);
	}
	xa_log("xw: title %s after %u ms\n", switched ? "switched and settled" : "unchanged", ms);
}

static void grab_present_controllers(void)
{
	volatile s32 *done = (volatile s32 *)(g_rescan_ctx + RESCAN_EVENT + 4);
	u32 matches = g_xw.match_calls;
	void *node = find_unrecognized_device();
	unsigned int ms;

	if (!node) {
		xa_log("xw: no device waiting in the unrecognized list\n");
		return;
	}
	if (!pResetAllUnrecognized) {
		xa_log("xw: a device is waiting (node %08x), but no rescan routine: re-plug it\n",
		       (u32)node);
		return;
	}
	memset(g_rescan_ctx, 0, sizeof(g_rescan_ctx));
	KeInitializeEvent((KEVENT *)(g_rescan_ctx + RESCAN_EVENT), 0, 0);
	BARRIER();
	xa_log("xw: unrecognized device present (node %08x): asking Usbd to offer it again\n",
	       (u32)node);
	xa_usbd_run(rescan_on_usbd_dpc, g_rescan_ctx);
	for (ms = 0; ms < RESCAN_WAIT_MS && !*done; ms += 10)
		wait_ms(10);
	xa_log("xw: rescan %s after %u ms, match calls +%u, %d controller(s) claimed\n",
	       *done ? "done" : "still running", ms, g_xw.match_calls - matches,
	       PORTS - ports_free());
}

/* ---- driver thread ------------------------------------------------------------------------- */
/* the CPU's timebase (50 MHz), for the loop timing */
#define TIMEBASE_PER_US 50
static u32 timebase(void)
{
	u32 t;

	__asm__ __volatile__("mftb %0" : "=r"(t));
	return t;
}

static u8 g_ceiling; /* the thread's own priority ceiling (KTHREAD +0xCA) as created */

static void raise_priority(void)
{
	u8 *kt = current_kthread();

	g_ceiling = kt[KTHREAD_PRIORITY_MAX];
	KeSetBasePriorityThread(kt, (u32)(THREAD_PRIORITY - kt[KTHREAD_PRIORITY_BASE]));
	xa_log("xw: driver thread priority %u\n", kt[KTHREAD_PRIORITY]);
}

/* ---- the system window ------------------------------------------------------------------
 * Read offline in the 17559 dump (docs/INTERNALS.md "The system window"): at each vertical
 * refresh the display interrupt (0x800D03B8 -> 0x80074E58) opens a window on the hardware
 * threads in K_WIN_CPUS (0x3C: 2-5). Its dispatch (0x800787D0) loads the decrementer with
 * K_WIN_LEN (50000 timebase ticks = 1 ms) and enables priorities 0-9 only (mask 0xEDB403FF);
 * the decrementer's interrupt (0x8007627C) ends it, and until the next refresh 2-9 are masked
 * off (0xF6DBFC03). This thread (9, hardware thread 2) therefore runs only in that window:
 * a pass still running at its end stops for the rest of the frame, ~15.6 ms.
 * Priorities enabled in both masks keep running across the end; WIN_PRIO is one (0, 1, 20,
 * 23, 26 and 29-31 are). */
#define K_WIN_CPUS 0x8017067C
#define K_WIN_LEN 0x80170680
#define WIN_PRIO 20
#define WIN_CUT_US 4000 /* a pass this long cannot have run in one window: it was stopped */

static const u32 win_code[] = {
	0x3D608017, /* 800787e0 lis r11,0x8017 */
	0x816B0680, /* 800787e4 lwz r11,0x680(r11): K_WIN_LEN */
	0x7D7603A6, /* 800787e8 mtdec r11 */
	0x3FA0EDB4, /* 800787ec lis r29,0xEDB4 */
	0xB36D001A, /* 800787f0 sth r27,26(r13) */
	0x9B8D0019, /* 800787f4 stb r28,25(r13) */
	0x63BD03FF, /* 800787f8 ori r29,r29,0x3FF: priorities 0-9 in the window */
};
static const struct sig win_sig = { 0x800787E0, 7, win_code };

static u32 g_win_ok;     /* the code above matched: the window's length can be read */
static u32 g_pass_start; /* this pass's start (timebase): about the window's opening */
static u32 g_boosted;    /* this pass went on at WIN_PRIO */

static void set_priority(u32 prio)
{
	u8 *kt = current_kthread();

	/* KeSetBasePriorityThread clamps the base to the ceiling (+0xCA, 9 for a system thread):
	 * raised first to go above it, and put back on the way down, because with the base
	 * under the ceiling the kernel charges a wait against the thread's quantum instead of
	 * renewing it (0x80077D58) */
	if (prio > kt[KTHREAD_PRIORITY_MAX])
		kt[KTHREAD_PRIORITY_MAX] = (u8)prio;
	KeSetBasePriorityThread(kt, (u32)((s32)prio - (s32)kt[KTHREAD_PRIORITY_BASE]));
	if (prio <= g_ceiling)
		kt[KTHREAD_PRIORITY_MAX] = g_ceiling;
}

static void win_init(void)
{
	u32 at = sig_mismatch(&win_sig);

	g_win_ok = !at && (*(volatile u32 *)K_WIN_CPUS & (1u << 2));
	xa_log("xw: system window %s (%u us)\n",
	       at ? "code differs from 17559's: not read" :
	       g_win_ok ? "on hardware thread 2" : "not on hardware thread 2",
	       g_win_ok ? *(volatile u32 *)K_WIN_LEN / TIMEBASE_PER_US : 0);
}

/* Before each piece of a pass that may take a while (an earpiece block, a poll): with xbdm's
 * win_boost on, a pass already 5/8 of a window long finishes at WIN_PRIO, so the window's end
 * cannot stop it; it then takes its overrun from whatever title thread runs on hardware
 * thread 2. 3/8 of a window (375 us at 1 ms) is left for the piece about to start and for the
 * pass's late start in its window (the display interrupt, Usbd's DPCs). The loop goes back to
 * THREAD_PRIORITY after its sleep, where it waits for the next window anyway. */
static void win_check(void)
{
	u32 len;

	if (g_boosted || !g_xw.win_boost || !g_win_ok)
		return;
	len = *(volatile u32 *)K_WIN_LEN; /* re-read: the kernel can change it with the mode */
	if (timebase() - g_pass_start < len / 8 * 5)
		return;
	set_priority(WIN_PRIO);
	g_boosted = 1;
	g_xw.win_boosts++;
}

static void gate(u32 n, const char *what)
{
	if (g_xw.go < n)
		xa_log("xw: gate %u (%s): waiting for xw.py go %u\n", n, what, n);
	while (g_xw.go < n)
		wait_ms(100);
	xa_log("xw: gate %u: %s\n", n, what);
}

BODY u32 XwThreadC(void *context)
{
	(void)context;
	g_xw.stage = STAGE_THREAD;
	xa_log("xw: driver thread started\n");
	raise_priority();
	g_ho.detours = (u32)&detour_registry;
	xv_init();
	panel_init();
	hud_init();
	xt_init();
	xb_init();
	xf_init();
	xq_init();
	xs_init();

	/* hot reload: a slot build learns here what it replaces, before it hooks anything */
	if (!takeover_import()) {
		g_xw.state = XW_IDLE;
		for (;;)
			wait_ms(1000);
	}

	gate(1, "installing the XInput hooks");
	ho_gate('T', 2, "XInput: XAM thread started, the 4 hook slots stored");
	trace(TR_TAKE_HOOKS);
	for (unsigned int ms = 0; xa_xinput_init() == -XA_ENODEV && ms < BOOT_RETRY_MS; ms += 1000)
		wait_ms(1000);
	trace(TR_TAKE_HOOKED);
	g_xw.stage = STAGE_HOOKS;

	/* link on Usbd's processor, serialised with Usbd itself */
	gate(2, "linking the USB class driver");
	ho_gate('T', 3, "link the USB class driver (Usbd's DPC)");
	trace(TR_TAKE_LINK);
	ke_dpc.initialize(&g_link_dpc, (void *)XwLinkDpc, 0);
	g_link_dpc.target_processor = ke_dpc.usbd_processor;
	ke_dpc.insert(&g_link_dpc, 0, 0);
	while (g_xw.state != XW_WAITING)
		wait_ms(10);
	trace(TR_TAKE_LINKED);
	g_xw.stage = STAGE_LINKED;

	/* hot reload: the previous build's controllers, now that new ones come to us too */
	if (g_pred)
		ho_gate('T', 4, "adopt the controllers (adopt_dpc on Usbd's DPC)");
	takeover_adopt();

	/* (not after a take-over: the previous build did this at boot, and what it held is
	 * ours now) */
	gate(3, "offering already-connected controllers again");
	if (xw_boot_grab && !g_pred) {
		wait_for_title_switch();
		grab_present_controllers();
	}
	g_xw.stage = STAGE_GRABBED;
	g_xw.state = XW_RUNNING;
	g_xw.stage = STAGE_RUNNING;
	trace(TR_TAKE_NOTIFY);
	xa_xinput_notify(XA_NOTIFY_READY);
	xa_log("xw: waiting for controllers\n");
	g_ho.state = HO_RUNNING;
	trace(TR_TAKE_RUNNING);

	win_init();
	for (u32 tb_prev = 0, skipped = 0;;) {
		u32 tb, run;

		if (g_boosted) {
			/* after the sleep: outside the window, so this thread now waits for the next
			 * one, where the pass below starts */
			set_priority(THREAD_PRIORITY);
			g_boosted = 0;
		}
		tb = timebase();
		g_pass_start = tb;
		if (tb_prev) {
			u32 us = (tb - tb_prev) / TIMEBASE_PER_US;

			g_xw.loop_us = us;
			if (us > g_xw.loop_max_us)
				g_xw.loop_max_us = us;
		}
		tb_prev = tb;
		if (g_ho.cmd_release) {
			u32 cmd = g_ho.cmd_release;

			g_ho.cmd_release = 0;
			do_release(cmd); /* returns only if the release was refused */
		}
		if (g_notify_full) {
			g_notify_full = 0;
			xa_xinput_notify(XA_NOTIFY_PAD_FULL);
		}
		xa_debug = (int)g_xw.debug;
		if (xa_xinput_mute != g_xw.mute) {
			xa_xinput_mute = g_xw.mute;
			xa_log("xw: test mode %s\n", g_xw.mute ? "on: the console sees no input" : "off");
		}
		for (int i = 0; i < PORTS; i++)
			service_port(&g_ports[i]);
		win_check();
		audio_poll();
		win_check();
		xv_poll();
		win_check();
		panel_poll();
		win_check();
		hud_poll();
		win_check();
		xt_poll();
		xb_poll();
		xq_poll();
		xs_poll();
		ho_title();
		g_xw.heartbeat++;
		run = (timebase() - tb) / TIMEBASE_PER_US;
		g_xw.run_us = run;
		if (run > g_xw.run_max_us)
			g_xw.run_max_us = run;
		g_xw.win_us = g_win_ok ? *(volatile u32 *)K_WIN_LEN / TIMEBASE_PER_US : 0;
		/* Stopped by the window's end (at THREAD_PRIORITY nothing else stops a pass for a
		 * frame): it resumed at the start of a new window, where this thread has its whole
		 * share again. Sleeping now would wait for the window after it and leave this one
		 * unused: two frames for one pass, and the earpiece's 40 ms of queued blocks run
		 * down. So the next pass starts at once, but only once in a row, so that a loop
		 * whose passes no longer fit a window still leaves the lower priorities some. */
		if (run > WIN_CUT_US && !g_boosted) {
			g_xw.win_cuts++;
			if (!skipped) {
				skipped = 1;
				continue;
			}
		}
		skipped = 0;
		xa_sleep_ms(2);
	}
}

/* ---- start ---------------------------------------------------------------------------------- */

static int resolve(HANDLE krnl, u32 ordinal, void *out)
{
	void *p = 0;

	XexGetProcedureAddress(krnl, ordinal, &p);
	if (!p) {
		g_xw.state = XW_RESOLVE_FAILED;
		g_xw.last_error = ordinal;
		return 0;
	}
	*(void **)out = p;
	return 1;
}

static int check_code(u32 addr, u32 expected)
{
	if (*(volatile u32 *)addr == expected)
		return 1;
	g_xw.state = XW_CODE_MISMATCH;
	g_xw.last_error = addr;
	return 0;
}

static void start(void)
{
	HANDLE krnl, thread;
	u32 tid;

	g_xw.state = XW_STARTED;
	g_xw.log_addr = (u32)&xa_log_ring;
	g_ho.trace_ring = (u32)&g_trace;
	wired_transport_noauth = wired_transport;
	wired_transport_noauth.no_auth = true;
	for (int i = 0; i < PORTS; i++) {
		g_ports[i].pad = -1;
		g_ports[i].open_status[0] = g_ports[i].open_status[1] = g_ports[i].open_status[2] = -1;
	}
	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0)
		return;
	if (!resolve(krnl, ORD_KeInitializeDpc, &ke_dpc.initialize) ||
	    !resolve(krnl, ORD_KeInsertQueueDpc, &ke_dpc.insert) ||
	    !resolve(krnl, ORD_UsbdResetDevice, &usbd.reset_device) ||
	    !resolve(krnl, ORD_UsbdCancelAsyncTransfer, &usbd.cancel_async_transfer) ||
	    !resolve(krnl, ORD_UsbdAddDeviceComplete, &usbd.add_device_complete) ||
	    !resolve(krnl, ORD_UsbdOpenDefaultEndpoint, &usbd.open_default_endpoint) ||
	    !resolve(krnl, ORD_UsbdOpenEndpoint, &usbd.open_endpoint) ||
	    !resolve(krnl, ORD_UsbdQueueAsyncTransfer, &usbd.queue_async_transfer) ||
	    !resolve(krnl, ORD_UsbdQueueCloseDefaultEndpoint, &usbd.queue_close_default_endpoint) ||
	    !resolve(krnl, ORD_UsbdQueueCloseEndpoint, &usbd.queue_close_endpoint) ||
	    !resolve(krnl, ORD_UsbdRemoveDeviceComplete, &usbd.remove_device_complete) ||
	    !resolve(krnl, ORD_UsbdGetEndpointDescriptor, &pUsbdGetEndpointDescriptor))
		return;
	{
		void *p = 0;

		XexGetProcedureAddress(krnl, ORD_UsbdTitleDriverResetAllUnrecognizedPorts, &p);
		if ((u32)p == 0x800D9BB8 && ((u32 *)p)[0] == 0x7D8802A6 && ((u32 *)p)[1] == 0x4803300D)
			pResetAllUnrecognized = p;
		p = 0;
		XexGetProcedureAddress(krnl, ORD_XexExecutableModuleHandle, &p);
		pExecutableModule = p;
	}

	/* headset audio: UsbdQueueIsochTransfer and the code usbd.h describes, or no audio */
	{
		void *p = 0;

		XexGetProcedureAddress(krnl, ORD_UsbdQueueIsochTransfer, &p);
		if ((u32)p == 0x800D82C0 && ((u32 *)p)[0] == 0x89430008 &&
		    ((u32 *)p)[2] == 0x396B8230 && ((u32 *)p)[5] == 0x89660096 &&
		    *(volatile u32 *)0x800DC9A8 == 0x7D8802A6 &&
		    *(volatile u32 *)0x800DC9C4 == 0x833A0010 &&  /* buffer at +0x10 */
		    *(volatile u32 *)0x800DCCF0 == 0x806A0020 &&  /* TD +32 = block */
		    *(volatile u32 *)0x800D5D20 == 0x7D8802A6 &&
		    *(volatile u32 *)0x800D5D3C == 0x809F0014 &&  /* length at +0x14 */
		    *(volatile u32 *)0x800D83E0 == 0x8943000B &&  /* GetEndpointDescriptor */
		    *(volatile u32 *)0x800D83F0 == 0x396B8220) {
			pUsbdQueueIsochTransfer = p;
			g_audio_kernel_ok = 1;
		}
	}
	if (!check_code(0x800D6184, 0x3D60801B) || !check_code(0x800D618C, 0x3BCB8048) ||
	    !check_code(K_WGC_DRIVER + 0x0C, 0x800F3400) ||
	    *(volatile u16 *)K_USBD_TIMER_DPC != 0x0013)
		return;

	ke_dpc.usbd_processor = *(volatile u8 *)(K_USBD_TIMER_DPC + 3);
	xa_usbd_calls_init();

	/* an interface-level driver, as the console's own wireless (FF/5D/81) driver is; the
	 * thread links it (after the XInput hooks) */
	g_driver.flags = *(volatile u32 *)(K_WGC_DRIVER + 0x08);
	g_driver.add_device = XwAddDevice;
	g_driver.remove_device = XwRemoveDevice;
	g_driver.match = XwMatch;

	xa_log("xw: starting\n");
	ExCreateThread(&thread, THREAD_STACK, &tid, 0, (void *)XwThread, 0, THREAD_FLAGS);
	g_xw.stage = STAGE_DLLMAIN;
}

BODY int DllMainC(unsigned int handle, unsigned int reason, unsigned int reserved)
{
	(void)handle, (void)reserved;
	if (reason == 1 /* DLL_PROCESS_ATTACH */)
		start();
	return 1;
}
