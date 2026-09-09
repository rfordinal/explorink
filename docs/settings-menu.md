# Settings menu: what the rider sees, and what was taken away

ExplorInk inherited CrossPoint's whole e-reader Settings screen. Most of it
configures books. This file says which rows went, which stayed, why, and what a
later pass still owes.

Status on this branch (`release/lilygo-t5-s3-pro`): **flashed and read on a
LilyGo T5 S3 Pro, 2026-09-07.** All four tabs were grabbed off the panel with
`CMD:SCREENSHOT` at the board's own 540x960. The `develop` side is built only:
`pio run -e default` (X4) and `-e simulator` link clean, and the simulator's
480x800 captures are host renders on the X4 profile.

## The staged removal, and why nothing is deleted yet

The reader is going away entirely, but that is a delicate operation: reader
activities, the file browser, EPUB/TXT/XTC parsing and the KOReader and OPDS
stacks all hang together. This pass is the **facade** only. A row stops being
reachable; the setting behind it keeps existing.

That distinction matters because `getSettingsList()` does two jobs:

- it builds the Settings screen (`SettingsActivity.cpp:52`), and
- it drives `settings.json` serialisation, both ways
  (`CrossPointSettings.cpp:66` writes, `:140` reads).

Delete an entry and the field stops being saved, so every device carrying a
stored value silently resets it on the next boot. So a hidden row keeps its
entry and gets `withHidden()` (`SettingsActivity.h:56`, `hiddenFromMenu`), which
only `rebuildSettingsLists()` looks at. Deleting the entries is the later,
real removal.

The old web settings page would have been the other consumer, but it was
removed before this (`webserver-endpoints.md`, "Device settings belong in the
device menus").

## Four tabs, not five

`categoryNames[]` (`SettingsActivity.cpp:30`) is now Display, Map, Controls,
System. The Reader tab is gone, and with it three sub-screens that were only
reachable from it:

| Went | What it was |
|---|---|
| Text Settings | 10 rows: font family, size, line spacing, margin, alignment, embedded style, focus reading, hyphenation, paragraph spacing, anti-aliasing |
| Manage Fonts | SD card font install/delete |
| Customise Status Bar | 11 rows, including the clock |

The status bar those 11 rows configure is the **reader's**. The map screen
draws its own header row and reads none of them — it takes the time from the
phone's BLE packet and ignores `SETTINGS.clockFormat` outright
(`MapActivity.cpp:2151-2153`, and `map-header-status.md`).

`TextSettingsActivity`, `StatusBarSettingsActivity`, `KOReaderSettingsActivity`
and `OpdsServerListActivity` are all still compiled and still constructible.
Only the rows that started them are gone.

## Frontlight: a row added, on the boards that have one

**Added 2026-09-07.** Display gained a **Frontlight** row, compiled in only where
the board has one (`#if FREEINK_CAP_FRONTLIGHT` in `SettingsList.h`). It is a
`SettingType::VALUE`, 10 to 100 % in tens, rendered with a `%` suffix (the
`STR_FRONTLIGHT` case in `SettingsActivity.cpp`).

Two things make it unusual and both are deliberate:

- **It carries no JSON key.** `frontlightOn` and `frontlightBrightness` are
  serialised by hand (`CrossPointSettings.cpp:109-110`, `:238-244`), so a list
  entry with a key would write the same field a second time.
- **Off is not one of its values.** Off is a state the buttons produce -- the
  home key's hold toggles, the user button's hold walks the rungs -- and storing
  it would lose the level the rider chose.

`loop()` applies a change while the light is on and never turns it on: choosing a
level is not a request for light. The gestures that share this number are in
`docs/lilygo-t5s3-bringup.md`, "The remap, 2026-09-07".

## Rows hidden, and the consumer that proves them reader-only

Each of these was hidden because its only consumer is a reader activity. The
citation is the consumer, not the definition.

| Row | Tab | Only consumer |
|---|---|---|
| Refresh Frequency | Display | `ReaderActivity.cpp:34` — it counts pages of a book |
| Touch Reader Controls | Controls | `ReaderUtils.h` |
| Orient front buttons | Controls | `MappedInputManager.cpp:48`, keyed on the reader's orientation |
| Long-press button behavior | Controls | `EpubReaderActivity`, `XtcReaderActivity` |
| Long-press Menu | Controls | `EpubReaderActivity.cpp` — options are KOReader sync, bookmark, dictionary |
| Quick-return from footnotes | Controls | `ReaderUtils.h` — footnotes are an EPUB concept |
| Short Back to File Browser | Controls | `ReaderUtils.h` |
| Tilt Page Turn (X3 only) | Controls | the IMU, turning book pages |
| Show Hidden Files | System | `FileBrowserActivity.cpp` |
| Clear Read Books from Recent List | System | `EpubReaderActivity.cpp` |
| Move Finished Books to Read Folder | System | `EpubReaderActivity.cpp` |

Three System actions went the same way: **KOReader Sync** (reading-position
sync), **OPDS Servers** (an e-book catalogue) and **Clear Reading Cache** (the
reader's pagination cache).

## Screen Orientation: the one dimmed row

`CrossPointSettings::orientation` moved from the dead Reader tab into Display
under a new label, `STR_SCREEN_ORIENTATION` ("Screen Orientation"), and is
drawn **disabled**.

It is not hidden because a device carried in landscape — on a mount or in a
hand — will want a real screen orientation, and this is the field that will
carry it. It is not active
because today the field rotates the reader only — `EpubReaderActivity`,
`TxtReaderActivity` and `SleepActivity` read it, the map and the rest of the UI
do not. Offering it would rotate nothing the rider is looking at.

Three mechanics behind `disabled` (`SettingsActivity.h:59`):

- The row draws dimmed through `drawList()`'s `rowDimmed` callback
  (`BaseTheme.h:316`), a checkerboard dither on the text
  (`BaseTheme.cpp:532`).
- **The dither is skipped on the selected row** (`BaseTheme.cpp:532`, `i !=
  selectedIndex`), so dimming alone tells a rider standing on the row nothing.
  The Confirm hint is therefore blanked for a disabled row.
- **What an empty hint label draws is per theme, and the device does not run
  the plain one.** `BaseTheme::drawButtonHints()` skips an empty label and
  draws nothing (`BaseTheme.cpp:261`). `LyraTheme` — the default, and what the
  T5 S3 Pro was running — draws a **short stub box** with no text instead
  (`LyraTheme.cpp:399-404`), so the slot is not empty, it is visibly shorter
  than its neighbours. Seen on the panel 2026-09-07.
- **The stub is not tappable.** `rememberFrontLabels()` records only non-empty
  labels (`BaseTheme.cpp:384-387`), `frontBoxActive()` gates the hit test on
  that flag (`BaseTheme.cpp:394`), and `frontHintBox()` refuses to return a
  rect for an inactive slot. So in BUTTONS touch mode the stub is drawn and
  dead, which is the behaviour wanted — but it was arrived at by accident, not
  designed. **Read, not measured.** The hardware pass ran with Touch Screen on
  Buttons only, where no list row is tappable at all, so the tap path was never
  exercised — only the Confirm button was. Setting touch to Anywhere and
  tapping the row would settle it.
- `toggleCurrentSetting()` returns early, so both the button path and the
  touch-tap path are inert.

**Confirmed on the panel, 2026-09-07.** Four tabs, cycling Display -> Map ->
Controls -> System -> Display. Refresh Frequency gone from Display; KOReader
Sync, OPDS Servers and Clear Reading Cache gone from System. Screen
Orientation draws last in Display, dithered. With the cursor parked on it,
Confirm pressed three times changed nothing and opened no popup, and the
maintainer's reading of the panel was "riadok je mrtvy" — the row reads as
dead. Two absences are not this change and were checked as such: Sunlight
Fading Fix (dropped on any touch board) and Check for updates (OTA is gated on
`!hasTouch`).

**Still open: `settings.json` was not read back.** The hidden rows keep being
serialised in theory, and the visible settings did survive the flash with the
rider's own values rather than defaults — but `CMD:SETTING` is a four-key
allow-list (`main.cpp:1335-1352`) and none of the hidden keys is in it, so
nothing was actually read off the device. Pulling the card or reaching it over
WebDAV would settle it.

**Only the label dims, not the value.** The simulator capture shows "Screen
Orientation" in dither grey with "Portrait" beside it in solid black:
`drawList()` applies the dither to the row title and draws the value at full
weight. It reads acceptably — the label is the part that says whether the row
is live — but it is not deliberate, and a hardware pass should say whether the
mixed weight is confusing on the panel.

**`RoundedRaffTheme` ignores `rowDimmed` entirely** (`RoundedRaffTheme.cpp:284`,
`(void)rowDimmed;`). Under that theme the row looks ordinary and only the
missing Confirm hint gives it away. Open — either that theme learns to dim, or
the row needs a second cue.

## What this pass did not do

- **Enum option lists were left alone.** Two rows offer book-only choices among
  useful ones: Hide Battery % has `In Reader`, and Short Power Button Click has
  `Page turn` and `Footnotes`. They cannot simply be dropped: enum settings are
  **persisted by index** (`SettingsList.h:194`, the comment at the top of
  `getSettingsList()`), so removing a middle option silently reassigns the
  values after it. That needs a migration, not an edit.
- **`STR_SIDE_BTN_LAYOUT` still reads "Side Button Layout (reader)".** The
  label is wrong: the field is a global remap in `MappedInputManager.cpp` and
  applies to the map's zoom ladder too. Renaming it in `english.yaml` alone
  leaves 30-odd translations saying "(reader)", so it wants its own pass.
- **No reordering.** The rows inside each tab are still in the order the
  reader-era list put them in.
