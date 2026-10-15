/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-unity-app.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <TargetConditionals.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/arm/thread_status.h>
#include <mach/vm_map.h>
#endif
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#include "husk-tl-bionic.h"
#include "husk-tl-internal.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"
#include "husk-tl-godot.h"
#include "husk-tl-unity.h"
#include "husk-tl-audio.h"
#include "husk-tl-geode.h"

/* Geode for the next cocos2d-x launch: its release zip and its launcher's APK (husk-tl-geode.c). Empty: off. */
static char g_geode_zip[1024], g_geode_launcher[1024];
#include "husk-tl-cocos.h"
#include "husk-tl-gameactivity.h"
#include "husk-tl-gamepad.h"
#include "husk-tl-sdl.h"
#include "husk-tl-flutter.h"
#include "husk-tl-flutter-text.h"
#include "husk-tl-gamemaker.h"
#include "husk-tl-dvm.h"
#include "husk-tl-dvm-javaapp.h"
#include "husk-tl-nativeactivity.h"
#include "husk-tl-gtasa.h"
#include "husk-tl-vulkan.h"

void tl_hle_set_ca_bundle(const char *path);
extern int tl_log_sink_fd;

/* The session-reset contract (declared in the owning engines' headers: husk-tl-dvm*.c, husk-tl-ld.c). Declared here too
 * so this file is correct by construction until those land -- a later identical declaration is a harmless redeclaration
 * in C. Do NOT implement these here. */
extern int  tl_javaapp_reset(void);     /* husk-tl-dvm-javaapp.h: stop the Java app's threads and drain its per-app state */
extern bool tl_dvm_end_app(void);       /* husk-tl-dvm.h: drop the app's dexes and detach the classes they defined */
extern void tl_ld_end_session(void);    /* husk-tl-ld.h: new linker session generation; stale mappings unreachable by name */
extern int  tl_ld_stale_count(void);    /* husk-tl-ld.h: libraries from an earlier session still reachable by name */

enum { ENGINE_UNITY = 0, ENGINE_COCOS = 1, ENGINE_GAMEACTIVITY = 2, ENGINE_SDL = 3, ENGINE_UE4 = 4, ENGINE_GTA = 5, ENGINE_GODOT = 6, ENGINE_FLUTTER = 7, ENGINE_GAMEMAKER = 8, ENGINE_JAVA = 9 };

/* Where the Java runtime is: the ART, i18n, tzdata and conscrypt APEX folders and husk-framework.dex (scripts/build_java_runtime.sh). */
static char g_java_root[1024];
static float g_java_density = 3.0f;
static int g_java_insets[4];
static bool g_java_night;
static struct {
    void (*keyboard)(int show, int input_type, int ime_options);
    void (*set_clipboard)(const char *utf8);
    char *(*get_clipboard)(void);
    void (*share)(const char *utf8);
    void (*orientation)(int android_orientation);
} g_java_host;

static unsigned long engine_frames(void);

static struct {
    atomic_int state;
    int engine;
    char apk[1024], data[1024], package[160], angle[1024], ca[1024], vulkan[1024];
    char extra[15][1024];
    int nextra;
    void *layer;
    int width, height;
} A;

/* -------------------------------------------------------------- crash report */

static int find_lib(uintptr_t bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    struct { uintptr_t addr; const char *name; uintptr_t off; } *r = user;
    const struct { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; } *p = phdr;
    for (unsigned i = 0; i < phnum; i++)
        if (p[i].type == 1 && r->addr >= bias + p[i].vaddr && r->addr < bias + p[i].vaddr + p[i].memsz) {
            r->name = name; r->off = r->addr - bias;
            return 1;
        }
    return 0;
}

static void where(char *out, size_t n, uintptr_t addr)
{
    struct { uintptr_t addr; const char *name; uintptr_t off; } r = { addr, NULL, 0 };
    tl_ld_iterate(find_lib, &r);
    if (r.name) snprintf(out, n, "%s+%#lx", r.name, (unsigned long)r.off);
    else snprintf(out, n, "%#lx", (unsigned long)addr);
}

static struct sigaction g_prev[32];

/* One line, once, naming a branch target the guest fetched in the WRITABLE view: the target, its executable-view
 * equivalent, the caller (library+offset), and whether the loader considers that target code. A writable-view address is
 * not a page the CPU fetches, so a fetch there is a pointer-provenance bug (a code pointer produced in the wrong view),
 * not a permission the guest asked for: the caller names who handed it out, known_code says whether it was a real
 * function reached in the wrong view (a layer bug) or never code at all (a guest bug). */
static void write_view_pointer(uintptr_t pc, uintptr_t lr)
{
    if (!tl_xmem_contains((const void *)pc) || tl_xmem_is_rx((const void *)pc)) return;   /* not a writable-view fetch */
    uintptr_t rx = (uintptr_t)((const char *)pc - tl_xmem_delta());
    if (!tl_xmem_contains((const void *)rx)) return;                                      /* no executable view to name */
    static int said;
    if (said++ >= 4) return;                                                              /* a few per run, then quiet */
    const char *lib = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at((const void *)lr, &lib, &sa);
    if (!lib) { const tl_lib *cl = tl_ld_lib_of((const void *)lr); if (cl) lib = tl_ld_lib_name(cl); }
    tl_log_line("husk: write-view pointer target=%p executable=%p caller=%s %s+%#lx known_code=%d",
                (void *)pc, (void *)rx, lib ? lib : "?", sym ? sym : "?",
                sa ? (unsigned long)(lr - (uintptr_t)sa) : 0ul, tl_ld_va_is_code((const void *)rx));
}

/* One line answering the one question robot gf's SIGBUS cannot: is the unbacked page the fault address lands on INSIDE
 * the span the loader/allocator emitted (the layer owns it and must back it), or BEYOND it (nothing the loader wrote
 * touched it, so this reserved StikDebug region cannot serve as the guest arena)? The offset is measured from the
 * executable view's base, the same unit tl_xmem_used() is in. No side effects, fatal path only.
 *
 *   offset <  used  -> INSIDE the emitted span: the allocator handed out this page with its bump pointer, so the layer
 *                     must back the WHOLE tl_xmem_alloc span at alloc time through the writable twin.
 *   offset >= used  -> BEYOND the emitted span: nothing the loader wrote, so refuse StikDebug's region as the guest
 *                     arena (make grant_own_arena a requirement) or verify the per-page M<addr>,1:69 sweep ran. */
static void arena_page_span(uintptr_t addr)
{
    if (!tl_xmem_contains((const void *)addr)) return;
    uintptr_t base = (uintptr_t)tl_xmem_region_base();
    unsigned long off = (unsigned long)(addr - base);
    size_t used = tl_xmem_used(), size = tl_xmem_size();
    tl_log_line("husk: arena page backing: fault offset +%#lx from the arena base %p; the bump allocator has handed out "
                "%zu KiB of %zu KiB (used=%zu size=%zu) -> %s",
                off, (void *)base, used >> 10, size >> 10, used, size,
                off < used
                    ? "INSIDE the emitted span -- the loader/allocator owns this page, so the layer is obliged to back it"
                    : "BEYOND the emitted span -- nothing the loader wrote, so this reserved region cannot be the guest arena (or the per-page M<addr>,1:69 sweep did not run)");
}

static void fault_address_region(uintptr_t pc)
{
    bool in_rx = tl_xmem_is_rx((const void *)pc);
    bool in_rw = !in_rx && tl_xmem_contains((const void *)pc);
    const char *view = in_rx ? "executable view" : in_rw ? "writable view" : "outside the region";
#if defined(__APPLE__)
    vm_address_t addr = (vm_address_t)pc; vm_size_t sz = 0; natural_t depth = 0;
    vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = vm_region_recurse_64(mach_task_self(), &addr, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt);
    if (kr != KERN_SUCCESS) {
        tl_log_line("husk: fetch fault at %p (%s): vm_region_recurse_64 returned %d (%s) -- nothing is mapped there, so the target was never code (or the region has a hole)",
                    (void *)pc, view, (int)kr, mach_error_string(kr));
        arena_page_span(pc);
        return;
    }
    tl_log_line("husk: fetch fault at %p (%s): entry %p..%p prot %d max %d depth %u tag %u -> %s",
                (void *)pc, view, (void *)addr, (void *)(addr + sz), (int)info.protection,
                (int)info.max_protection, (unsigned)depth, info.user_tag,
                (info.protection & 0x4) ? "names execute, so this fault is NOT a permission (the page is mapped executable and still did not fetch)"
                                        : "no execute (the page is mapped but not executable)");
    arena_page_span(pc);
#else
    tl_log_line("husk: fetch fault at %p (%s)", (void *)pc, view);
    arena_page_span(pc);
#endif
}

/*
 * Is this fault the GUEST failing in its own arena -- the shape robot gf died on? The region the layer makes for the
 * guest holds ALL of the guest's code and its JIT arena, so a fault whose FAULT ADDRESS is inside that region is the
 * APP's memory failing, not Husk's, whichever thread reported it. Two shapes reach here:
 *
 *   - a FETCH: the guest's compiled code branched (a computed `blr x8`) to an address in the arena that will not
 *     fetch; pc == fault address, both inside the region.
 *   - a DATA access: the guest's own code does a load or store to an address inside the region and that page will not
 *     be served; the pc is the guest library OUTSIDE the region, so a test that required pc == addr let it through as
 *     a host crash and the process died.
 *
 * Either way it is the APP's failure: it must end the game SESSION and leave Husk standing -- the rule the guest exit
 * family already follows (A GUEST'S FAILURE MUST NOT KILL THE HOST). Otherwise the report built to explain the run goes
 * down with the process. A fault whose address is NOT in the region is still a host crash. `why` is filled with a
 * one-line reason for the log, the UI and the persisted verdict file.
 */
static bool guest_arena_fault(siginfo_t *info, ucontext_t *uc, char *why, size_t whyn)
{
    uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    uintptr_t addr = (uintptr_t)info->si_addr;
    if (!tl_xmem_arena_fetch_fault(pc, addr)) return false;
    char at[200];
    where(at, sizeof(at), pc);
    if (pc == addr)
        snprintf(why, whyn, "the app's own code branched to %s in the guest arena and the page will not fetch (it is inside the arena the layer made, and iOS refuses to execute it on this device)", at);
    else
        snprintf(why, whyn, "the app's own code at %s touched %p inside the guest arena and the page would not be served (the address lies in the region the layer made for the guest -- its libraries and its JIT arena -- and this device gives no backing there)", at, (void *)addr);
    return true;
}

/* A fatal fault prints where it happened -- the guest library and offset of the faulting instruction and of its caller
 * -- before the app's own crash handling (which keeps the log file) runs. */
static void on_fatal(int sig, siginfo_t *info, void *uctx)
{
    ucontext_t *uc = uctx;
    char tn[40] = "", a[200], b[200], c[200];
    pthread_getname_np(pthread_self(), tn, sizeof(tn));
    where(a, sizeof(a), (uintptr_t)uc->uc_mcontext->__ss.__pc);
    where(b, sizeof(b), (uintptr_t)uc->uc_mcontext->__ss.__lr);
    where(c, sizeof(c), (uintptr_t)info->si_addr);
    tl_log_line("=== FATAL signal %d on thread '%s': fault address %p (%s)", sig, tn, info->si_addr, c);
    tl_log_line("    pc %s", a);
    tl_log_line("    lr %s", b);
    write_view_pointer((uintptr_t)uc->uc_mcontext->__ss.__pc, (uintptr_t)uc->uc_mcontext->__ss.__lr);
    fault_address_region((uintptr_t)info->si_addr);
    /* Which register held the fault address: a `blr xN` into a computed target is how robot gf died. Only when the fault
     * IS the fetch (pc == fault address), else a register holding the address is just an operand of a data fault. */
    if ((uintptr_t)info->si_addr == (uintptr_t)uc->uc_mcontext->__ss.__pc) {
        for (int i = 0; i <= 28; i++) {
            if ((uintptr_t)uc->uc_mcontext->__ss.__x[i] == (uintptr_t)info->si_addr) {
                tl_log_line("    the fault address was held in x%d (a computed `blr x%d` target, not a call to a fixed address)", i, i);
                break;
            }
        }
    }
    uintptr_t fp = uc->uc_mcontext->__ss.__fp;
    for (int i = 0; i < 12 && fp && (fp & 7) == 0 && fp > 0x100000000ull; i++) {
        uintptr_t *f = (uintptr_t *)fp;
        char w[200];
        where(w, sizeof(w), f[1]);
        tl_log_line("    frame %s", w);
        if (f[0] <= fp) break;
        fp = f[0];
    }
    for (int i = 0; i < 29; i += 4)
        tl_log_line("    x%d=%#llx x%d=%#llx x%d=%#llx x%d=%#llx", i, uc->uc_mcontext->__ss.__x[i], i + 1, uc->uc_mcontext->__ss.__x[i + 1],
                    i + 2, i + 2 < 29 ? uc->uc_mcontext->__ss.__x[i + 2] : 0ull, i + 3, i + 3 < 29 ? uc->uc_mcontext->__ss.__x[i + 3] : 0ull);
    /*
     * A guest fault INSIDE THE ARENA is the GUEST's failure, not Husk's, so it ends the game session and not the
     * process: the full report above is printed unchanged, then the run is unwound the way a guest exit() unwinds it
     * (tl_guest_exit_unwind), and the reason is written to the container for the next launch.
     *
     * Only the faulting thread ends, and only because it cannot continue: the page the guest touched supplies no
     * readable content, so there is NO "resume with a defined value" here the way an unserviced brk returns 0. So:
     * end the calling thread (pthread_exit, exactly what tl_guest_exit_unwind does -- never a cancel or join of
     * anyone else's thread), and leave Husk standing with the reason recorded. Nothing is unmapped by the layer.
     */
    {
        char gf[240];
        if (guest_arena_fault(info, uc, gf, sizeof(gf))) {
            tl_xmem_note_guest_fault(gf, (uintptr_t)info->si_addr);
            tl_log_line("husk: GUEST FAULT: %s; this is the APP failing, so the app is unwound and Husk stays "
                        "for the next game (never the host process; the verdict row and the log above say why)",
                        gf);
            /* End the RUN, and only by CAS: a session close already in flight (STOPPING) or the
             * closed gate (IDLE) is never clobbered -- the close finishes with the gate open. */
            if (!tl_guest_exit_close_in_flight()) {
                int cur = atomic_load(&A.state);
                if (cur != HUSK_UNITY_IDLE && cur != HUSK_UNITY_STOPPING) {
                    int expect_cur = cur;
                    atomic_compare_exchange_strong(&A.state, &expect_cur, HUSK_UNITY_FAILED);
                }
            }
            tl_javaapp_request_exit();
            tl_javaapp_thread_leaving();
            pthread_exit(NULL);
            /* never returns */
        }
    }
    if (g_prev[sig].sa_flags & SA_SIGINFO) {
        if (g_prev[sig].sa_sigaction) { g_prev[sig].sa_sigaction(sig, info, uctx); return; }
    } else if (g_prev[sig].sa_handler != SIG_DFL && g_prev[sig].sa_handler != SIG_IGN && g_prev[sig].sa_handler) {
        g_prev[sig].sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);                       /* nobody else handles it: re-fault with the default action */
}

static void install_crash_reporter(void)
{
    static bool done;
    if (done) return;
    done = true;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL };      /* not SIGTRAP: the app's brk guard owns it */
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, &g_prev[sigs[i]]);
}

/* ------------------------------------------------------------------- launch */

/* A guest exit()/_exit() ends the app, not Husk, and ends the thread that asked (the exit shim already named the call).
 * During a session close the close owns the state machine, so this does not touch it; only the calling thread is ended.
 * The app's own teardown has already run by the time this is reached. */
static void guest_exit(int status)
{
    tl_log_line("native: the app exited (%d)", status);
    tl_javaapp_request_exit();
    tl_javaapp_thread_leaving();
    /* Promote to ENDED unless a session close already owns the state (STOPPING or IDLE, and the close will open the
     * gate itself). A blind store would clobber the close's marker or re-close the gate it just opened, so CAS from the
     * value just read. */
    if (!tl_guest_exit_close_in_flight()) {
        int cur = atomic_load(&A.state);
        if (cur != HUSK_UNITY_IDLE && cur != HUSK_UNITY_STOPPING) {
            int expect_cur = cur;
            atomic_compare_exchange_strong(&A.state, &expect_cur, HUSK_UNITY_ENDED);
        }
    }
    pthread_exit(NULL);
}

/* The swapped-in stall probe, defined below near the engine/state names it prints. */
static void tl_swapstall_tick(unsigned long frames, bool running);

/* Says every few seconds that the engine is alive and how much memory the phone says is left: without it a silent death
 * cannot be told from a frozen game, or a jetsam kill from a crash. */
static void *heartbeat_thread(void *arg)
{
    (void)arg;
    pthread_setname_np("husk-unity-hb");
    unsigned long last = 0;
    for (int tick = 0;; tick++) {
        if (tick < 40) usleep(500000); else sleep(3);          /* twice a second for the first twenty seconds */
        unsigned long f = engine_frames();
        /* The frame counter resets to 0 at a session close, so clamp: a drop to zero is "no new frames", never a
         * negative delta. */
        unsigned long delta = f >= last ? f - last : 0;
        last = f;
#if TARGET_OS_IPHONE
        tl_log_line("unity: alive: %lu frames (+%lu), %zu MiB left before jetsam", f, delta, os_proc_available_memory() >> 20);
#else
        tl_log_line("unity: alive: %lu frames (+%lu)", f, delta);
#endif
        /* A swapped-in app that presented a frame and then stopped drawing may be blocked on a layer lock a parked thread
         * holds (tl_swapstall_tick). This is where such a stall names itself, once per stall. */
        tl_swapstall_tick(f, atomic_load(&A.state) == HUSK_UNITY_RUNNING);
        { extern volatile struct { uint64_t count, x21, impl, mask; } tl_va_clobber;
          static uint64_t seen;
          if (tl_va_clobber.count != seen) {
              seen = tl_va_clobber.count;
              Dl_info di;
              const char *nm = dladdr((void *)tl_va_clobber.impl, &di) && di.dli_sname ? di.dli_sname : "?";
              tl_log_line("unity: a variadic shim's implementation (%s) changed callee-saved registers (%llu times; x21 then %#llx, diff mask %#llx)",
                          nm, (unsigned long long)seen, (unsigned long long)tl_va_clobber.x21, (unsigned long long)tl_va_clobber.mask);
          } }
        /* A run still RUNNING may be promoted to ENDED, and only by CAS: a blind store would clobber the close's
         * STOPPING marker (failing the close's gate CAS) or re-close the gate it just opened. RUNNING -> ENDED and
         * nothing else; husk_unity_state() does the same. */
        if ((A.engine == ENGINE_COCOS && tl_cocos_ended()) || (A.engine == ENGINE_GODOT && tl_godot_ended()) || (A.engine == ENGINE_GAMEMAKER && tl_gm_ended()) || (A.engine == ENGINE_JAVA && tl_javaapp_ended())) {
            int expect_running = HUSK_UNITY_RUNNING;
            atomic_compare_exchange_strong(&A.state, &expect_running, HUSK_UNITY_ENDED);
        }
        /* Stop for any state that means this run is over, including the session close (STOPPING) and the
         * closed gate (IDLE): the heartbeat has nothing left to say and must not log into a torn-down run. */
        int st = atomic_load(&A.state);
        if (st == HUSK_UNITY_ENDED || st == HUSK_UNITY_FAILED || st == HUSK_UNITY_STOPPING || st == HUSK_UNITY_IDLE) return NULL;
    }
}

/* The launcher <activity> (the one holding category LAUNCHER), as a JNI class name. A class named with a leading dot
 * (or no dot) belongs to the app's package. */
static uint32_t rd32(const uint8_t *p);
static uint16_t rd16(const uint8_t *p);
static bool pool_string(const uint8_t *pool, size_t pool_size, uint32_t index, char *out, size_t n);
static bool manifest_launcher_activity(const char *apk, const char *package, char *out, size_t cap)
{
    tl_zip z; char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) return false;
    bool ok = false;
    const tl_zip_entry *e = tl_zip_find(&z, "AndroidManifest.xml");
    const uint8_t *data; size_t len; bool owned = false;
    if (e && tl_zip_data(&z, e, 8u << 20, &data, &len, &owned, err, sizeof(err)) && len > 8 && rd16(data) == 0x0003) {
        const uint8_t *pool = NULL; size_t pool_size = 0;
        char activity[200] = "";
        for (size_t off = rd16(data + 2); off + 8 <= len && !ok; ) {
            uint16_t type = rd16(data + off); uint32_t size = rd32(data + off + 4);
            if (size < 8 || off + size > len) break;
            if (type == 0x0001) { pool = data + off; pool_size = size; }
            else if ((type == 0x0102 || type == 0x0103) && pool && size >= 24) {
                const uint8_t *el = data + off;
                char name[64] = "";
                pool_string(pool, pool_size, rd32(el + 20), name, sizeof(name));
                if (type == 0x0103) { if (!strcmp(name, "activity")) activity[0] = 0; }
                else {
                    uint16_t astart = rd16(el + 24), asize = rd16(el + 26), acount = rd16(el + 28);
                    for (unsigned i = 0; i < acount; i++) {
                        const uint8_t *at = el + 16 + astart + (size_t)i * asize;
                        char an[64] = "", av[200] = "";
                        if (!pool_string(pool, pool_size, rd32(at + 4), an, sizeof(an)) || strcmp(an, "name")) continue;
                        if (rd32(at + 8) != 0xFFFFFFFFu) pool_string(pool, pool_size, rd32(at + 8), av, sizeof(av));
                        if (!strcmp(name, "activity")) snprintf(activity, sizeof(activity), "%s", av);
                        else if (!strcmp(name, "category") && !strcmp(av, "android.intent.category.LAUNCHER") && activity[0]) ok = true;
                    }
                }
            }
            off += size;
        }
        if (ok) {
            char full[260];
            if (activity[0] == '.') snprintf(full, sizeof(full), "%s%s", package, activity);
            else if (!strchr(activity, '.')) snprintf(full, sizeof(full), "%s.%s", package, activity);
            else snprintf(full, sizeof(full), "%s", activity);
            for (char *c = full; *c; c++) if (*c == '.') *c = '/';
            snprintf(out, cap, "%s", full);
        }
    }
    if (owned) free((void *)data);
    tl_zip_close(&z);
    return ok;
}

/* Start the Dalvik runtime on the bundled boot class path and load libcore's natives. Java recursion is C recursion in
 * the interpreter, and class initialisation runs deep, so this runs on its own thread with a big stack. */
static void *java_boot(void *arg)
{
    bool *ok = arg;
    const char *r = g_java_root;
    static const char *const jars[] = { "core-oj", "core-libart", "okhttp", "bouncycastle", "apache-xml" };
    static char paths[10][1200];
    const char *boot[10]; int nb = 0;
    for (int i = 0; i < 5; i++) { snprintf(paths[nb], sizeof(paths[nb]), "%s/com.android.art/javalib/%s.jar", r, jars[i]); boot[nb] = paths[nb]; nb++; }
    snprintf(paths[nb], sizeof(paths[nb]), "%s/com.android.i18n/javalib/core-icu4j.jar", r); boot[nb] = paths[nb]; nb++;
    snprintf(paths[nb], sizeof(paths[nb]), "%s/com.android.conscrypt/javalib/conscrypt.jar", r); boot[nb] = paths[nb]; nb++;
    snprintf(paths[nb], sizeof(paths[nb]), "%s/husk-framework.dex", r); boot[nb] = paths[nb]; nb++;
    /* Apache HttpClient, which Android gives apps that target 27 or lower (or ask for it in their manifest) */
    snprintf(paths[nb], sizeof(paths[nb]), "%s/org.apache.http.legacy.jar", r); boot[nb] = paths[nb]; nb++;
    char v[1200];
    snprintf(v, sizeof(v), "%s/com.android.art", r); setenv("ANDROID_ART_ROOT", v, 1);
    snprintf(v, sizeof(v), "%s/com.android.i18n", r); setenv("ANDROID_I18N_ROOT", v, 1);
    snprintf(v, sizeof(v), "%s/com.android.tzdata", r); setenv("ANDROID_TZDATA_ROOT", v, 1);
    snprintf(v, sizeof(v), "%s", A.data); setenv("ANDROID_ROOT", v, 1);
    snprintf(v, sizeof(v), "%s", A.data); setenv("ANDROID_DATA", v, 1);
    tl_jni_init();
    if (!tl_dvm_start(boot, nb)) { tl_log_line("java: the runtime did not start"); *ok = false; return NULL; }
    snprintf(v, sizeof(v), "%s/com.android.i18n/lib64", r); tl_ld_add_search_dir(v);
    snprintf(v, sizeof(v), "%s/com.android.conscrypt/lib64", r); tl_ld_add_search_dir(v);
    snprintf(v, sizeof(v), "%s/com.android.art/lib64", r);
    if (!tl_dvm_load_natives(v)) tl_log_line("java: libcore's natives did not all load");
    *ok = true;
    return NULL;
}

static void *launch_thread(void *arg)
{
    (void)arg;
    pthread_setname_np("husk-native-start");
    tl_guest_exit_hook = guest_exit;
    {
        char path[1100];
        snprintf(path, sizeof(path), "%s/%s", A.data, A.engine == ENGINE_UNITY ? "unity-run.log" : "native-run.log");
        tl_log_sink_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    }
    install_crash_reporter();
    tl_hle_set_ca_bundle(A.ca);

    /* Controllers go to the engine that is running. (GameActivity registers its own as it starts.) */
    if (A.engine == ENGINE_UNITY) tl_unity_register_pad_sink();
    else if (A.engine == ENGINE_COCOS) tl_cocos_register_pad_sink();

    bool ok;
    if (A.engine == ENGINE_GAMEACTIVITY) {
        tl_ga_config cfg = {
            .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
            .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0,
        };
        tl_log_line("gameactivity: starting %s as %s, %dx%d", A.apk, A.package, A.width, A.height);
        tl_audio_install();
        ok = tl_ga_start(&cfg) && tl_ga_run();
    } else if (A.engine == ENGINE_SDL) {
        tl_ga_config cfg = {
            .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
            .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0,
        };
        char activity[260];
        if (!manifest_launcher_activity(A.apk, A.package, activity, sizeof(activity))) {
            tl_log_line("sdl: the manifest names no launcher activity");
            atomic_store(&A.state, HUSK_UNITY_FAILED);
            return NULL;
        }
        for (int i = 0; i < A.nextra; i++) if (!tl_sdl_add_package(A.extra[i])) tl_log_line("sdl: cannot add %s", A.extra[i]);
        /* SDL games may draw with Vulkan (SDL_WINDOW_VULKAN, or DXVK under a Direct3D port): MoltenVK, when the app named it */
        if (A.vulkan[0]) tl_vk_configure(A.vulkan, NULL, 0);
        tl_log_line("sdl: starting %s (%s) as %s, %dx%d (Vulkan: %s)", A.apk, activity, A.package, A.width, A.height, A.vulkan[0] ? "yes" : "no");
        tl_audio_install();
        ok = tl_sdl_start(&cfg, activity) && tl_sdl_run();
    } else if (A.engine == ENGINE_FLUTTER) {
        /* Flutter apps: no Vulkan given, so Impeller takes its OpenGL ES backend through ANGLE */
        tl_ga_config cfg = {
            .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
            .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0,
        };
        tl_log_line("flutter: starting %s as %s, %dx%d", A.apk, A.package, A.width, A.height);
        tl_audio_install();
        ok = tl_flutter_start(&cfg) && tl_flutter_run();
    } else if (A.engine == ENGINE_GAMEMAKER) {
        tl_gm_config cfg = {
            .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
            .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0,
        };
        tl_log_line("gamemaker: starting %s as %s, %dx%d", A.apk, A.package, A.width, A.height);
        tl_audio_install();
        ok = tl_gm_start(&cfg) && tl_gm_run();
    } else if (A.engine == ENGINE_JAVA) {
        tl_log_line("java: starting %s as %s, %dx%d (runtime %s)", A.apk, A.package, A.width, A.height, g_java_root);
        ok = false;
        if (g_java_root[0]) {
            pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
            pthread_t t;
            if (pthread_create(&t, &at, java_boot, &ok) == 0) pthread_join(t, NULL);
            pthread_attr_destroy(&at);
        } else tl_log_line("java: no runtime folder given");
        if (ok) {
            tl_audio_install();
            char fwres[1100];
            snprintf(fwres, sizeof(fwres), "%s/framework-res.apk", g_java_root);
            tl_javaapp_config cfg = {
                .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
                .density = g_java_density, .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL,
                .framework_res = fwres, .show_keyboard = g_java_host.keyboard, .set_clipboard = g_java_host.set_clipboard,
                .get_clipboard = g_java_host.get_clipboard, .share = g_java_host.share, .set_orientation = g_java_host.orientation,
                .insets = { g_java_insets[0], g_java_insets[1], g_java_insets[2], g_java_insets[3] }, .night_mode = g_java_night,
            };
            ok = tl_javaapp_start(&cfg);
        }
    } else if (A.engine == ENGINE_GODOT) {
        tl_godot_config cfg = {
            .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
            .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0,
        };
        tl_log_line("godot: starting %s as %s, %dx%d", A.apk, A.package, A.width, A.height);
        tl_audio_install();
        ok = tl_godot_start(&cfg) && tl_godot_run();
    } else if (A.engine == ENGINE_GTA) {
        tl_ga_config cfg = {
            .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
            .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0,
        };
        tl_log_line("gta: starting %s as %s, %dx%d", A.apk, A.package, A.width, A.height);
        tl_audio_install();
        ok = tl_gta_start(&cfg) && tl_gta_run();
    } else if (A.engine == ENGINE_UE4) {
        tl_ga_config cfg = {
            .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
            .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0,
        };
        if (A.vulkan[0]) tl_vk_configure(A.vulkan, NULL, 0);
        tl_log_line("ue4: starting %s as %s, %dx%d (Vulkan: %s)", A.apk, A.package, A.width, A.height, A.vulkan[0] ? A.vulkan : "none");
        tl_audio_install();
        ok = tl_na_start(&cfg) && tl_na_run();
    } else if (A.engine == ENGINE_COCOS) {
        tl_cocos_config cfg = {
            .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
            .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0,
        };
        tl_log_line("cocos: starting %s as %s, %dx%d", A.apk, A.package, A.width, A.height);
        tl_audio_install();
        tl_cocos_text_install();
        if (g_geode_zip[0]) tl_geode_configure(g_geode_zip, g_geode_launcher, A.data, 0);
        ok = tl_cocos_start(&cfg) && tl_cocos_run();
    } else {
        tl_unity_config cfg = {
            .apk_path = A.apk, .data_dir = A.data, .package_name = A.package, .width = A.width, .height = A.height,
            .metal_layer = A.layer, .angle_egl = A.angle, .angle_gles = NULL, .frame_dir = NULL, .frame_every = 0,
        };
        tl_log_line("unity: starting %s as %s, %dx%d", A.apk, A.package, A.width, A.height);
        tl_audio_install();
        ok = tl_unity_start(&cfg) && tl_unity_run();
    }
    if (!ok) {
        tl_log_line("native: the game could not be started");
        atomic_store(&A.state, HUSK_UNITY_FAILED);
        return NULL;
    }
    atomic_store(&A.state, HUSK_UNITY_RUNNING);
    pthread_t hb;
    if (pthread_create(&hb, NULL, heartbeat_thread, NULL) == 0) pthread_detach(hb);
    /* This thread loaded the game's libraries and ran their JNI_OnLoad -- on Android the UI thread, which never ends.
     * Some code relies on it (Geode 5 keeps thread-local state here and its cleanup took Husk down), so it stays, asleep. */
    for (;;) pause();
}

static bool launch(int engine, const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                   const char *angle_dylib, const char *ca_bundle)
{
    int expected = HUSK_UNITY_IDLE;
    if (!apk || !data_dir || !metal_layer || width <= 0 || height <= 0 || !angle_dylib) return false;
    if (!atomic_compare_exchange_strong(&A.state, &expected, HUSK_UNITY_STARTING)) return false;
    A.engine = engine;
    snprintf(A.apk, sizeof(A.apk), "%s", apk);
    snprintf(A.data, sizeof(A.data), "%s", data_dir);
    snprintf(A.angle, sizeof(A.angle), "%s", angle_dylib);
    snprintf(A.ca, sizeof(A.ca), "%s", ca_bundle ? ca_bundle : "");
    A.layer = metal_layer; A.width = width; A.height = height;
    if (!husk_unity_package_name(apk, A.package, sizeof(A.package))) snprintf(A.package, sizeof(A.package), "%s", engine == ENGINE_GAMEACTIVITY ? "com.mojang.minecraftpe" : engine == ENGINE_GTA ? "com.rockstargames.gtasa" : engine == ENGINE_UE4 ? "com.epicgames.ue4" : engine == ENGINE_SDL ? "com.sdl.game" : engine == ENGINE_COCOS ? "com.cocos.game" : engine == ENGINE_GODOT ? "com.godot.game" : engine == ENGINE_FLUTTER ? "com.flutter.app" : engine == ENGINE_GAMEMAKER ? "com.company.game" : engine == ENGINE_JAVA ? "com.java.app" : "com.unity.game");
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int rc = pthread_create(&t, &at, launch_thread, NULL);
    pthread_attr_destroy(&at);
    if (rc) { atomic_store(&A.state, HUSK_UNITY_FAILED); return false; }
    return true;
}

bool husk_unity_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                       const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_UNITY, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}

void husk_cocos_set_geode(const char *release_zip, const char *launcher_apk)
{
    snprintf(g_geode_zip, sizeof(g_geode_zip), "%s", release_zip ? release_zip : "");
    snprintf(g_geode_launcher, sizeof(g_geode_launcher), "%s", launcher_apk ? launcher_apk : "");
}

bool husk_cocos_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                       const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_COCOS, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}

bool husk_gameactivity_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                              const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_GAMEACTIVITY, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}

bool husk_sdl_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                     const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_SDL, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}
bool husk_godot_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                       const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_GODOT, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}
bool husk_gta_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                     const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_GTA, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}
bool husk_ue4_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                     const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_UE4, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}
bool husk_flutter_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                         const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_FLUTTER, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}
bool husk_gamemaker_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                           const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_GAMEMAKER, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}
int husk_gamemaker_orientation(const char *apk) { return tl_gm_orientation(apk); }
bool husk_java_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                      const char *angle_dylib, const char *ca_bundle)
{
    return launch(ENGINE_JAVA, apk, data_dir, metal_layer, width, height, angle_dylib, ca_bundle);
}
void husk_java_set_runtime(const char *root, float density)
{
    snprintf(g_java_root, sizeof(g_java_root), "%s", root ? root : "");
    if (density > 0) g_java_density = density;
}
void husk_java_set_host(void (*keyboard)(int show, int input_type, int ime_options), void (*set_clipboard)(const char *utf8),
                        char *(*get_clipboard)(void), void (*share)(const char *utf8), void (*orientation)(int android_orientation))
{
    g_java_host.keyboard = keyboard; g_java_host.set_clipboard = set_clipboard; g_java_host.get_clipboard = get_clipboard;
    g_java_host.share = share; g_java_host.orientation = orientation;
}
static bool java_running(void) { return atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_JAVA; }
void husk_java_set_insets(int left, int top, int right, int bottom, int keyboard)
{
    g_java_insets[0] = left; g_java_insets[1] = top; g_java_insets[2] = right; g_java_insets[3] = bottom;
    if (java_running()) tl_javaapp_set_insets(left, top, right, bottom, keyboard);
}
void husk_java_set_night_mode(int night) { g_java_night = night != 0; }
void husk_java_insert_text(const char *utf8) { if (java_running()) tl_javaapp_text(utf8); }
void husk_java_delete_backward(void) { if (java_running()) tl_javaapp_text_delete(); }
void husk_java_text_action(void) { if (java_running()) tl_javaapp_text_action(); }
void husk_java_keyboard_closed(void) { if (java_running()) tl_javaapp_keyboard_closed(); }
void husk_java_back(void) { if (atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_JAVA) tl_javaapp_back(); }
void tl_web_attach(void *screen_view, void *container, float scale, bool fresh);
bool tl_web_hit(float x, float y);
void husk_java_web_attach(void *screen_view, void *container, float scale, int fresh) { tl_web_attach(screen_view, container, scale, fresh != 0); }
int husk_java_web_hit(float x, float y) { return java_running() && tl_web_hit(x, y) ? 1 : 0; }
void husk_java_key(int android_keycode, int down) { if (atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_JAVA) tl_javaapp_key(android_keycode, down != 0); }
int husk_flutter_is_app(const char *apk) { return tl_flutter_is_app(apk) ? 1 : 0; }
void husk_flutter_set_pixel_ratio(float ratio) { tl_flutter_set_pixel_ratio(ratio); }
void husk_flutter_set_insets(int top, int right, int bottom, int left) { tl_flutter_set_insets(top, right, bottom, left); }
void husk_flutter_resize(int width, int height) { if (A.engine == ENGINE_FLUTTER) tl_flutter_resize(width, height); }
void husk_flutter_insert_text(const char *utf8) { tl_flutter_key_insert(utf8); }
void husk_flutter_delete_backward(void) { tl_flutter_key_delete(); }
void husk_flutter_text_action(void) { tl_flutter_key_action(); }
bool husk_flutter_text_multiline(void) { return tl_flutter_text_multiline(); }
void husk_flutter_set_keyboard_inset(int bottom) { tl_flutter_set_keyboard_inset(bottom); }
void husk_flutter_back(void) { if (A.engine == ENGINE_FLUTTER) tl_flutter_back(); }
void husk_flutter_set_keyboard_handler(void (*fn)(int show, int kind)) { tl_flutter_text_set_keyboard_handler(fn); }
void husk_flutter_set_close_handler(void (*fn)(void)) { tl_flutter_set_close_handler(fn); }
void tl_set_cacerts_dir(const char *dir);
void tl_set_system_fonts_dir(const char *dir);
/* The certificates and fonts Android keeps under /system, as copies the app carries (cacerts/, and fonts/ + etc/fonts.xml). */
void husk_native_set_system_files(const char *cacerts_dir, const char *fonts_root)
{
    if (cacerts_dir) tl_set_cacerts_dir(cacerts_dir);
    if (fonts_root) tl_set_system_fonts_dir(fonts_root);
}
/* Where MoltenVK is: Unreal's Vulkan renderer runs on it. Before the launch call. */
void husk_ue4_set_vulkan(const char *dylib) { snprintf(A.vulkan, sizeof(A.vulkan), "%s", dylib ? dylib : ""); }
void tl_set_shared_storage(const char *dir);
void husk_native_set_shared_storage(const char *dir) { tl_set_shared_storage(dir); }

/* Where the layer writes the last arena verdict (tl_xmem_set_verdict_path): a file in the app's
 * container, so the Settings "Guest arena" row can read why the last run's arena did or did not run
 * even after a fault took the process. The app gives the path before a game starts; the layer writes
 * it at measurement time and again if a guest fault ends the run. */
void husk_native_set_arena_verdict_path(const char *path) { tl_xmem_set_verdict_path(path); }

void husk_native_add_package(const char *apk)
{
    if (apk && A.nextra < 15 && atomic_load(&A.state) == HUSK_UNITY_IDLE) {
        snprintf(A.extra[A.nextra++], sizeof(A.extra[0]), "%s", apk);
        tl_ld_queue_split(apk);
    }
}
void husk_sdl_set_safe_insets(int left, int top, int right, int bottom) { tl_sdl_set_safe_insets(left, top, right, bottom); }
int husk_sdl_apk_is_portrait(const char *apk) { return tl_sdl_manifest_portrait(apk) ? 1 : 0; }
int husk_apk_orientation(const char *apk) { return tl_manifest_orientation(apk); }
int husk_apk_is_game(const char *apk) { return tl_manifest_is_game(apk) ? 1 : 0; }
bool tl_manifest_split_required(const char *apk);
int husk_apk_split_required(const char *apk) { return tl_manifest_split_required(apk) ? 1 : 0; }
void husk_sdl_set_keyboard_handler(void (*handler)(int action)) { tl_sdl_set_keyboard_handler(handler); }
void husk_sdl_commit_text(const char *utf8) { if (atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_SDL) tl_sdl_commit_text(utf8); }
void husk_sdl_key(int keycode, int down) { if (atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_SDL) tl_sdl_key(keycode, down != 0); }

/* ---------------------------------------------------------------- controllers */

void husk_gamepad_connect(int slot, const char *name) { tl_pad_connect(slot, name); }
void husk_gamepad_disconnect(int slot) { tl_pad_disconnect(slot); }
void husk_gamepad_update(int slot, unsigned buttons, float lx, float ly, float rx, float ry, float lt, float rt)
{
    tl_pad_state s = { .buttons = buttons, .lx = lx, .ly = -ly, .rx = rx, .ry = -ry, .lt = lt, .rt = rt };   /* iOS has y up; Android has it down */
    tl_pad_update(slot, &s);
}

void husk_cocos_set_keyboard_handler(void (*handler)(int action)) { tl_cocos_keyboard_hook = handler; }
void husk_cocos_set_open_url_handler(void (*handler)(const char *url)) { tl_cocos_open_url_hook = handler; }
void husk_cocos_insert_text(const char *utf8) { if (atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_COCOS) tl_cocos_insert_text(utf8); }
void husk_cocos_delete_backward(void) { if (atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_COCOS) tl_cocos_delete_backward(); }
void husk_cocos_key_down(int keycode) { if (atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_COCOS) tl_cocos_key_down(keycode); }
void husk_cocos_request_text(void (*cb)(const char *utf8)) { if (atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_COCOS) tl_cocos_request_content_text(cb); }

/* Minecraft and other GameActivity games: GameTextInput's field, typed into from the iPhone's keyboard. */
void husk_ga_set_keyboard_handler(void (*handler)(int action)) { tl_ga_set_keyboard_handler(handler); }
static bool ga_running(void) { return atomic_load(&A.state) == HUSK_UNITY_RUNNING && A.engine == ENGINE_GAMEACTIVITY; }
void husk_ga_insert_text(const char *utf8) { if (ga_running()) tl_ga_insert_text(utf8); }
void husk_ga_delete_backward(void) { if (ga_running()) tl_ga_delete_backward(); }
void husk_ga_editor_action(void) { if (ga_running()) tl_ga_editor_action(); }
void husk_ga_text(char *out, unsigned long cap) { if (cap) out[0] = 0; if (ga_running()) tl_ga_text_copy(out, cap); }

/* A game that renders Direct3D through DXVK is a PC game ported over, and its menus answer a keyboard, mouse or controller, not touch. */
bool husk_native_wants_controller(void)
{
    return tl_ld_find_lib("libdxvk_dxgi.so") || tl_ld_find_lib("libdxvk_d3d11.so") || tl_ld_find_lib("libdxvk_d3d9.so");
}

const char *husk_native_loaded_apk(void) { return atomic_load(&A.state) == HUSK_UNITY_IDLE ? NULL : A.apk; }

/* ---------------------------------------------------------------- session swap */

/* The session boundary: one app at a time; between two apps the old run is closed, its bookkeeping reset and the gate
 * opened again, in that order. Nothing here touches memory, a guest thread or the allocator -- reset the BOOKKEEPING,
 * never the memory. The verdict of the last close/verification is kept for the UI. */
static char g_swap_status[160] = "no session";
static int  g_swap_verdict = -1;

static const char *engine_name(int e)
{
    switch (e) {
    case ENGINE_UNITY: return "unity";
    case ENGINE_COCOS: return "cocos";
    case ENGINE_GAMEACTIVITY: return "gameactivity";
    case ENGINE_SDL: return "sdl";
    case ENGINE_UE4: return "ue4";
    case ENGINE_GTA: return "gta";
    case ENGINE_GODOT: return "godot";
    case ENGINE_FLUTTER: return "flutter";
    case ENGINE_GAMEMAKER: return "gamemaker";
    case ENGINE_JAVA: return "java";
    default: return "none";
    }
}

static const char *state_name(int s)
{
    switch (s) {
    case HUSK_UNITY_IDLE: return "idle";
    case HUSK_UNITY_STARTING: return "starting";
    case HUSK_UNITY_RUNNING: return "running";
    case HUSK_UNITY_FAILED: return "failed";
    case HUSK_UNITY_ENDED: return "ended";
    case HUSK_UNITY_STOPPING: return "stopping";
    default: return "?";
    }
}

/* --------------------------------------------------- the swapped-in stall, made to name itself */

/*
 * A session close QUARANTINES every guest thread it cannot stop: the thread keeps running guest code and may hold a layer
 * lock forever (husk_tl_session_close). The strongest evidence that a swap is what breaks the second app is that the SAME
 * app runs perfectly alone: a swap in EITHER order leaves the first app fine and the second on its first frame, and two
 * parked threads were enough (not a crowd). A swapped-in app that then needs one of those locks stalls and shows nothing
 * else. This probe is how that names itself instead of being inferred.
 *
 * The heartbeat calls it once a tick. When the app has presented at least one frame and then made no new one for a bounded
 * time it prints ONCE -- not once per poll -- what the layer can honestly know: which of the layer's own locks are held
 * and by which thread, and every guest thread still counted live, with the identity, the name it gave itself, the entry
 * it was created on, and (where the kernel will read it) the pc that says whether it is inside the allocator. It names
 * each parked thread, not a crowd: ONE thread holding a lock is the mechanism to test.
 *
 * It takes no lock -- the census and the ledger are lock-free -- so a probe that reported the stall cannot itself be what
 * hangs, and it never runs on a fault path. It only READS a thread's state (thread_get_state), which briefly stops the
 * thread to copy its registers; nothing is ended, joined, unmapped or woken.
 */
#define TL_SS_SECS_DEFAULT 8
static unsigned long g_ss_last_frames;
static struct timespec g_ss_last_progress;
static int g_ss_printed;                       /* one line per distinct stall, not per poll */
static int g_ss_secs = -1;

#if defined(__APPLE__)
/* The thread's pc, where the kernel will read it. False -- never a guessed value -- when it will not. */
static bool ss_thread_pc(unsigned long tid, uintptr_t *pc)
{
    mach_port_t tp = pthread_mach_thread_np((pthread_t)(uintptr_t)tid);
    if (!tp) return false;
    struct __darwin_arm_thread_state64 st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(tp, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) != KERN_SUCCESS) return false;
    *pc = (uintptr_t)st.__pc;
    return true;
}
#else
static bool ss_thread_pc(unsigned long tid, uintptr_t *pc) { (void)tid; (void)pc; return false; }
#endif

/* Append to `buf` at offset `w`, clamped so a chained snprintf cannot walk past `cap`. Returns the new offset. */
static int ss_put(char *buf, int w, size_t cap, const char *fmt, ...)
{
    if (w < 0) return 0;
    if ((size_t)w >= cap) return (int)cap - 1;
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf + w, cap - (size_t)w, fmt, ap);
    va_end(ap);
    if (r < 0) return w;
    return (size_t)r >= cap - (size_t)w ? (int)cap - 1 : w + r;
}

/* A guest address as image+symbol+offset, or a bare hex when no library claims it. */
static void ss_where(char *out, size_t n, uintptr_t a, bool is_pc)
{
    const char *lib = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at((const void *)a, &lib, &sa);
    if (lib)
        snprintf(out, n, "%s %s+%#lx", lib, sym ? sym : "?", sa ? (unsigned long)(a - (uintptr_t)sa) : 0ul);
    else if (is_pc && tl_xmem_is_rx((const void *)a))
        snprintf(out, n, "the arena +%#lx (executable view: it is running JIT'd guest code)", (unsigned long)(a - (uintptr_t)tl_xmem_region_base()));
    else
        snprintf(out, n, "%#lx", (unsigned long)a);
}

#define TL_SS_THREADS 160
static void ss_probe(unsigned long frames, double quiet_s)
{
    char line[2000];
    int w = 0;

    tl_guest_thread_info gt[TL_SS_THREADS];
    int ngt = tl_guest_thread_census(gt, TL_SS_THREADS);
    if (ngt > TL_SS_THREADS) ngt = TL_SS_THREADS;
    int cur = tl_guest_thread_epoch();
    int parked = 0;
    for (int i = 0; i < ngt; i++) if (gt[i].epoch < cur) parked++;

    tl_lock_info lk[TL_LOCK_COUNT];
    int nlk = tl_lock_ledger(lk, TL_LOCK_COUNT);
    int held_parked = -1;
    for (int i = 0; i < nlk; i++) {
        if (lk[i].depth <= 0) continue;
        for (int j = 0; j < ngt; j++)
            if (gt[j].epoch < cur && gt[j].tid == lk[i].owner) { held_parked = i; break; }
        if (held_parked >= 0) break;
    }

    w = ss_put(line, w, sizeof(line),
               "swap-stall: app presented %lu frame(s) then no new frame for %.1f s while RUNNING (engine %s, state %s) -- the difference from this app run alone is exactly what the swap left behind; layer locks:",
               frames, quiet_s, engine_name(A.engine), state_name(atomic_load(&A.state)));
    for (int i = 0; i < nlk; i++) {
        int is_parked = 0;
        if (lk[i].depth > 0) {
            for (int j = 0; j < ngt; j++)
                if (gt[j].epoch < cur && gt[j].tid == lk[i].owner) { is_parked = 1; break; }
        }
        w = ss_put(line, w, sizeof(line), " %s=%s", lk[i].name ? lk[i].name : "?", lk[i].depth > 0 ? "HELD" : "free");
        if (lk[i].depth > 0)
            w = ss_put(line, w, sizeof(line), "(%s '%s' from %s%s)", is_parked ? "PARKED THREAD" : "thread",
                       lk[i].owner_name, lk[i].site, is_parked ? " -- a thread from the swapped-out session" : "");
    }
    w = ss_put(line, w, sizeof(line),
               "; codewrite guard takes no layer lock (a signal-time store redirect, husk-tl-codewrite.c); %d guest thread(s) still counted live, %d from an earlier session (parked)",
               ngt, parked);
    w = ss_put(line, w, sizeof(line), "; parked threads:");
    int shown = 0;
    for (int j = 0; j < ngt; j++) {
        if (gt[j].epoch >= cur) continue;                       /* this session's own threads are not parked */
        if (w >= (int)sizeof(line) - 240) break;                /* leave room for the count and the verdict */
        char nm[32] = "?"; pthread_getname_np((pthread_t)(uintptr_t)gt[j].tid, nm, sizeof(nm));
        char ent[128]; ss_where(ent, sizeof(ent), (uintptr_t)gt[j].entry, false);
        uintptr_t pc = 0; bool have = ss_thread_pc(gt[j].tid, &pc);
        char pcs[128];
        if (have) ss_where(pcs, sizeof(pcs), pc, true); else snprintf(pcs, sizeof(pcs), "not readable by the layer");
        w = ss_put(line, w, sizeof(line), " #%d '%s' entry [%s] pc [%s];", shown + 1, nm, ent, pcs);
        shown++;
    }
    if (parked == 0)
        w = ss_put(line, w, sizeof(line), " none (no thread from an earlier session is still counted live)");
    else if (parked > shown)
        w = ss_put(line, w, sizeof(line), " (%d more parked thread(s) not shown: the line is bounded)", parked - shown);
    w = ss_put(line, w, sizeof(line), ";");

    if (held_parked >= 0)
        w = ss_put(line, w, sizeof(line),
                   " VERDICT: a thread parked from the session that was swapped out holds '%s' -- the swapped-in app blocks on it here, so the quarantine (a thread left mid-hold by the close) IS the deadlock, and the fix is to make the swap wait for or reclaim those locks rather than quarantine a thread mid-hold.",
                   lk[held_parked].name ? lk[held_parked].name : "a layer lock");
    else
        w = ss_put(line, w, sizeof(line),
                   " VERDICT: no parked thread holds any layer lock the layer can see, so this stall is NOT the swap's quarantine -- look next at the app's OWN first-frame path (its onResume/launch and its own asset/asset-file reads). A guest-level lock (a bionic pthread mutex, a JVM monitor) is not in this ledger: the pthread shim already names one that waits 5 s under TL_MUTEX_TRACE (husk-tl-bionic-pthread.c), the paired run for that case.");
    if (w > 0) tl_log_line("%s", line);
}

static void tl_swapstall_tick(unsigned long frames, bool running)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!running) { g_ss_last_frames = frames; g_ss_last_progress = now; g_ss_printed = 0; return; }
    if (frames != g_ss_last_frames) {          /* the app is drawing: any earlier stall ended, arm a fresh one */
        g_ss_last_frames = frames; g_ss_last_progress = now; g_ss_printed = 0; return;
    }
    if (frames == 0) { g_ss_last_progress = now; return; }   /* nothing presented yet: not a stall on a frame */
    if (g_ss_secs < 0) { const char *s = getenv("TL_SWAPSTALL_SECS"); g_ss_secs = s ? atoi(s) : TL_SS_SECS_DEFAULT; if (g_ss_secs <= 0) g_ss_secs = TL_SS_SECS_DEFAULT; }
    double quiet = (now.tv_sec - g_ss_last_progress.tv_sec) + (now.tv_nsec - g_ss_last_progress.tv_nsec) / 1e9;
    if (g_ss_printed || quiet < (double)g_ss_secs) return;
    g_ss_printed = 1;                          /* one line per distinct stall, not per poll */
    ss_probe(frames, quiet);
}

/* Can this engine go on to a different app in this process? Static, per engine: SWAP_OK (cooperative stop), SWAP_PARTIAL
 * (driver unwinds but a guest engine singleton survives, so only a DIFFERENT engine is safe next), or
 * SWAP_NEEDS_RESTART (no in-process unwind). The per-engine evidence and the exact stop path are in
 * engine-swap-capability.md, from the reconnaissance under .hermes/plans/engine-reset/. */
int husk_tl_swap_capability(int engine)
{
    switch (engine) {
    case ENGINE_JAVA:      return SWAP_OK;
    case ENGINE_COCOS:
    case ENGINE_GODOT:
    case ENGINE_GAMEMAKER:
    case ENGINE_UNITY:     return SWAP_PARTIAL;
    case ENGINE_GAMEACTIVITY:
    case ENGINE_SDL:
    case ENGINE_UE4:
    case ENGINE_GTA:
    case ENGINE_FLUTTER:   return SWAP_NEEDS_RESTART;
    default:               return SWAP_NEEDS_RESTART;   /* an engine we do not know: never claim it is swappable */
    }
}

int husk_tl_swap_verdict(void) { return g_swap_verdict; }
const char *husk_tl_swap_status(void) { return g_swap_status; }

int husk_tl_session_close(void)
{
    int state = atomic_load(&A.state);

    /* Already closed: idempotent, a second call does nothing (F §4). */
    if (state == HUSK_UNITY_IDLE) {
        g_swap_verdict = SWAP_OK;
        snprintf(g_swap_status, sizeof(g_swap_status), "no session");
        return SWAP_OK;
    }

    /* A launch is still starting: wait, bounded, for the launch thread to settle (plan §3 step 1). */
    if (state == HUSK_UNITY_STARTING) {
        int i;
        for (i = 0; i < 40 && (state = atomic_load(&A.state)) == HUSK_UNITY_STARTING; i++) {
            struct timespec ts = { 0, 50 * 1000 * 1000 };       /* 40 * 50 ms = 2 s */
            nanosleep(&ts, NULL);
        }
        if (state == HUSK_UNITY_STARTING) {
            tl_log_line("husk: session close refused (still starting after 2000 ms; the launch thread has not settled)");
            g_swap_verdict = SWAP_NEEDS_RESTART;
            snprintf(g_swap_status, sizeof(g_swap_status), "swap failed, relaunch needed");
            return SWAP_NEEDS_RESTART;
        }
        if (state == HUSK_UNITY_IDLE) {
            g_swap_verdict = SWAP_OK;
            snprintf(g_swap_status, sizeof(g_swap_status), "no session");
            return SWAP_OK;
        }
    }

    /* Take the run out of its state so a launch cannot race the reset: STOPPING is not IDLE, so the launch CAS is
     * refused while we work. */
    int from = state, taken = 0;
    for (int i = 0; i < 64; i++) {
        if (from == HUSK_UNITY_IDLE) { g_swap_verdict = SWAP_OK; snprintf(g_swap_status, sizeof(g_swap_status), "no session"); return SWAP_OK; }
        if (from == HUSK_UNITY_STOPPING) {
            tl_log_line("husk: session close refused (already stopping)");
            return g_swap_verdict >= 0 ? g_swap_verdict : SWAP_NEEDS_RESTART;
        }
        if (atomic_compare_exchange_strong(&A.state, &from, HUSK_UNITY_STOPPING)) { taken = 1; break; }
    }
    if (!taken) {
        tl_log_line("husk: session close refused (the state kept changing: %s)", state_name(atomic_load(&A.state)));
        g_swap_verdict = SWAP_NEEDS_RESTART;
        snprintf(g_swap_status, sizeof(g_swap_status), "swap failed, relaunch needed");
        return SWAP_NEEDS_RESTART;
    }

    int engine = A.engine;
    const char *reason = from == HUSK_UNITY_FAILED ? "failed" : from == HUSK_UNITY_ENDED ? "ended"
                       : from == HUSK_UNITY_RUNNING ? "forced" : "starting";
    tl_log_line("husk: session close (%s, %s)", engine_name(engine), reason);

    /* THE FATAL-TIME MEASUREMENT, taken HERE on this normal thread -- NOT in the crash reporter's signal handler (see
     * tl_xmem_report_fault_time). A no-op when no guest fault was recorded and on a host with no arena. */
    tl_xmem_report_fault_time();

    /* From here the close is in flight: a guest quit call arriving now is the app confirming it is done, not a new
     * request -- husk_tl_guest_exit_requested() sees this and the shim lets the app unwind instead of re-entering the
     * close. Cleared on every way out below. */
    tl_guest_exit_set_close_in_flight(true);

    /* Re-arm the producers: no input reaches the old engine from here on. */
    tl_pad_set_sink(NULL);

    int verdict;
    switch (engine) {
    case ENGINE_JAVA:
        /* Unwind the Java app's own threads first, then trim the DVM. A DVM that would not trim leaves app A's
         * dexes/classes in place, so a swap would run app B over app A's bytecode: a refusal, not a partial. A thread
         * that would not stop was quarantined (logged, left parked): a partial. */
        {
            int  app_ok = tl_javaapp_reset();
            bool dvm_ok = tl_dvm_end_app();
            tl_log_line("husk: session close step java: tl_javaapp_reset=%d, tl_dvm_end_app=%d", app_ok, dvm_ok ? 1 : 0);
            /* tl_javaapp_reset: 0 unwound both threads, 1 a thread was asked to stop and would not (quarantined), 2
             * nothing could ask it. A clean 0 is not a leak. */
            verdict = !dvm_ok ? SWAP_NEEDS_RESTART : app_ok != 0 ? SWAP_PARTIAL : SWAP_OK;
        }
        break;
    default:
        /* No reset implementation for this engine yet. Do not pretend success: a second app would load on top of this
         * run's registry -- the exact fault this gate exists to stop. Refuse and report the capability. */
        tl_log_line("husk: session close: %s has no in-process reset yet (capability %d); refusing",
                    engine_name(engine), husk_tl_swap_capability(engine));
        verdict = SWAP_NEEDS_RESTART;
        break;
    }

    if (verdict == SWAP_NEEDS_RESTART) {
        /* Put the run back the way we found it, so the user keeps a working game. The game is still running, so
         * nothing below -- which tears down the layer's audio and message loops -- may run. */
        atomic_store(&A.state, from);
        g_swap_verdict = verdict;
        snprintf(g_swap_status, sizeof(g_swap_status), "swap failed, relaunch needed");
        tl_log_line("husk: session close refused (%s); the gate stays closed, relaunch needed", engine_name(engine));
        tl_guest_exit_set_close_in_flight(false);
        return verdict;
    }

    /*
     * The close proceeds. An app the user closed must not make noise, and the threads the LAYER owns must end rather
     * than be inherited by the next app, for every engine:
     *   1. stop the layer's audio producer threads while the host sink is still live, so an in-flight hook returns
     *      instead of parking and the joins cannot hang;
     *   2. ask every Android Looper thread to unwind (Unity dispatches frames over a HandlerThread looper the Java
     *      reset never touched);
     *   3. silence the sink and deactivate the host session, so a writer we could not stop drops its samples.
     */
    tl_aaudio_stop_all();
    tl_opensl_stop_all();
    tl_dvm_audio_stop();
    int loops_live = tl_loop_quit_all();
    tl_audio_close();

    /* A Looper that will not unwind is a guest thread still running guest code ("quarantined"): give them a short
     * grace, then report what remains honestly -- it downgrades the close to a partial. No false clean close. */
    for (int i = 0; i < 20 && (loops_live = tl_loop_live_count()) > 0; i++) {
        struct timespec ts = { 0, 10 * 1000 * 1000 };       /* 20 * 10 ms = 200 ms */
        nanosleep(&ts, NULL);
    }
    if (loops_live > 0) {
        tl_log_line("husk: session close: %d looper thread(s) still running; guest threads are quarantined", loops_live);
        if (verdict == SWAP_OK) verdict = SWAP_PARTIAL;
    } else {
        tl_log_line("husk: session close: every looper thread unwound");
    }

    /* Threads the guest started through the pthread shim are not ours to stop: no registry to walk, and cancelling a
     * thread that ran guest code runs its TLS destructors. A nonzero count is quarantined, reflected in the verdict. */
    int guest_live = tl_guest_thread_live_count();
    if (guest_live > 0) {
        tl_log_line("husk: session close: %d guest thread(s) still running (not ours to stop; quarantined)", guest_live);
        if (verdict == SWAP_OK) verdict = SWAP_PARTIAL;
    }

    /* Name what the partial close left behind, so `husk_tl_swap_status` states the condition. It used to read
     * "swapped, leaked (detail in the log)", and a caller that quotes the layer's own words -- the Swift swap record --
     * then printed the garbled "leaked (swapped, leaked (detail in the log))". The counts are the honest detail; a
     * partial with neither count nonzero is the Java reset's own quarantine (a thread asked to stop that would not). */
    char leak[96] = "";
    if (verdict == SWAP_PARTIAL) {
        if (guest_live > 0 && loops_live > 0)
            snprintf(leak, sizeof(leak), "%d guest thread(s) and %d looper(s) left parked", guest_live, loops_live);
        else if (guest_live > 0)
            snprintf(leak, sizeof(leak), "%d guest thread(s) left parked", guest_live);
        else if (loops_live > 0)
            snprintf(leak, sizeof(leak), "%d looper thread(s) left parked", loops_live);
        else
            snprintf(leak, sizeof(leak), "a Java thread would not stop");
    }

    /* Forget the old loopers once reported: the next app builds its own main looper and HandlerThreads. */
    tl_loop_reset();

    /* One app's HOST world, forgotten at the boundary (see tl_io_reset_for_new_app): the Ren'Py platform flag, the
     * virtual-file name table, stale descriptor redirects and the jar lookup cache -- all the layer's own bookkeeping,
     * no guest memory or mapping dropped. */
    { void tl_io_reset_for_new_app(void); tl_io_reset_for_new_app(); }

    /* The linker's session generation: stale mappings stay mapped but stop being reachable by name. */
    tl_ld_end_session();

    /* The guest-thread epoch advances at the same boundary: every thread still running from the run that just ended is now
     * "from an earlier session", so the stall probe can tell a parked thread from this session's own (tl_swapstall_tick). */
    tl_guest_thread_new_epoch();

    /* Reset our own bookkeeping ONLY -- never the memory, and field-by-field (a blanket clear would wipe the engine id
     * used for the log). */
    A.engine = ENGINE_UNITY;
    A.apk[0] = 0; A.data[0] = 0; A.package[0] = 0; A.angle[0] = 0; A.ca[0] = 0; A.vulkan[0] = 0;
    A.nextra = 0;
    A.layer = NULL; A.width = 0; A.height = 0;

    /*
     * Open the gate LAST, by CAS: this is the only writer of HUSK_UNITY_IDLE. The teardown above has run, so a
     * completed close must leave the machine at IDLE whatever terminal state the run reached -- leaving it elsewhere
     * keeps the gate shut and greys every other game out ("another game is loaded") until Husk is relaunched. Accept
     * every TERMINAL marker (STOPPING, ENDED, FAILED) and refuse anything that still means a live session (RUNNING,
     * STARTING). A refusal is a real refusal; the log says which state and why.
     */
    int opened = 0, gate_from = -1;
    for (int i = 0; i < 64; i++) {
        int cur = atomic_load(&A.state);
        gate_from = cur;
        if (cur == HUSK_UNITY_IDLE) { opened = 1; break; }        /* already open: nothing left to do (idempotent) */
        if (cur != HUSK_UNITY_STOPPING && cur != HUSK_UNITY_ENDED && cur != HUSK_UNITY_FAILED) break;
        int expect = cur;
        if (atomic_compare_exchange_strong(&A.state, &expect, HUSK_UNITY_IDLE)) { opened = 1; break; }
    }
    g_swap_verdict = verdict;
    if (verdict == SWAP_OK)
        snprintf(g_swap_status, sizeof(g_swap_status), "swapped cleanly");
    else if (verdict == SWAP_PARTIAL)
        /* The verdict word ("leaked") is carried by g_swap_verdict and printed by the caller; the status string names
         * the CONDITION, so a caller that quotes it inline does not print "leaked (swapped, leaked (...))". */
        snprintf(g_swap_status, sizeof(g_swap_status), "%s; not ours to stop", leak);
    else
        snprintf(g_swap_status, sizeof(g_swap_status), "swap failed, relaunch needed");
    if (!opened) {
        const char *why = gate_from == HUSK_UNITY_STARTING ? "a launch is still in flight"
                        : gate_from == HUSK_UNITY_RUNNING  ? "the engine is still running frames"
                        : "the run was not torn down";
        tl_log_line("husk: session close: the gate was not opened (state %s); %s, so this is not a torn-down run -- relaunch needed",
                    state_name(gate_from), why);
    } else if (gate_from == HUSK_UNITY_IDLE) {
        tl_log_line("husk: session close: the gate was already open; ready for the next app (%s)", engine_name(engine));
    } else if (gate_from == HUSK_UNITY_STOPPING) {
        tl_log_line("husk: session close: state %s -> idle; ready for the next app (%s)", state_name(from), engine_name(engine));
    } else {
        tl_log_line("husk: session close: state %s -> idle (the run had settled at %s when the gate opened); ready for the next app (%s)",
                    state_name(from), state_name(gate_from), engine_name(engine));
    }
    tl_guest_exit_set_close_in_flight(false);
    return verdict;
}

/* A swap is done when this passes, not when it was asked for: called once the new app has launched and loaded, it
 * checks the swap against the world, not against what we intended. */
int husk_tl_verify_swap(const char *apk, const char *package)
{
    int ok = 1;

    /* (a) Identity: the APK the caller asked for is the one now loaded, not the previous app's. */
    const char *loaded = husk_native_loaded_apk();
    if (!apk || !loaded || strcmp(loaded, apk) != 0) {
        tl_log_line("husk: swap verified FAILED (identity: loaded '%s', wanted '%s')", loaded ? loaded : "(none)", apk ? apk : "(null)");
        ok = 0;
    } else {
        tl_log_line("husk: swap verified: identity ok ('%s', package '%s')", apk, package ? package : "(unknown)");
    }

    /* (b) Scope: no library from an earlier session is reachable by name (the tl_ld session filter). */
    int stale = tl_ld_stale_count();
    if (stale != 0) {
        tl_log_line("husk: swap verified FAILED (scope: %d libraries from an earlier session are still reachable by name)", stale);
        ok = 0;
    } else {
        tl_log_line("husk: swap verified: scope ok (no library from an earlier session is reachable)");
    }

    /* (c) Liveness: the new engine has produced frames (the existing per-engine tl_*_frames() counters). */
    unsigned long frames = engine_frames();
    if (frames == 0) {
        tl_log_line("husk: swap verified FAILED (liveness: the new engine has produced no frames yet)");
        ok = 0;
    } else {
        tl_log_line("husk: swap verified: liveness ok (%lu frames)", frames);
    }

    /* (d) Accounting: what leaked or was quarantined is a number, not a feeling. */
    tl_log_line("husk: swap accounting: %d stale libraries, %lu frames, loaded '%s'", stale, frames, loaded ? loaded : "(none)");

    if (!ok) {
        g_swap_verdict = SWAP_NEEDS_RESTART;
        snprintf(g_swap_status, sizeof(g_swap_status), "swap failed, relaunch needed");
        tl_log_line("husk: swap verified FAILED ('%s', '%s')", apk ? apk : "(null)", package ? package : "(unknown)");
        return 0;
    }
    /* A pass does not upgrade a partial close: a leak found at close is still a leak. */
    if (g_swap_verdict != SWAP_PARTIAL) {
        g_swap_verdict = SWAP_OK;
        snprintf(g_swap_status, sizeof(g_swap_status), "swapped cleanly");
    }
    tl_log_line("husk: swap verified OK ('%s', '%s')", apk, package ? package : "(unknown)");
    return 1;
}

int husk_unity_state(void)
{
    /* RUNNING -> ENDED by CAS, not a store: the same rule as the heartbeat, so the UI polling this during a session
     * close cannot clobber the close's STOPPING marker or re-close the gate. */
    if (atomic_load(&A.state) == HUSK_UNITY_RUNNING && ((A.engine == ENGINE_COCOS && tl_cocos_ended()) || (A.engine == ENGINE_GODOT && tl_godot_ended()) || (A.engine == ENGINE_GAMEMAKER && tl_gm_ended()) || (A.engine == ENGINE_JAVA && tl_javaapp_ended()))) {
        int expect_running = HUSK_UNITY_RUNNING;
        atomic_compare_exchange_strong(&A.state, &expect_running, HUSK_UNITY_ENDED);
    }
    return atomic_load(&A.state);
}
static unsigned long engine_frames(void)
{
    return A.engine == ENGINE_JAVA ? tl_javaapp_frames() : A.engine == ENGINE_GAMEMAKER ? tl_gm_frames() : A.engine == ENGINE_FLUTTER ? tl_flutter_frames() : A.engine == ENGINE_GODOT ? tl_godot_frames() : A.engine == ENGINE_GTA ? tl_gta_frames() : A.engine == ENGINE_UE4 ? tl_na_frames() : A.engine == ENGINE_SDL ? tl_sdl_frames() : A.engine == ENGINE_GAMEACTIVITY ? tl_ga_frames() : A.engine == ENGINE_COCOS ? tl_cocos_frames() : tl_unity_frames();
}
unsigned long husk_unity_frames(void) { return engine_frames(); }
void husk_unity_perf_snapshot(husk_unity_perf *out)
{
    if (A.engine == ENGINE_GAMEACTIVITY || A.engine == ENGINE_SDL || A.engine == ENGINE_UE4 || A.engine == ENGINE_GTA || A.engine == ENGINE_FLUTTER || A.engine == ENGINE_JAVA) {
        /* The game paces its own frames; the rate is how many it presented since the last look. */
        static unsigned long last; static struct timespec since;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        unsigned long f = engine_frames();
        double dt = since.tv_sec ? (now.tv_sec - since.tv_sec) + (now.tv_nsec - since.tv_nsec) / 1e9 : 0;
        out->fps = dt > 0.05 ? (double)(f - last) / dt : 0;
        out->mean_ms = out->fps > 0 ? 1000.0 / out->fps : 0; out->max_ms = 0;
        last = f; since = now;
        return;
    }
    if (A.engine == ENGINE_COCOS) { tl_cocos_perf p; tl_cocos_perf_snapshot(&p); out->fps = p.fps; out->mean_ms = p.mean_ms; out->max_ms = p.max_ms; return; }
    if (A.engine == ENGINE_GAMEMAKER) { tl_gm_perf p; tl_gm_perf_snapshot(&p); out->fps = p.fps; out->mean_ms = p.mean_ms; out->max_ms = p.max_ms; return; }
    if (A.engine == ENGINE_GODOT) { tl_godot_perf p; tl_godot_perf_snapshot(&p); out->fps = p.fps; out->mean_ms = p.mean_ms; out->max_ms = p.max_ms; return; }
    tl_unity_perf p; tl_unity_perf_snapshot(&p); out->fps = p.fps; out->mean_ms = p.mean_ms; out->max_ms = p.max_ms;
}
/* The touches logged this process: bounded, so one run that ignores touches can be read without a line per finger per
 * frame. */
static atomic_int g_touch_notes;

void husk_unity_touch(int phase, int id, float x, float y)
{
    /* A touch has reached the layer from the host: the first few are logged, with which engine it is for or that no
     * game was running. A game that draws but ignores touches cannot be told from one whose input never left the host
     * unless this line and the host's own "input: host touch" line are both present. */
    int n = atomic_fetch_add(&g_touch_notes, 1);
    int state = atomic_load(&A.state);
    if (state != HUSK_UNITY_RUNNING) {
        if (n < 4) tl_log_line("husk: input: touch (phase %d, id %d, %.0f,%.0f px) dropped: no game is running (state %s)", phase, id, x, y, state_name(state));
        return;
    }
    if (n < 4) tl_log_line("husk: input: touch (phase %d, id %d, %.0f,%.0f px) -> engine %s", phase, id, x, y, engine_name(A.engine));
    if (A.engine == ENGINE_JAVA) tl_javaapp_touch(phase, id, x, y);
    else if (A.engine == ENGINE_GAMEMAKER) tl_gm_touch(phase, id, x, y);
    else if (A.engine == ENGINE_FLUTTER) tl_flutter_touch(phase, id, x, y);
    else if (A.engine == ENGINE_GODOT) tl_godot_touch(phase, id, x, y);
    else if (A.engine == ENGINE_GTA) tl_gta_touch(phase, id, x, y);
    else if (A.engine == ENGINE_UE4) tl_na_touch(phase, id, x, y);
    else if (A.engine == ENGINE_SDL) tl_sdl_touch(phase, id, x, y);
    else if (A.engine == ENGINE_GAMEACTIVITY) tl_ga_touch(phase, id, x, y);
    else if (A.engine == ENGINE_COCOS) tl_cocos_touch(phase, id, x, y); else tl_unity_touch(phase, id, x, y);
}
void husk_unity_set_paused(bool paused)
{
    if (atomic_load(&A.state) != HUSK_UNITY_RUNNING) return;
    if (A.engine == ENGINE_JAVA) { tl_javaapp_set_paused(paused); tl_audio_set_paused(paused); }
    else if (A.engine == ENGINE_GAMEMAKER) { tl_gm_set_paused(paused); tl_audio_set_paused(paused); }
    else if (A.engine == ENGINE_FLUTTER) { tl_flutter_set_paused(paused); tl_audio_set_paused(paused); }
    else if (A.engine == ENGINE_GODOT) { tl_godot_set_paused(paused); tl_audio_set_paused(paused); }
    else if (A.engine == ENGINE_GTA) { tl_gta_set_paused(paused); tl_audio_set_paused(paused); }
    else if (A.engine == ENGINE_UE4) { tl_na_set_paused(paused); tl_audio_set_paused(paused); }
    else if (A.engine == ENGINE_SDL) { tl_sdl_set_paused(paused); tl_audio_set_paused(paused); }
    else if (A.engine == ENGINE_GAMEACTIVITY) { tl_ga_set_paused(paused); tl_audio_set_paused(paused); }
    else if (A.engine == ENGINE_COCOS) { tl_cocos_set_paused(paused); tl_audio_set_paused(paused); }
    else { tl_unity_set_paused(paused); tl_audio_set_paused(paused); }
}

/* ------------------------------------------------------------- package name */

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* One string of an Android binary XML string pool, as UTF-8 into out. */
static bool pool_string(const uint8_t *pool, size_t pool_size, uint32_t index, char *out, size_t n)
{
    uint32_t count = rd32(pool + 8), flags = rd32(pool + 16), strings = rd32(pool + 20);
    if (index >= count || 28 + 4ull * index + 4 > pool_size) return false;
    size_t off = strings + rd32(pool + 28 + 4 * index);
    if (off + 4 > pool_size) return false;
    const uint8_t *p = pool + off;
    if (flags & 0x100) {                                        /* UTF-8: char length, byte length, bytes */
        size_t l = *p++; if (l & 0x80) p++;
        size_t b = *p++; if (b & 0x80) b = ((b & 0x7F) << 8) | *p++;
        if (b >= n) b = n - 1;
        memcpy(out, p, b); out[b] = 0;
    } else {                                                    /* UTF-16 */
        size_t l = rd16(p); p += 2;
        if (l & 0x8000) { l = ((l & 0x7FFF) << 16) | rd16(p); p += 2; }
        size_t k = 0;
        for (size_t i = 0; i < l && k + 1 < n; i++) out[k++] = (char)rd16(p + 2 * i);   /* package names are ASCII */
        out[k] = 0;
    }
    return true;
}

bool husk_unity_package_name(const char *apk, char *out, unsigned long out_len)
{
    tl_zip z;
    char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) return false;
    bool ok = false;
    const tl_zip_entry *e = tl_zip_find(&z, "AndroidManifest.xml");
    const uint8_t *data; size_t len; bool owned = false;
    if (e && tl_zip_data(&z, e, 8u << 20, &data, &len, &owned, err, sizeof(err)) && len > 8 && rd16(data) == 0x0003) {
        const uint8_t *pool = NULL; size_t pool_size = 0;
        for (size_t off = rd16(data + 2); off + 8 <= len; ) {
            uint16_t type = rd16(data + off); uint32_t size = rd32(data + off + 4);
            if (size < 8 || off + size > len) break;
            if (type == 0x0001) { pool = data + off; pool_size = size; }
            else if (type == 0x0102 && pool) {                  /* the first element is <manifest> */
                const uint8_t *el = data + off;
                uint16_t astart = rd16(el + 24), asize = rd16(el + 26), acount = rd16(el + 28);
                for (unsigned i = 0; i < acount; i++) {
                    const uint8_t *at = el + 16 + astart + (size_t)i * asize;
                    char name[40];
                    if (pool_string(pool, pool_size, rd32(at + 4), name, sizeof(name)) && !strcmp(name, "package")
                        && rd32(at + 8) != 0xFFFFFFFFu && pool_string(pool, pool_size, rd32(at + 8), out, out_len)) { ok = true; break; }
                }
                break;
            }
            off += size;
        }
    }
    if (owned) free((void *)data);
    tl_zip_close(&z);
    return ok;
}
