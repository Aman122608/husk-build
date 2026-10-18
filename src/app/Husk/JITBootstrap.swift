// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import UIKit
import os

/// `csops(2)`. Reading CS_DEBUGGED is how we ask "is a debugger attached right
/// now" without executing anything — unlike the brk probe, which costs a trap.
@_silgen_name("csops")
private func csops(_ pid: Int32, _ ops: Int32,
                   _ useraddr: UnsafeMutableRawPointer?, _ usersize: Int) -> Int32

private let CS_OPS_STATUS = Int32(0)
private let CS_DEBUGGED = UInt32(0x10000000)

/// Gets Husk from "launched normally, no executable memory" to "JIT is live".
///
/// On iOS 27 every supported device enforces TXM, so the app cannot grant itself
/// executable memory — only an attached debugger can. That debugger is either
/// StikDebug, which Husk hands the JIT script inline over its URL scheme, or
/// Husk's own built-in StikJIT helper (JITSetup.swift), which runs the same
/// script from an app extension.
enum JITBootstrap {
    private static let log = Logger(subsystem: "com.husk.app", category: "jit")

    enum State: Equatable {
        case unknown
        case waitingForDebugger
        case live
        case failed(String)
    }

    /// Install the trap guard. Must run before anything can execute a `brk`.
    ///
    /// Without this, a `brk` that StikDebug is not there to service is a fatal
    /// SIGTRAP rather than a failed call — the process simply dies.
    static func installTrapGuard() {
        HuskLog.log("jit", "installing brk trap guard")
        husk_ios_jit_install_trap_handler()
        HuskLog.log("jit", "trap guard installed -- an unserviced brk will now "
                         + "return 0 instead of killing the process")
    }

    /// Size QEMU will ask for. Must match tb-size in the phase 1 command line:
    /// a smaller region here means QEMU allocates a second one, at a point where
    /// StikDebug may be long gone.
    // Back to 256 MiB. Raising this to 512 was one of three changes made at
    // once in v15, and v15 was the first build to die inside qemu_init(). The
    // guest RAM -- the other suspect -- has since been shown to map and write
    // cleanly at 6144 MiB, which leaves this. The region itself allocates and
    // passes its selftest at 512; whatever objects is further in, where TCG
    // carves the buffer into per-vCPU regions.
    //
    // 512 MiB since the native runtime runs Minecraft: its main library alone is a 354 MiB image that has to sit in this
    // region, with the stubs the loader places beside it. A larger prewarm is safe for QEMU, which is handed the
    // prewarmed region whenever it is at least what tb-size asks for.
    //
    // THE SIZE IS NOW A SETTING (husk.jitArenaMiB), read HERE, at launch: the region is claimed at boot, before the guest
    // download, so a change takes effect at the NEXT launch, never this one. The default is unchanged at 512 MiB, so
    // nothing moves unless the owner moves it. This is NOT a performance lever -- the frame rate was capped by which VIEW
    // the guest's writable memory was named through, not by how large the region is (that is fixed) -- but it decides
    // whether a BIGGER title can hold its code+data+heap, whether a low-memory device can afford the claim, and whether a
    // game that cannot get the memory it asks for can be told so and retried at a larger number.
    static let defaultArenaMiB = 512

    /// The arena-size setting's honest range, DERIVED from the layer, not invented:
    ///
    ///  - the floor is 64 MiB. husk-tl-xmem.c never claims a smaller region: `grant_self` halves the request down to
    ///    `size >= (64u << 20)` and stops, and `near_alias`'s largest-fitting-alias loop stops at
    ///    `want_size >= (64u << 20)`. Below 64 MiB the layer has no region size it will honour, so the setting must not
    ///    offer one.
    ///  - the ceiling is 2048 MiB. The region is mapped TWICE (an executable view and a writable twin), and the layer
    ///    places the twin within 2 GiB of the executable view -- `reach = (uintptr_t)2 << 30` in husk-tl-xmem.c
    ///    (`near`/`near_alias`) and in src/ios-jit/husk-ios-jit.c's placement of the writable alias. A claim beyond 2 GiB
    ///    could not keep its two views within that reach, so 2048 MiB is the mechanical ceiling the code supports.
    ///
    /// A value outside this range is CLAMPED, with a named log line saying what was asked and what was used -- never
    /// silently -- and the Settings row shows what was ACTUALLY claimed, not what was asked for.
    static let arenaMinMiB = 64
    static let arenaMaxMiB = 2048

    /// The user's arena-size setting, in MiB, clamped to the range above. Persisted across launches (UserDefaults key
    /// "husk.jitArenaMiB"), defaulting to `defaultArenaMiB` so today's 512 MiB claim is unchanged until he changes it.
    static var arenaMiB: Int {
        let stored = UserDefaults.standard.object(forKey: "husk.jitArenaMiB") as? Int ?? defaultArenaMiB
        return min(max(stored, arenaMinMiB), arenaMaxMiB)
    }

    /// Whether the stored setting had to be clamped (the raw value is outside `arenaMinMiB...arenaMaxMiB`). The row and
    /// the launch log both say so when it is; nothing is silently corrected.
    static var arenaClamped: Bool {
        let stored = UserDefaults.standard.object(forKey: "husk.jitArenaMiB") as? Int ?? defaultArenaMiB
        return stored != arenaMiB
    }

    /// The region size to claim, from the setting. Read at LAUNCH (see the note above `defaultArenaMiB`).
    static var jitBytes: Int { arenaMiB * 1024 * 1024 }

    /// The size (MiB) the debugger ACTUALLY granted at the claim, or 0 when no region is held. Straight from the layer
    /// (`husk_ios_jit_region_size`), so the row reports what was claimed, not what was asked.
    static var claimedMiB: Int { Int(husk_ios_jit_region_size() / (1024 * 1024)) }

    /// True once the region is held. The memory budget needs this: after a
    /// prewarm the JIT is already counted in the footprint, so subtracting it
    /// again charges for it twice and cost the guest 256 MiB.
    nonisolated(unsafe) static var prewarmed = false

    /// Take the JIT region now, while StikDebug is definitely still attached.
    ///
    /// StikDebug lets go after a while, and a first run spends a minute
    /// downloading 1.1 GB of guest image before QEMU starts. By the time
    /// qemu_init() asked for memory the debugger had detached, and there is no
    /// recovering from that in-process -- without a debugger there is no
    /// executable memory at all, and asking again later is precisely what does
    /// not work. So claim it first and hold it.
    @discardableResult
    static func prewarm() -> Bool {
        if prewarmed { return true }
        // Before iOS 26 Husk can make the region itself where the device allows it (TrollStore's dynamic-codesigning, a
        // jailbreak, or a debugger that attached and let go), so it does not wait for CS_DEBUGGED there: Dopamine hides that
        // flag from apps unless "Allow JIT in Apps" is on, while still letting them run code they wrote.
        guard isDebuggerAttached || (canGrantOwnJIT && !selfGrantTried) else {
            HuskLog.log("jit", "no debugger attached yet; not prewarming")
            return false
        }
        if !isDebuggerAttached { selfGrantTried = true }
        // Before iOS 26, unless StikDebug's script is what attached, no brk: TrollStore's enable-jit (and its Open with JIT)
        // attaches for 100 ms and services nothing, and a brk in that window stops Husk for good (TrollStore then says
        // "trollstorehelper returned 3"). A debugged process there makes its own executable memory anyway: QEMU maps its
        // buffer with MAP_JIT and the translation layer makes its region itself.
        if canGrantOwnJIT && !stikDebugMayServiceTraps {
            selfGrantTried = true
            let works = mapJITWorks
            HuskLog.log("jit", "not trapping for a JIT region (no StikDebug); MAP_JIT \(works ? "executes" : "does not execute") here")
            if works { lastFailure = nil }
            return works
        }
        // The arena-size setting: what was asked, what the honest range allowed, and what will be used. Never silent.
        if arenaClamped {
            let asked = UserDefaults.standard.object(forKey: "husk.jitArenaMiB") as? Int ?? defaultArenaMiB
            HuskLog.log("jit", "arena size setting: asked \(asked) MiB, but the layer's honest range is "
                             + "\(arenaMinMiB)-\(arenaMaxMiB) MiB (husk-tl-xmem.c: the region is never claimed below the "
                             + "64 MiB floor and its twin must stay within 2 GiB of the executable view), so this launch "
                             + "uses \(arenaMiB) MiB -- CLAMPED, not silently ignored")
        }
        HuskLog.log("jit", "claiming \(jitBytes / (1024 * 1024)) MiB of JIT memory now, "
                         + "before the guest download (arena size \(arenaMiB) MiB, from Settings -- read at launch, so "
                         + "a change takes effect at the next launch) -- the region's execute is the debugger's "
                         + "per-entry grant, so the debugger is held rather than detached")
        let ok = husk_ios_jit_prewarm(jitBytes)
        if ok {
            prewarmed = true
            lastFailure = nil
            // Do NOT detach here. The region's execute is the debugger's per-entry grant and it
            // dies with the detach: measured in the 085540 run, the boot selftest executed
            // generated code through the RX alias while the helper was attached, and every entry
            // of the same region read `prot 1 max 3` with the first real fetch faulting once it
            // had detached. So claiming the region is also the moment the grant is taken; it is
            // given up only when no run is live (releaseGrantWhenIdle), which is the one moment
            // a suspended servicer cannot stall anything.
            holdGrantForRun("the claim")
        }
        else if mapJITWorks {
            // Not a failure worth reporting: this is the ordinary shape of an
            // iOS that does not need a trap servicer. QEMU maps its own buffer
            // with MAP_JIT a moment later and runs exactly as well.
            lastFailure = nil
            HuskLog.log("jit", "no trap servicer, but MAP_JIT executes here -- "
                             + "QEMU will map its own buffer")
        } else {
            lastFailure = "The debugger is attached but is not answering trap "
                        + "requests, and this device will not execute a MAP_JIT "
                        + "mapping either, so no executable memory could be "
                        + "claimed. This is what happens when Husk runs inside "
                        + "another container app rather than sideloaded on its own."
        }
        HuskLog.log("jit", ok ? "JIT region secured; it will be handed to QEMU later"
                              : "JIT prewarm FAILED -- StikDebug is not servicing traps")
        return ok
    }

    /// An unconditional "never detach", as a user setting.
    ///
    /// The detach is a real decision and its cost is now known: the region's execute is the
    /// debugger's per-entry grant, and it does not survive the detach (measured — see prewarm).
    /// So the default of this build is the run-scoped hold below, and this setting is the blunt
    /// override: with it on, the debugger is never detached even when no run is live.
    ///
    /// What the detach protected against, and still does: while a debugger is attached, every stop
    /// event (a `brk`, a signal, a Mach exception) halts the whole process until the servicer
    /// answers. iOS suspends StikDebug soon after it hands the foreground back, so the next stop
    /// froze Husk with nothing in the log — on iOS 26 that read as Android hanging on its boot
    /// screen. Detaching removed the debugger from the picture; holding it is now paid for by
    /// giving it up at the first moment no run is live, and by never starting a run with a
    /// servicer that has gone (JITCoordinator.reassertForRun).
    static var keepDebuggerAttached: Bool {
        get { UserDefaults.standard.bool(forKey: "husk.keepDebuggerAttached") }
        set { UserDefaults.standard.set(newValue, forKey: "husk.keepDebuggerAttached") }
    }

    /// Whether a run is holding the debugger's execute grant.
    ///
    /// The JIT region is claimed ONCE and cannot be claimed again; the grant is not the region,
    /// it is the debugger's *presence*, and presence can be had again. So the hold is scoped to a
    /// run: taken when the region is claimed and when a run starts, given up the moment no run is
    /// live — the window in which a suspended servicer could stall anything is exactly the window
    /// in which the grant is needed.
    nonisolated(unsafe) static private(set) var grantHeldForRun = false

    /// Hold the debugger's execute grant for the life of a run.
    ///
    /// Cheap and idempotent: it only raises a flag and says so once. Nothing is allocated and no
    /// `brk` is executed here, so it is safe to call from the engine's start path.
    static func holdGrantForRun(_ reason: String) {
        guard prewarmed, !detached else { return }
        // Measure at the moment the hold is taken: the gate writes a stub through the writable alias and
        // calls it at the executable view, so the log says whether the grant is live right here. It is
        // the same measurement at the run start and after the claim, which is what makes the next run
        // name the event that cost it instead of guessing.
        _ = husk_ios_jit_exec_gate("holding the grant (\(reason))")
        if !grantHeldForRun {
            grantHeldForRun = true
            HuskLog.log("jit", "holding the debugger's execute grant (\(reason)): the per-entry "
                             + "grant does not survive a detach, so the debugger stays attached "
                             + "while a game can run; it is given up when no run is live")
        }
    }

    /// No run is live: the one moment it is safe to let the debugger go.
    ///
    /// Called when the layer has cleared its session (`NativeSession.close`) and no engine is
    /// loaded, so nothing can execute guest code and no `brk` can be issued until a new run
    /// starts — and a new run renews the grant first (`JITCoordinator.reassertForRun`).
    static func releaseGrantWhenIdle() {
        guard grantHeldForRun else { return }
        grantHeldForRun = false
        // The FLAG is what is dropped here; whether the debugger itself is let go is detachIfIdle's
        // decision, and it never ends an attach Husk did not make (the user's external StikDebug). So
        // this line does not promise a detach.
        HuskLog.log("jit", "no run is live; the run-scoped execute-grant hold is released (the debugger "
                         + "itself is only let go by Husk's own helper -- see detachIfIdle)")
        detachIfIdle()
    }

    /// Whether StikDebug's script may be the debugger: the only one that answers a brk.
    private static var stikDebugMayServiceTraps: Bool {
        let method = UserDefaults.standard.string(forKey: "husk.jitMethod") ?? JITMethod.automatic.rawValue
        return method == JITMethod.stikDebug.rawValue || (method == JITMethod.automatic.rawValue && isStikDebugInstalled)
    }

    /// Whether the attached debugger is an EXTERNAL app the USER brought in — StikDebug, via its
    /// `stikjit://` scheme, or a jailbreak/TrollStore marking — rather than Husk's own
    /// `HuskJITHelper.appex` extension, which Husk starts itself.
    ///
    /// This is who may end the session, and it is the crux of the user's flow: "it's really just a
    /// quick redirect to StikDebug and back to the app, and the app stays saying that jit is enabled".
    /// Husk started neither of those and cannot start them again on its own — the external StikDebug
    /// attach is a foreground bounce that needs the user's hands — so sending the detach
    /// (`brk #0xf00d`, `x16 = 0`, `husk-brk.S`) to one of them is not "releasing a servicer", it is
    /// revoking the only route this device has to executable memory, for the rest of the process, with
    /// no way back from inside the app. Only Husk's own helper, which one more extension request
    /// brings back, may be let go. Mirrors `JITCoordinator.resolvedMethod`, kept here so a
    /// non-MainActor caller need not hop actors to ask.
    static var debuggerIsExternalApp: Bool {
        let stored = UserDefaults.standard.string(forKey: "husk.jitMethod") ?? JITMethod.automatic.rawValue
        let method = JITMethod(rawValue: stored) ?? .automatic
        switch method {
        case .automatic: return isStikDebugInstalled || isTrollStoreInstalled || isJailbroken
        case .stikDebug, .trollStore, .jailbreak: return true
        case .builtIn: return false
        }
    }

    /// The one try at making the region without a debugger has been made (it is not repeated: the answer will not change).
    nonisolated(unsafe) static private var selfGrantTried = false

    /// Whether Husk has JIT now: a debugger marked it, or the region is already held. Before iOS 26 this tries, once, to make
    /// the region itself, which is all TrollStore and jailbroken devices need. Cheap after the first call, and logs nothing.
    static var ready: Bool {
        if debuggedFlag || prewarmed || isLive { return true }
        guard canGrantOwnJIT, !selfGrantTried else { return false }
        return prewarm()
    }

    /// What the LAYER measured about the guest's own arena: the verdict of the guarded
    /// write-then-execute it runs when the arena opens (`tl_xmem_exec_verdict`, aimed at the address
    /// the guest both writes and executes).
    ///
    /// This is what makes the indicator honest. `prewarmed` is a flag this app SET, and a flag can be
    /// true while the pages cannot be fetched — measured in `husk-20261015-085540`, where the app
    /// detached the debugger at the claim, the region's per-entry execute grant went with it (every
    /// entry then read `prot 1 max 3`), and the guarded fetch faulted — and the UI said "JIT is on"
    /// regardless. The layer already takes the one measurement that decides it; the UI now reports
    /// that rather than the flag.
    enum ExecuteVerdict: Equatable {
        /// No arena has been opened yet, so nothing has been measured. The UI must say it does not
        /// know yet, not that it works.
        case unmeasured
        /// The guarded write-then-execute RAN: the guest's arena genuinely executes.
        case ran
        /// It FAULTED: the arena names RWX but will not run (or cannot be written). `why` is the
        /// layer's own one-line reason, stage and all.
        case faulted(String)
    }

    static var executeVerdict: ExecuteVerdict {
        switch tl_xmem_exec_verdict() {
        case 1: return .ran
        case 2: return .faulted(tl_xmem_exec_detail().map { String(cString: $0) }
                                ?? "the guarded write-then-execute faulted")
        default: return .unmeasured
        }
    }

    /// The honest state the UI reports, and the only basis for "JIT is on".
    enum JITState: Equatable {
        /// Nothing claimed and no debugger: JIT is off.
        case off
        /// A debugger is attached and the region is held, but the layer has not exercised the
        /// guest's arena yet. This is the state a fresh launch or the StikDebug bounce leaves behind,
        /// and it is what the user's working flow shows: the grant is held; the arena is unproven.
        case granted
        /// The layer's guarded write-then-execute RAN: the guest's arena was measured executing.
        case verified
        /// The layer MEASURED the guest's arena unable to run. The indicator must not assert success
        /// here, whatever `prewarmed` says; `why` is what the layer knows.
        case unusable(String)
    }

    static var state: JITState {
        switch executeVerdict {
        case .ran: return .verified
        case .faulted(let why): return .unusable(why)
        case .unmeasured: return ready ? .granted : .off
        }
    }

    /// Whether a game can be started: a debugger is attached AND the layer has not measured the
    /// guest's arena unable to run. `ready` alone answers the first half; this fuses in the second.
    static var usable: Bool {
        guard ready else { return false }
        if case .unusable = state { return false }
        return true
    }

    /// One line for the UI: what the layer measured, said plainly.
    ///
    /// When nothing has been measured in this process, this is not "unknown": the layer wrote its last
    /// measurement to the container before the previous run could die (see `storedVerdict`), and that
    /// is what the row shows -- labelled as the last game's measurement, not this process's. The user
    /// must be able to read why the last run died without the app being alive to ask.
    static var measuredDescription: String {
        switch executeVerdict {
        case .ran: return "measured: executes (the guarded write-then-execute RAN)"
        case .faulted(let why): return "measured: will NOT execute — \(why)"
        case .unmeasured:
            guard let stored = storedVerdict() else {
                return "not yet exercised (the layer opens and measures the arena when a game starts)"
            }
            let when = stored.when.map { verdictDate.string(from: $0) } ?? "the last game start"
            if stored.ran { return "measured at the last game start (\(when)): executes" }
            let why = stored.reason ?? "the guarded write-then-execute faulted"
            return "measured at the last game start (\(when)): will NOT execute — \(why)"
        }
    }

    /// The arena-size setting, said honestly and all in one place: what is configured for the NEXT launch, what THIS
    /// launch actually claimed, whether that came up mapped and executable (the layer's own guarded write-then-execute,
    /// not this app's flag), and the session high-water mark of the region's use against the size it was measured
    /// against. It never shows the requested number as if it were the achieved one: if the claim failed it says so, and
    /// the peak is what tells the owner whether his setting matters for a title.
    static var arenaSizeDescription: String {
        let configured = "set to \(arenaMiB) MiB"
            + (arenaClamped ? " (clamped to the layer's honest \(arenaMinMiB)-\(arenaMaxMiB) MiB range)" : "")
            + " — takes effect at the next launch (the region is claimed at boot); this launch keeps what it claimed"
        let claimed = claimedMiB
        guard claimed > 0 else {
            return configured + "; nothing claimed yet (the region is taken at launch, before the guest download), so the "
                 + "size above is a request, not a fact"
        }
        var s = configured + "; this launch claimed \(claimed) MiB"
        switch executeVerdict {
        case .ran:
            s += ", and it came up mapped and executable (measured: the guarded write-then-execute RAN)"
        case .faulted(let why):
            s += ", but the guarded fetch FAULTED — it does NOT run (\(why)); the Guest arena row above is the evidence"
        case .unmeasured:
            s += ", not exercised yet (the layer measures it when a game starts)"
        }
        let sizeMiB = Int(tl_xmem_size() / (1024 * 1024))
        let peakMiB = Int(tl_xmem_used_high_water() / (1024 * 1024))
        if sizeMiB > 0 {
            s += "; this run's guest memory peaked at \(peakMiB) MiB of the \(sizeMiB) MiB arena it ran from "
               + "(the region's own high-water mark, from the layer)"
        }
        return s
    }

    // MARK: the verdict, read back from the container
    /// Where the layer writes the last arena verdict at measurement time (`tl_xmem_set_verdict_path`),
    /// and where it records why the run died if a guest fetch fault ends it. In the app's Documents, so
    /// it is readable on the next launch -- the whole point being that the verdict outlives the process.
    static var arenaVerdictURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("husk-arena-verdict.txt")
    }

    /// What the layer last wrote to the container: the measurement, and (when a guest fault ended the
    /// run) the reason it died. The file is the layer's own `husk-arena-verdict 1` key/value record,
    /// written atomically (`<path>.tmp` then rename), so this never reads a half-written file.
    struct StoredVerdict {
        let ran: Bool
        let reason: String?
        let when: Date?
        /// Why the last run died, if a guest fetch fault ended it (nil when it did not).
        let fault: String?
    }

    private static let verdictDate: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .short
        return f
    }()

    /// The last recorded verdict, or nil when the layer has never written one. A pure file read; safe
    /// from a view body (no probe, nothing executed).
    static func storedVerdict() -> StoredVerdict? {
        guard let text = try? String(contentsOf: arenaVerdictURL, encoding: .utf8) else { return nil }
        var ran = false, reason: String?, fault: String?, when: Date?, sawHeader = false
        for raw in text.split(separator: "\n") {
            let line = String(raw)
            if line.hasPrefix("husk-arena-verdict") { sawHeader = true }
            else if line.hasPrefix("verdict ") { ran = line.contains("ran") }
            else if line.hasPrefix("reason ") { reason = String(line.dropFirst("reason ".count)) }
            else if line.hasPrefix("fault ") { fault = String(line.dropFirst("fault ".count)) }
            else if line.hasPrefix("when ") { when = Double(line.dropFirst("when ".count)).map { Date(timeIntervalSince1970: $0) } }
        }
        guard sawHeader else { return nil }
        return StoredVerdict(ran: ran, reason: reason, when: when, fault: fault)
    }

    /// Why the last run died, in words, or nil when the last run did not die this way. Read by the app
    /// so a survived guest fault is explained without needing the log.
    static var lastRunFault: String? { storedVerdict()?.fault }

    /// True once this process has told StikDebug to let go.
    nonisolated(unsafe) static private(set) var detached = false

    /// Release the debugger, but only when nothing needs the grant — and only if Husk is the one that
    /// attached it.
    ///
    /// Three guards, and all of them matter. `grantHeldForRun` is the run scope: while a run can still
    /// execute guest code the grant is the only route the guest has to executable memory on this
    /// device (jailbreak: false, "can grant its own JIT": false, TXM enforced), so it is not given up.
    /// `keepDebuggerAttached` is the user's blunt override. And `debuggerIsExternalApp` is the one the
    /// user's working flow forced: the external StikDebug attach is the user's own, Husk cannot bring
    /// it back by itself, so it must never be ended — see the comment inside.
    private static func detachIfIdle() {
        guard prewarmed, !detached, !keepDebuggerAttached, !grantHeldForRun else { return }
        // Never send the detach to a debugger Husk did not start. The user's flow is "a quick redirect
        // to StikDebug and back to the app, and the app stays saying that jit is enabled" — StikDebug
        // attached throughout. Ending that session is what revokes the region's per-entry execute
        // grant (measured: every entry reads `prot 1 max 3` after it, and the next fetch faults), and
        // re-attaching takes the user's hands and a foreground bounce, which the app cannot do from
        // inside itself. So this is not "releasing a servicer that could stall us": it is giving away
        // the only route to executable memory there is here, permanently, for the rest of the process.
        guard !debuggerIsExternalApp else {
            HuskLog.log("jit", "no run is live, but the debugger is an external app the user attached "
                             + "(StikDebug or the like); leaving the session as it is -- Husk did not "
                             + "start it and cannot start it again, and ending it would revoke the "
                             + "region's execute grant with no way back")
            return
        }
        HuskLog.log("jit", "JIT region held and no run is live; releasing Husk's own JIT helper")
        husk_ios_jit_detach()
        detached = true
    }

    /// Whether this device enforces TXM, so that only a trap servicer can grant
    /// executable memory. Uses the same rule as the "TXM expected" line in the
    /// log banner, so the two can never disagree.
    static let deviceEnforcesTXM: Bool = HuskLog.expectsTXM(model: HuskLog.deviceModel)

    /// Why the last prewarm failed, for the UI to show.
    ///
    /// CS_DEBUGGED being set is not the same as the debugger servicing traps.
    /// A build running inside LiveContainer reports itself debugged, answers no
    /// brk, and gets no executable memory -- and Husk started QEMU anyway,
    /// which segfaulted inside qemu_init() with a perfectly healthy 4 GB of
    /// headroom. The crash looked like memory pressure and was nothing of the
    /// kind.
    nonisolated(unsafe) static var lastFailure: String?

    /// Whether a plain MAP_JIT mapping executes in this process.
    ///
    /// Not run on a TXM device: MAP_JIT memory cannot execute there, and the
    /// probe is not harmless. With a debugger attached, its fault goes to the
    /// debugger as a stop, not to the probe's signal guard. If StikDebug is not
    /// answering, that stop freezes the app. Views must never trigger this probe
    /// directly.
    static var mapJITWorks: Bool {
        if let known = mapJITResult { return known }
        let result: Bool
        if deviceEnforcesTXM {
            HuskLog.log("jit", "TXM device: MAP_JIT cannot execute here, not probing")
            result = false
        } else {
            result = husk_ios_jit_mapjit_works()
        }
        mapJITResult = result
        return result
    }

    /// The MAP_JIT answer if it has been worked out, without working it out.
    /// For display only: views must never trigger the probe.
    nonisolated(unsafe) static private(set) var mapJITResult: Bool?

    /// True only after a JIT region has been allocated AND passed the execute
    /// self-test — which happens inside `qemu_init`. It is therefore always false
    /// before the guest starts, and must NOT be used to decide whether to start it.
    /// Use `isDebuggerAttached` for that, and this afterwards to confirm it worked.
    static var isLive: Bool { husk_ios_jit_is_available() }

    /// The actual precondition for starting the guest: StikDebug has attached.
    ///
    /// Checked with csops/CS_DEBUGGED rather than the `brk #0x69` probe, because
    /// this costs nothing and is safe to call repeatedly — the brk probe traps
    /// every time, and every trap is a chance to hit a moment when StikDebug is
    /// not listening. The brk probe still runs once, inside the allocator, where
    /// its answer is immediately acted on.
    ///
    /// The last CS_DEBUGGED answer this process LOGGED, so the per-poll status line is not repeated.
    ///
    /// The flag is still READ on every call -- only the LOG is coalesced: the first reading, and any
    /// change of state, print the `csops status = 0x...` line; an unchanged repeat stays silent. Views
    /// poll this and the log was filling with the same line every poll, which buried the rest of the
    /// run. This changes only how often the line is repeated, never what `isDebuggerAttached` returns.
    nonisolated(unsafe) static private var lastLoggedDebugged: Bool?

    static var isDebuggerAttached: Bool {
        guard let flags = csStatus() else {
            if lastLoggedDebugged != false {
                lastLoggedDebugged = false
                HuskLog.log("jit", "csops failed (errno \(errno)); assuming no debugger")
            }
            return false
        }
        let attached = (flags & CS_DEBUGGED) != 0
        if lastLoggedDebugged != attached {
            lastLoggedDebugged = attached
            HuskLog.log("jit", String(format: "csops status = 0x%08x, CS_DEBUGGED = %@",
                                      flags, attached ? "set" : "clear"))
        }
        return attached
    }

    /// `isDebuggerAttached` without the log line, for polling while the
    /// built-in helper attaches.
    static var debuggedFlag: Bool {
        (csStatus() ?? 0) & CS_DEBUGGED != 0
    }

    private static func csStatus() -> UInt32? {
        var flags: UInt32 = 0
        let rc = withUnsafeMutableBytes(of: &flags) { buf in
            csops(getpid(), CS_OPS_STATUS, buf.baseAddress, buf.count)
        }
        return rc == 0 ? flags : nil
    }

    /// Whether StikDebug is installed -- asked of the URL scheme, and asked ONCE.
    ///
    /// `canOpenURL` emits its own system line every single time it is called
    /// (`-canOpenURL: failed for URL: "stikjit://" - error: "This app is not allowed to query for
    /// scheme stikjit"`), and views and the JIT resolver call this on every body refresh, which
    /// flooded the log with hundreds of identical lines at the end of the run. Whether an app is
    /// installed does not change within a process, so the probe runs once and the answer is cached;
    /// it is not re-asked per poll. Nothing about what the probe DECIDES changes -- only how often
    /// it is asked and logged.
    nonisolated(unsafe) static private var stikDebugInstalled: Bool?

    static var isStikDebugInstalled: Bool {
        if let known = stikDebugInstalled { return known }
        let found = URL(string: "stikjit://").map(UIApplication.shared.canOpenURL) ?? false
        stikDebugInstalled = found
        HuskLog.log("jit", "stikjit:// install probe (asked once): StikDebug "
                         + (found ? "is installed" : "is NOT reachable (not installed, or the "
                                  + "'stikjit' query scheme is missing from Info.plist) -- answer "
                                  + "cached for this process, not re-probed per poll"))
        return found
    }

    /// Whether TrollStore is on this device. It answers enable-jit on the `apple-magnifier` scheme, but that scheme is also the
    /// Magnifier app's, on every iPhone, so whether it opens says nothing. A Husk TrollStore installed knows from its marker file;
    /// otherwise SpringBoard is asked to open TrollStore, suspended, which an ordinary app is refused (9) when it exists and told
    /// it does not (7) when it does not. TrollStore installs on iOS 14.0 to 17.0 only.
    static let isTrollStoreInstalled: Bool = {
        if isInstalledWithTrollStore { return true }
        if #available(iOS 17.0.1, *) { return false }
        typealias Launch = @convention(c) (NSString, NSURL?, NSDictionary?, NSDictionary?, Bool) -> Int32
        guard let lib = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW),
              let sym = dlsym(lib, "SBSLaunchApplicationWithIdentifierAndURLAndLaunchOptions") else { return false }
        let launch = unsafeBitCast(sym, to: Launch.self)
        for id in ["com.opa334.TrollStore", "com.opa334.TrollStoreLite"] {
            let r = launch(id as NSString, nil, nil, nil, true)
            HuskLog.log("jit", "SpringBoard on opening \(id): \(r)")
            if r == 9 { return true }
        }
        return false
    }()

    /// Whether this copy of Husk was installed by TrollStore (or TrollStore Lite): it leaves a marker file next to the app in
    /// its bundle container (TrollStore's `TS_MARKER`). Only then is it a TrollStore app, which keeps the entitlements it was
    /// built with -- including the memory ones.
    static var isInstalledWithTrollStore: Bool {
        let container = Bundle.main.bundleURL.deletingLastPathComponent()
        return ["_TrollStore", "_TrollStoreLite"].contains {
            FileManager.default.fileExists(atPath: container.appendingPathComponent($0).path)
        }
    }

    /// A rootless jailbreak (Dopamine and its kin) puts its files in /var/jb and lets apps see it.
    static var isJailbroken: Bool { FileManager.default.fileExists(atPath: "/var/jb") }

    /// Whether Husk was already marked as debugged when it was opened, before it asked anything of anyone. That is what a
    /// jailbreak that allows JIT in apps does to every app it launches (Dopamine's "Allow JIT in Apps" setting), and it needs
    /// no hand-off at all. Read once, early (HuskApp.init), so a later attach is not mistaken for it.
    nonisolated(unsafe) static var debuggedAtLaunch = false
    static func noteLaunchState() { debuggedAtLaunch = (csStatus() ?? 0) & CS_DEBUGGED != 0 }

    /// Whether this iOS lets a process that is marked as debugged make executable memory for itself, as opposed to needing a
    /// debugger to hand it out: true before iOS 26, which is when the Trusted Execution Monitor arrived. It is what makes
    /// TrollStore and jailbreaks enough, without StikDebug.
    static var canGrantOwnJIT: Bool {
        if #available(iOS 26, *) { return false }
        return true
    }

    /// husk-jit.js as standard base64, which Built-in StikJIT's custom script takes.
    static var scriptBase64: String? {
        loadScript()?.data(using: .utf8)?.base64EncodedString()
    }

    /// Ask StikDebug to attach to us and run the JIT script.
    ///
    /// This backgrounds Husk — iOS switches to StikDebug, which attaches over the
    /// debugserver protocol, runs the script, and relaunches us. Everything after
    /// this point happens in a *new* foreground pass of the app.
    @MainActor
    static func requestTrollStoreAttach() -> Bool {
        HuskLog.log("jit", "requestTrollStoreAttach() -- handing off to TrollStore")
        guard let bundleID = Bundle.main.bundleIdentifier else { return false }
        
        let url = "apple-magnifier://enable-jit?bundle-id=\(bundleID)"
        guard isTrollStoreInstalled, let launchURL = URL(string: url) else {
            HuskLog.log("jit", "FAIL: TrollStore is not installed")
            return false
        }
        
        HuskLog.log("jit", "opening apple-magnifier:// for bundle \(bundleID)")
        UIApplication.shared.open(launchURL)
        return true
    }

    @MainActor
    static func requestAttach() -> Bool {
        HuskLog.log("jit", "requestAttach() -- handing off to StikDebug")

        guard let bundleID = Bundle.main.bundleIdentifier else {
            HuskLog.log("jit", "FAIL: no bundle identifier")
            return false
        }

        var url = "stikjit://enable-jit?bundle-id=\(bundleID)"

        // Sending the script inline makes Husk self-contained. StikDebug accepts
        // script-data unconditionally (HomeView.handleExternalURL), so we do not
        // depend on the user having assigned a script to Husk in StikDebug's UI.
        if let script = loadScript(), let encoded = base64URLEncode(script) {
            HuskLog.log("jit", "loaded husk-jit.js (\(script.count) chars, "
                             + "\(encoded.count) chars encoded); sending inline")
            url += "&script-data=\(encoded)"
        } else {
            HuskLog.log("jit", "WARNING: could not load husk-jit.js from the bundle. "
                             + "Falling back to whatever script StikDebug has assigned "
                             + "to this bundle id -- if none, JIT will not be enabled.")
        }

        guard let launchURL = URL(string: url) else {
            HuskLog.log("jit", "FAIL: malformed StikDebug URL (\(url.count) chars)")
            return false
        }

        guard UIApplication.shared.canOpenURL(launchURL) else {
            HuskLog.log("jit", "FAIL: cannot open stikjit:// -- StikDebug is not "
                             + "installed, or LSApplicationQueriesSchemes is missing "
                             + "the 'stikjit' entry in Info.plist")
            return false
        }

        HuskLog.log("jit", "opening stikjit:// for bundle \(bundleID); "
                         + "Husk will be backgrounded and relaunched after attach")
        UIApplication.shared.open(launchURL)
        return true
    }

    private static func loadScript() -> String? {
        // Accept either bundle layout: the build copies husk-jit.js to the bundle
        // root, but a folder-reference build phase would put it under Resources/.
        let candidates = [
            Bundle.main.path(forResource: "husk-jit", ofType: "js"),
            Bundle.main.path(forResource: "husk-jit", ofType: "js", inDirectory: "Resources"),
        ]
        for case let path? in candidates {
            if let s = try? String(contentsOfFile: path, encoding: .utf8) { return s }
        }
        return nil
    }

    /// StikDebug expects base64url (`-`/`_`, no padding) and percent-encodes it
    /// into the query string.
    private static func base64URLEncode(_ s: String) -> String? {
        guard let data = s.data(using: .utf8) else { return nil }
        let b64 = data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: CharacterSet(charactersIn: "="))
        return b64.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed)
    }
}
