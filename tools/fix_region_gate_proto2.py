#!/usr/bin/env python3
"""
My previous fix put the prototype INSIDE a function body, and C forbids a
STATIC function to be declared at block scope:

  husk-ios-jit.c:510:5: error: function declared in block scope cannot have 'static' storage class
  husk-ios-jit.c:722:13: error: static declaration of 'husk_ios_jit_region_gate' follows non-static declaration

A static function can only be forward-declared at FILE scope. This does it
properly: remove the block-scope line, and declare it at file scope next to the
file's existing forward declaration (husk_ios_jit_allocate_real, line 491) --
which is already inside the same `#if defined(__APPLE__) && TARGET_OS_IPHONE`
branch as the definition (line 721), and well before the first use (line 510).
"""
import io, re, sys, difflib

P = r"C:/Users/Aiden/Desktop/HuskApp/src/ios-jit/husk-ios-jit.c"
src = io.open(P, encoding="utf-8", newline="").read()

# 1. remove the block-scope declaration I added (the semicolon makes it unique:
#    the real definition has no semicolon)
bad = re.compile(r"[ \t]*static\s+bool\s+husk_ios_jit_region_gate\s*\(\s*const char \*after\s*\)\s*;[ \t]*\r?\n")
if not bad.search(src):
    print("block-scope line not found (already removed?)")
else:
    src = bad.sub("", src, count=1)
    print("removed the block-scope declaration")

# 2. declare it at file scope, next to the file's existing forward declaration
PROTO = "static bool husk_ios_jit_region_gate(const char *after);   /* file scope: static functions cannot be declared in a block */\n"
anchor = re.search(r"^[ \t]*static\s+HuskDualMapping\s+husk_ios_jit_allocate_real\s*\([^)]*\)\s*;[ \t]*\r?\n", src, re.M)
if not anchor:
    print("ANCHOR NOT FOUND: husk_ios_jit_allocate_real prototype"); sys.exit(1)
if PROTO in src:
    print("file-scope prototype already present")
else:
    # insert on the line AFTER the anchor (keeps the anchor's own line untouched)
    line_end = anchor.end()
    src = src[:line_end] + PROTO + src[line_end:]
    print("inserted the file-scope prototype after line", src[:anchor.start()].count("\n") + 1)

io.open(P, "w", encoding="utf-8", newline="").write(src)

print("\n--- sanity ---")
print("braces balance:", src.count("{") - src.count("}"), "(0 = balanced)")
print("semicolon-prototypes:", len(re.findall(r"static\s+bool\s+husk_ios_jit_region_gate\s*\([^)]*\)\s*;", src)))
print("file-scope prototypes:", len(re.findall(r"^static\s+bool\s+husk_ios_jit_region_gate\s*\([^)]*\)\s*;", src, re.M)))
print("definitions:", len(re.findall(r"^static\s+bool\s+husk_ios_jit_region_gate\s*\([^)]*\)\s*$", src, re.M)))
