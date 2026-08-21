# Power review: plan, methodology, TODO

Continuous work, not a one-off. This device is battery powered, a ride lasts
hours, and until 2026-08-11 nothing in either repo could say where a milliamp
went. This file is the working plan: what the instruments are, how a run is
done so two runs can be compared, what to change and in what order, and what
each run found.

Findings themselves do **not** live here. They go in
[`power-management.md`](power-management.md), which is the topic doc. This is
the campaign.

The runnable procedures -- order of work, per-experiment steps, the pre-flash
checklist, the instrument fallbacks and the stop conditions -- are in
[`power-test-runbook.md`](power-test-runbook.md). This file stays the campaign and
its methodology; the runbook is what a session follows.

The parked map screen has its own design doc since 2026-08-19:
[`power-idle-sleep.md`](power-idle-sleep.md) -- what parks the device, what wakes
it, the power lab screen, and the experiment order. It supersedes route C below.

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
| Measured today | **44.4 mA** (run 1 below, from `batt_mv`) |
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

### The scenario the 72 hours has to hold for (defined 2026-08-21)

The number above had no scenario attached, which made it unfalsifiable: 72 hours
of a map nobody looks at and nothing updates is not the claim. The maintainer's
definition, and from here the one the campaign is measured against:

**A multi-day hike. The map stays open at an economical rung. The phone sends
position while the rider walks and stops sending when they stop. The map redraws
only when the position actually moved. The rider glances at it now and then.**

So the day splits into two states, and the split is what decides the budget:

| | Hours over 3 days | What runs |
|---|---|---|
| **Walking** | ~24 h (8 h/day) | link up, fixes arriving, marker moves, occasional viewport reset |
| **Stopped** (camp, breaks, sleep) | ~48 h | link up but silent -- `SendPolicy.MOVE_THRESHOLD_M` = 50 m means a stationary phone sends nothing but the hourly keepalive |

Budget, on 650 mAh spec at 85 % usable (**552 mAh**, and both of those numbers
are assumptions, not measurements): **7.67 mA average**. Then:

| Scenario | Walking | Stopped | Needs for 72 h | Lasts |
|---|---|---|---|---|
| Today, measured | 18.7 | 7.7 | 818 mAh | **48.6 h (2.0 d)** |
| Light sleep at camp only | 18.7 | 3.3 | 607 mAh | 65.5 h (2.7 d) |
| Light sleep in **both** states | 9.0 | 3.3 | 374 mAh | **106 h (4.4 d)** |

(Walking and stopped figures for row 1 are run 3's two measured legs; rows 2-3
use S2's predicted 2.3 mA plus a 1 mA board floor **[assumed]** and, for row 3, a
walking figure light sleep would have to deliver.)

**The conclusion is sharper than the old "factor of 5" framing.** Today's device
does **two days**, not one fifth of three. And light sleep *only while parked*
still does not reach three days -- 2.7 -- because 24 walking hours at 18.7 mA is
449 mAh, more than 80 % of the whole budget on its own. **So the 72 hours is
decided by whether light sleep works with the link up and fixes arriving**, not
by whether it works when nothing is happening. That is precisely experiment 3's
question, which makes it the single most valuable run in the campaign rather than
merely the first one.

**What the rung choice is worth, quantified.** The maintainer's instinct was that
a far rung is the economical one. Half right, and the ladder says which half
(`src/activities/map/MapViewport.h:118-127`, the `minMove` column) **[repo]**:

| Rung | m/px | `minMove` px | Metres per marker redraw |
|---|---|---|---|
| 0 | 1 | 12 | 12 m |
| 2 | 6 | 8 | 48 m |
| 3 | 12 | 8 | 96 m |
| 4 | 20 | 6 | 120 m |
| 6 | 45 | 2 | 90 m |

Marker redraws **saturate around rung 3-4** -- `minMove` shrinks as the rung
widens, so zooming further out past 12 m/px buys nothing there. And the phone
does not send under 50 m anyway, so at close rungs the phone's gate decides and
at far rungs the device's does. Where a far rung really pays is the **viewport
reset**: the marker only recentres when it reaches `kKeepInMarginPx` = 80 px of
the edge (`MapFollow.h:34`), which is roughly 3 km of walking at rung 4 and 7 km
at rung 6, against a few hundred metres at rung 0. A reset is the expensive one
-- tile reads off SD and `kickFullClock()` to 160 MHz -- so its rate is the term
a rung choice actually moves. **What one reset costs is [open]**, and it is
cheap to measure now that the scoreboard has a baseline to difference against.

A marker window, by contrast, is **~0.5 s of panel time** (run 3: 57 s/h across
113 refreshes/h **[measured]**), so 33 of them an hour is a 0.5 % duty cycle. The
panel current during a refresh is **[open]**, but for this to matter at all it
would have to be enormous.

## Status, 2026-08-16

- **First optimisation measured, and it works.** BLE modem sleep cuts the draw
  **33 % at matched voltage** (35.6 -> 24.0 mA) while the device did roughly
  twice the panel work. Run 2 below.
- The radio was never sleeping before this -- `CONFIG_BT_CTRL_MODEM_SLEEP` was
  simply off. That is now the single biggest confirmed win of the campaign.
- **The CPU is untouched and is now the largest remaining component**:
  `throttled_ms` was 0.02 % of run 2, so 160 MHz for the whole day.
- The mechanism behind both throttle-split hangs is understood (APB follows the
  CPU below 80 MHz; the controller's PM lock compiles out). An 80 MHz floor is
  safe by construction and is the next thing to build.
- 24.0 mA against a 9.0 mA budget: **2.7x still to find.**

## Status, 2026-08-15

- **First long endurance run done** (run 1 below): 11.5 h, map up, hike mode,
  rung 0, BLE linked the whole time, mostly stationary. 100 % -> 21 %.
  **~45 mA average**, which extrapolates to ~14.6 h on a full charge.
- That agrees with the earlier contaminated ~46 mA ride figure. Two independent
  arrivals at the same number; it is now the working baseline for state 3.
- The three-day target above is set against it. Routes and estimates below.
- **`power.csv` has been read off the device** (same evening, over the web
  server's `GET /download` in AP mode -- see "Getting the file off" below). The
  instrument works end to end. Phase 1's oldest open item is closed.
- The CSV **confirms the read-off-the-code split**: 98.5 % of the run at
  160 MHz, real work 2.2 % of wall, zero full panel refreshes. See "Where the
  45 mA goes".
- **The 10 MHz open question is answered, from data already on the card**: an
  earlier boot held a connected BLE link for ~7.8 h at `cpu_mhz=10`. Route A's
  throttle split is unblocked.
  **Refuted 2026-08-16, and this bullet is why two builds hung the X4 solid.**
  That boot was a device *believing* it was connected, not a live link. See the
  open questions below and `power-management.md`, "A connected BLE link does NOT
  survive 10 MHz".
- Because of that, **the throttle split is the first change to test**, not
  modem sleep. Ordering changed the same day, on the data.
- Nothing has been changed to save power yet. That is still deliberate.

## Getting the file off the device

`power.csv` lives at `/trailink/power.csv` on the card. Two ways off:

1. **Pull the card.** What the methodology below assumes. No network.
2. **Over the web server**, which is what was actually used on 2026-08-15:

   ```
   curl -OJ "http://<device-ip>/download?path=/trailink/power.csv"
   ```

   `GET /download` is a normal endpoint (`docs/webserver-endpoints.md:131`).
   Note that WiFi and the map do not run at the same time on X4, so this
   happens after a run, never during one.

**Guest and hotel networks usually block it.** Client isolation stops two
devices on the same AP from seeing each other; the symptom is a failed ARP for
the device while the gateway still pings. Fix by putting the device in AP mode
(open hotspot `CrossPoint-Reader`, `192.168.4.1`) and joining that, or by
putting both the laptop and the device on a phone hotspot -- the second keeps
the laptop's own internet, which matters if a tool on the laptop needs it.

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

## Where the 45 mA goes -- measured 2026-08-15

**Confidence: measured.** This section was written the same day as a read-off-
the-code hypothesis. Run 1's `power.csv` was then pulled off the device and
**confirmed it**. The counters below are deltas across the 11.42 h run window;
the code citations are kept because they say *why*, but the numbers are no
longer inferred.

| Counter | Over the run | What it says |
|---|---|---|
| `throttled_ms` | **1.55 % of wall** | The CPU held 160 MHz for **98.5 %** of the run. Per-row census: 766 rows at 160 MHz, 11 at 10 MHz. |
| `loops` | 97.4 Hz | The loop runs at ~100 Hz, as the code reading said. |
| `loop_busy_ms` | **1.34 % of wall** | The loop does real work 1.3 % of the time. |
| `panel_busy_ms` | 0.89 % of wall | 62 refreshes/h, of which `ref_full` = **0**. Almost all `ref_window`. |
| `ble` | 2 (connected) in 765 of 777 rows | The link held all day. |

**Real work took ~2.2 % of the day.** The other ~98 % was spent in `delay(10)`
at 160 MHz doing nothing, plus the radio. The panel is as cheap as predicted --
not one full refresh in 13 hours.

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

**Steps 1-3 stopped describing the code on 2026-08-17.** The bool is split, the
map screen does throttle, and the throttled alternative is **80 MHz** while the
BLE controller is enabled -- not the 10 MHz named above
(`lib/hal/HalPowerManager.h`, `BLE_SAFE_FREQ`; route A below). The
decomposition is kept as what run 1 measured, not as a reading of today's
firmware.

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
the map can leave the pinned 160 MHz while it is only waiting for a fix, and set
`CONFIG_BT_CTRL_MODEM_SLEEP=y`.

- Estimate: 44 mA -> 15-25 mA, so **26-43 h**. Written against a 10 MHz
  throttle, so read it as an upper bound on the saving -- the floor is 80.
- **The floor is 80 MHz, not 10 -- corrected 2026-08-16.** Two builds that
  throttled to 10 MHz with the controller up hung the device solid. Below
  80 MHz the CPU leaves the PLL for the crystal and APB follows it down; the
  controller's `ESP_PM_APB_FREQ_MAX` defence is compiled out because
  `CONFIG_PM_ENABLE` is not set (`power-management.md`, "Why 10 MHz breaks
  BLE"). The floor is enforced inside `HalPowerManager::lowPowerFloorMhz()`
  from `esp_bt_controller_get_status()`, not by a guard at the call site.
- **Shipped 2026-08-17 and bench-verified**: the throttle engages with a central
  connected, the link holds, `throttled_ms` 93.8 % of wall against run 2's
  0.02 %. Draw not yet measured -- prediction to refute is 24.0 -> 14-19 mA.
- The measurement is what promoted it. 98.5 % of run 1 sat at 160 MHz and only
  1.3 % of that did any work, so the throttle is the largest single lever in
  the tree, and it is no longer a guess about where the milliamps are.
- **It cannot reach 9 mA, and the 80 MHz floor is why.** The CPU does not stop,
  it halves, and it may not go lower for as long as the radio is up. Anything
  below the floor needs route B (which compiles the APB locks back in) or the
  radio switched off entirely. There is no gradual clock ramp between the two.
- Two things the implementation must get right, both from run 1's counters:
  - **Release the throttle around drawing.** A viewport reset is ~2 s at
    160 MHz and roughly doubles at 80 (arithmetic, not measured). Run 1 drew 62
    times an hour, so drawing runs at full clock --
    `MapActivity::kickFullClock()`.
  - **`preventAutoSleep()` must stay true while `preventThrottle()` goes
    false.** The split releases the clock, not the sleep guard.

### B. Automatic light sleep

`CONFIG_PM_ENABLE=y` plus tickless idle, so the device sleeps between BLE
connection events and wakes for each one.

- Estimate at 5 % duty: 35 mA awake + ~1 mA asleep = **~2.7 mA**, an order of
  magnitude past the target.
- **This is where three days actually lives.**
- **And it is the only legal way under 80 MHz while the radio is up.** Every
  `ESP_PM_APB_FREQ_MAX` lock site in the BLE controller sits inside
  `#ifdef CONFIG_PM_ENABLE` (`power-management.md`, "Why 10 MHz breaks BLE"), so
  this flag is not merely what enables light sleep -- it is what makes any
  sub-80 MHz state with a live controller supported at all. Route A's floor is a
  consequence of not having it.
- Riskiest item in the campaign: APB frequency moves under drivers that may not
  take PM locks -- SPI panel, ADC, USB CDC.
- **The flag list is longer than this line says, and one missing option makes
  the whole route a no-op.** With the low-power clock on the main crystal the
  controller holds an `ESP_PM_NO_LIGHT_SLEEP` lock for as long as Bluetooth is
  enabled unless `CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP` is set too --
  so PM_ENABLE plus tickless idle alone would compile, boot, hold the link and
  never light-sleep (`power-management.md`, "`CONFIG_PM_ENABLE` alone saves
  nothing while the radio is up"; full set in `power-idle-sleep.md`). Found
  2026-08-19 by reading the pinned IDF, not by burning a run on it.
- Constrained by the clock source, and **the X4's choice is already made for it.**
  Espressif's own measurement of a C3 BLE peripheral: light sleep is **2.3 mA** on
  the main crystal and **140 uA** on an external 32.768 kHz crystal
  (`power-management.md`, "What a C3 actually draws asleep") -- 16x. But the X4
  **cannot carry that crystal**: on C3 it can only be soldered across GPIO0/GPIO1,
  and those two pins are the button ADC ladder and the battery divider
  (`power-management.md`, "The X4 cannot have a 32.768 kHz crystal"). So route B on
  X4 lands on the 2.3 mA column plus the board's own floor, and the 140 uA column is
  a requirement for a future board. One caveat kept honest: that ceiling is for a
  build that holds connections. The internal 136 kHz RC needs no crystal and no pins
  and is forbidden only for the connection state, so a parked advertising-only build
  is unpriced -- experiment 6 in `power-idle-sleep.md`.
  The internal 136 kHz RC is not an option either -- its accuracy is "a lot larger
  than 500ppm which is required in Bluetooth communication" (IDF Kconfig), i.e. it
  cannot hold a connection.

### C. Duty-cycle the whole link

> **Superseded 2026-08-19, on arithmetic** -- see
> [`power-idle-sleep.md`](power-idle-sleep.md), "S3 -- rejected". Every wake is a
> full boot, so at any cadence that still resumes without the rider touching the
> device this averages roughly what route B costs, while adding a lossy wake
> channel, lost RAM per wake and appear/disappear churn on the phone. The case it
> was invented for -- parked with no link -- is covered by route B instead: an
> advertising peripheral light-sleeps between advertising events, so the
> advertisement can stay on the air continuously at single-milliamp cost. Deep
> sleep keeps the role it already has: the deliberate power-off.

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

## The frozen baseline: this campaign flashes a stale branch on purpose

**Exemption granted by the maintainer 2026-08-19.** The parent repo's `CLAUDE.md`
says: only flash a rebased branch, re-check whether `develop` moved right before
every upload, and rebase if it did. **The power campaign is exempt from that**, and
its measurement branch stays pinned to one base commit for the whole series.

**Why.** If `develop` moves between run 1 and run 5, a difference in draw can be
another session's commit rather than the option under test. That is the same
failure this campaign already walked into once, and no amount of care inside a run
fixes it afterwards. A comparison is only worth making against a fixed baseline.

**Why this is not "the rule was wrong".** The two rules protect different things.
The rebase rule exists so a flash cannot put firmware on the device that silently
lacks another session's fix, leaving someone reasoning about a device that is not
what they think it is. That risk is about the device's state being misleading. The
freeze is about numbers being comparable. Neither substitutes for the other, so the
exemption is not "staleness is fine here" -- it is **"staleness is the design, and
it must be declared rather than accidental"**.

Four things pay for it. Skipping any one of them turns the exemption into the
problem it was meant to avoid.

1. **The baseline lives in the data, not in memory.** Every `power.csv` row already
   carries `build` = `TRAILINK_VERSION`, added 2026-08-16 because 61 boots of mixed
   firmware were indistinguishable. The measurement branch sets a distinctive
   string -- `powerlab-<base-hash>-<n>` -- so every row identifies its own baseline
   and two runs months apart can be told apart or matched.
2. **Archive the binary, not just the branch.** `docs/firmware-builds/` (gitignored,
   see its README). The strongest form of a freeze is not "the branch has not
   moved", it is **"this exact binary is on disk and can be reflashed"**. When
   another session flashes the device in between, that restores the identical
   baseline with no rebuild to trust.
3. **A tripwire on `develop`.** If something lands there that touches the
   power-relevant surface -- BLE connection parameters, the map loop, refresh
   cadence, `PowerLog` -- the frozen base stops being conservative and starts being
   misleading: the work would be optimising a path that no longer exists. Then
   rebase deliberately and **restart the series**, rather than mixing bases.
4. **One validation run on rebased code before anything changes the product.** A
   finding from the frozen baseline is a finding about the frozen baseline until it
   has been seen on current `develop` once.

**Two classes of build, and only the first is really frozen.**

| Class | Examples | Build |
|---|---|---|
| Runtime states | idle, advertising, connected, light sleep, deep sleep + timer | **one** binary, bit-identical across the whole comparison, zero rebuilds |
| Compile-time options | `CONFIG_PM_ENABLE`, the main-XTAL PU flag, `CONFIG_ESP_PHY_MAC_BB_PD` | one binary per option, all from the **same frozen base**, differing only in that option |

The first class is what the power lab screen exists for
([`power-idle-sleep.md`](power-idle-sleep.md), "The power lab screen"): selecting
the state at runtime is what makes the binary identical, and an identical binary is
a stronger comparison than this campaign has ever managed. The second class cannot
be runtime-selected -- sdkconfig is compile time -- so there the old rule still
applies in full: **one option per build, per run.** The exemption changes only
which base they are cut from.

**What the exemption does not solve.** The device is shared. Another session
flashing its own branch between two runs ends the freeze whatever this file says.
That is the X4 lock's job, plus telling the human a series is in progress -- and
rider 2 above, so the baseline can be put back.

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
| 1 | Home screen, untouched | the idle floor, including the 10 MHz throttle actually engaging -- no BLE controller up, so this is the one state where the floor really is 10 |
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

### How long a run has to be, derived from the noise instead of guessed

**Re-analysis of run 1 and run 2, 2026-08-21, laptop only.** The tool is
`tools/powercsv.py` in the parent repo: it splits a downloaded `power.csv` on
boot, build, radio state, lost-row gap and voltage rise, fits each segment and
reports the fit's own uncertainty.

The row-to-row noise is **small**. Median residual after a linear fit is
**1.0 mV** across run 2's segments, against a 1 mV ADC step; run 1's median is
4.2 mV, and run 1 is the contaminated one (a car drive, panel activity swinging
by 4x between segments). So the instrument is not what limits a short run.

For rows at a fixed interval, the OLS slope uncertainty is
`sigma * sqrt(12 * interval / T) / T`, so the run length needed to resolve a
slope difference `d` at two sigma is `T = (2 * sigma * sqrt(12 * interval) / d)^(2/3)`.
At run 2's 1.0 mV and 60 s rows:

| Difference to resolve | Run length |
|---|---|
| 20 mV/h | 0.1 h |
| 10 mV/h | 0.2 h |
| 5 mV/h | 0.3 h |
| 2 mV/h | 0.6 h |
| 1 mV/h | 0.9 h |

Run 2 measured 32.6 mV/h at a nominal 24 mA, so 5 mV/h is roughly 3.7 mA, and
the table above suggests a 30-minute leg prices a 4 mA change.

> **Refuted the same day, 2026-08-21, by trying it.** The formula above is right
> about the *residual* and wrong about the *instrument*, because it assumes the
> only error is Gaussian noise. It is not: the ADC step is **1 mV**, one row a
> minute, so what a short leg actually resolves is set by how many whole counts
> the voltage moves. Four afternoon legs in the 4046-4042 mV plateau moved
> **1-2 mV in 18-43 minutes**, and came out ordered impossibly -- connected with
> a fix every 10 s read *lower* than advertising. The differences were counting
> noise.
>
> The honest table, at 1 mV per count and asking for five counts of movement:
>
> | Difference to see | Leg length |
> |---|---|
> | 8 mA | 27 min |
> | 4 mA | 55 min |
> | 2 mA | 109 min |
> | 1 mA | 219 min |
>
> **And the required length grows as the draw falls**, because a cheap state
> moves the voltage slowly -- exactly backwards from what the campaign needs, since
> every remaining question is about states drawing under 10 mA. See "The plateau
> problem" below.

### Voltage slope is not a property of the state -- it is a property of the charge

**The reason every comparison so far was shaky.** Same build, same state, one
run: the slope moves 5x as the pack empties.

| Start voltage | Slope |
|---|---|
| 3.90 V | 25.6 mV/h |
| 3.69 V | 109.1 mV/h |
| 3.43 V | 138.1 mV/h |

(run 1, `docs/power-runs/run1-2026-08-15.csv`, segments read by
`tools/powercsv.py` **[measured]**.) The discharge curve is flat in the middle
and steep at the ends, so mV/h says as much about where the pack is as about
what the firmware did. Constraint 4 asked for comparable start voltages; this
is the quantity that makes it non-negotiable.

Two consequences for the run shape:

- **Compare states as A-B-A, not A then B.** Three short legs, the second
  state bracketed by the first, cancels the drift along the curve. Two legs
  at different voltages do not, whatever their length.
- **The first half hour after unplugging is not a measurement.** Right off the
  charger the apparent slope is **128-160 mV/h even at throttled idle with the
  panel doing nothing** (run 1 segment at 4217 mV, run 2 segment at 4210 mV),
  which is surface-charge relaxation and not draw. The methodology's "rest
  ~10 minutes" is too short; discard 30 minutes. **Every mode change that reboots
  the device does the same thing on a smaller scale**: removing the load lets the
  pack recover, and 2026-08-21's post-reboot leg read 32.6 mV/h against 5.3 for
  the same state twenty minutes later.
- **Drop the boundary row.** A row written across a mode change carries the new
  mode's load, not the leg's. One such row turned a 2 mV drift into an apparent
  23 mV collapse on 2026-08-21's Home leg, which is a 10x error from a single
  sample.

### The plateau problem, and why the meter stopped being optional

**Established 2026-08-21.** Two facts, together, bound what this instrument can
ever do:

1. **The curve, not the load, sets the slope.** The same firmware in the same
   state read **25.6 mV/h at 4093-4068 mV** and **3.1 mV/h at 4046-4044 mV**, 90
   minutes apart. Both are correct readings of `batt_mv`; neither is a reading of
   power. Inside the plateau the pack barely moves whatever the device does.
2. **A slope only converts to mA through the local dV/dQ**, and nobody has
   measured that curve. Every mA in this campaign is scaled from run 2's single
   pair (32.9 mV/h against 24.0 mA) as if the relation were global. Fact 1 says
   it is not.

So the mV slope works for **long runs across a wide band** -- run 1 and run 2
traverse enough of the curve to average it, which is why their numbers held up --
and it does **not** work for comparing two states inside the plateau, which is
every question the campaign has left.

Two ways out, and only one is cheap:

- **Rapid alternation.** Switch A/B/A/B every 25-30 minutes for many cycles
  rather than running two long legs. The curve term is smooth and slow; the state
  term flips with a known schedule, so differencing across many alternations
  cancels the drift even though no single leg resolves anything. This is what the
  night run should do, and it is a scheduling change, not a purchase.
- **A meter.** `power-test-runbook.md`, "The instrument problem", treats a
  PPK2-class source-meter as the thing that makes experiment 1 possible. It is
  now more than that: it is the only way to price **any** sub-10-mA state
  directly, and every such state is what is left. The purchase moved from
  nice-to-have to the campaign's blocker, and the price still has to be read off
  a distributor page before anyone buys.

## TODO

**Bench rig, added 2026-08-16.** Any change that touches power or the radio is
proved on the bench before it runs unattended, with a **control run on the last
known-good build using the same rig**. Two builds hung the device that day and
only the control run made the cause unambiguous.

```
# reset, enter the map inside the full-clock window, then:
python3 tools/blefakephone.py --pos <lat> <lon> --heading 4 --no-tiles --no-serial
```

Pass: fixes keep arriving for minutes, the device still logs after the central
leaves. Fail: the link dies after the first packet, or serial goes silent and
stays silent past the 20 s supervision timeout.

Note the device only accepts `CMD:` for about 3 seconds after a reset -- once it
throttles to 10 MHz on any screen, serial RX is starved and nothing gets in. So
`CMD:GOTO_MAP` has to be sent in the boot window.

**Working rule, agreed 2026-08-15: one option at a time, tested hard before the
next.** Each item in phase 3 is a separate branch, a separate build and a
separate run of at least 2 hours in state 3. Two changes in one build cannot be
told apart afterwards, and this campaign has one instrument and one device.

Phase 1 -- make the instruments trustworthy:

- [x] Flash the telemetry build (2026-08-11, `f6372ea6`).
- [x] Confirm `stats` answers over a real BLE link.
- [x] Confirm `throttled_ms` stops rising on the map screen and rises on Home
      -- the code reading (`power-management.md`, item 3) checked on hardware.
- [x] Read a real `power.csv` off the card and check the rows are sane
      (2026-08-15, over `GET /download` in AP mode). 4028 rows across 61 boots;
      run 1 is the second-to-last boot. Rows are sane and the decomposition is
      now measured.
- [x] **Add the build to `power.csv`** (2026-08-16). Every row now carries
      `build` = `TRAILINK_VERSION` as the last column, and the header line is
      written **once per boot** rather than once per file. The header doubles as
      the boot marker, and it keeps a file readable across a column change
      because each boot's rows carry their own column list. Readers must skip
      any line starting with `uptime_s`. Built clean; **not yet on hardware**.
- [ ] Teach the analysis to reject contaminated stretches automatically. Run 1's
      file contains charging jumps mid-boot; a naive first-row-to-last-row slope
      over such a boot reports a draw that never happened. Cut on any rise
      above ADC noise before computing anything.
- [ ] Cross-check one `stats` reply against the CSV row written beside it.
- [ ] Chase `rssi()` returning 0 on a live link, or drop the line from `stats`.
- [ ] Record run 1 (state 1, 2 hours) and run 2 (state 2, 2 hours).

Phase 2 -- baseline the four states:

- [x] State 3 (map, phone connected): run 1, 2026-08-15, ~45 mA from percent.
      Coarse -- redo the arithmetic from `batt_mv` once the card is read.
- [ ] Runs for states 1, 2 and 4. State 2 (map, no phone) minus state 3 is the
      only clean way to price the radio.
- [x] **Answer the crystal question** (2026-08-19). The X4 cannot have a working
      external 32.768 kHz crystal: on C3 it is fixable only to GPIO0/GPIO1, and
      those carry the button ADC ladder and the battery divider. No flash, no
      meter -- decided from Espressif's register header and our own driver
      (`power-management.md`, "The X4 cannot have a 32.768 kHz crystal").
- [ ] Write all four voltage slopes into `power-management.md`, with the
      caveats each one carries.
- [ ] One inline-meter cross-check, documented as system-plus-charging.

Phase 3 -- one change per branch, per build, per run. Re-run state 3 after each
and record it in the table below before starting the next.

Route A -- **start here**. Ordered by measured lever size, biggest first:

- [x] **Split `preventAutoSleep()` into `preventAutoSleep()` +
      `preventThrottle()`, with an 80 MHz floor** (2026-08-17). Two earlier
      attempts hung the device by throttling to 10 MHz with the controller up;
      the fix is a floor enforced inside `HalPowerManager` from
      `esp_bt_controller_get_status()`, not a guard at the call site.
      Bench-verified: throttle engages with a central connected, link holds
      (44/45/33 fixes across three runs), `throttled_ms` **93.8 % of wall**
      against run 2's 0.02 %. Mechanism in `power-management.md`, "The map
      throttles to 80 MHz".
      **Prediction for the next run, to be refuted: 24.0 mA -> 14-19 mA.**
      That would be ~34-46 h, still short of the 9.0 mA three-day budget.
- [ ] Loop cadence: let the map take the 50 ms delay while it is only waiting
      for a fix. Small next to the throttle -- the loop only works 1.3 % of the
      time -- but nearly free.
- [ ] `CONFIG_BT_CTRL_MODEM_SLEEP=y` plus a mode/clock selection. Without an
      external 32.768 kHz crystal the low-power clock stays `MAIN_XTAL`, so the
      main crystal keeps running through modem sleep and the saving is smaller
      than the flag suggests.
- [ ] Advertising interval and TX power set explicitly in
      `BlePositionServer::begin()`. Affects state 2 most.

Route B (only if route A lands short of 9 mA):

- [x] **Crystal question settled 2026-08-19: no, and not fixable on X4.** GPIO1
      carries the button ladder and GPIO0 the battery divider, which on C3 are
      exactly the two crystal pins.
      The boot-log test is no longer needed; if it is ever run on another board,
      the controller prints `32.768kHz XTAL not detected, fall back to main XTAL
      as Bluetooth sleep clock` when there is none.
- [ ] Price the **board's own floor** -- deep sleep with the battery latch held
      HIGH instead of cut, measured with a uA meter in series with the battery.
      X4 has no switched rails, so SD, the divider, the regulator and the panel
      controller stay powered in every latched sleep state and nobody knows what
      that costs (`power-management.md`, "The board's own floor is unpriced").
      **Do this before the crystal work**: a floor of 1 mA or more spends most of
      the crystal's 16x before it is earned.
- [ ] `CONFIG_PM_ENABLE=y` with DFS and tickless idle, **and the full seven-option
      set** including `CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y` and
      `CONFIG_ESP_PHY_MAC_BB_PD=y` (`power-idle-sleep.md`). **Riskiest** -- APB
      frequency moves under drivers that may not take PM locks (SPI panel,
      ADC, USB CDC). Expect to find a driver that does not. Note USB serial dies
      when light sleep engages, so evidence comes from `power.csv` and BLE.
- [ ] The **power lab screen** -- a build-flagged activity that enters one power
      state deliberately, so states are selected at runtime and the binary is
      identical across a comparison (`power-idle-sleep.md`, "The power lab
      screen"). Not a measurement itself; the thing that makes the measurements
      comparable.
- [ ] Long run: 12 h minimum, then a real 72 h run before any public claim.

Route C (**superseded 2026-08-19** -- kept because the reconnect-cost item below
is still the right question if any duty-cycled variant is ever revived):

- [ ] Measure what one disconnect-sleep-reconnect cycle costs before building
      anything. If reconnect is expensive the whole route changes shape.
- [ ] Phone-side half: the app has to tolerate a peripheral that vanishes on
      purpose and comes back a minute later, without treating it as a fault.
- [ ] Wake-and-redraw path: every wake is a boot, so the map must come back
      from the tile cache fast enough to be worth it.

Phase 4 -- tune what the measurements expose:

- [ ] `MapFollow::kMaxPartialMoves` against real ghosting, now that refresh
      counts are visible per ride. **Sharpened 2026-08-16**: a windowed update
      costs the same panel time as a fast full refresh (~508 vs ~490 ms), so
      the target is the **count** of refreshes, not their size
      (`power-management.md`, "A windowed update costs the same panel time").
      "Many small partial moves rather than one reset" saves nothing. Run 2
      spent 119 s of panel time an hour, 95 % of it on marker moves.
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
  **Promoted 2026-08-16**: both 10 MHz attempts hung the device with a central
  connected, so this is no longer the cautious alternative -- it is the only
  remaining version of route A. Needs a per-caller frequency in
  `HalPowerManager` rather than the one `LOW_POWER_FREQ` constant.
- Ask the phone for a slower connection interval between fixes and a fast one
  only during a transfer. The central owns the interval, so this is
  phone-side work (`BlePositionServer::connIntervalUnits()` is how the device
  sees what it got).
- ~~Log a marker row into `power.csv` when a run starts~~ -- done differently
  2026-08-16: the header line is now written once per boot, which is the marker.

## Open questions

- ~~**Does an established NimBLE link survive a drop to 10 MHz?**~~
  **ANSWERED 2026-08-16: no.** This file answered "yes" on 2026-08-15 and was
  wrong, and the wrong answer is what justified two flashes that both hung the
  device solid. The correction is kept here rather than deleted, because the bad
  inference is the lesson.
  **Bench-measured with a control run**, same rig: the throttling build took one
  fix and went to total serial silence, never recovering; the control build held
  the link for 22 fixes over two minutes (`power-management.md`, "A connected BLE
  link does NOT survive 10 MHz").
  The refuted evidence was `power.csv` showing 466 of 513 rows with `cpu_mhz=10`
  and `ble=2` together. `ble=2` only means `connIntervalUnits_` is non-zero
  (`src/PowerLog.cpp:25-27`), and that field is cleared in
  `onCentralDisconnect()` (`lib/BlePositionServer/src/BlePositionServer.cpp:722`)
  -- which never runs if the link dies in a way NimBLE's disconnect callback does
  not service. So the rows say the device *believed* a central was there, and
  nothing more. **A counter derived from a cached field is only as good as the
  path that clears it**; a belief like that needs a second, independent signal --
  traffic arriving.
  Standing rule out of it: **never take the CPU below 80 MHz while the BT
  controller is enabled.** The separately verified `NimBLEDevice::init()` hang is
  the same violation, met earlier only because init is the first register
  conversation with the BT MAC.
- **What does the board draw with the battery latch held closed? BLOCKING, and
  ahead of the crystal.** X4 has no switched peripheral rails, so every sleep
  state that does not cut the latch keeps the SD card, the battery divider, the
  regulator and the panel controller powered (`power-management.md`, "The board's
  own floor is unpriced"). If that floor is 1 mA or more it bounds every deep
  state and most of what the crystal could buy. Needs a uA meter in series with
  the battery; `power.csv` cannot see microamps and a USB meter charges the cell.
- ~~**Does the X4 have an external 32.768 kHz crystal? BLOCKING.**~~
  **ANSWERED 2026-08-19: no, and it cannot.** On C3 the crystal is fixable only
  across GPIO0/GPIO1 (Espressif's own IO-MUX header) and GPIO1 is the X4's button
  ADC ladder, which works on hardware every day. Full argument, the one loophole
  it leaves, and what it means for a future board:
  `power-management.md`, "The X4 cannot have a 32.768 kHz crystal".
  It was blocking from 2026-08-15 because it gates route B and sizes the prize --
  16x on the parked floor. The prize on **this** board is therefore the 2.3 mA
  column, not 140 uA, and route B is still worth building for it: 24 mA today. The
  sub-milliamp door is not fully shut, only the crystal one: the internal 136 kHz RC
  is legal for advertising and unpriced (experiment 6).
  Cost of the answer: no flash, no meter, one read of a register header. The
  earlier plan here was to infer it from a boot log; the pin map is stronger,
  because a pin that already does something else cannot also hold a crystal.
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

Newest last. One row per run, with the file kept alongside. **What each mode
draws, and what each feature costs, is one table in
[`power-management.md`](power-management.md), "The scoreboard"** -- this table is
runs, that one is numbers.

| Date | Build | State | Duration | Start mV | End mV | Slope mV/h | Notes |
|---|---|---|---|---|---|---|---|
| 2026-08-15 | unrecorded | 3 | 11 h 25 min | 4220 | 3547 | **58.9** | Run 1, 99->21 %. See below. |
| 2026-08-16 | `9686ce21` | 3 | 13 h 12 min | 4178 | 3748 | **32.6** | Run 2, first optimisation measured: BLE modem sleep. See below. |
| 2026-08-21 | `55c9ed26` | 2 (adv, 80 MHz) | 60 min | 4159 | 4093 | **70.8** | Run 3 leg 1. **Discard** -- inside the relaxation window. See below. |
| 2026-08-21 | `55c9ed26` | 3 (80 MHz) | 61 min | 4093 | 4068 | **25.7** | Run 3 leg 2, fix every 10 s, panel 57 s/h. |
| 2026-08-21 | `55c9ed26` | 2 (adv, 80 MHz) | 33 min | 4068 | 4064 | **10.6** | Run 3 leg 3. Same state as leg 1, 6.7x lower. |

### Run 3 -- 2026-08-21, three legs on one boot, and what the first one cost us

Evidence: `docs/power-runs/run3-2026-08-21.csv` (parent repo), read with
`tools/powercsv.py`. Build `0.1.0-dev-wallet-viewer-55c9ed26` -- whatever was on
the device, **not** a frozen-baseline build; this run was made with the firmware
already flashed rather than by flashing one, which is why it has no `powerlab-`
version string and why the state column is absent from its rows.

One boot, four phases, timeline confirmed to the minute against the operator's
own note of entering the map at 10:31:

| Uptime | Wall clock | State | CPU | mV | Slope |
|---|---|---|---|---|---|
| 2-722 s | 10:19-10:31 | Home, radio down | 10 MHz | 4177-4159 | ~90 |
| 722-4324 s | 10:31-11:31 | advertising | 80 MHz | 4159-4093 | **70.8 +/- 2.5** |
| 4324-7986 s | 11:31-12:32 | connected, fix/10 s | 80 MHz | 4093-4068 | **25.7 +/- 3.5** |
| 7986-9968 s | 12:32-13:05 | advertising | 80 MHz | 4068-4064 | **10.6 +/- 1.1** |

**The map does not pin 160 MHz any more.** Every map phase above ran at 80 MHz
with `throttled_ms` at 100 % of the interval and the loop at **20 Hz**, against
run 2's 160 MHz and ~98 Hz. That is the two-deadline split working as
designed, built and bench-verified 2026-08-17 (`power-management.md`, "The map
throttles to 80 MHz"). What that bench proved was **functional** -- the link
survives the throttle, the fixes arrive. Nobody had put a number on what it
saves, which is T-201. **This run is T-201's answer**, for two states at once:

- **Advertising at the 80 MHz `BLE_SAFE_FREQ` floor: 10.6 +/- 1.1 mV/h.**
- **Connected, fix every 10 s, at 80 MHz: 25.7 +/- 3.5 mV/h**, against run 2's
  32.6 mV/h for the same state at 160 MHz -- though across different builds and
  different panel activity, so that pair is suggestive and not a measurement of
  the split.

Scaling run 2's calibration (32.6 mV/h at a nominal 24.0 mA, assumed
proportional **[assumed]**), the advertising figure is roughly **7.8 mA** --
below the campaign's 9 mA target, for the parked case, on hardware that already
shipped. It wants a repeat before it is believed, and the repeat has to sit in
the working band rather than an hour after a full charge.

**How far it never goes up.** `full_clock_ms` deltas over the same phases say
the map does not drift back to 160 MHz at all: **0.26 %** of the advertising
hour, **0.00 %** of the connected hour, with 47-50 windowed marker refreshes in
it. Only a heavy frame lifts the clock -- `kickFullClock()` sits at the top of
`renderViewport()`, `renderCurrent()`, `renderWaiting()`,
`renderLoadingTiles()`, `renderRouteOverview()` and `showBusy()`
(`src/activities/map/MapActivity.cpp:692,3771,3794,3831,4095,4342`) -- and a
marker move is a windowed update, which calls none of them. Radio traffic on its
own never lifts it: the wake check reads buttons, touch and tilt only
(`src/main.cpp:833-838`) **[measured]**.

**The same state measured 70.8 and 10.6 mV/h.** Identical counters (loop busy
2.0 %, panel 0 ms/h, 20 Hz, `throttled_ms` 100 %), identical build, 6.7x apart.
Nothing differs but time-since-unplug and state of charge. So the 30-minute
discard this file recommended a few hours earlier is far too short: **the first
hour off a full charge is not a measurement**, and part of the effect is the
genuine steepness of the curve above 4.10 V rather than relaxation alone. The
working band is roughly **4.05 V down to 3.80 V**.

**What the run does not answer.** The link's cost reads as 25.7 minus 10.6, so
up to ~15 mV/h -- but that is an upper bound, not a measurement: the connected
leg sat at a higher voltage and earlier in the relaxation than the advertising
leg it is being differenced against. Cancelling exactly that is what the closing
leg of an A-B-A is for, and this run lost it: the fake phone was stopped with
`timeout`, SIGTERM killed python before asyncio unwound the BleakClient, and
**the LE link stayed up in the kernel** -- so the intended third leg ran
connected-and-idle instead of advertising. Fixed in the tool
(`tools/blefakephone.py --duration`), not in the run. Also note "connected" here
is not only radio: the panel did 57 s/h of windowed refreshes for the marker.

### Run 2 -- 2026-08-16, BLE modem sleep, the first measured saving

Build `9686ce21` (`CONFIG_BT_CTRL_MODEM_SLEEP=y`, mode 1, main XTAL). File:
`docs/power-runs/run2-2026-08-16.csv` (parent repo). Day: 100 % at 10:30, small
movement around town and mostly static until 20:00, then a car drive
Prague -> Bratislava until 23:50, ending at 48 %.

**Not a clean repeat of run 1** -- the drive makes the second half a much
heavier workload. That makes the result stronger, not weaker: the device did
more and drew less.

**Like-for-like, the same voltage band both runs covered (4178 -> 3866 mV).**
This is the only honest comparison; whole-run figures flatter run 2 because run
1 continued into the steep tail below 3748 mV.

| | Run 1 (no modem sleep) | Run 2 (modem sleep) |
|---|---|---|
| Time for the same drop | 6.20 h | **9.48 h** |
| Slope | 50.3 mV/h | **32.9 mV/h** |
| Draw at 650 mAh | 35.6 mA | **24.0 mA** |
| Refreshes/h | 65 | 122 |
| `loop_busy_ms` | 1.50 % | 5.81 % |

**Draw down 33 %** while the panel did ~2x the work and the loop ~4x. The
saving at equal workload is therefore larger than 33 %; nothing here separates
the two, and only a static repeat would.

Phases of run 2, from the wall-clock split the rider reported:

| Phase | Duration | Slope | Draw | Refresh/h | loop busy |
|---|---|---|---|---|---|
| Static, 10:30-20:00 | 9.48 h | 32.9 mV/h | 24.0 mA | 122 | 5.81 % |
| Driving, 20:00-23:50 | 3.69 h | 31.7 mV/h | 29.9 mA | 526 | 21.78 % |

A full driving hour now costs less than run 1's static hour did.

**`throttled_ms` was 0.02 % of the run** -- the CPU held 160 MHz throughout, as
expected, since the throttle split is not in this build. So modem sleep is the
only variable between the two runs, and the CPU is still untouched and still
the largest remaining component.

**Against the target:** 24.0 mA gives ~27 h on a charge. The 9.0 mA budget for
three days is still 2.7x away.

### Run 1 -- 2026-08-15, first long endurance run

The first run long enough to mean anything, and the run the three-day target is
set against. File: `docs/power-runs/run1-2026-08-15.csv` (parent repo), the
second-to-last boot in it.

**Conditions.** State 3: map screen up the whole time, BLE connected the whole
time (`ble=2` in 765 of 777 rows), hike mode, rung 0, area Praha. Mostly
stationary.

**The window the device reported, from `batt_mv`:**

| | 99 % -> 21 % | full discharge |
|---|---|---|
| Duration | **11.42 h** | 12.94 h (to 5 %) |
| Voltage | 4220 -> 3547 mV | 4220 -> 3380 mV |
| Slope | **58.9 mV/h** | 64.9 mV/h |
| Drain | 6.83 %/h = **44.4 mA** | 7.27 %/h = 47.2 mA |
| 100 -> 0 extrapolated | **14.6 h** | 13.8 h |

The same day's percent-only arithmetic (100 % at 10:30, 21 % at 22:00, ~45 mA,
~14.6 h) came out right to a decimal. Worth knowing: over a run this long,
percent is not as useless as constraint 3 implies. Over a short one it still is.

**Counters over the run** are in "Where the 45 mA goes" above -- 98.5 % of the
run at 160 MHz, real work 2.2 % of wall, zero full panel refreshes.

Caveats:

- **Build not recorded.** Fix for run 2, and see the phase 1 item about adding
  a build column.
- **The boot began with an hour of charging** (4025 -> 4220 mV) before the run
  proper. The numbers above start at peak voltage, not at boot. Run 2 should
  unplug and rest 10 minutes so the window starts clean.
- **Stationary is optimistic.** A walking hiker triggers viewport resets this
  run did not pay for, so 14.6 h is a ceiling for state 3, not an expectation on
  a trail.
- **Agrees with the earlier ~46 mA ride figure**, which was contaminated. Two
  independent routes to the same number.

**Temperature is an unrecorded confounder for the run 1 / run 2 pair.**
Constraint 5 of this file's own methodology says to record where a run happened,
and this pair does not. Run 1 was a static day; run 2 spent its last 3.7 h in a
car cabin, whose temperature is unknown and need not match. Nothing here
suggests the 33 % is mostly thermal -- the matched-voltage bands agree across
the whole discharge, not only the driving half -- but the figure carries this
caveat until a static repeat exists.
