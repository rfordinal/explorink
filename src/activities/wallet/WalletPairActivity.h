#pragma once

#include <cstdint>
#include <memory>

#include "SyncChannel.h"
#include "activities/Activity.h"

// Pairing a phone with this device's wallet: the QR code (T-2098, step 2).
//
// The first of the three acts in docs/sync-channel.md ("Three acts, and they stay
// three"): pairing a phone, opening the gate, accepting a transfer. This screen
// is behind the gate already (it opens from the wallet list), and pairing here
// unlocks nothing and accepts nothing -- it only teaches the device one phone's
// public key.
//
// ## Not usable end to end yet
//
// The screen draws a real code from a real Channel, and the code is a real
// secret. But no BLE frame reaches this Channel: the wallet screens do not own
// the BLE server until T-2097 moves it there, and the phone half is T-174. So
// today the code times out unused. T-2097 attaches the transfer characteristic's
// frames to `channel_` while this screen is up; nothing else here changes.
//
// ## Buttons
//
//   CONFIRM   a new code (always a new secret -- a code is never shown twice)
//   LEFT      forget every paired phone; a second press confirms
//   BACK      back to the wallet list; the code dies with the screen
class WalletPairActivity final : public Activity {
 public:
  WalletPairActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void issue();
  void renderScreen(HalDisplay::RefreshMode mode);

  // ~1.4 KB: the handshake, both cipher states, one inner frame. On the heap for
  // the life of this screen only -- the map never carries it
  // (sync-channel.md, "Its state is owned by the wallet screen and dies with it").
  std::unique_ptr<syncchannel::Channel> channel_;
  bool ready_ = false;
  // What the panel shows now, so loop() repaints only when the code dies.
  bool showingCode_ = false;
  bool forgetArmed_ = false;
};
