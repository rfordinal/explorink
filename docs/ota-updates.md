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
- **Open:** a real download-and-install over OTA has still never been run on a
  device. The check path is what this pass covers; `installUpdate()` and
  `FirmwareFlasher`'s board check are exercised only by the SD path and host
  tests.

## Release process note

Nothing here requires the publish process to change: the list endpoint reads
prereleases fine, so releases can keep being published as prereleases. But a
release that is **not** visible to an unauthenticated client -- a draft -- is
invisible to the device too. The asset naming convention the suffix match
depends on lives in the parent repo, `docs/public-release.md`.
