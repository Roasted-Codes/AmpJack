// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Small kernel-side helpers shared by the plugin's files (docs/INTERNALS.md has the rules
 * behind them). Add to this file instead of copying a helper into another one.
 *
 * Files that still carry their own copy and should switch to this one when their owner next
 * edits them: BARRIER in detour.c, platform_360.c, xinput_360.c, game_360.c, hud_360.c,
 * team_360.c, xa_plugin.c; HOOK / BODY in game_360.c, hud_360.c, xinput_360.c, team_360.c,
 * xa_plugin.c; struct sig and its check loop in game_360.c (twice) and hud_360.c; the NV64
 * thunk in game_360.c (XG_*) and team_360.c (.irp form, same instructions); hud_360.c's
 * swap stub can use NV64_SAVE / NV64_LOAD / NV64_REGS for its r14-r31 part.
 */
#ifndef KERNEL_360_H
#define KERNEL_360_H

#include "../core/compat.h"

/* Orders this CPU's earlier stores before its later ones as other processors see them (a
 * packet's data before its status word, a ring entry before its index). Without it Usbd's
 * DPC or a title thread on another hardware thread can read a half-written entry. Host tests
 * (not PowerPC) get the compiler's full barrier. */
#ifdef __PPC__
#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#else
#define BARRIER() __sync_synchronize()
#endif

/* The C body of a kernel entry point (a hook, a DPC, a thread start), reached only from its
 * NV64 thunk: kept out of line and never dropped, so the thunk's `bl` always has a target.
 * HOOK and BODY are the same; both names are in use. */
#define HOOK __attribute__((used, noinline))
#define BODY __attribute__((used, noinline))

/* ---- kernel code checked before it is used ---------------------------------------------------
 * A run of instruction words that must match the 17559 dump word for word before the plugin
 * patches, calls or relies on that code. Anything else (another kernel, another patch) is
 * refused rather than guessed at. */
struct sig {
	u32 addr, n;
	const u32 *w;
};

/* the address of the first word of s that differs from memory, or 0 if all n words match */
static inline u32 sig_mismatch(const struct sig *s)
{
	const volatile u32 *c = (const volatile u32 *)s->addr;

	for (u32 k = 0; k < s->n; k++)
		if (c[k] != s->w[k])
			return s->addr + 4 * k;
	return 0;
}

/* ---- the NV64 thunk, for a file that defines its own kernel entry ------------------------------
 * The kernel calls with 64-bit values live in r14-r31; clang's ppc32 ABI saves only their low
 * halves, so a C body called directly would hand the kernel back truncated registers (a hang
 * some time later). The thunk saves all 64 bits around the body: frame 176, r14 at 16(1) ...
 * r31 at 152(1), LR at 168(1), exactly as thunks_common.s's NV64_THUNK macro, which the
 * entries listed in thunks*.s use. Use: __asm__(NV64_THUNK_ASM(XxHook, XxHookC)); */
#define NV64_SAVE(r) "    std " #r ", " #r "*8-96(1)\n"
#define NV64_LOAD(r) "    ld " #r ", " #r "*8-96(1)\n"
#define NV64_REGS(M) M(14) M(15) M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23) M(24) M(25) \
	M(26) M(27) M(28) M(29) M(30) M(31)
#define NV64_THUNK_ASM(name, target)                             \
	"    .text\n"                                             \
	"    .globl " #name "\n"                                  \
	"    .p2align 2\n"                                        \
	#name ":\n"                                               \
	"    mflr 0\n"                                            \
	"    stwu 1, -176(1)\n"                                   \
	"    stw 0, 168(1)\n"                                     \
	NV64_REGS(NV64_SAVE)                                      \
	"    bl " #target "\n"                                    \
	NV64_REGS(NV64_LOAD)                                      \
	"    lwz 0, 168(1)\n"                                     \
	"    mtlr 0\n"                                            \
	"    addi 1, 1, 176\n"                                    \
	"    blr\n"

#endif
