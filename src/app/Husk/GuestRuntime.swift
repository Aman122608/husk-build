// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Whether this build can run the Android guest, and when it cannot, which piece
/// is missing -- in words someone can act on.
///
/// "The guest is absent" is not one condition. Three different things can be
/// gone, they fail in three places that look nothing alike, and only the app can
/// say which of them it is:
///
///   * **No UEFI firmware in the bundle.** QEMU cannot bring the guest up at all;
///     from inside QEMU this prints "failed to find romfile \"efi-virtio.rom\"" or
///     drops to a bare EFI Shell, neither of which names a build decision. This
///     is `HUSK_BUNDLE_GUEST=0` (scripts/build_ipa_prebuilt.sh).
///   * **No system disk in Documents.** The guest has simply not been downloaded
///     yet; GuestImage already reports this as `.missing`, and it is the user's
///     to fix by downloading.
///   * **The QEMU library itself absent.** This cannot happen: the app links
///     `libqemu-aarch64-softmmu.dylib` (its symbols are in HuskBridge.h), and the
///     same library also carries the JIT region (`husk_ios_jit_*`), so the app
///     would not launch without it.
///
/// Naming the first one is the point. It is a build choice, not a fault, and it
/// has to read as that rather than as a broken image, a broken download or a
/// broken QEMU.
enum GuestRuntime {
    /// The UEFI code volume the guest boots through. Its presence is the single
    /// file that says "this build ships the VM's boot firmware".
    static var firmwareBundled: Bool {
        FileManager.default.fileExists(
            atPath: Bundle.main.bundlePath + "/edk2-aarch64-code.fd")
    }

    /// The pc-bios directory QEMU loads option ROMs from (`-L`). A guest that
    /// boots needs `efi-virtio.rom` for its virtio-net device.
    static var romsBundled: Bool {
        FileManager.default.fileExists(
            atPath: Bundle.main.bundlePath + "/pc-bios/efi-virtio.rom")
    }

    /// Whether the guest's boot payload is in this build at all.
    static var bundled: Bool { firmwareBundled && romsBundled }

    /// What to say when it is not. One sentence, honest about whose decision it
    /// was and what it costs, and specific about what is missing.
    static let unbundledMessage =
        "This build ships without the Android runtime's firmware — the QEMU guest "
      + "is not bundled. Android apps cannot run here. Nothing on your device is "
      + "broken: it is a build choice."

    /// One line to the log, always, before QEMU is asked to open a file it may
    /// never have been given. Cheap, and it is the difference between "the guest
    /// said nothing" and "the guest was never here".
    static func logAvailability() {
        HuskLog.log("guest", "bundle: UEFI firmware=\(firmwareBundled ? "present" : "ABSENT") "
                          + "pc-bios=\(romsBundled ? "present" : "ABSENT") "
                          + "-> guest is \(bundled ? "runnable" : "UNBUNDLED")")
    }
}
