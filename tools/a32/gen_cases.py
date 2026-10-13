#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
gen_cases.py -- the AArch32 (ARM + Thumb) case generator for the harness.

    python3 tools/a32/gen_cases.py     # writes a32_cases_gen.S / a32_cases_gen.h

WHAT A "CASE" IS (the case format)
----------------------------------
A case is a code fragment plus an initial register state and an entry
instruction-set state (ARM or Thumb). It is executed by BOTH sides of the
harness:

  * the qemu-arm oracle (a32oracle.c, ubuntu job) runs the fragment compiled into
    a32_cases_gen.S inside the a32_case_<id> wrapper, and writes the resulting
    architectural state as a golden;
  * the a32_interp reference interpreter (macos-26 compare job) runs the SAME
    fragment bytes via the byte table emitted into a32_cases_gen.h, and the
    comparer diffs the two.

The two sides execute byte-identical code because the generator is the single
source of truth: it ENCODES each fragment to raw words once, emits those words
as `.word`/`.hword` directives in the .S (so the oracle executes exactly them)
and as a uint8_t byte table in the .h (so the interpreter executes exactly
them). This is stronger than emitting mnemonics for the oracle and hoping an
encoder agrees.

HOW THE ORACLE ENTERS THE DECLARED STATE
----------------------------------------
The wrapper a32_case_<id> is an ARM-state function. For an ARM-entry case the
fragment is emitted inline (as before). For a Thumb-entry case the wrapper:
  * saves r4-r11/lr, points r12 at &out->regs[0], r9 at the scratch window,
    forces a known incoming flag state (`mov r0,#0 ; cmp r0,#0`), zeroes the
    comparable GPRs, loads the case's init (movw/movt only) and clears lr;
  * switches the CPU to Thumb with NO register side effect at all -- `ldr pc,
    .LgoN` where `.LgoN` is an inline word holding `1f + 1` -- so the fragment
    is handed exactly its declared state. (It used to be `adr r3, 1f ; orr r3,
    r3, #1 ; blx r3`, which left r3 = the fragment's address; r3 is a compared
    register and t32_tbb/t32_tbh read their table byte from it, so the oracle
    branched wild on those cases.)
  * runs the fragment; the generator then emits a Thumb capture epilogue (which
    the interpreter does NOT execute -- it stops at the fragment's byte end):
    it stores r0-r11 through r12, records cpsr, zeroes r12-r15, then switches
    back to ARM (`ldr r3, .LretN ; bx r3`) so the ARM epilogue can reload the
    harness's own sp (stashed in out->pad, because a PUSH/POP case deliberately
    parks sp inside the scratch window) and `pop {r4-r11, pc}`. Capture is done
    in Thumb *before* any register the return gadget clobbers, so the compared
    state is never disturbed.
An ARM-entry interworking case switches state itself (BX/BLX) inside the
fragment, exactly as the architecture says, and returns to ARM before the ARM
capture.

Reserved in a body: r9 = scratch window base (an ADDRESS, recorded, never
compared), r12 = the out pointer. The comparable GPRs are r0-r8, r10, r11.
r13/r14/r15 are written 0 and are not compared; branches are asserted through
GPRs. For PUSH/POP cases the body sets `mov r13, r9 ; add sp, #64` itself, so
the stack lives at a *fixed offset inside the scratch window* -- identical on
both sides because only r9-relative offsets are architectural and r9 is the
window base on both sides, and sp is not compared.

The scratch window both sides honour is A32_MEM_SIZE bytes seeded
word k = 0x10000000 + k (a32oracle.c's mem_seed and a32_interp.c agree), and its
FNV-1a hash is what the golden compares.

MONOTONY / DETERMINISM
----------------------
Nothing here is random at run time: operand variety comes from fixed tables plus
a fixed-seed PRNG, so regenerating produces byte-identical .S/.h and the CI
"the tree is in sync" check is meaningful.
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
# Thumb (T32) encoders. 16-bit forms return one halfword; wide forms return
# (hw1, hw2).  All of these were cross-checked with capstone: each decodes to
# the intended mnemonic/operands (see interp_selftest.py's decode check).
# ---------------------------------------------------------------------------
def t_lsl_i(rd, rm, imm5): return 0x0000 | (imm5 << 6) | (rm << 3) | rd
def t_lsr_i(rd, rm, imm5): return 0x0800 | (imm5 << 6) | (rm << 3) | rd
def t_asr_i(rd, rm, imm5): return 0x1000 | (imm5 << 6) | (rm << 3) | rd
def t_add3(rd, rn, rm):    return 0x1800 | (rn << 6) | (rm << 3) | rd   # ADD reg (flags)
def t_sub3(rd, rn, rm):    return 0x1A00 | (rn << 6) | (rm << 3) | rd
def t_add_i3(rd, rn, i3):  return 0x1C00 | (i3 << 6) | (rn << 3) | rd
def t_sub_i3(rd, rn, i3):  return 0x1E00 | (i3 << 6) | (rn << 3) | rd
def t_mov8(rd, imm8):      return 0x2000 | (rd << 8) | imm8
def t_cmp8(rd, imm8):      return 0x2800 | (rd << 8) | imm8
def t_add8(rd, imm8):      return 0x3000 | (rd << 8) | imm8
def t_sub8(rd, imm8):      return 0x3800 | (rd << 8) | imm8
def t_alu(op, rd, rm):     return 0x4000 | (op << 6) | (rm << 3) | rd
def t_hi(op, rd, rm):      # op: 0 ADD, 1 CMP, 2 MOV (no flags)
    return 0x4400 | (op << 8) | ((rd >> 3) << 7) | ((rm >> 3) << 6) | ((rm & 7) << 3) | (rd & 7)
def t_bx(rm, link=0):
    return 0x4700 | (link << 7) | ((rm >> 3) << 6) | ((rm & 7) << 3)
def t_ldr_lit(rd, imm8):   return 0x4800 | (rd << 8) | imm8
def t_ldst_reg(op, rm, rn, rt): return 0x5000 | (op << 9) | (rm << 6) | (rn << 3) | rt
def t_word_i5(l, imm5, rn, rt): return 0x6000 | (l << 11) | (imm5 << 6) | (rn << 3) | rt
def t_byte_i5(l, imm5, rn, rt): return 0x7000 | (l << 11) | (imm5 << 6) | (rn << 3) | rt
def t_half_i5(l, imm5, rn, rt): return 0x8000 | (l << 11) | (imm5 << 6) | (rn << 3) | rt
def t_sp_ldst(l, rd, imm8):     return 0x9000 | (l << 11) | (rd << 8) | imm8
def t_add_pc(rd, imm8):         return 0xA000 | (rd << 8) | imm8   # ADR
def t_add_rdsp(rd, imm8):       return 0xA800 | (rd << 8) | imm8   # ADD rd, sp, #imm8*4
def t_add_sp(imm7):             return 0xB000 | imm7
def t_sub_sp(imm7):             return 0xB080 | imm7
def t_push(lst, lr):            return 0xB400 | (lr << 8) | lst
def t_pop(lst, pc):             return 0xBC00 | (pc << 8) | lst
def t_stmia(rn, lst):           return 0xC000 | (rn << 8) | lst
def t_ldmia(rn, lst):           return 0xC800 | (rn << 8) | lst
def t_bcond(cond, imm8):        return 0xD000 | (cond << 8) | imm8
def t_b(imm11):                 return 0xE000 | imm11
def t_svc(imm8):                return 0xDF00 | imm8
def t_cbz(rn, imm5):            return 0xB100 | (imm5 << 3) | rn
def t_cbnz(rn, imm5):           return 0xB900 | (imm5 << 3) | rn
def t_nop():                    return 0xBF00
def t_it(cond, mask):           return 0xBF00 | (cond << 4) | mask


def it_mask(firstcond, senses):
    """The IT mask for the full sense sequence `senses` (list of 'T'/'E', one per
    instruction in the block, the first always 'T'), by simulating the
    architectural ITSTATE advance (ARM ARM ITAdvance)."""
    want = len(senses)
    for mask in range(1, 16):
        tz = (mask & -mask).bit_length() - 1
        if 4 - tz != want:
            continue
        its = (firstcond << 4) | mask
        ok = True
        for s in senses:
            cond = (its >> 4) & 0xF
            is_t = (cond == firstcond)
            if (s == "T") != is_t:
                ok = False
                break
            if (its & 7) == 0:
                its = 0
            else:
                its = (its & 0xE0) | ((its & 0x0F) << 1)
        if ok:
            return mask
    raise ValueError("no IT mask for %s %s" % (firstcond, senses))


# ---------------------------------------------------------------------------
# A body builder: instructions with a mode, a size and a printable comment,
# plus labels resolved in a second pass (so forward branches and ADR work).
# ---------------------------------------------------------------------------
class Body:
    def __init__(self):
        self.items = []        # dict(mode,size,val,text)
        self.post = []         # raw words emitted after the fragment (excluded from code)
        self.labels = {}
        self.fixups = []

    def w(self, word, text, mode="arm"):
        self.items.append(dict(mode=mode, size=4, val=word & 0xFFFFFFFF, text=text))
        return len(self.items) - 1

    def t(self, h, text):
        self.items.append(dict(mode="thumb", size=2, val=h & 0xFFFF, text=text))
        return len(self.items) - 1

    def t2(self, h1, h2, text):
        self.items.append(dict(mode="thumb", size=4,
                               val=((h1 & 0xFFFF) | ((h2 & 0xFFFF) << 16)), text=text))
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

    def tb(self, label, cond=None):
        i = self.t(0, "b%s %s" % ("" if cond is None else CND_NAME[cond], label))
        self.fixups.append((i, "tb", label, cond))

    def tbl(self, label, blx=False):
        i = self.t2(0xF000, 0xF800, "bl%s %s" % ("x" if blx else "", label))
        self.fixups.append((i, "tbl", label, blx))

    def resolve(self):
        offs, o = [], 0
        for it in self.items:
            offs.append(o)
            o += it["size"]
        offs.append(o)      # the end-of-fragment offset (a label may sit there)
        for fx in self.fixups:
            i, kind, label = fx[0], fx[1], fx[2]
            tgt = offs[self.labels[label]]
            a = offs[i]
            if kind in ("b", "bl"):
                cond = fx[3]
                off = tgt - (a + 8)
                assert off % 4 == 0 and -0x800000 <= off < 0x800000, "arm branch out of range"
                base = 0x0A000000 if kind == "b" else 0x0B000000
                self.items[i]["val"] = (cond << 28) | base | ((off >> 2) & 0xFFFFFF)
            elif kind == "adr":
                rd = fx[3]
                delta = tgt - (a + 8)
                if delta >= 0:
                    self.items[i]["val"] = dp_imm(OP["add"], rd, 15, delta)
                    self.items[i]["text"] = "adr r%d, %s (add r%d, pc, #%d)" % (rd, label, rd, delta)
                else:
                    self.items[i]["val"] = dp_imm(OP["sub"], rd, 15, -delta)
                    self.items[i]["text"] = "adr r%d, %s (sub r%d, pc, #%d)" % (rd, label, rd, -delta)
            elif kind == "tadr":
                rd = fx[3]
                base = (a + 4) & ~3
                delta = tgt - base
                assert delta >= 0 and delta % 4 == 0 and delta <= 1020, \
                    "thumb adr target out of range/unusable (delta=%d)" % delta
                self.items[i]["val"] = t_add_pc(rd, delta >> 2)
                self.items[i]["text"] = "adr r%d, %s (add r%d, pc, #%d)" % (rd, label, rd, delta)
            elif kind == "tb":
                cond = fx[3]
                off = tgt - (a + 4)
                assert off % 2 == 0 and -2048 <= off <= 2046, "thumb b out of range"
                if cond is None:
                    self.items[i]["val"] = t_b((off >> 1) & 0x7FF)
                else:
                    imm = off >> 1
                    assert -256 <= imm <= 255, "thumb bcond out of range"
                    self.items[i]["val"] = t_bcond(cond, imm & 0xFF)
            elif kind in ("tcbz", "tcbnz"):
                rn = fx[3]
                off = tgt - (a + 4)
                assert off % 2 == 0 and 0 <= off <= 0x7E, "thumb cbz out of range"
                imm5 = off >> 1
                self.items[i]["val"] = (t_cbz(rn, imm5) if kind == "tcbz"
                                        else t_cbnz(rn, imm5))
            elif kind == "tblx":
                off = tgt - ((a + 4) & ~3)      # BLX immediate is relative to Align(PC,4)+4
                assert off % 2 == 0, "thumb blx odd offset"
                S = (off >> 24) & 1
                imm10 = (off >> 13) & 0x3FF
                imm11 = (off >> 1) & 0x7FF
                I1 = (off >> 23) & 1
                I2 = (off >> 12) & 1
                J1 = (~I1 ^ S) & 1
                J2 = (~I2 ^ S) & 1
                h1 = 0xF000 | (S << 10) | imm10
                h2 = 0xC000 | (J1 << 13) | (J2 << 11) | imm11
                self.items[i]["val"] = (h1 & 0xFFFF) | ((h2 & 0xFFFF) << 16)
            elif kind == "tbl":
                blx = fx[3]
                off = tgt - (a + 4)
                assert off % 2 == 0, "thumb bl offset odd"
                S = (off >> 24) & 1
                imm10 = (off >> 13) & 0x3FF
                imm11 = (off >> 1) & 0x7FF
                I1 = (off >> 23) & 1
                I2 = (off >> 12) & 1
                J1 = (~I1 ^ S) & 1
                J2 = (~I2 ^ S) & 1
                h1 = 0xF000 | (S << 10) | imm10
                h2 = (0xC000 if blx else 0xD000) | (J1 << 13) | (J2 << 11) | imm11
                self.items[i]["val"] = (h1 & 0xFFFF) | ((h2 & 0xFFFF) << 16)
            else:
                raise AssertionError(kind)
        # Every item must already satisfy its state's alignment, so the .S's
        # `.p2align` never inserts padding (which would break the precomputed
        # branch/ADR offsets).
        o = 0
        for it in self.items:
            if it["mode"] == "arm":
                assert o % 4 == 0, "arm item at unaligned offset %d" % o
            else:
                assert o % 2 == 0, "thumb item at unaligned offset %d" % o
            o += it["size"]
        return self.items


CND_NAME = {v: "." + k for k, v in
            {"eq": 0, "ne": 1, "cs": 2, "cc": 3, "mi": 4, "pl": 5, "vs": 6,
             "vc": 7, "hi": 8, "ls": 9, "ge": 10, "lt": 11, "gt": 12,
             "le": 13}.items()}

COND_NAME = [None] * 16
for _n, _c in COND.items():
    COND_NAME[_c] = "." + _n if _n != "al" else ""

CASES = []


def add(name, group, desc, init, body, mode=None):
    cid = len(CASES) + 1
    for reg in init:
        assert reg in (0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11), \
            "init must not set a reserved register (r9/r12-r15)"
    items = body.resolve()
    entry = mode or (items[0]["mode"] if items else "arm")
    if entry == "arm":
        total = sum(it["size"] for it in items)
        assert total % 4 == 0, \
            "an ARM-entry fragment must be a multiple of 4 bytes (the ARM capture follows it unshifted)"
    CASES.append(dict(id=cid, name=name, group=group, desc=desc,
                      init=init, items=items, post=body.post, mode=entry))


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


# ===========================================================================
# A32 case set (unchanged; ids 1..N are stable, so existing goldens still match)
# ===========================================================================
def gen_dp_reg():
    for op in range(16):
        for k, (a, b) in enumerate(operand_pairs(6)):
            bd = Body()
            s = 1 if op in TEST_OPS else 0
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
        bd.w(dp_reg(OP["tst"], 0, 2, 2, s=1, st=0, si=1),
             "tst r2, r2, LSL #1")
        bd.w(dp_reg(OP["mov"], 0, 0, 1, st=3, si=0), "mov r0, r1, RRX")
        val = 0x80000000 if cbit else 0x40000000
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
    for k in range(3):
        bd = Body()
        bd.w(mrs(0), "mrs r0, cpsr")
        bd.w(dp_reg(OP["mov"], 0, 0, 0, st=1, si=28), "lsr r0, r0, #28")
        add("mrs_flag_%d" % k, "psr", "MRS r0,cpsr; LSR r0,#28", {}, bd)
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
        bd.w(dp_reg(OP["mov"], 1, 0, 9), "mov r1, r9")
        bd.w(dp_imm(OP["add"], 1, 1, 64), "add r1, r1, #64")
        bd.w(mem_w(1, 0, 0, 1, off, p=1, u=0), "ldr r0, [r1, #-%d]" % off)
        bd.w(dp_imm(OP["mov"], 1, 0, 0), "mov r1, #0")
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
    for (mn, p, u) in modes:
        bd = Body()
        bd.w(dp_reg(OP["mov"], 1, 0, 9), "mov r1, r9")
        bd.w(blk(0, 9, (1 << 2) | (1 << 3), p, u, w=1), "stm%s r9!, {r2, r3}" % mn)
        bd.w(dp_reg(OP["sub"], 0, 9, 1), "sub r0, r9, r1")
        bd.w(dp_imm(OP["mov"], 1, 0, 0), "mov r1, #0")
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
    bd.w(dp_imm(OP["mov"], 2, 0, 0), "mov r2, #0")
    bd.b("done")
    bd.label("fn")
    bd.w(dp_imm(OP["mov"], 0, 0, 0x66), "mov r0, #0x66")
    bd.w(bx(14), "bx lr")
    bd.label("done")
    add("blx_reg", "branch", "BLX register to an ARM label", {}, bd)

    for cn, cc in COND.items():
        if cn in ("al", "hs", "lo"):
            continue
        bd = Body()
        bd.w(dp_imm(OP["cmp"], 0, 1, 0, s=1), "cmp r1, #0")
        bd.b("taken", cc)
        bd.w(dp_imm(OP["mov"], 0, 0, 0x22), "mov r0, #0x22")
        bd.b("done")
        bd.label("taken")
        bd.w(dp_imm(OP["mov"], 0, 0, 0x11), "mov r0, #0x11")
        bd.label("done")
        add("cond_b_%s" % cn, "branch", "B%s after CMP r1,#0" % cn, {1: 0}, bd)


def gen_cond():
    for cn, cc in COND.items():
        if cn in ("al", "hs", "lo"):
            continue
        bd = Body()
        bd.w(dp_imm(OP["mov"], 1, 0, 0), "mov r1, #0")
        bd.w(dp_imm(OP["cmp"], 0, 1, 0, s=1), "cmp r1, #0")
        bd.w(dp_imm(OP["mov"], 0, 0, 0x11, cond=cc), "mov%s r0, #0x11" % cn)
        bd.w(dp_imm(OP["mov"], 2, 0, 0x22), "mov r2, #0x22")
        add("cond_mov_%s" % cn, "cond", "MOV%s r0,#0x11 after CMP r1,#0" % cn, {1: 0}, bd)
    for cn, cc in (("cs", COND["cs"]), ("cc", COND["cc"])):
        for cin in (0, 1):
            bd = Body()
            bd.w(dp_imm(OP["mov"], 1, 0, 1), "mov r1, #1")
            bd.w(dp_reg(OP["sub"], 0, 1, 1, s=1), "subs r1, r1, r1")
            if cin == 0:
                bd.w(dp_imm(OP["sub"], 0, 1, 1, s=1), "subs r1, r1, #1")
            else:
                bd.w(dp_imm(OP["add"], 0, 1, 0, s=1), "adds r1, r1, #0")
            bd.w(dp_imm(OP["mov"], 0, 0, 0x33, cond=cc), "mov%s r0, #0x33" % cn)
            add("cond_dp_%s_%d" % (cn, cin), "cond",
                "conditional data-processing on C=%d" % cin, {}, bd)


def gen_mixed():
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


# ===========================================================================
# Thumb (T32) case set
# ===========================================================================
T_ALU = [("and", 0x0), ("eor", 0x1), ("lsl", 0x2), ("lsr", 0x3), ("asr", 0x4),
         ("adc", 0x5), ("sbc", 0x6), ("ror", 0x7), ("tst", 0x8), ("neg", 0x9),
         ("cmp", 0xA), ("cmn", 0xB), ("orr", 0xC), ("mul", 0xD), ("bic", 0xE),
         ("mvn", 0xF)]


def gen_t_shift_imm():
    for mn, mk in (("lsl", t_lsl_i), ("lsr", t_lsr_i), ("asr", t_asr_i)):
        for si in (0, 1, 5, 31):
            bd = Body()
            bd.t(mk(0, 1, si), "%s r0, r1, #%d" % (mn, si))
            add("t_shift_%s_%d" % (mn, si), "thumb-shift",
                "T %s r0,r1,#%d" % (mn.upper(), si), {1: 0x80000001}, bd)


def gen_t_alu():
    for mn, op in T_ALU:
        for k, (a, b) in enumerate(operand_pairs(4)):
            bd = Body()
            bd.t(t_alu(op, 0, 1), "%s r0, r1" % mn)
            add("t_alu_%s_%d" % (mn, k), "thumb-alu",
                "T %s r0,r1" % mn.upper(), {0: a, 1: b}, bd)


def gen_t_imm8():
    for mn, mk in (("movs", t_mov8), ("cmp", t_cmp8), ("adds", t_add8), ("subs", t_sub8)):
        for k, iv in enumerate((0x00, 0x01, 0x7F, 0xFF)):
            bd = Body()
            bd.t(mk(0, iv), "%s r0, #0x%x" % (mn, iv))
            add("t_%s_imm8_%d" % (mn, k), "thumb-imm8",
                "T %s r0,#0x%x" % (mn, iv), {0: 0xDEADBEEF}, bd)


def gen_t_addsub():
    for k, (a, b) in enumerate(operand_pairs(3)):
        bd = Body(); bd.t(t_add3(0, 1, 2), "adds r0, r1, r2")
        add("t_adds_reg_%d" % k, "thumb-addsub", "T ADDS r0,r1,r2", {1: a, 2: b}, bd)
        bd = Body(); bd.t(t_sub3(0, 1, 2), "subs r0, r1, r2")
        add("t_subs_reg_%d" % k, "thumb-addsub", "T SUBS r0,r1,r2", {1: a, 2: b}, bd)
    for k, iv in enumerate((0, 1, 3, 7)):
        bd = Body(); bd.t(t_add_i3(0, 1, iv), "adds r0, r1, #%d" % iv)
        add("t_adds_i3_%d" % k, "thumb-addsub", "T ADDS r0,r1,#%d" % iv, {1: 0xFFFFFFF0}, bd)
        bd = Body(); bd.t(t_sub_i3(0, 1, iv), "subs r0, r1, #%d" % iv)
        add("t_subs_i3_%d" % k, "thumb-addsub", "T SUBS r0,r1,#%d" % iv, {1: 5}, bd)


def gen_t_hi():
    for k, (a, b) in enumerate(operand_pairs(3)):
        bd = Body(); bd.t(t_hi(0, 0, 8), "add r0, r8")
        add("t_add_hi_%d" % k, "thumb-hi", "T ADD r0,r8", {8: a}, bd)
        bd = Body(); bd.t(t_hi(2, 11, 2), "mov r11, r2")
        add("t_mov_hi_%d" % k, "thumb-hi", "T MOV r11,r2", {2: a}, bd)
        bd = Body()
        bd.t(t_hi(1, 8, 1), "cmp r8, r1")
        bd.t(t_mov8(0, 0x11), "movs r0, #0x11")
        add("t_cmp_hi_%d" % k, "thumb-hi", "T CMP r8,r1 then MOVS (flags)", {8: a, 1: b}, bd)
    # BLX register within Thumb to an odd (Thumb) target, returning via BX lr.
    for k in range(2):
        bd = Body()
        bd.t(t_mov8(0, 0x00), "movs r0, #0")
        bd.fixups.append((bd.t(0, "adr r3, tgt"), "tadr", "tgt", 3))
        bd.t(t_add8(3, 1), "adds r3, #1")
        bd.t(t_bx(3, link=1), "blx r3")
        bd.t(t_mov8(1, 0x55), "movs r1, #0x55")
        bd.tb("done")
        bd.label("tgt")
        bd.t(t_mov8(2, 0x66), "movs r2, #0x66")
        bd.t(t_bx(14), "bx lr")
        bd.label("done")
        add("t_blx_reg_%d" % k, "thumb-hi", "T BLX register (odd, stays Thumb) + BX lr", {}, bd)


def gen_t_ldst_reg():
    opnames = [(0, "str"), (1, "strh"), (2, "strb"), (3, "ldrsb"),
               (4, "ldr"), (5, "ldrh"), (6, "ldrb"), (7, "ldrsh")]
    for op, mn in opnames:
        for k, rm in enumerate((2, 4, 6)):
            bd = Body()
            bd.t(t_hi(2, 7, 9), "mov r7, r9")                       # r7 = window base
            bd.t(t_word_i5(0, 16 >> 2, 7, 1), "str r1, [r7, #16]")  # seed a known word
            bd.t(t_ldst_reg(op, rm, 7, 0), "%s r0, [r7, r%d]" % (mn, rm))
            bd.t(t_mov8(7, 0), "movs r7, #0")                       # never leave an address in a compared reg
            add("t_%s_reg_%d" % (mn, k), "thumb-ldst",
                "T %s r0,[r7,r%d]" % (mn.upper(), rm), {rm: 16, 1: 0xC0DE1234}, bd)


def gen_t_ldst_imm():
    for k, off in enumerate((0, 4, 20, 60)):
        bd = Body()
        bd.t(t_hi(2, 7, 9), "mov r7, r9")
        bd.t(t_word_i5(0, off >> 2, 7, 1), "str r1, [r7, #%d]" % off)
        bd.t(t_word_i5(1, off >> 2, 7, 0), "ldr r0, [r7, #%d]" % off)
        bd.t(t_mov8(7, 0), "movs r7, #0")
        add("t_str_ldr_imm_%d" % k, "thumb-ldst", "T STR/LDR imm5", {1: 0x1234ABCD}, bd)
    for k, off in enumerate((0, 2, 30)):
        bd = Body()
        bd.t(t_hi(2, 7, 9), "mov r7, r9")
        bd.t(t_half_i5(0, off >> 1, 7, 1), "strh r1, [r7, #%d]" % off)
        bd.t(t_half_i5(1, off >> 1, 7, 0), "ldrh r0, [r7, #%d]" % off)
        bd.t(t_mov8(7, 0), "movs r7, #0")
        add("t_strh_ldrh_%d" % k, "thumb-ldst", "T STRH/LDRH imm5", {1: 0xBEEF}, bd)
    for k, off in enumerate((0, 1, 7)):
        bd = Body()
        bd.t(t_hi(2, 7, 9), "mov r7, r9")
        bd.t(t_byte_i5(0, off, 7, 1), "strb r1, [r7, #%d]" % off)
        bd.t(t_byte_i5(1, off, 7, 0), "ldrb r0, [r7, #%d]" % off)
        bd.t(t_mov8(7, 0), "movs r7, #0")
        add("t_strb_ldrb_%d" % k, "thumb-ldst", "T STRB/LDRB imm5", {1: 0x5A}, bd)


def gen_t_ldr_lit():
    # LDR literal reads a word placed after the fragment (so the interpreter
    # never executes the literal as an instruction): base = Align(pc+4,4).
    for k, imm8 in enumerate((1, 2, 3)):
        bd = Body()
        bd.t(t_ldr_lit(0, imm8), "ldr r0, [pc, #%d]" % (imm8 * 4))
        # pad the fragment to exactly imm8*4 + 4 - 4 = imm8*4 bytes so the
        # literal lands at Align(4,4) + imm8*4 = 4 + imm8*4.
        while _bytes_len(bd) < imm8 * 4:
            bd.t(t_nop(), "nop")
        bd.post.append(0x12345678 + k)
        add("t_ldr_lit_%d" % k, "thumb-ldst",
            "T LDR literal reads a word after the fragment", {}, bd)


def gen_t_sp():
    for k, off in enumerate((0, 4, 16, 60)):
        bd = Body()
        bd.t(t_hi(2, 13, 9), "mov sp, r9")
        bd.t(t_add_sp(64 >> 2), "add sp, #64")
        bd.t(t_sp_ldst(0, 1, off >> 2), "str r1, [sp, #%d]" % off)
        bd.t(t_sp_ldst(1, 0, off >> 2), "ldr r0, [sp, #%d]" % off)
        add("t_sp_ldst_%d" % k, "thumb-sp", "T STR/LDR [sp,#imm8*4]", {1: 0x0BADF00D}, bd)


def gen_t_add_rdsp():
    for k, off in enumerate((0, 4, 16, 60)):
        bd = Body()
        bd.t(t_hi(2, 13, 9), "mov sp, r9")
        bd.t(t_add_rdsp(0, 0), "add r0, sp, #0")
        bd.t(t_add_rdsp(1, (off >> 2) + 2), "add r1, sp, #%d" % (off + 8))
        bd.t(t_sub3(0, 1, 0), "subs r0, r1, r0")   # the architectural delta (off+8)
        add("t_add_rdsp_%d" % k, "thumb-adr", "T ADD rd,sp,#imm8*4 (delta recorded)", {}, bd)


def gen_t_adr():
    for k in range(2):
        bd = Body()
        bd.fixups.append((bd.t(0, "adr r0, a"), "tadr", "a", 0))
        bd.fixups.append((bd.t(0, "adr r1, b"), "tadr", "b", 1))
        bd.t(t_sub3(0, 1, 0), "subs r0, r1, r0")   # r0 = b - a (architectural)
        bd.tb("done")
        bd.label("a")
        bd.t(t_nop(), "nop")
        bd.t(t_nop(), "nop")
        bd.label("b")
        bd.t(t_nop(), "nop")
        bd.label("done")
        add("t_adr_%d" % k, "thumb-adr", "T ADR of two labels; the difference", {}, bd)


def gen_t_pushpop():
    for k, lst in enumerate((0x03, 0x0F, 0x81)):
        bd = Body()
        bd.t(t_hi(2, 13, 9), "mov sp, r9")
        bd.t(t_add_sp(64 >> 2), "add sp, #64")
        bd.t(t_push(lst, 0), "push {list}")
        bd.t(t_pop(lst, 0), "pop {list}")
        add("t_pushpop_%d" % k, "thumb-pushpop", "T PUSH/POP r-list", {0: 0x11111111, 1: 0x22222222, 7: 0x77777777}, bd)
    for k in range(2):
        bd = Body()
        bd.t(t_hi(2, 13, 9), "mov sp, r9")
        bd.t(t_add_sp(64 >> 2), "add sp, #64")
        bd.t(t_push(0x01, 1), "push {r0, lr}")   # lr = 0
        bd.t(t_pop(0x02, 0), "pop {r1}")          # r1 = r0 ; lr slot popped into r2? no
        add("t_push_lr_%d" % k, "thumb-pushpop", "T PUSH {r0,lr}", {0: 0xCAFEBABE}, bd)
    for k in range(2):
        bd = Body()
        bd.t(t_hi(2, 13, 9), "mov sp, r9")
        bd.t(t_add_sp(64 >> 2), "add sp, #64")
        bd.t(t_push(0x01, 1), "push {r0, lr}")
        bd.t(t_pop(0x03, 0), "pop {r0, r1}")      # r0..r1 = r0, lr(=0)
        add("t_pushpop_lr_%d" % k, "thumb-pushpop", "T PUSH/POP with LR", {0: 0xFEEDFACE}, bd)


def gen_t_ldmstm():
    for k, lst in enumerate((0x0C, 0x06)):
        bd = Body()
        bd.t(t_hi(2, 7, 9), "mov r7, r9")
        bd.t(t_stmia(7, lst), "stmia r7!, {list}")
        bd.t(t_ldmia(7, lst), "ldmia r7, {list}")
        bd.t(t_mov8(7, 0), "movs r7, #0")
        add("t_ldmstm_%d" % k, "thumb-block", "T STMIA/LDMIA",
            {0: 1, 1: 2, 2: 3, 3: 4, 4: 5, 5: 6}, bd)
    for k in range(2):
        bd = Body()
        bd.t(t_hi(2, 7, 9), "mov r7, r9")
        bd.t(t_stmia(7, 0x0C), "stmia r7!, {r2,r3}")
        bd.t(t_mov8(7, 0), "movs r7, #0")
        add("t_stmia_%d" % k, "thumb-block", "T STMIA r7!,{r2,r3}", {2: 0xAAAA5555, 3: 0xBBBB6666}, bd)


def gen_t_branch():
    bd = Body()
    bd.t(t_mov8(0, 0), "movs r0, #0")
    bd.tb("skip")
    bd.t(t_mov8(0, 0x7F), "movs r0, #0x7F")
    bd.label("skip")
    bd.t(t_mov8(1, 0x2A), "movs r1, #0x2A")
    add("t_b_forward", "thumb-branch", "T B forward skips a write", {}, bd)

    bd = Body()
    bd.t(t_mov8(0, 0), "movs r0, #0")
    bd.label("top")
    bd.t(t_add8(0, 1), "adds r0, #1")
    bd.t(t_cmp8(0, 4), "cmp r0, #4")
    bd.tb("top", COND["lt"])
    add("t_b_backward", "thumb-branch", "T B backward loop to r0=4", {}, bd)

    for cn, cc in COND.items():
        if cn in ("al", "hs", "lo"):
            continue
        bd = Body()
        bd.t(t_cmp8(1, 0), "cmp r1, #0")
        bd.tb("taken", cc)
        bd.t(t_mov8(0, 0x22), "movs r0, #0x22")
        bd.tb("done")
        bd.label("taken")
        bd.t(t_mov8(0, 0x11), "movs r0, #0x11")
        bd.label("done")
        add("t_cond_b_%s" % cn, "thumb-branch", "T B%s after CMP r1,#0" % cn, {1: 0}, bd)


def gen_t_cbz():
    for k, v in enumerate((0, 1, 0x100, 0xFFFFFFFF)):
        bd = Body()
        bd.fixups.append((bd.t(0, "cbz r1, skip"), "tcbz", "skip", 1))
        bd.t(t_mov8(0, 0x11), "movs r0, #0x11")
        bd.tb("done")
        bd.label("skip")
        bd.t(t_mov8(0, 0x22), "movs r0, #0x22")
        bd.label("done")
        add("t_cbz_%d" % k, "thumb-cbz", "T CBZ r1", {1: v}, bd)
    for k, v in enumerate((0, 5)):
        bd = Body()
        bd.fixups.append((bd.t(0, "cbnz r1, skip"), "tcbnz", "skip", 1))
        bd.t(t_mov8(0, 0x33), "movs r0, #0x33")
        bd.tb("done")
        bd.label("skip")
        bd.t(t_mov8(0, 0x44), "movs r0, #0x44")
        bd.label("done")
        add("t_cbnz_%d" % k, "thumb-cbz", "T CBNZ r1", {1: v}, bd)


def gen_t_bl():
    bd = Body()
    bd.t(t_mov8(0, 0), "movs r0, #0")
    bd.tbl("sub1")
    bd.t(t_mov8(3, 0x99), "movs r3, #0x99")
    bd.tb("end")
    bd.label("sub1")
    bd.t(t_mov8(0, 0x11), "movs r0, #0x11")
    bd.t(t_bx(14), "bx lr")
    bd.label("end")
    add("t_bl_bx", "thumb-branch", "T BL then BX lr", {}, bd)


def gen_t_it():
    # IT with one instruction, both outcomes
    for k, v in enumerate((0, 1)):
        bd = Body()
        bd.t(t_mov8(1, v), "movs r1, #v")
        bd.t(t_cmp8(1, 0), "cmp r1, #0")
        bd.t(t_it(COND["eq"], it_mask(COND["eq"], ["T"])), "it eq")
        bd.t(t_mov8(0, 0x11), "moveq r0, #0x11")
        bd.t(t_mov8(2, 0x22), "movs r2, #0x22")
        add("t_it_%d" % k, "thumb-it", "T IT EQ (r1=%d)" % v, {}, bd)
    # ITE: then/else
    for k, v in enumerate((0, 1)):
        bd = Body()
        bd.t(t_mov8(1, v), "movs r1, #v")
        bd.t(t_cmp8(1, 0), "cmp r1, #0")
        bd.t(t_it(COND["eq"], it_mask(COND["eq"], ["T", "E"])), "ite eq")
        bd.t(t_mov8(0, 0xAA), "moveq r0, #0xAA")
        bd.t(t_mov8(0, 0xBB), "movne r0, #0xBB")
        add("t_ite_%d" % k, "thumb-it", "T ITE EQ (r1=%d)" % v, {}, bd)
    # ITT: two conditional instructions
    for k, v in enumerate((0, 1)):
        bd = Body()
        bd.t(t_mov8(1, v), "movs r1, #v")
        bd.t(t_cmp8(1, 0), "cmp r1, #0")
        bd.t(t_it(COND["eq"], it_mask(COND["eq"], ["T", "T"])), "itt eq")
        bd.t(t_mov8(0, 0x11), "moveq r0, #0x11")
        bd.t(t_mov8(2, 0x22), "moveq r2, #0x22")
        add("t_itt_%d" % k, "thumb-it", "T ITT EQ (r1=%d)" % v, {}, bd)
    # SIMD-ish four-instruction ITEEE
    for k, v in enumerate((0, 1)):
        bd = Body()
        bd.t(t_mov8(1, v), "movs r1, #v")
        bd.t(t_cmp8(1, 0), "cmp r1, #0")
        bd.t(t_it(COND["ne"], it_mask(COND["ne"], ["T", "E", "E", "E"])), "iteee ne")
        bd.t(t_mov8(0, 0x01), "movne r0, #1")
        bd.t(t_mov8(2, 0x02), "moveq r2, #2")
        bd.t(t_mov8(3, 0x03), "moveq r3, #3")
        bd.t(t_mov8(4, 0x04), "moveq r4, #4")
        add("t_iteee_%d" % k, "thumb-it", "T ITEEE NE (r1=%d)" % v, {}, bd)
    # IT with a flag-setting instruction inside (condition-carrying)
    for cn in ("eq", "ne", "cc", "cs"):
        bd = Body()
        bd.t(t_mov8(1, 0x10), "movs r1, #0x10")
        bd.t(t_cmp8(1, 0x10), "cmp r1, #0x10")   # Z=1, C=1
        bd.t(t_it(COND[cn], it_mask(COND[cn], ["T"])), "it %s" % cn)
        bd.t(t_mov8(0, 0x55), "mov%s r0, #0x55" % cn)
        bd.t(t_mov8(2, 0x66), "movs r2, #0x66")
        add("t_it_flag_%s" % cn, "thumb-it", "T IT %s after CMP equal" % cn, {}, bd)


def gen_t_w32_dp_reg():
    ops = [("and", 0x0), ("bic", 0x1), ("orr", 0x2), ("orn", 0x3), ("eor", 0x4),
           ("add", 0x8), ("adc", 0xA), ("sbc", 0xB), ("sub", 0xD), ("rsb", 0xE)]
    for mn, op in ops:
        for k, (a, b) in enumerate(operand_pairs(3)):
            for sh, amt in (("", 0), ("lsl", 2)):
                if amt:
                    h2 = ((amt >> 2) << 12) | (0 << 8) | ((amt & 3) << 6) | (0 << 4) | 2
                else:
                    h2 = (0 << 12) | (0 << 8) | (0 << 6) | (0 << 4) | 2
                h1 = 0xEA00 | (op << 5) | (0 << 4) | 1
                if op & 0x8:
                    h1 = 0xEA00 | (op << 5) | 1   # EB* is just EA* with bit8 set via op
                bd = Body()
                bd.t2(0xEA00 | (op << 5) | 1, h2, "%s.w r0, r1, r2%s" % (mn, (" lsl #%d" % amt if amt else "")))
                add("t32_dp_%s_%d_%d" % (mn, k, amt), "thumb2-dp",
                    "T32 %s.w r0,r1,r2%s" % (mn.upper(), (" lsl #%d" % amt if amt else "")),
                    {1: a, 2: b}, bd)
    # MOV.W (register) and MVN.W
    for k, a in enumerate(EDGE[:6]):
        bd = Body(); bd.t2(0xEA4F, (0 << 12) | (0 << 8) | 2, "mov.w r0, r2")
        add("t32_mov_%d" % k, "thumb2-dp", "T32 MOV.W r0,r2", {2: a}, bd)
        bd = Body(); bd.t2(0xEA6F, (0 << 12) | (0 << 8) | 2, "mvn.w r0, r2")
        add("t32_mvn_%d" % k, "thumb2-dp", "T32 MVN.W r0,r2", {2: a}, bd)


def gen_t_w32_dp_imm():
    # modified-immediate ops (op field), S=0
    ops = [("and", 0x0), ("bic", 0x1), ("orr", 0x2), ("orn", 0x3), ("eor", 0x4),
           ("add", 0x8), ("adc", 0xA), ("sbc", 0xB), ("sub", 0xD), ("rsb", 0xE)]

    def exp_imm(v):
        # find i:imm3:imm8 s.t. ThumbExpandImm == v
        for imm12 in range(0x1000):
            if thumb_expand_imm_py(imm12) == (v & 0xFFFFFFFF):
                return imm12
        return None

    imms = [0x01, 0xFF, 0x100, 0x80000000 & 0xFFFFFFFF, 0xFF00, 0x0F]
    imms = [v for v in imms if exp_imm(v) is not None]
    for mn, op in ops:
        for k, v in enumerate(imms):
            imm12 = exp_imm(v)
            i = (imm12 >> 11) & 1
            imm3 = (imm12 >> 8) & 7
            imm8 = imm12 & 0xFF
            h1 = 0xF000 | (i << 10) | (op << 5) | 0 | 1
            h2 = (imm3 << 12) | (0 << 8) | imm8
            bd = Body(); bd.t2(h1, h2, "%s.w r0, r1, #0x%x" % (mn, v))
            add("t32_immi_%s_%d" % (mn, k), "thumb2-imm",
                "T32 %s.w r0,r1,#0x%x" % (mn.upper(), v), {1: 0xDEADBEEF}, bd)
    # MOVW / MOVT
    for k, v in enumerate((0x1234, 0xBEEF, 0x0000, 0xFFFF)):
        imm4 = (v >> 12) & 0xF; i = (v >> 11) & 1
        imm3 = (v >> 8) & 7; imm8 = v & 0xFF
        bd = Body(); bd.t2(0xF240 | (i << 10) | imm4, (imm3 << 12) | (0 << 8) | imm8,
                           "movw r0, #0x%x" % v)
        add("t32_movw_%d" % k, "thumb2-imm", "T32 MOVW r0,#0x%x" % v, {}, bd)
        bd = Body()
        bd.t(t_mov8(0, 0xAA), "movs r0, #0xAA")
        bd.t2(0xF2C0 | (i << 10) | imm4, (imm3 << 12) | (0 << 8) | imm8,
              "movt r0, #0x%x" % v)
        add("t32_movt_%d" % k, "thumb2-imm", "T32 MOVT r0,#0x%x" % v, {}, bd)
    # ADDW / SUBW
    for k, v in enumerate((0x123, 0xFFF, 0x010)):
        i = (v >> 11) & 1; imm3 = (v >> 8) & 7; imm8 = v & 0xFF
        h2 = (imm3 << 12) | (0 << 8) | imm8
        bd = Body(); bd.t2(0xF200 | (i << 10) | (0 << 5) | 1, h2, "addw r0, r1, #0x%x" % v)
        add("t32_addw_%d" % k, "thumb2-imm", "T32 ADDW r0,r1,#0x%x" % v, {1: 1}, bd)
        bd = Body(); bd.t2(0xF200 | (i << 10) | (5 << 5) | 1, h2, "subw r0, r1, #0x%x" % v)
        add("t32_subw_%d" % k, "thumb2-imm", "T32 SUBW r0,r1,#0x%x" % v, {1: 0xFFFF}, bd)


def gen_t_w32_ldst():
    for k, off in enumerate((0, 4, 0x40, 0x100)):
        bd = Body(); bd.t2(0xF8C0 | 9, (0 << 12) | off, "str.w r0, [r9, #%d]" % off)
        bd.t2(0xF8D0 | 9, (1 << 12) | off, "ldr.w r1, [r9, #%d]" % off)
        add("t32_ldr_str_imm_%d" % k, "thumb2-ldst", "T32 LDR/STR imm12", {0: 0x13572468}, bd)
    for k, off in enumerate((4, 0x40)):
        bd = Body(); bd.t2(0xF840 | 9, (0 << 12) | 0xC00 | off, "str.w r0, [r9, #-%d]" % off)
        bd.t2(0xF850 | 9, (1 << 12) | 0xC00 | off, "ldr.w r1, [r9, #-%d]" % off)
        bd.t(t_hi(2, 13, 9), "mov sp, r9")
        bd.t(t_add_sp(64 >> 2), "add sp, #64")
        add("t32_ldr_str_neg_%d" % k, "thumb2-ldst", "T32 LDR/STR negative imm8", {}, bd)
    for k, rm in enumerate((2, 3)):
        bd = Body(); bd.t2(0xF840 | 9, (0 << 12) | 0 | rm, "str.w r0, [r9, r%d]" % rm)
        bd.t2(0xF850 | 9, (1 << 12) | 0 | rm, "ldr.w r1, [r9, r%d]" % rm)
        add("t32_ldr_str_reg_%d" % k, "thumb2-ldst", "T32 LDR/STR register offset", {0: 0x2468ACE0, rm: 8}, bd)
    for k, off in enumerate((0, 4, 20)):
        bd = Body(); bd.t2(0xF880 | 9, (0 << 12) | off, "strb.w r0, [r9, #%d]" % off)
        bd.t2(0xF890 | 9, (1 << 12) | off, "ldrb.w r1, [r9, #%d]" % off)
        add("t32_strb_ldrb_%d" % k, "thumb2-ldst", "T32 STRB/LDRB imm12", {0: 0x9A}, bd)
    for k, off in enumerate((0, 2, 10)):
        bd = Body(); bd.t2(0xF8A0 | 9, (0 << 12) | off, "strh.w r0, [r9, #%d]" % off)
        bd.t2(0xF8B0 | 9, (1 << 12) | off, "ldrh.w r1, [r9, #%d]" % off)
        add("t32_strh_ldrh_%d" % k, "thumb2-ldst", "T32 STRH/LDRH imm12", {0: 0xCAFEBABE}, bd)
    for k, off in enumerate((0, 2)):
        bd = Body(); bd.t2(0xF8A0 | 9, (0 << 12) | off, "strh.w r0, [r9, #%d]" % off)
        bd.t2(0xF990 | 9, (1 << 12) | off, "ldrsb.w r1, [r9, #%d]" % off)
        add("t32_ldrsb_%d" % k, "thumb2-ldst", "T32 LDRSB imm12", {0: 0x8001}, bd)
    for k, off in enumerate((0, 4)):
        bd = Body(); bd.t2(0xF8A0 | 9, (0 << 12) | off, "strh.w r0, [r9, #%d]" % off)
        bd.t2(0xF9B0 | 9, (1 << 12) | off, "ldrsh.w r1, [r9, #%d]" % off)
        add("t32_ldrsh_%d" % k, "thumb2-ldst", "T32 LDRSH imm12", {0: 0x8002}, bd)


def gen_t_w32_dual():
    for k, off in enumerate((0, 4, 8)):
        imm8 = off >> 2
        bd = Body()
        bd.t2(0xE9C0 | 9, (0 << 12) | (1 << 8) | imm8, "strd r0, r1, [r9, #%d]" % off)
        bd.t2(0xE9D0 | 9, (2 << 12) | (3 << 8) | imm8, "ldrd r2, r3, [r9, #%d]" % off)
        add("t32_strd_ldrd_%d" % k, "thumb2-dual", "T32 STRD/LDRD imm", {0: 0xA1A2A3A4, 1: 0xB1B2B3B4}, bd)
    bd = Body()
    bd.t2(0xE9C0 | 9, (0 << 12) | (1 << 8) | 0, "strd r0, r1, [r9]")
    bd.t2(0xE9F0 | 9, (2 << 12) | (3 << 8) | 1, "ldrd r2, r3, [r9, #4]!")
    add("t32_ldrd_wb", "thumb2-dual", "T32 LDRD pre-indexed writeback", {0: 0x11112222, 1: 0x33334444}, bd)
    for k, off in enumerate((2, 4)):
        imm8 = off >> 2
        bd = Body()
        bd.t2(0xE9C0 | 9, (0 << 12) | (1 << 8) | 0, "strd r0, r1, [r9]")
        bd.t2(0xE950 | 9, (2 << 12) | (3 << 8) | imm8, "ldrd r2, r3, [r9, #-%d]" % off)
        add("t32_ldrd_neg_%d" % k, "thumb2-dual", "T32 LDRD negative imm", {0: 0x55556666, 1: 0x77778888}, bd)


def gen_t_w32_table():
    # TBB/TBH: the table lives in the scratch window; the branch target is a label.
    for k in range(2):
        bd = Body()
        bd.t(t_hi(2, 7, 9), "mov r7, r9")
        bd.t(t_byte_i5(0, 0, 7, 3), "strb r3, [r7, #0]")        # table[0] = r3 (byte)
        bd.t(t_mov8(2, 0), "movs r2, #0")
        bd.t2(0xE8D0 | 7, 0xF000 | 2, "tbb [r7, r2]")
        bd.t(t_mov8(0, 0x11), "movs r0, #0x11")
        bd.tb("done")
        bd.t(t_mov8(0, 0x22), "movs r0, #0x22")
        bd.t(t_mov8(7, 0), "movs r7, #0")
        bd.label("done")
        add("t32_tbb_%d" % k, "thumb2-table", "T32 TBB", {3: 2}, bd)
    for k in range(2):
        bd = Body()
        bd.t(t_hi(2, 7, 9), "mov r7, r9")
        bd.t(t_half_i5(0, 0, 7, 3), "strh r3, [r7, #0]")          # halfword table entry
        bd.t(t_mov8(2, 0), "movs r2, #0")
        bd.t2(0xE8D0 | 7, 0xF010 | 2, "tbh [r7, r2, lsl #1]")
        bd.t(t_mov8(0, 0x33), "movs r0, #0x33")
        bd.tb("done")
        bd.t(t_mov8(0, 0x44), "movs r0, #0x44")
        bd.t(t_mov8(7, 0), "movs r7, #0")
        bd.label("done")
        add("t32_tbh_%d" % k, "thumb2-table", "T32 TBH", {3: 2}, bd)


def gen_t_w32_extend():
    tos = [("sxtb", 0xFA40), ("sxth", 0xFA00), ("uxtb", 0xFA50), ("uxth", 0xFA10)]
    for mn, base in tos:
        for k, v in enumerate((0x80, 0x8001, 0xFFFFFFFF, 0x7F)):
            bd = Body()
            bd.t2(base | 0xF, 0xF000 | (0 << 8) | (0x8 << 4) | 1, "%s r0, r1" % mn)
            add("t32_%s_%d" % (mn, k), "thumb2-extend", "T32 %s r0,r1" % mn.upper(), {1: v}, bd)
    revs = [("rev", 0x8), ("rev16", 0x9), ("revsh", 0xB)]
    for mn, op in revs:
        for k, v in enumerate((0x12345678, 0x0000FF00, 0x8001)):
            bd = Body()
            bd.t2(0xFA90 | 1, 0xF000 | (0 << 8) | (op << 4) | 1, "%s r0, r1" % mn)
            add("t32_%s_%d" % (mn, k), "thumb2-extend", "T32 %s r0,r1" % mn.upper(), {1: v}, bd)
    for k, v in enumerate((0x00000000, 0x00000001, 0x7FFFFFFF, 0xFFFFFFFF)):
        bd = Body()
        bd.t2(0xFAB0 | 1, 0xF000 | (0 << 8) | (0x8 << 4) | 1, "clz r0, r1")
        add("t32_clz_%d" % k, "thumb2-misc", "T32 CLZ r0,r1", {1: v}, bd)


def gen_t_w32_bitfield():
    # UBFX / SBFX: lsb = imm3:imm2, widthm1 = bits[4:0]
    def ubfx_h2(lsb, widthm1): return (((lsb >> 2) & 7) << 12) | (0 << 8) | (((lsb & 3) << 6)) | widthm1
    for k, (lsb, width) in enumerate(((0, 4), (4, 4), (8, 8), (0, 16))):
        wm1 = width - 1
        bd = Body()
        bd.t2(0xF3C0 | 1, ubfx_h2(lsb, wm1), "ubfx r0, r1, #%d, #%d" % (lsb, width))
        add("t32_ubfx_%d" % k, "thumb2-bitfield", "T32 UBFX r0,r1,#%d,#%d" % (lsb, width),
            {1: 0xDEADBEEF}, bd)
        bd = Body()
        bd.t2(0xF340 | 1, ubfx_h2(lsb, wm1), "sbfx r0, r1, #%d, #%d" % (lsb, width))
        add("t32_sbfx_%d" % k, "thumb2-bitfield", "T32 SBFX r0,r1,#%d,#%d" % (lsb, width),
            {1: 0xDEADBEEF}, bd)
    # BFI / BFC: lsb = imm3:imm2, msb = bits[4:0]
    def bfi_h2(lsb, msb): return (((lsb >> 2) & 7) << 12) | (0 << 8) | (((lsb & 3) << 6)) | msb
    for k, (lsb, width) in enumerate(((0, 4), (4, 4), (8, 8))):
        msb = lsb + width - 1
        bd = Body()
        bd.t2(0xF360 | 1, bfi_h2(lsb, msb), "bfi r0, r1, #%d, #%d" % (lsb, width))
        add("t32_bfi_%d" % k, "thumb2-bitfield", "T32 BFI r0,r1,#%d,#%d" % (lsb, width),
            {0: 0xAAAAAAAA, 1: 0xFFFF}, bd)
        bd = Body()
        bd.t2(0xF36F, bfi_h2(lsb, msb), "bfc r0, #%d, #%d" % (lsb, width))
        add("t32_bfc_%d" % k, "thumb2-bitfield", "T32 BFC r0,#%d,#%d" % (lsb, width),
            {0: 0xAAAAAAAA}, bd)


def gen_t_w32_sat():
    def sat_h2(lsb, sat_imm): return (((lsb >> 2) & 7) << 12) | (0 << 8) | (((lsb & 3) << 6)) | sat_imm
    for k, (sat, lsb) in enumerate(((8, 0), (8, 4), (16, 0), (4, 0))):
        bd = Body()
        bd.t2(0xF300 | 1, sat_h2(lsb, sat - 1), "ssat r0, #%d, r1, lsl #%d" % (sat, lsb))
        add("t32_ssat_%d" % k, "thumb2-sat", "T32 SSAT r0,#%d,r1,lsl #%d" % (sat, lsb),
            {1: 0x00010002}, bd)
        bd = Body()
        bd.t2(0xF380 | 1, sat_h2(lsb, sat), "usat r0, #%d, r1, lsl #%d" % (sat, lsb))
        add("t32_usat_%d" % k, "thumb2-sat", "T32 USAT r0,#%d,r1,lsl #%d" % (sat, lsb),
            {1: 0xFFFFFFFF}, bd)


def gen_t_w32_psr():
    for k in range(3):
        bd = Body()
        bd.t2(0xF3EF, 0x8000, "mrs r0, cpsr")
        bd.t(t_mov8(1, 28), "movs r1, #28")
        bd.t(t_alu(0x3, 0, 1), "lsrs r0, r0, r1")   # r0 = the flag nibble
        add("t32_mrs_%d" % k, "thumb2-psr", "T32 MRS + LSR #28 (flag nibble)", {}, bd)
    for k, val in enumerate((0xF0000000, 0x00000000, 0x80000000)):
        bd = Body()
        if val:
            bd.t(t_mov8(1, (val >> 24) & 0xFF), "movs r1, #0x%x" % ((val >> 24) & 0xFF))
            bd.t(t_lsl_i(1, 1, 24), "lsls r1, r1, #24")
        else:
            bd.t(t_mov8(1, 0), "movs r1, #0")
        bd.t2(0xF380 | 1, 0x8800, "msr apsr_nzcvq, r1")
        bd.t2(0xF3EF, 0x8000, "mrs r0, cpsr")
        bd.t(t_mov8(2, 28), "movs r2, #28")
        bd.t(t_alu(0x3, 0, 2), "lsrs r0, r0, r2")
        add("t32_msr_%d" % k, "thumb2-psr", "T32 MSR apsr_nzcvq + MRS", {}, bd)


def gen_t_w32_bl():
    bd = Body()
    bd.t(t_mov8(0, 0), "movs r0, #0")
    bd.tbl("sub1")
    bd.t(t_mov8(3, 0x99), "movs r3, #0x99")
    bd.tb("end")
    bd.label("sub1")
    bd.t(t_mov8(0, 0x11), "movs r0, #0x11")
    bd.t(t_bx(14), "bx lr")
    bd.label("end")
    add("t32_bl_bx", "thumb2-branch", "T32 BL then BX lr", {}, bd)


def gen_t_mixed():
    for k in range(20):
        a = RNG.getrandbits(32)
        b = RNG.getrandbits(32)
        bd = Body()
        bd.t(t_hi(2, 7, 9), "mov r7, r9")
        bd.t(t_add3(0, 1, 2), "adds r0, r1, r2")
        bd.t(t_lsl_i(3, 0, (k % 7) + 1), "lsls r3, r0, #%d" % ((k % 7) + 1))
        bd.t(t_word_i5(0, (k % 4), 7, 3), "str r3, [r7, #%d]" % ((k % 4) * 4))
        bd.t(t_word_i5(1, (k % 4), 7, 4), "ldr r4, [r7, #%d]" % ((k % 4) * 4))
        bd.t(t_alu(0x1, 5, 4), "eors r5, r4")
        bd.t(t_mov8(7, 0), "movs r7, #0")
        add("t_mixed_%d" % k, "thumb-mixed", "T ADDS/LSLS/STR/LDR/EORS", {1: a, 2: b}, bd)


# --- interworking ---------------------------------------------------------
def gen_t_interworking():
    # Thumb -> ARM (BX to an even address) -> back to Thumb (BX lr, lr odd).
    for k in range(3):
        bd = Body()
        bd.t(t_mov8(0, 0x11), "movs r0, #0x11")                      # 0
        bd.fixups.append((bd.t(0, "adr r2, back"), "tadr", "back", 2))  # 2
        bd.t(t_add8(2, 1), "adds r2, #1")                            # 4
        bd.t(t_hi(2, 14, 2), "mov lr, r2")                           # 6
        bd.fixups.append((bd.t(0, "adr r3, arm"), "tadr", "arm", 3))  # 8
        bd.t(t_bx(3), "bx r3")                                       # 10 (even -> ARM)
        bd.t(t_nop(), "nop")                                        # 12 pad
        bd.t(t_nop(), "nop")                                        # 14 pad
        bd.label("arm")                                             # 16 (ARM)
        bd.w(dp_imm(OP["mov"], 1, 0, 0x22), "mov r1, #0x22")
        bd.w(bx(14), "bx lr")                                       # -> lr odd -> Thumb
        bd.label("back")                                            # 24 (Thumb)
        bd.t(t_mov8(2, 0x33), "movs r2, #0x33")
        add("t_iw_t2a_%d" % k, "interwork",
            "Thumb BX(even)->ARM->BX lr->Thumb (T bit tracked)", {}, bd)
    # ARM entry -> Thumb (BLX register to an odd address) -> back to ARM.
    for k in range(3):
        bd = Body()
        bd.adr(3, "thumb")                                          # 0 (ARM)
        bd.w(0xE2833001, "add r3, r3, #1")                          # 4
        bd.w(blx_reg(3), "blx r3")                                  # 8 -> Thumb
        bd.w(dp_imm(OP["mov"], 1, 0, 0x44), "mov r1, #0x44")        # 12 (ARM again)
        bd.b("end")                                                 # 16
        bd.w(0xE1A00000, "nop")                                     # 20 pad
        bd.label("thumb")                                           # 24 (Thumb)
        bd.t(t_mov8(0, 0x55), "movs r0, #0x55")
        bd.t(t_bx(14), "bx lr")                                     # -> ARM
        bd.label("end")
        add("t_iw_a2t_%d" % k, "interwork",
            "A32 BLX(register, odd)->Thumb->BX lr->ARM", {}, bd)
    # BLX immediate (32-bit Thumb) switches to ARM.
    for k in range(2):
        bd = Body()
        bd.t(t_mov8(0, 0x66), "movs r0, #0x66")                     # 0
        bd.fixups.append((bd.t2(0xF000, 0xE800, "blx armfn"), "tblx", "armfn", None))  # 2
        bd.t(t_mov8(1, 0x77), "movs r1, #0x77")                     # 6 (return point)
        bd.tb("done")                                               # 8
        bd.t(t_nop(), "nop")                                        # 10 pad
        bd.label("armfn")                                           # 12 (ARM)
        bd.w(dp_imm(OP["mov"], 2, 0, 0x88), "mov r2, #0x88")
        bd.w(bx(14), "bx lr")                                       # -> Thumb at 6
        bd.label("done")
        add("t_iw_blximm_%d" % k, "interwork", "T32 BLX immediate -> ARM", {}, bd)


def thumb_expand_imm_py(imm12):
    if (imm12 & 0xC00) == 0:
        b = imm12 & 0xFF
        k = (imm12 >> 8) & 3
        if k == 0:
            return b
        if k == 1:
            return b | (b << 16)
        if k == 2:
            return (b << 24) | (b << 8)
        return (b * 0x01010101) & 0xFFFFFFFF
    v = 0x80 | (imm12 & 0x7F)
    rot = (imm12 >> 7) & 0x1F
    if rot == 0:
        return v
    return ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF


def build():
    gen_dp_reg(); gen_dp_reg_s(); gen_dp_imm(); gen_dp_imm_s()
    gen_shift_imm(); gen_shift_reg(); gen_shift_operand(); gen_rrx()
    gen_mul(); gen_psr(); gen_clz()
    gen_mem_imm(); gen_mem_reg(); gen_mem_indexed(); gen_mem_double(); gen_swp()
    gen_block(); gen_branch(); gen_cond(); gen_mixed()
    # Thumb
    gen_t_shift_imm(); gen_t_alu(); gen_t_imm8(); gen_t_addsub(); gen_t_hi()
    gen_t_ldst_reg(); gen_t_ldst_imm(); gen_t_ldr_lit(); gen_t_sp()
    gen_t_add_rdsp(); gen_t_adr(); gen_t_pushpop(); gen_t_ldmstm()
    gen_t_branch(); gen_t_cbz(); gen_t_bl(); gen_t_it()
    gen_t_w32_dp_reg(); gen_t_w32_dp_imm(); gen_t_w32_ldst(); gen_t_w32_dual()
    gen_t_w32_table(); gen_t_w32_extend(); gen_t_w32_bitfield(); gen_t_w32_sat()
    gen_t_w32_psr(); gen_t_w32_bl(); gen_t_mixed(); gen_t_interworking()


HEADER = """\
/* GENERATED by tools/a32/gen_cases.py -- do not edit by hand.
 * Regenerate with: python3 tools/a32/gen_cases.py */
"""


def _movw_movt_lines(reg, value):
    out = ["\tmovw\tr%d, #0x%04x" % (reg, value & 0xFFFF)]
    if (value >> 16) & 0xFFFF:
        out.append("\tmovt\tr%d, #0x%04x" % (reg, (value >> 16) & 0xFFFF))
    return out


def emit_body_items(items, o, start_mode):
    """Emit the fragment items, switching `.arm`/`.thumb` at mode boundaries."""
    cur = start_mode
    for it in items:
        if it["mode"] != cur:
            if it["mode"] == "thumb":
                o += ["\t.thumb", "\t.p2align 1"]
            else:
                o += ["\t.arm", "\t.p2align 2"]
            cur = it["mode"]
        v = it["val"]
        if it["size"] == 2:
            o.append("\t.hword\t0x%04x\t/* %s */" % (v & 0xFFFF, it["text"]))
        else:
            o.append("\t.word\t0x%08x\t/* %s */" % (v & 0xFFFFFFFF, it["text"]))
    return cur


def emit_s():
    o = [HEADER, "\t.syntax unified", "\t.arm", "\t.text", ""]

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
        cid = c["id"]
        fn = "a32_case_%d" % cid
        o += ["\t.p2align 2", "\t.globl\t%s" % fn, "\t.type\t%s, %%function" % fn, "%s:" % fn,
              "\tpush\t{r4-r11, lr}",
              "\tmov\tr12, r0",
              "\tadd\tr12, r12, #260",          # A32_OFF_REGS
              # Stash the harness's own stack pointer (the value `pop` must use)
              # in out->pad, which nothing else writes, is never compared and
              # never reaches the goldens. A PUSH/POP case parks sp inside the
              # scratch window on purpose, so the body's sp cannot be trusted
              # for the return frame; the return gadget reloads it from here.
              "\tstr\tsp, [r12, #88]",          # A32_OFF_PAD
              "\tmov\tr9, r1",
              "\tmov\tr0, #0",
              "\tcmp\tr0, #0"]                   # deterministic incoming flags: N=0 Z=1 C=1 V=0
        for reg in (0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11):
            o.append("\tmov\tr%d, #0" % reg)
        for reg in sorted(c["init"]):
            if c["init"][reg] is None:
                continue
            o += _movw_movt_lines(reg, c["init"][reg])
        # lr is cleared before the fragment runs: the interpreter presents lr = 0
        # and a case may read it (t_pushpop_lr pops it into a compared register).
        # The wrapper's own return address is already safe on the stack (the
        # prologue's push saved it, and the epilogue's pop loads pc from there),
        # so clearing lr cannot disturb the return.
        o += ["\tmov\tlr, #0"]

        if c["mode"] == "arm":
            emit_body_items(c["items"], o, "arm")
            # An ARM-entry case runs in ARM at the capture (an interworking arm
            # case returns to ARM inside its own fragment), so force `.arm`.
            o += ["\t.arm",
                  "\t.p2align 2",
                  "\tstm\tr12, {r0-r11}",
                  "\tmrs\tr0, cpsr",
                  "\tstr\tr0, [r12, #64]",
                  "\tmov\tr0, #0",
                  "\tstr\tr0, [r12, #48]",
                  "\tstr\tr0, [r12, #52]",
                  "\tstr\tr0, [r12, #56]",
                  "\tstr\tr0, [r12, #60]",
                  "\tldr\tsp, [r12, #88]",        # A32_OFF_PAD: the harness frame
                  "\tpop\t{r4-r11, pc}",
                  "\t.size\t%s, .-%s" % (fn, fn), ""]
        else:
            # Entry switch. It MUST NOT clobber a compared register. The old
            # `adr r3, 1f ; orr r3, r3, #1 ; blx r3` left r3 = the fragment's own
            # address, but r3 is comparable and cases use it as data (t32_tbb and
            # t32_tbh take their branch-table byte from r3), so the oracle ran
            # those with a garbage table entry and branched wild. A PC-relative
            # load of an inline word switches ARM -> Thumb with no register side
            # effect at all (the same idiom the capture block already uses for
            # .LretN), so the fragment is handed exactly its declared state.
            o += ["\tldr\tpc, .Lgo%d" % cid,
                  "\t.p2align 2",
                  ".Lgo%d:" % cid,
                  "\t.word\t1f + 1",
                  "\t.thumb", "\t.p2align 1", "1:"]
            emit_body_items(c["items"], o, "thumb")
            if c["post"]:
                # A 4-byte branch over the literal. Without it the oracle FALLS
                # INTO the data and executes it -- t_ldr_lit's word is `str r0,
                # [r7, r6]`, a store to address 0, i.e. SIGSEGV. Four bytes also
                # puts the literal at the address the fragment's own PC-relative
                # load computes (fragment end + 4), so the case actually reads the
                # literal the generator placed for it.
                o.append("\tb.w\t2f\t/* jump over this fragment's literal */")
            for w in c["post"]:
                o.append("\t.word\t0x%08x\t/* literal read by this fragment */" % (w & 0xFFFFFFFF))
            if c["post"]:
                o.append("2:")
            # The Thumb capture (not executed by the interpreter: it stops at the
            # fragment's byte end). Capture BEFORE the return gadget clobbers r3.
            #
            # FORCE `.thumb` FIRST, and do not inherit emit_body_items' last
            # directive. An interworking case can emit ARM helpers as its final
            # items (t_iw_blximm's `armfn` is emitted last), which leaves the
            # assembler in ARM mode -- but that fragment is back in Thumb when it
            # ends, so the ARM-encoded capture bytes would be decoded as Thumb
            # and run away. A Thumb-entry fragment MUST end in Thumb (the
            # generator's contract, checked by interp_selftest.py), so the
            # capture is Thumb and the CPU is Thumb.
            o += ["\t.thumb",
                  "\t.p2align 1",
                  "\tstmia\tr12, {r0-r11}",
                  "\tmrs\tr0, cpsr",
                  # A Thumb `mrs r0, cpsr` assembles to the APSR form, which does
                  # not have to report the T bit -- yet T is part of the compared
                  # nzcvq mask (a state-transition bug must not pass). The capture
                  # runs in Thumb by contract (a Thumb-entry fragment ends in
                  # Thumb; interp_selftest.py checks it), so set T explicitly.
                  "\torr.w\tr0, r0, #0x20",
                  "\tstr\tr0, [r12, #64]",
                  "\tmovs\tr0, #0",
                  "\tstr\tr0, [r12, #48]",
                  "\tstr\tr0, [r12, #52]",
                  "\tstr\tr0, [r12, #56]",
                  "\tstr\tr0, [r12, #60]",
                  "\tldr\tr3, .Lret%d" % cid,
                  "\tbx\tr3",
                  "\t.p2align 2",
                  ".Lret%d:" % cid,
                  "\t.word\t.Larm%d" % cid,
                  "\t.arm",
                  "\t.p2align 2",
                  ".Larm%d:" % cid,
                  "\tldr\tsp, [r12, #88]",        # A32_OFF_PAD: the harness frame
                  "\tpop\t{r4-r11, pc}"]
            o += ["\t.arm", "9:", "\t.size\t%s, .-%s" % (fn, fn), ""]
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
         "/* The byte-level view of each case: the initial register file, the raw",
         " * code bytes (byte-identical to what a32_case_<id> executes) and the",
         " * entry instruction-set state. a32diff runs these through a32_interp. */",
         "struct a32_case_words {",
         "    int          id;",
         "    const char  *name;",
         "    const char  *group;",
         "    const char  *desc;",
         "    const char  *mode;",
         "    uint32_t     init[A32_NREG];",
         "    uint32_t     code_len;",
         "    const uint8_t *code;",
         "};", ""]
    for c in CASES:
        o.append("extern void a32_case_%d(struct a32_result *, void *);" % c["id"])
    o += ["", "static const struct a32_case_meta a32_cases[] = {"]
    for c in CASES:
        o.append('    { %d, "%s", "%s", "%s", "handwritten", "%s", a32_case_%d },'
                 % (c["id"], c["name"], c["group"], c["mode"], _cstr(c["desc"]), c["id"]))
    o += ["};", "",
          "#define A32_CASE_COUNT (sizeof(a32_cases) / sizeof(a32_cases[0]))", ""]

    for c in CASES:
        o.append("static const uint8_t a32_code_%d[%d] = {" % (c["id"], max(1, len(_bytes(c)))))
        line = "    "
        for b in _bytes(c):
            piece = "0x%02x, " % b
            if len(line) + len(piece) > 96:
                o.append(line.rstrip())
                line = "    "
            line += piece
        if line.strip():
            o.append(line.rstrip())
        o.append("};")

    o += ["", "static const struct a32_case_words a32_case_words[] = {"]
    for c in CASES:
        init = ["0"] * 16
        for reg, val in c["init"].items():
            if val is not None and reg != 9:
                init[reg] = "0x%08xu" % (val & 0xFFFFFFFF)
        o.append('    { %d, "%s", "%s", "%s", "%s", { %s }, %d, a32_code_%d },'
                 % (c["id"], c["name"], c["group"], _cstr(c["desc"]), c["mode"],
                    ", ".join(init), len(_bytes(c)), c["id"]))
    o += ["};", "",
          "#define A32_WORDS_COUNT (sizeof(a32_case_words) / sizeof(a32_case_words[0]))",
          "", "#endif /* A32_CASES_GEN_H */"]
    return "\n".join(o) + "\n"


def _bytes_len(bd):
    return sum(it["size"] for it in bd.items)


def _bytes(c):
    out = bytearray()
    for it in c["items"]:
        if it["size"] == 2:
            out += (it["val"] & 0xFFFF).to_bytes(2, "little")
        else:
            out += (it["val"] & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(out)


def main():
    CASES.clear()
    build()
    with open(S_OUT, "w") as f:
        f.write(emit_s())
    with open(H_OUT, "w") as f:
        f.write(emit_h())
    print("gen_cases.py: wrote %s and %s (%d cases)"
          % (os.path.basename(S_OUT), os.path.basename(H_OUT), len(CASES)))


if __name__ == "__main__":
    main()
