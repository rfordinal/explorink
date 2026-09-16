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
LORA_PONG:rtt=412ms here_rssi=-61.0 here_snr=9.2 there=7 -58.0 10.5
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
| preamble | 16 symbols | MeshCore default |
| TX power | 14 dBm | bench default, not MeshCore's 22 |

They are MeshCore's defaults (its `platformio.ini`: `LORA_FREQ=869.618`,
`LORA_BW=62.5`, `LORA_SF=8`, plus `LORA_CR=5` in every variant) `[secondhand]`
so that a bring-up board and a MeshCore node share a channel and each is
evidence about the other. **Changing one of these changes which radios can hear
us**, which is the reason they live in one struct with the reasoning next to
them rather than being scattered as call arguments.

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
unchanged (NSS 46, DIO1 10, NRESET 1, BUSY 47) `[secondhand]`. **They are the
first suspects if the radio answers SPI and never hears anything**: a wrong
TCXO voltage leaves the chip without a stable clock, and DIO2 not switching the
antenna transmits into a dead path while reporting success.

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
| airtime, one 6-byte packet | 169-170 ms |
| round trip, ping to pong | 434, 485, 485, 436 ms |
| packets lost | 0 of 4 |

**The boards were side by side, so -33 dBm says the radios work and says
nothing about range.** Range is bring-up step 4 and needs a bike, not a bench
(parent `docs/lora.md`).

Airtime is worth reading twice: **170 ms for six bytes** at SF8 / 62.5 kHz.
That is the budget every protocol decision above this layer spends from, and it
is why an advert cadence is a real design question rather than a constant.

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
unless `RADIOLIB_GODMODE` is set, and a library-wide switch is a strange price
for one string.

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

### 3. `LORA_CS` is also the panel's `pin_oe` / `pin_pwr`

LovyanGFX drives GPIO46 as part of the display bus
(`LilyGoT5S3LgfxConfig.cpp:162,166`), and leaves it LOW from display init
onward. **So a panel refresh and a live radio transaction cannot be separated
by this code today.** The console is usable because a bench session is not
redrawing the map while it pings.

**This is the open piece of work, not an oversight.** A radio that listens
while the map renders needs arbitration that does not exist: either the panel
config stops using GPIO46 as filler, or radio transactions are scheduled
against refreshes. It has to be answered before any always-on mesh, and it is
the reason `CMD:LORA` is a console rather than a background service.

## What it costs

Measured 2026-09-16 on this laptop, `env:t5s3pro`, the same worktree an hour
apart, the only difference being the change itself (`git stash` for the
baseline):

| | flash | static RAM |
|---|---|---|
| release tip, no radio | 3,956,499 B | 71,904 B |
| with RadioLib and `CMD:LORA` | 3,983,911 B | 72,616 B |
| **cost** | **27,412 B (27 kB)** | **712 B** |

That is the whole radio driver plus the console. It is a devel-only env, so no
release build pays it today.

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
- **No duty-cycle accounting.** The console will transmit as often as it is
  told to.
- **No power figure.** The L series in `lora-idle-power.md` prices the radio's
  states; `CMD:LORA` is the instrument it needs (`L4` sleep, `L5` receive, `L6`
  transmit) and those legs can run as soon as a board and a meter are free.
