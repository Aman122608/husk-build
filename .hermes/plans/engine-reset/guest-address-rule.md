# The guest-address rule: one rule for every place that names a guest a page, and what it closes

Task: turn `a74459a`'s targeted fix (`b_mmap`'s RW-only branch hands the guest the writable view) into a
RULE enforced at every place that produces a guest-visible address: a page the guest may STORE to is always
named through the WRITABLE view, and only a page the guest may EXECUTE is named through the executable
view. Close the /proc/self/maps lie without moving the base; MEASURE whether `.data.rel.ro` stores are real
before touching RELRO. Re-verify the three routes `c99d2d8` listed at their `file:line` and audit for
others rather than trusting that list.

**Verdict in one line: the rule now lives in ONE function (`view_of`, `husk-tl-ld.c`) that every producer of
a guest address goes through; it fixed a real route `a74459a` missed — `dladdr`'s `dli_saddr` was naming a
DATA symbol's address through the executable view — and the maps now describe the library as the TWO views it
really is while the BASE stays exactly where it was (a code address). RELRO is MEASURED, not changed: the run
decides. The three routes `c99d2d8` named resolve to: (1) partially closeable and now closed where it can be
— the base cannot move, the per-address routes go through the rule; (2) measured; (3) correct as it is.**

Date: 2026-10-16. Evidence: the layer sources, and `c99d2d8` (`pre-2.0-comparison.md`), `fd11319`,
`a74459a`.

---

## 0. The rule, as code

One function decides it. `husk-tl-ld.c`:

```c
static uint64_t view_of(const tl_lib *L, uint64_t vaddr, bool is_code)
{
    uint64_t off = vaddr - L->base_vaddr;
    size_t page = (size_t)(off / PAGE);
    bool writable = page < L->npages && (L->pflags[page] & TL_PAGE_W);
    if (is_code || !writable) return (uint64_t)(uintptr_t)(L->rx + off);
    return (uint64_t)(uintptr_t)(L->rw + off);
}
```

- **A page the guest may STORE to** (`TL_PAGE_W`) is named through the WRITABLE view (`L->rw`) — its stores
  are native and never reach the store guard.
- **A page the guest may EXECUTE** sees the EXECUTABLE view (`L->rx`) — the only view that carries execute.
- **A page that is neither** (read-only data, and RELRO, which `tl_page_plan` keeps out of `TL_PAGE_W`)
  is named through the executable view: it is readable there, and naming it writable would invite a store
  the guest must not make.
- A page holding both code and data (a small library's code and data on one page) is writable, so the
  SECTION the address falls in decides: `is_code` (from `st_info`, or `in_code`) keeps a function executable.

This is behaviour-IDENTICAL to `sym_value`/`image_addr` as they were — it is those two bodies, extracted. The
rule is what makes the third caller correct.

---

## 1. The audited list: every place that produces a guest-visible address

Searched, not assumed — every `L->rx`/`L->rw` producer, every `tl_xmem_alloc` consumer that returns a view to
the guest, and `module symbol`/base/dladdr/maps/mmap/mprotect paths.

| # | place (file:line at this commit) | what it hands the guest | follows the rule? | action |
|---|---|---|---|---|
| 1 | `sym_value` — `husk-tl-ld.c:301` | a defined symbol's address (dlsym, the global namespace, `bind_symbol`) | yes (code→rx, data→rw) | **routed through `view_of`** (identical behaviour) |
| 2 | `image_addr` — `husk-tl-ld.c:1470` | the address of a relocatable object (relocations, pointer data) | yes (code→rx, data→rw; code-in-data page test) | **routed through `view_of`** (identical behaviour) |
| 3 | `sym_in_lib` (`*sym_addr`) — `husk-tl-ld.c:679` | `dladdr`'s `dli_saddr` / `dli_saddr` for a named symbol | **NO — always the executable view** | **FIXED** to `view_of(..., in_code(...))`: a DATA symbol's address is now named writable |
| 4 | `tl_ld_maps` — `husk-tl-ld.c:713` | the synthesized `/proc/self/maps` | **NO — one `r-xp` span at `L->rx` for code AND data** | **FIXED**: two honest view lines (see §2) |
| 5 | `tl_ld_lib_base` — `husk-tl-ld.c:630` | `dli_fbase` / the load bias | the BASE, a code address, deliberately | **UNCHANGED** (see §3) |
| 6 | `tl_ld_iterate` bias — `husk-tl-ld.c:724` | `dl_iterate_phdr`'s load bias | the BASE, deliberately | **UNCHANGED** (see §3) |
| 7 | `ensure_tcb` + adrp retarget (`:1345`) + `tpidr_el0` rewrite (`:1448`) | the TLS block (`tcb_rw`) | yes — the guest writes TLS and it is named `G.tcb_rw` | none needed |
| 8 | `make_stub` (`husk-tl-ld.c:789`) / `svc_stub` (`:962`) | an executable stub the guest branches to | yes — it is CODE, named rx | none needed |
| 9 | `b_mmap` PROT_EXEC branch — `husk-tl-bionic-io.c:1796-1803` | code memory for a hooking JIT | **correct** — the guest asked for code; the store guard is its service | **unchanged** (a74459a's sibling) |
| 10 | `b_mmap` RW-only branch — `husk-tl-bionic-io.c:1808-1834` | the guest's own writable memory | yes after `a74459a` (`return rw`) | unchanged |
| 11 | `glt_wrap` — `husk-tl-egl.c:718-745` | a GL thunk the guest CALLS | yes — CODE, named rx | none needed |
| 12 | `kernel_dst` — `husk-tl-bionic-io.c:573` | redirects a kernel `read()` destination to the twin | not a returned address; unchanged | none needed |
| 13 | `patch_image` adrp retarget — `husk-tl-ld.c:1254+` | rewrites code that reaches data to the writable view | not a returned address; already the rule | none needed |

**So the rule is enforced by one function, and three call sites now go through it; two of those (3 and 4) were
really broken, and (4) and (3) are the two the old passes had left.**

---

## 2. The maps description (route 1, part one): the truth, without moving the base

`c99d2d8` §4.2 (3) is right: the synthesized `/proc/self/maps` listed each library as ONE `r-xp` span at
`L->rx`, "code and data together". That is a lie about the guest's own memory: the guest's data pointers are
`L->rw`-based, and the only store-writable view is `L->rw`.

**The fix** (`tl_ld_maps`, `husk-tl-ld.c`): describe the library as the TWO views it really is —

```
<rx>-<rx+span> r-xp 00000000 fd:00 <n> <libdir>/<name>
<rw>-<rw+span> rw-p 00000000 fd:00 <n> <libdir>/<name>
```

- The **executable-view line is emitted FIRST and its text is byte-identical to before**, so any guest that
  takes the FIRST mapping for a library as its base reads exactly the base it always read. Only the
  writable-view line is NEW.
- Both lines are the same pages, at the same relative offsets, two views apart; the `rw-p` line is the
  honest answer to "where can I write", and it is what the guest's own data pointers already name.

**Why the base does not move, stated precisely.** The base is not a mapping line; it is
`tl_ld_lib_base(L) = L->rx - L->base_vaddr` (`husk-tl-ld.c:630`), the address of the image's vaddr 0 **in the
executable view**. Guest code, the relocations the loader baked (`image_addr`), and every pc-relative
retarget are written against that ONE number, and code must be executable, so the base is a code address
and cannot be moved to `L->rw` — moving it would break every guest that computes an address from it
(`c99d2d8` §4.2: "getting that wrong breaks every guest that computes an address from the base"). The fix
touches only the DESCRIPTION: the executable view keeps its line and its meaning; the writable view gains
the line it always deserved. `tl_ld_lib_base` and `dl_iterate_phdr`'s bias are untouched.

**Risk, stated.** A guest that parses maps and computes a library's extent as `max_hi - min_lo` across all
lines for that path now sees a span two views wide; a guest that takes the first line as the base is
unaffected; a guest that scans for its own writable anon regions is unaffected. This is the one part of this
pass that a device run should watch (the allocator behaviour is the observable), and it is why the first line
was kept identical rather than re-split per segment.

---

## 3. Route 1, part two: the base and `dladdr`/`dl_iterate_phdr`

`c99d2d8` §4.2 (3) also says `dladdr`/`dl_iterate_phdr`-derived DATA addresses name the executable view.
There are two distinct things there, and they resolve differently:

- **The bias / `dli_fbase`** (`tl_ld_lib_base`, `tl_ld_iterate`) is the base — a code address. It MUST stay
  the executable view's vaddr 0 (see §2). A guest that derives a data address as `bias + data_vaddr` still
  names rx; that is not fixable by moving the base without breaking code derivation, and the loader has
  already baked the correct view into every pointer the guest's own relocations carry (`image_addr` → rule).
  This sub-route is stated, not silently "fixed".
- **`dladdr`'s `dli_saddr`** was a genuine bug: `sym_in_lib` (`husk-tl-ld.c:679`) computed
  `*sym_addr = L->rx + off` **unconditionally**, so even a DATA symbol's address was named through the
  executable view, and a guest that STORES through `dli_saddr` faults into the store guard. It now goes
  through `view_of`: a data symbol's address is writable, a code symbol's stays executable. This is the
  route `a74459a` missed and the rule closes it.

So route 1's per-address half is now closed by the rule, and its base half is honestly out of scope (a base
is one address; the rule needs a per-address decision the base interface does not carry).

---

## 4. Route 2: RELRO — MEASURED, not changed

`c99d2d8` §4.2 (2): the loader hands data through rx for any page not marked `TL_PAGE_W`, and `TL_PAGE_W`
deliberately excludes the `PT_GNU_RELRO` range (`husk-tl-elf.c:211-223`), so a store into `.data.rel.ro`
names rx and traverses the guard. Whether the guest actually stores there is **UNMEASURED**, and this pass
does not guess.

**The measurement** — bounded, keyed on RELRO, summarised like the other counters:

- `tl_lib` carries the image's RELRO range (`relro_vaddr`, `relro_size`), set in `map_library` from the
  parsed `PT_GNU_RELRO` segment.
- `tl_ld_va_relro(addr)` (`husk-tl-ld.c`) answers 1 / 0 / -1 — RELRO, in a library but not RELRO, not in a
  library — for an address in EITHER view. It is lock-free and allocates nothing (it runs on the store
  guard's fault path, where `tl_ld_lib_of` is already called).
- `husk-tl-codewrite.c` increments `g_emulated_relro` for each CARRIED store whose fault address is a RELRO
  page — one relaxed increment per carry, no line per store.
- `tl_perf_summary` (`husk-tl-unity-app.c`) now prints it: `codewrite carried=… refused=… relro=…`.

**No behaviour changed.** `TL_PAGE_W`/RELRO naming is untouched; the counter only measures. If the run shows
real volume, the plan is to mark RELRO pages the writable view (they are data the guest may store to while
relocating) as a follow-up; if it shows zero, the route stays closed by "no guest stores there".

---

## 5. Invariants, re-checked after this pass

- **No `vm_protect` on the region** — none added; a pointer is chosen, not a protection changed.
- **The executable view is never written** — `view_of` returns a pointer; the loader still writes through
  `L->rw` (and the guard through the twin). No new write to `L->rx`.
- **Nothing the guest holds is unmapped** — no `munmap` added.
- **No thread that ran guest code is ended or joined** — none touched.
- **The honest refusal path is intact** — `tl_xmem_allow_exec` → EACCES, named once by `rw_exec_refuse_log`
  (`husk-tl-bionic-io.c`, untouched).
- **The store guard still handles genuine code writes** — `put()`/`emulate()` are unchanged except the RELRO
  counter; a store into code is still decoded and carried through the twin, and an undecodable form still
  refuses and chains.
- **The per-app JIT memory and named refusal stay as they are** — `rw_exec_memory_record` /
  `rw_exec_memory_load` / `rw_exec_refuse_log` untouched.
- **`b_mmap`'s PROT_EXEC branch is unchanged** — a request for code memory is served from the executable
  view on purpose (`c99d2d8` §4.2 (1)).

---

## 6. Expected reading, and how the next run distinguishes the two outcomes

`a74459a` should already have collapsed `codewrite carried=` from 29,538,738 toward the genuine code-write
count; this pass adds no carry of its own (route 3 is correct; route 2 is only measured) but it closes the
`dli_saddr` route.

| reading | today (`husk-20261016-093427`) | expected now |
|---|---|---|
| `codewrite carried=` | 29,538,738 (~147,000/s) | the genuine code writes: **0–100** for robot gf (self-modifies nothing) |
| `codewrite refused=` | 0 | **0** (must not rise) |
| `codewrite relro=` (NEW) | — | the fixture that says whether `.data.rel.ro` stores are real |
| `arena allocs= / pages=` | 48 / 60,088 | unchanged |
| frames / FPS | ~1035, ~40 FPS | above 40 FPS |

**The two outcomes, told apart:** if `carried` falls to a SMALL non-zero number (order 10²) with `relro`
accounting for it, the routes are CLOSED — the residual is `.data.rel.ro` and the fix is the measured
follow-up of §4. If `carried` does NOT fall, the routes were never the hot term (the storm was all one
branch) and §2–§4 changed nothing about the trap rate — which is itself the answer. `relro` is what removes
the guess: `relro≈carried` means every residual carry is RELRO data and the fix is clear; `relro≈0` means a
non-RELRO data page is still being named executable somewhere, and the audit in §1 should be re-run against
that address's library and page flags.

---

## 7. What this pass changed

- `src/translation-layer-next/husk-tl-ld.c` — `view_of` (the rule); `sym_value`, `image_addr` routed
  through it; `sym_in_lib`'s `dli_saddr` fixed; `tl_ld_maps` now describes both views; `tl_lib` gains the
  RELRO range; `tl_ld_va_relro` added.
- `src/translation-layer-next/husk-tl-ld.h` — the rule documented; `tl_ld_va_relro` declared.
- `src/translation-layer-next/husk-tl-codewrite.c` — the RELRO carry counter.
- `src/translation-layer-next/husk-tl-codewrite.h` — `tl_codewrite_relro_count` declared.
- `src/translation-layer-next/husk-tl-unity-app.c` — `tl_perf_summary` prints `relro=`.

No `vm_protect`, no mapping change, no thread lifetime change, no RELRO rename, no Swift, no other agent's
files (`tools/a32` untouched).
