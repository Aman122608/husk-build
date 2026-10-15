# robot gf: the crash-scoping rule was too narrow, and the SIGBUS at 0x122a51b90

Run: `HuskLogs/husk-20261015-101057-2.0.0-112.log` (build 2.0.0 (112) `6d11432-dirty`), game
`com.IncontinentCell.MyDystopianRobotGirlfriend` (Unity 6000.3.8f1, IL2CPP, arm64).
Two jobs: widen the guest-fault scoping so Husk survives the app's fault (A), and establish what the
new SIGBUS actually is (B). No compiler/Mac here; CI (`build-ipa-prebuilt.yml`) is the only compile, so
brace/paren/guard balance was checked by hand (counts unchanged vs HEAD for `{}`; balanced `()`).

---

## Task A — the scoping test is now the FAULT ADDRESS, not the pc

### What was wrong

`tl_xmem_arena_fetch_fault(pc, addr)` (husk-tl-xmem.c:1196) returned true only for
`g_x.open && pc == addr && tl_xmem_contains(pc)` -- a *fetch* whose pc is the fault address and is
inside the region. This run's fatal fault is the other shape:

```
:613  === FATAL signal 10 on thread 'UnityMain': fault address 0x122a51b90
:614      pc .../Documents/Data/App+0x4c5e8e4      <- the guest's il2cpp, NOT the region
:615      lr .../Documents/Data/App+0x4c5e804
:616  husk: fetch fault at 0x122a51b90 (executable view): entry 0x122a50000..0x122a54000 prot 5 max 7
```

`pc != addr` (compute: `0x122a51b90 - 0x4c5e8e4 = 0x11DF63…` is not 16 KiB-aligned, so the two are
different addresses), and the fault address `0x122a51b90` **is** inside the region
(`0x1174c8000..0x1374c8000`, offset `+0xb589b90` = +181.5 MiB). So the old test read **false**,
`on_fatal` re-raised to the previous handler / default action, and the process died. The next launch
carries the crash header `What : Husk closed while the game was running` and there is **no**
`husk: GUEST FAULT:` line — the app's failure took the host with it, which is the one thing the rule
exists to prevent.

### The new rule (landed)

`src/translation-layer-next/husk-tl-xmem.c`:

```c
bool tl_xmem_arena_fetch_fault(uintptr_t pc, uintptr_t addr)
{
    (void)pc;   /* the pc names the instruction; the ADDRESS names whose memory it was */
    return g_x.open && tl_xmem_contains((const void *)addr);
}
```

A fault whose **address** lies in the region is the guest's, whichever thread's pc reported it. The
refusal is one-sided and stays: a fault whose address is **outside** the region is still a host crash
and keeps the old behaviour exactly. `pc` is kept only so the caller can name the faulting
instruction in the log.

Why the address and not the pc (the honest reason): a fetch fault reports `pc == fault address`, but a
**data** fault reports the pc of the instruction that did the load/store, and that instruction is in
the guest's own library, **outside** the region. The region is guest memory regardless — the loader
emits every library image into it (`husk-tl-ld.c:1978-1997`: `tl_xmem_alloc` + `memset` of the whole
`npages+nstub` allocation + the PT_LOAD copies) and the JIT arena is carved from the same bump pointer
(`tl_xmem_alloc`, husk-tl-xmem.c:568-581), so every address the kernel places inside it belongs to the
guest.

### Comment/consumer updates (landed)

- `husk-tl-xmem.c:1172-1195` — the function's comment now states the address rule, cites this run's
  two lines, and says why the pc-based test was wrong.
- `husk-tl-xmem.h:191-203` — the declaration's contract updated to match.
- `husk-tl-unity-app.c` — `guest_arena_fetch()` renamed `guest_arena_fault()` (:198), its comment
  (:174-197) rewritten to name both shapes (fetch and data access), and `why` is now shape-specific:
  the fetch message is unchanged; the data case says *"the app's own code at <lib+off> touched <addr>
  inside the guest arena and the page would not be served …"* instead of the wrong "branched to".
  The `on_fatal` comment block (:256-269) says a load/store the device will not serve is a guest fault
  too, and that the layer unmaps nothing.

### The unwind path is unchanged and still correct (verified)

For the guest-fault case `on_fatal` runs exactly (:272-288):

```
tl_xmem_note_guest_fault(gf, si_addr);        /* one honest line into the verdict container */
tl_log_line("husk: GUEST FAULT: ...");        /* one honest line into the log */
if (!tl_guest_exit_close_in_flight()) { CAS state -> HUSK_UNITY_FAILED (never clobbers IDLE/STOPPING) }
tl_javaapp_request_exit();                    /* = atomic_store(&A.ended, true), nothing else */
tl_javaapp_thread_leaving();                  /* sets a done flag; "never joined, cancelled or signalled" */
pthread_exit(NULL);                           /* ends ONLY the calling thread */
```

This is the same four steps as the guest-exit family (`tl_guest_exit_unwind`, husk-tl-bionic.c:448-454:
`request_exit` → `thread_leaving` → hook → `pthread_exit`), and the design explicitly forbids
`pthread_kill`/`pthread_cancel` on a thread that ran guest code (husk-tl-bionic.c:443-446;
`tl_javaapp_thread_leaving` doc, husk-tl-dvm-android.c:1219-1222). Nothing is unmapped on this path
(only `tl_xmem_alloc` ever unmaps, and it does so on a failed *open*, not here). The app is marked
FAILED, one line is logged, one reason is persisted, and Husk stays for the next game.

Note the one live caveat: this only fires if `g_x.open` is true (the arena is open). On this run it is
(log :151/:410 `arena 0x1174c8000..0x1374c8000: 512 MiB …`), so the widened test will fire next run.

---

## Task B — what the SIGBUS at 0x122a51b90 is

**Answer: shape (1). The page is mapped (entry `0x122a50000..0x122a54000` prot 5 max 7, i.e. R|X) but
has no backing at that offset, and a DATA access to it takes SIGBUS. It is not (2) an alias/PTE split,
and the layer-level fault is not (3) the guest's pointer arithmetic — that is at most the provenance of
the address, not the defect.**

### The evidence that discriminates

1. **It is a DATA access, not a fetch.** `pc = App+0x4c5e8e4`, `si_addr = 0x122a51b90`, and they are
   different addresses (:614, :613). So the CPU was executing the guest's il2cpp and doing a load (or
   store) to an address that happens to lie in the region. The codewrite JIT guard only carries
   *stores* decoded from the pc and only inside the region (husk-tl-codewrite.c:236-241); a load is
   chained to the next handler, which is why the fault reached `on_fatal`.
2. **It is not a permission fault.** The entry names execute (`prot 5 max 7`, :616), the layer issues
   no `vm_protect` on this region (log :143/:402/:486; husk-tl-xmem.c `adopt_exec_view`, no
   protection-changing call), and the walk reads `512 MiB mapped, 512 MiB executable, 0 NOT executable`
   (:150/:409). Execute is present; the page still did not serve the access. Signal 10 (SIGBUS), not a
   protection SIGSEGV, is consistent with a valid mapping with no backed page behind it.
3. **It is not (2) — the RX alias and the writable twin are one set of physical pages, measured, not
   assumed.** The fault came through the RX view (`0x122a51b90` is in `g_x.rx`; its twin would be
   `0x142a51b90`). The layer's whole store-carry exists on the premise that the two views are the same
   pages, and that is proven at boot (husk-ios-jit.c:256-267: "RX alias reflects RW writes -- aliasing
   OK") and again in this run for a guest store (:290: "*the guest fetches the byte it wrote at
   0x11eadc000*"). If a page were backed through the twin, a read through the RX alias would be served.
   So the missing thing is a **backing page in the shared object**, not a differing PTE.
4. **The +0x1b90 point rules out the naive "beyond the entry" story.** The entry is exactly one 16 KiB
   page (`0x122a50000..0x122a54000`), and the address is only `+0x1b90` into it — well *inside* the
   entry, not past its end. So the backing granularity is the 16 KiB entry/page and **that whole entry
   has no backing**; nothing about "the access ran off the end" applies.
5. **Why an entry can name R+X and still not serve a load: the backing is per-16-KiB-page and is put
   there by the debugger's own sweep, not by the mapping.** StikDebug allocates the region with
   debugserver `_M<size>,rx` and then "walks every 16 KiB page with `M<addr>,1:69`" — and that walk is
   what *prepares* the pages: "Requesting a fresh region (x0 == 0) is the only branch that gets those
   pages prepared; handing in an address we allocated ourselves does not" (husk-ios-jit.c:534-543).
   The repo's own record says the same and names the failure: robot gf "faulted on a page whose entry
   NAMED execute (prot 5) but **was not backed**… The old walk stopped at 128 entries, which is exactly
   why a 512 MiB region could read as '2 MiB mapped' and hide an unbacked page"
   (husk-tl-xmem.c:825-827), and "robot gf (083442/083605): a fetch at a page whose entry NAMED execute
   (prot 5) still took a bus error — the page was not backed at all. That region is StikDebug's,
   reserved rather than populated" (husk-tl-xmem.c:339-341). The loader also has to compensate for it
   explicitly: "The region's pages are not guaranteed zero (StikDebug writes a byte into each)"
   (husk-tl-ld.c:1984). The prepared entry — "one prepared 16 KiB page per entry" (husk-tl-xmem.c:744)
   — "cannot be re-made in-process" (ibid.), and the layer must not `vm_protect` the region (that is the
   ask that revoked the grant). So a 16 KiB entry whose per-page preparation did not happen reads in
   the map as a valid R+X entry and faults on any data access.
6. **The layer fell back to exactly this region.** `xmem: no arena of the layer's own (this process
   cannot make executable memory for itself (Operation not permitted) …); falling back to StikDebug's
   region` (:141). This run uses StikDebug's reservation as the guest arena (`BreakGetJITMapping
   returned 0x1174c8000`, `[husk-jit] #1`, :40). That is the region the file already documents as
   "reserved rather than populated, and only a few MiB of it is ever mapped" (husk-tl-xmem.c:340-341),
   and it is why the layer *prefers* `grant_own_arena()` (husk-tl-xmem.c:328-354) — which this device
   refused.

### What (3) is and is not

The guest computed the pointer (an `ldr`/`str` through `x9 = 0x122a51b90`, with `x8 = 0x122a51a68`,
`x20 = 0x122a51000` — a structure walk inside the region's page, :617-624), so the *provenance* of the
address may well be the guest's own logic. But the SIGBUS is not the guest doing something illegal: the
address is inside the memory the layer promised the guest, in a map entry the kernel reports as mapped
and executable. The defect is at the region level (a region that is reserved, not backed), which is the
layer's choice of arena — not the guest's arithmetic. Saying "the guest's own bug, no layer defect"
would be wrong here precisely because the address is in the region, in a valid-looking entry.

### Is the missing `PlayerPrefs.pp` the trigger? No.

The three lines are the game's own save-container probe, not a layer path:

```
:355  W/Unity: Failed to find the main file "PlayerPrefs.pp"!
:357  W/Unity: Failed to find the backup file "PlayerPrefs.pp"!
:359  W/Unity: Failed loading PlayerPrefs!
:362  W/Unity: Called InitializeWithOnValueChanged, but OnValueChanged is null! Key: VerData Type: String!
```

- **Where it lives:** the game's own files dir. `git`-hidden prior finding:
  `.hermes/plans/engine-reset/unity-path-and-counter-findings.md:26` records a *successful* save from
  the same game: `I/Unity: Saved "PlayerPrefs.pp" in .../files/Saves`, and (:76-78) "`PlayerPrefs.pp`
  → `.../files/Saves` … wrote successfully". So the file is `<files>/Saves/PlayerPrefs.pp`, and the
  "not found" pair is the game's normal **first-run** behaviour (its sibling `save.mdrg` not-found pair
  is already classified there as "normal first-run behaviour, not a consequence of this error").
- **Where the layer lands that path:** the guest's `/data/data/com.IncontinentCell.MyDystopianRobotGirlfriend/files/...`
  is resolved by `tl_path_resolve()` to `tl_data_dir() + rest` (husk-tl-bionic-io.c:268-278, and :287-292),
  and `getFilesDir()`/`Context_getFilesDir` hand the guest `H.files` under the same data dir
  (husk-tl-jni-hle.c:133; `ContextImpl.getFilesDir()` = `dir("files")`, java-framework `ContextImpl.java:81-82`).
  So the game's expected path is exactly the path the layer serves — nothing is misplaced.
- **The OBB / Shared-Storage path is not involved:** the OBB/shared-storage mapping covers `/sdcard` and
  `Android/obb/...` (husk-tl-bionic-io.c:198-215, :305-315). `PlayerPrefs.pp` is under the app's own
  files dir, not shared storage, so nothing about the OBB mapping should (or does) land it there.
- **The `jar:file:`/content-read path is not involved:** that machinery serves entries *inside the APK*
  (`jar:file://…!/assets/…`, husk-tl-bionic-io.c:523-556, :570-581); a prefs file in the files dir is a
  plain file, not an APK entry, so it is read through the ordinary `open`/`read` path.
- **Timing:** the warnings are at `:355-362`; the SIGBUS is at `:613`, ~12 s and four heartbeats later
  (`unity: alive: 125 frames` ×5). Nothing wires the prefs load to the arena access.

So `PlayerPrefs.pp` is a benign first-run warning (the game creates the container and would save it on
the next write). It is **not** the trigger of the SIGBUS.

### The single next instrumentation (Task B; no code changed here — the evidence is strong but not conclusive on one point)

The one thing this log cannot yet say is **whether the unbacked page at `+0xb589b90` is inside the span
the loader emitted (a page the layer should have backed) or in region the emitter never touched (an
address only the guest's pointer ever named)**. That is the difference between "the loader's owned
span lost its backing" and "the guest pointed into never-written region"; both are the region being
reserved, but the fix differs. Instrument it cheaply on the fatal path, in `fault_address_region`
(husk-tl-unity-app.c:150-172), for the case `tl_xmem_contains(si_addr)`:

```c
/* one extra line: is the faulting address inside the span the emitter/allocator owns? */
tl_log_line("husk: arena fault offset +%#lx from the arena base; the bump allocator has handed out %zu MiB "
            "(so a page below that IS the layer's to back); %s",
            (unsigned long)((uintptr_t)si_addr - (uintptr_t)tl_xmem_region_base()),
            tl_xmem_used() >> 20,
            ((uintptr_t)si_addr - base) < tl_xmem_used() ? "inside the emitted span" : "beyond it -- nothing ever wrote this page");
```

(that needs a tiny one-line accessor for `g_x.rx`/`g_x.used`; `tl_xmem_used()`/`tl_xmem_size()` already
exist, husk-tl-xmem.h:174). One run then names the fix:

- **inside the emitted span** → the loader's owned pages must be backed explicitly. The loader already
  `memset`s every image through the twin (husk-tl-ld.c:1986), so this would mean the *allocator's* tail
  (stub pages, the JIT arena) is not, and the fix is to back the whole `tl_xmem_alloc` span once, at
  alloc time, through the writable twin.
- **beyond it** → the region's reserved-but-unbacked nature is the whole cause and no loader change
  helps; the fix is to refuse StikDebug's region as the guest arena when its backing cannot be proven
  (i.e. make the `grant_own_arena()` preference a requirement for this engine rather than a preference),
  or to have `husk_ios_jit_prewarm` verify the per-page `M<addr>,1:69` sweep actually happened and
  report it, so the layer stops handing the guest a region the debugger only reserved.

Either way the widened Task A rule is what makes the next run *survive* long enough to print this line
instead of dying with the app.

---

## Anchors

| item | file:line |
| --- | --- |
| narrow test (fixed) | `husk-tl-xmem.c:1196` (was `pc == addr && contains(pc)`) |
| new rule | `husk-tl-xmem.c:1196-1199` (`contains(addr)`) |
| region membership | `husk-tl-xmem.c:598-604` (`tl_xmem_contains`) |
| loader emit + memset (backing the layer owns) | `husk-tl-ld.c:1978-1997` |
| bump allocator | `husk-tl-xmem.c:568-581` |
| "not backed" record | `husk-tl-xmem.c:339-341`, `:825-827`, `:744` |
| debugger page preparation | `husk-ios-jit.c:534-543` |
| own-arena preference / this run's fallback | `husk-tl-xmem.c:328-354`; log `:141` |
| store-carry proves the aliases share pages | log `:290`; `husk-ios-jit.c:256-267` |
| codewrite guard (stores only) | `husk-tl-codewrite.c:230-241` |
| guest-fault unwind | `husk-tl-unity-app.c:272-289` |
| guest-exit unwind (same shape) | `husk-tl-bionic.c:448-454` |
| no pthread_kill/cancel/join | `husk-tl-bionic.c:443-446`; `husk-tl-dvm-android.c:1219-1222` |
| PlayerPrefs.pp is the game's own save | `.hermes/plans/engine-reset/unity-path-and-counter-findings.md:26,76-78` |
| files-dir path mapping | `husk-tl-bionic-io.c:268-292`; `husk-tl-jni-hle.c:133` |
| jar:file: is APK-only | `husk-tl-bionic-io.c:523-556` |
| /sdcard + OBB mapping | `husk-tl-bionic-io.c:198-215`, `:305-315` |

---

## The instrumentation landed (Task B)

`fault_address_region` (husk-tl-unity-app.c) now calls a new static `arena_page_span(addr)` after each of
its existing report lines, and `tl_xmem_region_base()` (husk-tl-xmem.c, declared in husk-tl-xmem.h) is a
one-line read-only accessor for the executable view's base `g_x.rx` -- the base the bump allocator's span
is measured from, so the offset and `tl_xmem_used()` are in the same units. On the fatal path when
`tl_xmem_contains(si_addr)` is true the next run prints, in addition to the unchanged "fetch fault at
… entry …" line:

```
husk: arena page backing: fault offset +%#lx from the arena base %p; the bump allocator has handed out %zu KiB of %zu KiB (used=%zu size=%zu) -> INSIDE the emitted span … | BEYOND the emitted span …
```

Filled in for `robot gf`'s known fault (offset `+0xb589b90` `= +181.5 MiB` from base `0x1174c8000`), the
line reads for example `fault offset +0xb589b90 from the arena base 0x1174c8000; the bump allocator has
handed out … KiB of 524288 KiB (used=… size=536870912) -> INSIDE the emitted span …` when the loader had
emitted past that page, or `-> BEYOND the emitted span …` when it had not.

Reading `offset < used` (the line says **INSIDE the emitted span**) means the page the guest touched was handed
out by the loader/allocator with its own bump pointer, so the layer owns it and is obliged to back it:
the fix is to back the WHOLE `tl_xmem_alloc` span once, at alloc time, through the writable twin (the
loader already `memset`s every image through the twin; the allocator's tail -- stub pages, the JIT arena
-- is what is left unbacked). Reading `offset >= used` (**BEYOND the emitted span**) means nothing the
loader wrote ever touched that page, so the region's reserved-but-unbacked nature is the whole cause and
no loader change helps: the fix is to refuse StikDebug's region as the guest arena when its backing
cannot be proven (make `grant_own_arena()` a requirement rather than a preference) or to have
`husk_ios_jit_prewarm` verify the per-page `M<addr>,1:69` sweep actually ran. Either way the widened
fault rule (Task A) is what lets the run survive to print this line instead of dying with the app. No
guard relaxed, nothing unmapped, no thread ended, the honest refusal unchanged.

---

## The fix landed: the allocator backs the span it hands out

The measurement above decided it: `offset +0xb589b90` is **INSIDE** the emitted span (fault address
190,356,368 < the allocator's high-water mark 193,953,792, by ~3.6 MB), so the page the guest touched
was handed out by the layer's own bump pointer and the layer is obliged to back it. The fault is a DATA
access (SIGBUS, not a protection SIGSEGV) into entry `0x1217b8000..0x1217bc000` reported `prot 5 max 7`
-- the entry NAMES EXECUTE and the page still had no backing -- which is the shape `husk-tl-xmem.c`'s own
record already named (`:339-341`, `:825-827`). The backing is created by a WRITE (the loader `memset`s
every image through the writable twin, and that store is what makes the kernel publish the page); the
bump allocator's tail -- stub pages, the JIT arena, any span handed out but never written -- was therefore
handed out mapped-but-unbacked.

`src/translation-layer-next/husk-tl-xmem.c`, `tl_xmem_alloc` (the single choke point for every arena
allocation -- the loader's library/JIT allocs, `husk-tl-ld.c`; the bionic-io and egl page allocs): after
the bump pointer advances, one byte is written per 4 KiB page across the span **this call** handed out,
through the WRITABLE view (`xmem_writable_view()`, never the executable view), forcing the kernel to back
each page before any caller can touch it. A store, not a full `memset`: the span is fresh and the caller
that keeps content writes it before use. Nothing in the whole 512 MiB reservation is touched beyond the
handed-out span, and `tl_xmem_used()`/`tl_xmem_size()` remain the only accounting -- no second counter is
invented. **No `vm_protect` is issued on the region** (the ask for RWX is what revoked the debugger's
per-entry execute grant -- see `adopt_exec_view`), nothing is unmapped, no thread is ended, no guard is
relaxed. The executable view is never written: every store goes through the writable twin.

The log line, once per backing pass, at the layer's usual level:

```
xmem: arena page backing: backed the span just handed out %p..%p (%zu bytes, %zu pages) -- one byte per 4 KiB page written through the writable view %p, so a later guest DATA access to these pages cannot SIGBUS on an unbacked page (no vm_protect, nothing unmapped)
```

Hand-checked against HEAD (no compiler here): `{}` counts 123/123 vs HEAD 122/122 (the one added block is
balanced), `()` balanced, the arena walk still reads `512 MiB mapped, 512 MiB executable, 0 NOT
executable`, the EXEC GATE pair and the guarded write-then-execute are untouched, and no path writes the
executable view.

---

## The backing was a DEAD STORE -- elided in Release -- so it is now volatile and proven by read-back

The fix above did not stop the fault. In `husk-20261015-113644` the backing pass ran and logged its
lines (39 of them, up to the multi-MB library images), yet the guest faulted at the **identical**
offset: `FATAL signal 10 on thread UnityMain: fault address 0x124651b90`, entry
`0x1246518000..0x124651c000 prot 5 max 7` (names execute, still not backed), and the offset line read
the same allocator state as the run before the fix -- `the bump allocator has handed out 189408 KiB
of 524288 KiB (used=193953792 size=536870912) -> INSIDE the emitted span`, frames to 122.

The diagnosis is a classic one, and it makes the old log line *non-evidence*: the per-page touch was
`wv[off] = 0` -- a store of zero to memory that is only ever **written and never read**. That is a
**dead store**. In a Release build the optimizer is entitled to delete the entire loop body; the loop
still iterates, the log line still prints "backed", and *nothing is written* -- so no page is backed.
The line said the loop ran; it never said the pages were backed, and it could not.

### The fix: a store the compiler cannot remove, and a read-back that proves it

`src/translation-layer-next/husk-tl-xmem.c`, `tl_xmem_alloc`, same choke point and same span:

1. The write goes through a **`volatile unsigned char *`** lvalue. A volatile access is an observable
   side effect and cannot be removed or elided, so the store -- and the page backing it forces -- is
   real in any build.
2. Each page is then **read back** through the SAME writable view and its byte folded into a checksum
   (`checksum ^= got ^ page_index`). A read whose result is *used* cannot be optimized away either, so
   a page that was never backed now faults **here, in the layer**, where the fault is honest and named
   -- instead of later, inside the guest. If a page cannot be read back the line says so rather than
   claiming success.
3. The log line keeps start/end/bytes/pages/where-it-wrote and adds the checksum and the read-back
   verdict, so a reader can tell "the loop ran" from "the pages are really backed":

```
xmem: arena page backing: backed the span just handed out %p..%p (%zu bytes, %zu pages) -- one VOLATILE store per 4 KiB page through the writable view %p (volatile, so the compiler cannot delete it as a dead store), then %s; checksum 0x%016llx over %zu page(s), so the store is real and a later guest DATA access to these pages cannot SIGBUS on an unbacked page (no vm_protect, nothing unmapped)
```

   where `%s` is `read back OK -- every page re-read through the same writable view returned the value
   just written`, or `read back FAILED -- a page did not return the value just written, so it may not
   be backed`. Kept: no `vm_protect` on the region, the executable view is never written, nothing is
   unmapped, no thread is ended, no guard relaxed.

### What this makes diagnostic for the NEXT run

If the same page faults again after a proven touch **and** a read-back, it is a **different** fault and
must be treated as one: the page was demonstrably written and re-read through the writable view at
alloc time, so the cause is no longer "the layer never backed it". It is then one of: the mapping was
revoked **after** alloc (a later `vm_protect`, a debugger detach that cost the per-entry execute grant,
a jetsam/CoW event), the faulting address is in a different region than the one the allocator backed
(the offset line vs the printed span must be compared, not assumed), or the guest reached the page
through an address the writable view does not alias -- not a dead store. The offset line and the new
span line, read together, are what separate those cases.

### Separately: the stikjit:// probe and the csops line no longer flood the log

Same run, `src/app/Husk/JITBootstrap.swift`: `isStikDebugInstalled` asked `canOpenURL("stikjit://")`
on every view/body refresh and every resolver call, and each call emitted the system line
`-canOpenURL: failed for URL: "stikjit://" ...` (48 in the tail of this run alone); `isDebuggerAttached`
re-logged `csops status = 0x32003005, CS_DEBUGGED = set` on every poll (19 in the run). Both now
**probe once and cache**, and the csops line prints at most **once per state change**. Nothing about
what the probes DECIDE changed -- only how often they are repeated and logged.

Hand-checked against HEAD (no compiler here): the one added `{}` block in `tl_xmem_alloc` is balanced,
the `?:` and the call's `()` pair balance, `%llx` is fed an `(unsigned long long)` cast and `%zu` a
`size_t`, and the Swift changes add no new control-flow brackets beyond the `if` bodies they show.
