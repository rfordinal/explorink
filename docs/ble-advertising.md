# BLE advertising: what the device puts on the air, and when

What a phone can see of this device before it connects. The wire contract for
what happens *after* connecting is `../../docs/ble-map-transfer-protocol.md` in
the parent repo.

Code: `lib/BlePositionServer/src/BlePositionServer.cpp`.

## When it advertises

Only while the **map screen** or the **sync map tiles** screen is open.
`MapActivity::onEnter()` calls `BlePositionServer::begin()`
(`src/activities/map/MapActivity.cpp:1126`), which builds the GATT table and
starts advertising (`BlePositionServer.cpp:279-296`). `end()` stops it and
deinits the stack (`BlePositionServer.cpp:307`), because the NimBLE host plus
controller is 64.5 KB of heap — measured, `map-memory.md`.

So advertising is not a background beacon. It is a statement that the rider is
looking at the map right now, and the phone app treats it as one: the OS watches
for this device's advertisement and starts the app's bridge service when it
appears (`../../docs/ble-app-wake.md`).

Advertising restarts on disconnect (`BlePositionServer.cpp:172-181`) so a link
dropped mid-ride can come back without leaving the screen.

## What is in the payload

Read off the code, 2026-08-11. Advertisement:

| Field | Bytes | Source |
|---|---|---|
| Flags | 3 | `NimBLEAdvertising` constructor, `BLE_HS_ADV_F_DISC_GEN` |
| 128-bit service UUID | 18 | `advertising->addServiceUUID(kServiceUuid)` |

That is 21 of the 31 available bytes. The **name goes in the scan response**, not
the advertisement — `XteinkX4Map` needs 13 more bytes and does not fit
(`BlePositionServer.cpp:282-292`). An active scan reads the scan response, which
is what Android does, so a phone still sees the name.

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
no bonding (`BlePositionServer.cpp:227`).

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
- Scan response actually reaching an Android scan, and the name showing in the
  pairing dialog: **not measured yet**. Settled by pairing a phone against a
  device running this build.
