/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The startup breadcrumbs. See husk-tl-startup-trace.h for why they exist and what they cost.
 *
 * The sites below are addresses inside the APK's own libil2cpp.so (com.IncontinentCell.MyDystopian
 * RobotGirlfriend v0.97.11, Unity 6000.3.8f1, 91,567,688 bytes) and libunity.so (18,066,480 bytes),
 * found with capstone and the .eh_frame_hdr binary-search table:
 *
 *   libil2cpp.so
 *     0x238f4cc  `mov x21, x0`         the body of the metadata loader that reads
 *                                      "global-metadata.dat" (0x238f4bc, reached from Runtime::Init)
 *     0x234864c  `stp x30,x19,[sp,#-16]!`  the entry of the EXPORTED il2cpp_init
 *     0x234866c  `and w0, w0, #1`      inside il2cpp_init, the instruction after `bl 0x23a0228`
 *                                      (the real Runtime::Init): il2cpp_init's core returned
 *     0x239fa40  `sub sp, sp, #0x70`   the body that the EXPORTED il2cpp_runtime_class_init thunk
 *                                      (0x2349098, one `b`) branches to -- x0 is the Il2CppClass*
 *     0x23bc63c  `ldr w1, [x24, #0xa64]`   Boehm's suspend pass: `pthread_kill(thread, GC_sig_suspend)`
 *     0x23bc89c  `ldr w1, [x24, #0xa68]`   Boehm's restart pass: `pthread_kill(thread, GC_sig_thr_restart)`
 *                                      (a restart signal only goes out once the world HAS stopped, so
 *                                       this site is the completion of the first stop-the-world cycle)
 *     0x23bd1b4  `cmp x28, x25`        the stop-the-world wait loop's condition (0x23bd188): one hit
 *                                      per wait, 3 ms apart, up to ~100 s
 *     0x23bd234  `mov x0, x23`         the timeout path that prints "Resent %d signals after timeout"
 *                                      (x24 = how many signals are being resent)
 *   libunity.so
 *     0x727578   `str x30,[sp,#-16]!`  a wrapper whose only work is `bl eglSwapBuffers` (0x72757c)
 *     0x736b84   `mov x1, x28`         the second eglSwapBuffers call site (0x736b88)
 *     0x744118   `add x0, x19, #0xc`   the instruction before the ONLY `bl AChoreographer_getInstance`
 *     0x744188   `mov x2, x19`         the instruction before `bl AChoreographer_postFrameCallback`
 *     0x744164   `mov x2, x19`         before `bl AChoreographer_postFrameCallback64` (site 1)
 *     0x7441d0   `mov x2, x19`         before `bl AChoreographer_postFrameCallback64` (site 2)
 *     0x72e3c0   `mov x3, x21`         before `bl pthread_create` (x2 is the start routine, resolved here)
 *     0x5af46c   `stp x30,x19,[sp,#-16]!`  the startup routine of a worker that names itself with the
 *                                      string "AssetGarbageCollectorHelper" (libunity.so+0xb10e6)
 *
 * Every site is checked against the instruction the APK's .so holds before it is patched, so a
 * different build of the game reports "NOT ARMED" instead of silently instrumenting the wrong
 * instruction. None of the sites is pc-relative (a stub cannot re-execute a pc-relative
 * instruction at its own address), which is why the milestone is the instruction BEFORE a marker
 * call rather than the call itself.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-startup-trace.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "husk-tl-bionic.h"     /* tl_log_line */
#include "husk-tl-ld.h"

/* The whole trace is bounded: this many lines, and no more, however long the run. */
#define ST_CAP 32
/* How many class initialisations name themselves (the first few are what a reader needs). */
#define ST_CLASS_LINES 4

/* ---------------------------------------------------------------- the milestones */

enum {
    G_METADATA = 0,     /* il2cpp is reading global-metadata.dat */
    G_INIT_ENTER,       /* il2cpp_init entered */
    G_INIT_RETURN,      /* il2cpp_init's core (Runtime::Init) returned */
    G_CLASS_INIT,       /* a managed class initialisation began */
    G_GC_SUSPEND,       /* the collector's first stop-the-world signal send */
    G_GC_RESTART,       /* the first restart signal: the first stop-the-world cycle COMPLETED */
    G_GC_WAIT,          /* the collector's wait-to-stop loop started waiting */
    G_GC_RESEND,        /* the wait timed out and the suspend signals were resent */
    G_SWAP,             /* the engine reached its first eglSwapBuffers */
    G_CHOREO,           /* the engine first asked for AChoreographer */
    G_FRAME_CB,         /* the engine posted its first frame callback */
    G_WORKER,           /* libunity created its first thread */
    G_ASSET_WORKER,     /* the worker that calls itself AssetGarbageCollectorHelper started */
    G_RENDER_ENTER,     /* the render thread entered nativeRender for the first time */
    G_RENDER_RETURN,    /* that first nativeRender returned */
    G_COUNT
};

static const char *const k_group_name[G_COUNT] = {
    "metadata(global-metadata.dat)", "il2cpp_init-entry", "il2cpp_init-returned", "first-class-init",
    "GC-suspend-send", "GC-restart-send(cycle-complete)", "GC-wait-to-stop", "GC-resend(timeout)",
    "eglSwapBuffers", "AChoreographer-getInstance", "choreographer-frameCallback",
    "libunity-pthread_create", "asset-worker-start", "render-thread-nativeRender", "first-frame-returned",
};

/* ------------------------------------------------------------------- the sites */

typedef struct {
    const char *lib;        /* the library the site lives in */
    uint64_t vaddr;         /* the site's link-time address in that library */
    uint32_t word;          /* the instruction the APK's .so holds there */
    int group;              /* the milestone, and the one-shot flag it shares with its siblings */
    void (*cb)(uint64_t *); /* called with x0..x28 as the registers stand */
    const char *what;       /* what the site is, for the arming line */
} st_site;

static atomic_int g_done[G_COUNT];      /* set once a milestone has been reached: one line each */
static atomic_int g_lines;              /* lines printed: the whole trace's cap */
static atomic_int g_class_lines;
static atomic_int g_reported;
static atomic_int g_armed_libs;
static atomic_int g_armed_sites;
static atomic_int g_failed_sites;

/* Which of the two target libraries the loader's hook actually saw, and what it armed in each. Reported at
 * the close even when a library never arrived, so a trace that produced nothing always names its cause in
 * the log ("the library was never seen") instead of failing silently. */
enum { ST_LIB_IL2CPP = 0, ST_LIB_UNITY = 1, ST_LIB_COUNT = 2 };
static const char *const k_lib_name[ST_LIB_COUNT] = { "libil2cpp.so", "libunity.so" };
static atomic_int g_lib_seen[ST_LIB_COUNT];
static atomic_int g_lib_armed[ST_LIB_COUNT];
static atomic_int g_lib_total[ST_LIB_COUNT];

typedef const char *(*class_name_fn)(void *klass);
static class_name_fn g_class_name_fn;   /* il2cpp_class_get_name, if the image exports it */

/* True for exactly the caller that may print, and only while the trace is under its cap. */
static int claim(int group)
{
    if (atomic_load_explicit(&g_done[group], memory_order_relaxed)) return 0;
    if (atomic_exchange(&g_done[group], 1)) return 0;
    return atomic_fetch_add(&g_lines, 1) < ST_CAP;
}

/* A milestone with no argument beyond the fact that it happened. */
static void hit(int group)
{
    if (claim(group)) tl_log_line("startup-trace: %s", k_group_name[group]);
}

static void h_metadata(uint64_t *r)     { (void)r; hit(G_METADATA); }
static void h_init_enter(uint64_t *r)   { (void)r; hit(G_INIT_ENTER); }
static void h_init_return(uint64_t *r)  { (void)r; hit(G_INIT_RETURN); }
static void h_gc_restart(uint64_t *r)   { (void)r; hit(G_GC_RESTART); }
static void h_swap(uint64_t *r)         { (void)r; hit(G_SWAP); }
static void h_choreo(uint64_t *r)       { (void)r; hit(G_CHOREO); }
static void h_frame_cb(uint64_t *r)     { (void)r; hit(G_FRAME_CB); }
static void h_asset_worker(uint64_t *r) { (void)r; hit(G_ASSET_WORKER); }

static void h_gc_suspend(uint64_t *r)
{
    (void)r;
    if (!claim(G_GC_SUSPEND)) return;
    tl_log_line("startup-trace: %s: the collector signalled a thread to stop (pthread_kill(t, SIGPWR) "
                "at libil2cpp.so+0x23bc640)", k_group_name[G_GC_SUSPEND]);
}

static void h_gc_wait(uint64_t *r)
{
    (void)r;
    if (!claim(G_GC_WAIT)) return;
    tl_log_line("startup-trace: %s: the collector entered its wait-for-the-world-to-stop loop "
                "(libil2cpp.so+0x23bd188), first wait", k_group_name[G_GC_WAIT]);
}

static void h_gc_resend(uint64_t *r)
{
    /* w24 (regs[24]'s low half) is the signal count the timeout path is about to print. */
    if (!claim(G_GC_RESEND)) return;
    tl_log_line("startup-trace: %s: the wait timed out and %u suspend signal(s) were RESENT -- the "
                "world did not stop", k_group_name[G_GC_RESEND], (unsigned)(r[24] & 0xffffffffu));
}

static void h_class_init(uint64_t *r)
{
    int n = atomic_fetch_add(&g_class_lines, 1);
    if (n >= ST_CLASS_LINES) return;
    if (atomic_fetch_add(&g_lines, 1) >= ST_CAP) return;
    atomic_store(&g_done[G_CLASS_INIT], 1);
    const char *nm = g_class_name_fn ? g_class_name_fn((void *)(uintptr_t)r[0]) : NULL;
    tl_log_line("startup-trace: %s #%d: %s (class %p, libil2cpp.so+0x239fa40)", k_group_name[G_CLASS_INIT],
                n + 1, nm ? nm : "-- name unresolved --", (void *)(uintptr_t)r[0]);
}

static void h_worker(uint64_t *r)
{
    if (!claim(G_WORKER)) return;
    /* x2 holds the start routine this call is about to hand pthread_create. */
    const char *libname = NULL; const void *sym = NULL;
    const char *nm = tl_ld_symbol_at_any((const void *)(uintptr_t)r[2], &libname, &sym);
    tl_log_line("startup-trace: %s: libunity.so created its first thread (start routine %s, %p)",
                k_group_name[G_WORKER], nm && libname ? nm : "-- symbol unresolved --", (void *)(uintptr_t)r[2]);
}

static const st_site k_sites[] = {
    { "libil2cpp.so", 0x238f4cc, 0xaa0003f5u, G_METADATA,    h_metadata,    "metadata load (global-metadata.dat)" },
    { "libil2cpp.so", 0x234864c, 0xa9bf4ffeu, G_INIT_ENTER,  h_init_enter,  "il2cpp_init entry (exported)" },
    { "libil2cpp.so", 0x234866c, 0x12000000u, G_INIT_RETURN, h_init_return, "il2cpp_init after Runtime::Init" },
    { "libil2cpp.so", 0x239fa40, 0xd101c3ffu, G_CLASS_INIT,  h_class_init,  "il2cpp_runtime_class_init's body" },
    { "libil2cpp.so", 0x23bc63c, 0xb94a6701u, G_GC_SUSPEND,  h_gc_suspend,  "GC suspend signal send" },
    { "libil2cpp.so", 0x23bc89c, 0xb94a6b01u, G_GC_RESTART,  h_gc_restart,  "GC restart signal send" },
    { "libil2cpp.so", 0x23bd1b4, 0xeb19039fu, G_GC_WAIT,     h_gc_wait,     "GC wait-to-stop loop" },
    { "libil2cpp.so", 0x23bd234, 0xaa1703e0u, G_GC_RESEND,   h_gc_resend,   "GC resend-on-timeout path" },
    { "libunity.so",  0x727578,  0xf81f0ffeu, G_SWAP,        h_swap,        "eglSwapBuffers call site 1" },
    { "libunity.so",  0x736b84,  0xaa1c03e1u, G_SWAP,        h_swap,        "eglSwapBuffers call site 2" },
    { "libunity.so",  0x744118,  0x91003260u, G_CHOREO,      h_choreo,      "AChoreographer_getInstance call" },
    { "libunity.so",  0x744188,  0xaa1303e2u, G_FRAME_CB,    h_frame_cb,    "postFrameCallback call" },
    { "libunity.so",  0x744164,  0xaa1303e2u, G_FRAME_CB,    h_frame_cb,    "postFrameCallback64 call 1" },
    { "libunity.so",  0x7441d0,  0xaa1303e2u, G_FRAME_CB,    h_frame_cb,    "postFrameCallback64 call 2" },
    { "libunity.so",  0x72e3c0,  0xaa1503e3u, G_WORKER,      h_worker,      "pthread_create call" },
    { "libunity.so",  0x5af46c,  0xa9bf4ffeu, G_ASSET_WORKER, h_asset_worker, "AssetGarbageCollectorHelper startup" },
};

#define N_SITES ((int)(sizeof(k_sites) / sizeof(k_sites[0])))

/* Follow one `b` at `vaddr`; used to confirm a site really is the body of an exported thunk. */
static int follow_b(tl_lib *lib, uint64_t vaddr, uint64_t *target)
{
    uint32_t w = 0;
    if (!tl_ld_read(lib, vaddr, &w, 4)) return 0;
    if ((w & 0xfc000000u) != 0x14000000u) return 0;
    int64_t imm = (int64_t)(w & 0x03ffffffu);
    if (imm & (1 << 25)) imm -= (1 << 26);
    *target = vaddr + imm * 4;
    return 1;
}

/* ------------------------------------------------------------------ installing */

void tl_startup_trace_on_load(tl_lib *lib)
{
    if (!lib) return;
    const char *name = tl_ld_lib_name(lib);
    if (!name) return;
    if (strcmp(name, "libil2cpp.so") && strcmp(name, "libunity.so")) return;

    if (!strcmp(name, "libil2cpp.so")) {
        g_class_name_fn = (class_name_fn)tl_ld_sym(lib, "il2cpp_class_get_name");
        /* The class-init site is the body an exported thunk branches to; confirm it, so the log can
         * name the symbol it is standing in for rather than assert it. */
        uint64_t thunk = 0;
        if (follow_b(lib, 0x2349098, &thunk))
            tl_log_line("startup-trace: libil2cpp.so+0x2349098 (exported il2cpp_runtime_class_init) "
                        "branches to +%#llx: the class-init site is %s",
                        (unsigned long long)thunk, thunk == 0x239fa40 ? "it" : "NOT it -- that site was not armed");
    }

    int li = !strcmp(name, k_lib_name[ST_LIB_IL2CPP]) ? ST_LIB_IL2CPP : ST_LIB_UNITY;
    int armed = 0, failed = 0, total = 0;
    char list[768];
    list[0] = 0;
    for (int i = 0; i < N_SITES; i++) {
        const st_site *s = &k_sites[i];
        if (strcmp(s->lib, name)) continue;
        total++;
        uint32_t w = 0;
        if (!tl_ld_read(lib, s->vaddr, &w, 4) || w != s->word) {
            tl_log_line("startup-trace: NOT ARMED %s+%#llx (%s): bad instruction word -- the image holds "
                        "%#010x, this build's site is %#010x (the game's library has changed)",
                        name, (unsigned long long)s->vaddr, s->what, w, s->word);
            failed++;
            continue;
        }
        if (!tl_ld_probe(lib, s->vaddr, s->cb)) {
            tl_log_line("startup-trace: NOT ARMED %s+%#llx (%s): out of branch range -- the loader could not "
                        "place a probe here (no stub room left, or the site is beyond the stub pool's reach)",
                        name, (unsigned long long)s->vaddr, s->what);
            failed++;
            continue;
        }
        armed++;
        if (list[0]) strncat(list, ", ", sizeof(list) - strlen(list) - 1);
        strncat(list, s->what, sizeof(list) - strlen(list) - 1);
    }
    atomic_store(&g_lib_seen[li], 1);
    atomic_store(&g_lib_armed[li], armed);
    atomic_store(&g_lib_total[li], total);
    atomic_fetch_add(&g_armed_libs, 1);
    atomic_fetch_add(&g_armed_sites, armed);
    atomic_fetch_add(&g_failed_sites, failed);
    tl_log_line("startup-trace: armed %d of %d site(s) in %s (base %p)%s -- %s", armed, total, name,
                tl_ld_lib_base(lib), failed ? " (failures logged above)" : "", list[0] ? list : "(none)");
}

/* -------------------------------------------------------------------- arming */

void tl_startup_trace_arm(void)
{
    static atomic_int armed;
    if (atomic_exchange(&armed, 1)) return;
    tl_ld_set_load_hook(tl_startup_trace_on_load);
    tl_log_line("startup-trace: armed: the loader's post-relocation hook is in place, so the "
                "breadcrumbs land in libil2cpp.so and libunity.so before the engine can run them, "
                "whatever engine driver the app arrived through (one line per milestone, once, cap %d)", ST_CAP);
}

/* ------------------------------------------------- the engine driver's milestone */

void tl_startup_trace_render(int phase, unsigned long long ns)
{
    if (phase == 0) {
        if (claim(G_RENDER_ENTER))
            tl_log_line("startup-trace: %s: the render thread entered nativeRender for the first time "
                        "(the player loop's first iteration)", k_group_name[G_RENDER_ENTER]);
    } else {
        if (claim(G_RENDER_RETURN))
            tl_log_line("startup-trace: %s: nativeRender returned after %llu ms",
                        k_group_name[G_RENDER_RETURN], ns / 1000000ull);
    }
}

/* ------------------------------------------------------------------- the report */

void tl_startup_trace_report(void)
{
    if (atomic_exchange(&g_reported, 1)) return;

    /* A target library the hook never saw is reported SITE BY SITE, so "the trace produced nothing" always
     * has a named cause in the log (bad instruction word, out of branch range, or the library never arrived)
     * instead of an absence a reader has to infer. */
    for (int l = 0; l < ST_LIB_COUNT; l++) {
        if (atomic_load(&g_lib_seen[l])) continue;
        for (int i = 0; i < N_SITES; i++) {
            const st_site *s = &k_sites[i];
            if (strcmp(s->lib, k_lib_name[l])) continue;
            tl_log_line("startup-trace: NOT ARMED %s+%#llx (%s): library not seen -- the loader never loaded "
                        "%s this session, so this site was never placed",
                        s->lib, (unsigned long long)s->vaddr, s->what, k_lib_name[l]);
        }
    }

    if (!atomic_load(&g_armed_libs)) {
        tl_log_line("startup-trace: how far startup got: nothing was traced -- the loader's post-relocation "
                    "hook saw neither %s nor %s (library not seen), so this run says nothing about the engine's "
                    "own startup", k_lib_name[ST_LIB_IL2CPP], k_lib_name[ST_LIB_UNITY]);
        return;
    }

    char libs[256];
    int n = 0;
    libs[0] = 0;
    for (int l = 0; l < ST_LIB_COUNT; l++) {
        if (!atomic_load(&g_lib_seen[l])) continue;
        n += snprintf(libs + n, sizeof(libs) - (size_t)n, "%s%s %d/%d armed", n ? ", " : "",
                      k_lib_name[l], atomic_load(&g_lib_armed[l]), atomic_load(&g_lib_total[l]));
        if (n >= (int)sizeof(libs)) { n = (int)sizeof(libs) - 1; break; }
    }

    char missing[512];
    int reached = 0, unseen = 0;
    missing[0] = 0;
    for (int g = 0; g < G_COUNT; g++) {
        if (atomic_load(&g_done[g])) { reached++; continue; }
        unseen++;
        if (missing[0]) strncat(missing, ", ", sizeof(missing) - strlen(missing) - 1);
        strncat(missing, k_group_name[g], sizeof(missing) - strlen(missing) - 1);
    }
    if (unseen)
        tl_log_line("startup-trace: how far startup got: %d of %d milestones reached, %d line(s) printed; "
                    "libraries: %s; NEVER REACHED: %s", reached, (int)G_COUNT, atomic_load(&g_lines), libs, missing);
    else
        tl_log_line("startup-trace: how far startup got: every one of the %d milestones was reached "
                    "(%d line(s) printed; libraries: %s)", (int)G_COUNT, atomic_load(&g_lines), libs);
}
