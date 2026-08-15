# Power review: plan, methodology, TODO

Continuous work, not a one-off. This device is battery powered, a ride lasts
hours, and until 2026-08-11 nothing in either repo could say where a milliamp
went. This file is the working plan: what the instruments are, how a run is
done so two runs can be compared, what to change and in what order, and what
each run found.

Findings themselves do **not** live here. They go in
[`power-management.md`](power-management.md), which is the topic doc. This is
the campaign.

Related: [`optimization/07-power-and-lifecycle.md`](optimization/07-power-and-lifecycle.md)
(the 2026-08-06 code review of the same area -- its step 1 is this plan's
phase 1).

## The target: three days on a hike

**Set 2026-08-15.** A hike trip lasts three days and the rider does not carry a
charger. So the target is **72 hours on one charge with the map up and the phone
linked**, not "as long as we can get".

The budget follows from the cell:

| | |
|---|---|
| Cell | 650 mAh (spec sheet, **not measured** -- see open questions) |
| Target | 72 h |
| **Budget** | **9.0 mA average** |
| Measured today | **45 mA** (run 1 below) |
| Gap | **factor 5** |

Two things this target is not:

- **It is not a zoom or refresh setting.** The panel costs nothing to hold an
  image, and run 1 was mostly stationary, so the panel and the renderer were
  close to free for 11.5 hours and the device still drew 45 mA. Tuning rung
  choice or refresh cadence cannot close a factor of five. Those are phase 4
  knobs, worth something only once the floor is near the budget -- at a 2-3 mA
  floor a viewport reset costs a measurable share, at 45 mA it is noise.
- **It is not reachable by throttling alone.** See route A below.

The target is about one thing: **how much of the 72 hours the CPU and the radio
spend asleep**. Today the answer is none of it.

Marketing wants this number too, so it has to survive being quoted. Nothing goes
on the public site until a real 72-hour run has happened on hardware.

## Status, 2026-08-15

- **First long endurance run done** (run 1 below): 11.5 h, map up, hike mode,
  rung 0, BLE linked the whole time, mostly stationary. 100 % -> 21 %.
  **~45 mA average**, which extrapolates to ~14.6 h on a full charge.
- That agrees with the earlier contaminated ~46 mA ride figure. Two independent
  arrivals at the same number; it is now the working baseline for state 3.
- The three-day target above is set against it. Routes and estimates below.
- `power.csv` for run 1 has **not been pulled off the card yet**, so the split
  between CPU, radio and panel is still read-off-the-code, not measured.
- Nothing has been changed to save power yet. That is still deliberate.

## Status, 2026-08-11

- Instruments built: `power.csv` on the card, `stats` on the map console,
  `PowerTelemetry` counters behind both. See `power-management.md`, "The
  device measures itself now".
- **Flashed and part-verified on hardware** (build `f6372ea6`): `stats`
  answers over serial and over BLE, and its numbers already confirmed two
  read-only claims (the map never throttles; the loop runs at ~100 Hz there).
  `power.csv` writes with no error, but no card has been read yet.
- One new open item: `rssi()` returns 0 on a live link
  (`power-management.md`, end of the instruments section).
- Two ride-derived draw figures exist (~46 mA and ~17 mA), both contaminated
  and both too coarse to build on.
- Nothing has been changed to save power yet. That is deliberate: measure
  first.

## Where the 45 mA goes -- read off the code, not measured

**Confidence: read.** Every claim below is a citation, not a measurement. The
split is what run 1's `power.csv` will confirm or refute; until then it is a
hypothesis with a good paper trail.

The map screen never lets the device rest, and it takes four steps to see why:

1. `MapActivity::preventAutoSleep()` returns true whenever the BLE server is
   running (`src/activities/map/MapActivity.cpp:2492`).
2. `main.cpp` treats that as user activity and resets `lastActivityTime` on
   **every** loop iteration (`src/main.cpp:783-786`).
3. So the inactivity branch that would call `setPowerSaving(true)` is never
   reached (`src/main.cpp:873-876`). The CPU holds 160 MHz for the whole
   session. On X4 the throttled alternative is 10 MHz
   (`lib/hal/HalPowerManager.h:31`), so this is the single largest lever in the
   file.
4. The loop then takes `delay(10)` (`src/main.cpp:879`), which is a
   `vTaskDelay` onto the FreeRTOS idle task. **`CONFIG_PM_ENABLE` is not set**
   (`sdkconfig.defaults:1680`), so the idle task does not light-sleep -- it
   spins at 160 MHz. Roughly 99 % of the run was full-clock idling.

The radio adds to it but is not the main cost:

- **`CONFIG_BT_CTRL_MODEM_SLEEP` is not set** (`sdkconfig.defaults:993`), so the
  BLE radio stays powered between connection events instead of sleeping through
  the gaps.
- Connection parameters are already sane, which is why the radio is the smaller
  suspect: idle interval 30-50 ms with latency 4
  (`lib/BlePositionServer/include/BlePositionServer.h:522-538`), so a mandatory
  event lands only every ~150-250 ms.

The panel and the renderer were close to free in run 1 -- stationary means few
viewport resets, and holding an image costs nothing.

## Three routes to 9 mA

Estimates are **assumed**, from the ESP32-C3 datasheet and the code reading
above. None is measured. They exist to order the work, not to be quoted.

### A. Throttle the CPU, sleep the modem

Split `preventAutoSleep()` into `preventAutoSleep()` + `preventThrottle()` so
the map can drop to 10 MHz while it is only waiting for a fix, and set
`CONFIG_BT_CTRL_MODEM_SLEEP=y`.

- Estimate: 45 mA -> 20-25 mA, so **26-32 h**.
- **Does not reach the target**, even if everything lands. Worth doing anyway:
  it is the cheapest, lowest-risk step, and it makes every later measurement
  cleaner.
- Blocked on: does a live NimBLE link survive a drop to 10 MHz? (open
  questions, below).

### B. Automatic light sleep

`CONFIG_PM_ENABLE=y` plus tickless idle, so the device sleeps between BLE
connection events and wakes for each one.

- Estimate at 5 % duty: 35 mA awake + ~1 mA asleep = **~2.7 mA**, an order of
  magnitude past the target.
- **This is where three days actually lives.**
- Riskiest item in the campaign: APB frequency moves under drivers that may not
  take PM locks -- SPI panel, ADC, USB CDC.
- Constrained by the clock source. `CONFIG_RTC_CLK_SRC_INT_RC=y`
  (`sdkconfig.defaults:1568`, and `CONFIG_ESP32C3_RTC_CLK_SRC_INT_RC=y` at
  3478) means the internal RC oscillator. BLE with light sleep works on the
  internal RC, but its drift forces wider RX windows, which eats part of the
  saving. An external 32.768 kHz crystal would be much tighter. Whether X4 has
  one is still unanswered, and that question is now **blocking**, because it
  decides whether B is good or excellent.

### C. Duty-cycle the whole link

Do not hold a BLE link at all while walking. The phone sends a fix, the device
deep-sleeps 30-60 s, wakes on the RTC timer, reconnects, reads, and redraws only
if the position moved enough to matter.

- Deep sleep on C3 is ~5 uA. At 3 s awake per minute (5 % duty, 40 mA) the
  average is **~2 mA**.
- **Independent of the crystal question**, which makes it the fallback if B
  stalls there.
- Costs: the position is up to a minute stale, and deep sleep loses RAM, so
  every wake is a boot and the map must re-render from the tile cache.
- A hiker moves ~65 m in a minute, so staleness is acceptable on foot and
  useless on a motorbike. **This is a hike-only power profile**, not a global
  change.
- Largest unknown: what a reconnect actually costs in radio-seconds. If
  reconnect takes 5 s the budget changes shape.

### The shape of the answer

Do A first because it is cheap and clarifies the rest. Then B, which is the one
that reaches the target. Keep C as the route that dodges B's blocking question.

Expose it to the rider as **one hike power profile setting** that picks the duty
cycle -- not as a pile of zoom, refresh and clock knobs. The rider already chose
hike mode; that should be enough.

## The constraints every run works under

1. **Charging cannot be disabled in software on X4** -- no charge-enable pin,
   no charger IC (`power-management.md`, "Charging cannot be turned off").
   So a run means: unplug, and do not plug in until it is over.
2. **No USB means no serial.** Cutting VBUS kills USB-Serial-JTAG. The card
   and BLE are the only ways data leaves the device during a run.
3. **Percent is too coarse.** 1 % of 650 mAh is 6.5 mAh, about 20 minutes of
   riding. Use `batt_mv`, and prefer long runs over precise arithmetic on
   short ones.
4. **A LiPo discharge curve is not linear.** Two runs are comparable only if
   they start at a similar state of charge. Same start voltage window, or the
   comparison is worthless.
5. **Temperature moves both the cell and the ADC.** A winter ride and a desk
   run are not the same experiment. Record where a run happened.

## Methodology: how to do one run

Fixed shape, so runs are comparable:

1. Charge to full, unplug, let the device rest ~10 minutes. Note the starting
   `batt_mv` from the first `power.csv` row.
2. Put the device in **one** state and leave it there for at least 60 minutes.
   Longer is better -- 2 hours makes the millivolt drop comfortably larger
   than the ADC noise.
3. Do not touch buttons. Every press restores full clock and resets the
   inactivity timer (`src/main.cpp:719-723`), which changes the thing being
   measured.
4. At the end, pull the card and keep `power.csv`. Rename it per run;
   the device appends to the same file across boots, so the row where
   `uptime_s` restarts is a reboot, not a gap.
5. Record the run in the table at the bottom of this file.

The four states worth isolating, in this order:

| # | State | What it isolates |
|---|---|---|
| 1 | Home screen, untouched | the idle floor, including the 10 MHz throttle actually engaging |
| 2 | Map screen, no phone connected | BLE advertising + pinned 160 MHz, no traffic |
| 3 | Map screen, phone connected, fix every 10 s | the real riding case |
| 4 | Tile sync, transfer running | the worst case |

State 3 is best driven by `tools/blereplay.py` (parent repo) rather than a real
ride: same fixes, same cadence, every time. A real ride cannot be repeated.

### Reading the numbers

Per interval, from two rows of `power.csv`:

- `dV = batt_mv(t1) - batt_mv(t0)` over `dt = uptime_s(t1) - uptime_s(t0)`.
  Voltage slope is the primary comparison between builds. It needs no capacity
  assumption and no percentage.
- `mAh` only if a capacity number is being claimed: `dPct/100 * 650`. Say that
  650 mAh is the spec sheet, not a measured capacity.
- `ref_full/half/fast/window` and `panel_busy_ms` deltas: what the panel did.
  A build that halves the refresh count and does not move the voltage slope
  has proven the panel is not the cost.
- `loops` and `loop_busy_ms` deltas: duty cycle. `loop_busy_ms / dt` is the
  fraction of the run the CPU spent working.
- `throttled_ms` delta: whether the CPU ever left full clock. Expected 0 on
  states 2-4 today.

### Cross-checks

- An inline USB meter measures system + charging, so it can only bound the
  total. Useful once, as a sanity check against the voltage slope; never as
  the primary number.
- `stats` over BLE during state 3 should agree with the CSV rows written at
  the same time. If it does not, one of the two instruments is wrong and that
  is the first bug to fix.

## TODO

**Working rule, agreed 2026-08-15: one option at a time, tested hard before the
next.** Each item in phase 3 is a separate branch, a separate build and a
separate run of at least 2 hours in state 3. Two changes in one build cannot be
told apart afterwards, and this campaign has one instrument and one device.

Phase 1 -- make the instruments trustworthy:

- [x] Flash the telemetry build (2026-08-11, `f6372ea6`).
- [x] Confirm `stats` answers over a real BLE link.
- [x] Confirm `throttled_ms` stops rising on the map screen and rises on Home
      -- the code reading (`power-management.md`, item 3) checked on hardware.
- [ ] Read a real `power.csv` off the card and check the rows are sane. **Run 1
      is sitting on the card now** -- ~690 rows, the first real state-3 data.
      Pull it before anything else; it turns the 45 mA decomposition above from
      read into measured.
- [ ] Cross-check one `stats` reply against the CSV row written beside it.
- [ ] Chase `rssi()` returning 0 on a live link, or drop the line from `stats`.
- [ ] Record run 1 (state 1, 2 hours) and run 2 (state 2, 2 hours).

Phase 2 -- baseline the four states:

- [x] State 3 (map, phone connected): run 1, 2026-08-15, ~45 mA from percent.
      Coarse -- redo the arithmetic from `batt_mv` once the card is read.
- [ ] Runs for states 1, 2 and 4. State 2 (map, no phone) minus state 3 is the
      only clean way to price the radio.
- [ ] Answer the crystal question -- it gates route B and nothing else in the
      list can substitute for it.
- [ ] Write all four voltage slopes into `power-management.md`, with the
      caveats each one carries.
- [ ] One inline-meter cross-check, documented as system-plus-charging.

Phase 3 -- one change per branch, per build, per run. Re-run state 3 after each
and record it in the table below before starting the next.

Route A (cheap, does not reach the target on its own):

- [ ] Loop cadence: let the map take the 50 ms delay while it is only waiting
      for a fix. Cheapest possible first step, low risk.
- [ ] Split `preventAutoSleep()` into `preventAutoSleep()` +
      `preventThrottle()` (plan 07, step 2) so the map can decline the clock
      pin without becoming sleepable. Blocked on the 10 MHz link question.
- [ ] `CONFIG_BT_CTRL_MODEM_SLEEP=y` plus a mode/clock selection.
- [ ] Advertising interval and TX power set explicitly in
      `BlePositionServer::begin()`. Affects state 2 most.

Route B (the one that reaches three days):

- [ ] Settle the external 32.768 kHz crystal question first.
- [ ] `CONFIG_PM_ENABLE=y` with DFS and tickless idle. **Riskiest** -- APB
      frequency moves under drivers that may not take PM locks (SPI panel,
      ADC, USB CDC). Expect to find a driver that does not.
- [ ] Long run: 12 h minimum, then a real 72 h run before any public claim.

Route C (fallback, and a hike-only profile in its own right):

- [ ] Measure what one disconnect-sleep-reconnect cycle costs before building
      anything. If reconnect is expensive the whole route changes shape.
- [ ] Phone-side half: the app has to tolerate a peripheral that vanishes on
      purpose and comes back a minute later, without treating it as a fault.
- [ ] Wake-and-redraw path: every wake is a boot, so the map must come back
      from the tile cache fast enough to be worth it.

Phase 4 -- tune what the measurements expose:

- [ ] `MapFollow::kMaxPartialMoves` against real ghosting, now that refresh
      counts are visible per ride.
- [ ] Decide whether `power.csv` should stay on by default or move behind a
      setting, once its real cost and value are known. **This gets sharper with
      the three-day target**: a once-a-minute write keeps the SD card from ever
      settling, which is noise beside 45 mA and could dominate a 2-3 mA floor.
      The instrument may end up being the thing that stops the target being
      met.
- [ ] Rung choice and refresh cadence against the floor, once there is a floor.
      Worth ~0.4 mA at 30 viewport resets/h (a reset is ~2 s of 160 MHz CPU) --
      noise today, a real share of a 2-3 mA budget.

## Ideas not yet worth a TODO

- Gate the ADC button ladder: sample it every other iteration, or only when a
  press is plausible. Saves two ADC conversions per iteration; probably noise
  next to the radio, so it needs a measurement before it is worth the risk to
  input feel.
- Drop the CPU to 80 MHz (not 10) while a BLE link is live. Smaller, safer
  step than the throttle split, and it answers the same open question.
- Ask the phone for a slower connection interval between fixes and a fast one
  only during a transfer. The central owns the interval, so this is
  phone-side work (`BlePositionServer::connIntervalUnits()` is how the device
  sees what it got).
- Log a marker row into `power.csv` when a run starts, so a run's rows can be
  found without matching timestamps by hand.

## Open questions

- **Does an established NimBLE link survive a drop to 10 MHz?** The
  hardware-verified finding is about `NimBLEDevice::init()` hanging, not about
  steady state (`power-management.md`). Nothing has tested a throttle with a
  connection up. If it cannot, the throttle split is dead and phase 3 loses
  one item.
- **Does the X4 have an external 32.768 kHz crystal? BLOCKING.** Decides which
  BLE low-power clock modes are available, and therefore how much modem sleep
  and light sleep can save. Promoted to blocking 2026-08-15: it gates route B,
  which is the only route with a measured path to the three-day target. The
  config today selects the internal RC (`sdkconfig.defaults:1568`), but that is
  a default, **not evidence about the board**. Settle it by inspecting the
  board or by building with `CONFIG_RTC_CLK_SRC_EXT_CRYS=y` and seeing whether
  the clock calibrates -- do not infer it from the sdkconfig.
- **How long does a viewport reset take at a reduced clock?** A reset is
  already close to two seconds at 160 MHz and a large share of it is software
  floating point, which scales with the clock. Whatever else changes, the
  reset must run at full speed.
- **What is the X4's real cell capacity?** 650 mAh is the spec sheet. Every
  mAh figure in this repo inherits that assumption -- including the 9.0 mA
  three-day budget, which moves with it.
- **What does a BLE reconnect cost?** Route C's whole case rests on the answer
  and nothing has measured it. Radio-seconds per disconnect-sleep-reconnect
  cycle, at the connection parameters the phone actually grants.

## Runs

Newest last. One row per run, with the file kept alongside.

| Date | Build | State | Duration | Start mV | End mV | Slope mV/h | Notes |
|---|---|---|---|---|---|---|---|
| 2026-08-15 | unrecorded | 3 | 11 h 30 min | -- | -- | -- | Run 1. See below. |

### Run 1 -- 2026-08-15, first long endurance run

The first run long enough to mean anything, and the run the three-day target is
set against.

- **State 3**: map screen up the whole time, BLE linked the whole time, hike
  mode, rung 0, area Praha. Mostly stationary, little movement.
- **Percent**: 100 % at 10:30, 21 % at 22:00. 79 % over 11.5 h = **6.9 %/h**.
- **Derived draw**: 79 % of 650 mAh = 513 mAh over 11.5 h = **~45 mA**.
- **Extrapolated full cycle**: ~14.6 h.

Caveats, all of which matter:

- **Percent, not millivolts.** Constraint 3 says not to do this. It is done
  here because the run was long enough that 1 % of coarseness is small against
  79 %, and because the CSV had not been read yet. Redo it from `batt_mv` when
  the card is pulled.
- **Build not recorded.** Fix for run 2: note the build before unplugging.
- **Stationary is optimistic.** A walking hiker triggers viewport resets this
  run did not pay for, so 14.6 h is a ceiling for state 3, not an expectation on
  a trail.
- **Agrees with the earlier ~46 mA ride figure**, which was contaminated. Two
  independent routes to the same number is why 45 mA is treated as the working
  baseline rather than one more soft data point.

`power.csv` for this run is still on the card -- ~690 rows. Reading it is the
top item in phase 1.
