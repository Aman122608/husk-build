/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-egl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/vm_map.h>
#endif

#include "husk-tl-bionic.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"

void *tl_nwindow_native(void *window);
int tl_nwindow_width(void *window);
void *tl_nwindow_texture(void *window);
void tl_nwindow_post(void *window, const void *rgba, int w, int h, bool flip);
int tl_nwindow_height(void *window);

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig, *EGLNativeWindowType;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_FALSE 0
#define EGL_TRUE 1

static struct {
    char egl_path[600], gles_path[600];
    void *egl, *gles;
    bool ready;
    char frame_dir[512];
    int frame_every;
    bool latest_only;           /* overwrite latest.bmp instead of keeping every frame */
    atomic_ulong presented;
    atomic_ulong composited;    /* frames handed to the runtime's on-screen compositor (the visible surface) */
    atomic_ulong comp_made;     /* composited WINDOW surfaces handed to an app (created, whether or not it ever swapped) */
    atomic_ulong dropped;       /* swaps refused because their surface's session had ended */
    EGLDisplay display;
    pthread_mutex_t lock;
} E = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ANGLE's entry points */
static void *(*a_eglGetProcAddress)(const char *);
static EGLDisplay (*a_eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
static EGLBoolean (*a_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*a_eglTerminate)(EGLDisplay);
static const char *(*a_eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean (*a_eglGetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*a_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*a_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLContext (*a_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*a_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLSurface (*a_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLSurface (*a_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLBoolean (*a_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*a_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
static EGLBoolean (*a_eglSurfaceAttrib)(EGLDisplay, EGLSurface, EGLint, EGLint);
static EGLBoolean (*a_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*a_eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean (*a_eglSwapInterval)(EGLDisplay, EGLint);
static EGLContext (*a_eglGetCurrentContext)(void);
static EGLSurface (*a_eglGetCurrentSurface)(EGLint);
static EGLDisplay (*a_eglGetCurrentDisplay)(void);
static EGLBoolean (*a_eglBindAPI)(EGLenum);
static EGLenum (*a_eglQueryAPI)(void);
static EGLint (*a_eglGetError)(void);
static EGLBoolean (*a_eglWaitGL)(void);
static EGLBoolean (*a_eglWaitNative)(EGLint);
static EGLBoolean (*a_eglReleaseThread)(void);
static void (*a_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
static void (*a_glGetIntegerv)(unsigned, int *);
static void (*a_glBindFramebuffer)(unsigned, unsigned);
static void (*a_glPixelStorei)(unsigned, int);

#define LOAD(lib, name) do { a_##name = dlsym(lib, #name); if (!a_##name) tl_log_line("egl: ANGLE does not export " #name); } while (0)

bool tl_egl_init(const char *egl_path, const char *gles_path, const char *frame_dir, int frame_every)
{
    pthread_mutex_lock(&E.lock);
    if (E.ready) { pthread_mutex_unlock(&E.lock); return true; }
    snprintf(E.egl_path, sizeof(E.egl_path), "%s", egl_path);
    snprintf(E.gles_path, sizeof(E.gles_path), "%s", gles_path ? gles_path : egl_path);
    E.egl = dlopen(egl_path, RTLD_NOW | RTLD_LOCAL);
    if (!E.egl) { tl_log_line("egl: cannot load %s: %s", egl_path, dlerror()); pthread_mutex_unlock(&E.lock); return false; }
    E.gles = gles_path ? dlopen(gles_path, RTLD_NOW | RTLD_LOCAL) : E.egl;
    if (!E.gles) { tl_log_line("egl: cannot load %s: %s", gles_path, dlerror()); pthread_mutex_unlock(&E.lock); return false; }
    void *g = E.egl;
    LOAD(g, eglGetProcAddress); LOAD(g, eglInitialize); LOAD(g, eglTerminate); LOAD(g, eglQueryString); LOAD(g, eglGetConfigs);
    LOAD(g, eglChooseConfig); LOAD(g, eglGetConfigAttrib); LOAD(g, eglCreateContext); LOAD(g, eglDestroyContext);
    LOAD(g, eglCreateWindowSurface); LOAD(g, eglCreatePbufferSurface); LOAD(g, eglDestroySurface); LOAD(g, eglQuerySurface);
    LOAD(g, eglSurfaceAttrib); LOAD(g, eglMakeCurrent); LOAD(g, eglSwapBuffers); LOAD(g, eglSwapInterval);
    LOAD(g, eglGetCurrentContext); LOAD(g, eglGetCurrentSurface); LOAD(g, eglGetCurrentDisplay); LOAD(g, eglBindAPI);
    LOAD(g, eglQueryAPI); LOAD(g, eglGetError); LOAD(g, eglWaitGL); LOAD(g, eglWaitNative); LOAD(g, eglReleaseThread);
    a_eglGetPlatformDisplayEXT = a_eglGetProcAddress ? a_eglGetProcAddress("eglGetPlatformDisplayEXT") : NULL;
    if (!a_eglGetPlatformDisplayEXT) { tl_log_line("egl: ANGLE has no eglGetPlatformDisplayEXT"); pthread_mutex_unlock(&E.lock); return false; }
    /* GLES functions this layer itself calls (frame capture) */
    a_glReadPixels = a_eglGetProcAddress("glReadPixels");
    a_glGetIntegerv = a_eglGetProcAddress("glGetIntegerv");
    a_glBindFramebuffer = a_eglGetProcAddress("glBindFramebuffer");
    a_glPixelStorei = a_eglGetProcAddress("glPixelStorei");
    if (frame_dir) { snprintf(E.frame_dir, sizeof(E.frame_dir), "%s", frame_dir); E.latest_only = frame_every < 0; E.frame_every = frame_every != 0 ? (frame_every < 0 ? -frame_every : frame_every) : 60; }
    E.ready = true;
    pthread_mutex_unlock(&E.lock);
    tl_log_line("egl: ANGLE loaded%s%s", frame_dir ? ", off-screen, frames to " : "", frame_dir ? frame_dir : "");
    return true;
}

unsigned long tl_egl_frames_presented(void) { return atomic_load(&E.presented); }
/* What a session is actually COMPOSITING to the screen, and how many of its frames were refused because they came from
 * a session that had already ended. Both monotonic over the process; the session's own reading is a difference from a
 * mark taken when the session started (husk-tl-unity-app.c: live_frames_mark / live_screen). */
unsigned long tl_egl_composited_frames(void) { return atomic_load(&E.composited); }
unsigned long tl_egl_dropped_frames(void) { return atomic_load(&E.dropped); }
/* How many WINDOW surfaces this layer has handed an app on the composited path (kind 2) over the process's life. Paired
 * with tl_egl_composited_frames this is the one-line answer to "did the app ever take its own EGL surface and draw into
 * it": created > 0 with swapped == 0 means the app was given a surface and never swapped it, which is a different
 * failure from the app never asking for one, and it is the shape a Unity title shows when it never finishes loading. */
unsigned long tl_egl_composited_surfaces(void) { return atomic_load(&E.comp_made); }

/* ------------------------------------------------------------ EGL wrappers */

static EGLDisplay w_eglGetDisplay(void *native)
{
    (void)native;
    pthread_mutex_lock(&E.lock);
    if (!E.display && E.ready) {
        const EGLint attribs[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
        E.display = a_eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, attribs);
        tl_log_line("egl: display %p (ANGLE over Metal)", E.display);
    }
    EGLDisplay d = E.display;
    pthread_mutex_unlock(&E.lock);
    return d;
}

#define PASS_BOOL(name, params, args) static EGLBoolean w_##name params { return a_##name args; }
PASS_BOOL(eglInitialize, (EGLDisplay d, EGLint *a, EGLint *b), (d, a, b))
/* eglTerminate of the display everything here shares (the app's GL, Husk's own render thread): Android's counts initializations and
 * tears down at the last; here Husk holds one for as long as it runs, so a library terminating its own use (Castle's GPU probe in X)
 * leaves the display, and everyone else's surfaces, standing */
static EGLBoolean w_eglTerminate(EGLDisplay d)
{
    if (d && d == E.display) { if (getenv("TL_EGL_TRACE")) tl_log_line("egl: eglTerminate of the shared display: kept"); return EGL_TRUE; }
    return a_eglTerminate(d);
}
PASS_BOOL(eglGetConfigs, (EGLDisplay d, EGLConfig *c, EGLint n, EGLint *r), (d, c, n, r))
/* ANGLE's configs with depth and stencil all have alpha. Android's also come without (RGBX), and an engine that asks for no alpha
 * picks one of those (King's): such a request also gets each 8-bit RGBA config again as a "no alpha" alias -- its handle with the
 * low bit set, which reports alpha 0 and is the real config everywhere else. */
static inline EGLConfig cfg_real(EGLConfig c) { return (EGLConfig)((uintptr_t)c & ~(uintptr_t)1); }
static inline bool cfg_alias(EGLConfig c) { return ((uintptr_t)c & 1) != 0; }
static EGLBoolean w_eglChooseConfig(EGLDisplay d, const EGLint *at, EGLConfig *c, EGLint n, EGLint *r)
{
    /* Android-only attributes (EGL_RECORDABLE_ANDROID, EGL_FRAMEBUFFER_TARGET_ANDROID) match nothing in ANGLE: every
     * config would be refused over a property it cannot have. They are dropped from the request. */
    EGLint filtered[128]; int nf = 0;
    bool alpha_wanted = false;
    for (int i = 0; at && at[i] != EGL_NONE && nf < 124; i += 2) {
        if (at[i] == 0x3142 || at[i] == 0x3147) continue;
        if (at[i] == 0x3021 /* EGL_ALPHA_SIZE */ && at[i + 1] > 0) alpha_wanted = true;
        filtered[nf++] = at[i]; filtered[nf++] = at[i + 1];
    }
    filtered[nf] = EGL_NONE;
    static EGLConfig tmp[256];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    EGLint k = 0;
    EGLBoolean ok = a_eglChooseConfig(d, at ? filtered : NULL, tmp, 256, &k);
    EGLint total = 0;
    if (ok) {
        EGLConfig all[512];
        for (int i = 0; i < k; i++) all[total++] = tmp[i];
        static int no_alias = -1;
        if (no_alias < 0) no_alias = getenv("TL_EGL_NO_ALIAS") ? 1 : 0;
        for (int i = 0; i < k && !alpha_wanted && !no_alias && total < 512; i++) {
            EGLint a = 0, rr = 0, g = 0, b = 0;
            a_eglGetConfigAttrib(d, tmp[i], 0x3021, &a); a_eglGetConfigAttrib(d, tmp[i], 0x3024, &rr);
            a_eglGetConfigAttrib(d, tmp[i], 0x3023, &g); a_eglGetConfigAttrib(d, tmp[i], 0x3022, &b);
            if (a == 8 && rr == 8 && g == 8 && b == 8) all[total++] = (EGLConfig)((uintptr_t)tmp[i] | 1);
        }
        if (c) { EGLint m = total < n ? total : n; for (int i = 0; i < m; i++) c[i] = all[i]; total = m; }
        if (r) *r = total;
    }
    pthread_mutex_unlock(&mu);
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace || !ok || (r && *r == 0)) {
        char buf[512]; size_t kk = 0;
        for (int i = 0; at && at[i] != EGL_NONE && i < 60 && kk + 24 < sizeof(buf); i += 2) kk += (size_t)snprintf(buf + kk, sizeof(buf) - kk, " %#x=%d", at[i], at[i + 1]);
        tl_log_line("egl: eglChooseConfig(%s ) -> %s, %d config(s)", buf, ok ? "true" : "false", r ? *r : -1);
    }
    return ok;
}
static EGLBoolean w_eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint at, EGLint *v)
{
    if (at == 0x3142 /* EGL_RECORDABLE_ANDROID */) { if (v) *v = 1; return EGL_TRUE; }
    bool alias = cfg_alias(c);
    EGLBoolean ok = a_eglGetConfigAttrib(d, cfg_real(c), at, v);
    if (ok && alias && v) { if (at == 0x3021 /* ALPHA_SIZE */) *v = 0; else if (at == 0x3020 /* BUFFER_SIZE */) *v = 24; else if (at == 0x302E /* NATIVE_VISUAL_ID */) *v = 2; }
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace) tl_log_line("egl: eglGetConfigAttrib(%p, %#x) -> %s %d", c, at, ok ? "true" : "false", v ? *v : -1);
    return ok;
}
PASS_BOOL(eglDestroyContext, (EGLDisplay d, EGLContext c), (d, c))
PASS_BOOL(eglQuerySurface, (EGLDisplay d, EGLSurface s, EGLint at, EGLint *v), (d, s, at, v))
PASS_BOOL(eglSurfaceAttrib, (EGLDisplay d, EGLSurface s, EGLint at, EGLint v), (d, s, at, v))
static EGLBoolean w_eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{
    EGLBoolean ok = E.ready ? a_eglMakeCurrent(d, dr, rd, c) : EGL_FALSE;
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace || !ok) {
        char who[32] = ""; pthread_getname_np(pthread_self(), who, sizeof(who));
        const void *lr = __builtin_return_address(0); const char *lib = NULL; const void *base = NULL;
        const char *sym = tl_ld_symbol_at(lr, &lib, &base);
        tl_log_line("egl: eglMakeCurrent(draw %p, read %p, context %p) -> %s%s%#x  [thread '%s', called from %s %s+%#lx]", dr, rd, c, ok ? "true" : "false", ok ? "" : ", eglGetError ", ok ? 0 : (E.ready ? a_eglGetError() : 0x3001),
                    who, lib ? lib : "?", sym ? sym : "?", base ? (unsigned long)((const char *)lr - (const char *)base) : 0ul);
    }
    return ok;
}
PASS_BOOL(eglSwapInterval, (EGLDisplay d, EGLint i), (d, i))
PASS_BOOL(eglBindAPI, (EGLenum api), (api))
PASS_BOOL(eglWaitGL, (void), ())
PASS_BOOL(eglWaitNative, (EGLint e), (e))
PASS_BOOL(eglReleaseThread, (void), ())

static const char *w_eglQueryString(EGLDisplay d, EGLint name) { return a_eglQueryString(d, name); }
static bool g_es31_shim;           /* defined with the rest of the shim, below */

/*
 * How many ANGLE worker threads are alive in THIS process right now, by ANGLE's own naming of them
 * ('ANGLE-Worker'). A failed eglCreateContext is a rare, cold event, so it is a fair place to count them: if a refused
 * creation left a worker behind, the count climbs across the app's retries and the next log proves it; if it is flat,
 * the workers were made once (ANGLE's own pool) and no attempt leaks one. This shim STARTS no thread of its own --
 * eglCreateContext only forwards to ANGLE's entry point and returns its result -- so any worker here is ANGLE's, and
 * this line is the only way to see whether repeated failures accumulate them.
 * Reads thread NAMES only (pthread_getname_np): nothing is suspended, ended or joined. Returns -1 when the process's
 * threads cannot be listed, so a missing count is printed as such and never as 0.
 */
static int angle_worker_count(void)
{
#if defined(__APPLE__)
    thread_act_array_t list = NULL; mach_msg_type_number_t n = 0; int k = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS || !list) return -1;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        pthread_t pt = pthread_from_mach_thread_np(list[i]);
        char nm[32] = "";
        if (pt && pthread_getname_np(pt, nm, sizeof(nm)) == 0 && !strncmp(nm, "ANGLE-Worker", 12)) k++;
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, (vm_size_t)n * sizeof(thread_t));
    return k;
#else
    return -1;
#endif
}
static EGLContext w_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *at)
{
    /* A game that insists on ES 3.1 or 3.2 (Unreal Engine tries 3.2, then 3.1) is given the 3.0 context ANGLE can make: the version it is then told is the shim's. */
    EGLint patched[40];
    if (g_es31_shim && at) {
        int n = 0;
        for (; at[n] != EGL_NONE && n < 36; n += 2) {
            patched[n] = at[n]; patched[n + 1] = at[n + 1];
            if (at[n] == 0x30fb /* EGL_CONTEXT_MINOR_VERSION */ && at[n + 1] > 0) patched[n + 1] = 0;
        }
        patched[n] = EGL_NONE;
        at = patched;
    }
    EGLContext ctx = a_eglCreateContext(d, cfg_real(c), share, at);
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace || !ctx) {
        char buf[200]; size_t k = 0;
        for (int i = 0; at && at[i] != EGL_NONE && i < 20 && k + 24 < sizeof(buf); i += 2) k += (size_t)snprintf(buf + k, sizeof(buf) - k, " %#x=%d", at[i], at[i + 1]);
        if (ctx)
            tl_log_line("egl: eglCreateContext(share %p,%s ) -> %p", share, buf, ctx);
        else {
            /* A refusal is where the version question and the worker question are answered, once, cold: what was ASKED
             * (minor > 0 means the shim did NOT downgrade it -- the 3.1/3.2 downgrade is armed only for an app whose
             * manifest claims ES 3.1+, or an engine that asks through tl_egl_es31_shim) and how many ANGLE worker
             * threads exist at this instant (a count that climbs across consecutive refusals proves they accumulate;
             * an unchanged count says ANGLE made them once). This shim starts no thread of its own. */
            int minor = 0;
            for (int i = 0; at && at[i] != EGL_NONE && i < 20; i += 2) if (at[i] == 0x30fb /* EGL_CONTEXT_MINOR_VERSION */) minor = at[i + 1];
            int workers = angle_worker_count();
            tl_log_line("egl: eglCreateContext(share %p,%s ) -> NULL (failed): minor asked=%d%s; ANGLE-Worker threads alive now=%s",
                        share, buf, minor,
                        (minor > 0 && !g_es31_shim)
                            ? " and the shim passed it through unchanged (this app's manifest claims no ES 3.1+/3.2, so the downgrade is not armed) -- a later create at minor 0 is what gives the app its 3.0 context"
                            : " and the shim is in its 3.1/3.2-claiming mode, so this request was already the shim's downgraded one and ANGLE refused that too",
                        workers < 0 ? "not countable (task_threads failed) -- unknown, never assumed" : (workers ? "printed next (compare it across consecutive refusals)" : "0"));
            if (workers > 0)
                tl_log_line("egl:   ANGLE-Worker threads alive at this refusal: %d (if this number climbs across the app's retries, a refused creation is leaving a worker behind; if it is the same each time, ANGLE made them once)", workers);
        }
    }
    return ctx;
}
static EGLContext w_eglGetCurrentContext(void) { return a_eglGetCurrentContext(); }
static EGLSurface w_eglGetCurrentSurface(EGLint w) { return a_eglGetCurrentSurface(w); }
static EGLDisplay w_eglGetCurrentDisplay(void) { return a_eglGetCurrentDisplay(); }
static EGLenum w_eglQueryAPI(void) { return a_eglQueryAPI(); }
static EGLint w_eglGetError(void) { return E.ready ? a_eglGetError() : 0x3001; }

/* The guest's ANativeWindow: a Metal layer on the phone, an off-screen buffer for tests. */
static bool g_offscreen_windows;
/* A game that draws with Vulkan on the window's layer still makes an OpenGL ES context first, to read what the device offers. The layer is MoltenVK's, so ANGLE gets a buffer instead. */
void tl_egl_offscreen_windows(bool on) { g_offscreen_windows = on; }
/* surfaces made on a TextureView's window: off-screen, read back into its SurfaceTexture at each swap.
 * Each carries the SESSION epoch it was made in, so a swap from it after that session has ended can be
 * refused: a thread left parked by a close must not keep presenting (see w_eglSwapBuffers). */
#define MAX_TEX_SURFACES 16
static struct { EGLSurface s; void *win; int epoch; } g_tex_surf[MAX_TEX_SURFACES];
static pthread_mutex_t g_tex_lock = PTHREAD_MUTEX_INITIALIZER;
/* The Java runtime's compositor (husk-tl-dvm-android.c), where it is linked: it holds the screen's surface, so an app that makes
 * an EGL window of its own (SDL, Unity on a SurfaceView) draws off screen and each swap hands the frame to it, to be put on the
 * screen under the app's views. Two window surfaces on one layer is what a phone refuses (EGL_BAD_ALLOC). */
__attribute__((weak)) bool tl_javaapp_compositing(void);
__attribute__((weak)) void tl_javaapp_app_pixels(const void *px, int w, int h);
#define MAX_COMP_SURFACES 8
/* Each composited surface carries the SESSION epoch it was made in, like the texture ones above. */
static struct { EGLSurface s; int epoch; } g_comp_surf[MAX_COMP_SURFACES];
/* Every real WINDOW surface this layer has made, with the session epoch it was made in: the compositor's own surface,
 * or a lone app's engine drawing straight into its layer. A swap from a surface whose session has ended is refused in
 * w_eglSwapBuffers -- the OS-level half of "a closed app cannot present". Bounded; a surface that did not fit is simply
 * left ungated (it keeps the old behaviour), so this table can never stop a live app from drawing. */
#define MAX_WIN_SURFACES 24
static struct { EGLSurface s; int epoch; } g_win_surf[MAX_WIN_SURFACES];
/* Which of the layer's own surfaces this is and the session it belongs to, in ONE pass under the lock, so a swap pays a
 * single mutex (the old code took it twice: tex_window_of then comp_surface). `kind_out` is SURF_TEX/COMP/WIN, and
 * `texwin_out` is the TextureView window to post to. Returns the creation epoch, or -1 when this layer did not make the
 * surface (a lone app's frame-dump pbuffer, say) -- an unregistered surface is never gated. */
static int surface_session(EGLSurface s, int *kind_out, void **texwin_out)
{
    int e = -1, kind = 0;
    if (texwin_out) *texwin_out = NULL;
    if (kind_out) *kind_out = 0;
    if (!s) return -1;
    pthread_mutex_lock(&g_tex_lock);
    for (int i = 0; i < MAX_TEX_SURFACES; i++) if (g_tex_surf[i].s == s) { e = g_tex_surf[i].epoch; kind = 1; if (texwin_out) *texwin_out = g_tex_surf[i].win; goto done; }
    for (int i = 0; i < MAX_COMP_SURFACES; i++) if (g_comp_surf[i].s == s) { e = g_comp_surf[i].epoch; kind = 2; goto done; }
    for (int i = 0; i < MAX_WIN_SURFACES; i++) if (g_win_surf[i].s == s) { e = g_win_surf[i].epoch; kind = 3; goto done; }
done:
    pthread_mutex_unlock(&g_tex_lock);
    if (kind_out) *kind_out = kind;
    return e;
}
/* The session just ended (husk_tl_session_close, AFTER the census epoch advanced so the epoch check above is armed).
 * Two things, together, end the closing app's ability to present: (1) the frame it had composited is dropped from the
 * runtime's on-screen compositor, so a render thread still running cannot keep redrawing the dead app's last frame;
 * (2) every surface it made now carries an older epoch, so any further swap from its quarantined threads is refused
 * above. This is the FRAMES being cut off, never the threads being killed -- the close leaves them parked by design. */
void tl_egl_session_ended(void)
{
    if (tl_javaapp_app_pixels) tl_javaapp_app_pixels(NULL, 0, 0);
    tl_log_line("egl: session ended: the composited frame is dropped and swaps from this session's surfaces are refused (a closed app cannot present into the screen)");
}
/* The current frame of a surface, read back (bottom-up RGBA, in a buffer of this thread's). */
static const uint8_t *read_back(EGLDisplay d, EGLSurface s, EGLint *pw, EGLint *ph)
{
    EGLint w = 0, h = 0;
    a_eglQuerySurface(d, s, EGL_WIDTH, &w); a_eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0 || !a_glReadPixels) return NULL;
    static __thread uint8_t *buf; static __thread size_t cap;
    size_t need = (size_t)w * h * 4;
    if (need > cap) { free(buf); buf = malloc(need); cap = buf ? need : 0; }
    if (!buf) return NULL;
    int old_fb = 0, old_pack = 0;
    a_glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_fb);
    a_glGetIntegerv(0x0D05 /* GL_PACK_ALIGNMENT */, &old_pack);
    a_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    a_glPixelStorei(0x0D05, 1);
    a_glReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, buf);
    a_glPixelStorei(0x0D05, old_pack);
    a_glBindFramebuffer(0x8CA8, (unsigned)old_fb);
    *pw = w; *ph = h;
    return buf;
}
static EGLSurface w_eglCreateWindowSurface(EGLDisplay d, EGLConfig cfg, void *win, const EGLint *at)
{
    cfg = cfg_real(cfg);
    if (tl_nwindow_texture(win)) {
        EGLint pb[] = { EGL_WIDTH, tl_nwindow_width(win), EGL_HEIGHT, tl_nwindow_height(win), EGL_NONE };
        EGLSurface s = a_eglCreatePbufferSurface(d, cfg, pb);
        if (s) {
            pthread_mutex_lock(&g_tex_lock);
            for (int i = 0; i < MAX_TEX_SURFACES; i++) if (!g_tex_surf[i].s) { g_tex_surf[i].s = s; g_tex_surf[i].win = win; g_tex_surf[i].epoch = tl_guest_thread_epoch(); break; }
            pthread_mutex_unlock(&g_tex_lock);
        }
        tl_log_line("egl: TextureView window %dx%d -> off-screen surface %p", pb[1], pb[3], s);
        return s;
    }
    if (tl_javaapp_compositing && tl_javaapp_compositing()) {
        EGLint pb[] = { EGL_WIDTH, tl_nwindow_width(win), EGL_HEIGHT, tl_nwindow_height(win), EGL_NONE };
        EGLSurface s = a_eglCreatePbufferSurface(d, cfg, pb);
        if (s) {
            pthread_mutex_lock(&g_tex_lock);
            for (int i = 0; i < MAX_COMP_SURFACES; i++) if (!g_comp_surf[i].s) { g_comp_surf[i].s = s; g_comp_surf[i].epoch = tl_guest_thread_epoch(); break; }
            pthread_mutex_unlock(&g_tex_lock);
        }
        unsigned long made = s ? atomic_fetch_add(&E.comp_made, 1) + 1 : atomic_load(&E.comp_made);
        /* The count is the app's own EGL ledger's CREATE side: a surface handed over with no swap to follow is a
         * different failure from the app never asking for one, and this line says which happened, in one run. */
        tl_log_line("egl: window %dx%d -> surface %p, composited under the app's views (window surface #%lu handed to an app this process)", pb[1], pb[3], s, made);
        return s;
    }
    if (E.frame_dir[0] || g_offscreen_windows) {
        EGLint pb[] = { EGL_WIDTH, tl_nwindow_width(win), EGL_HEIGHT, tl_nwindow_height(win), EGL_NONE };
        EGLSurface s = a_eglCreatePbufferSurface(d, cfg, pb);
        if (!s) {
            /* the config the game chose may draw only to windows: the closest one that also makes buffers */
            EGLint r = 8, g = 8, b = 8, a = 0, dp = 0, st = 0, rt = 0;
            a_eglGetConfigAttrib(d, cfg, 0x3024 /* EGL_RED_SIZE */, &r); a_eglGetConfigAttrib(d, cfg, 0x3023 /* EGL_GREEN_SIZE */, &g); a_eglGetConfigAttrib(d, cfg, 0x3022 /* EGL_BLUE_SIZE */, &b);
            a_eglGetConfigAttrib(d, cfg, 0x3021 /* EGL_ALPHA_SIZE */, &a); a_eglGetConfigAttrib(d, cfg, 0x3025 /* EGL_DEPTH_SIZE */, &dp); a_eglGetConfigAttrib(d, cfg, 0x3026 /* EGL_STENCIL_SIZE */, &st);
            a_eglGetConfigAttrib(d, cfg, 0x3040 /* EGL_RENDERABLE_TYPE */, &rt);
            tl_log_line("egl: off-screen surface for config %p failed (%#x); rgba %d%d%d%d depth %d stencil %d", cfg, a_eglGetError(), r, g, b, a, dp, st);
        }
        tl_log_line("egl: window %dx%d -> off-screen surface %p", pb[1], pb[3], s);
        return s;
    }
    void *layer = tl_nwindow_native(win);
    EGLSurface s = a_eglCreateWindowSurface(d, cfg, layer, at);
    if (s) {
        pthread_mutex_lock(&g_tex_lock);
        for (int i = 0; i < MAX_WIN_SURFACES; i++) if (!g_win_surf[i].s) { g_win_surf[i].s = s; g_win_surf[i].epoch = tl_guest_thread_epoch(); break; }
        pthread_mutex_unlock(&g_tex_lock);
    }
    tl_log_line("egl: window surface %p on layer %p (%dx%d), eglGetError %#x", s, layer, tl_nwindow_width(win), tl_nwindow_height(win), a_eglGetError());
    return s;
}
static EGLBoolean w_eglDestroySurface(EGLDisplay d, EGLSurface s)
{
    pthread_mutex_lock(&g_tex_lock);
    for (int i = 0; i < MAX_TEX_SURFACES; i++) if (g_tex_surf[i].s == s) g_tex_surf[i].s = NULL;
    for (int i = 0; i < MAX_WIN_SURFACES && s; i++) if (g_win_surf[i].s == s) g_win_surf[i].s = NULL;
    bool comp = false;
    for (int i = 0; i < MAX_COMP_SURFACES && s; i++) if (g_comp_surf[i].s == s) { g_comp_surf[i].s = NULL; comp = true; }
    pthread_mutex_unlock(&g_tex_lock);
    if (comp && tl_javaapp_app_pixels) tl_javaapp_app_pixels(NULL, 0, 0);
    return a_eglDestroySurface(d, s);
}
static EGLSurface w_eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *at) { return a_eglCreatePbufferSurface(d, cfg_real(c), at); }

/* A frame as a BMP: bottom-up BGR from GL's bottom-up RGBA, so no flip is needed. */
static void save_frame(EGLDisplay d, EGLSurface s, unsigned long n)
{
    EGLint w = 0, h = 0;
    a_eglQuerySurface(d, s, EGL_WIDTH, &w);
    a_eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0 || !a_glReadPixels) return;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    int old_fb = 0, old_pack = 0;
    a_glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_fb);
    a_glGetIntegerv(0x0D05 /* GL_PACK_ALIGNMENT */, &old_pack);
    a_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    a_glPixelStorei(0x0D05, 1);
    a_glReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, rgba);
    a_glPixelStorei(0x0D05, old_pack);
    a_glBindFramebuffer(0x8CA8, (unsigned)old_fb);
    char path[700], final_path[700] = "";
    if (E.latest_only) {
        snprintf(final_path, sizeof(final_path), "%s/latest.bmp", E.frame_dir);
        snprintf(path, sizeof(path), "%s/latest.tmp", E.frame_dir);
    } else {
        snprintf(path, sizeof(path), "%s/frame-%05lu.bmp", E.frame_dir, n);
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t rowbytes = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + rowbytes * (uint32_t)h;
        uint8_t hdr[54] = { 'B', 'M' };
        memcpy(hdr + 2, &size, 4); uint32_t off = 54; memcpy(hdr + 10, &off, 4);
        uint32_t dib = 40; memcpy(hdr + 14, &dib, 4); memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24; uint32_t img = rowbytes * (uint32_t)h; memcpy(hdr + 34, &img, 4);
        fwrite(hdr, 1, 54, f);
        uint8_t *row = calloc(1, rowbytes);
        for (int y = 0; y < h; y++) {
            const uint8_t *src = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) { row[x * 3] = src[x * 4 + 2]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4]; }
            fwrite(row, 1, rowbytes, f);
        }
        free(row); fclose(f);
        if (final_path[0]) rename(path, final_path);
    }
    free(rgba);
}

#define GLT_RING 256
static atomic_ulong g_glt_n, g_glt_draws, g_glt_uploads;
void tl_egl_gl_histogram(char *out, size_t cap);
void tl_egl_recent_calls(char *out, size_t n, int count);
static void tex_swap(EGLDisplay d, EGLSurface s, void *win)
{
    EGLint w = 0, h = 0;
    a_eglQuerySurface(d, s, EGL_WIDTH, &w); a_eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0 || !a_glReadPixels) return;
    static __thread uint8_t *buf; static __thread size_t cap;
    size_t need = (size_t)w * h * 4;
    if (need > cap) { free(buf); buf = malloc(need); cap = buf ? need : 0; }
    if (!buf) return;
    int old_fb = 0, old_pack = 0;
    a_glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_fb);
    a_glGetIntegerv(0x0D05 /* GL_PACK_ALIGNMENT */, &old_pack);
    a_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    a_glPixelStorei(0x0D05, 1);
    a_glReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, buf);
    a_glPixelStorei(0x0D05, old_pack);
    a_glBindFramebuffer(0x8CA8, (unsigned)old_fb);
    tl_nwindow_post(win, buf, w, h, true);
}
/* A swap from a surface whose session has ended is refused: the guest thread that made it was QUARANTINED by the
 * close (this project never stops a thread that ran guest code), and it may keep swapping for as long as it runs --
 * but a closed app's frames must not reach the screen, which is exactly the "it booted Lewdest House's menu with dead
 * buttons" symptom when the next app's session is live. The surface carries the session epoch it was made in; the
 * live session is tl_guest_thread_epoch(). Bounded logging, one atomic per refused frame. */
static void egl_refuse_dead_swap(EGLSurface s, int made_epoch, int cur_epoch)
{
    atomic_fetch_add(&E.dropped, 1);
    static atomic_int noted;
    if (atomic_fetch_add(&noted, 1) < 4)
        tl_log_line("egl: refused a swap from a session that has ended (surface %p was made in session epoch %d; the live session is epoch %d) -- a closed app cannot present into the screen",
                    s, made_epoch, cur_epoch);
}
static EGLBoolean w_eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    int cur_epoch = tl_guest_thread_epoch();
    int kind = 0; void *texwin = NULL;
    int made = surface_session(s, &kind, &texwin);   /* one lock: the surface, its kind, and the session it was made in */
    if (made >= 0 && made != cur_epoch) { egl_refuse_dead_swap(s, made, cur_epoch); return EGL_TRUE; }
    if (kind == 1) { tex_swap(d, s, texwin); return EGL_TRUE; }
    unsigned long n = atomic_fetch_add(&E.presented, 1) + 1;
    if (kind == 2) {
        EGLint w = 0, h = 0;
        const uint8_t *px = read_back(d, s, &w, &h);
        if (px && tl_javaapp_app_pixels) tl_javaapp_app_pixels(px, w, h);
        atomic_fetch_add(&E.composited, 1);
        if (n <= 3 || n % 600 == 0) tl_log_line("egl: swap #%lu (composited)", n);
        return EGL_TRUE;
    }
    if (n <= 3 || n % 600 == 0) {
        if (getenv("TL_GL_TRACE")) tl_log_line("egl: swap #%lu (%lu GL calls, %lu draws, %lu uploads so far)", n, (unsigned long)atomic_load(&g_glt_n), (unsigned long)atomic_load(&g_glt_draws), (unsigned long)atomic_load(&g_glt_uploads));
        else tl_log_line("egl: swap #%lu", n);
    }
    if ((n == 300 || n == 3000) && getenv("TL_GL_TRACE")) {
        static char gl[GLT_RING * 40];
        tl_egl_recent_calls(gl, sizeof(gl), GLT_RING);
        fprintf(stderr, "egl: the calls before swap #%lu: %s\n", n, gl);
        static char hist[1024 * 48];
        tl_egl_gl_histogram(hist, sizeof(hist));
        fprintf(stderr, "egl: calls so far, by function: %s\n", hist);
    }
    if (E.frame_dir[0] && (n % (unsigned long)E.frame_every == 0 || n <= 3)) save_frame(d, s, n);
    return E.frame_dir[0] ? EGL_TRUE : a_eglSwapBuffers(d, s);
}

/* The damage-region swaps (Flutter's engine, HWUI): the same as a whole swap here -- through the wrapper, so a frame reaches the
 * compositor, the frame dump, or the screen as a plain swap would. */
static EGLBoolean w_eglSwapBuffersWithDamage(EGLDisplay d, EGLSurface s, const EGLint *rects, EGLint n) { (void)rects; (void)n; return w_eglSwapBuffers(d, s); }
static EGLBoolean w_eglSetDamageRegion(EGLDisplay d, EGLSurface s, EGLint *rects, EGLint n) { (void)d; (void)s; (void)rects; (void)n; return EGL_TRUE; }

/* Android extensions this ANGLE does not have: Swappy and Unity probe for them. */
static EGLBoolean w_eglPresentationTimeANDROID(EGLDisplay d, EGLSurface s, int64_t t) { (void)d; (void)s; (void)t; return EGL_TRUE; }
static EGLBoolean w_eglGetNextFrameIdANDROID(EGLDisplay d, EGLSurface s, uint64_t *id) { (void)d; (void)s; if (id) *id = 0; return EGL_FALSE; }
static EGLBoolean w_eglGetFrameTimestampsANDROID(EGLDisplay d, EGLSurface s, uint64_t id, EGLint n, const EGLint *t, int64_t *v) { (void)d; (void)s; (void)id; (void)n; (void)t; (void)v; return EGL_FALSE; }
static EGLBoolean w_eglGetCompositorTimingANDROID(EGLDisplay d, EGLSurface s, EGLint n, const EGLint *t, int64_t *v) { (void)d; (void)s; (void)n; (void)t; (void)v; return EGL_FALSE; }

/* ------------------------------------------------- GLES: stack-argument fixes */

/*
 * AAPCS64 gives each stack argument its own 8-byte slot; Apple's ABI packs small ones. A call
 * with two 4-byte stack arguments therefore lays them out differently, and the callee reads
 * the second from the wrong place. Each adapter takes its stack arguments as 8-byte values,
 * which is how the guest wrote them, and passes them on properly typed.
 */
static void (*r_glBlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
static void w_glBlitFramebuffer(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, uint64_t mask, uint64_t filter)
{ r_glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, (unsigned)mask, (unsigned)filter); }

static void (*r_glTexSubImage3D)(unsigned, int, int, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, uint64_t fmt, uint64_t type, const void *px)
{ r_glTexSubImage3D(t, l, x, y, z, w, h, d, (unsigned)fmt, (unsigned)type, px); }

static void (*r_glCompressedTexSubImage3D)(unsigned, int, int, int, int, int, int, int, unsigned, int, const void *);
static void w_glCompressedTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, uint64_t fmt, uint64_t size, const void *data)
{ r_glCompressedTexSubImage3D(t, l, x, y, z, w, h, d, (unsigned)fmt, (int)size, data); }

static void (*r_glCopyImageSubData)(unsigned, unsigned, int, int, int, int, unsigned, unsigned, int, int, int, int, int, int, int);
static void w_glCopyImageSubData(unsigned sn, unsigned st, int sl, int sx, int sy, int sz, unsigned dn, unsigned dt,
                                 uint64_t dl, uint64_t dx, uint64_t dy, uint64_t dz, uint64_t w, uint64_t h, uint64_t d)
{ r_glCopyImageSubData(sn, st, sl, sx, sy, sz, dn, dt, (int)dl, (int)dx, (int)dy, (int)dz, (int)w, (int)h, (int)d); }

/* GLboolean arguments: AAPCS64 callers do not extend them, Apple's callee may assume they are. */
static void (*r_glColorMask)(unsigned, unsigned, unsigned, unsigned);
static void w_glColorMask(unsigned r, unsigned g, unsigned b, unsigned a) { r_glColorMask(r & 0xff, g & 0xff, b & 0xff, a & 0xff); }
static void (*r_glDepthMask)(unsigned);
static void w_glDepthMask(unsigned f) { r_glDepthMask(f & 0xff); }
static void (*r_glVertexAttribPointer)(unsigned, int, unsigned, unsigned, int, const void *);
static void w_glVertexAttribPointer(unsigned i, int s, unsigned t, unsigned n, int st, const void *p) { r_glVertexAttribPointer(i, s, t, n & 0xff, st, p); }
static void (*r_glUniformMatrix2fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix2fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix2fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix3fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix3fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix3fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix4fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix4fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix4fv(l, c, t & 0xff, v); }
static void (*r_glSampleCoverage)(float, unsigned);
static void w_glSampleCoverage(float v, unsigned i) { r_glSampleCoverage(v, i & 0xff); }


/*
 * Extensions ANGLE advertises that do not work well enough to use. EXT_disjoint_timer_query is the one that matters:
 * its queries never report a result on Metal, and an engine that times its GPU work (bgfx, so Minecraft) waits for the
 * result forever. Taken out of the extension lists, the engine never asks.
 */
static const char *const k_hidden_ext[] = { "GL_EXT_disjoint_timer_query", "GL_EXT_disjoint_timer_query_webgl2", NULL };
static bool ext_hidden(const char *name, size_t n)
{
    for (int i = 0; k_hidden_ext[i]; i++) if (strlen(k_hidden_ext[i]) == n && !strncmp(k_hidden_ext[i], name, n)) return true;
    /* TL_GL_HIDE=GL_a,GL_b: more to leave out, for finding which an engine's choice depends on (which texture format it picks, say). */
    const char *extra = getenv("TL_GL_HIDE");
    while (extra && *extra) {
        const char *e = strchr(extra, ',');
        size_t m = e ? (size_t)(e - extra) : strlen(extra);
        if (m == n && !strncmp(extra, name, n)) return true;
        extra += m; if (e) extra++;
    }
    return false;
}

#include "husk-tl-egl-es31.inc"

static const unsigned char *(*r_glGetString)(unsigned);
static const unsigned char *w_glGetString(unsigned name)
{
    if (!r_glGetString && a_eglGetProcAddress) r_glGetString = a_eglGetProcAddress("glGetString");
    const unsigned char *s = r_glGetString ? r_glGetString(name) : NULL;
    if (g_es31_shim && s && (name == 0x1F02 || name == 0x8B8C)) s = (const unsigned char *)es31_fix((const char *)s, name == 0x8B8C);
    if (s && (name == 0x1F02 || name == 0x8B8C || name == 0x1F01 || name == 0x1F00)) {
        static atomic_int said[4];
        int k = name == 0x1F02 ? 0 : name == 0x8B8C ? 1 : name == 0x1F01 ? 2 : 3;
        if (!atomic_exchange(&said[k], 1)) tl_log_line("gl: %s = %.200s", k == 0 ? "GL_VERSION" : k == 1 ? "GL_SHADING_LANGUAGE_VERSION" : k == 2 ? "GL_RENDERER" : "GL_VENDOR", (const char *)s);
    }
    if (name != 0x1F03 /* GL_EXTENSIONS */ || !s) return s;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static const unsigned char *source; static char *filtered;
    pthread_mutex_lock(&mu);
    if (source != s) {
        free(filtered);
        size_t len = strlen((const char *)s);
        filtered = malloc(len + 64);
        size_t k = 0;
        for (const char *p = (const char *)s; *p;) {
            const char *e = strchr(p, ' ');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n && !ext_hidden(p, n)) { if (k) filtered[k++] = ' '; memcpy(filtered + k, p, n); k += n; }
            p += n; if (e) p++;
        }
        /* The engines that need ES 3.1 buffer textures check for the extension before asking for its entry points. */
        if (g_es31_shim) { k += (size_t)snprintf(filtered + k, 62, "%sGL_EXT_texture_buffer", k ? " " : ""); }
        filtered[k] = 0;
        source = s;
    }
    pthread_mutex_unlock(&mu);
    return (const unsigned char *)filtered;
}

static const unsigned char *(*r_glGetStringi)(unsigned, unsigned);
static const unsigned char *w_glGetStringi(unsigned name, unsigned index)
{
    const unsigned char *s = r_glGetStringi ? r_glGetStringi(name, index) : NULL;
    if (name == 0x1F03 && s && ext_hidden((const char *)s, strlen((const char *)s))) return (const unsigned char *)"GL_ANGLE_husk_hidden_extension";
    return s;
}

/* A shader that does not compile is the commonest reason an engine draws nothing and says nothing: say why, for the first few. */
static void (*r_glCompileShader)(unsigned);
static void w_glCompileShader(unsigned sh)
{
    static void (*getiv)(unsigned, unsigned, int *), (*getlog)(unsigned, int, int *, char *), (*getsrc)(unsigned, int, int *, char *);
    static atomic_int reported;
    r_glCompileShader(sh);
    if (!getiv && a_eglGetProcAddress) { getiv = a_eglGetProcAddress("glGetShaderiv"); getlog = a_eglGetProcAddress("glGetShaderInfoLog"); getsrc = a_eglGetProcAddress("glGetShaderSource"); }
    if (!getiv || !getlog || !getsrc) return;
    int ok = 1;
    getiv(sh, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    if (ok) return;
    int n = atomic_fetch_add(&reported, 1);
    if (n >= 8) return;
    int len = 0; getiv(sh, 0x8B84 /* GL_INFO_LOG_LENGTH */, &len);
    char *log = calloc(1, (size_t)len + 2); int got = 0;
    if (len > 0) getlog(sh, len, &got, log);
    int slen = 0; getiv(sh, 0x8B88 /* GL_SHADER_SOURCE_LENGTH */, &slen);
    char *src = calloc(1, (size_t)slen + 2);
    if (slen > 0) getsrc(sh, slen, &got, src);
    for (char *q = log; *q; q++) if (*q == '\n') *q = '|';
    tl_log_line("gl: shader %u failed to compile (%d bytes of source): %.380s", sh, slen, log);
    const char *dir = getenv("TL_GL_SHADER_DUMP");
    if (dir) {
        char path[600]; snprintf(path, sizeof(path), "%s/shader-%d.txt", dir, n);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n---- source ----\n%s\n", log, src); fclose(f); }
    }
    free(log); free(src);
}

/* EGL sync objects: this ANGLE does not check the handle, and its Metal back end waits through the calling thread's context --
 * which a frame pacer's fence thread (Swappy's) does not have. Waited on from such a thread, a fence counts as signalled. */
static EGLint (*r_eglClientWaitSyncKHR)(EGLDisplay, void *, EGLint, uint64_t);
static EGLBoolean (*r_eglDestroySyncKHR)(EGLDisplay, void *);
static EGLBoolean (*r_eglGetSyncAttribKHR)(EGLDisplay, void *, EGLint, EGLint *);
static EGLint (*r_eglClientWaitSync)(EGLDisplay, void *, EGLint, uint64_t);
static EGLBoolean (*r_eglDestroySync)(EGLDisplay, void *);
static EGLint w_eglClientWaitSyncKHR(EGLDisplay d, void *s, EGLint f, uint64_t t)
{
    if (!s) return EGL_FALSE;
    if (!a_eglGetCurrentContext()) return 0x30F6;                   /* EGL_CONDITION_SATISFIED_KHR */
    return r_eglClientWaitSyncKHR(d, s, f, t);
}
static EGLBoolean w_eglDestroySyncKHR(EGLDisplay d, void *s) { return s ? r_eglDestroySyncKHR(d, s) : EGL_FALSE; }
static EGLBoolean w_eglGetSyncAttribKHR(EGLDisplay d, void *s, EGLint a, EGLint *v) { return s ? r_eglGetSyncAttribKHR(d, s, a, v) : EGL_FALSE; }
static EGLint w_eglClientWaitSync(EGLDisplay d, void *s, EGLint f, uint64_t t)
{
    if (!s) return EGL_FALSE;
    if (!a_eglGetCurrentContext()) return 0x30F6;
    return r_eglClientWaitSync(d, s, f, t);
}
static EGLBoolean w_eglDestroySync(EGLDisplay d, void *s) { return s ? r_eglDestroySync(d, s) : EGL_FALSE; }

#define ADAPT(name) { #name, (void *)w_##name, (void **)&r_##name }
static const struct { const char *name; void *wrap; void **real; } k_adapt[] = {
    ADAPT(glBlitFramebuffer), ADAPT(glTexSubImage3D), ADAPT(glCompressedTexSubImage3D), ADAPT(glCopyImageSubData),
    ADAPT(glColorMask), ADAPT(glDepthMask), ADAPT(glVertexAttribPointer), ADAPT(glUniformMatrix2fv),
    ADAPT(glUniformMatrix3fv), ADAPT(glUniformMatrix4fv), ADAPT(glSampleCoverage), ADAPT(glGetString), ADAPT(glGetStringi), ADAPT(glCompileShader), ADAPT(glShaderSource), ADAPT(glAttachShader), ADAPT(glLinkProgram), ADAPT(glGetIntegerv), ADAPT(glBindBufferBase), ADAPT(glBindBuffer), ADAPT(glBufferData), ADAPT(glBufferSubData), ADAPT(glBindBufferRange),
    ADAPT(eglClientWaitSyncKHR), ADAPT(eglDestroySyncKHR), ADAPT(eglGetSyncAttribKHR), ADAPT(eglClientWaitSync), ADAPT(eglDestroySync),
};

static const struct { const char *name; void *fn; } k_egl[] = {
    { "eglGetDisplay", w_eglGetDisplay }, { "eglInitialize", w_eglInitialize }, { "eglTerminate", w_eglTerminate },
    { "eglQueryString", w_eglQueryString }, { "eglGetConfigs", w_eglGetConfigs }, { "eglChooseConfig", w_eglChooseConfig },
    { "eglGetConfigAttrib", w_eglGetConfigAttrib }, { "eglCreateContext", w_eglCreateContext },
    { "eglDestroyContext", w_eglDestroyContext }, { "eglCreateWindowSurface", w_eglCreateWindowSurface },
    { "eglCreatePbufferSurface", w_eglCreatePbufferSurface }, { "eglDestroySurface", w_eglDestroySurface },
    { "eglQuerySurface", w_eglQuerySurface }, { "eglSurfaceAttrib", w_eglSurfaceAttrib }, { "eglMakeCurrent", w_eglMakeCurrent },
    { "eglSwapBuffers", w_eglSwapBuffers }, { "eglSwapInterval", w_eglSwapInterval },
    { "eglSwapBuffersWithDamageKHR", w_eglSwapBuffersWithDamage }, { "eglSwapBuffersWithDamageEXT", w_eglSwapBuffersWithDamage },
    { "eglSetDamageRegionKHR", w_eglSetDamageRegion },
    { "eglGetCurrentContext", w_eglGetCurrentContext }, { "eglGetCurrentSurface", w_eglGetCurrentSurface },
    { "eglGetCurrentDisplay", w_eglGetCurrentDisplay }, { "eglBindAPI", w_eglBindAPI }, { "eglQueryAPI", w_eglQueryAPI },
    { "eglGetError", w_eglGetError }, { "eglWaitGL", w_eglWaitGL }, { "eglWaitNative", w_eglWaitNative },
    { "eglReleaseThread", w_eglReleaseThread },
    { "eglPresentationTimeANDROID", w_eglPresentationTimeANDROID }, { "eglGetNextFrameIdANDROID", w_eglGetNextFrameIdANDROID },
    { "eglGetFrameTimestampsANDROID", w_eglGetFrameTimestampsANDROID }, { "eglGetCompositorTimingANDROID", w_eglGetCompositorTimingANDROID },
};


/* ----------------------------------------------------------- GL call tracing */

/*
 * TL_GL_TRACE: every GL function the guest is given is wrapped in a trampoline that notes its name in a ring and then
 * tail-calls the real function with the registers (and stack) as they were, so any signature works. A crash report
 * can then say which calls came last (tl_egl_recent_calls). The trampolines live in the executable region.
 */
static const char *volatile g_glt_ring[GLT_RING];

/* how often each GL function was called, keyed by the (unique, interned) name pointer */
static struct { _Atomic(const char *) name; atomic_ulong n; } g_glt_hist[1024];
void tl_egl_gl_histogram(char *out, size_t cap)
{
    size_t k = 0; out[0] = 0;
    for (int i = 0; i < 1024 && k + 48 < cap; i++) {
        const char *nm = atomic_load(&g_glt_hist[i].name);
        if (nm) k += (size_t)snprintf(out + k, cap - k, "%s=%lu ", nm, (unsigned long)atomic_load(&g_glt_hist[i].n));
    }
}
void tl_glt_note(const char *name)
{
    g_glt_ring[atomic_fetch_add(&g_glt_n, 1) % GLT_RING] = name;
    for (unsigned h = (unsigned)(((uintptr_t)name >> 3) * 2654435761u) % 1024, probe = 0; probe < 1024; probe++, h = (h + 1) % 1024) {
        const char *cur = atomic_load(&g_glt_hist[h].name);
        if (!cur) { const char *z = NULL; if (atomic_compare_exchange_strong(&g_glt_hist[h].name, &z, name)) cur = name; else cur = z; }
        if (cur == name) { atomic_fetch_add(&g_glt_hist[h].n, 1); break; }
    }
    if (name[2] == 'D' && !strncmp(name, "glDraw", 6)) atomic_fetch_add(&g_glt_draws, 1);
    else if (!strncmp(name, "glBufferData", 12) || !strncmp(name, "glTexImage", 10) || !strncmp(name, "glTexSubImage", 13) || !strncmp(name, "glCompressedTex", 15)) atomic_fetch_add(&g_glt_uploads, 1);
}

void tl_egl_recent_calls(char *out, size_t n, int count)
{
    size_t k = 0;
    unsigned long total = atomic_load(&g_glt_n);
    out[0] = 0;
    for (int i = count; i >= 1 && k + 40 < n; i--) {
        if (total < (unsigned long)i) continue;
        const char *nm = g_glt_ring[(total - (unsigned long)i) % GLT_RING];
        if (nm) k += (size_t)snprintf(out + k, n - k, "%s%s", k ? " " : "", nm);
    }
}

__attribute__((naked, used)) static void glt_common(void)
{
#if defined(__aarch64__)
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n"
        "sub sp, sp, #176\n"
        "stp x0, x1, [sp, #0]\n" "stp x2, x3, [sp, #16]\n" "stp x4, x5, [sp, #32]\n" "stp x6, x7, [sp, #48]\n"
        "stp x8, x17, [sp, #64]\n"
        "stp d0, d1, [sp, #80]\n" "stp d2, d3, [sp, #96]\n" "stp d4, d5, [sp, #112]\n" "stp d6, d7, [sp, #128]\n"
        "mov x0, x16\n"
        "bl _tl_glt_note\n"
        "ldp x0, x1, [sp, #0]\n" "ldp x2, x3, [sp, #16]\n" "ldp x4, x5, [sp, #32]\n" "ldp x6, x7, [sp, #48]\n"
        "ldp x8, x17, [sp, #64]\n"
        "ldp d0, d1, [sp, #80]\n" "ldp d2, d3, [sp, #96]\n" "ldp d4, d5, [sp, #112]\n" "ldp d6, d7, [sp, #128]\n"
        "add sp, sp, #176\n"
        "ldp x29, x30, [sp], #16\n"
        "br x17\n");
#endif
}

static void *glt_wrap(const char *name, void *real)
{
    static struct { const char *name; void *thunk; } cache[4096];
    static int n;
    static uint8_t *page_rx, *page_rw;
    static int used = 256;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) { void *t = cache[i].thunk; pthread_mutex_unlock(&mu); return t; }
    void *thunk = real;
    char err[120];
    if (n < 4096 && tl_xmem_open(768u << 20, err, sizeof(err))) {
        if (used == 256) { used = 0; if (!tl_xmem_alloc(TL_XMEM_PAGE, &page_rx, &page_rw)) page_rx = page_rw = NULL; }
        if (page_rx) {
            uint8_t *trx = page_rx + (size_t)used * 64, *trw = page_rw + (size_t)used * 64;
            used++;
            /* ldr x16, <name>; ldr x17, <real>; ldr x15, <common>; br x15 -- then the three 8-byte literals */
            uint32_t ins[4] = { 0x58000000u | ((16u / 4) << 5) | 16, 0x58000000u | ((20u / 4) << 5) | 17, 0x58000000u | ((24u / 4) << 5) | 15, 0xD61F01E0u };
            memcpy(trw, ins, 16);
            uint64_t lit[3] = { (uint64_t)(uintptr_t)strdup(name), (uint64_t)(uintptr_t)real, (uint64_t)(uintptr_t)glt_common };
            memcpy(trw + 16, lit, 24);
            tl_xmem_flush(trx, 64);
            thunk = trx;
        }
    }
    cache[n].name = strdup(name); cache[n].thunk = thunk; n++;
    pthread_mutex_unlock(&mu);
    return thunk;
}

/* Debug labels and groups name objects for a graphics debugger and change nothing else. ANGLE's versions of them are not
 * worth reaching (one dereferences a null while the guest names a buffer), so they do nothing. */
static void w_gl_noop(void) {}
static const char *const k_noop_names[] = {
    "glObjectLabel", "glObjectLabelKHR", "glObjectPtrLabel", "glObjectPtrLabelKHR", "glLabelObjectEXT", "glInsertEventMarkerEXT",
    "glPushGroupMarkerEXT", "glPopGroupMarkerEXT", "glPushDebugGroup", "glPushDebugGroupKHR", "glPopDebugGroup", "glPopDebugGroupKHR",
    "glDebugMessageInsert", "glDebugMessageInsertKHR", "glDebugMessageCallback", "glDebugMessageCallbackKHR", "glDebugMessageControl",
    "glDebugMessageControlKHR", NULL
};

/* ES 3.1 entry points an ES 3.0 context cannot honour. Under the 3.1 shim a game that asks for them is handed an empty function,
 * so that setup which merely creates the objects goes through; whatever really depends on them draws nothing. */
static const char *const k_es31_stub_names[] = {
    "glTexBufferEXT", "glTexBufferOES", "glTexBuffer", "glTexBufferRangeEXT", "glTexBufferRangeOES", "glTexBufferRange",
    "glBindImageTexture", "glDispatchCompute", "glDispatchComputeIndirect", "glMemoryBarrier", "glMemoryBarrierByRegion",
    "glFramebufferParameteri", "glGetFramebufferParameteriv", "glTexStorage2DMultisample", NULL
};

static void *egl_resolve(const char *name);
void *tl_egl_resolve(const char *name)
{
    void *r = egl_resolve(name);
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace && name[0] == 'e') tl_log_line("egl: resolve %s -> %p", name, r);
    return r;
}
static void *egl_resolve(const char *name)
{
    for (int i = 0; k_noop_names[i]; i++) if (!strcmp(k_noop_names[i], name)) return (void *)w_gl_noop;
    if (g_es31_shim) for (int i = 0; k_es31_stub_names[i]; i++) if (!strcmp(k_es31_stub_names[i], name)) {
        void *real = E.ready && a_eglGetProcAddress ? a_eglGetProcAddress(name) : NULL;
        if (real) break;
        static int said; if (!said++) tl_log_line("gl: stubbing ES 3.1 entry points ANGLE lacks (first: %s)", name);
        return (void *)w_gl_noop;
    }
    for (size_t i = 0; i < sizeof(k_egl) / sizeof(k_egl[0]); i++) if (!strcmp(k_egl[i].name, name)) return k_egl[i].fn;
    if (!E.ready || !a_eglGetProcAddress) return NULL;
    void *real = a_eglGetProcAddress(name);
    if (!real && E.gles) {
        /* dlsym on the handle also searches what ANGLE links against, and on a Mac that is the system's desktop OpenGL:
         * glPolygonMode and its kin would resolve there, and a GLES guest that calls one must not reach them. */
        real = dlsym(E.gles, name);
        Dl_info di;
        if (real && dladdr(real, &di) && di.dli_fname && strcmp(di.dli_fname, E.gles_path) != 0 && strcmp(di.dli_fname, E.egl_path) != 0) real = NULL;
    }
    if (!real) return NULL;
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_GL_TRACE") ? 1 : 0;
    for (size_t i = 0; i < sizeof(k_adapt) / sizeof(k_adapt[0]); i++) {
        if (!strcmp(k_adapt[i].name, name)) { *k_adapt[i].real = real; return trace ? glt_wrap(name, k_adapt[i].wrap) : k_adapt[i].wrap; }
    }
    return trace && name[0] == 'g' && name[1] == 'l' ? glt_wrap(name, real) : real;
}

/* eglGetProcAddress for the guest */
static void *w_eglGetProcAddress(const char *name) { return tl_egl_resolve(name); }
const tl_bionic_entry tl_tab_egl[] = {
    TL_WRAP("eglGetProcAddress", w_eglGetProcAddress), TL_WRAP("glGetString", w_glGetString),
    { "eglGetDisplay", w_eglGetDisplay }, { "eglInitialize", w_eglInitialize }, { "eglTerminate", w_eglTerminate },
    { "eglQueryString", w_eglQueryString }, { "eglGetConfigs", w_eglGetConfigs }, { "eglChooseConfig", w_eglChooseConfig },
    { "eglGetConfigAttrib", w_eglGetConfigAttrib }, { "eglCreateContext", w_eglCreateContext },
    { "eglDestroyContext", w_eglDestroyContext }, { "eglCreateWindowSurface", w_eglCreateWindowSurface },
    { "eglCreatePbufferSurface", w_eglCreatePbufferSurface }, { "eglDestroySurface", w_eglDestroySurface },
    { "eglQuerySurface", w_eglQuerySurface }, { "eglSurfaceAttrib", w_eglSurfaceAttrib }, { "eglMakeCurrent", w_eglMakeCurrent },
    { "eglSwapBuffers", w_eglSwapBuffers }, { "eglSwapInterval", w_eglSwapInterval },
    { "eglSwapBuffersWithDamageKHR", w_eglSwapBuffersWithDamage }, { "eglSwapBuffersWithDamageEXT", w_eglSwapBuffersWithDamage },
    { "eglSetDamageRegionKHR", w_eglSetDamageRegion },
    { "eglGetCurrentContext", w_eglGetCurrentContext }, { "eglGetCurrentSurface", w_eglGetCurrentSurface },
    { "eglGetCurrentDisplay", w_eglGetCurrentDisplay }, { "eglBindAPI", w_eglBindAPI }, { "eglQueryAPI", w_eglQueryAPI },
    { "eglGetError", w_eglGetError }, { "eglWaitGL", w_eglWaitGL }, { "eglWaitNative", w_eglWaitNative },
    { "eglReleaseThread", w_eglReleaseThread },
    TL_END
};
