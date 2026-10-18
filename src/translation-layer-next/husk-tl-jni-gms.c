/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Google Play services, as the translation layer's games see them: present, up to date, and signed out.
 *
 * Play services is Google's own Android app; a game reaches it through the client libraries it ships in its APK
 * (com.google.android.gms.*, com.android.billingclient.*), mostly from C# or C++ through JNI. None of that Java runs here, so
 * the calls that matter are answered in C, with answers a real phone gives a player who has not signed in:
 *
 *   - availability: installed and current (GoogleApiAvailability says SUCCESS), so games that refuse to start without Play
 *     services, or stop to show "Get Google Play services", carry on;
 *   - Play Games: sign-in reports "not authenticated"; achievements, leaderboards and events are accepted and dropped, and the
 *     calls that return a Task get one that has already finished (failed, where a signed-out player's would);
 *   - Play Billing: the connection finishes with BILLING_UNAVAILABLE, the answer for a device whose Play Store cannot sell.
 *
 * A listener the game hands over -- a Unity C# proxy, most often -- is called back with the result, as Play services would.
 * Signing in, cloud saves and purchases themselves would need a Google account and are not here.
 */
#define _DARWIN_C_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-bionic.h"
#include "husk-tl-jni.h"

bool tl_dexidx_has_class(const char *name);
jobj *tl_proxy_call(jobj *proxy, const char *iface, const char *name, const char *sig, jobj **args, int nargs);

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vi(int i) { jvalue v; v.j = 0; v.i = i; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
static void Noop(tl_jcall *c) { (void)c; }
static void RetFalse(tl_jcall *c) { c->ret = vz(0); }
static void RetZero(tl_jcall *c) { c->ret = vi(0); }

/* The Play services version a current phone has (25.13). */
#define GMS_VERSION 251333035

/* Play Asset Delivery's class names (see the section at the bottom of this file), here because the class list below needs them. */
#define PADF  "com/google/android/play/core/assetpacks/AssetPackManagerFactory"
#define PADM  "com/google/android/play/core/assetpacks/AssetPackManager"
#define PADS  "com/google/android/play/core/assetpacks/AssetPackState"
#define PADSS "com/google/android/play/core/assetpacks/AssetPackStates"
#define PADL  "com/google/android/play/core/assetpacks/AssetPackLocation"
#define PADAL "com/google/android/play/core/assetpacks/AssetLocation"
#define PADE  "com/google/android/play/core/assetpacks/AssetPackException"
#define PADNL "com/google/android/play/core/assetpacks/NativeAssetPackStateUpdateListener"

static jobj *singleton(const char *cls)
{
    static struct { const char *cls; jobj *obj; } cache[32];
    for (int i = 0; i < 32; i++) {
        if (cache[i].cls && !strcmp(cache[i].cls, cls)) return cache[i].obj;
        if (!cache[i].cls) { cache[i].cls = cls; cache[i].obj = tl_jni_new_object(tl_jni_class(cls)); return cache[i].obj; }
    }
    return tl_jni_new_object(tl_jni_class(cls));
}

static void once(const char *what)
{
    char note[200]; snprintf(note, sizeof(note), "gms: %s", what);
    tl_note_once(note);
}

/* Call a listener the game handed over: a proxy for C# (Unity's AndroidJavaProxy) goes through the bridge, anything else
 * through its implementation here. */
static void deliver(jobj *listener, const char *iface, const char *method, const char *sig, jobj *arg)
{
    if (!listener) return;
    if (!strcmp(tl_jni_class_name(listener), "java/lang/reflect/Proxy")) { tl_proxy_call(listener, iface, method, sig, &arg, 1); return; }
    jvalue a = vl(arg);
    tl_jni_call(listener, method, sig, &a);
    if (tl_jni_pending()) tl_jni_clear();
}

/* ------------------------------------------------------------------ Tasks */

#define TASK "com/google/android/gms/tasks/Task"

/* A Task that has already finished: with a result, or with an exception. */
static jobj *task_done(jobj *result, const char *error)
{
    jobj *t = tl_jni_new_object(tl_jni_class(TASK));
    tl_jni_set_field(t, "result", "Ljava/lang/Object;", vl(result));
    tl_jni_set_field(t, "successful", "Z", vz(error == NULL));
    if (error) {
        jobj *ex = tl_jni_new_object(tl_jni_class("com/google/android/gms/common/api/ApiException"));
        tl_jni_set_field(ex, "message", "Ljava/lang/String;", vl(tl_jni_new_string(error)));
        tl_jni_set_field(t, "exception", "Ljava/lang/Exception;", vl(ex));
    }
    return t;
}
static jobj *task_failed(void) { return task_done(NULL, "17: API_NOT_CONNECTED (not signed in to Google Play Games)"); }

static bool ok(jobj *t) { return tl_jni_get_field(t, "successful", "Z").z; }
static void Task_isComplete(tl_jcall *c) { c->ret = vz(1); }
static void Task_isSuccessful(tl_jcall *c) { c->ret = vz(ok(c->self)); }
static void Task_getResult(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "result", "Ljava/lang/Object;"); }
static void Task_getException(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "exception", "Ljava/lang/Exception;"); }

/* A listener is told at once: the task has finished. The last argument is the listener in every overload. */
static void call_listener(tl_jcall *c, int nargs, const char *method, const char *sig, bool want_success, bool want_failure)
{
    char iface[80];
    snprintf(iface, sizeof(iface), "com/google/android/gms/tasks/On%c%sListener", method[2], method + 3);
    jobj *listener = c->args[nargs - 1].l;
    c->ret = vl(c->self);
    if (!listener) return;
    bool s = ok(c->self);
    if ((s && !want_success) || (!s && !want_failure)) return;
    jobj *a;
    if (!strcmp(method, "onComplete")) a = c->self;
    else if (!strcmp(method, "onSuccess")) a = tl_jni_get_field(c->self, "result", "Ljava/lang/Object;").l;
    else a = tl_jni_get_field(c->self, "exception", "Ljava/lang/Exception;").l;
    deliver(listener, iface, method, sig, a);
}
#define LISTEN(nm, n, method, sig, s, f) static void nm(tl_jcall *c) { call_listener(c, n, method, sig, s, f); }
LISTEN(Task_onComplete1, 1, "onComplete", "(Lcom/google/android/gms/tasks/Task;)V", true, true)
LISTEN(Task_onComplete2, 2, "onComplete", "(Lcom/google/android/gms/tasks/Task;)V", true, true)
LISTEN(Task_onSuccess1, 1, "onSuccess", "(Ljava/lang/Object;)V", true, false)
LISTEN(Task_onSuccess2, 2, "onSuccess", "(Ljava/lang/Object;)V", true, false)
LISTEN(Task_onFailure1, 1, "onFailure", "(Ljava/lang/Exception;)V", false, true)
LISTEN(Task_onFailure2, 2, "onFailure", "(Ljava/lang/Exception;)V", false, true)

static void ApiException_getMessage(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "message", "Ljava/lang/String;"); }
static void ApiException_getStatusCode(tl_jcall *c) { (void)c; c->ret = vi(17); }

/* ------------------------------------------------------------ availability */

static void GA_getInstance(tl_jcall *c) { c->ret = vl(singleton("com/google/android/gms/common/GoogleApiAvailability")); }
static void GAL_getInstance(tl_jcall *c) { c->ret = vl(singleton("com/google/android/gms/common/GoogleApiAvailabilityLight")); }
static void GA_available(tl_jcall *c) { once("a game asked whether Play services is available: yes"); c->ret = vi(0); }    /* ConnectionResult.SUCCESS */
static void GA_version(tl_jcall *c) { c->ret = vi(GMS_VERSION); }
static void GA_makeAvailable(tl_jcall *c) { c->ret = vl(task_done(NULL, NULL)); }

/* -------------------------------------------------------------- Play Games */

static void PG_signInClient(tl_jcall *c) { c->ret = vl(singleton("com/google/android/gms/games/GamesSignInClient")); }
static void PG_achievements(tl_jcall *c) { c->ret = vl(singleton("com/google/android/gms/games/AchievementsClient")); }
static void PG_leaderboards(tl_jcall *c) { c->ret = vl(singleton("com/google/android/gms/games/LeaderboardsClient")); }
static void PG_players(tl_jcall *c) { c->ret = vl(singleton("com/google/android/gms/games/PlayersClient")); }
static void PG_events(tl_jcall *c) { c->ret = vl(singleton("com/google/android/gms/games/EventsClient")); }
static void PG_snapshots(tl_jcall *c) { c->ret = vl(singleton("com/google/android/gms/games/SnapshotsClient")); }
static void PG_initialize(tl_jcall *c) { (void)c; once("Play Games initialised (signed out)"); }

/* isAuthenticated / signIn: finished, successfully, with "not authenticated" -- what a signed-out player's phone answers. */
static void SignIn_result(tl_jcall *c)
{
    jobj *r = tl_jni_new_object(tl_jni_class("com/google/android/gms/games/AuthenticationResult"));
    c->ret = vl(task_done(r, NULL));
}
static void Auth_isAuthenticated(tl_jcall *c) { c->ret = vz(0); }
static void Task_failed(tl_jcall *c) { c->ret = vl(task_failed()); }

/* ----------------------------------------------------------------- Billing */

#define BC "com/android/billingclient/api/BillingClient"
#define BCB "com/android/billingclient/api/BillingClient$Builder"
#define BR "com/android/billingclient/api/BillingResult"
#define PPP "com/android/billingclient/api/PendingPurchasesParams"
#define PPPB "com/android/billingclient/api/PendingPurchasesParams$Builder"
#define BILLING_UNAVAILABLE 3

static jobj *billing_result(int code, const char *message)
{
    jobj *r = tl_jni_new_object(tl_jni_class(BR));
    tl_jni_set_field(r, "code", "I", vi(code));
    tl_jni_set_field(r, "message", "Ljava/lang/String;", vl(tl_jni_new_string(message)));
    return r;
}
static void BC_newBuilder(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class(BCB))); }
static void BCB_self(tl_jcall *c) { c->ret = vl(c->self); }
static void PPP_newBuilder(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class(PPPB))); }
static void PPPB_build(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class(PPP))); }
static void BCB_build(tl_jcall *c) { c->ret = vl(tl_jni_new_object(tl_jni_class(BC))); }
static void BC_startConnection(tl_jcall *c)
{
    once("Play Billing: connection finished with BILLING_UNAVAILABLE");
    deliver(c->args[0].l, "com/android/billingclient/api/BillingClientStateListener", "onBillingSetupFinished",
            "(Lcom/android/billingclient/api/BillingResult;)V", billing_result(BILLING_UNAVAILABLE, "Google Play Billing is not available in Husk"));
}
static void BC_isFeatureSupported(tl_jcall *c) { c->ret = vl(billing_result(BILLING_UNAVAILABLE, "")); }
static void BR_code(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "code", "I"); }
static void BR_message(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "message", "Ljava/lang/String;"); }

/* ------------------------------------------------------- Firebase's helpers */

/*
 * Firebase's C++ SDK (Unity's Firebase plugin is built on it) carries a few Java classes of its own, which it writes out as a
 * dex at start and loads through a DexClassLoader. Without them Firebase stops at "Java class ... Log not found" and nothing
 * of it works, the availability check included. They are glue between Java callbacks and its C++, so here they are, in C:
 * results are handed to the natives Firebase registered on these classes.
 */
#define FB "com/google/firebase/app/internal/cpp/"

#define GLVR "com/google/firebase/platforminfo/GlobalLibraryVersionRegistrar"
static void GLVR_getInstance(tl_jcall *c) { c->ret = vl(singleton(GLVR)); }
static void FBLog_getInstance(tl_jcall *c) { c->ret = vl(singleton(FB "Log")); }
static void FBLog_line(tl_jcall *c)
{
    const char *tag = tl_jni_string(c->args[0].l), *msg = tl_jni_string(c->args[1].l);
    tl_log_line("firebase/%s: %s", tag ? tag : "?", msg ? msg : "");
    c->ret = vi(0);
}

/* CppThreadDispatcherContext: a C++ function and its data, run on the thread asked for. */
typedef void (*fb_fn_native)(void *env, jobj *self, int64_t fn, int64_t data);
static void Ctx_init(tl_jcall *c)
{
    tl_jni_set_field(c->self, "functionPtr", "J", c->args[0]);
    tl_jni_set_field(c->self, "functionData", "J", c->args[1]);
    tl_jni_set_field(c->self, "cancelFunctionPtr", "J", c->args[2]);
}
static void ctx_execute(jobj *ctx)
{
    fb_fn_native f = tl_jni_native(FB "CppThreadDispatcherContext", "nativeFunction", "(JJ)V");
    int64_t fn = tl_jni_get_field(ctx, "functionPtr", "J").j;
    if (!f || !fn || tl_jni_get_field(ctx, "done", "Z").z) return;
    tl_jni_set_field(ctx, "done", "Z", vz(1));
    f(tl_jni_env(), ctx, fn, tl_jni_get_field(ctx, "functionData", "J").j);
    if (tl_jni_pending()) tl_jni_clear();
}
static void Ctx_execute(tl_jcall *c) { ctx_execute(c->self); }
static void Ctx_cancel(tl_jcall *c) { tl_jni_set_field(c->self, "done", "Z", vz(1)); }
static void Ctx_acquire(tl_jcall *c) { c->ret = vz(!tl_jni_get_field(c->self, "done", "Z").z); }
static void *ctx_thread(void *arg) { ctx_execute(arg); tl_jni_unref(arg); return NULL; }
static void Dispatch_main(tl_jcall *c) { ctx_execute(c->args[1].l); }
static void Dispatch_background(tl_jcall *c)
{
    pthread_t t;
    jobj *ctx = tl_jni_ref(c->args[0].l);
    if (pthread_create(&t, NULL, ctx_thread, ctx) == 0) pthread_detach(t); else { ctx_execute(ctx); tl_jni_unref(ctx); }
}

/* JniResultCallback: a Task's result for a C++ future. The Tasks here have finished already, so the answer goes at once. */
typedef void (*fb_result_native)(void *env, jobj *self, jobj *result, uint8_t success, uint8_t cancelled, jobj *status, int64_t fn, int64_t data);
static void jrc_deliver(jobj *self, jobj *result, bool success, bool cancelled, const char *status)
{
    fb_result_native f = tl_jni_native(FB "JniResultCallback", "nativeOnResult", "(Ljava/lang/Object;ZZLjava/lang/String;JJ)V");
    int64_t fn = tl_jni_get_field(self, "callbackFn", "J").j;
    if (!f || !fn || tl_jni_get_field(self, "done", "Z").z) return;
    tl_jni_set_field(self, "done", "Z", vz(1));
    f(tl_jni_env(), self, result, success, cancelled, status ? tl_jni_new_string(status) : NULL, fn, tl_jni_get_field(self, "callbackData", "J").j);
    if (tl_jni_pending()) tl_jni_clear();
}
static void JRC_init2(tl_jcall *c)
{
    tl_jni_set_field(c->self, "callbackFn", "J", c->args[0]);
    tl_jni_set_field(c->self, "callbackData", "J", c->args[1]);
}
static void JRC_init3(tl_jcall *c)
{
    jobj *task = c->args[0].l;
    tl_jni_set_field(c->self, "callbackFn", "J", c->args[1]);
    tl_jni_set_field(c->self, "callbackData", "J", c->args[2]);
    if (!task || strcmp(tl_jni_class_name(task), TASK)) { jrc_deliver(c->self, NULL, false, false, "Google Play services task not available in Husk"); return; }
    jobj *ex = tl_jni_get_field(task, "exception", "Ljava/lang/Exception;").l;
    jrc_deliver(c->self, tl_jni_get_field(task, "result", "Ljava/lang/Object;").l, ok(task), false,
                ex ? tl_jni_string(tl_jni_get_field(ex, "message", "Ljava/lang/String;").l) : NULL);
}
static void JRC_cancel(tl_jcall *c) { jrc_deliver(c->self, NULL, false, true, "cancelled"); }

/* GoogleApiAvailabilityHelper.makeGooglePlayServicesAvailable: there is nothing to make available; it says so straight away. */
static void GAH_make(tl_jcall *c)
{
    typedef void (*done_fn)(void *env, jobj *cls, int code, jobj *msg);
    done_fn f = tl_jni_native(FB "GoogleApiAvailabilityHelper", "onCompleteNative", "(ILjava/lang/String;)V");
    if (f) { f(tl_jni_env(), tl_jni_class_object(FB "GoogleApiAvailabilityHelper"), 0, tl_jni_new_string("")); if (tl_jni_pending()) tl_jni_clear(); }
    c->ret = vz(1);
}

/* ------------------------------------------------------------------ tables */

static const struct { const char *name, *super; } k_classes[] = {
    { "dalvik/system/BaseDexClassLoader", "java/lang/ClassLoader" }, { "dalvik/system/DexClassLoader", "dalvik/system/BaseDexClassLoader" },
    { TASK, "java/lang/Object" },
    { "com/google/android/gms/common/api/ApiException", "java/lang/Exception" },
    { "com/google/android/gms/common/GoogleApiAvailabilityLight", "java/lang/Object" },
    { "com/google/android/gms/common/GoogleApiAvailability", "com/google/android/gms/common/GoogleApiAvailabilityLight" },
    { "com/google/android/gms/common/GooglePlayServicesUtilLight", "java/lang/Object" },
    { "com/google/android/gms/common/GooglePlayServicesUtil", "com/google/android/gms/common/GooglePlayServicesUtilLight" },
    { "com/google/android/gms/games/PlayGamesSdk", "java/lang/Object" }, { "com/google/android/gms/games/PlayGames", "java/lang/Object" },
    { "com/google/android/gms/games/GamesSignInClient", "java/lang/Object" },
    { "com/google/android/gms/games/AuthenticationResult", "java/lang/Object" },
    { "com/google/android/gms/games/AchievementsClient", "java/lang/Object" },
    { "com/google/android/gms/games/LeaderboardsClient", "java/lang/Object" },
    { "com/google/android/gms/games/PlayersClient", "java/lang/Object" },
    { "com/google/android/gms/games/EventsClient", "java/lang/Object" },
    { "com/google/android/gms/games/SnapshotsClient", "java/lang/Object" },
    { FB "Log", "java/lang/Object" }, { FB "CppThreadDispatcher", "java/lang/Object" },
    { FB "CppThreadDispatcherContext", "java/lang/Object" }, { FB "JniResultCallback", "java/lang/Object" },
    { FB "GoogleApiAvailabilityHelper", "java/lang/Object" },
    { PPP, "java/lang/Object" }, { PPPB, "java/lang/Object" },
    { BC, "java/lang/Object" }, { BCB, "java/lang/Object" }, { BR, "java/lang/Object" },
    { PADF, "java/lang/Object" }, { PADM, "java/lang/Object" }, { PADS, "java/lang/Object" },
    { PADSS, "java/lang/Object" }, { PADL, "java/lang/Object" }, { PADE, "java/lang/Exception" },
    { PADNL, "java/lang/Object" },
};

/* ------------------------------------------------------------ class loader */

/*
 * ClassLoader.loadClass and friends, for every engine: native code reaches the game's own Java -- Firebase's and Play
 * services' among it -- through the activity's class loader by dotted name, as FindClass sees only the system's. The class
 * is there if the framework, the APK, or the runtime itself has it.
 */
static void CL_loadClass(tl_jcall *c)
{
    char name[300]; snprintf(name, sizeof(name), "%s", tl_jni_string(c->args[0].l) ? tl_jni_string(c->args[0].l) : "");
    for (char *p = name; *p; p++) if (*p == '.') *p = '/';
    bool framework = !strncmp(name, "android/", 8) || !strncmp(name, "java/", 5) || !strncmp(name, "javax/", 6)
                  || !strncmp(name, "dalvik/", 7) || !strncmp(name, "org/json/", 9);
    bool ours = false;
    for (size_t i = 0; i < sizeof(k_classes) / sizeof(k_classes[0]); i++) if (!strcmp(k_classes[i].name, name)) ours = true;
    if (name[0] && (framework || ours || tl_dexidx_has_class(name))) { c->ret = vl(tl_jni_class_object(name)); return; }
    tl_jni_throw("java/lang/ClassNotFoundException", name);
    c->ret = vl(NULL);
}
static void CL_loadClass2(tl_jcall *c) { CL_loadClass(c); }      /* loadClass(String, boolean resolve) */
/* new DexClassLoader(path, optimizedDir, libraryPath, parent): the classes it would add are looked up in the APK's anyway. */
static void CL_init(tl_jcall *c) { (void)c; }

/* ------------------------------------------------- Google Play Asset Delivery */

/*
 * Play's asset packs, as a game packaged for them sees them.
 *
 * GTA: San Andreas Definitive Edition delivers its paks this way, and the game checks before it will start: its first map
 * is a DownloadMenu that asks Play which packs of its own manifest are installed, requests the ones that are not, and only
 * goes on when they are all there. There is no Play service on iOS and no other source for that content, so with nothing
 * answering, the game sits at "starting the download" forever -- the download belongs to Play and Play is not here.
 *
 * The paks themselves are on the device: they are inside the APK's OBB, which the engine mounts, and this answers Play's
 * questions the way a phone whose packs had already been installed would -- every pack complete, at the folder the engine
 * looks for paks in (<external files>/UE4Game/<Project>/<Project>/Content/Paks, served by that same OBB), every request a
 * Task that has finished. A pack this APK genuinely does not carry still fails when its pak is mounted, and the log says
 * which one: that is a fact about the file, not something to answer around.
 */
#define PAD_COMPLETED     4      /* AssetPackStatus.COMPLETED */
#define PAD_STORAGE_FILES 2      /* AssetPackStorageMethod.STORAGE_FILES */
#define PAD_SEEN_MAX      64

const char *tl_hle_ext_files(void);
const char *tl_ue4_meta(const char *key);

static jvalue vj(int64_t j) { jvalue v; v.j = j; return v; }
static const char *S_(const jobj *o) { const char *s = tl_jni_string(o); return s ? s : ""; }

/* The folder the engine scans for paks -- and the one the OBB is mounted over, so the paks really are there. */
static const char *pad_pak_dir(void)
{
    static char dir[1000];
    if (!dir[0]) {
        const char *p = tl_ue4_meta("com.epicgames.ue4.GameActivity.ProjectName");
        if (!p || !p[0]) p = "Gameface";
        snprintf(dir, sizeof(dir), "%s/UE4Game/%s/%s/Content/Paks", tl_hle_ext_files(), p, p);
    }
    return dir;
}

/* Every pack the game has named, so getPackLocations() can answer for all of them. */
static struct { char name[80]; } g_pad[PAD_SEEN_MAX];
static int g_pad_n;
static jobj *g_pad_listener;

static void pad_note(const char *pack)
{
    if (!pack || !pack[0]) return;
    for (int i = 0; i < g_pad_n; i++) if (!strcmp(g_pad[i].name, pack)) return;
    if (g_pad_n < PAD_SEEN_MAX) snprintf(g_pad[g_pad_n++].name, sizeof(g_pad[0].name), "%s", pack);
}

/* A pack list as the plugin hands it over: a String[] (the usual), or a List if a game built one. */
static int pad_count(jobj *l)
{
    if (!l) return 0;
    if (l->kind == TL_K_OBJ_ARRAY && l->oarr.v) return (int)l->oarr.len;
    if (tl_jni_pending()) tl_jni_clear();
    jvalue r = tl_jni_call(l, "size", "()I", NULL);
    if (tl_jni_pending()) tl_jni_clear();
    return r.i > 0 && r.i < 4096 ? r.i : 0;
}
static jobj *pad_at(jobj *l, int i)
{
    if (!l) return NULL;
    if (l->kind == TL_K_OBJ_ARRAY && l->oarr.v) return i < (int)l->oarr.len ? l->oarr.v[i] : NULL;
    jvalue a = vi(i);
    jobj *o = tl_jni_call(l, "get", "(I)Ljava/lang/Object;", &a).l;
    if (tl_jni_pending()) tl_jni_clear();
    return o;
}

/* One pack, complete: what its getters hand back. */
static jobj *pad_state(const char *pack)
{
    jobj *s = tl_jni_new_object(tl_jni_class(PADS));
    tl_jni_set_field(s, "name", "Ljava/lang/String;", vl(tl_jni_new_string(pack)));
    tl_jni_set_field(s, "status", "I", vi(PAD_COMPLETED));
    tl_jni_set_field(s, "errorCode", "I", vi(0));
    tl_jni_set_field(s, "bytesDownloaded", "J", vj(0));
    tl_jni_set_field(s, "totalBytesToDownload", "J", vj(0));
    tl_jni_set_field(s, "transferProgressPercentage", "I", vi(100));   /* finished, so 100% */
    tl_jni_set_field(s, "installedVersionTag", "Ljava/lang/String;", vl(tl_jni_new_string("")));
    tl_jni_set_field(s, "availableVersionTag", "Ljava/lang/String;", vl(tl_jni_new_string("")));
    tl_jni_set_field(s, "updateAvailability", "I", vi(0));             /* no update available: the pack is current */
    return s;
}

/* Where its paks are: the folder the engine already finds them in. */
static jobj *pad_location(void)
{
    jobj *l = tl_jni_new_object(tl_jni_class(PADL));
    tl_jni_set_field(l, "path", "Ljava/lang/String;", vl(tl_jni_new_string(pad_pak_dir())));
    tl_jni_set_field(l, "assetsPath", "Ljava/lang/String;", vl(tl_jni_new_string(pad_pak_dir())));
    tl_jni_set_field(l, "packStorageMethod", "I", vi(PAD_STORAGE_FILES));
    return l;
}

static jobj *pad_map(jobj **vals, const char **names, int n)
{
    jobj *m = tl_jni_new_object(tl_jni_class("java/util/HashMap"));
    for (int i = 0; i < n; i++) {
        jvalue a[2];
        a[0] = vl(tl_jni_new_string(names[i]));
        a[1] = vl(vals[i]);
        tl_jni_call(m, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", a);
    }
    if (tl_jni_pending()) tl_jni_clear();
    return m;
}

/* AssetPackStates over the packs a call named. */
static jobj *pad_states(jobj *names)
{
    char buf[24][80];
    const char *nm[24];
    jobj *st[24];
    int n = pad_count(names);
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++) {
        const char *s = tl_jni_string(pad_at(names, i));
        snprintf(buf[i], sizeof(buf[i]), "%s", s && s[0] ? s : "?");
        nm[i] = buf[i];
        pad_note(nm[i]);
        st[i] = pad_state(nm[i]);
    }
    jobj *ss = tl_jni_new_object(tl_jni_class(PADSS));
    tl_jni_set_field(ss, "states", "Ljava/util/Map;", vl(pad_map(st, nm, n)));
    tl_jni_set_field(ss, "totalBytes", "J", vj(0));
    return ss;
}

/* The game's own listener, if it registered one, is told too: its onStateUpdate is a native the engine registered. */
static void pad_tell(const char *pack)
{
    static bool said_no_native;
    if (!g_pad_listener) return;
    typedef void (*cb)(void *, jobj *, jobj *);
    cb fn = (cb)tl_jni_native(PADNL, "onStateUpdate", "(Lcom/google/android/play/core/assetpacks/AssetPackState;)V");
    if (!fn) { if (!said_no_native) { said_no_native = true; tl_log_line("pad: the engine registered no onStateUpdate native; listeners are not called"); } return; }
    fn(tl_jni_env(), g_pad_listener, pad_state(pack));
}

static void pad_names_say(const char *what, jobj *names)
{
    char list[700];
    size_t at = 0;
    list[0] = 0;
    int n = pad_count(names);
    for (int i = 0; i < n && at < 600; i++) {
        const char *s = tl_jni_string(pad_at(names, i));
        at += (size_t)snprintf(list + at, sizeof(list) - at, "%s%s", i ? ", " : "", s ? s : "?");
    }
    tl_log_line("pad: %s%d pack(s): %s -- all complete, paks at %s", what, n, list[0] ? list : "(none)", pad_pak_dir());
}

static void pad_getInstance(tl_jcall *c) { c->ret = vl(singleton(PADM)); }
static void pad_register(tl_jcall *c)
{
    g_pad_listener = c->args[0].l;
    tl_log_line("pad: registerListener(%s) -- every pack is already final, so no update is coming", g_pad_listener ? tl_jni_class_name(g_pad_listener) : "(null)");
}
static void pad_unregister(tl_jcall *c) { if (c->args[0].l == g_pad_listener) g_pad_listener = NULL; }
static void pad_clear(tl_jcall *c) { (void)c; g_pad_listener = NULL; }
static void pad_getPackStates(tl_jcall *c) { pad_names_say("getPackStates: ", c->args[0].l); c->ret = vl(task_done(pad_states(c->args[0].l), NULL)); }
static void pad_fetch(tl_jcall *c)
{
    pad_names_say("fetch: ", c->args[0].l);
    c->ret = vl(task_done(pad_states(c->args[0].l), NULL));
    for (int i = 0, n = pad_count(c->args[0].l); i < n; i++) { const char *s = tl_jni_string(pad_at(c->args[0].l, i)); if (s) pad_tell(s); }
}
static void pad_cancel(tl_jcall *c) { pad_names_say("cancel: ", c->args[0].l); c->ret = vl(pad_states(c->args[0].l)); }
static void pad_removePack(tl_jcall *c)
{
    tl_log_line("pad: removePack(%s) -- refused: the content is the app's own", S_(c->args[0].l));
    c->ret = vl(task_done(NULL, "the pack is part of the app"));
}
static void pad_getPackLocation(tl_jcall *c)
{
    const char *n = S_(c->args[0].l);
    pad_note(n);
    tl_log_line("pad: getPackLocation(%s) -> %s", n, pad_pak_dir());
    c->ret = vl(pad_location());
}
static void pad_getPackLocations(tl_jcall *c)
{
    jobj *v[PAD_SEEN_MAX];
    const char *nm[PAD_SEEN_MAX];
    for (int i = 0; i < g_pad_n; i++) { nm[i] = g_pad[i].name; v[i] = pad_location(); }
    tl_log_line("pad: getPackLocations() -> %d pack(s) at %s", g_pad_n, pad_pak_dir());
    c->ret = vl(pad_map(v, nm, g_pad_n));
}
static void pad_getAssetLocation(tl_jcall *c)
{
    tl_log_line("pad: getAssetLocation(%s, %s)", S_(c->args[0].l), S_(c->args[1].l));
    jobj *a = tl_jni_new_object(tl_jni_class(PADAL));
    tl_jni_set_field(a, "path", "Ljava/lang/String;", vl(tl_jni_new_string(pad_pak_dir())));
    tl_jni_set_field(a, "offset", "J", vj(0));
    tl_jni_set_field(a, "size", "J", vj(0));
    c->ret = vl(a);
}
/* The confirmation dialogs Play shows before a download over cellular: there is nothing to download, so they are answered. */
static void pad_confirm_task(tl_jcall *c) { (void)c; c->ret = vl(task_done(NULL, NULL)); }
static void pad_confirm_bool(tl_jcall *c) { (void)c; c->ret = vz(1); }

static void PADS_name(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "name", "Ljava/lang/String;"); }
static void PADS_status(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "status", "I"); }
static void PADS_error(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "errorCode", "I"); }
static void PADS_bytes(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "bytesDownloaded", "J"); }
static void PADS_total(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "totalBytesToDownload", "J"); }
static void PADS_progress(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "transferProgressPercentage", "I"); }
static void PADS_update(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "updateAvailability", "I"); }
static void PADS_installed(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "installedVersionTag", "Ljava/lang/String;"); }
static void PADS_available(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "availableVersionTag", "Ljava/lang/String;"); }
static void PADS_toString(tl_jcall *c)
{
    char b[220];
    snprintf(b, sizeof(b), "AssetPackState{%s, COMPLETED}", S_(tl_jni_get_field(c->self, "name", "Ljava/lang/String;").l));
    c->ret = vl(tl_jni_new_string(b));
}
static void PADSS_packStates(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "states", "Ljava/util/Map;"); }
static void PADSS_totalBytes(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "totalBytes", "J"); }
static void PADSS_toString(tl_jcall *c) { c->ret = vl(tl_jni_new_string("AssetPackStates{complete}")); }
static void PADL_path(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "path", "Ljava/lang/String;"); }
static void PADL_assetsPath(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "assetsPath", "Ljava/lang/String;"); }
static void PADL_method(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "packStorageMethod", "I"); }
static void PADL_toString(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "path", "Ljava/lang/String;"); }
static void PADAL_path(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "path", "Ljava/lang/String;"); }
static void PADAL_offset(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "offset", "J"); }
static void PADAL_size(tl_jcall *c) { c->ret = tl_jni_get_field(c->self, "size", "J"); }
static void PADE_getErrorCode(tl_jcall *c) { (void)c; c->ret = vi(0); }    /* AssetPackErrorCode: NO_ERROR */
static void PADE_toString(tl_jcall *c) { (void)c; c->ret = vl(tl_jni_new_string("AssetPackException{no error}")); }

#define M(c, n, s, f) { c, n, s, f }
#define AC "com/google/android/gms/games/AchievementsClient"
#define LC "com/google/android/gms/games/LeaderboardsClient"
static const tl_jhle k_gms[] = {
    /* class loaders */
#define CLS_ "(Ljava/lang/String;)Ljava/lang/Class;"
#define CLSZ "(Ljava/lang/String;Z)Ljava/lang/Class;"
    M("java/lang/ClassLoader", "loadClass", CLS_, CL_loadClass), M("java/lang/ClassLoader", "loadClass", CLSZ, CL_loadClass2),
    M("java/lang/ClassLoader", "findClass", CLS_, CL_loadClass), M("java/lang/ClassLoader", "findLoadedClass", CLS_, CL_loadClass),
    M("dalvik/system/BaseDexClassLoader", "findClass", CLS_, CL_loadClass),
    M("dalvik/system/PathClassLoader", "loadClass", CLS_, CL_loadClass), M("dalvik/system/PathClassLoader", "findClass", CLS_, CL_loadClass),
    M("dalvik/system/DexClassLoader", "loadClass", CLS_, CL_loadClass),
    M("dalvik/system/DexClassLoader", "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V", CL_init),

    /* Task */
    M(TASK, "isComplete", "()Z", Task_isComplete), M(TASK, "isSuccessful", "()Z", Task_isSuccessful),
    M(TASK, "isCanceled", "()Z", RetFalse),
    M(TASK, "getResult", "()Ljava/lang/Object;", Task_getResult), M(TASK, "getException", "()Ljava/lang/Exception;", Task_getException),
    M(TASK, "addOnCompleteListener", "(Lcom/google/android/gms/tasks/OnCompleteListener;)Lcom/google/android/gms/tasks/Task;", Task_onComplete1),
    M(TASK, "addOnCompleteListener", "(Landroid/app/Activity;Lcom/google/android/gms/tasks/OnCompleteListener;)Lcom/google/android/gms/tasks/Task;", Task_onComplete2),
    M(TASK, "addOnCompleteListener", "(Ljava/util/concurrent/Executor;Lcom/google/android/gms/tasks/OnCompleteListener;)Lcom/google/android/gms/tasks/Task;", Task_onComplete2),
    M(TASK, "addOnSuccessListener", "(Lcom/google/android/gms/tasks/OnSuccessListener;)Lcom/google/android/gms/tasks/Task;", Task_onSuccess1),
    M(TASK, "addOnSuccessListener", "(Landroid/app/Activity;Lcom/google/android/gms/tasks/OnSuccessListener;)Lcom/google/android/gms/tasks/Task;", Task_onSuccess2),
    M(TASK, "addOnSuccessListener", "(Ljava/util/concurrent/Executor;Lcom/google/android/gms/tasks/OnSuccessListener;)Lcom/google/android/gms/tasks/Task;", Task_onSuccess2),
    M(TASK, "addOnFailureListener", "(Lcom/google/android/gms/tasks/OnFailureListener;)Lcom/google/android/gms/tasks/Task;", Task_onFailure1),
    M(TASK, "addOnFailureListener", "(Landroid/app/Activity;Lcom/google/android/gms/tasks/OnFailureListener;)Lcom/google/android/gms/tasks/Task;", Task_onFailure2),
    M(TASK, "addOnFailureListener", "(Ljava/util/concurrent/Executor;Lcom/google/android/gms/tasks/OnFailureListener;)Lcom/google/android/gms/tasks/Task;", Task_onFailure2),
    M("com/google/android/gms/common/api/ApiException", "getMessage", "()Ljava/lang/String;", ApiException_getMessage),
    M("com/google/android/gms/common/api/ApiException", "getStatusCode", "()I", ApiException_getStatusCode),

    /* availability */
    M("com/google/android/gms/common/GoogleApiAvailability", "getInstance", "()Lcom/google/android/gms/common/GoogleApiAvailability;", GA_getInstance),
    M("com/google/android/gms/common/GoogleApiAvailabilityLight", "getInstance", "()Lcom/google/android/gms/common/GoogleApiAvailabilityLight;", GAL_getInstance),
    M("com/google/android/gms/common/GoogleApiAvailabilityLight", "isGooglePlayServicesAvailable", "(Landroid/content/Context;)I", GA_available),
    M("com/google/android/gms/common/GoogleApiAvailabilityLight", "isGooglePlayServicesAvailable", "(Landroid/content/Context;I)I", GA_available),
    M("com/google/android/gms/common/GoogleApiAvailabilityLight", "getApkVersion", "(Landroid/content/Context;)I", GA_version),
    M("com/google/android/gms/common/GoogleApiAvailabilityLight", "isUserResolvableError", "(I)Z", RetFalse),
    M("com/google/android/gms/common/GoogleApiAvailability", "makeGooglePlayServicesAvailable", "(Landroid/app/Activity;)Lcom/google/android/gms/tasks/Task;", GA_makeAvailable),
    M("com/google/android/gms/common/GooglePlayServicesUtilLight", "isGooglePlayServicesAvailable", "(Landroid/content/Context;)I", GA_available),
    M("com/google/android/gms/common/GooglePlayServicesUtilLight", "isGooglePlayServicesAvailable", "(Landroid/content/Context;I)I", GA_available),
    M("com/google/android/gms/common/GooglePlayServicesUtilLight", "getApkVersion", "(Landroid/content/Context;)I", GA_version),

    /* Play Games v2 */
    M("com/google/android/gms/games/PlayGamesSdk", "initialize", "(Landroid/content/Context;)V", PG_initialize),
    M("com/google/android/gms/games/PlayGames", "getGamesSignInClient", "(Landroid/app/Activity;)Lcom/google/android/gms/games/GamesSignInClient;", PG_signInClient),
    M("com/google/android/gms/games/PlayGames", "getAchievementsClient", "(Landroid/app/Activity;)Lcom/google/android/gms/games/AchievementsClient;", PG_achievements),
    M("com/google/android/gms/games/PlayGames", "getLeaderboardsClient", "(Landroid/app/Activity;)Lcom/google/android/gms/games/LeaderboardsClient;", PG_leaderboards),
    M("com/google/android/gms/games/PlayGames", "getPlayersClient", "(Landroid/app/Activity;)Lcom/google/android/gms/games/PlayersClient;", PG_players),
    M("com/google/android/gms/games/PlayGames", "getEventsClient", "(Landroid/app/Activity;)Lcom/google/android/gms/games/EventsClient;", PG_events),
    M("com/google/android/gms/games/PlayGames", "getSnapshotsClient", "(Landroid/app/Activity;)Lcom/google/android/gms/games/SnapshotsClient;", PG_snapshots),
    M("com/google/android/gms/games/GamesSignInClient", "isAuthenticated", "()Lcom/google/android/gms/tasks/Task;", SignIn_result),
    M("com/google/android/gms/games/GamesSignInClient", "signIn", "()Lcom/google/android/gms/tasks/Task;", SignIn_result),
    M("com/google/android/gms/games/GamesSignInClient", "requestServerSideAccess", "(Ljava/lang/String;Z)Lcom/google/android/gms/tasks/Task;", Task_failed),
    M("com/google/android/gms/games/AuthenticationResult", "isAuthenticated", "()Z", Auth_isAuthenticated),
    M(AC, "unlock", "(Ljava/lang/String;)V", Noop), M(AC, "increment", "(Ljava/lang/String;I)V", Noop),
    M(AC, "reveal", "(Ljava/lang/String;)V", Noop), M(AC, "setSteps", "(Ljava/lang/String;I)V", Noop),
    M(AC, "unlockImmediate", "(Ljava/lang/String;)Lcom/google/android/gms/tasks/Task;", Task_failed),
    M(AC, "incrementImmediate", "(Ljava/lang/String;I)Lcom/google/android/gms/tasks/Task;", Task_failed),
    M(AC, "revealImmediate", "(Ljava/lang/String;)Lcom/google/android/gms/tasks/Task;", Task_failed),
    M(AC, "setStepsImmediate", "(Ljava/lang/String;I)Lcom/google/android/gms/tasks/Task;", Task_failed),
    M(AC, "load", "(Z)Lcom/google/android/gms/tasks/Task;", Task_failed),
    M(AC, "getAchievementsIntent", "()Lcom/google/android/gms/tasks/Task;", Task_failed),
    M(LC, "submitScore", "(Ljava/lang/String;J)V", Noop), M(LC, "submitScore", "(Ljava/lang/String;JLjava/lang/String;)V", Noop),
    M(LC, "submitScoreImmediate", "(Ljava/lang/String;J)Lcom/google/android/gms/tasks/Task;", Task_failed),
    M(LC, "getAllLeaderboardsIntent", "()Lcom/google/android/gms/tasks/Task;", Task_failed),
    M(LC, "getLeaderboardIntent", "(Ljava/lang/String;)Lcom/google/android/gms/tasks/Task;", Task_failed),
    M("com/google/android/gms/games/PlayersClient", "getCurrentPlayer", "()Lcom/google/android/gms/tasks/Task;", Task_failed),
    M("com/google/android/gms/games/PlayersClient", "getCurrentPlayerId", "()Lcom/google/android/gms/tasks/Task;", Task_failed),
    M("com/google/android/gms/games/EventsClient", "increment", "(Ljava/lang/String;I)V", Noop),

    /* Firebase's helpers */
    M(GLVR, "getInstance", "()Lcom/google/firebase/platforminfo/GlobalLibraryVersionRegistrar;", GLVR_getInstance),
    M(GLVR, "registerVersion", "(Ljava/lang/String;Ljava/lang/String;)V", Noop),
    M(FB "Log", "getInstance", "()Lcom/google/firebase/app/internal/cpp/Log;", FBLog_getInstance), M(FB "Log", "shutdown", "()V", Noop),
    M(FB "Log", "v", "(Ljava/lang/String;Ljava/lang/String;)I", FBLog_line), M(FB "Log", "d", "(Ljava/lang/String;Ljava/lang/String;)I", FBLog_line),
    M(FB "Log", "i", "(Ljava/lang/String;Ljava/lang/String;)I", FBLog_line), M(FB "Log", "w", "(Ljava/lang/String;Ljava/lang/String;)I", FBLog_line),
    M(FB "Log", "e", "(Ljava/lang/String;Ljava/lang/String;)I", FBLog_line), M(FB "Log", "wtf", "(Ljava/lang/String;Ljava/lang/String;)I", FBLog_line),
    M(FB "CppThreadDispatcherContext", "<init>", "(JJJ)V", Ctx_init), M(FB "CppThreadDispatcherContext", "execute", "()V", Ctx_execute),
    M(FB "CppThreadDispatcherContext", "cancel", "()V", Ctx_cancel), M(FB "CppThreadDispatcherContext", "clear", "()V", Ctx_cancel),
    M(FB "CppThreadDispatcherContext", "acquireExecuteCancelLock", "()Z", Ctx_acquire),
    M(FB "CppThreadDispatcherContext", "releaseExecuteCancelLock", "()V", Noop),
    M(FB "CppThreadDispatcher", "runOnMainThread", "(Landroid/app/Activity;Lcom/google/firebase/app/internal/cpp/CppThreadDispatcherContext;)V", Dispatch_main),
    M(FB "CppThreadDispatcher", "runOnBackgroundThread", "(Lcom/google/firebase/app/internal/cpp/CppThreadDispatcherContext;)V", Dispatch_background),
    M(FB "JniResultCallback", "<init>", "(JJ)V", JRC_init2),
    M(FB "JniResultCallback", "<init>", "(Lcom/google/android/gms/tasks/Task;JJ)V", JRC_init3),
    M(FB "JniResultCallback", "cancel", "()V", JRC_cancel),
    M(FB "GoogleApiAvailabilityHelper", "makeGooglePlayServicesAvailable", "(Landroid/app/Activity;)Z", GAH_make),
    M(FB "GoogleApiAvailabilityHelper", "stopCallbacks", "()V", Noop),

    /* Play Billing */
    M(BC, "newBuilder", "(Landroid/content/Context;)Lcom/android/billingclient/api/BillingClient$Builder;", BC_newBuilder),
    M(BCB, "setListener", "(Lcom/android/billingclient/api/PurchasesUpdatedListener;)Lcom/android/billingclient/api/BillingClient$Builder;", BCB_self),
    M(BCB, "enablePendingPurchases", "()Lcom/android/billingclient/api/BillingClient$Builder;", BCB_self),
    M(BCB, "enablePendingPurchases", "(Lcom/android/billingclient/api/PendingPurchasesParams;)Lcom/android/billingclient/api/BillingClient$Builder;", BCB_self),
    M(BCB, "enableAutoServiceReconnection", "()Lcom/android/billingclient/api/BillingClient$Builder;", BCB_self),
    M(BCB, "enableAlternativeBillingOnly", "()Lcom/android/billingclient/api/BillingClient$Builder;", BCB_self),
    M(PPP, "newBuilder", "()Lcom/android/billingclient/api/PendingPurchasesParams$Builder;", PPP_newBuilder),
    M(PPPB, "enableOneTimeProducts", "()Lcom/android/billingclient/api/PendingPurchasesParams$Builder;", BCB_self),
    M(PPPB, "enablePrepaidPlans", "()Lcom/android/billingclient/api/PendingPurchasesParams$Builder;", BCB_self),
    M(PPPB, "build", "()Lcom/android/billingclient/api/PendingPurchasesParams;", PPPB_build),
    M(BCB, "build", "()Lcom/android/billingclient/api/BillingClient;", BCB_build),
    M(BC, "startConnection", "(Lcom/android/billingclient/api/BillingClientStateListener;)V", BC_startConnection),
    M(BC, "isReady", "()Z", RetFalse), M(BC, "endConnection", "()V", Noop), M(BC, "getConnectionState", "()I", RetZero),
    M(BC, "isFeatureSupported", "(Ljava/lang/String;)Lcom/android/billingclient/api/BillingResult;", BC_isFeatureSupported),
    M(BR, "getResponseCode", "()I", BR_code), M(BR, "getDebugMessage", "()Ljava/lang/String;", BR_message),
    M(BR, "toString", "()Ljava/lang/String;", BR_message),
    /* Play Asset Delivery (GTA SA DE's content, delivered as Play asset packs: everything is already installed) */
    M(PADF, "<init>", "()V", Noop),
    M(PADF, "getInstance", "(Landroid/content/Context;)Lcom/google/android/play/core/assetpacks/AssetPackManager;", pad_getInstance),
    M(PADM, "registerListener", "(Lcom/google/android/play/core/assetpacks/AssetPackStateUpdateListener;)V", pad_register),
    M(PADM, "unregisterListener", "(Lcom/google/android/play/core/assetpacks/AssetPackStateUpdateListener;)V", pad_unregister),
    M(PADM, "clearListeners", "()V", pad_clear),
    M(PADM, "getPackStates", "(Ljava/util/List;)Lcom/google/android/gms/tasks/Task;", pad_getPackStates),
    M(PADM, "fetch", "(Ljava/util/List;)Lcom/google/android/gms/tasks/Task;", pad_fetch),
    M(PADM, "cancel", "(Ljava/util/List;)Lcom/google/android/play/core/assetpacks/AssetPackStates;", pad_cancel),
    M(PADM, "removePack", "(Ljava/lang/String;)Lcom/google/android/gms/tasks/Task;", pad_removePack),
    M(PADM, "getPackLocation", "(Ljava/lang/String;)Lcom/google/android/play/core/assetpacks/AssetPackLocation;", pad_getPackLocation),
    M(PADM, "getPackLocations", "()Ljava/util/Map;", pad_getPackLocations),
    M(PADM, "getAssetLocation", "(Ljava/lang/String;Ljava/lang/String;)Lcom/google/android/play/core/assetpacks/AssetLocation;", pad_getAssetLocation),
    M(PADM, "showCellularDataConfirmation", "(Landroid/app/Activity;)Lcom/google/android/gms/tasks/Task;", pad_confirm_task),
    M(PADM, "showConfirmationDialog", "(Landroid/app/Activity;)Lcom/google/android/gms/tasks/Task;", pad_confirm_task),
    M(PADM, "showCellularDataConfirmation", "(Landroidx/activity/result/ActivityResultLauncher;)Z", pad_confirm_bool),
    M(PADM, "showConfirmationDialog", "(Landroidx/activity/result/ActivityResultLauncher;)Z", pad_confirm_bool),
    M(PADS, "name", "()Ljava/lang/String;", PADS_name), M(PADS, "status", "()I", PADS_status),
    M(PADS, "errorCode", "()I", PADS_error), M(PADS, "bytesDownloaded", "()J", PADS_bytes),
    M(PADS, "totalBytesToDownload", "()J", PADS_total), M(PADS, "transferProgressPercentage", "()I", PADS_progress),
    M(PADS, "updateAvailability", "()I", PADS_update),
    M(PADS, "installedVersionTag", "()Ljava/lang/String;", PADS_installed),
    M(PADS, "availableVersionTag", "()Ljava/lang/String;", PADS_available),
    M(PADS, "toString", "()Ljava/lang/String;", PADS_toString),
    M(PADSS, "packStates", "()Ljava/util/Map;", PADSS_packStates), M(PADSS, "totalBytes", "()J", PADSS_totalBytes),
    M(PADSS, "toString", "()Ljava/lang/String;", PADSS_toString),
    M(PADL, "path", "()Ljava/lang/String;", PADL_path), M(PADL, "assetsPath", "()Ljava/lang/String;", PADL_assetsPath),
    M(PADL, "packStorageMethod", "()I", PADL_method), M(PADL, "toString", "()Ljava/lang/String;", PADL_toString),
    M(PADAL, "path", "()Ljava/lang/String;", PADAL_path), M(PADAL, "offset", "()J", PADAL_offset), M(PADAL, "size", "()J", PADAL_size),
    M(PADE, "getErrorCode", "()I", PADE_getErrorCode), M(PADE, "toString", "()Ljava/lang/String;", PADE_toString),
    { NULL, NULL, NULL, NULL }
};

void tl_gms_install(void)
{
    if (getenv("TL_NO_GMS")) return;      /* debug knob: run a game without any of this, to compare */
    for (size_t i = 0; i < sizeof(k_classes) / sizeof(k_classes[0]); i++) tl_jni_declare(k_classes[i].name, k_classes[i].super);
    tl_jni_register_hle(k_gms);
}
