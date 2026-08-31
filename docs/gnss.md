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

**Flashed and run on the board 2026-08-31.** Firmware `feat/t5s3-gnss`, env
`t5s3pro`, on the LilyGo T5 S3 Pro. The receiver works, the parser is verified
field by field against the wire, and the pass found something nobody was looking
for: **the receiver is already powered before any firmware asks for it.** Two
things remain unmeasured and are marked so.

## Status

| | |
|---|---|
| Receiver | Quectel L76K, on-board. GPS + GLONASS, both seen |
| Reached by | `CMD:GNSS` over the USB serial console, `env:t5s3pro` only |
| On any screen | no |
| Feeding the map | no -- the map still takes its position over BLE from the phone |
| Verified on hardware | 3D fix indoors, parser exact against the wire, SD card unaffected |
| Still open | cold-start TTFF, idle current of the powered pair |

**A working fix indoors**, on a desk, through a ceiling: `q=1`, 8 satellites
used, 19 in view, 9 tracked, best C/N0 32 dB-Hz, HDOP 2.0. That was not the
expected outcome -- an indoor attempt on the factory firmware got nothing
(2026-08-31, earlier the same day) -- so the antenna and its placement are
better than assumed. The receiver says so itself in words: it emits
`$GPTXT,01,01,01,ANTENNA OK`.

**Time arrives before a fix.** `utc` is populated while `q=0`, because the date
and time come off RMC well before a solution does. Verified in the second run,
and it matters: the safety budget in the parent repo's `safety-concept.md`
wants a wall clock, and this supplies one without a fix and without a phone.

The map is deliberately untouched. Position reaching the map is a separate
piece of work, and it is not "GNSS instead of BLE": it is one abstraction over
a position source with two implementations, which is what the parent repo's
`docs/lora.md` already argues for under "What can be built today".

## The receiver

**L76K**, identified from the factory firmware on 2026-08-31. Quectel's own
part, GPS + GLONASS + BeiDou, NMEA over UART.

- **9600 baud, 8N1 is correct**, confirmed on hardware: checksum-clean NMEA
  from the first read, no reframing, `cserr` at 0 across a rail-cycled session.
  The guess held, so the datasheet is no longer needed to unblock anything.
- **GPS and GLONASS, and only those two.** `GPGSV` and `GLGSV` arrive; no
  Galileo or BeiDou GSV was seen and `GNGSA` carries system ids 1 and 2 only.
  An `inview` of 19 was 10 GPS plus 9 GLONASS, which is also what confirms the
  parser's per-talker summing works rather than double-counting.
- **It sends more than we parse**, all checksum-valid, currently counted and
  dropped: `GNGSA`, `GNGLL`, `GNVTG`, `GNZDA`, `GNDHV`, `GPTXT`. Two are worth
  picking up later -- `GNZDA` carries date *and* time in one sentence, which
  would remove the GGA-plus-RMC stitch described below, and `GPTXT` is where
  `ANTENNA OK` comes from, a health line with no equivalent anywhere else.

## Two pins, and they are UART0's

```
freeink-sdk/libs/hardware/BoardT5S3/include/BoardT5S3Pins.h:20  GPS_RXD 44
                                                          :21  GPS_TXD 43
```

Read as **MCU-side**, and that reading is **confirmed**: `Serial1.begin(9600,
SERIAL_8N1, 44, 43)` produces clean NMEA, so `RXD 44` is where the S3 receives
and it goes to the receiver's TX. The header's names never actually said whose
RX was meant, and a swap would have looked exactly like a dead receiver -- worth
having settled rather than left as a coin flip for the next person.

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

### The rail is already up at boot, before any firmware writes to it

**Measured 2026-08-31, and it was not what anyone was looking for.** The
receiver is powered from the moment the board boots. Nothing in the firmware
enables it; it simply comes up.

The evidence is a pair of runs, because no single reading proves it:

- **Run 1.** Fresh boot at millis 401, `CMD:GNSS ON` at about millis 45,000, and
  a full 3D fix **531 ms later** with 8 satellites used. A receiver powered at
  the moment of that command cannot do that: it needs its own firmware boot
  first, and then even a hot start is seconds. So it was already tracking.
- **Run 2.** `CMD:GNSS OFF`, rail down for 45 s, then `ON` again -- and **no fix
  after 40 s**, `q=0`, tracked climbing 3 to 6. So the rail genuinely does
  control the module, and there is no backup supply keeping its ephemeris across
  three quarters of a minute.
- **A third run, after a reflash**, reproduced the same signature without being
  asked to: `ttff=526` on the first enable, and again exactly one checksum error
  at the first read. Two independent boots giving a half-second fix is not a
  coincidence.

Those two together leave one reading: at run 1's first `ON`, the part was
already powered by the board's own default state. A third, smaller sign agrees
-- the first read of the session logged one checksum error, exactly what opening
a UART mid-sentence produces, while a rail-cycled start logged none.

**The mechanism is inferred, not proven.** The PCA9535 comes out of reset with
every pin an input, so the enable line is high-Z and something on the board
pulls it up. That is consistent with everything observed and it is not measured;
what is measured is the effect.

**The consequence is a power leak nobody has costed.** Since this board's
bring-up, every boot has powered the GNSS receiver *and* the SX1262, all day,
whether the map was open or the device was sitting on Home. `disableGpsLora()`
never being called (above) is therefore not merely a missing button hook -- it
is current. How much is unmeasured and is the open item this file ends on.

It also reframes the SD-card result below: previous sessions were unknowingly
running with the rail up and `LORA_RST` floating, and the map drew and tiles
loaded then too.

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

### Verified against the wire, field by field

`CMD:GNSS RAW ON` and a status query one second apart, so the same solution can
be read raw and parsed. Every field matched: the degree-and-minute conversion
for both latitude and longitude (agreeing to 2e-6 degrees, which is the one
second of drift between the two samples), the hemisphere signs, HDOP,
satellites used, altitude, and the UTC assembly from a GGA time plus an RMC date
-- that last one **exact to the second**.

Coordinates are deliberately not reproduced here. The board sat at the
maintainer's address and `CLAUDE.md`'s screenshot rule treats an exact `lat lon`
in a log the same as a rendered map of the same place. The arithmetic is
`degrees + minutes / 60` on a `ddmm.mmmmm` field split at the decimal point;
anyone can re-run the check against their own fix.

One thing the check caught: **`CMD:GNSS RAW` was emitting lines that looked
like NMEA and were not.** The sink was called after the parser had already
terminated the string at `*`, so every logged sentence was missing its
checksum -- unusable in any NMEA tool, while looking perfectly fine in a log.
Fixed by calling the sink first.

**Fixed and confirmed on the board**, same day: 85 logged sentences, every
checksum recomputed by an XOR outside the firmware, **zero mismatches**. Worth
noting how that was checked -- comparing the firmware's output against the
firmware's own parser would have agreed with itself no matter what, so the
verification was done with a separate implementation. The archived binary is
`docs/firmware-builds/t5s3pro-b8276d9e-gnss-confirmed.bin` in the parent repo.

### A blocking render starves the UART

Also out of run 1, and it matters for anything that eventually feeds the map. A
full map redraw blocks the loop for **4,017 ms**. `poll()` is only called from
that loop, the driver's RX buffer is 256 bytes, and the receiver sends roughly
600 bytes a second -- so the buffer overruns in well under half a second and
stays overrun.

Measured across that render: about 30 sentences lost and one extra checksum
error. Harmless for a bring-up console, not harmless for a position source. The
fix is not "poll more often", because there is nowhere to poll from during a
blocking render; it is a bigger RX buffer (`setRxBufferSize()`), or the UART on
its own task, or an event-driven read. That decision belongs to the work that
puts GNSS behind the map's position source, not here.

Cost, measured off the two builds: flash went from 58.3 % to 58.5 % of the
6.4 MB app partition, about 13 kB for the library plus the command. Zero
compiler warnings.

## What the hardware pass found, in the order it was run

Run 2026-08-31, one serial session per run because every port open can reset an
S3. The board is a LilyGo T5 S3 Pro on a desk indoors.

1. **`CMD:GNSS ON` answers `GNSS_OK:on`.** Yes. The expander responds and the
   rail write succeeds. `CMD:GNSS OFF` and a re-enable also work.
2. **The SD card still reads with the rail up.** **Yes, and this was the check
   that mattered.** `CMD:GOTO_MAP` with the rail powered and the SX1262 held in
   reset read **1,393,825 bytes** off the card: `4 tiles ok, 0 missing (mask
   0x0), 2684 ways, 13 places`, `3194 ms in the card`, 8 CRC32 skips, and the
   frame drawn. No mount failure, no corrupt tile. So the `LORA_CS` collision,
   real as it is in the code, does **not** break the SD bus in this
   configuration. It also did not break it in earlier sessions, which ran with
   the rail up and the reset floating without knowing it.
3. **`bytes` climbs.** Yes, monotonically across every status: 11,644 to 21,820
   to 39,204 within one session. The pin direction as written is correct and no
   swap was needed.
4. **`cserr` stays at 0 while `sent` climbs.** Yes on a rail-cycled start. The
   two errors seen in run 1 are both explained and neither is the wiring: one
   from opening the UART mid-sentence on an already-running receiver, one from
   the buffer overrun during the 4 s map render.
5. **Outdoors: tracked and bestsnr rise, then a fix.** Got a **3D fix indoors**,
   which the pass did not expect, so the outdoor half was never needed to prove
   the receiver works. **The cold-start TTFF is still not measured**: run 2's
   45 s outage did not clear enough state for a true cold start, and 40 s
   indoors was not enough to reacquire. Open.
6. **The idle cost of the pair.** **Not measured.** Open, and now more
   interesting than it was: the pair is powered on every boot regardless of
   what the firmware wants, so this is not a feature's cost, it is the board's
   floor.

## Open

- **Idle current of the GNSS + LoRa pair**, against the BQ27220, and how much of
  it the board pays on every boot before anything asks. Name the instrument.
  This is the one with a consequence for the product.
- **Cold-start TTFF**, outdoors, from a genuinely cold receiver. Needs a longer
  power-off than 45 s, or a receiver command to clear the almanac.
- **Whether holding `LORA_RST` low is what keeps the SX1262 off the SPI bus**, or
  whether the bus would have been fine anyway. Check 2 passed with the reset
  asserted, and earlier sessions passed with it floating, so the reset's
  contribution is **unproven either way**. The SX126x datasheet should say
  whether reset parks MISO high-Z.
- **The RX buffer under a blocking render** -- which of the three fixes above,
  decided by the work that feeds position to the map.
- Whether the receiver can be left powered during a ride or should be
  duty-cycled. A power question and, given the shared rail, a LoRa question too.
