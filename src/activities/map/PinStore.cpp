#include "PinStore.h"

#include <cstring>

void PinStore::clear() {
  for (size_t i = 0; i < kSlotCount; ++i) slots_[i] = PinEntry{};
  highestSeq_ = 0;
  highestId_ = 0;
}

size_t PinStore::slotFor(std::string_view key) const {
  const size_t index = pinCatalogIndex(key);
  if (index != kPinIndexUnknown) return index;

  // A foreign key keeps the slot it already occupies, so a replace of an unknown
  // pin does not consume a second one.
  for (size_t i = kPinSlotCount; i < kSlotCount; ++i) {
    if (slots_[i].present && key == std::string_view(slots_[i].key)) return i;
  }
  for (size_t i = kPinSlotCount; i < kSlotCount; ++i) {
    if (!slots_[i].present) return i;
  }
  return kSlotCount;
}

size_t PinStore::findSlot(std::string_view key) const {
  const size_t index = pinCatalogIndex(key);
  if (index != kPinIndexUnknown) return slots_[index].present ? index : kSlotCount;

  for (size_t i = kPinSlotCount; i < kSlotCount; ++i) {
    if (slots_[i].present && key == std::string_view(slots_[i].key)) return i;
  }
  return kSlotCount;
}

size_t PinStore::presentCount() const {
  size_t count = 0;
  for (size_t i = 0; i < kSlotCount; ++i) {
    if (slots_[i].present) ++count;
  }
  return count;
}

bool PinStore::apply(const PinRecord& rec) {
  // The counters move first and unconditionally. A record this build cannot
  // store still happened, and reusing its seq or its id later would make the log
  // ambiguous to whoever reads it next.
  if (rec.seq > highestSeq_) highestSeq_ = rec.seq;
  if (rec.id > highestId_) highestId_ = rec.id;

  const std::string_view key(rec.key);
  if (!isValidPinKey(key)) return false;

  if (rec.op == PinOp::Delete) {
    const size_t slot = findSlot(key);
    // A delete of a slot that is already empty is not a failure: the log is a
    // history, and replaying it twice must land in the same place.
    if (slot < kSlotCount) slots_[slot].present = false;
    return true;
  }

  const size_t slot = slotFor(key);
  if (slot >= kSlotCount) return false;  // foreign key, every unknown slot taken
  if (!rec.hasPos) return false;         // add/rep/res without a coordinate places nothing

  PinEntry& entry = slots_[slot];
  entry.present = true;
  entry.catalogIndex = pinCatalogIndex(key);
  entry.id = rec.id;
  entry.latE7 = rec.latE7;
  entry.lonE7 = rec.lonE7;
  entry.utc = rec.utc;
  entry.seq = rec.seq;
  memcpy(entry.key, rec.key, sizeof(entry.key));
  return true;
}

bool PinStore::makeSetRecord(std::string_view key, int32_t latE7, int32_t lonE7, uint32_t utc, uint32_t uptimeMs,
                             PinRecord& out) const {
  if (!isValidPinKey(key)) return false;
  const size_t slot = slotFor(key);
  if (slot >= kSlotCount) return false;

  PinRecord rec;
  if (!setPinRecordKey(rec, key)) return false;
  rec.seq = nextSeq();
  rec.utc = utc;
  rec.uptimeMs = uptimeMs;
  rec.latE7 = latE7;
  rec.lonE7 = lonE7;
  rec.hasPos = true;

  const PinEntry& entry = slots_[slot];
  if (entry.present) {
    rec.op = PinOp::Replace;
    rec.id = entry.id;  // same pin, moved
  } else {
    rec.op = PinOp::Add;
    rec.id = nextId();
  }
  out = rec;
  return true;
}

bool PinStore::makeDeleteRecord(std::string_view key, uint32_t utc, uint32_t uptimeMs, PinRecord& out) const {
  const size_t slot = findSlot(key);
  if (slot >= kSlotCount) return false;

  PinRecord rec;
  if (!setPinRecordKey(rec, key)) return false;
  rec.seq = nextSeq();
  rec.utc = utc;
  rec.uptimeMs = uptimeMs;
  rec.op = PinOp::Delete;
  rec.id = slots_[slot].id;
  rec.hasPos = false;
  out = rec;
  return true;
}
