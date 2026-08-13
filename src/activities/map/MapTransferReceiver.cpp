#include "MapTransferReceiver.h"

#include <Arduino.h>
#include <BlePositionServer.h>
#include <Logging.h>
#include <esp_rom_crc.h>

#include <cstdio>
#include <cstring>

#include "MapByteFormat.h"

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

// Read-back buffer for the completion CRC check. Same size FontDownloadActivity
// uses for the same job -- 128 bytes of stack, well under CLAUDE.md's 256-byte
// cap on locals.
constexpr size_t kCrcReadBufferBytes = 128;

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

}  // namespace

MapTransferReceiver::MapTransferReceiver(const char* rootDir) : rootDir_(rootDir) {}

MapTransferReceiver::~MapTransferReceiver() {
  // No status to send and nobody to send it to by now -- just don't leave a
  // .part behind. detach() is the ordinary path; this is the backstop.
  if (active_) abandon(nullptr);
}

bool MapTransferReceiver::resetCounters() {
  if (active_) return false;
  // Same critical section the publish path uses: the NimBLE host task writes the
  // snapshot these mirror, and a torn read on the screen's next frame would show
  // a count that never existed.
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

  // `base/13/4482/2789.tib` needs /trailink/base/13/4482 to exist first.
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

  // A leftover .part from an earlier killed transfer is not resumed -- the
  // whole file is coming again, from offset 0 (O_TRUNC does the rest).
  if (!Storage.openFileForWrite("MAPXFER", partPath_, file_)) {
    refuse("open failed");
    return;
  }

  active_ = true;
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

  // Straight from the NimBLE attribute buffer to the card: no copy, no queue,
  // no buffer of our own. The ATT write response the stack sends when this
  // callback returns is therefore "the bytes are on the card", which is what
  // makes it usable as flow control (MapTransferReceiver.h, Threading).
  if (file_.write(payload, payloadLen) != payloadLen) {
    if (!active_) return;
    abandon("write failed");
    return;
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
  // Closed before the read-back: the same HalFile cannot be open for write
  // and reopened for read (firmware CLAUDE.md, DESTRUCTOR_CLOSES_FILE), and
  // the rename below needs the handle gone too.
  file_.close();
  if (!active_) return;

  uint32_t actualCrc = 0;
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

  // SdFat's rename fails onto an existing name, and re-pushing a tile that is
  // already there is the normal case for a corridor update.
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
  // The rename succeeded, so a complete, CRC-checked file *is* on the card --
  // but if detach() ran, the counters below belong to a screen that is gone and
  // sendTransferStatus would indicate into a server about to be end()ed. Say
  // nothing; the phone times this tile out and asks again, which costs one
  // re-push and risks nothing.
  if (!active_) return;

  const uint32_t bytes = received_;
  active_ = false;
  received_ = 0;
  declaredTotal_ = 0;
  activeTileValid_ = false;
  ++completed_;
  completedBytes_ += bytes;

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

  active_ = false;
  received_ = 0;
  declaredTotal_ = 0;
  activeTileValid_ = false;

  if (reason == nullptr) return;

  ++failed_;
  char reply[64];
  snprintf(reply, sizeof(reply), "ERR %s", reason);
  freeink::BlePositionServer::getInstance().sendTransferStatus(reply);
  LOG_ERR(kLogTag, "transfer failed: %s", reason);
}

void MapTransferReceiver::refuse(const char* reason) {
  ++failed_;
  char reply[64];
  snprintf(reply, sizeof(reply), "ERR %s", reason);
  freeink::BlePositionServer::getInstance().sendTransferStatus(reply);
  LOG_ERR(kLogTag, "refused: %s", reason);
}

bool MapTransferReceiver::relPathIsSafe(const char* path, size_t len) {
  if (len == 0 || len > kMaxRelPathBytes) return false;
  // Relative to /trailink, always. An absolute path would land wherever the
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
  const char* cursor = path;
  while (cursor != nullptr) {
    const char* slash = strchr(cursor, '/');
    const size_t partLen = slash != nullptr ? static_cast<size_t>(slash - cursor) : strlen(cursor);
    if (partLen == 2 && cursor[0] == '.' && cursor[1] == '.') return false;
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

  uint8_t buffer[kCrcReadBufferBytes];
  uint32_t crc = 0;
  size_t total = 0;
  while (file.available()) {
    const int read = file.read(buffer, sizeof(buffer));
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
