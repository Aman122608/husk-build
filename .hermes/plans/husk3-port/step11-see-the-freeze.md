# step11 — see the freeze

Target: `C:/Users/Aiden/Desktop/husk3.0`, branch `husk3`, commit `cd914e2`, pushed to `Aman122608/husk-build`.
Gate: every file below was compiled with `zig cc -target aarch64-macos -D_DARWIN_C_SOURCE -Isrc/translation-layer-next -Isrc/translation-layer -c <file>` — all nine touched files pass. No device, no Mac, no iOS toolchain was available, so nothing here is a run: it is correctness by construction plus the per-file compile gate, and it is labelled as such.

## The freeze, restated

`husk-20261018-114856-3.0.0-129.log`: the app is alive and not idle (`perf[hb]` shows `mutex_lock`/`mutex_unlock` climbing 91,761 → 99,078 while `cond_wait` stays 29 and frames stay 8), it presented 8 frames and then none for 8.1 s while RUNNING, and the swap-stall verdict says no parked thread holds any layer lock it can see. So the stuck thread is one of the ~32 LIVE guest threads this session started — and the three instruments that were supposed to look at it printed nothing.

## Why each instrument was blind

1. **Asset trace** (`tl_asset_read_note`, `husk-tl-bionic-io.c:819`). It ran only when `TL_ASSET_TRACE=1` (unsettable on the phone) or when `tl_swap_in_progress()` was true. That flag is set by `tl_swap_in_progress_set(1)` **inside `ss_probe`** (`husk-tl-unity-app.c:1018`) — i.e. at the instant a stall has already been detected. By then the app has stopped reading. An instrument that arms at the freeze can only record the silence after it; the newest log confirms it: the swap-stall line is present and there are zero `asset` lines.
2. **Guest-mutex trace** (`mutex_trace()`, `husk-tl-bionic-pthread.c:117`). Armed by the same switch (`tl_swap_in_progress()`) or `TL_MUTEX_TRACE`. Same failure mode: it turned on inside the probe, after the 5-second wait it exists to name had already gone by — zero `mutex:` lines.
3. **Stall ledger** (`ss_probe`, `husk-tl-unity-app.c:1013`). It counted the live guest threads and then annotated only the ones from an **earlier** session: `parked = count(gt[i].epoch < cur)` and every per-thread line is inside `if (gt[j].epoch >= cur) continue;`. The 32 live threads of *this* session — the only population that can contain the stuck one — were counted and then not reported.

## What now captures it

### (1) Asset reads leading up to the freeze — always-armed ring (`husk-tl-bionic-io.c`)
`tl_asset_read_note` no longer tests `tl_swap_in_progress()`; it records into a fixed 64-slot ring **on every run**. Hot path per asset read: one relaxed `__atomic_fetch_add` (the read's sequence number), a bounded ≤160-byte copy of the entry name, an 8-byte store of the reading thread's id, then the sequence is published last (`release`) — so a dumper never treats a half-written name as current. No allocation, no lock, no log line. `TL_ASSET_TRACE=1` still logs each read verbatim (a desktop aid); it is no longer what makes the record exist.
Dumped oldest-first by `tl_asset_ring_dump(tag)` at a detected stall (`tag="stall"`) and at the session boundary (`tag="close"`, in `tl_io_reset_for_new_app`, followed by `tl_asset_ring_reset()` so the next app starts with its own ring). Slots are read by walking `first..W` and skipping any whose stored sequence has been overwritten, so the dump is in order and never shows a torn name.

### (2) Guest-mutex trace armed from run start (`husk-tl-bionic-pthread.c`)
`mutex_trace()` now returns true from process start; `TL_MUTEX_TRACE=0` disables it. Its own rule already keeps the *log* quiet — one line per distinct `(waiter site, holder site)` pair and only after 5 s of waiting — so arming it from run start costs nothing until a lock actually waits.
Additionally, `tl_guest_mutex_waits()` scans the live-thread census at stall time and returns every guest mutex **currently held with a live waiter**, both sites and the holder, even when the holder was written after the wait began. `ss_probe` prints them.

### (3) Ledger covers ALL live guest threads (`husk-tl-unity-app.c`)
The census (`g_gt` in `husk-tl-bionic-pthread.c`) now also carries, per thread, `last_kind`/`last_site`/`last_ns`: the last shim lock call the thread entered and when (monotonic ns). `wait_kind` stays the *current* state (cleared when the shim returns); `last_*` persist across the return. `ss_probe` now emits one `stall-threads:` line **per live thread, this session's included**, with its session (this / PARKED), the entry it was created on, its pc, whether it is blocked inside a shim lock, its last shim and how long ago, and its wait object.
The frame/render thread is named explicitly (`stall-frame:`), by engine (`UnityMain` for Unity, `GLThread` for the Java driver/GL engines, `SDLThread`, `UiThread`). Unity's and the Java driver's frame thread are the **layer's own** threads (`pthread_create` directly in `husk-tl-unity.c` / `husk-tl-dvm-android.c`), so they are *not* in the guest census — the ledger says so and still samples the thread's pc via a new accessor (`tl_unity_thread()`, `tl_javaapp_render_thread()`), because that thread's missing next frame IS the stall.

### (4) Name the missing file (`husk-tl-dvm.c` + `husk-tl-bionic-io.c`)
`b_open`/`b_openat` now record the host path of every **failed** open that returned ENOENT into a bounded, deduped table (`TL_OPEN_FAIL_MAX` = 32, first-seen order). Failed opens are the cold path, so a successful open pays nothing.
`report_libcore_natives` (the loader for `libjavacore.so`/`libopenjdk.so`/`libicu_jni.so`) dumps that table when the pending exception's description contains `No such file` — which is exactly this run's line, `dvm: loading libcore's natives (libopenjdk.so) threw java.io.IOException: No such file or directory` (a native `ENOENT` reported with `strerror(errno)` and an empty stack trace).
**No fix was applied.** The file's identity is not yet known from a device run, and a guess would not be a fix; the instrument names it on the next run, and the fix lands only once the path is real.

## The exact new log lines a stalled run will print

At the detected stall (in order):

- `asset[stall]: the <N> asset read(s) recorded before this point, oldest first (#<a>..#<b> of <N> this session) -- these are the app's OWN last reads, so the freeze happened after (or inside) the last one:`
- `asset[stall]: #<seq> opening '<entry>' by '<thread>'` / `... read '<entry>' served by '<thread>'` / `... read of '<entry>' FAILED (errno <e>) by '<thread>'`
  → what the app was reading in the run-up to the freeze; the last entry is the last thing it touched.
- `stall-threads: <N> guest thread(s) counted live (<X> this session, <P> parked from an earlier session, epoch <E>); the FRAME/RENDER thread is 'UnityMain':`
- `stall-threads:  #<i> '<name>' [this session|PARKED, an earlier session] entry [<sym>] pc [<sym>|not readable by the layer]; <BLOCKED inside a shim lock|not in a shim lock ...>; last shim <b_mutex_lock|b_cond_wait|b_sem_wait|b_rwlock_lock|a shim lock> at <site> (<T> s ago); wait-object <ptr>|(none)[  <<< FRAME/RENDER THREAD: its next frame is the one that is missing]`
  → one line per live thread; the thread with `last shim b_mutex_lock ... (8.1 s ago)` inside `this session` is the stuck one, and its pc names where.
- `stall-frame: the frame/render thread is 'UnityMain' (tid 0x…, the layer starts it itself (not a guest pthread_create), so it is NOT in the guest-thread census above): pc [<sym>] -- this is the thread whose missing next frame IS the stall.`
- `stall-mutex: no live guest thread is blocked in b_mutex_lock right now, ...` **or**
  `stall-mutex: guest mutex <ptr> is HELD by '<holder>' (tid 0x…) locked at <site>, with a live waiter '<waiter>' (tid 0x…) blocked at <site> -- a guest-level lock (bionic pthread mutex), not in the layer lock ledger; if the holder is a parked thread, the guest deadlock IS the quarantine.`

From run start (no longer only after the stall), the existing 5-second line can now fire:
- `mutex: '<waiter>' has waited 5 s on mutex <ptr>, held by '<owner>' (locked from <site>); waiting at <site>`

At session close:
- `asset[close]: the <N> asset read(s) recorded before this point, oldest first (#<a>..#<b> of <N> this session) ...` plus its per-read lines.

If the libopenjdk natives load fails again:
- `open[libopenjdk.so]: the <K> path(s) a failed open() looked in, in the order first seen -- the file the guest reports only as "No such file or directory" is one of these:`
- `open[libopenjdk.so]: '<path>' (errno 2)`

With `TL_ASSET_TRACE=1` (desktop only): `asset: opening '<entry>' by '<thread>' (the app's first-frame path)` etc.

## Cost of always-on arming

- **Asset ring** — per APK-asset read: one relaxed atomic add, a ≤160-byte bounded copy, an 8-byte store. No lock, no allocation, no log. Asset reads are load-time (not per frame) in practice, so this is negligible; the only way it becomes measurable is an app that reopens assets every frame.
- **Mutex trace — the real cost.** With the trace on, `b_mutex_lock` no longer blocks in Darwin: it polls `pthread_mutex_trylock` + `usleep(200)` (≈25000 iterations per 5 s wait) and writes 16 bytes (owner tid + site) into the guest mutex object on every acquisition, cleared on unlock. Uncontended, this is one successful trylock (≈ the same as a blocking lock) plus two stores. Contended, a waiter burns CPU at ~5 kHz instead of sleeping — measurable, and the deliberate trade for seeing the wait.
- **Census timestamps** — `gt_wait_enter` now also reads `clock_gettime(CLOCK_MONOTONIC)` (a vDSO read, `gettimeofday`-class) once per shim lock entry, on top of the stores it already did. In principle measurable; in practice the lock shims already do more.
- **Failed-open table** — only touched on a failed open (cold path); a successful open pays nothing.

## What I could not verify

- **No run.** No Mac, no iOS toolchain, no device (`swiftc`/`clang` absent). Every behavioural claim is read from the code or is correctness by construction; only the zig per-file compile gate was exercised. (`husk-tl-xmem.c` fails it on a missing `mach/mach_vm.h` framework header — a pre-existing zig limitation, unrelated to this change; it is why the gate is described as per-file.)
- **The missing file is still unnamed** — that is the whole point of the instrument; I did not guess a path and applied no fix. If the next run still shows an empty `open[libopenjdk.so]:` dump, that itself says the failing open bypassed `b_open` (so the culprit is not a guest `open()` the layer interposes) and the search moves to the loader itself.
- **The mutex-trace contention cost** is asserted from the code's own spin loop, not measured on hardware.
- **`UnityMain`/`GLThread` being absent from the guest census** is read from their `pthread_create` call sites (the layer's own, not `b_create`), not observed on device.
- **Dedupe-by-mutex in `tl_guest_mutex_waits`** reports the first live waiter per held mutex; a mutex with several waiters shows one, with the count available from the census if a later step wants it.
