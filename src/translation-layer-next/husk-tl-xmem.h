/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Executable memory for guest code.
 *
 * Every byte of arm64 code an Android app brings -- its libraries, and the small
 * stubs the loader writes -- has to live in memory the CPU will execute, and on
 * the phone there is exactly one source of that: the region StikDebug grants once,
 * at launch, as two views of the same pages. Code is written through the
 * writable view and run through the executable one.
 *
 * Which of the two views carries the JIT authorisation (MAP_JIT, or the debugger's
 * blessing) decides whether a GUEST can execute the memory it wrote for itself. iOS
 * authorises execute per mapping ENTRY, so an alias never inherits it: a guest that
 * keeps a code pointer in the WRITABLE view -- an inline-hook arena, which the
 * loader's adrp retarget hands it in writable-view form -- cannot execute it when
 * the writable view is a vm_remap. So the layer puts the authorisation on the view
 * the guest addresses (the guest-facing view) and makes the loader's code view the
 * vm_remap (tl_xmem_guest_view_is_jit); where the kernel will not grant that, it
 * falls back to the old order and tl_xmem_allow_exec says so honestly.
 *
 * On a Mac there is no such restriction, but the loader is written against the
 * same two-view shape so the code that relocates, patches and addresses guest
 * images is the code that runs on the phone. The Mac gets its two views from
 * mach_vm_remap of ordinary memory.
 *
 * Whether "an alias never inherits it" is the whole truth is NOT assumed: on the phone the
 * process is CS_DEBUGGED (its own log says so), and a debugged process is allowed to make
 * writable memory executable, yet the read-back of the writable view did not name execute after
 * a successful vm_protect. tl_xmem_allow_exec therefore decides nothing by read-back: it
 * fetch-tests the page and reports what the kernel actually said
 * (exec-on-device-csdebugged.md).
 *
 * Allocation is a bump pointer: nothing is ever returned. The phone's region is
 * one-shot, so a freed hole could not be refilled by asking for another.
 */
#ifndef HUSK_TL_XMEM_H
#define HUSK_TL_XMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TL_XMEM_PAGE 16384u

/*
 * Find the region: the phone's StikDebug region, or on a Mac one made here of
 * `host_bytes`. Safe to call repeatedly; the region is found once.
 * Returns false when no executable memory can be had, and says why in `err`.
 */
bool tl_xmem_open(size_t host_bytes, char *err, size_t errlen);

/*
 * Take `bytes` (rounded up to whole pages) from the region. `*rx` is where the
 * code will run and `*rw` is the same memory for writing; rw - rx is the same
 * constant for every allocation. Returns false when the region is exhausted.
 */
bool tl_xmem_alloc(size_t bytes, uint8_t **rx, uint8_t **rw);

/* rw - rx for the region, valid after tl_xmem_open succeeds. */
ptrdiff_t tl_xmem_delta(void);

/* Whether an address is inside the region, in either view. */
bool tl_xmem_contains(const void *p);

/* Whether an address is in the executable view specifically. */
bool tl_xmem_is_rx(const void *p);

/*
 * Make the pages at the WRITABLE-view address `p` executable in that same view (the mirror of
 * tl_xmem_is_rx): the executable view of those pages already runs, but a guest that keeps a
 * code pointer in memory it wrote -- a JIT's trampoline arena, which the loader's adrp rewrite
 * hands it in writable-view form -- holds an address in the writable view and calls it. Adds
 * execute to that view and flushes the instruction cache.
 *
 * The verdict is the GUARDED FETCH, not the return code and not a read-back. The fetch writes a
 * movz/ret through that view and calls it there, under a signal guard, and reports whether it ran.
 * A read-back (vm_region_recurse_64) is taken too, and printed raw beside the fetch -- its own
 * return code and the entry it named (range, protection, max_protection, depth, tag) -- but it does
 * NOT decide: on the phone vm_protect returned KERN_SUCCESS and the read-back of the same page still
 * did not name execute, which is self-contradictory for a successful protect, so the read-back can
 * be wrong (a failed probe, or a different entry/level than the one vm_protect changed) while the
 * page would in fact fetch. Returns false -- and says so once, naming which check failed and what
 * the read-back saw -- otherwise, and sets the guest errno to EACCES so a JIT takes its interpreter
 * fallback instead of jumping into a page that will not fetch. The caller must not report success then.
 * TL_XMEM_NO_FETCH_VERDICT=1 restores the old behaviour (verdict = read-back AND fetch).
 */
bool tl_xmem_allow_exec(void *p, size_t n);

/*
 * Whether the guest-facing view -- the address the loader's adrp retarget hands the guest for
 * writable content, and the one a JIT calls its own arena by -- is the mapping that carries the
 * JIT authorisation (Fix A), i.e. the address the guest BOTH writes and executes. When true, the
 * address the guest already holds genuinely becomes executable when it asks with PROT_EXEC, and
 * tl_xmem_allow_exec's read-back plus guarded fetch succeed; when false, the layer is running on a
 * two-address scheme (the authorisation on the code view, the writable view a vm_remap) and a
 * writable-view arena cannot be made to fetch -- tl_xmem_allow_exec then refuses it honestly with
 * EACCES. Two phone crashes are why the layer now prefers an arena of its own over the debugger's
 * region (open_platform, grant_own_arena): a guest that computes a code pointer off the writable
 * base and branches (PvZ's A64HookFunction) cannot use the two-address scheme, and the debugger's
 * region is reserved rather than fully backed (robot gf's bus error on a page whose entry named
 * execute). Fixed once at open; valid after tl_xmem_open succeeds.
 */
bool tl_xmem_guest_view_is_jit(void);

/*
 * True only in the W^X FALLBACK: the guest was handed the EXECUTABLE view but that view could not
 * be HELD writable-and-executable in place (iOS enforces W^X -- the open-time guarded store faulted),
 * so a store into it is serviced one page at a time by the codewrite guard
 * (tl_xmem_page_set_writable / tl_xmem_page_set_executable). False wherever the guest has a genuinely
 * RWX address of its own. Fixed once at open; valid after tl_xmem_open succeeds.
 *
 * When true, tl_xmem_guest_view_is_jit() is false and the guest-facing view (g_x.rw, the address the
 * loader hands the guest and the one tl_xmem_allow_exec and the reports speak about) IS the
 * executable view, as RX -- it is the view whose generated code provably ran at boot, not the
 * writable vm_remap alias iOS will not fetch. The layer still WRITES through the debugger's writable
 * twin (the vm_remap): tl_xmem_alloc hands it back as *rw and tl_xmem_delta() is that twin minus the
 * executable view, so the loader's bulk emission, the codewrite redirect and the kernel-write
 * redirect all keep working without faulting a library image in page by page. tl_xmem_alloc's *rw
 * therefore stays a genuinely writable address even though the guest's own arena address (g_x.rw) is
 * read-only-to-the-kernel.
 */
bool tl_xmem_wx_fallback(void);

/*
 * The two halves of the W^X fallback's per-page bracket, at the mmap level (vm_protect) -- the level
 * iOS honours, unlike the per-thread pthread_jit_write_protect, which only governs MAP_JIT. Flip the
 * page(s) covering [p, p+n) writable for a store, then executable again for the fetch. Return false
 * if the kernel refused, in which case the caller must NOT attempt the store (a second fault inside
 * the signal handler would end the process). No-ops returning false off the phone.
 */
bool tl_xmem_page_set_writable(void *p, size_t n);
bool tl_xmem_page_set_executable(void *p, size_t n);

/*
 * Put the calling thread in write mode (emit_begin) or back in execute mode (emit_end) around the
 * layer's OWN emission into the region. Only meaningful when tl_xmem_guest_view_is_jit(): the
 * guest-facing view is then a MAP_JIT mapping, which on a device that enforces W^X is writable or
 * executable per thread, never both at once. A no-op where no write protection is enforced (and on
 * a host with no such mapping), so it is safe to call unconditionally. Never unmaps or joins
 * anything; purely the APRR/thread state.
 */
void tl_xmem_emit_begin(void);
void tl_xmem_emit_end(void);

/* Make bytes written through the RW view visible to instruction fetch. */
void tl_xmem_flush(const void *rx, size_t bytes);

size_t tl_xmem_used(void);
size_t tl_xmem_size(void);

/* Whether p is within dist bytes of the executable region (a JIT asking for code memory near its own code). */
bool tl_xmem_near(const void *p, size_t dist);

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_XMEM_H */
