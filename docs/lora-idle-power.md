# LoRa idle power: the L series

Measurement plan for the T5 S3 Pro. Written 2026-09-08, nothing run yet.

One question first, and it is not "what will LoRa cost". The radio is **already
powered**. `PCA9535_IO00_LORA_GPS_EN` gates the SX1262 and the L76K receiver
together, the expander latches that pin high, and `disableGpsLora()` has never
run on this board. So the SX1262 sits on a live rail with `LORA_RST` undriven,
inside every ride figure this project has, and nobody has priced it. The parent
repo's `docs/lora.md` (constraint 2) and [`gnss.md`](gnss.md) carry the
evidence; T-244 is the latch itself.

**The L series prices five radio states and one hazard.** It also answers the
question the parent repo asks in T-250 -- the ~20-40 unexplained mA -- from the
other end, because the shared rail is the biggest single suspect in it.

Confidence marks as everywhere in this campaign: **[measured]** on hardware,
**[repo]** read off code, **[primary]** vendor document, **[assumed]** nobody
checked.

## What the instrument has to become first

Today's T5 S3 Pro numbers are **coulomb-count steps**: the BQ27220's state of
charge drops one notch, the analysis counts notches over hours. The two ride
rows read that way -- 134.8 +/- 1.5 mA with the frontlight at 40 %, 91.3 +/-
0.6 mA with it off -- needed 3.2 h and 5.3 h of riding each. They live on branch
`rel-tip`'s `power-management.md`, unmerged as of 2026-09-08.

That instrument cannot run this plan. Six states at 3 h each is 18 h of bench
time, and the effect we are chasing is **4.6 mA** (SX1262 receive current,
Semtech's SX126x product brief, `semtech.com/uploads/common/LoRa_products_sx126x.pdf`
-- **[primary]**), which is inside the step size.

The gauge already measures current directly. `BQ27220_CURRENT` is register
`0x0C`, signed mA, and `BatteryMonitor.cpp` reads it today -- but only inside
`isCharging()`, for the sign. Nothing logs the value.

**Why the register and not the percentage.** `0x0C` is a sense-resistor
measurement. State of charge is a *learned* quantity the gauge derives from a
design capacity nobody here configured, which is why every mA in this campaign
so far carries a plus-minus and a warning. Neither is trusted alone: the
cross-check below compares an integral of the register against the steps of the
percentage. **[read]**

**L0, no device needed.** Log it.

- Read `0x0C` from `BoardConfig::ACTIVE.batteryGauge.gaugeAddr` and add
  `batt_ma` to `power.csv`. The header is
  `src/PowerLog.cpp:23-25` and has 18 columns today, none of them a current.
- Add `rail` (the expander's IO00 level, read back rather than assumed) and
  `radio` (`off` / `reset` / `sleep` / `rx` / `tx`) columns, so a row says which
  leg it belongs to without a wall-clock schedule.
- Do the register read **in our own code**, not in `BatteryMonitor`. The SDK
  stays a mirror (`CLAUDE.md`, "Upstream is two tracks"), and a new field in its
  `Status` struct is a fork we would carry forever for one measurement.
- `tools/powercsv.py` learns one flag: mean `batt_ma` per leg with its standard
  error, grouped by the new `radio` column.

**The check that can fail, and must run before any L row is believed.** Integrate
`batt_ma` over a leg of at least one hour and compare it against the coulomb-count
steps of the same leg. Two instruments, one run. If they disagree by more than
20 %, the register is not calibrated on this board and the whole L series is void
until that is understood -- the gauge's sense-resistor gain is factory data we
have never verified (**[assumed]**). A sign error shows up in the same check:
`isCharging()`'s comment says positive means charging, so a discharge leg must
come back negative.

## The states

Each leg is one row. Frontlight **off** in every one -- it is tens of milliamps
(~43 mA by difference, `rel-tip`) and it would swamp the effect.

| id | state | how | what it isolates |
|---|---|---|---|
| **L1** | rail off, both parts dead | call `disableGpsLora()` at boot | the reference every other leg subtracts from |
| **L2** | rail on, `LORA_RST` undriven, UART unread | **today's build, no change** | **what we already pay for nothing** |
| **L3** | rail on, `LORA_RST` held low, L76K in standby over UART | one pin write + the receiver's standby command | splits the pair: L2 - L3 is the GNSS share |
| **L4** | rail on, SX1262 initialised then `sleep()` | RadioLib, `CustomSX1262`-class init | the cheapest state a working radio can hold |
| **L5** | rail on, SX1262 in RX continuous | RadioLib `startReceive()` | **the price of listening** |
| **L6** | L5 plus one 100-byte transmit per minute at +22 dBm | RadioLib, dummy payload | the beacon's real cost, PA and airtime included |
| **L7** | L5 with the map up and tiles streaming off the SD card | map activity, CRC-checked reads | the GPIO46 hazard under real load |

Derived answers, each a subtraction of two rows:

- **L2 - L1** -- today's waste. The reason this plan exists.
- **L2 - L3** -- the L76K's share of it.
- **L4 - L3** -- what an initialised radio costs asleep.
- **L5 - L4** -- listening.
- **L6 - L5** -- beaconing.
- **L7 vs L5** -- whether the panel and the SD card survive a live radio.

## Order, and where it stops

1. **L0** -- no device, no lock. Host tests green, `powercsv.py` flag, the
   cross-check tool.
2. **L1, L2, L3** -- one flash. No RadioLib in the build yet: L1 is a call that
   already exists, L2 is today's behaviour, L3 is a pin write and a UART command.
   **Gate:** if `L2 - L1` is under 2 mA, the latch is not a power problem, this
   plan's premise is dead, and T-244 drops to a correctness note. Say so and
   stop.
3. **L4, L5, L6** -- second flash, RadioLib linked. Also the first real answer to
   "what will the radio cost", which is a design input for MeshCore.
4. **L7** -- last, and only after the datasheet question in T-246 is settled:
   does an SX126x in reset park MISO high-Z. L7 with a live radio on a shared
   SPI bus is the deliberate hazard test, so it runs knowing what it risks.

## Method per leg

- **Three A-B-A cycles, 10 minutes a leg.** The gauge's current register removes
  the drift problem that forced long alternation on the X4
  (`power-plan.md`, "The plateau problem"), but temperature and state of charge
  still move, so pairs stay paired.
- **Frozen conditions**: Home screen (not the map, except L7), frontlight off,
  CPU 80 MHz, BLE advertising only, **USB out**, state of charge between 60 %
  and 85 %, one build for every leg of one comparison.
- **USB out is not optional.** With VBUS in, the charger supplies the load and
  the cell current stops meaning anything (`../../docs/usb-power-meter.md`).
  That also rules the bench meter out of this plan entirely: it sees VBUS, and
  every L leg runs on the battery.
- **The floor of what may be claimed is 1 mA.** Below that this instrument says
  "indistinguishable", not a number. L4 will probably land there, and that is a
  result.
- **Every row names the device and the build.** T5 S3 Pro, `TRAILINK_VERSION`
  from the row itself.

## Predictions, written before the run

Recorded so the run can refute them rather than confirm a feeling.

- **P1: `L2 - L1` = 20-30 mA**, nearly all of it the L76K. Quectel's L76K
  Hardware Design V1.0, 2021-10-20, gives 29 mA for both acquisition and
  tracking, 20 uA standby, 8 uA backup -- table read 2026-09-08 from the copy
  hosted at `files.waveshare.com`, a Quectel document on a distributor's server
  and not fetched from Quectel (**[primary, mirrored host]**).
  The SX1262 with `LORA_RST` undriven contributes under 2 mA.
- **P2: `L5 - L4` = 4-6 mA**, from Semtech's 4.6 mA.
- **P3: `L6 - L5` under 2 mA** at one advert a minute. A ~100-byte packet at
  SF8 / BW 62.5 kHz is **615 ms** of airtime -- recomputed 2026-09-08, symbol
  time 4.096 ms, 138 payload symbols, explicit header, CR 4/5, low-data-rate
  optimize off (own arithmetic, **not [measured] and not from a tool**). The
  184-byte maximum payload is 1046 ms. So even a 130 mA transmit averages near
  1 mA at that cadence.

  **The same number prices the air, not the battery.** One packet a minute is
  36.9 s of airtime an hour, **1.0 % duty cycle** -- exactly at the limit of a
  1 % sub-band. That is why "which sub-band is 869.618 MHz in" (T-285 in the
  parent repo) decides the beacon cadence, and the power budget does not.
- **P4: `L4 - L3` under 1 mA**, i.e. unprovable with this instrument. A radio in
  sleep is free.
- **P5: L7 shows no SD read failure and no corrupt tile.** Weak prediction: the
  one test that has run could not have failed, because the card reads finished
  before the panel bus ran.

If P1 comes back under 5 mA, the interesting number is elsewhere and T-250's
residual has to be hunted somewhere other than this rail.

## What this plan can never answer

- **The board's own floor.** The unit is enclosed, so no meter goes in series
  with the cell (`../../docs/hardware-policy.md`). Every L number is a
  difference against L1, not an absolute floor.
- **The SX1262's true sleep current.** Microamps are below the gauge. Only a
  bare board with a series meter reaches it, and that board is an open ask to
  LilyGo.
- **Anything about range, or about a second node.** This plan prices states, not
  a network.

## Rules that bind the runs

- **Take the device lock immediately before each flash, not before the build**
  (`CLAUDE.md`). Release it when the leg set is done, and close every serial
  capture first.
- **Ask before every upload.** Two flashes are planned here; that is two asks.
- **Identify the port before each flash.** `303a` is the S3.
- **L5 to L7 are devel-only.** A build that leaves a radio receiving or
  transmitting is gated out of `gh_release` / `gh_release_rc`. Widening it later
  is a separate decision (`CLAUDE.md`, "Security").
- **L6 transmits in the clear.** Dummy payload, never a real position: 869.618
  MHz is a public band and a position beacon identifies the rider.
- **Archive the working build** once a leg set completes, before the worktree
  goes away.
