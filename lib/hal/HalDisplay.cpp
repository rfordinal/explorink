#include <HalDisplay.h>
#include <HalGPIO.h>
#include <PowerTelemetry.h>

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

// Every panel refresh is counted here, at the one choke point all of them pass
// through, so a ride's panel cost is a number instead of a guess
// (PowerTelemetry.h). The count is by waveform because the waveforms do not
// cost the same, and the elapsed time is recorded because it is the closest
// stand-in for panel energy without an inline meter.
PowerTelemetry::Refresh convertTelemetryKind(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return PowerTelemetry::Refresh::Full;
    case HalDisplay::HALF_REFRESH:
      return PowerTelemetry::Refresh::Half;
    case HalDisplay::FAST_REFRESH:
    default:
      return PowerTelemetry::Refresh::Fast;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  const uint32_t startedUs = micros();
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
  POWER_TELEMETRY.onRefreshUs(convertTelemetryKind(mode), micros() - startedUs);
}

void HalDisplay::displayBufferAsync(HalDisplay::RefreshMode mode) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  // Counted here, timed in waitRefreshComplete() -- but only when the driver
  // actually defers. A driver with no supportsAsyncDisplay() override (every
  // LgfxEpdDriver board, so the whole T5 S3 Pro line) makes the facade take the
  // blocking path inside this call, the later wait returns instantly, and
  // billing 0 here dropped a whole refresh out of panel_busy_ms. Verified off
  // source 2026-09-09: LgfxEpdDriver overrides neither displayWindow nor
  // supportsAsyncDisplay, so PanelDriver's `return false` stands.
  //
  // So ask first, and when nothing will defer, bill what this call really cost.
  const bool defers = einkDisplay.supportsAsyncRefresh();
  const uint32_t startedUs = micros();
  einkDisplay.displayBufferAsyncNoShadow(convertRefreshMode(mode));
  const uint32_t elapsedUs = micros() - startedUs;
  if (defers) {
    POWER_TELEMETRY.onRefreshUs(convertTelemetryKind(mode), 0);
  } else {
    POWER_TELEMETRY.onAsyncCompletedInline(elapsedUs);
  }
}

void HalDisplay::waitRefreshComplete() {
  const uint32_t startedUs = micros();
  einkDisplay.waitRefreshComplete();
  POWER_TELEMETRY.onPanelWaitUs(micros() - startedUs);
}

bool HalDisplay::supportsAsyncRefresh() const { return einkDisplay.supportsAsyncRefresh(); }

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  const uint32_t startedUs = micros();
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
  POWER_TELEMETRY.onRefreshUs(convertTelemetryKind(mode), micros() - startedUs);
}

void HalDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen,
                               PowerTelemetry::WindowSite site) {
  const uint32_t startedUs = micros();
  einkDisplay.displayWindow(x, y, w, h, turnOffScreen);
  // Its own bucket, not folded into the three waveforms, because a marker-move
  // ride is mostly these (MapFollow) and they have to stay countable.
  //
  // This comment used to say a window "addresses only a rectangle, so it is
  // neither a full frame nor free". That is true of Ssd1677Driver, which
  // overrides displayWindow and drives the rectangle. It is false of
  // LgfxEpdDriver, which overrides nothing, so PanelDriver's base
  // implementation drops the rectangle and pushes a whole Fast frame -- on the
  // T5 S3 Pro this bucket and ref_fast are the *same operation*, and the
  // measured 2x between them is unexplained rather than a cost of scoping
  // (docs/t5s3-partial-refresh.md, section 1).
  POWER_TELEMETRY.onRefreshUs(PowerTelemetry::Refresh::Window, micros() - startedUs, site);
}

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) { return einkDisplay.lendBuildStorage(sizeOut); }

void HalDisplay::returnFrameBufferStorage() { einkDisplay.returnBuildStorage(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  // X3: a HALF fallback means the caller wants a clean base (e.g. the sleep
  // cover, a full-screen swap from arbitrary prior content). Without this, the
  // X3 grayscale base takes its gentle differential happy path and the prior
  // home/reader frame ghosts through the soft aa_pre_bw_mid waveform. Forcing a
  // resync makes displayGrayscaleBase clear first, matching displayBuffer(HALF).
  // The reader's FAST path is deliberately left on the differential path so
  // per-page grayscale stays cheap.
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return einkDisplay.supportsStripGrayscale(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
