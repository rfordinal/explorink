#include "MapCommandConsole.h"

#include <cstdio>

namespace {

// Widest reply is an INFO line carrying a full coordinate.
constexpr size_t kReplyBuf = 48;

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
