# step14 — naming the retry loop that freezes Robot Girlfriend at 8 frames

Target: `C:/Users/Aiden/Desktop/husk3.0`, branch `husk3`, on top of `9883b36` (the build note for the sampler that
produced the log this step reads). Gate: both touched `.c` files were compiled with

```
zig cc -target aarch64-macos -D_DARWIN_C_SOURCE -Isrc/translation-layer-next -Isrc/translation-layer -c <file> -o <t.o>
```

— `husk-tl-unity-app.c` and `husk-tl-egl.c` both pass with only the pre-existing `_DARWIN_C_SOURCE` redefinition
warning. **No Mac, no iOS toolchain, no device: nothing below was run.** Everything is read from the log
`HuskLogs/husk-20261018-130848-3.0.0-129.log` or from the code, and is labelled as such.

## 0. What this step adds (and what it deliberately does not)

The brief asked for three things and, if the evidence did not name a cause, to say so and ship the instrument that
will. The evidence did **not** name the retrying function, so:

1. **The guest stack of the layer-started threads is now walked and printed at the stall** (§1). This is the whole
   point of the step: the pc could name only `__semwait_signal`; the walk names the CALLER.
2. **The "do failed `eglCreateContext` calls accumulate ANGLE workers?" question is now answerable from the next log**
   (§3) — the worker count is printed at every refusal — and the code answer is given here.
3. **The GLES-version behaviour of the shim is determined from the code and the log** (§4). It is NOT the retry loop,
   and I did **not** change it, because the evidence (the guest got its 3.0 context and drew) does not support a
   change. Saying so is the honest outcome, and §4 says why.

No always-on cost was added: the walk runs once per detected stall (only on the failure/cold path), and the worker
count runs only when an `eglCreateContext` **fails** (a handful of times per run). Nothing on a running path changed.

## 1. The guest-stack walk (`ss_guest_walk`, husk-tl-unity-app.c)

A host syscall pc names the kernel wait but not the caller, and the caller is the whole question — a thread parked in
`nanosleep` is either idling or inside a retry loop, and only the frames above the sleep tell the two apart. The guest
and the layer share the stack (a guest call into a shim pushes the guest's return address there), so the ARM64
frame-pointer chain from the thread's saved `x29` crosses the shim frames and reaches the guest frames.

At the stall, `ss_probe` now walks three threads and prints every frame:

- the **frame/render thread** (`GLThread` on this title);
- **`UnityMain`** — on this engine (the Java driver) it is the guest Unity engine thread that renamed itself from
  `Thread-1` (the log shows the same tid `0x702cf87000` as `Thread-1` at the first stall and as `UnityMain` at the
  second);
- the **Java driver's app/main+looper thread** (`main`).

Each line is `frame N fp <addr> -> lr [<image> <symbol>+<off>]`, resolved by the existing `ss_where` (guest image +
nearest symbol / the JIT arena / a NAMED host frame). The **first** line is the return address out of the function
whose pc was sampled — i.e. `nanosleep`'s caller — which is exactly the function that must be named.

Correctness-by-construction, stated as such:

- Links are read with `vm_read_overwrite` (`mach/vm_map.h`), which returns an error instead of raising a fault, so a
  bad link cannot crash the process the probe is diagnosing. `mach/mach_vm.h` is not in the SDK this gate compiles
  against (`fatal error: 'mach/mach_vm.h' file not found`), which is why the `vm_read_overwrite` form is used.
- An unreadable/unaligned link, a null return address, or a chain that does not climb stops the walk and prints which
  happened; a frame is never invented. A break is reported as "the code above omitted its frame pointer there", never
  as "idle".
- ARM64e frame records can carry a pointer-auth code in the top bits; a link whose raw value resolves in no image is
  retried with the top 16 bits cleared (`ss_strip_auth`), and the raw value is kept when neither form resolves. Guest
  Android arm64 carries no auth, so a plain pointer is returned unchanged.
- Only these three threads are walked, so the log stays bounded; the 32-thread idle pool has nothing to add.

## 2. What the log already narrows (guest stacks of the exact threads this step prints)

The counters `perf[hb]` print are bumped **only inside the bionic shims** — i.e. only when GUEST code calls them
(`b_gettimeofday`, `b_mutex_lock`/`b_mutex_unlock`, `b_cond_wait`, `b_clock_gettime` in husk-tl-bionic*.c). That makes
the hot-path counters a guest-code profile, and it names the shape of the loop without needing the walk:

| counter (guest shim calls) | first stall | later | reading |
|---|---|---|---|
| `gettimeofday` | 1,166 | 2,411 → 3,654 → 4,897 → 6,890 → 9,714 | climbs ~1,200–2,800 per heartbeat |
| `mutex_lock` / `mutex_unlock` | 91,768 | 93,352 → 94,933 → 96,528 → 99,057 → 102,650 | climbs ~1,300–2,700 per heartbeat |
| `clock_gettime` | 33 | 33 (flat) | the layer's own render loop is NOT looping |
| `cond_wait` | 29 | 29 (flat) | the loop waits on NO condition variable |
| `read`, `open`, `fstat`, `mmap` | flat | flat | no I/O in the loop |

So: a **guest** thread is spinning on `gettimeofday` + `pthread_mutex_lock`/`unlock`, with no condition wait and no
I/O, while frames stay at 8. `GLThread`'s own frame loop paces on the **host** `clock_gettime` (`now_ns()` in
`husk-tl-dvm-android.c`), which is not counted — so a flat `clock_gettime` does **not** clear GLThread, but the
`nanosleep` at `husk-tl-dvm-android.c:1028` is that loop's frame pacer and is the benign reading of GLThread's sleep.
`UnityMain` is the guest thread whose sleep can only be the loop's (it is the only layer-started guest engine thread),
and its `nanosleep` is where the sampler catches it.

This is a narrowing, not a naming: the log says the loop is guest code using `gettimeofday` + a mutex, but it cannot
say WHICH guest function, because the pc is inside `nanosleep`. **That is precisely what §1's walk prints**, and the
cause is therefore still unnamed in this step — stated plainly, per the brief.

Two further data points from the same log, recorded because they bear on "why 8 frames":

- `unity: alive: 8 frames ... on screen: 0 frame(s) composited` throughout. The 8 frames are `tl_javaapp_frames()`
  (`A.frames`, incremented by the Java driver's own compositor swap). The app's OWN window surface (`egl: window
  1748x804 -> surface 0x2, composited under the app's views`) never composited one — `tl_egl_composited_frames()` is
  0. So the layer's compositor presented 8 of its own frames and then stopped, while the title never presented its
  first real frame. This is consistent with the report (black "starting" screen) but is an observation, not a
  mechanism.
- The final close is loud and expected: `W/Unity: Timeout (2000 ms) while trying to pause the Unity Engine`, then the
  session close quarantines the 32 guest threads. Nothing in the close names the cause either.

## 3. Do failed `eglCreateContext` calls accumulate ANGLE workers?

**Code answer: the shim starts no thread of its own.** `w_eglCreateContext` (husk-tl-egl.c) only patches the attribute
list (see §4) and forwards to ANGLE's `eglCreateContext`, returning its result. There is no worker/thread creation
anywhere in husk-tl-egl.c (`grep -n 'pthread_create' husk-tl-egl.c` → none), so a worker left behind by a failed
creation would be ANGLE's, not the shim's, and cannot be fixed here — it is in a prebuilt framework this repo does not
build.

**Log answer, and why it does not support accumulation.** The log this step reads has *only this sampler's* log lines
for threads, and only this one build printed them — the earlier (working) runs, `…121707`/`…121906`, are older builds
with no `stall-host-threads:` lines at all, so the working run's worker count **cannot** be compared. What the log
does contain: exactly **two** failed `eglCreateContext` calls (ES 3.2, then ES 3.1), followed by a silent success at
ES 3.0 (`gl: GL_VERSION = OpenGL ES 3.0`, a working renderer, 8 frames). Twenty-one workers cannot be 7-per-attempt
from three attempts, and there is no repeated-attempt sequence in the log to accumulate over. So the plausible reading
is ANGLE's worker pool sized once at init, **not** the pile-up the brief hypothesised.

**What now proves it either way:** at every refusal the shim prints the live `ANGLE-Worker` count
(`angle_worker_count()`, `pthread_getname_np` over `task_threads`, nothing suspended/ended/joined; `-1` when the
thread list cannot be read, printed as "not countable", never as 0). If the count climbs across consecutive
refusals, a refused creation is leaking a worker and it is a real (ANGLE-side) bug; if it is the same number each
time, they were made once. This is the one change in `husk-tl-egl.c` beyond the include.

## 4. What the shim does when the guest asks for a GLES version it cannot give

Determined from the code and confirmed by the log:

- `w_eglCreateContext` patches `EGL_CONTEXT_MINOR_VERSION` **down to 0** — but **only when `g_es31_shim`** is set.
  `g_es31_shim` is armed by `tl_egl_es3_claim(minor)` (husk-tl-dvm-android.c:1180) for an app whose **manifest claims
  ES 3.1+**, or by `tl_egl_es31_shim(true)` for GameActivity/NativeActivity. It is otherwise **false**.
- For this title the downgrade is **not armed**: no `egl: the manifest asks for OpenGL ES 3.x` line appears in any log
  (grep: zero), and the failure lines print `0x30fb=2` then `0x30fb=1` — the *unmodified* requested minors (the log
  prints the attribute list actually passed to ANGLE, and a patched one would read `0x30fb=0`).
- So the shim **passed ES 3.2 and ES 3.1 straight through to ANGLE over Metal, which refuses anything above 3.0**,
  and returned ANGLE's NULL to the guest. Unity then created an ES 3.0 context (silent, because the log line only
  prints on failure or with `TL_EGL_TRACE`), and `glGetString` reported `OpenGL ES 3.0`.

So the answer to "does it return failure, or hand back the best available context?": **it returns failure for ES 3.2
and 3.1 unless the app's manifest claimed ES 3.1+, in which case it silently downgrades to 3.0 instead.** Both
behaviours are now logged with the requested minor and the reason, so the next log cannot be misread.

**Was this the retry loop? The evidence says no, so I did not change it.** The guest's ES 3.2/3.1 attempts failed and
it recovered on its own: it got a working 3.0 context (`GL_VERSION = OpenGL ES 3.0`, a renderer, a window surface,
8 frames presented by the layer's compositor). A "make the shim hand back the best available context" change would
therefore alter nothing about this freeze; adding it would be papering over an unproven cause, which the brief
forbids. It is recorded here as a latent robustness question (the shim could, on a failed create, retry once at minor
0 and return ANGLE's 3.0 context, exactly as it already does when `g_es31_shim` is armed), to be done only if a later
log shows a title whose retry loop is *enterable only through a refused create*.

## 5. The fix

**No behavioural fix was made, because no cause was proven.** The named deliverable — the function retrying — is not
yet named; §2 narrows it to "guest code spinning on `gettimeofday` + a guest mutex, with no condition wait", and §1
ships the walk that prints exactly which guest function that is on the next stall. Changing EGL version behaviour or
ANGLE's workers without that proof would be the "paper over it" the brief warns against.

## 6. The next log will distinguish

- `stall-guest-stack:` for `GLThread`, `UnityMain` and `main`, with `frame 0` = the **caller** of `nanosleep` /
  `__savewait_signal` (i.e. the guest function retrying, or the layer's frame pacer for GLThread — the walk resolves
  each to an image+symbol, so the two are told apart on sight).
- whether a `Husk`-image frame or a `/var/mobile/.../Translat … +0x…` (libil2cpp/Unity) frame sits directly under
  `nanosleep`.
- `ANGLE-Worker threads alive at this refusal: N` for each refusal — flat N means no accumulation, climbing N proves
  a leak.
- `egl: eglCreateContext(... minor asked=…)` — which minor each refusal actually asked for, and whether the shim's
  downgrade was armed.

## 7. What could not be verified

- **No run.** No Mac, no iOS toolchain, no device. Only the per-file zig compile gate was exercised. Every behavioural
  claim is read from the log or reasoned from the code.
- **The fp walk on a real device is unverified.** That the Android guest and the layer keep frame pointers through the
  shim path (so the chain reaches the guest) is a construction, not a measurement; `libsystem_*` is arm64e and may not
  keep a frame pointer, so the first link may break — which the code reports as a break, not as a frame. If the walk is
  short on the device, the fallback is the thread's `sp` (already printed) for a DWARF-based walk, which this step does
  not add.
- **That the ARM64e auth-code strip recovers the link in every case is not verified** (no arm64e device here); a wrong
  strip is not possible (the raw value is kept unless the stripped one resolves), but a *missed* strip would print hex.
- **The ANGLE worker count cannot be cross-checked against the working run** (that run's build had no sampler). The
  21-on-one-condvar figure is only comparable to future logs of this build.
- **`GLThread`'s `nanosleep` being the benign frame pacer** is a reading of `husk-tl-dvm-android.c:1028` plus the flat
  guest `clock_gettime`; the walk, not this note, is what proves it.
- **Which guest function calls `gettimeofday` and the mutex thousands of times per second is not named** — that is the
  open question this step instruments for.
- **`unity: alive: 8 frames … on screen: 0 frame(s) composited`** (the layer's compositor present, the app's own
  surface never) is an observation from one build; the older/working logs predate that counter and cannot confirm it.
