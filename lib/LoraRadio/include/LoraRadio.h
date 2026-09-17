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
// against LORA_CS46), and LORA_CS also reaches the panel's i80 bus as its
// dc_gpio_num, because that driver rejects a negative pin and this board has
// no spare GPIO (LilyGoT5S3LgfxConfig.cpp, prepareEpdPower() and the pinPwr
// comment). The SDK deselects it before the bus is built and lgfx::pinMode()
// writes no level, so it stays HIGH afterwards -- the radio is not selected by
// an ordinary refresh.
//
// What protects the card while the radio is off is park(): it holds NRESET
// low, which parks the chip's MISO (measured 2026-09-03, nine runs,
// main.cpp t5s3DeselectLoraRadio()).
//
// **A radio that listens while the map renders is a servicing problem, not a
// bus problem.** Measured 2026-09-16: a board rendering at zoom rung 6 heard
// 8 of 20 packets with zero card errors and zero CRC failures. Both users
// bracket the bus in SPI transactions, so the transfers serialise; what was
// lost was everything arriving while nobody called poll(). Whoever owns this
// class has to service it off the rendering path (docs/lora-bringup.md, "The
// radio has its own task now"; parent docs/TODO.md, T-2019 and T-2023).
//
// Two narrower bus questions stay open and unmeasured: the microsecond window
// where the i80 peripheral drives DC on this pin during bus setup, and whether
// anything re-initialises the display bus while the radio is up.

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
  float tcxoVoltage = 1.8f;       // SX126X_DIO3_TCXO_VOLTAGE
  bool dio2AsRfSwitch = true;     // SX126X_DIO2_AS_RF_SWITCH
  float currentLimitMa = 140.0f;  // SX126X_CURRENT_LIMIT
  bool rxBoostedGain = true;      // SX126X_RX_BOOSTED_GAIN
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
  //
  // **Only safe from a task whose core has no watchdog on its idle task.**
  // RadioLib waits for TxDone in a spin that calls the platform's yield(), and
  // on ESP32 Arduino that is vPortYield() (esp32-hal-misc.c, __yield), which
  // hands the CPU only to equal-or-higher priority work. A caller above idle
  // priority therefore starves its core's idle task for the whole time on air,
  // and `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y` with
  // `CONFIG_ESP_TASK_WDT_PANIC=y` turns a long send into a reboot. Seconds of
  // air are ordinary here: a slow spreading factor on a narrow bandwidth is
  // exactly what a range test reaches for. Use startTransmit() off the loop
  // task.
  bool transmit(const uint8_t* data, size_t length);
  bool transmit(const char* text);

  // The same send, without the spin. Hands the packet to the chip and returns;
  // the caller learns it finished from DIO1, which carries TxDone and nothing
  // else while transmitting. Leaves the radio not listening either way --
  // finishTransmit() does not re-arm, because only the caller knows whether it
  // wanted to be listening at all.
  bool startTransmit(const uint8_t* data, size_t length);
  bool startTransmit(const char* text);

  // True between a startTransmit() and the finishTransmit() that clears it.
  bool transmitting() const;

  // 1 when the send completed and the chip was released, 0 when it is still on
  // air, -1 on an error (the chip is released in that case too). Safe to call
  // when nothing is transmitting: it answers 0.
  int finishTransmit();

  // Ends a send that never reported TxDone. Without it a chip that missed its
  // interrupt would leave transmitting() true forever and the radio would
  // never listen again.
  void abortTransmit();

  // Puts the chip in continuous receive and leaves it there. Non-blocking on
  // purpose: a blocking listen would hold the firmware's loop() -- and with it
  // the panel, the buttons and BLE -- for the whole listening window.
  bool startListening();
  void stopListening();
  bool listening() const;

  // Reads one packet if one is waiting. Returns the byte count, 0 when nothing
  // arrived, -1 when a packet arrived damaged. rssi() and snr() describe that
  // packet and are overwritten by the next one.
  //
  // **The caller decides how often this runs, and that decision is the whole
  // game.** An SX126x holds exactly one packet: whatever arrives while the
  // previous one is still in the chip is lost. Called from a firmware loop()
  // that also renders a map, that meant 8 of 20 packets on this board
  // (parent docs/TODO.md, T-2023). Nothing in this class can fix that -- the
  // fix is where poll() is called from.
  int poll(uint8_t* buffer, size_t bufferSize);

  // Hands the DIO1 line to an interrupt handler, so a caller does not have to
  // poll a pin to learn that a packet landed. RadioLib attaches it to the pin
  // this class was given, which is why it lives here rather than in the board
  // code: the pin number is this class's business and nobody else's.
  //
  // **The handler runs in interrupt context**, so it may not touch SPI, the
  // radio, or anything that takes a lock -- an SX126x is read over the shared
  // bus and that read cannot happen in an ISR. Give a semaphore and leave.
  bool setPacketAction(void (*handler)());
  void clearPacketAction();

  // Puts the chip in its own sleep state, then holds NRESET low. The reset is
  // what makes the SD card safe again, not the sleep: an SX1262 in reset parks
  // MISO, and the card shares that wire.
  void park();

  // park() plus forgetting the chip. begin() can be called again afterwards.
  void end();

  float rssi() const { return rssi_; }
  float snr() const { return snr_; }

  // Carrier frequency offset of the last received packet, in Hz, as the chip
  // measured it. This is the instrument for the oscillator question: a TCXO
  // that started is accurate to a few ppm, and the two boards' combined error
  // is what a receiver has to absorb inside its bandwidth. Distance does not
  // enter into it, so it is answerable on a desk.
  float frequencyError();

  // The chip's own error register, and a test that provokes it.
  //
  // **A successful begin() does not mean the TCXO is running.** When the
  // oscillator fails to start, RadioLib clears the TCXO voltage and silently
  // retries in crystal mode (SX126x.cpp:1444-1450), so the radio comes up
  // either way. oscillatorStarts() asks the chip directly: clear the errors,
  // force standby on the external oscillator, and read the errors back.
  // XOSC_START_ERR (bit 0x20) means the part this board actually has did not
  // start at the voltage we configured.
  uint16_t deviceErrors();
  bool oscillatorStarts(uint16_t* errorsOut);

  // An unmodulated carrier, for measuring what the power amplifier draws.
  // RSSI at desk distance cannot separate an SX1261 from an SX1262 -- the two
  // differ by about 85 mA of PA current at full power, and that is measurable
  // with a USB meter and nothing else (parent docs/usb-power-meter.md).
  //
  // **Transmits continuously until it is turned off**, which is why the console
  // bounds it rather than exposing it raw.
  bool carrier(bool on);

  // RadioLib's status code from whatever failed last, 0 when nothing did.
  int lastError() const { return lastError_; }

  // The 16-byte version string the chip reports at register 0x0320. Measured
  // on this board 2026-09-16: "SX1261 V2D 2D02".
  //
  // **It does not say which part is fitted, and it cannot.** RadioLib expects
  // the same string from an SX1262 (`SX1262.h:16`, RADIOLIB_SX1262_CHIP_TYPE
  // is "SX1261"), so the two answer identically -- which is also why begin()
  // succeeding proves the radio is alive and proves nothing about its model.
  // The difference that matters is the power amplifier: SX1261 stops at
  // +15 dBm, SX1262 reaches +22. Settle it with the vendor schematic or a
  // measured output sweep, never with this string.
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
