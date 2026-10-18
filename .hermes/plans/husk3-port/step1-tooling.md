# Step 1 — tooling and CI ported into husk3.0

SOURCE = `C:/Users/Aiden/Desktop/HuskApp` (our fork, git repo) →
TARGET = `C:/Users/Aiden/Desktop/husk3.0` (upstream, **not a git repo — no git run inside it**).

Method: every candidate was classified SRC-ONLY / BOTH-SAME / BOTH-DIFF by sha256; each BOTH-DIFF was
`diff`ed before any overwrite; each ported script's own path literals were read and retargeted. Nothing
under `src/` was touched. Nothing under `tools/a32/**` was touched. `tools/serve-folder.py` was not touched
(see Skipped).

## Ported verbatim (byte-identical to source — `cmp` confirmed)

| file | note |
|---|---|
| `.github/workflows/build-ios.yml` | slow IPA path; calls `scripts/ci_build.sh` (already identical in both) |
| `.github/workflows/build-ipa-prebuilt.yml` | fast IPA path; calls `scripts/build_ipa_prebuilt.sh` + `tools/check-shipped-dex.py` |
| `scripts/build_ipa_prebuilt.sh` | all paths derive from `HUSK_ROOT="$(dirname "$0")/.."` → no change needed |
| `tools/stage-build.py` | paths derive from `__file__` (`<root>/HuskBuilds`) → no change needed |
| `tools/watch-husk-logs.py` | default log dir is `<Desktop>/HuskLogs` (beside, not inside, the repo) → tree-independent |
| `tools/check-shipped-dex.py` | takes the IPA as an argument → no paths |
| `tools/husk_log_receiver.py` | default `~/Desktop/HuskLogs` → tree-independent |
| `tools/apk-precheck.py` | takes APK paths as arguments; only a docstring reference to ours' `.hermes/plans/engine-reset/*.md` (see below) |
| `tools/husk-notes.py` | resolves `<repo>/../HuskLogs` relative to `__file__` → no change needed |
| `tools/precheck/README.md` | documentation for `apk-precheck.py`; contains no tree paths |

## Ported with edits (paths/guards retargeted for husk3.0)

| file | what changed | why |
|---|---|---|
| `.github/workflows/check-translation-layer.yml` | the `src/app/Husk/QemuEngineShim.c` compile is now guarded by `if [ ! -f … ]` → skipped when absent | that file belongs to our engine-reset and **does not exist in the target**; the step was unconditional and would have failed the gate on a file that has not been ported. Drop the guard once the engine-reset lands. The `paths:` filters already list the file harmlessly. |
| `tools/ls_send.py` | `CERT_DIR` now `os.path.dirname(dirname(__file__))/.local/lscert` instead of the hard-coded `…/HuskApp/.local/lscert`; `%TEMP%` fallback kept | so the one copy works from either tree. The client cert LocalSend demands was copied to the target so the handshake still succeeds: `husk3.0/.local/lscert/{cert.pem,key.pem}` (data, not tooling; `.local/` is gitignored in our fork — **the target's `.gitignore` has no `.local/` entry**) |
| `tools/renpy_unpack_test.py` | `APK` now `<repo>/TheLewdestHouse-0-2-42.apk`, overridable with `HUSK_RENPY_APK`, instead of the hard-coded HuskApp path | the fixture APK is **not** in husk3.0 — put it there (or set the env var) before use |
| `tools/a32_download_verify.py` | `OUT` now `<repo>/Games32` instead of `…/HuskApp/Games32` | retarget (the tool itself is named in scope; only `tools/a32/**` is on hold). `Games32/` does not exist in the target yet |
| `docs/build-without-a-mac.md` | 7× `C:/Users/Aiden/Desktop/HuskApp` → `…/husk3.0`, plus a port note at the top | paths. The note flags that its sections 1–3 (`gh repo create husk-build`, `git init`, committing a fresh tree) describe standing up the **fork's** repo and do not apply to upstream; start at §4 |

## Merged (existed in both, differed — diff read first)

**`scripts/package_ipa.sh`** (target 9811 B, ours 10864 B). Base = our copy; **one thing preserved from the
target**: re-added `libhuskps4.dylib` to the required-embedded-libraries loop (`for lib in
libANGLE-shared.dylib libhuskps4.dylib`). The target has PS4 support (`scripts/build_ps4_ios.sh`, which our
fork lacks), so our loop would have silently dropped the check that the PS4 dylib is embedded.

**What our copy drops from the target's — recorded, not silent:**

1. the QEMU **restaging** block (`BUILT` vs `STAGED` `libqemu-aarch64-softmmu.dylib` freshness copy) — ours
   removed it because the app no longer links that library;
2. `libqemu-aarch64-softmmu.dylib` from the required-embedded-libraries loop;
3. the **unconditional** guest-payload checks (`vmlinuz-virt`, `initramfs-virt`, `edk2-aarch64-code.fd`,
   `lineage-efi-vars-seed.fd`, `lineage-vdb-seed.qcow2`) — ours gates them behind `HUSK_BUNDLE_GUEST=1`;
4. ours **adds** a fatal `UNEXPECTED … qemu … embedded` guard.

All four encode our **engine-reset**, which the target has not done: husk3.0 still links/ships QEMU
(`src/app/Husk/QemuRunner.swift`, `HuskBridge.h`, `src/app/project.yml` all reference `libqemu`). Consequence
stated plainly: **`package_ipa.sh` as ported will fail packaging on husk3.0 until the engine-reset app-side
changes (plan steps 4–5) land**, because (4) is fatal and the target still embeds QEMU. That coupling is
deliberate here (the plan ports `scripts/` wholesale and `check-translation-layer.yml` is the gate that works
today), but it is the parent's call whether to sequence it before a target IPA build.

## Skipped (with reason)

| file | why |
|---|---|
| `tools/serve-folder.py` | **owned by another worker — untouched.** Verified after finishing: its sha256 is unchanged by me; the target's copy (mtime 22:10, i.e. the other worker's) currently equals ours. I never opened it for write. |
| `tools/a32/**` (24 files) + `.github/workflows/arm32-harness.yml` | 32-bit work deliberately on hold, per the task |
| `.github/workflows/probe-angle.yml` | not in the named list, and self-described as temporary ("workflow_dispatch only; delete once the ANGLE build is decided") |
| `scripts/integrate_husk.sh` | exists in **both** and differs by exactly one line: the target's symbol list keeps `husk_ios_jit_reserve_bottom`, ours removed it. Not named in scope, and the change is coupled to the step-3 layer port (the list prunes/adds symbols in `qemu.symbols` to match what the layer exports). Recommend revisiting it with the `src/ios-jit` port rather than now. |
| other SRC-ONLY root tools not in the scope list | `tools/{plan_husk3_port.py, fetch_rfl_apk.sh, probe_mtls.py, fix_region_gate_proto.py, fix_region_gate_proto2.py, fix_region_size.py, a32_harvest.py, a32_libdump.py, a32_cands1.json}`. Left in place — none is in the named port set; say the word and they come too. |
| target-only files | `tools/{jit-arena-test.c, ps4-test.m, playdl/login.swift, compat/classify.py}` are upstream's; not touched. |

## What must change in the target's repo for the CI to be usable

- **No repo secrets are required** by any ported workflow: the IPA is ad-hoc signed and SideStore/AltStore
  re-sign at install (stated in `docs/build-without-a-mac.md` and the workflow headers). `build-ipa-prebuilt.yml`
  needs only the default `GITHUB_TOKEN` with `permissions: contents: write` (it publishes the IPA as a release
  asset) — already declared in the file.
- **Triggers/tag scheme (reported, not changed):** `build-ios.yml` runs on `workflow_dispatch` + `push: tags: v*`;
  `build-ipa-prebuilt.yml` is `workflow_dispatch` only and **publishes releases tagged `night-<short-sha>`**
  (`gh release create "night-$TAG"`). Both workflows download donor IPAs from
  `github.com/Leviidev/Husk/releases/download/<tag>` (`1.1.0` for the graphics stack, `2.0` for the Java
  runtime) via `scripts/build_ipa_prebuilt.sh`. Those upstream release tags must exist for the prebuilt path to
  work — left exactly as authored.
- **Runners:** `build-ios.yml`/`build-ipa-prebuilt.yml` pin `macos-26` (needs the Xcode 16 `objectVersion 77`
  project format and the vendored Swift 6.4 `.swiftinterface`); `check-translation-layer.yml` uses `macos-14`.
- **`build_ipa_prebuilt.sh` and the merged `package_ipa.sh` assume the engine-reset** (`QemuEngineShim.c`,
  `project.yml` not linking `libqemu`, `project.yml` honoring `HUSK_BUNDLE_GUEST`). Until steps 4–5 land, the
  **fast gate `check-translation-layer.yml` is the CI that runs clean; the IPA paths will not.**
- **`apk-precheck.py`'s docstring** cites our `.hermes/plans/engine-reset/*.md`, which does not exist in the
  target — cosmetic only (documentation text, not a path the code reads).

## What could NOT be verified

- **Nothing executed the CI.** No macOS runner and no Mac here, so no workflow step, no `build_ipa_prebuilt.sh`
  run and no `package_ipa.sh` run was performed. The build-path claims above are reasoned from reading the
  scripts and the target's `project.yml`/`QemuRunner.swift`, not from a build.
- Verified only statically: `python -m py_compile` (9/9 OK), `bash -n` on both shell scripts (OK), and
  YAML parse of the three workflows (OK); `cmp` byte-identity for the ten verbatim ports; sha256 for
  `serve-folder.py`.
- `scripts/package_ipa.sh`'s target behavior cannot be confirmed without a build — in particular the fatal
  QEMU-absent guard is expected to trip on the current target.
- `docs/build-without-a-mac.md`, `tools/precheck/README.md` and `apk-precheck.py` were not executed (docs).
- The un-named SRC-ONLY tools listed under Skipped may contain further tree-specific assumptions; they were not
  audited because they are out of the named scope.
