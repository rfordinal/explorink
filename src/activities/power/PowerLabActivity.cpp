#include "PowerLabActivity.h"

#if defined(ENABLE_POWER_LAB) && ENABLE_POWER_LAB

#include <Arduino.h>
#include <BlePositionServer.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <esp_sleep.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "PowerLog.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "PWRLAB";
constexpr int kMargin = 16;

struct StateSpec {
  const char* label;    // goes into power.csv's `state` column verbatim
  bool radio;           // BLE controller up
  bool pinFullClock;    // true = hold 160 MHz, false = let the throttle engage
  const char* explains; // one line on the panel: what this state prices
};

// Index order matches PowerLabActivity::State.
constexpr StateSpec kStates[] = {
    {"idle-160", false, true, "no radio, 160 MHz pinned"},
    {"idle-10", false, false, "no radio, throttles to 10 MHz"},
    {"radio-160", true, true, "advertising, 160 MHz -- what the map does"},
    {"radio-80", true, false, "advertising, throttles to 80 MHz floor"},
};
static_assert(sizeof(kStates) / sizeof(kStates[0]) == static_cast<size_t>(PowerLabActivity::State::Count),
              "kStates and State must stay in step");

const char* wakeCauseName(const esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "reset (not a sleep wake)";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "timer";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "gpio";
    case ESP_SLEEP_WAKEUP_UART:
      return "uart";
    default:
      return "other";
  }
}

}  // namespace

volatile int PowerLabActivity::pendingState_ = -1;

bool PowerLabActivity::selectByName(const char* name) {
  if (name == nullptr) return false;
  for (size_t i = 0; i < static_cast<size_t>(State::Count); ++i) {
    if (std::strcmp(name, kStates[i].label) == 0) {
      pendingState_ = static_cast<int>(i);
      return true;
    }
  }
  return false;
}

void PowerLabActivity::onEnter() {
  Activity::onEnter();
  logEntryFacts();
  applyState();
}

void PowerLabActivity::onExit() {
  // Leave nothing behind: the next screen must not inherit a radio this one
  // brought up, and power.csv must stop claiming a state that has ended.
  if (radioUp_) {
    freeink::BlePositionServer::getInstance().end();
    radioUp_ = false;
  }
  powerManager.setPowerSaving(false);
  PowerLog::setState("-");
  Activity::onExit();
}

void PowerLabActivity::logEntryFacts() const {
  // The wake cause is the only way to tell a real timer wake from a brownout
  // that power-cycled the board, which is exactly the distinction experiment 1
  // has to make (docs/power-idle-sleep.md, "The power lab screen"). Printed on
  // every entry, not only after a sleep, so the line is always in the log to
  // compare against.
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  LOG_INF(kLogTag, "wake cause: %s (%d)", wakeCauseName(cause), static_cast<int>(cause));
  // A lab screen that brings up BLE pays the map's ~57 KB, so the number goes
  // in the log rather than being assumed (docs/power-idle-sleep.md).
  LOG_INF(kLogTag, "heap on entry: %u free, %u min", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMinFreeHeap()));
}

bool PowerLabActivity::preventThrottle() { return kStates[static_cast<size_t>(state_)].pinFullClock; }

void PowerLabActivity::applyState() {
  const StateSpec& spec = kStates[static_cast<size_t>(state_)];

  if (spec.radio && !radioUp_) {
    // Same trap MapActivity::onEnter() closes: NimBLEDevice::init() hangs
    // solid at a low clock, and this screen can be sitting in idle-10 when the
    // operator picks a radio state. HalPowerManager's BLE_SAFE_FREQ floor only
    // starts protecting once the controller is enabled, so the window before
    // that has to be closed here (docs/power-management.md, "Why 10 MHz breaks
    // BLE").
    powerManager.setPowerSaving(false);
    if (!freeink::BlePositionServer::getInstance().begin()) {
      LOG_ERR(kLogTag, "BlePositionServer.begin() failed -- state %s is not what it says", spec.label);
    } else {
      radioUp_ = true;
    }
  } else if (!spec.radio && radioUp_) {
    freeink::BlePositionServer::getInstance().end();
    radioUp_ = false;
  }

  // Only after the radio is where it belongs: the floor this asks for depends
  // on the controller's status, so asking first would compute the wrong one.
  powerManager.setPowerSaving(!spec.pinFullClock);

  // The label is the state, not the radio's moment-to-moment status.
  // Advertising versus connected is already the `ble` column's job, and
  // deriving it per tick would put work in a tick this screen exists to keep
  // empty.
  PowerLog::setState(spec.label);
  LOG_INF(kLogTag, "state %s: radio %s, clock %s", spec.label, spec.radio ? "up" : "down",
          spec.pinFullClock ? "pinned 160" : "throttle allowed");
  requestUpdate();
}

void PowerLabActivity::loop() {
  if (pendingState_ >= 0) {
    const int wanted = pendingState_;
    pendingState_ = -1;
    if (wanted < static_cast<int>(State::Count) && wanted != static_cast<int>(state_)) {
      state_ = static_cast<State>(wanted);
      applyState();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const auto step = [this](const int delta) {
    const int count = static_cast<int>(State::Count);
    int index = static_cast<int>(state_) + delta;
    if (index < 0) index = count - 1;
    if (index >= count) index = 0;
    state_ = static_cast<State>(index);
    applyState();
  };

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    step(-1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    step(1);
    return;
  }
  // Nothing else. No timer, no poll, no periodic redraw: every one of those
  // would be work inside the tick this screen is measuring.
}

void PowerLabActivity::render(RenderLock&&) {
  renderer.clearScreen();

  int y = 8;
  renderer.drawText(UI_12_FONT_ID, kMargin, y, "Power lab", true);
  y += 26;
  renderer.drawText(UI_10_FONT_ID, kMargin, y, "One state, held. Rows land in power.csv", true);
  y += 18;
  renderer.drawText(UI_10_FONT_ID, kMargin, y, "under the state name below.", true);
  y += 30;

  for (size_t i = 0; i < static_cast<size_t>(State::Count); ++i) {
    const bool current = i == static_cast<size_t>(state_);
    char line[64];
    snprintf(line, sizeof(line), "%s %s", current ? ">" : " ", kStates[i].label);
    renderer.drawText(UI_12_FONT_ID, kMargin, y, line, true);
    y += 20;
    renderer.drawText(UI_10_FONT_ID, kMargin + 24, y, kStates[i].explains, true);
    y += 24;
  }

  y += 8;
  renderer.drawText(UI_10_FONT_ID, kMargin, y, "Unplug USB before reading a slope:", true);
  y += 18;
  renderer.drawText(UI_10_FONT_ID, kMargin, y, "VBUS charges the cell and the number", true);
  y += 18;
  renderer.drawText(UI_10_FONT_ID, kMargin, y, "stops meaning anything.", true);

  // Deliberately not translated. This screen exists only in env:powerlab, is
  // read by whoever is running the bench, and adding a StrId per line to every
  // language file for an instrument would be churn with no reader.
  mappedInput.mapLabels("Home", "", "Prev", "Next");
}

#endif  // ENABLE_POWER_LAB
