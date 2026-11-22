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
    return;  // activate() may have started another activity; nothing more here
  }

  // Last, after input is dispatched, exactly where MapActivity::loop() calls
  // its own updateHeaderStatus(). Called first, its frame guard would still
  // read "no update pending" for a press that has already been latched this
  // pass, and the band would be repainted a moment before the full frame that
  // press asks for.
  //
  // Every early return above either asked for a frame or handed off to another
  // activity, with one exception: a touch Down on the row that is *already*
  // selected returns having done nothing. A finger resting there skips this
  // poll for as long as it rests, which costs a late battery reading and
  // nothing else.
  updateBatteryStatus();
}

Rect HomeActivity::headerRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Ends above the brand block: the band is [topPadding, topPadding +
  // homeTopPadding) = [5, 61) in Lyra and the logo starts at homeTopPadding +
  // kHeaderGap = 64, so a window over this rect cannot clip the mountain art.
  return Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.homeTopPadding};
}

void HomeActivity::recordDrawnBattery() {
  // Asks the theme what it just drew instead of reading the battery again.
  // The reads live inside drawBatteryRight(), and on a board with no fuel
  // gauge getBatteryPercentage() is uncached and steps an EMA on every call
  // (HalPowerManager.cpp), so a number read here would be a different number
  // from the one on the glass -- and since this value is also what the next
  // poll compares against, the panel would sit on a percentage nothing ever
  // corrected.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const BaseTheme::BatteryReading drawn = GUI.lastBatteryDrawn();
  drawnBatteryPercent_ = drawn.percentage;
  drawnCharging_ = drawn.charging;
  drawnBatteryStep_ = GUI.batteryIconStep(drawn.percentage, metrics.batteryWidth, drawn.charging);
}

// True when `deadline` has not been reached yet. Subtraction, not `now <
// deadline`: the difference is what survives millis() wrapping at 49.7 days.
static bool beforeDeadline(uint32_t now, uint32_t deadline) {
  return deadline != 0 && static_cast<int32_t>(now - deadline) < 0;
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
  // Same pair as MapActivity::frameInFlight(). **This is also why the call
  // site is at the end of loop(), after input is dispatched**: called first,
  // the flag is still false for a press that has already been latched, and
  // the band gets repainted a moment before the full frame that press asks
  // for.
  if (activityManager.updatePending() || RenderLock::peek()) return;

  // Polled rather than read every pass, for one reason and not the two an
  // earlier version of this comment claimed: on a gaugeless board
  // getBatteryPercentage() is an ADC read that also steps an EMA filter, so
  // asking at loop rate would both cost the read and move the filter. The
  // charge line is *not* a reason -- HalGPIO::update() already asks it once
  // per main-loop iteration, and HalPowerManager::isCharging() caches it for
  // CHARGING_POLL_MS precisely because of that. Same interval as the map's
  // kHeaderPollMs.
  const uint32_t now = millis();
  if (beforeDeadline(now, nextBatteryPollMs_)) return;
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
  const auto levelDiffers = [&]() { return textShown ? percentage != drawnBatteryPercent_ : step != drawnBatteryStep_; };
  if (!levelDiffers()) return;

  // **The charging bolt is deliberately not a condition here.** It looks like
  // the obvious second trigger and it is already somebody else's job:
  // HalGPIO::update() raises a one-iteration usbStateChanged on every plug and
  // unplug, and main.cpp asks for a full frame on it -- "Refresh the battery
  // icon when USB is plugged or unplugged" -- before activityManager.loop()
  // even runs. A branch here would never fire: by the time this poll comes
  // round, that frame has redrawn the band and recorded what it drew.
  //
  // This cost a round of hardware testing to learn. Three cable cycles across
  // two boards were read as proof that the repaint path works, and every one
  // of them was the main.cpp path answering in ~10 ms. So the level is the
  // whole of what this function adds, and it is the half nothing else covers.
  //
  // A floor under it, because the level has no hysteresis at the source -- a
  // gaugeless board reports a truncated EMA that can alternate on an integer
  // boundary -- and on this hardware a repaint of this band is a whole-panel
  // pass on most drivers (see the window comment below). The first change
  // after a quiet period always goes through at once: the floor is armed on
  // repaint, not on the change.
  if (beforeDeadline(now, nextLevelRepaintMs_)) return;

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
  //
  // RenderLock first, power lock second -- the same order ActivityManager's
  // render task takes them in, and the order is not cosmetic.
  // HalPowerManager::Lock refuses to nest: a second one finds
  // currentLockMode != None, logs "Lock already held, ignore" and comes back
  // **invalid**, which means it does not pin the clock
  // (HalPowerManager.cpp). Taken the other way round, a repaint that lands
  // while the render task is mid-frame gets exactly that -- an error line and
  // a panel pass on a throttled clock, which is what the lock was added to
  // prevent. Holding RenderLock first makes it impossible: whoever holds it
  // is not rendering, so the power lock is free.
  //
  // The lock is not optional either. ActivityManager wraps every render() in
  // one so a panel pass never runs on a throttled clock, and this is the same
  // kind of pass from the other task -- on Home, idle, which is exactly when
  // the main loop has already dropped to LOW_POWER_FREQ (10 MHz on a board
  // with no PSRAM, HalPowerManager.h).
  RenderLock lock;
  HalPowerManager::Lock powerLock;
  // Taking the lock can wait out a whole frame, and that frame redraws this
  // band and records what it drew. Re-ask against the refreshed state -- in
  // the same terms the decision above used, the printed number or the glyph
  // step, never both -- instead of trusting a decision taken before the wait.
  if (!levelDiffers()) return;

  // Armed here and not at the gate above, so a pass that turns out to be
  // unnecessary -- the re-check just took that exit -- does not hold a real
  // change off for the next thirty seconds.
  nextLevelRepaintMs_ = now + kLevelRepaintFloorMs;

  const Rect header = headerRect();
  GUI.drawHeader(renderer, header, nullptr);
  // What the theme actually drew, and before the refresh: the refresh blocks
  // for as long as the waveform takes and the battery moves underneath it.
  recordDrawnBattery();

  // Windowed, which is a scoped refresh only on a driver that implements one.
  // Ssd1677Driver and PaperMonoDriver do; every other panel inherits
  // PanelDriver::displayWindow()'s base body, which drops the rectangle and
  // pushes a whole Fast frame (freeink-sdk .../driver/PanelDriver.h, and
  // HalDisplay::displayWindow()'s own comment; the mechanism and its measured
  // cost are in docs/t5s3-partial-refresh.md, section 1). So on the T5 S3 Pro
  // (LgfxEpdDriver) and the X4 Pro (Uc8279X4Driver) this costs a full-panel
  // Fast pass -- which is the real reason the conditions above are as tight as
  // they are, and why both signals carry a floor.
  if (!renderer.displayBufferWindow(header.x, header.y, header.width, header.height,
                                    PowerTelemetry::WindowSite::Status)) {
    LOG_ERR(kLogTag, "battery header window rejected: %d,%d %dx%d", header.x, header.y, header.width, header.height);
    // The record above already says this value is on the glass, and a refused
    // window means it is not -- so without a fallback the panel would keep the
    // old number and every later poll would agree with itself. Same answer
    // GnssAcquireActivity::refreshBand() gives a refused window.
    renderer.displayBuffer();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, headerRect(), nullptr);
  // Straight after the draw, and before displayBuffer() blocks for the whole
  // waveform: recordDrawnBattery() takes the theme's own record of what it
  // drew, so this is the frame updateBatteryStatus() compares against.
  recordDrawnBattery();

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
