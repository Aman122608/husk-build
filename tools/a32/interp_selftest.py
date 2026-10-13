#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
interp_selftest.py -- a DEV-ONLY local cross-check for the AArch32 harness.

CI does NOT run this (it uses the qemu-arm goldens from the ubuntu job). It
exists so the corpus encodings and the a32_interp interpreter can be exercised
on a machine with no arm toolchain and no qemu:

  * every generated instruction (A32 word or Thumb halfword/word) is decoded with
    capstone in the mode it is meant to execute in, and any item that will not
    decode is reported;
  * every case is executed under Unicorn (a QEMU-TCG-derived ARM emulator, i.e.
    an independent oracle) with the harness's exact initial state -- including
    the CPSR T bit set for a Thumb-entry case -- and the results are written as
    a32_cases.json / a32_goldens.json in the SAME schema a32oracle.c writes;
  * a32diff (built without the Apple executable-memory path) is then run against
    those goldens, so the real compare code path is what is validated.

The VFP initial state is NOT assumed: it is read out of the generated assembly's
own a32_fp_init (movw/movt + vmov), the single source of truth, and written into
Unicorn's s0..s31 so the golden's fp[] is what the assembly would have produced.

    python3 tools/a32/interp_selftest.py <goldens-dir> [--a32diff PATH] [--wrapper-check]

The --wrapper-check option additionally executes the WHOLE per-case wrapper (the
generator's prologue + the fragment + the capture/return epilogue) under Unicorn
and asserts it returns cleanly with the fragment's own captured state. That is
the only way to exercise the epilogue -- which the interpreter never runs -- on a
host with no assembler, and it is what catches an sp-clobbering case or a capture
assembled in the wrong instruction-set state.

Requires: pip install capstone unicorn   (dev only)
"""
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_cases  # noqa: E402

CODE_BASE = 0x00010000
SCRATCH = 0x00018000
ARENA = 0x00020000
MEM = 4096
CPSR_INIT = 0x60000010   # N=0 Z=1 C=1 V=0, user mode -- what a32_case_N forces
T_BIT = 0x20


def fnv1a(b):
    h = 2166136261
    for c in b:
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def fp_init_from_asm():
    """Read the s0..s31 initial values out of a32_fp_init in the generated .S."""
    text = open(gen_cases.S_OUT).read()
    vals = []
    in_init = False
    for line in text.splitlines():
        if line.startswith("a32_fp_init:"):
            in_init = True
            continue
        if in_init:
            if line.startswith("\t.size\ta32_fp_init"):
                break
            m = re.search(r"movw\s+r1, #(0x[0-9a-f]+)", line)
            if m:
                vals.append(int(m.group(1), 16))
            m = re.search(r"movt\s+r1, #(0x[0-9a-f]+)", line)
            if m:
                vals[-1] |= int(m.group(1), 16) << 16
    assert len(vals) == 32, "a32_fp_init did not yield 32 s-register values (%d)" % len(vals)
    return vals


def decode_check(cases):
    try:
        import capstone
    except ImportError:
        print("capstone not installed -- skipping the encoding decode check")
        return 0
    md_arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md_thumb = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    bad = 0
    mnems = {}
    for c in cases:
        for i, it in enumerate(c["items"]):
            if it.get("data"):
                continue        # a literal-pool word: data in the fragment, not an instruction
            b = (it["val"] & (0xFFFF if it["size"] == 2 else 0xFFFFFFFF))
            b = b.to_bytes(it["size"], "little")
            md = md_thumb if it["mode"] == "thumb" else md_arm
            insns = list(md.disasm(b, CODE_BASE + i * 2, 1))
            if not insns or insns[0].size != it["size"]:
                print("  ENCODING: case %d %s item %d (%s) = %s does not decode"
                      % (c["id"], c["name"], i, it["mode"], b.hex()))
                bad += 1
                continue
            t = insns[0].mnemonic
            if t in ("invalid", "undefined", "udf"):
                print("  ENCODING: case %d %s item %d decodes as '%s': %s"
                      % (c["id"], c["name"], i, t, b.hex()))
                bad += 1
            key = ("T:" if it["mode"] == "thumb" else "A:") + t
            mnems[key] = mnems.get(key, 0) + 1
    print("capstone: decoded every instruction of %d cases; %d bad" % (len(cases), bad))
    top = sorted(mnems.items(), key=lambda kv: -kv[1])[:24]
    print("capstone: top mnemonics: " + ", ".join("%s=%d" % (k, v) for k, v in top))
    return bad


def run_unicorn(cases, fp_init):
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UcError
    from unicorn import arm_const as ac

    goldens, manifest = [], []
    faults = 0
    for c in cases:
        code = gen_cases._bytes(c)
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(CODE_BASE, ARENA)
        uc.mem_write(CODE_BASE, code)
        seed = bytearray(MEM)
        for k in range(MEM // 4):
            struct.pack_into("<I", seed, k * 4, 0x10000000 + k)
        uc.mem_write(SCRATCH, bytes(seed))
        init = [0] * 16
        for reg, val in c["init"].items():
            if val is not None:
                init[reg] = val & 0xFFFFFFFF
        init[9] = SCRATCH
        for r in range(13):
            uc.reg_write(getattr(ac, "UC_ARM_REG_R%d" % r), init[r])
        uc.reg_write(ac.UC_ARM_REG_SP, 0)
        uc.reg_write(ac.UC_ARM_REG_LR, 0)
        uc.reg_write(ac.UC_ARM_REG_PC, CODE_BASE)
        cpsr = CPSR_INIT | (T_BIT if c["mode"] == "thumb" else 0)
        uc.reg_write(ac.UC_ARM_REG_CPSR, cpsr)
        # Unicorn leaves the VFP disabled by default, so a VLDR/VSTR/VLDM/VSTM
        # case is UC_ERR_INSN_INVALID until FPEXC.EN is set (the qemu oracle the
        # CI job uses has it set). The wide-vfp cases need this.
        uc.reg_write(ac.UC_ARM_REG_FPEXC, 0x40000000)
        # the VFP init the generated assembly establishes (source of truth).
        for i, v in enumerate(fp_init):
            uc.reg_write(getattr(ac, "UC_ARM_REG_S%d" % i), v)

        stop = 0
        begin = CODE_BASE | (1 if c["mode"] == "thumb" else 0)
        try:
            uc.emu_start(begin, CODE_BASE + len(gen_cases._bytes(c)), 0, 100000)
        except UcError as e:
            print("  UNICORN: case %d %s faulted: %s" % (c["id"], c["name"], e))
            stop = 1
            faults += 1
        regs = [uc.reg_read(getattr(ac, "UC_ARM_REG_R%d" % r)) for r in range(12)]
        regs += [0, 0, 0, 0]
        regs[9] = SCRATCH
        for i in (12, 13, 14, 15):
            regs[i] = 0
        cpsr = uc.reg_read(ac.UC_ARM_REG_CPSR)
        memnow = bytes(uc.mem_read(SCRATCH, MEM))
        fp = [uc.reg_read(getattr(ac, "UC_ARM_REG_S%d" % i)) & 0xFFFFFFFF for i in range(32)]
        goldens.append({
            "id": c["id"], "name": c["name"],
            "regs": ["0x%08x" % (regs[r] & 0xFFFFFFFF) for r in range(16)],
            "cpsr": "0x%08x" % cpsr,
            "nzcvq": "0x%08x" % (cpsr & 0xF8000020),
            "fpscr": "0x00000000", "fpscr_masked": "0x00000000",
            "fp": ["0x%016x" % v for v in fp],
            "mem_size": MEM, "mem_hash": "0x%08x" % fnv1a(memnow),
            "mem_first_diff": "0x00000000", "stop_reason": stop,
        })
        manifest.append({"id": c["id"], "name": c["name"], "group": c["group"],
                         "mode": c["mode"], "source": "handwritten", "desc": c["desc"]})
    print("unicorn: ran %d cases, %d faults" % (len(cases), faults))
    return manifest, goldens


# ---------------------------------------------------------------------------
# Wrapper check (dev-only). Build the WHOLE per-case wrapper -- the prologue the
# generator emits, the case's own bytes, and the capture +
# return epilogue in the mode the fixed .S pins -- run it under Unicorn, and
# assert it RETURNS (reaches the sentinel LR with no fault) with the state it
# captured equal to a direct run of the fragment.
#
# This is the only way to exercise the epilogue on a host with no assembler: the
# interpreter never runs it and CI had never run it either, which is exactly why
# the sp-clobber and the ARM-mode capture shipped. The scaffolding here is
# hand-encoded Python, so each word is checked against capstone first; the case
# bytes themselves are the generator's own.
# ---------------------------------------------------------------------------
SENTINEL = 0x00034000
OUTBUF   = 0x00038000
STACKTOP = 0x00032800
OFF_REGS, OFF_CPSR, OFF_PAD = 260, 324, 348
OFF_PAD_REL = OFF_PAD - OFF_REGS      # what [r12, #n] uses: r12 == out + OFF_REGS


def _u32(words):
    return b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in words)


def _u16(halfwords):
    return b"".join(struct.pack("<H", h & 0xFFFF) for h in halfwords)


def _prologue_len(c):
    """Length in bytes of the generator's wrapper prologue for this case."""
    n = 19                                   # fixed words (see _prologue_words)
    for v in c["init"].values():
        if v is None:
            continue
        n += 1 if ((v >> 16) & 0xFFFF) == 0 else 2
    return (n + (2 if c["mode"] == "thumb" else 0)) * 4


def _prologue_words(c, frag_off):
    """The generator's wrapper prologue, word for word (ARM)."""
    G = gen_cases
    o = [G.blk(0, 13, 0xFF0 | (1 << 14), 1, 0, w=1),     # push {r4-r11, lr}
         G.dp_reg(G.OP["mov"], 12, 0, 0),                # mov r12, r0
         G.dp_imm(G.OP["add"], 12, 12, OFF_REGS),        # add r12, r12, #260
         G.mem_w(0, 0, 13, 12, OFF_PAD_REL),             # str sp, [r12, #88]
         G.dp_reg(G.OP["mov"], 9, 0, 1),                 # mov r9, r1
         G.dp_imm(G.OP["mov"], 0, 0, 0),                 # mov r0, #0
         G.dp_imm(G.OP["cmp"], 0, 0, 0, s=1)]            # cmp r0, #0
    for reg in (0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11):
        o.append(G.dp_imm(G.OP["mov"], reg, 0, 0))
    for reg in sorted(c["init"]):
        v = c["init"][reg]
        if v is None:
            continue
        o.append(G.movw(reg, v & 0xFFFF))
        if (v >> 16) & 0xFFFF:
            o.append(G.movt(reg, (v >> 16) & 0xFFFF))
    o.append(G.dp_imm(G.OP["mov"], 14, 0, 0))            # mov lr, #0
    if c["mode"] == "thumb":
        # ldr pc, .LgoN ; .LgoN: .word 1f+1   (the ARM -> Thumb switch)
        # PC-relative: base is pc+8, and the word sits immediately after the ldr.
        o.append(G.mem_w(1, 0, 15, 15, 4, p=1, u=0))
        o.append(CODE_BASE + frag_off + 1)               # the inline word
    return o


def _t_bw(off):
    """Thumb-2 B.W (T4); `off` is relative to Align(insn+4, 4). Returns 2 halves."""
    assert off % 2 == 0 and -(1 << 24) <= off < (1 << 24), off
    off &= 0x1FFFFFF
    S = (off >> 24) & 1
    I1 = (off >> 23) & 1
    I2 = (off >> 22) & 1
    imm10 = (off >> 12) & 0x3FF
    imm11 = (off >> 1) & 0x7FF
    J1 = ((~I1) & 1) ^ S
    J2 = ((~I2) & 1) ^ S
    return (0xF000 | (S << 10) | imm10,
            0x9000 | (J1 << 13) | (J2 << 11) | imm11)


def _epilogue_arm():
    G = gen_cases
    return [G.blk(0, 12, 0xFFF, 0, 1),                   # stm r12, {r0-r11}
            G.mrs(0),                                    # mrs r0, cpsr
            G.mem_w(0, 0, 0, 12, 64),                    # str r0, [r12,#64]
            G.dp_imm(G.OP["mov"], 0, 0, 0),              # mov r0, #0
            G.mem_w(0, 0, 0, 12, 48), G.mem_w(0, 0, 0, 12, 52),
            G.mem_w(0, 0, 0, 12, 56), G.mem_w(0, 0, 0, 12, 60),
            G.mem_w(1, 0, 13, 12, OFF_PAD_REL),          # ldr sp, [r12,#88]
            G.blk(1, 13, 0xFF0 | (1 << 15), 0, 1, w=1)]   # pop {r4-r11, pc}


def _epilogue_thumb(a):
    """The Thumb capture + return gadget. `a` is its own (even) address."""
    cap = _u16([0xE88C, 0x0FFF,    # stmia r12, {r0-r11}
                0xF3EF, 0x8000,    # mrs r0, cpsr
                0xF040, 0x0020,    # orr.w r0, r0, #0x20  (record the T bit)
                0xF8CC, 0x0040,    # str r0, [r12, #64]
                0x2000,            # movs r0, #0
                0xF8CC, 0x0030, 0xF8CC, 0x0034,
                0xF8CC, 0x0038, 0xF8CC, 0x003C])
    assert len(cap) == 34
    ldr_off = 34                  # ldr r3, .Lret  (T16, 2 bytes) then bx r3
    after = a + ldr_off + 4       # end of bx r3
    ret = (after + 3) & ~3        # `.p2align 2` then `.Lret: .word .Larm`
    base = (a + ldr_off + 4) & ~3  # a T16 literal load uses Align(insn+4, 4)
    delta = ret - base
    assert delta % 4 == 0 and 0 <= delta // 4 < 256, (a, base, ret)
    return (cap + _u16([0x4B00 | (delta // 4), 0x4718]) + b"\x00" * (ret - after) +
            _u32([CODE_BASE + ret + 4]) +   # .Lret: .word .Larm  (an absolute addr)
            _u32(_epilogue_arm()[-2:]))   # .Larm: ldr sp / pop


def capstone_check():
    """Every hand-encoded scaffolding word must decode to what we meant."""
    import capstone
    md_a = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md_t = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    body = _epilogue_arm()
    blob = _u32(body)
    got = ["%s %s" % (i.mnemonic, i.op_str) for i in
           md_a.disasm(blob, 0, len(blob))]
    want = ["stm ip, {r0, r1, r2, r3, r4, r5, r6, r7, r8, sb, sl, fp}",
            "mrs r0, apsr", "str r0, [ip, #0x40]", "mov r0, #0",
            "str r0, [ip, #0x30]", "str r0, [ip, #0x34]",
            "str r0, [ip, #0x38]", "str r0, [ip, #0x3c]",
            "ldr sp, [ip, #0x58]", "pop {r4, r5, r6, r7, r8, sb, sl, fp, pc}"]
    assert got == want, (got, want)
    tb = _epilogue_thumb(0)[:38]
    got = ["%s %s" % (i.mnemonic, i.op_str) for i in
           md_t.disasm(tb, 0, len(tb))]
    # the literal offset is derived, so state it from the encoder itself
    want = ["stm.w ip, {r0, r1, r2, r3, r4, r5, r6, r7, r8, sb, sl, fp}",
            "mrs r0, apsr", "orr r0, r0, #0x20", "str.w r0, [ip, #0x40]", "movs r0, #0",
            "str.w r0, [ip, #0x30]", "str.w r0, [ip, #0x34]",
            "str.w r0, [ip, #0x38]", "str.w r0, [ip, #0x3c]",
            "ldr r3, [pc, #4]", "bx r3"]
    assert got == want, (got, want)


def wrapper_check(cases):
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UcError, UC_HOOK_CODE
    from unicorn import arm_const as ac
    capstone_check()
    print("capstone: the hand-encoded wrapper scaffolding decodes as intended")

    fail = 0
    entry_bad = 0
    entry_regs = set()


    for c in cases:
        G = gen_cases
        frag = G._bytes(c)
        frag_off = _prologue_len(c)          # where the fragment starts
        pro = _u32(_prologue_words(c, frag_off))
        assert len(pro) == frag_off
        # The fragment now carries its own literal pool (see gen_cases.gen_t_ldr_lit):
        # there is no post-fragment guard/literal section in the .S any more, so the
        # capture follows the fragment immediately.
        a = frag_off + len(frag)
        epi = _u32(_epilogue_arm()) if c["mode"] == "arm" else _epilogue_thumb(a)
        image = pro + frag + epi

        def fresh():
            uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
            uc.mem_map(CODE_BASE, 0x40000)
            uc.mem_write(CODE_BASE, image)
            seed = bytearray(MEM)
            for k in range(MEM // 4):
                struct.pack_into("<I", seed, k * 4, 0x10000000 + k)
            uc.mem_write(SCRATCH, bytes(seed))
            uc.mem_write(OUTBUF, b"\x00" * 0x200)
            # CPSR FIRST: writing it selects the register bank and would
            # otherwise throw away a stack pointer written before it.
            uc.reg_write(ac.UC_ARM_REG_CPSR, CPSR_INIT)
            for r in range(13):
                uc.reg_write(getattr(ac, "UC_ARM_REG_R%d" % r), 0)
            uc.reg_write(ac.UC_ARM_REG_R0, OUTBUF)
            uc.reg_write(ac.UC_ARM_REG_R1, SCRATCH)
            uc.reg_write(ac.UC_ARM_REG_SP, STACKTOP)
            uc.reg_write(ac.UC_ARM_REG_LR, SENTINEL)
            uc.reg_write(ac.UC_ARM_REG_FPEXC, 0x40000000)   # enable VFP (see run_unicorn)
            return uc

        # A. the harness's own prologue + the fragment, stopping exactly where
        #    the capture begins. Everything up to this point is identical to the
        #    wrapper's own run, so a difference can ONLY be the epilogue.
        uc = fresh()
        seen = {}
        lo, hi = CODE_BASE + frag_off, CODE_BASE + frag_off + len(frag)

        def at_entry(_uc, addr, _size, _ud):
            if lo <= addr < hi and "r" not in seen:
                seen["r"] = [_uc.reg_read(getattr(ac, "UC_ARM_REG_R%d" % r))
                             for r in range(12)]
                seen["cpsr"] = _uc.reg_read(ac.UC_ARM_REG_CPSR)
        uc.hook_add(UC_HOOK_CODE, at_entry)
        ref_err = None
        try:
            uc.emu_start(CODE_BASE, hi, 0, 200000)
        except UcError as e:
            ref_err = str(e)
        ref = [uc.reg_read(getattr(ac, "UC_ARM_REG_R%d" % r)) for r in range(12)]
        ref_cpsr = uc.reg_read(ac.UC_ARM_REG_CPSR)

        # the state the fragment was actually handed, vs the case's declared one
        want0 = [0] * 12
        want0[9] = SCRATCH
        for reg, val in c["init"].items():
            if val is not None:
                want0[reg] = val & 0xFFFFFFFF
        if "r" in seen and seen["r"] != want0:
            entry_bad += 1
            for r in range(12):
                if seen["r"][r] != want0[r]:
                    entry_regs.add("r%d" % r)

        # B. the whole wrapper.
        uc = fresh()
        err = None
        try:
            uc.emu_start(CODE_BASE, SENTINEL, 0, 200000)
        except UcError as e:
            err = str(e)
        pc = uc.reg_read(ac.UC_ARM_REG_PC)
        got = ["0x%08x" % struct.unpack("<I", uc.mem_read(OUTBUF + OFF_REGS + 4 * r, 4))[0]
               for r in range(16)]
        cpsr = "0x%08x" % struct.unpack("<I", uc.mem_read(OUTBUF + OFF_CPSR, 4))[0]
        want = ["0x%08x" % (v & 0xFFFFFFFF) for v in ref] + ["0x00000000"] * 4
        # Compare exactly the field the comparer compares: nzcvq = cpsr &
        # A32_CPSR_MASK (NZCV + Q + the T bit). The remainder of a raw cpsr is
        # explicitly never compared, and a Thumb `mrs` need not report the mode
        # bits.
        nzcvq_mask = 0xF8000020
        cpsr_ok = (int(cpsr, 16) & nzcvq_mask) == (ref_cpsr & nzcvq_mask)
        if err or ref_err or pc != SENTINEL or got != want or not cpsr_ok:
            fail += 1
            print("  WRAPPER: case %d %s (%s entry): err=%s ref=%s pc=0x%08x%s%s"
                  % (c["id"], c["name"], c["mode"], err, ref_err, pc,
                     "" if got == want else " regs differ",
                     "" if cpsr_ok else " nzcvq differs"))
    print("wrapper: ran the full prologue+fragment+epilogue for %d cases; %d did not "
          "capture exactly the state the fragment left" % (len(cases), fail))
    if entry_bad:
        print("wrapper: NOTE %d cases are handed a different entry state than they "
              "declare (registers: %s) -- the harness's own entry trampoline writes "
              "them BEFORE the fragment runs, so the oracle records the trampoline's "
              "value where the interpreter has the declared one. That is the Thumb "
              "entry switch (`adr r3, 1f` / `blx r3`) clobbering r3 (and blx's lr), "
              "not an epilogue bug, but it WILL make the compare job name these cases."
              % (entry_bad, ", ".join(sorted(entry_regs))))
    return fail


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    outdir = sys.argv[1]
    a32diff = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--a32diff" else None
    os.makedirs(outdir, exist_ok=True)

    gen_cases.CASES.clear()
    gen_cases.build()
    cases = gen_cases.CASES

    fp_init = fp_init_from_asm()
    print("a32_fp_init: s0..s3 = %s (from the generated assembly)"
          % " ".join("0x%08x" % v for v in fp_init[:4]))

    bad = decode_check(cases)
    manifest, goldens = run_unicorn(cases, fp_init)
    wrap_bad = wrapper_check(cases) if "--wrapper-check" in sys.argv else 0

    with open(os.path.join(outdir, "a32_cases.json"), "w") as f:
        json.dump({"format": "a32-cases/1", "count": len(manifest), "cases": manifest}, f, indent=1)
    with open(os.path.join(outdir, "a32_goldens.json"), "w") as f:
        json.dump({"format": "a32-goldens/1", "oracle": "unicorn",
                   "arch": "armv7-a", "count": len(goldens), "cases": goldens}, f, indent=1)
    print("wrote %d cases and %d goldens to %s" % (len(manifest), len(goldens), outdir))

    if a32diff:
        print("--- a32diff %s ---" % outdir)
        r = subprocess.run([a32diff, outdir], capture_output=True, text=True)
        print(r.stdout)
        if r.stderr:
            print("stderr:", r.stderr)
        print("a32diff exit: %d" % r.returncode)
        return r.returncode
    return 1 if (bad or wrap_bad) else 0


if __name__ == "__main__":
    sys.exit(main())
