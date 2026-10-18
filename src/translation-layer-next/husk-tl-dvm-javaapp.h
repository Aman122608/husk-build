/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A Java app (no engine library of its own to drive, or one driven from Java, such as libGDX) run on Husk's Dalvik runtime with
 * Husk's Java framework: the launcher activity is created and its lifecycle run by husk.AppRunner, a GLSurfaceView's renderer draws
 * on Husk's GL thread into the app's CAMetalLayer through ANGLE, and touches are queued here for the app's main thread.
 * The runtime must already be started (tl_dvm_start + tl_dvm_load_natives) with husk-framework.dex on the boot class path.
 */
#ifndef HUSK_TL_DVM_JAVAAPP_H
#define HUSK_TL_DVM_JAVAAPP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct tl_javaapp_config {
    const char *apk_path;
    const char *data_dir;            /* the app's private folder */
    const char *package_name;        /* NULL: the manifest's */
    int width, height;               /* the screen, in pixels */
    float density;                   /* pixels per dp */
    void *metal_layer;
    const char *angle_egl, *angle_gles;
    const char *frame_dir;           /* host tests: frames written here */
    int frame_every;
    void (*vibrate)(int ms);
    void (*open_url)(const char *url);
    const char *framework_res;       /* the platform's resources (framework-res.apk): its resources.arsc, layouts, drawables */
    /* the host's side of the views: its keyboard for a focused text field (input type and IME options as EditorInfo has them),
       the clipboard (get returns a malloc'd string or NULL), the share sheet, the screen orientation the activity asks for */
    void (*show_keyboard)(int show, int input_type, int ime_options);
    void (*set_clipboard)(const char *utf8);
    char *(*get_clipboard)(void);
    void (*share)(const char *utf8);
    void (*set_orientation)(int android_orientation);
    /* the system's file picker: MIME types comma-separated, as the intent gives them (any type, images, "application/pdf"); the host shows its document
       picker, copies what was picked into the app's folder and answers with tl_javaapp_documents_picked */
    void (*pick_documents)(const char *mime_types, int multiple);
    int insets[4];                   /* left, top, right, bottom: the screen's notch and home indicator, in pixels */
    bool night_mode;                 /* the phone is in dark mode: the app's configuration says UI_MODE_NIGHT_YES */
} tl_javaapp_config;

bool tl_javaapp_manifest(const char *apk, char *pkg, size_t pn, char *activity, size_t an, char *application, size_t apn);
bool tl_javaapp_start(const tl_javaapp_config *cfg);
void tl_javaapp_touch(int phase, int id, float x, float y);    /* surface pixels; phase 0 down, 1 move, 2 up, 3 cancel */
void tl_javaapp_key(int android_keycode, bool down);
void tl_javaapp_back(void);
void tl_javaapp_set_paused(bool paused);
/* The host keyboard's typing, to the focused text field: text, one delete before the cursor, the editor action (return), the
   keyboard closed by the host. */
void tl_javaapp_text(const char *utf8);
void tl_javaapp_text_delete(void);
void tl_javaapp_text_action(void);
void tl_javaapp_keyboard_closed(void);
/* The file picker closed: the copies of the n picked files (n = 0: the user cancelled). */
void tl_javaapp_documents_picked(const char *const *paths, int n);
/* The screen's insets changed (rotation), or the keyboard now covers ime_bottom pixels of it. */
void tl_javaapp_set_insets(int left, int top, int right, int bottom, int ime_bottom);
unsigned long tl_javaapp_frames(void);
bool tl_javaapp_ended(void);
/* The render/GL thread's Darwin identity (as the guest-thread census carries it: (unsigned long)pthread_self()), so a
 * stall can name the frame/render thread explicitly; 0 before it starts. */
unsigned long tl_javaapp_render_thread(void);

/* A guest call that means "quit my app" reached the layer (Runtime.exit, System.exit, Process.killProcess, exit()/_exit()).
 * Mark the app ended so its own loops and the state machine unwind -- Husk's process is never ended for it. Idempotent. */
void tl_javaapp_request_exit(void);
/* The calling thread is unwinding after such a call: if it is one of the app's own driver threads (the app main thread or
 * the GL thread), mark its done flag, so a session close's bounded wait does not sit out its budget on a thread that is
 * already leaving. Never joins or cancels a thread. */
void tl_javaapp_thread_leaving(void);

/* Close the app's session, so a different app can run in this process: ask the app's main Looper to quit (so Looper.loop()
 * returns, AppRunner.run returns and the app's main thread returns) and set the end flag the GL thread's loop checks -- then
 * wait (bounded: Darwin has no pthread_timedjoin_np) for both threads to return. Never ends, cancels or joins a thread that
 * ran app code (its exit runs the guest's TLS destructors); a thread that will not stop is left parked and named in the log.
 * Then drains the input and text queues and the app's EGL frame state. The mutexes stay: no struct is memset.
 * Returns 0 = done, 1 = a thread is still live (quarantined), 2 = irreversible (no cooperative stop was available and a
 * thread is still live). Safe to call when nothing was started (returns 0). */
int tl_javaapp_reset(void);

#endif
