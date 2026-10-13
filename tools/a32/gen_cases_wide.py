#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
gen_cases_wide.py -- the 37 wide/Thumb-2 + VFP-memory cases (ids 971..1007).

These cases USED to live only here, appended to gen_cases' base 970 by
`add_cases()`, and were therefore protected only by a local Unicorn run: the CI
oracle and the CI compare job both build from gen_cases.py's own
a32_cases_gen.{S,h}, so a case in a side file is, by this codebase's own hard
lesson (Thumb-1 SUB SP survived 970 green tests purely by not being tested), a
case that silently rots.

They now live in the SINGLE source of truth: gen_cases.py emits them itself
(gen_wide(), called at the end of build()), so both CI jobs run all 1007. This
module keeps only its two supporting jobs:

  * `add_cases()` -- kept for out-of-tree callers (verify_wide.py); a no-op,
    because gen_cases.build() already emits the cases;
  * `write_subset()` -- write JUST the new cases to their own a32_cases_wide_gen
    .S/.h (a dev convenience subset; the authoritative full set is
    a32_cases_gen.{S,h}).

    python3 tools/a32/gen_cases_wide.py           # write the subset
    python3 tools/a32/verify_wide.py              # run all 1007 vs Unicorn
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import gen_cases as G  # noqa: E402

WIDE_S = os.path.join(HERE, "a32_cases_wide_gen.S")
WIDE_H = os.path.join(HERE, "a32_cases_wide_gen.h")

# The base cases (ids 1..970). The new cases follow immediately; the split point
# is derived from gen_cases' own id accounting, never hard-coded past this.
BASE_N = 970


def add_cases():
    """Compatibility shim. gen_cases.build() already appends the 37 wide cases,
    so there is nothing left to append here. Idempotent by construction."""
    if not any(c["name"] == "w_mul" for c in G.CASES):
        raise RuntimeError(
            "gen_cases.build() did not emit the wide cases -- gen_wide() is "
            "missing from build()")
    return


def write_subset():
    """Emit ONLY the new cases to their own .S/.h (a dev subset), so a host can
    look at them apart from the base 970."""
    G.CASES.clear()
    G.build()
    add_cases()
    new = list(G.CASES)[BASE_N:]
    assert new, "no new cases were emitted by gen_cases.build()"
    assert new[0]["name"] == "w_mul" and len(new) == 37, \
        "expected 37 wide cases starting at w_mul, got %d starting at %s" % (
            len(new), new[0]["name"])
    saved = G.CASES
    G.CASES = new
    try:
        with open(WIDE_S, "w") as f:
            f.write(G.emit_s())
        with open(WIDE_H, "w") as f:
            f.write(G.emit_h())
    finally:
        G.CASES = saved
    print("gen_cases_wide.py: wrote %s and %s (%d new cases, ids %d..%d)"
          % (os.path.basename(WIDE_S), os.path.basename(WIDE_H),
             len(new), new[0]["id"], new[-1]["id"]))
    return new


if __name__ == "__main__":
    write_subset()
