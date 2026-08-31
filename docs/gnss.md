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
`t5s3pro`, on the LilyGo T5 S3 Pro. The receiver works and the parser reads it
correctly for the one fix it was checked against.

**This file was rewritten the same day after an adversarial review broke three
of its conclusions.** What survived, what did not, and what the review found in
the code are all below. The short version: the receiver was already powered and
tracking when the firmware first enabled the rail (that holds); "therefore it is
powered at every boot by a board default" does **not** follow from the evidence
collected; the SD-card test could not have failed; and the sentence-loss figure
was wrong by a factor of three. Numbers here are now either log-derived and
shown, or labelled open.

## Status

| | |
|---|---|
| Receiver | Quectel L76K, on-board. GPS + GLONASS, both seen |
| Reached by | `CMD:GNSS` over the USB serial console, `env:t5s3pro` only |
| On any screen | no |
| Feeding the map | no -- the map still takes its position over BLE from the phone |
| Verified on hardware | 3D fix indoors; parser correct for one N/E fix on one date; the rail's ON/OFF path works |
| Still open | cold-start TTFF, idle current, whether the rail is on by board default, whether SPI contention is real |

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
- **GPS and GLONASS in this configuration.** `GPGSV` and `GLGSV` arrive; no
  Galileo or BeiDou GSV was seen and `GNGSA` carries system ids 1 and 2 only.
  That is an observation of what the module emits as shipped, not a statement
  about what the part can do -- the constellation set is configurable on an L76K
  and the vendor lists BeiDou.
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

**The intended defence: hold the SX1262 in reset.** `LORA_RST` (GPIO1) driven
low should park the part and its MISO with it. `gnssPowerEnable()` in
`src/main.cpp` does that before it touches the rail, every time.

**Whether any of this matters is untested**, and the hardware pass did not test
it -- see "What the hardware pass found", check 2. Two things are unestablished:
whether GPIO46 is ever driven *low* during a refresh at all (`Bus_EPD` is seen
driving it high, and a deasserted chip select is harmless), and whether reset
parks MISO high-Z on an SX126x. The reset assertion stays because it is free and
the failure it guards against is a corrupt tile read, not because it has been
shown to do anything.

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

### The receiver was already powered when the firmware first enabled the rail

**That much is measured. What it implies about "every boot" is not**, and the
first version of this section claimed the larger thing. Review broke it the same
day; this is the corrected account.

**What holds.** At run 1's first `CMD:GNSS ON` the receiver was already powered
and tracking. Two independent signs, neither of which is the TTFF number:

- The UART opened **mid-sentence**: exactly one framing-class error on the very
  first read, while a rail-cycled start later the same session logged none. A
  receiver that had just been powered 100 ms earlier (`GnssConfig::powerSettleMs`)
  is not in the middle of transmitting a sentence.
- The first status reported `q=1` with 8 satellites used. A receiver powered at
  the moment of that command cannot be there half a second later.

**Why the TTFF number is not evidence.** `ttffMs_` is `lastFixMs_ - beginMs_`
with `beginMs_` set after the UART opens (`lib/Gnss/src/Gnss.cpp`,
`begin()`/`parseGga`). On a receiver that is *already fixed* and emitting a 1 Hz
cycle led by GGA, that measures the phase between our UART open and the next
GGA -- a uniform draw over roughly 0 to 1000 ms. Any value under about 1.2 s
says "already tracking" and nothing more. The 531 ms and 526 ms readings landing
close together is chance, not reproduction, and the earlier claim that "two
independent boots giving a half-second fix is not a coincidence" was numerology.

**Why the third run was not independent at all.** `gnss_pass2.py` ends on
`CMD:GNSS ON` and never sends `OFF`, so run 2 left the rail **firmware-commanded
on**. The reflash that followed reset the S3 and not the PCA9535, which sits on
the 3.3 V rail and latches its output and direction registers until it loses
power. So the confirm run found an already-tracking module because run 2 had
turned it on and nothing turned it off.

**Why "the board powers it by default" does not follow.** The same latching
argument applies to run 1. No reset in any of the three runs power-cycles the
expander: a serial-open reset, a reflash and `ESP.restart()` all reset only the
S3. The board ran its **factory firmware** earlier the same day, and that
firmware drives this rail. If the board was not physically unplugged in between,
run 1's "already powered" is fully explained by a latched write from that
session -- no pull-up and no board default needed. Run 1's "fresh boot at millis
401" was itself a warm reset caused by the test script opening the port, which
is precisely the kind of reset the expander survives.

**Why the "no backup supply" conclusion is withdrawn.** Run 2 dropped the rail
for 49.7 s (not the 45 s previously written) and got no fix in the following
40 s, with 3 then 6 satellites tracked and best C/N0 27. A fix needs four usable
satellites. A backup-powered hot start indoors at 3 to 6 tracked can fail to
reacquire in 40 s just as easily as a cold one, so that outcome discriminates
nothing. Receivers in this class can also retain almanac and ephemeris in their
own flash, which no rail outage clears at all.

**What actually is established by run 2:** the `ON`/`OFF` path really does
control the module. The re-enable came back `q=0` with `inview` dropped from 20
to 12 and every counter reset -- a module that had stayed powered and fixed would
have answered `q=1` on its first GGA after the port reopened.

#### The experiment was run, 2026-08-31, and it killed my own explanation

USB unplugged for 7.7 s with **no battery attached** (maintainer confirmed), so
the board genuinely lost power. Then `CMD:GNSS PROBE`, which reads the
expander's registers and opens the UART with **no power hook at all**, before
anything can write the rail.

```
GNSS_PROBE:cfg0=0x00 out0=0xFF in0=0xFF io00_dir=output io00_level=high
           bytes=1767 sent=36 cserr=1 ferr=0
```

**Two results, and they point in different directions.**

**Solid: the receiver is powered without this firmware asking.** 1,767 bytes and
36 checksum-valid sentences in 2.5 s, with `powerEnable` set to `nullptr` so no
code path could have touched the rail. That half of the original finding stands.

**Killed: the pull-up explanation, which was mine.** This file previously said
"the expander comes out of reset with every pin an input and something on the
board pulls the enable line up". `io00_dir=output` says the opposite. The pin is
being **driven**, not pulled. A pull-up hypothesis predicts an input, and that
prediction failed.

**And `cfg0=0x00` is not explained by anything in this firmware.** That is all
eight pins of port 0 configured as outputs. `gnssPowerEnable()` sets one bit by
read-modify-write (`BoardT5S3::setPca9535PinMode`), which would leave 0xFE, and
the EPD code writes only port 1 -- `PCA9535_IO10..IO17` are linear indices 8 to
15, so `updatePca9535Bit` addresses register 0x07, never 0x06. Nothing here
writes eight bits of port 0.

Three candidates, and this probe cannot separate them:

1. **The expander did not actually lose power** in those 7.6 s, despite no
   battery. Then 0x00 is a latch from the factory firmware, which does configure
   the whole expander, and it has survived every reset since.
2. **This part's power-on default is not all-inputs.** An NXP PCA9535 resets its
   configuration registers to 0xFF; a second-source part or a clone may not.
   Datasheet question, no hardware needed.
3. **The read is misaddressed** and these are not the registers I think.

**The cheap check that excludes 3, and it has not been run:** read `CONFIG1`
(0x07) in the same probe. This firmware *does* configure port 1, for the EPD
pins, so `CONFIG1` should show exactly `IO10`, `IO11`, `IO13`, `IO14`, `IO15` as
outputs and `IO16`, `IO17` as inputs. If it does, the addressing is right and the
port-0 reading has to be believed. One line of code.

So the honest state: **the receiver is powered before any firmware asks, by a
driven expander output that this firmware did not write.** Why that output
survives a power cycle is open, and the mechanism matters for T-579 -- a latch
that any reset preserves is a very different power story from a board that holds
the rail by design.

#### The first honest acquisition figure: 41,751 ms

The same run produced what every earlier run could not. After the power cycle the
receiver had genuinely lost its fix, and `CMD:GNSS ON` was followed by two
statuses reporting `q=0` at 14.3 s and 22.7 s of uptime, then a fix at
**`ttff=41751`**.

Same code, same receiver, same room as the 531 ms and 526 ms readings. When
acquisition actually happened the number was **eighty times larger**. That is
independent, physical confirmation of what the review argued from the code: a
sub-second `ttff` was never an acquisition, only the phase between the UART open
and the next sentence. The warning now on `timeToFirstFixMs()` is not a
precaution, it is a measured fact.

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

**`cserr` is the baud-rate check and `ferr` is not.** `cserr` counts sentences
whose checksum did not match: at zero with `sent` climbing, the UART is clean;
climbing together with `sent`, the baud rate is wrong -- the framing is close
enough to produce lines but not correct ones. `ferr` counts input that had no
usable `*hh` at all, which is a garbage burst on a cold UART or a line lost to
buffer overflow. They were one counter until review pointed out that mixing them
makes the first useless: opening the UART on an already-talking receiver logs a
`ferr`, and reading that as a checksum error is how a clean line looks dirty.

`bytes` climbing with both counters at zero and `sent` flat means something is
talking and it is not NMEA.

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

### Checked against the wire, for one fix

`CMD:GNSS RAW ON` and a status query one second apart, so the same solution can
be read raw and parsed. The degree-and-minute split, the N and E hemisphere
handling, HDOP, satellites used and altitude all matched.

**Weaker than the first version of this section claimed**, and the review was
right to say so:

- The status line's timestamp corresponds to a sentence that was **never
  logged** -- `RAW OFF` cut the stream one second earlier. The "+1 s" was
  inferred from the 1 Hz cadence, which is reasonable and is not a comparison
  against the wire.
- The coordinate agreement to 2e-6 degrees was attributed entirely to one second
  of drift. That is circular: it also hides any error below roughly 0.2 m. What
  the check really proves is the gross `ddmm.mmmmm` split and the sign for north
  and east.
- **Never exercised:** the S and W negation, a date rollover, a leap second, the
  two-digit year window, and `quality=6` (dead reckoning) being committed as a
  fix like any other.

So: verified for **one fix, one hemisphere pair, one date**. Not "field by
field" in the sense that phrase implies.

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

### A blocking render starves the UART, and by more than first written

Real, and the first version of this section under-reported it by a factor of
three because it used a guessed byte rate and the wrong window. Here it is from
the log arithmetic.

The clean baseline, between two statuses with no screen work in between (millis
107,341 to 119,809): **816 bytes/s and 14.9 sentences/s**. The earlier text said
"roughly 600 bytes a second", which its own evidence contradicts by 35 %.

Across the map entry (119,809 to 146,960, 27.15 s): expected about 22,160 bytes
and 405 sentences at that baseline; observed 17,384 bytes and 320 sentences.
**Lost: about 4,780 bytes and 85 sentences.**

The blocking window is **6.07 s**, not the 4,017 ms render: the log's
`New max loop duration: 6074 ms` covers the whole map `onEnter`, which is
`BlePositionServer::begin()` plus the render. And that closes the arithmetic --
6.074 s x 816 B/s minus the 256 bytes the buffer does hold is 4,701 bytes, against
4,780 observed.

The 256 bytes is **read off the pinned framework, not measured**:
`framework-arduinoespressif32` `HardwareSerial.cpp:148` sets `_rxBufferSize(256)`
and nothing in this firmware calls `setRxBufferSize()` on `Serial1`.

One extra checksum error across that window is genuinely measured (1 to 2).

Harmless for a bring-up console, not harmless for a position source. The fix is
not "poll more often", because there is nowhere to poll from during a blocking
render: it is a bigger RX buffer, the UART on its own task, or an event-driven
read. That decision belongs to the work that puts GNSS behind the map's position
source. The header now states the constraint so the next caller meets it before
being surprised by it.

### The comprehension test, and what it changed

Two more reviewers were given the code and **forbidden from reading this file**,
then asked to explain how it works. The point was not defects: it was whether
the code explains itself to a stranger, which is what the upstream PR will
depend on. One capable reader and one weaker one, same ten questions, so the
difference between them says which explanations are load-bearing and which need
the reader to be sharp.

Nine of the ten answers came back right and sourced to the code. The tenth is
the finding.

**Both failed on `ttff`, and the weaker one failed with high confidence.** Asked
what `ttff=530` permits you to conclude, one answered "a very fast warm or hot
start, the receiver retained ephemeris"; the other answered "the receiver's
**cold-start** acquisition took 530 ms" and rated itself high-confidence,
code-based. A 530 ms cold start is physically impossible. Neither spotted that on
an already-tracking receiver the number is the phase between our UART open and
the next sentence.

That is the same mistake this file's own rail section had to withdraw, made
twice more by readers who had the code in front of them and no way to know
better. So the warning now lives on `timeToFirstFixMs()` in the header, not only
here. **A conclusion the code invites is the code's problem.**

**Both also converged on one structural criticism**, from opposite directions:
the strongest claims in the library are asserted in comments with no way to check
them from inside the code. The measured 816 B/s and the 0.3 s budget, the GPIO46
behaviour, the `sizeof(long)` verification -- all of them true, none of them
observable at runtime. One reviewer's single requested change was to make
`poll()`'s contract an observable rather than a warning.

Acted on:

- **`rxNearlyFullEvents()`**, reported as `rxfull` in the serial reply. Sentence
  loss was previously invisible: whole sentences vanish inside the driver, and
  the 2026-08-31 measurement proves it -- 85 lost, `cserr` moved by exactly one,
  so **84 disappeared with no counter moving at all**. A non-zero `rxfull` now
  says every other count in the line is an undercount.
- **`GnssConfig::rxBufferBytes`**, default 1024, applied before the UART opens.
  Four times the Arduino default, about 1.2 s of caller inattention. It does not
  survive a multi-second block and the comment says so.
- **The 2038 comment now explains that the cast removes the width dependency**
  rather than citing a verification the reader cannot repeat.
- **"GGA leads RMC by nine sentences" is scoped to this receiver**, with the
  point made explicit that the cure does not depend on the ordering at all -- a
  reader was right to flag it as reading like an NMEA law.
- **The reason GGA's time is unused moved to the top of `parseGga()`**, as a
  regression guard, so nobody "fixes" it by adding the field back.
- Three smaller things a reader named as slowing them down: the empty-field
  contract of `nmeaField()`, why `talkerFor()` drops rather than evicts, and why
  `parseGsv()` gets the same buffer twice.

### The fixes, verified on hardware 2026-08-31

Same run as the probe above, on the build carrying all of them.

- **`rxfull` catches the blocked render.** Three statuses across the session:
  `rxfull=0`, `rxfull=0`, then **`rxfull=1`** immediately after `CMD:GOTO_MAP`.
  Silent while nothing blocked, and it fired on the one thing it was built to
  see. The loss it reports used to be entirely invisible.
- **The `cserr` / `ferr` split behaves as designed.** The mid-stream UART open now
  lands in `ferr` (`cserr=0 ferr=1` on the first status), where it used to inflate
  the checksum counter and make a clean line look dirty.
- **`utc` from RMC and ZDA atomically.** The reported value matched the ZDA
  sentence on the wire to within one sentence interval, which is the resolution
  this check has -- not "exact", and the earlier version of this file made that
  mistake once already. The run also exercised the intended path for an RMC with
  status `V`: `$GNRMC,170236.000,V,,,,,,,310826,,,N,V` has no position and its
  date was still used, while speed and course were correctly ignored.
- **Not exercised:** the midnight rollover itself, the 2038 arithmetic, the
  dropped-GSV-message path, and the rail rollback. All four are why T-580 wants
  host tests rather than another night on the bench.

### Four defects review found in it, all fixed

None of these showed up in the hardware pass, which is the point of reading code
against a spec rather than only running it.

- **`fix.utc` regressed 24 hours at every UTC midnight.** GGA carried the time
  and RMC the date, stitched together -- and in this receiver's cycle GGA leads
  RMC by nine sentences, so a 00:00:0x GGA recomputed the clock against
  yesterday's date. Every day, guaranteed, for about 0.4 s at 816 B/s, and
  longer whenever the loop is blocked. Fixed by taking date and time only from
  sentences that carry both: RMC, and `ZDA` where the receiver sends it. `ZDA`
  is the better source anyway -- it has a four-digit year, so the two-digit
  century guess never runs. The GGA time path is gone.
- **Signed 32-bit overflow on 2038-01-19.** `days * 86400L` in `toUnixSeconds`,
  where `long` is 32 bits on this target -- verified by compiling a
  `_Static_assert` with the pinned `xtensa-esp32s3-elf` toolchain rather than
  assuming. The `days_from_civil` formula itself is correct; only the arithmetic
  width was wrong, in a function returning `uint32_t`. Cast before the multiply.
- **GSV counters double-counted after a dropped sentence.** The per-cycle
  accumulator was cleared only when message 1 arrived, and never after a commit.
  Lose message 1 to a checksum error and the survivors pile onto the previous
  sweep's residue, so `satsWithSignal()` could report up to double. Now the
  accumulator clears on commit and a cycle with a gap is skipped rather than
  committed.
- **A failed rail enable could leave the rail on.** `gnssPowerEnable()` is two
  I2C writes and the first one is the one that powers the pair; if the second
  failed, the function returned false with the receiver and the SX1262 live.
  Both it and `begin()` now roll back.

Two contract statements were also wrong rather than the code: `poll()` promised
to report any change to the fix but only reported position, and the header said
the library never blocks while `begin()` delays for the power settle. Both
corrected, and `cserr` no longer counts framing errors -- those have their own
counter, `ferr`, because mixing them makes the baud-rate diagnosis useless.

Cost, measured off the two builds: flash went from 58.3 % to 58.5 % of the
6.4 MB app partition, about 13 kB for the library plus the command. Zero
compiler warnings.

## What the hardware pass found, in the order it was run

Run 2026-08-31 on a LilyGo T5 S3 Pro, indoors, three serial sessions. Read the
caveats: two of the six checks did not test what they were written to test.

1. **`CMD:GNSS ON` answers `GNSS_OK:on`.** Yes. The expander responds, the rail
   write succeeds, and `OFF` plus a re-enable work -- the re-enable came back
   `q=0` with every counter reset, which is what proves the rail controls the
   module rather than the command merely returning success.
2. **The SD card still reads with the rail up.** It read, and **the check could
   not have failed.** `CMD:GOTO_MAP` read 1,393,825 bytes: `4 tiles ok, 0
   missing`, `3194 ms in the card`, frame drawn. But the log's own ordering
   shows card access and panel bus never overlapped -- `renderViewport start` at
   122,550, `3194 ms in the card` finishing at 126,566, `displayBuffer` at
   126,568. The hazard is a panel refresh toggling GPIO46 **while an SD
   transaction is in flight**, and that window never opened. n=1 of the wrong
   scenario. **The contention question is untested**, and so is the reset's
   contribution to it.
3. **`bytes` climbs.** Yes, monotonically: 11,644 to 21,820 to 39,204 in one
   session. Pin direction as written is correct, no swap needed.
4. **`cserr` stays at 0 while `sent` climbs.** Yes on a rail-cycled start. The
   two errors in run 1 are both explained and neither is the wiring -- one was
   the UART opening mid-sentence, which the code now counts as `ferr`, and one
   was the buffer overrun during the blocked map entry.
5. **Outdoors: tracked and bestsnr rise, then a fix.** A **3D fix indoors** made
   the outdoor half unnecessary for proving the receiver works. **Cold-start
   TTFF is still not measured**, and the number this pass produced does not
   measure it -- see the rail section on why a sub-second TTFF only says
   "already tracking".
6. **The idle cost of the pair.** **Not measured.** Open, and its framing
   depends on the CONFIG0 experiment above: whether it is the board's floor or a
   feature's price is exactly what has not been established.

**What a real test of check 2 looks like**, when someone runs it: a task doing
continuous SD reads with CRC verification while full and partial refreshes loop,
rail up, run twice -- `LORA_RST` floating, then held low. Anything less repeats
the non-overlapping path.

## Open

- **Is the rail on by board default, or was it latched by an earlier session?**
  The CONFIG0 read on a true power cycle, above. Everything about this board's
  power floor hangs off the answer, and it costs one boot.
- **Idle current of the GNSS + LoRa pair**, against the BQ27220 or a meter in
  series at the development board's battery connector. Name the instrument.
  T-579.
- **Cold-start TTFF**, outdoors, from a genuinely cold receiver -- longer than a
  49.7 s outage, or a receiver command that clears the almanac.
- **Is the SPI contention real at all?** Does GPIO46 go low during a refresh,
  and does an SX126x in reset park MISO high-Z? Datasheet plus the overlapping
  test above.
- **The RX buffer under a blocking render** -- bigger buffer, own task, or
  event-driven, decided by the work that feeds position to the map.
- **The unexercised parser paths**: southern and western hemispheres, a date
  rollover, a leap second, the two-digit year window, `quality=6`. Cheap to
  cover with host tests against recorded sentences, and worth doing before the
  upstream PR.
- Whether the receiver can stay powered during a ride or must be duty-cycled. A
  power question, and given the shared rail a LoRa question too.
