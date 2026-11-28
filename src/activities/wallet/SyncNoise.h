#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Noise, the two patterns the sync channel uses, and nothing else (T-2098).
//
//   Noise_IKpsk2_25519_AESGCM_SHA256   the first meeting: pairing by QR code
//   Noise_IK_25519_AESGCM_SHA256       every sync after it
//
// Spec: "The Noise Protocol Framework", revision 34 (noiseprotocol.org). The
// decision behind the choice is docs/sync-channel.md.
//
// ## Why this is written out and not a library
//
// The plan's rule is "a named implementation or a transcript-checked one, never
// hand-composed". noise-c (MIT) is the named one and it is a framework: every
// pattern, three curves, four ciphers, two hashes, its own crypto backends. We
// need one curve, one cipher, one hash and two token sequences that differ by two
// lines, and the primitives are already linked through mbedtls. So this is the
// transcript-checked option: the spec's SymmetricState and HandshakeState with the
// two token sequences spelled out, **checked byte for byte against the published
// cacophony vectors** for both protocol names (test/sync_channel) and against a
// second, independent implementation (Python `noiseprotocol`,
// scripts/sync_channel_phone.py). Nothing here is a design of ours: every function
// below is named after the spec function it is, and the order of every MixKey and
// MixHash is the spec's, not a choice.
//
// A generic pattern interpreter was rejected for the same reason: it is more code
// to review than the two sequences it would interpret.
//
// ## The primitives are injected
//
// Same shape as WalletCrypto.h: the device binds them to mbedtls
// (SyncCryptoDevice.cpp), the host tests to OpenSSL (test/sync_channel). Two
// libraries agreeing on a vector file is the check; a header that called either
// directly could only be tested against itself.
//
// Pure: no Arduino, no mbedtls, no heap. Every buffer is the caller's.

namespace syncnoise {

inline constexpr size_t kKeyLen = 32;
inline constexpr size_t kHashLen = 32;
inline constexpr size_t kDhLen = 32;
inline constexpr size_t kTagLen = 16;
inline constexpr size_t kNonceLen = 12;

// Noise's own ceiling on any message, handshake or transport. Ours are far
// smaller (one BLE write); the check is here so a caller cannot ask for more.
inline constexpr size_t kMaxMessageLen = 65535;

struct Crypto {
  // X25519 per RFC 7748: the scalar is clamped by the implementation, so a
  // private key is any 32 bytes. False on any library failure **and** on an
  // all-zero result -- a low-order public key forces that result whatever our
  // private key is, and a handshake that continued with it would derive keys an
  // attacker knows. WireGuard and libsodium refuse it the same way.
  bool (*dh)(const uint8_t priv[kDhLen], const uint8_t pub[kDhLen], uint8_t out[kDhLen]) = nullptr;
  // The public key for `priv`: X25519(priv, 9).
  bool (*pubFromPriv)(const uint8_t priv[kDhLen], uint8_t pub[kDhLen]) = nullptr;
  // AES-256-GCM. `out` receives len + kTagLen bytes, the tag last, which is the
  // spec's ciphertext layout. In-place (`out == pt`) must work.
  bool (*aeadSeal)(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* ad, size_t adLen,
                   const uint8_t* pt, size_t len, uint8_t* out) = nullptr;
  // Opens `ctLen` bytes, tag included; `out` receives ctLen - kTagLen bytes.
  // False on a tag failure, and then `out` holds nothing usable.
  bool (*aeadOpen)(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* ad, size_t adLen,
                   const uint8_t* ct, size_t ctLen, uint8_t* out) = nullptr;
  // SHA-256 over a || b. Two parts because MixHash is always HASH(h || data),
  // and copying h and data into one buffer first would need a buffer sized for
  // the largest message.
  bool (*sha256)(const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen, uint8_t out[kHashLen]) = nullptr;
  // HMAC-SHA256. HKDF below is built from it exactly as the spec writes it.
  bool (*hmacSha256)(const uint8_t key[kHashLen], const uint8_t* data, size_t len, uint8_t out[kHashLen]) = nullptr;
  // Fills an ephemeral private key. Never called when the caller pins one, which
  // only the vector tests do.
  bool (*random)(uint8_t* out, size_t len) = nullptr;
};

// Clears secrets. `volatile` so the store is not optimised away as dead -- the
// buffers wiped here are usually about to go out of scope, which is exactly
// when a compiler drops a plain memset.
inline void wipe(void* p, size_t len) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  while (len-- > 0) *v++ = 0;
}

// --- CipherState (spec 5.1) -------------------------------------------------

class CipherState {
 public:
  ~CipherState() { clear(); }

  void initializeKey(const uint8_t key[kKeyLen]) {
    std::memcpy(k_, key, kKeyLen);
    hasKey_ = true;
    n_ = 0;
  }
  bool hasKey() const { return hasKey_; }
  uint64_t nonce() const { return n_; }

  void clear() {
    wipe(k_, sizeof(k_));
    hasKey_ = false;
    n_ = 0;
  }

  // EncryptWithAd. `out` must hold len + kTagLen when a key is set, len when not.
  bool encryptWithAd(const Crypto& c, const uint8_t* ad, size_t adLen, const uint8_t* pt, size_t len, uint8_t* out) {
    if (!hasKey_) {
      if (out != pt && len > 0) std::memmove(out, pt, len);
      return true;
    }
    // 2^64-1 is reserved by the spec; reaching it means the session is over.
    if (n_ == UINT64_MAX) return false;
    uint8_t nonce[kNonceLen];
    buildNonce(n_, nonce);
    if (!c.aeadSeal(k_, nonce, ad, adLen, pt, len, out)) return false;
    ++n_;
    return true;
  }

  // DecryptWithAd. **The nonce advances only on success**, as the spec says: a
  // forged frame must not desynchronise the counter. The channel above kills the
  // session on the first failure anyway, so this never gets a second chance.
  bool decryptWithAd(const Crypto& c, const uint8_t* ad, size_t adLen, const uint8_t* ct, size_t ctLen, uint8_t* out) {
    if (!hasKey_) {
      if (out != ct && ctLen > 0) std::memmove(out, ct, ctLen);
      return true;
    }
    if (n_ == UINT64_MAX || ctLen < kTagLen) return false;
    uint8_t nonce[kNonceLen];
    buildNonce(n_, nonce);
    if (!c.aeadOpen(k_, nonce, ad, adLen, ct, ctLen, out)) return false;
    ++n_;
    return true;
  }

  // AESGCM's nonce: 32 zero bits, then n as a big-endian u64 (spec 12.3).
  static void buildNonce(uint64_t n, uint8_t out[kNonceLen]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<uint8_t>(n >> (56 - 8 * i));
  }

 private:
  uint8_t k_[kKeyLen] = {};
  bool hasKey_ = false;
  uint64_t n_ = 0;
};

// --- SymmetricState (spec 5.2) ----------------------------------------------

class SymmetricState {
 public:
  ~SymmetricState() { clear(); }

  void clear() {
    wipe(ck_, sizeof(ck_));
    wipe(h_, sizeof(h_));
    cs_.clear();
  }

  bool initialize(const Crypto& c, const char* protocolName) {
    const size_t len = std::strlen(protocolName);
    if (len <= kHashLen) {
      std::memset(h_, 0, kHashLen);
      std::memcpy(h_, protocolName, len);
    } else if (!c.sha256(reinterpret_cast<const uint8_t*>(protocolName), len, nullptr, 0, h_)) {
      return false;
    }
    std::memcpy(ck_, h_, kHashLen);
    cs_.clear();
    return true;
  }

  bool mixKey(const Crypto& c, const uint8_t* ikm, size_t len) {
    uint8_t out1[kHashLen];
    uint8_t out2[kHashLen];
    if (!hkdf(c, ikm, len, out1, out2, nullptr)) return false;
    std::memcpy(ck_, out1, kHashLen);
    cs_.initializeKey(out2);  // HASHLEN == 32, so no truncation to 32 is needed.
    wipe(out1, sizeof(out1));
    wipe(out2, sizeof(out2));
    return true;
  }

  bool mixHash(const Crypto& c, const uint8_t* data, size_t len) {
    uint8_t next[kHashLen];
    if (!c.sha256(h_, kHashLen, data, len, next)) return false;
    std::memcpy(h_, next, kHashLen);
    return true;
  }

  bool mixKeyAndHash(const Crypto& c, const uint8_t* ikm, size_t len) {
    uint8_t out1[kHashLen];
    uint8_t out2[kHashLen];
    uint8_t out3[kHashLen];
    bool ok = hkdf(c, ikm, len, out1, out2, out3);
    if (ok) {
      std::memcpy(ck_, out1, kHashLen);
      ok = mixHash(c, out2, kHashLen);
      cs_.initializeKey(out3);
    }
    wipe(out1, sizeof(out1));
    wipe(out2, sizeof(out2));
    wipe(out3, sizeof(out3));
    return ok;
  }

  // EncryptAndHash: `out` gets len (+ kTagLen once a key is set) bytes.
  bool encryptAndHash(const Crypto& c, const uint8_t* pt, size_t len, uint8_t* out, size_t& outLen) {
    outLen = len + (cs_.hasKey() ? kTagLen : 0);
    if (!cs_.encryptWithAd(c, h_, kHashLen, pt, len, out)) return false;
    return mixHash(c, out, outLen);
  }

  // DecryptAndHash. The hash is over the ciphertext, so it is taken **before**
  // `out` may overwrite `ct` (the caller is allowed to decrypt in place).
  bool decryptAndHash(const Crypto& c, const uint8_t* ct, size_t ctLen, uint8_t* out, size_t& outLen) {
    const size_t overhead = cs_.hasKey() ? kTagLen : 0;
    if (ctLen < overhead) return false;
    uint8_t nextH[kHashLen];
    if (!c.sha256(h_, kHashLen, ct, ctLen, nextH)) return false;
    if (!cs_.decryptWithAd(c, h_, kHashLen, ct, ctLen, out)) return false;
    std::memcpy(h_, nextH, kHashLen);
    outLen = ctLen - overhead;
    return true;
  }

  // Split: the two transport keys, initiator-to-responder first.
  bool split(const Crypto& c, CipherState& c1, CipherState& c2) {
    uint8_t k1[kHashLen];
    uint8_t k2[kHashLen];
    const bool ok = hkdf(c, nullptr, 0, k1, k2, nullptr);
    if (ok) {
      c1.initializeKey(k1);
      c2.initializeKey(k2);
    }
    wipe(k1, sizeof(k1));
    wipe(k2, sizeof(k2));
    return ok;
  }

  const uint8_t* handshakeHash() const { return h_; }
  bool hasKey() const { return cs_.hasKey(); }

 private:
  // HKDF as the spec defines it (section 4.3), from HMAC-HASH: one extract, then
  // two or three expand steps. `out3` nullptr asks for two outputs.
  bool hkdf(const Crypto& c, const uint8_t* ikm, size_t len, uint8_t out1[kHashLen], uint8_t out2[kHashLen],
            uint8_t* out3) {
    uint8_t temp[kHashLen];
    uint8_t buf[kHashLen + 1];
    bool ok = c.hmacSha256(ck_, ikm, len, temp);
    const uint8_t one = 0x01;
    ok = ok && c.hmacSha256(temp, &one, 1, out1);
    if (ok) {
      std::memcpy(buf, out1, kHashLen);
      buf[kHashLen] = 0x02;
      ok = c.hmacSha256(temp, buf, sizeof(buf), out2);
    }
    if (ok && out3 != nullptr) {
      std::memcpy(buf, out2, kHashLen);
      buf[kHashLen] = 0x03;
      ok = c.hmacSha256(temp, buf, sizeof(buf), out3);
    }
    wipe(temp, sizeof(temp));
    wipe(buf, sizeof(buf));
    return ok;
  }

  CipherState cs_;
  uint8_t ck_[kHashLen] = {};
  uint8_t h_[kHashLen] = {};
};

// --- HandshakeState for IK and IKpsk2 (spec 5.3, 7.5, 9) ------------------------
//
//   IK:                        IKpsk2:
//     <- s                       <- s
//     ...                        ...
//     -> e, es, s, ss            -> e, es, s, ss
//     <- e, ee, se               <- e, ee, se, psk
//
// In a psk handshake the `e` token also runs MixKey(e.public_key) (spec 9.2).
// That and the trailing `psk` token are the only two differences.

enum class Pattern : uint8_t { IK, IKpsk2 };
enum class Role : uint8_t { Initiator, Responder };

inline constexpr const char* kProtocolIK = "Noise_IK_25519_AESGCM_SHA256";
inline constexpr const char* kProtocolIKpsk2 = "Noise_IKpsk2_25519_AESGCM_SHA256";

// Bytes on the wire around a payload of P bytes:
//   message 1: e (32) + encrypted s (32 + 16) + payload (P + 16)  = 96 + P
//   message 2: e (32) + payload (P + 16)                          = 48 + P
inline constexpr size_t kMessage1Overhead = kDhLen + kDhLen + kTagLen + kTagLen;
inline constexpr size_t kMessage2Overhead = kDhLen + kTagLen;

class Handshake {
 public:
  ~Handshake() { clear(); }

  void clear() {
    wipe(s_, sizeof(s_));
    wipe(sPub_, sizeof(sPub_));
    wipe(e_, sizeof(e_));
    wipe(ePub_, sizeof(ePub_));
    wipe(rs_, sizeof(rs_));
    wipe(re_, sizeof(re_));
    wipe(psk_, sizeof(psk_));
    ss_.clear();
    step_ = Step::Dead;
  }

  // `sPriv` is this side's static private key. `rs` is the responder's static
  // public key for an initiator (the pre-message), nullptr for a responder.
  // `psk` is required for IKpsk2 and ignored for IK. `pinnedEphemeral` is for
  // the vector tests only; nullptr draws one from `crypto.random`.
  bool initialize(const Crypto* crypto, Pattern pattern, Role role, const uint8_t* prologue, size_t prologueLen,
                  const uint8_t sPriv[kDhLen], const uint8_t* rs, const uint8_t* psk,
                  const uint8_t* pinnedEphemeral = nullptr) {
    clear();
    c_ = crypto;
    pattern_ = pattern;
    role_ = role;
    if (c_ == nullptr || sPriv == nullptr) return false;
    if (role == Role::Initiator && rs == nullptr) return false;
    if (pattern == Pattern::IKpsk2) {
      if (psk == nullptr) return false;
      std::memcpy(psk_, psk, kKeyLen);
    }
    std::memcpy(s_, sPriv, kDhLen);
    if (!c_->pubFromPriv(s_, sPub_)) return fail();
    if (pinnedEphemeral != nullptr) {
      std::memcpy(e_, pinnedEphemeral, kDhLen);
      ephemeralPinned_ = true;
    } else {
      ephemeralPinned_ = false;
    }

    if (!ss_.initialize(*c_, pattern == Pattern::IKpsk2 ? kProtocolIKpsk2 : kProtocolIK)) return fail();
    if (!ss_.mixHash(*c_, prologue, prologueLen)) return fail();
    // The pre-message `<- s`: both sides hash the responder's static key.
    if (role == Role::Initiator) {
      std::memcpy(rs_, rs, kDhLen);
      if (!ss_.mixHash(*c_, rs_, kDhLen)) return fail();
    } else {
      if (!ss_.mixHash(*c_, sPub_, kDhLen)) return fail();
    }
    step_ = role == Role::Initiator ? Step::WriteMessage1 : Step::ReadMessage1;
    return true;
  }

  // -> e, es, s, ss, payload     (initiator)
  bool writeMessage1(const uint8_t* payload, size_t payloadLen, uint8_t* out, size_t outCap, size_t& outLen) {
    if (step_ != Step::WriteMessage1) return fail();
    if (payloadLen > kMaxMessageLen - kMessage1Overhead || outCap < kMessage1Overhead + payloadLen) return fail();
    size_t pos = 0;
    if (!writeE(out, pos)) return fail();
    if (!dhMix(e_, rs_)) return fail();  // es
    size_t n = 0;
    if (!ss_.encryptAndHash(*c_, sPub_, kDhLen, out + pos, n)) return fail();  // s
    pos += n;
    if (!dhMix(s_, rs_)) return fail();  // ss
    if (!ss_.encryptAndHash(*c_, payload, payloadLen, out + pos, n)) return fail();
    outLen = pos + n;
    step_ = Step::ReadMessage2;
    return true;
  }

  // -> e, es, s, ss, payload     (responder). `payloadOut` needs
  // len - kMessage1Overhead bytes. The initiator's static key is then
  // remoteStatic() -- the caller decides whether it knows that phone.
  bool readMessage1(const uint8_t* msg, size_t len, uint8_t* payloadOut, size_t payloadCap, size_t& payloadLen) {
    if (step_ != Step::ReadMessage1) return fail();
    if (len < kMessage1Overhead || len - kMessage1Overhead > payloadCap) return fail();
    size_t pos = 0;
    if (!readE(msg, pos)) return fail();
    if (!dhMix(s_, re_)) return fail();  // es
    size_t n = 0;
    if (!ss_.decryptAndHash(*c_, msg + pos, kDhLen + kTagLen, rs_, n) || n != kDhLen) return fail();  // s
    pos += kDhLen + kTagLen;
    if (!dhMix(s_, rs_)) return fail();  // ss
    if (!ss_.decryptAndHash(*c_, msg + pos, len - pos, payloadOut, payloadLen)) return fail();
    step_ = Step::WriteMessage2;
    return true;
  }

  // <- e, ee, se, [psk], payload (responder)
  bool writeMessage2(const uint8_t* payload, size_t payloadLen, uint8_t* out, size_t outCap, size_t& outLen) {
    if (step_ != Step::WriteMessage2) return fail();
    if (payloadLen > kMaxMessageLen - kMessage2Overhead || outCap < kMessage2Overhead + payloadLen) return fail();
    size_t pos = 0;
    if (!writeE(out, pos)) return fail();
    if (!dhMix(e_, re_)) return fail();  // ee
    if (!dhMix(e_, rs_)) return fail();  // se: initiator's s, our e
    if (pattern_ == Pattern::IKpsk2 && !ss_.mixKeyAndHash(*c_, psk_, kKeyLen)) return fail();
    size_t n = 0;
    if (!ss_.encryptAndHash(*c_, payload, payloadLen, out + pos, n)) return fail();
    outLen = pos + n;
    step_ = Step::Split;
    return true;
  }

  // <- e, ee, se, [psk], payload (initiator)
  bool readMessage2(const uint8_t* msg, size_t len, uint8_t* payloadOut, size_t payloadCap, size_t& payloadLen) {
    if (step_ != Step::ReadMessage2) return fail();
    if (len < kMessage2Overhead || len - kMessage2Overhead > payloadCap) return fail();
    size_t pos = 0;
    if (!readE(msg, pos)) return fail();
    if (!dhMix(e_, re_)) return fail();  // ee
    if (!dhMix(s_, re_)) return fail();  // se: our s, responder's e
    if (pattern_ == Pattern::IKpsk2 && !ss_.mixKeyAndHash(*c_, psk_, kKeyLen)) return fail();
    if (!ss_.decryptAndHash(*c_, msg + pos, len - pos, payloadOut, payloadLen)) return fail();
    step_ = Step::Split;
    return true;
  }

  // The transport keys, oriented for this side: `send` encrypts what this side
  // writes, `recv` decrypts what it reads. Wipes every handshake secret after,
  // because nothing but the two CipherStates may outlive the handshake.
  bool split(CipherState& send, CipherState& recv) {
    if (step_ != Step::Split) return fail();
    CipherState c1;
    CipherState c2;
    if (!ss_.split(*c_, c1, c2)) return fail();
    // Copy the handshake hash out before clear() -- channel binding and the
    // vector test both read it after the split.
    std::memcpy(hashAfterSplit_, ss_.handshakeHash(), kHashLen);
    CipherState& i2r = c1;
    CipherState& r2i = c2;
    if (role_ == Role::Initiator) {
      move(i2r, send);
      move(r2i, recv);
    } else {
      move(r2i, send);
      move(i2r, recv);
    }
    const uint8_t* keep = rs_;
    uint8_t remote[kDhLen];
    std::memcpy(remote, keep, kDhLen);
    clear();
    std::memcpy(rs_, remote, kDhLen);  // public, and the caller still needs it
    wipe(remote, sizeof(remote));
    step_ = Step::Done;
    return true;
  }

  // The other side's static public key. For a responder, valid after
  // readMessage1(); kept across split().
  const uint8_t* remoteStatic() const { return rs_; }
  const uint8_t* handshakeHash() const { return step_ == Step::Done ? hashAfterSplit_ : ss_.handshakeHash(); }
  bool done() const { return step_ == Step::Done; }

 private:
  enum class Step : uint8_t { Dead, WriteMessage1, ReadMessage1, WriteMessage2, ReadMessage2, Split, Done };

  bool fail() {
    clear();
    return false;
  }

  bool writeE(uint8_t* out, size_t& pos) {
    if (!ephemeralPinned_ && !c_->random(e_, kDhLen)) return false;
    if (!c_->pubFromPriv(e_, ePub_)) return false;
    std::memcpy(out + pos, ePub_, kDhLen);
    pos += kDhLen;
    if (!ss_.mixHash(*c_, ePub_, kDhLen)) return false;
    return pattern_ != Pattern::IKpsk2 || ss_.mixKey(*c_, ePub_, kDhLen);
  }

  bool readE(const uint8_t* msg, size_t& pos) {
    std::memcpy(re_, msg + pos, kDhLen);
    pos += kDhLen;
    if (!ss_.mixHash(*c_, re_, kDhLen)) return false;
    return pattern_ != Pattern::IKpsk2 || ss_.mixKey(*c_, re_, kDhLen);
  }

  bool dhMix(const uint8_t priv[kDhLen], const uint8_t pub[kDhLen]) {
    uint8_t shared[kDhLen];
    const bool ok = c_->dh(priv, pub, shared) && ss_.mixKey(*c_, shared, kDhLen);
    wipe(shared, sizeof(shared));
    return ok;
  }

  static void move(CipherState& from, CipherState& to) {
    // CipherState has no public key getter on purpose; this is the one place a
    // key changes owner, so it goes through a friendless copy and a clear.
    to = from;
    from.clear();
  }

  const Crypto* c_ = nullptr;
  Pattern pattern_ = Pattern::IK;
  Role role_ = Role::Initiator;
  Step step_ = Step::Dead;
  bool ephemeralPinned_ = false;
  SymmetricState ss_;
  uint8_t s_[kDhLen] = {};
  uint8_t sPub_[kDhLen] = {};
  uint8_t e_[kDhLen] = {};
  uint8_t ePub_[kDhLen] = {};
  uint8_t rs_[kDhLen] = {};
  uint8_t re_[kDhLen] = {};
  uint8_t psk_[kKeyLen] = {};
  uint8_t hashAfterSplit_[kHashLen] = {};
};

}  // namespace syncnoise
