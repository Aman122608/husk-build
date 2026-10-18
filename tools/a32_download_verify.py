#!/usr/bin/env python3
"""Download archive.org APKs and verify ABI dirs + package name."""
import os, sys, json, zipfile, urllib.request, urllib.parse, re

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Games32")

ENGINE_HINTS = {
    "libunity.so": "Unity (mono)", "libil2cpp.so": "Unity IL2CPP", "libmain.so": "Unity (il2cpp libmain)",
    "libcocos2dcpp.so": "cocos2d-x", "libcocos2djs.so": "cocos2d-js", "libPVZ2.so": "PopCap",
    "libflutter.so": "Flutter", "libUE4.so": "Unreal UE4", "libgame.so": "generic native libgame.so",
    "libminecraftpe.so": "Minecraft PE native", "libmonosgen": "Mono", "libsvg.so": "cocos",
    "libbox2d.so": "Box2D", "libplasma.so": "PopCap/EA Plasma", "libPVZ.so": "PopCap",
    "libpaperclips": "AirPlay", "libairplay": "AirPlay (Adobe AIR)", "libAdobe": "Adobe AIR",
    "libgdx": "libGDX", "libjni": "custom JNI", "libobby.so": "Unity", "libopentk": "OpenTK",
    "libhershey": "custom", "libnativetest": "custom",
}

def abi_dirs(names):
    dirs = set()
    for n in names:
        if n.startswith("lib/"):
            parts = n.split("/")
            if len(parts) >= 2:
                dirs.add(parts[1])
    return sorted(dirs)

def engine_from_libs(names):
    libs = [n.split("/")[-1] for n in names if n.startswith("lib/")]
    hits = []
    for lib in libs:
        base = lib
        for k, v in ENGINE_HINTS.items():
            if base.startswith(k):
                hits.append(lib)
                break
    return hits, libs

def package_name(zf):
    try:
        data = zf.read("AndroidManifest.xml")
    except Exception:
        return None
    # binary XML: look for utf-16 "package" attr, next utf-16 string is value
    for enc in ("utf-16-le", "utf-16-be"):
        try:
            needle = "package".encode(enc)
        except Exception:
            continue
        idx = data.find(needle)
        if idx < 0:
            continue
        # find end of "package" then next utf16 string start
        pos = idx + len(needle)
        # skip small filler; read following utf16 chars
        seg = data[pos:pos + 200]
        s = seg.decode(enc, "ignore")
        m = re.search(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+){1,5}", s)
        if m and "." in m.group(0):
            return m.group(0)
    # fallback: regex over both decodings
    for enc in ("utf-16-le", "utf-16-be", "latin-1"):
        try:
            s = data.decode(enc, "ignore")
        except Exception:
            continue
        cands = re.findall(r"(?:com|org|net|io|me|air|jp|cn|ru)\.[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+){1,4}", s)
        if cands:
            return cands[0]
    return None

def download(ident, fname, outname):
    dest = os.path.join(OUT, outname)
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return dest, "cached"
    url = "https://archive.org/download/" + ident + "/" + urllib.parse.quote(fname, safe="")
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    tmp = dest + ".part"
    with urllib.request.urlopen(req, timeout=300) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
    os.replace(tmp, dest)
    return dest, url

def main():
    os.makedirs(OUT, exist_ok=True)
    cands = json.load(open(sys.argv[1]))
    results = []
    for c in cands:
        ident, fname, outname = c["ident"], c["file"], c["out"]
        rec = {"out": outname, "ident": ident, "file": fname}
        try:
            dest, how = download(ident, fname, outname)
            rec["bytes"] = os.path.getsize(dest)
            rec["src"] = how if how == "cached" else how
        except Exception as e:
            rec["error"] = "download: " + repr(e)[:160]
            results.append(rec); print(json.dumps(rec)); continue
        try:
            zf = zipfile.ZipFile(dest)
            names = zf.namelist()
            if "AndroidManifest.xml" not in names:
                rec["error"] = "not an APK (no AndroidManifest.xml)"
            else:
                abis = abi_dirs(names)
                hits, all_libs = engine_from_libs(names)
                rec["abis"] = abis
                rec["pkg"] = package_name(zf)
                rec["n_libs"] = len(all_libs)
                rec["engine_libs"] = hits
                rec["arm64"] = "arm64-v8a" in abis
            zf.close()
        except Exception as e:
            rec["error"] = "zip: " + repr(e)[:160]
        results.append(rec)
        print(json.dumps(rec), flush=True)
    json.dump(results, open(os.path.join(OUT, "_verify.json"), "w"), indent=1)

if __name__ == "__main__":
    main()
