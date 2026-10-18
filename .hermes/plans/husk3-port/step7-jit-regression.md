# Step 7 — the JIT regression: 2.0 got the region, 3.0 never does

SOURCE = `C:/Users/Aiden/Desktop/HuskApp` (our fork, 2.0 — **ran games on this exact phone**)
TARGET = `C:/Users/Aiden/Desktop/husk3.0` (upstream 3.0 + our port, branch `husk3` of Aman122608/husk-build)

Logs read in full:

- WORKING — 2.0, build `2.0.0-112`: `C:/Users/Aiden/Desktop/HuskLogs/husk-20261016-130230-2.0.0-112.log`
  (**gone** — the owner emptied the directory; the grant steps below are the lines quoted in the brief
  plus the same sequence quoted in `HuskApp/.hermes/plans/engine-reset/jit-grant-held-for-the-run.md`
  §6 and `jit-session-not-ended.md` §6, which quote `husk-20261015-085540` and `husk-20261016-*`.)
- BROKEN — 3.0, build `3.0.0-129`, commit `1c97824-dirty`:
  `C:/Users/Aiden/Desktop/HuskLogs/husk-20261018-100534-3.0.0-129.log` (249 lines, read in full).

**No Mac, no iOS toolchain, no device here.** Correctness is by construction: per-file diffs, the two
built IPAs compared field by field, the app binaries compared symbol by symbol and byte by byte where
it matters, and Swift brace/paren balance re-checked (0/0/0) on every file touched. **No build was run
and none is claimed.**

---

## 1. What 2.0 did at each step of the grant — and what 3.0 does instead

| step | 2.0 (worked) | 3.0 (broken) |
|---|---|---|
| process state at launch | `debugged at launch: true`, `TrollStore install: false; jailbreak: false; can grant its own JIT: false` (`jit-session-not-ended.md` §1 quotes this for `085540`) | identical line, identical values: `[2.10ms][jit] debugged at launch: true; TrollStore install: false; jailbreak: false; can grant its own JIT: false` |
| the claim | `[jit] claiming 512 MiB of JIT memory now, before the guest download …` — `JITBootstrap.prewarm()` from `ContentView.evaluate()` on the first foreground pass, gated on `isDebuggerAttached` (a `csops`, not a probe) | identical line, 73.79 ms, same caller, same gate — `[23] csops status = 0x32003005, CS_DEBUGGED = set` |
| cheap attach probe (`brk #0x69`, `husk-brk.S`) | `[husk-jit] #1: StikDebug attach probe OK (0xcccccccc690000e0)` — low word `0x690000e0` is the byte-reversed `0xE0000069` the servicer's script writes into x0 (`husk-jit.js` `JIT26HandleBrk0x69`, `P0=E0000069`) | `[husk-jit] #1: StikDebug is NOT servicing traps -- probe returned 0, which is our own SIGTRAP handler stepping over an unanswered brk` |
| the region request | `[husk-jit] #1: requesting execute-capable region, size=536870912 (512.0 MiB), attempt 1/3` → `BreakGetJITMapping returned 0x1144e4000` (`brk #0xf00d`, x16 = 1) | **never issued.** `husk_prewarm_done` was set by the failed attempt (below), so every later `husk_ios_jit_prewarm()` returned false without a probe, and `husk_ios_jit_allocate()` answered `prewarm failed earlier; not trapping again` |
| the writable alias | `[husk-jit] #1: dual mapping established: rw=0x1344e4000 rx=0x1144e4000 size=536870912 diff=-536870912` (`vm_remap` twin) | — |
| the proof | `[husk-jit] selftest: PASS -- executed generated code from the RX alias, got 42. JIT is genuinely live.` | — |
| the arena | `xmem: arena …: 512 MiB, 512 MiB MAPPED, 512 MiB executable, 0 NOT executable` | `xmem: a plain RWX guest-facing view was created but neither view executed a function written into it; not adopting it` ×N, then `ld: no arena of the layer's own (this process cannot make executable memory for itself (Operation not permitted)…)` |
| the run | `unity: alive: <frames climbing>` | `dvm: libicu_jni.so did not load` → `dvm: initialising java/lang/System threw java.lang.NoClassDefFoundError` → `unity: alive: 0 frames (+0)`; the same for TheLewdestHouse (`renpy`) |

**The one line that decides it** is in the 3.0 log at 72.01 ms, and the seven claim attempts after it:

```
27|[    72.01ms][husk-jit] #1: StikDebug is NOT servicing traps -- probe returned 0 …
32|[   148.52ms][jit] claiming 512 MiB of JIT memory now, before the guest download …
33|[   148.55ms][jit] JIT prewarm FAILED -- StikDebug is not servicing traps
85|[ 63929.38ms][jit] claiming 512 MiB …     86|[ 63929.41ms][jit] JIT prewarm FAILED …
96|[ 84635.76ms][jit] claiming 512 MiB …     97|[ 84635.84ms][jit] JIT prewarm FAILED …
238/239, 242/243, 246/247: the same three more times …
```

Seven more "claiming … / prewarm FAILED" pairs and **not one further `#N:` probe line**: the C latch
`husk_prewarm_done` refused every one of them. So even a servicer that arrived a moment after launch
could never have granted the region in that process.

---

## 2. What the port changed — verified per file, not from any table

Every file on the JIT path was diffed `HuskApp` ↔ `husk3.0` in full. Result:

| file | difference |
|---|---|
| `src/ios-jit/husk-brk.S` | **none** |
| `src/ios-jit/husk-ios-jit.h` | **none** |
| `src/ios-jit/husk-ios-jit.c` | one hunk: upstream 3.0's **two-layout mmap fallback in `husk_self_dual_mapping`** (`rwx then rx`, then `rx`, each step naming itself). Kept, untouched — it is the pre-iOS-26 self-grant route and cannot run here (`TXM device: MAP_JIT cannot execute here, not probing`; the very next line in the log is `self: iOS 26 enforces TXM; only a debugger can grant executable memory here`). |
| `src/app/Husk/JITBootstrap.swift` | two hunks, both **upstream 3.0's newer work**: the TrollStore install probe (marker file + suspended SpringBoard call) replacing the `apple-magnifier://` `canOpenURL`, and the matching `guard isTrollStoreInstalled`. Nothing else: `prewarm()`, `ready`, `isDebuggerAttached`, `mapJITWorks`, `requestAttach`, `detachIfIdle` are byte-identical. |
| `src/app/Husk/JITSetup.swift` | line endings (CRLF) + upstream's TrollStore auto-JIT at launch (`trollStoreAutoKey`, `enableAtLaunchIfAsked`). Identical otherwise. |
| `src/app/Husk/JITCard.swift`, `JITBuiltIn.swift`, `JITPairing.swift`, `JITSetupView.swift` | `JITCard`/`JITBuiltIn`/`JITPairing` **identical**; `JITSetupView` differs by upstream's iOS-15 `.huskDestination(for:)` only. |
| `src/app/HuskJITHelper/*` | `HuskJITHelper.swift`, `HuskJITMessages.swift` **identical**; `Info.plist` differs by `CFBundleVersion` 112 → 113 only. |
| `src/app/Husk/HuskLog.swift`, `HuskApp.swift` (launch order), `HuskBridge.h`'s JIT block, `Resources/husk-jit.js` | **identical** (`husk-jit.js` is byte-identical, md5-checked in tree and in both IPAs). |
| `Husk.entitlements` | **identical** (`get-task-allow`, `dynamic-codesigning`, both memory entitlements). |
| `src/app/project.yml` + generated `pbxproj` | our port's only JIT-relevant change: `deploymentTarget` 16.0 → **15.5** (upstream's floor, kept on purpose) and the added PS4 dylib. The helper target, the **Embed StikJIT** phase, the entitlements wiring and the sources list are all intact — `husk-ios-jit.c`/`husk-brk.S` are in the app target in both trees, and the generated `project.pbxproj` the 3.0 IPA was built from carries both. |
| `scripts/integrate_husk.sh`, `ci_build.sh`, `package_ipa.sh`, `build_ipa_prebuilt.sh` | nothing on the grant; one comment about `husk_ios_jit_reserve_bottom` (the QEMU-sharing export our fork does not have). |

**Then the two built IPAs, field by field** (2.0 = `Husk-1016-1306-6f0191f…ipa`, build 112;
3.0 = `HuskBuilds-dl-husk3-1c97824/Husk.ipa`). Extracted, and compared with `plistlib` and a Mach-O
reader rather than by eye:

- `Info.plist`: same `CFBundleIdentifier` (`com.husk.app`), same `LSApplicationQueriesSchemes`
  (`['stikjit','stikdebug','stosdebug','apple-magnifier','localdevvpn']`), same key set, same
  `CFBundleURLTypes`. **The query-scheme plists are ruled out, as stated in the brief.**
- entitlements (blob type 5 out of `LC_CODE_SIGNATURE`): both are exactly
  `get-task-allow=true, dynamic-codesigning=true, increased-memory-limit=true,
  extended-virtual-addressing=true`.
- `PlugIns/`: both carry `HuskJITHelper.appex` (identical size, `CFBundleIdentifier
  com.husk.app.JITHelper`, `MinimumOSVersion 26.0`, same `NSExtension` dict) and
  `HuskDownloadsWidget.appex`.
- `Frameworks/`: both carry `StikJIT.framework` — **md5-identical** binaries
  (`56bf3c34cd111c125d99719c0f54c69d`) and identical `universal.js`/`legacy.js`.
- the main binary: `_husk_brk_probe`, `_husk_brk_get_jit_mapping`, `_husk_brk_jit_detach` are at the
  **same addresses** in both (`0x1000053b8`, `0x1000053a0`, `0x1000053ac`) and compiled from the
  identical `husk-brk.S`, and `husk-ios-jit.o`/`husk-brk.o` are in both. 3.0's app link is 2.0's plus
  `Compat15.o`, `ConsoleUI.o`, `PS4Library.o`, `PS4Screen.o`, `husk-console-host.o`,
  `husk-jit-arena.o` (upstream's PS4 work) — nothing removed.

**So the app's JIT path itself was not changed by the port.** What the broken run shows is a *state*
the worked run never sat in, and one place where the code could not recover from it:

```
2.0: [jit] claiming …            →  #1: probe OK  →  BreakGetJITMapping → selftest PASS → arena
3.0: [jit] claiming …            →  #1: probe returned 0 (nothing attached is answering)
     → husk_prewarm_done latched (src/ios-jit/husk-ios-jit.c) → no region, no arena, 0 frames, ever
```

`CS_DEBUGGED` is not "a servicer is answering". It is set for a process a launcher marked and it is
left behind by a debugger that let go — the file says so itself, about LiveContainer
(`JITBootstrap.swift`, `lastFailure`): *"A build running inside LiveContainer reports itself debugged,
answers no brk, and gets no executable memory."* In the broken run the flag was set and nothing held
the trap: the cheap probe was stepped over by our own SIGTRAP handler, which is itself the proof that
no debugger owned the task's stop for it. 2.0's log is the other order — a servicer attached and
answering at the moment of the claim.

**The exact line that changed behaviour** (it is the one that turned a transient state into a
permanent one, `src/ios-jit/husk-ios-jit.c`):

```c
    if (husk_prewarm_done) {
        return husk_prewarmed.rw_addr != NULL;      /* one failed probe = no region for the process */
    }
    husk_prewarm_done = true;
```

and its twin in the allocator: `if (husk_prewarm_done) { … "prewarm failed earlier; not trapping
again" … }`. On the app side the corresponding line is `JITBootstrap.ready`'s trust in the flag
(`if debuggedFlag || prewarmed || isLive { return true }`), which is why nothing ever asked StikDebug
or the helper for a real attach: the app believed it already had one.

---

## 3. The fix, in one build

1. **`src/ios-jit/husk-ios-jit.c` — a failed claim that went to NOBODY is retryable.**
   The probe's answer is now kept as state: `g_last_probe_found_nothing` is set when the probe
   returns 0 (**proof nothing was attached**, so a repeat cannot be a stop the process waits on) and
   cleared the moment a servicer *does* answer, so a servicer that refuses is never asked twice — the
   one-shot rule and its stall reasoning stay intact for exactly the case they were written for.
   `husk_ios_jit_prewarm()` may then claim again, bounded by `kClaimAttemptsMax = 3`; a held region
   still short-circuits; `husk_ios_jit_allocate()` follows the same rule so a QEMU start after the
   app's earlier failure can still be given a region. Each retry says so in the log.
2. **`src/app/Husk/JITBootstrap.swift` — `markedDebuggedWithoutRegion`** (`isDebuggerAttached &&
   !prewarmed`): the one flag combination that says the mark is worthless here.
3. **`src/app/Husk/JITSetup.swift` — `JITCoordinator.renewAttachAfterUnservicedClaim()`**: ask Husk's
   own method for a **real attach, once per process** — StikDebug by the `stikjit://` hand-off the
   owner describes, the built-in helper by an extension request, and for a jailbreak say which setting
   to turn on. No `brk`, nothing claimed: the attach is the whole ask. It is also gated on
   `!JITBootstrap.mapJITWorks`, so a device where a plain `MAP_JIT` mapping executes (the pre-iOS-26
   routes, where the layer makes its own region) is never asked for an attach it does not need. A
   method that cannot deliver (not installed, not paired) sets the same honest error and opens the
   walkthrough instead of leaving a run that cannot work.
4. **`src/app/Husk/ContentView.swift`** — `evaluate()` claims first and, when the claim fails on a
   process that is only marked debugged, asks (and stops there: with no region there is nothing to run
   a guest in); `start()` asks the same way before it refuses, so the Start button recovers instead of
   reporting.

Net effect on the broken run: the launch-time claim still fails (nothing was attached), but the app
now asks for the attach it never asked for, the claim stays open, and the next foreground pass claims
the region **while the servicer that answered is attached** — the order 2.0's log shows:

```
[jit] claiming 512 MiB of JIT memory now …
[husk-jit] #1: StikDebug attach probe OK (0xcccccccc690000e0)
[husk-jit] #1: BreakGetJITMapping returned 0x…
```

Everything upstream 3.0 added on this path was kept: the two-layout mmap fallback, the TrollStore
probe, `trollStoreAuto`, `.huskDestination`, the PS4 embed, and the 15.5 deployment target.

---

## 4. What could not be verified

- **No build and no device.** Nothing here is compiled or run; the C changes are the app's own
  translated logic and the Swift was brace/paren-checked only. A macOS build and one launch on the
  phone are the only judgement.
- **Which servicer the owner's working flow engages, and why none was attached in this run.** The
  app's side of that is gone from the log: there is no `requestAttach()` line and no
  `enabling JIT with the built-in helper` line anywhere in the 249 lines, and the two-phase
  `stikjit://`-hand-off logs its own first half in the *previous* process. What is *proved* is the
  state: `CS_DEBUGGED` set, no debugger holding the stop. The fix does not depend on knowing which
  one it was — it covers all four attachment paths.
- **Whether a retried probe can land while a debugger is attached but suspended** (the stall the
  one-shot rule was written for). It is bounded to at most three attempts, it is only reachable after
  a probe that proved nothing was attached, and the attach-ask is once per process — but this is a
  reasoned bound, not a measurement.
- **The indicator is still optimistic.** `JITBootstrap.ready` (and so `state`/`usable` and the "JIT is
  on" card) still reads true from the flag alone, so the UI can say "JIT is on" in the very state this
  report is about. That is deliberate here: tightening it would make `usable` false on the
  pre-iOS-26/TrollStore routes, where the region comes from the layer's own arena and `prewarmed`
  stays false. **Reported, not changed** — it needs its own decision.
- **`husk_ios_jit_get_mapping` is not in either app binary's symbol table** (the app's `dlsym` lookups
  in `husk-jit-arena.c` and `translation-layer/husk-tl-load.c:522` target it), in *both* builds, so it
  is not this regression — but the lookup is worth a look on its own, since it is how the layer would
  be handed the prewarmed region by name.
