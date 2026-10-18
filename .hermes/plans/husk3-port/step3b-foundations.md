# Step 3b — foundations: the xmem/ld two-way merge

Scope: exactly three files in `C:/Users/Aiden/Desktop/husk3.0/src/translation-layer-next/` —
`husk-tl-xmem.c`, `husk-tl-ld.c`, `husk-tl-ld.h`. Everything below is measured on disk with `diff`,
a comment/string-stripped identifier-set comparison and a brace/paren/`#if`-depth script.
**No compiler and no Mac here, so no build was made or claimed.** A compile is where this gets judged.

Method: ours is the BASE. Each of the three target files was replaced with our fork's copy and upstream's
work was then inserted into it (diff-driven, symbol by symbol), so none of the session's fixes can be lost
to a "copy theirs over ours". Backups kept outside both trees at `Desktop/.husk3-merge/{ours,upstream}/`
(the target is not a git repo, so upstream's pre-merge bytes are not otherwise recoverable locally).

| file | ours (base) | upstream | merged |
|---|---|---|---|
| `husk-tl-xmem.c` | 1,506 lines | 340 | **1,541** |
| `husk-tl-ld.c` | 2,329 lines | 2,142 | **2,483** |
| `husk-tl-ld.h` | 175 lines | 118 | **175** |

## The brief's list, item by item

### `husk-tl-xmem.c`

| item | verdict | where / choice |
|---|---|---|
| `tl_jit_reserve_bottom` | **INTEGRATED** | defined before `tl_xmem_alloc`; returns `!g_reserve \|\| g_reserve(end)`. Apple branch does the `dlsym`, a `#else` stub returns true on a host with no QEMU. |
| `find_reserve` | **INTEGRATED** | `pthread_once` + `dlsym(RTLD_DEFAULT, "husk_ios_jit_reserve_bottom")`, as upstream. |
| the `husk_ios_jit_reserve_bottom` bottom-reserve hook | **INTEGRATED (wired, optional)** | still resolved at run time by name, exactly as upstream: the hook lives in the app's JIT bootstrap and the layer does not link it, so its absence is not an error — it means "no limit". |
| the `g_x.shared` path (sharing QEMU's region) | **RECONCILED** | new `bool shared` in `g_x`. It is set **only** in the StikDebug-region branch of `open_platform` (`g_x.shared = true;`) and cleared at the top of `open_platform` (`g_x.shared = false;`). Our arena of the layer's own is never shared, so the reserve is asked exactly when the region really is QEMU's — **both designs stay reachable**: our own-arena preference is unchanged, and when the layer does fall back to the debugger's region the QEMU top-reserve is honoured. `tl_xmem_alloc`'s guard is now `n <= g_x.size - g_x.used && (!g_x.shared \|\| tl_jit_reserve_bottom(g_x.used + n))`. |

Deliberately not carried across (ours is the newer form, not a loss):
`self_selftest`/`self_fault`/`sa_handler` — ours is `self_runs`/`self_fault_info` with **`SA_SIGINFO`**
(upstream still uses a plain `signal()`-style handler, which cannot name `si_code`; see the arena refs);
`mach_vm_remap`/`mach_vm_address_t` — upstream's host `open_platform` body, which ours replaced with the
shared `make_views` builder so the Mac runs the code the phone runs.

### `husk-tl-ld.c`

| item | verdict | where / choice |
|---|---|---|
| `tl_ld_main_apk` | **INTEGRATED** | after `tl_ld_queued_split`. Upstream's caller is `husk-tl-bionic-io.c` (a still-diverged file); our tree's copy does not call it, so it is currently defined-and-unreferenced — harmless (non-static). |
| `tl_ld_extract_lib` | **INTEGRATED** | after `fetch_from_apks`, verbatim; took upstream's `<unistd.h>` include with it (for `unlink`). Same caller situation as `tl_ld_main_apk`. |
| `add_search_dir` | **INTEGRATED** | upstream's `add_search_dir(dir, bool app)`; `tl_ld_add_search_dir(dir)` is the public wrapper and passes `false` (system), matching what our tree's callers pass (`com.android.i18n/lib64`, `com.android.conscrypt/lib64`, the DVM's `libdir`). |
| `fetch_from_dirs` | **INTEGRATED** | filtered by the app/system flag. Replaced our single unfiltered `g_search` walk. |
| `find_loaded_ns` | **RECONCILED** | the loader now has BOTH dimensions. `find_loaded_ns(name, ns)` carries upstream's namespace choice (`NS_APP`/`NS_SYSTEM`/`NS_ANY`, plus the `sys` fallback and the `return ns == NS_APP ? NULL : sys`) **inside** our `lib_in_session()` filter, and `find_loaded(name)` is kept as the `NS_ANY` wrapper every one of our call sites already used. `build_scope` now looks a dependency up in the importing library's own namespace (`cur->system ? NS_SYSTEM : NS_ANY`) — upstream's fix — and the session filter still applies underneath. `struct tl_lib` gained `bool system` next to our `int session`. |
| `mono_aot_code_only` | **INTEGRATED** | verbatim, before `relocate`, called immediately before `patch_image` so a .NET AOT image has only its JIT/PLT/trampoline ranges patched. |
| `data_page` | **INTEGRATED** | upstream's `static inline bool data_page(L, page)` (which carries `L->go_data`) restored, and **reconciled with the guest-address rule**: `view_of` now asks `data_page` for its "writable" answer, so a Go library's read-only data is named writable there too and the rule and the patch sites cannot disagree. The three `(L->pflags[...] & TL_PAGE_W)` sites in `patch_image` (tpidr `adrp`, x18 `adrp`, `adr`) are back to `data_page(L, …)`. |
| `MAX_LIBS 512` | **INTEGRATED** | `96` → `512`, upstream's comment kept. Our `96` was the base import's value (only ever touched by commit `ed5a1b6`), so raising it costs nothing our tree relies on. |
| `TL_INIT_TRACE` | **INTEGRATED** | the whole upstream constructor block: the `rx/rw/pages` line, a line before and after each constructor, plus `TL_INIT_BREAK=<library>` (upstream ships them as one block; splitting them would have been an invented distinction). |
| the app/system search-dir split | **INTEGRATED** | `g_search_app[]`, `add_search_dir(dir, app)`, `fetch_from_dirs(name, app, …)`, `in_system_dir(path)` (with `tl_path_resolve`), and `load_locked(name, depth, bool system)` with upstream's app-first / system-fallback fetch order, `in_system_dir` promotion of a full path, the `find_loaded_ns(name, NS_SYSTEM)` retry, and `L->system = system`. `tl_ld_load` passes `false`. Kept on top of it: our `tl_load_now_ns()` timing around `map_library` and `relocate`. |

Extra upstream-only work found by the diff (not in the brief's list) and also integrated:
`in_system_dir`, the Go detection (`"\xff Go buildinf:"` + `memmem` → `L->go_data`, and the
`adr`-stub-bytes test that uses it), `NS_*`, `L->system`, `TL_INIT_BREAK`, `<unistd.h>`, and
**`tl_svc_common`'s `nzcv` + `q8–q15` saves with the 640-byte frame**. That last one matters: OUR BASE
CLAIMED in its comment to save "the condition flags and all of q8-q15" and **did not** — it saved neither,
so a guest's `svc` sites could return with the flags and the upper halves of `q8–q15` clobbered by the C
handler. Upstream's newer body is strictly additive and is now in.

Deliberately not carried across (ours is the newer form):
`is_func` in `sym_value` — ours decides code by **section** (`in_code`), the fix for the
address-provenance crash where a stripped/hand-written library marks a function `STT_NOTYPE`;
`tl_zip_data` — ours calls `tl_zip_data_cached` (inflate-once cache; a Unity title's two 109 MiB
libraries are deflated in the APK).

### `husk-tl-ld.h`

| item | verdict | where / choice |
|---|---|---|
| `TL_LD_MAX_APKS 48` | **INTEGRATED** | our `16` → `48` with upstream's comment. Our `16` was itself a deliberate fork value (grew `g_splits[4]`→`[15]` in `33cd42a`), so this is a **cap raise, not a port** — it cannot lower the asset-pack-split ceiling. `g_splits` and the `tl_ld_queue_split` bound in the `.c` now use the macro so the two cannot drift apart again. |

## The four blocked symbols — defined and declared

| symbol | declared | defined |
|---|---|---|
| `tl_ld_va_relro` | our `husk-tl-ld.h:136` | our `husk-tl-ld.c` |
| `tl_ld_apk_path_at` | our `husk-tl-ld.h:48` | our `husk-tl-ld.c` |
| `tl_xmem_emit_begin` / `tl_xmem_emit_end` | `husk-tl-xmem.h:115-116` (ours; untouched) | our `husk-tl-xmem.c` |
| `tl_load_add` / `tl_load_now_ns` | `src/translation-layer/husk-tl-internal.h:132-133` (already in the target) | **our `husk-tl-bionic-io.c`, not `husk-tl-ld.c`** — see below |

**Correction to the brief:** `tl_load_add`/`tl_load_now_ns` are **not** defined in our `husk-tl-ld.c`.
They are defined in our `husk-tl-bionic-io.c`, which owns the accumulator tables (`g_load_ns`,
`g_load_bytes`, `g_load_calls`) and prints them at session close. I deliberately did **not** copy the
definitions into `husk-tl-ld.c`: that would move half a counter to another file and collide with our
`husk-tl-bionic-io.c` the moment it is placed. They are DECLARED in `husk-tl-internal.h` (present in the
target), so the merged `husk-tl-ld.c` and the four blocked files all **compile** against them; **linking
needs our `husk-tl-bionic-io.c`**, which the orchestrator must merge regardless — `husk-tl-bionic-ndk.c`
also needs `tl_zip_data_cached` from that same file. This is flagged, not closed.

## Invariants — checked, not disturbed

- **No `vm_protect` on the region in `adopt_exec_view`.** The merge adds no protection-changing call to
  `husk-tl-xmem.c`; the W^X fallback, the store guard's arming, `make_views` and the exec gate are our
  copies unchanged. Grepped: the only `vm_protect` calls remain upstream's/ours in `make_views`,
  `adopt_alias` and `vm_protect(me, alias, size, FALSE, VM_PROT_READ|VM_PROT_EXECUTE)` inside `make_views`.
- **Nothing the guest holds is unmapped; no thread that ran guest code is ended or joined.** The merge
  unmaps nothing and touches no thread; the reserve hook only *asks*.
- **A guest fault is scoped by the FAULT ADDRESS** — `tl_xmem_arena_fetch_fault` is our copy, untouched.
- **Arena-size range and the 2 GiB twin bound** — `grant_self`'s halving to 64 MiB and
  `near()`/`adopt_alias` are untouched.
- **A host type larger than bionic's is a silent stack smash** — the pthread family is out of scope here;
  nothing merged touches a bionic-vs-Darwin type size. `bool shared` is inside `g_x`, which is the
  layer's own struct.

## What could not be verified

- **No compile, no link, no run.** There is no `cc`/`gcc`/`clang`/`zig`/`tcc` on this box, so the claim is
  correctness by construction only.
- Structural check that WAS run (comment/string-stripped): brace, paren and bracket balances are `0`;
  `#if/#ifdef/#ifndef` depth returns to `0` with no stray `#endif`; in all three merged files.
- Identifier-set check: **every upstream-only identifier is now present in the merged file**, except four
  that our copies deliberately replace (`self_selftest`/`self_fault`/`sa_handler`, `mach_vm_remap`/
  `mach_vm_address_t` in `xmem.c`; `is_func`, `tl_zip_data` in `ld.c`) and **no ours-only identifier was
  lost** in any of the three files.
- Line endings: `husk-tl-xmem.c` is CRLF-dominant in our own tree (1,488 of 1,506 lines); the merged copy
  is all-CRLF, i.e. it kept that file's own style. `husk-tl-ld.c` and `husk-tl-ld.h` stay all-LF, as ours are.
- The `husk_ios_jit_reserve_bottom` hook itself is not defined anywhere in either tree — it is a
  `dlsym`'d symbol the app's JIT bootstrap is expected to provide. Unverifiable here; upstream's own code
  treats its absence as "no limit".
