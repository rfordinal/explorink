// The serial console's synthetic button presses (src/DebugInput.cpp).
//
// Worth a host suite because the thing being asserted is a shape in *time* --
// which frame carries the press edge, which carries the release, what the held
// time reads on each -- and every consumer of it is a long-press check
// somewhere in the UI. Getting that wrong on the device looks like "the
// command does nothing" or "back went Home instead of one level up", and both
// cost a flash to find out.

#include <gtest/gtest.h>

#include "DebugInput.h"

namespace {

// Mirrors HalGPIO::BTN_*; the header deliberately does not include the HAL.
constexpr uint8_t kBack = 0;
constexpr uint8_t kConfirm = 1;
constexpr uint8_t kDown = 5;
constexpr uint8_t kPower = 6;

// A loop() iteration: the frame sequence steps, the clock advances.
class Frames {
 public:
  void step(const unsigned long ms = 10) {
    nowMs_ += ms;
    DebugInput::pump(++seq_, nowMs_);
  }
  unsigned long nowMs() const { return nowMs_; }

 private:
  uint32_t seq_ = 0;
  unsigned long nowMs_ = 1000;
};

class DebugInputTest : public ::testing::Test {
 protected:
  void SetUp() override { DebugInput::reset(); }
};

using Q = DebugInput::Query;

TEST_F(DebugInputTest, NothingQueuedIsSilent) {
  Frames frames;
  frames.step();
  EXPECT_FALSE(DebugInput::anyEdge(Q::Pressed));
  EXPECT_FALSE(DebugInput::anyEdge(Q::Released));
  EXPECT_FALSE(DebugInput::active());
}

TEST_F(DebugInputTest, TapPressesThenReleases) {
  Frames frames;
  ASSERT_TRUE(DebugInput::queue(kConfirm, 0));

  frames.step();
  EXPECT_TRUE(DebugInput::button(kConfirm, Q::Pressed));
  EXPECT_TRUE(DebugInput::button(kConfirm, Q::Down));
  EXPECT_FALSE(DebugInput::button(kConfirm, Q::Released));
  // The press edge lasts one frame, the same as a real button's.
  EXPECT_FALSE(DebugInput::button(kBack, Q::Pressed));

  frames.step();
  EXPECT_FALSE(DebugInput::button(kConfirm, Q::Pressed));
  EXPECT_TRUE(DebugInput::button(kConfirm, Q::Released));
  // Released means up: an activity polling isPressed() must not still see it.
  EXPECT_FALSE(DebugInput::button(kConfirm, Q::Down));

  frames.step();
  EXPECT_FALSE(DebugInput::anyEdge(Q::Released));
  EXPECT_FALSE(DebugInput::active());
}

TEST_F(DebugInputTest, HoldStaysDownUntilTheTimeIsUp) {
  Frames frames;
  ASSERT_TRUE(DebugInput::queue(kBack, 1500));

  frames.step();
  EXPECT_TRUE(DebugInput::button(kBack, Q::Pressed));

  // 100 frames of 10 ms: still held, and the held time is what the long-press
  // checks in ReaderUtils and friends read.
  for (int i = 0; i < 100; ++i) frames.step();
  EXPECT_TRUE(DebugInput::button(kBack, Q::Down));
  EXPECT_FALSE(DebugInput::anyEdge(Q::Released));
  EXPECT_GE(DebugInput::heldMs(), 1000u);
  EXPECT_LT(DebugInput::heldMs(), 1500u);

  for (int i = 0; i < 50; ++i) frames.step();
  EXPECT_TRUE(DebugInput::button(kBack, Q::Released));
  // The release frame carries the total, because that is the frame every
  // `wasReleased(X) && getHeldTime() < N` in the UI runs on.
  EXPECT_GE(DebugInput::heldMs(), 1500u);
  EXPECT_TRUE(DebugInput::active());
}

TEST_F(DebugInputTest, TwoTapsDoNotRunIntoOneHold) {
  Frames frames;
  ASSERT_TRUE(DebugInput::queue(kDown, 0));
  ASSERT_TRUE(DebugInput::queue(kDown, 0));

  frames.step();
  EXPECT_TRUE(DebugInput::button(kDown, Q::Pressed));
  frames.step();
  EXPECT_TRUE(DebugInput::button(kDown, Q::Released));

  // The idle frame between them: nothing down, nothing edging. Without it the
  // second press would start on the release frame and the pair would read as
  // one long press.
  frames.step();
  EXPECT_FALSE(DebugInput::active());
  EXPECT_FALSE(DebugInput::anyEdge(Q::Pressed));

  frames.step();
  EXPECT_TRUE(DebugInput::button(kDown, Q::Pressed));
  EXPECT_LT(DebugInput::heldMs(), 10u);
}

TEST_F(DebugInputTest, QueueRunsInOrder) {
  Frames frames;
  ASSERT_TRUE(DebugInput::queue(kBack, 0));
  ASSERT_TRUE(DebugInput::queue(kConfirm, 0));

  frames.step();
  EXPECT_TRUE(DebugInput::button(kBack, Q::Pressed));
  frames.step();  // release
  frames.step();  // idle
  frames.step();
  EXPECT_TRUE(DebugInput::button(kConfirm, Q::Pressed));
  EXPECT_FALSE(DebugInput::button(kBack, Q::Down));
}

TEST_F(DebugInputTest, FullQueueIsRefusedNotDropped) {
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(DebugInput::queue(kPower, 0)) << "entry " << i;
  // The ninth is refused so the console can answer BUTTON_ERR:busy. A silently
  // dropped press would leave a host script's screenshots one screen off.
  EXPECT_FALSE(DebugInput::queue(kPower, 0));
}

TEST_F(DebugInputTest, UnknownButtonIndexIsRefused) {
  EXPECT_FALSE(DebugInput::queue(7, 0));
  EXPECT_FALSE(DebugInput::queue(DebugInput::kNoButton, 0));
}

TEST_F(DebugInputTest, TwoPumpsInOneFrameAdvanceOnce) {
  ASSERT_TRUE(DebugInput::queue(kConfirm, 0));
  // Every reader below pumps lazily off the same frame counter, so the second
  // call in a frame has to be a no-op or an edge would be consumed by whoever
  // asked first.
  DebugInput::pump(1, 1000);
  DebugInput::pump(1, 1000);
  EXPECT_TRUE(DebugInput::button(kConfirm, Q::Pressed));
  EXPECT_TRUE(DebugInput::button(kConfirm, Q::Down));
}

TEST_F(DebugInputTest, NamesMapToButtonIndices) {
  EXPECT_EQ(DebugInput::buttonFromName("back"), kBack);
  EXPECT_EQ(DebugInput::buttonFromName("CONFIRM"), kConfirm);
  EXPECT_EQ(DebugInput::buttonFromName("Power"), kPower);
  EXPECT_EQ(DebugInput::buttonFromName("middle"), DebugInput::kNoButton);
  EXPECT_EQ(DebugInput::buttonFromName(""), DebugInput::kNoButton);
  // A prefix is not a name: "ba" must not select "back".
  EXPECT_EQ(DebugInput::buttonFromName("ba"), DebugInput::kNoButton);
  EXPECT_EQ(DebugInput::buttonFromName("backwards"), DebugInput::kNoButton);
  EXPECT_EQ(DebugInput::buttonFromName(nullptr), DebugInput::kNoButton);
}

}  // namespace
