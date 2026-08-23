# The simulator on a phone

Idea: build the simulator as an Android app, so the firmware UI runs on a phone
with no reader hardware in reach.

**Not an emulator.** The simulator is the firmware compiled as native code with
`lib/hal/` replaced by an SDL2 HAL (`platformio.ini`, `[env:simulator]`; the HAL
lives in the `explorink-simulator` fork). An ESP32-C3 `.bin` never enters it. So
an Android build is a third compile target of the same sources, and a new
firmware means a new APK.

Status: **the compile probe passed.** Nothing else is built yet.

## Milestone 0: does the code compile for Android at all? Yes

Verified 2026-08-23, NDK r27.3.13750724, clang 18, target `aarch64-linux-android24`.

| | |
|---|---|
| translation units | 208 |
| compiled clean for aarch64 | 206 |
| real failures | 2 |

Compile-only, no link. It answers one question: does Android's C library
(bionic) plus the NDK's libc++ accept this codebase. It says nothing about
linking, SDL2, or the app shell.

Method: build the host simulator's compile database, then replay every command
with the NDK compiler and throw the object away.

```bash
pio run -e simulator -t compiledb        # writes compile_commands.json
```

Then, per entry in `compile_commands.json`: swap `/usr/bin/g++` for
`$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++`,
rewrite `-o <file>.o` to `-o /dev/null`, run it in the entry's `directory`.

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

Fix: parse the float without `<charconv>` -- `strtof` on a NUL-terminated copy,
checking `errno` and the end pointer to keep the current "reject a non-numeric
suffix" behaviour. Untested.

CSS parsing is the EPUB reader's, not the map's.

### Real failure 2: `std::min` with no `<algorithm>`

`src/activities/map/MapRenderer.cpp:80`:

```
error: no member named 'min' in namespace 'std'; did you mean 'fmin'?
```

The file includes `<cmath>` and `<cstdio>` only (`MapRenderer.cpp:2-3`).
libstdc++ leaks `std::min` in through its header graph; libc++ does not.

This is a latent defect independent of Android: the device build compiles only
by accident of which headers libstdc++ happens to pull in.

Fix: `#include <algorithm>`. Verified -- re-running the probe with
`-include algorithm` compiles the file clean.

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

## What is still open

Everything past the compiler.

- **Toolchain in the build.** Plan: a new `[env:simulator_android]` extending
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
- **SDL2 for Android.** `ndk-build` in the SDL source tree. Unverified.
- **App shell.** An `SDLActivity` subclass whose `getLibraries()` returns
  `{"SDL2", "main"}`. `AndroidManifest.xml` needs `INTERNET` for the firmware's
  web server.
- **Entry point.** `src/simulator_main.cpp:1` includes `<SDL.h>` before
  defining `main`, and SDL renames `main` to `SDL_main` by macro on platforms
  that need it. **Open:** if that holds on Android, the entry point needs no
  source change.
- **Buttons.** Keep them in Java, injecting key events into SDL. Drawing them in
  the HAL would grow the fork's diff for no gain.
- **SD card path.** Already an environment variable: `CROSSPOINT_SIM_SD`, else
  `CROSSPOINT_EMU_SD`, else `./fs_` (fork's `src/HalStorage.cpp:20-25`). The
  Java shell sets it to the app's files directory. No C++ change.
- **`_exit(0)` on quit** (`simulator_main.cpp`, last line) kills the process, so
  Android restarts the activity rather than resuming it. Cosmetic.
- **ABI:** `arm64-v8a` only.

Nothing builds on the phone. The APK is cross-compiled on the laptop; gradle
only packages the `.so`.
