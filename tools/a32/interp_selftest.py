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

    python3 tools/a32/interp_selftest.py <goldens-dir> [--a32diff PATH]

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
        code = gen_cases._bytes(c) + b"".join(
            (w & 0xFFFFFFFF).to_bytes(4, "little") for w in c["post"])
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
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
