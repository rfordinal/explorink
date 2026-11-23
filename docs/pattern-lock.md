# The pattern gate

A 4x4 grid, a cursor walked by the four direction buttons, and a path that is
either the stored pattern or is not. One screen, pushed in front of whatever
needs guarding, reporting one bit back.

Two files:

- `src/components/lock/PatternLock.h` -- the arithmetic. Grid walk, path, encoding,
  validation. No I/O, no Arduino, no secret. Host-tested (`test/pattern_lock/`).
- `src/activities/lock/PatternLockActivity.{h,cpp}` -- the screen. Draws, reads
  buttons, calls the caller's verifier, finishes with `PatternLockResult`.

## Controls

| Button | Where it is | What it does |
|---|---|---|
| LEFT / RIGHT | front (bottom) buttons | cursor one cell |
| UP / DOWN | side buttons, fixed | cursor one cell |
| CONFIRM | front | try the path |
| BACK | front | one point back; leaves the screen when only the start is left |

LEFT and RIGHT are the rider's own remap (`SETTINGS.frontButtonLeft`); UP and DOWN
are fixed to the side buttons and are not remappable
(`src/MappedInputManager.cpp`, `Button::Up`). That is why the screen labels the
side buttons too: those two directions cannot be found by looking at the bottom
band.

A press into a wall changes nothing and does not redraw. A refresh per refused
press reads as a stutter and says nothing.

**On a touch board in `TOUCH_ANYWHERE` the screen draws and drives its own
controls.** The theme's hint boxes exist only in `TOUCH_BUTTONS_ONLY`
(`TouchPolicy::hintsVisible()`), and the stored default is `TOUCH_ANYWHERE`
(`CrossPointSettings.h`, `touchMode`), so on a factory-default X4 Pro -- two
hardware keys, everything else touch -- the first cut of this screen could move
the cursor left and right and do nothing else. No UP, no DOWN, no CONFIRM, no
BACK. In that mode the gate now takes a tap on a cell orthogonally next to the
cursor as that step, and draws its own BACK and OK boxes at the foot of the
screen.

A tap on a cell that is **not** next to the cursor does nothing on purpose. The
alternative -- walk the cursor to wherever the finger landed -- would invent
points the rider never entered, and the path is the secret.

## What a pattern is

A start cell plus 3 to 7 right-angle steps -- 4 to 8 points counting the start.
No diagonals: a press moves one cell, and the hardware has four directions. A cell
may be stepped on twice, because right-then-left comes back and forbidding that
would both surprise a rider and shrink an already small space.

The **start is fixed**, because it is the one point no button press produces. Let
it be wherever the cursor rested last and the same four presses mean two different
patterns on two days.

Encoding is one char per point, in order, start included: `'0'`-`'9'` then `'a'`-`'f'`
for cells 0 to 15. `"048cdef"` is down the left column then along the bottom. That
string is the only thing that leaves the component, and it is what a verifier
compares -- hashing or wrapping it is the caller's job.

## How large the space is

Enumerated, not estimated (`test/pattern_lock/PatternLockTest.cpp`, the three
`TheSpaceIs...` tests, which is where these figures come from and what fails if a
bound moves -- the edge row had no test behind it until a review noticed this
sentence claimed otherwise):

| Start | Distinct patterns, 4 to 8 points | Bits |
|---|---|---|
| corner | 2,810 | ~11.5 |
| edge | 4,545 | ~12.1 |
| middle four | 7,350 | ~12.8 |

So the default start is `kDefaultStart`, row 1 column 1 -- interior. The first
step branches four ways there instead of two, and that is 2.6x a corner start for
no cost at all.

**12.8 bits is about a four-digit decimal PIN.** The wallet's dot PIN
(`WalletUnlockActivity`) is 4^6 to 4^10, around 20 bits, because every press there
is a free choice and no grid wall ever refuses one. A grid is easier to remember
and smaller. That trade is what this screen is, and it is a trade -- so the same
sentence that holds for the dot PIN holds harder here: **the pattern is not the
strength.** What makes a wrap hard to attack is the device secret it is mixed
with, and what makes guessing impractical is the caller's rate limiter
(`../../../docs/wallet-plan.md`, and `WalletCrypto.h` on the wallet branch).

## What the hidden path buys

The screen draws the grid, the cursor, and a segmented meter counting how many
points are in. **It never draws the path.**

What that protects: the pattern is not on the panel after the screen leaves. An
e-ink panel holds its last frame with no power, so a device that is lost or stolen
would otherwise be handed over with the secret still visible, ghosting included.

What it does **not** protect: someone watching the entry. The cursor has to be
visible or the grid is pointless, and a watcher who sees every press has the
pattern. The dot PIN this replaces could claim more -- "a shoulder looking at the
panel must learn nothing" (`WalletUnlockActivity.h`) -- and a grid cannot. Anyone
weighing the two screens should weigh that, not just which is easier to enter.

## The verifier contract

The screen owns no secret. The caller hands it:

```cpp
PatternLockActivity::Config config;
config.title = tr(STR_WALLET_UNLOCK);
config.isLockedOut = [] { return wallet::pinIsLockedOut(wallet::KeyStore::failures()); };
pushActivity(std::make_unique<PatternLockActivity>(
    renderer, mappedInput, config,
    [](const char* path, bool speculative) { ... }));
```

Three things the caller keeps: the stored pattern, the failure counter, the rate
limiter. Three things the screen keeps: the cursor, the path in RAM, and the
discipline of wiping it on exit and after every attempt -- through
`patternlock::secureWipe()`, because a plain loop over a buffer that is dead on
the next statement is a store a release build may drop.

**`Config::startCell` is part of the stored secret, not a cosmetic default.**
The start is the one point no button press produces, so it has to match the
stored pattern's first character or nothing a rider enters can ever match it --
a permanent lockout. Read it off the pattern with
`patternlock::startCellOf(stored)` and pass the two together;
`patternlock::isWellFormed(path, startCell)` takes the start for the same reason
and refuses a pattern that begins anywhere else.

**`onEntryEnded` can run under the render mutex.** The exit path calls it from
`onExit()`, and the manager takes a `RenderLock` before destroying an activity;
that mutex is not recursive, so a callback that repaints -- `requestUpdateAndWait()`
or its own `RenderLock` -- deadlocks the device. Count, persist, return. Found in
review before any caller existed to hit it.

`isLockedOut` is asked once on entry, so a limiter the caller is already enforcing
shows up before the first press instead of after a wasted attempt -- a power cycle
must not buy a fresh set of guesses. It is its own callback rather than an
empty-path call into the verifier, because a verifier that counts failures would
count that call.

### Counting attempts: `onEntryEnded`

A walk that does not unlock costs the caller exactly one attempt, and the screen
says when: `Config::onEntryEnded` fires once per walk. A walk ends when the
candidate that was put to the verifier is abandoned or exhausted -- **a BACK that
undoes a step**, the eighth point, or the screen leaving. A CONFIRM is its own
counted attempt and does not fire it.

**The BACK case is the one that is easy to miss, and shipping without it made the
limiter unreachable a second time.** Settle only on the forward walk and an
attacker walks to four points (one real check against the verifier), backs off,
walks a different candidate, and repeats indefinitely for a single counted
failure. Found in the second review, 2026-11-23, after the first had closed the
forward path and declared the accounting sound.

**This is not a nicety, it closes a hole.** The first cut said a speculative
probe must count nothing, and with auto-submit on *every* guess is speculative --
so the counted path was never taken at all and an attacker could walk all 7,350
patterns with the failure counter at zero. The doc then claimed auto-submit was
"five times weaker", which understated an unreachable limiter as a factor. Found
in review, 2026-11-23. With `onEntryEnded` the factor is real: one walk yields up
to five candidates and costs one attempt.

### `speculative`, and why auto-submit is off by default

With `Config::autoSubmit`, the screen checks the path after every point from 4
upward, so reaching the last point unlocks by itself with no CONFIRM press. The
verifier is then called with `speculative = true`, and both sides of that matter:

- **Cost.** The wallet's unwrap is 905 ms (`../../../docs/wallet-plan.md`). Five
  probes on the way to an 8-point pattern is four and a half seconds of a rider
  staring at a panel. A verifier that derives a key should answer `Wrong` to a
  speculative probe without doing the work -- or leave auto-submit off.
- **Attempts.** A rider walking toward an 8-point pattern passes through four
  wrong prefixes. A verifier that counted each of those as a failure would lock
  them out of their own device on the way in. So a speculative miss says nothing
  on screen and must count nothing.
- **What it costs in guesses.** One walk of 8 points yields up to five candidates
  for seven presses and one counted attempt, so a limiter is five times weaker
  under auto-submit than under CONFIRM. Decide that per caller; it is why the
  default is off. **A caller that enables auto-submit and ignores
  `onEntryEnded` has no limiter at all.**

## Verified, and not

**Host-tested, 2026-11-23**: 17 tests in `test/pattern_lock/`, all green -- the
grid walk, walls, revisits, the 8-point cap, undo down to the start, the encoding,
the wipe, `isWellFormed` against both its accept and its reject cases, every path
the screen can produce validating, the two space counts, and no encoding collision
across all 7,350 paths.

**And a gap worth stating rather than hiding: none of them touch the screen.**
`test/pattern_lock/` exercises `PatternLock.h`, which is grid arithmetic. The
attempt accounting, the auto-submit path and the lifecycle live in
`PatternLockActivity` and have no automated coverage at all -- they are reachable
only by hand over `CMD:PATTERNLOCK` plus `CMD:BUTTON` on a board. That is exactly
where both reviews found their worst defects, and it is why they were found by
reading rather than by a red test. T-2083.

**On the panel, 2026-11-23, T5 S3 Pro** (board `7c:2c:67:8a:4c:b4`, 540x960,
`env:t5s3pro`, driven by `CMD:PATTERNLOCK` + `CMD:BUTTON` with the framebuffer
read back over `CMD:SCREENSHOT`). What a frame actually showed:

| Checked | Result |
|---|---|
| The screen draws at all | grid, cursor, meter, status line, bottom hints, side hints |
| A direction press moves the cursor one cell | yes, and the meter gains a segment |
| A press into a wall | nothing drawn, nothing counted -- three points stayed three at the right edge |
| CONFIRM on a correct path | `unlocked on 4 points`, activity popped, caller resumed |
| CONFIRM on a wrong path | bold "Wrong pattern", entry reset to the start cell |
| Auto-submit | the last point unlocked with no CONFIRM, on a 4-point and a 6-point pattern |
| A speculative miss | serial said `PATTERNLOCK_TRY:auto:wrong`; the panel still read "4 of 8 points" and said nothing |
| Side hints | "Up"/"Down" tabs clipped by the panel edge exactly as the map's own "+"/"--" are, so house style and not a defect |

Three things that frame changed, all of them layout:

1. The block hung from the header and left ~250 px of white under the status
   line. It is centred in its band now.
2. The grid was sixteen large hollow rings with a filled blob for the cursor,
   which read as holes with one swallowed. It is a lattice of small solid dots
   with a ring around the cursor now -- the cursor is a position, not a
   seventeenth kind of point.
3. The grid took about seven tenths of the panel's width and the progress row sat
   half a pitch under it, reading as a fifth row of the lattice. The pitch is
   `span / (cols + 2)` now, about half the width, and the row sits a full pitch
   below and is exactly as wide as the grid.
4. That row was still a row of dots under a lattice of dots, so moving it was not
   enough. It is a **segmented meter** now -- one pill-shaped outline in the
   theme's own box idiom, one segment per possible point, filled left to right,
   hairlines between the slots still to come. A box around the dots was tried
   first and rejected on the sketch: it made them a tray of dots rather than a
   different thing. **Changing the mark is what separates the two; the box is
   what makes it look like a control.** Confirmed on the panel at three points
   and at the full eight.

`../../docs/device-shots/2026-11-23-t5s3pro-pattern-gate-540x960.png` is the
frame as the panel drew it, 1:1.

**And on an X4 Pro the same day** (`b8:1f:3f:d4:89:bc`, 480x800, `env:x4pro`):
the derived geometry lands on a second panel size with nothing changed, and a
six-point walk plus CONFIRM unlocked. That board is the one T-265 is about --
BACK and CONFIRM there are touch only, and this screen wants six buttons -- so
the question was whether a rider can enter a vertical step at all.

**They can on that board, confirmed by thumb**: the side hint tabs are touch
targets like the bottom boxes are, `MappedInputManager::hintBoxAt()` mapping
`sideHintBox(0)` to `BTN_UP` and `sideHintBox(1)` to `BTN_DOWN`. The maintainer
tapped "Down" and kept tapping, and the frame afterwards shows six points in and
the cursor where the taps put it --
`../../docs/device-shots/2026-11-23-x4pro-pattern-gate-480x800.png`.

**But that board is in `TOUCH_BUTTONS_ONLY`, and the stored default is
`TOUCH_ANYWHERE`** (`CrossPointSettings.h`, `touchMode`), where
`TouchPolicy::hintsVisible()` is false, no boxes are drawn, and
`pumpHintTouch()` returns immediately. So the pass above says nothing about a
factory-default X4 Pro, and the first version of this paragraph -- "T-265 does
not block this screen" -- was true only of the mode that board happened to be
in. Caught in review the same evening.

That is what the screen's own touch controls are for (see "Controls" above), and
**both halves are now confirmed on that board** (2026-11-23, the touch row
switched to `Anywhere` in Settings and switched back afterwards): the theme's
hint boxes and side tabs disappear, the gate draws its own BACK and OK at the
foot of the screen, a tap on the cell next to the cursor steps there, and BACK
and OK answer a thumb. Four points were entered by tapping alone --
`../../docs/device-shots/2026-11-23-x4pro-pattern-gate-touchmode-480x800.png`.

So the claim is: **on a board with two hardware keys the gate is usable in both
touch modes.** Confirmed by thumb in both, which is the only instrument for it.

**Still not checked, and T-2079 is where it is tracked:**

1. **The C3 line.** `env:default` builds clean (T15, 2026-11-23: RAM 18.2 %,
   flash 57.2 %) and has never been on an X4 or an X3 panel -- 792x528 is the
   third framebuffer geometry and the only one still unseen. The X3 was not
   plugged in that day. `env:sticky` and `env:simulator` carry the flag too.
2. **Ghosting.** A framebuffer read cannot show what the panel keeps. Whether a
   `FAST_REFRESH` per cursor move leaves the old ring visible is an eye
   question, and nobody has looked at the glass while walking a pattern.

## The bench command

`CMD:PATTERNLOCK <pattern> [auto]` pushes the screen with a literal-compare
verifier -- the host supplies the pattern, so the screen can be put on a panel
before any caller exists.

```
CMD:PATTERNLOCK 5678        -> PATTERNLOCK_OK:4 points, start 5, auto 0
CMD:PATTERNLOCK 5678 auto   -> same, auto-submit on
CMD:PATTERNLOCK 0145        -> PATTERNLOCK_ERR:malformed   (1 to 4 is a diagonal)
```

Every attempt answers `PATTERNLOCK_TRY:confirm:ok` or `...:wrong`, and a
speculative probe answers `PATTERNLOCK_TRY:auto:...`, so a host can see what the
screen did without reading the panel. **An instrument answering correctly is not
the product working** (`../../../docs/research-rules.md`): the verdict for this
screen is a framebuffer, not a serial line.

**Send it once Home is up, not during boot.** A push that lands while the boot
screen is still on is replaced by Home a moment later: the command answers
`PATTERNLOCK_OK`, the log shows `Entering activity: PatternLock`, and the panel
ends up on Home anyway. It reads as a screen that failed to draw and is not one
(seen 2026-11-23).

Gated on `ENABLE_PATTERNLOCK_CMD`, devel envs only. It reads no stored secret and
writes nothing, so what it hands a stranger is a screen they could already reach
-- but a bench harness has no business in a release image (`platformio.ini`,
"What a stranger gets").
