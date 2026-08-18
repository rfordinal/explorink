#pragma once

// Host-only reader for the .rle sidecar the generator writes beside every asset.
//
// The firmware does not decode EWRL in P1 -- it reads the .dat straight into the
// framebuffer -- so this lives under test/, not under src/. It exists because the
// sidecar is a third of the size of the .dat, which makes it the thing worth
// committing as a fixture and the thing that will travel over BLE later.
//
// Format, from tools/walletgen.py (parent repo) rle_encode()/build_sidecar():
//
//   the asset's 32-byte header, verbatim
//   "EWRL" | u8 version=1 | u8 bandRows | u16 bandCount | u32 rawLen
//   u32 compressedLen[bandCount]
//   the bands, in order
//
// A band is PackBits over bandRows physical rows:
//   op < 0x80  -> copy the next (op + 1) literal bytes
//   op >= 0x80 -> repeat the next byte (op - 0x80 + 2) times

#include <cstdint>
#include <cstring>
#include <vector>

#include "WalletAsset.h"

namespace wallet {
namespace host {

// Decodes the payload out of a full sidecar file. False for anything that is not
// a sidecar, or whose bands do not add up to the rawLen its header states.
inline bool decodeSidecarPayload(const std::vector<uint8_t>& blob, std::vector<uint8_t>& out) {
  out.clear();
  constexpr size_t kRleFixed = 12;
  if (blob.size() < kAssetHeaderBytes + kRleFixed) return false;
  const uint8_t* p = blob.data() + kAssetHeaderBytes;
  const size_t len = blob.size() - kAssetHeaderBytes;
  if (std::memcmp(p, "EWRL", 4) != 0) return false;
  if (p[4] != 1) return false;
  const uint16_t bandCount = readLe16(p + 6);
  const uint32_t rawLen = readLe32(p + 8);
  size_t at = kRleFixed;
  if (at + 4u * bandCount > len) return false;
  std::vector<uint32_t> bandLens(bandCount);
  for (uint16_t i = 0; i < bandCount; ++i) bandLens[i] = readLe32(p + at + 4u * i);
  at += 4u * bandCount;
  out.reserve(rawLen);
  for (uint16_t band = 0; band < bandCount; ++band) {
    const size_t end = at + bandLens[band];
    if (end > len) return false;
    while (at < end) {
      const uint8_t op = p[at++];
      if (op < 0x80) {
        const size_t count = static_cast<size_t>(op) + 1;
        if (at + count > end) return false;
        out.insert(out.end(), p + at, p + at + count);
        at += count;
      } else {
        const size_t count = static_cast<size_t>(op) - 0x80 + 2;
        if (at >= end) return false;
        out.insert(out.end(), count, p[at]);
        ++at;
      }
    }
  }
  return out.size() == rawLen;
}

}  // namespace host
}  // namespace wallet
