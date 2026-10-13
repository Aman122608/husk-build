#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
interp_selftest.py -- a DEV-ONLY local cross-check for the A32 harness.

CI does NOT run this (it uses the qemu-arm goldens from the ubuntu job). It
exists so the corpus encodings and the a32_interp interpreter can be exercised
on a machine with no arm toolchain and no qemu:

  * every generated A32 word is decoded with capstone (an independent decoder)
    and any word that will not decode is reported;
  * every case is executed under Unicorn (a QEMU-TCG-derived ARM emulator, i.e.
    an independent oracle) with the harness's exact initial state, and the
    results are written as a32_cases.json / a32_goldens.json in the SAME schema
    a32oracle.c writes;
  * a32diff (built without the Apple executable-memory path) is then run against
    those goldens, so the real compare code path is what is validated.

    python3 tools/a32/interp_selftest.py <goldens-dir> [--a32diff PATH]

Requires: pip install capstone unicorn   (dev only)
"""
import json
import os
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


def fnv1a(b):
    h = 2166136261
    for c in b:
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def decode_check(cases):
    try:
        import capstone
    except ImportError:
        print("capstone not installed -- skipping the encoding decode check")
        return 0
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    bad = 0
    mnems = {}
    for c in cases:
        for i, w in enumerate(c["words"]):
            insns = list(md.disasm(struct.pack("<I", w), CODE_BASE + i * 4, 1))
            if not insns:
                print("  ENCODING: case %d %s word %d = 0x%08x does not decode"
                      % (c["id"], c["name"], i, w))
                bad += 1
                continue
            t = insns[0].mnemonic
            if t in ("invalid", "undefined"):
                print("  ENCODING: case %d %s word %d = 0x%08x decodes as '%s'"
                      % (c["id"], c["name"], i, w, t))
                bad += 1
            mnems[t] = mnems.get(t, 0) + 1
    print("capstone: decoded every word of %d cases; %d bad" % (len(cases), bad))
    print("capstone: mnemonics: " + ", ".join(
        "%s=%d" % (k, mnemonics) for k, mnemonics in sorted(mnems.items(), key=lambda kv: -kv[1])))
    return bad


def run_unicorn(cases):
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UcError
    from unicorn import arm_const as ac

    goldens, manifest = [], []
    faults = 0
    for c in cases:
        code = b"".join(struct.pack("<I", w) for w in c["words"])
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
        uc.reg_write(ac.UC_ARM_REG_CPSR, CPSR_INIT)

        stop = 0
        try:
            uc.emu_start(CODE_BASE, CODE_BASE + len(code), 0, 100000)
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
        goldens.append({
            "id": c["id"], "name": c["name"],
            "regs": ["0x%08x" % (regs[r] & 0xFFFFFFFF) for r in range(16)],
            "cpsr": "0x%08x" % cpsr,
            "nzcvq": "0x%08x" % (cpsr & 0xF8000000),
            "fpscr": "0x00000000", "fpscr_masked": "0x00000000",
            "fp": ["0x000000003f800000"] * 32,
            "mem_size": MEM, "mem_hash": "0x%08x" % fnv1a(memnow),
            "mem_first_diff": "0x00000000", "stop_reason": stop,
        })
        manifest.append({"id": c["id"], "name": c["name"], "group": c["group"],
                         "mode": "arm", "source": "handwritten", "desc": c["desc"]})
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

    bad = decode_check(cases)
    manifest, goldens = run_unicorn(cases)

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
