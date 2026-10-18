#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Offline APK compatibility pre-checker for Husk's translation layer.

    python tools/apk-precheck.py <apk-or-folder-or-glob> [...]
                               [--json OUT] [--json-only] [--no-imports] [--max-lib-mb N]

Given one or more APKs (files, folders, or globs), say which of Husk's support
classes each one falls into -- engine, ABI, content shape, Play gating, missing
imports -- with the exact evidence (file name / string / absence) each verdict
was built from, so a wrong verdict is arguable. Print a human table and write a
machine-readable JSON report.

Runs on a plain Windows / macOS / Linux box with Python 3 and the standard
library only. No capstone, no pyelftools, no phone, no network. Native-library
imports are read with a small inline ELF64 / ELF32 section + program-header
parser.

Verdict vocabulary (one per APK):
    runs-native                  engine Husk has a driver for, arm64, content present
    runs-java                    runs on Husk's Java runtime (or a Java-only app)
    needs-content                side-car OBB / first-launch download required
    needs-google                 needs Play services the layer does not provide
    unsupported-32bit            native code, but none for arm64
    unsupported-play-asset-packs Play Asset Delivery shape, unsatisfiable offline
    unsupported-split-missing    a base APK of a split set whose siblings are absent
    likely-missing-imports       known-unresolved imports the layer binds to stubs
    unknown                      cannot be classified from the APK alone

The knowledge encoded here (engine markers, the hard arm64 gate, content shapes,
Play asset packs, the unresolved-import set) lives in:
    src/translation-layer/husk-tl-scan.c
    src/app/Husk/TranslationLayer.swift:57
    src/translation-layer-next/husk-tl-ld.c
    src/translation-layer-next/husk-tl-jni-ue4.c / husk-tl-jni-gms.c
    .hermes/plans/engine-reset/*.md
"""
import argparse
import glob
import json
import os
import re
import struct
import sys
import zipfile

# --------------------------------------------------------------------------
# Constants
# --------------------------------------------------------------------------

# ABIs the layer's scanner knows; the one it requires (husk-tl-scan.c kAbis,
# and the hard gate at src/app/Husk/TranslationLayer.swift:57).
ABIS = ["arm64-v8a", "armeabi-v7a", "armeabi", "x86_64", "x86",
        "riscv64", "mips64", "mips"]
ARM64 = "arm64-v8a"

# Imports the layer has been seen to leave unbound -> bound to logging stubs.
# (src/translation-layer-next/husk-tl-ld.c "unresolved import"; observed set.)
KNOWN_MISSING = [
    "swscanf", "getgrgid", "getgrnam", "pthread_attr_setstack",
    "gzopen", "gzread", "gzclose",
]

# Play Asset Delivery markers.
#
# Decisive only where the *native* engine code or a manifest file shows the
# app expects Play to deliver its content:
#   - a GooglePAD-Manifest.txt (the pack list Unreal/Unity embed), or
#   - the full assetpacks class path / GooglePAD marker inside a native lib.
# Deliberately NOT the bare string "AssetPackManager" inside libunity.so
# (Unity's IL2CPP metadata carries it in ordinary builds) nor Play-core class
# names inside dex (bundled for in-app review/update in almost every app).
ASSETPACK_FILE_NEEDLES = [b"GooglePAD-Manifest.txt", b"assetpack"]
ASSETPACK_LIB_NEEDLES = [
    b"com/google/android/play/core/assetpacks",
    b"GooglePAD",
]
# Play services marker (dex).
GMS_STRINGS = [b"com/google/android/gms", b"Lcom/google/android/gms"]

# AXML string markers that mean "this is one split of a set". Only the strong
# ones: "com.android.vending.splits" is an ordinary Play-core permission and
# appears in bundled (non-split) APKs too, so it is not used.
SPLIT_MARKERS = [
    "isSplitRequired", "com.android.vending.splits.required",
]

# Engine class names inside the manifest, paired with the native library each
# must be present for: a manifest that names one without shipping it is a
# "declares an engine it does not ship" flag.
ENGINE_CLASS_MARKERS = [
    ("Unity", ["UnityPlayerActivity", "UnityPlayerForActivityOrService",
               "com.unity3d.player"], ["libunity.so", "libil2cpp.so"]),
    ("Unreal", ["com.epicgames.unreal"], ["libUE4.so", "libUnreal.so"]),
    ("Cocos", ["org.cocos2dx"], ["libcocos2d.so", "libcocos2dcpp.so",
                                 "libcocos2djs.so", "libcocos2d-x.so"]),
    ("Godot", ["org.godotengine"], ["libgodot_android.so"]),
    ("Flutter", ["io.flutter"], ["libflutter.so"]),
    ("React Native", ["com.facebook.react"],
     ["libreactnativejni.so", "libhermes.so"]),
    ("Ren'Py", ["org.renpy"], ["librenpython.so"]),
    ("GameMaker", ["com.yoyogames"], ["libyoyo.so"]),
    ("libGDX", ["com.badlogic.gdx"], ["libgdx.so"]),
]

# Engines the layer drives on the native runtime vs. on the Java runtime.
NATIVE_ENGINES = {"Unity (IL2CPP)", "Unity", "Unreal Engine", "Rockstar",
                  "Godot", "GameMaker", "Cocos", "Minecraft", "Ren'Py",
                  "NativeActivity"}
JAVA_ENGINES = {"Unity 6", "Flutter", "React Native", ".NET / Xamarin",
                "libGDX", "SDL", "Python (Kivy/p4a)", "Capacitor", "Cordova",
                "(EA/PopCap own engine)", "Java"}

# WHAT THIS TOOL DELIBERATELY CANNOT KNOW:
#  - whether a named OBB the user must supply is actually present next to the
#    APK (it only sees the APK's own contents);
#  - a first-launch download that happens only after the app starts;
#  - the exact entry activity of a heavily obfuscated manifest (best-effort).


# --------------------------------------------------------------------------
# Small binary readers
# --------------------------------------------------------------------------

def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def stream_contains(zf, name, needles, chunk=4 << 20):
    """Search a (possibly huge) zip member for byte needles without loading it.

    Returns the matched needle or None. Handles matches across chunk borders by
    keeping the tail of the previous chunk.
    """
    try:
        with zf.open(name) as fh:
            prev = b""
            while True:
                buf = fh.read(chunk)
                if not buf:
                    break
                window = prev + buf
                for nd in needles:
                    if nd in window:
                        return nd
                prev = window[-64:]
    except Exception:  # noqa: BLE001
        return None
    return None


# --------------------------------------------------------------------------
# Minimal ELF64 / ELF32 parser (section headers, PT_DYNAMIC fallback)
# --------------------------------------------------------------------------

class Elf:
    def __init__(self, data):
        self.data = data
        self.ok = False
        self.is64 = None
        self.machine = None
        self.imports = []
        self.needed = []
        self.soname = None
        self.note = None
        try:
            self._parse()
        except Exception as exc:  # noqa: BLE001 - malformed input is data
            self.note = "elf parse failed: %s" % exc

    def _parse(self):
        d = self.data
        if len(d) < 64 or d[:4] != b"\x7fELF":
            self.note = "not an ELF"
            return
        self.is64 = d[4] == 2
        self.machine = u16(d, 18)
        if self.is64:
            self.e_phoff = u64(d, 0x20)
            self.e_shoff = u64(d, 0x28)
            self.e_phentsize = u16(d, 0x36)
            self.e_phnum = u16(d, 0x38)
            self.e_shentsize = u16(d, 0x3A)
            self.e_shnum = u16(d, 0x3C)
        else:
            self.e_phoff = u32(d, 0x1C)
            self.e_shoff = u32(d, 0x20)
            self.e_phentsize = u16(d, 0x2A)
            self.e_phnum = u16(d, 0x2C)
            self.e_shentsize = u16(d, 0x2E)
            self.e_shnum = u16(d, 0x30)
        self.ok = True
        if not self._by_sections():
            self._by_dynamic()

    def _sec(self, i):
        d = self.data
        o = self.e_shoff + i * self.e_shentsize
        if self.is64:
            return {"type": u32(d, o + 4), "offset": u64(d, o + 24),
                    "size": u64(d, o + 32), "link": u32(d, o + 40),
                    "entsize": u64(d, o + 56)}
        return {"type": u32(d, o + 4), "offset": u32(d, o + 16),
                "size": u32(d, o + 20), "link": u32(d, o + 24),
                "entsize": u32(d, o + 36)}

    def _by_sections(self):
        d = self.data
        if not self.e_shoff or not self.e_shnum:
            return False
        if self.e_shoff + self.e_shnum * self.e_shentsize > len(d):
            return False
        sym_sec = dyn_sec = None
        for i in range(self.e_shnum):
            s = self._sec(i)
            if s["type"] == 11:          # SHT_DYNSYM
                sym_sec = s
            elif s["type"] == 6:         # SHT_DYNAMIC
                dyn_sec = s
        if sym_sec is None:
            return False
        str_sec = self._sec(sym_sec["link"]) if sym_sec["link"] else None
        if str_sec is None:
            return False
        symtab = d[sym_sec["offset"]:sym_sec["offset"] + sym_sec["size"]]
        strtab = d[str_sec["offset"]:str_sec["offset"] + str_sec["size"]]
        entsize = sym_sec["entsize"] or (24 if self.is64 else 16)
        self._collect_symbols(symtab, strtab, entsize)
        if dyn_sec is not None:
            dyn = d[dyn_sec["offset"]:dyn_sec["offset"] + dyn_sec["size"]]
            self._collect_dynamic(dyn, strtab)
        return True

    def _collect_symbols(self, symtab, strtab, entsize):
        n = len(symtab) // entsize if entsize else 0
        for i in range(n):
            o = i * entsize
            name_off = u32(symtab, o)
            if self.is64:
                shndx = u16(symtab, o + 6)
            else:
                shndx = u16(symtab, o + 14)
            name = self._cstr(strtab, name_off)
            if name and shndx == 0:      # SHN_UNDEF
                self.imports.append(name)

    def _collect_dynamic(self, dyn, strtab):
        step = 16 if self.is64 else 8
        for o in range(0, len(dyn) - step + 1, step):
            tag = u64(dyn, o) if self.is64 else u32(dyn, o)
            val = u64(dyn, o + 8) if self.is64 else u32(dyn, o + 4)
            if tag == 0:
                break
            if tag == 1:                 # DT_NEEDED
                self.needed.append(self._cstr(strtab, val))
            elif tag == 14:              # DT_SONAME
                self.soname = self._cstr(strtab, val)

    @staticmethod
    def _cstr(tab, off):
        if off is None or off >= len(tab):
            return ""
        end = tab.find(b"\x00", off)
        if end < 0:
            end = len(tab)
        return tab[off:end].decode("utf-8", "replace")

    def _ph(self, i):
        d = self.data
        o = self.e_phoff + i * self.e_phentsize
        if self.is64:
            return {"type": u32(d, o), "offset": u64(d, o + 8),
                    "vaddr": u64(d, o + 16), "filesz": u64(d, o + 32)}
        return {"type": u32(d, o), "offset": u32(d, o + 4),
                "vaddr": u32(d, o + 8), "filesz": u32(d, o + 16)}

    def _by_dynamic(self):
        d = self.data
        if not self.e_phoff or not self.e_phnum:
            self.note = "no program headers"
            return
        loads, dynph = [], None
        for i in range(self.e_phnum):
            p = self._ph(i)
            if p["type"] == 1:           # PT_LOAD
                loads.append(p)
            elif p["type"] == 2:         # PT_DYNAMIC
                dynph = p
        if dynph is None:
            self.note = "no PT_DYNAMIC"
            return

        def v2o(v):
            for p in loads:
                if p["vaddr"] <= v < p["vaddr"] + p["filesz"]:
                    return p["offset"] + (v - p["vaddr"])
            return None

        dyn = d[dynph["offset"]:dynph["offset"] + dynph["filesz"]]
        step = 16 if self.is64 else 8
        symtab = strtab = hash_ = strsz = None
        for o in range(0, len(dyn) - step + 1, step):
            tag = u64(dyn, o) if self.is64 else u32(dyn, o)
            val = u64(dyn, o + 8) if self.is64 else u32(dyn, o + 4)
            if tag == 0:
                break
            if tag == 6:
                symtab = val
            elif tag == 5:
                strtab = val
            elif tag == 10:
                strsz = val
            elif tag == 4:
                hash_ = val
            elif tag == 1:
                off = v2o(val)
                if off is not None:
                    self.needed.append(self._cstr(d, off))
            elif tag == 14:
                off = v2o(val)
                if off is not None:
                    self.soname = self._cstr(d, off)
        if symtab is None or strtab is None:
            self.note = "no DT_SYMTAB/DT_STRTAB"
            return
        soff, stoff = v2o(symtab), v2o(strtab)
        if soff is None or stoff is None:
            self.note = "dynamic offsets outside any PT_LOAD"
            return
        st = d[stoff:stoff + strsz] if strsz else d[stoff:]
        count = None
        if hash_ is not None:
            ho = v2o(hash_)
            if ho is not None and ho + 8 <= len(d):
                count = u32(d, ho + 4)   # DT_HASH nchain
        if count is None:
            span = stoff - soff if stoff > soff else 0
            count = span // (24 if self.is64 else 16)
            self.note = "symbol count estimated (GNU-hash only)"
        entsize = 24 if self.is64 else 16
        self._collect_symbols(d[soff:soff + count * entsize], st, entsize)


# --------------------------------------------------------------------------
# Binary AndroidManifest.xml (AXML): string pool + launcher activity walk
# --------------------------------------------------------------------------

def axml_strings(buf):
    if len(buf) < 8 or u32(buf, 0) != 0x00080003:
        return None
    off = 8
    if len(buf) < off + 28 or u16(buf, off) != 0x0001:
        return None
    header = u16(buf, off + 2)
    string_count = u32(buf, off + 8)
    flags = u32(buf, off + 16)
    strings_start = u32(buf, off + 20)
    is_utf8 = bool(flags & 0x100)
    offs_base = off + header
    data_base = off + strings_start
    out = []
    for i in range(string_count):
        if offs_base + i * 4 + 4 > len(buf):
            break
        p = data_base + u32(buf, offs_base + i * 4)
        try:
            if is_utf8:
                n = buf[p]
                p += 1
                if n & 0x80:
                    n = ((n & 0x7F) << 8) | buf[p]
                    p += 1
                out.append(buf[p:p + n].decode("utf-8", "replace"))
            else:
                n = u16(buf, p)
                p += 2
                if n & 0x8000:
                    n = ((n & 0x7FFF) << 16) | u16(buf, p)
                    p += 2
                out.append(buf[p:p + n * 2].decode("utf-16-le", "replace"))
        except Exception:  # noqa: BLE001
            out.append("")
    return out


def _axml_attr(buf, strings, pos):
    name_idx = u32(buf, pos + 20)
    header_size = u16(buf, pos + 2)
    attr_start = u16(buf, pos + 24)
    attr_count = u16(buf, pos + 28)
    name = strings[name_idx] if name_idx < len(strings) else ""
    attrs = {}
    base = pos + header_size + attr_start
    for a in range(attr_count):
        ao = base + a * 20
        if ao + 20 > len(buf):
            break
        an = u32(buf, ao + 4)
        raw = u32(buf, ao + 8)
        dtype = buf[ao + 15]
        data = u32(buf, ao + 16)
        key = strings[an] if an < len(strings) else ""
        if dtype == 0x03:                 # TYPE_STRING
            if raw != 0xFFFFFFFF and raw < len(strings):
                val = strings[raw]
            elif data < len(strings):
                val = strings[data]
            else:
                val = ""
        elif dtype == 0x12:               # TYPE_INT_BOOLEAN
            val = bool(data)
        else:
            val = str(data)
        attrs[key] = val
    return name, attrs


def axml_launcher(buf, strings):
    """Component name of the activity with a MAIN+LAUNCHER intent-filter."""
    if len(buf) < 8 or u32(buf, 0) != 0x00080003:
        return None
    pos = 8
    stack = []
    pending = None
    launcher = None
    while pos + 8 <= len(buf):
        ctype = u16(buf, pos)
        chunksize = u32(buf, pos + 4)
        if chunksize <= 0:
            break
        if ctype == 0x0102:               # START_ELEMENT
            name, attrs = _axml_attr(buf, strings, pos)
            if name == "intent-filter":
                pending = {"main": False, "launcher": False,
                           "owner": stack[-1] if stack else None}
            elif name == "action" and pending is not None \
                    and attrs.get("name") == "android.intent.action.MAIN":
                pending["main"] = True
            elif name == "category" and pending is not None \
                    and attrs.get("name") == "android.intent.category.LAUNCHER":
                pending["launcher"] = True
            stack.append({"name": name, "attrs": attrs})
        elif ctype == 0x0103:             # END_ELEMENT
            name_idx = u32(buf, pos + 20)
            name = strings[name_idx] if name_idx < len(strings) else ""
            if name == "intent-filter" and pending is not None:
                if pending["main"] and pending["launcher"] and pending["owner"]:
                    launcher = pending["owner"]["attrs"].get("name") or launcher
                pending = None
            if stack:
                stack.pop()
        pos += chunksize
    return launcher


# --------------------------------------------------------------------------
# APK analysis
# --------------------------------------------------------------------------

def read_member(zf, name, cap):
    try:
        info = zf.getinfo(name)
    except KeyError:
        return b"", False
    truncated = info.file_size > cap
    with zf.open(name) as fh:
        data = fh.read(cap)
    return data, truncated


def lib_abi(path):
    m = re.match(r"^lib/([^/]+)/([^/]+\.so)$", path)
    if not m:
        return None
    if m.group(1) not in ABIS:
        return None
    return m.group(1), m.group(2)


def analyze(path, max_lib_bytes, scan_imports=True):
    rec = {
        "apk": os.path.basename(path),
        "path": os.path.abspath(path),
        "verdict": "unknown",
        "engine": None,
        "abis": [],
        "size": os.path.getsize(path) if os.path.exists(path) else 0,
        "evidence": [],
        "content": [],
        "missing_imports": {},
        "libraries": [],
        "system_libraries": [],
        "entry_activity": None,
        "needs_google": False,
        "declares_engine_not_shipped": None,
        "split_required_marker": None,
    }
    ev = rec["evidence"]
    e = ev.append

    try:
        zf = zipfile.ZipFile(path)
    except Exception as exc:  # noqa: BLE001
        e("cannot open as zip: %s" % exc)
        return rec

    with zf:
        names = zf.namelist()
        nameset = set(names)

        # --- ABI / native libraries --------------------------------------
        libs = {}
        lib_sizes = {}
        for n in names:
            la = lib_abi(n)
            if la:
                libs.setdefault(la[0], []).append(la[1])
                try:
                    lib_sizes[(la[0], la[1])] = zf.getinfo(n).file_size
                except KeyError:
                    pass
        rec["abis"] = [a for a in ABIS if a in libs]
        if libs:
            e("ABIs from lib/<abi>/*.so: %s" % ", ".join(rec["abis"]))
        else:
            e("no lib/<abi>/*.so entries: pure Java/Dalvik app")

        # --- engine from library names -----------------------------------
        all_libs = [n for a in libs for n in libs[a]]
        has = lambda nm: nm in all_libs
        eng = None
        if has("libGame.so") and has("libopenal.so") and has("libVendor_mpg123.so"):
            eng = "Rockstar"
        elif has("libil2cpp.so"):
            eng = "Unity (IL2CPP)"
        elif has("libunity.so"):
            eng = "Unity"
        elif has("libUE4.so") or has("libUnreal.so"):
            eng = "Unreal Engine"
        elif has("libgodot_android.so"):
            eng = "Godot"
        elif has("libyoyo.so"):
            eng = "GameMaker"
        elif has("librenpython.so"):
            eng = "Ren'Py"
        elif any(n.startswith("libcocos") for n in all_libs):
            eng = "Cocos"
        elif has("libflutter.so"):
            eng = "Flutter"
        elif has("libPVZ2.so"):
            eng = "(EA/PopCap own engine)"
        elif any(n.startswith("libpython") for n in all_libs):
            eng = "Python (Kivy/p4a)"
        elif has("libSDL3.so") or has("libSDL2.so"):
            eng = "SDL"
        elif has("libgdx.so"):
            eng = "libGDX"
        elif has("libreactnativejni.so") or has("libhermes.so"):
            eng = "React Native"
        elif has("libminecraftpe.so"):
            eng = "Minecraft"
        rec["engine"] = eng
        if eng:
            e("engine marker: %s (native library names)" % eng)

        if "assets/capacitor.config.json" in nameset:
            rec["engine"] = "Capacitor"
            e("assets/capacitor.config.json present -> web app")
        if "assets/www/cordova.js" in nameset or "assets/www/cordova_plugins.js" in nameset:
            rec["engine"] = "Cordova"
            e("assets/www/cordova.js present -> web app")

        # --- Unity 6 marker inside libunity.so ---------------------------
        unity6 = False
        if "libunity.so" in all_libs:
            abi = ARM64 if ARM64 in libs else rec["abis"][0]
            if stream_contains(zf, "lib/%s/libunity.so" % abi,
                               [b"UnityPlayerForActivityOrService"]):
                unity6 = True
                rec["engine"] = "Unity 6"
                e("libunity.so contains 'UnityPlayerForActivityOrService' -> Unity 6 "
                  "(Husk runs it on the Java runtime)")

        # --- content shape -----------------------------------------------
        content = rec["content"]
        pre = len(ev)
        if "assets/private.mp3" in nameset:
            content.append("in-APK")
            e("assets/private.mp3 present (Ren'Py asset archive)")
        xgame = sum(1 for n in names if n.startswith("assets/x-game/"))
        xrenpy = sum(1 for n in names if n.startswith("assets/x-renpy/"))
        if xgame:
            content.append("in-APK")
            e("assets/x-game/ present (%d entries)" % xgame)
        if xrenpy:
            content.append("in-APK")
            e("assets/x-renpy/ present (%d entries)" % xrenpy)
        if any(n.startswith("assets/bin/Data/") for n in names):
            content.append("in-APK")
            e("assets/bin/Data/ present (Unity data inside the APK)")
        if any(n.startswith("assets/") for n in names) and "in-APK" not in content:
            content.append("in-APK-assets")
            e("assets/ present but no known content root under it")
        rec["content"] = list(dict.fromkeys(content))

        # --- Play gating: asset packs -----------------------------------
        # Decisive: a GooglePAD/assetpack manifest file inside the APK, or the
        # assetpacks class path / GooglePAD marker inside a *native* library.
        # (Not dex: Play-core classes are bundled even in non-PAD apps.)
        assetpack_hit = None
        for n in names:
            if n.endswith("GooglePAD-Manifest.txt") or "assetpack" in n.lower():
                assetpack_hit = "file %s" % n
                break
        if assetpack_hit is None:
            for n in names:
                if not lib_abi(n):
                    continue
                hit = stream_contains(zf, n, ASSETPACK_LIB_NEEDLES)
                if hit:
                    assetpack_hit = "%s : %s" % (n, hit.decode("latin-1"))
                    break
        if assetpack_hit:
            e("Play Asset Delivery marker: %s" % assetpack_hit)

        # --- Play services in dex ----------------------------------------
        for n in names:
            if n.startswith("classes") and n.endswith(".dex"):
                if stream_contains(zf, n, GMS_STRINGS):
                    rec["needs_google"] = True
                    e("dex %s references com/google/android/gms" % n)
                    break

        # --- manifest -----------------------------------------------------
        if "AndroidManifest.xml" in nameset:
            mdata, _ = read_member(zf, "AndroidManifest.xml", 16 << 20)
            strings = axml_strings(mdata)
            if strings is not None:
                joined = "\n".join(strings)
                for mk in SPLIT_MARKERS:
                    if mk in joined:
                        rec["split_required_marker"] = mk
                        e("manifest string %r present -> split APK set" % mk)
                        break
                launcher = axml_launcher(mdata, strings)
                rec["entry_activity"] = launcher
                if launcher:
                    e("entry activity (MAIN+LAUNCHER): %s" % launcher)
                for engname, classes, required in ENGINE_CLASS_MARKERS:
                    if any(c in joined for c in classes):
                        if not any(r in all_libs for r in required) \
                                and rec["engine"] != engname:
                            rec["declares_engine_not_shipped"] = engname
                            e("manifest names %s but ships none of %s"
                              % (engname, "/".join(required)))
            else:
                e("AndroidManifest.xml is not AXML; manifest markers not checked")
        else:
            e("no AndroidManifest.xml in the archive")

        # --- per-arm64-library ELF imports --------------------------------
        own = set(libs.get(ARM64, []))
        needed_all = set()
        for name in sorted(own):
            entry = {"name": name, "bytes": lib_sizes.get((ARM64, name)),
                     "imports": None, "needed": [], "note": None,
                     "is64": None, "machine": None}
            if scan_imports:
                data, trunc = read_member(zf, "lib/%s/%s" % (ARM64, name),
                                          max_lib_bytes)
                if trunc:
                    entry["note"] = "larger than scan cap; imports not read"
                else:
                    elf = Elf(data)
                    if not elf.ok:
                        entry["note"] = elf.note or "not ELF"
                    else:
                        entry["imports"] = len(elf.imports)
                        entry["needed"] = elf.needed
                        entry["is64"] = elf.is64
                        entry["machine"] = elf.machine
                        needed_all |= set(elf.needed)
                        miss = sorted(set(elf.imports) & set(KNOWN_MISSING))
                        if miss:
                            rec["missing_imports"][name] = miss
            rec["libraries"].append(entry)
        rec["system_libraries"] = sorted(needed_all - own)

        # =================================================================
        # Verdict selection (fixed vocabulary). Hard blocks first.
        # =================================================================
        has_native = bool(libs)
        arm64 = ARM64 in libs
        engine = rec["engine"]
        verdict = None

        if not names:
            verdict = "unknown"
        elif rec["split_required_marker"]:
            verdict = "unsupported-split-missing"
        elif assetpack_hit:
            verdict = "unsupported-play-asset-packs"
        elif has_native and not arm64:
            verdict = "unsupported-32bit"
            e("native libraries present but none for %s" % ARM64)
        else:
            # content required? (known engine, arm64, no content root in APK)
            needs_content = None
            if engine in ("Ren'Py", "Python (Kivy/p4a)") and "in-APK" not in content:
                needs_content = ("Ren'Py/Python app with no assets/private.mp3 or "
                                 "assets/x-* in the APK")
            elif engine and engine.startswith("Unity") and not unity6 \
                    and not any(n.startswith("assets/bin/Data/") for n in names) \
                    and "in-APK" not in content:
                needs_content = ("Unity build with no assets/bin/Data in the APK: data "
                                 "must come from an OBB or a first-launch download")
            elif engine == "(EA/PopCap own engine)" and "in-APK" not in content:
                needs_content = ("no content root in the APK (no assets/x-game, no "
                                 "assets/bin/Data): the game's data must come from a "
                                 "side-car OBB")
            if needs_content:
                verdict = "needs-content"
                e("content shape: %s" % needs_content)

        if verdict is None:
            if rec["missing_imports"]:
                verdict = "likely-missing-imports"
                for lib, nm in rec["missing_imports"].items():
                    e("lib/%s/%s imports %s: the layer binds these to logging stubs"
                      % (ARM64, lib, ", ".join(nm)))
            elif not has_native:
                verdict = "runs-java"
                if rec["needs_google"]:
                    verdict = "needs-google"
                    e("Java-only app that references Play services (com.google.android.gms)")
                else:
                    e("no native code: runs on Husk's own Java runtime")
            elif engine in NATIVE_ENGINES:
                verdict = "runs-native"
                e("engine %s has a driver in the layer" % engine)
            elif engine in JAVA_ENGINES:
                verdict = "runs-java"
                e("engine %s runs on the Java runtime" % engine)
            elif arm64:
                verdict = "runs-native"
                e("arm64 native code; engine not named, driven by the loader")
            else:
                verdict = "unknown"

        rec["verdict"] = verdict

    return rec


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------

def print_table(recs, show_imports=True):
    w = min(max((len(r["apk"]) for r in recs), default=12), 60)
    print()
    print("%-*s  %-30s  %-18s  %s" % (w, "APK", "VERDICT", "ENGINE", "ABIS"))
    print("-" * (w + 68))
    for r in recs:
        print("%-*s  %-30s  %-18s  %s" % (
            w, r["apk"][:w], r["verdict"], (r["engine"] or "-")[:18],
            ",".join(r["abis"]) or "-"))
    print()
    for r in recs:
        print("= %s" % r["apk"])
        print("  verdict : %s" % r["verdict"])
        print("  engine  : %s" % (r["engine"] or "(none detected)"))
        print("  size    : %.1f MB" % (r["size"] / (1024 * 1024)))
        print("  abis    : %s" % (", ".join(r["abis"]) or "(none)"))
        print("  content : %s" % (", ".join(r["content"]) or "(no content root in APK)"))
        if r.get("entry_activity"):
            print("  activity: %s" % r["entry_activity"])
        if r.get("needs_google"):
            print("  google  : dex references com.google.android.gms")
        if r.get("declares_engine_not_shipped"):
            print("  declares: names %s but does not ship it"
                  % r["declares_engine_not_shipped"])
        if r.get("split_required_marker"):
            print("  split   : %s" % r["split_required_marker"])
        if show_imports and r["missing_imports"]:
            for lib, nm in r["missing_imports"].items():
                print("  imports : lib/%s/%s -> %s" % (ARM64, lib, ", ".join(nm)))
        elif show_imports and r["libraries"]:
            scanned = [l for l in r["libraries"] if l["imports"] is not None]
            if scanned:
                total = sum(l["imports"] for l in scanned)
                skipped = len(r["libraries"]) - len(scanned)
                print("  imports : %d/%d arm64 lib(s) scanned, %d dynamic import(s); "
                      "none in the known-missing set%s"
                      % (len(scanned), len(r["libraries"]), total,
                         " (%d skipped)" % skipped if skipped else ""))
        print("  evidence:")
        for line in r["evidence"]:
            print("    - %s" % line)
        print()


def write_json(recs, out_path):
    payload = {
        "tool": "tools/apk-precheck.py",
        "fixtures": [r["apk"] for r in recs],
        "results": recs,
    }
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, ensure_ascii=False)
    return out_path


def expand_targets(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += sorted(glob.glob(os.path.join(a, "*.apk")))
        elif any(c in a for c in "*?["):
            out += sorted(glob.glob(a))
        else:
            out.append(a)
    seen, uniq = set(), []
    for p in out:
        ap = os.path.abspath(p)
        if ap not in seen:
            seen.add(ap)
            uniq.append(p)
    return uniq


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("targets", nargs="+",
                    help="APK file, folder, or glob (repeatable)")
    ap.add_argument("--json", default="precheck-report.json",
                    help="where to write the machine-readable report")
    ap.add_argument("--json-only", action="store_true",
                    help="write JSON without the human table")
    ap.add_argument("--no-imports", action="store_true",
                    help="skip reading native libraries for imports (fast)")
    ap.add_argument("--max-lib-mb", type=int, default=256,
                    help="skip native libs larger than this many MiB (default 256)")
    a = ap.parse_args(argv)

    paths = expand_targets(a.targets)
    if not paths:
        print("no APKs found for: %s" % " ".join(a.targets), file=sys.stderr)
        return 2
    recs = [analyze(p, a.max_lib_mb << 20, scan_imports=not a.no_imports)
            for p in paths]
    if not a.json_only:
        print_table(recs)
    out = write_json(recs, a.json)
    print("JSON report: %s (%d APK%s)" % (out, len(recs),
                                          "" if len(recs) == 1 else "s"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
