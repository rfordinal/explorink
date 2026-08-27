# Pins, as built

User pins on the device: base, parking, destination, meet, camp, favorite,
`#1`-`#5`. What
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
- Add / Replace lists all eleven catalogue slots and scrolls inside the popup's
  six-row window. List length costs one refresh per selection step and no RAM
  (`BaseTheme::optionPopupGeometry()`).
- A pin whose key this build does not know shows the raw key, and can still be
  shown, replaced and deleted.
- **Added 2026-08-26: each Add / Replace row carries the same 22px glyph the
  pin's own balloon draws on the map** (`PinIcons.h::pinGlyphIcon()`, wrapping
  the existing `pinGlyphBits()` bitmaps as a `freeink::Icon`), through a new
  icon column on `OptionPopup` (`BaseTheme::OptionPopupSpec::icons`,
  `OptionPopup::setIcons()`) that Nearby's category list
  (`docs/nearby-menu.md`) uses the same way for its POI icons. **Untested on
  hardware** -- read off the code that the 22px glyph fits the compact popup's
  row height; not measured, not on the panel.

### What is confirmed, and what a save refuses

- **Replace is always confirmed** -- `Replace Parking here?`, `Cancel` first so
  the destructive row is never under the cursor. Shortened from `Replace
  Parking with current location?` 2026-08-24, alongside the wrap fix below --
  worth trimming on its own, not just because it now has to fit.
- **Delete is always confirmed.** Nothing on the card is erased; a `del` record
  is appended.
- **An empty slot saves with no confirmation** -- unless the fix is old, and then
  it asks anyway: `Replace Camp here?`, same question as a normal replace. This
  is a deliberate addition to the plan, which said an empty slot never
  confirms. An unconfirmed save on a stale fix records where the rider *was*,
  and by the time a notice could say so the position is already written. The
  question used to spell out the age (`... (fix 7 min old)`); dropped
  2026-08-24 as unnecessary detail for a question whose only two answers are
  "yes" and "no" -- the confirmation step itself is the warning, not the
  wording of it.
- **No fix at all refuses**, with a reason, and never writes 0,0.

**The confirm dialog's title used to run off both edges of the screen.**
`BaseTheme::drawOptionPopup()` drew the title with `drawCenteredText()`,
centered on the full panel width with no wrap and no truncation -- fine for
the popup's usual short static titles, but a dynamic one long enough
(`Replace Camp with current location? (fix is from the last session)`, the
wording before the trim above) simply overflowed on both sides instead of
wrapping, since the dialog box itself is correctly capped to the panel width
but nothing capped the text drawn on top of it. Reported on the S8
2026-08-24. Fixed by wrapping the title (`wrapOptionPopupTitle()`,
`BaseTheme.cpp`) to the same width the dialog box is ever allowed to reach,
in both `optionPopupGeometry()` (so the dialog reserves the right height) and
`drawOptionPopup()` (so it actually draws that many lines) -- general to
every `OptionPopup` title, not pin-specific.

**And even wrapped, the confirm box still came out wider than the list it
replaces.** `confirmPinReplaceSlot()`/`confirmPinDelete()` were missing
`setSizeHint()` entirely at first (fixed the same day), but adding it was not
enough on its own: the title's wrap budget, when a size hint is present, has
to be the exact inverse of the `dialogW` formula --
`(maxTextWidth + innerPadding*2 + selectionHPadding*2) * widthPercent/100` --
not just `minDialogWidth - innerPadding*2`. The first cut of that clamp forgot
`selectionHPadding*2` and the `widthPercent` scaling, so a title that "fit"
its own budget still pushed `dialogW` past the hint once that formula added
the same padding back on top a second time. Measured on the S8: the Add/Replace
list at 280px, a one-line "Replace Base here?" confirm at 363px even with the
tightened clamp, both hinted at 280 -- fixed by the exact inverse, which lets
the wrap force a second line when the hint genuinely has no room for one, and
now both land on 280px exactly.

**Saving to a never-touched catalogue slot was broken until 2026-08-24** --
every save of an empty slot (including the plain, no-warning case above)
failed with "Card refused the write", logged as `PINS "no slot for ''"`.
`confirmPinReplaceSlot()` decided "foreign key, read it off the entry" from
`entry.catalogIndex >= kPinSlotCount` -- but an empty entry defaults
`catalogIndex` to `kPinIndexUnknown` (`PinStore.h`), which equals
`kPinSlotCount`, so every never-saved catalogue slot looked foreign and the
function read `entry.key` (empty, nothing had ever written it) instead of
`kPinCatalog[slot].key`. Fixed by checking `slot` itself, which is what
actually determines foreign-ness in `PinStore` (`MapActivity.cpp`,
`confirmPinReplaceSlot()`). Found on the S8 after firmware log output was
routed to `adb logcat` (`explorink-simulator`'s `ANDROID.md`, "`HWCDC`'s
stderr never reaches `adb logcat`") -- without that, the refusal had no
visible cause beyond the notice text.
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

Drawn by `MapActivity::drawPins()`, straight onto `GfxRenderer`, in the same
composition pass as the compass and the marker -- **not** by `MapRenderer`. So the
webapp's firmware preview panel will never show pins (parent
`docs/device-preview.md`); that is by design, not a bug to chase.

### A pin is a pin, and its point is the coordinate

**All of this was decided on the panel, 2026-08-17.** The first version drew a
16 px Lucide glyph on a white pad. It compiled, it was upright in a host preview,
and on the glass it failed twice over: a bare glyph could not be picked out of a
field of building outlines, and a pin saved at the rider's own position disappeared
under the 44 px position marker.

What ships instead: the map-pin shape from
`src/components/icons/pin-shape.svg`, **42x50 px** (frame 0 of the rotation table,
`src/components/icons/pins_shape.h:69`), with the type's glyph inside its head, a white halo one pixel wide around the whole outline, and the tail's
point **at** the coordinate. The halo does the job the marker's own halo does --
without it the black outline lands on road lines and the shape stops reading.
Anchoring at the point means the body hangs above the coordinate, which is both the
convention a rider already knows and the reason it no longer hides under the
marker.

Three 1bpp arrays, drawn back to front (`MapActivity::drawPinBalloon()`):

```
renderer.drawMono1bpp(frame.mask, x, y, frame.w, frame.h, false);  // silhouette + halo, white
renderer.drawMono1bpp(frame.ink,  x, y, frame.w, frame.h, true);   // outline
renderer.drawMono1bpp(pinGlyphBits(index), ..., true);             // the glyph, upright
```

The mask is not optional: the ink array is an outline, so without it the map shows
through the head. `frame` is `kPinShapeFrames[step]` -- step 0 (point-down) for a
pin in the viewport, and one of sixteen rotations for an edge marker (below).

### `drawIcon()` carries a quarter turn -- use `drawMono1bpp()`

`GfxRenderer::drawIcon()` maps `(row, col)` to `(size-1-row, col)`, reproducing a
byte-aligned blit that only the forced-Portrait UI themes ever needed; its own
comment says so. The map renders in its own orientation, so every pin glyph came
out **rotated 270 degrees** -- confirmed by diffing a `screenshot_gate.py` grab
against the assets, a 100 % pixel match at `rot270`.

`drawMono1bpp()` (added for this) has no quarter turn, takes non-square
dimensions, and takes the colour to paint -- which is what makes the white mask
pass possible. Anything on the map that draws a bitmap should use it.

### The asset pipeline

`scripts/gen_pin_icons.py` bakes `src/components/icons/pins_shape.h` from the
shape SVG plus `src/components/icons/pins.icons.txt`. It is separate from
freeink-sdk's `gen_icons.py` because that one emits square ink-only icons and
lives in a submodule that is upstream's.

What it does, all measured off the rasterised artwork rather than hardcoded:

- crops the shape to its ink, so the pin's height is the shape's height;
- derives the **fill mask** by flooding the not-ink pixels from the border --
  whatever the flood cannot reach is inside;
- **dilates** the mask by `--halo` (1) pixels: that is the white separation;
- finds the head's centre and clear radius as the widest enclosed run in the upper
  half, then picks the largest glyph whose diagonal fits;
- composites the glyph `--glyph-dy` (4) pixels **below** that centre. The tail
  drags the eye down, so a glyph on the geometric centre reads as if it were
  floating at the top -- judged on the panel twice.

The numbers moved when the rotations landed: that pass rasterises at a different
oversample and the head measured a pixel wider, so a 22 px glyph fits where 20 px
did before. **Read the sizes off `pins_shape.h`, not off this page** -- they are
generated.

Glyphs, all chosen at 20-22 px on the glass:

| pin | glyph | why |
|---|---|---|
| Base | `house` | |
| Parking | baked `P` | `circle-parking` read as a circle inside a circle |
| Destination | `flag-triangle-right` | a pennant survives at this size; `flag` is busier |
| Meet | `circle-dot` | `handshake` was mush at this size |
| Camp | `tent` | |
| Favorite | `heart` | direct Lucide match, no substitute needed |
| `#1`-`#5` | baked numerals | Lucide has no numerals, and nothing says "#3" like a 3 |

`text:X` in the manifest bakes characters from a system bold font, so the device
needs no font for them.

The generator needs `rsvg-convert` (librsvg) or `cairosvg`; this machine has only
the second, and the script falls back to it.

### Layer order

1. map context and the **active route** (both inside `MapRenderer::render()`)
2. pins
3. position and heading, the compass, the scale, the readout, the hints

**Not the order the plan asked for**, which put pins under the route. The route is
drawn inside `MapRenderer::render()` as a second source and pins deliberately are
not in that renderer, so a pin lands on top of a route line passing through it.
The alternative is handing pins to `MapRenderer`, which is the coupling the plan
rejected for a stronger reason.

A pin is projected with `projectMercWide()`, not the int16 path: a pin can be a
hundred kilometres away, and the narrow projection would wrap and draw it in the
middle of the map.

The overview frame (the menu's "Whole route") draws pins too -- "where is the car
relative to this whole route" is what that frame is for.

### Off-screen markers

`mapPinsOffscreen`, **default off**, in Settings (category Map) and over
`CMD:SETTING mapPinsOffscreen 1`.

**The marker is the pin, turned so its point aims at where the pin is.** Three
iterations on the panel got here (2026-08-17):

1. an arrow plus a distance -- says "11 km that way" and nothing about *what* is
   that way, which is the question;
2. the pin upright plus an arrow beside it -- says the direction twice, in two
   different shapes;
3. the pin itself, rotated. One object, and the identity and the direction are the
   same mark.

`scripts/gen_pin_icons.py --steps 16` bakes one frame per 22.5 degrees, clockwise
from point-down, each carrying where its point and its head ended up
(`PinShapeFrame`). The **glyph is not rotated with the shape** -- a numeral or a P
has to stay readable -- so the device draws the frame and then the glyph upright at
that frame's head centre. Step 0 is point-down, which is every pin inside the
viewport. `MapActivity::pinShapeStepFor()` picks the step from the screen-space
direction: step k points at `(-sin, cos)` of its angle, which inverts to
`atan2(-dx, dy)`.

Rotation happens at generation time, oversampled and downscaled, because rotating
a 1bpp bitmap at 1:1 turns a 3 px outline into dashes. Cost: ~6 KB of flash for
sixteen frames, no runtime cost, no float per frame beyond one `atan2`.

**The rotation pivot inside `gen_pin_icons.py` is the point (tail tip), not the
head.** `MapActivity::drawPinEdgeMark()` used to anchor the drawn frame on the
point, per the "point aims at where the pin is" goal above -- but the head (where
the glyph sits) then landed at a different screen offset for every one of the 16
frames, since it swings around the point rather than staying still. Reported from
the panel 2026-08-23: the glyph looked like it was jumping as the marker turned.
Fixed the same day by anchoring the *head* instead: `drawPinEdgeMark()` clamps and
targets `frame.headX/headY` against `pinEdgeArea()`, then back-solves the point
target from the frame's own point-to-head offset before calling
`drawPinBalloon()`. The head, and the glyph on it, now sit at the same screen spot
across all 16 rotations; the point is what swings, and it no longer sits exactly on
the area boundary -- it now pokes past it by a frame-dependent amount. Accepted
trade: a moving point reads as an arrow doing its job, a moving glyph read as a
bug.

**Where a marker may land** is `MapActivity::pinEdgeArea()`: the panel minus
everything this screen already draws over the map -- the bottom hint bar, the
side-hint boxes and the compass. The geometry comes from whoever owns each piece
(`BaseTheme::buttonHintsRect()`, `BaseTheme::sideButtonHintsRect()`, this file's
compass constants), because both of the first two were found the hard way: an
11 km marker came out underneath the zoom hints with only its "11" readable, and a
bottom one sat behind the button bar. On X3 the side hints are one band across the
full width, which this does not special-case -- read off the code, untested, no X3
here.

**The distance label is `SMALL_FONT_ID` (8pt) with a white halo, not `UI_10_FONT_ID`
in an opaque box** (changed 2026-08-23, reported alongside the pivot as too big and
too bold for what is detail-view chrome, not a primary label). The halo is the same
technique place labels use (`MapLabels.cpp:315-325`, `kHaloRing`): the string is
redrawn white at 8 ring offsets for two pixels of radius, then black on top, so the
map still shows between the letters instead of disappearing under a filled
rectangle.

**The numeral/letter glyphs baked into a pin's head** (`pin_c1`..`pin_c5`, the
parking `P`) come from a **regular**-weight system font since 2026-08-23 --
`DejaVuSans.ttf` / `LiberationSans-Regular.ttf` in `gen_pin_icons.py`'s
`DIGIT_FONTS`. They were baked bold before that, reported too heavy at the same
time as the pivot bug.

**The head's centre, for every frame, is a tracked point, not a re-detected
one.** The first version of this fix still called `head_circle()` -- the same
"widest interior run in the upper half" scan used to find the glyph's clear
area on the upright shape -- on each *rotated* frame to find `frame.headX/Y`.
Confirmed on the S8 the same day: at rotated (non-cardinal) steps the tail's
own stroke sweeps into that scan and drags the detected centre sideways, well
off the ring the panel actually draws -- the flag glyph sat visibly left of
centre inside the head. A circle is rotation-invariant -- only its position
moves -- so `gen_pin_icons.py` now measures the head's centre once, on the
upright raster, marks it with a filled dot in a side-channel image, and carries
that dot through the exact same rotate/resize/crop/pad pipeline the shape
artwork itself goes through; the dot's centroid in the finished frame is
`headX/Y`. That tracks the real transform instead of re-guessing it from a
silhouette the tail can contaminate. The per-frame clear-radius scan stays --
only step 0's result is ever used (for the glyph's pixel size), and that
number is unchanged.

**The bearing origin is the rider only while the rider is on the frame.** Panned
away in Observe mode they are not, and a ray between two points that are both off
the panel usually crosses none of it -- so every distant pin silently lost its
marker (reported from the device, 2026-08-17). Off-frame, bearings are measured
from the middle of what is on screen; the distance stays rider-to-pin, which is the
number the rider wants.

**The label** is the distance, under the head (the point is against the edge and
aiming outward, so there is no room that side), and it dodges **sideways** when it
would land under the position marker -- which is drawn after the pins and would eat
it. Near the bottom edge there is no room above or below, and there is always most
of a screen to one side. Measured: label 149,685 90x24 against a marker at
198,658 64x64.

Markers closer than 52 px merge into one, carrying the **nearest** pin's identity,
its distance and a count: `7.1 km x3` is three pins that way, the closest 7.1 km
off. At most eight markers a frame (a stack-local array against the 256-byte rule);
anything past that is logged as an error, never dropped silently.

### Per pin

`CrossPointSettings::mapPinsOffscreenMask`, one bit per catalogue slot, **all set
by default** -- and a settings file written before this existed loads as all-set,
so nothing changes for anyone until they touch it.

**Not in the pin log.** Visibility is a display preference, not something that
happened, and a new field in a record would mean a `v2` line -- which every older
build skips as an unknown version (above, "Reading, and damage"), costing a rider
their pins. A settings bit costs nothing and carries the same information.

It is edited from the map, not from Settings: a row per pin reads better than
sixteen toggles in a settings screen. The Pins list carries
`Off-screen markers   3/5` -- how many pins would mark the edge, out of how many
are saved -- and SELECT on it opens `All` plus one row per saved pin. SELECT
toggles a row and reopens the list on that row, so several can be flipped in one
visit; each toggle costs one full-panel refresh, because `OptionPopup` repaints
through `displayBuffer()`. Closing after a change renders a real frame rather than
restoring the backdrop: a marker has just appeared or gone away, so the map
underneath is wrong.

With `All` off, every per-pin row reads `-` rather than `Off`. The bit is whatever
it is; the master is simply overriding it, and this popup cannot grey a row.

A key this build does not know has no bit and follows the master --
wrong-but-visible beats a pin whose marker silently never appears.

**What is not built:** the plan's quantised per-fix redraw. Markers are drawn only
as part of a frame that was going to be rendered anyway, so a distance goes stale
between viewport resets and a marker costs nothing extra on the panel. That is
precisely the part the plan gated on a measurement ("refreshes per minute at a
realistic fix rate, ghosting after an hour, whether the patch restore is clean at
the panel edges"), and that measurement still has not been run. So the open item
now has two halves: measure what a per-fix edge redraw costs, then decide whether
to build it -- and only then can the default move.

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

The per-fix edge-marker redraw and the panel measurement it needs (above). The Pin
History UI and Restore stay deferred; `pin log` plus `pin set` recover a pin today.
Long-press SELECT, trips, per-pin visibility and log rotation stay deferred as
planned.

**The phone side is no longer a wire path.** The Android app drives `pin list`,
`pin set`, `pin del` and `pin log` from its own Pins screen as of 2026-08-19 --
what it adds is a coordinate the rider chose (pasted text, a shared maps link) and
a real `utc`, the two things this device cannot produce. It holds no copy and
reconciles nothing, so nothing changes on this side: the card is still the only
store. The parent repo's `docs/android-pins.md` is that side, and none of it has
run against hardware either.

**Nothing in this feature has run on the panel.** Every geometry and button claim
above is read off the code. What a hardware pass has to check:

**Confirmed on the device 2026-08-17** (maintainer at the panel, agent on the
console): a pin saved from the console survives a reflash and a reboot with its id
intact, `pin set/del/list/log`, the pins and their halo on the glass, edge markers
with their distances and their rotation, Show-on-map, Delete and Replace from the
row actions, the per-pin toggles, and 50-52 KB free on the map screen with pins
loaded.

**Still unverified:** every rotated orientation (INVERTED, LANDSCAPE_CW,
LANDSCAPE_CCW) -- the row actions' left/right mapping and the side hints are read
off `MappedInputManager`, not seen -- and the X3 side-hint layout, which is one
band across the full width rather than a strip on the right, and which
`pinEdgeArea()` does not special-case.

Firmware RAM after all six phases: 17.8 % (58,300 bytes) -- unchanged from
`a45efe5a`, because the store lives inside the heap-allocated activity and the
tables are `constexpr` in flash. Flash went 59.5 % to 59.7 %.

Pins will be drawn by `MapActivity`, not `MapRenderer`, so the webapp's firmware
preview panel will not show them (parent `docs/device-preview.md`).
