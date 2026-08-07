#include "MissingTilesStore.h"

#include <Logging.h>

#include <algorithm>

#include "MissingTilePriority.h"

namespace {
constexpr const char* kLogTag = "MTS";
}

void MissingTilesStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["tiles"].to<JsonArray>();
  for (const auto& hit : hits_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["z"] = hit.z;
    obj["col"] = hit.col;
    obj["row"] = hit.row;
    obj["count"] = hit.count;
  }
}

bool MissingTilesStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'tiles' key (treat as empty list); only a
  // JSON parse error is fatal, same convention as RecentBooksStore.
  hits_.clear();
  JsonArrayConst arr = doc["tiles"].as<JsonArrayConst>();
  hits_.reserve(std::min(arr.size(), kMaxEntries));
  for (JsonObjectConst obj : arr) {
    if (hits_.size() >= kMaxEntries) break;
    MissingTileHit hit;
    hit.z = obj["z"] | 0;
    hit.col = obj["col"] | 0;
    hit.row = obj["row"] | 0;
    hit.count = obj["count"] | 0;
    hits_.push_back(hit);
  }

  LOG_DBG(kLogTag, "missing tile list loaded from file (%d entries)", static_cast<int>(hits_.size()));
  return true;
}

void MissingTilesStore::record(uint8_t z, uint32_t col, uint32_t row) {
  auto it = std::find_if(hits_.begin(), hits_.end(),
                         [&](const MissingTileHit& hit) { return hit.z == z && hit.col == col && hit.row == row; });
  if (it != hits_.end()) {
    // Already on the list -- a count-only change, not a membership change.
    // Not marked dirty on its own (class comment); it still rides along
    // whenever some other tile's arrival or eviction triggers a flush.
    ++it->count;
    return;
  }

  // Field-by-field, not an aggregate initialiser: MissingTileHit carries a
  // `refused` flag between `z` and `col`, and a positional {z, col, row, 1}
  // would silently assign the column into it.
  MissingTileHit fresh;
  fresh.z = z;
  fresh.col = col;
  fresh.row = row;
  fresh.count = 1;
  // refused stays false: a tile nobody has been asked for yet has not been
  // refused, and re-recording a tile that WAS refused takes the branch above
  // instead, which leaves its flag alone.

  if (hits_.size() >= kMaxEntries) {
    // Full: give up the slot with the fewest hits so far rather than drop
    // the new one. A tile seen once and never again is the least useful
    // entry to keep; a fresh miss starts level with it (count 1) and will
    // overtake it the next time it is seen too.
    auto worst = std::min_element(hits_.begin(), hits_.end(),
                                  [](const MissingTileHit& a, const MissingTileHit& b) { return a.count < b.count; });
    *worst = fresh;
    dirty_ = true;
    return;
  }

  hits_.push_back(fresh);
  dirty_ = true;
}

bool MissingTilesStore::markRefused(uint8_t z, uint32_t col, uint32_t row) {
  auto it = std::find_if(hits_.begin(), hits_.end(),
                         [&](const MissingTileHit& hit) { return hit.z == z && hit.col == col && hit.row == row; });
  if (it == hits_.end()) return false;
  it->refused = true;
  return true;
}

bool MissingTilesStore::isRefused(uint8_t z, uint32_t col, uint32_t row) const {
  auto it = std::find_if(hits_.begin(), hits_.end(),
                         [&](const MissingTileHit& hit) { return hit.z == z && hit.col == col && hit.row == row; });
  return it != hits_.end() && it->refused;
}

bool MissingTilesStore::forget(uint8_t z, uint32_t col, uint32_t row) {
  auto it = std::find_if(hits_.begin(), hits_.end(),
                         [&](const MissingTileHit& hit) { return hit.z == z && hit.col == col && hit.row == row; });
  if (it == hits_.end()) return false;

  // erase(), not swap-and-pop: the list is handed out in fetch-priority order
  // (sortByFetchPriority()) and swapping the last entry into this hole would
  // break that order outright. Shifting costs a memmove of at most 200 small
  // structs, on the arrival of a whole file.
  //
  // Either way a removal during an active listing shifts one entry back across
  // a page boundary the reader has already passed, so that entry is missed
  // from this listing. Harmless in the flow this is built for: the phone pages
  // the whole list first and only then starts pushing tiles, so nothing is
  // removed while it is still reading. A missed entry is re-listed by the next
  // fetch anyway -- the tile is still absent and the map still hatches it.
  hits_.erase(it);
  dirty_ = true;
  return true;
}

void MissingTilesStore::sortByFetchPriority() {
  // std::sort, not stable_sort: the comparator is a total order (it falls
  // through to col/row), so stability buys nothing -- and stable_sort would
  // allocate a scratch buffer on a heap this firmware guards closely.
  // At most kMaxEntries = 200 elements, and only when a listing starts.
  std::sort(hits_.begin(), hits_.end(),
            [](const MissingTileHit& a, const MissingTileHit& b) { return missingTileFetchBefore(a, b); });
}

bool MissingTilesStore::flushIfDirty() {
  if (!dirty_) return true;
  if (!saveToFile()) {
    LOG_ERR(kLogTag, "failed to persist missing tile list");
    return false;
  }
  dirty_ = false;
  return true;
}
