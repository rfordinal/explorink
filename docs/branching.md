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

- **Bring-up and feature work for that device forks from `release/<device>`,
  not from `develop`.** Use the normal worktree recipe (`CLAUDE.md`, "Every
  change goes in a worktree"), just change the base branch:

  ```
  git -C firmware/explorink worktree add ../../.worktrees/firmware/<topic> -b <topic> release/<device>
  ```

- **Merge finished feature branches into `release/<device>`**, same testing
  bar as any other merge (`CLAUDE.md`, "Never merge into a production branch
  untested. Ask.") — `release/<device>` is not exempt just because it is not
  `develop`.
- **Promote `release/<device>` into `develop` only when it is tip-top** — the
  device's bring-up is stable, not mid-experiment. This is a second, separate
  ask: merging into `develop` still needs a hardware test and the maintainer's
  go-ahead, same as any other merge into a production branch.
- **Sync the other direction too.** `develop` keeps moving while a device's
  release branch is being worked — core fixes, shared refactors, other
  devices' contributions. Periodically merge `develop` into `release/<device>`
  so the device branch does not rot behind it. Do this whenever picking the
  device work back up after a gap, not on a fixed schedule.

## Why not just feature branches off `develop`

A single GNSS probe, touch driver and panel init are each small, but together
they are one long stretch where the board does not reliably boot. Nothing
about that should reach a session working on X4 firmware from `develop`. The
release branch is the holding area; `develop` only sees the result.

## Existing branches

- `release/lilygo-t5-s3-pro` — created 2026-08-31 from `develop`.
