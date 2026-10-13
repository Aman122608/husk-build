#!/usr/bin/env python3
"""
Verify that a built IPA really carries the Java framework built from THIS repo.

Why this exists: it happened. `husk-framework.dex` in the shipped IPA was the
*binary donor's* frozen copy while the repo's build step failed to compile -- and
because that step is `continue-on-error: true`, the workflow reported success and
the IPA shipped a stale framework. The result was that every change to
src/java-framework (the ad refusal, the touchable-web-page rule, the resource
tables, the activity lifecycle) was dead code in every build, with nothing in the
UI or the log to say so.

Checking the *artifact* rather than the step is the only version of this check
that cannot be fooled: a build step can fail quietly, but the bytes in the IPA
are the bytes the phone runs.

Usage:
    python tools/check-shipped-dex.py <ipa> [--marker NAME]... [--donor-dex PATH]

  --marker NAME   a class or method name that exists only in this repo's Java
                  source (defaults below). Each must appear in the shipped dex.
  --donor-dex     the donor IPA's husk-framework.dex, to assert the shipped one
                  is not byte-identical to it.

Exit: 0 = the shipped dex is the repo's own. 1 = it is stale (or missing).
"""
import argparse
import hashlib
import os
import sys
import zipfile

DEFAULT_MARKERS = ["resetForNewApp", "AdSurfaces"]
DEX_HINTS = ("husk-framework.dex",)


def find_dex(z):
    for n in z.namelist():
        if n.endswith(DEX_HINTS):
            return n
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ipa")
    ap.add_argument("--marker", action="append", default=[])
    ap.add_argument("--donor-dex", default=None)
    a = ap.parse_args()
    markers = a.marker or DEFAULT_MARKERS

    if not os.path.exists(a.ipa):
        print("check-dex: no such IPA: %s" % a.ipa)
        return 1
    z = zipfile.ZipFile(a.ipa)
    name = find_dex(z)
    if not name:
        print("check-dex: FAIL -- the IPA has no husk-framework.dex at all")
        return 1
    data = z.read(name)
    digest = hashlib.sha256(data).hexdigest()
    print("check-dex: %s -> %s (%d bytes, sha256 %s)" % (a.ipa, name, len(data), digest[:16]))

    bad = []
    for m in markers:
        hit = m.encode() in data
        print("  %-8s %s" % ("OK" if hit else "MISSING", m))
        if not hit:
            bad.append(m)

    if a.donor_dex:
        if not os.path.exists(a.donor_dex):
            print("  (donor dex %s is absent: skipping the identity check)" % a.donor_dex)
        else:
            dd = hashlib.sha256(open(a.donor_dex, "rb").read()).hexdigest()
            same = dd == digest
            print("  %-8s not the donor's dex (donor sha256 %s)" % ("OK" if not same else "STALE", dd[:16]))
            if same:
                bad.append("byte-identical to the donor's dex")

    if bad:
        print("")
        print("check-dex: FAIL -- this IPA carries the DONOR's framework, not this repo's.")
        print("  missing: %s" % ", ".join(bad))
        print("  Every change to src/java-framework is dead code in this IPA.")
        print("  Fix the compile error in the step 'Build Husk's Java framework (from this repo)'")
        print("  and rebuild; do not ship this IPA to a phone.")
        return 1
    print("check-dex: PASS -- the shipped framework is the one built from this repo.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
