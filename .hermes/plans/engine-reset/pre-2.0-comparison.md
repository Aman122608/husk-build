# pre-2.0 vs 2.0 guest memory: what the old layer did differently, and whether a74459a restores it

Task: test the owner's datum that Husk pre-2.0 ran robot gf smoothly, by comparing how
`src/translation-layer` (pre-2.0) and `src/translation-layer-next` (2.0, shipping) provide guest
memory and where guest data lives; establish whether pre-2.0 ever trapped a guest store; name the
single thing it did differently; judge whether that arrangement is still viable beside the
debugger's execute grant; and check whether `a74459a` is complete or leaves other guest-writable
pages on the executable view.

**Verdict in one line: pre-2.0 had NO store guard and NO executable view for guest data at all —
it kept the guest's writable memory genuinely writable (carved out of its executable region as
ordinary memory) and never intercepted a guest `mmap`; 2.0 made the guest-facing view the
debugger's EXECUTABLE view (the only view carrying the per-entry execute grant), so every store into
a guest-facing span faults into `husk-tl-codewrite.c`. `a74459a` restores the old property for the
one branch the measured storm came through, but the arrangement it treats is broader than that
branch, so the fix is a targeted patch, not the whole reset.**

Date: 2026-10-16. Evidence: the layer sources at `a74459a`, and the device log
`HuskLogs/husk-20261016-093427-2.0.0-112.log`.

---

## 0. The honest framing first: which layer ran robot gf

The owner's datum is his memory of smoothness and it is treated as specification. Our code says
something narrower, and that discrepancy is part of the finding:

- **`src/translation-layer-next` is the Unity/IL2CPP path — in every tree this repo contains.**
  `src/app/Husk/TranslationLayer.swift:701` states it outright: *"Unity and cocos2d-x games run
  through the native runtime (src/translation-layer-next); everything else through the older
  prototype loader."* `src/app/Husk/TLUnityView.swift:366` dispatches `case .unity:
  husk_unity_launch(...)`, and `husk_unity_launch` lives in `src/translation-layer-next/
  husk-tl-unity-app.c:631`. The same wiring is present at `33cd42a^` (the pre-promotion tree) and at
  the root commit — Unity games have not run through `src/translation-layer` in this lineage.
- **`src/translation-layer` calls itself what it is**: *"This is an attempt, not a runtime"*
  (`src/translation-layer/husk-tl-load.c:26`), and its CI comment agrees — *"the older one is where
  the loader and the bionic shim live"* (`.github/workflows/check-translation-layer.yml:67-70`).
  The only Swift that calls it is the prototype/attempt path
  (`TranslationLayer.swift:680 husk_tl_attempt_start`).

So the loader in `src/translation-layer` is **not** the thing the owner ran robot gf on. Two
candidate referents for "pre 2.0":

1. **The pre-translation-layer engine: QEMU.** `docs/04-translation-layer.md:4-6`: *"Husk runs an
   APK by booting a whole Android system under QEMU's TCG and showing one app's surface. It works,
   and before the first frame it costs a kernel, an init, `system_server`, zygote and minutes of
   emulated CPU."* Under QEMU's TCG the guest's memory is a software MMU's memory: a guest store is
   handled by TCG, never by a host signal, and the JIT code cache is separate from guest data. **A
   store guard is structurally impossible there.** This is the strongest candidate for "ran it
   smoothly" (steady, no trap storm).
2. **The old app's own Unity path**, whose history is not in this repo (33cd42a replaced the tree).

Either way the substantive answer is the same, and it is the one the two in-repo layers let us
prove: the trap storm is a property of the *2.0 arrangement*, and pre-2.0 had neither the
arrangement nor the guard.

---

## 1. How `src/translation-layer` (pre-2.0) provides guest memory

### 1.1 One executable mapping, writable pages carved out as ordinary memory

`src/translation-layer/husk-tl-load.c:11-13` (the loader's own summary):

```c
 *   3. executable pages come out of one MAP_JIT region and writable pages
 *      are carved out of it as ordinary memory (the layout husk-tl-probe
 *      measures on the device), so code finds its data PC-relatively
 *      while the data stays writable during execution;
```

The allocation and the carve, `husk-tl-load.c:1276-1332`:

```c
    /* Allocate executable memory: try StikDebug prewarmed dual mapping first
     * (required on TXM devices), falling back to plain MAP_JIT. */
    ...
    if (!base) {
        jit_init();
        base = mmap(NULL, npages * TL_PAGE,
                    PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        ...
        base_rw = base;
        jit_writable(true);
    }

    size_t carved = 0;
    if (!is_stikdebug) {
        for (size_t i = 0; i < npages; i++) {
            if (flags[i] & TL_PAGE_W) {
                void *target = base + i * TL_PAGE;
                void *r = mmap(target, TL_PAGE, PROT_READ | PROT_WRITE,
                               MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
                if (r != MAP_FAILED) {
                    flags[i] |= TL_PAGE_CARVED;
                    carved++;
                } ...
```

- **Guest CODE** lives in the JIT region at `base` (`PROT_READ|PROT_WRITE|PROT_EXEC` under MAP_JIT,
  or an RW/RX StikDebug dual mapping).
- **Guest DATA** lives in **ordinary `PROT_READ | PROT_WRITE` memory at the same address**, obtained
  by `MAP_FIXED`-replacing each `TL_PAGE_W` page inside the executable region (`TL_PAGE_CARVED`).
  `husk-tl-load.c:1433` reports it: *"load: %s at %p, %zu pages (%zu pages carved as data)"*. The
  probe that measured this on device is `husk-tl-probe.c:445-500` (*"a library's code and data can
  sit side by side"*).
- On the StikDebug path, `base`/`base_rw` are already two mappings; the carve is skipped and the RW
  alias is used (`husk-tl-load.c:1335-1338`). Either way, writable data is in a **genuinely writable
  mapping**.

### 1.2 No `mmap` shim: the guest's own memory is the host's memory

The equivalent of `b_mmap` in pre-2.0 is **nothing**. `mmap`, `mmap64` and `mprotect` appear
nowhere in `src/translation-layer/husk-tl-shim.c` — the shim's export table
(`husk-tl-shim.c:378+`, `g_exports[]`) has logging, ANativeWindow, ANativeActivity, AInput,
AAsset, EGL-resolver, JNI, and the libc passthroughs where the two ABIs agree, and **no allocator
or memory-management calls**. Every `mmap` in that directory is the layer's own:

```
src/translation-layer/husk-tl-load.c:1296   base = mmap(NULL, npages*TL_PAGE, ..., MAP_JIT, ...)   /* the JIT region */
src/translation-layer/husk-tl-load.c:1315   mmap(target, TL_PAGE, PROT_READ|PROT_WRITE, MAP_FIXED) /* the carve */
src/translation-layer/husk-tl-probe.c:454,473,518,527                                            /* self-tests */
src/translation-layer/husk-tl-zip.c:128
```

So the guest's own `mmap` — its allocator, its buffers, its arenas — went straight to Darwin and got
**ordinary, natively-writable host memory**. There was no path by which a guest's own writable span
could land in the executable region, and no code that would have put one there.

### 1.3 Was there a store guard? No — and nothing like one

There is **no store guard anywhere in `src/translation-layer`.** The only signal handlers in the
whole directory are:

- `husk-tl-probe.c:155-190` — a `SIGWINCH` handler, installed and removed around the TLS probe, to
  check `tpidr_el0` inside a handler.
- `husk-tl-probe.c:382-419` — `guard_arm`/`guard_disarm`, installing `SIGBUS`/`SIGSEGV`/`SIGILL`
  **only for the duration of one probe self-test**, whose handler `siglongjmp`s back into the probe
  (`guard_handler`, `:387`). It is a test harness, armed and disarmed inside the probe, on the probe's
  own thread — not a runtime service for guest faults.

There is no `put()`-style store emulation, no `codewrite`, no per-fault decode, no writable-twin
carry. `husk-tl-load.c` has no fault path at all.

**So: did a guest STORE into its own memory ever fault into a store guard in that design? No.**
Two independent reasons, either of which alone is sufficient:

1. **The guard did not exist.** Nothing in the layer services a guest store fault.
2. **There was nothing to fault on.** The guest's writable library pages were carved into plain
   writable memory, and the guest's own `mmap` memory was plain host memory. No writable page the
   guest could store to was in an executable mapping.

---

## 2. How `src/translation-layer-next` (2.0) provides guest memory

### 2.1 One set of pages, two views at a fixed delta; the guest holds the EXECUTABLE one

`husk-tl-xmem.c` builds the arena and hands the guest one of the two views. The state
(`husk-tl-xmem.c:40-59`):

```c
    /* True when the guest was handed the EXECUTABLE view of the debugger's region, held RX: its code arena executes and
     * the layer will NOT write it, so a store there faults and is carried through the writable twin ... */
    bool wx_fallback;
    /* The WRITABLE view the layer emits its OWN bytes through ... in the W^X fallback it is the debugger's
     * writable vm_remap instead, and tl_xmem_delta() reaches it. */
    uint8_t *wrw;
```

`adopt_exec_view`, `husk-tl-xmem.c:882-889` — the three assignments that ARE the arrangement:

```c
    g_x.rx = m->rx_addr;             /* guest code executes here */
    g_x.rw = m->rx_addr;             /* the ONE address the guest both writes and executes (RX) */
    g_x.wrw = m->rw_addr;            /* the writable twin: the layer's bulk emission AND every guest store */
```

and the guest-facing view is the delta's origin:

```c
static uint8_t *xmem_writable_view(void) { return g_x.wrw ? g_x.wrw : g_x.rw; }   /* :163 */
ptrdiff_t tl_xmem_delta(void) { return xmem_writable_view() - g_x.rx; }           /* :734 */
```

The device log agrees verbatim (`husk-20261016-093427.log`):

```
147  xmem: open: StikDebug's prewarmed region -- guest arena 0x117ba8000 is the EXECUTABLE view 0x117ba8000 (RX) ...
153  xmem: arena ...: guest-facing view 0x117ba8000 is the executable view, handed to the guest as RX ...
156  xmem: guest-facing view 0x117ba8000 IS the EXECUTABLE view (RX) ...
```

So in 2.0 the guest's **guest-facing view is the executable view**, the writable twin is the
debugger's `vm_remap` at `+658866176`, and any store into the guest-facing view faults and is carried
through the twin by `husk-tl-codewrite.c` (`put()`, `:69-75`).

### 2.2 When and why the guest's writable memory started being served from the executable view

Not in `a74459a`, and not even in the RW-only branch originally. The history, oldest first:

| commit | what it did |
|---|---|
| `33cd42a` (Oct 12) | promote 2.0 to the repo root; the tree already carries `b_mmap`'s RW-only branch returning `rx` |
| `18da8b3` (Oct 13) | **"xmem: hand the guest the view that executes, not the alias that does not"** — adds `adopt_exec_view` and hands the guest the EXECUTABLE view |
| `2a1231f` | **"xmem: hand the guest the executable view"** — fixes the fallback to actually give the guest that view |
| `acce1b8` | removes the layer's own `vm_protect` (it was revoking the debugger's grant) |
| `f071c9c` | *"io: a read into the JIT region's executable view answered EFAULT, because the guest's data lived there"* — the first symptom of data living on the executable view |
| `fd11319` (today) | **"the 147k store-guard traps are the guest's writable mmap handed the executable view"** |
| `a74459a` | "hand guest writable mappings the writable view, not the executable one" |

The **why** is in `18da8b3`'s own message, and it is a platform fact, not a choice:

```
The EXECUTABLE view ran generated code and its mappings survive the debugger detaching. The writable
view -- the vm_remap alias we had been handing the guest -- faulted when a page of it was made
executable and fetched. iOS authorises execute per mapping ENTRY, and an alias never inherits it.
```

and `2a1231f`:

```
That is why the guarded fetch, not the read-back, is the only verdict -- and why the guest must be
handed the view whose defining mapping carried the blessing.
```

**So the guest's writable memory is served from the executable view because the debugger's execute
grant lives on the defining mapping ENTRY of ONE view, and iOS will not let the alias fetch.** From
`18da8b3`/`2a1231f` on, *whatever the guest can execute must be addressed at the executable view* —
and because the layer then introduced/kept `b_mmap`'s RW-only branch returning `rx` (a branch that
hands out a slice of that same arena), the guest's own anonymous writable spans inherited it.

### 2.3 Is it the one branch, or broader? — Both, and that matters

- **The measured storm is the one branch.** `fd11319` and `data-out-of-the-executable-view.md` §2
  pin it by addresses: the writes at `0x11f1bc000` have their twin inside the span handed out at
  `0x146614000`, immediately after the library images, and the branch's own line
  (*"a JIT's code memory (mmap hinted near the code) comes from the JIT region"*) fired. Two
  independent negative controls in the same log, both **zero occurrences**:
  - `mm: <n> bytes of executable memory for the guest` → **0** (`b_mmap`'s `PROT_EXEC` branch never fired);
  - `xmem exec on the writable view` → **0** (the guest never `mprotect`ed any of it).
  Guest `mmap` count for the whole run is **64**; arena allocations **48**; carried stores
  **29,538,738**.
- **But the arrangement is broader than that branch.** See §4.

---

## 3. The diff, and the single thing pre-2.0 did differently

| | pre-2.0 (`src/translation-layer`) | 2.0 (`src/translation-layer-next`) |
|---|---|---|
| executable memory | one MAP_JIT mapping (or StikDebug RW/RX dual map) | one set of pages, two views at fixed delta (guest holds RX, layer writes the twin) |
| guest data in a library | **carved** into ordinary `PROT_READ\|PROT_WRITE` memory at the same address | placed at `L->rw` = the writable twin (a `vm_remap`) — **unchanged, and correct** |
| guest's own `mmap` memory | not intercepted at all → ordinary host memory | `b_mmap` HLE; hinted RW spans ≤32 MiB near the region → a slice of the arena |
| which view the guest's own writable span names | n/a — it is ordinary memory | **the EXECUTABLE view** (before `a74459a`) |
| store guard | **none exists** | `husk-tl-codewrite.c`: decode + twin carry per fault |
| a guest store into its own writable memory | native store, no fault, ever | a fault, a decode and a twin store per 16 bytes |

**The single difference: pre-2.0 kept every guest-writable page in a genuinely writable mapping and
named it as such; 2.0 names the executable view as the guest-facing view for the whole arena, so a
guest pointer into its own writable memory names an RX page.** Concretely, pre-2.0's carve
(`husk-tl-load.c:1315`) is the exact analogue of what `a74459a` now does by *view* instead of by
mapping: both make the address the guest stores through genuinely writable. The tabulated change
that introduced the cost is `2a1231f/18da8b3` (`g_x.rw = m->rx_addr`), and the change that exposed
it to the guest's own allocations is `b_mmap`'s `return rx` in the RW-only branch.

Note what did **not** change and was never broken: library `.data`/`.bss` has been at `L->rw` in both
layers (`husk-tl-ld.c:289`, `:1429-1438`; `tl_page_plan`'s `TL_PAGE_W`), and stores there are native
(`data-out-of-the-executable-view.md` §1). The loader's per-segment split is not the problem and
never was.

---

## 4. Is the old arrangement still viable, and is `a74459a` complete?

### 4.1 Still viable — and 2.0 already does it for library data

**The decisive code**: `tl_xmem_alloc` already returns the writable view as the second out-parameter,
and the loader has always used it.

```c
/* husk-tl-xmem.c:654-655 */
        if (rx) *rx = g_x.rx + start;
        if (rw) *rw = xmem_writable_view() + start;
```

That `*rw` is the debugger's writable twin, and `map_library` places every `TL_PAGE_W` page at it
(`husk-tl-ld.c:1967`, and the data-pointer rule at `:289` / `:1429-1438`). **So the writable view
exists, is genuinely writable, and the pre-2.0 behaviour for library data is not merely possible —
it is what 2.0 already ships.** `a74459a` extends exactly that to the guest's own RW mappings
(`return rw;` where it used to `return rx;`, `husk-tl-bionic-io.c:1832-1833`).

Nothing about this touches the debugger's grant: the layer makes **no `vm_protect` call on the
region** (`adopt_exec_view`'s whole point, `husk-tl-xmem.c:854-880`) and naming a different pointer
is not a protection change. **So: (a) still viable today, alongside the debugger's execute grant** —
the old arrangement is recoverable by view-selection, without the old `MAP_FIXED` carve.

**One qualification, which is (b) and not (c):** the arrangement is viable *for non-JIT titles*.
A guest that writes machine code into that memory and then `mprotect(PROT_EXEC)`s it needs the
EXECUTABLE view, because the writable twin is a `vm_remap` alias and iOS will not let an alias fetch
(`tl_xmem_allow_exec` → `EACCES`, `husk-tl-bionic-io.c:1905-1913`). Robot gf is AOT IL2CPP and never
`mprotect`s the region (log: `xmem exec on the writable view` ×0), so it gets the fast path;
LuaJIT-style titles do not. `a74459a` names that honestly (`rw_exec_refuse_log`) and remembers it
per package/version (`.husk-jit-writable`).

It is **not (c)**: the grant mechanism fixes *which view can execute*, not *which view the guest's
data pointers name*. Pre-2.0 is recoverable — as a rule about where writable pointers point, not as
a revived `MAP_FIXED` carve.

### 4.2 `a74459a` is a targeted patch, not the whole reset

For the **measured** storm, it should be complete: the log pins all 29.5M carries on the RW-only
branch (the two negative controls above), and `a74459a` changes precisely that branch's return.

But the property **"guest-writable pages are served from the executable view"** is broader than that
one branch, and these are still live:

1. **`b_mmap`'s `PROT_EXEC` branch still returns `rx`** — `husk-tl-bionic-io.c:1796-1804`:
   ```c
   if ((prot & PROT_EXEC) && (flags & 0x20) && !(flags & 0x10) && tl_codewrite_enabled()) {
       ...
       if (tl_xmem_alloc(n, &rx, &rw)) { memset(rw, 0, n); ... return rx; }
   }
   ```
   This is intentional (it is a request for *code* memory, and the store guard is the intended
   service) and it never fired for robot gf (`mm: … of executable memory for the guest` ×0), but it
   is a second place guest memory is served from the executable view.

2. **The loader's address-provenance hands data through the executable view for any page not marked
   `TL_PAGE_W`** — and `TL_PAGE_W` deliberately excludes the whole `PT_GNU_RELRO` range
   (`husk-tl-elf.c:211-223`; `TL_PAGE_W` is only marked outside `[r0, r1)`), which is why
   `data-out-of-the-executable-view.md` §3.8 records that *"a store into `.data.rel.ro` today goes
   through the guard"*:
   ```c
   /* husk-tl-ld.c:284-290 */
   if (!is_code && page < L->npages && (L->pflags[page] & TL_PAGE_W)) return L->rw + off;
   return L->rx + off;
   /* husk-tl-ld.c:1434-1438 */
   if (page < L->npages && (L->pflags[page] & TL_PAGE_W)) { ... return (uint64_t)(uintptr_t)(L->rw + off); }
   return (uint64_t)(uintptr_t)(L->rx + off);
   ```
   A RELRO (or otherwise non-`TL_PAGE_W`) page the guest writes still names the executable view and
   still traverses the store guard. Zero for a correct guest, but it is a live route.

3. **The library is described to the guest at `L->rx` in three places**, so a guest that derives a
   *data* address from any of them names the executable view rather than the writable twin:
   - the synthesized `/proc/self/maps` lists each library as **one** `r-xp` span at `L->rx`, code and
     data together — `husk-tl-ld.c:685-686`:
     ```c
     uintptr_t lo = (uintptr_t)L->rx, hi = lo + L->npages * PAGE;
     k += snprintf(buf + k, n - k, "%lx-%lx r-xp 00000000 fd:00 %d %s/%s\n", ...);
     ```
     (This is also *why* the guest's allocator hovers near the region and hits the RW-only branch at
     all — `data-out-of-the-executable-view.md` §2.)
   - `tl_ld_lib_base` returns `L->rx - L->base_vaddr` (`husk-tl-ld.c:606`), and
     `bionic_dl_iterate_phdr` / `bionic_dladdr` report it as the base
     (`husk-tl-bionic.c:787`, `:802`), so `dlinfo`/`dladdr`-derived data addresses land at `L->rx`.

None of (2)-(3) is touched by `a74459a`. If the guest reaches data through them, the counter will
show a **small residual, not zero** — which is the reading to watch on the next run. The storm
itself should collapse; the arrangement that produced it is only half-dismantled.

### 4.3 Invariants

Unchanged and re-checked after the analysis: no `vm_protect` on the region (none added, none
needed — `adopt_exec_view` issues none); the executable view is never written (a pointer is chosen,
not written to); nothing the guest holds is unmapped; no guest thread is ended or joined; the honest
refusal path is intact (`tl_xmem_allow_exec` → `EACCES`, named once by `rw_exec_refuse_log`); the
store guard remains for genuine code writes (`husk-tl-codewrite.c` untouched by this pass).

---

## 5. Readings for the next run

`tl_perf_summary` already prints everything needed (`codewrite carried=… refused=…; arena allocs=…
pages=…`).

| reading | today (`husk-20261016-093427`) | expected after `a74459a` |
|---|---|---|
| `codewrite carried=` | 29,538,738 (~147,000/s) | the genuine code writes — robot gf self-modifies nothing: **0–100** for the run |
| `codewrite refused=` | 0 | **0** (must not rise) |
| `arena allocs= / pages=` | 48 / 60,088 | unchanged (same pages, different view) |
| `guest mmap` (heartbeat) | 64 | unchanged |
| frames / FPS | ~1035, ~40 FPS | above 40 FPS |
| load to the main menu | ~49 s | shorter |

**If `carried` does not collapse to ≤10³, the storm is not the mmap view** and §4.2's routes are the
cause; **if it collapses to a small non-zero number**, that residual is route (2)/(3) and the fix is
still partial. Either way the counter kills or confirms this without a guess.

---

## 6. What this pass changed

No code. This document only. The analysis is a reading of `src/translation-layer`,
`src/translation-layer-next`, the device log, and the commit history (`18da8b3`, `2a1231f`,
`acce1b8`, `f071c9c`, `fd11319`, `a74459a`).
