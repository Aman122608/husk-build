/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The first bionic shim: the symbol surface an APK's arm64 code binds
 * against, implemented over Darwin.
 *
 * What is here is honest: __android_log_print into the attempt's log, the
 * libc passthroughs where the two ABIs agree, a software ANativeWindow the
 * guest draws RGBX into and Swift reads back, the JNI tables a
 * NativeActivity receives, and a mutex built inside bionic's 40-byte
 * pthread_mutex_t so a guest that embeds the type still works. EGL is
 * named but not shimmed: the first attempt renders through the software
 * window, and eglGetProcAddress hands guests a resolver that answers from
 * this table so a guest that hard-requires EGL symbols at relocation time
 * at least links.
 *
 * Everything the guest binds against is in one table; husk_tl_attempt_*
 * entry points live in husk-tl-load.c.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-internal.h"

/* ---- forward decls of the loader side, kept in one place ----------- */

void tl_log_line(const char *fmt, ...);          /* husk-tl-load.c */
void *tl_shim_find(const char *name);            /* this file, further down */

/* Names a guest address as "<library>+<offset>"; husk-tl-load.c owns it. Declared
 * here because the shim uses it to say which guest code called something, which is
 * the difference between "the call never arrived" and "the call arrived and failed". */
int husk_tl_describe_address(uintptr_t addr, char *out, size_t outlen);
void tl_loader_frame_posted(void);               /* husk-tl-load.c */
void tl_loader_request_stop(void);               /* husk-tl-load.c */

/* The frames the guest draws, read from Swift between begin/end. */
void husk_tl_frame_begin_read(void);
void husk_tl_frame_end_read(void);
const uint8_t *husk_tl_frame_pixels(void);
int husk_tl_frame_width(void);
int husk_tl_frame_height(void);
int husk_tl_frame_stride(void);

/* ------------------------------------------- the guest-facing types -- */

/* tl_window lives in husk-tl-internal.h: the loader shares it. */

struct tl_input_queue {
    pthread_mutex_t lock;
};

struct tl_asset_manager {
    int unused;
};

/* AInputEvent type codes, from the NDK. */
#define AINPUT_EVENT_TYPE_MOTION 2
#define AMOTION_EVENT_ACTION_DOWN 0
#define AMOTION_EVENT_ACTION_UP   1
#define AMOTION_EVENT_ACTION_MOVE 2

/* ------------------------------------------------ the JNI fake VM ---- */

/*
 * A NativeActivity receives JNIEnv*, jobject and JavaVM* and is entitled
 * to call into them. Java cannot run here (ART is milestone 2), so the
 * tables answer the calls made before anything draws -- GetVersion,
 * GetEnv, AttachCurrentThread -- and the rest are stubs that log and
 * fail. The table must be the full size and order of JNI's
 * NativeInterface because the guest indexes it with compiled-in offsets;
 * the macro list keeps the order and the assert keeps the size.
 */

typedef struct tl_jni_env_ { const void *functions; } tl_jni_env;
typedef struct tl_jni_vm_  { const void *functions; } tl_jni_vm;

#define JNI_ENV_ENTRIES(X) \
    X(0,  reserved0)   X(1,  reserved1)   X(2,  reserved2)   X(3,  reserved3) \
    X(4,  GetVersion)  X(5,  DefineClass) X(6,  FindClass)   X(7,  FromReflectedMethod) \
    X(8,  FromReflectedField) X(9,  ToReflectedMethod) X(10, GetSuperclass) \
    X(11, IsAssignableFrom) X(12, ToReflectedField) X(13, Throw) X(14, ThrowNew) \
    X(15, ExceptionOccurred) X(16, ExceptionDescribe) X(17, ExceptionClear) \
    X(18, FatalError) X(19, PushLocalFrame) X(20, PopLocalFrame) \
    X(21, NewGlobalRef) X(22, DeleteGlobalRef) X(23, DeleteLocalRef) \
    X(24, IsSameObject) X(25, NewLocalRef) X(26, EnsureLocalCapacity) \
    X(27, AllocObject) X(28, NewObject) X(29, NewObjectV) X(30, NewObjectA) \
    X(31, GetObjectClass) X(32, IsInstanceOf) X(33, GetMethodID) \
    X(34, CallObjectMethod) X(35, CallObjectMethodV) X(36, CallObjectMethodA) \
    X(37, CallBooleanMethod) X(38, CallBooleanMethodV) X(39, CallBooleanMethodA) \
    X(40, CallByteMethod) X(41, CallByteMethodV) X(42, CallByteMethodA) \
    X(43, CallCharMethod) X(44, CallCharMethodV) X(45, CallCharMethodA) \
    X(46, CallShortMethod) X(47, CallShortMethodV) X(48, CallShortMethodA) \
    X(49, CallIntMethod) X(50, CallIntMethodV) X(51, CallIntMethodA) \
    X(52, CallLongMethod) X(53, CallLongMethodV) X(54, CallLongMethodA) \
    X(55, CallFloatMethod) X(56, CallFloatMethodV) X(57, CallFloatMethodA) \
    X(58, CallDoubleMethod) X(59, CallDoubleMethodV) X(60, CallDoubleMethodA) \
    X(61, CallVoidMethod) X(62, CallVoidMethodV) X(63, CallVoidMethodA) \
    X(64, CallNonvirtualObjectMethod) X(65, CallNonvirtualObjectMethodV) \
    X(66, CallNonvirtualObjectMethodA) X(67, CallNonvirtualBooleanMethod) \
    X(68, CallNonvirtualBooleanMethodV) X(69, CallNonvirtualBooleanMethodA) \
    X(70, CallNonvirtualByteMethod) X(71, CallNonvirtualByteMethodV) \
    X(72, CallNonvirtualByteMethodA) X(73, CallNonvirtualCharMethod) \
    X(74, CallNonvirtualCharMethodV) X(75, CallNonvirtualCharMethodA) \
    X(76, CallNonvirtualShortMethod) X(77, CallNonvirtualShortMethodV) \
    X(78, CallNonvirtualShortMethodA) X(79, CallNonvirtualIntMethod) \
    X(80, CallNonvirtualIntMethodV) X(81, CallNonvirtualIntMethodA) \
    X(82, CallNonvirtualLongMethod) X(83, CallNonvirtualLongMethodV) \
    X(84, CallNonvirtualLongMethodA) X(85, CallNonvirtualFloatMethod) \
    X(86, CallNonvirtualFloatMethodV) X(87, CallNonvirtualFloatMethodA) \
    X(88, CallNonvirtualDoubleMethod) X(89, CallNonvirtualDoubleMethodV) \
    X(90, CallNonvirtualDoubleMethodA) X(91, CallNonvirtualVoidMethod) \
    X(92, CallNonvirtualVoidMethodV) X(93, CallNonvirtualVoidMethodA) \
    X(94, GetFieldID) X(95, GetObjectField) X(96, GetBooleanField) \
    X(97, GetByteField) X(98, GetCharField) X(99, GetShortField) \
    X(100, GetIntField) X(101, GetLongField) X(102, GetFloatField) \
    X(103, GetDoubleField) X(104, SetObjectField) X(105, SetBooleanField) \
    X(106, SetByteField) X(107, SetCharField) X(108, SetShortField) \
    X(109, SetIntField) X(110, SetLongField) X(111, SetFloatField) \
    X(112, SetDoubleField) X(113, GetStaticMethodID) \
    X(114, CallStaticObjectMethod) X(115, CallStaticObjectMethodV) \
    X(116, CallStaticObjectMethodA) X(117, CallStaticBooleanMethod) \
    X(118, CallStaticBooleanMethodV) X(119, CallStaticBooleanMethodA) \
    X(120, CallStaticByteMethod) X(121, CallStaticByteMethodV) \
    X(122, CallStaticByteMethodA) X(123, CallStaticCharMethod) \
    X(124, CallStaticCharMethodV) X(125, CallStaticCharMethodA) \
    X(126, CallStaticShortMethod) X(127, CallStaticShortMethodV) \
    X(128, CallStaticShortMethodA) X(129, CallStaticIntMethod) \
    X(130, CallStaticIntMethodV) X(131, CallStaticIntMethodA) \
    X(132, CallStaticLongMethod) X(133, CallStaticLongMethodV) \
    X(134, CallStaticLongMethodA) X(135, CallStaticFloatMethod) \
    X(136, CallStaticFloatMethodV) X(137, CallStaticFloatMethodA) \
    X(138, CallStaticDoubleMethod) X(139, CallStaticDoubleMethodV) \
    X(140, CallStaticDoubleMethodA) X(141, CallStaticVoidMethod) \
    X(142, CallStaticVoidMethodV) X(143, CallStaticVoidMethodA) \
    X(144, GetStaticFieldID) X(145, GetStaticObjectField) \
    X(146, GetStaticBooleanField) X(147, GetStaticByteField) \
    X(148, GetStaticCharField) X(149, GetStaticShortField) \
    X(150, GetStaticIntField) X(151, GetStaticLongField) \
    X(152, GetStaticFloatField) X(153, GetStaticDoubleField) \
    X(154, SetStaticObjectField) X(155, SetStaticBooleanField) \
    X(156, SetStaticByteField) X(157, SetStaticCharField) \
    X(158, SetStaticShortField) X(159, SetStaticIntField) \
    X(160, SetStaticLongField) X(161, SetStaticFloatField) \
    X(162, SetStaticDoubleField) X(163, NewString) X(164, GetStringLength) \
    X(165, GetStringChars) X(166, ReleaseStringChars) X(167, NewStringUTF) \
    X(168, GetStringUTFLength) X(169, GetStringUTFChars) \
    X(170, ReleaseStringUTFChars) X(171, GetArrayLength) \
    X(172, NewObjectArray) X(173, GetObjectArrayElement) \
    X(174, SetObjectArrayElement) X(175, NewBooleanArray) \
    X(176, NewByteArray) X(177, NewCharArray) X(178, NewShortArray) \
    X(179, NewIntArray) X(180, NewLongArray) X(181, NewFloatArray) \
    X(182, NewDoubleArray) X(183, GetBooleanArrayElements) \
    X(184, GetByteArrayElements) X(185, GetCharArrayElements) \
    X(186, GetShortArrayElements) X(187, GetIntArrayElements) \
    X(188, GetLongArrayElements) X(189, GetFloatArrayElements) \
    X(190, GetDoubleArrayElements) X(191, ReleaseBooleanArrayElements) \
    X(192, ReleaseByteArrayElements) X(193, ReleaseCharArrayElements) \
    X(194, ReleaseShortArrayElements) X(195, ReleaseIntArrayElements) \
    X(196, ReleaseLongArrayElements) X(197, ReleaseFloatArrayElements) \
    X(198, ReleaseDoubleArrayElements) X(199, GetBooleanArrayRegion) \
    X(200, GetByteArrayRegion) X(201, GetCharArrayRegion) \
    X(202, GetShortArrayRegion) X(203, GetIntArrayRegion) \
    X(204, GetLongArrayRegion) X(205, GetFloatArrayRegion) \
    X(206, GetDoubleArrayRegion) X(207, SetBooleanArrayRegion) \
    X(208, SetByteArrayRegion) X(209, SetCharArrayRegion) \
    X(210, SetShortArrayRegion) X(211, SetIntArrayRegion) \
    X(212, SetLongArrayRegion) X(213, SetFloatArrayRegion) \
    X(214, SetDoubleArrayRegion) X(215, RegisterNatives) \
    X(216, UnregisterNatives) X(217, MonitorEnter) X(218, MonitorExit) \
    X(219, GetJavaVM) X(220, GetStringRegion) X(221, GetStringUTFRegion) \
    X(222, GetPrimitiveArrayCritical) X(223, ReleasePrimitiveArrayCritical) \
    X(224, GetStringCritical) X(225, ReleaseStringCritical) \
    X(226, NewWeakGlobalRef) X(227, DeleteWeakGlobalRef) \
    X(228, ExceptionCheck) X(229, NewDirectByteBuffer) \
    X(230, GetDirectBufferAddress) X(231, GetDirectBufferCapacity) \
    X(232, GetObjectRefType)

/* The real ones. Forward declared where they refer to state below. */
static int32_t jni_GetVersion_impl(void) { return 0x00090000; }
static tl_jni_vm g_vm;
static int32_t jni_GetJavaVM_impl(void *env, void **vm)
{
    (void)env;
    if (vm) *vm = &g_vm;
    return 0;
}
static int32_t jni_MonitorEnter_impl(void *env, void *obj) { (void)env; (void)obj; return 0; }
static int32_t jni_MonitorExit_impl(void *env, void *obj)  { (void)env; (void)obj; return 0; }
static void *jni_NewDirectByteBuffer_impl(void *env, void *addr, int64_t cap)
{
    (void)env;
    /* A plain struct with the address and the capacity, in place of the
     * object: code that only reads its address and capacity gets truth. */
    struct { void *addr; int64_t cap; } *buf = malloc(sizeof(*buf) + (size_t)(cap > 0 ? cap : 0));
    if (!buf) return NULL;
    buf->addr = addr;
    buf->cap = cap;
    return buf;
}
static void *jni_GetDirectBufferAddress_impl(void *env, void *buf)
{
    (void)env;
    struct { void *addr; int64_t cap; } *b = buf;
    return b ? b->addr : NULL;
}
static int64_t jni_GetDirectBufferCapacity_impl(void *env, void *buf)
{
    (void)env;
    struct { void *addr; int64_t cap; } *b = buf;
    return b ? b->cap : 0;
}

/* Generic stub: log the entry's name and return a failure the caller can
 * see (0/false/NULL are all "no" in JNI's conventions). */
static int jni_stub_log(const char *name)
{
    tl_log_line("jni: %s called -- Java is not running (ART is milestone 2); returning failure", name);
    return 0;
}

#define JNI_SLOT(n, name) \
    static void *jni_slot_##name(void *a0, void *a1, void *a2, void *a3) { \
        (void)a0; (void)a1; (void)a2; (void)a3; \
        jni_stub_log(#name); \
        return 0; \
    }

/* Build the 233 slots: the real ones where implemented, stubs elsewhere. */
static const void *g_env_table[233];

/* Every unfinished slot points at its own thunk, generated by macro, so
 * a guest that reaches one learns exactly which entry it hit. */
#define THUNK(n, name) \
    static void *jni_thunk_##n(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) { \
        (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; \
        jni_stub_log(#name); \
        (void)a0; \
        return 0; \
    }

JNI_ENV_ENTRIES(THUNK)

static void jni_env_table_fill(void)
{
    void **t = (void **)g_env_table;
#define FILL(n, name) t[n] = (void *)jni_thunk_##n;
    JNI_ENV_ENTRIES(FILL)
#undef FILL
    t[4]   = (void *)jni_GetVersion_impl;
    t[217] = (void *)jni_MonitorEnter_impl;
    t[218] = (void *)jni_MonitorExit_impl;
    t[219] = (void *)jni_GetJavaVM_impl;
    t[229] = (void *)jni_NewDirectByteBuffer_impl;
    t[230] = (void *)jni_GetDirectBufferAddress_impl;
    t[231] = (void *)jni_GetDirectBufferCapacity_impl;
}

_Static_assert(sizeof(g_env_table) / sizeof(g_env_table[0]) == 233,
               "the JNI table must match NativeInterface's entry count");

static tl_jni_env g_env_main = { g_env_table };

/* NativeInterface's mirror, for the VM side. */
typedef struct {
    void *reserved0, *reserved1, *reserved2;
    int32_t (*DestroyJavaVM)(void *);
    int32_t (*AttachCurrentThread)(void *, void **, void *);
    int32_t (*DetachCurrentThread)(void *);
    int32_t (*GetEnv)(void *, void **, uint32_t);
    int32_t (*AttachCurrentThreadAsDaemon)(void *, void **, void *);
} tl_vm_vtable;

static int32_t vm_DestroyJavaVM(void *vm) { (void)vm; return 0; }
static int32_t vm_AttachCurrentThread(void *vm, void **env, void *args)
{
    (void)vm; (void)args;
    if (env) *env = &g_env_main;
    return 0;
}
static int32_t vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static int32_t vm_GetEnv(void *vm, void **penv, uint32_t version)
{
    (void)vm;
    if (penv) *penv = &g_env_main;
    return (version <= 0x00090000) ? 0 : -0x100;
}
static int32_t vm_AttachCurrentThreadAsDaemon(void *vm, void **env, void *args)
{
    return vm_AttachCurrentThread(vm, env, args);
}

static const tl_vm_vtable g_vm_vtable = {
    NULL, NULL, NULL,
    vm_DestroyJavaVM,
    vm_AttachCurrentThread,
    vm_DetachCurrentThread,
    vm_GetEnv,
    vm_AttachCurrentThreadAsDaemon,
};

static void jni_init(void)
{
    static bool once;
    if (!once) {
        jni_env_table_fill();
        g_vm.functions = &g_vm_vtable;
        once = true;
    }
}

void *tl_shim_env(void)  { jni_init(); return &g_env_main; }
void *tl_shim_vm(void)   { jni_init(); return &g_vm; }
void *tl_shim_activity_class(void) { return (void *)0x10; }   /* non-null stand-in */
int32_t JNI_GetDefaultJavaVMInitArgs(void *args) { (void)args; return -1; }
int32_t JNI_CreateJavaVM(void **vm, void **env, void *args)
{
    (void)vm; (void)env; (void)args;
    tl_log_line("jni: JNI_CreateJavaVM -- Java cannot start (ART is milestone 2)");
    return -1;
}
int32_t JNI_GetCreatedJavaVMs(void **vms, int max, int *count)
{
    jni_init();
    if (count) *count = 0;
    if (max > 0 && vms) vms[0] = NULL;
    return 0;
}

/* ------------------------------------------------- window and frames -- */

tl_window *tl_window_create(int width, int height)
{
    tl_window *w = calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->width = width;
    w->height = height;
    w->format = 1;               /* WINDOW_FORMAT_RGBA_8888 */
    w->stridePixels = width;
    w->bits = calloc(1, (size_t)width * height * 4);
    pthread_mutex_init(&w->lock, NULL);
    atomic_init(&w->refcount, 1);
    return w;
}

static void tl_window_free(tl_window *w)
{
    pthread_mutex_destroy(&w->lock);
    free(w->bits);
    free(w);
}

void tl_window_acquire(tl_window *w) { if (w) atomic_fetch_add(&w->refcount, 1); }
void tl_window_release(tl_window *w)
{
    if (w && atomic_fetch_sub(&w->refcount, 1) == 1) {
        tl_window_free(w);
    }
}

/* The lock the loader holds while the guest draws. */
void tl_window_lock(tl_window *w)   { if (w) pthread_mutex_lock(&w->lock); }
void tl_window_unlock(tl_window *w) { if (w) pthread_mutex_unlock(&w->lock); }
uint8_t *tl_window_bits(tl_window *w) { return w ? w->bits : NULL; }

int32_t ANativeWindow_getWidth(void *window)  { return window ? ((tl_window *)window)->width : 0; }
int32_t ANativeWindow_getHeight(void *window) { return window ? ((tl_window *)window)->height : 0; }
int32_t ANativeWindow_getFormat(void *window) { return window ? ((tl_window *)window)->format : 0; }

/*
 * ANativeWindow_lock: the NDK's ANativeWindow_Buffer is
 *   { void* bits; int32_t width, height, stride, format; ... }
 * so the guest reads fields[0] as the pixel pointer and fields[3] as the
 * row stride in pixels. Both readings of the historical layout (stride
 * second) get truth: fields[1] is width, and a guest written against the
 * old order reads width as its stride -- width == stride here, so that
 * is still correct.
 */
int32_t ANativeWindow_lock(void *window, void *outBuffer, void *rect)
{
    (void)rect;
    if (!window || !outBuffer) {
        return -1;
    }
    tl_window *w = window;
    pthread_mutex_lock(&w->lock);
    void **fields = outBuffer;
    fields[0] = w->bits;
    int32_t *nums = (int32_t *)(fields + 1);
    nums[0] = w->width;
    nums[1] = w->height;
    nums[2] = w->stridePixels;
    nums[3] = w->format;
    return 0;
}

int32_t ANativeWindow_unlockAndPost(void *window)
{
    if (!window) {
        return -1;
    }
    pthread_mutex_unlock(&((tl_window *)window)->lock);
    tl_loader_frame_posted();        /* the loader counts the frame */
    return 0;
}

void ANativeWindow_acquire(void *window) { tl_window_acquire(window); }
void ANativeWindow_release(void *window) { tl_window_release(window); }

int32_t ANativeWindow_setBuffersGeometry(void *window, int32_t width,
                                         int32_t height, int32_t format)
{
    (void)window; (void)width; (void)height; (void)format;
    /* The attempt's window does not resize; claiming success keeps a
     * guest that calls it before drawing alive, and the size it asked
     * for is what it already had. */
    return 0;
}

void ANativeWindow_setFrameRate(void *window, float fps, int mode)
{
    (void)window; (void)fps; (void)mode;
}

/* ------------------------------------------------------------------- GL --
 *
 * The guest's GLES and EGL come from the ANGLE the app already embeds
 * (Frameworks/libANGLE-shared.dylib) and that QEMU already dlopens for its own GL.
 *
 * The app links neither OpenGLES nor ANGLE, so dlsym(RTLD_DEFAULT) finds almost no gl*
 * or egl* symbol. What it did find was iOS's own OpenGL ES, which stops at ES 3.0: the
 * tell in the log was precise -- the 3.2 fence family resolved while every 3.1 addition
 * (images, compute, indirect draws, vertex-attribute binding) did not. A guest with half
 * its GL from one implementation and half from another is not a guest with GL at all, so
 * the whole surface is answered from ANGLE, and the stubs below only cover the case where
 * ANGLE cannot be loaded at all.
 *
 * Two calls this layer owns outright, because ANGLE cannot do them here:
 *
 *   eglCreateWindowSurface -- the guest's native window is this layer's own tl_window
 *     struct, not an iOS drawable. Handing that pointer to ANGLE's Metal backend means it
 *     messages a pointer that is not an object. A pbuffer of the same size is a surface
 *     ANGLE can genuinely render into.
 *   eglSwapBuffers -- reads the finished frame back into that window and posts it, which
 *     is how the rest of this layer (and Husk's display) sees anything at all.
 */
#define EGL_FALSE 0
#define EGL_TRUE  1
#define EGL_NONE  0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define GL_RGBA          0x1908
#define GL_UNSIGNED_BYTE 0x1401

/* The window this run draws into; set by tl_shim_bind_run. */
static tl_window *g_run_window;

static void *g_angle;
static bool  g_angle_tried;
static bool  g_angle_frame_logged;

static void *angle_open(void)
{
    if (!g_angle_tried) {
        g_angle_tried = true;
        g_angle = dlopen("@rpath/libANGLE-shared.dylib", RTLD_NOW | RTLD_LOCAL);
        if (!g_angle) {
            g_angle = dlopen("libANGLE-shared.dylib", RTLD_NOW | RTLD_LOCAL);
        }
        if (g_angle) {
            tl_log_line("gl: ANGLE loaded -- the guest's whole GLES/EGL surface comes from it");
        } else {
            /* One dlerror() call: it clears the error, so asking twice returns NULL. */
            const char *why = dlerror();
            tl_log_line("gl: ANGLE could not be loaded (%s); GLES/EGL stay stubbed and the "
                        "guest gets no graphics", why ? why : "unknown");
        }
    }
    return g_angle;
}

/* One symbol from ANGLE, or NULL. Every gl* and egl* name goes through here. */
static void *angle_symbol(const char *name)
{
    void *h = angle_open();
    return h ? dlsym(h, name) : NULL;
}

static bool is_gl_or_egl(const char *name)
{
    /* glXxx: gl + a capital. eglXxx. Nothing else in libc looks like that. The length
     * check is not decoration: a two-character name would otherwise read past its own
     * terminator. */
    if (!name || strlen(name) < 3) {
        return false;
    }
    if (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') {
        return true;
    }
    return name[0] == 'e' && name[1] == 'g' && name[2] == 'l';
}

/* Read the finished frame out of ANGLE's draw buffer and into the window the rest of
 * this layer presents from, then post it. GL's origin is bottom-left and the window's is
 * top-left, so the rows come back inverted; the window is RGBA_8888 and glReadPixels with
 * GL_RGBA/GL_UNSIGNED_BYTE produces exactly those bytes, so only the row order changes. */
static void angle_present_frame(void)
{
    tl_window *w = g_run_window;
    if (!w || !w->bits || w->width <= 0 || w->height <= 0) {
        return;
    }
    static uint8_t *scratch;
    static size_t scratch_cap;
    const size_t row = (size_t)w->width * 4;
    const size_t need = row * (size_t)w->height;
    if (scratch_cap < need) {
        uint8_t *grown = realloc(scratch, need);
        if (!grown) {
            return;
        }
        scratch = grown;
        scratch_cap = need;
    }

    static void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *);
    if (!read_pixels) {
        read_pixels = (void (*)(int, int, int, int, unsigned, unsigned, void *))
                      angle_symbol("glReadPixels");
    }
    if (!read_pixels) {
        return;
    }
    read_pixels(0, 0, w->width, w->height, GL_RGBA, GL_UNSIGNED_BYTE, scratch);

    const size_t stride = (size_t)(w->stridePixels > 0 ? w->stridePixels : w->width) * 4;
    pthread_mutex_lock(&w->lock);
    for (int y = 0; y < w->height; y++) {
        memcpy(w->bits + (size_t)y * stride,
               scratch + (size_t)(w->height - 1 - y) * row, row);
    }
    pthread_mutex_unlock(&w->lock);

    if (!g_angle_frame_logged) {
        g_angle_frame_logged = true;
        tl_log_line("gl: first frame read back from ANGLE into the window (%dx%d)",
                    w->width, w->height);
    }
    tl_loader_frame_posted();
}

void *eglGetProcAddress(const char *name)
{
    return tl_shim_find(name);
}

unsigned eglGetError(void) { return 0x3005; }   /* EGL_NOT_INITIALIZED */

/*
 * The rest of the EGL surface. With ANGLE loaded, none of these is reached -- the lookup
 * prefers ANGLE's own, and it has a real display, configs, contexts and surfaces. They are
 * what answers when it is not, and they report an EGL that cannot provide a display rather
 * than handing back a fake one a guest would dereference on its next call.
 */
void *eglGetDisplay(void *display_id)
{
    (void)display_id;
    return (void *)8;       /* non-NULL: EGL_NO_DISPLAY is NULL */
}
int eglInitialize(void *dpy, int *major, int *minor)
{
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 4;
    tl_log_line("egl: eglInitialize -- no ANGLE and no EGL on iOS, so the guest gets no display");
    return EGL_FALSE;
}
int eglTerminate(void *dpy) { (void)dpy; return EGL_TRUE; }
int eglChooseConfig(void *dpy, const int *attribs, void **configs, int config_size, int *num_config)
{
    (void)dpy; (void)attribs; (void)configs; (void)config_size;
    if (num_config) *num_config = 0;
    return EGL_TRUE;
}
int eglGetConfigAttrib(void *dpy, void *config, int attribute, int *value)
{
    (void)dpy; (void)config; (void)attribute;
    if (value) *value = 0;
    return EGL_FALSE;
}
void *eglCreateContext(void *dpy, void *config, void *share, const int *attribs)
{
    (void)dpy; (void)config; (void)share; (void)attribs;
    return NULL;            /* EGL_NO_CONTEXT */
}
int eglDestroyContext(void *dpy, void *ctx) { (void)dpy; (void)ctx; return EGL_TRUE; }

/* Ours, not ANGLE's: see the block comment. */
void *eglCreateWindowSurface(void *dpy, void *config, void *win, const int *attribs)
{
    (void)win; (void)attribs;
    void *(*make_pbuffer)(void *, void *, const int *) =
        (void *(*)(void *, void *, const int *))angle_symbol("eglCreatePbufferSurface");
    if (!make_pbuffer) {
        return NULL;        /* EGL_NO_SURFACE */
    }
    tl_window *w = g_run_window;
    const int width  = (w && w->width  > 0) ? w->width  : 540;
    const int height = (w && w->height > 0) ? w->height : 960;
    const int pbuffer[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    void *surface = make_pbuffer(dpy, config, pbuffer);
    tl_log_line("egl: window surface -> a %dx%d ANGLE pbuffer (%p)", width, height, surface);
    return surface;
}

void *eglCreatePbufferSurface(void *dpy, void *config, const int *attribs)
{
    void *(*make_pbuffer)(void *, void *, const int *) =
        (void *(*)(void *, void *, const int *))angle_symbol("eglCreatePbufferSurface");
    return make_pbuffer ? make_pbuffer(dpy, config, attribs) : NULL;
}
int eglDestroySurface(void *dpy, void *surface) { (void)dpy; (void)surface; return EGL_TRUE; }
int eglQuerySurface(void *dpy, void *surface, int attribute, int *value)
{
    (void)dpy; (void)surface; (void)attribute;
    if (value) *value = 0;
    return EGL_TRUE;
}
int eglSurfaceAttrib(void *dpy, void *surface, int attribute, int value)
{
    (void)dpy; (void)surface; (void)attribute; (void)value;
    return EGL_TRUE;
}
int eglBindAPI(unsigned api) { (void)api; return EGL_TRUE; }
void *eglGetCurrentContext(void) { return NULL; }
int eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx)
{
    (void)dpy; (void)draw; (void)read; (void)ctx;
    return EGL_FALSE;
}
const char *eglQueryString(void *dpy, int name)
{
    (void)dpy; (void)name;
    return "";              /* never NULL: a guest will strlen() it */
}
int eglSwapInterval(void *dpy, int interval) { (void)dpy; (void)interval; return EGL_TRUE; }

/* Ours, not ANGLE's: ANGLE presents into a pbuffer nobody can see, so the frame has to
 * be brought back into the window on every swap. */
int eglSwapBuffers(void *dpy, void *surface)
{
    int (*swap)(void *, void *) = (int (*)(void *, void *))angle_symbol("eglSwapBuffers");
    int ok = swap ? swap(dpy, surface) : EGL_TRUE;
    angle_present_frame();
    return ok;
}

/*
 * The ES 3.1 / 3.2 entry points. Whatever supplied this guest's ES 3.0 calls has
 * no symbol for these, and the tell is precise: the resolved set stops exactly at
 * the 3.1 additions (images, compute, indirect draws, vertex-attribute binding)
 * while the 3.2 fence family resolved. With no context there is nothing to call
 * them on, so naming them is all that is needed -- and all that is honest.
 */
void glBindImageTexture(unsigned unit, unsigned tex, int level, unsigned layered,
                        int layer, unsigned access, unsigned format)
{
    (void)unit; (void)tex; (void)level; (void)layered; (void)layer; (void)access; (void)format;
}
void glBindVertexBuffer(unsigned bindingindex, unsigned buffer, long offset, int stride)
{ (void)bindingindex; (void)buffer; (void)offset; (void)stride; }
void glDispatchCompute(unsigned a, unsigned b, unsigned c) { (void)a; (void)b; (void)c; }
void glDispatchComputeIndirect(long offset) { (void)offset; }
void glDrawArraysIndirect(unsigned mode, const void *indirect) { (void)mode; (void)indirect; }
void glDrawElementsIndirect(unsigned mode, unsigned type, const void *indirect)
{ (void)mode; (void)type; (void)indirect; }
void glMemoryBarrier(unsigned barriers) { (void)barriers; }
void glVertexAttribBinding(unsigned index, unsigned bindingindex)
{ (void)index; (void)bindingindex; }
void glVertexAttribFormat(unsigned index, int size, unsigned type, unsigned normalized,
                          unsigned relativeoffset)
{ (void)index; (void)size; (void)type; (void)normalized; (void)relativeoffset; }
void glVertexAttribIFormat(unsigned index, int size, unsigned type, unsigned relativeoffset)
{ (void)index; (void)size; (void)type; (void)relativeoffset; }
void glVertexBindingDivisor(unsigned bindingindex, unsigned divisor)
{ (void)bindingindex; (void)divisor; }

/* --------------------------------------------------------- activity -- */

void ANativeActivity_finish(void *activity)
{
    (void)activity;
    tl_log_line("activity: ANativeActivity_finish -- stopping the attempt");
    tl_loader_request_stop();
}

void ANativeActivity_setWindowFlags(void *a, unsigned flags, unsigned mask)
{
    (void)a; (void)flags; (void)mask;
}

void ANativeActivity_showSoftInput(void *a, unsigned flags) { (void)a; (void)flags; }
void ANativeActivity_hideSoftInput(void *a, unsigned flags) { (void)a; (void)flags; }
void ANativeActivity_setWindowFormat(void *a, int format) { (void)a; (void)format; }

/* ---------------------------------------------------------- input  -- */

int32_t AInputQueue_preDispatchEvent(void *q, void *e) { (void)q; (void)e; return 0; }
void AInputQueue_finishEvent(void *q, void *e, int handled) { (void)q; (void)e; (void)handled; }
void AInputQueue_attach(void *q) { (void)q; }
void AInputQueue_detach(void *q) { (void)q; }

/* The loader pushes touch events as AInputEvent-shaped memory; the guest
 * reads type, action and pointers from the offsets below. */
struct tl_input_event {
    int32_t type;          /* AINPUT_EVENT_TYPE_MOTION */
    int32_t action;        /* DOWN / UP / MOVE */
    int64_t when;
    float  x, y;
    int32_t pointer_count;
};

int32_t AInputEvent_getType(const void *event)
{
    return event ? ((const struct tl_input_event *)event)->type : 0;
}
int32_t AMotionEvent_getAction(const void *event)
{
    return event ? ((const struct tl_input_event *)event)->action : 0;
}
int32_t AMotionEvent_getPointerCount(const void *event)
{
    return event ? ((const struct tl_input_event *)event)->pointer_count : 1;
}
float AMotionEvent_getX(const void *event, size_t idx)
{
    (void)idx;
    return event ? ((const struct tl_input_event *)event)->x : 0.f;
}
float AMotionEvent_getY(const void *event, size_t idx)
{
    (void)idx;
    return event ? ((const struct tl_input_event *)event)->y : 0.f;
}
int64_t AMotionEvent_getEventTime(const void *event)
{
    return event ? ((const struct tl_input_event *)event)->when : 0;
}
int32_t AMotionEvent_getButtonState(const void *event) { (void)event; return 0; }
int32_t AMotionEvent_getPointerId(const void *event, size_t idx)
{
    (void)event;                /* one finger, so the pointer id is its index */
    return (int32_t)idx;
}

/*
 * The glue's whole lifecycle is single bytes down the message pipe, and nothing else in
 * this run writes one byte. So instrument exactly that: whether the command byte was
 * written, to which fd, and what came back -- and whether the game thread ever read it.
 *
 * Without this, a command written and never read looks identical to a command never
 * written at all, which is the same invisible-failure trap as the constructor loop.
 * Only len == 1 is logged, so the rest of the guest's I/O passes through untouched.
 */
ssize_t write_shim(int fd, const void *buf, size_t len)
{
    errno = 0;
    ssize_t r = write(fd, buf, len);
    int err = errno;
    if (len == 1 && fd >= 0) {
        tl_log_line("pipe cmd: write fd=%d byte=%#x -> %zd%s",
                    fd, (unsigned)*(const unsigned char *)buf, r,
                    r < 0 ? " (failed)" : "");
        if (r < 0) tl_log_line("pipe cmd: write errno %d", err);
    }
    errno = err;
    return r;
}

ssize_t read_shim(int fd, void *buf, size_t len)
{
    errno = 0;
    ssize_t r = read(fd, buf, len);
    int err = errno;
    if (len == 1 && r == 1) {
        tl_log_line("pipe cmd: read  fd=%d byte=%#x", fd,
                    (unsigned)*(const unsigned char *)buf);
    }
    errno = err;
    return r;
}

int pipe_shim(int fds[2])
{
    int r = pipe(fds);
    if (r == 0) tl_log_line("pipe cmd: created read=%d write=%d", fds[0], fds[1]);
    else        tl_log_line("pipe cmd: pipe failed, errno %d", errno);
    return r;
}

/* -------------------------------------------------------- looper  ---- */

/*
 * A looper that actually polls.
 *
 * The NDK's native_app_glue registers the message pipe here --
 * ALooper_addFd(looper, app->msgpipe[0], LOOPER_ID_MAIN, ALOOPER_EVENT_INPUT, NULL,
 * &app->looperSource) -- and reads a lifecycle command only when ALooper_pollAll returns
 * that ident. The previous stub returned ALOOPER_POLL_TIMEOUT immediately and ignored the
 * timeout outright, and addFd did nothing at all. So the pipe was registered into a
 * looper that forgot it, the game thread spun hot on a -1 timeout that never blocked, and
 * not one command was ever delivered. The engine sat waiting for APP_CMD_START.
 *
 * pollAll takes no looper argument, so the looper has to be found per thread -- which is
 * also what the NDK means by it: one looper per thread, created by ALooper_prepare.
 */
#define ALOOPER_POLL_TIMEOUT      (-3)
#define ALOOPER_POLL_ERROR        (-4)
#define ALOOPER_EVENT_INPUT       1
#define ALOOPER_EVENT_OUTPUT      2
#define TL_LOOPER_MAX             8
#define AINPUT_SOURCE_TOUCHSCREEN 0x1002

typedef struct {
    int   fd[TL_LOOPER_MAX];
    int   ident[TL_LOOPER_MAX];
    int   events[TL_LOOPER_MAX];
    void *callback[TL_LOOPER_MAX];
    void *data[TL_LOOPER_MAX];
    int   n;
} tl_looper;

static __thread tl_looper *tl_looper_cur;

void *ALooper_prepare(int opts)
{
    (void)opts;
    if (!tl_looper_cur) {
        tl_looper_cur = calloc(1, sizeof(tl_looper));
        tl_log_line("looper: prepare on thread %p -> %p", (void *)pthread_self(), (void *)tl_looper_cur);
    }
    return tl_looper_cur;
}

int ALooper_addFd(void *looper, int fd, int ident, int events, void *callback, void *data)
{
    tl_looper *l = looper ? (tl_looper *)looper : tl_looper_cur;
    if (!l || l->n >= TL_LOOPER_MAX) {
        tl_log_line("looper: addFd REFUSED looper=%p fd=%d ident=%d (n=%d)",
                    looper, fd, ident, l ? l->n : -1);
        return 0;
    }
    l->fd[l->n]       = fd;
    l->ident[l->n]    = ident;
    l->events[l->n]   = events;
    l->callback[l->n] = callback;
    l->data[l->n]     = data;
    l->n++;
    /* Which looper the pipe lands on is the whole question: it is registered on the game
     * thread's looper, so the game thread must be the one polling it. */
    tl_log_line("looper: addFd looper=%p (thread %p) fd=%d ident=%d events=%#x n=%d",
                (void *)l, (void *)pthread_self(), fd, ident, (unsigned)events, l->n);
    return 1;
}

int ALooper_pollAll(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    if (out_fd) *out_fd = -1;
    if (out_events) *out_events = 0;
    if (out_data) *out_data = NULL;

    tl_looper *l = tl_looper_cur;
    /* First few calls per looper, plus every call that returns a source: a hot polling
     * loop that never sees its fd is exactly the shape being chased. */
    static int seen;
    int trace = (seen < 6);
    if (trace) seen++;

    if (!l || l->n == 0) {
        if (trace) tl_log_line("looper: pollAll timeout_ms=%d on EMPTY looper %p (thread %p)",
                               timeout_ms, (void *)l, (void *)pthread_self());
        if (timeout_ms > 0) {
            struct timespec t;
            t.tv_sec = timeout_ms / 1000;
            t.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
            nanosleep(&t, NULL);
        }
        return ALOOPER_POLL_TIMEOUT;
    }

    struct pollfd p[TL_LOOPER_MAX];
    for (int i = 0; i < l->n; i++) {
        p[i].fd = l->fd[i];
        p[i].events = 0;
        p[i].revents = 0;
        if (l->events[i] & ALOOPER_EVENT_INPUT)  p[i].events |= POLLIN;
        if (l->events[i] & ALOOPER_EVENT_OUTPUT) p[i].events |= POLLOUT;
    }

    /* timeout_ms = -1 blocks, which is exactly what the glue asks for. */
    int r = poll(p, (nfds_t)l->n, timeout_ms);
    if (r < 0) {
        tl_log_line("looper: pollAll poll() failed errno=%d (thread %p)", errno, (void *)pthread_self());
        return ALOOPER_POLL_ERROR;
    }
    if (r == 0) {
        if (trace) tl_log_line("looper: pollAll timeout after %d ms on %d fd(s) (thread %p)",
                               timeout_ms, l->n, (void *)pthread_self());
        return ALOOPER_POLL_TIMEOUT;
    }

    for (int i = 0; i < l->n; i++) {
        if (!p[i].revents) continue;
        tl_log_line("looper: pollAll READY fd=%d ident=%d revents=%#x (thread %p)",
                    l->fd[i], l->ident[i], (unsigned)p[i].revents, (void *)pthread_self());
        if (out_fd)     *out_fd = l->fd[i];
        if (out_events) *out_events = l->events[i];
        if (out_data)   *out_data = l->data[i];
        return l->ident[i];
    }
    return ALOOPER_POLL_TIMEOUT;
}
void ALooper_acquire(void *looper) { (void)looper; }
void ALooper_release(void *looper) { (void)looper; }
void ALooper_wake(void *looper)    { (void)looper; }

void AInputQueue_attachLooper(void *q, void *looper, int ident, void *callback, void *data)
{
    (void)q; (void)looper; (void)ident; (void)callback; (void)data;
}
void AInputQueue_detachLooper(void *q) { (void)q; }
int32_t AInputQueue_getEvent(void *q, void **out_event)
{
    (void)q;
    if (out_event) *out_event = NULL;
    return -1;                  /* nothing pending, as a real empty queue reports */
}

int32_t AInputEvent_getDeviceId(const void *event) { (void)event; return 0; }
int32_t AInputEvent_getSource(const void *event)
{
    return event ? AINPUT_SOURCE_TOUCHSCREEN : 0;
}

int32_t AKeyEvent_getAction(const void *event) { (void)event; return 0; }
int32_t AKeyEvent_getFlags(const void *event) { (void)event; return 0; }
int32_t AKeyEvent_getKeyCode(const void *event) { (void)event; return 0; }
int32_t AKeyEvent_getMetaState(const void *event) { (void)event; return 0; }

/* --------------------------------------------------------- assets  -- */

void *AAssetManager_open(void *mgr, const char *name, int mode)
{
    (void)mgr; (void)mode;
    tl_log_line("asset: %s requested -- AAsset is not wired in the attempt", name);
    return NULL;
}
void AAsset_close(void *a) { (void)a; }
int AAsset_read(void *a, void *buf, size_t n) { (void)a; (void)buf; (void)n; return 0; }
int64_t AAsset_getLength(void *a) { (void)a; return 0; }
const void *AAsset_getBuffer(void *a) { (void)a; return NULL; }
void *AAssetManager_fromJava(void *env, void *mgr)
{
    (void)env;
    return mgr;                 /* the handle the other AAsset stubs ignore */
}
/* The rest of the AAsset surface libUE4 binds. Same position as
 * AAssetManager_open: named so the library relocates, honest that there are no
 * assets behind it. */
void AAssetDir_close(void *dir) { (void)dir; }
void *AAssetManager_openDir(void *mgr, const char *dirname)
{
    (void)mgr;
    tl_log_line("asset: directory %s requested -- AAsset is not wired in the attempt", dirname);
    return NULL;
}
int AAsset_openFileDescriptor(void *asset, int64_t *out_start, int64_t *out_length)
{
    (void)asset;
    if (out_start) *out_start = 0;
    if (out_length) *out_length = 0;
    return -1;
}

/* -------------------------------------------------- configuration  --- */

/*
 * A value the guest holds and passes back. Nothing reads it -- the attempt has no
 * real configuration -- so the getters answer with the default locale.
 *
 * The getters write exactly TWO bytes and no terminator, which is what bionic's
 * own implementation does (`outLanguage[0] = config->language[0]; [1] = ...`).
 * Writing a third byte "to be safe" would be an overflow into whatever the
 * caller put after its two-character buffer.
 */
void *AConfiguration_new(void) { return calloc(1, 16); }
void AConfiguration_delete(void *config) { free(config); }
void AConfiguration_fromAssetManager(void *out, void *mgr) { (void)out; (void)mgr; }
void AConfiguration_getLanguage(void *config, char *out)
{
    (void)config;
    if (!out) return;
    out[0] = 'e'; out[1] = 'n';
}
void AConfiguration_getCountry(void *config, char *out)
{
    (void)config;
    if (!out) return;
    out[0] = 'U'; out[1] = 'S';
}

/* --------------------------------------------------------- libc  ---- */

/*
 * pthread_mutex_t on bionic arm64 is 40 bytes; Darwin's is far larger.
 * Guests embed the type in their own structs, so the shim must fit IN 40
 * bytes, not hand out the host's. The attempt builds a spinlock inside
 * it: byte 0 is the lock byte, then the owner and a recursion count.
 *
 * Recursion has to be real, not reported. libUE4.so's crash-malloc
 * constructor asks for PTHREAD_MUTEX_RECURSIVE and its arena allocator
 * then locks that mutex re-entrantly -- which is the whole point of the
 * type. Serving that request with a plain spinlock wedges the main thread
 * on the second lock: no crash, no signal, just a black screen at 0 fps.
 */
/* The type lives in the single int bionic keeps it in, and now survives into the mutex. */
#define TL_MUTEX_DEFAULT     0
#define TL_MUTEX_RECURSIVE   1
#define TL_MUTEX_ERRORCHECK  2

typedef struct {
    uint8_t  lock;        /* 0 free, 1 held -- the spinlock byte */
    uint8_t  pad0[3];
    uint32_t owner;       /* the holding thread, 0 when free */
    uint32_t count;       /* recursion depth */
    uint32_t type;        /* TL_MUTEX_DEFAULT / _RECURSIVE / _ERRORCHECK */
    uint8_t  pad1[24];    /* the rest of bionic's 40 bytes */
} tl_mutex40;

static uint32_t tl_thread_id(void) { return (uint32_t)(uintptr_t)pthread_self(); }

int pthread_mutex_init_shim(void *m, const void *attr)
{
    if (!m) return EINVAL;
    memset(m, 0, 40);
    /* The attr is the single int the shim's own attr calls write, so the type
     * requested at init time is what the mutex ends up honouring. */
    ((tl_mutex40 *)m)->type = attr ? (uint32_t)*(const int *)attr : TL_MUTEX_DEFAULT;
    return 0;
}

static void spin_lock_40(uint8_t *b)
{
    while (__sync_bool_compare_and_swap(b, 0, 1) == false) {
        sched_yield();
    }
}

static void spin_unlock_40(uint8_t *b) { *b = 0; }

/*
 * Log the one lock that proves which path the arena allocator took.
 *
 * BinnedAllocFromOS takes a single static mutex at libUE4+0xaea6798 before it
 * decides anything. Seeing that lock proves the function was entered; seeing it
 * and no mmap afterwards proves the arena branch was taken with a zero-sized
 * request, which is the only way it can return null without ever mapping. That
 * distinction is otherwise invisible: the binding is provably right, the shim
 * provably works for other libc names, and the call simply never arrives.
 *
 * Logged once. The first entry is what matters and the function runs twice.
 */
static void note_arena_lock(void *m)
{
    static int n;
    if (!m) {
        return;
    }
    char where[128];
    if (!husk_tl_describe_address((uintptr_t)m, where, sizeof(where))) {
        snprintf(where, sizeof(where), "%p", m);
    }
    /* Log the ones that matter always, and a bounded sample of the rest: a filter
     * that matches nothing produces the same empty log as a call that never
     * happened, and telling those apart is the entire point. */
    bool arena = !strcmp(where, "libUE4.so+0xaea6798");
    if (arena || n < 40) {
        tl_log_line("mutex: lock %s%s", where,
                    arena ? "   <<< the arena allocator's static mutex" : "");
        n++;
    }
}

/*
 * Locking honours the type. A recursive mutex re-locked by the thread that already
 * holds it just deepens the count -- which is the behaviour the engine is relying on
 * and the reason the type was requested. A default mutex spins on the second lock
 * from the same thread, which is what a default mutex is supposed to do.
 */
int pthread_mutex_lock_shim(void *m)
{
    note_arena_lock(m);
    if (!m) return EINVAL;
    tl_mutex40 *mx = (tl_mutex40 *)m;
    uint32_t me = tl_thread_id();
    /* Only the owner can have written `owner`, so reading it without the lock is safe
     * and saves taking the lock just to discover we already hold it. */
    if (mx->type == TL_MUTEX_RECURSIVE && mx->count && mx->owner == me) {
        mx->count++;
        return 0;
    }
    spin_lock_40(&mx->lock);
    mx->owner = me;
    mx->count = 1;
    return 0;
}
int pthread_mutex_trylock_shim(void *m)
{
    if (!m) return EINVAL;
    tl_mutex40 *mx = (tl_mutex40 *)m;
    uint32_t me = tl_thread_id();
    if (mx->type == TL_MUTEX_RECURSIVE && mx->count && mx->owner == me) {
        mx->count++;
        return 0;
    }
    if (!__sync_bool_compare_and_swap(&mx->lock, 0, 1)) return EBUSY;
    mx->owner = me;
    mx->count = 1;
    return 0;
}
int pthread_mutex_unlock_shim(void *m)
{
    if (!m) return EINVAL;
    tl_mutex40 *mx = (tl_mutex40 *)m;
    if (mx->count > 1) {           /* still held recursively: one level down */
        mx->count--;
        return 0;
    }
    mx->count = 0;
    mx->owner = 0;
    spin_unlock_40(&mx->lock);     /* release last, once the fields are settled */
    return 0;
}
int pthread_mutex_destroy_shim(void *m) { if (m) memset(m, 0, 40); return 0; }

/*
 * pthread_mutexattr_t is the other half of that mismatch, and leaving it to Darwin is
 * worse than leaving the mutex there: the attr is usually a LOCAL, so the overflow lands
 * on the guest's own stack frame.
 *
 * bionic's pthread_mutexattr_t is one int (4 bytes). Darwin's is 64. So the ordinary
 *
 *     pthread_mutexattr_t attr;            // 4 bytes of stack
 *     pthread_mutexattr_init(&attr);       // Darwin writes 64
 *     pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
 *     pthread_mutex_init(&m, &attr);
 *     pthread_mutexattr_destroy(&attr);
 *
 * overwrote 60 bytes of saved registers, frame pointer and stack canary. libUE4.so's
 * 18th constructor does exactly this -- it is the run that ends inside it with no fault
 * line, because the corrupted return faults where the JIT guard owns the signal (SIGBUS
 * is deliberately left to it) and nothing gets logged.
 *
 * The type is kept in the single int bionic keeps it in, and pthread_mutex_init now
 * carries it into the mutex, so a recursive request is honoured rather than reported.
 */
int pthread_mutexattr_init_shim(void *a)
{
    if (!a) return EINVAL;
    *(int *)a = TL_MUTEX_DEFAULT;
    return 0;
}
int pthread_mutexattr_destroy_shim(void *a)
{
    if (!a) return EINVAL;
    *(int *)a = TL_MUTEX_DEFAULT;
    return 0;
}
int pthread_mutexattr_settype_shim(void *a, int type)
{
    if (!a) return EINVAL;
    if (type != TL_MUTEX_DEFAULT && type != TL_MUTEX_RECURSIVE && type != TL_MUTEX_ERRORCHECK) {
        return EINVAL;
    }
    if (type != TL_MUTEX_DEFAULT) {
        static int said;
        if (!said) {          /* informative, not per-instance -- it fires for every mutex */
            said = 1;
            tl_log_line("pthread: mutex type %d requested -- the mutex honours it", type);
        }
    }
    *(int *)a = type;
    return 0;
}
int pthread_mutexattr_gettype_shim(const void *a, int *type)
{
    if (!a || !type) return EINVAL;
    *type = *(const int *)a;
    return 0;
}

/*
 * The rest of the pthread types are the same trap, and there are five of them.
 *
 * bionic and Darwin disagree on the SIZE of most pthread objects, and a guest embeds
 * bionic's in its own structs and stack frames. Handing that memory to Darwin lets it
 * write past the end, and the overflow lands on whatever the guest put next -- which is
 * how an 8-byte pthread_key_t store into a 4-byte slot at sp+4 destroyed the stack
 * canary at sp+8 in libUE4.so's constructor 247, and the process died in
 * __stack_chk_fail through a signal the JIT guard owns, logging nothing.
 *
 *   type                 bionic  Darwin
 *   pthread_key_t             4       8     <- the value fits; widen and truncate
 *   pthread_attr_t           52      64
 *   pthread_rwlock_t         56     200
 *   pthread_condattr_t        8      16
 *   pthread_rwlockattr_t      8      24
 *   pthread_cond_t           48      48     <- equal, so conds pass straight through
 *
 * For the objects, the guest's own bytes are never handed to Darwin. They hold a
 * POINTER to a heap-allocated Darwin object instead, which has a pleasant property: a
 * statically initialised object (PTHREAD_RWLOCK_INITIALIZER) is all zeros, i.e. a NULL
 * pointer, which reads as "not created yet", so lazy creation on first use falls out
 * for free. Creation is done with a compare-and-swap on the slot, because two threads
 * racing to create it would otherwise end up locking two different rwlocks and the
 * mutual exclusion would silently not exist.
 */
static void *tl_side_create(void *guest, size_t bytes, void (*init)(void *))
{
    void *p = *(void **)guest;
    if (p) return p;
    void *n = calloc(1, bytes);
    if (!n) return NULL;
    if (init) init(n);
    /* Losing the race means another thread installed its own object first; ours is
     * unused and must be discarded, or the two would never see each other. */
    if (!__sync_bool_compare_and_swap((void **)guest, NULL, n)) {
        free(n);
        return *(void **)guest;
    }
    return n;
}

/* ---- pthread_key_t: 4 bytes on bionic, 8 on Darwin ---------------------- */

int pthread_key_create_shim(int *key, void (*destructor)(void *))
{
    pthread_key_t real = 0;
    int r = pthread_key_create(&real, destructor);
    if (r == 0 && key) *key = (int)real;      /* four bytes, never eight */
    return r;
}
int pthread_key_delete_shim(int key)      { return pthread_key_delete((pthread_key_t)key); }
int pthread_setspecific_shim(int key, const void *value)
{ return pthread_setspecific((pthread_key_t)key, value); }
void *pthread_getspecific_shim(int key)   { return pthread_getspecific((pthread_key_t)key); }

/* ---- pthread_attr_t: 52 on bionic, 64 on Darwin ------------------------ */

static void attr_init0(void *p) { pthread_attr_init((pthread_attr_t *)p); }

int pthread_attr_init_shim(void *guest)
{
    if (!guest) return EINVAL;
    return tl_side_create(guest, sizeof(pthread_attr_t), attr_init0) ? 0 : ENOMEM;
}
int pthread_attr_destroy_shim(void *guest)
{
    if (!guest) return EINVAL;
    pthread_attr_t *p = *(pthread_attr_t **)guest;
    if (p) { pthread_attr_destroy(p); free(p); *(void **)guest = NULL; }
    return 0;
}
int pthread_attr_setdetachstate_shim(void *guest, int v)
{
    pthread_attr_t *p = guest ? tl_side_create(guest, sizeof(pthread_attr_t), attr_init0) : NULL;
    return p ? pthread_attr_setdetachstate(p, v) : EINVAL;
}
int pthread_attr_setstacksize_shim(void *guest, size_t v)
{
    pthread_attr_t *p = guest ? tl_side_create(guest, sizeof(pthread_attr_t), attr_init0) : NULL;
    return p ? pthread_attr_setstacksize(p, v) : EINVAL;
}
/* bionic has no scheduling policy on the attr that iOS acts on; accepting it keeps the
 * guest's sequence intact instead of failing a call it does not check. */
int pthread_attr_setschedpolicy_shim(void *guest, int v)
{
    (void)guest; (void)v;
    return 0;
}
int pthread_attr_getstack_shim(const void *guest, void **addr, size_t *size)
{
    if (!guest) return EINVAL;
    pthread_attr_t *p = *(pthread_attr_t **)(void *)guest;
    if (!p) return EINVAL;
    return pthread_attr_getstack(p, addr, size);
}

/*
 * pthread_create has to be ours the moment the attr is ours.
 *
 * The guest's pthread_attr_t now holds a POINTER to the Darwin attr rather than the attr
 * itself, so handing those 52 bytes to Darwin's pthread_create would have it interpret a
 * heap address as an attr structure -- a good way to get a nonsense stack size and a
 * thread that never starts. NULL still means "default attributes", which is the common
 * case and must keep working.
 */
int pthread_create_shim(void *thread, const void *guest_attr,
                        void *(*fn)(void *), void *arg)
{
    pthread_attr_t *a = guest_attr ? *(pthread_attr_t **)(void *)guest_attr : NULL;
    return pthread_create((pthread_t *)thread, a, fn, arg);
}

/* ---- pthread_rwlock_t: 56 on bionic, 200 on Darwin --------------------- */

static void rwlock_init0(void *p) { pthread_rwlock_init((pthread_rwlock_t *)p, NULL); }

static pthread_rwlock_t *rwlock_of(void *guest)
{
    return guest ? tl_side_create(guest, sizeof(pthread_rwlock_t), rwlock_init0) : NULL;
}
int pthread_rwlock_init_shim(void *guest, const void *attr)
{
    (void)attr;                    /* bionic's rwlockattr is not used by these guests */
    return rwlock_of(guest) ? 0 : ENOMEM;
}
int pthread_rwlock_destroy_shim(void *guest)
{
    if (!guest) return EINVAL;
    pthread_rwlock_t *p = *(pthread_rwlock_t **)guest;
    if (p) { pthread_rwlock_destroy(p); free(p); *(void **)guest = NULL; }
    return 0;
}
int pthread_rwlock_rdlock_shim(void *guest)
{
    pthread_rwlock_t *p = rwlock_of(guest);
    return p ? pthread_rwlock_rdlock(p) : EINVAL;
}
int pthread_rwlock_wrlock_shim(void *guest)
{
    pthread_rwlock_t *p = rwlock_of(guest);
    return p ? pthread_rwlock_wrlock(p) : EINVAL;
}
int pthread_rwlock_unlock_shim(void *guest)
{
    pthread_rwlock_t *p = rwlock_of(guest);
    return p ? pthread_rwlock_unlock(p) : EINVAL;
}

/* ---- pthread_condattr_t: 8 on bionic, 16 on Darwin -------------------- */

static void condattr_init0(void *p) { pthread_condattr_init((pthread_condattr_t *)p); }

int pthread_condattr_init_shim(void *guest)
{
    if (!guest) return EINVAL;
    return tl_side_create(guest, sizeof(pthread_condattr_t), condattr_init0) ? 0 : ENOMEM;
}
int pthread_condattr_destroy_shim(void *guest)
{
    if (!guest) return EINVAL;
    pthread_condattr_t *p = *(pthread_condattr_t **)guest;
    if (p) { pthread_condattr_destroy(p); free(p); *(void **)guest = NULL; }
    return 0;
}
int pthread_condattr_setclock_shim(void *guest, int clock_id)
{
    pthread_condattr_t *p = guest ? tl_side_create(guest, sizeof(pthread_condattr_t), condattr_init0) : NULL;
    if (!p) return EINVAL;
    /* CLOCK_MONOTONIC is 1 on bionic and 6 on Darwin, so translating the number would
     * be guesswork; iOS condition variables are monotonic already, which is what the
     * guest is asking for, so the request is already satisfied. */
    (void)clock_id;
    return 0;
}

/*
 * pthread_cond_t is 48 bytes on BOTH platforms -- but passing it through is still wrong,
 * because Darwin's pthread_cond_wait also manipulates the MUTEX it is handed: it reads
 * and writes Darwin's own mutex layout, owner field and all. The guest's mutex is
 * bionic's 40 bytes holding this shim's spinlock, so handing that pair to Darwin either
 * corrupts the mutex or blocks forever.
 *
 * That is the shape of the current wall. ANativeActivity_onCreate is the NDK's
 * native_app_glue: it creates the message pipe, spawns the game thread with
 * pthread_create, and then waits on a condition variable until that thread reports
 * itself running -- pthread_cond_wait(&app->cond, &app->mutex), on a guest mutex.
 *
 * So each condition gets its own Darwin condition AND a Darwin mutex of its own. The
 * wait releases the guest's mutex, blocks on the Darwin one, and re-acquires -- the
 * standard way to build a condition over a mutex you do not own. Holding the condition's
 * internal mutex across the release-and-wait is what closes the race where a signal
 * arriving in between would be lost.
 *
 * The timeout passes through unchanged: bionic and Darwin both interpret the absolute
 * time in pthread_cond_timedwait against the realtime clock, which is the default for
 * both. (A guest that asked for CLOCK_MONOTONIC through the attr would need it
 * converted, but that request comes only from Flutter-built APKs, not from this one.)
 */
typedef struct { pthread_cond_t c; pthread_mutex_t m; } tl_cond;

static void cond_init0(void *p)
{
    pthread_cond_init(&((tl_cond *)p)->c, NULL);
    pthread_mutex_init(&((tl_cond *)p)->m, NULL);
}
static tl_cond *cond_of(void *g)
{
    return g ? tl_side_create(g, sizeof(tl_cond), cond_init0) : NULL;
}

int pthread_cond_init_shim(void *cond, const void *attr)
{
    (void)attr;                    /* the guest's condattr is 8 bytes; Darwin reads 16 */
    return cond_of(cond) ? 0 : ENOMEM;
}
int pthread_cond_destroy_shim(void *cond)
{
    if (!cond) return EINVAL;
    tl_cond *c = *(tl_cond **)cond;
    if (c) {
        pthread_cond_destroy(&c->c);
        pthread_mutex_destroy(&c->m);
        free(c);
        *(void **)cond = NULL;
    }
    return 0;
}
int pthread_cond_wait_shim(void *cond, void *mutex)
{
    tl_cond *c = cond_of(cond);
    if (!c || !mutex) return EINVAL;
    pthread_mutex_lock(&c->m);
    pthread_mutex_unlock_shim(mutex);          /* the guest's mutex, which is ours */
    pthread_cond_wait(&c->c, &c->m);
    pthread_mutex_unlock(&c->m);
    pthread_mutex_lock_shim(mutex);
    return 0;
}
int pthread_cond_timedwait_shim(void *cond, void *mutex, const struct timespec *abstime)
{
    tl_cond *c = cond_of(cond);
    if (!c || !mutex) return EINVAL;
    pthread_mutex_lock(&c->m);
    pthread_mutex_unlock_shim(mutex);
    int r = pthread_cond_timedwait(&c->c, &c->m, abstime);
    pthread_mutex_unlock(&c->m);
    pthread_mutex_lock_shim(mutex);
    return r;
}
/*
 * Signal and broadcast take the condition's own mutex, and that is not decoration.
 *
 * There is a window between the waiter releasing the guest's mutex and blocking on the
 * Darwin condition. Hold the condition's internal mutex across both sides and the
 * hand-off is ordered: the signal either lands while the waiter still holds it (so the
 * waiter is about to block with the signal already banked) or while it is genuinely
 * waiting. Without it a signal arriving inside that window is delivered to nobody and
 * the waiter blocks forever -- the same failure this whole change exists to fix.
 */
int pthread_cond_signal_shim(void *cond)
{
    tl_cond *c = cond_of(cond);
    if (!c) return EINVAL;
    pthread_mutex_lock(&c->m);
    int r = pthread_cond_signal(&c->c);
    pthread_mutex_unlock(&c->m);
    return r;
}
int pthread_cond_broadcast_shim(void *cond)
{
    tl_cond *c = cond_of(cond);
    if (!c) return EINVAL;
    pthread_mutex_lock(&c->m);
    int r = pthread_cond_broadcast(&c->c);
    pthread_mutex_unlock(&c->m);
    return r;
}

/* The timed variant of the same 40-byte mutex, and it has to be built on the
 * compare-and-swap rather than Darwin's pthread_mutex_timedlock: the object is
 * bionic's 40 bytes and Darwin's pthread_mutex_t is larger, so handing it over
 * would read past the guest's mutex. Try, nap, give up at the caller's deadline. */
int pthread_mutex_timedlock_shim(void *m, const struct timespec *deadline)
{
    if (!m) return EINVAL;
    for (;;) {
        if (__sync_bool_compare_and_swap((uint8_t *)m, 0, 1)) {
            return 0;
        }
        if (deadline) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline->tv_sec
                || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
                return ETIMEDOUT;
            }
        }
        struct timespec nap = { 0, 1000000 };       /* a millisecond */
        nanosleep(&nap, NULL);
    }
}

/* pthread_once_t is 4 bytes on bionic, 8 on Darwin (long). Same shape of
 * problem, same answer: 0 is "never run", 1 is "done", and the compare-
 * and-swap makes it safe enough for guest code. */
int pthread_once_shim(void *once, void (*fn)(void))
{
    if (!once || !fn) return EINVAL;
    int32_t *state = once;
    if (__sync_bool_compare_and_swap(state, 0, 2)) {
        fn();
        __sync_synchronize();
        *state = 1;
    } else {
        while (*state == 2) {
            sched_yield();
        }
    }
    return 0;
}

/*
 * sysconf: the guest asks with BIONIC's numbers, not Darwin's.
 *
 * bionic's _SC_PAGESIZE is 0x27 (39) and _SC_PAGE_SIZE is 0x28 (40); Darwin's are
 * 29 and 30. Answering only Darwin's meant a guest asking for the page size fell
 * through to sysconf(39) on Darwin -- an undefined name, which returns -1.
 *
 * That -1 is not harmless. FAndroidPlatformMemory::GetConstants() calls
 * sysconf(0x27) and stores the result in its page-size field, which UE4 then uses
 * as an ALIGNMENT: Align(0x200000, 0xFFFFFFFF) is 0 in 32-bit arithmetic. So the
 * engine's pool allocations came out as zero-size requests, and BinnedAllocFromOS
 * serves a zero-size request from its (still empty) arena without ever calling
 * mmap -- handing back the arena's cursor, which is zero. Hence a null pool base
 * with no mmap at all, which is what the crash showed.
 *
 * 16 KiB, not 4096: this host really has 16 KiB pages, and the arena unmaps in
 * page-size multiples, so a smaller answer would misalign Darwin's munmap.
 */
long sysconf_shim(int name)
{
    switch (name) {
    case 39:  /* _SC_PAGESIZE  (bionic: 0x27) */
    case 40:  /* _SC_PAGE_SIZE (bionic: 0x28) */
    case 29:  /* the same two names as Darwin numbers them, for a guest that uses those */
    case 30:
        return 16384;
    case 84:  /* _SC_NPROCESSORS_ONLN -- the same value in bionic and Darwin */
        return (long)sysconf(_SC_NPROCESSORS_ONLN);
    default:
        return sysconf(name);
    }
}

/* Darwin's is already 16 KiB on this host, but say so rather than depend on it: a
 * guest that read 4096 here would misalign every munmap the arena makes. */
long getpagesize_shim(void) { return 16384; }

void *dlopen_shim(const char *path, int mode)
{
    (void)mode;
    tl_log_line("shim: guest asked to dlopen %s -- not provided", path ? path : "(self)");
    return NULL;
}

void *dlsym_shim(void *handle, const char *name)
{
    (void)handle;
    return tl_shim_find(name);
}

extern int *__error(void);

static unsigned long getauxval_shim(unsigned long type)
{
    if (type == 16) { /* AT_HWCAP */
        return (1ul << 0) | (1ul << 1) | (1ul << 2) | (1ul << 3) |
               (1ul << 4) | (1ul << 5) | (1ul << 7);
    }
    if (type == 6) { /* AT_PAGESZ */
        return 16384;
    }
    return 0;
}

static int *__errno_shim(void)
{
    return __error();
}

static int __system_property_get_shim(const char *name, char *value)
{
    if (!name || !value) return 0;
    if (!strcmp(name, "ro.build.version.sdk")) {
        strcpy(value, "33");
        return (int)strlen(value);
    }
    value[0] = '\0';
    return 0;
}

static size_t __strlen_chk_shim(const char *s, size_t maxlen)
{
    size_t len = strlen(s);
    if (len >= maxlen) abort();
    return len;
}

static void *__memmove_chk_shim(void *dst, const void *src, size_t len, size_t dstlen)
{
    if (len > dstlen) abort();
    return memmove(dst, src, len);
}

static int __vsnprintf_chk_shim(char *s, size_t maxlen, int flag, size_t slen, const char *format, va_list args)
{
    (void)flag; (void)slen;
    return vsnprintf(s, maxlen, format, args);
}

static void android_set_abort_message_shim(const char *msg)
{
    tl_log_line("abort: %s", msg ? msg : "");
}

static void __cxa_finalize_shim(void *d) { (void)d; }
static int __cxa_atexit_shim(void (*fn)(void *), void *arg, void *d) { (void)fn; (void)arg; (void)d; return 0; }
static int dl_iterate_phdr_shim(void *cb, void *data) { (void)cb; (void)data; return 0; }

static uint64_t g_sF_storage[24]; /* 3 fake FILE structs */
static FILE *map_stream(void *s) {
    if (s == &g_sF_storage[0]) return stdin;
    if (s == &g_sF_storage[8]) return stdout;
    if (s == &g_sF_storage[16]) return stderr;
    return (FILE *)s;
}

static int fprintf_shim(void *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(map_stream(stream), fmt, ap);
    va_end(ap);
    return r;
}

static int vfprintf_shim(void *stream, const char *fmt, va_list ap) {
    return vfprintf(map_stream(stream), fmt, ap);
}

static int fputc_shim(int c, void *stream) {
    return fputc(c, map_stream(stream));
}

static int fflush_shim(void *stream) {
    return fflush(stream ? map_stream(stream) : NULL);
}

static size_t fwrite_shim(const void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, map_stream(stream));
}

/* bionic's stdio globals, which Darwin does not export under these names.
 *
 * On Darwin `stdout` is a macro for `__stdoutp`, so dlsym(RTLD_DEFAULT,
 * "stdout") finds nothing -- which is why libUE4.so could not relocate. The shim
 * already had three fake FILE structs and map_stream() already turned them into
 * the real streams; they were simply never reachable by name. bionic lays them
 * out as __sF[0]=stdin, [1]=stdout, [2]=stderr and g_sF_storage is those three
 * back to back, so the addresses handed out here are exactly the ones
 * map_stream() recognises. */
#define TL_STREAM_SLOT(n) ((void *)&g_sF_storage[(n) * 8])

/* bionic's ctype table. Guest code indexes it directly -- (_ctype_ + 1)[c] --
 * so it has to be the real 257-byte table with bionic's flag bits, not a pointer
 * into Darwin's rune locale, whose layout and bits are both different. */
#define TL_CTYPE_U 0x01   /* upper */
#define TL_CTYPE_L 0x02   /* lower */
#define TL_CTYPE_D 0x04   /* digit */
#define TL_CTYPE_S 0x08   /* space */
#define TL_CTYPE_P 0x10   /* punct */
#define TL_CTYPE_C 0x20   /* control */
#define TL_CTYPE_X 0x40   /* hex digit */
#define TL_CTYPE_B 0x80   /* blank */
static const char g_ctype[257] = {
    0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x88, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
    0x44, 0x44, 0x44, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x10, 0x10, 0x10, 0x10,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
};

/* The fortified libc entry points (_FORTIFY_SOURCE). bionic ships them; Darwin
 * has no equivalent, so every one is an unresolved import for an Android binary
 * built with fortify on -- and libUE4.so imports a dozen. The checks that matter
 * (a length that does not fit its buffer) abort, as bionic's do. */
static void *__memcpy_chk_shim(void *dst, const void *src, size_t n, size_t dstlen) {
    if (n > dstlen) abort();
    return memcpy(dst, src, n);
}
static void *__memset_chk_shim(void *dst, int c, size_t n, size_t dstlen) {
    if (n > dstlen) abort();
    return memset(dst, c, n);
}
static char *__strcat_chk_shim(char *dst, const char *src, size_t dstlen) {
    if (strlen(dst) + strlen(src) + 1 > dstlen) abort();
    return strcat(dst, src);
}
static char *__strcpy_chk_shim(char *dst, const char *src, size_t dstlen) {
    if (strlen(src) + 1 > dstlen) abort();
    return strcpy(dst, src);
}
static char *__strncat_chk_shim(char *dst, const char *src, size_t n, size_t dstlen) {
    if (strlen(dst) + strnlen(src, n) + 1 > dstlen) abort();
    return strncat(dst, src, n);
}
static char *__strncpy_chk_shim(char *dst, const char *src, size_t n, size_t dstlen) {
    if (n > dstlen) abort();
    return strncpy(dst, src, n);
}
/* bionic's strchr_chk walks until it finds the character or the terminator and
 * only faults if it runs off the end of the declared buffer first. */
static char *__strchr_chk_shim(const char *s, int c, size_t slen) {
    for (;; s++, slen--) {
        if (slen == 0) abort();
        if (*s == (char)c) return (char *)s;
        if (*s == '\0') return NULL;
    }
}
static char *__strrchr_chk_shim(const char *s, int c, size_t slen) {
    const char *save = NULL;
    for (;; s++, slen--) {
        if (slen == 0) abort();
        if (*s == (char)c) save = s;
        if (*s == '\0') return (char *)save;
    }
}
static int __vsprintf_chk_shim(char *dst, int flag, size_t dstlen, const char *fmt, va_list ap) {
    (void)flag;
    int n = vsnprintf(dst, dstlen, fmt, ap);
    if (n < 0 || (size_t)n >= dstlen) abort();
    return n;
}
static ssize_t __read_chk_shim(int fd, void *buf, size_t count, size_t buflen) {
    if (count > buflen) abort();
    return read(fd, buf, count);
}
static ssize_t __write_chk_shim(int fd, const void *buf, size_t count, size_t buflen) {
    if (count > buflen) abort();
    return write(fd, buf, count);
}
static ssize_t __pread_chk_shim(int fd, void *buf, size_t count, off_t off, size_t buflen) {
    if (count > buflen) abort();
    return pread(fd, buf, count, off);
}
static ssize_t __pread64_chk_shim(int fd, void *buf, size_t count, off_t off, size_t buflen) {
    if (count > buflen) abort();
    return pread(fd, buf, count, off);
}
static ssize_t __pwrite_chk_shim(int fd, const void *buf, size_t count, off_t off, size_t buflen) {
    if (count > buflen) abort();
    return pwrite(fd, buf, count, off);
}
static ssize_t __pwrite64_chk_shim(int fd, const void *buf, size_t count, off_t off, size_t buflen) {
    if (count > buflen) abort();
    return pwrite(fd, buf, count, off);
}
static void __FD_SET_chk_shim(int fd, fd_set *set, size_t setlen) {
    if (fd < 0 || (size_t)fd >= setlen * 8) abort();
    FD_SET(fd, set);
}
static int __FD_ISSET_chk_shim(int fd, const fd_set *set, size_t setlen) {
    if (fd < 0 || (size_t)fd >= setlen * 8) abort();
    return FD_ISSET(fd, set);
}
/* __open_2 is open() without the mode argument, so O_CREAT is the mistake it
 * exists to catch. */
static int __open_2_shim(const char *path, int flags) {
    if (flags & O_CREAT) abort();
    return open(path, flags);
}

/*
 * libc++ asks how many bytes a multibyte character can be, through a name that
 * exists in glibc and bionic but not on Darwin. MB_CUR_MAX is the same answer
 * the real function gives for the current locale.
 *
 * One symbol, and it was enough to stop the whole APK: libc++_shared.so would
 * not relocate without it, so it never loaded, so libUE4.so's 55 std::__ndk1
 * imports had nothing to bind to and the primary library could not be loaded
 * either. When a single import in a helper refuses the helper, read what the
 * *helper* is missing before reading the primary's list -- the primary's is
 * usually the consequence.
 */
static size_t __ctype_get_mb_cur_max_shim(void) { return MB_CUR_MAX; }

/* bionic's second fortified strncpy: as well as the destination bound, the
 * compiler passed the source length it knows about. */
static char *__strncpy_chk2_shim(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) {
    if (n > dstlen) abort();
    if (strnlen(src, srclen) >= srclen) abort();
    return strncpy(dst, src, n);
}

/* bionic's gettid(): no Darwin symbol. The kernel thread id is what it returns,
 * and pthread_threadid_np is how Darwin reaches it. */
static pid_t gettid_shim(void) {
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (pid_t)tid;
}

/*
 * bionic's *64 aliases. Darwin's off_t is already 64-bit, so these are the
 * ordinary calls under the name the guest binds -- the bionic name is the only
 * thing missing, not the 64-bit behaviour.
 */
static int64_t lseek64_shim(int fd, int64_t offset, int whence)
{ return (int64_t)lseek(fd, (off_t)offset, whence); }
static int64_t pread64_shim(int fd, void *buf, size_t n, int64_t offset)
{ return (int64_t)pread(fd, buf, n, (off_t)offset); }
static int ftruncate64_shim(int fd, int64_t length)
{ return ftruncate(fd, (off_t)length); }

/* memalign is declared on Darwin but not exported by that name. */
static void *memalign_shim(size_t alignment, size_t size)
{
    void *p = NULL;
    return posix_memalign(&p, alignment, size) == 0 ? p : NULL;
}

/* glibc has sincos; Darwin only has __sincos, so the plain names are missing. */
static void sincos_shim(double x, double *s, double *c)
{ if (s) *s = sin(x); if (c) *c = cos(x); }
static void sincosf_shim(float x, float *s, float *c)
{ if (s) *s = sinf(x); if (c) *c = cosf(x); }

/* ------------------------------------------------------ mmap, with notes --- */

/*
 * mmap and munmap as logging passthroughs.
 *
 * FAndroidPlatformMemory::BinnedAllocFromOS keeps one large mapped region and
 * carves slices out of it. When UE4's crash allocator asked it for its pool the
 * answer was NULL; the constructor noticed, logged an error, and then called
 * InitializeSmallPools() on the null anyway. So the allocation is the bug and
 * this is how to see it: the size, the flags, and the errno when it fails.
 * Android accepts flag bits Darwin does not, and a size computed from a rodata
 * table can come out absurd, so both are worth a line each.
 *
 * Every call is logged, with the caller's address. The first version filtered on
 * size, which was wrong twice over: it hid any small mapping, and when no line
 * appeared at all it could not tell "never called" from "called below the
 * threshold" -- which is exactly the ambiguity it produced. A guest's mmap count
 * is in the dozens, so the whole set stays readable.
 *
 * __builtin_return_address(0) is the guest's return address, not the shim's: the
 * PLT stub branches to us instead of calling, so x30 still holds the site that
 * made the call. That names the guest function that asked, which is the one thing
 * the previous build could not say.
 */

#include <sys/mman.h>

static void mmap_prot_str(int prot, char *out, size_t len)
{
    snprintf(out, len, "%s%s%s",
             (prot & PROT_READ)  ? "r" : "-",
             (prot & PROT_WRITE) ? "w" : "-",
             (prot & PROT_EXEC)  ? "x" : "-");
}

/* The call site, as "libUE4.so+0x6257e9c", so the log says which guest function
 * asked without a second pass over the ELF tables.
 *
 * The address is passed in rather than read with __builtin_return_address(1): at
 * -O2 the level-1 walk needs frame pointers that may not be there, whereas the
 * value read inside mmap_shim itself is the guest's own return address, because
 * the PLT stub branches here instead of calling. */
static const char *mmap_caller(void *addr)
{
    static _Thread_local char buf[128];
    if (!husk_tl_describe_address((uintptr_t)addr, buf, sizeof(buf))) {
        snprintf(buf, sizeof(buf), "%p", addr);
    }
    return buf;
}

/*
 * mmap's FLAGS are numbered differently, and here the difference is fatal rather than
 * cosmetic. bionic puts MAP_ANONYMOUS at 0x20; Darwin puts MAP_ANON at 0x1000 and calls
 * 0x20 MAP_RENAME, which it no longer supports. So forwarding a guest's
 * MAP_PRIVATE|MAP_ANONYMOUS (0x22) verbatim makes Darwin fail the call with EINVAL, and
 * the guest builds an allocator arena on a -1 pointer.
 *
 * Translate rather than pass through, and build the result from scratch: Android-only
 * bits collide with unrelated Darwin ones (bionic's MAP_EXECUTABLE is 0x1000, which is
 * Darwin's MAP_ANON), so an unrecognised bit has to be dropped, never forwarded.
 * bionic's MAP_32BIT (0x40) is meaningless on iOS and is dropped too.
 */
static int mmap_flags_to_darwin(int bionic)
{
    int d = 0;
    if (bionic & 0x0001) d |= 0x0001;   /* MAP_SHARED  -- same */
    if (bionic & 0x0002) d |= 0x0002;   /* MAP_PRIVATE -- same */
    if (bionic & 0x0010) d |= 0x0010;   /* MAP_FIXED   -- same */
    if (bionic & 0x0020) d |= 0x1000;   /* MAP_ANONYMOUS -> Darwin's MAP_ANON */
    if (bionic & 0x4000) d |= 0x0040;   /* MAP_NORESERVE */
    return d;
}

void *mmap_shim(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    errno = 0;
    int dflags = mmap_flags_to_darwin(flags);
    void *p = mmap(addr, len, prot, dflags, fd, (off_t)off);
    int err = errno;
    const char *from = mmap_caller(__builtin_return_address(0));

    char pr[4];
    mmap_prot_str(prot, pr, sizeof(pr));

    /* MAP_FAILED is (void *)-1, so a truthiness test reports a failure as a success
     * and prints a -1 pointer as if it were an address. */
    if (p != MAP_FAILED) {
        tl_log_line("mmap: len=%zu (%zu KiB) prot=%s flags=%#x (darwin %#x) fd=%d off=%ld -> %p  from %s",
                    len, len / 1024, pr, (unsigned)flags, (unsigned)dflags, fd, off, p, from);
    } else {
        tl_log_line("mmap: len=%zu (%zu KiB) prot=%s flags=%#x (darwin %#x) fd=%d off=%ld -> FAILED, "
                    "errno %d  from %s", len, len / 1024, pr, (unsigned)flags, (unsigned)dflags, fd, off,
                    err, from);
    }

    errno = err;
    return p;
}

int munmap_shim(void *addr, size_t len)
{
    errno = 0;
    int rc = munmap(addr, len);
    int err = errno;
    const char *from = mmap_caller(__builtin_return_address(0));
    tl_log_line("munmap: addr=%p len=%zu -> %d%s  from %s", addr, len, rc,
                rc ? " (FAILED)" : "", from);
    errno = err;
    return rc;
}

/* ------------------------------------------------------------------ /proc --- */

/*
 * The /proc files a game actually reads.
 *
 * FAndroidPlatformMemory::GetStats() opens /proc/meminfo and parses it with fgets
 * and atol. iOS has no /proc, so fopen returns NULL, the parse yields nothing, and
 * the engine concludes it has no memory at all -- which sizes UE4's crash-allocator
 * pools to zero and leaves a null base for the first store to land on. Half the
 * Android ecosystem reads the same file for the same reason, so it is worth
 * answering properly rather than stubbing.
 *
 * The content is written once to a real file in the process's own temporary
 * directory and a genuine FILE* is handed back: fgets, fseek and fclose all have to
 * keep working on it, and there is no portable way to fabricate a FILE*.
 *
 * A whitelist, not a general interception. fopen is on the hot path of every guest,
 * and an unlisted path falls through to Darwin's fopen and behaves exactly as it
 * did before this existed -- which is what makes the change safe to make at all.
 */

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

static uint64_t proc_physical_memory(void)
{
#if defined(__APPLE__)
    uint64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, NULL, 0) == 0 && bytes) {
        return bytes;
    }
#endif
    return 0;
}

static uint64_t proc_free_memory(void)
{
#if defined(__APPLE__)
    vm_size_t page = 0;
    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_page_size(mach_host_self(), &page) == KERN_SUCCESS
        && host_statistics64(mach_host_self(), HOST_VM_INFO64,
                             (host_info64_t)&vmstat, &count) == KERN_SUCCESS) {
        return (uint64_t)(vmstat.free_count + vmstat.purgeable_count) * (uint64_t)page;
    }
#endif
    return 0;
}

/* Never report zero: a guest that divides by it, or sizes a pool from it, is
 * worse off than one given a conservative guess. */
static void proc_memory_totals(uint64_t *total, uint64_t *freeb)
{
    uint64_t t = proc_physical_memory();
    uint64_t f = proc_free_memory();
    if (!t) t = 1024ull * 1024 * 1024;
    if (!f) f = t / 4;
    *total = t;
    *freeb = f;
}

static int proc_write_file(const char *path, const char *key)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return 0;
    }

    if (!strcmp(key, "meminfo")) {
        uint64_t total, freeb;
        proc_memory_totals(&total, &freeb);
        fprintf(f,
                "MemTotal:       %llu kB\n"
                "MemFree:        %llu kB\n"
                "MemAvailable:   %llu kB\n"
                "Buffers:               0 kB\n"
                "Cached:                0 kB\n"
                "SwapCached:            0 kB\n"
                "Active:                0 kB\n"
                "Inactive:              0 kB\n"
                "SwapTotal:             0 kB\n"
                "SwapFree:              0 kB\n",
                (unsigned long long)(total / 1024),
                (unsigned long long)(freeb / 1024),
                (unsigned long long)(freeb / 1024));
    } else if (!strcmp(key, "cpuinfo")) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        for (long i = 0; i < n; i++) {
            fprintf(f,
                    "processor\t: %ld\n"
                    "BogoMIPS\t: 38.40\n"
                    "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
                    "CPU implementer\t: 0x41\n"
                    "CPU architecture: 8\n"
                    "CPU variant\t: 0x0\n"
                    "CPU part\t: 0xd0b\n"
                    "CPU revision\t: 0\n\n", i);
        }
    } else if (!strcmp(key, "self-status")) {
        uint64_t rss = 0;
#if defined(__APPLE__)
        mach_task_basic_info_data_t info;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      (task_info_t)&info, &count) == KERN_SUCCESS) {
            rss = (uint64_t)info.resident_size;
        }
#endif
        fprintf(f,
                "Name:\tHusk\n"
                "State:\tR (running)\n"
                "Tgid:\t%ld\n"
                "Pid:\t%ld\n"
                "PPid:\t%ld\n"
                "Threads:\t1\n"
                "VmSize:\t%llu kB\n"
                "VmRSS:\t%llu kB\n",
                (long)getpid(), (long)getpid(), (long)getppid(),
                (unsigned long long)(rss / 1024),
                (unsigned long long)(rss / 1024));
    } else {
        fclose(f);
        return 0;
    }

    fclose(f);
    return 1;
}

FILE *fopen_shim(const char *path, const char *mode)
{
    /* Only a plain read of an absolute path is ours to answer. "r+"/"w"/"a" and
     * relative paths go straight to Darwin, so nothing that writes is affected. */
    bool plain_read = mode
        && mode[0] == 'r'
        && (mode[1] == '\0' || (mode[1] == 'b' && mode[2] == '\0'));
    if (!path || !plain_read || path[0] != '/') {
        return fopen(path, mode);
    }

    const char *key = NULL;
    if (!strcmp(path, "/proc/meminfo"))           key = "meminfo";
    else if (!strcmp(path, "/proc/cpuinfo"))      key = "cpuinfo";
    else if (!strcmp(path, "/proc/self/status"))  key = "self-status";

    if (!key) {
        return fopen(path, mode);
    }

    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) {
        tmpdir = "/tmp";
    }

    char file[512];
    snprintf(file, sizeof(file), "%s/husk-proc-%s", tmpdir, key);

    if (access(file, R_OK) != 0) {
        if (!proc_write_file(file, key)) {
            /* Could not synthesise one. Let the guest see the real failure rather
             * than a half-written file that parses to nonsense. */
            return fopen(path, mode);
        }
        tl_log_line("proc: %s has no iOS equivalent; serving a synthesised %s", path, file);
    }
    return fopen(file, mode);
}

/*
 * bionic's struct sysinfo, filled with the host's real numbers.
 *
 * This used to return -1, on the reasoning that a zeroed struct reads as real
 * information to a guest that acts on it. True -- but a failure reads as real
 * information too, and worse: FAndroidPlatformMemory asks this for total RAM, gets
 * nothing, and reports zero, which is one of the two ways the engine's crash
 * allocator ended up sizing its pools to zero. The fields below are the ones a
 * guest acts on.
 *
 * The layout is bionic's, 104 bytes on LP64: uptime, three loads, six ram fields,
 * procs/pad, two "high" fields, mem_unit, then glibc's _f padding which is empty
 * when long is 8 bytes.
 */
struct husk_sysinfo {
    long          uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int  mem_unit;
    char          _f[20 - 2 * sizeof(long) - sizeof(int)];
};

static int sysinfo_shim(void *info)
{
    if (!info) {
        return -1;
    }
    struct husk_sysinfo *si = (struct husk_sysinfo *)info;
    memset(si, 0, sizeof(*si));

    uint64_t total, freeb;
    proc_memory_totals(&total, &freeb);

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        si->uptime = (long)ts.tv_sec;
    }
    si->totalram  = (unsigned long)total;
    si->freeram   = (unsigned long)freeb;
    si->totalswap = 0;          /* iOS has no swap to report */
    si->freeswap  = 0;
    si->procs     = 1;
    si->mem_unit  = 1;          /* the values above are already in bytes */
    return 0;
}

static int sched_getaffinity_shim(int pid, size_t len, void *mask)
{ (void)pid; (void)len; (void)mask; return -1; }
static int sched_setaffinity_shim(int pid, size_t len, const void *mask)
{ (void)pid; (void)len; (void)mask; return -1; }
static int __sched_cpucount_shim(size_t setsize, const void *set)
{
    /* bionic's own answer is "how many CPUs are in this set". The shim cannot
     * see the guest's cpu_set_t, and one is the value that never claims more
     * parallelism than exists. */
    (void)setsize; (void)set;
    return 1;
}

/* Two more bionic-only libc entry points with no Darwin symbol of either name. */
static int __gnu_strerror_r_shim(int err, char *buf, size_t len) {
    if (!buf || !len) return EINVAL;
    snprintf(buf, len, "%s", strerror(err));
    return 0;
}
static int __libc_current_sigrtmin_shim(void) {
    return 32;      /* bionic's first real-time signal; nothing here delivers one */
}
static int __register_atfork_shim(void (*prepare)(void), void (*parent)(void),
                                  void (*child)(void), void *dso) {
    (void)dso;
    return pthread_atfork(prepare, parent, child);
}

/* OpenSL ES. libUE4.so binds slCreateEngine and six SL_IID_* interface IDs at
 * relocation time, none of them optional, and there is no OpenSL ES on iOS to
 * forward to -- so the library cannot load without these.
 *
 * The interface IDs are pointers the guest compares, so each has to be a
 * distinct, stable address; nothing dereferences them, because nothing here
 * hands out an engine. slCreateEngine reports the feature as unsupported rather
 * than returning a fake object: a fake engine gets dereferenced through a vtable
 * that does not exist, which turns "no sound" into a crash, where most engines
 * take a failed engine creation as "run silently". */
static const uint64_t g_sl_iids[8];
#define TL_SL_IID(n) ((void *)&g_sl_iids[n])
static int slCreateEngine_shim(void **engine, uint32_t nopts, const void *opts,
                               uint32_t nif, const void *ids, const void *req) {
    (void)nopts; (void)opts; (void)nif; (void)ids; (void)req;
    if (engine) *engine = NULL;
    tl_log_line("opensl: slCreateEngine -- no OpenSL ES here, so the game runs without sound");
    return 0x0000000C;      /* SL_RESULT_FEATURE_UNSUPPORTED */
}

/* Surface and HardwareBuffer stubs */
static void *ANativeWindow_fromSurface_shim(void *env, void *surface) { (void)env; (void)surface; return NULL; }
static void *ASurfaceControl_create_shim(void) { return NULL; }
static void *ASurfaceControl_createFromWindow_shim(void *p, const char *n) { (void)p; (void)n; return NULL; }
static void ASurfaceControl_release_shim(void *sc) { (void)sc; }
static void *ASurfaceTransaction_create_shim(void) { return NULL; }
static void ASurfaceTransaction_delete_shim(void *t) { (void)t; }
static void ASurfaceTransaction_apply_shim(void *t) { (void)t; }
static void ASurfaceTransaction_reparent_shim(void *t, void *sc, void *p) { (void)t; (void)sc; (void)p; }
static void ASurfaceTransaction_setOnComplete_shim(void *t, void *c, void *l) { (void)t; (void)c; (void)l; }
static void ASurfaceTransaction_setOnCommit_shim(void *t, void *c, void *l) { (void)t; (void)c; (void)l; }
static void ASurfaceTransaction_setBuffer_shim(void *t, void *sc, void *b, int f) { (void)t; (void)sc; (void)b; (void)f; }
static void ASurfaceTransaction_setVisibility_shim(void *t, void *sc, int8_t v) { (void)t; (void)sc; (void)v; }
static void ASurfaceTransaction_setZOrder_shim(void *t, void *sc, int32_t z) { (void)t; (void)sc; (void)z; }
static void ASurfaceTransaction_setDamageRegion_shim(void *t, void *sc, const void *r, size_t n) { (void)t; (void)sc; (void)r; (void)n; }
static void ASurfaceTransaction_setDesiredPresentTime_shim(void *t, int64_t pt) { (void)t; (void)pt; }
static void ASurfaceTransaction_setBufferTransparency_shim(void *t, void *sc, int8_t tr) { (void)t; (void)sc; (void)tr; }
static void ASurfaceTransaction_setBufferAlpha_shim(void *t, void *sc, float a) { (void)t; (void)sc; (void)a; }
static void ASurfaceTransaction_setCrop_shim(void *t, void *sc, const void *c) { (void)t; (void)sc; (void)c; }
static void ASurfaceTransaction_setPosition_shim(void *t, void *sc, float x, float y) { (void)t; (void)sc; (void)x; (void)y; }
static void ASurfaceTransaction_setScale_shim(void *t, void *sc, float sx, float sy) { (void)t; (void)sc; (void)sx; (void)sy; }
static void ASurfaceTransaction_setBufferTransform_shim(void *t, void *sc, int32_t tr) { (void)t; (void)sc; (void)tr; }
static void ASurfaceTransaction_setBufferDataSpace_shim(void *t, void *sc, int32_t ds) { (void)t; (void)sc; (void)ds; }
static void ASurfaceTransaction_setGeometry_shim(void *t, void *sc, const void *s, const void *d, int32_t tr) { (void)t; (void)sc; (void)s; (void)d; (void)tr; }
static void ASurfaceTransactionStats_getASurfaceControls_shim(void *st, void ***sc, size_t *cnt) { (void)st; if (sc) *sc = NULL; if (cnt) *cnt = 0; }
static int ASurfaceTransactionStats_getPreviousReleaseFenceFd_shim(void *st, void *sc) { (void)st; (void)sc; return -1; }
static void ASurfaceTransactionStats_releaseASurfaceControls_shim(void *st) { (void)st; }
static void ASurfaceTransaction_setFrameRateWithChangeStrategy_shim(void *t, void *sc, float f, int8_t c, int8_t s) { (void)t; (void)sc; (void)f; (void)c; (void)s; }
static void ASurfaceTransaction_setFrameRate_shim(void *t, void *sc, float f, int8_t c) { (void)t; (void)sc; (void)f; (void)c; }
static void *AHardwareBuffer_fromHardwareBuffer_shim(void *env, void *hb) { (void)env; (void)hb; return NULL; }
static void AHardwareBuffer_describe_shim(const void *b, void *d) { (void)b; (void)d; }
static int AHardwareBuffer_lock_shim(void *b, uint64_t u, int f, const void *r, void **out) { (void)b; (void)u; (void)f; (void)r; if (out) *out = NULL; return -1; }
static void AHardwareBuffer_release_shim(void *b) { (void)b; }
static int AHardwareBuffer_unlock_shim(void *b, int *f) { (void)b; (void)f; return 0; }
static void *AHardwareBuffer_toHardwareBuffer_shim(void *env, const void *b) { (void)env; (void)b; return NULL; }
static int AHardwareBuffer_allocate_shim(const void *d, void **out) { (void)d; if (out) *out = NULL; return -1; }

void *tl_shim_find(const char *name);

typedef struct { const char *name; void *addr; } tl_export_entry;

int __android_log_write(int prio, const char *tag, const char *text)
{
    (void)prio;
    tl_log_line("%s: %s", tag ? tag : "guest", text ? text : "");
    return 1;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    return __android_log_write(prio, tag, text);
}

int __android_log_buf_print(int prio, int buf, const char *tag, const char *fmt, ...)
{
    (void)buf;
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    return __android_log_write(prio, tag, text);
}

void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
    char text[1024];
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
    } else {
        snprintf(text, sizeof(text), "assertion failed: %s", cond ? cond : "unknown");
    }
    __android_log_write(6, tag, text);
    tl_loader_request_stop();
}

void tl_shim_bind_run(void *activity, void *window)
{
    (void)activity;
    /* The GL swap reads the finished frame back into this, so it has to be the window the
     * run is actually presenting from -- and it has to be cleared before that window is
     * freed, or the next swap writes into freed memory. The loader calls this with NULLs
     * when it tears a run down. */
    g_run_window = (tl_window *)window;
}

/* One line each. Order does not matter; the linear scan is honest and
 * small. */
static const tl_export_entry g_exports[] = {
    /* logging */
    { "__android_log_print",        __android_log_print },
    { "__android_log_write",        __android_log_write },
    { "__android_log_buf_print",    __android_log_buf_print },
    { "__android_log_assert",       __android_log_assert },
    { "__android_log_vprint",       NULL },   /* varargs adapter below */

    /* window */
    { "ANativeWindow_getWidth",     ANativeWindow_getWidth },
    { "ANativeWindow_getHeight",    ANativeWindow_getHeight },
    { "ANativeWindow_getFormat",    ANativeWindow_getFormat },
    { "ANativeWindow_lock",         ANativeWindow_lock },
    { "ANativeWindow_unlockAndPost", ANativeWindow_unlockAndPost },
    { "ANativeWindow_acquire",      ANativeWindow_acquire },
    { "ANativeWindow_release",      ANativeWindow_release },
    { "ANativeWindow_setBuffersGeometry", ANativeWindow_setBuffersGeometry },
    { "ANativeWindow_setFrameRate", ANativeWindow_setFrameRate },

    /* activity */
    { "ANativeActivity_finish",     ANativeActivity_finish },
    { "ANativeActivity_setWindowFlags", ANativeActivity_setWindowFlags },
    { "ANativeActivity_showSoftInput", ANativeActivity_showSoftInput },
    { "ANativeActivity_hideSoftInput", ANativeActivity_hideSoftInput },

    /* input */
    { "AInputQueue_preDispatchEvent", AInputQueue_preDispatchEvent },
    { "AInputQueue_finishEvent",    AInputQueue_finishEvent },
    { "AInputQueue_attach",         AInputQueue_attach },
    { "AInputQueue_detach",         AInputQueue_detach },
    { "AInputEvent_getType",        AInputEvent_getType },
    { "AMotionEvent_getAction",     AMotionEvent_getAction },
    { "AMotionEvent_getPointerCount", AMotionEvent_getPointerCount },
    { "AMotionEvent_getX",          AMotionEvent_getX },
    { "AMotionEvent_getY",          AMotionEvent_getY },
    { "AMotionEvent_getEventTime",  AMotionEvent_getEventTime },
    { "AMotionEvent_getButtonState", AMotionEvent_getButtonState },
    { "AMotionEvent_getPointerId",  AMotionEvent_getPointerId },
    { "AInputQueue_attachLooper",   AInputQueue_attachLooper },
    { "AInputQueue_detachLooper",   AInputQueue_detachLooper },
    { "AInputQueue_getEvent",       AInputQueue_getEvent },
    { "AInputEvent_getDeviceId",    AInputEvent_getDeviceId },
    { "AInputEvent_getSource",      AInputEvent_getSource },
    { "AKeyEvent_getAction",        AKeyEvent_getAction },
    { "AKeyEvent_getFlags",         AKeyEvent_getFlags },
    { "AKeyEvent_getKeyCode",       AKeyEvent_getKeyCode },
    { "AKeyEvent_getMetaState",     AKeyEvent_getMetaState },
    { "ALooper_prepare",            ALooper_prepare },
    { "ALooper_pollAll",            ALooper_pollAll },
    { "ALooper_addFd",              ALooper_addFd },
    { "ALooper_acquire",            ALooper_acquire },
    { "ALooper_release",            ALooper_release },
    { "ALooper_wake",               ALooper_wake },
    { "ANativeActivity_setWindowFormat", ANativeActivity_setWindowFormat },

    /* assets */
    { "AAssetManager_open",         AAssetManager_open },
    { "AAsset_close",               AAsset_close },
    { "AAsset_read",                AAsset_read },
    { "AAsset_getLength",           AAsset_getLength },
    { "AAsset_getBuffer",           AAsset_getBuffer },
    { "AAssetManager_fromJava",     AAssetManager_fromJava },
    { "AAssetDir_close",            AAssetDir_close },
    { "AAssetManager_openDir",      AAssetManager_openDir },
    { "AAsset_openFileDescriptor",  AAsset_openFileDescriptor },
    { "AConfiguration_new",         AConfiguration_new },
    { "AConfiguration_delete",      AConfiguration_delete },
    { "AConfiguration_fromAssetManager", AConfiguration_fromAssetManager },
    { "AConfiguration_getLanguage", AConfiguration_getLanguage },
    { "AConfiguration_getCountry",  AConfiguration_getCountry },

    /* EGL and GLES: named, honest failure at call time. There is no EGL on iOS
     * and the attempt draws through the software window, so these exist so a
     * library can relocate and report "no GPU" rather than be refused. */
    { "eglGetProcAddress",          eglGetProcAddress },
    { "eglGetError",                eglGetError },
    { "eglGetDisplay",              eglGetDisplay },
    { "eglInitialize",              eglInitialize },
    { "eglTerminate",               eglTerminate },
    { "eglChooseConfig",            eglChooseConfig },
    { "eglGetConfigAttrib",         eglGetConfigAttrib },
    { "eglCreateContext",           eglCreateContext },
    { "eglDestroyContext",          eglDestroyContext },
    { "eglCreateWindowSurface",     eglCreateWindowSurface },
    { "eglCreatePbufferSurface",    eglCreatePbufferSurface },
    { "eglDestroySurface",          eglDestroySurface },
    { "eglQuerySurface",            eglQuerySurface },
    { "eglSurfaceAttrib",           eglSurfaceAttrib },
    { "eglBindAPI",                 eglBindAPI },
    { "eglGetCurrentContext",       eglGetCurrentContext },
    { "eglMakeCurrent",             eglMakeCurrent },
    { "eglQueryString",             eglQueryString },
    { "eglSwapInterval",            eglSwapInterval },
    { "eglSwapBuffers",             eglSwapBuffers },
    { "glBindImageTexture",         glBindImageTexture },
    { "glBindVertexBuffer",         glBindVertexBuffer },
    { "glDispatchCompute",          glDispatchCompute },
    { "glDispatchComputeIndirect",  glDispatchComputeIndirect },
    { "glDrawArraysIndirect",       glDrawArraysIndirect },
    { "glDrawElementsIndirect",     glDrawElementsIndirect },
    { "glMemoryBarrier",            glMemoryBarrier },
    { "glVertexAttribBinding",      glVertexAttribBinding },
    { "glVertexAttribFormat",       glVertexAttribFormat },
    { "glVertexAttribIFormat",      glVertexAttribIFormat },
    { "glVertexBindingDivisor",     glVertexBindingDivisor },

    /* JNI entry points */
    { "JNI_GetDefaultJavaVMInitArgs", JNI_GetDefaultJavaVMInitArgs },
    { "JNI_CreateJavaVM",           JNI_CreateJavaVM },
    { "JNI_GetCreatedJavaVMs",      JNI_GetCreatedJavaVMs },

    /* libc: the 40-byte-type adapters and the passthroughs */
    { "pthread_mutex_init",         pthread_mutex_init_shim },
    { "pthread_mutex_lock",         pthread_mutex_lock_shim },
    { "pthread_mutex_trylock",      pthread_mutex_trylock_shim },
    { "pthread_mutex_unlock",       pthread_mutex_unlock_shim },
    { "pthread_mutex_destroy",      pthread_mutex_destroy_shim },
    { "pthread_mutex_timedlock",    pthread_mutex_timedlock_shim },
    /* The attr type is bionic's 4 bytes, not Darwin's 64 -- see the comment above the
     * implementations. Leaving these to Darwin overflows a guest's stack frame. */
    { "pthread_mutexattr_init",     pthread_mutexattr_init_shim },
    { "pthread_mutexattr_destroy",  pthread_mutexattr_destroy_shim },
    { "pthread_mutexattr_settype",  pthread_mutexattr_settype_shim },
    { "pthread_mutexattr_gettype",  pthread_mutexattr_gettype_shim },
    { "pthread_once",               pthread_once_shim },
    /* The types Darwin defines larger than bionic: see the block above for the sizes. */
    { "pthread_key_create",         pthread_key_create_shim },
    { "pthread_key_delete",         pthread_key_delete_shim },
    { "pthread_getspecific",        pthread_getspecific_shim },
    { "pthread_setspecific",        pthread_setspecific_shim },
    { "pthread_attr_init",          pthread_attr_init_shim },
    { "pthread_attr_destroy",       pthread_attr_destroy_shim },
    { "pthread_attr_setdetachstate", pthread_attr_setdetachstate_shim },
    { "pthread_attr_setstacksize",  pthread_attr_setstacksize_shim },
    { "pthread_attr_setschedpolicy", pthread_attr_setschedpolicy_shim },
    { "pthread_attr_getstack",      pthread_attr_getstack_shim },
    { "pthread_create",             pthread_create_shim },
    { "pthread_rwlock_init",        pthread_rwlock_init_shim },
    { "pthread_rwlock_destroy",     pthread_rwlock_destroy_shim },
    { "pthread_rwlock_rdlock",      pthread_rwlock_rdlock_shim },
    { "pthread_rwlock_wrlock",      pthread_rwlock_wrlock_shim },
    { "pthread_rwlock_unlock",      pthread_rwlock_unlock_shim },
    { "pthread_condattr_init",      pthread_condattr_init_shim },
    { "pthread_condattr_destroy",   pthread_condattr_destroy_shim },
    { "pthread_condattr_setclock",  pthread_condattr_setclock_shim },
    { "pthread_cond_init",          pthread_cond_init_shim },
    { "pthread_cond_destroy",       pthread_cond_destroy_shim },
    { "pthread_cond_wait",          pthread_cond_wait_shim },
    { "pthread_cond_timedwait",     pthread_cond_timedwait_shim },
    { "pthread_cond_signal",        pthread_cond_signal_shim },
    { "pthread_cond_broadcast",     pthread_cond_broadcast_shim },
    { "sysconf",                    sysconf_shim },
    { "getpagesize",                getpagesize_shim },
    /* The glue's command channel is one-byte writes to the message pipe; see the block
     * above. These log only len == 1 and forward everything else untouched. */
    { "write",                      write_shim },
    { "read",                       read_shim },
    { "pipe",                       pipe_shim },
    { "sysinfo",                    sysinfo_shim },
    /* /proc has no iOS equivalent, and the files below are read by name rather than
     * discovered, so the only way to answer them is at fopen. */
    { "fopen",                      fopen_shim },
    { "fopen64",                    fopen_shim },
    /* Logging passthroughs, not adapters: the guest's arena allocator is the
     * current suspect and its mmap is how the failure becomes visible. */
    { "mmap",                       mmap_shim },
    { "mmap64",                     mmap_shim },
    { "munmap",                     munmap_shim },
    { "dlopen",                     dlopen_shim },
    { "dlsym",                      dlsym_shim },

    { "getauxval",                  getauxval_shim },
    { "__errno",                    __errno_shim },
    { "__system_property_get",      __system_property_get_shim },
    { "__strlen_chk",               __strlen_chk_shim },
    { "__memmove_chk",              __memmove_chk_shim },
    { "__vsnprintf_chk",            __vsnprintf_chk_shim },
    { "android_set_abort_message",  android_set_abort_message_shim },
    { "__cxa_finalize",             __cxa_finalize_shim },
    { "__cxa_atexit",               __cxa_atexit_shim },
    { "dl_iterate_phdr",            dl_iterate_phdr_shim },
    { "__sF",                       g_sF_storage },
    { "fprintf",                    fprintf_shim },
    { "vfprintf",                   vfprintf_shim },
    { "fputc",                      fputc_shim },
    { "fflush",                     fflush_shim },
    { "fwrite",                     fwrite_shim },

    /* bionic's stdio globals and ctype table: data the guest binds by name and
     * Darwin has no symbol for. */
    { "stdin",                      TL_STREAM_SLOT(0) },
    { "stdout",                     TL_STREAM_SLOT(1) },
    { "stderr",                     TL_STREAM_SLOT(2) },
    { "_ctype_",                    (void *)g_ctype },

    /* Fortified libc. */
    { "__memcpy_chk",               __memcpy_chk_shim },
    { "__memset_chk",               __memset_chk_shim },
    { "__strcat_chk",               __strcat_chk_shim },
    { "__strcpy_chk",               __strcpy_chk_shim },
    { "__strncat_chk",              __strncat_chk_shim },
    { "__strncpy_chk",              __strncpy_chk_shim },
    { "__strchr_chk",               __strchr_chk_shim },
    { "__strrchr_chk",              __strrchr_chk_shim },
    { "__vsprintf_chk",             __vsprintf_chk_shim },
    { "__read_chk",                 __read_chk_shim },
    { "__write_chk",                __write_chk_shim },
    { "__pread_chk",                __pread_chk_shim },
    { "__pread64_chk",              __pread64_chk_shim },
    { "__pwrite_chk",               __pwrite_chk_shim },
    { "__pwrite64_chk",             __pwrite64_chk_shim },
    { "__FD_SET_chk",               __FD_SET_chk_shim },
    { "__FD_ISSET_chk",             __FD_ISSET_chk_shim },
    { "__open_2",                   __open_2_shim },
    { "__gnu_strerror_r",           __gnu_strerror_r_shim },
    { "__libc_current_sigrtmin",    __libc_current_sigrtmin_shim },
    { "__register_atfork",          __register_atfork_shim },
    { "__ctype_get_mb_cur_max",     __ctype_get_mb_cur_max_shim },
    { "__strncpy_chk2",             __strncpy_chk2_shim },
    { "gettid",                     gettid_shim },
    { "lseek64",                    lseek64_shim },
    { "pread64",                    pread64_shim },
    { "ftruncate64",                ftruncate64_shim },
    { "memalign",                   memalign_shim },
    { "sincos",                     sincos_shim },
    { "sincosf",                    sincosf_shim },
    { "sched_getaffinity",          sched_getaffinity_shim },
    { "sched_setaffinity",          sched_setaffinity_shim },
    { "__sched_cpucount",           __sched_cpucount_shim },

    /* OpenSL ES: named, never functional. See the comment above slCreateEngine. */
    { "slCreateEngine",             slCreateEngine_shim },
    { "SL_IID_ENGINE",              TL_SL_IID(0) },
    { "SL_IID_PLAY",                TL_SL_IID(1) },
    { "SL_IID_RECORD",              TL_SL_IID(2) },
    { "SL_IID_BUFFERQUEUE",         TL_SL_IID(3) },
    { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", TL_SL_IID(4) },
    { "SL_IID_VOLUME",              TL_SL_IID(5) },

    { "ANativeWindow_fromSurface",  ANativeWindow_fromSurface_shim },
    { "ASurfaceControl_create",     ASurfaceControl_create_shim },
    { "ASurfaceControl_createFromWindow", ASurfaceControl_createFromWindow_shim },
    { "ASurfaceControl_release",    ASurfaceControl_release_shim },
    { "ASurfaceTransaction_create", ASurfaceTransaction_create_shim },
    { "ASurfaceTransaction_delete", ASurfaceTransaction_delete_shim },
    { "ASurfaceTransaction_apply",  ASurfaceTransaction_apply_shim },
    { "ASurfaceTransaction_reparent", ASurfaceTransaction_reparent_shim },
    { "ASurfaceTransaction_setOnComplete", ASurfaceTransaction_setOnComplete_shim },
    { "ASurfaceTransaction_setOnCommit", ASurfaceTransaction_setOnCommit_shim },
    { "ASurfaceTransaction_setBuffer", ASurfaceTransaction_setBuffer_shim },
    { "ASurfaceTransaction_setVisibility", ASurfaceTransaction_setVisibility_shim },
    { "ASurfaceTransaction_setZOrder", ASurfaceTransaction_setZOrder_shim },
    { "ASurfaceTransaction_setDamageRegion", ASurfaceTransaction_setDamageRegion_shim },
    { "ASurfaceTransaction_setDesiredPresentTime", ASurfaceTransaction_setDesiredPresentTime_shim },
    { "ASurfaceTransaction_setBufferTransparency", ASurfaceTransaction_setBufferTransparency_shim },
    { "ASurfaceTransaction_setBufferAlpha", ASurfaceTransaction_setBufferAlpha_shim },
    { "ASurfaceTransaction_setCrop", ASurfaceTransaction_setCrop_shim },
    { "ASurfaceTransaction_setPosition", ASurfaceTransaction_setPosition_shim },
    { "ASurfaceTransaction_setScale", ASurfaceTransaction_setScale_shim },
    { "ASurfaceTransaction_setBufferTransform", ASurfaceTransaction_setBufferTransform_shim },
    { "ASurfaceTransaction_setBufferDataSpace", ASurfaceTransaction_setBufferDataSpace_shim },
    { "ASurfaceTransaction_setGeometry", ASurfaceTransaction_setGeometry_shim },
    { "ASurfaceTransactionStats_getASurfaceControls", ASurfaceTransactionStats_getASurfaceControls_shim },
    { "ASurfaceTransactionStats_getPreviousReleaseFenceFd", ASurfaceTransactionStats_getPreviousReleaseFenceFd_shim },
    { "ASurfaceTransactionStats_releaseASurfaceControls", ASurfaceTransactionStats_releaseASurfaceControls_shim },
    { "ASurfaceTransaction_setFrameRateWithChangeStrategy", ASurfaceTransaction_setFrameRateWithChangeStrategy_shim },
    { "ASurfaceTransaction_setFrameRate", ASurfaceTransaction_setFrameRate_shim },
    { "AHardwareBuffer_fromHardwareBuffer", AHardwareBuffer_fromHardwareBuffer_shim },
    { "AHardwareBuffer_describe",   AHardwareBuffer_describe_shim },
    { "AHardwareBuffer_lock",       AHardwareBuffer_lock_shim },
    { "AHardwareBuffer_release",    AHardwareBuffer_release_shim },
    { "AHardwareBuffer_unlock",     AHardwareBuffer_unlock_shim },
    { "AHardwareBuffer_toHardwareBuffer", AHardwareBuffer_toHardwareBuffer_shim },
    { "AHardwareBuffer_allocate",   AHardwareBuffer_allocate_shim },
};

static int __android_log_vprint_shim(int prio, const char *tag, const char *fmt, va_list ap)
{
    char text[1024];
    vsnprintf(text, sizeof(text), fmt, ap);
    __android_log_write(prio, tag, text);
    return 1;
}

void *tl_shim_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    /* GLES and EGL first, and from one place. A guest whose ES 3.0 calls came from iOS's
     * OpenGL ES and whose 3.1 calls came from nowhere is not a guest with GL; ANGLE is the
     * only implementation here that covers the whole surface. The two calls this layer owns
     * are named before the lookup so ANGLE cannot shadow them. */
    if (is_gl_or_egl(name)) {
        if (!strcmp(name, "eglCreateWindowSurface")) {
            return (void *)eglCreateWindowSurface;
        }
        if (!strcmp(name, "eglSwapBuffers")) {
            return (void *)eglSwapBuffers;
        }
        /* eglGetProcAddress is how most guests fetch their entry points, so it has to be
         * ours: ANGLE's would hand back ANGLE's own eglCreateWindowSurface and the pbuffer
         * substitution above would never happen. Ours routes through this same lookup, so
         * the two calls stay ours whichever way the guest asks for them. */
        if (!strcmp(name, "eglGetProcAddress")) {
            return (void *)eglGetProcAddress;
        }
        void *from_angle = angle_symbol(name);
        if (from_angle) {
            return from_angle;
        }
    }
    for (size_t i = 0; i < sizeof(g_exports) / sizeof(g_exports[0]); i++) {
        if (!strcmp(g_exports[i].name, name) && g_exports[i].addr) {
            return g_exports[i].addr;
        }
    }
    if (!strcmp(name, "__android_log_vprint")) {
        return (void *)__android_log_vprint_shim;
    }
    void *host = dlsym(RTLD_DEFAULT, name);
    if (host) {
        return host;
    }
    return NULL;
}

bool tl_shim_supplies(const char *soname)
{
    /* The attempt provides liblog and libc behaviour in-process. libEGL
     * resolves far enough to relocate; calling further into EGL fails at
     * eglGetProcAddress with what the table holds. */
    return !strcmp(soname, "liblog.so") || !strcmp(soname, "libc.so")
        || !strcmp(soname, "libm.so") || !strcmp(soname, "libdl.so")
        || !strcmp(soname, "libandroid.so") || !strcmp(soname, "libEGL.so")
        || !strcmp(soname, "libGLESv2.so") || !strcmp(soname, "libGLESv1_CM.so")
        || !strcmp(soname, "libGLESv3.so")
        /* Named, not implemented: slCreateEngine reports the feature unsupported
         * and the SL_IID_* handles are distinct addresses. That is enough for a
         * library to relocate and run without sound. */
        || !strcmp(soname, "libOpenSLES.so");
}

/* The pieces the loader wires into the ANativeActivity it passes. */
void *tl_shim_new_input_queue(void)
{
    struct tl_input_queue *q = calloc(1, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    return q;
}

void *tl_shim_new_asset_manager(void)
{
    return calloc(1, sizeof(struct tl_asset_manager));
}

void tl_shim_free(void *p) { free(p); }
