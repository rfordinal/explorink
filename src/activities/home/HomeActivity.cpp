#include "HomeActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <PowerTelemetry.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/home_icons.h"
#include "fontIds.h"
#include "images/HomeHeader.h"

namespace {
// The brand block: mountain line art with the logo mark and the wordmark over
// it, one bitmap (src/images/home-header.svg, scripts/gen_home_header.py).
// Drawn with drawMono1bpp(), not drawImage(): drawImage() rotates the origin and
// not the bits, and in Portrait a 480-tall pre-rotated buffer at x = 0 lands on
// `(479 - 0) - 480 == -1`, which the blit reads as a uint16_t and drops. That is
// measured, not read -- the first hardware pass drew no header at all
// (../../../docs/home-screen.md).
constexpr int kHeaderGap = 8;
// Home's own row height, not metrics.menuRowHeight (64 in Lyra): seven rows at 64
// end 16 px above the button hints, which reads as the list running into them.
// At 60 the list ends at 716 with 44 px of air, and a 32 px glyph still has room.
constexpr int kRowHeight = 60;

// How often the header's battery state is looked at
// (HomeActivity::updateBatteryStatus()). Same figure as the map's
// kHeaderPollMs, and for the same reason: it bounds how often an ADC read and
// an I2C charge-line read are asked for, and two seconds is still prompt for a
// cable that was just plugged in.
constexpr uint32_t kBatteryPollMs = 2000;

// And the floor between two repaints caused by nothing but the battery level
// moving. A plug or an unplug ignores it -- that one is caused by the rider
// and is never cosmetic. Same figure as the map's kHeaderBarsRepaintMs, and
// the same reason: the quantity can dither (a gaugeless board's percentage is
// a truncated EMA with no hysteresis) and every flip is a real waveform pass,
// a whole-panel one on a driver with no scoped window.
constexpr uint32_t kLevelRepaintFloorMs = 30 * 1000;

constexpr const char* kLogTag = "Home";
}  // namespace

// The menu, in the order it is drawn. Flash-resident: static constexpr, so the
// table costs no DRAM.
//
// Every glyph here is Lucide, as the icon rule asks (the parent repo's CLAUDE.md,
// "Icons: prefer Lucide"): backpack, map-pin, wallet, refresh-cw, wifi, settings,
// and chevron-right for the row arrow. None is hand-drawn, and none of the older
// hand-rolled bitmaps (components/icons/wifi.h, settings2.h, transfer.h,
// bookmark.h) is used on this screen any more.
//
// `icon_explore` is the one that is not Lucide, on purpose: it is the ExplorInk
// brand mark (src/images/logo.svg), asked for by the maintainer 2026-08-21. A
// brand mark has no Lucide equivalent.
//
// They are baked by scripts/gen_home_icons.py rather than the SDK's
// libs/assets/Icons/tools/gen_icons.py -- same freeink::Icon output, but the SDK
// script needs rsvg-convert (absent here) and is square-only, and the mark is
// 32x31. components/icons/home_icons.h carries the full reasoning.
const HomeActivity::Row* HomeActivity::rows() {
  static constexpr Row kRows[] = {
      {HomeMenuItem::MAP, StrId::STR_HOME_EXPLORE, &icon_explore},
      {HomeMenuItem::TRIPS, StrId::STR_HOME_TRIPS, &icon_trips},
#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH
      // Who else is out there. **Named for the feature and not for the radio**,
      // 2026-10-30: the row said "LoRa nodes" and the map menu said "Group"
      // for the same people, and a rider has no reason to know which radio
      // carried a position -- the group layer takes them from LoRa, from a
      // phone relay over BLE and from the console alike
      // (../../../docs/group-markers.md, "The layer does not know which radio
      // spoke").
      //
      // **Third, under Trips** (maintainer's call, 2026-10-30), not down by
      // Settings where it first landed. The top of this list is what the rider
      // reaches for before walking out: where am I, which trip, who is with
      // me. Sync and File Transfer are desk jobs done once, and the bench row
      // below them is not a rider's row at all -- putting the group behind
      // them ranked it as plumbing.
      //
      // Still gated on ENABLE_LORA_MESH rather than ENABLE_GROUP_MARKERS,
      // because what this screen *does* is still LoRa: two of its three
      // sections are the radio's controls and the nodes it hears. A board with
      // the group layer and no radio (x4pro, simulator) keeps its roster over
      // the console, and gets the Settings tab without this row.
      // The badge says LoRa even though the row does not, and the two are not
      // in tension: the *name* is the feature, because the layer is
      // transport-neutral, while the *badge* is about this screen, which
      // really is the radio's -- its controls, and the nodes it hears. A
      // rider who has to find where the radio lives has one place to look.
      {HomeMenuItem::GROUP, StrId::STR_GROUP_TITLE, &icon_group, StrId::STR_LORA_BADGE, &icon_lora_badge},
#endif
      {HomeMenuItem::TILE_SYNC, StrId::STR_HOME_SYNC, &icon_sync},
      {HomeMenuItem::FILE_TRANSFER, StrId::STR_FILE_TRANSFER, &icon_transfer},
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
      // The grayscale bench is a build-flag lab instrument, not a rider's row
      // (platformio.ini).
      {HomeMenuItem::PREVIEW, StrId::STR_PREVIEW, &icon_settings},
#endif
      {HomeMenuItem::SETTINGS_MENU, StrId::STR_SETTINGS_TITLE, &icon_settings},
  };
  static_assert(sizeof(kRows) / sizeof(kRows[0]) == kRowCount, "kRowCount must match the table");
  return kRows;
}

int HomeActivity::indexOf(const HomeMenuItem item) {
  const Row* table = rows();
  for (int i = 0; i < kRowCount; ++i) {
    if (table[i].item == item) return i;
  }
  return 0;
}

int HomeActivity::stepRow(const int from, const int step) { return (from + step + kRowCount) % kRowCount; }

void HomeActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : indexOf(initialMenuItem);

  // Nothing of this screen is on the panel yet, whatever the last visit left
  // in these. The windowed repaint in updateBatteryStatus() is gated on
  // headerDrawn_ for exactly this instant.
  headerDrawn_ = false;
  drawnBatteryStep_ = -1;
  drawnBatteryPercent_ = 0;
  drawnCharging_ = false;
  nextBatteryPollMs_ = 0;
  nextLevelRepaintMs_ = 0;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::activate(const int index) {
  if (index < 0 || index >= kRowCount) return;
  switch (rows()[index].item) {
    case HomeMenuItem::FILE_TRANSFER:
      onFileTransferOpen();
      break;
    case HomeMenuItem::MAP:
      onMapOpen();
      break;
    case HomeMenuItem::TRIPS:
      onTripsOpen();
      break;
    case HomeMenuItem::TILE_SYNC:
      onTileSyncOpen();
      break;
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
    case HomeMenuItem::PREVIEW:
      onPreviewOpen();
      break;
#endif
#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH
    case HomeMenuItem::GROUP:
      onGroupOpen();
      break;
#endif
    case HomeMenuItem::SETTINGS_MENU:
      onSettingsOpen();
      break;
    default:
      break;
  }
}

void HomeActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int count = kRowCount;

  // Before the input handling, and unconditional: this is the one condition on
  // this screen that fires while the rider is doing nothing at all, which is
  // the whole case it exists for.
  updateBatteryStatus();

  buttonNavigator.onNext([this] {
    selectorIndex = stepRow(selectorIndex, 1);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectorIndex = stepRow(selectorIndex, -1);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = stepRow(selectorIndex, 1);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = stepRow(selectorIndex, -1);
    requestUpdate();
    return;
  }

  // Rows are contiguous now (the brand block eats the space the old menu spent
  // on gaps), so the touch step is the row height itself.
  const int menuTop = metrics.homeTopPadding + kHeaderGap + HOMEHEADER_HEIGHT + kHeaderGap;
  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, kRowHeight, count, 0, INT32_MAX, kRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != menuRow) {
        selectorIndex = menuRow;
        requestUpdate();
      }
    } else {
      selectorIndex = menuRow;
      activate(menuRow);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selectorIndex);
  }
}

Rect HomeActivity::headerRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Ends above the brand block: the band is [topPadding, topPadding +
  // homeTopPadding) = [5, 61) in Lyra and the logo starts at homeTopPadding +
  // kHeaderGap = 64, so a window over this rect cannot clip the mountain art.
  return Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.homeTopPadding};
}

void HomeActivity::recordDrawnBattery(uint16_t percentage, bool charging) {
  // Takes what the caller read rather than reading again. Two reasons, and the
  // second one is the expensive one:
  //
  // - A re-read answers a later moment than the one the glyph was drawn from,
  //   and a repaint can sit behind a blocking panel refresh of well over a
  //   second. The first version recorded after displayBuffer() and could store
  //   "not charging" while the bolt was on the glass, which then stayed there
  //   until the percentage happened to move.
  // - getBatteryPercentage() is only cached on a board with a fuel gauge
  //   (HalPowerManager.cpp -- gaugeAddr != 0). On a gaugeless board, the plain
  //   X4 among them, every call is a fresh ADC read **and a step of an EMA
  //   filter**, so three calls in one pass return three different numbers and
  //   change the filter three times.
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawnBatteryPercent_ = percentage;
  drawnBatteryStep_ = GUI.batteryIconStep(percentage, metrics.batteryWidth, charging);
  drawnCharging_ = charging;
}

void HomeActivity::updateBatteryStatus() {
  // Nothing to patch before the first frame exists.
  if (!headerDrawn_) return;
  // Nothing to draw at all on a theme whose drawHeader() ignores a null title:
  // the band would be refreshed with no battery in it.
  if (!GUI.drawsHeaderWithoutTitle()) return;
  // And stand aside for a frame -- both halves of that. RenderLock::peek()
  // answers "being composed right now"; activityManager.updatePending()
  // answers "asked for and not started yet", which peek() cannot see because
  // the render task is notified only at the end of ActivityManager::loop().
  // Without the second half a button press on Home spends a windowed pass on
  // a band the full frame behind it overwrites a moment later. Same pair as
  // MapActivity::frameInFlight().
  if (activityManager.updatePending() || RenderLock::peek()) return;

  // Polled rather than read every pass: on a board with no usbDetect pin the
  // charge question is an I2C transaction on the bus the touch controller
  // shares, and on a gaugeless board the percentage is an ADC read that also
  // steps a filter. Same interval as the map's kHeaderPollMs, and prompt
  // enough that a plugged cable lights the bolt within two seconds.
  const uint32_t now = millis();
  if (nextBatteryPollMs_ != 0 && now < nextBatteryPollMs_) return;
  nextBatteryPollMs_ = now + kBatteryPollMs;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const bool charging = gpio.isUsbConnected();
  const int step = GUI.batteryIconStep(percentage, metrics.batteryWidth, charging);

  // Two different readers of this glyph, and they move at different rates.
  // With the number printed, a rider reads the number, so one percent is a
  // real change. With it hidden the icon is all there is, and that has four
  // states in Lyra -- repainting it per percent would buy nothing and cost a
  // waveform pass each time.
  const bool textShown = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const bool levelMoved = textShown ? percentage != drawnBatteryPercent_ : step != drawnBatteryStep_;
  // A plug or an unplug is instantaneous and the bolt appears or goes with it,
  // unlike the percentage this interval was sized for. Not rate-capped beyond
  // the poll: it is caused by the rider and is never cosmetic.
  const bool chargingChanged = charging != drawnCharging_;
  if (!levelMoved && !chargingChanged) return;

  // A floor under the level, and none under the bolt. The level is the one
  // that can dither: a gaugeless board reports a truncated EMA with no
  // hysteresis, so a value sitting on an integer boundary can alternate every
  // poll, and **on this hardware a repaint of this band is not cheap** -- see
  // the window comment below. Thirty seconds is the same floor the map puts
  // under its signal bars, and for the same reason.
  if (levelMoved && !chargingChanged && nextLevelRepaintMs_ != 0 && now < nextLevelRepaintMs_) return;

  // The header band goes through GUI.drawHeader() and nothing else, exactly as
  // render() draws it. The active theme's drawHeader() **clears the full width
  // of the rect it is given before drawing anything** -- LyraTheme.cpp opens
  // with fillRect(rect.x, rect.y, rect.width, rect.height), all 480 columns,
  // while only BaseTheme confines itself to an 80px battery box. A hand-rolled
  // "just repaint the battery" path here would have to reproduce that order and
  // keep reproducing it; the map paid for learning that on hardware on
  // 2026-08-15 (docs/map-header-status.md, "The clock").
  //
  // Home passes no title, so the band holds the battery and nothing else and
  // the clear costs nothing else on this screen.
  RenderLock lock;
  // Taking the lock can wait out a whole frame, and that frame redraws this
  // band and records what it drew. Re-ask against the refreshed state instead
  // of trusting the decision taken before the wait -- otherwise the pass just
  // waited for is followed by a second one that changes nothing.
  if (percentage == drawnBatteryPercent_ && step == drawnBatteryStep_ && charging == drawnCharging_) return;

  // Armed here and not at the gate above, so a pass that turns out to be
  // unnecessary -- the re-check just took that exit -- does not hold a real
  // change off for the next thirty seconds.
  if (levelMoved) nextLevelRepaintMs_ = now + kLevelRepaintFloorMs;

  const Rect header = headerRect();
  GUI.drawHeader(renderer, header, nullptr);
  // Before the refresh, never after it: the refresh blocks for as long as the
  // waveform takes and the battery can move underneath it.
  recordDrawnBattery(percentage, charging);

  // Windowed, which is a scoped refresh only on a driver that implements one.
  // Ssd1677Driver and PaperMonoDriver do; every other panel inherits
  // PanelDriver::displayWindow()'s base body, which drops the rectangle and
  // pushes a whole Fast frame (freeink-sdk .../driver/PanelDriver.h, and
  // HalDisplay::displayWindow()'s own comment). So on the T5 S3 Pro
  // (LgfxEpdDriver) and the X4 Pro (Uc8279X4Driver) this costs a full-panel
  // Fast pass -- which is the real reason the conditions above are as tight as
  // they are, and why the level carries a floor.
  if (!renderer.displayBufferWindow(header.x, header.y, header.width, header.height,
                                    PowerTelemetry::WindowSite::Status)) {
    LOG_ERR(kLogTag, "battery header window rejected: %d,%d %dx%d", header.x, header.y, header.width, header.height);
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Read once and record what this frame's header is drawn from. The read has
  // to bracket the draw and not the refresh: displayBuffer() below blocks for
  // the whole waveform, and a value read after it is a later moment than the
  // pixels (recordDrawnBattery()).
  const uint16_t headerPercent = powerManager.getBatteryPercentage();
  // gpio.isUsbConnected(), not powerManager.isCharging(): this has to be the
  // same question fillBatteryIcon() asks, or the bolt on the panel and the
  // bolt this tracks would be two different facts.
  const bool headerCharging = gpio.isUsbConnected();
  GUI.drawHeader(renderer, headerRect(), nullptr);
  recordDrawnBattery(headerPercent, headerCharging);

  const int headerTop = metrics.homeTopPadding + kHeaderGap;
  renderer.drawMono1bpp(HomeHeader, (pageWidth - HOMEHEADER_WIDTH) / 2, headerTop, HOMEHEADER_WIDTH, HOMEHEADER_HEIGHT,
                        true);

  const int menuTop = headerTop + HOMEHEADER_HEIGHT + kHeaderGap;

  // The theme draws rows, not menu items: 5 x 8 bytes of stack, well inside the
  // 256-byte local budget (../../../CLAUDE.md, The Resource Protocol).
  const Row* table = rows();
  BaseTheme::HomeRow drawRows[kRowCount];
  for (int i = 0; i < kRowCount; ++i) {
    drawRows[i] = BaseTheme::HomeRow{
        I18n::getInstance().get(table[i].label), table[i].icon,
        table[i].badgeLabel == StrId::STR_NONE_OPT ? nullptr : I18n::getInstance().get(table[i].badgeLabel),
        table[i].badgeIcon};
  }
  GUI.drawHomeMenu(renderer, Rect{0, menuTop, pageWidth, pageHeight - menuTop - metrics.buttonHintsHeight}, drawRows,
                   kRowCount, selectorIndex, kRowHeight);

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  // Last, and after the three drawn* stores above: this is the flag
  // updateBatteryStatus() gates on, so publishing it first would let the next
  // loop pass compare against values from the previous frame.
  headerDrawn_ = true;
}

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

// Straight to the map, no picker -- that is now Trips' job (onTripsOpen()).
// The satellite wait sits in front of the map on a build with a receiver, and
// falls through to the map itself on every other build and setting
// (ActivityManager::goToGnssAcquire()).
void HomeActivity::onMapOpen() { activityManager.goToGnssAcquire(); }

// The trip picker lives here, not on a separate row: Trips is where a rider
// already looks for "which trip", so it opens RouteSelectActivity rather than
// its own screen.
void HomeActivity::onTripsOpen() { activityManager.goToRouteSelect(); }

void HomeActivity::onTileSyncOpen() { activityManager.goToTileSync(); }

#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
void HomeActivity::onPreviewOpen() { activityManager.goToPreview(); }
#endif
#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH
void HomeActivity::onGroupOpen() { activityManager.goToGroup(); }
#endif
