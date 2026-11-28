#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "HalStorage.h"
#include "MapTilePath.h"
#include "MapTransferGate.h"
#include "MapTransferPath.h"

#if defined(ENABLE_WALLET) && ENABLE_WALLET
#include "../wallet/WalletAssetIngest.h"
#include "../wallet/WalletManifestIngest.h"
#endif

// Only for the panel geometry a wallet asset is checked against, and only
// through a pointer -- this class does not draw.
class GfxRenderer;

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
// ## A wallet asset, and the wallet's list, are sealed on the way in
//
// A `.dat` under `/explorink/wallet` is not stored as it arrives. The phone
// sends the readable bitmap and the **device** encrypts it with the key it made
// itself (T-2094), because no laptop and no phone can produce a tree this board
// can read any more. See WalletAssetIngest.h for the one-pass shape and for why
// the `OK` reply means something weaker for these files than it does for a tile:
// the CRC is accumulated from the arriving plaintext instead of read back off
// the card, since what is on the card is ciphertext the sender has never seen.
//
// Everything below about offsets, `.part`, the rename and the threading is
// unchanged by that -- the asset is the same number of bytes sealed as it was
// in the clear, so the arithmetic never learns the difference.
//
// `wallet/manifest.json` is the exception that does change it. It is buffered
// in RAM, sealed once at the end, and installed by its own code under its own
// name (`manifest.enc`, WalletManifestInstall.h) -- so for that one path the
// `.part` file below is never opened and the rename at the end never runs.
// Why it cannot be streamed like an asset is the whole of T-2096, and it is
// written down in WalletManifestIngest.h: the read path cannot stream, so a
// streamed seal would accept a list this same device can never open again.
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
// detach() is the one exception -- it runs on the activity task and it *does*
// write transfer state, because its whole job is to tear a transfer down. So
// it waits the host task out first through gate_ (MapTransferGate.h): a frame
// publishes that it is inside before it checks that the receiver is attached,
// and detach() withdraws the receiver before it checks for a frame inside, so
// one of the two always sees the other (see detach() below for the bound and
// what happens if it is exceeded). The disconnect hook goes through the same
// gate, so it and detach() cannot both run abandon().
// Without that wait, a frame that already passed the gate and is
// blocked in file_.write() resumes into state abandon() has already reset,
// and runs a completion pass on a file that is closed and deleted.
//
// The storage mutex (HalStorage.cpp:34) does not solve this on its own: it
// serializes each individual file operation, not the sequence of them, so it
// keeps the card consistent while the *counters* and the open handle still get
// mutated by two tasks.
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
  //
  // `renderer` is the live panel, and it is what a wallet asset's geometry is
  // checked against before the device seals it (handleBegin). A nullptr means
  // "this screen cannot take wallet assets" -- the push is refused rather than
  // written in the clear, because a plaintext document on the card is the one
  // outcome this whole rung exists to prevent.
  explicit MapTransferReceiver(const char* rootDir, const GfxRenderer* renderer = nullptr);
  ~MapTransferReceiver();

  MapTransferReceiver(const MapTransferReceiver&) = delete;
  MapTransferReceiver& operator=(const MapTransferReceiver&) = delete;

  // Registers the BLE hooks. Call after BlePositionServer::begin().
  void attach();

  // Zeroes the landed/failed counters so a screen can start a second run without
  // leaving and coming back. Refuses while a transfer is in flight -- the numbers
  // it would reset are the ones that transfer is about to add to.
  //
  // Needed because these counters are "since the screen opened" and the sync
  // screen can now re-ask on the same visit: a phone that turns up after a run
  // finished used to be ignored (TileSyncActivity::trackPhone).
  bool resetCounters();
  // Unregisters them and abandons any transfer in flight, deleting its .part
  // file. Call **before** BlePositionServer::end(): a hook still registered
  // when the activity is deleted is a callback that outlives its owner.
  //
  // Blocks for up to kDetachWaitMs waiting for a frame already inside the
  // callback to leave it, because that frame and this call would otherwise
  // mutate the same state from two tasks. A normal Back press mid-transfer
  // hits this, so it is the common case and not an error path.
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
    // True from the moment the last chunk is written until the file is renamed
    // into place: the close, the CRC read-back and the rename. `received`
    // equals `total` for the whole of it and not one byte moves, so without
    // this a screen showing byte progress simply stops, and a stop is what a
    // dead link looks like. On a megabyte tile the read-back is seconds of it
    // (see "Write to .part, rename at the end" -- the CRC is computed by
    // reading the card back, not accumulated from the chunks).
    //
    // Published on its own rather than derived from `received == total`: the
    // publish that would carry that equality is the one at the end of the
    // completing frame, which does not happen until the whole pass is over. So
    // the activity task's last view during the pause is the *previous* chunk's,
    // one payload short of the total, and there is nothing in it to read.
    bool verifying = false;
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
  static constexpr size_t kMaxRelPathBytes = maptransfer::kMaxRelPathBytes;

  // How long a transfer may sit with no frames before the next begin is
  // allowed to reclaim it. Long enough that a slow card or a busy phone is
  // not mistaken for a dead sender.
  static constexpr uint32_t kStaleTransferMs = 30000;

  // How long detach() waits for the host task to leave the frame callback.
  // Covers a single chunk's SD write with room for a card doing an internal
  // erase cycle; it does **not** cover the completion pass, which reads the
  // whole .part back to check the CRC and takes seconds on a 75 KB tile. The
  // bound is a bound, not a guarantee -- exceeding it is handled rather than
  // waited out, because a wedged card must not wedge the UI. handleChunk's
  // active_ re-checks are what make the exceeded case safe.
  static constexpr uint32_t kDetachWaitMs = 500;

  // True if `path` is a safe relative path under the root: no leading slash,
  // no `..` component, no empty component, printable ASCII only. This is what
  // stops a sender writing outside /explorink.
  //
  // **Public because the rule has two callers, not because this class is a
  // utility.** CMD:WALLETBENCH builds a card path from a host-supplied relative
  // path too, and a second copy of this rule is a second place for it to drift
  // (firmware CLAUDE.md, "Docs" -- content lives in exactly one place).
  static bool relPathIsSafe(const char* path, size_t len);

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

  // Reads the finished .part file back and returns its CRC32, or false if it
  // could not be read.
  bool computePartCrc32(uint32_t& outCrc) const;

  // Copies the host task's counters into snapshot_ under the critical
  // section. Called at the end of every frame -- the activity task reads only
  // snapshot_, so it can never see half of one frame's update.
  void publish();

  const char* rootDir_;
  const GfxRenderer* renderer_;

  // Host task only, all of it -- except active_, below.
  HalFile file_;
  // True while a transfer is in flight. Written by the host task and by
  // detach()'s abandon() on the activity task, and read by both, which is why
  // it alone of this block is atomic. Every blocking call in handleChunk is
  // followed by a re-check of it: detach() can time out its wait and abandon
  // the transfer underneath a frame (kDetachWaitMs), and the re-check is how the
  // frame notices. A plain bool made that a bet that the compiler reloads the
  // member after a call into another translation unit. seq_cst by default; it is
  // a byte, and on neither chip is the fence measurable next to an SD write.
  std::atomic<bool> active_{false};
  uint32_t declaredTotal_ = 0;
  uint32_t declaredCrc_ = 0;
  uint32_t received_ = 0;
  uint32_t lastFrameMs_ = 0;
  // "/explorink/base/13/4482/2789.tib" and the same with ".part" on the end.
  // Fixed buffers, not std::string: this is a member of the map activity and
  // the path length is bounded by the protocol.
  char finalPath_[kMaxRelPathBytes + 48] = {};
  char partPath_[kMaxRelPathBytes + 48] = {};

  // The one exception to "host task only" above: resetCounters() zeroes
  // these from the activity task (only while !active_). Plain uint32_t, not
  // volatile -- same standard as BlePositionServer.h's pendingStatusHead_/
  // Tail_ comment: portENTER_CRITICAL(&g_mux) is the synchronisation, not a
  // qualifier, and that only holds if *every* write goes through it. The
  // host-task increments (MapTransferReceiver.cpp's completion path in
  // handleChunk, abandon(), refuse()) take the same lock resetCounters()
  // does, so the two cannot interleave.
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
  // Host task's side of Status::verifying. Cleared wherever a transfer ends,
  // both ways.
  bool verifying_ = false;

#if defined(ENABLE_WALLET) && ENABLE_WALLET
  // The sealing state machine for the transfer in flight, when that transfer is
  // a wallet asset. A pointer, allocated in handleBegin and dropped wherever
  // the transfer ends, because it carries a 512 B scratch and a SHA-256 state
  // that a tile push has no use for -- and this object lives for as long as the
  // map screen is open (MapActivity, TileSyncActivity both hold one).
  std::unique_ptr<wallet::AssetIngest> walletIngest_;
  // The same, for the transfer that is the wallet's list of documents. A
  // separate machine rather than a mode on the one above, because the two are
  // opposites: an asset is encrypted chunk by chunk and never held, a manifest
  // is held whole and sealed in one shot -- and the reason for that is the read
  // path's 32 KB cap (WalletManifestIngest.h). It owns the plaintext buffer,
  // which is why dropping this pointer is what frees it.
  std::unique_ptr<wallet::ManifestIngest> manifestIngest_;
  // Takes the manifest's plaintext buffer out of the sealer under g_mux, so
  // exactly one of the two tasks can ever own it. The host task takes it before
  // it writes a byte of it -- a raw pointer handed to a blocking write is a
  // pointer the other task can free underneath -- and the activity task takes
  // it from abandon() to wipe it. The wipe is up to 32 KB and happens outside
  // the lock, on memory nobody else can reach by then.
  std::unique_ptr<uint8_t[]> takeManifestBuffer(size_t& length);
  // takeManifestBuffer() plus the wipe, for the caller that only wants it gone.
  void dropManifestBuffer();
  // Drops both sealers. The only place either is freed, host task only, with
  // the pointer writes under the same lock the activity task reads them
  // through.
  void clearSealers();
#endif

  // The one thing both tasks touch. Written by publish(), read by status().
  Status snapshot_;

  // Whether this receiver accepts work, and whether the host task is inside a
  // callback right now. The handshake and why its orderings are seq_cst are in
  // MapTransferGate.h, host-tested in test/map_transfer.
  maptransfer::FrameGate gate_;
};
