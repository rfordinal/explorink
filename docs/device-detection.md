# Which device am I: two mechanisms, and only one of them works

Surveyed 2026-09-09, after `/api/status` reported `"device":"X4"` on a LilyGo
T5 S3 Pro all day (T-290 in the parent repo). The status string is the symptom.
The cause is that this firmware answers "which device am I" in two unrelated
ways, and the one almost everything uses cannot name any board beyond X4 and X3.

## The two mechanisms

**`HalGPIO::DeviceType`** — a two-value enum, `{ X4, X3 }`
(`lib/hal/HalGPIO.h:55`), default `X4` (`:58`), read through `deviceIsX3()` and
`deviceIsX4()` (`:64-65`). **29 call sites** across `src/` and `lib/`.

**`BoardConfig::ACTIVE` and `BoardConfig::Board`** — the SDK's full board
profile: controller, geometry, pins, touch, frontlight, orientation, and a
`name` string. Every board has one.

`BlePositionServer.cpp:74-83` already does it the right way: a `switch` on
`Board` that returns a per-board advertising name, `XteinkX4Pro` included. That
is the pattern the rest of the firmware does not follow.

## The line that makes it wrong

`HalGPIO::begin()`, `lib/hal/HalGPIO.cpp:117-137`:

```cpp
#if FREEINK_MCU_C3
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? Board::XteinkX3 : Board::XteinkX4);
  ...
#else
  _deviceType = DeviceType::X4;
#endif
```

**On any non-C3 MCU the detection does not run and the answer is hardcoded to
`X4`.** So the T5 S3 Pro, the X4 Pro, the X4 Classic and Sticky all report and
behave as an X4 in every one of those 29 places. `BoardConfig::ACTIVE` is
correct on those boards -- it stays at the compile-time `DEFAULT_DEVICE` -- so
the two mechanisms disagree by construction, and the one that is right is the
one barely anybody asks.

**Confirmed on hardware the same day, and it is worse than a wrong label.** The
first flash of `env:x4pro` on a real X4 Pro booted with a clean log and a
mounted SD card and **painted nothing**: the `#if FREEINK_MCU_C3` guard meant
`applyXteinkDisplayController()` never ran on the S3, so the profile's default
SSD1677 stood unchallenged on a board that is a UC8279. All three drivers are
compiled in for this device precisely because the batch varies, and the probe is
the only thing that picks. So the guard does not merely misreport the board, it
silently skips the step that makes the panel work (parent `docs/PROGRESS.md`,
2026-09-09).

## "Which device" is the wrong question: it stands in for six properties

The 29 sites do not want to know the device. Each wants one property, and uses
the device as a proxy for it:

| Where | Sites | What it actually asks |
|---|---|---|
| `lib/hal/HalDisplay.cpp` | 5 | does this panel need a resync after a HALF_REFRESH |
| `lib/hal/HalGPIO.cpp` / `.h` | 6 | the detection itself, plus X4-only pin setup |
| `src/components/themes/BaseTheme.cpp` | 3 | screen geometry |
| `src/components/themes/lyra/LyraTheme.cpp` | 2 | screen geometry |
| `src/components/themes/HintGeometry.h` | 2 | hint-bar positions and whether to scale them |
| `src/activities/util/KeyboardEntryActivity.cpp` | 2 | physical up/down button order |
| `src/activities/util/IntervalSelectionActivity.cpp` | 2 | physical up/down button order |
| `src/activities/reader/EpubReaderPercentSelectionActivity.cpp` | 2 | physical up/down button order |
| `src/main.cpp` | 2 | the log line, and differential refresh |
| `src/activities/boot_sleep/SleepActivity.cpp` | 1 | (to classify) |
| `lib/hal/HalPowerManager.cpp` | 1 | (to classify) |
| `src/network/CrossPointWebServer.cpp` | 1 | the board's name -- T-290 |

Most of those already have a real answer in `BoardConfig::ACTIVE`:
`displayWidth` / `displayHeight` for geometry, `inputStyle` and `input` for
buttons, `displayController` for the panel, `name` for the name.

## A latent bug this survey found

**An X3 can carry either of two controllers**, and the code that cares about the
controller asks about the device instead. `HalGPIO.cpp:125` selects
`Board::XteinkX3Uc8279` when the probe finds a UC8279 rather than the UC8253.
But `HalDisplay.cpp:79/89/109/147` requests the post-HALF_REFRESH resync on
`deviceIsX3()` alone, whichever controller answered. So either the resync is a
UC8253 property and is being applied to a UC8279 for no reason, or it is needed
on both and nobody has said so. **Open -- needs a measurement on an X3 of each
batch**, and it is the clearest argument for asking the property rather than the
device.

## Where each capability belongs

Two of the six properties have nowhere to live yet, and they should not become
new `deviceIs*` calls.

- **Panel resync after a half refresh** belongs on the driver, not the board.
  `PanelDriver` already has this exact convention --
  `supportsAsyncDisplay()` (`PanelDriver.h:62`), `supportsStripGrayscale()`
  (`:103`), `supportsBusyGrayscaleStaging()` (`:131`), `hasPendingMaintenance()`
  (`:187`), all defaulting to `false` on the base class. One more of that shape
  answers all five `HalDisplay` sites and answers it per controller, which is
  what the latent bug above needs. **SDK change, so a PR to the fork**
  ([`freeink-sdk-fork.md`](freeink-sdk-fork.md)).
- **Physical up/down button order.** `BoardProfile` has `inputStyle` and
  `input` pins but nothing that says the side buttons run the other way. Either
  a profile field (SDK PR) or a small firmware-side table. Six sites, all doing
  the same `? -delta : delta` flip.

## The shape of the fix

1. `deviceIsX3()` and `deviceIsX4()` go away, and `DeviceType` with them.
2. Detection keeps its job but changes its return type: it answers with a
   `BoardConfig::Board`, and it is what `selectDevice()` is called with. On a
   non-C3 board there is nothing to detect -- one device per env -- so the
   `#else` branch stops asserting anything and leaves `ACTIVE` at the
   compile-time default.
3. Every call site asks `BoardConfig::ACTIVE` for the property it wanted, or
   the panel driver for a panel property.
4. `/api/status` reports `ACTIVE.name` (T-290).

Do it in that order: the call sites cannot be converted while the two mechanisms
can still disagree, and the two missing capabilities gate the display and button
groups.

## Not verified

Nothing here has been changed or run. This is a read of the code on `develop`
at `89e7b695`, with the `SleepActivity` and `HalPowerManager` sites still
unclassified.
