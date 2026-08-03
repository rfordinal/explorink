#include "MapStyleModes.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

namespace {

constexpr const char* kLogTag = "MAPSTYLE";

// mapstyle.json is about 6 KB today and the `modes` block about 0.6 KB of it.
// The cap is a sanity bound on a file that could be anything, not a budget:
// past it we keep the built-in masks rather than allocate an unbounded buffer
// on a 380 KB device.
constexpr size_t kMaxStyleBytes = 32 * 1024;

}  // namespace

bool loadMapModeMasks(const char* path, MapModeMasks& out) {
  if (!path || path[0] == '\0') return false;

  HalFile file;
  if (!Storage.openFileForRead(kLogTag, path, file)) {
    // Expected on a card built before this existed. Built-in masks stand.
    LOG_INF(kLogTag, "%s not on the card, using built-in mode masks", path);
    return false;
  }

  const size_t size = file.fileSize();
  if (size == 0 || size > kMaxStyleBytes) {
    LOG_ERR(kLogTag, "%s is %u bytes, outside 1..%u -- keeping built-in mode masks", path, static_cast<unsigned>(size),
            static_cast<unsigned>(kMaxStyleBytes));
    return false;
  }

  // Read whole, then parse in place: ArduinoJson's zero-copy path over a
  // mutable char buffer means the only lasting cost is the filtered document,
  // and this buffer is gone before the tile source is ever asked for a way.
  auto buffer = makeUniqueNoThrow<char[]>(size + 1);
  if (!buffer) {
    LOG_ERR(kLogTag, "OOM: %u bytes for %s", static_cast<unsigned>(size + 1), path);
    return false;
  }
  const int read = file.read(buffer.get(), size);
  if (read <= 0 || static_cast<size_t>(read) != size) {
    LOG_ERR(kLogTag, "short read on %s: %d of %u bytes", path, read, static_cast<unsigned>(size));
    return false;
  }
  buffer[size] = '\0';

  // Filtered deserialize: everything outside `modes` is skipped by the
  // parser rather than built and thrown away. The style file is mostly layer
  // rules nothing here reads, and on this device that difference is a real
  // one -- roughly 0.6 KB of document instead of 6 KB.
  JsonDocument filter;
  filter["modes"] = true;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, buffer.get(), size, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR(kLogTag, "JSON parse error in %s: %s -- keeping built-in mode masks", path, error.c_str());
    return false;
  }

  JsonObjectConst modes = doc["modes"].as<JsonObjectConst>();
  if (modes.isNull()) {
    LOG_INF(kLogTag, "%s carries no `modes` block, using built-in mode masks", path);
    return false;
  }

  MapModeMasks loaded = out;
  bool anyApplied = false;
  for (JsonPairConst entry : modes) {
    MapRideMode mode;
    if (!mapRideModeFromName(entry.key().c_str(), mode)) {
      LOG_ERR(kLogTag, "unknown mode '%s' in %s, ignored", entry.key().c_str(), path);
      continue;
    }
    JsonArrayConst classes = entry.value()["classes"].as<JsonArrayConst>();
    if (classes.isNull()) {
      LOG_ERR(kLogTag, "mode '%s' in %s has no `classes` list, ignored", entry.key().c_str(), path);
      continue;
    }

    uint32_t mask = 0;
    for (JsonVariantConst name : classes) {
      const char* text = name.as<const char*>();
      uint8_t classId = 0;
      if (text == nullptr || !mapClassIdFromName(text, classId)) {
        // A name the tile format does not have. Ignored rather than fatal:
        // it can only ever mean "this class is not drawn", and the rest of
        // the list is still good.
        LOG_ERR(kLogTag, "mode '%s': unknown class '%s', ignored", entry.key().c_str(), text ? text : "(not a string)");
        continue;
      }
      mask |= (1u << classId);
    }

    if (mask == 0) {
      // An empty mask draws no roads at all. That is never what a style file
      // means to say, so treat it as a broken entry and keep the default.
      LOG_ERR(kLogTag, "mode '%s' in %s resolves to an empty class mask, ignored", entry.key().c_str(), path);
      continue;
    }

    loaded.setMode(mode, mask);
    anyApplied = true;
    LOG_DBG(kLogTag, "mode %s mask 0x%08lx", mapRideModeName(mode), static_cast<unsigned long>(mask));
  }

  if (!anyApplied) return false;
  out = loaded;
  return true;
}
