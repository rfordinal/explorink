# GNSS

The LilyGo T5 S3 Pro carries a GNSS receiver on board. This file is what the
firmware does with it, what the board forces on any design, and what is still
unverified. The board itself is
[`lilygo-t5s3-bringup.md`](lilygo-t5s3-bringup.md); the radio next to it is the
parent repo's `docs/lora.md`.

**GNSS** is the generic name for satellite positioning -- GPS is one
constellation inside it, alongside GLONASS, Galileo and BeiDou. **NMEA 0183** is
the text protocol nearly every receiver speaks: one `$`-prefixed sentence per
line, comma-separated fields, an XOR checksum after a `*`. **TTFF** is time to
first fix, the wait from power-on to the first usable position.

Nothing in this file has run on hardware yet. Written 2026-08-31, built clean
for `env:t5s3pro`, flashed nowhere. Every claim below is read off the code or
off a vendor page, and the section that says so says which.

## Status

| | |
|---|---|
| Receiver | Quectel L76K, on-board |
| Reached by | `CMD:GNSS` over the USB serial console, `env:t5s3pro` only |
| On any screen | no |
| Feeding the map | no -- the map still takes its position over BLE from the phone |
| Verified on hardware | **nothing** |

The map is deliberately untouched. Position reaching the map is a separate
piece of work, and it is not "GNSS instead of BLE": it is one abstraction over
a position source with two implementations, which is what the parent repo's
`docs/lora.md` already argues for under "What can be built today".

## The receiver

**L76K**, identified from the factory firmware on 2026-08-31. Quectel's own
part, GPS + GLONASS + BeiDou, NMEA over UART.

- **9600 baud, 8N1** is what the firmware asks for
  (`src/main.cpp`, `CMD:GNSS ON`). **[open]** -- taken as the L76K's documented
  default and not yet checked against Quectel's datasheet. If the receiver
  answers with garbage rather than silence, this is the first suspect.
- An indoor test on 2026-08-31 returned no fix, which is the expected result
  for a bare receiver under a ceiling and says nothing either way about the
  wiring.

## Two pins, and they are UART0's

```
freeink-sdk/libs/hardware/BoardT5S3/include/BoardT5S3Pins.h:20  GPS_RXD 44
                                                          :21  GPS_TXD 43
```

Read as **MCU-side**: `RXD 44` is where the S3 receives, so it goes to the
receiver's TX. The header's names do not actually say whose RX is meant, and
the two are trivially easy to swap -- **the symptom of a swap is identical to a
dead receiver**, silence on both. So a bring-up that sees no bytes at all tries
the other assignment before suspecting anything else.

GPIO43 and GPIO44 are the ESP32-S3's default `U0TXD` / `U0RXD`. They are free
here only because this env runs its console over USB CDC
(`ARDUINO_USB_CDC_ON_BOOT=1`, and `logSerial` is `HWCDC& = Serial` in
`lib/Logging/Logging.h:35`). One consequence: the ROM bootloader still logs on
GPIO43 at every reset, so the receiver's RX takes a few lines of unsolicited
text on each boot. Harmless -- a receiver discards anything that is not a
command it knows.

## The power rail is shared with the LoRa radio, and that has a sharp edge

One expander pin gates both parts:

```
BoardT5S3Pins.h:70  PCA9535_IO00_LORA_GPS_EN 0
```

So **the GNSS receiver cannot be powered without also powering the SX1262.**
Any power budget is for the pair, never for the receiver alone.

That would be merely inconvenient if it were not for the panel:

```
freeink-sdk/.../BoardT5S3/src/LilyGoT5S3LgfxConfig.cpp:162  pinOe  = T5S3_LORA_CS   (GPIO46)
                                                      :166  pinPwr = T5S3_LORA_CS   (GPIO46)
```

`docs/lora.md` in the parent repo listed the consequence as `[open]` because
M5GFX was not checked out. It is checked out now, and it is **confirmed**:
LovyanGFX's `Bus_EPD` drives both fields as real GPIOs, and hands one of them to
the i80 peripheral as its DC line.

```
.pio/libdeps/t5s3pro/M5GFX/src/lgfx/v1/platforms/esp32/Bus_EPD.cpp:83   gpio_hi(_config.pin_oe)
                                                                 :85   gpio_hi(_config.pin_pwr)
                                                                 :91   gpio_lo(_config.pin_pwr)
                                                                 :120  pinMode(pin_oe, output)
                                                                 :129  bus_config.dc_gpio_num = pin_pwr
                                                                 :143  pinMode(pin_pwr, output)
```

Read off the M5GFX 0.2.28 source, not measured. What it means: once the rail is
up, **every panel refresh asserts the radio's chip select** -- and the radio
sits on the same SPI bus as the SD card (`SCLK 14 / MOSI 13 / MISO 21`,
`BoardT5S3Pins.h:27-29`), where a second device driving MISO corrupts a tile
read.

**The defence: hold the SX1262 in reset.** `LORA_RST` (GPIO1) driven low parks
the part, and its MISO with it. `gnssPowerEnable()` in `src/main.cpp` does that
before it touches the rail, every time.

### Why the firmware does that itself instead of trusting the SDK

The SDK has a function for exactly this -- `BoardT5S3::disableGpsLora()`, which
drives `LORA_RST` low and the rail off (`BoardT5S3.cpp:101-113`) -- and
`BoardT5S3::begin()` calls it (`:123`).

**Nothing in this firmware calls `BoardT5S3::begin()`.** Grep for it across
`src/` and `lib/` and there is not one hit; the only code using the expander is
`LilyGoT5S3LgfxConfig.cpp`, for the EPD power pins. So `disableGpsLora()` has
never run on this board, `LORA_RST` is undriven at boot, and the rail enable
sits in the PCA9535's power-on default of "all pins inputs".

That makes the reset assertion mandatory in our own code rather than inherited.
It is also why `gnssPowerEnable()` writes the expander's output register
*before* switching the pin to output: the other order would briefly drive
whatever the register happens to hold, which on a cold boot is high.

There is an unmerged branch (`feat/t5s3-board-begin`) that adds the
`BoardT5S3::begin()` call for the sake of the user button. Nothing here depends
on it either way, which is the point.

## The serial command

Devel-only, `-DENABLE_GNSS_CMD=1` in `env:t5s3pro` and in no release env. Two
independent reasons, and either one is enough under `CLAUDE.md`'s security rule:
it **powers a radio rail**, and its reply is **the rider's exact position**.

```
CMD:GNSS           ->  GNSS_FIX:... | GNSS_NOFIX:... | GNSS_OFF
CMD:GNSS ON        ->  GNSS_OK:on          powers the rail, opens the UART
CMD:GNSS OFF       ->  GNSS_OK:off         closes the UART, drops the rail
CMD:GNSS RAW ON    ->  GNSS_OK:raw=1       every sentence to the log
CMD:GNSS RAW OFF   ->  GNSS_OK:raw=0
```

A status reply is one line of `key=value`, so a host script can grep it:

```
GNSS_FIX:q=1 used=9 inview=14 tracked=11 bestsnr=38 lat=48.148000 lon=17.107000
  alt=191.0 hdop=1.20 speed=0.0 course=0.0 utc=1788178462 ttff=42313 age=612
  sent=1204 cserr=0 bytes=98304
GNSS_NOFIX:q=0 inview=11 tracked=4 bestsnr=19 utc=1788178462 uptime=63120
  sent=214 cserr=0 bytes=17408
```

(Wrapped here for reading. On the wire it is one line.)

**`used`, `inview` and `tracked` are three different counts, and the gaps
between them are the diagnosis.** `inview` comes from the almanac -- what the
receiver believes is above the horizon, which it will happily report from
indoors. `tracked` counts satellites reporting a non-zero C/N0, so it is what
the antenna can actually hear. `used` is what went into the solution. Under a
ceiling `inview` stays healthy while `tracked` collapses and `bestsnr` sits in
the teens; that pair, not the absence of a fix, is what says "no sky".

**`cserr` is the wiring check.** Checksum failures at zero with sentences
climbing means the UART is clean. Sentences climbing *and* `cserr` climbing with
it means the baud rate is wrong -- the framing is close enough to produce lines
but not to produce correct ones. Both at zero with `bytes` climbing means
something is talking and it is not NMEA.

### Privacy

`GNSS_FIX` prints latitude and longitude to six decimals, which is metres.
`CLAUDE.md`'s screenshot rule covers this: a log or a screenshot carrying a real
fix taken at the maintainer's address does not leave the local machine, and a
debug line printing exact coordinates is the same leak as a rendered map of the
same place. Redact or discard, do not publish.

## The parser

`lib/Gnss/` -- `Gnss.h`, `Gnss.cpp`, `library.json`. Written to be moved
upstream into `freeink-sdk/libs/hardware/` next to `Rtc` and `Imu`, so it obeys
that neighbourhood's conventions:

- **Board-agnostic.** Pins, baud and the power rail all arrive through
  `GnssConfig`; the rail is a plain function pointer, because on this board it
  lives behind an I2C expander the library has no business knowing about.
- **It never logs.** No SDK hardware library does -- `Rtc` and `Imu` contain no
  print of any kind. The caller reads the accessors and decides what to say.
- **It never blocks and never allocates after `begin()`.** `poll()` consumes
  what the UART already buffered and returns. Buffers are fixed members.

Sentences understood: **GGA** (fix quality, satellites used, position,
altitude, HDOP, time), **RMC** (date, ground speed, course), **GSV**
(satellites in view and their C/N0). Everything else is checksum-verified,
counted, offered to the raw sink and dropped.

Three details worth knowing before changing it:

- **Time and date arrive in different sentences.** GGA carries `hhmmss`, RMC
  carries `ddmmyy`, and a cold receiver emits a valid GGA time long before it
  has a date. `fix.utc` stays 0 until both exist, rather than reporting a
  plausible wrong day.
- **GSV is a cycle, not a sentence.** Each constellation sends `messageCount`
  sentences per sweep. Signal figures accumulate across the cycle and are
  committed only at its end, so a reader never sees a half-scanned sky. Talkers
  are tracked separately (`GP`, `GL`, `GA`, `GB`, ...) because GSV field 3 is
  per-constellation and summing it needs per-talker state.
- **Position is `ddmm.mmmm`, and the field is not fixed-width.** The last two
  digits before the decimal point are minutes; the split is found from the
  decimal point rather than assumed, because latitude has two degree digits and
  longitude has three.

Cost, measured off the two builds: flash went from 58.3 % to 58.5 % of the
6.4 MB app partition, about 13 kB for the library plus the command. Zero
compiler warnings.

## What a hardware pass has to check

In this order. Each one can invalidate the next.

1. **`CMD:GNSS ON` answers `GNSS_OK:on`.** If it answers `GNSS_ERR`, the
   expander did not respond and nothing else in this list matters.
2. **The SD card still reads with the rail up.** Open the map and let it pull
   tiles. This is the one that tests the `LORA_CS` collision above, and it is
   the reason the reset assertion exists. A corrupted tile read or a mount
   failure here means the mitigation is not enough.
3. **`bytes` climbs.** If it stays at 0, swap `rxPin` and `txPin` and repeat
   before suspecting anything else.
4. **`cserr` stays at 0 while `sent` climbs.** If both climb, the baud rate is
   wrong -- try 115200, then read Quectel's datasheet rather than guessing
   further.
5. **Outdoors: `tracked` and `bestsnr` rise, then a fix.** Record TTFF from a
   cold start. Indoors this step cannot succeed and is not evidence of a fault.
6. **The idle cost of the pair**, GNSS plus the SX1262 in reset, against the
   BQ27220. Name the instrument in the result. This is step 2 of `lora.md`'s
   bring-up order and it is still unmeasured.

## Open

- L76K default baud and its NMEA sentence set, against Quectel's datasheet.
- Whether holding `LORA_RST` low is actually enough to keep the SX1262 off the
  SPI bus. Read off the SX126x signalling shape, never measured. The datasheet
  should settle whether reset parks MISO high-Z.
- Idle current of the GNSS + LoRa pair.
- Whether the receiver can be left powered while the map draws, or has to be
  duty-cycled. That is a power question and a bus question at once.
