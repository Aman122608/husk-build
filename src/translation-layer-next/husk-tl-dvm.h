/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Husk's Dalvik runtime: an interpreter for DEX bytecode, so apps (and the Java side of engines) whose code is Java can run.
 *
 * Android's own runtime, ART, cannot run on Apple arm64: it keeps every object in the low 4 GB (32-bit references), and xnu never
 * lets a process map there. This runtime keeps ordinary 64-bit pointers instead. Its objects are the JNI world's jobj
 * (husk-tl-jni.c), so native code -- an engine's .so, libcore's own natives -- sees the same objects through JNI that the
 * interpreter works on, and the Android framework classes Husk implements in C (Activity, Context, Looper...) are reached the
 * same way the engines reach them.
 *
 * The boot class path is the real one: libcore (core-oj.jar, core-libart.jar) from the ART APEX, its bytecode interpreted, its
 * native methods called in libjavacore.so / libopenjdk.so through Husk's linker. What ART implements inside itself rather than in
 * those libraries -- Object, Class, String's natives, Thread, Unsafe, VMRuntime, reflection -- is in husk-tl-dvm-natives.c.
 */
#ifndef HUSK_TL_DVM_H
#define HUSK_TL_DVM_H

#include <stdbool.h>
#include <stdint.h>

#include "husk-tl-jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the runtime with these boot class path entries (.jar/.apk/.dex files, searched in order). Call after tl_jni_init. */
bool tl_dvm_start(const char *const *boot, int nboot);
/* Add an app's DEX files (every classesN.dex in the APK), after the boot class path. */
bool tl_dvm_add_apk(const char *apk);
bool tl_dvm_running(void);

/* Close the current app's session, so a different app can define its own classes in this process: truncate the app's dexes
 * (back to the boot class path's boundary), unlink their entries from the class index, and detach every tl_jclass that pointed
 * into one of them (dvm=NULL, the method/field caches emptied, statics zeroed) -- and clear the app's class loader and the
 * run-time DexFile cookies. Boot classes, the boot dexes and the interpreter itself stay up. Nothing is unmapped or freed
 * (the dex bytes stay resident): drain, never free. Call it only with no guest thread running. Returns false when the
 * runtime is not up or there was nothing to drop. */
bool tl_dvm_end_app(void);

/* How many dexes the app owns right now (0 when none): the verification step after tl_dvm_end_app(). */
int tl_dvm_app_dex_count(void);

/* True when this class's bytecode came from a dex the app added (so a second app must not run it). */
bool tl_dvm_class_is_app(tl_jclass *jc);

/* Re-attach the declared classes a newly added APK now defines (call after tl_dvm_add_apk for a new app): a class that was
 * detached by tl_dvm_end_app() comes back against the new APK's dexes, and one no APK defines stays detached. */
void tl_dvm_reattach_declared(void);

/* libcore's own native libraries (libjavacore.so, libopenjdk.so and what they need), from this folder (the ART APEX's lib64). */
bool tl_dvm_load_natives(const char *libdir);

/* Call a static method by name: what a driver does to start the app's code. Returns false when it threw (the exception is pending). */
bool tl_dvm_call_static(const char *cls, const char *name, const char *sig, const jvalue *args, jvalue *ret);

/* Make a string from UTF-16 / read a string as UTF-16 (made and kept on first use). */
jobj *tl_dvm_string_u16(const uint16_t *s, int32_t n);
const uint16_t *tl_dvm_string_chars(jobj *s, int32_t *n);

/* The exception pending on this thread as text ("java.lang.Foo: message"), for logs; NULL when there is none. */
const char *tl_dvm_describe_pending(char *buf, size_t n);

/* ---- Naming a Java thread's STACK from OUTSIDE it (the cold stall probe) ----
 *
 * The interpreter keeps a per-thread Java frame chain (the stack Reflection and stack traces read). It is thread-local,
 * so only the running thread can read its own -- which is why a stall's frame-POINTER walk inside the interpreter can
 * name only the interpreter's C functions (dvm_call/invoke_regs/dvm_call_inner) and never the Java method running inside
 * them. A thread that runs Java registers the addresses of its own frame stack here once, at its ENTRY (so nothing is
 * added to any running path), and dvm_thread_java_frames() then lets the cold stall path read that thread's Java frames
 * WITHOUT suspending, joining or signalling it. Unregister on the way out; reset clears every slot for a dead session. */
void dvm_thread_register_frames(void);
void dvm_thread_unregister_frames(void);
void dvm_thread_frames_reset(void);
/* One Java frame as the probe reads it: class name ("java/lang/String"), method name, descriptor ("(...)R"), the pc at
 * the frame's last call out, and the source line (-1 without debug info). */
typedef struct { const char *cls; const char *name; const char *sig; uint32_t pc; int line; } dvm_java_frame;
/* The thread `tid`'s Java frames, INNERMOST FIRST, up to `max` into `out`. Returns the count, or -1 when that thread
 * registered no Java frame stack (it does not run through the interpreter) -- the honest answer, never a frame invented. */
int dvm_thread_java_frames(unsigned long tid, dvm_java_frame *out, int max);

void tl_dvm_set_trace(int level);       /* 1: classes and natives; 2: every invoke */

#ifdef __cplusplus
}
#endif

#endif
