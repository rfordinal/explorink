# Pins, as built

User pins on the device: base, parking, destination, meet, camp, `#1`-`#5`. What
the rider needs a direction and a distance to right now, not a POI database.

The requirement, the decisions behind it and the phase plan are in
[`pins-plan.md`](pins-plan.md). This file is the mechanism as built. Every claim
here is **read off the code** unless it says otherwise; nothing in the pins path
has been measured on hardware yet.

Status, 2026-08-17: **phase 1 built** -- the store, the record format, the log and
the `pin` console commands. No UI, nothing drawn. 34 host tests
(`test/pins/`).

## The files

| file | what | pure? |
|---|---|---|
| `src/activities/map/PinCatalog.h` | the type table, `kPinSlotCount`, key lookup, key validation | yes |
| `src/activities/map/PinRecord.{h,cpp}` | one log line: encode, decode, CRC, version | yes |
| `src/activities/map/PinStore.{h,cpp}` | the active pins and the mutations that produce records | yes |
| `src/activities/map/PinLogScanner.{h,cpp}` | bytes to lines to records, and the replay | yes |
| `src/activities/map/PinLog.{h,cpp}` | the only file that touches the card | no (HalStorage) |
| `src/activities/map/MapPins.{h,cpp}` | log-then-apply, and the console's `IMapPinsSource` | no (Arduino) |

"Pure" means no Arduino, no HAL, no SD -- host-testable, and tested
(`test/pins/PinsTest.cpp`). `PinLog` is the exception on purpose: keeping the
parsing out of it is what makes the damage rules testable with no card attached.

## The catalogue is soft, the log is hard

`kPinCatalog` (`PinCatalog.h:39`) is `constexpr`, so it lands in flash. Adding a
type is one row; reordering and relabelling are free.

The log stores the **stable text key**, never an index into that table
(`PinRecord.h:26`). An index would change the meaning of every old record the day
a row is inserted.

A record also carries a monotonic `id` that is never reused. The key says *what
type*, the `id` says *which pin* -- so a Camp deleted and remade is two pins to
whoever reads the log later, while a Camp *replaced* keeps its id and is one pin
that moved (`PinStore.cpp`, `makeSetRecord()`).

**Unknown keys load.** A record written by a later firmware lands in one of four
held slots (`PinCatalog.h:55`), keeps its raw key, and stays deletable. Never
silently dropped: an update that eats a rider's camp is the failure this feature
exists to prevent.

## Active state

Fourteen fixed entries (`kPinMaxEntries`) -- ten catalogue slots plus the four
for foreign keys -- about 32 bytes each, inside `MapActivity` (`MapActivity.h`,
`pins_`). No heap, no `std::vector`: the count is bounded by the catalogue and
cannot grow at runtime.

**Derived, never stored.** `onEnter()` replays the log
(`MapActivity.cpp`, `pins_.begin()`). One source of truth on the card, so the
state and the history cannot disagree.

Firmware RAM after phase 1: 17.8 % (58,300 bytes), unchanged from `a45efe5a` --
the store is inside the heap-allocated activity, not static.

## The log on the card

`/trailink/pins/pins.log`. The `/trailink` prefix stays: the on-card layout is
infrastructure not yet renamed (parent `CLAUDE.md`, Naming).

One record per line, ASCII, `\n`-terminated:

```
v1|<seq>|<utc>|<uptime>|<op>|<key>|<id>|<latE7>|<lonE7>|<trip>|<crc32>
```

Real line (the record `test/pins/PinsTest.cpp`'s shape test builds; the checksum
checked against `zlib.crc32` of the same bytes):

```
v1|4|1755400000|60000|rep|parking|2|484372000|170186000||0afa87b9
```

- `utc` **0 means the device had no clock**, never a fabricated time. The device
  has no RTC; the clock arrives in the phone's BLE packet, and
  `BlePositionServer::utcNow()` (added for this) answers it unshifted. The header
  draws *local* time and must not be the source here -- local seconds in a `utc`
  field is a lie that only shows up months later.
- `uptime` is milliseconds since boot. It is what orders two records inside a run
  that never had a clock.
- `op` is `add` | `rep` | `del` | `res`. `res` is reserved for the deferred
  Restore UI and is never written yet.
- `latE7`/`lonE7` are int32 1e7 fixed point -- the same units the BLE packet and
  `MapCommandParser` already use, so nothing on the path from a command to a
  record needs a float.
- `trip` is reserved and always empty. A newer firmware's trip id is carried by
  the CRC and ignored, so it cannot make this build skip a record.
- `crc32` is `MapCrc32` (the zlib polynomial already used by tiles and routes),
  over every byte **before the final separator**, 8 lowercase hex digits.

ASCII rather than binary, deliberately: the card gets read on a laptop, and
`/trailink/power.csv` set that precedent.

### Writing

One line per user action, then `flush()` (`PinLog::append()`). Pin actions are
rare and each one *is* a change, so this does not violate write throttling
(`CLAUDE.md`, Resource Protocol 8 -- that rule forbids writing on every
interaction, not on every real change).

`O_CREAT` does not create the parent, so `append()` calls
`ensureDirectoryExists()` first when the file is missing -- the same trap
`PowerLog` hit on a card that has never had tiles pushed to it.

**The log is written first; the active set moves only if that worked**
(`MapPins::pinSet()`). The opposite order can leave RAM claiming a pin the card
never recorded, and a replay cannot repair that -- the rider would see a Camp
until the next reboot and then lose it with no trace.

A replay that failed (no card) also **refuses every save**. Appending onto a
history that was never read would renumber `seq` and `id` from 1 and overwrite
the meaning of what is already there.

### Reading, and damage

Streamed with one fixed line buffer, never whole-file into RAM. `PinLog` reads
64-byte chunks so the frame's locals stay inside the 256-byte stack rule.

- A line with a bad CRC, a bad field count, an unknown version or an unknown op
  is **skipped**, counted, and the reader carries on.
- A line longer than a record can be is swallowed to its own line end. The record
  after it still lands (tested).
- A final line with no terminator is a torn write and is **discarded**. Half a
  record can pass a field count and would then be applied as if it were whole.
- **A damaged record never invalidates the log.** `seq` continues past the
  records this build refused, so a later append cannot reuse one.

Skips are counted in `PinReplayStats` and logged with the applied count and the
active total (`PinLog::replay()`).

### Growth

About 70 bytes a record. Rotation and compaction are deferred -- the streaming
reader means length costs time, not memory. Still an open item: at what size, and
whether it ever discards.

## Console commands

Added to the existing grammar (`MapCommandParser.h`), so the USB serial console
and the BLE command characteristic cannot drift.

```
pin set <key> <lat> <lon> [<utc>]   create or replace, from anywhere
pin del <key>
pin list
pin log [<offset>]                  paged history, newest first
```

The key is checked against the catalogue **in the parser**, so a typo answers
`ERR unknown_pin` instead of occupying one of the four foreign-key slots.

Replies:

```
> pin set camp 48.4372 17.0186
INFO pin_set=camp
OK

> pin list
INFO pins_total=1
INFO pin_camp=48.4372000,17.0186000,0,1        lat,lon,utc,id
OK

> pin log
INFO pinlog_total=10
INFO pinlog_offset=0
INFO pinlog_10=rep,camp,48.5000000,17.1000000,0   seq=op,key,lat,lon,utc
...
INFO pinlog_next=8
OK
```

- A refused write answers `ERR pin_write` **and no `OK`** -- `ERR` is the
  terminator, and a sender reading one terminator per command must not see both.
  Deleting a pin that is not there answers the same way; `pin list` says which of
  the two it was.
- `pin log` pages eight records at a time (`MapConsoleState::kPinLogPageSize`),
  for the reason `missing` is paged at all: every reply line is one BLE
  indication and each waits for the peer's ATT confirm.
- Totals print before entries. Fetching the total without the entries is
  `pinLogPage(offset, 0, ...)` -- one extra scan of a small file, against holding
  a page of records in RAM.
- `pin set` and `pin del` return "redraw" from the console, because a pin is
  drawn in the map's own pass. Nothing draws one yet (phase 4).

Newest-first paging costs two scans plus a short read per printed line
(`PinLog::page()`): one pass counts the valid records, one notes the byte offset
of each line in the window, then each line is read back at its offset. The file
only reads forwards, and 4 bytes an offset is cheaper than 44 bytes a record.

## What is not built yet

Phases 2-7 of `pins-plan.md`: the `OptionPopup` row actions, the Pins UI, the
icons and in-viewport drawing, Show-on-map, the off-screen indicators and their
measurement. The phone side stays a wire path -- `pin set` exists, no app uses
it.

Pins will be drawn by `MapActivity`, not `MapRenderer`, so the webapp's firmware
preview panel will not show them (parent `docs/device-preview.md`).
