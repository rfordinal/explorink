# Pressing the buttons from the host: `CMD:BUTTON`

Devel builds accept a serial command that presses a hardware button.

```
CMD:BUTTON down          ->  BUTTON_OK:down:0
CMD:BUTTON back 1500     ->  BUTTON_OK:back:1500
CMD:BUTTON middle        ->  BUTTON_ERR:unknown:back,confirm,left,right,up,down,power
```

Names: `back` `confirm` `left` `right` `up` `down` `power`. Case insensitive.
Second argument is the hold in milliseconds, 0 (a tap) to 10000.

Host side: `tools/press.py` in the parent repo.

On the T5 S3 Pro this is worth more than on the X4: that board has one user
switch plus the capacitive home key, so `up`, `down`, `left` and `right` have
no thumb at all there (`docs/lilygo-t5s3-bringup.md`, and the button map in the
parent repo's `docs/devices/lilygo-t5-s3-pro.md`).

## Why

Before this, a laptop could reach two screens and no more. `CMD:GOTO_MAP` and
`CMD:GOTO_TILESYNC` put the map and the sync screen up; `CMD:SCREENSHOT` reads
the panel back. Home, the file browser, the reader, settings, the map menu and
every confirm prompt in between need a thumb on the device.

Two costs came out of that. A screen nobody can walk to gets reviewed when
somebody is standing at the device and not otherwise -- the same gap
`CMD:GOTO_TILESYNC` was added to close (`docs/tile-freshness.md`, "The check
queue is dots"). And a bug report that starts "press back twice in the reader"
cannot be reproduced without the hardware in hand. The simulator has no thumb
either, and still does not -- see "The simulator compiles it but cannot reach
it" below.

## Where it is injected

`MappedInputManager::rawButton()` (`src/MappedInputManager.cpp`), in front of
the real read:

```
hintButton(index, fn) || injectedButton(index, fn) || (gpio.*fn)(index)
```

The touch hint boxes already sit there, so an injected press is the second
synthetic source on a path activities cannot tell from a thumb.

Not in `HalGPIO`, deliberately. `lib/hal/` is replaced wholesale by the
simulator's own `HalGPIO` (`docs/simulator.md`), so a HAL-level injector would
have to be written twice and kept in sync. `src/` is compiled by both.

The price of that choice: **three places read `HalGPIO` directly and never see
an injected press.**

| what | where | effect |
|---|---|---|
| long-press-to-sleep | `src/main.cpp`, `getPowerButtonHeldTime()` | `CMD:BUTTON power 5000` cannot sleep the device -- **read off the code, not measured** |
| POWER+DOWN screenshot combo | `src/main.cpp` | not reachable; `CMD:SCREENSHOT` already is |
| the reader's own POWER+DOWN check | `src/activities/reader/EpubReaderActivity.cpp` | not reachable |

The first one is the good half. A host script cannot drop the port it is
talking through.

The short power press *is* covered: `main.cpp`'s force-refresh path reads
`mappedInputManager.wasReleased(Power)`.

## The timing model

`src/DebugInput.cpp`. One press at a time, the rest queued (8 deep), each press
advanced one step per input frame -- a frame being one `gpio.update()` in
`loop()`, about 10 ms.

```
frame N     wasPressed + isPressed, held time 0
frame N+k   isPressed, held time = now - down
frame N+m   wasReleased once, held time = the total (>= the requested hold)
frame N+m+1 idle; the next queued press may start
```

Three things in that shape are load-bearing:

- **The release frame carries the total held time.** Half the UI reads
  `wasReleased(X) && getHeldTime() < N` -- the reader's back button is one
  (`src/activities/reader/ReaderUtils.h`). A release frame reporting zero would
  make every long press look short.
- **`getHeldTime()` answers the injected time while a press is down.**
  `MappedInputManager::getHeldTime()` returns `DebugInput::heldMs()` whenever an
  injected press owns the frame. Otherwise the real hardware time under it is a
  stale zero and no `isPressed(X) && getHeldTime() >= N` path can ever fire.
- **The idle frame after a release.** Without it two queued taps run into each
  other as one long press, and `back back` would long-press to Home instead of
  stepping up two levels.

An injected press also counts as user input in `loop()`, for both the
auto-sleep deadline and the CPU throttle. A host walking the UI otherwise
watches the device throttle and then sleep under it.

## Security

Devel builds only: `ENABLE_BUTTON_CMD`, set in `default`, `sticky`, `t5s3pro`
and `simulator`, absent from `gh_release`, `gh_release_rc` and `slim`. The release
build has no injector compiled in at all -- `DebugInput.cpp` is empty there and
the call sites inline to `false`.

**Measured 2026-09-08**, not only reasoned from the `#ifdef`: a `gh_release`
build carries no `BUTTON_OK` or `BUTTON_ERR` string and no `DebugInput` symbol,
while the archived devel build carries one and three. So the check could have
failed.

The reason is the standing one: the device gets lost or stolen, and the person
holding it can plug in USB. A press injector is a thumb for that person -- walk
the menus, open the rider's books, read their pins, all without touching the
device's buttons.

Serial only. It is **not** in the `MapCommandParser` grammar, which BLE shares
(`src/activities/map/MapCommandParser.h`). BLE advertises with no pairing and
no bonding (`docs/ble-advertising.md`), so a BLE version would hand that thumb
to anyone in radio range instead of to someone holding a cable. Both channels
are unauthenticated today (T-222 in the parent repo's `docs/TODO.md`), which is
why the cable is the smaller surface rather than a safe one.

The command reveals nothing by itself: the reply is the button name back.

## The simulator compiles it but cannot reach it

`src/` is shared, so the simulator build carries `DebugInput` and the injection
point. It has no way to send a command: its `HardwareSerial::available()`
returns 0 (`src/HardwareSerial.h` in the simulator fork) -- read off that
header, never tried -- so nothing ever reaches `main.cpp`'s `CMD:` branch
there. Driving the simulator's UI from a
script needs a serial-input path in the fork first, or a different door
altogether -- the fork's JSON socket (`docs/simulator.md`).

## Tested

- `test/debug_input` -- nine host tests over the frame shape: press and release
  edges, hold, two taps not merging, queue order, a full queue refused, a
  double pump in one frame, name parsing. `ctest` runs them with the rest.
- **Verified on the LilyGo T5 S3 Pro, 2026-09-08**, build `db651273`, env
  `t5s3pro`, over `/dev/ttyACM0` with `tools/press.py`. A whole walk ran from
  the laptop with no thumb on the board: `down` moved the Home selection from
  Trips to Sync (the disabled Pins and Wallet rows skipped, as a thumb would),
  `up confirm` opened Explore, `confirm` opened the map menu, `confirm` again
  entered Look around, and `back back` came out to Home. Each step was read
  back with `CMD:SCREENSHOT`.
- **All seven names were pressed.** `left` and `right` pan the Look around view
  east and west: `right` then `left` came back to the same frame, 16 differing
  pixels out of 518,400, in two clusters on map linework (around x=325 y=362
  and x=330 y=556, listed pixel by pixel and looked at zoomed), so the two
  steps really are one step each and symmetric. The first write-up called those
  pixels a label's bounding box; that was inferred from the box and never
  looked at. A bounding box drawn around two clusters says nothing about what
  is inside them.
- **The hold works, and a tap is not a hold.** In Look around, `--hold 1500 up`
  zoomed the map one rung (scale bar 500 m to 200 m) instead of panning, which
  is the `getHeldTime() >= kObserveZoomHoldMs` path (600 ms,
  `MapActivity.cpp`). A plain `up` on the same screen panned north with the
  zoom unchanged. So the injected held time lands on both sides of a real
  600 ms threshold.
- **`power` was pressed, but not held.** The run sent a 0 ms tap, which no real
  button would have slept either, so that check could not have failed. It left
  the map on screen and the port up. The sleep path reads `HalGPIO` directly
  and an injected press never reaches it -- read off `main.cpp`, and open until
  a `--hold 5000 power` run says otherwise (T-286 in the parent repo).
- **It counts as user input.** Both runs whose log was captured (`press.py -v`)
  logged `[PWR] Restoring normal CPU frequency` on the press. That is the
  `userInput` flag in `loop()`; the auto-sleep deadline reads the same flag one
  line later, so the same press resets the sleep timer -- **read off
  `main.cpp`**, not timed out on hardware. The map screen holds
  `preventAutoSleep()` anyway, so timing it out needs the Home screen and a
  full timeout of pressing (T-286).
- **Not run on an X4 or X4 Pro** (C3). The injection point is board-agnostic
  `src/` code, but the C3 envs (`default`, `sticky`) have not been flashed with
  it.
