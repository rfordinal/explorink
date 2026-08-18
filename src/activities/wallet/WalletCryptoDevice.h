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

// AES-256-GCM decrypt, tag verified **before** the caller sees a plaintext byte.
// In place is allowed and is what the manifest read does: mbedtls refuses only a
// partial overlap, not an identical pointer (mbedtls/library/gcm.c, the
// `output > input` guard).
bool gcmDecryptInPlace(const uint8_t key[kWalletKeyLen], const uint8_t* nonce, size_t nonceLen, const uint8_t* tag,
                       size_t tagLen, uint8_t* data, size_t len);

// AES-256-GCM encrypt, for wrapping the wallet key. `out` is
// nonce || ciphertext || tag, kWrappedKeyLen bytes.
bool gcmWrapKey(const uint8_t kek[kWalletKeyLen], const uint8_t key[kWalletKeyLen], uint8_t* out, size_t outLen);
bool gcmUnwrapKey(const uint8_t kek[kWalletKeyLen], const uint8_t* wrapped, size_t wrappedLen,
                  uint8_t out[kWalletKeyLen]);

// KEK = PBKDF2-HMAC-SHA256(PIN bytes || deviceSecret, salt, iterations).
bool deriveKek(const char* pin, size_t pinLen, const uint8_t* deviceSecret, size_t secretLen, const uint8_t* salt,
               size_t saltLen, uint32_t iterations, uint8_t out[kWalletKeyLen]);

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
  // True when it cleared the key. Checked from the wallet screens' loop().
  bool expireIfIdle();

  static constexpr uint32_t kIdleTimeoutMs = 120u * 1000u;

 private:
  Session() = default;
  uint8_t key_[kWalletKeyLen] = {0};
  bool hasKey_ = false;
  uint32_t lastTouchMs_ = 0;
};

}  // namespace wallet
