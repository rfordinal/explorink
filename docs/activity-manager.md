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
into the framebuffer from `loop()`, on the main task. That was history, not a
decision -- the screen predates the centralized manager and was never migrated.

**What it cost.** A plain redraw blocked `loop()` for **2.80 s** and opening the
map for **4.34 s** (X4 Pro, measured 2026-09-14, `input-gestures.md`). Nothing
else in the firmware ran inside that window: no button edge, no GNSS drain, no
LoRa poll (T-2023 measured 12 of 20 packets lost across render windows), no
console line. The busy badge (`busy-feedback.md`) exists only because of it.

**What it is not.** Not parallelism. The render task is pinned to core 1
(`ActivityManager::begin()`) and the Arduino `loopTask` is on core 1 too
(`CONFIG_ARDUINO_RUNNING_CORE=1` in the prebuilt Arduino libs; the C3 has one
core anyway). Both tasks share a core, so the win is **preemption**: `loop()`
gets its slice every tick instead of waiting out the frame. A frame does not
compose any faster, and it loses the cycles `loop()` takes back.

### render*() asks, compose*() paints

The seam is a naming rule, and it is the whole design:

| | who may call it | what it does |
|---|---|---|
| `renderCurrent()`, `renderViewport()`, `renderWaiting()`, `renderLoadingTiles()`, `renderRouteOverview()` | anyone, any task | records the request, wakes the render task |
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
  frame being composed says the same thing or draws the same values, and the
  next fix moves the marker again a second later.
- **Refuse and let the caller re-render.** `swapChrome()`,
  `captureMenuBackdrop()`, `restoreMenuBackdrop()`. Reading the framebuffer
  mid-compose captures half a frame; writing to it tears one. Every caller
  already has a full-render fallback for "no snapshot".
- **Defer into `render()`.** The pin notice and the option popup, because both
  belong *on top* of the frame. Four call sites read
  `renderCurrent(); showPinNotice(...)`, an ordering that only worked while the
  render was synchronous.

### Presses that arrive mid-frame are held, not dropped

`loop()` now runs during a compose, which is the point -- and it means a press
can land while the frame is being drawn. The zoom rung, the marker rung and
`proj_` are all read throughout a compose, so applying a press immediately would
draw one frame out of two states. `serviceDeferredInput()` holds them and
applies them once the panel is idle:

- **Zoom and marker accumulate into one delta.** Three presses, one redraw.
- **Pans queue in order, four deep.** Each step projects through the frame the
  previous step drew, so they cannot be summed.

Nothing is dropped. That is the difference between this and ignoring input while
busy.

### What a hardware pass has to check

- **The render task's stack.** The map moved onto a task with an 8,192-byte
  stack, and the label pass alone wants ~3.8 KB (`MapLabels.h`). Every composed
  frame logs `stack free` at debug level; if that number gets close to zero the
  stack has to grow, and the cost is permanent RAM.
- **A press during a redraw is answered.** Same bench as the 2.80 s / 4.34 s
  numbers above.
- **The menu, the pin notice and the badge still land in the right order** --
  they are the three paths whose ordering the synchronous render used to
  guarantee for free.
- **Wall clock.** A frame should be slightly slower now, bounded by `loop()`'s
  own duty cycle (`delay(10)` at its tail, `POWER_TELEMETRY.onLoop()` reports
  it). Unmeasured.
- **Power.** `loop()` used to do nothing for 2.80 s and now runs ~280 iterations
  in that window. Expected small, unmeasured -- the bench meter settles it
  (parent `docs/usb-power-meter.md`).

### Known, and left open

A rung change *inside* a compose is deferred, but `composeViewport()` still
reads `zoomStep()` several times rather than snapshotting it once at the top.
Nothing can move it mid-frame today (that is what the deferral is for), so this
is a latent trap rather than a bug: the fix is a per-frame snapshot, and it is
the natural next step if any other writer of that rung appears.
