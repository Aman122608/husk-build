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
static inline void tl_hp_bump(int which)
{
    if (which >= 0 && which < TL_HP_N)
        __atomic_fetch_add(&tl_hp_count[which], 1ul, __ATOMIC_RELAXED);
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
 * census at a detected stall, so a stalled app's own lock deadlock is named even when the mutex trace armed after the wait
 * began. Deduped by the guest mutex object (one entry per held mutex, the first live waiter reported for it). holder_tid is
 * 0 when the holder was never recorded (a lock taken before the holder bytes were written). */
typedef struct {
    unsigned long mutex, waiter_tid, waiter_entry, waiter_site, holder_tid, holder_site;
} tl_guest_mutex_wait;
int tl_guest_mutex_waits(tl_guest_mutex_wait *out, int max);   /* live count (may exceed max); fills up to max */

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_BIONIC_H */
