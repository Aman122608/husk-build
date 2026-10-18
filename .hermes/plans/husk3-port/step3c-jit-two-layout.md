# Step 3c — restoring upstream's two-layout mmap probe into `src/ios-jit/husk-ios-jit.c`

Target: `C:/Users/Aiden/Desktop/husk3.0` (upstream, **not a git repo — no git was run in it**).
Source of the commit: `C:/Users/Aiden/Desktop/HuskApp` (git).

## What the second layout is — recovered exactly

The dropped feature is upstream's **two-layout probe loop** inside `husk_self_dual_mapping()`,
in the *self / plain* branch (the no-trap-servicer route: TrollStore, a jailbreak, or a
debugger-that-let-go, on iOS < 26). It is **not** in the StikDebug/`husk_brk_get_jit_mapping`
path.

Two layouts, asked for in order over the same reserved `base` (`size * 2`, `MAP_FIXED`):

1. `"rwx then rx"` — mmap `PROT_READ|PROT_WRITE|PROT_EXEC`, then `vm_protect` the exec view down to
   `PROT_READ|PROT_EXEC`. The kernel drops execute from the first mapping, so the page is usable
   as `r-x` only after the second step.
2. `"rx"` — mmap `PROT_READ|PROT_EXEC` from the start, the way UTM and Dolphin map it.

```c
static const int first_prot[2] = { PROT_READ | PROT_WRITE | PROT_EXEC, PROT_READ | PROT_EXEC };
static const char *const layout[2] = { "rwx then rx", "rx" };
```

Why the second exists: the kernel does **not** always keep the shape that was asked for. Upstream
added the loop for issue #127 — an **A15 on iOS 18 with `CS_DEBUGGED` set** where the plain route
failed, and the previous **single** attempt (mmap RWX once + `vm_protect` to `R|X`) could not say
*which* step had failed. The loop names each failing step (`mmap failed` / `no writable alias` /
`vm_protect r-x failed` / `the kernel did not keep execute on the page` / `code ... faulted or
returned the wrong value`) and, crucially, **tries the second layout rather than giving up on the
first**. This is the robustness fallback our port dropped; it is not a QEMU feature.

### Where the original was recovered from

The tree no longer contains upstream's copy (the port overwrote it in place), and upstream's
symbols were never in our fork's history (`git log -S` returns empty), so the deleted text could
not be read from either tree. It was recovered from upstream itself:
`https://raw.githubusercontent.com/Leviidev/Husk/main/src/ios-jit/husk-ios-jit.c`.

Confirmation that this is the removed loop and not new upstream work:

- The report's own description of what ours does instead — "mmaps `PROT_READ|PROT_WRITE|PROT_EXEC`
  once and `vm_protect`s to `R|X`, then self-tests" — matches the *single-attempt* branch that was
  in the ported file verbatim, i.e. the loop is the exact counterpart that was removed.
- The loop's comment cites the **A15 / iOS 18 / #127** shape the step-3 report quoted
  ("an A15/iOS-18 shape mentioned in the removed comment").
- Every other line of `husk_self_dual_mapping` — the forward declaration of
  `husk_page_is_executable`, `husk_self_fault`, `husk_self_selftest`, `husk_rw_alias`, the MAP_JIT
  branch — is **byte-identical** between upstream main and the tree's file. The only delta in the
  whole function is this loop, so main and the tree's release agree on it.

## Where the fallback now lives

`C:/Users/Aiden/Desktop/husk3.0/src/ios-jit/husk-ios-jit.c`, in `husk_self_dual_mapping()`,
branch `/* 2. Plain memory made executable ... */`:

- comment — lines **459–466**
- `first_prot[2]` / `layout[2]` — lines **467–468**
- the `for (int i = 0; i < 2; i++)` loop — lines **469–498**

The surrounding self path is unchanged and matches upstream's idiom (`HUSK_LOG`, the fault-guarded
`husk_self_selftest`). Added +30 lines to the file (997 lines now). The `MAP_FIXED` reuse of `base`
means the second attempt overwrites the first's mapping in place, exactly as upstream does.

## How it triggers

Only when `husk_ios_jit_allocate_real()` finds no trap servicer (`husk_brk_probe() == 0`) and calls
`husk_self_dual_mapping()`, on iOS < 26, where the process is debugged or jailbroken. Then layout 1
is tried; **any** failed step falls through to layout 2 over the same `base`; the first layout whose
`mmap` + writable alias + `vm_protect(r-x)` + kernel-execute check + self-test all pass is kept. If
both fail, the function reports `self: plain memory did not execute (...)` and returns none, as
before.

## The non-adjacent case is already handled — and is a different code path

The real device log the task cites —
`dual mapping established: rw=0x7000000000 rx=0x12f000000 diff=-475952840704` vs the usual
`diff=-536870912` — is printed only by **`husk_ios_jit_allocate_real()`**, i.e. the *debugger*
path, never by `husk_self_dual_mapping()`. The two-layout probe does **not** address adjacency.

That path already carries the adjacency handling, and it is **identical to upstream main**: the
writable-alias placement search at **lines 671–690**

- `const uintptr_t step = 16u << 20, reach = (uintptr_t)2 << 30;` — line **672**
- the `for (off ...) for (side ...)` loop that `vm_remap`s `VM_FLAGS_FIXED` after `rx`, then before
  it, in 16 MiB steps out to 2 GiB — lines **673–682**
- the last-resort `VM_FLAGS_ANYWHERE` `vm_remap` ("no free space within 2 GiB ... placing it
  anywhere") — lines **683–689**

So: the layer *does* try again when the first placement is unavailable (it walks ±2 GiB before ever
going anywhere-else), and it does so in both trees. What it does **not** do — in either tree — is
refuse a non-adjacent result; `VM_FLAGS_ANYWHERE` is the deliberate last resort, and
`diff=-475952840704` is that fallback firing. Upstream's two-layout loop is a *protection-shape*
fallback in the self path; it is not, and was never, an adjacency probe. Nothing about the
non-adjacent outcome is changed by this restore, and nothing needed to be — the debugger-path
placement retry was already present and untouched.

## What I could NOT recover

- **The exact upstream file revision this tree came from.** Only upstream *main* is reachable
  (no local clone, no release tarball in either tree, no `.orig`/`.bak`/`.rej`, and our fork's
  history never held the file). I recovered the loop from main and justified it as the removed text
  by the three checks above; it is **not** byte-proven against the tree's specific release, only
  against main, which is byte-identical to the tree across the rest of `husk_self_dual_mapping`.
  If a newer main had *also* changed this loop, this is where that would be wrong — I found no
  evidence of that.

## What I could NOT verify

- **No compiler, no Mac, no iOS SDK.** No build was run and none is claimed; **a compile on device
  is where this gets judged.** Correctness asserted by construction only: the merged region re-read
  in place; after stripping comments and literals the file has `()` 478/478, `{}` 116/116,
  `[]` 37/37, and the `#if/#else/#endif` chain is intact (`#ifdef __APPLE__` 6/8,
  `#if ... TARGET_OS_IPHONE` 10 → `#else` 756 → `#endif` 780, `__has_include` 69/73,
  `HUSK_HAS_QEMU_JIT_HEADER` 75/98). No new identifier is used that was not already declared
  (`husk_page_is_executable` is forward-declared at line 359; `kern_return_t`, `vm_protect`,
  `mach_error_string`, `MAP_FIXED` all already in scope in this file).
- Nothing outside `src/ios-jit/husk-ios-jit.c` was written.

## Not restored, deliberately

`husk_ios_jit_reserve_bottom` and the two-user sharing statics (`husk_share_lock`,
`husk_runtime_high`, `husk_qemu_top`) and the QEMU top-of-region split in
`husk_ios_jit_allocate()` are **not** restored: they are coupled to the QEMU/dylib engine this tree
unbundled, and `scripts/integrate_husk.sh` (line 626) has already been edited to stop demanding the
export. The two-layout probe is independent of that mechanism and was restored alone.

`src/translation-layer-next/husk-tl-xmem.c` (another worker's) was not touched and this change
needs nothing from it.
