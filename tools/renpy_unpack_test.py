#!/usr/bin/env python3
# A faithful port of the C in husk-tl-sdl-apps.c (rp_gunzip + rp_extract_tar), run against the
# APK's assets/private.mp3, to check the algorithm the C implements. Same steps, same offsets.
import zipfile, zlib, io, os, sys, tempfile, shutil

APK = r"C:\Users\Aiden\Desktop\HuskApp\TheLewdestHouse-0-2-42.apk"
DEST = os.path.join(tempfile.gettempdir(), "renpy_private_test")

def rp_gunzip(data):
    # inflateInit2(15+32): gzip/zlib auto
    return zlib.decompress(data, 15 + 32)

def rp_octal(b, off, n):
    v = 0
    for i in range(n):
        c = b[off + i]
        if c < ord('0') or c > ord('7'):
            break
        v = v * 8 + (c - ord('0'))
    return v

def rp_tar_name_ok(name):
    if not name or name[0] == '/':
        return False
    for seg in name.split('/'):
        if seg == '..':
            return False
    return True

def rp_extract_tar(tar, dest):
    off = 0
    files = 0
    dirs = 0
    longname = b""
    n = len(tar)
    while off + 512 <= n:
        h = tar[off:off + 512]
        if h[0] == 0:
            break
        nl = 0
        while nl < 100 and h[nl]:
            nl += 1
        name = h[:nl].decode('utf-8', 'replace')
        sz = rp_octal(h, 124, 12)
        typ = chr(h[156])
        data_at = off + 512
        nxt = data_at + (((sz if sz > 0 else 0) + 511) // 512) * 512
        if nxt > n or nxt < off:
            break
        use = longname.decode('utf-8', 'replace') if longname else name
        longname = b""
        if typ == 'L':
            longname = tar[data_at:data_at + min(sz, 1023)]
            off = nxt
            continue
        if rp_tar_name_ok(use):
            path = os.path.join(dest, use)
            if typ == '5':
                os.makedirs(path, exist_ok=True)
                dirs += 1
            elif typ in ('0', '\x00', '7'):
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, 'wb') as f:
                    if sz > 0:
                        f.write(tar[data_at:data_at + sz])
                files += 1
        off = nxt
    return files, dirs

z = zipfile.ZipFile(APK)
info = z.getinfo('assets/private.mp3')
raw = z.read('assets/private.mp3')
print("private.mp3: method=%d usize=%d csize=%d, first bytes %s" % (info.compress_type, info.file_size, info.compress_size, raw[:2].hex()))
tar = rp_gunzip(raw)
print("gunzipped tar: %d bytes (%d x 512 blocks)" % (len(tar), len(tar) // 512))

if os.path.isdir(DEST):
    shutil.rmtree(DEST)
os.makedirs(DEST)
files, dirs = rp_extract_tar(tar, DEST)
print("extracted: %d files, %d dirs" % (files, dirs))

for key in ("main.py", "renpy/__init__.py", "renpy/__init__.pyc", "lib/python3.9/os.py", "lib/python3.9/lib-dynload"):
    p = os.path.join(DEST, key)
    print("  %-34s %s" % (key, "present (%d bytes)" % os.path.getsize(p) if os.path.isfile(p) else ("dir" if os.path.isdir(p) else "MISSING")))
# a couple of extension modules must survive
mods = [f for f in os.listdir(os.path.join(DEST, "lib/python3.9/lib-dynload"))] if os.path.isdir(os.path.join(DEST, "lib/python3.9/lib-dynload")) else []
print("lib-dynload .so count:", sum(1 for m in mods if m.endswith('.so')), "of", len(mods), "->", mods[:6])
print("total bytes written:", sum(os.path.getsize(os.path.join(dp, f)) for dp, dn, fn in os.walk(DEST) for f in fn))
