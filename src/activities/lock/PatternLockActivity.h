#pragma once

#include <cstdint>
#include <functional>

#include "activities/Activity.h"
#include "components/lock/PatternLock.h"

// The gate screen: a 4x4 grid, a cursor walked by the four direction buttons, and
// a path that either is the stored pattern or is not.
//
// ## Why it is its own activity and owns no secret
//
// It is pushed in front of whatever needs guarding and reports one bit back
// (`PatternLockResult::unlocked`). It never reads NVS, never hashes, never knows
// what the right pattern is -- the caller hands it a `Verifier` and keeps the
// secret, the rate limiter and the failure counter on its own side. That is what
// makes the same screen usable for the wallet (verifier = unwrap the key) and for
// a device-level lock (verifier = compare a hash) without either one's policy
// leaking into the other.
//
// ## Controls
//
//   LEFT / RIGHT     front buttons, one cell (MappedInputManager.cpp, Button::Left)
//   UP / DOWN        side buttons, one cell (fixed there, not user-remappable)
//   CONFIRM          try the path
//   BACK             one point back, or leave the screen when only the start is left
//
// A press into a wall does nothing and does not redraw -- an e-ink refresh per
// refused press would read as a stutter and say nothing.
//
// **On a touch board in TOUCH_ANYWHERE the screen draws and drives its own
// controls**, because the theme's hint boxes exist only in TOUCH_BUTTONS_ONLY
// (`TouchPolicy::hintsVisible()`) and the stored default is ANYWHERE
// (`CrossPointSettings.h`, `touchMode`). Without this a factory-default X4 Pro --
// two hardware keys, everything else touch -- could move the cursor **up and
// down** and nothing else: no LEFT, no RIGHT, no CONFIRM, no BACK. That board's
// two keys are `up`=GPIO0 and `down`=GPIO7 with back/confirm/left/right all
// `PIN_UNASSIGNED` (`BoardConfig.h`, XTEINK_X4_PRO), which is the opposite of
// what this comment said until the second review read the pin table. A tap on a
// cell orthogonally next to the cursor steps there, and two boxes at the foot of
// the screen carry BACK and OK. That is this screen's own input model, not the
// theme's, and it exists only in the mode where the theme offers none.
//
// **With touch switched off entirely (`TOUCH_DISABLED`, or the panel lock) that
// board has no way to drive this screen at all** -- no hint boxes, no own boxes,
// and two keys that only move the cursor. The gate is then a dead end until the
// capacitive home key's double tap releases the lock, which `main.cpp` handles
// outside any activity. That is the device-wide question T-265 already asks, and
// this screen inherits rather than causes it (T-2084).
//
// ## What is on screen, and what that does and does not protect
//
// The grid, the cursor, and a segmented meter counting the points entered. **The
// path is not drawn.**
//
// That keeps the pattern off the panel after the screen leaves, ghosting included,
// which matters on a device that gets lost or stolen with the last frame still
// visible on an unpowered panel.
//
// It does **not** protect against someone watching the entry: the cursor has to be
// visible or the grid is pointless, and a watcher who sees every press has the
// pattern. Nor against a host on the cable: `CMD:SCREENSHOT` is ungated in every
// build and the cursor is in the framebuffer, so a poll per press rebuilds the
// path (T-2081). Stated here because the dot PIN it replaces made the stronger
// claim ("a shoulder looking at the panel must learn nothing",
// WalletUnlockActivity.h) and this screen cannot. docs/pattern-lock.md, "What the
// hidden path buys".
class PatternLockActivity final : public Activity {
 public:
  enum class Verdict : uint8_t { Wrong, Ok, LockedOut };

  // `speculative` is true for the probes `autoSubmit` fires after every point from
  // kMinPoints upward, false for a CONFIRM press.
  //
  // **A speculative call must not consume an attempt, and the screen pays that
  // back through `Config::onEntryEnded`** -- exactly one call per walk that did
  // not unlock, whether it ended at the eighth point, at BACK, or by the screen
  // leaving. Count there and a walk costs one attempt however many probes it
  // fired; count per probe and a rider walking toward an 8-point pattern locks
  // themselves out passing through four wrong prefixes.
  //
  // Without that callback auto-submit would make the limiter unreachable rather
  // than weaker: every guess an attacker makes arrives speculative, so the
  // counted path is never taken at all. Found in review, 2026-11-23, after the
  // first cut shipped exactly that hole with a comment claiming a factor of five.
  //
  // A verifier whose check is expensive (the wallet's unwrap is 905 ms,
  // docs/wallet-plan.md) should answer Wrong to a speculative probe without doing
  // the work, or leave auto-submit off.
  using Verifier = std::function<Verdict(const char* path, bool speculative)>;

  struct Config {
    // Already through tr() -- this component carries no strings of its own beyond
    // its status line, so one gate can say "Unlock wallet" and another "Unlock".
    const char* title = nullptr;
    const char* hint = nullptr;
    // Check the path as soon as it is long enough, with no CONFIRM press, so
    // reaching the last point unlocks by itself. Off by default: see `speculative`
    // above for what it costs a verifier that derives a key.
    bool autoSubmit = false;
    // The one point no button press produces, so it has to match the stored
    // pattern's first character or nothing a rider enters can ever match.
    // `patternlock::startCellOf()` reads it off a stored pattern; pass the two
    // together or not at all.
    uint8_t startCell = patternlock::kDefaultStart;
    // Asked once on entry, so a rate limiter the caller is already enforcing shows
    // up before the first press rather than after the first wasted attempt -- a
    // power cycle must not buy a fresh set of guesses. Its own callback and not an
    // empty-path call into the Verifier, because a verifier that counts failures
    // would count that call.
    std::function<bool()> isLockedOut = nullptr;
    // Seconds a caller's backoff still wants, re-read on every draw so a wait
    // counts down on the panel instead of sitting on a stale number. 0 means the
    // screen may be used now. Optional: a caller with no backoff omits it.
    std::function<uint32_t()> waitSecondsLeft = nullptr;
    // One walk ended without unlocking. The caller's cue to count a failed
    // attempt when auto-submit is on -- see `Verifier` above for why it cannot be
    // counted per probe. Never called for a walk that unlocked, and never twice
    // for one walk.
    //
    // **It can run while the ActivityManager holds the render mutex** -- the exit
    // path calls it from onExit(), and the manager's Pop branch takes a
    // `RenderLock` before destroying an activity (`ActivityManager.cpp`). That
    // mutex is not recursive, so a callback that repaints -- `requestUpdateAndWait()`,
    // or taking its own RenderLock -- deadlocks the device. Count, persist, and
    // return; draw on the next frame.
    std::function<void()> onEntryEnded = nullptr;
  };

  PatternLockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Config config, Verifier verifier);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // The manager's render task calls this one, not the private draw below. Every
  // other activity in the tree overrides it; the first cut of this screen did not
  // and drew only from its own code, so a manager-driven repaint -- the one after
  // a child activity pops back to this one -- left the previous screen's frame on
  // the panel while this one was live and reading buttons.
  void render(RenderLock&& lock) override;
  // A gate that a swipe dismisses is not a gate. The bottom-edge gesture would
  // otherwise reach ActivityManager::goHome(), which replaces the whole stack --
  // this screen AND the caller behind it, with no result delivered and nothing
  // guarding whatever the caller was protecting.
  bool handleHomeGesture() override;

 private:
  // Geometry derived from the panel and the theme, in one place because the
  // drawing and the touch hit-test have to agree about it exactly.
  struct Layout {
    int pitch = 0;
    int gridTop = 0;
    int gridLeft = 0;
    int nodeRadius = 0;
    int ringRadius = 0;
    int meterLeft = 0;
    int meterTop = 0;
    int meterWidth = 0;
    int meterHeight = 0;
    int statusY = 0;
    // The screen's own BACK/OK boxes. Zero height when the theme draws hint boxes
    // and this screen does not need its own.
    bool ownTouch = false;
    int boxTop = 0;
    int boxHeight = 0;
    int backLeft = 0;
    int okLeft = 0;
    int boxWidth = 0;
  };

  Layout layout() const;
  void draw(HalDisplay::RefreshMode mode);
  void redraw(HalDisplay::RefreshMode mode);
  void onStep(patternlock::Step step);
  void onBack();
  bool handleTouch();
  void submit(bool speculative);
  void finishUnlocked();
  // Hands the caller the one attempt a walk of speculative probes costs, if any
  // are outstanding. Idempotent: a walk is counted once or not at all.
  void settleEntry();

  Config config_;
  Verifier verifier_;
  patternlock::Entry entry_;
  bool lockedOut_ = false;
  bool lastAttemptWrong_ = false;
  // At least one speculative probe has been fired for the current walk and the
  // caller has not been told about it yet.
  bool speculativeOutstanding_ = false;
  // finish() is a deferred pop -- the manager runs it at the end of the loop, so
  // everything after it in this frame still executes. Without this flag the
  // auto-submit path draws the gate one more time on its way out, which on e-ink
  // is a visible refresh of a screen that is leaving.
  bool finished_ = false;
};
