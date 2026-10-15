/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-xmem.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#if TARGET_OS_OSX
#include <mach/mach_vm.h>
#endif
#endif

#include "husk-tl-internal.h"

void tl_log_line(const char *fmt, ...);
static void xmem_report_open(void);
#if defined(__APPLE__)
/* The guarded write-then-execute, defined with the other mach probes below; declared here so adopt_exec_view can
 * precede it. */
static bool xmem_writable_fetches(uintptr_t at, int *stage);
/* The codewrite store-fault guard (a store into the executable view, carried through the writable twin). Declared weak
 * so this file still links without husk-tl-codewrite.c (tools/a32/step0_xmem.c). */
extern bool tl_codewrite_enabled(void) __attribute__((weak));
#endif

static struct {
    uint8_t *rx, *rw;
    size_t size, used;
    bool open;
    /* True when the guest-facing view (g_x.rw) carries the JIT authorisation (Fix A): see tl_xmem_guest_view_is_jit. */
    bool guest_view_jit;
    /* How the guest-facing view's authorisation was obtained, for the report: "MAP_JIT", "plain RWX", or NULL when it
     * is a vm_remap (no authorisation of its own). */
    const char *route;
    /* True when the guest was handed the EXECUTABLE view of the debugger's region, held RX: its code arena executes and
     * the layer will NOT write it, so a store there faults and is carried through the writable twin -- with NO
     * vm_protect anywhere, because the ask for RWX revokes the debugger's per-entry execute grant (adopt_exec_view). */
    bool wx_fallback;
    /* The WRITABLE view the layer emits its OWN bytes through (library images, stubs, the codewrite/kernel-write
     * mirror). Same as g_x.rw where the guest-facing view is itself writable; in the W^X fallback it is the debugger's
     * writable vm_remap instead, and tl_xmem_delta() reaches it. NULL means "the same as g_x.rw". */
    uint8_t *wrw;
    pthread_mutex_t lock;
    /* The verdict of the guarded write-then-execute when the arena opened (tl_xmem_exec_verdict); the app's "JIT is
     * on" indicator is backed by it, not by the app's prewarmed flag. Zero-initialised = UNMEASURED. */
    int exec_measured;
    char exec_detail[240];
} g_x = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* The arena's own per-allocation work, counted so the log's per-alloc backing line is not the only evidence of how much
 * the backing pass costs: allocations handed out and 4 KiB pages backed by a volatile store (tl_xmem_alloc). Bumped under
 * g_x.lock; read racy from the heartbeat (a counter, not a control value). */
static size_t g_x_alloc_calls, g_x_alloc_pages;

/* See husk-tl-xmem.h: a session boundary has happened, so the diagnostics a swap needs are turned on. Plain __atomic
 * builtins -- no <stdatomic.h> needed here, and the a32 harness links this file with only its own errno stub. */
static int g_swap_in_progress;
void tl_swap_in_progress_set(int on) { __atomic_store_n(&g_swap_in_progress, on ? 1 : 0, __ATOMIC_RELEASE); }
int  tl_swap_in_progress(void)       { return __atomic_load_n(&g_swap_in_progress, __ATOMIC_ACQUIRE); }

#if defined(__APPLE__)
/*
 * The last GUEST FAULT the crash reporter handed in (tl_xmem_note_guest_fault), and whether the fatal-time verdicts
 * have been taken for it yet. The fault arrives in a SIGNAL HANDLER, where nothing but a plain record is safe: no mach
 * probe, and NEVER a sigsetjmp guard for the signal being handled (re-entrancy risk). So the handler only STORES the
 * address here; the three verdicts are taken LATER, ONCE, on the normal thread that runs the app-fault/session-close
 * path (tl_xmem_report_fault_time), after the handler returned. The record happens-before the run is marked FAILED and
 * the report happens-after the close observes that state, so the plain stores and loads are ordered by the state
 * machine. Nothing is unmapped, so the arena pages are still there when the report runs.
 */
static uintptr_t g_fault_addr;
static bool      g_fault_have;
static bool      g_fault_reported;
#endif

/* ============================================================================================
 * THE LAYER-LOCK LEDGER -- which of the layer's own locks is held, and by which thread.
 *
 * A session close QUARANTINES a guest thread it cannot stop: the thread keeps running guest code and may hold a layer
 * lock forever. A swapped-in app that then blocks on one of those locks stalls on its first frame and says nothing else.
 * So every layer lock a swapped-in app can block on records, lock-free, who holds it and where it was taken, and the
 * stall probe (husk-tl-unity-app.c) reads this table and NAMES the holder.
 *
 * The arena lock (g_x.lock) is slot ARENA; the loader's two locks (husk-tl-ld.c) are recorded through the SAME table, so
 * one reader sees every layer lock at once. The codewrite guard is deliberately not here: it takes no layer lock, it runs
 * on the signal path, and a store into the executable view is carried through the writable twin without acquiring
 * anything (husk-tl-codewrite.c).
 *
 * Lock-free, because a probe that reported a stall by blocking on a mutex a parked thread held would be the bug it looks
 * for. Only the holder writes owner/site; a reader reads them only after depth > 0 -- a release/acquire pairing. Depth is
 * 0 when free and counts nesting for the loader's recursive lock.
 */
static const char *const g_ledger_names[TL_LOCK_COUNT] = { "arena", "loader-big", "loader-libs" };
static struct {
    _Atomic int depth;
    _Atomic unsigned long owner;      /* pthread_t as an integer, 0 when free */
    char owner_name[32];
    char site[48];
} g_ledger[TL_LOCK_COUNT];

void tl_lock_acquired(int slot, const char *site)
{
    if (slot < 0 || slot >= TL_LOCK_COUNT) return;
    pthread_t t = pthread_self();
    pthread_getname_np(t, g_ledger[slot].owner_name, sizeof(g_ledger[slot].owner_name));
    if (!g_ledger[slot].owner_name[0]) snprintf(g_ledger[slot].owner_name, sizeof(g_ledger[slot].owner_name), "thread-%p", (void *)t);
    snprintf(g_ledger[slot].site, sizeof(g_ledger[slot].site), "%s", site ? site : "?");
    atomic_store_explicit(&g_ledger[slot].owner, (unsigned long)(uintptr_t)t, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ledger[slot].depth, 1, memory_order_release);
}

void tl_lock_released(int slot)
{
    if (slot < 0 || slot >= TL_LOCK_COUNT) return;
    int d = atomic_load_explicit(&g_ledger[slot].depth, memory_order_acquire);
    if (d > 0) atomic_fetch_sub_explicit(&g_ledger[slot].depth, 1, memory_order_release);
}

int tl_lock_ledger(tl_lock_info *out, int max)
{
    for (int i = 0; i < TL_LOCK_COUNT; i++) {
        if (!out || i >= max) break;
        out[i].name  = g_ledger_names[i];
        out[i].depth = atomic_load_explicit(&g_ledger[i].depth, memory_order_acquire);
        out[i].owner = atomic_load(&g_ledger[i].owner);
        snprintf(out[i].owner_name, sizeof(out[i].owner_name), "%s", g_ledger[i].owner_name);
        snprintf(out[i].site, sizeof(out[i].site), "%s", g_ledger[i].site);
    }
    return TL_LOCK_COUNT;
}

/* Record the guarded write-then-execute's outcome (RAN or FAULTED only); `detail` is copied, so the caller's buffer
 * need not outlive the call. Apple-only: the guarded probes are the mach ones below. The verdict is ALSO written to the
 * app's container here, because the guest's next fault may take the process down before the UI can ask, so the row must
 * be readable on the NEXT launch (xmem_persist_verdict, tl_xmem_set_verdict_path). */
#if defined(__APPLE__)
static void xmem_persist_verdict(void);      /* below, beside xmem_probe_entry (it reads the entry) */
static void xmem_note_exec(int verdict, const char *detail)
{
    g_x.exec_measured = verdict;
    if (detail && *detail) snprintf(g_x.exec_detail, sizeof(g_x.exec_detail), "%s", detail);
    else g_x.exec_detail[0] = '\0';
    xmem_persist_verdict();
}
#endif

/* The view the layer WRITES through: everything that mirrors a guest/executable address to a writable one goes
 * through this, never g_x.rw directly, because in the W^X fallback g_x.rw is the EXECUTABLE (read-only) view. */
static uint8_t *xmem_writable_view(void) { return g_x.wrw ? g_x.wrw : g_x.rw; }

/* Set while building the translation layer's OWN arena: make_views then adopts ONLY the order where the guest-facing
 * view carries the JIT authorisation (the address the guest both writes and executes). The fallback order -- a vm_remap
 * -- is what the two phone crashes proved iOS will not execute, so when the layer can have an authorised view of its own
 * it must not silently accept the fallback; if it cannot, the caller falls back to the debugger's region and says so. */
#if defined(__APPLE__)
static bool g_own_arena_only;
#endif

/* The per-thread W^X toggle for a MAP_JIT mapping. pthread_jit_write_protect_np() is in libsystem but the iOS SDK
 * marks it unavailable, so it is looked up at run time and nothing links against it: a thread's MAP_JIT pages are then
 * writable (write_mode(true)) or executable (write_mode(false)). A no-op where no write protection is enforced. */
static void xmem_write_mode(bool writable)
{
#if defined(__APPLE__)
    typedef int  (*xmem_wp_supported_fn)(void);
    typedef void (*xmem_wp_fn)(int);
    static xmem_wp_fn fn; static int resolved;
    if (!resolved) {
        xmem_wp_supported_fn supported = (xmem_wp_supported_fn)dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_supported_np");
        if (supported && supported()) fn = (xmem_wp_fn)dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np");
        resolved = 1;
    }
    if (fn) fn(writable ? 0 : 1);
#else
    (void)writable;
#endif
}

#if defined(__APPLE__)
/*
 * Executable memory a process makes for itself, where the kernel lets a process marked debugged (CS_DEBUGGED) run pages
 * it wrote: what a jailbreak allowing JIT in apps, TrollStore's enable-jit, or a debugger's attach on an iOS before TXM
 * gives. The two views sit a fixed distance apart (the loader reaches one from the other with adrp, +-4 GiB); which one
 * carries the JIT authorisation is settled by make_views below. Nothing is trusted until a function written through one
 * view has been CALLED through the other (and, for the preferred order, through itself); a guard turns a refusal into a
 * failure rather than a crash.
 */
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define TL_CS_OPS_STATUS 0
#define TL_CS_DEBUGGED   0x10000000u

/* Thread-local: the guarded fetch can run on any guest thread that asks to make its arena executable,
 * and a fault must longjmp into the frame of the thread that faulted, not another's. */
static __thread sigjmp_buf g_self_jump;
static __thread volatile sig_atomic_t g_self_running;
/* What the guarded fetch was killed by, kept so the verdict line can say it. "the fetch failed"
 * alone cannot tell a fault (SIGBUS with si_code KERN_PROTECTION_FAILURE) from a device that ends
 * the process for fetching unblessed memory; the run must name the signal and the si_code. */
static __thread volatile sig_atomic_t g_self_sig, g_self_code;
/* Which step of the guarded write-then-execute was running when it faulted: 0 none, 1 the store
 * into the guest's address, 2 the fetch of what was stored. The two answers call for different
 * things -- a store fault means the page cannot be WRITTEN (W^X), a fetch fault means it can be
 * written but cannot be EXECUTED -- so the verdict line names which. */
static __thread volatile sig_atomic_t g_self_stage;
static void self_fault_info(int sig, siginfo_t *info, void *uctx)
{
    (void)uctx;
    if (g_self_running) { g_self_sig = sig; g_self_code = info ? info->si_code : 0; siglongjmp(g_self_jump, 1); }
    signal(sig, SIG_DFL);
    raise(sig);
}

/*
 * Write the movz/ret stub through `write_at`, flush it, and call it at `call_at`, under the guard.
 * `write_at` and `call_at` are the two views of one set of pages, or one view named twice when the
 * question is whether that single view both takes a write and executes -- which is exactly the
 * question a guest's JIT arena asks of the writable view. The write is bracketed with the W^X toggle
 * so that a MAP_JIT view is writable for the store and executable for the call. True only if the call
 * ran and returned 0x5a5a.
 */
static bool self_runs(uint8_t *call_at, uint8_t *write_at)
{
    static const uint32_t code[2] = { 0x528B4B40u, 0xD65F03C0u };
    vm_address_t addr = (vm_address_t)call_at; vm_size_t sz = 0; natural_t depth = 0;
    vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (vm_region_recurse_64(mach_task_self(), &addr, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS || !(info.protection & VM_PROT_EXECUTE)) return false;
    struct sigaction sa, ob, os, oi;
    memset(&sa, 0, sizeof(sa)); sa.sa_sigaction = self_fault_info; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &ob); sigaction(SIGSEGV, &sa, &os); sigaction(SIGILL, &sa, &oi);
    bool ok = false;
    g_self_running = 1;
    g_self_sig = g_self_code = 0;
    if (sigsetjmp(g_self_jump, 1) == 0) {
        xmem_write_mode(true);
        memcpy(write_at, code, sizeof(code));
        xmem_write_mode(false);
        sys_icache_invalidate(call_at, sizeof(code));
        ok = ((int (*)(void))(void *)call_at)() == 0x5a5a;
    }
    g_self_running = 0;
    sigaction(SIGBUS, &ob, NULL); sigaction(SIGSEGV, &os, NULL); sigaction(SIGILL, &oi, NULL);
    return ok;
}

/*
 * Build the two views of one set of pages inside the reservation `base`, and hand them to the loader as g_x.rw (the view
 * the guest addresses and calls) and g_x.rx (the view guest CODE runs through). Which mapping carries the JIT
 * authorisation is the whole question:
 *
 *   - Fix A, tried first: the guest-facing view IS the mmap (PROT_READ|PROT_WRITE|PROT_EXEC, MAP_JIT, or plain where
 *     the host allows RWX), and the code view is a vm_remap of it. A guest's own arena is then an authorised mapping, so
 *     adding execute to it -- all the guest does -- is real.
 *   - the fallback: the mmap carries the authorisation as the CODE view and the guest-facing view is the vm_remap. A
 *     guest arena in the writable view is then a vm_remap alias no vm_protect can make fetch, which tl_xmem_allow_exec
 *     refuses honestly.
 *
 * The order is settled by MEASUREMENT, never a guess: a pair is adopted only after both views have been shown to run a
 * function written through the other (and, for Fix A, the guest-facing view one written through itself). A host that
 * refuses both fails loudly. Nothing is unmapped (the next attempt overwrites with MAP_FIXED) and no thread is ended.
 */
static bool make_views(uint8_t *base, size_t size, char *err, size_t errlen)
{
    task_t me = mach_task_self();
    vm_prot_t cur, max;
    int last_errno = 0;
    /* TL_XMEM_NO_JIT_VIEW=1 forces the fallback order (the negative control: a writable view that is a
     * vm_remap alias must stay non-executable). */
    bool allow_fix_a = getenv("TL_XMEM_NO_JIT_VIEW") == NULL;

    /* Fix A: the guest-facing view (g_x.rw) is the authorised mmap; the code view (g_x.rx) is a vm_remap. */
    static const int guest_extra[2] = { MAP_JIT, 0 };   /* MAP_JIT first, then plain (a host that allows RWX) */
    for (int k = 0; allow_fix_a && k < 2; k++) {
        uint8_t *rw = mmap(base, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_FIXED | MAP_PRIVATE | MAP_ANON | guest_extra[k], -1, 0);
        if (rw == MAP_FAILED) { last_errno = errno; continue; }
        vm_address_t alias = (vm_address_t)(uintptr_t)(base + size);
        kern_return_t kr = vm_remap(me, &alias, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                    me, (vm_address_t)(uintptr_t)rw, FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) { last_errno = EPERM; continue; }
        if (vm_protect(me, alias, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) { last_errno = EPERM; continue; }
        bool code_view_runs = self_runs((uint8_t *)(uintptr_t)alias, rw);   /* write the guest view, run the code view */
        bool guest_view_runs = self_runs(rw, rw);                          /* the question the phone died on */
        if (code_view_runs && guest_view_runs) {
            g_x.rw = rw; g_x.rx = (uint8_t *)(uintptr_t)alias; g_x.size = size; g_x.guest_view_jit = true;
            g_x.route = guest_extra[k] ? "MAP_JIT" : "plain RWX";
            xmem_note_exec(TL_XMEM_EXEC_RAN, NULL);   /* the guest's own view ran a function written into it */
            tl_log_line("xmem: Fix A: the view the guest addresses and calls (%p) is the %s mapping and the code view (%p) is a vm_remap of it; a guest's own arena there is authorised",
                        (void *)rw, guest_extra[k] ? "MAP_JIT" : "plain RWX", (void *)alias);
            return true;
        }
        tl_log_line("xmem: a %s guest-facing view was created but %s; not adopting it",
                    guest_extra[k] ? "MAP_JIT" : "plain RWX",
                    !code_view_runs && !guest_view_runs ? "neither view executed a function written into it"
                    : code_view_runs ? "the guest-facing view did not execute (its code view did)"
                                     : "the code view (a vm_remap) did not execute");
    }

    /* The layer wanted an arena it owns (g_own_arena_only): the vm_remap fallback below gives the
     * guest an address it cannot execute, which is the shape of the two phone crashes, so refuse
     * here rather than adopt it -- the caller can then fall back to the debugger's region. */
    if (g_own_arena_only) {
        snprintf(err, errlen, "no guest-owned writable-and-executable view here: the MAP_JIT/RWX mmap was refused or did not execute (%s), so the only view left for the guest is a vm_remap",
                 last_errno ? strerror(last_errno) : "refused");
        return false;
    }

    /* The fallback: the mmap carries the authorisation as the code view; the guest-facing view is the
     * vm_remap. This is the order that shipped, and the shape the negative control asserts. */
    static const int code_prot[2] = { PROT_READ | PROT_EXEC, PROT_READ | PROT_WRITE | PROT_EXEC };
    static const int code_extra[2] = { 0, MAP_JIT };
    for (int k = 0; k < 2; k++) {
        uint8_t *rx = mmap(base, size, code_prot[k], MAP_FIXED | MAP_PRIVATE | MAP_ANON | code_extra[k], -1, 0);
        if (rx == MAP_FAILED) { last_errno = errno; continue; }
        vm_address_t alias = (vm_address_t)(uintptr_t)(base + size);
        kern_return_t kr = vm_remap(me, &alias, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                    me, (vm_address_t)(uintptr_t)rx, FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) { last_errno = EPERM; continue; }
        if (vm_protect(me, alias, size, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) { last_errno = EPERM; continue; }
        if (self_runs(rx, (uint8_t *)(uintptr_t)alias)) {
            g_x.rx = rx; g_x.rw = (uint8_t *)(uintptr_t)alias; g_x.size = size; g_x.guest_view_jit = false;
            g_x.route = "vm_remap";
            /* The guest-facing view here is a vm_remap alias of the code view: guest CODE runs (through
             * rx), but a guest's OWN arena at a writable-view address cannot fetch -- which is exactly
             * the question the indicator asks. Measured now, not asserted. */
            xmem_note_exec(TL_XMEM_EXEC_FAULTED,
                           "the guest-facing view is a vm_remap of the code view; a guest's own arena there cannot fetch (iOS authorises execute per mapping ENTRY and an alias never inherits it)");
            tl_log_line("xmem: no authorised guest-facing view here: fell back to the two-view order (%s mapping, code view %p, guest view %p); a guest arena in the writable view cannot be made to fetch",
                        code_extra[k] ? "MAP_JIT" : "plain", (void *)rx, (void *)alias);
            return true;
        }
        last_errno = EPERM;
    }
    snprintf(err, errlen, "could not make two executable views of one set of pages here (%s)", last_errno ? strerror(last_errno) : "refused");
    return false;
}

static bool grant_self(size_t host_bytes, char *err, size_t errlen)
{
    uint32_t flags = 0;
    bool debugged = csops(getpid(), TL_CS_OPS_STATUS, &flags, sizeof(flags)) == 0 && (flags & TL_CS_DEBUGGED);
    int last_errno = 0;
    for (size_t size = (host_bytes + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1); size >= (64u << 20); size /= 2) {
        uint8_t *base = mmap(NULL, size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (base == MAP_FAILED) { last_errno = errno; continue; }
        char local[160] = "";
        if (make_views(base, size, local, sizeof(local))) {
            tl_log_line("xmem: made its own %zu MiB executable region (%s; process %s debugged)", size >> 20,
                        g_x.guest_view_jit ? "Fix A: the guest-facing view carries the JIT authorisation"
                                           : "two-view fallback: a writable-view arena will be refused",
                        debugged ? "is" : "is not");
            return true;
        }
        last_errno = EPERM;
        munmap(base, size * 2);
    }
    snprintf(err, errlen, "this process cannot make executable memory for itself (%s%s). It needs JIT: a jailbreak with JIT allowed for apps, TrollStore's enable-jit, or a debugger",
             last_errno ? strerror(last_errno) : "refused", debugged ? "" : "; it is not marked as debugged");
    return false;
}

/*
 * The translation layer's OWN arena: a mapping the APP owns, obtained here (not the debugger's region, whose owner is
 * QEMU), whose guest-facing view is the address the guest both writes and executes. Two phone crashes are why this is
 * preferred: a guest that derives a code pointer from the writable base cannot use the two-address scheme, and
 * StikDebug's region is reserved rather than populated, so a fetch at a page that NAMED execute can still bus-error
 * (xmem-arena-and-wx.md). An mmap of the app's own is fully backed, and with CS_DEBUGGED set MAP_JIT is honoured. It is
 * proven by measurement (make_views) before this returns true; when it cannot be had, the caller keeps the honest
 * refusal and falls back to the debugger's region, naming why.
 */
static bool grant_own_arena(size_t host_bytes, char *err, size_t errlen)
{
    g_own_arena_only = true;
    bool ok = grant_self(host_bytes, err, errlen);
    g_own_arena_only = false;
    return ok;
}
#endif

#if defined(__APPLE__) && !TARGET_OS_OSX
/* The loader retargets code at the writable view with adrp (+-4 GiB). A region whose writable alias the kernel placed far
 * away gets a second alias of the same pages next to the executable view: a local vm_remap, which needs no debugger. Free
 * addresses after the region, then before it, in 16 MiB steps out to 2 GiB. */
static bool near(uintptr_t a, uintptr_t rx) { intptr_t d = (intptr_t)(a - rx); return d < ((intptr_t)2 << 30) && d > -((intptr_t)2 << 30); }
static bool adopt_alias(vm_address_t a, uintptr_t rx, size_t size, const char *how, ptrdiff_t was)
{
    if (!near(a, rx)) { vm_deallocate(mach_task_self(), a, size); return false; }
    kern_return_t kr = vm_protect(mach_task_self(), a, size, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) { tl_log_line("xmem: %s alias at %p: vm_protect rw failed (%d)", how, (void *)a, kr); vm_deallocate(mach_task_self(), a, size); return false; }
    tl_log_line("xmem: the writable view was %td MiB from the executable one; a new one (%s) at %p is %td MiB from it", was >> 20, how, (void *)a, (ptrdiff_t)(a - rx) >> 20);
    if (g_x.wrw) g_x.wrw = (uint8_t *)a;   /* the W^X fallback's twin, when the guest holds the RX view */
    else g_x.rw = (uint8_t *)a;
    return true;
}
static void near_alias(void)
{
    ptrdiff_t d = xmem_writable_view() - g_x.rx;
    if (d < ((ptrdiff_t)2 << 30) && d > -((ptrdiff_t)2 << 30)) return;
    const uintptr_t step = 16u << 20, reach = (uintptr_t)2 << 30, rx = (uintptr_t)g_x.rx, size = g_x.size;
    task_t me = mach_task_self();
    vm_prot_t cur, max;
    /* what is around the region, for the log */
    {
        vm_address_t a = rx; vm_size_t sz = 0; natural_t depth = 0; vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(me, &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) == KERN_SUCCESS)
            tl_log_line("xmem: region rx %p size %zu MiB; the map entry there is %p..%p prot %d/%d tag %u", (void *)rx, size >> 20, (void *)a, (void *)(a + sz), info.protection, info.max_protection, info.user_tag);
        a = rx + size; sz = 0; depth = 0; cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(me, &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) == KERN_SUCCESS)
            tl_log_line("xmem: next entry after the region: %p..%p prot %d/%d tag %u", (void *)a, (void *)(a + sz), info.protection, info.max_protection, info.user_tag);
    }
    /* 1. the kernel's own search, upward from just after the region and from 2 GiB below it */
    uintptr_t hints[2] = { rx + size, rx > reach ? rx - reach : 0 };
    for (int h = 0; h < 2; h++) {
        vm_address_t a = hints[h];
        kern_return_t kr = vm_remap(me, &a, size, 0, VM_FLAGS_ANYWHERE, me, rx, FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) { tl_log_line("xmem: remap searching from %p failed (%d)", (void *)hints[h], kr); continue; }
        if (adopt_alias(a, rx, size, h ? "searched from below" : "searched from after", d)) return;
        tl_log_line("xmem: remap searching from %p landed at %p, too far", (void *)hints[h], (void *)a);
    }
    /* 2. fixed addresses after, then before, in 16 MiB steps out to 2 GiB; and when no gap near the region holds all of it (a
     *    region placed low in the address space, between other mappings), the largest part of it that fits: the region is used
     *    from its start, so a shorter alias of its first part is a shorter region. */
    kern_return_t first = KERN_SUCCESS;
    for (uintptr_t want_size = size; want_size >= (64u << 20); want_size -= (32u << 20)) {
        for (uintptr_t off = 0; off <= reach; off += step) {
            for (int side = 0; side < 2; side++) {
                if (side == 1 && rx < want_size + off + step) continue;
                vm_address_t want = side == 0 ? rx + size + off : rx - want_size - off;
                kern_return_t kr = vm_remap(me, &want, want_size, 0, VM_FLAGS_FIXED, me, rx, FALSE, &cur, &max, VM_INHERIT_NONE);
                if (kr != KERN_SUCCESS) { if (first == KERN_SUCCESS) first = kr; continue; }
                if (adopt_alias(want, rx, want_size, want_size == size ? "fixed" : "fixed, shorter", d)) {
                    if (want_size < size) { tl_log_line("xmem: the region is used as %zu MiB of its %zu", (size_t)(want_size >> 20), (size_t)(size >> 20)); g_x.size = want_size; }
                    return;
                }
            }
        }
    }
    tl_log_line("xmem: fixed remaps near the region failed (first error %d)", first);
    /* 3. a reservation first (plain memory, a hint near the region), then the alias over it */
    for (int h = 0; h < 2; h++) {
        void *res = mmap((void *)hints[h], size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (res == MAP_FAILED) { tl_log_line("xmem: reservation near %p failed (%d)", (void *)hints[h], errno); continue; }
        if (!near((uintptr_t)res, rx)) { tl_log_line("xmem: reservation near %p landed at %p, too far", (void *)hints[h], res); munmap(res, size); continue; }
        vm_address_t a = (vm_address_t)res;
        kern_return_t kr = vm_remap(me, &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, me, rx, FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) { tl_log_line("xmem: alias over the reservation at %p failed (%d)", res, kr); munmap(res, size); continue; }
        if (adopt_alias(a, rx, size, "over a reservation", d)) return;
    }
    tl_log_line("xmem: the writable view is %td MiB from the executable one and no address near it is free", d >> 20);
}

/* Hand the guest the StikDebug EXECUTABLE view, proven by a guarded write-then-execute; defined with the mach probes
 * below. */
static bool adopt_exec_view(tl_dual_mapping *m, char *err, size_t errlen);

/* The phone: an arena the layer makes for ITSELF if the device allows it, and only then StikDebug's region. Nothing is
 * trusted by version: both routes are proven by running a function written into the view the guest addresses (make_views),
 * and whichever is taken is named in one line. */
static bool open_platform(size_t host_bytes, char *err, size_t errlen)
{
    g_x.wrw = NULL;                    /* "writable view == g_x.rw"; the W^X fallback below sets it */
    tl_dual_mapping *m = tl_find_stikdebug_prewarmed();

    /* 1. The layer's own arena first: the address the guest both writes and executes, in a mapping the app owns. */
    {
        char own[160] = "";
        if (grant_own_arena(host_bytes, own, sizeof(own))) {
            tl_log_line("xmem: open: the translation layer's OWN arena -- guest-facing view %p (%s), code view %p, size %zu MiB, delta %td; the guest may write AND execute at one address here",
                        (void *)g_x.rw, g_x.route ? g_x.route : "?", (void *)g_x.rx, g_x.size >> 20, tl_xmem_delta());
            return true;
        }
        if (m && m->rw_addr && m->rx_addr)
            tl_log_line("xmem: no arena of the layer's own (%s); falling back to StikDebug's region", own[0] ? own : "refused");
        else {
            /* No debugger region: the layer makes its own and accepts the two-view fallback where Fix A is not granted
             * (libraries still load; only a guest JIT arena is refused, and tl_xmem_allow_exec says so). From iOS 26 the
             * Trusted Execution Monitor can end a process that runs unblessed memory, so without a debugger nothing is
             * tried there. */
            if (__builtin_available(iOS 26.0, *)) {
                snprintf(err, errlen, "no arena of the layer's own (%s), and no StikDebug JIT region: this iOS enforces TXM, where only a debugger (StikDebug or Built-in StikJIT) can grant executable memory", own[0] ? own : "refused");
                return false;
            }
            return grant_self(host_bytes, err, errlen);
        }
    }

    if (m && m->rw_addr && m->rx_addr) {
        /* Taken FIRST -- before a single protection-changing call on this region. If this says RAN and the one after
         * the open says FAULTED, one of the calls between them cost the execute; if both say RAN, none of ours did and
         * the loss is outside the layer (the debugger's presence going away). */
        tl_xmem_exec_gate("the arena opening, before ANY protection-changing call of the layer's", m->rw_addr, m->rx_addr);
        char why[240] = "";
        /* adopt_exec_view issues NO vm_protect on this region, ever (that ask is what revoked the grant; the two-run
         * control is in jit-grant-and-detach.md). It proves the executable view with the boot selftest's shape -- write
         * through the writable twin, call at the executable view -- and hands the guest the EXECUTABLE view (RX). */
        (void)adopt_exec_view(m, why, sizeof(why));
        near_alias();                    /* keep the writable twin within adrp range of the executable view */
        tl_log_line("xmem: open: StikDebug's prewarmed region -- guest arena %p is the EXECUTABLE view %p (RX), the one whose generated code provably ran at boot; the layer emits and carries guest stores through the writable twin %p (delta %td). The guard verdict: %s",
                    (void *)g_x.rw, (void *)g_x.rx, (void *)g_x.wrw, tl_xmem_delta(),
                    g_x.exec_measured == TL_XMEM_EXEC_RAN ? "the guarded write-then-execute RAN" : "the guarded write-then-execute FAULTED (see the line above and the honest refusal below)");
        /* Measure again, after the open: the pair with the gate above is the discriminator. */
        tl_xmem_exec_gate("the arena open (adopt_exec_view issues no vm_protect on this region)", m->rw_addr, m->rx_addr);
        /* A store into the executable view faults (it is RX), and only the JIT guard can carry it through the writable
         * twin. The guest holds a JIT arena here, so arm it now rather than wait for a driver that may never call
         * (xmem-arena-and-wx.md). Safe for any engine: on_fault acts only inside this region and only on a store it can
         * decode, and chains to the previous handler otherwise. */
        {
            const char *guard = !tl_codewrite_enabled ? "UNKNOWN (the codewrite module is not linked here)"
                              : tl_codewrite_enabled() ? "ARMED (a driver had already enabled it)"
                                                       : "NOT ARMED yet";
            tl_log_line("xmem: W^X: the JIT guard for stores into the executable view is %s", guard);
            extern void tl_codewrite_enable(void) __attribute__((weak));
            if (tl_codewrite_enable && (!tl_codewrite_enabled || !tl_codewrite_enabled())) {
                tl_codewrite_enable();
                tl_log_line("xmem: W^X: the JIT guard is now ARMED by the arena open -- a store into the executable view is decoded from the faulting pc and carried through the writable twin %p, and the executable page stays executable (no vm_protect of the region's pages, don't ask)", (void *)g_x.wrw);
            }
        }
        if (g_x.exec_measured != TL_XMEM_EXEC_RAN)
            tl_log_line("xmem: W^X: honest refusal -- %s", why[0] ? why : "the executable view does not fetch");
        return true;
    }
    /* Reached only if tl_find_stikdebug_prewarmed changed its mind between the two calls. */
    snprintf(err, errlen, "no StikDebug JIT region, and no arena of the layer's own");
    return false;
}
#else
/* A Mac (or a plain host): two views of ordinary memory. */
static bool open_platform(size_t host_bytes, char *err, size_t errlen)
{
    /* TL_XMEM_MIB: size the region as a phone's would be, to see whether a game fits there */
    if (getenv("TL_XMEM_MIB")) host_bytes = (size_t)atoi(getenv("TL_XMEM_MIB")) << 20;
#if defined(__APPLE__)
    /* TL_XMEM_SELF=1: make the region the way a jailbroken or TrollStore phone does, to test that path here */
    if (getenv("TL_XMEM_SELF")) return grant_self(host_bytes, err, errlen);
#endif
    size_t size = (host_bytes + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1);
    /* Reserve twice the span, views side by side: adrp reaches +-4 GiB and the loader retargets code at the writable
     * view with it, so the views must stay close. */
    uint8_t *base = mmap(NULL, size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        snprintf(err, errlen, "mmap of %zu MiB failed: %s", (size * 2) >> 20, strerror(errno));
        return false;
    }
#if defined(__APPLE__)
    /* The same builder the phone's self-grant path uses, so the Mac runs the code the phone runs -- including the Fix A
     * ordering and the two-view fallback. */
    if (!make_views(base, size, err, errlen)) { munmap(base, size * 2); return false; }
    return true;
#else
    snprintf(err, errlen, "no dual mapping on this host");
    munmap(base, size * 2);
    return false;
#endif
}
#endif

bool tl_xmem_open(size_t host_bytes, char *err, size_t errlen)
{
    bool ok = true;
    pthread_mutex_lock(&g_x.lock);
    tl_lock_acquired(TL_LOCK_ARENA, "tl_xmem_open");
    if (!g_x.open) {
        char local[160] = "";
        ok = open_platform(host_bytes, local, sizeof(local));
        if (ok) {
            g_x.open = true;
            /* What the kernel says the region's entries are, and whether the guest's view fetches. */
            xmem_report_open();
        } else if (err && errlen) {
            snprintf(err, errlen, "%s", local);
        }
    }
    tl_lock_released(TL_LOCK_ARENA);
    pthread_mutex_unlock(&g_x.lock);
    return ok;
}

/*
 * ONE guarded read through a named view, under the SAME signal guard self_runs and xmem_write_fetch use
 * (self_fault_info + sigsetjmp): the read returns a value or faults into the guard, and the verdict is which. It touches
 * nothing else -- no store, no vm_protect, nothing unmapped, no thread ended -- so a fault here is caught, not fatal.
 * Apple-only, like every guarded probe here; the host build does the plain read so this file still links (tools/a32/step0).
 */
#if defined(__APPLE__)
static bool xmem_guarded_read(const unsigned char *p, unsigned char *got, int *sig, int *code)
{
    struct sigaction sa, ob, os, oi;
    memset(&sa, 0, sizeof(sa)); sa.sa_sigaction = self_fault_info; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &ob); sigaction(SIGSEGV, &sa, &os); sigaction(SIGILL, &sa, &oi);
    bool ok = false;
    g_self_running = 1;
    g_self_sig = g_self_code = 0;
    if (sigsetjmp(g_self_jump, 1) == 0) { *got = *p; ok = true; }   /* the read; its value is USED below, so it cannot be elided */
    g_self_running = 0;
    sigaction(SIGBUS, &ob, NULL); sigaction(SIGSEGV, &os, NULL); sigaction(SIGILL, &oi, NULL);
    if (sig) *sig = (int)g_self_sig;
    if (code) *code = (int)g_self_code;
    return ok;
}
#else
/* No guard on the host: the read is plain, and there is no signal to name (`sig`/`code` are still filled so the
 * caller's line is the same everywhere). */
static bool xmem_guarded_read(const unsigned char *p, unsigned char *got, int *sig, int *code)
{ *got = *p; if (sig) *sig = 0; if (code) *code = 0; return true; }
#endif

/* Whether the backing PROOF runs. The per-page READ-BACK, the checksum folded from it and the two guarded reads of the
 * span's first page were the honest fix for the dead-store bug: they PROVE the volatile store landed (a page that was
 * never backed faults HERE, inside the layer, instead of later inside the guest). But that proof is a DEBUG ASSERTION,
 * not production work -- it doubles the pass's memory traffic and each guarded read installs and restores three signal
 * handlers, six per allocation. It runs only with TL_XMEM_BACKING_TRACE set. The VOLATILE STORE LOOP IS NEVER GATED: it
 * is what makes the kernel back each page (and being volatile, cannot be elided as a dead store). */
static bool xmem_backing_trace(void)
{
    static int on = -1;
    if (on < 0) { const char *s = getenv("TL_XMEM_BACKING_TRACE"); on = (s && *s && *s != '0') ? 1 : 0; }
    return on != 0;
}

/*
 * Hand out a span of the arena -- and BACK it.
 *
 * The span bumping g_x.used returns is MAPPED but the pages behind it are not necessarily RESIDENT until something
 * WRITES them (the same way the loader's memset is what makes the kernel back an image's pages). A span handed out but
 * never written -- a stub page, the JIT arena, any tail the emitter did not reach -- is mapped-but-unbacked, and a later
 * guest DATA access to one of its pages takes SIGBUS on a page whose entry NAMED execute (xmem-arena-and-wx.md).
 *
 * So back it here, the moment it is handed out, a write through the WRITABLE view (NEVER the executable view, which is
 * RX). One byte per 4 KiB page forces the kernel to back the page; a full memset is not required. Exactly the span THIS
 * call handed out is backed, and NO vm_protect is issued (the ask for RWX is what revoked the debugger's grant, see
 * adopt_exec_view). Nothing is unmapped and no thread is ended.
 */
bool tl_xmem_alloc(size_t bytes, uint8_t **rx, uint8_t **rw)
{
    size_t n = (bytes + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1);
    bool ok = false;
    pthread_mutex_lock(&g_x.lock);
    tl_lock_acquired(TL_LOCK_ARENA, "tl_xmem_alloc");
    if (g_x.open && n <= g_x.size - g_x.used) {
        size_t start = g_x.used;
        if (rx) *rx = g_x.rx + start;
        if (rw) *rw = xmem_writable_view() + start;
        g_x.used += n;
        ok = true;
        /* Back the span just handed out: one store per 4 KiB page through the writable view (a store, not a read, makes
         * the kernel publish the page; the span is fresh, so writing zero is safe).
         *
         * The store MUST be volatile: a store of zero to memory only ever written and never read is a DEAD STORE the
         * optimizer may delete in a Release build -- and did, leaving the guest to fault at the identical offset. A
         * volatile access is an observable side effect and cannot be elided.
         *
         * And it is PROVEN: each page is READ BACK through the same writable view and folded into a checksum the log
         * prints, so a page that was never backed faults HERE, inside the layer, instead of later inside the guest. */
        {
            const bool trace = xmem_backing_trace();
            uint8_t *wv = xmem_writable_view() + start;
            volatile unsigned char *vb = (volatile unsigned char *)wv;
            size_t touched = 0;
            uint64_t checksum = 0;
            bool readback_ok = true;
            for (size_t off = 0; off < n; off += 4096u) {
                vb[off] = 0;             /* volatile store -- the kernel backs the page. REQUIRED, never gated: this is
                                          * what publishes the page, and volatile so a Release build cannot delete it as
                                          * a dead store (it did, once -- the bug this pass exists to end). */
                if (trace) {
                    unsigned char got = vb[off]; /* read back the SAME byte through the SAME writable view */
                    if (got != 0) readback_ok = false;
                    checksum ^= (uint64_t)got ^ (uint64_t)(off / 4096u);
                }
                touched++;
            }
            __atomic_fetch_add(&g_x_alloc_calls, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_x_alloc_pages, touched, __ATOMIC_RELAXED);
            if (!trace) {
                /* Production: the volatile stores above are all the backing needs and cannot be elided. The read-back,
                 * the checksum and the two guarded reads are the DEBUG ASSERTION that PROVES the store landed -- off
                 * unless TL_XMEM_BACKING_TRACE is set (they double the pass's memory traffic, and the guarded reads
                 * install six signal handlers per allocation). The pages backed are still counted and summarised. */
                tl_log_line("xmem: backed the span just handed out %p..%p (%zu bytes, %zu pages; %zu page(s) backed so far) -- one VOLATILE store per 4 KiB page through the writable twin %p is what makes the kernel back each page (no vm_protect, nothing unmapped); the read-back/dead-store proof is OFF (TL_XMEM_BACKING_TRACE=1 to run it)",
                            (void *)wv, (void *)(wv + n), n, touched,
                            (size_t)__atomic_load_n(&g_x_alloc_pages, __ATOMIC_RELAXED), (void *)xmem_writable_view());
            } else {
            const char *rb = readback_ok
                ? "read back OK -- every page re-read through the same writable view returned the value just written"
                : "read back FAILED -- a page did not return the value just written, so it may not be backed";
            /* Read back the span's FIRST page (the byte the volatile store just backed, so it is genuinely twin-backed
             * and not a page the layer writes code into) through BOTH views and print the two verdicts on one line. If
             * the executable view READS it, the views share readable backing; if it FAULTS (caught, never fatal), pages
             * written only through the twin are not readable through the executable view, which is the fault. Nothing
             * is written to the executable view, no vm_protect is issued, nothing is unmapped, no thread is ended. */
            uint8_t xb = 0, wb = 0;
            int xsig = 0, xcode = 0, wsig = 0, wcode = 0;
            bool xok = xmem_guarded_read((const unsigned char *)(g_x.rx + start), &xb, &xsig, &xcode);  /* g_x.rx + start: the span's first page in the EXECUTABLE view */
            bool wok = xmem_guarded_read((const unsigned char *)wv, &wb, &wsig, &wcode);                /* the same page through the WRITABLE twin */
            const char *xverdict = xok
                ? "the executable view READ the backed page"
                : "the executable view FAULTED on a backed page -- the two views do not share readable backing";
            const char *wverdict = wok
                ? "the writable view READ the backed page"
                : "the writable view FAULTED on a backed page (unexpected: the twin is the view that was just written)";
            tl_log_line("xmem: arena page backing: backed the span just handed out %p..%p (%zu bytes, %zu pages) -- one VOLATILE store per 4 KiB page through the writable view %p (volatile, so the compiler cannot delete it as a dead store), then %s; checksum 0x%016llx over %zu page(s), so the store is real and a later guest DATA access to these pages cannot SIGBUS on an unbacked page (no vm_protect, nothing unmapped); and ONE guarded read of the span's first page (offset +0x0) through the two views: the EXECUTABLE view %p (arena base %p) -> %s (byte 0x%02x, signal %d si_code %d); the WRITABLE view %p -> %s (byte 0x%02x, signal %d si_code %d)",
                        (void *)wv, (void *)(wv + n), n, n / 4096u, (void *)xmem_writable_view(),
                        rb, (unsigned long long)checksum, touched,
                        (void *)(g_x.rx + start), (void *)g_x.rx, xverdict, (unsigned)xb, xsig, xcode,
                        (void *)wv, wverdict, (unsigned)wb, wsig, wcode);
            }
        }
    }
    tl_lock_released(TL_LOCK_ARENA);
    pthread_mutex_unlock(&g_x.lock);
    return ok;
}

/* The arena's per-allocation work so far (see g_x_alloc_calls/g_x_alloc_pages). Read-only, safe any time. */
void tl_xmem_backing_stats(size_t *allocs, size_t *pages)
{
    if (allocs) *allocs = __atomic_load_n(&g_x_alloc_calls, __ATOMIC_RELAXED);
    if (pages)  *pages  = __atomic_load_n(&g_x_alloc_pages,  __ATOMIC_RELAXED);
}

ptrdiff_t tl_xmem_delta(void) { return xmem_writable_view() - g_x.rx; }
bool tl_xmem_near(const void *p, size_t dist)
{
    if (!g_x.open || !p) return false;
    /* Two views of one set of pages; in the Fix A order the guest-facing one is the LOWER address, so the span is taken
     * from both views rather than assumed to start at rx. */
    uintptr_t a = (uintptr_t)p, lo = (uintptr_t)g_x.rx, hi = lo + g_x.size;
    uintptr_t views[2] = { (uintptr_t)g_x.rw, (uintptr_t)xmem_writable_view() };
    for (int i = 0; i < 2; i++) {
        if (views[i] < lo) lo = views[i];
        if (views[i] + g_x.size > hi) hi = views[i] + g_x.size;
    }
    return a + dist >= lo && a <= hi + dist;
}

bool tl_xmem_contains(const void *p)
{
    const uint8_t *b = p, *w = xmem_writable_view();
    return g_x.open && ((b >= g_x.rx && b < g_x.rx + g_x.size)
                     || (b >= g_x.rw && b < g_x.rw + g_x.size)
                     || (b >= w && b < w + g_x.size));
}

bool tl_xmem_is_rx(const void *p)
{
    const uint8_t *b = p;
    return g_x.open && b >= g_x.rx && b < g_x.rx + g_x.size;
}

void tl_xmem_flush(const void *rx, size_t bytes)
{
#if defined(__APPLE__)
    sys_icache_invalidate((void *)rx, bytes);
#else
    __builtin___clear_cache((char *)rx, (char *)rx + bytes);
#endif
}

/* b_mprotect, in the bionic shim, owns the guest's errno. step0 links only this file, so this is declared weak:
 * allow_exec sets EACCES where the shim is present and still links where it is not. */
extern void tl_set_guest_errno(int guest_errno) __attribute__((weak));

#if defined(__APPLE__)
/*
 * The two questions vm_protect's return code does not answer. KERN_SUCCESS means only that the request was within the
 * entry's max_protection and the protection field now names EXECUTE -- not that the pmap will publish a fetchable PTE.
 * On iOS a vm_remap alias of pages never granted JIT authorisation names execute and still will not fetch: the SIGBUS
 * (pc == fault address) the writable-view path exists to end. So the verdict is never taken from the return code.
 */

/* What the map entry that holds `p` actually says. Keeping the RAW answer -- the call's own return code plus the entry
 * the kernel named, its range, protection, max_protection, depth and tag -- distinguishes a truthful refusal from a
 * false negative: a probe that failed, an entry with no VM_PROT_EXECUTE, and an entry DIFFERENT from the one
 * vm_protect changed are three different answers, and only two of them mean the page cannot be executable. */
typedef struct {
    kern_return_t kr;       /* vm_region_recurse_64's own return code */
    vm_address_t  start;    /* start of the entry the kernel named */
    vm_size_t     size;     /* ...and its length */
    natural_t     depth;    /* submap nesting depth the kernel reported */
    vm_prot_t     prot, max;
    uint32_t      tag;
} xmem_entry;

static xmem_entry xmem_probe_entry(uintptr_t p)
{
    xmem_entry e; memset(&e, 0, sizeof(e));
    vm_address_t addr = (vm_address_t)p; vm_size_t sz = 0;
    natural_t depth = 0; vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    e.kr = vm_region_recurse_64(mach_task_self(), &addr, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt);
    if (e.kr == KERN_SUCCESS) {
        e.start = addr; e.size = sz; e.depth = depth;
        e.prot = info.protection; e.max = info.max_protection; e.tag = info.user_tag;
    }
    return e;
}

/*
 * The fetch itself: write a movz/ret through the writable view at `write_at` and call it at `fetch_at`, under the same
 * sigsetjmp guard self_runs uses, so a page that names execute but will not fetch is caught rather than fatal. Only a
 * real fetch answers the question. `write_at` is the guest's own arena, so its bytes are saved and put back whether the
 * call returns or faults; nothing is unmapped or left behind. The writes are bracketed with the W^X toggle so the store
 * lands on a MAP_JIT view (writable) and the call happens in execute mode.
 */
static bool xmem_write_fetch(uintptr_t write_at, uintptr_t fetch_at, int *stage)
{
    static const uint32_t code[2] = { 0x528B4B40u, 0xD65F03C0u };   /* movz w0, #0x5a5a ; ret */
    uint32_t saved[2];
    memcpy(saved, (const void *)write_at, sizeof(saved));
    struct sigaction sa, ob, os, oi;
    memset(&sa, 0, sizeof(sa)); sa.sa_sigaction = self_fault_info; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &ob); sigaction(SIGSEGV, &sa, &os); sigaction(SIGILL, &sa, &oi);
    bool ok = false;
    g_self_running = 1;
    g_self_sig = g_self_code = 0;
    g_self_stage = 0;
    if (sigsetjmp(g_self_jump, 1) == 0) {
        xmem_write_mode(true);
        g_self_stage = 1;                                    /* the store into the write view */
        memcpy((void *)write_at, code, sizeof(code));
        g_self_stage = 2;                                    /* the fetch of what was stored */
        xmem_write_mode(false);
        sys_icache_invalidate((void *)fetch_at, sizeof(code));
        ok = ((int (*)(void))(void *)fetch_at)() == 0x5a5a;
    }
    g_self_running = 0;
    sigaction(SIGBUS, &ob, NULL); sigaction(SIGSEGV, &os, NULL); sigaction(SIGILL, &oi, NULL);
    /* Put the guest's bytes back ONLY if the store landed: a store that itself faulted (W^X) must not be retried
     * outside the guard, and nothing was changed then anyway. */
    if (g_self_stage != 1) {
        xmem_write_mode(true);
        memcpy((void *)write_at, saved, sizeof(saved));
        xmem_write_mode(false);
        sys_icache_invalidate((void *)write_at, sizeof(saved));
    }
    if (stage) *stage = (int)g_self_stage;
    return ok;
}

/* Write the stub at `at` and call it at `at` (the same view named twice): the question a single address answers. */
static bool xmem_writable_fetches(uintptr_t at, int *stage)
{
    return xmem_write_fetch(at, at, stage);
}

#if defined(__APPLE__) && !TARGET_OS_OSX
/*
 * Hand the guest the StikDebug EXECUTABLE view and PROVE it fetches -- WITHOUT EVER ASKING FOR RWX.
 *
 * This used to raise the entry's maximum to RWX and vm_protect the whole view to RWX, then probe it: the ask for RWX is
 * what collapses the entry to `prot 1 max 3` and revokes the debugger's execute grant (two-run control:
 * jit-grant-and-detach.md). It now makes NO protection-changing call on this region, ever:
 *
 *   1. read the entry back raw (diagnostic only -- the field, not the pmap);
 *   2. write a movz/ret through the WRITABLE twin and CALL it at the executable view, under the same signal guard
 *      self_runs uses (xmem_write_fetch) -- the boot selftest's own shape.
 *
 * The guest is given the executable view (RX), the same view b_mmap hands a guest asking for code memory. Because it is
 * RX, a guest STORE into it faults and is carried through the writable twin by the JIT guard (husk-tl-codewrite.c),
 * never by making the page writable. The verdict is that call, carried out in `err`. Nothing is unmapped, no thread
 * ended.
 */
static bool adopt_exec_view(tl_dual_mapping *m, char *err, size_t errlen)
{
    xmem_entry e = xmem_probe_entry((uintptr_t)m->rx_addr);
    int stage = 0;
    /* Write through the debugger's writable view, call at the executable one: the boot selftest's shape. NO vm_protect
     * precedes it. */
    bool fetched = xmem_write_fetch((uintptr_t)m->rw_addr, (uintptr_t)m->rx_addr, &stage);
    g_x.rx = m->rx_addr;             /* guest code executes here */
    g_x.rw = m->rx_addr;             /* the ONE address the guest both writes and executes (RX) */
    g_x.wrw = m->rw_addr;            /* the writable twin: the layer's bulk emission AND every guest store */
    g_x.size = m->size;
    g_x.guest_view_jit = false;      /* it is RX, not an authorised-RWX view */
    g_x.wx_fallback = true;
    g_x.route = "the executable view, handed to the guest as RX (stores into it are carried through the writable twin; no page of this region is ever asked for RWX)";
    tl_log_line("xmem: the executable view %p: read-back %s (entry %p..%p prot %d max %d depth %u tag %u); the guarded write through the writable twin %p then call at the executable view %s. NO vm_protect is issued on this region, so the debugger's per-entry execute grant cannot be revoked by the layer",
                (void *)m->rx_addr, e.kr == KERN_SUCCESS ? "ok" : "FAILED",
                (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag,
                (void *)m->rw_addr,
                fetched ? "RAN -- the executable view is genuinely executable here; the guest's arena will run, and a store into it is carried through the writable twin"
                        : "FAULTED -- the executable view does not fetch, so the guest's arena cannot run");
    if (!fetched) {
        snprintf(err, errlen, "the executable view names no execute and will not run (entry %p..%p prot %d max %d depth %u tag %u; the guarded write through the writable twin then call at it %s, signal %d si_code %d) -- and the layer will NOT ask for RWX to change that, because that ask is what revoked the grant",
                 (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag,
                 stage == 1 ? "FAULTED ON THE STORE (unexpected: the twin should be writable)" : "FAULTED ON THE FETCH",
                 (int)g_self_sig, (int)g_self_code);
        xmem_note_exec(TL_XMEM_EXEC_FAULTED, err);
        return true;                 /* the region is all there is; the honest refusal rides in the verdict */
    }
    xmem_note_exec(TL_XMEM_EXEC_RAN, NULL);
    return true;
}

/*
 * One guarded write-then-execute, naming what was happening when it ran: call it BEFORE the layer's first
 * protection-changing call and after each one, and the pair of lines names the moment the fetch stops running -- or
 * proves none of our calls is the one (both RAN -> the grant was taken outside the layer). `write_at`/`fetch_at` are the
 * two views of one page, or one view named twice to ask whether a single view both takes a write and executes.
 */
bool tl_xmem_exec_gate(const char *after, void *write_at, void *fetch_at)
{
    if (!write_at || !fetch_at) return false;
    int stage = 0;
    bool ran = xmem_write_fetch((uintptr_t)write_at, (uintptr_t)fetch_at, &stage);
    tl_log_line("xmem: EXEC GATE after %s: a stub written through %p and called at %p -> %s",
                after ? after : "(unnamed)", write_at, fetch_at,
                ran ? "RAN -- execute is still there"
                    : stage == 1 ? "FAULTED ON THE STORE (the write view is not writable)"
                                 : "FAULTED ON THE FETCH -- NO EXECUTE at that address now");
    return ran;
}
#endif


/*
 * The one line that must end the guessing: the arena's own address and size, how many MiB of it the kernel says are MAPPED
 * and how many EXECUTABLE, which route gave the guest its view, the delta between the views, and separately whether the
 * guest-facing view really fetched a function written into it. The old walk stopped at 128 entries, which is why a 512 MiB
 * region could read as "2 MiB mapped" and hide an unbacked page; this walks the whole span.
 */
static void xmem_report_region(void)
{
    uintptr_t a = (uintptr_t)g_x.rx, end = a + g_x.size;
    size_t mapped = 0, execut = 0;
    unsigned entries = 0, gaps = 0, noexec = 0;
    while (a < end && entries < 200000) {
        xmem_entry e = xmem_probe_entry(a);
        if (e.kr != KERN_SUCCESS) {
            tl_log_line("xmem: arena walk at %p: vm_region_recurse_64 returned %d (%s) -- no entry there (a hole)", (void *)a, (int)e.kr, mach_error_string(e.kr));
            gaps++;
            break;
        }
        if (e.size == 0) break;
        if (e.start != a) gaps++;                    /* an unmapped gap before this entry */
        mapped += e.size;
        if (e.prot & VM_PROT_EXECUTE) execut += e.size;
        else { noexec++; if (noexec <= 4) tl_log_line("xmem: arena entry %p..%p prot %d max %d depth %u tag %u -- NOT EXECUTABLE", (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag); }
        a = e.start + e.size;
        entries++;
    }
    tl_log_line("xmem: arena walk: %d entries, %zu MiB mapped, %zu MiB executable, %d NOT executable (the first four are listed above; this line exists because this region has tens of thousands of them)", entries, mapped >> 20, execut >> 20, noexec);
    tl_log_line("xmem: arena %p..%p: %zu MiB, %zu MiB MAPPED, %zu MiB executable, guest-facing view %p is %s, delta %td -- %s",
                (void *)g_x.rx, (void *)end, g_x.size >> 20, mapped >> 20, execut >> 20, (void *)g_x.rw,
                g_x.route ? g_x.route : "? (the guest view has no authorisation of its own)", tl_xmem_delta(),
                (gaps || noexec || a < end)
                    ? "a fetch in an unmapped or non-executable entry is a requestless fault, not a permission the guest asked for"
                    : "the whole arena is mapped and executable");
    /* When execute is named here the grant is intact and the arena will run; when it is not, the layer's own ask for RWX
     * is the prime cause and this build no longer makes it. The guarded fetch printed next is the verdict either way, and
     * the EXEC GATE pair above names the moment it changed (the two-run control is in jit-grant-and-detach.md). */
    if (execut == 0)
        tl_log_line("xmem: the walk above names NO execute anywhere in the executable view. That is the state the layer's own vm_protect(RWX) used to leave behind: the run that rendered this game read `512 MiB executable` (build b0c3965, no such call) and the first build to open an arena with that call read `0 MiB executable` (build 18da8b3). This build issues no vm_protect on this region, so if execute is still absent here, the grant was gone before the arena opened -- read the EXEC GATE pair above, which is what says when");
    if (g_x.wx_fallback)
        tl_log_line("xmem: the guest-facing view is the EXECUTABLE view %p (RX); the layer writes through the writable twin %p (delta %td) and carries a guest store into the executable view through that twin in the JIT guard. 'resident but not executable' entries above are the executable view's own entries -- the field, and the pmap agrees with it here",
                    (void *)g_x.rw, (void *)g_x.wrw, (ptrdiff_t)(g_x.wrw - g_x.rx));
}

/* Ask the guest-facing view the question the guest will ask it: write a stub there and call it there, guarded, once per
 * open on bytes nothing has written yet. The read-back is printed beside it, raw. */
static void xmem_probe_guest_view(void)
{
    uintptr_t at = (uintptr_t)g_x.rw;
    xmem_entry e = xmem_probe_entry(at);
    if (g_x.wx_fallback) {
        /* The guest's view is the EXECUTABLE one (RX), so a store into it faults (the codewrite guard carries it through
         * the writable twin). The question "does the guest's view FETCH" is answered the boot selftest's way: write the
         * stub through the WRITABLE TWIN and call it at the guest's address -- WITHOUT any vm_protect, which is the fix.
         * If it faults on the fetch, the FETCH itself is what the device refuses, and no store service can change that. */
        int stage = 0;
        bool f = xmem_write_fetch((uintptr_t)xmem_writable_view(), at, &stage);
        xmem_note_exec(f ? TL_XMEM_EXEC_RAN : TL_XMEM_EXEC_FAULTED,
                       f ? NULL
                         : stage == 1 ? "the writable twin could not be written (unexpected: it should be)"
                                      : "the executable view names no execute and will not run: the guarded write through the writable twin then call at the guest's address FAULTED ON THE FETCH, and no store service can change that because the FETCH is what is refused (the layer issued no vm_protect here -- see adopt_exec_view for what did revoke it)");
        tl_log_line("xmem: guest-facing view %p IS the EXECUTABLE view (RX): read-back %s (entry %p..%p prot %d max %d depth %u tag %u), guarded write through the writable twin %p then call at %p %s",
                    (void *)at, e.kr == KERN_SUCCESS ? "ok" : "FAILED",
                    (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag,
                    (void *)xmem_writable_view(), (void *)at,
                    f ? "RAN -- the executable view is genuinely executable here, so the guest's arena will run; a store into it is carried through the writable twin by the JIT guard, and the executable page stays executable"
                      : stage == 1 ? "FAULTED writing the writable twin (unexpected: the twin should be writable)"
                                   : "FAULTED ON THE FETCH -- the executable view does not fetch. The layer's own ask for RWX is the measured cause of this state, and this build no longer makes that ask; if this line is still here, the grant was gone before the arena opened (see the EXEC GATE pair above)");
        return;
    }
    int stage = 0;
    bool f = xmem_writable_fetches(at, &stage);
    xmem_note_exec(f ? TL_XMEM_EXEC_RAN : TL_XMEM_EXEC_FAULTED,
                   f ? NULL
                     : stage == 1 ? "the guest cannot write at the address it will execute (W^X): the guarded write-then-execute FAULTED ON THE STORE"
                                  : "the guest's arena names RWX but will not run: the guarded write-then-execute FAULTED ON THE FETCH");
    tl_log_line("xmem: guest-facing view %p: read-back %s (entry %p..%p prot %d max %d depth %u tag %u), guarded write-then-execute %s%s", 
                (void *)at, e.kr == KERN_SUCCESS ? "ok" : "FAILED",
                (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag,
                f ? "RAN -- the guest's JIT is live at this address"
                  : stage == 1 ? "FAULTED ON THE STORE -- the guest cannot write here (W^X)"
                               : "FAULTED ON THE FETCH -- the guest's arena names RWX but will not run",
                f ? "" : " (a guarded fault is caught, not a dead process)");
}

/* ------------------------------------------- the three verdicts: ONE body, shared by BOTH probes */

/*
 * ONE page's three verdicts and the pmap entry that holds them -- the SAME body the open-time and fatal-time probes both
 * call, so a page is never judged by two pieces of code. `offset` is the page's offset from the executable view's base
 * (tl_xmem_region_base()), the unit tl_xmem_alloc's bump span is measured in, so an open-time offset and a fatal fault's
 * offset are directly comparable. Guarded probes only. The CALL is taken ONLY if the writable view read the page,
 * because xmem_write_fetch saves the guest's bytes with an UNGUARDED read before it arms the guard.
 */
typedef enum {
    XMEM_PAGE_UNBACKED,             /* both reads FAULTED: the page has no backing at this offset */
    XMEM_PAGE_READABLE_NOT_EXEC,    /* both reads READ, the guarded CALL did not                     */
    XMEM_PAGE_READABLE_AND_EXEC,    /* all three verdicts ok                                        */
    XMEM_PAGE_MIXED                 /* the two reads disagree, or the stub store was refused        */
} xmem_page_class;

typedef struct {
    xmem_entry      e;              /* the page's map entry, the fields the fatal report line prints */
    uintptr_t       page, twin;     /* the page in the EXECUTABLE view and in the WRITABLE view      */
    bool            xok, wok, ran;  /* read through rx, read through the twin, guarded call of stub  */
    uint8_t         xb, wb;         /* the bytes the two reads returned                              */
    int             xsig, xcode, wsig, wcode, stage;
    xmem_page_class cls;
} xmem_three;

static const char *xmem_page_class_name(xmem_page_class c)
{
    switch (c) {
    case XMEM_PAGE_UNBACKED:          return "UNBACKED (both reads faulted at this offset)";
    case XMEM_PAGE_READABLE_NOT_EXEC: return "READABLE-NOT-EXECUTABLE (the reads served the page, the guarded CALL did not)";
    case XMEM_PAGE_READABLE_AND_EXEC: return "READABLE-AND-EXECUTABLE (all three verdicts ok)";
    default:                          return "MIXED/UNEXPECTED (the two reads disagree, or the stub store was refused)";
    }
}

static const char *xmem_x_verdict(const xmem_three *t)
{
    return t->xok ? "READ -- the executable view served the page at the fault offset"
                  : "FAULTED -- the executable view has no readable backing at the fault offset";
}

static const char *xmem_w_verdict(const xmem_three *t)
{
    return t->wok ? "READ -- the writable view served the same page"
                  : "FAULTED -- the writable view has no readable backing at the same page";
}

static const char *xmem_c_verdict(const xmem_three *t)
{
    return !t->wok
        ? "NOT ATTEMPTED -- the writable view could not read this page, so the page is unbacked here (the stub is written through that view and xmem_write_fetch saves the guest's bytes with an unguarded read; nothing was attempted rather than end the process)"
        : t->ran
            ? "RAN -- the executable view fetched the stub, so this page is executable"
            : t->stage == 1 ? "FAULTED ON THE STORE -- the writable view would not take the stub (the page is not writable through the twin)"
                            : "FAULTED ON THE FETCH -- the page is readable but NOT executable (the per-16-KiB execute preparation did not take here)";
}

static void xmem_take_three(size_t offset, xmem_three *t)
{
    memset(t, 0, sizeof(*t));
    t->page = (uintptr_t)g_x.rx + offset;
    t->twin = (uintptr_t)xmem_writable_view() + offset;
    t->e    = xmem_probe_entry(t->page);
    t->xok  = xmem_guarded_read((const unsigned char *)t->page, &t->xb, &t->xsig, &t->xcode);
    t->wok  = xmem_guarded_read((const unsigned char *)t->twin, &t->wb, &t->wsig, &t->wcode);
    if (t->wok) t->ran = xmem_write_fetch(t->twin, t->page, &t->stage);
    if (!t->xok && !t->wok) t->cls = XMEM_PAGE_UNBACKED;
    else if (!t->xok || !t->wok) t->cls = XMEM_PAGE_MIXED;
    else if (t->ran) t->cls = XMEM_PAGE_READABLE_AND_EXEC;
    else t->cls = XMEM_PAGE_READABLE_NOT_EXEC;
}

/* The open-time probe's verdict, kept so the fatal-time probe can say MATCH or DIFFER against it. */
static size_t          g_open_page_offset;
static xmem_page_class g_open_page_class;
static bool            g_open_page_have;

/*
 * The deterministic fault offset, measured exhaustively on ONE page, ONCE per open. Every previous read-back tested a
 * span's first page; nothing had tested the page the guest actually dies on, nor separated "unbacked there" from
 * "readable but not executable there". This does both and prints the pmap's own entry on the same line. The offset is
 * the OBSERVED fault offset (robot gf's fatal SIGBUS is always at arena base + 0xb589b90), not a guess.
 *
 * Three measurements on that one page: (1) a guarded READ through the EXECUTABLE view; (2) a guarded READ through the
 * WRITABLE view (the control); (3) a guarded CALL of a movz/ret stub at that offset, written through the writable view
 * and called through the executable view. The CALL is taken ONLY if measure 2 read the page (see xmem_take_three).
 *
 * Both reads faulted = UNBACKED; reads served but the CALL faulted on the fetch = READABLE-NOT-EXECUTABLE; all three ok
 * = readable AND executable. Nothing is relaxed: no vm_protect, the executable view is never written, nothing unmapped,
 * no thread ended. Runs once at the open, never on an alloc.
 */
static void xmem_probe_fault_offset(void)
{
    static const size_t TL_XMEM_FAULT_OFFSET = (size_t)0xb589b90u;   /* the observed fault offset: arena base + 0xb589b90, every run */
    xmem_three t;
    xmem_take_three(TL_XMEM_FAULT_OFFSET, &t);   /* the SAME body the fatal-time probe uses: guarded, nothing relaxed */
    /* Keep THIS page's verdict so the fatal-time probe can say whether the same three verdicts still hold when the guest
     * faults. */
    g_open_page_offset = TL_XMEM_FAULT_OFFSET;
    g_open_page_class  = t.cls;
    g_open_page_have   = true;
    tl_log_line("xmem: the deterministic fault offset +%#lx (arena base %p + the observed fault offset, one page measured once): the page's entry as the pmap walk reports it %p..%p prot %d max %d depth %u tag %u -- the SAME fields the fatal line prints; and the three verdicts on this ONE page: guarded READ through the EXECUTABLE view %p -> %s (byte 0x%02x, signal %d si_code %d); guarded READ through the WRITABLE view %p -> %s (byte 0x%02x, signal %d si_code %d); guarded CALL of a movz/ret stub there (written through the writable view %p, flushed, called at the executable view %p) -> %s -- so UNBACKED (both reads faulted) vs READABLE-NOT-EXECUTABLE (reads ok, CALL faulted on the fetch) vs READABLE-AND-EXECUTABLE (all three ok) is named by these three verdicts; no vm_protect was issued, the executable view was not written, nothing was unmapped and no thread was ended",
                (unsigned long)TL_XMEM_FAULT_OFFSET, (void *)g_x.rx,
                (void *)t.e.start, (void *)(t.e.start + t.e.size), (int)t.e.prot, (int)t.e.max, (unsigned)t.e.depth, t.e.tag,
                (void *)t.page, xmem_x_verdict(&t), (unsigned)t.xb, t.xsig, t.xcode,
                (void *)t.twin, xmem_w_verdict(&t), (unsigned)t.wb, t.wsig, t.wcode,
                (void *)t.twin, (void *)t.page, xmem_c_verdict(&t));
}

/*
 * THE FATAL-TIME MEASUREMENT: the SAME three verdicts the open-time probe takes, on the page the guest ACTUALLY faulted
 * on, ONCE per fault, on a NORMAL THREAD, printed on ONE line with a plain MATCH / DIFFER against the open-time ones.
 *
 * Why not in the handler: on_fatal is a signal handler for the very signal being handled, and installing a sigsetjmp
 * guard and longjmping out of it THERE is re-entrancy risk. The handler only RECORDS the fault
 * (tl_xmem_note_guest_fault); this runs LATER on the normal thread that runs the app-fault/session-close path
 * (husk_tl_session_close). A no-op unless the arena is open and an unreported guest fault was recorded.
 *
 * THE COMPARISON IS THE POINT: the open-time probe measured this page ONCE at arena open. If its backing or its execute
 * grant was lost in between (a later vm_protect, a debugger detach, a jetsam/CoW event) the verdicts here differ, and
 * the line says so in one word instead of leaving a human to diff two log lines. Nothing is relaxed.
 */
void tl_xmem_report_fault_time(void)
{
    if (!g_x.open || !g_fault_have || g_fault_reported) return;   /* only for an unreported fault, once */
    g_fault_reported = true;                                      /* ONE measurement per fault, whatever follows */
    uintptr_t base = (uintptr_t)g_x.rx;
    uintptr_t addr = g_fault_addr;
    size_t    off  = (size_t)(addr - base);
    xmem_three t;
    xmem_take_three(off, &t);
    char cmp[600];
    if (!tl_xmem_contains((const void *)addr)) {
        snprintf(cmp, sizeof(cmp),
                 "NOT COMPARABLE -- the recorded fault address %p is not inside the arena, so there is no page here to compare with the open-time verdicts",
                 (void *)addr);
    } else if (!g_open_page_have || off != g_open_page_offset) {
        snprintf(cmp, sizeof(cmp),
                 "NOT COMPARABLE -- the open-time probe measured the fixed offset +%#lx (the deterministic fault offset); this fault is at offset +%#lx, so they are DIFFERENT pages and the open-time verdicts do not apply here",
                 g_open_page_have ? (unsigned long)g_open_page_offset : 0ul, (unsigned long)off);
    } else if (t.cls == g_open_page_class) {
        snprintf(cmp, sizeof(cmp),
                 "MATCH -- the fatal-time verdicts are the SAME as the open-time ones seen at arena open (both say %s), so the page lost NOTHING between arena open and this fatal access",
                 xmem_page_class_name(t.cls));
    } else {
        snprintf(cmp, sizeof(cmp),
                 "DIFFER -- THIS IS THE TRANSIENT: at arena open the page was %s, at this fatal access it is %s -- the page lost its backing or its execute grant between arena open and the fatal access (a later vm_protect, a debugger detach costing the per-entry grant, or a jetsam/CoW event)",
                 xmem_page_class_name(g_open_page_class), xmem_page_class_name(t.cls));
    }
    tl_log_line("husk: FATAL-TIME arena page verdicts (taken ONCE per fault, on this normal thread -- NOT in the crash reporter's signal handler, so no sigsetjmp guard is installed for the signal being handled): fault address %p, offset +%#lx from the arena base %p; the page's entry %p..%p prot %d max %d depth %u tag %u -- the SAME fields the fatal report line prints; and the three verdicts on this ONE page: guarded READ through the EXECUTABLE view %p -> %s (byte 0x%02x, signal %d si_code %d); guarded READ through the WRITABLE view %p -> %s (byte 0x%02x, signal %d si_code %d); guarded CALL of a movz/ret stub there (written through the writable view %p, flushed, called at the executable view %p) -> %s; VS the open-time verdicts at arena open (offset +%#lx): %s; no vm_protect was issued, the executable view was not written, nothing was unmapped and no thread was ended",
                (void *)addr, (unsigned long)off, (void *)base,
                (void *)t.e.start, (void *)(t.e.start + t.e.size), (int)t.e.prot, (int)t.e.max, (unsigned)t.e.depth, t.e.tag,
                (void *)t.page, xmem_x_verdict(&t), (unsigned)t.xb, t.xsig, t.xcode,
                (void *)t.twin, xmem_w_verdict(&t), (unsigned)t.wb, t.wsig, t.wcode,
                (void *)t.twin, (void *)t.page, xmem_c_verdict(&t),
                g_open_page_have ? (unsigned long)g_open_page_offset : 0ul,
                cmp);
}

static void xmem_report_open(void)
{
    if (!g_x.open) return;
    xmem_report_region();
    xmem_probe_guest_view();
    xmem_probe_fault_offset();   /* the one page nothing had measured: the observed fault offset (once per open) */
}
#else
/* No mach probe on a host that is not an Apple one: nothing to report. */
static void xmem_report_open(void) {}
void tl_xmem_report_fault_time(void) {}
#endif

#if !defined(__APPLE__) || TARGET_OS_OSX
/* A host with no mach probes: the gate cannot measure anything here, and says so rather than lying.
 * It is defined OUTSIDE the Apple block above so that a host build (tools/, the CI compile gate)
 * still links the symbol the header exports. */
bool tl_xmem_exec_gate(const char *after, void *write_at, void *fetch_at)
{
    (void)write_at; (void)fetch_at;
    tl_log_line("xmem: EXEC GATE after %s: not measurable on this host (the guarded probe is a mach one)", after ? after : "(unnamed)");
    return false;
}
#endif

/*
 * Make the writable view's pages executable where the guest named them with PROT_EXEC. A JIT (the game's own
 * A64HookFunction) writes a trampoline into memory it holds and calls it by the address that view gave it (the loader
 * retargets its adrp at the writable view -- the page has no TL_PAGE_X, husk-tl-ld.c). The executable view of the same
 * pages already runs, but the guest holds the guest-facing one, so execute must be added to THAT view. Fix A makes it
 * the view carrying the JIT authorisation (make_views); where the kernel will not grant that it is a vm_remap alias and
 * adding execute is legal-looking but ineffective.
 *
 * The verdict is the GUARDED FETCH; the read-back is a diagnostic -- the phone's runs are self-contradictory for it
 * (vm_protect returned KERN_SUCCESS and the read-back still did not name execute; xmem-arena-and-wx.md). The old code
 * only ran the fetch when the read-back agreed, so a lying read-back was indistinguishable from a page that will not
 * fetch. Now the fetch always runs and is the answer. It is guarded (sigsetjmp + SIGBUS/SIGSEGV/SIGILL), and this
 * device delivers an unblessed fetch as a CATCHABLE SIGBUS, so a page that will not fetch is a caught fault and an
 * honest EACCES, never a dead process. TL_XMEM_NO_FETCH_VERDICT=1 restores the old gating. TL_NO_XMEM_RW_EXEC=1 never
 * adds execute, to force the store-emulation fallback.
 */
bool tl_xmem_allow_exec(void *p, size_t n)
{
    static int no_rw_exec = -1;
    if (no_rw_exec < 0) no_rw_exec = getenv("TL_NO_XMEM_RW_EXEC") ? 1 : 0;
    if (no_rw_exec || !g_x.open || !p) return false;
    /* The guest can name either view: the guest-facing view (g_x.rw), or -- in the W^X fallback,
     * where g_x.rw IS the executable view -- the writable twin a guest JIT derives an address from.
     * Accept either, and work on the one it actually named. */
    uintptr_t a = (uintptr_t)p;
    uintptr_t rwbase = (uintptr_t)g_x.rw, wbase = (uintptr_t)xmem_writable_view();
    bool in_rw = a >= rwbase && a < rwbase + g_x.size;
    bool in_w  = a >= wbase  && a < wbase  + g_x.size;
    if (!in_rw && !in_w) return false;
    uintptr_t base = in_rw ? rwbase : wbase, end = base + g_x.size;
    uintptr_t page = a & ~(uintptr_t)(TL_XMEM_PAGE - 1);
    uintptr_t last = (a + n + TL_XMEM_PAGE - 1) & ~(uintptr_t)(TL_XMEM_PAGE - 1);
    if (last > end) last = end;
    size_t len = last > page ? last - page : TL_XMEM_PAGE;
    bool ok;
    char why[200];
#if defined(__APPLE__)
    /* NOTHING HERE ASKS FOR RWX, AND THAT IS THE POINT. It used to be vm_protect(page, RWX); that ask is what revoked
     * the debugger's per-entry execute grant for the whole region (jit-grant-and-detach.md), so the layer no longer
     * issues it anywhere. What is left is a MEASUREMENT: write a stub through the view that is writable at this address
     * and call it at the address the guest will fetch from. Two shapes:
     *   - the guest named the EXECUTABLE view: the stub is written through the writable twin at +delta and called at the
     *     guest's address (what the boot selftest and the arena open already measured);
     *   - the guest named the WRITABLE view (a vm_remap alias): written in place and called there, which faults -- iOS
     *     authorises execute per mapping ENTRY and an alias never inherits it. That is a refusal, and it is EACCES. */
    xmem_entry e = xmem_probe_entry(page);
    bool in_exec = page >= (uintptr_t)g_x.rx && page + len <= (uintptr_t)g_x.rx + g_x.size;
    uintptr_t write_at = in_exec ? page + (uintptr_t)tl_xmem_delta() : page;
    bool fetched = xmem_write_fetch(write_at, page, NULL);
    ok = fetched;
    if (fetched)
        snprintf(why, sizeof(why), "the guarded write-then-execute RAN at the guest's own address (%p)", (void *)page);
    else if (!in_exec)
        snprintf(why, sizeof(why), "the guest named %p in the WRITABLE view (entry %p..%p prot %d max %d depth %u tag %u): a stub written in place there and called at that same address FAULTED (signal %d, si_code %d) -- iOS authorises execute per mapping ENTRY and this alias has none; the same page DOES execute at %p (delta %td). No vm_protect is issued to paper over that, because the ask for RWX is what revokes the grant",
                 (void *)page, (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag,
                 (int)g_self_sig, (int)g_self_code, (void *)(page - (uintptr_t)tl_xmem_delta()), tl_xmem_delta());
    else
        snprintf(why, sizeof(why), "the guest named %p in the EXECUTABLE view (entry %p..%p prot %d max %d depth %u tag %u) and it does not fetch (signal %d, si_code %d): the debugger's execute grant is gone for this region and no vm_protect can bring it back -- EACCES is the honest answer",
                 (void *)page, (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag,
                 (int)g_self_sig, (int)g_self_code);
#else
    ok = mprotect((void *)page, len, PROT_READ | PROT_EXEC) == 0;
    snprintf(why, sizeof(why), "%s", ok ? "" : strerror(errno));
#endif
    /* A success is never silent, and a refusal always names which of the two shapes it was. */
    if (!ok) {
        static int said;
        if (said++ < 4)
            tl_log_line("husk: xmem exec refused at %p (%#zx bytes): %s", p, n, why);
        if (tl_set_guest_errno) tl_set_guest_errno(13);   /* EACCES: an honest failure, not a lie */
        return false;
    }
    tl_log_line("husk: xmem exec: the guest's page at %p (%#zx bytes) is executable and was MEASURED so (a stub written through the writable view at %td and called at the guest's address RAN); no vm_protect was issued on the region, on this page or any other", p, n, (ptrdiff_t)tl_xmem_delta());
    tl_xmem_flush(p, n);
    return true;
}

size_t tl_xmem_used(void) { return g_x.used; }
size_t tl_xmem_size(void)  { return g_x.size; }
const void *tl_xmem_region_base(void) { return g_x.rx; }

bool tl_xmem_guest_view_is_jit(void) { return g_x.open && g_x.guest_view_jit; }

/* The guarded write-then-execute's verdict, for the app's indicator: a measurement, not a flag. */
int tl_xmem_exec_verdict(void) { return g_x.exec_measured; }
const char *tl_xmem_exec_detail(void) { return g_x.exec_detail[0] ? g_x.exec_detail : NULL; }

/* True only in the W^X fallback: the guest was handed the executable view but it could not be held writable-and-
 * executable, so a store into it is serviced page by page (below). */
bool tl_xmem_wx_fallback(void) { return g_x.open && g_x.wx_fallback; }

/*
 * The per-page protection bracket that used to live here (vm_protect(RW) then vm_protect(RX) on a page of the guest's
 * arena) is GONE, and its removal is the fix: a vm_protect that names WRITE is the same ask that revoked the debugger's
 * grant (adopt_exec_view), and a store into the executable view does not need it -- the value goes through the writable
 * twin and the executable page is left executable (husk-tl-codewrite.c never flips a page).
 */

/* The layer's own emission into the region, bracketed so a device that enforces W^X lets the store and the fetch both
 * happen. Only the Fix A order matters; a no-op everywhere else. */
void tl_xmem_emit_begin(void) { if (g_x.guest_view_jit) xmem_write_mode(true); }
void tl_xmem_emit_end(void)   { if (g_x.guest_view_jit) xmem_write_mode(false); }

/* ============================================================ the verdict, persisted
 *
 * The measurement is useless if it cannot be read: a guest fault can take the PROCESS down, with it the UI row that
 * would have shown why. So the verdict is written to a file in the app's container at measurement time, and the
 * Settings "Guest arena" row reads the last one on the next launch -- the fact is recorded while it is known, because
 * nobody is alive to ask afterwards. The path is the app's to give (tl_xmem_set_verdict_path); empty writes nothing.
 */
static char g_verdict_path[1024];

void tl_xmem_set_verdict_path(const char *path)
{
    snprintf(g_verdict_path, sizeof(g_verdict_path), "%s", path ? path : "");
}

#if defined(__APPLE__)
/* What the file holds, kept so a later guest fault can rewrite the WHOLE record -- the measurement
 * AND why the run died -- without re-reading the file it just wrote. */
static struct {
    uintptr_t arena;
    bool      have_entry;
    uintptr_t entry_start, entry_end;
    int       prot, maxprot;
    unsigned  depth, tag;
    char      fault[240];
    uintptr_t fault_pc;
    long long fault_when;
} g_verdict_rec;

/* Write the whole record, to a sibling `.tmp` then renamed into place, so a reader never sees a half-written file (the
 * layer writes its prefs this way too; husk-tl-prefs.c). */
static void xmem_write_verdict_file(void)
{
    if (!g_verdict_path[0]) return;
    char tmp[1120];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_verdict_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        tl_log_line("xmem: the arena verdict could not be written to %s: %s (the row will read whatever is already there)", g_verdict_path, strerror(errno));
        return;
    }
    fprintf(f, "husk-arena-verdict 1\n");
    fprintf(f, "verdict %s\n", g_x.exec_measured == TL_XMEM_EXEC_RAN ? "ran" : "faulted");
    fprintf(f, "when %lld\n", (long long)time(NULL));
    fprintf(f, "arena %#llx\n", (unsigned long long)g_verdict_rec.arena);
    if (g_verdict_rec.have_entry)
        fprintf(f, "entry %#llx %#llx prot %d max %d depth %u tag %u\n",
                (unsigned long long)g_verdict_rec.entry_start, (unsigned long long)g_verdict_rec.entry_end,
                g_verdict_rec.prot, g_verdict_rec.maxprot, g_verdict_rec.depth, g_verdict_rec.tag);
    else
        fprintf(f, "entry none\n");
    fprintf(f, "reason %s\n", g_x.exec_detail[0] ? g_x.exec_detail
                              : g_x.exec_measured == TL_XMEM_EXEC_RAN ? "(the guarded write-then-execute ran)"
                                                                      : "(no reason recorded)");
    if (g_verdict_rec.fault[0]) {
        fprintf(f, "fault %s\n", g_verdict_rec.fault);
        fprintf(f, "faultpc %#llx\n", (unsigned long long)g_verdict_rec.fault_pc);
        fprintf(f, "faultwhen %lld\n", g_verdict_rec.fault_when);
    }
    fflush(f);
    if (fclose(f) != 0 || rename(tmp, g_verdict_path) != 0) {
        tl_log_line("xmem: the arena verdict file %s could not be put in place: %s", g_verdict_path, strerror(errno));
        remove(tmp);
        return;
    }
    tl_log_line("xmem: arena verdict %s persisted to %s at measurement time (the app reads it on the next launch)",
                g_x.exec_measured == TL_XMEM_EXEC_RAN ? "RAN" : "FAULTED", g_verdict_path);
}

/* The hook xmem_note_exec calls: fill the record with what was just measured plus the arena's map entry, then write. */
static void xmem_persist_verdict(void)
{
    if (!g_verdict_path[0]) return;
    g_verdict_rec.arena = (uintptr_t)g_x.rw;
    xmem_entry e = xmem_probe_entry((uintptr_t)g_x.rw);
    g_verdict_rec.have_entry = (e.kr == KERN_SUCCESS && e.size != 0);
    if (g_verdict_rec.have_entry) {
        g_verdict_rec.entry_start = (uintptr_t)e.start;
        g_verdict_rec.entry_end   = (uintptr_t)(e.start + e.size);
        g_verdict_rec.prot = (int)e.prot; g_verdict_rec.maxprot = (int)e.max;
        g_verdict_rec.depth = (unsigned)e.depth; g_verdict_rec.tag = e.tag;
    }
    xmem_write_verdict_file();
}

/*
 * Whether this fault is the GUEST failing in the region it runs from, where ALL of the guest's code and its JIT arena
 * live (the loader emits the app's libraries there and tl_xmem_alloc carves the arena from it), so a fault at such an
 * address is the APP's memory failing, not Husk's. The crash reporter treats it as a guest failure -- the app is
 * unwound and Husk stays -- rather than a host crash.
 *
 * THE TEST IS THE FAULT ADDRESS, NOT THE PROGRAM COUNTER: a fetch fault reports pc == fault address, but a DATA fault
 * reports the pc of the instruction that did the access, which is in the guest's own library OUTSIDE the region
 * (xmem-arena-and-wx.md). Whether a fetch or a load/store, the region is guest memory however the pc is reported; the
 * caller keeps pc only to name the faulting instruction in the log.
 *
 * A fault whose address is OUTSIDE the region is still a host crash and keeps the old behaviour exactly.
 */
bool tl_xmem_arena_fetch_fault(uintptr_t pc, uintptr_t addr)
{
    (void)pc;   /* the pc names the instruction; the ADDRESS names whose memory it was */
    return g_x.open && tl_xmem_contains((const void *)addr);
}

/* Record why the last run died, in the same container file as the verdict, so the reason survives a fault that keeps
 * the process alive only long enough to write it. */
void tl_xmem_note_guest_fault(const char *reason, uintptr_t pc)
{
    snprintf(g_verdict_rec.fault, sizeof(g_verdict_rec.fault), "%s", reason ? reason : "the guest faulted in its arena");
    g_verdict_rec.fault_pc = pc;
    g_verdict_rec.fault_when = (long long)time(NULL);
    /* RECORD the fault for the fatal-time probe -- nothing more: this runs in the signal handler, where no mach probe
     * and no sigsetjmp guard for the signal being handled is safe. The three verdicts are taken later, once, on the
     * normal thread that runs the session close. A NEW fault clears `reported` so the next close re-takes them. */
    g_fault_addr = pc;
    g_fault_have = true;
    g_fault_reported = false;
    xmem_write_verdict_file();
}
#else
/* A host with no mach probes records nothing (the verdict is never measured there either). */
bool tl_xmem_arena_fetch_fault(uintptr_t pc, uintptr_t addr) { (void)pc; (void)addr; return false; }
void tl_xmem_note_guest_fault(const char *reason, uintptr_t pc) { (void)reason; (void)pc; }
#endif

/*
 * husk_ios_jit_exec_gate -- the app's EXEC GATE (declared in src/app/Husk/HuskBridge.h). The app calls this to bracket
 * the moments that could cost the debugger's execute grant: the claim at boot, and the detach. It must live here, in
 * the layer, because this is where the arena and its measurement machinery are -- the layer is what the app links.
 *
 * One guarded write-then-execute, the boot selftest's shape: a movz/ret stub written through the writable view and
 * CALLED at the executable view. The call is the verdict -- no protection field is consulted, because the field is the
 * thing that can lie. Nothing is unmapped, no page is made writable, no thread is ended, and the bytes found there are
 * put back either way, so a gate run leaves the arena as it found it.
 */
bool husk_ios_jit_exec_gate(const char *after)
{
    const char *label = after ? after : "(unstated)";
    if (!g_x.rx || !g_x.size) {
        tl_log_line("xmem: EXEC GATE after %s: no arena is open, so there is nothing to measure -- the gate "
                 "reports the grant only where the guest's own code would run", label);
        return false;
    }
    int stage = 0;
    bool ran = xmem_write_fetch((uintptr_t)xmem_writable_view(), (uintptr_t)g_x.rx, &stage);
    if (ran) {
        tl_log_line("xmem: EXEC GATE after %s: a stub written through the writable view %p and called at the "
                 "executable view %p -> RAN -- execute is still there", label,
                 (void *)xmem_writable_view(), (void *)g_x.rx);
    } else if (stage == 1) {
        tl_log_line("xmem: EXEC GATE after %s: the STORE through the writable view %p was refused, so the call "
                 "could not be made -- the grant cannot be judged from a store that never landed",
                 label, (void *)xmem_writable_view());
    } else {
        tl_log_line("xmem: EXEC GATE after %s: the stub was written through %p and called at %p but FAULTED ON "
                 "THE FETCH -- %s", label, (void *)xmem_writable_view(), (void *)g_x.rx,
                 stage == 2 ? "the executable view names no execute for this device any more"
                            : "the guard did not run, so there is no verdict");
    }
    return ran;
}
