# Per-device release branches

Decided 2026-08-31, when LilyGo T5 S3 Pro bring-up started (GNSS, touch, a new
panel driver, all at once). The risk: `develop` carries the code every device
builds on, and device bring-up produces questionable, half-working code for
long stretches. That code should not sit on `develop` while it is being found
out.

## The model

One branch per device, `release/<device-slug>`, forked from `develop`. The
slug matches the device's doc: `docs/devices/<slug>.md` in the parent repo, so
`release/lilygo-t5-s3-pro` for [`../../docs/devices/lilygo-t5-s3-pro.md`](../../docs/devices/lilygo-t5-s3-pro.md).

- **Hardware-conditional work for that device forks from `release/<device>`.**
  Bring-up, a panel driver, a touch stack, a GNSS rail. **Everything else forks
  from `develop`**, which is most work — see "Where a branch forks from:
  `develop` by default" below. Use the normal worktree recipe (`CLAUDE.md`,
  "Every change goes in a worktree"), just change the base branch:

  ```
  git -C firmware/explorink worktree add ../../.worktrees/firmware/<topic> -b <topic> release/<device>
  ```

- **Merge finished feature branches into `release/<device>`**, same testing
  bar as any other merge (`CLAUDE.md`, "Never merge into a production branch
  untested. Ask.") — `release/<device>` is not exempt just because it is not
  `develop`.
- **When the branch is stable, all of it goes into `develop`** — that is what
  hands the work to every other device, and it is how the branch ends rather
  than becoming a parallel line. See "A stable device branch goes back into
  `develop`, whole" below. Still a separate ask with its own hardware test.
- **Sync the other direction too.** `develop` keeps moving while a device's
  release branch is being worked — core fixes, shared refactors, other
  devices' contributions. Merge `develop` into `release/<device>` **before
  forking any feature branch off it**, so the work starts on the current base.
  See "Sync the device branch before forking a feature off it" below, including
  the submodule pointer that merge will otherwise carry quietly.

## Why hardware-conditional work does not sit on `develop`

A single GNSS probe, touch driver and panel init are each small, but together
they are one long stretch where the board does not reliably boot. Nothing
about that should reach a session working on X4 firmware from `develop`. The
release branch is the holding area; `develop` sees the result.

That argument covers bring-up and nothing else. It is **not** a reason to put
ordinary features there — see "Where a branch forks from" below for the split,
and for what happens when the holding area quietly becomes the trunk.

## Verifying a `develop`-based branch on a device branch's board

**The old answer was to cherry-pick onto a throwaway branch. That is now
forbidden** -- see "No cherry-pick between our own branches" below.

The reason a cherry-pick was needed at all is one line of `platformio.ini`: the
device's env exists only on `release/<device>`, so a `develop`-based branch
cannot be flashed to that board. **With the env on `develop`, there is nothing
to carry**: build the branch itself, flash it, confirm, merge into `develop`.

Moving the envs down is therefore a prerequisite for this rule rather than an
option, and it is the open half of T-289 in the parent repo. Until that is done
there is no supported way to verify a `develop`-based branch on a board whose
env lives elsewhere -- say so and stop, rather than reaching for the banned tool.

History: `pins-on-sync` was verified the old way on 2026-09-06/07 on a T5 S3 Pro
with no X4 available, then merged into `develop` as `adf6faa1`. That is how it
was done, not how it is done.

## A `release/*` branch is never deleted

**Standing rule, maintainer, 2026-09-09.** `git branch -d` and `git branch -D`
are never run on a branch whose name starts with `release/`, and neither is a
`git push origin --delete`. That covers every one of them, device branches and
version branches alike: `release/xteink-x4-pro`, `release/xteink-x3`,
`release/xteink-x4`, `release/lilygo-t5-s3-pro`, `release/1.5.0`, and any added
later.

**No condition unlocks it.** Not merged, not stale, not superseded, not "the
board boots now", not "nobody has touched it in a month". If a release branch
looks finished, the reading is wrong, not the branch.

This is about the branch and nothing else: a release branch's *worktree* is
cleaned up like any other, and `git worktree add` brings the branch back into a
directory when the next round needs it.

The reason a session reaches for the delete is the number. Every change to a
board forks from its release branch, takes its hardware pass there and merges
up, so **being fully merged is the normal state between rounds**: a branch that
has just handed its work to `develop` reads `0 ahead of develop`, which looks
like nothing is left in it. It means the last round landed. A branch sitting
*behind* `develop` is the same non-event -- that is what the sync below is for.

Nor is a booting board a reason to retire its branch. The X4 Pro booted and
drew on 2026-09-09 and the branch stayed, because the next round of X4 Pro work
starts there too (T-297 in the parent repo is that next round).

## Existing branches

- `release/lilygo-t5-s3-pro` — created 2026-08-31 from `develop`.
- `release/xteink-x3` — created 2026-09-09 from `develop`, for the panel-layout
  work the first flash uncovered. See "The X3 needed no branch for its binary"
  below.
- `release/xteink-x4-pro` — created 2026-09-09 from `develop`, carrying
  `[env:x4pro]`. **The board's first boot ran the same day**: it boots, draws
  Home, mounts the SDMMC card and answers `CMD:SCREENSHOT`, after one fix that
  the branch then carried up into `develop` (`63a47ff3`, merged as `f2ddbaca`).
  Touch, the buttons, the frontlight and a map frame are still untried --
  [`xteink-x4-pro-bringup.md`](xteink-x4-pro-bringup.md) has the boot log and
  the rest of that list.

  **The branch stays, and it is the entry point for every X4 Pro change.**
  Maintainer's decision, 2026-09-09, after the first boot had already been
  merged up and the branch sat 0 ahead of `develop`. Being absorbed is not a
  reason to retire it: while a board is in bring-up, work that touches that
  board forks from `release/<device>`, gets its hardware pass there, and merges
  up. That is the model, not a phase the board graduates from once it boots
  once. A session that reads "0 ahead of develop" as "this branch is done" has
  the direction backwards -- 0 ahead means the last round landed, and the next
  round starts here again.

**The X3 needed no branch for its binary, and got one anyway for its panel.**
It is an ESP32-C3 and `[env:default]` already builds one binary for the X4 and
the X3 together (`platformio.ini`, `FREEINK_DEVICE_X4` and `FREEINK_DEVICE_X3`
side by side); the framebuffer is sized to the largest selected panel
(`BoardConfig.h`, `MAX_FRAMEBUFFER_BYTES`). That is still why the X3 never
needed a bring-up branch: it boots.

`release/xteink-x3` exists anyway, created 2026-09-09 from `develop` 92c949ae on
the maintainer's call, once the X3's first flash showed the binary is the easy
half. Three layout defects are specific to its 528x792 panel and invisible on a
480x800 X4 (T-295, T-296 in the parent repo), and work on those is exactly the
hardware-conditional kind this model keeps off `develop`. So the rule stands as
written -- a device gets a branch when its work would disturb other devices --
and the X3 qualifies on layout, not on boot.

## Fork from `origin/release/<device>`, never from the local ref

The local `release/<device>` branch is usually checked out in some other
session's worktree, pinned wherever that session left it. On 2026-09-07 the
local `release/lilygo-t5-s3-pro` was **136 commits behind origin and 0 ahead**,
and a branch taken from it built cleanly and was minutes from being flashed
against a stale base.

What caught it was the pre-flash check (`../../CLAUDE.md`, "Only flash a
rebased branch") -- run against `origin/release/<device>` rather than against
`develop`, which is the wrong target for a device branch:

```
git -C firmware/explorink fetch origin
git -C <worktree> log --oneline origin/release/<device> ^HEAD
```

Empty means current. Anything listed means rebase, rebuild, then flash.

**It happened again on 2026-09-10, and this time nothing caught it.** A session
building the T5 S3 Pro's first release forked from the local
`release/lilygo-t5-s3-pro`, which was **53 commits behind origin**. The build
was clean, so nothing complained; the board was flashed twice; and what the
maintainer saw on the panel was a device with **no double-tap touch lock and
none of the S3 input work**. The session then spent an hour deciding the feature
had never landed on that branch, on the evidence that the X4 Pro line's commit
`dcb8e023` is not an ancestor of it -- while `origin/release/lilygo-t5-s3-pro`
carried the same work in its own commits all along, in four files.

Two rules out of that hour:

- **The pre-flash check is for a release build too**, not only for a feature
  branch. A wrong base does not fail a build; it ships a device that looks like
  it regressed.
- **When a feature is expected to be in a build, grep the ref for it** rather
  than reasoning about ancestry. Ancestry answers "is this commit in here",
  which is a different question from "is this work in here" the moment the same
  work exists as two commits:

  ```
  git grep -il "<symbol>" origin/release/<device> -- src lib
  ```

The same stale ref makes `git branch -d` lie. It compares against the local
branch and answers `not fully merged` for a branch that is fully merged into
origin. Check the real question, then force:

```
git -C firmware/explorink merge-base --is-ancestor <branch> origin/release/<device>
git -C firmware/explorink branch -D <branch>
```

## Where a branch forks from: `develop` by default

**Maintainer's decision, 2026-09-09.** Two kinds of work, two bases:

- **New functionality forks from `develop`.** This is the default and it is most
  work. A map feature, a settings row, a BLE command, a refactor: none of it is
  about one board, so none of it belongs on a board's branch.
- **Hardware-conditional work forks from `release/<device>`.** Bring-up, a panel
  driver, a touch stack, a GNSS rail — anything that needs *that* board to run
  at all, or that leaves it unreliably booting for a while.

The test is not "which board is on the desk", it is **"would this work exist if
that board did not"**. A feature that merely has to be *verified* on a
particular board is still `develop` work.

## A stable device branch goes back into `develop`, whole

**Same decision.** When a device branch is stable — everything on it works —
**all of it merges into `develop`**. That is the step that hands the work to
every other device: the shared half of a bring-up (a driver seam, a capability
query, a settings row) is useful to boards nobody was holding at the time, and
it reaches them only through `develop`.

So the device branch is a **holding area with an exit**, not a parallel line. It
exists for the stretch where the board is unreliable, and it ends when the board
is not.

This is a merge into a production branch: it needs a hardware pass and the
maintainer's go-ahead, same as any other
([`../../../CLAUDE.md`](../../../CLAUDE.md), "Never merge into a production
branch untested"). "Stable" is the maintainer's call, not a branch statistic.

**The failure mode this rules out** is what `release/lilygo-t5-s3-pro` had
become by 2026-09-08: created 2026-08-31, never promoted, 212 commits ahead of
`develop` and carrying 124 docs-only commits plus 23 code commits that no other
board could see. It had stopped being a holding area and become the de-facto
trunk, with `develop` as the branch nobody built on.

### The consequence for build environments

If new functionality forks from `develop`, then a board's env has to be **on**
`develop`, or the default base cannot be flashed to that board and the rule
collapses back into the thing that produced the cherry-picks. Moving
`[env:t5s3pro]` and `[env:x4pro]` down is therefore part of this model, not a
separate cleanup. T-289 in the parent repo.

**The `[env:x4pro]` half is done, 2026-09-09.** It rode up with the first-boot
merge and sits on `develop` (`platformio.ini`, `[env:x4pro]`), so a
`develop`-based branch can now be built and flashed to an X4 Pro with nothing
carried. `[env:t5s3pro]` is still only on `release/lilygo-t5-s3-pro` and is the
open half of T-289.

## Sync the device branch before forking a feature off it

**Maintainer's decision, 2026-09-09.** A feature branch forked from
`release/<device>` must be forked from a base that already carries what
`develop` has. So the order is: merge `develop` into `release/<device>`, then
fork. Development happens on the current base, not on whatever the branch
happened to hold.

This replaces "periodically, whenever picking the device work back up after a
gap". A gap is not the trigger; forking is.

```
git -C firmware/explorink fetch origin
git -C <device worktree> log --oneline origin/develop ^HEAD    # what the sync brings
git -C <device worktree> merge origin/develop
```

**The sync does not need its own hardware pass.** The device branch is the
holding area for work that is allowed to be half-finished, and the feature's own
hardware pass then measures the feature against the synced base, which is the
combination that matters. Requiring a pass per sync is what makes people skip
syncing, and skipping syncing is what produced the cherry-picks.

**The sync will also move the `freeink-sdk` pin, and that is fine — but it
happens quietly.** Once `develop` and the device branch pin different SDK
commits and one contains the other, git decides it knows the answer: it stages
the newer one and prints a single `Note: Fast-forwarding submodule freeink-sdk`.
No conflict, nothing to confirm. Measured 2026-09-09: a sync of
`release/lilygo-t5-s3-pro` carries the pin from `55a49587` to `955b2530`,
**209 commits**, inside a routine merge whose only reported conflicts are in
documentation.

**That propagation is wanted.** A pin was moved deliberately somewhere, and a
sync is how the other branches get it. So this is not a thing to undo. What it
costs is a heavier pass, and the point is to notice you now owe it:

- **A row in [`freeink-sdk-pins.md`](freeink-sdk-pins.md)**, with what moved and
  why.
- **A full hardware pass on that branch, not a spot check.** The SDK is the
  panel driver, the SD card and the input layer, so: boot, a map frame (SD
  read), `MKCOL` + `PUT` (SD write), and a large WebDAV GET
  (`readFileToStream`). That set is what caught real defects in this subsystem
  twice.
- **The commit body says it**, with both SHAs.

`scripts/sdk_pin_check.py` answers it and the hooks call it for you:

```
python3 scripts/sdk_pin_check.py --from ORIG_HEAD    # after a merge
python3 scripts/sdk_pin_check.py                     # HEAD against its parent
```

It reports the direction, the span, and whether the patches on
`origin/explorink` are in the new pin; exit 1 means it moved, 2 means a patch of
ours is not in the new pin.

**The hooks are off on purpose, and turning them on is not yet safe.**
`.githooks/post-merge` and `post-commit` exist, and git ignores them until it is
told where they are:

```
git -C firmware/explorink config core.hooksPath .githooks     # NOT YET, see below
```

Nobody had that set as of 2026-09-09, which is why the `pre-commit`
clang-format hook had been running for no one and why 35 files had drifted out
of format unnoticed.

**Why the wait.** `core.hooksPath` is one setting for the whole repo, but a
relative path resolves **per working tree** -- measured 2026-09-09 with a
discriminating instrument, because `git rev-parse --git-path hooks` only echoes
the configured relative value and proves nothing. The two versions of
`pre-commit` print different lines, so a commit in a detached worktree at
`207e5a94` run with `git -c core.hooksPath=.githooks` said `Running clang-format
fix before commit...`, the pre-fix wording, and left **35 files modified in that
working tree** on a commit that touched only `README.md`. The rest of this
paragraph follows from that — so every worktree runs the hook
from *its own branch*, not from `develop`. When this was switched on for a few
minutes on 2026-09-09, 21 of 23 worktrees still carried the pre-fix
`pre-commit`, the one that reformats all 544 tracked C/C++ files instead of the
modified ones. Several of those are branches with work in progress. Switching it
on would have handed each of those sessions 35 rewritten files, in their own
working tree, on a commit that touched none of them.

**The gate**: turn it on once the branches people are working on carry
`./bin/clang-format-fix -g` (fixed on `develop` in `c7dcf135`). They pick it up
on their next sync from `develop`, which the rule at the top of this section
requires anyway. Check before flipping it:

```
for d in $(git -C firmware/explorink worktree list --porcelain \
            | sed -n 's/^worktree //p'); do
  grep -q 'clang-format-fix -g' "$d/.githooks/pre-commit" 2>/dev/null \
    || echo "old hook: $d"
done
```

Until then, run the pin check by hand — it is the same script the hook calls.

**CI cannot cover this, and the reason is bigger than the triggers.**
`.github/workflows/ci.yml` does trigger only on `push: branches: [master]` and
pull requests, and this repo's trunk is `develop` -- but even a PR would run
nothing. `rfordinal/explorink` is a **fork** of
`crosspoint-reader/crosspoint-reader`, and GitHub gates Actions on forks behind
a manual one-time enable that the API does not report: measured 2026-09-09,
`actions/permissions` says `{"enabled": true}`, five workflows are registered
`active`, and `actions/runs` returns `total_count: 0` -- **no workflow has ever
run in this repo.** So `release.yml`, wired to `on: push: tags`, did not fire
for either tag pushed on 2026-09-08 either; the two releases that exist were
made by hand. Tracked as T-294 in the parent repo. The hook plus the log is the
mechanism; there is no server-side net behind it.

**Cost of the pending sync, measured 2026-09-09 with `merge-tree`:**
`release/lilygo-t5-s3-pro` is 63 commits behind `develop` and 212 ahead, its pin
differs, and the merge conflicts in **nine** files. It was eight earlier the
same day; two of the nine are documentation this session wrote onto `develop`
alone (`docs/freeink-sdk-fork.md` among them), which is the deferred cost of the
no-cherry-pick rule working as intended rather than a new problem.
`release/xteink-x4-pro` is 6 behind, 1 ahead, same pin.

## `git log -S` skips merges, so it cannot prove a branch never had something

Used 2026-09-12 to ask when `[env:x4pro]` left `release/lilygo-t5-s3-pro`.

```
git log -S'[env:x4pro]' release/lilygo-t5-s3-pro -- platformio.ini
```

printed nothing, and that was read as "it was never there". Wrong: `-S` skips
merge commits by default, and a merge is exactly where a conflict resolution
drops a block. The conclusion drawn from it happened to be right and the
evidence was worthless.

What actually answers it is reading the file at each parent of the suspect
merge:

```
git show <merge>^1:platformio.ini | grep -c '^\[env:x4pro\]'   # 0, the branch side
git show <merge>^2:platformio.ini | grep -c '^\[env:x4pro\]'   # 1, the develop side
git show <merge>:platformio.ini   | grep -c '^\[env:x4pro\]'   # 0, the result
```

0, 1, 0 names the merge as the place it was dropped, and names which side won.

**On a branch with merges, a silent `-S` is not a negative finding.** Where the
pickaxe has to see merges at all, it needs `--diff-merges` -- but for "which
merge resolved this away", reading the two parents is shorter and says which
side was taken, which the pickaxe never does.

## No cherry-pick between our own branches

**Maintainer's decision, 2026-09-09.** A change reaches another branch of ours
by **merge**. `git cherry-pick` is not used to move our own work, and this
section used to say the opposite.

This does not cover taking commits out of a foreign upstream. A CrossPoint
review take is selecting from someone else's history, not moving our own work,
and it keeps its own procedure
([`upstream-crosspoint.md`](upstream-crosspoint.md)).

### What the old rule said, and why it was replaced

It said a `git merge` of a `develop`-based branch drags every `develop` commit
the device branch has not taken -- 54 of them on 2026-09-07 -- so the hardware
pass that follows measures all of it instead of measuring the change. That
observation is true. The conclusion was wrong, because the drag is a symptom of
the device branch being allowed to fall behind, and cherry-picking makes the
drift permanent instead of fixing it.

Measured on 2026-09-09, on this repo:

- **Twin branches.** Four pairs of them: `cmd-buttons` / `cmd-buttons-t5s3`,
  `diag/boot-reason` / `-t5s3`, `feat/map-popup-size-classes` / `-t5s3`,
  `touch-lock-flag` / `-t5s3`. Three of the four twins were merged into the
  release branch despite this doc calling them throwaway.
- **The same file written twice.** `docs/settings-menu.md` existed on both
  branches with an **identical commit subject**, a different SHA and 134 lines
  of divergence. Git cannot see those as the same patch, so the next sync hits
  it as an add/add conflict.
- **Eight conflicts** in the pending sync, five of them produced by the
  cherry-picking itself, and every one of them resolved twice -- once on each
  branch. Recounted 2026-09-09 from the recorded `merge-tree` output, which
  lists eight files; this document said seven all day because nobody counted the
  lines twice.

So: carry a change by merging, which means **the target branch has to be kept
current** rather than left to drift. A sync that drags 54 commits is a sync that
was overdue, not a reason to avoid syncing.

