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

/* How many guest threads the pthread shim has started and that have not returned (for a session close to report
 * what it could not stop; there is no registry to enumerate or suspend them). */
int tl_guest_thread_live_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_BIONIC_H */
