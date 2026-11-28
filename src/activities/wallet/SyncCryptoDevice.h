#pragma once

#include "SyncNoise.h"

// SyncNoise.h's primitives bound to mbedtls, the device half of the pair whose
// host half is test/sync_channel/SyncCryptoHost.h (OpenSSL). Every primitive is
// already linked for the wallet or for TLS: X25519 through the generic ECP code
// (`CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED`), AES-GCM, SHA-256 and HMAC.
// Nothing new comes in (docs/sync-channel.md, "The handshake").

namespace syncdevice {

const syncnoise::Crypto& crypto();

// Runs the published Noise_IK vector through these primitives and reports
// whether every byte matched, plus how long the two handshake messages took on
// the responder side -- the one number the host cannot give (sync-channel.md,
// "Costs and risks", item 2). `CMD:SYNCSELFTEST`, bench builds only.
bool selfTest(uint32_t& handshakeMicros, uint32_t& heapBefore, uint32_t& heapMinDuring);

}  // namespace syncdevice
