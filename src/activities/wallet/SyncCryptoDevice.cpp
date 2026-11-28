#include "SyncCryptoDevice.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <cstring>

namespace syncdevice {

namespace {

constexpr const char* kLogTag = "SYNC";

// mbedtls 3.6's Montgomery ladder refuses a NULL RNG outright (ecp.c,
// ecp_mul_mxz: `if (f_rng == NULL) return MBEDTLS_ERR_ECP_BAD_INPUT_DATA`): it
// randomises the projective coordinates against side channels. esp_fill_random
// is the hardware RNG the wallet already draws its keys from.
int rng(void*, unsigned char* out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

// X25519(k, u) per RFC 7748. mbedtls does not clamp for us: ecp_mul checks the
// scalar with mbedtls_ecp_check_privkey and **refuses** an unclamped one (bits 0-2
// set, or bit 254 clear -- ecp.c, "see RFC 7748 sec. 5 para. 5"). RFC 7748's
// decodeScalar25519 clamps every scalar, and Noise private keys are any 32 bytes
// (the cacophony vectors use unclamped ones), so the clamp happens here on a copy.
bool x25519(const uint8_t priv[32], const uint8_t* pub, uint8_t out[32]) {
  uint8_t k[32];
  std::memcpy(k, priv, 32);
  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;

  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point q;
  mbedtls_ecp_point r;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&q);
  mbedtls_ecp_point_init(&r);

  int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
  if (rc == 0) rc = mbedtls_mpi_read_binary_le(&d, k, 32);
  if (rc == 0) {
    // nullptr means the base point: u = 9.
    rc = pub == nullptr ? mbedtls_ecp_copy(&q, &grp.G) : mbedtls_ecp_point_read_binary(&grp, &q, pub, 32);
  }
  // check_pubkey inside ecp_mul also refuses the known low-order points
  // (ecp.c, ecp_check_bad_points_mx), so most of what the zero check below
  // catches is refused here first. Both stay: the zero check is the one the spec
  // names and the host binding shares.
  if (rc == 0) rc = mbedtls_ecp_mul(&grp, &r, &d, &q, rng, nullptr);
  size_t olen = 0;
  if (rc == 0) rc = mbedtls_ecp_point_write_binary(&grp, &r, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, out, 32);

  mbedtls_ecp_point_free(&r);
  mbedtls_ecp_point_free(&q);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  syncnoise::wipe(k, sizeof(k));

  if (rc != 0 || olen != 32) {
    LOG_ERR(kLogTag, "x25519 failed: -0x%04x", static_cast<unsigned>(-rc));
    return false;
  }
  return true;
}

bool dh(const uint8_t priv[32], const uint8_t pub[32], uint8_t out[32]) {
  if (!x25519(priv, pub, out)) return false;
  uint8_t acc = 0;
  for (int i = 0; i < 32; ++i) acc |= out[i];
  return acc != 0;
}

bool pubFromPriv(const uint8_t priv[32], uint8_t pub[32]) { return x25519(priv, nullptr, pub); }

bool aeadSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen, const uint8_t* pt,
              size_t len, uint8_t* out) {
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0)
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, 12, ad, adLen, pt, out, 16, out + len);
  mbedtls_gcm_free(&gcm);
  return rc == 0;
}

bool aeadOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen, const uint8_t* ct,
              size_t ctLen, uint8_t* out) {
  if (ctLen < 16) return false;
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&gcm, ctLen - 16, nonce, 12, ad, adLen, ct + ctLen - 16, 16, ct, out);
  mbedtls_gcm_free(&gcm);
  return rc == 0;
}

bool sha256(const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  int rc = mbedtls_sha256_starts(&ctx, 0);
  if (rc == 0 && aLen > 0) rc = mbedtls_sha256_update(&ctx, a, aLen);
  if (rc == 0 && bLen > 0) rc = mbedtls_sha256_update(&ctx, b, bLen);
  if (rc == 0) rc = mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
  return rc == 0;
}

bool hmacSha256(const uint8_t key[32], const uint8_t* data, size_t len, uint8_t out[32]) {
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  static const uint8_t kEmpty = 0;
  return md != nullptr && mbedtls_md_hmac(md, key, 32, len > 0 ? data : &kEmpty, len, out) == 0;
}

bool random(uint8_t* out, size_t len) {
  esp_fill_random(out, len);
  return true;
}

uint8_t fromHex(char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); }

void hexTo(const char* hex, uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>((fromHex(hex[2 * i]) << 4) | fromHex(hex[2 * i + 1]));
}

}  // namespace

const syncnoise::Crypto& crypto() {
  static const syncnoise::Crypto c{&dh, &pubFromPriv, &aeadSeal, &aeadOpen, &sha256, &hmacSha256, &random};
  return c;
}

bool selfTest(uint32_t& handshakeMicros, uint32_t& heapBefore, uint32_t& heapMinDuring) {
  // cacophony.txt, Noise_IK_25519_AESGCM_SHA256 -- the same vector the host suite
  // runs (test/sync_channel/SyncNoiseTest.cpp). Responder side only: that is the
  // device's role, and message 1 is the published initiator's bytes.
  static constexpr char kPrologue[] = "4a6f686e2047616c74";
  static constexpr char kRespStatic[] = "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893";
  static constexpr char kRespEphemeral[] = "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b";
  static constexpr char kMsg1[] =
      "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79444e417bc55c7a8166c993356c1be41ef67818a292426f3015"
      "56c7f26b21d25ddb097153891a9a956cff47b83e63ad8d701c1342c209cff1ca5ecd43402762ac249e3bd3a4c0a145fe07cb5dae28ea13a"
      "3";
  static constexpr char kPayload1[] = "Ludwig von Mises";
  static constexpr char kPayload2[] = "Murray Rothbard";
  static constexpr char kMsg2[] =
      "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843af2ccf9972e22afc67aeafcd25162f7f98c363b7762e3e4c"
      "b7d272e39f27a5";
  static constexpr char kHash[] = "669c8640d9e42a3cda2f232f78597ceefb01daa6e3df81181ccce6fc6b5026bf";

  uint8_t prologue[9], s[32], e[32], msg1[(sizeof(kMsg1) - 1) / 2], msg2[(sizeof(kMsg2) - 1) / 2], hash[32];
  hexTo(kPrologue, prologue, sizeof(prologue));
  hexTo(kRespStatic, s, 32);
  hexTo(kRespEphemeral, e, 32);
  hexTo(kMsg1, msg1, sizeof(msg1));
  hexTo(kMsg2, msg2, sizeof(msg2));
  hexTo(kHash, hash, 32);

  heapBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  heap_caps_monitor_local_minimum_free_size_start();

  // The Handshake is ~400 bytes; on the heap rather than this task's stack
  // (firmware CLAUDE.md, "Stack Safety").
  auto hs = makeUniqueNoThrow<syncnoise::Handshake>();
  auto send = makeUniqueNoThrow<syncnoise::CipherState>();
  auto recv = makeUniqueNoThrow<syncnoise::CipherState>();
  bool ok = hs && send && recv;
  if (!ok) LOG_ERR(kLogTag, "OOM: self-test state");

  uint8_t payload[32];
  uint8_t out[sizeof(msg2)];
  size_t n = 0;
  const uint32_t t0 = micros();
  ok = ok && hs->initialize(&crypto(), syncnoise::Pattern::IK, syncnoise::Role::Responder, prologue, sizeof(prologue),
                            s, nullptr, nullptr, e);
  ok = ok && hs->readMessage1(msg1, sizeof(msg1), payload, sizeof(payload), n);
  ok = ok && n == sizeof(kPayload1) - 1 && std::memcmp(payload, kPayload1, n) == 0;
  ok = ok && hs->writeMessage2(reinterpret_cast<const uint8_t*>(kPayload2), sizeof(kPayload2) - 1, out, sizeof(out), n);
  ok = ok && n == sizeof(msg2) && std::memcmp(out, msg2, n) == 0;
  ok = ok && hs->split(*send, *recv);
  handshakeMicros = micros() - t0;
  ok = ok && std::memcmp(hs->handshakeHash(), hash, 32) == 0;

  heapMinDuring = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  heap_caps_monitor_local_minimum_free_size_stop();
  syncnoise::wipe(s, sizeof(s));
  syncnoise::wipe(e, sizeof(e));
  LOG_INF(kLogTag, "self-test %s: responder handshake %lu us, heap %lu free before, %lu minimum during",
          ok ? "PASS" : "FAIL", static_cast<unsigned long>(handshakeMicros), static_cast<unsigned long>(heapBefore),
          static_cast<unsigned long>(heapMinDuring));
  return ok;
}

}  // namespace syncdevice
