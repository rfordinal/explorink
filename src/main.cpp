#include <Arduino.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FrontlightManager.h>
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

#ifdef ENABLE_GNSS_CMD
#include <BoardT5S3.h>
#include <Gnss.h>
#include <Wire.h>
#include <esp_system.h>
#endif

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
// Inert on every board whose profile has no frontlight (X4, X3), so it is
// unconditional here — FrontlightManager::present() is the runtime question.
FrontlightManager frontlight;

#ifdef ENABLE_GNSS_CMD
// Bring-up instrument for the LilyGo T5 S3 Pro's on-board L76K receiver, driven
// entirely from CMD:GNSS below. There is no UI and no map integration yet: the
// point is to find out whether the receiver is wired the way the header says
// before anything depends on it (docs/gnss.md).
Gnss gnss;

// The receiver's power rail is a single PCA9535 expander pin that powers the
// LoRa radio along with it -- there is no way to have GNSS on this board
// without also powering the SX1262 (BoardT5S3Pins.h:70).
//
// That matters more than it looks. LORA_CS (GPIO46) is also handed to LovyanGFX
// as the panel bus's pin_oe *and* pin_pwr (LilyGoT5S3LgfxConfig.cpp:162,166),
// and Bus_EPD really does drive both as plain GPIOs, one of them as the i80
// bus's DC line (M5GFX Bus_EPD.cpp:83,85,120,129,143). So once the rail is up,
// every panel refresh asserts the radio's chip select -- on the same SPI bus
// the SD card is on, where a second device driving MISO corrupts tile reads.
//
// The defence is to hold the SX1262 in reset, which parks its MISO high-Z. It
// is done here rather than left to BoardT5S3::disableGpsLora(), because nothing
// in this firmware calls BoardT5S3::begin(): that function has never run on
// this board, so LORA_RST is undriven at boot and cannot be assumed low.
static bool gnssPowerEnable(bool on) {
  if (BoardConfig::ACTIVE.board != BoardConfig::Board::LilyGoT5S3) return false;

  // Wire is normally already up from GT911 touch init (InputManager.cpp:839).
  // Only re-run the board's own I2C setup if the expander does not answer, so
  // a working bus is never reinitialised underneath the touch driver.
  if (!BoardT5S3::pca9535Present()) {
    BoardT5S3::beginI2C();
    if (!BoardT5S3::pca9535Present()) return false;
  }

  pinMode(T5S3_LORA_RST, OUTPUT);
  digitalWrite(T5S3_LORA_RST, LOW);

  // Level before direction, matching disableGpsLora(): switching an expander
  // pin to output first would drive whatever the output register happens to
  // hold, which on a cold boot is the PCA9535's power-on default of high.
  if (!BoardT5S3::writePca9535Pin(PCA9535_IO00_LORA_GPS_EN, on)) return false;
  if (!BoardT5S3::setPca9535PinMode(PCA9535_IO00_LORA_GPS_EN, OUTPUT)) {
    // The write above already took effect and the direction may already have
    // been output from an earlier call, so a failure here can leave the rail
    // live while this function reports failure. Undo it before returning.
    if (on) BoardT5S3::writePca9535Pin(PCA9535_IO00_LORA_GPS_EN, false);
    return false;
  }
  return true;
}

// Reads the PCA9535's own registers, which BoardT5S3 does not expose: it offers
// readPca9535Pin(), and that reads the INPUT port, i.e. the pin's level rather
// than its direction. Direction is the question here.
//
// Why direction answers anything: the expander comes out of power-on reset with
// every pin an input, and this firmware writes only port 1 (the EPD pins, in
// LilyGoT5S3LgfxConfig.cpp). Port 0 bit 0 is LORA_GPS_EN. So on a genuine power
// cycle it must still read as an input -- and if the receiver is nonetheless
// streaming NMEA, something outside this firmware is holding that rail on. If it
// reads as an output, a previous session latched it and the expander never lost
// power, which is the alternative this probe exists to exclude.
static bool gnssReadExpanderRegister(uint8_t reg, uint8_t* value) {
  BoardT5S3::ScopedI2CLock lock;
  Wire.beginTransmission(T5S3_PCA9535_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<uint8_t>(T5S3_PCA9535_ADDR), static_cast<uint8_t>(1)) != 1) {
    while (Wire.available()) Wire.read();
    return false;
  }
  *value = Wire.read();
  return true;
}

// Why the reset cause belongs in the PROBE reply and not in the boot log: the
// answer is only meaningful on a POWERON boot, and the ROM's own line is printed
// before the host can open the CDC, so it was missed on every attempt. This is
// read from the chip's retained reason, valid for the whole boot, so the
// precondition travels in the same line as the values it qualifies.
static const char* gnssResetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

// CMD:GNSS RAW passthrough. The parser hands over the sentence with its "*hh"
// checksum but without the leading '$', so put the '$' back: a line pasted out
// of this log is then feedable to any NMEA tool unchanged. The first version
// stripped the checksum too and produced lines that looked like NMEA and were
// not -- caught on hardware, 2026-08-31.
static void gnssRawSink(const char* sentence, size_t length) {
  logSerial.printf("GNSS_RAW:$%.*s\n", static_cast<int>(length), sentence);
}
#endif
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

// The map's own small face: a 12 px line, where the smallest face before it was
// 23 px. It exists for the contour height numbers, which have to be readable
// without becoming the loudest thing on a terrain frame. Regular only -- a
// number has no second tier (docs/place-labels.md, "The font").
EpdFont mapSmallFont(&ubuntu_5_regular);
EpdFontFamily mapSmallFontFamily(&mapSmallFont);

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
  renderer.insertFont(MAP_SMALL_FONT_ID, mapSmallFontFamily);

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
  frontlight.begin();
#if FREEINK_CAP_FRONTLIGHT && defined(ARDUINO) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // LilyGo's own answer, 2026-08-25: the PT4103B23F behind BL_EN wants a PWM
  // frequency "not above approximately 1 kHz". The SDK board profile asks for
  // 5 kHz (BoardConfig.h, LILYGO_T5S3), which is above the vendor's ceiling —
  // freeink-sdk is upstream, so correct it here rather than forking the SDK.
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::LilyGoT5S3 && frontlight.present()) {
    ledcChangeFrequency(BoardConfig::ACTIVE.frontlight.gpio, 1000,
                        BoardConfig::ACTIVE.frontlight.pwmResolutionBits);
  }
#endif
  halTiltSensor.begin();
  halClock.begin();

  // The X3/X4 GPIO probe only distinguishes those two; every other board is a
  // single-device binary whose profile is fixed at compile time. Report the
  // active profile so a non-Xteink build does not log itself as an X4.
  LOG_INF("MAIN", "Hardware detect: %s (%ux%u)",
          (BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4 ||
           BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3)
              ? (gpio.deviceIsX3() ? "X3" : "X4")
              : BoardConfig::ACTIVE.name,
          BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);

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

#ifdef ENABLE_GNSS_CMD
  // Drain the receiver's UART every iteration. The parser does no work beyond
  // what the port already buffered, and at 9600 baud a full NMEA cycle is well
  // under 1 kB per second -- but the driver's own RX buffer is 256 bytes, so
  // skipping iterations is how sentences get lost.
  gnss.poll();
#endif

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

  // Say what is blocking the command queue, once per boot. Everything above this
  // point consumes only whitespace, so a single non-'C' byte at the head wedges
  // every command for the rest of the session -- and on 2026-08-31 a whole
  // bring-up run had every command silently dropped, with no way to tell this
  // apart from a broken USB link. ModemManager probing a freshly enumerated ACM
  // device with "AT" would produce exactly that, and so would a torn first write.
  //
  // Five seconds of the SAME unconsumed byte, not merely a non-'C' byte: the map
  // screen's own console reads this port too (MapSerialConsole), so a non-'C'
  // head is perfectly normal while that is running and warning on it would cry
  // wolf on every map session.
  //
  // And then DRAIN it, which is the difference between a diagnosis and a fix.
  // Measured twice on 2026-08-31: after a cold power-on the head byte was 0x5B
  // ('['), the first character of this firmware's own log lines, and every CMD:
  // for the next eight minutes was silently ignored. Two whole bring-up runs were
  // lost to it before the log line above existed.
  //
  // Five seconds of the SAME byte is the trigger, not merely a non-'C' byte,
  // because the other reader on this port (MapSerialConsole) legitimately leaves
  // its own input at the head -- and it consumes within milliseconds when it is
  // running, so it never reaches this timeout. Draining one byte per pass rather
  // than the whole buffer keeps that true even if something arrives mid-line.
  {
    static bool reportedStuckHead = false;
    static int lastHead = -1;
    static unsigned long headSince = 0;
    const int pending = logSerial.available();
    const int head = pending > 0 ? logSerial.peek() : -1;
    if (head < 0 || head == 'C') {
      lastHead = -1;
      headSince = 0;
    } else if (head != lastHead) {
      lastHead = head;
      headSince = millis();
    } else if (headSince != 0 && millis() - headSince > 5000) {
      if (!reportedStuckHead) {
        reportedStuckHead = true;
        LOG_ERR("MAIN",
                "serial head byte 0x%02X (%c), %d pending, unconsumed for 5 s -- draining it; every "
                "CMD: was being ignored",
                head, (head >= 32 && head < 127) ? static_cast<char>(head) : '?', pending);
      }
      logSerial.read();
      lastHead = -1;
      headSince = 0;
    }
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
#ifdef ENABLE_FRONTLIGHT_CMD
      } else if (cmd == "LIGHT" || cmd.startsWith("LIGHT ")) {
        // Bring-up instrument, not a rider feature: the frontlight has no UI on
        // any screen yet, so this is the only way to find out whether the light
        // is even wired the way the schematic says. Devel-only on purpose
        // (-DENABLE_FRONTLIGHT_CMD lives in env:t5s3pro, not in gh_release):
        // it actuates the device, and CLAUDE.md's security rule defaults a new
        // command to devel until widening it is a deliberate decision.
        //
        //   CMD:LIGHT        ->  LIGHT_OK:<percent>        (query)
        //   CMD:LIGHT 40     ->  LIGHT_OK:40               (0-100, 0 = off)
        if (!frontlight.present()) {
          logSerial.printf("LIGHT_ERR:no frontlight on this board\n");
        } else {
          String value = cmd.substring(5);
          value.trim();
          if (value.length() > 0) {
            long pct = value.toInt();
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            frontlight.setBrightness(static_cast<uint8_t>(pct));
          }
          logSerial.printf("LIGHT_OK:%u\n", static_cast<unsigned>(frontlight.brightness()));
        }
#endif
#ifdef ENABLE_GNSS_CMD
      } else if (cmd == "GNSS" || cmd.startsWith("GNSS ")) {
        // Bring-up instrument for the on-board GNSS receiver, and the only way
        // to reach it: there is no UI and the map still takes its position over
        // BLE from the phone. Devel-only on purpose (-DENABLE_GNSS_CMD lives in
        // env:t5s3pro and in no release env) for two separate reasons -- it
        // powers a radio rail, and its reply is the rider's exact position.
        //
        //   CMD:GNSS           ->  GNSS_FIX:... | GNSS_NOFIX:... | GNSS_OFF
        //   CMD:GNSS ON        ->  GNSS_OK:on
        //   CMD:GNSS OFF       ->  GNSS_OK:off
        //   CMD:GNSS RAW ON    ->  GNSS_OK:raw=1   (every sentence to the log)
        //   CMD:GNSS RAW OFF   ->  GNSS_OK:raw=0
        //   CMD:GNSS PROBE     ->  GNSS_PROBE:...  (run first, on a cold boot)
        //
        // Reading the reply: `ttff` is NOT an acquisition time on a receiver
        // that was already running -- Gnss::timeToFirstFixMs() spells out why
        // anything under about 1.2 s means only "already tracking". `rxfull`
        // non-zero means sentences were lost inside the UART driver, so every
        // other count in the line is an undercount.
        String argument = cmd.substring(4);
        argument.trim();
        argument.toUpperCase();

        if (argument == "ON") {
          GnssConfig config;
          config.serial = &Serial1;
          // Board header names these GPS_RXD / GPS_TXD, which does not say whose
          // RX it means. Read as MCU-side here: RXD 44 is where the S3 receives,
          // so it goes to the receiver's TX. Both are UART0's default pins on an
          // S3, free only because this env runs its console over USB CDC. If a
          // bring-up sees no bytes at all, swapping these two is the first thing
          // to try -- the symptom is identical to a dead receiver.
          config.rxPin = T5S3_GPS_RXD;
          config.txPin = T5S3_GPS_TXD;
          // L76K default per Quectel, still unverified against the datasheet.
          config.baud = 9600;
          config.powerEnable = gnssPowerEnable;
          if (gnss.begin(config)) {
            logSerial.printf("GNSS_OK:on\n");
          } else {
            logSerial.printf("GNSS_ERR:power rail or expander unavailable\n");
          }
        } else if (argument == "OFF") {
          gnss.end();
          logSerial.printf("GNSS_OK:off\n");
        } else if (argument == "PROBE") {
          // Answers one question and must run BEFORE any CMD:GNSS ON in the
          // session, on a boot that is a real power-on rather than a reset:
          // is the receiver's rail held on by the board, or was it left on by an
          // earlier session? The 2026-08-31 bring-up could not tell those apart
          // and wrongly published the first one (docs/gnss.md).
          //
          // Reads the expander's direction, then opens the UART with NO power
          // hook at all, so nothing here can write the rail and spoil the
          // reading. Check the ROM's reset cause in the boot log too: only
          // POWERON makes the answer mean anything.
          uint8_t config0 = 0;
          uint8_t config1 = 0;
          uint8_t output0 = 0;
          uint8_t input0 = 0;
          const bool haveConfig = gnssReadExpanderRegister(0x06, &config0);
          // CONFIG1 is the addressing cross-check, because this firmware really
          // does configure port 1: prepareEpdPower() sets IO10, IO11, IO13, IO14,
          // IO15 as outputs and IO16, IO17 as inputs, and nothing that runs
          // configures IO12 (BoardT5S3::begin(), which would, is never called).
          // So under the datasheet's all-inputs default this must read 0xC4. If
          // it does, register 0x06 is being addressed correctly too and the port 0
          // reading has to be believed.
          const bool haveConfig1 = gnssReadExpanderRegister(0x07, &config1);
          const bool haveOutput = gnssReadExpanderRegister(0x02, &output0);
          const bool haveInput = gnssReadExpanderRegister(0x00, &input0);
          if (!haveConfig || !haveConfig1 || !haveOutput || !haveInput) {
            logSerial.printf("GNSS_PROBE_ERR:expander read failed\n");
          } else {
            const bool isInput = (config0 & 0x01) != 0;
            GnssConfig probe;
            probe.serial = &Serial1;
            probe.rxPin = T5S3_GPS_RXD;
            probe.txPin = T5S3_GPS_TXD;
            probe.baud = 9600;
            probe.powerEnable = nullptr;  // the whole point
            probe.powerSettleMs = 0;
            gnss.begin(probe);
            const unsigned long until = millis() + 2500;
            while (millis() < until) {
              gnss.poll();
            }
            logSerial.printf(
                "GNSS_PROBE:reset=%s cfg0=0x%02X cfg1=0x%02X(want 0xC4) out0=0x%02X in0=0x%02X "
                "io00_dir=%s io00_level=%s bytes=%lu sent=%lu cserr=%lu ferr=%lu\n",
                gnssResetReasonName(), config0, config1, output0, input0, isInput ? "input" : "output",
                (input0 & 0x01) ? "high" : "low", static_cast<unsigned long>(gnss.bytesRead()),
                static_cast<unsigned long>(gnss.sentencesParsed()),
                static_cast<unsigned long>(gnss.checksumErrors()),
                static_cast<unsigned long>(gnss.framingErrors()));
            gnss.end();  // powerEnable is null, so this touches no rail
          }
        } else if (argument == "RAW ON" || argument == "RAW") {
          gnss.setRawSink(gnssRawSink);
          logSerial.printf("GNSS_OK:raw=1\n");
        } else if (argument == "RAW OFF") {
          gnss.setRawSink(nullptr);
          logSerial.printf("GNSS_OK:raw=0\n");
        } else if (argument.length() > 0) {
          logSerial.printf("GNSS_ERR:expected ON, OFF, PROBE, RAW ON or RAW OFF\n");
        } else if (!gnss.running()) {
          logSerial.printf("GNSS_OFF\n");
        } else {
          // One line, fixed key=value shape, so a host script can grep it and a
          // person can read it. used/inview/tracked are three different counts
          // and the difference between them is the whole diagnosis: inview from
          // the almanac, tracked from a non-zero C/N0, used in the solution.
          const GnssFix& fix = gnss.fix();
          if (fix.valid) {
            logSerial.printf(
                "GNSS_FIX:q=%u used=%u inview=%u tracked=%u bestsnr=%u lat=%.6f lon=%.6f alt=%.1f "
                "hdop=%.2f speed=%.1f course=%.1f utc=%lu ttff=%lu age=%lu sent=%lu cserr=%lu ferr=%lu "
                "rxfull=%lu bytes=%lu\n",
                static_cast<unsigned>(fix.quality), static_cast<unsigned>(fix.satsUsed),
                static_cast<unsigned>(gnss.satsInView()), static_cast<unsigned>(gnss.satsWithSignal()),
                static_cast<unsigned>(gnss.bestSnr()), fix.latitude, fix.longitude, fix.altitudeMeters,
                fix.hdop, fix.speedKmh, fix.courseDegrees, static_cast<unsigned long>(fix.utc),
                static_cast<unsigned long>(gnss.timeToFirstFixMs()),
                static_cast<unsigned long>(gnss.fixAgeMs()),
                static_cast<unsigned long>(gnss.sentencesParsed()),
                static_cast<unsigned long>(gnss.checksumErrors()),
                static_cast<unsigned long>(gnss.framingErrors()),
                static_cast<unsigned long>(gnss.rxNearlyFullEvents()),
                static_cast<unsigned long>(gnss.bytesRead()));
          } else {
            logSerial.printf(
                "GNSS_NOFIX:q=%u inview=%u tracked=%u bestsnr=%u utc=%lu uptime=%lu sent=%lu cserr=%lu "
                "ferr=%lu rxfull=%lu bytes=%lu\n",
                static_cast<unsigned>(fix.quality), static_cast<unsigned>(gnss.satsInView()),
                static_cast<unsigned>(gnss.satsWithSignal()), static_cast<unsigned>(gnss.bestSnr()),
                static_cast<unsigned long>(fix.utc), static_cast<unsigned long>(gnss.uptimeMs()),
                static_cast<unsigned long>(gnss.sentencesParsed()),
                static_cast<unsigned long>(gnss.checksumErrors()),
                static_cast<unsigned long>(gnss.framingErrors()),
                static_cast<unsigned long>(gnss.rxNearlyFullEvents()),
                static_cast<unsigned long>(gnss.bytesRead()));
          }
        }
#endif
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
