/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Executable memory for guest code.
 *
 * Every arm64 instruction an Android app brings lives in memory the CPU will execute, and the phone has one source of
 * it: the region StikDebug grants at launch as two views of the same pages. Code is written through the writable view
 * and run through the executable view. On a Mac the same two-view shape is made from mach_vm_remap of ordinary memory,
 * so the loader relocates, patches and addresses guest images on the code that runs on the phone.
 *
 * iOS authorises execute per mapping ENTRY, and an alias never inherits it, so WHICH view carries the JIT authorisation
 * decides whether a guest can execute memory it wrote for itself. The layer prefers the view the guest ADDRESSES (the
 * guest-facing view) and makes the loader's code view the vm_remap (tl_xmem_guest_view_is_jit); where the kernel will
 * not grant that, it falls back and tl_xmem_allow_exec says so honestly. Neither is ASSUMED: the guarded
 * write-then-execute fetch-tests the page and reports what the kernel actually said (xmem-arena-and-wx.md).
 *
 * Allocation is a bump pointer: nothing is ever returned (the phone's region is one-shot, so a freed hole could not be
 * refilled by asking for another).
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

/* Find the region (the phone's StikDebug region, or on a Mac one made here of `host_bytes`). Safe to call repeatedly;
 * the region is found once. False, saying why in `err`, when no executable memory can be had. */
bool tl_xmem_open(size_t host_bytes, char *err, size_t errlen);

/* Take `bytes` (rounded up to whole pages) from the region. `*rx` is where the code runs, `*rw` is the same memory for
 * writing; rw - rx is a constant for every allocation. False when the region is exhausted. */
bool tl_xmem_alloc(size_t bytes, uint8_t **rx, uint8_t **rw);

/* rw - rx for the region, valid after tl_xmem_open succeeds. */
ptrdiff_t tl_xmem_delta(void);

/* Whether an address is inside the region, in either view. */
bool tl_xmem_contains(const void *p);

/* Whether an address is in the executable view specifically. */
bool tl_xmem_is_rx(const void *p);

/*
 * The verdict of the guarded write-then-execute, as a value the APP can read. This is the only honest basis for a
 * "JIT is on" indicator: the app's own `prewarmed` flag can be true while the pages cannot be fetched
 * (jit-grant-and-detach.md). The layer runs the guarded write-then-execute when the arena opens, aimed at the address
 * the guest both writes and executes, and keeps the outcome here. TL_XMEM_EXEC_UNMEASURED (0) is the truth before a
 * game has opened an arena: the UI must say it does not know yet, not that it works.
 */
enum { TL_XMEM_EXEC_UNMEASURED = 0, TL_XMEM_EXEC_RAN = 1, TL_XMEM_EXEC_FAULTED = 2 };

/* The last guarded write-then-execute's verdict: a TL_XMEM_EXEC_* value. */
int tl_xmem_exec_verdict(void);

/* Why it did not RAN, as one line for the UI, or NULL when it RAN (or has not run yet). */
const char *tl_xmem_exec_detail(void);

/*
 * Make the pages at the WRITABLE-view address `p` executable in that same view (the mirror of tl_xmem_is_rx): a guest
 * that keeps a code pointer in memory it wrote (a JIT's trampoline arena, handed to it in writable-view form by the
 * loader's adrp rewrite) holds an address in that view and calls it. Adds execute and flushes the instruction cache.
 *
 * The verdict is the GUARDED FETCH, not the return code and not a read-back. A read-back (vm_region_recurse_64) is
 * taken and printed raw beside the fetch, but it does NOT decide: on the phone vm_protect returned KERN_SUCCESS and
 * the read-back still did not name execute, and the read-back can be wrong (a failed probe, or a different
 * entry/level) while the page would in fact fetch. Returns false, saying once which check failed, and sets the guest
 * errno to EACCES so a JIT takes its interpreter fallback. The caller must not report success then.
 * TL_XMEM_NO_FETCH_VERDICT=1 restores the old behaviour (verdict = read-back AND fetch).
 */
bool tl_xmem_allow_exec(void *p, size_t n);

/*
 * Whether the guest-facing view -- the address the loader's adrp retarget hands the guest for writable content, and the
 * one a JIT calls its own arena by -- is the mapping that carries the JIT authorisation (Fix A), i.e. the address the
 * guest BOTH writes and executes. When true, the address the guest holds genuinely becomes executable when it asks with
 * PROT_EXEC. When false, the layer runs a two-address scheme (authorisation on the code view, the writable view a
 * vm_remap) and a writable-view arena cannot be made to fetch -- tl_xmem_allow_exec refuses it honestly with EACCES.
 * The layer prefers an arena of its own over the debugger's region for two measured reasons (open_platform,
 * grant_own_arena; xmem-arena-and-wx.md). Fixed once at open; valid after tl_xmem_open succeeds.
 */
bool tl_xmem_guest_view_is_jit(void);

/*
 * True when the guest's code arena is the EXECUTABLE view of the debugger's region, held RX: a guest store into it
 * faults and is carried through the writable twin by the codewrite guard, and NO page of the region is ever asked for
 * WRITE. Fixed once at open; valid after tl_xmem_open succeeds.
 *
 * When true, tl_xmem_guest_view_is_jit() is false and the guest-facing view (g_x.rw) IS the executable view, as RX. The
 * layer still WRITES through the debugger's writable twin: tl_xmem_alloc hands it back as *rw and tl_xmem_delta() is
 * that twin minus the executable view, so the loader's bulk emission, the codewrite redirect, the kernel-write redirect
 * and every guest store the guard carries land through a genuinely writable mapping.
 */
bool tl_xmem_wx_fallback(void);

/*
 * One guarded write-then-execute, naming what was happening when it ran -- the instrument that ends the guessing about
 * which call costs the execute grant. Call it BEFORE the layer's first protection-changing call and after each one: the
 * pair of lines names the moment a fetch stops running (so the call between them cost it), or proves that none of our
 * calls did (both RAN -> the loss is outside the layer -- the debugger's presence going away). `write_at`/`fetch_at` are
 * the two views of one page, or one view named twice to ask whether a single view both takes a write and executes. It
 * restores the bytes it found and never changes any mapping's protection.
 */
bool tl_xmem_exec_gate(const char *after, void *write_at, void *fetch_at);

/* Put the calling thread in write mode (emit_begin) or execute mode (emit_end) around the layer's OWN emission into the
 * region. Only meaningful when tl_xmem_guest_view_is_jit(): the guest-facing view is then a MAP_JIT mapping, writable or
 * executable per thread where W^X is enforced. A no-op where no write protection is enforced, so it is safe to call
 * unconditionally. Never unmaps or joins anything. */
void tl_xmem_emit_begin(void);
void tl_xmem_emit_end(void);

/* Make bytes written through the RW view visible to instruction fetch. */
void tl_xmem_flush(const void *rx, size_t bytes);
size_t tl_xmem_used(void);
size_t tl_xmem_size(void);

/* The arena's per-allocation backing work so far: allocations handed out and 4 KiB pages backed by a volatile store
 * through the writable twin (tl_xmem_alloc). Read-only; safe any time, and used by the perf summary. */
void tl_xmem_backing_stats(size_t *allocs, size_t *pages);

/* The base of the EXECUTABLE (guest-code) view of the region, so (addr - tl_xmem_region_base()) and tl_xmem_used() are
 * in the same units and directly comparable. Read-only; safe on the fatal path. */
const void *tl_xmem_region_base(void);

/* Whether p is within dist bytes of the executable region (a JIT asking for code memory near its own code). */
bool tl_xmem_near(const void *p, size_t dist);

/* ---------------------------------------------------- the verdict, made to survive a crash */

/* Where the app wants the last arena verdict written: a path inside its container, set before a game starts. The layer
 * writes the record there at MEASUREMENT time, so the verdict -- and later why the run died -- survives a fault that
 * takes the process. NULL or empty turns persistence off; the layer never guesses at a sandbox layout. Safe any time. */
void tl_xmem_set_verdict_path(const char *path);

/*
 * Whether a fatal fault is the GUEST failing in the region it runs from: the FAULT ADDRESS lies in the arena region,
 * where ALL of the guest's code and its JIT arena live. It is deliberately the address, not the program counter: a fetch
 * reports pc == fault address, but a DATA access reports the pc of the instruction, which is in the guest's own library
 * OUTSIDE the region. The crash reporter (husk-tl-unity-app.c, on_fatal) treats a true here as a GUEST failure: the run
 * ends, Husk stays, and the reason is persisted -- so a guest fault never takes the host process with it. A fault whose
 * address is OUTSIDE the region is still a host crash and keeps the old behaviour.
 */
bool tl_xmem_arena_fetch_fault(uintptr_t pc, uintptr_t addr);

/* Record why the last run died, in the same container file as the verdict (see tl_xmem_set_verdict_path). `pc` is the
 * address the guest branched to. Called from the crash reporter's guest-fault path; a no-op when no path was set. */
void tl_xmem_note_guest_fault(const char *reason, uintptr_t pc);

/*
 * THE FATAL-TIME MEASUREMENT: take the SAME three verdicts the open-time probe takes -- a guarded READ through each
 * view and a guarded CALL of a movz/ret stub -- on the page the guest actually faulted on, and print them on ONE line,
 * labelled, with whether they MATCH the open-time ones.
 *
 * It runs ONCE per fault, on a NORMAL THREAD, and NEVER inside the crash reporter's signal handler: installing and
 * longjmping out of a sigsetjmp guard for the signal already being handled is re-entrancy risk. The handler only
 * records the fault (tl_xmem_note_guest_fault); this is called later on the app-fault/session-close path
 * (husk_tl_session_close). A no-op unless the arena is open and an unreported guest fault was recorded. No vm_protect
 * is issued, the executable view is never written, nothing is unmapped, no thread is ended.
 */
void tl_xmem_report_fault_time(void);

/* ---------------------------------------------------------- the layer-lock ledger */

/*
 * Which of the layer's own locks is held, and by which thread. A session close QUARANTINES guest threads it cannot stop
 * (husk_tl_session_close); such a thread keeps running guest code and can hold a layer lock forever, and a swapped-in app
 * that then needs that lock stalls on its first frame with no other symptom. Every layer lock a swapped-in app can block
 * on records, lock-free, who holds it and where it was taken, so the stall probe (husk-tl-unity-app.c) can NAME the holder
 * instead of it being inferred.
 *
 * Slots are fixed (one per lock) so a note never takes a lock itself: the arena allocator's own lock is slot ARENA, and
 * the loader's two (husk-tl-ld.c) are recorded through the same table, so one reader sees every layer lock at once. The
 * codewrite guard is deliberately absent: it takes NO layer lock -- it runs on the signal path and carries a store into
 * the executable view through the writable twin without acquiring anything (husk-tl-codewrite.c).
 *
 * A note runs on the thread that just took (or is about to drop) the lock; only the holder writes owner/site, and a
 * reader reads them only after depth > 0 -- a release/acquire pairing. `depth` is 0 when free and counts nesting for the
 * loader's recursive lock. Reading never blocks, because a probe that reported a stall by blocking on a mutex a parked
 * thread held would be the very bug it looks for.
 */
enum { TL_LOCK_ARENA = 0, TL_LOCK_LOADER_BIG = 1, TL_LOCK_LOADER_LIBS = 2, TL_LOCK_COUNT };
typedef struct tl_lock_info {
    const char *name;      /* "arena", "loader-big", "loader-libs" */
    int depth;             /* 0 = free; > 0 = held (nesting depth for the recursive loader lock) */
    unsigned long owner;   /* the holding thread's pthread_t as an integer, 0 when free */
    char owner_name[32];   /* what the holder calls itself (pthread_getname_np), for the line */
    char site[48];         /* the layer function that last took it */
} tl_lock_info;
void tl_lock_acquired(int slot, const char *site);   /* call AFTER the mutex is taken */
void tl_lock_released(int slot);                     /* call AFTER the mutex is dropped */
int  tl_lock_ledger(tl_lock_info *out, int max);     /* snapshot; returns TL_LOCK_COUNT */

/* ------------------------------------------------------------------- a swap, in progress */

/*
 * Whether this process has crossed a session boundary (a swap). A close QUARANTINES guest threads it cannot stop and the
 * NEXT app is the one a swapped-out thread can wedge, so diagnostics that are too costly to keep on for a lone run turn
 * on from here: the guest-mutex trace (husk-tl-bionic-pthread.c) and the swapped-in asset trace (husk-tl-bionic-io.c).
 * The phone launches the IPA and nobody can set an environment variable on the device, so the LAYER decides. Set at the
 * close and by the stall probe when it fires; never cleared, because it is a process-level fact for the rest of the run.
 * One relaxed atomic on the reading side, so a hot path that checks it pays almost nothing.
 */
void tl_swap_in_progress_set(int on);
int  tl_swap_in_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_XMEM_H */
