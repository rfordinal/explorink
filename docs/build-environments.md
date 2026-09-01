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
