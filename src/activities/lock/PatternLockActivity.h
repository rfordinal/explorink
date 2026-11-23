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
// ## What is on screen, and what that does and does not protect
//
// The grid, the cursor, and a segmented meter counting the points entered. **The
// path is not drawn.**
// That keeps the pattern off the panel after the screen leaves, ghosting included,
// which matters on a device that gets lost or stolen with the last frame still
// visible on an unpowered panel.
//
// It does **not** protect against someone watching the entry: the cursor has to be
// visible or the grid is pointless, and a watcher who sees every press has the
// pattern. Stated here because the dot PIN it replaces made the stronger claim
// ("a shoulder looking at the panel must learn nothing", WalletUnlockActivity.h)
// and this screen cannot. docs/pattern-lock.md, "What the hidden path buys".
class PatternLockActivity final : public Activity {
 public:
  enum class Verdict : uint8_t { Wrong, Ok, LockedOut };

  // `speculative` is true for the probes `autoSubmit` fires after every point from
  // kMinPoints upward, false for a CONFIRM press. A verifier whose check is
  // expensive (the wallet's unwrap is 905 ms, docs/wallet-plan.md) should answer
  // Wrong to a speculative probe without doing the work, and a verifier that
  // counts failures must not count one either -- five probes per entry would lock
  // a rider out of their own 8-point pattern on the way to entering it.
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
    uint8_t startCell = patternlock::kDefaultStart;
    // Asked once on entry, so a rate limiter the caller is already enforcing shows
    // up before the first press rather than after the first wasted attempt -- a
    // power cycle must not buy a fresh set of guesses. Its own callback and not an
    // empty-path call into the Verifier, because a verifier that counts failures
    // would count that call.
    std::function<bool()> isLockedOut = nullptr;
  };

  PatternLockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Config config, Verifier verifier);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void render(HalDisplay::RefreshMode mode);
  void onStep(patternlock::Step step);
  void submit(bool speculative);
  void finishUnlocked();

  Config config_;
  Verifier verifier_;
  patternlock::Entry entry_;
  bool lockedOut_ = false;
  bool lastAttemptWrong_ = false;
  // finish() is a deferred pop -- the manager runs it at the end of the loop, so
  // everything after it in this frame still executes. Without this flag the
  // auto-submit path draws the gate one more time on its way out, which on e-ink
  // is a visible refresh of a screen that is leaving.
  bool finished_ = false;
};
