#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "WalletAsset.h"
#include "WalletCrypto.h"
#include "WalletSha256.h"

// Encrypting a wallet asset **as it arrives**, in one pass over the wire.
//
// The phone sends plaintext and the card holds ciphertext -- that is the whole
// point of T-2095. A board makes its own K now (T-2094), so nothing off the
// device can produce a tree this board can read, and the laptop stops being in
// the product path. What crosses the wire is therefore the readable bitmap, and
// the device is what seals it.
//
// ## Why one pass, and why that is possible at all
//
// The 32-byte header arrives first and **already carries the IV**:
// `sha256_prefix` is the first 8 bytes of the plaintext payload's hash, which
// is exactly what buildAssetIvFromHash() needs (WalletCrypto.h, "The IV follows
// the bytes"). So the IV is known before the first payload byte lands, and
// every later byte can be encrypted where it arrives. No staging file, no
// second write of 585 KB, no 48 KB buffer.
//
// The format is unchanged by this -- same 32-byte cleartext header, same
// AES-256-CTR payload (parent repo docs/wallet-format.md section 11). The
// device simply becomes a second writer of a format the generator already
// writes.
//
// ## The two checks that pay for trusting a prefix nobody verified
//
// 1. **Hash the plaintext as it streams** and compare with the header's prefix
//    at the end. Without this the device would build a keystream out of a
//    number it never checked, and a wrong prefix is not an error anywhere --
//    it is an asset that decrypts to noise on the panel for ever, with every
//    structural check passing. The hash is the only thing that can catch it,
//    and it has to be checked **before the rename**.
// 2. **CRC the plaintext as it streams**, because the answer the sender is
//    waiting for is about the bytes it sent. The old path read the finished
//    file back off the card and returned *that* CRC; after this the file on the
//    card is ciphertext and its CRC is a number the phone has never seen. So
//    the guarantee weakens on purpose, from "the card holds these bytes" to
//    "these bytes arrived whole and were written". Said out loud here and in
//    firmware docs/wallet-crypto.md rather than left to look like the old one.
//
// ## Two offsets, never one
//
// The file offset counts the 32-byte header; the CTR offset does not. This
// already bit the read path once (docs/wallet-crypto.md, "Assets: CTR, because
// a window has to be seekable"), so the payload offset is a separate member
// here and is the only thing ever handed to the cipher.
//
// ## Header-only, and bound to its primitives by callback
//
// Same reason WalletAsset.h and WalletCrypto.h are: the part that gets an
// offset wrong is pure arithmetic, and pure arithmetic can be host-tested. The
// device binds the cipher to mbedtls and the CRC to esp_rom_crc32_le; the host
// test binds them to OpenSSL and zlib. Two independent primitives, one
// contract -- and the sink is a callback too, so a test can collect the
// ciphertext in memory instead of on a card.

namespace wallet {

// Scratch for one slice of a chunk. The arriving bytes sit in NimBLE's own
// attribute buffer, which is const and is not ours to encrypt in place, so a
// copy is unavoidable; this bounds it.
//
// 512 covers a whole BLE chunk in one slice at any MTU the stack can negotiate
// (ATT 517 minus 3 ATT header minus 5 frame header = 509 payload bytes), so the
// common case does exactly one memcpy, one cipher call and one write. A bigger
// chunk is not refused, just sliced.
inline constexpr size_t kIngestScratchBytes = 512;

// Encrypts `len` bytes in place at **payload** offset `payloadOffset`. The
// device binds this to ctrDecryptInPlace() -- AES-CTR, so the same call
// encrypts -- and the host test binds it to OpenSSL through the same shared
// loop. False means the primitive failed and the transfer must be abandoned.
using PayloadCipher = bool (*)(void* ctx, const uint8_t iv[kAssetIvLen], uint32_t payloadOffset, uint8_t* data,
                               size_t len);

// CRC-32 (the zlib/esp_rom_crc32_le polynomial), accumulated over the bytes as
// they arrived. A callback rather than a second implementation: the number has
// to match what the sender computed, and the device's own ROM routine is the
// one that has to produce it.
using Crc32Update = uint32_t (*)(uint32_t crc, const uint8_t* data, size_t len);

// Writes `len` bytes to wherever the asset is being assembled -- the `.part`
// file on the device, a vector in the tests. False is a short write.
using IngestWrite = bool (*)(void* ctx, const uint8_t* data, size_t len);

// What an ingest step decided. Everything but Ok and Complete ends the
// transfer; the caller turns it into an `ERR <reason>` and deletes the `.part`.
enum class IngestStatus : uint8_t {
  Ok = 0,        // bytes consumed, more expected
  Complete,      // the declared length landed and every check passed
  Malformed,     // bad magic, short header, or geometry that cannot be true
  Encrypted,     // the sender already encrypted it -- this device makes its own K
  WrongPanel,    // built for another panel in the line
  BadLength,     // rawLen and the declared transfer length disagree
  HashMismatch,  // the plaintext does not hash to the prefix the header declared
  CrcMismatch,   // the bytes received are not the bytes the sender declared
  Overrun,       // more bytes than declared
  CipherFailed,
  WriteFailed,
  NotActive,  // fed without a begin()
};

// A short reason string for the wire, so the phone's log says which gate shut.
inline const char* ingestStatusReason(IngestStatus status) {
  switch (status) {
    case IngestStatus::Ok:
    case IngestStatus::Complete:
      return "ok";
    case IngestStatus::Malformed:
      return "bad asset";
    case IngestStatus::Encrypted:
      return "already encrypted";
    case IngestStatus::WrongPanel:
      return "wrong panel";
    case IngestStatus::BadLength:
      return "asset length";
    case IngestStatus::HashMismatch:
      return "hash mismatch";
    case IngestStatus::CrcMismatch:
      return "crc mismatch";
    case IngestStatus::Overrun:
      return "overrun";
    case IngestStatus::CipherFailed:
      return "encrypt failed";
    case IngestStatus::WriteFailed:
      return "write failed";
    case IngestStatus::NotActive:
      return "no transfer";
  }
  return "bad asset";
}

// Is this header one this board can be handed, and does its geometry hold
// together? `payloadLen` is what the transfer declared minus the header.
//
// **Deliberately not checkAssetForPanel().** That gate is for a full-screen
// panel-native tile and is reached with a manifest in hand; ingest has no
// manifest -- the manifest is its own transfer and its own task (T-2096) -- so
// a page image's stride, which the manifest normally states, has to come from
// the file's own numbers here. rawLen / height is that stride, whatever padding
// the generator chose, so this refuses nothing it cannot actually prove wrong.
inline IngestStatus checkAssetForIngest(const AssetHeader& header, const PanelGeometry& live, uint32_t payloadLen) {
  // The header's own length field and the transfer's declared length are two
  // independent statements about the same file. Disagreeing means one of them
  // is corrupt, and the asset would read short or long for ever.
  if (header.rawLen != payloadLen || payloadLen == 0) return IngestStatus::BadLength;
  if (header.height == 0 || header.width == 0) return IngestStatus::Malformed;

  switch (header.assetType) {
    case AssetType::Fit:
    case AssetType::DetailTile:
    case AssetType::OneToOneTile:
    case AssetType::MachineCode: {
      // A panel-native tile is one whole screen, so its geometry is the panel's
      // exactly -- the same three comparisons checkAssetForPanel() makes at
      // read time, made here so a wrong-device push never reaches the card.
      if (header.bitDepth != kBitDepth1) return IngestStatus::Malformed;
      if (live.bufferBytes == 0 || header.rawLen != live.bufferBytes) return IngestStatus::WrongPanel;
      if (header.width != live.width || header.height != live.height) return IngestStatus::WrongPanel;
      return IngestStatus::Ok;
    }
    case AssetType::PageImage:
    case AssetType::PageImageGrey:
    case AssetType::GreyPlanes: {
      const bool twoBitImage = header.assetType == AssetType::PageImageGrey;
      const bool threePlanes = header.assetType == AssetType::GreyPlanes;
      const uint8_t wantDepth = header.assetType == AssetType::PageImage ? kBitDepth1 : kBitDepth2;
      if (header.bitDepth != wantDepth) return IngestStatus::Malformed;
      // Three concatenated planes for assetType 7, one image otherwise
      // (WalletAsset.h, "Grey page images").
      const uint32_t rows = static_cast<uint32_t>(header.height) * (threePlanes ? 3u : 1u);
      if (rows == 0 || header.rawLen % rows != 0) return IngestStatus::Malformed;
      const uint32_t stride = header.rawLen / rows;

      // **The stride must be exactly wide enough for the row, never wider.**
      // Deriving it and then only asking "is it at least the panel's" was the
      // gap a review found on 2026-11-27: a header claiming width 800, height
      // 1600, rawLen 320000 yields a 200-byte stride where the row needs 100,
      // passes every other check here, gets sealed, and is then refused for ever
      // by the read path, which compares the stride against the manifest. That
      // is exactly "stores something that can never be drawn".
      //
      // It is also the one bug that draws a *plausible* picture when a reader
      // does accept it -- every row shifted a little further than the last,
      // which reads as a skewed document rather than as a broken file. The read
      // path says so in those words (WalletAsset.h, checkGreyGeometry); ingest
      // now makes the same check, against the file's own numbers rather than a
      // manifest it does not have.
      //
      // The upper bound is the ceiling form, so both spellings a generator might
      // use for a width that is a multiple of 8 agree, and nothing
      // `tools/walletgen.py` emits is refused: it writes `native_w // 8` for a
      // 1bpp page and `native_w // 4` for the 2bpp form, which equal the ceiling
      // for every width it produces.
      const uint32_t pixelsPerByte = twoBitImage ? 4u : 8u;
      const uint32_t maxStride = twoBitImage ? greyRowBytes2bpp(header.width) : greyPlaneRowBytes(header.width);
      if (stride * pixelsPerByte < header.width || stride > maxStride) return IngestStatus::Malformed;

      // A page is bigger than the panel and is not panel-shaped, so the only
      // thing left to assert without a manifest is that a panel-sized window
      // fits inside it. The 2bpp form is checked against the 1bpp stride its
      // planes would have, same as checkGreyGeometry() does: a page narrower
      // than the panel is a wrong-device asset either way.
      //
      // The live geometry is checked for zero here, the same way the tile branch
      // checks `live.bufferBytes` -- a renderer that answered zero would
      // otherwise make every page image pass.
      if (live.rowBytes == 0 || live.height == 0) return IngestStatus::WrongPanel;
      const uint32_t panelRow = twoBitImage ? greyPlaneRowBytes(header.width) : stride;
      if (panelRow < live.rowBytes || header.height < live.height) return IngestStatus::WrongPanel;
      return IngestStatus::Ok;
    }
    case AssetType::Unknown:
      break;
  }
  // An assetType this firmware does not know is refused rather than stored: it
  // would be sealed under a key only this board holds, and then never drawn.
  return IngestStatus::Malformed;
}

// Streams one asset: header first, then payload, encrypting as it goes.
//
// One instance per transfer, allocated when a wallet asset actually starts, so
// a tile push pays nothing for it (MapTransferReceiver.cpp).
class AssetIngest {
 public:
  struct Callbacks {
    PayloadCipher cipher = nullptr;
    void* cipherCtx = nullptr;
    Crc32Update crc32 = nullptr;
    IngestWrite write = nullptr;
    void* writeCtx = nullptr;
  };

  // `totalLen` is the whole file the sender declared, header included;
  // `declaredCrc` is its CRC-32 over those same bytes. False for a transfer too
  // short to be an asset at all, or callbacks that are not all wired.
  bool begin(uint32_t totalLen, uint32_t declaredCrc, const PanelGeometry& live, const Callbacks& cb) {
    // **First, before any way out.** A refused begin that left `active_` set
    // would let a caller that ignored the return value feed the *next*
    // transfer's plaintext into the *previous* transfer's keystream, at the
    // previous payload offset -- one keystream over two plaintexts, which is
    // the one failure in this file that cannot be recovered from. The receiver
    // allocates a fresh instance per transfer today, so this is unreachable;
    // it is one line and it keeps the class safe to reuse. Review, 2026-11-27.
    active_ = false;
    if (totalLen <= kAssetHeaderBytes) return false;
    if (cb.cipher == nullptr || cb.crc32 == nullptr || cb.write == nullptr) return false;
    cb_ = cb;
    live_ = live;
    total_ = totalLen;
    declaredCrc_ = declaredCrc;
    received_ = 0;
    payloadOffset_ = 0;
    headerFill_ = 0;
    headerDone_ = false;
    crc_ = 0;
    sha_.reset();
    std::memset(iv_, 0, sizeof(iv_));
    header_ = AssetHeader{};
    active_ = true;
    return true;
  }

  // One arriving chunk, in order. Returns Complete on the chunk that carries
  // the last declared byte -- and only after the hash and the CRC have both
  // been checked, because the caller renames on Complete.
  IngestStatus feed(const uint8_t* data, size_t len) {
    if (!active_) return IngestStatus::NotActive;
    if (len == 0) return IngestStatus::Ok;
    if (static_cast<uint64_t>(received_) + len > total_) return endWith(IngestStatus::Overrun);

    // The CRC covers the bytes **as the sender wrote them**, so it is taken
    // here, before the header's flag byte is rewritten below. That is what
    // makes the `OK <bytes> <crc>` reply comparable with what the phone
    // computed over its own file (android-wallet.md 14.5).
    crc_ = cb_.crc32(crc_, data, len);

    size_t at = 0;
    if (!headerDone_) {
      const size_t want = kAssetHeaderBytes - headerFill_;
      const size_t take = len < want ? len : want;
      std::memcpy(headerBuf_ + headerFill_, data, take);
      headerFill_ += take;
      at += take;
      received_ += static_cast<uint32_t>(take);
      // A 23-byte MTU carries 15 payload bytes, so the header can span three
      // chunks. Nothing is written and nothing is decided until all 32 are in.
      if (headerFill_ < kAssetHeaderBytes) return IngestStatus::Ok;
      const IngestStatus opened = openHeader();
      if (opened != IngestStatus::Ok) return endWith(opened);
    }

    while (at < len) {
      const size_t take = (len - at) < kIngestScratchBytes ? (len - at) : kIngestScratchBytes;
      // The plaintext is hashed before it is encrypted, and from the scratch
      // copy rather than from `data`, so the hash and the ciphertext are
      // provably over the same bytes.
      std::memcpy(scratch_, data + at, take);
      sha_.update(scratch_, take);
      if (!cb_.cipher(cb_.cipherCtx, iv_, payloadOffset_, scratch_, take)) return endWith(IngestStatus::CipherFailed);
      if (!cb_.write(cb_.writeCtx, scratch_, take)) return endWith(IngestStatus::WriteFailed);
      payloadOffset_ += static_cast<uint32_t>(take);
      received_ += static_cast<uint32_t>(take);
      at += take;
    }

    if (received_ < total_) return IngestStatus::Ok;
    return endWith(verify());
  }

  // The CRC-32 of the plaintext, which is the number the sender is waiting for.
  uint32_t plaintextCrc() const { return crc_; }
  const AssetHeader& header() const { return header_; }
  bool active() const { return active_; }

  // Drops the sensitive state. The IV is not secret -- it sits in the cleartext
  // header -- but the scratch holds up to 512 bytes of a rider's document, and
  // nothing else would clear it.
  //
  // secureWipe rather than memset: a store to a buffer that is about to die is
  // exactly the store a compiler is allowed to delete, and ~AssetIngest below
  // is that case (WalletCrypto.h, secureWipe).
  void reset() {
    active_ = false;
    headerDone_ = false;
    headerFill_ = 0;
    secureWipe(scratch_, sizeof(scratch_));
    secureWipe(headerBuf_, sizeof(headerBuf_));
    sha_.reset();
  }

  // A transfer killed mid-stream -- Back pressed, link dropped -- never reaches
  // a terminal feed(), so the destructor is the only wipe it gets.
  ~AssetIngest() { reset(); }

 private:
  // Parses the arrived header, decides whether this board will take the asset,
  // builds the IV, and writes the header **this device owns** -- the sender's
  // bytes with the two crypto flags set.
  IngestStatus openHeader() {
    if (!parseAssetHeader(headerBuf_, kAssetHeaderBytes, header_)) return IngestStatus::Malformed;
    // A sender that encrypted it already used a key this board does not have,
    // and encrypting again would bury the payload under two keystreams. There
    // is no path where this is the right thing to store.
    if ((header_.flags & kFlagEncrypted) != 0) return IngestStatus::Encrypted;

    const IngestStatus gate = checkAssetForIngest(header_, live_, total_ - static_cast<uint32_t>(kAssetHeaderBytes));
    if (gate != IngestStatus::Ok) return gate;

    // The prefix is taken on trust here and checked at the end -- it has to be,
    // because the keystream is needed before the payload it hashes has arrived.
    // verify() is what closes that loop.
    if (!buildAssetIvFromHash(header_.sha256Prefix, sizeof(header_.sha256Prefix), header_.version, iv_)) {
      return IngestStatus::Malformed;
    }

    // Byte 20 is `flags` (WalletAsset.h, parseAssetHeader). Both bits, always:
    // the payload below is now ciphertext, and its IV came from the content
    // hash rather than from the assetId, which is the only thing that tells the
    // read path which IV to rebuild.
    headerBuf_[20] = static_cast<uint8_t>(header_.flags | kFlagEncrypted | kFlagContentIv);
    header_.flags = headerBuf_[20];
    if (!cb_.write(cb_.writeCtx, headerBuf_, kAssetHeaderBytes)) return IngestStatus::WriteFailed;
    headerDone_ = true;
    return IngestStatus::Ok;
  }

  // Both end-of-transfer checks, in the order that matters: the CRC says the
  // bytes arrived whole, the hash says they are the bytes this keystream was
  // built for. A file that fails either one must not be renamed into place.
  IngestStatus verify() {
    if (crc_ != declaredCrc_) return IngestStatus::CrcMismatch;
    uint8_t digest[kSha256Bytes];
    sha_.finish(digest);
    if (std::memcmp(digest, header_.sha256Prefix, kSha256PrefixBytes) != 0) return IngestStatus::HashMismatch;
    return IngestStatus::Complete;
  }

  // Every exit but "more bytes please" goes through here, the clean one
  // included, so the document plaintext left in the scratch buffer is wiped on
  // the failure paths too and not only when a transfer completes.
  IngestStatus endWith(IngestStatus status) {
    reset();
    return status;
  }

  Callbacks cb_;
  PanelGeometry live_;
  AssetHeader header_;
  Sha256 sha_;
  uint8_t iv_[kAssetIvLen] = {0};
  uint8_t headerBuf_[kAssetHeaderBytes] = {0};
  uint8_t scratch_[kIngestScratchBytes] = {0};
  uint32_t total_ = 0;
  uint32_t declaredCrc_ = 0;
  uint32_t received_ = 0;
  uint32_t payloadOffset_ = 0;
  uint32_t crc_ = 0;
  size_t headerFill_ = 0;
  bool headerDone_ = false;
  bool active_ = false;
};

// ASCII case fold, because the thing this gate protects is matched that way.
inline char asciiUpper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

inline bool asciiEqualFold(const char* a, const char* b, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (asciiUpper(a[i]) != asciiUpper(b[i])) return false;
  }
  return true;
}

// True for a path this receiver must seal rather than store verbatim: a `.dat`
// under the wallet directory. **The path is the signal, not a flag on the
// wire**, so a sender cannot ask for a wallet asset to be written in the clear
// by clearing a bit, and a map tile cannot be turned into one by setting it.
//
// **Case-insensitive, because SdFat is.** This gate decides in C and the
// filesystem underneath decides in FAT, and FAT compares names folded:
// `FatFile::cmpName()` is `toUpper(u) != toUpper(fname->getch())`
// (SdFat `FatLib/FatFileLFN.cpp`). An exact-case gate therefore had a hole
// wide enough to drive a plaintext document through -- found in review,
// 2026-11-27, and it was not theoretical:
//
//   push `wallet/ab/0123456789abcdef.DAT`
//     -> this gate says "not a wallet asset", so no key is needed and the
//        wallet may be locked
//     -> the bytes take the plain tile path and land verbatim
//     -> `Storage.exists()` matches the *sealed* `…abcdef.dat`
//        case-insensitively, `remove()` deletes it, `rename()` puts the
//        plaintext in its place
//     -> the viewer opens `…abcdef.dat`, finds `kFlagEncrypted` clear, and
//        blits the attacker's bitmap as a genuine document.
//
// So the two sides have to agree on what one name means. They now fold the
// same way for the directory prefix and the extension; `relPathIsSafe()`
// (MapTransferReceiver) rejects the other spellings of one name -- a `.`
// component, and a component FAT would strip a trailing dot or space from.
// True for anything at all under `/explorink/wallet`, whatever it is called.
//
// **The whitelist's other half.** Two gates decide what gets sealed --
// pathIsWalletAsset() below and pathIsWalletManifest() -- and anything that
// misses both used to take the ordinary tile path and land on the card
// verbatim. That is a hole, because FAT gives one file more than one name:
// SdFat builds an 8.3 alias for every name that is not 8.3, writing `~1` at
// `seqPos` and setting FNAME_FLAG_LOST_CHARS (`FatLib/FatFileLFN.cpp`,
// lfnParse), and its open() matches a query spelled with that alias against
// the same directory entry (the `memcmp(dir->name, fname->sfn, ...)` branch).
// So `wallet/MANIFE~1.JSO` is `wallet/manifest.json` to the filesystem and is
// not `manifest.json` to a name gate. `manifest.enc` and `manifest.bak` have
// the same shape without needing an alias at all -- and since the backup
// became the half-swap survivor, writing junk over it is writing over the only
// list a half-installed card has.
//
// So the receiver refuses everything under this directory that is not one of
// the two things the format puts there. A name nobody can seal is a name
// nobody can leave in the clear either. Found in review, 2026-11-27, next to
// the case-fold hole below.
inline bool pathIsUnderWalletDir(const char* fullPath) {
  if (fullPath == nullptr) return false;
  const size_t dirLen = std::strlen(kWalletDir);
  if (std::strlen(fullPath) <= dirLen) return false;
  if (!asciiEqualFold(fullPath, kWalletDir, dirLen)) return false;
  return fullPath[dirLen] == '/';
}

inline bool pathIsWalletAsset(const char* fullPath) {
  if (fullPath == nullptr) return false;
  const size_t dirLen = std::strlen(kWalletDir);
  const size_t len = std::strlen(fullPath);
  if (len < dirLen + 1 + 4) return false;
  if (!asciiEqualFold(fullPath, kWalletDir, dirLen)) return false;
  if (fullPath[dirLen] != '/') return false;
  return asciiEqualFold(fullPath + len - 4, ".dat", 4);
}

}  // namespace wallet
