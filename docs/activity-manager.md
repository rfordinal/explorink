# Activity & ActivityManager Migration Guide

This document explains the refactoring from the original per-activity render task model to the centralized `ActivityManager` introduced in [PR #1016](https://github.com/crosspoint-reader/crosspoint-reader/pull/1016). It covers the architectural differences, what changed for activity authors, and the FreeRTOS task and locking model that underpins the system.

## Overview of Changes

| Aspect | Old Model | New Model |
|--------|-----------|-----------|
| Render task | One per activity (8KB stack each) | Single shared task in `ActivityManager` |
| Render mutex | Per-activity `renderingMutex` | Single global mutex in `ActivityManager` |
| `RenderLock` | Inner class of `Activity` | Standalone class, acquires global mutex |
| Subactivities | `ActivityWithSubactivity` base class | Activity stack managed by `ActivityManager` |
| Navigation | Free functions in `main.cpp` | `activityManager.goHome()`, `goToReader()`, etc. |
| Subactivity results | Callback lambdas stored in parent | `startActivityForResult()` / `setResult()` / `finish()` |
| `requestUpdate()` | Notifies activity's own render task | Delegates to `ActivityManager` (immediate or deferred) |

## Architecture

### Old Model: Per-Activity Render Tasks

Each activity created its own FreeRTOS render task on entry and destroyed it on exit:

```text
┌─────────────────────────────────────────────────────────┐
│ Main Task (Arduino loop)                                │
│  ┌───────────────────────────────────────────────────┐  │
│  │ currentActivity->loop()                           │  │
│  │   ├── handle input                                │  │
│  │   ├── update state (under RenderLock)             │  │
│  │   └── requestUpdate()  ──notify──►  Render Task   │  │
│  │                                      (per-activity)│  │
│  │                                      8KB stack     │  │
│  │                                      owns mutex    │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│ ActivityWithSubactivity:                                │
│  ┌──────────────┐     ┌──────────────┐                  │
│  │ Parent        │────►│ SubActivity   │                 │
│  │ (has render   │     │ (has own      │                 │
│  │  task)        │     │  render task) │                 │
│  └──────────────┘     └──────────────┘                  │
└─────────────────────────────────────────────────────────┘
```

Problems with this approach:

- **8KB per render task**: Each activity allocated an 8KB FreeRTOS stack for its render task, even though only one renders at a time
- **Dangerous deletion patterns**: `exitActivity()` + `enterNewActivity()` in callbacks led to `delete this` situations where the caller was destroyed while its code was still on the stack
- **Subactivity coupling**: Parents stored callbacks to child results, creating tight coupling and lifetime hazards

### New Model: Centralized ActivityManager

A single `ActivityManager` owns the render task and manages an activity stack:

```text
┌──────────────────────────────────────────────────────────┐
│ Main Task (Arduino loop)                                 │
│                                                          │
│  activityManager.loop()                                  │
│    │                                                     │
│    ├── currentActivity->loop()                           │
│    │     ├── handle input                                │
│    │     ├── update state (under RenderLock)              │
│    │     └── requestUpdate()                             │
│    │                                                     │
│    ├── process pending actions (Push / Pop / Replace)    │
│    │                                                     │
│    └── if requestedUpdate: ──notify──► Render Task       │
│                                         (single, shared) │
│                                         8KB stack        │
│                                         global mutex     │
│                                                          │
│  Activity Stack:                                         │
│  ┌──────────┬──────────┬──────────┐    ┌──────────┐     │
│  │ Home     │ Settings │ Wifi     │    │ Keyboard │     │
│  │ (stack)  │ (stack)  │ (stack)  │    │ (current)│     │
│  └──────────┴──────────┴──────────┘    └──────────┘     │
│   stackActivities[]                    currentActivity   │
└──────────────────────────────────────────────────────────┘
```

## Migration Checklist

### 1. Change Base Class

If your activity extended `ActivityWithSubactivity`, change it to extend `Activity`:

```cpp
// BEFORE
class MyActivity final : public ActivityWithSubactivity {
  MyActivity(GfxRenderer& r, MappedInputManager& m, std::function<void()> goBack)
      : ActivityWithSubactivity("MyActivity", r, m), goBack(goBack) {}
};

// AFTER
class MyActivity final : public Activity {
  MyActivity(GfxRenderer& r, MappedInputManager& m)
      : Activity("MyActivity", r, m) {}
};
```

Note that navigation callbacks like `goBack` are no longer stored — use `finish()` or `activityManager.goHome()` instead.

### 2. Replace Navigation Functions

The free functions `exitActivity()` / `enterNewActivity()` in `main.cpp` are gone. Use `ActivityManager` methods:

```cpp
// BEFORE (in main.cpp or via stored callbacks)
exitActivity();
enterNewActivity(new SettingsActivity(renderer, mappedInput, onGoHome));

// AFTER (from any Activity method)
activityManager.goToSettings();
// or for arbitrary navigation:
activityManager.replaceActivity(std::make_unique<MyActivity>(renderer, mappedInput));
```

`replaceActivity()` destroys the current activity and clears the stack. Use it for top-level navigation (home, reader, settings, etc.).

### 3. Replace Subactivity Pattern

The `enterNewActivity()` / `exitActivity()` subactivity pattern is replaced by a stack with typed results:

```cpp
// BEFORE
void MyActivity::launchWifi() {
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
      [this](bool connected) { onWifiDone(connected); }));
}
// Child calls: onComplete(true); // triggers callback, which may call exitActivity()

// AFTER
void MyActivity::launchWifi() {
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        auto& wifi = std::get<WifiResult>(result.data);
        onWifiDone(wifi.connected);
      });
}
// Child calls:
//   setResult(WifiResult{.connected = true, .ssid = ssid});
//   finish();
```

Key differences:

- **`startActivityForResult()`** pushes the current activity onto the stack and launches the child
- **`setResult()`** stores a typed result on the child activity
- **`finish()`** signals the manager to pop the child, call the result handler, and resume the parent
- The parent is never deleted during this process — it's safely stored on the stack

### 4. Update `render()` Signature

The `RenderLock` type changed from `Activity::RenderLock` (inner class) to standalone `RenderLock`:

```cpp
// BEFORE
void render(Activity::RenderLock&&) override;

// AFTER
void render(RenderLock&&) override;
```

Include `RenderLock.h` if not transitively included via `Activity.h`.

### 5. Update `onEnter()` / `onExit()`

Activities no longer create or destroy render tasks:

```cpp
// BEFORE
void MyActivity::onEnter() {
  Activity::onEnter();       // created render task + logged
  // ... allocate resources
  requestUpdate();
}
void MyActivity::onExit() {
  // ... free resources
  Activity::onExit();        // acquired RenderLock, deleted render task
}

// AFTER
void MyActivity::onEnter() {
  Activity::onEnter();       // just logs
  // ... allocate resources
  requestUpdate();
}
void MyActivity::onExit() {
  // ... free resources
  Activity::onExit();        // just logs
}
```

The render task lifecycle is handled entirely by `ActivityManager::begin()`.

### 6. Update `requestUpdate()` Calls

The signature changed to accept an `immediate` flag:

```cpp
// BEFORE
void requestUpdate();  // always immediate notification to per-activity render task

// AFTER
void requestUpdate(bool immediate = false);
// immediate=false (default): deferred until end of current loop iteration
// immediate=true: sends notification to render task right away
```

**When to use `immediate`**: Almost never. Deferred updates are batched — if `loop()` triggers multiple state changes that each call `requestUpdate()`, only one render happens. Use `immediate` only when you need the render to start before the current function returns (e.g., before a blocking network call).

**`requestUpdateAndWait()`**: Blocks the calling task until the render completes. Use sparingly — it's designed for cases where you need the screen to reflect new state before proceeding (e.g., showing "Checking for update..." before calling a network API).

### 7. Remove Stored Navigation Callbacks

Old activities often stored `std::function` callbacks for navigation:

```cpp
// BEFORE
class SettingsActivity : public ActivityWithSubactivity {
  const std::function<void()> goBack;   // stored callback
  const std::function<void()> goHome;   // stored callback
public:
  SettingsActivity(GfxRenderer& r, MappedInputManager& m,
                   std::function<void()> goBack, std::function<void()> goHome)
      : ActivityWithSubactivity("Settings", r, m), goBack(goBack), goHome(goHome) {}
};

// AFTER
class SettingsActivity : public Activity {
public:
  SettingsActivity(GfxRenderer& r, MappedInputManager& m)
      : Activity("Settings", r, m) {}
  // Use finish() to go back, activityManager.goHome() to go home
};
```

This removes `std::function` overhead (~2-4KB per unique signature) and eliminates lifetime risks from captured `this` pointers.

## Technical Details

### FreeRTOS Task Model

The firmware runs on an ESP32-C3, a single-core RISC-V microcontroller. FreeRTOS provides cooperative and preemptive multitasking on this single core — only one task executes at any moment, and the scheduler switches between tasks at yield points (blocking calls, `vTaskDelay`, `taskYIELD`) or when a tick interrupt promotes a higher-priority task.

There are two tasks relevant to the activity system:

```text
┌──────────────────────┐     ┌──────────────────────────┐
│ Main Task            │     │ Render Task              │
│ (Arduino loop)       │     │ (ActivityManager-owned)   │
│ Priority: 1          │     │ Priority: 1              │
│                      │     │                          │
│ Runs:                │     │ Runs:                    │
│ - gpio.update()      │     │ - ulTaskNotifyTake()     │
│ - activity->loop()   │     │   (blocks until notified)│
│ - pending actions    │     │ - RenderLock (mutex)     │
│ - sleep/power mgmt   │     │ - activity->render()     │
│ - requestUpdate →────┼─────┼─► xTaskNotify()          │
│   (end of loop)      │     │                          │
└──────────────────────┘     └──────────────────────────┘
```

Both tasks run at priority 1. Since the ESP32-C3 is single-core, they alternate execution: the main task runs `loop()`, then at the end of the loop iteration, notifies the render task if an update was requested. The render task wakes, acquires the mutex, calls `render()`, releases the mutex, and blocks again.

The X4 Pro and the T5 S3 Pro are dual-core S3s, and the reasoning does not change on them: `ActivityManager::begin()` pins the render task to core 1 and `CONFIG_ARDUINO_RUNNING_CORE=1` puts `loopTask` on core 1 as well, so the two still share one core and still alternate. Nothing in this firmware runs a render in parallel with `loop()`.

Do not use `xTaskCreate` inside activities. If you have a use case that seems to require a background task, open a discussion to propose a lifecycle-aware `Worker` abstraction first.

### The Render Mutex and RenderLock

A single FreeRTOS mutex (`renderingMutex`) protects shared state between `loop()` and `render()`. Since these run on different tasks, any state read by `render()` and written by `loop()` must be guarded.

`RenderLock` is an RAII wrapper:

```cpp
// Standalone class (not tied to any specific activity)
class RenderLock {
  bool isLocked = false;
public:
  explicit RenderLock();           // acquires activityManager.renderingMutex
  explicit RenderLock(Activity&);  // same — Activity& param kept for compatibility
  ~RenderLock();                   // releases mutex if still held
  void unlock();                   // early release
};
```

**Usage patterns:**

```cpp
// In loop(): protect state mutations that render() reads
void MyActivity::loop() {
  if (somethingChanged) {
    RenderLock lock;
    state = newState;        // safe — render() can't run while lock is held
  }
  requestUpdate();           // trigger render after lock is released
}

// In render(): lock is passed in, held for duration of render
void MyActivity::render(RenderLock&&) {
  // Lock is held — safe to read shared state
  renderer.clearScreen();
  renderer.drawText(..., stateString, ...);
  renderer.displayBuffer();
  // Lock released when RenderLock destructor runs
}
```

**Critical rule**: Never call `requestUpdateAndWait()` while holding a `RenderLock`. The render task needs the mutex to call `render()`, so holding it while waiting for the render to complete is a deadlock:

```text
Main Task                    Render Task
──────────                   ───────────
RenderLock lock;             (blocked on mutex)
requestUpdateAndWait();
  → notify render task
  → block waiting for
    render to complete        → wakes up
                              → tries to acquire mutex
                              → DEADLOCK: main holds mutex,
                                waits for render; render
                                waits for mutex
```

### requestUpdate() vs requestUpdateAndWait()

```text
requestUpdate(false)          requestUpdate(true)
─────────────────             ─────────────────
Sets flag only.               Notifies render task
Render happens after          immediately.
loop() returns and            Render may start
ActivityManager checks        before the calling
the flag.                     function returns.
                              (Does NOT wait for
                              render to complete.)

requestUpdateAndWait()
──────────────────────
Notifies render task AND
blocks calling task until
render is done. Uses
FreeRTOS direct-to-task
notification on the
caller's task handle.
```

`requestUpdateAndWait()` flow in detail:

```text
Calling Task                 Render Task
────────────                 ───────────
requestUpdateAndWait()
  ├─ assert: not render task
  ├─ assert: not holding RenderLock
  ├─ store waitingTaskHandle
  ├─ xTaskNotify(renderTask)  → wakes render task
  └─ ulTaskNotifyTake() ─┐
     (blocked)           │    RenderLock lock;
                         │    activity->render();
                         │    // render complete
                         │    taskENTER_CRITICAL
                         │    waiter = waitingTaskHandle
                         │    waitingTaskHandle = nullptr
                         │    taskEXIT_CRITICAL
                         │    xTaskNotify(waiter) ───┐
                         │                           │
  ┌──────────────────────┘                           │
  │ (woken by notification) ◄────────────────────────┘
  └─ return
```

### Activity Lifecycle Under ActivityManager

```text
activityManager.replaceActivity(make_unique<MyActivity>(...))
  │
  ▼
╔═══════════════════════════════════════════════════╗
║  pendingAction = Replace                          ║
║  pendingActivity = MyActivity                     ║
╚═══════════════════════════════════════════════════╝
  │
  ▼ (next loop iteration)
  ActivityManager::loop()
  │
  ├── currentActivity->loop()     // old activity's last loop
  │
  ├── process pending action:
  │   ├── RenderLock lock;
  │   ├── oldActivity->onExit()   // cleanup under lock
  │   ├── delete oldActivity
  │   ├── clear stack
  │   ├── currentActivity = MyActivity
  │   ├── lock.unlock()
  │   └── MyActivity->onEnter()   // init new activity
  │
  └── if requestedUpdate:
      └── notify render task
```

For push/pop (subactivity) navigation:

```text
Parent calls: startActivityForResult(make_unique<Child>(...), handler)
  │
  ▼
╔══════════════════════════════════════╗
║  pendingAction = Push               ║
║  pendingActivity = Child            ║
║  parent->resultHandler = handler    ║
╚══════════════════════════════════════╝
  │
  ▼ (next loop iteration)
  ├── Parent moved to stackActivities[]
  ├── currentActivity = Child
  └── Child->onEnter()

        ... child runs ...

Child calls: setResult(MyResult{...}); finish();
  │
  ▼
╔══════════════════════════════════════╗
║  pendingAction = Pop                ║
║  child->result = MyResult{...}      ║
╚══════════════════════════════════════╝
  │
  ▼ (next loop iteration)
  ├── result = child->result
  ├── Child->onExit(); delete Child
  ├── currentActivity = Parent (popped from stack)
  ├── Parent->resultHandler(result)
  └── requestUpdate()   // automatic re-render for parent
```

### Common Pitfalls

**Calling `finish()` and continuing to access `this`**: `finish()` sets `pendingAction = Pop` but does not immediately destroy the activity. The activity is destroyed on the next `ActivityManager::loop()` iteration. It's safe to access member variables after `finish()` within the same function, but don't rely on the activity surviving past the current `loop()` call.

**Modifying shared state without `RenderLock`**: If `render()` reads a variable and `loop()` writes it, the write must be under a `RenderLock`. Without it, `render()` could see a half-written value (e.g., a partially updated string or struct).

The map screen departs from this rule deliberately, and the departure is the whole subject of the T-2024 section below: it gates its own writes on `frameInFlight()` instead of taking a `RenderLock` for them, because a `RenderLock` in `loop()` would wait out a frame that takes seconds, which is the stall the change exists to remove. That trade is only available to a writer that can afford to postpone itself. Anything that cannot -- a write whose result must be visible now -- still takes the lock, and the map's pin-store writes do.

**Creating background tasks that outlive the activity**: Any FreeRTOS task created in `onEnter()` must be deleted in `onExit()` before the activity is destroyed. The `ActivityManager` does not track or clean up background tasks.

**Holding `RenderLock` across blocking calls**: The render task is blocked on the mutex while you hold the lock. Keep critical sections short — acquire, mutate state, release, then do blocking work.

```cpp
// WRONG — blocks render for the entire network call
void MyActivity::doNetworkStuff() {
  RenderLock lock;
  state = LOADING;
  auto result = http.get(url);  // blocks for seconds with lock held
  state = DONE;
}

// CORRECT — release lock before blocking
void MyActivity::doNetworkStuff() {
  {
    RenderLock lock;
    state = LOADING;
  }
  requestUpdate(true);           // render "Loading..." immediately, before we block
  auto result = http.get(url);   // lock is not held
  {
    RenderLock lock;
    state = DONE;
  }
  requestUpdate();
}
```

## The map screen: how it joined the model (T-2024)

`MapActivity` was the last activity outside this model. It had no
`render(RenderLock&&)` override at all: about thirty call sites drew straight
into the framebuffer from `loop()`, on the main task. That is history rather
than a decision -- the screen predates the centralized manager and was never
migrated.

**What it cost.** A plain redraw blocked the input sampler (`gpio.update()`) for
**2.80 s** and opening the map for **4.34 s** (X4 Pro, measured 2026-09-14 with
`CMD:LOOPGAP`, `input-gestures.md` -- that instrument times the gap between
sampler calls, which is what a rider feels). Nothing else in the firmware ran
inside that window: no button edge, no GNSS drain, no LoRa poll, no console
line. The busy badge (`busy-feedback.md`) answers the same wait and stays, since
a frame is no faster now.

**What it is not.** Not parallelism. The render task and the Arduino `loopTask`
share one core on every board here. On the S3 the render task is pinned to core
1 (`ActivityManager::begin()`) and `CONFIG_ARDUINO_RUNNING_CORE=1` puts
`loopTask` there too; on the C3 that setting is `0`, the pinning is compiled out
(`#if configNUM_CORES > 1`), and there is one core anyway. (Read from
`~/.platformio/packages/framework-arduinoespressif32-libs/{esp32s3,esp32c3}/sdkconfig`
on 2026-09-18; that directory is rewritten by whichever build ran last, so the
claim carries its date.) The win is **preemption**: `loop()` gets its slice every
tick instead of waiting out the frame. A frame does not compose faster, and it
loses the cycles `loop()` takes back.

### The invariant the whole design rests on

**Every frame request comes from the task that runs `loop()`.**

Twenty-one places on this screen check `frameInFlight()` and then paint. That is
a check-then-paint, and it is race-free only because a compose cannot *start*
between the check and the paint: a request sets a deferred flag that
`ActivityManager::loop()` turns into a task notification at the tail of the
iteration, after `MapActivity::loop()` has returned. One `requestUpdate(true)`
from a BLE, web or GNSS callback would turn all twenty-one into silent TOCTOU
races.

So `requestFrame()` checks the calling task against the one that ran `onEnter()`,
logs at `ERR` and asserts. The assert is compiled out of a release build; the log
line is not.

### render*() asks, compose*() paints

The seam is a naming rule, and it is the whole design:

| | who may call it | what it does |
|---|---|---|
| `renderCurrent()`, `renderViewport()`, `renderWaiting()`, `renderLoadingTiles()`, `renderRouteOverview()` | the main task only | records the request, wakes the render task |
| `composeCurrent()`, `composeViewport()`, ... | `render(RenderLock&&)` only | paints the frame |

The request is one small struct behind a `portMUX` spinlock, not a `RenderLock`:
asking for a frame must never block, and a `RenderLock` there would make every
requester wait out the frame already being composed -- which is the stall this
change removes. **Last request wins**, so three quick presses coalesce into the
one frame the rider is waiting for.

A `compose*()` that needs a different frame (no tile source, a route fit that
read short) calls its `compose*()` sibling directly. It is already inside the
render; requesting would bounce the work to the next tick.

### What still runs on the main task, and how it stays safe

Small paints stay where they were, because routing a 40x40 marker patch through
a task switch buys nothing: the busy badge, the marker move, the chrome swap,
the menu backdrop, the pin notice, the option popup. Each one asks
`frameInFlight()` first -- `pendingFrame_` for a frame that is queued, and
`RenderLock::peek()` for one already being composed, the same non-blocking test
`EpubReaderActivity` uses for its background build. `pendingFrame_` alone cannot
answer the second question, because `render()` clears it before composing.

Three different answers, depending on what the paint is for:

- **Skip it.** The badge, the marker move, the header and debug repaints. The
  frame being composed says the same thing or draws the same values.
- **Refuse and let the caller re-render.** `swapChrome()`,
  `captureMenuBackdrop()`, `restoreMenuBackdrop()`. Reading the framebuffer
  mid-compose captures half a frame; writing to it tears one. Every caller has a
  full-render fallback -- including the touch dismiss, which used to treat a held
  backdrop as proof that the map was underneath. A refusal made that false, and
  for a while a menu dismissed by a tap outside stayed on the glass.
- **Paint on the next main-task tick, from `servicePendingPaints()`.** The pin
  notice and the option popup, because both belong *on top* of the frame. Five
  call sites read `renderCurrent(); showPinNotice(...)`, an ordering that only
  worked while the render was synchronous. Never from `render()`: `OptionPopup`
  rebuilds a layout cache and owns vectors that `handleInput()` can replace, and
  the notice's patch is a `unique_ptr` the main task also resets. Painting either
  from the render task is a cross-task write to a container, which is a heap bug
  rather than a torn pixel.

  **Where that drain sits is load-bearing, and the first attempt got it wrong.**
  It lived in `serviceDeferredInput()`, near the bottom of `loop()` -- below the
  popup block, which returns early for as long as a menu is open. So a CONFIRM
  during a compose stashed the repaint, the menu became active, and every tick
  from then on returned before the drain: the frame landed and **the menu never
  appeared**, until some unrelated press made the popup repaint itself.
  Reproduced twice by hand on an X4 Pro, 2026-09-17. `servicePendingPaints()` is
  called above the popup block.

### One task per piece of state, for everything with a size

The rule the first cut of this change got wrong: **moving the frame to another
task moves everything the frame touches.** Four things the compose used to do
came back to the main task afterwards.

- **The missing-tiles store.** `drawMapLayers()` called `MISSING_TILES.record()`
  per hatched tile. That store is a bare `std::vector` with no lock
  (`MissingTilesStore.h`), and `loop()` erases from it, walks it and serialises
  it to the card -- on ticks that now run *during* a compose. A `push_back` that
  reallocates under the main task's iterator is heap corruption discovered hours
  later, nowhere near its cause. The frame now **collects** what it hatched into
  a fixed array and `recordHatchedTiles()` records from `loop()`.
- **The held-tiles store**, `g_heldTiles`, for the same reason and with the same
  shape: a fixed array whose `record()` rewrites entries in place, read by
  `maybeCheckTileFreshness()` on ordinary ticks. The first fix pass applied the
  rule to one store and missed its sibling.
- **The BLE reply that carries the viewport diagonal.**
  `sendViewportDiagonalIfChanged()` indicates over BLE and then waits up to
  **3 s** for the phone's confirm (`BlePositionServer::sendCommandChunk`). Called
  from the compose, that wait happens with the `RenderLock` held: every partial
  paint refused, every held press stalled, and a `Back` press blocking
  `ActivityManager` itself. The frame sets a flag; `loop()` sends.
- **An arriving fix.** Not only because `applyFix()` projects through `proj_`,
  which the compose rewrites, but because the BLE and GNSS branches write
  `trust_`, the altitude and `lastDrawnSeq_` *before* `applyFix()` decides
  anything, and the compose reads all three. Holding inside `applyFix()` was not
  enough -- a live marker style could be drawn on the previous position. Both
  branches are gated on `frameInFlight()` instead; `getLatest()` does not
  consume, so the packet is still there on the next idle tick.

The pin-store writes the menu makes still take a `RenderLock` and wait out the
frame -- rare, rider-initiated, and a pin must not be lost.

**What this rule does not cover.** `overviewShown_`, `busyShown_`,
`headerRowDrawn_` and `markerPatchValid_` are still written from both tasks.
They are single bytes and every main-task reader sits behind `frameInFlight()`,
so they are safe for that reason and not because they have one owner. The
compose also reads live main-task state unlocked -- `zoomStep()`, `mode_`,
`screenMode_`, `SETTINGS.*`, the pin store -- so a menu row or a fix that lands
mid-frame can produce one frame that is half one state and half another, always
followed by a correct one. The design that closes that class by construction is
a per-frame immutable input snapshot, and it is the natural follow-up rather
than part of this change.

### Presses that arrive mid-frame are held

`loop()` now runs during a compose, which is the point -- and it means a press
can land while the frame is being drawn. The zoom rung, the marker rung and
`proj_` are all read throughout a compose, so applying a press immediately would
draw one frame out of two states. `serviceDeferredInput()` holds them and
applies them once the panel is idle:

- **Zoom and marker accumulate into one delta**, and the drain applies it **one
  rung at a time**. Both ladders refuse an out-of-range result outright instead
  of clamping it, so feeding back an accumulated `-3` from rung 2 would have
  moved nothing at all and swallowed three presses. The settle timer still
  collapses them into one redraw.
- **Pans queue in order, four deep.** Each step projects through the frame the
  previous step drew, so they cannot be summed.

**Only two things drop a press, and both log it at `INF`**: a pan queue already
four deep, and a context the rider has left -- a held ladder press once the route
overview is up, a held pan once Observe is over. A queued pan applied after the
rider returned to Follow would anchor the frame 30 % off the rider and leave a
marker claiming to be where they are not.

### What the console pays

`serial_.poll()` and `ble_.poll()` are skipped while a frame is in flight, so a
console command waits up to one frame. Deliberate: `pin set` rewrites the store
`drawPins()` is walking and a command cannot be deferred the way a button can,
because it is already parsed and its reply is owed. Locking inside
`MapCommandConsole` is not an option -- host tests compile that file and it must
stay free of firmware-only headers. So of the old block's four casualties, the
console is the one this change does not give back.

### Why the frame's wall clock stays bounded

`main.cpp` drops the CPU clock after a few seconds of inactivity, but
`setPowerSaving()` is a no-op while a power lock is held, and the render task
holds `HalPowerManager::Lock` for the whole of `render()`
(`ActivityManager::renderTaskLoop()`). A compose therefore always runs at full
clock, however long the rider has been staring at the screen.

### What the simulator proves, and how to re-run it

Host-side, with the map seeded from a persisted fix and the local CDN mirror
symlinked under `fs_/` (`simulator.md`). Re-run on the tip after every fix round,
most recently on `582017f5`, against a baseline worktree checked out at the same
`develop` commit the branch has merged:

```bash
echo '{"mapHasLastFix": true, "mapLastLatE7": 483770000,
       "mapLastLonE7": 175880000, "mapLastHeading": 0}' > fs_/.crosspoint/settings.json
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software \
  CROSSPOINT_SIM_INPUT_SCRIPT='1500:ENTER;4000:UP;5000:UP;12000:QUIT' \
  CROSSPOINT_SIM_SCREENSHOTS='10000:./qa-artifacts/zoomin.bmp' \
  ./.pio/build/simulator/program
```

Six states, one run each, reseeded every time because the simulator writes
settings back: map entry, two zoom steps, a marker step, the menu open, the menu
closed again, a menu row activated. **0 of 384,000 pixels differ from `develop`
in all six.** The frame after a Back is byte-identical to the frame before the
menu opened, so the backdrop capture and restore survive their `frameInFlight()`
refusals.

**And `loop()` really does run during a compose.** The host composes in ~20 ms,
so nothing lands mid-frame there naturally; a throwaway build with `delay(2500)`
at the top of `composeViewport()` makes it panel-slow. Two zoom presses inside
that window are both seen by `loop()`, both held, and applied one rung at a time
afterwards -- one redraw, not two. The same trick reproduced the invisible-menu
bug and confirmed its fix.

What the simulator cannot show: the panel's own refresh, the real compose cost,
the render task's stack (the fork stubs the high-water mark at a flat 2,048
bytes), the console paths (parent `docs/TODO.md` T-155, the shim is unreachable
on `develop` too), or anything about power.

### What the hardware passes measured

X4 Pro, 2026-09-17, three passes. Every number below is a `LOG_DBG` line read off
a serial capture taken with `scripts/debugging_monitor.py`; the captures
themselves were not archived.

| pass | build | what changed since | what it showed |
|---|---|---|---|
| 1 | `1c606edb` | first hardware run of the branch | 13 frames on the render task, lowest `stack free` **4,548 of 8,192 bytes**; frames 0.6-2.2 s while the worst loop iteration stayed 656 ms; a zoom press held 1.6 s and applied 5 ms after the frame; a 329 kB tile fetched over BLE through it all |
| 2 | `a46734d4` | the nine review fixes, plus a merge of `develop` | **a menu asked for during a compose never appeared** until an unrelated press repainted it |
| 3 | `69cb49fd` | the menu fix | the menu appears by itself the moment the frame lands; a pin notice lands on top of the frame; a menu opened on an idle panel still takes its backdrop (14,280 bytes) and closes instantly; a zoom press held through a 1.8 s compose applied 4 ms after it; lowest `stack free` **4,640 of 8,192 bytes** |

A fourth pass, build `fe46e7e8`, added the second audit's fixes and the menu
settle: the map, the menu during a compose, the tap-outside dismiss and a pin
notice all behaved, and `requestFrame()`'s new off-the-loop-task check stayed
quiet.

**The T5 S3 Pro has now run it too** (board B, same build, `t5s3pro-b-map-render-task-fe46e7e8-good`):
lowest `stack free` **4,584 of 8,192 bytes**, a compose of 1,983 ms against a
worst loop iteration of 756 ms, the menu backdrop taken normally (19,100 bytes,
382x388, 122 kB free heap), and the tap-outside dismiss repainting as designed.
Two taps of Up moved two rows there as well.

The 656 ms worst iteration is `onEnter()`'s synchronous loading frame, painted on
the main task on purpose so it reaches the panel before the expensive one starts.
A 3,596 ms spike in pass 1 belongs to a BLE reply timeout on the main task -- the
same 3 s wait that, found inside the compose, became the worst defect of the
second audit.

**Read pass 1 carefully: it passed, and nine defects were still there.** Every one
needs two tasks to interleave inside a few milliseconds, and a session of
hand-testing does not open those windows. Code review is not a substitute for the
device here, and the device is not a substitute for review.

Two caveats on the numbers. The stack figures are per-session minima over
different frame sets -- 4,640 against 4,548 is not growth, and neither covers the
deepest paths (point layer on, Hike, a route loaded, the debug overlay), where
`Cluster clusters[40]` alone is 640 bytes. And the tile transfer was measured
before `ble_.poll()` was gated; it has not been repeated since.

### The menu's own repaint, which async rendering did not fix

Reported from the device 2026-09-18, and worth separating from everything above:
**two quick taps of Up in the map menu moved the selection once.** Nothing to do
with the compose. `OptionPopup::processRender()` ends in a whole-panel
`displayBuffer()` -- the better part of a second on an X4 Pro, about twice that on
the T5 S3 Pro -- and it runs on the main task, so `gpio.update()` does not run
inside it and a press landing in that window is never sampled at all.

Handling the input is free; only the picture is expensive. So `paintPopup()` now
arms a 150 ms settle and `servicePendingPaints()` draws the settled result: a
burst moves the selection several times and costs one refresh. Every other screen
gets this for nothing by passing `requestUpdate()` as the popup's redraw callback
(`SettingsActivity` and friends), which `ActivityManager` coalesces once per
loop; the map paints its own popup and needed its own settle.

**Two taps land, three usually do not**, and the arithmetic says why: the third
arrives 200-250 ms after the first, by which time the settle has expired and the
refresh has started. Removing that properly means the refresh not blocking input
sampling -- a driver or input-task question, not a map one. Confirmed on both
touch boards with the settle at 150 ms.

### Reviewed twice, and what the reviews found

Eight independent read-only reviews ran over the finished branch before any merge
was proposed: four on races, ordering, adversarial sequences and lifecycle, then
four more on concurrency, design and the docs. Between them they found the
missing-tiles heap bug, the BLE indication inside the compose, the held-tiles
sibling, the fix branches' ungated side effects, the route overview lost to a
fix, a menu backdrop kept after a refused capture, presses swallowed at the
ladder ends, the menu's zoom row composing the old rung, deferred input draining
into a context the rider had left, the notice and popup painted from the render
task, a dismissed menu left on the glass, and an unstated invariant carrying
twenty-one gates. All are fixed above; the two design follow-ups are named in
"left open".

### Known, and left open

- **Power.** `loop()` used to do nothing for the length of a frame and now runs
  its ordinary ticks there. Bounded by the loop's own duty cycle, but unmeasured
  -- the bench meter settles it (parent `docs/usb-power-meter.md`) with the same
  redraw sequence either side, both in the same hour.
- **No C3 board has run this branch at all.** The X3 left for field testing on
  2026-09-17. The C3 is the tightest RAM target in the line and the one board
  where the render task is not pinned at all. Both S3 boards have run it.
- **The menu still drops the third tap of a burst**, because the refresh blocks
  the input sampler. The settle is 150 ms; raising it trades single-press latency
  for burst length, and the real fix is elsewhere (see the menu section above).
- **A frame can still be half one state and half another** when a menu row or a
  fix lands mid-compose. Self-healing, one frame; closed properly only by the
  per-frame input snapshot above.
- **`composeViewport()` reads `zoomStep()` several times** rather than
  snapshotting it once. Nothing can move the rung mid-frame today -- that is what
  the deferral is for -- so this is a latent trap rather than a bug.
- **A popped activity does not repaint the map.** `ActivityManager`'s Pop path
  relies on `requestUpdate()`, and this screen paints nothing when no frame kind
  is pending. Nothing pushes an activity over the map today.
- **The transfer path has not been re-measured** on the tip (see the caveat
  above), and the fifth queued pan is dropped by design.

**How this compares to the usual arrangement.** LVGL runs its handler in one task
and requires everyone else to take `lvgl_port_lock()` before touching a widget;
LVGL 9's parallel draw units consume an already-built, immutable draw-task list.
The common shape is: the renderer consumes a snapshot, so state can change freely
while the frame is drawn. This screen does the inversion -- the main task owns the
state, the render task reads it live, and the owner defers its own writes while a
frame is in flight. It works, and it matches the rest of this firmware, but it is
unusual enough that the invariant carrying it had to be written down.
