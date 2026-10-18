# Step 3h — `husk-tl-codewrite.c`: the store guard into downstream 3.0

Scope: exactly one file, placed from `C:/Users/Aiden/Desktop/HuskApp` (our fork) into
`C:/Users/Aiden/Desktop/husk3.0/src/translation-layer-next/husk-tl-codewrite.c` (target).
No compiler and no Mac on this box: **correctness by construction only, no build made or claimed.**
Measured on disk with `diff`, a comment/string-stripped identifier comparison, a substantive-line test,
a token-subsequence test, a brace/paren/`#if`-depth script and a mask-disjointness check.

| file | ours (base) | upstream | placed |
|---|---|---|---|
| `husk-tl-codewrite.c` | 443 lines, 25,404 B | 207 lines, 9,318 B | ours verbatim, 443 lines |

Placed by `cp -p`; `cmp` confirms the target copy is **byte-identical** to our fork's
(`sha256 929fc3246742fe9a3207565e2726abd861c0443990831127b524cbe044a4cfee`), all-LF as both copies are
(`grep -c $'\r'` = 0). The target is **not a git repo** — no git command was run inside it. Upstream's
pre-merge bytes were already parked at `Desktop/.husk3-merge/upstream/` (not touched) and are in any case
recoverable: **upstream HEAD is byte-identical to the pre-merge target copy** (`sha256 0e16b153…`, target
copy and `raw.githubusercontent.com/Leviidev/Husk/main/src/translation-layer-next/husk-tl-codewrite.c`
match), so nothing of upstream's is unrecoverable.

## 1. Is ours a superset? — yes, and how it was proved (the brief was checked, not trusted)

The foundations worker measured upstream-only content here as NOTHING. Re-measured with three independent
methods, all on disk:

1. **Comment/string-stripped identifier sets.** Strip `//`, `/* */`, string and char literals, tokenize
   both copies; list identifiers present in upstream but absent from ours. **Result: 0.** No upstream-only
   identifier exists — every name, constant and macro upstream's copy uses is present in ours.
2. **Target-only substantive source lines.** Same strip, braces/`#endif` removed; lines of upstream whose
   exact text is absent from ours. **Result: 17 lines — all substitutions, none a loss** (classified below).
3. **Token-subsequence test.** `False` — our token stream is *not* an ordered supersequence of upstream's,
   because 17 upstream lines are *replaced* rather than purely added to. So the strongest form of "superset"
   does **not** hold here, unlike `husk-tl-vulkan.m` / `husk-tl-xmem.h`. The reason is entirely the
   `REFUSE(...)` rewrite and the `on_fault` restructure; the accept/reject decision upstream made is
   preserved in every case (see §2).

Also fetched and diffed against live upstream HEAD: identical to the target copy, so there is **no newer
upstream work** beyond what was already on disk.

## 2. Upstream content integrated — none needed; every target-only line is a preserved substitution

The 17 target-only lines, each classified. None drops an upstream capability:

| upstream line (target-only) | ours | decision change? |
|---|---|---|
| `if (!tl_xmem_is_rx(…) \|\| !tl_xmem_is_rx(…)) return false;` | first branch is the same RX test; on failure it now falls to a second `tl_xmem_contains(…)` branch that serves the **guest-facing view** in place, else `return false` | **Widens SERVICE only.** Upstream accepted an address iff it was RX; ours accepts it if RX **or** in the guest-facing view. Ours never refuses an address upstream accepted. |
| `static bool emulate(mcontext_t mc)` | `emulate(mcontext_t mc, const char **why)` | signature only; both call sites updated |
| `… if (!put(a, buf, bytes)) return false;` (×7) and `if ((insn>>22)&1) return false;`, `if (scale > 4) return false;`, `if (!is_store) return false;` (×3) | `REFUSE("<reason>")` | **none** — `REFUSE(txt)` is `do { *why = txt; return false; } while (0)`, i.e. the identical `return false` with the reason carried out |
| `return put(a, buf, bytes);` (×3) | `if (!put(…)) REFUSE(…); return true;` | none — returns `true` iff `put` succeeded, exactly as upstream |
| `if (g_on && info && tl_xmem_is_rx(info->si_addr) && emulate(u->uc_mcontext))` | `if (g_on && info) { if (tl_xmem_contains(info->si_addr)) { if (emulate(…)) {…} } else why = …; name_refusal(…); } chain(…);` | **Widens SERVICE only** (attempts the guard for either view). The refusal path is `chain` — the same call, same arguments, same place. |

**No upstream line is lost.** Nothing was integrated from upstream because there was nothing outside ours;
this file is not a two-way merge, it is our superset placed verbatim. This is the one file where the
foundations worker's "upstream-only = nothing" verdict was independently re-confirmed (measure 1 = 0), and
the earlier `diff` reads as a pure ours-adds diff.

## 3. The invariant that matters — preserved, with its exact conditions

### Advanced SIMD structure-store families (the fix that thawed robot gf)

Present ahead of the STP block, keyed on the family mask (`fam = insn & 0xBF000000`):

- single-structure lane stores `fam == 0x0D000000`, multiple-structure whole-vector stores `fam == 0x0C000000`;
- **Rn is the address, the register(s) the value(s), no pre-index form**; post-index bit 23 → Rm (bits[20:16])
  or, **with Rm == 31, an immediate of exactly the bytes transferred** (`nreg * per`) — line 212;
- single-structure opcode bits[15:13] give the element size; **bit 21 (R) selects ST1/ST3 vs ST2/ST4**;
- the lane index is `(Q, S, size)`: 8-bit `index=(q<<3)|(s<<2)|size`; 16-bit `(q<<2)|(s<<1)|(size>>1)` with
  `size&1` refused; 32-bit `(q<<1)|s`; 64-bit `index=q` with S refused — **Q (bit 30) is read only as the
  high lane-index bit, never as vector width**;
- multiple-structure: `per = q ? 16 : 8`, register count from bits[15:12] (`0x0/0x4/0x8` = ST4/ST3/ST2;
  `0x2/0x6/0x7/0xA` = ST1 of 4/3/1/2), **R (bit 21) set refused as unallocated for a store**, single-doubleword
  (`!q && size==3`) refused;
- **store direction only** — `(insn >> 22) & 1` refuses a load on a store fault.

The 5,709-encoding capstone agreement is unchanged: the block is byte-identical to the file the Python
mirror was diffed against (same sha256 as our fork). **No rule was reworded, reordered or re-derived here.**

### The refusal names its instruction — unchanged

`TL_CW_REFUSALS 8` (8-entry fixed static table, keyed on `(pc, instruction)`), printed **once per DISTINCT
refusal** via `refusal_is_new(pc, insn)` — verified: the table is 8 entries and `refusal_is_new` compares
both `pc` and `insn`; `name_refusal` prints the **pc, the guest image `+0x…` (`tl_ld_lib_of`/`_base`/`_name`),
the raw 4 bytes, the word, the mnemonic/operands (`describe_insn`) and the decoder's own reason**. 25
`REFUSE(...)` sites inside `emulate`, one `#define`/one `#undef`. Nothing allocating on the fault path
(`snprintf` into stack buffers, no malloc).

### RELRO accounting — present, defined by the merged loader

`long tl_codewrite_relro_count(void) { return g_emulated_relro; }`, and in `on_fault`, immediately after a
carried store: `if (tl_ld_va_relro(info->si_addr) == 1) g_emulated_relro++`. `tl_ld_va_relro` is **declared**
`husk-tl-ld.h:136` and **defined** `husk-tl-ld.c:1345` in the current target (the foundations merge landed
it), so the placed file compiles against it. `tl_codewrite_refused_count` / `tl_codewrite_relro_count` are
**declared** in the target's already-placed `husk-tl-codewrite.h` (lines 16, 22) and **defined** here — the
pair agrees.

### Dependencies — all present in the target (verified, not assumed)

| symbol | declared | defined in target |
|---|---|---|
| `tl_ld_va_relro` | `husk-tl-ld.h:136` | `husk-tl-ld.c:1345` |
| `tl_xmem_emit_begin` / `tl_xmem_emit_end` | `husk-tl-xmem.h:115-116` | `husk-tl-xmem.c:1379-1380` |
| `tl_xmem_contains` / `_is_rx` / `_delta` / `_flush` | `husk-tl-xmem.h:41-119` | `husk-tl-xmem.c` |
| `tl_ld_lib_of` / `_lib_base` / `_lib_name`, `tl_lib` | `husk-tl-ld.h:35,105,109,110` | `husk-tl-ld.c:681,683` |
| `tl_log_line` | `husk-tl-bionic.h:111` | present |

Nothing had to be defined elsewhere; nothing is missing.

## 4. Nothing widens a refusal to a form the guard can serve — checked two ways

1. **Mask disjointness.** The new SIMD test `(insn & 0xBF000000) == 0x0C/0x0D000000` cannot shadow any later
   case: the pair case's constant `0x28000000`, the unsigned-immediate `0x39000000` and the
   unscaled/register-offset `0x38000000` all mask (under `0xBF000000`) to values other than `0x0C/0x0D000000`
   (verified in Python). So no word previously carried by the STP or load/store-register blocks can now be
   captured by the SIMD block and refused instead. The SIMD opcode space (bits[31:24] 0x0C/0x0D) is disjoint
   from load/store-register (0x38-0x3B) and pair (0x28-0x2C).
2. **The two service-widenings are the only changes to coverage, and both widen SERVICE, never refusal.**
   `put` now serves an address that is in the guest-facing view (upstream: refused it); `on_fault` now
   attempts the guard for either view (upstream: only RX). In both cases a form the guard cannot serve still
   returns `false`/chains exactly as before, and the `REFUSE` substitutions are the identical `return false`.

The device measurement is the arbiter and could not be re-run here: with this file in, robot gf ran
`codewrite carried=0 refused=0 relro=0` through **two apps in one process** (against 29,538,738 carries
before it) — i.e. under the guard, and with the SIMD fix carried, neither family was ever refused again.

## 5. Structural check (no compiler)

Comment/string-stripped: brace `/`, paren `()` and bracket `[]` balances all **0**; `#if/#ifdef/#ifndef`
depth returns to **0** with **0 stray `#endif`**; `#define REFUSE`/`#undef REFUSE` balanced (1 each); line
endings all-LF, matching the file's own style. The placed file is byte-identical (`cmp`) to our fork's, so
it is the proven-good file, unmodified.

## 6. What could not be verified

- **No compile, no link, no run.** No `cc`/`gcc`/`clang`/`zig`/`tcc` on this box. A compile (`check-translation-layer.yml`)
  plus a device run is where this is judged. **No build is claimed.**
- The 5,709-encoding capstone agreement is taken from the reference / the plan note; it was not re-run here
  (no capstone/toolchain run made). The *bytes* implementing it are unchanged (same sha256 as the file the
  mirror was diffed against).
- The device measurement (`carried=0 refused=0 relro=0` through two apps) is quoted from the reference; it
  was not reproduced here.
- `tl_ld_va_relro` is only *linked* correctly if the merged `husk-tl-ld.c` stays as the foundations worker
  left it (it does, at this time); if a later merge changes `tl_ld_va_relro`'s signature, this file's one
  call site must follow.

Nothing outside `src/translation-layer-next/husk-tl-codewrite.c` was touched. No git command was run inside
`husk3.0`. This report is committed in the SOURCE repo only.
