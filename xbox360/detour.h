/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Function hooks for kernel and XAM code (PowerPC). The first four instructions of a
 * function become a far jump to the hook; a trampoline holds them, relocated, and
 * jumps back. The encoding follows iMoD1998's Detours.h (as used by hiddriver360).
 *
 * The hook is entered with the caller's arguments and return address, so it must be
 * an NV64 thunk (thunks.s). It calls the original through the trampoline.
 *
 * Hot reload (docs/HOT-RELOAD.md). The jump written over the function is indirect, through
 * a "slot" word in the image that patched it:
 *
 *     lis r12, slot@ha;  lwz r12, slot@l(r12);  mtctr r12;  bctr
 *
 * so a later build takes a hook over with one aligned 32-bit store into that slot. The code
 * is patched once per function and console boot and never rewritten at a reload: no thread
 * can be caught half-way through a patch. The image that patched it, with its slot and its
 * trampoline, must stay loaded for good.
 *
 * Every hook this module knows, its own and those inherited from earlier builds, is listed
 * in detour_registry; the next build is given its address (handover.h) and calls
 * detour_inherit before it installs anything. The registry's layout is shared between
 * builds: append only, and bump XW_HO_VERSION (handover.h) if an existing field changes.
 */
#pragma once

#include "../core/compat.h"

/* 4 words for the hook; per instruction at most 6 words relocated, plus 6 to go back */
#define DETOUR_TRAMPOLINE_WORDS 30

struct detour {
	const char *name;
	u32 *target;
	u32 original[4];
	u32 *trampoline;    /* call this to run the original */
	volatile u32 *slot; /* the word the patched function jumps through */
	u32 prev_hook;      /* adopted: what the slot held before (the previous build's hook) */
	int adopted;        /* the patch is an earlier build's: only the slot was written */
};

#define DETOUR_REG_MAX 16

struct detour_entry {
	u32 target;      /* the patched function */
	u32 slot;        /* address of the word its jump loads (the current hook); 0 = unpatched */
	u32 trampoline;  /* runs the original; in the image that patched the function */
	u32 original[4]; /* the function's first four instructions before the patch */
};

struct detour_registry {
	u32 count;
	struct detour_entry e[DETOUR_REG_MAX];
};

extern struct detour_registry detour_registry;

/* copies a previous build's entries whose patch is still in place (the function still jumps
 * through the entry's slot); the number taken. Call it before any detour_install. */
int detour_inherit(const struct detour_registry *from);

/* whether `target` is hooked by a patch this module knows (its own or an inherited one):
 * its first four instructions are then the slot jump, not the function's own code */
bool detour_known(const void *target);

/* hooks target -> hook; 0, or a negative error with nothing patched. A function that an
 * inherited entry covers is adopted: its slot is pointed at `hook`, no code is patched. */
int detour_install(struct detour *d, const char *name, void *target, const void *hook);

/* undoes detour_install. An adopted hook gets its previous owner back (one store). A hook
 * this module patched in gets the original instructions back: only safe while nothing can
 * be calling the function, and never done at a hot reload. */
void detour_remove(struct detour *d);
