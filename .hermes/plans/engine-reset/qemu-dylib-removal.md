# Getting the QEMU dylib out of the bundle without losing the JIT

Read-only inspection on Windows plus source/build-script edits. **No Mac and no
iOS toolchain here, so nothing was compiled** — the enumeration, the three
JIT-alive proofs and the size arithmetic below are read from the code, and what a
real build must confirm is listed at the end. The commit is pushed; CI compiles.

Measured against the IPA he is running: `Payload/Husk.app/Frameworks/`
`libqemu-aarch64-softmmu.dylib` is **33.9 MiB raw, 6.7 MiB packed** — the largest
single non-guest entry left in an 84.9 MiB IPA. The guest firmware/kernel/ROMs
went in the earlier unbundle (`guest-unbundled.md`); this is the last of QEMU.

---

## 1. Enumeration — everything that resolved inside that dylib

### 1a. The JIT substrate (the reason it survived the unbundle)

Defined in `src/ios-jit/husk-ios-jit.c` + `husk-brk.S`, copied into QEMU's `tcg/`
by `scripts/integrate_husk.sh:11-13`, wired into `tcg/meson.build` (`:54-67`, so
`husk-ios-jit.c` and `husk-brk.S` compile into the shared library), and exported
from it by `system/qemu.symbols` (`integrate_husk.sh:619-627`).

| symbol | who resolves it, how |
|---|---|
| `husk_ios_jit_install_trap_handler` | the app, direct call — `JITBootstrap.swift:38`, declared `HuskBridge.h:56` |
| `husk_ios_jit_prewarm` | the app, direct call — `JITBootstrap.swift:149`; **and** the native translation layer by `dlsym(RTLD_DEFAULT, "husk_ios_jit_prewarm")` — `src/translation-layer/husk-tl-load.c:529,534` |
| `husk_ios_jit_get_mapping` | the native translation layer by `dlsym(RTLD_DEFAULT, "husk_ios_jit_get_mapping")` — `husk-tl-load.c:522` (this is `tl_find_stikdebug_prewarmed()`, called from `husk-tl-xmem.c:479` when the arena opens) |
| `husk_ios_jit_region_size` | the app, direct call — `JITBootstrap.swift:100`, declared `HuskBridge.h:61` |
| `husk_ios_jit_is_available` | the app — `JITBootstrap.swift:522` |
| `husk_ios_jit_mapjit_works` | the app — `JITBootstrap.swift:508` |
| `husk_ios_jit_self_route` | the app — `SettingsTab.swift:517,520` |
| `husk_ios_jit_detach` | the app — `JITBootstrap.swift:475` |
| `husk_ios_jit_log_footprint` | the app — `HuskLog.swift:392` |
| `husk_ios_available_memory` | the app — `QemuRunner.swift:888,1620`, `QemuRunner.swift` budget lines |
| `husk_ios_jit_allocate` | the engine itself, inside its own `qemu_init` (`tcg/region.c`, patched by `patches/husk-qemu-ios-jit.patch` + `integrate_husk.sh`, takes the prewarmed region) |
| `husk_ios_jit_exec_gate` | **not the dylib's.** The app's is `src/translation-layer-next/husk-tl-xmem.c:1463`, compiled into the app binary; the dylib's copy was local (not in `qemu.symbols`). Called from `JITBootstrap.swift:219`, `JITSetup.swift:367` |

Also inside the dylib and *not* in `qemu.symbols`: `husk_ios_jit_exec_gate`,
`husk_ios_jit_region_size`, `husk_ios_jit_allocate` — i.e. the arena-size commit
(77d0d53) added an app call to a symbol the engine's export list never carried.
Compiling the substrate into the app removes that whole class of "links fine from
a branch that was never built" problem, which is the accident the task names.

### 1b. The engine itself and the bridge compiled beside it

| symbol | who resolves it, how |
|---|---|
| `qemu_init` / `qemu_main_loop` / `qemu_cleanup` | the app, direct call — `QemuRunner.swift:1358,1478,1482`; declared `HuskBridge.h:29-31`, exported via `system/qemu.symbols` |
| `husk_display_init`, `husk_display_lock_frame`, `husk_display_unlock_frame`, `husk_display_sequence`, `husk_display_send_pointer`, `husk_display_send_key`, `husk_display_set_ui_size`, `husk_display_gl_early`, `husk_display_gl_create`, `husk_display_gl_probe`, `husk_display_gl_bind`, `husk_display_gl_frames`, `husk_display_gl_set_metal_presenter` | the app, direct call — `QemuRunner.swift:1260,1269,1339,1386,1462,1468,1509`; `HuskGLView.swift:235`; `HuskKeyboard.swift:48-244`; `HuskMetalView.swift:80,106,114,139`; `HuskMetalPresenter.swift:223` |
| `husk_audio_pull` / `husk_audio_frames_in` / `husk_audio_underruns` | the app — `HuskAudio.swift:71,102`, `QemuRunner.swift:1518-1519` |
| `husk_snapshot_load_at_startup` / `husk_snapshot_save` | the app — `QemuRunner.swift:1429,556` |
| `husk_balloon_set_bytes` | the app — `QemuRunner.swift:1631` |

All of those are in `src/ios-jit` (`husk-display*.c`, `husk-audio.c`,
`husk-snapshot.c`, `husk-balloon.c`) and are QEMU-side: they include QEMU's own
headers and are wired into QEMU's meson build by `integrate_husk.sh`. They are not
part of the JIT and nothing outside the engine needs them.

### 1c. Who DECIDES to load it

Nobody decided — and that is the point. The dylib was an **unconditional
`LC_LOAD_DYLIB`**: `project.yml` listed it as a `framework:` dependency with
`embed: true`, so dyld required `@rpath/libqemu-aarch64-softmmu.dylib` to exist or
the app would not launch. There was no engine-selection path, no "is it there",
and no code that could run without it. `GuestRuntime.bundled` decides whether the
*guest payload* is present, and it was correct that it could not decide this one:
its own comment said "the QEMU library itself absent — this cannot happen".

### 1d. What the UI/log showed if it were simply absent

Before this change: **nothing**, because the app would not launch — dyld aborts
with `Library not loaded: @rpath/libqemu-aarch64-softmmu.dylib`. That is why the
removal needed the split first, not a deletion.

After this change (see §3): the log carries
`[husk-engine] no QEMU engine in this app bundle: …`, `qemu`/`ui` name it in the
launch log, and the same sentence the guest-unbundled alert uses is raised in
`ContentView` — `QemuEngine.absentMessage`.

---

## 2. Where the JIT bootstrap lives now

**`src/ios-jit/husk-ios-jit.c` + `husk-brk.S`, compiled into the APP binary** (the
Husk target's sources in `src/app/project.yml`).

This is the deliberate version of the accident the task names: `src/ios-jit` was
outside the app target's glob, which is exactly why a call into it produced an
undefined symbol. Now two of its files are in the target and the rest stay out,
for a reason that is written down there — `husk-display*.c`, `husk-audio.c`,
`husk-snapshot.c`, `husk-balloon.c` need QEMU's headers and are the engine's.

It is a few tens of KB in the app binary, against 6.7 MiB packed that leaves the
IPA. No new dylib, no embedding, no code-signing step, no new build stage: the
symbols the app calls directly are simply *in* the app, and the translation
layer's `dlsym(RTLD_DEFAULT, …)` finds them because the main executable is in the
default lookup scope. Nothing links it by name any more.

Three things had to be sorted deliberately rather than by accident, because the
file now shares an image with the app's other C:

1. **`pipe2`** — `src/app/Husk/PipeShim.c` already defines it (strongly, with the
   fishhook constructor that rebinds every loaded image). A second strong
   definition in the same image is a duplicate-symbol link error, so the copy in
   `husk-ios-jit.c` is now `__attribute__((weak))`: the shim wins in the app, and
   the copy is still there for the engine's own build, where nothing else defines
   it.
2. **`husk_ios_jit_exec_gate`** — the app-facing definition is the layer's
   (`husk-tl-xmem.c:1463`, against the arena the guest runs from). The dylib's copy
   was local, so there was no clash while it lived in the dylib; in one image there
   would be. It is now a `static` **region gate** (`husk_ios_jit_region_gate`) that
   prewarm and the detach pair call directly, plus a **weak** public wrapper for
   the engine's build. The app's strong definition wins, exactly as it did.
3. **QEMU's `tcg/tcg-apple-jit.h`** — the file's only QEMU-tree include (the APRR
   write-protect toggle used by the MAP_JIT probe). It is now behind
   `__has_include`: present in the engine's build (unchanged), absent in the app,
   where the same two-call API is provided over the libsystem symbols
   (`pthread_jit_write_protect[_supported]_np`) — the same mechanism
   `husk-tl-load.c:490-497` already uses for the same reason.

---

## 3. Who loads what, now

* **The app** carries the JIT substrate. `JITBootstrap.prewarm()` → the app's
  `husk_ios_jit_prewarm` → the app's allocator. The arena does not need the engine
  and never did.
* **The native translation layer** finds the same functions by name at run time
  (`husk-tl-load.c`), which now resolves to the app's own copy.
* **The engine** is looked up once, lazily, by `src/app/Husk/QemuEngineShim.c`,
  the first time any engine entry point is called: `dlopen()` of
  `@rpath/libqemu-aarch64-softmmu.dylib` and two other places a built one is ever
  put. The shipped build has none, so it is not found and every forwarder takes
  the honest branch. A build that wants the guest puts the library back beside the
  app and it works again.
  * The lookup goes through the **dlopen handle, never `RTLD_DEFAULT`** — the shim
    defines these very names in the app binary, so `RTLD_DEFAULT` would find the
    forwarder and recurse forever.
* **The app's engine symbols are defined by `QemuEngineShim.c`**: `qemu_*` and the
  display/audio/snapshot/balloon bridge are forwarders — a call into the engine
  when it is there, a named no-op when it is not (the audio pull writes the
  silence the engine's own shortfall path would; the snapshot callback always
  fires, with `false`). `husk_qemu_engine_present()` is the question.
* **Who decides:** `QemuEngine.present` — asked in `QemuRunner.run()` and
  `ContentView.start()` *before* a guest is offered, so "the payload is here but
  the engine that runs it is not" is read as that sentence rather than discovered
  as a `qemu_init()` that returns immediately. `GuestRuntime` still decides the
  payload half, unchanged.

---

## 4. The three JIT-alive proofs, from the code

None of them touches the engine, and none of the code that produces them changed.
What changed is *where the file is compiled*, not a line of what it does.

### 4a. Boot selftest — PASS, 42

`husk_jit_selftest()` (`src/ios-jit/husk-ios-jit.c:239`) writes `movz w0,#42; ret`
through the **writable alias**, flushes, reads the bytes back **through the
executable alias**, and then **calls them**:

```
selftest: PASS -- executed generated code from the RX alias, got 42. JIT is genuinely live.
```

It is reached from `husk_ios_jit_allocate_real()` (`:633`), which is reached from
`husk_ios_jit_prewarm()` — called by `JITBootstrap.prewarm()`
(`JITBootstrap.swift:149`), which `ContentView.evaluate()` calls at every launch
(`ContentView.swift:293`), before the guest is even considered. So the line runs
in a build with no engine and no guest. The `!iOS` stub branch (`:869+`) is not
taken: the app is iOS.

### 4b. 512 MiB MAPPED, 512 MiB executable

The arena line is the native layer's, not QEMU's:
`xmem: arena %p..%p: %zu MiB, %zu MiB MAPPED, %zu MiB executable, guest-facing
view %p is %s, …` — `src/translation-layer-next/husk-tl-xmem.c:962`, on the region
`tl_find_stikdebug_prewarmed()` hands over (`husk-tl-load.c:520` →
`husk-xmem.c:479,515`). That region is the one this app's `husk_ios_jit_prewarm`
claimed: the same two views, `rw` and `rx`.

The size is the setting's, untouched: `JITBootstrap.defaultArenaMiB = 512`
(`JITBootstrap.swift:63`), `jitBytes` read from `arenaMiB` (`:96`), range 64–2048
(`:78-79`), clamped with a named log line (`:138-144`) — 77d0d53 as landed. The
layer's floor/ceiling reasoning (`grant_self` halving to `64 MiB`, `near_alias`'s
`want_size >= (64u << 20)`, `reach = 2 GiB` in both `husk-tl-xmem.c` and
`husk-ios-jit.c`'s writable-alias placement loop) is untouched.

### 4c. The EXEC GATE pair — RAN

`husk_ios_jit_detach()` (`husk-ios-jit.c:709+`) brackets the detach with two
gates, and each gate's verdict line is
`EXEC GATE after <name>: … -> RAN -- execute is still there` (or `FAULTED …`):

```
EXEC GATE after immediately BEFORE the debugger detach: … -> RAN -- execute is still there
EXEC GATE after immediately AFTER the debugger detach: … -> RAN -- execute is still there
```

plus the third gate at the claim, `EXEC GATE after the claim (prewarm)` — the
baseline the pair is differenced against (`:463`). All three call the file's
`static` region gate, which is why the weak/static split in §2 keeps them
byte-for-byte the same instrument. The layer's own arena gate
(`husk-tl-xmem.c:1463`) is unchanged and is what `JITBootstrap` /
`JITSetup` / `SettingsTab` call, also unchanged.

### 4d. Invariants, checked against the diff

* no `vm_protect` on the region — nothing added; the only `vm_protect` calls are
  the pre-existing `husk_rw_alias` / `husk_ios_jit_allocate_real` ones, untouched
* the executable view is never written — the gate writes the **writable** alias
  (`rw`) and restores it; unchanged
* nothing the guest holds is unmapped — `husk_ios_jit_release` is unchanged and
  the prewarm path does not call it
* no thread that ran guest code is ended or joined — no thread is created or
  touched by this change
* the honest refusal path intact — `husk_self_dual_mapping`'s named refusals and
  `husk_ios_jit_mapjit_works`'s guarded probe are unchanged; §3 *adds* a refusal
  (the absent engine), it does not replace one
* the f96a98a view rule untouched — `husk-tl-xmem.c` and the whole
  `src/translation-layer-next` tree are not modified at all
* the 77d0d53 arena-size setting untouched — `JITBootstrap.swift`'s size code is
  not modified at all

---

## 5. Files changed

| file | what |
|---|---|
| `src/app/project.yml` | `../ios-jit/husk-ios-jit.c` + `husk-brk.S` added to the app target; the `libqemu-aarch64-softmmu.dylib` framework dependency **removed** (that is what takes the file out of `Payload/Husk.app/Frameworks`); comments updated |
| `src/app/Husk/QemuEngineShim.c` | **new** — `husk_qemu_engine_present()` + the 22 engine entry points the app calls, each a forwarder through a dlopen handle with a named no-op fallback |
| `src/app/Husk/QemuEngine.swift` | **new** — `present`, `absentMessage`, `logAvailability()` |
| `src/app/Husk/HuskBridge.h` | two-provider header comment; `husk_qemu_engine_present()` declared; JIT section says it is the app's |
| `src/app/Husk/QemuRunner.swift` | `run()` refuses with the named reason when the engine is absent (after the existing guest-payload refusal) |
| `src/app/Husk/ContentView.swift` | `start()` names the absent engine and raises the alert (`guestUnavailable`) |
| `src/app/Husk/GuestRuntime.swift` | the "this cannot happen" bullet is now the real case, and says so |
| `src/ios-jit/husk-ios-jit.c` | `pipe2` weak; `__has_include` for `tcg/tcg-apple-jit.h` with a libsystem fallback; `husk_ios_jit_exec_gate` → `static` region gate + weak public wrapper; the three internal gate calls point at the region gate |
| `scripts/build_ipa_prebuilt.sh` | QEMU no longer staged (staging step replaced with a named note); donor check no longer requires it; manifest line honest |
| `scripts/package_ipa.sh` | the stale-dylib restage block removed; validation now **requires QEMU to be absent** and still requires ANGLE |
| `scripts/guest-unbundled.md` → `.hermes/plans/engine-reset/guest-unbundled.md` §5/§10 | unchanged — its §10 is what this change is |

Not touched: `tools/a32`, `src/translation-layer*` (any file), `src/java-framework`,
`tests/`, `.github/workflows/`, `patches/`, `scripts/integrate_husk.sh` (the
engine's own build still copies the JIT into QEMU's `tcg/`, which is still right
for a from-source engine).

---

## 6. Size

| entry | compressed | uncompressed |
|---|---:|---:|
| `Payload/Husk.app/Frameworks/libqemu-aarch64-softmmu.dylib` | **6.7 MB** | **33.9 MB** |
| added: `husk-ios-jit.c` + `husk-brk.S` in the app binary | ~0.03 MB | ~0.04 MB |
| added: `QemuEngineShim.c` in the app binary | ~0.005 MB | ~0.01 MB |

* IPA before: **84.9 MB** (as measured on the running build).
* IPA after (expected): **~78.2 MB** — **−6.7 MB** (−7.9% of the IPA), the
  measured packed size of the dylib, less a hundred KB of new app code.
* `Payload/Husk.app/Frameworks/libqemu-aarch64-softmmu.dylib`: **absent**. The
  absence is asserted in `scripts/package_ipa.sh` (it fails the build if the
  library reappears), so the next build either shows it gone or says why not.

This is a measurement-by-arithmetic on an artifact built elsewhere, not a
re-measurement here — the number to confirm is `zipinfo`/`unzip -l` on the new
`build/Husk.ipa`. The claim that matters is the one with real evidence: the entry
is 6.7 MB packed in the IPA he is running.

---

## 7. What blocks it, and what only a real build can settle

Nothing blocks the change; it is landed and pushed. Four things cannot be settled
from here and are named rather than assumed:

1. **It compiles.** No toolchain on this machine. The two riskiest constructs are
   `__has_include("tcg/tcg-apple-jit.h")` (true in QEMU's build, false in the
   app's) and the weak/strong pairs (`pipe2`,
   `husk_ios_jit_exec_gate`). A duplicate `husk_ios_jit_exec_gate` or `pipe2` link
   error, or a `jit_write_protect` redefinition, would be the failure shape; the
   fix in each case is written next to the code.
2. **That the app LAUNCHES with no engine in `Frameworks/`.** This is the dyld
   question: the dylib is no longer a dependency at all, so there is no
   `LC_LOAD_DYLIB` left to fail on — but that is a claim about a build.
3. **That the three log lines actually appear.** They are read from the code and
   the code that produces them is unmodified and now in the app's own image; the
   run is his to make. The engine's absence can only remove them if the JIT path
   had been reaching into the engine — which §1 says it never did.
4. **The `GuestRuntime`/`QemuEngine` wording in the UI** — reviewed, not compiled.

One consequence to know about, recorded in `QemuEngineShim.c` as well: with the
JIT in the app, a build that *re-embeds* the engine has two copies of the JIT
substrate, and the engine's own copy keeps its own `husk_prewarmed` state — it
would claim a second region at `qemu_init()` rather than the region the arena
adopted, instead of the single region the design intends. It still runs (the
debugger's grant is held for the run), and it is moot in a shipped build because
the engine is not there at all. That is a second reason the engine is meant to
stay out, and it is the one thing to fix if the guest path is ever turned back on.
