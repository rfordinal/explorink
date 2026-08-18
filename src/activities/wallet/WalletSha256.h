#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// SHA-256, and the one question the wallet asks it: do these payload bytes hash
// to what the manifest said they would?
//
// ## Why not mbedtls
//
// The firmware already links mbedtls and uses `mbedtls_sha256_*` for the OTA
// image (src/network/FirmwareFlasher.cpp:8, :129-224). This is a second
// implementation on purpose, for one reason: the host tests do not link mbedtls,
// and the hash-mismatch path is exactly the path that must be *proved to fire*
// against real generator bytes (test/wallet, WalletCodeHash.*). A verify step
// nobody can test on the host is a verify step nobody has tested.
//
// The cost is ~1 KB of flash and one more implementation to be wrong. Pinned
// against the FIPS-180-4 vectors in the host test, so being wrong is caught
// rather than assumed.
//
// Header-only for the same reason WalletAsset.h is: pure byte work, no Storage,
// no renderer, no globals, so test/wallet and test/wallet_preview link it
// without dragging the firmware in.

namespace wallet {

inline constexpr size_t kSha256Bytes = 32;
inline constexpr size_t kSha256HexLen = 64;
inline constexpr size_t kSha256HexBufBytes = kSha256HexLen + 1;
// The asset header carries only the first 8 bytes (WalletAsset.h, AssetHeader).
inline constexpr size_t kSha256PrefixBytes = 8;

class Sha256 {
 public:
  Sha256() { reset(); }

  void reset() {
    len_ = 0;
    fill_ = 0;
    h_[0] = 0x6a09e667u;
    h_[1] = 0xbb67ae85u;
    h_[2] = 0x3c6ef372u;
    h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu;
    h_[5] = 0x9b05688cu;
    h_[6] = 0x1f83d9abu;
    h_[7] = 0x5be0cd19u;
  }

  void update(const uint8_t* data, size_t len) {
    if (data == nullptr) return;
    len_ += len;
    while (len > 0) {
      const size_t take = (sizeof(block_) - fill_) < len ? (sizeof(block_) - fill_) : len;
      std::memcpy(block_ + fill_, data, take);
      fill_ += take;
      data += take;
      len -= take;
      if (fill_ == sizeof(block_)) {
        compress(block_);
        fill_ = 0;
      }
    }
  }

  // Finishes into `out`. The object is left used-up; call reset() to hash again.
  void finish(uint8_t out[kSha256Bytes]) {
    const uint64_t bits = len_ * 8u;
    static const uint8_t kPad = 0x80;
    update(&kPad, 1);
    static const uint8_t kZero = 0;
    while (fill_ != 56) update(&kZero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    // Straight into the block: update() would add these 8 bytes to len_, which
    // is already baked into `bits`.
    std::memcpy(block_ + 56, tail, sizeof(tail));
    compress(block_);
    for (int i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
  }

 private:
  static uint32_t ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

  void compress(const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + s1 + ch + k[i] + w[i];
      const uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }

  uint32_t h_[8] = {0};
  uint8_t block_[64] = {0};
  size_t fill_ = 0;
  uint64_t len_ = 0;
};

inline bool hexNibble(char c, uint8_t& out) {
  if (c >= '0' && c <= '9') {
    out = static_cast<uint8_t>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    out = static_cast<uint8_t>(c - 'a' + 10);
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    out = static_cast<uint8_t>(c - 'A' + 10);
    return true;
  }
  return false;
}

// Exactly 64 hex characters, or false. A short, long or non-hex string is not a
// hash and must never be treated as one -- silently accepting a truncated hash
// would turn the verify step into a formality.
inline bool sha256FromHex(const char* hex, uint8_t out[kSha256Bytes]) {
  if (hex == nullptr) return false;
  size_t i = 0;
  for (; hex[i] != '\0'; ++i) {
    if (i >= kSha256HexLen) return false;
    uint8_t nibble = 0;
    if (!hexNibble(hex[i], nibble)) return false;
    if ((i % 2) == 0) {
      out[i / 2] = static_cast<uint8_t>(nibble << 4);
    } else {
      out[i / 2] = static_cast<uint8_t>(out[i / 2] | nibble);
    }
  }
  return i == kSha256HexLen;
}

// Which authority the payload was checked against. The manifest's full 32-byte
// hash when it carries one; the asset header's 8-byte prefix otherwise -- a
// manifest written before the field existed, or by something other than the
// generator, still gets 64 bits of corruption cover.
enum class HashAuthority : uint8_t { Full, Prefix };

struct HashResult {
  bool ok = false;
  HashAuthority authority = HashAuthority::Prefix;
};

// Hashes `len` bytes and compares. **This detects corruption, not tampering**:
// anybody who can write the manifest can write the payload beside it, and P1
// ships no signature. The point is that a half-written card or a bad sector
// cannot put a wrong barcode in front of a gate agent.
inline HashResult checkPayloadHash(const uint8_t* payload, size_t len, const char* expectedHex,
                                   const uint8_t headerPrefix[kSha256PrefixBytes]) {
  HashResult result;
  if (payload == nullptr || len == 0) return result;

  Sha256 sha;
  sha.update(payload, len);
  uint8_t digest[kSha256Bytes];
  sha.finish(digest);

  uint8_t expected[kSha256Bytes];
  if (sha256FromHex(expectedHex, expected)) {
    result.authority = HashAuthority::Full;
    result.ok = std::memcmp(digest, expected, sizeof(digest)) == 0;
    return result;
  }
  result.authority = HashAuthority::Prefix;
  if (headerPrefix == nullptr) return result;
  result.ok = std::memcmp(digest, headerPrefix, kSha256PrefixBytes) == 0;
  return result;
}

}  // namespace wallet
