// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Which way the app may turn. The app follows the device, except while a landscape game is on screen:
/// Geometry Dash is a landscape game, and a fixed-size surface cannot follow a rotation.
enum HuskOrientation {
    static var standard: UIInterfaceOrientationMask {
        UIDevice.current.userInterfaceIdiom == .pad ? .all : [.portrait, .landscapeLeft, .landscapeRight]
    }
    static var mask: UIInterfaceOrientationMask = standard

    /// The names for a mask, so a refusal and an application read the same way in the log.
    static func describe(_ m: UIInterfaceOrientationMask) -> String {
        var parts: [String] = []
        if m.contains(.portrait) { parts.append("portrait") }
        if m.contains(.portraitUpsideDown) { parts.append("portraitUpsideDown") }
        if m.contains(.landscapeLeft) { parts.append("landscapeLeft") }
        if m.contains(.landscapeRight) { parts.append("landscapeRight") }
        return parts.isEmpty ? "none" : parts.joined(separator: ", ")
    }

    /// What the screen is showing now, for the log's "still ..." half.
    @MainActor static func describeCurrent() -> String {
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            switch scene.interfaceOrientation {
            case .portrait: return "portrait"
            case .portraitUpsideDown: return "portraitUpsideDown"
            case .landscapeLeft: return "landscapeLeft"
            case .landscapeRight: return "landscapeRight"
            @unknown default: return "unknown"
            }
        }
        return "no window scene"
    }

    /// Allow only `new`, and turn the screen to it if it is not already there.
    ///
    /// A game's screen asks as it appears, while its full-screen cover is still being presented, and iOS can refuse then
    /// ("Supported: portrait") because it has not yet asked the cover what it allows. So a refusal is retried a few times,
    /// a moment apart, for as long as `new` is still what is wanted.
    ///
    /// `app` names the game that asked, so a refusal says WHICH game's screen was refused and what it asked for, instead
    /// of leaving a bare UIKit string in the log; and the refusal is retried from EVERY window of the scene, not just the
    /// key one, because the cover's own hosting controller is the view controller iOS consults and the key window's chain
    /// can miss it while the cover is still being presented.
    @MainActor static func set(_ new: UIInterfaceOrientationMask, app: String? = nil, attempt: Int = 0) {
        mask = new
        let who = app ?? "Husk"
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            for w in scene.windows {
                var vc = w.rootViewController
                while let v = vc { v.setNeedsUpdateOfSupportedInterfaceOrientations(); vc = v.presentedViewController }
            }
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: new)) { error in
                HuskLog.log("ui", "orientation change refused (attempt \(attempt + 1)) for '\(who)' (asked for \(Self.describe(new))): \(error.localizedDescription)")
                guard attempt < 6 else {
                    HuskLog.log("ui", "orientation was NOT applied for '\(who)' after \(attempt + 1) attempts: it asked for \(Self.describe(new)) and the screen is still \(Self.describeCurrent())")
                    return
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                    if mask == new { set(new, app: app, attempt: attempt + 1) }
                }
            }
        }
    }
}

final class HuskAppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        HuskOrientation.mask
    }

    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        // The download session reconnects to whatever was running before Husk was closed or relaunched in the background.
        _ = Downloads.shared
        return true
    }

    /// iOS woke Husk because background downloads finished or need attention: handle them, then say so.
    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String, completionHandler: @escaping () -> Void) {
        guard identifier == Downloads.sessionID else { completionHandler(); return }
        HuskLog.log("downloads", "woken for background download events")
        Downloads.shared.backgroundCompletion = completionHandler
    }
}

@main
struct HuskApp: App {
    @UIApplicationDelegateAdaptor(HuskAppDelegate.self) private var appDelegate

    init() {
        // Order matters. HuskLog redirects stderr, so anything that logs before
        // this point is lost -- and the JIT path is exactly what we cannot afford
        // to lose the first line of.
        HuskLog.start()
        HuskLog.logFootprint("app-launch")
        // Before anything asks a debugger for anything: was this process already marked as debugged (a jailbreak that allows JIT in apps)?
        JITBootstrap.noteLaunchState()
        HuskLog.log("jit", "debugged at launch: \(JITBootstrap.debuggedAtLaunch); TrollStore install: \(JITBootstrap.isInstalledWithTrollStore); jailbreak: \(JITBootstrap.isJailbroken); can grant its own JIT: \(JITBootstrap.canGrantOwnJIT)")

        // Then the trap guard: without it, any brk we issue when StikDebug is
        // absent kills the process outright rather than returning an error.
        JITBootstrap.installTrapGuard()

        // Android no longer starts by itself unless someone turns that on: once, for everyone who had it from the old default.
        let d = UserDefaults.standard
        if !d.bool(forKey: "husk.autoStart.offByDefault") {
            d.set(false, forKey: "husk.autoStart")
            d.set(true, forKey: "husk.autoStart.offByDefault")
        }
        // Copies of shared APKs that were never placed.
        IncomingFiles.clearLeftovers()

        // Game controllers, for the games the native runtime runs.
        Task { @MainActor in HuskGamepads.shared.start() }
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                // An APK shared to Husk, or opened in it from Files.
                .onOpenURL { url in
                    // husk://open_app?unique_identifier=… from a Home Screen clip; anything else may be a shared APK
                    if !AppClip.handle(url) { IncomingFiles.shared.receive(url) }
                }
        }
    }
}
