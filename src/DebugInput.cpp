#include "DebugInput.h"

#ifdef ENABLE_BUTTON_CMD

namespace DebugInput {
namespace {

struct Pending {
  uint8_t button;
  uint32_t holdMs;
};

// Eight is enough for a host that pipes a short menu walk in one go ("back,
// back, down, confirm") and small enough that a runaway script is refused
// rather than buffered into a minute of ghost presses.
constexpr uint8_t kQueueSize = 8;

Pending pendingQueue[kQueueSize] = {};
uint8_t queueHead = 0;
uint8_t queueCount = 0;

enum class Phase : uint8_t { Idle, Down, Released };
Phase phase = Phase::Idle;

uint8_t heldButton = kNoButton;
bool pressedEdge = false;
bool releasedEdge = false;
unsigned long downAtMs = 0;
uint32_t requestedHoldMs = 0;
unsigned long heldSoFarMs = 0;
// No frame has been seen yet. Any real sequence differs from this, so the
// first pump() always runs.
uint32_t lastFrameSeq = 0xFFFFFFFFu;

char lowerAscii(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool namesEqual(const char* a, const char* b) {
  while (*a != '\0' && *b != '\0') {
    if (lowerAscii(*a) != lowerAscii(*b)) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

}  // namespace

bool queue(const uint8_t hwButton, const uint32_t holdMs) {
  if (hwButton >= kButtonCount) return false;
  if (queueCount >= kQueueSize) return false;
  pendingQueue[(queueHead + queueCount) % kQueueSize] = Pending{hwButton, holdMs};
  ++queueCount;
  return true;
}

void pump(const uint32_t frameSeq, const unsigned long nowMs) {
  if (frameSeq == lastFrameSeq) return;
  lastFrameSeq = frameSeq;
  pressedEdge = false;
  releasedEdge = false;

  switch (phase) {
    case Phase::Released:
      // One idle frame after every release before the next press starts.
      // Without it two queued taps run into each other and the activity sees
      // one press whose held time spans both -- a "back, back" walk would
      // long-press back to Home instead of stepping up twice.
      phase = Phase::Idle;
      heldButton = kNoButton;
      heldSoFarMs = 0;
      return;
    case Phase::Down:
      heldSoFarMs = nowMs - downAtMs;
      if (heldSoFarMs >= requestedHoldMs) {
        releasedEdge = true;
        phase = Phase::Released;
      }
      return;
    case Phase::Idle:
      break;
  }

  if (queueCount == 0) return;
  const Pending next = pendingQueue[queueHead];
  queueHead = static_cast<uint8_t>((queueHead + 1) % kQueueSize);
  --queueCount;

  heldButton = next.button;
  requestedHoldMs = next.holdMs;
  downAtMs = nowMs;
  heldSoFarMs = 0;
  pressedEdge = true;
  phase = Phase::Down;
}

bool button(const uint8_t hwButton, const Query query) {
  if (hwButton != heldButton) return false;
  return anyEdge(query);
}

bool anyEdge(const Query query) {
  switch (query) {
    case Query::Pressed:
      return pressedEdge;
    case Query::Released:
      return releasedEdge;
    case Query::Down:
      return phase == Phase::Down;
  }
  return false;
}

// The release frame counts: a reader page turn is `wasReleased(Back) &&
// getHeldTime() < GO_HOME_MS`, so the frame that reports the release is
// exactly the frame that has to report the total held time.
bool active() { return phase == Phase::Down || releasedEdge; }

unsigned long heldMs() { return heldSoFarMs; }

uint8_t buttonFromName(const char* name) {
  if (name == nullptr) return kNoButton;
  for (uint8_t i = 0; i < kButtonCount; ++i) {
    if (namesEqual(name, kButtonNames[i])) return i;
  }
  return kNoButton;
}

void reset() {
  queueHead = 0;
  queueCount = 0;
  phase = Phase::Idle;
  heldButton = kNoButton;
  pressedEdge = false;
  releasedEdge = false;
  downAtMs = 0;
  requestedHoldMs = 0;
  heldSoFarMs = 0;
  lastFrameSeq = 0xFFFFFFFFu;
}

}  // namespace DebugInput

#endif  // ENABLE_BUTTON_CMD
