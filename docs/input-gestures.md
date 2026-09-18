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
presses, one `0x90` frame each, then silence for the whole 60-90 ms hold. A held
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

**The raw files are kept**, with a verdict per capture, in
[`measurements/2026-09-14-gt911-x4pro/`](measurements/2026-09-14-gt911-x4pro/).
Prose that quotes a number nobody can re-derive is folklore with a citation.

### The controller

| what | what it sends |
|---|---|
| nothing touching | no frames at all. Longest observed silence 880 ms, status `0x00`, INT high |
| a contact on the glass | a new frame every **10 ms**, median exactly 10,000 us over 146 intervals, and it does not stop while the finger rests |
| **the capacitive home key, held** | **one frame at the press, then nothing** until the release. Press to first release frame: 60, 65, 65, 65, 75, 85, 90 ms over seven presses |
| any release | exactly **three** `0x80` frames about 10 ms apart, key and glass alike |

`[measured]` **The key is a separate pad, not a screen region.** Its frames are
`0x90` -- ready plus the key bit, contact count **zero**. Contacts near the bottom
edge of the glass report `y` between 773 and 791 of 799 with no key bit, so the
pad sits below the digitizer and is easy to miss: two captures made while the
maintainer believed a finger was on the key contain contacts and no key bit at
all.

`[measured]` **INT is reliable per frame.** Across the two complete captures --
`cap5` and `cap9`, **384 frames** with bit 7 set -- not one appeared with INT
high. It is not exact per *sample*: INT also stays asserted between the frames of
a release burst and while a frame is pending, so 415 samples show INT low with no
ready bit. While a frame is pending and unacknowledged, INT holds low with a
single 5 ms release every 345 ms (28 of them in 8 s, unexplained).

**Corrected 2026-09-14, same day.** This first said "every one of 868 frames",
which counted `cap7` -- a capture this file's own rule marks lossy and forbids
concluding from. 384 is the honest number.

### The loop

`[measured]` `CMD:LOOPGAP` records the interval between successive
`gpio.update()` calls -- the sampler, not `loop()`.

**Measured 2026-09-14, before T-2024.** The map's compose then ran on the main
task, so a frame and a sampler gap were the same thing. Since T-2024 the compose
runs on the render task and the sampler keeps running through it -- the worst gaps
on the map are now the main task's own windowed refreshes (measured 656 to 756 ms
on the two S3 boards, `activity-manager.md`). The numbers below still describe the
panel's cost; they no longer describe the sampler's.

| screen | typical gap | worst single gap |
|---|---|---|
| Home, idle | ~15 ms (265 of 284 in the 10-20 ms bucket) | 78 ms |
| map, steady state | **~53 ms** (547 of 548 in the 50-100 ms bucket) | -- |
| map, plain `redraw` | ~53 ms | **2.80 s** |
| map, opening it (tiles + render + panel) | ~53 ms | **4.34 s** |

The firmware's own log agrees on the last one: `New max loop duration: 4292 ms`,
of which render 2,171 ms and panel wait 1,341 ms.

### The mechanism, end to end, and where it stops being measured

**Corrected 2026-09-14, same day, after the first version claimed more than the
data carries.** Steps 1-4 are measured. Step 5 was an assumption stated as fact,
and it is the step that decides the outcome.

1. `[measured]` The loop's last poll cleared `0x814E`, so the controller is free.
2. `[measured]` The map render blocks the sampler for 2.8 to 4.3 s. **True for
   the build measured, not for the tip: T-2024 moved the compose off the main
   task, and what blocks the sampler now is a windowed refresh, not a frame.**
3. `[measured]` The first edge in that window -- the first tap's `0x90` --
   latches. `cap12` is that capture: the key-press frame sat in the register
   unchanged for 402 samples, 2.01 s, with the finger long gone.
4. `[measured]` Everything behind the lock -- the release, the second tap, its
   release -- is **destroyed**. This is the hard finding: 7.99 s of an 8.00 s
   capture on one frame with repeated taps reaching nothing.
5. `[measured]` The loop returns, reads the surviving frame and clears it, and
   **the controller emits a fresh frame reflecting the current state 10 ms
   later** -- one frame period. `cap12`: cleared at 2,005,001 us, one sample of
   `0x00`, then `0x80` from 2,015,001 us onward.

So: **a gesture made during a render is destroyed down to one edge**, and that
edge is then completed by the controller's re-report. The firmware sees a press
and a release. **One tap** -- which is the reported bug.

**But the controller is not what decides the outcome. The gap after the resume
is.** The press edge sets `touchHomeKeyDownAt` and the hold timer runs above the
ready gate, so what happens next depends on when the next poll lands:

| the poll after the resume | outcome |
|---|---|
| within 700 ms -- the normal 15-53 ms case | the `0x80` is read as a release: **one tap** |
| after 700 ms, because the loop went straight into another render | the hold fires first: **a phantom long press**, and `touchHomeKeyLongFired` then eats the real release |

The second row is not hypothetical. The 2026-09-05 incident recorded further up
this file -- a double tap that locked the panel, opened the map, and lit the
frontlight *once the render had finished* -- is that row, with the 700 ms elapsed
inside the second render.

**Corrected twice in one day, and worth keeping as a lesson.** The first version
asserted step 5 as fact without measuring it. The second retracted it as unknown
and offered three outcomes. The measurement then confirmed the original answer
and showed that both remaining outcomes are real but selected by something else
entirely -- the sampler gap after the resume, not the controller.

`[measured]` **At rest the key is safe, and the margin is 3 ms.** An earlier
version of this section said the key was unreliable on the map screen with or
without a refresh, comparing a 53 ms sampler against a 60-90 ms press. That
comparison is the wrong one and the conclusion was wrong: **a latching controller
cannot lose a press edge**, however slowly it is polled, because the frame waits
in the register.

What can be lost is the **release**, and only when no poll happens between the
press frame and the release: the release is then generated while the press still
occupies the latch, and is discarded. So the criterion is *sampler gap versus
press duration*, not versus anything else. Measured: the map's steady gap tops out
at **56.99 ms** (`measurements/2026-09-14-gt911-x4pro/loopgap-map-open.txt`, the
worst-ten list) against a shortest observed press of **60 ms**. It holds, by
about 3 ms. A slower loop, or a shorter press, breaks it -- and the failure is
not a lost tap but a **phantom long press**, because a missed release leaves
`touchHomeKeyDown` latched and the wall-clock hold timer above the gate fires
`HOME_KEY_LONG_PRESS_MS` later.

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

## What was built, 2026-09-14

**Written the same pass, unverified on hardware at the time of writing.** The
section above says what the controller and the loop do; this says what was done
about it.

### The shape

A FreeRTOS task samples the GT911 every 10 ms and owns two things: the I2C half
(`gt911ReadFrame`, including the acknowledging write) and the home key's
recogniser. Nothing else moved.

The controller poll was split first, as a refactor with no behaviour change:
`gt911ReadFrame()` does every bus access and touches no gesture state,
`gt911ApplyFrame()` does no bus access and is the previous body. That was
possible only because `now` was already a parameter rather than a `millis()`
call inside, so **a frame applied late carries the time it was read**. The whole
design rests on that one property.

### Why the key moved down and the glass did not

The key is recognised in the task because a recogniser fed from the app's loop
measures the loop's latency instead of the finger -- which is what the old
`pumpHomeKey()` did, and why its window had crept to 500 ms.

The glass stays on the app's thread because it is consumed as a **live level**:
`isScreenTouchHeld()` drives the sliders, there is a 90 ms touch-down select, and
the hint-box hit test needs `UITheme`. A finished-gesture queue cannot answer
"where is the finger now". Contact frames are queued and applied unchanged on the
app thread.

An earlier version of this plan said the task would recognise *everything* the
GT911 produces. That was wrong for the reason above. A later version said the
task would hand over only the **latest** frame, which is worse: a glass tap made
entirely inside a render would be consumed by the task and never reach the app,
which is a regression against today. The handover is a coalesced queue of state
changes -- edges always, a resting contact at 50 ms -- so a 4.3 s render costs
tens of frames and not 430.

### Three rules that decide whether it fails safe

- **One gesture per `update()`, never a drain.** The key's events are one-shot
  bools cleared at the top of `update()`, so mapping a whole queue into them
  collapses it: two double taps would toggle the touch lock once instead of
  twice, and a tap plus a hold would fire in the same frame, breaking "a hold
  never also selects". One per call spreads a backlog over consecutive frames
  instead of destroying it.
- **A sampling gap over 100 ms cancels the gesture in flight.** After a stall the
  last seen level is not evidence. Believing it is exactly how a missed release
  becomes a phantom long press -- without this rule the 2026-09-05 frontlight
  bug moves into the task instead of dying.
- **A full frame queue suppresses the contact in flight.** A torn stream must not
  become a tap or a swipe the finger never made.

### What it costs, and who pays

About 5.6 kB at runtime -- a 3 kB task stack and a 64-entry frame queue -- and
only on a board whose GT911 answered the probe. The task refuses to start if
`beginAsync()` is running and vice versa: two threads in this class's unlocked
state is a data race on every field.

### The constants, retuned

The 500 ms double-tap window becomes **300 ms**, because 500 was compensating for
poll latency that no longer exists and would now fuse two deliberate single taps
-- free tapping on this key was measured at intervals down to 240 ms. The 500 ms
refractory window is **deleted** and replaced by a 40 ms lower bound on the
inter-tap interval, which is Android's `DOUBLE_TAP_MIN_TIME`: it rejects contact
bounce without swallowing a fast third tap. Measured here, there is no bounce on
this key to reject anyway -- a press is exactly one frame and a release exactly
three, every time.

### How it has to be verified

Not by one double tap. The failure is probabilistic -- the report was "I have to
try many times" -- so the test is statistical and adversarial:

1. **N >= 20 double taps during renders**, against today's failure rate.
2. **The false-positive direction**: two deliberate single taps 400-600 ms apart
   during a render must give two Confirms, not a lock.
3. **The phantom long press**: hold the key through a settings save, and tap
   during a render where the first poll after the resume is late.
4. **Glass regression**: a slider drag mid-render, the hint boxes, and a double
   tap on the steady-state map at 53 ms, not only on map-open.
5. **`TASKGAP` in `CMD:LOOPGAP`**: `LOOPGAP` must still show the same ~4 s loop
   stall -- that is the control, the loop is *supposed* to stall -- while the
   task's own worst gap stays near 10 ms. `cancels` is a failure count, not a
   success: it means a gesture was dropped rather than mistimed.
6. **X4 or X3 boot**: no task, no RAM delta, touch paths compiled out.

### What the hardware run showed, 2026-09-15

`[measured]` X4 Pro, serial `b8:1f:3f:d4:89:bc`. Renders were driven from the
host, one map `redraw` every 6 s, with the maintainer gesturing during each.

**The control holds and so does the claim.** Across the runs the loop stalled
exactly as before -- 20 gaps over 2 s in the first series, worst **6.50 s** in
the second -- while the sampling task's own worst interval was **11.0 ms**
against a 10 ms target, with `gaps_over_limit=0` in every run. The task is doing
what the loop could not.

| what | result |
|---|---|
| double tap during a render | **20 of 20** (before: "I have to try many times") |
| long press | works |
| false lock from two separate taps | **0** -- the 300 ms window is not too wide |
| phantom long press | **0** |
| `cancels` (gap rule fired) | **0** -- the fix worked directly, not via its safety net |
| `produced` vs `delivered` | **equal in every run**, `queue_drops=0` |

`produced == delivered` is the one that closes the original worry: **nothing is
lost between the recogniser and the app.** A run of 42 gestures (21 taps)
delivered 42, of which 7 were refused by the staleness rule -- so the taps that
"did not register" are a rule with a number, not a hole.

**Two things the run taught that were not on the list.**

`[measured]` The USB CDC link dropped and re-enumerated on a different
`/dev/ttyACM*` node mid-series, while the host was writing during a render. The
firmware did **not** reboot -- the counters kept accumulating across it, which is
how it was told apart from a crash. A host script must therefore not assume its
port survives a long run, and a vanished port is not evidence of a device fault.

`[open]` **A correct drop is indistinguishable from a broken key.** The
maintainer's verdict on using it was that it felt good, and that not knowing
whether the device heard you is the weak part -- there is no busy indicator at
all. Tracked as T-2018 in the parent repo. The staleness rule is right and
invisible, and on a panel that holds a stale image for a second while it works,
invisible is a design problem.

### The glass, checked by thumb

`[measured]` The contact path was the least certain part of the change -- frames
now arrive through a coalesced queue (edges always, a resting contact at 50 ms)
instead of being read from the register on each poll, and the 50 ms threshold was
a choice nobody had tested. Exercised on the X4 Pro: a brightness slider dragged
slowly and quickly, taps into a list, and the left-edge back swipe. The slider
tracks the finger smoothly, the swipe works, the taps land.

The counters agree over the same twelve minutes: **`frame_overflows=0`** and
`cancels=0` across 70,378 sampler ticks, so the queue never filled and the
coalescing never lost a frame. Worst task interval in that window was **17.0 ms**
-- worse than the 11.0 ms seen under renders, almost certainly from the flash
writes that a settings change makes, and still six times under the 100 ms cancel
threshold.

`[measured]` That last number is worth keeping for its own sake: **flash writes
do not stall this task the way the design feared.** The gap rule was written for
exactly that case, and across twelve minutes including settings work it never
came close to firing.

### Still unverified

**The gap rule itself is unmeasured, not verified.** It has never fired on
hardware -- `cancels=0` in every run -- so the branch that cancels an in-flight
gesture after a stall has executed exactly zero times outside a compiler. The
test that would exercise it is a settings save (a flash write) while the key is
held; deferred by the maintainer to a bug-fixing session, 2026-09-15. Read the
zero as "the condition did not arise", never as "the handler works".

Two other checks were on the list and are resolved differently. The phantom long
press was sought and not seen across every run. A boot on an X4 or X3 was
**dropped as low value** rather than skipped: `FREEINK_CAP_TOUCH` is 0 on those
boards, so `beginGt911Task()` compiles to an empty stub, `pumpHomeKey()` takes
the no-window branch that behaves exactly as before, and `wasHomeKeyTapped()` is
always false there as it always was. The `default` environment builds. There is
almost nothing left for a boot to find, and it would cost a flash on a board
another session may be using.

### What it does not fix

`getHeldTime()` still reports the first key of a chord, `ButtonNavigator` still
swallows one release edge after a cancelled hold, and the T5 S3 Pro's user button
still cannot long-press Confirm. The ADC button ladder is still sampled from
`loop()` and still loses edges across a render. Those are the rest of T-266.

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

### `delay<N>`, and why the capture arms itself

A third mode, added for open question 5: never acknowledge, until the frame has
sat unacknowledged for N ms, then acknowledge **exactly once and never again**.
What follows that single write is the whole measurement, so clearing a second
time would destroy it. `TOUCHLOG_CLEARED:<us>` marks the instant.

**And the capture waits for the finger, not the other way round.** The first
three attempts came back empty because the operator was being asked to tap inside
a window opened by a chat round trip, and that round trip is unpredictable -- 
sometimes tens of seconds. Being told to time a 5 ms-resolution experiment by
hand is not an instruction, it is a design defect. So a delay-mode capture arms
first: it reads without clearing and without recording until a frame appears, up
to 20 s, and only then starts the window and the delay clock. `TOUCHLOG_ARMED`
reports how long it waited and whether it timed out, so an empty capture can
never be mistaken for a quiet controller.

The arm phase polls with `delay(2)` rather than the recording loop's busy-wait:
20 s of spinning would starve the idle task and trip the watchdog, and cadence
does not matter while nothing is being recorded. The wait is also, conveniently,
the exact state under study -- a register left unacknowledged while nobody polls.

Both commands are gated on the same `ENABLE_TOUCHLOG_CMD`.

**Results are in "What the controller and the loop actually do".**

## Open, with the measurement that settles each

**Four of the six are answered.** Question 5 was opened and closed the same
day: it existed because the mechanism above rested on an assumption, and the
assumption turned out to be right. 2026-09-14, X4 Pro, `CMD:TOUCHLOG` and
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
   set was seen with it asserted -- 384 for 384 across the two complete
   captures, no exceptions.
5. ~~Does the controller re-report after a late acknowledgment?~~ **Answered
   the same day it was opened: yes, within one frame period.** `cap12`, the
   `delay2000` capture -- a key-press frame held 2.01 s, acknowledged once, and a
   fresh `0x80` 10 ms later. So a render-straddling gesture reaches the firmware
   as one tap, unless the poll after the resume is itself more than 700 ms away,
   in which case the hold timer fires first.
6. **The natural double-tap interval on this key**, with true timestamps. Partly
   filled in: free tapping gave key-press intervals of 885, 980, 595, 570, 240
   and 385 ms, and press-to-release is 60-90 ms over seven presses. A *deliberate* double tap has
   still not been captured, and that is the number the window should be set from.

## Considered and rejected: a double tap on the glass

**Decided 2026-09-14.** Written down because it is cheap to propose and expensive
to re-cost, and the next person to want it should start from the number rather
than from the idea.

A double tap on the glass cannot exist unless **every single tap waits** to find
out whether a second one is coming. Otherwise the first tap of a double tap also
fires as a single tap. That is the same trade the T5 S3 Pro's home key already
pays, and this file says so under "Whether the capacitive home key carries a
double tap" -- but on the glass it is not one key, it is the whole screen.

The cost, counted: **29 call sites** consume `wasScreenTapped()` or
`wasScreenTouchDown()` across the activities, and the **hint boxes** go through
`gpio.wasTouchTap()` in `MappedInputManager::pumpHintTouch()`. The hint boxes
stand in for hardware buttons, so making them wait would put a delay on what the
rider reads as a physical key. At Android's 300 ms that is 300 ms added to every
tap on the device.

Not worth it for a gesture nothing currently needs.

**What stays true anyway.** The recogniser is parameterised
`{longMs, doubleWindowMs, minInterTapMs}` and `doubleWindowMs = 0` means "fire
the tap on release, immediately", which is what the glass gets and what boards
without a key double tap already need. So the capability is one parameter away.
If a real use ever turns up, the work is not implementing it -- it is deciding
where the latency is acceptable, and that decision is the whole cost.

**What upstream thinks is unknown.** Neither `freeink-sdk` nor CrossPoint
implements a glass double tap. The latency argument above is ours, measured
against our own call sites; **no statement from either project has been read
saying they considered it**. Do not repeat this section as "upstream rejected it".

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
