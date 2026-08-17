#include "MapPins.h"

#include <Arduino.h>
#include <BlePositionServer.h>
#include <Logging.h>

namespace {
constexpr const char* kLogTag = "PINS";
}

bool MapPins::begin() {
  const bool ok = PinLog::replay(store_, replayStats_);
  replayed_ = ok;
  if (!ok) LOG_ERR(kLogTag, "no history read -- saving a pin will be refused");
  return ok;
}

uint32_t MapPins::utcNowOrZero() {
  uint32_t utc = 0;
  if (!freeink::BlePositionServer::getInstance().utcNow(utc)) return 0;
  return utc;
}

bool MapPins::pinSet(std::string_view key, int32_t latE7, int32_t lonE7, uint32_t utc) {
  if (!replayed_) {
    // Appending onto a history that was never read would renumber seq and id from
    // 1 and overwrite the meaning of every record already on the card.
    LOG_ERR(kLogTag, "history not loaded -- refusing to save");
    return false;
  }

  PinRecord rec;
  if (!store_.makeSetRecord(key, latE7, lonE7, utc, millis(), rec)) {
    LOG_ERR(kLogTag, "no slot for '%.*s'", static_cast<int>(key.size()), key.data());
    return false;
  }
  // The log first, always. See the class comment.
  if (!PinLog::append(rec)) return false;
  if (!store_.apply(rec)) {
    // The record is on the card and the active set refused it: a replay will pick
    // it up, so the history is intact and only this session is out of date.
    LOG_ERR(kLogTag, "record %lu appended but not applied", static_cast<unsigned long>(rec.seq));
    return false;
  }
  LOG_INF(kLogTag, "%s %s at %ld,%ld (id %lu, seq %lu)", pinOpText(rec.op), rec.key, static_cast<long>(rec.latE7),
          static_cast<long>(rec.lonE7), static_cast<unsigned long>(rec.id), static_cast<unsigned long>(rec.seq));
  return true;
}

bool MapPins::pinDelete(std::string_view key) {
  if (!replayed_) {
    LOG_ERR(kLogTag, "history not loaded -- refusing to delete");
    return false;
  }

  PinRecord rec;
  if (!store_.makeDeleteRecord(key, utcNowOrZero(), millis(), rec)) return false;  // nothing there
  if (!PinLog::append(rec)) return false;
  if (!store_.apply(rec)) return false;
  LOG_INF(kLogTag, "del %s (id %lu, seq %lu)", rec.key, static_cast<unsigned long>(rec.id),
          static_cast<unsigned long>(rec.seq));
  return true;
}

size_t MapPins::pinCount() const { return store_.presentCount(); }

PinEntry MapPins::pinAt(size_t index) const {
  // Slot order is catalogue order, and the foreign-key slots come after it, so
  // walking the slots is already the order the lists want.
  size_t seen = 0;
  for (size_t slot = 0; slot < PinStore::kSlotCount; ++slot) {
    if (!store_.at(slot).present) continue;
    if (seen == index) return store_.at(slot);
    ++seen;
  }
  return PinEntry{};
}

uint32_t MapPins::pinLogPage(uint32_t offset, uint32_t maxCount, IPinLogVisitor& visitor) {
  return PinLog::page(offset, maxCount, visitor);
}
