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
  user button behind the expander has no hook -- which is the same gap the
  unmerged `feat/t5s3-board-begin` branch was opened for, and part of why the
  map is half-operable here (below).
- **LovyanGFX does drive `LORA_CS` as a GPIO.** The parent repo's
  `docs/lora.md` had this `[open]` because M5GFX was not checked out. It is now:
  `Bus_EPD` drives the pins handed to it as `pin_oe` and `pin_pwr` as real
  GPIOs, and passes one to the i80 peripheral as its DC line
  (`Bus_EPD.cpp:83,85,120,129,143`). On this board both are GPIO46, which is the
  SX1262's chip select, on the SD card's SPI bus.
  **That the wiring is shared is settled; that it is a hazard is not.** The
  cited lines drive the pin *high*, and a deasserted chip select is harmless --
  whether it is ever driven low during a refresh has not been established, and
  neither has whether an SX126x in reset parks MISO high-Z. Do not read this
  bullet as a confirmed fault.

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

**A third data point, and a worse one: after a cold power-on, commands stopped
arriving.** 2026-08-31, USB unplugged 21 s with no battery, then replugged. The
device logged happily for 271 s across **four** separate port opens -- so
device-to-host was fine and none of those opens reset it -- while **every command
sent to it was dropped**. Not one `CMD:` reply, including read-only ones. An
`esptool ... --after hard-reset` fixed it immediately and the same script then
worked first try.

**Diagnosed 2026-08-31, and it was two faults wearing one symptom.** The
firmware now logs the head byte it is refusing to consume, and it named it: `0x5B`
(`[`) with 17 bytes pending -- the first character of this firmware's own log
lines, arriving on its own RX. So the peek trap really was one of the causes, and
the loop now drains such a byte after five seconds instead of only complaining
about it. The other cause was **deep sleep**: `CMD:GNSS PROBE` answered
`reset=DEEPSLEEP`, so the board had put itself to sleep and the vanished device
node was that, not an unplug. At least today's dropouts are therefore auto-sleep
and not a bus fault, which is what this section previously suspected.

Where 17 bytes of our own output come from is still unexplained. A loopback in
the USB Serial/JTAG peripheral would do it; so would something on the host
writing back what it read. **Open**, and cheap to narrow: the drain now dumps the
byte, so extend it to dump all of them once and the content will say whether it
is our own log text.

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

## Open: it reboots on leaving the map

**2026-08-31, not diagnosed.** The maintainer opened, closed and used the map
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

**The cheapest next step is the ROM's own line.** The first thing printed at
boot names the reset cause -- `rst:0x15 (USB_UART_CHIP_RESET)` for a host
opening the serial port, and it would say `TG_WDT_SYS_RST`, `RTCWDT_RTC_RST`,
`BROWNOUT_RST` or `SW_CPU_RESET` for the ones that matter here. So: hold the
port open, reproduce, and read that line. It settles watchdog versus brownout
versus software reset without adding any code.

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
