#pragma once

#include <cstddef>
#include <cstdint>

#include "WalletCrypto.h"

// The device half of wallet crypto v1: mbedtls, and the session key.
//
// Everything here needs a library or the chip. The arithmetic that decides *where*
// a decrypt starts is in WalletCrypto.h and is shared with the host tests; this
// file is the primitives under it.
//
// mbedtls rather than a second implementation on purpose: the firmware already
// links it for the OTA image (src/network/FirmwareFlasher.cpp), and on the C3 its
// SHA-256 and AES run on the hardware accelerators. The consequence is that the
// host tests cannot exercise *these* functions -- they bind the same shared
// arithmetic to OpenSSL instead. What that leaves unverified is written down
// (../../../docs/wallet-crypto.md, "What the host tests cannot reach").
namespace wallet {

// AES-256-CTR over a buffer already in place, starting at payload byte `offset`.
// The one call the asset read path makes.
bool ctrDecryptInPlace(const uint8_t key[kWalletKeyLen], const uint8_t iv[kAssetIvLen], uint32_t offset, uint8_t* data,
                       size_t len);

// Why a GCM decrypt failed, which is **not** one question.
//
// A wrong key and an altered file are genuinely indistinguishable and both are
// AuthFailed -- that is GCM working as intended. An allocation failure inside
// the chip's AES driver is a third thing entirely, and lumping it in with the
// other two puts "wrong key, or the wallet list was altered" on a rider's panel
// when the truth is "this board had no memory for it".
//
// Measured on an X3, 2026-11-27: a 22,528-byte manifest read back with BLE up
// logged `E esp-aes: Failed to allocate memory` and failed as a tag mismatch.
// The separation is exact rather than a guess -- the ESP-IDF port returns
// MBEDTLS_ERR_GCM_AUTH_FAILED only for a real tag difference
// (`components/mbedtls/port/aes/esp_aes_gcm.c`, esp_aes_gcm_auth_decrypt) and
// propagates everything else unchanged -- the allocation failure itself is the
// `return -1` in `components/mbedtls/port/aes/dma/esp_aes_dma_core.c`.
enum class GcmResult : uint8_t {
  Ok = 0,
  AuthFailed,  // the tag did not match: wrong key, or somebody changed the file
  Failed,      // the primitive itself refused -- out of memory, bad parameters
};

// AES-256-GCM decrypt, tag verified **before** the caller sees a plaintext byte.
// In place is allowed and is what the manifest read does: mbedtls refuses only a
// partial overlap, not an identical pointer (mbedtls/library/gcm.c, the
// `output > input` guard).
GcmResult gcmDecryptInPlace(const uint8_t key[kWalletKeyLen], const uint8_t* nonce, size_t nonceLen, const uint8_t* tag,
                            size_t tagLen, uint8_t* data, size_t len);

// AES-256-GCM seal of a buffer in place, with the tag written out separately.
// The manifest's write half: `data` goes in as plaintext and comes out as
// ciphertext of the same length, `tag` gets the 16 bytes that authenticate it.
//
// **One shot, deliberately, and not because mbedtls cannot stream.** It can --
// `mbedtls_gcm_update()` buffers in whole blocks and the pinned ESP-IDF
// (5.5.2.260206) documents `output_size >= input_length + 15` as always
// sufficient. A streamed seal would accept a manifest of any size, and this same
// device refuses to *read* one above kMaxEncryptedManifestBytes. The plaintext
// is therefore buffered and bounded first (WalletCrypto.h,
// kMaxManifestPlaintextBytes), which is what makes a one-shot call the right
// shape rather than a limitation.
//
// The nonce is the caller's: it goes in the cleartext envelope, so the caller is
// the one that has to write the same bytes into both places.
bool gcmEncryptInPlace(const uint8_t key[kWalletKeyLen], const uint8_t* nonce, size_t nonceLen, uint8_t* data,
                       size_t len, uint8_t* tag, size_t tagLen);

// AES-256-GCM encrypt, for wrapping the wallet key. `out` is
// nonce || ciphertext || tag, kWrappedKeyLen bytes.
bool gcmWrapKey(const uint8_t kek[kWalletKeyLen], const uint8_t key[kWalletKeyLen], uint8_t* out, size_t outLen);
bool gcmUnwrapKey(const uint8_t kek[kWalletKeyLen], const uint8_t* wrapped, size_t wrappedLen,
                  uint8_t out[kWalletKeyLen]);

// KEK = PBKDF2-HMAC-SHA256(pattern bytes || deviceSecret, salt, iterations).
bool deriveKek(const char* pattern, size_t patternLen, const uint8_t* deviceSecret, size_t secretLen,
               const uint8_t* salt, size_t saltLen, uint32_t iterations, uint8_t out[kWalletKeyLen]);

// Hardware RNG. Used for the device secret, the salt and every wrap nonce.
void randomBytes(uint8_t* out, size_t len);

// How long `iterations` of PBKDF2-HMAC-SHA256 take on this chip, in
// microseconds. Exists because the format doc's iteration count was a guess off a
// laptop rate and had to be replaced by a measurement (CMD:WALLETPBKDF2).
uint32_t timePbkdf2Micros(uint32_t iterations);

// The wallet key while the wallet is unlocked, and nowhere else.
//
// One process-wide holder rather than a member of an activity: the browse screen,
// the viewer and the code screen all need it, they are pushed on top of each other,
// and the key must survive moving between them but not survive sleep. Every exit
// path calls clear() explicitly -- a destructor that runs "eventually" is not a
// zeroize (../../../docs/wallet-crypto.md, "The key's lifetime").
class Session {
 public:
  static Session& instance();

  bool hasKey() const { return hasKey_; }
  // The key, for the read path. nullptr when locked.
  const uint8_t* key() const { return hasKey_ ? key_ : nullptr; }

  void setKey(const uint8_t key[kWalletKeyLen]);
  // Explicit wipe: overwrites the bytes through a volatile pointer so the write
  // cannot be optimised away, then drops the flag.
  void clear(const char* reason);

  // Called by every wallet screen on every frame it draws. The key dies after
  // kIdleTimeoutMs of no wallet screen touching it, so a device left on a table
  // unlocked locks itself.
  void touch();
  // Milliseconds until the next unlock attempt is allowed, 0 when it may go now.
  uint32_t retryWaitMs() const;
  // Milliseconds of idle left before the key dies, 0 when there is no key.
  uint32_t idleLeftMs() const;
  // Arms the rate limiter's delay for `failures` consecutive failures. Lives here
  // rather than in the gate screen so the screen and CMD:WALLETUNLOCK share one gate
  // -- otherwise a host command could step around a delay the screen is enforcing.
  void armRetryDelay(uint8_t failures);
  // True when it cleared the key. Checked from the wallet screens' loop().
  bool expireIfIdle();

  static constexpr uint32_t kIdleTimeoutMs = 120u * 1000u;

 private:
  Session() = default;
  uint8_t key_[kWalletKeyLen] = {0};
  bool hasKey_ = false;
  uint32_t lastTouchMs_ = 0;
  // When the next unlock attempt may run, and what it was armed for. RAM only:
  // the *count* is persisted, and the deadline is armed from it when an unlock
  // path starts after a reboot, so a power cycle does not hand out a free fast
  // retry. The arithmetic is `wallet::RetryGate` in WalletCrypto.h, host-tested,
  // because the bug it carries was in the arming rule and not in the table.
  RetryGate retry_;
};

}  // namespace wallet
