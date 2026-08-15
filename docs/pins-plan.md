# Pins: the agreed requirement, and the build plan

User pins on the device. A pin is a place the rider needs to keep a spatial
relation to during a trip — base, car, camp, the bus. Not a POI database, not
routing, not turn-by-turn.

Status: **agreed 2026-08-15, nothing built.** Every "how" below is read off the
code and cited. Nothing is measured on hardware yet; the one item gated on a
measurement is called out in "Off-screen pins".

This file is the whole feature: what it must do, what the device forces on it,
and the phase-by-phase plan to build it. A session picking this up needs
nothing else. Read "Rules the implementer works under" before the first commit.

## Philosophy

A pin is not a saved POI. It is a place the rider needs *right now* to know the
direction and distance to.

The device stays dumb: no typed names, no typed coordinates, no keyboard. The
rider picks a type; the position is whatever the last fix says. Everything else
is the phone's job, later.

## Hard facts this feature obeys

Not design choices. What the device is.

- **No GPS on the device.** Position arrives in the phone's BLE packet
  (`lib/BlePositionServer/include/BlePositionServer.h:25`; the parent repo's
  `docs/architecture-plan.md:488` says why it will stay that way). "Save at my
  position" means "save at the last fix". No phone, no fix, no pin.
- **No clock on the device.** UTC rides in the same packet and `0` means the
  sender had none either (same line). Off the phone there is no wall time. A
  record must be able to say the time is unknown instead of inventing one.
- **The map screen owns the BLE peripheral for exactly its own lifetime**
  (`MapActivity::onExit()` calls `BlePositionServer::end()`,
  `src/activities/map/MapActivity.cpp:1761`). A separate Pins *activity* would
  drop the phone link, stop position, and cost a full redraw to come back —
  seconds on e-ink (`docs/zoom-rungs.md`). **So the Pins UI is popups inside
  `MapActivity`.**
- **No spare button** (`MapActivity.h`, "The buttons"). CONFIRM opens the menu;
  every direction button is a ladder or a pan. Anything new enters through that
  menu.
- **The option popup has a ceiling and scrolls inside it.** Six visible rows,
  50 % of panel height (`BaseTheme::optionPopupGeometry()`,
  `src/components/themes/BaseTheme.cpp:981`, `BaseTheme.h:290`). **List length
  is not a RAM cost**: the backdrop `MapActivity` saves is
  `OptionPopup::frameRect()` (`src/components/OptionPopup.h:165`), the capped
  dialog. A long list costs one whole-panel refresh per selection step
  (`OptionPopup::processRender()` → `renderer.displayBuffer()`,
  `OptionPopup.h:148`).
- **`/trailink/trips/` is taken.** Route files, `*.tir`, flat
  (`src/activities/map/MapRouteStore.h:26`). It is not a trip journal. Pins do
  not go there.
- **No FPU.** ESP32-C3 is RV32IMC; every float is soft-float (read off the
  target spec, not measured). Distance math stays cheap.

## Decisions

Taken 2026-08-15. Each closed a real fork; the reason is why it stays closed.

1. **Entry is a `Pins` row in the CONFIRM menu.** Long-press SELECT is a
   separate later step. No long press exists anywhere in the firmware —
   `MappedInputManager` gives `wasPressed` / `wasReleased` / `isPressed`
   (`src/MappedInputManager.h:38-40`) — and the menu opens on the *release* of
   CONFIRM (`MapActivity.cpp:2042`). A hold is a new device-wide convention; it
   needs the Pins UI to exist first and its own hardware check that a hold is
   caught reliably around blocking renders.
2. **One global log, no trips.** `/trailink/pins/pins.log`. The record carries a
   `trip` field, always empty today. Trips are a lifecycle feature (who opens
   one, who closes one, what a reboot mid-trip means) and belong with the future
   track/black-box log. Reserving the field costs nothing; adding it later is a
   format change.
3. **The log ships in the MVP; the history UI does not.** History (list, Show,
   Restore) is deferred, the log is not. It is the only part that **cannot** be
   added retroactively — pins made before the log exists are gone. And it is not
   extra work: pins must survive a reboot anyway, so the choice is a rewritten
   state file versus an append-only log replayed at boot. The log satisfies
   "Delete never erases history" for free.
4. **Fixed slots: 5 named + `#1`–`#5`.** Base, Parking, Destination, Meet, Camp,
   `#1`–`#5`. Predictable beats clever: no number handed out by the device, no
   recycling rules. The cap is one constant. (The RAM argument against long
   lists was wrong — see the popup ceiling above.)
5. **The catalogue is soft; the log is hard.** The catalogue (key, label, icon,
   order) is one table in the firmware and is expected to change. The log stores
   a **stable text key**, never an index into that table — an index would change
   the meaning of old records the day a type is inserted, which is worse than no
   history. A record also carries a monotonic `id`, never reused: the key says
   *what type*, the `id` says *which pin*, so a Camp deleted and remade is two
   pins to whoever reads the log later.
6. **Off-screen pins default OFF, and the default does not move until it is
   measured on the panel.**
7. **The device is authoritative. No sync.** The phone can push a pin over the
   existing command console; it holds no copy and reconciles nothing.
8. **`pin` console commands ship in the MVP**, with no phone UI. They are the
   only way to test a pin at a distant location without driving there.
9. **`OptionPopup` learns per-row LEFT / RIGHT actions.** One-press Delete /
   Show / Replace is worth changing the shared component. Front Left and Right
   are aliases for scrolling today, so a popup in this mode scrolls on the side
   buttons instead — opt-in, so no other popup changes. Mechanics and the
   orientation trap: "Acting on a pin".

## Scope

### In the MVP

- Pins row in the CONFIRM menu; Pins list; Add / Replace; Delete; Show on map.
- Per-row LEFT / RIGHT actions in `OptionPopup`, opt-in, plus its side hints.
- Create and Replace from the last fix. Replace and Delete always confirmed.
- Append-only log on the card, written on every Create / Replace / Delete.
- Active state rebuilt by replaying the log.
- Pins drawn in the viewport.
- Off-screen indicators, with a setting, defaulting OFF.
- Straight-line distance in the list and on the map.
- `pin set` / `pin del` / `pin list` / `pin log` on the command console.

### Deferred

- **Pin History UI and Restore.** The data is written from day one; only the
  screen is missing. `pin log` covers recovery meanwhile.
- **Long-press SELECT.**
- **Everything on the phone.** No map picker, no coordinate screen, no history
  export. The wire path (`pin set`) exists; the app does not use it.
- **Trips**, and the track / black-box log.
- **Per-pin visibility** (a far Base stored but kept off the edge).
- **Log compaction / rotation.**

### Not goals

Typed names on the device, any text input on the device, a POI database,
routing or turn-by-turn to a pin, cloud anything, any recovery path that needs
the phone.

## Data model

### The catalogue

One table, one row per type:

| field | what |
|---|---|
| `key` | stable ASCII, in the log forever: `base`, `parking`, `dest`, `meet`, `camp`, `c1`…`c5` |
| `label` | `StrId` for `tr()` — user-facing text must be translatable (`CLAUDE.md`, Resource Protocol 5) |
| `icon` | Lucide-generated glyph (see Icons) |
| `order` | position in the lists |

`static constexpr`, so it lands in flash, not DRAM (Resource Protocol 6).
Adding a type is one row; reordering and relabelling are free.

`kPinSlotCount = 10` today. Raising it is the constant plus catalogue rows.

### Active state

Ten fixed entries, static, no heap:

```
catalogue index | id (uint32) | latE7 | lonE7 | utc | seq | present
```

~32 bytes each, ~320 bytes of static DRAM for the lot. No `std::vector`, no
allocation: it is bounded by the catalogue.

**Derived, never stored.** Boot replays the log. One source of truth on the
card, so state and history cannot disagree.

**Unknown key rule.** A record whose key is not in the current catalogue still
loads: generic icon, raw key as label, deletable. Never silently dropped — an
update that eats a rider's camp is the exact failure this feature prevents.

## The log on the card

`/trailink/pins/pins.log`. (`/trailink` stays — the on-card path is
infrastructure not yet renamed; parent `CLAUDE.md`, Naming.)

One record per line, ASCII, `\n`-terminated:

```
v1|<seq>|<utc>|<uptime>|<op>|<key>|<id>|<latE7>|<lonE7>|<trip>|<crc32>
```

| field | notes |
|---|---|
| `v1` | format version; a reader refuses a line whose version it does not know and keeps reading |
| `seq` | monotonic, never reused; after a reboot it is `max(seq)+1` from the replay |
| `utc` | unix seconds, **`0` = no clock at the time** |
| `uptime` | ms since boot; orders records inside a run that had no clock |
| `op` | `add` \| `rep` \| `del` \| `res` (`res` reserved for Restore, not written yet) |
| `key` | catalogue key, stable forever |
| `id` | monotonic pin id, never reused |
| `latE7` / `lonE7` | int32, 1e7 fixed point — the units the BLE packet and `MapCommandParser` already use; empty on `del` |
| `trip` | reserved, always empty today |
| `crc32` | over everything before the final separator; `MapCrc32.h` already has the function |

ASCII, not binary, deliberately: the card gets read on a laptop, and
`/trailink/power.csv` set that precedent.

### Writing

Append one line per user action, then flush. Pin actions are rare and each one
*is* a change, so this does not violate write throttling (`CLAUDE.md`, Resource
Protocol 8 — that rule forbids writing on every interaction, not on every real
change). All SD access goes through `HalStorage` (`lib/hal/HalStorage.h`);
`openFileForWrite()` plus an append open flag, never raw SdFat.

### Reading, and damage

Streamed with a fixed line buffer, never whole-file into RAM.

- A line with a bad CRC, a bad field count or an unknown version is **skipped**.
  The reader counts skips and logs the count.
- A final line with no `\n` is a torn write: discarded.
- **A damaged record never invalidates the log.** Older valid records keep their
  meaning; the replay simply lacks that event.

### Growth

~70 bytes a record; a heavy multi-day trip is a few hundred records. Rotation
and compaction are deferred — the streaming reader means length costs time, not
memory.

## UI

All of it is `OptionPopup` inside `MapActivity`, with the existing backdrop
capture (`MapActivity::captureMenuBackdrop()`, `MapActivity.cpp:2085`) so a
dismissal costs one window refresh and no card read.

### Pins list

Opened from the CONFIRM menu. Title `Pins`. Rows:

```
+ Add / Replace
⌂ Base           2.6 km
P Parking        1.2 km
△ Camp             —
#1               890 m
```

- **Only existing pins are listed.** Empty slots live in Add / Replace. The
  typical list is short and does not scroll.
- The distance column is the popup's existing value column
  (`OptionPopup::showWithValues()`, `OptionPopup.h:63`, and `docs/map-menu.md`).
- `—` when there is no fix to measure from.

### Acting on a pin: LEFT = Delete, SELECT = Show, RIGHT = Replace

**Front Left and Right are not free in a popup today.** They are aliases for
scrolling: `NavNext` is side Down **or** front Right, `NavPrevious` is side Up
**or** front Left (`src/MappedInputManager.cpp:99-107`), and
`OptionPopup::handleInput()` reads those two (`OptionPopup.h:123-130`). So row
actions do not add buttons, they **take the front pair away from scrolling**:

- selection moves on the **side buttons only** (`Button::Up` / `Button::Down`,
  fixed hardware, never remapped — `MappedInputManager.cpp:68-73`),
- the **front pair acts on the selected row**.

Same split the map screen already teaches: side = move through something,
front = do something.

**Opt-in per popup.** A flag plus two callbacks (`onLeft(index)`,
`onRight(index)`). Every existing popup keeps front-button scrolling; only a
popup that asks for row actions loses it. No regression elsewhere.

**Orientation.** Actions read `Button::ScreenLeft` / `Button::ScreenRight` and
label through `mapDirectionalLabels()` (`src/MappedInputManager.h:73`) — both go
through `mapScreenDirection()`, so the label is always on the button that does
the thing. Reading raw `Left`/`Right` and labelling with `mapLabels()` silently
swaps in INVERTED and LANDSCAPE_CCW (`MappedInputManager.cpp:340`): the rider
presses Delete because the hint said Replace.

**Hints.** The hint bar shows the four *front* buttons (`mapFrontLabels()`,
`MappedInputManager.cpp:360`), so the Pins list reads
`Back | Show | Delete | Replace`. Side buttons have no slot there, so the popup
needs its own side hints (`^`/`v`) the way the map draws
`drawZoomSideHints()` — otherwise scrolling looks broken to a rider who just
lost the front pair.

**Touch** (X4 Pro): a tap on a row is Show. Delete and Replace stay
button-only.

### Add / Replace

Lists all ten slots (scrolls, four steps). Existing slots show their distance.

- Empty slot → save at the last fix, no confirmation.
- Occupied slot → confirm: `Replace Parking with current location?` /
  `Cancel` — `Replace`. Two-option confirm precedent:
  `src/activities/reader/EpubReaderBookmarksActivity.cpp:141`.
- **No fix, or a stale one** → refuse with a reason; never write 0,0. Show the
  fix's age when it matters ("fix 4 min old"): a Replace on a dead link saves
  where the rider *was*.

### Delete

Always confirmed: `Delete Parking?` / `Cancel` — `Delete`. Clears the slot and
appends a `del` record. **Nothing on the card is erased.**

### Feedback after a save

Short confirmation — `Parking saved` — then back to the map. Dismissed by a
timer in `loop()` or any button. The distance line from the original sketch is
dropped: a pin saved at the current position is always 0 m.

### Show on map

Reuses observation mode wholesale (`docs/map-observation-mode.md`): render the
viewport around the pin's coordinate, switch to `MapScreenMode::Observe` so the
next fix does not yank the frame back, highlight the pin. GPS keeps being
recorded; navigation and the route are untouched.

Return already exists: the menu's `Follow mode` row snaps back to the real
position (`MapActivity.cpp:2321` restores the stored return anchor).

## Drawing

### In the viewport

Drawn by `MapActivity` straight onto `GfxRenderer`, in the same pass as the
position marker — not by `MapRenderer`. That is where the marker lives, and it
is the layer the host preview does not cover (parent `docs/device-preview.md`),
so pins will not appear in the webapp's firmware panel. Say so there rather than
letting someone chase it as a bug.

Order, closest to the glass last:

1. map context
2. relevant pins
3. active route
4. position and heading

The rider's marker and the route stay the strongest things on the panel. A pin
is a glyph with a white halo, anchored at its screen point.

### Off-screen pins

**The only part that changes the map's drawing model, and the only part gated on
a measurement.**

Today a new fix usually does *not* redraw the frame: the marker's pixels are
saved, restored and moved, and two small rectangles are refreshed
(`MapActivity::saveMarkerPatch()`, `MapActivity.cpp:2577`, `docs/map-follow.md`).
No card read, no whole-panel waveform.

An edge indicator's content depends on where the rider is — bearing and distance
both move with every fix. Naively that is e-ink drawing at the screen edges
continuously for a whole ride: ghosting, battery, a visible flicker per fix.

The plan:

- Same patch technique as the marker, one rect per indicator.
- **Quantised**: redraw only when the bearing moves past a threshold or the
  distance changes by more than a percentage. A parked rider draws nothing.
- Placed where the bearing ray leaves the screen rect, clamped to a margin — not
  snapped to four corners.
- **Crowding**: indicators that would overlap merge into one glyph with a count.
- Bearing computed in screen space, after map rotation — the map has rotation
  modes and a north-up mode.

Setting: `mapPinsOffscreen`, next to `mapAutoSyncTiles`
(`src/CrossPointSettings.h:285`), **default OFF**. Pins inside the viewport are
always drawn; they cost nothing beyond the frame already being rendered.

**Open — needs measurement.** Before the default can change: refreshes per
minute at a realistic fix rate, ghosting after an hour, and whether the patch
restore is clean at the panel edges. Measure on the panel, not on the host.

## Distance

Straight line, geodetic, never routing. Equirectangular approximation with a
`cos(lat)` scale — accurate far past any distance that matters here, and cheap
on a target with no FPU.

Formatting: metres below 1 km (10 m steps), one decimal below 10 km, whole
kilometres above. No fix means `—`, never `0 m`.

## Icons

Type glyphs are static, so they come from the Lucide pipeline, not hand-drawn
bitmaps (parent `CLAUDE.md`, Icons):
`freeink-sdk/libs/assets/Icons/tools/gen_icons.py --manifest … --svgdir
freeink-sdk/libs/assets/Icons/lucide/icons --sizes 24 --out …`, consumed with
`GfxRenderer::drawIcon()` (`lib/GfxRenderer/GfxRenderer.h:241`). Precedent:
`src/components/icons/search24.h`. Candidates: `house`, `car`, `flag`, `target`,
`tent`, `hash`.

The off-screen indicator's **direction arrow is dynamic** — it points at a live
bearing — so it stays a hand-drawn vector primitive, the documented exception.

## Console commands

Added to the existing grammar (`src/activities/map/MapCommandParser.h`), which
the USB serial console and the BLE command characteristic both parse, so the two
cannot drift:

```
pin set <key> <lat> <lon> [utc]   create or replace, from anywhere
pin del <key>
pin list                          active pins, one per line
pin log [<offset>]                paged history, newest first
```

`pin log` pages exactly like `missing [<offset>]`, for the same reason: a whole
log does not fit in one reply. `pin set` is what a phone will eventually use;
today it is how a distant pin gets tested without driving to it.

The parser is pure and host-tested — `test/map_command_parser/` gets the new
cases.

## Acceptance criteria, mapped

| Criterion | MVP |
|---|---|
| Fast entry to Pins | yes — CONFIRM menu row (long-press deferred) |
| Create from the device's current position | yes (= last fix from the phone) |
| Phone can send a pin at a chosen position | wire path only (`pin set`); no app UI |
| Base, Parking, Destination, Meet, Camp, `#1`–`#3` | yes, plus `#4`, `#5` |
| Show an existing pin on the map | yes |
| Delete confirmed | yes |
| Replace confirmed | yes |
| Delete never erases the history record | yes |
| Replace keeps the old position in history | yes |
| Pin in the viewport is drawn | yes |
| Off-screen indicator with bearing and distance | yes, default OFF |
| Off-screen can be turned off | yes |
| Distance is straight-line | yes |
| History on the SD card | yes (written; no on-device viewer) |
| A historical pin can be shown on the map | deferred with the history UI |
| A deleted or replaced pin can be restored | data yes, UI deferred (`pin log` + `pin set` recovers it today) |
| Restore writes a new event | `res` op reserved, written when the UI lands |
| History survives a reboot | yes |
| A damaged last record does not invalidate older ones | yes |
| The model allows more custom pins and black-box logging later | yes — one constant, and a record format built to be shared |

Two rows are honestly partial: on-device Restore is deferred, and the phone side
is a wire path with nothing driving it.

## Rules the implementer works under

Not optional, and not restated per phase:

- **Worktree, own branch.** Firmware work does not happen on the checked-out
  branch (parent `CLAUDE.md`). Branch off `develop`.
- **Never flash without asking, every single time.** Build first, *then* take
  the device lock (`python3 tools/x4lock.py`), then ask. Holding the lock is not
  permission. Rebase onto `develop` and rebuild right before any upload.
- **Archive a known-good build** right after it is confirmed on hardware.
- **Resource Protocol** (`CLAUDE.md`): locals under 256 bytes, no bare `new`,
  `tr()` for every user-facing string, `static constexpr` tables, `.reserve()`
  before `push_back` loops.
- **All SD access through `HalStorage`.** SdFat is not thread-safe.
- **Check free heap after each phase that touches the map screen** — the map
  sits around 54 KB free (`docs/map-menu.md`, measured 2026-08-12), and the
  backdrop already spends ~9 KB of it.
- **Document in the same pass**, not at the end. Mark measured apart from read.
- **Screenshots**: never publish or commit one covering Beniakova or Einsteinova
  (parent `CLAUDE.md`).
- Conventional commits, no AI attribution.

Build and test commands:

```
pio run -e default                                   # firmware build
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
cmake --build build/test && ctest --test-dir build/test
```

## Build plan

Seven phases. Each ends buildable and green. Phases 1 and 3 are each worth their
own flash: a pin that saves and survives a reboot is the whole feature, the rest
is presentation.

### Phase 1 — `PinStore`, the log, the console commands (no UI)

Everything here is host-testable, so it lands before a single pixel.

New files under `src/activities/map/`:

- `PinCatalog.h` — the `static constexpr` table, `kPinSlotCount`, key↔index
  lookup.
- `PinRecord.{h,cpp}` — encode and decode one line, CRC32 via `MapCrc32.h`,
  version check, field validation. Pure: no Arduino, no HAL.
- `PinStore.{h,cpp}` — the ten active entries, `apply(record)`, `replay(reader)`,
  mutations that *produce* records, `nextSeq`/`nextId`. Pure.
- `PinLog.{h,cpp}` — the only file that touches SD: `ensureDirectoryExists()`,
  append-open, one-line append, flush, and a streaming line reader with a fixed
  buffer. Keep the parsing in `PinRecord` so tests never need the HAL.

Console: `MapCommandType::Pin` in `MapCommandParser`, with a sub-verb and paged
`pin log` output modelled on `missing [<offset>]`; replies in
`MapCommandConsole`.

Tests: new `test/pins/` (copy `test/map_command_parser/CMakeLists.txt`, add it
to `test/CMakeLists.txt`). Cases, at minimum:

- encode → decode round trip, every op
- bad CRC skipped, older/unknown version skipped, reader continues past both
- torn final line discarded, earlier records intact
- replay order, and `del` clearing a slot
- delete-then-create gives a new `id`, same key
- `seq` continues after a simulated reboot (replay, then append)
- unknown key survives replay and stays deletable
- `utc = 0` records order by `uptime`
- console: `pin set/del/list/log`, paging, bad arity, out-of-range coordinates

**Done when**: host tests green; `pin set` / `pin list` / `pin log` work over
the serial console; a reboot rebuilds the same active pins.

### Phase 2 — `OptionPopup` row actions

Shared component, so it lands alone and gets its own look on the panel before
anything depends on it.

- `src/components/OptionPopup.h`: opt-in row actions (flag + `onLeft` /
  `onRight` + their labels). In that mode selection reads `Button::Up` /
  `Button::Down` only; `ScreenLeft` / `ScreenRight` fire the callbacks.
- Labels through `mapDirectionalLabels()`, never `mapLabels()` (see the
  orientation trap above).
- Side hints (`^`/`v`) for the popup, following `drawZoomSideHints()`.

**Done when**: every existing popup behaves exactly as before (check the map
menu and a settings picker), the new mode is correct **in a rotated orientation
as well as upright**, and a list long enough to scroll still scrolls.

### Phase 3 — the Pins UI

- `Pins` row in `MapActivity::openMapMenu()`.
- The Pins list: existing pins only, distance in the value column, row actions
  wired to Delete / Show / Replace.
- Add / Replace list: all ten slots.
- Both confirmations; the saved feedback; the no-fix refusal and the stale-fix
  warning.
- New state on `MapActivity`, reusing the backdrop capture.

**Done when**: create, replace and delete all work on the device; each one
appends the expected line; a reboot brings the pins back. Check free heap.

### Phase 4 — icons and in-viewport drawing

- Icon manifest, `gen_icons.py` run, generated header committed.
- Draw pins in the marker pass, with the halo and the layer order above.
- Note in the parent's `docs/device-preview.md` that the host preview cannot
  show them.

**Done when**: a pin drawn at a known coordinate lands where it should on the
panel, and does not out-shout the marker or the route. Screenshot — mind the
privacy rule.

### Phase 5 — Show on map, and getting back

- Viewport around the pin, `MapScreenMode::Observe`, pin highlighted.
- Return through the existing `Follow mode` row; verify the return anchor is the
  real fix, not the pin.

**Done when**: Show, then Follow, lands the rider back where they are, with no
change to route or navigation state.

### Phase 6 — off-screen indicators, then the measurement

- `mapPinsOffscreen` in `CrossPointSettings` + `SettingsList.h` toggle + an
  `I18nKeys.h` entry and its `translations/english.yaml` string + the
  `CMD:SETTING` mapping in `src/main.cpp:716`.
- Patch-based edge indicators, quantised, bearing in screen space, crowding
  merged.
- **Then measure on the panel**: refreshes per minute at a realistic fix rate,
  ghosting after an hour, patch restore at the edges. Write the numbers into
  `docs/pins.md`.

**Done when**: the numbers exist and are written down. The default flips to ON
only if they justify it — otherwise it stays OFF and the doc says why.

### Phase 7 — documentation and merge

- `docs/pins.md` in this repo: the mechanism as built, measured apart from read,
  every claim cited.
- Update this file's status; it stops being a plan.
- Parent repo: `docs/PROGRESS.md` entry, README pointer, and a look at whether
  `web/` should say anything (only once it works on hardware).
- Merge to `develop`, push, remove the worktree.

## Open items

- Off-screen refresh cost — must be measured before the default flips.
- Stale-fix threshold: how old is too old for a save, and is it a refusal or a
  warning?
- Log rotation: at what size, and does it ever discard, or only fold into a
  snapshot?
