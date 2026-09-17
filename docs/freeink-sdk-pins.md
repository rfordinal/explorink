# Every move of the freeink-sdk pin

One row per commit that changes the `freeink-sdk` gitlink, newest first, per
branch. **Moving the pin is normal and usually deliberate** — this file is not a
gate, it is the record, and its job is to make the move visible after the fact.
Why that needs a file at all: the firmware repo stores no SDK files, only one
line saying which SDK commit to build against, so `git show --stat` renders any
SDK change as `freeink-sdk | 2 +-`, one changed file, whatever is behind it.

What a move obliges is in [`freeink-sdk-fork.md`](freeink-sdk-fork.md): a row
here, a commit body that names both SHAs, and **a full hardware pass rather than
a spot check**.

`python3 scripts/sdk_pin_check.py --from <ref> --to <ref>` prints the direction,
the span, and whether the patches on `origin/explorink` are in the new pin.

## `develop`

| Commit | Date | From → to | Span | Verified by |
|---|---|---|---|---|
| `60612aab` | 2026-09-08 | `cb9167d5` → `955b2530` | +1, our watchdog patch | Merged 2026-09-09 by maintainer exemption with `default` and `sticky` building only. **Exemption retired the same day**: the X3 arrived and took the first flash of our firmware on a C3, `develop` 92c949ae, env `default` -- it boots, names itself X3 off the fingerprint probe, mounts the SD card and drew Bratislava with 4609 ways (parent `docs/PROGRESS.md`, 2026-09-09, and `docs/devices/xteink-x3.md`). **Still not covered on a C3**: `readFileToStream`, the fix this pin exists for. That pass moved tiles over BLE at ~6 kB/s, not a large WebDAV GET, so the S3 remains the only board the watchdog fix has been exercised on. |
| `e06fca1b` | 2026-09-08 | `e514a868` → `cb9167d5` | +208 (the commit body says 217; corrected 2026-09-09) | Laptop only, and the entry said so: all six envs build, host tests 437/437, RAM +296 B / flash +21 kB on the S3 env. Deliberate, for the X4 Classic board profile. |

## `release/lilygo-t5-s3-pro`

| Commit | Date | From → to | Span | Verified by |
|---|---|---|---|---|
| `089d1d42` | 2026-09-08 | `e514a868` → `55a49587` | +2, both ours | T5 S3 Pro, env `t5s3pro`: boot to Home and a map frame, which is an SD read. Extended 2026-09-09 to the full pass — a 733 kB WebDAV GET (`readFileToStream`, twice, bit-identical, `uptime` continuous) and `MKCOL` + `PUT` + `GET` + `DELETE` for SD writes. Tag `good/2026-09-08-t5s3pro-sdk-pin`. |
| `44fe2972` | 2026-09-07 | `55a49587` → `e514a868` | **−2, both ours** | **Nothing, and nobody meant to move it.** The body is about a frontlight Settings row and does not mention the submodule. It cost three days of this branch building without the LoRa chip-select fix a hardware pass had confirmed on 09-03. This row is why the hooks exist. |
| `43394f0d` | 2026-09-06 | `94e19f73` → `55a49587` | +1, ours | T5 S3 Pro, the 733 kB WebDAV GET that had reset the board. The commit body names the move. |
| `e7b9a442` | 2026-09-03 | `e514a868` → `94e19f73` | +1, ours | T5 S3 Pro: the SD card mounts with the EPD config no longer claiming the LoRa chip select (parent `docs/BUGS.md`, BUG-037). |

## `release/xteink-x4-pro`

Forked from `develop` 2026-09-09 and carries its two rows unchanged
(`e06fca1b`, `60612aab`). Nothing has run on that board, so nothing on this pin
is verified there either.

## What is not in this file

Pin moves inherited from upstream CrossPoint's own history, before the fork had
branches of its own. `git log --format='%h %s' <branch> -- freeink-sdk` lists
them; several sit inside commits about unrelated UI work, which is the same
accident `44fe2972` repeated with our own hands.
