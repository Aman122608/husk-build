#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
gen_cases.py -- the AArch32 case generator for the differential harness.

    python3 tools/a32/gen_cases.py            # writes a32_cases_gen.S / .h next to itself

WHAT A "CASE" IS (the case format)
----------------------------------
A case is a hand-written ARM-mode fragment plus an initial register state. The
generator emits, into a32_cases_gen.S, one function per case with this ABI:

    void a32_case_<id>(struct a32_result *out  /* r0 */, void *scratch /* r1 */);

The wrapper the generator writes around every body:

    push  {r4-r11, lr}
    mov   r12, r0                 ; r12 = &out->regs[0]  (out + A32_OFF_REGS)
    add   r12, r12, #260
    mov   r9, r1                  ; r9 = scratch window base
    mov   r0, #0
    cmp   r0, #0                  ; known incoming flags: N=0 Z=1 C=1 V=0
    mov   r0..r8,r10,r11, #0      ; zero the comparable GPRs (mov: no flags)
    <init>                        ; movw/movt only (ldr too), never touch flags
    <body>                        ; must not touch r9 or r12
    stm   r12, {r0-r11}           ; capture r0..r11
    mrs   r0, cpsr
    str   r0, [r12, #64]          ; out->cpsr   (A32_OFF_REGS + 64 == A32_OFF_CPSR)
    mov   r0, #0
    str   r0, [r12, #48]          ; out->regs[12] = 0  (was the base pointer)
    str   r0, [r12, #52]          ; out->regs[13] = 0
    str   r0, [r12, #56]          ; out->regs[14] = 0
    str   r0, [r12, #60]          ; out->regs[15] = 0
    pop   {r4-r11, pc}

Reserved: r9 = scratch base (an ADDRESS: recorded, never compared), r12 = out+260.
So a case body may freely use r0-r8, r10, r11; the comparable GPRs are exactly
r0-r8, r10, r11 (see a32_state.h). r13/r14/r15 are written 0 and are not compared
yet: a branch case proves the branch happened by setting a register on the path it
took (e.g. a value only reached if BL landed), so no PC/offset handling is needed.

The VFP helpers a32_fp_init / a32_fp_capture / a32_fp_fpscr are emitted once and
let the oracle put a deterministic pattern in s0..s31 and read it back. The oracle
is compiled -mfloat-abi=soft so C never disturbs the VFP registers between the case
and the capture.

HOW TO EXTEND (the extension points, named and not wired in this increment)
---------------------------------------------------------------------------
* Hundreds of cases: append to CASES. Randomised/edge operands are a Python loop
  away; nothing downstream distinguishes one generated case from another.
* Lifted real functions: tools/a32/extract.py (milestone 2) will cut functions out
  of lib/armeabi-v7a/*.so (libLive2DCubismCore.so from
  Games/TheLewdestHouse-0-2-42.apk first), synthesise a body that loads its arg
  registers and calls into the slice, and write the SAME `source` tag into the
  case metadata. The oracle and the comparer need no change.
* Thumb / stepping / loops: `mode` becomes "thumb", and a generated loop sets
  stop_reason = A32_STOP_LIMIT instead of running away.

Every 32-bit value in the golden is a hex STRING (see a32_state.h) so a reader can
never round it.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
S_OUT = os.path.join(HERE, "a32_cases_gen.S")
H_OUT = os.path.join(HERE, "a32_cases_gen.h")

# ---------------------------------------------------------------------------
# CASES. Each: id, name, group, desc, init {reg: value}, body [asm lines].
# A body must not use r9 or r12 (reserved by the wrapper) and must leave its
# result in a comparable register (r0-r8, r10 or r11).
# ---------------------------------------------------------------------------
CASES = [
    # ---- data-processing, register form -----------------------------------
    dict(id=1, name="add_reg", group="dp-reg", desc="ADD r0,r1,r2",
         init={1: 0x00000007, 2: 0x00000005}, body=["add r0, r1, r2"]),
    dict(id=2, name="sub_reg", group="dp-reg", desc="SUB r0,r1,r2",
         init={1: 0x00000007, 2: 0x00000005}, body=["sub r0, r1, r2"]),
    dict(id=3, name="and_reg", group="dp-reg", desc="AND r0,r1,r2",
         init={1: 0xF0F0F0F0, 2: 0x0FF00FF0}, body=["and r0, r1, r2"]),
    dict(id=4, name="orr_reg", group="dp-reg", desc="ORR r0,r1,r2",
         init={1: 0xF0F0F0F0, 2: 0x0F0F0F0F}, body=["orr r0, r1, r2"]),
    dict(id=5, name="bic_reg", group="dp-reg", desc="BIC r0,r1,r2",
         init={1: 0xFF00FF00, 2: 0x0FF00FF0}, body=["bic r0, r1, r2"]),
    # ---- data-processing, immediate form ----------------------------------
    dict(id=6, name="add_imm", group="dp-imm", desc="ADD r0,r1,#0x230",
         init={1: 0x00001000}, body=["add r0, r1, #0x230"]),
    dict(id=7, name="and_imm", group="dp-imm", desc="AND r0,r1,#0x0000FF00",
         init={1: 0xDEADBEEF}, body=["and r0, r1, #0x0000FF00"]),
    dict(id=8, name="mov_imm", group="dp-imm", desc="MOV r0,#0xFF",
         init={}, body=["mov r0, #0xFF"]),
    dict(id=9, name="movw_movt", group="dp-imm", desc="MOVW/MOVT r0,#0xDEADBEEF",
         init={}, body=["movw r0, #0xBEEF", "movt r0, #0xDEAD"]),
    # ---- shifts -----------------------------------------------------------
    dict(id=10, name="lsl_imm", group="shift", desc="MOV r0,r1,LSL #4",
         init={1: 0x80000001}, body=["mov r0, r1, LSL #4"]),
    dict(id=11, name="lsr_imm", group="shift", desc="MOV r0,r1,LSR #28",
         init={1: 0x80000001}, body=["mov r0, r1, LSR #28"]),
    dict(id=12, name="asr_imm", group="shift", desc="MOV r0,r1,ASR #28",
         init={1: 0x80000001}, body=["mov r0, r1, ASR #28"]),
    dict(id=13, name="ror_imm", group="shift", desc="MOV r0,r1,ROR #4",
         init={1: 0x00000081}, body=["mov r0, r1, ROR #4"]),
    dict(id=14, name="lsl_reg", group="shift", desc="MOV r0,r1,LSL r2",
         init={1: 0x00000001, 2: 0x0000000C}, body=["mov r0, r1, LSL r2"]),
    dict(id=15, name="add_shifted", group="shift", desc="ADD r0,r1,r2,LSL #3",
         init={1: 0x00000001, 2: 0x00000002}, body=["add r0, r1, r2, LSL #3"]),
    # ---- multiply ---------------------------------------------------------
    dict(id=16, name="mul", group="mul", desc="MUL r0,r1,r2",
         init={1: 0x0000FFFF, 2: 0x00010001}, body=["mul r0, r1, r2"]),
    dict(id=17, name="umull", group="mul", desc="UMULL r0,r3,r1,r2",
         init={1: 0xFFFFFFFF, 2: 0x00000002}, body=["umull r0, r3, r1, r2"]),
    # ---- compare / test (flags are the result) ----------------------------
    dict(id=18, name="cmp_eq", group="cmp", desc="CMP r1,#5 -> Z=1 C=1",
         init={1: 0x00000005}, body=["cmp r1, #5"]),
    dict(id=19, name="tst", group="cmp", desc="TST r1,#1 -> Z=0",
         init={1: 0x00000005}, body=["tst r1, #1"]),
    dict(id=20, name="cmn", group="cmp", desc="CMN r1,#1 -> Z=0 C=0",
         init={1: 0x00000005}, body=["cmn r1, #1"]),
    dict(id=21, name="teq", group="cmp", desc="TEQ r1,r1 -> Z=1",
         init={1: 0x00000000}, body=["teq r1, r1"]),
    dict(id=22, name="adds_v", group="cmp", desc="ADDS r0,r1,#1 -> V=1 N=1",
         init={1: 0x7FFFFFFF}, body=["adds r0, r1, #1"]),
    # ---- conditional execution -------------------------------------------
    dict(id=23, name="cond_eq_taken", group="cond", desc="MOVEQ taken (Z=1)",
         init={1: 0x00000000},
         body=["cmp r1, #0", "moveq r0, #0x11", "movne r0, #0x22"]),
    dict(id=24, name="cond_ne_taken", group="cond", desc="MOVNE taken (Z=0)",
         init={1: 0x00000001},
         body=["cmp r1, #0", "moveq r0, #0x11", "movne r0, #0x22"]),
    # ---- loads and stores (r9 = scratch base) -----------------------------
    dict(id=25, name="str_ldr_imm", group="mem", desc="STR then LDR, immediate offset",
         init={1: 0x11223344}, body=["str r1, [r9, #16]", "ldr r0, [r9, #16]"]),
    dict(id=26, name="strb_ldrb", group="mem", desc="STRB then LDRB",
         init={1: 0x000000AB}, body=["strb r1, [r9, #3]", "ldrb r0, [r9, #3]"]),
    dict(id=27, name="strh_ldrh", group="mem", desc="STRH then LDRH",
         init={1: 0x0000BEEF}, body=["strh r1, [r9, #6]", "ldrh r0, [r9, #6]"]),
    dict(id=28, name="ldr_reg_off", group="mem", desc="LDR [r9, r2] register offset",
         init={2: 0x00000008}, body=["ldr r0, [r9, r2]"]),
    # ---- branches ---------------------------------------------------------
    dict(id=29, name="b_target", group="branch",
         desc="B skips a write; the fall-through value must survive",
         init={}, body=["mov r0, #0", "b 1f", "mov r0, #0x7F", "1:", "mov r1, #0x2A"]),
    dict(id=30, name="bl_bx_lr", group="branch",
         desc="BL into a subroutine, BX lr back, execution continues",
         init={},
         body=["mov r0, #0", "bl 1f", "mov r3, #0x99", "b 2f",
               "1:", "mov r0, #0x11", "bx lr", "2:"]),
    dict(id=31, name="bx_lr_label", group="branch",
         desc="BX lr after adr lr,1f jumps to the label (a fall-through gives 0xAB)",
         init={},
         body=["adr lr, 1f", "bx lr", "mov r5, #0xAB", "b 2f",
               "1:", "mov r5, #0x5A", "2:"]),
    dict(id=32, name="adr_delta", group="branch",
         desc="ADR of two labels; their difference is comparable",
         init={},
         body=["adr r4, 1f", "adr r5, 2f", "1:", "nop", "nop", "2:",
               "sub r0, r5, r4", "mov r4, #0", "mov r5, #0"]),
]

HEADER = """\
/* GENERATED by tools/a32/gen_cases.py -- do not edit by hand.
 * Regenerate with: python3 tools/a32/gen_cases.py */
"""


def movw_movt(reg, value):
    """A 32-bit load that does not touch flags and needs no literal pool."""
    lines = ["\tmovw\tr%d, #0x%04x" % (reg, value & 0xFFFF)]
    if (value >> 16) & 0xFFFF:
        lines.append("\tmovt\tr%d, #0x%04x" % (reg, (value >> 16) & 0xFFFF))
    return lines


def emit_s():
    o = [HEADER, "\t.syntax unified", "\t.arm", "\t.text", ""]

    # --- VFP helpers -------------------------------------------------------
    o += ["\t.globl\ta32_fp_init", "\t.type\ta32_fp_init, %function",
          "a32_fp_init:"]
    for n in range(32):
        o += ["\tmovw\tr1, #0x%04x" % (n & 0xFFFF), "\tmovt\tr1, #0x3f80",
              "\tvmov\ts%d, r1" % n]
    o += ["\tbx\tlr", "\t.size\ta32_fp_init, .-a32_fp_init", ""]

    o += ["\t.globl\ta32_fp_capture", "\t.type\ta32_fp_capture, %function",
          "a32_fp_capture:"]
    for n in range(32):
        o += ["\tvmov\tr1, s%d" % n, "\tstr\tr1, [r0, #%d]" % (n * 8),
              "\tmov\tr2, #0", "\tstr\tr2, [r0, #%d]" % (n * 8 + 4)]
    o += ["\tbx\tlr", "\t.size\ta32_fp_capture, .-a32_fp_capture", ""]

    o += ["\t.globl\ta32_fp_fpscr", "\t.type\ta32_fp_fpscr, %function",
          "a32_fp_fpscr:", "\tvmrs\tr0, fpscr", "\tbx\tlr",
          "\t.size\ta32_fp_fpscr, .-a32_fp_fpscr", ""]

    # --- one function per case --------------------------------------------
    for c in CASES:
        fn = "a32_case_%d" % c["id"]
        o += ["\t.p2align 2", "\t.globl\t%s" % fn,
              "\t.type\t%s, %%function" % fn, "%s:" % fn,
              "\tpush\t{r4-r11, lr}",
              "\tmov\tr12, r0",
              "\tadd\tr12, r12, #260",          # A32_OFF_REGS
              "\tmov\tr9, r1",
              "\tmov\tr0, #0",
              "\tcmp\tr0, #0"]                   # deterministic incoming flags
        # Zero every comparable register so a register a case does not touch
        # reads 0 and not whatever the C caller happened to leave there. mov
        # does not touch flags, so the flag state set above survives.
        for reg in (0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11):
            o.append("\tmov\tr%d, #0" % reg)
        for reg in sorted(c["init"]):
            o += movw_movt(reg, c["init"][reg])
        for line in c["body"]:
            o.append("\t" + line)
        o += ["\tstm\tr12, {r0-r11}",
              "\tmrs\tr0, cpsr",
              "\tstr\tr0, [r12, #64]",          # A32_OFF_CPSR - A32_OFF_REGS
              "\tmov\tr0, #0",
              "\tstr\tr0, [r12, #48]",          # regs[12]
              "\tstr\tr0, [r12, #52]",          # regs[13]
              "\tstr\tr0, [r12, #56]",          # regs[14]
              "\tstr\tr0, [r12, #60]",          # regs[15]
              "\tpop\t{r4-r11, pc}",
              "\t.size\t%s, .-%s" % (fn, fn), ""]
    return "\n".join(o) + "\n"


def emit_h():
    o = [HEADER, "#ifndef A32_CASES_GEN_H", "#define A32_CASES_GEN_H", "",
         '#include "a32_state.h"', "",
         "struct a32_case_meta {",
         "    int          id;",
         "    const char  *name;",
         "    const char  *group;",
         "    const char  *mode;",
         "    const char  *source;",
         "    const char  *desc;",
         "    void       (*fn)(struct a32_result *, void *);",
         "};", "", "/* The generated case entry points. */"]
    for c in CASES:
        o.append("extern void a32_case_%d(struct a32_result *, void *);" % c["id"])
    o += ["", "static const struct a32_case_meta a32_cases[] = {"]
    for c in CASES:
        o.append('    { %d, "%s", "%s", "arm", "handwritten", "%s", a32_case_%d },'
                 % (c["id"], c["name"], c["group"], c["desc"], c["id"]))
    o += ["};", "",
          "#define A32_CASE_COUNT (sizeof(a32_cases) / sizeof(a32_cases[0]))",
          "", "#endif /* A32_CASES_GEN_H */"]
    return "\n".join(o) + "\n"


def main():
    with open(S_OUT, "w") as f:
        f.write(emit_s())
    with open(H_OUT, "w") as f:
        f.write(emit_h())
    print("gen_cases.py: wrote %s (%d cases) and %s"
          % (os.path.basename(S_OUT), len(CASES), os.path.basename(H_OUT)))


if __name__ == "__main__":
    main()
