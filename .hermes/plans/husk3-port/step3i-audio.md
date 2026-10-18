# Step 3i — `husk-tl-audio.c`: the last layer file (a COPY, not a merge)

SOURCE = `C:/Users/Aiden/Desktop/HuskApp` (our fork, git). TARGET = `C:/Users/Aiden/Desktop/husk3.0`
(upstream, **not a git repo — no git was run inside it**). Commit records live in SOURCE only.

Owner of one file: `src/translation-layer-next/husk-tl-audio.c`. `husk-tl-audio.h` was already ported by the
unity-app worker (`step3e-unity-app.md`) and was **not touched here**. `husk-tl-codewrite.c` belongs to another
worker and was not touched.

## Verdict: **copy** (ours is a strict superset). How that was measured, not assumed

The brief has been wrong four times on this port, so direction was established three ways:

1. **What upstream had** — fetched `raw.githubusercontent.com/Leviidev/Husk/main/src/translation-layer-next/husk-tl-audio.c`
   and `cmp`-compared it to the target's copy: **byte-identical**, sha256 `65040aedf8ed33cf2f3c7b9587b35e6d9c6a8427c4b4bf76c1906a6f2cff4749`, 6,870 B / 161 lines.
   So the target's 161-line copy *is* upstream verbatim, and "nothing of upstream's was dropped" reduces to
   "every upstream line is present in ours".
2. **Did we ever have the new symbols** — `git log -S` in our fork: `tl_audio_close` and `AudioSessionSetActive`
   both first appear in commit **`10727a4` "session close: stop the game the user closed, and stop lying about it"**.
   They are *our* deliberate additions, not upstream's.
3. **Superset test, line by line** — every non-empty line of the target's copy appears in ours, with equal or
   greater multiplicity. Exactly **two** upstream lines are not present verbatim, and both are the *same
   construct widened*, not dropped:
   - `bool paused, failed;` → `bool paused, failed, closed;` (the struct gained the `closed` flag; upstream's two fields survive)
   - `void tl_audio_install(void) { tl_cocos_audio_hook = getenv("TL_AUDIO_MUTE") ? muted_write : host_write; }`
     → the same statement kept verbatim as the first line of a widened block body.

   No other upstream line is absent or reduced. → **ours is a strict superset; the correct action is a copy.**

## What ours carries (the content the copy preserves)

| region | why it matters |
|---|---|
| `A.closed` + the `if (A.closed)` early-return in `ring_write` (`.c:116-123`) | the clean-close rule. A writer that outlived its game — a guest thread we could not stop — is **dropped and paced** here, so it can neither reopen the queue nor spin filling a ring that belongs to a run that is gone. This is the "a guest crash ends the SESSION, never the process" rule applied to the audio writer. |
| expanded `tl_audio_install` (`.c:163-174`) | a launch **re-arms** the sink a previous close silenced: clears `closed`/`paused`/`failed`, restarts a paused queue, broadcasts the condvar. Without it a second game in the same process cannot play. |
| `tl_audio_close` (`.c:199-207`) | **the symbol the brief was chasing.** Pauses the AudioQueue (the clock stops, the ring is **not drained**), sets `closed` so surviving writers drop, deactivates the host audio session. **It frees nothing and joins nothing** — a guest writer parked in the sink is left parked, never handed a freed ring. |
| `tl_audio_session_active` + `dlsym("AudioSessionSetActive")` (`.c:191-197`) | iOS keeps the session (and the "audio is playing" indicator + route lock) active until the app deactivates it; looked up lazily because macOS has no such session. |
| `#include <dlfcn.h>` (`.c:6`) | required by the `dlsym` above; upstream's copy has no `dlsym`. |

The audio pump lifecycle itself (the `on_buffer` callback, `open_queue`, the ring, `host_write`'s 2× up-sampling
for a 24 kHz FMOD mixer) is **byte-identical** between the two copies — only the close/re-arm additions are ours.
That is consistent with the brief's evidence: a real device log shows `audio close: stopped an aaudio pump
(992256 frames it had written)`, i.e. the pump ran and was then closed cleanly.

## Upstream content preserved / integrated

**None had to be integrated** — there was no upstream-only content. Every upstream *code* line survives
verbatim inside our copy (proving it twice: the `cmp` against upstream HEAD establishes what upstream had; the
line-multiset test establishes ours contains all of it). The only two lines not byte-identical are the two
same-construct widenings listed above, each of which keeps upstream's exact expression.

The pre-merge upstream bytes are parked for recovery at `.husk3-merge/upstream/husk-tl-audio.c` (6,870 B),
outside both trees, as `PLAN.md` requires for a non-git target.

## `tl_audio_close` is now DEFINED, and what else this file supplies

In `husk3.0/src/translation-layer-next/husk-tl-audio.c`:

| symbol | line | storage |
|---|---|---|
| `tl_audio_install` | 163 | extern |
| `tl_audio_set_paused` | 176 | extern |
| `tl_audio_session_active` | 191 | static (internal) |
| **`tl_audio_close`** | **199** | extern ← the link hole is closed |

The call site was already merged and now resolves: `husk-tl-unity-app.c:1253` calls `tl_audio_close()`, declared
in the ported `husk-tl-audio.h:32`. **Nothing else in the target declares or defines `tl_audio_close`** (grep: only
the header declaration and the one call site), so there is no duplicate.

### Not duplicated here — deliberately

`husk-tl-audio.h` also declares `tl_aaudio_stop_all`, `tl_opensl_stop_all`, `tl_dvm_audio_stop`, but their
definitions live in other files and already exist in the target; this file supplies none of them:

- `tl_aaudio_stop_all` → `husk-tl-bionic-ndk.c:485`
- `tl_opensl_stop_all` → `husk-tl-opensles.c:235`
- `tl_dvm_audio_stop`  → `husk-tl-dvm-audio.c:147`

### The close ORDER the hazard demands is preserved

`husk-tl-unity-app.c:1249-1253` (already merged, not this task):

```
tl_aaudio_stop_all();   /* the layer's audio producer threads end while the host sink is still live, */
tl_opensl_stop_all();   /*   so an in-flight hook returns instead of parking and the joins cannot hang */
tl_dvm_audio_stop();
int loops_live = tl_loop_quit_all();
tl_audio_close();       /* only NOW is the sink silenced                                       */
```

Producers are stopped **before** the sink is closed — exactly the ordering our `husk-tl-audio.h` prose describes.
A torn-down pump cannot take the process with it: `tl_audio_close` pauses and marks closed, frees nothing, joins
nothing, and leaves any still-parked writer parked.

## Other symbols the file needs, and where they are defined in the target

- `tl_log_line` — declared `husk-tl-bionic.h:111` (definitions exist across the layer).
- `tl_cocos_audio_hook` — `husk-tl-cocos.h:86` (already in the target).

No link hole remains for anything this file references.

## Compile check — real, and honestly bounded

zig 0.13.0 is present: `/c/Users/Aiden/AppData/Local/Temp/zigdl/zig-windows-x86_64-0.13.0/zig.exe`.

**With the real toolchain flags, the file does NOT compile here — and that is an environment gap, not a merge
failure, exactly as the brief predicted:**

```
zig cc -target aarch64-macos -D_DARWIN_C_SOURCE -Isrc/translation-layer-next -Isrc/translation-layer \
    -Wno-unused-parameter -Wno-unused-function -c .../husk-tl-audio.c -o $LOCALAPPDATA/Temp/zigobj/out.o
# src/translation-layer-next/husk-tl-audio.c:2:9: warning: '_DARWIN_C_SOURCE' macro redefined
# src/translation-layer-next/husk-tl-audio.c:5:10: fatal error: 'AudioToolbox/AudioToolbox.h' file not found
# zig exit=1
```

zig bundles no `AudioToolbox` (it ships no Apple SDK frameworks), and this one file is a framework consumer by
design. So the plain compile stops at line 5.

**What WAS verified instead** — a minimal stub `AudioToolbox/AudioToolbox.h` (declaring only the AudioQueue
types/constants/functions this file uses, in `$LOCALAPPDATA/Temp/atstub/`) let the file's *own* C be compiled
for real:

```
zig cc -target aarch64-macos -D_DARWIN_C_SOURCE -I$atstub -Isrc/translation-layer-next -Isrc/translation-layer \
    -Wall -Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
    -Wno-unused-parameter -Wno-unused-function -c .../husk-tl-audio.c -o $LOCALAPPDATA/Temp/zigobj/out.o
# exit 0; 28,888-byte object; only the benign _DARWIN_C_SOURCE redefinition (from -D on the command line)
```

`-Werror=implicit-function-declaration` is the meaningful gate: **every** symbol the file calls is declared
somewhere it includes. Reading the object's string table confirms the four symbols and the `dlsym` name landed:

```
tl_audio_close   tl_audio_install   tl_audio_session_active   tl_audio_set_paused
AudioSessionSetActive        (+ the AudioQueue* externs)
```

This is real evidence that `tl_audio_close` compiles as a definition and that the file has no missing
declarations — but see "what could NOT be verified".

## What could NOT be verified

- **The stub is not AudioToolbox.** The `exit 0` compile used a hand-written header, so it is *not* proof the
  file compiles against the real Apple framework — it is proof the file's own C is well-formed and every symbol
  it calls is declared. The genuine framework compile needs a macOS machine / Apple SDK.
- **No iOS SDK, no device, no full husk3.0 build.** No `aarch64-apple-ios` build was run and none is claimed; no
  Swift was compiled at all. A build of husk3.0 remains the only real proof and is not claimed here.
- **Runtime behaviour is not exercised.** The clean-close path (a parked writer being dropped and paced, the
  session deactivation, the re-arm on the next launch) is *read*, not *run*. The device log line quoted in the
  brief is the only runtime evidence that the pump works, and it is upstream of this box.
- **The target is edited concurrently by other workers.** The symbol-location table above is a snapshot; re-scan
  after every layer file has landed.
- Nothing outside `src/translation-layer-next/husk-tl-audio.c` was written; the header, Swift, `project.yml`,
  `tools/`, `scripts/`, `.github/` were untouched. No git command was run inside `husk3.0` (it is not a repo).
