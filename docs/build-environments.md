# Build environments, and which one you can publish

`platformio.ini` carries five environments. They are not five flavours of the
same firmware: two of them are missing the feature the map depends on. This doc
says which is which, because the first public release nearly went out of the
wrong one.

Verified by reading `platformio.ini` on 2026-08-22. What is measured on hardware
is marked as such.

## The environments

| Env | MCU | Device flags | BLE peripheral | Serial log | What it is for |
|---|---|---|---|---|---|
| `default` | ESP32-C3 | X4 + X3 | **yes** | on, `LOG_LEVEL=2` | development, and every flash this project has ever done |
| `gh_release` | ESP32-C3 | X4 + X3 | **no** | on, `LOG_LEVEL=1` | intended for releases. See the warning below |
| `gh_release_rc` | ESP32-C3 | X4 + X3 | **no** | on, `LOG_LEVEL=1` | release candidate, same gap |
| `slim` | ESP32-C3 | X4 + X3 | **no** | off | size experiments |
| `sticky` | ESP32-S3 | Seeed Sticky | **no** | on | a different MCU family, one binary per family |
| `t5s3pro` | ESP32-S3 | LilyGo T5 S3 Pro | **yes** | on, `LOG_LEVEL=2` | bring-up on the non-Xteink validation board. Adds `CMD:LIGHT` and `CMD:GNSS`, neither of which is in any other env. See [`lilygo-t5s3-bringup.md`](lilygo-t5s3-bringup.md) and [`gnss.md`](gnss.md) |
| `x4pro` | ESP32-S3 | X4 Pro | **yes** | on, `LOG_LEVEL=2` | bench work on the X4 Pro. Carries `CMD:SETTING`, `CMD:BUTTON` and `CMD:TOUCHLOG`, so it is **not publishable** |
| `gh_release_x4pro` | ESP32-S3 | X4 Pro | **yes** | on, `LOG_LEVEL=1` | the X4 Pro build that goes on GitHub and into the browser flasher. Added 2026-09-14 |

## What a published build must not contain, and what two of them did

**Three serial commands are bench instruments and every one of them is a way to
hurt the person who lost the device.** The device gets lost or stolen, and USB
and BLE share one unauthenticated command grammar (`MapCommandParser.h`, P3 and
P5), so anyone holding it, or in radio range, reaches whatever is compiled in.

| Flag | What it gives whoever picked the device up |
|---|---|
| `ENABLE_SETTING_CMD` | writes persisted settings: `mapDebugInfo` paints the rider's exact position on the panel, `mapAutoSyncTiles` and `mapTileFreshnessMode` spend their mobile data. The write survives a reboot |
| `ENABLE_BUTTON_CMD` | injects button presses, reaching every screen the rider can |
| `ENABLE_TOUCHLOG_CMD` | read-only, but it blocks the loop for up to eight seconds, which freezes the screen |

**Measured 2026-09-14, on the published images fetched back off GitHub**, with
`strings`:

| Release | `SETTING_OK:` | `BUTTON_OK` | Built from |
|---|---|---|---|
| `explorink-x4pro-v0.2.0-alpha` | **present** | **present** | `env:x4pro`, the bench env |
| `explorink-v0.2.0-alpha` (C3) | **present** | absent | `env:default`: its version string is `0.2.0-dev-v4-release-a9d7f244`, which only `scripts/git_branch.py` produces |

`ENABLE_TOUCHLOG_CMD` is absent from both only because it did not exist when
they were cut.

**Why the C3 release was built from a bench env at all, and it is not
carelessness:** `gh_release` has no `nimble_dep`, so it has no BLE peripheral,
so it cannot receive a position or a tile from the phone. The only C3 env that
can do the product's own job is `default`, and `default` carries the backdoor.
**The release env is unusable, so the dev env shipped.** That is the defect;
the backdoor is its symptom.

`gh_release_x4pro` is the shape that fixes it: `base` plus `nimble_dep`, the
device flags, `LOG_LEVEL=1`, and none of the three bench flags. The C3 side
still needs the same treatment -- see T-2011 in the parent repo's
`docs/TODO.md`.

**Check a release before publishing it, against the image and not the branch:**

```
strings -n 4 firmware.bin | grep -E 'SETTING_OK:|BUTTON_OK|TOUCHLOG'
```

Nothing printed is the pass. This is the same rule the flash procedure already
has -- a claim about what a build contains is about the binary, not the branch.

`TRAILINK_VERSION` is set explicitly in every env except `default`, where
`scripts/git_branch.py` derives it from the branch and short SHA.

## Where `custom_sdkconfig` actually lands

`[base]`'s `custom_sdkconfig` (`platformio.ini:88`) is not a set of compiler
defines. It **rebuilds the Arduino core libs from source**, and the result is
written into `~/.platformio/packages/framework-arduinoespressif32-libs/<chip>/`,
replacing what the package shipped.

Two consequences.

**Options that only the core carries are real only if that rebuild ran.** The
~32-37 kB heap reclamation is in that class (`map-memory.md`), and so is
`CONFIG_PM_ENABLE` if it is ever set. Whether every environment triggers a
rebuild is open -- parent `docs/TODO.md`, T-256 -- and it is answered from a
build log.

**That directory is shared, unversioned, mutable state.** One copy serves every
worktree, branch and session on this machine, and it has **no version suffix in
its name**, so a modified copy keeps reporting the stock version forever. A
build for one chip leaves the other chip's subtree alone until something
rewrites it. Observed 2026-09-05: the `esp32c3` and `esp32s3` subtrees swapped
which one looked freshly built inside one hour, because a parallel session
built. So **never cite it as evidence about a particular build** (a claim off it
names the date and the environment), and **never repair it in place** -- delete
the whole directory and let PlatformIO fetch it again.

**A second observed symptom, 2026-09-06/07**: `env:default` failed with
`lib/hal/HalStorage.h:3:10: fatal error: Print.h: No such file or
directory` in a file (`BmpViewerActivity.cpp`) untouched by the change being
built. An immediate retry with zero code changes succeeded. Plausible --
matches this pattern, several other sessions were building the same shared
framework directory at that exact time -- but not independently
instrumented (no timestamp check at the failure moment). If it recurs: retry
once before treating it as a real code problem.

A library compiled **per environment** out of `.pio/libdeps/<env>/` does not
need the core rebuild at all: it picks the config up from the generated
`sdkconfig.defaults` when it compiles. NimBLE-Arduino is the one that matters
here, which is why `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517` is measured working
on the T5 S3 Pro -- 13.6 kB/s against 3.9 kB/s at MTU 256, same board, same day
(parent `docs/ble-map-transfer-protocol.md`, "Measured 2026-09-03") --
regardless of any of this.

## `gh_release` has no Bluetooth, and that is not a regression

`FREEINK_CAP_BLE_PERIPHERAL=1` and the `h2zero/NimBLE-Arduino` dependency are
declared **only** in `[env:default]` (`platformio.ini:210-231`). The comment
right above them says so: "scoped to this dev env only ... not yet added to
gh_release/slim/sticky".

What a `gh_release` binary does: boots, reads the SD card, draws a map, answers
`CMD:` over USB serial. What it cannot do: anything involving a phone.
`lib/BlePositionServer/src/BlePositionServer.cpp:1020` is the `#else` half of the
file, stub bodies with no BLE code linked, so there is no position receiver, no
tile transfer, no pin channel and no app wake.

That means:

- **No GPS.** The device has no receiver of its own; the phone is the receiver.
- **No map squares arriving.** Missing tiles are fetched over BLE.
- **A map that only ever shows what is already on the card.**

Nobody noticed for months because every build that reached a device was a
`default` build, and so is the published one
(`../../docs/public-release.md` in the parent repo). Fixing it is a one-line
build-flag change plus the `lib_deps` entry, and it cannot be called fixed until
a `gh_release` binary is on a device with a phone connected to it.

## Bench-only commands get their own flag, never `ENABLE_SERIAL_LOG`

`ENABLE_SERIAL_LOG` is **not** a devel marker. The table above shows why: it is
set in `default`, `gh_release`, `gh_release_rc` and `sticky`, and only `slim`
clears it (`platformio.ini`, the `-UENABLE_SERIAL_LOG` line). So a command
gated `#ifdef ENABLE_SERIAL_LOG` ships in both release builds.

`CMD:SETTING` was gated that way until 2026-09-02, with a comment above it
claiming `ENABLE_SERIAL_LOG` is "set only in env:default". It was not. The
command writes persisted settings and `SETTINGS.saveToFile()` puts them on the
card, so a person holding a lost device with a USB cable could turn on
`mapAutoSyncTiles` or `mapTileFreshnessMode` (both spend the rider's mobile
data) or `mapDebugInfo` (paints the rider's exact position on the panel), and
the flip survives a reboot with nothing on screen to say who made it.

It now has its own flag, `ENABLE_SETTING_CMD=1`, declared in `default`,
`sticky`, `simulator` and `t5s3pro`, and in no release env. Same shape as
`ENABLE_FRONTLIGHT_CMD` / `ENABLE_GNSS_CMD` here.

`t5s3pro` exists only on this branch, so `develop` could not declare the flag
in it. Merging `develop` here silently drops `CMD:SETTING` off the bench board
until it is re-declared -- it happened on 2026-09-02 and the strings check below
is what caught it. Check `t5s3pro` after every merge down from `develop`.

**The gate is checkable without a device**, and the check can fail, which is
why it is worth running. Build both, then look for the reply strings:

```
strings -a .pio/build/default/firmware.bin    | grep -c SETTING_OK   # 1
strings -a .pio/build/gh_release/firmware.bin | grep -c SETTING_OK   # 0
```

Measured 2026-09-02 in one worktree, one instrument, the branch the only
difference. `gh_release` at `09eaa466` (before) has `SETTING_OK` 1 and
`SETTING_ERR` 1 and literally carries `SETTING_OK:%s=%u` and
`SETTING_ERR:unknown`; at `a2f4bacb` (after) both are 0 and the binary is 336 B
smaller. `default` at `a2f4bacb` has 1 and 1, so the strings are still emitted
where they should be.

Both release binaries were built with the `esp_bt.h` include path lent in
through a throwaway `platformio.local.ini` (see the next section). It adds no
`-D`, so it cannot change which code is compiled in.

`mapPinsOffscreen` and the other key names stay in both binaries and that is
correct -- those come from the settings serializer (`CrossPointSettings`), not
from the command.

`slim` is unaffected. A `slim` binary built before and after the change is
byte-identical apart from two gzip mtimes in the embedded web assets and the
image SHA256 that follows from them (67 bytes at offsets 177-208, 1576669,
1578089 and the trailing 32) -- the block was excluded there before, via
`-UENABLE_SERIAL_LOG`, and is excluded now, via the absent flag.

## The release envs did not compile from 2026-08-17 to 2026-09-08

And the other two release envs almost certainly do not either. **Measured** on
`gh_release`. `gh_release_rc` and `slim` are **read**, not measured: they carry
identical `lib_deps`, and `slim` only built here once the same include path was
lent to it as well.

Found while checking the gate above, 2026-09-02, on `develop` at `09eaa466`.

```
lib/hal/HalPowerManager.cpp:8:10: fatal error: esp_bt.h: No such file or directory
```

`HalPowerManager.cpp:8` includes `<esp_bt.h>` unconditionally (it asks the BT
controller for its state, see `lowPowerFloorMhz()`), but the bt include
directory only reaches the compiler in an env that pulls a BLE library --
which is `default` alone, for the reason in the section above. `gh_release`,
`gh_release_rc` and `slim` all fail on that line. It arrived with the power
work (`c0c8ef09`, `8f44dbc2`), and it is a separate defect from the
`FREEINK_CAP_BLE_PERIPHERAL` gap: that one makes a release binary useless, this
one stops it existing. Tracked in the parent repo's `docs/TODO.md` as T-240 (and fixed, see below).

The gate measurement above was taken with the include path lent to the release
envs through a throwaway `platformio.local.ini`, nothing committed.

**Fixed 2026-09-08.** `HalPowerManager.cpp` now guards the include with
`__has_include(<esp_bt.h>)` and skips the controller question when the header is
absent -- where the BT component is not built no controller can exist, so the floor
answers itself and the guard cannot hide a wrong clock. **Measured after the fix, in
a fresh worktree with no `platformio.local.ini`:**

| Env | Chip | RAM | Flash |
|---|---|---|---|
| `gh_release` | ESP32-C3 | 16.1 %, 52,812 B | 57.5 %, 3,768,871 B |
| `gh_release_rc` | ESP32-C3 | 16.1 %, 52,812 B | 57.5 %, 3,768,867 B |
| `slim` | ESP32-C3 | 16.1 %, 52,788 B | 56.8 %, 3,720,817 B |
| `sticky` | ESP32-S3 | 19.2 %, 62,844 B | 55.0 %, 3,606,343 B |

So the "almost certainly" above was right: all three release envs failed, and so did
**`sticky`**, the only S3 env -- which nobody had reported, because the 2026-09-02
pass was hunting a release binary and never built it. The dating is read off the
commit that added the include plus a check that `sticky` carried no BLE dependency
then either (`extends = base` and one log flag, at `8f44dbc2` and today), so the env
could not have compiled from that commit onward.

**Two consequences worth keeping.** The task was in the parent repo as **T-240**,
not T-237 as the paragraph above used to say, and its compile half is now done; what
remains of it is CI, tracked as T-280. And the lesson is the one the whole section
demonstrates twice: **an env nobody builds is an env that is broken**, so a merge
builds every env, not the one being worked in.

## CrossPoint `develop` does not build here, with or without a patch

`pio run -e default` on `upstream/develop` (`7db14a01`) fails after 14 minutes,
before reaching a single source file:

```text
idf_tools.py installation failed (rc=1). Tail:
    raise RuntimeError(f'at level {level}, expected 1 entry, got {contents}')
RuntimeError: at level 0, expected 1 entry, got ['riscv32-esp-elf', 'picolibc',
  'bin', 'package.json', 'include', 'share', 'lib', 'libexec']
TypeError: expected str, bytes or os.PathLike object, not NoneType:
  File "scripts/patch_pioarduino_cache.py", line 51
```

It is a toolchain-install problem in this environment against their pinned
pioarduino 55.03.311, not a broken tree, and it happens with or without any
patch applied. Measured 2026-09-06.

**Consequence for upstream work:** a fix offered to CrossPoint cannot be
compile-checked locally. Say so in the PR and lean on their CI rather than
implying the branch was built. `crosspoint-reader/crosspoint-reader#3410` is
written that way.

Our own environments are unaffected -- they pin a different pioarduino.

## `platformio.ini` states a range, not a version

A `lib_deps` line is a constraint, not a fact about the build. `h2zero/NimBLE-Arduino @ ^2.3.8` resolved to **2.5.1** on 2026-09-01, two minor
versions up.

So a claim about what a library does is read from the tree actually on disk,
`.pio/libdeps/<env>/<lib>/`, and the version confirmed in that directory's
`.piopm`:

```
cat .pio/libdeps/t5s3pro/NimBLE-Arduino/.piopm
```

Reading the ini instead put a wrong version into a bug report before it was
caught (`ble-deinit-crash.md`). The same applies to anything else pinned with
`^` or `~`.

## A fresh worktree cannot build `env:default` offline

`lib_deps` pulls JPEGDEC from a git URL, so the first build in a new worktree
needs network and fails behind a sandbox with
`could not read Username for 'https://github.com'`. Copying
`.pio/libdeps/default/JPEGDEC` from another checkout gets past that and then hits
a second wall: `lib/hal/HalPowerManager.cpp` includes `<esp_bt.h>`, which the
isolated core rebuild only ships when something enables the BT controller.

So a board-specific change (say T5S3-only) cannot be regression-built against
`env:default` in a new worktree without setting that up first. Say that, rather
than reporting the env as broken by the change -- on 2026-09-02 a session nearly
did.

## Flashing: three images, three offsets

Verified against a real X4's own 16 MB flash dump, not read off a datasheet.

| Offset | File | Note |
|---|---|---|
| `0x0` | `bootloader.bin` | the C3 puts it at zero, not at `0x1000` like the original ESP32 |
| `0x8000` | `partitions.bin` | built from `partitions.csv`; byte-identical to the table dumped off a device |
| `0x10000` | `firmware.bin` | `app0` starts here per the partition table |

```
pio run -e default -t upload            # build and flash in one step
esptool -p /dev/ttyACM0 write-flash \
  0x0     .pio/build/default/bootloader.bin \
  0x8000  .pio/build/default/partitions.bin \
  0x10000 .pio/build/default/firmware.bin
```

No filesystem image is written. The map style is compiled in (`data/mapstyle.json`
is a build-time input) and the `spiffs` partition gets no image, confirmed by
what lands in the build directory.

**Which of the three images has ever been on a device.** `partitions.bin` is
byte-identical to a table dumped off a real X4. `firmware.bin` in a release is an
archived build that was confirmed working. `bootloader.bin` is only ever a build:
it differs from the dumped device's bootloader region (18,377 of 18,672 bytes on
2026-08-22), because the dump predates it. All three go on together with
`pio run -t upload`, so the combination is exercised on every flash -- but a
write onto a **stock** device has never been done by us.

**Back up the device's flash before the first write.** The reader ships with the
vendor's own firmware and there is no download that puts it back:
`esptool -p /dev/ttyACM0 read-flash 0 0x1000000 stock-backup.bin`. See
[`fix-bricked-xteink.md`](fix-bricked-xteink.md) for the recovery side.

## Published builds

Releases live on GitHub Releases in this repo, tagged `explorink-v<version>`.
The prefix matters: this fork inherited CrossPoint's own `0.1.0` ... `1.6.0rc`
tags, so a bare `v0.1.0` reads as an upstream release.

The first one, `explorink-v0.1.0-alpha` (2026-08-22), publishes the archived
binary that was confirmed working on an X4 rather than a build made for the
release: same bytes, same SHA-256. A published build nobody ran on hardware is
worse than no build.

## `SPI.h: No such file` means another session's build, not a broken include path

A build that worked minutes ago can fail like this:

```
freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h:17:10:
fatal error: SPI.h: No such file or directory
```

The include path is fine. `framework-arduinoespressif32-libs/<chip>/` is shared
mutable state -- whichever build ran last rewrites it, in any session -- and a
parallel build in another worktree swapped it mid-compile. Seen 2026-09-06 on
`t5s3pro` while another session was building the same environment.

**Re-run the same build.** It succeeds. Do not "repair" the framework directory:
deleting or hand-editing anything in it breaks every other build on the machine,
and there was never anything wrong with it.

## `undefined reference to ble_store_config_*` is the shared build cache

A link that failed 2026-09-10 on `t5s3pro`, with every source file compiling
clean:

```
ble_store_nvs.c.o: undefined reference to `ble_store_config_num_our_secs'
ble_store_nvs.c.o: undefined reference to `ble_store_config_our_bond_count'
collect2: error: ld returned 1 exit status
```

Nothing in the tree was wrong: the symbols live in `ble_store_config.c`, which
the same log shows the build **retrieved from cache** rather than compiling. So
the object in PlatformIO's build cache did not match the flags this environment
builds NimBLE with. That cache (`~/.buildcache` by default) is per-machine and
therefore **shared across every worktree and every session**, exactly like
`framework-arduinoespressif32-libs/` two sections up.

**The cause is isolated, not inferred.** The failing build had already had
`.pio/build/t5s3pro` deleted, and the passing one differed from it in exactly one
thing: `PLATFORMIO_BUILD_CACHE_DIR` pointing at a private directory. A clean
environment directory on its own did not fix it.

**The fix is a private cache for the run, not a global settings change:**

```
PLATFORMIO_BUILD_CACHE_DIR=<scratch>/piocache pio run -e t5s3pro
```

That is an environment variable for one process. `pio settings set` would change
the cache for every build on the machine, which is the shared-config blast
radius this repo has already been bitten by -- measure who a shared setting hits
before moving it.

The two shared-state failures read differently and both look like a broken
checkout: this one names a symbol, the framework one names a header.

## `file format not recognized` from objdump is a corrupt object, not a broken tree

Three builds failed 2026-09-05 on

```
riscv32-esp-elf-objdump -h .../esp-idf/app_trace/libapp_trace.a
  returned non-zero exit status 1
```

while every source file compiled fine. Running objdump by hand named the
culprit, which the build never prints:

```
app_trace.c.o: file format not recognized
```

One object inside the archive was truncated or garbage.

**Cause unknown.** It first appeared with two `pio run` invocations racing the
same `.pio`, which is the obvious suspect, but it recurred once with only a
gradle build running alongside. So "do not build twice at once" is a suspicion
worth having and not a finding, and it is written here as one.

**The fix is settled and it is cheap.** Delete the one subdirectory and rebuild:

```bash
rm -rf .pio/build/<env>/esp-idf/app_trace
pio run -e <env>
```

Under three minutes. Deleting the whole `.pio/build/<env>` also works and costs
about four times that, which is what the first two recoveries paid before
anybody ran objdump by hand.

## Building a second environment in one worktree wipes the first one's build

`pio run -e default` run after `pio run -e simulator` in the same worktree left
`.pio/build/` holding only `default`. The simulator binary was gone, and the
scripted screenshot run that followed failed with
`No such file or directory` -- which reads like a build that never happened
rather than a build that was deleted.

PlatformIO keys the build directory on a project checksum that the environment
is part of, so switching environment invalidates it. Nothing warns.

Build one environment, use its artefact, and only then switch. When two
environments are needed at once -- a device binary and a simulator run against
the same commit, say -- give each its own worktree.

Measured 2026-09-07, `settings-facade` on `develop`, `default` and `simulator`.

## `pio run -t upload` can flash a binary older than what `pio run` just built

**2026-09-13.** A rebuild reported `SUCCESS` with the right new size in its own
output, then a separate `pio run -t upload --upload-port <port>` a few minutes
later -- with no `pio run` in between -- wrote a `firmware.bin` whose mtime
predated the source file's own last edit and whose size matched the *previous*
build. `esptool` reported success; the device ran the old code.

Cause: nothing pinned the two commands to the same build. Anything that
touches `.pio/build/<env>/` between the build you trust and the upload --
another environment's build in the same worktree (see "Building a second
environment ... wipes the first one's build" above), a build in a different
session, a stale link -- can leave `-t upload` shipping whatever sits in that
directory, not what the last `pio run` you watched succeed produced.

**Rule: `pio run -t upload` alone is not evidence of what got flashed.** Before
trusting an upload, `rm -rf .pio/build/<env>` and `pio run -e <env>`
immediately before `-t upload`, with nothing else touching that directory in
between. If a serial command's behaviour does not match what was just fixed,
check the binary's build time and size against the source's own mtime before
assuming the fix is wrong.

## Both device branches had `env:simulator` broken, and `develop` did not

Found 2026-09-12, promoting `release/lilygo-t5-s3-pro` and
`release/xteink-x4-pro` into `develop`. Seven of the eight environments built on
the merged tree; `simulator` failed. Built on each branch on its own to place
the blame:

| Branch | `pio run -e simulator` |
|---|---|
| `develop` 85af8066 | SUCCESS |
| `release/lilygo-t5-s3-pro` 6844ac06 | FAILED |
| `release/xteink-x4-pro` 573e5c3f | FAILED |

So it was not a merge conflict resolved badly. Both device branches had been
carrying a broken native env for as long as nobody built it, and the promotion
was about to move that break onto the one branch where the env still worked.
This is the section above read backwards: an env nobody builds is an env that is
broken, and a device branch is exactly where "nobody builds it" happens, because
the board on the desk is not the host.

Two independent causes, both in the simulator fork rather than in this repo:

- `src/main.cpp:6` includes `<FrontlightManager.h>` unconditionally. The SDK
  library cannot simply be added to `[env:simulator]`'s `lib_deps` -- its header
  includes `<BoardConfig.h>` and reads `BoardConfig::ACTIVE.frontlight`, and the
  fork ships a reduced `BoardConfig.h` on purpose so the native build pulls in no
  ESP32 GPIO headers. The fork got a shim whose `present()` is false, which is
  the same answer the real class gives on an X4 or an X3.
- `WebDAVHandler.cpp:362` hands a `NetworkClient` to a `Print&` parameter, which
  is correct against Arduino (`NetworkClient` is a `Stream`, a `Stream` is a
  `Print`). The fork's `NetworkClient` derived from neither, so every device
  build was fine and only the host build failed.

**Consequence for the next promotion:** build `simulator` on the device branch
*before* merging it up, not on the merged tree. The merged tree cannot tell you
which side broke it, and on this pass that cost two extra worktrees and two
builds to find out.

## No CI has ever run on this repo

**Measured 2026-09-09.** Five workflows are registered and `active` --
`ci.yml`, `pr-formatting-check.yml`, `release.yml`, `release_candidate.yml`,
`release-fonts.yml` -- and
`gh api 'repos/rfordinal/explorink/actions/runs'` returns `total_count: 0`.
Not one run, ever.

`gh api repos/rfordinal/explorink/actions/permissions` answers
`{"enabled": true, "allowed_actions": "all"}`, which is why this went
unnoticed. That flag is not the gate. **This repo is a fork** of
`crosspoint-reader/crosspoint-reader` (`gh api repos/rfordinal/explorink`,
`"fork": true`), and GitHub disables Actions on a fork until somebody clicks
enable in the Actions tab once. The API does not report that state.

Three consequences:

- **Nothing checks a build or the formatting.** The green-CI assurance the
  `pr-formatting-check` workflow implies does not exist, which is one reason 35
  files had drifted out of clang-format by 2026-09-09.
- **`release.yml` does not fire on a tag** despite `on: push: tags: '*'`. Two
  tags pushed 2026-09-08 produced no run. The two releases that exist were made
  by hand.
- **A local check is the only check.** `pio run -e <env>`, the host tests and
  `./bin/clang-format-fix -g` are it.

Enabling it is T-294 in the parent repo, and it is a decision rather than a
chore: five workflows that have never executed will all fire at once.

## Two pio runs at once destroy `managed_components/`

`pio run -t upload` on a C3 env, started while a `pio run` for an S3 env was
still compiling, decided to **reinstall the Arduino framework** mid-flight.
Both S3 builds then failed, and not with anything that names the cause:

```
kconfiglib.core.KconfigError: .pio/build/x4pro/kconfigs_projbuild.in:7:
  '.../managed_components/espressif__esp-sr/Kconfig.projbuild' not found
FileNotFoundError: '.../sdkconfig.t5s3pro'
```

Measured 2026-09-13. Re-running the same six environments serially, with
nothing else touching pio, gave 6/6 SUCCESS and no source change in between.

The framework directory was already known to be shared mutable state, per chip.
What is new is that an **upload** rewrites it too, and that the wreckage lands
in `managed_components/` and the generated `sdkconfig.<env>` rather than in the
framework directory the rule names. So: one pio process at a time in this tree,
whatever it is doing, and a build failure naming a missing `managed_components`
path is an environment race until proven otherwise.

The flash itself has a path around this:

```
esptool.py --chip esp32c3 --port <port> write_flash 0x10000 <firmware.bin>
```

That writes the app partition and depends on no pio state at all.

**A different symptom, same race, same day.** `pio run -e x4pro -t upload`
failed later the same session with

```
Error: Invalid value for '<address> <filename>...': [Errno 2] No such file or
directory: '.../.pio/build/x4pro/bootloader.bin'
```

right after `pio run -e x4pro` alone had reported `SUCCESS` moments earlier,
with a background `pio run` for `default` then `t5s3pro` started in between.
Waiting for both to exit (`pgrep -f "pio run"` empty) and re-running the exact
same upload command, unchanged, succeeded first try -- same fix as above, so
this is read as the same race, not isolated to a specific file the way the
`managed_components` case was. Every build log that session also printed
`*** Original Arduino "idf_component.yml" restored ***`, a candidate for what
a concurrent build clobbers, but that was not confirmed further.

**A third symptom, and this time the restore step itself failed.** `pio run
-e t5s3pro` failed 2026-09-15 with

```
*** Original Arduino "idf_component.yml" couldnt be restored ***
Building .pio/build/t5s3pro/firmware.bin
...
Error: Path '.pio/build/t5s3pro/firmware.elf' does not exist.
```

right after a `pio run -e default` had finished in a different worktree of
this same clone. Re-running the identical `pio run -e t5s3pro`, nothing else
touching `pio`, succeeded first try -- same fix as the two 2026-09-13 cases,
so read as the same race, not confirmed by isolating it. The two earlier
cases both saw the restore itself report success and something *after* it
break; this is the first time the restore step has failed outright.
