#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;

  // Which screen the device went to sleep from, for the wake-side routing.
  // lastSleepFromReader cannot answer this: it is one bool, and MapActivity does
  // not set it (see docs/sleep-screen.md, "Hardware finding"). Written by
  // enterDeepSleep() from the live activity, so it describes the sleep that just
  // happened and not some earlier one.
  enum SLEEP_ACTIVITY : uint8_t { SLEEP_ACTIVITY_OTHER = 0, SLEEP_ACTIVITY_MAP = 1 };
  uint8_t lastSleepActivity = SLEEP_ACTIVITY_OTHER;

  // The route MapActivity had open, so a wake into the map comes back with it.
  // MapActivity::routePath_ is a bare member and dies with the activity, and the
  // route picker is a separate screen -- without this, a wake would land in the
  // map with the route silently gone. Read only when lastSleepActivity is
  // SLEEP_ACTIVITY_MAP, so a stale value cannot resurrect a route on its own.
  std::string lastSleepRoutePath;

  // Anti-boot-loop guard for the wake-into-map path, the same shape as
  // readerActivityLoadCount above: incremented before entering the map, zeroed by
  // MapActivity once it is actually up. Nonzero at boot means the last attempt
  // never finished, so the wake routes to Home instead. Without it, firmware that
  // hangs inside the map hangs again on every wake and there is no way out
  // (a panic reboots into the crash report; a hang does not).
  uint8_t mapActivityLoadCount = 0;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
