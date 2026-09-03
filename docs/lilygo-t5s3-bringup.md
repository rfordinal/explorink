# LilyGo T5 S3 Pro bring-up

First ExplorInk build flashed to the board on 2026-08-31. It boots, draws, takes
touch, and runs the whole activity stack. This file records what the build needs,
what already works, and what is still wrong.

The board itself, its parts and the vendor correspondence are in the parent repo:
[`../../../docs/devices/lilygo-t5-s3-pro.md`](../../../docs/devices/lilygo-t5-s3-pro.md).
Branching for this device: [`branching.md`](branching.md).

## The build environment

`[env:t5s3pro]` in `platformio.ini`. Almost no firmware source is
board-specific: the board itself lives in `freeink-sdk`, so the env is mostly
flags and `lib_deps`. Two devel-only commands are scoped to it and appear in no
release env -- `CMD:LIGHT` (`ENABLE_FRONTLIGHT_CMD`, below) and `CMD:GNSS`
(`ENABLE_GNSS_CMD`, [`gnss.md`](gnss.md)).

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
~192 KB against the X4's ~50 KB `[the X4 figure is read from this repo's docs, not measured: there has been no X4 since 2026-08-22]`.

## What works, verified on hardware

Flashed with `pio run -e t5s3pro -t upload`. The board takes bootloader,
partition table and app the ordinary way -- no offset trickery, unlike the X4.

- **Boot to Home.** `Boot` then `Home` activity, both drawn.
- **The panel.** Full frame in 34 ms from `clearScreen` to `displayBuffer`, and
  6 ms for the boot screen. Much faster than the X4 `[the X4 figures are read
  from this repo's docs, not measured: there has been no X4 since 2026-08-22]`,
  and fast enough to be
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
`ledcChangeFrequency(gpio, 1000, bits)` rather than forking the SDK -- which was
the only option at the time and is no longer: `freeink-sdk` is forked as of
2026-09-03 ([`freeink-sdk-fork.md`](freeink-sdk-fork.md)), so this correction
belongs upstream. T-247 in the parent repo. The light
was only ever driven at 1 kHz here; 5 kHz was never tried, so nothing is known
about how it behaves.

## The user button: tap is Select, hold toggles the frontlight

**Written 2026-09-02, not yet run on hardware.**

This board has four switches and only one of them is readable and free of a
fixed job: switch S3 (silkscreened `IO48`), which is net `BUTTON` on PCA9535
`IO12` -- the parent repo's `docs/devices/lilygo-t5-s3-pro.md` has the schematic
trace for all four. So that one button carries two functions:

| gesture | result |
|---|---|
| tap (release under 600 ms) | `BTN_CONFIRM` -- Select, on every screen |
| hold 600 ms | frontlight toggles, immediately, without releasing |

Why the light hangs off a physical hold rather than a touch control: gloves
defeat the capacitive panel, and gloves are not a motorcycle detail -- winter
walking, cold hands and rain produce the same hand. Whatever the person is doing,
the interface must not require touch (Outline, "Voľba zariadenia", point 3).

**Untested on this hardware.** Nobody has held a gloved finger to this panel; the
claim is inherited from how capacitive digitizers work, not measured. What would
settle it: one person, one glove, one tap.

**It is wired in `src/main.cpp`, not through `BoardT5S3::begin()`.** That
function installs the SDK's own hook, which reports the button as `BTN_DOWN`,
and it is still never called here (see the GNSS section above). `setup()`
configures `IO12` as an input on the expander and installs a local
`userButtonHook()` instead, right after `frontlight.begin()` -- the hook can
toggle the light, so it must not be reachable before the LEDC channel exists.

Three things in that hook are load-bearing:

- **Nothing is reported while the button is down.** A `Confirm` press edge at
  touch-down would let the activity act before the hold could still turn out to
  mean the frontlight. The tap is synthesised after release instead.
- **The synthetic press is measured in polls, not milliseconds.**
  `InputManager` commits a state change only after two `update()` calls at
  least `DEBOUNCE_DELAY` (5 ms) apart saw the same state. A wall-clock pulse
  would expire unobserved inside a multi-second panel refresh and the tap would
  vanish; the pulse therefore waits for 3 polls **and** 20 ms.
- **The hold fires at the threshold, not on release**, so the light comes on
  under the thumb.

**What this costs: nothing that worked.** An earlier version of this section
said the change costs `BTN_DOWN` -- the `POWER` + `DOWN` screenshot combo and the
map's Down action. That was wrong, and it looked right because `BoardT5S3.cpp`
really does OR the button into `BTN_DOWN`. But that hook is installed by
`BoardT5S3::begin()`, which this firmware never calls, and
`LILYGO_T5S3.input.down` is `PIN_UNASSIGNED` -- so **`BTN_DOWN` had no source on
this board before this change either**. The button went from doing nothing to
doing two things. The screenshot combo was never available here; `CMD:SCREENSHOT`
is and always was the way. Rule: before pricing a change as a loss, check the
thing being lost was reachable.

`enterDeepSleep()` now drives the frontlight off before sleeping: a hold is one
gesture away from leaving the light on in a bag, and deep sleep stops the LEDC
peripheral without defining what the pin does afterwards.

**Confirmed on hardware, 2026-09-02**, both gestures and the persistence:

```
[INPUT] button released: Confirm (1)          tap reaches the app as Confirm
[BTN] User button hold: frontlight 40%        hold toggles, once per hold
```

The hold logged no `Confirm` line of its own, which is the suppression working:
a hold never also selects.

**The brightness is persisted.** `CrossPointSettings` gained `frontlightOn` and
`frontlightBrightness`; `setup()` restores them after `loadFromFile()`, and both
the hold and `CMD:LIGHT` write them. Two fields rather than one, because turning
the light off must not forget the level it was at. The write happens in `loop()`
and not in the input hook: an SD write on the input path would block every other
poll behind it.

Verified over three reboots (every serial open resets this board, which made the
test cheap): `CMD:LIGHT 40` then reboot answered `LIGHT_OK:40`; a hold that
switched the light off then reboot answered `LIGHT_OK:0`; and the next hold came
back at **40 %, not the SDK's 50 % default**, which is the part that says the
level survived the off.

**Still to check:** that a tap never double-fires and is never dropped after a
slow redraw, that the I2C read per input poll does not disturb GT911 touch or a
panel refresh, and that the light is off after a sleep/wake cycle (deep sleep,
not the reset a serial open causes).

## The board has a second programmable input: the capacitive home key

**Found 2026-09-02, on hardware.** The GT911 reports a capacitive key below the
panel on this board, and the firmware already sees it:

```
[112860] [DBG] [INPUT] home key pressed
[113588] [DBG] [INPUT] home key long-pressed
```

The SDK reads the key's status bit (`0x10`) unconditionally, so this works even
though `LILYGO_T5_PRO_GT911` leaves `hasHomeKey` at its default `false`
(`BoardConfig.h`) -- the flag gates nothing today. `InputManager` already
separates the two gestures: `wasHomeKeyTapped()` fires on the release of a short
press, `wasHomeKeyLongPressed()` at 700 ms (`HOME_KEY_LONG_PRESS_MS`), and the
hold suppresses the tap.

**Nothing in the app reads any of it yet.** `HalGPIO` forwards all three events
and logs them; no activity asks.

So this board has **two** programmable inputs, not one: the side switch (S3,
PCA9535 `IO12`) and this key.

**Both carry the same two gestures, 2026-09-02** (maintainer's call: try each in
real use before splitting them up). Tap is Select, hold toggles the frontlight,
whichever input the rider reached for. **Verified on hardware the same day**,
all four combinations -- the maintainer confirmed both taps select on screen,
and the log carries the rest:

```
[INPUT] home key tapped
[BTN] Home key hold: frontlight 40%        then 0% on the next hold
[INPUT] button pressed: Confirm (1)        side switch, released 34 ms later
[BTN] User button hold: frontlight 40%     then 0%
```

| | tap | hold |
|---|---|---|
| side switch (S3, `IO12`) | Confirm | frontlight (600 ms) |
| home key (GT911) | Confirm | frontlight (700 ms, the SDK's threshold) |

Two different code paths, because the two inputs arrive differently. The switch
is synthesised into a `BTN_CONFIRM` click by the board hook in `main.cpp`; the
key already has tap and hold events in the SDK, so `MappedInputManager` reports
its tap as `Button::Confirm` (next to the swipe that becomes Back) and `loop()`
takes its hold. Both holds land in one `toggleFrontlight()`, so the gesture
cannot come to mean two different things.

**The X4 Pro inherits the home-key half** -- it has a home key too and the
`MappedInputManager` change is not board-conditional. No env builds that board
today, so nothing ships with it untested, but a future X4 Pro env starts with
its home key selecting.

The split is still worth revisiting once both have been used: gloves defeat the
capacitive key and do not defeat the switch, so anything the person needs while
moving belongs on the switch -- and gloves here mean winter, rain and cold hands
as much as a motorcycle.

**Identify an input by making it log, not by its name.** This board's two inputs
are both "the button under the screen" in conversation, and the first
implementation of this feature went to the wrong one. One capture with `[INPUT]`
logging and one press settles it in a minute; a build and a flash do not.



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

**MTU settles at 256, and that is the normal figure, not a shortfall.** The X4
with the same Android app logs `MTU now 256` too
(`../../docs/ble-review-2026-08.md:250`, `303`, `314`, `317`). The 517 in
`../../docs/ble-bridge.md:28` is not a device at all: it is the firmware built
as a host binary behind the BLE bridge. A first draft of this file called 256 a
halving against "the X4's 517" and made a measurement task out of a gap that
does not exist. **Lesson: a number quoted from another doc carries that doc's
conditions with it, and "the firmware logged it" is not the same as "the device
logged it".**

### Three things the frame shows

- **The chrome is sized for the X4.** Header, scale bar and marker are drawn at
  the same pixel sizes as on 480x800, merely spread over 540x960 on a denser
  panel (~234 PPI). The profile carries `uiScale 1.2` (`BoardConfig.h:905`) and
  **nothing reads it**: outside its declaration at `BoardConfig.h:587` the
  identifier does not appear in `src/`, in `lib/`, or in any freeink-sdk
  library. It is a dead field, not a half-wired one, so the chrome cannot scale
  with the panel on any device. This is the universal-style defect, not a LilyGo
  quirk.
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

## Two memory pools, not one

The biggest difference from the X4 after the panel. The C3 has internal SRAM and
nothing else, which is why this firmware's whole memory discipline exists: the
map screen sits near 50 KB free there and `BlePositionServer` has to be torn
down on leaving the screen because it costs 57 KB.

The S3 board has both, and they are separate:

| | what it is | how much, measured on this board | what reports it |
|---|---|---|---|
| internal DRAM | SRAM on the die | 307,684 B total, 191,512 B free on Home | `ESP.getFreeHeap()` |
| PSRAM | 8 MB octal, off-die | 6.5 MB free across the whole heap | `esp_get_free_heap_size()` |

Both numbers are off this board's own log, not off a datasheet. The two APIs are
**not** two views of one number: Arduino's `ESP.getFreeHeap()` is
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`
(`framework-arduinoespressif32/cores/esp32/Esp.cpp:163`), so it never sees
PSRAM; `esp_get_free_heap_size()` counts everything the allocator can hand out.

Three consequences, and none of them is "there is 8 MB now, stop worrying":

- **Internal DRAM is still ~300 KB**, and everything that must be reachable
  from an ISR or by DMA has to live there. PSRAM cannot hold it.
- **The split is at 4 KB.** The core is built with `CONFIG_SPIRAM=y`,
  `CONFIG_SPIRAM_USE_MALLOC=y` and `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`
  (`framework-arduinoespressif32-libs/esp32s3/sdkconfig`): allocations at or
  below 4 KB are forced internal, larger ones may go to PSRAM. So the big
  buffers -- the driver's canvas, a window buffer, the menu backdrop -- land in
  the 8 MB, while every small allocation still competes for the 300 KB.
- **PSRAM is slower.** It is reached over octal SPI through a cache rather than
  on the core bus, so random access costs far more than SRAM.
  `LgfxEpdDriver` keeps its 8-bit grayscale canvas there and expands into it
  every frame, which is one candidate for the ~2.7 s map redraw alongside the
  plain fact that 960x540 is 2.7x the X4's pixel count. Neither has been
  measured apart. `[open]`

**And it makes a class of existing check wrong on this board.** Code written for
the C3 asks `ESP.getFreeHeap()` and then allocates with `new`. On the C3 those
are the same pool. Here the question is about internal DRAM and the answer is
served from PSRAM. `MapActivity::captureMenuBackdrop()` is the clearest case
(`src/activities/map/MapActivity.cpp:2858-2862`): it compares a
hundreds-of-kilobytes backdrop against internal free heap and skips the capture
when it would not fit, although the allocation itself would come out of the 8 MB.
Not dangerous -- it is stricter than reality, so the menu closes the slow way --
but wrong, and wrong the same way on the X4 Pro, which is also an S3. T-574.

## GNSS works. Whether the receiver is on at every boot is open

`CMD:GNSS` over the USB console, `env:t5s3pro` only, no UI and no map
integration. **Run on the board 2026-08-31**: a 3D fix indoors (8 satellites
used, 19 in view, best C/N0 32 dB-Hz), 9600 baud right first time, the pin
direction as the header implies, and the parser correct for the fix it was
checked against. [`gnss.md`](gnss.md) has all of it.

**An adversarial review the same day broke three of that pass's conclusions**,
and the corrected versions matter for this board:

- **The receiver was already powered and tracking when the firmware first
  enabled the rail.** That holds. But "therefore the board powers it at every
  boot" does **not** follow: the PCA9535 sits on the 3.3 V rail and latches its
  registers across an S3 reset, the factory firmware ran on this board earlier
  the same day, and no run in the evidence was a true power cycle. One CONFIG0
  register read on a cold boot settles it and has not been done.
- **The SD-card check could not have failed.** Tiles read fine with the rail up,
  but the log shows card access and panel bus never overlapped, and overlap is
  the whole hazard. So the `LORA_CS` collision remains untested rather than
  cleared.
- **A blocked loop costs 85 sentences, not 30.** The blocking window is the
  whole map `onEnter` at 6.07 s, not the 4,017 ms render, and the receiver sends
  816 B/s measured against a 256-byte RX buffer.

Two more findings out of that work belong here rather than there, because they
are about this board and not about GNSS:

- **`BoardT5S3::begin()` is never called by this firmware.** Not once across
  `src/` and `lib/`. The only code touching the PCA9535 today is
  `LilyGoT5S3LgfxConfig.cpp`, for the EPD power pins, and the only reason I2C is
  up at all is GT911 touch init (`InputManager.cpp:839`). So
  `disableGpsLora()` has never run, `LORA_RST` is undriven at boot, and the
  user button behind the expander had no hook -- which is the same gap the
  unmerged `feat/t5s3-board-begin` branch was opened for, and part of why the
  map is half-operable here (below). The button now has one, installed from
  `src/main.cpp` rather than by calling `BoardT5S3::begin()` (see "The user
  button" above); everything else in this bullet still holds.
- **LovyanGFX does drive `LORA_CS` as a GPIO.** The parent repo's
  `docs/lora.md` had this `[open]` because M5GFX was not checked out. It is now:
  `Bus_EPD` drives the pins handed to it as `pin_oe` and `pin_pwr` as real
  GPIOs, and passes one to the i80 peripheral as its DC line
  (`Bus_EPD.cpp:83,85,120,129,143`). On this board both are GPIO46, which is the
  SX1262's chip select, on the SD card's SPI bus.
  **Answered 2026-09-03, and it is a fault.** This bullet used to end "that it
  is a hazard is not settled", because the lines cited above drive the pin high
  and a deasserted chip select is harmless. The hazard is not in those lines: it
  is that `lgfx::pinMode(pin, output)` sets no level, so the pin is simply left
  low from display init and never raised. See the SD-card section below, which
  also retracts the "every refresh" reading of `Bus_EPD`. Whether an SX126x in
  reset parks MISO high-Z is still unread, and no longer load-bearing.

## The EPD config asserted the LoRa radio's chip select, and the SD card died

**Root-caused and fixed 2026-09-03.** This section was wrong three times before
it was right, so it says which parts are measured and which are read off code.
The parent repo's BUG-037 has the full history including the dead versions.

The card became unusable: every BLE tile `begin` answered `ERR mkdir failed`,
`settings.json` would not save, the Wi-Fi File Transfer page answered `HTTP 500`
to `/mkdir` and listed the volume as empty, and roughly one boot in two came up
on "SD card error". Three unrelated tasks, three vocabularies.

**The card is fine. Measured on the laptop 2026-09-02**, in a USB reader: a
64 KB write, `mkdir base/11/1125` -- the exact path the firmware had just
refused -- and a copy into it, all `rc=0`, checksums matching after an unmount
and remount. `dmesg` says `Write Protect is off`. It also took a **second
reader** to see the card at all: invisible on the SY-T18 (`14cd:1212`), fine on
a Genesys (`05e3:0764`). That reader trouble was never fixed, only worked around.

### The cause is a placeholder pin in our own EPD config

Not the board. The T5 S3 Pro is wired the ordinary way for a device that has both
a radio and a card. What broke it is `freeink-sdk`'s
`lilygoT5S3LgfxConfig()`, which passed `T5S3_LORA_CS` (GPIO46, the SX1262's
`NSS`, on the SD card's SPI bus) as **both** `pinOe` and `pinPwr`.

Both were placeholders and the commit that added the driver says so: `9becf6c`
(2026-06-05) writes `/*OE dummy*/ T5S3_LORA_CS` and `/*pwr dummy*/
T5S3_LORA_CS`, and `LgfxEpdConfig.h` documents `pinOe` as "may be a dummy GPIO if
real OE is via an expander hook". LovyanGFX requires the two fields; this panel
needs neither, because its real output-enable is `PCA9535_IO10_EP_OE` and its
power sequence is the TPS65185, both driven by the injected hooks. GPIO46 was
simply what was to hand.

`Bus_EPD::init()` then calls `lgfx::pinMode(pin, output)` on both
(`Bus_EPD.cpp:120,143`). That call writes no level -- its `gpio_hi()` is guarded
to non-output modes (`common.cpp:599-601`) -- and `GPIO_OUT1_REG` resets to 0
(`gpio_reg.h:77-80`). So the pad becomes an output at 0 and **nothing ever raises
it**. The `powerControl()` that would have set a level is virtual
(`Bus_EPD.h:95`) and `FreeInkBusEPD` overrides it without calling the base
(`LgfxEpdDriver.cpp:34-45`), so it never runs.

**An earlier version of this section said the pin is driven low per refresh. It
is not.** It is left low, once, and never touched again.

### What the hardware says

Measured 2026-09-03 with `CMD:SDBUS`, which toggles the chip select, the reset
line and the shared GNSS/LoRa rail independently and reads a 19,855-byte file
with a CRC32. Nine runs, each after a hard reset with a passing baseline read.

| rail | reset line | radio selected | card |
|---|---|---|---|
| off | driven | no | reads, `7bda5027`, 40 ms |
| off | driven high | no | reads |
| on | driven high | no | reads |
| on | driven high | **yes** | reads |
| on | driven low | **yes** | reads |
| on | **floating** | **yes** | fails |
| off | driven low | **yes** | fails, reproduced twice |
| off | floating | **yes** | fails |

**Deselected, the card read correctly in every combination.** Selected, it failed
unless the radio was **both** powered **and** had its reset actively driven. No
exception in either direction. The failure is sticky within a boot: raising the
chip select again does not bring the card back, only a reset does.

**Why an undefined radio loads the bus is inference, not measurement.** No
SX126x datasheet is on disk and GPIO46 has never been scoped on the pad. State
the table, not the theory.

### GPIO46 comes out of reset low, and that is the other half of the symptom

**Measured 2026-09-03**, first boot of the fixed build:

```
[411] [SDBUS] LORA_CS (GPIO46) was LOW -- the radio was selected on the card's bus at boot, now deselected
[418] [SD] SD card detected
```

GPIO46 is a strapping pin and its reset level is in nothing on disk. Now it is
observed: **low**. So at `Storage.begin()`, before any of our code or the EPD
driver has touched the pin, the radio is already selected. That is the
"one boot in two came up on SD card error" half, which no earlier version of this
section could explain.

### The fix is in two places because there are two windows

- **`freeink-sdk`, `prepareEpdPower()`** deselects GPIO46 before the EPD bus is
  built, and `pinOe` becomes `-1` so LovyanGFX stops touching it at all. That
  hook is the last thing to run before `Bus_EPD::init()`, and nothing downstream
  writes the level back. Upstream PR `Free-Ink/freeink-sdk#73`; we carry it on
  our fork's `explorink` branch ([`freeink-sdk-fork.md`](freeink-sdk-fork.md)).
  `pinPwr` has to stay a real GPIO: it reaches the i80 driver as `dc_gpio_num`,
  which the IDF rejects when negative, and this board has no free pin.
- **`t5s3DeselectLoraRadio()` in `src/main.cpp`** runs before `Storage.begin()`.
  The SDK's fix runs at display init, which is ~300 ms later, so it cannot cover
  card detection. This covers card detection. Two windows, not two belts. It also
  reads the pin before writing it, which is where the reset-level measurement
  above comes from, and which makes a lost SDK fix loud instead of silent.

**Neither cuts the rail, and an earlier version did.** The rail cut was wrong
twice over: it is half of the condition that breaks the card, and it belongs to
the battery question, not this one. A rail left on keeps the receiver and the
radio powered through deep sleep, measured in both directions -- rail up across a
wake on 08-31 (`out0=0xFF bytes=1633`), rail down across a wake on 09-03
(`out0=0xFE bytes=0`). That is T-244 and it runs on its own schedule. **Do not
couple them again.**

**Verified on hardware 2026-09-03**, release branch with both halves: the boot
line above, `SD card detected`, settings and the missing-tile list loaded, a
19,855-byte read with the right CRC, `POST /mkdir` 200 and the directory listed
and deleted again. And a control: asserting the chip select by hand still kills
the read, so the failure mode is intact and merely no longer triggered.

**A build carrying only the SDK half, with `t5s3DeselectLoraRadio()` removed,
also worked** -- including with the rail cut, which had been the dead state. So
the SDK fix alone restores the card after boot; the firmware half is for the
detection window.

### Why it took three days to surface

The bug has been in the config since 2026-06-05. On this board it was masked for
two days by unrelated work: GNSS bring-up powers the shared rail and drives the
radio's reset low, which is exactly the combination the card tolerates. A BLE
regression run on 2026-09-02 at 11:08 set `mapGnssPosition` to 0 and never set it
back, the setting persists to the card, and from the next boot GNSS never ran --
so the masking went with it.

**Two things this does not explain, and they stay open.** The first failure was
at 15:23, four hours after the setting changed, and the archived build at 14:41
rendered the map from card tiles in between. Nobody read the expander in that
window, so the rail state then cannot be recovered. And the earlier write-up
blaming `gnss.end()` on map exit for cutting the rail is **mechanically
impossible**: `MapActivity` only starts GNSS when `mapGnssPosition != 0` and only
calls `gnss.end()` if it started it.

### The same class, upstream, the same day

`Free-Ink/freeink-sdk#72` fixed an EPD driver pulsing a pin that also gates the
SD rail on the OnePage board, merged 2026-09-02. An EPD driver writing a pin it
does not own, with the SD card as collateral. Ours is the second instance.

### The worst part is not the bus

The firmware **reported failure and the card changed anyway.** `fsck.vfat` found
an orphaned long-filename part for `crash_report.txt` and 64 KB of a `TIB1` tile
in a cluster chain no directory entry pointed at. Which call produced each is
inferred; what is certain is that a rename returned an error and had renamed the
file, and that tile bytes reached the card with nothing pointing at them. No
crossed files, no broken chains, so the volume survived -- that is luck, not
design. T-245, and it outlives this bug.
`../../docs/crashes/2026-09-02-t5s3-card/` has both.

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

## Serial usually resets the board, and you cannot rely on either outcome

**It resets on some opens and not on others, and what decides it is not known.**
Both have been measured on this board:

- **2026-09-02, five opens, every one reset it.** A plain `pyserial` open with
  `dtr = False` / `rts = False` before `open()` still restarted it -- the log
  timestamps went back to `[411]` each time.
- **2026-09-03, several opens, none reset it.** A plain `cat /dev/ttyACM0` and a
  default `serial.Serial(...)` both attached to a **running** board: the first
  line seen was at `[5578]`, and a later one at `[110393]`. The boot log was
  gone and could not be retaken.

The port is USB Serial/JTAG, not a UART bridge with real modem lines, so there is
no line to hold either way. **Do not plan on either outcome.** Two lost boot-log
measurements on 09-03 came from assuming the 09-02 behaviour, including the one
reading that would have said whether the LoRa radio was powered during BUG-037.

**So take the boot deliberately: open the port first, then cause the reset
yourself.** This resets it from the same handle, no second opener fighting for
the port:

```python
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3)
s.setDTR(False); s.setRTS(False); time.sleep(0.1)
s.setDTR(True);  s.setRTS(False); time.sleep(0.1)
s.setDTR(False); s.setRTS(True);  time.sleep(0.1)
s.setDTR(False); s.setRTS(False)
s.reset_input_buffer()      # then read; the boot log starts at [410]
```

Confirmed 2026-09-03: that sequence produced a boot from
`[410] [INF] [BTN] User button` onward. `esptool --after hard-reset chip-id`
also works and is the fallback, but it needs the port to itself, so a capture
has to be closed and reopened around it -- which is the thing to avoid.

**One capture per boot still holds**, for the other reason: reopening to "check"
may reset the board and cost the state being measured.

**A third data point, and a worse one: after a cold power-on, commands stopped
arriving.** 2026-08-31, USB unplugged 21 s, then replugged. **The board kept
running throughout** -- the battery is connected (**assumed, from conversation;
not confirmed on the device**), so removing USB drops nothing
(corrected 2026-09-01; this note first said no battery was attached). The
device logged happily for 271 s across **four** separate port opens -- so
device-to-host was fine and none of those opens reset it -- while **every command
sent to it was dropped**. Not one `CMD:` reply, including read-only ones. An
`esptool ... --after hard-reset` fixed it immediately and the same script then
worked first try.

**Diagnosed 2026-08-31, and it was two faults wearing one symptom.** The
firmware now logs the head byte it is refusing to consume, and it named it: `0x5B`
(`[`) with 17 bytes pending -- the first character of this firmware's own log
lines, arriving on its own RX. So the peek trap really was one of the causes, and
the loop now also drains such a byte after five seconds. **The drain does
recover the console unattended, measured 2026-09-03** -- see "The drain works,
and it takes about a minute" below. The other cause was **deep sleep**: `CMD:GNSS PROBE` answered
`reset=DEEPSLEEP`, so the board had put itself to sleep and the vanished device
node was that, not an unplug. At least today's dropouts are therefore auto-sleep
and not a bus fault, which is what this section previously suspected.

Where our own output on our own RX comes from is still unexplained. A loopback in
the USB Serial/JTAG peripheral would do it; so would something on the host
writing back what it read. **Open**, and cheap to narrow: the drain now dumps the
byte, so extend it to dump all of them once and the content will say whether it
is our own log text. Two samples now, both `0x5B` (`[`): 17 bytes pending
2026-08-31, about 15 bytes 2026-09-03. Small, and the same head byte twice.

### The drain works, and it takes about a minute

**Measured 2026-09-03 on the T5 S3 Pro**, firmware `0.2.0-t5s3pro`, over
`/dev/ttyACM0` with pyserial. The board had been up 170 s and was logging
normally. The first command sent produced the drain line:

```
[169589] [ERR] [MAIN] serial head byte 0x5B ([), 45 pending, unconsumed for 5 s
                      -- draining it; every CMD: was being ignored
```

Then **every `CMD:` went unanswered for 69 s**, read-only ones included -- a
`CMD:LIGHT` query answered nothing. At millis 238642, 238695 and 238748 all
three queued commands answered, in the order they were sent.

Three facts fall out of that, and the third is the useful one.

**The drain recovers the console on its own.** This is what was `[open]` until
now. No reset, no reflash, no host action.

**It costs about 5 seconds per foreign byte.** The stuck-head block reads
**one** byte per trigger (`src/main.cpp`, the `logSerial.read()` after the 5 s
timeout), and the trigger needs 5 s of the *same* head byte, so each byte
restarts the clock. Whitespace is free -- the peek loop above it consumes
`\n`, `\r`, space and tab without limit. The 45 pending bytes included the
~30 bytes of the command just written, so roughly 15 foreign bytes cleared in
69 s. That is 5 s per byte, arithmetic agreeing with the code.

**Commands are queued, not lost.** They sit behind the foreign bytes in the
same RX ring and all run when the head clears. So a host script that "got no
reply" has not failed -- its commands fire minutes later, at a moment nothing
on the host is expecting them. A screenshot or a `CMD:SETTING` landing that
late is the shape of a test that measured the wrong state.

**And a leading newline does not clear this wedge.** Every command in this run
was framed `\nCMD:...\n`, which is the fix T-113 asks `mapcmd.py` for, and the
console stayed wedged for the full 69 s. Read off the code: the whitespace loop
only consumes whitespace **at the head**, and a newline the host writes lands at
the **tail**, behind the foreign bytes. The newline clears a wedge the host
itself left -- its own torn partial line -- and nothing else.

Only one drain line is ever logged (`reportedStuckHead` is a `static bool`), so
the log says a wedge started and never says it ended. **Do not read a single
drain line as a single drained byte.**

**What was established before that was the symptom, not the layer.** The first version of this
note called it "the CDC came up transmit-only", which places the fault in USB
without evidence. Review named an alternative this firmware documents against
itself: the command reader consumes only whitespace before a `'C'`, so **any
single other byte sitting at the head of the RX buffer blocks every command for
the rest of the session** (`src/main.cpp`, the peek loop). A torn first write into
a freshly enumerated endpoint does that, and so does ModemManager probing a new
ACM device with `AT` -- and a hard reset clears both, so recovery discriminates
nothing. Reset-on-open is no discriminator either: a healthy session on the same
day also failed to reset on open.

Two things make the next occurrence diagnosable instead of a coin flip, one of
them now in the firmware: the peek loop **logs the head byte** it is refusing to
consume once it has sat there five seconds, and on the host side
`journalctl -u ModemManager` covers the enumeration window.

Either way: a silent board is not necessarily a hung board. Check whether it is
still logging before assuming a crash, and reset the chip before blaming the
build. Possibly the same fault as the three USB dropouts below.

**Data read right after opening the port can be stale, and it cost a whole
conclusion.** The kernel buffers what the device sent while nothing had the port
open and hands it over on open, so the first lines can come from a *previous*
boot, mixed with live output -- and their `millis` do not belong to the current
one. On 2026-08-31 that produced a confident argument that the board had never
lost power, built on timestamps from the wrong boot. Read the reset cause from
the device instead: `CMD:GNSS PROBE` reports it, and the chip is the only
authority on its own boot.

**A second data point, 2026-08-31: one open did not reset it at all.** The GNSS
pass opened the port twice in a row; the first open produced a full boot log
from millis 401, the second continued from millis 150,627 with the map still up.
So "usually" is the right word and a script must not assume either -- the second
run's counters were the first run's, which is confusing until you notice why.

Opening `/dev/ttyACM0` normally resets the S3, even with DTR and RTS both driven
low before the port is opened: the USB-JTAG-serial peripheral is on the SoC
itself, so there is no external auto-reset circuit to defeat. **But not every
open does it** -- one open in this session landed on a device whose uptime
counter carried on at `[231152]`, with the map still on screen. So a `CMD:` may
arrive at a freshly booted device on Home, or at whatever was already running,
and a script has to handle both: watch for the boot banner, and drive to the
screen you need rather than assuming.

The board also disappears from the USB bus entirely when it sleeps or is
powered down -- `/dev/ttyACM*` vanishes and `lsusb` shows no `303a` device. That
is not a crash. BOOT (GPIO0) is the profile's power button and the deep-sleep
wake source.

## Explained: it reboots on leaving the map

**Settled 2026-09-01, and it is not the board.** A second restart the next day,
on the same code path, was traced from a coredump to a NULL event callback in
NimBLE's own shutdown -- an upstream defect in NimBLE-Arduino that our
`BlePositionServer::end()` triggers. Cause, evidence and fix options are in
[`ble-deinit-crash.md`](ble-deinit-crash.md); the mechanics of reading a
coredump are in [`crash-reporting.md`](crash-reporting.md). It cannot be
*proven* that the 31 August restart below is the same fault, because our
coredump overwrote its one before anyone extracted it. Everything else lines
up: same function, same stop before the `BLEPOS heap:` line, phone connected
both times. The original write-up stays below, with its two dead hypotheses
marked.

**2026-08-31, not diagnosed at the time.** The maintainer opened, closed and used the map
menu repeatedly and the device restarted. This is what the card's
`/crash_report.txt` held afterwards, verbatim -- kept here because that card is
no longer in the board and the next session will not have it:

```
TrailInk version: 0.2.0-t5s3pro

Panic reason:

Last logs:
[869536] [INF] [MAP] freshness: 0 tile(s) out of date
[869774] [INF] [BLEPOS] conn params: interval 12 units (15 ms), latency 0, timeout 500
[870011] [INF] [BLEPOS] conn params: interval 24 units (30 ms), latency 0, timeout 500
[871473] [DBG] [MAP] menu gesture: opening map menu
[871475] [DBG] [MAP] menu backdrop 11360 bytes (284x306), free heap 122028
[871482] [DBG] [GFX] Time = 14215 ms from clearScreen to displayBuffer
[874473] [DBG] [PWR] Going to low-power mode (80 MHz)
[875801] [DBG] [PWR] Restoring normal CPU frequency
[881802] [DBG] [PWR] Going to low-power mode (80 MHz)
[882688] [DBG] [PWR] Restoring normal CPU frequency
[884824] [DBG] [ACT] Exiting activity: Map
[884854] [DBG] [MTS] missing tile list saved (2 entries)
[1] [DBG] [UI] Using Lyra theme

Stack memory:
```

What that says, and what it does not:

- **No panic reason and no stack.** The reboot-from-panic flag was set --
  `HalSystem::checkPanic()` only writes this file when it is -- but the message
  is empty. That does not look like a C++ exception or an `abort()`, both of
  which carry text. It fits a reset below the firmware: a watchdog, a brownout,
  or one where the panic wrapper never ran.

  **Two of those three are now ruled out, 2026-09-01.** A watchdog and a
  brownout each set their own reset-reason hint, and
  `HalSystem::isRebootFromPanic()` (`lib/hal/HalSystem.cpp:149`) accepts only
  `ESP_RST_PANIC` and `ESP_RST_CPU_LOCKUP` -- so neither would have written this
  file at all. **The file existing is the proof.** The third guess was the right
  one: a CPU exception, where the panic wrapper never runs. The empty stack is
  not a clue either, it is empty on every Xtensa crash by construction
  ([`crash-reporting.md`](crash-reporting.md), "Gap 1").
- **The restart lands exactly on leaving the map.** `Exiting activity: Map`,
  the missing-tile list saved, then the next boot's first line.
  `MapActivity::onExit()` is also where `BlePositionServer::end()` deinitialises
  the whole NimBLE stack.
- **A 14,215 ms display operation**, right after the menu opened, against 2.7 s
  for a map redraw and 34 ms for Home. Treat it as a lead, not a fact: the line
  is a delta between `clearScreen` and `displayBuffer`, so a `clearScreen` long
  beforehand inflates it. Worth reading the timer's own definition before
  building anything on it.
- **The menu backdrop is exonerated**: 11,360 bytes with 122 KB internal free.
  A first guess in that session blamed heap exhaustion using the X4's numbers,
  on a board with 8 MB of PSRAM. It was wrong twice over.

**The planned next step is no longer needed.** It was to hold the serial port
open, reproduce, and read the ROM's own `rst:` line to settle watchdog versus
brownout versus software reset. The reset-reason filter above settles it for
free, from a file that was already on the card. The ROM line is still the right
tool when *no* `crash_report.txt` appears -- which, per
[`crash-reporting.md`](crash-reporting.md), is exactly the watchdog case.

**The lead was already in this section and nobody followed it.** The bullet
above notes that `MapActivity::onExit()` is where `BlePositionServer::end()`
deinitialises the whole NimBLE stack. That was the answer, written down a day
before it was found the expensive way.

Possibly related and also undiagnosed: **the board dropped off the USB bus three
times** during that session, once with auto-sleep switched off, each time
needing a long BOOT press to come back. Whether that is the same fault has not
been established.

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
