#include "LoraMeshRadio.h"

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <MeshCore.h>  // MAX_TRANS_UNIT

namespace {

// Approximate SNR floor per spreading factor, SF7 first. Reimplemented from
// MeshCore's own table (helpers/radiolib/RadioLibWrappers.cpp, `snr_threshold`)
// rather than reached into, because that one is a file-static inside a class we
// deliberately do not link -- see the LoraMeshRadio class comment.
//
// **The numbers have to match theirs, not merely be plausible.** The score
// feeds Dispatcher::calcRxDelay(), which is how two nodes that heard the same
// packet avoid answering on top of each other: a node that scores a packet
// higher waits less. Score it on a different curve than the rest of the mesh
// and this device becomes the one that always talks first.
constexpr float kSnrFloorDb[] = {
    -7.5f,   // SF7
    -10.0f,  // SF8
    -12.5f,  // SF9
    -15.0f,  // SF10
    -17.5f,  // SF11
    -20.0f,  // SF12
};

}  // namespace

void LoraMeshRadio::loop() {
  if (!radio_->ready()) return;
  // A send in flight owns the chip, and finishing it is isSendComplete()'s
  // job, not this one's. Only the gap between "not transmitting" and "not
  // listening" is repaired here.
  if (radio_->transmitting()) return;
  if (!radio_->listening()) radio_->startListening();
}

int LoraMeshRadio::recvRaw(uint8_t* bytes, int sz) {
  if (!radio_->ready()) return 0;
  const int n = radio_->poll(bytes, static_cast<size_t>(sz));
  // -1 is a CRC failure. The Dispatcher's contract is 0 for "nothing arrived",
  // and a damaged packet is nothing it can do anything with -- the counter that
  // cares about the difference is the console's, not the mesh's.
  return n > 0 ? n : 0;
}

uint32_t LoraMeshRadio::getEstAirtimeFor(int lenBytes) { return radio_->timeOnAirMs(static_cast<size_t>(lenBytes)); }

float LoraMeshRadio::packetScore(float snr, int packetLen) {
  if (spreadingFactor_ < 7 || spreadingFactor_ > 12) return 0.0f;
  const float floorDb = kSnrFloorDb[spreadingFactor_ - 7];
  if (snr < floorDb) return 0.0f;

  const float bySnr = (snr - floorDb) / 10.0f;
  // 256 is MeshCore's own assumed maximum, one byte above MAX_TRANS_UNIT. Kept
  // as they wrote it for the same reason the table above is: the curve is a
  // shared convention between nodes, not a local tuning knob.
  const float collisionPenalty = 1.0f - (static_cast<float>(packetLen) / 256.0f);

  float score = bySnr * collisionPenalty;
  if (score < 0.0f) score = 0.0f;
  if (score > 1.0f) score = 1.0f;
  return score;
}

bool LoraMeshRadio::startSendRaw(const uint8_t* bytes, int len) {
  if (!radio_->ready()) return false;
  sendDone_ = false;
  sendFailed_ = false;
  // startTransmit(), never transmit(): the blocking one spins on yield() for
  // the whole time on air, and on this chip at a slow spreading factor that is
  // seconds of a starved idle task and a watchdog reboot
  // (lib/LoraRadio/include/LoraRadio.h, the transmit() comment).
  return radio_->startTransmit(bytes, static_cast<size_t>(len));
}

bool LoraMeshRadio::isSendComplete() {
  if (sendDone_) return true;
  const int state = radio_->finishTransmit();
  if (state == 0) return false;
  sendDone_ = true;
  sendFailed_ = state < 0;
  return true;
}

void LoraMeshRadio::onSendFinished() {
  sendDone_ = false;
  sendFailed_ = false;
  // Back to listening. LoraRadio::finishTransmit() deliberately does not
  // re-arm, because only its caller knows whether it wanted to be listening at
  // all -- and here the answer is always yes: a mesh node that stops listening
  // after it speaks hears nobody's reply.
  radio_->startListening();
}

bool LoraMeshRadio::isInRecvMode() const { return radio_->listening(); }

#endif  // ENABLE_LORA_MESH
