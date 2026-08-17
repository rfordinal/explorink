#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "PinCatalog.h"
#include "PinRecord.h"

// The active pins, and the mutations that produce log records.
//
// Pure: no Arduino, no HAL, no SD. PinLog streams the card into apply(); this
// object never reads or writes a file, which is what makes every rule below
// host-testable (test/pins/).
//
// **Derived, never stored.** Boot replays the log and rebuilds this. One source
// of truth on the card, so the state and the history cannot disagree
// (docs/pins-plan.md, decision 3).
//
// Fixed array, no heap: kPinMaxEntries entries at ~32 bytes each, ~450 bytes of
// static DRAM inside whatever owns it. A std::vector was rejected because the
// count is bounded by the catalogue and cannot grow at runtime.
//
// Slot layout: slots 0..kPinSlotCount-1 belong to catalogue rows, one each, so a
// known key always lands in the same slot. Slots above that hold keys this build
// does not know (PinCatalog.h, kPinUnknownSlots).

struct PinEntry {
  bool present = false;
  size_t catalogIndex = kPinIndexUnknown;  // kPinIndexUnknown for a foreign key
  uint32_t id = 0;
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  uint32_t utc = 0;  // when the pin was placed; 0 = the device had no clock
  uint32_t seq = 0;  // the log record that placed it
  char key[kPinKeyBytes] = {};
};

class PinStore {
 public:
  void clear();

  // Replays one record. False means the record could not be stored -- today the
  // only cause is a foreign key arriving with every unknown slot already taken.
  // A false does not invalidate the replay; the event is simply not represented.
  bool apply(const PinRecord& rec);

  // Builds the record for a create-or-replace, without touching the store: the
  // caller appends it to the log first and calls apply() only if that worked, so
  // a full card cannot leave RAM claiming a pin the log has never heard of.
  //
  // Picks Add for an empty slot and Replace for an occupied one, keeps the id on
  // a Replace (same pin, moved) and takes a fresh one otherwise -- so a Camp
  // deleted and remade is two pins to whoever reads the log later
  // (docs/pins-plan.md, decision 5).
  bool makeSetRecord(std::string_view key, int32_t latE7, int32_t lonE7, uint32_t utc, uint32_t uptimeMs,
                     PinRecord& out) const;

  // Builds the Delete record. False when nothing is stored under that key --
  // deleting an empty slot is not an event and must not reach the log.
  bool makeDeleteRecord(std::string_view key, uint32_t utc, uint32_t uptimeMs, PinRecord& out) const;

  // One past the highest seq ever seen, so a reboot continues the sequence
  // rather than restarting it (docs/pins-plan.md, the log's `seq` field).
  uint32_t nextSeq() const { return highestSeq_ + 1; }
  uint32_t nextId() const { return highestId_ + 1; }

  size_t presentCount() const;

  static constexpr size_t kSlotCount = kPinMaxEntries;
  const PinEntry& at(size_t slot) const { return slots_[slot < kSlotCount ? slot : 0]; }

  // Slot holding `key`, or kSlotCount. Only ever a present slot: a cleared slot
  // keeps its key for nothing and find() must not resurrect it.
  size_t findSlot(std::string_view key) const;

  const PinEntry* find(std::string_view key) const {
    const size_t slot = findSlot(key);
    return slot < kSlotCount ? &slots_[slot] : nullptr;
  }

 private:
  // Where a key belongs, whether or not anything is there yet. kSlotCount when
  // the key is foreign and no unknown slot is free.
  size_t slotFor(std::string_view key) const;

  PinEntry slots_[kSlotCount];
  uint32_t highestSeq_ = 0;
  uint32_t highestId_ = 0;
};
