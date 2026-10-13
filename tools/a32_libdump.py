import os, zipfile, json
OUT = r"C:/Users/Aiden/Desktop/HuskApp/Games32"
rows = []
for fn in sorted(os.listdir(OUT)):
    if not fn.lower().endswith(".apk"):
        continue
    p = os.path.join(OUT, fn)
    zf = zipfile.ZipFile(p)
    libs = sorted(n for n in zf.namelist() if n.startswith("lib/"))
    abis = sorted({n.split("/")[1] for n in libs if n.count("/") >= 2})
    print("== %s  (%d bytes)" % (fn, os.path.getsize(p)))
    print("   ABI dirs:", abis)
    for l in libs:
        print("     ", l)
