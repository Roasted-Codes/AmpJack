// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Hard-disk files (file_360.h): one worker thread that runs posted jobs, and the blocking
 * whole-file read and write those jobs use.
 *
 * Threads and locking:
 *   - xf_post (producers: the driver thread in practice; any thread allowed) reserves a queue
 *     slot with one compare-and-swap on g_head, fills it, then publishes it with `ready`
 *     after an lwsync. No lock: nothing here may block the driver thread.
 *   - The worker (XfWorker, its own system thread) is the only reader of the queue and the
 *     only writer of g_tail. It waits on a synchronization KEVENT that xf_post sets, with a
 *     1 s timeout, so a lost wake-up costs at most a second.
 *   - xf_quiesce (driver thread) and the worker meet through g_quiesced / g_busy with full
 *     `sync` barriers (a store then a load on each side: lwsync does not order that).
 * The worker runs at the default priority of a system thread (below the driver thread's 9) on
 * hardware thread 3: not 2 (driver thread, Usbd's DPCs), 4 (the XAM thread, the audio render
 * thread) or 5 (team voice's net thread). 3 is where the HUD's draw thread is created and is
 * known to run at the default priority; on 1 a default-priority thread never ran (hud_360.c).
 * If something busier holds hardware thread 3, jobs are late, never lost, and nobody waits for
 * them except xf_quiesce (bounded). Jobs do not log: xa_log is not thread-safe.
 *
 * Kernel rules (docs/INTERNALS.md): the thread entry is an NV64 thunk (below); NtCreateFile
 * has nine arguments and goes through xa_kcall9; the KEVENT is the kernel's 16-byte layout,
 * never the SDK's 4-byte placeholder; no floating point, no zero-initialised statics.
 */
#include <xecore/xboxkrnl.h>

#include "../core/platform.h"
#include "file_360.h"
#include "kernel_360.h"

/* Built for size: nothing here is on a hot path (a job a few times a minute, the disk
 * dominating its time), and -O2 nearly doubled the code. */
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

/* a store then a load, ordered (lwsync does not order that pair): see xf_quiesce */
#define FULL_BARRIER() __asm__ __volatile__("sync" ::: "memory")

#define XF_QUEUE 8            /* jobs; the contract promises 4 */
#define XF_STACK 0x8000       /* file system and XAM calls nest deep; as team / HUD threads */
#define XF_HW_THREAD 3
#define XF_WAIT_MS 1000
#define XF_QUIESCE_MS 1500    /* longest wait for a job in progress (a disk spin-up) */

/* ExCreateThread: system thread, affinity mask in the top byte (as xw_plugin.c) */
#define XF_THREAD_FLAGS (0x2 | (1u << XF_HW_THREAD) << 24)

/* NtCreateFile arguments */
#define ACC_READ 0x80100000u   /* GENERIC_READ | SYNCHRONIZE */
#define ACC_WRITE 0x40110000u  /* GENERIC_WRITE | DELETE | SYNCHRONIZE (DELETE: the rename) */
#define ACC_DELETE 0x00110000u /* DELETE | SYNCHRONIZE */
#define OPT_SYNC_FILE 0x60     /* FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE */
#define OPT_DELETE_ON_CLOSE 0x1000
#define ST_NAME_NOT_FOUND 0xC0000034u
#define ST_PATH_NOT_FOUND 0xC000003Au
#define ST_NO_SUCH_FILE 0xC000000Fu
#define ST_END_OF_FILE 0xC0000011u
#define INFO_RENAME 10         /* FileRenameInformation */

long xa_kcall9(const u32 args[9], void *fn); /* thunks_common.s */

/* the kernel's KEVENT (16 bytes; KeInitializeEvent writes all of them) */
struct xf_kevent {
	u8 type, absolute, size, inserted;
	volatile s32 signal_state;
	u32 wait_flink, wait_blink;
};
_Static_assert(sizeof(struct xf_kevent) == 16, "KEVENT on 17559 (KeInitializeEvent)");

/* FILE_RENAME_INFORMATION as the 360 kernel takes it (as the original Xbox's): the new name
 * is a counted ANSI object path, not inline UTF-16; 16 bytes in all. */
struct xf_rename {
	u32 replace;     /* see do_rename */
	u32 root;        /* 0: name is a full object path */
	ANSI_STRING name;
};
_Static_assert(sizeof(struct xf_rename) == 16, "FILE_RENAME_INFORMATION");

struct xf_job {
	void (*fn)(void *);
	void *arg;
	volatile u32 ready;
};

static struct xf_job g_q[XF_QUEUE];
static volatile u32 g_head;     /* producers: slots reserved */
static volatile u32 g_tail;     /* worker: slots taken */
static volatile u32 g_started;  /* the worker exists (xf_post accepts) */
static volatile u32 g_quiesced;
static volatile u32 g_busy;     /* the worker is inside a job (or about to check g_quiesced) */
static volatile u32 g_done;
static volatile u32 g_inplace;
static struct xf_kevent g_wake __attribute__((aligned(16)));

/* ---- the worker -------------------------------------------------------------------------- */

__asm__(NV64_THUNK_ASM(XfWorker, XfWorkerC));
u32 XfWorker(void *context);

/* one job, or false if there is none (or the module is quiesced) */
static bool run_one(void)
{
	u32 t = g_tail;
	struct xf_job *j = &g_q[t % XF_QUEUE];
	void (*fn)(void *);
	void *arg;

	g_busy = 1;
	FULL_BARRIER(); /* busy is visible before quiesced is read (xf_quiesce reads them the
			 * other way round) */
	if (g_quiesced || t == g_head || !j->ready) {
		g_busy = 0;
		return false;
	}
	BARRIER(); /* ready was seen: fn and arg are */
	fn = j->fn;
	arg = j->arg;
	j->ready = 0;
	BARRIER();
	g_tail = t + 1; /* the slot is free for a producer from here */
	fn(arg);
	g_done++;
	BARRIER();
	g_busy = 0;
	return true;
}

BODY u32 XfWorkerC(void *context)
{
	(void)context;
	for (;;) {
		int64_t timeout = -(int64_t)XF_WAIT_MS * 10000; /* relative, 100 ns units */

		/* Executive, KernelMode, not alertable */
		KeWaitForSingleObject((KEVENT *)&g_wake, 0, 0, 0, &timeout);
		while (run_one())
			;
	}
	return 0;
}

void xf_init(void)
{
	HANDLE thread = 0;
	u32 tid = 0;

	if (g_started)
		return;
	/* synchronization: a set while the worker is busy is kept for its next wait */
	KeInitializeEvent((KEVENT *)&g_wake, 1, 0);
	ExCreateThread(&thread, XF_STACK, &tid, 0, (void *)XfWorker, 0, XF_THREAD_FLAGS);
	BARRIER();
	g_started = thread ? 1 : 0;
	xa_log("file: worker %s on hardware thread %u\n", thread ? "started" : "NOT created",
	       XF_HW_THREAD);
}

bool xf_post(void (*fn)(void *), void *arg)
{
	u32 h;

	if (!g_started || g_quiesced || !fn)
		return false;
	do {
		h = g_head;
		if (h - g_tail >= XF_QUEUE)
			return false;
	} while (!__sync_bool_compare_and_swap(&g_head, h, h + 1));
	g_q[h % XF_QUEUE].fn = fn;
	g_q[h % XF_QUEUE].arg = arg;
	BARRIER(); /* fn and arg before ready */
	g_q[h % XF_QUEUE].ready = 1;
	KeSetEvent((KEVENT *)&g_wake, 0, 0);
	return true;
}

void xf_quiesce(void)
{
	u32 ms = 0;

	g_quiesced = 1;
	FULL_BARRIER();
	while (g_busy && ms < XF_QUIESCE_MS) {
		xa_sleep_ms(5);
		ms += 5;
	}
	/* the worker takes nothing more; drop what is queued (producers are refused already) */
	for (u32 i = 0; i < XF_QUEUE; i++)
		g_q[i].ready = 0;
	BARRIER();
	g_tail = g_head;
	xa_log("file: quiesced%s\n", g_busy ? " (a job is STILL running)" : "");
}

void xf_resume(void)
{
	BARRIER();
	g_quiesced = 0;
	xa_log("file: resumed\n");
}

u32 xf_jobs_done(void)
{
	return g_done;
}

u32 xf_inplace_writes(void)
{
	return g_inplace;
}

/* ---- the files (worker thread) ---------------------------------------------------------- */

static u32 slen(const char *s)
{
	u32 n = 0;

	while (s[n])
		n++;
	return n;
}

static long open_file(HANDLE *h, const char *path, u32 access, u32 share, u32 disposition,
		      u32 options)
{
	ANSI_STRING name = { (u16)slen(path), (u16)(slen(path) + 1), (char *)path };
	OBJECT_ATTRIBUTES attrs = { 0, &name, OBJ_CASE_INSENSITIVE };
	IO_STATUS_BLOCK io;
	/* handle, access, attributes, io, allocation size, FILE_ATTRIBUTE_NORMAL, share,
	 * disposition, create options (the ninth: xa_kcall9 puts it where the kernel reads it) */
	const u32 args[9] = { (u32)h, access, (u32)&attrs, (u32)&io, 0, 0x80, share, disposition,
			      options };

	*h = 0;
	return xa_kcall9(args, (void *)NtCreateFile);
}

static bool not_found(long st)
{
	return (u32)st == ST_NAME_NOT_FOUND || (u32)st == ST_PATH_NOT_FOUND ||
	       (u32)st == ST_NO_SUCH_FILE;
}

int xf_read(const char *path, void *buf, u32 max, u32 *len)
{
	HANDLE h;
	IO_STATUS_BLOCK io;
	u32 total = 0;
	u8 extra;
	long st;

	*len = 0;
	st = open_file(&h, path, ACC_READ, 1 /* FILE_SHARE_READ */, 1 /* FILE_OPEN */,
		       OPT_SYNC_FILE);
	if (st < 0)
		return not_found(st) ? XF_E_NOFILE : XF_E_IO;
	while (total < max) {
		int64_t off = total;

		io.Information = 0;
		st = NtReadFile(h, 0, 0, 0, &io, (u8 *)buf + total, max - total, &off);
		if (st < 0 || !io.Information)
			break;
		total += io.Information;
	}
	if (st >= 0 && total == max) {
		/* full: one byte more means the file is longer than the caller can take */
		int64_t off = total;

		io.Information = 0;
		st = NtReadFile(h, 0, 0, 0, &io, &extra, 1, &off);
		if (st >= 0 && io.Information) {
			NtClose(h);
			return XF_E_TOOBIG;
		}
		st = 0;
	}
	NtClose(h);
	if (st < 0 && (u32)st != ST_END_OF_FILE)
		return XF_E_IO;
	*len = total;
	return XF_OK;
}

int xf_read_at(const char *path, u32 offset, void *buf, u32 len, u32 *got)
{
	HANDLE h;
	IO_STATUS_BLOCK io;
	u32 total = 0;
	long st;

	*got = 0;
	st = open_file(&h, path, ACC_READ, 1 /* FILE_SHARE_READ */, 1 /* FILE_OPEN */,
		       OPT_SYNC_FILE);
	if (st < 0)
		return not_found(st) ? XF_E_NOFILE : XF_E_IO;
	while (total < len) {
		int64_t off = (int64_t)offset + total;

		io.Information = 0;
		st = NtReadFile(h, 0, 0, 0, &io, (u8 *)buf + total, len - total, &off);
		if (st < 0 || !io.Information)
			break;
		total += io.Information;
	}
	NtClose(h);
	if (st < 0 && (u32)st != ST_END_OF_FILE)
		return XF_E_IO;
	*got = total;
	return XF_OK;
}

/* all len bytes at offset 0 of an open file, then to the disk */
static bool write_all(HANDLE h, const void *buf, u32 len)
{
	IO_STATUS_BLOCK io;
	int64_t off = 0;
	long st;

	io.Information = 0;
	st = NtWriteFile(h, 0, 0, 0, &io, (void *)buf, len, &off);
	if (st < 0 || io.Information != len)
		return false;
	return NtFlushBuffersFile(h, &io) >= 0;
}

static long do_rename(HANDLE h, const char *to)
{
	IO_STATUS_BLOCK io;
	/* ReplaceIfExists is a BOOLEAN in the kernel's struct (byte 0) on the original Xbox; a
	 * reader of it as a 32-bit word also sees non-zero */
	struct xf_rename r = { 0x01000001u, 0, { (u16)slen(to), (u16)(slen(to) + 1), (char *)to } };

	return NtSetInformationFile(h, &io, &r, sizeof(r), INFO_RENAME);
}

/* deletes path (FILE_DELETE_ON_CLOSE: how this kernel deleted a file by accident once, see
 * docs/INTERNALS.md); missing counts as deleted */
static bool delete_file(const char *path)
{
	HANDLE h;
	long st = open_file(&h, path, ACC_DELETE, 0, 1 /* FILE_OPEN */,
			    OPT_SYNC_FILE | OPT_DELETE_ON_CLOSE);

	if (st < 0)
		return not_found(st);
	NtClose(h);
	return true;
}

static int write_in_place(const char *path, const void *buf, u32 len)
{
	HANDLE h;
	bool ok;

	if (open_file(&h, path, ACC_WRITE, 0, 5 /* FILE_OVERWRITE_IF */, OPT_SYNC_FILE) < 0)
		return XF_E_IO;
	ok = write_all(h, buf, len);
	NtClose(h);
	return ok ? XF_OK : XF_E_IO;
}

int xf_write(const char *path, const void *buf, u32 len)
{
	char tmp[XF_PATH_MAX];
	u32 n = slen(path), k = slen(XF_NEW_SUFFIX);
	HANDLE h;
	bool ok;
	long st;

	if (n + k + 1 > sizeof(tmp))
		return XF_E_IO;
	memcpy(tmp, path, n);
	memcpy(tmp + n, XF_NEW_SUFFIX, k + 1);

	/* 1. the new bytes, whole and on the disk, under the temporary name */
	if (open_file(&h, tmp, ACC_WRITE, 0, 5 /* FILE_OVERWRITE_IF */, OPT_SYNC_FILE) < 0)
		return XF_E_IO;
	ok = write_all(h, buf, len);
	/* 2. over the real name: one directory update, so a cut leaves one file or the other */
	st = ok ? do_rename(h, path) : -1;
	NtClose(h);
	if (!ok)
		return XF_E_IO; /* the real file was not touched */
	if (st >= 0)
		return XF_OK;
	/* 3. a file system that will not replace on rename: delete, then rename (between the
	 * two only the temporary copy exists, and it is whole) */
	if (delete_file(path) &&
	    open_file(&h, tmp, ACC_DELETE, 0, 1 /* FILE_OPEN */, OPT_SYNC_FILE) >= 0) {
		st = do_rename(h, path);
		NtClose(h);
		if (st >= 0)
			return XF_OK;
	}
	/* 4. no rename at all: in place (the temporary copy stays whole beside it) */
	g_inplace++;
	return write_in_place(path, buf, len);
}

#pragma clang attribute pop
