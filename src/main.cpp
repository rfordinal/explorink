#include <Arduino.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <GrayscaleFrame.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <PowerTelemetry.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "MissingTilesStore.h"
#include "OpdsServerStore.h"
#include "PowerLog.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "activities/wallet/WalletAsset.h"
#include "activities/wallet/WalletCryptoDevice.h"
#include "activities/wallet/WalletKeyStore.h"
#include "activities/wallet/WalletStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

// Chunked, retrying write for a bulk buffer over logSerial. A single
// logSerial.write() call gives up as soon as HWCDC's TX ring buffer (256
// bytes by default) fills and the host doesn't drain it inside the ~1ms
// window set by setTxTimeoutMs(1) in setup() -- confirmed on real hardware
// (docs/debug-screenshot-channel-plan.md): a 48,000-byte write() returned
// anywhere from ~300 bytes to the full count, unpredictably. That per-call
// timeout is load-bearing elsewhere and stays as-is; this loop just retries
// the remainder across many short calls instead of trusting one to finish,
// bounded by a total timeout so a genuinely dead link still returns.
size_t writeAllChunked(uint8_t* data, size_t len, uint32_t totalTimeoutMs) {
  size_t sent = 0;
  const unsigned long deadline = millis() + totalTimeoutMs;
  while (sent < len) {
    const int avail = logSerial.availableForWrite();
    if (avail <= 0) {
      if (static_cast<long>(millis() - deadline) >= 0) break;
      delay(2);
      continue;
    }
    const size_t want = static_cast<size_t>(avail) < (len - sent) ? static_cast<size_t>(avail) : (len - sent);
    const size_t written = logSerial.write(data + sent, want);
    sent += written;
    if (written == 0) {
      if (static_cast<long>(millis() - deadline) >= 0) break;
      delay(2);
    }
  }
  return sent;
}

// Plane bands from GrayscaleFrame::replayPlanes, straight onto the wire in the
// order they arrive (LSB plane first, then MSB, each band in y order). Counts
// what actually went out so the handler can report a truncated dump.
static size_t screenshotPlaneBytes = 0;
static void screenshotPlaneSink(void*, bool, const uint8_t* rows, int, int numRows) {
  const size_t len = static_cast<size_t>(display.getDisplayWidthBytes()) * static_cast<size_t>(numRows);
  screenshotPlaneBytes += writeAllChunked(const_cast<uint8_t*>(rows), len, /*totalTimeoutMs=*/3000);
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  // The wallet key goes first, before anything here can fail or take a lock. Deep
  // sleep is a chip reset on wake, so RAM does not survive it -- but "the RAM is
  // gone anyway" is an argument about what usually happens, and a key is wiped
  // because it is asked to be, not because something else probably did it
  // (docs/wallet-crypto.md, "The key's lifetime").
  wallet::Session::instance().clear("sleep");

  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  // Read from the live activity, before goToSleep() tears it down, for the same
  // reason lastSleepFromReader is: afterwards there is nothing left to ask.
  APP_STATE.lastSleepActivity =
      activityManager.isMapActivity() ? CrossPointState::SLEEP_ACTIVITY_MAP : CrossPointState::SLEEP_ACTIVITY_OTHER;

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  BoardConfig::holdPowerRails();

  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
  // Default TX ring buffer is 256 bytes (HWCDC::begin()). CMD:SCREENSHOT
  // dumps the 48,000-byte framebuffer through writeAllChunked(), which
  // retries around the 1ms timeout above rather than needing a bigger
  // buffer to work at all -- but 256 bytes means ~190 chunks minimum even
  // when the host keeps up. 4096 cuts that to ~12 and costs 3,840 bytes of
  // heap, negligible next to the ~118KB free heap this build reports.
  logSerial.setTxBufferSize(4096);
  // The mirror of the above, for CMD:SHOWIMAGE reading a 48,000-byte
  // framebuffer *in*. The read loop drains whatever `available()` reports, so a
  // 256-byte ring works but has to be serviced ~190 times with no slack; a host
  // burst that outruns one pass is dropped by the USB stack, not queued. Same
  // 3,840 bytes of heap for the same reason.
  logSerial.setRxBufferSize(4096);
#endif
#endif

  HalSystem::begin();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  // Return value read, unlike the stores above, because this one guards a file the
  // rider's queued squares live in: a first boot has no file (normal) and an
  // unreadable card looks the same from here, so it is said out loud either way.
  // MissingTilesStore::flushIfDirty() is what refuses to overwrite in that state.
  if (!MISSING_TILES.loadFromFile()) {
    LOG_INF("MAIN", "missing tile list not read (no file yet, or unreadable) -- not saving over it this run");
  }
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      if (!gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP)) {
        powerManager.startDeepSleep(gpio);
      }
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting TrailInk version " TRAILINK_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;

  // Resume straight back into the map when that is where the sleep came from. Held
  // off by a held Back button (the same escape hatch the reader resume has) and by
  // the load-count guard, so firmware that cannot get through MapActivity::onEnter
  // cannot trap the device in a wake-crash-wake loop.
  const bool resumeIntoMap =
      resume == BootResume::QuickResume && APP_STATE.lastSleepActivity == CrossPointState::SLEEP_ACTIVITY_MAP &&
      APP_STATE.mapActivityLoadCount == 0 && !mappedInputManager.isPressed(MappedInputManager::Button::Back);
  if (resume == BootResume::QuickResume && !resumeIntoMap &&
      APP_STATE.lastSleepActivity == CrossPointState::SLEEP_ACTIVITY_MAP) {
    LOG_INF("MAIN", "wake into map declined (loadCount=%u, back=%d)",
            static_cast<unsigned>(APP_STATE.mapActivityLoadCount),
            static_cast<int>(mappedInputManager.isPressed(MappedInputManager::Button::Back)));
  }

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (resumeIntoMap) {
        // No paint here. MapActivity's entry frame is a whole-panel HALF
        // (pendingEntryCleanRefresh_) that rewrites every pixel this would have
        // drawn, so painting first would spend 1,684 ms on a frame with a lifetime
        // of a few seconds -- and the panel is not blank meanwhile: e-ink holds the
        // sleep screen, i.e. the map with its moon, until the live map lands on it.
        // loadSleepFrameBuffer() still runs, for its other job: it removes
        // sleep_frame.bin, and a file left behind would be restored by some later,
        // unrelated quick resume.
        (void)loadSleepFrameBuffer();
      } else if (loadSleepFrameBuffer()) {
        const bool useDifferentialRefresh = gpio.deviceIsX3();
        if (useDifferentialRefresh) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline before replacing the moon with the loading icon.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }

        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        if (useDifferentialRefresh) {
          renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
          allowFastInitialReaderRefresh = true;
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (resumeIntoMap) {
    // Counted up before entering, cleared by MapActivity's first loop() tick. A
    // wake that never reaches that tick leaves the count standing, and the next
    // wake declines and lands on Home -- the same contract readerActivityLoadCount
    // has below.
    APP_STATE.mapActivityLoadCount++;
    APP_STATE.saveToFile();
    const auto& routePath = APP_STATE.lastSleepRoutePath;
    LOG_INF("MAIN", "wake into map, route \"%s\"", routePath.c_str());
    activityManager.goToMap(routePath.empty() ? nullptr : routePath.c_str(),
                            /*resumedFromSleep=*/true);
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

// --- CMD:WALLETBENCH -------------------------------------------------------
//
// Measures what a design-B wallet window actually costs off the card. Design B
// stores one whole-page image per zoom level in panel-native order and blits an
// arbitrary window out of it, which turns one 48,000-byte sequential read into
// 480 strided reads of one panel row each. Whether that is affordable is the
// only thing standing between B and pre-cut overlapping tiles
// (docs/wallet-viewer.md, "Whole-screen paging was rejected").
//
// Three modes, same file, same window:
//
//   windowed    480 reads of rowBytes at `stride`, straight into the framebuffer.
//               Design B's real per-frame cost.
//   sequential  one read of the whole framebuffer from the payload start. The
//               baseline the ratio is against -- what tiles cost today.
//   oversized   the same 480 rows, but pulling 512 bytes per row and keeping the
//               rowBytes that matter. Asked whether the card's block size
//               dominates; answered something else and more useful -- measured at
//               613 ms against 283, because a 512-byte read over a 322-byte stride
//               overshoots the row and every read is followed by a *backward*
//               seek. Seek direction costs, block size did not enter into it
//               (docs/wallet-viewer.md, "Two beliefs the numbers corrected").
//   stream      the window's whole row range read sequentially, no seeks at all:
//               rows * stride bytes through a small chunk buffer, lifting rowBytes
//               out of each row on the way past. 480 x 322 = 154,560 bytes instead
//               of 48,000, but at the sequential rate rather than the strided one.
//               Estimated ~211 ms against the measured 283. A candidate, not the
//               shipped path -- the shipped path is `windowed` until this is
//               measured to beat it.
//
// What it does NOT measure, and must not be read as measuring: any decrypt (P1
// has none), any panel refresh (deliberately outside the timed section -- a FAST
// waveform is 500 ms and would swamp the card entirely), the cost of opening the
// file (opened once, outside the timing, which is what design B would do too),
// more than one file, or more than one card.
//
// Usage: CMD:WALLETBENCH <path-under-/trailink> <stride> <x> <y> <iters>
//   stride  bytes per row of the source image (not the panel's rowBytes)
//   x, y    window origin in native pixels; x must be a multiple of 8, because
//           an 8-aligned window is the whole reason B needs no bit rotation
//   iters   1..kWalletBenchMaxIters
namespace {

constexpr int kWalletBenchMaxIters = 32;
constexpr size_t kWalletBenchOversizedRead = 512;
constexpr int kWalletBenchModes = 4;

// Both static, not stack. The 512-byte buffer is over the 256-byte guidance for a
// stack buffer in this tree, and the sample array wants to outlive the RenderLock
// scope so the summary can be printed with the lock released. 512 + 4 * 32 * 4 =
// 1,024 bytes of .bss, and no heap at all -- the point of the exercise is the
// card, not the allocator. Modes 3 and 4 share the one buffer; they never run at
// the same time.
uint8_t walletBenchScratch[kWalletBenchOversizedRead];
uint32_t walletBenchSamples[kWalletBenchModes][kWalletBenchMaxIters];

uint32_t walletBenchMedian(uint32_t* samples, const int count) {
  // Insertion sort in place: count is at most 32 and this runs once, after the
  // timed section.
  for (int i = 1; i < count; ++i) {
    const uint32_t key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      --j;
    }
    samples[j + 1] = key;
  }
  return count % 2 == 1 ? samples[count / 2] : (samples[count / 2 - 1] + samples[count / 2]) / 2;
}

void walletBenchReport(const char* mode, uint32_t* samples, const int count, const size_t payloadBytes,
                       const size_t readBytes) {
  uint32_t total = 0;
  for (int i = 0; i < count; ++i) total += samples[i];
  const uint32_t lo = *std::min_element(samples, samples + count);
  const uint32_t hi = *std::max_element(samples, samples + count);
  const uint32_t med = walletBenchMedian(samples, count);  // sorts in place; do this last
  const double kbps = med > 0 ? (static_cast<double>(payloadBytes) * 1000.0) / static_cast<double>(med) : 0.0;
  logSerial.printf(
      "WALLETBENCH mode=%s total_ms=%.2f min_ms=%.2f med_ms=%.2f max_ms=%.2f payload_kbps=%.1f read_bytes=%u\n", mode,
      total / 1000.0, lo / 1000.0, med / 1000.0, hi / 1000.0, kbps, static_cast<unsigned>(readBytes));
}

void walletBenchRun(const char* relPath, const uint32_t stride, const uint32_t originX, const uint32_t originY,
                    int iters) {
  if (iters < 1) iters = 1;
  if (iters > kWalletBenchMaxIters) iters = kWalletBenchMaxIters;

  if (!renderer.hasFrameBuffer()) {
    logSerial.printf("WALLETBENCH_ERR no framebuffer\n");
    return;
  }
  const uint32_t rowBytes = display.getDisplayWidthBytes();
  const uint32_t rows = display.getDisplayHeight();
  const size_t bufferBytes = display.getBufferSize();
  uint8_t* const fb = renderer.getFrameBuffer();

  if ((originX % 8) != 0) {
    // Not a limitation of the card: a window that does not start on a byte
    // needs every row bit-rotated, which is the cost design B exists to avoid.
    logSerial.printf("WALLETBENCH_ERR x=%lu is not a multiple of 8\n", static_cast<unsigned long>(originX));
    return;
  }
  const uint32_t xByte = originX / 8;
  if (stride < xByte + rowBytes) {
    logSerial.printf("WALLETBENCH_ERR stride=%lu too small for x=%lu + %lu row bytes\n",
                     static_cast<unsigned long>(stride), static_cast<unsigned long>(originX),
                     static_cast<unsigned long>(rowBytes));
    return;
  }

  char path[128];
  if (snprintf(path, sizeof(path), "/trailink/%s", relPath) >= static_cast<int>(sizeof(path))) {
    logSerial.printf("WALLETBENCH_ERR path too long\n");
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("WBENCH", path, file)) {
    logSerial.printf("WALLETBENCH_ERR cannot open %s\n", path);
    return;
  }
  const size_t fileBytes = file.size();

  // Skip a wallet asset header if there is one. Reuses the reader's own parse
  // (src/activities/wallet/WalletAsset.h) rather than assuming an offset: a
  // whole-page image for design B is not a panel-sized asset, so the panel gate
  // would refuse it, but its header is the same 32 bytes.
  uint32_t base = 0;
  uint8_t head[wallet::kAssetHeaderBytes];
  wallet::AssetHeader header;
  if (file.read(head, sizeof(head)) == static_cast<int>(sizeof(head)) &&
      wallet::parseAssetHeader(head, sizeof(head), header)) {
    base = wallet::kAssetHeaderBytes;
  }

  const uint64_t lastRowEnd =
      static_cast<uint64_t>(base) + static_cast<uint64_t>(originY + rows - 1) * stride + xByte + rowBytes;
  if (lastRowEnd > fileBytes) {
    logSerial.printf("WALLETBENCH_ERR window runs past EOF: needs %llu bytes, file is %u\n",
                     static_cast<unsigned long long>(lastRowEnd), static_cast<unsigned>(fileBytes));
    file.close();
    return;
  }
  if (base + bufferBytes > fileBytes) {
    logSerial.printf("WALLETBENCH_ERR file too small for the sequential baseline\n");
    file.close();
    return;
  }

  // A legend, so the three lines below need no doc to read: the min/med/max are
  // per frame, one frame being one whole window.
  logSerial.printf("WALLETBENCH fields=total_ms,min|med|max_ms_per_frame,payload_kbps,read_bytes\n");
  logSerial.printf("WALLETBENCH file=%s bytes=%u base=%lu stride=%lu win=%lu,%lu rows=%lu rowbytes=%lu iters=%d\n",
                   path, static_cast<unsigned>(fileBytes), static_cast<unsigned long>(base),
                   static_cast<unsigned long>(stride), static_cast<unsigned long>(originX),
                   static_cast<unsigned long>(originY), static_cast<unsigned long>(rows),
                   static_cast<unsigned long>(rowBytes), iters);

  size_t oversizedRead = 0;
  size_t streamRead = 0;
  bool shortRead = false;

  {
    // The framebuffer is the destination and the render task owns it too. Held
    // across every iteration of every mode: nothing may repaint in the middle of
    // a measurement, and the summary is printed after the lock goes away so CDC
    // writes are outside both the lock and the timing.
    RenderLock lock;

    for (int i = 0; i < iters; ++i) {
      // 1. windowed
      const uint32_t t0 = micros();
      for (uint32_t r = 0; r < rows; ++r) {
        const uint32_t off = base + (originY + r) * stride + xByte;
        if (!file.seekSet(off) || file.read(fb + r * rowBytes, rowBytes) != static_cast<int>(rowBytes)) {
          shortRead = true;
          break;
        }
      }
      walletBenchSamples[0][i] = micros() - t0;

      // 2. sequential baseline, from the payload start
      const uint32_t t1 = micros();
      if (!file.seekSet(base) || file.read(fb, bufferBytes) != static_cast<int>(bufferBytes)) shortRead = true;
      walletBenchSamples[1][i] = micros() - t1;

      // 3. oversized row reads
      size_t pulled = 0;
      const uint32_t t2 = micros();
      for (uint32_t r = 0; r < rows; ++r) {
        const uint32_t off = base + (originY + r) * stride + xByte;
        size_t want = kWalletBenchOversizedRead;
        if (off + want > fileBytes) want = fileBytes - off;
        if (!file.seekSet(off)) {
          shortRead = true;
          break;
        }
        const int got = file.read(walletBenchScratch, want);
        if (got < static_cast<int>(rowBytes)) {
          shortRead = true;
          break;
        }
        pulled += static_cast<size_t>(got);
        memcpy(fb + r * rowBytes, walletBenchScratch, rowBytes);
      }
      walletBenchSamples[2][i] = micros() - t2;
      oversizedRead = pulled;

      // 4. sequential stream over the window's row range, no seeks after the first
      size_t streamed = 0;
      const uint32_t t3 = micros();
      {
        uint32_t pos = base + originY * stride + xByte;
        if (!file.seekSet(pos)) shortRead = true;
        for (uint32_t r = 0; r < rows && !shortRead; ++r) {
          const uint32_t rowWant = base + (originY + r) * stride + xByte;
          // Walk forward to this row's window by reading, never by seeking. That
          // is the whole idea: the card is fast in a straight line.
          while (pos < rowWant) {
            size_t skip = rowWant - pos;
            if (skip > sizeof(walletBenchScratch)) skip = sizeof(walletBenchScratch);
            const int got = file.read(walletBenchScratch, skip);
            if (got <= 0) {
              shortRead = true;
              break;
            }
            pos += static_cast<uint32_t>(got);
            streamed += static_cast<size_t>(got);
          }
          if (shortRead) break;
          if (file.read(fb + r * rowBytes, rowBytes) != static_cast<int>(rowBytes)) {
            shortRead = true;
            break;
          }
          pos += rowBytes;
          streamed += rowBytes;
        }
      }
      walletBenchSamples[3][i] = micros() - t3;
      streamRead = streamed;
    }
  }

  file.close();

  walletBenchReport("windowed", walletBenchSamples[0], iters, bufferBytes, bufferBytes);
  walletBenchReport("sequential", walletBenchSamples[1], iters, bufferBytes, bufferBytes);
  walletBenchReport("oversized", walletBenchSamples[2], iters, bufferBytes, oversizedRead);
  walletBenchReport("stream", walletBenchSamples[3], iters, bufferBytes, streamRead);
  logSerial.printf("WALLETBENCH note=no-decrypt,no-refresh,file-kept-open,one-file,one-card,fixed-mode-order\n");
  if (shortRead) {
    logSerial.printf("WALLETBENCH_ERR a read came up short -- the numbers above are not trustworthy\n");
    return;
  }
  // The framebuffer now holds whatever the last read left in it and the panel
  // still shows the previous frame. Nothing refreshes here on purpose; the next
  // activity repaint fixes it.
  logSerial.printf("WALLETBENCH_OK\n");
}

}  // namespace

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  //
  // Peek before consuming: this used to read a whole line unconditionally
  // and discard it whenever it wasn't "CMD:...", which silently ate the
  // first line of anything else sharing the UART -- found the hard way by
  // MapSerialConsole (src/activities/map/MapSerialConsole.cpp), whose
  // MapActivity::loop() call runs after this one and never saw a command's
  // first line. "CMD:" is a deliberate namespace prefix for exactly this
  // reason (compare MapSerialConsole's '<' reply prefix); peeking the first
  // byte is what actually respects it instead of just picking the name.
  // Whitespace at the head of the buffer would sit there forever: nothing below
  // consumes a byte unless it is a 'C', and MapSerialConsole (the other reader on
  // this port) only ever gets a look once this branch declines. So one stray
  // newline from a host script blocks every command behind it until reboot.
  // Read off the code, NOT measured -- a suspected case on 2026-08-05 turned out
  // to be a different firmware on the device. Blank bytes mean nothing to either
  // consumer, so they can be dropped; anything else is still left strictly
  // alone.
  while (logSerial.available() > 0) {
    const int head = logSerial.peek();
    if (head != '\n' && head != '\r' && head != ' ' && head != '\t') break;
    logSerial.read();
  }

  if (logSerial.available() > 0 && logSerial.peek() == 'C') {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      // A host command means a host is waiting on the other end of the wire, so
      // come out of low-power mode first. Serial traffic is not "user activity"
      // (see the gpio/touch/tilt check below), so after IDLE_POWER_SAVING_MS the
      // CPU sits at LOW_POWER_FREQ -- 10 MHz on X4 -- and a 48,000-byte CDC dump
      // starves there: writeAllChunked() spends its whole 3-second budget on the
      // first ~4 KB and reports a truncated screenshot. Measured 2026-08-05 on
      // hardware, on both screenshot commands. Same reason CMD:GOTO_MAP does
      // this before touching NimBLE.
      powerManager.setPowerSaving(false);
      if (cmd == "SCREENSHOT_GRAY") {
        // Grey is not in any buffer to dump: the planes are streamed to the
        // controller band by band and the scratch is freed, and in the
        // framebuffer a grey pixel is *black*. So the planes are re-rendered
        // here from the last grey frame's own draw callback -- bit-identical to
        // what the panel got, 8 KB of scratch, no 96 KB shadow.
        //
        // Wire format:
        //   SCREENSHOT_GRAY_START:<totalBytes>:<planeBytes>:<exact 0|1>\n
        //   <BW frame><LSB plane><MSB plane>      (planes omitted when planeBytes == 0)
        //   SCREENSHOT_GRAY_END\n
        // Each blob is bufferSize bytes in physical row order, same layout as
        // CMD:SCREENSHOT. exact=0 means a region nudge has run since the last
        // full frame, so the panel carries grey the replay cannot reproduce.
        const uint32_t bufferSize = display.getBufferSize();
        // The replay drives the renderer's strip target, which the render task
        // also uses -- hold the lock for the whole dump so the BW frame and the
        // planes come from the same picture.
        RenderLock lock;
        // Nothing else may write to this wire until the last plane byte is out:
        // one log line in the middle of the payload corrupts it (see
        // SerialLogMute). Errors still reach the RTC ring buffer and the next
        // unmuted line.
        SerialLogMute quiet;
        const bool withPlanes = GrayscaleFrame::supported(renderer) && GrayscaleFrame::hasSource();
        const uint32_t total = withPlanes ? bufferSize * 3 : bufferSize;
        logSerial.printf("SCREENSHOT_GRAY_START:%u:%u:%d\n", (unsigned)total, (unsigned)(withPlanes ? bufferSize : 0),
                         GrayscaleFrame::sourceIsExact() ? 1 : 0);

        screenshotPlaneBytes = 0;
        const size_t bwWritten = writeAllChunked(display.getFrameBuffer(), bufferSize, /*totalTimeoutMs=*/3000);
        if (bwWritten != bufferSize) {
          LOG_ERR("SCR", "grey screenshot BW write incomplete: %u of %u bytes", (unsigned)bwWritten,
                  (unsigned)bufferSize);
        }
        if (withPlanes) {
          const GrayPlaneSink sink{nullptr, &screenshotPlaneSink};
          if (!GrayscaleFrame::replayPlanes(renderer, sink)) {
            LOG_ERR("SCR", "grey screenshot plane replay failed");
          } else if (screenshotPlaneBytes != bufferSize * 2) {
            LOG_ERR("SCR", "grey screenshot plane write incomplete: %u of %u bytes", (unsigned)screenshotPlaneBytes,
                    (unsigned)(bufferSize * 2));
          }
        }
        logSerial.printf("SCREENSHOT_GRAY_END\n");
      } else if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        SerialLogMute quiet;  // same reason as CMD:SCREENSHOT_GRAY above
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        const size_t written = writeAllChunked(buf, bufferSize, /*totalTimeoutMs=*/3000);
        if (written != bufferSize) {
          LOG_ERR("SCR", "screenshot write incomplete: %u of %u bytes", (unsigned)written, (unsigned)bufferSize);
        }
        logSerial.printf("SCREENSHOT_END\n");
      } else if (cmd == "SHOWIMAGE") {
        // CMD:SCREENSHOT backwards: the host pushes a whole framebuffer and the
        // panel shows it. There is no other way to judge a dither on this
        // device. A hatch or a tone looks like separate dots on a laptop LCD and
        // like flat grey on the panel, so every tone decision made against a PNG
        // preview is unverified until it has been through here
        // (docs/map-legibility.md, "judged on the wrong medium").
        //
        // Wire format, host side:
        //   CMD:SHOWIMAGE\n  ->  SHOWIMAGE_READY:<bufferSize>\n
        //   <bufferSize raw bytes>  ->  SHOWIMAGE_OK:<bytes>\n or SHOWIMAGE_ERR:<bytes>\n
        // The payload is the framebuffer exactly as CMD:SCREENSHOT dumps it:
        // 800x480 landscape, 1bpp MSB-first, physical row order, bit 1 = white.
        // tools/show_on_device.py in the parent repo builds it from a 480x800
        // portrait PNG.
        //
        // No allocation: the bytes go straight into the framebuffer the panel
        // already owns. Whatever was on screen is destroyed, which is the point.
        const uint32_t bufferSize = display.getBufferSize();
        uint8_t* buf = display.getFrameBuffer();
        if (buf == nullptr) {
          logSerial.printf("SHOWIMAGE_ERR:0\n");
        } else {
          // Held for the read *and* the refresh: the render task writes this
          // same buffer, and a repaint landing mid-transfer would leave half the
          // host's image on the panel and half of whatever it drew.
          RenderLock lock;
          SerialLogMute quiet;  // a log line mid-payload is indistinguishable from image data
          logSerial.printf("SHOWIMAGE_READY:%u\n", (unsigned)bufferSize);

          // Read the remainder against whatever has arrived, same shape as
          // writeAllChunked and for the same reason: one readBytes() call cannot
          // be trusted to drain a 48 KB transfer through HWCDC's ring buffer.
          // 10 seconds total -- the host has to push 48,000 bytes, which is
          // slower than the device sending them.
          size_t got = 0;
          const unsigned long deadline = millis() + 10000;
          while (got < bufferSize) {
            const int avail = logSerial.available();
            if (avail <= 0) {
              if (static_cast<long>(millis() - deadline) >= 0) break;
              delay(2);
              continue;
            }
            const size_t want =
                static_cast<size_t>(avail) < (bufferSize - got) ? static_cast<size_t>(avail) : (bufferSize - got);
            got += logSerial.readBytes(buf + got, want);
          }

          if (got != bufferSize) {
            // The framebuffer now holds a partial image. Say so rather than
            // refreshing: a half-written panel read as a rendering result would
            // be a lie, and the next activity repaint cleans it up anyway.
            logSerial.printf("SHOWIMAGE_ERR:%u\n", (unsigned)got);
          } else {
            // FULL_REFRESH, not FAST: the fast LUT leaves ghosting, and ghosting
            // on top of a dither is exactly the thing being judged.
            display.displayBuffer(HalDisplay::RefreshMode::FULL_REFRESH);
            logSerial.printf("SHOWIMAGE_OK:%u\n", (unsigned)got);
          }
        }
#ifdef ENABLE_SERIAL_LOG
      } else if (cmd.startsWith("SETTING ")) {
        // Flip one of the map's opt-in toggles from the host: bench tests cannot
        // press buttons, and the two features worth testing unattended -- tile
        // autosync and the freshness check -- are both off by default because
        // they spend the rider's mobile data. Without this, testing them means a
        // human walking the Settings menu before every run.
        //
        // Deliberately a short allow-list rather than a generic settings poke:
        // this is a serial backdoor into persisted state, so it can reach exactly
        // the three toggles a test needs and nothing else. ENABLE_SERIAL_LOG is
        // set only in env:default (platformio.ini), so it is not in any release
        // build.
        //
        //   CMD:SETTING mapAutoSyncTiles 1   ->  SETTING_OK:mapAutoSyncTiles=1
        //   CMD:SETTING <unknown> 1          ->  SETTING_ERR:unknown
        const int space = cmd.indexOf(' ', 8);
        String key = space < 0 ? cmd.substring(8) : cmd.substring(8, space);
        String value = space < 0 ? String("") : cmd.substring(space + 1);
        key.trim();
        value.trim();
        uint8_t* target = nullptr;
        if (key == "mapAutoSyncTiles")
          target = &SETTINGS.mapAutoSyncTiles;
        else if (key == "mapTileFreshnessMode")
          target = &SETTINGS.mapTileFreshnessMode;
        else if (key == "mapDebugInfo")
          target = &SETTINGS.mapDebugInfo;
        else if (key == "mapPinsOffscreen")
          target = &SETTINGS.mapPinsOffscreen;
        if (target == nullptr) {
          logSerial.printf("SETTING_ERR:unknown\n");
        } else if (value.length() == 0) {
          logSerial.printf("SETTING_OK:%s=%u\n", key.c_str(), static_cast<unsigned>(*target));
        } else {
          *target = static_cast<uint8_t>(value.toInt());
          SETTINGS.saveToFile();
          logSerial.printf("SETTING_OK:%s=%u\n", key.c_str(), static_cast<unsigned>(*target));
        }
#endif
      } else if (cmd == "GOTO_MAP" || cmd.startsWith("GOTO_MAP ")) {
        // Power saving is already off for every CMD: above -- load-bearing here
        // in particular: NimBLEDevice::init() (MapActivity::onEnter() ->
        // BlePositionServer::begin()) hangs solid if entered while still in
        // power-saving mode after idle -- confirmed on real hardware, see
        // docs/power-management.md.
        // Same call HomeActivity::onMapOpen() makes on manual selection --
        // arms replaceActivity(), resolved by activityManager.loop() later
        // in this same iteration.
        // An optional route path after the command, so a host can put the map on
        // screen *with a route loaded* -- docs/route-layer.md's open "no console
        // command" item. Without it the only way to a loaded route is the
        // picker's buttons, which means the whole route frame path cannot be
        // exercised or regression-tested from the laptop at all.
        //
        // MapActivity's constructor copies the path into its own fixed buffer
        // (MapActivity.cpp, `routePath_`), and runs synchronously inside
        // goToMap(), so handing it this local String's storage is safe.
        String routePath = cmd.substring(8);
        routePath.trim();
        if (routePath.isEmpty()) {
          LOG_DBG("MAIN", "CMD:GOTO_MAP received, calling goToMap()");
          activityManager.goToMap();
        } else {
          LOG_DBG("MAIN", "CMD:GOTO_MAP received with route %s", routePath.c_str());
          activityManager.goToMap(routePath.c_str());
        }
        LOG_DBG("MAIN", "goToMap() returned");
        logSerial.printf("GOTO_MAP_OK\n");
      } else if (cmd.startsWith("WALLETBENCH ")) {
        // Card-read benchmark for the wallet's pan design. powerManager
        // .setPowerSaving(false) already ran for every CMD above -- at 10 MHz
        // every one of these numbers would be a lie.
        char relPath[96] = {0};
        unsigned long strideArg = 0;
        unsigned long xArg = 0;
        unsigned long yArg = 0;
        int itersArg = 0;
        if (sscanf(cmd.c_str(), "WALLETBENCH %95s %lu %lu %lu %d", relPath, &strideArg, &xArg, &yArg, &itersArg) != 5) {
          logSerial.printf("WALLETBENCH_ERR usage: CMD:WALLETBENCH <path-under-/trailink> <stride> <x> <y> <iters>\n");
        } else {
          walletBenchRun(relPath, static_cast<uint32_t>(strideArg), static_cast<uint32_t>(xArg),
                         static_cast<uint32_t>(yArg), itersArg);
        }
      } else if (cmd == "GOTO_WALLET" || cmd.startsWith("GOTO_WALLET ")) {
        // The wallet was three button presses deep: Down, Down, Select, and then a
        // walk back into it after every asset push. That made every verification
        // round cost a person standing at the device, which is how a screen ends up
        // checked once and never again -- the same reason GOTO_TILESYNC below
        // exists.
        //
        // With an item and a code index a host script can land directly on one code
        // and screenshot it, which is what the P2 code screen needs and cannot get
        // any other way (docs/wallet-viewer.md, "CMD:GOTO_WALLET").
        //
        // Power saving is already off for every CMD: above. The wallet opens no
        // radio at all, so the NimBLE hang GOTO_MAP guards against cannot apply
        // here -- it inherits the same call for consistency, not necessity.
        int itemArg = -1;
        int codeArg = -1;
        String args = cmd.substring(11);
        args.trim();
        if (!wallet::parseGotoWalletArgs(args.c_str(), itemArg, codeArg)) {
          // Refused, not coerced: a mistyped index must not silently show document 0.
          logSerial.printf("GOTO_WALLET_ERR usage: CMD:GOTO_WALLET [<item> [<code>]]\n");
        } else {
          LOG_DBG("MAIN", "CMD:GOTO_WALLET received, item %d code %d", itemArg, codeArg);
          activityManager.goToWallet(itemArg, codeArg);
          LOG_DBG("MAIN", "goToWallet() returned");
          // OK means the request was well formed and the screen is armed. Whether the
          // index exists is only knowable once WalletActivity has read the manifest,
          // which happens after this returns -- it logs GOTO_WALLET: no item N and
          // says so on the panel. Same shape as GOTO_MAP, which does not check that
          // its route path exists either.
          logSerial.printf("GOTO_WALLET_OK item=%d code=%d\n", itemArg, codeArg);
        }
#if defined(ENABLE_WALLET_TEST_CMDS) && ENABLE_WALLET_TEST_CMDS
      } else if (cmd.startsWith("WALLETPROVISION ")) {
        // ## TEST PATH. Not how a device gets provisioned in the end.
        //
        // The wallet key and the PIN are supposed to arrive from the phone over BLE
        // (P4/P6). That app does not exist, so the laptop writes provision.json and
        // this command pushes it in -- which means the key crosses a USB cable in the
        // clear and is visible in any serial log of this session. It is a test seam
        // and the log says so every time it runs.
        //
        // Replaced by BLE provisioning; when that lands, this goes.
        char keyHex[80] = {0};
        char pinText[16] = {0};
        char saltHex[48] = {0};
        unsigned long itersArg = 0;
        char forceArg[16] = {0};
        const int fields = sscanf(cmd.c_str(), "WALLETPROVISION %79s %15s %47s %lu %15s", keyHex, pinText, saltHex,
                                  &itersArg, forceArg);
        if (fields < 4) {
          logSerial.printf(
              "WALLETPROVISION_ERR usage: CMD:WALLETPROVISION <keyhex64> <pin> <salthex32> <iters> [force]\n");
        } else {
          const bool force = fields >= 5 && strcmp(forceArg, "force") == 0;
          LOG_ERR("MAIN", "*** TEST PATH: CMD:WALLETPROVISION carries the wallet key in the clear over USB. ***");
          LOG_ERR("MAIN",
                  "*** Replaced by BLE provisioning (P4/P6). Never use it on a device holding real papers. ***");
          uint8_t key[wallet::kWalletKeyLen];
          uint8_t salt[wallet::kPbkdf2SaltLen];
          char pin[wallet::kPinBufBytes];
          size_t pinLen = 0;
          const bool keyOk = wallet::hexToBytes(keyHex, key, sizeof(key));
          const bool saltOk = wallet::hexToBytes(saltHex, salt, sizeof(salt));
          const bool pinOk = wallet::normalisePin(pinText, pin, sizeof(pin), pinLen);
          if (!keyOk) {
            logSerial.printf("WALLETPROVISION_ERR key must be %u hex characters\n",
                             static_cast<unsigned>(wallet::kWalletKeyLen * 2));
          } else if (!saltOk) {
            logSerial.printf("WALLETPROVISION_ERR salt must be %u hex characters\n",
                             static_cast<unsigned>(wallet::kPbkdf2SaltLen * 2));
          } else if (!pinOk) {
            logSerial.printf("WALLETPROVISION_ERR pin must be %u-%u of U/D/L/R\n",
                             static_cast<unsigned>(wallet::kPinMinLen), static_cast<unsigned>(wallet::kPinMaxLen));
          } else if (itersArg == 0 || itersArg > wallet::kMaxProvisionIterations) {
            logSerial.printf("WALLETPROVISION_ERR iterations must be 1..%lu\n",
                             static_cast<unsigned long>(wallet::kMaxProvisionIterations));
          } else {
            // Timed around the provisioning itself rather than with a second PBKDF2 run:
            // the derivation inside it is the same work an unlock pays, and running it
            // twice would double a wait that already blocks the main loop.
            const uint32_t startedUs = micros();
            const bool ok = wallet::KeyStore::provision(key, pin, pinLen, salt, sizeof(salt),
                                                        static_cast<uint32_t>(itersArg), force);
            const uint32_t micros1 = micros() - startedUs;
            wallet::secureWipe(key, sizeof(key));
            wallet::secureWipe(pin, sizeof(pin));
            if (ok) {
              // Provisioning invalidates any session key: it may be a different wallet.
              wallet::Session::instance().clear("provisioned");
              // provision_us is PBKDF2 plus the wrap plus the NVS writes -- an upper bound
              // on what an unlock costs, not the clean PBKDF2 figure. For that, use
              // CMD:WALLETPBKDF2.
              logSerial.printf("WALLETPROVISION_OK iters=%lu provision_us=%lu\n", itersArg,
                               static_cast<unsigned long>(micros1));
            } else {
              logSerial.printf("WALLETPROVISION_ERR refused (already provisioned? pass 'force')\n");
            }
          }
        }
      } else if (cmd.startsWith("WALLETPBKDF2 ")) {
        // The measurement the format doc is waiting for. Its 50,000 iterations came
        // off a laptop rate and is explicitly unverified; this times the real thing on
        // the chip, at full clock (power saving is already off for every CMD: above --
        // at 10 MHz the number would be a lie, docs/power-management.md).
        //
        // Capped: this runs to completion inside the main loop and the task watchdog
        // panics after 5 s (sdkconfig.defaults, CONFIG_ESP_TASK_WDT_TIMEOUT_S=5). The
        // answer being looked for is the largest count under 1 s, so the cap is never in
        // the way -- and if a run does trip the watchdog, the reboot IS the answer that
        // the count is too large.
        unsigned long itersArg = 0;
        if (sscanf(cmd.c_str(), "WALLETPBKDF2 %lu", &itersArg) != 1 || itersArg == 0 ||
            itersArg > wallet::kMaxProvisionIterations) {
          logSerial.printf("WALLETPBKDF2_ERR usage: CMD:WALLETPBKDF2 <iterations up to %lu>\n",
                           static_cast<unsigned long>(wallet::kMaxProvisionIterations));
        } else {
          const uint32_t elapsed = wallet::timePbkdf2Micros(static_cast<uint32_t>(itersArg));
          const unsigned long perSecond =
              elapsed > 0 ? static_cast<unsigned long>((itersArg * 1000000ULL) / elapsed) : 0;
          logSerial.printf("WALLETPBKDF2_OK iters=%lu us=%lu ms=%lu iters_per_s=%lu\n", itersArg,
                           static_cast<unsigned long>(elapsed), static_cast<unsigned long>(elapsed / 1000), perSecond);
        }
      } else if (cmd.startsWith("WALLETUNLOCK ")) {
        // ## TEST PATH. The same two warnings as WALLETPROVISION apply.
        //
        // PIN entry is four physical buttons, so without this nobody can verify the
        // whole crypto path -- manifest decrypt, asset decrypt, the wrong-key refusal,
        // the code screen on an encrypted tree -- unless a person is standing at the
        // device. That is precisely the list the host tests cannot reach
        // (docs/wallet-crypto.md).
        //
        // It drives the REAL path: KeyStore::tryUnlock(), the same call the PIN screen
        // makes, with the same unwrap, the same rate limiter and the same session. A
        // command that installed K directly would verify nothing about the thing it is
        // meant to verify.
        char pinText[16] = {0};
        if (sscanf(cmd.c_str(), "WALLETUNLOCK %15s", pinText) != 1) {
          logSerial.printf("WALLETUNLOCK_ERR usage attempts=%u\n", static_cast<unsigned>(wallet::KeyStore::failures()));
        } else {
          LOG_ERR("MAIN", "*** TEST PATH: CMD:WALLETUNLOCK opened the wallet from the host, not from a person. ***");
          LOG_ERR("MAIN",
                  "*** The PIN crossed a USB cable in the clear. Replaced by nothing -- it is a test seam. ***");
          uint32_t unwrapMicros = 0;
          uint32_t waitMs = 0;
          uint8_t failures = 0;
          const wallet::UnlockResult result = wallet::KeyStore::tryUnlock(pinText, unwrapMicros, failures, waitMs);
          // The typed PIN does not get to stay in the command buffer.
          wallet::secureWipe(pinText, sizeof(pinText));
          if (result == wallet::UnlockResult::Ok) {
            logSerial.printf("WALLETUNLOCK_OK unwrap_us=%lu\n", static_cast<unsigned long>(unwrapMicros));
          } else {
            // attempts is the count AFTER this one, and wait_ms is the delay now being
            // enforced -- so a host can watch the limiter double without guessing.
            logSerial.printf("WALLETUNLOCK_ERR %s attempts=%u of %u wait_ms=%lu\n", wallet::unlockResultName(result),
                             static_cast<unsigned>(failures), static_cast<unsigned>(wallet::kMaxPinFailures),
                             static_cast<unsigned long>(waitMs));
            if (result == wallet::UnlockResult::NotProvisioned) {
              logSerial.printf("WALLETUNLOCK_ERR no wrap in NVS: run CMD:WALLETPROVISION first\n");
            }
          }
        }
      } else if (cmd == "WALLETSTATUS") {
        // One line, so a host-driven check reads state instead of inferring it from a
        // screenshot. Every field is cheap: two Storage::exists calls and NVS reads.
        const wallet::Session& session = wallet::Session::instance();
        const char* manifestKind =
            wallet::treeIsEncrypted() ? "enc" : (Storage.exists(wallet::kManifestPath) ? "json" : "none");
        // The item count needs the manifest, so it is only knowable for a cleartext
        // tree or an unlocked encrypted one. -1 means "not knowable right now" rather
        // than "zero", which are different answers.
        int items = -1;
        if (!(wallet::treeIsEncrypted() && !session.hasKey()) && manifestKind[0] != 'n') {
          uint16_t stored = 0;
          uint32_t seen = 0;
          wallet::DeclaredPanel declared;
          wallet::Error error = wallet::Error::None;
          auto rows = makeUniqueNoThrow<wallet::ItemEntry[]>(4);
          if (rows && wallet::Store::listItems(rows.get(), 4, renderer, stored, seen, declared, error)) {
            items = static_cast<int>(seen);
          }
        }
        logSerial.printf(
            "WALLETSTATUS provisioned=%d unlocked=%d attempts=%u of %u wait_ms=%lu manifest=%s items=%d "
            "idle_left_ms=%lu iters=%lu\n",
            wallet::KeyStore::isProvisioned() ? 1 : 0, session.hasKey() ? 1 : 0,
            static_cast<unsigned>(wallet::KeyStore::failures()), static_cast<unsigned>(wallet::kMaxPinFailures),
            static_cast<unsigned long>(session.retryWaitMs()), manifestKind, items,
            static_cast<unsigned long>(session.idleLeftMs()),
            static_cast<unsigned long>(wallet::KeyStore::iterations()));
      } else if (cmd == "WALLETLOCK") {
        // The explicit lock, and the only way to drop the key without sleeping or
        // waiting out the idle timeout. Also how a host script proves the unlock path
        // twice in one session -- which is why the reply distinguishes "locked it" from
        // "it was already locked": a script needs to know whether its lock did
        // anything.
        const bool wasUnlocked = wallet::Session::instance().hasKey();
        wallet::Session::instance().clear("CMD:WALLETLOCK");
        logSerial.printf("WALLETLOCK_OK was_unlocked=%d\n", wasUnlocked ? 1 : 0);
#endif  // ENABLE_WALLET_TEST_CMDS
      } else if (cmd == "GOTO_TILESYNC") {
        // The sync screen was the one screen a host could not reach. Its grid --
        // outlined squares for missing tiles, dots for the freshness check queue
        // -- is a layout decision that has to be judged on the panel, and the
        // only way onto it was pressing buttons. So every look at it cost a
        // person standing at the device, which is how a layout ends up
        // unreviewed (docs/tile-freshness.md, "The check queue is dots").
        //
        // Same power-saving reason as GOTO_MAP above: this screen also calls
        // BlePositionServer::begin(), and NimBLEDevice::init() hangs solid if
        // entered while still in power-saving mode.
        LOG_DBG("MAIN", "CMD:GOTO_TILESYNC received, calling goToTileSync()");
        activityManager.goToTileSync();
        LOG_DBG("MAIN", "goToTileSync() returned");
        logSerial.printf("GOTO_TILESYNC_OK\n");
      }
    }
  }

  // Two deadlines, not one. They used to be the same variable, which meant an
  // activity could not ask to stay awake without also pinning the CPU at
  // 160 MHz -- and the map screen asks to stay awake for the whole ride. Run 2
  // measured what that costs: 160 MHz for all but 0.02 % of a 13 h day, with
  // the loop doing real work a few percent of the time (docs/power-plan.md).
  //
  // Real user input still resets both, so nothing about pressing a button
  // changed.
  static unsigned long lastActivityTime = millis();   // -> auto-sleep timeout
  static unsigned long lastFullClockTime = millis();  // -> CPU throttle
  const bool userInput =
      gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity();
  if (userInput || activityManager.preventAutoSleep()) {
    lastActivityTime = millis();
  }
  if (userInput || activityManager.preventThrottle()) {
    lastFullClockTime = millis();
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  // Duty cycle of the whole device: iterations, time spent working, worst
  // iteration. On the map screen this loop runs at ~100 Hz and mostly does
  // nothing (docs/power-management.md), and the counter is how that claim gets
  // checked on hardware rather than argued from the code.
  POWER_TELEMETRY.onLoop(static_cast<uint32_t>(loopDuration));
  // At most one CSV row per minute; a no-op on every other iteration.
  PowerLog::tick();
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    // Reads lastFullClockTime, not lastActivityTime: an activity that only
    // asked not to be slept (the map) must still be allowed to throttle.
    if (millis() - lastFullClockTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
