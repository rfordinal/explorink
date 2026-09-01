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
| Feeding the map | **built, not yet run on hardware** -- behind `mapGnssPosition`, off by default. See "The map reads it" |
| Verified on hardware | 3D fix indoors; parser correct for one N/E fix on one date; the rail's ON/OFF path works |
| Still open | idle current, whether the rail is on by design or by an uncleared latch, whether SPI contention is real, a TTFF from the receiver's own power-on |
| What comes next | [`gnss-to-map-plan.md`](gnss-to-map-plan.md) -- five steps to the map reading this receiver, and the merge gate |

**A working fix indoors**, on a desk, through a ceiling: `q=1`, 8 satellites
used, 19 in view, 9 tracked, best C/N0 32 dB-Hz, HDOP 2.0. That was not the
expected outcome -- an indoor attempt on the factory firmware got nothing
(2026-08-31, earlier the same day) -- so the antenna and its placement are
better than assumed.

The receiver also emits `$GPTXT,01,01,01,ANTENNA OK`. **That is an open/short
detection line, not an endorsement of placement**, and the first draft used it as
one. What it actually asserts should be read out of the L76K or CASIC manual
before it is quoted again; it is currently uncited.

**Time arrives before a fix.** `utc` is populated while `q=0`, because the date
and time come off RMC well before a solution does. Verified in the second run,
and it matters: the safety budget in the parent repo's `safety-concept.md`
wants a wall clock, and this supplies one without a fix and without a phone.

Position reaching the map is a separate piece of work, written up under "The map
reads it" below. It turned out **not** to need the abstraction over a position
source the parent repo's `docs/lora.md` argues for under "What can be built
today": `MapActivity::applyFix()` already is that seam, and the receiver became
its third caller.

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
text on each boot. Presumed harmless, on the grounds that a receiver discards anything that is not
a command it knows -- **inferred, never observed**, and it is 115200-baud output
arriving at a 9600-baud input, so the receiver sees framing errors rather than
text.

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

- The UART opened **mid-sentence**: exactly one error of that class in the
  session, while a rail-cycled start later the same session logged none. (The
  counter was first *sampled* 14 s and 215 sentences after the open, so
  "mid-open caused it" is the best explanation rather than a timed observation.) A
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
S3. The board ran its **factory firmware** earlier the same day, and that firmware
presumably drives this rail -- **assumed, not established**; nobody has read its
expander writes. If the board was not physically unplugged in between,
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
to 12 -- a module that had stayed powered and fixed would have answered `q=1` on
its first GGA after the port reopened.

The first version of this paragraph also cited "every counter reset" as evidence.
That was vacuous: `begin()` zeroes the counters in software on any successful
call, powered module or not. Same class of check as the SD-card test below, and
deleted for the same reason.

#### The experiment was run, 2026-08-31, and it killed my own explanation

**The board did not lose power, and this section said it did.** Corrected
2026-09-01: the maintainer had said no battery was attached, this section rested
its whole validity on that, and it was a slip -- **the battery is connected.**

**Label that claim `assumed`, not measured.** It comes from conversation on
2026-09-01, and the opposite came from conversation the day before -- the same
class of evidence that voided the numbers below. Nothing on the device has
confirmed it. **Two free ways to settle it, neither needing a code change:**
`CMD:SCREENSHOT` on Home shows the battery percentage the theme draws
(`src/components/themes/roundedraff/RoundedRaffTheme.cpp:70-74`), and step 2a of
[`gnss-to-map-plan.md`](gnss-to-map-plan.md) settles it as a by-product -- a
`reset=POWERON` that appears only once the cell is unplugged *is* the proof the
cell was holding the board.

So
removing USB leaves the board running on the cell and the 3.3 V rail up. Every
"power cycle" in this file's evidence removed nothing.

The USB device node still vanished, which is what the test scripts watched, and
that is only the USB peripheral losing its host. **A vanished node is not a power
cut** -- written here already for deep sleep, and true for this reason too.

Two things follow immediately: `cfg0=0x00` surviving is expected rather than
puzzling, and **no test in this file has ever power-cycled the expander.** The
outage figures below (21.0 s, later 345 s and 381 s) are node-absence, not
outages, and the wording keeps them only because the logs do.

**The outage figure was also read off the wrong line** -- the first version said
7.7 s, which was the script's wait for the maintainer to pull the plug, not the
gap; the node was absent 21.0 s. Then `CMD:GNSS PROBE`, which reads the
expander's registers and opens the UART with **no power hook at all**, before
anything can write the rail.

```
GNSS_PROBE:cfg0=0x00 out0=0xFF in0=0xFF io00_dir=output io00_level=high
           bytes=1767 sent=36 cserr=1 ferr=0
```

**Two results, and they point in different directions.**

**Solid, but narrower than it reads -- the mechanism is a stale latch, not board
design; see "Settled 2026-09-01" below. The receiver is powered without this
firmware asking.** 1,767 bytes and
36 checksum-valid sentences in 2.5 s, with `powerEnable` set to `nullptr` so no
code path could have touched the rail. That half of the original finding stands.

**With one caveat the first draft hid.** These registers were read **two board
resets and 5.5 minutes after** the power-on they are being interpreted against.
The bridge is a code argument -- nothing in this firmware writes port 0, verified
by grep and by the port arithmetic in `updatePca9535Bit` -- which is read off the
code, not measured. And `CMD:GNSS PROBE`'s own comment demands a `POWERON` reset
cause before the answer means anything: **no log in this session contains one.**
The power-cycle boot's capture started mid-line at millis 944, past the ROM
banner, and the boot that actually ran the probe was an esptool reset. The rerun
is cheap now that the dropped-command failure mode is understood: open the port
*before* the replug.

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

The part's own datasheets settle what a reset would have produced, and they are
unanimous across four vendors plus the common clone:

| fact | value | source |
|---|---|---|
| Configuration register reset default | **0xFF**, all pins inputs | TI SCPS129K Tables 8-3, 8-7; NXP Rev. 6 Tables 11, 12 |
| Configuration bit polarity | 1 = input, 0 = output | TI SCPS129K 8.3.2; NXP Rev. 6 6.2.5 |
| Output register reset default | 0xFF | TI Table 8-5; NXP Tables 7, 8 |
| Register map | 0x00/01 input, 0x02/03 output, 0x04/05 polarity, 0x06/07 config | TI Table 8-3; NXP Table 4 |
| Any way for a register to survive losing VCC | **none** -- no backup pin, no non-volatile storage | both, by omission and by 8.2 |

Checked against TI SCPS129K (rev. March 2021), NXP PCA9535/PCA9535C Rev. 6,
NXP PCA9535A Rev. 1.1, Nexperia Rev. 1.1 and the Xinluda XL9535 clone, which
agrees on all of it.

**So a genuine reset cannot produce 0x00 here.** But the reset condition is about
volts, not about the cable: it trips when VCC at the chip falls below VPORF,
0.77 to 1.14 V (TI Table 10-1), and both vendors say a *guaranteed* power-reset
cycle needs VCC below 0.2 V. Standby draw is 1 uA max. So an unloaded rail held
up by bulk decoupling decays slowly, and **whether 21 s at the connector took
this board's 3.3 V rail below 0.8 V at the expander is a board question no
datasheet answers.** The ESP32 clearly reset, so the rail fell below its brownout
of roughly 2.5 V; the remaining 1.7 V is the whole question.

Three candidates, and this probe cannot separate them:

1. **The expander never lost power.** No longer one candidate among three --
   **this is the reading, as of 2026-09-01, and it is now proven** -- by
   `CMD:GNSS RELEASE` rather than by any power cycle, see "Settled 2026-09-01"
   below. `cfg0=0x00` is a latch that has survived every reset since something
   wrote it, plausibly the factory firmware, which does configure the whole
   expander.

   **The route this paragraph used to argue is withdrawn, and it was resting on an
   unconfirmed claim.** It said the battery is connected, so removing USB never
   dropped the rail and the datasheet's VPORF threshold (0.77 to 1.1 V, TI
   SCPS129K Table 10-1) never came into it. Whether a cell is fitted at all is
   **still unconfirmed** -- `batt_mv=4102` is a node voltage the BQ25896 holds near
   float with or without a cell (T-583). The conclusion no longer needs it: the
   release test does not care what powers the board.
2. **This part's power-on default is not all-inputs.** An NXP PCA9535 resets its
   configuration registers to 0xFF; a second-source part or a clone may not.
   Datasheet question, no hardware needed.
3. ~~**The read is misaddressed**~~ -- **excluded 2026-08-31.** `CONFIG1` reads
   **0xC4**, exactly the value predicted from `prepareEpdPower()`'s writes, so
   register 0x06 is being addressed correctly and `cfg0=0x00` has to be believed.
   Review
   added a specific mechanism for this that I had not considered: the PCA9535
   holds a command-byte pointer until it is rewritten, and a read with the
   pointer at 0x00 returns Input Port 0, whose value is the external pin levels
   -- all-low inputs read exactly 0x00. Argued against by the data, not
   excluded by it: the three reads returned 0x00, 0xFF and 0xFF, so at least
   0x06 is being addressed differently from 0x02 and 0x00, which a stuck pointer
   would not do.

**What the datasheet makes most likely is candidate 1**, and the datasheet was
right.

#### Settled 2026-09-01: candidate 1, and no power cycle was needed

**`CMD:GNSS RELEASE` answered it on the bench, twice.** The power-cycle route was
dropped before it was ever run: it needed unplugging connector `P2`, whose part
number is `[open]` and which nobody here has identified on the board, and it was
only ever a detour. The direct question is not *what does the expander come up
as* -- that is a proxy -- but **does anything other than the expander hold
`LORA_GPS_EN` high**. Stop the expander driving the pin and ask the receiver.

| window | run 1 | run 2 | what it shows |
|---|---|---|---|
| `cfg0_base` | `0x00` | `0x00` | bit 0 clear: the pin was already an **output** before anything touched it |
| baseline, 3 s | 1722 B / 42 sentences | 1606 B / 38 | the receiver really was streaming first |
| `cfg0_released` | `0x01` | `0x01` | the I2C write took -- bit 0 set, pin now an input |
| released, 5 s | 308 B / 6 | 467 B / 10 | **NMEA stops** |
| `cfg0_restored` | `0x00` | `0x00` | back to output |
| restored, 4 s | 1737 B / 43 | 1743 B / 43 | the receiver returns; the test left the board as it found it |

Measured on the LilyGo T5 S3 Pro, `env:t5s3pro`, firmware built from
`feat/t5s3-gnss` at the merge of `release/lilygo-t5-s3-pro`. The instrument is
the receiver itself -- `Gnss::bytesRead()` and `Gnss::sentencesParsed()` deltas
across three windows -- and no external meter is involved.

**The released window is not zero, and the most likely reason is a rail coasting
down** -- 308 B against that run's own 574 B/s baseline is 0.54 s, 467 B against
535 B/s is 0.87 s. That reading is **inferred, not measured**: the alternative is
the driver's RX ring handing back bytes that arrived before the release. It is
disfavoured because `sample()` polls in a tight loop, so the ring is near empty,
but nothing here measured the ring depth at the moment of release. The window is
5 s and not 3 for the same reason.

**Both baselines run about 30 % under this file's 810 to 816 B/s**, at a matching
sentence rate (14 and 12.7/s against 14.9). That is 41 to 42 bytes per sentence
against the established 55, which is what a NOFIX sentence mix on an indoor desk
looks like, and it is **unconfirmed**. It does not touch the conclusion: the
arithmetic above uses each run's own baseline, not the file's headline number.

**So the rail was held by the expander's own latched output.** Nothing else on
the board holds it. `cfg0=0x00` is a latch that no reset has ever cleared,
because the expander has never lost power -- which is also why every "power
cycle" in this file's earlier evidence could not have cleared it, battery or no
battery.

**One earlier claim needs its scope narrowed rather than withdrawn.** "The
receiver is powered without this firmware asking" stands as written: in the boot
that ran the probe, no code here wrote the rail. What it never established, and
what the first draft implied, is *by design* -- the mechanism is a stale latch,
most plausibly left by the factory firmware, which does configure the whole
expander.

**Two consequences for step 3.** The receiver cannot be assumed powered on a
board whose expander has genuinely been cold-started, so `gnssPowerEnable()`
stays the path that turns it on rather than an optional courtesy. And a rail this
firmware did not ask for is a rail nobody is accounting for in the power budget,
which is step 2b's problem.

**Still open here:** `reset=UNKNOWN` came back from this boot, which began with
an esptool hard reset. That is **one** sample, not two -- both `RELEASE` runs sat
on the same boot, because nothing between them reset the board
(`mapcmd.open_port` never toggles DTR/RTS). Reset cause is a per-boot fact, so
two runs of the command are one observation of it. The field is worth keeping in
the reply, but on this board it has not yet named a single reset cause, so
nothing should be concluded from the absence of `POWERON`.

#### The detection method was wrong, and it cost three attempts

**A vanished USB device node does not mean the board lost power.** It means the
USB peripheral went down, and on this board the ordinary cause is the firmware
falling asleep. Three probe attempts on 2026-08-31 waited for the node to
disappear and treated that as an unplug. One of them was demonstrably **deep
sleep**: the board itself said so.

**Retracted the same day: `millis` continuity was not sound evidence for the
other two.** The first draft here argued that `millis` continued straight across
the gap, which no power loss can do. But opening the port can deliver
kernel-buffered bytes from before the unplug, so the first lines' `millis` need
not belong to the current boot -- a proxy used without checking the proxy, which
is the same mistake one level up. What stands is `reset=DEEPSLEEP` for the one
boot that reported it, read from the chip. Whether the two earlier attempts were
sleep or real power loss is **unknown**, and the probe line is now the only way
to tell.

`reset=DEEPSLEEP` in the probe reply is what finally settled it. Which is the
lesson: **the device is the only authority on whether it lost power**, and the
probe now self-certifies -- the register values arrive in the same line as the
reset cause that qualifies them. No line saying `reset=POWERON`, no conclusion.
This is why that field was added, and it earned itself on the first run.

**Before any of that, rule out the boring cause.** A `CMD:GNSS` that answers
nothing may not be compiled in. On 2026-09-01 two probe attempts were spent on
the RX-starve hypothesis when the board was simply running a build without
`-DENABLE_GNSS_CMD` -- that flag lives in `env:t5s3pro` and in no release env, so
the whole handler is absent and an unknown command falls through in silence. The
tell is cheap and decisive: **another `CMD:` still answers.** `CMD:SCREENSHOT`
and the map console both replied in the same session, so the handler loop was
alive and only the GNSS branch was missing. Check that first, because the
RX-starve trap makes the wrong diagnosis the attractive one and it costs a flash
to find out otherwise.

Two consequences for any repeat, both free:

- **Keep the board awake.** Real input resets its sleep deadline, so a harmless
  read-only command every 15 s during a long wait stops it dropping off. The
  three failed attempts each sat silent for minutes and were put to sleep by
  their own patience.
- **Wait in one long window, not four short ones.** The firmware's stuck-byte
  drain only fires after 5 s of the same byte, so a reader that gives up after
  12 s can never see the recovery it triggered.

  **And whether that drain clears the wedge is unverified.** In the session where
  commands finally arrived it never fired -- zero `serial head byte` lines -- so
  they arrived because the board was awake and its buffer clean, not because
  anything was cleared. What is verified is that the **diagnostic** names the
  byte, which is what turned this from a coin flip into a mechanism. What would
  settle the fix: reproduce the wedge and watch a `CMD:` succeed within seconds
  of the drain line appearing.

And it explains something the bring-up doc had filed as an open USB fault: at
least today's dropouts were auto-sleep, not a bus problem.

**The cheap check that excludes 3, and it has not been run:** read `CONFIG1`
(0x07) in the same probe. This firmware *does* configure port 1, for the EPD
pins, so the expected value is computable rather than a hand-wave.
`prepareEpdPower()` sets `IO10`, `IO11`, `IO13`, `IO14`, `IO15` as outputs and
`IO16`, `IO17` as inputs, and **it never touches `IO12`** (the button, linear
index 10) -- `BoardT5S3::begin()` would set that one as an input and never runs.
So under the all-inputs default `CONFIG1` should read **0xC4**.

The first version of this prediction enumerated the five outputs and said
"exactly", omitting `IO12` -- so a correct 0xC4 would have read as a failure to
whoever ran it. Caught by review before anybody did.

So the honest state: **the receiver is powered before any firmware asks, by a
driven expander output that this firmware did not write.** Why that output
survives a power cycle is open, and the mechanism matters for T-579 -- a latch
that any reset preserves is a very different power story from a board that holds
the rail by design.

#### An acquisition really happened. The 41,751 ms figure still is not it

**Corrected before merge, by the author, and it is the same mistake twice.**

The run reported `ttff=41751` after the power cycle, with two statuses at `q=0`
first (14.3 s and 22.7 s of `Gnss` uptime). This file's first draft called that
"the first honest acquisition figure". It is not one.

The receiver never lost power between the USB replug and that command. The board
was reset twice in between -- once by a port open, once by esptool -- but a board
reset does not touch the rail, so the receiver had been powered and searching the
whole time. From the log timestamps and the board's own `millis`:

```
USB replug, receiver powered:  18:56:44
CMD:GNSS ON:                   19:02:17   -> 333 s (5.5 min) already powered
first fix:                     19:02:59   -> 41.8 s after begin()
```

So the acquisition took about **6.2 minutes** from power-on, and `ttff=41751` is
its **last 11 %**. `timeToFirstFixMs()` measures from `begin()`, and `begin()`
arrived five and a half minutes late. Wrong interval attributed to the number --
exactly what the 531 ms readings did, in the other direction.

**What does survive, and it matters:**

- At `begin()` the receiver was **not** holding a fix, proven by two `q=0`
  statuses. So this reading reflects an acquisition in progress rather than a
  phase offset, which is a qualitative difference from every earlier run and does
  corroborate the review's argument about what a sub-second value means.
- ~~**The reacquisition took roughly 6.2 minutes indoors**~~ -- **void as of
  2026-09-01.** It was measured from the USB replug on the belief that the
  receiver had lost power there. With the battery connected it never did, so the
  interval spans "the receiver was already running" to "a fix", and that measures
  nothing. **A fifth number attributed to the wrong interval**, and the first one
  where the wrong endpoint came from a fact about the hardware rather than a
  misread log. There is still **no acquisition figure** for this receiver.

  Two earlier attempts to salvage this number both failed, and the sequence is
  the lesson. First it was published as an acquisition figure; review pointed out
  the counter started 5.5 minutes late. Then it was relabelled a warm
  reacquisition after a 21 s power interruption. Now there was no power
  interruption at all. **A number that needs relabelling twice is a number whose
  endpoints were never established** -- the honest move was to drop it at the
  first correction, not to find a weaker claim it could still support.

The lesson is not about GNSS. `timeToFirstFixMs()` now carries a warning that a
small value means nothing, and the author then read a large value as if it meant
something. **A counter that starts when you happen to start looking measures your
attention, not the world.** A real TTFF needs the measurement to begin when the
receiver does, which means either the firmware powers it, or the number comes
from somewhere other than this counter.

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
CMD:GNSS PROBE     ->  GNSS_PROBE:...      reads the expander, writes no rail
CMD:GNSS RELEASE   ->  GNSS_RELEASE:...    stops the expander driving the rail pin,
                                           then puts it back (step 2a)
```

**`RELEASE` is the one that writes**, which is why it is not folded into `PROBE`.
It drops the receiver's power for five seconds by design, and a caller who
expected a read would lose the fix underneath them. It restores the pin before it
replies, and the reply carries the `CONFIG0` readback at each stage so a restore
that silently failed cannot be mistaken for a clean run.

A status reply is one line of `key=value`, so a host script can grep it:

```
GNSS_FIX:q=1 used=9 inview=14 tracked=11 bestsnr=38 lat=12.345678 lon=98.765432
  alt=191.0 hdop=1.20 speed=0.0 course=0.0 utc=1788178462 ttff=42313 age=612
  sent=1204 cserr=0 bytes=98304
GNSS_NOFIX:q=0 inview=11 tracked=4 bestsnr=19 utc=1788178462 uptime=63120
  sent=214 cserr=0 bytes=17408
```

(Wrapped here for reading. On the wire it is one line. **The coordinates above
are invented**, not a recorded fix -- this repo is public and a real one would be
the maintainer's address. See Privacy, below.)

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

## The map reads it

**Written 2026-09-01. NOT yet run on hardware** -- every claim below is read off
the code, and the one that matters (a dot on the panel from this receiver, with
no phone) is exactly the one a device pass has to settle. Step 3 of
[`gnss-to-map-plan.md`](gnss-to-map-plan.md).

### It is three callers of one function, not an abstraction

`MapActivity::applyFix()` is already transport-agnostic: the follow decision, the
persisted-fix banner, the marker move and the debounced save all sit on its far
side, so a new position source adds a reader and nothing else.

| caller | source | `MapActivity.cpp` |
|---|---|---|
| 1 | the BLE packet from the phone | the `getLatest(update)` block in `loop()` |
| 2 | the command console, `pos ...` | the `consoleState_.hasPosition()` block in `loop()` |
| 3 | **the on-device receiver** | `pollGnssFix()`, called from `loop()` |

No interface, no base class, no per-source state machine. One function, three
callers of it.

### What was added

- `src/GnssAccess.h` -- the whole seam: `extern Gnss gnss` and `gnssStart()`.
  Entirely behind `ENABLE_GNSS_CMD`, so on any other env the header is empty and
  `MapActivity` compiles as it did before.
- `gnssStart()` in `main.cpp`, lifted out of the `CMD:GNSS ON` branch that used
  to hold it inline. Two callers must not carry two copies of the pin numbers,
  the baud and the ring size.
- `CrossPointSettings::mapGnssPosition`, `uint8_t`, **default 0**. In the
  settings file, deliberately **not** in `SettingsList`: a Settings row would
  offer every rider a toggle for hardware one development board has.
- `mapGnssPosition` in the `CMD:SETTING` allow-list, itself behind
  `ENABLE_GNSS_CMD` -- a build with no receiver answers `SETTING_ERR:unknown`
  rather than `SETTING_OK` for a toggle that cannot do anything.
- `MapActivity::pollGnssFix()`, plus the rail's lifecycle in `onEnter()` /
  `onExit()`.

### Three decisions inside it, and the reason for each

**Heading was 0 and stayed 0, until a ride found it.** Mapping the course
straight through would be wrong rather than rough -- at rest it is noise
(`speed=1.3 course=211.9` on a stationary desk, measured 2026-08-31) -- so the
first cut passed 0 and left step 4 for later. On the first real ride,
2026-09-01, the marker followed the rider correctly and its arrow pointed north
the whole way. **A wrong heading and a missing one look identical on the
panel**, which is why "leave it for later" was the wrong call and not a
conservative one. `MapGnssHeading` now derives it -- see "Heading, and the gate
that decides whether to believe the course".

**A sample is identified by when the driver's fix last changed**, computed as
`millis() - Gnss::fixAgeMs()`. `Gnss::poll()` returns "something changed", but
`main.cpp`'s `loop()` is the one calling `poll()` and it consumes that answer, so
a second reader needs its own way to tell a new sample from the same one. The
change instant is constant between changes and moves on every one of them.

**`quality == 0` and `quality == 6` are skipped.** 0 is no fix. 6 is dead
reckoning with no satellites behind it (`Gnss.h`), and a map whose whole claim is
*where am I* must not draw a position nothing measured. `fix.valid` cannot do
this job: it latches true on the first solution and stays true, so it says "has
ever had a fix", not "has one now".

### Heading, and the gate that decides whether to believe the course

`src/activities/map/MapGnssHeading.{h,cpp}`, a pure module next to `MapFollow`
and split out for the same two reasons: the decision is worth testing on the
host, and a second client (iOS, the simulator, a replay tool) has to reproduce
it exactly. Eleven host tests in `test/map_gnss_heading/`.

| | |
|---|---|
| believe the course above | **3.0 km/h** |
| stop believing below | **2.0 km/h** |
| deadband past a step boundary | **6 degrees** |
| step width | 22.5 degrees, 16 steps (`MapHeading`) |

**Two thresholds, not one.** A rider sitting on a single threshold flips between
believed and not on every fix, and a flip that changes the step rotates the whole
frame -- a full e-ink redraw, about a second. What the hysteresis holds between
the two is the **gate**, not the step: a rider already believed keeps being
believed, so the course still drives the heading down to 2.0.

**3.0 rather than a motorbike-shaped 5.0, because of Hike mode.** Walking is
about 4 to 5 km/h, and a gate at 5 would leave a hiker permanently without a
heading. 3.0 still clears the 1.3 km/h noise floor. **A first cut, chosen on
those two numbers and not yet judged on the device** -- a per-mode gate is the
obvious refinement and is deliberately not built.

**Below the gate the last step is held, never decayed to north.** A held heading
is not a missing one, and north is a claim.

**The deadband is measured against the step on the panel, not the previous
course.** The question is whether the course has moved far enough to be worth
rotating the frame, and the frame shows a step. 6 degrees is about a quarter of
a step: enough to stop the flutter, small enough that a real turn still lands
within one fix.

Two wrap-around cases have tests because both are easy to get wrong: 350 degrees
is 10 degrees from north and not 350 (measured the long way it looks like a huge
turn and rotates the frame for nothing), and a receiver reporting 360.0 must not
land on step 16 in a four-bit field.

**Not verified on hardware.** Nothing here has been ridden with.

### The rail comes up with the map and goes down with it

`onEnter()` calls `gnssStart()` when `mapGnssPosition` is set; `onExit()` calls
`gnss.end()` **only if this activity is what started it**, so a `CMD:GNSS ON`
bring-up session from the host survives a trip through the map screen.

Not at boot, on purpose: that rail also powers the LoRa radio, and leaving it on
behind a screen that is not using a position is a silent drain. The price is that
leaving the map and coming back pays acquisition again -- tens of seconds from
cold, not the sub-second figure `Gnss::timeToFirstFixMs()` reports for a receiver
that was already tracking. Whether that trade is right is the duty-cycle question
in step 5 of the plan, and it needs step 2b's numbers first.

**This is also the first code path that powers the rail while the map is reading
tiles off the SD card**, which is the SPI contention that has never been tested
(`LORA_CS` is GPIO46, which LovyanGFX also drives as the panel bus's DC line --
see "The power rail is shared with the LoRa radio"). The defence, holding the
SX1262 in reset, is already in `gnssPowerEnable()`. A hardware pass of this step
exercises that combination for the first time.

### Both sources live: not decided here

With a phone connected and the receiver running, `pollGnssFix()` runs after the
BLE read in the same `loop()` iteration, so the later sample wins. That is the
smallest thing that is **not** a priority decision -- the real one is step 5, and
it needs the power numbers and a device-side heading first. The case step 3 is
built for has no phone connected at all.

One consequence worth knowing: `applyFix()` wants the phone's rolling packet
counter and the receiver has none, so `pollGnssFix()` passes a counter of its
own. The two can collide on one value in 256, which with both sources live costs
at most one skipped BLE packet -- the same 5 s the phone's next packet arrives
in.

### What a hardware pass has to check

Nothing below has been run.

1. **`CMD:SETTING mapGnssPosition 1` first.** The setting is 0 by default and 0
   means the old BLE path: a run that forgets this line tests the code that was
   already there and looks like a pass. `CMD:SETTING mapGnssPosition` with no
   value reads it back, so the run can prove it was set rather than assume it.
2. Enter the map **with no phone connected**. The waiting banner should clear
   and the dot should land where the device is.
3. The same build with `mapGnssPosition 0` still draws from the phone.
4. **The tiles get looked at, with the rail up.** See below -- this is the one
   check that fails silently.
5. `CMD:GNSS` after leaving the map reports `GNSS_OFF`, and a session started by
   `CMD:GNSS ON` before entering the map is still running after leaving it.

### The SPI check has to be a look, not a survival

The failure mode of the contention is a **corrupt tile read**, not a crash. So
"the device did not lock up" is a check that cannot fail: it also passes when
nothing was tested, when the rail was never up, and when the map drew no data at
all. Marking T-576 closed or deferred off a run like that leaves it untested and
makes it look like it passed -- the same shape of mistake as reading `rxfull=0`
as proof the UART survived when it also reads 0 when nothing was measured.

What settles it is somebody looking at the pixels:

- `CMD:SCREENSHOT` (or `CMD:SCREENSHOT_GRAY`) **with the rail up**, over ground
  that has real coverage -- `CMD:GOTO_MAP` prints `N tiles ok, M missing`, and a
  viewport of missing tiles proves nothing about tile reads.
- **More than one tile, and more than one frame.** Contention is intermittent by
  nature: it needs a panel refresh asserting `LORA_CS` to coincide with a card
  read, so a single screenshot that looks right is one sample.
- What to look for is **torn geometry where map belongs**: a road that stops
  mid-span, a coastline stepping sideways, an area fill bleeding past its
  outline, or hatch where a tile should have drawn. A tile that failed its CRC
  is drawn hatched rather than white on purpose (`MapHatch.h`: "absent,
  truncated or crc32-mismatched is drawn as hatch, never as white"), so hatch in
  the middle of covered ground is the loudest form this takes. A corruption that
  still passes crc32 shows up as the torn geometry above instead, which is why
  both are worth looking for.
- The same frames with `mapGnssPosition 0` and the rail down are the control.
  Without them a rendering artefact that was always there reads as contention.

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
`New max loop duration: 6074 ms` covers the whole map `onEnter`. Its composition
was also wrong in the first draft, which blamed `BlePositionServer::begin()` --
that costs **48 ms**. The real breakdown is the 4,017 ms render, about 1.5 s of
panel refresh after it, and roughly 0.5 s of setup. That matters for the fix:
the second-largest blocker is the **refresh**, not BLE, so anything that moves
the UART off this loop has to survive a refresh too. And that closes the arithmetic --
6.074 s x 816 B/s minus the 256 bytes the buffer does hold is 4,701 bytes, against
4,780 observed.

The 256 bytes is **read off the pinned framework, not measured**:
`framework-arduinoespressif32` `HardwareSerial.cpp:148` sets `_rxBufferSize(256)`
and nothing in this firmware calls `setRxBufferSize()` on `Serial1`.

One extra checksum error across that window is genuinely measured (1 to 2).

Harmless for a bring-up console, not harmless for a position source. The fix is
not "poll more often", because there is nowhere to poll from during a blocking
render. It was settled 2026-09-01 -- next section.

### The fix: an 8 KB ring, and why not a task

Settled 2026-09-01, step 1 of [`gnss-to-map-plan.md`](gnss-to-map-plan.md).
Three candidates were on the table: a bigger RX ring, the UART on its own task,
or an event-driven read. The ring won, and two findings decided it -- one about
what the product needs, one about how the measurement was being taken wrong.

**What a position source needs is the newest sentence, not the stream.** That
sounds obvious and it changes the sizing question completely. The maintainer's
constraint, 2026-09-01: the phone sends a position **at most once every 7 s**,
so anything that keeps the fix within about a second of current is more than
enough.

Follow what a ring too small for a block actually does. The driver does **not**
evict old bytes to make room -- when the ring is full it refuses the incoming
batch and switches the RX interrupts off until the app drains (ESP-IDF 5.5.2,
`components/esp_driver_uart/src/uart.c:1302`). So what survives a block is the
**oldest** part of it. Walk the clock: before a 15 s block the fix is from t=0,
the ring holds t=0 to t=1.3, and when the caller returns it parses up to t=1.3
and then reaches t=15 within a second. **1.3 is newer than 0.** The fix only
ever moves forward; an undersized ring costs position *age*, never a wrong
position. An earlier draft of this section claimed the opposite and it was
wrong.

That is why there is no task. A task removes an age penalty of about a second
on a rare event, and charges a mutex around `fix()`, a copy instead of a
reference, and a parser on a 2 KB stack at priority `configMAX_PRIORITIES-1`.
Nothing in the product asks for that. A caller that logged a track from the
receiver would ask for it; the ride recorder is in the phone.

**And the measurement had to be redone, because the first three runs measured
empty ground.** The device sat where its SD card had no tile coverage, so every
render drew nothing. `CMD:GOTO_MAP` there reported `0 tiles ok, 4 missing` and
the map console's `tiles` confirmed every tile missing at every zoom rung. The
mirror was copied onto the card and the same run repeated.

The gap is fourfold, so it is not a detail:

| | no tile coverage | real tiles |
|---|---|---|
| redraw | 481 ms | **2,687 ms** |
| map entry render | 478-712 ms | **2,696 ms** |
| worst render, whole zoom ladder | 712 ms | **4,072 ms** (1,361 kB, 2,682 ways) |
| worst main-loop block | 6,143 ms (mostly refresh and setup) | **5,760 ms** |

Any sizing taken from the left column would have been wrong. Coordinates are
deliberately not recorded here -- the run used the device's own persisted fix.

**The result, `rxBufferBytes` = 8192 on this board, verified on hardware
2026-09-01.** Baseline **810 B/s, 15.00 sentences/s** over 60 s by the device's
own clock. Then the whole zoom ladder plus a redraw, each a 33 s window:

| phase | sentences | expected | lost | rxfull | ovf | fifoovf |
|---|---|---|---|---|---|---|
| baseline, idle on map | 900 | 900 | 0 | 0 | 0 | 0 |
| redraw | 496 | 495 | -1 | 0 | 0 | 0 |
| zoom 0 | 496 | 496 | 0 | 0 | 0 | 0 |
| zoom 1 | 495 | 496 | 1 | 0 | 0 | 0 |
| zoom 2 | 495 | 495 | 0 | 0 | 0 | 0 |
| zoom 3 | 482 | 481 | -1 | 0 | 0 | 0 |
| zoom 4 | 495 | 495 | 0 | 0 | 0 | 0 |

The noise floor is **+/-1 sentence** here, against +/-37 in an earlier run whose
receiver rate drifted, so this "zero" is a much stronger statement than a zero
measured against a noisy baseline. `rxbuf=8192` in the same reply confirms the
driver granted what was asked -- `setRxBufferSize()` can return less.

**8192 does not cover everything, on purpose.** The maintainer reports redraws
reaching **15 s** in use, which is about 12.2 kB of arriving bytes. Per the
paragraph above that costs roughly a second of fix age and nothing else, so
buying 16 kB for it would spend RAM on something the product does not ask for.
`Gnss::ringOverflows()` reports it when it happens.

**What this is NOT: a controlled A/B.** The before figure (1024 bytes, 65
sentences lost on a 6,143 ms map entry) was taken over empty ground and the
after figure over real tiles. The two blocking windows are close, 6.1 s against
5.8 s, so the comparison is reasonable -- but 1024 was never run against a
loaded viewport, and nobody should quote this as one experiment. What is
directly measured is the after: at 8192, real map work at every rung lost
nothing.

### Sentence loss is observable now, and by the driver rather than by a guess

`rxNearlyFullEvents()` (`rxfull`) is this firmware's own inference: `poll()`
finds the ring within 32 bytes of full on entry. It fires on a stall that lost
nothing, and it reads 0 both when nothing was lost and when nothing was
measured -- so "rxfull=0" on its own is a check that cannot fail.

Two counters from the driver were added 2026-09-01:

- **`ringOverflows()`** (`ovf`) counts `UART_BUFFER_FULL`: the ISR could not
  push a batch into the ring and disabled the RX interrupts
  (`esp_driver_uart/src/uart.c:1302`). The refused batch is stashed and
  re-delivered, so this alone means "the ring filled", not yet lost data.
- **`fifoOverflows()`** (`fifoovf`) counts `UART_FIFO_OVF`, which is the loss:
  ring full, interrupts off, the hardware FIFO fills next and the rest is gone.

Both come through `HardwareSerial::onReceiveError()`
(`framework-arduinoespressif32` 3.3.7), which runs on the UART event task. The
callback only increments an atomic -- parsing there would put the whole parser
on that task's 2 KB stack at priority `configMAX_PRIORITIES-1`
(`HardwareSerial.cpp:177`), which is a much bigger decision than counting.

**Both undercount.** The driver's event queue is 20 entries deep
(`esp32-hal-uart.c:793`) and the ISR drops an event it cannot enqueue. Non-zero
means it happened; zero across a window whose sentence count also matches the
baseline is what "nothing was lost" looks like.

Two smaller reporting gaps were closed in the same pass, both because this
measurement needed them and neither existed: **`uptime`** on `GNSS_FIX`, so a
window is timed by the device's own clock instead of by the timestamps of
whatever log lines happen to sit near it, and **`rxbuf`**, the ring the driver
actually granted rather than the one that was requested.

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

- **`rxfull` fired on the blocked render, and it reports proximity, not loss.**
  Three statuses: `rxfull=0`, `rxfull=0`, then **`rxfull=1`** after
  `CMD:GOTO_MAP` (19 s after, not "immediately"). Silent while nothing blocked,
  which is the part that matters -- it is not simply always on.

  Precisely what it means: the buffer was within 32 bytes of full when `poll()`
  ran. At 816 B/s into 1024 bytes, a stall between about 1.22 s and 1.29 s trips
  it having lost nothing at all. It is a proxy, and the header now says "almost
  certainly" rather than "means". Loss itself is knowable only to the driver.
  n=1 positive against n=2 negative.
- **The first-sentence discard is verified, 2026-08-31.** A mid-stream open on an
  already-talking receiver -- 1,633 bytes and 36 sentences parsed -- reported
  **`cserr=0 ferr=0`**. Before the fix the identical situation put a 1 in one
  counter or the other depending on where the first sentence tore. So `cserr` now
  means what this file says it means.
- **The `cserr` / `ferr` split did NOT achieve what it was for** on its own, and
  the run before that proved it. One status showed `cserr=0 ferr=1`, which is what the split was
  meant to deliver -- and the `PROBE` in the same session, also a mid-stream open,
  showed `cserr=1 ferr=0`. Two mid-opens, one in each counter.

  The mechanism, found by review: which counter takes the torn first line depends
  on where the tear falls. Cut before the `*`, which is most of a sentence's
  length, and the tail still carries a valid `*hh`, fails the checksum, and lands
  in `cserr` -- exactly the pollution the split was supposed to remove. Only a cut
  inside the `*hh` lands in `ferr`. So the split alone achieved the goal about one
  time in ten.

  **Fixed properly rather than re-documented:** `poll()` now discards everything
  before the session's first `$`. The split stays, because the two failures really
  are different, but it is the discard that makes `cserr` mean what this file says
  it means. Unverified on hardware.
- **`utc` produced a correct value. The check does not reach the path.** The
  reported number was one second after the last logged ZDA, so the match is again
  inferred from the 1 Hz cadence rather than read off the wire -- the same
  weakness this file flags for its own coordinate check and failed to flag here.
  Worse for attribution: RMC and ZDA both write `fix_.utc` every second, so a
  correct value cannot say which parser produced it, and neither path is
  individually verified. `$GNRMC,170236.000,V,,,,,,,310826,,,N,V` did appear, but
  "its date was used" is not attributable with ZDA in the same stream, and
  "speed and course correctly ignored" was never observable at all -- `GNSS_NOFIX`
  prints neither field. That last one was read off the code and written as though
  the run had shown it.

  What settles it: host tests on recorded streams, one with ZDA stripped, one with
  RMC stripped, one crossing midnight. T-580.
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
- **A cold start cannot be produced today.** `power-management.md`'s G1 row
  (added on `develop` 2026-08-31, from a branch this session did not create) asks
  for `CMD:GNSS ON` from
  a receiver with a cleared almanac, and **no command in this firmware clears
  one** -- the L76K needs a CASIC restart command that is not implemented. Until
  it is, G1 is not executable and only warm figures are obtainable. T-581.
- **Cold-start TTFF**, outdoors, from a genuinely cold receiver -- longer than a
  49.7 s outage, or a receiver command that clears the almanac.
- **Is the SPI contention real at all?** Does GPIO46 go low during a refresh,
  and does an SX126x in reset park MISO high-Z? Datasheet plus the overlapping
  test above.
- ~~The RX buffer under a blocking render~~ -- **settled 2026-09-01**, an 8 KB
  ring on this board. See "The fix: an 8 KB ring, and why not a task". Still
  open inside it: a redraw of the reported 15 s length has never been captured
  with the counters running, so the age penalty it costs is arithmetic rather
  than measurement.
- **The unexercised parser paths**: southern and western hemispheres, a date
  rollover, a leap second, the two-digit year window, `quality=6`. Cheap to
  cover with host tests against recorded sentences, and worth doing before the
  upstream PR.
- Whether the receiver can stay powered during a ride or must be duty-cycled. A
  power question, and given the shared rail a LoRa question too.
