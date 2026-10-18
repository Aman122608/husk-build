# Step 3d — the provider pass: `husk-tl-bionic-io.c` + `husk-tl-bionic.c`

Scope: the two **provider** files, in `C:/Users/Aiden/Desktop/husk3.0/src/translation-layer-next/`.
Method: **ours is the base** — each target file was replaced with our fork's copy
(`cp`, `cmp`-verified) and upstream's newer work was then inserted into it, diff-driven and symbol
by symbol, so no fix of ours can be lost to a "copy theirs over ours".

| file | ours (base) | upstream | merged |
|---|---|---|---|
| `husk-tl-bionic-io.c` | 2,544 lines / 145,685 B | 1,930 / 107,469 | **2,671 / 153,499** |
| `husk-tl-bionic.c` | 972 / 47,364 | 914 / 44,551 | **987 / 48,507** |

**No compiler and no Mac here** (step3b confirmed no `cc`/`gcc`/`clang`/`zig`/`tcc` on this host), so
**no build was made and none is claimed**: correctness is by construction only.

The upstream bytes this merge started from were **verified first**: the target's two files were
byte-identical (`cmp`) to `raw.githubusercontent.com/Leviidev/Husk/main/…` for both — i.e. the target
still held upstream's pre-merge state. (Upstream main's `bionic.c` is **44,551 B**, not the brief's
29,909 B; the brief's figure was stale, the file has advanced.) Upstream's pre-merge bytes are parked
**outside both trees** at `Desktop/.husk3-merge-providers/{ours,upstream}/` — `.husk3-merge/` was left
undisturbed, and the same bytes are re-fetchable from upstream main regardless.

## Upstream-only work — item by item

### `husk-tl-bionic.c`

| item | verdict | where |
|---|---|---|
| `TL_SYSPROPS` injection block | **INTEGRATED** | in `bionic___system_property_get`, after `tl_sysprop()` and before the `!value` guard |
| `tl_bionic_uname` | **INTEGRATED** | after `bionic_uname()`: `int tl_bionic_uname(void *buf) { return bionic_uname(buf); }` |
| `libvulkan.so.1` (Godot 4's versioned soname) — dlopen gate | **INTEGRATED** | `dlopen_impl`'s Vulkan gate now `!strcmp(base,"libvulkan.so") \|\| !strcmp(base,"libvulkan.so.1")` |
| `libvulkan.so.1` in the known system-library list | **INTEGRATED** | the `sys[]` name list |
| `tl_ld_sym_deep` for a handle's `dlsym` | **INTEGRATED** | the library-then-its-dependencies breadth-first lookup, upstream's comment kept. Defined in the already-merged `husk-tl-ld.c` (647/667) and declared in `husk-tl-ld.h:102`, so it links. |
| raw `abort()` in the four abort paths | **DELIBERATELY NOT** | ours routes each through `tl_guest_exit_unwind(…, SIGABRT)` — a guest abort/assert ends the APP, never Husk. Upstream's raw `abort()` is the older behaviour. |
| `exit(status)` in `bionic_exit` / `TL_DIRECT(_exit)` | **DELIBERATELY NOT** | ours: `bionic_exit`/`bionic__exit` go through `tl_guest_exit_unwind`, and `_exit`/`_Exit` are `TL_WRAP`ped rather than passed through. Same reason. |

Ours-only work preserved verbatim: the hot-path counters (`tl_hp_count`, `tl_hp_name`), the whole exit
family (`tl_guest_exit_unwind`, `husk_tl_guest_exit_requested`, `tl_guest_exit_set_close_in_flight`,
`g_guest_exit_close_in_flight`, `bionic__exit`), and the `abort()`→unwind substitutions.

### `husk-tl-bionic-io.c`

| item | verdict | where |
|---|---|---|
| `const char *tl_ld_main_apk(void);` forward decl | **INTEGRATED** | top of file, between the `husk-tl-bionic.h` and `husk-tl-internal.h` includes (upstream's own placement; `tl_ld_main_apk` is defined in the merged `husk-tl-ld.c:170`, declared in no header) |
| OBB kept beside the APKs (`obb/<file>`) | **RECONCILED** | added as the last resort of the `/sdcard` branch — see the conflict below |
| `g_extrafonts` + `tl_set_extra_fonts_dir` (#95, the CJK fonts the bundle omits) | **INTEGRATED** | declared next to `g_sysfonts`; consumed by the `/system/fonts` branch of `tl_path_resolve` |
| the `/system/fonts` extra-fonts fallback | **INTEGRATED** | in `tl_path_resolve`'s `g_sysfonts` branch: a bundle font that is absent is then looked for under the extra-fonts dir |
| `fill_lib_standin` + `fill_lib_standin_at` (+`F_GETPATH`) | **INTEGRATED** | defined after `tl_note_missing_file`; called from `b_open` (before `/auxv`, as upstream), `b_fstatat`, and `b_openat` (both behind `(flags & 3) == 0`) |
| `shifted_file_map` / `shifted_unmap` (+`g_shifted`/`g_nshifted`/`g_shifted_lock`, `getpagesize`) | **INTEGRATED** | defined immediately before `b_mmap`; the `bool shifted = …` test and the `: shifted ? shifted_file_map(…)` arm restored in `b_mmap`'s mmap call; `shifted_unmap` called from `b_munmap` before `phantom_clamp` |
| the `arm_thread_state64_*` guest-ucontext accessors + `_STRUCT_ARM_THREAD_STATE64` | **INTEGRATED** | `guest_sigcontext`/`guest_ucontext` typedefs + the two `_Static_assert`s immediately before `host_signal_entry`, which now builds the bionic layout from Darwin's and copies the interrupted registers back after the handler |
| the xattr family (`getxattr`…`fsetxattr`) | **DELIBERATELY NOT — wrong file** | the brief puts them in `bionic-io.c`; **they are in `husk-tl-bionic-ldbl.c`** (upstream: `TL_WRAP("getxattr", b_enotsup)` …), which is outside this pass's scope. That file is listed as diverged in `step3-layer-core.md` and still needs its own merge. |

Ours-only work preserved verbatim: the inflate-once zip cache (`tl_zip_data_cached` + `load_fnv` /
`load_entry_identity` / the `.lib-cache`), the load-time accumulator (`g_load_ns`/`g_load_bytes`/
`g_load_calls`, `tl_load_now_ns`/`tl_load_add`/`tl_load_summary`), `tl_path_resolve` + `tl_note_missing_file`
(ours calls `tl_note_missing_file(real)`/`(real)`, the **host** path it looked in), `tl_renpy_android_on`
+ `renpy_hidden_macver`, `note_obb_used`, `synth_dir`, the whole vfd machinery (`vfd_copy`/`vfd_drop`/
`vfd_set`/`vfd_take_over`/`vfd_note_*`, the wider `vfd` struct with `was`+`name`), `jar_unescape` +
`csize` in `jar_entry` + the fingerprinted `jar_inflated`, the vfd-aware `open`/`read`/`pread`/`readv`/
`preadv`/`lseek`/`close`/`dup`/`dup2`/`sendfile`, `tl_io_reset_for_new_app`, `tl_asset_read_note`,
`runtime_data_path`/`tl_note_missing_data`, the rw-exec memory note (`g_rw_app_needs_exec`,
`rw_exec_memory_{load,record}`, `rw_exec_refuse_log`), and **`kernel_dst`/`iov_dst`/`vfd_note_dst` — the
read-into-the-writable-twin fix for the kernel-write EFAULT**, and the vfd-aware raw syscalls
(`case 57/62/63`) plus `preadv` in the syscall table.

## Conflicts reconciled (the two-way choices)

1. **`/sdcard` and `Android/obb` — `tl_path_resolve`.** Upstream *excludes* `/Android/obb` from shared
   storage and gives the OBB a `obb/<name>`-beside-the-APK fallback via `tl_ld_main_apk()`; ours *includes*
   it, deliberately, with the comment "Android/obb is included on purpose". **Both are kept**: ours'
   shared-storage branch is untouched (the deliberate fix stands), and upstream's beside-the-APK lookup is
   restored as the fallback of the private `/sdcard` mirror, exactly as upstream wrote it. Upstream's
   `strncmp(rest, "/Android/obb", 12)` exclusion is the one upstream line **not** taken, and that is the
   reconciliation.
2. **`dlsym(handle, name)` — `tl_ld_sym` vs `tl_ld_sym_deep`.** Upstream's deeper lookup was taken (our tree
   already defines `tl_ld_sym_deep` in `ld.c` and declares it in `ld.h`; the shallower `tl_ld_sym` was the
   older state).
3. **The JIT-region branch of `b_mmap` returns `rx` upstream, ours returns `rw` for an app not remembered
   as needing exec.** **Ours wins** (the trap-free writable view is the whole point of the rw-exec note);
   the `_Static_assert`/ucontext work is unrelated and was taken.

## The MUST SUPPLY list — all DEFINED

| symbol | file:line (target) | note |
|---|---|---|
| `tl_zip_data_cached` | `husk-tl-bionic-io.c:186` | the inflate-once cache; declared `translation-layer/husk-tl-internal.h:114` |
| `tl_load_add` | `husk-tl-bionic-io.c:123` | declared `husk-tl-internal.h:132` |
| `tl_load_now_ns` | `husk-tl-bionic-io.c:117` | declared `husk-tl-internal.h:133` |
| `tl_load_summary` | `husk-tl-bionic-io.c:138` | declared `husk-tl-internal.h:134` |
| `tl_path_resolve` | `husk-tl-bionic-io.c:363` | each caller declares it locally (`bionic-str.c:34`, `-net.c`, `-sys.c`, `jni-hle.c`, `ld.c`, …) |
| `tl_note_missing_file` | `husk-tl-bionic-io.c:1009` | declared locally by `bionic-str.c:252` |
| `tl_renpy_android_on` | `husk-tl-bionic-io.c:341` | declared locally by `husk-tl-python.c:165` |
| `tl_guest_exit_unwind` | `husk-tl-bionic.c:468` | declared `husk-tl-bionic.h:81` |
| `husk_tl_guest_exit_requested` | `husk-tl-bionic.c:444` | declared `husk-tl-bionic.h:71` |
| `tl_opensl_stop_all` | `husk-tl-opensles.c:235` | **not** in bionic.c/io.c — its real home in our tree is the OpenSL shim; already declared `husk-tl-audio.h:37` |
| `tl_dvm_audio_stop` | `husk-tl-dvm-audio.c:147` | its real home is the DVM mixer; already declared `husk-tl-audio.h:38` |

Nothing in the list is still missing. Each is defined **exactly once** across
`src/translation-layer-next/*.c` (checked); the other files that name them only call them.

**No edit to `husk-tl-bionic.h` was needed.** It already declares the exit family
(`husk_tl_guest_exit_requested`, `tl_guest_exit_unwind`) from the core worker's port; the zip/load
symbols come from `husk-tl-internal.h`, the audio stops from `husk-tl-audio.h`, and
`tl_path_resolve`/`tl_note_missing_file`/`tl_renpy_android_on` are declared locally by each caller. So the
header was left exactly as the core worker left it — adding anything would have been noise.

## Verification performed (and what is not verified)

- **Byte-diff driven.** Our base was restored with `cp` and `cmp`-checked; every upstream block inserted
  was extracted and confirmed **byte-identical to upstream's text** (verbatim check on the top declaration,
  the extra-fonts pair, the `/system/fonts` branch, the OBB-beside-APK block, `fill_lib_standin`(+`_at`),
  the whole shifted block, the ucontext typedefs, and the one-line edits: the `shifted` test, the
  `shifted_file_map` arm, the `shifted_unmap` call, the three `fill_lib_standin` call sites, the six
  `arm_thread_state64_*`/`_STRUCT_ARM_THREAD_STATE64` lines, the two `_Static_assert`s, `case 160`).
- **Re-diff against upstream** after the merge: the only upstream lines *not* in the merged file are those
  ours deliberately supersedes (the abort/exit/_exit family, the shallower `dlsym`, the older
  `jar_lookup`/`jared`/`b_open`/`b_read`/`b_pread64`/`b_lseek`/`b_close`/`b_dup`/`b_access`/`b_stat`/`b_fcntl`/
  `b_sendfile`/`b_clock_gettime` bodies, the syscall `case 57/62/63` and the table entry without `preadv`,
  the unmatched `return rx`, the `/Android/obb` exclusion) **plus comment rewordings** — no upstream *work*
  left unintegrated.
- **Structure, with a single-pass C lexer** (strings handled before comments, since this file contains
  `/*` inside string literals — a naive strip reports a phantom imbalance): merged `bionic-io.c` and
  `bionic.c` both have **brace/paren/bracket balances 0**, **`#if` depth 0**, **no stray `#endif`**.
- **Undefined-symbol sweep** over the two files (every `tl_*`/`husk_*` identifier they name): all resolve
  inside the target tree — including the cross-file ones (`tl_ld_find_lib` `ld.c:493`,
  `tl_ld_symbol_at` `ld.c:736`, `tl_guest_thread_epoch` `bionic-pthread.c:528`, `tl_timerfd_is`
  `bionic-sys.c:277`, `tl_hle_package`/`tl_hle_version_code` `jni-hle.c:1178/1180`,
  `tl_codewrite_enabled` `codewrite.c:41` — and `codewrite.c` is already `cmp`-identical to ours).
- **Line endings** follow each file's own style: both files were all-LF in our tree and remain all-LF
  (0 CRLF of 2,671 / 987 lines).
- **Not verified:** no compile, no link, no run. In particular the `arm_thread_state64_*` accessors and
  `_STRUCT_ARM_THREAD_STATE64` are taken verbatim from upstream (which compiles them in CI against the
  iPhoneOS SDK) and rely on `<mach/arm/thread_status.h>` arriving transitively — as it does for upstream,
  whose include set is the same as ours; that is an assumption, not a measurement. `F_GETPATH` likewise
  rides on Darwin's `<fcntl.h>`.
- Nothing outside `src/translation-layer-next/` was written. **No git command was run inside `husk3.0`**
  (it is not a repo). This report is committed in the SOURCE repo only.
