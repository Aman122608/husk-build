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
 * The verdict of the guarded write-then-execute, as a value the APP can read.
 *
 * This is the only honest basis for a "JIT is on" indicator: the app's own `prewarmed` flag is
 * something the app SET, and a flag can be true while the pages cannot be fetched (measured --
 * husk-20261015-085540: the app detaching the debugger revoked the region's per-entry grant, every
 * entry then read prot 1 max 3, and the guarded fetch FAULTED -- while the UI still said "JIT is
 * on"). The layer runs the guarded write-then-execute when the arena opens, aimed at the address
 * the guest both writes and executes, and its outcome is kept here so the UI reports a measurement
 * rather than an assertion. TL_XMEM_EXEC_UNMEASURED (0) is the truth before a game has opened an
 * arena: the UI must say it does not know yet, not that it works.
 */
enum { TL_XMEM_EXEC_UNMEASURED = 0, TL_XMEM_EXEC_RAN = 1, TL_XMEM_EXEC_FAULTED = 2 };

/* The last guarded write-then-execute's verdict: a TL_XMEM_EXEC_* value. */
int tl_xmem_exec_verdict(void);

/* Why it did not RAN, as one line for the UI, or NULL when it RAN (or has not run yet). */
const char *tl_xmem_exec_detail(void);

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
 * True when the guest's code arena is the EXECUTABLE view of the debugger's region, held RX: a guest
 * store into it faults and is carried through the writable twin by the codewrite guard, and NO page of
 * the region is ever asked for WRITE. Fixed once at open; valid after tl_xmem_open succeeds.
 *
 * This flag used to mean "the executable view could not be HELD writable-and-executable in place",
 * and the code that tried to hold it -- vm_protect(RWX) on the whole 512 MiB view -- is exactly what
 * destroyed the debugger's execute grant. That ask is gone: the view is never written and never
 * protected (see adopt_exec_view in husk-tl-xmem.c for the two-run control that proves it). When true,
 * tl_xmem_guest_view_is_jit() is false and the guest-facing view (g_x.rw) IS the executable view, as
 * RX -- the view whose generated code provably ran at boot, and the same view b_mmap hands a guest
 * that asks for code memory. The layer still WRITES through the debugger's writable twin:
 * tl_xmem_alloc hands it back as *rw and tl_xmem_delta() is that twin minus the executable view, so
 * the loader's bulk emission, the codewrite redirect, the kernel-write redirect and every guest store
 * the guard carries all land through a genuinely writable mapping.
 */
bool tl_xmem_wx_fallback(void);

/*
 * One guarded write-then-execute, naming what was happening when it ran -- the instrument that ends
 * the guessing about which call costs the execute grant. Call it BEFORE the layer's first
 * protection-changing call and after each one: the pair of lines names the moment a fetch stops
 * running (so the call between them is the one that cost it), or proves that none of our calls did
 * (both RAN, so the loss is outside the layer -- the debugger's presence going away).
 *
 * `write_at`/`fetch_at` are the two views of one page, or one view named twice when the question is
 * whether a single view both takes a write and executes. It writes a movz/ret through `write_at`,
 * flushes, calls it at `fetch_at` under a signal guard, and restores the bytes it found. True if the
 * call ran and returned the marker. Never changes any mapping's protection.
 */
bool tl_xmem_exec_gate(const char *after, void *write_at, void *fetch_at);

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

/*
 * The base of the EXECUTABLE (guest-code) view of the region -- the address tl_xmem_alloc carves
 * its bump span from, so (addr - tl_xmem_region_base()) and tl_xmem_used() are in the same units
 * and directly comparable. Read-only; no side effects, safe on the fatal path.
 */
const void *tl_xmem_region_base(void);

/* Whether p is within dist bytes of the executable region (a JIT asking for code memory near its own code). */
bool tl_xmem_near(const void *p, size_t dist);

/* ---------------------------------------------------- the verdict, made to survive a crash */

/*
 * Where the app wants the last arena verdict written: a path inside its container, set before a game
 * starts. The layer writes the record there at MEASUREMENT time (inside tl_xmem_open, from
 * xmem_note_exec), so the verdict -- and, later, why the run died -- survives a fault that takes the
 * process, and the app's Settings "Guest arena" row reads it on the next launch. NULL or empty turns
 * persistence off; the layer never guesses at a sandbox layout. Safe to call any time.
 */
void tl_xmem_set_verdict_path(const char *path);

/*
 * Whether a fatal fault is the GUEST failing in the region it runs from: the FAULT ADDRESS lies in
 * the arena region -- which is where ALL of the guest's code and its JIT arena live (the loader emits
 * the app's libraries and its JIT arena into the same region, and it owns every address in it). It is
 * deliberately the address, not the program counter: a fetch reports pc == fault address, but a DATA
 * access (a load or store) reports the pc of the instruction, which is in the guest's own library
 * OUTSIDE the region -- robot gf's second crash (fault address 0x122a51b90, pc in
 * Documents/Data/App+0x4c5e8e4) is that shape, and the pc-equality test let it through as a host
 * crash. The crash reporter (husk-tl-unity-app.c, on_fatal) treats a true here as a GUEST failure:
 * the run is ended, Husk stays, and the reason is displayed and persisted, the same rule that makes a
 * guest exit() unwind the app rather than the process. A fault whose address is OUTSIDE the region is
 * still a host crash and keeps the old behaviour.
 */
bool tl_xmem_arena_fetch_fault(uintptr_t pc, uintptr_t addr);

/*
 * Record why the last run died, in the same container file as the verdict (see tl_xmem_set_verdict_path).
 * `reason` is one line; `pc` is the address the guest branched to. Called from the crash reporter's
 * guest-fault path. A no-op when no path was set, and on a host with no arena at all.
 */
void tl_xmem_note_guest_fault(const char *reason, uintptr_t pc);

/*
 * THE FATAL-TIME MEASUREMENT: take the SAME three verdicts the open-time probe takes -- a guarded READ
 * through the executable view, a guarded READ through the writable view, and a guarded CALL of a
 * movz/ret stub -- on the page the guest actually faulted on, and print them on ONE line, labelled as
 * the FATAL-TIME measurement, together with whether they MATCH the open-time ones seen at arena open.
 *
 * It runs ONCE per fault, on a NORMAL THREAD, and NEVER inside the crash reporter's signal handler:
 * installing and longjmping out of a sigsetjmp guard for the signal already being handled is
 * re-entrancy risk. The handler only records the fault (tl_xmem_note_guest_fault); this is called
 * later on the app-fault/session-close path (husk_tl_session_close), after the handler returned and
 * only the faulting thread ended. Nothing is unmapped by the layer, so the arena pages are still there.
 * A no-op unless the arena is open and an unreported guest fault was recorded; a no-op on a host with
 * no arena at all. No vm_protect is issued, the executable view is never written, nothing is unmapped,
 * no thread is ended, and a fault in its guards is caught, not fatal.
 */
void tl_xmem_report_fault_time(void);

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_XMEM_H */
