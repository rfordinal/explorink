# OTA firmware updates

How the device answers "is there a new firmware?", and the two reasons it
could not answer it at all until 2026-11-03.

## The path a user walks

Settings -> System -> **Check for updates**. The row starts
`OtaUpdateActivity` (`src/activities/settings/OtaUpdateActivity.cpp`), which:

1. turns on WiFi and launches `WifiSelectionActivity`,
2. calls `OtaUpdater::checkForUpdate()`,
3. draws one of three end states: **New update available** with the current
   and the new version, **No update**, or **Update failed**.

The confirmation screen is the answer to the question. Confirm (or a tap on
the Update half of the screen, on a touch board) starts the download and the
flash; Back cancels.

The other firmware path, **SD Card Firmware Update**, is unrelated to this
doc. It reads a `.bin` off the card and never touches the network.

## Wall 1: the menu row was hidden on every touch board

The row was pushed only under `!BoardConfig::hasTouch()`
(`src/activities/settings/SettingsActivity.cpp`), with a TODO saying touch
devices needed their own update path and artifacts first.

Both halves of that TODO were stale by 2026-11-03:

- `OtaUpdateActivity` draws and hits-tests Cancel/Update rects under
  `mappedInput.hasTouch()`, so the screen is usable by thumb.
- Per-board release assets have shipped since `explorink-x4pro-v0.2.0-alpha`
  (2026-09-09).

What the guard actually did was hide the row on **both** S3 boards. The X4 Pro
carries a GT911 digitizer exactly like the T5 S3 Pro
(`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h`, profile
`XTEINK_X4_PRO`), so `hasTouch()` is true there too. Only the C3 boards (X4,
X3, X4 Classic) ever saw the row. The guard is gone; the row is on every
board.

**Reading the source is not enough to find this.** The X4 Pro's `TouchConfig`
is written inline in its profile rather than as a named constant like the T5's
`LILYGO_T5_PRO_GT911`, so a grep for the named constant says the X4 Pro has no
touch. Grep `TouchController::` instead -- it lists every board that has one.

## Wall 2: `/releases/latest` is pinned to the oldest release (T-2053)

`checkForUpdate()` used to fetch
`https://api.github.com/repos/rfordinal/explorink/releases/latest`.

**GitHub's `/releases/latest` skips every release flagged "prerelease".**
Every ExplorInk release but the very first is flagged that way, so that
endpoint always resolved to `explorink-v0.1.0-alpha` (2026-08-22, C3 assets
only) no matter how many newer per-board releases existed. Per board:

| board | what it got | why |
|---|---|---|
| X4 / X3 (C3) | `NO_UPDATE` | 0.1.0-alpha is older than anything running |
| X4 Pro | `NO_UPDATE` | 0.1.0-alpha carries no S3 asset |
| T5 S3 Pro | `NO_UPDATE` | same |

So the answer was always "no update", on every board, forever.

### What it does now

It fetches the release **list**:

```
https://api.github.com/repos/rfordinal/explorink/releases?per_page=10
```

The list carries every release, prereleases included, newest first. Draft
releases are not visible to an unauthenticated client, which is what we want.
`ReleaseJsonParser` in **list mode** walks it and keeps the first entry whose
assets carry a name ending in this board's suffix
(`OtaUpdater::expectedAssetSuffix()`):

| board tag | asset suffix |
|---|---|
| `x3x4` | `-esp32c3-firmware.bin` |
| `x4pro` | `-x4pro-esp32s3-firmware.bin` |
| `t5s3pro` | `-t5s3pro-esp32s3-firmware.bin` |

A board with no published asset (`sticky`, T-2003) returns `nullptr` and OTA
reports `NO_UPDATE` without a fetch.

`per_page=10`: one release round publishes one entry per board, so ten entries
cover three full rounds -- enough slack for a board that sat out a round --
while bounding the response.

### List mode, and why one array level costs more than it looks

The list is the single-release shape one array level deeper, so every depth
test in `ReleaseJsonParser` shifts by one (`releaseDepth()`). Two things do
not fall out of that for free:

- **A release with nothing for this board must be rolled back whole.** Its
  `tag_name` arrives before its `assets`, so by the time we learn the release
  is useless the tag is already stored. Left there, the scan ends holding the
  **oldest** release's tag with no firmware -- which reads exactly like "found
  a release, it just had no asset for you" and hides the real reason. The tag
  is cleared at the release object's closing brace when no asset matched.
- **The release that supplied the firmware is locked** (`releaseLocked`), so a
  later -- always older -- entry cannot overwrite its tag. The lock is at
  release level, not at `firmwareFound`, so the matching release's own tag is
  still captured if `tag_name` were ever to arrive after `assets`. Today's API
  puts it before, but key order is not part of GitHub's contract.

### Aborting early

`isComplete()` goes true once a tag and a matching asset are both in hand, and
`checkForUpdate()` returns `false` from the HTTP data callback to stop the
transfer there. GitHub orders a release object's keys with `tag_name` (8th)
and `assets` (17th) ahead of the long `body` (last), so everything needed is
already read and the rest -- the release notes, plus every older release -- is
dead weight.

`HttpDownloader` cannot tell a deliberate stop from a dropped connection and
reports both as a failed fetch, so `checkForUpdate()` records **why** it
stopped and only treats an unplanned failure as `HTTP_ERROR`.

Measured on the host against the live API response, 2026-11-03, 512-byte
chunks, full list 148 kB:

| board | resolves to | bytes read |
|---|---|---|
| x3x4 | `explorink-v0.2.2` | 22 kB |
| x4pro | `explorink-x4pro-v0.2.2` | 8 kB |
| t5s3pro | `explorink-t5s3pro-v0.2.2` | 37 kB |
| a board with no asset | nothing | 148 kB (whole list) |

Almost all of the 148 kB is release-note `body` text.

## The confirmation screen on a touch board

The screen draws its own half-width **Cancel** and **Update** rects along the
bottom 80 px, and separately asks the theme for the four button hints. Which
of the two a board shows is decided by the touch mode (`src/TouchPolicy.h`),
and the two predicates are disjoint on purpose:

| touch mode | `touchAnywhere()` | `hintsVisible()` | what is drawn |
|---|---|---|---|
| no digitizer | false | true | hint boxes only |
| ANYWHERE | true | false | the Cancel/Update rects only |
| BUTTONS\_ONLY | false | true | hint boxes only |
| DISABLED | false | false | the padlock only |

**Guarding the rects on `mappedInput.hasTouch()` is wrong and was the bug
(T-2059).** That call answers "is there a digitizer", not "may a tap anywhere
do anything". In BUTTONS_ONLY, `MappedInputManager::wasScreenTapped()` returns
false outside the hint boxes, so the rects were drawn, invited a finger, and
did nothing -- sitting on top of the hint boxes, which the theme draws in
exactly that mode and into exactly that 80 px. A board in DISABLED got the same
two dead buttons beside the padlock.

**Fixed and confirmed on an X4 Pro 2026-11-03**, which is in BUTTONS_ONLY: the
confirmation screen now shows the hint boxes alone, nothing over them.

Draw a touch-only control under the same predicate that gates its tap.
`BaseTheme.h` states the rule from the other side -- "a hit area that is not
the box the user can see is a lie they aim at" -- and a drawn control that is
not a hit area is that lie reversed.

## Deciding "newer"

`ota_version::isVersionNewer()` (`src/network/OtaVersionCompare.cpp`) compares
the running `TRAILINK_VERSION` against the release `tag_name`. It finds the
version inside a tag by the `-v<digit>` marker, not by the first digit in the
string -- `explorink-x4pro-v0.2.2` contains a `4` in `x4pro` long before the
real version starts, and a first-digit scan parses that as major version 4 and
reports an update that does not exist.

Equal numeric versions are "newer" only when the running build is an RC.

## State

- Host-tested: 44 tests in `test/release_json_parser/`, 11 of them list mode,
  plus `test/ota_version_compare/`.
- Host-tested against the live GitHub response for all three boards.
- **Confirmed on an X4 Pro, 2026-11-03, both answers.** An x4pro build with
  its version string lowered to 0.2.1 reached `Settings -> System -> Check for
  updates`, connected to a saved network, and drew `New update available! /
  Current Version: 0.2.1-x4pro / New Version: explorink-x4pro-v0.2.2` -- the
  list scan, over real WiFi and TLS, naming this board's own newest release.
  The same branch built at the real 0.2.2 then drew **No update available** on
  the same board.

  The negative on its own proves nothing: the broken code said exactly that
  too, on every board, forever. It is the positive run that carries the
  evidence, and the negative that confirms the version compare -- one build,
  two version strings, one board, nothing else different.
- **Download and install proven on an X4 Pro, 2026-11-03** -- the first time
  on this project. A board flashed over USB with an 0.2.1-labelled build
  pressed Update, pulled the 3.93 MB
  `explorink-0.2.2-x4pro-esp32s3-firmware.bin` from GitHub over wolfSSL,
  wrote it to the inactive slot, switched boot and restarted into it.

  **"Settings says 0.2.2" is not the proof, and it was worth not stopping
  there.** The slot the image lands in (`app1`) held an older build from a
  past session, so a boot flag flipping to a stale 0.2.2 image produces the
  same version on screen with no download at all. What settles it is the
  256-byte `esp_app_desc` read back out of `app1`
  (`esptool read-flash 0x650020 256`) compared against the same 256 bytes of
  the published asset: byte-identical, and different from the branch's own
  build (`project_name` `v022-x4pro` against `ota-reachable`). The device is
  running exactly what it downloaded.

  That run also exercised the board checks on the install path for real: the
  `chip_id` test after the first 14 bytes and the streamed board-tag scan
  both passed on an image independently confirmed to carry
  `EXPLORINK-BOARD-V1:x4pro;` and `chip_id 0x0009`.
- **Repeated on a T5 S3 Pro the same day, and that is the run that covers the
  list scan.** On the X4 Pro the matching release is **entry 0** of the list,
  so the skip-and-roll-back path -- the part that needed new code -- never ran
  on hardware at all. The T5 S3 Pro has to discard `explorink-x4pro-v0.2.2`
  and `explorink-v0.2.2`, tags included, before it reaches its own. Its screen
  named `explorink-t5s3pro-v0.2.2`, which is the proof: **without the rollback
  it would have shown entry 0's tag beside entry 2's asset.** It then
  downloaded and installed that release, and the `esp_app_desc` read back out
  of its `app1` matches the published t5s3pro asset byte for byte
  (`project_name` `v022-t5s3` against the branch's `ota-reachable-t5s3pro`).

  The touch fix holds there too: the confirmation screen on that board, a
  540x960 panel also in BUTTONS_ONLY, shows the hint boxes alone.
- **Still open:** no failure path has been exercised. A wrong-board image, a
  connection dropped mid-write, and a corrupt image are all handled in code
  (`esp_ota_abort()`, and boot is switched only after `esp_ota_end()`
  verifies) and none of the three has been made to happen on a device.

## Release process note

Nothing here requires the publish process to change: the list endpoint reads
prereleases fine, so releases can keep being published as prereleases. But a
release that is **not** visible to an unauthenticated client -- a draft -- is
invisible to the device too. The asset naming convention the suffix match
depends on lives in the parent repo, `docs/public-release.md`.
