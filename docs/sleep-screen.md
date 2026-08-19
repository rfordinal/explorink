# Sleep screen

Verified against branch `feat/sleep-location-screen` (based on `develop`
`7ee28192`) unless noted. Inherited wholesale from upstream CrossPoint;
ExplorInk has repurposed one mode (`COVER` → `LOCATION`, see below) and changed
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

`LOCATION` / `LOCATION_CUSTOM` are ExplorInk's rename of upstream's `COVER` /
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
| `LOCATION` | `renderLocationSleepScreen()` (`:250-269`) — last known map fix, LIGHT-style (never inverted) | none needed — see below |
| `LOCATION_CUSTOM` | Location info if `SETTINGS.mapHasLastFix`, else `renderCustomSleepScreen()` (`:45-54`) | `renderCustomSleepScreen()` |
| `QUICK_RESUME` | falls through `switch`'s `default:` to `renderDefaultSleepScreen()` — but see Quick Resume below, it's actually intercepted earlier | — |
| `DARK` / `LIGHT` | `renderDefaultSleepScreen()` (`:158-168`) — brand splash (see below), inverted to black unless `sleepScreen == LIGHT` (`:163-165`) | — |

`DARK` and `LIGHT` are not two render paths — they're the same
`renderDefaultSleepScreen()`, and `LIGHT` just skips the `invertScreen()` call.

## Repurposing COVER: `renderLocationSleepScreen()`

Upstream's `COVER` mode rendered the currently-open book's cover, parsed live
from the open XTC/TXT/EPUB (`APP_STATE.openEpubPath`) — real work: file load,
cover BMP generation, three format branches, a function-pointer fallback
chain for every failure point (no book open, load failed, cover-gen failed,
BMP header invalid). None of that applies to a device that doesn't open
books during a ride. ExplorInk's `LOCATION` mode replaces the whole thing with
a single bool check and a `snprintf` (`SleepActivity.cpp:253-266`):

```cpp
if (SETTINGS.mapHasLastFix) {
  // format "<lat>, <lon>  <compass>" into buf, hand it to drawBrandSplash()
} else {
  drawBrandSplash(renderer, tr(STR_SLEEPING));
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
after hardware testing showed the reader gate never fires on ExplorInk (see
"Hardware finding" below).
`renderer.invertScreen()` is never called in either DARK/LIGHT check nor
LOCATION today so LOCATION cannot be "dark"; if a dark variant is ever
wanted, it would need its own case rather than branching on `sleepScreen`
inside the function, since `SETTINGS.sleepScreen == LOCATION` is guaranteed
true here and can't distinguish "dark" from "light" the way `DARK`/`LIGHT`
share one function today.

## Brand splash: shared with the boot screen

`DARK`/`LIGHT` and `LOCATION` used to each draw their own logo + bold-text
title (`tr(STR_CROSSPOINT)`, whose value had drifted to the literal string
`"TrailInk"` -- fixed in the same change). `BootActivity` drew the identical
block. All three now call one function, `drawBrandSplash()`
(`src/activities/boot_sleep/BrandSplash.h:9`,
`src/activities/boot_sleep/BrandSplash.cpp:17-26`): the `Logo120` icon, the
`ExplorinkWordmark` image below it, and one caller-supplied subtitle line.
Boot passes `tr(STR_BOOTING)`; the two sleep screens pass `tr(STR_SLEEPING)`
or the coordinate line. The caller still owns `clearScreen()`/`displayBuffer()`
and anything extra on the page (Boot's version string at
`BootActivity.cpp:16`, Sleep's `invertScreen()` at `SleepActivity.cpp:163-165`).

### The wordmark image, and a drawImage constraint that only shows up on a non-square asset

`images/ExplorinkWordmark.h` was generated from
`docs/branding/explorink-wordmark-path.svg` (cropped to its ink bounding box,
then resized and rotated) -- the same by-hand process `scripts/convert_icon.py`
already automates for square icons, done manually here to control the crop.

Every icon in this tree before this one (`Logo120`, `MoonIcon`, `LoadingIcon`,
everything under `src/components/icons/`) happens to be square, which hides a
real constraint: `GfxRenderer::drawImage()`
(`lib/GfxRenderer/GfxRenderer.cpp:1208-1229`) rotates the *origin point* for
the current orientation but not the pixel data (`// TODO: Rotate bits`,
`GfxRenderer.cpp:1227`) -- and the low-level blit it calls,
`FreeInkDisplay::drawImage()`
(`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:213-228`),
indexes the source buffer as `imageWidthBytes = w/8` bytes per row for `h`
rows, straight off the `width`/`height` arguments. Those arguments must
therefore equal the *raw buffer's* row-major dimensions, not the image's
authored on-screen size -- and `width` must be a multiple of 8, since
`w/8` truncates.

For a square icon the two sizes are the same number, so this never mattered
before. `ExplorinkWordmark` is authored at 170x40 (the wordmark as a human
would size it) but pre-rotated 90 degrees before packing, the same convention
`convert_icon.py`'s own `rotate(90, expand=True)` step uses
(`scripts/convert_icon.py:32`) -- so the raw buffer is 40x170, and
`EXPLORINKWORDMARK_WIDTH`/`_HEIGHT` (`images/ExplorinkWordmark.h:8-9`) are
`40`/`170`, the numbers that actually go into `drawImage()`
(`BrandSplash.cpp:23-24`). Centring math (`(pageWidth -
kWordmarkOnScreenWidth) / 2`, `BrandSplash.cpp:23`) uses the *authored* 170
width instead, since that's the box as it appears on screen once the
coordinate rotation runs.
Getting these two numbers backwards does not fail to compile or assert --
`w/8` just truncates or reads past the intended rows, so the picture comes
out corrupted rather than erroring.

**Measured on hardware, 2026-08-08**: built, flashed, confirmed correctly
oriented and legible on the boot screen (wordmark 170px wide on a 480px-wide
Portrait panel, 20px+ clear of the logo above it) before this convention was
copied to the two sleep screens above.

## Hardware finding: `lastSleepFromReader` never fires on ExplorInk

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

If true, `renderLastScreenSleepScreen()` runs instead of anything above: it
leaves whatever is already in the framebuffer -- from *any* activity, reader or
not -- and overlays a small moon icon bottom-left. X3 gets
`displayGrayscaleBase(FAST_REFRESH)`; X4 and the rest get a windowed refresh of
just the moon's rectangle (`displayBufferWindow()`), which costs the same 500 ms
a whole-panel `FAST` would and leaves every pixel outside that rectangle
physically untouched. Which mode means what, and why never `FULL_REFRESH`:
[`refresh-modes.md`](refresh-modes.md).

The X4 path was a whole-panel `displayBuffer(HALF_REFRESH)` until 2026-08-19 --
1,684 ms repainting the entire glass to the same pixels plus a moon. It *looked*
differential because the frame it repainted was the frame already there, not
because any pixel was skipped.

### The marker is swapped for a small one on the way into sleep

The frame the panel holds through the whole sleep is a live map frame, marker and
all. Every live marker states a heading: Ride and Cycle draw a triangle pointing
along it, Hike draws a watch hand off its ring (`MapActivity::drawPositionMarker`).
On a sleeping device that is a claim about the past dressed as the present -- the
position is frozen and the heading is whatever held before the sleep.

So `MapActivity::onExit()` calls `drawSleepMarker()` when a quick-resume sleep is
the destination (gated on `APP_STATE.showBootScreen`, the same bool the
clean-frame request reads). It erases the live marker through the existing patch
-- the same `writeFramebufferRegion()` erase `moveMarker()` does, and for the same
reason: in single-buffer mode the map under a marker exists nowhere else -- and
draws Hike's shape **minus the hand**: white halo, black ring, centre dot.

Sizes are in `MapMarkerMetrics.h` (`kSleepMarker*`): ring 18 px, stroke 2, dot 6,
halo 3, against the live marker's 54 px ring. Deliberately not scaled by zoom rung
like the live marker is -- it is not tracking anything, so a size that moved with
the rung would only make it harder to recognise.

**The sizes are a judgement call, not a measurement.** What makes it findable is
the shape being recognisable, not its area: the white halo punches a hole in the
map ink, and below roughly this size the 2 px ring stroke and the dot start
reading as one blob. Judged on the glass, per CLAUDE.md -- a laptop PNG is the
wrong medium for it.

Cost is one windowed refresh (500 ms), over the *live* marker's box, which is
larger than and concentric with the sleep one so a single window covers the erase
and the new shape. With the sleep screen's moon that makes the way into sleep two
windowed refreshes -- still less panel time than the single whole-panel `HALF` it
was before 2026-08-19.

No-op, leaving the live marker alone, when there is no viewport on the panel (the
waiting banner) or no valid patch to erase with. Drawing over the live marker
without erasing it would stack two markers.

**Not verified on hardware yet** (2026-08-19). Needs a look at the glass: the
small marker is findable on a dense frame, and no fragment of the live marker is
left around it.

### It did not work from the map: `MapActivity::onExit()` wiped the frame first (fixed 2026-08-19)

"Leaves whatever is already in the framebuffer" is only as good as what the
outgoing activity leaves there. `MapActivity::onExit()` ended with
`renderer.clearScreen()` plus a whole-panel `HALF_REFRESH`, and `onExit()` runs
in `exitActivity()` (`src/activities/ActivityManager.cpp:140`) **before**
`SleepActivity::onEnter()`. So sleeping from the map went:

1. `enterDeepSleep()` calls `activityManager.goToSleep()` (`src/main.cpp:254`)
2. `MapActivity::onExit()` blanks the framebuffer and pushes white to the panel
3. `renderLastScreenSleepScreen()` draws the moon onto an all-white buffer

Reported off the device as "a clean white screen instead of the map". Both halves
of quick resume were affected, not just the sleep screen: `saveSleepFrameBuffer()`
runs *after* `goToSleep()` (`src/main.cpp:260`), so the white frame was what got
written to `/.crosspoint/sleep_frame.bin` and restored on wake too.

`MapActivity` and `BmpViewerActivity` (`:142`) are the only two activities in the
tree that wipe the panel in `onExit()` -- which is exactly why quick resume worked
from Home and the reader and not from the map. The fix replaces the map's wipe
with `renderer.requestCleanNextFrame()`, a one-shot request the *arriving* screen
spends ([`refresh-modes.md`](refresh-modes.md), "Handing the clean forward"), and
skips even that when a quick-resume sleep is what we are exiting into:
`enterDeepSleep()` writes that decision to `APP_STATE.showBootScreen` before
`goToSleep()` runs, so `onExit()` reads the same bool one activity later.
`BmpViewerActivity` still wipes; nothing has asked it not to.

**Measured on the X4 2026-08-19**, build `0.1.0-dev-map-exit-clean-8a5851cc`,
off the device's refresh log (`tools/quick_resume_gate.py` in the parent repo):

```
[558894] Exiting activity: Map    [558990] Entering activity: Sleep
[559491]   Wait complete: refresh (500 ms)      <- windowed moon
[ 14501] Exiting activity: Home   [ 14501] Entering activity: Sleep
[ 15002]   Wait complete: refresh (500 ms)      <- same, from Home
```

One windowed `FAST` on each, where the pre-fix build logged a `HALF` (the map's
wipe) plus a `HALF` (the sleep screen). The maintainer confirmed the panel shows
**the map** with the moon, not white, which is the reported bug closed.

### The restored frame is on the glass for about 150 ms, and nobody sees it

The wake half works and is invisible. Two wakes in the same run, identical:

```
[2834]   Wait complete: refresh (1683 ms)     restored frame + loading icon
[2878] [DBG] [ACT] Entering activity: Home
[3485]   Wait complete: refresh (500 ms)      Home, so its waveform began ~2985
```

So the map is alone on the panel for roughly 150 ms before Home starts painting
over it. Asked to watch for it, the maintainer saw Home, not the map -- which is
what that number predicts on e-ink, and is **not** evidence the restore failed.
The log settles that instead: `loadSleepFrameBuffer()` returning false routes to
`goToBoot()` (`src/main.cpp`), which would log `Entering activity: Boot`. The run
has zero of those, so the frame loaded and painted both times.

The consequence is worth stating plainly: **without a wake path that returns to
the map, the whole sleep-frame save/load round trip restores an image nobody can
perceive.** That is upstream quick resume's design, not a regression from this
change, but it is the reason the routing half is worth building -- see below.

### Wake back into the map

A wake from the map used to route to `goHome()` like any other, because
`APP_STATE.lastSleepFromReader` was the only "what was the app doing" signal the
boot routing had and `MapActivity` does not set it. Three pieces of state close
that, all in `CrossPointState` (`/.crosspoint/state.json`, which survives power
loss -- RTC memory does not):

| field | written by | read by |
|---|---|---|
| `lastSleepActivity` | `enterDeepSleep()`, from `activityManager.isMapActivity()` | the boot routing |
| `lastSleepRoutePath` | `MapActivity::onEnter()` | the boot routing, only when `lastSleepActivity` says Map |
| `mapActivityLoadCount` | the boot routing, before entering | cleared by `MapActivity::loop()` |

`isMapActivity()` is a virtual on `Activity` (`Activity.h`, default false) overridden
in `MapActivity`, walked over the stack by `ActivityManager::isMapActivity()` --
the same shape as `isReaderActivity()`. It is not a comparison against the
activity's `name`: that string is a log label, and routing a boot on a log label
breaks silently the day the label moves.

`enterDeepSleep()` reads it *before* `goToSleep()` tears the activity down, for the
same reason `lastSleepFromReader` is read there: afterwards there is nothing left
to ask.

The route needs its own field because `MapActivity::routePath_` is a bare member
that dies with the activity, and the picker is a separate screen -- without it a
wake would land in the map with the route silently gone. It is only ever read when
`lastSleepActivity` says Map, so a stale value cannot resurrect a route on its own.

#### Two ways out, because a map that cannot start must not trap the device

`mapActivityLoadCount` is the same contract `readerActivityLoadCount` has
(`main.cpp`, `EpubReaderActivity.cpp:226`): counted up before entering, cleared
once the activity is demonstrably alive. Nonzero at boot declines the resume and
lands on Home. Without it, firmware that cannot get through
`MapActivity::onEnter()` fails again on every wake and there is no way back to a
usable screen -- a panic reboots into the crash report, but a **hang** does not.

It is cleared on `MapActivity`'s first `loop()` tick rather than at the end of
`onEnter()`: reaching a `loop()` tick proves `onEnter()` returned, which a hang
inside the first render would not. The write is value-checked, so it costs one SD
write per wake and nothing on an ordinary map entry.

The second way out is a held **Back** at boot, which the reader resume already
had.

#### Two intermediate frames are skipped on this path, for the same reason

E-ink holds the sleep screen -- the map with its moon -- for the whole boot,
because nothing repaints the panel until the live map is ready. That is better
feedback than any "waking up" graphic, and it is free. So both frames that would
otherwise cover it are skipped:

**The quick-resume restore paint** (`main.cpp`). Normally the wake loads the saved
frame, adds a loading icon and spends a whole-panel `HALF` (1,684 ms) showing it.
On the way into the map that frame lives a few seconds before `MapActivity`'s own
entry `HALF` (`pendingEntryCleanRefresh_`) rewrites every one of its pixels.
`loadSleepFrameBuffer()` still runs, for its *other* job -- deleting
`sleep_frame.bin`, which a later unrelated quick resume would otherwise restore.

**The "reading tiles" splash** (`MapActivity::renderLoadingTiles()`, skipped via the
`resumedFromSleep` constructor flag that `goToMap()` passes). Its own comment
justifies it as feedback for "the only viewport reset with no feedback of any kind
in front of it" -- a premise that is false here. And it is not cheap on this path:
it asks `FAST_REFRESH` (`MapActivity.cpp`), but the driver promotes the first paint
after a wake (`_needsInitialFull`, `Ssd1677Driver.cpp:364-373`), so the request
becomes a whole-panel `HALF`.

**Measured on the X4 2026-08-19**, one wake with no route:

```
[934]  Starting TrailInk version 0.1.0-dev-map-wake-resume-5a40cc85
[1187] wake into map, route ""
[2984]   Wait complete: refresh (1683 ms)     <- renderLoadingTiles(), promoted
[5194] framebuffer ready in 2166 ms
[6994]   Wait complete: refresh (1683 ms)     <- the live map
```

1,683 ms for a picture with a ~2.2 s lifetime. The same frame on an ordinary map
entry cost 500 ms in the same run (`RouteSelect -> Map`: 500 ms then 1,683 ms),
which is what makes the promotion the cause rather than the frame itself.

The route path never paid this: with a route, `onEnter()` calls
`renderRouteOverview()` instead and there is no splash. Measured in the same run,
one `HALF` (1,684 ms) and no second one.

The flag is passed in rather than inferred from `APP_STATE.mapActivityLoadCount`,
which is nonzero for a slightly different reason and stops being nonzero at a
different moment.

#### What it costs, and what "resume" does not include

**Measured on the X4 2026-08-19**, off the run that verified the map-exit fix, so
these are the parts rather than the assembled path:

| stage | time | where it goes |
|---|---|---|
| reset to first paint ready | ~1,150 ms | derived: the restore's `HALF` completes at t=2834 and takes 1,683 ms |
| `MapActivity::onEnter()` | 5,019 ms | logged as `New max loop duration` |
| ... of which framebuffer | 2,530 ms | `framebuffer ready in 2530 ms`, 1,311 ms of it reading the card |
| ... of which panel | 1,683 ms | the entry `HALF` |

The assembled path was then **measured at 6,060 ms** (banner at t=934, live map on
the glass at t=6994, no route). The boot is the small half of it: the map's own
entry work dominates, which is where to look if this needs to be faster.

Skipping the promoted splash then **measured 4,036 ms** (build
`0.1.0-dev-map-wake-resume-9c3ba1ee`), against 5,807 ms for the same milestones on
the build before it -- `wake into map` in the log to the live map's refresh
completing, which is the pair both runs captured:

```
[1189] wake into map, route ""
[3447] framebuffer ready in 2193 ms
[5225]   Wait complete: refresh (1683 ms)     <- and nothing before it
```

**1,771 ms saved, one refresh instead of two.** Slightly more than the 1,683 ms of
panel time predicted, because the splash also did its own framebuffer work
(`clearScreen()`, the logo, three text draws) that no longer runs either.

The splash still costs 500 ms on an ordinary map entry, unpromoted, in the same
run (`[85383] Wait complete: refresh (499 ms)`) -- so it is skipped on the wake
path only, which is what was intended.

Not restored, and worth being explicit about since "exactly as before" is the
obvious expectation:

- **`screenMode_` resets to `Follow`** (`MapActivity.cpp`, `onEnter()`). Falling
  asleep with the menu or observation mode open wakes into Follow. A deliberate
  choice, not an oversight.
- **The BLE link.** The server restarts in ~60 ms (measured, `BlePositionServer.begin()
  returned` at t=537473 against entry at 537413) but the phone has to reconnect, and
  nothing on the device can make it.
- Zoom, ride mode, marker step and the last fix were already persisted in
  `CrossPointSettings` before this change (`mapZoomStep[]`, `mapMode`,
  `mapMarkerStep[]`, `mapHasLastFix`), so those do come back as they were.

**Verified on the X4 2026-08-19**, build `0.1.0-dev-map-wake-resume-5a40cc85`, via
`tools/quick_resume_gate.py` in the parent repo:

- sleep from the map wakes into the map: `wake into map, route ""` then
  `Entering activity: Map`, and the guard round-trips (`wake-into-map guard
  cleared` at t=7128)
- the route comes back: `wake into map, route "/trailink/trips/baba-serp.tir"` then
  `route "Baba serpentina 503" loaded: 122 points`
- a held Back lands on Home, and sleeping from Home is unaffected

One gap worth naming: the **`wake into map declined` line was not captured** for
the held-Back wake. It is logged in the first ~1.2 s, exactly the window USB CDC
re-enumeration eats -- only 2 of 5 boots in that run captured their banner at all.
So the log shows the decline happened and cannot show *why*; a stuck
`mapActivityLoadCount` would look identical. What rules that out is the `guard
cleared` line from the preceding wake, plus the operator knowing Back was held.
Proving the decline's cause from the log alone needs the decision persisted
somewhere the CDC gap cannot swallow.

The splash skip was verified separately on build
`0.1.0-dev-map-wake-resume-9c3ba1ee`: no logo frame on the wake, one refresh
instead of two, and the 4,036 ms above.

The frame this restores from is saved/loaded around deep sleep by
`saveSleepFrameBuffer()` / `loadSleepFrameBuffer()` (`main.cpp:203-222`).

## Custom-wallpaper anti-repeat

`CUSTOM` and `LOCATION_CUSTOM`'s fallback track recently-shown wallpapers to
avoid repeats: `CrossPointState.h:14,17-19` (16-slot ring buffer,
`recentSleepImages`/`recentSleepPos`/`recentSleepFill`), used via
`isRecentSleep()` / `pushRecentSleep()` (`SleepActivity.cpp:128,131`).

## Default: still DARK upstream; ExplorInk flips to LIGHT on a separate branch

Upstream CrossPoint's default is `DARK` (`CrossPointSettings.h:179` reads
`uint8_t sleepScreen = DARK;`, unaffected by this branch). ExplorInk flips the
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
