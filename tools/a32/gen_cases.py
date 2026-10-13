#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
gen_cases.py -- the AArch32 case generator for the differential harness.

    python3 tools/a32/gen_cases.py     # writes a32_cases_gen.S / a32_cases_gen.h

WHAT A "CASE" IS (the case format, unchanged from the first increment)
---------------------------------------------------------------------
A case is an ARM-state (A32) fragment plus an initial register state, and it is
executed by BOTH sides of the harness:

  * the qemu-arm oracle (a32oracle.c, ubuntu job) runs the fragment compiled into
    a32_cases_gen.S inside the a32_case_<id> wrapper, and writes the resulting
    architectural state as a golden;
  * the a32_interp reference interpreter (macos-26 compare job) runs the SAME
    fragment via the word table emitted into a32_cases_gen.h, and the comparer
    diffs the two.

The two sides execute byte-identical code because the generator is the single
source of truth: it ENCODES each fragment to raw A32 words once, emits those
words as `.word` directives in the .S (so the oracle executes exactly them) and
as a uint32_t table in the .h (so the interpreter executes exactly them). This is
stronger than emitting mnemonics for the oracle and hoping an encoder agrees.

The wrapper a32_case_<id> (unchanged): saves r4-r11/lr, points r12 at
&out->regs[0] (out + A32_OFF_REGS) and r9 at the scratch window, forces a known
incoming flag state (`mov r0,#0 ; cmp r0,#0` -> N=0 Z=1 C=1 V=0), zeroes the
comparable GPRs, loads the case's init (movw/movt only, never touching flags),
runs the fragment, then captures r0..r11 (`stm r12,{r0-r11}`), cpsr (`mrs`) and
forces regs[12..15] to 0. `push`/`pop` are kept so the bodies never depend on the
current sp/lr, and the interpreter reproduces the same initial state directly.

Reserved in a body: r9 = scratch window base (an ADDRESS, recorded, never
compared), r12 = the out pointer. The comparable GPRs are r0-r8, r10, r11.
r13/r14/r15 are written 0 and are not compared; branches are asserted through
GPRs, and `adr` results are always consumed into a comparable value, never left
in a compared register (an absolute address is not architectural).

The scratch window both sides honour is A32_MEM_SIZE bytes seeded
word k = 0x10000000 + k (a32oracle.c's mem_seed and a32_interp.c agree), and its
FNV-1a hash is what the golden compares.

MONOTONY / DETERMINISM
----------------------
Nothing here is random at run time: operand variety comes from fixed tables plus
a fixed-seed PRNG, so regenerating produces byte-identical .S/.h and the CI
"the tree is in sync" check is meaningful.

EXTENSION POINTS (named, not wired)
-----------------------------------
* Lifted real functions: tools/a32/extract.py (milestone 2) will cut functions out
  of lib/armeabi-v7a/*.so, encode them as words and add a case with
  source:"lifted"; the oracle, the interpreter and the comparer need no change.
* Thumb / VFP / NEON / IT-state: out of scope this increment; a case that reaches
  one makes a32_interp fault BY NAME and the harness reports it as not passing.
"""
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
S_OUT = os.path.join(HERE, "a32_cases_gen.S")
H_OUT = os.path.join(HERE, "a32_cases_gen.h")

# ---------------------------------------------------------------------------
# A32 encoder. Every function returns one 32-bit ARM (A32) instruction word.
# ---------------------------------------------------------------------------
COND = {"eq": 0, "ne": 1, "cs": 2, "hs": 2, "cc": 3, "lo": 3, "mi": 4, "pl": 5,
        "vs": 6, "vc": 7, "hi": 8, "ls": 9, "ge": 10, "lt": 11, "gt": 12,
        "le": 13, "al": 14}

OPN = ["and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
       "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"]
OP = {n: i for i, n in enumerate(OPN)}
TEST_OPS = {8, 9, 10, 11}
SHN = ["LSL", "LSR", "ASR", "ROR"]


def rot_imm(value):
    """(rot, imm8) with ROR(imm8, 2*rot) == value, or None if not encodable."""
    value &= 0xFFFFFFFF
    for rot in range(16):
        s = 2 * rot
        imm8 = value if s == 0 else ((value << s) | (value >> (32 - s))) & 0xFFFFFFFF
        if imm8 < 256:
            return (rot, imm8)
    return None


def dp_reg(op, rd, rn, rm, s=0, cond=14, st=0, si=0):
    return (cond << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | (si << 7) | (st << 5) | rm


def dp_regrs(op, rd, rn, rm, rs, s=0, cond=14, st=0):
    return (cond << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | (rs << 8) | (st << 5) | (1 << 4) | rm


def dp_imm(op, rd, rn, value, s=0, cond=14):
    r = rot_imm(value)
    assert r is not None, "0x%08x is not an encodable ARM immediate" % value
    rot, imm8 = r
    return (cond << 28) | (1 << 25) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | (rot << 8) | imm8


def movw(rd, imm16, cond=14):
    return (cond << 28) | 0x03000000 | (((imm16 >> 12) & 0xF) << 16) | (rd << 12) | (imm16 & 0xFFF)


def movt(rd, imm16, cond=14):
    return (cond << 28) | 0x03400000 | (((imm16 >> 12) & 0xF) << 16) | (rd << 12) | (imm16 & 0xFFF)


def mul(rd, rm, rs, s=0, cond=14):
    return (cond << 28) | (s << 20) | (rd << 16) | (rs << 8) | 0x90 | rm


def mla(rd, rn, rm, rs, s=0, cond=14):
    return (cond << 28) | (1 << 21) | (s << 20) | (rd << 16) | (rn << 12) | (rs << 8) | 0x90 | rm


def mls(rd, rn, rm, rs, cond=14):
    return (cond << 28) | (0x6 << 20) | (rd << 16) | (rn << 12) | (rs << 8) | 0x90 | rm


def lmul(u, a, rdlo, rdhi, rm, rs, s=0, cond=14):
    return (cond << 28) | (0x1 << 23) | (u << 22) | (a << 21) | (s << 20) | (rdhi << 16) | (rdlo << 12) | (rs << 8) | 0x90 | rm


def mrs(rd, cond=14):
    return (cond << 28) | 0x010F0000 | (rd << 12)


def msr_reg(rm, cond=14):
    return (cond << 28) | 0x0128F000 | rm


def msr_imm(value, cond=14):
    rot, imm8 = rot_imm(value)
    return (cond << 28) | 0x0328F000 | (rot << 8) | imm8


def clz(rd, rm, cond=14):
    return (cond << 28) | 0x016F0F10 | (rd << 12) | rm


def mem_w(l, b, rd, rn, imm12, p=1, u=1, w=0, cond=14):
    return (cond << 28) | 0x04000000 | (p << 24) | (u << 23) | (b << 22) | (w << 21) | (l << 20) | (rn << 16) | (rd << 12) | (imm12 & 0xFFF)


def mem_w_reg(l, b, rd, rn, rm, p=1, u=1, w=0, cond=14, st=0, si=0):
    return (cond << 28) | 0x06000000 | (p << 24) | (u << 23) | (b << 22) | (w << 21) | (l << 20) | (rn << 16) | (rd << 12) | (si << 7) | (st << 5) | rm


def mem_h(l, s, h, rd, rn, off, p=1, u=1, w=0, cond=14):
    return (cond << 28) | (p << 24) | (u << 23) | 0x00400000 | (w << 21) | (l << 20) | (rn << 16) | (rd << 12) | (((off >> 4) & 0xF) << 8) | (1 << 7) | (s << 6) | (h << 5) | (1 << 4) | (off & 0xF)


def mem_h_reg(l, s, h, rd, rn, rm, p=1, u=1, w=0, cond=14):
    return (cond << 28) | (p << 24) | (u << 23) | (w << 21) | (l << 20) | (rn << 16) | (rd << 12) | (1 << 7) | (s << 6) | (h << 5) | (1 << 4) | rm


def dword(load, rd, rn, off, p=1, u=1, w=0, cond=14):
    """LDRD (bits[7:4]=1101) / STRD (bits[7:4]=1111); bit20 is 0 for both."""
    nib = 0xD if load else 0xF
    return (cond << 28) | (p << 24) | (u << 23) | 0x00400000 | (w << 21) | (rn << 16) | (rd << 12) | (((off >> 4) & 0xF) << 8) | (nib << 4) | (off & 0xF)


def swp(rd, rm, rn, b=0, cond=14):
    return (cond << 28) | (b << 22) | 0x01000090 | (rn << 16) | (rd << 12) | rm


def blk(l, rn, lst, p, u, w=0, cond=14):
    return (cond << 28) | 0x08000000 | (p << 24) | (u << 23) | (w << 21) | (l << 20) | (rn << 16) | lst


def bx(rm, cond=14):
    return (cond << 28) | 0x012FFF10 | rm


def blx_reg(rm, cond=14):
    return (cond << 28) | 0x012FFF30 | rm


# ---------------------------------------------------------------------------
# A tiny body builder: words with a printable comment, plus labels resolved in a
# second pass (so forward branches and `adr` work).
# ---------------------------------------------------------------------------
class Body:
    def __init__(self):
        self.items = []        # [word, comment]
        self.labels = {}
        self.fixups = []       # (index, kind, label, *rest)

    def w(self, word, text):
        self.items.append([word & 0xFFFFFFFF, text])
        return len(self.items) - 1

    def label(self, name):
        self.labels[name] = len(self.items)

    def b(self, label, cond=14):
        i = self.w(0, "b%s %s" % ("" if cond == 14 else COND_NAME[cond], label))
        self.fixups.append((i, "b", label, cond))

    def bl(self, label, cond=14):
        i = self.w(0, "bl%s %s" % ("" if cond == 14 else COND_NAME[cond], label))
        self.fixups.append((i, "bl", label, cond))

    def adr(self, rd, label):
        i = self.w(0, "adr r%d, %s" % (rd, label))
        self.fixups.append((i, "adr", label, rd))

    def resolve(self):
        words = [it[0] for it in self.items]
        comments = [it[1] for it in self.items]
        for fx in self.fixups:
            i, kind, label = fx[0], fx[1], fx[2]
            tgt = self.labels[label]
            if kind in ("b", "bl"):
                cond = fx[3]
                off = tgt - (i + 2)
                assert -0x800000 <= off < 0x800000, "branch out of range"
                base = 0x0A000000 if kind == "b" else 0x0B000000
                words[i] = (cond << 28) | base | (off & 0xFFFFFF)
            else:                                  # adr
                rd = fx[3]
                delta = tgt * 4 - (i * 4 + 8)
                if delta >= 0:
                    words[i] = dp_imm(OP["add"], rd, 15, delta)
                    comments[i] = "adr r%d, %s (add r%d, pc, #%d)" % (rd, label, rd, delta)
                else:
                    words[i] = dp_imm(OP["sub"], rd, 15, -delta)
                    comments[i] = "adr r%d, %s (sub r%d, pc, #%d)" % (rd, label, rd, -delta)
        return words, comments


COND_NAME = [None] * 16
for _n, _c in COND.items():
    COND_NAME[_c] = "." + _n if _n != "al" else ""

CASES = []


def add(name, group, desc, init, body):
    cid = len(CASES) + 1
    for reg in init:
        assert reg in (0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11), \
            "init must not set a reserved register (r9/r12-r15)"
    words, comments = body.resolve()
    CASES.append(dict(id=cid, name=name, group=group, desc=desc,
                      init=init, words=words, comments=comments))


# ---------------------------------------------------------------------------
# Operand tables (fixed + a fixed-seed PRNG, so regeneration is deterministic).
# ---------------------------------------------------------------------------
RNG = random.Random(0xA32)
EDGE = [0x00000000, 0x00000001, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000,
        0x0000FFFF, 0xDEADBEEF, 0x12345678, 0x00000002, 0xFFFFFFFE]


def operand_pairs(n):
    out = []
    for i in range(n):
        if i < len(EDGE) - 1:
            out.append((EDGE[i], EDGE[i + 1]))
        else:
            out.append((RNG.getrandbits(32), RNG.getrandbits(32)))
    return out


def imm_values(n):
    cand = [0x0, 0x1, 0xFF, 0x100, 0x3F0, 0x80000000, 0x7FFFFFFF, 0xFFFF0000,
            0x00000FF0, 0x00FF00FF, 0x0000000F]
    out = [v for v in cand if rot_imm(v) is not None]
    while len(out) < n:
        v = RNG.getrandbits(32)
        if rot_imm(v) is not None:
            out.append(v)
    return out[:n]


# ---------------------------------------------------------------------------
# The generated case set.
# ---------------------------------------------------------------------------
def gen_dp_reg():
    for op in range(16):
        for k, (a, b) in enumerate(operand_pairs(6)):
            bd = Body()
            s = 1 if op in TEST_OPS else 0
            # MOV/MVN must have Rn == 0 in the encoding (a non-zero Rn is
            # undefined); every other op uses r1 as the first operand.
            rn = 0 if op in (13, 15) else 1
            bd.w(dp_reg(op, 0, rn, 2, s=s), "%s r0, r1, r2" % (OPN[op] + ("s" if s else "")))
            add("dp_reg_%s_%d" % (OPN[op], k), "dp-reg",
                "%s r0,r1,r2 (reg form)" % OPN[op].upper(), {1: a, 2: b}, bd)


def gen_dp_reg_s():
    for op in (0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15):
        for k, (a, b) in enumerate(operand_pairs(3)):
            bd = Body()
            rn = 0 if op in (13, 15) else 1
            bd.w(dp_reg(op, 0, rn, 2, s=1), "%ss r0, r1, r2" % OPN[op])
            add("dp_reg_s_%s_%d" % (OPN[op], k), "dp-reg-s",
                "%ss r0,r1,r2 (flags)" % OPN[op].upper(), {1: a, 2: b}, bd)


def gen_dp_imm():
    imms = imm_values(6)
    for op in range(16):
        for k, iv in enumerate(imms):
            bd = Body()
            s = 1 if op in TEST_OPS else 0
            rn = 0 if op in (13, 15) else 1
            bd.w(dp_imm(op, 0, rn, iv, s=s), "%s%s r0, r1, #0x%x" % (OPN[op], "s" if s else "", iv))
            add("dp_imm_%s_%d" % (OPN[op], k), "dp-imm",
                "%s r0,r1,#0x%x" % (OPN[op].upper(), iv), {1: 0xDEADBEEF}, bd)


def gen_dp_imm_s():
    imms = imm_values(3)
    for op in (0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15):
        for k, iv in enumerate(imms):
            bd = Body()
            rn = 0 if op in (13, 15) else 1
            bd.w(dp_imm(op, 0, rn, iv, s=1), "%ss r0, r1, #0x%x" % (OPN[op], iv))
            add("dp_imm_s_%s_%d" % (OPN[op], k), "dp-imm-s",
                "%ss r0,r1,#0x%x (flags)" % (OPN[op].upper(), iv), {1: 0x12345678}, bd)


def gen_shift_imm():
    for st in range(4):
        for si in (0, 1, 5, 31):
            bd = Body()
            bd.w(dp_reg(OP["mov"], 0, 0, 1, st=st, si=si),
                 "mov r0, r1, %s #%d" % (SHN[st], si))
            add("shift_imm_%s_%d" % (SHN[st].lower(), si), "shift",
                "MOV r0,r1,%s #%d" % (SHN[st], si), {1: 0x80000001}, bd)


def gen_shift_reg():
    for st in range(4):
        for rs in (0, 1, 4, 8, 16, 30, 31, 32, 33, 63, 64, 255):
            bd = Body()
            bd.w(dp_regrs(OP["mov"], 0, 0, 1, 2, st=st),
                 "mov r0, r1, %s r2" % SHN[st])
            add("shift_reg_%s_%d" % (SHN[st].lower(), rs), "shift",
                "MOV r0,r1,%s r2 (r2=%d)" % (SHN[st], rs), {1: 0xFFFFFFFF, 2: rs}, bd)


def gen_shift_operand():
    for st in range(4):
        for si in (0, 3, 8, 16):
            bd = Body()
            bd.w(dp_reg(OP["add"], 0, 1, 2, st=st, si=si),
                 "add r0, r1, r2, %s #%d" % (SHN[st], si))
            add("add_shifted_%s_%d" % (SHN[st].lower(), si), "shift",
                "ADD r0,r1,r2,%s #%d" % (SHN[st], si), {1: 1, 2: 0x12345678}, bd)


def gen_rrx():
    for cbit in (0, 1):
        bd = Body()
        # TST sets C from a shift so we control the incoming carry, then MOV ...RRX
        bd.w(dp_reg(OP["tst"], 0, 2, 2, s=1, st=0, si=1),
             "tst r2, r2, LSL #1")   # C = bit31 of r2
        bd.w(dp_reg(OP["mov"], 0, 0, 1, st=3, si=0), "mov r0, r1, RRX")
        val = 0x80000000 if cbit else 0x40000000   # LSL #1 carry-out = 1 or 0
        add("rrx_c%d" % cbit, "shift", "MOV r0,r1,RRX (carry=%d)" % cbit,
            {1: 0x80000001, 2: val}, bd)


def gen_mul():
    for k, (a, b) in enumerate(operand_pairs(4)):
        bd = Body(); bd.w(mul(0, 1, 2), "mul r0, r1, r2")
        add("mul_%d" % k, "mul", "MUL r0,r1,r2", {1: a, 2: b}, bd)
    for k, (a, b) in enumerate(operand_pairs(3)):
        bd = Body(); bd.w(mla(0, 3, 1, 2), "mla r0, r3, r1, r2")
        add("mla_%d" % k, "mul", "MLA r0,r3,r1,r2", {1: a, 2: b, 3: 0x11111111}, bd)
    for k, (a, b) in enumerate(operand_pairs(3)):
        bd = Body(); bd.w(mls(0, 3, 1, 2), "mls r0, r3, r1, r2")
        add("mls_%d" % k, "mul", "MLS r0,r3,r1,r2", {1: a, 2: b, 3: 0x11111111}, bd)
    for k, (a, b) in enumerate(operand_pairs(3)):
        bd = Body(); bd.w(lmul(0, 0, 0, 3, 1, 2), "umull r0, r3, r1, r2")
        add("umull_%d" % k, "mul", "UMULL r0,r3,r1,r2", {1: a, 2: b}, bd)
    for k, (a, b) in enumerate(operand_pairs(2)):
        bd = Body(); bd.w(lmul(0, 1, 0, 3, 1, 2), "umlal r0, r3, r1, r2")
        add("umlal_%d" % k, "mul", "UMLAL r0,r3,r1,r2", {1: a, 2: b, 3: 1}, bd)
    for k, (a, b) in enumerate(operand_pairs(3)):
        bd = Body(); bd.w(lmul(1, 0, 0, 3, 1, 2), "smull r0, r3, r1, r2")
        add("smull_%d" % k, "mul", "SMULL r0,r3,r1,r2", {1: a, 2: b}, bd)
    for k, (a, b) in enumerate(operand_pairs(2)):
        bd = Body(); bd.w(lmul(1, 1, 0, 3, 1, 2), "smlal r0, r3, r1, r2")
        add("smlal_%d" % k, "mul", "SMLAL r0,r3,r1,r2", {1: a, 2: b, 3: 1}, bd)
    for k, (a, b) in enumerate(operand_pairs(2)):
        bd = Body(); bd.w(mul(0, 1, 2, s=1), "muls r0, r1, r2")
        add("muls_%d" % k, "mul", "MULS r0,r1,r2 (flags)", {1: a, 2: b}, bd)
    for k, (a, b) in enumerate(operand_pairs(2)):
        bd = Body(); bd.w(lmul(0, 0, 0, 3, 1, 2, s=1), "umulls r0, r3, r1, r2")
        add("umulls_%d" % k, "mul", "UMULLS r0,r3,r1,r2 (flags)", {1: a, 2: b}, bd)


def gen_psr():
    # MRS then extract the flag nibble so no mode/reserved bits leak into a
    # compared register.
    for k in range(3):
        bd = Body()
        bd.w(mrs(0), "mrs r0, cpsr")
        bd.w(dp_reg(OP["mov"], 0, 0, 0, st=1, si=28), "lsr r0, r0, #28")
        add("mrs_flag_%d" % k, "psr", "MRS r0,cpsr; LSR r0,#28", {}, bd)
    # MSR flags then read them straight back (no flag-setting op in between).
    for k, val in enumerate((0xF0000000, 0x00000000, 0x20000000, 0xE0000000)):
        bd = Body()
        bd.w(movw(1, val & 0xFFFF), "movw r1, #0x%x" % (val & 0xFFFF))
        if (val >> 16) & 0xFFFF:
            bd.w(movt(1, (val >> 16) & 0xFFFF), "movt r1, #0x%x" % ((val >> 16) & 0xFFFF))
        bd.w(msr_reg(1), "msr cpsr_f, r1")
        bd.w(mrs(0), "mrs r0, cpsr")
        bd.w(dp_reg(OP["mov"], 0, 0, 0, st=1, si=28), "lsr r0, r0, #28")
        add("msr_flag_%d" % k, "psr", "MSR cpsr_f then read the flags back", {1: 0}, bd)
    for k, iv in enumerate((0x80000000, 0x40000000, 0xF0000000)):
        bd = Body()
        bd.w(msr_imm(iv), "msr cpsr_f, #0x%x" % iv)
        bd.w(mrs(0), "mrs r0, cpsr")
        bd.w(dp_reg(OP["mov"], 0, 0, 0, st=1, si=28), "lsr r0, r0, #28")
        add("msr_imm_flag_%d" % k, "psr", "MSR immediate flags + read back", {}, bd)


def gen_clz():
    for k, v in enumerate((0x00000000, 0x00000001, 0x08000000, 0xFFFFFFFF)):
        bd = Body()
        bd.w(clz(0, 1), "clz r0, r1")
        add("clz_%d" % k, "misc", "CLZ r0,r1", {1: v}, bd)


def gen_mem_imm():
    for k, off in enumerate((0, 4, 8, 16, 32, 64)):
        bd = Body()
        bd.w(mem_w(0, 0, 1, 9, off), "str r1, [r9, #%d]" % off)
        bd.w(mem_w(1, 0, 0, 9, off), "ldr r0, [r9, #%d]" % off)
        add("str_ldr_imm_%d" % k, "mem", "STR/LDR word immediate offset", {1: 0x11223344 + k}, bd)
    for k, off in enumerate((0, 3, 7, 15)):
        bd = Body()
        bd.w(mem_w(0, 1, 1, 9, off), "strb r1, [r9, #%d]" % off)
        bd.w(mem_w(1, 1, 0, 9, off), "ldrb r0, [r9, #%d]" % off)
        add("strb_ldrb_imm_%d" % k, "mem", "STRB/LDRB immediate offset", {1: 0xAB + k}, bd)
    for k, off in enumerate((0, 2, 6, 10)):
        bd = Body()
        bd.w(mem_h(0, 0, 1, 1, 9, off), "strh r1, [r9, #%d]" % off)
        bd.w(mem_h(1, 0, 1, 0, 9, off), "ldrh r0, [r9, #%d]" % off)
        add("strh_ldrh_imm_%d" % k, "mem", "STRH/LDRH immediate offset", {1: 0xBEEF}, bd)
    for k, off in enumerate((0, 4, 8, 12)):
        bd = Body()
        bd.w(mem_h(1, 1, 0, 0, 9, off), "ldrsb r0, [r9, #%d]" % off)
        bd.w(mem_h(1, 1, 1, 1, 9, off), "ldrsh r1, [r9, #%d]" % off)
        add("ldrs_imm_%d" % k, "mem", "LDRSB/LDRSH immediate offset", {}, bd)
    for k, off in enumerate((4, 8, 20, 40)):
        bd = Body()
        bd.w(mem_h(1, 0, 1, 0, 9, off, p=0, u=1), "ldrh r0, [r9], #%d" % off)
        add("ldrh_post_%d" % k, "mem", "LDRH post-indexed", {}, bd)


def gen_mem_reg():
    for k, rm in enumerate((4, 8, 12, 16)):
        bd = Body()
        bd.w(mem_w_reg(0, 0, 1, 9, 2), "str r1, [r9, r2]")
        bd.w(mem_w_reg(1, 0, 0, 9, 2), "ldr r0, [r9, r2]")
        add("ldr_str_reg_%d" % k, "mem", "LDR/STR register offset", {1: 0x55667788, 2: rm}, bd)
    for k, (rm, si) in enumerate(((2, 0), (2, 2))):
        bd = Body()
        bd.w(mem_w_reg(1, 0, 0, 9, rm, st=0, si=si), "ldr r0, [r9, r%d, LSL #%d]" % (rm, si))
        add("ldr_shifted_reg_%d" % k, "mem", "LDR shifted register offset", {rm: 4}, bd)
    for k, rm in enumerate((4, 8)):
        bd = Body()
        bd.w(mem_h_reg(0, 0, 1, 1, 9, rm), "strh r1, [r9, r%d]" % rm)
        bd.w(mem_h_reg(1, 0, 1, 0, 9, rm), "ldrh r0, [r9, r%d]" % rm)
        add("strh_ldrh_reg_%d" % k, "mem", "STRH/LDRH register offset", {1: 0xCAFE, rm: 6}, bd)


def gen_mem_indexed():
    for k, off in enumerate((4, 8, 12)):
        bd = Body()
        bd.w(mem_w(0, 0, 1, 9, off, p=1, u=1, w=1), "str r1, [r9, #%d]!" % off)
        bd.w(mem_w(1, 0, 0, 9, off, p=1, u=1, w=1), "ldr r0, [r9, #%d]!" % off)
        add("str_ldr_pre_wb_%d" % k, "mem", "STR/LDR pre-indexed writeback", {1: 0x99AABBCC}, bd)
    for k, off in enumerate((4, 8)):
        bd = Body()
        bd.w(mem_w(0, 0, 1, 9, off, p=0, u=1), "str r1, [r9], #%d" % off)
        bd.w(mem_w(1, 0, 0, 9, off, p=0, u=1), "ldr r0, [r9], #%d" % off)
        add("str_ldr_post_%d" % k, "mem", "STR/LDR post-indexed", {1: 0xDEAD1234}, bd)
    for k, off in enumerate((4, 8)):
        bd = Body()
        bd.w(dp_reg(OP["mov"], 1, 0, 9), "mov r1, r9")     # window base, then +64
        bd.w(dp_imm(OP["add"], 1, 1, 64), "add r1, r1, #64")
        bd.w(mem_w(1, 0, 0, 1, off, p=1, u=0), "ldr r0, [r1, #-%d]" % off)
        bd.w(dp_imm(OP["mov"], 1, 0, 0), "mov r1, #0")   # r1 held an address; clear it
        add("ldr_neg_%d" % k, "mem", "LDR negative immediate offset (still inside the window)",
            {}, bd)


def gen_mem_double():
    for k, off in enumerate((0, 4, 8, 16)):
        bd = Body()
        bd.w(dword(0, 2, 9, off), "strd r2, [r9, #%d]" % off)
        bd.w(dword(1, 4, 9, off), "ldrd r4, [r9, #%d]" % off)
        add("strd_ldrd_%d" % k, "mem", "STRD/LDRD immediate", {2: 0xAAAABBBB, 3: 0xCCCCDDDD}, bd)
    for k, off in enumerate((0, 8)):
        bd = Body()
        bd.w(dword(0, 2, 9, off), "strd r2, [r9, #%d]" % off)
        bd.w(dword(1, 4, 9, off), "ldrd r4, [r9, #%d]" % off)
        bd.w(dp_reg(OP["mov"], 2, 0, 0), "mov r2, #0")
        bd.w(dp_reg(OP["mov"], 3, 0, 0), "mov r3, #0")
        add("strd_ldrd_zero_%d" % k, "mem", "STRD/LDRD then zero the sources", {}, bd)


def gen_swp():
    for k, b in enumerate((0, 1)):
        bd = Body()
        bd.w(mem_w(0, 0 if not b else 1, 1, 9, 8), "str r1, [r9, #8]")
        bd.w(swp(0, 2, 9, b), "swp r0, r2, [r9]")
        add("swpb_%d" % (b), "mem", "SWP/SWPB", {1: 0x01020304, 2: 0xFEEDFACE}, bd)


def gen_block():
    modes = [("ia", 0, 1), ("ib", 1, 1), ("da", 0, 0), ("db", 1, 0)]
    for (mn, p, u) in modes:
        bd = Body()
        bd.w(blk(0, 9, (1 << 2) | (1 << 3) | (1 << 4), p, u), "stm%s r9, {r2-r4}" % mn)
        add("stm_%s" % mn, "block", "STM%s r9,{r2,r3,r4}" % mn.upper(),
            {2: 0x11111111, 3: 0x22222222, 4: 0x33333333}, bd)
        bd = Body()
        bd.w(blk(1, 9, (1 << 0) | (1 << 1) | (1 << 2), p, u), "ldm%s r9, {r0-r2}" % mn)
        add("ldm_%s" % mn, "block", "LDM%s r9,{r0,r1,r2}" % mn.upper(), {}, bd)
    # writeback forms: measure the delta against a saved copy of the base, so no
    # absolute address ever reaches a compared register.
    for (mn, p, u) in modes:
        bd = Body()
        bd.w(dp_reg(OP["mov"], 1, 0, 9), "mov r1, r9")
        bd.w(blk(0, 9, (1 << 2) | (1 << 3), p, u, w=1), "stm%s r9!, {r2, r3}" % mn)
        bd.w(dp_reg(OP["sub"], 0, 9, 1), "sub r0, r9, r1")
        bd.w(dp_imm(OP["mov"], 1, 0, 0), "mov r1, #0")   # r1 held an address; clear it
        add("stm_wb_%s" % mn, "block", "STM%s r9!,{r2,r3}; delta of the writeback" % mn.upper(),
            {2: 0xA1A2A3A4, 3: 0xB1B2B3B4}, bd)
    bd = Body()
    bd.w(blk(1, 9, (1 << 2) | (1 << 3), 0, 1, w=1), "ldmia r9!, {r2, r3}")
    add("ldm_wb_ia", "block", "LDMIA r9!,{r2,r3}", {}, bd)


def gen_branch():
    bd = Body()
    bd.w(dp_imm(OP["mov"], 0, 0, 0), "mov r0, #0")
    bd.b("skip")
    bd.w(dp_imm(OP["mov"], 0, 0, 0x7F), "mov r0, #0x7F")
    bd.label("skip")
    bd.w(dp_imm(OP["mov"], 1, 0, 0x2A), "mov r1, #0x2A")
    add("b_forward", "branch", "B forward skips a write", {}, bd)

    bd = Body()
    bd.w(dp_imm(OP["mov"], 0, 0, 0), "mov r0, #0")
    bd.label("top")
    bd.w(dp_imm(OP["add"], 0, 0, 1), "add r0, r0, #1")
    bd.w(dp_imm(OP["cmp"], 0, 0, 4, s=1), "cmp r0, #4")
    bd.b("top", COND["lt"])
    add("b_backward", "branch", "B backward: a real loop that terminates at r0=4", {}, bd)

    bd = Body()
    bd.w(dp_imm(OP["mov"], 0, 0, 0), "mov r0, #0")
    bd.bl("sub1")
    bd.w(dp_imm(OP["mov"], 3, 0, 0x99), "mov r3, #0x99")
    bd.b("end")
    bd.label("sub1")
    bd.w(dp_imm(OP["mov"], 0, 0, 0x11), "mov r0, #0x11")
    bd.w(bx(14), "bx lr")
    bd.label("end")
    add("bl_bx_lr", "branch", "BL into a subroutine and BX lr back", {}, bd)

    bd = Body()
    bd.adr(14, "here")
    bd.w(bx(14), "bx lr")
    bd.w(dp_imm(OP["mov"], 5, 0, 0xAB), "mov r5, #0xAB")
    bd.b("out")
    bd.label("here")
    bd.w(dp_imm(OP["mov"], 5, 0, 0x5A), "mov r5, #0x5A")
    bd.label("out")
    add("adr_bx_lr", "branch", "ADR lr then BX lr to a label", {}, bd)

    bd = Body()
    bd.adr(4, "a")
    bd.adr(5, "b")
    bd.label("a")
    bd.w(0xE1A00000, "nop")
    bd.w(0xE1A00000, "nop")
    bd.w(0xE1A00000, "nop")
    bd.label("b")
    bd.w(dp_reg(OP["sub"], 0, 5, 4), "sub r0, r5, r4")
    bd.w(dp_imm(OP["mov"], 4, 0, 0), "mov r4, #0")
    bd.w(dp_imm(OP["mov"], 5, 0, 0), "mov r5, #0")
    add("adr_delta", "branch", "ADR of two labels; the difference is architectural", {}, bd)

    bd = Body()
    bd.adr(2, "fn")
    bd.w(blx_reg(2), "blx r2")
    bd.w(dp_imm(OP["mov"], 3, 0, 0x55), "mov r3, #0x55")
    bd.w(dp_imm(OP["mov"], 2, 0, 0), "mov r2, #0")   # r2 held an address; clear it
    bd.b("done")
    bd.label("fn")
    bd.w(dp_imm(OP["mov"], 0, 0, 0x66), "mov r0, #0x66")
    bd.w(bx(14), "bx lr")
    bd.label("done")
    add("blx_reg", "branch", "BLX register to an ARM label", {}, bd)

    # all 16 conditions on a B, taken and not-taken from a known flag state.
    for cn, cc in COND.items():
        if cn in ("al", "hs", "lo"):
            continue
        bd = Body()
        bd.w(dp_imm(OP["cmp"], 0, 1, 0, s=1), "cmp r1, #0")  # Z=1 C=1
        bd.b("taken", cc)
        bd.w(dp_imm(OP["mov"], 0, 0, 0x22), "mov r0, #0x22")
        bd.b("done")
        bd.label("taken")
        bd.w(dp_imm(OP["mov"], 0, 0, 0x11), "mov r0, #0x11")
        bd.label("done")
        add("cond_b_%s" % cn, "branch", "B%s after CMP r1,#0" % cn, {1: 0}, bd)


def gen_cond():
    # every condition, exercised through a conditional MOV after a flags-setting
    # instruction, so the condition evaluator itself is what is compared.
    for cn, cc in COND.items():
        if cn in ("al", "hs", "lo"):
            continue
        bd = Body()
        bd.w(dp_imm(OP["mov"], 1, 0, 0), "mov r1, #0")
        bd.w(dp_imm(OP["cmp"], 0, 1, 0, s=1), "cmp r1, #0")
        bd.w(dp_imm(OP["mov"], 0, 0, 0x11, cond=cc), "mov%s r0, #0x11" % cn)
        bd.w(dp_imm(OP["mov"], 2, 0, 0x22), "mov r2, #0x22")
        add("cond_mov_%s" % cn, "cond", "MOV%s r0,#0x11 after CMP r1,#0" % cn, {1: 0}, bd)
    # conditional data-processing on both outcomes of a carry
    for cn, cc in (("cs", COND["cs"]), ("cc", COND["cc"])):
        for cin in (0, 1):
            bd = Body()
            bd.w(dp_imm(OP["mov"], 1, 0, 1), "mov r1, #1")
            bd.w(dp_reg(OP["sub"], 0, 1, 1, s=1), "subs r1, r1, r1")  # Z=1 C=1
            if cin == 0:
                bd.w(dp_imm(OP["sub"], 0, 1, 1, s=1), "subs r1, r1, #1")  # borrow -> C=0
            else:
                bd.w(dp_imm(OP["add"], 0, 1, 0, s=1), "adds r1, r1, #0")  # C unchanged
            bd.w(dp_imm(OP["mov"], 0, 0, 0x33, cond=cc), "mov%s r0, #0x33" % cn)
            add("cond_dp_%s_%d" % (cn, cin), "cond",
                "conditional data-processing on C=%d" % cin, {}, bd)


def gen_mixed():
    # small multi-instruction sequences that combine a flag-setting op, a shift
    # and a memory access -- the shape that actually appears in real code.
    for k in range(20):
        a = RNG.getrandbits(32)
        b = RNG.getrandbits(32)
        st = k % 4
        si = (k % 7) + 1
        bd = Body()
        bd.w(dp_reg(OP["add"], 0, 1, 2, s=1), "adds r0, r1, r2")
        bd.w(dp_reg(OP["mov"], 3, 0, 0, st=st, si=si), "mov r3, r0, %s #%d" % (SHN[st], si))
        bd.w(mem_w(0, 0, 3, 9, (k % 8) * 4), "str r3, [r9, #%d]" % ((k % 8) * 4))
        bd.w(mem_w(1, 0, 4, 9, (k % 8) * 4), "ldr r4, [r9, #%d]" % ((k % 8) * 4))
        bd.w(dp_reg(OP["eor"], 5, 4, 3), "eor r5, r4, r3")
        add("mixed_%d" % k, "mixed", "ADDS/MOV-shift/STR/LDR/EOR", {1: a, 2: b}, bd)


def build():
    gen_dp_reg(); gen_dp_reg_s(); gen_dp_imm(); gen_dp_imm_s()
    gen_shift_imm(); gen_shift_reg(); gen_shift_operand(); gen_rrx()
    gen_mul(); gen_psr(); gen_clz()
    gen_mem_imm(); gen_mem_reg(); gen_mem_indexed(); gen_mem_double(); gen_swp()
    gen_block(); gen_branch(); gen_cond(); gen_mixed()


HEADER = """\
/* GENERATED by tools/a32/gen_cases.py -- do not edit by hand.
 * Regenerate with: python3 tools/a32/gen_cases.py */
"""


def _movw_movt_lines(reg, value):
    out = ["\tmovw\tr%d, #0x%04x" % (reg, value & 0xFFFF)]
    if (value >> 16) & 0xFFFF:
        out.append("\tmovt\tr%d, #0x%04x" % (reg, (value >> 16) & 0xFFFF))
    return out


def emit_s():
    o = [HEADER, "\t.syntax unified", "\t.arm", "\t.text", ""]

    # VFP helpers (unchanged): deterministic s0..s31 and a captured fpscr.
    o += ["\t.globl\ta32_fp_init", "\t.type\ta32_fp_init, %function", "a32_fp_init:"]
    for n in range(32):
        o += ["\tmovw\tr1, #0x%04x" % (n & 0xFFFF), "\tmovt\tr1, #0x3f80",
              "\tvmov\ts%d, r1" % n]
    o += ["\tbx\tlr", "\t.size\ta32_fp_init, .-a32_fp_init", ""]
    o += ["\t.globl\ta32_fp_capture", "\t.type\ta32_fp_capture, %function", "a32_fp_capture:"]
    for n in range(32):
        o += ["\tvmov\tr1, s%d" % n, "\tstr\tr1, [r0, #%d]" % (n * 8),
              "\tmov\tr2, #0", "\tstr\tr2, [r0, #%d]" % (n * 8 + 4)]
    o += ["\tbx\tlr", "\t.size\ta32_fp_capture, .-a32_fp_capture", ""]
    o += ["\t.globl\ta32_fp_fpscr", "\t.type\ta32_fp_fpscr, %function", "a32_fp_fpscr:",
          "\tvmrs\tr0, fpscr", "\tbx\tlr", "\t.size\ta32_fp_fpscr, .-a32_fp_fpscr", ""]

    for c in CASES:
        fn = "a32_case_%d" % c["id"]
        o += ["\t.p2align 2", "\t.globl\t%s" % fn, "\t.type\t%s, %%function" % fn, "%s:" % fn,
              "\tpush\t{r4-r11, lr}",
              "\tmov\tr12, r0",
              "\tadd\tr12, r12, #260",          # A32_OFF_REGS
              "\tmov\tr9, r1",
              "\tmov\tr0, #0",
              "\tcmp\tr0, #0"]                   # deterministic incoming flags: N=0 Z=1 C=1 V=0
        for reg in (0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11):
            o.append("\tmov\tr%d, #0" % reg)
        for reg in sorted(c["init"]):
            if c["init"][reg] is None:
                continue
            o += _movw_movt_lines(reg, c["init"][reg])
        for w, cmt in zip(c["words"], c["comments"]):
            o.append("\t.word\t0x%08x\t/* %s */" % (w, cmt))
        o += ["\tstm\tr12, {r0-r11}",
              "\tmrs\tr0, cpsr",
              "\tstr\tr0, [r12, #64]",          # A32_OFF_CPSR - A32_OFF_REGS
              "\tmov\tr0, #0",
              "\tstr\tr0, [r12, #48]",
              "\tstr\tr0, [r12, #52]",
              "\tstr\tr0, [r12, #56]",
              "\tstr\tr0, [r12, #60]",
              "\tpop\t{r4-r11, pc}",
              "\t.size\t%s, .-%s" % (fn, fn), ""]
    return "\n".join(o) + "\n"


def _cstr(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


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
         "};", "",
         "/* The word-level view of each case: the initial register file and the raw",
         " * A32 fragment, byte-identical to what a32_case_<id> executes. a32diff runs",
         " * these through a32_interp. */",
         "struct a32_case_words {",
         "    int          id;",
         "    const char  *name;",
         "    const char  *group;",
         "    const char  *desc;",
         "    uint32_t     init[A32_NREG];",
         "    uint32_t     code_len;",
         "    const uint32_t *code;",
         "};", ""]
    for c in CASES:
        o.append("extern void a32_case_%d(struct a32_result *, void *);" % c["id"])
    o += ["", "static const struct a32_case_meta a32_cases[] = {"]
    for c in CASES:
        o.append('    { %d, "%s", "%s", "arm", "handwritten", "%s", a32_case_%d },'
                 % (c["id"], c["name"], c["group"], _cstr(c["desc"]), c["id"]))
    o += ["};", "",
          "#define A32_CASE_COUNT (sizeof(a32_cases) / sizeof(a32_cases[0]))", ""]

    for c in CASES:
        o.append("static const uint32_t a32_words_%d[%d] = {" % (c["id"], max(1, len(c["words"]))))
        line = "    "
        for w in c["words"]:
            piece = "0x%08xu, " % w
            if len(line) + len(piece) > 96:
                o.append(line.rstrip())
                line = "    "
            line += piece
        o.append(line.rstrip())
        o.append("};")

    o += ["", "static const struct a32_case_words a32_case_words[] = {"]
    for c in CASES:
        init = ["0"] * 16
        for reg, val in c["init"].items():
            if val is not None and reg != 9:
                init[reg] = "0x%08xu" % (val & 0xFFFFFFFF)
        o.append('    { %d, "%s", "%s", "%s", { %s }, %d, a32_words_%d },'
                 % (c["id"], c["name"], c["group"], _cstr(c["desc"]),
                    ", ".join(init), len(c["words"]), c["id"]))
    o += ["};", "",
          "#define A32_WORDS_COUNT (sizeof(a32_case_words) / sizeof(a32_case_words[0]))",
          "", "#endif /* A32_CASES_GEN_H */"]
    return "\n".join(o) + "\n"


def main():
    build()
    with open(S_OUT, "w") as f:
        f.write(emit_s())
    with open(H_OUT, "w") as f:
        f.write(emit_h())
    print("gen_cases.py: wrote %s and %s (%d cases)"
          % (os.path.basename(S_OUT), os.path.basename(H_OUT), len(CASES)))


if __name__ == "__main__":
    main()
