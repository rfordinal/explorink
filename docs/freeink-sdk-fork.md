# The freeink-sdk submodule points at our fork

**Since 2026-09-03.** `freeink-sdk` is fetched from
**`git@github.com:rfordinal/freeink-sdk.git`**, our fork of
`Free-Ink/freeink-sdk`. The pinned commit did not change when the URL did, and
does not have to: a fork carries upstream's whole history, so every commit any
branch here pins is reachable in it.

## The trap: an existing checkout keeps the old URL

`git submodule update --init` reads the URL from `.git/config`, not from
`.gitmodules`, and `git submodule init` does **not** overwrite an entry that is
already there. So a checkout that has ever initialised this submodule keeps
fetching from `Free-Ink` and nothing says so. Run:

```
git submodule sync freeink-sdk
git -C freeink-sdk remote get-url origin      # must be rfordinal/freeink-sdk
```

Measured 2026-09-03: after the `.gitmodules` change alone, a fresh worktree
still cloned from `Free-Ink` and reported the old remote. Only `sync` moved it.

## Remotes inside the submodule

| remote | where |
|---|---|
| `origin` | `git@github.com:rfordinal/freeink-sdk.git` (our fork) |
| `upstream` | `https://github.com/Free-Ink/freeink-sdk.git` |

Same convention as this repo against CrossPoint: `origin` is ours, `upstream` is
theirs.

## Branches in the fork

- **`main`** mirrors upstream. Do not commit to it. A PR to upstream is branched
  from it, so it has to stay clean.
- **`explorink`** is upstream plus our patches. This is what the firmware pins.
  Today that is one patch: `readFileToStream` feeding the task watchdog
  (`55a49587`). The other one, the T5 S3 Pro EPD config no longer asserting the
  LoRa radio's chip select, went upstream as `Free-Ink/freeink-sdk#73` and is
  now in `main`, so the fork no longer carries it.

`.gitmodules` says `branch = main` on every branch of this repo, deliberately.
That field only steers `git submodule update --remote`, which is not part of the
normal flow, and keeping it identical everywhere means `.gitmodules` never
conflicts when `develop` and a `release/*` branch merge. **What is used is the
pinned commit**, and that is allowed to differ per branch.

## Why a fork at all

`CLAUDE.md` used to say "freeink-sdk is upstream, so correct it here rather than
forking the SDK". That cost a workaround in our own tree twice:

- the frontlight PWM ceiling -- the vendor caps the PT4103B23F at about 1 kHz and
  the SDK board profile asks for 5 kHz, corrected in `src/main.cpp` instead
- the T5 S3 Pro EPD config handing the SX1262's chip select to LovyanGFX as a
  dummy pin, which killed the SD card (parent `docs/BUGS.md`, BUG-037)

Neither is a firmware bug and both were patched in the firmware because an SDK
fix had nowhere to be pushed: we have read access only, and our pinned base sat
199 commits behind upstream `main`. This repo already forks for that exact
reason, and so does the simulator.

**So an SDK-level defect now gets a branch in the fork and a PR upstream**, not
a workaround here. Keep the fork's `main` tracking upstream so the PR branch has
a clean base.

## Moving the pinned commit is its own decision

Upstream `main` is far ahead of what this repo builds against, and the gap
carries panel-driver work for the shipping devices -- a new X4 Pro display
driver, e-ink init and ghosting changes, UC8279C grayscale, a new X4C board,
deep-sleep panel parking. Bumping the pin is a separate task with its own
hardware pass on an X4 or X4 Pro. It does not ride along with a board bring-up
fix. See [`branching.md`](branching.md).

## Rule, 2026-09-08: the fork stays a mirror and the bump is its own pass

**Maintainer's words:** track upstream closely and carry only small changes of our
own. Two consequences, and the second one is new:

- **Moving the SDK pointer is its own pass**, never folded into a CrossPoint sync.
  The other track has its own file and its own rules
  ([`upstream-crosspoint.md`](upstream-crosspoint.md)). The reason is diagnostic:
  the SDK is a library whose API only grows, so a bump either builds or does not,
  while a CrossPoint sync is a judgement call per commit. Bundled, a red build says
  nothing about which half broke.
- **A local SDK commit is a cost, and getting it upstream retires the cost.** The
  EPD config asserting the LoRa chip select went upstream as PR #73, **merged
  2026-09-03**, so the fork no longer carries it. **Corrected 2026-09-08:** this
  bullet used to end "nothing of ours in the SDK line we pin", which was already
  untrue when it was written -- `55a49587` had landed on `explorink` on 09-06.
  Assuming the fork is empty is how the pin walked off it; see "The pin can walk
  off the fork".

Measured 2026-09-08 through the GitHub compare API:

| | Value |
|---|---|
| Our `main` | `24003795`, 2026-09-02, **zero commits of its own** -- a plain ancestor of upstream `main` |
| Upstream `main` ahead of our `main` | 18 commits |
| `explorink` branch | one patch: `55a49587`, the `readFileToStream` watchdog fix. PR #73 is upstream and no longer carried |

### The pin has to sit on a branch of our own remote

**Trap found 2026-09-08, resolved the same day.** The pin `develop` carried at the
time, `cb9167d5`, was **9 commits ahead of our fork's `main`**, so it was on no
branch of `rfordinal/freeink-sdk`. Merging `cb9167d5` into `explorink` (giving
`955b2530`) fixed this as a side effect: the commit is now reachable from a
branch of our own remote. The trap is still worth knowing, because it comes back
the moment a pin is taken from upstream directly. It
resolves only because GitHub keeps a fork network in one object store -- a property
of the host, not of our repository. If that ever stops holding, a fresh clone fails
`git submodule update` with a commit-not-found error that names nothing useful.

The fix is one push, and it is the rule anyway: fast-forward our `main` to upstream
`main`, then pin a commit that sits on it. Nothing can be lost -- our `main` has no
commits of its own.

```
git -C freeink-sdk fetch upstream
git -C freeink-sdk push origin upstream/main:main     # fast-forward, ask first
```

**Done 2026-09-08.** Our `main` was fast-forwarded to upstream (`2400379..7f6bd0f`)
and the pin is now reachable from our own branch. It was verified as a real
fast-forward before the push -- `git merge-base --is-ancestor origin/main upstream/main`
held, so nothing of ours could be lost. Keep it that way: that one line is the check
before any future push.

### The 2026-09-08 bump, and what it cost

`e514a868` (2026-07-28) to `cb9167d5` (2026-09-04), 208 commits (`git rev-list
--count e514a868..cb9167d5`; the entry first said 217, corrected 2026-09-08). What it
is *for*:
the **X4 Classic board profile** (`Board::XteinkX4Classic`, SDK
`docs/xteink-x4c-support.md`, full pinout), which is what an S3 Xteink env needs.
It also brings our own PR #73 in from upstream, so the `explorink` patch stops being
load-bearing.

Evidence, all from a laptop, none from a device:

- **No header deleted, none renamed** (`git diff --name-status e514a868 cb9167d5 -- '*.h'`):
  68 headers change, additive, +8548 lines, mostly FreeInkUI list, tile-grid and
  sheet components we do not link. `BoardConfig.h` gains 780 lines.
- **All six envs build**: `default`, `gh_release`, `gh_release_rc`, `slim` (C3),
  `sticky` (S3), `simulator` (host).
- **Host tests 437 of 437**, 2.73 s.
- **Cost, measured by building the S3 env against each SDK commit in turn:**
  RAM +296 B, flash +21,920 B (+21 kB) for 208 commits.

| Env | Chip | RAM | Flash |
|---|---|---|---|
| `default` | ESP32-C3 | 18.0 %, 58,924 B | 61.5 %, 4,033,587 B |
| `gh_release` | ESP32-C3 | 16.1 %, 52,812 B | 57.5 %, 3,768,871 B |
| `gh_release_rc` | ESP32-C3 | 16.1 %, 52,812 B | 57.5 %, 3,768,867 B |
| `slim` | ESP32-C3 | 16.1 %, 52,788 B | 56.8 %, 3,720,817 B |
| `sticky` | ESP32-S3 | 19.2 %, 62,844 B | 55.0 %, 3,606,343 B |

**None of that says the firmware runs.** A bump is trusted once a C3 device boots,
draws a map and holds a BLE link on it.

### Checklist for the next bump

1. `git -C freeink-sdk fetch upstream`, then read what changed under
   `libs/hardware/BoardConfig`, `libs/display` and `docs/`.
2. Confirm no header was deleted or renamed.
3. **Bring `explorink` to the new base and pin that**, never the mirror commit
   itself: a pin taken straight from `main` drops every patch of ours (that is the
   2026-09-08 defect). Fast-forward our `main` to upstream first, then merge the
   new base into `explorink`. Check the result with
   `git -C freeink-sdk merge-base --is-ancestor origin/explorink <pin>`.
4. Build every env, run the host tests, record RAM and flash for one C3 and one S3
   env here.
5. Say in the commit message what the bump is **for**. A pointer move with no reason
   cannot be reverted with confidence.

## The pin can walk off the fork, and it did twice

Our patches live only on `explorink`. Nothing checks that the commit a firmware
branch pins is on that branch, so the pin can leave it silently. Both ways
happened and both were found on 2026-09-08.

**A commit that moves the gitlink without saying so.** `44fe2972` on
`release/lilygo-t5-s3-pro` (2026-09-07, *"feat(t5s3): the frontlight level is a
Settings row"*) moved that branch's pin backwards from `55a49587` to
`e514a868`, the base `explorink` forks from. Its body does not mention the
submodule at all. That dropped both patches on that branch, including the LoRa
chip-select fix BUG-037 had confirmed on hardware 2026-09-03. Restored and
verified on a T5 S3 Pro the next day. The restore is `089d1d42`, merged as
`1a816ff0`; the build it was confirmed with is the tag
`good/2026-09-08-t5s3pro-sdk-pin` and the archived binary
`docs/firmware-builds/2026-09-08-t5s3pro-sdk-pin-089d1d42.bin` in the parent
repo.

**What that hardware pass does and does not cover.** LilyGo T5 S3 Pro (MAC
`7c:2c:67:8a:4c:b4`), env `t5s3pro`, flashed over `/dev/ttyACM0`, hash verified.
The device booted to Home and the map screen drew live tile linework. That is an
SD **read**: tiles have one source in this firmware
(`src/activities/map/HalFileSource.cpp:13`, `Storage.open`) and nothing about the map
survives a reset (`src/activities/map/MapActivity.h:47`), so the frame cannot be
a stale panel or a cache. It does **not** cover SD **writes**, which is what
BUG-037 actually failed at (`ERR mkdir failed`).

**`readFileToStream` was then exercised, 2026-09-09, and it holds.** The device
on WiFi at `10.100.11.222`, `GET /trailink/base/13/4485/2842.tib` over WebDAV --
**the same 733 kB file that triggered the watchdog on 2026-09-06**, which is what
`55a49587` was written for:

| | |
|---|---|
| HTTP | 200, 732,765 bytes, matching the `PROPFIND` length exactly |
| Wall time | 9.6 s and 9.8 s over two runs, ~76 kB/s |
| Two GETs | bit-identical (`cmp`), so the read repeats and does not corrupt |
| Content | starts `TIB1`, 613,442 non-zero bytes of 732,765 |
| `uptime` from `/api/status` | 142 s before, 165 s after the first, 215 s after the second -- **continuous** |
| `freeHeap` | 123,032 B before, 123,016 B after |

A task-watchdog reset would have zeroed `uptime`, so the continuity is the
evidence, not the absence of an error message. The earlier failure sat in the
task for 16 s; these runs blocked for a fraction of that and never tripped it.

**A bump pass that lands on a mirror commit.** The 2026-09-08 SDK pass moved
`develop` from `e514a868` to `cb9167d5`, 208 commits -- deliberate and measured, for the X4
Classic board profile (parent `docs/PROGRESS.md`). But `cb9167d5` is on `main`,
not on `explorink`, so it dropped `55a49587`.

**It dropped one patch, not two, and the difference is the lesson.**
`merge-base --is-ancestor 94e19f73 cb9167d5` answers NO, which says only that
the *commit* is not an ancestor. The *change* was there: upstream had merged
our PR, and `LilyGoT5S3LgfxConfig.cpp` is byte-identical between the two. A
rebase of `explorink` onto `cb9167d5` says so out loud -- *"dropping 94e19f73
... patch contents already upstream"*. **An ancestry check answers a question
about history, not about content**; when it says NO, diff the files the patch
touched before calling anything lost.

**Two rules follow.**

- **A bump pass brings `explorink` to the new base and pins that**, never the
  mirror commit. The pass is not done while our patches are only on the old
  base.
- **A commit that moves the gitlink says so in its body**, with the old and new
  SHA and why. A gitlink is one line in a diff and `--stat` counts it as one
  changed file, so nothing else makes it visible in review.

Screen any pin against the fork before trusting it:

List the fork's own commits, then ask whether the pin contains each one:

```
git -C freeink-sdk rev-list origin/explorink --not origin/main --no-merges
git -C freeink-sdk merge-base --is-ancestor <each of those> $(git rev-parse HEAD:freeink-sdk)
```

Exit 0 for every one means the pin carries our patches. A commit that fails is
not automatically lost -- check whether upstream took its *content* before
concluding anything (that is how `94e19f73` looks missing and is not).

**Two earlier versions of this check were wrong, and both were written into
four documents before anyone ran them against a pin known to be bad.** Keep
them here, because the shape of the error repeats.

```
git -C freeink-sdk branch -r --contains <pin>                      # WRONG 1
git -C freeink-sdk merge-base --is-ancestor origin/explorink <pin> # WRONG 2
```

**Wrong 1 passes a bad pin.** Merging `cb9167d5` into `explorink` made
`cb9167d5` reachable from `origin/explorink`, so `--contains` lists our fork
branch for a pin with no patch of ours in it. Reachability is not containment
of a change.

**Wrong 2 fails a good pin.** It asks whether the pin contains the fork's
*current tip*, so `release/lilygo-t5-s3-pro` -- which pins `55a49587`, an older
fork tip that carries the patch perfectly well -- comes back FAIL. A branch is
allowed to pin an older fork commit.

Both mistakes are the same one: asking git a question about *history* when the
question is about *content*. When in doubt, skip the plumbing and look at the
code at that pin:

```
git -C freeink-sdk grep -c esp_task_wdt_reset <pin> -- libs/hardware/SDCardManager/src/SDCardManager.cpp
```

Run against every branch on 2026-09-09, the working check says:

| branch | pin | verdict |
|---|---|---|
| `develop` | `955b2530` | PASS |
| `release/lilygo-t5-s3-pro` | `55a49587` | PASS |
| `release/xteink-x4` | `e514a868` | FAIL -- genuinely has neither patch, untouched since 2026-08-22 |

**A pin an older branch still uses gets a tag before the fork branch moves.**
`release/lilygo-t5-s3-pro` pins `55a49587`, which was `explorink`'s tip and on
no other ref, so a rebase of the fork branch would have left it unreachable and
a fresh clone's `submodule update` would fail on it. It is kept alive as the tag
`pin/release-lilygo-t5-s3-pro-2026-09-08`. **In the end the rebase did not
happen** -- `explorink` merged instead, which keeps `55a49587` as a parent, so
the tag is insurance rather than the thing that saved it. Take the tag anyway:
the decision between merge and rebase came after.
