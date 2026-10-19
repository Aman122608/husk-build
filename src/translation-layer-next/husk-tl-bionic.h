/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Android's libc (bionic) and friends, implemented over Darwin.
 *
 * Guest code binds against names, and the two ABIs agree on many of them, so the
 * symbol table has three kinds of entry:
 *
 *   DIRECT  Darwin's function used as it is: same signature, same layouts, same
 *           meaning. strlen, memcpy, sin.
 *   WRAP    ours, because something differs: a structure's layout (stat, dirent),
 *           a constant's value (O_CREAT, errno numbers, signal numbers), the size
 *           of a type the guest embeds in its own structures (pthread_mutex_t is 40
 *           bytes here and 64 there), or a calling convention (variadics).
 *   DATA    an object the guest reads or writes: __sF, _ctype_, environ.
 *
 * Nothing falls back to "whatever Darwin happens to export under that name". That
 * would hand Android code Darwin's C++ ABI and Darwin's idea of every flag.
 */
#ifndef HUSK_TL_BIONIC_H
#define HUSK_TL_BIONIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tl_bionic_entry {
    const char *name;
    void *addr;
} tl_bionic_entry;

#define TL_DIRECT(fn)       { #fn, (void *)(fn) }
#define TL_WRAP(name, impl) { name, (void *)(impl) }
#define TL_DATA(name, ptr)  { name, (void *)(ptr) }
#define TL_END              { NULL, NULL }

/* The tables, one per file. Each ends with TL_END. */
extern const tl_bionic_entry tl_tab_core[];
extern const tl_bionic_entry tl_tab_str[];
extern const tl_bionic_entry tl_tab_io[];
extern const tl_bionic_entry tl_tab_net[];
extern const tl_bionic_entry tl_tab_io2[];
extern const tl_bionic_entry tl_tab_str2[];
extern const tl_bionic_entry tl_tab_pthread[];
extern const tl_bionic_entry tl_tab_ndk[];
extern const tl_bionic_entry tl_tab_egl[];
extern const tl_bionic_entry tl_tab_cxx[];
extern const tl_bionic_entry tl_tab_opensles[];
extern const tl_bionic_entry tl_tab_sys[];
extern const tl_bionic_entry tl_tab_ldbl[];
extern const tl_bionic_entry tl_tab_jvm[];
void *tl_egl_resolve(const char *name);   /* husk-tl-egl.c: GLES by name, through ANGLE */
void *tl_vk_resolve(const char *name);    /* husk-tl-vulkan.m: Vulkan by name, through MoltenVK */
bool tl_vk_available(void);                /* a MoltenVK was configured, so libvulkan.so can be opened */

extern void (*tl_guest_exit_hook)(int status);   /* set by an app host: exit() from guest code calls it instead of exiting */

/*
 * The exit family. A guest call that means "quit my app" -- Runtime.exit/halt, System.exit,
 * Process.killProcess, exit/_exit/_Exit/abort -- must never be forwarded to the HOST process: the
 * guest shares Husk's process, so the host's exit() takes Husk down at the moment the app quits and
 * kills the session close that is already running (the log showed "dvm: Runtime.exit(0)" and then
 * ended, with no "session closed"). husk_tl_guest_exit_requested() records the request and names the
 * call in the log; the caller then unwinds the APP and never the process. A close already in flight
 * is treated as the app confirming it is done. tl_guest_exit_set_close_in_flight() is set by
 * husk_tl_session_close() for the duration of the close; tl_guest_exit_close_in_flight() reads it.
 */
bool husk_tl_guest_exit_requested(const char *call, int status);
bool tl_guest_exit_close_in_flight(void);
void tl_guest_exit_set_close_in_flight(bool in_flight);

/*
 * The one way a guest exit()/_exit()/abort()/failed assert is honoured: it ends the app's world and
 * the CALLING thread, never the host process, and never returns. A native engine's own hook
 * (tl_guest_exit_hook) runs first when set. This is for guest-triggered fatals and exits whose caller
 * cannot return; a Java Runtime.exit/System.exit returns instead (see husk_tl_guest_exit_requested).
 */
void tl_guest_exit_unwind(const char *call, int status);

void *tl_bionic_find(const char *name);
bool  tl_bionic_is_system_lib(const char *soname);

/* ----------------------------------------------------------------- errno */

/*
 * The guest's errno is its own per-thread integer, holding Linux numbers. Darwin's
 * errno holds Darwin numbers, and the two diverge past 34 (EAGAIN is 11 there and
 * 35 here). A wrapper clears Darwin's errno, calls, and publishes whatever the call
 * set -- translated -- only when it set something, so success leaves the guest's
 * errno alone.
 */
int  tl_errno_to_guest(int darwin_errno);
int  tl_errno_from_guest(int guest_errno);
int *tl_guest_errno_ptr(void);
void tl_set_guest_errno(int guest_errno);

#define TL_ERRNO_BEGIN()  (errno = 0)
#define TL_ERRNO_END()    do { if (errno) tl_set_guest_errno(tl_errno_to_guest(errno)); } while (0)

/* Signal numbers: Linux <-> Darwin. 0 and negative on no equivalent. */
int tl_signal_to_darwin(int linux_sig);
int tl_signal_from_darwin(int darwin_sig);

/* The signal-delivery ledger (husk-tl-bionic.c): did the guest's handler get INSTALLED, was the signal SENT, and did the
 * handler RUN on the thread it was sent to. A cross-thread handshake that hangs (Boehm GC's stop-the-world is the one that
 * matters for il2cpp) hangs because one of the three did not happen, and no other line in a run says which. */
void tl_signal_note_install(int guest_sig, int darwin_sig);
void tl_signal_note_send(int guest_sig, pthread_t target, int refused);
/* The OUTCOME of a thread-directed send that was actually ATTEMPTED, recorded AFTER pthread_kill returns:
 * host_errno 0 = the send was accepted (delivered to that thread), ESRCH = the thread was gone, EINVAL, or another
 * errno. Without it, "sent N, RAN 0" reads the same whether N signals landed or all N failed. Counted once per send;
 * only the first two of each outcome pay for a thread name and a log line. */
void tl_signal_note_send_result(int guest_sig, pthread_t target, int host_errno);
/* The same OUTCOME, WITH the per-send identity: the guest pthread_t the guest passed, the host pthread_t the layer
 * actually called pthread_kill with, the target's Darwin tid, and whether the live-thread census maps it back to the
 * same handle. If the guest record and the host handle can ever disagree, that is the bug -- so both are printed and
 * compared on one line. A call site uses THIS or tl_signal_note_send_result, never both (each counts the send once). */
void tl_signal_note_send_result_ex(int guest_sig, unsigned long guest_pthread, unsigned long host_pthread,
                                   unsigned long guest_tid, unsigned long census_pthread, int host_errno);
/* The STAGED delivery chain, written from INSIDE the signal handler and therefore async-signal-safe by construction --
 * one relaxed atomic add each, NO lock, NO name lookup, NO log. host handler ENTERED / guest handler pointer FOUND /
 * guest handler CALLED / guest handler RETURNED. The chain stops at RETURNED: a Boehm acknowledgement is the guest's own
 * step and the layer cannot see it. `enter` also moves the legacy `ran` headline; tl_signal_report prints the stages. */
void tl_signal_note_enter(int guest_sig);
void tl_signal_note_ptr_found(int guest_sig);
void tl_signal_note_called(int guest_sig);
void tl_signal_note_returned(int guest_sig);
void tl_signal_note_rearm(int guest_sig, int darwin_sig, int refused_install, int host_errno, int found_disposition);
/* A thread about to run guest code arrived with a guest-armed signal BLOCKED in its mask (a mask from HOST code,
 * inherited at thread creation -- the only way a blocked bit reaches a guest thread, and the case a send/run ledger
 * cannot see from the send side). Said once per signal; the caller then clears the mask. */
void tl_signal_note_arrived_blocked(int guest_sig, int darwin_sig);
/* The mask a thread runs guest code with, MEASURED at the handoff: tl_guest_mask_default reads the calling thread's OWN
 * mask (the only thread a process can read) and reports it unconditionally, once per thread and capped -- the earlier
 * form named a bit only when the guest had ALREADY armed that signal, which is never true at a thread's entry, so it
 * never fired and the inherited mask stayed invisible. These keep the run's totals for the report: how many threads
 * reached guest code, how many were BORN with a signal blocked (a mask from HOST code, inherited at thread creation),
 * and whether any clear did not take. 0 dirty threads is the measured form of "no guest thread ever inherited a blocked
 * signal" -- what the old send/run ledger could only guess at. One atomic add per thread entry. */
void tl_signal_note_mask_entry(unsigned long darwin_mask);
void tl_signal_note_mask_clear_failed(int pthread_sigmask_rc, unsigned long mask_left);
/* The calling thread's mask for one Darwin number was brought in line with the guest's expectation at the moment the
 * guest armed a real handler for it on a thread the LAYER started (not one a guest created): a layer thread that runs
 * guest code is born with the HOST process's mask by inheritance, and the guest's handler would then be armed on a
 * thread whose mask swallows it. One line per signal. */
void tl_signal_note_mask_unblocked(int guest_sig, int darwin_sig, int rc);
void tl_signal_report(const char *tag);
/* READ BACK the disposition of a Darwin signal with Darwin's own sigaction(d, NULL, &cur) -- the fact the ledger cannot
 * see. Fills `out` with the address, its nearest symbol and its image (base), classified against THIS image's
 * host_signal_entry: SIG_DFL / SIG_IGN (Darwin DISCARDS a send of it), OURS, ANOTHER HANDLER in this image, or ANOTHER
 * IMAGE'S handler (a different Mach-O installed it, so this image's ledger can never count its runs). Does the work only
 * when called; callers gate it to once per signal. */
void tl_signal_describe_disposition(int darwin_sig, char *out, size_t cap);

/* The one-shot signal self-test (husk-tl-bionic-io.c): installs its OWN probe handler on a Darwin signal the layer owns,
 * sends it to a thread the layer owns, and reports PASS/FAIL with the errno -- answering, by measurement on the device,
 * whether this process can deliver a thread-directed signal at all, and whether the Darwin number bionic SIGPWR (30) is
 * mapped to (SIGINFO, 29) actually runs a handler. Runs ONCE, at start-up, before the guest arms anything. If SIGINFO
 * cannot deliver but the control (SIGEMT, 7) can, it moves bionic SIGPWR onto a number a send really delivers and says
 * so. It also prints the address of the ledger THIS image reads, beside the probe's own run count, so a layer loaded
 * twice (a handler in one image, the report in another) is visible in one log. */
void tl_signal_selftest(void);
/* The probe handler's first action: records, on the same state tl_signal_report reads, that a delivery RAN. Returns how
 * many probe deliveries ran in THIS image; tl_signal_ledger_image() is this image's ledger address. */
void tl_signal_probe_ran(void);
unsigned long tl_signal_probe_count(void);
void *tl_signal_ledger_image(void);
/* The RUNNING process's image identity for the signal subsystem (the self-test prints all of these): the address of THIS
 * image's ledger, guest-handler table, SIGPWR override, and the send call site. If the layer is loaded twice these are
 * at different addresses, and a handler installed by one copy is invisible to the other -- signal dispositions are
 * process-wide while g_sig/g_guest_sig/g_pwr_darwin are image-local. */
void *tl_signal_guest_table_image(void);
void *tl_signal_pwr_addr(void);
void *tl_signal_send_site(void);
/* bionic SIGPWR's Darwin number for this run: 0 = the table's default (29/SIGINFO), else what the self-test measured. */
void tl_signal_set_pwr_darwin(int darwin_sig);
int  tl_signal_pwr_darwin(void);

/* Make sure the guest's own dispatcher is really installed on the Darwin signal a send is about to use, and install it
 * again if something cleared it. Darwin DISCARDS a signal whose disposition is SIG_DFL -- no handler, nothing pending --
 * so a cleared disposition is indistinguishable in a ledger from a handler that never ran. Returns 1 if it had to
 * re-install, 0 if the dispatcher was already armed (or the guest has no handler for that signal), -1 if the host
 * refused the install. Cheap: one sigaction() query, and only on the send path (one per suspended thread per collection,
 * never per frame, never per call). */
int tl_signal_arm_guest(int darwin_sig, int guest_sig);

/* The guest's signal mask applied to the calling thread (bionic's `how`: SIG_BLOCK 0 / SIG_UNBLOCK 1 / SIG_SETMASK 2).
 * Shared by pthread_sigmask, sigprocmask and the raw rt_sigprocmask syscall. */
int tl_bionic_sigmask(int how, const uint64_t *set, uint64_t *old);

/* Run the CALLING thread's guest code with the mask the guest expects: put the thread's signal mask back to bionic's
 * empty thread mask, so a park of HOST code's mask (inherited at thread creation from one of the layer's own threads,
 * which the guest never set) cannot ride into a guest thread -- a blocked thread-directed signal is left PENDING by
 * Darwin, its handler never runs, and a stop-the-world handshake waits forever. Called at the moment layer code hands off
 * to guest code (each guest thread's entry, and each layer thread that runs the guest). One pthread_sigmask per thread
 * ENTRY, never per call or per frame; it also records in the ledger, once per signal, any blocked bit it removed. */
void tl_guest_mask_default(void);
/* Whether the CALLING thread is one the layer started for a guest (a guest thread it honours the guest's mask
 * inheritance for, g_my_slot >= 0) rather than a thread the LAYER itself started. A layer thread that runs guest code
 * carries HOST code's mask -- the guest never set it -- and is the one case a handler can be armed on a thread whose
 * mask swallows its signal. */
int tl_thread_is_guest(void);

/* The layer's pthread handle for a live guest thread named by its Darwin thread id (what a raw tkill/tgkill carries), or
 * NULL. Reads the layer's own live-thread census; never invents a handle, never suspends or joins anyone. */
pthread_t tl_guest_pthread_for_tid(unsigned long tid);

/* The ALooper delivery ledger (husk-tl-bionic-ndk.c): one line per guest ALooper with how often it was polled, by which
 * thread, and whether the callbacks registered on it were ever delivered -- the question "a guest registered an fd with a
 * callback and woke the looper: did the callback ever run" answered from one log, read by the stall probe and the close. */
void tl_looper_report(const char *tag);

/* Android system properties, for __system_property_get and Java's Build. */
const char *tl_sysprop(const char *name);

/* The log. */
void tl_log_line(const char *fmt, ...);

/* A one-line note, once per distinct text, for things that are stubbed. */
void tl_note_once(const char *what);

/* ------------------------------------------------------------- hot-path counters
 *
 * One relaxed atomic tally per intercepted call that runs per frame, so "which shim is hot" is a NUMBER and not a guess:
 * the log does not print a line per fstat/clock_gettime/read, and a per-call log line would cost more than the call. The
 * bump is a lock-free, allocation-free relaxed atomic add at the TOP of each wrapping entry point; the totals are read by
 * the periodic perf summary (husk-tl-unity-app.c) once per N seconds and once at session close -- never per event. */
enum {
    TL_HP_CLOCK_GETTIME = 0, TL_HP_GETTIMEOFDAY, TL_HP_READ, TL_HP_PREAD64,
    TL_HP_FSTAT, TL_HP_FSTATAT, TL_HP_OPEN, TL_HP_MMAP,
    TL_HP_MUTEX_LOCK, TL_HP_MUTEX_UNLOCK, TL_HP_COND_WAIT,
    TL_HP_N
};
extern volatile unsigned long tl_hp_count[TL_HP_N];
const char *tl_hp_name(int which);
/* THE SAMPLE, not a per-call trace: one mask test on the counter this bump already does, and every 4096th call on a shim
 * records WHERE that shim was called FROM (the guest's return address). A guest thread spinning on gettimeofday /
 * pthread_mutex_lock without presenting a frame is an engine's own retry loop, and the only thing the log can otherwise
 * say about it is a bare pc; these call sites name the loop by address, with a count, resolvable against the APK's .so.
 * Bounded (16 sites per shim, the rest counted, never stored) and bounded in cost: no lock and no store on a call that
 * is not sampled. */
void tl_hp_note_callsite(int which, void *ret);
void tl_hp_callsites_each(void (*cb)(const char *shim, const void *ret, unsigned long n, void *user), void *user);
static inline __attribute__((always_inline)) void tl_hp_bump(int which)
{
    if (which >= 0 && which < TL_HP_N) {
        unsigned long n = __atomic_fetch_add(&tl_hp_count[which], 1ul, __ATOMIC_RELAXED) + 1;
        if ((n & 0xFFFul) == 0) tl_hp_note_callsite(which, __builtin_return_address(0));
    }
}

/* How many guest threads the pthread shim has started and that have not returned: what a session close reports it could
 * not stop. A close never ends or joins one -- cancelling a thread that ran guest code runs its TLS destructors. */
int tl_guest_thread_live_count(void);

/*
 * The app's OWN reads leading up to a freeze, the PAIRED instrument. The stall probe's verdict, when no parked thread
 * holds a layer lock, points here: at the app's onResume/launch and its asset/asset-file reads. An ALWAYS-ARMED,
 * fixed-capacity ring records the most recent APK-asset reads (entry name, served/failed, and the reading thread) without
 * one log line per read -- because the old swap-gated trace turned ON only after a stall was detected, when the app had
 * already stopped reading, so it could only ever record the silence it was meant to explain. The ring is dumped, oldest
 * first, when a stall is detected and at session close. The hot path is one relaxed atomic add and a bounded copy; no
 * allocation, no lock, no log line while the app runs. TL_ASSET_TRACE=1 still logs each read as it happens (a desktop
 * aid). `ok` is 1 for a read served, 0 for one that failed, -1 for an attempt/open. */
void tl_asset_read_note(const char *what, int ok, int err);
/* Dump the asset ring, oldest first, labelled `tag`; a no-op if nothing was recorded. Called at a detected stall and at
 * the session boundary. */
void tl_asset_ring_dump(const char *tag);
/* Forget the ring at a session boundary, after the close dump: the next app starts with its own. */
void tl_asset_ring_reset(void);

/* The paths a failed open() looked in, most recent first, always armed and bounded (once per distinct path). The guest
 * reports a native ENOENT as a bare IOException with no path ("No such file or directory"), so when libcore's natives
 * fail to load the loader prints this instead of only the errno. */
void tl_open_fail_dump(const char *why);

/*
 * WHO those live threads are, so a stall can name itself. The counter can say a close left 35 threads running and cannot
 * say which; a swapped-in app that then hangs on its first frame cannot be explained from a number. A small lock-free
 * table records, per live thread, its Darwin identity, the entry it was created on and the session EPOCH it started in,
 * so a reader can tell a thread from an EARLIER session (parked from the app that was swapped out) from this session's
 * own. Reading it never blocks and never stops a thread -- it is for reporting, exactly as the counter is.
 */
typedef struct {
    unsigned long tid, entry;
    int epoch;
    /* Why the thread is inside a guest lock wait, or 0 (TL_GW_NONE) when it is not. A thread BLOCKED INSIDE A GUEST LOCK
     * is a different state from an idle one -- a worker pool waiting for work looks identical to a crowd on a lock from
     * outside -- so the shim records it (it is the shim's own b_mutex_lock/b_cond_wait/b_sem_wait that is running) and
     * the stall probe reports it. wait_obj/wait_site are the object waited on and where the wait was entered. */
    unsigned long wait_kind, wait_obj, wait_site;
    /* The LAST shim lock call this thread entered and when it did (monotonic ns), so a stall can say what a thread that is
     * not blocked RIGHT NOW was last doing inside a shim and how long ago. `wait_kind` above stays the CURRENT state
     * (cleared when the shim returns); these persist across the return. last_kind is 0 when the thread never entered one. */
    unsigned long last_kind, last_site, last_ns;
} tl_guest_thread_info;
/* wait_kind values (TL_GW_*), shared with husk-tl-bionic-pthread.c's census writer. */
enum { TL_GW_NONE = 0, TL_GW_MUTEX = 1, TL_GW_COND = 2, TL_GW_SEM = 3, TL_GW_RWLOCK = 4 };
int  tl_guest_thread_census(tl_guest_thread_info *out, int max);   /* live count (may exceed max); fills up to max */
int  tl_guest_thread_epoch(void);                                  /* the current session epoch */
void tl_guest_thread_new_epoch(void);                             /* a session boundary: later threads are this session's */

/*
 * Every guest mutex currently HELD that has a live waiter, with both sites. Not a per-5s log line: a scan of the live-thread
 * census at a detected stall, so a stalled app's own lock deadlock is named from the owner bytes the lock shim records on
 * acquisition (no per-wait line -- that would require polling the wait). Deduped by the guest mutex object (one entry per
 * held mutex, the first live waiter reported for it). holder_tid is
 * 0 when the holder was never recorded (a lock taken before the holder bytes were written). */
typedef struct {
    unsigned long mutex, waiter_tid, waiter_entry, waiter_site, holder_tid, holder_site;
} tl_guest_mutex_wait;
int tl_guest_mutex_waits(tl_guest_mutex_wait *out, int max);   /* live count (may exceed max); fills up to max */

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_BIONIC_H */
