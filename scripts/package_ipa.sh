#!/bin/bash
# Build Husk and package one IPA for every installer, validating the bundle before shipping it.
#
# No signing team is needed: AltStore / SideStore re-sign at install with the
# user's own profile. The IPA is signed ad hoc with Husk.entitlements embedded
# anyway, because TrollStore keeps the entitlements a binary already carries
# (RootHelper's signApp reads them from the binary) and invents only
# get-task-allow for one that has none. Signed this way, the same file gets the
# memory and dynamic-codesigning entitlements under TrollStore, and a sideloader
# simply replaces the signature.
#
# The validation step exists because a bundle missing CFBundleIdentifier or
# CFBundleExecutable builds and zips perfectly happily, and then fails to install
# with no useful message. Xcode does not inject those keys when a custom
# INFOPLIST_FILE is supplied without GENERATE_INFOPLIST_FILE, which is exactly how
# this project is set up.
set -euo pipefail

HUSK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DD="${DD:-/tmp/husk_ipa}"
TROLLSTORE=false
OUT=""
for arg in "$@"; do
    case "$arg" in
        --trollstore|--tipa) TROLLSTORE=true ;;
        *) OUT="${OUT:-$arg}" ;;
    esac
done
[[ "$OUT" == *.tipa ]] && TROLLSTORE=true
if [ -z "$OUT" ]; then
    OUT="$HOME/Desktop/Husk.$([ "$TROLLSTORE" = true ] && echo tipa || echo ipa)"
fi
mkdir -p "$DD"

# Nothing to restage. This used to copy a freshly built QEMU dylib over the staged
# one in build/ios-arm64/lib, because the Xcode target linked it and a plain
# `ninja` in the QEMU tree therefore produced a dylib the IPA never saw -- which
# looked exactly like a fix that did not work. The app no longer links, embeds or
# ships that library at all (the JIT substrate that made it load-bearing is
# compiled into the app: src/ios-jit/husk-ios-jit.c), so there is no staged copy
# to go stale. See .hermes/plans/engine-reset/qemu-dylib-removal.md.

# Regenerate the project first.
#
# project.yml globs src/app/Husk, so adding a source file there is meant to be
# all it takes -- but the checked-in .pbxproj is a build artefact of that glob,
# and nothing was regenerating it. A new file was therefore silently absent from
# the target, and the only symptom was "cannot find X in scope" for a type that
# is plainly right there on disk.
if command -v xcodegen >/dev/null 2>&1; then
    echo "==> regenerating the project from project.yml"
    (cd "$HUSK_ROOT/src/app" && xcodegen generate --quiet)
else
    echo "==> xcodegen not installed; using the checked-in project as-is" >&2
fi

echo "==> building"
xcodebuild -project "$HUSK_ROOT/src/app/Husk.xcodeproj" -scheme Husk \
    -sdk iphoneos -configuration Release -derivedDataPath "$DD" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" \
    build 2>&1 | tee "$DD/build.log" | grep -E "error:|BUILD (SUCCEEDED|FAILED)" || true

# A failed build used to sail straight past this: the previous .app is still in
# DerivedData, so validation and packaging both succeed and produce an IPA of
# the LAST build. Shipping a stale binary silently is the worst outcome here --
# it looks exactly like a fix that did not work.
if ! grep -q "BUILD SUCCEEDED" "$DD/build.log"; then
    echo "build failed; refusing to package a stale app" >&2
    grep -E "error:" "$DD/build.log" | head -10 >&2
    exit 1
fi

APP="$DD/Build/Products/Release-iphoneos/Husk.app"

# The Java runtime in the app must be the one just built (build_java_runtime.sh): copied again here, and checked, so an IPA can
# never carry an older one -- which looks exactly like Java fixes that did not work.
JRT="$HUSK_ROOT/build/java-runtime"
if [ -f "$JRT/husk-framework.dex" ]; then
    rsync -a --delete "$JRT/" "$APP/java-runtime/"
    if ! cmp -s "$JRT/husk-framework.dex" "$APP/java-runtime/husk-framework.dex"; then
        echo "the app's Java framework is not build/java-runtime's; refusing to package" >&2
        exit 1
    fi
    echo "==> java runtime: $(stat -f '%z bytes, %Sm' "$JRT/husk-framework.dex")"
fi
[ -d "$APP" ] || { echo "no app bundle at $APP" >&2; exit 1; }

# Stamp the build's identity into the bundle so its logs can name themselves.
# A log from a stale install is otherwise indistinguishable from a log proving a
# fix did not work.
APP_PLIST="$DD/Build/Products/Release-iphoneos/Husk.app/Info.plist"
if [ -f "$APP_PLIST" ]; then
    COMMIT="$(git -C "$HUSK_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    git -C "$HUSK_ROOT" diff --quiet 2>/dev/null || COMMIT="$COMMIT-dirty"
    plutil -replace HuskBuildCommit -string "$COMMIT" "$APP_PLIST"
    plutil -replace HuskBuildDate -string "$(date -u '+%Y-%m-%d %H:%M UTC')" "$APP_PLIST"
    echo "==> stamped build $COMMIT"

    # The "what this build is for" note is keyed by the commit, and a note for build N cannot be written before N's sha
    # exists -- so a CI build never matched and the app fell back to the newest hand-written entry, showing the SAME stale
    # note on every install. Write this build's own entry here, from the commit itself, so the popup always describes the
    # build that was actually installed.
    NOTE_SHA="${COMMIT%%-dirty}"
    if command -v python3 >/dev/null 2>&1; then
        NOTE_TITLE="$(git -C "$HUSK_ROOT" log -1 --format=%s 2>/dev/null || echo 'untitled build')"
        NOTE_BODY="$(git -C "$HUSK_ROOT" log -1 --format=%b 2>/dev/null || true)"
        NOTE_TITLE="$NOTE_TITLE" NOTE_BODY="$NOTE_BODY" NOTE_SHA="$NOTE_SHA" \
        python3 - "$APP" <<'PY'
import json, os, sys
from datetime import date

app = sys.argv[1]
sha = os.environ["NOTE_SHA"]
title = os.environ["NOTE_TITLE"].strip() or "untitled build"
body = os.environ["NOTE_BODY"].strip()

lines = [l.strip(" \t-*") for l in body.splitlines()]
lines = [l for l in lines if l]
goal = lines[0] if lines else title
details = lines[1:] if len(lines) > 1 else []
entry = {"sha": sha, "date": date.today().isoformat(),
         "title": title, "goal": goal, "details": details[:24]}

targets = []
for root, _dirs, files in os.walk(app):
    if "BuildNotes.json" in files:
        targets.append(os.path.join(root, "BuildNotes.json"))
if not targets:
    targets = [os.path.join(app, "BuildNotes.json")]

for path in targets:
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        notes = [n for n in doc.get("notes", []) if n.get("sha") != sha]
    except Exception:
        notes = []
    notes.insert(0, entry)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump({"notes": notes}, fh, indent=2, ensure_ascii=False)
    print(f"==> note for {sha} written to {path}")
PY
    fi
fi

echo "==> validating bundle"
PLIST="$APP/Info.plist"
rc=0
for key in CFBundleIdentifier CFBundleExecutable CFBundleName \
           CFBundlePackageType CFBundleVersion CFBundleShortVersionString \
           MinimumOSVersion UIDeviceFamily; do
    val="$(/usr/libexec/PlistBuddy -c "Print :$key" "$PLIST" 2>/dev/null || true)"
    if [ -z "$val" ]; then
        echo "  MISSING  $key   <-- the app will not install" >&2
        rc=1
    else
        printf "  ok       %-28s %s\n" "$key" "$(echo "$val" | head -1)"
    fi
done

# The executable named in the plist must actually exist.
EXE="$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$PLIST" 2>/dev/null || true)"
if [ -n "$EXE" ] && [ ! -f "$APP/$EXE" ]; then
    echo "  MISSING  executable '$EXE' named by CFBundleExecutable" >&2
    rc=1
elif [ -n "$EXE" ]; then
    printf "  ok       %-28s %s\n" "executable present" "$EXE"
fi

# ANGLE must be embedded, or the app dies at launch with a dyld error.
#
# ANGLE is checked here because it was not, and an IPA shipped without it: the
# app reached qemu_egl_init_dpy_cocoa and aborted with "Couldn't open
# @rpath/libANGLE-shared.dylib". Everything else in this validation passed. A
# check that covers one of two required libraries is a check that reports
# success on a bundle that cannot launch.
for lib in libANGLE-shared.dylib libhuskps4.dylib; do
    if [ ! -f "$APP/Frameworks/$lib" ]; then
        echo "  MISSING  Frameworks/$lib" >&2
        rc=1
    else
        printf "  ok       %-28s %s\n" "${lib%%-*} dylib embedded" \
            "$(du -h "$APP/Frameworks/$lib" | cut -f1)"
    fi
done

# And QEMU must NOT be embedded: it is no longer linked, no longer a dependency,
# and no longer shipped. Its JIT substrate lives in the app binary now
# (src/ios-jit/husk-ios-jit.c) and the engine's own symbols are forwarded by
# QemuEngineShim.c. A copy appearing here means the dependency came back -- worth
# failing over, because 6.7 MB of IPA is exactly what this change removed.
if [ -f "$APP/Frameworks/libqemu-aarch64-softmmu.dylib" ]; then
    echo "  UNEXPECTED  Frameworks/libqemu-aarch64-softmmu.dylib is embedded; the engine is supposed to be gone" >&2
    rc=1
else
    printf "  ok       %-28s %s\n" "qemu engine not embedded" "6.7 MB packed saved"
fi

# Guest images, firmware, and blank disk seeds required by first launch -- only
# when this build bundles the guest (HUSK_BUNDLE_GUEST=1). When it does not, their
# absence is expected and must NOT fail packaging: the build ships without the VM
# on purpose (see .hermes/plans/engine-reset/guest-unbundled.md).
for f in husk-jit.js; do
    if [ ! -f "$APP/$f" ]; then
        echo "  MISSING  $f" >&2
        rc=1
    else
        printf "  ok       %-28s %s\n" "$f" "$(du -h "$APP/$f" | cut -f1)"
    fi
done
if [ "${HUSK_BUNDLE_GUEST:-0}" = "1" ]; then
    for f in vmlinuz-virt initramfs-virt \
             edk2-aarch64-code.fd lineage-efi-vars-seed.fd lineage-vdb-seed.qcow2; do
        if [ ! -f "$APP/$f" ]; then
            echo "  MISSING  $f" >&2
            rc=1
        else
            printf "  ok       %-28s %s\n" "$f" "$(du -h "$APP/$f" | cut -f1)"
        fi
    done
else
    echo "  n/a      Android guest payload not bundled (HUSK_BUNDLE_GUEST=0)"
fi

# Built-in StikJIT's helper (without it the app still installs and runs with
# StikDebug, but its JIT setup would offer a method that can only fail), and the
# widget that draws the download Live Activity.
for f in "$APP/PlugIns/HuskJITHelper.appex/HuskJITHelper" \
         "$APP/PlugIns/HuskDownloadsWidget.appex/HuskDownloadsWidget" \
         "$APP/Frameworks/StikJIT.framework/StikJIT" \
         "$APP/Frameworks/StikJIT.framework/Info.plist"; do
    if [ ! -f "$f" ]; then
        echo "  MISSING  ${f#$APP/}" >&2
        rc=1
    else
        printf "  ok       %-28s %s\n" "$(basename "$f")" "${f#$APP/}"
    fi
done

[ $rc -eq 0 ] || { echo "==> bundle is not installable; refusing to package" >&2; exit 1; }

echo "==> packaging"
# staged next to the build (DD), not in the system temp folder: a full startup disk made codesign fail with "internal error"
STAGE="$(mktemp -d "$DD/stage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/Payload"
cp -R "$APP" "$STAGE/Payload/"

# Ad hoc, inside out: loose dylibs and frameworks, then app extensions, then the
# app with its entitlements (see the top of this file for why).
ENT_BASE="$HUSK_ROOT/src/app/Husk/Husk.entitlements"
ENT="$STAGE/entitlements.plist"
cp "$ENT_BASE" "$ENT"
BANNED_KEYS=(
    "com.apple.private.cs.debugger"
    "dynamic-codesigning"
    "com.apple.private.skip-library-validation"
)
if [ "$TROLLSTORE" = true ]; then
    for key in "${BANNED_KEYS[@]}"; do
        plutil -remove "$key" "$ENT" 2>/dev/null || true
    done
fi
SAPP="$STAGE/Payload/$(basename "$APP")"
sign() { codesign --force --sign - --timestamp=none "$@"; }
find "$SAPP" -name "*.dylib" -not -path "*/Frameworks/*.framework/*" | while read -r f; do sign "$f"; done
for fw in "$SAPP"/Frameworks/*.framework; do [ -d "$fw" ] && sign "$fw"; done
for ex in "$SAPP"/PlugIns/*.appex; do [ -d "$ex" ] && sign "$ex"; done
sign --entitlements "$ENT" "$SAPP"
echo "==> entitlements in the signed app:"
EMBEDDED_ENTS=$(codesign -d --entitlements - "$SAPP" 2>&1 || true)
REQUIRED_PATTERN="get-task-allow|increased-memory|extended-virtual"
[ "$TROLLSTORE" != true ] && REQUIRED_PATTERN+="|dynamic-codesigning"
if ! grep -qE "$REQUIRED_PATTERN" <<< "$EMBEDDED_ENTS"; then
    echo "==> entitlements did not embed; refusing to package" >&2
    rm -rf "$STAGE"
    exit 1
fi
if [ "$TROLLSTORE" = true ]; then
    BANNED_PATTERN=$(IFS='|'; echo "${BANNED_KEYS[*]}")
    if matched=$(grep -oE "$BANNED_PATTERN" <<< "$EMBEDDED_ENTS" | head -n 1) && [ -n "$matched" ]; then
        echo "==> banned entitlement '$matched' found in trollstore package; refusing to package" >&2
        rm -rf "$STAGE"
        exit 1
    fi
fi
TMP_OUT="$STAGE/package.tmp"
( cd "$STAGE" && zip -qry "$TMP_OUT" Payload )

# Atomic replace so a half-written IPA never sits where the good one was.
mv -f "$TMP_OUT" "$OUT"
rm -rf "$STAGE"
echo "==> $OUT  ($(du -h "$OUT" | cut -f1))"
