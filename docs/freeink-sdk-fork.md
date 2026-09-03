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
  Today it is one commit: the T5 S3 Pro EPD config no longer asserting the LoRa
  radio's chip select (`Free-Ink/freeink-sdk#73`).

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
