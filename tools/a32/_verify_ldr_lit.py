"""Local unicorn verification for the t_ldr_lit_* (fresh-image) fix.

Unicorn is a QEMU-TCG derived engine -- the same class as the CI oracle -- so it
can confirm the diagnosis and the fix without the ARM toolchain or qemu-arm.
"""
import struct, unicorn
from unicorn import Uc, UC_ARCH_ARM
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_CPSR

LDR_LIT_IMM1 = 0x4801   # ldr r0, [pc, #4]
NOP          = 0xbf00
TB_BASE      = 0xE000   # 16-bit unconditional branch
THUMB, ARM = unicorn.UC_MODE_THUMB, unicorn.UC_MODE_ARM

def run(image, thumb=True, begin=0x10000, end=None):
    B = 0x10000
    uc = Uc(UC_ARCH_ARM, thumb and THUMB or ARM)
    uc.mem_map(B, 0x1000)
    uc.mem_write(B, image)
    uc.emu_start((begin | (1 if thumb else 0)), end if end is not None else begin + len(image))
    return uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_PC)

frag = struct.pack("<HH", LDR_LIT_IMM1, NOP)     # ldr ; nop   (code_len = 4)
lit  = 0x12345678

# 1. the interpreter's current case image: only the fragment bytes exist; the
#    arena is zero past code_len. The PC-relative load computes entry+8 (correct),
#    but nothing was ever placed there.
r0, _ = run(frag + b"\x00" * 16, end=0x10000 + 4)
print("A) interpreter image (code_len=4, zeros after): r0 = 0x%08x" % r0)

# 2. the oracle's image: fragment, 4-byte b.w over the literal, literal at entry+8.
r0, _ = run(frag + b"\x00" * 4 + struct.pack("<I", lit), end=0x10000 + 4)
print("B) oracle image (literal at entry+8):           r0 = 0x%08x" % r0)

# 3. FIX: fold the literal INTO the fragment image with a 16-bit branch over it.
#    fragment = ldr ; nop ; b(over) ; nop ; .word lit   -> code_len = 12
b_off  = 4                                    # tgt-(a+4); a=4, tgt=12 -> imm11=2
b_insn = TB_BASE | (b_off >> 1)
fixed  = struct.pack("<HHHH", LDR_LIT_IMM1, NOP, b_insn, NOP) + struct.pack("<I", lit)
r0, pc = run(fixed, end=0x10000 + 12)         # run the whole fragment to code_end
print("C) fixed image (literal inside fragment, b over it): r0 = 0x%08x, pc = 0x%08x"
      % (r0, pc))

assert r0 == lit if False else True
assert run(frag + b"\x00" * 16, end=0x10000 + 4)[0] == 0
assert run(frag + b"\x00" * 4 + struct.pack("<I", lit), end=0x10000 + 4)[0] == lit
assert r0 == lit and pc == 0x10000 + 12
print("\nRESULT: the load address is right in every model; the value is 0 only when the")
print("case image does not contain the post-fragment literal. Folding the literal into")
print("the fragment (with a branch over it) makes both engines read the same word at the")
print("same PC-relative address, and the fragment still ends at code_end (pc=0x%08x)."
      % pc)
