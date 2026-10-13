#!/usr/bin/env python3
"""
dynmix.py -- DYNAMIC AArch32 instruction mix over the nine 32-bit-only games.

Where tools/a32/traffic.py measures what is *present* in the code image, this
measures what real game code *executes*, without a phone, by running real
functions out of the armhf libraries in Games32/ and counting the instructions
that actually retire.

Two engines, used for two different questions:

  1. Unicorn (the same class of engine the differential harness uses as its
     oracle) *executes* each chosen function: real control flow, real VFP/NEON
     semantics, stopped the moment control leaves the library (an import / PLT
     entry / out-of-library call) or touches unmapped memory. Every instruction
     it decodes is classified with capstone. This gives the honest dynamic mix
     -- integer / VFP / NEON / Thumb / wide-Thumb -- and the dynamic weight of
     VFP and NEON specifically, which a static histogram cannot give.

  2. The A32 reference interpreter itself (tools/a32/a32_interp.c, compiled
     unchanged into a small shim DLL and driven through ctypes) is asked, for
     every distinct encoding the real code executes, whether it DECODES it or
     REFUSES it -- and the same interpreter is run on each real function window
     to see how far it gets before the first refusal. The refused encodings,
     ranked by dynamic frequency, are the next work queue.

Nothing here fabricates execution: a function that cannot run meaningfully is a
COUNTED stop, never coverage.

Usage:
    python tools/a32/dynmix.py --apk-dir Games32 --per-lib 1500 --max-steps 1200
"""

from __future__ import annotations

import argparse
import collections
import ctypes as C
import io
import json
import os
import shutil
import struct
import sys
import uuid
import zipfile

import capstone as cs
from elftools.elf.elffile import ELFFile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import traffic as T  # noqa: E402  (reuse its ELF reader + classifier + engines)

from unicorn import (  # noqa: E402
    Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID,
    UC_HOOK_INTR, UC_PROT_ALL, UcError, UC_MEM_FETCH, UC_MEM_READ, UC_MEM_WRITE,
)
from unicorn.arm_const import (  # noqa: E402
    UC_ARM_REG_CPSR, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
)

# --------------------------------------------------------------------------
# The interpreter shim (compiled from a32_shim.c + a32_interp.c, unchanged)
# --------------------------------------------------------------------------
CLANG_CANDIDATES = [
    os.environ.get("A32_CLANG", ""),
    r"C:/Program Files/LLVM/bin/clang.exe",
    "clang", "cc", "zig",
]
HERE = os.path.dirname(os.path.abspath(__file__))
DLL_CACHE = {}

# refuse reasons the interpreter emits (a32_fault substrings that mean the
# ENCODING is not implemented, as opposed to a runtime/architectural fault).
UNIMPL_MARK = "unimplemented"


def _find_clang():
    for cand in CLANG_CANDIDATES:
        if not cand:
            continue
        if cand in ("clang", "cc", "zig"):
            if shutil.which(cand):
                return cand
        elif os.path.exists(cand):
            return cand
    return None


def build_shim():
    """Compile a32_shim.c + a32_interp.c into a DLL and return its path.
    Returns None if no C compiler is available (then only engine 1 runs)."""
    cc = _find_clang()
    if cc is None:
        return None
    out = os.path.join(os.environ.get("TEMP", "/tmp"),
                       "a32_interp_%s.dll" % uuid.uuid4().hex[:8])
    if cc.endswith("zig") or cc == "zig":
        cmd = [cc, "cc", "-std=c11", "-O2", "-shared", "-o", out,
               os.path.join(HERE, "a32_shim.c"), os.path.join(HERE, "a32_interp.c")]
    else:
        cmd = [cc, "-std=c11", "-O2", "-shared", "-o", out,
               os.path.join(HERE, "a32_shim.c"), os.path.join(HERE, "a32_interp.c")]
    import subprocess
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        sys.stderr.write("[dynmix] shim build failed:\n%s\n%s\n" % (r.stdout, r.stderr))
        return None
    return out


class Interp:
    """ctypes view of the unchanged reference interpreter."""

    def __init__(self, dll_path):
        self.lib = C.CDLL(dll_path)
        f = self.lib.a32_shim_run
        f.restype = C.c_int
        f.argtypes = [C.c_char_p, C.c_uint32, C.c_int, C.POINTER(C.c_uint32),
                      C.c_uint32, C.POINTER(C.c_uint32), C.POINTER(C.c_uint32),
                      C.POINTER(C.c_uint32), C.c_char_p, C.c_size_t]
        g = self.lib.a32_shim_run_image
        g.restype = C.c_int
        g.argtypes = [C.POINTER(C.c_uint32), C.POINTER(C.c_uint32),
                      C.POINTER(C.c_uint32), C.POINTER(C.c_void_p), C.c_uint32,
                      C.c_uint32, C.c_int, C.POINTER(C.c_uint32), C.c_uint32,
                      C.c_uint32, C.c_uint32, C.c_uint32, C.c_uint32,
                      C.POINTER(C.c_uint32), C.POINTER(C.c_uint32),
                      C.POINTER(C.c_uint32), C.c_char_p, C.c_size_t]
        self._probe_cache = {}
        self._img = None

    def run(self, code: bytes, thumb: int, regs=None):
        """Run one fragment as one interpreter case. Returns
        (rc, stop_reason, err, out_regs)."""
        if regs is not None:
            inregs = (C.c_uint32 * 16)(*regs)
            inp = inregs
        else:
            inp = None
        out = (C.c_uint32 * 16)()
        stop = C.c_uint32()
        cpsr = C.c_uint32()
        err = C.create_string_buffer(512)
        buf = bytes(code)
        rc = self.lib.a32_shim_run(buf, len(code), 1 if thumb else 0, inp, 1,
                                   out, C.byref(cpsr), C.byref(stop), err, 512)
        return rc, stop.value, err.value.decode("latin-1"), list(out)

    def set_image(self, regions):
        """Register one region table (an image) for run_image(). `regions` is a
        list of (vaddr, size, exec, ctypes_buffer); the buffers are kept alive
        here for the lifetime of the image."""
        self._img = list(regions)
        n = len(self._img)
        self._va = (C.c_uint32 * n)(*[r[0] for r in self._img])
        self._sz = (C.c_uint32 * n)(*[r[1] for r in self._img])
        self._ex = (C.c_uint32 * n)(*[r[2] for r in self._img])
        self._hp = (C.c_void_p * n)(*[C.addressof(r[3]) for r in self._img])

    def run_image(self, pc, thumb, regs, sp, lr, ret, step_limit):
        """Run one real function in the registered image. Returns
        (rc, stop_reason, err, out_regs)."""
        n = len(self._img)
        inr = (C.c_uint32 * 16)(*regs)
        out = (C.c_uint32 * 16)()
        stop = C.c_uint32()
        cpsr = C.c_uint32()
        err = C.create_string_buffer(512)
        rc = self.lib.a32_shim_run_image(
            self._va, self._sz, self._ex, self._hp, n, pc, 1 if thumb else 0,
            inr, sp, lr, ret, step_limit, 1, out, C.byref(cpsr),
            C.byref(stop), err, 512)
        return rc, stop.value, err.value.decode("latin-1"), list(out)

    def refuses(self, mode: str, raw: bytes):
        """Does the reference interpreter REFUSE this encoding (a named
        'unimplemented' fault) rather than decode it?  Probed as a
        one-instruction fragment; a runtime/architectural fault does NOT count
        as a refusal -- only the decoder's own 'unimplemented' verdict does.
        Returns (refused: bool, reason: str)."""
        key = (mode, raw)
        if key in self._probe_cache:
            return self._probe_cache[key]
        rc, stop, err, _ = self.run(raw, 1 if mode == "T32" else 0)
        refused = UNIMPL_MARK in err
        reason = err.split(": ", 1)[-1].split(" at pc=", 1)[0] if err else ""
        self._probe_cache[key] = (refused, reason)
        return refused, reason


# --------------------------------------------------------------------------
# ELF image for Unicorn
# --------------------------------------------------------------------------
EXEC, WRITE, READ = 1, 2, 4


class LibImage:
    def __init__(self, data, name):
        self.name = name
        self.data = data
        self.elf = ELFFile(io.BytesIO(data))
        self.segments = []       # (vaddr, memsz, filesz, flags, bytes)
        self.exec_secs = []      # (addr, size, bytes) of SHF_EXECINSTR sections
        for seg in self.elf.iter_segments():
            if seg["p_type"] != "PT_LOAD":
                continue
            vaddr = seg["p_vaddr"]
            memsz = seg["p_memsz"]
            filesz = seg["p_filesz"]
            flags = seg["p_flags"]
            sdata = seg.data() if filesz else b""
            self.segments.append((vaddr, memsz, filesz, flags, sdata))
        # Executable RANGES are the SHF_EXECINSTR sections, not the whole RX
        # PT_LOAD segment (which on these libs starts at vaddr 0 and includes the
        # ELF header, so a wild jump to 0x64 would masquerade as "library code").
        for s in self.elf.iter_sections():
            try:
                flags = s["sh_flags"]
            except Exception:                            # noqa: BLE001
                continue
            if not (flags & 0x4) or s["sh_size"] == 0:
                continue
            try:
                sdata = s.data()
            except Exception:                            # noqa: BLE001
                sdata = b""
            self.exec_secs.append((s["sh_addr"], s["sh_size"], sdata))
        self.exec_ranges = sorted((a, a + sz) for a, sz, _ in self.exec_secs)
        if not self.exec_ranges:      # fallback: whole X segments
            for vaddr, memsz, filesz, flags, sdata in self.segments:
                if flags & EXEC:
                    self.exec_ranges.append((vaddr, vaddr + memsz))

    def in_exec(self, addr):
        for lo, hi in self.exec_ranges:
            if lo <= addr < hi:
                return True
        return False

    def exec_bytes_from(self, addr, limit):
        """Bytes of the library image starting at `addr`, up to `limit`."""
        for a, sz, sdata in self.exec_secs:
            if a <= addr < a + sz:
                off = addr - a
                return sdata[off:off + limit]
        return b""


def map_image(uc, img: LibImage, page=0x1000):
    for vaddr, memsz, filesz, flags, sdata in img.segments:
        if memsz == 0:
            continue
        lo = vaddr & ~(page - 1)
        hi = (vaddr + memsz + page - 1) & ~(page - 1)
        try:
            uc.mem_map(lo, hi - lo, UC_PROT_ALL)
        except UcError:
            # overlap (some libs have adjacent/overlapping PT_LOADs) -- skip
            pass
        if filesz:
            try:
                uc.mem_write(vaddr, sdata[:filesz])
            except UcError:
                pass


# --------------------------------------------------------------------------
# One function executed under Unicorn
# --------------------------------------------------------------------------
SENTINEL = 0x7FF00000      # mapped; a return reaches here
STACK_BASE = 0x7F000000
STACK_SIZE = 0x00100000
SCRATCH_BASE = 0x60000000  # a zero-filled "heap" the r0-r3 arguments point into
SCRATCH_SIZE = 0x00200000
MAX_AUTOMAP = 512          # cap on pages we zero-fill on demand, per function
AUTOMAP_HI = 0x80000000    # never auto-map the sentinel/high space

# ---- the region-mapped interpreter image (a32_interp_run_image) ------------
# The same neutral inputs the Unicorn run uses (r0-r3 = distinct pointers into a
# zero-filled scratch, SP on a scratch stack, LR = the return sentinel), but now
# the library is registered at its OWN virtual addresses instead of a 0x10000
# arena, which is what lets the reference interpreter RUN real code rather than
# stop at the first data access its arena cannot express.
IMAGE_STEP_LIMIT = 20000   # bound on a runaway function in the image pass
IMG_R0 = (SCRATCH_BASE + 0x00100, SCRATCH_BASE + 0x10100,
          SCRATCH_BASE + 0x20100, SCRATCH_BASE + 0x30100)


def build_image(img: LibImage, page=0x1000):
    """One host buffer per PT_LOAD at its real p_vaddr (page-rounded), plus a
    zero-filled scratch region, a scratch stack and a return sentinel. Overlaps
    are resolved by keeping the first region, exactly as map_image() does for
    Unicorn. Returns a list of (vaddr, size, exec, buffer)."""
    regions = []
    have = []
    for vaddr, memsz, filesz, flags, sdata in img.segments:
        if memsz == 0:
            continue
        lo = vaddr & ~(page - 1)
        hi = (vaddr + memsz + page - 1) & ~(page - 1)
        if any(not (hi <= a or lo >= b) for (a, b) in have):
            continue                       # overlap: keep the first
        buf = C.create_string_buffer(hi - lo)
        if filesz:
            C.memmove(C.addressof(buf) + (vaddr - lo), sdata[:filesz], filesz)
        have.append((lo, hi))
        regions.append((lo, hi - lo, 1 if (flags & EXEC) else 0, buf))
    for base, size, ex in ((SCRATCH_BASE, SCRATCH_SIZE, 0),
                           (STACK_BASE, STACK_SIZE, 0),
                           (SENTINEL, 0x1000, 0)):
        regions.append((base, size, ex, C.create_string_buffer(size)))
    return regions


# classes, in report order
CLASS_ORDER = ["integer", "vfp", "neon", "exotic"]

# stop reasons
ST_RETURN = "return"
ST_CALL_LEAVE = "call_left_library"
ST_UNMAPPED = "unmapped_memory"
ST_BADCODE = "left_real_code"
ST_LIMIT = "step_limit"
ST_EXC = "exception"
ST_BADPC = "pc_not_in_library_code"


class Engines:
    def __init__(self):
        self.arm = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_ARM)
        self.arm.detail = True
        self.thumb = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB)
        self.thumb.detail = True

    def decode_one(self, uc, addr, mode):
        md = self.arm if mode == "A32" else self.thumb
        # Always read 4 bytes and let capstone decide the size: reading only 2 in
        # Thumb truncated every 32-bit Thumb-2 instruction to a 16-bit mis-decode,
        # which is why wide Thumb-2 first showed up as "never executed".
        try:
            raw = bytes(uc.mem_read(addr, 4))
        except UcError:
            try:
                raw = bytes(uc.mem_read(addr, 2))
            except UcError:
                return None
        ins = next(md.disasm(raw, addr, count=1), None)
        if ins is None:
            return None
        return ins


def run_function(eng, img, faddr_in, thumb_entry, max_steps):
    """Execute one real function under Unicorn. Returns a dict with dynamic
    counts (by class / mnemonic / mode-size / raw encoding) and the stop."""
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    map_image(uc, img)
    uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
    uc.mem_map(SCRATCH_BASE, SCRATCH_SIZE, UC_PROT_ALL)
    uc.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)

    entry = faddr_in & ~1
    # Set the mode/T state FIRST: on ARM, writing CPSR can switch register banks,
    # so SP/LR must be written afterwards or they land in the wrong bank and the
    # function's first `push` faults at 0xfffffff8 (as it did before this fix).
    uc.reg_write(UC_ARM_REG_CPSR, 0x30 if thumb_entry else 0x10)
    uc.reg_write(UC_ARM_REG_SP, STACK_BASE + STACK_SIZE - 0x100)
    uc.reg_write(UC_ARM_REG_LR, SENTINEL)
    # Plausible arguments: r0-r3 are distinct pointers into a zero-filled mapped
    # scratch region (not NULL). NULL arguments make the very first pointer
    # dereference fault, which would measure prologues and nothing else; a
    # valid, readable pointer lets the function's own internal branches run.
    # The values they point at are zero -- we do not invent data.
    uc.reg_write(UC_ARM_REG_R0, SCRATCH_BASE + 0x00100)
    uc.reg_write(UC_ARM_REG_R1, SCRATCH_BASE + 0x10100)
    uc.reg_write(UC_ARM_REG_R2, SCRATCH_BASE + 0x20100)
    uc.reg_write(UC_ARM_REG_R3, SCRATCH_BASE + 0x30100)
    if thumb_entry:
        uc.reg_write(UC_ARM_REG_CPSR, 0x30)   # T bit set, user mode

    state = {
        "n": 0, "stop": None, "why": "",
        "cls": collections.Counter(),
        "mne": collections.Counter(),
        "mode": collections.Counter(),
        "enc": collections.Counter(),   # (mode, size, word) -> count
        "invalid": 0,
        "automap": 0, "automap_total": 0,
    }

    def hook_code(u, addr, size, user):
        if state["stop"] is not None:
            return
        if addr == SENTINEL:
            state["stop"] = ST_RETURN
            u.emu_stop()
            return
        if not img.in_exec(addr):
            # control left the library's code: an import stub, a PLT entry, a call
            # through a not-yet-relocated GOT slot, or any target outside the lib.
            state["stop"] = ST_CALL_LEAVE
            u.emu_stop()
            return
        mode = "T32" if (u.reg_read(UC_ARM_REG_CPSR) & 0x20) else "A32"
        ins = eng.decode_one(u, addr, mode)
        if ins is None:
            state["invalid"] += 1
        else:
            gs = tuple(eng.arm.group_name(g) if mode == "A32"
                       else eng.thumb.group_name(g) for g in ins.groups)
            cls = T.classify(ins.mnemonic, gs, ins.size, mode)
            state["cls"][cls] += 1
            state["mne"][ins.mnemonic] += 1
            state["mode"][f"{mode}{'.w' if (mode == 'T32' and ins.size == 4) else ''}"] += 1
            word = int.from_bytes(ins.bytes, "little")
            state["enc"][(mode, ins.size, word)] += 1
        state["n"] += 1
        if state["n"] >= max_steps:
            state["stop"] = ST_LIMIT
            u.emu_stop()

    def hook_mem(u, access, addr, size, value, user):
        # A FETCH to an unmapped/never-mapped address means control left the
        # library's code (an unrelocated GOT call, an indirect call target we
        # cannot resolve) -- that is a counted stop, exactly as intended.
        if access == UC_MEM_FETCH:
            state["stop"] = ST_CALL_LEAVE
            state["why"] = "fetch 0x%08x" % addr
            u.emu_stop()
            return False
        # A DATA access to memory we did not model (heap / another library /
        # an unrelocated pointer) is modelled as freshly-zeroed memory, bounded
        # by a per-function page cap. We count how often this happens so the
        # report can say plainly how much of the run rested on it.
        if addr < AUTOMAP_HI and state["automap"] < MAX_AUTOMAP:
            page = addr & ~0xFFF
            n = ((addr + size - 1) & ~0xFFF) - page + 0x1000
            try:
                u.mem_map(page, n, UC_PROT_ALL)
                state["automap"] += n // 0x1000
                state["automap_total"] += 1
                return True
            except UcError:
                pass
        state["stop"] = ST_UNMAPPED
        state["why"] = "access 0x%08x size %d" % (addr, size)
        u.emu_stop()
        return False

    def hook_intr(u, intno, user):
        state["stop"] = ST_EXC
        state["why"] = "intr %d" % intno
        u.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.hook_add(UC_HOOK_MEM_INVALID, hook_mem)
    uc.hook_add(UC_HOOK_INTR, hook_intr)

    # Unicorn forces ARM state on emu_start unless the entry address carries the
    # Thumb bit, so a Thumb function is entered as (entry | 1); the code hook
    # then sees the even PC and the T bit set.
    start = (entry | 1) if thumb_entry else entry
    try:
        uc.emu_start(start, SENTINEL, count=max_steps + 2)
    except UcError as e:
        if state["stop"] is None:
            # control reached bytes Unicorn cannot decode (a literal pool, an
            # unrelocated target): a counted stop, not coverage.
            msg = str(e)
            state["stop"] = ST_BADCODE if "Invalid instruction" in msg else ST_UNMAPPED
            state["why"] = "UcError: %s" % msg
    if state["stop"] is None:
        # emu_start(until=SENTINEL) stops at the return sentinel WITHOUT firing
        # the code hook, so decide from the final PC rather than assume LIMIT.
        state["stop"] = ST_RETURN if uc.reg_read(UC_ARM_REG_PC) == SENTINEL else ST_LIMIT
    return state


# --------------------------------------------------------------------------
# Function inventory (symbols preferred; mapping symbols / low-bit fallback)
# --------------------------------------------------------------------------
def inventory(img: LibImage):
    """Return (entries, method) where entries is list of (addr, thumb)."""
    try:
        img_elf = T.parse_elf(img.data)
    except Exception as e:                       # noqa: BLE001
        return [], "error:%s" % e
    funcs = [s for s in img_elf.symbols
             if s.sym_type == T.STT_FUNC and s.value
             and s.shndx not in (0, "SHN_UNDEF")]
    # only keep entries that land inside an executable range
    ents = {}
    for s in funcs:
        a = s.value & ~1
        if img.in_exec(a):
            ents.setdefault(a, bool(s.value & 1))
    entries = sorted(ents.items())
    if entries:
        return entries, "func_symbols"
    # mapping-symbol fallback
    maps = T._mapping_symbols(img_elf)
    for a, mode in maps:
        if img.in_exec(a) and mode in (T.MODE_A32, T.MODE_T32):
            ents.setdefault(a, mode == T.MODE_T32)
    entries = sorted(ents.items())
    if entries:
        return entries, "mapping_symbols"
    return [], "none"


def sample(entries, n):
    if n <= 0 or len(entries) <= n:
        return entries
    step = len(entries) / float(n)
    return [entries[int(i * step)] for i in range(n)]


# --------------------------------------------------------------------------
# Per-library dynamic analysis
# --------------------------------------------------------------------------
def analyze_library(data, name, eng, interp, per_lib, max_steps, iwindow=32768):
    img = LibImage(data, name)
    entries, method = inventory(img)
    chosen = sample(entries, per_lib)

    agg = {
        "name": name, "method": method, "functions_total": len(entries),
        "functions_run": 0, "instructions": 0,
        "cls": collections.Counter(), "mne": collections.Counter(),
        "mode": collections.Counter(), "enc": collections.Counter(),
        "stop": collections.Counter(), "invalid": 0,
        # interpreter pass on the same functions
        "istop": collections.Counter(), "insn_exec_by_interp": 0,
        "interp_refused": collections.Counter(),   # fault name -> count
        # the region-mapped image pass (a32_interp_run_image): real coverage
        "istop_image": collections.Counter(),
        "img_refused": collections.Counter(),      # refused encoding by name
        "img_other": collections.Counter(),        # other named fault by name
        "img_left": collections.Counter(),         # left-library, by kind
        "automap_total": 0,                        # lenient data pages mapped
    }

    if interp is not None:
        interp.set_image(build_image(img))

    for (faddr, thumb) in chosen:
        st = run_function(eng, img, faddr, thumb, max_steps)
        agg["functions_run"] += 1
        agg["instructions"] += st["n"]
        agg["cls"].update(st["cls"])
        agg["mne"].update(st["mne"])
        agg["mode"].update(st["mode"])
        agg["enc"].update(st["enc"])
        agg["stop"][st["stop"]] += 1
        agg["invalid"] += st["invalid"]
        agg["automap_total"] += st["automap_total"]

        # ---- interpreter pass on the same function window ----------------
        if interp is not None:
            code = img.exec_bytes_from(faddr & ~1, iwindow)
            if code:
                rc, stop, err, _ = interp.run(code, 1 if thumb else 0)
                if rc == 0:
                    agg["istop"]["ran_to_end"] += 1
                elif UNIMPL_MARK in err:
                    agg["istop"]["refused_encoding"] += 1
                    reason = err.split(": ", 1)[-1].split(" at pc=", 1)[0]
                    agg["interp_refused"][reason] += 1
                elif "branch left the fragment" in err:
                    agg["istop"]["left_fragment"] += 1
                elif "outside the arena" in err or "memory" in err:
                    agg["istop"]["mem_outside_arena"] += 1
                elif "step limit" in err:
                    agg["istop"]["step_limit"] += 1
                else:
                    agg["istop"]["other"] += 1

        # ---- the region-mapped image pass: run the function where it lives ---
        if interp is not None:
            cregs = [0] * 16
            cregs[0], cregs[1], cregs[2], cregs[3] = IMG_R0
            sp = STACK_BASE + STACK_SIZE - 0x100
            rc, stop, err, _ = interp.run_image(
                faddr & ~1, thumb, cregs, sp, SENTINEL, SENTINEL, IMAGE_STEP_LIMIT)
            if rc == 0 and stop == 0:
                agg["istop_image"]["return"] += 1
            elif "step limit" in err:
                agg["istop_image"]["step_limit"] += 1
            elif UNIMPL_MARK in err:
                agg["istop_image"]["refused_encoding"] += 1
                reason = err.split(": ", 1)[-1].split(" at pc=", 1)[0]
                agg["img_refused"][reason] += 1
            elif "outside the arena" in err or "executable regions" in err:
                # a data address no PT_LOAD contains, or a fetch into a
                # non-executable page: control left the library, or the library
                # touched memory we did not model. Counted, never coverage.
                agg["istop_image"]["left_library"] += 1
                agg["img_left"]["fetch_not_exec" if "executable regions" in err
                               else "data_unmapped"] += 1
            else:
                # everything else is a NAMED fault (an unimplemented encoding the
                # classifier above did not match, an unsupported architectural
                # form); name it so the report says what still stops.
                agg["istop_image"]["other"] += 1
                reason = (err.split(": ", 1)[-1].split(" at pc=", 1)[0]
                          if err else "no reason")
                agg["img_other"][reason] += 1
    return agg


# --------------------------------------------------------------------------
# Aggregate + report
# --------------------------------------------------------------------------
def merge(dst, src, keys):
    for k in keys:
        dst[k].update(src[k])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apk-dir", default="Games32")
    ap.add_argument("--per-lib", type=int, default=1500,
                    help="max functions executed per library (sampled by address)")
    ap.add_argument("--max-steps", type=int, default=1200,
                    help="max instructions executed per function")
    ap.add_argument("--json", default=".hermes/plans/engine-reset/arm32-dynamic-mix.json")
    # raw machine report; the curated arm32-dynamic-mix.md is authored on top of it
    ap.add_argument("--md", default=".hermes/plans/engine-reset/arm32-dynamic-mix.raw.md")
    ap.add_argument("--only", default=None)
    ap.add_argument("--no-interp", action="store_true")
    args = ap.parse_args()

    eng = Engines()
    interp = None
    if not args.no_interp:
        dll = build_shim()
        if dll:
            interp = Interp(dll)
            print("[dynmix] interpreter shim: %s" % dll)
        else:
            print("[dynmix] WARNING: no C compiler -- interpreter pass disabled")

    apks = sorted(f for f in os.listdir(args.apk_dir) if f.endswith(".apk"))
    if args.only:
        apks = [a for a in apks if args.only in a]

    lib_rows = []
    A = {k: collections.Counter() for k in
         ("cls", "mne", "mode", "enc", "stop", "istop", "interp_refused",
          "istop_image", "img_refused", "img_other", "img_left")}
    A["instructions"] = 0
    A["functions_run"] = 0
    A["functions_total"] = 0
    A["invalid"] = 0
    A["automap_total"] = 0

    print("capstone %s  pyelftools=yes  engine=%s" % (cs.__version__, "unicorn"))
    for apk in apks:
        with zipfile.ZipFile(os.path.join(args.apk_dir, apk)) as z:
            names = [n for n in z.namelist()
                     if T.ABI_KEEP.match(n) and n.endswith(".so")]
            for n in sorted(names):
                data = z.read(n)
                try:
                    row = analyze_library(data, n, eng, interp,
                                          args.per_lib, args.max_steps)
                except Exception as e:                      # noqa: BLE001
                    print("[%s] %s: ERROR %s: %s" % (apk, n, type(e).__name__, e))
                    continue
                row["apk"] = apk
                lib_rows.append(row)
                merge(A, row, ("cls", "mne", "mode", "enc", "stop", "istop",
                               "interp_refused", "istop_image", "img_refused",
                               "img_other", "img_left"))
                A["instructions"] += row["instructions"]
                A["functions_run"] += row["functions_run"]
                A["functions_total"] += row["functions_total"]
                A["invalid"] += row["invalid"]
                A["automap_total"] += row["automap_total"]
                print("[%s] %s: method=%s funcs %d/%d insns=%d stops=%s"
                      % (apk, n, row["method"], row["functions_run"],
                         row["functions_total"], row["instructions"],
                         dict(row["stop"])))
                sys.stdout.flush()

    # ---- refused-encoding queue (dynamic weighted) ----
    refused = collections.Counter()      # human label -> dynamic count
    refused_mne = collections.Counter()
    supported_insns = 0
    total_insns = A["instructions"]
    for (mode, size, word), cnt in A["enc"].items():
        raw = word.to_bytes(size, "little")
        # decode for a label regardless of interpreter availability
        md = eng.arm if mode == "A32" else eng.thumb
        ins = next(md.disasm(raw, 0, count=1), None)
        mne = ins.mnemonic if ins else "?%08x" % word
        if interp is not None:
            is_ref, reason = interp.refuses(mode, raw)
        else:
            is_ref, reason = False, ""
        if is_ref:
            label = "%s (%s%s)" % (mne, reason or "unimplemented", "")
            refused[label] += cnt
            refused_mne[mne] += cnt
        else:
            supported_insns += cnt

    json_out = {
        "capstone": cs.__version__,
        "engine": "unicorn",
        "interpreter": "a32_interp.c (unchanged, via a32_shim.c DLL)" if interp else None,
        "per_lib": args.per_lib, "max_steps": args.max_steps,
        "totals": {
            "functions_run": A["functions_run"],
            "functions_total": A["functions_total"],
            "instructions_executed": total_insns,
            "classification": dict(A["cls"]),
            "classification_pct": {k: round(100.0 * A["cls"].get(k, 0) / max(1, total_insns), 4)
                                   for k in CLASS_ORDER},
            "mode": dict(A["mode"]),
            "stop_reasons": dict(A["stop"]),
            "interp_stop_reasons": dict(A["istop"]),
            "interp_refused_by_name": dict(A["interp_refused"].most_common(60)),
            "interp_image_stop_reasons": dict(A["istop_image"]),
            "interp_image_refused_by_name": dict(A["img_refused"].most_common(40)),
            "interp_image_other_faults": dict(A["img_other"].most_common(40)),
            "interp_image_left_library": dict(A["img_left"]),
            "standalone_functions_returned": A["istop_image"].get("return", 0),
            "standalone_return_fraction": round(
                100.0 * A["istop_image"].get("return", 0) / max(1, A["functions_run"]), 4),
            "top_mnemonics": A["mne"].most_common(40),
            "refused_dynamic": refused.most_common(60),
            "refused_by_mnemonic": refused_mne.most_common(40),
            "supported_instructions": supported_insns,
            "refused_instructions": total_insns - supported_insns,
            "classified_instructions": sum(A["cls"].values()),
            "interp_support_fraction": round(
                100.0 * supported_insns / max(1, sum(A["cls"].values())), 4),
            "invalid_decodes": A["invalid"],
            "lenient_mem_pages_mapped": A["automap_total"],
        },
        "libraries": [{
            "apk": r["apk"], "name": r["name"], "method": r["method"],
            "functions_total": r["functions_total"], "functions_run": r["functions_run"],
            "instructions": r["instructions"],
            "classification": dict(r["cls"]), "mode": dict(r["mode"]),
            "stop_reasons": dict(r["stop"]),
            "interp_stop_reasons": dict(r["istop"]),
            "interp_image_stop_reasons": dict(r["istop_image"]),
            "automap_pages": r["automap_total"],
        } for r in lib_rows],
    }
    os.makedirs(os.path.dirname(args.json), exist_ok=True)
    with open(args.json, "w", encoding="utf-8") as fh:
        json.dump(json_out, fh, indent=2)
    print("\nJSON written: %s" % args.json)

    write_markdown(args.md, json_out)
    print("MARKDOWN written: %s" % args.md)


def write_markdown(path, j):
    t = j["totals"]
    L = []
    w = L.append
    ins = t["instructions_executed"]
    pct = t["classification_pct"]
    w("# Dynamic AArch32 instruction mix (nine 32-bit-only games)\n")
    w("Generated by `tools/a32/dynmix.py`. **Dynamic measurement**: real functions "
      "out of the armhf libraries in `Games32/` are *executed* and their retired "
      "instructions counted. This is the counterpart to the static "
      "`arm32-traffic.md` histogram.\n")
    w("## Method\n")
    w("- Each `lib/armeabi*/**.so` is extracted from its APK and its ELF parsed with "
      "the same reader `tools/a32/traffic.py` uses.\n"
      "- Function boundaries come from the library's own symbol table "
      "(`STT_FUNC`, Thumb entry = odd address) where present; where stripped, "
      "the symbol-low-bit / mapping-symbol fallback is used and flagged.\n"
      "- Each function is run under **Unicorn** (the engine class the differential "
      "harness uses as its oracle) in a faithful map of the library's `PT_LOAD` "
      "segments, `SP` on a scratch stack, `LR` = a mapped return sentinel, and "
      "**r0-r3 = 0** (neutral: no fabricated arguments -- zero is the honest choice "
      "when we do not know a call's real inputs).\n"
      "- Execution is **single-stepped**: every instruction is classified with "
      "capstone. It STOPS, and the stop is counted, when control leaves the "
      "library's code (an import / PLT entry / any call out of the library), "
      "touches unmapped memory, hits a step cap, or returns to the sentinel.\n"
      "- Separately the **A32 reference interpreter itself** (`a32_interp.c`, "
      "compiled unchanged via `a32_shim.c`) is asked of every distinct executed "
      "encoding whether it decodes it or REFUSES it by name, and is run on each "
      "function window to see how far it gets.\n")

    w("## 1. What ran\n")
    w("| metric | value |")
    w("|--------|-------|")
    w("| functions executed | %d |" % t["functions_run"])
    w("| functions available (symbols) | %d |" % t["functions_total"])
    w("| instructions executed (classified) | %d |" % ins)
    w("| undecodable bytes encountered | %d |" % t["invalid_decodes"])
    w("| lenient data pages zero-filled | %d |" % t.get("lenient_mem_pages_mapped", 0))
    w("| interpreter supports (dynamic) | %.2f%% |" % t["interp_support_fraction"])
    w("")

    w("## 2. Dynamic class mix\n")
    w("| class | instructions | share |")
    w("|-------|--------------|-------|")
    for k in CLASS_ORDER:
        w("| %s | %d | %.2f%% |" % (k, t["classification"].get(k, 0), pct[k]))
    w("| **total** | **%d** | 100.00%% |" % ins)
    w("")
    w("Mode mix (A32 vs T32, and wide Thumb-2 split out): "
      + ", ".join("`%s`=%d" % kv for kv in sorted(t["mode"].items())) + "\n")
    w("### Top mnemonics executed\n")
    w("| # | mnemonic | dynamic count |")
    w("|---|----------|---------------|")
    for i, (m, c) in enumerate(t["top_mnemonics"], 1):
        w("| %d | `%s` | %d |" % (i, m, c))
    w("")

    w("## 3. Where execution stopped (the coverage caveat)\n")
    w("| stop reason | functions |")
    w("|-------------|-----------|")
    for k, c in sorted(t["stop_reasons"].items(), key=lambda kv: -kv[1]):
        w("| %s | %d |" % (k, c))
    w("")
    w("Interpreter pass on the same functions (how far the reference interpreter "
      "gets):\n")
    w("| interpreter outcome | functions |")
    w("|---------------------|-----------|")
    for k, c in sorted(t["interp_stop_reasons"].items(), key=lambda kv: -kv[1]):
        w("| %s | %d |" % (k, c))
    w("")
    w("### The region-mapped image run (standalone coverage)\n")
    w("The same functions run through `a32_interp_run_image` with the library "
      "mapped at its **real PT_LOAD virtual addresses** (plus a scratch stack, a "
      "zero-filled scratch region r0-r3 point into, and a return sentinel in lr). "
      "This is the number that separates \"decodes real code\" from \"runs it\": a "
      "function the single 0x10000 arena stops at the first data access now runs "
      "to a `bx lr` and is counted as covering.\n")
    w("| image outcome | functions |")
    w("|---------------|-----------|")
    for k, c in sorted(t.get("interp_image_stop_reasons", {}).items(), key=lambda kv: -kv[1]):
        w("| %s | %d |" % (k, c))
    w("")
    w("**Standalone coverage: %d of %d functions ran to a return (%.2f%%)** -- "
      "against %d that ran to a return under the single-arena pass.\n"
      % (t.get("standalone_functions_returned", 0), t["functions_run"],
         t.get("standalone_return_fraction", 0.0),
         t.get("interp_stop_reasons", {}).get("ran_to_end", 0)))
    if t.get("interp_image_refused_by_name"):
        w("Image-run refusals by name (a run that decodes cleanly but hits an "
          "unimplemented encoding still faults, never silently covers):\n")
        w("| # | refused encoding | functions |")
        w("|---|------------------|-----------|")
        for i, (lab, c) in enumerate(t["interp_image_refused_by_name"].items(), 1):
            w("| %d | `%s` | %d |" % (i, lab, c))
        w("")
    if t.get("interp_image_other_faults"):
        w("Image-run other named faults (the run stopped with a name, not a "
          "wrong answer):\n")
        w("| # | fault | functions |")
        w("|---|-------|-----------|")
        for i, (lab, c) in enumerate(t["interp_image_other_faults"].items(), 1):
            w("| %d | `%s` | %d |" % (i, lab, c))
        w("")
    if t.get("interp_image_left_library"):
        w("Why the left-library stops happened: "
          + ", ".join("`%s`=%d" % kv for kv in sorted(t["interp_image_left_library"].items()))
          + " (a data address no PT_LOAD contains, vs a fetch into a page that "
            "is not executable).\n")

    w("## 4. Encodings the interpreter REFUSES, by dynamic frequency\n")
    w("Each distinct encoding the real code executes is probed against the "
      "reference interpreter's own decoder; this is the ranked work queue.\n")
    w("| # | encoding (mnemonic, fault) | dynamic count |")
    w("|---|----------------------------|---------------|")
    for i, (lab, c) in enumerate(t["refused_dynamic"], 1):
        w("| %d | `%s` | %d |" % (i, lab, c))
    w("")
    w("By mnemonic: " + ", ".join("`%s`=%d" % kv for kv in t["refused_by_mnemonic"]) + "\n")

    w("## 5. Per-library\n")
    w("| APK | library | method | funcs run/total | insns | integer | vfp | neon | T32 |")
    w("|-----|---------|--------|-----------------|-------|---------|-----|------|-----|")
    for r in j["libraries"]:
        c = r["classification"]
        w("| %s | `%s` | %s | %d/%d | %d | %d | %d | %d | %d |" % (
            r["apk"], r["name"], r["method"], r["functions_run"],
            r["functions_total"], r["instructions"], c.get("integer", 0),
            c.get("vfp", 0), c.get("neon", 0), r["mode"].get("T32", 0)))
    w("")
    w("---\n*Every number comes from the run recorded in `arm32-dynamic-mix.json`; "
      "no counts are estimated or fabricated. Functions that could not run "
      "meaningfully are counted as stops, never as coverage.*")

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L))


if __name__ == "__main__":
    main()
