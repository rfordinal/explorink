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

A start cell plus right-angle steps, one cell per press. No diagonals: the
hardware has four directions. A cell may be stepped on twice, because
right-then-left comes back and forbidding that would both surprise a rider and
shrink an already small space.

**How many points is the caller's, not this file's**: `Config::minPoints` and
`Config::maxPoints`, defaulting to 4 and 12. `patternlock::kMaxPoints` is only
the path buffer's bound. Two callers want different things from the same screen
and both are right:

| Caller | Range | Why |
|---|---|---|
| bench harness (`CMD:PATTERNLOCK`) | 4 to 12, the file default | it drives any caller's pattern, so it must not be narrower than the widest of them |
| the wallet (`WalletUnlockActivity`) | 8 to 12 | the path is a KDF password and the floor is a key's floor |
| a future screen lock | its own call | a short gesture in front of a screen is not a key; 4 to 8 is defensible there and is not here |

A caller that sets a range **must pass the same range to `isWellFormed()`** when
it validates a stored pattern. The two numbers disagreeing is the same class of
failure as a wrong start cell: a stored pattern the gate will never let a rider
finish.

The **start is fixed**, because it is the one point no button press produces. Let
it be wherever the cursor rested last and the same four presses mean two different
patterns on two days.

The cells are numbered row by row, and a pattern is a path through them:

```
 0  1  2  3
 4  5  6  7
 8  9  a  b
 c  d  e  f
```

So `"0145"` is **malformed** -- cell 1 is row 0 column 1 and cell 4 is row 1
column 0, which is a diagonal, and `isWellFormed()` refuses it. Two bench
commands were typed that way on 2026-11-23 before anybody drew the grid out;
the error message was right both times and the pattern was wrong.

Encoding is one char per point, in order, start included: `'0'`-`'9'` then `'a'`-`'f'`
for cells 0 to 15. `"048cdef"` is down the left column then along the bottom. That
string is the only thing that leaves the component, and it is what a verifier
compares -- hashing or wrapping it is the caller's job.

## How large the space is

Enumerated, not estimated (`test/pattern_lock/PatternLockTest.cpp`, the three
`TheSpaceIs...` tests, which is where these figures come from and what fails if a
bound moves -- the edge row had no test behind it until a review noticed this
sentence claimed otherwise):

| Start | 4 to 8 points | Bits | 8 to 12 points | Bits |
|---|---|---|---|---|
| corner | 2,810 | ~11.5 | 307,970 | ~18.2 |
| edge | 4,545 | ~12.1 | 498,305 | ~18.9 |
| middle four | 7,350 | ~12.8 | 806,270 | ~19.6 |

So the default start is `kDefaultStart`, row 1 column 1 -- interior. The first
step branches four ways there instead of two, and that is 2.6x a corner start for
no cost at all.

**Each extra point is worth about +1.7 bits** -- four directions less the walls,
a branching factor of about 3.24. `scripts/pattern_space.py` prints both tables
and the per-length breakdown; it is the same enumeration the tests run, so a
number here can be re-derived in a second rather than trusted.

**The floor matters more than the total**, because an attacker searches the
shortest patterns first and nothing stops a rider picking one. From an interior
cell:

| Points | Presses | Walks | Bits |
|---|---|---|---|
| 4 | 3 | 46 | 5.5 |
| 8 | 7 | 5,094 | 12.3 |
| 12 | 11 | 172,646 | 17.4 |

That table is why the wallet asks for 8 to 12 rather than taking the default.
It replaced a 6-to-10-symbol dot PIN worth 12.0 to 20.0 bits, and 4 points would
have dropped the floor by six and a half bits while `docs/wallet-crypto.md` still
promised the old number. 8 to 12 points is 7 to 11 presses -- one more than the
PIN at each end -- for 12.3 to 19.6 bits.

**The pattern is still not the strength.** What makes a wrap hard to attack is
the device secret it is mixed with, and what makes guessing impractical is the
caller's rate limiter (`WalletCrypto.h`, "Why 8 to 12 and not the gate's own
4 to 8"; `../../../docs/wallet-plan.md`).

## The standard system

Android's pattern lock is the obvious predecessor and this screen is a
deliberate variant of it, so the differences are worth naming. Both of the real
ones fall out of the hardware rather than out of taste:

- **No finger, so no lift.** On a phone, releasing the finger is the submit --
  the gesture has an end built into it. Buttons have none, so the end has to be
  a CONFIRM press or the auto-submit probe, and that single missing affordance
  is what produced the whole attempt-accounting problem this screen was wrong
  about twice (see "Counting attempts" above).
- **The path is not drawn.** A phone is held close and put away in a pocket; an
  e-ink panel keeps its last frame with no power, in a bag, in someone else's
  hands.

Everything else about how Android's version behaves -- how it counts a failed
attempt, whether it allows a dot to be used twice, what it does about a stroke
that passes over an unvisited dot -- is `[open]` here. It is widely described
second hand and this repo does not cite memory: settle it by reading AOSP's
`LockPatternView` / `LockPatternUtils` at a named commit before any of it is
written down as fact or used to argue for a change.

## What the hidden path buys

The screen draws the grid, the cursor, and a segmented meter counting how many
points are in. **It never draws the path.**

What that protects: the pattern is not on the panel after the screen leaves. An
e-ink panel holds its last frame with no power, so a device that is lost or stolen
would otherwise be handed over with the secret still visible, ghosting included.

What it does **not** protect: someone watching the entry. The cursor has to be
visible or the grid is pointless, and a watcher who sees every press has the
pattern. The dot PIN this replaced could claim more -- "a shoulder looking at the
panel must learn nothing" -- and a grid cannot. That claim was **given up, not
forgotten**, when the wallet moved to this screen on 2026-11-26: anybody reading
`docs/wallet-crypto.md` is reading about a gate a watcher can read off the panel,
and nothing in the product text may say otherwise.

## The verifier contract

The screen owns no secret. The caller hands it:

```cpp
PatternLockActivity::Config config;
config.title = tr(STR_WALLET_UNLOCK);
config.minPoints = wallet::kPatternMinPoints;   // 8: the path is a KDF password
config.maxPoints = wallet::kPatternMaxPoints;   // 12
config.startCell = wallet::KeyStore::patternStart();
config.isLockedOut = [] { return wallet::isLockedOut(wallet::KeyStore::failures()); };
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

**Host-tested, 2026-11-23, extended 2026-11-26**: 21 tests in
`test/pattern_lock/`, all green -- the grid walk, walls, revisits, the length cap,
undo down to the start, the encoding, the wipe, `isWellFormed` against both its
accept and its reject cases and against a caller's own range, a caller's range
being clamped rather than obeyed, every path the screen can produce validating,
four space counts, and no encoding collision.

**The producer is walked against its own validator exhaustively, and the count is
exact**: all 808,526 paths of 4 to 12 points from an interior start are generated
by `Entry` and each one fed to `isWellFormed()`. An exact count rather than a
floor, because the whole point of that test is that a disagreement between the
two halves is a lockout, and "enough of them agreed" is not an assertion that
catches one.

**And a gap worth stating rather than hiding: none of them touch the screen.**
`test/pattern_lock/` exercises `PatternLock.h`, which is grid arithmetic. The
attempt accounting, the auto-submit path and the lifecycle live in
`PatternLockActivity` and have no automated coverage at all -- they are reachable
only by hand over `CMD:PATTERNLOCK` plus `CMD:BUTTON` on a board. That is exactly
where both reviews found their worst defects, and it is why they were found by
reading rather than by a red test. T-2083.

**On the panel, 2026-11-26, Xteink X3** (board `7c:e8:b1:6f:c2:9c`, 792x528
framebuffer, `env:default`, ESP32-C3) -- the first run of this gate with a real
secret behind it, driven by `CMD:BUTTON` written straight to the port and read
back with `CMD:SCREENSHOT`:

| Checked | Result |
|---|---|
| The gate opens on the cell NVS stored | cursor ring on cell 5, before any press |
| The meter is the caller's length | twelve segments, status line "At least 8 points" |
| Seven presses | "8 of 12 points", cursor on cell 12, **path not drawn** |
| CONFIRM on the stored pattern | verifier 902,552 us, `unlocked on 8 points`, popped to `WalletUnlock`, which replaced itself with `Wallet` |
| The browse list behind it | "Gate bench, 1 page" off a manifest that only the unwrapped key could decrypt |
| CONFIRM on a wrong 8-point walk | bold "Wrong pattern", meter back to one, entry at the start cell |
| That attempt against the limiter | `attempts` went 0 to 1 -- the panel moves the same counter the serial command does |
| The locked-out dead end | message drawn, **no `PatternLock` pushed at all** -- the shim refuses a gate that cannot succeed |

**The C3's PBKDF2 cost, measured at last.** `docs/wallet-crypto.md` carried
"the C3 figure stays unmeasured -- an S3 does not answer for it" from the day
the wrap was written. At 18,000 iterations on this X3: **903,157 us to
provision, 902,558 us to unwrap**. The prediction from 19,900 iters/s was
~904 ms, so the generator's default was right.

**And the number that is uncomfortable.** An unlock pushes the main loop to a
**4,109 ms** maximum (`LOOP New max loop duration`), against a task watchdog
that panics at 5 s (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`). That is 82 % of the
budget spent on one button press: 903 ms of PBKDF2 plus the panel work behind
it. It did not trip, and nothing here says how much headroom a slower card or a
longer list would leave. Open -- T-2089.

**On the panel, 2026-11-26, Xteink X4 Pro** (board `b8:1f:3f:d4:89:bc`,
800x480, `env:x4pro` plus `-DENABLE_WALLET_TEST_CMDS=1`, ESP32-S3) -- the S3
half, which the protocol asks for because this gate compiles into every
environment (`../../docs/device-test-protocol.md`, step 1):

| Checked | Result |
|---|---|
| The gate at 800x480 | grid, cursor on the stored cell 5, twelve segments, "At least 8 points" -- same layout, narrower panel |
| The whole walk and CONFIRM | unlocked, `WalletUnlock` replaced itself with `Wallet` |
| Provisioning on an S3 | 1,034,466 us, against 1,047 ms measured on this board on 2026-11-23 -- the KDF cost did not move |

**What the board was carrying is the part worth keeping.** It still held a
wallet provisioned on 2026-11-23, under the directional PIN. That is the
upgrade case, and it surfaced a defect in the first cut of this change: a wrap
with no stored start cell was lumped in with no wrap at all, so the panel said
**"No wallet key on this device"** on a device that has one. The key was there
and unwrappable; only the PIN that reaches it had stopped existing. It is its
own state and its own sentence now -- "This wallet predates the pattern lock /
Set it up again".

There is **no migration**, and the screen should not imply one. The KEK needs
the secret, and nothing can re-derive a PIN from a wrap. A wallet provisioned
before this gate has to be provisioned again, which means the documents on the
card are readable only if K is still held somewhere else -- the phone's own
keystore, in practice (`../../docs/android-wallet.md`).

A second layout defect came off the same board. The locked-out and pre-gate
messages were single strings, and "This wallet predates the pattern lock. Set
it up again." is 52 characters: it fit the X3's 528 px and was clipped at
**both** ends on this 480 px panel. Two lines now, bold label over plain
detail, mirroring `RouteEmptyActivity` -- which is the house shape for an
empty-state explanation and was there to copy. A one-line message is sized
against one panel, and that is the defect CLAUDE.md's "Styles must be
universal" names.

**Still not on a panel**: `env:t5s3pro`, and this gate's **own touch controls**.
The X4 Pro drew the theme's hint boxes, so `TouchPolicy::hintsVisible()` was
true on it -- the TOUCH_ANYWHERE path where the screen draws its own BACK/OK
boxes (`PatternLockActivity.h`) has still never been seen. Nor has the browse
list behind the gate on an S3: this board's card carries a tree encrypted under
a different key, so the unlock correctly ended on "Wrong key, or the wallet
list was altered" rather than a list. That half was proven on this same board
on 2026-11-23 and the gate swap does not touch it.

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
| A candidate abandoned with BACK | one `PATTERNLOCK_ENTRY_ENDED`, and the next candidate's BACK another -- the backtracking hole, confirmed closed on both boards 2026-11-23 |
| A wrong walk reaching the last point | one `PATTERNLOCK_ENTRY_ENDED`, "Wrong pattern" on the panel, meter back to the start |
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

**And on an X3, 2026-11-26** (`7c:e8:b1:6f:c2:9c`, `env:default`, ESP32-C3,
528x792 -- the third framebuffer geometry and the last one unseen): the gate
draws, a step moves the cursor, the wall holds (four `right` presses from the
interior start leave three points and the cursor on the right column), a
six-point walk plus CONFIRM unlocks, and CONFIRM at three points is inert.
`../../docs/device-shots/2026-11-26-x3-pattern-gate-528x792.png`.

Worth knowing for the next pass on that board: the theme puts the side hints on
**opposite edges** there -- "Up" left, "Down" right -- where the T5 S3 Pro stacks
both on the right. Neither collides with the grid, which the layout gets by
construction from `buttonHintsRect()` and by luck from the side boxes.

**Still not checked, and T-2079 is where it is tracked:**

1. **`env:sticky` and `env:simulator`** carry the devel flag and nothing has run
   the screen there. Both are the same code paths as envs already passed; this is
   the weakest of the gaps.
2. **Ghosting.** A framebuffer read cannot show what the panel keeps. Whether a
   `FAST_REFRESH` per cursor move leaves the old ring visible is an eye
   question, and nobody has looked at the glass while walking a pattern.

## The first real caller: the wallet

Until 2026-11-26 the only thing that pushed this screen was the bench harness.
The document wallet is the first caller that brings a secret, and the shape of
that call is what the Config fields were built for.

`WalletUnlockActivity` keeps the half this screen refuses to hold: the wrap, the
persisted failure count, the rate limiter, the start cell, and the item a
`CMD:GOTO_WALLET 0 3` asked for before the gate appeared. It pushes the gate and
gets one bit back. The verifier is `wallet::KeyStore::tryUnlock()` -- the same
call `CMD:WALLETUNLOCK` drives, which is what keeps the serial command a test
driver rather than a test bypass.

Three things that call had to settle, and every one of them is a field rather
than a change to this screen:

- **The range.** 8 to 12, not the default, because the path is the KDF password
  and the floor is a key's floor. See "How large the space is".
- **The start cell.** `Config::startCell` comes from NVS (`pstart`), written at
  provisioning. The wrap says nothing about the path that made it, so without a
  stored start the gate would open on `kDefaultStart` and refuse every pattern a
  rider can walk, permanently. `KeyStore::provision()` validates with
  `isWellFormed()` against the start it is about to store, so the two cannot
  disagree.
- **Auto-submit off.** The verifier is a 905 ms PBKDF2. Probing it after every
  point from the eighth would spend four seconds against a five-second task
  watchdog. `onEntryEnded` is therefore unused here: with auto-submit off every
  verifier call is a CONFIRM and `tryUnlock()` counts it.

What this screen gave up to take the job is written in "What the hidden path
buys": the dot PIN could claim a shoulder learns nothing, and a grid cannot.

## The bench command

`CMD:PATTERNLOCK <pattern> [auto]` pushes the screen with a literal-compare
verifier -- the host supplies the pattern, so the screen can be put on a panel
before any caller exists.

```
CMD:PATTERNLOCK 567b        -> PATTERNLOCK_OK:4 points, start 5, auto 0
CMD:PATTERNLOCK 567b auto   -> same, auto-submit on
CMD:PATTERNLOCK 567ba98c    -> PATTERNLOCK_OK:8 points, start 5, auto 0
CMD:PATTERNLOCK 0145        -> PATTERNLOCK_ERR:malformed   (1 to 4 is a diagonal)
CMD:PATTERNLOCK 5678        -> PATTERNLOCK_ERR:malformed   (7 to 8 jumps a row)
```

That last line **was this doc's own worked example** until 2026-11-26, written
as a success. Cell 7 is row 1 column 3 and cell 8 is row 2 column 0, so it is
the exact error the paragraph above it warns about, in the paragraph that warns
about it. The grid is drawn out in "What a pattern is" for a reason: reading
`5678` as four adjacent cells is what everybody does, and the numbers wrap.

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
