#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <stddef.h>
#include <stdint.h>

// Who else is on the air, and when we last heard them.
//
// **This is not the roster.** The roster (MapTeam) is an allowlist: it decides
// who may put a marker on the panel, and a stranger is refused there by design
// (docs/team-markers.md, "A stranger is not a member"). This table is the
// opposite question -- *every* node whose advert reached this antenna, known or
// not -- because "can the two boards hear each other" cannot be answered by a
// list that hides the half we have not approved yet.
//
// Keeping them separate is what lets both stay honest: a stranger shows up here
// and still never reaches the map.
//
// Plain C++ on purpose, no Arduino and no radio: every rule below is arithmetic
// and is proved by test/lora/ on the host rather than by a board.

struct LoraPeer {
  // The Ed25519 public key the advert was signed with. The whole key, not a
  // prefix: it is both the identity a roster entry matches against and the key
  // that verified the signature, and a prefix is a thing two nodes can collide
  // on. MeshCore's own path hashes are one byte, so collisions there are
  // ordinary and expected -- this table must not inherit that.
  uint8_t pubKey[32];

  // The advert's name field, empty when the sender published none. MeshCore
  // caps advert data at 32 bytes including the flags, the coordinates and the
  // terminator, so a name cannot reach that length in practice.
  char name[21];

  int32_t latE6;  // degrees * 1e6, about 11 cm. Only meaningful when hasLoc.
  int32_t lonE6;
  bool hasLoc;

  // The timestamp the sender put in its own advert, in unix seconds. Their
  // clock, not ours -- a node with no fix and no RTC sends a number that means
  // nothing, which is why heardUptimeMs exists beside it.
  uint32_t advertUtc;

  // Our own millis() when the advert landed, and the only age this device can
  // compute without trusting anybody. Same reason MapTeam stamps its own clock
  // on an incoming position.
  uint32_t heardUptimeMs;

  int16_t rssiDbm;  // of the last advert heard from this node
  int8_t snrDb;
  uint16_t adverts;  // how many we have heard since boot
};

class LoraPeerTable {
 public:
  // Sixteen, which is more riders than a group that can stay together on a
  // trail and still well inside what the panel can list. Fixed array, no heap:
  // the thing filling it is a radio anyone in range can talk to, so a table
  // that grows on demand is a table a stranger can grow (CLAUDE.md,
  // "Security: think about a stolen or physically-accessed device").
  static constexpr size_t kMaxPeers = 16;

  // Records one advert. Returns true when this was a node we had not heard
  // before, so the caller can log a new neighbour without logging every repeat.
  //
  // A full table evicts the node heard longest ago. It never evicts to make
  // room for a node already in the table, because that path is an update.
  bool heard(const uint8_t pubKey[32], const char* name, bool hasLoc, int32_t latE6, int32_t lonE6, uint32_t advertUtc,
             int16_t rssiDbm, int8_t snrDb, uint32_t nowUptimeMs);

  size_t count() const { return count_; }

  // Listing order is newest heard first, which is the order the question "who
  // is out there right now" wants. Only called with index < count().
  const LoraPeer& at(size_t index) const;

  // Index of a node by its key, or -1. The seam the roster uses: a member's id
  // is a MeshCore key, so this is how a heard node becomes a map marker.
  int find(const uint8_t pubKey[32]) const;

  void clear() { count_ = 0; }

 private:
  LoraPeer peers_[kMaxPeers];
  size_t count_ = 0;
  // Listing order, newest-heard first. Held as indices into peers_ so an
  // update does not have to move a 76-byte record.
  uint8_t order_[kMaxPeers];

  // Moves `slot` to the head of the listing order. `orderLen` is how many
  // entries of order_ are valid *before* this call -- one less than count_ on
  // the path that just appended a slot, and count_ on every other path.
  // Passed rather than derived because order_ beyond orderLen is uninitialised
  // memory, and a search that reads it can match by accident.
  void promote(size_t slot, size_t orderLen);
};

#endif  // ENABLE_LORA_MESH
