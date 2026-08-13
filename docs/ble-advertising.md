# BLE advertising: what the device puts on the air, and when

What a phone can see of this device before it connects. The wire contract for
what happens *after* connecting is `../../docs/ble-map-transfer-protocol.md` in
the parent repo.

Code: `lib/BlePositionServer/src/BlePositionServer.cpp`.

## When it advertises

Only while the **map screen** or the **sync map tiles** screen is open.
`MapActivity::onEnter()` calls `BlePositionServer::begin()`
(`src/activities/map/MapActivity.cpp:1314`), which builds the GATT table and
starts advertising (`BlePositionServer.cpp:295-320`). `end()` stops it and
deinits the stack (`BlePositionServer.cpp:351`), because the NimBLE host plus
controller is 64.5 KB of heap — measured, `map-memory.md`.

So advertising is not a background beacon. It is a statement that the rider is
looking at the map right now, and the phone app treats it as one: the OS watches
for this device's advertisement and starts the app's bridge service when it
appears (`../../docs/ble-app-wake.md`).

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

Advertising interval: **30–60 ms**, not set here. `m_advParams` is
zero-initialised and the NimBLE host substitutes `BLE_GAP_ADV_FAST_INTERVAL1`
when both bounds are 0 (`ble_gap.c:3401`, `ble_gap.h:59`). Fast enough for a
phone-side low-power scan to notice within seconds; measuring the actual
discovery latency is open.

Connection mode is undirected connectable, general discoverable, no pairing and
no bonding (`BlePositionServer.cpp:248`).

## The name is wire-visible

`kBleDeviceName` (`lib/BlePositionServer/include/BlePositionServer.h`) is matched
by the phone app and shown to the rider in Android's companion-device pairing
dialog. Changing it breaks an installed app's name filter and makes an existing
pairing look like a different device. Change it only with the app.

**Every X4 running this firmware advertises the same name and the same service
UUID.** Nothing on the air distinguishes one device from another, so the phone
cannot tell the rider's X4 from another one in range by advertisement alone. The
app solves this by pinning the MAC address it paired with; the device side has no
per-device identity to offer yet. Open: an owner-settable device name would fix
it at the source and would make Android's pairing dialog readable when two X4s
are present.

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
