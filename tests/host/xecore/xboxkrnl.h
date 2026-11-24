/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host tests only (cc -Itests/host): stands in for the toolchain's <xecore/xboxkrnl.h> so a
 * plugin file that includes voice_priv.h compiles on the Mac. Declares nothing: a test that
 * needs a kernel call fakes it itself. */
#ifndef HOST_XBOXKRNL_H
#define HOST_XBOXKRNL_H
#endif
