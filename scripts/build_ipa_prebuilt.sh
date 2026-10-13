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
# builds, or the Java runtime and Android system it stages from another released
# IPA (the step below). scripts/ci_build.sh is the from-source path. This script
# exists so a translation-layer fix can reach a device without waiting for the
# graphics configuration to be settled.
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HUSK_ROOT/build/Husk.ipa}"
TAG="${HUSK_UPSTREAM_TAG:-1.1.0}"
WORK="$HUSK_ROOT/build/upstream-$TAG"
IPA="$WORK/Husk.ipa"
SYSROOT_LIB="$HUSK_ROOT/build/ios-arm64/sysroot/lib"
APP_LIB="$HUSK_ROOT/build/ios-arm64/lib"

# Whether the QEMU-under-Android guest is IN this IPA.
#
#   HUSK_BUNDLE_GUEST=0 (default)  no guest payload: no UEFI firmware, no pc-bios
#                                  ROMs, no Alpine Phase 0 kernel/initramfs, no
#                                  qcow2 seeds. The VM cannot boot in this build;
#                                  the app says so by name instead of failing
#                                  somewhere inside QEMU.
#   HUSK_BUNDLE_GUEST=1            the full guest, as every build had it.
#
# What this switch does NOT remove, and why: libqemu-aarch64-softmmu.dylib. Its
# name says "QEMU", but the same library also exports husk_ios_jit_prewarm /
# husk_ios_jit_get_mapping (scripts/integrate_husk.sh, system/qemu.symbols) --
# the JIT region the app claims and the native translation layer looks up by
# name with dlsym (src/translation-layer/husk-tl-load.c:522,529). Dropping the
# dylib therefore disables JIT for the guest AND the native runtime at once; it
# is a library split, not a payload deletion. See
# .hermes/plans/engine-reset/guest-unbundled.md.
HUSK_BUNDLE_GUEST="${HUSK_BUNDLE_GUEST:-0}"
export HUSK_BUNDLE_GUEST

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

step "stage QEMU's pc-bios data"
# The app's "Stage guest image" build phase reads QEMU's pc-bios directory, which
# only exists once QEMU has been built. Take the same ROMs and the same UEFI
# firmware the donor's QEMU uses. The firmware is 64 MiB of mostly zeros and the
# phase bunzip2s it, so repack the .fd the donor already ships rather than
# re-downloading QEMU's sources for one file.
#
# Only for a guest-bundling build. This is the payload that makes the VM boot, so
# nothing is staged when the guest is excluded -- and project.yml's build phase
# reads the same HUSK_BUNDLE_GUEST and ships neither.
DONOR_APP="$WORK/x/Payload/Husk.app"
BIOS="$HUSK_ROOT/third_party/build/qemu-10.0.12-utm/pc-bios"
if [ "$HUSK_BUNDLE_GUEST" = "1" ]; then
    mkdir -p "$BIOS"
    cp "$DONOR_APP/pc-bios/"*.rom "$BIOS/" 2>/dev/null || true
    if [ -f "$DONOR_APP/edk2-aarch64-code.fd" ]; then
        bzip2 -9 -c "$DONOR_APP/edk2-aarch64-code.fd" > "$BIOS/edk2-aarch64-code.fd.bz2"
    fi
    [ -s "$BIOS/edk2-aarch64-code.fd.bz2" ] \
        || { echo "donor IPA has no edk2-aarch64-code.fd to stage" >&2; exit 1; }
    echo "guest payload: bundling UEFI firmware + pc-bios ROMs"
else
    step "guest payload: NOT bundled (HUSK_BUNDLE_GUEST=0)"
    echo "  excluded: edk2-aarch64-code.fd, pc-bios/*.rom, vmlinuz-virt, initramfs-virt, qcow2 seeds"
    echo "  the Android VM cannot boot in this IPA; the app names the missing piece at launch"
    echo "  set HUSK_BUNDLE_GUEST=1 to ship the guest again"
fi

step "java runtime + Android system (from the 2.0 release IPA)"
# Two more binaries this path reuses from a released IPA, and the reason a build from this script
# could never run a Java app: the app's own build phase stages build/java-runtime and
# build/android-system when they are there, and 1.1.0 -- the graphics donor above -- carries
# neither, so every Java/Dalvik app (PvZ2, libGDX, Flutter) died at "no java/lang/Object on the
# boot class path" with nothing in the log to say why. Husk 2.0's IPA ships both: the ART jars and
# natives laid out as APEXes, Husk's Java framework, framework-res (stripped), and the system fonts
# and cacerts. Taken as released and checked, exactly like the graphics stack. The same caveat
# applies: a change to src/java-framework or to a Java shim in this tree is NOT in here, so rebuild
# the runtime with scripts/build_java_runtime.sh rather than reusing this when that is what is
# under test.
JAVA_DONOR_TAG="${HUSK_JAVA_DONOR_TAG:-2.0}"
JR_WORK="$HUSK_ROOT/build/java-donor-$JAVA_DONOR_TAG"
JR_IPA="$JR_WORK/Husk.ipa"
mkdir -p "$JR_WORK"
if [ ! -s "$JR_IPA" ]; then
    curl -fL --retry 3 -o "$JR_IPA.part" \
        "https://github.com/Leviidev/Husk/releases/download/$JAVA_DONOR_TAG/Husk.ipa"
    mv "$JR_IPA.part" "$JR_IPA"
fi
rm -rf "$JR_WORK/x"; mkdir -p "$JR_WORK/x"
( cd "$JR_WORK/x" && unzip -q "$JR_IPA" )
JR_APP="$JR_WORK/x/Payload/Husk.app"
[ -f "$JR_APP/java-runtime/husk-framework.dex" ] \
    || { echo "donor $JAVA_DONOR_TAG has no java-runtime to stage" >&2; exit 1; }
[ -d "$JR_APP/android-system/fonts" ] \
    || { echo "donor $JAVA_DONOR_TAG has no android-system to stage" >&2; exit 1; }
rm -rf "$HUSK_ROOT/build/java-runtime" "$HUSK_ROOT/build/android-system"
rsync -a "$JR_APP/java-runtime/" "$HUSK_ROOT/build/java-runtime/"
# Husk's own Java framework, if CI built it from this tree (the "Build Husk's Java framework" step in
# build-ipa-prebuilt.yml): ours wins over the donor's, because a framework change made here is
# otherwise dead code -- the donor's dex is frozen at its release. Everything else in java-runtime
# (the ART jars, ICU, conscrypt, framework-res) still comes from the donor, which is the point of this
# path. When it is absent the donor's dex ships and the log says so, so a stale framework is never
# silently assumed to be current.
if [ -s "$HUSK_ROOT/build/husk-framework.dex" ]; then
    install -m 644 "$HUSK_ROOT/build/husk-framework.dex" "$HUSK_ROOT/build/java-runtime/husk-framework.dex"
    step_note="husk-framework.dex: built from this repo ($(git -C "$HUSK_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown))"
else
    step_note="husk-framework.dex: donor $JAVA_DONOR_TAG (repo build absent -- framework changes are NOT in this IPA)"
fi
echo "$step_note" | tee -a "$HUSK_ROOT/build/logs/framework-dex.txt" 2>/dev/null || true
step "$step_note"
rsync -a "$JR_APP/android-system/" "$HUSK_ROOT/build/android-system/"
[ -s "$HUSK_ROOT/build/java-runtime/com.android.art/javalib/core-oj.jar" ] \
    || { echo "the staged java-runtime has no core-oj.jar; a Java app still cannot start" >&2; exit 1; }
du -sh "$HUSK_ROOT/build/java-runtime" "$HUSK_ROOT/build/android-system"

if [ "$HUSK_BUNDLE_GUEST" = "1" ]; then
    # The only thing this still buys is edk2: the script unpacks the firmware
    # into guests/phase0, but the staged copy the build phase reads comes from
    # the pc-bios step above. Its Alpine kernel/initramfs are no longer staged at
    # all -- no shipped path starts Phase 0 (see QemuRunner.run()).
    step "guest firmware (edk2)"
    ./scripts/fetch_phase0_guest.sh
else
    # Nothing to fetch. The Phase 0 Alpine kernel/initramfs (19.6 MB compressed)
    # were the guest's diagnostic boot, never started on any shipped path, and
    # the edk2 firmware is the VM's; both go when the guest goes.
    step "guest kernel + firmware: skipped (HUSK_BUNDLE_GUEST=0)"
fi

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
    [ -f "$f" ] && printf '%s  %s\n' "$(shasum -a 256 "$f" | cut -d' ' -f1)" "$(basename "$f")"
done
printf '%s  %s\n' "$(shasum -a 256 "$HUSK_ROOT/build/java-runtime/husk-framework.dex" | cut -d' ' -f1)" "java-runtime/husk-framework.dex"
printf '%s  %s\n' "$(find "$HUSK_ROOT/build/java-runtime" "$HUSK_ROOT/build/android-system" -type f | wc -l | tr -d ' ')" "java-runtime + android-system files"
echo "graphics stack (ANGLE, MoltenVK): upstream Husk $TAG (unchanged)"
echo "qemu library (libqemu-aarch64-softmmu.dylib): upstream Husk $TAG (unchanged; also hosts the JIT)"
if [ "$HUSK_BUNDLE_GUEST" = "1" ]; then
    echo "Android guest payload: BUNDLED (HUSK_BUNDLE_GUEST=1)"
else
    echo "Android guest payload: NOT bundled (HUSK_BUNDLE_GUEST=0) -- the Android VM cannot boot in this IPA"
fi
echo "java runtime + Android system: upstream Husk $JAVA_DONOR_TAG (unchanged)"
echo "app binary: built from this tree at $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
