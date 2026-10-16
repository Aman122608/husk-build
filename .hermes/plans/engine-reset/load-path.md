# The load path to robot gf's main menu: what inflates, what re-reads, and which phase is the minute

Scope: the ~60 s from launching **Factorial Omega: My Dystopian Robot Girlfriend**
(`com.IncontinentCell.MyDystopianRobotGirlfriend`, Unity 6000.3.8f1, IL2CPP, arm64) to its main menu.
Files touched: `husk-tl-bionic-io.c`, `husk-tl-ld.c`, `husk-tl-ld.h`, `husk-tl-bionic-ndk.c`,
`husk-tl-internal.h`. Nothing under `tools/a32`, and no other agent's file, was touched.

Device log read: `HuskLogs/husk-20261016-093427-2.0.0-112.log` (541 lines).

---

## 1. The minute, measured from the log

The log puts the whole load on a clock:

| marker | line | time |
| --- | --- | --- |
| `==== new app: …Factorial_Omega…apk` (launch) | 127 | **139 001.49 ms** |
| `launch com.unity3d.player.UnityPlayerActivity` | 265 | 139 239 ms |
| `ld: …App 18.6 MiB … 28333 relocs` (libunity) | 274 | — |
| `ld: …App 89.5 MiB … 583789 relocs` (libil2cpp) | 280 | — |
| `egl: swap #1` / `unity: alive: 1 frames` | 303-305 | ≈139 800 ms |
| first `swap-stall` (8.1 s, at frame 123) | 385 | — |
| `assets/aa/settings.json`, `assets/aa/catalog.bin` read | 394-398 | — |
| second `swap-stall` (9.0 s, at frame 552) | 462 | — |
| `GameScript+MainMenuState` / `[status] …: plays` | 480-482 | **188 367.02 ms** |

**139.0 s → 188.4 s = 49.4 s to the main menu.** That is the owner's "solid minute".

Counts over the run (`perf[close]`, line 539): `read=8382`, `open=1171`, `fstat=356` — so the minute is
**not I/O-bound**. The clock lines the log does have say the two 8–9 s stalls (17.1 s) sit on the guest's
own first-frame/asset path.

One number dominates everything else in that same line:

```
perf[hb]  (line 437, mid-load):  codewrite carried=12 794 550  refused=0
perf[hb]  (line 464, just before main menu): codewrite carried=28 236 715  refused=0
perf[close] (line 539):                      codewrite carried=29 538 738  refused=0
```

`g_emulated++` is incremented **once per signal** in `on_fault` (`husk-tl-codewrite.c:412`): every
"carry" is a SIGBUS/SIGSEGV round-trip through the executable view's twin. ~28.2 M carries before the
main menu (~575 k/s across the 49 s) is the dominant load-path term by orders of magnitude; a sibling
change measured the same 29 538 738 total over the whole run and traced its source to `b_mmap`'s RW-only
divert handing guest writable memory the **executable** view (`husk-tl-bionic-io.c:1534`;
`.hermes/plans/engine-reset/data-out-of-the-executable-view.md`), i.e. the guest's own writes to its
writable mmaps fault every time. It is the same term the owner reports as "40 FPS"; the guard is out of
scope here (the perf pass, `performance.md` §"what was deliberately NOT done", left its behaviour alone).
It is named here because it is what the log's own numbers say the minute is, and it must not be mistaken
for the inflate — which is what this task set out to cut, and which is real but ~1 s.

## 2. Every inflate / re-read on the load path

| site | file:line | what it does | cached today? | per session / per launch |
| --- | --- | --- | --- | --- |
| `jar_inflated` (the `jar:file:` path) | `husk-tl-bionic-io.c:830` | inflates a **deflated** APK entry into `<data>/.jar-cache/<name>`; a stored entry is served in place | **yes**, but the key was only the entry name + a size check | cached once; a hit is free |
| `fetch_from_apks` (the loader) | `husk-tl-ld.c:214` | reads `lib/arm64-v8a/<name>` out of the APKs for `map_library` | **no** — `tl_zip_data` inflated on every load | **every launch** |
| `fetch_from_file` (the loader) | `husk-tl-ld.c:391` | reads a library the app already unpacked (Unity's SoLoader writes `libil2cpp.so`/`libunity.so` out as `<data>/…/App`) | n/a (plain file read) | every launch, a full read |
| `b_AAssetManager_open` | `husk-tl-bionic-ndk.c:295` | the NDK asset surface (`AAssetManager`): the Addressables catalogue and bundles | **no** — inflated on every open | **every open** |

The two library reads that matter for robot gf, per the log's own report (line 121):

* `lib/arm64-v8a/libil2cpp.so` — 91 567 688 B, **`compressed:true`** (deflated)
* `lib/arm64-v8a/libunity.so` — 18 066 480 B, **`compressed:true`** (deflated)
* together **109.6 MB / 104.5 MiB deflated per launch**

and the assets the game actually walks first are **stored**, not deflated — so the Addressables
catalogue costs nothing to inflate:

```
husk: apk entry assets/aa/settings.json: 854 bytes, stored (read in place)
husk: apk entry assets/aa/catalog.bin:  129422 bytes, stored (read in place)
```

(the only deflated library on robot gf's *loader* path is `libLive2DCubismCore.so`, 75 680 B.)

There is **no layer-side content hashing on this path**: `gnu_hash`/`sysv_hash`
(`husk-tl-ld.c:241`) are for symbol lookup and are memoised in `L->symcache`; the only content hash
in play is the guest's own BoringSSL FIPS self-check, which the loader already skips
(`husk-tl-ld.c:2132`). So "stop hashing what does not need hashing" is **already satisfied** — nothing
was changed for it, and nothing needs to be.

## 3. What was implemented

### 3a. Timed, not guessed: a load-phase split (`load[close]`)

`husk-tl-internal.h` gains a small enum and three calls; `husk-tl-bionic-io.c` implements them and
prints one line at the session boundary via `tl_io_reset_for_new_app()`:

```
load[close]: inflate=…s/N call(s)/… MiB copy=… map=… asset=…  -- inflate is the CPU a cache removes; copy is a later launch reading the cache
```

Phases: `inflate` (zlib on an APK entry), `copy` (reading an inflated on-disk copy), `hash` (the layer
does none — the field exists so a future hash shows up as a number, not a suspicion), `map`
(`map_library` + `relocate` + adrp patching), `asset` (an `AAssetManager` open). Accounting is a few
relaxed atomics **at the load sites only** — never per frame, never per event, no lock, no allocation.

### 3b. Cache the inflated bytes: a later launch pays a copy, not an inflate

`tl_zip_data_cached()` (`husk-tl-bionic-io.c`) wraps `tl_zip_data()`: a **deflated** entry is inflated
once, written to `<data>/.lib-cache/`, and read back as a plain copy on every later launch; a **stored**
entry is still served in place. `fetch_from_apks` (`husk-tl-ld.c`) and `b_AAssetManager_open`
(`husk-tl-bionic-ndk.c`) now go through it, so the loader's library read and the NDK asset read both
stop re-inflating.

**Invalidation (the point of the design).** The cache-file name is a 64-bit FNV-1a over
*entry identity, size, and the app's own version*:

```
<data>/.lib-cache/<fnv1a(apk path, apk size, apk mtime, entry name, usize, csize)>.bin
```

so an updated APK, a rebuilt entry, or a re-zipped one yields a **different file name** and a stale
copy is never even looked at; the old file simply goes unused. Two further guards:

* the write is `<name>.part` + `rename()`, so a launch that dies mid-write can never leave a torn file
  (a rename only lands a complete one);
* the read is only trusted when `stat().st_size == usize` **and** `fread` returns exactly that many
  bytes; anything short is `free()`d and the entry inflated afresh. A corrupted cache is worse than a
  slow load, so the cache is only served when it is *exactly* right.

### 3c. `jar_inflated` invalidation strengthened

The existing `.jar-cache` was keyed on the entry name alone (slashes → `_`) and validated by size only —
same name, same size, different bytes after an in-place app update would have been served stale. It now
uses the same identity hash (APK path + size + mtime, entry, usize **and** csize, which needed one extra
field on `jar_entry`), keeping the entry name in the file name for readability.

### 3d. Invariants kept

Nothing the guest can see changed: the caches live under the app's own `TranslationLayer/<uuid>/`
directory, outside the guest's `java-data` view. No `vm_protect`, the executable view is never written,
nothing the guest holds is unmapped, no guest thread is ended or joined, and the refusal path is
untouched. The store guard was **not** modified.

## 4. Expected effect, and how the next run confirms it

| change | expected effect | confirmed by |
| --- | --- | --- |
| `.lib-cache` for loader + `AAssetManager` | removes re-inflate on the loader/asset paths. On a fresh install it removes the one-time **~109 MB inflate ≈ 0.6–0.9 s**; on every later launch it guarantees **0 s** of inflate where a stale/invalidated `jar-cache` would have cost that again. Robot gf's only deflated loader lib is 75 KB, so for *this* game, on *this* launch, the win is small. | `load[close]: … inflate=0.000s/0 call(s)` and `copy=…/2 call(s)/104.5 MiB` on the second launch |
| `jar_inflated` identity | no stale inflated copy after an app update (a correctness fix, not a speed one) | a `husk: apk entry … deflated (inflated once, then cached under .jar-cache)` line, then no re-inflate on the next launch with the same APK |
| `load[close]` instrument | no perf change; turns "which phase is the minute" into a number | one new line per session close |
| — the honest summary — | the cache is **~1 s**, not the minute. The log's own `codewrite carried≈28.2 M before the main menu` (≈48 s) is the dominant term, and it is the 40-FPS task's subject, not this one's. | `perf[close]: … codewrite carried=…`, `load[close]: …` together |

**To reach "seconds, not ~60", the term to attack is the store guard's per-store signal**, measured in
this very log; the inflate is real but small. This document is deliberately explicit about that rather
than claiming the cache is the fix.

## 5. Out of scope / follow-ups

* A one-time sweep of superseded `.lib-cache` / `.jar-cache` entries after an app update (they are never
  served, only left on disk) — bounded disk use, not a correctness issue.
* The store guard (`husk-tl-codewrite.c`) — the measured dominant term; belongs to the 40-FPS work.
