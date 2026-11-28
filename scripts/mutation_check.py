#!/usr/bin/env python3
"""Run a list of mutants against the host tests and report which ones survive.

A mutant is one deliberate break of an invariant: a file, an exact snippet and
its replacement. For each one this script copies the tree's `src`, `lib`,
`data`, `scripts` and `test` into a scratch directory, applies the replacement,
rebuilds only the named host targets and runs them. The mutant is KILLED when at
least one test fails (or the binary aborts); it SURVIVED when everything passes,
which means no test guards that invariant.

Why a script and not a reviewer: a reviewer asked "find anything" never answers
empty, and two reviewers do not run the same mutants. This list is the same
every time, so "all killed" is a result that can be repeated and compared.

Usage:
  python3 scripts/mutation_check.py test/mutation/t2104.json
  python3 scripts/mutation_check.py test/mutation/t2104.json --cpus 0,2    # two physical cores
  python3 scripts/mutation_check.py test/mutation/t2104.json --load        # every core busy

Pin to two *physical* cores for the small-host check: `lscpu -e=CPU,CORE` names
them. Two hyperthreads of one core hide memory-ordering bugs -- a release/acquire
gate survived `--cpus 0,1` (one core) and died on `--cpus 0,2`, 2026-11-28. On one
CPU the race tests skip, so their mutants survive by design.

The list file is JSON: [{"id", "file", "old", "new", "targets": [...],
"filter": "<gtest filter>", "why"}]. `old` must occur exactly once in `file`.
Exit status 1 when any mutant survives or cannot be applied.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
COPY = ["src", "lib", "data", "scripts", "test"]
# Target name -> binary path relative to the cmake build dir.
BINARIES = {"WalletTest": "wallet/WalletTest", "MapTransferTest": "map_transfer/MapTransferTest"}


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("listfile")
    ap.add_argument("--cpus", help="taskset CPU list for the test runs, e.g. 0 or 0,1")
    ap.add_argument("--load", action="store_true", help="busy-loop every core while tests run")
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    mutants = json.load(open(args.listfile))
    work = tempfile.mkdtemp(prefix="mutation-")
    tree = os.path.join(work, "tree")
    os.makedirs(tree)
    for d in COPY:
        shutil.copytree(os.path.join(ROOT, d), os.path.join(tree, d), symlinks=True,
                        ignore=shutil.ignore_patterns(".pio", "build"))
    build = os.path.join(work, "build")
    if run(["cmake", "-S", os.path.join(tree, "test"), "-B", build]).returncode != 0:
        print("cmake configure failed", file=sys.stderr)
        return 2

    targets = sorted({t for m in mutants for t in m["targets"]})
    r = run(["cmake", "--build", build, "--target", *targets, "-j8"])
    if r.returncode != 0:
        print("baseline build failed:\n" + r.stdout[-2000:], file=sys.stderr)
        return 2

    load = []
    if args.load:
        for _ in range(os.cpu_count() or 1):
            load.append(subprocess.Popen(["sh", "-c", "while :; do :; done"]))

    def run_tests(m):
        failed = False
        for t in m["targets"]:
            cmd = [os.path.join(build, BINARIES[t])]
            if m.get("filter"):
                cmd.append("--gtest_filter=" + m["filter"])
            if args.cpus:
                cmd = ["taskset", "-c", args.cpus] + cmd
            try:
                p = run(cmd, timeout=args.timeout)
                if p.returncode != 0:
                    failed = True
            except subprocess.TimeoutExpired:
                return "HANG"
        return "fail" if failed else "pass"

    results = []
    try:
        base = run_tests({"targets": targets})
        if base != "pass":
            print(f"baseline tests do not pass ({base}); nothing below would mean anything")
            return 2
        for m in mutants:
            path = os.path.join(tree, m["file"])
            src = open(path).read()
            if src.count(m["old"]) != 1:
                results.append((m["id"], "NOT-APPLIED", f"`old` occurs {src.count(m['old'])} times"))
                continue
            open(path, "w").write(src.replace(m["old"], m["new"]))
            b = run(["cmake", "--build", build, "--target", *m["targets"], "-j8"])
            if b.returncode != 0:
                verdict = "KILLED (does not compile)"
            else:
                outcome = run_tests(m)
                verdict = {"fail": "KILLED", "pass": "SURVIVED", "HANG": "HANG"}[outcome]
            open(path, "w").write(src)
            os.utime(path, None)  # newer than the mutant's object, so the next build reuses nothing stale
            results.append((m["id"], verdict, m.get("why", "")))
    finally:
        for p in load:
            p.kill()

    width = max(len(r[0]) for r in results)
    bad = 0
    for mid, verdict, why in results:
        if not verdict.startswith("KILLED"):
            bad += 1
        print(f"{mid.ljust(width)}  {verdict:<26} {why}")
    print(f"\n{len(results) - bad}/{len(results)} killed"
          + (f"  (cpus={args.cpus})" if args.cpus else "") + ("  (load)" if args.load else ""))
    shutil.rmtree(work, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
