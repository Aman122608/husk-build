# Step 3f — the bionic consumers (`ndk`, `sys`, `cxx`, `ldbl`, `net`, `pthread`, `egl-es31.inc`)

Target: `C:/Users/Aiden/Desktop/husk3.0/src/translation-layer-next/` (UPSTREAM — **not a git repo; no git was run in it**).
Source: `C:/Users/Aiden/Desktop/HuskApp/` (our fork). Method: `diff -u`, then a comment/string-stripped
identifier-set comparison, then `cp`/targeted edit. **No compiler, no Mac, no iOS SDK here: correctness is by
construction and no build is claimed.**

## Headline: the brief's direction is REVERSED for `ldbl` (and `net`/`egl` are upstream-newer too)

The brief calls `ndk/sys/cxx/ldbl` "a clean superset of ours". That is **true for `ndk`, `sys` and `cxx`** and
**false for `ldbl`**. Measured on disk and against our own history:

| file | `ldbl` claim vs reality |
|---|---|
| `husk-tl-bionic-ldbl.c` | the brief says ours adds `b___signbit{,f}`, `capget/capset`, the xattr family, `b_getgr_r`, `b_splice`, `b_enotsup`, `readv`/`strtoimax`. **Ours contains none of them** (`grep -c` = 0 in our copy) and **upstream has all of them** (`git log -S b___signbit -- …/husk-tl-bionic-ldbl.c` is empty in our repo, i.e. the symbol was never ours). This matches the earlier step-3 finding (`step3-layer-core.md` line 24 lists them as *upstream-only*). Ours' only unique content here is the `b_dup3` fix. Placing our copy wholesale would have **deleted upstream's signbit/capget/xattr/getgr_r/splice/enotsup/readv/strtoimax work** — a regression. Treated as a two-way merge instead. |
| `husk-tl-bionic-net.c` | ours has **no** unique functional content (ours-only identifier set = empty); upstream carries the whole RPC hostent marshalling. Merge = ours + upstream's block. |
| `husk-tl-egl-es31.inc` | ours has **no** unique content; it had *removed* `oes_external_rewrite`. Merge result = upstream's file (see below). |

`ndk` is a superset with exactly one substitution (`tl_zip_data` → `tl_zip_data_cached`) plus one upgraded
function (`b_AAudioStream_close` gains `aa_forget`); `sys` and `cxx` are strict supersets (upstream-only
identifier set empty).

## Per-file result

| file | verdict | what was done | upstream work preserved |
|---|---|---|---|
| `husk-tl-bionic-ndk.c` | **placed (superset)** | our copy copied verbatim (`cmp` OK). Adds `aa_forget` + the AAudio live list + `tl_aaudio_stop_all`, `tl_asset_read_note` instrumentation, the cached-zip path, `tl_ld_apk_path_at`/`tl_load_add`/`tl_load_now_ns` accounting, and the `calloc` NULL guard. | Every upstream HEAD line is present in the result except two **deliberate substitutions**: `tl_zip_data(...)` → `tl_zip_data_cached(...)` (inflate-once cache, ours' newer form) and upstream's `b_AAudioStream_close` line, which ours extends with `aa_forget(s)` so a session close cannot hold a freed stream. |
| `husk-tl-bionic-sys.c` | **placed (superset)** | our copy copied verbatim. Adds `getgrgid`/`getgrnam` (honest NULL) and replaces upstream's `abort()` in `__assert` with `tl_guest_exit_unwind("__assert", 6)`. | Every upstream HEAD line except `abort();` — a **deliberate substitution**: a guest assert must end the APP, never Husk. |
| `husk-tl-bionic-cxx.c` | **placed (superset)** | our copy copied verbatim — a 1-line superset: `husk_tl_guest_exit_requested("kill(self)", sig)` added to the kill shim (before the existing `tl_guest_exit_hook`). | All upstream HEAD lines present (0 missing). |
| `husk-tl-bionic-ldbl.c` | **merged** (upstream base + our fix) | started from upstream's file (which is the superset here) and inserted ours' `b_dup3` block: `dup3` now routes through the guest's `dup2` shim via `tl_bionic_find("dup2")` so the layer's descriptor→entry table stays true. `#include <inttypes.h>` kept. | **All** of upstream's newer work retained: `b___signbit`/`b___signbitf` (+ their `TL_WRAP`s), `b_capget`/`capset`, the whole xattr family (`getxattr`…`fsetxattr`), `b_getgr_r` (`getgrgid_r`/`getgrnam_r`), `b_splice`, `b_enotsup`, `TL_DIRECT(readv)`, `TL_DIRECT(strtoimax)`/`strtoumax`, `settimeofday`/`adjtime` `b_eperm`. Only upstream line not carried verbatim is `int r = dup2(old, nw);`, which is exactly what ours replaces. |
| `husk-tl-bionic-pthread.c` | **merged** (ours base + upstream's getter) | our copy placed, then upstream's `b_attr_getdetachstate` re-added as a function (before `b_attr_setstacksize`) and in the table. | **The attr getter is back**: `pthread_attr_getdetachstate` → `b_attr_getdetachstate`, which reads the `GATTR_DETACHED` bit back out of the guest's own 56-byte `guest_attr` — it never touches a host `pthread_attr_t`, so upstream's getter does **not** reintroduce the host-type-larger-than-bionic hazard. Ours' additions all retained (thread census `gt_enter`/`gt_leave`/`gt_wait_*`/`tl_guest_thread_census`, `mutex_trace` made swap-aware, `tl_ld_symbol_at_any`, `b_attr_setstack`, the `setname` one-line-per-name log, the `TL_MT_*` distinct-site table, `tl_hp_bump` hot-path counters). Upstream's 9 "missing" lines are all lines ours deliberately rewrites in place (e.g. upstream's `mutex_trace` body survives *inside* ours' swap-aware version; `tl_ld_symbol_at` → `tl_ld_symbol_at_any`), none is dropped functionality. |
| `husk-tl-bionic-net.c` | **merged** (ours base + upstream's block) | our copy placed, then upstream's RPC hostent marshalling inserted verbatim: `he_lock`, `he_copy`, `he_finish`, `b_gethostbyname_r`, `b_gethostbyname2_r`, `b_gethostbyaddr_r`; the `g_in6addr_any`/`g_in6addr_loopback` data and their `TL_DATA` entries; the three `TL_WRAP` hostent entries. | **All 27 upstream-only identifiers now present; 0 upstream HEAD lines absent.** The RPC hostent family is `bionic`'s `gethostbyname_r`/`gethostbyname2_r`/`gethostbyaddr_r` (Python's `socket` module uses them) with a lock around the lookup and the result copied into the caller's buffer (`ERANGE`/`HOST_NOT_FOUND`/`*herr` as bionic). Ours contributed no unique net content, so nothing of ours was at stake. |
| `husk-tl-egl-es31.inc` | **left as upstream's copy (no write needed)** | ours contributed **no** unique lines (ours-only identifiers = empty), and it had *removed* `oes_external_rewrite`. The correct merge result is therefore upstream's file, and the target **is byte-identical to upstream HEAD** (`diff` OK). No byte was changed. | `oes_external_rewrite` (the `GL_OES_EGL_image_external` / `samplerExternalOES` → `sampler2D` rewrite) and, crucially, its **placement**: upstream applies it *before* the `if (!g_es31_shim)` branch. That ordering is load-bearing — `w_glShaderSource` is installed unconditionally through `k_adapt`/`ADAPT(glShaderSource)` in `husk-tl-egl.c`, so the rewrite must run for non-3.1-shim apps too (it exists for Flutter/Impeller, which is not a 3.1-shim app). Placing ours, whose `!g_es31_shim` early-return passes the original source straight through, would have disabled the rewrite for exactly the apps it was written for. |

## Provider symbols still missing at finish — REPORTED, not defined here

Every symbol below is **declared** in the target's headers but **not yet defined** in the target, because the
providers that own them (`husk-tl-bionic.c`, `husk-tl-bionic-io.c`) are **still unmerged** (both still at
upstream's original 20:31 mtime / original bytes) when this step finished. The declarations mean the placed
files **compile**; **linking** needs those two provider merges.

| symbol | needed by (this step) | declared | defined in our fork |
|---|---|---|---|
| `husk_tl_guest_exit_requested` | `husk-tl-bionic-cxx.c` (also `husk-tl-jni-minecraft.c`) | `husk-tl-bionic.h:71` ✅ | our `husk-tl-bionic.c:436` |
| `tl_guest_exit_unwind` | `husk-tl-bionic-sys.c` (also `husk-tl-jni.c`) | `husk-tl-bionic.h:81` ✅ | our `husk-tl-bionic.c:460` |
| `tl_asset_read_note` | `husk-tl-bionic-ndk.c` | `husk-tl-bionic.h:147` ✅ | our `husk-tl-bionic-io.c:794` |
| `tl_zip_data_cached` | `husk-tl-bionic-ndk.c` (also `husk-tl-ld.c`) | `husk-tl-internal.h:114` ✅ | our `husk-tl-bionic-io.c:185` |
| `tl_load_add` | `husk-tl-bionic-ndk.c` (also `husk-tl-ld.c`) | `husk-tl-internal.h:132` ✅ | our `husk-tl-bionic-io.c:122` |
| `tl_load_now_ns` | `husk-tl-bionic-ndk.c` (also `husk-tl-ld.c`) | `husk-tl-internal.h:133` ✅ | our `husk-tl-bionic-io.c:116` |

Already present in the target (no action needed): `tl_bionic_find` (our `husk-tl-bionic.c`, still upstream's
copy — but upstream's own `husk-tl-bionic.c` **does** export it, so the `ldbl` `b_dup3` change links),
`tl_ld_apk_path_at` + `tl_ld_symbol_at_any` (`husk-tl-ld.c`, merged), `tl_swap_in_progress` (`husk-tl-xmem.c`,
merged). `tl_aaudio_stop_all` is **defined by our `husk-tl-bionic-ndk.c` itself** and its declaration landed in
`husk-tl-audio.h` during this step (audio.h was merged by another worker at 22:28), so that consumer is whole.

None of the six missing symbols was defined or declared by me anywhere — per the brief, they are the provider
pass's to supply.

## Invariants

- **A host type larger than bionic's is a silent stack smear.** Nothing placed reintroduces a raw host type:
  `pthread_attr_getdetachstate` (re-added) reads the guest's own `guest_attr.flags` bit; the pthread family
  stays shimmed; `ndk`'s additions are the layer's own structs.
- **bionic's `O_*`/`sysconf`/`MAP_*` constants are not Darwin's.** No placed file adds a passthrough of them.
  `b_dup3` still expresses `O_CLOEXEC` as bionic's `0x80000` and sets it with a *host* `fcntl(F_SETFD,
  FD_CLOEXEC)` — correct (the bit is bionic's in the guest's argument; `FD_CLOEXEC` is the host's own value).
- **A guest assert or `kill(self)` ends the APP, never Husk** — `sys.__assert` → `tl_guest_exit_unwind`,
  `cxx` kill shim → `husk_tl_guest_exit_requested`. Upstream's `abort()` is gone from `__assert`.
- **Nothing the guest holds is unmapped; no thread is ended or joined.** The pthread census is reporting-only
  (a counter + a lock-free slot table, no stop/suspend/join), exactly as ours' own comment states.

## Verified / not verified

Verified on disk (all 7 files): comment/string-stripped `(){}[]` balances are `0`; `#if/#ifdef/#ifndef` depth
returns to `0` with no stray `#endif`; every merged file is all-LF (no CRLF introduced); the three copied files
are `cmp`-identical to our fork; **every upstream HEAD line is present** in the merged files except the
documented, deliberate substitutions (`net` 0 missing, `egl` byte-identical, `ldbl` 1, `pthread` 9, `ndk` 2,
`sys` 1, `cxx` 0 — each a line our fork rewrites in place, none dropped functionality); no upstream-only
identifier remains missing; no ours-only identifier was lost; no function is accidentally duplicated.

**Not verified / not done here:**
- **No compile, no link, no run** — there is no `cc`/`clang`/`zig`/`tcc` on this box. The link is *known*
  incomplete until `husk-tl-bionic.c` and `husk-tl-bionic-io.c` are merged (see the table above).
- The target's `husk-tl-bionic.c`/`husk-tl-bionic-io.c` were not read line-by-line (out of scope) — I only
  checked whether the six symbols were defined; they are not.
- `husk-tl-egl-es31.inc` was **not written** (its merge is a no-op). If another worker has since changed the
  target's copy, re-check it against upstream HEAD before treating this as settled.
- Nothing outside `src/translation-layer-next/` was touched. No git command was run inside `husk3.0`.
