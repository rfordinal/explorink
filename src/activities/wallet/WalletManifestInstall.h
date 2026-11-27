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
  TooBig,       // it sealed and wrote, and this board had no memory to read it back
  SwapFailed,   // a rename the card refused -- the previous list is still live
};

// A short reason for the wire and the log, same convention as the ingest
// statuses: stable strings, because a host script greps them.
const char* manifestInstallReason(ManifestInstallStatus status);

// **Two calls, so the two 32 KB buffers are never alive at once.** The sender's
// buffer holds the sealed file; the verification reads the same file back off
// the card into a buffer of its own. Doing both inside one call means a peak of
// twice the manifest, and on a C3 with BLE up that is what actually happens:
// measured on an X3, 2026-11-27, an 8,234-byte manifest sealed, wrote, and then
// failed to verify with `OOM: 8234 bytes for the encrypted manifest` -- the
// install refused a list the board could have held, and the old one stayed.
//
// So the caller writes the part file, **drops its own buffer**, and only then
// asks for the verify and the swap.
//
// `sealed` is the whole file: envelope, ciphertext, tag.
ManifestInstallStatus writeSealedManifestPart(const uint8_t* sealed, size_t len);

// Proves the part file opens with the read path, then swaps it in and deletes a
// stale cleartext manifest. Needs the session key, because it decrypts; the
// caller already holds it, since a locked wallet never gets as far as sealing.
//
// Every failure here leaves a card the read path can still open.
ManifestInstallStatus finishManifestInstall();

}  // namespace wallet
