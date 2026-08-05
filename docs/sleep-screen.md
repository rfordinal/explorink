# Sleep screen

Verified against branch `feat/sleep-location-screen` (based on `develop`
`7ee28192`) unless noted. Inherited wholesale from upstream CrossPoint;
TrailInk has repurposed one mode (`COVER` → `LOCATION`, see below) and changed
one default value.

## Where it lives

`SleepActivity` (`src/activities/boot_sleep/SleepActivity.h:6-20`) is the one
`Activity` entered on every sleep (both idle timeout and manual sleep — see
`fromTimeout` ctor arg, `SleepActivity.h:8-9`). `onEnter()`
(`SleepActivity.cpp:17-58`) is the dispatcher; it picks one of six render
methods.

Sleep entry itself is `enterDeepSleep()` (`src/main.cpp:225`), which at
line 227 does:

```cpp
APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
```

before tearing anything down. This is the only "what was the app doing"
signal the sleep screen gets.

## The mode setting: `SETTINGS.sleepScreen`

Enum `SLEEP_SCREEN_MODE` (`src/CrossPointSettings.h:18-29`):

```cpp
DARK = 0, LIGHT = 1, CUSTOM = 2, LOCATION = 3, LOCATION_CUSTOM = 4, BLANK = 5, QUICK_RESUME = 6
```

`LOCATION` / `LOCATION_CUSTOM` are TrailInk's rename of upstream's `COVER` /
`COVER_CUSTOM` (`CrossPointSettings.h:22-25`) — same numeric values, same
slot in the menu, different content: no book cover, so this now shows the
last known map fix instead. See "Repurposing COVER" below.

User-facing labels: Settings > Display > Sleep screen, `src/SettingsList.h:196-213`.
Persisted at `/.crosspoint/settings.json` (`CrossPointSettings.h:376`).
Default is still `DARK` (`CrossPointSettings.h:179`) on this branch — see
"Default" below.

Dispatch in `onEnter()` (`SleepActivity.cpp:38-57`):

| Setting | Renders | Fallback |
|---|---|---|
| `BLANK` | `renderBlankSleepScreen()` — clear screen, half refresh | — |
| `CUSTOM` | `renderCustomSleepScreen()` — random BMP from `/.sleep` or `/sleep` on SD, or a fixed `/sleep.bmp` if present | `renderDefaultSleepScreen()` if no BMP found |
| `LOCATION` | `renderLocationSleepScreen()` (`:255-279`) — last known map fix, LIGHT-style (never inverted) | none needed — see below |
| `LOCATION_CUSTOM` | Location info if `SETTINGS.mapHasLastFix`, else `renderCustomSleepScreen()` (`:45-54`) | `renderCustomSleepScreen()` |
| `QUICK_RESUME` | falls through `switch`'s `default:` to `renderDefaultSleepScreen()` — but see Quick Resume below, it's actually intercepted earlier | — |
| `DARK` / `LIGHT` | `renderDefaultSleepScreen()` (`:158-173`) — logo + "Sleeping" text, inverted to black unless `sleepScreen == LIGHT` (`:168-170`) | — |

`DARK` and `LIGHT` are not two render paths — they're the same
`renderDefaultSleepScreen()`, and `LIGHT` just skips the `invertScreen()` call.

## Repurposing COVER: `renderLocationSleepScreen()`

Upstream's `COVER` mode rendered the currently-open book's cover, parsed live
from the open XTC/TXT/EPUB (`APP_STATE.openEpubPath`) — real work: file load,
cover BMP generation, three format branches, a function-pointer fallback
chain for every failure point (no book open, load failed, cover-gen failed,
BMP header invalid). None of that applies to a device that doesn't open
books during a ride. TrailInk's `LOCATION` mode replaces the whole thing with
a single bool check and a `snprintf` (`SleepActivity.cpp:263-276`):

```cpp
if (SETTINGS.mapHasLastFix) {
  // format "<lat>, <lon>  <compass>" into buf, draw it
} else {
  renderer.drawCenteredText(SMALL_FONT_ID, ..., tr(STR_SLEEPING));
}
```

Data source: `SETTINGS.mapHasLastFix` / `mapLastLatE7` / `mapLastLonE7` /
`mapLastHeading` (`CrossPointSettings.h:244-247`) — already persisted by
`MapActivity` so re-entering Map (or a reboot) has something to show before
the next GPS/BLE fix arrives. The sleep screen reads the exact same fields,
so showing "where we last were" costs nothing extra to compute and needs no
boot into Map.

`mapLastHeading` is a 0-15 step index into the 16-point compass
(`MapHeading.h:11`, `N, NNE, NE, ENE, ... NNW`), not raw degrees — confirmed
by its only writer, `MapCommandConsole::heading()` (`MapCommandConsole.h:95`,
comment "0-15, see MapHeading.h"). `renderLocationSleepScreen()` keeps its
own small abbreviation table in the same order (`SleepActivity.cpp:267-268`)
rather than including `MapHeading.h`, to avoid a boot_sleep → map header
dependency. Masked with `& 0x0F` before indexing as a cheap guard against a
stale/corrupt persisted value, even though every known writer stays in
0-15.

Because the data source can't fail (a bool read, never a missing file), there
is no fallback chain inside `renderLocationSleepScreen()` — unlike the old
`renderCoverSleepScreen()`'s function-pointer dance. The only "no data" case
(`mapHasLastFix == false`, i.e. no fix has ever landed since flash) shows the
plain "Sleeping" text instead of a coordinate line, still in the same
LIGHT-style layout (logo, never inverted).

`LOCATION_CUSTOM`'s gate was first ported unchanged from `COVER_CUSTOM` —
`APP_STATE.lastSleepFromReader` — then changed to `SETTINGS.mapHasLastFix`
after hardware testing showed the reader gate never fires on TrailInk (see
"Hardware finding" below).
`renderer.invertScreen()` is never called in either DARK/LIGHT check nor
LOCATION today so LOCATION cannot be "dark"; if a dark variant is ever
wanted, it would need its own case rather than branching on `sleepScreen`
inside the function, since `SETTINGS.sleepScreen == LOCATION` is guaranteed
true here and can't distinguish "dark" from "light" the way `DARK`/`LIGHT`
share one function today.

## Hardware finding: `lastSleepFromReader` never fires on TrailInk

`APP_STATE.lastSleepFromReader` is a single bool, not a per-app enum. It comes
from `ActivityManager::isReaderActivity()` (`src/activities/ActivityManager.cpp:269-273`),
`true` only if the current activity (or any activity in the stack) overrides
`isReaderActivity()` to return `true`:

- `EpubReaderActivity.h:204`
- `TxtReaderActivity.h:54`
- `XtcReaderActivity.h:51`
- `ReaderActivity.h:39`
- `IntervalSelectionActivity.h:33` — configurable, returns a stored
  `readerActivity` flag rather than a hardcoded `true`.

Default is `false` (`Activity.h:46`). `MapActivity` does not override it
(verified: no match for `isReaderActivity` in
`src/activities/map/MapActivity.h`), so being in the map is indistinguishable
from being in Home or Settings for this flag.

**Measured on hardware, 2026-08-05**: `LOCATION_CUSTOM` first shipped gated on
`lastSleepFromReader` (matching upstream `COVER_CUSTOM`'s gate exactly). On
the X4, with a book essentially never open, that flag is false on every
sleep — so `LOCATION_CUSTOM` always fell to `renderCustomSleepScreen()`, and
with no `/.sleep`/`/sleep` folder on the SD card, that fell through again to
`renderDefaultSleepScreen()` — which inverts because
`SETTINGS.sleepScreen != LIGHT` (it's `LOCATION_CUSTOM`). Net effect: a plain
inverted dark screen, no location line, ever. Fixed by keying the gate on
`SETTINGS.mapHasLastFix` instead (`SleepActivity.cpp:45-52`) — show the fix if
one exists, regardless of which activity was last open. Not verified whether
upstream's `COVER_CUSTOM` has the same practical issue for reader-only
CrossPoint devices (out of scope here — that codepath is gone in this fork).

`lastSleepFromReader` still gates, unchanged by this fix:
- The "Entering Sleep" popup's orientation (`SleepActivity.cpp:29-36`) — shown
  in reader orientation if last-reader, else default orientation.
- Wake-side auto-resume back into the reader (`main.cpp:460`).

These two are reader-specific by design (there's no "resume the map" wake
path to gate), so they keep the original flag.

## Quick Resume: a third axis, checked before the mode switch

`onEnter()` checks this first (`SleepActivity.cpp:23-30`), independent of
`sleepScreen`:

```cpp
SETTINGS.sleepScreen == QUICK_RESUME ||
(fromTimeout && SETTINGS.quickResumeSleepScreen == QUICK_RESUME_AFTER_TIMEOUT)
```

`QUICK_RESUME_SLEEP_SCREEN` enum: `QUICK_RESUME_NEVER = 0, QUICK_RESUME_AFTER_TIMEOUT = 1`
(`CrossPointSettings.h:172-175`).

If true, `renderLastScreenSleepScreen()` (`:281-291`) runs instead of anything
above: it leaves whatever is already on the e-ink panel — from *any* activity,
reader or not — and overlays a small moon icon bottom-left, using the panel's
differential refresh (`FAST_REFRESH` on X3, `HALF_REFRESH` on X4/other) so
the underlying content isn't repainted. This is the one path that is
genuinely screen-agnostic: a Map screen frozen with a moon icon on it is a
real, working sleep screen, not a designed one.

The frame this restores from is saved/loaded around deep sleep by
`saveSleepFrameBuffer()` / `loadSleepFrameBuffer()` (`main.cpp:203-222`).

## Custom-wallpaper anti-repeat

`CUSTOM` and `LOCATION_CUSTOM`'s fallback track recently-shown wallpapers to
avoid repeats: `CrossPointState.h:14,17-19` (16-slot ring buffer,
`recentSleepImages`/`recentSleepPos`/`recentSleepFill`), used via
`isRecentSleep()` / `pushRecentSleep()` (`SleepActivity.cpp:128,131`).

## Default: still DARK upstream; TrailInk flips to LIGHT on a separate branch

Upstream CrossPoint's default is `DARK` (`CrossPointSettings.h:179` reads
`uint8_t sleepScreen = DARK;`, unaffected by this branch). TrailInk flips the
default to `LIGHT` in commit `088f7b57` ("feat: default the sleep screen to
LIGHT", 2026-08-05) on a separate branch, `feat/boot-splash-blink`
(worktree `trailink-worktrees/boot-blink`) — not yet merged into `develop`,
and not touched by this change. Full writeup:
`/home/rfordinal/Development/xteink/docs/PROGRESS.md:166-189` (parent repo).

## Open — not fully designed yet

- `LOCATION` has no dark variant — see note above. Not needed yet since
  `DARK`/`LIGHT` cover that need for the logo screen.
- No staleness indication: a fix from days ago renders identically to one
  from seconds ago. Whether that matters depends on how often `mapHasLastFix`
  actually gets set outside an active ride — open, needs real usage to judge.
