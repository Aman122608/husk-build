/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Java a particular SDL game adds around SDL, implemented in C.
 *
 * SDL games are mostly one native library, but some put an Android interface around it (a start menu, a dialog for a name, a loading overlay) that the library calls
 * into and waits on. Each is a handful of methods; they are kept here, by package, and installed only for that game.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "husk-tl-bionic.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-internal.h"
#include "husk-tl-ld.h"

jobj *tl_assetstream_list(const char *path);
void tl_sdl_display_metrics(tl_jcall *c);
jobj *tl_assetstream_file_tree(void);
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static const char *Str(const jobj *o) { const char *s = tl_jni_string(o); return s ? s : ""; }
static void H_void(tl_jcall *c) { (void)c; }
static void H_false(tl_jcall *c) { c->ret = vz(0); }

/* A native of the game's own library, by its JNI name. */
static void *game_native(const char *mangled)
{
    static const char *const libs[] = { "libmain.so", NULL };
    (void)libs;
    void *fn = NULL;
    extern void *tl_sdl_game_symbol(const char *name);
    fn = tl_sdl_game_symbol(mangled);
    if (!fn) tl_log_line("sdl: the game has no native %s", mangled);
    return fn;
}

/* The answer Java gives later, from another thread, once its dialog is dismissed: here a moment after the question, from a thread of its own. */
struct answer { void (*fn)(void *env, void *self, int a); void *self; int a; void (*fn_text)(void *env, void *self, uint8_t ok, void *text); char text[200]; };
static void *deliver(void *arg)
{
    struct answer *r = arg;
    usleep(250000);
    void *env = tl_jni_env();
    if (r->fn) r->fn(env, r->self, r->a);
    else if (r->fn_text) r->fn_text(env, r->self, 1, tl_jni_new_string(r->text));
    free(r);
    return NULL;
}
static void answer_later(struct answer *r)
{
    pthread_t t;
    if (pthread_create(&t, NULL, deliver, r) == 0) pthread_detach(t); else free(r);
}

/* ---------------------------------------------------------------- Brogue CE */

#define BROGUE "org/broguece/game/BrogueActivity"
static void BR_showStartMenu(tl_jcall *c)                              /* (hasSavedGame, ...): New Game */
{
    struct answer *r = calloc(1, sizeof(*r));
    r->fn = game_native("Java_org_broguece_game_BrogueActivity_nativeStartMenuResult");
    r->self = tl_jni_ref(c->self); r->a = 0;
    if (!r->fn) { free(r); return; }
    tl_log_line("sdl: Brogue start menu: new game");
    answer_later(r);
}
static void BR_showTextInput(tl_jcall *c)                              /* (title, default, maxLength, ...): accept the default */
{
    struct answer *r = calloc(1, sizeof(*r));
    r->fn_text = game_native("Java_org_broguece_game_BrogueActivity_nativeTextInputResult");
    r->self = tl_jni_ref(c->self);
    snprintf(r->text, sizeof(r->text), "%s", c->args[1].l ? Str(c->args[1].l) : "");
    if (!r->fn_text) { free(r); return; }
    answer_later(r);
}
static void BR_getInt(tl_jcall *c) { c->ret = vi(c->args[1].i); }       /* a setting not stored: its default */
static const tl_jhle k_brogue[] = {
    { BROGUE, "showStartMenu", "(ZZ)V", BR_showStartMenu },
    { BROGUE, "showTextInputDialog", "(Ljava/lang/String;Ljava/lang/String;IZ)V", BR_showTextInput },
    { BROGUE, "setOverlayVisible", "(Z)V", H_void },
    { BROGUE, "setLoadingVisible", "(Z)V", H_void },
    { BROGUE, "setLoadingProgress", "(I)V", H_void },
    { BROGUE, "setRestoringVisible", "(Z)V", H_void },
    { BROGUE, "getSettingBool", "(Ljava/lang/String;)Z", H_false },
    { BROGUE, "getSettingInt", "(Ljava/lang/String;I)I", BR_getInt },
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------------ Ren'Py */

/*
 * Ren'Py on Android is SDL2 plus embedded CPython, behind org.renpy.android.PythonSDLActivity.
 *
 * SDL_main is (renpy-build/runtime/librenpython3_android.c):
 *
 *     int SDL_main(...) { init_environ(); setenv_workaround("RENPY_PLATFORM", ...);
 *                         call_prepare_python();  return start_python(); }
 *     int start_python(void) {
 *         char *private = getenv("ANDROID_PRIVATE");     // NULL if preparePython did nothing
 *         chdir(private);
 *         snprintf(python, 2048, "%s/python", private);
 *         snprintf(main_py, 2048, "%s/main.py", private);
 *         ...
 *         config.home = Py_DecodeLocale(private, NULL);  // strlen(NULL) -> SIGSEGV
 *     }
 *
 * call_prepare_python() reaches the Java method through JNI (GetObjectClass, GetMethodID,
 * CallVoidMethod). No Java runs here, so it returned zero and did nothing -- and then start_python
 * read a variable that was never set. The two things the Java does, both of which the engine needs
 * before start_python, are done here:
 *
 *   1. unpackData("private", getFilesDir()). The APK carries the engine and its Python in
 *      assets/private.mp3 -- a gzip'd tar whose .mp3 name stops aapt compressing it. It holds
 *      lib/ (the CPython standard library), renpy/ and main.py, and is unpacked into the app's
 *      files directory. Without it there is no interpreter to run even once the crash is gone.
 *   2. nativeSetEnv("ANDROID_PRIVATE", getFilesDir()) and ANDROID_PUBLIC / ANDROID_OLD_PUBLIC /
 *      ANDROID_APK -- the environment Ren'Py's own C reads.
 *
 * Both are done, and the environment is set eagerly when the Ren'Py activity is first seen, so the
 * result does not depend on call_prepare_python() reaching the method.
 */
#define RENPY_ACTIVITY "org/renpy/android/PythonSDLActivity"
#define RENPY_ACTIVITY2 "org/renpy/android/PythonActivity"

extern const char *tl_hle_files_dir(void);
extern const char *tl_hle_data_dir(void);
extern const char *tl_hle_apk_path(void);
extern const char *tl_hle_public_dir(void);

static void rp_mkdirs(const char *path)
{
    char tmp[1300]; snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}
static void rp_setenv(const char *name, const char *value)
{
    if (name && *name && value && *value) setenv(name, value, 1);
}

void tl_renpy_set_env(void)
{
    const char *priv = tl_hle_files_dir();
    const char *pub = tl_hle_public_dir();
    const char *apk = tl_hle_apk_path();
    char path[1400];
    rp_setenv("ANDROID_PRIVATE", priv);
    rp_setenv("ANDROID_PUBLIC", pub);
    rp_setenv("ANDROID_OLD_PUBLIC", pub);
    rp_setenv("ANDROID_APK", apk);
    rp_setenv("ANDROID_ARGUMENT", priv);
    rp_setenv("ANDROID_UNPACK", priv);
    rp_setenv("ANDROID_APP_PATH", priv);
    rp_setenv("PYTHONHOME", priv);
    snprintf(path, sizeof(path), "%s:%s/lib", priv, priv);
    rp_setenv("PYTHONPATH", path);
    rp_setenv("PYTHONOPTIMIZE", "2");
    rp_setenv("RENPY_RENDERER", "gl");
    tl_log_line("renpy: environment set (ANDROID_PRIVATE=%s, ANDROID_APK=%s)", priv, apk);
}

/* ------------------------------------------------- unpackData("private", ...) */

/* gunzip into one buffer. inflateInit2 with 16 in the window bits takes a gzip (or zlib) stream. */
static uint8_t *rp_gunzip(const uint8_t *in, size_t n, size_t *outn)
{
    z_stream zs; memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return NULL;
    size_t cap = n * 6 + (1u << 16), len = 0;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&zs); return NULL; }
    zs.next_in = (Bytef *)in; zs.avail_in = (uInt)n;
    for (;;) {
        if (len == cap) { cap *= 2; uint8_t *t = realloc(out, cap); if (!t) { free(out); inflateEnd(&zs); return NULL; } out = t; }
        zs.next_out = out + len; zs.avail_out = (uInt)(cap - len);
        int rc = inflate(&zs, Z_NO_FLUSH);
        len = cap - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) { free(out); inflateEnd(&zs); return NULL; }
        if (zs.avail_in == 0 && zs.avail_out != 0) { free(out); inflateEnd(&zs); return NULL; }   /* truncated */
    }
    inflateEnd(&zs);
    *outn = len;
    return out;
}

static long rp_octal(const char *p, size_t n)
{
    long v = 0;
    for (size_t i = 0; i < n; i++) { char c = p[i]; if (c < '0' || c > '7') break; v = v * 8 + (c - '0'); }
    return v;
}

/* An entry name is written under `dest`; one that could escape it (absolute, or with "..") is
 * refused. The archive is APK content, which is untrusted input. */
static bool rp_tar_name_ok(const char *name)
{
    if (!name[0] || name[0] == '/') return false;
    for (const char *p = name; *p; ) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - seg);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return false;
        if (*p == '/') p++;
    }
    return true;
}

static int rp_extract_tar(const uint8_t *tar, size_t len, const char *dest)
{
    size_t off = 0;
    int files = 0;
    char longname[1024] = "";
    while (off + 512 <= len) {
        const uint8_t *h = tar + off;
        if (h[0] == 0) break;                                   /* the two zero blocks at the end */
        char name[256];
        size_t nl = 0; while (nl < 100 && h[nl]) nl++;
        snprintf(name, sizeof(name), "%.*s", (int)nl, (const char *)h);
        long sz = rp_octal((const char *)h + 124, 12);
        char type = (char)h[156];
        size_t data_at = off + 512;
        size_t next = data_at + (((size_t)(sz > 0 ? sz : 0) + 511) / 512) * 512;
        if (next > len || next < off) break;
        const char *use = longname[0] ? longname : name;
        longname[0] = 0;
        if (type == 'L') {                                      /* GNU long name record */
            size_t k = (size_t)sz < sizeof(longname) - 1 ? (size_t)sz : sizeof(longname) - 1;
            memcpy(longname, tar + data_at, k); longname[k] = 0;
            off = next; continue;
        }
        if (rp_tar_name_ok(use)) {
            char path[1300];
            snprintf(path, sizeof(path), "%s/%s", dest, use);
            if (type == '5') {
                rp_mkdirs(path);
            } else if (type == '0' || type == 0 || type == '7') {
                char *slash = strrchr(path, '/');
                if (slash) { *slash = 0; rp_mkdirs(path); *slash = '/'; }
                FILE *f = fopen(path, "wb");
                if (f) { if (sz > 0) fwrite(tar + data_at, 1, (size_t)sz, f); fclose(f); files++; }
            }
        }
        off = next;
    }
    return files;
}

/* The whole of unpackData("private", getFilesDir()): find assets/private.mp3 in the APK(s), inflate
 * it, and extract it into the app's files directory. Ren'Py writes a version marker and skips the
 * work when it matches; the marker here is the asset's own size, so an updated APK re-extracts. */
static void rp_unpack_private(void)
{
    const char *dest = tl_hle_files_dir();
    char vpath[1300]; snprintf(vpath, sizeof(vpath), "%s/private.version", dest);
    for (int i = 0;; i++) {
        const tl_zip *z = tl_ld_apk_at(i);
        if (!z) break;
        const tl_zip_entry *e = tl_zip_find(z, "assets/private.mp3");
        if (!e) continue;
        char want[80];
        snprintf(want, sizeof(want), "%llu-%llu", (unsigned long long)e->usize, (unsigned long long)e->csize);
        char have[80] = "";
        FILE *f = fopen(vpath, "r");
        if (f) { if (!fgets(have, sizeof(have), f)) have[0] = 0; fclose(f); }
        size_t n = strlen(have);
        while (n && (have[n - 1] == '\n' || have[n - 1] == '\r')) have[--n] = 0;
        if (!strcmp(have, want)) { tl_log_line("renpy: private data already unpacked (%s)", want); break; }

        const uint8_t *data; size_t dlen; bool owned; char err[160];
        if (!tl_zip_data(z, e, (size_t)1 << 30, &data, &dlen, &owned, err, sizeof(err))) {
            tl_log_line("renpy: cannot read assets/private.mp3: %s", err);
            break;
        }
        size_t tlen = 0;
        uint8_t *tar = rp_gunzip(data, dlen, &tlen);
        if (owned) free((void *)data);
        if (!tar) { tl_log_line("renpy: assets/private.mp3 did not gunzip"); break; }
        rp_mkdirs(dest);
        int files = rp_extract_tar(tar, tlen, dest);
        tl_log_line("renpy: unpacked private data: %d files into %s", files, dest);
        free(tar);
        f = fopen(vpath, "w");  if (f) { fputs(want, f); fclose(f); }
        { char nm[1300]; snprintf(nm, sizeof(nm), "%s/.nomedia", dest); f = fopen(nm, "w"); if (f) fclose(f); }
        break;
    }
}

/* preparePython(): unpackData, then the environment. Called from call_prepare_python() in SDL_main,
 * and once eagerly. */
void tl_renpy_prepare(void)
{
    rp_unpack_private();
    tl_renpy_set_env();
}

static void RP_preparePython(tl_jcall *c) { (void)c; tl_renpy_prepare(); }
static void RP_toast(tl_jcall *c) { tl_log_line("renpy: toast %s", c->args[0].l ? Str(c->args[0].l) : ""); }

static const tl_jhle k_renpy[] = {
    /* The method SDL_main calls, and whose absence is the reported last log line. */
    { RENPY_ACTIVITY, "preparePython", "()V", RP_preparePython },
    { RENPY_ACTIVITY, "toast", "(Ljava/lang/String;)V", RP_toast },
    { RENPY_ACTIVITY2, "preparePython", "()V", RP_preparePython },
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------- Pekka Kana 2 */

#define PK2 "org/pgnapps/pk2/PK2Activity"
static void PK_listDir(tl_jcall *c)                                    /* (path): the entries of an asset folder */
{
    jvalue r; r.j = 0; r.l = tl_assetstream_list(Str(c->args[0].l));
    c->ret = r;
}
static const tl_jhle k_pk2[] = {
    { PK2, "externalPermitted", "()Z", H_false },                      /* no shared storage: levels come from the app */
    { PK2, "listDir", "(Ljava/lang/String;)[Ljava/lang/String;", PK_listDir },
    { "org/libsdl/app/SDLActivity", "setSurfaceViewFormat", "(I)V", H_void },
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------------- LOVE */

/* LOVE (love2d.org) on Android: its GameActivity adds a few screen settings and the message box LOVE shows when the game's Lua fails. */
#define LOVE "org/love2d/android/GameActivity"
static void LV_dpi(tl_jcall *c) { jvalue v; v.j = 0; v.f = 1.0f; c->ret = v; }                 /* the surface is in real pixels */
static void LV_messagebox(tl_jcall *c)                                                          /* (flags, title, message, ...): said in the log, no button pressed */
{
    tl_log_line("love: message box \"%s\": %s", c->args[1].l ? Str(c->args[1].l) : "", c->args[2].l ? Str(c->args[2].l) : "");
    c->ret = vi(0);
}
static void LV_buildFileTree(tl_jcall *c) { jvalue v; v.j = 0; v.l = tl_assetstream_file_tree(); c->ret = v; }
static const tl_jhle k_love[] = {
    { LOVE, "buildFileTree", "()[Ljava/lang/String;", LV_buildFileTree },
    { LOVE, "getMetrics", "()Landroid/util/DisplayMetrics;", tl_sdl_display_metrics },
    { LOVE, "getImmersiveMode", "()Z", H_false },
    { LOVE, "setImmersiveMode", "(Z)V", H_void },
    { LOVE, "getDPIScale", "()F", LV_dpi },
    { LOVE, "messageboxShowMessageBox", "(ILjava/lang/String;Ljava/lang/String;[I[I[Ljava/lang/String;[I)I", LV_messagebox },
    { NULL, NULL, NULL, NULL }
};

/*
 * LuaJIT. Its compiler writes machine code into memory it makes executable, which the phone does not give a guest (only the loader gets executable memory); with the JIT
 * off, LuaJIT runs its interpreter, and that is what a game gets. Each new Lua state is switched off as the standard libraries are opened.
 */
static void (*g_real_openlibs)(void *L);
static int (*g_jit_setmode)(void *L, int idx, int mode);
static void luajit_openlibs(void *L)
{
    if (!g_real_openlibs) g_real_openlibs = tl_ld_sym(NULL, "luaL_openlibs");
    if (!g_jit_setmode) g_jit_setmode = tl_ld_sym(NULL, "luaJIT_setmode");
    if (g_real_openlibs) g_real_openlibs(L);
    if (g_jit_setmode) g_jit_setmode(L, 0, 0 /* LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF */);
}

/* ------------------------------------------------------------------ install */

void tl_sdl_apps_install(const char *package)
{
    if (!strcmp(package, "org.broguece.game")) tl_jni_register_hle(k_brogue);
    if (tl_dexidx_has_class(LOVE)) { tl_jni_register_hle(k_love); tl_ld_interpose("luaL_openlibs", (void *)luajit_openlibs); }
    if (!strcmp(package, "org.pgnapps.pk2")) tl_jni_register_hle(k_pk2);
    if (tl_dexidx_has_class(RENPY_ACTIVITY) || tl_dexidx_has_class(RENPY_ACTIVITY2)) {
        tl_jni_register_hle(k_renpy);
        tl_renpy_prepare();     /* unpackData + the environment, before SDL_main runs start_python */
    }
}
