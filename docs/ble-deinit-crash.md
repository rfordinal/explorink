# BLE deinit crashes the host task

`NimBLEDevice::deinit(true)` can kill the device. The NimBLE host task calls a
NULL function pointer and the chip resets. The bug is in NimBLE-Arduino, not in
our code, and it is still unfixed upstream as of 2026-09-01.

We call `deinit(true)` from `BlePositionServer::end()`
(`lib/BlePositionServer/src/BlePositionServer.cpp:457`), and two activities call
`end()` on their way out: the map
(`src/activities/map/MapActivity.cpp:2350`) and the sync screen
(`src/activities/map/TileSyncActivity.cpp:512`). Every exit through either one
rolls this dice.

Status: **root cause confirmed from a real coredump**. Fix not chosen yet, see
T-233. The coredump, the SD report, the decoded backtrace and the build it came
from are archived in the parent repo at
`docs/crashes/2026-09-01-nimble-host-null-callback/`.

## The crash

Real ride, LilyGO T5 S3 Pro, 2026-09-01. Build `0.2.0-t5s3pro` from
`feat/t5s3-gnss` @ `4e3305e7`. Rider held the power button to sleep the device
while the map was open. Device reset instead.

`crash_report.txt` on the SD card had an empty panic reason and an empty stack
(see [`crash-reporting.md`](crash-reporting.md) for why both are empty on
Xtensa). The coredump partition had the real answer.

```
Crashed task: 'nimble_host'
exccause  0x14 (InstFetchProhibitedCause)
pc        0x0
a0        0x82053472   -> NimBLEDevice::host_task+6
a10       0x3fca5878   -> ble_hs_timer + 8
```

`pc 0x0` with `InstFetchProhibited` means a call through a NULL function
pointer. The faulting instruction is in `nimble_port_run()`:

```asm
0x420638da:  l32i   a8, a10, 4     ; a8 = ev->fn
0x420638dd:  callx8 a8             ; a8 == 0
```

`a10` is the event that was dequeued. It resolves to `ble_hs_timer + 8`, which
is the `ev` member of the host's own timer callout. So **`ble_hs_timer.ev.fn`
was NULL when the host task ran the event**.

That fact rests on the registers and the faulting instruction, not on a memory
read. `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` is off, so `.bss` is not in the
coredump — gdb prints `ble_hs_timer` as zeros, but those zeros come from the
ELF's NOBITS section, not from the device. Do not quote them as evidence.

## Two tasks, one moment

`loopTask` was on the sleep path, blocked. The call chain, with function
definitions located on `develop`:

```
enterDeepSleep (src/main.cpp:401)
  ActivityManager::goToSleep -> exitActivity
    MapActivity::onExit (src/activities/map/MapActivity.cpp:2289)
      BlePositionServer::end (lib/BlePositionServer/src/BlePositionServer.cpp:457)
        NimBLEDevice::deinit(clearAll=true)
          nimble_port_stop -> ble_hs_stop (ble_hs_stop.c:242)
            ble_gap_preempt -> ble_gap_adv_stop_no_lock -> HCI "adv disable"
              waiting on the BT controller semaphore
```

Those are definition lines, not the coredump's own frames. The coredump was
taken on `feat/t5s3-gnss` @ `4e3305e7`, where the same frames carry
`main.cpp:420`, `MapActivity.cpp:2425` and `BlePositionServer.cpp:457`.
`backtrace.txt` in the case directory is the verbatim record; this block is the
readable path through `develop`.

`nimble_host` crashed while `loopTask` sat in that wait. This is why the
`BLEPOS heap:` line (`lib/BlePositionServer/src/BlePositionServer.cpp:472`) is
missing from the crash report: `end()` never returned.

## Root cause

All five steps read off the vendored source in
`.pio/libdeps/<env>/NimBLE-Arduino/src/nimble/`, version 2.5.1.

1. `ble_hs_stop_begin()` (`nimble/host/src/ble_hs_stop.c:190`) sets
   `ble_hs_enabled_state = BLE_HS_ENABLED_STATE_STOPPING`, then calls
   `ble_hs_timer_resched()` on the next line.
2. `ble_hs_is_enabled()` (`nimble/host/src/ble_hs.c:353`) returns true only for
   `_ON`. It is now false.
3. `ble_hs_timer_reset()` (`nimble/host/src/ble_hs.c:477`) therefore takes the
   branch `ble_npl_callout_stop(); ble_npl_callout_deinit();`.
4. `npl_freertos_callout_deinit()`
   (`porting/npl/freertos/src/npl_os_freertos.c:475`) does
   `ble_npl_event_deinit(&co->ev)` and then
   `memset(co, 0, sizeof(struct ble_npl_callout))`. **`ev.fn` is now NULL.**
5. If the timer had already expired, its `ev` is **already sitting in
   `g_eventq_dflt`**. The memset does not take it out of the queue. The host
   task dequeues it in `nimble_port_run()`
   (`porting/nimble/src/nimble_port.c:296`) and calls `ev->fn`.

`ble_npl_eventq_remove()` exists in that port
(`porting/npl/freertos/include/nimble/nimble_npl_os.h:186`) and is exactly what
step 4 fails to call.

Why it is rare: the host timer must be in flight in the same instant the stop
begins. The map has been exited hundreds of times without this.

**The exit route is not part of it.** This paragraph used to end "sleeping from
the map is the most common exit, so it is the most likely place to hit it",
reasoning from the only two crashes known on 2026-09-01, both of which were the
power button. Two later crashes killed that: 09-03 came out of
`TileSyncActivity`, a different screen entirely, and 09-04 came out of the map
by a **swipe**. Three routes, one call site. What has to line up is the timer,
not the gesture.

**GNSS is not involved.** It appears nowhere in the chain. A 10 Hz NMEA stream
does change loop timing, so it may change how often the window is hit — that is
speculation, not a finding.

## Four times now

| when | screen | how it was left | coredump |
| --- | --- | --- | --- |
| 2026-08-31 | map | menu opened and closed repeatedly | overwritten before anyone pulled it |
| 2026-09-01 | map | power button, into sleep | decoded, this document |
| 2026-09-03 | sync screen | leaving `TileSyncActivity` | decoded, `crashes/2026-09-03-t5s3-tilesync/` |
| 2026-09-04 | map | swipe | pulled, **not** decodable, see below |

The parent repo's `docs/BUGS.md` (BUG-033) is the running incident list; the
mechanism below is written once and does not change per crash.

**The 2026-09-04 dump could not be symbol-decoded, and that is the reusable
lesson.** The build was flashed at 22:12 and the worktree that produced it was
rebuilt an hour later, taking the only matching `firmware.elf` with it.
`esp-coredump` compares the ELF SHA256 recorded in the dump against the ELF it
is handed and **refuses a near-miss** rather than decoding into plausible
nonsense, so the wrong-ELF trap cannot be walked into by accident -- but the
answer is then simply unavailable. The registers were still readable straight
out of the dump's own ELF notes, with no symbol table: `nimble_host`,
`exccause 0x14`, `pc 0x0`, `a7` and `a10` byte-identical to the 09-01 dump and
`a0` and `a8` offset from it by a constant `0x100`. That is enough to identify
the fault and not enough to name a line. Full write-up in the parent repo at
`docs/crashes/2026-09-04-map-exit-restart/`.

## The 2026-08-31 restart, found after the fact

**One day before the 09-01 coredump**, the same board restarted while the map menu was
opened and closed repeatedly. That report is quoted verbatim in
`lilygo-t5s3-bringup.md` on `release/lilygo-t5-s3-pro`, and its tail is:

```
[884824] [DBG] [ACT] Exiting activity: Map
[884854] [DBG] [MTS] missing tile list saved (2 entries)
[1] [DBG] [UI] Using Lyra theme
```

Ours stops one statement earlier in the same function -- `saveLaddersIfChanged()`
rather than `MISSING_TILES.flushIfDirty()`. Four things line up:

- Both restarts land inside `MapActivity::onExit()`.
- Both stop **before** the `BLEPOS heap:` line, so both are inside or before
  `BlePositionServer::end()`.
- Both wrote a `crash_report.txt` at all, which **rules out a watchdog and a
  brownout**: those set their own reset-reason hints and `isRebootFromPanic()`
  filters them out, so neither would have produced a file
  ([`crash-reporting.md`](crash-reporting.md), "Gap 3").
- On 31 August a phone was connected -- the log carries `conn params` lines --
  so `deinit(true)` ran against a live link, which makes the race more likely,
  not less.

**Read, strongly corroborated, not proven.** The 31 August coredump was
overwritten by ours before anyone extracted it: one partition,
`CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` off. `docs/crashes/README.md` in the
parent repo exists to stop that happening again, and it arrived one day late.

## The offending line is not upstream

Checked 2026-09-01 by reading the actual files:

| tree | `ble_hs_timer_reset()` when not enabled |
|---|---|
| apache/mynewt-nimble master | `ble_npl_callout_stop()` only |
| espressif/esp-nimble master | `ble_npl_callout_stop()` only |
| espressif/esp-nimble @ `f566133`, `e3cbdc0`, `70439dd` | `stop()` **and** `deinit()` |
| h2zero/NimBLE-Arduino master | `stop()` **and** `deinit()` |

So the `deinit()` came from esp-nimble, espressif has since dropped it, and
NimBLE-Arduino still carries the old snapshot. **No newer NimBLE-Arduino
release fixes this** — 2.5.1 (2026-07-30) is the latest and is what we build
against; master still has the line.

`platformio.ini` asks for `h2zero/NimBLE-Arduino @ ^2.3.8`, which resolves to
2.5.1 today. The version in the ini is not the version on disk — check
`.pio/libdeps/<env>/NimBLE-Arduino/.piopm`.

## Removing the line leaks nothing

`ble_hs_deinit()` (`nimble/host/src/ble_hs.c:910`) already ends with
`ble_npl_callout_deinit(&ble_hs_timer)`. That is the right place: the host has
stopped and the queue is drained by then. The early one in
`ble_hs_timer_reset()` is redundant as well as harmful.

## Fix options

Not decided. T-233.

1. **Stop calling `deinit(true)` on map exit.** Keep the stack up, pay the RAM.
   Cheapest to test, most expensive in memory, and memory is tight here
   (`map-memory.md`).
2. **Patch the vendored NimBLE-Arduino** — drop the
   `ble_npl_callout_deinit(&ble_hs_timer)` line, matching esp-nimble and
   mynewt-nimble. Exact fix, but it lives in `.pio/libdeps`, so it needs a
   pinned fork rather than an edit that the next `pio pkg update` eats.
3. **Reported upstream 2026-09-01**:
   [NimBLE-Arduino#1184](https://github.com/h2zero/NimBLE-Arduino/issues/1184),
   with the coredump evidence, the upstream comparison and an offer of the
   one-line PR. Tracked as T-235. This does not unblock the two options above:
   even a fast merge is weeks from a release we pin against, and the device
   crashes today.

## Verified and not

- **Verified on hardware**: the fault, the two backtraces, the event identity.
  One coredump from one real crash, 2026-09-01.
- **Verified by reading source**: the five-step chain, the upstream comparison,
  `ble_hs_deinit()` doing the deinit anyway.
- **Open**: how often it actually happens. Four crashes in an unknown number of
  exits. Nobody has tried to reproduce it on purpose.
- **Open**: whether any other `deinit(true)` call site
  (`BlePositionServer.cpp:303`, `:327`, `:371`, `:393`, `:460`) has hit it.
  They run on failure paths, so they are rarer, not safer.
