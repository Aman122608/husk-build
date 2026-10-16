#!/usr/bin/env python3
"""
The QEMU-removal commit turned the public husk_ios_jit_exec_gate into a weak
wrapper around a NEW static husk_ios_jit_region_gate -- but it is CALLED at
line 510 and DEFINED at 721, so C compiles an implicit declaration and then
rejects the static definition:

  husk-ios-jit.c:510:33: error: call to undeclared function 'husk_ios_jit_region_gate'
  husk-ios-jit.c:721:13: error: static declaration ... follows non-static declaration

A prototype before the first use fixes both. Placed immediately above the
first-use line, so it stays inside whichever guard that code is under (the
non-Apple stub branch never sees it).
"""
import io, re, sys, difflib

P = r"C:/Users/Aiden/Desktop/HuskApp/src/ios-jit/husk-ios-jit.c"
lines = io.open(P, encoding="utf-8", newline="").read().split("\n")
src = "\n".join(lines)

name = "husk_ios_jit_region_gate"

defn = re.search(r"^\s*static\s+([A-Za-z_][\w \*]*?)\s*" + name + r"\s*\(([^)]*)\)", src, re.M)
if not defn:
    print("NO STATIC DEFINITION FOUND for", name); sys.exit(1)
ret, params = defn.group(1).strip(), defn.group(2).strip()
proto = "static %s %s(%s);" % (ret, name, params)

if re.search(r"^\s*static\s+[^\n]*" + name + r"\s*\([^)]*\)\s*;", src, re.M):
    print("prototype already present, nothing to do"); sys.exit(0)

def_line = src[:defn.start()].count("\n") + 1
uses = [i for i, l in enumerate(lines)
        if name in l and "(" in l and not re.match(r"^\s*static\s", l)]
if not uses:
    print("NO USE FOUND"); sys.exit(1)
first = uses[0]
print("definition on line %d, first use on line %d" % (def_line, first + 1))

indent = re.match(r"\s*", lines[first]).group(0)
lines.insert(first, indent + proto)
out = "\n".join(lines)
io.open(P, "w", encoding="utf-8", newline="").write(out)

print("inserted:", proto)
print("\n".join(difflib.unified_diff(src.split("\n"), out.split("\n"), "before", "after", lineterm="", n=2))[:900])
