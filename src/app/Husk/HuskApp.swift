// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Which way the app may turn. The app follows the device, except while a landscape game is on screen:
/// Geometry Dash is a landscape game, and a fixed-size surface cannot follow a rotation.
enum HuskOrientation {
    static var standard: UIInterfaceOrientationMask {
        UIDevice.current.userInterfaceIdiom == .pad ? .all : [.portrait, .landscapeLeft, .landscapeRight]
    }
    static var mask: UIInterfaceOrientationMask = standard

    /// Allow only `new`, and turn the screen to it if it is not already there.
    ///
    /// A game's screen asks as it appears, while its full-screen cover is still being presented, and iOS can refuse then
    /// ("Supported: portrait") because it has not yet asked the cover what it allows. So a refusal is retried a few times,
    /// a moment apart, for as long as `new` is still what is wanted. Each retry re-asserts the mask on every view
    /// controller in the chain first: a freshly presented cover answers orientation from its own controller until it has
    /// been asked once, and the refusal is what asks it.
    @MainActor static func set(_ new: UIInterfaceOrientationMask, attempt: Int = 0) {
        mask = new
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            var vc = scene.keyWindow?.rootViewController
            while let v = vc { v.setNeedsUpdateOfSupportedInterfaceOrientations(); vc = v.presentedViewController }
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: new)) { error in
                HuskLog.log("ui", "orientation change refused (attempt \(attempt + 1)): \(error.localizedDescription)")
                guard attempt < 12 else { return }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
                    if mask == new { set(new, attempt: attempt + 1) }
                }
            }
        }
    }
}

/// Hosts SwiftUI content in a view controller that reports the mask it is given, so iOS's
/// orientation question ("Supported: portrait") is answered with the game's orientations rather
/// than whatever the surrounding controller defaults to. Wrap a game's full-screen content in it,
/// passing `HuskOrientation.mask`.
struct OrientationHost<Content: View>: UIViewControllerRepresentable {
    let mask: UIInterfaceOrientationMask
    @ViewBuilder var content: () -> Content

    func makeUIViewController(context: Context) -> UIViewController {
        let host = MaskedHostingController(rootView: content())
        host.mask = mask
        return host
    }

    func updateUIViewController(_ vc: UIViewController, context: Context) {
        guard let host = vc as? MaskedHostingController<Content> else { return }
        host.mask = mask
    }
}

/// A hosting controller whose `supportedInterfaceOrientations` is exactly the mask handed to it.
final class MaskedHostingController<Content: View>: UIHostingController<Content> {
    var mask: UIInterfaceOrientationMask = .all {
        didSet {
            if mask != oldValue { setNeedsUpdateOfSupportedInterfaceOrientations() }
        }
    }
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { mask }
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
                .onOpenURL { IncomingFiles.shared.receive($0) }
        }
    }
}
