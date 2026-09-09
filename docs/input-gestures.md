# Input: how a tap, a double tap and a hold are recognised

Written 2026-09-06 after three sessions in a row produced gesture bugs on the
LilyGo T5 S3 Pro's capacitive home key. It is a **design analysis, not a
description of a finished system**: the layering described under "What is wrong"
is what the code does today, and the layering under "The proposed shape" is not
built. Every claim carries how it is known.

`docs/touch-modes.md` is the touch-mode feature this analysis came out of. This
file is about the input layer underneath it.

## What is wrong: gesture recognition is not a layer

`[read]` Tap / double tap / hold / auto-repeat are re-implemented in **six
places**, each with its own timer and its own threshold. Worse, they are stacked,
so one recogniser's *output* is another recogniser's *level input*:

| where | what it recognises |
|---|---|
| `freeink-sdk/.../InputManager.cpp`, `applyStateChange` and the debounce | GPIO/ADC key edges, one global hold timer for the whole bitmask |
| the same file, `updateConfirmBackHold` / `updateConfirmPowerHold` | board-style long presses, emitted as a **fake key level** |
| the same file, `pollGt911` | the GT911 home key's tap and hold |
| `src/main.cpp`, `boardButtonHook()` | the T5 S3 Pro user button and BOOT: edges, a 600 ms hold that repeats every 500 ms, taps-on-release |
| `src/MappedInputManager.cpp`, `pumpHomeKey` / `pumpHintTouch` | a second home-key recogniser, plus synthetic key levels for the hint boxes |
| per activity, and `src/util/ButtonNavigator.cpp` | long press by polling a held time, and auto-repeat |

Two terms, because the rest of this file needs them. A **level** is "is this key
down right now". An **edge** is "it changed just now". No layer today owns the
truth about either: not "which keys are down", not "when did that edge really
happen".

### The stacking, concretely

`[read]` `boardButtonHook()` (`src/main.cpp`) is a full recogniser that runs
**inside** `InputManager::update()`. It calls `cycleFrontlight()` from there — a
side effect inside the sampler — and it publishes its taps as a **synthetic
CONFIRM or BACK level** held for at least three polls and 20 ms, purely so the
SDK's own debounce will accept it. So the chain is: recogniser, fake level, debouncer,
edge, app recogniser. Four layers to express one tap.

### `getHeldTime()` describes the wrong input, three different ways

`[read]` `InputManager::getHeldTime()`
(`freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:445`) returns
`buttonPressFinish - buttonPressStart`, and `buttonPressStart` is set only when
nothing was already down (`:277`). Consequences:

1. **With nothing down it returns the *previous* press's duration.** That is a
   value about the past, answered as if about the present. It is what made a tap
   on an on-screen hint box read as a half-second hold, once a 600 ms frontlight
   hold had left a big number behind (`docs/touch-modes.md`).
2. **In a chord it returns the first key's duration for every key.** Back pressed
   while Up is held reports Up's hold time, so a long-press-Back feature fires
   instantly.
3. About fifteen call sites read it at arbitrary times, not only on a release
   edge, which is the only frame where it means anything.

### The GT911 home key: edges gated, hold timer not

`[read]` `pollGt911()` reads the key from bit `0x10` of register `0x814E`. The
press/release **edges** are taken only after the buffer-ready gate
(`InputManager.cpp:943`, `if (!(status & 0x80))`), while the **hold timer** runs
from a latched `touchHomeKeyDown` above that gate. The split is deliberate and
the comment says why: a motionless hold stops producing new-data frames, so a
gated timer would never cross `HOME_KEY_LONG_PRESS_MS`
(`InputManager.h:263`, 700 ms).

`[read]` The cost is that `touchHomeKeyDown` (`:957`) is only ever re-synced from
a fresh frame. A **missed release edge** therefore leaves the key latched down,
and the hold fires later from a press that another layer already turned into a
tap. There is no staleness notion at all — unlike the CHSC6x path in the same
file, which self-heals with a release-by-timeout.

`[read]` **The controller holds a frame until it is cleared.** M5GFX's GT911
driver (`Touch_GT911.cpp`, in the M5GFX library the T5 S3 Pro build pulls in)
discards the frame and re-reads after a gap, on the stated grounds that the GT911
keeps the same frame until zero is written to `0x814E`. So a frame read after the
main loop was blocked describes the past, and our code trusts it.

`[measured]` On the T5 S3 Pro a single loop iteration blocks for **seconds**
during a map render, and a windowed panel refresh alone costs ~1,081 ms
(`docs/map-follow.md`, measured over a 4 h 36 min walk 2026-09-05). So the
blocked-loop case is not an edge case on this board.

## What that produces, measured

`[measured]` 2026-09-05, maintainer, on the device: **one double tap on the Home
screen locked the touch panel, opened the map, and lit the frontlight** once the
map had finished rendering. Three gestures from one.

`[measured]` A deliberate double tap regularly read as two separate single taps
against a 300 ms window.

`[inferred]` The explanation is a mixture of a missed release edge (which lets a
pending single tap expire into a Select, and leaves the latched hold to fire
later) and more tap events than gestures. **Whether the extra events are contact
bounce or stale frames is open**, and it decides the fix: bounce wants a minimum
press width, stale frames want the frame discarded. Nobody has logged the events.

## What is in place today, and why it is a patch

`src/MappedInputManager.cpp`, `pumpHomeKey()` holds the first tap for a 500 ms
window, ignores taps for 500 ms after a resolved gesture, and believes a hold only
while no tap has been made of the current press. It works against the symptoms
and it is the wrong shape: it filters a noisy stream instead of making the stream
trustworthy, and its window measures *poll latency* rather than finger timing,
because the events it times are stamped when they are read.

## The proposed shape

**Not built.** Three layers, one vocabulary, a spec per key.

- **L0, level sources.** GPIO/ADC, the expander button, the GT911 key bit, touch
  contact, and app-injected virtual keys. They report a level and a sample time
  and nothing else. No timers.
- **L1, edge layer, per key.** Owns `down` and `downAt` and emits
  `Down` / `Up` / `Cancel`. Two policies: a *continuous* level (GPIO, expander)
  keeps today's debounce; a *frame-latched* level (GT911) discards a frame read
  after a sampling gap and re-reads, and a key seen down before the gap and up
  after it emits **`Cancel`** — the release time is unknown, so neither a tap nor
  a hold may be claimed from it.
- **L2, one recogniser per key**, parameterised
  `{longMs, doubleWindowMs, repeatStartMs, repeatIntervalMs, minPressMs}`, emitting
  `Tap` / `DoubleTap` / `LongPress` / `Repeat`. The only permitted duration query
  is `heldMs(key)`, which is `now - downAt` while down and **0 otherwise**.

The frame-latched sources must be polled from a **task**, not from `loop()`, or
the edge timestamps keep measuring loop latency. The SDK already contains an
async poll task; this firmware never starts it (`grep beginAsync` finds no call
in our tree), and it queues too little to drive a recogniser as written.

`Cancel` over guessing is the deliberate trade: a genuine tap that straddles a
render is dropped and the rider taps again, which is cheaper than a phantom hold
or a mode toggled by a stale frame, each of which costs a 1–2 s refresh.

Split: L0/L1/L2 belong in **`freeink-sdk`**, which is our own fork
(`github.com/rfordinal/freeink-sdk`), so this is a fix at the source rather than
a workaround. The app keeps the key-to-meaning mapping, the hint-box hit test
(it needs the theme), and a facade over the legacy
`wasPressed`/`wasReleased`/`isPressed` so the ~96 existing call sites compile
unchanged.

## Failure inventory

Ranked by how likely it is to bite. Everything here is `[read]` from the code
unless marked otherwise.

1. **Any edge inside a blocking render is gone.** `gpio.update()` runs once per
   loop iteration; a press and release both inside the gap never happen as far as
   the firmware is concerned. On the GT911 the tap leaves a `count=0` frame, so no
   press edge ever existed.
2. **A missed home-key release has two more consequences** beyond the frontlight
   case: the second tap of a double tap expires into a Select, and the stale
   latched key turns the rider's *next screen touch* into a tap event seconds
   late. `[inferred]`
3. **Blocking sub-loops eat resolved gestures.** `CrossPointWebServerActivity`
   and `FontDownloadActivity` call `mappedInput.update()` themselves; each pump
   resolves and clears the one-shot gesture flags, and only `Back` is read. The
   home key's Select, the touch lock and the frontlight are lost for the whole
   duration of those screens.
4. **The power-saving cadence drops short presses.** After three idle seconds the
   loop delay becomes 50 ms while the debounce needs the same state in two
   samples; a tap shorter than one gap is never committed. `[inferred]`
5. **`getHeldTime()`** — the three problems above.
6. **The T5 S3 Pro user button cannot long-press Confirm at all.** Its hook emits
   a ~30 ms synthetic pulse *after* release, so `isPressed(Confirm) &&
   getHeldTime() >= X` is unreachable on that board — every reader feature built
   that way is dead there.
7. **Synthetic gestures answer both `wasPressed` and `wasReleased` in one frame**
   (the home-key Confirm and the left-edge Back swipe), so an activity that tests
   both acts twice. The Back swipe also still answers `wasSwipe() == Right`.
8. **`ButtonNavigator` swallows one release after a cancel.** It clears its
   continuous-run marker only on a release edge, and a cancelled hint-box press
   emits none.
9. **A GT911 NACK on the user button reads as a release** (`BoardT5S3.cpp`
   returns false on a failed read), so one bus glitch mid-hold is a spurious
   Select plus a restarted hold timer. `[read]`, frequency `[needs-measurement]`.
10. **Only touch point 1 is decoded**, so a second contact makes the reported
    point jump past the slop and a release can decode as a swipe. `[inferred]`

## Open, with the measurement that settles each

1. **Does the GT911 raise the buffer-ready flag periodically while a finger sits
   motionless, or only on change?** The SDK comment asserts "only on change" with
   no source; M5GFX's retry-after-clear implies a fresh frame does arrive while
   touched. Hold the key three seconds and log `0x814E` every 5 ms, once clearing
   after each read and once not. Goodix's *GT911 Programming Guide* register
   table (`0x8056` refresh rate, `0x814E` status, `0x8093` key value) and the INT
   description in the datasheet are the paper sources. **The whole L1 policy
   depends on this.**
2. **Does an uncleared frame hold or get overwritten on this panel?** Block the
   loop two seconds, tap during it, dump the first status after. Decides whether a
   straddling tap appears as a stale down or vanishes.
3. **Are the extra tap events bounce or stale frames?** Only a timestamped edge
   log answers it. Decides `minPressMs`.
4. **Who holds the T5 S3 Pro's I2C mutex during a panel refresh?** If the panel
   driver holds it for the whole refresh, a poll task stalls with it and the
   design degrades to cancel-after-gap; the GT911 would then need its own bus or
   an interrupt-driven read.
5. **The natural double-tap interval on this key**, with true timestamps. Sets the
   window; today's 500 ms is a guess on top of poll latency.

## Load-bearing behaviour a redesign must not break

- The ADC-ladder debounce, and the boot-time trick that absorbs a held button as
  a non-edge, which depends on it.
- `getPowerButtonHeldTime()` and `verifyPowerButtonWakeup()` — a per-key hold that
  already works the way the design generalises.
- The Confirm/Back and Confirm/Power hold styles for other boards.
- Tap position is the **first** contact sample, not the last.
- A hold suppresses the tap, on both the home key and the user button.
- The frontlight fires at the hold threshold while the key is still down — the
  light comes on under the thumb, which is the feedback that says "let go".
- Hint boxes: box index is the hardware button index, an empty label is not a
  button, the hit test runs in portrait, and sliding off fires nothing.
