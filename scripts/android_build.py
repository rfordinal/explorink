#!/usr/bin/env python3
"""Build the simulator for Android: replay the host build with the NDK.

The simulator env's compile database names every translation unit and every
flag. This replays each command with the NDK's aarch64 clang, then links the
objects into libmain.so for the Android shell (the simulator repo's
android/app/src/main/jniLibs/).

    pio run -e simulator -t compiledb
    python3 scripts/android_build.py                 # compile and link
    python3 scripts/android_build.py --compile-only  # just answer "does it compile"

docs/simulator-android.md has the findings and the open list.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

DEFAULT_NDK = os.path.expanduser("~/Android/Sdk/ndk/27.3.13750724")
# Must match minSdk in the shell's android/app/build.gradle.kts and the
# APP_PLATFORM its fetch_sdl2.sh builds libSDL2.so with.
API = 24
SDL_DIR = os.path.expanduser("~/.cache/explorink/sdl2/SDL-2.32.10")


def errors(stderr):
    return [l for l in stderr.splitlines() if ": error:" in l or ": fatal error:" in l]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ndk", default=DEFAULT_NDK)
    ap.add_argument("--db", default="compile_commands.json")
    ap.add_argument("--objdir", default=".pio/build/android")
    ap.add_argument("--sdl", default=SDL_DIR,
                    help="SDL2 source tree, for its Android headers")
    ap.add_argument("--out", default=None,
                    help="where to write libmain.so (default: alongside libSDL2.so "
                         "in the simulator repo's jniLibs, if found)")
    ap.add_argument("--compile-only", action="store_true")
    args = ap.parse_args()

    bindir = f"{args.ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin"
    cxx = f"{bindir}/aarch64-linux-android{API}-clang++"
    cc = f"{bindir}/aarch64-linux-android{API}-clang"
    if not os.path.exists(cxx):
        sys.exit(f"no NDK compiler at {cxx}")
    if not os.path.exists(args.db):
        sys.exit(f"no {args.db} -- run: pio run -e simulator -t compiledb")
    sdl_include = os.path.join(args.sdl, "include")
    if not os.path.isdir(sdl_include):
        sys.exit(f"no SDL2 headers at {sdl_include} -- run the shell's "
                 "tools/android/fetch_sdl2.sh")

    os.makedirs(args.objdir, exist_ok=True)
    db = json.load(open(args.db))

    def compile_one(index_entry):
        index, entry = index_entry
        # The object path in the database is the host build's; give each unit a
        # unique name here instead, since two libraries can share a basename.
        stem = os.path.splitext(os.path.basename(entry["file"]))[0]
        obj = os.path.abspath(os.path.join(args.objdir, f"{index:04d}-{stem}.o"))
        # The command from the database is a shell string and stays one, so its
        # own quoting (-DFOO=\"bar\") survives untouched. Everything
        # substituted into it is quoted here, since a path with a space would
        # otherwise become two arguments.
        cmd = re.sub(r"^/usr/bin/g\+\+", shlex.quote(cxx), entry["command"])
        cmd = re.sub(r"^/usr/bin/gcc", shlex.quote(cc), cmd)
        cmd = re.sub(r"-o \S+\.o", f"-o {shlex.quote(obj)}", cmd)
        # The host's SDL2 headers are Debian's, split across a multiarch path
        # and compiled for x86. Point at the Android SDL2 source instead.
        cmd = cmd.replace("-I/usr/include/SDL2", f"-I{shlex.quote(sdl_include)}")
        # Everything ends up in one shared library.
        cmd = cmd.replace(" -c ", " -c -fPIC ")
        p = subprocess.run(cmd, shell=True, cwd=entry["directory"],
                           capture_output=True, text=True)
        return entry["file"], obj, p.returncode, p.stderr

    results = list(ThreadPoolExecutor(max_workers=os.cpu_count())
                   .map(compile_one, enumerate(db)))
    failed = [r for r in results if r[2] != 0]

    print(f"translation units: {len(results)}")
    print(f"compiled clean:    {len(results) - len(failed)}")
    print(f"failed:            {len(failed)}")
    for path, _, _, err in failed:
        print(f"\n--- {path}")
        for line in errors(err)[:8]:
            print("   ", line)
    if failed:
        return 1
    if args.compile_only:
        return 0

    out = args.out
    if out is None:
        guess = os.path.join(
            os.path.dirname(os.path.abspath(args.db)),
            "..", "..", "simulator", "android-shell",
            "android/app/src/main/jniLibs/arm64-v8a")
        out = os.path.join(os.path.normpath(guess), "libmain.so")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    libdir = os.path.dirname(out)
    if not os.path.exists(os.path.join(libdir, "libSDL2.so")):
        sys.exit(f"no libSDL2.so in {libdir} -- run the shell's "
                 "tools/android/fetch_sdl2.sh")

    objs = [r[1] for r in results]
    link = [
        cxx, "-shared", "-fPIC",
        # Android 15+ rejects 4 kB-aligned LOAD segments.
        "-Wl,-z,max-page-size=16384",
        # A shared library links fine with unresolved symbols by default and
        # then fails at dlopen on the phone. Make the linker say so here.
        "-Wl,--no-undefined",
        # One .so to ship instead of libc++_shared.so alongside it.
        "-static-libstdc++",
        "-o", out,
        *objs,
        f"-L{libdir}", "-lSDL2",
        "-llog",
    ]
    print(f"\nlinking {len(objs)} objects into {out}")
    p = subprocess.run(link, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stderr[-20000:])
        return 1
    print(f"linked: {os.path.getsize(out) / 1024:.0f} kB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
