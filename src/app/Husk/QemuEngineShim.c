/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The QEMU engine's front door for the app -- and what the app says when the
 * engine is not there.
 *
 * ---------------------------------------------------------------------------
 * Why this file exists
 * ---------------------------------------------------------------------------
 * The app used to link libqemu-aarch64-softmmu.dylib strongly, which put a
 * 33.9 MiB (6.7 MiB packed) library in the bundle for two different reasons:
 * the emulator itself, AND the JIT substrate, which was compiled into it
 * (scripts/integrate_husk.sh). Removing the library therefore removed the JIT
 * as a side effect of removing the emulator -- which is the reason it survived
 * the earlier unbundle.
 *
 * That split is undone now, deliberately. The JIT substrate's own home is
 * src/ios-jit/husk-ios-jit.c, and it is compiled into the APP binary, where the
 * app and the native translation layer both find it (the layer dlsym()s it by
 * name: src/translation-layer/husk-tl-load.c). The emulator is no longer linked,
 * no longer embedded and no longer shipped.
 *
 * What is left is the app's own reference to the emulator: qemu_init /
 * qemu_main_loop / qemu_cleanup, and the display, audio, snapshot and balloon
 * bridge that lived beside them. The declarations are still in HuskBridge.h --
 * Swift still calls those names -- so THIS FILE PROVIDES THE DEFINITIONS, each
 * one a forwarder that resolves the engine's implementation when the engine is
 * present and reports honestly, by name, when it is not. Call it a no-op with a
 * receipt: nothing is silent, and nothing is a dangling lookup that fails
 * without saying so.
 *
 * ---------------------------------------------------------------------------
 * Who decides, and what the log says
 * ---------------------------------------------------------------------------
 * The engine is looked for once, lazily, the first time any of these is called:
 * dlopen() of @rpath/libqemu-aarch64-softmmu.dylib (and the two other places a
 * built one is ever put). A build that wants the guest bundles the library back
 * beside the app and this finds it; the shipped build does not have it, so every
 * forwarder takes the "no engine" branch. The first such call logs one line
 * naming the library and the plan document, and the engine-selection path --
 * QemuRunner.run() / ContentView.start() -- refuses to start a guest with the
 * reason in words rather than discovering it inside qemu_init().
 *
 * The lookup goes through the dlopen HANDLE, never RTLD_DEFAULT: this file
 * defines these very names in the app binary, so RTLD_DEFAULT would find the
 * forwarder and recurse forever.
 *
 * A note for anyone who re-enables the engine: the JIT is now the APP's, so the
 * engine's own copy of it -- compiled into the dylib by integrate_husk.sh --
 * keeps its own husk_prewarmed state and is no longer the one the prewarm at
 * launch fills. The engine would claim a second region at qemu_init() rather
 * than the region the arena adopted. It works (the debugger's grant is held for
 * the run), it is just not the single region the design intends. That, and not
 * size, is the reason the engine is meant to stay out.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The same struct tag as HuskBridge.h, so this is the same type without this
   file having to pull in the app's whole bridging header (which includes the
   translation layer's). Only the pointer crosses this boundary. */
typedef struct HuskFrameInfo HuskFrameInfo;

/* Mirrors HuskBridge.h's husk_metal_present_fn. */
typedef void (*husk_metal_present_fn)(void *texture, int32_t flip,
                                      int32_t width, int32_t height);

static void *g_engine;
static int   g_engine_probed;
static int   g_engine_absent_reported;

/*
 * The engine, loaded once on first use, or NULL.
 *
 * @rpath first: that is where an embedded copy resolves to from the app
 * (@executable_path/Frameworks, see LD_RUNPATH_SEARCH_PATHS). The bare name last
 * covers a copy placed where dyld's own search finds it.
 */
static void *husk_engine(void)
{
    if (g_engine_probed) {
        return g_engine;
    }
    g_engine_probed = 1;

    static const char *const candidates[] = {
        "@rpath/libqemu-aarch64-softmmu.dylib",
        "@executable_path/Frameworks/libqemu-aarch64-softmmu.dylib",
        "libqemu-aarch64-softmmu.dylib",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        g_engine = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (g_engine != NULL) {
            fprintf(stderr, "[husk-engine] QEMU engine loaded from %s\n", candidates[i]);
            return g_engine;
        }
    }
    fprintf(stderr, "[husk-engine] no QEMU engine in this app bundle: this build ships "
                    "without libqemu-aarch64-softmmu.dylib, so the Android guest cannot be "
                    "started here. The JIT substrate and the native translation layer do not "
                    "use it and are unaffected. See "
                    ".hermes/plans/engine-reset/qemu-dylib-removal.md\n");
    return NULL;
}

/* One of the engine's own symbols, through the handle -- never RTLD_DEFAULT. */
static void *husk_engine_sym(const char *name)
{
    void *h = husk_engine();
    return h != NULL ? dlsym(h, name) : NULL;
}

/* Say it once per process, not once per call: some of these are on hot paths
   (the audio render thread, every touch). */
static void husk_engine_absent(const char *what)
{
    if (__atomic_exchange_n(&g_engine_absent_reported, 1, __ATOMIC_ACQ_REL) == 0) {
        fprintf(stderr, "[husk-engine] %s reached with no engine loaded; taken as a no-op. "
                        "Nothing it would have driven is running.\n", what);
    }
}

/* -------------------------------------------------- engine selection / lifecycle */

/*
 * Whether this process has the QEMU engine. The app's one question, asked before
 * it offers to start a guest: GuestRuntime answers "is the guest payload here",
 * and this answers "is the thing that would run it".
 */
bool husk_qemu_engine_present(void)
{
    return husk_engine() != NULL;
}

void qemu_init(int argc, char **argv)
{
    void (*fn)(int, char **) = (void (*)(int, char **))husk_engine_sym("qemu_init");
    if (fn == NULL) { husk_engine_absent("qemu_init"); return; }
    fn(argc, argv);
}

void qemu_main_loop(void)
{
    void (*fn)(void) = (void (*)(void))husk_engine_sym("qemu_main_loop");
    if (fn == NULL) { husk_engine_absent("qemu_main_loop"); return; }
    fn();
}

void qemu_cleanup(void)
{
    void (*fn)(void) = (void (*)(void))husk_engine_sym("qemu_cleanup");
    if (fn == NULL) { husk_engine_absent("qemu_cleanup"); return; }
    fn();
}

/* ---------------------------------------------------------------- display */

void husk_display_init(void)
{
    void (*fn)(void) = (void (*)(void))husk_engine_sym("husk_display_init");
    if (fn == NULL) { husk_engine_absent("husk_display_init"); return; }
    fn();
}

bool husk_display_lock_frame(HuskFrameInfo *out)
{
    bool (*fn)(HuskFrameInfo *) = (bool (*)(HuskFrameInfo *))husk_engine_sym("husk_display_lock_frame");
    if (fn == NULL) { husk_engine_absent("husk_display_lock_frame"); return false; }
    return fn(out);
}

void husk_display_unlock_frame(void)
{
    void (*fn)(void) = (void (*)(void))husk_engine_sym("husk_display_unlock_frame");
    if (fn == NULL) { husk_engine_absent("husk_display_unlock_frame"); return; }
    fn();
}

uint64_t husk_display_sequence(void)
{
    uint64_t (*fn)(void) = (uint64_t (*)(void))husk_engine_sym("husk_display_sequence");
    if (fn == NULL) { husk_engine_absent("husk_display_sequence"); return 0; }
    return fn();
}

void husk_display_send_pointer(int32_t x, int32_t y, bool button_down)
{
    void (*fn)(int32_t, int32_t, bool) =
        (void (*)(int32_t, int32_t, bool))husk_engine_sym("husk_display_send_pointer");
    if (fn == NULL) { husk_engine_absent("husk_display_send_pointer"); return; }
    fn(x, y, button_down);
}

bool husk_display_send_key(const char *qcode_name, bool down)
{
    bool (*fn)(const char *, bool) =
        (bool (*)(const char *, bool))husk_engine_sym("husk_display_send_key");
    if (fn == NULL) { husk_engine_absent("husk_display_send_key"); return false; }
    return fn(qcode_name, down);
}

void husk_display_request_update(void)
{
    void (*fn)(void) = (void (*)(void))husk_engine_sym("husk_display_request_update");
    if (fn == NULL) { husk_engine_absent("husk_display_request_update"); return; }
    fn();
}

void husk_display_set_ui_size(int32_t width, int32_t height)
{
    void (*fn)(int32_t, int32_t) =
        (void (*)(int32_t, int32_t))husk_engine_sym("husk_display_set_ui_size");
    if (fn == NULL) { husk_engine_absent("husk_display_set_ui_size"); return; }
    fn(width, height);
}

void husk_display_guest_size(int32_t *width, int32_t *height)
{
    void (*fn)(int32_t *, int32_t *) =
        (void (*)(int32_t *, int32_t *))husk_engine_sym("husk_display_guest_size");
    if (fn == NULL) {
        husk_engine_absent("husk_display_guest_size");
        if (width != NULL)  { *width  = 0; }
        if (height != NULL) { *height = 0; }
        return;
    }
    fn(width, height);
}

/* ------------------------------------------------------------- GL display */

bool husk_display_gl_early(void)
{
    bool (*fn)(void) = (bool (*)(void))husk_engine_sym("husk_display_gl_early");
    if (fn == NULL) { husk_engine_absent("husk_display_gl_early"); return false; }
    return fn();
}

bool husk_display_gl_create(void *native_layer, int32_t width, int32_t height)
{
    bool (*fn)(void *, int32_t, int32_t) =
        (bool (*)(void *, int32_t, int32_t))husk_engine_sym("husk_display_gl_create");
    if (fn == NULL) { husk_engine_absent("husk_display_gl_create"); return false; }
    return fn(native_layer, width, height);
}

bool husk_display_gl_probe(void)
{
    bool (*fn)(void) = (bool (*)(void))husk_engine_sym("husk_display_gl_probe");
    if (fn == NULL) { husk_engine_absent("husk_display_gl_probe"); return false; }
    return fn();
}

bool husk_display_gl_bind(void)
{
    bool (*fn)(void) = (bool (*)(void))husk_engine_sym("husk_display_gl_bind");
    if (fn == NULL) { husk_engine_absent("husk_display_gl_bind"); return false; }
    return fn();
}

uint64_t husk_display_gl_frames(void)
{
    uint64_t (*fn)(void) = (uint64_t (*)(void))husk_engine_sym("husk_display_gl_frames");
    if (fn == NULL) { husk_engine_absent("husk_display_gl_frames"); return 0; }
    return fn();
}

void husk_display_gl_set_metal_presenter(husk_metal_present_fn fn)
{
    void (*real)(husk_metal_present_fn) =
        (void (*)(husk_metal_present_fn))husk_engine_sym("husk_display_gl_set_metal_presenter");
    if (real == NULL) { husk_engine_absent("husk_display_gl_set_metal_presenter"); return; }
    real(fn);
}

/* ------------------------------------------------------------------ audio */

/*
 * The render callback calls this on Core Audio's own real-time thread, and it
 * runs whether or not the guest is up (HuskAudio starts with the guest). With no
 * engine there is no ring to pull from, so this writes the silence the engine's
 * own shortfall path would have written -- an uninitialised buffer would be
 * noise, and noise is worse than nothing.
 */
int32_t husk_audio_pull(int16_t *dst, int32_t frames)
{
    int32_t (*fn)(int16_t *, int32_t) =
        (int32_t (*)(int16_t *, int32_t))husk_engine_sym("husk_audio_pull");
    if (fn == NULL) {
        husk_engine_absent("husk_audio_pull");
        if (dst != NULL && frames > 0) {
            memset(dst, 0, (size_t)frames * 2u * sizeof(int16_t));
        }
        return 0;
    }
    return fn(dst, frames);
}

bool husk_audio_active(void)
{
    bool (*fn)(void) = (bool (*)(void))husk_engine_sym("husk_audio_active");
    if (fn == NULL) { husk_engine_absent("husk_audio_active"); return false; }
    return fn();
}

uint64_t husk_audio_frames_in(void)
{
    uint64_t (*fn)(void) = (uint64_t (*)(void))husk_engine_sym("husk_audio_frames_in");
    if (fn == NULL) { husk_engine_absent("husk_audio_frames_in"); return 0; }
    return fn();
}

uint64_t husk_audio_underruns(void)
{
    uint64_t (*fn)(void) = (uint64_t (*)(void))husk_engine_sym("husk_audio_underruns");
    if (fn == NULL) { husk_engine_absent("husk_audio_underruns"); return 0; }
    return fn();
}

/* -------------------------------------------------------------- snapshots */

/*
 * "Nothing to restore" is the engine's own answer for a normal first run, so
 * false here needs no explanation beyond the one line the absent-report prints.
 */
bool husk_snapshot_load_at_startup(void)
{
    bool (*fn)(void) = (bool (*)(void))husk_engine_sym("husk_snapshot_load_at_startup");
    if (fn == NULL) { husk_engine_absent("husk_snapshot_load_at_startup"); return false; }
    return fn();
}

/*
 * A save is asked for, and answered -- never left hanging. The engine's contract
 * is that the callback always fires, so this fires it with the failure and the
 * reason rather than dropping it on the floor; the caller logs "FAILED" and
 * stops waiting.
 */
void husk_snapshot_save(void (*cb)(bool ok, const char *what))
{
    void (*fn)(void (*)(bool, const char *)) =
        (void (*)(void (*)(bool, const char *)))husk_engine_sym("husk_snapshot_save");
    if (fn == NULL) {
        husk_engine_absent("husk_snapshot_save");
        if (cb != NULL) { cb(false, "no QEMU engine in this build"); }
        return;
    }
    fn(cb);
}

/* --------------------------------------------------------------- balloon */

void husk_balloon_set_bytes(int64_t target_bytes)
{
    void (*fn)(int64_t) = (void (*)(int64_t))husk_engine_sym("husk_balloon_set_bytes");
    if (fn == NULL) { husk_engine_absent("husk_balloon_set_bytes"); return; }
    fn(target_bytes);
}
