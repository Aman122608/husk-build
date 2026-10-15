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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "HuskBuilds")
INDEX = os.path.join(OUT, "INDEX.md")
LAN = "192.168.1.123"
PORT = 8643


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
