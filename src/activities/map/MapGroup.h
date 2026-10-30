#pragma once

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS

#include <cstdint>
#include <string_view>

#include "GroupFiles.h"
#include "GroupRoster.h"
#include "GroupSource.h"
#include "GroupStore.h"

// The group-markers layer: who is approved, where each of them was last seen,
// and the black box that outlives the ride.
//
// **This is the transport-independent seam.** groupPosition() is what a LoRa
// frame, a BLE relay from the phone and a devel console line all call, and
// nothing below it knows which one spoke (parent docs/lora.md, "What can be
// built today, with no hardware"). Today only the console calls it; the wire
// formats are a later pass and change nothing here.
//
// **The black box is written first, and the marker moves only if that worked.**
// Same rule and same reason as MapPins: RAM claiming a position the card never
// recorded survives to the next reboot as a lie, and the file is the half that
// somebody looking for a rider actually reads.
//
// Owned by MapActivity for the map screen's lifetime, and it *is* the
// IMapGroupSource the console talks to.
//
// RAM: one roster (twelve members, ~50 bytes each) plus one store (twelve fixes,
// ~28 bytes each), about 950 bytes inside the activity. No heap: the count is
// fixed by kGroupMaxMembers and cannot grow at runtime.
class MapGroup final : public IMapGroupSource {
 public:
  // **The roster outlives the map screen, since 2026-10-22.**
  //
  // It used to be a member of MapActivity, which heap-allocates and deletes
  // every activity, so the roster was loaded and the black box replayed on
  // every single map entry -- and no other screen could reach it at all. The
  // LoRa node list needs to add and remove members
  // (../../../docs/lora-mesh.md), and two owners of one roster file is how a
  // group ends up with two answers about who is in it.
  //
  // Same singleton shape as SETTINGS, Storage and GUI, because this codebase
  // already has one and a third mechanism would be a third thing to learn.
  //
  // Costs about 950 bytes of DRAM permanently rather than only while the map
  // is open -- twelve roster slots and twelve fixes, no heap. What it buys
  // beyond reachability: a member's last known position survives leaving the
  // map, which is what somebody looking for that person actually wants.
  static MapGroup& getInstance() {
    static MapGroup instance;
    return instance;
  }

  // Loads the roster, rotates the black box and rebuilds every member's last
  // known position from it. False when the card could not be read -- the roster
  // is then empty, which refuses every incoming position rather than letting
  // strangers through.
  bool begin();

  const GroupRoster& roster() const { return roster_; }
  const GroupStore& store() const { return store_; }

  // IMapGroupSource. The ingest and the roster edits; see that interface for what
  // each result means.
  Ingest groupPosition(std::string_view idOrAcr, const GroupFix& fix) override;
  Edit groupAdd(std::string_view acr, std::string_view id, std::string_view name) override;
  Edit groupRemove(std::string_view idOrAcr) override;
  bool groupReload(size_t& skipped) override;
  size_t groupCount() const override;
  GroupMember groupMemberAt(size_t index) const override;
  GroupFix groupFixAt(size_t index) const override;
  uint32_t groupLogPage(uint32_t offset, uint32_t maxCount, IGroupLogVisitor& visitor) override;

  // The rider's own row in the same file, when they have turned it on.
  //
  // **Off by default and its own switch**, separate from the peers' rows: this
  // one is the rider's own movement history on a card that can be lost with the
  // device, and that is their call to make, not a side effect of riding with a
  // group (../../../docs/group-markers.md, "Two switches, because it is two kinds of
  // data").
  //
  // Rate-limited, unlike a peer's position: a phone pushes a fix a second, and a
  // row per fix would be a card write per second for a whole ride.
  void logSelf(int32_t latE7, int32_t lonE7, uint32_t utc, uint8_t heading, bool hasHeading, uint16_t speedKmh,
               bool hasSpeed, GroupFixSource source, uint32_t nowUptimeMs);

  // Which roster slot the nth listed member is. Listing order is slot order.
  size_t slotForIndex(size_t index) const;

  // Positions refused since boot because the sender was not in the roster. Shown
  // by the map's debug readout rather than swallowed: on a radio this is the
  // number that says somebody outside the group is talking.
  uint32_t strangersRefused() const { return strangersRefused_; }

  // Unix seconds, or 0 when the device genuinely does not know the time. Same
  // source and same rule as MapPins::utcNowOrZero().
  static uint32_t utcNowOrZero();

  // This run's identity, drawn once from the hardware RNG and never zero.
  //
  // It is what makes a stored `uptime_ms` usable: on a device with no clock the
  // age of a position is the difference between two uptimes, and that only means
  // anything inside one run. Written into every black box row and compared on
  // replay (GroupRecord.h, the `boot` column).
  static uint32_t bootId();

  // How often, and how far, before the rider's own row is written again.
  // Deliberately not settings: a rider choosing a logging cadence is a
  // preference nobody asked for, and both numbers exist only to keep a
  // once-a-second fix off the card.
  static constexpr uint32_t kSelfLogMinIntervalMs = 30000;
  static constexpr uint32_t kSelfLogMinMetres = 25;

 private:
  bool loaded_ = false;
  GroupRoster roster_;
  GroupStore store_;
  uint32_t strangersRefused_ = 0;
  // Last row written for the rider, for the rate limit above.
  uint32_t selfLastUptimeMs_ = 0;
  int32_t selfLastLatE7_ = 0;
  int32_t selfLastLonE7_ = 0;
  bool selfLogged_ = false;
};

#endif  // ENABLE_GROUP_MARKERS
