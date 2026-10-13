/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * a32_interp.h -- a pure AArch32 (ARM + Thumb) integer reference interpreter.
 *
 * Purpose: this is the *reference* the translator will be developed against.
 * It is deliberately not fast, not clever and not a decoder table beyond what
 * clarity wants: it decodes one instruction at a time and executes it in C.
 * Its only job is to be *right*, and -- when it is not -- to say so loudly and
 * with a name, never to mis-execute silently.
 *
 * Scope:
 *   - ARM state (A32), user mode, integer core:
 *       data-processing (all 16 opcodes) register / immediate /
 *       register-shifted-register, all shifts with correct flags;
 *       multiply MUL/MLA/MLS/UMULL/UMLAL/SMULL/SMLAL;
 *       MRS / MSR (flags field) and MOVW/MOVT/CLZ;
 *       loads/stores LDR/STR/LDRB/STRB/LDRH/STRH/LDRSH/LDRSB, immediate and
 *       register (shifted) offset, pre/post-indexed, with/without writeback,
 *       plus LDRD/STRD and SWP/SWPB;
 *       block transfer LDM/STM in all four addressing modes;
 *       branches B/BL/BX/BLX(register)/BLX(immediate), PC reads as address+8.
 *   - Thumb state (T32), user mode, integer core (this increment):
 *       16-bit Thumb: LSL/LSR/ASR imm/reg, ADD/SUB reg/imm3/imm8,
 *       MOV/CMP/ADD/SUB imm8, the whole ALU group (AND/EOR/LSL/LSR/ASR/ADC/
 *       SBC/ROR/TST/NEG/CMP/CMN/ORR/MUL/BIC/MVN) with correct flag rules,
 *       hi-register ADD/CMP/MOV and BX/BLX-register, LDR literal, all
 *       load/store register and immediate forms (byte/half/word, signed),
 *       SP-relative and PC-relative load-address, ADD/SUB SP, PUSH/POP,
 *       LDMIA/STMIA, conditional and unconditional B, CBZ/CBNZ, BL, SVC/UDEF
 *       (named faults) and IT (including nested/skipped semantics);
 *       32-bit Thumb-2 (the encodings the corpus shows): MOVW/MOVT, wide
 *       ADD/SUB/logical register and modified-immediate, ADDW/SUBW, wide
 *       LDR/STR imm12 (+/-) and register offset, wide byte/half and
 *       signed-byte/half loads, LDRD/STRD, TBB/TBH, BL/BLX immediate,
 *       MRS/MSR, SXTB/SXTH/UXTB/UXTH, REV/REV16/REVSH, CLZ, SSAT/USAT,
 *       BFI/BFC/UBFX/SBFX;
 *       16-bit Thumb SXTB/SXTH/UXTB/UXTH and REV/REV16/REVSH (the 1011
 *       extend/reverse group);
 *       wide Thumb-2 multiply (1111 1011 xxxx): MUL/MLA/MLS and the long
 *       multiplies SMULL/UMULL/SMLAL/UMLAL;
 *       the wide Thumb-2 load/store single forms in full: imm12, the imm8
 *       form with every P/U/W addressing combination, and the register
 *       offset (the "1101 form");
 *       DMB/DSB/ISB (a barrier has no effect on a single observer).
 *   - VFP LOAD/STORE (register file <-> memory, no arithmetic): VLDR/VSTR
 *     (single and double), VLDM/VSTM in all four addressing modes and the
 *     VPUSH/VPOP aliases, in both ARM and Thumb-2 encodings. The s0..s31
 *     register file is tracked and compared (fp[]); VFP *arithmetic* is still
 *     a named unimplemented fault.
 *   - INTERWORKING: the CPSR T bit is tracked honestly; BX/BLX to an odd
 *     address enters Thumb and an even address returns to ARM; BLX immediate
 *     enters Thumb; a PC whose alignment disagrees with the current state, or
 *     an encoding that is not implemented, is a NAMED fault -- never a wrong
 *     result. The T bit is part of the compared state (A32_CPSR_MASK), so a
 *     state-transition bug cannot pass.
 *
 * Deliberately OUT of scope, and reported as a named unimplemented-instruction
 * fault if a case reaches one: VFP / NEON arithmetic and the coprocessor data
 * path (VMOV/VMRS/VCMP/CDP), CP15, exceptions, interrupts, CPU modes other
 * than user, banked registers, MMU and permissions, syscalls (SVC is a named
 * fault in user mode), threads; saturating QADD family, SEL, PKH, RBIT, CPS,
 * SETEND; the Thumb-2 encodings not listed above (the media multiplies, wide
 * LDM/STM/PUSH/POP kept apart from VFP, the parallel add/sub, B.W's
 * modified-immediate space).
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
 *   - CPSR NZCV = 0110 (N=0 Z=1 C=1 V=0), Q=0, user mode (0x10), T set iff the
 *     case's entry state is Thumb (in->thumb);
 *   - FPSCR = 0 and s0..s31 = 0x3f800000 + i (1.0f + i ulp) -- the exact
 *     convention gen_cases.py's a32_fp_init establishes, not an assumption.
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
 * r15 is ignored, the entry is always code[0]), the raw code bytes and the
 * entry instruction-set state. The bytes are byte-identical to what a32_case_N
 * executes, because both come from the same gen_cases.py emission (a .word/
 * .hword in the .S and the same bytes in the generated C table).
 *
 * `thumb` is 1 iff the case enters in Thumb state (CPSR.T == 1), 0 for ARM. */
struct a32_interp_case {
    uint32_t        id;         /* the case id, echoed into a fault message */
    uint32_t        regs[A32_NREG];
    const uint8_t  *code;
    uint32_t        code_len;   /* in BYTES (Thumb instructions are 2 or 4) */
    int             thumb;      /* entry state: 1 = Thumb, 0 = ARM */
};

/* Run one case.
 *
 * Returns 0 iff the case ran to the end of its code fragment (the natural
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

/* CPSR.T -- the Thumb state bit. Exposed so a host (and the comparison) can see
 * the state transition explicitly. */
#define A32_CPSR_T 0x00000020u

#endif /* A32_INTERP_H */
