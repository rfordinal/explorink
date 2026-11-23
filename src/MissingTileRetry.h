#pragma once

#include <cstdint>

// The two decisions behind MissingTilesStore::msUntilNextRetryIn(), pulled out
// so they can be tested without the store.
//
// Same reason MissingTilePriority.h exists: MissingTilesStore.h includes
// ArduinoJson and the SD-backed PersistableStore, so a host test that wants the
// policy would have to drag the card with it. The store includes this header
// and keeps the vector walk; everything decided per tile lives here.

// Whether one recorded tile sits inside the rectangle a frame drew. The zoom
// level has to match exactly: the same ground is a different tile at every
// rung, and a z13 refusal says nothing about the z11 parent on screen.
inline bool missingTileRetryInRange(uint8_t tileZ, uint32_t tileCol, uint32_t tileRow, uint8_t z, uint32_t col0,
                                    uint32_t row0, uint32_t col1, uint32_t row1) {
  if (tileZ != z) return false;
  if (tileCol < col0 || tileCol > col1) return false;
  if (tileRow < row0 || tileRow > row1) return false;
  return true;
}

// The longest delay the refusal schedule can ever produce
// (MissingTilesStore::kRefusalMaxMs, which is defined as this). It is also what
// makes the wrap below decidable: a stored deadline is always `now + delay`
// with `delay <= ceiling`, so a remaining time ABOVE the ceiling is not a
// future a schedule could have produced.
inline constexpr uint32_t kMissingTileRetryCeilingMs = 60u * 60u * 1000u;

// How long until `retryAtMs`, or 0 when nothing is pending.
//
// `retryAtMs == 0` means never refused -- the store uses 0 as "no schedule",
// which costs one millisecond of resolution every 49.7 days and is why
// markRefused() steps off a wrapped 0.
//
// The subtraction is signed on purpose. millis() wraps, and a plain
// `nowMs < retryAtMs` reads a wrapped clock as "another 49 days to wait"
// (MissingTilesStore::isRefused() has the same form for the same reason).
//
// **And the signed form alone is not enough**, which a host test caught after
// the panel had already been patched downstream: a deadline stamped early in
// the run reads as +2,135,057,296 ms once uptime passes ~24.8 days, because
// the difference aliases back into the positive half. The map printed that as
// "auto retry in 35584 min". Capping the *source* at the ceiling resolves it
// where the knowledge is -- no schedule can be further out than the ceiling,
// so anything beyond it is an expired deadline the clock has lapped. Within
// 24.8 days of uptime nothing else changes.
inline uint32_t missingTileRetryRemainingMs(uint32_t retryAtMs, uint32_t nowMs) {
  if (retryAtMs == 0) return 0;
  const int32_t remaining = static_cast<int32_t>(retryAtMs - nowMs);
  if (remaining <= 0) return 0;
  const uint32_t remainingMs = static_cast<uint32_t>(remaining);
  if (remainingMs > kMissingTileRetryCeilingMs) return 0;
  return remainingMs;
}
