# LoRa bring-up on the T5 S3 Pro

The SX1262 console: what it does, what it proves, and what it deliberately
does not solve yet. Written 2026-09-16 and **run on two boards the same day**: the link works.
The measured run is in "The first contact" below. Claims are marked
`[measured]` on hardware, `[repo]` (read off code) or `[secondhand]` (read off
another project).

The assessment that scoped this work is the parent repo's `docs/lora.md`.

## What exists

`lib/LoraRadio` wraps RadioLib's SX1262 driver. It is thin on purpose: one
packet out, one packet in, and the wire settings in one place. No mesh, no
routing, no protocol. MeshCore comes later and would be impossible to debug on
top of an unproven radio, because **every wrong wire setting fails as silence**
-- a radio that hears nothing and is heard by nobody -- rather than as an
error.

`CMD:LORA` in `src/main.cpp` drives it. Devel-only: `-DENABLE_LORA_CMD` lives
in `env:t5s3pro` and in no release env.

```
CMD:LORA              LORA_STATE:ready= listening= pong= rail= freq= bw= sf= cr= power= rx= tx=
CMD:LORA ON           rail up, chip up, prints the version string off silicon
CMD:LORA OFF          radio to sleep, NRESET low, rail released
CMD:LORA RX ON|OFF    continuous receive; packets print as LORA_RX:
CMD:LORA TX <text>    one packet
CMD:LORA PING         one PING, waits for the far end's PONG
CMD:LORA PONG ON|OFF  answer every PING heard
CMD:LORA POWER <dbm>  -9..22
CMD:LORA FREQ <mhz>   150..960
```

### The two-board link test

`CMD:LORA PONG ON` on one board, `CMD:LORA PING` on the other. One exchange
reports **both directions**, which is why the reply carries numbers rather
than being a bare acknowledgement:

```
LORA_PONG:rtt=434ms here_rssi=-33.0 here_snr=11.8 there=1 -33.0 12.0
```

`here_*` is how this board heard the reply. `there=` is the sequence number
plus how the far end heard the ping. A one-sided report would hide an
asymmetric link, which is the normal failure at range.

## The wire settings, and why these

| setting | value | why |
|---|---|---|
| frequency | 869.618 MHz | MeshCore's own EU default |
| bandwidth | 62.5 kHz | MeshCore default |
| spreading factor | 8 | MeshCore default |
| coding rate | 5 | MeshCore default |
| sync word | 0x12 (private) | MeshCore default |
| preamble | 16 symbols | what MeshCore's `std_init` passes -- **but see below** |
| TX power | 14 dBm | bench default, not MeshCore's 22 |

They are MeshCore's defaults (its `platformio.ini`: `LORA_FREQ=869.618`,
`LORA_BW=62.5`, `LORA_SF=8`, plus `LORA_CR=5` in every variant) `[secondhand]`
so that a bring-up board and a MeshCore node share a channel and each is
evidence about the other. **Changing one of these changes which radios can hear
us**, which is the reason they live in one struct with the reasoning next to
them rather than being scattered as call arguments.

**The preamble is where "same channel as MeshCore" stops being true, found in
review 2026-09-16.** `CustomSX1262::std_init()` does pass 16
(`src/helpers/radiolib/CustomSX1262.h:57`), but every MeshCore node then calls
`RadioLibWrapper::setParams()`, which overrides it with
`preambleLengthForSF(sf)` -- and that is **32 for any SF of 8 or below**
(`src/helpers/radiolib/RadioLibWrappers.h:56`, `CustomSX1262Wrapper.h:20`).
At MeshCore's own default SF8, a real node transmits a 32-symbol preamble and
we transmit 16. Two of our own boards are unaffected, which is why the link
test passed; joining a MeshCore mesh is not, and T-2020 has to pin 32 rather
than 16.

TX power is the one deliberate difference: 14 dBm on a bench, raised by hand
for a range test. What is legal is narrower than what the chip allows, and the
EU 868 sub-band and duty cycle are still open questions (parent `docs/TODO.md`,
T-285).

## Three board facts that are not preferences

```
tcxoVoltage    = 1.8 V   SX126X_DIO3_TCXO_VOLTAGE
dio2AsRfSwitch = true    SX126X_DIO2_AS_RF_SWITCH
currentLimit   = 140 mA  SX126X_CURRENT_LIMIT
```

All three are read off `dz0ny/meshcore-paperui`, which runs MeshCore on this
exact board -- its `platformio.ini` `env:t5-epaper` also lists our four pins
unchanged (NSS 46, DIO1 10, NRESET 1, BUSY 47) `[secondhand]`.

**Only two of the three are actually deltas**, found in review: RadioLib's own
`begin()` already sets `setDio2AsRfSwitch(true)` and `setCurrentLimit(60.0)`
(`SX126x.cpp:54,57`). So the DIO2 line restates what the library does, the
current limit raises 60 mA to 140, and the TCXO voltage is the one setting that
would otherwise be wrong (RadioLib defaults to 1.6 V).

**The TCXO voltage is the first suspect if the radio answers SPI and never
hears anything**: without a stable clock the chip does everything except work,
and every call still reports success.

`useRegulatorLDO` stays false, i.e. the DC-DC converter. The LDO roughly
doubles receive current. `[assumed]` for this board.

## The first contact, 2026-09-16 `[measured]`

Two T5 S3 Pro boards on one desk, both on this build, `PONG ON` on one and
`PING` on the other. Four exchanges, **four replies, nothing lost**:

```
board A (pinging)                             board B (answering)
LORA_OK:ping=1 air=170ms                      LORA_RX:len=6 rssi=-33.0 snr=12.0 text=PING 1
LORA_PONG:rtt=434ms here_rssi=-33.0           LORA_OK:pong=1 -33.0 12.0
          here_snr=11.8 there=1 -33.0 12.0
```

| | value |
|---|---|
| RSSI, both directions | -32 to -33 dBm |
| SNR, both directions | 11.2 to 12.2 dB |
| transmit call, one 6-byte packet | 169-170 ms wall time |
| round trip, ping to pong | 434, 485, 485, 436 ms |
| packets lost | 0 of 4 |

**The boards were side by side, so -33 dBm says the radios work and says
nothing about range.** Range is bring-up step 4 and needs a bike, not a bench
(parent `docs/lora.md`).

**The 170 ms is the transmit call, not the airtime**, and the doc said
otherwise until review caught it: `air=` is `millis()` around a blocking
`LoraRadio::transmit()`, so it includes the SPI traffic, the BUSY waits and the
re-arm afterwards. Computed time on air for this packet is **157 ms**
(4.096 ms per symbol at SF8 / 62.5 kHz; 20.25 preamble symbols = 82.9 ms plus
18 payload symbols = 73.7 ms), which leaves about 13 ms of call overhead.

157 ms for six bytes is the budget every protocol decision above this layer
spends from, and it is why an advert cadence is a real design question rather
than a constant. The arithmetic also confirms the wire settings independently:
a 32-symbol preamble would have taken 222 ms, so these boards really did
transmit with 16.

The round trip is roughly two airtimes plus the far end's turnaround, which
means it is dominated by the radio and not by our loop.

## What the console proves, and what it cannot

**That a radio is there, powered and out of reset.** `CMD:LORA ON` prints the
16-byte version string from register 0x0320. Both boards answer
`SX1261 V2D 2D02` `[measured]`.

**It does not say which part is fitted, and an earlier version of this section
claimed it did.** RadioLib expects the string `SX1261` from an SX1262 as well
(`SX1262.h:16`, `RADIOLIB_SX1262_CHIP_TYPE`), so the two models answer
identically and `RADIOLIB_ERR_CHIP_NOT_FOUND` only separates "something
answers" from "nothing does". The SX1262 identity stays `[open]` in parent
`docs/lora.md`. What would settle it: the vendor schematic, or a measured
output sweep -- an SX1261 stops at +15 dBm where an SX1262 reaches +22.

The string is read with our own SPI transaction (opcode `0x1D`, the 16-bit
address, one dummy byte) because RadioLib's `readRegister()` is protected
behind `#if !RADIOLIB_GODMODE && !RADIOLIB_LOW_LEVEL` (`SX126x.h:849`), and
both are library-wide switches -- a strange price for one string.

## The three hazards this shares a board with

### 1. One rail, two radios

`PCA9535_IO00_LORA_GPS_EN` powers the GNSS receiver and the SX1262 together
(`BoardT5S3Pins.h:70`). So `CMD:GNSS OFF` must not kill a listening radio, and
`CMD:LORA OFF` must not blind a map following a fix.

`t5s3RailHold(user, on)` in `src/main.cpp` holds the rail for a **bitmask of
users**, not a counter: a counter drifts the first time a caller asks twice,
and `Gnss::begin()` treats a second call as a no-op by design. The rail drops
when the last user lets go.

### 2. The SD card is on the same SPI bus

`SCLK14 MISO21 MOSI13`, with `SD_CS12` against `LORA_CS46`. A radio out of
reset drives MISO and corrupts card transfers -- measured 2026-09-03, nine runs
(`src/main.cpp`, `t5s3DeselectLoraRadio()`).

Every exit from `LoraRadio` therefore ends in `park()`, which sleeps the chip
**and holds NRESET low**. The reset is the part the card needs; the sleep is
only power.

### 3. `LORA_CS` also reaches the panel's i80 bus

GPIO46 is handed to LovyanGFX as `pinPwr`, which becomes the i80 driver's
`dc_gpio_num` -- the IDF rejects a negative one and this board has no spare
GPIO to give instead (`LilyGoT5S3LgfxConfig.cpp`, the `pinPwr` comment).
`pinOe` is `-1` since the SDK fix of 2026-09-03; it used to carry `LORA_CS` as
a placeholder, which is what put the radio's chip select on an EPD pin and
killed the SD card (BUG-037).

**An earlier version of this section said the panel bus holds GPIO46 LOW from
display init onward, and that a listening radio is therefore impossible. That
was a stale reading** of the pre-fix file, caught in review 2026-09-16.
`prepareEpdPower()` drives the pin HIGH before the bus is built, and
`lgfx::pinMode()` writes no level, so it stays HIGH: an ordinary refresh does
not select the radio.

What is genuinely open is narrower, and still unmeasured:

- the microsecond window during i80 bus setup where the peripheral drives DC on
  that pad, named in the SDK's own comment as the one case it does not cover;
- whether anything re-initialises the display bus while the radio is up, which
  would re-claim the pad for LCD_CAM;
- whether continuous receive survives a full map redraw at all, with tile reads
  on the same SPI bus.

Both users bracket the bus in `SPI.beginTransaction()`, so transfers serialise
on the Arduino bus lock. **Serialised is not the same as tested** -- T-2019, and
the reason `CMD:LORA` is a console rather than a background service.

**First look, 2026-09-16, and then the run that actually asked the question.**

The first attempt used zoom rungs 2 and 3, where a render takes 49 to 909 ms,
and packets two seconds apart: 10 of 10 delivered in both a control phase and a
rendering phase, with the card genuinely in use. It proved nothing, because
**no packet ever arrived while a render was running**.

Rung 6 renders for 3 to 3.9 seconds, so the second run forced exactly that
window. Twenty pings 1.2 s apart at the rendering board, which also answered
each one, so radio transmits landed in the same window as the card reads:

| | result |
|---|---|
| pings heard by the rendering board | **8 of 20** |
| pongs that reached the sender back | 4 of 20 |
| render windows, from the board's own log | 8, up to 3904 ms |
| packets reported inside a render window | **0** |
| card errors, CRC failures, radio errors | **0** |

**Integrity is fine and throughput is not.** Nothing was corrupted: no tile read
failed, no CRC mismatch, no `LORA_RX_ERR`. What happened is that traffic
arriving during a render is lost.

**The mechanism, inferred rather than measured**: `loraPoll()` runs from
`loop()`, and an SX126x holds exactly one received packet. If `loop()` does not
run for three seconds, the second packet of that window overwrites nothing --
it simply never gets picked up, and the count lands near one per window, which
is what 8 heard across 8 windows looks like. The zero-inside-a-window column
says the same thing from the other side: nothing was *reported* during a
render.

**What this costs the design.** A device that listens while it draws cannot
service the radio from `loop()`. That is a real constraint on any always-on
mesh, and it is not the constraint this task was opened for: the bus hazard did
not appear at all. What was built in answer is "The radio has its own task now"
below -- written, not yet exercised.

**Still unmeasured**: whether a radio *transmit* can corrupt a card read. The
four missing pongs are consistent with the same starvation and do not separate
the two.

## The radio has its own task now `[written, not exercised]`

Written 2026-09-16 against the 8-of-20 measurement above, on branch
`lora/rx-task` off `feat/lora`. **Nothing here has run on hardware.** What it
has to pass before it counts is at the end of this section.

### Why `loop()` was the wrong place

A map render is not on the render task. `MapActivity` paints from its own
`loop()` (`src/activities/map/MapActivity.cpp`, `renderCurrent()`, and the note
at the top of its `loop()` about painting outside `Activity::render()`), so a
rung 6 redraw owns the Arduino loop task for 3 to 3.9 s. `loraPoll()` was the
last call in `main.cpp`'s `loop()`. An SX126x holds exactly one packet. Those
three facts multiply into one per render window, which is what 8 of 20 was.

Every reference port already answers this the same way, and **none of them uses
a packet FIFO** -- the packet is read on a task, not in an interrupt, because
reading it means SPI:

| port | how |
|---|---|
| OpenTrailPaper | `setDio1Action` ISR (`src/lora_radio.cpp:70`), mesh on a core-0 task at priority 2 (`src/main.cpp:644`), semaphore with a 250 ms timeout |
| `dz0ny/meshcore-paperui`, our board | `xTaskCreatePinnedToCore(mesh_task_fn, "mesh", 8192, NULL, 5, NULL, 0)`, `the_mesh_ptr->loop()` under a mutex |
| LilyGo factory firmware | `lora_task`, suspended when the radio is not wanted (`examples/factory/main/peri_lora.cpp:174`) |

MeshCore assumes it: `Dispatcher::loop()` carries a watchdog for a radio stuck
out of receive (`src/Dispatcher.cpp:74`), which only makes sense if something
calls it often.

### What was decided, and against what

**Core 0, priority 3.** `ActivityManager` pins its render task to core 1
(`src/activities/ActivityManager.cpp:40`) and the Arduino loop task runs there
too, both at priority 1. A radio task on core 0 is behind neither of them, and
3 is far below the BLE host's tasks, which must not wait for a radio read.

**Woken by a DIO1 interrupt, but never waiting on it alone.** `LoraRadio` gained
`setPacketAction()` / `clearPacketAction()`, thin pass-throughs to RadioLib's
`setPacketReceivedAction()`. The handler gives a semaphore and returns: SPI
needs a mutex and a mutex may not be taken in an ISR. The task's wait carries a
**100 ms tick** on top, because a missed edge would leave a listening radio deaf
while `LORA_STATE` still reported `listening=1`, and nothing downstream could
tell that from quiet air. With the radio off the wait is `portMAX_DELAY`, so the
task costs nothing until somebody turns the radio on.

**No new SPI bus lock, and that is a finding rather than an omission.** On this
board the SD card and the radio share the *global* `SPI` object --
`BoardConfig.h`'s `LILYGO_T5S3` profile sets `separateSpi = false`, so
`SDCardManager::begin()` takes the `SPI.begin()` branch rather than the
dedicated-HSPI one. Arduino's SPI takes a real mutex for the length of a
transaction (`esp32-hal-spi.c`, `SPI_MUTEX_LOCK`; the pinned framework's
`sdkconfig` says `CONFIG_DISABLE_HAL_LOCKS is not set`), and SdFat in
`SHARED_SPI` mode releases the card's chip select at the end of every operation
(`SdSpiCard.cpp`, `readSectors()` -> `readStop()` -> `spiStop()`). So the two
users' byte traffic already serialises across tasks. What no part of that
protects is RadioLib's own chip state and `LoraRadio`'s `rssi_`/`snr_` members,
so **one recursive mutex owns the chip**: the service task takes it around a
read, and `CMD:LORA` takes it across its whole body.

Recursive for the reason `HalStorage`'s is (`lib/hal/HalStorage.cpp`).
`loraStop()` is reached from the console *and* from the deep-sleep and restart
paths, so it locks itself -- and a plain mutex would then deadlock on the one
path that parks the radio before sleep.

**The task touches the chip and nothing else.** It reads the packet, answers a
`PING` if pong mode is on, and queues the bytes. `loop()` prints. SdFat is not
thread-safe and neither is an activity, so storage, the panel and the console
stay on the side they were already on -- and the "a received payload is
attacker-controlled text" sanitising stays in one place.

**The queue is 8 deep and never blocks the task.** By the time anything is
queued the chip is already listening again, so a full queue costs a console
line, not a packet -- counted and printed as `LORA_RX_UNPRINTED:<n>`, because on
a bench that counts packets the difference between "not heard" and "heard but
not printed" is the whole finding.

### Two things the console now reports differently

`LORA_RX:` gained `at=<ms>`, the service task's own `millis()`. A render can
hold the console for seconds after the packet was read and answered, so a bench
that timed arrivals by when they printed would be measuring the panel. `rssi`,
`snr` and `ferr` are captured with the packet for the same reason. Round-trip
time is computed from that stamp too.

`CMD:LORA ISR OFF` detaches the interrupt and leaves the tick as the only way a
packet gets read. It exists because the tick is the thing standing between a
missed edge and a silent deafness, and a fallback nobody exercises is a wish.
`LORA_STATE` gained `isr=` and `armed=` so both are readable.

### Cost

`env:t5s3pro` only -- `ENABLE_LORA_CMD` is in no release env. Against the same
build one commit earlier, both built in the same hour: **+1.4 kB of flash and
+24 B of static RAM**, plus about 5 kB of heap while the devel build runs
(a 4 kB task stack, the queue, the task control block).

### What a hardware pass has to check

The bench is the one that produced the 8 of 20: two boards, twenty pings 1.2 s
apart, the receiving board rendering continuously at zoom rung 6 and answering
each ping.

1. **Throughput.** 20 of 20 heard, no `LORA_RX_ERR`, no `LORA_RX_UNPRINTED`.
   This is the done-condition of T-2023 and the reason the branch exists.
2. **Service latency.** The gap between a packet's `at=` stamp and the ping's
   send time stays under 100 ms with a render running.
3. **Round trip.** Pong RTT during rendering within about 100 ms of the idle
   figure (the first contact measured ~500 ms).
4. **The render does not pay for it.** Rung 6 redraw times from the board's own
   log stay within 5 % of the pre-change build.
5. **The tick alone works.** Repeat a short run with `CMD:LORA ISR OFF`. Packets
   must still arrive.
6. **It stays up.** 30 minutes of receive plus rendering, ending with
   `LORA_STATE` still `ready=1 listening=1`, no watchdog reset and no
   `crash_report.txt` on the card.
7. **The card is not hurt by a radio on another task.** `CMD:SDBUS READ <path>`
   in a loop during traffic, CRC32 stable. This is also the one case T-2019 left
   open -- whether a radio *transmit* can corrupt a card read -- so run it with
   pong mode on, where the board transmits inside its own card reads.
8. **Idle power did not move.** With the radio off the task must never run: a
   VBUS reading against the pre-change build, both in the same hour, same board.
   With the radio listening and no traffic, the tick costs ten trivial wakes a
   second; measure it rather than assume it.

## The oscillator, answered on the desk 2026-09-16 `[measured]`

A review argued that our TCXO supply of 1.8 V was the most suspicious number in
the configuration, because LilyGo's examples use 2.4 or 3.0 V. **The argument
was wrong twice and the measurement settled it.**

Wrong first because those examples call `radio.begin()` before `setTCXO()`, and
RadioLib's default is 1.6 V -- so the vendor's own firmware initialises this
module *below* the value being called underfed, and its three different values
(1.6, 2.4, 3.0) say nobody there tuned it either.

Wrong second because the failure mode is not a slow loss of sensitivity.
**RadioLib hides a dead oscillator**: on `XOSC_START_ERR` it sets the TCXO
voltage to zero and retries in crystal mode (`SX126x.cpp:1444-1450`), so
`begin()` succeeds either way and proves nothing. `CMD:LORA OSC` asks the chip
instead -- clear the errors, force `STANDBY_XOSC`, read them back.

| | result |
|---|---|
| oscillator at 1.6 / 1.8 / 2.2 / 2.4 / 3.0 V | starts, `errors=0x0000`, every one |
| oscillator at 0 V (no DIO3 supply) | `begin()` fails, `-707 SPI_CMD_FAILED` |
| frequency error, board A / board B | **+641 to +657 Hz / -645 to -661 Hz** |
| link at BW 7.8 kHz | works, 1430 ms airtime, SNR 11-12 dB |

The 0 V row is the one that says this module **has** a TCXO fed from DIO3: take
the supply away and the radio does not come up at all.

The frequency error is equal and opposite at the two ends, which makes it a
real relative offset between the boards rather than an artefact: about 650 Hz
at 869.618 MHz is **0.75 ppm**, a TCXO figure. The narrow-bandwidth link is the
independent check -- at 7.8 kHz a sick oscillator does not get a packet through,
and both directions worked.

**So 1.8 V stays.** The setting is now measured rather than inherited.

## What the power amplifier draws `[measured]`

`CMD:LORA CW <seconds>` puts an unmodulated carrier up so a USB meter can read
the PA. Board A through the meter, whole-board VBUS at 5.25 V, one reader per
run, all in one session on 2026-09-16:

| commanded power | idle | carrier | PA delta |
|---|---|---|---|
| +10 dBm | 89.2 mA | 133.2 mA | **44.0 mA** |
| +14 dBm | 89.7 mA | 158.5 mA | **68.8 mA** |
| +17 dBm | 89.1 mA | 183.8 mA | **94.8 mA** |
| +20 dBm | 88.9 mA | 210.7 mA | **121.7 mA** |
| +22 dBm | 89.2 mA | 228.0 mA | **138.9 mA** |

The idle row is the same to within 0.8 mA across every run, and +22 dBm
measured twice in the session came back 137.2 and 138.9 mA -- about 1 %. Those
two are the reason the ladder can be read as a ladder rather than as five
separate numbers.

**This settles the part.** An SX1261 stops at +15 dBm; this radio keeps
climbing through 17, 20 and 22, and draws 139 mA of VBUS doing it. It is an
SX1262, which is also what LilyGo's product README says for this board.
The chip's own version register cannot say so -- see above.

**It is VBUS, not chip current.** The number includes the 5 V to 3.3 V
conversion and whatever else the board does while transmitting, so it is not
comparable to a datasheet's PA figure. What it is good for is exactly what it
was used for: a ratio between power settings, and a class of amplifier.

### The first two runs were charging current, not radio current

The 10 and 14 dBm runs were taken first and read an **idle of 356 mA**, four
times the settled figure, which swamped a 44 mA PA delta and produced an
inverted ladder. The cause is in the instrument's own caveat: VBUS is the board
**plus the charger**, and the cell was still taking a charge.

The rule this leaves: **a PA ladder is only readable once the idle row repeats**.
Take idle first, wait for it to stop moving, and re-run any step whose idle
disagrees with the others -- the ladder's whole value is the difference between
rows, and a drifting baseline destroys it silently.

## Deep sleep needs a latch, not just a park `[written, not exercised]`

`loraStop()` is not enough on its own, and the gap is not obvious.

It releases the **LoRa** user of the shared rail. With the GNSS receiver
holding the other one (`CMD:GNSS ON`, or the map following a fix), the rail
stays up through deep sleep -- the PCA9535 is an external chip and keeps its
output register. Meanwhile GPIO1 goes hi-Z as the SoC sleeps, the SX126x's
reset pull-up releases the chip, and the radio wakes powered, out of reset, on
the SD card's SPI bus. That is the exact state `t5s3DeselectLoraRadio()` exists
to prevent, arrived at from the other direction.

The fix is the vendor's own sequence (LilyGo `T5S3-4.7-e-paper-PRO`,
`examples/factory/main/ui_port.cpp`): sleep the radio, drive NRESET low, hold
the pad, then cut the rail.

```
lora.sleep + NRESET low        enterDeepSleep()
gpio_hold_en(T5S3_LORA_RST)    latch, or the pad floats while the SoC sleeps
gpio_deep_sleep_hold_en()
rail down (if nobody holds it)
```

**Both halves are required.** A held pad ignores `digitalWrite()`, so
`setup()` calls `gpio_deep_sleep_hold_dis()` and `gpio_hold_dis()` before
anything touches the radio -- without that, the next boot could not reset it
and `begin()` would answer `CHIP_NOT_FOUND` until the power was pulled. The
vendor does both; copying only the sleep half converts a rare SD hazard into a
certain radio outage.

**Not exercised on hardware.** The code is on both boards, but nothing has
deep-slept with the GNSS receiver holding the rail and then checked the card.
That run is what would confirm it.

## The first boot after a flash lands in download mode, twice over

Flashing this board and then wondering why it is silent cost an hour on
2026-09-16, and the same thing happened on the first board in August. The
sequence, so the next session skips it:

1. `pio run -t upload` succeeds, every image verified. The board then boots
   into the ROM loader, not into the app: `rst:0x15 (USB_UART_CHIP_RESET)`,
   `boot:0x23 (DOWNLOAD(USB/UART0))`, `waiting for download` `[measured]`. On
   this board **DTR drives GPIO0**, so a reset caused by the host -- esptool's
   own, or merely closing a serial capture -- re-enters the loader
   (parent `docs/PROGRESS.md`, 2026-09-06 and the browser-flasher entry).
2. Left alone, it then drops off USB entirely. That is **deep sleep**, not an
   unplug, and it looks exactly like a dead board.
3. The way out is the board's own power button: **hold BOOT (left, top) for
   about two seconds**. A tap does not do it -- the wake check subtracts boot
   time (`lib/hal/HalGPIO.cpp`).

The e-paper panel makes this worse than it needs to be: it keeps the last
stock frame with no power, so "the old firmware is still on screen" is not
evidence of anything.

## What is not done

- **Range is unmeasured.** The link works at desk distance; that is all.
  Bring-up step 4 is a moving bike.
- **No host test pinning the wire bytes.** Parent `docs/lora.md` bring-up step
  3 asks for one, after OpenTrailPaper's `tools/mesh_test/`: sync word,
  preamble, channel hash and slot numbers checked against known-good bytes,
  because each of them fails as silence. The settings live in one struct now,
  which is the precondition for such a test, not a substitute for it.
- **The service task has never run on hardware.** It is written and it builds;
  "The radio has its own task now" lists the eight things a pass has to check,
  starting with 20 of 20 packets through a rung 6 redraw (T-2023).
- **No duty-cycle accounting.** The console will transmit as often as it is
  told to.
- **No power figure.** The L series in `lora-idle-power.md` prices the radio's
  states; `CMD:LORA` is the instrument it needs (`L4` sleep, `L5` receive, `L6`
  transmit) and those legs can run as soon as a board and a meter are free.
