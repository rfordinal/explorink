#include "MapCommandConsole.h"

#include <cstdio>
#include <cstring>

namespace {

// Widest reply is a `missing` entry with every field at its type's maximum:
// "INFO missing_255_4294967295_4294967295=4294967295" is 49 characters plus
// the terminator. Real values are far shorter (z <= 13, col/row < 2^13), but
// a silently truncated tile coordinate would send the laptop tool after the
// wrong tile, so the buffer is sized for the type and not for the data.
constexpr size_t kReplyBuf = 64;

// int32 scaled by 1e7 back to plain decimal degrees. Integer only -- the
// device has no FPU, and a printf("%f") on a soft-float double is both slow
// and a chunk of flash nothing else here needs.
void formatE7(int32_t value, char* buf, size_t bufLen) {
  const bool negative = value < 0;
  const uint32_t magnitude =
      negative ? static_cast<uint32_t>(-static_cast<int64_t>(value)) : static_cast<uint32_t>(value);
  snprintf(buf, bufLen, "%s%lu.%07lu", negative ? "-" : "", static_cast<unsigned long>(magnitude / 10000000u),
           static_cast<unsigned long>(magnitude % 10000000u));
}

}  // namespace

MapLineAssembler::Result MapLineAssembler::feed(char c) {
  if (c == '\n' || c == '\r') {
    if (discarding_) {
      discarding_ = false;
      len_ = 0;
      lineLen_ = 0;
      buf_[0] = '\0';
      return Result::Overflow;
    }
    buf_[len_] = '\0';
    lineLen_ = len_;
    len_ = 0;
    return Result::Line;
  }

  if (discarding_) return Result::Pending;

  if (len_ >= kMaxLine) {
    // Too long. Drop what we have and swallow the rest of this line: a
    // truncated command is worse than no command, because it can parse.
    discarding_ = true;
    len_ = 0;
    return Result::Pending;
  }

  buf_[len_++] = c;
  return Result::Pending;
}

void MapLineAssembler::reset() {
  len_ = 0;
  lineLen_ = 0;
  discarding_ = false;
  buf_[0] = '\0';
}

bool MapConsoleState::execute(const MapCommand& cmd, IMapReplyWriter& out) {
  char line[kReplyBuf];

  switch (cmd.type) {
    case MapCommandType::Empty:
      // A blank line is not an error and is not a command. Answering it
      // would desynchronise a sender that reads one reply per command.
      return false;

    case MapCommandType::Error:
      snprintf(line, sizeof(line), "ERR %s", mapCommandErrorText(cmd.error));
      out.reply(line);
      return false;

    case MapCommandType::Pos:
      hasPosition_ = true;
      latE7_ = cmd.latE7;
      lonE7_ = cmd.lonE7;
      if (cmd.hasHeading) heading_ = cmd.heading;
      if (cmd.hasSpeed) speedKmh_ = cmd.speedKmh;
      ++seq_;
      out.reply("OK");
      return true;

    case MapCommandType::Heading:
      heading_ = cmd.heading;
      ++seq_;
      out.reply("OK");
      return true;

    case MapCommandType::Redraw:
      ++seq_;
      out.reply("OK");
      return true;

    case MapCommandType::Info:
      writeInfo(out);
      out.reply("OK");
      return false;

    case MapCommandType::Tiles:
      writeTiles(out);
      out.reply("OK");
      return false;

    case MapCommandType::Missing:
      writeMissing(cmd.missingOffset, out);
      out.reply("OK");
      return false;

    case MapCommandType::Skip: {
      // Counted, not acted on: the tile is still missing, so it stays on the
      // store's list. This only stops the fetch screen waiting for it.
      ++skips_.count;
      skips_.z = cmd.skipZ;
      skips_.col = cmd.skipCol;
      skips_.row = cmd.skipRow;
      // memcpy of a fixed field, not strncpy: both sides are the same array
      // size and the parser already nul-terminated it.
      memcpy(skips_.reason, cmd.skipReason, sizeof(skips_.reason));
      // Before the reply, so a screen that redraws on this has the tally and
      // the row state agreeing by the time the phone hears `OK`.
      if (skipObserver_ != nullptr) skipObserver_->onTileSkipped(cmd.skipZ, cmd.skipCol, cmd.skipRow);

      char line[kReplyBuf];
      snprintf(line, sizeof(line), "INFO skipped=%lu", static_cast<unsigned long>(skips_.count));
      out.reply(line);
      out.reply("OK");
      return false;
    }

    case MapCommandType::Zoom:
      // The parser already rejected anything outside 0-4, so the ladder is
      // never indexed off its end from here.
      zoomStep_ = cmd.zoom;
      ++seq_;
      out.reply("OK");
      return true;

    case MapCommandType::Marker:
      markerStep_ = cmd.marker;
      ++seq_;
      out.reply("OK");
      return true;

    case MapCommandType::Mode:
      mode_ = cmd.mode;
      ++seq_;
      out.reply("OK");
      // The caller is expected to push the new mode's *stored* ladder steps
      // back through setLadders() before reporting anything -- switching mode
      // restores that mode's steps, it does not carry the old ones across.
      return true;
  }

  return false;
}

void MapConsoleState::writeInfo(IMapReplyWriter& out) const {
  char line[kReplyBuf];
  char number[24];

  snprintf(line, sizeof(line), "INFO pos=%d", hasPosition_ ? 1 : 0);
  out.reply(line);

  formatE7(latE7_, number, sizeof(number));
  snprintf(line, sizeof(line), "INFO lat=%s", number);
  out.reply(line);

  formatE7(lonE7_, number, sizeof(number));
  snprintf(line, sizeof(line), "INFO lon=%s", number);
  out.reply(line);

  snprintf(line, sizeof(line), "INFO heading=%u", static_cast<unsigned>(heading_));
  out.reply(line);

  snprintf(line, sizeof(line), "INFO speed_kmh=%u", static_cast<unsigned>(speedKmh_));
  out.reply(line);

  snprintf(line, sizeof(line), "INFO seq=%lu", static_cast<unsigned long>(seq_));
  out.reply(line);

  // zoom/lod/mpp used to answer `unimplemented` -- there was nothing to
  // read before the merge with P4 gave the console a real viewport to ask
  // about. The key names are unchanged, only the values are now real; a
  // script grepping for these keys does not break.
  snprintf(line, sizeof(line), "INFO zoom=%u", static_cast<unsigned>(zoomStep_));
  out.reply(line);

  snprintf(line, sizeof(line), "INFO lod=%u", static_cast<unsigned>(lod_));
  out.reply(line);

  snprintf(line, sizeof(line), "INFO mpp=%.1f", mpp_);
  out.reply(line);

  // marker and mode were the last two `unimplemented` values. Same keys, real
  // values now: the marker-height ladder step and the travel mode whose class
  // mask is filtering the drawing.
  snprintf(line, sizeof(line), "INFO marker=%u", static_cast<unsigned>(markerStep_));
  out.reply(line);

  snprintf(line, sizeof(line), "INFO mode=%s", mapRideModeName(mode_));
  out.reply(line);

  // New keys, not previously in the grammar's `unimplemented` set: what the
  // last viewport reset actually loaded. Zero before the first reset.
  snprintf(line, sizeof(line), "INFO tiles_ok=%lu", static_cast<unsigned long>(tilesOk_));
  out.reply(line);

  snprintf(line, sizeof(line), "INFO tiles_missing=%lu", static_cast<unsigned long>(tilesMissing_));
  out.reply(line);

  snprintf(line, sizeof(line), "INFO ways=%lu", static_cast<unsigned long>(ways_));
  out.reply(line);

  // The mode filter's own evidence: the same coordinate in two modes reads
  // the same tiles and drops a different number of ways here.
  snprintf(line, sizeof(line), "INFO ways_filtered=%lu", static_cast<unsigned long>(waysFiltered_));
  out.reply(line);

  snprintf(line, sizeof(line), "INFO bytes=%lu", static_cast<unsigned long>(bytesRead_));
  out.reply(line);

  // Which .tib version this build reads. A supplier of tiles needs it before it
  // pushes anything -- a tile built to another version passes CRC and is then
  // refused on open (MapTileReader::kFormatVersion).
  if (tileFormatVersion_ != 0) {
    snprintf(line, sizeof(line), "INFO tile_fmt=%u", static_cast<unsigned>(tileFormatVersion_));
    out.reply(line);
  }

  // The link's real ATT MTU and what it leaves for a file chunk. The phone side
  // cannot see this from its end, and it decides the whole transfer speed.
  if (linkMtuProvider_ != nullptr) {
    const uint16_t mtu = linkMtuProvider_();
    if (mtu != 0) {
      snprintf(line, sizeof(line), "INFO mtu=%u", static_cast<unsigned>(mtu));
      out.reply(line);
      snprintf(line, sizeof(line), "INFO chunk_payload=%u", static_cast<unsigned>(mtu > 8 ? mtu - 8 : 0));
      out.reply(line);
    }
  }

  if (freeHeapProvider_ != nullptr) {
    snprintf(line, sizeof(line), "INFO heap=%lu", static_cast<unsigned long>(freeHeapProvider_()));
    out.reply(line);
  }
}

void MapConsoleState::writeTiles(IMapReplyWriter& out) const {
  if (!tileRange_.valid) {
    // Nothing to report before the first viewport reset -- no BLE fix or
    // pos/heading/redraw command has landed yet.
    out.reply("INFO tiles=none");
    return;
  }

  char line[kReplyBuf];
  const uint32_t rowSpan = tileRange_.row1 - tileRange_.row0 + 1;
  const uint32_t count = (tileRange_.col1 - tileRange_.col0 + 1) * rowSpan;
  // Same 32-tile cap as MapTileSource::unavailableMask() -- one bit per
  // index, and the range is never wider than that (MapViewport::kMaxTiles).
  for (uint32_t index = 0; index < count && index < 32; ++index) {
    const uint32_t col = tileRange_.col0 + index / rowSpan;
    const uint32_t row = tileRange_.row0 + index % rowSpan;
    const bool missing = (tileRange_.unavailableMask & (1u << index)) != 0;
    snprintf(line, sizeof(line), "INFO tile_%u_%u_%u=%s", static_cast<unsigned>(tileRange_.z),
             static_cast<unsigned>(col), static_cast<unsigned>(row), missing ? "missing" : "ok");
    out.reply(line);
  }
}

void MapConsoleState::writeMissing(uint16_t offset, IMapReplyWriter& out) const {
  if (missingTiles_ == nullptr) {
    // No store wired in. Distinct from an empty list on purpose: a laptop
    // tool must not read "the device needs no tiles" out of a build that
    // simply never connected the two.
    out.reply("INFO missing=unavailable");
    return;
  }

  // Page 0 is the start of a listing, so this is where the list goes into
  // fetch-priority order. Later pages walk the order page 0 fixed -- a
  // re-sort mid-listing would move entries across a page boundary and the
  // reader would miss some and see others twice. `missing 20` with no
  // `missing` before it therefore pages whatever order the store already
  // holds, which is a debugging convenience, not a contract.
  //
  // const method, non-const call: the console keeps no state of its own, and
  // the ordering happens in the source, which is not ours to be const about.
  if (offset == 0) missingTiles_->orderForFetch();

  char line[kReplyBuf];
  const size_t total = missingTiles_->missingTileCount();

  snprintf(line, sizeof(line), "INFO missing_total=%lu", static_cast<unsigned long>(total));
  out.reply(line);
  snprintf(line, sizeof(line), "INFO missing_offset=%u", static_cast<unsigned>(offset));
  out.reply(line);

  size_t index = offset;
  const size_t pageEnd = static_cast<size_t>(offset) + kMissingPageSize;
  const size_t end = pageEnd < total ? pageEnd : total;
  for (; index < end; ++index) {
    const MapMissingTile tile = missingTiles_->missingTileAt(index);
    snprintf(line, sizeof(line), "INFO missing_%u_%lu_%lu=%lu", static_cast<unsigned>(tile.z),
             static_cast<unsigned long>(tile.col), static_cast<unsigned long>(tile.row),
             static_cast<unsigned long>(tile.count));
    out.reply(line);
  }

  // Where the next command should start, or `done`. The reader never has to
  // do the arithmetic, so it cannot get it wrong -- and an offset past the
  // end simply lands here with nothing printed.
  if (index < total) {
    snprintf(line, sizeof(line), "INFO missing_next=%lu", static_cast<unsigned long>(index));
    out.reply(line);
  } else {
    out.reply("INFO missing_next=done");
  }
}

bool MapCommandConsole::feed(char c, IMapReplyWriter& out) {
  switch (assembler_.feed(c)) {
    case MapLineAssembler::Result::Pending:
      return false;

    case MapLineAssembler::Result::Line: {
      const std::string_view line = assembler_.line();
      // Observer before execute, never after: it is what puts a log line
      // between the command and its reply on the shared UART. Swapping
      // these two lines makes LineObserverFiresBeforeTheReply fail, which
      // is the point of that test.
      if (lineObserver_ != nullptr) lineObserver_(line);
      return state_.execute(parseMapCommand(line), out);
    }

    case MapLineAssembler::Result::Overflow: {
      MapCommand cmd;
      cmd.type = MapCommandType::Error;
      cmd.error = MapCommandError::LineTooLong;
      return state_.execute(cmd, out);
    }
  }

  return false;
}
