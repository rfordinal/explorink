#pragma once
#include <ArduinoJson.h>
#include <Epub/ReaderRenderSpec.h>
#include <PersistableStore.h>

#include <cstdint>

#include "activities/map/MapRideMode.h"

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    // Was COVER (book cover) upstream. TrailInk has no book open most of the
    // time, so this slot now shows the last known map fix instead.
    LOCATION = 3,
    LOCATION_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum STATUS_BAR_CLOCK_MODE {
    STATUS_BAR_CLOCK_HIDE = 0,
    STATUS_BAR_CLOCK_RIGHT = 1,
    STATUS_BAR_CLOCK_LEFT = 2,
    STATUS_BAR_CLOCK_MODE_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTONS_DISABLED = 2, SIDE_BUTTON_LAYOUT_COUNT };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { NOTOSERIF = 0, NOTOSANS = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Reader font size is a point size, not an enum slot — see fontPointSize.
  // Legacy 1.4-and-earlier files stored a 0..3 SMALL/MEDIUM/LARGE/EXTRA_LARGE
  // slot; fromJson() folds that range up (see LEGACY_FONT_SIZE_MAX).
  static constexpr uint8_t LEGACY_FONT_SIZE_MAX = 3;
  static constexpr uint8_t DEFAULT_FONT_POINT_SIZE = 14;
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, FORCE_REFRESH = 3, FOOTNOTES = 4, SHORT_PWRBTN_COUNT };

  // Long-press Confirm action while reading an EPUB. The setting cycles through these values.
  // Persisted in settings.json by index: any new function (e.g. dictionary, bookmark) MUST use a
  // value >= 2 and be appended at the END of the enumValues array in SettingsList.h, otherwise the
  // stored indices shift and existing saves are silently misinterpreted.
  enum LONG_PRESS_MENU_FUNCTION {
    LP_MENU_KOSYNC = 0,
    LP_MENU_DISABLED = 1,
    LP_MENU_BOOKMARK = 2,
    LP_MENU_DICTIONARY = 3,
    LONG_PRESS_MENU_FUNCTION_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Map zoom mode. Manual is the button-driven ladder (stepZoom(), unchanged).
  // Auto reserves the setting for a speed- and junction-proximity-driven
  // picker (docs/map-data-spec.md, "Auto zoom picks a rung") -- not wired up
  // yet, this only lets the rider select it.
  enum MAP_ZOOM_MODE { MAP_ZOOM_MANUAL = 0, MAP_ZOOM_AUTO = 1, MAP_ZOOM_MODE_COUNT };
  // Whether the frame's "up" is the rider's heading or true north.
  enum MAP_ROTATION_MODE { MAP_ROTATION_HEADING_UP = 0, MAP_ROTATION_NORTH_UP = 1, MAP_ROTATION_MODE_COUNT };
  // Whether the frame's heading (when MAP_ROTATION_HEADING_UP) tracks the
  // incoming fix or stays frozen at whatever it was when the rider switched
  // to Manual (MapActivity::updateManualHeadingCapture()).
  enum MAP_HEADING_MODE { MAP_HEADING_AUTO = 0, MAP_HEADING_MANUAL = 1, MAP_HEADING_MODE_COUNT };
  // When the device asks the phone whether the tiles it already holds have
  // been republished (docs/tile-freshness.md). Off by default: it spends the
  // phone's mobile data and the rider has to choose it, same rule as
  // mapAutoSyncTiles.
  //
  //   Off        never ask.
  //   SyncScreen ask once when the tile sync screen opens -- preparation at
  //              home, which is where a rider expects data to be spent.
  //   Live       ask from the map screen too, on a cooldown, mid-ride.
  enum MAP_TILE_FRESHNESS_MODE {
    MAP_TILE_FRESHNESS_OFF = 0,
    MAP_TILE_FRESHNESS_SYNC_SCREEN = 1,
    MAP_TILE_FRESHNESS_LIVE = 2,
    MAP_TILE_FRESHNESS_MODE_COUNT
  };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, ROUNDEDRAFF = 3 };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };

  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Sleep screen settings. LIGHT, not upstream's DARK: this fork's screens are
  // black-on-white throughout (map, splash), and a dark sleep image also stays
  // in the controller as the baseline the next boot paints over.
  uint8_t sleepScreen = LIGHT;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (X3 only, requires DS3231 RTC)
  uint8_t statusBarClock = STATUS_BAR_CLOCK_HIDE;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  // Short power button click behaviour
  uint8_t shortPwrBtn = IGNORE;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Map screen ladder state, one entry per travel mode (MapRideMode: 0 ride,
  // 1 hike, 2 cycle) plus the mode itself. Stored per mode on purpose --
  // switching ride to hike and back returns each ladder to what it was
  // (docs/architecture-plan.md, "Marker height is on the bottom buttons").
  //
  // Owned by MapActivity's buttons, not by the Settings screen, so these are
  // out of SettingsList and saved by hand in toJson/fromJson, the same way
  // the front-button remap is.
  //
  // **Every save here is an SD write** -- CrossPointSettings persists to
  // /.crosspoint/settings.json, not to NVS. CLAUDE.md rule 8 is explicit that
  // settings must not be written on every interaction, so MapActivity
  // coalesces presses and saves once when they settle. Do not call
  // saveToFile() per press.
  uint8_t mapZoomStep[kMapRideModeCount] = {kDefaultZoomStepForMode[0], kDefaultZoomStepForMode[1],
                                            kDefaultZoomStepForMode[2]};
  uint8_t mapMarkerStep[kMapRideModeCount] = {kDefaultMarkerStepForMode[0], kDefaultMarkerStepForMode[1],
                                              kDefaultMarkerStepForMode[2]};
  uint8_t mapMode = static_cast<uint8_t>(MapRideMode::Ride);
  // The last position a BLE/console fix actually landed on, so re-entering
  // Map (or a reboot) has something to show before the next fix arrives
  // instead of a blank "waiting" screen. Same coalesced-write rule as the
  // ladder steps above -- MapActivity debounces this, never writes per fix.
  bool mapHasLastFix = false;
  int32_t mapLastLatE7 = 0;
  int32_t mapLastLonE7 = 0;
  uint8_t mapLastHeading = 0;
  // Ask the phone for a tile the moment the map hatches one, instead of
  // waiting for a deliberate trip to the tile sync screen. Off by default:
  // it spends the phone's mobile data without being asked, so it has to be
  // a decision the rider made once, on purpose.
  //
  // In SettingsList (category Map), unlike the ladder state above -- this one
  // IS a Settings-screen toggle, so the generic toJson/fromJson loop carries
  // it and nothing here has to.
  uint8_t mapAutoSyncTiles = 0;
  // Take the map's position from a receiver on the device instead of from the
  // phone, on a board that has one (GnssAccess.h). Off by default, and off is
  // what every shipping device does today: the X4 and the X4 Pro have no
  // receiver at all, so on those this field can only ever be 0 and the code
  // that reads it is not even compiled in.
  //
  // In the settings file but **not** in SettingsList, unlike mapAutoSyncTiles
  // above -- a Settings row would offer every rider a toggle for hardware only
  // one development board has. It is reached from the host instead
  // (CMD:SETTING mapGnssPosition 1, main.cpp), which is all step 3 of
  // ../docs/gnss-to-map-plan.md needs. The day a shipping device carries a
  // receiver, this gets a row and the row gets a board condition.
  uint8_t mapGnssPosition = 0;
  // Write one CSV row per accepted GNSS fix to /trailink/gnss.csv (GnssLog.h).
  // Off by default and it must stay that way: the file is a **track log**, not
  // a single point, so on a lost or stolen device it is a record of where the
  // rider went. Turned on for one measurement, deliberately, and turned off
  // after. Only exists on a build with a receiver.
  uint8_t mapGnssLog = 0;
  // Edge markers for pins outside the viewport: a direction arrow and the
  // distance, drawn where the bearing ray leaves the screen
  // (MapActivity::drawPins(), ../docs/pins.md). Pins *inside* the viewport are
  // always drawn and this does not touch them.
  //
  // **Off by default, and the default does not move until it is measured on the
  // panel** (../docs/pins-plan.md, decision 6). An edge marker's content changes
  // with every fix, so the risk is e-ink drawing at the screen edges all ride:
  // ghosting, battery, a visible flicker per fix. What is built today only draws
  // them as part of a frame that was going to be rendered anyway, which is why
  // this can ship at all before that measurement exists.
  uint8_t mapPinsOffscreen = 0;
  // Per-pin override of the above: bit N is catalogue slot N (PinCatalog.h). All on
  // by default, so turning the master on behaves exactly as it did before this
  // existed. A pin whose key this build does not know has no bit and follows the
  // master.
  //
  // In the settings file but **not** in SettingsList: it is edited from the map's
  // own Pins list (a row per pin reads better than sixteen toggles in a settings
  // screen), so toJson/fromJson carry it by hand.
  //
  // Deliberately not in the pin log. Visibility is a display preference, not
  // something that happened -- and a new log field would mean a v2 record, which
  // every older build skips as an unknown version and would cost a rider their
  // pins (docs/pins.md, "Reading, and damage").
  uint16_t mapPinsOffscreenMask = 0xFFFF;
  // The GPS/tile/BLE readout in the map's top-left corner. Off by default: it
  // is diagnostic text, not something a rider needs on screen. Toggled from
  // the Settings screen (category Map) or live from the map's own menu
  // (MapActivity::openMapMenu()) -- both flip this same field, so the two
  // never disagree about the current value (same pattern as mapZoomMode
  // below).
  uint8_t mapDebugInfo = 0;
  // Zoom/rotation/heading mode settings, all category Map, all default to the
  // behaviour the map screen had before this setting existed.
  uint8_t mapZoomMode = MAP_ZOOM_MANUAL;
  uint8_t mapRotationMode = MAP_ROTATION_HEADING_UP;
  uint8_t mapHeadingMode = MAP_HEADING_AUTO;
  uint8_t mapTileFreshnessMode = MAP_TILE_FRESHNESS_OFF;
  // Whether the device uses the point layer at all: the .tip shards on the card,
  // the marks they draw, and the `Nearby` menu over them
  // (../../docs/point-layer-lifecycle.md, decision 2). One switch for the whole
  // layer, not one per category -- which categories are drawn is a view the map
  // menu owns and it does not survive a reboot (docs/nearby-menu.md).
  //
  // **On by default, and that does not spend data.** When a shard is fetched is
  // mapTileFreshnessMode's question, and that one defaults to Off; reading a
  // shard the card already holds costs card time and no data at all. So this
  // toggle answers "does this device deal in points", nothing else.
  //
  // Off dims the `Nearby` row rather than removing it (MapActivity::openMenu()):
  // a row that vanished reads as a firmware that lost a feature, a dimmed one
  // reads as a switch somebody turned off, which is the truth.
  uint8_t mapPointsEnabled = 1;
  // Reader font settings
  uint8_t fontFamily = NOTOSERIF;
  // Point size of the reader font. Only sizes the active family actually ships
  // are selectable; SdCardFontSystem::ensureLoaded() snaps this to the nearest
  // available size (and persists the snap) whenever the family changes.
  uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  static constexpr uint8_t SCREEN_MARGIN_MIN = 5;
  static constexpr uint8_t SCREEN_MARGIN_MAX = 40;
  static constexpr uint8_t SCREEN_MARGIN_STEP = 5;
  uint8_t screenMargin = SCREEN_MARGIN_MIN;
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // Long-press Confirm function in EPUB reader (cycles through LONG_PRESS_MENU_FUNCTION values).
  // Defaults to Disabled so shortcut-based bookmark toggling remains opt-in.
  uint8_t longPressMenuFunction = LP_MENU_DISABLED;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // Dictionary folder name under /dictionaries (empty = no dictionary)
  char dictionaryName[32] = "";
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Short press Back goes to file browser instead of home (0 = disabled, 1 = enabled)
  uint8_t backShortToFileBrowser = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Touch screen reader zones/gestures on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }
  int getReaderFontId() const;

  // Drop the SD font selection and fall back to the built-in family. The reader
  // point size comes back into BUILTIN_READER_POINT_SIZES with it, since that is
  // the only set a built-in family ships — otherwise the settings UI would keep
  // offering a size nothing renders at. Both fields are persisted in one write.
  void clearSdFontFamily();

  // Resolved status-bar composition. Consumers read the spec; only settings
  // editors read the raw fields.
  //
  // Deliberately NOT built under storeMutex: every field it reads is a single
  // byte, so a concurrent settings write can never produce a corrupt value —
  // only a snapshot mixing pre- and post-change fields. That costs at most one
  // e-ink frame drawn with a mixed status bar, which self-corrects on the next
  // refresh. Locking here would instead put a mutex on the render path and
  // stall it behind the SD write inside saveToFile(). Don't add one back.
  struct StatusBarSpec {
    bool showChapterPageCount = false;
    bool showBookProgressPercent = false;
    uint8_t titleMode = HIDE_TITLE;  // STATUS_BAR_TITLE
    bool showBattery = false;
    bool showBatteryPercent = false;
    uint8_t clockMode = STATUS_BAR_CLOCK_HIDE;  // STATUS_BAR_CLOCK_MODE
    bool clock12h = false;
    uint8_t clockUtcOffsetQ = 48;             // 48 = UTC+0
    uint8_t progressBarMode = HIDE_PROGRESS;  // STATUS_BAR_PROGRESS_BAR
    uint8_t progressBarHeightPx = 0;          // (thickness+1)*2; 0 when the bar is hidden
    uint8_t xtcMode = XTC_STATUS_BAR_HIDE;    // XTC_STATUS_BAR_MODE

    bool showsProgressBar() const { return progressBarMode != HIDE_PROGRESS; }
    bool showsTitle() const { return titleMode != HIDE_TITLE; }
    bool showsClock() const { return clockMode != STATUS_BAR_CLOCK_HIDE; }
    // Visibility of the text lane. Clock hardware presence is the caller's
    // concern: pass halClock.isAvailable(), or true for layout reservation.
    bool textLaneVisible(bool clockAvailable) const {
      return showChapterPageCount || showBookProgressPercent || showsTitle() || showBattery ||
             (showsClock() && clockAvailable);
    }
  };
  StatusBarSpec statusBarSpec() const;

  // Resolved text-rendering configuration for the Epub layout engine. The
  // viewport is renderer/orientation-derived, so the caller supplies it —
  // passing it in keeps a spec from ever existing in a half-filled state.
  // Unlocked for the same reason as statusBarSpec(); see the note above.
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
