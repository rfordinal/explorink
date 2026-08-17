# Pins, as built

User pins on the device: base, parking, destination, meet, camp, `#1`-`#5`. What
the rider needs a direction and a distance to right now, not a POI database.

The requirement, the decisions behind it and the phase plan are in
[`pins-plan.md`](pins-plan.md). This file is the mechanism as built. Every claim
here is **read off the code** unless it says otherwise; nothing in the pins path
has been measured on hardware yet.

Status, 2026-08-17: **phases 1-6 built** -- the store, the log, the console
commands, `OptionPopup` row actions, the Pins UI, Show-on-map, the icons, the
in-viewport drawing and the off-screen markers (default off). 37 host tests
(`test/pins/`). **Nothing has been on hardware**, and the measurement phase 6's
default flip is gated on has not been run -- see "Off-screen markers" below for
what is actually built versus what the plan described.

## The files

| file | what | pure? |
|---|---|---|
| `src/activities/map/PinCatalog.h` | the type table, `kPinSlotCount`, key lookup, key validation | yes |
| `src/activities/map/PinRecord.{h,cpp}` | one log line: encode, decode, CRC, version | yes |
| `src/activities/map/PinStore.{h,cpp}` | the active pins and the mutations that produce records | yes |
| `src/activities/map/PinLogScanner.{h,cpp}` | bytes to lines to records, and the replay | yes |
| `src/activities/map/PinLog.{h,cpp}` | the only file that touches the card | no (HalStorage) |
| `src/activities/map/PinGeo.{h,cpp}` | straight-line distance and how it is written | yes |
| `src/activities/map/MapPins.{h,cpp}` | log-then-apply, and the console's `IMapPinsSource` | no (Arduino) |
| `src/activities/map/PinLabels.h` | catalogue row to `StrId` | no (I18n) |

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

## The UI

All of it is `OptionPopup` inside `MapActivity` -- no Pins activity. A separate
activity would drop the BLE peripheral, because this screen owns it for exactly
its own lifetime (`MapActivity::onExit()` calls `BlePositionServer::end()`), and
coming back would cost a full redraw.

Entry is a **`Pins` row in the CONFIRM menu**, above the settings toggles, with
the saved count in the value column. Long-press SELECT is still deferred: no long
press exists anywhere in this firmware.

### Row actions: LEFT deletes, SELECT shows, RIGHT replaces

`OptionPopup` learned per-row actions for this (`OptionPopup.h`,
`setRowActions()`). The front Left and Right buttons are **not free** in a popup
-- they are aliases for scrolling (`NavNext` is side Down *or* front Right) -- so
the Pins list does not gain buttons, it takes that pair away from scrolling:

- selection moves on the screen's up/down pair (`Button::ScreenUp` /
  `ScreenDown`),
- the screen's left/right pair acts on the selected row.

Everything goes through the `Screen*` buttons and `mapDirectionalLabels()`, never
raw `Left`/`Right` with `mapLabels()`: the physical pair swaps in INVERTED and
LANDSCAPE_CCW, and a label on the wrong button means Delete under a hint that
said Replace. **Read off `MappedInputManager.cpp:20-50`, not yet checked on a
rotated panel.**

The four-box hint bar has no slot for the side buttons, so the popup draws its own
side hints through the theme (`setSideHints()`, arrows chosen for the theme's 90°
CW text rotation, the same correction `drawPanSideHints()` verified on hardware).
Those boxes land outside the dialog, so the menu close refreshes a taller window
than the backdrop rect (`restoreMenuBackdrop()`).

Opt-in per popup, and every `show()` clears it: a menu that inherited a list's
row actions would delete a pin from a row that means something else.

### The two lists

```
Pins                          Add / Replace
+ Add / Replace               Base          2.6 km
Base              2.6 km      Parking       1.2 km
Parking           1.2 km      Destination
Camp                   -      Meet
#1                890 m       Camp          890 m
                              #1 ... #5
```

- The Pins list shows **existing pins only**; an empty slot lives in Add /
  Replace. With nothing saved, the list is the one Add row -- ten rows saying
  "empty" would be worse.
- The distance column is the popup's existing value column
  (`showWithValues()`), and it is `-` when there is no fix to measure from.
  Never `0 m`: zero is a real distance and reads as "you are standing on it".
- Add / Replace lists all ten catalogue slots and scrolls inside the popup's
  six-row window. List length costs one refresh per selection step and no RAM
  (`BaseTheme::optionPopupGeometry()`).
- A pin whose key this build does not know shows the raw key, and can still be
  shown, replaced and deleted.

### What is confirmed, and what a save refuses

- **Replace is always confirmed** -- `Replace Parking with current location?`,
  `Cancel` first so the destructive row is never under the cursor.
- **Delete is always confirmed.** Nothing on the card is erased; a `del` record
  is appended.
- **An empty slot saves with no confirmation** -- unless the fix is old, and then
  it asks anyway, with the age in the question: `Replace Camp with current
  location? (fix 7 min old)`. This is a deliberate addition to the plan, which
  said an empty slot never confirms. An unconfirmed save on a stale fix records
  where the rider *was*, and by the time a notice could say so the position is
  already written.
- **No fix at all refuses**, with a reason, and never writes 0,0.
- "Old" is `MapActivity::kPinStaleFixMs`, 2 minutes, or any frame still showing
  the fix restored from the card. **Not measured, and not the answer to "how old
  is too old"** -- that stays open in `pins-plan.md`. Two minutes is simply longer
  than the phone's own send interval at any speed.

The position saved is the **rider's**, not the frame's: in Observe mode
`lastLatE7_` is wherever the rider panned to, so a save reads
`observeReturnLatE7_` instead (`MapActivity::riderLatE7()`).

### Feedback after a save

A short boxed line above the button hints -- `Camp saved`, `Camp deleted`, or the
reason nothing was saved. It saves the pixels it covers and refreshes only its own
rectangle, so it costs one small window refresh and leaves the map up, and the
patch is what lets it disappear again 2.5 s later (or on the first button) without
re-reading a tile. Same technique as the marker patch.

The distance line the original sketch had is dropped: a pin saved at the current
position is always 0 m.

### Show on map

Reuses observation mode wholesale (`docs/map-observation-mode.md`): render the
viewport around the pin's coordinate, switch to `MapScreenMode::Observe` so the
next fix does not yank the frame back, keep the frame's heading
(`anchorHeading_`, the same choice `panBy()` makes, so looking at a pin does not
also rotate the map). GPS keeps being recorded; the route and navigation are
untouched.

Return already existed: the menu's `Follow mode` row renders around the stored
return anchor, which `showPinOnMap()` captures from the rider's fix *before* the
frame moves.

The pin is not highlighted yet -- nothing draws a pin at all until phase 4.

## Drawing

### In the viewport

Drawn by `MapActivity::drawPins()`, straight onto `GfxRenderer`, in the same
composition pass as the compass and the marker -- **not** by `MapRenderer`. So the
webapp's firmware preview panel will never show pins (parent
`docs/device-preview.md`); that is by design, not a bug to chase.

Each pin is a 16 px Lucide glyph on an opaque white pad. The pad is not
decoration: the glyph's own bitmap leaves its background alone (bit 1 is
transparent, `Icon.h`), so without it the icon lands unreadable on road lines and
landuse hatch. Same reasoning as the compass halo.

Layer order, closest to the glass last:

1. map context and the **active route** (both inside `MapRenderer::render()`)
2. pins
3. position and heading, the compass, the scale, the readout

**This is not the order the plan asked for.** The plan put pins *under* the route.
The route is drawn inside `MapRenderer::render()` as a second source, and pins are
deliberately not in that renderer at all, so a pin lands on top of a route line
that passes through it. The glyph is 16 px with a 2 px pad, so what it covers is a
few pixels at the pin's own place; the marker and the route are still the strongest
things on the panel everywhere else. Changing this would mean handing pins to
`MapRenderer`, which is the coupling the plan rejected for a stronger reason.

A pin is projected with `projectMercWide()`, not the int16 path: a pin can be a
hundred kilometres away, and the narrow projection would wrap and draw a glyph in
the middle of the map.

The overview frame (the menu's "Whole route") draws pins too -- "where is the car
relative to this whole route" is what that frame is for.

### Icons

Lucide, through the existing pipeline (parent `CLAUDE.md`, Icons). Manifest
`src/components/icons/pins.icons.txt`, generated header
`src/components/icons/pins16.h`, consumed with `GfxRenderer::drawIcon()`:

| pin | lucide |
|---|---|
| Base | `house` |
| Parking | `car` |
| Destination | `flag` |
| Meet | `users` |
| Camp | `tent` |
| `#1`-`#5`, and any unknown key | `hash` |

`#1`-`#5` share one glyph: numbering them in the bitmap would mean five
near-identical assets and a sixth the day the cap moves, and the list is where a
rider reads which number it is.

The generator needs `rsvg-convert` (librsvg), which this machine does not have.
The header was generated by putting a `cairosvg`-backed script named
`rsvg-convert` on `PATH` for that one run -- same `-w N -h N file.svg` to PNG
contract, so `gen_icons.py` itself was not touched (it lives in the `freeink-sdk`
submodule, which is upstream's). Re-generating on a machine with librsvg needs no
shim.

### Off-screen markers

`mapPinsOffscreen`, **default off**, in Settings (category Map) and over
`CMD:SETTING mapPinsOffscreen 1`.

What is built: a pin outside the viewport gets an arrow where the bearing ray from
the rider leaves the screen (Liang-Barsky against a 14 px inset rect), plus the
distance, and markers closer than 28 px merge into one arrow with a count
(`2.4 km x3` -- the nearest pin's distance survives the merge). Bearing is
computed in screen space, after the frame's rotation, so it is correct in every
rotation and heading mode. The arrow is a hand-drawn triangle, not a glyph: it
points at a live value, which is the documented exception to the Lucide rule.

At most eight markers per frame (a stack-local array against the 256-byte rule).
Anything past that is logged as an error, never dropped silently.

**What is not built, and why:** the plan's quantised per-fix patch update. Today
markers are drawn only as part of a frame that was going to be rendered anyway --
a viewport reset -- so a marker's distance goes stale between resets, and a marker
move costs nothing extra on the panel. The plan wanted them redrawn per fix
through the marker's own patch technique, quantised on bearing and distance
thresholds. That is precisely the part the plan gated on a measurement
("refreshes per minute at a realistic fix rate, ghosting after an hour, whether
the patch restore is clean at the panel edges"), and that measurement has not been
run. Drawing them per frame ships the feature with **no new refresh behaviour at
all**, which is why it can land before the measurement exists.

So the open item is unchanged and now has two halves:

- measure what a per-fix edge redraw costs on the panel, then decide whether to
  build it at all;
- only then can `mapPinsOffscreen`'s default move.

## Distance

Straight line, never routing. Equirectangular with a `cos(latitude)` scale taken
at the **midpoint** latitude, so the answer does not depend on the argument order.

Integer only, no libm, no float: the ESP32-C3 is RV32IMC and every float is
soft-float. The cosine is a 91-entry `uint16` table (182 bytes of flash) scaled by
1024 and interpolated linearly; the square root is the bit-by-bit integer method.

Checked against the haversine formula on a 6,371,008.8 m sphere at six
separations, 0 m to 33 km, all within 1 % (`test/pins/PinsTest.cpp`,
`PinGeo.KnownSeparationsAreWithinAPercent`). The 1 % is dominated by the constant
this code uses for a degree (111,320 m, the WGS84 equatorial figure) against the
mean-radius one haversine uses -- 0.11 % -- plus the projection's own error, which
grows with separation. Both are far below the 10 m the result is rounded to.

Written as `820 m` (10 m steps) below 1 km, `4.2 km` below 10 km, `37 km` above.
999 m prints as `1.0 km`, not `1000 m`.

## What is not built yet

The per-fix edge-marker redraw and the panel measurement it needs (above). The
phone side stays a wire path -- `pin set` exists, no app uses it. The Pin History
UI and Restore stay deferred; `pin log` plus `pin set` recover a pin today. Long-
press SELECT, trips, per-pin visibility and log rotation stay deferred as planned.

**Nothing in this feature has run on the panel.** Every geometry and button claim
above is read off the code. What a hardware pass has to check:

- a pin saved from the menu, then a reboot, then the pin still there;
- the row actions in a rotated orientation, and that the side hints do not look
  broken;
- that the notice patch restores cleanly;
- that a 16 px glyph is actually legible on the glass over a road and over hatch,
  and that it does not out-shout the marker or the route;
- an edge marker's arrow and distance with `mapPinsOffscreen` on;
- free heap with the Pins list open (the map sits around 54 KB free and the menu
  backdrop already spends ~9 KB of it).

Firmware RAM after all six phases: 17.8 % (58,300 bytes) -- unchanged from
`a45efe5a`, because the store lives inside the heap-allocated activity and the
tables are `constexpr` in flash. Flash went 59.5 % to 59.7 %.

Pins will be drawn by `MapActivity`, not `MapRenderer`, so the webapp's firmware
preview panel will not show them (parent `docs/device-preview.md`).
