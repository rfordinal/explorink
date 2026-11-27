#pragma once

#include <cstddef>
#include <cstdint>

// Putting a freshly sealed manifest on the card, without ever leaving the
// wallet without a list (T-2096).
//
// WalletStore is read-only by construction and stays that way -- this is the
// one place in the wallet that writes, renames or deletes on the card, so the
// rule "the card is the only offline copy" has exactly one file to be checked
// against.
//
// ## The order, and what each step is for
//
//   1. write    `manifest.enc.part`   -- nothing live is touched yet
//   2. verify    that file, with the read path itself (Store, not a second
//                opinion): envelope, GCM tag, JSON, formatVersion, one item
//   3. rename   `manifest.enc` -> `manifest.bak`
//   4. rename   `manifest.enc.part` -> `manifest.enc`
//   5. delete    a stale cleartext `manifest.json`, if the card still has one
//
// **Step 2 is the cheap proof nothing else gives.** The file is at most 32 KB
// and the key is in hand, so "the card now holds a list this board can open"
// can be answered before the old one is moved, rather than discovered by a
// rider at a border crossing.
//
// **Steps 3 and 4 are two renames because FAT has no atomic replace.** A power
// cut between them leaves a card with `manifest.bak` and no `manifest.enc`, and
// that is exactly why the read path falls back to the backup
// (WalletStore.cpp, encryptedManifestPath()). Without that fallback this
// sequence would have a window in which the wallet reads as empty, which is the
// failure the task was written to avoid.
//
// **Step 5 is not tidying.** The format says an encrypted tree carries no
// cleartext manifest (parent repo docs/wallet-format.md section 3), and leaving
// one is a readable list of every document title sitting next to an encrypted
// wallet -- on a card that gets lost with the bike.

namespace wallet {

enum class ManifestInstallStatus : uint8_t {
  Ok = 0,
  WriteFailed,  // the card refused the bytes, or took fewer than offered
  Unreadable,   // the written file did not open: card, or a sealer bug
  SwapFailed,   // a rename the card refused -- the previous list is still live
};

// A short reason for the wire and the log, same convention as the ingest
// statuses: stable strings, because a host script greps them.
const char* manifestInstallReason(ManifestInstallStatus status);

// Writes `sealed`, proves it opens, and swaps it in. `sealed` is the whole
// file: envelope, ciphertext, tag.
//
// Needs the session key, because step 2 decrypts. The caller already holds it
// -- a locked wallet never gets as far as sealing.
ManifestInstallStatus installSealedManifest(const uint8_t* sealed, size_t len);

}  // namespace wallet
