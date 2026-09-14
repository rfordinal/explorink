#pragma once

#include <Print.h>

#include <cstdint>

// A timestamped log of the GT911's raw status register, taken with the loop
// deliberately blocked (`CMD:TOUCHLOG`, src/main.cpp).
//
// Why this exists: T-266 cannot be designed without it. `docs/input-gestures.md`
// lists five open questions about this controller and every one of them is a
// question about *when* a byte changes, which nothing in this firmware can
// currently see. What is recorded today is the recogniser's opinion after the
// fact -- "a double tap read as two single taps" -- and that opinion is stamped
// when the loop got round to reading, not when the finger moved. The whole edge
// policy of the redesign rests on the answers, so guessing them is not an
// option.
//
// This is the ordinary instrument for the job rather than an invention: Linux
// has `evtest` and Android has `getevent`, and both print a timestamped stream
// of raw events *before* any gesture layer interprets them. Neither would help
// here, because they sit above a driver we do not have; this is that same idea
// pushed down to the one register the driver would be reading.
//
// What it records, per sample:
//
//   - `micros()` at the moment of the read, so intervals are the controller's
//     and not the loop's
//   - the status byte at 0x814E: bit 7 buffer-ready, bit 4 the capacitive home
//     key, bits 3..0 the contact count
//   - the level on the GT911's INT line, which mainline Linux's `goodix.c`
//     treats as the event and this firmware does not read at all
//   - the first contact's raw coordinates, or -1 when the frame had none. Added
//     after the first captures: the status byte cannot tell a finger on the
//     capacitive home key from a finger on the glass, and a finger believed to
//     be on the pad often lands on the glass beside it instead
//
// Two modes, and the difference between them is open question 2:
//
//   clear    write 0 back to 0x814E after every read that had bit 7 set, the
//            way `goodix_ts_irq_handler()` does after processing a report
//   noclear  never write, so a frame left unacknowledged is visible for as long
//            as the controller holds it
//
// The capture blocks `loop()` for its whole duration and never calls
// `gpio.update()`. That is the point, not a side effect: the failure being
// measured only happens while the loop is blocked by a panel refresh, so an
// instrument that keeps the loop running would measure a state the bug does not
// live in.
//
// Devel builds only, gated on ENABLE_TOUCHLOG_CMD (platformio.ini: default,
// x4pro, t5s3pro -- not gh_release, gh_release_rc, slim), and serial only, never
// on the BLE grammar (MapCommandParser.h). It is read-only and says nothing
// about the rider -- the reply is touch-controller register bytes and nothing
// else -- but a command that can freeze the screen for eight seconds is a denial
// of service in the hands of whoever picks up a lost device, and BLE advertises
// with no pairing and no bonding (docs/ble-advertising.md). Widening it to a
// release build would be a separate, deliberate decision.
namespace DebugTouchLog {

#ifdef ENABLE_TOUCHLOG_CMD

// Run one capture and print it. Blocks for `durationMs`.
//
// `intervalUs` is the target gap between reads; the real gap is recorded per
// sample, so a missed deadline shows up in the data instead of being hidden.
// Output is run-length encoded on the (status, INT) pair: one line per change
// plus how many identical samples followed it, which keeps a six-second capture
// to a few dozen lines while still answering "did the ready flag come back on
// its own". Coordinates are the run's FIRST sample: the question they answer is
// which zone a contact is in, and a run is one zone.
//
// Rows are written behind `availableForWrite()` rather than straight to printf.
// Without that the dump loses lines -- measured, twice -- and a dropped block
// reads as a silent controller. `TOUCHLOG_END` carries the row count so the host
// can tell it lost some.
//
// Returns false when the controller could not be addressed at all; the caller
// prints the error, this prints the data.
bool capture(Print& out, uint32_t durationMs, uint32_t intervalUs, bool clearAfterRead);

// How long the input sampler actually goes unread (`CMD:LOOPGAP`).
//
// The capture above says what the controller does; this says whether anyone is
// listening. They are the two halves of the same question, because a GT911 holds
// exactly one unacknowledged frame and discards everything after it (measured
// 2026-09-14, `noclear`: the status sat on one frame for 7.99 s while the key was
// tapped repeatedly, and not one of those taps reached the register). So the gap
// between two `gpio.update()` calls is the width of the window in which a whole
// gesture collapses into one edge.
//
// `noteUpdate()` is called from `MappedInputManager::update()`, which is the
// exact call whose spacing decides it -- not `loop()`, which can iterate without
// sampling. Cost is one micros() read and one branch.
//
// Kept as a histogram plus the ten worst gaps rather than a trace: the interesting
// number is the tail, a map render is rare against thousands of fast iterations,
// and a trace of every gap would need the same flow control the capture needed.
void noteUpdate();

// Print the histogram and the worst gaps, then start over. Resetting on read is
// deliberate: a measurement is "since I last asked", so a render can be isolated
// by reading, doing the thing, and reading again.
void reportGaps(Print& out);

#endif  // ENABLE_TOUCHLOG_CMD

}  // namespace DebugTouchLog
