/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Java apps on Husk's Dalvik runtime: the natives of Husk's Java framework (husk.Native, android.opengl.GLES20), the GL thread a
 * GLSurfaceView's renderer runs on, the touch queue, and the driver that starts an app (husk.AppRunner).
 *
 * The framework itself is Java (src/java-framework): Activity, View, Handler and the rest are classes run by the interpreter. What
 * they cannot do in Java is here.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-dvm-javaapp.h"
#include "husk-tl-dvm-gl.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-egl.h"
#include "husk-tl-internal.h"
#include "husk-tl-ld.h"

void tl_nwindow_configure(int w, int h, void *layer);
void *tl_nwindow_get(void);

static struct {
    tl_javaapp_config cfg;
    char apk[1024], data[512], ext[512], pkg[200], activity[260], application[260], frame_dir[512], angle_egl[600], angle_gles[600];
    pthread_mutex_t q_lock;
    int32_t q[4096]; int qn;                    /* packed input events: phase, id, x bits, y bits */
    atomic_ulong frames;
    atomic_bool ended, paused, render_requested;
    jobj *gl_view, *gl_renderer;
    int gl_version;
    pthread_t gl_thread;
    bool gl_started;
    pthread_t app_thread;                       /* the app main thread: a close waits on it (its pthread_t was once a local) */
    bool gl_thread_live, app_thread_live;       /* the two threads were created, so a close has something to unwind */
    atomic_bool app_done, gl_done;              /* each thread's own "I have returned" flag, for a bounded close */
    jobj *content;
    char fwres[1024];
    tl_zip fwres_zip; bool fwres_tried, fwres_ok;
    pthread_mutex_t t_lock;
    char **text; int ntext, ctext;              /* typed text waiting for Native.pollText: "t<text>", "d", "e", "h" */
    int insets[5];                              /* left, top, right, bottom, keyboard */
} A = { .q_lock = PTHREAD_MUTEX_INITIALIZER, .t_lock = PTHREAD_MUTEX_INITIALIZER };

/* The touches queued for the app, and the drains of that queue: both bounded, so the input chain can be read in one run
 * from a touch to the app's own view (host input: line -> tl_javaapp_touch -> N_pollInput -> AppRunner.deliver -> ViewRoot). */
static atomic_int g_input_queued, g_input_polls;

static jvalue L(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue I(int32_t i) { jvalue v; v.j = (uint32_t)i; return v; }
static jvalue J(int64_t j) { jvalue v; v.j = j; return v; }
static jvalue F(float f) { jvalue v; v.j = 0; v.f = f; return v; }

/* ================================================================== GL helpers for the generated natives */

void *gl_fn(const char *name) { return tl_egl_resolve(name); }

const char *gl_str(jobj *s) { return s ? tl_jni_string(s) : NULL; }

void *gl_array(jobj *arr, int32_t offset)
{
    if (!arr) return NULL;
    if (arr->kind == TL_K_PRIM_ARRAY) return (uint8_t *)arr->arr.data + (size_t)offset * arr->arr.esz;
    return NULL;
}

/* A java.nio buffer's address: a direct one's memory, or a heap one's backing array, from its position. */
void *gl_buffer(jobj *b)
{
    if (!b) return NULL;
    static dvm_field *f_addr, *f_pos, *f_shift;
    dvm_class *bc = dvm_class_of(dvm_class_named("java/nio/Buffer"));
    if (!f_addr && bc) { f_addr = dvm_find_field(bc, "address", false); f_pos = dvm_find_field(bc, "position", false); f_shift = dvm_find_field(bc, "_elementSizeShift", false); }
    if (!f_addr) return NULL;
    jvalue *s = dvm_slots(b);
    int64_t addr = s[f_addr->slot].j;
    int32_t pos = f_pos ? s[f_pos->slot].i : 0, shift = f_shift ? s[f_shift->slot].i : 0;
    if (addr) return (void *)(uintptr_t)(addr + ((int64_t)pos << shift));
    /* a heap buffer: hb (its array) and offset, on the concrete class */
    dvm_class *c = dvm_class_of(dvm_object_class(b));
    dvm_field *hb = c ? dvm_find_field(c, "hb", false) : NULL, *off = c ? dvm_find_field(c, "offset", false) : NULL;
    if (!hb) return NULL;
    jobj *arr = s[hb->slot].l;
    if (!arr || arr->kind != TL_K_PRIM_ARRAY) return NULL;
    int32_t o = off ? s[off->slot].i : 0;
    return (uint8_t *)arr->arr.data + ((int64_t)(o + pos) << shift);
}

/* ---- the GLES20 calls that hand back strings, written by hand */

static jobj *jstr(const char *s) { return dvm_new_string_utf8(s ? s : ""); }
#define NAT(fn) static bool fn(jobj *self, const jvalue *a, jvalue *ret)

NAT(G_glGetString)
{
    (void)self;
    const unsigned char *(*f)(uint32_t) = gl_fn("glGetString");
    const unsigned char *s = f ? f((uint32_t)a[0].i) : NULL;
    *ret = L(s ? jstr((const char *)s) : NULL);
    return true;
}
static jobj *info_log(const char *getiv, const char *getlog, int32_t obj)
{
    void (*iv)(uint32_t, uint32_t, int32_t *) = gl_fn(getiv);
    void (*lg)(uint32_t, int32_t, int32_t *, char *) = gl_fn(getlog);
    int32_t len = 0;
    if (iv) iv((uint32_t)obj, 0x8B84 /* GL_INFO_LOG_LENGTH */, &len);
    if (len <= 0 || !lg) return jstr("");
    char *buf = calloc(1, (size_t)len + 1);
    lg((uint32_t)obj, len, NULL, buf);
    jobj *r = jstr(buf);
    free(buf);
    return r;
}
NAT(G_glGetProgramInfoLog) { (void)self; *ret = L(info_log("glGetProgramiv", "glGetProgramInfoLog", a[0].i)); return true; }
NAT(G_glGetShaderInfoLog) { (void)self; *ret = L(info_log("glGetShaderiv", "glGetShaderInfoLog", a[0].i)); return true; }
NAT(G_glGetShaderSource)
{
    (void)self;
    void (*f)(uint32_t, int32_t, int32_t *, char *) = gl_fn("glGetShaderSource");
    char buf[65536] = "";
    if (f) f((uint32_t)a[0].i, sizeof(buf), NULL, buf);
    *ret = L(jstr(buf));
    return true;
}
static jobj *active(const char *fn, int32_t program, int32_t index, int32_t *size, int32_t *type)
{
    void (*f)(uint32_t, uint32_t, int32_t, int32_t *, int32_t *, uint32_t *, char *) = gl_fn(fn);
    char name[512] = ""; int32_t len = 0, sz = 0; uint32_t ty = 0;
    if (f) f((uint32_t)program, (uint32_t)index, sizeof(name), &len, &sz, &ty, name);
    if (size) *size = sz;
    if (type) *type = (int32_t)ty;
    return jstr(name);
}
NAT(G_glGetActiveAttrib6)
{
    (void)self;
    *ret = L(active("glGetActiveAttrib", a[0].i, a[1].i, gl_array(a[2].l, a[3].i), gl_array(a[4].l, a[5].i)));
    return true;
}
NAT(G_glGetActiveAttrib4) { (void)self; *ret = L(active("glGetActiveAttrib", a[0].i, a[1].i, gl_buffer(a[2].l), gl_buffer(a[3].l))); return true; }
NAT(G_glGetActiveUniform6)
{
    (void)self;
    *ret = L(active("glGetActiveUniform", a[0].i, a[1].i, gl_array(a[2].l, a[3].i), gl_array(a[4].l, a[5].i)));
    return true;
}
NAT(G_glGetActiveUniform4) { (void)self; *ret = L(active("glGetActiveUniform", a[0].i, a[1].i, gl_buffer(a[2].l), gl_buffer(a[3].l))); return true; }
static void active11(const char *fn, const jvalue *a)
{
    void (*f)(uint32_t, uint32_t, int32_t, int32_t *, int32_t *, uint32_t *, char *) = gl_fn(fn);
    if (f) f((uint32_t)a[0].i, (uint32_t)a[1].i, a[2].i, gl_array(a[3].l, a[4].i), gl_array(a[5].l, a[6].i), gl_array(a[7].l, a[8].i), gl_array(a[9].l, a[10].i));
}
NAT(G_glGetActiveAttrib11) { (void)self; (void)ret; active11("glGetActiveAttrib", a); return true; }
NAT(G_glGetActiveUniform11) { (void)self; (void)ret; active11("glGetActiveUniform", a); return true; }

NAT(G_glShaderSource)
{
    (void)self; (void)ret;
    void (*f)(uint32_t, int32_t, const char *const *, const int32_t *) = gl_fn("glShaderSource");
    const char *src = gl_str(a[1].l);
    if (f && src) f((uint32_t)a[0].i, 1, &src, NULL);
    return true;
}

static const gl_native k_gles_special[] = {
    { "glShaderSource", "(ILjava/lang/String;)V", G_glShaderSource },
    { "glGetString", "(I)Ljava/lang/String;", G_glGetString },
    { "glGetProgramInfoLog", "(I)Ljava/lang/String;", G_glGetProgramInfoLog },
    { "glGetShaderInfoLog", "(I)Ljava/lang/String;", G_glGetShaderInfoLog },
    { "glGetShaderSource", "(I)Ljava/lang/String;", G_glGetShaderSource },
    { "glGetActiveAttrib", "(II[II[II)Ljava/lang/String;", G_glGetActiveAttrib6 },
    { "glGetActiveAttrib", "(IILjava/nio/IntBuffer;Ljava/nio/IntBuffer;)Ljava/lang/String;", G_glGetActiveAttrib4 },
    { "glGetActiveUniform", "(II[II[II)Ljava/lang/String;", G_glGetActiveUniform6 },
    { "glGetActiveUniform", "(IILjava/nio/IntBuffer;Ljava/nio/IntBuffer;)Ljava/lang/String;", G_glGetActiveUniform4 },
    { "glGetActiveAttrib", "(III[II[II[II[BI)V", G_glGetActiveAttrib11 },
    { "glGetActiveUniform", "(III[II[II[II[BI)V", G_glGetActiveUniform11 },
    { NULL, NULL, NULL },
};

/* ================================================================== husk.Native */

NAT(N_log)
{
    (void)self; (void)ret;
    static const char lv[] = "??VDIWEF";
    int p = a[0].i;
    tl_log_line("%c/%s: %s", p >= 0 && p < 8 ? lv[p] : '?', tl_jni_string(a[1].l) ? tl_jni_string(a[1].l) : "", tl_jni_string(a[2].l) ? tl_jni_string(a[2].l) : "");
    return true;
}
/* Assets: the app's APK first, then its splits and asset packs (a Play install keeps a game's data in an install-time asset pack). */
static const tl_zip_entry *asset_find(const char *n, const tl_zip **zout)
{
    /* "/lib/x.so": a file from the APK's root (openNonAssetFd), which asset names never start with */
    char path[1024]; if (n && n[0] == '/') snprintf(path, sizeof(path), "%s", n + 1); else snprintf(path, sizeof(path), "assets/%s", n ? n : "");
    for (int i = 0; i < 16; i++) {
        const tl_zip *z = tl_ld_apk_at(i);
        if (!z) { if (i == 0) continue; break; }
        const tl_zip_entry *e = tl_zip_find(z, path);
        if (e) { if (zout) *zout = z; return e; }
    }
    return NULL;
}
NAT(N_readAsset)
{
    (void)self;
    const tl_zip *z = NULL;
    const tl_zip_entry *e = asset_find(tl_jni_string(a[0].l), &z);
    if (!e) { *ret = L(NULL); return true; }
    const uint8_t *d; size_t len; bool owned; char err[160];
    if (!tl_zip_data(z, e, (size_t)1 << 30, &d, &len, &owned, err, sizeof(err))) { *ret = L(NULL); return true; }
    jobj *arr = tl_jni_new_prim_array('B', (uint32_t)len);
    arr->refs = 1u << 30;
    memcpy(arr->arr.data, d, len);
    if (owned) free((void *)d);
    *ret = L(arr);
    return true;
}
NAT(N_listAssets)
{
    (void)self;
    const char *dir = tl_jni_string(a[0].l);
    char prefix[1024]; snprintf(prefix, sizeof(prefix), "assets/%s%s", dir ? dir : "", dir && *dir ? "/" : "");
    size_t pl = strlen(prefix);
    char **names = NULL; int n = 0, cap = 0;
    for (int zi = 0; zi < 16; zi++) {
        const tl_zip *z = tl_ld_apk_at(zi);
        if (!z) { if (zi == 0) continue; break; }
        for (size_t i = 0; i < z->count; i++) {
            const char *en = z->entries[i].name;
            if (strncmp(en, prefix, pl)) continue;
            const char *rest = en + pl;
            if (!*rest) continue;
            size_t l = strcspn(rest, "/");
            bool dup = false;
            for (int k = 0; k < n && !dup; k++) dup = strlen(names[k]) == l && !strncmp(names[k], rest, l);
            if (dup) continue;
            if (n == cap) { cap = cap ? cap * 2 : 32; names = realloc(names, (size_t)cap * sizeof(*names)); }
            names[n++] = strndup(rest, l);
        }
    }
    jobj *arr = tl_jni_new_obj_array(tl_jni_class("java/lang/String"), (uint32_t)n);
    arr->cls = tl_jni_class("[Ljava/lang/String;");
    arr->refs = 1u << 30;
    for (int i = 0; i < n; i++) { arr->oarr.v[i] = jstr(names[i]); free(names[i]); }
    free(names);
    *ret = L(arr);
    return true;
}
/* An asset stored (not compressed) can be read straight out of its APK: a file descriptor and offset, as Android gives them. */
NAT(N_assetFd)
{
    (void)self;
    const tl_zip *z = NULL;
    const tl_zip_entry *e = asset_find(tl_jni_string(a[0].l), &z);
    if (!e || e->method != 0) { *ret = J(-1); return true; }
    const uint8_t *lh = z->map + e->local_offset;
    uint64_t data = e->local_offset + 30 + (lh[26] | lh[27] << 8) + (lh[28] | lh[29] << 8);
    int fd = z == tl_ld_apk_at(0) ? open(A.apk, O_RDONLY) : dup(z->fd);
    *ret = J(fd < 0 ? -1 : ((int64_t)fd << 40) | (int64_t)data);
    return true;
}
NAT(N_assetLength) { (void)self; const tl_zip_entry *e = asset_find(tl_jni_string(a[0].l), NULL); *ret = J(e ? (int64_t)e->usize : -1); return true; }
NAT(N_dataDir) { (void)self; (void)a; *ret = L(jstr(A.data)); return true; }
NAT(N_externalDir) { (void)self; (void)a; *ret = L(jstr(A.ext)); return true; }
NAT(N_packageName) { (void)self; (void)a; *ret = L(jstr(A.pkg)); return true; }
NAT(N_screenWidth) { (void)self; (void)a; *ret = I(A.cfg.width); return true; }
NAT(N_screenHeight) { (void)self; (void)a; *ret = I(A.cfg.height); return true; }
NAT(N_density) { (void)self; (void)a; *ret = F(A.cfg.density > 0 ? A.cfg.density : 3.0f); return true; }
NAT(N_requestRender) { (void)self; (void)a; (void)ret; atomic_store(&A.render_requested, true); return true; }
NAT(N_setContentView) { (void)self; (void)ret; A.content = a[0].l; return true; }
NAT(N_vibrate) { (void)self; (void)a; (void)ret; if (A.cfg.vibrate) A.cfg.vibrate((int)a[0].j); return true; }
NAT(N_openUrl) { (void)self; (void)ret; const char *u = tl_jni_string(a[0].l); tl_log_line("javaapp: open %s", u ? u : "?"); if (A.cfg.open_url && u) A.cfg.open_url(u); return true; }
NAT(N_exit)
{
    (void)self; (void)a; (void)ret;
    /* husk.Native.exit() is the framework's app-quit path (Process.killProcess, Activity.moveTaskToBack, AppRunner's own
     * finish once the activity stack is empty). The app shares Husk's process: name the call, mark the app ended and
     * return -- its own teardown goes on and a session close already running can complete. Never the host's exit(). */
    husk_tl_guest_exit_requested("husk.Native.exit", 0);
    atomic_store(&A.ended, true);
    return true;
}
NAT(N_uptimeNanos) { (void)self; (void)a; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); *ret = J((int64_t)t.tv_sec * 1000000000ll + t.tv_nsec); return true; }
NAT(N_pollInput)
{
    (void)self; (void)a;
    pthread_mutex_lock(&A.q_lock);
    int n = A.qn / 4;
    if (!n) { pthread_mutex_unlock(&A.q_lock); *ret = L(NULL); return true; }
    /* The queue end of the input chain: the app's main thread has taken the queued touches off it. The first few drains
     * only, so one run says whether input that reached the queue ever left it. A touch logged as queued (tl_javaapp_touch)
     * but never as taken is a queue nobody drains -- a Java main looper that is not running. */
    if (atomic_fetch_add(&g_input_polls, 1) < 4)
        tl_log_line("husk: java input: the app main thread took %d event(s) off the queue", n);
    jobj *arr = tl_jni_new_prim_array('I', (uint32_t)(1 + A.qn));
    arr->refs = 1u << 30;
    int32_t *v = arr->arr.data;
    v[0] = n;
    memcpy(v + 1, A.q, (size_t)A.qn * 4);
    A.qn = 0;
    pthread_mutex_unlock(&A.q_lock);
    *ret = L(arr);
    return true;
}

/* ---- files in the APKs: 0 the app's, 1 the platform's resources, 2.. the app's splits */
static const tl_zip *apk_n(int n, const char **path)
{
    if (path) *path = NULL;
    if (n == 0) { if (path) *path = A.apk; return tl_ld_apk_at(0); }
    if (n == 1) {
        if (!A.fwres_tried) {
            A.fwres_tried = true;
            char err[160];
            if (A.fwres[0]) A.fwres_ok = tl_zip_open(&A.fwres_zip, A.fwres, err, sizeof(err));
            if (!A.fwres_ok) tl_log_line("javaapp: no platform resources (%s): %s", A.fwres[0] ? A.fwres : "none given", A.fwres[0] ? err : "-");
        }
        if (path) *path = A.fwres;
        return A.fwres_ok ? &A.fwres_zip : NULL;
    }
    return tl_ld_apk_at(n - 1);
}
const char *tl_ld_queued_split(int i);
void dvm_set_app_loader(jobj *loader);
NAT(N_setAppClassLoader) { (void)self; (void)ret; dvm_set_app_loader(a[0].l); return true; }
/* The NDK's ALooper for a Java Looper's thread: native code that asks for its UI thread's looper (ALooper_forThread) and watches
 * file descriptors on it (Unity 6, GameActivity-style code) has its callbacks run between that thread's messages */
NAT(N_looperPrepare) { (void)self; (void)a; (void)ret; void *(*prep)(int) = tl_bionic_find("ALooper_prepare"); if (prep) prep(0); return true; }
NAT(N_looperPoll)
{
    (void)self; (void)a;
    ret->j = 0;
    void *(*cur)(void) = tl_bionic_find("ALooper_forThread");
    int (*poll_all)(int, int *, int *, void **) = tl_bionic_find("ALooper_pollAll");
    if (cur && poll_all && cur()) { poll_all(0, NULL, NULL, NULL); ret->z = true; }
    return true;
}
NAT(N_splitPaths)
{
    (void)self; (void)a;
    int n = 0;
    while (n < 16 && tl_ld_queued_split(n)) n++;
    jobj *arr = tl_jni_new_obj_array(tl_jni_class("java/lang/String"), (uint32_t)n);
    arr->cls = tl_jni_class("[Ljava/lang/String;");
    arr->refs = 1u << 30;
    for (int i = 0; i < n; i++) arr->oarr.v[i] = jstr(tl_ld_queued_split(i));
    *ret = L(arr);
    return true;
}
NAT(N_readApkFile)
{
    (void)self;
    const char *n = tl_jni_string(a[1].l);
    const tl_zip *z = apk_n(a[0].i, NULL);
    const tl_zip_entry *e = z && n ? tl_zip_find(z, n) : NULL;
    if (!e) { *ret = L(NULL); return true; }
    const uint8_t *d; size_t len; bool owned; char err[160];
    if (!tl_zip_data(z, e, (size_t)1 << 30, &d, &len, &owned, err, sizeof(err))) { *ret = L(NULL); return true; }
    jobj *arr = tl_jni_new_prim_array('B', (uint32_t)len);
    arr->refs = 1u << 30;
    memcpy(arr->arr.data, d, len);
    if (owned) free((void *)d);
    *ret = L(arr);
    return true;
}
NAT(N_apkFileFd)
{
    (void)self;
    const char *n = tl_jni_string(a[1].l), *path;
    const tl_zip *z = apk_n(a[0].i, &path);
    const tl_zip_entry *e = z && n ? tl_zip_find(z, n) : NULL;
    if (!e || e->method != 0) { *ret = J(-1); return true; }
    const uint8_t *lh = z->map + e->local_offset;
    uint64_t data = e->local_offset + 30 + (lh[26] | lh[27] << 8) + (lh[28] | lh[29] << 8);
    int fd = path && path[0] ? open(path, O_RDONLY) : dup(z->fd);
    *ret = J(fd < 0 ? -1 : ((int64_t)fd << 40) | (int64_t)data);
    return true;
}
NAT(N_apkFileLength)
{
    (void)self;
    const char *n = tl_jni_string(a[1].l);
    const tl_zip *z = apk_n(a[0].i, NULL);
    const tl_zip_entry *e = z && n ? tl_zip_find(z, n) : NULL;
    *ret = J(e ? (int64_t)e->usize : -1);
    return true;
}
NAT(N_apkPath) { (void)self; (void)a; *ret = L(jstr(A.apk)); return true; }

/* ---- the host keyboard, clipboard, share sheet, orientation, insets */
static void text_push(const char *kind, const char *utf8)
{
    pthread_mutex_lock(&A.t_lock);
    if (A.ntext == A.ctext) { A.ctext = A.ctext ? A.ctext * 2 : 16; A.text = realloc(A.text, (size_t)A.ctext * sizeof(*A.text)); }
    size_t l = strlen(kind) + (utf8 ? strlen(utf8) : 0) + 1;
    char *e = malloc(l);
    snprintf(e, l, "%s%s", kind, utf8 ? utf8 : "");
    A.text[A.ntext++] = e;
    pthread_mutex_unlock(&A.t_lock);
    tl_javaapp_touch(7, 0, 0, 0);
}
void tl_javaapp_text(const char *utf8) { if (utf8 && *utf8) text_push("t", utf8); }
void tl_javaapp_text_delete(void) { text_push("d", NULL); }
void tl_javaapp_text_action(void) { text_push("e", NULL); }
void tl_javaapp_keyboard_closed(void) { text_push("h", NULL); }
void tl_javaapp_set_insets(int l, int t, int r, int b, int ime)
{
    A.insets[0] = l; A.insets[1] = t; A.insets[2] = r; A.insets[3] = b; A.insets[4] = ime;
    tl_javaapp_touch(8, 0, 0, 0);
}
NAT(N_pollText)
{
    (void)self; (void)a;
    pthread_mutex_lock(&A.t_lock);
    int n = A.ntext;
    if (!n) { pthread_mutex_unlock(&A.t_lock); *ret = L(NULL); return true; }
    jobj *arr = tl_jni_new_obj_array(tl_jni_class("java/lang/String"), (uint32_t)n);
    arr->cls = tl_jni_class("[Ljava/lang/String;");
    arr->refs = 1u << 30;
    for (int i = 0; i < n; i++) { arr->oarr.v[i] = jstr(A.text[i]); free(A.text[i]); }
    A.ntext = 0;
    pthread_mutex_unlock(&A.t_lock);
    *ret = L(arr);
    return true;
}
NAT(N_showKeyboard) { (void)self; (void)ret; if (A.cfg.show_keyboard) A.cfg.show_keyboard(a[0].i != 0, a[1].i, a[2].i); else tl_log_line("javaapp: the app asked for the keyboard (%s)", a[0].i ? "show" : "hide"); return true; }
NAT(N_setClipboard) { (void)self; (void)ret; const char *t = tl_jni_string(a[0].l); if (A.cfg.set_clipboard) A.cfg.set_clipboard(t ? t : ""); return true; }
NAT(N_getClipboard)
{
    (void)self; (void)a;
    char *t = A.cfg.get_clipboard ? A.cfg.get_clipboard() : NULL;
    *ret = L(t ? jstr(t) : NULL);
    free(t);
    return true;
}
NAT(N_share) { (void)self; (void)ret; const char *t = tl_jni_string(a[0].l); tl_log_line("javaapp: share %.80s", t ? t : ""); if (A.cfg.share && t) A.cfg.share(t); return true; }
NAT(N_setOrientation) { (void)self; (void)ret; if (A.cfg.set_orientation) A.cfg.set_orientation(a[0].i); return true; }
NAT(N_nightMode) { (void)self; (void)a; jvalue v; v.j = A.cfg.night_mode ? 1 : 0; *ret = v; return true; }
NAT(N_insets)
{
    (void)self; (void)a;
    jobj *arr = tl_jni_new_prim_array('I', 5);
    arr->refs = 1u << 30;
    memcpy(arr->arr.data, A.insets, sizeof(A.insets));
    *ret = L(arr);
    return true;
}

static void *gl_main(void *arg);
NAT(N_startGL)
{
    void tl_thread_altstack(void); tl_thread_altstack();
    (void)self; (void)ret;
    if (A.gl_started) return true;
    A.gl_version = a[2].i; A.gl_view = a[0].l;
    A.gl_started = true;
    /* the render thread picks it up; released after the version and view, so it never sees the renderer without them */
    __atomic_store_n(&A.gl_renderer, a[1].l, __ATOMIC_RELEASE);
    return true;
}

static const struct { const char *name, *sig; dvm_native_fn fn; } k_native[] = {
    { "log", "(ILjava/lang/String;Ljava/lang/String;)V", N_log },
    { "readAsset", "(Ljava/lang/String;)[B", N_readAsset },
    { "listAssets", "(Ljava/lang/String;)[Ljava/lang/String;", N_listAssets },
    { "assetFd", "(Ljava/lang/String;)J", N_assetFd },
    { "assetLength", "(Ljava/lang/String;)J", N_assetLength },
    { "dataDir", "()Ljava/lang/String;", N_dataDir },
    { "externalDir", "()Ljava/lang/String;", N_externalDir },
    { "packageName", "()Ljava/lang/String;", N_packageName },
    { "screenWidth", "()I", N_screenWidth },
    { "screenHeight", "()I", N_screenHeight },
    { "density", "()F", N_density },
    { "startGL", "(Ljava/lang/Object;Ljava/lang/Object;I)V", N_startGL },
    { "requestRender", "()V", N_requestRender },
    { "setContentView", "(Ljava/lang/Object;)V", N_setContentView },
    { "pollInput", "()[I", N_pollInput },
    { "vibrate", "(J)V", N_vibrate },
    { "openUrl", "(Ljava/lang/String;)V", N_openUrl },
    { "exit", "()V", N_exit },
    { "uptimeNanos", "()J", N_uptimeNanos },
    { "readApkFile", "(ILjava/lang/String;)[B", N_readApkFile },
    { "splitPaths", "()[Ljava/lang/String;", N_splitPaths },
    { "nativeLooperPrepare", "()V", N_looperPrepare },
    { "nativeLooperPoll", "()Z", N_looperPoll },
    { "setAppClassLoader", "(Ljava/lang/ClassLoader;)V", N_setAppClassLoader },
    { "apkFileFd", "(ILjava/lang/String;)J", N_apkFileFd },
    { "apkFileLength", "(ILjava/lang/String;)J", N_apkFileLength },
    { "apkPath", "()Ljava/lang/String;", N_apkPath },
    { "pollText", "()[Ljava/lang/String;", N_pollText },
    { "showKeyboard", "(ZII)V", N_showKeyboard },
    { "setClipboard", "(Ljava/lang/String;)V", N_setClipboard },
    { "getClipboard", "()Ljava/lang/String;", N_getClipboard },
    { "share", "(Ljava/lang/String;)V", N_share },
    { "setOrientation", "(I)V", N_setOrientation },
    { "insets", "()[I", N_insets },
    { "nightMode", "()Z", N_nightMode },
    { NULL, NULL, NULL },
};

static dvm_native_fn egl_native_lookup(const char *name, const char *sig);
dvm_native_fn dvm_android_native(const char *cls, const char *name, const char *sig);
dvm_native_fn tl_gfx_native(const char *name, const char *sig);
dvm_native_fn tl_audio_native(const char *name, const char *sig);
dvm_native_fn tl_sensors_native(const char *name, const char *sig);
dvm_native_fn tl_sqlite_native(const char *name, const char *sig);
dvm_native_fn tl_media_native(const char *name, const char *sig);
dvm_native_fn tl_web_native(const char *name, const char *sig);
bool tl_web_any_visible(void);
dvm_native_fn dvm_android_native(const char *cls, const char *name, const char *sig)
{
    if (!strcmp(cls, "husk/Sqlite")) return tl_sqlite_native(name, sig);
    if (!strcmp(cls, "husk/MediaCodecNative")) return tl_media_native(name, sig);
    if (!strcmp(cls, "husk/Web")) return tl_web_native(name, sig);
    if (!strcmp(cls, "husk/Sensors")) return tl_sensors_native(name, sig);
    if (!strcmp(cls, "husk/Gfx")) return tl_gfx_native(name, sig);
    if (!strcmp(cls, "husk/Audio")) return tl_audio_native(name, sig);
    if (!strcmp(cls, "husk/EGLNative")) {
        return egl_native_lookup(name, sig);
    }
    if (!strcmp(cls, "husk/Native")) {
        for (int i = 0; k_native[i].name; i++) if (!strcmp(k_native[i].name, name) && !strcmp(k_native[i].sig, sig)) return k_native[i].fn;
    } else if (!strcmp(cls, "android/opengl/GLES20")) {
        for (int i = 0; k_gles_special[i].name; i++) if (!strcmp(k_gles_special[i].name, name) && !strcmp(k_gles_special[i].sig, sig)) return k_gles_special[i].fn;
        for (int i = 0; k_gles20[i].name; i++) if (!strcmp(k_gles20[i].name, name) && !strcmp(k_gles20[i].sig, sig)) return k_gles20[i].fn;
    } else if (!strncmp(cls, "android/opengl/GLES3", 20) || !strncmp(cls, "android/opengl/GLES1", 20)) {
        const char *v = cls + 19;
        const gl_native *t = !strcmp(v, "30") ? k_gles30 : !strcmp(v, "31") ? k_gles31 : !strcmp(v, "32") ? k_gles32 : !strcmp(v, "10") ? k_gles10
                           : !strcmp(v, "10Ext") ? k_gles10ext : !strcmp(v, "11") ? k_gles11 : !strcmp(v, "11Ext") ? k_gles11ext : NULL;
        for (int i = 0; t && t[i].name; i++) if (!strcmp(t[i].name, name) && !strcmp(t[i].sig, sig)) return t[i].fn;
    }
    return NULL;
}

/* ================================================================== the GL thread */

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig;
typedef int32_t EGLint;
#define EGL_NONE 0x3038

static int64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec; }

static void log_pending(const char *where)
{
    char buf[600];
    if (tl_dvm_describe_pending(buf, sizeof(buf))) tl_log_line("javaapp: %s threw %s", where, buf);
    tl_jni_clear();
}

/* ---- the view system's frame, drawn over whatever the app's GL drew (its own context, so the app's GL state is never touched) */

bool tl_ui_frame_take(const uint8_t **px, int *w, int *h, bool *any);
void tl_ui_frame_done(void);
bool tl_ui_has_frame(void);

typedef struct {
    uint32_t prog, tex, vbo;
    int tw, th;
} overlay;

static uint32_t ov_shader(uint32_t kind, const char *src)
{
    uint32_t (*create)(uint32_t) = gl_fn("glCreateShader");
    void (*source)(uint32_t, int32_t, const char *const *, const int32_t *) = gl_fn("glShaderSource");
    void (*compile)(uint32_t) = gl_fn("glCompileShader");
    uint32_t sh = create(kind);
    source(sh, 1, &src, NULL);
    compile(sh);
    return sh;
}

static void ov_init_flip(overlay *o, bool bottom_up)
{
    static const char *vs_top = "attribute vec2 p; varying vec2 t; void main() { t = vec2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5); gl_Position = vec4(p, 0.0, 1.0); }";
    static const char *vs_bottom = "attribute vec2 p; varying vec2 t; void main() { t = vec2((p.x + 1.0) * 0.5, (1.0 + p.y) * 0.5); gl_Position = vec4(p, 0.0, 1.0); }";
    const char *vs = bottom_up ? vs_bottom : vs_top;
    static const char *fs = "precision mediump float; uniform sampler2D s; varying vec2 t; void main() { gl_FragColor = texture2D(s, t); }";
    uint32_t (*createProgram)(void) = gl_fn("glCreateProgram");
    void (*attach)(uint32_t, uint32_t) = gl_fn("glAttachShader");
    void (*bindAttrib)(uint32_t, uint32_t, const char *) = gl_fn("glBindAttribLocation");
    void (*link)(uint32_t) = gl_fn("glLinkProgram");
    void (*genTex)(int32_t, uint32_t *) = gl_fn("glGenTextures");
    void (*genBuf)(int32_t, uint32_t *) = gl_fn("glGenBuffers");
    void (*bindBuf)(uint32_t, uint32_t) = gl_fn("glBindBuffer");
    void (*bufData)(uint32_t, intptr_t, const void *, uint32_t) = gl_fn("glBufferData");
    o->prog = createProgram();
    attach(o->prog, ov_shader(0x8B31, vs));
    attach(o->prog, ov_shader(0x8B30, fs));
    bindAttrib(o->prog, 0, "p");
    link(o->prog);
    genTex(1, &o->tex);
    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    genBuf(1, &o->vbo);
    bindBuf(0x8892, o->vbo);
    bufData(0x8892, sizeof(quad), quad, 0x88E4);
}
static void ov_init(overlay *o) { ov_init_flip(o, false); }

static void ov_upload(overlay *o, const uint8_t *px, int w, int h)
{
    void (*bindTex)(uint32_t, uint32_t) = gl_fn("glBindTexture");
    void (*texParam)(uint32_t, uint32_t, int32_t) = gl_fn("glTexParameteri");
    void (*texImage)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *) = gl_fn("glTexImage2D");
    void (*texSub)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *) = gl_fn("glTexSubImage2D");
    void (*activeTex)(uint32_t) = gl_fn("glActiveTexture");
    void (*pixelStore)(uint32_t, int32_t) = gl_fn("glPixelStorei");
    activeTex(0x84C0);
    bindTex(0x0DE1, o->tex);
    pixelStore(0x0CF5, 4);
    if (o->tw != w || o->th != h) {
        texParam(0x0DE1, 0x2801, 0x2601); texParam(0x0DE1, 0x2800, 0x2601);
        texParam(0x0DE1, 0x2802, 0x812F); texParam(0x0DE1, 0x2803, 0x812F);
        texImage(0x0DE1, 0, 0x1908, w, h, 0, 0x1908, 0x1401, px);
        o->tw = w; o->th = h;
    } else texSub(0x0DE1, 0, 0, 0, w, h, 0x1908, 0x1401, px);
}

static void ov_draw(overlay *o, int vw, int vh)
{
    void (*viewport)(int32_t, int32_t, int32_t, int32_t) = gl_fn("glViewport");
    void (*useProgram)(uint32_t) = gl_fn("glUseProgram");
    void (*enable)(uint32_t) = gl_fn("glEnable");
    void (*disable)(uint32_t) = gl_fn("glDisable");
    void (*blendFunc)(uint32_t, uint32_t) = gl_fn("glBlendFunc");
    void (*bindTex)(uint32_t, uint32_t) = gl_fn("glBindTexture");
    void (*bindBuf)(uint32_t, uint32_t) = gl_fn("glBindBuffer");
    void (*attrPtr)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void *) = gl_fn("glVertexAttribPointer");
    void (*enableAttr)(uint32_t) = gl_fn("glEnableVertexAttribArray");
    void (*draw)(uint32_t, int32_t, int32_t) = gl_fn("glDrawArrays");
    void (*activeTex)(uint32_t) = gl_fn("glActiveTexture");
    if (!o->tw) return;
    viewport(0, 0, vw, vh);
    disable(0x0B71); disable(0x0B44); disable(0x0C11); disable(0x0B90);    /* depth, cull, scissor, stencil */
    enable(0x0BE2);
    blendFunc(1, 0x0303);                                                  /* premultiplied: ONE, ONE_MINUS_SRC_ALPHA */
    useProgram(o->prog);
    activeTex(0x84C0);
    bindTex(0x0DE1, o->tex);
    bindBuf(0x8892, o->vbo);
    attrPtr(0, 2, 0x1406, 0, 0, NULL);
    enableAttr(0);
    draw(0x0005, 0, 4);
}

/* ---- EGL for the app's own threads (a GLSurfaceView copied into the app, an engine's GL thread, EGL14 users)
 *
 * The app's contexts are real ANGLE contexts made on its threads, sharing the render thread's objects. A window surface is a pbuffer
 * the size of the SurfaceView; eglSwapBuffers copies it into a texture (two, in turn) and hands that to the render thread, which
 * draws it where the SurfaceView is, under the window's views. A context that cannot share (ANGLE refuses some pairs) reads its
 * frame back into memory instead. */
typedef struct { EGLContext c; int version; bool readback; } app_ctx;
typedef struct { EGLSurface pb; int w, h; uint32_t tex[2]; int tw[2], th[2]; int cur; uint8_t *px; } app_surf;
static struct {
    pthread_mutex_t lk;
    pthread_cond_t cv;
    EGLDisplay dpy; EGLConfig cfg; EGLContext ui; bool es3, ready;
    /* the frame to show: a shared texture, or pixels (bottom row first) */
    uint32_t tex; uint8_t *px; int pw, ph; int x, y, w, h; uint64_t seq; bool on;
} E2 = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

static bool e2_wait_ready(void)
{
    pthread_mutex_lock(&E2.lk);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    while (!E2.ready) if (pthread_cond_timedwait(&E2.cv, &E2.lk, &ts)) break;
    bool r = E2.ready;
    pthread_mutex_unlock(&E2.lk);
    return r;
}

NAT(EN_createContext)
{
    (void)self;
    ret->j = 0;
    if (!e2_wait_ready()) return true;
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = tl_egl_resolve("eglCreateContext");
    int v = a[0].i <= 1 ? 1 : a[0].i >= 3 && E2.es3 ? 3 : 2;
    app_ctx *share = (app_ctx *)(uintptr_t)a[1].j;
    app_ctx *c = calloc(1, sizeof(*c));
    for (int tries = 0; tries < 2 && !c->c; tries++) {
        const EGLint attr[] = { 0x3098, v, EGL_NONE };
        c->c = createContext(E2.dpy, E2.cfg, share && share->c ? share->c : E2.ui, attr);
        if (!c->c) { c->c = createContext(E2.dpy, E2.cfg, share && share->c ? share->c : NULL, attr); c->readback = c->c != NULL; }
        if (!c->c && v == 1) v = 2;
    }
    if (!c->c) { tl_log_line("egl: no ES %d context for the app's thread", v); free(c); return true; }
    c->version = v;
    tl_log_line("egl: the app made an ES %d context on its own thread%s", v, c->readback ? " (frames read back)" : "");
    ret->j = (int64_t)(uintptr_t)c;
    return true;
}
NAT(EN_destroyContext)
{
    (void)self; (void)ret;
    app_ctx *c = (app_ctx *)(uintptr_t)a[0].j;
    if (!c) return true;
    unsigned (*destroy)(EGLDisplay, EGLContext) = tl_egl_resolve("eglDestroyContext");
    if (destroy) destroy(E2.dpy, c->c);
    free(c);
    return true;
}
NAT(EN_createSurface)
{
    (void)self;
    ret->j = 0;
    if (!e2_wait_ready()) return true;
    EGLSurface (*createPbuffer)(EGLDisplay, EGLConfig, const EGLint *) = tl_egl_resolve("eglCreatePbufferSurface");
    int w = a[0].i > 0 ? a[0].i : 1, h = a[1].i > 0 ? a[1].i : 1;
    const EGLint attr[] = { 0x3057, w, 0x3056, h, EGL_NONE };
    EGLSurface pb = createPbuffer ? createPbuffer(E2.dpy, E2.cfg, attr) : NULL;
    if (!pb) { tl_log_line("egl: no %dx%d surface for the app", w, h); return true; }
    app_surf *s = calloc(1, sizeof(*s));
    s->pb = pb; s->w = w; s->h = h;
    ret->j = (int64_t)(uintptr_t)s;
    return true;
}
NAT(EN_destroySurface)
{
    (void)self; (void)ret;
    app_surf *s = (app_surf *)(uintptr_t)a[0].j;
    if (!s) return true;
    unsigned (*destroy)(EGLDisplay, EGLSurface) = tl_egl_resolve("eglDestroySurface");
    pthread_mutex_lock(&E2.lk);
    if (E2.on && (E2.tex == s->tex[0] || E2.tex == s->tex[1])) { E2.on = false; E2.seq++; }
    pthread_mutex_unlock(&E2.lk);
    if (destroy) destroy(E2.dpy, s->pb);
    free(s->px);
    free(s);
    return true;
}
/* makeCurrent(surface, context): no context releases the thread's */
NAT(EN_makeCurrent)
{
    (void)self;
    unsigned (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = tl_egl_resolve("eglMakeCurrent");
    app_surf *s = (app_surf *)(uintptr_t)a[0].j;
    app_ctx *c = (app_ctx *)(uintptr_t)a[1].j;
    ret->j = 0;
    if (!E2.ready) { ret->z = !c; return true; }
    if (!c) { ret->z = makeCurrent(E2.dpy, NULL, NULL, NULL) != 0; return true; }
    EGLSurface surf = s ? s->pb : NULL;
    if (!surf) {
        /* a context with no surface: a small pbuffer per thread when the display cannot go surfaceless */
        static __thread EGLSurface tiny;
        if (makeCurrent(E2.dpy, NULL, NULL, c->c)) { ret->z = true; return true; }
        if (!tiny) { EGLSurface (*createPbuffer)(EGLDisplay, EGLConfig, const EGLint *) = tl_egl_resolve("eglCreatePbufferSurface"); const EGLint at[] = { 0x3057, 1, 0x3056, 1, EGL_NONE }; tiny = createPbuffer(E2.dpy, E2.cfg, at); }
        surf = tiny;
    }
    ret->z = makeCurrent(E2.dpy, surf, surf, c->c) != 0;
    if (!ret->z) tl_log_line("egl: makeCurrent failed for the app's thread");
    return true;
}
/* swap(surface, context, x, y, w, h): the frame the app drew goes on screen at the view's place */
NAT(EN_swap)
{
    (void)self;
    app_surf *s = (app_surf *)(uintptr_t)a[0].j;
    app_ctx *c = (app_ctx *)(uintptr_t)a[1].j;
    ret->j = 0;
    if (!s || !c) return true;
    void (*getIntegerv)(uint32_t, int32_t *) = gl_fn("glGetIntegerv");
    void (*bindTex)(uint32_t, uint32_t) = gl_fn("glBindTexture");
    void (*activeTex)(uint32_t) = gl_fn("glActiveTexture");
    void (*genTex)(int32_t, uint32_t *) = gl_fn("glGenTextures");
    void (*texParam)(uint32_t, uint32_t, int32_t) = gl_fn("glTexParameteri");
    void (*copyTex)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t) = gl_fn("glCopyTexImage2D");
    void (*copyTexSub)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) = gl_fn("glCopyTexSubImage2D");
    void (*bindFb)(uint32_t, uint32_t) = gl_fn("glBindFramebuffer");
    void (*readPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *) = gl_fn("glReadPixels");
    void (*pixelStore)(uint32_t, int32_t) = gl_fn("glPixelStorei");
    void (*finish)(void) = gl_fn("glFinish");
    int32_t fb = 0, active = 0x84C0, tex = 0, pack = 4;
    if (c->version >= 2) getIntegerv(0x8CA6, &fb);                /* the app's framebuffer: the frame is the surface's own */
    getIntegerv(0x84E0, &active);
    if (fb && bindFb) bindFb(0x8D40, 0);
    if (c->readback) {
        getIntegerv(0x0D05, &pack);
        s->px = realloc(s->px, (size_t)s->w * s->h * 4);
        pixelStore(0x0D05, 4);
        readPixels(0, 0, s->w, s->h, 0x1908, 0x1401, s->px);
        pixelStore(0x0D05, pack);
        pthread_mutex_lock(&E2.lk);
        E2.px = realloc(E2.px, (size_t)s->w * s->h * 4);
        memcpy(E2.px, s->px, (size_t)s->w * s->h * 4);
        E2.pw = s->w; E2.ph = s->h; E2.tex = 0;
    } else {
        activeTex(0x84C0);
        getIntegerv(0x8069, &tex);
        int k = s->cur;
        if (!s->tex[k]) genTex(1, &s->tex[k]);
        bindTex(0x0DE1, s->tex[k]);
        if (s->tw[k] != s->w || s->th[k] != s->h) {
            texParam(0x0DE1, 0x2801, 0x2601); texParam(0x0DE1, 0x2800, 0x2601);
            texParam(0x0DE1, 0x2802, 0x812F); texParam(0x0DE1, 0x2803, 0x812F);
            copyTex(0x0DE1, 0, 0x1908, 0, 0, s->w, s->h, 0);
            s->tw[k] = s->w; s->th[k] = s->h;
        } else copyTexSub(0x0DE1, 0, 0, 0, 0, 0, s->w, s->h);
        bindTex(0x0DE1, (uint32_t)tex);
        activeTex((uint32_t)active);
        finish();
        s->cur ^= 1;
        pthread_mutex_lock(&E2.lk);
        E2.tex = s->tex[k]; E2.pw = s->w; E2.ph = s->h;
    }
    E2.x = a[2].i; E2.y = a[3].i; E2.w = a[4].i > 0 ? a[4].i : s->w; E2.h = a[5].i > 0 ? a[5].i : s->h;
    E2.on = true; E2.seq++;
    pthread_mutex_unlock(&E2.lk);
    if (fb && bindFb) bindFb(0x8D40, (uint32_t)fb);
    ret->z = true;
    return true;
}
/* the GL thread Husk drives GLSurfaceView from: there EGL10 answers for the context it made */
static pthread_t g_render_thread;
NAT(EN_isRenderThread) { (void)self; (void)a; ret->j = 0; ret->z = pthread_equal(pthread_self(), g_render_thread) != 0; return true; }
NAT(EN_hide) { (void)self; (void)a; (void)ret; pthread_mutex_lock(&E2.lk); E2.on = false; E2.seq++; pthread_mutex_unlock(&E2.lk); return true; }
static const struct { const char *name, *sig; dvm_native_fn fn; } k_eglnative[] = {
    { "createContext", "(IJ)J", EN_createContext }, { "destroyContext", "(J)V", EN_destroyContext },
    { "createSurface", "(II)J", EN_createSurface }, { "destroySurface", "(J)V", EN_destroySurface },
    { "makeCurrent", "(JJ)Z", EN_makeCurrent }, { "swap", "(JJIIII)Z", EN_swap },
    { "isRenderThread", "()Z", EN_isRenderThread }, { "hide", "()V", EN_hide },
    { NULL, NULL, NULL },
};
static dvm_native_fn egl_native_lookup(const char *name, const char *sig)
{
    for (int i = 0; k_eglnative[i].name; i++) if (!strcmp(k_eglnative[i].name, name) && !strcmp(k_eglnative[i].sig, sig)) return k_eglnative[i].fn;
    return NULL;
}

/* drawing the app's frame on the render thread: a quad at the view's place, the frame's bottom row first */
static void app_frame_draw(overlay *o, overlay *up, int vw, int vh)
{
    pthread_mutex_lock(&E2.lk);
    bool on = E2.on; uint32_t tex = E2.tex; int x = E2.x, y = E2.y, w = E2.w, h = E2.h;
    if (on && !tex && E2.px) { ov_upload(up, E2.px, E2.pw, E2.ph); }
    pthread_mutex_unlock(&E2.lk);
    if (!on) return;
    void (*viewport)(int32_t, int32_t, int32_t, int32_t) = gl_fn("glViewport");
    void (*useProgram)(uint32_t) = gl_fn("glUseProgram");
    void (*disable)(uint32_t) = gl_fn("glDisable");
    void (*bindTex)(uint32_t, uint32_t) = gl_fn("glBindTexture");
    void (*bindBuf)(uint32_t, uint32_t) = gl_fn("glBindBuffer");
    void (*attrPtr)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void *) = gl_fn("glVertexAttribPointer");
    void (*enableAttr)(uint32_t) = gl_fn("glEnableVertexAttribArray");
    void (*draw)(uint32_t, int32_t, int32_t) = gl_fn("glDrawArrays");
    void (*activeTex)(uint32_t) = gl_fn("glActiveTexture");
    viewport(x, vh - y - h, w, h);
    disable(0x0B71); disable(0x0B44); disable(0x0C11); disable(0x0B90); disable(0x0BE2);
    useProgram(o->prog);
    activeTex(0x84C0);
    bindTex(0x0DE1, tex ? tex : up->tex);
    bindBuf(0x8892, o->vbo);
    attrPtr(0, 2, 0x1406, 0, 0, NULL);
    enableAttr(0);
    draw(0x0005, 0, 4);
    viewport(0, 0, vw, vh);
}
/* For EGL: whether this runtime's compositor holds the screen, and an app's own EGL window's frame for it (NULL: no longer shown) */
bool tl_javaapp_compositing(void) { pthread_mutex_lock(&E2.lk); bool r = E2.ready; pthread_mutex_unlock(&E2.lk); return r; }
void tl_javaapp_app_pixels(const void *px, int w, int h)
{
    pthread_mutex_lock(&E2.lk);
    if (!px) { E2.on = false; E2.seq++; pthread_mutex_unlock(&E2.lk); return; }
    size_t n = (size_t)w * h * 4;
    if (E2.pw != w || E2.ph != h || !E2.px) E2.px = realloc(E2.px, n);
    if (E2.px) memcpy(E2.px, px, n);
    E2.pw = w; E2.ph = h; E2.tex = 0;
    E2.x = 0; E2.y = 0; E2.w = A.cfg.width; E2.h = A.cfg.height;
    E2.on = true; E2.seq++;
    pthread_mutex_unlock(&E2.lk);
}
static uint64_t app_frame_seq(void) { pthread_mutex_lock(&E2.lk); uint64_t q = E2.seq; pthread_mutex_unlock(&E2.lk); return q; }
static bool app_frame_on(void) { pthread_mutex_lock(&E2.lk); bool q = E2.on; pthread_mutex_unlock(&E2.lk); return q; }

/*
 * The render thread: one EGL surface on the app's screen, and on it, each frame, what the app's GLSurfaceView renderer draws (in
 * the app's own context) with the window's views over it (in Husk's). An app without GL still gets this thread for its views.
 */
static void *gl_main(void *arg)
{
    (void)arg;
    pthread_setname_np("GLThread");
    EGLDisplay (*getDisplay)(void *) = tl_egl_resolve("eglGetDisplay");
    unsigned (*initialize)(EGLDisplay, EGLint *, EGLint *) = tl_egl_resolve("eglInitialize");
    unsigned (*bindAPI)(unsigned) = tl_egl_resolve("eglBindAPI");
    unsigned (*chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = tl_egl_resolve("eglChooseConfig");
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = tl_egl_resolve("eglCreateContext");
    EGLSurface (*createWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *) = tl_egl_resolve("eglCreateWindowSurface");
    unsigned (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = tl_egl_resolve("eglMakeCurrent");
    unsigned (*swapBuffers)(EGLDisplay, EGLSurface) = tl_egl_resolve("eglSwapBuffers");
    unsigned (*swapInterval)(EGLDisplay, EGLint) = tl_egl_resolve("eglSwapInterval");
    if (!getDisplay || !createWindowSurface || !swapBuffers) { tl_log_line("javaapp: EGL is not available"); atomic_store(&A.gl_done, true); return NULL; }
    EGLDisplay dpy = getDisplay(NULL);
    EGLint major = 0, minor = 0;
    initialize(dpy, &major, &minor);
    bindAPI(0x30A0);
    EGLConfig cfg = NULL; EGLint ncfg = 0;
    const EGLint want3[] = { 0x3040, 0x40, 0x3033, 0x5, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, EGL_NONE };
    const EGLint want2[] = { 0x3040, 0x4, 0x3033, 0x5, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, EGL_NONE };
    bool es3 = chooseConfig(dpy, want3, &cfg, 1, &ncfg) && ncfg >= 1;
    if (!es3 && (!chooseConfig(dpy, want2, &cfg, 1, &ncfg) || ncfg < 1)) { tl_log_line("javaapp: no EGL config"); atomic_store(&A.gl_done, true); return NULL; }
    const EGLint ui_attr[] = { 0x3098, 2, EGL_NONE };
    EGLContext ui = createContext(dpy, cfg, NULL, ui_attr), app = NULL;
    EGLSurface surf = createWindowSurface(dpy, cfg, tl_nwindow_get(), NULL);
    if (!ui || !surf || !makeCurrent(dpy, surf, surf, ui)) { tl_log_line("javaapp: no GL context"); atomic_store(&A.gl_done, true); return NULL; }
    if (swapInterval) swapInterval(dpy, 1);
    overlay ov = { 0 }, appov = { 0 }, appup = { 0 };
    ov_init(&ov);
    ov_init_flip(&appov, true);
    ov_init_flip(&appup, true);
    g_render_thread = pthread_self();
    pthread_mutex_lock(&E2.lk);
    E2.dpy = dpy; E2.cfg = cfg; E2.ui = ui; E2.es3 = es3; E2.ready = true;
    pthread_cond_broadcast(&E2.cv);
    pthread_mutex_unlock(&E2.lk);
    uint64_t app_seq = 0;
    void (*clearColor)(float, float, float, float) = gl_fn("glClearColor");
    void (*clear)(uint32_t) = gl_fn("glClear");
    tl_log_line("javaapp: render thread ready (ES %d available), %dx%d", es3 ? 3 : 2, A.cfg.width, A.cfg.height);
    /* Which EGL display/context/surface THIS app got, and that the render thread is a fresh one: the swap
     * hypothesis is that a second app inherits the first app's surface/context, and this line proves or
     * disproves it in one run (a surface/layer number that differs from the previous app's while the
     * previous one is never destroyed is the leak; the display is process-wide and intentionally shared). */
    tl_log_line("javaapp: GL up for this app: display %p, ui context %p, window surface %p (the render thread created it for this app; the previous app's surface is not reused)",
                (void *)dpy, (void *)ui, (void *)surf);

    jobj *gl10 = NULL;
    jvalue p1[1];
    int64_t next = now_ns();
    while (!atomic_load(&A.ended)) {
        if (atomic_load(&A.paused)) { usleep(20000); next = now_ns(); continue; }
        bool drew = false;
        /* the app's renderer, once its GLSurfaceView is on screen */
        if (__atomic_load_n(&A.gl_renderer, __ATOMIC_ACQUIRE) && !app) {
            /* GLSurfaceView's default is OpenGL ES 1 (fixed function, through ANGLE's ES1 front end); 2 and 3 as asked */
            int es = A.gl_version >= 3 && es3 ? 3 : A.gl_version <= 1 ? 1 : 2;
            const EGLint attr[] = { 0x3098, es, EGL_NONE };
            app = createContext(dpy, cfg, NULL, attr);
            if (!app && es == 1) {
                tl_log_line("javaapp: no GL ES 1 context; the app gets ES 2");
                es = 2;
                const EGLint attr2[] = { 0x3098, 2, EGL_NONE };
                app = createContext(dpy, cfg, NULL, attr2);
            }
            if (!app || !makeCurrent(dpy, surf, surf, app)) { tl_log_line("javaapp: no GL ES %d context for the app", es); A.gl_renderer = NULL; app = NULL; }
            else {
                tl_log_line("javaapp: the app's GL ES %d context is ready", es);
                tl_log_line("javaapp: app GL context %p on window surface %p (ES %d): onSurfaceCreated/onSurfaceChanged follow", (void *)app, (void *)surf, es);
                gl10 = dvm_new_object(dvm_class_named("android/opengl/HuskGL10"));
                dvm_slots(gl10);
                tl_jclass *cfgc = dvm_class_named("javax/microedition/khronos/egl/HuskEGL$Config");
                jobj *eglcfg = cfgc ? dvm_new_object(cfgc) : NULL;
                if (eglcfg) dvm_slots(eglcfg);
                jvalue p2[2] = { L(gl10), L(eglcfg) };
                tl_jni_call(A.gl_renderer, "onSurfaceCreated", "(Ljavax/microedition/khronos/opengles/GL10;Ljavax/microedition/khronos/egl/EGLConfig;)V", p2);
                if (tl_jni_pending()) log_pending("onSurfaceCreated");
                jvalue p3[3] = { L(gl10), I(A.cfg.width), I(A.cfg.height) };
                tl_jni_call(A.gl_renderer, "onSurfaceChanged", "(Ljavax/microedition/khronos/opengles/GL10;II)V", p3);
                if (tl_jni_pending()) log_pending("onSurfaceChanged");
                p1[0] = L(gl10);
            }
        }
        const uint8_t *px; int fw, fh; bool any;
        bool fresh = tl_ui_frame_take(&px, &fw, &fh, &any);
        if (fresh) {
            makeCurrent(dpy, surf, surf, ui);
            if (any) ov_upload(&ov, px, fw, fh);
            else ov.tw = ov.th = 0;
            tl_ui_frame_done();
        }
        if (app) {
            makeCurrent(dpy, surf, surf, app);
            jvalue draw = tl_jni_call(A.gl_view, "huskBeginFrame", "()Z", NULL);
            if (tl_jni_pending()) log_pending("a queued GL event");
            /* a new frame of views needs the scene under it again: what was swapped is gone */
            if (draw.z || fresh || atomic_exchange(&A.render_requested, false)) {
                tl_jni_call(A.gl_renderer, "onDrawFrame", "(Ljavax/microedition/khronos/opengles/GL10;)V", p1);
                if (tl_jni_pending()) log_pending("onDrawFrame");
                drew = true;
            }
        }
        uint64_t q = app_frame_seq();
        bool app_fresh = q != app_seq;
        app_seq = q;
        if (drew || fresh || app_fresh) {
            makeCurrent(dpy, surf, surf, ui);
            if (!app) { clearColor(0, 0, 0, tl_web_any_visible() && !app_frame_on() ? 0 : 1); clear(0x4000); }     /* web views show through from under the screen */
            if (!app) app_frame_draw(&appov, &appup, A.cfg.width, A.cfg.height);
            if (tl_ui_has_frame()) ov_draw(&ov, A.cfg.width, A.cfg.height);
            swapBuffers(dpy, surf);
            if (atomic_fetch_add(&A.frames, 1) == 0) tl_log_line("javaapp: first frame presented for this app (the render thread's own compositor frame)");
        }
        next += 1000000000ll / 60;
        int64_t late = now_ns() - next;
        if (late > 100000000ll) next = now_ns();
        else if (late < 0) { struct timespec ts = { 0, (long)(-late) }; nanosleep(&ts, NULL); }
    }
    atomic_store(&A.gl_done, true);         /* the close waits on this instead of joining (a join-collect would run TLS dtors) */
    return NULL;
}

/* ================================================================== the driver */

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static bool pool_str(const uint8_t *pool, size_t sz, uint32_t idx, char *out, size_t n)
{
    uint32_t count = rd32le(pool + 8), flags = rd32le(pool + 16), strings = rd32le(pool + 20);
    if (idx >= count) return false;
    uint32_t off = rd32le(pool + 28 + 4 * idx);
    const uint8_t *p = pool + strings + off;
    if (p >= pool + sz) return false;
    if (flags & 0x100) {                                       /* UTF-8 */
        if (*p & 0x80) p += 2; else p++;
        size_t len = *p; if (*p & 0x80) { len = ((size_t)(p[0] & 0x7f) << 8) | p[1]; p += 2; } else p++;
        snprintf(out, n, "%.*s", (int)len, (const char *)p);
    } else {
        size_t len = rd16le(p); p += 2;
        size_t k = 0;
        for (size_t i = 0; i < len && k + 1 < n; i++) { uint16_t c = rd16le(p + 2 * i); out[k++] = c < 0x80 ? (char)c : '?'; }
        out[k] = 0;
    }
    return true;
}

/* The launcher activity and package, from the binary manifest. */
bool tl_javaapp_manifest(const char *apk, char *pkg, size_t pn, char *activity, size_t an, char *application, size_t apn)
{
    tl_zip z; char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) return false;
    bool ok = false;
    const tl_zip_entry *e = tl_zip_find(&z, "AndroidManifest.xml");
    const uint8_t *d; size_t len; bool owned = false;
    char act[260] = "", found[260] = "", app[260] = "";
    if (application && apn) application[0] = 0;
    if (e && tl_zip_data(&z, e, 8u << 20, &d, &len, &owned, err, sizeof(err)) && len > 8 && rd16le(d) == 0x0003) {
        const uint8_t *pool = NULL; size_t psz = 0;
        const uint8_t *resmap = NULL; uint32_t nres = 0;              /* string index -> attribute resource id */
        for (size_t off = rd16le(d + 2); off + 8 <= len; ) {
            uint16_t type = rd16le(d + off); uint32_t size = rd32le(d + off + 4);
            if (size < 8 || off + size > len) break;
            if (type == 0x0001) { pool = d + off; psz = size; }
            else if (type == 0x0180) { resmap = d + off + rd16le(d + off + 2); nres = (size - rd16le(d + off + 2)) / 4; }
            else if (type == 0x0102 && pool && size >= 36) {
                const uint8_t *el = d + off;
                char name[64] = "";
                pool_str(pool, psz, rd32le(el + 20), name, sizeof(name));
                uint16_t astart = rd16le(el + 24), asize = rd16le(el + 26), acount = rd16le(el + 28);
                for (unsigned i = 0; i < acount; i++) {
                    const uint8_t *at = el + 16 + astart + (size_t)i * asize;
                    char an2[64] = "", av[260] = "";
                    uint32_t ni = rd32le(at + 4);
                    if (!pool_str(pool, psz, ni, an2, sizeof(an2))) continue;
                    /* a shrunk manifest keeps only the attribute's resource id (PayPal's): name and targetActivity by id */
                    if (!an2[0] && resmap && ni < nres) {
                        uint32_t id = rd32le(resmap + 4 * ni);
                        if (id == 0x01010003) snprintf(an2, sizeof(an2), "name"); else if (id == 0x01010202) snprintf(an2, sizeof(an2), "targetActivity");
                    }
                    if (rd32le(at + 8) != 0xFFFFFFFFu) pool_str(pool, psz, rd32le(at + 8), av, sizeof(av));
                    if (!strcmp(name, "manifest") && !strcmp(an2, "package")) snprintf(pkg, pn, "%s", av);
                    if (!strcmp(name, "application") && !strcmp(an2, "name")) snprintf(app, sizeof(app), "%s", av);
                    if ((!strcmp(name, "activity") || !strcmp(name, "activity-alias")) && !strcmp(an2, "name")) snprintf(act, sizeof(act), "%s", av);
                    if (!strcmp(name, "activity-alias") && !strcmp(an2, "targetActivity")) snprintf(act, sizeof(act), "%s", av);
                    if (!strcmp(name, "category") && !strcmp(an2, "name") && !strcmp(av, "android.intent.category.LAUNCHER") && act[0] && !found[0]) snprintf(found, sizeof(found), "%s", act);
                }
            }
            off += size;
        }
    }
    if (found[0]) {
        if (found[0] == '.') snprintf(activity, an, "%s%s", pkg, found);
        else if (!strchr(found, '.')) snprintf(activity, an, "%s.%s", pkg, found);
        else snprintf(activity, an, "%s", found);
        ok = true;
    }
    /* the app's Application subclass, created and its onCreate run before any activity */
    if (app[0] && application && apn) {
        if (app[0] == '.') snprintf(application, apn, "%s%s", pkg, app);
        else if (!strchr(app, '.')) snprintf(application, apn, "%s.%s", pkg, app);
        else snprintf(application, apn, "%s", app);
    }
    if (owned) free((void *)d);
    tl_zip_close(&z);
    return ok;
}

/* The pending exception's stack trace, through android.util.Log, line by line; the exception is cleared. */
static void log_pending_stack(void)
{
    jobj *e = tl_jni_pending_object();
    if (!e) return;
    tl_jni_clear();
    jvalue a[1] = { L(e) }, r;
    if (!tl_dvm_call_static("android/util/Log", "getStackTraceString", "(Ljava/lang/Throwable;)Ljava/lang/String;", a, &r)) { tl_jni_clear(); return; }
    const char *t = tl_jni_string(r.l);
    while (t && *t) {
        const char *nl = strchr(t, '\n');
        int len = nl ? (int)(nl - t) : (int)strlen(t);
        tl_log_line("javaapp:   %.*s", len, t);
        t = nl ? nl + 1 : NULL;
    }
}

static void *app_main(void *arg)
{
    void tl_thread_altstack(void); tl_thread_altstack();
    (void)arg;
    pthread_setname_np("main");
    jvalue a[2] = { L(dvm_new_string_utf8(A.activity)), L(A.application[0] ? dvm_new_string_utf8(A.application) : NULL) };
    jvalue r;
    if (!tl_dvm_call_static("husk/AppRunner", "run", "(Ljava/lang/String;Ljava/lang/String;)V", a, &r)) {
        char buf[800];
        tl_log_line("javaapp: the app's main thread ended with %s", tl_dvm_describe_pending(buf, sizeof(buf)) ? buf : "?");
        log_pending_stack();
        atomic_store(&A.ended, true);
    }
    atomic_store(&A.app_done, true);        /* the close waits on this instead of joining (a join-collect would run TLS dtors) */
    return NULL;
}

/* An empty file in the native library directory for each library the APKs carry: ClassLoader.findLibrary() and File.exists() checks
 * find it (Unity asks the class loader where libil2cpp is), while loading still takes the library from the APK by its name. */
static void stub_lib(const char *name, uint64_t size, void *dir)
{
    (void)size;
    char p[1400]; snprintf(p, sizeof(p), "%s/%s", (const char *)dir, name);
    struct stat st;
    if (stat(p, &st) != 0) { int fd = open(p, O_CREAT | O_WRONLY, 0755); if (fd >= 0) close(fd); }
}

bool tl_javaapp_start(const tl_javaapp_config *cfg)
{
    /* stores into a library's executable view (data sharing a page with its code, as Unity's does) go through the writable one */
    { void tl_codewrite_enable(void); tl_codewrite_enable(); }
    A.cfg = *cfg;
    /* A fresh session: clear the previous app's lifecycle flags (field by field -- A holds live mutexes, so never memset it) */
    A.gl_started = false; A.gl_thread_live = false; A.app_thread_live = false;
    A.gl_view = NULL; A.gl_renderer = NULL; A.content = NULL; A.gl_version = 0;
    atomic_store(&A.ended, false);
    atomic_store(&A.paused, false);
    atomic_store(&A.frames, 0);
    atomic_store(&A.app_done, false);
    atomic_store(&A.gl_done, false);
    pthread_mutex_lock(&A.q_lock); A.qn = 0; pthread_mutex_unlock(&A.q_lock);
    snprintf(A.apk, sizeof(A.apk), "%s", cfg->apk_path);
    /* an app whose manifest needs OpenGL ES 3.1 or 3.2 is told the driver has it (ANGLE over Metal stops at 3.0; the shim writes
       its 310/320 es shaders down), as it would otherwise refuse to start */
    { int tl_manifest_gles(const char *); void tl_egl_es3_claim(int); int gv = tl_manifest_gles(cfg->apk_path);
      if (gv >= 0x30001) { tl_egl_es3_claim(gv & 0xffff); tl_log_line("egl: the manifest asks for OpenGL ES 3.%d: the driver is described so", gv & 0xffff); } }
    snprintf(A.data, sizeof(A.data), "%s", cfg->data_dir);
    snprintf(A.ext, sizeof(A.ext), "%s/sdcard", cfg->data_dir);
    snprintf(A.fwres, sizeof(A.fwres), "%s", cfg->framework_res ? cfg->framework_res : "");
    for (int i = 0; i < 4; i++) A.insets[i] = cfg->insets[i];
    mkdir(A.data, 0755); mkdir(A.ext, 0755);
    if (!tl_javaapp_manifest(cfg->apk_path, A.pkg, sizeof(A.pkg), A.activity, sizeof(A.activity), A.application, sizeof(A.application))) {
        tl_log_line("javaapp: the manifest names no launcher activity");
        return false;
    }
    if (cfg->package_name && cfg->package_name[0]) snprintf(A.pkg, sizeof(A.pkg), "%s", cfg->package_name);
    tl_log_line("javaapp: %s, launcher %s, application %s", A.pkg, A.activity, A.application[0] ? A.application : "(none)");
    tl_nwindow_configure(cfg->width, cfg->height, cfg->metal_layer);
    if (cfg->angle_egl && !tl_egl_init(cfg->angle_egl, cfg->angle_gles, cfg->frame_dir, cfg->frame_every)) return false;
    { void tl_set_data_dir(const char *dir); tl_set_data_dir(cfg->data_dir); }
    /* A Ren'Py game routed to this driver (its launcher is a PythonSDLActivity, not a driver's own SDL) needs Ren'Py's
       Android environment before its Python starts -- the SDL path sets it in tl_sdl_start, this path never did, and that
       is why its loader found no modules. Same detection and the same single implementation; a non-Ren'Py app is untouched. */
    { bool tl_renpy_prepare_if_app(const char *apk, const char *data_dir); tl_renpy_prepare_if_app(cfg->apk_path, cfg->data_dir); }
    /* the environment every Android process starts with (TrustedCertificateStore builds paths from ANDROID_DATA) */
    setenv("ANDROID_DATA", "/data", 0); setenv("ANDROID_ROOT", "/system", 0); setenv("ANDROID_STORAGE", "/storage", 0);
    setenv("EXTERNAL_STORAGE", "/sdcard", 0); setenv("ANDROID_ASSETS", "/system/app", 0); setenv("BOOTCLASSPATH", "", 0);
    if (!tl_ld_add_apk(cfg->apk_path)) return false;
    if (!tl_dvm_add_apk(cfg->apk_path)) return false;
    /* a class a previous app detached (its dex was dropped on close) comes back against this app's dexes */
    tl_dvm_reattach_declared();
    { char lib[1100]; snprintf(lib, sizeof(lib), "%s/lib", A.data); mkdir(lib, 0755); tl_ld_apk_libs(stub_lib, lib); }
    { pthread_attr_t rt; pthread_attr_init(&rt); pthread_attr_setstacksize(&rt, 64u << 20);
      A.gl_thread_live = pthread_create(&A.gl_thread, &rt, gl_main, NULL) == 0; }
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256u << 20);
    if (pthread_create(&A.app_thread, &at, app_main, NULL) != 0) return false;   /* the handle a close waits on, not a local */
    A.app_thread_live = true;
    return true;
}

void tl_javaapp_touch(int phase, int id, float x, float y)
{
    union { float f; int32_t i; } fx, fy; fx.f = x; fy.f = y;
    pthread_mutex_lock(&A.q_lock);
    if (A.qn + 4 <= 4096) { A.q[A.qn++] = phase; A.q[A.qn++] = id; A.q[A.qn++] = fx.i; A.q[A.qn++] = fy.i; }
    int depth = A.qn / 4;
    pthread_mutex_unlock(&A.q_lock);
    /* One end of the input chain: the touch is now on the app's queue, waiting for the app main thread's next pickup
     * (N_pollInput). The first few touches only, and only touches -- keys, back and the rest share this queue and would
     * otherwise fill the log. A touch logged here but never in "the app main thread took ... off the queue" is a queue
     * nobody drains: the app's Java main looper is not polling. */
    if (phase <= 3 && atomic_fetch_add(&g_input_queued, 1) < 4)
        tl_log_line("husk: java input: queued touch (phase %d, id %d, %.0f,%.0f) -- %d event(s) waiting", phase, id, x, y, depth);
}
void tl_javaapp_key(int code, bool down) { tl_javaapp_touch(4, code, down ? 1e-45f : 0.0f, 0); }
void tl_javaapp_back(void) { tl_javaapp_touch(5, 0, 0, 0); }
void tl_javaapp_set_paused(bool p)
{
    if (atomic_exchange(&A.paused, p) != p) tl_javaapp_touch(6, p ? 1 : 0, 0, 0);     /* the activity pauses and resumes with the host */
}
unsigned long tl_javaapp_frames(void) { return atomic_load(&A.frames); }
bool tl_javaapp_ended(void) { return atomic_load(&A.ended); }

/* The app's own quit call reached the layer: mark it ended. Its GL loop leaves at its own check, its main thread unwinds
 * itself -- the layer ends nothing, and neither does the host. Idempotent (A.ended is only ever set true here). */
void tl_javaapp_request_exit(void) { atomic_store(&A.ended, true); }

/* The calling thread is leaving after a guest exit()/_exit()/abort(). If it is one of the app's own driver threads, its
 * done flag is set now, so tl_javaapp_reset()'s bounded wait does not spend its whole budget on a thread that is already
 * gone. Compared by handle; the thread is never joined, cancelled or signalled. */
void tl_javaapp_thread_leaving(void)
{
    pthread_t self = pthread_self();
    if (A.app_thread_live && pthread_equal(self, A.app_thread)) atomic_store(&A.app_done, true);
    if (A.gl_thread_live && pthread_equal(self, A.gl_thread)) atomic_store(&A.gl_done, true);
}

/* ================================================================== closing the app's session */

#define JAVA_RESET_POLL_MS 20
#define JAVA_RESET_BUDGET_MS 5000

/* Wait (bounded: Darwin has no pthread_timedjoin_np) for a thread's own done flag, as the old tree polled after a finished run.
 * Never pthread_join: collecting a thread that ran app code is the class of act that took Husk down before. */
static bool java_reset_wait(atomic_bool *done, int *ms_waited)
{
    int ms = 0;
    while (ms < JAVA_RESET_BUDGET_MS) {
        if (atomic_load(done)) break;
        struct timespec ts = { 0, JAVA_RESET_POLL_MS * 1000000L };
        nanosleep(&ts, NULL);
        ms += JAVA_RESET_POLL_MS;
    }
    if (ms_waited) *ms_waited = ms;
    return atomic_load(done);
}

/* What the app's own close actually did, read back from husk.AppRunner while its dexes are still loaded: how many
 * activities it took through onPause/onStop/onDestroy, and whether the teardown ran at all. Called only once the app's
 * main thread has returned (no guest thread left to race the lookups). Honest by construction: -1 means "could not read". */
static void report_app_close(void)
{
    jvalue r;
    bool ran = false; int p = -1, s = -1, d = -1;
    if (tl_dvm_call_static("husk/AppRunner", "huskShutdownRan", "()Z", NULL, &r)) ran = r.z != 0;
    if (tl_dvm_call_static("husk/AppRunner", "huskShutdownPaused", "()I", NULL, &r)) p = r.i;
    if (tl_dvm_call_static("husk/AppRunner", "huskShutdownStopped", "()I", NULL, &r)) s = r.i;
    if (tl_dvm_call_static("husk/AppRunner", "huskShutdownDestroyed", "()I", NULL, &r)) d = r.i;
    if (tl_jni_pending()) tl_jni_clear();
    tl_log_line("husk: java reset app close %s: onPause x%d, onStop x%d, onDestroy x%d (the app ran its own teardown)",
                ran ? "dispatched" : "DID NOT RUN", p, s, d);
}

int tl_javaapp_reset(void)
{
    if (!A.app_thread_live && !A.gl_thread_live) { tl_log_line("husk: java reset skipped (no app was started)"); return 0; }

    /* 1. Close the app the way Android does when the user leaves it: ask husk.AppRunner to run onPause -> onStop ->
          onDestroy for its activities (newest first) and Application.onTerminate on the MAIN thread, and only then to quit
          its main looper. This is the close the layer never had -- before it only stopped its OWN machinery and cleared
          the activity stack (resetForNewApp), so an app that does not quit on its own (a Unity game, whose frames and
          Handler messages and U.handleMessage run on its OWN threads) was never told to close and kept running after the
          session was reported closed. Driving the platform lifecycle is the safe way to end it: the app unwinds its own
          threads itself, as on a phone. shutdown() posts the work and returns; step 3 waits, bounded, for the main thread. */
    bool asked = false;
    if (A.app_thread_live) {
        jvalue r;
        if (tl_dvm_call_static("husk/AppRunner", "shutdown", "()V", NULL, &r)) {
            asked = true;
            tl_log_line("husk: java reset app close requested (onPause/onStop/onDestroy for the activity stack, then looper quit)");
            if (tl_jni_pending()) { char b[300]; tl_log_line("husk: java reset app close threw %s", tl_dvm_describe_pending(b, sizeof(b)) ? b : "?"); tl_jni_clear(); }
        } else {
            if (tl_jni_pending()) { char b[300]; tl_log_line("husk: java reset no husk.AppRunner.shutdown: %s; falling back to a bare looper quit", tl_dvm_describe_pending(b, sizeof(b)) ? b : "?"); tl_jni_clear(); }
            else tl_log_line("husk: java reset no husk.AppRunner.shutdown; falling back to a bare looper quit");
        }
        /* Fallback only: a framework without shutdown() still needs its main looper quit, which is all the close used to do.
           Issuing it is a cooperative stop, so `asked` stays true for the status the same way it did before. */
        if (!asked) {
            jvalue looper;
            if (tl_dvm_call_static("android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", NULL, &looper) && looper.l) {
                tl_jni_call(looper.l, "quit", "()V", NULL);
                asked = true;
                if (tl_jni_pending()) { char b[300]; tl_log_line("husk: java reset looper quit threw %s", tl_dvm_describe_pending(b, sizeof(b)) ? b : "?"); tl_jni_clear(); }
                tl_log_line("husk: java reset looper quit requested");
            } else {
                if (tl_jni_pending()) tl_jni_clear();
                tl_log_line("husk: java reset no main looper to quit");
            }
        }
    }

    /* 2. End the GL loop. It owns the ANGLE UI context and the window surface, so it leaves at its own check (never abruptly). */
    if (A.gl_thread_live) { atomic_store(&A.ended, true); tl_log_line("husk: java reset GL loop end requested"); }

    /* 3. Bounded wait for each thread to return; a thread that will not is left parked and named. */
    int status = 0;
    if (A.gl_thread_live) {
        int ms = 0;
        if (java_reset_wait(&A.gl_done, &ms)) { tl_log_line("husk: java reset GL thread returned (%d ms)", ms); pthread_detach(A.gl_thread); }
        else { tl_log_line("husk: java reset GL thread still running after %d ms; left parked", ms); status = 1; }
    }
    if (A.app_thread_live) {
        int ms = 0;
        if (java_reset_wait(&A.app_done, &ms)) {
            report_app_close();                                  /* the lifecycle teardown ran (or did not); say which */
            tl_log_line("husk: java reset app main thread returned (%d ms)", ms);
            pthread_detach(A.app_thread);
        } else {
            /* The main thread never ran the close within the grace: an app wedged on its own main thread. Quit its looper
             * directly as the close used to, give it one more bounded wait, and report how it ended -- never claim a close
             * that did not happen. */
            if (asked) {
                tl_log_line("husk: java reset the app's main thread did not run the close within %d ms; quitting its looper directly", ms);
                jvalue looper;
                if (tl_dvm_call_static("android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", NULL, &looper) && looper.l) {
                    tl_jni_call(looper.l, "quit", "()V", NULL);
                    if (tl_jni_pending()) tl_jni_clear();
                } else if (tl_jni_pending()) tl_jni_clear();
                java_reset_wait(&A.app_done, &ms);
            }
            if (atomic_load(&A.app_done)) {
                report_app_close();
                tl_log_line("husk: java reset app main thread returned after a bare quit (%d ms)", ms);
                pthread_detach(A.app_thread);
            } else {
                tl_log_line("husk: java reset app main thread still running after %d ms; left parked (the app was NOT closed)", ms);
                status = asked ? 1 : 2;          /* would not stop (quarantined) vs nothing could ask it to stop (irreversible) */
            }
        }
    }
    A.gl_thread_live = false;
    A.app_thread_live = false;

    /* 4. Drain the app's input and text queues; the locks stay (never memset A: a parked thread could still be waiting on one). */
    pthread_mutex_lock(&A.q_lock);
    int drained_input = A.qn;
    A.qn = 0;
    pthread_mutex_unlock(&A.q_lock);
    pthread_mutex_lock(&A.t_lock);
    int drained_text = A.ntext;
    for (int i = 0; i < A.ntext; i++) { free(A.text[i]); A.text[i] = NULL; }
    A.ntext = 0;
    pthread_mutex_unlock(&A.t_lock);
    tl_log_line("husk: java reset drained %d input events, %d text entries", drained_input, drained_text);

    /* 5. The app's GL objects and counters, one field at a time. */
    A.gl_view = NULL; A.gl_renderer = NULL; A.content = NULL;
    A.gl_started = false; A.gl_version = 0;
    atomic_store(&A.frames, 0);
    atomic_store(&A.paused, false);
    A.fwres_tried = false; A.fwres_ok = false;

    /* 6. The app's EGL frame to show is dropped; the display and contexts were the GL thread's (left to it, never destroyed
          from here). A new gl_main re-inits them. */
    pthread_mutex_lock(&E2.lk);
    E2.on = false; E2.ready = false; E2.tex = 0; E2.px = NULL; E2.pw = E2.ph = 0; E2.w = E2.h = 0; E2.seq++;
    pthread_cond_broadcast(&E2.cv);
    pthread_mutex_unlock(&E2.lk);

    /* 7. The window binding and the EGL texture surfaces belong to husk-tl-bionic-ndk.c / husk-tl-egl.c (not this file); the next
          app configures its own window through tl_nwindow_configure, so nothing is released here. */
    tl_log_line("husk: java reset done (status %d)", status);
    return status;
}
