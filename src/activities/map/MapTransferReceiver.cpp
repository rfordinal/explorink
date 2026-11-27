#include "MapTransferReceiver.h"

#include <Arduino.h>
#include <BlePositionServer.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_rom_crc.h>

#include <cstdio>
#include <cstring>

#include "MapByteFormat.h"

#if defined(ENABLE_WALLET) && ENABLE_WALLET
#include "../wallet/WalletCryptoDevice.h"
#include "../wallet/WalletManifestInstall.h"
#include "../wallet/WalletStore.h"
#endif

namespace {

constexpr const char* kLogTag = "MAPXFER";

// Opcodes. See the wire format block in MapTransferReceiver.h.
constexpr uint8_t kOpBegin = 0x01;
constexpr uint8_t kOpChunk = 0x02;
constexpr uint8_t kOpAbort = 0x03;

// Bytes after the opcode: totalLen + crc32 + pathLen.
constexpr size_t kBeginFixedBytes = 9;
// Bytes after the opcode: offset.
constexpr size_t kChunkHeaderBytes = 4;

// A sanity ceiling, not a real limit. The whole point of this channel is the
// small stuff -- a corridor of tiles, a safety layer, a trip file. Anything
// bigger belongs on the card over WiFi/SD, and a declared length in the
// hundreds of MB is a corrupt frame, not a plan.
constexpr uint32_t kMaxFileBytes = 8u * 1024u * 1024u;

// Read-back buffer for the completion CRC check. One-shot heap allocation,
// held only for the duration of computePartCrc32() (freed at scope exit,
// before the OK/ERR status line goes out) -- not a member, so it costs
// nothing for the rest of the transfer's lifetime.
//
// 4096 matches MapTileReader::streamBuffer_ (docs/map-memory.md:138), the
// buffer size the same HalStorage/SD path is already measured at: 550-608
// KB/s across every rung, tile and layer mix
// (firmware/explorink/docs/optimization/01-render-pipeline.md:175-187,
// "Card versus compute, measured 2026-08-06"). That is a different call site
// (MapTileReader's streamed layer reads, not this readback), so treat it as
// an informed estimate for this path, not a measurement of it -- open until
// someone times computePartCrc32() itself.
//
// Arithmetic against the 30 s ATT transaction budget (the final chunk's ATT
// write response is withheld for this whole completion pass -- ble-fix-plan.md
// T5.4): kMaxFileBytes (8,388,608 B) / 4096 B = 2048 reads, exact. At the
// conservative 550 KB/s bound that is 8,388,608 / (550 * 1024) ~= 14.9 s;
// at the measured upper bound (608 KB/s) ~= 13.5 s. Either way it fits inside
// the 30 s budget with roughly half the budget (~15 s) to spare, so
// kMaxFileBytes does not need to shrink. The old 128 B buffer needed 65,536
// reads for the same 8 MB file -- this removes 32x the mutex/SD-command round
// trips, which is the actual defect (docs/ble-review-2026-08.md, "CRC
// readback blocks the host task with a 128 B buffer").
constexpr size_t kCrcReadBufferBytes = 4096;

// Fallback path when the 4096 B heap allocation above fails (OOM). Kept
// compiled, not deleted -- ble-fix-plan.md T5.4 requires it stay reachable.
// Same size FontDownloadActivity uses for the same job -- 128 bytes of stack,
// well under CLAUDE.md's 256-byte cap on locals.
constexpr size_t kCrcFallbackReadBufferBytes = 128;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// Little-endian u32 out of an unaligned BLE attribute buffer. memcpy, never a
// cast-and-deref: the ESP32-C3 faults on an unaligned wide load (firmware
// CLAUDE.md, "RISC-V Alignment").
uint32_t readU32(const uint8_t* p) {
  uint32_t value;
  memcpy(&value, p, sizeof(value));
  return value;
}

// Holds MapTransferReceiver::inFrame_ true for the lifetime of the frame
// callback, so detach() on the activity task can wait the frame out.
//
// A guard object rather than a set/clear pair, even though onFrame has a single
// exit today: a clear left behind by a later early `return` would leave
// inFrame_ stuck true, and a stuck-true flag makes every later detach() burn
// its whole kDetachWaitMs bound and log a failure that did not happen. A
// destructor cannot be skipped.
struct FrameGuard {
  std::atomic<bool>& flag;
  explicit FrameGuard(std::atomic<bool>& f) : flag(f) { flag.store(true, std::memory_order_relaxed); }
  ~FrameGuard() { flag.store(false, std::memory_order_release); }
  FrameGuard(const FrameGuard&) = delete;
  FrameGuard& operator=(const FrameGuard&) = delete;
};

#if defined(ENABLE_WALLET) && ENABLE_WALLET
// --- The wallet's three primitives, bound to this device --------------------
//
// WalletAssetIngest.h is pure arithmetic and takes these as callbacks, for the
// same reason WalletCrypto.h's ctrXorInPlace does: the offset maths is the part
// that gets a windowed CTR read wrong, so it exists once and is host-tested
// once, against OpenSSL and zlib instead of mbedtls and the ROM.

// AES-256-CTR, which is symmetric -- the same call the read path decrypts with
// encrypts here.
//
// **The key is fetched here, every chunk, and never captured at begin.** A
// pointer taken once would still be a valid, non-null pointer after the wallet
// locked: `Session::clear()` wipes `key_` in place and drops a flag, it does not
// move or free anything (`WalletCryptoDevice.cpp`). So a lock part-way through a
// transfer -- the rider pressing power, which calls `clear("sleep")` several
// hundred milliseconds before the chip actually resets (`main.cpp`), or
// `CMD:WALLETLOCK` -- would have silently enciphered the rest of the asset under
// 32 zero bytes.
//
// That failure had no error in it anywhere, which is what made it worth a
// re-read per chunk rather than a comment: both end-of-transfer checks are over
// the **plaintext**, so the hash and the CRC would both still have passed, and
// the half-good file would have been renamed over the previous good one.
// Permanently undecryptable, and the asset it replaced gone. Found in review,
// 2026-11-27.
//
// Cost is a flag test and a pointer load per chunk, against AES over the same
// bytes. `ctx` is unused and stays in the signature because the sealer's
// callback contract is shared with the host tests, which do bind a key to it.
bool walletCipher(void*, const uint8_t* iv, uint32_t payloadOffset, uint8_t* data, size_t len) {
  const uint8_t* key = wallet::Session::instance().key();
  if (key == nullptr) return false;
  return wallet::ctrDecryptInPlace(key, iv, payloadOffset, data, len);
}

// The ROM's CRC-32, which is the one that has to produce the number the phone
// compares against (it computes zlib.crc32 over the same bytes).
uint32_t walletCrc32(uint32_t crc, const uint8_t* data, size_t len) { return esp_rom_crc32_le(crc, data, len); }

bool walletSink(void* ctx, const uint8_t* data, size_t len) {
  HalFile* file = static_cast<HalFile*>(ctx);
  return file != nullptr && file->write(data, len) == len;
}

// --- The manifest sealer's three primitives ---------------------------------

// A fresh nonce per write, off the hardware RNG. Fresh is the requirement, not
// unpredictable-looking: the manifest is written again on every sync, and one
// key with one nonce over two different plaintexts breaks GCM outright.
void walletRandom(void*, uint8_t* out, size_t len) { wallet::randomBytes(out, len); }

// The key is read here, at the end of the transfer, for the same reason
// walletCipher() reads it per chunk: a wallet locked while the manifest was
// arriving must end the transfer, not seal the list under 32 zero bytes.
bool walletSealManifest(void*, const uint8_t* nonce, uint8_t* data, size_t len, uint8_t* tag) {
  const uint8_t* key = wallet::Session::instance().key();
  if (key == nullptr) return false;
  return wallet::gcmEncryptInPlace(key, nonce, wallet::kGcmNonceLen, data, len, tag, wallet::kGcmTagLen);
}

// "Is this a wallet manifest, and which version does it say it is?" -- asked
// with the parser the browse screen uses, so a body that passes here is a body
// that opens later. Anything stricter would be a second opinion about what a
// manifest is, and the two opinions would drift.
//
// Heap, not stack: ManifestParser carries StreamingJsonParser's 512-byte token
// buffer and this runs on the NimBLE host task's 4 KB stack.
bool walletInspectManifest(void*, const char* json, size_t len, uint32_t& walletVersion) {
  auto parser = makeUniqueNoThrow<wallet::ManifestParser>();
  if (!parser) {
    LOG_ERR(kLogTag, "OOM: manifest parser");
    return false;
  }
  // List mode with nowhere to store: the whole document is walked, the header
  // fields come out filled, not one entry is copied.
  parser->beginList(nullptr, 0);
  parser->feed(json, len);
  if (parser->hasError()) return false;
  if (parser->formatVersion() != wallet::kSupportedFormatVersion) return false;
  walletVersion = parser->walletVersion();
  return true;
}
#endif

}  // namespace

MapTransferReceiver::MapTransferReceiver(const char* rootDir, const GfxRenderer* renderer)
    : rootDir_(rootDir), renderer_(renderer) {}

MapTransferReceiver::~MapTransferReceiver() {
  // No status to send and nobody to send it to by now -- just don't leave a
  // .part behind. detach() is the ordinary path; this is the backstop.
  if (active_) abandon(nullptr);
}

bool MapTransferReceiver::resetCounters() {
  if (active_) return false;
  // g_mux is the actual synchronisation (MapTransferReceiver.h, above
  // completed_): it only excludes the host task's increments because those
  // increments take the same lock (handleChunk's completion path, abandon(),
  // refuse()). Without that on both sides, this critical section excluded
  // nothing -- the host task could still land ++completed_ between this
  // store and the next status() read.
  portENTER_CRITICAL(&g_mux);
  completed_ = 0;
  completedBytes_ = 0;
  failed_ = 0;
  portEXIT_CRITICAL(&g_mux);
  publish();
  return true;
}

void MapTransferReceiver::attach() {
  freeink::BlePositionServer::TransferHooks hooks;
  hooks.ctx = this;
  hooks.onFrame = &frameTrampoline;
  hooks.onDisconnect = &disconnectTrampoline;
  attached_ = true;
  freeink::BlePositionServer::getInstance().setTransferHooks(hooks);
}

void MapTransferReceiver::detach() {
  // Flag first, hooks second: a frame that has not reached the attached_ check
  // yet sees false and returns without opening anything, and clearing the
  // hooks stops any further frame from arriving at all.
  attached_ = false;
  freeink::BlePositionServer::getInstance().setTransferHooks(freeink::BlePositionServer::TransferHooks{});

  // A frame already *past* that check is the one that matters, and it is the
  // normal case: a Back press mid-transfer lands here while the host task sits
  // in file_.write() on the card. abandon() below closes file_, deletes the
  // .part and zeroes active_/received_/declaredTotal_ -- all of it state that
  // frame reads and writes when its write returns. Two tasks, no lock, so wait
  // the frame out instead.
  //
  // vTaskDelay, not a busy spin: this runs on the activity task, which is
  // priority 1 against the NimBLE host task's configMAX_PRIORITIES - 4
  // (nimble_port_freertos.c:44). A busy spin would still be preempted, it
  // would just burn the CPU the frame needs to finish.
  const uint32_t waitStart = millis();
  while (inFrame_.load(std::memory_order_acquire)) {
    if (millis() - waitStart >= kDetachWaitMs) {
      // The completion pass reads a whole tile back off the card and can
      // outlast the bound (MapTransferReceiver.h, kDetachWaitMs). Proceeding
      // is deliberate: a stuck card would otherwise hold the screen on a Back
      // press for as long as it stays stuck. handleChunk re-checks active_
      // after every blocking call and bails, so the frame notices.
      LOG_ERR(kLogTag, "detach: frame still in flight after %lu ms, proceeding",
              static_cast<unsigned long>(kDetachWaitMs));
      break;
    }
    vTaskDelay(1);
  }

  if (active_) abandon("screen closed");
}

void MapTransferReceiver::frameTrampoline(void* ctx, const uint8_t* data, size_t len) {
  static_cast<MapTransferReceiver*>(ctx)->onFrame(data, len);
}

void MapTransferReceiver::disconnectTrampoline(void* ctx) {
  auto* self = static_cast<MapTransferReceiver*>(ctx);
  if (self->active_) {
    LOG_ERR(kLogTag, "link dropped at %lu/%lu bytes, dropping %s", static_cast<unsigned long>(self->received_),
            static_cast<unsigned long>(self->declaredTotal_), self->partPath_);
    // No status line: the peer that would read it is the one that just went
    // away. Resuming across a reconnect is deliberately not built.
    self->abandon(nullptr);
    self->publish();
  }
}

void MapTransferReceiver::onFrame(const uint8_t* data, size_t len) {
  if (!attached_ || data == nullptr || len == 0) return;

  // Everything below can touch transfer state, so from here on detach() must
  // wait for this call to return. Declared before the dispatch and never
  // cleared by hand.
  //
  // Today every switch case `break`s into the shared lastFrameMs_/publish()
  // tail, so there is exactly one exit past this line -- falling off the end.
  // The guard is still a guard and not a clear-at-the-bottom, because the next
  // person to add an early `return` inside the switch will not come back up
  // here to add the clear.
  //
  // The gap between the attached_ read above and this store is not a hole on
  // this hardware: the ESP32-C3 is single core and the host task outranks the
  // activity task (see detach()), so the activity task cannot run between two
  // adjacent instructions of this task. It never gets to observe the gap.
  FrameGuard guard(inFrame_);

  const uint8_t opcode = data[0];
  switch (opcode) {
    case kOpBegin:
      handleBegin(data + 1, len - 1);
      break;
    case kOpChunk:
      handleChunk(data + 1, len - 1);
      break;
    case kOpAbort:
      if (active_) {
        LOG_INF(kLogTag, "sender aborted at %lu/%lu bytes", static_cast<unsigned long>(received_),
                static_cast<unsigned long>(declaredTotal_));
        abandon("aborted");
      }
      break;
    default:
      refuse("bad opcode");
      break;
  }

  lastFrameMs_ = millis();
  publish();
}

void MapTransferReceiver::handleBegin(const uint8_t* body, size_t len) {
  if (len < kBeginFixedBytes + 1) {
    refuse("short begin");
    return;
  }

  const uint32_t total = readU32(body + 0);
  const uint32_t crc = readU32(body + 4);
  const size_t pathLen = body[8];

  // Exact length, not "at least": trailing bytes in a begin frame mean the
  // sender and this parser disagree about the format, and guessing which one
  // is right is how a wire format rots.
  if (pathLen == 0 || pathLen > kMaxRelPathBytes || len != kBeginFixedBytes + pathLen) {
    refuse("bad path length");
    return;
  }
  if (total == 0 || total > kMaxFileBytes) {
    refuse("bad length");
    return;
  }

  // A multi-KB push at a 15 B (default MTU 23) payload runs at 1/25th the
  // airtime a negotiated MTU gives it (docs/ble-review-2026-08.md,
  // "Performance"). Accepted either way -- this is a diagnostic, not a
  // policy -- but silent until now, so a misconfigured sender looked like a
  // slow SD card instead of what it is.
  //
  // No LOG_WRN in this tree (Logging.h has only ERR/INF/DBG) -- LOG_ERR is
  // this file's existing choice for "worth a look but not a refusal" (see
  // the stale-transfer reclaim below).
  const uint16_t payloadBytes = freeink::BlePositionServer::getInstance().transferPayloadBytes();
  if (payloadBytes < 100 && total > 4096) {
    LOG_ERR(kLogTag, "large transfer (%lu B) on a %u B payload -- misconfigured sender (MTU not negotiated?)",
            static_cast<unsigned long>(total), static_cast<unsigned>(payloadBytes));
  }

  char rel[kMaxRelPathBytes + 1];
  memcpy(rel, body + kBeginFixedBytes, pathLen);
  rel[pathLen] = '\0';
  if (!relPathIsSafe(rel, pathLen)) {
    LOG_ERR(kLogTag, "rejected path: %s", rel);
    refuse("bad path");
    return;
  }

  // The status channel is where every verdict goes. Starting a transfer with
  // nobody subscribed means writing a file and never being able to say
  // whether it is good, which is worse than not starting.
  if (!freeink::BlePositionServer::getInstance().isTransferSubscribed()) {
    refuse("status not subscribed");
    return;
  }

  if (active_) {
    // A sender that died without disconnecting leaves this stuck busy
    // forever, so a long-idle transfer is reclaimed rather than defended.
    if (millis() - lastFrameMs_ < kStaleTransferMs) {
      refuse("busy");
      return;
    }
    LOG_ERR(kLogTag, "reclaiming stale transfer of %s at %lu bytes", partPath_, static_cast<unsigned long>(received_));
    abandon(nullptr);
  }

  snprintf(finalPath_, sizeof(finalPath_), "%s/%s", rootDir_, rel);
  snprintf(partPath_, sizeof(partPath_), "%s.part", finalPath_);

  // `base/13/4482/2789.tib` needs /explorink/base/13/4482 to exist first.
  // ensureDirectoryExists creates parents (SdFat's mkdir defaults to pFlag).
  //
  // Truncated in place and put back rather than copied into a second buffer:
  // this runs on the NimBLE host task, whose stack is 4 KB and which already
  // has a 512-byte VLA of NimBLE's own below this frame (NimBLEServer.cpp's
  // BLE_GATT_ACCESS_OP_WRITE_CHR case) before SdFat's write path is entered
  // on top.
  if (char* lastSlash = strrchr(finalPath_, '/'); lastSlash != nullptr && lastSlash != finalPath_) {
    *lastSlash = '\0';
    const bool dirReady = Storage.ensureDirectoryExists(finalPath_);
    *lastSlash = '/';
    if (!dirReady) {
      refuse("mkdir failed");
      return;
    }
  }

#if defined(ENABLE_WALLET) && ENABLE_WALLET
  // A wallet asset is sealed on the way in, and the decision is made here --
  // before a single byte is opened for write -- so every refusal below leaves
  // the card exactly as it was. The previous transfer's machine is dropped
  // whatever this one turns out to be, so a tile push can never inherit one.
  walletIngest_.reset();
  // Freed here and not at the end of the previous transfer's abandon path: it
  // owns up to 32 KB, and abandon() can be running on the activity task while
  // the host task is still inside feed() (see abandon()).
  manifestIngest_.reset();
  if (wallet::pathIsWalletManifest(finalPath_)) {
    // The list of documents. Buffered whole and sealed once, which is why every
    // refusal it can answer is answered here, before a byte is taken: the begin
    // frame carries the length, so an oversized manifest costs nothing on the
    // wire (WalletManifestIngest.h, "Why this one is buffered").
    const uint8_t* key = wallet::Session::instance().key();
    if (key == nullptr) {
      refuse("wallet locked");
      return;
    }
    if (total > wallet::kMaxManifestPlaintextBytes) {
      // Said with both numbers, because the person reading it has to decide
      // what to drop from the wallet.
      LOG_ERR(kLogTag, "manifest is %lu bytes, this device can seal at most %u", static_cast<unsigned long>(total),
              static_cast<unsigned>(wallet::kMaxManifestPlaintextBytes));
      refuse("manifest too big");
      return;
    }
    auto ingest = makeUniqueNoThrow<wallet::ManifestIngest>();
    auto buffer = makeUniqueNoThrow<uint8_t[]>(wallet::sealedManifestLen(total));
    if (!ingest || !buffer) {
      // Under the cap and still no memory: BLE holds the big blocks, so this is
      // a message rather than a crash (WalletCrypto.h, the cap's comment).
      LOG_ERR(kLogTag, "OOM: %u bytes for the manifest", static_cast<unsigned>(wallet::sealedManifestLen(total)));
      refuse("oom");
      return;
    }
    wallet::ManifestIngest::Callbacks cb;
    cb.crc32 = walletCrc32;
    cb.inspect = walletInspectManifest;
    cb.random = walletRandom;
    cb.seal = walletSealManifest;
    const wallet::ManifestIngestStatus started =
        ingest->begin(total, crc, std::move(buffer), wallet::sealedManifestLen(total), cb);
    if (started != wallet::ManifestIngestStatus::Ok) {
      refuse(wallet::manifestIngestReason(started));
      return;
    }
    manifestIngest_ = std::move(ingest);
    // Everything below is shared with every other transfer. The two places this
    // one differs -- no `.part` file is opened, and the rename at the end is
    // replaced by the install -- are both written as a test of this pointer,
    // rather than as a second copy of the begin and completion tails that would
    // drift away from them.
  }
  if (wallet::pathIsWalletAsset(finalPath_)) {
    // No key means a locked wallet, and a locked wallet gets a refusal rather
    // than a half-written document: storing the plaintext "for now" is the one
    // outcome this rung exists to prevent (docs/wallet-provisioning.md).
    const uint8_t* key = wallet::Session::instance().key();
    if (key == nullptr) {
      refuse("wallet locked");
      return;
    }
    // The geometry gate needs the live panel, and only a screen can supply it.
    if (renderer_ == nullptr) {
      refuse("no panel");
      return;
    }
    auto ingest = makeUniqueNoThrow<wallet::AssetIngest>();
    if (!ingest) {
      LOG_ERR(kLogTag, "OOM: wallet ingest");
      refuse("oom");
      return;
    }
    wallet::AssetIngest::Callbacks cb;
    cb.cipher = walletCipher;
    // No key is carried: walletCipher() asks the session for it on every chunk,
    // so a wallet locked mid-transfer ends the transfer instead of sealing the
    // rest of it under zeros. The `key == nullptr` check above is the "do not
    // even start" half of the same rule.
    cb.cipherCtx = nullptr;
    cb.crc32 = walletCrc32;
    cb.write = walletSink;
    // Stable for the life of this object, and nothing is written through it
    // until the first chunk arrives -- by which time the file below is open.
    cb.writeCtx = &file_;
    if (!ingest->begin(total, crc, wallet::livePanel(*renderer_), cb)) {
      refuse("bad length");
      return;
    }
    walletIngest_ = std::move(ingest);
  }
#endif

  // A leftover .part from an earlier killed transfer is not resumed -- the
  // whole file is coming again, from offset 0 (O_TRUNC does the rest).
  //
  // **A manifest opens nothing here.** Its bytes are buffered in RAM and the
  // card is touched once, at the end, by the install -- so there is no `.part`
  // for it to truncate and no handle for it to hold open across the transfer.
  bool needsPartFile = true;
#if defined(ENABLE_WALLET) && ENABLE_WALLET
  needsPartFile = !manifestIngest_;
#endif
  if (needsPartFile && !Storage.openFileForWrite("MAPXFER", partPath_, file_)) {
#if defined(ENABLE_WALLET) && ENABLE_WALLET
    // The sealer was built above and has no transfer behind it now. Dropped
    // here rather than left for the next begin frame, so "a live sealer exists"
    // and "a transfer is in flight" never disagree.
    walletIngest_.reset();
#endif
    refuse("open failed");
    return;
  }

  active_ = true;
  verifying_ = false;
  declaredTotal_ = total;
  declaredCrc_ = crc;
  received_ = 0;
  // Which tile this is, so a screen can put the byte counts above against one
  // row instead of a batch total. A non-tile push (route, style) parses false
  // and simply has no row to be about.
  activeTileValid_ = parseMapTilePath(finalPath_, activeTile_);

  char reply[48];
  snprintf(reply, sizeof(reply), "RDY %lu", static_cast<unsigned long>(total));
  freeink::BlePositionServer::getInstance().sendTransferStatus(reply);
  LOG_INF(kLogTag, "begin %s, %lu bytes, crc %08lx", finalPath_, static_cast<unsigned long>(total),
          static_cast<unsigned long>(crc));
}

void MapTransferReceiver::handleChunk(const uint8_t* body, size_t len) {
  if (!active_) {
    refuse("no transfer");
    return;
  }
  if (len <= kChunkHeaderBytes) {
    abandon("short chunk");
    return;
  }

  const uint32_t offset = readU32(body);
  const uint8_t* payload = body + kChunkHeaderBytes;
  const size_t payloadLen = len - kChunkHeaderBytes;

  // The offset is checked against bytes actually written, so this catches a
  // repeat, a gap and a reorder with one comparison. On a single BLE link with
  // write-with-response none of the three should ever happen; if one does, the
  // transfer is wrong and finishing it would produce a file that passes no
  // check anyway.
  if (offset != received_) {
    LOG_ERR(kLogTag, "chunk offset %lu, expected %lu", static_cast<unsigned long>(offset),
            static_cast<unsigned long>(received_));
    abandon("offset");
    return;
  }

  // Offset 0 and received_ == 0 (checked above) can only be the transfer's
  // first chunk -- logged once per transfer, not once per chunk, which would
  // itself eat into the airtime this is diagnosing (docs/ble-review-2026-08.md,
  // "Performance", item 4).
  if (offset == 0) {
    LOG_INF("MAPTX", "first chunk payload %u B (mtu %u)", static_cast<unsigned>(payloadLen),
            static_cast<unsigned>(freeink::BlePositionServer::getInstance().negotiatedMtu()));
  }

  if (received_ + payloadLen > declaredTotal_) {
    abandon("overrun");
    return;
  }

#if defined(ENABLE_WALLET) && ENABLE_WALLET
  if (manifestIngest_) {
    // Into RAM, nowhere near the card. The last chunk is the one that seals:
    // feed() answers Complete only after the CRC matched, the body inspected as
    // a manifest and the GCM call returned, so by the time the completion path
    // below runs there is a finished file to install or nothing at all.
    const wallet::ManifestIngestStatus fed = manifestIngest_->feed(payload, payloadLen);
    if (!active_) return;
    if (fed != wallet::ManifestIngestStatus::Ok && fed != wallet::ManifestIngestStatus::Complete) {
      LOG_ERR(kLogTag, "manifest refused: %s", wallet::manifestIngestReason(fed));
      abandon(wallet::manifestIngestReason(fed));
      return;
    }
  } else if (walletIngest_) {
    // A wallet asset is encrypted into a scratch copy and written from there,
    // so this one path does copy -- the arriving bytes are NimBLE's buffer and
    // are not ours to overwrite. Everything the sealer needs (the header, the
    // IV, the running plaintext hash and CRC) is inside it; the receiver's own
    // counters below are unchanged, because the file is the same length sealed
    // as it was in the clear.
    const wallet::IngestStatus fed = walletIngest_->feed(payload, payloadLen);
    // feed() writes to the card, so detach() can have abandoned this transfer
    // underneath it -- same re-check, same reason as the plain path below.
    if (!active_) return;
    if (fed != wallet::IngestStatus::Ok && fed != wallet::IngestStatus::Complete) {
      LOG_ERR(kLogTag, "wallet ingest refused %s: %s", finalPath_, wallet::ingestStatusReason(fed));
      abandon(wallet::ingestStatusReason(fed));
      return;
    }
  } else
#endif
  {
    // Straight from the NimBLE attribute buffer to the card: no copy, no queue,
    // no buffer of our own. The ATT write response the stack sends when this
    // callback returns is therefore "the bytes are on the card", which is what
    // makes it usable as flow control (MapTransferReceiver.h, Threading).
    if (file_.write(payload, payloadLen) != payloadLen) {
      if (!active_) return;
      abandon("write failed");
      return;
    }
  }
  // Every blocking call below is a place detach() can have run its full wait,
  // given up, and abandoned this transfer underneath us -- file closed, .part
  // deleted, counters zeroed. Re-checking active_ after each one turns that
  // from a bogus completion pass (spurious ERR to the phone, double-counted
  // failed_) into a silent bail, which is the correct answer: the screen this
  // transfer belonged to is gone.
  //
  // A plain read of a plain bool is enough. Single core, so no cache
  // coherence to worry about, and each check sits directly after a call into
  // another translation unit (HalStorage, no LTO in platformio.ini), so the
  // compiler must reload the member rather than reuse a cached copy.
  if (!active_) return;
  received_ += payloadLen;

  if (received_ < declaredTotal_) return;

  // --- complete ---
  // Every byte is on the card and the verdict work starts. Published right
  // here, not at the end of the frame like everything else: what follows is a
  // close, a whole-file read-back and a rename, seconds of it on a megabyte
  // tile, and the frame's own publish() at the bottom of onFrame() only runs
  // once all of that is over. Without this the activity task sees the previous
  // chunk's counters for the whole pause and the panel reads as hung.
  verifying_ = true;
  publish();

  // Closed before the read-back: the same HalFile cannot be open for write
  // and reopened for read (firmware CLAUDE.md, DESTRUCTOR_CLOSES_FILE), and
  // the rename below needs the handle gone too.
  //
  // **Guarded, because a manifest transfer never opened one.** HalFile::close()
  // asserts on a handle that was never opened (HalStorage.cpp,
  // HAL_FILE_WRAPPED_CALL) -- isOpen() is the one accessor that tolerates it,
  // which is why abandon() has used this exact shape all along.
  if (file_.isOpen()) file_.close();
  if (!active_) return;

  uint32_t actualCrc = 0;
#if defined(ENABLE_WALLET) && ENABLE_WALLET
  if (manifestIngest_) {
    // Same weaker guarantee as an asset, and for the same reason: the card
    // holds ciphertext, so the number the phone compares against is the CRC of
    // the plaintext as it arrived.
    actualCrc = manifestIngest_->plaintextCrc();
    const wallet::ManifestInstallStatus installed =
        wallet::installSealedManifest(manifestIngest_->sealed(), manifestIngest_->sealedLen());
    LOG_INF(kLogTag, "manifest walletVersion %lu", static_cast<unsigned long>(manifestIngest_->walletVersion()));
    // Freed before the verdict is sent: it holds the whole document list, and
    // the install is the last thing that needs it.
    manifestIngest_.reset();
    if (!active_) return;
    if (installed != wallet::ManifestInstallStatus::Ok) {
      abandon(wallet::manifestInstallReason(installed));
      return;
    }
  } else if (walletIngest_) {
    // **The reply means something weaker for a wallet asset, on purpose.** The
    // card holds ciphertext, so reading it back would answer a question nobody
    // asked -- the phone compares this number with the CRC of the plaintext it
    // sent (android-wallet.md 14.5). So `OK` here is "these bytes arrived whole
    // and were written", not "the card holds these bytes". Said out loud in
    // WalletAssetIngest.h and in docs/wallet-crypto.md rather than left to look
    // like the tile guarantee.
    //
    // Both end-of-transfer checks already ran inside feed(), before it answered
    // Complete: the CRC against what the sender declared, and the plaintext
    // hash against the prefix the keystream was built from. Neither can be
    // deferred to here, because this code only runs if they passed.
    actualCrc = walletIngest_->plaintextCrc();
  } else
#endif
  {
    if (!computePartCrc32(actualCrc)) {
      // Checked before the verdict, not after: the readback failing *because*
      // detach() deleted the .part out from under it is exactly the spurious
      // `ERR readback failed` this whole change exists to stop.
      if (!active_) return;
      abandon("readback failed");
      return;
    }
    if (actualCrc != declaredCrc_) {
      LOG_ERR(kLogTag, "crc mismatch: got %08lx, declared %08lx", static_cast<unsigned long>(actualCrc),
              static_cast<unsigned long>(declaredCrc_));
      abandon("crc mismatch");
      return;
    }
  }

  // SdFat's rename fails onto an existing name, and re-pushing a tile that is
  // already there is the normal case for a corridor update.
  //
  // **A manifest has already landed by now** -- under another name, through
  // installSealedManifest() above, which does its own verified two-rename swap.
  // There is no `.part` here to move and `finalPath_` (`manifest.json`) is a
  // file this device deliberately does not keep.
  bool needsRename = true;
#if defined(ENABLE_WALLET) && ENABLE_WALLET
  needsRename = !wallet::pathIsWalletManifest(finalPath_);
#endif
  if (needsRename) {
    if (Storage.exists(finalPath_) && !Storage.remove(finalPath_)) {
      if (!active_) return;
      abandon("replace failed");
      return;
    }
    if (!Storage.rename(partPath_, finalPath_)) {
      if (!active_) return;
      abandon("rename failed");
      return;
    }
  }
  // The rename succeeded, so a complete, CRC-checked file *is* on the card --
  // but if detach() ran, the counters below belong to a screen that is gone and
  // sendTransferStatus would indicate into a server about to be end()ed. Say
  // nothing; the phone times this tile out and asks again, which costs one
  // re-push and risks nothing.
  if (!active_) return;

  const uint32_t bytes = received_;
  active_ = false;
  verifying_ = false;
  received_ = 0;
  declaredTotal_ = 0;
  activeTileValid_ = false;
#if defined(ENABLE_WALLET) && ENABLE_WALLET
  // Freed here rather than kept for the next transfer: it holds a 512 B scratch
  // and a SHA-256 state, and the next push is usually a tile that needs
  // neither. Its destructor is also the last thing that wipes the scratch.
  walletIngest_.reset();
  // Already reset above, on the manifest branch -- this is the backstop for a
  // path that returned early past it.
  manifestIngest_.reset();
#endif
  // Same lock resetCounters() zeroes these under (MapTransferReceiver.h,
  // above completed_) -- otherwise its critical section excludes nothing.
  portENTER_CRITICAL(&g_mux);
  ++completed_;
  completedBytes_ += bytes;
  portEXIT_CRITICAL(&g_mux);

  // The path is the only place this class learns what the file *is*. A tile
  // path means a MissingTilesStore entry just became stale; the activity task
  // reads this and does the removal, because the store has exactly one writer
  // task (MapTransferReceiver.h, Status::lastTile). A non-tile push -- a route,
  // a style -- simply parses false and clears nothing.
  MapTileCoord landed;
  if (parseMapTilePath(finalPath_, landed)) {
    lastTile_ = landed;
    lastTileValid_ = true;
    ++tileSeq_;
  }

  char reply[48];
  snprintf(reply, sizeof(reply), "OK %lu %08lx", static_cast<unsigned long>(bytes),
           static_cast<unsigned long>(actualCrc));
  freeink::BlePositionServer::getInstance().sendTransferStatus(reply);
  // The high-water mark is the measurement behind the decision to do SD I/O
  // on the NimBLE host task rather than hand chunks to a worker: if this ever
  // gets close to zero, that decision is the thing to revisit.
  LOG_INF(kLogTag, "done %s, %lu bytes, crc %08lx, host task stack free %u", finalPath_,
          static_cast<unsigned long>(bytes), static_cast<unsigned long>(actualCrc),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void MapTransferReceiver::abandon(const char* reason) {
  // isOpen() is the one HalFile accessor that tolerates a handle that was
  // never opened; close() itself asserts on one. The completion path also
  // reaches here after its own close(), so this must be safe twice.
  if (file_.isOpen()) file_.close();
  if (partPath_[0] != '\0') Storage.remove(partPath_);

  // Those two calls block on the storage mutex, and detach() is the one caller
  // that runs them on the activity task. So this is the mirror of handleChunk's
  // active_ re-checks: if the host task finished the transfer while the two
  // calls above were queued behind it, it has already sent its own `OK` and
  // cleared active_. Reporting `ERR screen closed` on top of that OK would be
  // a second verdict for one transfer and a failed_ count for a file that
  // landed. Every reason != nullptr caller holds active_ == true on entry
  // (each is behind an `if (active_)` or handleChunk's "no transfer" gate), so
  // false here can only mean somebody else ended it.
  const bool endedElsewhere = !active_;

  active_ = false;
  verifying_ = false;
  received_ = 0;
  declaredTotal_ = 0;
  activeTileValid_ = false;
#if defined(ENABLE_WALLET) && ENABLE_WALLET
  // Wiped, **not freed**. This is the one place a transfer ends on the activity
  // task (detach(), past its kDetachWaitMs bound), and the host task can be
  // inside feed() right now -- deleting the object under it would be a
  // use-after-free, where overwriting its buffers is the same kind of data race
  // the file_ close/remove above already is, and ends the same way: a failed
  // write and a transfer that is already being abandoned. The memory goes back
  // at the next begin, which is host-task only.
  if (walletIngest_) walletIngest_->reset();
  // Same rule, and it matters more here: this one holds the whole document
  // list, so leaving it un-wiped would leave every title in a heap block until
  // the next transfer.
  if (manifestIngest_) manifestIngest_->reset();
#endif

  if (reason == nullptr || endedElsewhere) return;

  // Same lock resetCounters() zeroes this under -- see the comment on
  // the completion path above.
  portENTER_CRITICAL(&g_mux);
  ++failed_;
  portEXIT_CRITICAL(&g_mux);
  char reply[64];
  snprintf(reply, sizeof(reply), "ERR %s", reason);
  freeink::BlePositionServer::getInstance().sendTransferStatus(reply);
  LOG_ERR(kLogTag, "transfer failed: %s", reason);
}

void MapTransferReceiver::refuse(const char* reason) {
  // Same lock resetCounters() zeroes this under -- see the comment on the
  // completion path above.
  portENTER_CRITICAL(&g_mux);
  ++failed_;
  portEXIT_CRITICAL(&g_mux);
  char reply[64];
  snprintf(reply, sizeof(reply), "ERR %s", reason);
  freeink::BlePositionServer::getInstance().sendTransferStatus(reply);
  LOG_ERR(kLogTag, "refused: %s", reason);
}

bool MapTransferReceiver::relPathIsSafe(const char* path, size_t len) {
  if (len == 0 || len > kMaxRelPathBytes) return false;
  // Relative to /explorink, always. An absolute path would land wherever the
  // sender fancied.
  if (path[0] == '/' || path[len - 1] == '/') return false;

  size_t componentLen = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = path[i];
    // Printable ASCII only, and no backslash: SdFat takes '\\' as an ordinary
    // filename character, so allowing it just makes paths that look like
    // directories but are not.
    if (c < 0x20 || c > 0x7e || c == '\\') return false;
    if (c != '/') {
      ++componentLen;
      continue;
    }
    // An empty component is "//" -- harmless but a sign the sender built the
    // path wrong, and cheaper to reject than to normalise.
    if (componentLen == 0) return false;
    componentLen = 0;
  }

  // `..` anywhere is the one that matters: it is how a relative path escapes
  // the root. Checked as a whole component so a file legitimately called
  // `a..b.tib` is still allowed.
  //
  // **`.` and FAT's trailing-character trimming are rejected for a different
  // reason**: not escape, but *aliasing*. One file must have one spelling here,
  // because a second spelling is a second answer from every gate that reads the
  // path -- `wallet::pathIsWalletAsset()` decides whether a document gets
  // sealed, and FAT resolves `wallet/./ab/x.dat` and `wallet/ab/x.dat.` to the
  // same directory entry the gate just said was not a wallet asset. Added
  // 2026-11-27 with the case-folding fix next to it; the escape check alone let
  // all of these through.
  const char* cursor = path;
  while (cursor != nullptr) {
    const char* slash = strchr(cursor, '/');
    const size_t partLen = slash != nullptr ? static_cast<size_t>(slash - cursor) : strlen(cursor);
    if (partLen == 2 && cursor[0] == '.' && cursor[1] == '.') return false;
    if (partLen == 1 && cursor[0] == '.') return false;
    // FAT trims trailing dots and spaces from a name, so `x.dat.` and `x.dat `
    // open `x.dat`. An honest sender never writes either.
    if (partLen > 0 && (cursor[partLen - 1] == '.' || cursor[partLen - 1] == ' ')) return false;
    cursor = slash != nullptr ? slash + 1 : nullptr;
  }

  return true;
}

bool MapTransferReceiver::computePartCrc32(uint32_t& outCrc) const {
  // Read back off the card rather than accumulate over the arriving chunks:
  // this checks what MapTileReader will open, which is the thing that has to
  // be right. It also catches a card that accepted a write and lost it.
  HalFile file;
  if (!Storage.openFileForRead("MAPXFER", partPath_, file)) return false;

  // One-shot 4096 B heap buffer for this read loop only -- see the arithmetic
  // above kCrcReadBufferBytes. makeUniqueNoThrow, not bare new (firmware
  // CLAUDE.md, heap-discipline skill): this runs on the NimBLE host task,
  // inside onFrame's inFrame_ window that detach() spins on, and an OOM here
  // must degrade, not abort() the firmware. `heapBuffer` frees itself at this
  // function's return, well before the caller sends the OK/ERR status line.
  auto heapBuffer = makeUniqueNoThrow<uint8_t[]>(kCrcReadBufferBytes);
  uint8_t fallbackBuffer[kCrcFallbackReadBufferBytes];
  uint8_t* buffer = heapBuffer.get();
  size_t bufferBytes = kCrcReadBufferBytes;
  if (buffer == nullptr) {
    // Falls back to the old 128 B stack path rather than failing the whole
    // readback -- correct but slow is better than refusing a transfer for
    // one failed allocation. kept compiled per ble-fix-plan.md T5.4.
    LOG_ERR(kLogTag, "OOM: %u B crc readback buffer, falling back to %u B stack buffer",
            static_cast<unsigned>(kCrcReadBufferBytes), static_cast<unsigned>(kCrcFallbackReadBufferBytes));
    buffer = fallbackBuffer;
    bufferBytes = kCrcFallbackReadBufferBytes;
  }

  uint32_t crc = 0;
  size_t total = 0;
  while (file.available()) {
    const int read = file.read(buffer, bufferBytes);
    if (read <= 0) break;
    crc = esp_rom_crc32_le(crc, buffer, static_cast<uint32_t>(read));
    total += static_cast<size_t>(read);
  }
  if (total != received_) {
    LOG_ERR(kLogTag, "read back %lu bytes of %lu written", static_cast<unsigned long>(total),
            static_cast<unsigned long>(received_));
    return false;
  }
  outCrc = crc;
  return true;
}

void MapTransferReceiver::publish() {
  Status next;
  next.active = active_;
  next.received = received_;
  next.total = declaredTotal_;
  next.completed = completed_;
  next.completedBytes = completedBytes_;
  next.failed = failed_;
  next.lastTileValid = lastTileValid_;
  next.lastTile = lastTile_;
  next.tileSeq = tileSeq_;
  next.activeTileValid = activeTileValid_;
  next.activeTile = activeTile_;
  next.verifying = verifying_;

  portENTER_CRITICAL(&g_mux);
  snapshot_ = next;
  portEXIT_CRITICAL(&g_mux);
}

MapTransferReceiver::Status MapTransferReceiver::status() const {
  portENTER_CRITICAL(&g_mux);
  const Status copy = snapshot_;
  portEXIT_CRITICAL(&g_mux);
  return copy;
}

void MapTransferReceiver::formatStatus(char* out, size_t outSize) const {
  if (out == nullptr || outSize == 0) return;
  out[0] = '\0';

  const Status snapshot = status();
  if (snapshot.active) {
    // kB and a percent, not two raw byte counts. "xfer 18432/27648" is arithmetic
    // homework at a glance on a handlebar; the sync screen has always stated the
    // same transfer in kB, and the two readouts have to agree
    // (MapByteFormat.h).
    char got[16];
    char want[16];
    mapfmt::formatBytes(snapshot.received, got, sizeof(got));
    mapfmt::formatBytes(snapshot.total, want, sizeof(want));
    const uint32_t pct = snapshot.total > 0 ? snapshot.received * 100 / snapshot.total : 0;
    snprintf(out, outSize, "xfer %s / %s %lu%%", got, want, static_cast<unsigned long>(pct));
    return;
  }
  // Nothing in flight and nothing has happened: the readout stays two lines,
  // as it was before this channel existed.
  if (snapshot.completed == 0 && snapshot.failed == 0) return;
  snprintf(out, outSize, "xfer %lu ok %lu err", static_cast<unsigned long>(snapshot.completed),
           static_cast<unsigned long>(snapshot.failed));
}
