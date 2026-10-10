# Building Husk without a Mac

Husk's app half is Swift + UIKit and links QEMU, ANGLE and MoltenVK; it is built by
`scripts/ci_build.sh`, which opens with `set -euo pipefail` and the comment *"Runs on
macOS with Xcode."* Nothing on Windows — not MSVC, not clang, not zig — can produce a
runnable iOS binary, because there is no iOS SDK and no `codesign`. What a Windows box
*can* do is say whether the C compiles, and hand the whole build to a Mac in the cloud.

There are three things you can do, cheapest first:

| | what | where | how long |
|---|---|---|---|
| A | compile-check the translation layer | this machine (zig) | seconds |
| B | compile-check against the real iPhoneOS SDK | GitHub Actions, macOS | ~1 min |
| C | build the IPA | GitHub Actions, macOS | ~2 h first time |

The rest of this file is C, end to end, then how to put the result on a phone.

---

## What is already in the repo

- `.github/workflows/check-translation-layer.yml` — job **check-translation-layer**
  (path B). Compiles every `src/translation-layer-next/*.c` against the real iPhoneOS
  SDK. Runs on push and pull request. No setup.
- `.github/workflows/build-ios.yml` — job **build-ios** (path C). Runs
  `scripts/ci_build.sh` on a `macos-14` runner and uploads `Husk.ipa` + `Husk.tipa`.
  It does **not** run on a push — only manually or on a `v*` tag — so a push never
  costs two hours by surprise.
- `.gitignore` / `.gitattributes` — the 750 MB test APK and source zips are ignored
  (GitHub rejects any file over 100 MB), and line endings are pinned to LF so the
  shell scripts survive a Windows checkout.

---

## Path C — build the IPA (GitHub Actions)

### 0. What you need

- A GitHub account, and a **public** repo. Actions macOS minutes are billed at **10×**
  for private repos — one cold build (~120 min) burns ~1200 of the 2000 free
  minutes a month. Public repos are free. Husk is GPL-2.0, so public is also correct.

### 1. Make sure the big files are not staged

The repo copy on the Desktop has a 750 MB APK and a source zip next to the code.
`.gitignore` already excludes them; confirm before committing:

```sh
cd "C:/Users/Aiden/Desktop/HuskApp"
git check-ignore -v TheLewdestHouse-0-2-42.apk Husk-main.zip   # should print both, with .gitignore line numbers
```

### 2. Commit the tree

```sh
cd "C:/Users/Aiden/Desktop/HuskApp"
git config user.name  "Your Name"
git config user.email "you@example.com"
git add -A
git commit -m "Husk: Ren'Py support in the translation layer + CI"
```

`git init -b main` has already been run; 357 files stage, nothing above ~3 MB.

### 3. Install and authenticate `gh`, then create the repo

`gh` is not installed on this machine:

```sh
winget install --id GitHub.cli -e --accept-source-agreements --accept-package-agreements
# open a NEW shell so PATH updates, then:
gh auth login          # GitHub.com → HTTPS → "Login with a web browser",
                       # paste the one-time code into the page it opens
```

Then create the remote and push in one step:

```sh
cd "C:/Users/Aiden/Desktop/HuskApp"
gh repo create husk-build --public --source . --remote origin --push
```

(No `gh`? Create an empty repo in the GitHub web UI, then
`git remote add origin https://github.com/<you>/husk-build.git && git push -u origin main`.
The push will ask for credentials — a Personal Access Token with `repo` scope works as
the password.)

### 4. Run the fast check first (path B)

A push already started it. Watch it and confirm the C compiles against the real SDK:

```sh
gh run list --workflow check-translation-layer
gh run watch $(gh run list --workflow check-translation-layer --limit 1 --json databaseId -q '.[0].databaseId')
```

Green here means the translation layer is sound. Red shows the exact file and line.

### 5. Run the full build

Either the UI: **Actions → build-ios → Run workflow → Run**.
Or the CLI:

```sh
gh workflow run build-ios
gh run watch $(gh run list --workflow build-ios --limit 1 --json databaseId -q '.[0].databaseId')
```

The first run takes a couple of hours (it fetches and cross-compiles QEMU, ANGLE and
MoltenVK). Later runs reuse the Actions cache and only rebuild what changed.

### 6. Download the IPA

```sh
gh run download $(gh run list --workflow build-ios --limit 1 --json databaseId -q '.[0].databaseId') -n Husk-<sha>
```

or from the run page, **Artifacts → Husk-&lt;sha&gt;**. You get `Husk.ipa` and `Husk.tipa`.

### 7. Put it on the iPhone

- **SideStore / AltStore**: open the `.ipa` in the app (Files → Share → SideStore), or
  add it as a local source. It re-signs with your own profile and installs.
- **TrollStore** (supported iOS versions): use the `.tipa` — it keeps Husk's embedded
  entitlements.
- The IPA is signed **ad hoc, not for a team**: no Apple Developer account is involved
  and no secrets are needed in CI. Installers replace the signature.

### 8. Verify the Ren'Py fix

Add `TheLewdestHouse-0-2-42.apk` to Husk's translation layer and launch it. In the log
(Settings → the game's page → log, or `school_esp`-style logfile) you want to see, in
order, instead of the crash:

```
renpy: unpacked private data: N files into /…/files
renpy: environment set (ANDROID_PRIVATE=/…/files, ANDROID_APK=/…/base.apk)
sdl: SDL_main starting
```

and **no** `=== FATAL signal 11 on thread 'SDLThread'`. If a new `SIGSEGV` appears, the
crash report now names every frame (`libsystem_platform strcmp+…`, not `???`) and the
first call of any unresolved import aborts with its name — read those two lines first.

---

## Path A — compile-check the C here, on Windows

`clang` is installed (`C:\Program Files\LLVM`) but has no libc headers without the MSVC
environment, so use **zig**, whose bundle includes real Darwin headers:

```sh
ZIG="$LOCALAPPDATA/Temp/zigdl/zig-windows-x86_64-0.13.0/zig.exe"   # already downloaded
cd "C:/Users/Aiden/Desktop/HuskApp/src/translation-layer-next"
for f in *.c; do
  "$ZIG" cc -target aarch64-macos -D_DARWIN_C_SOURCE \
    -IC:/Users/Aiden/Desktop/HuskApp/src/translation-layer-next \
    -IC:/Users/Aiden/Desktop/HuskApp/src/translation-layer \
    -c "C:/Users/Aiden/Desktop/HuskApp/src/translation-layer-next/$f" -o /tmp/o.o || echo "FAIL $f"
done
```

Expected: **36 of 42** pass. The six that fail want framework headers zig does not ship
(`AudioToolbox`, `CoreGraphics`, `Security`, `resolv.h`, `mach/mach_vm.h`,
`if_nametoindex`) in files unrelated to the Ren'Py change — they are fine in the real
SDK, which is exactly what path B checks. Notes: zig's `-fsyntax-only` is broken (use
`-c -o`), and `zig.exe` is a native binary, so pass `C:/...` paths, not MSYS `/c/...`.

---

## Troubleshooting

- **`remote: error: File … is 750 MB; this exceeds GitHub's file size limit of 100 MB.`**
  The APK got staged — it should be ignored. `git rm --cached TheLewdestHouse-*.apk`
  (and any `*.zip`), re-commit.
- **Build job fails early with "No space left on device."** The Xcode image ships
  ~14 GB free; QEMU + ANGLE + MoltenVK + sources need more. The workflow already
  deletes dotnet and Homebrew's download cache; if it still runs out, use a larger
  runner or split the cache.
- **Actions shows no macOS minutes / build never starts.** Check Settings → Actions →
  General → Workflow permissions, and that the repo is public (or that you have
  minutes left).
- **The app launches but the game still crashes.** The IPA may be stale: `package_ipa.sh`
  refuses to ship a stale `.app`, but a sideloader can still install an older copy.
  Confirm the log's `HuskBuildCommit` (stamped into `Info.plist`) matches your commit.
- **Scripts fail with `\r` errors when you run them on a Mac.** The tree was committed
  with CRLF. `.gitattributes` pins LF; re-clone rather than copying the working tree.
