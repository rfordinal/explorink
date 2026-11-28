#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "WalletAsset.h"

// Wallet crypto v1, the parts that are pure byte work.
//
// The contract is frozen and lives in the parent repo's `docs/wallet-format.md`,
// section 11. This header implements the half of it that needs no library: the
// CTR IV, the offset arithmetic a windowed read depends on, the encrypted
// manifest's envelope, and the unlock pattern. `WalletCryptoDevice.h` binds the
// primitives to mbedtls on the device; the host tests bind them to OpenSSL, and
// the arithmetic below is the same code on both sides -- which is the point,
// because the arithmetic is what a windowed read gets wrong.
//
// ## What is encrypted, and what that buys
//
//   assets    AES-256-CTR, key K, IV per (asset, version)
//   manifest  AES-256-GCM, key K, fresh 12-byte nonce per write
//
// Card pulled out and read elsewhere: nothing readable. **Device in hand with a
// USB cable: readable** -- the wrapped key is in NVS, NVS is plaintext flash.
// See ../../../docs/wallet-crypto.md, "The threat boundary".

namespace wallet {

inline constexpr size_t kWalletKeyLen = 32;
inline constexpr size_t kAesBlockLen = 16;
inline constexpr size_t kAssetIvLen = 16;

// Exactly `outLen * 2` hex characters into `outLen` bytes, or false. Used for the
// key and the salt a provisioning command hands over: a short or mistyped hex
// string must be refused, never zero-padded into a key that half works.
inline bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
  if (hex == nullptr || out == nullptr) return false;
  size_t i = 0;
  for (; hex[i] != '\0'; ++i) {
    if (i >= outLen * 2) return false;
    uint8_t nibble = 0;
    if (!hexNibble(hex[i], nibble)) return false;
    if ((i % 2) == 0) {
      out[i / 2] = static_cast<uint8_t>(nibble << 4);
    } else {
      out[i / 2] = static_cast<uint8_t>(out[i / 2] | nibble);
    }
  }
  return i == outLen * 2;
}

// ---------------------------------------------------------------------------
// Assets: AES-256-CTR
// ---------------------------------------------------------------------------

// IV = assetId raw bytes (8) || u32 version little endian || u32 zero.
//
// The trailing word is where the block counter lives. AES-CTR increments the
// whole 128-bit block, which is the same thing here: the largest asset is a page
// image of a few hundred thousand bytes, tens of thousands of blocks, nowhere
// near a carry out of that word.
inline bool buildAssetIv(const char* assetId, uint32_t version, uint8_t iv[kAssetIvLen]) {
  if (iv == nullptr || !isValidAssetId(assetId)) return false;
  std::memset(iv, 0, kAssetIvLen);
  for (size_t i = 0; i < 8; ++i) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (!hexNibble(assetId[i * 2], hi) || !hexNibble(assetId[i * 2 + 1], lo)) return false;
    iv[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  iv[8] = static_cast<uint8_t>(version & 0xFF);
  iv[9] = static_cast<uint8_t>((version >> 8) & 0xFF);
  iv[10] = static_cast<uint8_t>((version >> 16) & 0xFF);
  iv[11] = static_cast<uint8_t>((version >> 24) & 0xFF);
  return true;
}

// IV = sha256(payload)[0:8] || u32 version little endian || u32 zero.
//
// **The content-derived form, flags bit 1 (`kFlagContentIv`).** Same shape as
// the assetId form above and the same trailing counter word; only the first
// eight bytes change, from "which asset is this" to "which bytes are these".
// That is the whole fix: an assetId repeats across a re-render, the payload's
// hash does not, so one key can never run the same keystream over two different
// plaintexts. The hash is the one already in the asset's cleartext header, so
// the device reads it from bytes it has in hand before the first decrypted byte
// exists.
//
// Two assets with identical bytes do get identical ciphertext. That is not a new
// leak: the plaintext hash sits in the cleartext header either way.
inline bool buildAssetIvFromHash(const uint8_t* sha256Prefix, size_t prefixLen, uint32_t version,
                                 uint8_t iv[kAssetIvLen]) {
  if (iv == nullptr || sha256Prefix == nullptr || prefixLen < 8) return false;
  std::memset(iv, 0, kAssetIvLen);
  std::memcpy(iv, sha256Prefix, 8);
  iv[8] = static_cast<uint8_t>(version & 0xFF);
  iv[9] = static_cast<uint8_t>((version >> 8) & 0xFF);
  iv[10] = static_cast<uint8_t>((version >> 16) & 0xFF);
  iv[11] = static_cast<uint8_t>((version >> 24) & 0xFF);
  return true;
}

// Picks the IV form the asset's own flags ask for. Every call site goes through
// this rather than choosing for itself -- a call site that picked the legacy form
// for a content-IV asset would decrypt to noise, with no error anywhere.
inline bool buildAssetIvForAsset(uint8_t flags, const char* assetId, const uint8_t* sha256Prefix, size_t prefixLen,
                                 uint32_t version, uint8_t iv[kAssetIvLen]) {
  if ((flags & kFlagContentIv) != 0) return buildAssetIvFromHash(sha256Prefix, prefixLen, version, iv);
  return buildAssetIv(assetId, version, iv);
}

// Where a decrypt has to start to reach payload byte `offset`.
//
// **This is the property design B depends on.** A windowed read seeks to a row at
// an arbitrary byte offset inside the payload -- 480 of them per frame, none of
// them 16-aligned in general -- and every one has to be decryptable without
// touching the bytes before it. CTR gives that: start the counter at
// `offset / 16` and throw away `offset % 16` bytes of keystream. Anything chained
// would have forced a decrypt from byte zero per row.
struct CtrStart {
  uint32_t block = 0;
  uint8_t skip = 0;
};

inline CtrStart ctrStartForOffset(uint32_t offset) {
  CtrStart start;
  start.block = offset / kAesBlockLen;
  start.skip = static_cast<uint8_t>(offset % kAesBlockLen);
  return start;
}

// The IV advanced by `blocks` blocks, big-endian over all 128 bits -- what
// AES-CTR does to its counter block between blocks.
inline void advanceCounter(const uint8_t iv[kAssetIvLen], uint32_t blocks, uint8_t out[kAesBlockLen]) {
  std::memcpy(out, iv, kAesBlockLen);
  uint32_t carry = blocks;
  for (int i = static_cast<int>(kAesBlockLen) - 1; i >= 0 && carry != 0; --i) {
    const uint32_t sum = static_cast<uint32_t>(out[i]) + (carry & 0xFF);
    out[i] = static_cast<uint8_t>(sum & 0xFF);
    carry = (carry >> 8) + (sum >> 8);
  }
}

// Encrypts one AES block. The device binds this to mbedtls's ECB (hardware
// accelerated on the C3); the host tests bind it to OpenSSL. Deliberately a
// callback rather than two copies of the loop below: the loop is the part that
// gets an offset wrong, so it exists once and is host-tested once.
//
// **It answers, and a false is not ignorable.** On both chips the ESP-IDF AES
// driver is the DMA one, and on an allocation failure, an ISR init failure or any
// AES failure it zeroises the output block and returns -1 (framework-espidf
// 5.5.2, components/mbedtls/port/aes/dma/esp_aes_dma_core.c, the comment above
// esp_aes_process_dma). A zero keystream block XORed in is the plaintext left as
// it was: on the ingest path that is a document written to the card in the
// clear, under an `OK`, because both end-of-transfer checks are over the
// plaintext. Found by the T-2104 security review, 2026-11-28.
using BlockEncrypt = bool (*)(void* ctx, const uint8_t in[kAesBlockLen], uint8_t out[kAesBlockLen]);

// Defined at the end of this file; ctrXorInPlace() below wipes with it.
inline void secureWipe(void* data, size_t len);

// XORs `len` bytes at `data` with the keystream for payload offset `offset`.
// Symmetric: the same call encrypts and decrypts. **In place, always** -- the
// data is already in the framebuffer and a second 48 KB buffer would not fit
// (../../../docs/map-memory.md:57).
//
// False when a block failed to encrypt. `data` is then partly XORed and must be
// treated as nothing: the caller refuses the whole unit (walletCipher() ends the
// transfer, the read path reports a decrypt failure). The keystream and counter
// are wiped on every return, so no keystream outlives the call on the stack.
inline bool ctrXorInPlace(BlockEncrypt encrypt, void* ctx, const uint8_t iv[kAssetIvLen], uint32_t offset,
                          uint8_t* data, size_t len) {
  if (encrypt == nullptr || data == nullptr) return false;
  if (len == 0) return true;
  const CtrStart start = ctrStartForOffset(offset);
  uint8_t counter[kAesBlockLen];
  advanceCounter(iv, start.block, counter);

  uint8_t keystream[kAesBlockLen];
  size_t at = 0;
  size_t skip = start.skip;
  bool ok = true;
  while (at < len) {
    if (!encrypt(ctx, counter, keystream)) {
      ok = false;
      break;
    }
    for (size_t i = skip; i < kAesBlockLen && at < len; ++i) data[at++] ^= keystream[i];
    skip = 0;
    // Increment the counter block, big-endian, exactly as CTR does.
    for (int i = static_cast<int>(kAesBlockLen) - 1; i >= 0; --i) {
      if (++counter[i] != 0) break;
    }
  }
  secureWipe(keystream, sizeof(keystream));
  secureWipe(counter, sizeof(counter));
  return ok;
}

// ---------------------------------------------------------------------------
// Manifest: AES-256-GCM in an "EWM1" envelope
// ---------------------------------------------------------------------------

inline constexpr uint8_t kManifestMagic[4] = {'E', 'W', 'M', '1'};
// v1: no version field. v2 adds a cleartext u32 walletVersion after
// plaintextLen, so a **locked** device can still say which version the card
// holds -- background sync computes pending work with no key present
// (brief 23-26, 54). Both are read; a v1 card must not become unreadable.
inline constexpr uint8_t kManifestEncVersionV1 = 1;
inline constexpr uint8_t kManifestEncVersion = 2;
inline constexpr size_t kGcmNonceLen = 12;
inline constexpr size_t kGcmTagLen = 16;
// v1: magic(4) | version(1) | flags(1) | nonce(12) | plaintextLen(4)
// v2: ... | walletVersion(4)
inline constexpr size_t kManifestEnvelopeLenV1 = 22;
inline constexpr size_t kManifestEnvelopeLen = 26;

inline constexpr const char* kManifestEncPath = "/explorink/wallet/manifest.enc";

// The whole encrypted manifest has to be in RAM before a single byte of it is
// parsed, because GCM only authenticates once it has seen everything. So the size
// is capped rather than trusted.
//
// 32 KB is about three or four documents at the demo tree's density -- 8.8 KB of
// manifest for one document with all three tile levels, 21 assets and a code --
// and far more for a page-image tree or a document with a small 1:1 grid. Above
// the cap the wallet says so instead of failing an allocation
// (../../../docs/wallet-crypto.md, "The manifest has to fit in RAM").
inline constexpr size_t kMaxEncryptedManifestBytes = 32u * 1024u;

// Reads only the cleartext envelope of manifest.enc -- 26 bytes, no key, no
// decryption, so it answers while the wallet is locked. Returns false when the
// card has no encrypted manifest or the envelope is a v1 that does not carry a
// version. See ManifestEnvelope::walletVersion for why this value is a hint and
// never a trust anchor.
bool readCardWalletVersion(uint32_t& out);

struct ManifestEnvelope {
  bool valid = false;
  uint8_t nonce[kGcmNonceLen] = {0};
  uint32_t plaintextLen = 0;
  // 0 means "this card does not say" -- a v1 envelope, not version zero.
  // **Never authenticated**: it sits outside the GCM tag, so a flipped bit here
  // is not caught by anything. Worst case it makes the phone re-send work it did
  // not need to, because every asset is still verified by its own hash. Do not
  // treat it as trustworthy, and do not gate anything destructive on it.
  uint32_t walletVersion = 0;
  bool hasWalletVersion = false;
  // Where the ciphertext starts and how long it is; the 16-byte tag follows it.
  size_t ciphertextOffset = 0;
  size_t ciphertextLen = 0;
  size_t tagOffset = 0;
};

// Structural parse of the envelope. **The envelope itself is not authenticated**:
// the generator seals with no associated data
// (`walletgen.py`, `encrypt_manifest`), so magic, version, flags and
// `plaintextLen` are all outside the tag. A flipped bit in `plaintextLen` is
// caught by the length check *after* the tag verifies, not by the tag. Stated
// because it is the kind of thing a reader assumes the other way round.
inline bool parseManifestEnvelope(const uint8_t* bytes, size_t fileLen, ManifestEnvelope& out) {
  out = ManifestEnvelope{};
  if (bytes == nullptr) return false;
  if (fileLen < kManifestEnvelopeLenV1 + kGcmTagLen) return false;
  if (std::memcmp(bytes, kManifestMagic, sizeof(kManifestMagic)) != 0) return false;
  const uint8_t container = bytes[4];
  size_t envelopeLen = 0;
  if (container == kManifestEncVersionV1) {
    envelopeLen = kManifestEnvelopeLenV1;
  } else if (container == kManifestEncVersion) {
    envelopeLen = kManifestEnvelopeLen;
    if (fileLen < envelopeLen + kGcmTagLen) return false;
    out.walletVersion = readLe32(bytes + 22);
    out.hasWalletVersion = true;
  } else {
    return false;
  }
  // bytes[5] is flags; none is defined, and an unknown one is not a reason to
  // refuse a file the tag will authenticate anyway.
  std::memcpy(out.nonce, bytes + 6, kGcmNonceLen);
  out.plaintextLen = readLe32(bytes + 18);
  out.ciphertextOffset = envelopeLen;
  const size_t sealed = fileLen - envelopeLen;
  if (sealed < kGcmTagLen) return false;
  out.ciphertextLen = sealed - kGcmTagLen;
  out.tagOffset = envelopeLen + out.ciphertextLen;
  // The header's promise has to match the file's shape before anything is
  // decrypted -- a mismatch here means the file is truncated or spliced.
  if (out.plaintextLen != out.ciphertextLen) return false;
  if (out.plaintextLen == 0) return false;
  out.valid = true;
  return true;
}

// Builds the 26-byte v2 envelope a sealed manifest starts with. The mirror of
// parseManifestEnvelope() above, and the reason it is here rather than next to
// the mbedtls call: the two halves of one layout belong in one file, where a
// field moved on one side is visibly not moved on the other.
//
// **The device is now a second writer of this format**, after the generator
// (`tools/walletgen.py`, `encrypt_manifest`) and the phone. Nothing about the
// layout changes for that -- parent repo docs/wallet-format.md section 11 is the
// contract and it was not reopened (T-2096).
//
// `flags` is written 0: no bit is defined for this envelope, and a reader is
// told to ignore unknown ones. `walletVersion` sits **outside the tag** on
// purpose, so a locked card can still be asked which version it holds; 0 means
// "this card does not say", which costs the phone a resend and never a wrong
// sync.
inline bool buildManifestEnvelope(const uint8_t nonce[kGcmNonceLen], uint32_t plaintextLen, uint32_t walletVersion,
                                  uint8_t* out, size_t outLen) {
  if (nonce == nullptr || out == nullptr || outLen < kManifestEnvelopeLen) return false;
  // A zero-length manifest is refused rather than sealed: parseManifestEnvelope()
  // refuses to open one, so writing it would be writing a file this device
  // cannot read -- the exact failure T-2096 exists to design against.
  if (plaintextLen == 0) return false;
  std::memcpy(out, kManifestMagic, sizeof(kManifestMagic));
  out[4] = kManifestEncVersion;
  out[5] = 0;
  std::memcpy(out + 6, nonce, kGcmNonceLen);
  writeLe32(out + 18, plaintextLen);
  writeLe32(out + 22, walletVersion);
  return true;
}

// The sealed size of a manifest of `plaintextLen` bytes: envelope, ciphertext
// (GCM is a stream cipher, so ciphertext length == plaintext length), tag.
//
// Exists so the **one** place that decides whether a manifest may be accepted
// at all computes the same number the read path will later measure off the
// card, instead of a comment claiming they agree.
inline constexpr size_t sealedManifestLen(const size_t plaintextLen) {
  return kManifestEnvelopeLen + plaintextLen + kGcmTagLen;
}

// The largest plaintext this device may seal, which is derived from the cap the
// read path enforces rather than chosen next to it.
//
// **This is the whole size trap of T-2096.** mbedtls can stream a GCM seal, so a
// sealer written from the wire would happily write a manifest of any size -- and
// the same device then refuses to read anything above kMaxEncryptedManifestBytes.
// That is a wallet written successfully and never openable again on the board
// that wrote it. So the plaintext is buffered, bounded by this, and refused
// before a byte is taken.
inline constexpr size_t kMaxManifestPlaintextBytes = kMaxEncryptedManifestBytes - kManifestEnvelopeLen - kGcmTagLen;

// ---------------------------------------------------------------------------
// The unlock pattern
// ---------------------------------------------------------------------------

// A path walked on the gate's 4x4 grid (`components/lock/PatternLock.h`), 8 to 12
// cells counting the start, encoded one character per cell: '0'..'9' then
// 'a'..'f'.
//
// **The encoded path IS the password.** `PatternLockActivity` hands its verifier
// exactly this string, so nothing translates between what a rider walks and what
// PBKDF2 hashes -- a translation step is a place for the two sides to disagree,
// and a disagreement here is an unopenable wallet.
//
// ## Why 8 to 12 and not the gate's own 4 to 8
//
// This replaced a 6-to-10-symbol directional PIN worth 12.0 to 20.0 bits. A
// pattern buys about 1.7 bits per point, so the gate's default floor of 4 points
// is 46 walks -- 5.5 bits -- and accepting it would have dropped the floor by
// six and a half bits while the docs still promised the old number. 8 to 12
// points is 7 to 11 presses, one more than the old PIN at each end, for 12.3 to
// 19.6 bits: the same strength for the same effort.
//
//   8 to 12 points, interior start   806,270 walks   19.6 bits
//
// Counted by `scripts/pattern_space.py`, not estimated.
//
// **The pattern is still not the strength**, exactly as the PIN was not. What
// makes the wrap hard to attack is the 32-byte device secret mixed into the KDF
// password, and what makes guessing impractical is the rate limiter below
// (../../../docs/wallet-crypto.md, "What the pattern is for").
inline constexpr size_t kPatternMinPoints = 8;
inline constexpr size_t kPatternMaxPoints = 12;
inline constexpr size_t kPatternBufBytes = kPatternMaxPoints + 1;

// The start cell a stored pattern was provisioned on. Kept next to the salt in
// NVS rather than derived, because the wrap is all the device stores and a wrap
// says nothing about the path that produced it. It is **not a secret**: the gate
// draws the cursor on it before the first press, so a shoulder has it anyway, and
// NVS is plaintext flash to anyone with a cable (../../../docs/wallet-crypto.md,
// "The threat boundary").
//
// It still has to be stored, because a gate opened on the wrong start cell is a
// permanent lockout: every path a rider can walk begins with that cell's
// character and the stored one begins with another.
inline constexpr uint8_t kPatternNoStart = 0xFF;

// Lower-case hex digit for a cell, matching `patternlock::Entry::encode()`.
inline bool patternCharIsLegal(const char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline char patternCharFold(const char c) { return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c; }

// Normalises a pattern string to lower-case hex of a legal length. False for
// anything else -- a pattern that is not exactly what was provisioned cannot
// unwrap the key anyway, and refusing it here gives a message instead of a failed
// attempt against the rate limiter.
//
// It checks the alphabet and the length and **not the geometry**: at unlock time
// the path came from the gate, which cannot produce an illegal step, and a
// host-side typo that happens to be legal hex is a wrong pattern rather than a
// malformed one. Provisioning is where geometry is checked, with
// `patternlock::isWellFormed()` against the start cell being stored -- that is
// the call that prevents an unopenable wallet, and it needs the start cell, which
// this function does not have.
inline bool normalisePattern(const char* text, char* out, size_t outLen, size_t& length) {
  length = 0;
  if (text == nullptr || out == nullptr || outLen < kPatternBufBytes) return false;
  size_t at = 0;
  for (; text[at] != '\0'; ++at) {
    if (at >= kPatternMaxPoints) return false;
    if (!patternCharIsLegal(text[at])) return false;
    out[at] = patternCharFold(text[at]);
  }
  out[at] = '\0';
  if (at < kPatternMinPoints) return false;
  length = at;
  return true;
}

// ---------------------------------------------------------------------------
// Rate limiting (brief section 44)
// ---------------------------------------------------------------------------

// After this many failures the wrap is locked until the device is provisioned
// again. There is no unlock-by-waiting past it: a pattern is 2^12 to 2^20
// guesses, so an attacker with unlimited tries wins, and the only defence that
// actually holds is a hard stop.
inline constexpr uint8_t kMaxUnlockFailures = 10;
// Failures below this cost nothing: a rider fat-fingering one press should not be
// punished.
inline constexpr uint8_t kFreeUnlockFailures = 3;

// How long the next attempt has to wait, in milliseconds. Doubles per failure
// past the free ones, capped so the screen never looks hung.
//
// **This is a UX defence, not a cryptographic one.** It slows a person at the
// device. It does nothing against somebody who dumps NVS over USB and attacks the
// wrap offline -- that case is out of scope and the doc says so.
inline uint32_t unlockFailureDelayMs(uint8_t failures) {
  if (failures <= kFreeUnlockFailures) return 0;
  const uint8_t over = static_cast<uint8_t>(failures - kFreeUnlockFailures);
  uint32_t delay = 1000;
  for (uint8_t i = 1; i < over && delay < 30000; ++i) delay *= 2;
  return delay > 30000 ? 30000 : delay;
}

inline bool isLockedOut(uint8_t failures) { return failures >= kMaxUnlockFailures; }

// The rate limiter's deadline, as arithmetic with the clock passed in.
//
// It lives here rather than inside `Session` because the part that was wrong was
// never the delay table -- that is a pure function and the host tests had it
// right from the start. What was wrong was **when the deadline is armed**, and
// that was unreachable from a host because it sat next to `millis()`. Pulling it
// out is the whole lesson of the 2026-11-26 X3 run: a tested table and an
// untested interaction add up to a wallet that locks its owner out for ever
// after four typos.
struct RetryGate {
  // 0 means no deadline. Wrap-safe against a 49-day `millis()` rollover through
  // the signed difference below.
  uint32_t retryAtMs = 0;
  // The failure count the deadline was armed for. 0xFF is "nothing armed yet",
  // which no live count reaches -- the hard stop is kMaxUnlockFailures.
  uint8_t armedForFailures = 0xFF;

  // Arms the delay `failures` has earned, **once per count**. A caller arms on
  // every unlock attempt so a reboot cannot skip a delay the persisted count
  // already earned; arming again for a count already armed would push the
  // deadline forward on an attempt that is itself being refused, and the wait
  // could then never be served.
  void arm(const uint8_t failures, const uint32_t nowMs) {
    if (armedForFailures == failures) return;
    armedForFailures = failures;
    const uint32_t delay = unlockFailureDelayMs(failures);
    retryAtMs = delay > 0 ? nowMs + delay : 0;
  }

  uint32_t waitMs(const uint32_t nowMs) const {
    if (retryAtMs == 0) return 0;
    const int32_t left = static_cast<int32_t>(retryAtMs - nowMs);
    return left > 0 ? static_cast<uint32_t>(left) : 0;
  }
};

// What one unlock attempt did. Shared by the gate screen and by
// CMD:WALLETUNLOCK, because they run the same attempt through the same code
// (KeyStore::tryUnlock).
enum class UnlockResult : uint8_t {
  Ok = 0,
  NotProvisioned,  // no wrap in NVS: provision first
  Malformed,       // not 8..12 hex cells, so not spent against the limiter
  Waiting,         // the rate limiter's delay has not run out
  LockedOut,       // ten failures: the wrap is refused until re-provisioning
  BadPattern,      // the unwrap did not authenticate
};

// A stable one-word token per outcome, for the serial reply and the log. Stable on
// purpose: a host script greps these, so they are part of the command's contract
// and renaming one breaks a caller.
//
// Two were renamed once, with the gate: `malformed_pin` and `wrong_pin` became
// `malformed_pattern` and `wrong_pattern` when the directional PIN was replaced,
// because the command's argument changed in the same pass and a token naming a
// PIN that no longer exists is worse than a break nobody can have shipped
// against. `CMD:WALLETUNLOCK` is `env:default` only (`ENABLE_WALLET_TEST_CMDS`
// in platformio.ini), so these tokens have never reached a release image and the
// only callers are this tree's own scripts. **`ENABLE_WALLET` itself is in
// `base`**, so the wallet SCREENS do reach a release build -- the two flags are
// not the same scope, and only the narrower one makes this rename free.
inline const char* unlockResultName(UnlockResult result) {
  switch (result) {
    case UnlockResult::Ok:
      return "ok";
    case UnlockResult::NotProvisioned:
      return "not_provisioned";
    case UnlockResult::Malformed:
      return "malformed_pattern";
    case UnlockResult::Waiting:
      return "rate_limited";
    case UnlockResult::LockedOut:
      return "locked_out";
    case UnlockResult::BadPattern:
      return "wrong_pattern";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// The wrapped key, as it sits in NVS
// ---------------------------------------------------------------------------

// nonce(12) | ciphertext(32) | tag(16). AES-256-GCM over K under the KEK, which
// is PBKDF2-HMAC-SHA256(pattern bytes || deviceSecret, salt, iters). K itself never
// touches the card and never touches NVS unwrapped.
inline constexpr size_t kWrappedKeyLen = kGcmNonceLen + kWalletKeyLen + kGcmTagLen;
inline constexpr size_t kDeviceSecretLen = 32;
inline constexpr size_t kPbkdf2SaltLen = 16;
inline constexpr size_t kKekPasswordBufBytes = kPatternMaxPoints + kDeviceSecretLen;

// What an enrollment on the device provisions with.
//
// Not a guess and not the format doc's 50,000 placeholder: it is the count every
// measurement in `../../../docs/wallet-crypto.md` was taken against -- **903 ms
// on the C3** (X3, `env:default`, 2026-11-26) and **1,047 ms on the S3** (X4 Pro
// and T5 S3 Pro, 2026-11-23). It is also `tools/walletgen.py`'s own default, so
// a wallet made on the device and one made on the laptop cost the same unlock.
//
// Changing it changes nothing about an existing wallet: the count is stored
// beside the wrap it was used for, and an unwrap reads it back
// (`WalletKeyStore.cpp`, `kKeyIters`). It only decides what the *next*
// enrollment costs.
inline constexpr uint32_t kProvisionIterations = 18000u;

// The most PBKDF2 iterations a serial command will run in one go.
//
// mbedtls's PBKDF2 is one blocking call and the serial commands run on the main
// loop, where the task watchdog panics after 5 s (`sdkconfig.defaults`,
// `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`). 200,000 is well inside that at any rate that
// makes the format doc's 50,000 placeholder plausible, and the number anybody is
// looking for is the largest count under **1 s** -- so this cap cannot get in the
// way of the measurement it exists to make safe.
inline constexpr uint32_t kMaxProvisionIterations = 200000u;

// The KDF's password: the encoded pattern then the device secret, in that order
// and with nothing between them.
//
// The pattern bytes are the gate's own encoding, lower-case, start cell included
// -- the same string `patternlock::Entry::encode()` produces. **The start cell is
// part of the password even though it is stored in the clear**, which costs
// nothing: an attacker who can read `pstart` out of plaintext NVS can also read
// the salt and the wrap, so the only question is whether a second implementation
// agrees about the bytes. Including it keeps that answer short.
//
// Separate from the derivation itself so the concatenation -- the part a second
// implementation gets wrong, and the part the laptop's `provision.json` describes
// in prose ("pattern bytes || deviceSecret") -- is host-testable. The PBKDF2 call
// over it is mbedtls on the device and OpenSSL in the tests.
inline bool buildKekPassword(const char* pattern, size_t patternLen, const uint8_t* secret, size_t secretLen,
                             uint8_t* out, size_t outLen, size_t& length) {
  length = 0;
  if (pattern == nullptr || secret == nullptr || out == nullptr) return false;
  if (patternLen < kPatternMinPoints || patternLen > kPatternMaxPoints) return false;
  if (secretLen != kDeviceSecretLen) return false;
  if (outLen < patternLen + secretLen) return false;
  std::memcpy(out, pattern, patternLen);
  std::memcpy(out + patternLen, secret, secretLen);
  length = patternLen + secretLen;
  return true;
}

// Overwrites a buffer and stops the compiler removing the write. `memset` on a
// buffer that is about to die is a dead store and a release build is entitled to
// drop it, which is exactly how a key stays in RAM after a "zeroize".
inline void secureWipe(void* data, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(data);
  while (len-- > 0) *p++ = 0;
}

}  // namespace wallet
