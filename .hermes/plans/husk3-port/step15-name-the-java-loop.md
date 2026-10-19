# step15 — naming the JAVA loop that freezes Robot Girlfriend at 8 frames

Target: `C:/Users/Aiden/Desktop/husk3.0`, branch `husk3`, on top of `88668be` (the build note for the guest-stack
walk). Gate: every touched `.c` was compiled with

```
zig cc -target aarch64-macos -D_DARWIN_C_SOURCE -Isrc/translation-layer-next -Isrc/translation-layer -c <file> -o <t.o>
```

— `husk-tl-dvm.c`, `husk-tl-dvm-android.c`, `husk-tl-unity-app.c`, `husk-tl-bionic-pthread.c` all pass with only the
pre-existing `_DARWIN_C_SOURCE` redefinition warning. **No Mac, no iOS toolchain, no device: nothing below was run.**
Everything is read from the code and from `HuskLogs/husk-20261018-193130-3.0.0-129.log`, and is labelled as such.

## 0. What this step adds (and what it deliberately does not)

The brief asked for three things and, where the evidence could not name the answer, to say so and ship the instrument
that will. The instrumentation is now in place; the identity it prints is determined by the NEXT run:

1. **The JAVA frame chain of the layer's Java threads is now walked and printed at the stall** (§1). The guest
   frame-pointer walk of a thread inside the interpreter could only ever name `dvm_call`/`invoke_regs`/`dvm_call_inner`
   — the interpreter's own C functions. It printed a twenty-four-frame chain of nothing but those (§1's log excerpt).
   The interpreter DOES keep a walkable Java frame chain; a thread that runs Java now registers the addresses of its
   own frame stack once, at entry, so the cold stall path can read `class.method+descriptor` frames from OUTSIDE that
   thread.
2. **Those threads are now sampled TWICE**, a short bounded interval apart, and the log says whether the pc and the Java
   frame MOVED or HELD (§2). Today's log cannot distinguish "looping in Java" from "sitting deep in one Java call"; the
   two-sample decides it.
3. **UnityMain is now found by NAME from the process-wide thread list** and walked, instead of through the layer's
   unity-thread handle, which is null under the JAVA engine because Unity's own native code — not the layer — started
   that thread (§3).

No always-on cost was added. Registration happens ONCE per thread, at its entry (thread creation is not a hot path);
the Java-frame read and the twice-sample run only on the cold stall path (once per detected stall). No thread is ever
suspended, joined or signalled: the probe only READS a live thread's state and frame pointers, exactly as the existing
`ss_thread_pc` does. Nothing on a running path changed.

## 1. The JAVA frame chain (`dvm_thread_java_frames`, and `ss_java_walk` in husk-tl-unity-app.c)

### Why the existing walk could not name it

`stall-guest-stack` on the Java driver's app/main+looper thread printed, at the stall:

```
frame 0  -> lr [Husk dvm_call+0x278]
frame 1  -> lr [Husk invoke_regs+0x9b0]
frame 2  -> lr [Husk dvm_call_inner+0x53c]
... replicating dvm_call / invoke_regs / dvm_call_inner to frame 23,
    "the walk reached the 24-frame bound without ending"
```

That is the interpreter's own recursion: `invoke_regs` calls `dvm_call`, which calls `dvm_call_inner`, which calls
`interpret`, which on the next invoke opcode calls `invoke_regs` again. Every Java call is one turn of that C cycle, so
a frame-pointer walk can only ever name the C functions, never the Java method running inside them. The brief is right
that "the DVM knows" — it is the C walk that cannot ask it.

### What the interpreter actually keeps

`husk-tl-dvm.c` already keeps a per-thread Java frame chain (`t_frames[8192]` / `t_pcs[8192]` / `t_depth`, "the stack
Reflection and stack traces read" — `dvm_capture_frames` and `tl_dvm_print_stack` read it). It is `_Thread_local`, so
only the running thread can read its own. This step makes it readable from another thread:

- `dvm_thread_register_frames()` records, in a small process-wide table keyed by `pthread_t`, the ADDRESSES of that
  thread's own `t_frames`/`t_pcs`/`t_depth`. It is called once, at the entry of each thread that runs Java:
  `app_main` and `gl_main` (husk-tl-dvm-android.c) and every guest thread `start_thunk` (husk-tl-bionic-pthread.c).
  It is the counterpart of registration in reverse: `dvm_thread_unregister_frames()` on the way out, and
  `dvm_thread_frames_reset()` from `tl_dvm_end_app` so a later probe never reads a dead thread's unmapped TLS.
- `dvm_thread_java_frames(tid, out, max)` lets the cold stall path read another thread's frames. It reads `t_depth`
  ONCE and clamps it to `[0, 8192]`, then reads that many `dvm_method*` and their pcs — a read-only inspection that
  never touches the sampled thread's scheduling. It returns **-1 when the thread registered no Java frame stack**
  (it does not run through the interpreter), and the walk prints that plainly rather than invent one.

### The line the next log will carry

```
stall-java-stack: the Java driver's app/main+looper thread: its JAVA frame chain, INNERMOST FIRST
  (class.method+descriptor), read from the frame stack the thread registered ... N frame(s):
stall-java-stack:    java #0  android/os/MessageQueue.next()Ljava/lang/Object; @<pc> (line <L>)
stall-java-stack:    java #1  android/os/Looper.loop()V @<pc> (line <L>)
stall-java-stack:    java #2  husk/AppRunner.run(...)V @<pc> (line <L>)
...
```

### What the EXISTING log already tells us (inference, not the new line — labelled)

The old log happens to contain **two** stall probes of this same process, seconds apart (lines 280–305 and 434–453),
which is itself a two-sample of the app/main+looper thread:

- **First probe:** its guest FP walk was 23 unbroken frames of `dvm_call`/`invoke_regs`/`dvm_call_inner` — i.e. that
  thread was *executing interpreted Java at that instant*, deep, with no bottom reached.
- **Second probe:** its chain was
  `dvm_monitor_wait` ← `Object_wait` ← `dvm_call` ← `invoke_regs` ← … ← `tl_dvm_call_static` ← `app_main+0x8c`.
  That is the classic idle Android main looper: `app_main` → `tl_dvm_call_static("husk/AppRunner","run")` →
  `Looper.loop()` → `MessageQueue.next()` → `Object.wait()`.

So the looper thread **changed state between the two probes** — from executing Java to parked in `Object.wait`. It is
not one fixed runaway loop; it ends up idle, waiting for a message that never arrives. The exact Java method of the
*first* probe's execution cannot be recovered from the C-only walk in the old log — that is precisely what this step's
instrumentation names, and the report of it must come from the next run.

## 2. SPIN vs BLOCK — the twice-sample (`ss_java_sample_twice`)

At the stall, for the Java driver's app/main+looper thread and for the frame/render thread, the probe now takes the pc
(`thread_get_state`) and the innermost Java frame (`dvm_thread_java_frames(...,1)`), sleeps a bounded **300 ms**, and
takes both again, printing:

```
stall-java-sample: <thread> (tid <p>): sample #1 pc [<sym>] java <Cls.meth+sig> @<pc>;
  300 ms later sample #2 pc [<sym>] java <...> -> MOVED (progressing: not sitting in one Java place)
                                                      | HELD (pc AND Java frame UNCHANGED: stuck/blocked, or spinning in ONE place)
```

The interval is a `nanosleep` on THIS (the probe's) thread. The sampled thread is never suspended, joined or signalled,
and nothing about it is written — so the observation cannot perturb the thing observed.

What the existing log already suggests (labelled as inference, from §1's two probes): the looper thread **MOVED**
(executing → idle in `Object.wait`), which points at a **missing signal** — the callback/message the main looper waits
for is never posted — rather than a spin. The new line confirms or refutes this per-run.

Note on what "HELD" can mean, so it is not over-read: `t_pcs` for the innermost frame is written at each **invoke**
opcode (`husk-tl-dvm.c` line ~1954/1963), not every instruction. A thread spinning inside one method *without making
calls* holds that particular pc but still MOVES its mach pc (which is inside `interpret`); a thread sitting deep in one
non-returning call, or blocked in `Object.wait`/a monitor, holds both. The verdict line reports MOVED when pc OR Java
frame changed, so a genuine Java spin still reads MOVED.

## 3. UnityMain, found by name

`ss_probe` walked `UnityMain` only through `tl_unity_thread()`, which is null on the JAVA engine: this title's
`UnityMain` (log `0x1761c7000`) was started by Unity's own native code, so no layer handle holds it, and its
frame-pointer walk was silently **skipped** (the log shows GLThread and `main` walked, and no UnityMain walk at all).
The fix, in `ss_probe`:

```c
unsigned long unity_id = tl_unity_thread();
if (!unity_id) unity_id = ss_find_thread_by_name("UnityMain");   /* the engine handle is null: find it by NAME */
```

`ss_find_thread_by_name()` enumerates the process with `task_threads` — the same list `ss_host_threads` already walks —
and returns the `pthread_t` whose `pthread_getname_np` name equals `UnityMain`, reading only its state. `ss_host_threads`
also now labels that thread's row by name (it was otherwise printed as "a layer-started or host thread"), so a run where
the engine handle is null still names it.

The existing log, before the fix, still shows UnityMain's sampled pc: `__semwait_signal` with `lr nanosleep+0xdc` and
`wait-args x0=0xa03 x1=0 x2=0x1 x3=0x1` — byte-for-byte the same paced `nanosleep` wait as GLThread. So UnityMain is in
a pacing sleep, not computing. Its frames ABOVE that sleep are what the walk now adds. This thread runs under the JAVA
engine's own native code (il2cpp), so it registered its frame stack via `start_thunk`; the new walk will report its Java
frames (likely empty — it is largely native) or say plainly that it holds none.

## 4. Files changed

| File | Change |
| --- | --- |
| `src/translation-layer-next/husk-tl-dvm.h` | Declares `dvm_thread_register_frames` / `dvm_thread_unregister_frames` / `dvm_thread_frames_reset` / `dvm_java_frame` / `dvm_thread_java_frames`. |
| `src/translation-layer-next/husk-tl-dvm.c` | The frame-slot registry (256 slots), register/unregister/reset, and the read-only `dvm_thread_java_frames`; `tl_dvm_end_app` now calls `dvm_thread_frames_reset()`. |
| `src/translation-layer-next/husk-tl-dvm-android.c` | `app_main` and `gl_main` register their frame stack at entry and unregister on every exit path. |
| `src/translation-layer-next/husk-tl-bionic-pthread.c` | `start_thunk` registers at entry and unregisters after `c.fn` returns, so guest Java threads are walkable too. |
| `src/translation-layer-next/husk-tl-unity-app.c` | `ss_java_walk` + `ss_java_take`/`ss_java_sample_twice`; `ss_find_thread_by_name`; UnityMain found by name; `ss_host_threads` labels UnityMain by name; `ss_probe` prints the Java chain and the twice-sample. |

## 5. What could NOT be verified (and must be read off the next run)

- **The Java method identity itself.** No device, no Mac: the code is correct by construction and compiles, but the
  actual `class.method` of the looper's loop is printed only when the app next runs. §1's inference is from the old
  log's two probes and is labelled as inference.
- **The MOVED/HELD verdict per run.** The two-sample is implemented and bounded, but its verdict is likewise a next-run
  reading; §2's "MOVED → missing signal" is inference from the two probes already in the old log, not from the new line.
- **UnityMain's frames ABOVE the sleep.** The old log had no UnityMain walk (the gap this step closes). Its pc is the
  same paced `nanosleep` as GLThread; the frames above it are a next-run reading.
- **The exact interpreter-frame race.** `dvm_thread_java_frames` reads `t_depth` then that many pointers from a LIVE
  thread's TLS. On arm64 the depth read and the pointer reads are individually atomic, but the two are not a snapshot:
  a frame pushed or popped between them can show one stale innermost frame. It is a cold diagnostic on a stalled thread
  and never writes or perturbs the target; the alternative (suspending the target) is forbidden by the brief.
- **No end-to-end run.** Per the operating constraints there is no iOS toolchain or device here, so nothing was
  executed; every claim is from the code, the compile gate, and the labelled reading of the log.
