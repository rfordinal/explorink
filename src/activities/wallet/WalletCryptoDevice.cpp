#include "WalletCryptoDevice.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace wallet {

namespace {

constexpr const char* kLogTag = "WALLETCRY";

// The block-encrypt callback ctrXorInPlace() drives. One AES-ECB block, which on
// the C3 is the hardware accelerator through mbedtls.
void encryptBlock(void* ctx, const uint8_t in[kAesBlockLen], uint8_t out[kAesBlockLen]) {
  mbedtls_aes_crypt_ecb(static_cast<mbedtls_aes_context*>(ctx), MBEDTLS_AES_ENCRYPT, in, out);
}

}  // namespace

bool ctrDecryptInPlace(const uint8_t key[kWalletKeyLen], const uint8_t iv[kAssetIvLen], const uint32_t offset,
                       uint8_t* data, const size_t len) {
  if (key == nullptr || iv == nullptr || data == nullptr) return false;
  if (len == 0) return true;

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  // CTR only ever encrypts, in both directions: the keystream is the same and the
  // XOR is its own inverse. So this is setkey_enc even though it is a decrypt.
  if (mbedtls_aes_setkey_enc(&aes, key, static_cast<unsigned int>(kWalletKeyLen * 8)) != 0) {
    mbedtls_aes_free(&aes);
    LOG_ERR(kLogTag, "AES setkey failed");
    return false;
  }
  ctrXorInPlace(encryptBlock, &aes, iv, offset, data, len);
  mbedtls_aes_free(&aes);
  return true;
}

bool gcmDecryptInPlace(const uint8_t key[kWalletKeyLen], const uint8_t* nonce, const size_t nonceLen,
                       const uint8_t* tag, const size_t tagLen, uint8_t* data, const size_t len) {
  if (key == nullptr || nonce == nullptr || tag == nullptr || data == nullptr) return false;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, static_cast<unsigned int>(kWalletKeyLen * 8)) == 0;
  if (ok) {
    // No associated data: the generator seals with none, so the envelope around
    // this ciphertext is not covered by the tag (WalletCrypto.h).
    ok = mbedtls_gcm_auth_decrypt(&gcm, len, nonce, nonceLen, nullptr, 0, tag, tagLen, data, data) == 0;
  }
  mbedtls_gcm_free(&gcm);
  if (!ok) {
    // Wrong key, or somebody changed the file. Not distinguishable, and that is
    // GCM working as intended.
    LOG_ERR(kLogTag, "GCM tag did not verify over %u bytes", static_cast<unsigned>(len));
  }
  return ok;
}

bool gcmWrapKey(const uint8_t kek[kWalletKeyLen], const uint8_t key[kWalletKeyLen], uint8_t* out, const size_t outLen) {
  if (kek == nullptr || key == nullptr || out == nullptr || outLen < kWrappedKeyLen) return false;

  randomBytes(out, kGcmNonceLen);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, static_cast<unsigned int>(kWalletKeyLen * 8)) == 0;
  if (ok) {
    ok = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, kWalletKeyLen, out, kGcmNonceLen, nullptr, 0, key,
                                   out + kGcmNonceLen, kGcmTagLen, out + kGcmNonceLen + kWalletKeyLen) == 0;
  }
  mbedtls_gcm_free(&gcm);
  return ok;
}

bool gcmUnwrapKey(const uint8_t kek[kWalletKeyLen], const uint8_t* wrapped, const size_t wrappedLen,
                  uint8_t out[kWalletKeyLen]) {
  if (kek == nullptr || wrapped == nullptr || out == nullptr || wrappedLen != kWrappedKeyLen) return false;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, static_cast<unsigned int>(kWalletKeyLen * 8)) == 0;
  if (ok) {
    ok = mbedtls_gcm_auth_decrypt(&gcm, kWalletKeyLen, wrapped, kGcmNonceLen, nullptr, 0,
                                  wrapped + kGcmNonceLen + kWalletKeyLen, kGcmTagLen, wrapped + kGcmNonceLen, out) == 0;
  }
  mbedtls_gcm_free(&gcm);
  if (!ok) secureWipe(out, kWalletKeyLen);
  return ok;
}

bool deriveKek(const char* pin, const size_t pinLen, const uint8_t* deviceSecret, const size_t secretLen,
               const uint8_t* salt, const size_t saltLen, const uint32_t iterations, uint8_t out[kWalletKeyLen]) {
  if (pin == nullptr || deviceSecret == nullptr || salt == nullptr || out == nullptr) return false;
  if (pinLen == 0 || pinLen > kPinMaxLen || secretLen != kDeviceSecretLen || iterations == 0) return false;

  // password = PIN bytes || deviceSecret. The PIN alone is 2^12..2^20; the secret
  // is what makes the wrap worth attacking offline at all, and it never leaves the
  // device, so the laptop cannot compute this.
  uint8_t password[kKekPasswordBufBytes];
  size_t passwordLen = 0;
  if (!buildKekPassword(pin, pinLen, deviceSecret, secretLen, password, sizeof(password), passwordLen)) {
    secureWipe(password, sizeof(password));
    return false;
  }

  const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, password, passwordLen, salt, saltLen, iterations,
                                               static_cast<uint32_t>(kWalletKeyLen), out);
  // The password held the PIN; it does not get to sit on the stack afterwards.
  secureWipe(password, sizeof(password));
  if (rc != 0) {
    LOG_ERR(kLogTag, "PBKDF2 failed: %d", rc);
    secureWipe(out, kWalletKeyLen);
    return false;
  }
  return true;
}

void randomBytes(uint8_t* out, const size_t len) {
  if (out == nullptr || len == 0) return;
  // esp_fill_random is the hardware RNG. Seeded by the RF/ADC noise source; the
  // ESP-IDF contract is that it is fine for keys once the radio or the ADC has
  // been up, which it has by the time any wallet screen runs.
  esp_fill_random(out, len);
}

uint32_t timePbkdf2Micros(const uint32_t iterations) {
  // A fixed, meaningless password and salt: the cost of PBKDF2 does not depend on
  // their content, only on the iteration count and the digest.
  uint8_t password[kPinMaxLen + kDeviceSecretLen] = {0};
  uint8_t salt[kPbkdf2SaltLen] = {0};
  uint8_t out[kWalletKeyLen];
  const uint32_t started = micros();
  const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, password, sizeof(password), salt, sizeof(salt),
                                               iterations, static_cast<uint32_t>(sizeof(out)), out);
  const uint32_t elapsed = micros() - started;
  secureWipe(out, sizeof(out));
  return rc == 0 ? elapsed : 0;
}

Session& Session::instance() {
  static Session session;
  return session;
}

void Session::setKey(const uint8_t key[kWalletKeyLen]) {
  if (key == nullptr) return;
  std::memcpy(key_, key, kWalletKeyLen);
  hasKey_ = true;
  lastTouchMs_ = millis();
  LOG_INF(kLogTag, "wallet unlocked; key held for %u s of idle", static_cast<unsigned>(kIdleTimeoutMs / 1000));
}

void Session::clear(const char* reason) {
  if (hasKey_) LOG_INF(kLogTag, "wallet locked (%s)", reason != nullptr ? reason : "?");
  secureWipe(key_, sizeof(key_));
  hasKey_ = false;
  lastTouchMs_ = 0;
}

void Session::touch() { lastTouchMs_ = millis(); }

bool Session::expireIfIdle() {
  if (!hasKey_) return false;
  // millis() wraps after 49 days; the subtraction is unsigned, so a wrap makes the
  // difference large and the key dies early. Erring towards locked is the right
  // direction for this particular bug.
  if (millis() - lastTouchMs_ < kIdleTimeoutMs) return false;
  clear("idle");
  return true;
}

}  // namespace wallet
