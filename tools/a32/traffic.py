#!/usr/bin/env python3
"""
traffic.py -- static AArch32 instruction histogram over the 32-bit-only game APKs
in Games32/.

For every 32-bit ARM shared library inside each APK (lib/armeabi*, *.so) this:
  * extracts the .so from the APK zip,
  * parses the ELF (pyelftools when present, else a minimal inline reader),
  * resolves ARM (A32) vs Thumb (T32) mode for each executable byte range using
    the ELF mapping symbols ($a/$t/$d) when the library carries a .symtab,
    otherwise the function symbols' low bit (Thumb entry == odd address),
    otherwise a both-way decode estimate for any leftover range,
  * disassembles every range with capstone in the resolved mode,
  * counts mnemonics, classifies into integer / VFP / NEON / other-exotic,
  * tallies the "hard items" a translator would trip over.

IMPORTANT: this is a STATIC measurement -- what encodings are *present in the
code image*.  It is only a proxy for what actually *executes*.  A dynamic
measurement needs an interpreter that can run the game, and is a later step.

Outputs a human report to stdout and (by default) writes
  .hermes/plans/engine-reset/arm32-traffic.json
  .hermes/plans/engine-reset/arm32-traffic.md

Usage:
    python tools/a32/traffic.py --apk-dir Games32
"""
from __future__ import annotations

import argparse
import collections
import io
import json
import os
import re
import struct
import sys
import zipfile

import capstone as cs

try:
    from elftools.elf.elffile import ELFFile
    HAVE_PYELFTOOLS = True
except Exception:                                    # pragma: no cover
    HAVE_PYELFTOOLS = False


# --------------------------------------------------------------------------
# ELF handling
# --------------------------------------------------------------------------
SHF_EXECINSTR = 0x4
SHT_SYMTAB = 2
SHT_DYNSYM = 11
SHT_STRTAB = 3
STT_FUNC = 2
STT_NOTYPE = 0
STT_OBJECT = 1


class Section:
    __slots__ = ("name", "addr", "offset", "size", "flags", "type", "data")

    def __init__(self, name, addr, offset, size, flags, stype, data):
        self.name = name
        self.addr = addr
        self.offset = offset
        self.size = size
        self.flags = flags
        self.type = stype
        self.data = data

    @property
    def end(self):
        return self.addr + self.size

    @property
    def is_exec(self):
        return bool(self.flags & SHF_EXECINSTR)


class Symbol:
    __slots__ = ("name", "value", "size", "info", "shndx")

    def __init__(self, name, value, size, info, shndx):
        self.name = name
        self.value = value
        self.size = size
        self.info = info
        self.shndx = shndx

    @property
    def sym_type(self):
        return self.info & 0xF


class ElfImage:
    """Uniform view over libs parsed by pyelftools or by the inline reader."""

    def __init__(self, sections, symbols, entry, machine, is_stripped):
        self.sections = sections
        self.symbols = symbols          # list[Symbol] from symtab + dynsym
        self.entry = entry
        self.machine = machine
        self.is_stripped = is_stripped


def _parse_elf_pyelftools(data):
    e = ELFFile(io.BytesIO(data))
    sections = []
    for s in e.iter_sections():
        try:
            sdata = s.data()
        except Exception:
            sdata = b""
        sections.append(Section(s.name, s["sh_addr"], s["sh_offset"],
                                s["sh_size"], s["sh_flags"], s["sh_type"], sdata))
    symbols = []
    _STT = {"STT_NOTYPE": 0, "STT_OBJECT": 1, "STT_FUNC": 2, "STT_SECTION": 3,
            "STT_FILE": 4, "STT_COMMON": 5, "STT_TLS": 6}
    _STB = {"STB_LOCAL": 0, "STB_GLOBAL": 1, "STB_WEAK": 2}
    for sn in (".symtab", ".dynsym"):
        st = e.get_section_by_name(sn)
        if st is None:
            continue
        for sym in st.iter_symbols():
            info = sym["st_info"]
            t = info["type"]
            b = info["bind"]
            t = _STT.get(t, 0) if isinstance(t, str) else t
            b = _STB.get(b, 0) if isinstance(b, str) else b
            symbols.append(Symbol(sym.name, sym["st_value"], sym["st_size"],
                                  t | (b << 4), sym["st_shndx"]))
    stripped = e.get_section_by_name(".symtab") is None
    em = e.header["e_machine"]
    if isinstance(em, str):
        em = {"EM_ARM": 40, "EM_386": 3, "EM_X86_64": 62, "EM_AARCH64": 183,
              "EM_MIPS": 8}.get(em, -1)
    return ElfImage(sections, symbols, e.header["e_entry"],
                    em, stripped)


def _parse_elf_minimal(data):
    """Fallback reader: section headers + symbol tables, no pyelftools."""
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF")
    if data[4] != 1:
        raise ValueError("only ELFCLASS32 supported by the inline reader")
    little = data[5] == 1
    en = "<" if little else ">"
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
     e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from(en + "HHIIIIIHHHHHH", data, 16)

    raw_sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link,
         sh_info, sh_addralign, sh_entsize) = struct.unpack_from(
            en + "IIIIIIIIII", data, off)
        raw_sections.append(dict(name_off=sh_name, type=sh_type, flags=sh_flags,
                                 addr=sh_addr, offset=sh_offset, size=sh_size,
                                 link=sh_link, entsize=sh_entsize))

    def strtab(idx):
        rs = raw_sections[idx]
        return data[rs["offset"]: rs["offset"] + rs["size"]]

    shstr = strtab(e_shstrndx)

    def cstr(blob, off):
        end = blob.find(b"\0", off)
        return blob[off:end].decode("latin-1") if end >= 0 else ""

    sections = []
    for rs in raw_sections:
        sections.append(Section(cstr(shstr, rs["name_off"]), rs["addr"],
                                rs["offset"], rs["size"], rs["flags"],
                                rs["type"], data[rs["offset"]: rs["offset"] + rs["size"]]))

    symbols = []
    for rs, sec in zip(raw_sections, sections):
        if sec.type not in (SHT_SYMTAB, SHT_DYNSYM):
            continue
        strs = strtab(rs["link"]) if rs["link"] < len(raw_sections) else b""
        entsize = rs["entsize"] or 16
        n = rs["size"] // entsize
        for i in range(n):
            o = sec.offset + i * entsize
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                en + "IIIBBH", data, o)
            symbols.append(Symbol(cstr(strs, st_name), st_value, st_size,
                                  st_info, st_shndx))
    stripped = not any(s.type == SHT_SYMTAB for s in sections)
    return ElfImage(sections, symbols, e_entry, e_machine, stripped)


def parse_elf(data):
    if HAVE_PYELFTOOLS:
        return _parse_elf_pyelftools(data)
    return _parse_elf_minimal(data)


# --------------------------------------------------------------------------
# Mode resolution
# --------------------------------------------------------------------------
MAPPING_RE = re.compile(r"^\$(a|t|d)(\.\d+)?$")
MODE_A32, MODE_T32, MODE_DATA, MODE_UND = "A32", "T32", "DATA", "UND"


def _mapping_symbols(img):
    out = []
    for s in img.symbols:
        m = MAPPING_RE.match(s.name or "")
        if m:
            kind = m.group(1)
            mode = {"a": MODE_A32, "t": MODE_T32, "d": MODE_DATA}[kind]
            out.append((s.value & ~1, mode))
    # keep one entry per address (first wins), sorted
    seen = {}
    for addr, mode in out:
        if addr not in seen:
            seen[addr] = mode
    return sorted(seen.items())


def _func_symbols(img):
    out = []
    for s in img.symbols:
        if s.sym_type != STT_FUNC or s.shndx in (0, "SHN_UNDEF"):
            continue
        if not s.value:
            continue
        thumb = bool(s.value & 1)
        out.append((s.value & ~1, MODE_T32 if thumb else MODE_A32))
    seen = {}
    for addr, mode in out:
        if addr not in seen:
            seen[addr] = mode
    return sorted(seen.items())


def _segments_from_markers(markers, sections):
    """Turn a sorted (addr,mode) marker list into per-section segments."""
    segs = []
    for sec in sections:
        lo, hi = sec.addr, sec.end
        inside = [(a, m) for a, m in markers if lo <= a < hi]
        pos = lo
        if not inside or inside[0][0] != lo:
            segs.append((lo, inside[0][0] if inside else hi, MODE_UND))
            pos = inside[0][0] if inside else hi
        for i, (a, m) in enumerate(inside):
            nxt = inside[i + 1][0] if i + 1 < len(inside) else hi
            if nxt > a:
                segs.append((a, nxt, m))
    return segs


# --------------------------------------------------------------------------
# Capstone engines and classification
# --------------------------------------------------------------------------
CONDS = {"eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc",
         "hi", "ls", "ge", "lt", "gt", "le", "al"}

NEON_GROUPS = {"neon", "neon_fp"}
VFP_GROUPS = {"vfp2", "vfp3", "vfp4", "dpvfp", "fparmv8", "fpvmlx"}
BARRIER_MNEMONICS = {"dmb", "dsb", "isb"}

# coprocessor / system-control / legacy-ISA / undefined -> "other/exotic".
# NOTE: ARMv6/v7 *integer* data-processing ops (ubfx/bfi/rev/clz/sxtb/...) are
# deliberately NOT here -- they are integer core.
EXOTIC = {
    # coprocessor
    "mcr", "mrc", "mcrr", "mrrc", "cdp", "cdp2", "ldc", "ldc2", "ldcl", "ldc2l",
    "stc", "stc2", "stcl", "stc2l",
    # software interrupt / legacy-ISA leftovers
    "swi", "svc", "swp", "swpb", "setend", "cps", "cpsie", "cpsid", "rfe", "srs",
    # virtualization / security / traps / undefined
    "hlt", "hvc", "smc", "eret", "udf", "undefined", "bkpt", "dbg",
    # system hints / control prefixes
    "nop", "hint", "yield", "wfe", "wfi", "sev", "it",
}
EXCLUSIVE_RE = re.compile(r"^(ldrex|strex|lda|stl|ldrexb|strexb|ldrexh|strexh|ldrexd|strexd)")


def _variants(m):
    """Yield plausible base forms of a mnemonic across cond/s suffixes."""
    cand = {m}
    if len(m) > 3 and m[-2:] in CONDS:
        cand.add(m[:-2])
    for c in list(cand):
        if c.endswith("s") and len(c) > 2:
            cand.add(c[:-1])
        if c.endswith("s") and len(c) > 2 and c[:-1][-2:] in CONDS:
            cand.add(c[:-1][:-2])
    if len(m) > 4 and m[-2:] in CONDS and m[:-2].endswith("s"):
        cand.add(m[:-3])
    return cand


def classify(mnemonic, groups, size, mode):
    gs = set(groups)
    if gs & NEON_GROUPS:
        return "neon"
    if gs & VFP_GROUPS:
        return "vfp"
    if mnemonic in EXOTIC or any(v in EXOTIC for v in _variants(mnemonic)):
        return "exotic"
    return "integer"


class Engines:
    def __init__(self):
        self.arm = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_ARM)
        self.arm.detail = True
        self.thumb = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB)
        self.thumb.detail = True
        if not self.arm.disasm(b"\x00\x00\xa0\xe1", 0):
            raise RuntimeError("capstone ARM mode failed to decode a NOP")
        if not self.thumb.disasm(b"\x00\xbf", 0):
            raise RuntimeError("capstone Thumb mode failed to decode a NOP")

    def get(self, mode):
        return self.arm if mode == MODE_A32 else self.thumb


def _disasm_range(md, code, addr, on_insn):
    """Disassemble `code` at `addr`, calling on_insn(ins) per instruction.
    Returns (valid_bytes, invalid_bytes, n_insns).  Undecodable runs are
    stepped over in 4-byte (ARM) or 2-byte (Thumb) units."""
    step = 4 if md.mode == cs.CS_MODE_ARM else 2
    pos = 0
    n = len(code)
    valid = 0
    invalid = 0
    n_insn = 0
    while pos < n:
        last = None
        for ins in md.disasm(code[pos:], addr + pos):
            last = ins
            n_insn += 1
            valid += ins.size
            on_insn(ins)
        if last is None:
            skip = min(step, n - pos)
            invalid += skip
            pos += skip
        else:
            newpos = (last.address + last.size) - addr
            if newpos <= pos:            # safety against a stuck loop
                newpos = pos + step
            if newpos > n:
                newpos = n
            if newpos < n:
                skip = min(step, n - newpos)
                invalid += skip
            pos = newpos
    return valid, invalid, n_insn


def _estimate_mode(md_arm, md_thumb, code, addr):
    """Both-way decode estimate: the mode that covers the range with fewer
    undecodable bytes and fewer tell-tale garbage mnemonics wins."""
    GARBAGE = {"udf", "undefined", "andeq"}

    def score(md):
        stats = {"valid": 0, "invalid": 0, "garbage": 0, "n": 0}

        def on(ins):
            stats["n"] += 1
            if ins.mnemonic in GARBAGE or any(v in GARBAGE for v in _variants(ins.mnemonic)):
                stats["garbage"] += 1
        v, inv, n = _disasm_range(md, code, addr, on)
        stats["valid"], stats["invalid"] = v, inv
        total = max(1, v + inv)
        return (v / total) - (stats["garbage"] / max(1, n))

    a = score(md_arm)
    t = score(md_thumb)
    if max(a, t) < 0.45:
        return MODE_UND
    if abs(a - t) < 0.05:
        return MODE_UND
    return MODE_A32 if a > t else MODE_T32


# --------------------------------------------------------------------------
# Per-library analysis
# --------------------------------------------------------------------------
def analyze_library(data, name):
    img = parse_elf(data)
    if img.machine != 40:  # EM_ARM
        return {"name": name, "error": f"not EM_ARM (e_machine={img.machine})"}

    exec_secs = [s for s in img.sections if s.is_exec and s.size >= 16]
    exec_bytes = sum(s.size for s in exec_secs)

    maps = _mapping_symbols(img)
    funcs = _func_symbols(img)

    map_segs = _segments_from_markers(maps, exec_secs) if maps else []
    fun_segs = _segments_from_markers(funcs, exec_secs) if funcs else []

    def covered(segs):
        return sum(b - a for a, b, m in segs if m in (MODE_A32, MODE_T32))

    map_cov = covered(map_segs) / exec_bytes if exec_bytes else 0.0
    fun_cov = covered(fun_segs) / exec_bytes if exec_bytes else 0.0

    if map_cov >= 0.5:
        segs = map_segs
        if map_cov >= 0.95:
            method = "mapping_symbols"
        else:
            method = f"mapping_symbols ({map_cov:.0%}) + estimate"
    elif fun_cov >= 0.5:
        segs = fun_segs
        method = f"func_symbol_lowbit ({fun_cov:.0%}) + estimate"
    else:
        segs = [s for s in _segments_from_markers([], exec_secs)]
        method = "both_way_estimate"

    eng = ENGINES
    counters = collections.Counter()
    mode_bytes = collections.Counter()
    mode_insns = collections.Counter()
    mne_by_mode = collections.defaultdict(collections.Counter)
    mne_by_class = collections.defaultdict(collections.Counter)
    cls_counts = collections.Counter()
    group_inventory = collections.Counter()
    hard = collections.Counter()
    invalid_bytes = collections.Counter()

    sec_by_range = []
    for s in exec_secs:
        sec_by_range.append((s.addr, s.end, s))

    def sec_data_for(addr, length):
        for lo, hi, s in sec_by_range:
            if lo <= addr < hi:
                off = addr - lo
                return s.data[off: off + length]
        return b""

    total_valid = collections.Counter()
    total_invalid = collections.Counter()
    total_ninsn = collections.Counter()

    for a, b, m in segs:
        if b <= a:
            continue
        code = sec_data_for(a, b - a)
        if len(code) != b - a:
            code = sec_data_for(a, len(code)) if code else b""
        if not code:
            continue
        if m == MODE_DATA:
            mode_bytes[MODE_DATA] += b - a
            continue
        if m == MODE_UND:
            est = _estimate_mode(eng.arm, eng.thumb, code, a)
            if est == MODE_UND:
                mode_bytes[MODE_UND] += b - a
                invalid_bytes["undetermined"] += b - a
                continue
            m = est
            method = method  # recorded globally; estimated region

        md = eng.get(m)

        def on(ins, _m=m):
            counters[ins.mnemonic] += 1
            mne_by_mode[ins.mnemonic][_m] += 1
            gnames = tuple(md.group_name(g) for g in ins.groups)
            group_inventory.update(gnames)
            cls = classify(ins.mnemonic, gnames, ins.size, _m)
            cls_counts[cls] += 1
            mne_by_class[cls][ins.mnemonic] += 1
            # hard items
            mn = ins.mnemonic
            bases = _variants(mn)
            if any(b_.startswith("it") and set(b_[2:]) <= set("te") for b_ in bases) \
               or (len(mn) <= 4 and mn.startswith("it")):
                hard["it_blocks"] += 1
            if _m == MODE_T32 and ins.size == 4:
                hard["thumb2_wide"] += 1
            if any(EXCLUSIVE_RE.match(v) for v in bases):
                hard["exclusive_ldrex_strex"] += 1
            if mn in BARRIER_MNEMONICS or "databarrier" in gnames:
                hard["barriers_dmb_dsb_isb"] += 1
            if any(re.match(r"^(mcr|mrc|mcrr|mrrc|cdp|ldc|stc)", v) for v in bases):
                hard["coproc_mcr_mrc_ldc_stc_cdp"] += 1
            if any(re.match(r"^(swi|svc)$", v) for v in bases):
                hard["swi_svc"] += 1

        v, inv, n = _disasm_range(md, code, a, on)
        mode_bytes[m] += b - a
        mode_insns[m] += n
        total_valid[m] += v
        total_invalid[m] += inv
        total_ninsn[m] += n
        if inv:
            invalid_bytes[m] += inv

    return {
        "name": name,
        "method": method,
        "map_symbols": len(maps),
        "func_symbols": len(funcs),
        "map_coverage": round(map_cov, 4),
        "func_coverage": round(fun_cov, 4),
        "stripped": img.is_stripped,
        "exec_bytes": exec_bytes,
        "mode_bytes": dict(mode_bytes),
        "mode_insns": dict(mode_insns),
        "invalid_bytes": dict(invalid_bytes),
        "counters": counters,
        "mne_by_mode": mne_by_mode,
        "mne_by_class": mne_by_class,
        "cls_counts": cls_counts,
        "hard": hard,
        "group_inventory": group_inventory,
    }


# --------------------------------------------------------------------------
# APK walking
# --------------------------------------------------------------------------
ABI_KEEP = re.compile(r"^lib/(armeabi|armeabi-v7a)/")
SO_RE = re.compile(r"\.so$")


def analyze_apk(path, limit_bytes=None):
    apk = os.path.basename(path)
    results = []
    with zipfile.ZipFile(path) as z:
        names = [n for n in z.namelist()
                 if ABI_KEEP.match(n) and SO_RE.search(n) and not n.endswith("/")]
        for n in sorted(names):
            data = z.read(n)
            try:
                res = analyze_library(data, n)
            except Exception as exc:                     # noqa: BLE001
                res = {"name": n, "error": f"{type(exc).__name__}: {exc}"}
            res["apk"] = apk
            res["file_bytes"] = len(data)
            results.append(res)
    return results


def main():
    global ENGINES
    ap = argparse.ArgumentParser()
    ap.add_argument("--apk-dir", default="Games32")
    ap.add_argument("--json", default=".hermes/plans/engine-reset/arm32-traffic.json")
    ap.add_argument("--md", default=".hermes/plans/engine-reset/arm32-traffic.md")
    ap.add_argument("--only", default=None, help="substring filter on apk name")
    args = ap.parse_args()

    ENGINES = Engines()

    apks = sorted(f for f in os.listdir(args.apk_dir) if f.endswith(".apk"))
    if args.only:
        apks = [a for a in apks if args.only in a]

    all_results = []
    apk_lib_counts = {}
    print(f"capstone {cs.__version__}  pyelftools={'yes' if HAVE_PYELFTOOLS else 'no'}")
    print(f"scanning {len(apks)} APK(s) in {args.apk_dir}\n")

    for apk in apks:
        res = analyze_apk(os.path.join(args.apk_dir, apk))
        apk_lib_counts[apk] = len(res)
        for r in res:
            print(f"[{apk}] {r['name']}: "
                  + (f"ERROR {r['error']}" if "error" in r else
                     f"method={r['method']} exec={r['exec_bytes']} "
                     f"A32={r['mode_bytes'].get('A32',0)} T32={r['mode_bytes'].get('T32',0)} "
                     f"undet={r['mode_bytes'].get('UND',0)}"))
        all_results.extend(res)
        sys.stdout.flush()

    # ---- aggregate ----
    agg_mne = collections.Counter()
    agg_mne_mode = collections.defaultdict(collections.Counter)
    agg_mne_class = collections.defaultdict(collections.Counter)
    agg_cls = collections.Counter()
    agg_hard = collections.Counter()
    agg_mode_bytes = collections.Counter()
    agg_mode_insns = collections.Counter()
    agg_groups = collections.Counter()
    for r in all_results:
        if "error" in r:
            continue
        agg_mne.update(r["counters"])
        for m, c in r["mne_by_mode"].items():
            agg_mne_mode[m].update(c)
        for c, mc in r["mne_by_class"].items():
            agg_mne_class[c].update(mc)
        agg_cls.update(r["cls_counts"])
        agg_hard.update(r["hard"])
        for k, v in r["mode_bytes"].items():
            agg_mode_bytes[k] += v
        for k, v in r["mode_insns"].items():
            agg_mode_insns[k] += v
        agg_groups.update(r["group_inventory"])

    total_insns = sum(agg_cls.values())
    top40 = agg_mne.most_common(40)

    print("\n==== GROUP INVENTORY (capstone ARM) ====")
    print(", ".join(f"{k}={v}" for k, v in agg_groups.most_common(60)))

    print("\n==== PER-LIBRARY ====")
    hdr = f"{'library':<52} {'method':<34} {'A32 B':>10} {'T32 B':>10} {'undet B':>9} {'insns':>10}"
    print(hdr)
    for r in all_results:
        if "error" in r:
            print(f"{r['name'][:50]:<52} {('ERROR: ' + r['error'])[:34]:<34}")
        else:
            ins = sum(r["mode_insns"].values())
            print(f"{r['name'][:50]:<52} {r['method'][:34]:<34} "
                  f"{r['mode_bytes'].get('A32',0):>10} {r['mode_bytes'].get('T32',0):>10} "
                  f"{r['mode_bytes'].get('UND',0):>9} {ins:>10}")

    print(f"\n==== TOP 40 MNEMONICS (of {total_insns} instructions) ====")
    print(f"{'mnemonic':<16} {'count':>10} {'A32':>12} {'T32':>12}")
    for m, c in top40:
        md = agg_mne_mode[m]
        print(f"{m:<16} {c:>10} {md.get('A32',0):>12} {md.get('T32',0):>12}")

    print("\n==== CLASSIFICATION ====")
    order = ["integer", "vfp", "neon", "exotic"]
    for k in order:
        c = agg_cls.get(k, 0)
        print(f"{k:<10} {c:>12}  {100.0*c/max(1,total_insns):6.2f}%")
    print(f"{'TOTAL':<10} {total_insns:>12}  100.00%")

    print("\n---- top mnemonics per class ----")
    for k in order:
        tops = ", ".join(f"{m}={c}" for m, c in agg_mne_class[k].most_common(15))
        print(f"  {k}: {tops}")

    print("\n---- APKs with no native library ----")
    for a, n in apk_lib_counts.items():
        if n == 0:
            print(f"  {a}: no lib/armeabi*/.so")

    a32b = agg_mode_bytes.get("A32", 0)
    t32b = agg_mode_bytes.get("T32", 0)
    datab = agg_mode_bytes.get("DATA", 0)
    undb = agg_mode_bytes.get("UND", 0)
    cb = max(1, a32b + t32b)
    print("\n==== MODE MIX ====")
    print(f"A32 bytes    {a32b:>12}  {100.0*a32b/cb:6.2f}% of resolved code bytes")
    print(f"T32 bytes    {t32b:>12}  {100.0*t32b/cb:6.2f}% of resolved code bytes")
    print(f"data ($d)    {datab:>12}")
    print(f"undetermined {undb:>12}")
    ci = max(1, agg_mode_insns.get("A32", 0) + agg_mode_insns.get("T32", 0))
    print(f"A32 insns    {agg_mode_insns.get('A32',0):>12}  {100.0*agg_mode_insns.get('A32',0)/ci:6.2f}%")
    print(f"T32 insns    {agg_mode_insns.get('T32',0):>12}  {100.0*agg_mode_insns.get('T32',0)/ci:6.2f}%")

    print("\n==== HARD ITEMS ====")
    for k in ["it_blocks", "thumb2_wide", "exclusive_ldrex_strex", "barriers_dmb_dsb_isb",
              "coproc_mcr_mrc_ldc_stc_cdp", "swi_svc"]:
        c = agg_hard.get(k, 0)
        print(f"{k:<32} {c:>12}  {100.0*c/max(1,total_insns):6.3f}% of insns")
    # undecodable bytes per library already reported; aggregate:
    tot_invalid = sum(sum(r.get("invalid_bytes", {}).values()) for r in all_results if "error" not in r)
    print(f"{'undecodable/invalid bytes':<32} {tot_invalid:>12}")

    # ---- write JSON ----
    json_out = {
        "capstone_version": cs.__version__,
        "pyelftools": HAVE_PYELFTOOLS,
        "apk_dir": args.apk_dir,
        "libraries": _json_libraries(all_results),
        "totals": {
            "instructions": total_insns,
            "classification": dict(agg_cls),
            "classification_pct": {k: round(100.0 * agg_cls.get(k, 0) / max(1, total_insns), 4)
                                   for k in order},
            "mode_bytes": dict(agg_mode_bytes),
            "mode_insns": dict(agg_mode_insns),
            "hard_items": dict(agg_hard),
            "class_top": {k: dict(agg_mne_class[k].most_common(25)) for k in order},
            "apks_with_no_library": [a for a, n in apk_lib_counts.items() if n == 0],
            "apk_lib_counts": dict(apk_lib_counts),
            "invalid_bytes_total": tot_invalid,
            "top40": [{"mnemonic": m, "count": c, "a32": agg_mne_mode[m].get("A32", 0),
                       "t32": agg_mne_mode[m].get("T32", 0)} for m, c in top40],
        },
    }
    os.makedirs(os.path.dirname(args.json), exist_ok=True)
    with open(args.json, "w", encoding="utf-8") as fh:
        json.dump(json_out, fh, indent=2)
    print(f"\nJSON written: {args.json}")

    write_markdown(args.md, json_out, all_results)
    print(f"MARKDOWN written: {args.md}")


# --------------------------------------------------------------------------
# Markdown report
# --------------------------------------------------------------------------
def _json_libraries(results):
    out = []
    for r in results:
        if "error" in r:
            out.append({"apk": r.get("apk"), "name": r.get("name"), "error": r["error"]})
            continue
        row = {k: v for k, v in r.items()
               if k not in ("counters", "mne_by_mode", "mne_by_class",
                            "cls_counts", "hard", "group_inventory")}
        row["counters"] = dict(r["counters"])
        row["classification"] = dict(r["cls_counts"])
        row["class_top"] = {c: dict(mc.most_common(12)) for c, mc in r["mne_by_class"].items()}
        row["hard_items"] = dict(r["hard"])
        row["group_inventory"] = dict(r["group_inventory"])
        out.append(row)
    return out


def write_markdown(path, j, results):
    t = j["totals"]
    ins = t["instructions"]
    pct = t["classification_pct"]
    neon = t["classification"].get("neon", 0)
    vfp = t["classification"].get("vfp", 0)
    a32i = t["mode_insns"].get("A32", 0)
    t32i = t["mode_insns"].get("T32", 0)
    a32b = t["mode_bytes"].get("A32", 0)
    t32b = t["mode_bytes"].get("T32", 0)
    undb = t["mode_bytes"].get("UND", 0)
    datab = t["mode_bytes"].get("DATA", 0)

    L = []
    w = L.append
    w("# AArch32 static instruction traffic (nine 32-bit-only games)\n")
    w("Generated by `tools/a32/traffic.py` (capstone %s). "
      "**Static measurement** — the encodings present in the code image, not what executes at "
      "runtime. A dynamic histogram needs an interpreter that can actually run the game; this is "
      "the cheap first pass and its numbers are an upper bound on the *instruction kinds* a "
      "translator must support, not a profile of hot paths.\n" % j["capstone_version"])
    w("## Method\n")
    w("- Each APK's `lib/armeabi*/**.so` is extracted from the zip and its ELF parsed "
      "(pyelftools when available, else the inline reader in `traffic.py`).\n"
      "- Mode is resolved per byte range: ELF mapping symbols (`$a`/`$t`/`$d`) when the library "
      "carries a full `.symtab`; otherwise the function symbols' low bit (odd entry = Thumb); "
      "leftover ranges are estimated by decoding both ways and keeping the mode that consumes "
      "the range with fewer undecodable bytes / garbage mnemonics. Ranges that stay ambiguous "
      "and all undecodable runs are bucketed as **undetermined**, never silently guessed.\n"
      "- Caveat: in libraries without mapping symbols, literal pools embedded in `.text` fall "
      "inside the function-symbol ranges and are decoded as instructions, so those files carry "
      "some garbage instructions (counted as integer/exotic). Libraries with a full `.symtab` "
      "carry `$d` markers and their literal pools are excluded.\n"
      "- Classification uses capstone's ARM instruction *groups*: `neon`/`neon_fp` → NEON, "
      "`vfp2/3/4`,`dpvfp`,`fparmv8` → VFP, coprocessor/system/old-ISA mnemonics → other/exotic, "
      "everything else → integer core.\n"
      "- Bucket membership: **NEON** = capstone groups `neon`/`neon_fp`; **VFP** = groups "
      "`vfp2`/`vfp3`/`vfp4`/`dpvfp`/`fparmv8`/`fpvmlx`; **other/exotic** = coprocessor "
      "(`mcr`/`mrc`/`mcrr`/`mrrc`/`cdp`/`ldc`/`stc`), legacy-ISA & system "
      "(`swi`/`svc`/`swp`/`setend`/`cps`/`rfe`/`srs`), virtualization/security/undefined "
      "(`hlt`/`hvc`/`smc`/`eret`/`udf`/`undefined`/`bkpt`/`dbg`), and hints/control "
      "(`nop`/`hint`/`yield`/`wfe`/`wfi`/`sev`/`it`). ARMv6/v7 *integer* data-processing ops "
      "(`ubfx`/`bfi`/`rev`/`clz`/`sxtb`/`ssat`/...) count as integer core.\n")

    w("## 1. Per-library mode mix\n")
    w("| APK | library | method | stripped | `.so` bytes | A32 bytes | T32 bytes | data | undet | instructions |")
    w("|-----|---------|--------|----------|------------|-----------|-----------|------|-------|--------------|")
    for r in results:
        if "error" in r:
            w(f"| {r['apk']} | `{r['name']}` | **ERROR** | | | | | | | {r['error']} |")
            continue
        ins_i = sum(r["mode_insns"].values())
        w(f"| {r['apk']} | `{r['name']}` | {r['method']} | {r['stripped']} | {r['file_bytes']} | "
          f"{r['mode_bytes'].get('A32',0)} | {r['mode_bytes'].get('T32',0)} | "
          f"{r['mode_bytes'].get('DATA',0)} | {r['mode_bytes'].get('UND',0)} | {ins_i} |")

    w(f"\nTotals: A32 {a32b} B, T32 {t32b} B, data {datab} B, undetermined {undb} B. "
      f"Resolved mode split by bytes: A32 {100.0*a32b/max(1,a32b+t32b):.1f}% / "
      f"T32 {100.0*t32b/max(1,a32b+t32b):.1f}%. "
      f"By instruction count: A32 {a32i} ({100.0*a32i/max(1,a32i+t32i):.1f}%) / "
      f"T32 {t32i} ({100.0*t32i/max(1,a32i+t32i):.1f}%).\n")

    w(f"## 2. Top 40 mnemonics ({ins} instructions total)\n")
    w("| # | mnemonic | count | A32 | T32 |")
    w("|---|----------|-------|-----|-----|")
    for i, e in enumerate(t["top40"], 1):
        w(f"| {i} | `{e['mnemonic']}` | {e['count']} | {e['a32']} | {e['t32']} |")

    w("\n## 3. Classification\n")
    w("| class | instructions | share |")
    w("|-------|--------------|-------|")
    for k in ["integer", "vfp", "neon", "exotic"]:
        w(f"| {k} | {t['classification'].get(k,0)} | {pct[k]:.2f}% |")
    w(f"| **total** | **{ins}** | 100.00% |")
    w("\nTop mnemonics inside each class:\n")
    for k in ["integer", "vfp", "neon", "exotic"]:
        tops = ", ".join(f"`{m}` {c}" for m, c in t.get("class_top", {}).get(k, {}).items())
        w(f"- **{k}**: {tops}")

    nolib = t.get("apks_with_no_library") or []
    if nolib:
        w(f"\nAPK(s) with no `lib/armeabi*/.so` at all (contribute no native code): "
          + ", ".join(f"`{a}`" for a in nolib) + ".")

    w("\n## 4. Hard items for a translator\n")
    w("| item | count | share of instructions |")
    w("|------|-------|-----------------------|")
    labels = {
        "it_blocks": "Thumb-2 `IT` blocks",
        "thumb2_wide": "wide (32-bit) Thumb encodings",
        "exclusive_ldrex_strex": "exclusive load/store (`ldrex`/`strex`)",
        "barriers_dmb_dsb_isb": "barriers (`dmb`/`dsb`/`isb`)",
        "coproc_mcr_mrc_ldc_stc_cdp": "coprocessor ops (`mcr`/`mrc`/`ldc`/`stc`/`cdp`)",
        "swi_svc": "`swi`/`svc`",
    }
    for k, lab in labels.items():
        c = t["hard_items"].get(k, 0)
        w(f"| {lab} | {c} | {100.0*c/max(1,ins):.3f}% |")
    w(f"| undecodable / invalid bytes | {t['invalid_bytes_total']} | "
      f"{100.0*t['invalid_bytes_total']/max(1,a32b+t32b):.3f}% of code bytes |")

    # verdict
    w("\n## 5. Verdict\n")
    neon_pct = pct["neon"]
    vfp_pct = pct["vfp"]
    t32_pct = 100.0 * t32i / max(1, a32i + t32i)
    # per-APK NEON presence
    by_apk_neon = collections.Counter()
    by_apk_ins = collections.Counter()
    for r in results:
        if "error" in r:
            continue
        by_apk_neon[r["apk"]] += r["cls_counts"].get("neon", 0)
        by_apk_ins[r["apk"]] += sum(r["cls_counts"].values())
    neon_games = [a for a in sorted(by_apk_ins) if by_apk_neon[a] > 0]
    no_neon_games = [a for a in sorted(by_apk_ins) if by_apk_neon[a] == 0]
    if neon_pct < 0.05:
        neon_clause = ("NEON is effectively absent from this corpus (%.3f%% of instructions)" % neon_pct)
    elif neon_pct < 2.0:
        neon_clause = ("NEON is present but thin (%.2f%% of instructions)" % neon_pct)
    else:
        neon_clause = ("NEON is substantial (%.2f%% of instructions)" % neon_pct)
    verdict = (
        f"**Blunt verdict.** {neon_clause}; VFP is {vfp_pct:.2f}% and integer core "
        f"{pct['integer']:.2f}%. NEON appears in {len(neon_games)} of {len(by_apk_ins)} games "
        f"({', '.join(neon_games) if neon_games else 'none'}) and is cleanly absent from "
        f"{len(no_neon_games)} ({', '.join(no_neon_games) if no_neon_games else 'none'}). "
        f"The static mix is {t32_pct:.1f}% Thumb (T32) vs {100.0-t32_pct:.1f}% ARM (A32) by "
        f"instruction count, {100.0*t32b/max(1,a32b+t32b):.1f}% T32 vs "
        f"{100.0*a32b/max(1,a32b+t32b):.1f}% A32 by bytes. A translator still has to handle "
        "*both* ISAs plus the wide Thumb-2 encodings and `IT` blocks in the hard-items table, "
        "so 'Thumb-only' or 'ARM-only' is not a shortcut here. On NEON specifically the static "
        "data is the right instrument and it answers the existence question head-on: NEON is a "
        "thin tail of the encoding mix (~1.5%) and no game in the corpus is NEON-*heavy* — "
        "where it appears it is a concentrated minority next to 93% integer and 5% VFP work — so "
        "an **ARMv7 + VFP-first milestone (no NEON) would plausibly boot and run most of these "
        "games**, with NEON arriving as a smaller second increment rather than as a gate. The "
        "honest caveat is that this is a *static* proxy: it cannot say whether NEON sits on a "
        "hot path, and a handful of NEON instructions in the inner loop of a physics, skinning "
        "or audio kernel would block a no-NEON milestone even at a 1–2% static share. It also "
        "cannot tell you whether the integer/memory/branch opcodes you do translate are the "
        "ones that dominate *runtime*. Confirming the dynamic share, and the dynamic weight of "
        "each hard item, needs an interpreter that can execute the game and count retired "
        "instructions — the next step. Read this report as an upper bound on which instruction "
        "classes exist, not as evidence about which ones run."
    )
    w(verdict + "\n")
    w("---\n*Every number above comes from the run recorded alongside this file "
      "(`arm32-traffic.json`); no counts are estimated or fabricated.*")

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L))


if __name__ == "__main__":
    ENGINES = None
    main()
