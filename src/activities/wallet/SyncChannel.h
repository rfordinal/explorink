#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "SyncNoise.h"

// The sync channel, device side: who may push a wallet document (T-2098).
//
// The decision is docs/sync-channel.md; this header is its device half, pure, so
// the host can drive every refusal. In one paragraph: a phone is **paired once**
// by scanning a QR code off the wallet screen (Noise IKpsk2, the psk is the
// code's one-time secret), and **every sync after that is a fresh Noise IK
// session** between two static keys that already know each other. Wallet
// transfer frames run only inside such a session; outside one, the receiver
// refuses them (MapTransferDecide.h, `decideWalletGates`, "not paired").
//
// ## Wire format, version 1
//
// In, on the transfer characteristic, one BLE write per frame. The plain
// transfer opcodes 0x01..0x03 (MapTransferDecide.h) are not this layer's; the
// owner sends them to the receiver as before, and they can only ever carry tiles.
//
//   0x10 PAIR    u8 version, Noise_IKpsk2 message 1 (empty payload, 96 bytes)
//   0x11 OPEN    u8 version, Noise_IK message 1     (empty payload, 96 bytes)
//   0x12 SEALED  one Noise transport message: an inner frame plus a 16-byte tag
//
// A PAIR or OPEN frame is 98 bytes, so it needs an ATT MTU of at least 101. Every
// phone we support negotiates more (the receiver logs the MTU per transfer); one
// that does not gets "short frame" rather than a half-read handshake.
//
// Inner frames, inside SEALED:
//
//   0x01..0x03   a transfer frame, exactly as MapTransferDecide.h defines it
//   0x30 PAIRED  the phone's "I have the keys": the only inner frame a pairing
//                session accepts, and the moment the pairing is stored
//
// Out, on the status characteristic, one text line each:
//
//   NX2 <base64>   Noise message 2 (empty payload, 48 bytes -> 64 characters)
//   NXS <base64>   one sealed status line: the receiver's RDY/OK/ERR, or OK paired
//   ERR <reason>   the channel refusing, in the clear -- a refusal carries nothing
//
// Text and base64 rather than raw bytes because the status characteristic is a
// line stream the phone already parses (TransferFrames.kt); a binary line on it
// would be the first, and a line-splitting reader would cut one at its first 0x0A.
//
// ## Why the pairing is stored on the first sealed frame, not on message 1
//
// In IKpsk2 the psk is mixed in at message **2**. Message 1 proves the phone
// holds its static key; it proves nothing about the QR. So a stranger who never
// saw the panel can send a valid message 1 and the device cannot tell. The first
// transport message from the phone is the first thing whose key depends on the
// psk, so the first SEALED frame that opens is the proof -- and the only point
// where the phone's key may be written to the table (test: "a pairing without
// the code is never stored").
//
// ## The code is one attempt, short-lived, never shown twice
//
// The three conditions of sync-channel.md, "Three conditions on the QR":
// `CMD:SCREENSHOT` is ungated (T-2081), so the code is readable by anyone with a
// cable or a radio for as long as it is on the panel. Hence: the first PAIR frame
// consumes it whatever happens next; it dies after kCodeLifetimeMs; it dies when
// the screen does; and issueCode() always draws a new secret. **There is no
// "show the same code again"** -- a redraw of a live code is the same frame on
// the panel, a new code is a new secret.
//
// ## Threading
//
// None. Every method runs on whichever task the owner calls it from, and the
// owner serialises -- the same rule as the receiver's FrameGate
// (MapTransferGate.h). The NimBLE host task delivers frames; the screen's task
// issues codes and shuts down. That gate belongs to the owner (T-2097's wallet
// screen), not to this header.
//
// Pure: no Arduino, no NVS, no heap. Persisting the table is the owner's job,
// handed back through `PairingSink`.

namespace syncchannel {

inline constexpr uint8_t kWireVersion = 1;

inline constexpr uint8_t kOpPair = 0x10;
inline constexpr uint8_t kOpOpen = 0x11;
inline constexpr uint8_t kOpSealed = 0x12;
inline constexpr uint8_t kInnerPaired = 0x30;

// Mixed into every handshake as the Noise prologue, so a transcript from any
// other protocol -- or from a later wire version -- can never verify as ours.
inline constexpr char kPrologue[] = "explorink-sync/1";

// How long a pairing code lives once drawn. Long enough to take a phone out and
// point it; short enough that a code left on a parked bike is dead before
// anybody finds it. A judgment, not a measurement -- revisit after the first
// pairing on a real phone.
inline constexpr uint32_t kCodeLifetimeMs = 90u * 1000u;

// Phones one device remembers. Four covers a rider with a phone and a spare; the
// table is 4 x 32 bytes of NVS. Full means the rider forgets one first.
inline constexpr size_t kMaxPhones = 4;

// The largest BLE write the device can take: ATT_MTU 517 minus 3. A SEALED frame
// larger than this is not a frame we sent.
inline constexpr size_t kMaxFrameLen = 514;
inline constexpr size_t kMaxInnerLen = kMaxFrameLen - 1 - syncnoise::kTagLen;

// A status line from the receiver is at most this long, without its newline
// (BlePositionServer::sendTransferStatus buffer). Sealed and base64'd it must
// still fit the status line buffer: 4 + 4*ceil((63 + 16) / 3) = 112 < 127.
inline constexpr size_t kMaxStatusLineLen = 63;
inline constexpr size_t kMaxOutLineLen = 127;

inline constexpr size_t kHandshakeFrameLen = 2 + syncnoise::kMessage1Overhead;

// --- base64 (RFC 4648, standard alphabet, padded) ----------------------------

inline size_t base64Len(size_t n) { return 4 * ((n + 2) / 3); }

// Writes base64Len(len) characters and a terminator; false if it does not fit.
inline bool base64Encode(const uint8_t* in, size_t len, char* out, size_t cap) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (base64Len(len) + 1 > cap) return false;
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t b0 = in[i];
    const uint32_t b1 = i + 1 < len ? in[i + 1] : 0;
    const uint32_t b2 = i + 2 < len ? in[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out[o++] = kAlphabet[(v >> 18) & 63];
    out[o++] = kAlphabet[(v >> 12) & 63];
    out[o++] = i + 1 < len ? kAlphabet[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < len ? kAlphabet[v & 63] : '=';
  }
  out[o] = '\0';
  return true;
}

// --- The pairing table -------------------------------------------------------
//
// NVS blob, version 1: u8 version, u8 count, count x 32-byte X25519 public keys.
// Public keys only. The device's own static private key is a separate NVS entry
// (SyncPairingStore.cpp), and the boundary on both is the wallet wrap's: NVS is
// plaintext flash, `esptool` reads it (wallet-crypto.md, "The threat boundary").

inline constexpr uint8_t kTableVersion = 1;
inline constexpr size_t kTableBlobLen = 2 + kMaxPhones * syncnoise::kDhLen;

struct PairedPhones {
  uint8_t count = 0;
  uint8_t keys[kMaxPhones][syncnoise::kDhLen] = {};

  int find(const uint8_t pub[syncnoise::kDhLen]) const {
    for (uint8_t i = 0; i < count; ++i) {
      if (std::memcmp(keys[i], pub, syncnoise::kDhLen) == 0) return i;
    }
    return -1;
  }

  // Already there is success: a phone that pairs again (reinstalled app keeping
  // its key, or a rider who did not know it was paired) changes nothing.
  bool add(const uint8_t pub[syncnoise::kDhLen]) {
    if (find(pub) >= 0) return true;
    if (count >= kMaxPhones) return false;
    std::memcpy(keys[count], pub, syncnoise::kDhLen);
    ++count;
    return true;
  }

  // Order is kept so the screen's list does not reshuffle under the rider.
  bool remove(uint8_t index) {
    if (index >= count) return false;
    for (uint8_t i = index; i + 1 < count; ++i) std::memcpy(keys[i], keys[i + 1], syncnoise::kDhLen);
    --count;
    std::memset(keys[count], 0, syncnoise::kDhLen);
    return true;
  }

  void clear() { *this = PairedPhones{}; }

  size_t serialize(uint8_t out[kTableBlobLen]) const {
    out[0] = kTableVersion;
    out[1] = count;
    std::memcpy(out + 2, keys, static_cast<size_t>(count) * syncnoise::kDhLen);
    return 2 + static_cast<size_t>(count) * syncnoise::kDhLen;
  }

  // Exact length or nothing. A blob that does not parse is treated as "no
  // phones" by the caller -- refusing every sync is the safe side, and the rider
  // can pair again; trusting half a table is not an option.
  bool parse(const uint8_t* in, size_t len) {
    clear();
    if (in == nullptr || len < 2 || in[0] != kTableVersion || in[1] > kMaxPhones) return false;
    if (len != 2 + static_cast<size_t>(in[1]) * syncnoise::kDhLen) return false;
    count = in[1];
    std::memcpy(keys, in + 2, static_cast<size_t>(count) * syncnoise::kDhLen);
    return true;
  }
};

// --- The QR payload ------------------------------------------------------------
//
// `EXPLORINK-PAIR:1:<64 hex: device static public key><64 hex: psk>`, hex in
// upper case so the whole string stays inside QR's alphanumeric mode (0-9, A-Z,
// and ` $%*+-./:`), which packs 5.5 bits a character instead of 8. 145
// characters. The version is in the clear so a phone of another vintage can say
// "update the app" instead of failing a handshake.

inline constexpr char kQrPrefix[] = "EXPLORINK-PAIR:1:";
inline constexpr size_t kQrTextLen = sizeof(kQrPrefix) - 1 + 128;

inline bool buildQrText(const uint8_t devicePub[syncnoise::kDhLen], const uint8_t psk[syncnoise::kKeyLen], char* out,
                        size_t cap) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  if (cap < kQrTextLen + 1) return false;
  size_t o = 0;
  for (size_t i = 0; kQrPrefix[i] != '\0'; ++i) out[o++] = kQrPrefix[i];
  for (size_t i = 0; i < syncnoise::kDhLen; ++i) {
    out[o++] = kHex[devicePub[i] >> 4];
    out[o++] = kHex[devicePub[i] & 15];
  }
  for (size_t i = 0; i < syncnoise::kKeyLen; ++i) {
    out[o++] = kHex[psk[i] >> 4];
    out[o++] = kHex[psk[i] & 15];
  }
  out[o] = '\0';
  return true;
}

// --- The channel ---------------------------------------------------------------

// Persists the table after a pairing. Returning false refuses the pairing: a
// phone told "paired" that the device forgets at the next boot is worse than a
// phone told to try again.
struct PairingSink {
  void* ctx = nullptr;
  bool (*persist)(void* ctx, const PairedPhones& table) = nullptr;
};

// What the owner does with the outcome of one frame.
enum class Outcome : uint8_t {
  // Nothing to send (a frame the channel swallowed with no answer).
  None,
  // Send `line()` on the status characteristic.
  Reply,
  // `inner()` / `innerLen()` is a transfer frame from a session: hand it to the
  // receiver as a sealed frame. Its status lines come back through sealLine().
  DeliverInner,
};

class Channel {
 public:
  // `staticPriv` is the device's static private key, loaded by the owner from
  // NVS. `table` is the paired phones, also the owner's, updated in place on a
  // pairing. Neither is copied anywhere but here, and shutdown() wipes both.
  bool begin(const syncnoise::Crypto* crypto, const uint8_t staticPriv[syncnoise::kDhLen], const PairedPhones& table,
             PairingSink sink) {
    shutdown();
    if (crypto == nullptr || staticPriv == nullptr) return false;
    c_ = crypto;
    std::memcpy(staticPriv_, staticPriv, syncnoise::kDhLen);
    if (!c_->pubFromPriv(staticPriv_, staticPub_)) {
      shutdown();
      return false;
    }
    table_ = table;
    sink_ = sink;
    ready_ = true;
    return true;
  }

  // Wipes every secret: static key, code, handshake, session. The wallet
  // screen's onExit (sync-channel.md, "Its state is owned by the wallet screen
  // and dies with it").
  void shutdown() {
    endSession();
    killCode();
    syncnoise::wipe(staticPriv_, sizeof(staticPriv_));
    syncnoise::wipe(staticPub_, sizeof(staticPub_));
    table_.clear();
    syncnoise::wipe(inner_, sizeof(inner_));
    ready_ = false;
    c_ = nullptr;
  }

  // --- The pairing code ---------------------------------------------------

  // A fresh code, always a new secret. Refused when the table is full -- a code
  // that can only end in "table full" is a secret on the panel for nothing.
  bool issueCode(uint32_t nowMs) {
    killCode();
    if (!ready_ || table_.count >= kMaxPhones) return false;
    if (!c_->random(psk_, sizeof(psk_))) return false;
    codeIssuedMs_ = nowMs;
    codeAlive_ = true;
    return true;
  }

  // Whether a code is still usable. Also the screen's cue to take the QR off the
  // panel: the owner polls this and redraws without it once it turns false.
  bool codeAlive(uint32_t nowMs) {
    if (codeAlive_ && nowMs - codeIssuedMs_ >= kCodeLifetimeMs) killCode();
    return codeAlive_;
  }

  // The text for the QR. Only while the code is alive.
  bool qrText(uint32_t nowMs, char* out, size_t cap) {
    if (!codeAlive(nowMs)) return false;
    return buildQrText(staticPub_, psk_, out, cap);
  }

  void killCode() {
    syncnoise::wipe(psk_, sizeof(psk_));
    codeAlive_ = false;
    codeIssuedMs_ = 0;
  }

  // --- Frames -------------------------------------------------------------

  Outcome onFrame(const uint8_t* data, size_t len, uint32_t nowMs) {
    line_[0] = '\0';
    innerLen_ = 0;
    if (!ready_ || data == nullptr || len == 0) return Outcome::None;
    switch (data[0]) {
      case kOpPair:
        return onHandshake(syncnoise::Pattern::IKpsk2, data, len, nowMs);
      case kOpOpen:
        return onHandshake(syncnoise::Pattern::IK, data, len, nowMs);
      case kOpSealed:
        return onSealed(data + 1, len - 1);
      default:
        return refuse("bad channel opcode");
    }
  }

  // The link went away: any session or half-done pairing is dead. The code is
  // not -- it dies by the clock, the screen, or its one attempt.
  void onDisconnect() { endSession(); }

  // Seals one receiver status line into `NXS <base64>`, into line(). False with
  // no session -- the caller must then drop the line, never send it plain: a
  // sealed transfer's status is part of the session.
  bool sealLine(const char* text) {
    line_[0] = '\0';
    if (state_ != State::Open || text == nullptr) return false;
    const size_t len = std::strlen(text);
    if (len > kMaxStatusLineLen) return false;
    uint8_t buf[kMaxStatusLineLen + syncnoise::kTagLen];
    if (!send_.encryptWithAd(*c_, nullptr, 0, reinterpret_cast<const uint8_t*>(text), len, buf)) return fail("seal");
    return putB64("NXS ", buf, len + syncnoise::kTagLen);
  }

  const char* line() const { return line_; }
  const uint8_t* inner() const { return inner_; }
  size_t innerLen() const { return innerLen_; }
  bool sessionOpen() const { return state_ == State::Open; }
  const PairedPhones& table() const { return table_; }
  const uint8_t* staticPub() const { return staticPub_; }

  // For the screen's "forget" row. The owner persists after.
  bool forgetPhone(uint8_t index) {
    if (!table_.remove(index)) return false;
    endSession();  // a session for a forgotten phone must not outlive the forget
    return true;
  }

 private:
  enum class State : uint8_t {
    Idle,
    // A pairing handshake finished; waiting for the phone's PAIRED frame, the
    // first proof it holds the psk.
    PairPending,
    // A sync session: sealed transfer frames are delivered.
    Open,
  };

  Outcome onHandshake(syncnoise::Pattern pattern, const uint8_t* data, size_t len, uint32_t nowMs) {
    // A new handshake ends whatever was running: a paired phone reconnecting is
    // the normal case, and two live sessions on one link is never one.
    endSession();
    if (pattern == syncnoise::Pattern::IKpsk2) {
      // One attempt per code, counted **before** any check on the frame, so a
      // malformed or wrong-version PAIR still spends it. Anything else would let
      // an attacker probe for free.
      const bool alive = codeAlive(nowMs);
      uint8_t psk[syncnoise::kKeyLen];
      std::memcpy(psk, psk_, sizeof(psk));
      killCode();
      if (!alive) {
        syncnoise::wipe(psk, sizeof(psk));
        return refuse("no pairing code");
      }
      const Outcome o = runHandshake(pattern, data, len, psk);
      syncnoise::wipe(psk, sizeof(psk));
      return o;
    }
    return runHandshake(pattern, data, len, nullptr);
  }

  Outcome runHandshake(syncnoise::Pattern pattern, const uint8_t* data, size_t len, const uint8_t* psk) {
    if (len < 2) return refuse("short frame");
    // Version before length: a phone of another wire version should read "update
    // the app", not "short frame".
    if (data[1] != kWireVersion) return refuse("channel version");
    if (len != kHandshakeFrameLen) return refuse("short frame");

    if (!hs_.initialize(c_, pattern, syncnoise::Role::Responder, reinterpret_cast<const uint8_t*>(kPrologue),
                        sizeof(kPrologue) - 1, staticPriv_, nullptr, psk)) {
      return abandon("handshake");
    }
    uint8_t payload[1];
    size_t payloadLen = 0;
    if (!hs_.readMessage1(data + 2, len - 2, payload, sizeof(payload), payloadLen) || payloadLen != 0) {
      hs_.clear();
      return refuse("handshake");
    }
    // A sync session only for a phone in the table. Checked after message 1
    // opened -- before that the key is not known -- and before message 2 is
    // written, so an unknown phone never gets a session key.
    if (pattern == syncnoise::Pattern::IK && table_.find(hs_.remoteStatic()) < 0) {
      hs_.clear();
      return refuse("not paired");
    }
    uint8_t msg2[syncnoise::kMessage2Overhead];
    size_t msg2Len = 0;
    if (!hs_.writeMessage2(nullptr, 0, msg2, sizeof(msg2), msg2Len) || !hs_.split(send_, recv_)) {
      return abandon("handshake");
    }
    std::memcpy(peer_, hs_.remoteStatic(), sizeof(peer_));
    hs_.clear();
    state_ = pattern == syncnoise::Pattern::IKpsk2 ? State::PairPending : State::Open;
    putB64("NX2 ", msg2, msg2Len);
    return Outcome::Reply;
  }

  Outcome onSealed(const uint8_t* ct, size_t len) {
    if (state_ == State::Idle) return refuse("no session");
    if (len <= syncnoise::kTagLen || len - syncnoise::kTagLen > kMaxInnerLen) {
      endSession();
      return refuse("bad sealed frame");
    }
    // A frame that does not open ends the session: a forgery, a replay from
    // another session, or a desynchronised counter. None of them is a reason to
    // keep the keys. The spec leaves this to the application; for one ordered
    // link with write-with-response, there is no benign cause.
    if (!recv_.decryptWithAd(*c_, nullptr, 0, ct, len, inner_)) {
      endSession();
      return refuse("bad sealed frame");
    }
    innerLen_ = len - syncnoise::kTagLen;

    if (state_ == State::PairPending) {
      // The only thing a pairing session may say. A transfer here would make
      // pairing and receiving one act, which sync-channel.md forbids ("Three
      // acts, and they stay three").
      if (innerLen_ != 1 || inner_[0] != kInnerPaired) {
        endSession();
        return refuse("pairing only");
      }
      PairedPhones next = table_;
      if (!next.add(peer_)) {
        endSession();
        return refuse("table full");
      }
      if (sink_.persist == nullptr || !sink_.persist(sink_.ctx, next)) {
        endSession();
        return refuse("not stored");
      }
      table_ = next;
      // Answered sealed, then closed: a sync is a separate OPEN, so pairing a
      // phone never leaves a transfer-capable session behind it.
      state_ = State::Open;
      sealLine("OK paired");
      endSession();
      innerLen_ = 0;
      return Outcome::Reply;
    }

    // Open. Only transfer frames go further; the receiver re-parses them.
    if (inner_[0] == kInnerPaired) {
      innerLen_ = 0;
      return Outcome::None;  // a repeated PAIRED in a sync session is harmless and says nothing
    }
    return Outcome::DeliverInner;
  }

  Outcome refuse(const char* reason) {
    putText("ERR ", reason);
    return Outcome::Reply;
  }

  Outcome abandon(const char* reason) {
    endSession();
    putText("ERR ", reason);
    return Outcome::Reply;
  }

  bool fail(const char* reason) {
    endSession();
    putText("ERR ", reason);
    return false;
  }

  void endSession() {
    hs_.clear();
    send_.clear();
    recv_.clear();
    syncnoise::wipe(peer_, sizeof(peer_));
    state_ = State::Idle;
  }

  void putText(const char* prefix, const char* text) {
    size_t o = 0;
    for (size_t i = 0; prefix[i] != '\0' && o < kMaxOutLineLen; ++i) line_[o++] = prefix[i];
    for (size_t i = 0; text[i] != '\0' && o < kMaxOutLineLen; ++i) line_[o++] = text[i];
    line_[o] = '\0';
  }

  bool putB64(const char* prefix, const uint8_t* data, size_t len) {
    const size_t p = std::strlen(prefix);
    std::memcpy(line_, prefix, p);
    if (!base64Encode(data, len, line_ + p, sizeof(line_) - p)) {
      line_[0] = '\0';
      return false;
    }
    return true;
  }

  const syncnoise::Crypto* c_ = nullptr;
  bool ready_ = false;
  State state_ = State::Idle;
  uint8_t staticPriv_[syncnoise::kDhLen] = {};
  uint8_t staticPub_[syncnoise::kDhLen] = {};
  PairedPhones table_;
  PairingSink sink_;

  uint8_t psk_[syncnoise::kKeyLen] = {};
  bool codeAlive_ = false;
  uint32_t codeIssuedMs_ = 0;

  syncnoise::Handshake hs_;
  syncnoise::CipherState send_;
  syncnoise::CipherState recv_;
  uint8_t peer_[syncnoise::kDhLen] = {};

  uint8_t inner_[kMaxInnerLen] = {};
  size_t innerLen_ = 0;
  char line_[kMaxOutLineLen + 1] = {};
};

}  // namespace syncchannel
