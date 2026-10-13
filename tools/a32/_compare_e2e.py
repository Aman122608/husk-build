"""End-to-end exercise of the compare job on a plain host (no qemu).

The interpreter dumps a cases/goldens pair; this replaces the 17 affected
cases' goldens with ORACLE-LIKE values (a different code base and scratch base,
as the real qemu-arm oracle has) and then runs a32diff. It proves the compare
CHANGE behaves: the placement-dependent cases go green via the placement-aware
path, and planting a wrong offset/state bit still goes red.
"""
import json, os, shutil, subprocess, sys

DIR = sys.argv[1]                          # dir holding a32diff.exe + the dumps
DIFF = os.path.join(DIR, "a32diff.exe")
CASES = os.path.join(DIR, "a32_cases.json")
GOLD  = os.path.join(DIR, "a32_goldens.json")

# oracle-like addresses: a code base the interpreter never uses, and the mmap'd
# scratch window the oracle's r9 actually points at.
ORA_SCRATCH = 0x41021000
# per-case code base chosen so the reported golden values fall out (each case
# function is at its own link-time address in the oracle).
ORA_CODE = {616: 0x257d4, 617: 0x257d4, 663: 0x273e4, 664: 0x273e4,
            963: 0x323b8, 964: 0x323b8, 965: 0x323b8,
            966: 0x323b0, 967: 0x323b0, 968: 0x323b0}

LDR = {652: 0x12345678, 653: 0x12345679, 654: 0x1234567a}
# (case id -> (register, oracle-like golden value))
PLACED = {}
for _i, _off in ((659, 0x8), (660, 0xc), (661, 0x18), (662, 0x44)):
    PLACED[_i] = (1, ORA_SCRATCH + _off)      # r1 = r9 + off
for _i in (663, 664):
    PLACED[_i] = (1, 0x273f0)                 # r1 = ADR(b): a code address (even)
for _i in (616, 617):
    PLACED[_i] = (3, ORA_CODE[_i] + 0xd)      # r3 = ADR(tgt)+1 (odd)
for _i in (963, 964, 965):
    PLACED[_i] = (3, 0x323c8)                 # r3 = ADR(arm) (even)
for _i in (966, 967, 968):
    PLACED[_i] = (3, 0x323c9)                 # r3 = ADR(thumb)+1 (odd)

def base_doc():
    with open(GOLD) as f: g = json.load(f)
    for rec in g["cases"]:
        i = rec["id"]
        r = rec["regs"]
        if i in LDR:
            r[0] = "0x%08x" % LDR[i]
        if i in PLACED:
            reg, val = PLACED[i]
            r[reg] = "0x%08x" % val
            if i in (659, 660, 661, 662):
                r[9] = "0x%08x" % ORA_SCRATCH
    return g

def write(g, path):
    with open(path, "w") as f:
        json.dump(g, f)

def run(goldens_path):
    p = subprocess.run([DIFF, DIR, "--self-check"], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr

def summary(out):
    for line in out.splitlines():
        if line.startswith("compare: interpreter:") or "MISMATCH" in line \
           or line.startswith("compare: PASS") or line.startswith("compare: FAIL") \
           or "--self-check PASS" in line or "case" in line and "field" in line:
            print("   " + line.strip())
    return

g = base_doc()
write(g, GOLD)
print("== baseline: oracle-like goldens for the 17 affected cases ==")
rc, out = run(GOLD); summary(out); print("   exit=%d" % rc)

def mutate(case_id, reg, value, label):
    g = base_doc()
    for rec in g["cases"]:
        if rec["id"] == case_id:
            rec["regs"][reg] = "0x%08x" % value
    write(g, GOLD)
    print("== planted red: %s (case %d regs[%d]=0x%x) ==" % (label, case_id, reg, value))
    rc, out = run(GOLD); summary(out); print("   exit=%d (1 = red, as required)" % rc)

mutate(660, 1, ORA_SCRATCH + 0x10, "scratch-relative wrong offset")
mutate(616, 3, ORA_CODE[616] + 0x0c, "code-relative wrong STATE BIT (even, want odd)")
write(base_doc(), GOLD)                     # restore
