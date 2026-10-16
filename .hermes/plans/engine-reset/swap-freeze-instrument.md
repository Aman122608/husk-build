# The swap's liveness and stall measurements must count the app that is ON SCREEN

Scope: `HuskApp` on `main`. Evidence is one real device run, read first:
`C:/Users/Aiden/Desktop/HuskLogs/husk-20261016-130343-2.0.0-112.log` (build `b717cca`). No compiler and no
Mac on this box: everything below is read from that log and the tree, written by construction, and re-read.
`.github/workflows/check-translation-layer.yml` compiles the layer; the Swift side is not covered by that
gate (it needs the IPA build), so the Swift edits here are deliberately small and mechanical.

## 1. What the log proves

Three titles in one process, no restart. Robot gf (Unity/il2cpp, session `6DEAEDE8`) ran and closed at
`201890.18ms` (`leaked (35 guest thread(s) left parked; not ours to stop)`). The Lewdest House (Ren'Py,
**launched as `ENGINE_JAVA`** — `java: starting … com.dmf.thelewdesthouse`, line 526) then swapped in at
`238650.52ms` and the layer's own verification passed:

```
637  husk: swap verified: identity ok ('…/TheLewdestHouse-0-2-42.apk', package 'com.dmf.thelewdesthouse')
638  husk: swap verified: scope ok (no library from an earlier session is reachable)
639  husk: swap verified: liveness ok (1 frames)
640  husk: swap accounting: 0 stale libraries, 1 frames, loaded '…/TheLewdestHouse-0-2-42.apk'
641  husk: swap verified OK ('…/TheLewdestHouse-0-2-42.apk', 'com.dmf.thelewdesthouse')
```

It got its first compositor frame (`612 javaapp: first frame presented for this app`), and then nothing:
5 egl swaps in the whole file, none after the swap, and `unity: alive: 1 frames (+0)` for the rest of the
run. The stall probe did fire (`660 swap-stall: app presented 1 frame(s) then no new frame for 8.1 s …`),
so the freeze was *seen*; what it could not do is prove the count it used belonged to the app on screen.

## 2. The defect: the verdict was not scoped to the live session

Both apps arrive through the Java driver, so `engine_frames()` → `tl_javaapp_frames()` → `A.frames`
(`husk-tl-dvm-android.c`), a **single process-global static**. Its per-session meaning comes only from the
teardown happening to zero it (`tl_javaapp_reset`, `:1360`; `tl_javaapp_start`, `:1149`). Nothing in the
verdict ties the number to the session that presented it, so:

- a non-Unity engine whose counter is **not** reset by the teardown (`tl_egl_frames_presented()` for SDL /
  GameActivity, a process-global egl swap tally) would hand an earlier session's frames to a swap about the
  new one;
- a thread the swapped-out session left running that still presents ticks the same counter, so its pulse
  can be read as the new app's;
- the heartbeat derived both `unity: alive:` and the stall probe's frame count from that same raw counter
  (`husk-tl-unity-app.c`, `heartbeat_thread`).

The rule this breaks is the one the whole swap path rests on: **a verdict may never be derived from a thread
belonging to an earlier session.**

## 3. What now measures the live session

The census already carries the session boundary — `tl_guest_thread_epoch()` / `tl_guest_thread_new_epoch()`
(`husk-tl-bionic-pthread.c`) — so the frame ledger is keyed to it and adds no second notion of a session:

- `g_live_frames { int epoch; unsigned long base; }` — the mark and the census epoch it belongs to.
- `live_frames_mark()` sets `base = engine_frames()` and `epoch = tl_guest_thread_epoch()`. It is called in
  two places, both session boundaries: in `launch()` right after `A.engine = engine` (the new engine's own
  counter starting value — no other engine's baseline can be crossed), and right after
  `tl_guest_thread_new_epoch()` in `husk_tl_session_close()` (the explicit tie to the census epoch, and the
  correct reading for a close with no relaunch).
- `unsigned long husk_tl_frames_live(int *epoch_out)` returns `engine_frames() − base`, clamped at 0, and
  returns **0** when `tl_guest_thread_epoch()` no longer equals the mark's epoch — "no frame of this app's
  own", never an earlier session's frames. `epoch_out` hands the caller the epoch the reading belongs to, so
  the line can print it.
- `engine_frames_for(int engine)` was split out of `engine_frames()` so the ledger can snapshot a counter by
  name; `engine_frames()` is now `engine_frames_for(A.engine)` and `husk_unity_frames()` is unchanged.
- For a non-Unity app the source is unchanged but now correctly scoped: an SDL/Ren'Py app's `tl_sdl_frames()`
  (its own egl/vk swap), a GameActivity app's `tl_ga_frames()`, and for the Java driver's apps the render
  thread's own compositor counter (`A.frames`, incremented at `husk-tl-dvm-android.c:999` next to the
  `javaapp: first frame presented for this app` line). The mark and the epoch check are what make the count
  THIS session's.

Readers changed to use it:

| site | before | now |
|---|---|---|
| `heartbeat_thread` (both lines) | `engine_frames()` | `husk_tl_frames_live(&ep)`, printed as `unity: alive: N frames (+k) in this session (session epoch E)` |
| `husk_tl_verify_swap` (c) | `engine_frames()` | `husk_tl_frames_live(&ep)` |
| `tl_swapstall_tick` / `ss_probe` | the heartbeat's raw `engine_frames()` | the same live count, plus the epoch |

New verdict text (the honest claim, not "1 frames"):

```
husk: swap verified: liveness ok (<N> frames presented by the app now on screen '<package> (<apk file>)' since the swap; session epoch <E>)
husk: swap verified FAILED (liveness: the app now on screen '<package> (<apk file>)' has presented no frame of its own since the swap; session epoch <E>)
husk: swap accounting: <S> stale libraries, <N> frames presented by '<package> (<apk file>)' since the swap, loaded '<apk>'
```

## 4. The failure names itself, scoped to the live session

`ss_probe` keeps its existing shape (one line, once per distinct stall, lock-free, no thread ended/joined/
unmapped) and gains the app's identity and the epoch. The first segment is now:

```
swap-stall: the app now on screen (<package> (<apk file>)) presented <N> frame(s) of its own since the swap,
then no new frame for <q> s while RUNNING (engine <engine>, state <state>, session epoch <E>) -- counted from
the live session only, never from a thread the swapped-out session left running; the difference from this app
run alone is exactly what the swap left behind; layer locks: <ledger>; … parked threads: …; VERDICT: …
```

`ss_app_name()` builds `<package> (<apk file>)` from `A.package` / `A.apk` — the live session's own
bookkeeping, no inference. It is the only new helper the line needs; everything after it (the layer-lock
ledger, each parked thread with its entry/pc and LOCK-WAITING vs idle, and the VERDICT) is the existing
probe, unchanged. The probe is driven from the heartbeat for every engine, so a non-Unity second app is
covered by construction; in `husk-20261016-130343` the swapped-in app was the Java driver's, and the probe
fired for it (`660`).

## 5. The refused landscape request

The log named no culprit:

```
521  [238009.51ms][ui] orientation change refused (attempt 1): None of the requested orientations are supported by the view controller. Requested: landscapeLeft, landscapeRight; Supported: portrait
522  [238313.12ms][ui] orientation change refused (attempt 2): None of the requested orientations are supported by the view controller. Requested: landscapeLeft, landscapeRight; Supported: portrait
```

Two real defects, both now fixed:

1. **The guest's request was dropped.** The game asked for an orientation through the layer
   (`N_setOrientation` → `A.cfg.set_orientation` → the Swift host callback) and the callback only logged it
   (`TLUnityView.swift`, `java: the app asks for orientation <n>`). A game that asks for landscape therefore
   kept a portrait screen. The callback now maps Android's `ActivityInfo.SCREEN_ORIENTATION_*` to the mask
   iOS can be asked for — `0,6,8,11` (LANDSCAPE, SENSOR_/REVERSE_/USER_LANDSCAPE) → `.landscape`;
   `1,7,9,12` (PORTRAIT, SENSOR_/REVERSE_/USER_PORTRAIT) → `.portrait`; `2,4,10,13` (USER, SENSOR,
   FULL_SENSOR, FULL_USER) → `HuskOrientation.standard`; UNSPECIFIED/BEHIND/NOSENSOR and anything unknown →
   no change — and calls `HuskOrientation.set(_:app:)` on the main actor (`Task { @MainActor in … }`, the
   pattern the file already uses for guest callbacks).
2. **The refusal named nobody and asked the wrong chain.** `HuskOrientation.set` now takes `app:` and its
   refusal says which game asked and what it asked for:
   `orientation change refused (attempt N) for '<app>' (asked for landscapeLeft, landscapeRight): <UIKit error>`.
   It walks **every window of the scene** (not just `scene.keyWindow`), because the cover's own hosting
   controller is the view controller iOS consults and the key window's chain can miss it while the
   full-screen cover is still being presented — which is the ("Supported: portrait") refusal itself. The
   retry tail widened to 6 attempts × 0.4 s (the cover's presentation is ~0.35–0.5 s), and if the retries are
   exhausted the log says so outright, naming the app, what it asked for, and what the screen is actually
   showing (`describeCurrent()`), instead of stopping silently.

## 6. Deliberately not done

- **No speculation about the freeze.** The instrument is made honest and capable; the next run names the
  cause. The `083442`/`130343` shape (one frame then silence, no `FATAL`, engine startup reached) still
  points at the arena/exec path, but nothing here asserts it.
- **No lock, thread, mapping or guard was touched.** No `vm_protect` on the region, the executable view is
  never written, nothing the guest holds is unmapped, no thread that ran guest code is ended or joined
  (quarantine only), the honest refusal path is intact, the `f96a98a` view rule (`ss_where`) is untouched,
  the `77d0d53` arena-size setting is untouched, and the codewrite store guard is exactly as it was (the
  evidence run measured `carried=0 refused=0 relro=0` through both apps — nothing here disturbs it).
- **No new session mechanism.** The ledger reads the census epoch; it does not introduce one.
- **The `[swap] <id>: leaked` line printed by the Swift swap record against the NEW app's id while quoting
  the PREVIOUS close's detail** (log line 636) is a caller-side labelling quirk (`GameStatus.swift`), not a
  measurement. It was left alone; it is named here so the next reader does not read it as a second close.
