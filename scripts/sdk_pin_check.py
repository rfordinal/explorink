#!/usr/bin/env python3
"""Report when the freeink-sdk submodule pointer moves, and how far.

    python3 scripts/sdk_pin_check.py                      # HEAD against its parent
    python3 scripts/sdk_pin_check.py --from ORIG_HEAD     # after a merge
    python3 scripts/sdk_pin_check.py --from origin/develop --to HEAD

Why this exists: the firmware repo does not contain the SDK's files. It stores
one line saying which freeink-sdk commit to build against -- a gitlink. So
`git show --stat` renders an SDK change as `freeink-sdk | 2 +-`, one changed
file, and 208 commits of panel-driver, SD and input code hide behind it.

Moving the pin is a normal, deliberate thing to do on a branch. What is not
normal is *not noticing*, and that happened twice:

  * 44fe2972 (2026-09-07) moved it backwards inside a commit about a frontlight
    Settings row, dropping two patches of our own for three days.
  * a develop -> release merge staged it forward with no conflict at all, because
    git fast-forwards a gitlink when one side's commit is an ancestor of the
    other's. The 7 conflicts that merge did report were all in documentation.

So this prints a warning rather than refusing anything. A moved pin means the
next hardware pass on that branch is the fuller one, not a spot check -- see
docs/freeink-sdk-fork.md.

Exit status: 0 unchanged, 1 moved, 2 moved and a patch of ours is not in the new
pin, 3 could not answer (no submodule, unknown ref).
"""

import argparse
import subprocess
import sys

SUBMODULE = "freeink-sdk"
FORK_BRANCH = "origin/explorink"
MIRROR_BRANCH = "origin/main"


def git(args, cwd=".", check=True):
    r = subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        return None
    return r.stdout.strip()


def require_submodule_checkout():
    """Refuse to answer from the wrong repository.

    An uninitialised submodule leaves `freeink-sdk/` as an empty directory, and
    `git -C freeink-sdk ...` then walks *up* and answers from the firmware repo
    instead -- silently, with a zero exit status. Every query here would then be
    about the wrong history: `rev-parse --git-dir` succeeds, `log -1` prints a
    firmware commit, and `merge-base --is-ancestor` says no to everything, so a
    backward pin move reads as "sideways" and lost patches read as present.
    Same family as the .git-file gitlink trap in the parent repo's
    docs/worktrees.md.

    A fresh worktree has no submodule checkout at all -- see the parent
    CLAUDE.md, "A fresh firmware worktree has no freeink-sdk checkout".
    """
    top = git(["rev-parse", "--show-toplevel"], cwd=SUBMODULE)
    if top is None or not top.endswith("/" + SUBMODULE):
        return (f"{SUBMODULE}/ is not a checked-out submodule here"
                f" (git there answers from {top or 'nowhere'}).\n"
                f"Run: git submodule update --init {SUBMODULE}")
    return None


def pin_at(ref):
    """The submodule commit a firmware ref points at, or None."""
    return git(["rev-parse", f"{ref}:{SUBMODULE}"])


def sub(args, check=True):
    return git(args, cwd=SUBMODULE, check=check)


def is_ancestor(a, b):
    r = subprocess.run(["git", "merge-base", "--is-ancestor", a, b],
                       cwd=SUBMODULE, capture_output=True, text=True)
    return r.returncode == 0


def our_patches():
    """Commits on the fork branch that are not on the mirror.

    These are the patches a pin has to contain to be carrying our work. A commit
    listed here whose *content* upstream has since taken shows up as missing;
    the caller is told to diff rather than to panic.
    """
    out = sub(["rev-list", FORK_BRANCH, "--not", MIRROR_BRANCH, "--no-merges"])
    return [c for c in (out or "").splitlines() if c]


def describe(commit):
    return sub(["log", "-1", "--format=%h %ci %s", commit]) or commit


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--from", dest="old", default="HEAD^",
                   help="firmware ref to compare from (default HEAD^; use ORIG_HEAD after a merge)")
    p.add_argument("--to", dest="new", default="HEAD", help="firmware ref to compare to (default HEAD)")
    p.add_argument("--quiet-when-unchanged", action="store_true",
                   help="print nothing at all when the pin did not move (for hooks)")
    args = p.parse_args()

    problem = require_submodule_checkout()
    if problem:
        print(f"sdk_pin_check: {problem}", file=sys.stderr)
        return 3

    new_pin = pin_at(args.new)
    if new_pin is None:
        print(f"sdk_pin_check: {args.new} has no {SUBMODULE} gitlink", file=sys.stderr)
        return 3
    old_pin = pin_at(args.old)
    if old_pin is None:
        # A first commit, an unborn ref, or a ref that predates the submodule.
        print(f"sdk_pin_check: cannot read {SUBMODULE} at {args.old}; nothing to compare",
              file=sys.stderr)
        return 3

    for label, pin in (("--from", old_pin), ("--to", new_pin)):
        if sub(["cat-file", "-t", pin]) != "commit":
            print(f"sdk_pin_check: {label} pin {pin[:8]} is not in the local "
                  f"{SUBMODULE} clone. Run: git -C {SUBMODULE} fetch origin",
                  file=sys.stderr)
            return 3

    if old_pin == new_pin:
        if not args.quiet_when_unchanged:
            print(f"freeink-sdk pin unchanged: {new_pin[:8]}")
        return 0

    # The pin moved. Say how far and in which direction before anything else.
    forward = is_ancestor(old_pin, new_pin)
    backward = is_ancestor(new_pin, old_pin)
    if forward:
        span = sub(["rev-list", "--count", f"{old_pin}..{new_pin}"]) or "?"
        direction = f"forward, {span} commits"
    elif backward:
        span = sub(["rev-list", "--count", f"{new_pin}..{old_pin}"]) or "?"
        direction = f"BACKWARD, dropping {span} commits"
    else:
        direction = "sideways -- neither pin contains the other"

    print("=" * 72)
    print(f"WARNING: the freeink-sdk pin moved ({direction})")
    print("=" * 72)
    print(f"  from  {describe(old_pin)}")
    print(f"  to    {describe(new_pin)}")
    print()
    print("  One line of diff. That span is panel-driver, SD and input code.")

    # Whether our own patches survived is the part that actually breaks a board.
    missing = [c for c in our_patches() if not is_ancestor(c, new_pin)]
    if missing:
        print()
        print(f"  {len(missing)} patch(es) on {FORK_BRANCH} are NOT in the new pin:")
        for c in missing:
            print(f"    {describe(c)}")
        print()
        print("  Before concluding they are lost, check whether upstream took the")
        print("  content: git -C freeink-sdk diff <commit> <pin> -- <the files it touched>.")
        print("  An empty diff means upstream merged it and the commit is redundant.")
        print("  Anything else means this pin does not carry our work.")
    else:
        print(f"  Every patch on {FORK_BRANCH} is in the new pin.")

    print()
    print("  This is allowed and often deliberate. What it changes:")
    print("   * record the move -- docs/freeink-sdk-pins.md, one row, with why")
    print("   * the next hardware pass on this branch is the FULL one, not a spot")
    print("     check: boot, a map frame (SD read), MKCOL+PUT (SD write), and a")
    print("     large WebDAV GET (readFileToStream). docs/freeink-sdk-fork.md")
    print("   * say it in the commit body, with both SHAs")
    print("=" * 72)
    return 2 if missing else 1


if __name__ == "__main__":
    sys.exit(main())
