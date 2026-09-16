#include "LoraRadio.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>

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
  impl_ = new Impl(pins);

  // RadioLib verifies the part before configuring it: begin() calls
  // SX126x::findChip(), which reads the 16-byte version string at 0x0320 and
  // returns RADIOLIB_ERR_CHIP_NOT_FOUND (-2) when it does not say "SX1262".
  // So a success here is the first hardware evidence of which radio is fitted
  // -- until now the part was inferred from a BUSY pin in a pin header
  // (parent docs/lora.md marks it [open]).
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
  // Without this the chip transmits into a disconnected path: the send still
  // reports success and nothing ever hears it.
  if (config.dio2AsRfSwitch) {
    lastError_ = impl_->radio.setDio2AsRfSwitch(true);
    if (lastError_ != RADIOLIB_ERR_NONE) {
      end();
      return false;
    }
  }

  // Over-current protection for the PA. 140 mA is what the one project running
  // MeshCore on this board uses; the chip's own default is 60 mA, which clips
  // a +22 dBm transmit.
  impl_->radio.setCurrentLimit(config.currentLimitMa);

  // About 2 dB of receive sensitivity for about 2 mA. Worth it on a device
  // that spends its life listening, and the choice is recorded here rather
  // than left to the chip default so the power measurement knows about it.
  impl_->radio.setRxBoostedGainMode(config.rxBoostedGain);

  ready_ = true;
  return true;
}

bool LoraRadio::transmit(const uint8_t* data, size_t length) {
  if (!ready_) return false;
  const bool wasListening = impl_->listening;
  lastError_ = impl_->radio.transmit(data, length);
  if (wasListening && lastError_ == RADIOLIB_ERR_NONE) startListening();
  return lastError_ == RADIOLIB_ERR_NONE;
}

bool LoraRadio::transmit(const char* text) {
  return transmit(reinterpret_cast<const uint8_t*>(text), strlen(text));
}

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
  const size_t wanted = length < bufferSize ? length : bufferSize;
  lastError_ = impl_->radio.readData(buffer, wanted);

  // RSSI and SNR describe the packet just read and are only valid here, before
  // the next startReceive() overwrites them.
  rssi_ = impl_->radio.getRSSI();
  snr_ = impl_->radio.getSNR();

  // Back to listening whatever happened: a CRC error is a normal event on a
  // radio link, not a reason to go deaf.
  impl_->radio.startReceive();

  if (lastError_ != RADIOLIB_ERR_NONE) return -1;
  return static_cast<int>(wanted);
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
