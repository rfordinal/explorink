# The simulator on a phone

Idea: build the simulator as an Android app, so the firmware UI runs on a phone
with no reader hardware in reach.

**Not an emulator.** The simulator is the firmware compiled as native code with
`lib/hal/` replaced by an SDL2 HAL (`platformio.ini`, `[env:simulator]`; the HAL
lives in the `explorink-simulator` fork). An ESP32-C3 `.bin` never enters it. So
an Android build is a third compile target of the same sources, and a new
firmware means a new APK.

Status: **it runs.** The firmware compiles, links, boots and draws the map on
three phones (Android 9, 12 and 16). The shell, the build scripts and the
findings live in the simulator fork's `ANDROID.md`; this file covers the
firmware side.

## Milestone 0: does the code compile for Android at all? Yes

Verified 2026-08-23, NDK r27.3.13750724, clang 18, target `aarch64-linux-android24`.

| | |
|---|---|
| translation units | 208 |
| compiled clean for aarch64, first run | 206 |
| real failures, first run | 2 |
| compiled clean after the fixes | 208 |

Compile-only, no link. It answers one question: does Android's C library
(bionic) plus the NDK's libc++ accept this codebase. It says nothing about
linking, SDL2, or the app shell.

Method: build the host simulator's compile database, then replay every command
with the NDK compiler and throw the object away.

```bash
pio run -e simulator -t compiledb        # writes compile_commands.json
python3 scripts/android_build.py --compile-only
```

`scripts/android_build.py` replays each entry with the NDK compiler. Without
`--compile-only` it keeps the objects and links them into `libmain.so`, writing
it straight into the simulator fork's `android/app/src/main/jniLibs/`.

`compiledb` covers project `src/`, `lib/`, and the `lib_deps` (the simulator's
own HAL sources included) -- 208 units, verified by listing the database.

### Real failure 1: libc++ has no floating-point `from_chars`

`lib/Epub/Epub/css/CssParser.cpp:112`, instantiated as `tryParseNumber<float>`
from `CssParser.cpp:287`:

```
error: call to deleted function 'from_chars'
note: candidate template ignored: requirement 'is_integral<float>::value' was not satisfied
```

libstdc++ implements `std::from_chars` for floating point; libc++ ships only the
integral overloads (NDK r27's
`sysroot/usr/include/c++/v1/__charconv/from_chars_integral.h:223`). Both the
ESP32 and the Linux simulator build use libstdc++, so this has never shown up.

**Fixed.** `tryParseNumber` now splits on `if constexpr
(std::is_floating_point_v<T>)`. The integral path is the old `from_chars` call,
untouched. The floating-point path copies into a 64-byte stack buffer and calls
`strtof`, rejecting on `ERANGE` or an unconsumed tail.

`strtof` accepts input `from_chars` refuses -- hex floats, `inf`, `nan`, leading
whitespace -- so a character whitelist (`0-9`, `.`, `-`, `+`, `e`, `E`) runs
first. On everything that gets past it, the two agree.

Two details worth keeping:

- **Locale.** `strtof` reads the decimal separator from the current locale;
  `from_chars` is always `.`. Nothing in `src/` or `lib/` calls `setlocale` or
  touches `std::locale`, so the C locale is in force and the separator is `.`,
  as CSS requires. Verified by grep, not by test.
- **Length.** Input of 64 characters or more is now rejected rather than
  parsed. A CSS length that long is not a real document.

Parity checked against the old implementation over 39 inputs (integers,
decimals, signs, leading `.`, trailing `.`, `1e3`, malformed `1.2.3` / `--1` /
`+-1`, empty, subnormal, overflow, 70 digits). 37 identical. The two that differ
are `inf` and `nan`: the old code accepted them as a CSS length, the new one
rejects them. Neither can reach the parser anyway -- `parseCssLength` cuts the
number at the first character outside `[0-9.+-]` (`CssParser.cpp:280-284`), so
`inf` arrives as an empty string and was already rejected one level up.

CSS parsing is the EPUB reader's, not the map's.

**Open:** that parity check was a throwaway host program comparing the two
implementations side by side, not a test in the tree. `CssParser` is not built
by any suite under `test/` -- it needs `Arduino.h`, `Logging.h` and `HalFile`
stubs that do not exist there yet. Until someone writes them, this parser has no
regression coverage at all, Android or not.

### Real failure 2: `std::min` with no `<algorithm>`

`src/activities/map/MapRenderer.cpp:80`:

```
error: no member named 'min' in namespace 'std'; did you mean 'fmin'?
```

The file includes `<cmath>` and `<cstdio>` only (`MapRenderer.cpp:2-3`).
libstdc++ leaks `std::min` in through its header graph; libc++ does not.

This is a latent defect independent of Android: the device build compiles only
by accident of which headers libstdc++ happens to pull in.

**Fixed:** `#include <algorithm>` (`MapRenderer.cpp:3`).

### Both builds still compile, and the device build got smaller

Verified 2026-08-23, both zero warnings:

| build | result |
|---|---|
| `pio run -e simulator` (g++/libstdc++) | SUCCESS |
| `pio run -e default` (ESP32-C3) | SUCCESS |

The device binary **shrank by 19.2 kB** of flash: 3,959,101 B before the fixes,
3,939,459 B after. Static RAM did not move (58,332 B both ways). Measured in one
worktree by reverting only the two source files and rebuilding incrementally, so
nothing else differs between the two numbers.

The saving is libstdc++'s floating-point `std::from_chars`, which carries a
full correctly-rounded decimal conversion. `strtof` is already linked in --
newlib's `printf` family needs it -- so dropping `from_chars` removes the
duplicate. Worth remembering as a general lever: `<charconv>` on floats is not
a cheap header on this target.

### Five failures that were the probe's fault, not the code's

The first run reported 7 failures. Five were missing host headers, and all five
were **fatal** errors, so the rest of those files went unchecked. Supplying the
headers cleared all five with zero errors.

| file(s) | fatal error | why |
|---|---|---|
| `HalDisplay.cpp`, `HalGPIO.cpp`, `simulator_main.cpp` | `'SDL2/_real_SDL_config.h' file not found` | Debian splits SDL's config into `/usr/include/x86_64-linux-gnu/SDL2/`, which an aarch64 target does not search. A real Android build uses Android SDL2 headers. |
| `KOReaderCredentialStore.cpp`, `KOReaderDocumentId.cpp` | `'openssl/md5.h' file not found` | No Android OpenSSL headers on this machine. |

Two traps worth writing down, because both look like real portability failures:

- **Do not add `-I/usr/include` to reach a host header.** It drags glibc into an
  Android build and dies on `gnu/stubs.h:7: 'gnu/stubs-32.h' file not found`.
  Copy the specific headers into a shim directory instead.
- **The host SDL config enables x86 SIMD.** Borrowing Debian's
  `_real_SDL_config.h` sets `HAVE_IMMINTRIN_H 1`, and clang then fails 19 times
  inside `immintrin.h` (`"This header is only meant to be used on x86 and x64
  architecture"`). Strip that define from the shim copy.

## OpenSSL is one MD5, and can go

`-lssl -lcrypto` in `[env:simulator]` has exactly one consumer: the fork's
`src/MD5Builder_linux.h:12`. Nothing else in the simulator includes `openssl/`.
A `MD5Builder_android.h` with a bundled MD5 removes the whole
"cross-compile OpenSSL for Android" problem. `src/MD5Builder_mac.h` is the
precedent -- same class over CommonCrypto.

Per the fork's `FORKING.md` this is the first kind of diff (platform emulation
gap), so it belongs upstream rather than staying in the fork.

## The Android build compiles the e-reader too, and that is why CSS was in the way

ExplorInk is a map device. The Android simulator exists for the map screens.
But the firmware is one binary with the inherited CrossPoint e-reader in it, and
nothing strips the reader out: `[env:slim]` only drops the serial log
(`platformio.ini`, `[env:slim]`), and no build flag gates `lib/Epub`. So an
Android build has to compile the reader's CSS parser whether or not anyone
intends to open an EPUB on a phone. That is the only reason the `from_chars` gap
above had to be fixed at all.

It follows that the CSS change does not gate this work, and an EPUB is the wrong
thing to check on hardware for it. The Android milestone is verified by the map
screens; the CSS parser is verified by opening an EPUB, whenever someone cares
to.

**A map-only build would be worth having on its own** -- smaller device flash,
and an Android build with no inherited reader surface in it. It is not a
`build_src_filter` line:

- `src/activities/ActivityManager.cpp:23` includes `reader/ReaderActivity.h`
  directly and constructs it at `:246`.
- `src/main.cpp:240` and `:568` branch on `activityManager.isReaderActivity()`
  for sleep bookkeeping and tilt page-turn.

So it needs compile-time gates through `ActivityManager` and `main.cpp`. How
much flash it would return is **unmeasured** -- object-file sizes carry debug
info and are not a proxy for it.

## Milestone 1: does it link? Yes, with nothing missing

Verified 2026-08-23. 208 objects into an 18 MB `libmain.so`.

The prediction was wrong in a useful way. The expectation was a pile of
undefined symbols -- everything the SDL HAL assumes a host provides. There were
none.

**A shared library link is not a test unless you ask it to be.** `-shared`
resolves nothing by default and leaves undefined symbols for `dlopen` to fail
on, on the phone, later. The link therefore runs with `-Wl,--no-undefined`, and
it still passes. Checked afterwards as well: all 229 undefined dynamic symbols
are versioned against `LIBC` and friends, `NEEDED` is only
libSDL2/liblog/libm/libdl/libc, and `SDL_main` is exported.

Two flags matter for the link and neither is optional:

- `-Wl,-z,max-page-size=16384` -- Android 15+ refuses 4 kB-aligned segments.
- `-static-libstdc++` -- otherwise `libc++_shared.so` has to ship alongside.

The one host dependency that did have to go was OpenSSL, and it was one MD5:
the fork now carries `MD5Builder_android.h` (see its `ANDROID.md`).

## What is still open

- **Toolchain in the build.** `scripts/android_build.py` works and is what
  built every binary above, but it needs a `compiledb` run first and it is not
  a PlatformIO environment. Original plan, still unbuilt: a new
  `[env:simulator_android]` extending
  `[env:simulator]`, plus a `post:` extra_script that does
  `env.Replace(CC=..., CXX=..., LINK=...)`. The native platform's builder is 80
  lines and picks its compiler off `PATH` (`~/.platformio/platforms/native/builder/main.py:24-35`,
  deletes `CC`/`CXX` then calls `env.Tool("gcc")`), so there is no pinned
  toolchain package fighting the override. **Open:** SCons expands `$CXX` at
  build time, so a post-script `Replace` should take -- but `PROGSUFFIX` and the
  target name are fixed inside `env.BuildProgram()`, which runs first. Producing
  `libmain.so` therefore needs a rename step, or the CMake fallback below.
- **Fallback if the override cannot be tamed.** Generate `compile_commands.json`
  and turn it into a `CMakeLists.txt` for gradle's `externalNativeBuild`. The
  file list still comes from `build_src_filter` and `lib_deps`, not a hand-kept
  copy.
Done, and recorded in the fork's `ANDROID.md`: SDL2 for Android, the app shell,
the entry point (no source change needed), and the SD card path (resolved
natively from `SDL_AndroidGetInternalStoragePath`, because Java cannot set an
environment variable early enough).

- **Buttons.** Still none on screen. Not blocking: `adb shell input keyevent`
  reaches SDL through `SDLActivity`, which is enough for scripted runs. On-screen
  buttons belong in Java so `HalGPIO` stays untouched.
- **Panel geometry.** The map is clipped on the right and at the bottom,
  identically on three phones with three different window sizes -- so it is
  inside the 480x800 panel space, not the scaling. The fork's `ANDROID.md` has
  the reasoning and what would settle it.
- **`_exit(0)` on quit** (`simulator_main.cpp`, last line) kills the process, so
  Android restarts the activity rather than resuming it. Cosmetic.
- **ABI:** `arm64-v8a` only.
- **The recorded "main thread" will be the wrong one.** The fork captures it in
  a global initialiser: `const std::thread::id simulatorMainThread =
  std::this_thread::get_id();` (`src/HalDisplay.cpp:66`), and
  `HalDisplay::displayBuffer` presents the frame immediately only when the
  caller matches it (`src/HalDisplay.cpp:389`). On Android those are two
  different threads: static initialisers run when `System.loadLibrary` maps
  `libmain.so`, which is the activity's UI thread, while SDL runs `SDL_main` on
  a thread of its own. So the comparison is always false and `displayBuffer`
  never presents.
  Probably not fatal -- `simulator_main.cpp`'s loop calls `presentIfNeeded()`
  once per frame anyway, so frames would appear one iteration late. It does
  become fatal if the firmware ever blocks inside `loop()` waiting on a
  refresh.
  Fix: capture the id at the top of `main()` instead of at static-init time.
  That is a latent bug on any platform where those two differ, so it belongs
  upstream, not in the fork. **Read off the code, not yet observed running.**

Nothing builds on the phone. The APK is cross-compiled on the laptop; gradle
only packages the `.so`.

## BLE console over real radio, 2026-08-24

The Android build's NimBLE is real by default -- the TCP shim only exists
when `CROSSPOINT_SIM_BLE_PORT` is set, and a plain `am start` leaves it
unset. So `tools/mapcmd.py --ble <cmd>` (bleak, no `--sim`) reaches the map
console straight over the phone's own advertised GATT service: `zoom N`,
`mode ride`, `pos <lat> <lon> heading <N>`, `info` all round-tripped against
a Samsung S10 with no bridge and no `adb forward`. Confirms the Android
build is a real test target for anything the console can reach, not just a
visual check.

Also reproduced from a different firmware branch than the 2026-08-23
baseline (`scripts/android_build.py --out .../jniLibs/arm64-v8a/libmain.so`
against a feature branch's `compile_commands.json`), so the pipeline is
branch-general, not tied to one checkout.

Two things cost time:

- **`mapcmd.py`'s global flags must come before the command.** Everything
  after the recognised options is joined and sent verbatim as the command
  line, so `mapcmd.py --ble info -v` sends the literal command `info -v`
  (2 tokens) and the parser answers `ERR bad_arity`. Flags go first:
  `mapcmd.py --ble -v info`.
- **Two phones running `org.explorink.simulator` at once both advertise the
  same service UUID.** The scanner connects to whichever answers first, so
  a stale instance left running from an earlier session can silently eat
  every command. `adb -s <serial> shell am force-stop
  org.explorink.simulator` on every phone but the one under test before
  scanning.
