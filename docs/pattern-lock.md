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

Enumerated, not estimated (`test/pattern_lock/PatternLockTest.cpp`, the two
`TheSpaceIs...` tests, which is where these figures come from and what fails if a
bound moves):

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
discipline of wiping it on exit and after every attempt.

`isLockedOut` is asked once on entry, so a limiter the caller is already enforcing
shows up before the first press instead of after a wasted attempt -- a power cycle
must not buy a fresh set of guesses. It is its own callback rather than an
empty-path call into the verifier, because a verifier that counts failures would
count that call.

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
  for seven presses. A limiter that counts entries rather than candidates is
  therefore five times weaker under auto-submit than under CONFIRM. Decide that
  per caller; it is why the default is off.

## Verified, and not

**Host-tested, 2026-11-23**: 14 tests in `test/pattern_lock/`, all green -- the
grid walk, walls, revisits, the 8-point cap, undo down to the start, the encoding,
the wipe, `isWellFormed` against both its accept and its reject cases, every path
the screen can produce validating, the two space counts, and no encoding collision
across all 7,350 paths.

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

**Still not checked, and T-2079 is where it is tracked:**

1. **Any other board.** Only `env:t5s3pro` has run this. The devel flag also
   reaches `default` (ESP32-C3, 792x528), `sticky`, `x4pro` (800x480) and
   `simulator`, and the geometry is derived per panel, so each is its own case.
2. **Ghosting.** A framebuffer read cannot show what the panel keeps. Whether a
   `FAST_REFRESH` per cursor move leaves the old ring visible is an eye
   question, and nobody has looked at the glass while walking a pattern.
3. **The X4 Pro.** BACK and CONFIRM there are touch only (T-265) and this screen
   assumes six buttons. It may be unusable on that board as written.
4. **The side-button labels per board.** They were right here; the T5 S3 Pro's
   four names for one key is a trap already paid for (`lilygo-t5s3-bringup.md`).

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
