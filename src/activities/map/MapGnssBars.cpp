#include "MapGnssBars.h"

namespace MapGnssBars {

namespace {

// One rung of a rising ladder, with the hysteresis applied only to rungs that
// are already lit. `lit` is how many rungs the panel currently shows, so rung i
// gets its discount exactly when it is one of them.
//
// The discount is subtracted from the threshold rather than added to the
// reading, because the reading is unsigned and a threshold of 4 with a slack of
// 2 has to become 2, not wrap.
int rungsFor(uint16_t value, const uint8_t* thresholds, int count, int lit, uint8_t hysteresis) {
  int rungs = 0;
  for (int i = 0; i < count; ++i) {
    uint16_t need = thresholds[i];
    if (lit > i) need = need > hysteresis ? static_cast<uint16_t>(need - hysteresis) : 1;
    if (value >= need) rungs = i + 1;
  }
  return rungs;
}

}  // namespace

Block resolve(uint8_t tracked, uint8_t bestSnr, const State& state) {
  Block block;
  block.bars = rungsFor(tracked, kTrackedForBar, kBarCount, state.bars, kTrackedHysteresis);
  block.heightStep = rungsFor(bestSnr, kBestSnrForHeightStep, kHeightStepCount, state.heightStep, kBestSnrHysteresis);
  // A receiver hearing nothing reports 0, and `need` above is floored at 1, so
  // zero can never light a rung. Stated here anyway: it is the case the whole
  // block was added for, and an empty block has to be reachable.
  return block;
}

int barHeightPx(int heightStep, int iconHeight) {
  if (heightStep <= 0) return 0;
  if (heightStep >= kHeightStepCount) return iconHeight;
  // Ceiling division. See the header for why rounding up matters at the bottom
  // end of a 14 px row.
  return (iconHeight * heightStep + kHeightStepCount - 1) / kHeightStepCount;
}

}  // namespace MapGnssBars
