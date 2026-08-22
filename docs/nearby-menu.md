# Useful places, and the point layer under it

What useful things are around the rider: drinking water, shelter, huts,
lodging, fuel, medical, pharmacy, rescue, SOS phones, transport out. Three
screens off the map menu, plus the marks those points draw on the map itself.

The design is `../../../docs/safety-concept.md`, "Useful places", and this file is what
was built against it. The file format is `../../../docs/point-file-spec.md`; the
mark vocabulary is `../../../docs/map-render-spec.md`, "Point mark vocabulary".

**Status: built 2026-08-21, host-tested only. Nothing here has run on an X4.**
What a hardware pass has to check is at the bottom.

## Not an emergency mode, and the name says so

A plain menu entry named `Useful places`. An emergency mode has to be entered,
and a rider who has to decide they are in trouble before pressing anything
presses nothing.

Two names were rejected on the way:

- **`Help`**, which was the first intent -- this is mostly what a rider needs in
  trouble. It reads as documentation in most software, and the word is worth more
  later: emergency info for a finder, the rider's own details, last known
  position, SOS. Keep it free.
- **`Nearby`**, which shipped first and lasted a day. It collides with
  `Look around`, the observe-mode row in the same popup: two spatial-sounding
  labels a few rows apart, one meaning "pan the map" and the other "what is
  around me".

`Useful places` promises nothing, is not a manual, and still covers the layer
after landmarks land -- a castle is not a service and not help, but it is a
useful place.

**The code and this filename still say `nearby`.** `nearbyCategoryMask_`,
`openNearbyMenu()`, `NearbyPopup`, `STR_NEARBY_*` and `nearby-menu.md` are
unchanged, so the docs match the code. Renaming the symbols is its own pass, not
folded into a label change (`../../../CLAUDE.md`, the same rule the CrossPoint
mentions follow).

## The three screens

```
NEARBY                       WATER                    SPRING

Water        * 0.7 km        Show on map      On      ? 0.7 km NE
Shelter        2.4 km        ? Spring    0.7 km NE    Water quality unverified
Huts           4.1 km        Drinking w. 2.1 km E     View on map
Medical  None within 25 km   ? Spring    3.4 km SE    Set destination
Pharmacy      14.3 km
Rescue         8.6 km
Hide all
```

All three are `OptionPopup` inside `MapActivity`, not an activity of their own --
the same reason Pins is (`MapActivity.h`, "Pins"): this screen owns the BLE
peripheral for exactly its own lifetime, so leaving it would drop the phone link
and cost a full redraw to come back.

Each screen is opened through `pendingNearbyPopup_` from `loop()`, never from
inside a popup callback: `show()` from a callback reassigns the `std::function`
that is currently executing.

### Screen 1: the category list

- **One row per category, always, in a fixed order.** The order is
  `MapSafetyCategory`'s own (`MapPointTypes.h`, generated from
  `point_spec.py`), which is why that enum is ordered for this menu and not for
  the classifier. Muscle memory beats a list that rearranges itself.
- **A category never disappears.** `None within 25 km` is a row. An absent row
  reads as zero distance or as a bug, and both are worse than the truth. The
  radius is printed because a radius the rider cannot reason about is a radius
  that lies.
- **The value column is the nearest point of that category**, so walking the menu
  is useful on its own: water 700 m, hut 4.1 km, no hospital in range, read
  without opening anything.
- **A `*` marks a category whose marks are on the map** right now, and a
  `Hide all` row appears only while at least one is.
- **With no fix, the menu refuses.** It puts up `No position yet -- nothing to
  search from` and opens nothing. The search starts at the rider, so with no
  rider there is no question to answer -- and a list measured from 0,0 would be
  worse than no list.

### Screen 2: one category

`Show on map` first, then the nearest points of that category, nearest first,
with distance and one of eight compass sectors. A `?` in front of the value means
the point carries a condition; which condition it is belongs on the detail
screen.

**No clever reordering.** An unverified spring at 700 m stays above a confirmed
tap at 2.1 km. Ranking one over the other is the device deciding something the
rider should decide, and it hides the data instead of showing it.

At most eight rows (`MapPointQuery::kMaxHits`) -- the cap is what lets the whole
result live in a fixed array with no allocation anywhere in the query.

### Screen 3: one point

Title is the point's name (`Unnamed` when OSM has none). Two information rows,
then two actions:

- **`View on map`** frames the point and switches to Observe mode, so the next
  fix does not yank the map back. Reuses observation mode wholesale, exactly as
  a pin's `Show` does, which also means the return path already exists (the map
  menu's `Follow mode` row).
- **`Set destination`** writes the existing `dest` pin. See below.

**Every row does something.** The first cut carried the distance and the
condition as rows of their own, and walking a menu cursor through text that
cannot be pressed reads as a hack -- said plainly by the maintainer on hardware
2026-08-21. So:

- the distance and sector ride in the **value column of `View on map`**, the row
  whose target they describe
- the condition is a **note**: one line under the title, not selectable, drawn
  only when the point carries one (`OptionPopup::setNote`, and
  `BaseTheme::OptionPopupSpec::note` under it -- the widget grew the capability
  it was missing, which pins and the route picker can use too)

## `View on map` has to leave something to look at

The first cut re-anchored the frame on the POI and did nothing else, so the
rider arrived at a place with **nothing marked on it** ("kde je ten zdroj vody?
ani prd" -- maintainer, on hardware). The marks are drawn only for categories the
mask carries, and moving the frame set no bit.

Now it does three things:

1. **Turns that category's layer on** (`nearbyCategoryMask_`), so the point and
   its siblings are drawn at all. The menu shows it with a `*` afterwards, and
   `Hide all` turns it off again -- nothing hidden happened.
2. **Remembers which point was asked for** and rings it: a **circle** around the
   square with a 5 px white gap, drawn by
   `MapActivity::drawViewedNearbyPoint()` straight onto `GfxRenderer` after the
   map, the same place and reason as `drawPins()`. In a village a category is a
   field of squares and being centred on one of them does not say which one it
   was.

   **A circle, and not the rounded rect this started as.** The first cut drew a
   rect 3 px outside the square, and on the panel that reads as a square with a
   fatter border: the maintainer reported no ring at all, and this session looked
   at the same screenshot and agreed -- while the device's own log said
   `ring drawn at 230,600`. A different *shape* is what carries at this size.
   The episode is also why that function logs a line per refusal: it draws
   straight onto `GfxRenderer`, so **the laptop preview cannot show it** (the
   preview runs `MapRenderer` through `IMapCanvas` only), and without the log
   there was nothing to distinguish "not drawn" from "drawn and unreadable".
3. **Enters Observe**, so the next fix does not yank the frame back.

The ring is dropped when the rider goes back to Follow: they are following
themselves again, and a ring around something they looked at once is decoration.
It is also not drawn when the point is off the panel -- clamping it to an edge
would claim the point is at the edge. Same open question as the rider's own
off-screen fix: only pins have edge arrows today.

## One setting for the whole layer, and a dimmed row when it is off

`SETTINGS.mapPointsEnabled`, category Map, **default on**
(`../src/CrossPointSettings.h`, next to `mapTileFreshnessMode`). It answers one
question: does this device deal in the point layer at all. It is not about data
-- *when* a shard is fetched is `mapTileFreshnessMode`'s question, and that one
defaults to `Off` (`../../../docs/point-layer-lifecycle.md`, decision 2).

On by default because reading a shard the card already holds spends card time
and no mobile data. So the default costs nothing and the feature is there for a
rider who never opens Settings.

`MapActivity::pointsEnabled()` is the single read, and three paths go through
it:

- `onEnter()` does not allocate `MapPointSource` or its file handle at all, so
  1.3 kB goes back to the heap the map screen is always short of.
- `renderViewport()` hands the render no point source, so no shard is opened.
- the map menu's `Useful places` row is **dimmed and unselectable**, with `OFF` in the
  value column.

Read live rather than cached in `onEnter()`: the Settings screen can flip it
while this activity is alive, and a cached copy would keep drawing marks the
rider just switched off.

### The row sits low in the menu

Just above `Refresh`, below the four mode toggles. It used to sit straight after
`Pins` on the argument that both answer a question about the ground around the
rider. That was wrong about how often it is pressed: a rider opens this screen
when they need water or a hut, which is rare, while the rows above it are
touched every ride. Muscle memory belongs to the frequent rows.

### Landmarks get no rows here

T-305's landmarks (castles, viewpoints, peaks, passes) are **not** categories in
this menu. They draw on the map when they exist, and that is all
(`../../../docs/point-layer-lifecycle.md`, decision 4). So this screen stays a
list of things a rider goes looking for, and the landmark category ids that
`point-file-spec.md` leaves open are a render and glyph question only, not a row
order.

### The row is dimmed, not removed

A row that vanished reads as a firmware that lost a feature. A dimmed row reads
as a switch somebody turned off, which is the truth, and it stays where muscle
memory expects it.

`OptionPopup::setDisabledRows()` carries the mask, parallel to the options, and
is called **after** `showWithValues()` -- which clears it, the same rule
`setNote()` follows, so a stale mask cannot survive into the next popup. A
disabled row is skipped by the selection walk (the same walk
`HomeActivity::nextSelectable()` does), swallows a tap, and refuses a `Confirm`
that somehow lands on it.

**The ink comes from `BaseTheme::dimDisabledRow()`, which Home's menu already
used.** Every second pixel row to white, drawn last so it dims the label and the
value together. A line screen and not a checkerboard: the panel has no grey in
BW mode (`eink-grayscale.md`) and at this text size a checkerboard speckles the
glyphs instead of greying them. Home had this inline; it moved into a shared
helper so the two lists cannot drift into two different ideas of "disabled".

**Not verified on the panel.** The dim is Home's, so it is already known to read
there, but it has never been seen on an `OptionPopup` row -- a popup row is
shorter than a Home row and carries a value box. What a hardware pass has to
check is at the bottom.

## `Show on map` is a view, not a setting

`nearbyCategoryMask_`, one bit per category, on `MapActivity` and **not** in
`CrossPointSettings`. It does not survive a reboot, on purpose: it is a temporary
layer a rider switches on to find something, not a preference.

Zero -- the default -- draws nothing at all, and then the render never opens a
shard. So the map is unchanged until the rider asks for a layer, which also keeps
the marks sparse: a render with every category on is a village buried under
squares (seen in the preview, `mapbuilder/tools/poi_glyph_probe.py`'s sibling
render).

Toggling a layer **closes the popup and draws the map**, rather than reopening
the list the way the off-screen pin list does. A rider who just switched water on
wants to see where the water is; the list is one press away again.

The mask filters the render walk (`MapPointSource::Config::categoryMask`), never
the style and never the card. Same rule as the mode class mask
(`../../../docs/map-data-spec.md`, "Mode is a render-time filter").

## `Set destination`, not `Save as pin`

The pin type already exists: `dest` / "Destination", third slot of `kPinCatalog`
(`PinCatalog.h`). One slot means one destination at a time, and replacing an
occupied slot is already `PinOp::Replace` (`PinRecord.h`). So the action writes an
ordinary v1 pin record through `MapPins::pinSet()` -- the same path a pin saved
from a popup or pushed over BLE takes -- and needs no catalogue row and no format
change.

**The POI's category is deliberately not stored.** A pin record is
`v1|seq|utc|uptime|op|key|id|latE7|lonE7|trip|crc32` with no field for
provenance. Adding one means a v2 line, and a build that does not know the
version **skips the whole record**, not just the new field -- it loses the pin.
Once the rider has committed to going somewhere, the bearing and the distance are
what matter, not that it was a spring.

On screen the change needs no words: the square stops being a square and becomes
the destination pin's balloon.

## The header readout, and the repaint floor it needs

While a destination is set, its sector and distance replace the place name in the
header slot that already exists (`drawHeaderPlaceName()`). No taller header, no
smaller map.

Quantised harder than the Pins list, which prints 10 m steps:

| distance | printed |
|---|---|
| under 1 km | 100 m steps, `NE 700 m` |
| 1 to 10 km | 0.1 km steps, `NE 4.2 km` |
| over 10 km | 1 km steps, `NE 14 km` |

Bearing is one of eight sectors (N, NE, E, SE, S, SW, W, NW), never degrees.

**Repaint at most once per 30 s** (`kDestRepaintMs`), and only when the printed
value or the sector actually changed -- `destQuantisedDistance()` returns the
value in the printed unit's own steps precisely so an unchanged reading cannot
cause a repaint. Two exceptions with no floor, because they change what the row
*means* rather than its value: setting a destination and clearing one. Same shape
as the structural exception already in `map-header-status.md`'s repaint policy.

At 30 km/h a 10 m step would change about once a second, and every change is a
real waveform pass. This is a product decision as much as a power one: the device
says *the target is roughly that way, roughly that far*. It is not a bike
computer.

A destination change takes the whole-row repaint path
(`drawHeaderStatus()`), not the strip path: `drawHeaderStatusStrip()` does not
draw the place-name slot, and the whole-row path is the one that gets the
clear-then-draw order right (the 2026-08-15 finding in
`map-header-status.md`, "The clock").

## What reads the card

Three classes, each with its own file handle, because they all stream during one
render and one seek cursor cannot serve two readers:

| class | question | grid |
|---|---|---|
| `MapPointReader` | one `.tip` shard, record by record | -- |
| `MapPointSource` | which marks does this viewport draw | shards the viewport touches |
| `MapPointQuery` | what is within 25 km of the fix | shards the circle touches |

Measured with `sizeof` on the host, same layout as the target
(`MapPointReader` 1,104 B, `MapPointSource` 1,368 B, `MapPointQuery` 1,312 B, one
`Hit` 48 B):

- `MapPointSource` is allocated in `onEnter()` next to the tile source, whether or
  not a layer is switched on, so flipping one never has to allocate on a button
  press. With the mask at zero the render never hands it over and it opens no
  file.
- `MapPointQuery` is allocated the first time the rider opens `Useful places` and kept
  for the screen's life.
- Fixed cost inside the activity: `nearbyHits_` is 8 x 48 B and
  `nearbyDistances_` is 44 B, so about 430 B on top of the two heap objects.

The build after this work: **RAM 17.8 % (58,332 B of 327,680), flash 60.4 %
(3,956,217 B of 6,553,600)**. Static RAM is the activity's own members only --
both point objects are heap and live for the map screen's lifetime, like the tile
and route sources next to them.

RAM is O(1) in the number of points, the same rule the tile and route readers
follow: records stream through a fixed 1 kB buffer and nothing accumulates. Names
are **not** in the record -- `readName()` seeks into the name pool and back, so
the nearest-per-category pass reads 16 bytes per point and pays for a string only
on a row it prints.

**The crc is checked before a record reaches the screen, not before it becomes a
distance.** Verifying costs a second full read of the shard: `verifyBody()`
streams the whole body, then `beginRecords()` re-reads the record array.
Measured on the device 2026-08-22, six shards for one press of `Useful places`:

```
nearby: 6 shard(s) read, 0 missing, 0 corrupt, 201994 bytes
```

against roughly 142 kB of actual file. So the distance pass
(`nearestPerCategory`) runs without the check and the display pass
(`listCategory`) runs with it. A corrupt record in the distance pass can only
make one row's number wrong, and `nextRecord()` already refuses anything grossly
broken (outside the declared bbox, non-zero reserved half-word, a name running
past the pool). A corrupt record that reaches the screen becomes a name a rider
reads and a coordinate `Set destination` writes into the pin log, so that pass
checks the crc and a failing shard contributes no rows at all.

**The read cost is a city problem, not a mountain one.** Measured on a real
build (`region1`, 2026-08-21): a rural shard is 22 to 500 points, and the
Bratislava shard is 2,602 -- 41.6 kB of records for one `Useful places` press, off one
file, through a 1 kB buffer. Zahorie is a few kB. That number is unmeasured *on
the device*, which is what makes it the first thing a hardware pass should time.

A shard that fails its checksum is skipped whole and the walk continues with the
next one. One bad file must not hide the eight good ones around it, and a
half-read shard would put a hospital somewhere there is none.

## The marks on the map

`MapPointMarks::draw()`: white knock-out, square outline, category glyph, and one
corner triangle when the point carries a condition. Drawn after the place dots
and before the names -- a square is a bigger mark than a dot and must not be
buried under one, and a name must still win over both.

**Sizes, and who chose them.** 15 px square / 1 px border / 9 px glyph / 4 px
flag came off a real-size render before any of it was on hardware. Grown 20 % to
**18 / 2 / 11 / 5** on the maintainer's call after seeing it on the panel
2026-08-22 -- the 2 px border is what makes a mark read as a mark over a hatched
forest.

Growing it moved two glyphs out of tune, which is worth knowing because it will
happen again: the shapes were drawn against a 9 px box, and at 11 px the water
drop's disc swallowed its triangle (a blob) and the bed's mattress became a black
slab. Both retuned to proportions of the box rather than fixed offsets -- the
disc is two thirds of the glyph, the mattress a third. The drop is still the
weakest of the ten and is the one to look at first on the glass.

**The glyphs are drawn from `IMapCanvas` primitives, not from icons.** That was
the one thing `map-render-spec.md` left open for this task, and it was settled on
a render of ten glyphs at real size rather than on paper
(`mapbuilder/tools/poi_glyph_probe.py`, sheet in
`../../../docs/design-shots/poi-glyphs.png`): Lucide's 24 px, 1.5 px-stroke SVGs
thresholded to 1 bpp at 9 px break into dots -- the droplet, the pump, the cross,
the pill, the buoy and the handset all came out as noise -- while primitive
shapes read as shapes at that size. `MapPointMarks.h` has the full reasoning,
including why the emergency-phone glyph is an exclamation mark and not a handset.

`not_potable` never reaches the device: the writer drops such a point from water
entirely, so a square with a water glyph always means candidate for drinking
water (`point-file-spec.md`, "The honesty rules are in the writer").

## Verified, and not

Host tests (`test/map_points/`, 9 tests, all passing): the format round-trips
against a fixture written by `point_file.py`, six kinds of corruption are
refused, y grows north, the flag mask is exactly the four conditions, the shard
grid arithmetic holds, the radius search answers every category including the
empty ones, a category list comes back nearest-first with names and sectors, and
the eight sectors are right at 48 degrees north (where a degree of longitude is
two thirds of a degree of latitude on the ground).

Preview render (`test/map_preview`, real tiles and real shards built for
Terchova with `build_tiles.py --around 49.2084 19.0424 --radius 6`, which wrote
177 points into 2 shards and dropped 3 `not_potable`):

- every category on: 139 marks from 2 shards, 7.5 kB read
- water+huts+shelter only, which is what a rider would actually switch on: 42
  marks, same 7.5 kB (the filter drops after the read, so a narrow layer costs
  the same card time and a much quieter map)
- **0 heap allocations during the render**, both times

The sheet is `../../../docs/design-shots/nearby-marks-preview.png`. It is a
**laptop preview render** of the device's own renderer, not a device screenshot,
and must never be presented as one (`../../../docs/device-preview.md`).

**Found on hardware, 2026-08-21, and fixed:** `View on map` left the device
believing the rider was standing on the POI. Not a `Useful places` bug -- the ladder/fix
save persisted `lastLatE7_`, which `renderViewport()` repoints at whatever it
draws, so panning in Observe and a pin's `Show` did it too. `Useful places` made it easy
to reach because `View on map` re-anchors kilometres away.
`map-observation-mode.md`, "The pan target is not the rider", has the chain.

**Measured on hardware 2026-08-22, after the crc split and the window bound:**

```
nearby: 6 shard(s) read, 0 missing, 0 corrupt, 78880 bytes
nearby layer mask 0x000a
```

- One press of `Useful places` reads **78.9 kB** off the card for six shards, down from
  201,994 bytes before the split. The remainder is the record arrays and the
  headers; no name pool and no crc pass.
- No `full refresh instead` line in the whole session, so the 4 kB window margin
  leaves the cheap menu close cheap while still refusing the 48 kB full-panel
  window that aborted the device.
- Free heap on the map screen 47.6 kB, largest block 45.0 kB, and `Min Free`
  14,684 bytes -- which is the menu-close window's own 33,733-byte buffer and is
  now a deliberate, checked transient rather than a coin flip. That last
  attribution is **arithmetic, not a logged fact**: 47.6 kB free minus the
  33,733-byte window leaves 13.9 kB against a measured floor of 14,684 B, and no
  log line ties the two together.
- The layer mask reached 0x000a (water and huts), so `Show on map` and
  `View on map` do set their bits.

**Verified on hardware 2026-08-21:** the header readout. With a destination set
it read `SE 5.2 km` in the place-name slot, matching the destination pin's own
edge marker to the same 0.1 km step.

**Not verified -- needs an X4 pass:**

- Nothing here has run on the device. No screen has been on the panel.
- **The dimmed `Useful places` row.** Switch `Useful places` off in Settings, open the
  map menu: the row must be there, visibly dimmed, reading `OFF`, and neither
  the button walk nor a tap may land on it. The dim is Home's own line screen,
  but on a shorter row with a value box beside it.
- The three popups' layout at the map menu's dialog size: row count, the value
  column with `?` in it, and a long POI name in the title.
- ~~Whether the 15 px square and the 9 px glyph read on the glass.~~
  **Judged on the panel 2026-08-21 and accepted by the maintainer.** No flash was
  involved: a `test/map_preview` frame (Vratna, 25 marks -- bus stops, beds, a
  flagged water drop, a tent) went to the panel with
  `tools/show_on_device.py` over `CMD:SHOWIMAGE`, and `CMD:SCREENSHOT` read it
  back bit-identical (0 of 384,000 pixels differed). This is the one item on
  this list the serial path can settle, because everything else here needs
  `MapActivity` running.
- The card cost of a real query: how long the shard opens take on the SD path,
  and whether a menu press is perceptibly slower than the Pins list. **Time it
  in Bratislava, not in the hills**: the dense shard is 2,602 points and 41.6 kB
  of records against a rural shard's few kB, so the hills would give a number
  that flatters the design. The device logs
  `nearby: N shard(s) read, M missing, K corrupt, B bytes`.
- The header readout: that it actually replaces the place name, that the 30 s
  floor holds while riding, and that setting a destination repaints at once.
- `Set destination` writing the log: that the record lands and survives a
  reboot.
- **How a shard reaches the card.** A push works, and this is now **measured on
  hardware (2026-08-21)**: nine shards, 156 kB, pushed with `tools/blepush.py`
  into `points/10/<col>/<row>.tip` and each one acknowledged by the device with
  its own crc32. Throughput 3.1 to 4.0 kB/s, 39 s of transfer for the nine.
  `MapTransferReceiver` needed no change: it accepts any relative path under the
  root, so the point layer rides the tile transfer as it is.

  **The phone has to be off the link for it.** A connected peripheral stops
  advertising, so while ExplorInk GPS holds the connection the laptop cannot
  find the device at all -- `blepush` fails with "no device advertising the map
  service", which reads like a broken adapter and is not. Turn the phone's BT
  off, push, turn it back on; the last fix stays in RAM, so `Useful places` still has
  somewhere to search from in between. What does **not** happen is automatic: the tile index and
  the auto-sync path are built around `base/` (`tools/build_index.py` scans that
  directory only), so a rider whose area gains a point shard gets it only by a
  deliberate push or a card copy. That is the same hole as the freshness
  question below and it wants one decision, not two.
- **Freshness.** A `.tip` carries a `build_epoch` and nothing else. A device
  compares `content_id` to notice a stale tile
  (`../../../docs/tile-index-spec.md`); there is no equivalent here, so a shard
  that was rebuilt looks identical to one that was not.
- Marks overlapping in a village. With one category on it is much thinner than
  the all-categories preview, but nothing declutters them -- two squares 3 px
  apart read as one blob. Open: whether that needs a merge rule like the pin
  edge markers have.
