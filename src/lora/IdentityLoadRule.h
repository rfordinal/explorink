#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <stddef.h>

// What to do about the identity file on the card, as a decision separated from
// the card itself.
//
// **It is here, pure and testable, because getting it wrong destroys something
// that cannot be recreated.** The private key exists in exactly one place; a
// rule that generates a new one at the wrong moment hands the rider's group a
// stranger and there is no undo. The I/O around it is ordinary and the branch
// table is not, so the branch table is the part that gets a host test
// (test/lora_identity/).
//
// The failure this encodes was real: the first version asked only "did the
// open succeed", which made a busy card indistinguishable from an empty one.
// The card is busiest on the map, which since 2026-10-21 is the screen that
// triggers the load.
enum class IdentityLoadAction {
  UseExisting,  // the file is there and the right length -- read it, never write
  Create,       // there is genuinely no file -- this is the one path that makes a key
  Refuse,       // something is there and we could not read it whole -- stay down
};

// `exists` and `opened` are separate on purpose: a file that exists and will
// not open is the dangerous case, and merging them is what caused the defect.
// `bytesRead` is only consulted when the file opened.
inline IdentityLoadAction identityLoadAction(bool exists, bool opened, int bytesRead, size_t expectedBytes) {
  if (!exists) return IdentityLoadAction::Create;
  if (!opened) return IdentityLoadAction::Refuse;
  if (bytesRead != static_cast<int>(expectedBytes)) return IdentityLoadAction::Refuse;
  return IdentityLoadAction::UseExisting;
}

#endif  // ENABLE_LORA_MESH
