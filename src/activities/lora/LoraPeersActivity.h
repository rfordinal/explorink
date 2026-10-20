#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include "activities/Activity.h"
#include "lora/LoraPeerTable.h"
#include "util/ButtonNavigator.h"

// Who else is on the air: one row per node whose advert this radio heard.
//
// **A bench screen, not a rider's screen**, and it is behind `ENABLE_LORA_MESH`
// for the same reason the grayscale bench is behind `ENABLE_PREVIEW_BENCH`: it
// exists to answer "do these two boards hear each other, and how far apart",
// which is the measurement the whole LoRa feature is blocked on (parent
// docs/lora.md, bring-up step 4). It shows raw keys, RSSI and SNR, and it is
// not what a group feature will eventually look like.
//
// **This is not the group roster.** The roster (`MapTeam`) is an allowlist and
// refuses a stranger; this lists everyone, because a board that is not in the
// roster yet is exactly the board a range test is trying to hear. Nothing on
// this screen puts a marker on the map.
//
// Select re-reads the radio. There is no auto-refresh timer: on e-ink a screen
// that repaints itself every few seconds is a screen that is unreadable while
// it does, and a range test is walked between deliberate readings anyway.
class LoraPeersActivity final : public Activity {
 public:
  LoraPeersActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LoraPeers", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  // The screen's own copy, taken under the chip mutex once per repaint. The
  // live table belongs to the radio task (src/lora/LoraPresence.h).
  LoraPeer peers_[LoraPeerTable::kMaxPeers];
  size_t peerCount_ = 0;
  uint32_t snapshotMs_ = 0;
  bool meshRunning_ = false;
  uint32_t beaconSec_ = 0;
  uint32_t rejected_ = 0;
  char nodeName_[21] = {0};

  void refresh();
};

#endif  // ENABLE_LORA_MESH
