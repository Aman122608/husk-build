// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Whether this build has the QEMU engine, and what to say when it does not.
///
/// The engine — libqemu-aarch64-softmmu.dylib, 33.9 MiB raw and the largest
/// single entry left in the IPA — is no longer linked, embedded or shipped. What
/// used to make that impossible was that the JIT substrate was compiled into it,
/// so removing the engine removed the JIT. That split is undone: the JIT now
/// lives in the app itself (src/ios-jit/husk-ios-jit.c), and the native
/// translation layer keeps reaching it by name. See
/// .hermes/plans/engine-reset/qemu-dylib-removal.md.
///
/// Every engine symbol the app calls is defined by QemuEngineShim.c, which
/// forwards to the engine when one is present and otherwise does nothing and
/// says so by name. This is the question the app asks BEFORE it offers to start
/// a guest, so "the guest payload is here but the thing that runs it is not" is
/// read as that, rather than discovered inside a no-op `qemu_init()`.
///
/// `present` is asked once and cached (Swift statics are lazy): the answer
/// involves a dlopen() attempt, and it cannot change within a process.
enum QemuEngine {
    static let present: Bool = husk_qemu_engine_present()

    /// One sentence, honest about whose decision it was and what it costs. The
    /// wording mirrors GuestRuntime.unbundledMessage, because the two are the
    /// same kind of fact: a build choice, not a fault on the user's device.
    static let absentMessage =
        "This build ships without the QEMU engine — libqemu-aarch64-softmmu.dylib "
      + "is not bundled. Android apps cannot run here. Everything the native "
      + "translation layer runs (Unity, cocos2d-x, Godot, Java, Flutter, Minecraft) "
      + "is unaffected: it does not use the engine, and its JIT is built into the "
      + "app. Nothing on your device is broken: it is a build choice."

    /// One line to the log, always, before anything is asked of the engine.
    static func logAvailability() {
        HuskLog.log("qemu", "bundle: QEMU engine \(present ? "present" : "ABSENT") "
                          + "-> the Android guest is \(present ? "runnable" : "NOT runnable in this build")")
    }
}
