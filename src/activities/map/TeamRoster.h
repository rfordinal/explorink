#pragma once

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "TeamMembers.h"

// The permanent list of group members, and the JSON document that holds it.
//
// Pure: no Arduino, no HAL, no SD. TeamRosterFile does the card; this does the
// bytes and the rules, so every one of them is host-testable (test/team/).
//
// **The file is the truth, and it is rewritten whole.** There is no append-only
// history here, unlike the pins log: a roster is a small setting, it is edited
// by a human between rides, and a partial write of twelve short rows is not a
// risk worth a second format. What *is* append-only is the black box, which is
// where the history of the feature actually lives (TeamBlackBox.h).
//
// Document, /trailink/team/members.json:
//
//   {"v":1,"members":[{"acr":"RF","name":"Roman","on":true,"ids":["a4c1380c","ble-7f"]}]}
//
// **`ids` is a list because one person can be heard on more than one radio**,
// and the transports call them different things (TeamMembers.h). The older
// single `"id":"..."` spelling still loads and becomes a one-entry list.
//
// An unknown version is refused rather than guessed at: loading a v2 roster with
// v1 rules could silently drop a member, and a member missing from an allowlist
// is a rider whose marker never appears and who is never told why.
class TeamRoster {
 public:
  static constexpr size_t kSlotCount = kTeamMaxMembers;
  static constexpr uint8_t kVersion = 1;

  void clear();

  size_t count() const;
  const TeamMember& at(size_t slot) const { return slots_[slot < kSlotCount ? slot : 0]; }

  // Slot for an exact id, or kSlotCount. Case sensitive: an id is machine text
  // and the transport that issued it decides its case.
  size_t findId(std::string_view id) const;

  // Slot for an acronym, or kSlotCount. Case insensitive, because a human types
  // this one (teamNormaliseAcr).
  size_t findAcr(std::string_view acr) const;

  // Either, id first. What the ingest path and the console both resolve through:
  // a radio hands over an id, a person at a console types an acronym, and both
  // have to land on the same member.
  size_t find(std::string_view idOrAcr) const;

  // Adds a member, or attaches another identifier to one that already exists.
  //
  // An acronym that is already in the roster keeps its slot and **gains** the
  // id rather than replacing it: the same rider heard over LoRa and relayed over
  // BLE is one person with two names, and only this device can know that.
  //
  // False when: the acronym or id is unstorable, the id already belongs to a
  // different member, the roster is full, or the member already carries
  // kTeamIdsPerMember identifiers.
  bool set(std::string_view id, std::string_view acr, std::string_view name, bool enabled);

  // False when no member holds that acronym or id.
  bool remove(std::string_view idOrAcr);

  // Replaces the whole roster from a JSON document. False on a malformed or
  // unknown-version document, and the roster is then left **empty** rather than
  // half-loaded -- a half-loaded allowlist silently drops members.
  //
  // Rows that are individually invalid (bad acronym, duplicate id, roster full)
  // are skipped and counted in `skipped`; the load still succeeds, because one
  // bad row must not cost the rider the other eleven members.
  bool parseJson(std::string_view json, size_t& skipped);

  // Writes the document. Returns the length written, or 0 when the buffer is too
  // small. Sized by kJsonBytes.
  size_t writeJson(char* buf, size_t bufLen) const;

  // Every member at their widest plus the framing, rounded up: twelve members
  // with three identifiers each is about 1.5 kB. Sized for the type, not for
  // today's data (same rule as kPinLineMax).
  static constexpr size_t kJsonBytes = 2048;

 private:
  size_t firstFreeSlot() const;

  TeamMember slots_[kSlotCount];
};
#endif  // ENABLE_TEAM_MARKERS
