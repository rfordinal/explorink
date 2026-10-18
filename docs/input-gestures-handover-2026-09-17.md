# T-266 handover: the premise changed under the branch

Written 2026-09-17, at the end of the session that built the GT911 sampling
task, for whoever does the simplification. [`input-gestures.md`](input-gestures.md)
remains the topic doc; this is the state of one unfinished branch and why it
should shrink.

**Read this before deciding anything about `t266-gt911-task`.**

## What changed

The branch was built on a measured premise: `loop()` stops sampling for **2.80 s**
on a map redraw and **4.34 s** opening the map, and the GT911 holds exactly one
unacknowledged frame and discards everything behind it -- so every edge of a
gesture made during a render but the first was destroyed.

**T-2024 moved the map compose to the render task, and `loop()` no longer waits
for the panel** ([`busy-feedback.md`](busy-feedback.md)). Maintainer, on the
async build and without any of this branch: two taps during a redraw already get
through, and a tap during a redraw registers.

So the stall that justified a 10 ms sampling task is gone. **What is still broken
is double-tap and long-press recognition**, and that is a different half of this
branch.

## The branch splits cleanly, and only one half is still needed

| half | what it is | verdict |
|---|---|---|
| **the task** | `beginGt911Task()`, the 10 ms sampler, the frame queue, coalescing, overflow handling, the `beginAsync` guard, the simulator stubs | **probably delete** -- it exists to survive a stall that no longer happens |
| **the recogniser** | `gt911RecogniseKey()`, the `{longMs, doubleWindowMs, minInterTapMs}` spec, gestures with timestamps, one-per-`update()` delivery, the gap rule, the staleness rule, the per-type counters | **keep** -- it fixes why double tap and hold fail, which a fast loop does not |

The recogniser fixes four things that a fast loop does not touch:

1. Gestures were recognised **twice**, in the SDK and again above it in
   `pumpHomeKey()`, with one taking the other's output as its input.
2. A 500 ms refractory window after a resolved gesture swallowed the next tap.
3. The hold fired from a latched down-state that survived a missed release, so
   it arrived attached to a gesture other than the one that made it.
4. Nothing carried a timestamp, so nothing could be timed from the finger.

Running the recogniser from `pollGt911()` in the loop keeps all four fixes. At a
15-53 ms sampling interval the timestamps are accurate enough for a 500 ms window
and a 700 ms hold.

**Deleting the task removes almost every review finding**, because they were all
in the task: the frame queue and its sizing, the coalescing regression, the
`beginAsync` conflict, the input-style early returns, overflow, the torn
contact, ~8 kB of RAM, and the simulator stubs. What remains is small and far
easier to argue upstream.

## Before deleting the task: one measurement

Run `CMD:LOOPGAP` on an async-`develop` build and look at the tail. If the worst
loop gap is now well under 100 ms, the task has no case. If seconds still appear
occasionally -- a flash write, an SD read, something else -- it does, and
deleting it would be a mistake. **One run settles it and nobody has done it.**

## What is on the branch

`t266-gt911-task` in three repos, none merged.

Firmware (8 commits, `b5beebb9`..`9af667ec`), SDK (6, `5e4b9b5`..`a597d80`),
simulator (1, `1ea9215`). The firmware branch already carries a merge of
`origin/develop` including T-2024, and builds clean on `x4pro`, `t5s3pro`,
`default` and `simulator`.

`platformio.local.ini` in that worktree points `[simdep]` at the local simulator
worktree. It is gitignored and must not be committed.

## What ran on hardware and what did not

`[measured]` X4 Pro, with the task, renders driven from the host:

- 20 of 20 double taps during renders, against "I have to try many times"
- long press intact; zero false locks from two separate taps; zero phantom holds
- `produced == delivered`, `queue_drops = 0` in every run
- worst loop stall 6.50 s against a worst task interval of 11.0 ms
- glass by thumb -- slider drag, edge swipe, list taps -- with
  `frame_overflows = 0` across 70,378 ticks

`[measured]` The A/B that set the window, same map and cadence, 20 double taps
during redraws each way:

| build | locked | spurious menus |
|---|---|---|
| pre-change, 500 ms window, no task | 15/20 | **0** |
| this branch, 300 ms window | -- | often |

Narrowing to 300 ms was wrong: 500 ms was compensating for poll latency **and**
for the finger, because during a render the rider has no feedback and the two
presses drift to 350-450 ms apart. On the map a single tap from this key is
Confirm, so the miss opens a menu nobody asked for. The number is back at 500 ms
with that measurement in the comment.

**Not verified:** the 500 ms build has never run on hardware. Neither has the
double-tap/hold suppression fix, the fast-flick fix, or the glass staleness rule.
The gap rule has never fired at all (`cancels = 0` everywhere), so its branch is
**unmeasured, not verified**.

## What the two reviews found, and where each finding lives

Both reviews were adversarial and both changed the outcome. Findings that belong
to the task die with it; the rest must survive the simplification.

**Dies with the task:** frame queue depth against the 6.50 s worst stall;
drop-newest overflow leaving a contact latched; coalescing turning a sub-50 ms
flick into a tap at the landing point; `beginAsync` coexistence; unsynchronised
`homeKeySpec` across the thread boundary; `xTaskCreate` unchecked; no stop API;
`CMD:TOUCHLOG` racing the task for the register it measures; the I2C safety
resting on the Arduino core's `TwoWire` lock.

**Survives, and must be kept:**

- **One gesture per `update()`, never a drain.** The key's events are one-shot
  bools cleared at the top of `update()`, so mapping a queue into them collapses
  it -- two double taps would toggle the lock once, a tap plus a hold would fire
  together.
- **The pop must not sit behind the input-style early returns.** `update()`
  returns early for three styles and Sticky is a GT911 board using one.
- **A double tap's second press must be marked spent**, or the hold timer stays
  armed on it and tap-then-hold locks the panel *and* lights the frontlight.
- **Meaning belongs in the app.** `TouchPolicy` must not move into the SDK; the
  library recognises mechanics, the app decides what a gesture means and what
  expires.
- **A tap expires, a mode toggle does not.** A tap is aimed at something and the
  target moves; a lock is still what was asked for however late. Glass taps got
  the same rule, tested at the release so a finger still down is left alone.

## Three process lessons, all paid for

- **The hardware-tested code sat uncommitted** while the results were written up.
  A review caught it. Commit before the flash, not after the numbers.
- **"Not a regression" was asserted without measuring the old behaviour.** It
  was a regression, and the A/B that proved it took one archived binary and ten
  minutes. The archive is what made it possible.
- **Truncating grep output produced a false claim in the tracker** -- T-2018 said
  no busy indicator existed when `MapActivity::showBusy()` had painted one since
  2026-08-05.
