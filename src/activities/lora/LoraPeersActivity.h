#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <I18n.h>  // StrId, for the error line the header holds

#include "activities/Activity.h"
#include "lora/LoraPeerTable.h"
#include "util/ButtonNavigator.h"

// The LoRa node: its four controls, and who it has heard.
//
// **A bench screen, not a rider's screen**, and it is behind `ENABLE_LORA_MESH`
// for the same reason the grayscale bench is behind `ENABLE_PREVIEW_BENCH`: it
// exists to answer "do these two boards hear each other, and how far apart",
// which is the measurement the whole LoRa feature is blocked on (parent
// docs/lora.md, bring-up step 4). It shows raw keys, RSSI and SNR, and it is
// not what a group feature will eventually look like.
//
// **It is also the only place the radio can be switched on by hand.** The
// first version of this screen listed peers and nothing else, which left
// `CMD:LORA MESH ON` over a USB cable as the only way to start the node --
// not a thing a device on a handlebar has. The four rows at the top are that
// gap closed; each one writes the same CrossPointSettings field the console
// writes, so the two cannot disagree and the choice survives a reboot.
//
// **This is not the group roster.** The roster (`MapTeam`) is an allowlist and
// refuses a stranger; this lists everyone, because a board that is not in the
// roster yet is exactly the board a range test is trying to hear. Nothing on
// this screen puts a marker on the map.
//
// There is no auto-refresh timer: on e-ink a screen that repaints itself every
// few seconds is unreadable while it does, and a range test is walked between
// deliberate readings anyway. Every row activation re-reads the radio, and so
// does the Refresh button.
class LoraPeersActivity final : public Activity {
 public:
  LoraPeersActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LoraPeers", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // The control rows, above the heard nodes. Order is deliberate: the switch
  // that makes everything else mean anything comes first, and the one that
  // starts transmitting comes after the one that decides what is transmitted.
  enum Row : int {
    kRowMesh = 0,
    kRowBeacon,
    kRowName,
    kRowAdvert,
    kControlRowCount,
  };

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
  bool identityPersisted_ = false;
  char nodeName_[21] = {0};

  // Set when a control row failed, cleared on the next activation. Shown in
  // the header, because a row that silently does nothing is the worst thing a
  // switch can do -- a radio that refuses to start looks exactly like a radio
  // nobody has pressed.
  StrId lastError_ = StrId::STR_LORA_PEERS;
  bool hasError_ = false;

  // When the list was last re-read, and a fingerprint of what it drew.
  //
  // **The first version had no timer at all and that was the defect**: the one
  // screen whose job is "who is out there" could not show somebody arriving
  // while the rider watched it. The stated reason -- an e-ink screen must not
  // repaint every few seconds -- was right about the cost and wrong about the
  // answer, which is a slow check that repaints only when the drawn text would
  // actually differ. Same shape the map already uses for team marker ages.
  uint32_t lastCheckMs_ = 0;
  uint32_t drawnSignature_ = 0;
  static constexpr uint32_t kCheckIntervalMs = 20000;

  uint32_t listSignature() const;

  int itemCount() const { return kControlRowCount + static_cast<int>(peerCount_); }
  void refresh();
  void activate(int index);
  std::string rowTitle(int index) const;
  std::string rowValue(int index) const;
  std::string rowSubtitle(int index) const;
};

#endif  // ENABLE_LORA_MESH
