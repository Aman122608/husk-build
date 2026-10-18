# Step 4 — the QEMU-engine removal, ported into husk3.0

SOURCE = `C:/Users/Aiden/Desktop/HuskApp` (our fork, git). TARGET = `C:/Users/Aiden/Desktop/husk3.0`
(upstream, **not a git repo — no git was run inside it**). Read first: `PLAN.md`, `step1-tooling.md`,
`step2-features.md`, `step3-layer-core.md`, `step3-layer-rest.md`, `step3c-jit-two-layout.md`.

**No compiler, no Mac, no iOS SDK here. No build was run and none is claimed — a build is where this
gets judged.** Correctness below is by construction: every change re-read in place, brace/paren/bracket
balances checked on each touched source, YAML parsed, and the pbxproj structurally validated (balances,
section pairing, no referenced-but-undefined object id). See *What could NOT be verified*.

## What the removal is, and what makes it legal now

The QEMU engine (`libqemu-aarch64-softmmu.dylib`, 33.9 MiB raw / 6.7 MiB packed) is no longer linked and
no longer embedded. What made it impossible to drop before was that the JIT substrate was **compiled into
it** (`scripts/integrate_husk.sh`), so removing the library removed `husk_ios_jit_prewarm` /
`husk_ios_jit_get_mapping` — which the app and the native translation layer both need. That split is
undone: the JIT substrate's own home, `src/ios-jit/husk-ios-jit.c` + `husk-brk.S`, is now listed
explicitly in the app target. Every engine symbol the app still calls is **defined** by
`QemuEngineShim.c` as a forwarder through a `dlopen` handle (never `RTLD_DEFAULT`, which would find the
forwarder itself and recurse), and otherwise does nothing and says so **by name**. The decision is asked
**before a guest is offered**, so "the payload is here but the engine that runs it is not" reads as that
sentence, not as a `qemu_init()` that returns immediately.

## Files ported (byte-identical to our fork — `cmp` confirmed)

| file in `husk3.0/src/app/Husk/` | provenance | note |
|---|---|---|
| `QemuEngineShim.c` | ours-only, copied verbatim | 361 lines. The engine's front door: `husk_engine()` dlopens `@rpath/…`, `@executable_path/Frameworks/…`, bare name; `husk_engine_sym()` resolves through the handle; `husk_engine_absent()` reports once per process (`__atomic_exchange_n`). Defines `husk_qemu_engine_present()` + `qemu_init/qemu_main_loop/qemu_cleanup` + the display/GL/audio/snapshot/balloon forwarders. Uses an opaque `typedef struct HuskFrameInfo HuskFrameInfo;` so it does not include the bridging header. |
| `QemuEngine.swift` | ours-only, copied verbatim | 40 lines. `enum QemuEngine`: `present` (cached `husk_qemu_engine_present()`), `absentMessage`, `logAvailability()`. |
| `GuestRuntime.swift` | ours-only, copied verbatim | 63 lines. Scans the bundle for `edk2-aarch64-code.fd` and `pc-bios/efi-virtio.rom`; `bundled`, `unbundledMessage`, `logAvailability()`. |
| `QemuRunner.swift` | **merge — ours replaced theirs** | 98,083 B, byte-identical to our fork. |

### Why `QemuRunner.swift` is a replace and not a hand-merge

The whole diff against upstream is 181 lines and is *entirely* two things: the **Phase 0 Alpine guest +
the `Verbosity` enum** (which ours deletes) and the **engine-decision guards** (which ours adds). There is
**no upstream-only content** in the file beyond Phase 0/verbosity — I checked every `<` line in the diff
against that filter; nothing else of theirs exists to lose.

Its QEMU path is **preserved as a forwarding caller**: `phase1Arguments()` builds the same Android
LineageOS command line upstream has, and `qemu_init()` / `qemu_main_loop()` / `qemu_cleanup()` are called
unchanged — they now resolve through `QemuEngineShim.c`. What ours adds, in `run()` before anything else:

```swift
GuestRuntime.logAvailability()
guard GuestRuntime.bundled else { … guestUnavailable = GuestRuntime.unbundledMessage … return }

QemuEngine.logAvailability()
guard QemuEngine.present else { … guestUnavailable = QemuEngine.absentMessage … return }
```

plus `@Published var guestUnavailable: String?` and `let args = phase1Arguments()` unconditionally.

**This is not an optional flourish — the already-ported tooling demands it.** `scripts/build_ipa_prebuilt.sh`
(the step-1 port) says so in its own comments and code:
- line 126-127: *"project.yml's build phase reads the same HUSK_BUNDLE_GUEST and ships neither"* — so the
  build phase must be gated, or a `HUSK_BUNDLE_GUEST=0` build dies at the pre-build script requiring
  `guests/phase0` (which the same script **deliberately does not fetch** at line 201-206);
- line 198 and 202-205: *"no shipped path starts Phase 0 (see `QemuRunner.run()`)"* — so `QemuRunner` must
  be the version whose `profile` is gone.

So the Phase 0/verbosity removal is not scope creep: it is the half of the change the ported tooling
already assumes exists.

## `project.yml` — merged

Ours is the base (the JIT source list, the `HUSK_BUNDLE_GUEST` gating, the qemu dependency removed), with
**upstream's own entries re-added / kept**. YAML parses (`yaml.safe_load` → `iOS 15.5`, 6 dependencies,
8 source entries).

**Upstream entries preserved:**

1. **`deploymentTarget: iOS "15.5"`** — ours has `16.0`. I kept **15.5**: raising it is not the
   QEMU-engine removal, it would drop the iOS 15 / TrollStore / Dopamine devices upstream supports (#92),
   and the merged `ContentView.swift` still uses `Compat15.swift`'s `.huskDetents`. (Flagged, not applied.)
2. `sources`: `- path: Husk` (excludes `Resources/**`), `- path: Husk/Resources` (buildPhase resources),
   `- path: ../translation-layer`, `- path: ../translation-layer-next` (excludes `husk-tl-unity-app.h`),
   `- path: HuskJITHelper/HuskJITMessages.swift`, `- path: Shared` — all unchanged.
3. The whole Husk target `settings.base` block (bundle id, asset-catalog names, code sign, `Info.plist`,
   entitlements, `SWIFT_OBJC_BRIDGING_HEADER`, `LD_RUNPATH_SEARCH_PATHS`, `TARGETED_DEVICE_FAMILY`,
   `LIBRARY_SEARCH_PATHS`, `OTHER_LDFLAGS` incl. `-lhusk_rppairing`, Security, WebKit,
   VideoToolbox/CoreMedia/CoreVideo/Accelerate, ImageIO, `-lsqlite3`, `-lresolv`) — unchanged.
4. Dependencies: `HuskJITHelper`, `HuskDownloadsWidget`, `Frameworks/libFilePickerFix.dylib`,
   `libANGLE-shared.dylib` (`link:false`), `MoltenVK.xcframework` (`link:false`) — unchanged.
5. **`libhuskps4.dylib` (`embed:true, codeSign:true, link:false`) — RE-ADDED.** Ours does not have it
   (our fork has no PS4 support, so this entry does not exist in our file at all); without re-adding it the
   PS4 layer would silently stop being embedded, and the ported `package_ipa.sh` would fail its own
   `libhuskps4.dylib embedded` check (step 1 re-added that check to the loop for exactly this reason).
6. `preBuildScripts: "Stage guest image"` — the phase name and its body are kept **inside** the new gate:
   the pc-bios ROM copy, the `edk2-aarch64-code.fd` bunzip2, the two `qemu-img create` qcow2 seeds, the
   `android-system` rsync and the `java-runtime` rsync. `basedOnDependencyAnalysis: false` kept. Only the
   Phase 0 kernel/initramfs copy and the unconditional `guests/phase0` requirement are gone (see above),
   and `outputFiles:` is dropped because the phase no longer always produces those guest files.
7. `postBuildScripts: "Embed StikJIT"` — unchanged.
8. `HuskJITHelper` (app-extension, `deploymentTarget 26.0`, StikJIT.xcframework) and
   `HuskDownloadsWidget` (app-extension, `deploymentTarget 16.1`) targets — unchanged.

## `project.pbxproj` — merged (and kept in step with `project.yml`)

This is a *maintained* file (the other workers added their merged sources to it), so it was edited by hand
to match the merged `project.yml`; `package_ipa.sh` also regenerates it with `xcodegen generate` when that
is installed (`build_ipa_prebuilt.sh` does so unconditionally at line 215), so both paths agree.

Object counts, upstream → now: `PBXBuildFile` 190 → 193, `PBXFileReference` 236 → 240, `PBXGroup` 13 → 14;
`PBXNativeTarget` 3, `PBXSourcesBuildPhase` 3, `XCBuildConfiguration` 8 — **unchanged**. `libqemu` now
occurs **0 times**.

**Removed (the QEMU engine, 6 objects/entries):**

- `PBXBuildFile` `38E7D6B6947ED3CF1A82BB7D` (`… in Frameworks`) and `EC8B11F723C188F32B5D9DCA`
  (`… in Embed Frameworks`);
- the `Embed Frameworks` copy-phase child and the Husk `Frameworks` build-phase child;
- `PBXFileReference` `1B29247002CE43A3B9DEC633` and its `Frameworks` group child.

**Added (5 files, 2 of them the JIT substrate):**

| file | group | Sources phase |
|---|---|---|
| `../ios-jit/husk-ios-jit.c` (id `A1B2C3D4E5F60718293A4B5C`) | new `ios-jit` group (id `E5F60718293A4B5C6D7E8F90`, `path = "ios-jit"`, child of the `src` group) | yes |
| `../ios-jit/husk-brk.S` (id `B2C3D4E5F60718293A4B5C6D`, `sourcecode.asm`) | same | yes |
| `QemuEngineShim.c` (id `F60718293A4B5C6D7E8F90A1`) | `Husk` group | yes |
| `QemuEngine.swift` (id `E8F90A1B2C3D4E5F60718293`) | `Husk` group | yes |
| `GuestRuntime.swift` (id `A1B2C3D4E5F60718293A4B5D`) | `Husk` group | yes |

**Kept in step with `project.yml`:** the `Stage guest image` `shellScript` was replaced with the escaped
form of the merged `project.yml` body (the escaper was validated by round-tripping the untouched
`Embed StikJIT` script against its existing escaped form in the same file — exact match), and that phase's
`outputPaths` emptied (it no longer always produces the guest files).

**Upstream entries preserved in `project.pbxproj` — every one they own, untouched:** all 184 remaining
`PBXBuildFile`/`PBXFileReference` pairs and every group. Specifically the ones this pass could have
dropped and did not:

- **PS4 / console:** `libhuskps4.dylib` (Embed Frameworks child + FileReference + `Frameworks` group
  child), `PS4Library.swift`, `PS4Screen.swift`, `ConsoleUI.swift` (`Husk` group + Sources phase),
  `husk-console-host.c` (+ `husk-console-host.h`, `husk-console-abi.h`) and `husk-jit-arena.c`/`.h`
  (the `translation-layer-next` group + Sources phase);
- **iOS 15.5 compatibility:** `Compat15.swift` (group + Sources phase) and
  `IPHONEOS_DEPLOYMENT_TARGET = 15.5` in **both** project build configurations (Debug + Release);
- all 3 native targets and their `buildPhases` (Stage guest image, Sources, Resources, Frameworks, Embed
  Foundation Extensions, Embed Frameworks, Embed StikJIT), the 2 `PBXTargetDependency`s, and all 4
  `XCConfigurationList`s.

The only upstream lines not carried verbatim are the 6 QEMU lines above, the old `Stage guest image`
`shellScript` line, and the 5 guest `outputPaths` — and the guest copy code itself lives on inside the new
gated script.

## `HuskBridge.h` — REPORTED, not edited (another worker owns this header)

The target's `HuskBridge.h` declares the engine's whole API. **Exactly one declaration my ported files need
is missing from it:**

```c
/* --- QEMU's own public API ---
 * Defined by QemuEngineShim.c: a forwarder to the engine when it is present; a named no-op when it is not.
 * Ask husk_qemu_engine_present() before offering to start a guest -- never discover it inside qemu_init(). */
bool husk_qemu_engine_present(void);
```

It is the **only** symbol `QemuEngine.swift` (and nothing else of mine) calls; without it the app target
fails to compile at `QemuEngine.present`. Every other C symbol the three ported files call
(`qemu_init`/`qemu_main_loop`/`qemu_cleanup`, `husk_display_*`, `husk_display_gl_*`, `husk_audio_*`,
`husk_snapshot_*`, `husk_balloon_set_bytes`, `husk_ios_available_memory`) is **already declared** in the
target's header, and `QemuEngineShim.c` **defines** all of them — signatures checked against the header,
no mismatch.

Two more things the header will need, which are **not mine** but are directly coupled and easy to miss
because our fork's header replaces the console include instead of adding to it:

```c
/* keep upstream's PS4 include -- do NOT drop it when taking our header */
#include "../../translation-layer-next/husk-console-host.h"

/* our fork's extra JIT declarations (for the src/ios-jit worker's Swift) */
size_t husk_ios_jit_region_size(void);
bool   husk_ios_jit_exec_gate(const char *after);

/* and our xmem verdict include, which ours has where upstream has the console include */
#include "../../translation-layer-next/husk-tl-xmem.h"
```

The trap: our fork's `HuskBridge.h` **replaces** `#include "husk-console-host.h"` with
`#include "husk-tl-xmem.h"` (our fork has no console). Upstream's header must end up with **both** — dropping
the console include would break `PS4Library.swift`/`ConsoleUI.swift`. I did not edit the file.

## One wiring step still owned elsewhere: the named alert

`QemuRunner.run()` publishes `guestUnavailable`, but the target's `ContentView.swift` (the other worker's
merge) has no consumer, so the sentence is logged but not *shown*. I did not edit it, per the ownership
rule. The minimal addition, matching the target's existing `.alert` style:

```swift
// beside the existing confirmAndroid alert, on the guest/launcher surface
.alert("Android is not in this build", isPresented: Binding(
        get: { runner.guestUnavailable != nil },
        set: { if !$0 { runner.guestUnavailable = nil } })) {
    Button("OK", role: .cancel) { runner.guestUnavailable = nil }
} message: {
    Text(runner.guestUnavailable ?? "")
}
```

(`Launcher.swift` and `Downloads.swift` need nothing: `Downloads.swift` is byte-identical between the two
trees, and neither references the engine. Note also that `ServeFolder.swift` and `SendLogToPC.swift` are
still absent from the target's `project.pbxproj` — the step-2 worker's files, not mine.)

## Does `scripts/package_ipa.sh`'s QEMU-absent guard now pass?

**Yes, structurally.** The guard is:

```sh
if [ -f "$APP/Frameworks/libqemu-aarch64-softmmu.dylib" ]; then  # fatal, rc=1
```

The dylib reaches `Frameworks/` only via a framework dependency with `embed: true`, and there is no longer
any such dependency (`project.yml`) nor any `Embed Frameworks` child / `PBXFileReference` for it
(`project.pbxproj`) — so nothing copies it, and the guard takes its `else` branch ("qemu engine not
embedded"). The ported `build_ipa_prebuilt.sh` also no longer stages it (line 78-88), so the file is not
even present to be picked up. Likewise the guest-payload checks are behind `HUSK_BUNDLE_GUEST=1` and the
build phase now honors the same flag. **But no build was run: the guard passing is an argument from the
project files, not a measurement.**

`package_ipa.sh` itself was **not** touched (it was already merged in step 1); its
`libhuskps4.dylib` required-embedded check is satisfied by the preserved PS4 dependency.

## What could NOT be verified

- **No compiler, no Mac, no iOS SDK.** No build, no `xcodegen generate`, no `package_ipa.sh` run, no IPA.
  **A compile on device is where this gets judged.**
- Structural checks that *were* run: `cmp` byte-identity for the four ported files; comment/string-stripped
  brace/paren/bracket balances of 0 on `QemuEngineShim.c`, `QemuEngine.swift`, `GuestRuntime.swift`,
  `QemuRunner.swift`; shim-vs-header signature comparison (no mismatch); `yaml.safe_load` on `project.yml`;
  and on `project.pbxproj` — balances 0, all 12 `Begin/End` sections paired, all object ids referenced from
  `files`/`children` lists defined, and both `shellScript` values round-tripping to their `project.yml`
  bodies.
- One write bug was caught and fixed during this pass: Python text-mode writes had converted the whole
  `project.pbxproj` to CRLF (the tree is LF-only) and a first substitution had dropped the `;` after the
  new `shellScript` value. Both corrected; the file is LF-only and the semicolon is present.
- `HUSK_BUNDLE_GUEST=1` (a guest-bundling build) was **not** exercised, and `fetch_phase0_guest.sh` was not
  run: the Phase 0 kernel/initramfs copy is gone from the stage script, which is fine for the default
  build but means a `HUSK_BUNDLE_GUEST=1` IPA no longer ships `vmlinuz-virt`/`initramfs-virt` (nothing
  starts Phase 0, so nothing reads them).
- The deployment target was left at upstream's **15.5**, not our fork's 16.0 — deliberate (see above), but
  it is a divergence from "the way our fork has it" that someone may want to revisit.
- `HuskBridge.h` is unedited, so the target **does not compile yet** until the one
  `husk_qemu_engine_present` declaration lands. That is a hard dependency of this pass, stated plainly.
