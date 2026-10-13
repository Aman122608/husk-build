#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build.sh <target> [out] -- build one piece of the AArch32 differential harness.
#
#   step0    the go/no-go: does this host execute code the process just wrote?
#            (drives src/translation-layer-next/husk-tl-xmem.c's own self-test)
#   diff     the macos-26 comparer (a32diff) + the executable-memory proof
#   oracle   the armhf AArch32 oracle (needs arm-linux-gnueabi-gcc; run under qemu-arm)
#
# For step0/diff this mirrors tools/regress/build.sh: clang, the two include
# dirs, and husk-tl-xmem.c. No Apple frameworks are needed for the AArch32 path.
#
#   Tools: bash tools/a32/build.sh step0            # then TL_XMEM_SELF=1 <out>
#   The whole local loop (no Mac, no CI):
#     python3 tools/a32/gen_cases.py
#     bash tools/a32/build.sh oracle /tmp/a32oracle
#     qemu-arm -cpu cortex-a15 /tmp/a32oracle /tmp/a32-goldens
#     bash tools/a32/build.sh diff   /tmp/a32diff
#     /tmp/a32diff /tmp/a32-goldens
set -e
R="$(cd "$(dirname "$0")/../.." && pwd)"
N="$R/src/translation-layer-next"
T="$R/src/translation-layer"
A="$R/tools/a32"

target="${1:?usage: build.sh step0|diff|oracle [out]}"
out="${2:-}"

case "$target" in
  step0)
    out="${out:-/tmp/a32-step0}"
    rm -f "$out"
    clang -g -O1 -Wall -I"$T" -I"$N" -o "$out" "$A/step0_xmem.c" "$N/husk-tl-xmem.c" -lpthread
    echo "built $out"
    echo "run:   TL_XMEM_SELF=1 $out   # the layer's own self-test"
    echo "       $out                   # the ordinary Mac two-view path"
    ;;
  diff)
    out="${out:-/tmp/a32diff}"
    rm -f "$out"
    clang -g -O1 -Wall -I"$T" -I"$N" -o "$out" "$A/a32diff.c" "$N/husk-tl-xmem.c" -lpthread
    echo "built $out"
    echo "run:   $out <goldens-dir> [--self-check]"
    ;;
  oracle)
    out="${out:-/tmp/a32oracle}"
    cc="${A32_CC:-arm-linux-gnueabi-gcc}"
    command -v "$cc" >/dev/null || { echo "$cc not found (apt-get install gcc-arm-linux-gnueabi)" >&2; exit 2; }
    rm -f "$out"
    "$cc" -std=gnu11 -O1 -marm -march=armv7-a -mfpu=vfpv3 -mfloat-abi=soft \
          -D_GNU_SOURCE -Wall -I"$A" -static -o "$out" "$A/a32oracle.c" "$A/a32_cases_gen.S"
    echo "built $out"
    echo "run:   qemu-arm -cpu cortex-a15 $out <outdir>"
    ;;
  *)
    echo "unknown target: $target (want step0, diff or oracle)" >&2
    exit 2
    ;;
esac
