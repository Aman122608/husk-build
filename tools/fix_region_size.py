#!/usr/bin/env python3
"""
The app links the layer (src/translation-layer-next) but NOT src/ios-jit, so a
husk_* entry point the Swift calls must be DEFINED here. This is the second time
this exact trap has cost a build (husk_ios_jit_exec_gate was the first).

Define husk_ios_jit_region_size() in the layer, next to tl_xmem_size(), answering
what the owner's setting calls "claimed": the size of the region THIS LAUNCH got,
never what was asked for. Once the arena is open that is g_x.size; before any game
starts it is the size of the prewarmed region the layer will use (0 if none).
"""
import io, os, re, sys, difflib

P = r"C:/Users/Aiden/Desktop/HuskApp/src/translation-layer-next/husk-tl-xmem.c"
src = io.open(P, encoding="utf-8", newline="").read()

if "husk_ios_jit_region_size" in src:
    print("already present, nothing to do"); sys.exit(0)

m = re.search(r"^[^\n]*tl_xmem_size\s*\(\s*void\s*\)[^\n]*$", src, re.M)
if not m:
    print("ANCHOR NOT FOUND: tl_xmem_size(void)"); sys.exit(1)

# find the end of that function's body: the first '}' at or after the anchor
end = src.find("}", m.end())
if end < 0:
    print("ANCHOR BODY NOT FOUND"); sys.exit(1)
insert_at = end + 1

FN = '''

/*
 * What the JIT arena size setting calls "claimed": the size of the region THIS
 * LAUNCH actually got -- the number the app must show, never the number it asked
 * for. Once the arena is open that is g_x.size; before any game starts it is the
 * size of the prewarmed region the layer will use, which is the honest answer at
 * that point, and 0 if there is none. Defined here, in the layer, because the app
 * links the layer and not src/ios-jit.
 */
size_t husk_ios_jit_region_size(void)
{
    if (g_x.open && g_x.size) return g_x.size;
    {
        tl_dual_mapping *m = tl_find_stikdebug_prewarmed();
        return (m && m->size) ? m->size : 0;
    }
}
'''
out = src[:insert_at] + FN + src[insert_at:]
io.open(P, "w", encoding="utf-8", newline="").write(out)

print("inserted after:", " ".join(src[m.start():end + 1].split()))
print("\n".join(difflib.unified_diff(src.splitlines(), out.splitlines(), "before", "after", lineterm="", n=3))[:1500])
