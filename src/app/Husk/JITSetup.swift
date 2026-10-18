// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import SwiftUI
import UIKit

/// How Husk gets a debugger attached. See docs/06-built-in-jit.md.
enum JITMethod: String, CaseIterable, Identifiable {
    /// StikDebug when it is installed, then TrollStore, otherwise Built-in StikJIT.
    case automatic
    case stikDebug
    case trollStore
    case jailbreak
    case builtIn

    var id: String { rawValue }
    var title: String {
        switch self {
        case .automatic: return "Automatic"
        case .stikDebug: return "StikDebug"
        case .trollStore: return "TrollStore"
        case .jailbreak: return "Jailbreak (Dopamine)"
        case .builtIn: return "Built-in StikJIT"
        }
    }
}

/// Where Built-in StikJIT's pairing file came from.
enum JITPairingSource: String {
    /// Made by Husk on this device (iOS 27, OnDevicePairing).
    case onDevice
    /// Imported from a file made on a computer.
    case imported
}

/// The RPPairing file Built-in StikJIT authenticates with. Lives in Documents,
/// so it is visible through Finder file sharing -- it is device-sensitive and
/// is only ever sent to Husk's own helper process.
enum JITPairingFileStore {
    static var directory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT", isDirectory: true)
    }
    static var url: URL { directory.appendingPathComponent("pairingFile.plist") }
    static var exists: Bool { FileManager.default.fileExists(atPath: url.path) }
    private static let sourceKey = "husk.jitPairingSource"

    static var source: JITPairingSource? {
        guard exists else { return nil }
        return UserDefaults.standard.string(forKey: sourceKey).flatMap(JITPairingSource.init(rawValue:)) ?? .imported
    }

    static func data() throws -> Data { try Data(contentsOf: url) }

    static func importFile(from source: URL) throws {
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        try store(try Data(contentsOf: source), source: .imported)
    }

    /// Validates an RPPairing plist and makes it the pairing file.
    static func store(_ data: Data, source: JITPairingSource) throws {
        guard !data.isEmpty,
              let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
              let dictionary = plist as? [String: Any],
              let publicKey = dictionary["public_key"] as? Data, publicKey.count == 32,
              let privateKey = dictionary["private_key"] as? Data, privateKey.count == 32,
              let identifier = dictionary["identifier"] as? String, !identifier.isEmpty else {
            throw NSError(domain: "HuskJIT", code: 10,
                          userInfo: [NSLocalizedDescriptionKey:
                            "That is not a remote pairing file. Make one with the StikDebug pairing-file guide and try again."])
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try data.write(to: url, options: .atomic)
        UserDefaults.standard.set(source.rawValue, forKey: sourceKey)
    }
}

@MainActor
final class JITCoordinator: ObservableObject {
    static let shared = JITCoordinator()

    /// Why the built-in helper could not reach this device, read from its error.
    enum ConnectionProblem: Equatable {
        /// Nothing answered at LocalDevVPN's address: the VPN is off or not routing.
        case vpn
        /// The device closed the connection, usually because it no longer accepts
        /// this pairing (every new pairing replaces the last).
        case pairing

        init?(helperMessage: String) {
            let text = helperMessage.lowercased()
            let has = { (needles: [String]) in needles.contains(where: text.contains) }
            if has(["connectionreset", "connection reset", "device refused connection"]) {
                self = .pairing
            } else if has(["connectionrefused", "connection refused", "timedout", "timed out",
                           "networkunreachable", "network unreachable", "hostunreachable",
                           "host unreachable", "no route"]) {
                self = .vpn
            } else {
                return nil
            }
        }

        var message: String {
            switch self {
            case .vpn:
                return "Husk couldn't reach this device. Connect LocalDevVPN, then try again."
            case .pairing:
                return "This device closed the connection, which usually means it no longer accepts "
                     + "Husk's pairing. Pair again, and check that LocalDevVPN is connected."
            }
        }
    }

    @Published var method: JITMethod {
        didSet { UserDefaults.standard.set(method.rawValue, forKey: "husk.jitMethod") }
    }
    @Published var showSetup = false
    @Published private(set) var connectionProblem: ConnectionProblem?
    @Published private(set) var busy = false
    @Published private(set) var status: String?
    @Published private(set) var error: String?
    @Published private(set) var txmPresent: Bool?
    /// The setup check passed for the current pairing file.
    @Published private(set) var prepared = false
    @Published private(set) var pairingSource = JITPairingFileStore.source
    /// Bumped each time the built-in helper attaches. Husk stays in the
    /// foreground for that, so nothing else tells ContentView to claim the region.
    @Published private(set) var attachGeneration = 0

    var hasPairing: Bool { pairingSource != nil }

    private init() {
        method = UserDefaults.standard.string(forKey: "husk.jitMethod")
            .flatMap(JITMethod.init(rawValue:)) ?? .automatic
    }

    var resolvedMethod: JITMethod {
        guard method == .automatic else { return method }
        if JITBootstrap.isStikDebugInstalled { return .stikDebug }
        if JITBootstrap.isTrollStoreInstalled { return .trollStore }
        if JITBootstrap.isJailbroken { return .jailbreak }
        return .builtIn
    }

    var automaticDescription: String {
        switch resolvedMethod {
        case .stikDebug: return "StikDebug is installed, so Husk will open it."
        case .trollStore: return "TrollStore is installed, so Husk will ask it to enable JIT."
        case .jailbreak: return "This device is jailbroken, so JIT comes from the jailbreak's Allow JIT in Apps setting."
        default: return "StikDebug, TrollStore and a jailbreak were not found, so Husk will use its built-in helper."
        }
    }

    func refreshPairingStatus() {
        pairingSource = JITPairingFileStore.source
    }

    private func log(_ line: String) { HuskLog.log("jit", line) }

    /// Get a debugger attached with whichever method applies, or open the setup
    /// walkthrough when that method is not set up yet.
    /// "Turn on JIT at launch": when Husk opens without JIT, ask the built-in StikJIT for it straight away, once per launch.
    /// Only when StikJIT can do it unattended -- it is available here and already paired -- so opening Husk never lands in
    /// the setup walkthrough by surprise.
    static let autoEnableKey = "husk.jit.autoEnable"
    private var autoTried = false

    /// A Husk TrollStore installed asks TrollStore for JIT each time it opens without it (#135), unless turned off in Settings:
    /// TrollStore attaches to the running Husk for a moment, with nothing to tap.
    static let trollStoreAutoKey = "husk.jit.trollStoreAuto"
    static var trollStoreAuto: Bool { UserDefaults.standard.object(forKey: trollStoreAutoKey) as? Bool ?? true }

    func enableAtLaunchIfAsked() {
        if !autoTried, JITBootstrap.isInstalledWithTrollStore, Self.trollStoreAuto {
            autoTried = true
            guard !JITBootstrap.ready else { return }
            log("installed by TrollStore: asking it for JIT at launch")
            method = .trollStore
            enable()
            return
        }
        guard !autoTried, UserDefaults.standard.bool(forKey: Self.autoEnableKey) else { return }
        autoTried = true
        guard !JITBootstrap.ready else { return }
        guard HuskBuiltInJIT.isAvailable, hasPairing else {
            log("JIT at launch is on, but built-in StikJIT is not available or not paired; skipped")
            return
        }
        log("turning JIT on at launch with built-in StikJIT")
        method = .builtIn
        enable()
    }

    func enable() {
        guard !JITBootstrap.ready else { return }
        error = nil
        connectionProblem = nil
        switch resolvedMethod {
        case .automatic:
            assertionFailure("Automatic must resolve to a concrete JIT method")
        case .stikDebug:
            if !JITBootstrap.requestAttach(), !JITBootstrap.requestTrollStoreAttach() {
                error = "StikDebug is not installed. Install it, or set up Built-in StikJIT."
                showSetup = true
            }
        case .trollStore:
            if JITBootstrap.requestTrollStoreAttach() {
                awaitTrollStore()
            } else {
                error = "TrollStore could not be opened. Install Husk through TrollStore, or choose another method."
                showSetup = true
            }
        case .jailbreak:
            // Nothing to ask: a jailbreak marks an app as debugged as it opens, if it has been told to. Say what to turn on.
            error = "Turn on Allow JIT in Apps in Dopamine's settings, then open Husk again."
            showSetup = true
        case .builtIn:
            guard HuskBuiltInJIT.isAvailable, hasPairing else {
                log("built-in JIT is not set up; opening the walkthrough")
                showSetup = true
                return
            }
            enableBuiltIn()
        }
    }

    /// After TrollStore is asked: it opens Husk again and attaches to it for a moment, which marks the process as debugged. Watch for
    /// that, so the Games tab and the Library notice without waiting for another trip to the foreground, and say what to check if
    /// it never comes -- TrollStore only answers enable-jit once its URL Scheme setting is on.
    private func awaitTrollStore() {
        busy = true
        status = "Waiting for TrollStore to enable JIT…"
        log("waiting for TrollStore's attach")
        _ = Self.waitForDebugger(timeout: 60) { [weak self] attached in
            guard let self else { return }
            busy = false
            if attached {
                status = "JIT is on."
                error = nil
                attachGeneration += 1
                log("TrollStore enabled JIT")
            } else {
                status = nil
                error = "TrollStore did not enable JIT. In TrollStore's Settings turn on URL Scheme, and make sure Husk was "
                      + "installed through TrollStore (or is signed with get-task-allow)."
                log("TrollStore did not enable JIT within a minute")
                showSetup = true
            }
        }
    }

    func importPairingFile(_ source: URL) {
        do {
            try JITPairingFileStore.importFile(from: source)
            OnDevicePairing.shared.cancel()
            refreshPairingStatus()
            method = .builtIn
            prepared = false
            status = "Pairing file imported."
            error = nil
            log("pairing file imported")
        } catch {
            self.error = error.localizedDescription
        }
    }

    /// OnDevicePairing finished: its file becomes the pairing file and Built-in StikJIT the method.
    func storeOnDevicePairing(_ data: Data) throws {
        try JITPairingFileStore.store(data, source: .onDevice)
        refreshPairingStatus()
        method = .builtIn
        prepared = false
        status = "Paired on this device."
        error = nil
    }

    /// Check LocalDevVPN and the Developer Disk Image, mounting it when needed.
    func prepareBuiltIn() {
        guard let pairing = builtInPairing() else { return }
        busy = true
        error = nil
        connectionProblem = nil
        status = "Checking LocalDevVPN and the Developer Disk Image…"
        HuskBuiltInJIT.send(.prepare(pairingData: pairing)) { [weak self] result in
            guard let self else { return }
            busy = false
            switch result {
            case .success(let response):
                txmPresent = response.txmPresent
                prepared = response.success
                status = response.success ? response.message : nil
                error = response.success ? nil : helperFailure(response.message)
                log("setup check \(response.success ? "passed" : "failed"): \(response.message)")
            case .failure(let failure):
                prepared = false
                status = nil
                error = failure.localizedDescription
            }
        }
    }

    func enableBuiltIn() {
        guard let pairing = builtInPairing() else { return }
        guard let script = JITBootstrap.scriptBase64 else {
            error = "Husk's JIT script is missing from this installation. Reinstall Husk."
            return
        }

        busy = true
        error = nil
        connectionProblem = nil
        status = "Starting Husk's JIT helper…"
        log("enabling JIT with the built-in helper")
        var readiness: Timer?
        var finished = false
        let finish: (String?) -> Void = { [weak self] failure in
            guard let self, !finished else { return }
            finished = true
            readiness?.invalidate()
            busy = false
            if let failure {
                status = nil
                error = failure
                log("built-in JIT failed: \(failure)")
                showSetup = true
            } else {
                status = "JIT is on."
                error = nil
                attachGeneration += 1
                log("built-in helper attached")
            }
        }

        HuskBuiltInJIT.send(
            .enable(targetPID: getpid(), pairingData: pairing, scriptBase64: script),
            started: { [weak self] in
                self?.status = "Waiting for the helper to attach…"
                readiness = Self.waitForDebugger { attached in
                    finish(attached ? nil : "The helper did not attach within 90 seconds. "
                                          + "Check that LocalDevVPN is connected, then try again.")
                }
            },
            completion: { [weak self] result in
                // The helper replies once Husk detaches, or when attaching failed.
                switch result {
                case .success(let response):
                    self?.txmPresent = response.txmPresent
                    HuskLog.log("jit", "helper: \(response.message)")
                    if !response.success {
                        finish(self?.helperFailure(response.message) ?? response.message)
                    }
                case .failure(let failure):
                    finish(failure.localizedDescription)
                }
            })
    }

    /// Renew the ATTACH when this process is only MARKED debugged and no region was ever granted.
    ///
    /// `CS_DEBUGGED` is not "a servicer is answering": it is set for a process a launcher marked, and
    /// it is left behind by a debugger that let go. On a TXM device it grants no executable memory by
    /// itself, so the claim at launch goes to nobody, and the old build then refused every later claim
    /// (`husk_ios_jit_prewarm` returned false from then on) -- the region was never granted, the arena
    /// was never created, and every title sat at 0 frames with the log saying "StikDebug is not servicing
    /// traps" and not one further probe. The working log is the other order: the region is claimed while
    /// the servicer that answered is attached. So ask Husk's own method for a real attach, ONCE per
    /// process, and let the next foreground pass claim the region then -- StikDebug by its `stikjit://`
    /// hand-off (the user's own flow), Husk's helper by an extension request, and for a jailbreak say
    /// which setting to turn on. Nothing is claimed here and no `brk` is issued: the attach is the whole
    /// of what is asked for, and a method that cannot be asked from inside the app says so instead.
    @discardableResult
    func renewAttachAfterUnservicedClaim() -> Bool {
        // And only when there is genuinely nothing else: on an OS where a plain MAP_JIT mapping executes,
        // the layer can make its own region and no attach is needed (the pre-iOS-26 routes).
        guard JITBootstrap.markedDebuggedWithoutRegion, !JITBootstrap.mapJITWorks,
              !renewedAfterUnservicedClaim else { return false }
        renewedAfterUnservicedClaim = true
        let method = resolvedMethod
        log("the process is marked debugged but no JIT region was granted: the flag is set with nothing "
            + "answering a brk (a launcher marks it, or a debugger that let go leaves it behind), and on this "
            + "device it grants no executable memory by itself. Asking \(method.title) for a REAL attach, once, "
            + "so the next foreground pass can claim the region while the servicer is attached")
        switch method {
        case .stikDebug:
            if JITBootstrap.requestAttach() { return true }
            error = "The process is marked debugged, but nothing that answers a JIT request is attached, and "
                  + "StikDebug could not be opened. Install StikDebug and turn JIT on, or set up Built-in StikJIT."
        case .trollStore:
            if JITBootstrap.requestTrollStoreAttach() { awaitTrollStore(); return true }
            error = "The process is marked debugged but no JIT region was granted, and TrollStore could not be "
                  + "opened. In TrollStore's Settings turn on URL Scheme, then turn JIT on and try again."
        case .builtIn:
            // Husk's own helper: the extension request IS what installs the servicer, and it can be made
            // from inside the app, so this is the one method that needs no hand-off at all.
            guard HuskBuiltInJIT.isAvailable else {
                error = HuskBuiltInJIT.unavailableReason
                showSetup = true
                return false
            }
            guard hasPairing else {
                error = "The process is marked debugged but no JIT region was granted, and Built-in StikJIT is not "
                      + "paired yet. Pair this device or import its pairing file, then try again."
                showSetup = true
                return false
            }
            enableBuiltIn()
            return true
        case .jailbreak:
            error = "The process is marked debugged but no JIT region was granted. Turn on Allow JIT in Apps in "
                  + "Dopamine's settings, then open Husk again."
        case .automatic:
            break
        }
        showSetup = true
        return false
    }

    /// The once-per-process guard for the ask above: a servicer that never answers must not turn every
    /// foreground pass into another attach (and every attach into another claim attempt).
    private var renewedAfterUnservicedClaim = false

    /// Renew the debugger's presence before a run starts, when it is no longer there.
    ///
    /// The JIT region is one-shot; the grant is not. The grant is the debugger's *presence*, and a
    /// fresh attach is how presence is obtained, so this asks for an attach and nothing else — no
    /// allocation, no re-claim, no `brk`. It is deliberately decided from the free
    /// `CS_DEBUGGED` flag (`JITBootstrap.debuggedFlag`, a `csops`, no trap) and never from the
    /// `brk #0x69` probe: a probe into a debugger that is attached but suspended is exactly the
    /// stall this whole hold has to avoid, and it would freeze on the first call.
    ///
    /// Started before a run rather than blocking it: the grant matters at the guest's *first JIT
    /// write*, which is well after the engine's start. For Husk's own helper the renew is a request,
    /// safe mid-session; for an external debugger the app cannot renew it at all, so nothing is
    /// started on the user's behalf and the status says exactly what they must do. Either way the
    /// layer still reports its own measurement of the guest's arena (`tl_xmem_exec_verdict`), which
    /// is what the "JIT is on" indicator reads — never a flag this app set.
    func reassertForRun() {
        guard JITBootstrap.prewarmed, !JITBootstrap.detached else { return }
        // Measure before touching anything: is execute still there at the moment a run starts? A "NO"
        // here, with no call of the layer's in between, is the debugger's presence going away -- the
        // hazard the old detach was written for, and the only other candidate. A "NO" after the
        // re-attach below would make the re-attach itself the cost.
        let live = husk_ios_jit_exec_gate("a run is about to start, before any re-attach")
        log("execute at the run start: " + (live ? "RAN -- the grant is held and live"
                                                  : "FAULTED -- NO execute at the region, and this run has not started yet"))
        guard !JITBootstrap.debuggedFlag else { return }   // still attached: nothing to renew
        log("a run is about to start with no debugger attached; asking for one again "
            + "(the region is kept, only the attach is renewed)")
        guard !busy else { return }
        switch resolvedMethod {
        case .builtIn:
            // Husk's own helper: an extension request, no foreground hand-off, so it is safe to
            // ask for mid-session. It attaches and services traps exactly as the first one did.
            enableBuiltIn()
        case .stikDebug, .trollStore, .jailbreak, .automatic:
            // These need the USER's hands — a foreground bounce to the external StikDebug app, or a
            // device setting — so the attach cannot be renewed from inside a run. Say exactly what to
            // do rather than start a run that will not have the grant. This is the one part of the
            // user's working flow the app cannot reproduce from inside itself: StikDebug is left
            // attached by the USER, not by Husk, so when it has gone the user brings it back.
            let title = resolvedMethod.title
            let how = "Open \(title) and let it enable JIT for Husk again, then come back and start the game."
            error = "JIT was turned off when the debugger let go. \(how)"
            status = nil
            log("the \(title) attach is gone and cannot be renewed from inside a run; telling the user "
                + "to re-attach: \(how)")
        }
    }
    func resetDDI() {
        busy = true
        error = nil
        status = "Resetting the Developer Disk Image cache…"
        HuskBuiltInJIT.send(.resetDDI) { [weak self] result in
            guard let self else { return }
            busy = false
            prepared = false
            switch result {
            case .success(let response):
                status = response.success ? response.message : nil
                error = response.success ? nil : response.message
            case .failure(let failure):
                status = nil
                error = failure.localizedDescription
            }
        }
    }

    private func builtInPairing() -> Data? {
        if let reason = HuskBuiltInJIT.unavailableReason {
            error = reason
            return nil
        }
        guard let pairing = try? JITPairingFileStore.data() else {
            error = "Pair this device or import its pairing file first."
            return nil
        }
        return pairing
    }

    /// The helper's failure as shown: a connection problem gets its plain
    /// explanation (the helper's own message is already in the log).
    private func helperFailure(_ message: String) -> String {
        connectionProblem = ConnectionProblem(helperMessage: message)
        return connectionProblem?.message ?? message
    }

    /// Polls CS_DEBUGGED until it is set or `timeout` passes.
    private static func waitForDebugger(timeout: TimeInterval = 90,
                                        completion: @escaping (Bool) -> Void) -> Timer {
        let deadline = Date().addingTimeInterval(timeout)
        let timer = Timer(timeInterval: 0.5, repeats: true) { timer in
            if JITBootstrap.debuggedFlag {
                timer.invalidate()
                completion(true)
            } else if Date() >= deadline {
                timer.invalidate()
                completion(false)
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        return timer
    }
}

/// LocalDevVPN, which Built-in StikJIT and StikDebug reach the device through:
/// open it to connect when it is installed, otherwise its App Store page.
enum LocalDevVPN {
    static let appStore = URL(string: "https://apps.apple.com/us/app/localdevvpn/id6755608044")!
    /// `enable` connects the VPN; `scheme` has LocalDevVPN return to Husk afterwards.
    static let connect = URL(string: "localdevvpn://enable?scheme=husk")!

    static var isInstalled: Bool { UIApplication.shared.canOpenURL(URL(string: "localdevvpn://")!) }
    static var actionTitle: String { isInstalled ? "Connect LocalDevVPN" : "Get LocalDevVPN" }

    @MainActor static func open() {
        let installed = isInstalled
        HuskLog.log("jit", "LocalDevVPN \(installed ? "connect" : "app-store")")
        UIApplication.shared.open(installed ? connect : appStore)
    }
}
