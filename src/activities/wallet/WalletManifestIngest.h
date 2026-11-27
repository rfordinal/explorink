#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "WalletAssetIngest.h"
#include "WalletCrypto.h"

// Sealing the **list of documents** on the device, as it arrives (T-2096).
//
// The last piece of the wallet that was still built with a key the phone holds.
// A board makes its own K now (T-2094) and seals every asset that arrives
// (T-2095); this is the manifest's half of the same move. The phone sends
// plaintext JSON, the device encrypts it into the `EWM1` envelope and swaps it
// into place.
//
// **The device never authors the JSON.** It encrypts a blob it was handed, and
// the one number it reads out of that blob -- `walletVersion` -- it reads with
// the parser the read path already uses. Authoring would hand the device the
// manifest format and a second buffer, on a device whose encrypted manifest
// already caps at 32 KB (../../../docs/wallet-crypto.md, "The manifest has to
// fit in RAM", and parent repo docs/wallet-provisioning.md, "Four things to
// specify", point 1).
//
// ## Why this one is buffered, when an asset is streamed
//
// AssetIngest encrypts each chunk where it lands and never holds the file.
// This one holds all of it, and the difference is not GCM versus CTR -- mbedtls
// streams GCM perfectly well (`mbedtls_gcm_update()`, pinned ESP-IDF
// 5.5.2.260206). It is that **the read path cannot stream.** GCM authenticates
// only once it has seen everything, so `feedEncryptedManifest()` loads the
// whole file into RAM and refuses anything above kMaxEncryptedManifestBytes.
//
// A streamed seal would therefore accept a manifest this same device can never
// open again: written successfully, confirmed to the phone, and unreadable for
// ever after. That is the worst failure this ladder can produce, so the size is
// refused **at `begin`** -- the frame carries `totalLen`, so the refusal costs
// no bytes on the wire -- and the bound is the read path's own constant rather
// than a second number next to it (WalletCrypto.h, kMaxManifestPlaintextBytes).
//
// ## What the CRC in the reply means
//
// The same weaker thing it means for an asset: the card holds ciphertext, so
// the number answered to the phone is the CRC of the **plaintext as it
// arrived**, not of the file on the card. "These bytes arrived whole and were
// written", not "the card holds these bytes" (WalletAssetIngest.h, and
// firmware docs/wallet-crypto.md).
//
// ## Header-only and callback-bound, same as the asset sealer
//
// The buffering, the envelope and the ordering of the end-of-transfer checks are
// arithmetic, and arithmetic can be host-tested. The device binds the seal to
// mbedtls, the RNG to the hardware and the inspection to ManifestParser; the
// host test binds them to OpenSSL and the same parser. Two independent
// primitives, one contract.

namespace wallet {

enum class ManifestIngestStatus : uint8_t {
  Ok = 0,       // bytes consumed, more expected
  Complete,     // sealed, and the sealed bytes are ready to be written
  TooBig,       // over the cap the read path enforces -- refused at begin
  NoBuffer,     // the caller's buffer is missing or too small for the seal
  NotWired,     // a callback the sealer cannot work without was not supplied
  BadLength,    // a declared length that cannot be a manifest
  NotJson,      // the body is not a wallet manifest this firmware can read
  CrcMismatch,  // the bytes received are not the bytes the sender declared
  Overrun,      // more bytes than declared
  SealFailed,   // the GCM primitive refused
  NotActive,    // fed without a begin()
};

// A short reason for the wire, so the phone's log says which gate shut. Same
// convention as ingestStatusReason(): stable strings, because a host script
// greps them.
inline const char* manifestIngestReason(ManifestIngestStatus status) {
  switch (status) {
    case ManifestIngestStatus::Ok:
    case ManifestIngestStatus::Complete:
      return "ok";
    case ManifestIngestStatus::TooBig:
      return "manifest too big";
    case ManifestIngestStatus::NoBuffer:
      return "oom";
    case ManifestIngestStatus::NotWired:
      // Not "oom": a phone told to retry an out-of-memory condition will, and
      // no amount of retrying fixes a caller that forgot a callback.
      return "sealer not wired";
    case ManifestIngestStatus::BadLength:
      return "manifest length";
    case ManifestIngestStatus::NotJson:
      return "not a manifest";
    case ManifestIngestStatus::CrcMismatch:
      return "crc mismatch";
    case ManifestIngestStatus::Overrun:
      return "overrun";
    case ManifestIngestStatus::SealFailed:
      return "seal failed";
    case ManifestIngestStatus::NotActive:
      return "no transfer";
  }
  return "not a manifest";
}

// Reads the arrived JSON well enough to answer two questions: is this a wallet
// manifest at all, and which `walletVersion` does it carry. The device binds
// this to a heap-allocated ManifestParser -- the same class the read path uses,
// so "is it a manifest" is answered by the code that will later have to open it
// rather than by a second opinion.
//
// False means the body is not a manifest this firmware can read, and nothing is
// sealed. `walletVersion` 0 means the manifest does not state one; see
// ManifestEnvelope::walletVersion for what that costs (a resend, never a wrong
// sync).
using ManifestInspect = bool (*)(void* ctx, const char* json, size_t len, uint32_t& walletVersion);

// A fresh 12-byte nonce per write. **Fresh is the whole requirement**: one key
// over two different plaintexts with one nonce breaks GCM outright, and this
// device writes the manifest again on every sync.
using RandomFill = void (*)(void* ctx, uint8_t* out, size_t len);

// AES-256-GCM over the buffer in place, tag out. The device binds this to
// gcmEncryptInPlace(); the host test binds it to OpenSSL.
using ManifestSeal = bool (*)(void* ctx, const uint8_t nonce[kGcmNonceLen], uint8_t* data, size_t len,
                              uint8_t tag[kGcmTagLen]);

// Buffers one arriving manifest, checks it, and seals it in one shot.
//
// One instance per transfer. It owns the buffer it was handed, so the wipe in
// its destructor can never run against memory somebody else already freed --
// the buffer holds every document title in the wallet.
class ManifestIngest {
 public:
  struct Callbacks {
    Crc32Update crc32 = nullptr;
    ManifestInspect inspect = nullptr;
    void* inspectCtx = nullptr;
    RandomFill random = nullptr;
    void* randomCtx = nullptr;
    ManifestSeal seal = nullptr;
    void* sealCtx = nullptr;
  };

  // `totalLen` is the plaintext JSON the sender declared; `declaredCrc` its
  // CRC-32. `buffer` must hold sealedManifestLen(totalLen) and is taken over.
  //
  // Returns the refusal rather than a bool, because every one of these has to
  // reach the phone as its own `ERR <reason>` -- "it would not start" is the
  // answer that sends somebody reading firmware logs.
  ManifestIngestStatus begin(uint32_t totalLen, uint32_t declaredCrc, std::unique_ptr<uint8_t[]> buffer,
                             size_t bufferLen, const Callbacks& cb) {
    // First, before any way out: a refused begin that left `active_` set would
    // let a caller that ignored the answer feed the next transfer into this
    // one's buffer. Same rule, same reason as AssetIngest::begin().
    active_ = false;
    sealedLen_ = 0;
    walletVersion_ = 0;
    // A previous run's state, before any way out: `openingChecked_` left true
    // would skip the `{` refusal for this transfer, and a buffer left behind
    // would keep the previous manifest's titles in RAM past the point the
    // caller believes it dropped them. Unreachable today (the receiver
    // allocates a fresh object per transfer) and one line to keep it so.
    openingChecked_ = false;
    if (buffer_) {
      secureWipe(buffer_.get(), bufferLen_);
      buffer_.reset();
    }
    bufferLen_ = 0;
    if (cb.crc32 == nullptr || cb.inspect == nullptr || cb.random == nullptr || cb.seal == nullptr) {
      return ManifestIngestStatus::NotWired;
    }
    if (totalLen == 0) return ManifestIngestStatus::BadLength;
    if (totalLen > kMaxManifestPlaintextBytes) return ManifestIngestStatus::TooBig;
    const size_t want = sealedManifestLen(totalLen);
    if (!buffer || bufferLen < want) return ManifestIngestStatus::NoBuffer;

    buffer_ = std::move(buffer);
    bufferLen_ = bufferLen;
    cb_ = cb;
    total_ = totalLen;
    declaredCrc_ = declaredCrc;
    received_ = 0;
    crc_ = 0;
    active_ = true;
    return ManifestIngestStatus::Ok;
  }

  // One arriving chunk, in order. Returns Complete on the chunk carrying the
  // last declared byte, and only after the CRC matched, the body inspected as a
  // manifest and the seal succeeded -- the caller writes the card on Complete,
  // so nothing that could still fail may be left past it.
  ManifestIngestStatus feed(const uint8_t* data, size_t len) {
    if (!active_) return ManifestIngestStatus::NotActive;
    if (data == nullptr) return endWith(ManifestIngestStatus::NotActive);
    if (len == 0) return ManifestIngestStatus::Ok;
    if (static_cast<uint64_t>(received_) + len > total_) return endWith(ManifestIngestStatus::Overrun);

    crc_ = cb_.crc32(crc_, data, len);
    std::memcpy(plaintext() + received_, data, len);
    received_ += static_cast<uint32_t>(len);

    // The cheapest refusal there is, taken as early as it can be: a manifest is
    // a JSON object, so the first non-blank byte is '{'. A sender pushing
    // something else -- a tile, an image, a truncated file -- is stopped on its
    // first chunk instead of after it has sent 30 KB that the inspection at the
    // end would reject anyway.
    if (!openingChecked_) {
      for (uint32_t at = 0; at < received_; ++at) {
        const char c = static_cast<char>(plaintext()[at]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        openingChecked_ = true;
        if (c != '{') return endWith(ManifestIngestStatus::NotJson);
        break;
      }
      // All blanks so far: say nothing yet, the next chunk decides. A file that
      // is blank to its end fails the inspection below.
    }

    if (received_ < total_) return ManifestIngestStatus::Ok;
    return endWith(finish());
  }

  // The sealed file: envelope, ciphertext, tag, contiguous and in that order.
  // Valid only after Complete.
  const uint8_t* sealed() const { return sealedLen_ > 0 ? buffer_.get() : nullptr; }
  size_t sealedLen() const { return sealedLen_; }
  // What the manifest said about itself, for the log and the doc. 0 means it
  // said nothing.
  uint32_t walletVersion() const { return walletVersion_; }
  // The CRC-32 of the plaintext, which is the number the sender is waiting for.
  uint32_t plaintextCrc() const { return crc_; }
  bool active() const { return active_; }

  // Drops the document plaintext. The buffer is wiped and kept, not freed: the
  // one caller that reaches here off its own task (MapTransferReceiver::abandon)
  // can be racing a feed() on the host task, and freeing under it would be a
  // use-after-free where overwriting is the same benign race the file close
  // next to it already is.
  void reset() {
    active_ = false;
    sealedLen_ = 0;
    received_ = 0;
    openingChecked_ = false;
    if (buffer_) secureWipe(buffer_.get(), bufferLen_);
  }

  // Wipes the buffer and gives the memory back, **without destroying this
  // object**. The install needs the 32 KB back before it reads the same file
  // off the card, and the object has to outlive that: a transfer is only over
  // once its caller clears `active_`, and until then the activity task may
  // enter abandon() and call reset() on this pointer. Freeing the object there
  // instead -- which is what the first version of this did -- is a
  // use-after-free that writes up to 32 KB of zeros over the heap, found in
  // review 2026-11-27.
  void releaseBuffer() {
    active_ = false;
    sealedLen_ = 0;
    received_ = 0;
    openingChecked_ = false;
    if (buffer_) {
      secureWipe(buffer_.get(), bufferLen_);
      buffer_.reset();
    }
    bufferLen_ = 0;
  }

  // A transfer killed mid-stream -- Back pressed, link dropped -- never reaches
  // a terminal feed(), so this is the only wipe it gets.
  ~ManifestIngest() { reset(); }

 private:
  uint8_t* plaintext() { return buffer_.get() + kManifestEnvelopeLen; }

  // Everything that has to be true before the card is touched, in the order
  // that matters.
  ManifestIngestStatus finish() {
    // The bytes the sender declared, first: inspecting or sealing a corrupt
    // body wastes the work and could seal half a document list.
    if (crc_ != declaredCrc_) return ManifestIngestStatus::CrcMismatch;

    // Then: is it a manifest? Asked with the read path's own parser, so a body
    // that seals here is a body that opens later.
    uint32_t version = 0;
    if (!cb_.inspect(cb_.inspectCtx, reinterpret_cast<const char*>(plaintext()), total_, version)) {
      return ManifestIngestStatus::NotJson;
    }
    walletVersion_ = version;

    // Fresh nonce, into the envelope and into the seal from the same bytes --
    // written once and read twice, rather than generated twice.
    uint8_t nonce[kGcmNonceLen] = {0};
    cb_.random(cb_.randomCtx, nonce, sizeof(nonce));
    if (!buildManifestEnvelope(nonce, total_, walletVersion_, buffer_.get(), bufferLen_)) {
      return ManifestIngestStatus::SealFailed;
    }
    // In place: the plaintext becomes the ciphertext between the envelope and
    // the tag, so the file is one buffer and one write.
    if (!cb_.seal(cb_.sealCtx, nonce, plaintext(), total_, buffer_.get() + kManifestEnvelopeLen + total_)) {
      return ManifestIngestStatus::SealFailed;
    }
    sealedLen_ = sealedManifestLen(total_);
    return ManifestIngestStatus::Complete;
  }

  // Every exit but "more bytes please" goes through here -- including Complete,
  // where `active_` must drop but the sealed bytes must survive for the caller
  // to write.
  ManifestIngestStatus endWith(ManifestIngestStatus status) {
    if (status == ManifestIngestStatus::Complete) {
      active_ = false;
      openingChecked_ = false;
      return status;
    }
    reset();
    return status;
  }

  Callbacks cb_;
  std::unique_ptr<uint8_t[]> buffer_;
  size_t bufferLen_ = 0;
  size_t sealedLen_ = 0;
  uint32_t total_ = 0;
  uint32_t declaredCrc_ = 0;
  uint32_t received_ = 0;
  uint32_t crc_ = 0;
  uint32_t walletVersion_ = 0;
  bool openingChecked_ = false;
  bool active_ = false;
};

// True for the one path this receiver seals as a manifest:
// `<wallet dir>/manifest.json`, the name the phone and the generator both use
// for the cleartext list (parent repo docs/wallet-format.md section 3).
//
// **Case-insensitive, for the same reason pathIsWalletAsset() is**: SdFat
// compares names folded (`FatFile::cmpName()`), so `MANIFEST.JSON` is the same
// directory entry. An exact-case gate here would be the same hole the asset
// gate had in review on 2026-11-27 -- push `wallet/MANIFEST.JSON`, have it miss
// this gate, land verbatim as cleartext, and sit on the card as a readable list
// of every document title.
//
// The other spellings of one name -- a `.` component, a trailing dot or space
// FAT would strip -- are rejected before this is ever called
// (`MapTransferReceiver::relPathIsSafe()`).
inline bool pathIsWalletManifest(const char* fullPath) {
  if (fullPath == nullptr) return false;
  const size_t dirLen = std::strlen(kWalletDir);
  const char* kName = "manifest.json";
  const size_t nameLen = std::strlen(kName);
  if (std::strlen(fullPath) != dirLen + 1 + nameLen) return false;
  if (!asciiEqualFold(fullPath, kWalletDir, dirLen)) return false;
  if (fullPath[dirLen] != '/') return false;
  return asciiEqualFold(fullPath + dirLen + 1, kName, nameLen);
}

}  // namespace wallet
