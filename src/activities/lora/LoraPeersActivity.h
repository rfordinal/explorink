#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <I18n.h>  // StrId, for the error line the header holds

#include "activities/Activity.h"
#include "lora/LoraPeerTable.h"
#include "lora/LoraTeamId.h"
#include "util/ButtonNavigator.h"

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
#include "activities/map/TeamMembers.h"
#endif

// The LoRa node, and the group roster it feeds.
//
// **A bench screen, not a rider's screen**, behind `ENABLE_LORA_MESH` for the
// same reason the grayscale bench is behind `ENABLE_PREVIEW_BENCH`: it shows
// raw keys, RSSI and SNR, and it exists to answer "do these boards hear each
// other" (parent docs/lora.md, bring-up step 4).
//
// **It is also the only place the radio can be switched on by hand.** The
// first version listed peers and nothing else, which left `CMD:LORA MESH ON`
// over a USB cable as the only way to start the node -- not a thing a device
// on a handlebar has.
//
// **And it is the one place the group is managed**, decided 2026-10-22. The
// map menu's Group list stays read-only: it answers *where is everybody*,
// which is a rider's question mid-ride. Adding and removing people is a
// sit-down job, and one place where the roster changes is one place where it
// can be got wrong.
//
// Three sections, in this order:
//
//   1. the four radio controls
//   2. nodes heard on the air, each marked with its acronym when it is already
//      a member
//   3. members this radio is not hearing -- the section exists so somebody
//      who left the group can still be removed, which is impossible from a
//      list of only what the antenna can reach
//
// **Section three says "not on LoRa", not "not heard", and the distinction is
// the roster's whole design.** `MapTeam` is transport-neutral: a position
// reaches it from this radio, from a phone relay over BLE, or from the devel
// console, and a member carries one id per transport that has ever named them
// (docs/team-markers.md, "One person can be heard on more than one radio").
// This screen can only see the LoRa half, so it must not claim the other
// halves are silent. Answering "heard on anything" means asking the team layer
// for a last-position age rather than this radio's table, and that is a
// different question than this screen asks today.
//
// Select does the one thing that fits the row: a heard stranger is added (the
// acronym is guessed and then offered for editing), and a member is removed
// after a confirmation. Nothing here shows a position on the map; that stays
// the Group list's job.
class LoraPeersActivity final : public Activity {
 public:
  LoraPeersActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LoraPeers", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // The control rows, above everything else. Order is deliberate: the switch
  // that makes the rest mean anything comes first, and the one that starts
  // transmitting comes after the one that decides what is transmitted.
  enum Row : int {
    kRowMesh = 0,
    kRowBeacon,
    kRowName,
    kRowAdvert,
    kControlRowCount,
  };

  enum class RowKind : uint8_t { Control, HeardPeer, RosterOnly };

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

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
  // Per heard peer, the acronym it carries in the roster, or empty when it is
  // a stranger. Filled in refresh() so neither the draw nor the signature has
  // to walk the roster once per row.
  char peerAcr_[LoraPeerTable::kMaxPeers][kTeamAcrBytes] = {};
  // Roster entries nobody is hearing, as roster listing indices.
  uint8_t rosterOnly_[kTeamMaxMembers] = {};
  size_t rosterOnlyCount_ = 0;
#endif

  // Set when a row's action failed, cleared on the next one. Shown in the
  // header, because a control that silently does nothing is the worst kind.
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

  // What the cursor is on, as an identity rather than a row number.
  //
  // **Found on hardware, 2026-10-22, and it is a defect the periodic check
  // created.** A stranger was heard while the screen was open, the heard
  // section grew by a row, and a Select aimed at a group member landed on the
  // new node instead -- offering to *add* somebody who had just walked into
  // range. On a screen where one button both joins and removes people, a row
  // that moves under the cursor is not a cosmetic problem.
  //
  // So the selection is anchored to the thing selected: eight key bytes for a
  // heard node, the acronym for a member, the row number only for the fixed
  // controls. refresh() puts the cursor back on the same item wherever it
  // moved to, and falls back to the first row when it is gone entirely.
  struct Anchor {
    RowKind kind = RowKind::Control;
    int controlRow = 0;
    uint8_t key[8] = {};
#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
    char acr[kTeamAcrBytes] = {};
#endif
  };
  Anchor anchor_;

  void captureAnchor();
  void restoreAnchor();

  uint32_t listSignature() const;

  RowKind rowKind(int index) const;
  int rosterBase() const { return kControlRowCount + static_cast<int>(peerCount_); }
  int itemCount() const;

  void refresh();
  void activate(int index);
  void addHeardPeer(size_t peerIndex);
  void removeMember(const std::string& idOrAcr, const std::string& label);
  std::string rowTitle(int index) const;
  std::string rowValue(int index) const;
  std::string rowSubtitle(int index) const;
};

#endif  // ENABLE_LORA_MESH
