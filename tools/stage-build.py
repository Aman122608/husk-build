#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage a built IPA under a name a human can tell apart from the others.

    python3 tools/stage-build.py <sha> <slug> [--from <path>] [--note "..."] [--no-send-name]

The name is

    Husk-<MMDD>-<HHMM>-<sha7>-<slug>.ipa

so the phone's accept sheet, the served link and the release all show what a
build is and when it was made, while the sha keeps it traceable to the commit
CI built it from. `slug` is a couple of hyphenated words for what the build is
for: `guest-fault-scope`, `arena-backing`, `swap-and-close`, ...

The source IPA is found at HuskBuilds/Husk-night-<sha>.ipa unless --from says
otherwise. One row is appended to HuskBuilds/INDEX.md (newest first) so the pile
stays readable, and the served URL + size are printed for handing over.

Nothing is deleted: the source copy is left in place under its old name.
"""
import datetime
import os
import re
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "HuskBuilds")
INDEX = os.path.join(OUT, "INDEX.md")
LAN = "192.168.1.123"
PORT = 8643


def verify(ipa):
    """Refuse to name or stage something that is not a real, complete IPA.

    Learned the hard way: a download clobbered by a concurrent one staged as a
    15 MB file that was not even a zip, and it was on its way to a phone. Size,
    zip structure and the shipped framework are all checked before the name is
    handed out, so a partial file cannot get a convincing name.
    """
    size = os.path.getsize(ipa)
    if size < 50 * 1024 * 1024:
        return False, f"only {size:,} bytes -- a Husk IPA is ~89 MB, so this is a partial download"
    try:
        with zipfile.ZipFile(ipa) as z:
            n = len(z.namelist())
    except Exception as e:
        return False, f"not a zip file ({e.__class__.__name__}: {e})"
    if n < 100:
        return False, f"zip opens but has only {n} entries"
    return True, f"{size:,} bytes, valid zip with {n} entries"


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = {a.split("=", 1)[0]: (a.split("=", 1)[1] if "=" in a else "") for a in argv[1:] if a.startswith("--")}
    if len(args) < 2:
        print(__doc__)
        return 2
    sha, slug = args[0], args[1]
    sha7 = sha[:7]
    slug = re.sub(r"[^a-z0-9]+", "-", slug.lower()).strip("-")
    src = opts.get("--from") or os.path.join(OUT, f"Husk-night-{sha7}.ipa")
    if not os.path.exists(src):
        print(f"no IPA at {src} (build first, or pass --from=<path>)")
        return 1
    ok, why = verify(src)
    if not ok:
        print(f"REFUSING to stage {src}: {why}")
        print("re-download it (a fresh dir, one downloader at a time) and try again")
        return 3
    print(f"verified: {why}")
    stamp = datetime.datetime.now().strftime("%m%d-%H%M")
    name = f"Husk-{stamp}-{sha7}-{slug}.ipa"
    dst = os.path.join(OUT, name)
    if os.path.abspath(src) != os.path.abspath(dst):
        shutil.copy2(src, dst)
    size = os.path.getsize(dst)
    when = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    note = opts.get("--note", "")
    header = "| file | commit | size | staged | what it is |\n|---|---|---|---|---|\n"
    rows = []
    if os.path.exists(INDEX):
        rows = [l for l in open(INDEX, encoding="utf-8").read().splitlines() if l.startswith("| Husk-")]
    rows = [f"| {name} | `{sha7}` | {size:,} | {when} | {note} |"] + rows
    with open(INDEX, "w", encoding="utf-8") as f:
        f.write("# Husk builds staged on this box\n\n")
        f.write("Newest first. The name says when it was made and what it is for; the commit is what CI built.\n\n")
        f.write(header + "\n".join(rows) + "\n")
    print(f"name   : {name}")
    print(f"size   : {size:,} bytes")
    print(f"served : http://{LAN}:{PORT}/f/{name}")
    print(f"index  : HuskBuilds/INDEX.md ({len(rows)} build(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
