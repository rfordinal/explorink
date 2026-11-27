#include "WalletManifestInstall.h"

#include <HalStorage.h>
#include <Logging.h>

#include "WalletCryptoDevice.h"
#include "WalletStore.h"

namespace wallet {

namespace {

constexpr const char* kLogTag = "WALLETMAN";

// The card is written in bites rather than in one call. SdFat takes a 32 KB
// write happily enough; what the loop buys is the **offset** in the log when a
// write fails, which a single call cannot give. It does not distinguish a short
// write from a failed one -- both end the same way, as `ERR write failed` -- and
// an earlier version of this comment claimed it did (review, 2026-11-27).
constexpr size_t kWriteChunk = 1024;

bool writePart(const uint8_t* sealed, const size_t len) {
  HalFile file;
  if (!Storage.openFileForWrite(kLogTag, kManifestPartPath, file)) {
    LOG_ERR(kLogTag, "cannot open %s for write", kManifestPartPath);
    return false;
  }
  for (size_t at = 0; at < len;) {
    const size_t take = (len - at) < kWriteChunk ? (len - at) : kWriteChunk;
    if (file.write(sealed + at, take) != take) {
      LOG_ERR(kLogTag, "short write at %u of %u", static_cast<unsigned>(at), static_cast<unsigned>(len));
      // Closed before the caller removes it: the same handle cannot be open
      // while the path is deleted (firmware CLAUDE.md, DESTRUCTOR_CLOSES_FILE).
      file.close();
      return false;
    }
    at += take;
  }
  file.close();
  return true;
}

}  // namespace

const char* manifestInstallReason(const ManifestInstallStatus status) {
  switch (status) {
    case ManifestInstallStatus::Ok:
      return "ok";
    case ManifestInstallStatus::WriteFailed:
      return "write failed";
    case ManifestInstallStatus::Unreadable:
      return "manifest unreadable";
    case ManifestInstallStatus::Locked:
      return "wallet locked";
    case ManifestInstallStatus::NoMemory:
      // **Not "unreadable", and not "manifest too big" either.** The file is
      // fine; this board had no heap to read it back. "Unreadable" would have
      // the phone re-send the same bytes for ever, and `manifest too big` is
      // already the begin-frame refusal for a manifest over the format's cap --
      // two different facts needing two different actions, which they did share
      // until a review caught it (2026-11-27). This is the same heap failure
      // the ingest allocation reports, so it says the same word. Measured on an
      // X3 with BLE up: 20,480 bytes installed and 22,528 did not, nowhere near
      // the 32 KB the format allows.
      return "oom";
    case ManifestInstallStatus::SwapFailed:
      return "swap failed";
  }
  return "manifest unreadable";
}

ManifestInstallStatus writeSealedManifestPart(const uint8_t* sealed, const size_t len) {
  if (sealed == nullptr || len == 0) return ManifestInstallStatus::WriteFailed;
  if (!writePart(sealed, len)) {
    Storage.remove(kManifestPartPath);
    return ManifestInstallStatus::WriteFailed;
  }
  return ManifestInstallStatus::Ok;
}

ManifestInstallStatus finishManifestInstall() {
  // The read path, on the file that is about to become the list. Not a second
  // implementation and not a re-decrypt of the buffer still in RAM: what has to
  // be proved is that **the card** holds a readable list, and only a read off
  // the card proves that.
  Error error = Error::None;
  if (!verifyEncryptedManifestFile(kManifestPartPath, error)) {
    // Three different facts, three different answers for the phone. "Too big"
    // and "locked" are both true statements about this board rather than about
    // the file, and a phone told "unreadable" re-sends the same bytes for ever.
    ManifestInstallStatus status = ManifestInstallStatus::Unreadable;
    const char* why = "unreadable";
    if (error == Error::ManifestTooBig) {
      status = ManifestInstallStatus::NoMemory;
      why = "no memory to read it back";
    } else if (error == Error::Locked || !Session::instance().hasKey()) {
      // The second half of that test is not belt and braces. `Error::Locked`
      // only comes out when the key was already gone at the top of the read; a
      // lock landing a moment later, **inside** the GCM decrypt, surfaces as a
      // tag mismatch -- which would otherwise be reported as "wrong key, or the
      // wallet list was altered" about a file this device had just written
      // correctly. If the session has no key by the time we are standing here,
      // that is the explanation, whatever the read called it (review,
      // 2026-11-27).
      status = ManifestInstallStatus::Locked;
      why = "the wallet locked while it was being written";
    }
    LOG_ERR(kLogTag, "the sealed manifest did not open (%s) -- keeping the old list", why);
    Storage.remove(kManifestPartPath);
    return status;
  }

  // From here the card is being changed. Every failure below leaves a state the
  // read path can still open -- that is what the backup is for.
  if (Storage.exists(kManifestEncPath)) {
    // SdFat's rename fails onto an existing name, so the previous backup goes
    // first. Losing it costs nothing: it is a copy of a list that is itself
    // about to be replaced by one already proven readable.
    if (Storage.exists(kManifestBakPath) && !Storage.remove(kManifestBakPath)) {
      LOG_ERR(kLogTag, "cannot remove the old %s", kManifestBakPath);
      Storage.remove(kManifestPartPath);
      return ManifestInstallStatus::SwapFailed;
    }
    if (!Storage.rename(kManifestEncPath, kManifestBakPath)) {
      LOG_ERR(kLogTag, "cannot move the live manifest aside");
      Storage.remove(kManifestPartPath);
      return ManifestInstallStatus::SwapFailed;
    }
  }

  if (!Storage.rename(kManifestPartPath, kManifestEncPath)) {
    // The window this comment is about is open right now: the live name does
    // not exist and the backup does. The read path reads the backup, so the
    // wallet still opens with the previous list, and the next sync repairs it.
    LOG_ERR(kLogTag, "cannot rename the sealed manifest into place -- the card is on the backup list");
    // Removed here too, so this is not the one failure branch that leaves a
    // `.part` on the card indefinitely. The next install would truncate it
    // anyway; a card that quietly carries a half-finished file is still worse
    // than one that does not (review, 2026-11-27).
    Storage.remove(kManifestPartPath);
    return ManifestInstallStatus::SwapFailed;
  }

  // A cleartext list next to an encrypted wallet is a readable index of every
  // document the rider carries. Removed after the swap, never before: until the
  // rename above succeeded, it was one of the two things that could still
  // answer "what is on this card".
  if (Storage.exists(kManifestPath) && !Storage.remove(kManifestPath)) {
    // Not a failed install -- the new list is live and the wallet works. Loud,
    // because the card is carrying a plaintext title list until somebody
    // deletes it.
    LOG_ERR(kLogTag, "the stale cleartext %s is still on the card", kManifestPath);
  }

  LOG_INF(kLogTag, "sealed manifest installed");
  return ManifestInstallStatus::Ok;
}

}  // namespace wallet
