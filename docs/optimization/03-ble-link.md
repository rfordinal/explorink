# Plan 03 — BLE link

The link carries three things: position packets, command lines, and map files.
The first two work and are cheap. The third is the one with headroom.

## What is already right

Worth stating so nobody "fixes" it:

- **Indication, not notification, for replies.** A notification is
  unacknowledged, so a burst can be silently dropped by the controller while
  `notify()` still returns true — confirmed on hardware, 18 of 18 calls
  succeeded and the client saw two (`BlePositionServer.cpp:174-188`). The
  confirm wait in `sendCommandReply()` (`:358-368`) is what serialises replies
  at the link's real pace.
- **Write-with-response as flow control on the transfer channel.** The ATT
  response is sent only after the callback returns, and the callback returns
  only after the bytes are on the card, so the sender physically cannot outrun
  the SD write (`BlePositionServer.cpp:80-91`, `MapTransferReceiver.h:38-41`).
- **Hooks copied out under the critical section before being called**, so a
  hook cannot be swapped between the null check and the call
  (`BlePositionServer.cpp:388-395`).
- **CRC verified by reading the finished file back off the card**, not
  accumulated from arriving chunks, so a card that accepted a write and lost it
  is caught (`MapTransferReceiver.cpp:375-398`).

## The throughput picture

**read.** The preferred ATT MTU is 256 (`sdkconfig.defaults:752`,
`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256`), and the ACL buffer is 255
(`:3455`). So the device is willing to carry a 253-byte ATT write, which after
the 5-byte chunk header (`0x02` + `u32 offset`, `MapTransferReceiver.h:23`)
means a 248-byte payload per frame.

But **the central initiates the MTU exchange, and nothing on the device
requests or logs it.** `begin()` never calls `NimBLEDevice::setMTU()`
(`BlePositionServer.cpp:132-225`), and `ServerCallbacks` implements only
`onDisconnect` (`:109-122`) — no `onConnect`, no `onMTUChange`, so the
negotiated MTU never reaches the log.

Consequences, all **read**:

- If the phone app does not request a larger MTU, the link runs at the 23-byte
  default: 20 bytes of ATT payload, **15 bytes of file payload per write**.
- Write-with-response is one ATT transaction at a time, so throughput is
  roughly `payload / connectionInterval`. Android's post-connection interval is
  typically 30-50 ms unless the app asks for high priority.

Order-of-magnitude, **open** until measured on the real app:

| MTU | payload/frame | at 40 ms | 30 KB tile |
|---|---|---|---|
| 23 (default) | 15 B | ~375 B/s | ~80 s |
| 256 | 248 B | ~6.2 kB/s | ~5 s |
| 256 + 15 ms interval | 248 B | ~16 kB/s | ~2 s |

That is the difference between a usable "fill the gaps" button and one nobody
presses twice. The phone app does not exist yet, which makes this the right
moment to pin it down.

## Step 1 — see the link's real parameters

Add to `ServerCallbacks` (`BlePositionServer.cpp:109`):

```cpp
void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
  LOG_INF("BLEPOS", "connected: interval %u units (%u ms), latency %u, timeout %u",
          info.getConnInterval(), info.getConnInterval() * 5 / 4,
          info.getConnLatency(), info.getConnTimeout());
}
void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
  LOG_INF("BLEPOS", "MTU now %u, file payload %u", mtu, mtu - 3 - 5);
}
```

Both hooks exist in this NimBLE version (`NimBLEServer.h:177` for
`onMTUChange`). Nothing else in this plan is worth doing before these two lines
are on the device, because everything else is a guess without them.

Also expose the negotiated MTU on the console — one more `info` line. It is the
number a phone-side developer needs and cannot see from their end.

## Step 2 — ask for the link we want

Three calls, all available in this NimBLE version:

- `NimBLEDevice::setMTU(256)` in `begin()` (`NimBLEDevice.h:156`). The
  peripheral cannot force an exchange, but this makes the device's preference
  explicit rather than inherited from a config file two repos away.
- `NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK)`
  (`NimBLEDevice.h:163`). 2M PHY halves air time per frame. Both ends must
  support it; the negotiation falls back on its own.
- `NimBLEServer::updateConnParams(handle, 12, 24, 0, 400)`
  (`NimBLEServer.h:69`) — 15-30 ms interval, 4 s supervision timeout —
  **while a transfer is running, and only then.** A 15 ms interval costs radio
  current continuously; the map screen already prevents sleep (see plan 07), so
  this is a real battery item and must be scoped to the transfer, not left on.

Where to put the scoping: `MapTransferReceiver::handleBegin()` requests the
fast params on accept, `handleChunk`'s completion path and `abandon()` restore
the slow ones. Both run on the NimBLE host task, which is where the server
object lives, so no cross-task call is added.

**open**: whether Android honours the peripheral's request. Android usually
ignores peripheral-initiated parameter updates and expects the app to call
`requestConnectionPriority(CONNECTION_PRIORITY_HIGH)` itself. So this belongs in
the phone app's brief as well — record it in `docs/ble-map-transfer-protocol.md`
(parent repo) as a requirement on the central, not only as a device-side call.

## Step 3 — stage chunks into a sector-sized buffer

Today every chunk is written straight to the card
(`MapTransferReceiver.cpp:243`). At a 15-byte payload that is 34 partial-sector
writes per 512-byte sector. SdFat buffers internally, so this is not 34 card
transactions — but it is 34 trips through the SdFat write path, each taking the
`HalStorage` mutex (firmware `CLAUDE.md`, HAL section), on the NimBLE host task.

Change: a 512-byte staging buffer in `MapTransferReceiver`, flushed when full
and at completion. 512 bytes as a member, not a stack local — the host task has
5120 bytes of stack (`sdkconfig.defaults:700`) and already carries NimBLE's own
512-byte VLA plus SdFat's write path (`MapTransferReceiver.cpp:174-179`).

What this costs: the ATT write response stops meaning "the bytes are on the
card" and starts meaning "the bytes are accounted for". That guarantee is what
`MapTransferReceiver.h:38-41` sells the design on, so the header comment has to
change with the code.

Why it is still safe: the completion check reads the whole file back off the
card and CRCs it (`:375-398`), and nothing is renamed into place until that
matches (`:262-278`). The end-to-end guarantee is the CRC, not the per-chunk
response. Flow control also survives: the response is still withheld until the
callback returns, and the callback still does the SD write on every 512 bytes.

Do this **after** step 1-2. With a 248-byte payload the buffer saves 1 write in
2; with a 15-byte payload it saves 33 in 34. The size of the win depends on the
MTU, so measure the MTU first.

## Step 4 — bigger read-back buffer for the completion CRC

`computePartCrc32` reads in 128-byte chunks (`MapTransferReceiver.cpp:34`,
`:382`). For a 30 KB tile that is 240 read calls, each taking the storage
mutex. 128 was chosen to stay under CLAUDE.md's 256-byte stack cap
(`:32-34`), which is the right reason for a stack local.

Make it a 512-byte member instead — same object that already holds two ~128-byte
path buffers (`MapTransferReceiver.h:171-172`). One sector per read, 60 calls
instead of 240, no stack growth.

## Step 5 — tell the rider the link is up

**Feature gap, not an optimisation.** Nothing on the map screen says whether a
phone is connected. The debug readout shows transfer counters only after a file
has moved (`MapActivity.cpp:1051`, `MapTransferReceiver.cpp:423-438`), and the
sync screen discovers "nobody is listening" only when its `NEED_TILES`
indication fails (`TileSyncActivity.cpp:98-104`).

A rider whose phone dropped off sees a map that simply stops updating. That is
the same picture as "no GPS fix", "app crashed" and "everything is fine, you are
stationary".

Cheapest honest version: reuse the compass corner — a small filled dot when
`BlePositionServer` has a live connection, nothing when it does not. It costs
one more piece of the frame that is already being drawn (`drawCompass` at
`MapActivity.cpp:298`) and no extra refresh.

Needs one new thing in `BlePositionServer`: a connection count. `onConnect` /
`onDisconnect` already exist (step 1 adds the first), so it is a `volatile
uint8_t`.

## Step 6 — security note, recorded not fixed

**read.** There is no pairing and no bonding (`BlePositionServer.cpp:155-159`),
by a documented decision — short range, low stakes, and it keeps the phone side
to "connect and write". For position packets that is a defensible trade.

The transfer channel changes the stakes: **any unpaired central in range can
write files under `/trailink` on the SD card while either the map screen or the
tile-sync screen is open** — both attach a receiver (`MapActivity.cpp:385`,
`TileSyncActivity.cpp:42`). The
path validator is solid — no absolute paths, no `..` component, no empty
component, printable ASCII only, length-bounded
(`MapTransferReceiver.cpp:338-373`) — so the blast radius is "junk files inside
the map root", not "arbitrary filesystem write". A wrong-version or corrupt tile
is also caught on open (`MapTileReader.cpp:85`) and hatches rather than drawing
garbage.

So this is a nuisance ceiling, not a takeover. Two cheap mitigations, if it ever
matters:

- Accept a begin frame only while the tile-sync screen is up. After `412e0ed9`
  that is nearly free: sync is its own activity with its own receiver
  (`TileSyncActivity.cpp:42`), so dropping `transfer_` from `MapActivity`
  (`MapActivity.h:294`) closes the window and deletes code. It also settles plan
  04's item 1, which is open for the same reason. The cost: a phone can no longer
  push a corridor update while the rider is looking at the map.
- Restrict the accepted extension set to what the device reads (`.tib`).

Decide deliberately. Do not add pairing to solve this — pairing adds a phone-side
UI flow the design explicitly bought out of.

## Step 7 — small things

- `g_indicateAckSem` is created in `begin()` and never deleted in `end()`
  (`BlePositionServer.cpp:151-153`, `:227-259`). Deliberate reuse across
  sessions, not a leak — one binary semaphore, created at most once. Worth one
  comment saying so, because it reads like a leak.
- `hasUpdate_` is never cleared once set (`:294`), so `getLatest()` returns the
  same packet on every `loop()` and `MapActivity` de-duplicates by `seq`
  (`MapActivity.cpp:521`). Correct, and cheaper than clearing under the
  critical section. Also means a phone that stops updating `seq` goes
  undetected — see step 5.
- `sendCommandReply` can block for up to `kMaxAttempts * kRetryDelayMs` plus the
  confirm timeout (`:355-368`): 1 s of retries plus 500 ms. A 20-line `missing`
  page is therefore up to ~10 s of held `loop()` worst case. The page size is
  already capped at 20 for exactly this reason
  (`MapCommandConsole.h:217-223`). No change; recorded so the next person
  raising the page size knows what they are buying.
- The command ring is 256 bytes and drops an over-long write whole rather than
  truncating (`BlePositionServer.h:168`, `.cpp:298-315`). Right call — half a
  command can still parse.
