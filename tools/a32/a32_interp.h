/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * a32_interp.h -- a pure A32 (ARM 32-bit, ARM state) integer reference
 * interpreter.
 *
 * Purpose: this is the *reference* the translator will be developed against.
 * It is deliberately not fast, not clever and not a decoder table beyond what
 * clarity wants: it decodes one instruction at a time and executes it in C.
 * Its only job is to be *right*, and -- when it is not -- to say so loudly and
 * with a name, never to mis-execute silently.
 *
 * Scope (this increment): ARM state (A32) only, user mode, integer core:
 *   - data-processing (all 16 opcodes), register / immediate /
 *     register-shifted-register forms, all shifts (LSL/LSR/ASR/ROR/RRX) with
 *     correct carry-out and S-bit flag effects;
 *   - multiply MUL/MLA/MLS/UMULL/UMLAL/SMULL/SMLAL;
 *   - MRS / MSR (flags field) and MOVW/MOVT/CLZ;
 *   - loads/stores LDR/STR/LDRB/STRB/LDRH/STRH/LDRSH/LDRSB, immediate and
 *     register (shifted) offset, pre/post-indexed, with/without writeback,
 *     plus LDRD/STRD and SWP/SWPB;
 *   - block transfer LDM/STM in all four addressing modes;
 *   - branches B/BL/BX/BLX(register)/BLX(immediate), PC reads as address+8.
 *
 * Deliberately OUT of scope, and reported as an unimplemented-instruction fault
 * (a NAMED failure, never a silent wrong answer) if a case ever reaches one:
 *   - everything Thumb / Thumb-2 (so a BX/BLX to a Thumb address, or a BLX
 *     immediate which always enters Thumb, stops with a named reason);
 *   - VFP / NEON / coprocessor / CP15, exceptions, interrupts, CPU modes other
 *     than user, banked registers, MMU and permissions, syscalls, threads;
 *   - CLZ is supported; the saturating/extend/bitfield/BFI/REV/... families are
 *     not (any such word faults by name).
 *
 * Memory model: one flat little-endian arena (ARENA_SIZE bytes) holding the
 * case's code and its 4 KiB scratch window next to each other. Every access is
 * bounds-checked; an out-of-range access is a clean, named failure carrying the
 * case id and the address, and never touches the harness.
 *
 * The scratch window's initial contents are the harness's agreed seed
 * (word k = 0x10000000 + k), and its FNV-1a hash is what the golden compares --
 * so the interpreter does not invent an initial memory: it reproduces the one
 * tools/a32/a32oracle.c seeds.
 *
 * Initial architectural state, which the interpreter reproduces exactly so a
 * run is comparable to a golden a32_case_N produced:
 *   - r0..r8, r10, r11 = the case's init (others 0); r9 = the scratch base;
 *     r12,r13,r14 = 0; r15 = the fragment's entry (code[0]);
 *   - CPSR NZCV = 0110 (N=0 Z=1 C=1 V=0), Q=0, user mode (0x10) -- the state
 *     a32_case_N's wrapper forces with `mov r0,#0 ; cmp r0,#0` before the body;
 *   - FPSCR = 0 and s0..s31 = 1.0f, the harness's agreed (untouched-by-integer)
 *     VFP state.
 */
#ifndef A32_INTERP_H
#define A32_INTERP_H

#include <stddef.h>
#include <stdint.h>

#include "a32_state.h"

/* The scratch window's base address as the interpreter maps it. A case body
 * uses [r9, #off] and [r9, reg]; the harness sets r9 to this, exactly as
 * a32_case_N's wrapper sets r9 to the oracle's mmap'd window. The absolute value
 * is arbitrary -- only offsets from it are architectural -- and regs[9] is not
 * compared (it is an address, not a value). */
#define A32_INTERP_SCRATCH_BASE 0x00018000u

/* One case as the interpreter consumes it: the initial register file (r0..r15;
 * r15 is ignored, the entry is always code[0]) and the raw A32 word fragment.
 * The word fragment is byte-identical to what a32_case_N executes, because both
 * come from the same gen_cases.py emission (a .word in the .S and the same word
 * in the generated C table). */
struct a32_interp_case {
    uint32_t        id;         /* the case id, echoed into a fault message */
    uint32_t        regs[A32_NREG];
    const uint32_t *code;
    uint32_t        code_len;   /* in words */
};

/* Run one case.
 *
 * Returns 0 iff the case ran to the end of its word fragment (the natural
 * fall-through -- the same thing a32_case_N's body reaching the wrapper does).
 * On return, *out holds the full architectural result (regs, cpsr, fpscr, fp,
 * mem_size, mem_hash, mem_first_diff, stop_reason).
 *
 * Returns nonzero iff the case could NOT be run to completion. *out is still
 * fully populated (so a caller can diff it), with out->stop_reason naming why
 * (A32_STOP_FAULT or A32_STOP_LIMIT), and err (if non-NULL) holding a reason
 * string that names the instruction word, its address and (if set) the case id.
 * A case that did not complete is a FAILURE for the harness -- it must never be
 * counted as a pass. */
int a32_interp_run(const struct a32_interp_case *c, struct a32_result *out,
                   char *err, size_t errn);

/* The arena the interpreter maps its code + scratch into (exposed for hosts). */
#define A32_INTERP_ARENA_BASE 0x00010000u
#define A32_INTERP_ARENA_SIZE 0x00020000u

#endif /* A32_INTERP_H */
