#pragma once

#include <stddef.h>
#include <stdint.h>

// The SX1262 on the LilyGo T5 S3 Pro, wrapped thin.
//
// Thin on purpose. This is bring-up: one packet out, one packet in, with the
// wire settings named in one place so two boards can be told they are on the
// same air. No mesh, no routing, no protocol -- those come from MeshCore later
// and would be impossible to debug on top of an unproven radio, because every
// wrong sync word, bandwidth or spreading factor fails as silence rather than
// as an error (parent docs/lora.md, bring-up step 3).
//
// Board-agnostic like lib/Gnss: pins and power come from the caller, because
// the board knowledge lives in main.cpp. The radio's rail is shared with the
// GNSS receiver on this board, so this class deliberately does NOT switch
// power -- it assumes the rail is already up and says so in begin().
//
// **The SPI bus is shared with the SD card** (SCLK14 MISO21 MOSI13, SD_CS12
// against LORA_CS46) and LORA_CS is also handed to the panel driver as its
// pin_oe/pin_pwr. So nothing here may run while the card or the panel is in
// use; park() puts the radio back in reset, which is the state the card needs
// (measured 2026-09-03, main.cpp t5s3DeselectLoraRadio()). Arbitration between
// map rendering and a listening radio does not exist yet and is the next piece
// of work, not an oversight.

struct LoraPins {
  int8_t cs;    // NSS
  int8_t irq;   // DIO1
  int8_t rst;   // NRESET
  int8_t busy;  // BUSY
};

// Wire settings. The defaults are MeshCore's own defaults for the EU band
// (its platformio.ini: LORA_FREQ=869.618, LORA_BW=62.5, LORA_SF=8, LORA_CR=5,
// sync word RADIOLIB_SX126X_SYNC_WORD_PRIVATE), so that a MeshCore node and a
// board running this bring-up are on the same channel and each is evidence
// about the other. Changing one of these is changing which radios can hear us.
struct LoraConfig {
  float freqMhz = 869.618f;
  float bandwidthKhz = 62.5f;
  uint8_t spreadingFactor = 8;
  uint8_t codingRate = 5;
  uint8_t syncWord = 0x12;  // RADIOLIB_SX126X_SYNC_WORD_PRIVATE
  int8_t txPowerDbm = 14;
  uint16_t preambleLength = 16;

  // Module-level facts about this board, not preferences. All three are read
  // off dz0ny/meshcore-paperui's env:t5-epaper, which targets this exact board
  // (platformio.ini:102-113) -- secondhand until our own hardware confirms it.
  // A wrong TCXO voltage is the classic silent failure here: the radio answers
  // SPI and never hears anything.
  float tcxoVoltage = 1.8f;      // SX126X_DIO3_TCXO_VOLTAGE
  bool dio2AsRfSwitch = true;    // SX126X_DIO2_AS_RF_SWITCH
  float currentLimitMa = 140.0f; // SX126X_CURRENT_LIMIT
  bool rxBoostedGain = true;     // SX126X_RX_BOOSTED_GAIN
};

class LoraRadio {
 public:
  // Brings the chip up on the shared SPI bus. The rail must already be on and
  // the caller must not be touching the SD card. Returns false and leaves
  // lastError() set when the chip does not answer -- which on this board means
  // either "not powered", "held in reset by someone else", or "not an SX1262".
  bool begin(const LoraPins& pins, const LoraConfig& config);

  // True between a successful begin() and end().
  bool ready() const { return ready_; }

  // Blocking send. Returns false on a RadioLib error; airtime is the caller's
  // problem, not this class's -- see the duty-cycle note in docs/lora-bringup.md.
  bool transmit(const uint8_t* data, size_t length);
  bool transmit(const char* text);

  // Puts the chip in continuous receive and leaves it there. Non-blocking on
  // purpose: a blocking listen would hold the firmware's loop() -- and with it
  // the panel, the buttons and BLE -- for the whole listening window.
  bool startListening();
  void stopListening();
  bool listening() const;

  // Called from loop(). Returns the byte count of one received packet, 0 when
  // nothing arrived, -1 when a packet arrived damaged. rssi() and snr()
  // describe that packet and are overwritten by the next one.
  int poll(uint8_t* buffer, size_t bufferSize);

  // Puts the chip in its own sleep state, then holds NRESET low. The reset is
  // what makes the SD card safe again, not the sleep: an SX1262 in reset parks
  // MISO, and the card shares that wire.
  void park();

  // park() plus forgetting the chip. begin() can be called again afterwards.
  void end();

  float rssi() const { return rssi_; }
  float snr() const { return snr_; }

  // RadioLib's status code from whatever failed last, 0 when nothing did.
  int lastError() const { return lastError_; }

  // The 16-byte version string the chip reports at register 0x0320, e.g.
  // "SX1262 V2D". **This is the only thing that proves which part is fitted**
  // -- parent docs/lora.md marks the SX1262 identity [open] because it was
  // inferred from a BUSY pin in a header, never read off silicon.
  const char* chipVersion();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool ready_ = false;
  int lastError_ = 0;
  float rssi_ = 0.0f;
  float snr_ = 0.0f;
  char version_[17] = {0};
};
