# The transfer receiver's threading

`MapTransferReceiver` takes files over BLE (tiles, point shards, index blocks,
wallet assets, the wallet manifest). Frames and the disconnect arrive on the
NimBLE host task. `detach()` runs on the activity task, and it is the one caller
outside the host task that writes transfer state, because tearing a transfer down
is its job.

**On `env:default` (C3) the two tasks share one core. On `env:x4pro` and
`env:t5s3pro` they do not.** `CONFIG_FREERTOS_UNICORE` is unset there and NimBLE
is pinned to core 0 against the Arduino task on core 1. Code in this file was
first written for the C3, and comments that justified unsynchronised state with
"the ESP32-C3 is single core" were wrong for both S3 boards (T-2104).

## The gate: `src/activities/map/MapTransferGate.h`

- A frame or a disconnect takes a `FrameGate::Ticket` first. The ticket marks
  "inside", then asks "attached?", and backs out if not.
- `detach()` clears the hooks, then `close()`s the gate: "not attached", then
  waits while anything is "inside", up to `kDetachWaitMs` (500 ms).
- The destructor calls `detach()` while still attached.

**Both store-then-load pairs are `seq_cst`, and must stay so.** Each side writes
one flag and then reads the other: Dekker's shape. Release/acquire does not stop
a store being overtaken by a later load of a different variable (C++20
[atomics.order]), so under acq/rel both sides can miss each other and a frame runs
while `detach()` abandons under it.

Host-tested with two threads, `test/map_transfer/MapTransferGateTest.cpp`,
`ConcurrentCloseNeverOverlapsAFrame`: one host thread for the whole run against
open/close cycles for 4 s. Both broken orderings are mutants in
`test/mutation/t2104.json`, run by `scripts/mutation_check.py`. Measured
2026-11-28 on this 14-core laptop: the shipped gate passes; the release/acquire
mutant and the old order (check "attached", then mark "inside") fail every run
unpinned and pinned to two physical cores (`--cpus 0,2`). Pinned to two
hyperthreads of one core (`0,1`) the release/acquire mutant failed 0 of 10 runs,
so a two-CPU check must use two cores. The test **skips on fewer than two usable
CPUs**, where nothing can reorder, and when fewer than 200,000 cycles fit in its
budget on a loaded host.

## `active_` is atomic

Every blocking call in `handleChunk` is followed by a re-check of `active_`. That
is how a frame usually notices a timed-out `detach()` abandoned the transfer under
it -- usually, because each re-check is a load followed by an action, and
`abandon()` can land between them. A
plain `bool` there was a bet that the compiler reloads the member after a call
into another translation unit.

## The wallet key is borrowed, never pointed at

The sealers' key comes from `wallet::KeyCopy`, a copy taken under the lock
`Session::clear()` takes. `docs/wallet-crypto.md`, "How a caller borrows K".

## What is not closed

Both predate the gate.

- **The wait has a bound.** On a wedged card `detach()` gives up after
  `kDetachWaitMs` and abandons with a frame or a disconnect still inside. The
  `active_` re-checks make a frame bail in most of that case, not all of it: an
  `abandon()` that lands between a re-check and the next `file_` call leaves the
  frame writing to a closed file. A disconnect's `abandon()` stuck behind
  the card can still run at the same time as `detach()`'s: two closes of one
  file, `failed_` counted twice. The bound was meant to keep a stuck card from
  holding the screen. **It does not**: both owners call `BlePositionServer::end()`
  right after `detach()`, and `end()` reaches `nimble_port_stop()`, which waits
  with `BLE_NPL_TIME_FOREVER` until the host task services the stop -- after the
  frame in flight returns (NimBLE-Arduino,
  `src/nimble/porting/nimble/src/nimble_port.c`). So the screen exit is held
  anyway, and the overlap past the bound buys nothing. Read off the code, not
  measured. Whether to drop the bound is T-2112 in the parent repo's TODO.
- **A hook can arrive after `detach()` returned.** `BlePositionServer` copies the
  hooks under its lock and calls them after releasing it
  (`lib/BlePositionServer/src/BlePositionServer.cpp`, `onCentralDisconnect` and
  the frame path). A call copied before `setTransferHooks({})` but not yet inside
  the gate when `close()` looked reaches the receiver late, possibly after its
  owner freed it. Only the owner calling `BlePositionServer::end()` before the
  receiver dies closes that window: `end()` waits for the host task without a
  bound, so a dispatched hook has returned by the time it does.
  `MapActivity` and `TileSyncActivity` both call it.

**Verified on the host only.** No board has run a `detach()` against a frame in
flight on purpose.
