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

---

# Addendum: the swapped-in black-screen stall, made to name itself

Scope: `src/translation-layer-next/husk-tl-xmem.{c,h}`, `husk-tl-ld.c`,
`husk-tl-bionic-pthread.c`, `husk-tl-bionic.h`, `husk-tl-unity-app.c`. Nothing under
`tools/a32`; no other plan touched.

## 0. The premise, restated: the first app always works, the second never comes up

The stall is not one app, one engine or one direction. In `130333` it was robot gf (first,
1327 frames, fine) then lewdest house swapped in → 1 frame → black. In `131029` it is the
**reverse**: lewdest house ran first to 275 frames and closed cleanly, then robot gf was
swapped in — `swap verified: scope ok`, `liveness ok (1 frames)` (`131029:534-535`) — and sat
on its first frame (`131029:532`). **The same app runs perfectly standalone on this build.**
So the variable is not the app; it is *what the swap left behind*. That is the whole
inference to test.

Two further facts from `131029` sharpen it, and both are carried into the probe:

- The carried-over thread count is **2**, not 35 (`131029:435`: `2 guest thread(s) still
  running (not ours to stop; quarantined)`). So **crowding is out**: one or two parked threads
  are enough. The mechanism to test is **a single thread holding a lock**, not contention
  among many — so the probe names *each* parked thread and *who holds what*, at the
  granularity of one thread.
- The verdict lines confirm the read-order bug diagnosis on a fresh log: the *first* close in
  the process says `leaked (no session)` (the boot default, `131029:433`) and a later close
  says the one-behind string (`131029:532`), with each line carrying that app's own
  `TranslationLayer/<uuid>/` id (`6E5E49CD…` lewdest house, `AEA429E1…` robot gf). The §1
  diagnosis stands; the §4 fix is untouched here.

## 1. Every lock a parked guest thread could hold, and a swapped-in app could block on

A parked thread is one the close neither ended nor joined — **it keeps running** guest code
(`husk_tl_session_close`; `130333:473`). For it to hang the next app, it must be *inside* a
lock when the close walks away and *never leave* it. The candidates, in this layer:

**A. The arena allocator's lock — `husk-tl-xmem.c`.** The arena has one lock, a **single global
non-recursive `pthread_mutex_t`**, `g_x.lock` (`husk-tl-xmem.c:56`, `g_x` at `:61`). It is
taken by exactly two functions — `tl_xmem_open` (`:505/517`) and `tl_xmem_alloc` (`:567/621`)
— and **nowhere else**. What is held under it: the whole bump allocation, the per-page
read-back of the handed-out span, a pair of guarded signal-probes, and one `tl_log_line`
(`:583-619`). **No guest code runs under it** — there is no `c.fn`, no constructor, no
guest call inside the critical section. So a parked thread is only realistically inside it if
it was **interrupted mid-allocation**; and the only blocking-capable step inside is the logger's
own mutex (`husk-tl-load.c:338`, short-held) — so it would have to be stuck there. Ranked
**third**: real, but the lock is never held *across* guest code.

**B. The loader's serialising lock — `husk-tl-ld.c`.** `g_big` is a **recursive**
`pthread_mutex_t` (`:116`) taken by four entry points (`tl_ld_load`, `tl_ld_init`,
`tl_ld_begin_session`, `tl_ld_end_session`). Crucially, `tl_ld_init` holds it **across
`init_locked`**, which calls the library's `DT_INIT` and every `DT_INIT_ARRAY` constructor —
i.e. **arbitrary guest code** (`husk-tl-ld.c:2099`, `:2105`, under `:2115`). A constructor
that maps a big asset, waits on I/O, or runs a game loop holds `g_big` for as long as it runs;
if the session closes while a thread is inside one, that thread stays inside — **still holding
`g_big`** — and the next app's very first `dlopen`/`dl_ld_init` blocks on it forever. This is
the one lock **held across a guest call that can park**, so it ranks **first**.

**C. The loader's registry lock — `husk-tl-ld.c`.** `G.lock` (plain, `:101`) guards registry
bookkeeping only (`tl_ld_maps`, `drop_other_sessions`, the end-session APK close,
`tl_ld_stale_count`, `tl_ld_handle_live`) — no guest code under it, and it is always nested
*inside* `g_big` during a load. Ranked **fourth** (bookkeeping).

**D. The codewrite store guard — `husk-tl-codewrite.c`.** **It takes no layer lock at all.**
It runs on the SIGSEGV/SIGBUS fault path, decodes the faulting store, and carries it through
the writable twin (`put()`, `:67-93`); its only shared state is a lock-free refusal table. It
takes neither `g_x.lock` nor `g_big`, so **nothing a swapped-in app needs is held by it** —
ruled out as a lock holder (the probe says so explicitly).

**E. Guest- and Java-level locks.** The bionic pthread shim's mutexes (`husk-tl-bionic-pthread.c`:
`b_mutex_lock` etc., the guest's own objects, plus the shim's own `obj_host` init race), the
DVM's Java object monitors (`husk-tl-dvm.c:762` `mon` = a `pthread_mutex_t` + owner + count) and
its `g_lock`/`g_open_lock`, and the JNI `JNI_OnLoad` mutex (`husk-tl-jni-hle.c:419`). These are
**guest/Java-level** locks held by guest code. The layer's ledger does **not** cover them, and
cannot enumerate who holds an arbitrary guest mutex. Two reasons this is not the first suspect
for *this* stall: (i) the stall is direction-independent across a Java app and an SDL app, and a
Java monitor held by an SDL thread (or vice versa) is a mismatch, not a shared resource; (ii) the
pthread shim **already** names a guest mutex that has waited 5 s, under `TL_MUTEX_TRACE`
(`husk-tl-bionic-pthread.c:107-127`) — that is the paired run for this class, and the probe says
so rather than guessing.

**If a parked app-A thread is inside the arena lock when app B allocates:** app B's
`tl_xmem_alloc` blocks on the same `g_x.lock` (via `tl_xmem_open`/`tl_xmem_alloc` in `ld.c` at
a library load, `husk-tl-bionic-io.c:1483/1496` for a JIT mmap, `husk-tl-ld.c:695/1601/1913`
for loader spans, `husk-tl-egl.c:730`). The same question for `g_big` yields the same deadlock,
one level up and far more likely.

### Ranked, with the one line of code that supports each

1. **loader `g_big`** — held across guest constructors: `husk-tl-ld.c:2115` →
   `init_locked` → `:2099 ((init_fn)(L->rx + …))(…)`. *Recursive, so a thread parked one level
   deep keeps it.*
2. **arena `g_x.lock`** — a single global mutex, `husk-tl-xmem.c:56/567`; held across the
   allocation + its read-back/probe/log, **never across guest code** — a candidate only if
   interrupted mid-hold.
3. **loader `G.lock`** — `husk-tl-ld.c:101`; bookkeeping only, nested inside `g_big`.
4. **guest mutexes / JVM monitors** — not in the ledger; the shim's 5 s `TL_MUTEX_TRACE` waiter
   line is the tool for them (`husk-tl-bionic-pthread.c:124`).
5. **codewrite guard** — takes no layer lock; ruled out.

## 2. The instrument: a lock ledger, a thread census, and a stall probe

Three pieces, all **lock-free**, so a probe that reports a stall cannot itself be what hangs,
and none of it runs on a fault path or installs a signal handler:

- **Layer-lock ledger** (`husk-tl-xmem.c`, declared in `husk-tl-xmem.h`): fixed slots
  `TL_LOCK_ARENA`, `TL_LOCK_LOADER_BIG`, `TL_LOCK_LOADER_LIBS`. `tl_lock_acquired(slot, site)`
  records the holder's `pthread_self()` + its name and a site string; `tl_lock_released(slot)`
  drops it. Depth counts nesting (the loader lock is recursive). It lives in `xmem.c`, not a new
  file, **because `tools/a32/build.sh` links `husk-tl-xmem.c` on its own** (`step0`/`diff`,
  `arm32-harness.yml`): the ledger must not add an undefined symbol there. The loader's sites
  call it through `ld_big_lock/unlock` and `ld_libs_lock/unlock` wrappers (`husk-tl-ld.c:125-128`).
- **Guest-thread census** (`husk-tl-bionic-pthread.c`, declared in `husk-tl-bionic.h`): the bare
  `g_guest_threads` counter is joined by a small lock-free table — per live thread its Darwin
  `pthread_t`, the entry it was created on, and the **session epoch** it started in. A session
  close calls `tl_guest_thread_new_epoch()`, so a thread from the swapped-out app is
  `epoch < current`. `tl_guest_thread_census()` reads it without ever taking a lock.
- **Stall probe** (`husk-tl-unity-app.c`): the heartbeat calls `tl_swapstall_tick(frames,
  running)` every tick. When the app has presented ≥1 frame and then made no new one for a
  bounded time (default **8 s**, `TL_SWAPSTALL_SECS` overrides), it prints **once per distinct
  stall** — the latch clears when frames advance again. It reads the census and the ledger,
  cross-references holder-vs-parked by `pthread_t`, and — where the kernel will read it — takes
  each parked thread's **pc** with `thread_get_state` (it briefly stops the thread to copy its
  registers; it does not end, join, unmap or install a signal handler for a signal being
  handled). Where the kernel will not, it prints `not readable by the layer` — never a guess.

## 3. The exact line the probe prints

```
swap-stall: app presented 1 frame(s) then no new frame for 8.0 s while RUNNING (engine sdl, state running) -- the difference from this app run alone is exactly what the swap left behind; layer locks: arena=free loader-big=HELD(PARKED THREAD 'SDLTimer' from loader load/init -- a thread from the swapped-out session) loader-libs=free; codewrite guard takes no layer lock (a signal-time store redirect, husk-tl-codewrite.c); 2 guest thread(s) still counted live, 2 from an earlier session (parked); parked threads: #1 'SDLAudioP1' entry [libmain.so ?+0x0] pc [libmain.so ?+0x1a3c]; #2 'SDLTimer' entry [libmain.so ?+0x0] pc [the arena +0x21f40 (executable view: it is running JIT'd guest code)]; VERDICT: a thread parked from the session that was swapped out holds 'loader-big' -- the swapped-in app blocks on it here, so the quarantine (a thread left mid-hold by the close) IS the deadlock, and the fix is to make the swap wait for or reclaim those locks rather than quarantine a thread mid-hold.
```

The two shapes the `VERDICT` clause can take are the whole point:

- `VERDICT: a thread parked from the session that was swapped out holds '<lock>' … the
  quarantine (a thread left mid-hold by the close) IS the deadlock …`
- `VERDICT: no parked thread holds any layer lock the layer can see, so this stall is NOT the
  swap's quarantine -- look next at the app's OWN first-frame path (its onResume/launch and its
  own asset/asset-file reads) …`

## 4. What the probe proves, either way

- **If a parked thread is inside the allocator (or holds the loader lock / any layer lock) when
  app B stalls** — the line names the thread, the lock and the site: that **is the deadlock**, and
  the fix is to make the swap **wait for or reclaim** those threads' locks instead of
  quarantining a thread mid-hold (out of scope here; prove the cause first, as required).
- **If no parked thread holds anything app B needs** — the line says so plainly, and the next
  place to look is **app B's own first-frame path**: its `onResume`/launch and its own asset /
  virtual-file reads. A guest-level mutex would then be the remaining suspect, and the paired run
  is `TL_MUTEX_TRACE` (the shim's existing 5 s waiter line names holder and waiter).

The probe does not force the first story: it reports the ledger and the census as they are, and
the verdict sentence follows the evidence. Nothing the guest holds is unmapped, no thread that
ran guest code is ended or joined, and the honest accounting (`leaked`, the parked count) is
unchanged — the fix in §4 of this file is untouched.

## 5. What a next run must show

- One `swap-stall:` line, **once**, within ~10 s of the swapped-in app's first frame going quiet.
- The line states `engine …` and `state running`, the number still counted live and how many are
  from an earlier session.
- For each layer lock: `free`, or `HELD(<PARKED THREAD|thread> '<name>' from <site> …)`.
- Each parked thread named: `'<name>' entry [<image …>] pc [<image …> | the arena +0xN | not
  readable by the layer]`.
- A final `VERDICT:` naming either the lock (deadlock confirmed) or that no parked thread holds
  a layer lock (next place: the app's own first-frame path).

