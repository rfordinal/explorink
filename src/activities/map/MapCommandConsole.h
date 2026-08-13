#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "HeldTilesStore.h"
#include "MapCommandParser.h"
#include "StaleTilesList.h"

// The channel-free half of the map command console: line assembly, the
// state the grammar drives, and the replies. Pure -- no Arduino, no serial,
// no activity. MapSerialConsole is the USB front end over this; P5's BLE
// characteristic is the second one, over the same object.

// Where replies go. One line per call, no marker prefix and no newline --
// the channel adds both, because the framing differs per channel (serial
// prefixes a marker, BLE will not need one).
class IMapReplyWriter {
 public:
  virtual ~IMapReplyWriter() = default;
  virtual void reply(const char* line) = 0;
};

// Bytes in, whole lines out. Handles the three things a real UART does that
// a desk test does not: a line split across several poll() calls, CR/LF or
// bare LF or bare CR, and a line longer than the buffer.
//
// An over-long line is discarded to its own end of line and reported once,
// as Overflow, at that terminator. It never overflows the buffer and it
// never leaves half a line to be parsed as if it were whole.
class MapLineAssembler {
 public:
  enum class Result : uint8_t {
    Pending,   // byte consumed, line not finished
    Line,      // line() is now valid (possibly empty)
    Overflow,  // the line that just ended was too long and was discarded
  };

  Result feed(char c);

  // Valid until the next feed(), after feed() returned Line.
  std::string_view line() const { return std::string_view(buf_, lineLen_); }

  void reset();

  static constexpr size_t kMaxLine = 95;

 private:
  char buf_[kMaxLine + 1] = {};
  size_t len_ = 0;
  size_t lineLen_ = 0;
  bool discarding_ = false;
};

// One tile's outcome in the most recent viewport reset -- what the `tiles`
// command prints one line per index for. Pushed, never polled: MapActivity
// is the only thing that knows the tile range, so it calls setTileRange()
// right after every reset (MapActivity.cpp, renderViewport()).
//
// Convention matches MapViewport::TileRange and MapTileSource::
// unavailableMask(): column-major index over col0..col1 then row0..row1,
// one bit per index, set means absent/truncated/crc-mismatched.
struct MapTileRangeSnapshot {
  bool valid = false;  // false until the first reset
  uint8_t z = 0;
  uint32_t col0 = 0;
  uint32_t row0 = 0;
  uint32_t col1 = 0;
  uint32_t row1 = 0;
  uint32_t unavailableMask = 0;
};

// Told about each `stale` line as it lands, so the screen that asked can put the
// tile on its fetch list. Synchronous, same contract as IMapSkipObserver: the
// observer runs on the activity task draining the console and cannot miss one.
class IMapStaleObserver {
 public:
  virtual ~IMapStaleObserver() = default;
  virtual void onTileStale(uint8_t z, uint32_t col, uint32_t row) = 0;
  // The verdict. [known] false is `checked unknown` -- the phone could not read
  // the index and is claiming nothing. **Not the same as zero stale tiles**, and
  // an implementation that treats it as such buries the bug this feature exists
  // to find.
  virtual void onCheckFinished(bool known, uint16_t staleCount) = 0;
};

// One entry of the persisted missing-tile list, as the `missing` command
// prints it. A field-for-field copy of MissingTileHit on purpose:
// MapConsoleState must not include MissingTilesStore.h, which pulls in
// ArduinoJson and the SD-backed PersistableStore -- neither of which the
// native console tests link, and neither of which this half is allowed to
// depend on.
struct MapMissingTile {
  uint8_t z = 0;
  uint32_t col = 0;
  uint32_t row = 0;
  uint32_t count = 0;
};

// Read-only window onto that list. MapActivity implements it over
// MissingTilesStore; the native tests implement it over a fixed array.
//
// Pull, not push -- the opposite of setTileRange() above, and for a reason:
// the viewport snapshot is 7 words, while this list is up to 200 entries
// (MissingTilesStore::kMaxEntries). Pushing it would put a second copy of
// the same table in DRAM and would go stale the moment another tile hatched.
class IMissingTilesSource {
 public:
  virtual ~IMissingTilesSource() = default;
  // A listing is starting: put the list in fetch-priority order. Called once
  // per listing, on the page-0 request only -- later pages must not reshuffle
  // under a reader that is halfway through paging, and re-sorting per page
  // would cost the sort again for nothing.
  //
  // The policy lives with the data (MissingTilesStore::sortByFetchPriority,
  // src/MissingTilePriority.h); this half only knows when to ask for it.
  virtual void orderForFetch() = 0;
  virtual size_t missingTileCount() const = 0;
  // Only ever called with index < missingTileCount().
  virtual MapMissingTile missingTileAt(size_t index) const = 0;
};

// What the supplier of tiles has given up on, as counted by the `skip`
// command. The fetch progress screen reads this so it can show "N failed"
// instead of sitting on a tile that will never arrive.
//
// A count plus the last one, not a list: the screen shows a number, and
// keeping 200 skipped coordinates would be a second copy of the store's list
// for no reader. The last one is here for the log line MapActivity writes.
struct MapSkipTally {
  uint32_t count = 0;
  uint8_t z = 0;
  uint32_t col = 0;
  uint32_t row = 0;
  char reason[MapCommand::kSkipReasonBytes] = {};
};

// What the `stats` command answers: everything that costs power, as counted by
// PowerTelemetry plus the battery and the link.
//
// A plain struct filled by a provider, not a set of pushed values, for the same
// reason the MTU is a provider: every field changes on its own schedule and a
// pushed copy would report whatever was true at the last viewport reset. Kept
// free of PowerTelemetry.h so this half stays host-testable with no Arduino
// behind it -- MapActivity does the copying.
//
// Field names match power.csv's columns one for one (src/PowerLog.cpp). A
// script that reads a ride off the card and a script that polls a live device
// over BLE should not need two vocabularies for one measurement.
struct MapPowerStats {
  uint16_t batteryMv = 0;
  uint16_t batteryPct = 0;
  uint32_t uptimeS = 0;

  uint16_t cpuMhz = 0;
  uint32_t fullClockMs = 0;
  uint32_t throttledMs = 0;

  uint32_t loopIters = 0;
  uint32_t loopBusyMs = 0;
  uint32_t loopMaxMs = 0;

  uint32_t refreshFull = 0;
  uint32_t refreshHalf = 0;
  uint32_t refreshFast = 0;
  uint32_t refreshWindow = 0;
  uint32_t panelBusyMs = 0;

  uint32_t freeHeap = 0;
  uint32_t minFreeHeap = 0;

  // 0 dBm is not a real BLE reading, so it doubles as "nothing connected"
  // (BlePositionServer::rssi()).
  int8_t rssiDbm = 0;
};

// Told about each `skip` as it lands, so a screen showing one row per tile can
// mark the right row rather than a count.
//
// A synchronous callback, not a queue: `execute()` runs on the activity task
// while it drains its console, so the observer is on the same task and cannot
// miss an event -- which a "last skip plus a counter" snapshot can, if two skips
// arrive between two polls.
class IMapSkipObserver {
 public:
  virtual ~IMapSkipObserver() = default;
  virtual void onTileSkipped(uint8_t z, uint32_t col, uint32_t row) = 0;
};

// What the commands actually do, and the only thing that knows it. Holds no
// channel and no hardware, so P3's serial console and P5's BLE
// characteristic run the same lines through the same object and get the
// same behaviour by construction.
//
// **One instance per map screen, shared by every channel.** MapActivity owns
// it and hands a reference to each transport. Two states would mean the map
// had two zooms, and which one you got would depend on which cable was
// plugged in.
//
// Implements the whole grammar: pos, heading, zoom, marker, mode, redraw,
// tiles, missing, info. zoom/marker/mode only move the numbers here -- applying them
// to the projection, the class mask and the settings file is MapActivity's
// job, which is what keeps this half free of hardware.
class MapConsoleState {
 public:
  // Runs one command, writes exactly the reply lines it produces, and
  // returns true if the screen needs redrawing.
  bool execute(const MapCommand& cmd, IMapReplyWriter& out);

  bool hasPosition() const { return hasPosition_; }
  int32_t latE7() const { return latE7_; }
  int32_t lonE7() const { return lonE7_; }
  uint8_t heading() const { return heading_; }  // 0-15, see MapHeading.h
  uint16_t speedKmh() const { return speedKmh_; }
  // Metres above sea level, valid only if hasAltitude() -- not yet consumed
  // by anything drawn, wired ahead of hike mode.
  bool hasAltitude() const { return hasAltitude_; }
  int16_t altitudeM() const { return altitudeM_; }

  // Ladder steps and travel mode, as last set by a command or pushed back by
  // MapActivity. MapActivity reads these after a command lands and applies
  // them; nothing here knows what a step means in pixels or metres.
  uint8_t zoomStep() const { return zoomStep_; }
  uint8_t markerStep() const { return markerStep_; }
  MapRideMode mode() const { return mode_; }
  // Bumped by every command that changes what is on screen. A caller that
  // only redraws on change can compare it; poll() returning true is the
  // simpler way.
  uint32_t seq() const { return seq_; }

  // Optional platform hook for `info`. Left unset (the default) the heap
  // line is simply omitted, which is what the native tests want.
  void setFreeHeapProvider(uint32_t (*provider)()) { freeHeapProvider_ = provider; }

  // Same shape, for the link's negotiated ATT MTU. A provider rather than a
  // pushed value because it changes when a central connects, and a number
  // pushed once would report the last link's MTU forever. Returns 0 when
  // nothing is connected, and `info` then omits the line rather than claiming
  // an MTU of zero.
  void setLinkMtuProvider(uint16_t (*provider)()) { linkMtuProvider_ = provider; }

  // Where `stats` gets its numbers. Returns false when the platform cannot
  // answer, and `stats` then replies `INFO stats=unavailable` rather than a
  // page of zeroes -- a zeroed battery reads as a flat one, which is a lie a
  // measurement log must not contain. Left unset (the default) is the same
  // case, which is what the native tests want.
  void setPowerStatsProvider(bool (*provider)(MapPowerStats&)) { powerStatsProvider_ = provider; }

  // Same shape again, for the connection interval in milliseconds. Reported
  // next to the MTU because it, not the MTU, is what caps a transfer: a chunk
  // is write-with-response, so it costs one interval each way.
  void setLinkIntervalProvider(uint16_t (*provider)()) { linkIntervalProvider_ = provider; }

  // Pushed by MapActivity after every viewport reset: what the zoom step it
  // actually rendered at resolves to. `info`'s zoom/lod/mpp lines read this.
  void setZoomInfo(uint8_t zoomStep, uint8_t lod, double mpp) {
    zoomStep_ = zoomStep;
    lod_ = lod;
    mpp_ = mpp;
  }

  // Pushed by MapActivity whenever it changes a ladder itself -- a button
  // press, or a `mode` command restoring that mode's stored steps. Without
  // this the console would report the last value *typed* rather than the one
  // on screen, and the two diverge the moment a button is pressed.
  void setLadders(uint8_t zoomStep, uint8_t markerStep, MapRideMode mode) {
    zoomStep_ = zoomStep;
    markerStep_ = markerStep;
    mode_ = mode;
  }

  // Pushed by MapActivity after every viewport reset -- pos, heading and
  // redraw all end in one. `info`'s tiles_ok/tiles_missing/ways/bytes lines
  // read this. Zero before the first reset, which reads correctly: nothing
  // has been drawn yet.
  void setRenderStats(uint32_t tilesOk, uint32_t tilesMissing, uint32_t ways, uint32_t bytesRead,
                      uint32_t waysFiltered = 0) {
    tilesOk_ = tilesOk;
    tilesMissing_ = tilesMissing;
    ways_ = ways;
    bytesRead_ = bytesRead;
    waysFiltered_ = waysFiltered;
  }

  // Pushed alongside setRenderStats() by the same reset. `tiles` reads this.
  void setTileRange(const MapTileRangeSnapshot& range) { tileRange_ = range; }

  // Where `have` reads its list from, and what `checked` settles. Not owned;
  // must outlive this state. Left unset (the default) `have` answers
  // `INFO have=none`, which is what a native test with no map behind it should
  // see.
  //
  // Pull, not push -- the opposite of setTileRange() above, and for the same
  // reason as the missing list: the viewport snapshot is 7 words, while this
  // one is up to HeldTilesStore::kMaxEntries and accumulates across renders.
  // Pushing it would copy a kilobyte per viewport reset and the copy would be
  // the thing `checked` had to settle, which is the wrong object.
  //
  // Non-const because a listing stamps what it listed (beginListing()).
  void setHeldTilesStore(HeldTilesStore* store) { heldTiles_ = store; }

  // Which tiles the phone has already reported stale, so `tiles` can flag them.
  // Not owned; must outlive this state. Left unset, no tile is ever flagged
  // stale and the reply is exactly what it was before this existed.
  void setStaleTiles(const StaleTilesList* stale) { staleTiles_ = stale; }

  // Where a `stale` line and the closing `checked` go. Not owned; must outlive
  // this state. Left unset, both are parsed, answered and dropped -- which is
  // what a native test with no fetch behind it wants.
  void setStaleObserver(IMapStaleObserver* observer) { staleObserver_ = observer; }

  // The .tib format version this build reads (MapTileReader::kFormatVersion),
  // pushed once in onEnter(). Reported by `info` so a tile supplier can ask
  // what to build without starting a fetch to find out; the fetch itself quotes
  // the same number in `NEED_TILES`. Pushed rather than included, for the same
  // reason as the missing list: this half stays free of the map's headers.
  void setTileFormatVersion(uint16_t version) { tileFormatVersion_ = version; }

  // Where `missing` reads its list from. Not owned; must outlive this state.
  // Left unset (the default) `missing` answers `INFO missing=unavailable`,
  // which is what a native test with no SD card behind it should see.
  //
  // Non-const because a listing reorders the source (orderForFetch above).
  void setMissingTilesSource(IMissingTilesSource* source) { missingTiles_ = source; }

  // Tiles the phone has given up on. Reset by the screen that starts a fetch,
  // so the count it shows belongs to this fetch and not to the last one.
  const MapSkipTally& skips() const { return skips_; }
  void clearSkips() { skips_ = MapSkipTally{}; }

  // Per-skip callback for a screen that shows one row per tile. Not owned; must
  // outlive this state. Left unset, only the tally above is kept.
  void setSkipObserver(IMapSkipObserver* observer) { skipObserver_ = observer; }

  // Entries one `missing` command will print. Bounded because every reply
  // line is one BLE indication and each one waits for the peer's ATT confirm
  // before the next goes out (BlePositionServer::sendCommandReply) -- 200
  // lines in one command would hold the activity's loop() for minutes.
  // 20 keeps a page in the same order as `info`'s 18 lines, which is already
  // proven on hardware.
  static constexpr uint16_t kMissingPageSize = 20;

 private:
  void writeInfo(IMapReplyWriter& out) const;
  void writeStats(IMapReplyWriter& out) const;
  void writeTiles(IMapReplyWriter& out) const;
  // Non-const: listing is also what puts the entries on the wire, and the
  // store has to know which ones so a later `checked` settles those and not a
  // tile recorded since (HeldTilesStore::beginListing).
  void writeHave(IMapReplyWriter& out);
  void writeMissing(uint16_t offset, IMapReplyWriter& out) const;

  bool hasPosition_ = false;
  int32_t latE7_ = 0;
  int32_t lonE7_ = 0;
  uint8_t heading_ = 0;
  uint16_t speedKmh_ = 0;
  bool hasAltitude_ = false;
  int16_t altitudeM_ = 0;
  uint32_t seq_ = 0;
  uint32_t (*freeHeapProvider_)() = nullptr;
  uint16_t (*linkMtuProvider_)() = nullptr;
  uint16_t (*linkIntervalProvider_)() = nullptr;
  bool (*powerStatsProvider_)(MapPowerStats&) = nullptr;

  uint8_t zoomStep_ = 0;
  uint8_t markerStep_ = 0;
  MapRideMode mode_ = MapRideMode::Ride;
  uint8_t lod_ = 0;
  double mpp_ = 0.0;
  uint32_t tilesOk_ = 0;
  uint32_t tilesMissing_ = 0;
  uint32_t ways_ = 0;
  uint32_t waysFiltered_ = 0;
  uint32_t bytesRead_ = 0;
  MapTileRangeSnapshot tileRange_;
  HeldTilesStore* heldTiles_ = nullptr;
  const StaleTilesList* staleTiles_ = nullptr;
  IMapStaleObserver* staleObserver_ = nullptr;
  IMissingTilesSource* missingTiles_ = nullptr;
  MapSkipTally skips_;
  IMapSkipObserver* skipObserver_ = nullptr;
  // 0 until MapActivity pushes the real one -- `info` then omits the line
  // rather than claiming version 0, which is not a version that ever existed.
  uint16_t tileFormatVersion_ = 0;
};

// Line assembler + parser + a reference to the shared state. One of these
// per channel.
//
// **The assembler is per channel and the state is not.** A UART's half-typed
// line and a BLE write's half-typed line are two different half-typed lines
// and must not interleave into one buffer; the map's zoom, on the other hand,
// is one number whichever channel set it. That split is the whole reason this
// object and MapConsoleState are separate types.
class MapCommandConsole {
 public:
  explicit MapCommandConsole(MapConsoleState& state) : state_(state) {}

  // One received byte. Returns true if a line completed and the screen
  // needs redrawing.
  bool feed(char c, IMapReplyWriter& out);

  MapConsoleState& state() { return state_; }
  const MapConsoleState& state() const { return state_; }

  // Fired the moment a line completes, before it is parsed and before any
  // reply is written. MapSerialConsole hangs a LOG_DBG off it.
  //
  // That is on purpose, not incidental: the log line then lands between the
  // command and its reply, on the same UART, which is exactly the hazard
  // the '<' marker exists to survive. Without it nothing logs in that
  // window and the interleaving is never actually exercised.
  //
  // A plain function pointer, not std::function -- see the firmware
  // CLAUDE.md on std::function's heap and binary cost.
  void setLineObserver(void (*observer)(std::string_view line)) { lineObserver_ = observer; }

 private:
  MapLineAssembler assembler_;
  MapConsoleState& state_;
  void (*lineObserver_)(std::string_view) = nullptr;
};
