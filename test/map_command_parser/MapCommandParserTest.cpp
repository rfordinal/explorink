// P3 gate 1: the whole command grammar, the line assembler and the console
// replies, tested natively with no device involved.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "MapCommandConsole.h"
#include "MapCommandParser.h"

namespace {

class CollectingWriter final : public IMapReplyWriter {
 public:
  void reply(const char* line) override { lines.emplace_back(line); }
  std::vector<std::string> lines;
};

// Feeds a whole line plus a terminator, returns the redraw flag.
bool feedLine(MapCommandConsole& console, CollectingWriter& out, const std::string& line,
              const char* terminator = "\n") {
  bool redraw = false;
  for (const char c : line) {
    if (console.feed(c, out)) redraw = true;
  }
  for (const char* t = terminator; *t != '\0'; ++t) {
    if (console.feed(*t, out)) redraw = true;
  }
  return redraw;
}

MapCommandError errorOf(const std::string& line) {
  const MapCommand cmd = parseMapCommand(line);
  EXPECT_EQ(cmd.type, MapCommandType::Error) << line;
  return cmd.error;
}

}  // namespace

// ---------------------------------------------------------------- grammar

TEST(MapCommandParser, PosMinimal) {
  const MapCommand cmd = parseMapCommand("pos 48.4372 17.0186");
  ASSERT_EQ(cmd.type, MapCommandType::Pos);
  EXPECT_EQ(cmd.latE7, 484372000);
  EXPECT_EQ(cmd.lonE7, 170186000);
  EXPECT_FALSE(cmd.hasHeading);
  EXPECT_FALSE(cmd.hasSpeed);
}

TEST(MapCommandParser, PosNegativeAndIntegerDegrees) {
  const MapCommand cmd = parseMapCommand("pos -33.8688 -0.5");
  ASSERT_EQ(cmd.type, MapCommandType::Pos);
  EXPECT_EQ(cmd.latE7, -338688000);
  EXPECT_EQ(cmd.lonE7, -5000000);

  const MapCommand whole = parseMapCommand("pos 48 17");
  ASSERT_EQ(whole.type, MapCommandType::Pos);
  EXPECT_EQ(whole.latE7, 480000000);
  EXPECT_EQ(whole.lonE7, 170000000);

  const MapCommand plus = parseMapCommand("pos +1.5 +2.25");
  ASSERT_EQ(plus.type, MapCommandType::Pos);
  EXPECT_EQ(plus.latE7, 15000000);
  EXPECT_EQ(plus.lonE7, 22500000);
}

TEST(MapCommandParser, PosExtremesAreInRange) {
  EXPECT_EQ(parseMapCommand("pos 90 180").type, MapCommandType::Pos);
  EXPECT_EQ(parseMapCommand("pos -90 -180").type, MapCommandType::Pos);
  EXPECT_EQ(parseMapCommand("pos 0 0").type, MapCommandType::Pos);
}

TEST(MapCommandParser, PosFractionBeyondSevenPlacesIsIgnored) {
  const MapCommand cmd = parseMapCommand("pos 48.43720009999 17.0");
  ASSERT_EQ(cmd.type, MapCommandType::Pos);
  EXPECT_EQ(cmd.latE7, 484372000);
  EXPECT_EQ(cmd.lonE7, 170000000);
}

TEST(MapCommandParser, PosOptionalTailBare) {
  const MapCommand cmd = parseMapCommand("pos 48.4372 17.0186 4 30");
  ASSERT_EQ(cmd.type, MapCommandType::Pos);
  EXPECT_TRUE(cmd.hasHeading);
  EXPECT_EQ(cmd.heading, 4);
  EXPECT_TRUE(cmd.hasSpeed);
  EXPECT_EQ(cmd.speedKmh, 30);
}

TEST(MapCommandParser, PosOptionalTailKeyworded) {
  const MapCommand cmd = parseMapCommand("pos 48.4372 17.0186 heading 4 speed 30");
  ASSERT_EQ(cmd.type, MapCommandType::Pos);
  EXPECT_EQ(cmd.heading, 4);
  EXPECT_EQ(cmd.speedKmh, 30);

  // Speed alone needs its keyword -- a bare tail value is the heading.
  const MapCommand speedOnly = parseMapCommand("pos 1 2 speed 55");
  ASSERT_EQ(speedOnly.type, MapCommandType::Pos);
  EXPECT_FALSE(speedOnly.hasHeading);
  EXPECT_TRUE(speedOnly.hasSpeed);
  EXPECT_EQ(speedOnly.speedKmh, 55);

  const MapCommand mixed = parseMapCommand("pos 1 2 7 speed 55");
  ASSERT_EQ(mixed.type, MapCommandType::Pos);
  EXPECT_EQ(mixed.heading, 7);
  EXPECT_EQ(mixed.speedKmh, 55);
}

TEST(MapCommandParser, HeadingZoomMarker) {
  const MapCommand heading = parseMapCommand("heading 15");
  ASSERT_EQ(heading.type, MapCommandType::Heading);
  EXPECT_EQ(heading.heading, 15);

  const MapCommand zoom = parseMapCommand("zoom 0");
  ASSERT_EQ(zoom.type, MapCommandType::Zoom);
  EXPECT_EQ(zoom.zoom, 0);

  const MapCommand marker = parseMapCommand("marker 4");
  ASSERT_EQ(marker.type, MapCommandType::Marker);
  EXPECT_EQ(marker.marker, 4);
}

TEST(MapCommandParser, Modes) {
  EXPECT_EQ(parseMapCommand("mode ride").mode, MapRideMode::Ride);
  EXPECT_EQ(parseMapCommand("mode hike").mode, MapRideMode::Hike);
  EXPECT_EQ(parseMapCommand("mode cycle").mode, MapRideMode::Cycle);
  EXPECT_EQ(parseMapCommand("mode cycle").type, MapCommandType::Mode);
}

TEST(MapCommandParser, BareCommands) {
  EXPECT_EQ(parseMapCommand("redraw").type, MapCommandType::Redraw);
  EXPECT_EQ(parseMapCommand("tiles").type, MapCommandType::Tiles);
  EXPECT_EQ(parseMapCommand("info").type, MapCommandType::Info);
}

TEST(MapCommandParser, WhitespaceIsTolerated) {
  const MapCommand cmd = parseMapCommand("  pos\t48.4372   17.0186  ");
  ASSERT_EQ(cmd.type, MapCommandType::Pos);
  EXPECT_EQ(cmd.latE7, 484372000);
}

TEST(MapCommandParser, EmptyLine) {
  EXPECT_EQ(parseMapCommand("").type, MapCommandType::Empty);
  EXPECT_EQ(parseMapCommand("   ").type, MapCommandType::Empty);
  EXPECT_EQ(parseMapCommand("\t \t").type, MapCommandType::Empty);
}

// ------------------------------------------------------------ bad grammar

TEST(MapCommandParser, UnknownCommand) {
  EXPECT_EQ(errorOf("fly 1 2"), MapCommandError::UnknownCommand);
  EXPECT_EQ(errorOf("POS 1 2"), MapCommandError::UnknownCommand);  // case sensitive
  EXPECT_EQ(errorOf("po"), MapCommandError::UnknownCommand);
  EXPECT_EQ(errorOf("!!!"), MapCommandError::UnknownCommand);
}

TEST(MapCommandParser, WrongArity) {
  EXPECT_EQ(errorOf("pos"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pos 48.4372"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pos 1 2 3 4 5"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pos 1 2 heading"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pos 1 2 speed"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pos 1 2 heading 3 heading 4"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("heading"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("heading 1 2"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("zoom"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("marker 1 2"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("mode"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("mode ride hard"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("redraw now"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("tiles all"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("info 1"), MapCommandError::BadArity);
  // More tokens than the tokeniser holds is an arity error, not a truncated
  // command that happens to parse.
  EXPECT_EQ(errorOf("pos 1 2 3 4 5 6 7 8 9 10"), MapCommandError::BadArity);
}

TEST(MapCommandParser, BadNumbers) {
  EXPECT_EQ(errorOf("pos abc 17"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("pos 48.4372 seventeen"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("pos 1.2.3 17"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("pos 4e5 17"), MapCommandError::BadNumber);  // exponent not accepted
  EXPECT_EQ(errorOf("pos - 17"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("pos . 17"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("pos 12x 17"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("pos 1 2 -3"), MapCommandError::BadNumber);  // heading is unsigned
  EXPECT_EQ(errorOf("heading x"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("heading 1.5"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("zoom -1"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("marker two"), MapCommandError::BadNumber);
}

TEST(MapCommandParser, OutOfRange) {
  EXPECT_EQ(errorOf("pos 90.0000001 0"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("pos -90.0000001 0"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("pos 0 180.0000001"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("pos 0 -180.0000001"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("pos 12345 0"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("pos 1 2 16"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("pos 1 2 4 70000"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("heading 16"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("heading 255"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("zoom 5"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("marker 5"), MapCommandError::OutOfRange);
}

TEST(MapCommandParser, BadMode) {
  EXPECT_EQ(errorOf("mode swim"), MapCommandError::BadMode);
  EXPECT_EQ(errorOf("mode Ride"), MapCommandError::BadMode);
}

TEST(MapCommandParser, ErrorTextIsStable) {
  EXPECT_STREQ(mapCommandErrorText(MapCommandError::UnknownCommand), "unknown_command");
  EXPECT_STREQ(mapCommandErrorText(MapCommandError::BadArity), "bad_arity");
  EXPECT_STREQ(mapCommandErrorText(MapCommandError::BadNumber), "bad_number");
  EXPECT_STREQ(mapCommandErrorText(MapCommandError::OutOfRange), "out_of_range");
  EXPECT_STREQ(mapCommandErrorText(MapCommandError::BadMode), "bad_mode");
  EXPECT_STREQ(mapCommandErrorText(MapCommandError::LineTooLong), "line_too_long");
}

// --------------------------------------------------------- line assembler

TEST(MapLineAssembler, LineSplitAcrossFeeds) {
  MapLineAssembler assembler;
  for (const char c : std::string("pos 48.4372")) {
    EXPECT_EQ(assembler.feed(c), MapLineAssembler::Result::Pending);
  }
  for (const char c : std::string(" 17.0186")) {
    EXPECT_EQ(assembler.feed(c), MapLineAssembler::Result::Pending);
  }
  ASSERT_EQ(assembler.feed('\n'), MapLineAssembler::Result::Line);
  EXPECT_EQ(assembler.line(), "pos 48.4372 17.0186");
}

TEST(MapLineAssembler, CrLfAndBareCrAndBareLf) {
  MapLineAssembler assembler;

  for (const char c : std::string("info")) assembler.feed(c);
  ASSERT_EQ(assembler.feed('\r'), MapLineAssembler::Result::Line);
  EXPECT_EQ(assembler.line(), "info");
  // The LF of a CRLF pair completes an empty line, which is a no-op.
  ASSERT_EQ(assembler.feed('\n'), MapLineAssembler::Result::Line);
  EXPECT_EQ(assembler.line(), "");

  for (const char c : std::string("redraw")) assembler.feed(c);
  ASSERT_EQ(assembler.feed('\n'), MapLineAssembler::Result::Line);
  EXPECT_EQ(assembler.line(), "redraw");
}

TEST(MapLineAssembler, OverLongLineIsDiscardedNotTruncated) {
  MapLineAssembler assembler;
  const std::string tooLong(MapLineAssembler::kMaxLine + 40, 'x');
  for (const char c : tooLong) {
    ASSERT_EQ(assembler.feed(c), MapLineAssembler::Result::Pending);
  }
  EXPECT_EQ(assembler.feed('\n'), MapLineAssembler::Result::Overflow);

  // And the next line is clean.
  for (const char c : std::string("info")) assembler.feed(c);
  ASSERT_EQ(assembler.feed('\n'), MapLineAssembler::Result::Line);
  EXPECT_EQ(assembler.line(), "info");
}

TEST(MapLineAssembler, ExactlyMaxLineIsAccepted) {
  MapLineAssembler assembler;
  const std::string atLimit(MapLineAssembler::kMaxLine, 'x');
  for (const char c : atLimit) assembler.feed(c);
  ASSERT_EQ(assembler.feed('\n'), MapLineAssembler::Result::Line);
  EXPECT_EQ(assembler.line().size(), MapLineAssembler::kMaxLine);
}

// ---------------------------------------------------------------- console

TEST(MapCommandConsole, PosRepliesOkAndRedraws) {
  MapCommandConsole console;
  CollectingWriter out;

  EXPECT_TRUE(feedLine(console, out, "pos 48.4372 17.0186 heading 4"));
  ASSERT_EQ(out.lines.size(), 1u);
  EXPECT_EQ(out.lines[0], "OK");

  EXPECT_TRUE(console.state().hasPosition());
  EXPECT_EQ(console.state().latE7(), 484372000);
  EXPECT_EQ(console.state().lonE7(), 170186000);
  EXPECT_EQ(console.state().heading(), 4);
  EXPECT_EQ(console.state().seq(), 1u);
}

TEST(MapCommandConsole, EmptyLineIsSilent) {
  MapCommandConsole console;
  CollectingWriter out;
  EXPECT_FALSE(feedLine(console, out, ""));
  EXPECT_FALSE(feedLine(console, out, "   "));
  EXPECT_TRUE(out.lines.empty());
}

TEST(MapCommandConsole, ErrorLinesCarryTheReason) {
  MapCommandConsole console;
  CollectingWriter out;
  EXPECT_FALSE(feedLine(console, out, "fly"));
  EXPECT_FALSE(feedLine(console, out, "heading 99"));
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "ERR unknown_command");
  EXPECT_EQ(out.lines[1], "ERR out_of_range");
}

TEST(MapCommandConsole, OverLongLineRepliesErrAndKeepsGoing) {
  MapCommandConsole console;
  CollectingWriter out;

  EXPECT_FALSE(feedLine(console, out, std::string(MapLineAssembler::kMaxLine + 10, 'x')));
  ASSERT_EQ(out.lines.size(), 1u);
  EXPECT_EQ(out.lines[0], "ERR line_too_long");

  EXPECT_TRUE(feedLine(console, out, "pos 1 2"));
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[1], "OK");
}

TEST(MapCommandConsole, UnimplementedCommandsParseThenRefuse) {
  MapCommandConsole console;
  CollectingWriter out;
  for (const char* line : {"zoom 3", "marker 2", "mode hike", "tiles"}) {
    EXPECT_FALSE(feedLine(console, out, line)) << line;
  }
  ASSERT_EQ(out.lines.size(), 4u);
  for (const std::string& reply : out.lines) EXPECT_EQ(reply, "ERR unimplemented");
  EXPECT_EQ(console.state().seq(), 0u);
}

TEST(MapCommandConsole, RedrawBumpsSeqWithoutMoving) {
  MapCommandConsole console;
  CollectingWriter out;
  feedLine(console, out, "pos 1 2");
  const int32_t lat = console.state().latE7();
  EXPECT_TRUE(feedLine(console, out, "redraw"));
  EXPECT_EQ(console.state().latE7(), lat);
  EXPECT_EQ(console.state().seq(), 2u);
}

TEST(MapCommandConsole, HeadingOnlyKeepsPosition) {
  MapCommandConsole console;
  CollectingWriter out;
  feedLine(console, out, "pos 48.4372 17.0186");
  EXPECT_TRUE(feedLine(console, out, "heading 9"));
  EXPECT_EQ(console.state().heading(), 9);
  EXPECT_EQ(console.state().latE7(), 484372000);
}

TEST(MapCommandConsole, InfoReportsStateAndEndsWithOk) {
  MapCommandConsole console;
  CollectingWriter out;
  feedLine(console, out, "pos -0.5 17.0186 heading 3 speed 42");
  out.lines.clear();

  EXPECT_FALSE(feedLine(console, out, "info"));
  ASSERT_FALSE(out.lines.empty());
  EXPECT_EQ(out.lines.back(), "OK");

  const std::vector<std::string> expected = {
      "INFO pos=1",        "INFO lat=-0.5000000", "INFO lon=17.0186000",
      "INFO heading=3",    "INFO speed_kmh=42",   "INFO seq=1",
  };
  for (const std::string& want : expected) {
    EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), want), out.lines.end()) << want;
  }
  // No heap provider set natively, so no heap line.
  for (const std::string& line : out.lines) EXPECT_EQ(line.rfind("INFO heap=", 0), std::string::npos);
}

TEST(MapCommandConsole, InfoUsesTheHeapProviderWhenSet) {
  MapCommandConsole console;
  CollectingWriter out;
  console.state().setFreeHeapProvider([]() -> uint32_t { return 123456; });
  feedLine(console, out, "info");
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), std::string("INFO heap=123456")), out.lines.end());
}

// The observer is what puts a log line between a command and its reply on
// the device. Order matters: if it fired after execute() the log would land
// after the reply, where a sender has already stopped reading, and the
// marker filter would never be exercised.
std::vector<std::string> g_observed;

TEST(MapCommandConsole, LineObserverFiresBeforeTheReply) {
  MapCommandConsole console;
  CollectingWriter out;
  g_observed.clear();
  console.setLineObserver([](std::string_view line) { g_observed.emplace_back(line); });

  feedLine(console, out, "pos 48.4372 17.0186");
  ASSERT_EQ(g_observed.size(), 1u);
  EXPECT_EQ(g_observed[0], "pos 48.4372 17.0186");
  EXPECT_EQ(out.lines.size(), 1u);

  // Bad lines are observed too -- the device should log what it was sent,
  // not only what it understood.
  feedLine(console, out, "fly");
  ASSERT_EQ(g_observed.size(), 2u);
  EXPECT_EQ(g_observed[1], "fly");
  EXPECT_EQ(out.lines.back(), "ERR unknown_command");
}

TEST(MapCommandConsole, TwoCommandsInOneBurst) {
  MapCommandConsole console;
  CollectingWriter out;
  const std::string burst = "pos 48.4372 17.0186\r\nheading 8\n";
  for (const char c : burst) console.feed(c, out);
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "OK");
  EXPECT_EQ(out.lines[1], "OK");
  EXPECT_EQ(console.state().heading(), 8);
  EXPECT_EQ(console.state().seq(), 2u);
}
