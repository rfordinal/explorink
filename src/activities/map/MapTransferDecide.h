#pragma once

#include <cstddef>
#include <cstdint>

#include "MapTransferPath.h"

// What a transfer frame means and whether it is allowed -- the decisions
// MapTransferReceiver used to make inline, lifted out so a host test can reach
// them.
//
// **Why this header exists, in one number.** T-2096 took five review rounds and
// found eleven real defects. Every one of them was in `MapTransferReceiver.cpp`.
// The pure headers beside it -- `WalletAssetIngest.h`, `WalletManifestIngest.h`,
// the crypto -- carried 164 host tests and produced none, across all five
// rounds. That file is not worse code; it is the code with no instrument on it,
// because it cannot link on the host: `Arduino.h`, NimBLE, `HalStorage` and the
// renderer are all in its include list. So the only way to ask it a question was
// a board, a push and an eye. T-2106.
//
// The receiver does five things and only the fifth needs hardware:
//
//   1. parse a frame (opcode, lengths, the path bytes);
//   2. resolve what the request is for -- already pure, in the wallet headers;
//   3. decide whether it is allowed;
//   4. decide what the state machine does with a chunk;
//   5. open, write, hash, rename, answer over BLE.
//
// 1, 3 and 4 are arithmetic over bytes and counters, and they are where all
// eleven defects lived. They are here. 5 stays in the `.cpp`, which gathers the
// facts, asks, and acts.
//
// Same shape as `cardRootMigrateDecide()` (CardRootMigration.h): facts in, an
// action plus a reason out, the I/O that acts on it left untested.
//
// ## The reason strings are the device's answer to the rider
//
// A reason here becomes `ERR <reason>` on the status characteristic, and the
// phone strips the `ERR ` and shows the rest (`TransferFrames.kt`). So these
// strings are user-visible text, not a parsed contract: changing one changes
// what a rider reads and breaks no parse. They are reproduced here byte for
// byte from the receiver they came out of -- a move that renames one is a move
// nobody can review.
//
// ## What is NOT here, and why
//
// Every refusal whose answer is an allocation or a card operation stays at the
// call site: `oom`, `mkdir failed`, `open failed`, the ingest and install
// reasons. A decision that needs memory returns the number and lets the caller
// allocate (firmware CLAUDE.md -- `makeUniqueNoThrow` and `Memory.h` belong to
// the acting half). `esp_rom_crc32_le` is not on the host at all, which is why
// the wallet path already passes CRC in as a function pointer; this layer stays
// free of it rather than growing a second seam.
//
// Pure: no Storage, no NimBLE, no wallet headers, nothing from Arduino.

namespace maptransfer {

// --- The wire format --------------------------------------------------------
//
// One BLE write is one frame; first byte is the opcode, the rest is little
// endian. The full block is in MapTransferReceiver.h. These constants live here
// rather than in the `.cpp`'s anonymous namespace because they are half of what
// the parse below means, and a test that cannot see them can only re-type them.

inline constexpr uint8_t kOpBegin = 0x01;
inline constexpr uint8_t kOpChunk = 0x02;
inline constexpr uint8_t kOpAbort = 0x03;

// Bytes after the opcode in a begin frame, before the path: totalLen + crc32 +
// pathLen.
inline constexpr size_t kBeginFixedBytes = 9;

// Bytes after the opcode in a chunk frame: the offset.
inline constexpr size_t kChunkHeaderBytes = 4;

// A sanity ceiling, not a real limit. The whole point of this channel is the
// small stuff -- a corridor of tiles, a safety layer, a trip file. Anything
// bigger belongs on the card over WiFi/SD, and a declared length in the
// hundreds of MB is a corrupt frame, not a plan.
inline constexpr uint32_t kMaxFileBytes = 8u * 1024u * 1024u;

// Little-endian u32 out of an unaligned BLE attribute buffer, byte by byte --
// never a cast-and-deref: the ESP32-C3 faults on an unaligned wide load
// (firmware CLAUDE.md, "RISC-V Alignment"). The receiver used `memcpy` for the
// same reason; this is the same thing written so the begin parse below and the
// chunk offset read in the `.cpp` share one copy of it.
inline uint32_t readU32le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// --- 1. The frame ----------------------------------------------------------

enum class FrameAction : uint8_t {
  // Not attached, or nothing to read. Answered with silence and **before** the
  // frame guard is taken, which is why this is not folded into the opcode
  // switch: an abort frame arriving while no transfer runs also does nothing,
  // but it still stamps `lastFrameMs_` and publishes on the way out.
  Ignore,
  Begin,
  Chunk,
  Abort,
  // `ERR bad opcode`, and the transfer in flight (if any) is left alone.
  BadOpcode,
};

inline FrameAction decideFrame(const bool attached, const uint8_t* data, const size_t len) {
  if (!attached || data == nullptr || len == 0) return FrameAction::Ignore;
  switch (data[0]) {
    case kOpBegin:
      return FrameAction::Begin;
    case kOpChunk:
      return FrameAction::Chunk;
    case kOpAbort:
      return FrameAction::Abort;
    default:
      return FrameAction::BadOpcode;
  }
}

// --- 2. The begin frame's header -------------------------------------------

struct BeginHeader {
  uint32_t total = 0;
  uint32_t crc = 0;
  size_t pathLen = 0;
  // Into the caller's frame buffer, `pathLen` bytes, not terminated.
  const uint8_t* path = nullptr;
};

// Parses the fixed part of a begin frame and answers the three refusals that
// need nothing but the frame itself. Returns nullptr when the header is good.
//
// `body` is the frame past the opcode, `len` its length.
inline const char* parseBeginHeader(const uint8_t* body, const size_t len, BeginHeader& out) {
  if (body == nullptr || len < kBeginFixedBytes + 1) return "short begin";

  const uint32_t total = readU32le(body + 0);
  const uint32_t crc = readU32le(body + 4);
  const size_t pathLen = body[8];

  // Exact length, not "at least": trailing bytes in a begin frame mean the
  // sender and this parser disagree about the format, and guessing which one is
  // right is how a wire format rots.
  if (pathLen == 0 || pathLen > kMaxRelPathBytes || len != kBeginFixedBytes + pathLen) return "bad path length";
  if (total == 0 || total > kMaxFileBytes) return "bad length";

  out.total = total;
  out.crc = crc;
  out.pathLen = pathLen;
  out.path = body + kBeginFixedBytes;
  return nullptr;
}

// --- 3. What the request is for, and whether it is allowed ------------------

// What the sender's relative path resolves to. The caller works this out with
// the wallet's own parsers (`WalletAssetIngest.h`), which are pure and already
// host-tested; this enum is only how the answer gets in here, so that this
// header needs no wallet include.
//
// **The resolution is pure, so the caller may do it before the gates below
// rather than between them**, which is where the receiver used to do it. That
// costs a few hundred nanoseconds in the `busy` case and changes nothing
// observable -- the refusal for `WalletForeign` is still issued in its original
// position in the ladder, after `busy`.
enum class BeginTarget : uint8_t {
  // Written where the sender spelled it, under the tile root. Everything that
  // is not the wallet, and everything at all when `ENABLE_WALLET` is off.
  Plain,
  // `wallet/<2 hex>/<16 hex>.dat`. The card path is rebuilt from the parsed id,
  // never from the sender's bytes.
  WalletAsset,
  // `wallet/manifest.json`. Lands under another name entirely.
  WalletManifest,
  // Under `wallet/` and not one of the two above. Refused outright, **before
  // anything touches the card** -- a push to `wallet/manifest.bak/x` used to
  // make a *directory* called `manifest.bak`, and `Storage.remove()` cannot
  // delete one.
  WalletForeign,
};

struct BeginGateFacts {
  // maptransfer::relPathIsSafe() over the frame's path bytes.
  bool pathSafe = false;
  // Somebody is listening on the status characteristic. Starting a transfer
  // with nobody subscribed means writing a file and never being able to say
  // whether it is good, which is worse than not starting.
  bool statusSubscribed = false;
  // A transfer is already in flight.
  bool transferActive = false;
  // millis() since that transfer's last frame. Meaningless when
  // `transferActive` is false.
  uint32_t msSinceLastFrame = 0;
  // How long a transfer may sit silent before the next begin reclaims it
  // (MapTransferReceiver::kStaleTransferMs). A fact rather than a constant here
  // so a test can drive both sides of the boundary without waiting.
  uint32_t staleAfterMs = 0;
  BeginTarget target = BeginTarget::Plain;
};

enum class BeginAction : uint8_t {
  Refuse,
  // The transfer in flight has gone quiet past `staleAfterMs`: abandon it
  // silently (no `ERR`, it is nobody's transfer any more), then carry on with
  // this begin. A sender that died without disconnecting would otherwise leave
  // the receiver stuck busy for ever, so a long-idle transfer is reclaimed
  // rather than defended. This is the only timeout in the receiver, and it
  // deliberately fires here -- on the host task, when the next begin arrives --
  // rather than from a poll on the activity task, which would have to close a
  // file this task owns.
  ReclaimStaleThenProceed,
  Proceed,
};

// Which gate answered. The reason string is what goes on the wire; this is
// what the receiver switches on, because two of these refusals also carry the
// offending path into the log and a reason string cannot say which one fired.
// Comparing the strings instead would work right up until two gates share one.
enum class BeginGate : uint8_t {
  None,
  UnsafePath,
  NotSubscribed,
  Busy,
  ForeignWalletPath,
};

struct BeginDecision {
  BeginAction action = BeginAction::Proceed;
  BeginGate gate = BeginGate::None;
  // Only set when action is Refuse.
  const char* reason = nullptr;
};

// The begin ladder, in the order the receiver has always run it.
inline BeginDecision decideBeginGates(const BeginGateFacts& f) {
  if (!f.pathSafe) return {BeginAction::Refuse, BeginGate::UnsafePath, "bad path"};
  if (!f.statusSubscribed) return {BeginAction::Refuse, BeginGate::NotSubscribed, "status not subscribed"};

  BeginAction proceed = BeginAction::Proceed;
  if (f.transferActive) {
    if (f.msSinceLastFrame < f.staleAfterMs) return {BeginAction::Refuse, BeginGate::Busy, "busy"};
    proceed = BeginAction::ReclaimStaleThenProceed;
  }

  // Under the wallet and not one of the two things the format puts there.
  // **Before the caller creates a directory**, which creates every parent
  // component and cannot be undone.
  if (f.target == BeginTarget::WalletForeign) {
    return {BeginAction::Refuse, BeginGate::ForeignWalletPath, "not a wallet file"};
  }

  return {proceed, BeginGate::None, nullptr};
}

// --- 4. The wallet's own gates ---------------------------------------------
//
// Asked after the card path is built and before a single byte is opened for
// write, so every refusal here leaves the card exactly as it was.

struct WalletGateFacts {
  BeginTarget target = BeginTarget::Plain;
  // The session holds a key: the wallet is unlocked. A locked wallet gets a
  // refusal rather than a half-written document -- storing the plaintext "for
  // now" is the one outcome this whole rung exists to prevent
  // (docs/wallet-provisioning.md).
  bool walletUnlocked = false;
  // A live panel is attached. A wallet asset's geometry is checked against it
  // before the device seals the bitmap, and only a screen can supply one; a
  // nullptr renderer means "this screen cannot take wallet assets".
  bool havePanel = false;
  // The begin frame's declared length, against...
  uint32_t declaredTotal = 0;
  // ...what this device can seal in one shot (wallet::kMaxManifestPlaintextBytes).
  // A fact, not an include: this header stays free of the wallet.
  uint32_t manifestCapBytes = 0;  // The begin frame arrived inside a sync-channel session: a phone the rider
  // paired, a fresh Noise session, the frame opened under its key
  // (SyncChannel.h, T-2098). **False is the default and the refusal**: a wallet
  // document from anything else -- a stranger in radio range, a plain BLE write,
  // the map screen -- is refused before the lock is even asked about, because
  // plaintext on the air is the thing the channel exists to end.
  bool sealedSession = false;
};

enum class WalletGate : uint8_t {
  None,
  // Not inside a sync session. First in the ladder: whether the wallet is
  // locked is nothing a stranger should learn.
  NotPaired,
  Locked,
  // Logged with both numbers, because the person reading it has to decide what
  // to drop from the wallet -- which is why this one needs a gate and not just
  // a string.
  ManifestTooBig,
  NoPanel,
};

struct WalletGateDecision {
  WalletGate gate = WalletGate::None;
  const char* reason = nullptr;
};

// `gate == None` means the wallet has nothing to object to -- which includes
// every transfer that is not a wallet transfer at all.
inline WalletGateDecision decideWalletGates(const WalletGateFacts& f) {
  const bool walletTarget = f.target == BeginTarget::WalletManifest || f.target == BeginTarget::WalletAsset;
  if (walletTarget && !f.sealedSession) return {WalletGate::NotPaired, "not paired"};
  if (f.target == BeginTarget::WalletManifest) {
    if (!f.walletUnlocked) return {WalletGate::Locked, "wallet locked"};
    // The list of documents is buffered whole and sealed once, so this refusal
    // is answered here, before a byte is taken: the begin frame carries the
    // length, so an oversized manifest costs nothing on the wire.
    if (f.declaredTotal > f.manifestCapBytes) return {WalletGate::ManifestTooBig, "manifest too big"};
  }
  if (f.target == BeginTarget::WalletAsset) {
    if (!f.walletUnlocked) return {WalletGate::Locked, "wallet locked"};
    if (!f.havePanel) return {WalletGate::NoPanel, "no panel"};
  }
  return {WalletGate::None, nullptr};
}

// --- 5. The chunk state machine --------------------------------------------

struct ChunkFacts {
  bool transferActive = false;
  // Length of the frame past the opcode: the 4-byte offset plus the payload.
  size_t bodyLen = 0;
  // The offset the frame declares.
  uint32_t offset = 0;
  // Bytes already written for this transfer.
  uint32_t received = 0;
  // What the begin frame declared.
  uint32_t declaredTotal = 0;
};

enum class ChunkAction : uint8_t {
  // `ERR <reason>`, and nothing in flight is touched. Only one case: a chunk
  // with no transfer behind it.
  Refuse,
  // `ERR <reason>`, the `.part` is closed and deleted and the transfer ends.
  Abandon,
  // Write it.
  Accept,
};

enum class ChunkGate : uint8_t {
  None,
  NoTransfer,
  ShortChunk,
  // Logged with the offset it carried and the one it should have, because that
  // pair is the whole diagnosis of a repeat, a gap or a reorder.
  BadOffset,
  Overrun,
};

struct ChunkDecision {
  ChunkAction action = ChunkAction::Accept;
  ChunkGate gate = ChunkGate::None;
  const char* reason = nullptr;
  // **These two describe the frame, not the verdict**, so they are filled as
  // soon as they are known rather than only on Accept: `payloadLen` once the
  // frame is long enough to have one, `isFirst` once the offset lines up. The
  // receiver's first-chunk diagnostic prints both and has always run after the
  // offset check and *before* the overrun check, so an overrunning first chunk
  // logs its real payload size on the way to being abandoned.
  size_t payloadLen = 0;
  // This is the transfer's first chunk. Worth one log line per transfer rather
  // than per chunk -- the payload size against the negotiated MTU is the
  // diagnostic for a sender that never negotiated one, and logging it per chunk
  // would itself eat into the airtime it is diagnosing.
  bool isFirst = false;
  // These bytes finish the file: the completion pass runs after they are
  // written. Equivalent to the receiver's `received_ < declaredTotal_` test
  // after the increment, because the overrun gate above has already excluded
  // every case where the sum could exceed the total.
  bool completesTransfer = false;
};

inline ChunkDecision decideChunk(const ChunkFacts& f) {
  if (!f.transferActive) return {ChunkAction::Refuse, ChunkGate::NoTransfer, "no transfer", 0, false, false};
  // A header and nothing behind it is not a chunk. `<=`, not `<`: a frame
  // carrying only the offset moves no bytes, so accepting it would make the
  // offset arithmetic below a no-op the sender could repeat for ever.
  if (f.bodyLen <= kChunkHeaderBytes) {
    return {ChunkAction::Abandon, ChunkGate::ShortChunk, "short chunk", 0, false, false};
  }

  const size_t payloadLen = f.bodyLen - kChunkHeaderBytes;

  // The offset is checked against bytes actually written, so this catches a
  // repeat, a gap and a reorder with one comparison. On a single BLE link with
  // write-with-response none of the three should ever happen; if one does, the
  // transfer is wrong and finishing it would produce a file that passes no
  // check anyway.
  if (f.offset != f.received) {
    return {ChunkAction::Abandon, ChunkGate::BadOffset, "offset", payloadLen, false, false};
  }

  // `offset == 0` and `received == 0` (equal, checked above) can only be the
  // transfer's first chunk.
  const bool isFirst = f.offset == 0;

  // Promoted to 64-bit for the comparison, which the receiver's own 32-bit
  // version did not need and this one does not either: `received` is bounded by
  // `declaredTotal` (<= 8 MB) and `payloadLen` by the ATT MTU, so the sum
  // cannot wrap. Written wide anyway because the bound is an invariant of the
  // caller rather than of this function, and a future caller that breaks it
  // should get a refusal and not an overflow.
  if (static_cast<uint64_t>(f.received) + payloadLen > f.declaredTotal) {
    return {ChunkAction::Abandon, ChunkGate::Overrun, "overrun", payloadLen, isFirst, false};
  }

  ChunkDecision d;
  d.action = ChunkAction::Accept;
  d.payloadLen = payloadLen;
  d.isFirst = isFirst;
  d.completesTransfer = static_cast<uint64_t>(f.received) + payloadLen == f.declaredTotal;
  return d;
}

}  // namespace maptransfer
