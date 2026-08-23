#!/usr/bin/env python3
"""Compile-only probe: does the simulator's source compile for Android?

Replays the host simulator build's compile database with the NDK's aarch64
clang, discarding every object. No linking. It answers exactly one question --
does bionic plus the NDK's libc++ accept this codebase -- and nothing about
SDL2, linking or the app shell.

    pio run -e simulator -t compiledb
    python3 scripts/android_compile_probe.py

See docs/simulator-android.md for the findings and for why the shim exists.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

DEFAULT_NDK = os.path.expanduser("~/Android/Sdk/ndk/27.3.13750724")
API = 24


def build_shim(directory):
    """Copy the host headers an aarch64 target cannot find on its own.

    SDL2 and OpenSSL are host dependencies of the simulator, and a real Android
    build supplies Android versions of both. For a compile-only probe the host
    headers are good enough -- but they must be copied into a private directory,
    never reached with -I/usr/include, which drags glibc in and dies on
    gnu/stubs-32.h.
    """
    os.makedirs(f"{directory}/SDL2", exist_ok=True)
    sdl_config = "/usr/include/x86_64-linux-gnu/SDL2/_real_SDL_config.h"
    if os.path.exists(sdl_config):
        dest = f"{directory}/SDL2/_real_SDL_config.h"
        shutil.copy(sdl_config, dest)
        # Debian's config opts into x86 SIMD; immintrin.h refuses to compile for
        # ARM, 19 errors deep.
        text = open(dest).read().replace(
            "#define HAVE_IMMINTRIN_H 1",
            "/* HAVE_IMMINTRIN_H stripped: aarch64 probe */")
        open(dest, "w").write(text)
    for src in ("/usr/include/openssl",
                "/usr/include/x86_64-linux-gnu/openssl"):
        if os.path.isdir(src):
            shutil.copytree(src, f"{directory}/openssl", dirs_exist_ok=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ndk", default=DEFAULT_NDK)
    ap.add_argument("--db", default="compile_commands.json")
    ap.add_argument("--shim", default=".pio/android-probe-shim")
    args = ap.parse_args()

    bindir = f"{args.ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin"
    cxx = f"{bindir}/aarch64-linux-android{API}-clang++"
    cc = f"{bindir}/aarch64-linux-android{API}-clang"
    if not os.path.exists(cxx):
        sys.exit(f"no NDK compiler at {cxx}")
    if not os.path.exists(args.db):
        sys.exit(f"no {args.db} -- run: pio run -e simulator -t compiledb")

    build_shim(args.shim)
    shim = os.path.abspath(args.shim)
    db = json.load(open(args.db))

    def run(entry):
        cmd = re.sub(r"^/usr/bin/g\+\+", cxx, entry["command"])
        cmd = re.sub(r"^/usr/bin/gcc", cc, cmd)
        cmd = re.sub(r"-o \S+\.o", "-o /dev/null", cmd)
        cmd = cmd.replace(" -c ", f" -c -I{shim} ")
        p = subprocess.run(cmd, shell=True, cwd=entry["directory"],
                           capture_output=True, text=True)
        return entry["file"], p.returncode, p.stderr

    results = list(ThreadPoolExecutor(max_workers=os.cpu_count()).map(run, db))
    failed = [r for r in results if r[1] != 0]

    print(f"translation units: {len(results)}")
    print(f"compiled clean:    {len(results) - len(failed)}")
    print(f"failed:            {len(failed)}")
    for path, _, err in failed:
        print(f"\n--- {path}")
        for line in [l for l in err.splitlines() if ": error:" in l or ": fatal error:" in l][:8]:
            print("   ", line)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
