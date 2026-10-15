// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// How a game did the last time it was played here, noted by Husk itself: it played, it crashed, or it would not start.
///
/// Written to status.json in the game's folder as it happens, so the library can say at a glance which games work on this
/// iPhone without anyone keeping a list. Beside that result it now also keeps the *swap* verdict — how the layer let go of
/// the game when it was left — because "can I start another game without relaunching Husk?" is the thing that used to be a
/// mystery, and it belongs to an app and an engine, not to a feeling.
@MainActor
final class GameStatusStore: ObservableObject {
    static let shared = GameStatusStore()

    enum Result: String, Codable {
        case plays, crashed, failed

        var label: String {
            switch self {
            case .plays: return "Plays"
            case .crashed: return "Crashed last time"
            case .failed: return "Did not start"
            }
        }
        var symbol: String {
            switch self {
            case .plays: return "checkmark.circle.fill"
            case .crashed: return "exclamationmark.triangle.fill"
            case .failed: return "xmark.circle.fill"
            }
        }
        var color: Color {
            switch self {
            case .plays: return .green
            case .crashed: return .orange
            case .failed: return .red
            }
        }
    }

    /// How the session ended when this app was last left, so the library can say whether a *second* app can start in the
    /// same run of Husk. The three answers are the plan's own: it swapped cleanly, it swapped but leaked (with the layer's
    /// words about what), or the swap failed and Husk has to be relaunched for the next game.
    enum SwapVerdict: String, Codable {
        case clean
        case leaked
        case needsRelaunch

        var label: String {
            switch self {
            case .clean: return "Swapped cleanly"
            case .leaked: return "Swapped, with a leak"
            case .needsRelaunch: return "Swap failed — relaunch Husk"
            }
        }
        var symbol: String {
            switch self {
            case .clean: return "arrow.triangle.2.circlepath.circle.fill"
            case .leaked: return "drop.triangle.fill"
            case .needsRelaunch: return "power.circle.fill"
            }
        }
        var color: Color {
            switch self {
            case .clean: return .green
            case .leaked: return .orange
            case .needsRelaunch: return .red
            }
        }
        /// Whether the layer is really finished with the game, so the next one may start.
        var canStartAnother: Bool { self != .needsRelaunch }
    }

    struct Status: Codable {
        var result: Result
        var time: Date
        var detail: String?
        /// Why it stopped, when the run's log said (AppDiagnosis)
        var why: Why?
        /// How the session closed the last time this app was left. Optional and defaulted on purpose: status.json files
        /// written before this field existed have no `swap` key, and Swift decodes a missing optional to nil — so an old
        /// file still reads rather than crashing the library.
        var swap: Swap? = nil
    }
    struct Why: Codable { var kind: String; var title: String; var detail: String; var evidence: String? }

    /// The swap verdict as written down: the verdict's name, the layer's own words about what leaked (may be empty), and
    /// when. Kept as plain strings so a future verdict name is a reader's problem, not a decode failure.
    struct Swap: Codable { var verdict: String; var detail: String?; var time: Date? }

    /// A message worth showing over whatever is on screen (an outcome the user would otherwise not learn until they tried
    /// to start the next game). Rendered by the root view, exactly like the Android host's own toast.
    @Published var notice: Toast?

    /// The reason the last run stopped, while it has not played since.
    func diagnosis(_ appID: String) -> AppDiagnosis? {
        guard let s = status(appID), s.result != .plays, let w = s.why, let kind = AppDiagnosis.Kind(rawValue: w.kind) else { return nil }
        return AppDiagnosis(kind: kind, title: w.title, detail: w.detail, evidence: w.evidence)
    }

    func record(_ appID: String, _ result: Result, diagnosis d: AppDiagnosis) {
        guard !appID.isEmpty else { return }
        let s = Status(result: result, time: Date(), detail: d.title, why: Why(kind: d.kind.rawValue, title: d.title, detail: d.detail, evidence: d.evidence))
        cache[appID] = s
        if let data = try? JSONEncoder().encode(s) { try? data.write(to: Self.file(appID), options: .atomic) }
        HuskLog.log("status", "\(appID): \(result.rawValue) (\(d.title))")
    }

    @Published private var cache: [String: Status] = [:]
    private var loaded: Set<String> = []

    private static func file(_ appID: String) -> URL {
        TranslationLayer.root.appendingPathComponent(appID, isDirectory: true).appendingPathComponent("status.json")
    }

    func status(_ appID: String) -> Status? {
        if !loaded.contains(appID) {
            loaded.insert(appID)
            if let data = try? Data(contentsOf: Self.file(appID)),
               let s = try? JSONDecoder().decode(Status.self, from: data) { cache[appID] = s }
        }
        return cache[appID]
    }

    func record(_ appID: String, _ result: Result, detail: String? = nil) {
        guard !appID.isEmpty, status(appID)?.result != result || result != .plays else { return }
        let s = Status(result: result, time: Date(), detail: detail, why: nil)
        cache[appID] = s
        if let data = try? JSONEncoder().encode(s) { try? data.write(to: Self.file(appID), options: .atomic) }
        HuskLog.log("status", "\(appID): \(result.rawValue)\(detail.map { " (\($0))" } ?? "")")
    }

    // MARK: the swap verdict

    /// Write down how the swap went for this app, keeping whatever the last run's result already said. A verdict with no
    /// result recorded before it is only honest as "did not start" unless the swap verified — a game that swapped cleanly
    /// plainly ran, so that one counts as played.
    func recordSwap(_ appID: String, _ verdict: SwapVerdict, detail: String? = nil) {
        guard !appID.isEmpty else { return }
        let now = Date()
        var s = status(appID) ?? Status(result: verdict == .clean ? .plays : .failed, time: now, detail: nil, why: nil)
        s.swap = Swap(verdict: verdict.rawValue, detail: detail, time: now)
        cache[appID] = s
        if let data = try? JSONEncoder().encode(s) { try? data.write(to: Self.file(appID), options: .atomic) }
        HuskLog.log("swap", "\(appID): \(verdict.rawValue)\(detail.map { " (\($0))" } ?? "")")
    }

    /// The swap verdict for this app, or nil when it has never been left since the feature existed.
    func swapVerdict(_ appID: String) -> SwapVerdict? {
        guard let raw = status(appID)?.swap?.verdict else { return nil }
        return SwapVerdict(rawValue: raw)
    }

    /// The layer's words about the last swap, when it had any. nil for an app that has never been left, or left cleanly.
    func swapDetail(_ appID: String) -> String? {
        guard let d = status(appID)?.swap?.detail, !d.isEmpty else { return nil }
        return d
    }
}

// MARK: - the session, as the layer sees it

/// What the layer says about swapping the engine that is running for another in the same process
/// (`husk_tl_swap_capability`): SWAP_OK, SWAP_PARTIAL or SWAP_NEEDS_RESTART. The classification is the layer's table,
/// asserted per engine; Swift only reads the answer.
enum SwapCapability: Int32 {
    case ok = 0
    case partial = 1
    case needsRestart = 2

    /// Whether another app may be started after this engine at all, without relaunching Husk.
    var canSwap: Bool { self != .needsRestart }

    var label: String {
        switch self {
        case .ok: return "Can be swapped"
        case .partial: return "Swaps, with a caveat"
        case .needsRestart: return "Needs a relaunch"
        }
    }
    var detail: String {
        switch self {
        case .ok: return "After this game, another can start without relaunching Husk."
        case .partial: return "Another game can start, but this engine leaves something behind."
        case .needsRestart: return "This engine cannot be swapped in the same process; Husk must be relaunched for another game."
        }
    }
    var symbol: String {
        switch self {
        case .ok: return "arrow.triangle.2.circlepath"
        case .partial: return "arrow.triangle.2.circlepath.circle"
        case .needsRestart: return "power.circle"
        }
    }
    var color: Color {
        switch self {
        case .ok: return .green
        case .partial: return .orange
        case .needsRestart: return .red
        }
    }
}

/// What the layer answers when it is asked to close the running session (`husk_tl_session_close`), the same SWAP_* the
/// header names: closed cleanly, closed with a thread left parked (a partial swap — something leaked), or it refused and
/// the gate stays closed (Husk must be relaunched for the next game).
enum SessionClose: Int32 {
    case clean = 0          // SWAP_OK
    case partial = 1        // SWAP_PARTIAL: a thread was quarantined, and named in the log
    case needsRestart = 2   // SWAP_NEEDS_RESTART: refused; the gate did not open

    /// Closed, whether or not a thread was quarantined: the gate to the next app may open.
    var closed: Bool { self != .needsRestart }
}

/// The one native (translation-layer) session, and the two halves of the swap: closing it when a game is left, and
/// verifying the swap once a different app has loaded.
///
/// Husk can only run one translated app per process because an engine cannot be unloaded. The layer now offers a session
/// boundary — close it and a second app can start — and this is Swift's half of the contract. What the layer says outright
/// (a refusal, a leak) is written down as given. A *clean* close is deliberately not written down as a swap: a swap is not
/// done because it was asked for — it is done when the next app has actually loaded and `husk_tl_verify_swap` passes, and
/// only then is the verdict recorded, against the app that swapped in.
@MainActor
enum NativeSession {
    /// Whether a native session is open right now. This is the gate for starting a second app: the layer clears it when
    /// `husk_tl_session_close()` succeeds (`husk_native_loaded_apk` goes NULL), so one app at a time holds without the UI
    /// having to remember anything.
    static var isOpen: Bool { husk_native_loaded_apk() != nil }

    /// Whether any engine that can execute guest code is still live, so the JIT grant must stay held.
    ///
    /// This is the guard on releasing the debugger. The Android guest's TCG translation buffer IS
    /// the prewarmed JIT region (`husk_ios_jit_allocate` hands it over), so QEMU needs the
    /// debugger's execute grant exactly as a native engine does — and the guest keeps running
    /// after its surface is left (`ContentView`: "the guest runs continuously once started").
    /// Releasing the grant while either is live would take execute away from generated code
    /// mid-run, which is the very failure the hold exists to end.
    static var guestOrEngineLive: Bool { QemuRunner.shared.isRunning || isOpen }

    /// The APK the layer is holding, if any.
    static var loadedAPK: String? { husk_native_loaded_apk().map { String(cString: $0) } }

    /// What the layer says about swapping this engine for another, read before anything is started. An answer we do not
    /// recognise is treated as the worst case: refusing to promise a swap is the only honest reading of an unknown code.
    static func capability(_ engine: TLNativeEngine) -> SwapCapability {
        SwapCapability(rawValue: husk_tl_swap_capability(engine.swapID)) ?? .needsRestart
    }

    struct Outcome {
        let verdict: GameStatusStore.SwapVerdict
        /// What to tell the user, when simply letting them start the next game would not be true.
        let message: Toast?
    }

    /// Leaving a game: ask the layer to close its session so a different app can start later.
    ///
    /// Returns nil when nothing is open: there is no session to close, and inventing a verdict for one would be a lie.
    /// Only the layer's own verdict is recorded here, and only when it is a caveat — a refusal (`SWAP_NEEDS_RESTART`,
    /// the session restored as it was) or a leak (`SWAP_PARTIAL`). A clean close records nothing, because it is not a
    /// swap yet; `verifyLoaded` writes that down once a second app has loaded and the layer has verified it.
    @discardableResult
    static func close(appID: String, apk: String?, package: String?, engine: TLNativeEngine) -> Outcome? {
        guard isOpen else { return nil }
        // The layer's own words about the close, if it has any. `husk_tl_swap_status` returns a const string the layer
        // owns, so unlike the attempt log it is read and not freed.
        let detail = statusText()
        switch SessionClose(rawValue: husk_tl_session_close()) ?? .needsRestart {
        case .needsRestart:
            let tail = "This game's engine would not give up its session, so another game cannot start until Husk is relaunched."
            return finish(appID, .needsRelaunch, detail: detail,
                          message: Toast(title: "Close and reopen Husk", detail: detail.map { "\($0). \(tail)" } ?? tail, good: false))
        case .partial:
            let tail = "The next game can start, but something from this session was left behind."
            let outcome = finish(appID, .leaked, detail: detail,
                                 message: Toast(title: "Swapped, with a leak", detail: detail.map { "\($0). \(tail)" } ?? tail, good: false))
            // The layer replaced the session (with a leak): no native run is live. The Android
            // guest may still be, and QEMU shares this grant, so release only when nothing is.
            if !guestOrEngineLive { JITBootstrap.releaseGrantWhenIdle() }
            return outcome
        case .clean:
            // The layer cleared its session: nothing can execute guest code now, so this is the
            // moment the debugger's execute grant can be given up (JITBootstrap.holdGrantForRun).
            // Only when the Android guest is not up too: QEMU's TCG buffer is this same region.
            if !guestOrEngineLive { JITBootstrap.releaseGrantWhenIdle() }
            return Outcome(verdict: .clean, message: nil)
        }
    }

    /// The other half of the swap. Once a different app has actually loaded, ask the layer whether the swap really happened
    /// and write the verdict down against the app that is now running. `husk_tl_verify_swap` proves all four checks at once
    /// (the loaded APK is the one requested, no library from an earlier session is reachable, the new engine has drawn
    /// frames, and the accounting is in the log); a swap that does not verify — or whose close had refused — is a relaunch,
    /// never a success. Skipped when nothing was closed in this process (the first app did not swap in), and only called
    /// once the new engine has produced frames.
    @discardableResult
    static func verifyLoaded() -> Outcome? {
        // `husk_tl_swap_verdict` is -1 until a close has run: with nothing closed there is no swap to verify.
        guard husk_tl_swap_verdict() >= 0 else { return nil }
        guard let apk = loadedAPK,
              let app = TranslationLayerStore.shared.apps.first(where: { $0.apks.first == apk }) else { return nil }
        let detail = statusText()
        let last = SessionClose(rawValue: husk_tl_swap_verdict()) ?? .needsRestart
        // A refused close left the old session in place, so there is nothing to verify and it is already a relaunch.
        let verified = last == .needsRestart ? false : husk_tl_verify_swap(apk, app.packageName ?? "") != 0
        let verdict: GameStatusStore.SwapVerdict
        if !verified {
            verdict = .needsRelaunch
        } else if last == .partial {
            verdict = .leaked
        } else {
            verdict = .clean
        }
        return finish(app.id, verdict, detail: detail, message: message(for: verdict, detail: detail))
    }

    /// Close whatever session is open, for callers that do not know which app it belongs to (a surface that is simply
    /// being left). Resolves the app from the store so the verdict still lands on the right one.
    @discardableResult
    static func closeLoaded() -> Outcome? {
        guard let apk = loadedAPK else { return nil }
        guard let app = TranslationLayerStore.shared.apps.first(where: { $0.apks.first == apk }) else {
            // A session we cannot match to an app still deserves closing; there is just nowhere honest to write a verdict,
            // so this close is silent rather than promising an outcome nobody can be shown.
            _ = husk_tl_session_close()
            if !guestOrEngineLive { JITBootstrap.releaseGrantWhenIdle() }
            return nil
        }
        return close(appID: app.id, apk: apk, package: app.packageName, engine: app.screenEngine)
    }

    /// The verdict, written down for the app and turned into the one thing worth saying.
    private static func finish(_ appID: String, _ verdict: GameStatusStore.SwapVerdict,
                               detail: String?, message: Toast?) -> Outcome {
        GameStatusStore.shared.recordSwap(appID, verdict, detail: detail)
        return Outcome(verdict: verdict, message: message)
    }

    private static func message(for verdict: GameStatusStore.SwapVerdict, detail: String?) -> Toast? {
        switch verdict {
        case .clean:
            return nil
        case .leaked:
            let tail = "The next game can start, but something from this session was left behind."
            return Toast(title: "Swapped, with a leak", detail: detail.map { "\($0). \(tail)" } ?? tail, good: false)
        case .needsRelaunch:
            return Toast(title: "Close and reopen Husk",
                         detail: detail ?? "The session could not be replaced, so reopen Husk before starting another game.",
                         good: false)
        }
    }

    /// The layer's own words about the last close, or nil when it has nothing to say.
    private static func statusText() -> String? {
        guard let c = husk_tl_swap_status() else { return nil }
        let text = String(cString: c).trimmingCharacters(in: .whitespacesAndNewlines)
        return text.isEmpty ? nil : text
    }
}

extension TLNativeEngine {
    /// The engine's id for the layer's swap calls, taken straight from the header's `HUSK_TL_ENGINE_*` (which mirrors the
    /// C side's own `ENGINE_*` enum), so Swift and the layer cannot drift apart on the numbering.
    var swapID: Int32 {
        switch self {
        case .unity: return Int32(HUSK_TL_ENGINE_UNITY)
        case .cocos: return Int32(HUSK_TL_ENGINE_COCOS)
        case .minecraft: return Int32(HUSK_TL_ENGINE_GAMEACTIVITY)
        case .sdl: return Int32(HUSK_TL_ENGINE_SDL)
        case .ue4: return Int32(HUSK_TL_ENGINE_UE4)
        case .gta: return Int32(HUSK_TL_ENGINE_GTA)
        case .godot: return Int32(HUSK_TL_ENGINE_GODOT)
        // The NativeActivity driver runs through husk_ue4_launch, so it is the Unreal engine's number here too.
        case .nativeactivity: return Int32(HUSK_TL_ENGINE_UE4)
        case .gamemaker: return Int32(HUSK_TL_ENGINE_GAMEMAKER)
        case .java: return Int32(HUSK_TL_ENGINE_JAVA)
        case .flutter: return Int32(HUSK_TL_ENGINE_FLUTTER)
        }
    }
}

// MARK: - the marks and lines the library draws

/// The small mark on a game's icon in the library.
struct GameStatusBadge: View {
    let result: GameStatusStore.Result

    var body: some View {
        Image(systemName: result.symbol)
            .font(.system(size: 15, weight: .bold))
            .symbolRenderingMode(.palette)
            .foregroundStyle(.white, result.color)
            .background(Circle().fill(Color(uiColor: .systemBackground)).padding(1))
            .accessibilityLabel(result.label)
    }
}

/// The library mark for a swap with a caveat: an engine that cannot be swapped at all, or a run that left something behind
/// or would not close. Nothing is drawn for a clean swap: the result badge beside it already says the game plays.
struct SwapBadge: View {
    let symbol: String
    let color: Color
    let label: String

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: 12, weight: .bold))
            .symbolRenderingMode(.palette)
            .foregroundStyle(.white, color)
            .background(Circle().fill(Color(uiColor: .systemBackground)).padding(1))
            .accessibilityLabel(label)
    }
}

/// The line that answers, before the tap, whether this game can be swapped in for another in the same run of Husk.
struct SwapCapabilityRow: View {
    let engine: TLNativeEngine

    var body: some View {
        let cap = NativeSession.capability(engine)
        LabeledContent("Another game") {
            Label(cap.label, systemImage: cap.symbol).foregroundStyle(cap.color)
        }
    }
}

/// The line that answers, after a run, how the swap actually went — and what the layer said leaked, when it did.
struct SwapVerdictRow: View {
    let verdict: GameStatusStore.SwapVerdict
    let detail: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Label(verdict.label, systemImage: verdict.symbol).foregroundStyle(verdict.color)
            if let detail, !detail.isEmpty {
                Text(detail).font(.footnote).foregroundStyle(.secondary)
            }
        }
    }
}
