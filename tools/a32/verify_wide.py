#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
verify_wide.py -- local, real verification of the new encoding classes.

It builds the existing 970 cases plus gen_cases_wide.py's new cases, then for
EVERY case:

  * decodes every instruction with capstone (a bad encoding is named, not run);
  * executes it under Unicorn with the harness's exact initial state (the same
    oracle class the differential CI job uses) to produce a golden;
  * executes the same bytes through the unchanged reference interpreter
    (a32_shim.c, compiled into a DLL) and diffs regs / nzcvq / fp[] / mem_hash /
    stop_reason field by field -- exactly the fields a32diff compares.

The 970 existing cases are included so a regression cannot hide, and the new
ones are the classes this change implements (wide multiply, wide load/store,
the Thumb-1 extend/reverse group, and the VFP memory forms). A case that the
interpreter cannot run is a FAILURE, never a silent pass.

    python3 tools/a32/verify_wide.py
"""
from __future__ import annotations

import ctypes as C
import os
import shutil
import subprocess
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gen_cases as G          # noqa: E402
import gen_cases_wide as W     # noqa: E402
import interp_selftest as S    # noqa: E402

NZCVQ = 0xF8000020


def run_unicorn(cases, fp_init):
    """The interp_selftest oracle, with FPEXC.EN set: Unicorn leaves the VFP
    disabled by default, so a VFP instruction is UC_ERR_INSN_INVALID until the
    FPEXC enable bit is set (the qemu oracle the CI job uses has it set)."""
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UcError
    from unicorn import arm_const as ac
    CODE_BASE, SCRATCH, ARENA, MEM = 0x00010000, 0x00018000, 0x00020000, 4096
    CPSR_INIT, T_BIT = 0x60000010, 0x20

    def fnv1a(b):
        h = 2166136261
        for c in b:
            h ^= c
            h = (h * 16777619) & 0xFFFFFFFF
        return h

    goldens = []
    faults = 0
    for c in cases:
        # The literal pool is an ITEM of the fragment now (there is no Body.post),
        # so the fragment bytes are the whole image both engines see.
        code = G._bytes(c)
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(CODE_BASE, ARENA)
        uc.mem_write(CODE_BASE, code)
        seed = bytearray(MEM)
        import struct
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
        uc.reg_write(ac.UC_ARM_REG_CPSR, CPSR_INIT | (T_BIT if c["mode"] == "thumb" else 0))
        uc.reg_write(ac.UC_ARM_REG_FPEXC, 0x40000000)      # enable VFP
        for i, v in enumerate(fp_init):
            uc.reg_write(getattr(ac, "UC_ARM_REG_S%d" % i), v)

        stop = 0
        begin = CODE_BASE | (1 if c["mode"] == "thumb" else 0)
        try:
            uc.emu_start(begin, CODE_BASE + len(G._bytes(c)), 0, 100000)
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
            "nzcvq": "0x%08x" % (cpsr & NZCVQ),
            "fp": ["0x%016x" % v for v in fp],
            "mem_hash": "0x%08x" % fnv1a(memnow),
            "stop_reason": stop,
        })
    print("unicorn: ran %d cases, %d faults" % (len(cases), faults))
    return goldens


def build_shim():
    for cc in [os.environ.get("A32_CLANG", ""), r"C:/Program Files/LLVM/bin/clang.exe",
               "clang", "cc", "zig"]:
        if not cc:
            continue
        if cc in ("clang", "cc", "zig"):
            if not shutil.which(cc):
                continue
        elif not os.path.exists(cc):
            continue
        out = os.path.join(os.environ.get("TEMP", "/tmp"), "wide_%s.dll" % uuid.uuid4().hex[:8])
        cmd = [cc] + (["cc"] if cc == "zig" else []) + \
              ["-std=c11", "-O2", "-shared", "-o", out,
               os.path.join(HERE, "a32_shim.c"), os.path.join(HERE, "a32_interp.c")]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(out):
            return out
        sys.stderr.write(r.stdout + r.stderr)
    return None


class Interp:
    def __init__(self, path):
        self.lib = C.CDLL(path)
        f = self.lib.a32_shim_run_full
        f.restype = C.c_int
        f.argtypes = [C.c_char_p, C.c_uint32, C.c_int, C.POINTER(C.c_uint32), C.c_uint32,
                      C.POINTER(C.c_uint32), C.POINTER(C.c_uint32), C.POINTER(C.c_uint32),
                      C.POINTER(C.c_uint32), C.POINTER(C.c_uint32), C.c_char_p, C.c_size_t]

    def run(self, code, thumb, regs):
        inr = (C.c_uint32 * 16)(*regs)
        out = (C.c_uint32 * 16)()
        cpsr = C.c_uint32(); stop = C.c_uint32(); mhash = C.c_uint32()
        fp = (C.c_uint32 * 32)()
        err = C.create_string_buffer(512)
        rc = self.lib.a32_shim_run_full(bytes(code), len(code), 1 if thumb else 0, inr, 1,
                                        out, C.byref(cpsr), fp, C.byref(mhash),
                                        C.byref(stop), err, 512)
        return rc, list(out), cpsr.value, list(fp), mhash.value, stop.value, err.value.decode("latin-1")


def main():
    G.CASES.clear()
    G.build()
    W.add_cases()
    cases = list(G.CASES)
    n_base = 970
    print("cases: %d total (%d existing + %d new)" % (len(cases), n_base, len(cases) - n_base))

    bad = S.decode_check(cases)
    fp_init = S.fp_init_from_asm()
    goldens = run_unicorn(cases, fp_init)
    gold = {g["id"]: g for g in goldens}

    dll = build_shim()
    if not dll:
        print("no C compiler -- cannot run the interpreter pass")
        return 2
    it = Interp(dll)

    fails = 0
    new_fails = 0
    for c in cases:
        cid = c["id"]
        g = gold[cid]
        regs = [0] * 16
        for r, v in c["init"].items():
            if v is not None:
                regs[r] = v & 0xFFFFFFFF
        rc, out, cpsr, fp, mhash, stop, err = it.run(
            G._bytes(c), c["mode"] == "thumb", regs)
        problems = []
        if rc != 0:
            problems.append("did not run: %s" % err.split(": ", 1)[-1])
        g_regs = [int(x, 16) for x in g["regs"]]
        for r in range(16):
            if out[r] != g_regs[r]:
                problems.append("r%d interp=0x%08x oracle=0x%08x" % (r, out[r], g_regs[r]))
        if (cpsr & NZCVQ) != int(g["nzcvq"], 16):
            problems.append("nzcvq interp=0x%08x oracle=%s" % (cpsr & NZCVQ, g["nzcvq"]))
        gfp = [int(x, 16) for x in g["fp"]]
        for i in range(32):
            if fp[i] != (gfp[i] & 0xFFFFFFFF):
                problems.append("s%d interp=0x%08x oracle=0x%08x" % (i, fp[i], gfp[i] & 0xFFFFFFFF))
        if mhash != int(g["mem_hash"], 16):
            problems.append("mem_hash interp=0x%08x oracle=%s" % (mhash, g["mem_hash"]))
        if stop != g["stop_reason"]:
            problems.append("stop interp=%d oracle=%d" % (stop, g["stop_reason"]))
        if problems:
            fails += 1
            if cid > n_base:
                new_fails += 1
            print("  FAIL case %d %s (%s)%s: %s"
                  % (cid, c["name"], c["group"],
                     "  [NEW]" if cid > n_base else "", "; ".join(problems[:6])))
    print("verify: %d/%d cases agree with the Unicorn oracle field-for-field; "
          "%d failures (%d of them new cases)"
          % (len(cases) - fails, len(cases), fails, new_fails))
    if bad:
        print("verify: %d encodings did not decode" % bad)
    return 1 if (fails or bad) else 0


if __name__ == "__main__":
    sys.exit(main())
