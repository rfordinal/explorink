# LilyGo T5 S3 Pro bring-up

First ExplorInk build flashed to the board on 2026-08-31. It boots, draws, takes
touch, and runs the whole activity stack. This file records what the build needs,
what already works, and what is still wrong.

The board itself, its parts and the vendor correspondence are in the parent repo:
[`../../../docs/devices/lilygo-t5-s3-pro.md`](../../../docs/devices/lilygo-t5-s3-pro.md).
Branching for this device: [`branching.md`](branching.md).

## The build environment

`[env:t5s3pro]` in `platformio.ini`. No firmware source is board-specific: the
whole board lives in `freeink-sdk`, so the env is flags and `lib_deps` only.

- `BoardT5S3` (`freeink-sdk/libs/hardware/BoardT5S3`) supplies the parallel-bus
  pins, the PCA9535 + TPS65185 EPD power sequence and the user-button hook.
- The panel has no on-glass controller, so `LgfxEpdDriver` wraps LovyanGFX's
  `Panel_EPD` out of `m5stack/M5GFX`.
- `-DFREEINK_DEVICE_LILYGO=1` selects `BoardConfig::LILYGO_T5S3`, which derives
  GT911 touch, the PWM frontlight and the BQ27220/BQ25896 I2C gauge
  (`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:878-905`).

Three things the SDK's own `platformio.sample.ini` does not tell you:

- **M5GFX 0.2.20 does not exist in the PlatformIO registry.** The sample env
  pins it; the registry publishes 0.1.17, 0.2.27 and 0.2.28 only. Pinned to
  0.2.28, which builds.
- **NimBLE-Arduino is needed to build at all**, not just for BLE features.
  `lib/hal/HalPowerManager.cpp:8` includes `<esp_bt.h>` unconditionally, and the
  isolated Arduino core rebuild ships that header only when something enables the
  BT controller. Without NimBLE in `lib_deps` the build stops at
  `fatal error: esp_bt.h: No such file or directory`.
- **PSRAM matters here and does not on the `sticky` env**, because
  `LgfxEpdDriver` keeps its 8-bit grayscale canvas there. `esp32-s3-devkitc1-n16r8`
  is the stand-in board definition for the real ESP32-S3-WROOM-1 N16R8: octal
  PSRAM and `-DBOARD_HAS_PSRAM`.

Sizes, 2026-08-31: flash 58.3 % of the 6.4 MB app partition, DIRAM 45 %, IRAM
full at 16 KB (same as every other env). Free heap on the Home screen is
~192 KB against the X4's ~50 KB.

## What works, verified on hardware

Flashed with `pio run -e t5s3pro -t upload`. The board takes bootloader,
partition table and app the ordinary way -- no offset trickery, unlike the X4.

- **Boot to Home.** `Boot` then `Home` activity, both drawn.
- **The panel.** Full frame in 34 ms from `clearScreen` to `displayBuffer`, and
  6 ms for the boot screen. Much faster than the X4, and fast enough to be
  surprising -- worth its own measurement rather than a note.
- **Touch.** GT911 works with no configuration upload; Settings was reached and
  the map opened by finger. `[maintainer, on the board]`
- **SD card.** Detected on the SPI pins the profile names.
- **BLE.** `NimBLEDevice::init()` succeeds; `BlePositionServer::begin()` costs
  57,080 bytes, within 1.5 KB of what it costs on the X4.
- **The frontlight**, through `CMD:LIGHT` (below). Bright and even across the
  whole panel. `[maintainer, on the board]`
- **Power management.** The 80 MHz idle throttle engages and restores.

## The frontlight, and the vendor's frequency ceiling

The firmware had never called `FrontlightManager` at all -- the library was not
even in `lib_deps`. It is now in `[base]`, so every env gets it; it is inert on a
board whose profile has no frontlight (`FREEINK_CAP_FRONTLIGHT=0` links stub
bodies and pulls in no LEDC code).

There is no frontlight UI on any screen yet, so bring-up drives it over serial:

```
CMD:LIGHT        ->  LIGHT_OK:<percent>     (query)
CMD:LIGHT 50     ->  LIGHT_OK:50            (0-100, 0 = off)
```

Devel-only on purpose: `-DENABLE_FRONTLIGHT_CMD=1` lives in `env:t5s3pro` and in
no release env. It actuates the device, and `CLAUDE.md`'s security rule defaults
a new command to devel until widening it is a deliberate decision.

**The SDK profile asks for a PWM frequency the vendor says is too high.**
`LILYGO_T5S3` carries `{11, 5000, 8, true}` -- GPIO11 at 5 kHz
(`BoardConfig.h:899`). LilyGo's mail of 2026-08-25 says the PT4103B23F behind
`BL_EN` wants a frequency **not above approximately 1 kHz** (quoted verbatim in
[Outline: LilyGo](https://wiki.comsultia.com/doc/lilygo-3X4JgoVOpf)). `freeink-sdk` is upstream,
so `src/main.cpp` corrects it after `frontlight.begin()` with
`ledcChangeFrequency(gpio, 1000, bits)` rather than forking the SDK. The light
was only ever driven at 1 kHz here; 5 kHz was never tried, so nothing is known
about how it behaves.

## The map draws, 2026-08-31

`CMD:GOTO_MAP`, then `CMD:SCREENSHOT`, which answers `SCREENSHOT_START:64800` --
960x540 at 1 bpp, exactly what the profile declares. The frame carries the
header, the compass rose, the scale bar, the position marker with its heading
arrow, place labels, roads with casings, buildings, and the hatch and dither
area fills. Both z12 tiles were on the card by then: `2 tiles ok, 0 missing,
6829 ways, 7 places, 1021804 bytes`.

**The tiles arrived over BLE, from the phone, with nothing driving it.** The
Android app woke on the advertisement (`../../docs/ble-app-wake.md`) and
autosync asked for what the viewport was missing:

```
[BLEPOS]  connected: interval 24 units (30 ms), latency 0, timeout 500
[BLEPOS]  command channel subscribed / transfer status subscribed
[BLEPOS]  MTU now 256, file payload 248 bytes per chunk
[MAP]     autosync: asked for 1 tiles on screen
[MAPXFER] begin /trailink/base/12/2242/1421.tib, 461791 bytes, crc f29a63c3
[MAP]     ble fix: seq 3, utc 1788178462, accuracy 100 m, alt 191
```

So connection, both notify channels, MTU negotiation, the `tiles` command, a
461 kB file transfer and a real GPS fix all work on this board unchanged.
`tools/blepos.py` is neither needed nor usable while the phone holds the link:
the device stops advertising once connected, which is correct behaviour and not
a fault.

**MTU settles at 256, not the X4's 517.** That halves the chunk payload, 248 B
against 509 B. Whether the ceiling is the phone, the NimBLE config or the S3 is
unmeasured.

### Three things the frame shows

- **The chrome is sized for the X4.** Header, scale bar and marker are drawn at
  the same pixel sizes as on 480x800, merely spread over 540x960 on a denser
  panel (~234 PPI). The profile carries `uiScale 1.2` and the map's chrome does
  not appear to use it. This is the universal-style defect, not a LilyGo quirk.
- **The scale bar is jammed into the bottom-left corner**, its label sitting
  practically on the frame edge.
- **A full map redraw costs about 2.7 s** -- `GFX Time = 2691 ms` and `2644 ms`
  across two viewport resets. The 6 ms and 34 ms figures elsewhere in this file
  are the Boot and Home screens, a different path with trivial content. Nothing
  here says the map redraw is slow *for this panel* rather than slow for the
  amount of work: it is a firmware-side number, not a panel measurement.

### The map is half-operable on this board

`MapActivity` is driven entirely by the logical buttons Up / Down / Left /
Right / Confirm / Back (`src/activities/map/MapActivity.cpp:2706-2760`): in
Follow, Up and Down are the zoom ladder and Left/Right move the look-ahead; in
Observe the four pan and a held Up/Down zooms; Confirm opens the menu that
carries Zoom in and Zoom out (`map-menu.md`). The activity's own code reads no
touch -- no `wasScreenTap`, no `rowTouch`.

On the T5 the profile assigns none of those buttons: `input` is
`{PIN_UNASSIGNED x6, 0, false}` (`BoardConfig.h:884-885`), where the `0` is the
power button on GPIO0 (BOOT). The board's second button is behind the PCA9535,
reachable only through `InputManager::setButtonHook()`, and **this firmware
never calls it** -- the symbol does not appear anywhere in `src/` or `lib/`.
`LILYGO_T5_PRO_GT911` does not set `synthConfirm` either
(`BoardConfig.h:611-613`).

**Two of the six work anyway, and it is worth knowing why**, because it is not
in `MapActivity` at all:

- **Back** comes from a gesture one layer down.
  `MappedInputManager::wasPressed()` returns true for `Button::Back` whenever
  `wasBackGesture()` fires (`src/MappedInputManager.cpp:312-315`), and that is a
  left-to-right swipe starting inside the left 25 % of the screen
  (`MappedInputManager.cpp:268-280`). So the map exits by finger without
  knowing a finger exists. **Confirmed on the board by the maintainer,
  2026-08-31.**
- **Home** is handled globally: `ActivityManager::loop()` takes
  `wasHomeGesture()` -- an upward swipe from the bottom 14 % -- for every
  activity that is not Home (`src/activities/ActivityManager.cpp:75`).

What is missing on the map, then, is **Confirm, Up, Down, Left and Right**: the
menu, the zoom ladder, the look-ahead and panning. A rider on this board can
reach the map, see it, and leave it, and can change nothing about the view.

**One gesture already exists for the gap.** `wasMenuGesture()` -- a downward
swipe from the top 14 % (`MappedInputManager.cpp:283-294`) -- is consumed by
exactly one place in the firmware, the reader (`ReaderUtils.h:110`). Routing it
to `openMapMenu()` puts Zoom in and Zoom out within reach without inventing an
input model, because the menu already carries them. Tracked as T-573 in the
parent repo.

## What is wrong or missing

- **The profile declares `NO_SENSORS` although the board has a PCF8563 RTC**
  (`BoardConfig.h:904`; the RTC is confirmed by LilyGo's mail of 2026-08-26,
  correcting their own README's PCF85063). So boot logs `[CLK] RTC not found`
  and the device cannot know the time across a reboot. Tracked as part of T-571
  in the parent repo, because the sunrise/sunset frontlight policy depends on it.
- **`[GYR] SDK IMU not found`** at every boot. The board has no IMU; the profile
  says so. The message is an `LOG_ERR` for a thing that is absent by design.
- **No ambient light sensor**, on this board or any other in the line, and
  `SensorsConfig` has no field for one (`BoardConfig.h:524-534`). The vendor pin
  map defines exactly one light-related pin and it is the output `BOARD_BL_EN
  (11)` (`Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO`, branch `H752-01`,
  `docs/pin_define.md`, read 2026-08-31). T-571.
- **The map screen cannot be reached without a position.** Opening Explore sits
  waiting for a BLE fix, which is the X4's behaviour too. `CMD:GOTO_MAP` then
  `pos <lat> <lon>` on the map console is the way in from a laptop -- but see the
  serial reset below.

## Serial resets the board, always

Opening `/dev/ttyACM0` resets the S3, even with DTR and RTS both driven low
before the port is opened. The USB-JTAG-serial peripheral is on the SoC itself,
so there is no external auto-reset circuit to defeat. Consequence: **every
`CMD:` arrives at a device that has just rebooted and is on the Home screen.**
A command that depends on being inside an activity has to be preceded by the
command that gets there.

The board also disappears from the USB bus entirely when it sleeps or is
powered down -- `/dev/ttyACM*` vanishes and `lsusb` shows no `303a` device. That
is not a crash. BOOT (GPIO0) is the profile's power button and the deep-sleep
wake source.

## Open, needs measurement

- **Refresh timing.** 6 ms and 34 ms are what the firmware logs around
  `displayBuffer`, not a panel measurement, and they cover different waveforms.
  What a full, a fast and a 16-grey refresh actually cost on this panel is one of
  the three numbers promised to LilyGo (T-567 in the parent repo).
- **Frontlight current.** LilyGo says ~20 mA on the 3.3 V rail at full current;
  nothing here has measured it, and the number decides what a night ride costs
  on 1500 mAh.
- **The frontlight at 5 kHz.** Never tried, because the vendor's ceiling was
  applied first. Whether the SDK's default is merely out of spec or visibly
  worse is unknown.
