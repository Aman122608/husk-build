#!/bin/bash
# Build Husk.ipa WITHOUT compiling the graphics stack.
#
# ANGLE, virglrenderer, MoltenVK and QEMU are the great majority of a cold build
# (QEMU alone is tens of minutes to over an hour). A change in the translation
# layer -- src/translation-layer-next, which is compiled into the APP binary, not
# into any of those libraries -- does not need them rebuilt to be tested on a
# device. Take them from a released Husk IPA instead: those binaries are
# known-good, because they are what already runs every other Android game on Husk.
#
# What this does NOT validate: our own ANGLE / virglrenderer / MoltenVK / QEMU
# builds. scripts/ci_build.sh is the from-source path. This script exists so a
# translation-layer fix can reach a device without waiting for the graphics
# configuration to be settled.
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HUSK_ROOT/build/Husk.ipa}"
TAG="${HUSK_UPSTREAM_TAG:-1.1.0}"
WORK="$HUSK_ROOT/build/upstream-$TAG"
IPA="$WORK/Husk.ipa"
SYSROOT_LIB="$HUSK_ROOT/build/ios-arm64/sysroot/lib"
APP_LIB="$HUSK_ROOT/build/ios-arm64/lib"

cd "$HUSK_ROOT"
step() { printf '\n\033[1;34m##### %s\033[0m\n' "$*"; }

step "fetch upstream Husk $TAG (binary donor)"
mkdir -p "$WORK"
if [ ! -s "$IPA" ]; then
    curl -fL --retry 3 -o "$IPA.part" \
        "https://github.com/Leviidev/Husk/releases/download/$TAG/Husk.ipa"
    mv "$IPA.part" "$IPA"
fi
ls -lh "$IPA"

step "unpack the donor"
rm -rf "$WORK/x"
mkdir -p "$WORK/x"
( cd "$WORK/x" && unzip -q "$IPA" )
DONOR="$WORK/x/Payload/Husk.app/Frameworks"
for f in libANGLE-shared.dylib libqemu-aarch64-softmmu.dylib; do
    [ -f "$DONOR/$f" ] || { echo "donor IPA has no $f" >&2; exit 1; }
done
[ -d "$DONOR/MoltenVK.framework" ] || { echo "donor IPA has no MoltenVK.framework" >&2; exit 1; }

step "stage ANGLE + QEMU from the donor"
mkdir -p "$SYSROOT_LIB" "$APP_LIB"
install -m 755 "$DONOR/libANGLE-shared.dylib" "$SYSROOT_LIB/libANGLE-shared.dylib"
install -m 755 "$DONOR/libqemu-aarch64-softmmu.dylib" "$APP_LIB/libqemu-aarch64-softmmu.dylib"

# The donor ships MoltenVK as a framework, because that is what its own app
# embeds. project.yml asks for an xcframework, so wrap the same binary in one
# rather than shipping a different build of MoltenVK than the games were tested on.
step "stage MoltenVK as an xcframework"
XCF="$APP_LIB/MoltenVK.xcframework"
rm -rf "$XCF"
mkdir -p "$XCF/ios-arm64"
cp -R "$DONOR/MoltenVK.framework" "$XCF/ios-arm64/MoltenVK.framework"
cat > "$XCF/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundlePackageType</key><string>XFWK</string>
    <key>XCFrameworkFormatVersion</key><string>1.0</string>
    <key>AvailableLibraries</key>
    <array>
        <dict>
            <key>LibraryIdentifier</key><string>ios-arm64</string>
            <key>LibraryPath</key><string>MoltenVK.framework</string>
            <key>SupportedArchitectures</key><array><string>arm64</string></array>
            <key>SupportedPlatform</key><string>ios</string>
        </dict>
    </array>
</dict>
</plist>
PLIST

step "guest kernel + firmware"
./scripts/fetch_phase0_guest.sh

step "on-device pairing (Rust)"
./scripts/build_rppairing_ios.sh

step "regenerate the Xcode project"
# package_ipa.sh generates with --quiet and ignores the exit status, so a failure
# there surfaces only as xcodebuild's "Unable to read project". Do it here, loudly,
# and stop on the real error instead of a symptom of it.
( cd src/app && xcodegen --version && xcodegen generate ) \
    || { echo "xcodegen failed; project dir is:" >&2; ls -la src/app/Husk.xcodeproj >&2 || true; exit 1; }

step "app + IPA"
mkdir -p "$(dirname "$OUT")"
# Keep DerivedData (and therefore build.log) inside the workspace: package_ipa.sh
# defaults to /tmp/husk_ipa, where a failure's log cannot be uploaded or read.
export DD="$HUSK_ROOT/build/DerivedData"
if ! ./scripts/package_ipa.sh "$OUT"; then
    # package_ipa.sh pipes xcodebuild through `grep -E "error:|BUILD ..."`, so a
    # failure can surface as a bare "BUILD FAILED" with none of the reasons. Show
    # them here instead of leaving a run that says nothing.
    echo "===== xcodebuild diagnostics ($DD/build.log) =====" >&2
    grep -nE "error:|BUILD FAILED|The following build commands failed|fatal error|❌" "$DD/build.log" 2>/dev/null | head -40 >&2
    echo "===== last 60 lines =====" >&2
    tail -60 "$DD/build.log" 2>/dev/null >&2
    exit 1
fi
./scripts/package_ipa.sh --trollstore "${OUT%.ipa}.tipa"

step "manifest: which binaries are whose"
for f in "$SYSROOT_LIB/libANGLE-shared.dylib" "$APP_LIB/libqemu-aarch64-softmmu.dylib"; do
    printf '%s  %s\n' "$(shasum -a 256 "$f" | cut -d' ' -f1)" "$(basename "$f")"
done
echo "graphics/QEMU stack: upstream Husk $TAG (unchanged)"
echo "app binary: built from this tree at $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
