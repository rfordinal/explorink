#include "MapRouteStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "HalFileSource.h"
#include "MapRouteReader.h"

namespace {

constexpr const char* kLogTag = "ROUTE";

bool hasRouteExtension(const char* name) {
  const size_t len = std::strlen(name);
  const size_t extLen = std::strlen(MapRouteStore::kExtension);
  if (len <= extLen) return false;
  return std::strcmp(name + (len - extLen), MapRouteStore::kExtension) == 0;
}

// Sorted by file name so two cards with the same routes list them in the same
// order. Insertion sort: the list is capped at kMaxRoutes and this runs once,
// when the picker opens.
void insertSorted(MapRouteStore::Entry* out, uint32_t count, const MapRouteStore::Entry& entry) {
  uint32_t at = 0;
  while (at < count && std::strcmp(out[at].fileName, entry.fileName) < 0) ++at;
  for (uint32_t i = count; i > at; --i) out[i] = out[i - 1];
  out[at] = entry;
}

}  // namespace

bool MapRouteStore::buildPath(const char* fileName, char* out, uint32_t outCapacity) {
  if (fileName == nullptr || out == nullptr) return false;
  const size_t dirLen = std::strlen(kTripsDir);
  const size_t nameLen = std::strlen(fileName);
  if (dirLen + 1 + nameLen + 1 > outCapacity) return false;
  std::memcpy(out, kTripsDir, dirLen);
  out[dirLen] = '/';
  std::memcpy(out + dirLen + 1, fileName, nameLen + 1);
  return true;
}

bool MapRouteStore::anyRoutes() {
  auto dir = Storage.open(kTripsDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  bool found = false;
  char name[kMaxFileNameBytes];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDir = file.isDirectory();
    if (!isDir) {
      file.getName(name, sizeof(name));
      if (name[0] != '.' && hasRouteExtension(name)) found = true;
    }
    file.close();
    if (found) break;
  }
  dir.close();
  return found;
}

uint32_t MapRouteStore::list(Entry* out, uint32_t capacity, uint32_t& outFound) {
  outFound = 0;
  if (out == nullptr || capacity == 0) return 0;

  auto dir = Storage.open(kTripsDir);
  if (!dir || !dir.isDirectory()) {
    // Not an error. A card that has never had a route pushed to it has no trips
    // directory, and the picker simply has nothing but Skip on it.
    if (dir) dir.close();
    return 0;
  }

  uint32_t count = 0;
  // Long enough for any name the loop below accepts; a longer one is skipped
  // rather than truncated, because a truncated name opens the wrong file.
  char name[kMaxFileNameBytes + 1];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDir = file.isDirectory();
    if (isDir) {
      file.close();
      continue;
    }
    file.getName(name, sizeof(name));
    file.close();

    if (name[0] == '.' || !hasRouteExtension(name)) continue;
    if (std::strlen(name) >= kMaxFileNameBytes) {
      LOG_ERR(kLogTag, "route file name too long, skipped: %s", name);
      continue;
    }
    ++outFound;
    if (count >= capacity) continue;

    Entry entry;
    std::memcpy(entry.fileName, name, std::strlen(name) + 1);

    // The header only. A route's name and length cost about 100 bytes, so a
    // full listing is cheap even with two dozen routes on the card -- the point
    // array is checked later, by whatever is about to draw it.
    char path[kMaxFileNameBytes + 32];
    if (buildPath(name, path, sizeof(path))) {
      HalFileSource source;
      MapRouteReader reader;
      if (reader.open(source, path)) {
        entry.valid = true;
        entry.pointCount = reader.pointCount();
        const char* routeName = reader.name();
        std::memcpy(entry.name, routeName, std::strlen(routeName) + 1);
        reader.close();
      } else {
        LOG_ERR(kLogTag, "route header refused: %s", path);
      }
    }
    // A route with no name in its header falls back to its file name, so a row
    // is never blank.
    if (entry.name[0] == '\0') std::memcpy(entry.name, entry.fileName, std::strlen(entry.fileName) + 1);

    insertSorted(out, count, entry);
    ++count;
  }
  dir.close();

  if (outFound > count) {
    LOG_ERR(kLogTag, "%lu routes on the card, listing the first %lu", static_cast<unsigned long>(outFound),
            static_cast<unsigned long>(count));
  }
  return count;
}
