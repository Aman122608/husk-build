#!/usr/bin/env python3
"""refuse_probe.py -- DEV-only: show the exact REFUSED encodings (raw bytes +
capstone text) that real game code executes, so a decoder fix works from the
real encoding rather than a guess. Reuses dynmix.py's Unicorn harness.

    python tools/a32/refuse_probe.py --per-lib 400 --max-steps 1500 [--top 120]
"""
import argparse, collections, os, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dynmix as D
import traffic as T


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apk-dir", default="Games32")
    ap.add_argument("--per-lib", type=int, default=400)
    ap.add_argument("--max-steps", type=int, default=1500)
    ap.add_argument("--top", type=int, default=120)
    args = ap.parse_args()

    eng = D.Engines()
    dll = D.build_shim()
    interp = D.Interp(dll) if dll else None
    assert interp, "no shim built"

    refused = collections.Counter()       # (mode, size, raw) -> count
    apks = sorted(f for f in os.listdir(args.apk_dir) if f.endswith(".apk"))
    for apk in apks:
        with zipfile.ZipFile(os.path.join(args.apk_dir, apk)) as z:
            names = [n for n in z.namelist()
                     if T.ABI_KEEP.match(n) and n.endswith(".so")]
            for n in sorted(names):
                data = z.read(n)
                img = D.LibImage(data, n)
                entries, method = D.inventory(img)
                for (faddr, thumb) in D.sample(entries, args.per_lib):
                    st = D.run_function(eng, img, faddr, thumb, args.max_steps)
                    for (mode, size, word), cnt in st["enc"].items():
                        raw = word.to_bytes(size, "little")
                        is_ref, reason = interp.refuses(mode, raw)
                        if is_ref:
                            refused[(mode, size, word)] += cnt
                sys.stderr.write("[%s] %s: %d distinct refused so far\n"
                                 % (apk, n, len(refused)))

    print("\n=== top %d refused encodings (dynamic weight) ===" % args.top)
    for (mode, size, word), cnt in refused.most_common(args.top):
        raw = word.to_bytes(size, "little")
        md = eng.arm if mode == "A32" else eng.thumb
        ins = next(md.disasm(raw, 0, count=1), None)
        text = ("%s %s" % (ins.mnemonic, ins.op_str)) if ins else "?invalid"
        is_ref, reason = interp.refuses(mode, raw)
        print("%6d  %-4s sz%d  %-10s  %-34s  %s"
              % (cnt, mode, size, raw.hex(), text, reason))


if __name__ == "__main__":
    main()
