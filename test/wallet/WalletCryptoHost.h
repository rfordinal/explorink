#pragma once

// Host-side crypto for the wallet tests, through OpenSSL.
//
// The device uses mbedtls (WalletCryptoDevice.cpp) because the firmware already
// links it and the C3 accelerates its AES and SHA-256 in hardware. The host tests
// cannot link mbedtls, so they bind the SAME shared arithmetic -- the CTR IV, the
// offset maths, the envelope parse, the KEK password (WalletCrypto.h) -- to a
// different library.
//
// That is not a workaround, it is the strongest check available without hardware:
// two independent primitive implementations agreeing on one contract, against
// ciphertext a third implementation (Python `cryptography`, in the generator) wrote.
// What it does NOT cover is mbedtls's own calls, and that is stated in
// firmware docs/wallet-crypto.md, "What the host tests cannot reach".

#include <openssl/evp.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "WalletCrypto.h"

namespace wallet {
namespace host {

// The BlockEncrypt callback ctrXorInPlace() drives, backed by AES-256-ECB.
struct AesCtx {
  const uint8_t* key = nullptr;
};

inline void encryptBlock(void* ctx, const uint8_t in[16], uint8_t out[16]) {
  const AesCtx* aes = static_cast<const AesCtx*>(ctx);
  EVP_CIPHER_CTX* evp = EVP_CIPHER_CTX_new();
  int len = 0;
  EVP_EncryptInit_ex(evp, EVP_aes_256_ecb(), nullptr, aes->key, nullptr);
  EVP_CIPHER_CTX_set_padding(evp, 0);
  EVP_EncryptUpdate(evp, out, &len, in, 16);
  EVP_CIPHER_CTX_free(evp);
}

// AES-256-CTR in place at a payload offset, through the shared loop.
inline void ctrXor(const uint8_t key[kWalletKeyLen], const uint8_t iv[kAssetIvLen], uint32_t offset, uint8_t* data,
                   size_t len) {
  AesCtx ctx{key};
  ctrXorInPlace(encryptBlock, &ctx, iv, offset, data, len);
}

// AES-256-GCM decrypt with the tag verified. False means the tag failed, and the
// output must be treated as nothing.
inline bool gcmDecrypt(const uint8_t key[kWalletKeyLen], const uint8_t* nonce, size_t nonceLen, const uint8_t* tag,
                       size_t tagLen, const uint8_t* in, size_t inLen, uint8_t* out) {
  EVP_CIPHER_CTX* evp = EVP_CIPHER_CTX_new();
  int len = 0;
  bool ok = EVP_DecryptInit_ex(evp, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonceLen), nullptr) == 1 &&
            EVP_DecryptInit_ex(evp, nullptr, nullptr, key, nonce) == 1 &&
            EVP_DecryptUpdate(evp, out, &len, in, static_cast<int>(inLen)) == 1 &&
            EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tagLen), const_cast<uint8_t*>(tag)) == 1;
  int finalLen = 0;
  if (ok) ok = EVP_DecryptFinal_ex(evp, out + len, &finalLen) == 1;
  EVP_CIPHER_CTX_free(evp);
  return ok;
}

inline bool gcmEncrypt(const uint8_t key[kWalletKeyLen], const uint8_t* nonce, size_t nonceLen, const uint8_t* in,
                       size_t inLen, uint8_t* out, uint8_t* tag, size_t tagLen) {
  EVP_CIPHER_CTX* evp = EVP_CIPHER_CTX_new();
  int len = 0;
  bool ok = EVP_EncryptInit_ex(evp, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonceLen), nullptr) == 1 &&
            EVP_EncryptInit_ex(evp, nullptr, nullptr, key, nonce) == 1 &&
            EVP_EncryptUpdate(evp, out, &len, in, static_cast<int>(inLen)) == 1;
  int finalLen = 0;
  if (ok) ok = EVP_EncryptFinal_ex(evp, out + len, &finalLen) == 1;
  if (ok) ok = EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tagLen), tag) == 1;
  EVP_CIPHER_CTX_free(evp);
  return ok;
}

// PBKDF2-HMAC-SHA256, the KEK derivation the device runs with mbedtls.
inline bool pbkdf2(const uint8_t* password, size_t passwordLen, const uint8_t* salt, size_t saltLen,
                   uint32_t iterations, uint8_t out[kWalletKeyLen]) {
  return PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(password), static_cast<int>(passwordLen), salt,
                           static_cast<int>(saltLen), static_cast<int>(iterations), EVP_sha256(),
                           static_cast<int>(kWalletKeyLen), out) == 1;
}

}  // namespace host
}  // namespace wallet
