package husk;

/** What Husk's runtime does for the framework: logging, the app's files and assets, the screen, input, the host's keyboard and clipboard. */
public final class Native {
    private Native() {}
    public static native void log(int priority, String tag, String msg);
    public static native byte[] readAsset(String path);
    public static native String[] listAssets(String dir);
    public static native long assetFd(String path);          // fd << 40 | offset; -1 when compressed or missing
    public static native long assetLength(String path);
    /** A file in one of the APKs: 0 the app's, 1 the platform's resources (framework-res), 2.. the app's split APKs. */
    public static native byte[] readApkFile(int apk, String path);
    public static native long apkFileFd(int apk, String path);
    public static native long apkFileLength(int apk, String path);
    public static native String apkPath();
    /** The app's split APKs (a Google Play install: libraries, resources, asset packs), in the order they were given. */
    public static native String[] splitPaths();
    /** The NDK looper of this thread: made with the Java one, and its callbacks run between messages. */
    public static native void nativeLooperPrepare();
    public static native boolean nativeLooperPoll();
    /** Class.getClassLoader for the app's own classes. */
    public static native void setAppClassLoader(ClassLoader loader);
    public static native String dataDir();

    /* -----------------------------------------------------------------------------------------------------------
     * DELIVERY DIAGNOSTICS (husk3 port, "the missing signal").
     *
     * A run that stalls must answer one question about the LAYER, not about the interpreter: what did the layer
     * DELIVER to the app (lifecycle, window add/remove, window focus, the surface callbacks, and every message put on a
     * looper), and what did the app's main thread DO with it. This is where those events are written down, so all of
     * them land in one log stream (android.util.Log -> the layer's own sink) next to the layer's lines.
     *
     * The one fact a bare "posted"/"dispatched" pair cannot give is whether the post WOKE a thread that was inside
     * Object.wait() or whether no thread was waiting and the queue's own 8 ms poll would have to pick the message up --
     * two different failures with the same symptom (a looper sitting idle). android.os.MessageQueue reads its waiting
     * flag and passes it here, so the next log says which of the two happened, per message.
     *
     * BOUNDED BY CONSTRUCTION: a full note for the first DELIVERY_FULL events and for DELIVERY_WINDOW_MS after the app's
     * main thread starts (the window a freeze lives in), then every DELIVERY_EVERY-th. The cost on the delivery path is
     * one counter increment; the clock is read at most once per 32 events once the window is over, and NOTHING here runs
     * per frame (these are called by the lifecycle, the window and the looper, never by the render thread).
     * ----------------------------------------------------------------------------------------------------------- */
    public static final int DELIVERY_FULL = 64, DELIVERY_EVERY = 512;
    public static final long DELIVERY_WINDOW_MS = 10000;
    private static final java.util.concurrent.atomic.AtomicInteger sDelivered = new java.util.concurrent.atomic.AtomicInteger();
    private static final java.util.concurrent.atomic.AtomicInteger sPosts = new java.util.concurrent.atomic.AtomicInteger();
    private static final java.util.concurrent.atomic.AtomicInteger sDispatched = new java.util.concurrent.atomic.AtomicInteger();
    private static final java.util.concurrent.atomic.AtomicInteger sFoundWaiter = new java.util.concurrent.atomic.AtomicInteger();
    private static volatile long sSessionStart;

    /** The app's main thread is starting: the delivery window opens here, so each app's notes are its own. */
    public static void deliverySessionStart() { sSessionStart = android.os.SystemClock.uptimeMillis(); }

    /** One line naming something the layer just delivered (lifecycle, window, surface): once per delivery, bounded. */
    public static void delivered(String what) {
        int n = sDelivered.incrementAndGet();
        if (deliveryWanted(n)) android.util.Log.d("HuskDelivery", what);
    }

    /** A message was put on a looper. foundWaiter: a thread was inside Object.wait() when it was enqueued (this post woke
     *  it); !foundWaiter: nothing was waiting, so the queue's 8 ms poll is what has to pick the message up. */
    public static void post(String handler, String what, long whenMs, String thread, boolean foundWaiter) {
        int n = sPosts.incrementAndGet();
        if (foundWaiter) sFoundWaiter.incrementAndGet();
        if (!deliveryWanted(n)) return;
        long delay = whenMs - android.os.SystemClock.uptimeMillis();
        android.util.Log.d("HuskDelivery", "post #" + n + " " + handler + " " + what + " due " + (delay <= 0 ? "now" : "in " + delay + "ms")
                + " thread=" + thread + (foundWaiter ? " -> WOKE a thread inside wait()" : " -> no thread was in wait() (the queue's 8 ms poll picks it up)"));
    }

    /** The app's main thread took that message off the queue and is dispatching it: the other end of the pair. A post
     *  with no matching dispatch is a callback never delivered; a post whose line says no thread was waiting is not. */
    public static void dispatch(String handler, String what, long dueMs, String thread) {
        int n = sDispatched.incrementAndGet();
        if (!deliveryWanted(n)) return;
        android.util.Log.d("HuskDelivery", "dispatch #" + n + " " + handler + " " + what + " after " + Math.max(0, android.os.SystemClock.uptimeMillis() - dueMs) + "ms in the queue thread=" + thread);
    }

    /** Totals, printed with every DELIVERY_EVERY-th note: the next log carries these without a line per event. */
    public static String deliverySummary() {
        return "delivery totals: events=" + sDelivered.get() + " posts=" + sPosts.get() + " dispatched=" + sDispatched.get()
             + " posts-that-found-a-waiter=" + sFoundWaiter.get();
    }

    private static boolean deliveryWanted(int n) {
        if (n <= DELIVERY_FULL) return true;
        if ((n % 32) == 0) {
            long t = android.os.SystemClock.uptimeMillis(), s = sSessionStart;
            if (s == 0) { sSessionStart = s = t; }
            if (t - s <= DELIVERY_WINDOW_MS) return true;
        }
        if ((n % DELIVERY_EVERY) == 0) { android.util.Log.d("HuskDelivery", deliverySummary()); return true; }
        return false;
    }

    public static native String externalDir();
    public static native String packageName();
    public static native int screenWidth();
    public static native int screenHeight();
    public static native float density();
    public static native void startGL(Object view, Object renderer, int esVersion);
    public static native void requestRender();
    public static native void setContentView(Object view);
    public static native int[] pollInput();                  // packed touch events, or null
    public static native String[] pollText();                // typed text, deletes and actions from the keyboard
    public static native void showKeyboard(boolean show, int inputType, int imeOptions);
    public static native void setClipboard(String text);
    public static native String getClipboard();
    public static native void share(String text);
    public static native void pickDocuments(String mimeTypes, boolean multiple);   // the answer arrives through pollText ("f<path>", "F")
    public static native void setOrientation(int androidOrientation);
    public static native int[] insets();
    public static native boolean nightMode();                // the phone is in dark mode                     // left, top, right, bottom of the screen the app must keep clear
    public static native void vibrate(long ms);
    public static native void openUrl(String url);
    public static native void exit();
    public static native long uptimeNanos();
}
