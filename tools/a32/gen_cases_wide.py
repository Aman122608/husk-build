#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
gen_cases_wide.py -- the cases that cover the encodings the measured work queue
named, kept in a SEPARATE file so it never collides with tools/a32/gen_cases.py
(which another change is editing for the oracle crash).

It imports gen_cases (building the existing 970 cases in memory), then appends
cases for the classes this change implements:

  * wide Thumb-2 multiply (11111 011x): MUL/MLA/MLS, SMULL/UMULL/SMLAL/UMLAL
  * the wide Thumb-2 load/store single forms real code executes: imm12, the
    imm8 form in every P/U/W combination, and the register offset (1101 form)
  * the 16-bit Thumb extend/reverse group (1011 001x / 1011 101x)
  * the VFP memory forms: VLDR/VSTR (single and double), VLDM/VSTM, VPUSH/VPOP

Every case is chosen so a WRONG decode changes a compared field -- that is the
lesson of the SUB SP bug: a suite is only as good as the encodings it actually
tests. The group names are `wide-mul`, `wide-ldst`, `wide-ext`, `wide-vfp`.

    python3 tools/a32/gen_cases_wide.py           # append + write the .S/.h subset
    python3 tools/a32/verify_wide.py              # run them against Unicorn + the interpreter
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import gen_cases as G  # noqa: E402

WIDE_S = os.path.join(HERE, "a32_cases_wide_gen.S")
WIDE_H = os.path.join(HERE, "a32_cases_wide_gen.h")

# ---------------------------------------------------------------------------
# Encoders for the new classes. Each returns the raw halfword(s)/word(s); they
# are checked against capstone by verify_wide.py's decode pass.
# ---------------------------------------------------------------------------
def mul_w(rd, rn, rm):        # MUL Rd, Rn, Rm  (Ra == 15)
    return (0xFB00 | rn, (0xF << 12) | (rd << 8) | (0 << 4) | rm)
def mla_w(rd, rn, rm, ra):    # MLA Rd, Rn, Rm, Ra
    return (0xFB00 | rn, (ra << 12) | (rd << 8) | (0 << 4) | rm)
def mls_w(rd, rn, rm, ra):    # MLS Rd, Rn, Rm, Ra
    return (0xFB00 | rn, (ra << 12) | (rd << 8) | (1 << 4) | rm)
def smull_w(rdlo, rdhi, rn, rm):
    return (0xFB80 | rn, (rdlo << 12) | (rdhi << 8) | (0 << 4) | rm)
def umull_w(rdlo, rdhi, rn, rm):
    return (0xFBA0 | rn, (rdlo << 12) | (rdhi << 8) | (0 << 4) | rm)
def smlal_w(rdlo, rdhi, rn, rm):
    return (0xFBC0 | rn, (rdlo << 12) | (rdhi << 8) | (0 << 4) | rm)
def umlal_w(rdlo, rdhi, rn, rm):
    return (0xFBE0 | rn, (rdlo << 12) | (rdhi << 8) | (0 << 4) | rm)

# wide load/store single. imm12 base keys (h1) and the imm8/register forms.
H1_LDRB, H1_LDR, H1_STR, H1_STRB, H1_STRH, H1_LDRH = (0xF890, 0xF8D0, 0xF8C0, 0xF880, 0xF8A0, 0xF8B0)
H1_LDRSB, H1_LDRSH = 0xF990, 0xF9B0
H1_LDRB8, H1_LDR8, H1_STR8, H1_STRB8, H1_STRH8, H1_LDRH8 = (0xF810, 0xF850, 0xF840, 0xF800, 0xF820, 0xF830)
H1_LDRSB8, H1_LDRSH8 = 0xF910, 0xF930

def wid_imm12(h1, rn, rt, imm):
    return (h1 | rn, (rt << 12) | (imm & 0xFFF))
def wid_imm8(h1, rn, rt, P, U, W, imm):
    return (h1 | rn, (rt << 12) | (1 << 11) | (P << 10) | (U << 9) | (W << 8) | (imm & 0xFF))
def wid_reg(h1, rn, rt, rm, imm2):
    return (h1 | rn, (rt << 12) | (imm2 << 4) | rm)

def t_sxth(rd, rm):  return 0xB200 | (rm << 3) | rd
def t_sxtb(rd, rm):  return 0xB240 | (rm << 3) | rd
def t_uxth(rd, rm):  return 0xB280 | (rm << 3) | rd
def t_uxtb(rd, rm):  return 0xB2C0 | (rm << 3) | rd
def t_rev(rd, rm):   return 0xBA00 | (rm << 3) | rd
def t_rev16(rd, rm): return 0xBA40 | (rm << 3) | rd
def t_revsh(rd, rm): return 0xBAC0 | (rm << 3) | rd

# VFP memory, A32 (bits: cond 110 P U D W L Rn | Vd 101x imm8; SP = r13)
def _a32_vfp(cop, P, U, D, W, L, rn, Vd, imm8):
    return ((0xE << 28) | (0x6 << 25) | (P << 24) | (U << 23) | (D << 22) |
            (W << 21) | (L << 20) | (rn << 16) | (Vd << 12) | (cop << 8) | (imm8 & 0xFF))
def a32_vldr_s(rn, s, off=0):  return _a32_vfp(0xA, 1, 1, s & 1, 0, 1, rn, s >> 1, off >> 2)
def a32_vstr_s(rn, s, off=0):  return _a32_vfp(0xA, 1, 1, s & 1, 0, 0, rn, s >> 1, off >> 2)
def a32_vldr_d(rn, d, off=0):  return _a32_vfp(0xB, 1, 1, (d >> 4) & 1, 0, 1, rn, d & 0xF, off >> 2)
def a32_vpush_s(first, count): return _a32_vfp(0xA, 1, 0, first & 1, 1, 0, 13, first >> 1, count)
def a32_vpop_s(first, count):  return _a32_vfp(0xA, 0, 1, first & 1, 1, 1, 13, first >> 1, count)
def a32_vpush_d(first, count): return _a32_vfp(0xB, 1, 0, (first >> 4) & 1, 1, 0, 13, first & 0xF, count * 2)
def a32_vpop_d(first, count):  return _a32_vfp(0xB, 0, 1, (first >> 4) & 1, 1, 1, 13, first & 0xF, count * 2)

# VFP memory, T32 (h1 = 1110 110 P U D W L Rn, h2 = Vd 101x imm8)
def _t32_vfp(P, U, D, W, L, rn, Vd, cop, imm8):
    h1 = 0xEC00 | (P << 8) | (U << 7) | (D << 6) | (W << 5) | (L << 4) | rn
    h2 = (Vd << 12) | (cop << 8) | (imm8 & 0xFF)
    return (h1, h2)
def t32_vldr_s(rn, s, off=0): return _t32_vfp(1, 1, s & 1, 0, 1, rn, s >> 1, 0xA, off >> 2)
def t32_vstr_s(rn, s, off=0): return _t32_vfp(1, 1, s & 1, 0, 0, rn, s >> 1, 0xA, off >> 2)
def t32_vstm_ia(rn, first, count): return _t32_vfp(0, 1, first & 1, 0, 0, rn, first >> 1, 0xA, count)
def t32_vldm_ia(rn, first, count): return _t32_vfp(0, 1, first & 1, 0, 1, rn, first >> 1, 0xA, count)


# scratch offsets whose seeded contents are known: word k = 0x10000000 + k
W0, W1, W2 = 0x10000000, 0x10000001, 0x10000002


def add_cases():
    """Append the new cases to gen_cases.CASES. Idempotent given the same
    starting CASES (build the base first)."""
    base = [c["name"] for c in G.CASES]
    if "w_mul" in base:
        return

    def T(name, group, desc, init, items):
        bd = G.Body()
        for it in items:
            if it[0] == "w":
                bd.w(it[1], it[2])
            elif it[0] == "t":
                bd.t(it[1], it[2])
            else:
                bd.t2(it[1][0], it[1][1], it[2])
        G.add(name, group, desc, init, bd, mode="thumb")

    def A(name, group, desc, init, items):
        bd = G.Body()
        for (word, text) in items:
            bd.w(word, text)
        G.add(name, group, desc, init, bd, mode="arm")

    # ---- wide multiply ------------------------------------------------
    T("w_mul", "wide-mul", "Thumb-2 MUL.W Rd,Rn,Rm (Ra=15 alias)",
      {0: 0x12345678, 1: 0x00001000}, [("2", mul_w(2, 0, 1), "mul r2, r0, r1")])
    T("w_mla", "wide-mul", "Thumb-2 MLA.W adds Ra (distinguishes it from MUL)",
      {0: 0x00000005, 1: 0x00000007, 3: 0x00000064},
      [("2", mla_w(2, 0, 1, 3), "mla r2, r0, r1, r3")])
    T("w_mls", "wide-mul", "Thumb-2 MLS.W Ra - Rn*Rm",
      {0: 0x00000005, 1: 0x00000007, 3: 0x00000064},
      [("2", mls_w(2, 0, 1, 3), "mls r2, r0, r1, r3")])
    T("w_smull", "wide-mul", "Thumb-2 SMULL signed 64-bit product",
      {0: 0xFFFFFFFF, 1: 0x00000002},
      [("2", smull_w(2, 3, 0, 1), "smull r2, r3, r0, r1")])
    T("w_umull", "wide-mul", "Thumb-2 UMULL unsigned 64-bit product",
      {0: 0xFFFFFFFF, 1: 0x00000002},
      [("2", umull_w(2, 3, 0, 1), "umull r2, r3, r0, r1")])
    T("w_smlal", "wide-mul", "Thumb-2 SMLAL signed accumulate (64-bit) and sign",
      {0: 0x80000000, 1: 0x00000002, 2: 0x00000001, 3: 0x00000001},
      [("2", smlal_w(2, 3, 0, 1), "smlal r2, r3, r0, r1")])
    T("w_umlal", "wide-mul", "Thumb-2 UMLAL unsigned accumulate (64-bit)",
      {0: 0xFFFFFFFF, 1: 0x00000003, 2: 0x00000001, 3: 0x00000001},
      [("2", umlal_w(2, 3, 0, 1), "umlal r2, r3, r0, r1")])

    # ---- wide load/store single (base r9 = scratch, offsets known) ----
    T("w_ldrb_imm12", "wide-ldst", "Thumb-2 LDRB imm12 (0xF890)",
      {}, [("2", wid_imm12(H1_LDRB, 9, 0, 4), "ldrb.w r0, [r9, #4]")])
    T("w_ldr_imm12", "wide-ldst", "Thumb-2 LDR imm12 (0xF8D0)",
      {}, [("2", wid_imm12(H1_LDR, 9, 0, 4), "ldr.w r0, [r9, #4]")])
    T("w_str_imm12", "wide-ldst", "Thumb-2 STR imm12 writes the scratch window",
      {0: 0xCAFEBABE}, [("2", wid_imm12(H1_STR, 9, 0, 8), "str.w r0, [r9, #8]")])
    T("w_strh_imm12", "wide-ldst", "Thumb-2 STRH imm12",
      {0: 0x0000BEEF}, [("2", wid_imm12(H1_STRH, 9, 0, 6), "strh.w r0, [r9, #6]")])
    T("w_ldrh_imm12", "wide-ldst", "Thumb-2 LDRH imm12",
      {}, [("2", wid_imm12(H1_LDRH, 9, 0, 4), "ldrh.w r0, [r9, #4]")])
    T("w_ldrb_imm8_pos", "wide-ldst", "Thumb-2 LDRB imm8 P=1,U=1,W=0 (the T3 form)",
      {}, [("2", wid_imm8(H1_LDRB8, 9, 0, 1, 1, 0, 4), "ldrb.w r0, [r9, #4]")])
    T("w_ldrb_imm8_neg", "wide-ldst", "Thumb-2 LDRB imm8 U=0 reads below the base (0 in the arena)",
      {}, [("2", wid_imm8(H1_LDRB8, 9, 0, 1, 0, 0, 4), "ldrb r0, [r9, #-4]")])
    T("w_ldr_post", "wide-ldst", "Thumb-2 LDR post-indexed: the writeback moves the next load's address",
      {}, [("2", wid_imm8(H1_LDR8, 9, 0, 0, 1, 1, 4), "ldr r0, [r9], #4"),
           ("2", wid_imm8(H1_LDR8, 9, 1, 1, 1, 0, 0), "ldr r1, [r9]")])
    T("w_ldr_pre_wb", "wide-ldst", "Thumb-2 LDR pre-indexed writeback (P=1,U=1,W=1)",
      {}, [("2", wid_imm8(H1_LDR8, 9, 0, 1, 1, 1, 4), "ldr r0, [r9, #4]!"),
           ("2", wid_imm8(H1_LDR8, 9, 1, 1, 1, 0, 0), "ldr r1, [r9]")])
    T("w_str_pre_wb_neg", "wide-ldst", "Thumb-2 STR pre-indexed negative writeback writes below the base",
      {0: 0xAAAA5555}, [("2", wid_imm8(H1_STR8, 9, 0, 1, 0, 1, 4), "str r0, [r9, #-4]!")])
    T("w_ldr_reg", "wide-ldst", "Thumb-2 LDR register offset",
      {2: 0x00000004}, [("2", wid_reg(H1_LDR8, 9, 0, 2, 0), "ldr.w r0, [r9, r2]")])
    T("w_ldr_reg_lsl", "wide-ldst", "Thumb-2 LDR register offset with LSL #1 (tests the shift)",
      {2: 0x00000002}, [("2", wid_reg(H1_LDR8, 9, 0, 2, 1), "ldr.w r0, [r9, r2, lsl #1]")])
    T("w_ldrsb", "wide-ldst", "Thumb-2 LDRSB sign-extends a planted 0xFF byte",
      {1: 0x000000FF},
      [("2", wid_imm12(H1_STRB, 9, 1, 1), "strb.w r1, [r9, #1]"),
       ("2", wid_imm12(H1_LDRSB, 9, 0, 1), "ldrsb.w r0, [r9, #1]")])
    T("w_ldrsh", "wide-ldst", "Thumb-2 LDRSH sign-extends a planted 0x8001 halfword",
      {1: 0x00008001},
      [("2", wid_imm12(H1_STRH, 9, 1, 2), "strh.w r1, [r9, #2]"),
       ("2", wid_imm12(H1_LDRSH, 9, 0, 2), "ldrsh.w r0, [r9, #2]")])

    # ---- 16-bit Thumb extend / reverse ---------------------------------
    T("w_x_sxtb", "wide-ext", "Thumb-1 SXTB sign-extends the low byte",
      {0: 0x000080FF}, [("t", t_sxtb(1, 0), "sxtb r1, r0")])
    T("w_x_uxtb", "wide-ext", "Thumb-1 UXTB keeps only the low byte",
      {0: 0x12345678}, [("t", t_uxtb(1, 0), "uxtb r1, r0")])
    T("w_x_sxth", "wide-ext", "Thumb-1 SXTH sign-extends the low halfword",
      {0: 0x00008001}, [("t", t_sxth(1, 0), "sxth r1, r0")])
    T("w_x_uxth", "wide-ext", "Thumb-1 UXTH keeps only the low halfword",
      {0: 0x1234ABCD}, [("t", t_uxth(1, 0), "uxth r1, r0")])
    T("w_x_rev", "wide-ext", "Thumb-1 REV byte-reverses the word",
      {0: 0x12345678}, [("t", t_rev(1, 0), "rev r1, r0")])
    T("w_x_rev16", "wide-ext", "Thumb-1 REV16 byte-reverses each halfword",
      {0: 0x12345678}, [("t", t_rev16(1, 0), "rev16 r1, r0")])
    T("w_x_revsh", "wide-ext", "Thumb-1 REVSH reverses the low halfword and sign-extends",
      {0: 0x00000080}, [("t", t_revsh(1, 0), "revsh r1, r0")])

    # ---- VFP memory ----------------------------------------------------
    A("w_vfp_vldr_s", "wide-vfp", "A32 VLDR s0, [r9]: the register file takes the memory word",
      {}, [(a32_vldr_s(9, 0), "vldr s0, [r9]")])
    A("w_vfp_vstr_s", "wide-vfp", "A32 VSTR s0, [r9, #4]: the scratch window takes s0",
      {}, [(a32_vstr_s(9, 0, 4), "vstr s0, [r9, #4]")])
    A("w_vfp_vldr_d", "wide-vfp", "A32 VLDR d0, [r9, #4]: two s registers from one doubleword",
      {}, [(a32_vldr_d(9, 0, 4), "vldr d0, [r9, #4]")])
    A("w_vfp_vpush_vpop_s", "wide-vfp", "A32 VPUSH then VPOP: s8..s15 come back equal to s0..s7",
      {}, [(G.dp_imm(G.OP["add"], 13, 9, 0x100), "add sp, r9, #0x100"),
           (a32_vpush_s(0, 8), "vpush {s0-s7}"),
           (a32_vpop_s(8, 8), "vpop {s8-s15}")])
    A("w_vfp_vpush_vpop_d", "wide-vfp", "A32 VPUSH/VPOP doublewords: d4..d7 come back equal to d0..d3",
      {}, [(G.dp_imm(G.OP["add"], 13, 9, 0x100), "add sp, r9, #0x100"),
           (a32_vpush_d(0, 4), "vpush {d0-d3}"),
           (a32_vpop_d(4, 4), "vpop {d4-d7}")])
    A("w_vfp_vstm_vldm", "wide-vfp", "A32 VSTMIA/VLDMIA on a general register (no writeback)",
      {}, [(0xEC890A08, "vstmia r9, {s0-s7}"),
           (0xEC994A08, "vldmia r9, {s8-s15}")])
    T("w_vfp_t32_vldr", "wide-vfp", "Thumb-2 VLDR s0, [r9]",
      {}, [("2", t32_vldr_s(9, 0), "vldr s0, [r9]")])
    T("w_vfp_t32_vstr", "wide-vfp", "Thumb-2 VSTR s0, [r9, #4]",
      {}, [("2", t32_vstr_s(9, 0, 4), "vstr s0, [r9, #4]")])
    T("w_vfp_t32_vstm_vldm", "wide-vfp", "Thumb-2 VSTMIA/VLDMIA round trip",
      {}, [("2", t32_vstm_ia(9, 0, 8), "vstmia r9, {s0-s7}"),
           ("2", t32_vldm_ia(9, 8, 8), "vldmia r9, {s8-s15}")])


def write_subset():
    """Emit ONLY the new cases to their own .S/.h, so the harness can pick them
    up without regenerating gen_cases.py's file."""
    G.CASES.clear()
    G.build()
    add_cases()
    all_cases = list(G.CASES)
    base_n = 970
    new = all_cases[base_n:]
    assert new, "no new cases were appended"
    saved = G.CASES
    G.CASES = new
    try:
        with open(WIDE_S, "w") as f:
            f.write(G.emit_s())
        with open(WIDE_H, "w") as f:
            f.write(G.emit_h())
    finally:
        G.CASES = saved
    print("gen_cases_wide.py: wrote %s and %s (%d new cases, ids %d..%d)"
          % (os.path.basename(WIDE_S), os.path.basename(WIDE_H),
             len(new), new[0]["id"], new[-1]["id"]))
    return new


if __name__ == "__main__":
    write_subset()
