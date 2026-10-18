# Step 3e — `husk-tl-unity-app.{c,h}` (+ `husk-tl-audio.h`): the two-way merge

SOURCE = `C:/Users/Aiden/Desktop/HuskApp` (our fork, git). TARGET = `C:/Users/Aiden/Desktop/husk3.0`
(upstream, **not a git repo — no git was run inside it**). Commit records live in SOURCE only.

This is the riskiest merge in the port and it is **genuinely two-way**: 3.0 is a parallel branch that also
advanced, and its console/picker/keyboard/extra-fonts work landed in this very file. Method: **ours is the
BASE** (it carries every session/swap symbol in the brief), and each upstream-only construct was then
re-inserted by hand, diff-driven and symbol by symbol.

**Target's pre-merge bytes were confirmed to be upstream `main` verbatim** — fetched
`raw.githubusercontent.com/Leviidev/Husk/main/src/translation-layer-next/husk-tl-unity-app.{c,h}` and
`cmp`-identical to the target's copies (C `4d5fd9c62a8a922bb3bdc5fdce4d27ba`, H
`3076780ba01784fc18bfa5c61c8bddaf`; both 42,161 B / 12,806 B). Parked for recovery at
`.husk3-merge/upstream/husk-tl-unity-app.{c,h}` alongside the ld/xmem bytes already there.

| file | base (ours) | target (upstream) | merged |
|---|---|---|---|
| `husk-tl-unity-app.c` | 1,552 lines / 90,879 B | 742 / 42,161 B | **1,611 lines / 93,120 B** |
| `husk-tl-unity-app.h` | 220 / 15,304 B | 183 / 12,806 B | **233 / 16,465 B** |
| `husk-tl-audio.h` | 1,924 B | 848 B | **1,924 B (ours, a superset)** |

## Compile check — this one HAS a compiler

zig 0.13.0 is present (`AppData/Local/Temp/zigdl/...`). The merged file **compiles**:

```
zig cc -target aarch64-macos -Isrc/translation-layer-next -Isrc/translation-layer \
    -Wall -Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
    -c src/translation-layer-next/husk-tl-unity-app.c -o merged_unity.o
# exit 0; 192,856-byte object; the only diagnostic is a benign _DARWIN_C_SOURCE redefinition
# caused by passing -D on the command line (not present in the source).
```

`-Werror=implicit-function-declaration` is the meaningful gate here: it proves **every** symbol the file
calls is declared somewhere it includes, including the externs this merge had to add (below). No build of
husk3.0 as a whole was run and none is claimed.

## The brief's upstream-only list, item by item

Each verified absent from our fork before this merge (`grep` across our whole `src/`: **0** hits), and each
verified present in the target afterwards.

| upstream-only item | verdict | where in the merged files |
|---|---|---|
| `#include "husk-console-host.h"` | **INTEGRATED** | `husk-tl-unity-app.c:4` |
| `husk_console_loaded_game` (the console game holds the JIT region too) | **INTEGRATED** | used by `husk_native_loaded_apk()` at `husk-tl-unity-app.c:846` |
| `husk_java_set_document_picker` (MIME document picker) | **INTEGRATED** | `.c:747`, declared `.h:132` |
| `husk_java_documents_picked` | **INTEGRATED** | `.c:748`, declared `.h:133` |
| `g_java_host.pick_documents` field + the `tl_javaapp_config` assignment | **INTEGRATED** | field `.c:88`; assign `.c:577` |
| `husk_native_set_extra_fonts` | **INTEGRATED** | `.c:780`, declared `.h:154` |
| `tl_set_extra_fonts_dir` (local declaration upstream relies on) | **INTEGRATED** | `.c:779` (definition is in `husk-tl-bionic-io.c:165`, already in the target) |
| `husk_unity_set_keyboard_handler` | **INTEGRATED** | `.c:824`, declared `.h:181` |
| `husk_unity_insert_text` / `_delete_backward` / `_finish_text` / `_text` | **INTEGRATED** | `.c:826-829`, declared `.h:182-185` |
| `unity_running()` (the Unity-engine state guard the four above use) | **INTEGRATED** | `.c:825` |
| upstream's PS4/console integration *in general* | **INTEGRATED for this file** | only two touch points exist in this file; the console host itself (`husk-console-host.{c,h}`, `husk-console-abi.h`, `husk-jit-arena.{c,h}`) is **untouched** and keeps working. `ContentView.swift:356` calls `husk_console_loaded_game()` and now finds the same fallback it had upstream. |

### Reconciled (both sides changed the same place)

| region | our form (kept) | upstream's form (kept as well) | resolution |
|---|---|---|---|
| `husk_native_loaded_apk()` | ours returned `NULL` when `HUSK_UNITY_IDLE` | upstream returns `husk_console_loaded_game()` | **RECONCILED**: upstream's console fallback is the body; when idle with no console game it still returns NULL, so our `husk_tl_verify_swap` identity check (which runs while a session is live, `state != IDLE` → `A.apk`) is unaffected. `GameStatus.isOpen`'s `husk_native_loaded_apk() != nil` still means "an app is loaded", console included. |
| `engine_frames()` | ours split it into `engine_frames_for(int)` + a one-line wrapper | upstream had the single-expression body on `A.engine` | **RECONCILED**: `engine_frames_for(int)` is ours; `engine_frames()` remains as `engine_frames_for(A.engine)`, byte-identical behaviour to upstream. The ledger needs the by-name form. |
| `husk_unity_state()` / heartbeat ENDED promotion | ours uses CAS `RUNNING → ENDED` | upstream used a blind `atomic_store` | **RECONCILED toward ours** — a blind store would clobber a session close's `STOPPING` marker or re-close the gate it just opened. This is required by the swap machinery; upstream's line is a strict subset of ours. |

### Deliberately NOT carried across (ours is the newer form; nothing of upstream's *behaviour* is lost)

| upstream line(s) | why left |
|---|---|
| the old `on_fatal` / `guest_exit` / heartbeat / launcher / `java_boot` / "this thread stays asleep" comments | comments only; ours is the newer text and carries the guest-fault and session-boundary rationale |
| `guest_exit()`'s `native: the game exited` + blind `atomic_store(ENDED)` | superseded by ours, which ends the app not Husk, calls `tl_javaapp_request_exit`/`_thread_leaving`, and promotes to ENDED by CAS unless a close owns the state |
| heartbeat `unsigned long f = engine_frames(); … last = f;` and the two `unity: alive:` lines with `f - last` | superseded by the **live-session** read and the clamped delta — this is the exact regress the brief warns about (the old `f - last` also went negative across a close). See "swap instrument" below. |
| heartbeat `if (state == ENDED ‖ FAILED) return NULL;` | widened by ours to also stop on `STOPPING` and `IDLE` (the close's own markers) |
| `husk_unity_touch`'s bare `if (state != RUNNING) return;` | superseded by ours, which logs the first few dropped/forwarded touches (bounded by `g_touch_notes`) |

Nothing else in upstream's file is unreferenced by the merge: every upstream *code* line not listed above
survives verbatim, because those regions are byte-identical between the two trees.

## Every ours-only symbol, now DEFINED in the target

In `husk3.0/src/translation-layer-next/husk-tl-unity-app.c`:

| symbol | line |
|---|---|
| `write_view_pointer` | 151 |
| `fault_address_region` | 189 |
| `guest_arena_fault` (the guest-fault-ends-the-session rule) | 232 |
| `husk_native_set_arena_verdict_path` | 788 |
| `tl_perf_summary` (the hot-path counter summary) | 988 |
| `ss_probe` (the swap-stall probe: lock ledger + thread census) | 1013 |
| `tl_swapstall_tick` | 1099 |
| `husk_tl_swap_capability` | 1119 |
| `husk_tl_swap_verdict` | 1136 |
| `husk_tl_swap_status` | 1137 |
| `husk_tl_session_close` | 1139 |
| `husk_tl_verify_swap` (the three checks + accounting) | 1372 |
| `engine_frames_for` | 1437 |
| `engine_frames` (wrapper) | 1441 |
| `live_frames_mark` (the live-session frame ledger's mark) | 1447 |
| `husk_tl_frames_live` | 1452 |

Declared in `husk3.0/src/translation-layer-next/husk-tl-unity-app.h`:
`HUSK_UNITY_STOPPING` (22), the `HUSK_TL_ENGINE_*` enum (28-30), the `SWAP_*` enum (38),
`husk_tl_swap_capability` (41), `husk_tl_swap_verdict` (44), `husk_tl_session_close` (52),
`husk_tl_swap_status` (57), `husk_tl_verify_swap` (65), `husk_native_set_arena_verdict_path` (163),
`husk_tl_frames_live` (222).

## Header ↔ definitions ↔ already-merged Swift, made to agree

The already-merged Swift in the target calls these C entry points; each now has a declaration in the header
**and** a definition the link can reach:

| Swift call site | C declaration | C definition |
|---|---|---|
| `husk_console_loaded_game()` — `ContentView.swift:356` | `husk-console-host.h` (upstream, untouched) | `husk-console-host.c:288` |
| `husk_unity_set_keyboard_handler{…}` — `TLUnityView.swift:657` | `husk-tl-unity-app.h:181` | merged `.c:824` |
| `husk_java_set_document_picker{…}` — `TLUnityView.swift:559` | `.h:132` | merged `.c:747` |
| `husk_native_set_extra_fonts(…)` — `TLUnityView.swift:329` | `.h:154` | merged `.c:780` |
| `tl_xmem_set_verdict_path(…)` — `TLUnityView.swift:324` (the workaround for the absent wrapper) | `husk-tl-xmem.h:151` | `husk-tl-xmem.c` |

`husk_native_set_arena_verdict_path` — the wrapper the previous worker had to call *around* — **now exists
in both places again** (`.h:163`, `.c:788`), so when `GameStatus.swift`/`NativeSession` is merged the app
can call the upstream-named wrapper; the direct `tl_xmem_set_verdict_path` workaround remains valid either
way.

The swap entry points (`husk_tl_frames_live`, `husk_tl_verify_swap`, `husk_tl_swap_capability`,
`husk_tl_session_close`, `husk_tl_swap_status`, `husk_tl_swap_verdict`) are declared in the header and
defined here. **No Swift in the target calls them yet** — `GameStatus.swift` is still upstream's 99-line
copy and `TLUnityView.leave()/checkedSwap/NativeSession` were deliberately left out by the app worker
(step4-app-jit.md "Blocked" #1). They are in place for the moment that file lands; the header/definition
pair agrees now.

## Swap instrument — no regression of the "1 frames from the swapped-out app" bug

The lesson the instrument exists for: its liveness verdict once read `ok (1 frames)` from the swapped-OUT
app's quarantined heartbeat while the app on screen had presented 612. Every frame/liveness/stall
measurement must be scoped to the CURRENT session via the census epoch. Verified in the merged file:

- `husk_tl_frames_live()` (`.c:1452-1459`) returns **0** when `g_live_frames.epoch != tl_guest_thread_epoch()`
  ("a session boundary passed since the mark: nothing counted here is this app's") and otherwise the
  **clamped** delta `f - base` — never a raw process-global counter, never a wrapped delta.
- `live_frames_mark()` (`.c:1447`) records `{epoch, base}` at `launch()` (`.c:649`) **and** at the session
  close (`.c:1310`), immediately after `tl_guest_thread_new_epoch()` (`.c:1305`) — so the ledger shares
  the layer's one session boundary and adds no second mechanism.
- The liveness check in `husk_tl_verify_swap` (`.c:1400`) reads `husk_tl_frames_live(&ep)`, not
  `engine_frames()`; its failure text names the epoch.
- The heartbeat (`.c:379`) and `tl_swapstall_tick` (`.c:389`) both consume the epoch-scoped value; `ss_probe`
  (`.c:1013`) uses the epoch to split this session's threads from parked ones (`gt[i].epoch < cur`).
- `husk_unity_frames()` (`.c:1460`) is unchanged — it is the raw per-engine counter, for the perf row only,
  and is never used for a liveness verdict.

`grep` confirms the only `engine_frames()` reader left is the ledger's own `live_frames_mark`, `husk_tl_frames_live`,
`husk_unity_frames` and the perf block — no frame verdict reads the raw counter.

**Guest fault scoped by FAULT ADDRESS, not pc.** `guest_arena_fault` (`.c:232-248`) passes *both* `pc` and
`info->si_addr`, and the decision lives in `tl_xmem_arena_fetch_fault(uintptr_t pc, uintptr_t addr)`
(`husk-tl-xmem.c:1479-1483`), which `(void)pc;`s the pc and returns
`g_x.open && tl_xmem_contains((const void *)addr)` — **address only**. That is the fix for the case where
scoping by pc let a data fault whose pc was a guest library outside the region escape scoping and take the
process down. A fault whose address is outside the region is still a host crash, as before. The merged
`.c` calls it with the same argument order and the same signature the header declares.

## Declarations the app needs that this merge could NOT supply a definition for

All are **declared** (so the file compiles — proven by the zig `-Werror=implicit-function-declaration`
build) but their **definitions still live in target files that are upstream's/other workers'**, which this
task does not own. They are link-time holes, reported not papered over:

| symbol | declared at | definition lives in (not yet in the target) |
|---|---|---|
| `tl_io_reset_for_new_app` | `husk-tl-unity-app.c:73` (local extern, as ours) | our `husk-tl-bionic-io.c` (target's is upstream's, 1,930 lines, no such symbol) |
| `tl_audio_close` | ported `husk-tl-audio.h` | our `husk-tl-audio.c` (target's 161-line copy has no `tl_audio_close`) |
| `tl_hp_name` + `tl_guest_exit_close_in_flight` + `tl_guest_exit_set_close_in_flight` | `husk-tl-bionic.h:129,72,73` (already ported by the core worker) | our `husk-tl-bionic.c` (target's 914-line copy has none of the three) |
| `tl_codewrite_refused_count`, `tl_codewrite_relro_count` | `husk-tl-codewrite.h:16,22` (already ported) | our `husk-tl-codewrite.c` (target's 207-line copy has neither) |

Every *other* symbol the merged file calls was found **defined** in the target during this merge:
`tl_javaapp_request_exit` (`husk-tl-dvm-android.c:1232`), `tl_javaapp_thread_leaving` (1237),
`tl_javaapp_reset` (1280), `tl_dvm_end_app` (`husk-tl-dvm.c:2464`), `tl_javaapp_documents_picked`
(`husk-tl-dvm-android.c:415`), `tl_ld_end_session`/`tl_ld_stale_count`/`tl_ld_symbol_at_any`
(`husk-tl-ld.c:2434/2467/745`), `tl_loop_quit_all`/`_live_count`/`_reset`
(`husk-tl-jni-loop.c:405/421/435`), `tl_opensl_stop_all` (`husk-tl-opensles.c:235`),
`tl_dvm_audio_stop` (`husk-tl-dvm-audio.c:147`), `tl_aaudio_stop_all` (`husk-tl-bionic-ndk.c:485`),
`tl_guest_thread_new_epoch`/`_census` (`husk-tl-bionic-pthread.c:529/581`), `tl_lock_ledger`
(`husk-tl-xmem.c:141`), `tl_swap_in_progress_set` (80), `tl_xmem_report_fault_time` (1176/1223),
`tl_xmem_backing_stats` (769), `tl_xmem_report_high_water` (1350), `tl_xmem_note_guest_fault` (1487),
`tl_xmem_arena_fetch_fault` (1479), `tl_pad_set_sink` (`husk-tl-gamepad.c:36`),
`tl_set_extra_fonts_dir` (`husk-tl-bionic-io.c:165`), `husk_console_loaded_game` (`husk-console-host.c:288`),
`tl_codewrite_count` (`husk-tl-codewrite.c:37`).

To keep the file correct by construction against the target **as it stands** (its `husk-tl-dvm-javaapp.h`
still lacks `tl_javaapp_request_exit`/`_thread_leaving`/`_reset`, and its `husk-tl-jni.h` still lacks the
`tl_loop_*` trio), the merge adds a small local `extern` block (`husk-tl-unity-app.c:64-73`) beside the one
ours already had. A repeated declaration is harmless in C, so this stays correct when those headers are
merged.

## `husk-tl-audio.h` — the one other file touched

Our copy is a strict superset of upstream's: it keeps upstream's `tl_audio_install` and
`tl_audio_set_paused` verbatim, and adds `tl_audio_close`, `tl_aaudio_stop_all`, `tl_opensl_stop_all`,
`tl_dvm_audio_stop` — the four the merged `husk_tl_session_close` calls, plus the prose that explains the
close ordering. The target's copy was replaced with it (1,924 B). This is the file the task permits touching
because the changes need declarations there. `tl_opensl_stop_all` and `tl_dvm_audio_stop` are defined in the
target today; `tl_audio_close` and `tl_aaudio_stop_all`'s definition status is in the table above
(`tl_aaudio_stop_all` is defined; `tl_audio_close` is not).

## Invariants — checked, not disturbed

- **Nothing the guest holds is unmapped; no thread that ran guest code is ended or joined.** The merge adds
  no `munmap`/`vm_deallocate` and joins nothing; the guest-fault path uses `pthread_exit` on the calling
  thread only (as ours does), and the stall probe merely *reads* a thread's registers via
  `thread_get_state` (`ss_thread_pc`). Quarantine, not join.
- **A guest crash ends the SESSION, not the process.** `guest_arena_fault` + the CAS to `HUSK_UNITY_FAILED`
  + `tl_javaapp_request_exit`/`_thread_leaving` + `pthread_exit` — unchanged from ours.
- **The executable view is never written.** `husk-tl-unity-app.c` issues no `vm_protect` and no write
  through the rx view; `write_view_pointer`/`fault_address_region` only read.
- **Host type larger than bionic's is a silent stack smash.** No bionic-vs-Darwin type size is introduced;
  the added struct field is inside the layer's own `g_java_host`.

## What could NOT be verified

- **No iOS SDK, no device, no full husk3.0 build.** The zig `aarch64-macos` compile of the merged `.c` is
  the strongest evidence available here and it is a real compile (exit 0), but it is not the app target's
  `arm64-apple-ios` build, and Swift is not compiled at all.
- **Runtime behaviour of the swap is not exercised.** The frame-scoping, the stall probe, the three
  verification checks and the guest-fault session-end are read, not run. The membership of `ENGINE_*` in
  `husk_tl_swap_capability` is copied from our working build, not re-derived.
- **The target is being edited concurrently by other workers.** The symbol-availability scan above is a
  snapshot: `tl_javaapp_request_exit`/`_thread_leaving`/`_reset` were *absent* from `husk-tl-dvm-android.c`
  minutes before this merge and are present now, so the "definition lives elsewhere" table can decay as
  those files land. Re-scan after the layer files are all merged.
- **Two `§` characters** survive in ours' comments (`husk-tl-unity-app.c` `(F §4)`, `(plan §3 step 1)`);
  the file is UTF-8 and was carried byte-for-byte, so this is unchanged from our tree.
- Nothing outside `src/translation-layer-next/` was written; `src/app/` Swift, `project.yml`,
  `husk-console-*.{c,h}`, `husk-jit-arena.*`, `tools/`, `scripts/`, `.github/` were untouched. No git
  command was run inside `husk3.0` (it is not a repo).
