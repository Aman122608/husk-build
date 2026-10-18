# apk-precheck — offline, phone-free APK compatibility pre-check

`tools/apk-precheck.py` answers one question before anyone spends a phone test
on a game: **what will Husk do with this APK, and if it will fail, which known
failure class is it in?**

It reads only the APK's bytes on this machine. No phone, no boot, no network,
no build. Standard library only — the native-library import scan uses a small
inline ELF64/ELF32 parser, so there is no capstone / pyelftools dependency for
testers to install.

## Run

```sh
# one file, a folder, or a glob — repeatable
python tools/apk-precheck.py Games/
python tools/apk-precheck.py "Games/*.apk" "C:/path/to/Other.apk"

# machine-readable report (default ./precheck-report.json)
python tools/apk-precheck.py Games/ --json report.json

# fast mode: skip reading native libraries for imports
python tools/apk-precheck.py Games/ --no-imports

# libraries larger than 256 MiB are skipped for imports by default
python tools/apk-precheck.py Games/ --max-lib-mb 384
```

It prints a summary table then a per-APK section, and always writes a JSON
report (`results[]` holds every field the table shows, plus the evidence list).
Re-running on a folder is the intended workflow: keep the JSON and diff it.

## Verdict vocabulary

Exactly one verdict per APK:

| verdict | meaning |
|---|---|
| `runs-native` | engine Husk has a native driver for, arm64 present, content present |
| `runs-java` | runs on Husk's Java runtime (Java-only app, or an engine routed to Java) |
| `needs-content` | content is not in the APK: side-car OBB / first-launch download required |
| `needs-google` | Java-only app that references Play services the layer does not provide |
| `unsupported-32bit` | ships native code, but none for `arm64-v8a` |
| `unsupported-play-asset-packs` | Play Asset Delivery shape — unsatisfiable for a sideloaded build |
| `unsupported-split-missing` | base APK of a split set whose sibling splits are absent |
| `likely-missing-imports` | native lib imports a symbol the layer binds to a logging stub |
| `unknown` | cannot be classified from the APK alone — the report says what file would settle it |

## What it checks, and where each rule comes from

- **ABIs** — every `lib/<abi>/*.so`, against the scanner's ABI list
  (`src/translation-layer/husk-tl-scan.c`). No `arm64-v8a` library is the hard
  gate (`src/app/Husk/TranslationLayer.swift:57`) → `unsupported-32bit`.
- **Engine** — recognised by the libraries an engine always ships
  (`husk-tl-scan.c:note_engine`): Unity IL2CPP, Unreal (`libUE4.so`), cocos2d-x,
  Godot, GameMaker, SDL, Flutter, Ren'Py (`librenpython.so`), EA/PopCap PvZ2
  (`libPVZ2.so`), Rockstar (`libGame.so` + OpenAL + mpg123), Minecraft,
  libGDX, React Native, and plain Java/Dalvik (no engine library). Unity 6 is
  the marker `UnityPlayerForActivityOrService` inside `libunity.so` → routed to
  the **Java** runtime (`husk-tl-scan.c:328-329`, `TranslationLayer.swift:61`).
  Web frameworks are file markers (`assets/capacitor.config.json`,
  `assets/www/cordova.js`).
- **Content shape** — in-APK roots (`assets/private.mp3`, `assets/x-game/`,
  `assets/x-renpy/`, `assets/bin/Data/`) vs. absence, which means a side-car
  OBB or a first-launch download. The Play-asset-pack shape is a
  `GooglePAD-Manifest.txt` inside, or the `com/google/android/play/core/assetpacks`
  / `GooglePAD` marker inside a native library (as in GTA SA DE's `libUE4.so`).
- **Splits** — `isSplitRequired` / `com.android.vending.splits.required` in the
  manifest string pool (the layer has `husk_apk_split_required`).
- **Java-only apps** — entry activity (MAIN+LAUNCHER, parsed from the binary
  `AndroidManifest.xml`), Play services (`com/google/android/gms` in dex), and
  an engine class named in the manifest whose library is not shipped.
- **Unresolved imports** — each `lib/arm64-v8a/*.so`'s dynamic symbol table,
  listed against the set the layer has been seen to leave unbound and bind to a
  logging stub: `swscanf`, `getgrgid`, `getgrnam`, `pthread_attr_setstack`,
  `gzopen`, `gzread`, `gzclose` (`src/translation-layer-next/husk-tl-ld.c`).

## Evidence

Every verdict carries an `evidence` list naming the exact file name, string, or
absence it came from — so a wrong verdict is arguable rather than mysterious.
The JSON `results[]` keeps the same list under `evidence`, plus per-library
`libraries[]` (name, bytes, dynamic-import count, DT_NEEDED) and
`system_libraries` (DT_NEEDED names the app does not ship).

## What it cannot know

- Whether a side-car OBB the user must supply is actually present next to the
  APK — it only sees the APK's own contents.
- A first-launch download that happens only after the app starts.
- The exact entry activity of a heavily obfuscated manifest (best-effort).

When one of those is the deciding factor the verdict is `needs-content` or
`unknown`, and the report says which file would settle it.
