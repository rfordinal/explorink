#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <Dispatcher.h>  // MeshCore: mesh::Radio
#include <LoraRadio.h>

// MeshCore's radio interface, answered by the SX1262 driver this repo already
// proved on hardware.
//
// **Why not MeshCore's own RadioLibWrapper.** It ships one, and it works, and
// it would be the obvious thing to use. It drives RadioLib directly -- which on
// this board means a second owner of a chip select that is also the SD card's
// bus mate and the panel's `pin_pwr` (lib/LoraRadio/include/LoraRadio.h, the
// class comment). Everything that keeps those three apart lives in LoraRadio
// and in main.cpp's service task: one mutex over every chip call, NRESET held
// low while the radio is off so the card gets its MISO back, and a DIO1
// interrupt that services receive off the rendering path. A second driver would
// have to reproduce all of it or quietly break it.
//
// So this class is thin by design: it is a translation of one vocabulary into
// another, and it adds no radio behaviour of its own.
//
// **Every method assumes the chip mutex is already held by the caller.** The
// Dispatcher calls these from inside its loop(), that loop runs on the LoRa
// service task, and that task holds the lock for its whole serviced section.
// Taking the lock here as well would be a second, finer-grained critical
// section -- which is how a packet ends up half-read between two owners.
class LoraMeshRadio : public mesh::Radio {
 public:
  explicit LoraMeshRadio(LoraRadio& radio) : radio_(&radio) {}

  // mesh::Radio. See Dispatcher.h for each one's contract.
  //
  // begin() and loop() are the two the base class leaves empty, and both are
  // needed here. Nothing else puts this chip into receive: the Dispatcher only
  // *notices* that the radio is not in receive mode, raises
  // ERR_EVENT_STARTRX_TIMEOUT after eight seconds and carries on. Upstream's
  // own wrapper re-arms in its loop(), so a radio that silently stopped
  // listening is a failure mode their design already expects and repairs --
  // and ours repairs it the same way, for the same reason the console path has
  // its own re-arm watchdog in main.cpp.
  void begin() override { radio_->startListening(); }
  void loop() override;

  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int lenBytes) override;
  float packetScore(float snr, int packetLen) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  float getLastRSSI() const override { return radio_->rssi(); }
  float getLastSNR() const override { return radio_->snr(); }

  // The spreading factor the score table is read at. Set from the same
  // LoraConfig the radio was brought up with, because a score computed at the
  // wrong SF is a wrong collision-avoidance delay, not an error anybody sees.
  void setSpreadingFactor(uint8_t sf) { spreadingFactor_ = sf; }

 private:
  LoraRadio* radio_;
  uint8_t spreadingFactor_ = 8;

  // Latches what finishTransmit() answered, because LoraRadio::finishTransmit()
  // releases the chip on the call that reports success and the Dispatcher polls
  // isSendComplete() repeatedly until it is true. Asking the chip twice would
  // answer "still sending" the second time and strand the outbound packet until
  // its expiry.
  bool sendDone_ = false;
  bool sendFailed_ = false;
};

#endif  // ENABLE_LORA_MESH
