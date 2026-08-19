#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// The pin catalogue: one row per pin type the device offers.
//
// Pure: no Arduino, no I18n, no HAL. The user-facing label is deliberately
// *not* here -- a StrId would pull in the generated I18nKeys.h, which the host
// tests do not build (test/CMakeLists.txt generates only the mode mask and the
// style defaults). PinLabels.h maps a catalogue index to its StrId on the
// device side; the ASCII name below is what a console reply and a log dump use.
//
// Soft table, hard log (docs/pins-plan.md, decision 5): this table is expected
// to change -- rows added, reordered, relabelled. The log stores `key`, never an
// index into this array, so an inserted row cannot change what an old record
// means.

// Longest catalogue key is "parking" (7). The extra room is for a key written by
// a *later* firmware that this build has to load, print and delete without
// understanding it (the unknown-key rule below).
inline constexpr size_t kPinKeyBytes = 12;

struct PinType {
  const char* key;   // stable ASCII, in the log forever
  const char* name;  // ASCII fallback for console replies; UI uses PinLabels.h
};

// 6 named slots plus #1-#5. Raising this is this constant plus catalogue rows.
inline constexpr size_t kPinSlotCount = 11;

// static constexpr, so the table lands in flash and not DRAM (CLAUDE.md,
// Resource Protocol 6).
inline constexpr PinType kPinCatalog[kPinSlotCount] = {
    {"base", "Base"}, {"parking", "Parking"}, {"dest", "Destination"},
    {"meet", "Meet"}, {"camp", "Camp"},       {"favorite", "Favorite"},
    {"c1", "#1"},     {"c2", "#2"},           {"c3", "#3"},
    {"c4", "#4"},     {"c5", "#5"},
};

// Slots held for keys this build does not know. A record written by a newer
// firmware still loads into one of these: generic label, raw key, deletable.
// Never silently dropped -- an update that eats a rider's camp is the exact
// failure the log exists to prevent (docs/pins-plan.md, "Unknown key rule").
inline constexpr size_t kPinUnknownSlots = 4;
inline constexpr size_t kPinMaxEntries = kPinSlotCount + kPinUnknownSlots;

// Returned by pinCatalogIndex() for a key that is not in the table.
inline constexpr size_t kPinIndexUnknown = kPinSlotCount;

// Index of `key` in the catalogue, or kPinIndexUnknown. Case sensitive: the key
// in the log is the key in the table, byte for byte.
inline constexpr size_t pinCatalogIndex(std::string_view key) {
  for (size_t i = 0; i < kPinSlotCount; ++i) {
    if (key == std::string_view(kPinCatalog[i].key)) return i;
  }
  return kPinIndexUnknown;
}

// A key is storable if it is non-empty, fits the field, and carries no byte that
// would break the log's own framing or a line-based console reply.
inline constexpr bool isValidPinKey(std::string_view key) {
  if (key.empty() || key.size() >= kPinKeyBytes) return false;
  for (const char c : key) {
    if (c == '|' || c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == ',') return false;
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
  }
  return true;
}
