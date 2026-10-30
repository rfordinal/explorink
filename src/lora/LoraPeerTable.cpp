#include "LoraPeerTable.h"

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <string.h>

namespace {

// Oldest-heard first, by our own clock. millis() wraps after 49 days and the
// subtraction below is what survives that: the difference of two unsigned
// values is correct across the wrap, an ordinary `<` comparison is not.
size_t oldestSlot(const LoraPeer* peers, size_t count, uint32_t nowUptimeMs) {
  size_t worst = 0;
  uint32_t worstAge = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t age = nowUptimeMs - peers[i].heardUptimeMs;
    if (age >= worstAge) {
      worstAge = age;
      worst = i;
    }
  }
  return worst;
}

}  // namespace

int LoraPeerTable::find(const uint8_t pubKey[32]) const {
  for (size_t i = 0; i < count_; ++i) {
    if (memcmp(peers_[i].pubKey, pubKey, 32) == 0) return static_cast<int>(i);
  }
  return -1;
}

void LoraPeerTable::promote(size_t slot, size_t orderLen) {
  // Move `slot` to the head of the listing order, shifting the rest down. The
  // table is sixteen entries, so a memmove beats a heap or a linked list and
  // costs nothing a radio can notice.
  size_t at = 0;
  while (at < orderLen && order_[at] != slot) ++at;
  if (at == orderLen) {
    // Not in the order yet -- a slot that was just appended.
    if (orderLen > 0) memmove(&order_[1], &order_[0], orderLen);
    order_[0] = static_cast<uint8_t>(slot);
    return;
  }
  if (at == 0) return;
  memmove(&order_[1], &order_[0], at);
  order_[0] = static_cast<uint8_t>(slot);
}

bool LoraPeerTable::heard(const uint8_t pubKey[32], const char* name, bool hasLoc, int32_t latE6, int32_t lonE6,
                          uint32_t advertUtc, int16_t rssiDbm, int8_t snrDb, uint32_t nowUptimeMs) {
  int at = find(pubKey);
  const bool isNew = at < 0;
  size_t orderLen = count_;

  if (isNew) {
    size_t slot;
    if (count_ < kMaxPeers) {
      slot = count_++;  // orderLen stays at the old count: order_ has no entry for this slot yet
    } else {
      slot = oldestSlot(peers_, count_, nowUptimeMs);
    }
    memset(&peers_[slot], 0, sizeof(peers_[slot]));
    memcpy(peers_[slot].pubKey, pubKey, 32);
    at = static_cast<int>(slot);
  }

  LoraPeer& p = peers_[static_cast<size_t>(at)];

  // A later advert with no name does not erase a name we already have. The
  // advert's name field is optional per packet, not per node (MeshCore
  // AdvertDataHelpers.h, ADV_NAME_MASK), so treating its absence as "they are
  // nameless now" would make a node flicker between its name and its key.
  if (name != nullptr && name[0] != '\0') {
    strncpy(p.name, name, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = '\0';
  }

  // Same rule for the coordinate, and it matters more: the difference between
  // "has not told us" and "is at 0,0" is one the whole group layer keeps
  // (docs/group-markers.md), and an advert sent without a fix must not blank a
  // position we were told five minutes ago.
  if (hasLoc) {
    p.latE6 = latE6;
    p.lonE6 = lonE6;
    p.hasLoc = true;
  }

  p.advertUtc = advertUtc;
  p.heardUptimeMs = nowUptimeMs;
  p.rssiDbm = rssiDbm;
  p.snrDb = snrDb;
  if (p.adverts < 0xFFFF) ++p.adverts;

  promote(static_cast<size_t>(at), orderLen);
  return isNew;
}

const LoraPeer& LoraPeerTable::at(size_t index) const { return peers_[order_[index]]; }

#endif  // ENABLE_LORA_MESH
