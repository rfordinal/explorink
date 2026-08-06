#pragma once

#include <cstdint>

// Finds the routes on the card. `/trailink/trips/*.tir`, flat, one file per
// route (../../../docs/route-file-spec.md in the parent xteink repo).
//
// The name shown to the rider comes out of each file's header, not from its file
// name: renaming a file must not rename a route, and a route pushed over BLE is
// named by whoever built it. Reading it costs the header only -- about 100 bytes
// per row, because .tir checks its point array separately (MapRouteReader.h).
//
// A fixed cap and fixed-size entries, no std::string and no vector: this list is
// built once when the picker opens and is bounded by what a rider can scroll
// through anyway. Past kMaxRoutes the extras are ignored and the caller is told
// how many were dropped -- a truncated list that says so beats one that quietly
// hides a route.
namespace MapRouteStore {

inline constexpr const char* kTripsDir = "/trailink/trips";
inline constexpr const char* kExtension = ".tir";

// 24 rows is well past what fits on one screen and costs 2.8 KB while the picker
// is open. A card with more routes than this is a filing problem, not a use case.
inline constexpr uint32_t kMaxRoutes = 24;
// Longest file name accepted, nul included. `zahorie-2026-08.tir` is 20.
inline constexpr uint32_t kMaxFileNameBytes = 48;
// MapRouteReader::kMaxNameBytes plus the terminator.
inline constexpr uint32_t kMaxNameBytes = 65;

struct Entry {
  char fileName[kMaxFileNameBytes] = {};
  char name[kMaxNameBytes] = {};
  uint32_t pointCount = 0;
  // False when the header was refused -- bad magic, wrong format version, a
  // failed header crc. The row is still listed, because a route the rider
  // pushed and cannot see is worse than one shown as unreadable, and picking it
  // fails cleanly.
  bool valid = false;
};

// Fills `out` with up to `capacity` entries, sorted by file name so the order
// does not depend on the card's directory layout. Returns how many were
// written; `outFound` gets how many .tir files were actually there, which is
// larger when the cap truncated the list.
uint32_t list(Entry* out, uint32_t capacity, uint32_t& outFound);

// True when at least one .tir file exists. Cheaper than list() -- it stops at
// the first hit and opens nothing.
bool anyRoutes();

// Builds "/trailink/trips/<fileName>" into `out`. False if it would not fit.
bool buildPath(const char* fileName, char* out, uint32_t outCapacity);

}  // namespace MapRouteStore
