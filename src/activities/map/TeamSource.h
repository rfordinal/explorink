#pragma once

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "TeamFix.h"
#include "TeamMembers.h"
#include "TeamRecord.h"

// Where a paged read of the black box hands its rows, same streaming contract as
// IPinLogVisitor: a page costs one record on the stack, not an array of them.
class ITeamLogVisitor {
 public:
  virtual ~ITeamLogVisitor() = default;
  virtual void onTeamLogRecord(const TeamRecord& rec) = 0;
};

// What a `team` command does, and the seam the radios will arrive through.
//
// **teamPosition() is the transport-independent ingest**, and it is the whole
// point of this interface: a LoRa frame, a BLE relay from the phone and a
// console line all land here, are checked against the same allowlist, write the
// same black box row and move the same marker (parent docs/lora.md, "the
// group-markers layer below stays the same"). Nothing above this line knows
// which radio spoke.
class IMapTeamSource {
 public:
  virtual ~IMapTeamSource() = default;

  enum class Ingest : uint8_t {
    Accepted,
    UnknownMember,  // not in the roster -- a stranger, or another group
    Muted,          // in the roster with `on` false; heard, deliberately not drawn
    WriteFailed,    // the card refused the row; the marker does not move either
  };

  enum class Edit : uint8_t {
    Ok,
    UnknownMember,
    Full,
    Invalid,      // unstorable acronym or id, or a pair that crosses an existing one
    WriteFailed,  // the roster file could not be rewritten; nothing changed in RAM
  };

  virtual Ingest teamPosition(std::string_view idOrAcr, const TeamFix& fix) = 0;
  virtual Edit teamAdd(std::string_view acr, std::string_view id, std::string_view name) = 0;
  virtual Edit teamRemove(std::string_view idOrAcr) = 0;
  // Re-reads the roster file after a hand edit on the card. `skipped` counts the
  // rows the file holds and the rules refused.
  virtual bool teamReload(size_t& skipped) = 0;

  virtual size_t teamCount() const = 0;
  virtual TeamMember teamMemberAt(size_t index) const = 0;  // only called with index < teamCount()
  virtual TeamFix teamFixAt(size_t index) const = 0;        // .present false = has not told us

  // Newest-first page of the black box. Returns the total row count, so the
  // caller can say where the next page starts; `maxCount` 0 asks for the total
  // only and must visit nothing (same contract as pinLogPage).
  virtual uint32_t teamLogPage(uint32_t offset, uint32_t maxCount, ITeamLogVisitor& visitor) = 0;
};

#endif  // ENABLE_TEAM_MARKERS
