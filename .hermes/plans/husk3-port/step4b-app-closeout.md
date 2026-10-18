# Step 4b — the app-side closeout the earlier merges deferred

SOURCE = `C:/Users/Aiden/Desktop/HuskApp` (our fork, git). TARGET = `C:/Users/Aiden/Desktop/husk3.0`
(upstream, **not a git repo — no git was run inside it**). Read first: `PLAN.md`, `step4-app-jit.md`,
`step4-engine-out.md`. This report finishes the three items those two left open.

**No compiler, no Mac, no iOS SDK here. zig cannot compile Swift. No build was run and none is claimed —
a macOS/device compile of husk3.0 is the only judgement of this work.** Correctness below is by
construction: every merged region re-read in place, comments/string literals stripped and `{}` `()` `[]`
balance checked per file, and each C symbol the new Swift calls located as a *definition* in the target.

Method, per the plan's own rule: diff `target` against `source` first so a `-` line is upstream's work and
a `+` line is ours, then take the **target as the base** and apply only our in-scope hunks. Direction was
re-measured per file (the brief has been wrong five times on this port).

## Per-file result

| file | result | what upstream's own work was, and how it was preserved |
|---|---|---|
| `src/app/Husk/GameStatus.swift` | **merged — ours taken, upstream untouched** | Ours 22,857 B vs upstream 3,967 B. The diff is **one `-` line**: upstream's doc-comment sentence `/// iPhone without anyone keeping a list.` is replaced by our continuation of the same sentence (`… Beside that result it now also keeps the *swap* verdict …`). Every other line of upstream's file is present unchanged; there is **no upstream-only content** to lose. Taken byte-identical to our fork (`cmp` after copy). This is where `NativeSession`, `GameStatusStore.SwapVerdict`/`Swap`/`notice`, `SwapCapability`, `SessionClose`, `extension TLNativeEngine { var swapID }` and the `SwapBadge`/`SwapCapabilityRow`/`SwapVerdictRow` marks live. |
| `src/app/Husk/TLUnityView.swift` | **merged — four hunks applied onto upstream's base** | Target is the base. Upstream's whole Unity `TouchScreenKeyboard` handler (`installUnityKeyboardHandler`, `unityMultiline`, the `husk_unity_text`/`insert_text`/`delete_backward`/`finish_text` calls, the `override func resignFirstResponder()`), upstream's Java document picker (`husk_java_set_document_picker`, `answerDocumentPicker`, `contentTypes`, `import UniformTypeIdentifiers`), `husk_native_set_extra_fonts(CJKFonts.directory.path)`, and the iOS-15 `.huskHideSystemOverlays()` / `.huskDefersSystemGestures(on: .all)` are **all kept** (12 upstream markers re-confirmed present after editing). Applied on top, verbatim from our fork: `checkedSwap`, the `verifyLoaded` block in `TLUnityModel.poll()`, `leave()`, and the three `dismiss()` → `leave()` call sites. |
| `src/app/Husk/ContentView.swift` | **merged — the one reported item added, nothing else touched** | ContentView was already the features worker's merge (mtime 22:10); it was diffed first and only the engine-removal worker's named alert was inserted. Upstream's `confirmAndroid` alert, the crash sheet, the updates alert, `AutoStart`, `GuestScreenView`, the log/share sheets and the guest/`huskHideSystemOverlays` surface are untouched. |

## What was applied from the two reports' deferred items

1. **`step4-app-jit.md` "Blocked / adapted" item 1 — the swap instrument's app side.** The three hunks it
   left out are now in `TLUnityView.swift`, ours verbatim, because `GameStatus.swift` (this pass) and the
   `husk_tl_*` layer symbols (already merged) now exist:
   - `TLUnityModel.checkedSwap` (line 787) and the once-per-launch verify in `poll()` (lines 803-808):
     after the engine has reached `HUSK_UNITY_RUNNING` and drawn a frame, `NativeSession.verifyLoaded()`
     writes the swap verdict against the app that is now running.
   - `TLCocosAttemptView.leave()` (lines 903-908): closes the session through `NativeSession.close(…)`,
     publishes `GameStatusStore.shared.notice` when the layer has something to say, then `dismiss()`.
   - The three `dismiss()` → `leave()` call sites: the `appClosedItself` receive, the
     `AppStoppedOverlay` onClose, and the bar's Close button.
2. **`step4-engine-out.md` "One wiring step still owned elsewhere: the named alert."** Applied in
   `ContentView.swift` immediately after the `confirmAndroid` alert, in upstream's own Binding-based
   alert idiom (the same shape as its `updates.available` alert): title **"Android is not in this build"**,
   message `runner.guestUnavailable ?? ""`, dismissed by clearing `runner.guestUnavailable`. This is
   `QemuRunner`'s `@Published var guestUnavailable` (QemuRunner.swift:20), set at lines 1290/1307 from
   `GuestRuntime.unbundledMessage` / `QemuEngine.absentMessage` — i.e. the payload is present but the
   engine that runs it is not, said by name rather than discovered inside `qemu_init()`.
3. **`step4-router`.** `TLCocosAttemptView` still resolves the guest-orientation `husk_java_orientation`
   callback; `HuskOrientation` itself is unchanged (target signature) and remains the step-4 merge's
   adapted form (no `app:` parameter); the app-name in the refusal log is still the `appLabel` fallback.

## C symbols the new Swift calls — every one is DEFINED in the target

Visibility: `HuskBridge.h` is the bridging header (`project.yml:90 SWIFT_OBJC_BRIDGING_HEADER`) and
includes `../../translation-layer-next/husk-tl-unity-app.h` (**line 148**) and
`../../translation-layer-next/husk-tl-xmem.h` (**line 153**), so the declarations below reach Swift.
Build: `../translation-layer-next` is a `sources` path (`project.yml:51`; only the `.h` is excluded), so
`husk-tl-unity-app.c` is compiled and linked into the app.

| symbol called by the new Swift | declared | defined |
|---|---|---|
| `HUSK_TL_ENGINE_*` (enum; the `swapID` mapping) | `husk-tl-unity-app.h:27-30` | n/a (enum) |
| `husk_native_loaded_apk(void)` | `husk-tl-unity-app.h:212` | `husk-tl-unity-app.c:846` |
| `husk_tl_swap_capability(int)` | `husk-tl-unity-app.h:41` | `husk-tl-unity-app.c:1119` |
| `husk_tl_session_close(void)` | `husk-tl-unity-app.h:52` | `husk-tl-unity-app.c:1139` |
| `husk_tl_swap_verdict(void)` | `husk-tl-unity-app.h:44` | `husk-tl-unity-app.c:1136` |
| `husk_tl_swap_status(void)` | `husk-tl-unity-app.h:57` | `husk-tl-unity-app.c:1137` |
| `husk_tl_verify_swap(const char*, const char*)` | `husk-tl-unity-app.h:65` | `husk-tl-unity-app.c:1372` |

`husk_tl_frames_live(int *epoch)` — **not called by the Swift**; it is the layer-side instrument
`husk_tl_verify_swap` uses internally (`husk-tl-unity-app.c:1400`, defined `:1452`, declared `:222`), which
is exactly the point (see below).

**The one symbol the earlier work could not place is now DEFINED:** `husk_native_set_arena_verdict_path`
(`husk-tl-unity-app.c:788`, declared `husk-tl-unity-app.h:163`). The app currently sets the verdict path
through `tl_xmem_set_verdict_path` (`husk-tl-xmem.c:1391`, declared `husk-tl-xmem.h:151`) — the step-4
workaround, and still valid and defined. Switching the call back to the layer's own wrapper is a one-line
change now available; it was **not** in this task's named hunks, so it was left as the step-4 merge set it.

**Declarations with no definition the target can link: NONE.**

## The swap verdict is keyed to the live session epoch

The bug the instrument exists to prevent was `liveness ok (1 frames)` reported from the swapped-OUT app's
quarantined heartbeat while the live app had presented 612 frames. The app side does not reintroduce that:

- `NativeSession.verifyLoaded()` reads `husk_tl_swap_verdict() >= 0` (is there a swap to verify at all) and
  then `husk_tl_verify_swap(apk, package)` — it never reads a frame count itself.
- `husk_tl_verify_swap` (`:1372`) proves all four checks and takes liveness from
  `husk_tl_frames_live(&ep)` (`:1400`), which is epoch-keyed: it returns 0 unless
  `g_live_frames.epoch == tl_guest_thread_epoch()` (`:1452-1458`). A count from before a session boundary
  is therefore *not* this app's, which is what kills the false pass. The verdict string names the epoch
  (`… since the swap; session epoch %d`).
- The only frame count the Swift reads at all is `husk_unity_frames()` in `poll()` (a raw per-engine
  counter) — and it is used **solely as the trigger** (`frames > 0`) for *when* to ask the layer, never as
  the verdict. The verdict is the layer's epoch-keyed answer.
- `NativeSession.close()` records only the layer's own verdict (a refusal `SWAP_NEEDS_RESTART` or a leak
  `SWAP_PARTIAL`) and deliberately records **nothing** for a clean close, because a clean close is not yet
  a swap — `verifyLoaded()` writes that down once a second app has loaded and the layer has verified it.

## iOS 15.5 — upstream's idioms, floor not raised

The target builds `deploymentTarget: iOS "15.5"`. Each line of the new Swift was checked for iOS-16-only
API:

- `GameStatus.swift`'s one such call, `LabeledContent("Another game")` in `SwapCapabilityRow`, resolves to
  **upstream's own `Compat15.swift` shim** (`Compat15.swift:11`, with
  `extension LabeledContent where Label == Text { init(_:content:) }` at `:39`) — the same idiom upstream
  uses in `AppDetailView.swift`. It compiles at 15.5 and upgrades to `SwiftUI.LabeledContent` under
  `if #available(iOS 16, *)` inside the shim.
- No `NavigationStack`, `presentationDetents`, `persistentSystemOverlays`, `defersSystemGestures`,
  `.gradient`, `ShareLink`, `TextField(axis:)` or the two-parameter `onChange` appear in the copied file
  or the added `TLUnityView` hunks (grep clean).
- `symbolRenderingMode(.palette)` / `foregroundStyle` are iOS 15-ready; `Binding(get:set:)`, `ToastView`,
  `Task { @MainActor … }`, `UserDefaults`/`FileManager` are all older than 15.5.
- The `TLUnityView` additions keep upstream's `.huskHideSystemOverlays()` / `.huskDefersSystemGestures(on:)`
  — ours' iOS-16 `.persistentSystemOverlays` / `.defersSystemGestures` were **not** taken.

## Verification, and what is NOT verified

- Structural checks that *were* run in the target after editing:
  - `{}`/`()`/`[]` balance = 0/0/0 on all three files with comments and string literals stripped
    (GameStatus 95/135/8 matching, TLUnityView 333/652/51, ContentView 196/353/10).
  - `GameStatus.swift` is byte-identical to our fork (`cmp`).
  - No duplicate declarations of the new types anywhere under `src/app/Husk` (`NativeSession`,
    `SwapCapability`, `SessionClose`, `swapID`, `SwapBadge`, `SwapCapabilityRow`, `SwapVerdictRow`,
    `SwapVerdict`) — one hit each.
  - Upstream content re-confirmed present by name after editing: `installUnityKeyboardHandler`,
    `husk_java_set_document_picker`, `answerDocumentPicker`, `husk_native_set_extra_fonts`,
    `huskHideSystemOverlays`, `huskDefersSystemGestures`, `unityMultiline` (12 markers); ContentView's
    `confirmAndroid` alert and every other modifier.
  - Each C symbol the new Swift calls located as a definition (table above); every such definition sits
    in a `sources` path, so it is in the app's link.
  - The only remaining bare `dismiss()` in `TLUnityView.swift` is the one inside `leave()` itself.
- **Not verified (and not claimed):**
  - **No build.** A mac/device compile of husk3.0 is the only proof of any of this; none was run.
  - **The notice has no renderer in the target.** Our `GameStatusStore.notice` is *set* by the swap code
    (the hunks applied here) but the target's `ContentView` (the features worker's merge) has neither the
    `@ObservedObject var statuses = GameStatusStore.shared` nor the `ToastView(toast: notice)` block our
    fork renders it with — both were `+` lines in the ContentView diff. Out of this task's scope (only the
    named alert was to be added), so the swap toast is currently written and silently dropped. **Flagged
    for the ContentView owner.**
  - `NativeSession`'s `@MainActor` calls (`husk_tl_session_close`, `GameStatusStore.recordSwap`) from
    `TLCocosAttemptView.leave()` rely on `View` members being main-actor-isolated, as ours' identical code
    did in our fork; not re-proven here.
  - The layer's own epoch/liveness behaviour was read, not exercised: `husk_tl_verify_swap`'s four checks
    and `husk_tl_frames_live`'s epoch comparison are argued from the source.
  - `husk_native_set_arena_verdict_path` was left unused (the app still calls `tl_xmem_set_verdict_path`);
    both are defined, but the wrapper is not exercised.
  - Nothing under `src/translation-layer*`, `src/ios-jit`, `project.yml`, `project.pbxproj`,
    `tools/`, `scripts/`, `.github/` was written. No git command was run inside `husk3.0`.
