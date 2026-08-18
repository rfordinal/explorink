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
// manifest's envelope, and the directional PIN. `WalletCryptoDevice.h` binds the
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
using BlockEncrypt = void (*)(void* ctx, const uint8_t in[kAesBlockLen], uint8_t out[kAesBlockLen]);

// XORs `len` bytes at `data` with the keystream for payload offset `offset`.
// Symmetric: the same call encrypts and decrypts. **In place, always** -- the
// data is already in the framebuffer and a second 48 KB buffer would not fit
// (../../../docs/map-memory.md:57).
inline void ctrXorInPlace(BlockEncrypt encrypt, void* ctx, const uint8_t iv[kAssetIvLen], uint32_t offset,
                          uint8_t* data, size_t len) {
  if (encrypt == nullptr || data == nullptr || len == 0) return;
  const CtrStart start = ctrStartForOffset(offset);
  uint8_t counter[kAesBlockLen];
  advanceCounter(iv, start.block, counter);

  uint8_t keystream[kAesBlockLen];
  size_t at = 0;
  size_t skip = start.skip;
  while (at < len) {
    encrypt(ctx, counter, keystream);
    for (size_t i = skip; i < kAesBlockLen && at < len; ++i) data[at++] ^= keystream[i];
    skip = 0;
    // Increment the counter block, big-endian, exactly as CTR does.
    for (int i = static_cast<int>(kAesBlockLen) - 1; i >= 0; --i) {
      if (++counter[i] != 0) break;
    }
  }
}

// ---------------------------------------------------------------------------
// Manifest: AES-256-GCM in an "EWM1" envelope
// ---------------------------------------------------------------------------

inline constexpr uint8_t kManifestMagic[4] = {'E', 'W', 'M', '1'};
inline constexpr uint8_t kManifestEncVersion = 1;
inline constexpr size_t kGcmNonceLen = 12;
inline constexpr size_t kGcmTagLen = 16;
// magic(4) | version(1) | flags(1) | nonce(12) | plaintextLen(4)
inline constexpr size_t kManifestEnvelopeLen = 22;

inline constexpr const char* kManifestEncPath = "/trailink/wallet/manifest.enc";

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

struct ManifestEnvelope {
  bool valid = false;
  uint8_t nonce[kGcmNonceLen] = {0};
  uint32_t plaintextLen = 0;
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
  if (fileLen < kManifestEnvelopeLen + kGcmTagLen) return false;
  if (std::memcmp(bytes, kManifestMagic, sizeof(kManifestMagic)) != 0) return false;
  if (bytes[4] != kManifestEncVersion) return false;
  // bytes[5] is flags; no flag is defined in v1, and an unknown one is not a
  // reason to refuse a file the tag will authenticate anyway.
  std::memcpy(out.nonce, bytes + 6, kGcmNonceLen);
  out.plaintextLen = readLe32(bytes + 18);
  out.ciphertextOffset = kManifestEnvelopeLen;
  const size_t sealed = fileLen - kManifestEnvelopeLen;
  if (sealed < kGcmTagLen) return false;
  out.ciphertextLen = sealed - kGcmTagLen;
  out.tagOffset = kManifestEnvelopeLen + out.ciphertextLen;
  // The header's promise has to match the file's shape before anything is
  // decrypted -- a mismatch here means the file is truncated or spliced.
  if (out.plaintextLen != out.ciphertextLen) return false;
  if (out.plaintextLen == 0) return false;
  out.valid = true;
  return true;
}

// ---------------------------------------------------------------------------
// The directional PIN
// ---------------------------------------------------------------------------

// Four symbols, 6 to 10 of them: 4^6 = 4,096 to 4^10 = 1,048,576. Not a lot, and
// it is not meant to be -- the PIN is not the key and not the strength. It gates
// a device already in the rider's hand, and the rate limiter is what makes
// guessing it impractical (../../../docs/wallet-crypto.md, "What the PIN is for").
inline constexpr size_t kPinMinLen = 6;
inline constexpr size_t kPinMaxLen = 10;
inline constexpr size_t kPinBufBytes = kPinMaxLen + 1;

enum class PinSymbol : uint8_t { Up = 0, Down = 1, Left = 2, Right = 3 };

// The byte a symbol contributes to the KDF password is its **ASCII letter**, not
// its enum value: `provision.json` spells the PIN "UDLRUD" and the password is
// "PIN bytes || deviceSecret" over exactly those characters. A phone app later has
// to send the same spelling.
inline char pinSymbolChar(PinSymbol symbol) {
  switch (symbol) {
    case PinSymbol::Up:
      return 'U';
    case PinSymbol::Down:
      return 'D';
    case PinSymbol::Left:
      return 'L';
    case PinSymbol::Right:
      return 'R';
  }
  return '?';
}

inline bool pinSymbolFromChar(char c, PinSymbol& out) {
  switch (c) {
    case 'U':
    case 'u':
      out = PinSymbol::Up;
      return true;
    case 'D':
    case 'd':
      out = PinSymbol::Down;
      return true;
    case 'L':
    case 'l':
      out = PinSymbol::Left;
      return true;
    case 'R':
    case 'r':
      out = PinSymbol::Right;
      return true;
    default:
      return false;
  }
}

// Normalises a PIN string to upper-case U/D/L/R of a legal length. False for
// anything else -- a PIN that is not exactly what was provisioned cannot unwrap
// the key anyway, and refusing it here gives a message instead of a failed
// attempt against the rate limiter.
inline bool normalisePin(const char* text, char* out, size_t outLen, size_t& length) {
  length = 0;
  if (text == nullptr || out == nullptr || outLen < kPinBufBytes) return false;
  size_t at = 0;
  for (; text[at] != '\0'; ++at) {
    if (at >= kPinMaxLen) return false;
    PinSymbol symbol = PinSymbol::Up;
    if (!pinSymbolFromChar(text[at], symbol)) return false;
    out[at] = pinSymbolChar(symbol);
  }
  out[at] = '\0';
  if (at < kPinMinLen) return false;
  length = at;
  return true;
}

// ---------------------------------------------------------------------------
// Rate limiting (brief section 44)
// ---------------------------------------------------------------------------

// After this many failures the wrap is locked until the device is provisioned
// again. There is no unlock-by-waiting past it: a directional PIN is 2^12 to 2^20
// guesses, so an attacker with unlimited tries wins, and the only defence that
// actually holds is a hard stop.
inline constexpr uint8_t kMaxPinFailures = 10;
// Failures below this cost nothing: a rider fat-fingering one press should not be
// punished.
inline constexpr uint8_t kFreePinFailures = 3;

// How long the next attempt has to wait, in milliseconds. Doubles per failure
// past the free ones, capped so the screen never looks hung.
//
// **This is a UX defence, not a cryptographic one.** It slows a person at the
// device. It does nothing against somebody who dumps NVS over USB and attacks the
// wrap offline -- that case is out of scope and the doc says so.
inline uint32_t pinFailureDelayMs(uint8_t failures) {
  if (failures <= kFreePinFailures) return 0;
  const uint8_t over = static_cast<uint8_t>(failures - kFreePinFailures);
  uint32_t delay = 1000;
  for (uint8_t i = 1; i < over && delay < 30000; ++i) delay *= 2;
  return delay > 30000 ? 30000 : delay;
}

inline bool pinIsLockedOut(uint8_t failures) { return failures >= kMaxPinFailures; }

// What one unlock attempt did. Shared by the PIN screen and by
// CMD:WALLETUNLOCK, because they run the same attempt through the same code
// (KeyStore::tryUnlock).
enum class UnlockResult : uint8_t {
  Ok = 0,
  NotProvisioned,  // no wrap in NVS: provision first
  Malformed,       // not 6..10 of U/D/L/R, so not spent against the limiter
  Waiting,         // the rate limiter's delay has not run out
  LockedOut,       // ten failures: the wrap is refused until re-provisioning
  BadPin,          // the unwrap did not authenticate
};

// A stable one-word token per outcome, for the serial reply and the log. Stable on
// purpose: a host script greps these, so they are part of the command's contract
// and renaming one breaks a caller.
inline const char* unlockResultName(UnlockResult result) {
  switch (result) {
    case UnlockResult::Ok:
      return "ok";
    case UnlockResult::NotProvisioned:
      return "not_provisioned";
    case UnlockResult::Malformed:
      return "malformed_pin";
    case UnlockResult::Waiting:
      return "rate_limited";
    case UnlockResult::LockedOut:
      return "locked_out";
    case UnlockResult::BadPin:
      return "wrong_pin";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// The wrapped key, as it sits in NVS
// ---------------------------------------------------------------------------

// nonce(12) | ciphertext(32) | tag(16). AES-256-GCM over K under the KEK, which
// is PBKDF2-HMAC-SHA256(PIN bytes || deviceSecret, salt, iters). K itself never
// touches the card and never touches NVS unwrapped.
inline constexpr size_t kWrappedKeyLen = kGcmNonceLen + kWalletKeyLen + kGcmTagLen;
inline constexpr size_t kDeviceSecretLen = 32;
inline constexpr size_t kPbkdf2SaltLen = 16;
inline constexpr size_t kKekPasswordBufBytes = kPinMaxLen + kDeviceSecretLen;

// The most PBKDF2 iterations a serial command will run in one go.
//
// mbedtls's PBKDF2 is one blocking call and the serial commands run on the main
// loop, where the task watchdog panics after 5 s (`sdkconfig.defaults`,
// `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`). 200,000 is well inside that at any rate that
// makes the format doc's 50,000 placeholder plausible, and the number anybody is
// looking for is the largest count under **1 s** -- so this cap cannot get in the
// way of the measurement it exists to make safe.
inline constexpr uint32_t kMaxProvisionIterations = 200000u;

// The KDF's password: PIN bytes then the device secret, in that order and with
// nothing between them.
//
// Separate from the derivation itself so the concatenation -- the part a second
// implementation gets wrong, and the part the laptop's `provision.json` describes
// in prose ("PIN bytes || deviceSecret") -- is host-testable. The PBKDF2 call over
// it is mbedtls on the device and OpenSSL in the tests.
inline bool buildKekPassword(const char* pin, size_t pinLen, const uint8_t* secret, size_t secretLen, uint8_t* out,
                             size_t outLen, size_t& length) {
  length = 0;
  if (pin == nullptr || secret == nullptr || out == nullptr) return false;
  if (pinLen < kPinMinLen || pinLen > kPinMaxLen) return false;
  if (secretLen != kDeviceSecretLen) return false;
  if (outLen < pinLen + secretLen) return false;
  std::memcpy(out, pin, pinLen);
  std::memcpy(out + pinLen, secret, secretLen);
  length = pinLen + secretLen;
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
