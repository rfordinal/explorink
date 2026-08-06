#pragma once

#include <cstddef>
#include <cstdint>

#include "HalStorage.h"
#include "MapTilePath.h"

// Receives one map data file over the BLE transfer channel and writes it to
// the SD card -- docs/ble-map-transfer-brief.md in the parent xteink repo.
//
// This is the route-corridor and emergency-resync path, not the base map
// preload. A whole region is tens of MB and still goes on the card over
// WiFi/SD; this carries the small, occasional pushes over the link the phone
// already holds open for position updates.
//
// ## Wire format
//
// One BLE write to the transfer characteristic is one frame. First byte is
// the opcode, everything after it is little endian:
//
//   0x01 begin   u32 totalLen | u32 crc32 | u8 pathLen | pathLen path bytes
//   0x02 chunk   u32 offset   | payload bytes
//   0x03 abort   (no body)
//
// Status comes back on the status characteristic, one indication per line:
//
//   RDY <totalLen>          begin accepted, send chunks
//   OK <bytes> <crc32hex>   complete, crc verified, renamed into place
//   ERR <reason>            refused or failed; nothing is left on the card
//
// The chunk frame carries a byte **offset**, not a sequence number. Same
// guarantee -- a frame that is not the next one is rejected -- but the device
// checks it against how many bytes it has actually written, so the two sides
// never have to agree on a chunk size, and a chunk that arrives twice or out
// of order is caught by arithmetic rather than by bookkeeping.
//
// Nothing acks a chunk. The transfer characteristic is a plain WRITE, so the
// ATT write response is the ack, and the device sends it only after the bytes
// are on the card -- the sender physically cannot outrun the SD card
// (BlePositionServer.h, TransferHooks).
//
// ## Write to .part, rename at the end
//
// Bytes land in `<path>.part` and the file is renamed only after the whole-
// file CRC32 matches what the begin frame declared. MapTileReader trusts a
// .tib the moment it opens it, so a half-written file sitting at the final
// path -- BLE link killed mid-chunk -- would be read as a complete tile on
// the next viewport reset. The CRC is computed by reading the finished .part
// back off the card, not accumulated from the arriving chunks: that checks
// what the card holds, which is the thing MapTileReader will open.
//
// ## Threading
//
// Every frame, the disconnect notice and all SD I/O run on the NimBLE host
// task, and that task is the only one that touches the transfer state. The
// activity task only ever reads a counter snapshot (status()) under a
// critical section, for the map screen's debug readout. Nothing is queued and
// there is no worker task: a chunk is written where it arrives, which is also
// what makes the ATT write response mean "on the card".
//
// One transfer at a time. A begin frame while one is in progress is an ERR,
// not a queued job -- unless the one in progress has gone quiet for
// kStaleTransferMs, in which case it is reclaimed. That reclaim is the only
// timeout, and it deliberately happens on the host task when the next begin
// arrives rather than from a poll on the activity task: a timeout fired from
// another task would have to close a file this task owns.
class MapTransferReceiver {
 public:
  // rootDir is MapActivity's kTileRoot. Every path the sender gives is
  // relative to it and may not escape it.
  explicit MapTransferReceiver(const char* rootDir);
  ~MapTransferReceiver();

  MapTransferReceiver(const MapTransferReceiver&) = delete;
  MapTransferReceiver& operator=(const MapTransferReceiver&) = delete;

  // Registers the BLE hooks. Call after BlePositionServer::begin().
  void attach();
  // Unregisters them and abandons any transfer in flight, deleting its .part
  // file. Call **before** BlePositionServer::end(): a hook still registered
  // when the activity is deleted is a callback that outlives its owner.
  void detach();

  // Counters for the map screen's debug readout. Activity task.
  struct Status {
    bool active = false;
    uint32_t received = 0;   // bytes of the transfer in flight
    uint32_t total = 0;      // its declared length
    uint32_t completed = 0;  // files that landed since the screen opened
    // Bytes of those files, summed. A progress screen needs a real rate to put
    // a time on the remainder, and a count of files cannot give one: the tiles
    // in one fetch ranged from 6 KB to 75 KB.
    uint32_t completedBytes = 0;
    uint32_t failed = 0;  // transfers refused or failed since then
    // The last file that landed, if its path was a tile
    // (MapTilePath.h). `completed` counts every landed file including
    // non-tiles, so it is not the same signal.
    //
    // Here rather than in MapActivity because only this class sees the path.
    // Read on the activity task, which is the only task allowed to touch
    // MissingTilesStore -- record() runs from renderViewport(), and a second
    // writer from the NimBLE host task would corrupt the vector. So the host
    // task publishes a coordinate and the activity task does the removal.
    bool lastTileValid = false;
    MapTileCoord lastTile;
    // The tile currently on the wire, when the transfer in flight is one.
    // `received`/`total` above are its byte counts, so a screen can draw a
    // progress bar for this specific tile rather than for the batch.
    //
    // Parsed from the begin frame's path, which is the only place this class
    // learns what a transfer is. A route or style push leaves this false.
    bool activeTileValid = false;
    MapTileCoord activeTile;
    // Bumped once per landed tile. The activity task keeps its own copy and
    // acts when the two differ; that is what makes the handoff a signal
    // rather than a repeated instruction.
    //
    // Two tiles landing between two loop() iterations would collapse into one
    // removal here. A whole file takes seconds and loop() runs continuously,
    // so this is a theoretical race, and its cost is one stale entry that the
    // next fetch asks for again -- not corruption.
    uint32_t tileSeq = 0;
  };
  Status status() const;

  // Renders status() into `out` as one short readout line, or an empty string
  // if nothing has been transferred and nothing is in flight.
  void formatStatus(char* out, size_t outSize) const;

  // Longest relative path accepted in a begin frame, e.g.
  // "base/13/4482/2789.tib". Nul terminator not included.
  static constexpr size_t kMaxRelPathBytes = 80;

  // How long a transfer may sit with no frames before the next begin is
  // allowed to reclaim it. Long enough that a slow card or a busy phone is
  // not mistaken for a dead sender.
  static constexpr uint32_t kStaleTransferMs = 30000;

 private:
  // BLE hook trampolines -- plain functions, so no std::function anywhere on
  // this path (firmware CLAUDE.md).
  static void frameTrampoline(void* ctx, const uint8_t* data, size_t len);
  static void disconnectTrampoline(void* ctx);

  void onFrame(const uint8_t* data, size_t len);
  void handleBegin(const uint8_t* body, size_t len);
  void handleChunk(const uint8_t* body, size_t len);

  // Closes the file and deletes the .part, then goes back to idle. `reason`
  // is nullptr for a clean end (the file was already renamed away), otherwise
  // it is sent as `ERR <reason>` and counted as a failure.
  void abandon(const char* reason);
  // Refuses a frame without touching a transfer in flight.
  void refuse(const char* reason);

  // True if `path` is a safe relative path under the root: no leading slash,
  // no `..` component, no empty component, printable ASCII only. This is what
  // stops a sender writing outside /trailink.
  static bool relPathIsSafe(const char* path, size_t len);

  // Reads the finished .part file back and returns its CRC32, or false if it
  // could not be read.
  bool computePartCrc32(uint32_t& outCrc) const;

  // Copies the host task's counters into snapshot_ under the critical
  // section. Called at the end of every frame -- the activity task reads only
  // snapshot_, so it can never see half of one frame's update.
  void publish();

  const char* rootDir_;

  // Host task only, all of it.
  HalFile file_;
  bool active_ = false;
  uint32_t declaredTotal_ = 0;
  uint32_t declaredCrc_ = 0;
  uint32_t received_ = 0;
  uint32_t lastFrameMs_ = 0;
  // "/trailink/base/13/4482/2789.tib" and the same with ".part" on the end.
  // Fixed buffers, not std::string: this is a member of the map activity and
  // the path length is bounded by the protocol.
  char finalPath_[kMaxRelPathBytes + 48] = {};
  char partPath_[kMaxRelPathBytes + 48] = {};

  uint32_t completed_ = 0;
  uint32_t completedBytes_ = 0;
  uint32_t failed_ = 0;
  // Host task's side of Status::lastTile / tileSeq above.
  bool lastTileValid_ = false;
  MapTileCoord lastTile_;
  uint32_t tileSeq_ = 0;
  // Host task's side of Status::activeTile. Set when a begin frame's path parses
  // as a tile, cleared whenever the transfer ends, either way.
  bool activeTileValid_ = false;
  MapTileCoord activeTile_;

  // The one thing both tasks touch. Written by publish(), read by status().
  Status snapshot_;

  // Checked at the top of every frame so a frame already inside the callback
  // when detach() ran cannot open a new file behind it.
  volatile bool attached_ = false;
};
