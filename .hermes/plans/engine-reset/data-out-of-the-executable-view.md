# Getting the guest's DATA out of the executable view — evidence, and why the ELF-loader split is not it

Task: cut the 147,000 store-guard traps/second (`codewrite carried=29,538,738` in ~200 s) that cap
robot gf at 40 FPS, by "splitting the guest's read-write data out of the executable arena into a
separate writable mapping".

**Verdict in one line: NOT LANDED — not because the split is hard to write, but because the
measurement says the traps are NOT the library's read-write segments.** The ELF loader already puts
every library's read-write data in a genuinely writable mapping (the debugger's writable twin,
`g_x.wrw`), and stores there do not fault. The 147k/s is the guest's **own writable memory**, handed
the EXECUTABLE view by `b_mmap`'s RW-divert branch (`husk-tl-bionic-io.c:1534-1544`). A loader-side
segment split would not move that number at all, and it carries the risk list in §3 for nothing.

What is delivered instead: the loader's present behaviour (§1, quoted), the exhaustive risk list for
the proposed split (§3), the measured mechanism (§2), the one change that would cut the count and
what has to be true before it is safe (§4), and the counters to confirm or kill it (§5).

---

## 1. How the loader places a library's segments today

`map_library`, `src/translation-layer-next/husk-tl-ld.c`.

**One span, not per-segment.** The loader reads every `PT_LOAD` into a `tl_segment` list and asks
`tl_page_plan` for the page count and a per-page flag byte:

```c
/* :1854-1879 */
tl_segment loads[16]; int nloads = 0; tl_segment relro = {0}; bool has_relro = false;
...
if (p->p_type == PT_LOAD_ && nloads < 16) {
    loads[nloads].vaddr = p->p_vaddr; loads[nloads].memsz = p->p_memsz; loads[nloads].flags = p->p_flags;
    nloads++;
}
...
uint64_t base_vaddr = 0;
size_t npages = tl_page_plan(loads, (size_t)nloads, has_relro ? &relro : NULL, PAGE, NULL, 0, &base_vaddr);
uint8_t *flags = malloc(npages);
tl_page_plan(loads, (size_t)nloads, has_relro ? &relro : NULL, PAGE, flags, npages, &base_vaddr);
```

and then takes **exactly one** allocation for the whole image plus its stub pages, and lays every
segment at its own vaddr offset inside it:

```c
/* :1957-1977 */
uint8_t *rx, *rw;
if (!tl_xmem_alloc((npages + nstub) * PAGE, &rx, &rw)) { ... }
memset(rw, 0, (npages + nstub) * PAGE);
for (unsigned i = 0; i < eh->e_phnum; i++) {
    const elf_phdr *p = &phs[i];
    if (p->p_type != PT_LOAD_ || !p->p_filesz) continue;
    ...
    memcpy(rw + (p->p_vaddr - base_vaddr), file + p->p_offset, p->p_filesz);
}
```

So: **flattened into one span, at the image's own relative vaddrs, `base_vaddr` = the lowest
`p_vaddr`.** `L->rx`/`L->rw` are that one allocation's two views, `L->npages` the span, `L->pflags`
the per-page flags, and the stub pool is the tail of the same allocation (`:1993`
`L->stub_rx = rx + npages * PAGE; L->stub_rw = rw + npages * PAGE;`).

**It does honour `p_flags` — the split already exists, at the view level.** `tl_page_plan`
(`src/translation-layer/husk-tl-elf.c:204-223`) marks `TL_PAGE_X` from `PF_X` and `TL_PAGE_W` from
`PF_W` (with the `PT_GNU_RELRO` range subtracted from `W`). Everything *else* in the loader is keyed
off those flags:

- `image_addr` (`:1421-1432`): a relocation pointer into a `TL_PAGE_W` page becomes
  **`L->rw + off`** — the writable twin. Code and read-only data become `L->rx + off`.
- `sym_value` (`:275-282`): same rule for a defined symbol (`in_code` decides code, not `st_info`).
- `reloc_one` (`:1475`): relocation *targets* are written at `L->rw + off` (the twin).
- `patch_image` (`:1283-1293`): an `adrp` whose target page is `TL_PAGE_W` is **retargeted at
  `L->rw`**, i.e. `w[i] = encode_adrp(rd, pc, tp + (L->rw - L->rx))`, unless `adrp_targets_code`
  says the address it forms is a function.

**What addresses the guest sees for data today**: the writable twin, `rw_base + (vaddr - base_vaddr)`
— for robot gf, `0x13f000000 + off` (`delta 658866176`). The writable twin is the debugger's
`vm_remap`, a genuinely writable mapping (`husk-tl-xmem.c:883-889`, `adopt_exec_view`:
`g_x.rw = m->rx_addr; g_x.wrw = m->rw_addr;`). **A store to a library `.data`/`.bss` object is a
native store with no trap.** Code and read-only data stay at `L->rx`, which is RX — correct, they are
never stored to.

So the arrangement the task asks to build ("code and read-only into the executable view, read-write
into a separate writable mapping") **is already the arrangement**; what is *not* separated is the
guest's own machine-allocated memory, which is a different subsystem entirely.

## 2. What the measured traps actually are

From `husk-20261016-093427-2.0.0-112.log`:

```
30   dual mapping established: rw=0x13f000000 rx=0x117ba8000 size=536870912 diff=-658866176
147  xmem: open: StikDebug's prewarmed region -- guest arena 0x117ba8000 is the EXECUTABLE view ... delta 658866176
153  xmem: arena 0x117ba8000..0x137ba8000: 512 MiB ... guest-facing view 0x117ba8000 is the executable view ... delta 658866176
309  husk: apk entry lib/arm64-v8a/libil2cpp.so: 91567688 bytes ...
310  xmem: backed the span just handed out 0x146614000..0x146654000 (262144 bytes, 64 pages ...)
311  bionic: a JIT's code memory (mmap hinted near the code) comes from the JIT region
312  xmem: backed the span just handed out 0x146654000..0x146694000 (262144 bytes ...)
313  husk: store into the executable view at 0x11f1bc000 (16 bytes) carried through the writable twin at 0x146614000 ...
314  ... 0x11f1bc010 ...  315 ... 0x11f1bc020 ...  316 ... 0x11f1bc030 ...
```

Read it off the addresses:

- `libil2cpp.so`'s allocation is the twin span `0x140c84000..0x146614000` (93,913,088 B = 89.5 MiB).
- The very next span, `0x146614000..0x146654000`, is handed out, then line 311 — the
  `b_mmap` divert — then a second span, then the first carries land **at `0x11f1bc000`**, whose twin
  is `0x146614000`, i.e. **inside that just-handed-out span, immediately after the library images
  end.**
- The writes are 16 bytes at a 0x10 stride — a zero-fill / block-store loop into a fresh buffer, not
  self-modifying code.

Two more facts settle it:

- **`mm: <n> bytes of executable memory for the guest at <p>` never appears in any log**, so
  `b_mmap`'s PROT_EXEC branch (`:1522-1530`, the one that legitimately hands `rx` for code) never
  fired. Every region span after the libraries came from the **RW-only** branch.
- **`mprotect` on the region never appears either** (`b_mprotect`'s `husk: xmem exec on the writable
  view` line is absent in every log), so the guest never asked to make any of this memory executable.

And the volume matches: the region's used pages are 60,088 × 4 KiB = **234.7 MiB**, of which the
library images are ~118 MiB and the rest — **~110–128 MiB across the spans after the libraries** —
is the guest's own writable memory, all handed `rx`. That is the memory the guest writes to
continuously: ~29.5 M trapped stores over the run.

### The branch that does it

```c
/* husk-tl-bionic-io.c:1531-1544 */
/* A JIT's code memory where only the JIT region can execute ... An anonymous, code-sized, writable
 * mapping hinted near the region comes from the region instead. */
if (addr && !(prot & PROT_EXEC) && (prot & PROT_WRITE) && (flags & 0x20) && !(flags & 0x10) && len <= (32u << 20)
    && tl_codewrite_enabled() && phone_jit() && tl_xmem_near(addr, 256u << 20)) {
    uint8_t *rx, *rw;
    size_t n = (len + TL_XMEM_PAGE - 1) & ~(size_t)(TL_XMEM_PAGE - 1);
    if (tl_xmem_alloc(n, &rx, &rw)) {
        memset(rw, 0, n);
        tl_note_once("a JIT's code memory (mmap hinted near the code) comes from the JIT region");
        mm_trace("mmap-jit", rx, len, prot, flags);
        return rx;                      /* <-- the EXECUTABLE view, for a WRITABLE request */
    }
}
```

`tl_xmem_alloc` returns genuinely two views (`*rw = xmem_writable_view() + start`, the debugger's
writable twin). The branch throws the writable one away and returns `rx`. The guest then treats `rx`
as ordinary writable memory — which is exactly what makes its arena executable later — and every
store into it traps, is decoded by `husk-tl-codewrite.c`, written through the twin at `tl_xmem_delta()`,
and resumed: a signal, a decode and a twin store per 16 bytes of data.

Why the guest hovers near the region at all: the layer's synthesized `/proc/self/maps` names each
library at `L->rx` (`husk-tl-ld.c` `tl_ld_maps` → `L->rx`, the executable view), so a guest that
allocates "near its own code" lands within `tl_xmem_near(addr, 256<<20)` of the region.

**The direct cost quoted in the task ("the guest is handed the EXECUTABLE view as its whole address
space") is right; the owner of it is `b_mmap`, not `map_library`.**

## 3. The exhaustive risk list for the proposed loader split

Recorded because it was asked for, and because it is the reason not to do it blind. If a library's
`TL_PAGE_W` pages were placed in a **second, separate** mapping at a different address from the
library's `rx`, all of the following break or have to be re-derived:

1. **The single-delta invariant.** `tl_xmem_delta()` is one constant for the whole region
   (`husk-tl-xmem.c:734`) and it is used by `husk-tl-codewrite.c` `put()` (`:75`), `kernel_dst()`
   (`husk-tl-bionic-io.c:549-554`), `b_mprotect` (`:1761`), `write_view_pointer`
   (`husk-tl-unity-app.c`), and `tl_xmem_contains`/`is_rx`/`near`. The loader's own comment states
   the invariant outright (`husk-tl-ld.c:2186-2188`): *"unmapping a slice would break the fixed
   rw<->rx delta every retargeted adrp, image_addr and the code-write emulator rely on."* A data
   mapping at a third address needs a per-mapping delta and every one of those call sites reworked.
2. **Code-to-data pc-relative arithmetic in the guest.** Anything that computes an object's address
   as `function_address + K` (baked by the Android linker when code and data were one mapping) or
   derives it from `&some_code` breaks when data is elsewhere. There is no way to enumerate these
   from the ELF; il2cpp/Unity do a great deal of it.
3. **`patch_image`'s adrp retargeting.** It retargets to `tp + (L->rw - L->rx)` — a per-library
   delta that would become wrong for the data pages, and `adrp_targets_code`'s "is this page code"
   test operates on `L->pflags`, which is indexed by `(tp - L->rx)/PAGE` and assumes data and code
   share one base.
4. **`image_addr` / `sym_value` / `reloc_one`.** All three compute `off = vaddr - L->base_vaddr`
   and index `L->pflags[off/PAGE]`; a second mapping needs a second base and a second flag array,
   and every caller that assumes `L->rw + off` is "the data twin of `L->rx + off`" has to change.
5. **The reloc table reused as a stub pool** (`:2025-2033`): `isl_rx`/`isl_rw` are carved out of the
   dead `.rela` range *inside the same allocation*; a split makes the RELRO/`.data` region's position
   relative to it undefined.
6. **`tl_xmem_is_rx` / `tl_xmem_contains`.** Their whole model is "one region, two (or three) views at
   a fixed delta". A data mapping outside the region makes a guest data fault land **outside**
   `tl_xmem_arena_fetch_fault`'s test, so it stops being a "guest fault that must not kill Husk" and
   becomes a host crash again.
7. **`tl_xmem_allow_exec`** (`husk-tl-xmem.c:1213+`) accepts an address in either view of the region.
   Library writable data outside the region would fall through `!in_rw && !in_w` → `false`, changing
   the answer for anything that mprotects it.
8. **RELRO.** `tl_page_plan` deliberately does *not* mark `PT_GNU_RELRO` pages `TL_PAGE_W`
   (`husk-tl-elf.c:214-221`), so a store into `.data.rel.ro` today goes through the guard. A split
   has to decide where RELRO lives, and the guest gets no say.
9. **TLS template, FDE/unwind ranges, `if (p->p_vaddr - base_vaddr + p->p_filesz > npages*PAGE)`** —
   all written against the one-span layout.
10. **The invariants the task names.** No `vm_protect` on the region; the executable view is never
    written; nothing the guest holds is unmapped; no thread that ran guest code is ended or joined;
    the honest refusal path intact; the store guard stays for code writes. A second mapping for data
    is fine on paper, but it is only provably safe if (1)–(9) are settled, and none of them can be
    settled without the phone.

**And the decisive objection: even done perfectly, it changes nothing.** §2 shows the trapped stores
are not on library data pages. The traps are in the guest's own spans, which no loader change touches.

## 4. The change that would cut it — and what has to be true before it lands

The fix is in `b_mmap`'s RW-only branch: for a request that is **writable and not executable**, hand
the guest `rw` (the writable view of the same pages) instead of `rx`. Stores become native and the
trap volume collapses. The pages are the same pages, so nothing is lost in the mapping; what changes
is which view the guest's pointer names.

Why it is not landed:

- The branch exists **for a JIT** (the comment names LuaJIT: "writes machine code, then mprotects it
  executable"). A JIT that is given `rw` and later calls `mprotect(PROT_EXEC)` on that address takes
  `b_mprotect`'s `tl_xmem_allow_exec` path (`husk-tl-bionic-io.c:1756-1764`), which on this device
  **refuses with EACCES** (the writable view is a `vm_remap` alias; `tl_xmem_guest_view_is_jit()` is
  false — `xmem-arena-and-wx.md`). The honest contract is that the JIT then falls back to its
  interpreter, but a JIT that does not check errno calls into non-executable memory and dies. That is
  a regression from "runs, slowly" to "does not run" — exactly the "half-done split is far worse
  than 40 FPS" the task warns about.
- There is **no device to verify it on**: no compiler and no Mac here, and the only way to tell a
  JIT that tolerates EACCES from one that does not is a phone run.
- **It cannot be flag-gated usefully.** The task suggests a flag for a large change; the phone has no
  way to set an environment variable (`staging-an-ipa-safely.md`), so a default-off flag is
  untestable on device.

**What would have to be true for it to land safely:**

1. For the engine in question, the guest's writable spans are **never made executable** — measured,
   not assumed. For robot gf this already looks true (no `mprotect` on the region in any log), so
   *for robot gf* handing `rw` is a pure win; for LuaJIT it is not.
2. A rule distinguishes the two at mmap time, or a JIT that is handed `rw` tolerates
   `mprotect(PROT_EXEC)` → EACCES and falls back. Candidates, in order of confidence:
   - the guest's hint width (a JIT hints within a page or two of its code; the spans here are
     256 KiB–18 MiB and grow like a heap),
   - a **wedge**: hand `rw` until the guest has demonstrated a JIT by mprotecting anything executable
     (then hand `rx` again), which preserves LuaJIT's later mappings if not its first,
   - an engine-keyed default (`tl_codewrite_enabled()` is already on for Java/Flutter/Geode and off
     for the native path).
3. The next run shows `carried` collapsed **and** the game still rendering — see §5.

## 5. The measurement that confirms or kills it

The counters already exist and are already summarised every 8 heartbeats and at session close
(`tl_perf_summary`, `husk-tl-unity-app.c`), with `husk-tl-codewrite.c:41`'s carry counter and
`tl_xmem_backing_stats()`.

| line | today (`husk-20261016-093427`) | after a correct fix |
|---|---|---|
| `codewrite carried=` | **29,538,738** (≈147,000/s) | the count of **genuine code writes** — for robot gf, whose guest never self-modifies code, this should be **near zero: expected ≲ a few thousand for a 200 s run, and plausibly 0–100** |
| `codewrite refused=` | 0 | 0 (unchanged — must not rise) |
| `arena allocs= / pages=` | 48 / 60,088 | unchanged (same memory, different view) |
| frames / `unity: alive` | ~1035 max, ~40 FPS | should rise above the owner's 40 FPS if the traps are the bottleneck |

So the expectation to state for the next run: **`carried` collapses from 29,538,738 to the small
number of genuine code writes (order 10²–10³ for a 200 s run, not 10⁷), `refused` stays 0, and the
frame rate moves.** If `carried` does not collapse, the trap source is not the mmap view and the
design above is wrong — which is the point of stating it.

## 6. Invariants held by this pass

No code was changed, so every invariant stands as it was: no `vm_protect` on the region; the
executable view is never written; nothing the guest holds is unmapped; no thread that ran guest code
is ended or joined; the honest refusal path is intact; the store guard still services genuine code
writes (`put()` still decodes and carries a store into the executable view, and still refuses
honestly what it cannot decode).

## 7. Next step (the smallest one that is safe)

A one-line experiment in `b_mmap`'s RW-only branch — `return rw;` where it returns `rx;` — run once
on the phone, read `carried` and the frame rate, then reverted if anything that worked before stops
working. It is deliberately **not** landed here: it changes when a guest's writable memory becomes
executable, and that cannot be validated from this box.

## 8. LANDED — the change, the honest JIT failure, the per-app memory, and the readings to confirm it

**Decision: `return rw` in `b_mmap`'s RW-only branch.** The trapped stores are the guest's own
writable spans (§2), so this is the one change that moves the count. It is landed together with an
honest named failure for the one case it can regress (a JIT) and a per-app memory so that case is not
permanent.

Files changed (the layer; no Swift, no other agent's files):

- `src/translation-layer-next/husk-tl-bionic-io.c` — the `return rw` and its comment; the RW-exec
  memory (`rw_exec_memory_load` / `rw_exec_memory_record`); the honest-failure line
  (`rw_exec_refuse_log`); the reset of the memory in `tl_set_data_dir`.
- `src/translation-layer-next/husk-tl-jni-hle.c` — `tl_hle_package()`, so the per-app note can carry
  the app's real identity (package), beside the existing `tl_hle_version_code()`.

### 8.1 The caller survey (who reaches the RW-only branch, and who relied on it being the executable view)

The branch is guarded by `!(prot & PROT_EXEC) && (prot & PROT_WRITE) && (flags & 0x20) && !(flags & 0x10)`
… `&& tl_codewrite_enabled() && phone_jit() && tl_xmem_near(addr, 256u << 20)`: **an anonymous
(`MAP_ANON`), non-`MAP_FIXED`, writable, non-executable mapping ≤ 32 MiB, asked for by the guest while
`tl_codewrite_enabled()` and on a phone, hinted within 256 MiB of the region.** It is not every
anonymous writable mapping — only the ones a guest hints near its code, which is exactly the behaviour
§2 pinned on the guest's own allocator (its synthesized `/proc/self/maps` names libraries at `L->rx`).
The only caller is the guest, through the `mmap`/`mmap64` shim (`husk-tl-bionic-io.c:2278`).

Who in the layer reads a mapping the branch handed out, and what they assumed:

- `b_munmap` (`:1874`): `if (tl_xmem_contains(a)) return 0;` — true for **either** view, so free is
  unchanged.
- `b_madvise`/`anon_zap`: the branch returns **before** `anon_add` (as it did returning `rx`), so
  `MADV_DONTNEED` handling is unchanged.
- `kernel_dst` (`:573`): `if (n && tl_xmem_is_rx(p) …)` redirects a kernel `read()` destination to the
  writable twin. With the guest holding `rw` this test is now **false**, so the kernel writes the
  destination directly — which is **more** correct (the writable view *is* writable), and the redirect
  the comment describes is still there for the executable view.
- `husk-tl-codewrite.c` `put()` (`:69`): only acts when the faulting store is in `tl_xmem_is_rx`.
  Stores into `rw` do not fault, so the guard is simply not entered — the guard itself is untouched.
- `write_view_pointer` / `fault_address_region` (`husk-tl-unity-app.c:122,160`): describe a fetch fault
  in the writable view. A JIT that ignores EACCES and jumps into its `rw` memory now lands here and is
  reported (the app wakes, Husk stays) instead of being a silent host crash.
- `ss_where` (`husk-tl-unity-app.c:918`): names the executable view when `tl_xmem_is_rx`; unaffected.

**No caller needs the executable view for a reason other than JIT.** `b_mprotect`'s writable-view
branch is the only place that cared, and that is the JIT case handled below. Nothing else in the layer
reads a guest RW mapping as executable.

### 8.2 The honest failure (the one real risk), and the per-app memory

A guest handed `rw` that writes code and calls `mprotect(PROT_EXEC)` reaches `tl_xmem_allow_exec`,
which cannot grant execute to a `vm_remap` alias and refuses with **EACCES**. The refusal is unchanged
(`tl_set_guest_errno(13); return -1;`); what is added is that it is never silent:

```
husk: RW-exec refused for <pkg> (version <n>) at <addr> (<l> bytes), asked by <lib> <sym>: this app
writes code into writable memory and then marks it executable: it needs the executable view and will
not run on this device while writable mappings are handed the writable view. The app is remembered and
gets the executable view on its next launch
```

(the caller site is the first guest library frame in the thread's return addresses, or `?`.)

**The per-app memory** (`rw_exec_memory_record`) writes `<data dir>/.husk-jit-writable` — i.e. in the
app's own `TranslationLayer/<uuid>/<engine>-data/` — atomically (`.tmp` + `rename`), holding the
header, the **package**, the **versionCode**, the time and the refused address. On the next launch
`rw_exec_memory_load` reads it on first `mmap`/`mprotect`, and **only if package and version both
match** hands THAT app the executable view for its RW mappings again (its old behaviour); a note for a
different app or version is ignored, never guessed at, and said so. Once the app shows it is a JIT, the
rest of the current run also reverts to `rx` (its old behaviour). **To clear it: delete
`<data dir>/.husk-jit-writable`** (or delete the app's data) — the app then gets native stores again
and re-records the note only if it asks once more.

### 8.3 The counters to confirm it (no new counters)

`tl_perf_summary` already prints `codewrite carried=… refused=…; arena allocs=… pages=…`. Expected on
the next robot gf run:

| reading | today (`husk-20261016-093427`) | expected now |
|---|---|---|
| `codewrite carried=` | 29,538,738 (~147,000/s) | the **genuine** code writes — robot gf self-modifies nothing, so **0–100** for the run (order 10²–10³ at most) |
| `codewrite refused=` | 0 | **0** (must not rise) |
| `arena allocs= / pages=` | 48 / 60,088 | **unchanged** (same pages, different view) |
| frames / FPS | ~1035, ~40 FPS | **above 40 FPS** |
| load to the main menu | ~49 s | **shorter** |

If `carried` does not collapse, the trap source is not the mmap view and this change is wrong — which is
the point of stating the reading before the run. The JIT guard is the `husk: RW-exec refused for …`
line: its **absence** on robot gf is expected; its **presence** on a LuaJIT-style title is the honest
record that that title needs the executable view.

### 8.4 Invariants held (re-checked after the change)

No `vm_protect` on the region (none added); the executable view is never written (the branch still
`memset`s `rw`, and returns it); nothing the guest holds is unmapped (no `munmap` added); no thread that
ran guest code is ended or joined (none touched); the honest refusal path is intact (EACCES, not a
pretend grant); the store guard stays for genuine code writes (`husk-tl-codewrite.c` untouched). The
only behaviour that changes is which view the guest's own writable pointer names, plus a per-app note
that restores the old view for an app that proves it is a JIT.

