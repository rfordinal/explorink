#pragma once

#include <I18n.h>

#include <functional>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class GfxRenderer;

class IntervalSelectionActivity final : public Activity {
 public:
  // Optional: fired with the live value on every drag/tap/step change, before
  // Confirm. Lets a caller preview an effect immediately (e.g. a frontlight
  // color mix) instead of only on the final ActivityResult. Empty by default,
  // so existing callers (sleep timeout) are unaffected. Deliberately not a
  // SETTINGS write itself -- callers that use it for a persisted field still
  // need to revert it on a cancelled result themselves, since this activity
  // has no concept of what "revert" means for an arbitrary caller.
  std::function<void(int)> onValueChanged;

  explicit IntervalSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* activityName,
                                     StrId titleId, int initialValue, int minValue, int maxValue, int smallStep,
                                     int largeStep, StrId valueFormatId = StrId::STR_NONE_OPT,
                                     bool readerActivity = false, bool ignoreInitialConfirmRelease = false,
                                     StrId maxBoundaryLabelId = StrId::STR_NONE_OPT)
      : Activity(activityName, renderer, mappedInput),
        titleId(titleId),
        valueFormatId(valueFormatId),
        maxBoundaryLabelId(maxBoundaryLabelId),
        value(initialValue),
        minValue(minValue),
        maxValue(maxValue),
        smallStep(smallStep),
        largeStep(largeStep),
        readerActivity(readerActivity),
        ignoreConfirmRelease(ignoreInitialConfirmRelease) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return readerActivity; }

 private:
  StrId titleId;
  StrId valueFormatId;
  StrId maxBoundaryLabelId;
  int value;
  int minValue;
  int maxValue;
  int smallStep;
  int largeStep;
  bool readerActivity;
  bool ignoreConfirmRelease;
  bool draggingBar = false;
  ButtonNavigator buttonNavigator;

  void adjustValue(int delta);
  int clampedValue(int candidate) const;
  void drawStepHintLine(int y, StrId labelId, int step);
};
