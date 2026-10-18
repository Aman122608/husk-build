# Step 3g — DVM/JNI families, per-engine drivers, gamepad/unity, the old tree's scanner

Scope: `husk-tl-dvm.{c,android.c,javaapp.h}`, `husk-tl-jni.{c,h,hle.c,godot.c}`,
`husk-tl-godot.c`, `husk-tl-gamepad.{c,h}`, `husk-tl-unity.{c,h}` (all under
`src/translation-layer-next/`), and `src/translation-layer/husk-tl-scan.c`.

Method: `diff -u target source` per file, then a **two-way merge** — every one of these is a genuine
merge, not a copy, because 3.0 is a parallel branch that also advanced. Where our copy had nothing
upstream lacks, the merge is "restore upstream's work"; where both sides added, the file keeps both.
The four files withheld only for link order (`codewrite.c`, `bionic-{ndk,sys,cxx}.c`) were not touched
(another worker's). No `git` was run inside `husk3.0` (it is not a repo); upstream's pre-merge bytes for
the files this pass rewrote are parked at
`%LOCALAPPDATA%/Temp/husk3g/upstream_bak/` during the run (upstream is also fetchable at
raw.githubusercontent.com/Leviidev/Husk).

**No compiler, no Mac, no iOS SDK here — correctness by construction, no build claimed.**

## Per-file disposition

| file | verdict | upstream work preserved | ours added |
|---|---|---|---|
| `husk-tl-dvm.c` | **merged** (base = upstream; our three additions inserted) | ALL of it: `class_missing()` (:339), `throw_missing()` (:349), `name_start_ok()` (UTF-8 class names), the `mref.missing`/`fref.missing` plumbing and every `throw_missing` site, the `OutOfMemoryError` probe before `new-array` | the "no native" dedup logger in `call_native` (:1158), `report_libcore_natives()` (:2350) with its three per-library calls, and the whole session-close block (:2409–2507): `dex_dropped`, `tl_dvm_class_is_app` (:2422), `tl_dvm_app_dex_count` (:2428), `detach_app_class`, `reset_framework_statics`, `tl_dvm_end_app` (:2464), `tl_dvm_reattach_declared` (:2507) |
| `husk-tl-dvm-android.c` | **merged** | the system file picker: `tl_javaapp_documents_picked()` (:415), `N_pickDocuments` (:451), the `pickDocuments` native-table entry (:518), and the `cfg.pick_documents` call | our driver-thread handles (`app_thread`/`gl_thread_live`/`app_done`/`gl_done`), the input-queue logging, the GL-up/first-frame diagnostics, the `tl_renpy_prepare_if_app` call for a Ren'Py app on this path, `tl_dvm_reattach_declared()` at start (:1197), the per-session flag clear, and `tl_javaapp_request_exit` (:1232) / `tl_javaapp_thread_leaving` (:1237) / `tl_javaapp_reset` (:1280) |
| `husk-tl-dvm-javaapp.h` | **merged** | the `pick_documents` host hook (:34) and `tl_javaapp_documents_picked` (:53) | `tl_javaapp_request_exit` (:62), `tl_javaapp_thread_leaving` (:66), `tl_javaapp_reset` (:75) |
| `husk-tl-jni.c` | **merged** | `tl_jni_class_known()` (:227) — restored verbatim | `jni_FatalError` now runs `tl_guest_exit_unwind("JNIEnv::FatalError", SIGABRT)` instead of the host `abort()` |
| `husk-tl-jni.h` | **merged** | `tl_jni_class_known` declaration (:79) — kept | `tl_loop_quit_all`/`tl_loop_live_count`/`tl_loop_reset` (:139–141) |
| `husk-tl-jni-hle.c` | **merged** | `SystemClock.uptimeMillis/elapsedRealtime/elapsedRealtimeNanos` (:487–489), `SharedPreferences.getFloat/getLong` and `Editor.commit/putFloat/putLong/remove/clear`, the whole `FileOutputStream` family (:604–633), `File.setReadOnly`, and the `#include <fcntl.h>` they need | `tl_path_resolve`-based `file_resolved` for every `java.io.File` check (so a `/sdcard` path resolves as the native calls do), the `/sdcard/Android/obb/<pkg>` `getObbDir(s)` mapping, `tl_jni_hle_reset_onload()`, and `tl_hle_ext_files()` (:1177) / `tl_hle_package()` (:1180) |
| `husk-tl-jni-godot.c` | **merged → byte-identical to upstream** | the `GodotNativeBridge` HLE alias (#140, :430–438) and `AssetManager.setScreenOrientation` (:353) — restored | nothing (our copy was upstream minus those two; adding them back makes the files byte-equal) |
| `husk-tl-godot.c` | **merged → byte-identical to upstream** | forcing Godot 4 onto `opengl3`/`gl_compatibility` with the `TL_GODOT_VULKAN` opt-out (:97) — restored | nothing |
| `husk-tl-gamepad.c` / `.h` | **merged → byte-identical to upstream** | `tl_pad_read()` (`.c`:40, `.h`:46), used by upstream's `husk-console-host.c` — restored | nothing |
| `husk-tl-unity.c` / `.h` | **merged → byte-identical to upstream** | the whole `TouchScreenKeyboard` implementation (`.c`:191–348 — `showSoftInput`/`hideSoftInput`/`setSoftInputStr`, `tl_unity_insert_text`/`_delete_backward`/`_finish_text`/`_text_copy`, delivered as `nativeSetInputString`/`nativeSoftInputClosed`), its `keyboard_install()`/`keyboard_deliver()` hooks, and the declarations (`.h`:58–62) — restored | nothing |
| `src/translation-layer/husk-tl-scan.c` | **merged** | `starts_native_activity()` (:98–120) and its call at :381 (so Go's `libgojni` glue does not make an Ebitengine game a NativeActivity) — restored | our engine ordering: the Flutter test stays **last** (an engine that is definitely present outranks a launcher shell — the GTA SA DE fix) |

## The four cross-file declaration items

All four are **DONE** — every caller the neighbouring worker placed now has a declaration *and* a
definition in the target.

1. **`tl_javaapp_request_exit` / `_thread_leaving` / `_reset` — DONE.**
   Declared `husk-tl-dvm-javaapp.h:62,66,75`; defined `husk-tl-dvm-android.c:1232,1237,1280`.
   Callers satisfied: `husk-tl-dvm-jvm.c:22` (extern) calls it at `:38`; `husk-tl-dvm-natives.c:839,847`;
   `husk-tl-bionic.c:451-452` (extern); `husk-tl-unity-app.c:1212` calls `tl_javaapp_reset()`.
2. **`tl_dvm_end_app` / `tl_dvm_app_dex_count` / `tl_dvm_class_is_app` / `tl_dvm_reattach_declared` — DONE.**
   Defined `husk-tl-dvm.c:2464,2428,2422,2507`; declared by the already-ported `husk-tl-dvm.h:39-49`.
   Upstream's `class_missing`/`throw_missing` kept (`husk-tl-dvm.c:339,349`). Callers:
   `husk-tl-unity-app.c:63,1213`; `husk-tl-dvm-android.c:1197`.
3. **`tl_loop_quit_all` / `tl_loop_live_count` / `tl_loop_reset` — DONE, with upstream's `tl_jni_class_known` kept.**
   Declared `husk-tl-jni.h:139-141`; `tl_jni_class_known` kept at `husk-tl-jni.h:79` (also defined
   `husk-tl-jni.c:227` for upstream's `husk-tl-dvm.c` and `husk-console-host.c`). Loop definitions in the
   already-ported `husk-tl-jni-loop.c:405,421,435`. Caller: `husk-tl-unity-app.c:70-72,1252,1257,1293`.
4. **`tl_hle_ext_files` + `tl_hle_package` — DONE.**
   Defined `husk-tl-jni-hle.c:1177` and `:1180`. `tl_hle_ext_files`'s caller (`husk-tl-jni-gms.c:345,358`)
   declares it locally and now links; `tl_hle_package`'s caller declares it in `husk-tl-bionic-io.c:65`.

**Nothing is reported still missing for this pass's four items.** The one item from the neighbouring
worker's list that is *not* in this scope is item 7 (`tl_renpy_android_on` + `tl_zip_data_cached` /
`tl_load_*` in `husk-tl-bionic-io.c`) — that file is another worker's, untouched here.

## Verification run (and its limits)

- **Upstream-line preservation.** A script reduced each upstream pre-merge file to substantive lines and
  checked each is present in the merged file. Result: 0 unintended losses. The only upstream lines absent
  are deliberate supersessions where our newer fix rewrites the same statement in place:
  `jni_FatalError`'s `abort()`; the twelve `java.io.File` one-liners and `Context_getObbDir` (replaced by
  the resolved-path versions); the single-line `UnsatisfiedLinkError` and the single libcore-pending line
  in `husk-tl-dvm.c` (replaced by the dedup logger and `report_libcore_natives`); the old Flutter comment
  in `scan.c` (the code line is kept, relocated). Every upstream *symbol* is present.
- **Ours-line preservation** for `husk-tl-dvm.c`: the six ours-only lines absent are exactly the ones
  upstream's newer code supersedes (`class_missing`/`name_start_ok`/OOM-probe sites) — intentional.
- **Structural check** (comment/string-stripped) on all 13 files: `{}`, `()`, `[]` balances are 0 and
  `#if/#ifdef/#ifndef` depth returns to 0 with no stray `#endif`.
- **Caught by the checker:** the first draft of the `husk-tl-jni-godot.c` `GodotNativeBridge` block had
  `static const NB[] = …` (the `char` was dropped while retyping) — precisely the "copy it out of the
  binary, never retype it" hazard. Fixed to `static const char NB[]`; also restored the missing
  `AssetManager.setScreenOrientation` entry. Both re-checked.
- **Duplicate-definition check:** each of the declaration families resolves to exactly one definition in
  the target (grep over all `.c`/`.h`/`.m`).

## What could not be verified

- **No compile, no link, no run.** No `cc`/`clang`/`zig`/`tcc` on this box; the claims above are
  diff-driven and structural, not a build. A `zig cc -target aarch64-macos -c` syntax pass — where the
  bundled headers suffice — is the cheapest next check; the real proof is the app target's compile.
- `src/app/project.yml` / `project.pbxproj` wiring was not touched (later step, another worker).
- The `husk-tl-jni-hle.c` edit raised the tool's "modified by sibling subagent" note; the merged file was
  re-read and structurally checked, but if another worker is concurrently editing that same file the later
  writer wins — worth a re-diff before the build.
- `Atomic_int`/`atomic_fetch_add` in the new `husk-tl-dvm.c` code rely on `<stdatomic.h>`, which arrives
  transitively via `husk-tl-dvm-internal.h:7` (the same include our copy relied on) — consistent, but not
  compiler-confirmed.
