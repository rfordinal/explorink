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
  const uint32_t magnitude = negative ? static_cast<uint32_t>(-static_cast<int64_t>(value))
                                      : static_cast<uint32_t>(value);
  snprintf(buf, bufLen, "%s%lu.%07lu", negative ? "-" : "",
           static_cast<unsigned long>(magnitude / 10000000u),
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

    case MapCommandType::Zoom:
    case MapCommandType::Marker:
    case MapCommandType::Mode:
    case MapCommandType::Tiles:
      // Parsed, understood, and deliberately not acted on: zoom, marker and
      // mode are P5, tiles needs P4's tile range to exist. The grammar is
      // fixed now so it is fixed once (docs/prototype-plan.md).
      out.reply("ERR unimplemented");
      return false;
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

  // Keys that exist in the grammar but have no value yet. Emitted so the
  // key set a script greps for does not change when P4/P5 fill them in.
  out.reply("INFO zoom=unimplemented");
  out.reply("INFO marker=unimplemented");
  out.reply("INFO mode=unimplemented");
  out.reply("INFO lod=unimplemented");
  out.reply("INFO mpp=unimplemented");

  if (freeHeapProvider_ != nullptr) {
    snprintf(line, sizeof(line), "INFO heap=%lu", static_cast<unsigned long>(freeHeapProvider_()));
    out.reply(line);
  }
}

bool MapCommandConsole::feed(char c, IMapReplyWriter& out) {
  switch (assembler_.feed(c)) {
    case MapLineAssembler::Result::Pending:
      return false;

    case MapLineAssembler::Result::Line: {
      const std::string_view line = assembler_.line();
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
