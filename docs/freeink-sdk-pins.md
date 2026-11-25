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
| `86db027b` | 2026-11-25 | `154d57ee` → `824713ce` | +503 upstream | **Xteink X3** (`7C:E8:B1:6F:C2:9C`), env `default`, build `sdk-pin-2087 / 0.2.3-dev-t2087/sdk-pin-ad7176eb`, archived `-good`. The full SDK set, and it closes the C3 gap the `60612aab` row below had held open since 2026-09-09: boot and hardware detect, a map frame reading 474 kB of tiles off the card, `MKCOL` + `PUT` + `GET` + `DELETE`, and **`readFileToStream` on a C3 at last** -- the same 732,765-byte `2842.tib` as the S3 pass, twice, bit-identical, `uptime` 604 → 705 → 801 with no reset. **Runtime cost measured too**: the map is untouched (redraw 380 ms both pins, render and card time inside the spread) and only Home's first paint after a boot moves, 380 ms to 1,797 ms -- once per boot, matched to upstream `c27da30d` by its commit message and the file it touches -- **read, not bisected**. Wake to Home judged by eye on both pins: no imprint either way. **T5 S3 Pro, both boards**, env `t5s3pro`: same set, plus the panel by eye -- board A clean, board B a small imprint **on both pins**, so a unit difference and not this bump. **Not covered**: Trips, Sync, the Settings tabs and the map Options menu; `env:x4pro` has no board; and the X4 half of `env:default` has no board at all (lost 2026-08-22). All nine envs build with no warning from our tree or the SDK, host tests 742/742 in 1.5 s. Cost, two builds with the pin as the only difference: `default` (C3) RAM +312 B / flash +43 kB, `t5s3pro` (S3) RAM +72 B / flash +13 kB. For the SecureNet TLS record reserve (parent T-2064) and `BatteryMonitor::loadDesignCapacity()` (parent T-2076). Parent T-2087. |
| — | 2026-09-09 … 2026-09-18 | `955b2530` → `154d57ee` | +9, all ours | **Eight moves with no row here.** The T5 S3 Pro waveform work moved this gitlink inside commits about waveform tables (`75eb1ea2`, `d9d00325`, `47382303`, `7c7e30a9`, `7db8415d`, `943c7739`, `d13b7c60`, `28c8270c`) and this file was not touched once. Each move was deliberate and each commit body is about the panel, which is exactly the `44fe2972` shape below with the intent reversed: the move was meant, the record was not kept. It also walked `develop` off `origin/explorink` and onto the fork branch `t5s3-fast-lut`, where it stayed until 2026-11-25. Reconstructed from `git log -- freeink-sdk`, not from anybody's notes. |
| `60612aab` | 2026-09-08 | `cb9167d5` → `955b2530` | +1, our watchdog patch | Merged 2026-09-09 by maintainer exemption with `default` and `sticky` building only. **Exemption retired the same day**: the X3 arrived and took the first flash of our firmware on a C3, `develop` 92c949ae, env `default` -- it boots, names itself X3 off the fingerprint probe, mounts the SD card and drew Bratislava with 4609 ways (parent `docs/PROGRESS.md`, 2026-09-09, and `docs/devices/xteink-x3.md`). **Covered on a C3 at last, 2026-11-25**, by the `86db027b` row above: the same 732,765-byte file, twice, bit-identical, `uptime` continuous across a 100 s transfer. Two and a half months after this row first said it was open. The C3 moved this file at **~7.3 kB/s**. A T5 S3 Pro moved a *different* 683,936-byte file over the same WiFi the same day at **~104 kB/s**. Different file, different card, different chip: that pair is **two readings, not a ratio**, and nothing measured here says why they differ. No C3 baseline against the old pin was taken either, so this says nothing about the bump. |
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
