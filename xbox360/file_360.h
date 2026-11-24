// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Whole-file reads and writes on the console's hard disk, for the settings file and the
 * preset file, and ranged reads for the surround demo clip (file_360.c).
 *
 * Why a worker thread: a hard-disk access can stall for tens of milliseconds (spin-up, a busy
 * title). On the driver thread that would starve the earpiece (gaps) and the controllers. So
 * xf_read / xf_write block, and run only on the file worker thread: a caller hands work to it
 * with xf_post. The worker is a low-priority system thread, entered through a thunk like every
 * kernel callback (docs/INTERNALS.md). NtCreateFile takes nine arguments: call it through
 * xa_kcall9 (thunks_common.s), never directly.
 *
 * Paths are NT object paths; XF_DIR is Hdd:\Plugins, where xw.xex itself lives. xw.xex is a
 * system-process plugin, not a title: "Hdd:" is a link a title's launcher (or a dashboard)
 * sets up, and nothing guarantees it in the system process, so the device path is used, as
 * xa.xex's firmware loader does (read from a system plugin: works on the console). Writing
 * from the system process is the same NtCreateFile / NtWriteFile the test titles use
 * (echo.c, voicetest.c); not yet seen on the console from a plugin.
 */
#ifndef FILE_360_H
#define FILE_360_H

#include "../core/compat.h"

#define XF_DIR "\\Device\\Harddisk0\\Partition1\\Plugins\\"
/* xf_write writes path + this first, then renames it over path, so a power cut leaves the
 * old file or the new one whole. Where the rename is refused it writes path in place and
 * this copy stays as complete as the new file: a reader that finds path damaged may read
 * this one instead. */
#define XF_NEW_SUFFIX ".new"
#define XF_PATH_MAX 96 /* path + XF_NEW_SUFFIX + NUL; longer: XF_E_IO */

/* results: 0, or one of these (negative) */
enum { XF_OK = 0, XF_E_NOFILE = -1, XF_E_IO = -2, XF_E_TOOBIG = -3, XF_E_NOWORKER = -4 };

/* driver thread, once at start-up, before xs_init and xq_init: creates the worker thread */
void xf_init(void);
/* Hot reload, driver thread: waits (bounded) for the job in progress, then takes no more;
 * jobs still queued are dropped (their callers must not depend on them running).
 * xf_resume: a refused reload; jobs are taken again. */
void xf_quiesce(void);
void xf_resume(void);

/* worker thread only (inside a function given to xf_post). xf_read: the whole file into buf
 * (at most max bytes; a longer file is XF_E_TOOBIG), *len = its size. xf_write: replaces the
 * file with these len bytes. */
int xf_read(const char *path, void *buf, u32 max, u32 *len);
int xf_write(const char *path, const void *buf, u32 len);
/* worker thread only: len bytes of the file from byte `offset` into buf (one open, the reads,
 * one close: for a file too big to hold, read a piece at a time). *got = the bytes read, fewer
 * than len only where the file ends (XF_OK all the same). */
int xf_read_at(const char *path, u32 offset, void *buf, u32 len, u32 *got);

/* any thread: run fn(arg) on the worker thread, in the order posted. The queue holds at
 * least 4 jobs (at boot the settings load and the preset load are both posted from init);
 * false = full, or no worker: nothing was queued, try again later. fn must not block for
 * anything but the disk, and owns arg until it returns. */
bool xf_post(void (*fn)(void *), void *arg);

/* jobs the worker has finished since start-up (a status block shows it is alive); writes
 * where the rename was refused and the file was written in place (see XF_NEW_SUFFIX) */
u32 xf_jobs_done(void);
u32 xf_inplace_writes(void);

#endif
