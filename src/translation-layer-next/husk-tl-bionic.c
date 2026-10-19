/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The core of the bionic shim: how names are looked up, how errno and signal
 * numbers cross over, system properties, the C runtime hooks, and the dynamic
 * linking calls.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-bionic.h"

#include <crt_externs.h>
#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "husk-tl-ld.h"
#include "husk-tl-va.h"

/* ------------------------------------------------------------------ errno */

static const struct { int darwin, guest; } k_errno[] = {
    { EPERM, 1 }, { ENOENT, 2 }, { ESRCH, 3 }, { EINTR, 4 }, { EIO, 5 }, { ENXIO, 6 }, { E2BIG, 7 },
    { ENOEXEC, 8 }, { EBADF, 9 }, { ECHILD, 10 }, { EAGAIN, 11 }, { ENOMEM, 12 }, { EACCES, 13 },
    { EFAULT, 14 }, { ENOTBLK, 15 }, { EBUSY, 16 }, { EEXIST, 17 }, { EXDEV, 18 }, { ENODEV, 19 },
    { ENOTDIR, 20 }, { EISDIR, 21 }, { EINVAL, 22 }, { ENFILE, 23 }, { EMFILE, 24 }, { ENOTTY, 25 },
    { ETXTBSY, 26 }, { EFBIG, 27 }, { ENOSPC, 28 }, { ESPIPE, 29 }, { EROFS, 30 }, { EMLINK, 31 },
    { EPIPE, 32 }, { EDOM, 33 }, { ERANGE, 34 }, { EDEADLK, 35 }, { ENAMETOOLONG, 36 }, { ENOLCK, 37 },
    { ENOSYS, 38 }, { ENOTEMPTY, 39 }, { ELOOP, 40 }, { ENOMSG, 42 }, { EIDRM, 43 }, { EOVERFLOW, 75 },
    { EILSEQ, 84 }, { ENOTSOCK, 88 }, { EDESTADDRREQ, 89 }, { EMSGSIZE, 90 }, { EPROTOTYPE, 91 },
    { ENOPROTOOPT, 92 }, { EPROTONOSUPPORT, 93 }, { ESOCKTNOSUPPORT, 94 }, { ENOTSUP, 95 },
    { EPFNOSUPPORT, 96 }, { EAFNOSUPPORT, 97 }, { EADDRINUSE, 98 }, { EADDRNOTAVAIL, 99 },
    { ENETDOWN, 100 }, { ENETUNREACH, 101 }, { ENETRESET, 102 }, { ECONNABORTED, 103 },
    { ECONNRESET, 104 }, { ENOBUFS, 105 }, { EISCONN, 106 }, { ENOTCONN, 107 }, { ESHUTDOWN, 108 },
    { ETOOMANYREFS, 109 }, { ETIMEDOUT, 110 }, { ECONNREFUSED, 111 }, { EHOSTDOWN, 112 },
    { EHOSTUNREACH, 113 }, { EALREADY, 114 }, { EINPROGRESS, 115 }, { ESTALE, 116 }, { EDQUOT, 122 },
    { ECANCELED, 125 }, { EOPNOTSUPP, 95 },
};

int tl_errno_to_guest(int e)
{
    for (size_t i = 0; i < sizeof(k_errno) / sizeof(k_errno[0]); i++) if (k_errno[i].darwin == e) return k_errno[i].guest;
    return e;
}

int tl_errno_from_guest(int e)
{
    for (size_t i = 0; i < sizeof(k_errno) / sizeof(k_errno[0]); i++) if (k_errno[i].guest == e) return k_errno[i].darwin;
    return e;
}

static __thread int t_guest_errno;
int *tl_guest_errno_ptr(void) { return &t_guest_errno; }
void tl_set_guest_errno(int e) { t_guest_errno = e; }
static int *bionic___errno(void) { return &t_guest_errno; }

/* ---------------------------------------------------------------- signals */

/* Linux arm64 signal numbers by index; the value is Darwin's. */
static const int k_sig_to_darwin[65] = {
    [0] = 0, [1] = SIGHUP, [2] = SIGINT, [3] = SIGQUIT, [4] = SIGILL, [5] = SIGTRAP, [6] = SIGABRT,
    [7] = SIGBUS, [8] = SIGFPE, [9] = SIGKILL, [10] = SIGUSR1, [11] = SIGSEGV, [12] = SIGUSR2,
    [13] = SIGPIPE, [14] = SIGALRM, [15] = SIGTERM, [16] = -1, [17] = SIGCHLD, [18] = SIGCONT,
    [19] = SIGSTOP, [20] = SIGTSTP, [21] = SIGTTIN, [22] = SIGTTOU, [23] = SIGURG, [24] = SIGXCPU,
    [25] = SIGXFSZ, [26] = SIGVTALRM, [27] = SIGPROF, [28] = SIGWINCH, [29] = SIGIO,
    [30] = SIGINFO /* SIGPWR: Boehm's stop-the-world signal */, [31] = SIGSYS,
};

/* SIGPWR's Darwin number, resolved to what a thread-directed send ACTUALLY delivers (see tl_signal_selftest). 0 means
 * "use the table": the mapping below is the default and is only overridden when the one-shot self-test has measured
 * that Darwin SIGINFO will not run a handler for a thread-directed send while a concrete alternative does. The
 * alternative is Darwin SIGEMT (7), the one Darwin number in 1..31 no bionic signal claims, so the override steals
 * nothing: it frees 29 and takes an otherwise-unused number. Read on the send path (one per suspended thread per
 * collection, never per frame), so it stays a single predictable branch. */
static int g_pwr_darwin;

int tl_signal_to_darwin(int s)
{
    if (s < 0 || s > 64) return -1;
    if (s >= 32) return -1;
    if (s == 0) return 0;
    if (s == 30 && g_pwr_darwin) return g_pwr_darwin;   /* bionic SIGPWR, resolved by measurement; see tl_signal_selftest */
    return k_sig_to_darwin[s] ? k_sig_to_darwin[s] : -1;
}

int tl_signal_from_darwin(int d)
{
    if (d <= 0) return 0;
    if (g_pwr_darwin && d == g_pwr_darwin) return 30;   /* the overridden SIGPWR number, before the table is consulted */
    for (int i = 1; i < 32; i++) {
        if (i == 30 && g_pwr_darwin) continue;          /* bionic 30 moved off SIGINFO in this run: Darwin 29 is nobody's */
        if (k_sig_to_darwin[i] == d && k_sig_to_darwin[i] > 0) return i;
    }
    /* A Darwin signal no bionic signal maps to (SIGEMT, and the numbers the table leaves -1): it is NOT a guest signal, so
     * it must not be reported as one. Returning `d` here made Darwin 7 look like bionic 7 (SIGBUS) and Darwin 16 like
     * bionic 16 -- a delivered signal the layer does not own would then have run the WRONG guest handler. */
    return 0;
}

/* Set only by tl_signal_selftest, once, on the number a thread-directed send was MEASURED to deliver. */
void tl_signal_set_pwr_darwin(int d) { g_pwr_darwin = d; }
int  tl_signal_pwr_darwin(void) { return g_pwr_darwin; }
/* The address of THIS image's SIGPWR-override variable, for the self-test's image-identity proof. */
void *tl_signal_pwr_addr(void) { return (void *)&g_pwr_darwin; }

/* ------------------------------------------------------------------ signal delivery ledger
 *
 * What a signal ledger has to answer for a guest that hangs: a signal the guest relies on for a cross-thread
 * handshake was INstalled, then SENT to a thread, and the guest's handler RAN there -- or one of the three did not
 * happen. Boehm GC (il2cpp's collector) is the case that matters most: GC_stop_world sends SIGPWR (bionic 30 ->
 * Darwin SIGINFO) to EVERY registered thread and waits for each one's handler to post an ack semaphore, so a single
 * thread whose handler never runs hangs the collector forever -- and the collector is the engine's main thread, so
 * the app never reaches its first frame. This run's own log cannot tell that apart from any other stall, so the
 * three events are counted per guest signal here and dumped by the stall probe / the close.
 *
 * Cost: one relaxed add per event on the cold paths only -- sigaction is called a handful of times, a handler runs
 * when a signal is delivered, and a send is one per suspended thread per collection. Nothing here is on the
 * per-frame path. The first two of each kind per signal are written to the log; the rest only move a counter. */
/* Cost: the hot event -- a handler running, or a signal being sent -- is ONE relaxed atomic add, and only the first two
 * of each kind per signal pay for a thread name and a log line. sigaction is a handful of calls per process. Nothing
 * here is on the per-frame path, and nothing takes a lock unless a line is about to be written. */
static struct {
    atomic_ulong installed, sent, ran, refused, rearmed, failed, arrived_blocked;
    /* The OUTCOME of each send that was actually ATTEMPTED, keyed by the errno pthread_kill returned: a send counted
     * before the attempt made "sent N, RAN 0" read the same whether N signals landed or all N failed. */
    atomic_ulong delivered, esrch, einval, other;
    atomic_ulong other_errno;                 /* the last non-ESRCH/EINVAL errno seen, for the report */
    /* The staged delivery chain, so the log CANNOT conflate "a signal reached the host dispatcher" with "the guest's
     * handler ran": host handler ENTERED / guest handler pointer FOUND / guest handler CALLED / guest handler RETURNED.
     * A Boehm acknowledgement cannot be counted from the layer, so the chain stops at RETURNED and claims nothing past
     * it. All four are ONE relaxed atomic add each, written from INSIDE the signal handler, so they are async-signal-safe
     * by construction (no lock, no name lookup, no log). */
    atomic_ulong entered, ptr_found, called, returned;
    atomic_ulong first_entered_by;            /* the pthread_t that first ENTERED the handler, for the report's name */
    int n_install, n_send, n_run;
    char inst_by[32], sent_from[32], sent_to[32];
} g_sig[65];
static pthread_mutex_t g_sig_mu = PTHREAD_MUTEX_INITIALIZER;

/* --- the one-shot signal self-test's shared state (see tl_signal_selftest, husk-tl-bionic-io.c) ---
 * The probe handler's FIRST action is one relaxed atomic add HERE, on the same state tl_signal_report reads. If the
 * handler were installed by one copy of the layer and the report drawn from another, the report would read 0 while the
 * self-test's own line -- printed from the same image that installed the handler -- says it ran, which is exactly how
 * the two-image hazard would look. Both lines also print the address of this image's ledger, so the two are compared
 * in one log; nothing here is on a hot path. */
static atomic_ulong g_sig_probe_runs;
void  tl_signal_probe_ran(void) { atomic_fetch_add(&g_sig_probe_runs, 1); }
unsigned long tl_signal_probe_count(void) { return (unsigned long)atomic_load(&g_sig_probe_runs); }
void *tl_signal_ledger_image(void) { return (void *)&g_sig[0]; }

static void sig_thread_name(char *out, size_t cap)
{
    out[0] = '?'; out[1] = 0;
    pthread_getname_np(pthread_self(), out, cap);
    if (!out[0]) snprintf(out, cap, "?");
}

void tl_signal_note_install(int gsig, int dsig)
{
    if (gsig < 1 || gsig > 64) return;
    unsigned long n = atomic_fetch_add(&g_sig[gsig].installed, 1);
    if (n >= 2) return;
    char me[32]; sig_thread_name(me, sizeof(me));
    pthread_mutex_lock(&g_sig_mu);
    if (!g_sig[gsig].inst_by[0]) snprintf(g_sig[gsig].inst_by, sizeof(g_sig[gsig].inst_by), "%s", me);
    g_sig[gsig].n_install++;
    pthread_mutex_unlock(&g_sig_mu);
    tl_log_line("signal[N]: guest installed a handler for signal %d (Darwin %d) on '%s' -- deliveries of it are counted, and the stall/close dump says whether the handler ever RAN (ledger image %p)", gsig, dsig, me, (void *)&g_sig[0]);
}

void tl_signal_note_send(int gsig, pthread_t target, int refused)
{
    if (gsig < 1 || gsig > 64) return;
    unsigned long n = atomic_fetch_add(refused ? &g_sig[gsig].refused : &g_sig[gsig].sent, 1);
    if (n >= 2) return;                       /* the name and the line are for the first two only: the counter is the ledger */
    char me[32], tn[32] = "?";
    sig_thread_name(me, sizeof(me));
    pthread_getname_np(target, tn, sizeof(tn));
    if (!tn[0]) snprintf(tn, sizeof(tn), "?");
    pthread_mutex_lock(&g_sig_mu);
    if (!g_sig[gsig].sent_from[0]) snprintf(g_sig[gsig].sent_from, sizeof(g_sig[gsig].sent_from), "%s", me);
    if (!g_sig[gsig].sent_to[0]) snprintf(g_sig[gsig].sent_to, sizeof(g_sig[gsig].sent_to), "%s", tn);
    g_sig[gsig].n_send++;
    pthread_mutex_unlock(&g_sig_mu);
    tl_log_line("signal[N]: '%s' sent signal %d to '%s'%s", me, gsig, tn, refused ? " -- REFUSED by the layer (no Darwin equivalent for this bionic signal number)" : "");
}

/* The OUTCOME of a thread-directed send that was actually attempted. `host_errno` is what pthread_kill/raise returned
 * (0 = it accepted the signal for that thread). This is recorded AFTER the attempt, so a run's ledger can tell apart
 * "the sends landed and no handler ran" (delivered > 0, RAN 0) from "the sends never landed" (delivered 0, esrch/einval
 * > 0) -- which the old count-before-the-attempt could not, and which the 'BLOCKED' verdict silently assumed away. */
void tl_signal_note_send_result(int gsig, pthread_t target, int host_errno)
{
    if (gsig < 1 || gsig > 64) return;
    atomic_fetch_add(&g_sig[gsig].sent, 1);   /* this send was ATTEMPTED: the one place a send is now counted */
    atomic_ulong *slot = host_errno == 0 ? &g_sig[gsig].delivered
                       : host_errno == ESRCH  ? &g_sig[gsig].esrch
                       : host_errno == EINVAL ? &g_sig[gsig].einval
                       : &g_sig[gsig].other;
    unsigned long n = atomic_fetch_add(slot, 1);
    if (host_errno != 0 && host_errno != ESRCH && host_errno != EINVAL) atomic_store(&g_sig[gsig].other_errno, (unsigned long)host_errno);
    if (n >= 2) return;                       /* the name and the line are for the first two of each outcome */
    char me[32], tn[32] = "?";
    sig_thread_name(me, sizeof(me));
    pthread_getname_np(target, tn, sizeof(tn));
    if (!tn[0]) snprintf(tn, sizeof(tn), "?");
    if (host_errno == 0)
        tl_log_line("signal[N]: '%s' sent signal %d to '%s' -- the send was ACCEPTED (pthread_kill returned 0): the "
                    "signal is on its way to that thread, so a missing handler run is the thread, not the send",
                    me, gsig, tn);
    else
        tl_log_line("signal[N]: '%s' sent signal %d to '%s' -- the send FAILED, pthread_kill returned %s (%d): the "
                    "signal never reached the thread, whatever its disposition",
                    me, gsig, tn, strerror(host_errno), host_errno);
}

/* The OUTCOME of a send, WITH the per-send identity the review asked for (its candidates 1 and 3): the sender's name,
 * the target's name, the guest pthread_t the guest passed, the host pthread_t the layer actually called pthread_kill
 * with, the target's Darwin tid, and whether the layer's live-thread census maps that tid back to the SAME handle. If
 * the guest record and the host handle can ever disagree, THAT is the bug -- so both are printed and compared on the
 * line itself. Used by b_kill (the guest's pthread_kill); counts and logs exactly as tl_signal_note_send_result does,
 * so a call site uses one or the other, never both. */
void tl_signal_note_send_result_ex(int gsig, unsigned long guest_pt, unsigned long host_pt,
                                   unsigned long guest_tid, unsigned long census_pt, int host_errno)
{
    if (gsig < 1 || gsig > 64) return;
    atomic_fetch_add(&g_sig[gsig].sent, 1);
    atomic_ulong *slot = host_errno == 0 ? &g_sig[gsig].delivered
                       : host_errno == ESRCH  ? &g_sig[gsig].esrch
                       : host_errno == EINVAL ? &g_sig[gsig].einval
                       : &g_sig[gsig].other;
    unsigned long n = atomic_fetch_add(slot, 1);
    if (host_errno != 0 && host_errno != ESRCH && host_errno != EINVAL) atomic_store(&g_sig[gsig].other_errno, (unsigned long)host_errno);
    if (n >= 2) return;                       /* the identity line is for the first two sends only: the counters are the ledger */
    char me[32], tn[32] = "?";
    sig_thread_name(me, sizeof(me));
    pthread_getname_np((pthread_t)(uintptr_t)host_pt, tn, sizeof(tn));
    if (!tn[0]) snprintf(tn, sizeof(tn), "?");
    tl_log_line("signal[N]: '%s' sent signal %d to '%s': the guest passed pthread_t %#lx; the layer called pthread_kill "
                "on pthread_t %#lx (%s); the target's guest tid is %lu; the live-thread census %s; pthread_kill returned %s (%d)",
                me, gsig, tn, guest_pt, host_pt,
                guest_pt == host_pt ? "guest and host handle are the SAME value -- the layer does not translate it"
                                    : "guest and host handle DIFFER, which would be the bug",
                guest_tid,
                census_pt && census_pt == host_pt ? "maps that tid back to the SAME handle"
               : census_pt ? "maps that tid to a DIFFERENT handle" : "cannot place that tid",
                host_errno ? strerror(host_errno) : "0 = accepted", host_errno);
}

void tl_signal_note_enter(int gsig)
{
    if (gsig < 1 || gsig > 64) return;
    atomic_fetch_add(&g_sig[gsig].entered, 1);
    atomic_fetch_add(&g_sig[gsig].ran, 1);    /* the legacy headline: "the guest's dispatcher was reached" */
    unsigned long z = 0;                      /* record the FIRST entering thread, lock-free, for the report's name */
    atomic_compare_exchange_strong(&g_sig[gsig].first_entered_by, &z, (unsigned long)(uintptr_t)pthread_self());
}
void tl_signal_note_ptr_found(int gsig) { if (gsig >= 1 && gsig <= 64) atomic_fetch_add(&g_sig[gsig].ptr_found, 1); }
void tl_signal_note_called(int gsig)    { if (gsig >= 1 && gsig <= 64) atomic_fetch_add(&g_sig[gsig].called, 1); }
void tl_signal_note_returned(int gsig)  { if (gsig >= 1 && gsig <= 64) atomic_fetch_add(&g_sig[gsig].returned, 1); }

/* A send whose handler was NOT armed on the host when the signal was about to go out.
 *
 * This is the one state a signal ledger can otherwise never see: Darwin DISCARDS a signal whose disposition is SIG_DFL
 * (or SIG_IGN) -- it does not run a handler and it does not leave the signal pending, so the ledger reads a send with no
 * run and no refusal, which is exactly the shape that says a thread was signalled and never answered. The layer now
 * re-installs the guest's own dispatcher before sending (see tl_signal_arm_guest) and records here that it had to, so a
 * run where a handler was found cleared says so in the layer's own words instead of leaving the send unexplained. */
void tl_signal_note_rearm(int gsig, int dsig, int kind, int host_errno, int found)
{
    if (gsig < 1 || gsig > 64) return;
    unsigned long n = atomic_fetch_add(kind ? &g_sig[gsig].failed : &g_sig[gsig].rearmed, 1);
    if (n >= 2) return;                       /* first two only: the counter is the ledger */
    if (kind)
        tl_log_line("signal[N]: the layer could NOT install the guest's handler for signal %d on Darwin %d -- %s (%d); "
                    "a send of it cannot run a handler here", gsig, dsig, strerror(host_errno), host_errno);
    else
        tl_log_line("signal[N]: the guest's handler for signal %d (Darwin %d) was NOT in place on the host before this send "
                    "(found %s) -- the layer re-installed its dispatcher and sent anyway. A disposition of SIG_DFL makes "
                    "Darwin DISCARD the signal: no handler runs and nothing is left pending, so the send would otherwise "
                    "read as a signal whose handler never ran. Host code changes dispositions through Darwin's sigaction "
                    "WITHOUT passing this layer, so the ledger cannot see it happen -- which is why the send path verifies "
                    "instead of assuming", gsig, dsig,
                    found == 0 ? "SIG_DFL" : found == 1 ? "SIG_IGN" : "another handler");
}

/* A thread that was about to run GUEST code arrived with a signal BLOCKED in its mask that the guest itself has armed a
 * handler for. The guest never blocks such a signal (il2cpp's one mask call site UNBLOCKS the collector's signals), so a
 * blocked bit here rode in from HOST code -- inherited at thread creation from a layer/host thread whose mask the guest
 * never set, which is the one path a mask can take onto a guest thread at all. It is fatal to a stop-the-world
 * handshake: a thread-directed signal to a blocked thread is left PENDING by Darwin, its handler does not run, and
 * nothing on that thread ever unblocks it, so the collector waits forever. The layer now clears the mask before guest
 * code runs (tl_guest_mask_default); this line is said once per signal (the counter is the ledger) and is the ONLY trace
 * of the mask, because the send side cannot see a blocked target. */
void tl_signal_note_arrived_blocked(int gsig, int dsig)
{
    if (gsig < 1 || gsig > 64) return;
    unsigned long n = atomic_fetch_add(&g_sig[gsig].arrived_blocked, 1);
    if (n >= 2) return;
    char me[32]; sig_thread_name(me, sizeof(me));
    tl_log_line("signal[N]: a thread about to run guest code ('%s') ARRIVED with Darwin %d (bionic %d) BLOCKED in its mask "
                "-- the guest never blocks it, so it rode in from HOST code (inherited at thread creation); a signal sent to a "
                "thread that has it blocked stays PENDING and its handler never runs, which is what a stop-the-world handshake "
                "hangs on. The layer CLEARED the mask before guest code ran", me, dsig, gsig);
}

/* --- the mask a thread runs guest code with, as a MEASUREMENT (see tl_signal_note_mask_entry in the header) ---------
 * tl_guest_mask_default() reads the calling thread's OWN mask at every handoff to guest code and reports it, once per
 * thread and capped. These counters are the run's totals, printed by the report: a run with dirty_threads == 0 is the
 * measured form of "no guest thread ever ran guest code with a blocked signal", which the send/run ledger could only
 * guess at ("SENDS > RUNS with every handler armed ... at least one thread has the Darwin signal BLOCKED" was a
 * hypothesis, because no Darwin call reads another thread's mask; the thread reads its own here). */
static atomic_ulong g_mask_threads_seen, g_mask_threads_dirty, g_mask_clear_failed, g_mask_unblocked;
void tl_signal_note_mask_entry(unsigned long darwin_mask)
{
    atomic_fetch_add(&g_mask_threads_seen, 1);
    if (darwin_mask) atomic_fetch_add(&g_mask_threads_dirty, 1);
}
void tl_signal_note_mask_clear_failed(int rc, unsigned long left)
{
    unsigned long n = atomic_fetch_add(&g_mask_clear_failed, 1);
    if (n >= 2) return;
    tl_log_line("signal-mask: clearing the mask did NOT take on this thread (pthread_sigmask returned %d and the mask still reads "
                "0x%08lx) -- a signal blocked on a thread that runs guest code is a stop-the-world handshake that never completes, so "
                "this has to be fixed, not measured again", rc, left);
}
void tl_signal_note_mask_unblocked(int gsig, int dsig, int rc)
{
    unsigned long n = atomic_fetch_add(&g_mask_unblocked, 1);
    if (n >= 4) return;
    char me[32]; sig_thread_name(me, sizeof(me));
    tl_log_line("signal-mask: '%s' is a LAYER thread (not one a guest created), so the mask it was born with is HOST code's: the guest "
                "armed a handler for signal %d (Darwin %d) on it, and Darwin %d was BLOCKED there, which would have left every delivery "
                "of it PENDING with no handler ever running. It was unblocked on this thread now (pthread_sigmask returned %d) so the "
                "thread runs guest code with the mask the guest expects", me, gsig, dsig, dsig, rc);
}

void tl_signal_report(const char *tag)
{
    char buf[420];
    unsigned long total_sent = 0, total_ran = 0, total_ref = 0, total_rearm = 0, total_failed = 0, total_blocked = 0;
    unsigned long total_ok = 0, total_esrch = 0, total_einval = 0, total_other = 0;
    unsigned long total_entered = 0, total_ptr = 0, total_called = 0, total_returned = 0;
    int any = 0;
    for (int g = 1; g <= 64; g++) {
        unsigned long i = atomic_load(&g_sig[g].installed), s = atomic_load(&g_sig[g].sent);
        unsigned long r = atomic_load(&g_sig[g].ran), f = atomic_load(&g_sig[g].refused);
        unsigned long ra = atomic_load(&g_sig[g].rearmed), fl = atomic_load(&g_sig[g].failed);
        unsigned long ab = atomic_load(&g_sig[g].arrived_blocked);
        unsigned long ok = atomic_load(&g_sig[g].delivered), es = atomic_load(&g_sig[g].esrch);
        unsigned long iv = atomic_load(&g_sig[g].einval), ot = atomic_load(&g_sig[g].other);
        unsigned long ote = atomic_load(&g_sig[g].other_errno);
        unsigned long en = atomic_load(&g_sig[g].entered), pf = atomic_load(&g_sig[g].ptr_found);
        unsigned long cl = atomic_load(&g_sig[g].called),  rt = atomic_load(&g_sig[g].returned);
        if (!i && !s && !r && !f && !ra && !fl && !ab && !ok && !es && !iv && !ot && !en) continue;
        pthread_mutex_lock(&g_sig_mu);
        char iby[32], sf[32], st[32];
        snprintf(iby, sizeof(iby), "%s", g_sig[g].inst_by);
        snprintf(sf, sizeof(sf), "%s", g_sig[g].sent_from);
        snprintf(st, sizeof(st), "%s", g_sig[g].sent_to);
        pthread_mutex_unlock(&g_sig_mu);
        char by[32] = "?";
        if (en) pthread_getname_np((pthread_t)(uintptr_t)atomic_load(&g_sig[g].first_entered_by), by, sizeof(by));
        any = 1; total_sent += s; total_ran += r; total_ref += f; total_rearm += ra; total_failed += fl; total_blocked += ab;
        total_ok += ok; total_esrch += es; total_einval += iv; total_other += ot;
        total_entered += en; total_ptr += pf; total_called += cl; total_returned += rt;
        snprintf(buf, sizeof(buf), "%s: signal %d (Darwin %d): handler installed %lu time(s) by '%s' (install(s) the host refused: %lu), sent %lu time(s) (first from '%s' to '%s'), guest handler RAN %lu time(s), refused by the layer %lu time(s), host handler re-armed before a send %lu time(s); send OUTCOME: accepted %lu, ESRCH %lu, EINVAL %lu, other %lu (errno %lu); staged chain: host handler ENTERED %lu (first by '%s'), guest pointer FOUND %lu, guest CALLED %lu, guest RETURNED %lu",
                 tag, g, tl_signal_to_darwin(g), i, iby[0] ? iby : "?", fl, s, sf[0] ? sf : "?", st[0] ? st : "?", r, f, ra,
                 ok, es, iv, ot, ote, en, by, pf, cl, rt);
        tl_log_line("%s", buf);
        /* Read the disposition BACK, here and at the close: the one fact the send/run ledger cannot see, and the line
         * that separates "our dispatcher is installed and the kernel/console still will not deliver" (the delivery path)
         * from "something overwrote the install" (named, with its image) from SIG_DFL/SIG_IGN (Darwin discards the send). */
        { int dd = tl_signal_to_darwin(g); char desc[320];
          tl_signal_describe_disposition(dd, desc, sizeof(desc));
          tl_log_line("%s: signal %d: the disposition of its Darwin number %d, read back NOW: %s", tag, g, dd, desc); }
    }
    if (!any) { tl_log_line("%s: no guest signal was installed, sent or delivered this run (the guest never armed one)", tag); return; }
    /* The mask, named while it is being removed: a thread that reached guest code with a guest-armed signal already
     * blocked is carrying HOST code's mask (inherited at thread creation), the one way a blocked bit can reach a guest
     * thread. It is the case a send/run ledger cannot see from the send side, and it is why the block is cleared where
     * guest code is entered rather than worked around at send time. */
    if (total_blocked)
        tl_log_line("%s: %lu thread(s) about to run guest code ARRIVED with a guest-armed signal BLOCKED in the mask they were born "
                    "with -- a mask from HOST code, inherited at thread creation (no guest and no layer code ever blocks it). A "
                    "thread-directed signal to such a thread stays PENDING and its handler never runs, which is what a stop-the-world "
                    "handshake hangs on; the layer CLEARED the mask before guest code ran (the signal[N] lines above name it)",
                    tag, total_blocked);
    /* The mask MEASURED, not inferred: every thread that reached guest code read its OWN mask at the handoff (the only
     * thread whose mask a process can read), so the run can say how many were born with a signal blocked -- the exact
     * fact the send/run ledger could only hypothesise ("SENDS > RUNS ... consistent with the target's mask BLOCKING it").
     * dirty_threads == 0 exonerates the mask as the cause of a delivery that never ran. */
    {
        unsigned long mseen = atomic_load(&g_mask_threads_seen);
        unsigned long mdirty = atomic_load(&g_mask_threads_dirty);
        unsigned long mfail = atomic_load(&g_mask_clear_failed);
        unsigned long munb = atomic_load(&g_mask_unblocked);
        if (mseen)
            tl_log_line("%s: signal-mask: %lu thread(s) reached guest code and READ THEIR OWN mask there; %lu were BORN with a signal "
                        "blocked (a mask from HOST code, inherited at thread creation -- no guest and no layer code blocks it), and the "
                        "layer CLEARED it before guest code ran; %lu clear(s) did not take; %lu layer-thread arm(s) had the armed number "
                        "unblocked. A dirty count of 0 is the MEASURED form of \"no guest thread ran guest code with a blocked signal\", so "
                        "if a send still ran no handler the mask is NOT the reason -- look at the number, the disposition, or a delivery "
                        "blocked elsewhere (a debugger/exception port, which is not a mask at all)",
                        tag, mseen, mdirty, mfail, munb);
    }
    /* The verdict the ledger exists for: a send whose handler did not run is a thread that never answered -- and, now that
     * the outcome of each send is recorded, the verdict can say WHICH of the two it is instead of only guessing. */
    const char *verdict;
    if (total_ref)
        verdict = "a refused send is a bionic signal number the layer has no Darwin equivalent for (>=32: Android's real-time signals); the thread it was meant for never sees it";
    else if (total_failed)
        verdict = "the HOST refused to install a guest handler (see the install lines): a send of that signal can never run a handler here, and the refusal is the reason, not the delivery";
    else if (total_ran)
        verdict = "every counted send either ran a handler or was accounted for by an outcome below -- read the outcome counters against the run count";
    else if (total_sent == 0)
        verdict = "no send was attempted, so nothing is claimed about delivery";
    else if (total_ok == 0)
        verdict = "NO SEND WAS ACCEPTED: every pthread_kill here FAILED (ESRCH/EINVAL/other above). The signal never reached a thread, so the send, not the target's mask, is why no handler ran -- the reading the old count-before-the-attempt could not produce";
    else
        verdict = "SENDS > RUNS with the sends ACCEPTED by pthread_kill (a failed send is counted in the outcomes above, not here). An accepted thread-directed signal whose handler still did not run is consistent with the target's mask BLOCKING it (Darwin leaves such a signal PENDING and runs no handler) -- but that is a HYPOTHESIS, not a measurement: no Darwin call reads another thread's mask. The measured facts are on this line and the outcome counters; the mask claim needs the target to report its own mask, which only it can do. A run that happens only LATER is not in this count yet -- the ledger prints again at the close, so RAN rising there means a pending signal finally ran";
    tl_log_line("%s: %lu guest signal send(s), %lu handler run(s), %lu refused, %lu host handler re-armed, %lu install(s) the host refused; "
                "send outcomes: %lu ACCEPTED by pthread_kill, %lu ESRCH (the thread was gone), %lu EINVAL, %lu other -- %s",
                tag, total_sent, total_ran, total_ref, total_rearm, total_failed,
                total_ok, total_esrch, total_einval, total_other, verdict);
    /* The staged chain, kept apart on purpose: the log cannot conflate "a signal reached the host dispatcher" with "the
     * guest's handler ran". A break in the chain names WHERE it broke -- and the layer stops at RETURNED, because a
     * Boehm acknowledgement is the guest's own step and the layer cannot see it. */
    if (total_entered || total_ran) {
        const char *chainverdict;
        if (total_entered == 0)
            chainverdict = "NEVER ENTERED: no guest-armed signal reached the host dispatcher at all -- the send never got there, or the disposition was not ours";
        else if (total_ptr == 0)
            chainverdict = "ENTERED but NO GUEST POINTER: the dispatcher ran for a signal with no recorded handler (SIG_IGN/SIG_DFL recorded, or a Darwin number that is not a guest signal) -- nothing to call";
        else if (total_called == 0)
            chainverdict = "pointer FOUND but never CALLED: the conversion path returned before the guest handler (should not happen; look at the handler)";
        else if (total_returned < total_called)
            chainverdict = "CALLED but did NOT all RETURN: a guest handler did not come back (longjmp, or it faulted) -- the interrupted thread never resumes";
        else
            chainverdict = "the WHOLE chain completed: host ENTERED -> guest pointer FOUND -> guest CALLED -> guest RETURNED. What the layer cannot see (and does not claim) is the guest's own acknowledgement past RETURNED";
        tl_log_line("%s: staged delivery chain: host handler ENTERED %lu, guest pointer FOUND %lu, guest CALLED %lu, guest RETURNED %lu -- %s",
                    tag, total_entered, total_ptr, total_called, total_returned, chainverdict);
    }
    /* The probe handler's own run count, read from the ledger THIS report draws from, beside the image's identity. Both
     * are printed by the self-test too; if the two disagree the layer is loaded twice and a handler one copy runs is
     * invisible to the other -- answered in one log rather than assumed away. */
    tl_log_line("%s: signal-selftest: probe handler ran %lu time(s) as recorded in the ledger THIS report reads (ledger image %p); "
                "the self-test line prints the same count and address, so a two-image layer would show it disagreeing",
                tag, (unsigned long)atomic_load(&g_sig_probe_runs), (void *)&g_sig[0]);
}


/* ------------------------------------------------------------------ notes */

void tl_note_once(const char *what)
{
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static const char *seen[256];
    static int n;
    pthread_mutex_lock(&m);
    for (int i = 0; i < n; i++) if (!strcmp(seen[i], what)) { pthread_mutex_unlock(&m); return; }
    if (n < 256) seen[n++] = strdup(what);
    pthread_mutex_unlock(&m);
    tl_log_line("bionic: %s", what);
}

/* ------------------------------------------------------------------ hot-path counters */

volatile unsigned long tl_hp_count[TL_HP_N];

static const char *const k_hp_name[TL_HP_N] = {
    "clock_gettime", "gettimeofday", "read", "pread64",
    "fstat", "fstatat", "open", "mmap",
    "mutex_lock", "mutex_unlock", "cond_wait"
};

const char *tl_hp_name(int which) { return (which >= 0 && which < TL_HP_N) ? k_hp_name[which] : "?"; }

/* --------------------------------------------- where the hot shims are called FROM
 *
 * The counters above say WHICH shim is hot; a stall also needs to say WHERE. Every 4096th call (the mask in tl_hp_bump)
 * the guest's return address is recorded here, so an app that spins on gettimeofday + pthread_mutex_lock without
 * presenting a frame -- an engine's own retry loop -- is named by the address of the function doing it, with a count,
 * and that address resolves against the app's own .so offline. 16 sites per shim; a site past the bound is counted by the
 * shim's own counter and never invented. The lock is taken only on a sampled call (a few times a second), never per call. */
#define TL_HP_SITES 16
static struct { void *ret; unsigned long n; } g_hp_sites[TL_HP_N][TL_HP_SITES];
static pthread_mutex_t g_hp_sites_mu = PTHREAD_MUTEX_INITIALIZER;

void tl_hp_note_callsite(int which, void *ret)
{
    if (which < 0 || which >= TL_HP_N || !ret) return;
    pthread_mutex_lock(&g_hp_sites_mu);
    for (int i = 0; i < TL_HP_SITES; i++)
        if (g_hp_sites[which][i].ret == ret) { g_hp_sites[which][i].n++; pthread_mutex_unlock(&g_hp_sites_mu); return; }
    for (int i = 0; i < TL_HP_SITES; i++)
        if (!g_hp_sites[which][i].ret) { g_hp_sites[which][i].ret = ret; g_hp_sites[which][i].n = 1; pthread_mutex_unlock(&g_hp_sites_mu); return; }
    pthread_mutex_unlock(&g_hp_sites_mu);          /* the table is full: the shim's counter still holds the total */
}

void tl_hp_callsites_each(void (*cb)(const char *shim, const void *ret, unsigned long n, void *user), void *user)
{
    if (!cb) return;
    pthread_mutex_lock(&g_hp_sites_mu);
    for (int i = 0; i < TL_HP_N; i++)
        for (int k = 0; k < TL_HP_SITES; k++)
            if (g_hp_sites[i][k].ret) cb(tl_hp_name(i), g_hp_sites[i][k].ret, g_hp_sites[i][k].n, user);
    pthread_mutex_unlock(&g_hp_sites_mu);
}

/* ------------------------------------------------------------ properties */

static const struct { const char *k, *v; } k_props[] = {
    { "ro.build.version.sdk", "34" }, { "ro.build.version.release", "14" },
    { "ro.build.version.release_or_codename", "14" }, { "ro.build.version.codename", "REL" },
    { "ro.build.version.incremental", "11000000" }, { "ro.build.version.security_patch", "2024-05-05" },
    { "ro.build.id", "UP1A.231105.001" }, { "ro.build.display.id", "UP1A.231105.001" },
    { "ro.build.type", "user" }, { "ro.build.tags", "release-keys" },
    { "ro.build.fingerprint", "google/shiba/shiba:14/UP1A.231105.001/11000000:user/release-keys" },
    { "ro.product.manufacturer", "Google" }, { "ro.product.brand", "google" },
    { "ro.product.model", "Pixel 8" }, { "ro.product.name", "shiba" }, { "ro.product.device", "shiba" },
    { "ro.product.board", "shiba" }, { "ro.product.cpu.abi", "arm64-v8a" },
    { "ro.product.cpu.abilist", "arm64-v8a" }, { "ro.hardware", "shiba" }, { "ro.board.platform", "gs301" },
    { "ro.soc.manufacturer", "Google" }, { "ro.soc.model", "Tensor G3" },
    { "ro.opengles.version", "196610" }, { "ro.kernel.qemu", "0" }, { "ro.debuggable", "0" },
    { "ro.secure", "1" }, { "ro.zygote", "zygote64" }, { "ro.config.low_ram", "false" },
    { "ro.sf.lcd_density", "420" }, { "ro.hwui.use_vulkan", "" }, { "persist.sys.locale", "en-US" },
    { "persist.sys.timezone", "UTC" }, { "gsm.operator.iso-country", "us" }, { "ro.carrier", "unknown" },
    { "ro.com.google.clientidbase", "android-google" }, { "debug.hwui.renderer", "" },
    { "ro.vendor.build.fingerprint", "google/shiba/shiba:14/UP1A.231105.001/11000000:user/release-keys" },
    { "ro.system.build.version.sdk", "34" }, { "ro.product.first_api_level", "34" },
};

const char *tl_sysprop(const char *name)
{
    for (size_t i = 0; i < sizeof(k_props) / sizeof(k_props[0]); i++) if (!strcmp(k_props[i].k, name)) return k_props[i].v;
    return NULL;
}

/* __system_property_find returns an opaque pointer that read() takes back. */
/* sysinfo(2): what the engines ask for to size their caches -- how much RAM there is, and how much is free. */
typedef struct { long uptime; unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap; unsigned short procs, pad; unsigned long totalhigh, freehigh; unsigned int mem_unit; char pad2[4]; } guest_sysinfo;
static int bionic_sysinfo(guest_sysinfo *si)
{
    memset(si, 0, sizeof(*si));
    uint64_t mem = 0; size_t len = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    si->uptime = ts.tv_sec;
    si->totalram = (unsigned long)mem;
    si->freeram = (unsigned long)(mem / 4);
    si->mem_unit = 1;
    si->procs = 1;
    return 0;
}

/* CPU_COUNT(): the number of CPUs in a set. */
static int bionic_sched_cpucount(size_t setsize, const unsigned char *set)
{
    int n = 0;
    for (size_t i = 0; i < setsize; i++) n += __builtin_popcount(set[i]);
    return n;
}

static const void *bionic___system_property_find(const char *name)
{
    for (size_t i = 0; i < sizeof(k_props) / sizeof(k_props[0]); i++) if (!strcmp(k_props[i].k, name)) return &k_props[i];
    return NULL;
}

static int bionic___system_property_read(const void *pi, char *name, char *value)
{
    if (!pi) return 0;
    const struct { const char *k, *v; } *p = pi;
    if (name) snprintf(name, 32, "%s", p->k);
    if (value) snprintf(value, 92, "%s", p->v);
    return (int)strlen(p->v);
}

/* __system_property_foreach: each property Husk answers, as a find() handle */
static int bionic___system_property_foreach(void (*fn)(const void *pi, void *cookie), void *cookie)
{
    if (!fn) return -1;
    for (size_t i = 0; i < sizeof(k_props) / sizeof(k_props[0]); i++) fn(&k_props[i], cookie);
    return 0;
}

int tl_dns_servers(char out[][64], int max);

static int bionic___system_property_get(const char *name, char *value)
{
    /* net.dns1, net.dns2: the phone's DNS servers (see tl_dns_servers) */
    if (name && value && !strncmp(name, "net.dns", 7) && name[7] >= '1' && name[7] <= '4' && !name[8]) {
        char servers[4][64];
        int n = tl_dns_servers(servers, 4), i = name[7] - '1';
        snprintf(value, 92, "%s", i < n ? servers[i] : "");
        return (int)strlen(value);
    }
    const char *v = name ? tl_sysprop(name) : NULL;
    /* TL_SYSPROPS="key=value;key=value" (host tests): properties an app reads for its own debugging (debug.mono.log) */
    static __thread char over[92];
    const char *env = name ? getenv("TL_SYSPROPS") : NULL;
    for (const char *p = env; p && *p; ) {
        const char *end = strchr(p, ';'); size_t l = end ? (size_t)(end - p) : strlen(p), nl = strlen(name);
        if (l > nl && !strncmp(p, name, nl) && p[nl] == '=') { snprintf(over, sizeof(over), "%.*s", (int)(l - nl - 1), p + nl + 1); v = over; break; }
        p = end ? end + 1 : NULL;
    }
    if (!value) return 0;
    if (!v) { value[0] = 0; return 0; }
    snprintf(value, 92, "%s", v);
    return (int)strlen(value);
}
int tl_property_get(const char *name, char *value) { return bionic___system_property_get(name, value); }
/* every property, as getprop prints them ([name]: [value]) */
size_t tl_property_list(char *buf, size_t n)
{
    size_t o = 0; buf[0] = 0;
    for (size_t i = 0; i < sizeof(k_props) / sizeof(k_props[0]) && o + 1 < n; i++) {
        char v[92]; bionic___system_property_get(k_props[i].k, v);
        o += (size_t)snprintf(buf + o, n - o, "[%s]: [%s]\n", k_props[i].k, v);
    }
    return o < n ? o : n - 1;
}

/* The rest of the property API Android's own libraries use (ART's libbase reads properties through the callback). */
typedef void (*guest_prop_cb)(void *cookie, const char *name, const char *value, uint32_t serial);
static void bionic___system_property_read_callback(const void *pi, guest_prop_cb cb, void *cookie)
{
    if (!pi || !cb) return;
    const struct { const char *k, *v; } *p = pi;
    cb(cookie, p->k, p->v, 0);
}
static uint32_t bionic___system_property_serial(const void *pi) { (void)pi; return 0; }
static uint32_t bionic___system_property_area_serial(void) { return 0; }
/* Nothing ever changes a property here, so a wait always times out. */
static bool bionic___system_property_wait(const void *pi, uint32_t old, uint32_t *serial, const void *timeout)
{
    (void)pi; (void)old; (void)timeout;
    if (serial) *serial = 0;
    return false;
}
static int bionic___system_property_set(const char *name, const char *value) { (void)name; (void)value; return 0; }

/* ---------------------------------------------------------------- logging */

static void log_emit(int prio, const char *tag, const char *text)
{
    static const char letters[] = "??VDIWEF";
    char p = (prio >= 0 && prio < 8) ? letters[prio] : '?';
    tl_log_line("%c/%s: %s", p, tag ? tag : "guest", text ? text : "");
}

static int bionic___android_log_write(int prio, const char *tag, const char *text)
{
    log_emit(prio, tag, text);
    return 1;
}

/* liblog's API since Android 11: a minimum priority, a pluggable logger and aborter. Android's own libraries (libbase) call these. */
typedef struct { size_t struct_size; int32_t buffer_id; int32_t priority; const char *tag; const char *file; uint32_t line; const char *message; } guest_log_message;
typedef void (*guest_logger)(const guest_log_message *);
typedef void (*guest_aborter)(const char *);
static _Atomic int32_t g_log_min;                 /* ANDROID_LOG_DEFAULT (0): info and up */
static _Atomic(guest_logger) g_logger;
static _Atomic(guest_aborter) g_aborter;
static const char *_Atomic g_default_tag;

static int bionic___android_log_is_loggable(int prio, const char *tag, int def)
{
    (void)tag;
    int32_t min = g_log_min ? g_log_min : (def ? def : 4);
    return prio >= min;
}
static int bionic___android_log_is_loggable_len(int prio, const char *tag, size_t len, int def) { (void)len; return bionic___android_log_is_loggable(prio, tag, def); }
static int32_t bionic___android_log_get_minimum_priority(void) { return g_log_min; }
static int32_t bionic___android_log_set_minimum_priority(int32_t p) { return atomic_exchange(&g_log_min, p); }
static void bionic___android_log_set_default_tag(const char *tag) { g_default_tag = tag; }
static void bionic___android_log_logd_logger(const guest_log_message *m)
{
    if (m) log_emit(m->priority, m->tag ? m->tag : g_default_tag, m->message);
}
static void bionic___android_log_set_logger(guest_logger fn) { g_logger = fn; }
static void bionic___android_log_write_log_message(guest_log_message *m)
{
    if (!m) return;
    /* as liblog does: a message without a tag gets the default one (the process name, unless set) */
    if (!m->tag) { const char *t = g_default_tag; m->tag = t ? t : "app_process64"; }
    guest_logger fn = g_logger;
    if (fn && fn != bionic___android_log_logd_logger) fn(m); else bionic___android_log_logd_logger(m);
}
static void bionic___android_log_default_aborter(const char *msg)
{
    tl_log_line("bionic: abort message: %s", msg ? msg : "");
    /* a guest LOG_ALWAYS_FATAL ends the APP, not Husk (the guest shares Husk's process) */
    tl_guest_exit_unwind("__android_log_default_aborter", 6 /* SIGABRT */);
}
static void bionic___android_log_set_aborter(guest_aborter fn) { g_aborter = fn; }
static void bionic___android_log_call_aborter(const char *msg)
{
    guest_aborter fn = g_aborter;
    if (fn && fn != bionic___android_log_default_aborter) fn(msg); else bionic___android_log_default_aborter(msg);
}

static int bionic___android_log_buf_write(int buf, int prio, const char *tag, const char *text)
{
    (void)buf;
    log_emit(prio, tag, text);
    return 1;
}

static int bionic___android_log_vprint(int prio, const char *tag, const char *fmt, tl_va_list *ap)
{
    char text[2048];
    tl_format(text, sizeof(text), fmt, ap);
    log_emit(prio, tag, text);
    return 1;
}

int tl_vai_android_log_print(tl_va_frame *f)
{
    tl_va_list ap; char text[2048];
    tl_va_start(f, 3, 0, &ap);
    tl_format(text, sizeof(text), (const char *)f->gp[2], &ap);
    log_emit((int)f->gp[0], (const char *)f->gp[1], text);
    return 1;
}
TL_VA_STUB(tl_va_android_log_print, tl_vai_android_log_print);
extern void tl_va_android_log_print(void);

int tl_vai_android_log_buf_print(tl_va_frame *f)
{
    tl_va_list ap; char text[2048];
    tl_va_start(f, 4, 0, &ap);
    tl_format(text, sizeof(text), (const char *)f->gp[3], &ap);
    log_emit((int)f->gp[1], (const char *)f->gp[2], text);
    return 1;
}
TL_VA_STUB(tl_va_android_log_buf_print, tl_vai_android_log_buf_print);
extern void tl_va_android_log_buf_print(void);

static void guest_abort(const char *why);

int tl_vai_android_log_assert(tl_va_frame *f)
{
    /* __android_log_assert(cond, tag, fmt, ...) */
    tl_va_list ap; char text[2048];
    const char *fmt = (const char *)f->gp[2];
    if (fmt) { tl_va_start(f, 3, 0, &ap); tl_format(text, sizeof(text), fmt, &ap); }
    else snprintf(text, sizeof(text), "assertion failed: %s", f->gp[0] ? (const char *)f->gp[0] : "?");
    log_emit(7, (const char *)f->gp[1], text);
    guest_abort("__android_log_assert");
    return 0;
}
TL_VA_STUB(tl_va_android_log_assert, tl_vai_android_log_assert);
extern void tl_va_android_log_assert(void);

int tl_vai_syslog(tl_va_frame *f)
{
    tl_va_list ap; char text[1024];
    tl_va_start(f, 2, 0, &ap);
    tl_format(text, sizeof(text), (const char *)f->gp[1], &ap);
    log_emit(4, "syslog", text);
    return 0;
}
TL_VA_STUB(tl_va_syslog, tl_vai_syslog);
extern void tl_va_syslog(void);

static void bionic_openlog(const char *ident, int option, int facility) { (void)ident; (void)option; (void)facility; }
/* vsyslog: bionic's va_list is AAPCS64's struct, not the host's; the format is logged as it stands */
static void bionic_vsyslog(int pri, const char *fmt, void *ap) { (void)pri; (void)ap; log_emit(4, "syslog", fmt ? fmt : ""); }
/* __progname: the program's name, as libc keeps it for err() and syslog */
static const char *g_progname = "app_process64";
static void bionic_closelog(void) {}

/* --------------------------------------------------- C runtime and aborts */

static void describe_caller(void *lr, char *out, size_t n)
{
    const char *ln = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at(lr, &ln, &sa);
    extern void *tl_ld_lib_base(const tl_lib *L);
    if (ln && sym) snprintf(out, n, "%s %s+%#lx", ln, sym, sa ? (unsigned long)((const char *)lr - (const char *)sa) : 0ul);
    else if (ln) snprintf(out, n, "%s +%#lx", ln, (unsigned long)((const char *)lr - (const char *)tl_ld_lib_base(tl_ld_lib_of(lr))));
    else snprintf(out, n, "%p", lr);
}
/* the guest's frames above an exit or abort, by frame pointer */
static void log_guest_frames(void *fp)
{
    for (int i = 0; i < 16 && fp && ((uintptr_t)fp & 7) == 0; i++) {
        void **f = fp;
        void *lr = f[1];
        if (!lr) break;
        char where[200]; describe_caller(lr, where, sizeof(where));
        tl_log_line("bionic:   from %s", where);
        if (f[0] <= fp) break;
        fp = f[0];
    }
}

static void guest_abort(const char *why)
{
    char where[200];
    describe_caller(__builtin_return_address(0), where, sizeof(where));
    tl_log_line("bionic: guest abort (%s) at %s", why, where);
    /* a guest abort (a failed assertion, a stack smash) ends the APP, never Husk: the guest shares Husk's process */
    tl_guest_exit_unwind("abort", 6 /* SIGABRT */);
}

static void bionic_abort(void)
{
    char where[200];
    describe_caller(__builtin_return_address(0), where, sizeof(where));
    tl_log_line("bionic: abort() called from %s", where);
    tl_guest_exit_unwind("abort", 6 /* SIGABRT */);
}

/* When set, a guest exit() ends the guest, not the app that is hosting it. The hook does not return. */
void (*tl_guest_exit_hook)(int status);

/* --------------------------------------------------------- the guest exit family */

/*
 * The guest shares Husk's process, so any guest call that means "quit my app" must end the APP, never
 * the process. Before this, Runtime.exit/System.exit/Process.killProcess/JVM_Exit called the host's
 * _exit() straight through, and exit()/abort() fell through to Darwin's: a Ren'Py game ran its own
 * real quit path (working as designed) and Husk died with it -- the log's last line was
 * "dvm: Runtime.exit(0)" and the session close already running never finished (no "session closed").
 * A close in flight is the app confirming it is done, not a new request; the caller unwinds the app
 * and lets the close complete. This flag is set by husk_tl_session_close() for its duration.
 */
static _Atomic bool g_guest_exit_close_in_flight;

void tl_guest_exit_set_close_in_flight(bool in_flight) { atomic_store(&g_guest_exit_close_in_flight, in_flight); }
bool tl_guest_exit_close_in_flight(void) { return atomic_load(&g_guest_exit_close_in_flight); }

/* One honest line naming the call, one saying what the layer did instead. Returns true if a session
 * close is already in flight (the caller must let the app unwind itself, never re-enter the close). */
bool husk_tl_guest_exit_requested(const char *call, int status)
{
    bool closing = atomic_load(&g_guest_exit_close_in_flight);
    const char *c = call ? call : "exit";
    tl_log_line("husk: the app asked to exit (%s(%d))", c, status);
    if (closing)
        tl_log_line("husk: %s(%d) arrived while the session was closing; the app is being unwound, Husk stays", c, status);
    else
        tl_log_line("husk: %s(%d) from guest code: the app is unwound and Husk stays for the next app (never the host process)", c, status);
    return closing;
}

/* The Java driver's side of a guest exit (husk-tl-dvm-android.c): mark the running app ended, and
 * note that the calling thread is leaving if it is one of the app's own. Declared here rather than
 * pulling the DVM headers into the bionic shim. */
void tl_javaapp_request_exit(void);
void tl_javaapp_thread_leaving(void);

/*
 * The one way a guest exit()/_exit()/abort() is honoured: end the app's world and the CALLING thread,
 * never the host process. A native engine's own hook runs first when set; the calling thread then
 * ends itself, which is exactly what exit() means and is not a cancel of anyone else's thread (the
 * design forbids pthread_kill/pthread_cancel on a thread that ran guest code). Never returns.
 */
void tl_guest_exit_unwind(const char *call, int status)
{
    husk_tl_guest_exit_requested(call, status);
    tl_javaapp_request_exit();
    tl_javaapp_thread_leaving();
    if (tl_guest_exit_hook) tl_guest_exit_hook(status);
    pthread_exit(NULL);
}

static void bionic_exit(int status)
{
    char where[200];
    describe_caller(__builtin_return_address(0), where, sizeof(where));
    tl_log_line("bionic: exit(%d) called from %s", status, where);
    log_guest_frames(__builtin_frame_address(1));
    tl_guest_exit_unwind("exit", status);
}

/* _exit/_Exit: the same act, skipping the guest's stdio flush and the frames it passed over */
static void bionic__exit(int status)
{
    char where[200];
    describe_caller(__builtin_return_address(0), where, sizeof(where));
    tl_log_line("bionic: _exit(%d) called from %s", status, where);
    tl_guest_exit_unwind("_exit", status);
}

static void bionic___stack_chk_fail(void) { guest_abort("stack smashing detected"); }

static void bionic___assert2(const char *file, int line, const char *func, const char *expr)
{
    char where[200];
    describe_caller(__builtin_return_address(0), where, sizeof(where));
    tl_log_line("bionic: assertion failed: %s:%d: %s: %s (from %s)", file, line, func ? func : "?", expr, where);
    tl_guest_exit_unwind("__assert2", 6 /* SIGABRT */);
}

/* fdsan: Android's file descriptor ownership checks. Owner tags are kept per descriptor and never enforced. */
static _Atomic uint64_t g_fd_tags[4096];
static uint64_t bionic_android_fdsan_create_owner_tag(int type, uint64_t tag) { return ((uint64_t)(type & 0xff) << 56) | (tag & 0x00ffffffffffffffull); }
static void bionic_android_fdsan_exchange_owner_tag(int fd, uint64_t expected, uint64_t tag) { (void)expected; if (fd >= 0 && fd < 4096) atomic_store(&g_fd_tags[fd], tag); }
static uint64_t bionic_android_fdsan_get_owner_tag(int fd) { return fd >= 0 && fd < 4096 ? atomic_load(&g_fd_tags[fd]) : 0; }
static const char *bionic_android_fdsan_get_tag_type(uint64_t tag)
{
    static const char *const k[] = { "unowned", "FILE*", "DIR*", "unique_fd", "sqlite", "FileInputStream", "FileOutputStream", "RandomAccessFile", "ParcelFileDescriptor", "ART FdFile", "DatagramSocketImpl", "SocketImpl", "ZipArchive" };
    unsigned t = (unsigned)(tag >> 56);
    return t < sizeof(k) / sizeof(k[0]) ? k[t] : "native object of unknown type";
}
static uint64_t bionic_android_fdsan_get_tag_value(uint64_t tag) { return tag & 0x00ffffffffffffffull; }
int tl_guest_close(int fd);
static int bionic_android_fdsan_close_with_tag(int fd, uint64_t tag) { (void)tag; if (fd >= 0 && fd < 4096) atomic_store(&g_fd_tags[fd], 0); return tl_guest_close(fd); }
static int g_fdsan_level;
static int bionic_android_fdsan_get_error_level(void) { return g_fdsan_level; }
static int bionic_android_fdsan_set_error_level(int l) { int o = g_fdsan_level; g_fdsan_level = l; return o; }
static int bionic_android_fdsan_set_error_level_from_property(int l) { return bionic_android_fdsan_set_error_level(l); }
static int bionic_tcdrain(int fd) { (void)fd; return 0; }
static int bionic_tcsendbreak(int fd, int d) { (void)fd; (void)d; return 0; }
static int bionic_munlock(const void *a, size_t l) { (void)a; (void)l; return 0; }

static void bionic_android_set_abort_message(const char *msg) { tl_log_line("bionic: abort message: %s", msg ? msg : "(null)"); }

static void bionic___libc_init(void) {}

/* Fork handlers have nothing to run for: this process never forks guest code. */
static int bionic___register_atfork(void *prepare, void *parent, void *child, void *dso)
{
    (void)prepare; (void)parent; (void)child; (void)dso;
    return 0;
}
static int bionic_pthread_atfork(void *prepare, void *parent, void *child) { (void)prepare; (void)parent; (void)child; return 0; }

typedef struct { void (*fn)(void *); void *arg; void *dso; } atexit_ent;
static atexit_ent *g_atexit;       /* grows: a big C++ program registers a destructor per static object, thousands of them */
static int g_natexit, g_capatexit;
static pthread_mutex_t g_atexit_lock = PTHREAD_MUTEX_INITIALIZER;

static int bionic___cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    pthread_mutex_lock(&g_atexit_lock);
    if (g_natexit == g_capatexit) {
        int cap = g_capatexit ? g_capatexit * 2 : 1024;
        atexit_ent *n = realloc(g_atexit, (size_t)cap * sizeof(atexit_ent));
        if (n) { g_atexit = n; g_capatexit = cap; }
    }
    int ok = g_natexit < g_capatexit;
    if (ok) g_atexit[g_natexit++] = (atexit_ent){ fn, arg, dso };
    pthread_mutex_unlock(&g_atexit_lock);
    return ok ? 0 : -1;
}

static void bionic___cxa_finalize(void *dso)
{
    for (;;) {
        atexit_ent e = {0};
        pthread_mutex_lock(&g_atexit_lock);
        for (int i = g_natexit - 1; i >= 0; i--) {
            if (g_atexit[i].fn && (!dso || g_atexit[i].dso == dso)) {
                e = g_atexit[i];
                g_atexit[i].fn = NULL;
                break;
            }
        }
        pthread_mutex_unlock(&g_atexit_lock);
        if (!e.fn) return;
        e.fn(e.arg);
    }
}

/* -------------------------------------------------------- system queries */

unsigned long tl_bionic_getauxval(unsigned long type);
static unsigned long bionic_getauxval(unsigned long type) { return tl_bionic_getauxval(type); }
unsigned long tl_bionic_getauxval(unsigned long type)
{
    static uint8_t random16[16] = { 0x4a, 0x13, 0x9c, 0x71, 0xe2, 0x05, 0x88, 0x3d, 0xb6, 0x21, 0x5f, 0xc4, 0x90, 0x2e, 0x67, 0xd8 };
    switch (type) {
    case 6:  return 16384;                                   /* AT_PAGESZ */
    case 16: return 0xff;                                    /* AT_HWCAP: FP, ASIMD, EVTSTRM, AES, PMULL, SHA1, SHA2, CRC32 */
    case 26: return 0;                                       /* AT_HWCAP2 */
    case 25: return (unsigned long)random16;                 /* AT_RANDOM */
    case 17: return 100;                                     /* AT_CLKTCK */
    case 23: return 0;                                       /* AT_SECURE */
    default: errno = 0; tl_set_guest_errno(2); return 0;
    }
}

static int bionic_getpagesize(void) { return 16384; }

static long bionic_sysconf(int name)
{
    switch (name) {
    case 0:    return 131072;                                /* _SC_ARG_MAX */
    case 5:    return 1024;                                  /* _SC_CHILD_MAX */
    case 6:    return 100;                                   /* _SC_CLK_TCK */
    case 10:   return 65536;                                 /* _SC_NGROUPS_MAX */
    case 11:   return 32768;                                 /* _SC_OPEN_MAX */
    case 27:   return 20;                                    /* _SC_STREAM_MAX */
    case 28:   return 6;                                     /* _SC_TZNAME_MAX */
    case 37:   return 2147483647;                            /* _SC_ATEXIT_MAX */
    case 38:   return 1024;                                  /* _SC_IOV_MAX */
    case 39: case 40: return 16384;                          /* _SC_PAGESIZE / _SC_PAGE_SIZE */
    case 96: case 97: return getenv("TL_NCPU") ? atol(getenv("TL_NCPU")) : (long)sysconf(_SC_NPROCESSORS_ONLN);   /* _SC_NPROCESSORS_CONF / ONLN */
    case 98: { uint64_t m = 0; size_t l = sizeof(m); sysctlbyname("hw.memsize", &m, &l, NULL, 0); return (long)(m / 16384); }
    case 99: { uint64_t m = 0; size_t l = sizeof(m); sysctlbyname("hw.memsize", &m, &l, NULL, 0); return (long)(m / 16384 / 2); }
    case 100:  return 200809;                                /* _SC_MONOTONIC_CLOCK */
    case 0x4a: return 16384;                                 /* _SC_THREAD_STACK_MIN-ish */
    default:
        tl_set_guest_errno(22);
        return -1;
    }
}

static int bionic_uname(void *buf)
{
    /* bionic: six fields of 65 bytes. */
    char *b = buf;
    memset(b, 0, 65 * 6);
    snprintf(b + 65 * 0, 65, "Linux");
    snprintf(b + 65 * 1, 65, "localhost");
    snprintf(b + 65 * 2, 65, "5.15.119-android14-11-g1");
    snprintf(b + 65 * 3, 65, "#1 SMP PREEMPT Fri Nov 03 00:00:00 UTC 2023");
    snprintf(b + 65 * 4, 65, "aarch64");
    snprintf(b + 65 * 5, 65, "localdomain");
    return 0;
}

/* the same answer for uname by raw system call (fingerprinting libraries avoid libc: FingerprintJS's libfp in Airbnb) */
int tl_bionic_uname(void *buf) { return bionic_uname(buf); }

static int bionic_gethostname(char *name, size_t len) { snprintf(name, len, "localhost"); return 0; }

static int bionic_gettid(void)
{
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (int)tid;
}

static int bionic_getpriority(int which, int who) { (void)which; (void)who; return 0; }
static int bionic_setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }

static int bionic_sched_getaffinity(int pid, size_t size, void *mask)
{
    (void)pid;
    memset(mask, 0, size);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < n && (size_t)(i / 8) < size; i++) ((uint8_t *)mask)[i / 8] |= (uint8_t)(1u << (i % 8));
    return 0;
}
static int bionic_sched_setaffinity(int pid, size_t size, const void *mask) { (void)pid; (void)size; (void)mask; return 0; }
static int bionic_sched_getparam(int pid, int *param) { (void)pid; if (param) *param = 0; return 0; }
static int bionic_sched_getscheduler(int pid) { (void)pid; return 0; }

static int bionic_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
    (void)a3; (void)a4; (void)a5;
    switch (option) {
    case 15: pthread_setname_np((const char *)a2); return 0;                 /* PR_SET_NAME */
    case 16: pthread_getname_np(pthread_self(), (char *)a2, 16); return 0;   /* PR_GET_NAME */
    case 0x53564d41: return 0;                                               /* PR_SET_VMA (naming anonymous memory) */
    case 38: case 4: case 36: return 0;                                      /* NO_NEW_PRIVS, DUMPABLE, THP: accepted, nothing to do */
    default:
        tl_set_guest_errno(22);
        return -1;
    }
}

long tl_linux_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long nr);

static long bionic_syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6)
{
    (void)a6;
    long r = tl_linux_syscall(a1, a2, a3, a4, a5, a6, num);
    if (r < 0 && r > -4096) { tl_set_guest_errno((int)-r); return -1; }
    return r;
}

static int bionic_fork(void) { tl_set_guest_errno(38); return -1; }
static int bionic_execv(const char *p, char *const a[]) { (void)p; (void)a; tl_set_guest_errno(38); return -1; }
static int bionic_execve(const char *p, char *const a[], char *const e[]) { (void)p; (void)a; (void)e; tl_set_guest_errno(38); return -1; }
/* posix_spawn reports its error as the return value */
static int bionic_posix_spawn(void *pid, const char *p, const void *fa, const void *at, char *const a[], char *const e[]) { (void)pid; (void)p; (void)fa; (void)at; (void)a; (void)e; return 38; }
static int bionic_waitpid(int pid, int *st, int opt) { (void)pid; (void)st; (void)opt; tl_set_guest_errno(10); return -1; }
static long bionic_ptrace(int req, int pid, void *addr, void *data) { (void)req; (void)pid; (void)addr; (void)data; tl_set_guest_errno(1); return -1; }

/* A passwd entry like Android's: an app user. */
/* bionic's LP64 struct passwd: name, passwd, uid, gid, gecos, dir, shell (an app's user, as bionic makes it from the uid) */
typedef struct { char *pw_name, *pw_passwd; uint32_t pw_uid, pw_gid; char *pw_gecos, *pw_dir, *pw_shell; } guest_passwd;
static guest_passwd g_pwd = { "u0_a1", NULL, 10001, 10001, NULL, "/data", "/system/bin/sh" };
const void *tl_bionic_app_passwd(void) { return &g_pwd; }
static void *bionic_getpwuid(uint32_t uid) { (void)uid; return &g_pwd; }
static int bionic_getpwuid_r(uint32_t uid, void *pwd, char *buf, size_t n, void **result)
{
    (void)uid; (void)buf; (void)n;
    memcpy(pwd, &g_pwd, sizeof(g_pwd));
    *result = pwd;
    return 0;
}

/* ------------------------------------------------------------------ dl*** */

typedef struct { int unused; } sysh;
static sysh g_sys_handle[16];
static char g_sys_names[16][48];
static int g_nsys;
static __thread char t_dlerr[200];
static __thread bool t_dlerr_set;

static void dl_fail(const char *fmt, const char *arg)
{
    snprintf(t_dlerr, sizeof(t_dlerr), fmt, arg);
    t_dlerr_set = true;
}

static void *dlopen_impl(const char *path, int flags);
/* __loader_dlopen, which bionic's dlopen calls with its caller: the first call in dlopen's code is to it, as on a phone, because
 * liblinkernsbypass (GameNative, Turnip loaders) finds __loader_dlopen by walking dlopen's instructions to its first BL */
__attribute__((noinline)) static void *bionic_loader_dlopen(const char *path, int flags, const void *caller) { (void)caller; return dlopen_impl(path, flags); }
__attribute__((noinline, disable_tail_calls)) static void *bionic_dlopen(const char *path, int flags) { return bionic_loader_dlopen(path, flags, __builtin_return_address(0)); }
static void *dlopen_impl(const char *path, int flags)
{
    if (!path) return &g_sys_handle[0];          /* the global namespace */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    /* RTLD_NOLOAD (bionic: 4): only one already loaded. Geode finds the game this way. */
    if (flags & 4) {
        tl_lib *L = tl_ld_find_lib(base);
        if (getenv("TL_DL_TRACE")) tl_log_line("dl: dlopen(%s, NOLOAD) -> %s", path, L ? "loaded" : "not loaded");
        if (!L) dl_fail("dlopen failed: library \"%s\" is not loaded", path);
        return L;
    }
    /* libvulkan.so.1 is the versioned name Godot 4 asks for: the same loader */
    if ((!strcmp(base, "libvulkan.so") || !strcmp(base, "libvulkan.so.1")) && !tl_vk_available()) { dl_fail("dlopen failed: library \"%s\" not found", path); return NULL; }
    if (tl_bionic_is_system_lib(base)) {
        for (int i = 0; i < g_nsys; i++) if (!strcmp(g_sys_names[i], base)) return &g_sys_handle[i + 1];
        if (g_nsys < 15) { snprintf(g_sys_names[g_nsys], 48, "%s", base); g_nsys++; return &g_sys_handle[g_nsys]; }
        return &g_sys_handle[0];
    }
    /* A path to a file of its own (a mod's library) loads from there; a bare name, or a path into the app, from the APK. */
    tl_lib *L = tl_ld_find_lib(base);
    if (!L) L = tl_ld_load(path[0] == '/' ? path : base);
    if (getenv("TL_DL_TRACE")) tl_log_line("dl: dlopen(%s) -> %s", path, L ? "ok" : "not found");
    if (!L) { dl_fail("dlopen failed: library \"%s\" not found", path); return NULL; }
    tl_ld_init(L);
    return L;
}

static void *bionic_dlsym(void *handle, const char *name);
static void *bionic_loader_android_dlopen_ext(const char *path, int flags, const void *info, const void *caller) { (void)info; (void)caller; return bionic_dlopen(path, flags); }
static void *bionic_android_dlopen_ext(const char *path, int flags, const void *info) { (void)info; return bionic_dlopen(path, flags); }
static void *bionic_loader_dlsym(void *handle, const char *name, const void *caller) { (void)caller; return bionic_dlsym(handle, name); }
static void *bionic_loader_dlvsym(void *handle, const char *name, const char *ver, const void *caller) { (void)ver; (void)caller; return bionic_dlsym(handle, name); }

static void *bionic_dlsym(void *handle, const char *name)
{
    if (!name) return NULL;
    void *r = NULL;
    if (handle == (void *)-1L) {
        /* RTLD_NEXT: the definition after the caller's own, which for a library wrapping libc (ART's signal chain) is libc's */
        r = tl_bionic_find(name);
        if (!r) r = tl_ld_sym(NULL, name);
    } else if (handle == NULL) {
        r = tl_ld_sym(NULL, name);
        if (!r) r = tl_bionic_find(name);
    } else if ((sysh *)handle >= g_sys_handle && (sysh *)handle < g_sys_handle + 16) {
        r = tl_bionic_find(name);
    } else {
        /* the library, then what it needs, breadth-first, as bionic's dlsym: .NET asks libicui18n for ucurr_forLocale, which
           libicuuc (its dependency) defines */
        void *tl_ld_sym_deep(tl_lib *lib, const char *name, tl_lib **found_in);
        r = tl_ld_sym_deep((tl_lib *)handle, name, NULL);
    }
    if (getenv("TL_DL_TRACE")) tl_log_line("dl: dlsym(%s) -> %s", name, r ? "found" : "missing");
    if (!r) dl_fail("undefined symbol: %s", name);
    return r;
}

static int bionic_dlclose(void *handle) { (void)handle; return 0; }

static char *bionic_dlerror(void)
{
    if (!t_dlerr_set) return NULL;
    t_dlerr_set = false;
    return t_dlerr;
}

typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } guest_dl_info;
static int bionic_dladdr(const void *addr, guest_dl_info *info)
{
    const char *ln = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at(addr, &ln, &sa);
    if (!ln) return 0;
    info->dli_fname = ln;
    /* The base of the library the address is in: Geode computes every hook address from the game's. */
    info->dli_fbase = tl_ld_lib_base(tl_ld_lib_of(addr));
    info->dli_sname = sym;
    info->dli_saddr = (void *)sa;
    return 1;
}

typedef struct { uint64_t addr; const char *name; const void *phdr; uint16_t phnum; uint64_t adds, subs; size_t tls_modid; void *tls_data; } guest_phdr_info;
typedef int (*guest_phdr_cb)(guest_phdr_info *, size_t, void *);
struct phdr_ctx { guest_phdr_cb cb; void *data; };
static int phdr_trampoline(uintptr_t bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    struct phdr_ctx *c = user;
    guest_phdr_info i = { bias, name, phdr, (uint16_t)phnum, 0, 0, 0, NULL };
    return c->cb(&i, sizeof(i), c->data);
}
static int bionic_dl_iterate_phdr(guest_phdr_cb cb, void *data)
{
    struct phdr_ctx c = { cb, data };
    return tl_ld_iterate(phdr_trampoline, &c);
}

/* ------------------------------------------------------------------ data */

static char **g_environ_slot;
static char **tl_environ_ptr(void) { return NULL; }

/* ------------------------------------------------------------- the table */

const tl_bionic_entry tl_tab_core[] = {
    TL_WRAP("__errno", bionic___errno),
    TL_WRAP("__system_property_find", bionic___system_property_find),
    TL_WRAP("__system_property_read", bionic___system_property_read), TL_WRAP("__system_property_foreach", bionic___system_property_foreach),
    TL_WRAP("__system_property_get", bionic___system_property_get),
    TL_WRAP("__system_property_read_callback", bionic___system_property_read_callback),
    TL_WRAP("__system_property_serial", bionic___system_property_serial),
    TL_WRAP("__system_property_area_serial", bionic___system_property_area_serial),
    TL_WRAP("__system_property_wait", bionic___system_property_wait),
    TL_WRAP("__system_property_set", bionic___system_property_set),
    TL_WRAP("__android_log_is_loggable", bionic___android_log_is_loggable),
    TL_WRAP("__android_log_is_loggable_len", bionic___android_log_is_loggable_len),
    TL_WRAP("__android_log_get_minimum_priority", bionic___android_log_get_minimum_priority),
    TL_WRAP("__android_log_set_minimum_priority", bionic___android_log_set_minimum_priority),
    TL_WRAP("__android_log_set_default_tag", bionic___android_log_set_default_tag),
    TL_WRAP("__android_log_logd_logger", bionic___android_log_logd_logger),
    TL_WRAP("__android_log_stderr_logger", bionic___android_log_logd_logger),
    TL_WRAP("__android_log_set_logger", bionic___android_log_set_logger),
    TL_WRAP("__android_log_write_log_message", bionic___android_log_write_log_message),
    TL_WRAP("__android_log_default_aborter", bionic___android_log_default_aborter),
    TL_WRAP("__android_log_set_aborter", bionic___android_log_set_aborter),
    TL_WRAP("__android_log_call_aborter", bionic___android_log_call_aborter),
    TL_WRAP("__android_log_write", bionic___android_log_write),
    TL_WRAP("__android_log_buf_write", bionic___android_log_buf_write),
    TL_WRAP("__android_log_vprint", bionic___android_log_vprint),
    TL_WRAP("__android_log_print", tl_va_android_log_print),
    TL_WRAP("__android_log_buf_print", tl_va_android_log_buf_print),
    TL_WRAP("__android_log_assert", tl_va_android_log_assert),
    TL_WRAP("syslog", tl_va_syslog),
    TL_WRAP("openlog", bionic_openlog),
    TL_WRAP("vsyslog", bionic_vsyslog),
    TL_DATA("__progname", &g_progname),
    TL_DIRECT(strsep), TL_DIRECT(arc4random_uniform),
    TL_WRAP("closelog", bionic_closelog),
    TL_WRAP("abort", bionic_abort),
    TL_WRAP("exit", bionic_exit),
    TL_WRAP("_exit", bionic__exit), TL_WRAP("_Exit", bionic__exit),
    TL_WRAP("__stack_chk_fail", bionic___stack_chk_fail),
    TL_WRAP("__assert2", bionic___assert2),
    TL_WRAP("android_set_abort_message", bionic_android_set_abort_message),
    TL_WRAP("android_fdsan_create_owner_tag", bionic_android_fdsan_create_owner_tag),
    TL_WRAP("android_fdsan_exchange_owner_tag", bionic_android_fdsan_exchange_owner_tag),
    TL_WRAP("android_fdsan_get_owner_tag", bionic_android_fdsan_get_owner_tag),
    TL_WRAP("android_fdsan_get_tag_type", bionic_android_fdsan_get_tag_type),
    TL_WRAP("android_fdsan_get_tag_value", bionic_android_fdsan_get_tag_value),
    TL_WRAP("android_fdsan_close_with_tag", bionic_android_fdsan_close_with_tag),
    TL_WRAP("android_fdsan_get_error_level", bionic_android_fdsan_get_error_level),
    TL_WRAP("android_fdsan_set_error_level", bionic_android_fdsan_set_error_level),
    TL_WRAP("android_fdsan_set_error_level_from_property", bionic_android_fdsan_set_error_level_from_property),
    TL_WRAP("tcdrain", bionic_tcdrain), TL_WRAP("tcsendbreak", bionic_tcsendbreak), TL_WRAP("munlock", bionic_munlock),
    TL_WRAP("__libc_init", bionic___libc_init),
    TL_WRAP("__register_atfork", bionic___register_atfork),
    TL_WRAP("pthread_atfork", bionic_pthread_atfork),
    TL_WRAP("__cxa_atexit", bionic___cxa_atexit),
    TL_WRAP("__cxa_finalize", bionic___cxa_finalize),
    TL_WRAP("getauxval", bionic_getauxval),
    TL_WRAP("getpagesize", bionic_getpagesize),
    TL_WRAP("sysconf", bionic_sysconf), TL_WRAP("sysinfo", bionic_sysinfo),
    TL_WRAP("uname", bionic_uname),
    TL_WRAP("gethostname", bionic_gethostname),
    TL_WRAP("gettid", bionic_gettid),
    TL_DIRECT(getpid), TL_DIRECT(getppid), TL_DIRECT(getuid), TL_DIRECT(geteuid), TL_DIRECT(getegid),
    TL_WRAP("getpriority", bionic_getpriority),
    TL_WRAP("setpriority", bionic_setpriority),
    TL_WRAP("sched_getaffinity", bionic_sched_getaffinity), TL_WRAP("__sched_cpucount", bionic_sched_cpucount),
    TL_WRAP("sched_setaffinity", bionic_sched_setaffinity),
    TL_WRAP("sched_getparam", bionic_sched_getparam),
    TL_WRAP("sched_getscheduler", bionic_sched_getscheduler),
    TL_DIRECT(sched_yield),
    TL_WRAP("prctl", bionic_prctl),
    TL_WRAP("syscall", bionic_syscall),
    TL_WRAP("fork", bionic_fork),
    TL_WRAP("vfork", bionic_fork),
    TL_WRAP("posix_spawn", bionic_posix_spawn),
    TL_WRAP("posix_spawnp", bionic_posix_spawn),
    TL_WRAP("execv", bionic_execv),
    TL_WRAP("execve", bionic_execve),
    TL_WRAP("waitpid", bionic_waitpid),
    TL_WRAP("ptrace", bionic_ptrace),
    TL_WRAP("getpwuid", bionic_getpwuid),
    TL_WRAP("getpwuid_r", bionic_getpwuid_r),
    TL_WRAP("dlopen", bionic_dlopen),
    TL_WRAP("dlsym", bionic_dlsym),
    TL_WRAP("dlclose", bionic_dlclose),
    TL_WRAP("dlerror", bionic_dlerror),
    TL_WRAP("dladdr", bionic_dladdr),
    TL_WRAP("dl_iterate_phdr", bionic_dl_iterate_phdr),
    /* libdl's implementation entry points, which Facebook's loader hooks look up by name to call with a caller address */
    TL_WRAP("android_dlopen_ext", bionic_android_dlopen_ext), TL_WRAP("__loader_dlopen", bionic_loader_dlopen), TL_WRAP("__loader_android_dlopen_ext", bionic_loader_android_dlopen_ext),
    TL_WRAP("__loader_dlsym", bionic_loader_dlsym), TL_WRAP("__loader_dlvsym", bionic_loader_dlvsym),
    TL_WRAP("__loader_dlclose", bionic_dlclose), TL_WRAP("__loader_dlerror", bionic_dlerror), TL_WRAP("__loader_dladdr", bionic_dladdr),
    TL_WRAP("__loader_dl_iterate_phdr", bionic_dl_iterate_phdr),
    TL_END
};

/* ----------------------------------------------------------------- lookup */

static const tl_bionic_entry *const k_tables[] = { tl_tab_core, tl_tab_str, tl_tab_io, tl_tab_io2, tl_tab_str2, tl_tab_net, tl_tab_pthread, tl_tab_ndk, tl_tab_egl, tl_tab_cxx, tl_tab_opensles, tl_tab_sys, tl_tab_ldbl, tl_tab_jvm };

typedef struct { const char *name; void *addr; } slot;
static slot *g_slots;
static size_t g_nslots;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static uint64_t fnv(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; } return h; }

static void build(void)
{
    size_t total = 0;
    for (size_t t = 0; t < sizeof(k_tables) / sizeof(k_tables[0]); t++) for (const tl_bionic_entry *e = k_tables[t]; e->name; e++) total++;
    g_nslots = 1;
    while (g_nslots < total * 2) g_nslots <<= 1;
    g_slots = calloc(g_nslots, sizeof(slot));
    (void)g_environ_slot; (void)tl_environ_ptr;
    for (size_t t = 0; t < sizeof(k_tables) / sizeof(k_tables[0]); t++) {
        for (const tl_bionic_entry *e = k_tables[t]; e->name; e++) {
            size_t i = fnv(e->name) & (g_nslots - 1);
            while (g_slots[i].name) {
                if (!strcmp(g_slots[i].name, e->name)) { tl_log_line("bionic: duplicate table entry %s", e->name); break; }
                i = (i + 1) & (g_nslots - 1);
            }
            g_slots[i].name = e->name;
            g_slots[i].addr = e->addr;
        }
    }
}

void *tl_bionic_find(const char *name)
{
    pthread_once(&g_once, build);
    /* environ is a variable the guest reads whenever it walks the environment. The process may change it under the guest (setenv reallocates the array and frees the
     * old one), so the guest is given the address of the live variable, not of a copy made at start-up. */
    if (name[0] == 'e' && !strcmp(name, "environ")) return _NSGetEnviron();
    size_t i = fnv(name) & (g_nslots - 1);
    while (g_slots[i].name) {
        if (!strcmp(g_slots[i].name, name)) return g_slots[i].addr;
        i = (i + 1) & (g_nslots - 1);
    }
    /* OpenGL ES entry points are not in a table: they come from ANGLE, by name. */
    if (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') return tl_egl_resolve(name);
    if (!strncmp(name, "egl", 3)) return tl_egl_resolve(name);
    /* Vulkan is MoltenVK's, the same way. */
    if (name[0] == 'v' && name[1] == 'k' && name[2] >= 'A' && name[2] <= 'Z') return tl_vk_resolve(name);
    return NULL;
}

bool tl_bionic_is_system_lib(const char *soname)
{
    static const char *const sys[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libEGL.so", "libGLESv1_CM.so",
        "libGLESv2.so", "libGLESv3.so", "libz.so", "libmediandk.so", "libOpenSLES.so", "libaaudio.so",
        "libvulkan.so", "libvulkan.so.1", "libnativewindow.so", "libjnigraphics.so", "libcamera2ndk.so", "libstdc++.so",
        /* the rest of the NDK's libraries an app may list as needed without calling much in them (Roblox needs OpenMAX AL) */
        "libOpenMAXAL.so", "libneuralnetworks.so", "libbinder_ndk.so", "libsync.so", "libamidi.so",
        /* the Java runtime's: answered by Husk (husk-tl-dvm-jvm.c), never loaded */ "libopenjdkjvm.so", "libdl_android.so", "libart.so", NULL };
    for (int i = 0; sys[i]; i++) if (!strcmp(sys[i], soname)) return true;
    return false;
}
