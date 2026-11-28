#pragma once

// Host-side primitives for the sync channel tests, through OpenSSL.
//
// Same reasoning as test/wallet/WalletCryptoHost.h: the device binds
// SyncNoise.h to mbedtls (src/activities/wallet/SyncCryptoDevice.cpp), the host
// to OpenSSL, and the published cacophony vectors decide between them. What this
// file cannot reach is mbedtls itself; that half is checked by
// CMD:SYNCSELFTEST on a board (docs/sync-channel.md, "What the host cannot
// reach").

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstring>

#include "SyncNoise.h"

namespace synchost {

inline bool dh(const uint8_t priv[32], const uint8_t pub[32], uint8_t out[32]) {
  EVP_PKEY* mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
  EVP_PKEY* theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, pub, 32);
  EVP_PKEY_CTX* ctx = mine ? EVP_PKEY_CTX_new(mine, nullptr) : nullptr;
  size_t len = 32;
  bool ok = ctx && theirs && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, theirs) == 1 &&
            EVP_PKEY_derive(ctx, out, &len) == 1 && len == 32;
  EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(mine);
  EVP_PKEY_free(theirs);
  if (ok) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i) acc |= out[i];
    ok = acc != 0;
  }
  return ok;
}

inline bool pubFromPriv(const uint8_t priv[32], uint8_t pub[32]) {
  EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
  size_t len = 32;
  const bool ok = k && EVP_PKEY_get_raw_public_key(k, pub, &len) == 1 && len == 32;
  EVP_PKEY_free(k);
  return ok;
}

inline bool gcm(bool enc, const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen,
                const uint8_t* in, size_t len, uint8_t* out, uint8_t* tag) {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int n = 0;
  bool ok = c && (enc ? EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, key, nonce)
                      : EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, key, nonce)) == 1;
  if (ok && adLen > 0) ok = EVP_CipherUpdate(c, nullptr, &n, ad, static_cast<int>(adLen)) == 1;
  if (ok && len > 0) ok = EVP_CipherUpdate(c, out, &n, in, static_cast<int>(len)) == 1;
  if (ok && !enc) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1;
  uint8_t tail[16];
  if (ok) ok = EVP_CipherFinal_ex(c, tail, &n) == 1;
  if (ok && enc) ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
  EVP_CIPHER_CTX_free(c);
  return ok;
}

inline bool aeadSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen, const uint8_t* pt,
                     size_t len, uint8_t* out) {
  return gcm(true, key, nonce, ad, adLen, pt, len, out, out + len);
}

inline bool aeadOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen, const uint8_t* ct,
                     size_t ctLen, uint8_t* out) {
  if (ctLen < 16) return false;
  uint8_t tag[16];
  std::memcpy(tag, ct + ctLen - 16, 16);
  return gcm(false, key, nonce, ad, adLen, ct, ctLen - 16, out, tag);
}

inline bool sha256(const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen, uint8_t out[32]) {
  EVP_MD_CTX* c = EVP_MD_CTX_new();
  unsigned int n = 0;
  const bool ok = c && EVP_DigestInit_ex(c, EVP_sha256(), nullptr) == 1 &&
                  (aLen == 0 || EVP_DigestUpdate(c, a, aLen) == 1) &&
                  (bLen == 0 || EVP_DigestUpdate(c, b, bLen) == 1) && EVP_DigestFinal_ex(c, out, &n) == 1 && n == 32;
  EVP_MD_CTX_free(c);
  return ok;
}

inline bool hmacSha256(const uint8_t key[32], const uint8_t* data, size_t len, uint8_t out[32]) {
  unsigned int outLen = 0;
  static const uint8_t kEmpty = 0;
  return HMAC(EVP_sha256(), key, 32, len > 0 ? data : &kEmpty, len, out, &outLen) != nullptr && outLen == 32;
}

inline bool random(uint8_t* out, size_t len) { return RAND_bytes(out, static_cast<int>(len)) == 1; }

inline const syncnoise::Crypto& crypto() {
  static const syncnoise::Crypto c{&dh, &pubFromPriv, &aeadSeal, &aeadOpen, &sha256, &hmacSha256, &random};
  return c;
}

}  // namespace synchost
