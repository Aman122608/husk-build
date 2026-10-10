#!/bin/bash
# Build ANGLE (EGL + GLES over Metal) for iOS arm64.
#
# This is the host-side GPU stack Husk needs. virglrenderer turns the guest's
# GL calls into real draw calls, but it needs an EGL/GLES implementation to make
# them against -- and iOS has no EGL at all. ANGLE supplies one on top of Metal.
# UTM does the same thing, which is what makes GPU acceleration inside a QEMU
# guest on iOS a solved problem rather than a hope.
#
# ANGLE is taken from WebKit rather than upstream because WebKit carries an
# Xcode project for it, which avoids needing depot_tools and gn. A blob-filtered
# sparse checkout keeps that to ~260 MB instead of cloning all of WebKit.
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GPU="$HUSK_ROOT/third_party/gpu"
WK="$GPU/webkit"
PREFIX="$HUSK_ROOT/build/ios-arm64/sysroot"
LOG="$HUSK_ROOT/build/logs/angle-build.log"
mkdir -p "$GPU" "$(dirname "$LOG")"

if [ ! -d "$WK" ]; then
    echo "==> sparse-cloning WebKit for its ANGLE tree"
    git clone --depth 1 --filter=blob:none --sparse \
        https://github.com/WebKit/WebKit.git "$WK"
fi

# Configurations and Tools/ccache are not optional: the ANGLE xcconfigs include
# ../../../../Tools/ccache/ccache.xcconfig, and the build fails to even start
# without it.
( cd "$WK" && git sparse-checkout set \
    Source/ThirdParty/ANGLE Configurations Tools/Scripts Tools/ccache )

cd "$WK/Source/ThirdParty/ANGLE"

if [ "${HUSK_ANGLE_PROBE:-0}" = "1" ]; then
    # Diagnostic. Instantiating an undefined template with a value makes clang name
    # that value in the diagnostic, which is the only way to read a constexpr
    # without running the build under a debugger.
    echo "==> probing ResourceMap.h"
    python3 - <<'PY'
p = "src/libANGLE/ResourceMap.h"
s = open(p).read()

macro_probe = (
    '#if defined(ANGLE_ENABLE_SHARE_CONTEXT_LOCK)\n'
    '#pragma message("PROBE: ANGLE_ENABLE_SHARE_CONTEXT_LOCK is DEFINED")\n'
    '#else\n'
    '#pragma message("PROBE: ANGLE_ENABLE_SHARE_CONTEXT_LOCK is NOT defined")\n'
    '#endif\n\n'
)
ns = ("namespace angle_probe {\n"
      "template <bool B> struct Bool;\n"
      "template <unsigned long long N> struct Size;\n"
      "}  // namespace angle_probe\n\n")

assert "\nnamespace gl" in s, "no namespace gl"
s = s.replace("\nnamespace gl", "\n" + macro_probe + ns + "namespace gl", 1)

old = "        static_assert(kNeedsLock);"
assert old in s, "assert site not found"
s = s.replace(old,
    "        angle_probe::Bool<kNeedsLock> angle_probe_fn_needs_lock;\n"
    "        angle_probe::Size<kAdditionalFlatResourcesSize> angle_probe_fn_additional;\n" + old, 1)

open(p, "w").write(s)
print("  injected probes")
PY
fi

# Two overrides worth explaining.
#
# WK_AVAILABILITY_OVERLAY_FLAGS points at a clang VFS overlay that WebKit
# generates to defuse availability annotations. Nothing in this subset of the
# tree generates it, so the build dies looking for availability-overlay.yaml.
# Clearing the flag drops the overlay -- which then surfaces the errors it was
# hiding, all of the form "MTLPixelFormatBC1_RGBA is only available on iOS
# 16.4". Those are enum constants, harmless to compile in; the one call that
# matters, supportsBCTextureCompression, sits behind @available, so the warning
# is turned off rather than the target raised. Husk deploys to 15.5 (TrollStore
# and Dopamine devices), and ANGLE is built to match.
ALIASES="$GPU/angle-aliases.txt"

angle_build () {
    # The alias flag has to reach xcodebuild as ONE argument. Written inline as
    # an unquoted ${1:+...} it word-splits at the space, xcodebuild sees a bare
    # "-Wl,-alias_list,..." and dies with "invalid option" -- which the error
    # filter below then hides, because it drops lines mentioning xcodebuild.
    # Build it in an array instead, and expand that array quoted.
    #
    # $(inherited) is for xcodebuild to expand, not the shell, so it stays
    # backslash-escaped here.
    local ldflags=()
    if [ -n "${1:-}" ]; then
        ldflags=("OTHER_LDFLAGS=\$(inherited) -Wl,-alias_list,$1")
    fi

    # -DANGLE_ENABLE_SHARE_CONTEXT_LOCK=1: required, not optional.
    #
    # ANGLE's GN build defines this (BUILD.gn: angle_enable_share_context_lock);
    # WebKit's Xcode project defines nothing of the sort, so building ANGLE the
    # Xcode way silently takes ResourceMap.h's "locks disabled" #else branch, where
    # kNeedsLock and kAdditionalFlatResourcesSize are constants that do not depend
    # on IDType. That makes clang evaluate the static_assert(kNeedsLock) inside
    # if constexpr (kAdditionalFlatResourcesSize > 0), even though the branch
    # should be discarded -- so the build dies on an assert that a dependency-based
    # discard is supposed to hide.
    #
    # Measured, not inferred: with a probe injected at the assert the compiler
    # reported ANGLE_ENABLE_SHARE_CONTEXT_LOCK undefined, kAdditionalFlatResourcesSize
    # == 0, kNeedsLock == false -- and instantiated the branch anyway. Defining the
    # macro makes both values depend on IDType (BufferID: 0x6000/true, every other
    # ID: 0/false), so the branch is correctly discarded for the lockless types and
    # the assert holds for BufferID. Pass 1 then builds clean.
    local cflags='$(inherited) -Wno-unguarded-availability-new -Wno-thread-safety-attributes -Wno-thread-safety-analysis -DANGLE_ENABLE_SHARE_CONTEXT_LOCK=1'
    local cxxflags="$cflags"

    # -Wno-thread-safety-*: ANGLE's src/common/SimpleMutex.h annotates a
    # std::mutex with acquire_capability/release_capability. clang 16+ diagnoses
    # that as -Wthread-safety-attributes, and WebKit's xcconfigs build with
    # -Werror, so the build dies on ANGLE's own annotation macros. It is ANGLE's
    # internal bookkeeping, not a defect in anything Husk links, so the two
    # diagnostics are silenced rather than the source patched -- ANGLE here is a
    # WebKit HEAD checkout, which is a moving target that a local patch would not
    # survive.
    env -i PATH="$PATH" HOME="$HOME" xcodebuild archive \
        -archivePath "ANGLE" -scheme "ANGLE" \
        -sdk iphoneos -arch arm64 -configuration Release \
        WEBCORE_LIBRARY_DIR="/usr/local/lib" NORMAL_UMBRELLA_FRAMEWORKS_DIR="" \
        CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
        WK_AVAILABILITY_OVERLAY_FLAGS="" WK_AVAILABILITY_OVERLAY_SWIFT_FLAGS="" \
        IPHONEOS_DEPLOYMENT_TARGET="15.5" \
        OTHER_CFLAGS="$cflags" \
        OTHER_CPLUSPLUSFLAGS="$cxxflags" \
        ${ldflags[@]+"${ldflags[@]}"} \
        > "$LOG" 2>&1 \
      || { echo "ANGLE build failed; last errors:" >&2
           grep -a "error:\|^ld: " "$LOG" | head -40 >&2; exit 1; }
}

# Two passes, because of a name mismatch.
#
# WebKit builds ANGLE with its entry points renamed -- the dylib exports
# EGL_ChooseConfig, not eglChooseConfig -- since WebCore reaches them through
# ANGLE's own loader. libepoxy and virglrenderer expect the standard names.
# Rather than hand-write ~115 forwarding functions, the second pass relinks with
# ld's -alias_list, which adds the standard name as an alias of each renamed
# symbol. No wrappers, no extra indirection at call time.
#
# EGL only, deliberately. Aliasing the 828 GL_* exports the same way fails with
# "ld: 828 duplicate symbols" -- ANGLE already carries internal gl* symbols that
# the aliases collide with. It is also unnecessary: epoxy resolves GL entry
# points through eglGetProcAddress, so EGL is the only surface that must be
# reachable by its standard name.
echo "==> building ANGLE for iOS arm64, pass 1 (log: $LOG)"
angle_build ""

echo "==> generating EGL symbol aliases"
nm -g "ANGLE.xcarchive/Products/usr/local/lib/libANGLE-shared.dylib" \
  | awk '$2=="T"{print $3}' | grep -E '^_EGL_' \
  | awk '{o=$1; a=$1; sub(/^_EGL_/,"_egl",a); print o, a}' > "$ALIASES"
echo "    $(wc -l < "$ALIASES") aliases"

echo "==> pass 2, relinking with standard EGL names"
angle_build "$ALIASES"

DYLIB="ANGLE.xcarchive/Products/usr/local/lib/libANGLE-shared.dylib"
[ -f "$DYLIB" ] || { echo "no dylib at $DYLIB" >&2; exit 1; }

install -d "$PREFIX/lib" "$PREFIX/include"
cp "$DYLIB" "$PREFIX/lib/"
rsync -a "include/" "$PREFIX/include/"

# WebKit ships it for loading out of WebCore.framework; ours is embedded in the
# app, so the install name has to say so or dyld will not find it at runtime.
install_name_tool -id "@rpath/libANGLE-shared.dylib" "$PREFIX/lib/libANGLE-shared.dylib"

echo "==> staged $(ls -lh "$PREFIX/lib/libANGLE-shared.dylib" | awk '{print $5}') to $PREFIX/lib"
otool -D "$PREFIX/lib/libANGLE-shared.dylib" | tail -1
echo "==> standard EGL entry points exported: $(nm -g "$PREFIX/lib/libANGLE-shared.dylib" | grep -c ' T _egl')"
