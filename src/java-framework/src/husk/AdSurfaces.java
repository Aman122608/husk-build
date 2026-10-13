package husk;

import java.util.HashSet;

/**
 * Ad surfaces Husk cannot honor, refused instead of letting them cover the game.
 *
 * An ad SDK that shows a full-screen ad does it by starting an Activity whose whole content is a surface the SDK renders itself
 * (AdMob's {@code com.google.android.gms.ads.AdActivity} and its {@code ...internal.overlay.*} activities, Ad Manager's
 * {@code ...ads.admanager.AdManagerAdActivity}). On a phone that activity draws, and the user leaves it with its own close button
 * or Back. In Husk there is no ad service feeding it and no inventory, and the surface it builds is a view with no bounds over the
 * game: the game's frames stop and the user cannot leave. That is exactly what the 2026-10-13 Doodle Jump run showed -- the AdMob
 * interstitial opened, the frame counter froze at 406 and stayed there, and Back did nothing.
 *
 * So an ad that can only half-run is failed CLOSED: the activity is never created, a caller that asked for a result hears the user
 * backed out (as it would from a system screen Husk has none of), and one line says why. Nothing is drawn in its place, and no ad
 * is fabricated -- the game underneath keeps running, which is the point. Banners and rewarded views a host activity lays out
 * itself are not full-screen surfaces and are not touched here.
 */
final class AdSurfaces {
    private AdSurfaces() {}

    private static final HashSet<String> sRefused = new HashSet<>();

    /** Whether an activity is a full-screen advertising surface of Google's ads SDK (the only ones Husk has seen take the screen). */
    static boolean isFullScreenAd(String className) {
        if (className == null) return false;
        if (!className.startsWith("com.google.android.gms.ads.")) return false;
        return className.endsWith("AdActivity") || className.contains(".overlay.");
    }

    /** Refuse to open {@code className} if it is a full-screen ad surface; true when it was refused (and the line was written once). */
    static boolean refuse(String className) {
        if (!isFullScreenAd(className)) return false;
        synchronized (sRefused) { if (!sRefused.add(className)) return true; }
        android.util.Log.d("Husk", "husk: ads refusing " + className
            + ": a full-screen ad surface Husk cannot draw or dismiss (no ad service, no inventory); the game underneath keeps running");
        return true;
    }
}
