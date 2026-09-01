# BLE advertising: what the device puts on the air, and when

What a phone can see of this device before it connects. The wire contract for
what happens *after* connecting is `../../docs/ble-map-transfer-protocol.md` in
the parent repo.

Code: `lib/BlePositionServer/src/BlePositionServer.cpp`.

## When it advertises

While the **map screen** or the **sync map tiles** screen is open, minus one
carve-out (next section). `MapActivity::onEnter()` calls
`BlePositionServer::begin()` (`src/activities/map/MapActivity.cpp:1314`), which
builds the GATT table and starts advertising (`BlePositionServer.cpp:295-320`).
`end()` stops it and deinits the stack (`BlePositionServer.cpp:351`), because the
NimBLE host plus controller is 57 KB of heap — measured 2026-08-10 after the BLE
config trim (56,972 B; 64,544 B before it), `map-memory.md:203`. The pre-trim
64.5 KB figure was still quoted here until 2026-08-18.

So advertising is not a background beacon. It is a statement that the rider is
looking at the map right now (or the sync screen is moving bytes), and the phone
app treats it as one: the OS watches for this device's advertisement and starts
the app's bridge service when it appears (`../../docs/ble-app-wake.md`).

## Observe mode: no radio when there is nothing to send or receive

**Added 2026-08-21, verified on hardware (maintainer, same day).**
`MapActivity::loop()`
(`src/activities/map/MapActivity.cpp`, the block right before the
`serviceAdvertising()` call) now stops the whole BLE server -- same call
`onExit()` makes, `BlePositionServer::end()` -- whenever `screenMode_ ==
MapScreenMode::Observe` (`MapActivity.h:649`) **and** no file transfer is
moving bytes (`transfer_.status().active`). It comes back the moment either
condition flips: returning to Follow, or a transfer starting while still
observing (autosync itself is Follow-only, `recheckHatchedTiles()`, but a
transfer already in flight when Observe was entered is not interrupted).

Why Observe specifically: a fix received while observing does not redraw at
all -- `applyFix()` just records it into `observeReturnLatE7_`/`Lon_` and
returns ("Two coordinates, not one", `map-observation-mode.md`) -- so there is
nothing on this screen for the radio to deliver that would change anything
visible. `MapActivity::preventAutoSleep()` reads
`BlePositionServer::isRunning()` (`MapActivity.cpp:3759`), so this also means
Observe with the radio down no longer blocks the idle-sleep timer purely
because BLE happened to still be up -- a rider who stops pressing buttons while
looking around goes to sleep on the normal idle timeout, same as anywhere else
that has no reason of its own to stay awake.

Does not touch `bleStartFailed_`: that flag is set once from `onEnter()`'s
`begin()` and this code only calls `begin()` again on the transition back into
"radio needed", so a genuinely failed init is not retried every tick, only on
that transition.

The maintainer's own-device pass confirmed the basic flip works: BLE drops in
Observe and comes back in Follow, no hang. **Still open:** whether a session
with a lot of rapid Observe/Follow toggling (each flip a NimBLE init/deinit,
~57 KB of heap churn per the section above) costs more than it saves over a
long ride -- that wants a longer soak, not a spot check.

Advertising restarts on disconnect (`BlePositionServer.cpp:193-202`) so a link
dropped mid-ride can come back without leaving the screen. That restart can
fail, and who retries it matters — next section.

## A failed restart is the activity task's problem, not the host task's

`onDisconnect` calls `advertising->start()` **once** and hands the result to
`onAdvertisingState()` (`BlePositionServer.cpp:201`). No loop, no delay. On
failure the flag `advertisingDown_` goes up
(`BlePositionServer.cpp:604-618`) and one `LOG_ERR` is written. The retry runs
somewhere else.

**Why it cannot retry in place.** `onDisconnect` runs on the NimBLE host task
(NimBLE's GAP events are dispatched from `nimble_port_run()` on the task
`NimBLEDevice::init()` creates — `NimBLEDevice.cpp:884-888`, `:1009`). The most
likely reason a `start()` fails right after a link drops is `m_synced == false`,
which `NimBLEAdvertising::start()` rejects before doing anything else
(`.pio/libdeps/default/NimBLE-Arduino/src/NimBLEAdvertising.cpp:189-192`). The
event that sets `m_synced` back to true is dispatched **on that same task**
(`NimBLEDevice.cpp:835-864`, `onSync`). So a retry loop that sleeps on the host
task blocks the event it is waiting for: every attempt fails by construction.

That is what the code did before 2026-08-13: 5 attempts, `vTaskDelay(50 ms)`
between them, on the host task. The old wording of this section claimed the
retries turned a transient failure into a recovered link. They could not. The
mid-ride permanent-BLE-death incident (`docs/power-management.md`, "BLE stopped
accepting connections mid-ride") stayed possible, because nothing re-checked
advertising afterwards either — the screens never looked.

**Now.** `MapActivity::loop()` (`src/activities/map/MapActivity.cpp:1689`) and
`TileSyncActivity::loop()` (`src/activities/map/TileSyncActivity.cpp:319`) each
call `BlePositionServer::serviceAdvertising()` once per tick. Those loops run on
the Arduino `loopTask` (`src/main.cpp:522`), a different task from the NimBLE
host, so a retry there cannot block the sync event and can succeed once the host
has resynced. `serviceAdvertising()` is the single owner of per-tick advertising
work (`BlePositionServer.cpp:638-648`): it returns immediately when a central is
connected — `connIntervalUnits_ != 0`, the same "is the phone there" test the
map header draws with (`MapActivity.cpp:914`, `:1194`) — and otherwise calls
`retryAdvertising()`.

`retryAdvertising()` (`BlePositionServer.cpp:620-636`) is rate-limited to one
attempt per 1000 ms (`BlePositionServer.h:310`, `kAdvertisingRetryMs`), so a
radio that is dead for good costs one `start()` call per second rather than one
per loop tick. Success clears the flag and logs `LOG_INF`; failures log at
`LOG_DBG` so the serial log does not fill at 1 Hz.

The flag is also cleared on connect (`BlePositionServer.cpp:146`) — a central
that got in is proof advertising was up, and NimBLE stops advertising for the
duration of a connection by design — and on `begin()`/`end()`
(`BlePositionServer.cpp:333`, `:369`).

**NimBLE has a partial self-heal of its own, and it is not enough.** When the
host resyncs, `NimBLEDevice::onSync()` calls
`NimBLEAdvertising::onHostSync()`, which restarts advertising if the last
`start()` asked for an unlimited duration (`NimBLEAdvertising.cpp:279-290`) —
ours does. So the host-not-synced case would eventually recover without this
change, *provided* the host actually resyncs. It does nothing for the other ways
`start()` fails: the GATT server failing to start (`NimBLEAdvertising.cpp:200-204`),
advertisement or scan-response data failing to set (`:207-217`), or
`ble_gap_adv_start()` returning an error (`:236-239`). Those had no retry at all
before, and the flag covers them.

Verification status: **read off the code** (this repo plus the vendored
NimBLE-Arduino), cited above. **Not measured on hardware** — no host harness
reaches NimBLE, and the failure needs a `start()` that actually fails. What
would settle it: force a failure (drop the link while the host is resetting, or
temporarily make `onDisconnect` pass a bogus start) and watch for the `LOG_ERR`
followed by the `advertising restarted from the activity task` `LOG_INF`.

## Two-phase interval: fast for 30 s, then slow

Before 2026-08-13 the interval was never set, so the NimBLE host always
substituted its fast default (30–60 ms, next section) — for the whole time the
map screen is open, phone in range or not. A map screen left up with nobody
around advertises at that rate indefinitely, roughly 10x the TX events a
200–300 ms interval costs for the same "is anybody there" signal
(`docs/ble-review-2026-08.md`, "Power").

**Fast for the first 30 s, slow after.** `kFastAdvertisingMs` = 30000
(`BlePositionServer.h:420`). The window restarts — not just continues — at
three points, each one "advertising just started mattering again":

- `begin()` (`BlePositionServer.cpp:363`) — a freshly opened map screen gets
  the snappy interval.
- `ServerCallbacks::onConnect` (`BlePositionServer.cpp:152`) — a phone that
  connects, then drops again a few seconds later, gets a full fresh window on
  the way back out rather than resuming wherever the interrupted one left off.
- `ServerCallbacks::onDisconnect` (`BlePositionServer.cpp:199-219`) — reconnect
  UX stays snappy after any drop, whether the link had been idle 5 s or 5 min
  before it dropped.

`resetAdvertisingPhase()` (`BlePositionServer.cpp:698-701`) is the one place
that does this: stamps `phaseStartMs_` to `millis()` and clears
`advertisingSlow_`. `onDisconnect` also explicitly resets the NimBLE-level
bounds to `0`/`0` (`BlePositionServer.cpp:213-216`) before restarting —
`m_advParams` is set once on the advertising object and only ever touched
again by the slow-phase switch below or here, so a link that drops while slow
needs the fast bounds put back explicitly; there is no "restore defaults" call
in NimBLE-Arduino.

**The switch itself runs from `serviceAdvertising()`**
(`BlePositionServer.cpp:682-696`), the same per-tick hook that owns the
failed-restart retry above — one owner of advertising state, not two timers.
When nothing is connected and the fast window has elapsed,
`maybeEnterSlowAdvertising()` (`BlePositionServer.cpp:703-733`) sets
`setMinInterval(0x140)` / `setMaxInterval(0x1E0)` — 320 x 0.625 ms = 200 ms to
480 x 0.625 ms = 300 ms, the controller's advertising-interval field being in
0.625 ms units — then `stop()`s and `start()`s the advertising object, because
`NimBLEAdvertising::start()` is a no-op success while already advertising
(`NimBLEAdvertising.cpp:194-197`) and will not pick up new bounds without a
stop first. `advertisingSlow_` goes up before that restart is attempted, not
after, so a `start()` that fails here (the same transient "host not synced"
case the section above covers) falls through to the ordinary
`advertisingDown_`/`retryAdvertising()` path instead of retrying the whole
stop/set/start cycle every tick — and that ordinary retry already carries the
slow bounds, since nothing resets them until the next connect or disconnect.

**The wake-latency trade.** The signal the phone's background scan is waiting
for (`docs/ble-app-wake.md`) still has to land within a scan window once it
switches to the slow interval. Android's own docs for
`ScanSettings.SCAN_MODE_LOW_POWER` (the mode a background/opportunistic scan
uses) describe a duty cycle on the order of a few seconds on, tens of seconds
off — cited here as **read off Android's public scan-mode documentation, not
measured against this app's actual `CompanionDeviceManager` presence path**,
which does not expose its scan parameters to the app at all. A 200–300 ms
advertiser sends roughly 15-25 advertisements inside a several-second scan
window, which reads as comfortably enough to be caught — but "comfortably
enough" is a read of someone else's docs about a different scan mechanism, not
a number for this pairing. **Open — needs measurement**: map-open-to-`X4
appeared` latency once the device is actually advertising at the slow
interval. That is `docs/ble-fix-plan.md`'s H2 — phone paired, app killed, map
opened, five trials, median compared against 10 s.

## What iOS expects, and where we sit

**Primary source, read 2026-08-19**: Apple's
[Bluetooth Accessory Design Guidelines Q&A QA1931](https://developer.apple.com/library/archive/qa/qa1931/_index.html).
It matters because `CLAUDE.md` makes iOS a target for the phone client, and an
iOS central may simply refuse a parameter set outside these rules.

Advertising, per Apple: 20 ms for at least the first 30 s, then one of
**152.5, 211.25, 318.75, 417.5, 546.25, 760, 852.5, 1022.5, 1285 ms**.

Ours: the fast phase leaves both bounds at 0 so the host substitutes
`BLE_GAP_ADV_FAST_INTERVAL1` = 30-60 ms (`BlePositionServer.cpp:217-218`), and
the slow phase is 200-300 ms (`kSlowMinIntervalUnits`/`kSlowMaxIntervalUnits`,
`BlePositionServer.h:474-479`). Neither is on Apple's list, though 200-300 ms
straddles the sanctioned 211.25 ms. Not known to break anything; worth aligning
when the intervals are next touched -- 211.25 ms for the ordinary slow phase, and
1022.5 ms is the value to reach for if a parked screen ever advertises for hours
([`power-idle-sleep.md`](power-idle-sleep.md)).

Connection parameters, per Apple: `Interval Min` >= 15 ms in multiples of 15;
`Interval Min + 15 ms <= Interval Max`; `Interval Max * (Slave Latency + 1) <=
2 s`; `Interval Max * (Slave Latency + 1) * 3 < connSupervisionTimeout`;
`Slave Latency <= 30`; and **2 s <= connSupervisionTimeout <= 6 s**.

Our idle set is 30-50 ms with latency 4 and a supervision timeout of **20 s**
(`BlePositionServer.h:553-569`, `:591`). Every rule passes except the last:
**20 s is over Apple's 6 s ceiling.** The timeout was raised deliberately to
survive mid-ride dropouts, so this is a real trade-off and not an oversight --
but it is the one parameter an iOS client may reject the whole set over, and a
rejected set means the phone keeps its own defaults rather than ours. Logged in
the parent repo's `docs/BUGS.md`.

**Caveat: QA1931 is in Apple's archive.** The current source is the Accessory Design
Guidelines for Apple Devices PDF, which nobody has checked against these numbers. The
2-6 s range and the advertising grid are quoted from the archived Q&A -- treat them as
the right shape and verify the exact values before changing a parameter on their
authority. **[open]**

**Not measured against an iPhone.** Nobody has connected an iOS central to this
firmware. What is above is the documented rule and the code's values, nothing more.

## What is in the payload

Read off the code, 2026-08-11. Advertisement:

| Field | Bytes | Source |
|---|---|---|
| Flags | 3 | `NimBLEAdvertising` constructor, `BLE_HS_ADV_F_DISC_GEN` |
| 128-bit service UUID | 18 | `advertising->addServiceUUID(kServiceUuid)` |

That is 21 of the 31 available bytes. The **name goes in the scan response**, not
the advertisement — `XteinkX4Map` needs 13 more bytes and does not fit
(`BlePositionServer.cpp:302-313`). An active scan reads the scan response, which
is what Android does, so a phone still sees the name
(`BlePositionServer.cpp:314-315`).

**Both of those lines are set explicitly and have to be.** NimBLE 2.x defaults
scan response *off* (`NimBLEAdvertising.cpp:44`, `m_scanResp{false}`) and
`NimBLEDevice::init(name)`'s name only reaches the GAP Device Name
characteristic, readable after connecting. Before 2026-08-11 this device
therefore advertised **no name at all**, and the phone app's
`ScanFilter.setDeviceName("XteinkX4Map")` filter could never match — only its
service-UUID filter did. Nothing looked broken because the two filters are OR'd.

Advertising interval: **30–60 ms for the first 30 s, 200–300 ms after** — see
"Two-phase interval" above for the full mechanism. The fast number comes from
leaving both bounds at `0`, which makes the NimBLE host substitute
`BLE_GAP_ADV_FAST_INTERVAL1` (`ble_gap.c:3401`, `ble_gap.h:59`); the slow
number is `setMinInterval(0x140)`/`setMaxInterval(0x1E0)` explicitly.

Connection mode is undirected connectable, general discoverable, no pairing and
no bonding (`BlePositionServer.cpp:248`).

## What the open channel now demonstrably allows

"No pairing and no bonding" above is a one-line fact with consequences that used
to be theoretical. **They are not any more.** On 2026-08-23 the whole BLE surface
was driven for the first time, in the desktop simulator (`simulator.md`, "BLE"),
by a client that could put arbitrary bytes on every characteristic. **Nothing ran
on hardware; there is no device.** Everything below is therefore
measured-in-the-simulator or read-off-the-code, and the transport is a TCP socket
rather than a radio -- but the reachability argument does not depend on the
transport. It depends on this section: an undirected connectable, generally
discoverable peripheral with no pairing and no bonding accepts a write from
whoever connects first.

The threat model is the standing one: the device is carried on a bike or in a
bag, it gets lost or stolen, and while it is in someone else's hands -- or merely
in radio range -- they can write anything a phone could.

What it **leaks** to an unauthenticated peer, with no ownership check:

- The rider's current position, heading, speed and clock, and the tile
  coordinates around them, straight out of `info`. A tile coordinate is a
  location.
- The list of tiles the device is missing and the content id of every tile it
  holds (`missing`, `have`), which is a coarse history of where the device has
  been asked to draw.

What it **lets someone do to the device or the rider**:

- **Kill the map screen from outside.** One 21-byte position packet with
  `lat = 90` sends the renderer into a 16.7-million-tile scan that the activity
  loop never returns from: no frame, no redraw, no button, and the BLE link
  stays up so the device still looks connected. Measured in the simulator;
  mechanism and citations in
  [`map-follow.md`](map-follow.md), "Nothing validates a position packet".
  Treat it as a remotely reachable denial of service on the navigation screen.
- **Inject a fake position, including a replayed real one.** The redraw gate
  tests "changed", not "newer", so a packet recorded off the air and written back
  later moves the rider's displayed position to wherever it was recorded. Same
  doc.
- **Make the command channel stop answering, with no trace.** A single write
  longer than the 256-byte command ring
  (`lib/BlePositionServer/include/BlePositionServer.h:397`) is discarded whole
  with **no `ERR`, no reply and no log line**
  (`lib/BlePositionServer/src/BlePositionServer.cpp:507-523`). Measured with a
  300-byte write at MTU 517, which is a perfectly legal single ATT write --
  nothing about it is malformed at the link layer. The peer then waits forever.
  Compare the line-length overflow, which does answer (`ERR line_too_long`), and
  the position characteristic, which drops a wrong-length write silently but at
  least cannot be mistaken for a request. Dropping the write whole rather than
  truncated is right, and is explained in the code; the silence is the gap.
- **Freeze the whole main loop for over a minute.** A peer that subscribes to
  the command channel, issues one `info` at the default MTU and then never
  acknowledges anything holds the activity task for a measured **69.1 s**, with
  no `loop()` iteration for 63 s of it -- so no buttons either. Details in
  [`tile-freshness.md`](tile-freshness.md), "The cap does not bound the freeze".
- **Write any file under the SD root**, `pins/pins.log` included. The path check
  holds against every escape tried, so this is the documented design rather than
  a hole -- but the design is that the channel carries any file, and the channel
  has no ownership check.
- **Occupy the transfer slot**, making the rider's own phone see `ERR busy` for
  30 s, and **create directories without bound** by sending `begin` frames that
  never write a byte. Both in
  [`missing-tiles.md`](missing-tiles.md), "The transfer path, driven end to end".

**None of this is new in kind** -- `MapCommandParser.h` already documents the
serial and BLE channels as one shared, unauthenticated grammar, and T-222 tracks
the audit. What changed is that the list is now specific, cited, and reproducible
without a device, which is what a fix has to be measured against.

**What a hardware pass would still have to settle:** whether the ATT bearer
refuses any of these earlier than the firmware does (it cannot refuse the
position packet, which is a legal 21-byte write, and it cannot refuse a 300-byte
write at MTU 517), and whether the watchdog fires during the map wedge before a
rider gives up and resets.

## The name is wire-visible, and per board since 2026-09-01

`bleDeviceNameForActiveBoard()` (`lib/BlePositionServer.cpp`) picks the
advertised name from `BoardConfig::ACTIVE.board`: `XteinkX4Map`, `XteinkX3Map`
(X3 and the newer Uc8279 panel both read as this -- same device to the phone),
`XteinkX4ProMap`, `LilyGoT5S3Map`, or `ExplorInkMap` for a board this function
does not know. Before this it was one hardcoded `kBleDeviceName`
(`"XteinkX4Map"`) for every board, so an X3 or a T5S3 Pro on the test bench
advertised as an X4. Every name here is matched by Android's
`BleLink.KNOWN_DEVICE_NAMES` (`android/`) and shown to the rider in Android's
companion-device pairing dialog -- adding a board on either side means adding
its name on the other in the same change.

**Every device of the *same* board still advertises the same name and the same
service UUID.** Nothing on the air distinguishes one X4 from a second X4 in
range -- the per-board name above answers "what kind of device is this",
not "which one". The app solves the latter by pinning the MAC address it
paired with; the device side has no per-unit identity to offer yet. Open: an
owner-settable device name would fix that at the source and would make
Android's pairing dialog readable when two devices of the same board are
present.

## Verification status

- Payload contents, interval defaults, name placement: **read off the code**
  (this repo and the vendored NimBLE), cited above.
- **The name reaches an active scan — measured on hardware 2026-08-11.** Flashed
  this build, opened the map with `CMD:GOTO_MAP`, scanned from the laptop with
  bleak after dropping BlueZ's cache entry for the device
  (`bluetoothctl remove`, so a cached name could not be mistaken for a fresh
  read). One device answered: `14:63:93:F4:8A:36  local_name='XteinkX4Map'
  rssi=-53  uuids=['5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0001']`. Before this change
  the same scan returned the service UUID and `local_name=None`.
- The name showing in Android's companion pairing dialog: **not measured**. Needs
  a phone. Same scan data underneath, so it is likely, not proven.
- Per-board naming (`bleDeviceNameForActiveBoard()`): **read off the code and
  built for the `simulator` env** (device build failed on an unrelated
  toolchain issue, `esp8266-compat.h`/`base64.h` missing from the whole
  `framework-arduinoespressif32` package, not from this change). **Not
  measured on any real board yet** -- needs an X4, an X3, an X4 Pro and a
  LilyGo T5S3 each scanned and each connecting from the Android app with
  `KNOWN_DEVICE_NAMES`.
