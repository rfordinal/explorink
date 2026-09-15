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
#include <BlePositionServer.h>  // gnssStart() asks it for a clock to seed with
#include <Gnss.h>
#include <Wire.h>
#include <esp_system.h>
#endif

#ifdef ENABLE_CHARGE_CMD
#include <Wire.h>  // the charger and the gauge sit on the same I2C bus
#endif

#ifdef ENABLE_BLE_CMD
#include <BlePositionServer.h>
#endif

#ifdef ENABLE_WIFI_CMD
// CMD:WIFI CONNECT reads the network the menu last joined. Read-only: a bench
// run must not be able to change which network the device prefers.
#include "WifiCredentialStore.h"
#endif

#ifdef ENABLE_SDBUS_CMD
#include <esp_rom_crc.h>
#endif

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DebugInput.h"
#include "DebugTouchLog.h"

// Every input sample in loop() goes through here so the gap between two of them
// can be measured. The gap IS the bug being chased: a GT911 holds exactly one
// unacknowledged frame and discards everything after it, so a wide gap turns a
// double tap into a single one (src/DebugTouchLog.h). Hooking the six call sites
// rather than HalGPIO::update() keeps the recorder in src/, which lib/hal cannot
// include and which the simulator replaces wholesale.
static inline void sampleInput() {
#ifdef ENABLE_TOUCHLOG_CMD
  DebugTouchLog::noteUpdate();
#endif
  gpio.update();
}
#include "GnssAccess.h"
#include "GnssFakeSky.h"
#include "GnssLog.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "MissingTilesStore.h"
#include "OpdsServerStore.h"
#include "PowerLog.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "TouchPolicy.h"
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

// True while a held button is still walking the frontlight rungs. loop() waits
// for it to clear before it writes the level to the card: a hold steps every
// 500 ms and each step would otherwise be its own SD write, on the input path,
// for a level the rider is still choosing.
bool frontlightHoldActive = false;

// The rungs a held user button walks through, off included. A cycle rather than
// an on/off toggle because the panel needs very different amounts of light at
// dusk and in full dark, and there is no other control for it on this board: no
// frontlight row in Settings, and touch is what gloves defeat.
//
// 10 % is the bottom rung on purpose: it is enough to read the panel in a dark
// tent and it is the one setting a rider can leave on for hours. 100 % costs
// about 43 mA off the cell at 40 % already (docs/devices/lilygo-t5-s3-pro.md),
// so the top rung is a look-at-it-now rung, not a ride setting.
constexpr uint8_t FRONTLIGHT_RUNGS[] = {0, 10, 30, 60, 100};

// The user button's hold. The home key keeps its own plain on/off below: the two
// inputs deliberately do different things now, because the key is the one a
// glove cannot reach and "give me light" is the gesture worth having there.
//
// Steps to the first rung strictly above the current brightness, wrapping to
// off. Comparing against the live brightness rather than a stored index is what
// keeps a value that is on no rung -- a settings.json from an older build, or a
// CMD:LIGHT during bring-up -- from stalling the cycle: 50 % steps to 60 %.
void cycleFrontlight(const char* source) {
  if (!frontlight.present()) return;
  const uint8_t current = frontlight.brightness();
  uint8_t next = 0;
  for (const uint8_t rung : FRONTLIGHT_RUNGS) {
    if (rung > current) {
      next = rung;
      break;
    }
  }
  frontlight.setBrightness(next);
  frontlightStateChanged = true;
  LOG_INF("BTN", "%s: frontlight %u%%", source, static_cast<unsigned>(frontlight.brightness()));
}

// The home key's hold: off from anywhere, on at the level in Settings. It reads
// SETTINGS.frontlightBrightness rather than FrontlightManager's own remembered
// level, so the Settings row and the user button's rungs are the only things
// that decide how bright "on" is -- one number, three ways to set it.
void toggleFrontlight(const char* source) {
  if (!frontlight.present()) return;
  if (frontlight.brightness() > 0) {
    frontlight.setBrightness(0);
  } else {
    frontlight.setBrightness(SETTINGS.frontlightBrightness);
  }
  frontlightStateChanged = true;
  LOG_INF("BTN", "%s: frontlight %u%%", source, static_cast<unsigned>(frontlight.brightness()));
}

// Not inside the T5 S3 Pro's button block below, and that is the point: any
// board with a capacitive home key and a digitizer carries this gesture
// (TouchPolicy::homeKeyDoubleTapLocksTouch()), the X4 Pro included.
void toggleTouchLock() {
  // One flag, flipped. Nothing has to be remembered across it: the mode the
  // rider chose lives in SETTINGS.touchMode and the lock never touches it, so
  // unlocking simply stops overriding it (TouchPolicy::mode()). The earlier
  // version stored DISABLED *into* touchMode and kept the previous value in RAM,
  // which lost it across a reboot and put a value in that field that the
  // Settings row does not list.
  SETTINGS.touchLocked = SETTINGS.touchLocked != 0 ? 0 : 1;
  // One SD write per deliberate tap, the same reasoning toggleFrontlight() above
  // carries: a handful of writes a ride, not one per interaction.
  SETTINGS.saveToFile();
  // The hint boxes appear or vanish with the mode and the layout reserves room
  // for them or does not, so the screen is repainted rather than nudged.
  activityManager.requestUpdate();
  LOG_INF("BTN", "Home key: touch %s", SETTINGS.touchLocked != 0 ? "locked" : "unlocked");
}

// How long BOOT must be held before it means sleep. On the T5 S3 Pro a shorter
// press means Back (boardButtonHook() below), so the two gestures share one
// number and it has to be long enough to tap deliberately with gloves on:
// 400 ms, the setting's own answer, is a window a rider misses and sleeps the
// device instead of stepping back.
//
// Wake is passed the same number, but it does not mean the same thing:
// verifyPowerButtonWakeup() subtracts the time already spent booting
// (HalGPIO.cpp:212-213), so what a wake actually requires is that the button is
// still down when setup() reaches that check -- 1 ms of it if boot took longer
// than 1500 ms. Sleep is the only gesture this number really gates.
uint16_t powerHoldDurationMs() {
#if FREEINK_DEVICE_LILYGO
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::LilyGoT5S3) return 1500;
#endif
  return SETTINGS.getPowerButtonDuration();
}
}  // namespace

#if FREEINK_DEVICE_LILYGO
// ===========================================================================
// THE T5 S3 PRO'S BUTTON MAP. Read this before changing any of it.
// ===========================================================================
//
// Three sessions in a row mis-identified which switch is which here, because the
// silkscreen, the schematic and the firmware each use a different name for the
// same thing. The canonical hardware page is the parent repo's
// docs/devices/lilygo-t5-s3-pro.md, "The four physical buttons"; this table is
// what the firmware actually does with them.
//
// | Physical            | Schematic       | Reaches the MCU as    | Firmware job     |
// |---------------------|-----------------|-----------------------|------------------|
// | BOOT, left, top     | S2, net IO0     | GPIO0 = input.power   | tap = Back       |
// |                     |                 |                       | hold 1500ms =    |
// |                     |                 |                       |  sleep, wake too |
// | IO48 silkscreen,    | S3, net BUTTON  | PCA9535 U1 (0x20)     | tap = Confirm    |
// |   left, bottom      |                 |   pin IO1_0, polled   | hold 600ms = the |
// |   ("the user        |                 |   by boardButtonHook()|   next frontlight|
// |    button")         |                 |   below               |   rung           |
// | RST, right, top     | S1, net RST/EN  | nothing -- it is the  | none, and never  |
// |                     |                 |   hardware reset pin  |   readable       |
// | PWR, right, bottom  | S4, BQ25896 QON | nothing -- no MCU or  | none, and never  |
// |                     |                 |   expander pin at all |   readable       |
//
// **"The user button", "S3", "the IO48 one" and "the left bottom button" are all
// the same single switch.** It is NOT a home button, and GPIO48 has no switch on
// it anywhere in the schematic (GPIO48 is EP_CKV, the panel bus clock).
//
// **The capacitive home key is a fifth, separate input** and is not one of the
// four above. It is not a GPIO at all: the GT911 reports it in its own status
// byte, bit 0x10, and InputManager::serviceTouch() reads that bit on every board
// **regardless of TouchConfig::hasHomeKey** -- that flag is consulted nowhere in
// InputManager, so do not go looking for it as the switch that turns this key
// on. It is not unused, though: `TouchPolicy::homeKeyDoubleTapLocksTouch()`
// reads it through `BoardConfig::hasHomeKey()` to decide whether the key carries
// three gestures or one. Confirmed working on this panel 2026-09-05 (holding it
// turns the frontlight on). Its jobs are handled in loop(), not here:
//
//   home key tap        -> Confirm (Select), after the double-tap window
//   home key double tap -> lock / unlock the touch panel (toggleTouchLock)
//   home key hold       -> frontlight on / off (toggleFrontlight)
//
// **None of those three is specific to this board any more.** They are keyed on
// having a home key and a digitizer, so the X4 Pro gets all three; the table
// above is about the four physical switches, which really are this board's.
//
// Why the light hangs off a physical hold and not a touch control: gloves defeat
// the capacitive panel, and the light is exactly what a rider reaches for with
// gloves on. Why Back is on BOOT (2026-09-07): without it this board has no way
// out of a screen except touch, which is the input a glove removes, and BOOT's
// short press was doing nothing here -- shortPwrBtn defaults to IGNORE. Sleep
// and Back are now the same press told apart by how long it is held, which is
// what powerHoldDurationMs() above sets. Why the lock hangs off a double tap:
// nothing else on a touch board can stop the glass reacting to a bag, a palm or
// rain, and the single tap was worth keeping as Select. The cost is that Select
// through this key waits out the double-tap window -- a single tap cannot be
// known to be single until then.
//
// And while the lock is on, that single tap does not select at all
// (`MappedInputManager::pumpHomeKey()`), so the double tap is the only way out
// of it. On the X4 Pro that is not a detail: Back and Confirm both come from
// touch there, so a lock with no working unlock gesture would be a dead device.
namespace {
constexpr unsigned long USER_BUTTON_HOLD_MS = 600;
// A held button keeps stepping the light at this rate. Slow enough to let go on
// the rung you meant (five rungs take 2.6 s end to end), fast enough that
// walking the whole cycle is not a chore. There is no SD write per step: the
// hold sets a flag (frontlightHoldActive) and loop() saves the level once, once
// the button is up.
constexpr unsigned long USER_BUTTON_REPEAT_MS = 500;

// A synthetic press has to survive InputManager's debounce, which commits a
// state change only once two update() calls at least DEBOUNCE_DELAY (5 ms)
// apart saw the same state (InputManager.cpp, update()). Counting polls rather
// than wall time is what makes this survive a panel refresh: a millisecond
// window would expire unobserved while the main loop sits in a multi-second
// redraw, and the tap would be silently dropped. Both conditions must hold, so
// the pulse is long enough in time AND seen often enough.
constexpr uint8_t SYNTHETIC_CLICK_POLLS = 3;
constexpr unsigned long SYNTHETIC_CLICK_MS = 20;

// One synthetic press in flight at a time, as a key bitmask. Two gestures cannot
// overlap on a board with two buttons and one thumb, and if they did, the newer
// one is the one the rider meant.
uint8_t syntheticClickMask = 0;
uint8_t syntheticClickPolls = 0;
unsigned long syntheticClickSince = 0;

// Both taps are reported *after* release, never on the press edge: on either
// button the press could still turn out to be a hold, and an activity that acted
// at touch-down would have acted before the gesture was known.
void beginSyntheticClick(uint8_t button, unsigned long now) {
  syntheticClickMask = static_cast<uint8_t>(1U << button);
  syntheticClickPolls = 0;
  syntheticClickSince = now;
}

bool userButtonDown = false;
bool userButtonLongFired = false;
unsigned long userButtonDownAt = 0;
unsigned long userButtonRungAt = 0;

bool powerButtonLevelKnown = false;
bool powerButtonDown = false;
unsigned long powerButtonDownAt = 0;

// Runs inside InputManager::update() (one call per poll), i.e. in whatever task
// drives the main loop. Reads one PCA9535 input register over I2C; BoardT5S3
// takes the bus mutex for us, so this is safe next to the panel's own expander
// writes. The BOOT read is a plain digitalRead of the same pin and polarity
// InputManager samples for BTN_POWER (InputManager.cpp, getDigitalState()) --
// this only adds a meaning to it, it does not take the power button away.
uint8_t boardButtonHook() {
  const unsigned long now = millis();
  const bool down = BoardT5S3::readButton();

  if (down && !userButtonDown) {
    userButtonDown = true;
    userButtonLongFired = false;
    userButtonDownAt = now;
    // Drop a tap still being reported: a second press starting inside that
    // window would otherwise be seen as Confirm held down.
    syntheticClickMask = 0;
  } else if (down && now - userButtonDownAt >= USER_BUTTON_HOLD_MS &&
             (!userButtonLongFired || now - userButtonRungAt >= USER_BUTTON_REPEAT_MS)) {
    // Fires the moment the hold is long enough, not on release: the light
    // changes under the thumb, which is the feedback that says "let go now".
    // Then it keeps stepping while the button stays down, so a rider walks to
    // the rung they want with one press instead of four -- the light itself is
    // the readout, and letting go is how they stop.
    userButtonLongFired = true;
    userButtonRungAt = now;
    frontlightHoldActive = true;
    cycleFrontlight("User button hold");
  } else if (!down && userButtonDown) {
    userButtonDown = false;
    frontlightHoldActive = false;
    if (!userButtonLongFired) beginSyntheticClick(InputManager::BTN_CONFIRM, now);
  }

  const bool powerDown =
      digitalRead(BoardConfig::ACTIVE.input.power) == (BoardConfig::ACTIVE.input.powerActiveHigh ? HIGH : LOW);
  if (!powerButtonLevelKnown) {
    // The first poll after install lands while the button that woke the device
    // may still be held. Adopt the level instead of calling it a press edge, or
    // every wake would end in a Back the rider never asked for.
    powerButtonLevelKnown = true;
    powerButtonDown = powerDown;
  } else if (powerDown && !powerButtonDown) {
    powerButtonDown = true;
    powerButtonDownAt = now;
  } else if (!powerDown && powerButtonDown) {
    powerButtonDown = false;
    // A hold long enough to sleep never reaches here: loop() calls
    // enterDeepSleep() at the threshold, while the button is still down. The
    // check is for the case where it could not -- the two-second post-boot
    // sleep guard (allowSleepAt), or a screenshot combo -- where a long press
    // must not turn into a Back on release.
    if (now - powerButtonDownAt < powerHoldDurationMs()) {
      beginSyntheticClick(InputManager::BTN_BACK, now);
    }
  }

  if (!syntheticClickMask) return 0;
  ++syntheticClickPolls;
  if (syntheticClickPolls > SYNTHETIC_CLICK_POLLS && now - syntheticClickSince >= SYNTHETIC_CLICK_MS) {
    syntheticClickMask = 0;
    return 0;
  }
  return syntheticClickMask;
}
// --- Deselect the LoRa radio before the card comes up ----------------------
//
// The SD card and the SX1262 share one SPI bus on this board: MISO21 MOSI13
// SCLK14, with SD_CS12 against LORA_CS46 (BoardT5S3Pins.h). GPIO46 runs straight
// to the radio module's NSS with no pull-up, so nothing holds the radio
// deselected unless the firmware does.
//
// Measured 2026-09-03 with CMD:SDBUS, nine runs, each after a hard reset with a
// passing baseline read: with the radio deselected the card read correctly in
// every combination of rail and reset line. With it selected the read failed
// unless the radio was BOTH powered AND had its reset actively driven. So
// deselecting is the whole defence, and it is the only one this function needs.
//
// **Why this exists when freeink-sdk already fixes it.** The SDK's fix lives in
// prepareEpdPower(), which runs at display init -- and display init happens
// AFTER Storage.begin(). It covers the rest of the run; it cannot cover card
// detection. This covers card detection. Two windows, not two belts.
//
// The pin survives display init afterwards because lgfx::pinMode() writes no
// level for output mode, which is the same property that made the bug possible.
//
// **What this deliberately does NOT do.** It does not cut the shared GNSS/LoRa
// rail and it does not drive the radio's reset. An earlier version did both. The
// rail cut was wrong twice over: it is half of the condition that breaks the
// card, and it belongs to the battery question (T-244), not to this one. Do not
// couple them again.
void t5s3DeselectLoraRadio() {
  if (BoardConfig::ACTIVE.board != BoardConfig::Board::LilyGoT5S3) return;

  // Read before writing. The level here is the pad's reset state, which nothing
  // on disk documents for GPIO46 -- it is a strapping pin -- so this line is the
  // only place that number has ever been observed. It also makes a silent
  // regression loud: if the SDK fix is ever lost, this still saves the card and
  // the log still says the pin came up asserted.
  pinMode(T5S3_LORA_CS, INPUT);
  const int before = digitalRead(T5S3_LORA_CS);

  pinMode(T5S3_LORA_CS, OUTPUT);
  digitalWrite(T5S3_LORA_CS, HIGH);

  LOG_INF("SDBUS", "LORA_CS (GPIO%d) was %s at boot, now deselected", T5S3_LORA_CS,
          before ? "high" : "LOW -- the radio was selected on the card's bus");
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
// t5s3DeselectLoraRadio() above for why, and for why "every panel refresh
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
  if (!gnss.begin(config)) return false;

  // Ask for all three constellations before anything else. The module was
  // running GPS+GLONASS (mode 5): a raw capture on 2026-09-04 carried 45 GPGSV
  // and 45 GLGSV sentences and not one GBGSV, and `gnss.md` had already noted
  // the set is configurable and that the vendor lists BeiDou.
  //
  // **Why it matters here and not as a nicety: this antenna's problem is
  // satellites in view, and BeiDou is about 45 more of them.** A weak signal
  // does not need a better satellite, it needs more chances at four usable
  // ones. Modes are from the official CASIC spec, 1.6.5 CAS04:
  //   1 GPS, 2 BDS, 3 GPS+BDS, 4 GLONASS, 5 GPS+GLONASS, 6 BDS+GLONASS,
  //   7 GPS+BDS+GLONASS.
  //
  // Sent on every start rather than saved to the module's flash: it costs one
  // 16-byte sentence, it cannot drift out of step with this code, and it wears
  // nothing out. **Unverified that this module accepts mode 7** -- the spec says
  // the supported subset is per product model and does not list the L76K's.
  // The check is cheap and needs no sky: a `$GBGSV` in `CMD:GNSS RAW ON` means
  // it took.
  if (!gnss.sendNmeaSentence("PCAS04,7")) {
    LOG_ERR("GNSS", "constellation request not sent");
  }

  // Seed the receiver before it starts searching. **Without this a cold start
  // cannot finish at the signal level this board delivers** -- reading the
  // ephemeris off the air needs more signal than merely tracking a satellite
  // does, and this antenna sits between the two (Gnss::injectAidIni has the
  // numbers and where they come from). A 15-minute walk on 2026-09-04 got no
  // fix at all; the receiver was tracking one satellite the whole time.
  //
  // Position comes from the persisted last fix, which is the same value the map
  // opens its first frame on, so it is as good as the device has and costs
  // nothing to pass. 50 km of claimed accuracy is deliberately loose: it is a
  // fix from some earlier trip, and on 2026-09-04 that meant Bratislava while
  // the device was in Barcelona. A seed that lies about its accuracy is worse
  // than a wide one, because the receiver trusts it.
  //
  // Time only when something has it. A GNSS map session runs no BLE
  // (MapActivity's bleInUse_), and this board has no RTC ("RTC not found" at
  // boot), so utcNow() answers only when a phone set the clock earlier in this
  // same boot. Position-only aiding is still most of the win.
  if (SETTINGS.mapHasLastFix) {
    uint32_t utc = 0;
    const bool haveTime = freeink::BlePositionServer::getInstance().utcNow(utc);
    const double lat = static_cast<double>(SETTINGS.mapLastLatE7) / 1e7;
    const double lon = static_cast<double>(SETTINGS.mapLastLonE7) / 1e7;
    if (gnss.injectAidIni(lat, lon, haveTime, utc)) {
      LOG_INF("GNSS", "aiding sent: %.5f,%.5f, time %s", lat, lon, haveTime ? "included" : "not available");
    } else {
      LOG_ERR("GNSS", "aiding not sent, the frame was written short");
    }
  } else {
    // Worth a line rather than silence: this is the one case where the receiver
    // really does start from nothing, and it is the case that takes minutes.
    LOG_INF("GNSS", "no persisted fix to seed with, this is a cold start");
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

// CMD:GNSS RAW BYTES passthrough. T-210: a CASIC binary reply (e.g. an
// ACK-ACK, `BA CE ...`) has no '$' and no NMEA checksum, so gnssRawSink()
// above never sees it -- every decisive answer in T-209's bench is exactly
// this shape. Buffered rather than printed per byte: at ~800 B/s that would be
// 800 log lines a second even with nothing but ordinary NMEA flowing, since
// this sink sees every byte, not only reply bytes. Flushed on either a 32-byte
// line or a 50 ms gap since the last byte, which is generous against a 9600
// baud line's own byte time (~1 ms) and short against the pause between two
// unrelated sentences.
static uint8_t gGnssRawByteBuf[32];
static size_t gGnssRawByteLen = 0;
static unsigned long gGnssRawByteLastMs = 0;

static void gnssFlushRawBytes() {
  if (gGnssRawByteLen == 0) return;
  char hex[sizeof(gGnssRawByteBuf) * 3 + 1];
  size_t pos = 0;
  for (size_t i = 0; i < gGnssRawByteLen; ++i) {
    pos +=
        static_cast<size_t>(snprintf(hex + pos, sizeof(hex) - pos, "%02X ", static_cast<unsigned>(gGnssRawByteBuf[i])));
  }
  logSerial.printf("GNSS_RAWBYTES:%s\n", hex);
  gGnssRawByteLen = 0;
}

static void gnssRawByteSink(uint8_t b) {
  const unsigned long now = millis();
  if (gGnssRawByteLen > 0 && (now - gGnssRawByteLastMs) > 50) {
    gnssFlushRawBytes();
  }
  gGnssRawByteBuf[gGnssRawByteLen++] = b;
  gGnssRawByteLastMs = now;
  if (gGnssRawByteLen >= sizeof(gGnssRawByteBuf)) {
    gnssFlushRawBytes();
  }
}
#endif
#if defined(ENABLE_BATT_CMD) || defined(ENABLE_CHARGE_CMD)
// I2C for the two power chips on the gauge bus: the BQ27220 fuel gauge (0x55)
// and the BQ25896 charger (0x6B) on the T5 S3 Pro. Shared by CMD:BATT and
// CMD:CHARGE rather than written twice, because CMD:CHARGE's whole safety story
// is that every write is a read-modify-write and the two commands must not be
// able to drift into two different definitions of that.
//
// **Why this is not BatteryMonitor.** The SDK reads three of these registers
// and throws the rest away, and its repo is upstream's -- see CMD:BATT below
// for the full reason. Same bus, same pins, same clock, so re-begin()
// reconfigures the bus to what it already is (Wire.cpp: an already-initialised
// bus returns early).
namespace powerbus {

TwoWire& wire() {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
#if SOC_I2C_NUM > 1
  if (g.i2cBus == 1) return Wire1;
#endif
  (void)g;
  return Wire;
}

void begin() {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  wire().begin(g.i2cSda, g.i2cScl, g.i2cHz);
}

bool read8(uint8_t addr, uint8_t reg, uint8_t& out) {
  TwoWire& w = wire();
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  out = w.read();
  return true;
}

bool read16(uint8_t addr, uint8_t reg, uint16_t& out) {
  TwoWire& w = wire();
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) return false;
  const uint8_t lo = w.read();
  const uint8_t hi = w.read();
  out = static_cast<uint16_t>(lo) | static_cast<uint16_t>(hi << 8);
  return true;
}

bool write8(uint8_t addr, uint8_t reg, uint8_t value) {
  TwoWire& w = wire();
  w.beginTransmission(addr);
  w.write(reg);
  w.write(value);
  return w.endTransmission(true) == 0;
}

}  // namespace powerbus
#endif  // ENABLE_BATT_CMD || ENABLE_CHARGE_CMD

#ifdef ENABLE_CHARGE_CMD
// BQ25896 register writes for the bench, and the rules they obey.
//
// **Every write here is a read-modify-write of named bits, and there is no
// generic write command at all.** That is deliberate: the parent repo's
// docs/t5s3-power-path.md carries a "Never write these" table, and a `CMD:CHARGE
// REG 0x14 0x80` would put every one of them one typo away. REG14 bit 7
// (REG_RST) resets every register including BATFET_DIS; REG03 bit 5
// (OTG_CONFIG) drives 5 V back onto VBUS; REG00 bit 7 (EN_HIZ) drops the board
// onto the cell and makes a meter read nothing. None of them is reachable
// through this command, because no path here writes a byte a host chose.
//
// Registers and bit meanings are quoted from SLUSC76C (BQ25896, rev. C, May
// 2018), tables 9, 13, 15 and 20-24, via the parent repo's power-path doc.
namespace bq25896 {

constexpr uint8_t kRegAdcCtrl = 0x02;    // CONV_START bit 7, CONV_RATE bit 6
constexpr uint8_t kRegChargeCtrl = 0x03; // CHG_CONFIG bit 4 (bit 5 is OTG -- never touched)
constexpr uint8_t kRegWatchdog = 0x07;   // WATCHDOG[1:0] in bits 5:4
constexpr uint8_t kRegBatfet = 0x09;     // BATFET_DIS bit 5
constexpr uint8_t kRegStatus = 0x0B;     // VBUS_STAT 7:5, CHRG_STAT 4:3, PG_STAT 2
constexpr uint8_t kRegFault = 0x0C;      // WATCHDOG_FAULT bit 7
constexpr uint8_t kRegBatV = 0x0E;       // BATV[6:0], 20 mV/LSB, offset 2.304 V
constexpr uint8_t kRegSysV = 0x0F;       // SYSV[6:0], 20 mV/LSB, offset 2.304 V
constexpr uint8_t kRegVbusV = 0x11;      // VBUSV[6:0], 100 mV/LSB, offset 2.6 V
constexpr uint8_t kRegIchg = 0x12;       // ICHGR[6:0], 50 mA/LSB, charge current only
constexpr uint8_t kRegPart = 0x14;       // PN[5:3] = 000 for bq25896; bit 7 is REG_RST, never written

uint8_t address() { return BoardConfig::ACTIVE.batteryGauge.chargerAddr; }

// Set the bits in `mask` to `value` (masked), leaving every other bit as read.
// Returns false if either half of the transaction failed, so a caller never
// reports a write that did not land.
bool updateBits(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = 0;
  if (!powerbus::read8(address(), reg, current)) return false;
  const uint8_t next = static_cast<uint8_t>((current & ~mask) | (value & mask));
  if (!powerbus::write8(address(), reg, next)) return false;
  // Read back rather than trust the ACK: the watchdog bits and CHG_CONFIG are
  // exactly the bits the chip is allowed to change under us, and a bench number
  // taken against a state nobody confirmed is the failure this task exists to
  // stop repeating.
  uint8_t verify = 0;
  if (!powerbus::read8(address(), reg, verify)) return false;
  return (verify & mask) == (value & mask);
}

// The watchdog's own encoding, both ways. 00 disable, 01 40 s (reset default),
// 10 80 s, 11 160 s (Table 13, p.39).
uint16_t watchdogSeconds(uint8_t reg07) {
  switch ((reg07 >> 4) & 0x03) {
    case 0: return 0;
    case 1: return 40;
    case 2: return 80;
    default: return 160;
  }
}

bool watchdogBitsFromSeconds(long seconds, uint8_t& bits) {
  switch (seconds) {
    case 0: bits = 0; return true;
    case 40: bits = 1; return true;
    case 80: bits = 2; return true;
    case 160: bits = 3; return true;
    default: return false;
  }
}

// True when an input source is attached. VBUS_STAT (REG0B bits 7:5) is 000 only
// when there is no input. This gates the BATFET experiment: BATFET_DIS with no
// VBUS *is* ship mode -- SYS drops, the ESP32 stops, and only the S4 button or
// an adapter brings the board back (SLUSC76C p.26). A bench command that can
// park the board in that state is a command that will, eventually.
bool vbusPresent(uint8_t reg0b) { return ((reg0b >> 5) & 0x07) != 0; }

}  // namespace bq25896
#endif  // ENABLE_CHARGE_CMD
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
  sampleInput();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    sampleInput();
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
  // freeink-sdk was upstream-only when this was written, so the correction had
  // to live here. It is forked as of 2026-09-03 (docs/freeink-sdk-fork.md), so
  // this belongs upstream now and the workaround should go. T-247.
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
      InputManager::setButtonHook(boardButtonHook);
      LOG_INF("BTN", "User button: tap = Confirm, hold %lu ms = frontlight rung; BOOT: tap = Back, hold %u ms = sleep",
              USER_BUTTON_HOLD_MS, static_cast<unsigned>(powerHoldDurationMs()));
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
  // Before Storage.begin() on purpose, and this is the window the SDK fix does
  // not reach: prepareEpdPower() runs at display init, which is later.
  t5s3DeselectLoraRadio();
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
    // No-op on a single-channel board (FrontlightManager.h) -- calling it
    // unconditionally still requires present() so it never runs on a board with
    // no light at all.
    if (frontlight.hasColorTemperature()) frontlight.setColorTemperature(SETTINGS.frontlightColorTemperature);
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
      if (!gpio.verifyPowerButtonWakeup(powerHoldDurationMs(),
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
      sampleInput();
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
    sampleInput();
    delay(10);
    sampleInput();
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
  sampleInput();
  // One step of any injected button press, in the same frame the real buttons
  // were read (DebugInput.h). Before the CMD: parser below, so a press queued
  // this iteration starts on the next one and never lands mid-frame with the
  // activity having already read its edges.
  DebugInput::pump(gpio.updateSequence(), millis());
  // The second way to the light: a hold on the capacitive home key below the
  // panel, on any board that has one. Handled here rather than in an activity so
  // every screen has it, and before activityManager.loop() so the screen on top
  // cannot consume it first. The SDK suppresses the key's tap once the hold
  // fires (InputManager::serviceTouch), so a hold never also selects.
  // Through MappedInputManager, not gpio: the SDK's hold fires from a latched
  // down-state that survives a missed release edge, so the raw event can arrive
  // from a press already spent as a tap. That is how one double tap on Home
  // locked the panel, opened the map, and lit the frontlight when the map
  // finished rendering (measured 2026-09-05).
  if (mappedInputManager.wasHomeKeyLongPress()) {
    toggleFrontlight("Home key hold");
  }
  // The third gesture on the same key: a double tap locks or unlocks the panel.
  // Resolved in MappedInputManager, which holds the first tap for the double-tap
  // window and decides between Confirm and this -- a tap that had already
  // selected could not be taken back once the second tap arrived.
  //
  // No board condition here: wasHomeKeyDoubleTap() is false on a board that has
  // no home key or no digitizer, because pumpHomeKey() never resolves one there.
  if (mappedInputManager.wasHomeKeyDoubleTap()) {
    toggleTouchLock();
  }
  // The Settings row writes the level straight into SETTINGS, so the light has
  // to be told. Only while it is on: changing the level must not turn it on.
  static uint8_t appliedFrontlightBrightness = SETTINGS.frontlightBrightness;
  if (SETTINGS.frontlightBrightness != appliedFrontlightBrightness) {
    appliedFrontlightBrightness = SETTINGS.frontlightBrightness;
    if (frontlight.present() && frontlight.brightness() > 0) {
      frontlight.setBrightness(appliedFrontlightBrightness);
    }
  }
  // Same reasoning as frontlightBrightness above: the color-temperature picker
  // (SettingsActivity::openFrontlightColorTemperaturePicker()) writes straight
  // into SETTINGS on Confirm, so the light has to be told here too. Applied
  // regardless of on/off state -- unlike brightness, changing the warm/cool mix
  // while the light is off is harmless and should still take effect once it's
  // switched back on.
  static uint8_t appliedFrontlightColorTemperature = SETTINGS.frontlightColorTemperature;
  if (SETTINGS.frontlightColorTemperature != appliedFrontlightColorTemperature) {
    appliedFrontlightColorTemperature = SETTINGS.frontlightColorTemperature;
    if (frontlight.hasColorTemperature()) {
      frontlight.setColorTemperature(appliedFrontlightColorTemperature);
    }
  }
  if (frontlightStateChanged && !frontlightHoldActive) {
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
#ifdef ENABLE_BUTTON_CMD
      } else if (cmd == "BUTTON" || cmd.startsWith("BUTTON ")) {
        // Press a hardware button from the host. Every screen that is not the
        // map or the sync screen is reachable only by a thumb, so without this
        // a laptop can put two activities on the panel and read the panel back
        // but cannot walk from one screen to the next -- see DebugInput.h for
        // why that keeps costing us reviews of screens nobody can reach.
        //
        //   CMD:BUTTON down          ->  BUTTON_OK:down:0
        //   CMD:BUTTON back 1500     ->  BUTTON_OK:back:1500     (long press)
        //   CMD:BUTTON middle        ->  BUTTON_ERR:unknown:back,confirm,...
        //
        // Devel builds only, and deliberately: a shipped injector is a thumb
        // for whoever finds a lost device. Same gate shape as CMD:SETTING
        // above, its own flag rather than a logging one.
        String rest = cmd.length() > 6 ? cmd.substring(7) : String("");
        rest.trim();
        const int space = rest.indexOf(' ');
        String name = space < 0 ? rest : rest.substring(0, space);
        String holdArg = space < 0 ? String("") : rest.substring(space + 1);
        name.trim();
        holdArg.trim();
        const uint8_t button = DebugInput::buttonFromName(name.c_str());
        // A hold long enough to matter is a second or two. The cap is there so
        // a typo (`1500000`) cannot park the queue for half an hour with the
        // host waiting on presses that never run.
        constexpr long kMaxHoldMs = 10000;
        const long holdMs = holdArg.length() == 0 ? 0 : holdArg.toInt();
        if (button == DebugInput::kNoButton) {
          logSerial.printf("BUTTON_ERR:unknown:back,confirm,left,right,up,down,power\n");
        } else if (holdMs < 0 || holdMs > kMaxHoldMs) {
          logSerial.printf("BUTTON_ERR:hold:0-%ld\n", kMaxHoldMs);
        } else if (!DebugInput::queue(button, static_cast<uint32_t>(holdMs))) {
          logSerial.printf("BUTTON_ERR:busy\n");
        } else {
          logSerial.printf("BUTTON_OK:%s:%ld\n", DebugInput::kButtonNames[button], holdMs);
        }
#endif  // ENABLE_BUTTON_CMD
#ifdef ENABLE_TOUCHLOG_CMD
      } else if (cmd == "TOUCHLOG" || cmd.startsWith("TOUCHLOG ")) {
        // Raw GT911 status register, timestamped, with the loop deliberately
        // blocked for the whole capture. The five open questions in
        // firmware/explorink docs/input-gestures.md are all questions about when
        // a byte changes, and nothing else in this firmware can see that --
        // src/DebugTouchLog.h has the reasoning and the two modes.
        //
        //   CMD:TOUCHLOG                      ->  3000 ms at 5 ms, clearing
        //   CMD:TOUCHLOG 6000 5000 noclear    ->  6 s at 5 ms, never clearing
        //   CMD:TOUCHLOG 8000 5000 delay2000  ->  hold the frame 2 s, ack once, watch
        long durationMs = 3000;
        long intervalUs = 5000;
        bool clearAfterRead = true;
        long clearDelayMs = 0;
        String rest = cmd.length() > 8 ? cmd.substring(9) : String("");
        rest.trim();
        if (rest.length() > 0) {
          const int firstGap = rest.indexOf(' ');
          durationMs = (firstGap < 0 ? rest : rest.substring(0, firstGap)).toInt();
          if (firstGap >= 0) {
            String tail = rest.substring(firstGap + 1);
            tail.trim();
            const int secondGap = tail.indexOf(' ');
            const String intervalToken = secondGap < 0 ? tail : tail.substring(0, secondGap);
            // A mode token may sit in either slot, so the interval is optional.
            // `delay<N>` is the third mode: hold the frame N ms, acknowledge it
            // once, then watch (src/DebugTouchLog.h, open question 5).
            auto applyMode = [&](const String& mode) {
              if (mode.startsWith("delay")) {
                clearDelayMs = mode.substring(5).toInt();
                clearAfterRead = false;
              } else {
                clearAfterRead = mode != "noclear";
              }
            };
            if (intervalToken == "clear" || intervalToken == "noclear" || intervalToken.startsWith("delay")) {
              applyMode(intervalToken);
            } else {
              intervalUs = intervalToken.toInt();
              if (secondGap >= 0) {
                String mode = tail.substring(secondGap + 1);
                mode.trim();
                applyMode(mode);
              }
            }
          }
        }
        if (durationMs <= 0 || intervalUs <= 0) {
          logSerial.printf("TOUCHLOG_ERR:args:<ms> <us> clear|noclear\n");
        } else {
          DebugTouchLog::capture(logSerial, static_cast<uint32_t>(durationMs), static_cast<uint32_t>(intervalUs),
                                 clearAfterRead, static_cast<uint32_t>(clearDelayMs < 0 ? 0 : clearDelayMs));
        }
      } else if (cmd == "LOOPGAP") {
        // How long the input sampler goes unread. Read it, do the thing being
        // measured, read it again -- the report resets on read, so the second
        // answer covers only the interval between them.
        DebugTouchLog::reportGaps(logSerial);
#endif  // ENABLE_TOUCHLOG_CMD
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
        //   CMD:GNSS RAW BYTES ON  ->  GNSS_OK:rawbytes=1  (every byte, hex-dumped
        //                          in GNSS_RAWBYTES: lines -- sees a binary CASIC
        //                          reply that RAW ON cannot, because it has no '$'
        //                          and no NMEA checksum)
        //   CMD:GNSS RAW BYTES OFF ->  GNSS_OK:rawbytes=0
        //   CMD:GNSS SEND <hex>    ->  GNSS_OK:sent=<n> bytes  (T-210: write a
        //                          pre-computed frame verbatim, hex with or
        //                          without spaces; RAW BYTES ON first to see
        //                          the reply)
        //   CMD:GNSS EPH       ->  dead on the L76K, LT= is always 0 (use NAV-STATUS)
        //   CMD:GNSS PROBE     ->  GNSS_PROBE:...  (run first, on a cold boot)
        //   CMD:GNSS RELEASE   ->  GNSS_RELEASE:... (writes the rail pin, step 2a)
        //   CMD:GNSS LOG       ->  GNSS_LOG:...    (sizes of the fix log, never its rows)
        //   CMD:GNSS SKY 12    ->  GNSS_OK:sky=12 heard=9 best=45  (a synthetic
        //                          sky for the wait screen -- GnssFakeSky.h)
        //   CMD:GNSS SKY OFF   ->  GNSS_OK:sky=off
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

        if (argument.startsWith("SKY")) {
          // A synthetic sky for the wait screen, so the plot can be judged
          // without waiting for weather (GnssFakeSky.h). Feeds the sky and the
          // readout's counts only -- never a position, so the map is unaffected
          // and a screenshot taken with this on says nothing about it.
          String amount = argument.substring(3);
          amount.trim();
          if (amount.length() == 0 || amount == "OFF" || amount == "0") {
            FAKE_SKY.disable();
            logSerial.printf("GNSS_OK:sky=off\n");
          } else {
            const long requested = amount.toInt();
            if (requested <= 0) {
              logSerial.printf("GNSS_ERR:sky wants a count or OFF\n");
            } else {
              FAKE_SKY.enable(static_cast<uint8_t>(requested > 255 ? 255 : requested));
              logSerial.printf("GNSS_OK:sky=%u heard=%u best=%u\n", static_cast<unsigned>(FAKE_SKY.count()),
                               static_cast<unsigned>(FAKE_SKY.satsWithSignal()),
                               static_cast<unsigned>(FAKE_SKY.bestSnr()));
            }
          }
        } else if (argument == "ON") {
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
                gnssResetReasonName(), cfgBase, haveReleased ? cfgReleased : 0xEE, haveRestored ? cfgRestored : 0xEE,
                wroteLevel ? 1 : 0, released ? 1 : 0, restored ? 1 : 0, baseBytes, baseSent, offBytes, offSent,
                backBytes, backSent);
            gnss.end();  // powerEnable is null, so this touches no rail
          }
        } else if (argument == "RAW ON" || argument == "RAW") {
          gnss.setRawSink(gnssRawSink);
          logSerial.printf("GNSS_OK:raw=1\n");
        } else if (argument == "RAW OFF") {
          gnss.setRawSink(nullptr);
          logSerial.printf("GNSS_OK:raw=0\n");
        } else if (argument == "RAW BYTES ON") {
          gnss.setRawByteSink(gnssRawByteSink);
          logSerial.printf("GNSS_OK:rawbytes=1\n");
        } else if (argument == "RAW BYTES OFF") {
          gnss.setRawByteSink(nullptr);
          gnssFlushRawBytes();  // the tail of the last reply may still be buffered
          logSerial.printf("GNSS_OK:rawbytes=0\n");
        } else if (argument.startsWith("SEND")) {
          // T-210: write a pre-computed frame verbatim -- T-209's bench sends
          // ready-made CASIC bytes, so this needs no framing and no checksum,
          // only a hex decode. Hex with or without spaces, e.g.
          // "SEND BACE0400060206FF01000003020602" and
          // "SEND BA CE 04 00 06 02 06 FF 01 00 00 03 02 06 02" both work;
          // RAW BYTES ON first, or the reply goes nowhere (same rule as EPH's
          // PCAS06 query above).
          String hexArg = argument.substring(4);
          hexArg.trim();
          String hexClean;
          hexClean.reserve(hexArg.length());
          for (unsigned int i = 0; i < hexArg.length(); ++i) {
            const char ch = hexArg[i];
            if (ch != ' ') hexClean += ch;
          }
          static constexpr size_t kMaxSendBytes = 128;
          const auto hexNibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
          };
          if (hexClean.length() == 0 || (hexClean.length() % 2) != 0) {
            logSerial.printf("GNSS_ERR:SEND wants an even number of hex digits\n");
          } else if (hexClean.length() / 2 > kMaxSendBytes) {
            logSerial.printf("GNSS_ERR:SEND payload too long, max %u bytes\n", static_cast<unsigned>(kMaxSendBytes));
          } else {
            const size_t byteCount = hexClean.length() / 2;
            uint8_t bytes[kMaxSendBytes];
            bool badHex = false;
            for (size_t i = 0; i < byteCount; ++i) {
              const int hi = hexNibble(hexClean[i * 2]);
              const int lo = hexNibble(hexClean[i * 2 + 1]);
              if (hi < 0 || lo < 0) {
                badHex = true;
                break;
              }
              bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
            }
            if (badHex) {
              logSerial.printf("GNSS_ERR:SEND has non-hex characters\n");
            } else if (gnss.sendRaw(bytes, byteCount)) {
              logSerial.printf("GNSS_OK:sent=%u bytes, RAW BYTES ON to see the reply\n",
                               static_cast<unsigned>(byteCount));
            } else {
              logSerial.printf("GNSS_ERR:send failed, receiver not running\n");
            }
          }
        } else if (argument == "EPH") {
          // Ask a CASIC receiver how many valid ephemerides it is holding. The
          // answer comes back as an ordinary sentence carrying `LT=<n>`, so it
          // reaches the raw sink and nothing else -- CMD:GNSS RAW ON first.
          //
          // **It does not work on the L76K, measured 2026-09-11 (T-209): `LT=`
          // is 0 in every reply, including seconds when NAV-STATUS reported
          // three effective ephemerides.** So this command answers nothing on
          // this board and is kept only because a future receiver may fill the
          // field in.
          //
          // **The working instrument is NAV-STATUS (0x01 0x00)**, enabled with
          // CFG-MSG and read per satellite: it is what answered the question
          // this comment used to claim for LT=, namely whether a rail cycle
          // costs the receiver its ephemeris. It does not below about 3 min and
          // does above about 5. docs/gnss.md, "What the bench actually
          // answered".
          //
          // Reading it rather than injecting it is the whole point. Ephemeris
          // *injection* on this module is a known unsolved problem -- CASIC's
          // own spec lists MSG-GPSEPH as an output, and OpenTrailPaper measured
          // 4 ACKs out of 33 attempts with the count staying at zero
          // (investigations/agnss.md). The module decodes its own ephemeris
          // perfectly given signal, so retention is the lever, not injection.
          if (gnss.sendNmeaSentence("PCAS06,L")) {
            logSerial.printf("GNSS_OK:eph-query sent -- LT= reads 0 on the L76K whatever it holds, "
                             "measured 2026-09-11; enable NAV-STATUS instead\n");
          } else {
            logSerial.printf("GNSS_ERR:eph query not sent, receiver not running\n");
          }
        } else if (argument.length() > 0) {
          logSerial.printf(
              "GNSS_ERR:expected ON, OFF, PROBE, RELEASE, EPH, SEND <hex>, RAW ON, RAW OFF, RAW BYTES ON or RAW "
              "BYTES OFF\n");
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
      } else if (cmd == "BATT" || cmd.startsWith("BATT ")) {
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
        // there means a change to freeink-sdk, which is a mirror of upstream
        // carrying almost nothing of ours and whose pointer moves in a pass of
        // its own (docs/freeink-sdk-fork.md) -- a bench command should not wait
        // on that, so this reads the same registers from our side. (Until
        // 2026-09-03 this comment said the SDK was upstream's repo outright,
        // which stopped being true when it was forked; the approach did not
        // change, only the reason given for it.)
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
        //
        // **CMD:BATT DM <hex>** reads one 32-byte data-memory block instead
        // (T-251's third question). Two of the gauge's defaults decide whether
        // a small current means anything at all: `Deadband` (0x91DE, default
        // 5 mA) makes Current() report a hard 0 below it, and Operation Config A
        // (0x9206, default 0x0484, bit 2) lets the gauge drop to a 20 s sample
        // period below the Sleep Current threshold. Whether LilyGo left either
        // at TI's default was unreadable until this existed, so every "the board
        // draws almost nothing" reading was unfalsifiable.
        //
        //   CMD:BATT DM 0x91DE  ->  BATT_DM:addr=0x91DE len=36 sum=ok u8=5 u16=0x2905 data=05 29 ...
        //
        // `sum=ok` is the point of the reply. A sealed or unresponsive gauge
        // answers a data-memory read with zeros that look exactly like a real
        // "Deadband is 0", so the block is only believable when the address
        // echoes back and MACDataSum() matches what the data adds up to. Read
        // path per SLUUBD4A 2.29-2.31 and 3.1; no CFGUPDATE, because nothing
        // here writes.
        const auto& g = BoardConfig::ACTIVE.batteryGauge;
        String battArg = cmd.length() > 4 ? cmd.substring(5) : String("");
        battArg.trim();
        battArg.toUpperCase();  // the subcommand; a hex address parses either case
        if (g.gaugeAddr == 0) {
          logSerial.printf("BATT_ERR:no gauge on this board\n");
        } else if (battArg.length() > 0 && !battArg.startsWith("DM")) {
          logSerial.printf("BATT_ERR:unknown:DM\n");
        } else if (battArg.startsWith("DM")) {
          String addrArg = battArg.substring(2);
          addrArg.trim();
          const long addr = strtol(addrArg.c_str(), nullptr, 0);
          if (addrArg.length() == 0 || addr <= 0 || addr > 0xFFFF) {
            logSerial.printf("BATT_ERR:dm addr\n");
          } else {
            powerbus::begin();
            TwoWire& w = powerbus::wire();
            // ManufacturerAccessControl is 0x3E/0x3F and takes the data-memory
            // address little-endian: for 0x929F the TRM's own worked example
            // writes 0x9F to 0x3E and 0x92 to 0x3F (SLUUBD4A, "Accessing the
            // Data Memory", step 5-6). One transaction, because the pair is
            // what arms the block transfer.
            w.beginTransmission(g.gaugeAddr);
            w.write(0x3E);
            w.write(static_cast<uint8_t>(addr & 0xFF));
            w.write(static_cast<uint8_t>((addr >> 8) & 0xFF));
            const bool armed = w.endTransmission(true) == 0;
            delay(15);  // the block transfer is not instant; the TRM polls, this waits past it

            // **One incremental read from 0x3E, not 36 single-register reads.**
            // The first version here read each address on its own and got the
            // same 20 bytes back for three different data-memory addresses,
            // echo and checksum both wrong: a block transfer that is re-armed
            // by every fresh addressed read never delivers the block. The TRM
            // says to read it in one go -- "read the response using an
            // incremental read. To the device address 0xAB, starting at command
            // 0x3E" (SLUUBD4A 2.2). 0x3E..0x61 is contiguous: two address
            // bytes, 32 data bytes, then MACDataSum() and MACDataLen().
            uint8_t echo[2] = {0, 0};
            uint8_t block[32] = {0};
            uint8_t sum = 0, len = 0;
            uint8_t mac[36] = {0};
            bool ok = armed;
            if (ok) {
              w.beginTransmission(g.gaugeAddr);
              w.write(0x3E);
              ok = w.endTransmission(false) == 0;
            }
            if (ok) {
              ok = w.requestFrom(g.gaugeAddr, static_cast<uint8_t>(sizeof(mac)), static_cast<uint8_t>(true)) ==
                   static_cast<int>(sizeof(mac));
            }
            if (ok) {
              for (size_t i = 0; i < sizeof(mac); ++i) mac[i] = w.read();
              echo[0] = mac[0];
              echo[1] = mac[1];
              memcpy(block, mac + 2, sizeof(block));
              sum = mac[34];
              len = mac[35];
            }
            // The gauge's security state, in the same reply. A SEALED gauge
            // answers a data-memory read with something rather than an error,
            // so without this a refused read and a real value are the same
            // bytes. SEC[1:0] is OperationStatus() bits 2:1 (SLUUBD4A 2.27):
            // 11 sealed, 10 unsealed, 01 full access.
            uint16_t opStatus = 0;
            const bool opOk = powerbus::read16(g.gaugeAddr, 0x3A, opStatus);
            if (!ok) {
              logSerial.printf("BATT_ERR:dm i2c\n");
            } else {
              // MACDataLen counts the two address bytes, the data, and the
              // length and checksum bytes themselves -- the TRM's 32-byte
              // example writes 0x24 (36). So the data is len - 4 bytes, and the
              // checksum is 255 minus the 8-bit sum of address plus that data.
              const int dataLen = static_cast<int>(len) - 4;
              const bool lenSane = dataLen > 0 && dataLen <= static_cast<int>(sizeof(block));
              uint8_t calc = static_cast<uint8_t>(echo[0] + echo[1]);
              for (int i = 0; lenSane && i < dataLen; ++i) calc = static_cast<uint8_t>(calc + block[i]);
              calc = static_cast<uint8_t>(255 - calc);
              const bool echoOk = echo[0] == static_cast<uint8_t>(addr & 0xFF) &&
                                  echo[1] == static_cast<uint8_t>((addr >> 8) & 0xFF);
              const bool sumOk = lenSane && calc == sum && echoOk;

              char hex[sizeof(block) * 3 + 1];
              size_t pos = 0;
              const int printLen = lenSane ? dataLen : static_cast<int>(sizeof(block));
              for (int i = 0; i < printLen; ++i) {
                pos += static_cast<size_t>(
                    snprintf(hex + pos, sizeof(hex) - pos, "%02X ", static_cast<unsigned>(block[i])));
              }
              // Both readings of the first bytes, because the data type is the
              // parameter's, not the block's: Deadband is U1 and Operation
              // Config A is H2, and a reply that picked one would be wrong for
              // the other. Big-endian for the 16-bit form: the TRM's own
              // Design Capacity example reads the MSB at 0x40.
              char secText[8] = "?";
              if (opOk) snprintf(secText, sizeof(secText), "%u", static_cast<unsigned>((opStatus >> 1) & 0x03));
              logSerial.printf("BATT_DM:addr=0x%04lX len=%u sum=%s echo=%s sec=%s opstat=0x%04X u8=%u u16=0x%04X data=%s\n",
                               addr, static_cast<unsigned>(len), sumOk ? "ok" : "bad", echoOk ? "ok" : "bad", secText,
                               static_cast<unsigned>(opStatus), static_cast<unsigned>(block[0]),
                               static_cast<unsigned>((block[0] << 8) | block[1]), hex);
            }
          }
        } else {
          powerbus::begin();

          uint16_t mv = 0, pct = 0, rawCurrent = 0;
          uint8_t chargerStatus = 0;
          const bool mvOk = powerbus::read16(g.gaugeAddr, 0x08, mv);
          const bool pctOk = powerbus::read16(g.gaugeAddr, 0x2C, pct);
          const bool currOk = powerbus::read16(g.gaugeAddr, 0x0C, rawCurrent);
          const bool chgOk = g.chargerAddr != 0 && powerbus::read8(g.chargerAddr, 0x0B, chargerStatus);

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
#ifdef ENABLE_CHARGE_CMD
      } else if (cmd == "CHARGE" || cmd.startsWith("CHARGE ")) {
        // Switch the BQ25896's charging off from the host, and read back what
        // the datasheet leaves open. T-251.
        //
        // **Why a bench needs this.** A USB inline meter on VBUS reads the board
        // *plus* whatever the charger is doing, so no VBUS number is board draw
        // while a cell charges behind it. SLUSC76C p.18 says charging off opens
        // the BATFET on its own ("If battery charging is disabled, BATFET turns
        // off"), which takes the cell out of the path with no device opened and
        // no bare board -- the only measurement route this project's hardware
        // policy allows. Every per-refresh and per-state number T-275 and T-594
        // owe starts here.
        //
        // **The 40 s trap, and why the order below is not cosmetic.** The chip
        // is in default mode until the first write; that write puts it in host
        // mode and starts the I2C watchdog. On expiry it restores defaults --
        // and CHG_CONFIG is not on the exception list, so charging switches
        // itself back on 40 seconds into a measurement while the run says it is
        // off (SLUSC76C p.31). So OFF disables the watchdog *first* and only
        // then clears CHG_CONFIG. ON puts charging back and leaves the watchdog
        // alone -- see the ON branch for why re-arming it is not a restore.
        //
        //   CMD:CHARGE            ->  CHARGE:chg=1 wd_s=40 batfet_dis=0 vbus_stat=1 chrg_stat=2 pg=1 wd_fault=0 curr_ma=214
        //   CMD:CHARGE OFF        ->  CHARGE_OK:off wd_s=0 ... (then the status line)
        //   CMD:CHARGE ON         ->  CHARGE_OK:on wd_s=40 ...
        //   CMD:CHARGE WD 0|40|80|160
        //   CMD:CHARGE BATFET <dwell_ms>
        //   CMD:CHARGE ADC        ->  CHARGE_ADC:vbat_mv=3912 sys_mv=4032 vbus_mv=5000 ichg_ma=0
        //   CMD:CHARGE REG        ->  CHARGE_REG:00=3a 01=... 14=x
        //
        // **There is no register-write subcommand and there will not be one.**
        // See the bq25896 namespace above: four named bits are reachable, the
        // "never write these" ones are not reachable at all.
        //
        // Devel-only, t5s3pro only, and this one is not merely a UI-less
        // command: it can leave a rider's device not charging. Both command
        // channels are unauthenticated (parent docs/TODO.md, T-222), so in a
        // release build this would hand anyone in BLE range a way to flatten the
        // device silently.
        String rest = cmd.length() > 6 ? cmd.substring(7) : String("");
        rest.trim();
        rest.toUpperCase();

        const uint8_t chargerAddr = bq25896::address();
        if (chargerAddr == 0) {
          logSerial.printf("CHARGE_ERR:no charger on this board\n");
        } else {
          powerbus::begin();

          // One status line, read fresh every time it is printed. Everything in
          // it comes off the chip, never off what a command just wrote.
          auto printStatus = [&]() {
            uint8_t reg03 = 0, reg07 = 0, reg09 = 0, reg0b = 0, reg0c = 0;
            const bool ok03 = powerbus::read8(chargerAddr, bq25896::kRegChargeCtrl, reg03);
            const bool ok07 = powerbus::read8(chargerAddr, bq25896::kRegWatchdog, reg07);
            const bool ok09 = powerbus::read8(chargerAddr, bq25896::kRegBatfet, reg09);
            const bool ok0b = powerbus::read8(chargerAddr, bq25896::kRegStatus, reg0b);
            const bool ok0c = powerbus::read8(chargerAddr, bq25896::kRegFault, reg0c);
            if (!ok03 || !ok07 || !ok09 || !ok0b || !ok0c) {
              logSerial.printf("CHARGE_ERR:i2c\n");
              return;
            }
            // The gauge's own current, in the same line and at the same instant
            // as the charger state: with charging off it is the check that the
            // BATFET really opened (it should fall to ~0), and it is the number
            // a meter reading is compared against. Signed, positive into the
            // cell. Its own floor is a lie below 5 mA -- see CMD:BATT DM.
            uint16_t rawCurrent = 0;
            const bool currOk = BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0 &&
                                powerbus::read16(BoardConfig::ACTIVE.batteryGauge.gaugeAddr, 0x0C, rawCurrent);
            char currText[12] = "?";
            if (currOk) snprintf(currText, sizeof(currText), "%d", static_cast<int>(static_cast<int16_t>(rawCurrent)));
            logSerial.printf("CHARGE:chg=%u wd_s=%u batfet_dis=%u vbus_stat=%u chrg_stat=%u pg=%u wd_fault=%u curr_ma=%s\n",
                             static_cast<unsigned>((reg03 >> 4) & 0x01),
                             static_cast<unsigned>(bq25896::watchdogSeconds(reg07)),
                             static_cast<unsigned>((reg09 >> 5) & 0x01), static_cast<unsigned>((reg0b >> 5) & 0x07),
                             static_cast<unsigned>((reg0b >> 3) & 0x03), static_cast<unsigned>((reg0b >> 2) & 0x01),
                             static_cast<unsigned>((reg0c >> 7) & 0x01), currText);
          };

          if (rest.isEmpty()) {
            printStatus();
          } else if (rest == "OFF") {
            // Watchdog first. The other order works for 40 seconds and then
            // silently stops being true.
            if (!bq25896::updateBits(bq25896::kRegWatchdog, 0x30, 0x00)) {
              logSerial.printf("CHARGE_ERR:watchdog write\n");
            } else if (!bq25896::updateBits(bq25896::kRegChargeCtrl, 0x10, 0x00)) {
              logSerial.printf("CHARGE_ERR:chg_config write\n");
            } else {
              logSerial.printf("CHARGE_OK:off\n");
              printStatus();
            }
          } else if (rest == "ON") {
            // Charging back on, and **the watchdog is deliberately left where
            // it is.** The first version put it back to the chip's 40 s reset
            // default, on the theory that the board should leave the bench in
            // stock behaviour. On this board that is backwards: the watchdog was
            // already disabled when the first bench run read it (2026-09-12,
            // REG07 = 0x8D), because LilyGo's factory firmware disables it and
            // the charger keeps its registers across an ESP32 reflash. So the
            // chip's default is not this board's as-found state, and "restoring"
            // it would arm a timer whose expiry resets every non-excepted
            // register -- SYS_MIN among them, which the vendor set to 3.3 V and
            // the default would put back to 3.5 V. CMD:CHARGE WD 40 is there for
            // whoever actually wants the timer.
            if (!bq25896::updateBits(bq25896::kRegChargeCtrl, 0x10, 0x10)) {
              logSerial.printf("CHARGE_ERR:chg_config write\n");
            } else {
              logSerial.printf("CHARGE_OK:on\n");
              printStatus();
            }
          } else if (rest.startsWith("WD")) {
            String arg = rest.substring(2);
            arg.trim();
            uint8_t bits = 0;
            if (!bq25896::watchdogBitsFromSeconds(arg.toInt(), bits)) {
              logSerial.printf("CHARGE_ERR:wd:0,40,80,160\n");
            } else if (!bq25896::updateBits(bq25896::kRegWatchdog, 0x30, static_cast<uint8_t>(bits << 4))) {
              logSerial.printf("CHARGE_ERR:watchdog write\n");
            } else {
              logSerial.printf("CHARGE_OK:wd\n");
              printStatus();
            }
          } else if (rest.startsWith("BATFET")) {
            // The open question this task exists to answer: **is BATFET_DIS
            // honoured at all while VBUS is present?** The datasheet never says.
            // It specs VSYS for the BATFET-disabled case (p.8), which only means
            // anything with the converter running from VBUS -- but "plug in
            // adapter" is listed as an *exit* event from ship mode, and whether
            // an already-present adapter counts is unstated. The two TI E2E
            // threads on exactly this are behind a bot wall.
            //
            // **A pulse, never a latch.** The bit is set, read back, held for a
            // dwell, then cleared in the same command. Two reasons it cannot be
            // left set: BATFET_DIS survives a watchdog expiry (it is on p.31's
            // exception list, unlike CHG_CONFIG), and if the USB cable comes out
            // while it is set the board is in ship mode -- SYS at zero, I2C
            // dead, and only the S4 button or an adapter returns it. A bench
            // command that can park the board there is a command that will.
            //
            //   CMD:CHARGE BATFET 2000
            //     -> CHARGE_BATFET:set=1 alive=1 curr_before=214 curr_during=0 curr_after=213 cleared=1 dwell_ms=2000
            //
            // `set` is the bit read back after the write: 0 means the chip
            // refused it with VBUS present, which is itself the answer. `alive`
            // is trivially 1 in any reply that arrives -- a board that stopped
            // cannot print -- and it is in the line so a run's log carries the
            // evidence rather than its absence.
            String arg = rest.substring(6);
            arg.trim();
            const long dwellMs = arg.length() == 0 ? 2000 : arg.toInt();
            uint8_t reg0b = 0;
            if (dwellMs < 0 || dwellMs > 10000) {
              logSerial.printf("CHARGE_ERR:dwell:0-10000\n");
            } else if (!powerbus::read8(chargerAddr, bq25896::kRegStatus, reg0b)) {
              logSerial.printf("CHARGE_ERR:i2c\n");
            } else if (!bq25896::vbusPresent(reg0b)) {
              // Refused, not warned. With no input source this write is ship
              // mode and the board does not come back without a thumb.
              logSerial.printf("CHARGE_ERR:no vbus, batfet refused\n");
            } else {
              const uint8_t gaugeAddr = BoardConfig::ACTIVE.batteryGauge.gaugeAddr;
              auto gaugeCurrent = [&](char* out, size_t outSize) {
                uint16_t raw = 0;
                if (gaugeAddr != 0 && powerbus::read16(gaugeAddr, 0x0C, raw)) {
                  snprintf(out, outSize, "%d", static_cast<int>(static_cast<int16_t>(raw)));
                } else {
                  snprintf(out, outSize, "?");
                }
              };
              char before[12], during[12], after[12];
              gaugeCurrent(before, sizeof(before));

              const bool wrote = bq25896::updateBits(bq25896::kRegBatfet, 0x20, 0x20);
              uint8_t reg09 = 0;
              const bool readBack = powerbus::read8(chargerAddr, bq25896::kRegBatfet, reg09);
              const unsigned setBit = (readBack && ((reg09 >> 5) & 0x01)) ? 1u : 0u;

              // The gauge updates Current() once a second, so a dwell under
              // ~1.5 s reads a value from before the bit landed. Sampled at the
              // end of the dwell for that reason, not at the start.
              delay(static_cast<uint32_t>(dwellMs));
              gaugeCurrent(during, sizeof(during));

              const bool cleared = bq25896::updateBits(bq25896::kRegBatfet, 0x20, 0x00);
              delay(50);
              gaugeCurrent(after, sizeof(after));
              if (!cleared) {
                // Loud, because this is the one failure that leaves the board in
                // a state a cable pull turns into ship mode.
                LOG_ERR("CHARGE", "BATFET_DIS could not be cleared -- do not unplug USB");
              }
              logSerial.printf(
                  "CHARGE_BATFET:set=%u alive=1 curr_before=%s curr_during=%s curr_after=%s cleared=%u dwell_ms=%ld\n",
                  setBit, before, during, after, cleared ? 1u : 0u, dwellMs);
              if (!wrote && setBit == 0) {
                // updateBits() verifies its own readback, so a failed write and
                // a refused bit look the same from here. Say so rather than
                // letting a run record "refused" for an I2C error.
                logSerial.printf("CHARGE_NOTE:write not verified, i2c error and a refusal are indistinguishable\n");
              }
              printStatus();
            }
          } else if (rest == "ADC") {
            // The voltage side of the power balance, off the charger's own ADC:
            // SYSV to 20 mV and VBUSV to 100 mV mean V_sys never has to be
            // assumed when converting a VBUS reading to board draw.
            //
            // One-shot, not the 1 s continuous mode, and switched off again by
            // the chip itself: "When battery monitor is active, the REGN power
            // is enabled and can increase device quiescent current" (SLUSC76C
            // p.24). Leaving it running changes the thing being measured.
            //
            // REG12 (ICHGR) is charge current into the cell, 50 mA per step, and
            // it reads 0 in DISABLE CHARGE mode (Table 4, p.25) -- so it is *not*
            // a board-current register and nothing here should be read as one.
            // There is no VBUS-current and no SYS-current register on this chip.
            //
            // **This board is already converting when the command arrives.**
            // REG02 read 0x51 on the first bench run, 2026-09-12: CONV_RATE
            // (bit 6) is 1, so the chip is in 1 s continuous mode and
            // CONV_START is read-only ("This bit is read-only when CONV_RATE =
            // 1", Table 8). The first version here wrote CONV_START anyway,
            // verified the readback and reported CHARGE_ERR:adc start on a chip
            // that was working perfectly. Nobody set that bit from our firmware
            // -- the charger keeps its registers across an ESP32 reflash, and
            // LilyGo's factory build configures it (SYS_MIN and the watchdog are
            // off their reset values too). So: start a one-shot only when the
            // chip is not already running one.
            uint8_t reg02 = 0;
            bool started = powerbus::read8(chargerAddr, bq25896::kRegAdcCtrl, reg02);
            const bool continuous = started && (reg02 & 0x40) != 0;
            if (started && !continuous) started = bq25896::updateBits(bq25896::kRegAdcCtrl, 0x80, 0x80);
            if (!started) {
              logSerial.printf("CHARGE_ERR:adc start\n");
            } else {
              // CONV_START stays high for the conversion; tCONV is 8 ms min and
              // 1000 ms max (p.12), so the poll gets a little past the max. In
              // continuous mode there is nothing to wait for: a sample is at
              // most a second old already.
              const unsigned long deadline = millis() + 1200;
              bool done = continuous;
              while (!done && millis() < deadline) {
                delay(10);
                if (!powerbus::read8(chargerAddr, bq25896::kRegAdcCtrl, reg02)) break;
                if ((reg02 & 0x80) == 0) {
                  done = true;
                  break;
                }
              }
              uint8_t batv = 0, sysv = 0, vbusv = 0, ichg = 0;
              const bool ok = powerbus::read8(chargerAddr, bq25896::kRegBatV, batv) &&
                              powerbus::read8(chargerAddr, bq25896::kRegSysV, sysv) &&
                              powerbus::read8(chargerAddr, bq25896::kRegVbusV, vbusv) &&
                              powerbus::read8(chargerAddr, bq25896::kRegIchg, ichg);
              if (!ok) {
                logSerial.printf("CHARGE_ERR:adc read\n");
              } else {
                // Offsets and LSBs from tables 20-24, pp. 45-47. VBUSV reads 0
                // when no input is attached, which is a real 0 rather than a
                // failed read -- the status line's vbus_stat says which.
                logSerial.printf("CHARGE_ADC:vbat_mv=%u sys_mv=%u vbus_mv=%u ichg_ma=%u done=%u cont=%u\n",
                                 static_cast<unsigned>(2304 + (batv & 0x7F) * 20),
                                 static_cast<unsigned>(2304 + (sysv & 0x7F) * 20),
                                 static_cast<unsigned>((vbusv & 0x7F) == 0 ? 0 : 2600 + (vbusv & 0x7F) * 100),
                                 static_cast<unsigned>((ichg & 0x7F) * 50), done ? 1u : 0u, continuous ? 1u : 0u);
              }
            }
          } else if (rest == "REG") {
            // The whole map, read-only, one line. A bench run that reports a
            // number should be able to show the register state it was taken in,
            // and a hand-typed subcommand cannot be trusted to have landed.
            char line[3 * 21 + 1];
            size_t pos = 0;
            bool ok = true;
            for (uint8_t reg = 0x00; reg <= bq25896::kRegPart; ++reg) {
              uint8_t value = 0;
              if (!powerbus::read8(chargerAddr, reg, value)) {
                ok = false;
                break;
              }
              pos += static_cast<size_t>(snprintf(line + pos, sizeof(line) - pos, "%02X ", static_cast<unsigned>(value)));
            }
            if (!ok) {
              logSerial.printf("CHARGE_ERR:i2c\n");
            } else {
              logSerial.printf("CHARGE_REG:%s\n", line);
            }
          } else {
            logSerial.printf("CHARGE_ERR:unknown:OFF,ON,WD,BATFET,ADC,REG\n");
          }
        }
#endif  // ENABLE_CHARGE_CMD
#ifdef ENABLE_BLE_CMD
      } else if (cmd == "BLE" || cmd.startsWith("BLE ")) {
        // Bring the BLE peripheral up and down from the console, so its power
        // cost can be measured as a difference between two otherwise identical
        // states.
        //
        // **Why it exists.** Until this, BLE came up only as a side effect of
        // entering the map or the sync screen (MapActivity::onEnter() ->
        // BlePositionServer::begin()). So the only measurable pair was "home
        // screen" against "map with BLE", and the difference between those two
        // is tiles, a renderer and a panel refresh as much as it is a radio.
        // That is not a measurement of BLE. With CMD:CHARGE taking the charger
        // out of the reading (docs/charge-control.md), a radio that toggles on
        // its own is the last piece the bench needs.
        //
        //   CMD:BLE       ->  BLE:running=0
        //   CMD:BLE ON    ->  BLE_OK:on running=1
        //   CMD:BLE OFF   ->  BLE_OK:off running=0
        //
        // **Use it on the home screen, not on the map.** Nothing here asks who
        // owns the radio, because nothing can: `begin()` is idempotent and
        // `end()` is unconditional, so an OFF issued while the map is open
        // takes the map's own channel down and the map will not notice until it
        // is left and re-entered. The bench measures a screen that is not
        // driving the radio anyway -- that is the whole point of toggling it by
        // hand.
        //
        // Devel-only, same reason as CMD:CHARGE rather than the weaker one: ON
        // starts an unauthenticated command channel (T-222 in the parent repo's
        // docs/TODO.md) on a device whose screen gives no sign of it.
        String rest = cmd.length() > 3 ? cmd.substring(4) : String("");
        rest.trim();
        rest.toUpperCase();
        auto& ble = freeink::BlePositionServer::getInstance();
        if (rest.isEmpty()) {
          logSerial.printf("BLE:running=%u\n", ble.isRunning() ? 1u : 0u);
        } else if (rest == "ON") {
          // powerManager.setPowerSaving(false) already ran for every CMD: above,
          // and it is load-bearing here: NimBLEDevice::init() hangs solid if it
          // is entered while the CPU is still in power-saving mode after idle
          // (docs/power-management.md). Same reason CMD:GOTO_MAP does it.
          const bool ok = ble.begin();
          if (!ok) {
            logSerial.printf("BLE_ERR:begin\n");
          } else {
            logSerial.printf("BLE_OK:on running=%u\n", ble.isRunning() ? 1u : 0u);
          }
        } else if (rest == "OFF") {
          ble.end();
          logSerial.printf("BLE_OK:off running=%u\n", ble.isRunning() ? 1u : 0u);
        } else {
          logSerial.printf("BLE_ERR:unknown:ON,OFF\n");
        }
#endif  // ENABLE_BLE_CMD
#ifdef ENABLE_WIFI_CMD
      } else if (cmd == "WIFI" || cmd.startsWith("WIFI ")) {
        // Bring the WiFi radio up and down from the console, in the three
        // shapes the firmware actually uses it in, so each one can be priced as
        // a difference against the same idle screen.
        //
        // **Why it exists.** Every WiFi state on this device is behind a menu
        // that also repaints the panel, scans, or serves a page -- the sync
        // screen, the web server, OTA, the font download. None of them is one
        // thing changing. The power campaign needs the radio alone, the same
        // way CMD:BLE gives it the other radio alone (docs/power-bench.md).
        //
        //   CMD:WIFI            ->  WIFI:mode=0 conn=0 rssi=0 ip=0.0.0.0 clients=0
        //   CMD:WIFI OFF        ->  WIFI_OK:off
        //   CMD:WIFI STA        ->  WIFI_OK:sta            (radio up, associated to nothing)
        //   CMD:WIFI AP         ->  WIFI_OK:ap ssid=<x> ip=<y>
        //   CMD:WIFI SCAN       ->  WIFI_OK:scan n=<x> ms=<y>
        //   CMD:WIFI CONNECT [ssid]  ->  WIFI_OK:connect ssid=<x> rssi=<y> ms=<z>
        //
        // CONNECT with no argument uses the last network the device joined
        // through the menu; it reads the credential store and never writes it,
        // so a bench run cannot change which network the device prefers.
        //
        // **A WiFi state is never a clean CPU state.** HalPowerManager::
        // setPowerSaving() forces power saving *off* whenever WiFi.getMode() is
        // not WIFI_MODE_NULL (HalPowerManager.cpp:59-64), so every reading below
        // carries a 240 MHz CPU as well as a radio. Price the clock separately
        // with CMD:CPU and subtract it, or the radio gets billed for both.
        //
        // Devel-only, same gate shape as CMD:BLE: AP mode puts an open network
        // with the device's name on the air, and SCAN is a radio burst, both
        // with nothing on the screen to say so.
        String rest = cmd.length() > 4 ? cmd.substring(5) : String("");
        rest.trim();
        String verb = rest;
        String arg;
        const int sp = rest.indexOf(' ');
        if (sp > 0) {
          verb = rest.substring(0, sp);
          arg = rest.substring(sp + 1);
          arg.trim();
        }
        verb.toUpperCase();
        if (verb.isEmpty()) {
          const wifi_mode_t mode = WiFi.getMode();
          logSerial.printf("WIFI:mode=%d conn=%u rssi=%d ip=%s clients=%d\n", static_cast<int>(mode),
                           WiFi.status() == WL_CONNECTED ? 1u : 0u, static_cast<int>(WiFi.RSSI()),
                           (mode & WIFI_MODE_AP) ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str(),
                           (mode & WIFI_MODE_AP) ? WiFi.softAPgetStationNum() : -1);
        } else if (verb == "OFF") {
          WiFi.disconnect(true, false);
          WiFi.softAPdisconnect(true);
          WiFi.mode(WIFI_OFF);
          logSerial.printf("WIFI_OK:off\n");
        } else if (verb == "STA") {
          // Mode only, no begin(): the radio is initialised and listening and
          // has joined nothing. This is the floor of "WiFi is on".
          WiFi.mode(WIFI_STA);
          WiFi.disconnect(false, false);
          logSerial.printf("WIFI_OK:sta mode=%d\n", static_cast<int>(WiFi.getMode()));
        } else if (verb == "AP") {
          // Same call the web server activity makes, minus the web server, so
          // the pair (AP up, AP up + server) is also measurable later.
          WiFi.mode(WIFI_AP);
          char ssid[32];
          uint8_t mac[6] = {0};
          WiFi.macAddress(mac);
          snprintf(ssid, sizeof(ssid), "ExplorInk-bench-%02X%02X", mac[4], mac[5]);
          const bool ok = WiFi.softAP(ssid, nullptr, 1, false, 4);
          if (!ok) {
            logSerial.printf("WIFI_ERR:softap\n");
          } else {
            logSerial.printf("WIFI_OK:ap ssid=%s ip=%s\n", ssid, WiFi.softAPIP().toString().c_str());
          }
        } else if (verb == "SCAN") {
          // A burst rather than a state: it is the most expensive thing the
          // radio does and it is short, so it is measured as energy over a
          // repeat, not as a level.
          WiFi.mode(WIFI_STA);
          const unsigned long t0 = millis();
          const int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
          const unsigned long ms = millis() - t0;
          WiFi.scanDelete();
          logSerial.printf("WIFI_OK:scan n=%d ms=%lu\n", n, ms);
        } else if (verb == "CONNECT") {
          std::string ssid = arg.length() > 0 ? std::string(arg.c_str()) : WIFI_STORE.getLastConnectedSsid();
          if (ssid.empty()) {
            logSerial.printf("WIFI_ERR:no saved ssid\n");
          } else {
            const WifiCredential* cred = WIFI_STORE.findCredential(ssid);
            WiFi.mode(WIFI_STA);
            if (cred != nullptr && !cred->password.empty()) {
              WiFi.begin(ssid.c_str(), cred->password.c_str());
            } else {
              WiFi.begin(ssid.c_str());
            }
            const unsigned long t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
              delay(100);
            }
            if (WiFi.status() != WL_CONNECTED) {
              logSerial.printf("WIFI_ERR:connect ssid=%s status=%d ms=%lu\n", ssid.c_str(),
                               static_cast<int>(WiFi.status()), millis() - t0);
            } else {
              logSerial.printf("WIFI_OK:connect ssid=%s rssi=%d ip=%s ms=%lu\n", ssid.c_str(),
                               static_cast<int>(WiFi.RSSI()), WiFi.localIP().toString().c_str(), millis() - t0);
            }
          }
        } else {
          logSerial.printf("WIFI_ERR:unknown:OFF,STA,AP,SCAN,CONNECT\n");
        }
#endif  // ENABLE_WIFI_CMD
#ifdef ENABLE_CPU_CMD
      } else if (cmd == "CPU" || cmd.startsWith("CPU ")) {
        // Pin the CPU clock so a state can be measured at a known frequency,
        // and released again.
        //
        // **Why it exists.** The loop throttles to HalPowerManager::
        // LOW_POWER_FREQ three seconds after the last input (main.cpp, the
        // IDLE_POWER_SAVING_MS branch), so an untouched bench state is at
        // 80 MHz on this board and a state that keeps WiFi up is at the full
        // clock -- setPowerSaving() refuses to throttle while the radio is up.
        // Two states that differ by "a radio" therefore also differ by "a
        // clock", and without this command there is no way to tell how much of
        // the difference is which.
        //
        //   CMD:CPU              ->  CPU:mhz=80 hold=0 held=0
        //   CMD:CPU HOLD 240     ->  CPU_OK:hold mhz=240 held=1
        //   CMD:CPU AUTO         ->  CPU_OK:auto mhz=240
        //
        // **80, 160 and 240 only.** Below 80 the CPU leaves the PLL for the
        // crystal, which drags APB down with it and takes PSRAM and flash
        // timing with it on an S3 -- both of those are correctness bounds the
        // power manager already documents (HalPowerManager.h, LOW_POWER_FREQ
        // and BLE_SAFE_FREQ). This command must not be the one place that
        // walks past them.
        //
        // The hold is a HalPowerManager::Lock, and the manager allows exactly
        // one at a time. `held=0` in the reply means something else already
        // holds it and the clock will be thrown away by the next throttle --
        // a run taken against `held=0` is not a measurement of what it says.
        static HalPowerManager::Lock* cpuHold = nullptr;
        String rest = cmd.length() > 3 ? cmd.substring(4) : String("");
        rest.trim();
        String verb = rest;
        String arg;
        const int sp = rest.indexOf(' ');
        if (sp > 0) {
          verb = rest.substring(0, sp);
          arg = rest.substring(sp + 1);
          arg.trim();
        }
        verb.toUpperCase();
        if (verb.isEmpty()) {
          logSerial.printf("CPU:mhz=%u hold=%u held=%u\n", static_cast<unsigned>(getCpuFrequencyMhz()),
                           cpuHold != nullptr ? 1u : 0u, cpuHold != nullptr ? 1u : 0u);
        } else if (verb == "AUTO") {
          delete cpuHold;
          cpuHold = nullptr;
          logSerial.printf("CPU_OK:auto mhz=%u\n", static_cast<unsigned>(getCpuFrequencyMhz()));
        } else if (verb == "HOLD") {
          const long mhz = arg.toInt();
          if (mhz != 80 && mhz != 160 && mhz != 240) {
            logSerial.printf("CPU_ERR:mhz:80,160,240\n");
          } else {
            if (cpuHold == nullptr) {
              cpuHold = new HalPowerManager::Lock();
            }
            const bool ok = setCpuFrequencyMhz(static_cast<uint32_t>(mhz));
            logSerial.printf("CPU_%s:hold mhz=%u held=%u\n", ok ? "OK" : "ERR",
                             static_cast<unsigned>(getCpuFrequencyMhz()), cpuHold != nullptr ? 1u : 0u);
          }
        } else {
          logSerial.printf("CPU_ERR:unknown:HOLD,AUTO\n");
        }
#endif  // ENABLE_CPU_CMD
#ifdef ENABLE_REFRESH_CMD
      } else if (cmd == "REFRESH" || cmd.startsWith("REFRESH ")) {
        // Drive the panel through N refreshes of one mode at a fixed cadence
        // and report every duration, so the meter sees a repeated cycle long
        // enough to integrate.
        //
        // **Why a repeat and not one refresh.** The bench meter samples about
        // 66 times a second, so a single ~1,100 ms whole-panel frame on this
        // board lands as roughly 70 samples with a state change at each end
        // (parent repo docs/power-bench.md, "What this bench cannot do"). One
        // frame is a spike to be integrated, not a level to be read. A run of
        // them at a known cadence is a level: the mean draw over the run minus
        // the idle draw of the same screen, times the run length, divided by
        // the count, is the energy one refresh costs.
        //
        //   CMD:REFRESH                              ->  REFRESH:modes=fast,half,full
        //   CMD:REFRESH fast 30                      ->  30 refreshes, no gap, flipping
        //   CMD:REFRESH half 20 5000                 ->  20 refreshes, 5 s apart
        //   CMD:REFRESH fast 30 2000 light           ->  a tenth of the rows change
        //   CMD:REFRESH fast 30 2000 none            ->  nothing changes between frames
        //
        // **The pattern is part of the measurement, not a detail.** FAST is
        // differential on both panels this firmware drives (docs/refresh-modes.md):
        // it moves only the pixels that differ from the previous plane, so
        // `none` asks what the *call* costs when there is nothing to do, `flip`
        // asks what the panel costs when every pixel moves, and `light` sits
        // where a map redraw sits. Quoting one of them as "the cost of a
        // refresh" without saying which is how a number becomes folklore.
        //
        // Writes straight into the framebuffer the panel already owns, exactly
        // as CMD:SHOWIMAGE does, so whatever was on screen is destroyed. The
        // next activity repaint cleans it up.
        String rest = cmd.length() > 7 ? cmd.substring(8) : String("");
        rest.trim();
        if (rest.isEmpty()) {
          logSerial.printf("REFRESH:modes=fast,half,full patterns=flip,light,none\n");
        } else {
          // mode count [gap_ms] [pattern]
          String tok[4];
          int nTok = 0;
          int from = 0;
          while (nTok < 4 && from <= static_cast<int>(rest.length())) {
            int to = rest.indexOf(' ', from);
            if (to < 0) to = rest.length();
            String t = rest.substring(from, to);
            t.trim();
            if (t.length() > 0) tok[nTok++] = t;
            from = to + 1;
          }
          String modeName = tok[0];
          modeName.toLowerCase();
          const long count = nTok > 1 ? tok[1].toInt() : 0;
          const long gapMs = nTok > 2 ? tok[2].toInt() : 0;
          String pattern = nTok > 3 ? tok[3] : String("flip");
          pattern.toLowerCase();

          HalDisplay::RefreshMode mode = HalDisplay::RefreshMode::FAST_REFRESH;
          bool modeOk = true;
          if (modeName == "fast") {
            mode = HalDisplay::RefreshMode::FAST_REFRESH;
          } else if (modeName == "half") {
            mode = HalDisplay::RefreshMode::HALF_REFRESH;
          } else if (modeName == "full") {
            mode = HalDisplay::RefreshMode::FULL_REFRESH;
          } else {
            modeOk = false;
          }

          const bool patternOk = (pattern == "flip" || pattern == "light" || pattern == "none");
          // 15 minutes of wall clock, assuming a 2 s worst-case frame. The cap
          // is on the *run*, not on the count: a 200-frame run with a 10 s gap
          // would hold the main loop for half an hour and the device would look
          // dead to everything else on it.
          const long budgetMs = count * (gapMs + 2000);

          if (!modeOk) {
            logSerial.printf("REFRESH_ERR:mode:fast,half,full\n");
          } else if (!patternOk) {
            logSerial.printf("REFRESH_ERR:pattern:flip,light,none\n");
          } else if (count < 1 || count > 400) {
            logSerial.printf("REFRESH_ERR:count:1-400\n");
          } else if (gapMs < 0 || gapMs > 60000) {
            logSerial.printf("REFRESH_ERR:gap:0-60000\n");
          } else if (budgetMs > 900000) {
            logSerial.printf("REFRESH_ERR:budget:%ld ms over 900000\n", budgetMs);
          } else {
            uint8_t* buf = display.getFrameBuffer();
            const uint32_t bufferSize = display.getBufferSize();
            if (buf == nullptr || bufferSize == 0) {
              logSerial.printf("REFRESH_ERR:no framebuffer\n");
            } else {
              // Held for the whole run for the same reason CMD:SHOWIMAGE holds
              // it for one frame: the render task writes this same buffer, and
              // a repaint landing between the pattern and the refresh would put
              // an unknown image in the middle of a measurement.
              RenderLock lock;
              unsigned long total = 0;
              unsigned long worst = 0;
              unsigned long best = 0xFFFFFFFFul;
              const unsigned long runStart = millis();
              logSerial.printf("REFRESH_BEGIN:mode=%s count=%ld gap_ms=%ld pattern=%s bytes=%u\n", modeName.c_str(),
                               count, gapMs, pattern.c_str(), static_cast<unsigned>(bufferSize));
              for (long i = 0; i < count; i++) {
                if (pattern == "flip") {
                  // bit 1 = white (CMD:SHOWIMAGE's wire format), so this is a
                  // whole-panel black/white alternation: every pixel moves.
                  memset(buf, (i & 1) ? 0xFF : 0x00, bufferSize);
                } else if (pattern == "light") {
                  // One byte in ten flipped, spread across the whole buffer, so
                  // the changed area is about a tenth of the panel rather than
                  // a tenth of one edge.
                  const uint8_t fill = (i & 1) ? 0xFF : 0x00;
                  for (uint32_t b = 0; b < bufferSize; b += 10) {
                    buf[b] = fill;
                  }
                }
                const unsigned long t0 = millis();
                display.displayBuffer(mode);
                const unsigned long ms = millis() - t0;
                total += ms;
                if (ms > worst) worst = ms;
                if (ms < best) best = ms;
                logSerial.printf("REFRESH:i=%ld ms=%lu\n", i, ms);
                if (gapMs > 0) delay(static_cast<uint32_t>(gapMs));
              }
              const unsigned long span = millis() - runStart;
              logSerial.printf("REFRESH_END:mode=%s count=%ld total_ms=%lu mean_ms=%lu min_ms=%lu max_ms=%lu span_ms=%lu\n",
                               modeName.c_str(), count, total, total / static_cast<unsigned long>(count), best, worst,
                               span);
            }
          }
        }
#endif  // ENABLE_REFRESH_CMD
#ifdef ENABLE_SDBUS_CMD
      } else if (cmd == "SDBUS" || cmd.startsWith("SDBUS ")) {
        // Bench instrument for BUG-037: toggle the three things
        // t5s3DeselectLoraRadio() does, plus the two it deliberately no longer does,
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
                // Level before direction, the same order disableGpsLora()
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
  // Touch counts as activity only while touch is allowed to do anything. With
  // it switched off the glass is dead input, so a pocket or a palm on it must
  // not hold the device awake.
  // The capacitive home key is neither a button nor a coordinate frame, so
  // neither of the two tests below sees it: wasAnyPressed/Released read the
  // button bitmask and wasTouchActivity() reads contact frames. A rider who
  // drove the device from that key alone was therefore slept on schedule and
  // spent the whole time on the throttled 50 ms loop, which also stretched the
  // key's own gesture timing.
  const bool homeKeyActivity = gpio.wasHomeKeyPressed() || gpio.wasHomeKeyTapped() || gpio.wasHomeKeyLongPressed();
  // An injected press counts as user input for both deadlines. Without it a
  // host walking the UI from a script would watch the device throttle and then
  // auto-sleep under it, which drops the very port the script is driving.
  const bool userInput = gpio.wasAnyPressed() || gpio.wasAnyReleased() || homeKeyActivity || DebugInput::active() ||
                         (TouchPolicy::touchActive() && gpio.wasTouchActivity()) || halTiltSensor.hadActivity();
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
      gpio.getPowerButtonHeldTime() > powerHoldDurationMs()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  //
  // Not on a board where the short press is already Back (boardButtonHook()):
  // the setting would then fire a full refresh on every step back, and the
  // rider has no way to see that the two are the same press.
  const bool shortPowerIsBack =
#if FREEINK_DEVICE_LILYGO
      BoardConfig::ACTIVE.board == BoardConfig::Board::LilyGoT5S3;
#else
      false;
#endif
  if (!shortPowerIsBack && SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
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
