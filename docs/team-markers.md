# Team markers: other riders on the map

Who else is out there, where they were last seen, and what the card keeps when
the ride is over.

Built 2026-09-16 and **run on an X4 Pro the same day** -- see "The hardware
pass" at the end for what was actually exercised and what still is not. The
rules below are proved by host tests (`test/team/`) unless a line says
otherwise.

## What it is

Each approved member of a riding group gets a marker on the map: the same pin
shape a rider's own pins use, with a **filled head carrying their two or three
letter acronym**. A rider's own pins are hollow-headed, so a person and a place
cannot be confused at a glance.

Three parts:

1. **The roster** -- `/trailink/team/members.json`, an allowlist of who we are
   willing to hear from.
2. **The live positions** -- in RAM, one per roster slot, drawn on the map and
   listed under Group in the map menu.
3. **The black box** -- `/trailink/team/bb-YYYYMMDD.csv`, every accepted
   position, one file per day, plus the rider's own trace when they turn it on.

It answers *where is everybody* and *where were they last seen*. It is not
navigation and it does not route anybody anywhere.

## The layer does not know which radio spoke

`MapTeam::teamPosition()` is the only way a position gets in. A LoRa frame, a
BLE relay from the phone and a `team pos` console line all call it, all pass the
same allowlist, write the same row and move the same marker.

That is the decision the parent repo's `docs/lora.md` made before any radio
existed ("What can be built today, with no hardware"): the source of other
riders' positions is an abstraction, not "LoRa". **No transport is
implemented.** Today the only caller is the devel console, which is enough to
put markers on a panel and judge them.

## A stranger is not a member

The roster is an allowlist, not a contact list. A position from an id that is
not in it is **refused, counted and logged** -- never drawn, never written to
the black box. `MapTeam::strangersRefused()` holds the count since boot.

This is the case the feature has to get right: a radio hears whoever is in
range, including another group on the same channel and anyone who wants to put
a false rider on somebody's panel. BLE advertising here runs with no pairing and
no bonding (`ble-advertising.md`), so "in range" is the only credential a
stranger needs.

Two identifiers per member, because they answer different questions:

| field | what it is |
|---|---|
| `id` | what the transport calls the sender -- a MeshCore key prefix, a node id, a BLE address. Machine text, checked rather than trusted. |
| `acr` | the two or three letters on the panel. Chosen by the rider, uppercased on the way in, unique in the roster. |
| `name` | optional, for the Group list only. Never on the map: the head fits an acronym and nothing else. |
| `on` | muted rather than deleted, so a member who is not riding today keeps their acronym and their history. |

`ME` is reserved: it is the rider's own row in the black box, and a member under
that acronym would put two people on one name in the file somebody reads when
looking for them.

Twelve members, fixed array, no heap.

## The roster file

`/trailink/team/members.json`, rewritten whole on every edit:

```json
{"v":1,"members":[{"id":"a4c1380c","acr":"RF","name":"Roman","on":true}]}
```

There is no append-only history here, unlike the pins log: a roster is a small
setting, edited by a human between rides. The history of this feature lives in
the black box instead.

An unknown `v` is **refused whole** and the roster is left empty, which refuses
every incoming position rather than letting strangers through. A single
malformed row is skipped and counted (`team reload` prints `team_skipped`) --
one bad row must not cost the rider the other eleven members.

Edited three ways: by hand on the card, then `team reload`; or with `team add` /
`team del` over the console, which rewrite the file.

## The black box

One CSV row per accepted position, header written when a file is created:

```
utc,uptime_ms,who,lat,lon,heading,speed_kmh,src
1789430400,81234,RF,48.1486000,17.1077000,4,62,lora
```

| field | notes |
|---|---|
| `utc` | the sender's clock; **0 means they had no clock**, never a time we invented |
| `uptime_ms` | our uptime at receipt -- what orders rows inside a run with no clock |
| `who` | member acronym, or `ME` for the rider |
| `lat`, `lon` | decimal degrees, 7 places, integer-formatted (there is no FPU) |
| `heading` | 0-15 sixteenths of a turn, empty when not sent |
| `speed_kmh` | empty when not sent |
| `src` | `cmd` / `ble` / `lora` / `gnss` / `unknown` |

CSV rather than the pins log's framed-and-checksummed lines, deliberately: this
file is read by a person on a laptop, possibly by somebody looking for a rider,
and `power.csv` and `gnss.csv` set that precedent. No CRC for the same reason --
a torn last line is visibly torn, and a checksum that makes a rescuer's
spreadsheet refuse a row helps nobody.

**The card is written first and the marker moves only if that worked.** Same
rule as the pins log, same reason: RAM claiming a position the card never
recorded survives to the next reboot as a lie.

### Rotation is by whole days

`bb-20260916.csv`, one file per day. Rotation deletes whole files older than
`mapTeamKeepDays` (14 by default), so the oldest evidence goes first and a file
a laptop is reading never changes under it.

A device with no clock writes `bb-noclock.csv`, and **rotation never deletes
it**: a day it cannot name is a day it cannot judge. Every X4 is such a device
until the phone hands it a time.

### What comes back at boot

`TeamBlackBox::replayLast()` rebuilds every member's last known position from
the card, so a member heard on the previous ride is on the panel before anybody
transmits again.

Newest evidence wins: dated files newest first, then older ones only for the
members those did not answer for, then `bb-noclock.csv` last. A dated row always
beats an undated one.

The walk is by **file name, not by the clock** -- the clock is exactly what the
device may not have at boot.

## Fresh, stale, hidden

| state | drawn as | when |
|---|---|---|
| fresh | solid black balloon, white initials | younger than `mapTeamStaleMin` (5 min) |
| stale | grey balloon, black initials | older than that |
| hidden | not drawn | older than `mapTeamHideMin` (30 min); 0 turns hiding off |

**Grey, not hollow.** A hollow marker is a different *shape* of thing and reads
as a different kind of mark rather than as the same person an hour older;
fading is what everyone already reads as "older" without being told. Maintainer's
call, 2026-09-16, on the first version. Grey on this panel is a dither
(`eink-grayscale.md`), painted row by row inside the silhouette because a
rectangle of it would spill past the shape -- and it is the **light** dither,
because black initials on the dark one are mush at this size.

Age prefers the sender's own timestamp when both clocks exist, because that is
when the rider was actually there, and falls back to our receipt uptime for a
fix heard this run.

**A position whose age cannot be known is never hidden.** It draws as stale. A
replayed fix on a device with no clock has no knowable age, and hiding what it
cannot date would empty the panel at exactly the moment a rider wants to see
where everyone was.

## On the panel

`MapActivity::drawTeam()`, straight onto `GfxRenderer` right after the pins and
for the same reason: a marker is not map data, and `MapRenderer` knows nothing
about it. So the webapp's firmware preview panel cannot show team markers
either.

**The whole balloon is filled, not a disc inside its head.** Maintainer's call,
2026-09-16, made on a simulator frame and confirmed on the second one: a black circle
inside a white balloon reads as a pin with a dot in it, and the thing that has
to be obvious at a glance is person-versus-place. `scripts/gen_pin_icons.py` now
bakes a third array for that -- `kPinShapeBody0Bits`, the silhouette with no
halo around it, upright only, because a team marker never rotates and sixteen
more arrays would be flash spent on nothing.

So a **current** member is a solid black balloon with white initials, and a
**stale** one is the same balloon in grey with black ones -- the shape never
changes, only how dark it is.

**The letters are as large as they fit**, which a filled balloon allows and a
hollow one did not: outline and fill are the same ink, so a letter only has to
stay inside the silhouette rather than clear of an outline. The ladder is
`UI_12`, `UI_10`, `SMALL`, `MAP_SMALL`.

**What "fit" means is geometry, not a magic number.** A word is a band through
the middle of the head disc, and a circle is narrower there than at its
diameter, so the bound is the disc's **chord at the cap height**: the generator
emits the disc's outer radius (`kPinShapeHeadOuterRadius`, 20 px) and the cap
height is taken as the usual ~0.72 of the ascender, with a pixel of margin each
side. Three attempts got there -- a flat 26 px, then 28, then the clear radius a
baked glyph uses, which is the wrong circle for a filled mark.

**Measured on the ink, not on the advance.** `getTextWidth()` counts both side
bearings; `JKL` carries a wide trailing bearing on its `L` and sat visibly left
inside the head while `RF` did not. The first and last glyph's metrics
(`EpdGlyph::left`, `width`, `advanceX`) give the real ink box, and both the fit
test and the centring use it.

With those in place: `RF` takes UI_12 (ink 27 px against 34 usable), `MK` UI_10
(32 against 34), `JKL` SMALL (29 against 36). **Two acronyms of the same length
can land on different faces**, because letter shapes differ -- each marker keeps
the largest face its own letters fit rather than the whole group dropping to
what the widest member can take.

**Centred on the head circle, and on the capitals.** Two corrections, both
made 2026-09-16 after the letters read low and right on the panel:

- `headX`/`headY` in the baked frame are *not* the circle's centre. They carry
  `--glyph-dy`, the downward nudge that makes a Lucide glyph read right inside
  the head, and text does not want it. The generator now emits the circle's own
  centre as `kPinShapeHead0X`/`Y` and the letters use that.
- `drawText`'s `y` is the top of the ascender box, so centring on the line
  height hangs an acronym low by half a descender -- which uppercase and digits
  never use. The letters are placed so the cap band centres instead (cap height
  taken as the usual ~0.72 of the ascender).

Measured off the rendered frame afterwards, against the asset's own geometry:
`RF` lands dead centre, `MK` within half a pixel.

`MapActivity::drawTeamBalloon()` logs each acronym's chosen face, its ink box and
the usable width at `LOG_DBG`. That line is what settled every number above, and
it is the only way to tell "the font stepped down" from "the letters are drawn
wrong" without a ruler on a screenshot.

**No off-screen edge markers yet.** The pins' edge markers merge overlapping
marks into one arrow with a count, and a merged marker that eats somebody's
initials answers the wrong question. Until that is designed, a member outside
the viewport is counted in the log and carried by the Group list, which has
their distance and direction.

### The label under a marker

The balloon says who and whether the position is current. The line under it says
**how far** and **how old**, and it has four shapes:

| label | when |
|---|---|
| *(nothing)* | current, and close enough that the map itself shows the distance |
| `1.2 km` | far: the rung is zoomed out past rung 2 (6 m/px, 2.9 x 4.8 km on the panel) |
| `1.2 km/15m` | far and stale |
| `15m` | stale, but close enough to judge the distance by eye |

So a group riding together draws no text at all, which is the common case and
the one that must stay clean. Age appears exactly when the marker goes hollow,
so the two say the same thing at two resolutions; `?` is an age that cannot be
known, which is every replayed fix on a device with no clock. **Under a minute
the age is left out** -- `0m` reads as information and carries none, and there
the hollow head is the whole message.

**Both numbers are quantised, and neither is an accident.** The distance goes in
100 m steps under a kilometre, tenths to ten, whole kilometres above -- the same
grid the map's destination readout uses. The number behind it is a position that
arrived minutes ago from a fix good to tens of metres, so a metre of it is noise
dressed as precision, and a digit that moves with every fix is a waveform pass
per fix on a panel that would otherwise hold its frame. Under 100 m it says
`100 m` rather than `0 m`: no fix this old can promise they are on top of you.

**Ages are rounded up, to five minutes.** Up rather than down because a position
must never be claimed fresher than it is, and to five because that is how often
the panel revisits it (below): a finer number would be wrong between refreshes.

### The panel re-reads the ages every five minutes

What is on the glass is only as fresh as the last redraw, and what makes the map
redraw is a **fix**. A rider whose phone has stopped talking, or whose group has
gone quiet, would otherwise read a frame saying everyone is current for as long
as they look at it -- which is exactly the moment the ages matter.

So `MapActivity::serviceTeamAges()` revisits them on their own clock, one step
apart (`kTeamAgeRefreshMs` = `kTeamAgeStepMinutes`), and **redraws only when the
frame would differ**: it folds every member's visibility and reported age step
into one number and compares it with the last drawn one. A group that has gone
quiet therefore costs one compare every five minutes and no refresh at all once
the last marker is past hiding. It does not stamp the busy badge -- that belongs
to a press the rider made, and nobody asked for this frame.

**A label that cannot find a clear spot is dropped, not overprinted.** It tries
four places around the marker -- under the point, above the head, then either
side -- against the balloons already drawn, the labels already placed, the
rider's own marker and the screen's furniture. Two numbers on top of each other
read as one wrong number. The count of dropped labels goes in the debug line
next to the drawn/too-old/off-panel counts.

It carries a white halo, drawn as four offset passes before the black text: at
`SMALL` on a map full of road lines and area dither it is otherwise unreadable
(simulator, 2026-09-16).

### The Group list

Map menu > **Group**. One row per member: acronym, optional name, and a value
column with distance, compass sector and age (`1.2 km NE 4m`). Confirm shows
that member on the map, which drops into Observation mode exactly as showing a
pin does.

`--` in the value column means *they have not told us where they are*, which is
a different statement from a coordinate we do not trust. `?` for the age means
the position cannot be dated.

## The console, devel builds only

```
team pos <acr|id> <lat> <lon> [utc <s>] [heading <0-15>] [speed <kmh>] [src <word>]
team add <acr> <id> [<name>]
team del <acr|id>
team list
team reload
team log [<offset>]
```

Gated behind `-DENABLE_TEAM_CMD`, which is in no release environment. Two
reasons, either one enough:

- `team pos` injects a position under somebody else's name -- a forged rider on
  the panel of whoever holds the device.
- `team list` prints where the whole group is, to anyone in BLE range, with no
  pairing and no ownership check.

The tail of `team pos` is keyword-only, unlike `pos`: after a coordinate a bare
number is as likely a unix time as a heading, and guessing wrong puts a member
on the panel with an age that is off by years.

`team list` prints **empty coordinate fields** for a member who has not been
heard from. Not `0,0` -- that is a place in the Atlantic, and the difference
between "has not told us" and "is at 0,0" is one the whole layer has to keep.

## Two switches, because it is two kinds of data

| setting | default | what it does |
|---|---|---|
| `mapTeamMarkers` | on | draw the markers at all |
| `mapTeamLogPeers` | on | write the group's positions to the black box |
| `mapTeamLogSelf` | **off** | write the rider's own trace to the same file |
| `mapTeamStaleMin` | 5 | minutes before a marker goes stale |
| `mapTeamHideMin` | 30 | minutes before it is not drawn (0 = never hide) |
| `mapTeamKeepDays` | 14 | days of black box kept on the card (0 keeps everything) |

A member's position is theirs, sent to us on purpose. The rider's own trace is a
record of where *they* went, on a card that is lost with the device -- so it is
their call, the same call `mapGnssLog` made, and the settings row says "Record my
track too" rather than naming the file.

The rider's own rows are **rate-limited**: at most one every 30 s, or one every
25 m, whichever comes first. A phone pushes a fix a second and a row per fix
would be a card write per second for a whole ride. Peers are not rate-limited --
a radio sends a position rarely, and every one of them is worth keeping.

The rider's rows carry no speed: the fix path into the map has a coordinate and
a heading, and inventing a number for the column would be worse than an empty
one.

The first three settings have rows in Settings > Map. The three numbers do not:
they are set once, if ever, and three more rows would push the ones a rider uses
off the first screen. They are in the settings file and nowhere else.

## Building it, or not building it

Two flags, and both are off by default:

- `-DENABLE_TEAM_MARKERS=1` -- the whole layer. Without it none of this
  compiles: no roster, no black box, no markers, no Group row, no settings
  fields.
- `-DENABLE_TEAM_CMD=1` -- the console commands. Needs the first one, and says
  so with an `#error` if it does not get it.

On in `env:x4pro`, `env:t5s3pro` and `env:simulator`. **Off everywhere else**,
including every release environment and every C3 build -- the X4 and the X3
share one binary, and the X3 is the device that cannot afford the DRAM.

### What it costs

Measured 2026-09-16 on the C3 -- the chip the argument is about, since the X3 and
the X4 share that binary. Same commit, same environment (`env:default`), one
build with the flags and one without, twenty minutes apart:

| | flash | static RAM |
|---|---|---|
| flags off | 4,068,927 B (62.1 %) | 59,140 B (18.0 %) |
| flags on | 4,087,457 B (62.4 %) | 59,140 B (18.0 %) |
| **the feature** | **+18,530 B** | **0 B** |

The static RAM is unchanged because the roster and the store live inside the
`MapActivity` instance, which is not a static object: their ~1 kB is heap, held
only while the map screen is up. **That kilobyte has not been read off a
device** -- a `heap` figure with the map open, flags on and off, is the half of
T-2020 still outstanding.

`env:x4pro` with both flags on links at 3,904,998 B flash (59.6 %) and 69,780 B
static RAM (21.3 %), clean, no warnings from any of the new files.

## What is open

- **Daylight and ghosting.** The marks were read on the panel indoors (below).
  Whether a solid black balloon ghosts where a hollow one does not, and how both
  read in direct sun, is a ride and not a desk test.
- **Flash and DRAM cost**, measured against the same tree with the flags off.
- **Off-screen members**: no edge marker, by decision, until a design exists
  that does not merge two people into one arrow.
- **A transport.** LoRa is the parent repo's `docs/lora.md` bring-up order; the
  BLE relay through the phone is not designed either. Both plug into
  `MapTeam::teamPosition()` and change nothing above it.
- **Anti-spoofing beyond the allowlist.** A roster id is whatever the sender
  claims. MeshCore adverts are Ed25519-signed, which is the reason that protocol
  was picked, but nothing here verifies a signature yet -- the allowlist stops a
  stranger, not somebody replaying a member's id.

## The hardware pass

X4 Pro, 2026-09-16. Flashed twice: `47aa6817` for the first pass and
`e125db7c` once the letters were resized and centred (both archived as
`docs/firmware-builds/2026-09-16-x4pro-team-markers-*-good.*` in the parent
repo). Driven entirely over the USB console, no radio and no phone.

What ran:

- `team add` three times, then `team pos` three times, then `pin set camp` for
  something to compare against. Every one answered `OK`.
- **Three members on the panel**, solid black balloons with white initials --
  `RF` at UI_12, `MK` at UI_10 and a three-letter `JKL` at SMALL, each centred in
  its head. The hollow camp pin sits next to them in the same frame
  (`docs/device-shots/2026-09-16-x4pro-team-markers-480x800.png`, reshot after
  the sizing fix).
- **The Group list**: `RF Roman  450 m NE 1m`, `MK Marek  440 m SW 1m`,
  `JKL Jakub  480 m S 1m` (`...-team-group-list-480x800.png`).
- **The black box on a real card.** `team log` read back three rows, newest
  first, each with `utc` 0 -- the device had no phone and therefore no clock,
  and it recorded that rather than inventing a time.
- **The boot replay.** A hard reset, then back into the map:
  `roster: 3 member(s), 0 row(s) skipped` / `black box: 1 file(s) read, 3 member
  position(s) restored` / `team: 3 drawn, 0 too old, 0 off the panel`. The three
  came back **hollow with black letters**, which is the age-unknown rule doing
  exactly what it says: a replayed fix on a device with no clock cannot be
  dated, so it draws as stale and is never hidden
  (`...-team-after-reboot-480x800.png`).

What did **not** run: any radio, any phone, a stale-by-clock marker (that needs
a device that knows the time), a rejected stranger over a real transport, and
day rotation (one file, one day).
