/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A bounded trace of a Unity/il2cpp player's OWN startup: how far the engine gets before the first
 * frame, one line per milestone, once.
 *
 * Why it exists: the investigation of the "no first frame" stall spent its whole budget on the
 * signal path and had only SAMPLED the app's own startup (il2cpp metadata and class initialisation,
 * GC initialisation and the first stop-the-world, the player loop and render thread, the first
 * swap, the asset/worker threads). An external reviewer ranked "the app's own startup never
 * completes" as a live alternative to the signal hypothesis, and that window was never traced.
 * These breadcrumbs make it visible and are useful however the signal question resolves.
 *
 * How it is placed: the sites are addresses inside the APK's own libil2cpp.so / libunity.so, found
 * by disassembling them (capstone) -- an exported entry point, the instruction after a call, and the
 * call site of a marker call. The loader's post-relocation hook (husk-tl-ld.c) arms them before the
 * engine's own code can reach them. Each site is checked against the instruction the APK's .so
 * actually holds before it is patched; a site that does not match is reported and left alone.
 *
 * Cost: every site's instruction is replaced by a branch into a 64-byte stub that saves the
 * registers, calls a handler, re-executes the original instruction and branches back. The handler's
 * fast path is one relaxed atomic load, so a site that is hit again pays ~100 ns and prints
 * nothing. The sites chosen are one-shot engine entry points, plus two per-stop-the-world sites
 * (the collector's own signal send and its wait loop) which are hit once per collection, never per
 * frame. The whole trace is capped at ST_CAP lines.
 *
 * No line is printed and no lock is taken unless a milestone has actually been reached.
 */
#ifndef HUSK_TL_STARTUP_TRACE_H
#define HUSK_TL_STARTUP_TRACE_H

#include "husk-tl-ld.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Arm the trace: register the loader's post-relocation hook so the breadcrumbs are in place before
 * the engine's own code can reach them. Call before the app's first library loads. Idempotent. */
void tl_startup_trace_arm(void);

/* The loader's hook: `lib` is mapped and relocated, its constructors have not run yet. */
void tl_startup_trace_on_load(tl_lib *lib);

/* The engine driver's own two milestones. phase 0: the render thread is about to call nativeRender
 * for the first time; phase 1: that first call returned, after `ns` nanoseconds. */
void tl_startup_trace_render(int phase, unsigned long long ns);

/* One line at the end of a session naming the milestones that were never reached. Idempotent. */
void tl_startup_trace_report(void);

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_STARTUP_TRACE_H */
