# The store guard's refusal, named: `st1 {v1.s}[1], [x9]` (Advanced SIMD structure store)

Log: `C:/Users/Aiden/Desktop/HuskLogs/husk-20261015-123117-2.0.0-112.log` (build `2.0.0 (112) e982c32-dirty`,
robot gf `com.IncontinentCell.MyDystopianRobotGirlfriend`, `(java)`/Unity-6 driver, IL2CPP).

```
=== FATAL signal 10 on thread 'UnityMain': fault address 0x124a51b90 (0x124a51b90)
    pc .../Documents/Data/App+0x4c5e8e4
    lr .../Documents/Data/App+0x4c5e804
```

The page is not the fault: the log's own open-time and fatal-time probes agree on both views
(READABLE-AND-EXECUTABLE, all three verdicts, `VS the open-time verdicts ... MATCH`), so the transient
hypothesis is excluded. It is a DATA access whose pc lies outside the arena, i.e. the guest's own code
storing into the EXECUTABLE view of a page it owns — exactly what `src/translation-layer-next/husk-tl-codewrite.c`
exists to carry. It returned false and chained, the crash reporter classified the fault (SIGBUS is
deliberately left to the JIT guard), the app was unwound. The refusal was correct by design and anonymous.

## 1. Which image, and is the reported offset a vaddr or a file offset

`App` is the guest's IL2CPP: the loader's own summary for it reads `583789 relocs`, which is
`libil2cpp.so`'s relocation count from the same log's library report (`Games/Factorial_Omega_...apk`,
`lib/arm64-v8a/libil2cpp.so`, 91,567,688 bytes = the "89.5 MiB" image; the other two `App` lines are
`libunity.so` and the burst library). The loader emits that image into the JIT arena, so the runtime pc is
an ARENA address and the crash report's `+0x...` is:

- `husk-tl-ld.c:667` hands the crash reporter `bias = L->rx - L->base_vaddr` for each library;
- `husk-tl-unity-app.c:99` (`find_lib`) then sets `off = addr - bias`.

So **`off` is an image-relative VADDR**, not a file offset. Confirmed against the ELF: `libil2cpp.so`'s
minimum `p_vaddr` is `0`, and its executable `PT_LOAD` has `p_offset 0x206a620` but `p_vaddr 0x206e620`
— a `0x4000` difference (the skill's "vaddr used as an offset disassembles 0x10..0x1000 off" trap, here
0x4000). The file offset is therefore `0x4c5e8e4 - 0x4000 = 0x4c5a8e4`, inside the `il2cpp` section
(`sh_addr 0x2434aa4`, `sh_offset 0x2430aa4`, i.e. the same `-0x4000`).

The arithmetic settles it, and the register dump settles it independently: read as a file offset the word
is `ldr w10, [x19, #0x314]` (a load, on a load one operand too small); read as a vaddr it is a store to
`[x9]`, and the dump has `x9 = 0x124a51b90` = `si_addr` exactly, with `add x9, x8, #0x128` two
instructions earlier and `x8 = 0x124a51a68` (`0x124a51a68 + 0x128 = 0x124a51b90`). The instruction
sequence that executed is the one disassembled from the vaddr.

## 2. The instruction

Disassembled with capstone (arm64) from the APK's `lib/arm64-v8a/libil2cpp.so` at file offset `0x4c5a8e4`
(vaddr `0x4c5e8e4`) — no device, no probe build:

```
0x4c5e8c0  e822389b   smaddl x8, w23, w24, x8
0x4c5e8c4  c13240fd   ldr d1, [x22, #0x60]
0x4c5e8c8  09b10491   add x9, x8, #0x12c
0x4c5e8cc  210100fd   str d1, [x9]
0x4c5e8d0  09a10491   add x9, x8, #0x128          <-- x9 = x8 + 0x128 = the fault address
0x4c5e8d4  60f644bd   ldr s0, [x19, #0x4f4]
0x4c5e8d8  0204040e   dup v2.2s, v0.s[0]
0x4c5e8dc  21d4a20e   fsub v1.2s, v1.2s, v2.2s
0x4c5e8e0  012101bd   str s1, [x8, #0x120]        <-- a SIMD STR the guard DOES carry
0x4c5e8e4  2191000d   st1 {v1.s}[1], [x9]         <-- the faulting pc
0x4c5e8e8  c12e00fd   str d1, [x22, #0x58]
0x4c5e8ec  3b000014   b #0x4c5e9d8
```

**The instruction: `st1 {v1.s}[1], [x9]`** — raw bytes `21 91 00 0d`, word `0x0d009121`.

- mnemonic: `st1` (ST1, Advanced SIMD **single structure**, one lane — store direction)
- value: **`v1.s[1]`** — the second 32-bit lane of `v1`, the register the `fsub` above just produced
- address: **`x9`** = `0x124a51b90`, i.e. `si_addr` (no offset: this form has none)
- access size: **4 bytes** (`.s` = 32-bit element)
- index form: **no pre/post-index** (post-index bit 23 = 0, Rm = 0). A post-index variant would be
  `st1 {v1.s}[1], [x9], #4` or `..., x9-visible register`, and the guard handles those too now.
- what it is doing: a vectorising compiler writing one lane of a float2 into a struct field; the
  immediately preceding `str s1, [x8, #0x120]` writes lane 0 and, being a plain SIMD STR, **was** carried by
  the guard — the very next store is the structure store it refused.

## 3. The decoder gap

`emulate()` matched the instruction against, in order: DC ZVA, STXR/STLXR (`0x3FE07C00==0x08007C00`),
STLR (`0x3FFFFC00==0x089FFC00`), CAS (`0x3FA07C00==0x08A07C00`), STP/STNP (`0x3A000000==0x28000000`),
and the "load/store register" family (`0x3B000000==0x39000000`, `0x3B200000==0x38000000`,
`0x3B200C00==0x38200800`). `0x0d009121 & 0x3B000000 = 0x09000000`, `& 0x3A000000 = 0x08000000`,
`& 0x3FE07C00 = 0x0d001000` — none of them match, and it fell through to `return false`.

**What is missing is the whole Advanced SIMD STRUCTURE store class, both of its encodings:**

| form | encoding | matches the guard before the fix? |
|---|---|---|
| `st1/st2/st3/st4 {Vt.<T>}[idx], [Xn]` (single structure, ONE lane) | bits[29:24]=`001101` → `(insn & 0xBF000000)==0x0D000000` | no |
| `st1/st2/st3/st4 {V1..Vn.<T>}, [Xn]{, Xm or #imm}` (multiple structures, WHOLE vectors) | bits[29:24]=`001100` → `(insn & 0xBF000000)==0x0C000000` | no |

So it is not the twin lookup, not a size mismatch, and not a pc that could not be mapped: the pc is
4-aligned, in the emitted image's executable view, and `put()` would have written through the writable twin
at a fixed delta (the log's own `store into the executable view ... carried through the writable twin`
lines prove that path works in this very run). Only the decoder.

## 4. The fix (implemented)

`husk-tl-codewrite.c`, `emulate()`: a case for both structure families, ahead of the STP block. Not a
special case for this game — the guard is engine-agnostic, so every engine and app that patches or spills
vectors into code memory gets it. Shape, shared by both families: `Rn` is the address, the register(s) are
the value(s), no pre-index form, and post-index bit 23 names `Rm` (bits[20:16]) or, with `Rm == 31`, an
immediate of exactly the bytes transferred (`nreg * per`).

- **single structure (one lane)**: opcode bits[15:13] picks the element (0/1 → 8-bit, 2/3 → 16-bit,
  4/5 → 32/64-bit), bit 21 (`R`) picks ST1/ST3 vs ST2/ST4, and the lane index is spread over Q (bit 30),
  S (bit 12) and size (bits[11:10]) — Q must NOT be read as a vector width here. `per` = element bytes.
- **multiple structures (whole vectors)**: opcode bits[15:12] gives the register count
  (0→ST4=4, 2→ST1×4, 4→ST3=3, 6→ST1×3, 7→ST1×1, 8→ST2=2, A→ST1×2), `per` = Q ? 16 : 8 bytes.
- validity is enforced exactly as the architecture does, not guessed: op ≤ 5 for the lane family;
  size<0> clear for 16-bit, size ≤ 1 for 32/64-bit, S clear for 64-bit (one lane); `R` (bit 21) clear for
  the multiple family; `.1d` (Q=0, size=3) only for the ST1-multiple opcodes; Rm must be 0 when there is
  no post-index. Loads are refused (a load fault is not this guard's to serve) and so is every unallocated
  word.

**Verification without a device.** The decode was mirrored in Python and compared against capstone over the
whole of both families' spaces (Q, L, post/Rm, opcode/`R`, size, lane bits, bit 31 — 32×16×4×2×2×2×2
combinations): **5709 structure-store encodings, 5709 exact agreements** on mnemonic, arrangement, lane
index, register count, post-index form and address, and **every** unallocated word in the same space still
refused. `st1 {v1.s}[1], [x9]` is one of the 5709. (The same harness proves the pre-fix decoder refuses it:
`0x0d009121` matches none of the old masks.)

**Not implemented, and next to bite:** the decoder still has no case for `STXP/STLXP` (store-exclusive
*pair*), the LSE atomic `SWP/SWPA/SWPAL/SWPB/SWPH` family (an atomic exchange emits `swp`, not
`stxr`+`stlr`), `STLLR`, and MTE's `STG/STZG`. Of those, `STXP` (C++ `std::atomic<pair>`/128-bit
exclusive) and `SWP` (`-moutline-atomics`, `std::atomic::exchange`) are the likely next refusals in an
IL2CPP title; each is one more mask + one more store, in the same place.

## 5. The refusal now names itself

`on_fault` keeps the chain call byte-for-byte — same call, same arguments, same place. Added before it:
one line per DISTINCT refusal (fixed 8-entry static table keyed on `(pc, instruction)`, because this runs on
the fault path and may not allocate), naming the pc, the guest image and image-relative offset (via
`tl_ld_lib_of`/`tl_ld_lib_base`, the same arithmetic the crash report prints), the raw bytes and word, the
mnemonic and operands where the decoder knows the form, and the decoder's own stated reason.

For **this** instruction the line would have been (pc is a runtime arena address; the image-relative part
is exact):

```
codewrite: REFUSED a store at pc 0x1???????? (App+0x4c5e8e4), fault address 0x124a51b90: bytes 21 91 00 0d (word 0d009121) = st1 {v1.s}[1], [x9] -- no case in this guard's decoder matches the instruction word; nothing was carried and the fault is chained to the previous handler unchanged (distinct refusal 1 of 8, 1 refused in all)
```

After the fix that instruction is carried, so the next refusal names the *next* gap (`stxp`/`swp`/…)
without a probe build.

Invariants kept: no `vm_protect` on the region, the executable view is never written (the store goes
through the writable twin at the fixed delta), nothing is unmapped, no thread is ended, and the refusal
path still chains to the previous handler exactly as before — the log line is the only thing added to it.

## Files

- `src/translation-layer-next/husk-tl-codewrite.c` — the structure-store cases, `describe_insn`, the
  once-per-distinct-refusal naming, and `on_fault`'s reason plumbing.
- This document.

No other tree has this file: `src/translation-layer/` (the older runtime) has no codewrite guard, so
nothing there changes.
