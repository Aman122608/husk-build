# Step 4 — the app's JIT / settings / diagnostics surface

SOURCE = `C:/Users/Aiden/Desktop/HuskApp` (our fork, git) → TARGET = `C:/Users/Aiden/Desktop/husk3.0`
(upstream, **not a git repo — no git was run inside it**). Commit records live in SOURCE only.

Method: `diff -u target source` per file first, so a `-` line is upstream's work and a `+` line is
ours; then, for every file, **the TARGET file is the base** and only our in-scope fixes are applied
on top of it. Where our copy was a strict superset of target with nothing of upstream's left to keep
(`JITBootstrap.swift`, `JITCard.swift`) the file was taken whole, after proving the target had no
unique content except lines our fix deliberately supersedes. Target-relative paths throughout.

**No compiler, no Mac, no iOS SDK here.** zig cannot compile Swift. Correctness is by construction:
each merged region re-read in place, comments/literals stripped and `{}` `()` `[]` balance = 0 on every
touched file, and a tree scan confirming every C symbol our Swift now calls has a definition in the
app's link set. **No build was run and none is claimed.**

## What changed in the target since this task's files were first read

The engine-removal worker landed while this ran, and it changes the picture for `HuskBridge.h`:

- `src/app/Husk/QemuEngineShim.c` (mtime 22:22) now **defines** `husk_qemu_engine_present()` and the
  whole QEMU/display/audio/snapshot/balloon bridge (lines 133–356).
- `src/app/Husk/QemuEngine.swift` (22:22) calls `husk_qemu_engine_present()` at `QemuEngine.present`.
- `src/app/project.yml` (22:22) now lists `../ios-jit/husk-ios-jit.c` and `../ios-jit/husk-brk.S` in the
  app target's `sources`, so the `husk_ios_jit_*` substrate is in the app's link.
- `src/translation-layer-next/husk-tl-xmem.c` (100,319 B, core worker) **defines** the app-facing arena
  API: `husk_ios_jit_region_size` (1335), `husk_ios_jit_exec_gate` (1516), `tl_xmem_exec_verdict`
  (1363), `tl_xmem_exec_detail` (1364), `tl_xmem_size` (1316), `tl_xmem_used_high_water` (1325),
  `tl_xmem_set_verdict_path` (1391).

This is why `HuskBridge.h` is the container header and every declaration in it now has a definition the
app links — see the table at the end.

## Per-file result

| file | result | what upstream's own work was, and how it was preserved |
|---|---|---|
| `src/app/Husk/HuskBridge.h` | **merged** | Target's `#include "../../translation-layer-next/husk-console-host.h"` (their PS4 console integration) is **kept**, and our `#include ".../husk-tl-xmem.h"` is added *after* it — the header ends with BOTH includes (line 149 and 153). Our two new app-facing JIT declarations were added to upstream's JIT block. The file's prose is our engine-reset wording (accurate now that QEMU is forwarded by `QemuEngineShim.c`), because the old "app's view of libqemu-aarch64-softmmu.dylib" line was no longer true. |
| `src/app/Husk/JITBootstrap.swift` | **merged** | Taken from ours (the fix work), then upstream's two newer pieces re-applied verbatim: upstream's `static let isTrollStoreInstalled` (marker file + suspended SpringBoard probe; upstream rewrote this because `canOpenURL("apple-magnifier://")` is the Magnifier app on every iPhone and says nothing) and upstream's matching `enableTStore` guard `guard isTrollStoreInstalled, let launchURL …`. Everything else upstream had in this file was a line our fix deliberately supersedes: `static let jitBytes = 512 MiB` → the setting; `detachIfDone()` → run-scoped `holdGrantForRun`/`detachIfIdle`; the plain `isStikDebugInstalled`/csops logging → the once-only probes (they flooded the log). |
| `src/app/Husk/JITSetup.swift` | **merged** | Target is the base; upstream's whole TrollStore auto-JIT feature is **kept** (`trollStoreAutoKey`, `trollStoreAuto`, the `enableAtLaunchIfAsked` block that asks TrollStore for JIT at launch, #135) — the target's `trollStoreAutoKey` is also referenced by `SettingsTab`, so dropping it would have broken that file. Our `reassertForRun()` (renew the attach before a run, decided from the free `CS_DEBUGGED` flag, never a `brk`) inserted before `resetDDI()`. |
| `src/app/Husk/JITCard.swift` | **merged** | Taken from ours. Upstream's only unique line, `private var isOn: Bool { JITBootstrap.ready }`, is exactly what the fix supersedes: `isOn` is now `JITBootstrap.usable` (false when the layer measured the arena unable to run), plus `measuredProblem` shown instead of "JIT is on". |
| `src/app/Husk/JITSetupView.swift` | **merged** | Target is the base; **upstream's iOS-15 idiom `.huskDestination(for: Step.self)` is kept** (ours used `.navigationDestination`, iOS 16 — it would not build on the 15.5 floor). Applied on top: `done: JITBootstrap.isTrollStoreInstalled` on TrollStore step 2, and our reworded "TrollStore was not found" `Label`. |
| `src/app/Husk/SettingsTab.swift` | **merged** | Target is the base. **All of upstream's newer settings kept**: the `FontSettings()`/CJK fonts page and `CJKFonts`, the `FAQTab()` row, the `AndroidSystemPage()` row with its "Android Emulator" header/footer, the whole `Remove Android` destructive section and `GuestImage.removeAll()`, the holiday line, and every `Color.…huskGradient` (the iOS-15 shim; ours used `.gradient`, iOS 16). Applied on top, inside the JIT section: `@State private var arenaMiB`, the **Execute grant** row, the **Guest arena** row (`measuredDescription`), the **arena-size `Stepper` (64–2048 MiB)** with its named log line, and the **Claimed & measured** row (`arenaSizeDescription`, including the session high-water mark), plus our corrected "Keep debugger attached" footer. |
| `src/app/Husk/TLUnityView.swift` | **merged (partial)** | Target is the base. **Upstream's whole Unity `TouchScreenKeyboard` handler is kept** (`installUnityKeyboardHandler`, `unityMultiline`, `resignFirstResponder`, the `husk_unity_*` text calls), as is upstream's **Java document picker** (`husk_java_set_document_picker`, `answerDocumentPicker`, `contentTypes`, `import UniformTypeIdentifiers`), `husk_native_set_extra_fonts(CJKFonts.directory.path)`, and the **iOS-15 `.huskHideSystemOverlays()` / `.huskDefersSystemGestures(on:)`** (ours used the iOS-16 spellings). Applied on top: the grant hold at launch (`holdGrantForRun` + `JITCoordinator.reassertForRun` when `!debuggedFlag`), the `==== new app ====` boundary marker, the arena-verdict-path call, the bounded host-touch / web-took-touch logging (`appLabel`, `touchesLogged`, `websTookTouch`), and the guest-orientation application. **Left in this file: `checkedSwap`/`leave()`/`NativeSession`** — see "Blocked" below. |
| `src/app/Husk/AppDiagnosis.swift` | **merged** | Target is the base. **Upstream's `.noJIT` kind, its `bolt.slash` icon, both noJIT diagnoses ("no StikDebug JIT region" and "QEMU holds its top / no room for") and upstream's `HuskShareLink` (iOS-15 shim; ours used `ShareLink`) are all kept.** Applied on top: our two new diagnoses — the layer-survived **"husk: GUEST FAULT:"** → "The game crashed in its JIT arena", and **"The app stopped after an error"** (read the app's own traceback before blaming anti-tamper). |
| `src/app/Husk/Compatibility.swift` | **left unchanged** | Target is strictly newer here: upstream's `send(…, attachLog:)`, `logTail(limit:)`, the `@AppStorage("husk.compatAttachLog")` toggle, the **`if #available(iOS 16, *)` `TextField`/`TextEditor` guard**, and `.huskDetents([.medium,.large])` (iOS-15 shim). Ours has nothing to add — every line ours differs by is a *removal* of that upstream work or an iOS-16 downgrade (`.presentationDetents`, unguarded `TextField(axis:)`). This file is also the source of the compat idiom the features worker copied for the log-note field. |

## Blocked / adapted (nothing papered over)

1. **The swap instrument's app side is not portable from this scope.** The instrument the brief names
   (`husk_tl_frames_live(int *epoch)`, `husk_tl_verify_swap`, `husk_tl_swap_capability`, the
   "liveness ok (N frames … since the swap; session epoch E)" verdict) lives in
   `src/translation-layer-next/husk-tl-unity-app.{c,h}` — which the target still has as **upstream's**
   copy (target 42,161 B / 12,806 B vs ours 90,879 B / 15,304 B; grep for those three names in the
   target: **no hits**). Its Swift side is `NativeSession` in **`GameStatus.swift`** (target: upstream's,
   no `NativeSession` at all; ours 22,857 B vs target 3,967 B), which is not in this task's scope.
   So `TLUnityView.leave()`, `checkedSwap` and `NativeSession.verifyLoaded()` were **left out** and
   upstream's `dismiss()` kept, rather than added as code that cannot compile. **This is the one named
   carry this task could not place**: it needs `husk-tl-unity-app.{c,h}` (owning worker) **and**
   `GameStatus.swift` merged first. With the layer merged, `TLUnityView`'s `leave()`/`checkedSwap`
   hunks are ours verbatim and need no adaptation.
2. **`husk_native_set_arena_verdict_path` is undefined in the target.** Our `TLUnityView` set the
   verdict path through that app-facing wrapper, which is defined in our `husk-tl-unity-app.c` (line
   773) — a file the target still has as upstream's copy, so the wrapper is absent (grep: no hits
   anywhere under `husk3.0/src`). Rather than call a symbol with no definition, the app now calls
   **`tl_xmem_set_verdict_path(...)` directly** — declared in the ported `husk-tl-xmem.h` and defined
   at `husk-tl-xmem.c:1391`. This is exactly the `app-visible-entry-points.md` rule ("if the value is a
   layer fact, define it in the layer"): the persisted verdict path is set from the layer, and the
   call links today. The upstream-named wrapper is reported, not added.
3. **`HuskOrientation.set(_:app:)` is absent in the target.** Target's `HuskApp.swift` has
   `@MainActor static func set(_ new: UIInterfaceOrientationMask, attempt: Int = 0)`; ours added an
   `app: String?` parameter (naming the game in the refusal log, retrying from every window). The
   guest-orientation fix **is applied** (`case 0/6/8/11 → .landscape`, `1/7/9/12 → .portrait`,
   `2/4/10/13 → standard`, `default: break`), adapted to the target's signature; the app-name in the
   refusal log needs `HuskApp.swift` (not in this scope). The `appLabel` property was added anyway so
   the adapted log line can name the game.
4. **`ContentView.swift` not touched** (not this task's file). The engine-removal worker reports a
   named-alert snippet it could not apply — the "payload is here but the engine that runs it is not"
   message has no consumer yet. **Reported here, not added.**

## iOS 15.5 guards — the compat idioms kept rather than the floor raised

The target builds `deploymentTarget: iOS 15.5`. Every iOS-16-only API our copy carried was **not**
ported; upstream's own idiom was kept instead:

| our copy (iOS 16) | target keeps (iOS 15, via `Compat15.swift`) | where |
|---|---|---|
| `.navigationDestination(for:)` | `.huskDestination(for:)` | `JITSetupView.swift` |
| `.persistentSystemOverlays(.hidden)` | `.huskHideSystemOverlays()` | `TLUnityView.swift` |
| `.defersSystemGestures(on:)` | `.huskDefersSystemGestures(on:)` | `TLUnityView.swift` |
| `.presentationDetents([.medium,.large])` | `.huskDetents([.medium,.large])` | `Compatibility.swift` (left) |
| `Color.…gradient` | `Color.…huskGradient` | `SettingsTab.swift` |
| `ShareLink` | `HuskShareLink` | `AppDiagnosis.swift` |
| unguarded `TextField(_:text:axis:)` / `.lineLimit(3...8)` | `if #available(iOS 16, *) { … } else { TextEditor }` | `Compatibility.swift` (left) |

New code added by this merge uses only iOS-15-or-earlier API: `Stepper` + `.onChange(of:)`,
`Task { @MainActor in … }`, string/`UserDefaults`/`FileManager` — all older than 15.5.

## `HuskBridge.h` — every declaration and whether the target defines it

Method: for each declaration, a definition was located in the target tree (file:line) and confirmed to
be in the app target's link, i.e. under `../translation-layer`, `../translation-layer-next`, `../ios-jit`
(both now `sources` in `project.yml`), `Husk/` (globs `Husk`, so `QemuEngineShim.c` is in), or a header.

| declaration (HuskBridge.h) | defined at | links? |
|---|---|---|
| `bool husk_qemu_engine_present(void)` | `src/app/Husk/QemuEngineShim.c:133` | **yes** |
| `void qemu_init(int, char**)` | `QemuEngineShim.c:138` (forwards to the dylib when present) | yes |
| `void qemu_main_loop(void)` | `QemuEngineShim.c:145` | yes |
| `void qemu_cleanup(void)` | `QemuEngineShim.c:152` | yes |
| `void husk_display_init(void)` | `QemuEngineShim.c:161` | yes |
| `bool husk_display_lock_frame(HuskFrameInfo*)` | `QemuEngineShim.c:168` | yes |
| `void husk_display_unlock_frame(void)` | `QemuEngineShim.c:175` | yes |
| `uint64_t husk_display_sequence(void)` | `QemuEngineShim.c:182` | yes |
| `void husk_display_send_pointer(int32_t,int32_t,bool)` | `QemuEngineShim.c:189` | yes |
| `bool husk_display_send_key(const char*,bool)` | `QemuEngineShim.c:197` | yes |
| `void husk_display_request_update(void)` | `QemuEngineShim.c:205` | yes |
| `void husk_ios_jit_install_trap_handler(void)` | `src/ios-jit/husk-ios-jit.c:218` (now in `project.yml`) | **yes** |
| `bool husk_ios_jit_prewarm(size_t)` | `husk-ios-jit.c:527` | yes |
| `size_t husk_ios_jit_region_size(void)` **(added)** | `husk-tl-xmem.c:1335` (+ weak `husk-ios-jit.c:560`) | **yes** |
| `bool husk_ios_jit_is_available(void)` | `husk-ios-jit.c:836` | yes |
| `bool husk_ios_jit_mapjit_works(void)` | `husk-ios-jit.c:902` | yes |
| `const char *husk_ios_jit_self_route(void)` | `husk-ios-jit.c:418` | yes |
| `void husk_ios_jit_detach(void)` | `husk-ios-jit.c:813` | yes |
| `bool husk_ios_jit_exec_gate(const char*)` **(added)** | `husk-tl-xmem.c:1516` (+ weak `husk-ios-jit.c:792`) | **yes** |
| `void husk_ios_jit_log_footprint(const char*)` | `husk-ios-jit.c:243` | yes |
| `size_t husk_ios_available_memory(void)` | `husk-ios-jit.c:270` | yes |
| `bool husk_display_gl_early(void)` | `QemuEngineShim.c:235` | yes |
| `bool husk_display_gl_create(void*,int32_t,int32_t)` | `QemuEngineShim.c:242` | yes |
| `bool husk_display_gl_probe(void)` | `QemuEngineShim.c:250` | yes |
| `bool husk_display_gl_bind(void)` | `QemuEngineShim.c:257` | yes |
| `uint64_t husk_display_gl_frames(void)` | `QemuEngineShim.c:264` | yes |
| `void husk_display_set_ui_size(int32_t,int32_t)` | `QemuEngineShim.c:212` | yes |
| `void husk_display_guest_size(int32_t*,int32_t*)` | `QemuEngineShim.c:220` | yes |
| `int32_t husk_audio_pull(int16_t*,int32_t)` | `QemuEngineShim.c:288` | yes |
| `bool husk_audio_active(void)` | `QemuEngineShim.c:302` | yes |
| `uint64_t husk_audio_frames_in(void)` | `QemuEngineShim.c:309` | yes |
| `uint64_t husk_audio_underruns(void)` | `QemuEngineShim.c:316` | yes |
| `void husk_display_gl_set_metal_presenter(husk_metal_present_fn)` | `QemuEngineShim.c:271` | yes |
| `bool husk_snapshot_load_at_startup(void)` | `QemuEngineShim.c:329` | yes |
| `void husk_snapshot_save(void (*)(bool,const char*))` | `QemuEngineShim.c:342` | yes |
| `void husk_balloon_set_bytes(int64_t)` | `QemuEngineShim.c:356` | yes |
| `#include "HuskRPPairing.h"` | header present | n/a |
| `#include "../../translation-layer/husk-tl.h"` | header present | n/a |
| `#include "../../translation-layer-next/husk-tl-unity-app.h"` | header present (upstream's) | n/a |
| `#include "../../translation-layer-next/husk-console-host.h"` | header present, **kept from upstream** | n/a |
| `#include "../../translation-layer-next/husk-tl-xmem.h"` | header present (ported, byte-identical to ours) | n/a |

**Declarations with no definition the target can link: NONE.**

The one *related* symbol that is not a `HuskBridge.h` declaration and **is** undefined is
**`husk_native_set_arena_verdict_path`** (declared in our `husk-tl-unity-app.h`, defined in our
`husk-tl-unity-app.c:773`; the target has neither because that file is still upstream's). It is
reported above and worked around by calling `tl_xmem_set_verdict_path` directly.

## Verification, and what is NOT verified

- **No compiler, no Mac, no iOS SDK.** zig is present but cannot compile Swift. No build of husk3.0 was
  run and none is claimed; a device/macOS build is the only judgement of this merge.
- Every touched file re-read after editing; after stripping comments and string literals, `{}`/`()`/`[]`
  balance is 0/0/0 on all eight merged files.
- Every C symbol the merged Swift now calls was scanned for a definition in the target tree and all hit
  (table above) — except the deliberately-not-called `husk_native_set_arena_verdict_path`.
- Upstream content preserved was checked by name in the target after editing: `TLUnityView` retains
  `installUnityKeyboardHandler`, `husk_java_set_document_picker`, `husk_native_set_extra_fonts`,
  `huskHideSystemOverlays`; `SettingsTab` retains `FontSettings()`, `FAQTab()`, `AndroidSystemPage()`,
  `huskGradient`, `trollStoreAutoKey`, `Remove Android`; `JITSetupView` retains `.huskDestination`;
  `AppDiagnosis` retains `.noJIT`/`HuskShareLink`; `JITSetup` retains `trollStoreAutoKey`.
- **Not measured**: that the merged files compile; that the arena `Stepper`'s bounds match what
  `husk-tl-xmem.c` actually honours (the 64–2048 MiB derivation is read from the layer's code, not
  exercised); that the persisted verdict file round-trips (the file format is the layer's
  `husk-arena-verdict 1` record, read by `storedVerdict()` — unrun); that the orientation switch's
  case numbers match `ActivityInfo` (copied from our working build, not re-derived).
- Nothing under `src/translation-layer*`, `src/ios-jit`, `QemuEngine*`, `GuestRuntime.swift`,
  `project.yml`, `tools/`, `scripts/`, `.github/`, `ContentView.swift`, or `GameStatus.swift` was
  written. No git command was run inside `husk3.0` (it is not a repo).
