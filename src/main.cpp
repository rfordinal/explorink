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

#if FREEINK_DEVICE_LILYGO
// The board's own support code: the PCA9535 expander behind the user button and
// the GNSS/LoRa power rail both live here.
#include <BoardT5S3.h>
// For the expander's direction register, which BoardT5S3 does not expose.
#include <Wire.h>
#endif

#ifdef ENABLE_GNSS_CMD
#include <Gnss.h>
#include <Wire.h>
#include <esp_system.h>
#endif

#ifdef ENABLE_SDBUS_CMD
#include <esp_rom_crc.h>
#endif

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GnssAccess.h"
#include "GnssLog.h"
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

namespace {
// Set when a gesture changed the light; loop() turns it into the one SD write.
// Never written from the input hook's own call site for a reason: the hook runs
// inside InputManager::update(), and a card write there would sit on the input
// path and block every other poll behind it.
bool frontlightStateChanged = false;

// Both of this board's programmable inputs land here, so the gesture means the
// same thing whichever one the rider used (side switch or capacitive home key).
void toggleFrontlight(const char* source) {
  if (!frontlight.present()) return;
  if (frontlight.brightness() > 0) {
    frontlight.off();
  } else {
    frontlight.on();
  }
  frontlightStateChanged = true;
  LOG_INF("BTN", "%s: frontlight %u%%", source, static_cast<unsigned>(frontlight.brightness()));
}
}  // namespace

#if FREEINK_DEVICE_LILYGO
// The T5 S3 Pro's user button (switch S3, silkscreened IO48, wired to PCA9535
// IO12 -- docs/devices/lilygo-t5-s3-pro.md, "The four physical buttons") is the
// only button on this board firmware can read at all: BOOT is the power button,
// RST resets the MCU and PWR sits on the charger. So it carries two jobs.
//
//   tap   -> Confirm (Select)
//   hold  -> toggle the frontlight
//
// Why the light hangs off a physical hold and not a touch control: gloves defeat
// the capacitive panel, and the light is exactly what a rider reaches for with
// gloves on.
namespace {
constexpr unsigned long USER_BUTTON_HOLD_MS = 600;

// The tap is reported as a synthetic Confirm press *after* the button is
// released, because a press edge at touch-down would let the activity act
// before the hold could still turn out to mean the frontlight.
//
// It has to survive InputManager's debounce, which commits a state change only
// once two update() calls at least DEBOUNCE_DELAY (5 ms) apart saw the same
// state (InputManager.cpp, update()). Counting polls rather than wall time is
// what makes this survive a panel refresh: a millisecond window would expire
// unobserved while the main loop sits in a multi-second redraw, and the tap
// would be silently dropped. Both conditions must hold, so the pulse is long
// enough in time AND seen often enough.
constexpr uint8_t USER_BUTTON_CLICK_POLLS = 3;
constexpr unsigned long USER_BUTTON_CLICK_MS = 20;

bool userButtonDown = false;
bool userButtonLongFired = false;
unsigned long userButtonDownAt = 0;
bool userButtonClickPending = false;
uint8_t userButtonClickPolls = 0;
unsigned long userButtonClickSince = 0;

// Runs inside InputManager::update() (one call per poll), i.e. in whatever task
// drives the main loop. Reads one PCA9535 input register over I2C; BoardT5S3
// takes the bus mutex for us, so this is safe next to the panel's own expander
// writes.
uint8_t userButtonHook() {
  const unsigned long now = millis();
  const bool down = BoardT5S3::readButton();

  if (down && !userButtonDown) {
    userButtonDown = true;
    userButtonLongFired = false;
    userButtonDownAt = now;
    // Drop a tap still being reported: a second press starting inside that
    // window would otherwise be seen as Confirm held down.
    userButtonClickPending = false;
  } else if (down && !userButtonLongFired && now - userButtonDownAt >= USER_BUTTON_HOLD_MS) {
    // Fires the moment the hold is long enough, not on release: the light comes
    // on under the thumb, which is the feedback that says "let go now".
    userButtonLongFired = true;
    toggleFrontlight("User button hold");
  } else if (!down && userButtonDown) {
    userButtonDown = false;
    if (!userButtonLongFired) {
      userButtonClickPending = true;
      userButtonClickPolls = 0;
      userButtonClickSince = now;
    }
  }

  if (!userButtonClickPending) return 0;
  ++userButtonClickPolls;
  if (userButtonClickPolls > USER_BUTTON_CLICK_POLLS && now - userButtonClickSince >= USER_BUTTON_CLICK_MS) {
    userButtonClickPending = false;
    return 0;
  }
  return static_cast<uint8_t>(1U << InputManager::BTN_CONFIRM);
}
// --- SD bus vs. the LoRa radio -----------------------------------------------
//
// The SD card and the SX1262 share one SPI bus on this board: MISO21 MOSI13
// SCLK14, with SD_CS12 against LORA_CS46 (BoardT5S3Pins.h). LORA_CS is *also*
// handed to LovyanGFX as the panel bus's pin_oe and pin_pwr
// (LilyGoT5S3LgfxConfig.cpp:162,166).
//
// And LovyanGFX leaves that pin driven LOW, which on SPI means "radio, talk".
// Bus_EPD::init() does lgfx::pinMode(pin_oe, output) and later the same for
// pin_pwr (Bus_EPD.cpp:120,143), and lgfx::pinMode does NOT set a level for
// output mode -- its gpio_hi() is guarded to non-output modes
// (common.cpp:599-601). So GPIO46 becomes an output holding whatever the output
// register had, which is 0, and nothing ever raises it again. LORA_CS is
// asserted continuously from display init onward. A selected radio drives the
// shared MISO and every card transfer after that comes back corrupt: reads
// answer empty and writes fail, from every task at once and on any card. That
// is BUG-037.
//
// Do NOT write that this happens per refresh. It was written that way first and
// it is wrong: Bus_EPD::powerControl is virtual (Bus_EPD.h:95) and
// FreeInkBusEPD overrides it without calling the base
// (LgfxEpdDriver.cpp:34-45), so Bus_EPD.cpp's own gpio_lo() calls never run
// here. The pin is not toggled, it is simply left low.
//
// Measured 2026-09-03, and it is why this function deselects rather than merely
// unpowers: the pre-fix binary was reflashed with the card untouched and the
// shared rail confirmed OFF, and the card still read as empty from every path.
// So cutting the rail alone does not restore the card.
//
// Do not read more into that than it carries, and the doc says so at length.
// GPIO46 has never been probed on the pad, so "the radio was selected" is read
// off the code above, not measured. LORA_RST low and LORA_CS high have never
// run without each other. And whether an SX126x with no VDD loads MISO when NSS
// is low is unread -- no datasheet for it is on disk, and a parasitic path
// through the MCU's ESD diodes would mean it was never really unpowered.
//
// The counterexample that keeps this open: the same pre-fix binary, with this
// same pin at 0 and the rail ON, read tiles fine for days before 2026-09-02
// 15:23. A static wiring fault does not switch on by itself, so a second factor
// is likely and the enclosure is the candidate. This function is worth having
// either way, because it removes a load that should never have been there.
//
// The rail is cut anyway, for two independent reasons: it is shared with the
// GNSS receiver (PCA9535_IO00_LORA_GPS_EN), so it can be left on by an earlier
// session -- the expander keeps its registers across a soft reset AND a reflash
// -- and a rail left up drains the battery through deep sleep (T-244).
//
// BoardT5S3::begin() is never called in this firmware, and it is the only
// caller of disableGpsLora() and prepareSdBus(). prepareSdBus() is exactly this
// deselect, shipped by the SDK and dead on this board; T-243 is the general
// form of that problem.
//
// Runs before Storage.begin() and before display init. Both matter: the first
// because a corrupt bus fails card detection outright, the second because
// display init is what asserts the pin.
void t5s3ParkLoraOffSdBus() {
  if (BoardConfig::ACTIVE.board != BoardConfig::Board::LilyGoT5S3) return;

  // Unconditional and first: these two cost nothing when the radio is already
  // unpowered, and are the whole fix when it is not.
  pinMode(T5S3_LORA_RST, OUTPUT);
  digitalWrite(T5S3_LORA_RST, LOW);
  pinMode(T5S3_LORA_CS, OUTPUT);
  digitalWrite(T5S3_LORA_CS, HIGH);

  // Wire is normally already up from GT911 touch init, and gpio.begin() has run
  // by now. Only re-run the board's own I2C setup if the expander does not
  // answer, so a working bus is never reinitialised underneath the touch
  // driver.
  if (!BoardT5S3::pca9535Present()) {
    BoardT5S3::beginI2C();
    if (!BoardT5S3::pca9535Present()) {
      LOG_ERR("SDBUS", "PCA9535 silent: rail state unknown, radio held in reset only");
      return;
    }
  }

  // Direction, not level, is what says whether an earlier session latched the
  // rail: the expander comes out of power-on reset with every pin an input, and
  // this firmware writes port 0 nowhere else. So port 0 bit 0 reading as an
  // *output* here means a previous run wrote it and the expander never lost
  // power. Level alone cannot separate that from a board-side pull.
  //
  // Register 0x06 is CONFIG0. Read raw because BoardT5S3 exposes only
  // readPca9535Pin(), which reads the INPUT port.
  uint8_t cfg0 = 0;
  bool cfgOk = false;
  {
    BoardT5S3::ScopedI2CLock lock;
    Wire.beginTransmission(T5S3_PCA9535_ADDR);
    Wire.write(0x06);
    if (Wire.endTransmission(false) == 0 &&
        Wire.requestFrom(static_cast<uint8_t>(T5S3_PCA9535_ADDR), static_cast<uint8_t>(1)) == 1) {
      cfg0 = Wire.read();
      cfgOk = true;
    } else {
      while (Wire.available()) Wire.read();
    }
  }
  bool railHigh = false;
  const bool levelOk = BoardT5S3::readPca9535Pin(PCA9535_IO00_LORA_GPS_EN, &railHigh);

  // Level before direction, matching disableGpsLora(): switching the pin to
  // output first would drive whatever the output register happens to hold,
  // which on a cold boot is the PCA9535's power-on default of high.
  const bool wroteLevel = BoardT5S3::writePca9535Pin(PCA9535_IO00_LORA_GPS_EN, false);
  const bool wroteDir = BoardT5S3::setPca9535PinMode(PCA9535_IO00_LORA_GPS_EN, OUTPUT);

  // One line, and it carries its own provenance: a claim of "latched" that
  // rests on a failed I2C read would otherwise read exactly like a measurement.
  LOG_INF("SDBUS", "LORA/GPS rail at boot: cfg0=%s%02X dir=%s level=%s%s -- cut %s, radio in reset off the SD bus",
          cfgOk ? "0x" : "READ-FAILED:0x", cfg0,
          cfgOk ? ((cfg0 & 0x01) ? "input (cold)" : "OUTPUT (latched by an earlier session)") : "unknown",
          levelOk ? (railHigh ? "high" : "low") : "read-failed",
          (cfgOk && !(cfg0 & 0x01) && levelOk && railHigh) ? " -- radio WAS powered" : "",
          (wroteLevel && wroteDir) ? "ok" : "FAILED");
}
}  // namespace
#endif  // FREEINK_DEVICE_LILYGO

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
// and it is left driven LOW from display init onward -- see the long comment on
// t5s3ParkLoraOffSdBus() above for why, and for why "every panel refresh
// asserts it" (what this comment used to say) is wrong. The radio therefore
// sits selected on the same SPI bus the SD card is on, where a second device
// driving MISO corrupts every card transfer.
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

// Opens the receiver: rail up, UART up, with this board's pins and ring size.
// Declared in GnssAccess.h so the map can call it too -- CMD:GNSS ON was the
// only caller when this was inline, and a second caller must not carry a second
// copy of the pin numbers.
bool gnssStart() {
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
#ifdef GNSS_RX_BUFFER_BYTES
  // The board raises the library's modest default, because this board
  // blocks its main loop for seconds at a time and the library is meant
  // to run on ones that do not. platformio.ini carries the measurement
  // that picked the number.
  config.rxBufferBytes = GNSS_RX_BUFFER_BYTES;
#endif
  config.powerEnable = gnssPowerEnable;
  return gnss.begin(config);
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
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    default:
      return "UNKNOWN";
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
  // Inert on a board without one. The light is a hold away from being left on
  // in a bag, and deep sleep stops the LEDC peripheral without defining what
  // the pin does afterwards, so drive it off while the rail is still ours.
  frontlight.off();
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
    ledcChangeFrequency(BoardConfig::ACTIVE.frontlight.gpio, 1000, BoardConfig::ACTIVE.frontlight.pwmResolutionBits);
  }
#endif
#if FREEINK_DEVICE_LILYGO
  // After frontlight.begin() on purpose: the hook can toggle the light, so it
  // must not be reachable before the LEDC channel exists.
  //
  // BoardT5S3::begin() -- which would configure IO12 and install the SDK's own
  // hook, the one that reports the button as Down -- is never called in this
  // firmware (see gnssPowerEnable() above). So this is the whole wiring of the
  // button, and setting the direction is not redundant: the expander comes out
  // of power-on reset with every pin an input, but a soft reset leaves it
  // holding whatever the previous session wrote.
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::LilyGoT5S3) {
    if (!BoardT5S3::pca9535Present()) BoardT5S3::beginI2C();
    if (BoardT5S3::pca9535Present()) {
      BoardT5S3::setPca9535PinMode(PCA9535_IO12_BUTTON, INPUT);
      InputManager::setButtonHook(userButtonHook);
      LOG_INF("BTN", "User button: tap = Confirm, hold %lu ms = frontlight", USER_BUTTON_HOLD_MS);
    } else {
      LOG_ERR("BTN", "PCA9535 not answering: user button stays dead");
    }
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

#if FREEINK_DEVICE_LILYGO
  // Before Storage.begin() on purpose: a powered SX1262 corrupts the card's
  // very first transfer, so this has to win the race with SD detection.
  t5s3ParkLoraOffSdBus();
#endif

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
  // Restore the light the rider left on. Deliberately after loadFromFile() and
  // not next to frontlight.begin(): the settings file is not read until here.
  // setBrightness() first in both branches, because that is what seeds the
  // manager's "last brightness" -- off() alone would leave a later toggle
  // restoring the SDK's 50 % default instead of the level actually saved.
  if (frontlight.present()) {
    frontlight.setBrightness(SETTINGS.frontlightBrightness);
    if (!SETTINGS.frontlightOn) frontlight.off();
  }
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
  // The second way to the light: a hold on the capacitive home key below the
  // panel, on any board that has one. Handled here rather than in an activity so
  // every screen has it, and before activityManager.loop() so the screen on top
  // cannot consume it first. The SDK suppresses the key's tap once the hold
  // fires (InputManager::serviceTouch), so a hold never also selects.
  if (gpio.wasHomeKeyLongPressed()) {
    toggleFrontlight("Home key hold");
  }
  if (frontlightStateChanged) {
    frontlightStateChanged = false;
    SETTINGS.frontlightOn = frontlight.brightness() > 0 ? 1 : 0;
    if (frontlight.brightness() > 0) SETTINGS.frontlightBrightness = frontlight.brightness();
    // One SD write per deliberate hold, never per poll. Same rule the map's
    // ladder state follows (CrossPointSettings.h): a rider toggles the light a
    // handful of times a ride, so this is not an every-interaction write.
    SETTINGS.saveToFile();
  }
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
#ifdef ENABLE_SETTING_CMD
      } else if (cmd.startsWith("SETTING ")) {
        // Flip one of the map's opt-in toggles from the host: bench tests cannot
        // press buttons, and the two features worth testing unattended -- tile
        // autosync and the freshness check -- are both off by default because
        // they spend the rider's mobile data. Without this, testing them means a
        // human walking the Settings menu before every run.
        //
        // Deliberately a short allow-list rather than a generic settings poke:
        // this is a serial backdoor into persisted state, so it can reach exactly
        // the toggles a test needs and nothing else.
        //
        // Gated on ENABLE_SETTING_CMD, its own bench-only flag, NOT on
        // ENABLE_SERIAL_LOG: that one is set in gh_release and gh_release_rc too
        // (only slim clears it), so until 2026-09-02 this backdoor shipped in
        // both release builds while the comment here claimed it did not. Anyone
        // who picks up a lost device and plugs in USB can reach it: the reply
        // costs the rider mobile data (mapAutoSyncTiles, mapTileFreshnessMode)
        // or paints their exact position on the panel (mapDebugInfo), and the
        // write persists to the card. Do not re-tie it to a logging flag.
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
#ifdef ENABLE_GNSS_CMD
        // Only on a build that has a receiver: elsewhere the field exists but
        // nothing reads it, and answering SETTING_OK for a toggle that cannot
        // do anything is worse than answering SETTING_ERR:unknown.
        else if (key == "mapGnssPosition")
          target = &SETTINGS.mapGnssPosition;
        else if (key == "mapGnssLog")
          target = &SETTINGS.mapGnssLog;
#endif
        if (target == nullptr) {
          logSerial.printf("SETTING_ERR:unknown\n");
        } else if (value.length() == 0) {
          logSerial.printf("SETTING_OK:%s=%u\n", key.c_str(), static_cast<unsigned>(*target));
        } else {
          *target = static_cast<uint8_t>(value.toInt());
          SETTINGS.saveToFile();
          logSerial.printf("SETTING_OK:%s=%u\n", key.c_str(), static_cast<unsigned>(*target));
        }
#endif  // ENABLE_SETTING_CMD
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
            // Persist what the console set, so the instrument and the button
            // cannot disagree about what "the light" is after a reboot.
            SETTINGS.frontlightOn = pct > 0 ? 1 : 0;
            if (pct > 0) SETTINGS.frontlightBrightness = static_cast<uint8_t>(pct);
            SETTINGS.saveToFile();
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
        //   CMD:GNSS RELEASE   ->  GNSS_RELEASE:... (writes the rail pin, step 2a)
        //   CMD:GNSS LOG       ->  GNSS_LOG:...    (sizes of the fix log, never its rows)
        //
        // Reading the reply: `ttff` is NOT an acquisition time on a receiver
        // that was already running -- Gnss::timeToFirstFixMs() spells out why
        // anything under about 1.2 s means only "already tracking".
        //
        // Three counters say whether the rest of the line can be believed, and
        // they are not the same claim. `rxfull` is this firmware's own guess
        // that the ring came close to full, so it fires on a stall that lost
        // nothing. `ovf` is the driver saying the ring actually refused bytes,
        // and `fifoovf` is the driver saying bytes were dropped on the floor.
        // Non-zero `ovf` or `fifoovf` means every other count in the line is an
        // undercount; all three zero across a window whose sentence count also
        // matches the receiver's baseline rate is what "nothing was lost" looks
        // like. `rxbuf` is the ring the driver actually granted, which is not
        // always the size that was asked for.
        String argument = cmd.substring(4);
        argument.trim();
        argument.toUpperCase();

        if (argument == "ON") {
          if (gnssStart()) {
            logSerial.printf("GNSS_OK:on\n");
          } else {
            logSerial.printf("GNSS_ERR:power rail or expander unavailable\n");
          }
        } else if (argument == "LOG") {
          // "Did the ride record?" -- a question with a wrong answer available,
          // which is the point. Sizes only, never rows: the file is the rider's
          // track and printing it would hand a position log to anyone with a
          // cable.
          uint32_t onCard = 0;
          uint32_t buffered = 0;
          bool loggingDisabled = false;
          GnssLog::status(onCard, buffered, loggingDisabled);
          logSerial.printf("GNSS_LOG:setting=%u bytes=%lu buffered=%lu disabled=%d path=%s\n",
                           static_cast<unsigned>(SETTINGS.mapGnssLog), static_cast<unsigned long>(onCard),
                           static_cast<unsigned long>(buffered), loggingDisabled ? 1 : 0, GnssLog::kPath);
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
                static_cast<unsigned long>(gnss.sentencesParsed()), static_cast<unsigned long>(gnss.checksumErrors()),
                static_cast<unsigned long>(gnss.framingErrors()));
            gnss.end();  // powerEnable is null, so this touches no rail
          }
        } else if (argument == "RELEASE") {
          // Step 2a of docs/gnss-to-map-plan.md, and it replaces the power-cycle
          // route rather than adding to it. PROBE answers a proxy -- what
          // direction the expander pin has -- and four attempts at reading that
          // proxy on a "cold" boot failed for reasons that had nothing to do with
          // the rail. The real question is whether anything OTHER than the
          // expander holds LORA_GPS_EN high. Stop the expander driving it, and
          // ask the receiver:
          //
          //   NMEA keeps flowing -> something on the board holds the rail, so the
          //                         receiver is powered by design.
          //   NMEA stops         -> the expander's own latched output was holding
          //                         it, and no reset has ever cleared that latch.
          //
          // Its own subcommand because it writes device state, and named so
          // nobody reaches for it while looking for a read.
          //
          // Three windows, not one. The baseline proves the receiver was
          // streaming BEFORE the release, so a silent middle window means the
          // release stopped it rather than that nothing was ever running -- the
          // failure mode that would otherwise read as a clean answer. The restore
          // window proves the test left the board as it found it.
          //
          // Each window reports the CONFIG0 readback beside its byte count,
          // because an I2C write that silently did not take would show "NMEA
          // still flows" and look exactly like the by-design answer.
          uint8_t cfgBase = 0;
          if (!gnssReadExpanderRegister(0x06, &cfgBase)) {
            logSerial.printf("GNSS_RELEASE_ERR:expander read failed\n");
          } else {
            // powerEnable stays null for the same reason PROBE leaves it null:
            // the rail must not be written by the very code that is measuring it.
            // LORA_RST is deliberately left alone too, so this differs from the
            // steady state in exactly one bit -- the one under test.
            GnssConfig probe;
            probe.serial = &Serial1;
            probe.rxPin = T5S3_GPS_RXD;
            probe.txPin = T5S3_GPS_TXD;
            probe.baud = 9600;
            probe.powerEnable = nullptr;
            probe.powerSettleMs = 0;
            gnss.begin(probe);

            unsigned long bytesBefore = 0;
            unsigned long sentBefore = 0;
            const auto sample = [&](unsigned long windowMs, unsigned long* bytesOut, unsigned long* sentOut) {
              const unsigned long until = millis() + windowMs;
              while (millis() < until) {
                gnss.poll();
              }
              const unsigned long bytesNow = static_cast<unsigned long>(gnss.bytesRead());
              const unsigned long sentNow = static_cast<unsigned long>(gnss.sentencesParsed());
              *bytesOut = bytesNow - bytesBefore;
              *sentOut = sentNow - sentBefore;
              bytesBefore = bytesNow;
              sentBefore = sentNow;
            };

            unsigned long baseBytes = 0, baseSent = 0;
            sample(3000, &baseBytes, &baseSent);

            // Direction only. The output register is left holding whatever it
            // held, so the restore below can put the pin back without guessing.
            const bool released = BoardT5S3::setPca9535PinMode(PCA9535_IO00_LORA_GPS_EN, INPUT);
            uint8_t cfgReleased = 0;
            const bool haveReleased = gnssReadExpanderRegister(0x06, &cfgReleased);

            // 5 s, not 3: the receiver's own supply has bulk capacitance, and a
            // rail that is coasting down looks like a working receiver for the
            // first part of the window.
            unsigned long offBytes = 0, offSent = 0;
            sample(5000, &offBytes, &offSent);

            // Level before direction, the same order gnssPowerEnable() uses and
            // for the same reason: switching to output first would drive
            // whatever the output register happens to hold.
            const bool wroteLevel = BoardT5S3::writePca9535Pin(PCA9535_IO00_LORA_GPS_EN, true);
            const bool restored = BoardT5S3::setPca9535PinMode(PCA9535_IO00_LORA_GPS_EN, OUTPUT);
            uint8_t cfgRestored = 0;
            const bool haveRestored = gnssReadExpanderRegister(0x06, &cfgRestored);

            unsigned long backBytes = 0, backSent = 0;
            sample(4000, &backBytes, &backSent);

            logSerial.printf(
                "GNSS_RELEASE:reset=%s cfg0_base=0x%02X cfg0_released=0x%02X cfg0_restored=0x%02X "
                "wrote=%d released=%d restored=%d "
                "base_bytes=%lu base_sent=%lu off_bytes=%lu off_sent=%lu back_bytes=%lu back_sent=%lu\n",
                gnssResetReasonName(), cfgBase, haveReleased ? cfgReleased : 0xEE,
                haveRestored ? cfgRestored : 0xEE, wroteLevel ? 1 : 0, released ? 1 : 0, restored ? 1 : 0,
                baseBytes, baseSent, offBytes, offSent, backBytes, backSent);
            gnss.end();  // powerEnable is null, so this touches no rail
          }
        } else if (argument == "RAW ON" || argument == "RAW") {
          gnss.setRawSink(gnssRawSink);
          logSerial.printf("GNSS_OK:raw=1\n");
        } else if (argument == "RAW OFF") {
          gnss.setRawSink(nullptr);
          logSerial.printf("GNSS_OK:raw=0\n");
        } else if (argument.length() > 0) {
          logSerial.printf("GNSS_ERR:expected ON, OFF, PROBE, RELEASE, RAW ON or RAW OFF\n");
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
                "hdop=%.2f speed=%.1f course=%.1f utc=%lu ttff=%lu age=%lu uptime=%lu sent=%lu cserr=%lu "
                "ferr=%lu rxfull=%lu ovf=%lu fifoovf=%lu rxbuf=%lu bytes=%lu\n",
                static_cast<unsigned>(fix.quality), static_cast<unsigned>(fix.satsUsed),
                static_cast<unsigned>(gnss.satsInView()), static_cast<unsigned>(gnss.satsWithSignal()),
                static_cast<unsigned>(gnss.bestSnr()), fix.latitude, fix.longitude, fix.altitudeMeters, fix.hdop,
                fix.speedKmh, fix.courseDegrees, static_cast<unsigned long>(fix.utc),
                static_cast<unsigned long>(gnss.timeToFirstFixMs()), static_cast<unsigned long>(gnss.fixAgeMs()),
                static_cast<unsigned long>(gnss.uptimeMs()), static_cast<unsigned long>(gnss.sentencesParsed()),
                static_cast<unsigned long>(gnss.checksumErrors()), static_cast<unsigned long>(gnss.framingErrors()),
                static_cast<unsigned long>(gnss.rxNearlyFullEvents()), static_cast<unsigned long>(gnss.ringOverflows()),
                static_cast<unsigned long>(gnss.fifoOverflows()), static_cast<unsigned long>(gnss.rxBufferSize()),
                static_cast<unsigned long>(gnss.bytesRead()));
          } else {
            logSerial.printf(
                "GNSS_NOFIX:q=%u inview=%u tracked=%u bestsnr=%u utc=%lu uptime=%lu sent=%lu cserr=%lu "
                "ferr=%lu rxfull=%lu ovf=%lu fifoovf=%lu rxbuf=%lu bytes=%lu\n",
                static_cast<unsigned>(fix.quality), static_cast<unsigned>(gnss.satsInView()),
                static_cast<unsigned>(gnss.satsWithSignal()), static_cast<unsigned>(gnss.bestSnr()),
                static_cast<unsigned long>(fix.utc), static_cast<unsigned long>(gnss.uptimeMs()),
                static_cast<unsigned long>(gnss.sentencesParsed()), static_cast<unsigned long>(gnss.checksumErrors()),
                static_cast<unsigned long>(gnss.framingErrors()), static_cast<unsigned long>(gnss.rxNearlyFullEvents()),
                static_cast<unsigned long>(gnss.ringOverflows()), static_cast<unsigned long>(gnss.fifoOverflows()),
                static_cast<unsigned long>(gnss.rxBufferSize()), static_cast<unsigned long>(gnss.bytesRead()));
          }
        }
#endif
#ifdef ENABLE_BATT_CMD
      } else if (cmd == "BATT") {
        // The gauge's own numbers, on demand, for a power run whose other half
        // is a meter on VBUS.
        //
        // **Why this exists at all.** A USB meter reads the board *plus* the
        // charger, so a VBUS number is not board draw while a cell is charging
        // behind it (parent docs/usb-power-meter.md). Subtracting the gauge's
        // average current is one of the three ways round that, and it needs the
        // number at the same instant as the meter reading -- which means on
        // demand from the host, not once a minute in a log.
        //
        // **Why it re-reads the registers instead of asking BatteryMonitor.**
        // The SDK already reads all three (freeink-sdk BatteryMonitor.cpp:211
        // reads 0x0C) and throws the current away: its public Status carries
        // percentage, millivolts and a charging bool, no current. Adding a field
        // there means editing freeink-sdk, which is upstream's repo and whose
        // submodule pointer stays on upstream main -- so this reads the same
        // registers from our side and the SDK stays untouched.
        //
        //   CMD:BATT  ->  BATT:mv=4102 pct=100 curr_ma=-38 chg=1 gauge=0x55 charger=0x6b
        //
        // A field that could not be read prints `?`. curr_ma is signed: TI's
        // sign convention is positive into the cell (charging), negative out of
        // it, which is why a charging board reports the opposite sign to what
        // "draw" suggests.
        //
        // Devel-only like the rest, and today only in env:t5s3pro -- the board
        // with the gauge that the power campaign is measuring. X3 carries a
        // BQ27220 too and would answer this on the C3 binary, so the flag is
        // worth widening when that measurement comes up; until then a C3 build
        // does not pay for it. It leaks nothing about the rider --
        // no position, no route, no identity -- so the reason is not secrecy: a
        // command with no UI behind it and one measurement session's worth of
        // use does not belong in a build a stranger flashes.
        const auto& g = BoardConfig::ACTIVE.batteryGauge;
        if (g.gaugeAddr == 0) {
          logSerial.printf("BATT_ERR:no gauge on this board\n");
        } else {
#if SOC_I2C_NUM > 1
          TwoWire& w = (g.i2cBus == 1) ? Wire1 : Wire;
#else
          TwoWire& w = Wire;
#endif
          // Same pins and clock the SDK uses, so re-begin reconfigures the bus
          // to what it already is rather than fighting it.
          w.begin(g.i2cSda, g.i2cScl, g.i2cHz);

          // TI command registers, values copied from the SDK's own table
          // (freeink-sdk BatteryMonitor.cpp, BQ27220_* / BQ25896_REG_STATUS) so
          // the two cannot drift apart silently.
          auto read16 = [&w](uint8_t addr, uint8_t reg, uint16_t& out) -> bool {
            w.beginTransmission(addr);
            w.write(reg);
            if (w.endTransmission(false) != 0) return false;
            if (w.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) return false;
            const uint8_t lo = w.read();
            const uint8_t hi = w.read();
            out = static_cast<uint16_t>(lo | (hi << 8));
            return true;
          };
          auto read8 = [&w](uint8_t addr, uint8_t reg, uint8_t& out) -> bool {
            w.beginTransmission(addr);
            w.write(reg);
            if (w.endTransmission(false) != 0) return false;
            if (w.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
            out = w.read();
            return true;
          };

          uint16_t mv = 0, pct = 0, rawCurrent = 0;
          uint8_t chargerStatus = 0;
          const bool mvOk = read16(g.gaugeAddr, 0x08, mv);
          const bool pctOk = read16(g.gaugeAddr, 0x2C, pct);
          const bool currOk = read16(g.gaugeAddr, 0x0C, rawCurrent);
          const bool chgOk = g.chargerAddr != 0 && read8(g.chargerAddr, 0x0B, chargerStatus);

          char mvText[12] = "?";
          char pctText[12] = "?";
          char currText[12] = "?";
          char chgText[12] = "?";
          if (mvOk) snprintf(mvText, sizeof(mvText), "%u", static_cast<unsigned>(mv));
          if (pctOk) snprintf(pctText, sizeof(pctText), "%u", static_cast<unsigned>(pct));
          if (currOk) snprintf(currText, sizeof(currText), "%d", static_cast<int>(static_cast<int16_t>(rawCurrent)));
          if (chgOk) snprintf(chgText, sizeof(chgText), "%u", static_cast<unsigned>((chargerStatus >> 3) & 0x03));
          logSerial.printf("BATT:mv=%s pct=%s curr_ma=%s chg=%s gauge=0x%02X charger=0x%02X\n", mvText, pctText,
                           currText, chgText, static_cast<unsigned>(g.gaugeAddr), static_cast<unsigned>(g.chargerAddr));
        }
#endif
#ifdef ENABLE_SDBUS_CMD
      } else if (cmd == "SDBUS" || cmd.startsWith("SDBUS ")) {
        // Bench instrument for BUG-037: toggle the three things
        // t5s3ParkLoraOffSdBus() does, one at a time, and read a file back with
        // a CRC after each. It exists because the fix writes all three at once
        // and the hardware runs never separated them.
        //
        //   CMD:SDBUS               ->  SDBUS:cs=1 rst=0 rail=0
        //   CMD:SDBUS CS 0|1        ->  same reply, after the write
        //   CMD:SDBUS RST 0|1
        //   CMD:SDBUS RAIL 0|1
        //   CMD:SDBUS READ <path>   ->  SDBUS_READ:<path> bytes=<n> crc32=<hex> ms=<n>
        //
        // `cs` is the radio's chip select (GPIO46): 1 means deselected, which is
        // what the fix sets. `rst` is LORA_RST (GPIO1): 0 holds the radio in
        // reset. `rail` is the expander pin that powers the GNSS receiver and
        // the radio together. All three are reported as read back from the pin,
        // not from what we last wrote.
        //
        // **Read-only on purpose.** No write, no mkdir, no settings save. With
        // the bus deliberately broken a write allocates from a misread FAT and
        // can land anywhere, and that already happened once on this card
        // (parent docs/BUGS.md, BUG-037, the two fsck fragments). The question
        // this answers -- does the card come back -- a read answers.
        //
        // **What a run cannot rule out:** SdFat caches directory and FAT blocks,
        // so a read that follows a successful one is not entirely off the card.
        // Prefer a file big enough to force data blocks, and treat a *failure*
        // as the strong signal rather than a success.
        //
        // Devel-only, and t5s3pro only, for two reasons rather than one:
        // `RAIL 1` powers a radio, and leaving `CS 0` behind breaks the card
        // until something puts it back. Neither belongs in a build a stranger
        // flashes.
        String rest = cmd.substring(5);
        rest.trim();

        auto reportState = [&]() {
          bool railHigh = false;
          const bool railOk =
              BoardT5S3::pca9535Present() && BoardT5S3::readPca9535Pin(PCA9535_IO00_LORA_GPS_EN, &railHigh);
          char railText[4] = "?";
          if (railOk) snprintf(railText, sizeof(railText), "%d", railHigh ? 1 : 0);
          logSerial.printf("SDBUS:cs=%d rst=%d rail=%s\n", digitalRead(T5S3_LORA_CS) ? 1 : 0,
                           digitalRead(T5S3_LORA_RST) ? 1 : 0, railText);
        };

        if (rest.isEmpty()) {
          reportState();
        } else if (rest.startsWith("READ ")) {
          String path = rest.substring(5);
          path.trim();
          if (path.isEmpty()) {
            logSerial.printf("SDBUS_READ_ERR:no path\n");
          } else {
            HalFile f;
            if (!Storage.openFileForRead("SDBUS", path, f)) {
              logSerial.printf("SDBUS_READ_ERR:%s open failed\n", path.c_str());
            } else {
              // 512 to match the card's own block size, so a chunk boundary
              // never straddles two blocks and the byte count is what the bus
              // actually delivered.
              uint8_t buf[512];
              uint32_t crc = 0;
              size_t total = 0;
              const unsigned long t0 = millis();
              int n = 0;
              while ((n = f.read(buf, sizeof(buf))) > 0) {
                crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
                total += static_cast<size_t>(n);
              }
              const unsigned long ms = millis() - t0;
              f.close();
              // A negative read is a bus failure mid-file and must not look like
              // a short file: the count and the CRC are both meaningless then.
              if (n < 0) {
                logSerial.printf("SDBUS_READ_ERR:%s read failed after %u bytes\n", path.c_str(),
                                 static_cast<unsigned>(total));
              } else {
                logSerial.printf("SDBUS_READ:%s bytes=%u crc32=%08lx ms=%lu\n", path.c_str(),
                                 static_cast<unsigned>(total), static_cast<unsigned long>(crc), ms);
              }
            }
          }
        } else {
          const int sp = rest.lastIndexOf(' ');
          const String what = (sp < 0) ? rest : rest.substring(0, sp);
          const String valText = (sp < 0) ? String() : rest.substring(sp + 1);
          if (valText != "0" && valText != "1") {
            logSerial.printf("SDBUS_ERR:want 0 or 1\n");
          } else {
            const bool high = (valText == "1");
            if (what == "CS") {
              pinMode(T5S3_LORA_CS, OUTPUT);
              digitalWrite(T5S3_LORA_CS, high ? HIGH : LOW);
              reportState();
            } else if (what == "RST") {
              pinMode(T5S3_LORA_RST, OUTPUT);
              digitalWrite(T5S3_LORA_RST, high ? HIGH : LOW);
              reportState();
            } else if (what == "RAIL") {
              if (!BoardT5S3::pca9535Present()) {
                BoardT5S3::beginI2C();
              }
              if (!BoardT5S3::pca9535Present()) {
                logSerial.printf("SDBUS_ERR:PCA9535 silent\n");
              } else {
                // Level before direction, the same order t5s3ParkLoraOffSdBus()
                // and disableGpsLora() use: switching to output first would
                // drive whatever the output register happens to hold.
                const bool wroteLevel = BoardT5S3::writePca9535Pin(PCA9535_IO00_LORA_GPS_EN, high);
                const bool wroteDir = BoardT5S3::setPca9535PinMode(PCA9535_IO00_LORA_GPS_EN, OUTPUT);
                if (!wroteLevel || !wroteDir) logSerial.printf("SDBUS_ERR:rail write failed\n");
                // The rail needs a moment before the parts on it settle, and
                // this is a hand-driven bench command, so it can afford to wait.
                delay(50);
                reportState();
              }
            } else {
              logSerial.printf("SDBUS_ERR:want CS, RST, RAIL or READ\n");
            }
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
