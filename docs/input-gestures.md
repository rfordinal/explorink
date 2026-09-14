# Input: how a tap, a double tap and a hold are recognised

Written 2026-09-06 after three sessions in a row produced gesture bugs on the
LilyGo T5 S3 Pro's capacitive home key. **Confirmed on the X4 Pro 2026-09-14**,
so it is not one board's problem; the same pass added "How other systems do it",
which reads five foreign implementations against the design proposed here. It is a **design analysis, not a
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

`[measured]` 2026-09-14, X4 Pro. **That claim is true for the key and false for a
contact**, and the split matters. A held key produces **no frame at all** -- seven
presses, one `0x90` frame each, then silence for the whole 55-90 ms hold. A held
contact produces a new frame **every 10 ms** without pause. So the hold timer
above the gate is not caution, it is the only way the key's hold can be timed;
and any design that assumes "held means frames keep coming" is right about the
glass and wrong about the key.

`[read]` The cost is that `touchHomeKeyDown` (`:957`) is only ever re-synced from
a fresh frame. A **missed release edge** therefore leaves the key latched down,
and the hold fires later from a press that another layer already turned into a
tap. There is no staleness notion at all — unlike the CHSC6x path in the same
file, which self-heals with a release-by-timeout.

`[measured]` **The controller holds a frame until it is cleared, and while it
holds one it produces no other.** M5GFX's GT911 driver (`Touch_GT911.cpp`, in the
M5GFX library the T5 S3 Pro build pulls in) discards the frame and re-reads after
a gap, on the stated grounds that the GT911 keeps the same frame until zero is
written to `0x814E`. Measured on the X4 Pro 2026-09-14 and it is worse than that
wording suggests: in `noclear` mode the status byte sat on **one** frame for
**7.99 s of a 8.00 s capture** -- 1,599 of 1,601 samples -- while the key was
tapped over and over, and **not one of those taps reached the register**. The
frame is not merely stale. It is a lock, and everything that happens behind it is
discarded.

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

`[measured]` 2026-09-14, maintainer, on the **X4 Pro**: a double tap made while
the map is rendering is regularly read as a single tap, and has to be repeated
many times before it takes. The same double tap on a static screen -- Home,
Settings, anywhere the loop is not blocked -- lands first time. That moves the
fault off one board. It was measured on the T5 S3 Pro first, and the X4 Pro is
the reference device.

`[read]` Two ways a blocked loop eats that tap, and the report matches both.
Either the gap between polls is longer than `HOME_KEY_DOUBLE_TAP_WINDOW_MS`, so
the first tap has already expired into a Select before the second one is read;
or both edges of one tap fall inside a single gap, and the GT911 reports only
the state at the next read, so that tap never existed. Neither needs a second
mechanism to explain the symptom.

`[inferred]` The explanation is a mixture of a missed release edge (which lets a
pending single tap expire into a Select, and leaves the latched hold to fire
later) and more tap events than gestures. **Whether the extra events are contact
bounce or stale frames is open**, and it decides the fix: bounce wants a minimum
press width, stale frames want the frame discarded. Nobody has logged the events.

## What the controller and the loop actually do, X4 Pro, 2026-09-14

`[measured]` All of this is `CMD:TOUCHLOG` and `CMD:LOOPGAP` on one X4 Pro,
serial `b8:1f:3f:d4:89:bc`, sampling at 5 ms. Every capture quoted below carries
zero dropped rows; a lossy one is named as lossy and nothing is concluded from
it.

### The controller

| what | what it sends |
|---|---|
| nothing touching | no frames at all. Longest observed silence 880 ms, status `0x00`, INT high |
| a contact on the glass | a new frame every **10 ms**, median exactly 10,000 us over 146 intervals, and it does not stop while the finger rests |
| **the capacitive home key, held** | **one frame at the press, then nothing** until the release |
| any release | exactly **three** `0x80` frames about 10 ms apart, key and glass alike |

`[measured]` **The key is a separate pad, not a screen region.** Its frames are
`0x90` -- ready plus the key bit, contact count **zero**. Contacts near the bottom
edge of the glass report `y` between 773 and 791 of 799 with no key bit, so the
pad sits below the digitizer and is easy to miss: two captures made while the
maintainer believed a finger was on the key contain contacts and no key bit at
all.

`[measured]` **INT is exact.** Across two complete captures, every one of 868
frames with bit 7 set had the INT line asserted, and no frame with bit 7 set ever
appeared with INT high. While a frame is pending and unacknowledged, INT stays
asserted with a single 5 ms release every 345 ms (28 of them in 8 s, unexplained).

### The loop

`[measured]` `CMD:LOOPGAP` records the interval between successive
`gpio.update()` calls -- the sampler, not `loop()`.

| screen | typical gap | worst single gap |
|---|---|---|
| Home, idle | ~15 ms (265 of 284 in the 10-20 ms bucket) | 78 ms |
| map, steady state | **~53 ms** (547 of 548 in the 50-100 ms bucket) | -- |
| map, plain `redraw` | ~53 ms | **2.80 s** |
| map, opening it (tiles + render + panel) | ~53 ms | **4.34 s** |

The firmware's own log agrees on the last one: `New max loop duration: 4292 ms`,
of which render 2,171 ms and panel wait 1,341 ms.

### The mechanism, end to end

Put the two together and the reported failure follows with nothing left over.

1. The loop's last poll cleared `0x814E`, so the controller is free.
2. The map render blocks the sampler for 2.8 to 4.3 s.
3. The first edge in that window -- say the first tap's `0x90` -- latches.
4. Its release, the second tap and *its* release all happen behind the lock and
   are **discarded**.
5. The loop returns, reads `0x90`, clears. The controller then latches whatever
   is true now, a finger-off `0x80`.
6. The firmware saw a press and a release. **One tap.**

So a double tap made during a render does not arrive late and does not arrive
twice: it **collapses to a single tap**, and which edge survives depends on where
the window boundary fell, which is why it takes many attempts.

`[measured]` **It is marginal even when nothing is rendering.** The map's steady
sampler gap is 53 ms and the key's press-to-release is 55-90 ms. The margin is
one or two tens of milliseconds, so the key is unreliable on the map screen
whether or not a refresh is in flight. Home, at 15 ms, has four times the margin
and that is where the gesture is reported to work.

### What this rules out

**Not clearing the status and draining later is dead.** It is the obvious
workaround and the measurement kills it: an unacknowledged frame stops the
controller reporting anything at all, so the device would go from losing a
gesture to losing every gesture until something cleared the register.

That leaves one class of fix, and it is the one the redesign already proposes:
**read promptly**, from a task or from the INT line, so no window of that width
ever exists. Every implementation surveyed in "How other systems do it" does
exactly that, and mainline Linux drives this same chip from the interrupt that is
wired to GPIO10 on this board and read by nothing.

## What is in place today, and why it is a patch

`src/MappedInputManager.cpp`, `pumpHomeKey()` holds the first tap for a 500 ms
window, ignores taps for 500 ms after a resolved gesture, and believes a hold only
while no tap has been made of the current press. It works against the symptoms
and it is the wrong shape: it filters a noisy stream instead of making the stream
trustworthy, and its window measures *poll latency* rather than finger timing,
because the events it times are stamped when they are read.

## How other systems do it

`[read]` 2026-09-14, five implementations, each opened in its own source. Short
version: **the shape proposed below is the ordinary one, and today's code is the
unusual part.** Foreign files are cited at the commit that last touched them,
because a line number in an unpinned repo rots.

### Android

`GestureDetector.java` (`aosp-mirror/platform_frameworks_base`, `2091e8f3e2f2`),
`isConsideredDoubleTap()` decides from the **event timestamps**, not from when
the app read them:

```java
final long deltaTime = secondDown.getEventTime() - firstUp.getEventTime();
if (deltaTime > DOUBLE_TAP_TIMEOUT || deltaTime < DOUBLE_TAP_MIN_TIME) return false;
```

`ViewConfiguration.java` (`8b948e548b78`) holds the numbers: `TAP_TIMEOUT = 100`,
`DOUBLE_TAP_TIMEOUT = 300`, `DOUBLE_TAP_MIN_TIME = 40`, `TOUCH_SLOP = 8`,
`DOUBLE_TAP_SLOP = 100`. `onTouchEvent()` handles `ACTION_CANCEL`.

Two things follow for this file.

- **The 500 ms window here is not evidence that fingers are slow.** 300 ms is the
  number when the interval is measured at the sample. Ours measures poll latency
  instead. Stamping the sample puts 300 ms back in play.
- **`DOUBLE_TAP_MIN_TIME = 40` is what open question 3 is asking for**, and it is
  a lower bound on the interval rather than a refractory period after a resolved
  gesture. A lower bound drops a bounce and keeps a fast third tap.
  `HOME_KEY_REFRACTORY_MS` drops both.

### The web platform

W3C Pointer Events Level 3, section 4.2.7, "The pointercancel event": the user
agent MUST fire `pointercancel` when it detects a scenario to suppress a pointer
event stream. A standard event type whose only job is "this stream can no longer
be trusted, do not make a gesture of it". The `Cancel` edge below is that idea.

### Linux, on this exact chip

Mainline `drivers/input/touchscreen/goodix.c` (`torvalds/linux`, `5ed62a96e06b`)
is interrupt driven: `devm_request_threaded_irq()` with `IRQF_ONESHOT`, and
`goodix_ts_irq_handler()` reads the report then writes 0 back to the coord
register to clear it.

It gates on the same bit this firmware does -- `GOODIX_BUFFER_STATUS_READY`,
which is `BIT(7)`, our `status & 0x80` -- but there the bit is a **validity check
after an interrupt**, never the event itself. The comment in
`goodix_ts_read_input_report()` says why:

> The 'buffer status' bit, which indicates that the data is valid, is not set as
> soon as the interrupt is raised, but slightly after. This takes around 10 ms to
> happen, so we poll for 20 ms.

**The INT line is wired on our boards and nothing uses it.** SDK `BoardConfig.h`,
X4 Pro profile: *"CONFIRMED ON HARDWARE: INT=GPIO10, RST=GPIO4"*. The T5 S3 Pro
profile names `INT3`. The only `attachInterrupt` in the whole SDK is the EPD busy
line, in `FreeInkDisplay/src/bus/EpdBus.cpp`.

### Espressif, twice, and both are the three-layer shape

ESP-IDF `components/touch_element`, read from the pinned `5.5.2.260206` on disk.
An ISR pushes into `intr_msg_queue` with `xQueueSendFromISR`; a periodic
`esp_timer` turns that into Press / Release / LongPress; the app drains a second
queue, `event_msg_queue`. Two queues, and the recogniser never runs in the app's
loop.

`esp-iot-solution` `components/button/iot_button.c` (`69fbec42dcae`) is the usual
answer to double click on an ESP32. One `esp_timer` at
`CONFIG_BUTTON_PERIOD_TIME_MS` (default 5 ms) drives a state machine per button,
counted in ticks of that period rather than in `millis()`. Its `Kconfig`
defaults: debounce 2 ticks, short press 180 ms, long press 1500 ms.
`BUTTON_DOUBLE_CLICK` and `BUTTON_MULTIPLE_CLICK` are counted in `button_handler()`.

This one also answers the upstream question: a layer built the way Espressif's
own components are built is easier to argue in a `freeink-sdk` PR than something
invented here.

### LVGL, the one that also polls

LVGL polls and still keeps the timing. `lv_indev.c` (`lvgl/lvgl`, `991e067db3af`),
in `indev_read_core()`, lets the driver stamp its own sample and only falls back
to read time when it did not:

```c
/*Set the time stamp to the current time is it was not set in the read_cb*/
if(data->timestamp == 0) data->timestamp = lv_tick_get();
```

Long press is then `lv_tick_diff(i->timestamp, i->pr_timestamp)` -- against the
sample's stamp, not the current tick. Reading runs on its own `lv_timer` created
in `lv_indev_create()`, not on the app's frame loop. `LV_EVENT_PRESS_LOST` is its
cancel.

### Three rules all five share

1. The sampler and the recogniser run off the thread that draws.
2. Gesture timing is measured from when the sample was taken.
3. There is a way to say "do not make a gesture of this stream".

`pumpHomeKey()` does none of the three. The proposed shape below is a return to
the ordinary one, not a rewrite for elegance.

### What is ours, and not borrowed

**Rejecting a frame read after a sampling gap, and emitting `Cancel` for a key
seen down before the gap and up after it, appears in none of the five.** None of
them needs it: their sampler never stalls. Ours stalls for seconds during a map
render. The rule stands on that ground alone, and it gets labelled as ours rather
than as prior art.

None of this section is measured on our hardware. It is five foreign sources
read.

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

## The instrument: `CMD:TOUCHLOG`

`[read]` Built 2026-09-14 for step 1 of T-266. Every open question below is a
question about **when a byte changes**, and nothing in this firmware could see
that: what gets recorded today is the recogniser's opinion after the fact, and
that opinion is stamped when the loop got round to reading it.

`src/DebugTouchLog.h` has the reasoning; the shape in one place:

```
CMD:TOUCHLOG                     3000 ms at 5 ms, clearing after each ready frame
CMD:TOUCHLOG 6000 5000 noclear   6 s at 5 ms, never acknowledging a frame
```

Per sample it records `micros()` at the read, the status byte at `0x814E`
(bit 7 buffer-ready, bit 4 the capacitive home key, bits 3..0 the contact
count), and **the level on the GT911's INT line**, which mainline Linux treats
as the event and this firmware does not read at all. Output is run-length
encoded on the (status, INT) pair, so a state that holds shows as one line with
a repeat count rather than as 600 identical lines.

Three deliberate choices, each of which is the answer to a question the log
would otherwise beg:

- **The capture blocks `loop()` and never calls `gpio.update()`.** The failure
  only happens while the loop is blocked, so an instrument that keeps polling
  measures a state the bug does not live in.
- **`clear` versus `noclear` is open question 2 made runnable.** `clear` writes
  0 back to `0x814E` after every ready frame, the way `goodix_ts_irq_handler()`
  does; `noclear` never writes, so a frame left unacknowledged stays visible for
  as long as the controller holds it.
- **A failed read is recorded as `0xFF`, not dropped.** A silent gap in a log
  reads as a quiet controller, and telling those two apart is half the point.

This is the ordinary instrument rather than an invention: Linux has `evtest`,
Android has `getevent`, and both print raw timestamped events before any gesture
layer interprets them. Neither reaches here, because both sit above a driver we
do not have -- this is the same idea pushed down to the one register that driver
would read.

Devel builds only, gated on its own `ENABLE_TOUCHLOG_CMD`, set on `x4pro` and
`t5s3pro` (the two boards with a GT911) and on no release env. Serial only,
never on the BLE grammar: the reply says nothing about the rider, but a command
that freezes the screen for eight seconds is a denial of service for whoever
picks up a lost device, and BLE advertises with no pairing and no bonding.

### `CMD:LOOPGAP`, the other half

The capture says what the controller does; `CMD:LOOPGAP` says whether anyone is
listening. It records the interval between successive `gpio.update()` calls --
the sampler itself, hooked at the six call sites in `loop()` rather than in
`HalGPIO::update()`, because `lib/hal` cannot include `src/` and the simulator
replaces that directory wholesale. Kept as a histogram plus the ten worst gaps,
since the interesting number is the tail: a render is rare against thousands of
fast iterations.

**It resets on read**, so a render can be isolated -- read, do the thing, read
again.

A first attempt hooked `MappedInputManager::update()` and reported zero samples
forever: in this firmware only two activities call it, and `loop()` calls
`gpio.update()` directly.

Both commands are gated on the same `ENABLE_TOUCHLOG_CMD`.

**Results are in "What the controller and the loop actually do".**

## Open, with the measurement that settles each

**Three of the five are answered.** 2026-09-14, X4 Pro, `CMD:TOUCHLOG` and
`CMD:LOOPGAP`; the numbers are in "What the controller and the loop actually do".

1. ~~Does the GT911 raise the buffer-ready flag periodically while a finger sits
   motionless, or only on change?~~ **Answered, and it is both.** A held
   *contact* produces a frame every 10 ms; a held *key* produces none at all.
   The SDK comment was right about the key and wrong about the glass, and the L1
   policy has to treat them as two sources rather than one.
2. ~~Does an uncleared frame hold or get overwritten on this panel?~~
   **Answered: it holds, and it blocks.** One frame sat in `0x814E` for 7.99 s
   while the key was tapped repeatedly and nothing else got through. A straddling
   tap does not appear as a stale down and does not vanish -- it becomes the
   *only* thing the controller will report until someone clears the register.
   This kills the drain-it-later workaround outright.
3. ~~Are the extra tap events bounce or stale frames?~~ **Answered as far as the
   key is concerned: neither.** A press is exactly one frame and a release is
   exactly three, every time, on the glass and on the key alike -- that burst is
   what this controller does, not contact bounce, so `minPressMs` buys nothing
   here. The 2026-09-05 "three gestures from one double tap" is explained by the
   latch plus the stale `touchHomeKeyDown`, with no bounce needed.
4. **Who holds the T5 S3 Pro's I2C mutex during a panel refresh?** Still open,
   and still decides whether a poll task is enough or the read has to be
   interrupt-driven. What is no longer open is whether the interrupt is
   available: the INT line is in the board profile on both boards, confirmed on
   hardware for the X4 Pro (`BoardConfig.h`, *"CONFIRMED ON HARDWARE: INT=GPIO10,
   RST=GPIO4"*), nothing in the SDK attaches to it, and every frame with bit 7
   set was seen with it asserted -- 868 for 868, no exceptions.
5. **The natural double-tap interval on this key**, with true timestamps. Partly
   filled in: free tapping gave key-press intervals of 885, 980, 595, 570, 240
   and 385 ms, and press-to-release is 55-90 ms. A *deliberate* double tap has
   still not been captured, and that is the number the window should be set from.

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
