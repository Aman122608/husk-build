# Step 3 — the core translation-layer port (`src/translation-layer-next/`)

Scope: `husk-tl-xmem.{c,h}`, `husk-tl-ld.{c,h}`, `husk-tl-codewrite.{c,h}`, `husk-tl-unity-app.{c,h}`,
the `husk-tl-bionic*` family (incl. `husk-tl-bionic.h`), `husk-tl-egl*` (incl. the `.inc`), `husk-tl-vulkan.m`.
Read first: `PLAN.md`, `merge-list.txt`, `tree-diff.json`, and the `husk-translation-layer` skill references.
No Mac, no compiler here: every claim below is a diff measured on disk, not a build.

## The recon's conclusion is FALSIFIED

The plan asserts upstream 3.0's layer "is an ANCESTOR of ours, not a fork with different work" and that
the port "runs ours -> theirs" for the whole layer. That is false for most of these files.

3.0 is a **parallel branch that also advanced**. It carries its own newer layer work that our fork does
not have. Measured, symbol by symbol (grep across our whole `src/`):

| file | upstream-only content our fork LACKS (verified absent from our tree) |
|---|---|
| `husk-tl-xmem.c` | `tl_jit_reserve_bottom`, `find_reserve`, the `husk_ios_jit_reserve_bottom` bottom-reserve hook (QEMU holds the top of StikDebug's region), the `g_x.shared` path |
| `husk-tl-ld.c` | `tl_ld_main_apk`, `tl_ld_extract_lib`, `add_search_dir`, `fetch_from_dirs`, `find_loaded_ns`, `mono_aot_code_only`, `data_page`, `MAX_LIBS 512`, `TL_INIT_TRACE`, the app/system search-dir split |
| `husk-tl-ld.h` | `#define TL_LD_MAX_APKS **48**` (ours is **16**) |
| `husk-tl-unity-app.{c,h}` | the whole console/host integration: `husk_console_loaded_game`, `husk_java_documents_picked`, `husk_java_set_document_picker`, `husk_native_set_extra_fonts`, `husk_unity_{text,insert_text,delete_backward,finish_text,set_keyboard_handler}`, `tl_set_extra_fonts_dir`, MIME document picker |
| `husk-tl-bionic.c` | `tl_bionic_uname` (raw-syscall uname for fingerprinting libs), `libvulkan.so.1` (Godot 4's versioned name), the `TL_SYSPROPS` injection block |
| `husk-tl-bionic-io.c` | the `arm_thread_state64_{get,set}_*` guest-ucontext accessors, `_STRUCT_ARM_THREAD_STATE64`, `F_GETPATH`, xattr family |
| `husk-tl-bionic-ldbl.c` | `b___signbit`/`b___signbitf`, `b_capget`/capset, xattr (`getxattr`…), `b_getgr_r` (`getgrgid_r`/`getgrnam_r`), `b_splice`, `b_enotsup`, `readv`/`sync`/`getsid`/`strtoimax`/`strtoumax` |
| `husk-tl-bionic-net.c` | the whole RPC hostent marshalling: `b_gethostbyname_r`, `b_gethostbyname2_r`, `b_gethostbyaddr_r`, `he_copy`/`he_finish`, `g_in6addr_{any,loopback}` |
| `husk-tl-bionic-pthread.c` | `b_attr_getdetachstate` / `pthread_attr_getdetachstate` |
| `husk-tl-egl-es31.inc` | `oes_external_rewrite` — the `GL_OES_EGL_image_external` / `samplerExternalOES` shader rewrite |
| `husk-tl-codewrite.c` | nothing (see below — ours is a clean superset) |

Corroborated by `tree-diff.json` `only_new`: 3.0 also ships files that exist nowhere in our tree —
`husk-console-host.{c,h}`, `husk-console-abi.h`, `husk-jit-arena.{c,h}`, and (app side) `PS4Library.swift`,
`PS4Screen.swift`, `ConsoleUI.swift`, `Compat15.swift`. A PS4-console feature is new upstream product
work, and `husk-tl-unity-app.c` is where it lands.

So this step is a **per-file merge, not a sweep**. The port was run only where our copy is a *proven
superset* of upstream's — i.e. upstream has nothing ours lacks.

## Superset proof method (used per file before writing anything)

For every in-scope differing file, three independent measures, all on disk:

1. **Comment-stripped code-token sets.** Strip `//` and `/* */` and string literals, tokenize both
   copies, and list identifiers present in upstream but absent from ours. `0` ⇒ no upstream-only
   identifier.
2. **Target-only substantive source lines.** Same strip, then list upstream lines (minus braces/`#endif`)
   whose exact text is absent from ours. Distinguishes real upstream code from reworded comments.
3. **Token-subsequence test.** Is upstream's token sequence an ordered subsequence of ours? `True` ⇒
   ours = upstream + insertions, with **no deletion and no reordering** — the strongest form of "superset".

A file was ported only if measure 1 was `0`/comment-only **and**, in addition, every symbol our copy
newly *calls* is already defined (or declared and defined) somewhere in the current target tree — so the
placed file compiles and links as the tree stands. Files that are supersets but depend on symbols that
live only in a still-diverged file were **left** and are listed with the exact missing symbols; placing
them would have made the target fail to compile or link for no gain the orchestrator cannot recover by
merging the defining file first.

Structural check on every placed file (no compiler available): comment/string-stripped brace, paren and
bracket balances are 0, `#if/#ifdef/#ifndef` depth returns to 0 with no stray `#endif`, and each placed
file is byte-identical (`cmp`) to our fork's copy — i.e. it is our proven-good file, unmodified.

## PORTED — ours is a proven superset (5 files)

| file | proof | what our copy adds |
|---|---|---|
| `husk-tl-vulkan.m` | measure 1 = 0; measure 2 = 0; **measure 3 = subsequence True** (pure insertion, 41156→49102 B, +151 lines) | the MoltenVK compat layer's newer retries. Invariants preserved and verified in place: `k_over[]` consulted by `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr` and the direct resolver (line 849/877); the "**ask exactly as asked, retry only on refusal**" contract (`vkCreateImage` LINEAR→OPTIMAL, 747; `vkAllocateMemory` pNext cut with the sType trace, 829). Upstream shared all of this and we add to it. |
| `husk-tl-bionic-str.c` | measure 1 = 0; measure 2 = 1 line, a *substitution*; not a subsequence because ours replaces it | ours adds the zlib `gzopen`/`gzread`/`gzclose` shims (dlopen of host libz, honest NULL/-1 when absent) and `TL_DIRECT(swscanf)`. The one upstream line not carried verbatim is `tl_note_missing_file(**path**)`, which ours calls with `**real**` (the resolved **host** path) — strictly better, same function, same ENOENT guard. Deps `tl_path_resolve`/`tl_note_missing_file` are defined in the target's `husk-tl-bionic-io.c` (lines 167, 523) ⇒ links. |
| `husk-tl-bionic.h` | measure 1 = 0; measure 2 = 0 | the header we own. Every upstream declaration survives; ours adds the exit family, the `TL_HP_*` hot-path counters, `tl_asset_read_note`, the guest-thread census/epoch. Safe: it only *declares* extra API (definitions live in `husk-tl-bionic.c`, left); nothing in the current tree calls the extras. |
| `husk-tl-xmem.h` | measure 1 = 0; measure 2 = comment-only; **measure 3 = subsequence True** | adds the extended arena API — `tl_xmem_exec_verdict`/`_detail`, `tl_xmem_allow_exec`, `tl_xmem_guest_view_is_jit`, `tl_xmem_wx_fallback`, `tl_xmem_exec_gate`, `tl_xmem_emit_begin/end`, `tl_xmem_used_high_water`, `tl_xmem_region_base`, the verdict-persistence pair, the fault-time probe, the lock ledger and the swap-in-progress flag. Every upstream declaration (`open/alloc/delta/contains/is_rx/flush/used/size/near`) is retained. Declares API defined only in the left `husk-tl-xmem.c` — inert until that file is merged. |
| `husk-tl-codewrite.h` | measure 1 = 0; measure 2 = 0 | adds `tl_codewrite_refused_count` (DECL only — the definition is in the left `husk-tl-codewrite.c`) and `tl_codewrite_relro_count`; every upstream declaration kept. |

`husk-tl-egl.c`, `husk-tl-egl.h`, `husk-tl-vulkan.h` are **byte-identical** in both trees (not in
`merge-list.txt`) — nothing to port. No ours-only file exists under any of my scope names in
`tree-diff.json` `only_old` (the only `-next` entry there is none; `only_new` holds `husk-console-*` /
`husk-jit-arena.*`, outside my scope).

## LEFT for the orchestrator (15 files)

### Diverged — upstream has real work our fork lacks (11)

| file | what upstream has that ours lacks |
|---|---|
| `husk-tl-xmem.c` | `tl_jit_reserve_bottom` + `find_reserve` + the `husk_ios_jit_reserve_bottom` bottom-reserve hook and the `g_x.shared` (shares-QEMU's-region) path. Ours redesigned the arena (own arena, exec view, W^X verdicts) and dropped the bottom reserve; re-adding it into our 97 KB file is a merge, not a copy. |
| `husk-tl-ld.c` | `tl_ld_main_apk`, `tl_ld_extract_lib`, `add_search_dir`, `fetch_from_dirs`, `find_loaded_ns`, `mono_aot_code_only`, `data_page`, `MAX_LIBS 512`, `TL_INIT_TRACE`, the app/system search-dir split (21 upstream-only code lines and 36 upstream-only identifiers). |
| `husk-tl-ld.h` | `#define TL_LD_MAX_APKS 48` (ours 16). Overwriting would LOWER the asset-pack-split cap — a regression, not a port. |
| `husk-tl-unity-app.c` | the console/host + document-picker + keyboard + extra-fonts integration (`husk_console_loaded_game`, `husk_unity_text` family, `husk_java_*` pickers, `tl_set_extra_fonts_dir`). |
| `husk-tl-unity-app.h` | same surface, declared. |
| `husk-tl-bionic.c` | `tl_bionic_uname`, `libvulkan.so.1`, the `TL_SYSPROPS` block. (Ours adds the exit family + hot-path counters — a real merge both ways.) |
| `husk-tl-bionic-io.c` | the `arm_thread_state64_*` guest-ucontext accessors, `F_GETPATH`, xattr. |
| `husk-tl-bionic-ldbl.c` | `__signbit`, `capget`/`capset`, xattr, `getgr*_r`, `splice`, `readv`/`sync`/`getsid`/`strtoimax`. |
| `husk-tl-bionic-net.c` | the `gethostbyname{,2,byaddr}_r` family + `he_copy`/`he_finish` + `in6addr_*` (40 upstream-only code lines). |
| `husk-tl-bionic-pthread.c` | `b_attr_getdetachstate` / `pthread_attr_getdetachstate`. |
| `husk-tl-egl-es31.inc` | `oes_external_rewrite` (the external-texture sampler rewrite). |

### Superset, but blocked on a symbol from a still-diverged file (4)

These four are clean supersets (measure 1 = 0) but were left because placing them **breaks the target's
compile or link** until the defining file is merged. Keep them with their partner file:

| file | blocker |
|---|---|
| `husk-tl-codewrite.c` | ours adds the Advanced SIMD ST1/ST2/ST3/ST4 structure-store families, `REFUSE()` naming each instruction, and `tl_ld_va_relro` RELRO accounting. **Its invariant is the one upstream's copy LOSES — port this one.** Blocked because it calls `tl_ld_va_relro` (declared only in our `husk-tl-ld.h`, defined in our `husk-tl-ld.c`) and `tl_xmem_emit_begin/end` (our `husk-tl-xmem.c`) ⇒ implicit-declaration error + undefined refs. Merge together with `ld.{c,h}` and `xmem.c`. |
| `husk-tl-bionic-ndk.c` | ours adds `aa_forget`, asset notes, the cached-zip path. Calls `tl_zip_data_cached` (our `husk-tl-bionic-io.c`), `tl_ld_apk_path_at` (our `husk-tl-ld.h`), `tl_load_add`/`tl_load_now_ns` (our `husk-tl-ld.c`) — three of them **undeclared** in the target ⇒ compile error. |
| `husk-tl-bionic-sys.c` | ours adds `getgrgid`/`getgrnam` and replaces upstream's `abort()` in `__assert` with `tl_guest_exit_unwind` (guest assert ends the APP, never Husk). Calls `tl_guest_exit_unwind` (declared in the ported `bionic.h`, **defined only in our `husk-tl-bionic.c`**) ⇒ link error. |
| `husk-tl-bionic-cxx.c` | 1-line superset: adds `husk_tl_guest_exit_requested("kill(self)", sig)` to the `kill` shim. Declared in the ported `bionic.h`, **defined only in our `husk-tl-bionic.c`** ⇒ link error. |

## Invariants from the brief — status

- **Executable view is NEVER written; `adopt_exec_view` issues no `vm_protect`; guest stores go through the
  writable twin.** Preserved in `husk-tl-vulkan.m` (no xmem interaction). The guard that embodies it
  (`husk-tl-codewrite.c`) is left for the orchestrator — and specifically so it is **not** replaced by
  upstream's weaker copy that lacks the SIMD families. Nothing in the 5 placed files issues `vm_protect`.
- **The store guard carries the Advanced SIMD structure-store families; a refusal names its instruction.**
  Confirmed intact in our `husk-tl-codewrite.c` (the `fam == 0x0C000000|0x0D000000` block + `REFUSE(...)`
  with the mnemonic/bytes); upstream's copy has neither. Left (blocked) — flagged as the priority merge.
- **Nothing the guest holds is unmapped; no thread that ran guest code is ended or joined (quarantine only).**
  No placed file unmaps guest memory or joins threads. The census/exit-family declarations in the ported
  `husk-tl-bionic.h` are reporting-only.
- **Arena-size honest range (64–2048 MiB)** and the `grant_self` halving / 2 GiB twin bound live in
  `husk-tl-xmem.c` (left). Our ported `husk-tl-xmem.h` keeps `TL_XMEM_PAGE 16384` and the region API
  unchanged.
- **Host type larger than bionic's is a silent stack smash (the pthread family).** The pthread shim is
  left (upstream added the attr getter); no placed file introduces a host/bionic size mismatch.

Nothing outside `src/translation-layer-next/` was touched. No git command was run inside `husk3.0`
(it is not a repo). This report is committed in the SOURCE repo only.
