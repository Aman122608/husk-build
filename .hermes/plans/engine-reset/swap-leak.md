# The `[swap] <uuid>: leaked (no session)` line — what it is, why it says that, and the fix

Scope: the line the engine's clean close writes at session close, e.g. from
`HuskLogs/husk-20261015-130333-2.0.0-112.log:471`:

```
[400968.76ms][swap] AEA429E1-BC08-44E8-9B5A-13C73044EA12: leaked (no session)
```

Files read: `src/app/Husk/GameStatus.swift`, `src/translation-layer-next/husk-tl-unity-app.{c,h}`,
`src/translation-layer-next/husk-tl-bionic-pthread.c`, `husk-tl-jni-loop.c`, the logs under
`HuskLogs/`. Files changed: `GameStatus.swift`, `husk-tl-unity-app.c`, `husk-tl-unity-app.h`.
Nothing under `tools/a32` and no other agent's plan was touched.

---

## 0. The premise to correct first: it is not a resource registry, and the UUID is not a
## swap-session id

There is **no** native structure keyed by a swap-session UUID. The line is written by Swift, not
by the layer: `GameStatusStore.recordSwap(_:_:detail:)` at `GameStatus.swift:145`:

```swift
HuskLog.log("swap", "\(appID): \(verdict.rawValue)\(detail.map { " (\($0))" } ?? "")")
```

`HuskLog.log("swap", …)` prints the `[swap]` tag (the same mechanism that prints `[status]`,
`[tl]`, `[prev]`); `appID` is the app's identity — the per-app directory name the importer mints
under `Documents/TranslationLayer/<uuid>/`, which is also the APK's parent directory. The *same*
UUID appears on the `[status]` line for the same app (`GameStatus.swift:106/130`), e.g.
`130333:440 [status] AEA429E1-…: plays`, and the app's APK path is
`…/TranslationLayer/AEA429E1-…/Factorial_Omega_My_Dystopian_Robot_Girlfriend_.97.11.apk`
(`130333:106`). So the "swap UUID" in the second app's line (`6E5E49CD-…`) is *that app's own id*,
because the id names its own directory — not a session object, and not an orphan from app A.

**Resource kind: not a surface, context, GL object, view, descriptor table or audio pump.** It is
a **per-app bookkeeping verdict** — the `swap` field of that app's `status.json`
(`GameStatus.swift:89`, `:142`), composed by `NativeSession.close`/`verifyLoaded` from the layer's
`husk_tl_swap_verdict()`/`husk_tl_swap_status()`.

---

## 1. Where `leaked` comes from, and where `no session` comes from

`NativeSession.close(...)` (`GameStatus.swift:263`) closes the layer's session and, on a
`SWAP_PARTIAL` result, records verdict `.leaked`:

```swift
case .partial:
    let outcome = finish(appID, .leaked, detail: detail, …)   // GameStatus.swift:275
```

`SWAP_PARTIAL` is the layer's own return from `husk_tl_session_close()`
(`husk-tl-unity-app.c:846`). For a Java-engine run it is downgraded from `SWAP_OK` by any of:

- a Java app thread asked to stop that would not — `:924`
  (`verdict = !dvm_ok ? SWAP_NEEDS_RESTART : app_ok != 0 ? SWAP_PARTIAL : SWAP_OK;`)
- a Looper thread still running — `:970`
- a guest thread the pthread shim started and that has not returned — `:980`

In the 130333 run robot gf's close printed `tl_javaapp_reset=0, tl_dvm_end_app=1` (`:469`, so the
`:924` term is clean), `every looper thread unwound` (`:472`), and
`35 guest thread(s) still running (not ours to stop; quarantined)` (`:473`) — so the `:980` term is
what made this close `SWAP_PARTIAL`. **The verdict `leaked` is therefore honest: 35 guest threads
from robot gf were never returned.**

The `(no session)` parenthetical is a **read-order bug in Swift**. `close()` read the layer's words
*before* asking the layer to close:

```swift
let detail = statusText()                              // husk_tl_swap_status(), BEFORE the close
switch SessionClose(rawValue: husk_tl_session_close()) // the close writes g_swap_status here
```

`husk_tl_swap_status()` returns `g_swap_status`, whose value is only ever written *inside* a close
(`husk-tl-unity-app.c:853/872/881` set `"no session"`; `:1034-1039` set the real close's string).
Its initial value is the literal `"no session"` (`:789`). So on the **first close of a process**
the captured `detail` is the boot default — hence
`[swap] AEA429E1-…: leaked (no session)`. On any later close it is one close behind: the *second*
app's line in the same process shows the *first* close's words, which is exactly the "garbled"
line the owner saw, `[swap] 6E5E49CD-…: leaked (swapped, leaked (detail in the log))`
(`130333:638` = the verify path, `:711` = its own close). The full census confirms it: a log shows
`(no session)` for its first close and `(swapped, leaked (detail in the log))` for every later one
(`005154:289` then `:401`; `011250:340` then `:452`; `011530:331,442,481`).

So the owner lookup does not fail because a session was never opened, or already ended, or a
resource was registered outside one. There is no lookup. **The message states the parent close's
words, or the boot default, as this close's detail.**

---

## 2. Is anything genuinely lost?

**No.** Nothing the layer holds is freed, and "leaked" is the layer's honest word for *quarantine*,
not for a lost allocation:

- The close resets **bookkeeping only** — `husk_tl_session_close` says so at `:48-50` and does it at
  `:991` (`tl_ld_end_session`), `:1005` (`tl_io_reset_for_new_app`), `:1010-1015` (field-by-field
  `A.*` clear). Mappings stay mapped; the arena is not unmapped; no guest thread is ended
  (`:975-976`: "not ours to stop").
- The 35 threads are tracked, not lost: `g_guest_threads` is an atomic counter incremented on
  `start_thunk` entry and decremented on return (`husk-tl-bionic-pthread.c:447-448,454,466`). A
  thread that never returns keeps the count; `tl_guest_thread_live_count()` is the number the close
  reports.
- The `Swap` record is a `status.json` field, overwritten on the next swap
  (`GameStatus.swift:142`); it holds no GPU/EGL/fd.

What the close *does* leave behind for the process lifetime, by design, is real but is not what the
`[swap]` line is about: the previous app's library mappings, the shared JIT arena (never reset, a
bump allocator — the `xmem: arena page backing` spans only ever grow), the parked guest threads, and
a fresh EGL context + window surface per app that are never destroyed (`ui context 0x1, window
surface 0x1` for robot gf → `ui context 0x5, window surface 0x3` for lewdest house, `130333:227`
vs `:613`, same shared `display 0x167870a0 0`).

---

## 3. Does it break the next app? — the message is benign; the swap is where the risk is

The message itself cannot break anything: it is a string written to a log and to `status.json`.

The empirical answer from `130333` is that the second app **did not come up**: lewdest house
(`com.dmf.thelewdesthouse`, Ren'Py/SDL) got its own GL display/context/surface and presented **one**
frame (`swap verified: liveness ok (1 frames)`, `:641`), then sat on the black "starting" screen —
`unity: alive: 1 frames (+0)` for ~45 s (`:644-691`) — and was ended by the user. Its swap plumbing
all passed (`identity ok`, `scope ok`, `liveness ok`, `swap verified OK`, `:639-643`).

What I can **prove** from the code and logs about cross-swap carryover:

- 35 guest threads from app A are still live and still counted at app B's close (still exactly `35`
  in `130333:713`). They run guest code in the **shared** arena that app B is handed.
- Per-app EGL contexts/surfaces are never destroyed (see §2), so GPU-side objects accumulate one set
  per swapped-out app.
- `tl_loop_reset()` (`husk-tl-jni-loop.c:435`) does forget the old loopers (`g_nloopers = 0`), so
  loopers do not accumulate.

What I am **inferring** (correlation, not proof — no compiler and no device here): a swapped-in
session that reaches SDL `onResume()` and then stalls on its first frame is consistent with a
*parked* guest thread from app A holding a guest lock (a JVM monitor, the pthread-shim mutex, or the
arena bump allocator) that app B's new thread then blocks on. The mechanism is plausible and matches
the design's own warning that `SWAP_PARTIAL` means "a thread from this session may still execute
guest code" (`GameStatus.swift:277-282`), but I cannot prove it from the artifacts alone; the
discriminator would be a run with the parked threads' lock state logged at the stall.

The `E/AsynchronousCloseMonitor: setting blocked thread signal handler failed: Invalid argument`
line is **not** swap-specific: it appears in the *first* app's own launch in every log, including
robot gf (`130333:192`, `125856:192`), i.e. before any swap has run. It is unrelated to the leak.

So: the `[swap] leaked (no session)` line is benign bookkeeping; the real cross-app risk is the
quarantine the *verdict* names (parked threads in a shared arena) plus the un-destroyed per-app EGL
objects — and neither is caused by, nor fixed by, the message.

---

## 4. The fix (general; both halves kept honest)

The message was wrong about ownership, so it was corrected while the honest verdict was kept. The
verdict still says `leaked`; the parenthetical now states the actual condition.

1. **`GameStatus.swift` — read the layer's words *after* the close.** The detail a close records is
   now this close's own (`:270-271`):
   ```swift
   let close = SessionClose(rawValue: husk_tl_session_close()) ?? .needsRestart
   let detail = statusText()
   switch close { … }
   ```
2. **`husk-tl-unity-app.c` — make the partial status string name the condition.** For `SWAP_PARTIAL`
   the string is now built from the counts the close already reports (`:987-997`, used at
   `:1037`), e.g. `35 guest thread(s) left parked; not ours to stop`, instead of the placeholder
   `swapped, leaked (detail in the log)` that produced the self-referential
   `leaked (swapped, leaked (detail in the log))`. The verdict word is carried by
   `husk_tl_swap_verdict()`, so the composed line reads `[swap] <id>: leaked (35 guest thread(s)
   left parked; not ours to stop)`. `husk-tl-unity-app.h:54` documents the new shape.

No invariant was relaxed: the close still does not unmap, free or end anything the guest holds, and
`SWAP_PARTIAL` still downgrades the verdict (nothing was silenced). The cross-app hazard the verdict
names — parked guest threads in the shared arena — remains reported, now with a readable reason.

### What a next run must show

- robot gf's close: `[swap] AEA429E1-…: leaked (35 guest thread(s) left parked; not ours to stop)`
  — no `(no session)`.
- No line in the shape `leaked (swapped, leaked (…))`.
- The black-screen stall is *not* addressed by this fix; if it persists, the next step is to log the
  parked threads' lock state at the stall to prove the deadlock inference.
