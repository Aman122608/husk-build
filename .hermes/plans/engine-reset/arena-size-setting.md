# JIT arena size: a user-facing setting, honestly reported

Owner's ask, in his words: *"how large is the jit allocation? can you add a setting for it if possible
if it could improve performance."*

## The facts this is built on

- The region is claimed at boot, before the guest download: `JITBootstrap.prewarm()` logs `claiming 512 MiB
  of JIT memory now, before the guest download` and calls `husk_ios_jit_prewarm(512 MiB)`.
- It comes up fully mapped and executable: the layer's arena walk logs `arena 0x117ba8000..0x137ba8000:
  512 MiB, 512 MiB MAPPED, 512 MiB executable` (`husk-tl-xmem.c`, `xmem_report_open`).
- robot gf's whole load peaks at ~185 MiB of it (`used=193,953,792` at its worst point, the fault-path line
  in `husk-tl-unity-app.c`).

## The honest frame

This is **not a performance lever**. The frame rate was capped by 147,000 trapped stores per second, and that
came from *which view* the guest's writable memory was named through, not from how large the region is — and
that is fixed (a74459a) and made a rule (f96a98a). A bigger region would not have made robot gf faster.

What the size setting *is* good for:

1. **Bigger titles.** The guest's own mmap/heap memory is served from this region, so a title needing more
   code+data+heap than the claim allows cannot run. GTA-scale titles are the obvious case.
2. **Low-memory devices.** A smaller claim can be the difference between running and being killed.
3. **Diagnosis.** A game that cannot get the memory it asks for should fail in a way the log names, and the
   user should be able to raise the number and try again.

## What was built

### The setting (app side, `src/app/Husk/JITBootstrap.swift`)

- `husk.jitArenaMiB` in `UserDefaults`, **default 512 MiB** (`defaultArenaMiB`) — today's claim, unchanged
  unless the owner changes it.
- `JITBootstrap.jitBytes` is now `arenaMiB * 1024 * 1024` (was a `static let` of 512 MiB). It is read at
  launch, in `prewarm()`: the region is claimed at boot, before the guest download, so **a change takes
  effect at the next launch**, which the Settings row, the Stepper, and the launch log all say plainly.
- The Settings row lives in `JITSettings` (`src/app/Husk/SettingsTab.swift`), right beside the existing
  **Guest arena** row (the honest indicator for this same region). It is a `Stepper` (64 MiB steps) plus a
  new **Claimed & measured** `DetailRow`.

### Range and clamping, derived from the code

- **Floor: 64 MiB.** `husk-tl-xmem.c` never claims a smaller region — `grant_self` halves the request down to
  `size >= (64u << 20)` and stops, and `near_alias`'s largest-fitting-alias loop stops at
  `want_size >= (64u << 20)`.
- **Ceiling: 2048 MiB.** The region is mapped **twice** (an executable view and a writable twin), and the
  layer places the twin within 2 GiB of the executable view — `reach = (uintptr_t)2 << 30` in
  `husk-tl-xmem.c` (`near`/`near_alias`) and in `src/ios-jit/husk-ios-jit.c`'s placement of the writable
  alias. A claim beyond 2 GiB could not keep its two views within that reach.

A value outside `64...2048` is clamped, with a **named log line** saying what was asked and what was used —
never silently:

```
arena size setting: asked 4096 MiB, but the layer's honest range is 64-2048 MiB (husk-tl-xmem.c: the region
is never claimed below the 64 MiB floor and its twin must stay within 2 GiB of the executable view), so this
launch uses 2048 MiB -- CLAMPED, not silently ignored
```

### Honest reporting (`JITBootstrap.arenaSizeDescription`)

The **Claimed & measured** row shows, in one place:

- what is configured for the **next** launch (`set to N MiB` … `takes effect at the next launch`), naming a
  clamp when one happened;
- what **this** launch actually claimed — from the layer, `husk_ios_jit_region_size()` (the size the debugger
  really granted, `0` when none is held), never the requested number shown as achieved;
- whether that came up **mapped and executable**, reusing the layer's own guarded write-then-execute verdict
  (`tl_xmem_exec_verdict` / `tl_xmem_exec_detail`, the evidence behind the existing **Guest arena** row). If
  it faulted, the row says so and points at the Guest arena row; it never asserts success;
- the **session high-water mark** of the region's use against the size it was measured against.

### The peak (layer side)

- `g_x.used_hwm` in `husk-tl-xmem.c`: the high-water mark of `used`, kept in `tl_xmem_alloc`; exposed as
  `tl_xmem_used_high_water()`. It is the same `used` the fault path already prints (`arena_page_span`,
  `used=... size=...`), read back at its worst — a measurement, not a new claim.
- `tl_xmem_report_high_water(tag)` logs one session-summary line, called once from `husk_tl_session_close`:

```
xmem: arena high-water[close]: the guest's memory peaked at 185 MiB used of the 512 MiB region claimed
(peak=193953792 bytes, size=536870912 bytes) -- a peak close to the claim says this title wants a LARGER
arena size in Settings; a peak far below it says a smaller claim would do
```

So the owner can see for himself whether his setting is too small, too large, or irrelevant for a title.

### Bridge surface (new, minimal)

- `husk_ios_jit_region_size(void)` — `src/ios-jit/husk-ios-jit.c` / `.h`, declared in
  `src/app/Husk/HuskBridge.h`. The size actually granted.
- `tl_xmem_used_high_water(void)`, `tl_xmem_report_high_water(const char *)` — `husk-tl-xmem.h` / `.c`
  (`husk-tl-xmem.h` is already included by `HuskBridge.h`).

## Invariants deliberately preserved

- **Default unchanged** at 512 MiB; nothing changes unless he changes it.
- **How the region is used is unchanged**: no change to allocation, the bump pointer, the backing pass, or
  which view is which.
- **No invariant relaxed**: no `vm_protect` on the region, the executable view is never written, nothing the
  guest holds is unmapped, no thread that ran guest code is ended or joined, the honest refusal path is
  intact, and the writable/executable view rule from f96a98a is untouched.
- **Nothing touched** in `tools/a32/` or any other agent's files.

## What was deliberately NOT done

- Did not change `tb-size` (QEMU's translation-block size, still 256 MiB) or how it is passed. The claim is
  handed over whenever it is at least `tb-size`; a value the owner picks below 256 MiB would stop serving
  the QEMU engine, and the footer could say so in a later pass. The floor was kept at the layer's own 64 MiB
  because that is what *the layer* will honour.
- Did not make the ceiling dynamic (from `husk_ios_available_memory()`); it is the static mechanical limit
  the code supports (2 GiB twin reach).
- Did not move any default, add any new allocation path, or claim a performance win.
