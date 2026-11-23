#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include "MapPointShards.h"
#include "MapPointSource.h"

// The two SD-card operations `points`/`gone` need on a point shard's file --
// existence and delete-if-present -- factored out of TileSyncActivity and
// MapActivity, which had byte-identical copies of both (code review,
// 2026-09-13). Header-only and inline, matching MapPointShards.h's own
// shape, but this one is not host-testable the way that file is: it touches
// HalStorage, and stays out of the native test suite for the same reason
// every other Storage-touching call in these activities does.
namespace MapPointShardStorage {

inline bool exists(const char* rootDir, uint32_t col, uint32_t row) {
  char path[MapPointSource::kMaxPathLen];
  if (!MapPointShards::buildPath(path, sizeof(path), rootDir, col, row)) return false;
  return Storage.exists(path);
}

// Deletes the shard if the card has it. Nothing to delete is not a failure:
// `gone` can arrive for a shard this card never had (the phone's own
// 404-twice guard races the device's own last request), and that is the same
// outcome as a delete that worked. [logTag] keeps the log line attributed to
// the caller's own activity rather than a shared one.
inline void deleteIfPresent(const char* logTag, const char* rootDir, uint32_t col, uint32_t row) {
  char path[MapPointSource::kMaxPathLen];
  if (!MapPointShards::buildPath(path, sizeof(path), rootDir, col, row)) return;
  if (!Storage.exists(path)) return;
  if (!Storage.remove(path)) {
    LOG_ERR(logTag, "gone: could not delete %s", path);
    return;
  }
  LOG_INF(logTag, "gone: deleted point shard %lu/%lu", static_cast<unsigned long>(col),
          static_cast<unsigned long>(row));
}

}  // namespace MapPointShardStorage
