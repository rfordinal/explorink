# Touch modes

What a touch panel is allowed to do, and when the on-screen button boxes are
drawn. Added 2026-09-05. **Not yet run on hardware** — see "What a hardware pass
has to check" at the bottom.

## The problem it fixes

`gpio.hasTouch()` answered two different questions at once. It says whether the
board has a digitizer (`BoardConfig.h`, `hasTouch()` reads
`ACTIVE.touch.controller`), and every theme also used it as a *policy*: a board
with a panel drew no button hints at all and every pixel of the screen was live.

So on a T5 S3 Pro the four bottom boxes and the two side boxes vanished, and the
only input left was touch anywhere plus whatever hardware keys the board has —
which on that board is almost none (BOOT on GPIO0, plus a user button behind the
PCA9535 expander). There was no way to ask for the boxes back.

## The three modes

One Controls setting, `touchMode` (`src/CrossPointSettings.h`, enum `TOUCH_MODE`).
It is only offered on a board that has a digitizer; `SettingsList.h` drops the
row otherwise.

| Mode | Boxes drawn | What touch does |
|---|---|---|
| `TOUCH_ANYWHERE` (0, default) | no | everything: list rows, swipes, edge gestures, map drag |
| `TOUCH_BUTTONS_ONLY` (1) | yes | only the six boxes, each acting as its hardware button |
| `TOUCH_DISABLED` (2) | no | nothing, and touch stops counting as user activity |

**`TOUCH_DISABLED` is an effective mode, never a stored one, and Settings does
not offer it.** The Controls row has two values; the lock is its own persisted
flag, `CrossPointSettings::touchLocked`, and `TouchPolicy::mode()` reports
DISABLED while it is set. Two reasons, and the first is the one that matters:

- **A rider who picked OFF in Settings could not reach Settings again to undo
  it.** On an X4 Pro, where Back and Confirm both come from touch, that is a
  device with no working input at all. The lock is only reachable from a gesture
  that can also undo it.
- A stored value outside the row's own list was an **out-of-bounds read** in the
  settings screen: `SettingsActivity.cpp` indexed `enumValues[value]` unchecked
  on the `valuePtr` path, while the `valueGetter` path two branches below already
  bounds-checked. Now both do.

The flag is persisted rather than kept in RAM: a device locked when it went to
sleep wakes up locked, because the rider put it in a bag and coming back unlocked
would be the surprise.

**It is written by hand in `CrossPointSettings::toJson()` / `fromJson()`, and it
has to be.** Serialisation is driven by `SettingsList`, and this flag
deliberately has no entry there -- a Settings row that can lock the rider out of
Settings must not exist. So a field added to the struct and nowhere else is never
written and never read: measured 2026-09-07, the panel came back unlocked from
every sleep and reboot, which is exactly the case the flag exists for. The
front-button remap and the map ladder state are in the same position and are
loaded the same way. The preference underneath survives untouched, so
unlocking needs nothing remembered. `TouchPolicy::mode()` also treats a *stored*
DISABLED as ANYWHERE, so a settings file written by an older build cannot lock a
device whose owner has no way to unlock it.

Default is `TOUCH_ANYWHERE`, so a device that was already in use behaves exactly
as it did before the setting existed.

**Only BUTTONS draws the boxes, and that is the indication of which mode is on.**
Drawing them in OFF too would put six buttons on the glass that do nothing, and
on a board with no keys under them the labels would name keys that are not there.
A board with no digitizer draws them always: there the box labels the physical
key beneath it, which is what it always did.

## Where the decision lives

`src/TouchPolicy.h` — four questions, one place:

- `touchAnywhere()` — the whole screen is live.
- `touchHintBoxes()` — only the boxes are.
- `touchActive()` — any touch reaches the UI at all (false in OFF).
- `hintsVisible()` — the boxes are drawn and the layout reserves room for them.

Nothing else reads `gpio.hasTouch()` for policy any more. The themes and
`UITheme::getMetrics()` ask `hintsVisible()`; `MappedInputManager` asks the other
three.

## How a tap becomes a button press

Four HAL touch primitives are the only way touch enters the firmware
(`src/MappedInputManager.cpp`: `wasScreenTapped`, `wasScreenTouchDown`,
`isScreenTouchHeld`, `decodeSwipe`). Each returns false unless
`TouchPolicy::touchAnywhere()`. Every other touch helper — list hit tests,
`rowTouch`, `colTouch`, the home/menu/back gestures — is built on those four, so
one gate switches all 96 call sites off.

In BUTTONS mode, `MappedInputManager::pumpHintTouch()` runs once per
`update()` and turns contact into button edges:

- touch down inside a box: that box's hardware button reports `wasPressed` for
  one frame, and `isPressed` until the finger leaves.
- tap released on the **same** box: `wasReleased` for one frame, the way a
  physical key does not fire when the finger slides off it. It also calls
  `rememberTouchHeldTime()`, so `getHeldTime()` returns the contact duration and
  long-press behaviours work off a box.
- lifted without producing a tap: the held state is cleared with no release
  event. Without this branch the button would stay held for good.
- **dragged off the box while still on the glass: the same silent cancel.**
  `InputManager::isTouchHeldAt()` has no slop gate, so a finger that left the box
  still reported as held and the button stayed pressed wherever the finger went
  -- `ButtonNavigator`'s continuous step kept scrolling from a box the finger had
  left. A cancel emits nothing, the way sliding off a physical key does not press
  it.

One tail of that cancel is **not** fixed and is worth knowing: `ButtonNavigator`
clears `lastContinuousNavTime` only on a release edge
(`ButtonNavigator.cpp`, `onRelease`), so after a cancel that followed a
continuous run, the next genuine release on that button is swallowed once. It
only bites an activity that acts on release rather than on press. Fixing it means
changing `ButtonNavigator`, which every device shares, so it waits for the input
redesign rather than being patched here.

**A tap on a box must not read as a hold.** `ButtonNavigator::onNext()` is
`onPress` plus `onContinuous`, and the continuous step fires when the button
`isPressed` and `getHeldTime()` is over 500 ms. The synthetic press holds
`isPressed` for as long as the finger is on the box, and `HalGPIO::getHeldTime()`
knows nothing about that: with no hardware button down,
`InputManager::getHeldTime()` returns `buttonPressFinish - buttonPressStart`, the
length of the **last hardware press**. So after a 600 ms frontlight hold on the
user button, every later box tap looked like a half-second hold and moved the
Home selection twice -- once from the press step, once from the continuous step.
It stopped once some shorter hardware press replaced the stale value, which is
why it read as intermittent.

`MappedInputManager::getHeldTime()` now reports the live duration of the touch
while a box is held (`hintDownAtMs`). A tap reads as a tap, and holding a box for
half a second gives the same auto-repeat a hardware key does.

Every hardware read in `mapButton()` goes through `rawButton()`, which ORs the
synthetic state in before asking `HalGPIO`. That is why nothing downstream
changed: the front-button remap (`frontButtonBack` and friends), the
orientation-following axis swap, the reader's side-button layout and every
activity's `wasPressed(Button::Confirm)` all work on a box tap unmodified.

**Box index is the hardware button index.** Front box 0..3 are
`BTN_BACK`, `BTN_CONFIRM`, `BTN_LEFT`, `BTN_RIGHT` — the same order
`mapFrontLabels()` hands the labels to `drawButtonHints()`, so the box under a
label really is the key that label names. A `static_assert` in `hintBoxAt()`
holds that. Side boxes are `BTN_UP` (top) and `BTN_DOWN` (bottom), which the
reader's page pair maps onto.

### A box with no label is not a button

Screens pass an empty label for a key they do not use, and the themes draw
nothing (or, in Lyra, a stub) there. The input layer never sees labels, so each
theme records the last painted set (`BaseTheme::rememberFrontLabels()` /
`rememberSideLabels()`) and `frontHintBox()` / `sideHintBox()` return false for
an empty one. Without it a tap on blank glass where a box used to be would still
fire its button.

### The hit test runs in portrait

The boxes are always painted in portrait — every `drawButtonHints()` forces
`Orientation::Portrait` and restores the caller's afterwards. `tapToLogical()`
maps a touch to whatever orientation is *currently* being drawn, which the reader
rotates. So `MappedInputManager::tapToPortrait()` applies the portrait transform
directly (the same arithmetic as `GfxRenderer`'s `Portrait` branch) and the hit
test compares against portrait rects.

## Geometry on a panel that is not an X4

The position arrays in each theme are X4 and X3 numbers, hand-tuned so a box sits
above the key it names. Those are never derived and are unchanged.

Any other panel has no keys under the boxes, so there is nothing to line up with
and the only requirement is that the six boxes land on screen in the same
arrangement. `src/components/themes/HintGeometry.h` scales the X4's 480x800
portrait layout: X by `screenWidth / 480`, Y by `screenHeight / 800`.

`UITheme::getMetrics()` scales `buttonHintsHeight` (by the Y factor) and
`sideButtonHintsWidth` (by the X factor) the same way when the boxes are visible.
This matters: the T5 S3 Pro is 540x960 in portrait at about 234 PPI, where an
unscaled 40 px band is a 4.3 mm tap target. Scaled it is 48 px, about 5.2 mm —
still small, and a candidate for a deliberate touch-sized band later rather than
a scaled button-era one.

Boards and what they get:

| Board | Portrait screen | Layout |
|---|---|---|
| X4, X4 Pro | 480x800 | the X4 arrays, unscaled |
| X3 | 528x792 | the X3 arrays, unscaled |
| T5 S3 Pro | 540x960 | X4 arrays scaled x1.125 / x1.2 |

`BoardConfig::ACTIVE.displayHeight` is the portrait width (the profile stores the
panel's native landscape size), which is how `HintGeometry` answers without a
renderer — `ThemeMetrics` is a singleton with nothing to ask.

## The T5 S3 Pro: the capacitive home key locks and unlocks the panel

**The button map for this board is in `src/main.cpp`, above `userButtonHook()`,
and the hardware page is the parent repo's `docs/devices/lilygo-t5-s3-pro.md`,
"The four physical buttons".** Read one of them before touching any of this:
three sessions in a row mis-identified which switch is which, because the
silkscreen, the schematic and the firmware each call the same switch something
different.

The short version, because it is the thing that keeps getting confused:

- **The user button** (schematic S3, net `BUTTON`, PCA9535 pin IO1_0, silkscreen
  "IO48", physically bottom-left) is one switch with several names. Tap =
  Confirm, hold 600 ms = frontlight. Unchanged by any of this.
- **The capacitive home key** is a *separate, fifth* input. It is not a GPIO at
  all: the GT911 reports it in its own status byte, bit 0x10.

The home key carries three gestures:

- **tap** — Confirm (Select), fired once the double-tap window has passed.
- **double tap** — lock or unlock touch: `TOUCH_DISABLED` from whatever mode was
  on, and back to that same mode on the next double tap.
- **hold** — toggle the frontlight. Already wired before this work, and it stays
  on a physical hold because gloves defeat the digitizer and the light is what a
  rider reaches for with gloves on.

**The single tap has to wait, and that is the price of the double tap.**
`MappedInputManager::pumpHomeKey()` holds the first tap for
`HOME_KEY_DOUBLE_TAP_WINDOW_MS` (500 ms) and then decides: no second tap means
Confirm, a second tap means the lock and the held Confirm is dropped. Firing
Confirm on arrival cannot work — it would activate whatever the cursor was on
before the second tap could mean the lock instead, and a select cannot be taken
back. Half a second is still small against a panel refresh measured in whole
seconds.

### Four defects the audit found, fixed 2026-09-06

A read-only audit of the whole input path (see "What a hardware pass has to
check") turned up four things in this layer that were wrong independently of the
home key:

- **A finger dragged off a hint box left the button held** -- above.
- **The home key was not activity.** `main.cpp`'s sleep timer reads the button
  bitmask and `wasTouchActivity()`; the capacitive key is neither, so a rider
  driving the device from that key alone was slept on schedule and spent the
  whole time on the throttled 50 ms loop -- which also stretched the key's own
  gesture timing. It now counts.
- **`wasAnyPressed()` / `wasAnyReleased()` did not see hint-box taps**, so a
  screen driven only by the boxes looked idle to anything asking "did the rider
  do something". `main.cpp`'s sleep timer deliberately still does not come
  through here: it reads `HalGPIO` plus `wasTouchActivity()`, which already
  counts the touch that made the press.
- **`getPressedFrontButton()` did not either**, which left the button remap
  screen unusable by touch on a board with no front keys.
- **The touch held-time override answered for the wrong input.**
  `getHeldTime()` returns the last touch's duration for 250 ms when no button
  edge landed this frame. A hardware button going down inside that window starts
  an input the override knows nothing about, and while it is held there are no
  further edges to stop it answering -- `ButtonNavigator` read a slow screen tap
  as an instant half-second hold on the key. A press now ends the override's
  claim.

Two more the audit found are **not** fixed here, because they are in the SDK and
belong to the input redesign rather than to another patch: `getHeldTime()`
reports the duration of the **first** key of a chord for every key in it
(`InputManager.cpp`, `buttonPressStart` is set only when nothing was down), and
the T5 S3 Pro's user button can never long-press Confirm, because its hook emits
a ~30 ms synthetic pulse *after* release, so `isPressed(Confirm) && getHeldTime()`
is unreachable there.

### The key produces more events than the rider makes gestures

**Measured on hardware 2026-09-05.** One double tap on the Home screen locked the
panel, opened the map, *and* lit the frontlight once the map finished rendering.
All three gestures, from one gesture. The cause is in how the GT911's key is
read, and it cannot be fixed from this repo **as the input layer stands** --
`freeink-sdk` is our own fork, so the real fix belongs there, and
[`input-gestures.md`](input-gestures.md) is the design for it:

`InputManager::pollGt911()` takes the key's **press/release edges only from a
fresh touch frame** (the `status & 0x80` gate) but runs the **hold timer from a
latched down-state above that gate**. That split is deliberate -- a motionless
hold stops producing frames, so a gated timer would never cross the threshold --
but it means a *missed release edge* leaves the key latched down, and the hold
then fires from a press that was already spent as a tap. A map render blocks the
main loop for seconds, so the stale hold surfaced the moment polling resumed.

The same coarse sampling explains the rest: a deliberate double tap regularly
landed outside a 300 ms window.

**Careful with the next sentence, because it is the one that keeps being
repeated.** What was *measured* is the outcome above: one gesture, three
behaviours. That extra **tap events** caused it -- a third tap starting a fresh
single-tap window that then selected -- is the **inferred** explanation. Nobody
logged the events. Whether the extra ones are contact bounce or stale GT911
frames is open, and it decides the real fix: bounce wants a minimum press width,
stale frames want the frame discarded.
[`input-gestures.md`](input-gestures.md) carries that question and the
measurement that settles it.

Three filters in `pumpHomeKey()`, all of them our side of a noisy source:

- **The window is 500 ms**, not 300. The second tap is seen later than the finger
  made it.
- **A refractory window of 500 ms after any resolved gesture.** A tap arriving
  inside it is the tail of a gesture already answered, not a new one.
- **A hold is believed only while no tap has been made of the current press**
  (`homeTapConsumedSinceDown`, cleared on each press edge). That is what rejects
  the stale hold.

Residual risk, stated rather than hidden: if the SDK ever missed a *press* edge
while this layer had consumed a tap, a genuine hold would be rejected. The press
edge is what starts the SDK's own hold timer, so a hold cannot fire without one
having been seen there -- but this layer only sees it if something queries input
that frame.

Three details in that machine:

- **A hold cancels a pending tap.** The SDK suppresses the hold's own release tap
  (`InputManager::serviceTouch`), so without this a tap-then-hold would light the
  frontlight and then still select when the window ran out.
- **The window is timed off the per-frame input pump**, the same one the hint
  boxes use, so it resolves on whatever query the activity makes rather than
  needing a tick of its own. An activity that asked for no input at all would
  hold the Confirm longer, and would also have nothing to do with it.
- **Only a board whose key carries the double tap pays the latency.**
  `TouchPolicy::homeKeyDoubleTapLocksTouch()` is false everywhere else, and there
  the tap is Confirm the instant it lands.

**`TouchConfig::hasHomeKey` gates nothing, and this cost a session.** It reads
like the flag that turns the key on -- it is `false` for this board -- but
`InputManager::serviceTouch()` reads the status bit unconditionally and the flag
appears nowhere in `InputManager` at all. So the key has always worked here.
**Measured on hardware 2026-09-05** by the maintainer: holding it turns the
frontlight on. A session that read the flag and concluded the key was dead was
wrong about the board and wrong about which switch the rider was pressing.

Four details worth knowing:

- **A locked screen does not select.** The single tap is still held for the
  window, because a second tap inside it is the unlock, but once it resolves as a
  single tap on a locked panel it means nothing and no Confirm is emitted.
  Locking is the rider saying "ignore what I touch", and the key stays listened
  to for exactly one thing.
- **A hold never also toggles the lock or selects.** The SDK reports the tap only
  on release and only when the hold threshold was not crossed
  (`InputManager::serviceTouch`), and `pumpHomeKey()` drops any pending tap when
  the hold fires.
- **Nothing has to be remembered across the lock.** It is one persisted flag,
  `CrossPointSettings::touchLocked`, and it never touches the mode the rider
  chose -- unlocking simply stops overriding it. The first version stored
  DISABLED *into* `touchMode` and kept the previous value in RAM, which lost it
  across a reboot and put a value in that field the Settings row does not list.
- **One SD write per deliberate tap.** The same reasoning the frontlight hold
  carries: a handful of writes a ride, not one per interaction.

A toggle repaints the screen (`activityManager.requestUpdate()`), because the
chrome at the bottom changes with the mode. That is a full refresh per tap on
e-ink.

## The padlock: how a locked panel is told apart from a live one

Boxes on screen mean the boxes are live, but their absence is ambiguous —
ANYWHERE draws none either. So OFF draws a padlock where the band would be:

- `TouchPolicy::lockIndicator()` is the one test.
- **It is drawn as a button, not as a small glyph in an empty band.** One box the
  size of a hint box, centred, with the padlock inside it. A 20 px padlock
  floating in a thin strip read as a status mark rather than as the thing that
  took the buttons' place, so it is now a 28 px glyph in a real box.
- `UITheme::getMetrics()` reserves **the same band the boxes get**, not a thinner
  strip. Nothing above it moves when the panel locks, and the map's chrome swap
  refreshes one rectangle that fits both modes.
- `BaseTheme::drawTouchLockIndicator()` paints it, called from every theme's
  `drawButtonHints()` on the path where that draws no boxes. That is why it
  reaches home, settings, the reader and the map without any of them changing.
- **The box is `drawTouchLockBox()`, and it is virtual**, because every theme
  draws its hint boxes differently: Lyra rounds the top corners, RoundedRaff uses
  its own 2 px outline and bottom radius, the classic theme is square. The
  padlock stands in for those boxes, so a square box next to rounded ones read as
  a different kind of thing. The caller keeps the policy, the orientation and the
  geometry; an override changes the look and nothing else.
- The glyph is Lucide `lock` at 28 px through
  `scripts/gen_touch_lock_icon.py` (the icon rule in the parent repo's
  `CLAUDE.md`), drawn with `drawMono1bpp()`.

`buttonHintsRect()` returns that strip while locked, so a caller repainting part
of the panel still refreshes the padlock.

### The map has to notice the change itself

`toggleTouchLock()` asks for a repaint with `activityManager.requestUpdate()`,
and that reaches every screen except one. `MapActivity` does not implement
`Activity::render(RenderLock&&)` at all -- it paints from its own `loop()` on the
main task (`MapActivity.h`, the note above `renderCurrent()`), so the render
task's request never lands there.

**Measured on hardware 2026-09-05:** tapping the home key on the map locked the
panel for real -- touch went dead -- while the hint boxes stayed on screen and no
padlock appeared. Functional change, no visual one.

So `MapActivity::loop()` compares `drawnTouchMode_` against
`TouchPolicy::mode()` -- the **effective** mode, since the lock is its own flag
and overrides the stored preference; polling `SETTINGS.touchMode` would miss
every lock and unlock --
and swaps the chrome when they differ. Two details in that check:

- It sits **below** the option popup's early return. A menu open over the map
  owns the panel, and repainting the map under it would strand the popup's
  pixels; the check fires on the first frame after it closes instead.
- `0xFF` means nothing has been painted yet, so entering the map settles the
  value without spending a redraw on it.

### The map swaps the chrome, it does not re-render

Re-rendering the whole map to take two strips of chrome off it costs tiles off
the card and a full-panel refresh, for a change that touches nothing else. So
`MapActivity` snapshots the panel under the chrome on every full frame and swaps
it later, the way the option popup already saves the map under an open dialog:

- `captureRegion()` / `restoreRegion()` are the reusable form of
  `captureMenuBackdrop()` / `restoreMenuBackdrop()`. **Restore keeps the
  snapshot** -- its bits are still a clean picture of the map under that
  rectangle, so the next swap needs no new capture.
- `swapChrome()` restores the map, calls `drawMapButtonHints()` (which draws the
  boxes, the padlock, or nothing, per mode) and refreshes **one window over both
  rects**. A windowed refresh costs the same panel time as a full one whatever
  its area -- ~1,081 ms on this panel, measured over a 4h36m walk 2026-09-05
  ([`map-follow.md`](map-follow.md), "A windowed refresh blocks the loop") -- so
  area is free and the *count* is what costs. Two windows would be 2.2 s against
  1.08 s for one.
- **The union is affordability-tested, not assumed.**
  `displayBufferWindow()` allocates a buffer per window inside the driver, and an
  unbounded union of two far-apart boxes is the whole panel, which aborted the
  device on a map screen (measured 2026-08-17). When the union does not fit,
  `swapChrome()` gives up and lets the caller do the full render: two windows
  would cost the same panel time as that render, and the render is at least
  correct about the layout.
- **Unverified on hardware.** The union window and its affordability test were
  flashed 2026-09-06 but not specifically exercised: the last map lock the
  maintainer confirmed ran on the two-window build. The union wants about 37 kB
  as one block on a T5 S3 Pro, and whether `ESP.getMaxAllocHeap()` offers that on
  a map screen is `[read]`, not measured. If it does not, the fallback is the
  full render -- correct, and slow.
- **This trade-off flips when the T5 S3 Pro gets a real partial-window refresh**
  (planned, another session). Today area is free and the window count is
  everything, so one union wins. Once a window costs in proportion to its area,
  two small far-apart rects beat one union that spans mostly untouched panel --
  revisit `swapChrome()` then rather than assuming it still holds.
- **Two rectangles, never their union.** The bottom band and the side boxes are
  far apart; one rect covering both would be 540 x 546 on a T5 S3 Pro, about
  37 kB, against roughly 4 kB for the pair.
- **The band is snapshotted at `UITheme::chromeBandHeight()`**, the tallest
  chrome any mode draws, not at the current mode's height. The boxes are taller
  than the padlock strip, so a snapshot sized for the padlock would leave a
  sliver of stale box pixels above it -- and e-ink holds that indefinitely.
- The snapshot obeys the same heap reserve the menu backdrop does. If there is no
  room, or no full frame has been drawn yet, `swapChrome()` returns false and the
  caller falls back to the full render.

One consequence, on purpose: the map keeps the layout it was rendered with. The
reserved band differs between modes, so after a swap the map's own content still
sits where the old mode put it until the next full frame. Swapping chrome is not
a relayout.

## The X4 Pro trap

The X4 Pro has a digitizer plus **two** hardware keys (`Left` on GPIO0, `Right`
on GPIO7) and the capacitive Home key. Back and Confirm come from touch. So
`TOUCH_DISABLED` on an X4 Pro leaves no way to confirm or go back. The setting
does not currently block that — see the parent repo's `docs/TODO.md`.

## What a hardware pass has to check

Built and host-tested: `default` (esp32c3) and `t5s3pro` (esp32s3) both compile
clean with no new warnings, 424/424 host tests pass. None of that says anything
about a finger on glass, so the list below is what hardware has to answer.

**Rows 1 and 2 are answered, on a T5 S3 Pro, 2026-09-10.** The maintainer used
every map control by finger on the panel: the six boxes are there, each fires
the key it names, and panning, the look-ahead, the zoom ladder and the menu all
work without a serial cable. That closed T-573 in the parent repo. **Rows 3 to
6 are still open** -- nobody has tested the slide-off branch, OFF mode or the
X4 regression, and a green host suite says nothing about any of them.

1. **T5 S3 Pro, BUTTONS mode.** Are the six boxes on screen, in the X4
   arrangement, fully inside 540x960? Does a tap on each fire the right action?
   **Done 2026-09-10, on the board.**
2. **Do the boxes match the keys they name** as screens change (home, settings,
   reader, map) — and does a tap on a box with no label do nothing?
   **Done 2026-09-10 for the map path**; the reader and settings screens were
   not walked box by box.
3. **Slide off.** Press a box, drag off it, lift. Nothing should fire, and the
   next press must still work (the stuck-held branch).
4. **Long press on a box** — chapter skip / the long-press menu, whichever the
   settings select. `lastTouchHeldMs()` is written on release
   (`InputManager.cpp`), so a long press should report its real duration; this is
   read off the code, not measured.
5. **OFF mode.** Nothing on the panel reacts, and the device still sleeps on
   time with a palm resting on the glass.
6. **X4 regression.** The bottom band and the side boxes must be pixel-identical
   to before — the X4 arrays and metrics are untouched, so any difference is a
   bug in this change.
7. **The T5 S3 Pro's capacitive home key.** A tap should make the boxes vanish,
   leave a padlock at the bottom, and kill the glass; the next tap should bring
   the boxes back in the mode that was on before. The key itself is known to
   report (the hold was measured 2026-09-05), so a tap doing nothing means the
   tap event or the toggle is wrong, not the hardware.
8. **The three home-key gestures must not bleed into each other.** A single tap
   selects (after a beat) and never locks. A double tap locks and never selects.
   A hold lights the frontlight and does neither. And the user button
   (bottom-left, S3) must still be Confirm on a tap and the frontlight on a hold
   — that switch is not part of this change.
9. **Boot while locked, then tap:** it should come up in Buttons only.
10. **The Settings row offers two values, not three.** OFF must not be
    selectable; the lock is reachable only from the home key's double tap.
11. **Reboot while locked.** It comes up locked, and one double tap returns it to
    the mode stored in Settings, not to a default.
12. **The padlock matches the boxes it replaced.** The same corner style as this
    theme's hint boxes, centred, and nothing above the band moves when the panel
    locks.
