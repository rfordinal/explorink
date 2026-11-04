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

## Upstream solves this differently, and that explains our problem

CrossPoint still fetches `/releases/latest` (`upstream/develop ec17c4ed`,
`src/network/OtaUpdater.cpp:22`) and it works for them. The difference is not
the code, it is how each project publishes:

| | CrossPoint | ExplorInk |
|---|---|---|
| releases per round | one, carrying every board's asset | **one per board** |
| prerelease flag | RC builds only; stable releases are not flagged | **everything except the first** |
| tag | `2.0.0` -- the version itself | `explorink-x4pro-v0.2.2` -- not the version |
| asset match | builds the exact filename from the tag (`crosspoint-<tag>-x3-x4.bin`) and switches the parser to it mid-stream | suffix match |

Each of those three differences on its own breaks `/releases/latest` for us,
and all three are ours. **So the list scan is a workaround for our publishing
process, not a property of GitHub.** One release per round holding all three
assets, not flagged prerelease, would make `/releases/latest` correct again and
list mode unnecessary.

Worth knowing before anyone "fixes" the publishing side: changing it does not
make this code wrong, it makes it redundant. Their derive-the-filename-from-the-tag
trick is not available to us either way, because our tag does not carry the
version in the form the filename uses.

## The C3 installs about one run in five (T-2064)

Measured on an X3, 2026-11-04. The check works and offers `explorink-v0.2.2`.
The download usually dies partway through:

```
[SecureClient] read failed: -125, free heap 25876
[ERR] [HTTP] wolfSSL incomplete: got 61810 of 4105888 bytes
[ERR] [OTA] Firmware install failed (download)
```

`-125` is `MEMORY_E`, "out of memory error"
(`Arduino-wolfSSL/src/wolfssl/wolfcrypt/error-crypt.h:72`). Both handshakes
succeed before it (395 ms to github.com, 160 ms to release-assets after the
redirect), so it is neither the handshake nor the board check. It does
sometimes get all the way through -- roughly one attempt in five.

### max_fragment_length does not help, and the flag was taken back out

RFC 6066's `max_fragment_length` asks the peer to cap TLS records at 2 kB
instead of 16 kB. `SecureClient.cpp` has always requested it, but under
`#ifdef HAVE_MAX_FRAGMENT`, and that macro is defined in no build flags of
ours -- so the request has always been compiled out.

Defining it temporarily and logging what the peer agreed to answers the
question in one run. Measured on the X3, 2026-11-04:

```
[SecureClient] max_fragment: 2048  (MAX_RECORD_SIZE 16384)   <- api.github.com
[SecureClient] max_fragment: 16384 (MAX_RECORD_SIZE 16384)   <- release asset host
```

Repeated on the X4 Pro and a T5 S3 Pro with the same two answers, so the
behaviour belongs to the server and not to the chip.

**The API host accepts it; the asset host does not.** Small records would make
the cheap request cheaper and do nothing for the expensive one, so the flag
buys nothing on the path that fails. It was removed again rather than shipped:
in the pinned wolfSSL (Arduino-wolfSSL 5.7.2) the macro also swaps the
record-layer length check from a fixed `MAX_RECORD_SIZE` to `ssl->max_fragment`
(`src/src/internal.c:11473-11480`), which is harmless when the peer ignores the
extension -- `internal.c:7440` defaults the field to `MAX_RECORD_SIZE` -- and a
new hard `LENGTH_ERROR` for any peer that accepts it and then sends a larger
record. Putting that on every TLS connection on every board, for no measured
gain on the one path it was meant for, is not a trade worth making.

### The heap during the download, measured

The failure was first read as fragmentation, because the board enters the
download with 47.8 kB free and a largest block of only 27.6 kB. Tracing the
heap every 10 % of the transfer shows something simpler. Same board, same run,
`Total: 244,356 B`:

| point | free | largest block |
|---|---|---|
| Home, WiFi off | 108,720 | 30,708 |
| WiFi up, confirmation screen | 47,784 | 27,636 |
| download at 0 % | 3,668 | 2,036 |
| download at 50 % | 6,144 | 3,188 |
| download at 90 % | 8,624 | 5,620 |
| download finished | 35,960 | 19,444 |

**The download itself consumes almost the whole heap.** Through the transfer
there are 3.6 to 8.6 kB free with a largest block of 2 to 5.6 kB, so any
transient allocation above a few kB fails. That is the `MEMORY_E`. The earlier
reading that "nothing drains during the transfer" was taken before this trace
existed and was wrong.

### One run proves nothing here

Where the download dies varies enormously between attempts of the *same*
build: measured failures at 32 kB and at 4.1 MB, which is the whole file. So a
build that completes one download has not been shown to fix anything, and a
build that fails one has not been shown to be worse. Any claim about this path
needs a run of five.

This was learned by publishing a "50x improvement" off one sample per build,
2026-11-04. It was not supported.

### The fix: resume instead of restart

`runGetWolf` (`src/network/HttpDownloader.cpp`) keeps every byte already handed
to the sink and asks for the rest with `Range: bytes=N-`. A dead connection
then costs a reconnect, not the download.

**Measured on the X3, 2026-11-04: five installs in a row, all completed.**

| install | resumes | wall clock after the confirm |
|---|---|---|
| 1 | 9 | 47 s |
| 2 | 3 | 38 s |
| 3 | 3 | 10 min 28 s |
| 4 | 10 | 5 min 46 s |
| 5 | 5 | 49 s |

**Every one of them needed it**; none completed on a single connection. So the
resume is not a safety net here, it is the mechanism. The wall-clock spread is
the WiFi link and the panel repaint, not the retries -- a reconnect is one
handshake, about 600 ms.

#### Why a reconnect is the right answer and not merely a retry

At each handshake the largest free block reads 19 to 28 kB, which is enough for
a 16.4 kB record, and it is gone by the next one. The failing condition belongs
to the connection, not to the board, so a fresh connection restores exactly the
precondition that failed. That is also upstream's diagnosis: wolfSSL frees its
grown input buffer after every record and cannot get an equivalent block back.

#### What makes it sound, and the three things that are checked

Resuming is only correct because every sink on this path is append-only --
`esp_ota_write`, a file write, a string append -- and none can seek, so
`sink.downloaded` is the exact resume point. That argument holds only if the
server's answer really starts there, so three things are checked rather than
assumed:

- **A 206's `Content-Range` must begin exactly at the requested offset.** No
  sink can seek, so a range starting anywhere else would be spliced in at the
  wrong place. A 206 with no usable `Content-Range` is refused too.
- **A 200 in answer to a Range request** means the server ignored it and
  restarted the body at zero. Fatal, not another attempt.
- **Completion needs the bytes, not the server's word for it.** RFC 7233 lets a
  server satisfy `bytes=N-` with less than the remainder, so "the announced
  range arrived whole" no longer implies a whole body. Without the
  `downloaded >= total` check a short range reports success and the caller keeps
  a truncated file -- on the OTA path only `esp_ota_end`'s SHA-256 would have
  caught it, and on the font path nothing would.

Two bounds also had to be right, and the first version had both wrong:

- **A reconnect that delivers nothing is not the end.** Late in a transfer the
  connection often dies on the first record after the handshake. Giving up on
  the first barren attempt stopped a download at 3,661,739 of 4,105,888 bytes.
  Three barren attempts in a row now end it; one between two productive ones
  costs nothing.
- **Redirects count per attempt, not across the transfer.** A signed-redirector
  answers 30x on every new connection, and a budget shared with the resumes
  blames "too many redirects" for a download that merely reconnected.

A signed CDN URL can also expire mid-transfer, so a 403 or 404 while resuming a
redirected URL goes back to the start URL to mint a fresh one. The `Range`
header is sent either way, so nothing already received is refetched. The same
status on the start URL is an ordinary failure. **Not observed on hardware** --
the longest run above was 12 minutes and never hit it.

#### The decision is a pure function with host tests

`src/network/DownloadResumePolicy.h` holds the whole after-each-attempt
decision as `decide(Attempt, State&) -> Step`, and `test/download_resume/`
exercises it. A short range, a barren reconnect, an expired signature and a
server that ignores `Range` are precisely the transitions a real download will
not reproduce on demand; the alternative was a four-megabyte download on a C3
per case.

#### What is not fixed

The download URL is logged as `scheme://host` only. GitHub's release CDN signs
it with `X-Amz-Signature`, so the full string is a bearer credential and does
not belong in a log on a device this project assumes can be stolen.

Two things the resume does **not** address, both pre-existing:

- `SecureClient::setInsecure()` is in force on this path
  (`SecureClient.cpp:87`, `WOLFSSL_VERIFY_NONE`), so there is no TLS peer
  verification. The resume adds a seam where two halves of a file could come
  from different answers; the `Content-Range` check closes the accidental
  version of that and not the deliberate one. No `If-Range` validator is sent.
  T-2067.
- On the OTA path the only integrity backstop is `esp_ota_end`'s SHA-256, which
  proves the image is self-consistent, not that it is ours. There is no
  signature check, and the board-tag check runs only on S3
  (`OtaUpdater.cpp:250`).

### The S3 boards: one connection, no resume, and the contrast that confirms it

Both S3 boards ran the same build and the same walk, 2026-11-04. Neither
needed a single reconnect. (That build still carried the temporary
`HAVE_MAX_FRAGMENT` flag described above, which the asset host ignored, so the
download itself ran under the same 16 kB records as the shipped code.)

| board | resumes | wall clock | free during the download | largest block |
|---|---|---|---|---|
| X4 Pro | 0 | 33 s | 174,248 | 126,964 |
| T5 S3 Pro (board B) | 0 | 40 s | 108,864 | 63,476 |

Both numbers are **flat across the whole transfer** -- the same reading at
0 %, 50 % and 100 % -- against the C3's 3.6 to 8.6 kB free and a largest block
that moves between 2 and 5.6 kB. That is the contrast the C3 diagnosis rests
on, and it is measured now rather than reasoned from "the S3 has PSRAM".

The T5 S3 Pro was the second board on purpose: its release sits third in the
GitHub list, so the scan walks past `explorink-x4pro-v0.2.2` and
`explorink-v0.2.2` to reach `explorink-t5s3pro-v0.2.2`, which is what its
confirmation screen named. The X4 Pro's own release is first in that list and
puts the scan through nothing.

### What the retries cost, and why layer two still matters

The resumes are not evenly spread. Early in a download an attempt carries
hundreds of kB; past about 3.5 MB an attempt often carries exactly one 16 kB
record and dies. At the handshake the largest free block reads 19 to 28 kB --
enough for a 16.4 kB record -- and it is gone by the next one. That is
upstream's diagnosis word for word: wolfSSL frees its grown input buffer after
every record and cannot get an equivalent block back.

So the resume makes the download finish; it does not make it cheap. Each
reconnect is a full TLS handshake.

**This is the same order upstream took.** `freeink-sdk` `118c6723` describes
downloads that "failed with MEMORY_E and fell back to many Range retries" --
their `fetchResumable` already resumed, and the commit adds a *second* layer
on top: a full-record buffer reserved right after the handshake, while a large
block is still free, lent to wolfSSL through an allocator hook
(`setReserveFullRecord`). They skip it when the peer accepted
`max_fragment_length` -- which, per the measurement above, our asset host does
not, so that layer would apply to us too.

We do not have either piece: our SDK pin is 339 commits behind and
`freeink::fetchResumable` does not exist in it. The Range resume above is our
own, in our own `HttpDownloader`. The allocator hook is **not** ported: a first
attempt at it crashed the X3 outright (`std::bad_alloc` in `readLine`,
`docs/crashes/2026-11-04-x3-ota-tls-reserve/` in the parent repo) and it is
the harder half. T-2064 tracks the rest.

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
- **Repeated on a T5 S3 Pro the same day.** Its release is third in the list,
  so it walks past `explorink-x4pro-v0.2.2` and `explorink-v0.2.2` to reach
  its own. Its screen named `explorink-t5s3pro-v0.2.2`, it downloaded and
  installed that release, and the `esp_app_desc` read back out of its `app1`
  matches the published t5s3pro asset byte for byte (`project_name`
  `v022-t5s3` against the branch's `ota-reachable-t5s3pro`).

  The touch fix holds there too: the confirmation screen on that board, a
  540x960 panel also in BUTTONS_ONLY, shows the hint boxes alone.

### What each run actually proved, and what neither did

This was written down wrong first. The T5 S3 Pro screen naming its own tag was
reported as proof of the **rollback**, and it is not: each release's `tag_name`
overwrites the previous one anyway, so the last-matching case comes out right
with the rollback removed. Checked by compiling three variants of the parser
against the same input -- unmodified, rollback removed, lock removed:

| variant | match is **last** (t5s3pro) | match is **first**, two older behind (x4pro) |
|---|---|---|
| unmodified | `t5s3pro-v0.2.2` | `x4pro-v0.2.2` |
| rollback removed | `t5s3pro-v0.2.2` | `x4pro-v0.2.2` |
| lock removed | `t5s3pro-v0.2.2` | **`x4pro-v0.2.1-alpha`** |

So the two guards do different jobs and neither is what the hardware showed:

- **`releaseLocked`** stops a later -- always older -- release overwriting the
  matched tag. It only bites when the match is **not last**.
- **The rollback** only changes the outcome when **nothing matches**: it turns
  a leftover tag with no asset, which reads as "found a release, it had
  nothing for you", into a clean `NO_UPDATE`. It executed twice on the T5 S3
  Pro and changed nothing observable.

| run | what it proved |
|---|---|
| X4 Pro | the depth shift on real data, the early abort, the whole path including install |
| T5 S3 Pro | walking past two non-matching releases to a later match |
| neither | the rollback's effect -- needs a board with no published asset (`sticky`) |
| neither | the lock's effect -- see below |

**The lock may not be reachable in production at all.** The early abort stops
parsing inside the matching release, so on the live list an X4 Pro never reads
the older `explorink-x4pro-v0.2.1-alpha` that would exercise it. It is a guard
for the case where the abort does not land in time, because the check runs per
chunk and a chunk can carry more than one release. Tracked as T-2062: prove
both guards on hardware, or drop the one that turns out to be dead.

The host test `NoReleaseForThisBoardLeavesNothingBehind` does catch a missing
rollback (`foundTag` comes back 1 instead of 0), so the suite is not vacuous --
that was checked the same way.
- **One failure path has now run, by accident:** the C3 download above. The
  device behaved correctly -- `app0` untouched, "Update failed" on the panel,
  a retry possible with nothing reset. What it could not do is say *why*
  (T-2065).
- **Still open:** no failure path has been exercised deliberately. A wrong-board image, a
  connection dropped mid-write, and a corrupt image are all handled in code
  (`esp_ota_abort()`, and boot is switched only after `esp_ota_end()`
  verifies) and none of the three has been made to happen on a device.

## Release process note

Nothing here requires the publish process to change: the list endpoint reads
prereleases fine, so releases can keep being published as prereleases. But a
release that is **not** visible to an unauthenticated client -- a draft -- is
invisible to the device too. The asset naming convention the suffix match
depends on lives in the parent repo, `docs/public-release.md`.
