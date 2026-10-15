/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-xmem.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

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
/* The guarded write-then-execute, defined beside the other mach probes far below; declared here so
 * the StikDebug path's adopt_exec_view (just above open_platform) can be written before it. */
static bool xmem_writable_fetches(uintptr_t at, int *stage);
/* The codewrite store-fault guard, which services a store into the executable view one page at a
 * time in the W^X fallback. Declared weak so this file still links without husk-tl-codewrite.c
 * (tools/a32/step0_xmem.c), where the fallback cannot engage and the open line says so. */
extern bool tl_codewrite_enabled(void) __attribute__((weak));
#endif

static struct {
    uint8_t *rx, *rw;
    size_t size, used;
    bool open;
    /* True when the guest-facing view (g_x.rw) is the mapping that carries the JIT authorisation
     * (Fix A). It is the whole point of this file's two-view ordering: see tl_xmem_guest_view_is_jit. */
    bool guest_view_jit;
    /* How the guest-facing view's authorisation was obtained, for the one-line report:
     * "MAP_JIT" when the mapping the guest addresses is a MAP_JIT mmap, "plain" when it is
     * plain RWX, NULL when the guest-facing view is a vm_remap (no authorisation of its own). */
    const char *route;
    /* True in the W^X fallback: the guest was handed the executable view, but that view could not
     * be HELD writable-and-executable in place (iOS enforces W^X -- the guarded store faulted), so
     * a store into it is serviced by flipping the one page writable for the store and back to
     * executable for the fetch (tl_xmem_wx_fallback, tl_xmem_page_set_writable/executable). False
     * everywhere the guest has a genuinely RWX address of its own (Fix A, or the StikDebug
     * executable view proven RWX). */
    bool wx_fallback;
    /* The WRITABLE view the layer emits its OWN bytes through: library images, stubs, and the
     * codewrite/kernel-write mirror. It is the same as g_x.rw everywhere the guest-facing view is
     * itself writable (Fix A, the own arena, the plain two-view fallback). In the W^X fallback the
     * guest-facing view is the EXECUTABLE one and is RX, so this is the debugger's writable vm_remap
     * instead -- the loader still writes every library image through a genuinely writable address,
     * and tl_xmem_delta() (writable view - executable view) is what the adrp retarget and the mirror
     * use to reach it. NULL means "the same as g_x.rw". */
    uint8_t *wrw;
    pthread_mutex_t lock;
    /* The verdict of the guarded write-then-execute on the address the GUEST both writes and
     * executes, taken when the arena opens (see tl_xmem_exec_verdict). It is the measurement the
     * app's "JIT is on" indicator is backed by, because the app's own prewarmed flag can be true
     * while the pages cannot be fetched. Zero-initialised: exec_measured is TL_XMEM_EXEC_UNMEASURED
     * and exec_detail is the empty string until a measurement is taken. */
    int exec_measured;
    char exec_detail[240];
} g_x = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* Record the guarded write-then-execute's outcome, so the app can report a measurement rather than
 * the flag it set. Called only with a real verdict (RAN or FAULTED); `detail` is copied, so the
 * caller's stack buffer need not outlive the call. Apple-only because every call site is: the
 * guarded probes are the mach ones below. */
#if defined(__APPLE__)
static void xmem_note_exec(int verdict, const char *detail)
{
    g_x.exec_measured = verdict;
    if (detail && *detail) snprintf(g_x.exec_detail, sizeof(g_x.exec_detail), "%s", detail);
    else g_x.exec_detail[0] = '\0';
}
#endif

/* The view the layer WRITES through. Everything that mirrors a guest/executable address to a
 * writable one -- the loader's bulk emission (tl_xmem_alloc), the codewrite redirect (tl_xmem_delta)
 * and the kernel-write redirect (kernel_dst) -- goes through this, never through g_x.rw directly,
 * because in the W^X fallback g_x.rw is the EXECUTABLE (read-only) view. */
static uint8_t *xmem_writable_view(void) { return g_x.wrw ? g_x.wrw : g_x.rw; }

/* Set while building the translation layer's OWN arena: make_views then adopts ONLY the order
 * where the guest-facing view is the mapping that carries the JIT authorisation (the address the
 * guest both writes and executes). The fallback order -- guest-facing view a vm_remap -- is
 * exactly what the two phone crashes proved iOS will not execute (PvZ's A64HookFunction arena,
 * robot gf's UnityMain bus error), so when the layer can have an authorised view of its own it
 * must not silently accept the fallback; if it cannot, the caller falls back to the debugger's
 * region and says so. */
#if defined(__APPLE__)
static bool g_own_arena_only;
#endif

/*
 * The per-thread W^X toggle for a MAP_JIT mapping. pthread_jit_write_protect_np() is in libsystem
 * but the iOS SDK marks it unavailable, so it is looked up at run time and nothing links against it.
 * With it, a thread's MAP_JIT pages are writable (write_mode(true)) or executable (write_mode(false));
 * where no write protection is enforced -- and for any mapping that is not MAP_JIT -- it is a no-op
 * and the mapping is plain RWX. (QEMU pokes the comm-page APRR registers itself inside tcg/; this is
 * the libsystem entry point that governs the same state, and the one reachable from here.)
 */
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
 * Executable memory a process makes for itself, on a device that does not need a debugger to hand it out: one where the kernel lets a process that is marked as debugged
 * (CS_DEBUGGED) run pages it wrote. That is what a jailbreak does when it is told to allow JIT in apps (Dopamine marks every app it launches as debugged), what
 * TrollStore's enable-jit does (its root helper attaches with ptrace and lets go, which leaves the flag set), and what a debugger's attach does on an iOS before TXM. It is
 * the way UTM gets its JIT there too: an anonymous read+execute mapping, and a read+write alias of the same pages made with vm_remap.
 *
 * The two views sit side by side, a fixed distance apart, because the loader reaches one from the other with adrp (+-4 GiB). Which one carries the JIT authorisation is settled by make_views below: the
 * view the guest addresses and calls is preferred, so that an arena the guest keeps at a writable-view address can really be made to execute. Nothing is trusted until a function written through one view
 * has been called through the other (and, for the preferred order, through itself); a guard turns a refusal into a failure rather than a crash.
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
 * Build the two views of one set of pages inside the reservation `base` (room for `base` and
 * `base + size`), and hand them to the loader as g_x.rw (the view the guest addresses and calls,
 * where the loader's adrp retarget and sym_value put writable content) and g_x.rx (the view guest
 * CODE is executed through). Which mapping carries the JIT authorisation is the whole question:
 *
 *   - Fix A, tried first: the guest-facing view IS the `mmap`, created PROT_READ|PROT_WRITE|PROT_EXEC
 *     with MAP_JIT (or plain where the host allows a writable-and-executable mapping), and the code
 *     view is a `vm_remap` of it. The address a guest keeps for its own arena is then an authorised
 *     mapping, so adding execute to it -- which is all the guest does -- is real.
 *   - the fallback, exactly what shipped before: the `mmap` carries the authorisation as the CODE
 *     view and the guest-facing view is the `vm_remap`. A guest arena in the writable view is then a
 *     vm_remap alias that no vm_protect can make fetch (the phone's SIGBUS), which tl_xmem_allow_exec
 *     refuses honestly.
 *
 * The order is settled by MEASUREMENT, never by a guess: a pair is adopted only after both views have
 * been shown to execute code written through the other (and, for Fix A, the guest-facing view a
 * function written through itself). A host that refuses the Fix A mapping, or whose code view turns
 * out not to execute, is given the fallback; and a host that refuses both fails loudly rather than
 * returning a region that cannot run. Nothing is unmapped -- the next attempt overwrites the same
 * reservation with MAP_FIXED -- and no thread is ended or joined.
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
 * The translation layer's OWN arena: a mapping the APP owns, obtained here (not the debugger's
 * region, whose owner is QEMU), whose guest-facing view is the address the guest both writes and
 * executes. Two phone crashes are the reason this is preferred over the debugger's region:
 *
 *   PvZ (083605): the guest computed a code pointer off the writable base the layer handed it
 *   (0x13b012e10 in the writable view, `blr x8` from libRFLib.so A64HookFunction) and branched;
 *   the page was mapped, vm_protect returned 0, and it still had no execute -- a vm_remap alias
 *   that iOS will not run. The two-address scheme fails in the field whenever the guest derives an
 *   address from the writable base.
 *
 *   robot gf (083442/083605): a fetch at a page whose entry NAMED execute (prot 5) still took a
 *   bus error -- the page was not backed at all. That region is StikDebug's, reserved rather than
 *   populated, and only a few MiB of it is ever mapped.
 *
 * An mmap of the app's own is fully backed and, with CS_DEBUGGED set, MAP_JIT is honoured here. It
 * is proven by measurement, never by the version: make_views runs the function written through one
 * view and called through the other (and through itself) before this returns true. When it cannot
 * be had, the caller keeps the honest refusal and falls back to the debugger's region, naming why.
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
/* The loader retargets code at the writable view with adrp (+-4 GiB). A region whose writable alias the kernel placed anywhere
 * (VM_FLAGS_ANYWHERE: hundreds of GiB away on iOS 27) gets a second alias of the same pages next to the executable view: a local
 * vm_remap, which needs no debugger. Free addresses after the region, then before it, in 16 MiB steps out to 2 GiB. */
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

/* The StikDebug executable view, made writable-in-place and proven by a guarded write-then-execute
 * before it is adopted as the guest's arena; defined beside the mach probes below. */
static bool adopt_exec_view(tl_dual_mapping *m, char *err, size_t errlen);

/* The phone: an arena the layer makes for ITSELF if the device allows it (with CS_DEBUGGED set, MAP_JIT is honoured), and only
 * then StikDebug's region -- whose owner is QEMU, which is reserved rather than populated and whose guest-facing view is a
 * vm_remap iOS will not execute. Nothing is trusted by version: both routes are proven by running a function written into the
 * view the guest addresses (make_views), and whichever is taken is named in one line. */
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
            /* No debugger region: the old path applies -- the layer makes its own and accepts the two-view
             * fallback where Fix A is not granted (libraries still load and run through the code view; only a
             * guest JIT arena is refused, and tl_xmem_allow_exec says so). From iOS 26 the Trusted Execution
             * Monitor decides what may execute and an attempt to run unblessed memory can end the process, so
             * without a debugger nothing is tried there. */
            if (__builtin_available(iOS 26.0, *)) {
                snprintf(err, errlen, "no arena of the layer's own (%s), and no StikDebug JIT region: this iOS enforces TXM, where only a debugger (StikDebug or Built-in StikJIT) can grant executable memory", own[0] ? own : "refused");
                return false;
            }
            return grant_self(host_bytes, err, errlen);
        }
    }

    if (m && m->rw_addr && m->rx_addr) {
        /* The debugger hands back TWO views of one set of pages, and the boot log has already
         * measured which one works: the EXECUTABLE view (m->rx_addr) executed generated code the
         * debugger wrote through the writable alias, and its mappings persist after the debugger
         * detaches; the writable alias -- a vm_remap -- FAULTED when a page of IT was made
         * executable and fetched (xmem_probe_guest_view below). iOS authorises execute per mapping
         * ENTRY and an alias never inherits it, so the vm_remap can never be the guest's arena.
         * So hand the guest the EXECUTABLE view -- the one whose generated code provably ran --
         * made writable IN PLACE: vm_protect(RWX) on that view, then PROVE it by writing a function
         * through that very address and calling it there (adopt_exec_view). Only if that call RAN
         * is the address adopted, as BOTH views, so the single address the guest both writes and
         * executes is the one it already holds -- what PvZ's A64HookFunction computed and what
         * robot gf's UnityMain branched to. */
        char why[240] = "";
        if (adopt_exec_view(m, why, sizeof(why))) {
            tl_log_line("xmem: open: StikDebug's prewarmed region -- guest arena %p is the EXECUTABLE view (made RWX in place); the guest writes AND executes at this one address (rx == rw, delta %td). Guarded write-then-execute RAN",
                        (void *)g_x.rw, tl_xmem_delta());
            return true;
        }
        /* The executable view could not be HELD writable-and-executable: its store (or its fetch)
         * faulted -- iOS keeps W^X. The guest is handed the EXECUTABLE view as its ONE arena address
         * (the view whose generated code provably ran at boot); because that view is RX, a store into
         * it is serviced one page at a time by the codewrite guard. The debugger's writable vm_remap
         * is kept, but ONLY as the twin the layer emits library images and codewrite mirrors through
         * (g_x.wrw) -- never as the view the guest holds. The old code contradicted its own
         * declaration here: it reported handing over the executable view while setting g_x.rw (the
         * guest-facing view) to that vm_remap, which is exactly the alias iOS will not fetch. */
        g_x.rx = m->rx_addr;             /* guest code executes here, and this is the guest's arena */
        g_x.rw = m->rx_addr;             /* the ONE address the guest both writes and executes (RX) */
        g_x.wrw = m->rw_addr;            /* the writable twin: the layer's bulk emission (vm_remap) */
        g_x.size = m->size;
        g_x.guest_view_jit = false;      /* it is RX, not an authorised-RWX view */
        g_x.wx_fallback = true;
        g_x.route = "the executable view, handed to the guest as RX (W^X fallback: stores into it are serviced one page at a time; the layer emits through the writable twin)";
        near_alias();                    /* keep the writable twin within adrp range of the executable view */
        {
            /* adrp, which the loader uses to retarget guest code at the writable view, reaches +-4 GiB. */
            ptrdiff_t d = (ptrdiff_t)(g_x.wrw - g_x.rx);
            if (d > ((ptrdiff_t)3 << 30) || d < -((ptrdiff_t)3 << 30))
                tl_log_line("xmem: the writable twin %p is %td MiB from the executable view: too far for the loader's adrp", (void *)g_x.wrw, d >> 20);
        }
        tl_log_line("xmem: open: StikDebug's prewarmed region -- the EXECUTABLE view %p could NOT be made writable in place (%s); the guest is handed THAT view as its one arena address (RX), the layer emits through the writable twin %p (delta %td), and a guest store into the arena is serviced in the JIT guard, one page at a time",
                    (void *)m->rx_addr, why[0] ? why : "refused", (void *)g_x.wrw, tl_xmem_delta());
        /* The other fact the log stated about the vm_remap and now must not: that it "names RWX", yet
         * fetching it faults. Reconcile it here, once, so the arena lines are consistent. */
        tl_log_line("xmem: W^X fallback: the writable twin %p is a vm_remap of the same pages; it can be vm_protected to NAME RWX (its max is the debugger's 7), yet a fetch there faults -- iOS authorises execute per mapping ENTRY and an alias never inherits the debugger's blessing, so the protection field says RWX while the pmap publishes no fetchable PTE. That is why the twin is NOT the guest's arena and why 'names RWX' and 'will not fetch' were both true of the same address",
                    (void *)g_x.wrw);
        /* Whether the store service can engage at all: the JIT guard must be armed for this engine. A
         * guard that was never enabled cannot service any store, and the guest's first store into the
         * arena would fault instead of being carried through the writable twin. Say which, here. */
        {
            const char *guard = !tl_codewrite_enabled ? "UNKNOWN (the codewrite module is not linked here)"
                              : tl_codewrite_enabled() ? "ARMED"
                                                       : "NOT ARMED -- tl_codewrite_enable() has not run for this engine, so a store into the executable view will FAULT, not be serviced";
            tl_log_line("xmem: W^X fallback: the JIT guard for stores into the executable view is %s", guard);
        }
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
    /* Reserve twice the span and put the two views side by side. adrp reaches +-4 GiB, and
     * the loader retargets code at the writable view with it, so the views must stay close:
     * left to place the alias anywhere, the kernel can put it more than 3 GiB away. */
    uint8_t *base = mmap(NULL, size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        snprintf(err, errlen, "mmap of %zu MiB failed: %s", (size * 2) >> 20, strerror(errno));
        return false;
    }
#if defined(__APPLE__)
    /* The same builder the phone's self-grant path uses, so the Mac runs the code the phone runs --
     * including the Fix A ordering: where this host grants a writable-and-executable mapping (a Mac
     * with the JIT entitlement, or one that does not enforce W^X), the guest-facing view carries the
     * authorisation; where it does not, the two-view fallback is adopted and the negative control
     * keeps the shape it asserts. */
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
    if (!g_x.open) {
        char local[160] = "";
        ok = open_platform(host_bytes, local, sizeof(local));
        if (ok) {
            g_x.open = true;
            /* One run must be able to end the guessing: what the kernel says the region's entries
             * are, entry by entry, and whether the view the guest addresses and calls will fetch. */
            xmem_report_open();
        } else if (err && errlen) {
            snprintf(err, errlen, "%s", local);
        }
    }
    pthread_mutex_unlock(&g_x.lock);
    return ok;
}

bool tl_xmem_alloc(size_t bytes, uint8_t **rx, uint8_t **rw)
{
    size_t n = (bytes + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1);
    bool ok = false;
    pthread_mutex_lock(&g_x.lock);
    if (g_x.open && n <= g_x.size - g_x.used) {
        if (rx) *rx = g_x.rx + g_x.used;
        if (rw) *rw = xmem_writable_view() + g_x.used;
        g_x.used += n;
        ok = true;
    }
    pthread_mutex_unlock(&g_x.lock);
    return ok;
}

ptrdiff_t tl_xmem_delta(void) { return xmem_writable_view() - g_x.rx; }
bool tl_xmem_near(const void *p, size_t dist)
{
    if (!g_x.open || !p) return false;
    /* Two views of one set of pages, and in the Fix A order the guest-facing one is the LOWER address,
     * so the span is taken from the two views rather than assumed to start at rx. */
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

/*
 * b_mprotect, in the bionic shim, owns the guest's errno. step0 links only this file and carries
 * no shim, so this is declared weak: allow_exec sets EACCES where the shim is present and still
 * links where it is not.
 */
extern void tl_set_guest_errno(int guest_errno) __attribute__((weak));

#if defined(__APPLE__)
/*
 * The two questions vm_protect's return code does not answer.
 *
 * vm_protect returning KERN_SUCCESS means only that the request was within the entry's
 * max_protection and that the entry's protection field now names EXECUTE -- it says nothing about
 * whether the pmap will publish a fetchable PTE. On iOS an alias made with vm_remap of pages whose
 * defining mapping was never granted JIT authorisation (MAP_JIT) names execute and still will not
 * fetch: that is the SIGBUS (pc == fault address) the writable-view path exists to end. So the
 * verdict is taken from the two things that decide it, never from the return code.
 */

/*
 * What the map entry that holds `p` actually says.
 *
 * The yes/no this used to be (xmem_entry_is_exec) collapsed three different answers into "no
 * execute": the probe failing outright (vm_region_recurse_64 returning non-success), the probe
 * naming an entry that genuinely does not carry VM_PROT_EXECUTE, and the probe naming a DIFFERENT
 * entry than the one vm_protect changed (a submap level, see `depth`). Only the first two mean the
 * page cannot be executable. Keeping the raw answer -- the call's own return code plus the entry
 * the kernel named, its range, protection, max_protection, nesting depth and user tag -- is the
 * difference between a truthful refusal and a false negative, and it is what the phone's log could
 * not say (exec-on-device-csdebugged.md).
 */
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
 * The fetch itself: write a movz/ret through the writable view at `at` and call it there, under the
 * same sigsetjmp guard self_runs uses, so a page that names execute but will not fetch is caught
 * rather than fatal. Nothing but a real fetch can answer the question, because a protection field
 * can name EXECUTE while the pmap refuses the fetch. `at` is the guest's own arena, so its bytes are
 * saved and put back whether the call returns or faults -- a thread that reached that address between
 * the two writes must find the code it had, and nothing here unmaps or leaves a stub behind. The two
 * writes are bracketed with the W^X toggle so the store lands on a MAP_JIT view (writable), and the
 * call happens in execute mode -- the two states a guest's arena passes through as it writes and then
 * runs its own trampoline.
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
    /* Put the guest's bytes back ONLY if the store actually landed. A store that itself faulted (the
     * page cannot be written at all -- W^X) must not be retried here: the guard has been left, and a
     * second fault would end the process. Nothing was changed in that case, so there is nothing to
     * restore. */
    if (g_self_stage != 1) {
        xmem_write_mode(true);
        memcpy((void *)write_at, saved, sizeof(saved));
        xmem_write_mode(false);
        sys_icache_invalidate((void *)write_at, sizeof(saved));
    }
    if (stage) *stage = (int)g_self_stage;
    return ok;
}

/* Write the stub at `at` and call it at `at` (the same view named twice): the question a single
 * address answers, when that address is the one the guest both writes and executes. */
static bool xmem_writable_fetches(uintptr_t at, int *stage)
{
    return xmem_write_fetch(at, at, stage);
}

#if defined(__APPLE__) && !TARGET_OS_OSX
/*
 * Hand the guest the StikDebug EXECUTABLE view, made writable in place, and do not trust it until
 * it has proven itself. The two views have already answered for themselves in the boot log: the
 * executable view EXECUTED generated code written through the writable alias (the debugger's own
 * selftest, "PASS -- executed generated code from the RX alias, got 42") and its mappings persist
 * after the debugger detaches; the writable alias -- a vm_remap -- FAULTED when a page of IT was
 * made executable and fetched. iOS authorises execute per mapping ENTRY and an alias never inherits
 * it, so the vm_remap can never be the guest's arena; the executable view is the one whose generated
 * code provably ran. So:
 *
 *   1. raise this entry's maximum to RWX, then vm_protect the whole view RWX in place (the region
 *      reports max 7 = RWX, so this is within the entry's contract); and
 *   2. write a movz/ret through the address the guest is given and CALL it there, under the same
 *      signal guard self_runs uses (xmem_writable_fetches). Only if that call returns 0x5a5a is the
 *      address adopted -- as BOTH views, rw == rx == m->rx_addr, so the single address the guest
 *      both writes and executes is the one it already holds.
 *
 * A vm_protect that returned KERN_SUCCESS is NOT proof: on this family of devices a protection
 * field can name EXECUTE while the pmap refuses the store or the fetch. The guarded call is the
 * verdict, and the stage it faulted at (store vs fetch) is carried out in `err` so the caller's one
 * line says which -- a store fault means iOS keeps W^X (the fallback), a fetch fault means the view
 * will not execute. Nothing is unmapped and no thread is ended: on failure the caller keeps the
 * debugger's two views exactly as they were.
 */
static bool adopt_exec_view(tl_dual_mapping *m, char *err, size_t errlen)
{
    task_t me = mach_task_self();
    vm_address_t base = (vm_address_t)(uintptr_t)m->rx_addr;
    size_t size = m->size;
    /* set_maximum first: vm_protect below cannot name a right the entry's max_protection never
     * allowed, and the region's own read-back already says max 7 = RWX. If even the maximum cannot
     * be raised, the entry was capped and this fails for the right reason. */
    kern_return_t kr = vm_protect(me, base, size, TRUE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
    if (kr == KERN_SUCCESS)
        kr = vm_protect(me, base, size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
    xmem_entry e = xmem_probe_entry((uintptr_t)base);
    if (kr != KERN_SUCCESS) {
        snprintf(err, errlen, "vm_protect(RWX) on the executable view returned %d (%s); entry %p..%p prot %d max %d depth %u tag %u",
                 (int)kr, mach_error_string(kr),
                 (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag);
        xmem_note_exec(TL_XMEM_EXEC_FAULTED, err);   /* the guest's arena cannot be made writable-and-executable */
        return false;
    }
    int stage = 0;
    bool ran = xmem_writable_fetches((uintptr_t)base, &stage);
    if (!ran) {
        /* Leave the view exactly as it was (RX): the fallback relies on a store into it faulting so
         * the codewrite guard can service it in place, and a view left nominally RWX would let the
         * guest store straight into a page that would not run -- no fault, no service, no fix. */
        (void)vm_protect(me, base, size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
        snprintf(err, errlen, "the executable view names RWX (entry %p..%p prot %d max %d depth %u tag %u) but the guarded write-then-execute %s (signal %d, si_code %d)",
                 (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag,
                 stage == 1 ? "FAULTED ON THE STORE -- iOS keeps W^X here"
                            : stage == 2 ? "FAULTED ON THE FETCH -- the view names RWX but will not run"
                                         : "FAULTED",
                 (int)g_self_sig, (int)g_self_code);
        xmem_note_exec(TL_XMEM_EXEC_FAULTED, err);   /* the guarded probe is the verdict, and it said no */
        return false;
    }
    g_x.rx = g_x.rw = m->rx_addr;
    g_x.size = m->size;
    g_x.guest_view_jit = true;      /* the guest-facing view carries the authorisation: it is RWX */
    g_x.wx_fallback = false;
    g_x.wrw = NULL;                 /* the guest-facing view is writable in place: no separate twin */
    g_x.route = "the executable view, made RWX in place";
    xmem_note_exec(TL_XMEM_EXEC_RAN, NULL);   /* one address, written and executed, and it ran */
    return true;
}
#endif

/*
 * The one line that must end the guessing: the arena's own address and size, how many MiB of it the kernel says are MAPPED and
 * how many EXECUTABLE, which route gave the guest its view (MAP_JIT, plain RWX, or a vm_remap that has no authorisation of its
 * own), the delta between the two views, and -- separately below -- whether the guest-facing view really fetched a function
 * written into it. Both phone crashes would have been one line instead of weeks: robot gf faulted on a page whose entry NAMED
 * execute (prot 5) but was not backed, and PvZ's writable view was mapped but never executable. The old walk stopped at 128
 * entries, which is exactly why a 512 MiB region could read as "2 MiB mapped" and hide an unbacked page; this walks the whole
 * span.
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
    /* Reconcile the walk's own contradiction, so the next run is not read wrong. The walk reports the
     * kernel's PROTECTION FIELD; on this device family execute is granted per defining mapping entry
     * (the debugger's blessing), NOT by that field, and the boot selftest executed generated code at
     * this view's first page -- so "0 MiB executable / NOT EXECUTABLE" is what the kernel names, not
     * proof the view cannot run. The guarded fetch printed next is the verdict. */
    if (execut == 0)
        tl_log_line("xmem: the walk above names NO execute anywhere in the executable view, yet the boot selftest ran generated code at this view's first page -- so 'non-executable' here is the protection field lying, not the pmap. On this region execute comes from the debugger's per-entry grant; the guarded write-then-execute below, aimed at the view the guest holds, is the only verdict");
    if (g_x.wx_fallback)
        tl_log_line("xmem: the guest-facing view is the EXECUTABLE view %p (RX); the layer writes through the writable twin %p (delta %td). 'resident but not executable' entries above are the executable view's own entries -- the field, not a second view",
                    (void *)g_x.rw, (void *)g_x.wrw, (ptrdiff_t)(g_x.wrw - g_x.rx));
}

/*
 * Ask the guest-facing view the question the guest will ask it: write a stub there and call it
 * there, guarded. This is the same shape self_runs uses, pointed at the one view the phone died on,
 * and it runs once per open on bytes nothing has written yet (the region is empty at this point).
 * The read-back is printed beside it, raw, so the next run says which of the two answers is real.
 */
static void xmem_probe_guest_view(void)
{
    uintptr_t at = (uintptr_t)g_x.rw;
    xmem_entry e = xmem_probe_entry(at);
    if (g_x.wx_fallback) {
        /* The guest's view is the EXECUTABLE one and is RX, so a store into it faults (that is what
         * the codewrite guard exists to service). The question "does the guest's view FETCH" is
         * therefore answered the way the boot selftest answered it: write the stub through the
         * WRITABLE TWIN and call it at the guest's address. This is the decisive measurement -- if it
         * faults on the fetch, the executable view names no execute and will not run, and no store
         * service can change that: the fetch itself is what iOS is refusing. */
        int stage = 0;
        bool f = xmem_write_fetch((uintptr_t)xmem_writable_view(), at, &stage);
        xmem_note_exec(f ? TL_XMEM_EXEC_RAN : TL_XMEM_EXEC_FAULTED,
                       f ? NULL
                         : stage == 1 ? "the writable twin could not be written (unexpected: it should be)"
                                      : "the executable view names no execute and will not run: the guarded write through the writable twin then call at the guest's address FAULTED ON THE FETCH, and no store service can change that because the FETCH is what is refused");
        tl_log_line("xmem: guest-facing view %p IS the EXECUTABLE view (RX): read-back %s (entry %p..%p prot %d max %d depth %u tag %u), guarded write through the writable twin %p then call at %p %s",
                    (void *)at, e.kr == KERN_SUCCESS ? "ok" : "FAILED",
                    (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag,
                    (void *)xmem_writable_view(), (void *)at,
                    f ? "RAN -- the executable view is genuinely executable here, so the guest's arena will run; a store into it is serviced by the JIT guard, one page at a time"
                      : stage == 1 ? "FAULTED writing the writable twin (unexpected: the twin should be writable)"
                                   : "FAULTED ON THE FETCH -- the executable view names no execute and will not run: iOS is refusing write-and-execute JIT on this device, and the store service cannot help because the FETCH is what is refused");
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

static void xmem_report_open(void)
{
    if (!g_x.open) return;
    xmem_report_region();
    xmem_probe_guest_view();
}
#else
/* No mach probe on a host that is not an Apple one: nothing to report. */
static void xmem_report_open(void) {}
#endif

/*
 * Make the writable view's pages executable where the guest named them with PROT_EXEC.
 *
 * A JIT -- here the game's own A64HookFunction -- writes a trampoline into memory it holds and calls
 * it by the address that view gave it (the loader retargets its adrp at the writable view because the
 * page has no TL_PAGE_X, husk-tl-ld.c). The executable view of the same pages already runs, but the
 * address the guest holds is the guest-facing one, so execute has to be added to THAT view -- and then
 * flushed, so the bytes just written are what the CPU fetches. Fix A makes that view the one that
 * carries the JIT authorisation (make_views); where the kernel will not grant that, it is a vm_remap
 * alias and adding execute to it is legal-looking but ineffective.
 *
 * The verdict is the GUARDED FETCH, and the read-back is a diagnostic. In the Fix A order the
 * guest-facing view is the mapping that carries the JIT authorisation, so adding execute to it is
 * real and the fetch runs. In the fallback order it may be a vm_remap whose defining mapping was
 * never granted that authorisation, in which case the fetch faults and the refusal is honest.
 *
 * WHY THE FETCH DECIDES AND NOT THE READ-BACK (exec-on-device-csdebugged.md): the phone's runs are
 * self-contradictory for the read-back. vm_protect returned KERN_SUCCESS -- which by XNU's contract
 * means the request was inside the entry's max_protection and the entry's protection was set -- and
 * then the read-back of the SAME page said it did not name execute. A successful vm_protect cannot
 * both have happened and not have happened, so either the read-back failed outright (its own
 * return code is now kept and printed) or it named a different entry/level than the one vm_protect
 * changed. The old code hid all of that: it only ran the fetch when the read-back agreed, so a
 * read-back that lies was indistinguishable from a page that will not fetch, and the phone refused
 * a page that may well have executed. Now the fetch always runs and its result is the answer; the
 * read-back is printed raw beside it. The fetch is guarded (sigsetjmp + SIGBUS/SIGSEGV/SIGILL) and
 * this device demonstrably delivers an unblessed fetch as a CATCHABLE SIGBUS -- the guest's own
 * jump into the arena produced signal 10, caught by the crash reporter -- so a page that really
 * will not fetch is a caught fault and an honest EACCES, never a dead process. TL_XMEM_NO_FETCH_VERDICT=1
 * restores the old gating (fetch only when the read-back agrees) for a device that ends the process
 * rather than faulting.
 *
 * TL_NO_XMEM_RW_EXEC=1 never adds execute here, to force the store-emulation fallback.
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
    static int readback_decides = -1;
    if (readback_decides < 0) readback_decides = getenv("TL_XMEM_NO_FETCH_VERDICT") ? 1 : 0;
    kern_return_t kr = vm_protect(mach_task_self(), (vm_address_t)page, len, FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
    xmem_entry e = xmem_probe_entry(page);
    bool named = kr == KERN_SUCCESS && e.kr == KERN_SUCCESS && (e.prot & VM_PROT_EXECUTE) != 0;
    bool fetched = xmem_writable_fetches(page, NULL);   /* the ground truth, and it runs either way */
    ok = readback_decides ? (named && fetched) : fetched;
    if (kr != KERN_SUCCESS)
        snprintf(why, sizeof(why), "the kernel's vm_protect returned %d (the entry was never given execute)", (int)kr);
    else if (e.kr != KERN_SUCCESS)
        snprintf(why, sizeof(why), "vm_protect returned 0 and the read-back itself failed (vm_region_recurse_64 returned %d, %s), and the guarded fetch faulted (signal %d, si_code %d)",
                 (int)e.kr, mach_error_string(e.kr), (int)g_self_sig, (int)g_self_code);
    else if (!named)
        snprintf(why, sizeof(why), "vm_protect returned 0 but the read-back named entry %p..%p prot %d max %d depth %u tag %u (no execute), and the guarded fetch faulted (signal %d, si_code %d)",
                 (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag,
                 (int)g_self_sig, (int)g_self_code);
    else
        snprintf(why, sizeof(why), "vm_protect returned 0 and the read-back names execute, but the guarded fetch faulted (signal %d, si_code %d)",
                 (int)g_self_sig, (int)g_self_code);
#else
    ok = mprotect((void *)page, len, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
    snprintf(why, sizeof(why), "%s", ok ? "" : strerror(errno));
#endif
    /* A success is worth a line WHEN THE READ-BACK DISAGREED: that is exactly the phone's shape,
     * and the run that finally lets the guest's arena run must say the read-back was wrong. */
#if defined(__APPLE__)
    if (ok && !named) {
        static int said_ok;
        if (said_ok++ < 4)
            tl_log_line("husk: xmem exec on the writable view at %p (%#zx bytes): MADE EXECUTABLE although the read-back did not name execute (read-back: %s, entry %p..%p prot %d max %d depth %u tag %u) -- the guarded fetch is the verdict and it RAN",
                        p, n, e.kr == KERN_SUCCESS ? "probe ok" : "probe FAILED",
                        (void *)e.start, (void *)(e.start + e.size), (int)e.prot, (int)e.max, (unsigned)e.depth, e.tag);
    }
#endif
    if (!ok) {
        static int said;
        if (said++ < 1)
            tl_log_line("husk: xmem exec refused on the writable view at %p (%#zx bytes): %s, so the guest's JIT arena stays non-executable", p, n, why);
        if (tl_set_guest_errno) tl_set_guest_errno(13);   /* EACCES: an honest failure, not a lie */
        return false;
    }
    tl_xmem_flush(p, n);
    return true;
}

size_t tl_xmem_used(void) { return g_x.used; }
size_t tl_xmem_size(void)  { return g_x.size; }

bool tl_xmem_guest_view_is_jit(void) { return g_x.open && g_x.guest_view_jit; }

/* The guarded write-then-execute's verdict, for the app's indicator: a measurement, not a flag. */
int tl_xmem_exec_verdict(void) { return g_x.exec_measured; }
const char *tl_xmem_exec_detail(void) { return g_x.exec_detail[0] ? g_x.exec_detail : NULL; }

/* True only in the W^X fallback: the guest was handed the executable view but it could not be HELD
 * writable-and-executable, so a store into it is serviced page by page (below). */
bool tl_xmem_wx_fallback(void) { return g_x.open && g_x.wx_fallback; }

/*
 * Flip the pages covering [p, p+n) to writable (RW) or back to executable (RX), at the MMAP level
 * -- which is the level iOS honours. The per-thread pthread_jit_write_protect (xmem_write_mode) only
 * governs MAP_JIT mappings; a page that was made executable with vm_protect is toggled with
 * vm_protect and nothing else. These are the two halves of the W^X fallback's per-page bracket,
 * called from the codewrite store-fault guard: a store into the executable view faults, the page is
 * made writable for the store, stored, then made executable again for the guest's fetch.
 *
 * One whole page at each end is covered, so a store that straddles a 16 KiB boundary is bracketed on
 * both sides. Returns false if the kernel refused (then the caller must NOT attempt the store: a
 * second fault inside the signal handler would end the process).
 */
#if defined(__APPLE__)
static bool xmem_page_prot(void *p, size_t n, vm_prot_t prot)
{
    uintptr_t lo = (uintptr_t)p, hi = lo + (n ? n - 1 : 0);
    uintptr_t page = lo & ~(uintptr_t)(TL_XMEM_PAGE - 1);
    uintptr_t last = (hi + TL_XMEM_PAGE) & ~(uintptr_t)(TL_XMEM_PAGE - 1);
    return vm_protect(mach_task_self(), (vm_address_t)page, (vm_size_t)(last - page), FALSE, prot) == KERN_SUCCESS;
}
bool tl_xmem_page_set_writable(void *p, size_t n)   { return xmem_page_prot(p, n, VM_PROT_READ | VM_PROT_WRITE); }
bool tl_xmem_page_set_executable(void *p, size_t n) { return xmem_page_prot(p, n, VM_PROT_READ | VM_PROT_EXECUTE); }
#else
bool tl_xmem_page_set_writable(void *p, size_t n)   { (void)p; (void)n; return false; }
bool tl_xmem_page_set_executable(void *p, size_t n) { (void)p; (void)n; return false; }
#endif

/*
 * The layer's own emission into the region, bracketed so a device that enforces W^X lets the store
 * and the fetch both happen. Only the Fix A order has a host-facing view that is simultaneously the
 * one the guest executes, so only there does the state matter; a no-op everywhere else.
 */
void tl_xmem_emit_begin(void) { if (g_x.guest_view_jit) xmem_write_mode(true); }
void tl_xmem_emit_end(void)   { if (g_x.guest_view_jit) xmem_write_mode(false); }
