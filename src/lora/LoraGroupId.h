#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// How a MeshCore node is written down in the group roster.
//
// **The roster cannot hold the whole key, and that is a design fact rather
// than a shortcut.** A roster id is `kGroupIdBytes` = 24 bytes including the
// terminator (GroupMembers.h), and an Ed25519 public key is 64 hex characters.
// So membership is matched on a prefix: `lora-` plus the first eight bytes of
// the key, 21 characters.
//
// **Eight bytes is enough, and the reason is not collision odds.** An advert
// carries its own public key and MeshCore verifies the signature against that
// key, so an impostor cannot borrow a member's key -- they would have to
// *generate* a keypair whose public key starts with the same eight bytes,
// which is 2^64 work. Accidental collision between two nodes in one group is
// not a consideration at that width.
//
// It does mean the peer table and the roster match on different things: the
// table keys on the whole 32 bytes (LoraPeerTable.h) and the roster on this
// prefix. Both are correct for what they do, and this header exists so the
// prefix has exactly one spelling -- two places formatting it differently
// would make a member silently stop being recognised.
//
// `lora-` follows the roster's own convention for transport-tagged ids
// (docs/group-markers.md: `"ids":["lora-8f3a","ble-7f"]`), which is what lets
// one person be known by a LoRa key and a phone relay name at once.

inline constexpr size_t kLoraGroupIdBytes = 22;  // "lora-" + 16 hex + NUL

inline void loraGroupId(const uint8_t pubKey[32], char* buf, size_t bufLen) {
  snprintf(buf, bufLen, "lora-%02x%02x%02x%02x%02x%02x%02x%02x", pubKey[0], pubKey[1], pubKey[2], pubKey[3], pubKey[4],
           pubKey[5], pubKey[6], pubKey[7]);
}

// A first guess at the two or three letters that will sit on the map marker,
// taken from whatever the node calls itself.
//
// **A guess the rider is always shown and can always change.** An acronym is
// what a balloon on the panel carries for as long as that person is in the
// group, and it is derived from a string they chose for a different purpose --
// `SK-BA-Pribisova` is a place, not a person. Deriving saves the common case
// from a keyboard; offering the keyboard anyway is what keeps the uncommon
// case from being wrong forever.
//
// Alphanumerics only, uppercased, at most three. A node with no usable name
// falls back to the key, which is unreadable and unique -- better than empty,
// because the roster refuses an empty acronym and the rider would be left
// with a row that cannot be added at all.
inline void loraGroupAcrGuess(const char* name, const uint8_t pubKey[32], char* buf, size_t bufLen) {
  size_t out = 0;
  if (name != nullptr) {
    for (const char* c = name; *c != '\0' && out + 1 < bufLen && out < 3; ++c) {
      const bool alnum = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9');
      if (!alnum) continue;
      buf[out++] = (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 'a' + 'A') : *c;
    }
  }
  if (out == 0) {
    snprintf(buf, bufLen, "%02X%01X", pubKey[0], pubKey[1] >> 4);
    return;
  }
  buf[out] = '\0';
}

#endif  // ENABLE_LORA_MESH
