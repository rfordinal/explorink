# Power test runbook

Step-by-step procedures for the next sessions of the power campaign. This file
says what to do, in what order, with which checklists. It does not repeat the
findings or the design:

- Design (the two states, the lab screen spec, the experiments):
  [`power-idle-sleep.md`](power-idle-sleep.md).
- Campaign (target, constraints, run methodology, the run table):
  [`power-plan.md`](power-plan.md).
- Findings (what is measured, what broke, why):
  [`power-management.md`](power-management.md).

Every run here uses the frozen baseline and its four conditions
(`power-plan.md`, "The frozen baseline"). Experiment order, agreed 2026-08-19:
**3 first, then 1, with 6 riding along on 3's bench**
(`power-idle-sleep.md`, "Experiments").

Confidence marks: **[measured]** on hardware here, **[repo]** read off this
code, **[primary]** vendor page or pinned IDF, **[assumed]** nobody checked.

**And every measurement says which device it was taken on.** Standing
instruction, 2026-08-21. X4 is the only device this campaign has run on so far;
X4 Pro and X3 are targets. A number without a device name is not a finding, and
on this line it is not a formality: one C3 binary drives X4 and X3 with the
profile chosen at runtime, so nothing else in a row identifies the hardware.

## Order of work across sessions

| # | Step | Precondition | Exit criterion |
|---|---|---|---|
| 0 | No-flash, no-instrument work (below) | none | host test green; lab screen builds clean; attempt-2 card read |
| 1 | Implementation minimum for the first flash (below) | step 0's lab screen | build passes the pre-flash checklist |
| 2 | Bench smoke: control run + experiment 3 | step 1; X4 lock; flash approved | go/no-go verdict for S2 recorded in `power-management.md`, residency number included |
| 3 | Experiment 3 slope run (unattended, >= 2 h) | step 2 passed | run row in `power-plan.md` table + record below |
| 4 | Experiment 6 (RTC_SLOW, advertising only) | step 2's rig proven | advertising slope recorded, verdict vs experiment 3's advertising state |
| 5 | Experiment 1 (board floor, latch-held deep sleep) | a uA-capable meter exists (see "The instrument problem") | floor priced, or brownout verdict on this revision |
| 6 | One validation run on rebased `develop` | a finding is about to change the product | finding reproduced once on current `develop` (frozen-baseline condition 4) |

Steps 0 and 1 can happen in any session with no device and no lock. Step 5 can
run before step 4 if the meter arrives first -- they are independent.

## The unattended night run: what to do with 6-8 h and nobody there

**Written 2026-08-21 for that night's run; the shape is reusable.** The whole
point of an unattended window is that it can only run what needs **no hands**, so
the division of labour is the plan:

- **Needs a person** (a button press, WiFi on, reading an IP): the idle floor on
  Home, WiFi mode, observation mode, and anything on the power lab screen. Do
  these while the maintainer is at the desk. They are 20-30 minutes each.
- **Needs nobody**: everything reachable over BLE from the laptop, which is every
  connected/advertising comparison. The device sits in the map, unplugged, and
  `tools/blefakephone.py` drives it.

**Do not flash for an unattended night.** It is tempting -- the power lab screen
would label every leg in the `state` column -- but `CMD:GOTO_POWERLAB` and
`CMD:POWERLAB_STATE` are wired to `main.cpp`'s **serial** `CMD:` dispatch, not to
the BLE command characteristic (which lands in `MapCommandParser`). With USB out,
which a power run requires, the lab screen cannot be driven at all. So a flash
would risk the whole night for a cosmetic column, when `ble` plus a timestamped
schedule already separates the legs.

### The leg pattern: rapid alternation, not long legs

**Rewritten 2026-08-21 after run 4.** The first version of this plan was nine
40-minute legs with a reference between each pair. Run 4 showed why that fails: a
40-minute leg in the plateau moves the voltage **1-2 ADC counts**, so no single
leg resolves anything and no amount of reference legs fixes it
(`power-plan.md`, "The plateau problem").

What does work with the same instrument is **alternation**. The curve term is
smooth and slow; the state term flips on a schedule we choose. Difference every
adjacent A/B pair and average over many cycles, and the drift cancels even though
each individual leg is inside noise. The estimator is the mean of paired
differences, and its error falls as the square root of the number of pairs -- so
sixteen 25-minute legs beat four 100-minute ones for a *comparison*, while a long
run is still what a single state's absolute slope needs.

One night, one comparison, alternating:

| Plan | Legs | Answers |
|---|---|---|
| **A: the link's own cost** | `adv` / `conn --interval 3600` x 8 pairs, 25 min each | the scoreboard's `[open]` "bringing the radio up at all" row |
| **B: send cadence (M2)** | `conn --interval 7` / `conn --interval 30` x 8 pairs, 25 min each | the maintainer's question, with the panel held constant by construction |

**Pick one per night.** Two comparisons in one night halves the pairs for each,
and the pair count is the whole point. Plan A first: it is the bigger term, and
plan B's answer only matters if the radio turns out to cost anything.

**Check the isolation rather than assuming it**: `ref_window` and
`panel_busy_ms` deltas must match across the two states of a pair. If they do
not, the difference is not the radio.

**Drop the first leg after any reboot, and every boundary row.** Both cost run 4
a leg (`power-plan.md`, run 4).

### Rules for the unattended window

- **`--duration`, never `timeout`.** Verified on hardware 2026-08-21: SIGTERM
  kills python before asyncio unwinds `BleakClient` and the LE link stays up in
  the kernel, so the next leg silently runs connected. `--duration` disconnects
  cleanly (`tools/blefakephone.py`).
- **`python3 -u`**, or a killed process takes its log with it.
- **Retry each leg.** A link that drops at 03:00 with nobody watching costs the
  rest of the night otherwise.
- **USB stays out.** VBUS charges the cell and the run stops meaning anything
  (`power-plan.md`, constraint 1).
- **Log the schedule with wall-clock timestamps.** That log plus `uptime_s` is
  what maps a CSV row to a leg, and run 3 proved it lines up to the minute.
- **Check the band, not just the battery percent.** Legs are only comparable
  inside roughly 4.05-3.80 V. Six hours at a 13 mA mix costs ~85 mAh, about 13 %
  of a 650 mAh pack, so a night that starts at 85 % ends around 72 % -- inside
  the band at both ends.
- **A flash in between ends the series.** USB charges the cell, so the pack comes
  back to the top of the curve and the first hour after unplugging is relaxation
  again (`power-plan.md`). And the build changes, which is the one column the
  scoreboard cannot difference across. After any flash: start a new series, do not
  extend an old one, and let the pack sit 30 minutes off the charger first.

## Step 0: what needs no flash and no instrument

All of this is laptop work. Do it whenever a session has no device access.

- **The parked-loop policy as a pure function, plus its host test.** Guard
  item 2 of `power-idle-sleep.md`, "How this gets guarded": inputs are parked /
  queued work / transfer active / recent input, output is the tick cadence. No
  hardware in it. Host tests configure the same way the native map preview does
  (`README.md`, "Native map preview"); registration is per-test-directory via
  `gtest_discover_tests` (e.g. `test/map_tile_path/CMakeLists.txt:14`), the top-level
  `test/CMakeLists.txt` only calls `enable_testing()`:

  ```
  cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
  cmake --build build/test && ctest --test-dir build/test
  ```

  Not required before experiment 3 (see below), but it is free to write now
  and it is the only guard that fires at CI time.
- **The power lab screen**, to the spec in `power-idle-sleep.md`, "The power
  lab screen". Compiles and host-builds with no device.
- **The `state` column in `power.csv`** and a reader script that groups rows by
  it. Append-only, never reorder (`src/PowerLog.cpp:15-17`) **[repo]**.
- **Read the attempt-2 bench boot off the card** (a card-reader job, no
  device): rows continuing past the 10 MHz throttle mean the CPU lived and only
  the radio died. Open and cheap (`power-management.md`, "Why 10 MHz breaks
  BLE", last paragraph).
- **Config verification with no flash.** After any `pio run`, grep the
  generated `sdkconfig.default` for the `_EFF` values -- requested is not
  compiled-in (`power-management.md`, the modem-sleep section, "Verify it
  compiled in rather than merely requested"). That generated file is
  gitignored; cite `platformio.ini`, not it (`power-management.md`,
  "`sdkconfig.defaults` is generated and gitignored").

## Step 1: the implementation minimum before the first useful flash

Experiment 3's build must contain, and nothing more:

- [ ] Lab screen with at least three runtime states: idle radio down,
      advertising, connected. Paint once, then never redraw.
- [ ] `state` column written to `power.csv` per row.
- [ ] `esp_sleep_get_wakeup_cause()` printed on entry.
- [ ] `CMD:GOTO_POWERLAB`, with `setPowerSaving(false)` before any BLE init,
      same as `CMD:GOTO_MAP` (`src/main.cpp:734`) **[repo]**.
- [ ] Behind `-DENABLE_POWER_LAB=1`, `default` and `slim` only. Precedent:
      `ENABLE_PREVIEW_BENCH` (`src/activities/ActivityManager.cpp:221-222`,
      `platformio.ini:233`) **[repo]**.
- [ ] A residency counter. **`CONFIG_PM_PROFILING`** makes `esp_pm_dump_locks()`
      report "what time the chip spends in each power saving mode" (ESP-IDF
      `components/esp_pm/Kconfig:37-48`; `esp_pm_dump_locks()` declared in
      `components/esp_pm/include/esp_pm.h:197`) **[primary]**.
      **The trap is in the same help text**: it "does incur some run-time overhead, so
      should typically be disabled in production builds". So the residency build is not
      the draw build, and a frozen-baseline series cannot get both numbers from one
      binary -- take residency first as its own short bench, then the slope on a
      profiling-free build. `CONFIG_PM_TRACE` is the alternative with no software
      overhead: it signals idle entry and exit on GPIOs (`Kconfig:50-57`) and needs a
      scope.
- [ ] An evidence channel that survives light sleep: `power.csv` is mandatory;
      `stats` over BLE from the lab screen is optional (it is wired to
      `MapActivity` and `TileSyncActivity` today through `fillMapPowerStats()`,
      `src/activities/map/MapPowerStatsProvider.h:19`, used at
      `MapActivity.cpp:1828` and `TileSyncActivity.cpp:146` **[repo]** -- the
      lab screen needs its own wiring or does without).
- [ ] Heap check after adding the screen -- a lab screen that brings up BLE
      pays the map's 57 KB (`power-idle-sleep.md`, "The power lab screen").

**The parked-loop work is deliberately NOT in this minimum.** Experiment 3
measures the residency of today's 100 Hz loop first; that number decides how
much of "S2's missing half" gets built (`power-idle-sleep.md`, "The open
question this design turns on"). Building it before measuring is doing the
design backwards.

## Process gates: the checklist for every flash

In flow order. Skipping a line is how the 2026-08-16 hangs happened twice.

**Build, before any lock:**

- [ ] Fresh worktree? `git -C <worktree> submodule update --init --recursive`
      first, or `pio run` fails on a `freeink-sdk` symlink (parent
      `CLAUDE.md`).
- [ ] Branch is cut from the **frozen base commit**, not from current
      `develop`. The rebase-before-flash rule is exempted for this campaign;
      the substitute check is the tripwire -- if `develop` gained a change to
      the power surface (BLE params, map loop, refresh cadence, `PowerLog`),
      restart the series on a new base instead of mixing
      (`power-plan.md`, "The frozen baseline", condition 3).
- [ ] `TRAILINK_VERSION` identifies the baseline: `powerlab-<base-hash>-<n>`
      (condition 1; the string is produced by `scripts/git_branch.py` and
      injected at `platformio.ini:222,243` **[repo]**).
- [ ] `pio run` clean, zero warnings.
- [ ] Grep the generated `sdkconfig.default` for every option the experiment
      claims to set. Requested is not compiled in.

**Only now, the device:**

- [ ] `python3 tools/x4lock.py acquire --owner <session> --reason "<exp>" --ttl <s>`
      (parent repo). Never take it before the binary exists.
- [ ] Identify the port: `udevadm info -q property -n /dev/ttyACM0 | grep
      ID_VENDOR_ID` -- `303a` is the X4, `04e8` the phone. On the wrong port
      esptool says "Invalid head of packet", which is not a cable problem.
- [ ] **Ask the maintainer before the upload.** Every upload, its own question.
      Holding the lock is not permission.
- [ ] Flash, watch the boot log for the experiment's expected lines.
- [ ] `CMD:` only lands in the ~3 s boot window after reset
      (`power-plan.md`, bench rig note) **[measured]** -- send
      `CMD:GOTO_POWERLAB` there.

**After:**

- [ ] Release the lock as soon as device work is done.
- [ ] Once a build is confirmed good on hardware, archive it:
      `docs/firmware-builds/` in the parent repo, with a `.sha256` beside it
      like the existing archives. For this campaign that archive is also
      frozen-baseline condition 2 -- the reflashable baseline.
- [ ] Record the run: one row in `power-plan.md`'s Runs table plus the record
      template below. Evidence CSV goes to `docs/power-runs/` in the parent
      repo (precedent: `run2-2026-08-16.csv`).

**If a build hangs the device** (screen frozen, serial dead, buttons dead):
the ROM bootloader still answers. Reflash the last archived known-good binary,
no rebuild:

```
esptool write-flash 0x10000 docs/firmware-builds/<last-good>.bin
```

(parent repo path; mechanism confirmed 2026-08-04, parent `CLAUDE.md`)
**[measured]**.

## Experiment 3: the `CONFIG_PM_ENABLE` light-sleep smoke test

Go/no-go for S2, and the only unconditional experiment. Full rationale:
`power-idle-sleep.md`, experiment 3 and "The landmine".

**Build.** One branch from the frozen base. Add to `custom_sdkconfig` in
`platformio.ini` (the block opens at `platformio.ini:88`; the three
modem-sleep lines at `:158-160` are already set) -- never to the gitignored
`sdkconfig.defaults`:

```
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_ESP_PHY_MAC_BB_PD=y
CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y
```

The PU line is the one whose absence makes the whole route a silent no-op
(`power-management.md`, "`CONFIG_PM_ENABLE` alone saves nothing while the
radio is up") **[primary]**. Plus one code change: `esp_pm_configure()` with
max 160, min 80, light sleep enabled.

**Build verification, before the lock:** all four options present in the
generated `sdkconfig.default`. After flashing, the boot log must NOT print
`light sleep mode will not be able to apply when bluetooth is enabled.` -- if
it does, the config did not take; that is a build bug, not a hardware finding.

**Bench.** The 2026-08-16 rule applies: **control run first**, on the last
archived known-good build, same rig, then the PM build.

```
# reset, send CMD:GOTO_POWERLAB (or CMD:GOTO_MAP) in the boot window, then:
python3 tools/blefakephone.py --pos <lat> <lon> --heading 4 --no-tiles --no-serial
```

(parent repo; pass/fail criteria for the rig itself: `power-plan.md`, "Bench
rig".)

**What to watch, 15-30 min on the bench:**

- The link: at least as many fixes as the control run over the same duration,
  and the device still logging to `power.csv` after the central leaves.
- **USB serial dying when sleep engages is the feature working, not a fault**
  (`power-idle-sleep.md`, experiment 3). From that moment evidence comes from
  `power.csv` and BLE only. Distinguishing "sleeping" from "hung": `power.csv`
  rows keep appearing once a minute (`src/PowerLog.h:33` **[repo]**), and the
  fake phone keeps receiving.
- The drivers under DFS plus light sleep: panel refresh completes and is
  legible; ladder buttons register; SD keeps writing rows. Expect to find at
  least one break (`power-idle-sleep.md`, open questions).
- The residency number.

**Duration.** Bench: minutes to tens of minutes. Then, if the bench passes,
one unattended slope run >= 2 h in the connected state, comparable start
voltage (`power-plan.md`, "Methodology" and constraint 4).

**Predictions, stated to be refuted:**

- Pass: link holds, drivers survive, residency above zero and reported. **No
  numeric draw prediction exists for this build** -- the loop is unparked, so
  the deliverable is the residency number and the driver verdict, not a mA
  figure.
- Fail: device hangs (no `power.csv` rows, no BLE, control run clean), or a
  driver corrupts (panel garbage, buttons dead, SD errors). Name the driver;
  that is the next work item.
- Inconclusive: serial silence alone. Never call a run failed on serial
  evidence after sleep engages.

**The decision the residency number makes:** near-zero residency at the 100 Hz
loop means the parked-loop work ("S2's missing half", all five guard items)
must be built before any slope run is worth the battery cycle. Substantial
residency means a slope run is already informative and the parked-loop work is
a later optimisation. No threshold is invented here: derive it from the
light-sleep entry and exit overhead the residency number implies, which nobody
has measured for this case **[open]**.

## Experiment 6: `RTC_SLOW` advertising-only, riding on 3's bench

The only remaining path to a sub-milliamp parked floor on X4
(`power-idle-sleep.md`, experiment 6). A separate binary from the same frozen
base: in `custom_sdkconfig`, replace `CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y`
(`platformio.ini:160`) with:

```
CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y
```

keeping experiment 3's four PM options. One option changed against the
experiment-3 build, so the two runs price exactly the clock.

**Procedure:** same bench, but **never connect**. Verify the advertisement is
on the air with a scanner (`BleakScanner`, or the phone's scan list), then
leave the device parked in the lab screen's advertising state. The internal RC
cannot hold a connection -- its accuracy is far outside the 500 ppm BLE needs
(IDF Kconfig; `power-management.md`, "`CONFIG_PM_ENABLE` alone saves nothing")
**[primary]** -- so a central connecting mid-run invalidates the run and may
misbehave. That is expected, not a device fault.

**Duration:** the honest problem is resolution. If this state really lands
near 2 mA, the expected slope is around 2-3 mV/h -- arithmetic scaled from run
2's 32.6 mV/h at 24.0 mA, assumed proportional **[assumed]** -- which is close
to ADC noise per hour. So: many hours (overnight), or wait for the meter from
experiment 1 and read it in minutes.

**Predictions, stated to be refuted:**

- Worth pursuing: parked-advertising slope measurably below the
  experiment-3 build's advertising-state slope at matched start voltage.
- Refuted: indistinguishable slopes -- then the crystal really was the only
  path and X4 stops at the main-XTAL column (`power-idle-sleep.md`,
  experiment 6).
- Inconclusive: slope difference within ADC noise over the run length --
  extend the run or use the meter before writing a verdict.

## Experiment 1: the board floor, and whether deep sleep can keep the latch

Prices the one term left in the parked budget
(`power-management.md`, "The board's own floor is unpriced"). Needs the meter;
read the next section first.

**Code change** (the lab screen's deep-sleep state, safety rules from
`power-idle-sleep.md`, "The power lab screen"):

- Drive GPIO13 HIGH instead of LOW before the existing hold in
  `startDeepSleep()` (`lib/hal/HalPowerManager.cpp:100-109`) **[repo]**.
- Always arm `esp_sleep_enable_timer_wakeup()`, cap the duration, and paint
  what the device is about to do and when it returns, before it sleeps.
- On wake, reconfigure GPIO13 as output-HIGH **before** `gpio_hold_dis()` --
  the non-self-latching field revision power-cycles itself otherwise
  (`power-idle-sleep.md`, "Safety, not optional") **[primary]**, **[repo]**.

**Bench:** meter in series with the battery, USB disconnected -- USB charges
the cell and kills the measurement (`power-plan.md`, constraint 1)
**[measured]**. Enter the deep-sleep state from the lab screen, read the
steady current, let the timer wake fire, confirm the device boots and prints
the wake cause. Minutes per reading, not hours: the meter reads directly.
While the meter is wired, also read the lab screen's other states -- each
reading cross-checks a slope-derived figure for free.

**Predictions, stated to be refuted** (bands from `power-idle-sleep.md`,
experiment 1):

- Brownout on entry or on wake: this hardware revision cannot hold its own
  latch, and the whole latch-held deep-sleep class is dead on it.
- 1 mA or more: the board floor bounds every deep state, most of what a future
  crystal board would buy, and S2's expected total.
- 10-100 uA: the deep states are real and the floor is not the problem.
- Plus the functional half, meter or no meter: the timer wake fires and the
  device boots from it.

Deep sleep is used here **once, as an instrument**, then never again -- it is
rejected as a feature (`power-idle-sleep.md`, "S3 -- rejected").

## X3 has a current meter on the board, and the firmware already reads it

**Found 2026-08-21 while asking how a meter would physically attach to an X4.**
The answer for X3 is: it does not have to.

`BoardConfig`'s X3 profile carries a **BQ27220 fuel gauge at 0x55** on SDA20/SCL0
(`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h`, the X3 profile)
**[repo]**. X4 has none -- `gaugeAddr` 0, battery read off an ADC divider -- which
is why the two devices need different instruments and why every measurement has
to name its device.

**And the register is already being read.**
`freeink-sdk/libs/hardware/BatteryMonitor/src/BatteryMonitor.cpp:23` defines
`BQ27220_CURRENT = 0x0C`, described in that comment as *average current, signed
mA*, and `:211` reads it -- then uses **only its sign**, to decide whether the pack
is charging, and throws the magnitude away **[repo]**.

So on X3 the device can report its own draw in milliamps, with no meter, no
soldering and no teardown. Two consequences, both large:

- **It measures current, not voltage, so the plateau problem does not exist for
  it.** Every difficulty in `power-plan.md`'s "The plateau problem" comes from
  inferring power from a slowly-moving voltage. A current register is a direct
  reading at one operating point.
- **It arrives with a device rather than a purchase.** X3 dev units were asked for
  in the 2026-08-19 outreach. If one lands, the campaign gets its instrument for
  free -- for X3.

What is **not** settled, and must be read off the BQ27220 datasheet before
anything is claimed (this repo's research-numbers rule: a primary page, not
memory): the register's resolution, its averaging window, and whether it is
trustworthy at single-digit milliamps, which is exactly the range that matters
here. A gauge tuned for a phone's discharge may quantise at 1 mA or worse.

The work to expose it is small and specified: `BatteryMonitor` needs a public
`readCurrentMa()` beside `readMillivolts()`, and `PowerLog` gains a `cur_ma`
column next to `board`. It is deliberately **not** written yet -- it can only be
verified on an X3, and this repo does not merge untested firmware.

## The instrument problem, honestly

`power.csv` resolves milliamps over hours from the `batt_mv` slope. It cannot
see microamps, and plugging USB in charges the cell and kills the run
(`power-plan.md`, constraints 1-3) **[measured]**. USB serial also dies when
light sleep engages, so mid-run evidence is `power.csv` and BLE, nothing else.

**We do not know what the maintainer owns.** So the plan degrades:

**How it physically attaches to an X4, which nobody has checked.** Every plan
below says "a meter in series with the battery", and the prerequisite behind that
sentence has never been examined: **is the X4's cell on a connector, or soldered
to the board?** Nothing in this repo says, because nobody has opened one. That is
the first thing to establish, before any purchase, and it decides which of two
wiring schemes is even possible:

- **Source-meter mode -- the meter replaces the cell.** Disconnect the battery,
  feed the board from the instrument at a fixed voltage. This is the better
  measurement *and* the easier one: no shunt in series with a live cell, no burden
  voltage at a wake spike, and -- the real prize -- **a constant supply voltage, so
  the discharge curve stops being a confounder at all**. Needs the cell to be
  disconnectable and needs the battery MOSFET latch (GPIO13,
  `lib/hal/HalPowerManager.cpp:100-109`) to behave when fed from outside, which is
  itself **[open]**.
- **Ampere-meter mode -- in series with the existing cell.** Break one battery
  lead, insert the meter, the cell still powers the device. No supply needed, but
  it means cutting or unplugging a lead, and it carries the burden-voltage trap
  described below.

**Do this on a lab device, not the daily driver.** A device that stays open with
leads hanging out of it is the right home for this; the 2026-08-19 outreach asked
for X3 and X4 Pro units partly for that reason.

**A candidate for the no-teardown route, 2026-08-21: Joy-IT JT-UM120.** Read off
the manufacturer's page (`https://joy-it.net/en/products/JT-UM120`) **[primary]**:

| Spec | Stated |
|---|---|
| Current | 0-7 A, resolution **0.00001 A (10 uA)**, accuracy +/- 0.05 % + 2 digits |
| Voltage | 4-28 V, resolution 0.00001 V |
| Interface | "Evaluation via PC software", 10 measurement groups, Micro-USB PC port |
| Connectors | in USB-A / USB-C / Micro-USB, out USB-A / USB-C |

**Resolution is three orders of magnitude better than this campaign needs** -- the
open questions are separations of single milliamps, and at a 50 mA reading the
stated accuracy works out near 45 uA. Its limits are the route's limits, not the
instrument's:

- It sits on the **5 V side**, so it prices differences between states, not
  absolute current out of the cell ("Except once charging has terminated" in
  `power-management.md`).
- The PC software is almost certainly Windows, so on this laptop expect **manual
  readings off its display**. That suits short settled readings and rules it out
  for driving an unattended alternation overnight -- which is a pity, because that
  is the run it would otherwise be best at.
- The charger will still wake to top off; that shows as a step, not a finding.

Price **[open]** -- the alza.sk listing returns 403 to an automated fetch, so it
has to be read off the page by hand and written down with its currency and date,
per the parent `CLAUDE.md` research-numbers rule.

- **PPK2-class instrument** (Nordic Power Profiler Kit II or equivalent:
  source-meter, nA-to-mA autorange, logs a current waveform). Gives experiment
  1 its answer in minutes, catches wake spikes, and prices every lab-screen
  state directly instead of through hours of slope. The cheapest purpose-built
  option if nothing suitable exists -- on the order of USD 100 **[assumed]**,
  and the price must be read off a distributor page before buying, per the
  repo's research-numbers rule.
- **A cheap multimeter in series** can answer experiment 1's *decisive*
  question -- is the floor 1 mA or more -- on its mA range, where 0.1 mA
  resolution is enough for that verdict. What it cannot do: resolve 10 uA from
  100 uA on the mA range. And its uA range is a trap: the shunt's burden
  voltage at a wake spike (tens of mA through a uA-range shunt) collapses the
  supply and browns the device out, and many meters break the circuit while
  switching ranges, which is a power cycle. If a uA reading is attempted
  anyway, enter deep sleep first on the mA range, then switch a *parallel*
  meter in -- never range-switch in series under a device that can wake. A
  brownout seen with a DMM on the uA range is the meter's doing, not the latch
  verdict, which is exactly the confusion the wake-cause print exists to
  catch.
- **No meter at all:** experiments 3 and 6 run in full -- they are mA-scale and
  the `power.csv` slope prices them. Experiment 1's functional half runs too
  (does the latch hold, does timer wake boot, brownout or not on the
  self-latching question). Only the floor's actual value waits for a meter.

## Per-run record

One row goes in `power-plan.md`'s Runs table
(`Date | Build | State | Duration | Start mV | End mV | Slope mV/h | Notes`).
This fuller record is kept with the run's CSV in the parent repo's
`docs/power-runs/`:

```
## Run N -- YYYY-MM-DD, <experiment / state>

- **Device: <X4 | X4 Pro | X3>** -- first line, never omitted. One C3 binary
  drives X4 and X3 and the profile is runtime-selected, so the build string does
  not say which, and LOW_POWER_FREQ, the panel controller, the fuel gauge and the
  cell all differ. A `board` column waits on branch `power-lab`, unmerged, so for
  now the device is recorded here and nowhere else.
- Base commit (frozen base): <hash>
- Build (TRAILINK_VERSION): powerlab-<base-hash>-<n>
- Options differing from the frozen base: <exact custom_sdkconfig lines, or
  "none -- runtime state only">
- State: <lab screen state, or campaign state 1-4>
- Rig: <bench + blefakephone args / unattended>; where it ran (temperature
  confounder, power-plan.md constraint 5)
- Instrument: <power.csv slope / meter model / both>
- Start mV / end mV / duration / slope mV/h:
- Counter deltas: throttled_ms % | loops Hz | loop_busy % | panel_busy % |
  ref_full/half/fast/window | ble
- Residency (PM builds only):
- Evidence file: docs/power-runs/runN-YYYY-MM-DD.csv (parent repo)
- Prediction this run tested, and the verdict:
```

The `build` column in every CSV row carries `TRAILINK_VERSION`, so the file
identifies its own baseline (`power-plan.md`, "The frozen baseline",
condition 1).

## What would make us stop

Per experiment, the outcome that ends the line rather than continuing it:

- **Experiment 3:** the SPI panel breaks under DFS plus light sleep and no PM
  lock or driver fix restores it. That kills S2 as configured, and with the
  crystal already ruled out (`power-idle-sleep.md`, experiment 2, answered),
  X4's floor stays where route A left it. A broken ADC ladder or SD is a work
  item, not a stop -- both have software workarounds (gate the poll, pause the
  log); the panel does not. USB CDC breaking is expected and is not a finding.
- **Experiment 6:** slope indistinguishable from the main-XTAL build's
  advertising state after a run long enough to resolve it. Then there is no
  sub-milliamp story on X4 at all: stop optimising the parked floor and spend
  the effort on S1 and the parked-loop items, which pay on any build.
- **Experiment 1:** floor 1 mA or more. Then every deep state is bounded by the
  board, S2's 2.3-mA-plus-floor target is recomputed against it, and no
  further sleep-state work is worth a session until the board changes. A
  brownout on the non-self-latching revision stops only the latch-held class,
  and only on that revision -- note which revision was tested.
- **Any experiment:** a hang whose control run is also dirty means the rig is
  broken, not the firmware -- stop and fix the rig before spending another
  flash (the 2026-08-16 lesson, `power-plan.md`, "Bench rig"). And a BlueZ
  `start_notify` failure on the laptop after many connect cycles is a host
  problem (`bluetoothctl power off; power on`), not a device finding
  (`power-management.md`, "A note on the rig") **[measured]**.

A stop verdict is a finding: write it into `power-management.md` with the run
that produced it, and update `power-idle-sleep.md`'s open questions in the
same pass.
