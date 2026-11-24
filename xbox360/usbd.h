/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The kernel's USB driver interface (Usbd) as kernel 17559 has it. Nothing here is
 * declared by the toolchain's headers; layouts come from hiddriver360 (EinTim23, GPL-3.0)
 * and the kernel's own drivers (see docs/INTERNALS.md).
 *
 * Every callback the kernel makes (class-driver entries, transfer completions) runs on
 * Usbd's DPC and must go through an NV64 thunk (thunks.s).
 *
 * Usbd takes no locks: queueing, cancelling and resetting manipulate the host
 * controller's lists directly. The kernel's own drivers only call it from Usbd's DPC
 * (or its timers), so the driver thread does too, through platform_360.c's call queue.
 */
#pragma once

#include <stdint.h>

/* entry in the runtime class-driver list at 0x801A8048, walked by UsbdSelectClassDriver */
struct usbd_class_driver {
	struct usbd_class_driver *flink, *blink;
	uint32_t flags;
	void (*add_device)(void *node);
	void (*remove_device)(void *node);
	uint32_t (*match)(const uint8_t *ifdesc, const uint8_t *devdesc,
			  struct usbd_class_driver *self);
	void (*after_add)(void *node, uint32_t one, void *ctx);
};

/*
 * A transfer block. UsbdOpen*Endpoint fills `endpoint`; before each queue the
 * endpoint is copied to `saved_endpoint`. To close an endpoint, put its handle in a
 * block's `saved_endpoint` and queue the close with that block.
 */
struct usbd_transfer {
	uint32_t endpoint;
	void *callback;           /* long cb(struct usbd_transfer *, long status) */
	uint32_t saved_endpoint;
	uint32_t cancel_link;     /* Usbd: next block on the controller's cancel list */
	uint8_t flags;            /* 1 for every transfer */
	uint8_t state;            /* Usbd: 0 once queued, 1 when cancelling/finished */
	uint8_t controller_index; /* written by UsbdQueueAsyncTransfer */
	uint8_t endpoint_index;   /* written by UsbdQueueAsyncTransfer */
	void *buffer;
	uint32_t length;
};

/* control transfer: the setup packet's 16-bit fields are stored byte-swapped */
struct usbd_control_transfer {
	struct usbd_transfer t;
	uint8_t reserved[4];
	uint8_t request_type;
	uint8_t request;
	uint16_t value;
	uint16_t index;
	uint16_t length;
};

/*
 * An isochronous transfer block (UsbdQueueIsochTransfer, ordinal 895). Not the async
 * layout: read from 17559's OHCI code (queue 0x800DC9A8, completion 0x800DCCE0 ->
 * 0x800D5D20, abort on close 0x800D5D68), not from documentation:
 * - queue(node, block, frame_lengths): one OHCI iTD per block, start frame = the ED's next
 *   frame if transfers are pending, else the current frame; one PSW per frame at
 *   (buffer & 0xFFF) + sum of the previous lengths; BP0/BE = physical buffer start/end.
 *   Nothing in the block is written. The TD comes from a pool of 12 isochronous TDs shared
 *   by every OHCI endpoint (0x801A7F88; an empty pool is dereferenced, not reported), and
 *   each open endpoint keeps one of them as its tail.
 * - completion: callback(block, long status[frames], u16 length[frames]); both arrays are
 *   on Usbd's stack. Status per frame from the PSW condition code (table 0x800447F8):
 *   0 ok, 0xC0050003 data underrun (a short IN packet: the data is valid), 0xC005100E not
 *   accessed (frame missed, length 0), others errors. A close aborts pending blocks with
 *   0xC0000120 in every entry (48 of them) and length 0.
 * - UsbdCancelAsyncTransfer must never be given this block: it reads +0x11/+0x12, which are
 *   the buffer pointer here.
 */
struct usbd_isoch_transfer {
	uint32_t endpoint;        /* 0x00 UsbdOpenEndpoint fills it */
	void *callback;           /* 0x04 */
	uint32_t saved_endpoint;  /* 0x08 the endpoint, copied before each queue */
	uint32_t cancel_link;     /* 0x0C Usbd: abort list when the endpoint closes */
	void *buffer;             /* 0x10 */
	uint32_t length;          /* 0x14 total bytes */
	uint8_t frames;           /* 0x18 1 ms frames, at most 8 on OHCI (one iTD) */
	uint8_t reserved[3];
	uint16_t frame_length[8]; /* 0x1C per-frame lengths, passed as the third argument */
};

#define USBD_TYPE_ISOCH 1
#define USBD_TYPE_BULK 2
#define USBD_TYPE_INTERRUPT 3
#define USBD_ISOCH_FRAMES_MAX 8

/* kernel ordinals */
#define ORD_KeInitializeDpc 0x06F
#define ORD_KeInsertQueueDpc 0x07B
#define ORD_UsbdAddDeviceComplete 740
#define ORD_UsbdCancelAsyncTransfer 741
#define ORD_UsbdOpenDefaultEndpoint 746
#define ORD_UsbdOpenEndpoint 747
#define ORD_UsbdQueueAsyncTransfer 748
#define ORD_UsbdQueueCloseDefaultEndpoint 749
#define ORD_UsbdQueueCloseEndpoint 750
#define ORD_UsbdRemoveDeviceComplete 751
#define ORD_UsbdResetDevice 758 /* (node): the kernel's export ignores a second argument */
#define ORD_UsbdQueueIsochTransfer 895

struct usbd_api {
	void (*add_device_complete)(void *node, long status);
	long (*open_default_endpoint)(void *node, struct usbd_control_transfer *t);
	long (*open_endpoint)(void *node, int type, int address, int max_packet, int interval,
			      struct usbd_transfer *t);
	void (*queue_async_transfer)(void *node, void *t);
	void (*queue_close_default_endpoint)(void *node, void *t);
	void (*queue_close_endpoint)(void *node, void *t);
	void (*remove_device_complete)(void *node);
	/* completes the transfer later, through its callback, with an error status */
	void (*cancel_async_transfer)(void *t);
	void (*reset_device)(void *node, uint32_t reenumerate);
};

struct kdpc360 {
	uint16_t type;
	uint8_t inserted;
	uint8_t target_processor; /* processor + 1, 0 = current */
	void *flink, *blink;
	void (*routine)(struct kdpc360 *dpc, void *context, void *arg1, void *arg2);
	void *context, *arg1, *arg2;
};

struct ke_dpc_api {
	void (*initialize)(struct kdpc360 *dpc, void *routine, void *context);
	uint32_t (*insert)(struct kdpc360 *dpc, void *arg1, void *arg2);
	uint8_t usbd_processor; /* target_processor value of Usbd's own DPC */
};

extern struct ke_dpc_api ke_dpc;

extern struct usbd_api usbd;
