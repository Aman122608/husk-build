# Where robot girlfriend's time goes

Run: `husk-20261015-135308-2.0.0-112.log` (479 lines), `com.IncontinentCell.MyDystopianRobotGirlfriend`,
its own IL2CPP, max 1035 frames, ~40 FPS by the owner's report. Engine window ~166130 ms → ~230094 ms (~64 s of app
run). Getter: ANGLE over Metal (Apple A19 Pro), 6 big cores.

## Counts (from THIS log — no claim below is a guess unless labelled inference)

| thing | count | how |
|---|---|---|
| store-guard carries ("store into the executable view … carried through the writable twin") | **4** | 4 lines, lines 301–304 |
| store-guard refusals | **0** | no `codewrite: REFUSED` lines |
| arena backing spans | **48** | `xmem: arena page backing: backed the span …` |
| arena pages backed (4 KiB) | **60 088** | sum of the per-span page counts |
| largest spans | 22 928, 4 800, 4 564, 4 096 ×4 pages | |
| `unity: alive` heartbeats | 54 | |
| `egl: swap` lines | 6 | |
| swap-stalls | **2** | 8.1 s @ frame 121, 9.0 s @ frame 580 |
| audio frames written | 2 933 760 | `audio close` line |
| guest threads live at close | 33 → 35 | swap-stall / close lines |

### The store guard — the log CANNOT answer this, and that is the finding
The carry line is **throttled to the first four** (`husk-tl-codewrite.c`, `if (said++ < 4)`), so the log proves only
**≥ 4 carries**, all in one 16-byte-stride burst at `0x11c6e8000..+0x40` around frame 120 (JIT code emission). The
guard *does* count every carry (`g_emulated`, `tl_codewrite_count()`) but that number is never summarised.

**Per-second trap estimate from the log: ≥ 4 / ~64 s ≈ 0.06 traps/s — a floor only.** The true rate is unanswerable
from this log; the throttle, not the count, is what the log shows. That is exactly why a summary counter was added.

### Hottest intercepted calls
The log prints **no per-call line** for any intercepted syscall — `fstat`, `clock_gettime`, `gettimeofday`, `read`,
`pthread_*` all have **zero** occurrences. So the answer is a labelled inference plus a counter:
- **inference (high confidence):** `clock_gettime` / `gettimeofday` are the per-frame hot pair (Unity frame timing +
  its Engine timer thread), with `pthread_mutex_lock/unlock` and `pthread_cond_wait` close behind (33 guest threads on
  6 cores), and `read`/`pread64` dominating the *load* phase (the `husk: apk entry … read in place` lines).
- the log's own traffic supports the split: 72 `ld:` lines, 68 `xmem:`, 54 heartbeat, 32 `husk:`, 14 `egl:`.

## Per-allocation arena backing overhead (`tl_xmem_alloc`)
Per allocation the pass does, **per 4 KiB page of the span**:
1. a **volatile store** through the writable twin — **required**; this is what makes the kernel back the page and
   (being volatile) cannot be elided as a dead store; and
2. a **read-back** through the same view, folded into a checksum — **debug only** (the honest fix for the dead-store
   bug: a page that was never backed faults *here* instead of later in the guest).

Per allocation additionally: **two `xmem_guarded_read`s** of the span's first page (each installs + restores three
signal handlers → **6 `sigaction` calls per allocation**), and one ~700-byte log line.

Magnitude for this run: **60 088 read-backs + 96 guarded reads (≈576 sigaction calls) + 48 big log lines**, all pure
proof, none required for the pages to be backed. The reads roughly **double** the pass's memory traffic; the guarded
reads are the expensive per-alloc part (syscalls, on 48 allocations). The volatile store loop itself — the only part
that must stay — is unchanged.

## Ranked optimizations (expected effect vs measured count, and risk)
1. **Gate the backing read-back + guarded reads to debug (DONE).** Removes ~60 k dependent loads and ~576
   `sigaction` calls per run; halves the pass's traffic; kills 48 big log lines; stops 6 signal-handler
   install/restore per allocation (which can perturb other threads). *Effect: small in CPU (backing is tens of ms),
   but it is the clearly-safe win and was self-inflicted. Risk: none — the volatile store loop that actually backs
   the pages is NOT gated.*
2. **Summarise store-guard carries/refusals (DONE — counter).** Turns "≥4, unknown rate" into a number, so the next
   decision (does the guard need work?) is measured. No perf change by itself.
3. **Count intercepted calls by name + arena allocs/pages (DONE — counter).** Answers "which shim is hot" and
   "allocations per frame", which the log cannot.
4. **Store-guard behaviour change / JIT scope / arena layout — NOT implemented.** All could change semantics (the
   guard's decode, the region's layout, the execute-grant scope) and must be provable from the code first; and item 2
   has not yet returned the number that would justify them. Describe, do not guess.
5. **Engine-side, from the log (not ours to change here):** `W/Unity: Hardware resolution scaling not supported,
   falling back to software scaling (blit)` — a real per-frame cost; and the **695 MB APK's `libil2cpp.so` (91 MB) +
   `libunity.so` (18 MB) are `deflated (inflated once)`** on the load path — the most likely "slow loading" term.

## The two 8–9 s stalls
Both are **the guest's own first-frame / asset path**, and the log's own verdict says so: layer locks all `free`,
no parked thread, `0` threads from an earlier session. Stall 1 (frame 121) sits on IL2CPP bring-up (91 MB + 18 MB
inflated once from the APK, then the big arena backing spans). Stall 2 (frame 580) sits on the save/mod bootstrap:
`localmodinfo.json` / `PlayerPrefs.pp` / `save.mdrg` are **created new** (the run had no save), then state →
`GameScript+MainMenuState`. So the stall is the guest's own asset I/O **as served by Husk** (inflate-once + arena
backing + vfd reads), not the swap quarantine.

## What changed (implemented)
- `husk-tl-xmem.c` / `.h`: backing **read-back + checksum + the two guarded reads gated behind `TL_XMEM_BACKING_TRACE`**
  (off by default); the volatile store loop is never gated; a short per-alloc line replaces the big one when off;
  counters `g_x_alloc_calls` / `g_x_alloc_pages` and `tl_xmem_backing_stats()`.
- `husk-tl-codewrite.c` / `.h`: `tl_codewrite_refused_count()` (the carry counter already existed).
- `husk-tl-bionic.h` / `.c`: per-call counters `tl_hp_count[]` + `tl_hp_name()`, and `tl_hp_bump()` at the top of the
  wrapping entry points (relaxed atomic add, no lock, no allocation).
- `husk-tl-bionic-io.c`: bumps on `clock_gettime`, `gettimeofday`, `read`, `pread64`, `fstat`, `fstatat`, `open`, `mmap`.
- `husk-tl-bionic-pthread.c`: bumps on `mutex_lock`, `mutex_unlock`, `cond_wait`.
- `husk-tl-unity-app.c`: `tl_perf_summary()` — **one line per 8 heartbeats (~20 s) and once at session close**, never
  per event; prints carries/refusals, arena allocs/pages, and the calls by name.

## What was deliberately NOT done
- No change to the store guard's behaviour, the arena's layout, or the JIT scope — each could change semantics and
  none is justified before the new counters return a number. All invariants held: no `vm_protect` on the region, the
  executable view is never written, nothing the guest holds is unmapped, no thread that ran guest code is ended or
  joined, the honest refusal path is intact.
