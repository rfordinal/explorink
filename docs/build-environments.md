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

`TRAILINK_VERSION` is set explicitly in every env except `default`, where
`scripts/git_branch.py` derives it from the branch and short SHA.

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

## `gh_release` does not compile at all right now

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
one stops it existing. Tracked as T-237 in the parent repo's `docs/TODO.md`.

The gate measurement above was taken with the include path lent to the release
envs through a throwaway `platformio.local.ini`, nothing committed.

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
