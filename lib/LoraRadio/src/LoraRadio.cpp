#include "LoraRadio.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>

#include <new>

// RadioLib's objects are big and its headers are heavy, so they stay out of
// LoraRadio.h: everything above this line can include the radio without
// pulling RadioLib into its own translation unit.
struct LoraRadio::Impl {
  LoraPins pins;
  Module module;
  SX1262 radio;
  bool listening = false;

  Impl(const LoraPins& boardPins)
      // The fourth pin is RadioLib's "gpio", which for an SX126x is BUSY.
      // Passing the real pin matters: without it RadioLib falls back to a
      // fixed delay after every command instead of waiting on the line, which
      // is both slower and unreliable at the transfer rates the SD card sets
      // on this shared bus.
      : pins(boardPins),
        module(boardPins.cs, boardPins.irq, boardPins.rst, boardPins.busy, SPI,
               SPISettings(2000000, MSBFIRST, SPI_MODE0)),
        radio(&module) {}
};

bool LoraRadio::begin(const LoraPins& pins, const LoraConfig& config) {
  if (impl_ != nullptr) end();

  // Nothrow, because a bare `new` that fails on ESP32 calls abort() rather than
  // returning null (CLAUDE.md, rule 9) -- and "the radio could not start" has to
  // stay a value the caller can log, not a panic.
  impl_ = new (std::nothrow) Impl(pins);
  if (impl_ == nullptr) {
    lastError_ = RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED;
    return false;
  }

  // RadioLib checks that an SX126x answers at all before configuring it:
  // begin() calls SX126x::findChip(), which reads the version string at 0x0320
  // and returns RADIOLIB_ERR_CHIP_NOT_FOUND (-2) when nothing matches.
  // **That check does not identify the model.** RadioLib expects "SX1261" from
  // an SX1262 too (`SX1262.h:16`), so it separates "a radio is there, powered
  // and out of reset" from "nothing answers", and nothing finer. Which part is
  // fitted stays open (parent docs/lora.md).
  //
  // useRegulatorLDO stays false, i.e. the DC-DC converter: LilyGo's modules
  // carry the inductor for it, and the LDO would roughly double receive
  // current. Unverified for this board -- if the radio answers SPI but never
  // receives, this flag is one of the three suspects, with tcxoVoltage and
  // dio2AsRfSwitch.
  lastError_ = impl_->radio.begin(config.freqMhz, config.bandwidthKhz, config.spreadingFactor, config.codingRate,
                                  config.syncWord, config.txPowerDbm, config.preambleLength, config.tcxoVoltage,
                                  /*useRegulatorLDO=*/false);
  if (lastError_ != RADIOLIB_ERR_NONE) {
    end();
    return false;
  }

  // The antenna switch is wired to DIO2 on every SX1262 module of this shape.
  // Without it the chip transmits into a disconnected path: the send still
  // reports success and nothing ever hears it.
  //
  // RadioLib's begin() already sets this (SX126x.cpp:57), so this is a
  // deliberate re-statement rather than the fix it looks like -- the setting is
  // one of the three that decide whether the radio is heard at all, and a
  // reader should find it named here with the other two.
  if (config.dio2AsRfSwitch) {
    lastError_ = impl_->radio.setDio2AsRfSwitch(true);
    if (lastError_ != RADIOLIB_ERR_NONE) {
      end();
      return false;
    }
  }

  // Over-current protection for the PA. 140 mA is what the one project running
  // MeshCore on this board uses. **RadioLib's** begin() has already set 60 mA
  // (SX126x.cpp:54), which clips a +22 dBm transmit -- that number is the
  // library's choice, not the chip's power-on default.
  //
  // Checked, unlike an earlier version of this code: a silent failure here
  // leaves the radio transmitting at reduced power, which looks like bad range
  // and nothing else.
  lastError_ = impl_->radio.setCurrentLimit(config.currentLimitMa);
  if (lastError_ != RADIOLIB_ERR_NONE) {
    end();
    return false;
  }

  // Buys receive sensitivity for a little more receive current. **How much of
  // each is [open] here**: the figures an earlier comment carried (2 dB for
  // 2 mA) had no source, and the SX126x datasheet is behind a form
  // (parent docs/TODO.md, T-285). Recorded as a deliberate setting either way,
  // so the power campaign knows which state it measured.
  lastError_ = impl_->radio.setRxBoostedGainMode(config.rxBoostedGain);
  if (lastError_ != RADIOLIB_ERR_NONE) {
    end();
    return false;
  }

  ready_ = true;
  return true;
}

bool LoraRadio::transmit(const uint8_t* data, size_t length) {
  if (!ready_) return false;

  const bool wasListening = impl_->listening;

  // transmit() leaves the chip in standby whether it worked or not
  // (SX126x.cpp:248-251), so the flag stops being true the moment the call
  // returns -- not when the re-arm below succeeds. A listening flag that
  // outlives the listening is how a deaf radio reports itself as healthy.
  impl_->listening = false;

  const int16_t sent = impl_->radio.transmit(data, length);
  lastError_ = sent;

  // Re-arm only after a send that worked: after a failure the chip's state is
  // not known, and startReceive() on top of that hides the original error.
  // The result of the send is captured above, because startListening()
  // overwrites lastError_ -- returning that instead reported a packet that
  // really went out as a failure, and a caller would send it twice.
  if (wasListening && sent == RADIOLIB_ERR_NONE) startListening();

  return sent == RADIOLIB_ERR_NONE;
}

bool LoraRadio::transmit(const char* text) { return transmit(reinterpret_cast<const uint8_t*>(text), strlen(text)); }

bool LoraRadio::startListening() {
  if (!ready_) return false;
  lastError_ = impl_->radio.startReceive();
  impl_->listening = lastError_ == RADIOLIB_ERR_NONE;
  return impl_->listening;
}

void LoraRadio::stopListening() {
  if (!ready_) return;
  impl_->radio.standby();
  impl_->listening = false;
}

bool LoraRadio::listening() const { return impl_ != nullptr && impl_->listening; }

int LoraRadio::poll(uint8_t* buffer, size_t bufferSize) {
  if (!ready_ || !impl_->listening) return 0;

  // DIO1 is the only thing polled here, deliberately: reading the chip's IRQ
  // register over SPI on every loop() would take the shared bus away from the
  // SD card thousands of times a second for nothing. The pin is high exactly
  // when a packet is waiting.
  if (digitalRead(impl_->pins.irq) == LOW) return 0;

  const size_t length = impl_->radio.getPacketLength();
  size_t wanted = length < bufferSize ? length : bufferSize;

  // **Never pass 0 to readData().** RadioLib reads the length again itself and
  // treats a zero `len` as "no limit", copying the whole packet -- up to 255
  // bytes -- into the caller's buffer (SX126x.cpp:557-563). Zero is reachable
  // here: getPacketLength() returns the zero-initialised status byte when its
  // own SPI read fails, which on a bus shared with the SD card is not
  // hypothetical. Clamping to the buffer keeps that from writing past it.
  if (wanted == 0) wanted = bufferSize;
  if (wanted == 0) {
    impl_->radio.startReceive();
    return 0;
  }

  lastError_ = impl_->radio.readData(buffer, wanted);

  // RSSI and SNR describe the packet just read and are only valid here, before
  // the next startReceive() overwrites them.
  rssi_ = impl_->radio.getRSSI();
  snr_ = impl_->radio.getSNR();

  // Back to listening whatever happened: a CRC error is a normal event on a
  // radio link, not a reason to go deaf. The result is checked, because a
  // failed re-arm is the worst failure this class can have -- the radio stops
  // hearing anything while listening() still says it is listening, which is
  // indistinguishable from quiet air.
  const int16_t rearmed = impl_->radio.startReceive();
  if (rearmed != RADIOLIB_ERR_NONE) {
    impl_->listening = false;
    lastError_ = rearmed;
    return -1;
  }

  if (lastError_ != RADIOLIB_ERR_NONE) return -1;
  return static_cast<int>(wanted);
}

bool LoraRadio::setPacketAction(void (*handler)()) {
  if (!ready_ || handler == nullptr) return false;
  impl_->radio.setPacketReceivedAction(handler);
  return true;
}

void LoraRadio::clearPacketAction() {
  if (impl_ == nullptr) return;

  // Not gated on ready_, unlike setPacketAction(). park() clears ready_ while
  // the interrupt is still attached to a live GPIO, so a gate here would leave
  // the handler armed on a pin whose radio is being held in reset -- and the
  // reset itself moves that line.
  impl_->radio.clearPacketReceivedAction();
}

float LoraRadio::frequencyError() {
  if (!ready_) return 0.0f;
  return impl_->radio.getFrequencyError();
}

uint16_t LoraRadio::deviceErrors() {
  if (!ready_) return 0;
  return impl_->radio.getDeviceErrors();
}

bool LoraRadio::oscillatorStarts(uint16_t* errorsOut) {
  if (errorsOut != nullptr) *errorsOut = 0;
  if (!ready_) return false;

  impl_->radio.clearDeviceErrors();

  // Standby on the external oscillator is what forces the question: in
  // STANDBY_RC the chip runs off its internal RC and never touches the TCXO,
  // so the error can only appear once something asks for the real clock.
  lastError_ = impl_->radio.standby(RADIOLIB_SX126X_STANDBY_XOSC);

  // The SX126x datasheet's TCXO startup delay is set by begin() as 16 ms; give
  // it more than that before reading the verdict.
  delay(50);

  const uint16_t errors = impl_->radio.getDeviceErrors();
  if (errorsOut != nullptr) *errorsOut = errors;

  // Back to the cheap standby whatever happened, so this leaves the radio the
  // way it found it.
  impl_->radio.standby();

  return (errors & RADIOLIB_SX126X_XOSC_START_ERR) == 0;
}

bool LoraRadio::carrier(bool on) {
  if (!ready_) return false;
  if (on) {
    impl_->listening = false;
    lastError_ = impl_->radio.transmitDirect();
  } else {
    lastError_ = impl_->radio.standby();
  }
  return lastError_ == RADIOLIB_ERR_NONE;
}

const char* LoraRadio::chipVersion() {
  version_[0] = '\0';
  if (impl_ == nullptr) return version_;

  // Read the version string ourselves rather than through RadioLib, whose
  // readRegister() is protected without RADIOLIB_GODMODE -- and GODMODE is a
  // library-wide switch that would be a strange price for one string.
  //
  // The SX126x SPI shape, from the datasheet's command table: opcode 0x1D
  // (ReadRegister), the 16-bit address, one dummy byte, then the data. BUSY
  // must be low before the transaction starts, which is the chip's rule for
  // every command.
  const uint32_t busyDeadline = millis() + 100;
  while (digitalRead(impl_->pins.busy) == HIGH) {
    if (static_cast<int32_t>(millis() - busyDeadline) > 0) {
      strncpy(version_, "busy", sizeof(version_) - 1);
      return version_;
    }
  }

  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(impl_->pins.cs, LOW);
  SPI.transfer(0x1D);
  SPI.transfer(0x03);
  SPI.transfer(0x20);
  SPI.transfer(0x00);
  for (size_t i = 0; i < 16; ++i) {
    const uint8_t byte = SPI.transfer(0x00);
    version_[i] = (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
  }
  digitalWrite(impl_->pins.cs, HIGH);
  SPI.endTransaction();
  version_[16] = '\0';
  return version_;
}

void LoraRadio::park() {
  if (impl_ == nullptr) return;
  if (ready_) {
    impl_->radio.standby();
    impl_->radio.sleep();
    impl_->listening = false;
  }

  // ready_ drops here, not only in end(). A parked chip is held in reset, so
  // its BUSY line stays high and every later RadioLib call would wait out the
  // 1000 ms bus timeout before failing (Module.h, spiConfig.timeout) -- several
  // of those per command, inside loop(), with the panel and the buttons behind
  // it. begin() is the way back.
  ready_ = false;

  // The sleep above is for the radio's own power; this line is for the SD
  // card. NRESET low parks the chip's MISO, and the card shares that wire with
  // it -- with the radio selected and out of reset, a card read fails
  // (measured 2026-09-03, main.cpp t5s3DeselectLoraRadio()). Every path out of
  // this class goes through here for that reason.
  pinMode(impl_->pins.rst, OUTPUT);
  digitalWrite(impl_->pins.rst, LOW);
}

void LoraRadio::end() {
  park();
  delete impl_;
  impl_ = nullptr;
  ready_ = false;
}
