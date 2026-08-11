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

## Status, 2026-08-11

- Instruments built: `power.csv` on the card, `stats` on the map console,
  `PowerTelemetry` counters behind both. See `power-management.md`, "The
  device measures itself now".
- Instruments **unverified on hardware**. No card has been read, no `stats`
  reply has come back over a real link.
- Two ride-derived draw figures exist (~46 mA and ~17 mA), both contaminated
  and both too coarse to build on.
- Nothing has been changed to save power yet. That is deliberate: measure
  first.

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

Phase 1 -- make the instruments trustworthy:

- [ ] Flash the telemetry build and confirm `power.csv` appears with sane rows.
- [ ] Confirm `stats` answers over a real BLE link from the phone.
- [ ] Cross-check one `stats` reply against the CSV row written beside it.
- [ ] Confirm `throttled_ms` is non-zero on the Home screen and zero on the
      map screen -- that is the code reading (`power-management.md`, item 3)
      being checked on hardware.
- [ ] Record run 1 (state 1, 2 hours) and run 2 (state 2, 2 hours).

Phase 2 -- baseline the four states:

- [ ] Runs for states 3 and 4.
- [ ] Write all four voltage slopes into `power-management.md`, with the
      caveats each one carries.
- [ ] One inline-meter cross-check, documented as system-plus-charging.

Phase 3 -- change one thing at a time, re-run state 3 after each:

- [ ] `CONFIG_BT_CTRL_MODEM_SLEEP=y` plus a mode/clock selection. First check
      whether the X4 has an external 32.768 kHz crystal; without one the
      choice of low-power clock is restricted. **Highest suspected saving.**
- [ ] Advertising interval and TX power set explicitly in
      `BlePositionServer::begin()`. Affects state 2 most.
- [ ] Split `preventAutoSleep()` into `preventAutoSleep()` +
      `preventThrottle()` (plan 07, step 2) so the map can decline the clock
      pin without becoming sleepable. Blocked on the open question below.
- [ ] Loop cadence: let the map take the 50 ms delay while it is only waiting
      for a fix. Cheap, small, low risk.
- [ ] `CONFIG_PM_ENABLE=y` with DFS. **Riskiest** -- APB frequency moves under
      drivers that may not take PM locks (SPI panel, ADC, USB CDC). Only after
      everything above, and only if the numbers justify it.

Phase 4 -- tune what the measurements expose:

- [ ] `MapFollow::kMaxPartialMoves` against real ghosting, now that refresh
      counts are visible per ride.
- [ ] Decide whether `power.csv` should stay on by default or move behind a
      setting, once its real cost and value are known.

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
- **Does the X4 have an external 32.768 kHz crystal?** Decides which BLE
  low-power clock modes are available, and therefore how much modem sleep can
  save.
- **How long does a viewport reset take at a reduced clock?** A reset is
  already close to two seconds at 160 MHz and a large share of it is software
  floating point, which scales with the clock. Whatever else changes, the
  reset must run at full speed.
- **What is the X4's real cell capacity?** 650 mAh is the spec sheet. Every
  mAh figure in this repo inherits that assumption.

## Runs

Newest last. One row per run, with the file kept alongside.

| Date | Build | State | Duration | Start mV | End mV | Slope mV/h | Notes |
|---|---|---|---|---|---|---|---|
| _(none yet)_ | | | | | | | |
