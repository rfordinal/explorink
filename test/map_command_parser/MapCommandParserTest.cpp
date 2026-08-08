// P3 gate 1: the whole command grammar, the line assembler and the console
// replies, tested natively with no device involved.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "MapCommandConsole.h"
#include "MapCommandParser.h"

namespace {

// One shared, ordered record of everything the console emitted, replies and
// observer calls alike. Two separate containers cannot show which came
// first, and for the observer the ordering is the whole point.
std::vector<std::string> g_events;

class CollectingWriter final : public IMapReplyWriter {
 public:
  void reply(const char* line) override {
    lines.emplace_back(line);
    g_events.emplace_back(std::string("reply:") + line);
  }
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
  EXPECT_FALSE(cmd.hasAltitude);
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

TEST(MapCommandParser, PosAltitude) {
  const MapCommand cmd = parseMapCommand("pos 48.4372 17.0186 alt 412");
  ASSERT_EQ(cmd.type, MapCommandType::Pos);
  EXPECT_TRUE(cmd.hasAltitude);
  EXPECT_EQ(cmd.altitudeM, 412);

  // Below sea level is a real place -- alt is signed, unlike heading/speed.
  const MapCommand below = parseMapCommand("pos 31.5 35.5 alt -420");
  ASSERT_EQ(below.type, MapCommandType::Pos);
  EXPECT_TRUE(below.hasAltitude);
  EXPECT_EQ(below.altitudeM, -420);

  // Combines with heading and speed, in any order relative to alt.
  const MapCommand combined = parseMapCommand("pos 1 2 heading 4 alt 100 speed 30");
  ASSERT_EQ(combined.type, MapCommandType::Pos);
  EXPECT_EQ(combined.heading, 4);
  EXPECT_EQ(combined.altitudeM, 100);
  EXPECT_EQ(combined.speedKmh, 30);

  // alt never fills bare -- a bare tail value is heading, then speed, never
  // altitude, so this is speed=100 with no altitude at all.
  const MapCommand bare = parseMapCommand("pos 1 2 5 100");
  ASSERT_EQ(bare.type, MapCommandType::Pos);
  EXPECT_EQ(bare.heading, 5);
  EXPECT_EQ(bare.speedKmh, 100);
  EXPECT_FALSE(bare.hasAltitude);
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
  EXPECT_EQ(errorOf("pos 1 2 alt"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pos 1 2 heading 3 heading 4"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pos 1 2 alt 3 alt 4"), MapCommandError::BadArity);
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
  EXPECT_EQ(errorOf("pos 1 2 alt x"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("pos 1 2 alt 1.5"), MapCommandError::BadNumber);  // altitude is whole metres
  EXPECT_EQ(errorOf("pos 1 2 alt -"), MapCommandError::BadNumber);
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
  EXPECT_EQ(errorOf("pos 1 2 alt 9001"), MapCommandError::OutOfRange);   // past Everest's margin
  EXPECT_EQ(errorOf("pos 1 2 alt -1001"), MapCommandError::OutOfRange);  // past the Dead Sea's margin
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
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  EXPECT_TRUE(feedLine(console, out, "pos 48.4372 17.0186 heading 4"));
  ASSERT_EQ(out.lines.size(), 1u);
  EXPECT_EQ(out.lines[0], "OK");

  EXPECT_TRUE(console.state().hasPosition());
  EXPECT_EQ(console.state().latE7(), 484372000);
  EXPECT_EQ(console.state().lonE7(), 170186000);
  EXPECT_EQ(console.state().heading(), 4);
  EXPECT_EQ(console.state().seq(), 1u);
  EXPECT_FALSE(console.state().hasAltitude());
}

TEST(MapCommandConsole, PosAltitudePersistsUntilOverwritten) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  feedLine(console, out, "pos 48.4372 17.0186 alt 412");
  EXPECT_TRUE(console.state().hasAltitude());
  EXPECT_EQ(console.state().altitudeM(), 412);

  // A later pos with no alt keyword leaves the last altitude in place --
  // same behaviour as heading/speed, which also only update on hasX.
  feedLine(console, out, "pos 48.4380 17.0190");
  EXPECT_TRUE(console.state().hasAltitude());
  EXPECT_EQ(console.state().altitudeM(), 412);
}

TEST(MapCommandConsole, EmptyLineIsSilent) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  EXPECT_FALSE(feedLine(console, out, ""));
  EXPECT_FALSE(feedLine(console, out, "   "));
  EXPECT_TRUE(out.lines.empty());
}

TEST(MapCommandConsole, ErrorLinesCarryTheReason) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  EXPECT_FALSE(feedLine(console, out, "fly"));
  EXPECT_FALSE(feedLine(console, out, "heading 99"));
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "ERR unknown_command");
  EXPECT_EQ(out.lines[1], "ERR out_of_range");
}

TEST(MapCommandConsole, OverLongLineRepliesErrAndKeepsGoing) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  EXPECT_FALSE(feedLine(console, out, std::string(MapLineAssembler::kMaxLine + 10, 'x')));
  ASSERT_EQ(out.lines.size(), 1u);
  EXPECT_EQ(out.lines[0], "ERR line_too_long");

  EXPECT_TRUE(feedLine(console, out, "pos 1 2"));
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[1], "OK");
}

TEST(MapCommandConsole, LadderAndModeCommandsTakeEffect) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  // The last three `ERR unimplemented` commands. All of them now move a
  // number and ask for a redraw; nothing in the grammar changed.
  for (const char* line : {"zoom 3", "marker 2", "mode hike"}) {
    EXPECT_TRUE(feedLine(console, out, line)) << line;
  }
  ASSERT_EQ(out.lines.size(), 3u);
  for (const std::string& reply : out.lines) EXPECT_EQ(reply, "OK");
  EXPECT_EQ(console.state().zoomStep(), 3);
  EXPECT_EQ(console.state().markerStep(), 2);
  EXPECT_EQ(console.state().mode(), MapRideMode::Hike);
  EXPECT_EQ(console.state().seq(), 3u);
}

TEST(MapCommandConsole, TwoChannelsShareOneState) {
  // What MapActivity builds: one state, one assembler per transport. A
  // command over either channel lands on the same numbers, and a line split
  // across the two does not interleave into one buffer.
  MapConsoleState state;
  MapCommandConsole serial(state);
  MapCommandConsole ble(state);
  CollectingWriter out;

  EXPECT_TRUE(feedLine(serial, out, "zoom 4"));
  EXPECT_EQ(ble.state().zoomStep(), 4);

  EXPECT_TRUE(feedLine(ble, out, "mode cycle"));
  EXPECT_EQ(serial.state().mode(), MapRideMode::Cycle);

  // Half a line on each channel. Neither completes, so neither runs, and
  // the halves never form one command.
  CollectingWriter halves;
  EXPECT_FALSE(serial.feed('z', halves));
  EXPECT_FALSE(ble.feed('m', halves));
  EXPECT_TRUE(halves.lines.empty());
  EXPECT_EQ(state.zoomStep(), 4);
}

TEST(MapCommandConsole, LaddersPushedBackByTheActivityAreWhatInfoReports) {
  // MapActivity owns the ladders; a button press changes them without any
  // command being typed, and `info` must report the screen, not the last
  // thing typed.
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  EXPECT_TRUE(feedLine(console, out, "zoom 1"));
  state.setLadders(/*zoomStep=*/4, /*markerStep=*/0, MapRideMode::Cycle);

  out.lines.clear();
  EXPECT_FALSE(feedLine(console, out, "info"));
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), "INFO zoom=4"), out.lines.end());
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), "INFO marker=0"), out.lines.end());
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), "INFO mode=cycle"), out.lines.end());
}

TEST(MapCommandConsole, TilesCommandBeforeAnyResetReportsNone) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  EXPECT_FALSE(feedLine(console, out, "tiles"));
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "INFO tiles=none");
  EXPECT_EQ(out.lines[1], "OK");
  EXPECT_EQ(console.state().seq(), 0u);  // tiles never redraws or bumps seq
}

TEST(MapCommandConsole, TilesCommandListsTheLastSnapshot) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  // What MapActivity::renderViewport() pushes after a reset: a 2x1 range,
  // tile at (col0, row0) missing.
  MapTileRangeSnapshot range;
  range.valid = true;
  range.z = 13;
  range.col0 = 4483;
  range.row0 = 2832;
  range.col1 = 4484;
  range.row1 = 2832;
  range.unavailableMask = 0b1;  // column-major: index 0 is (col0, row0)
  console.state().setTileRange(range);

  EXPECT_FALSE(feedLine(console, out, "tiles"));
  ASSERT_EQ(out.lines.size(), 3u);
  EXPECT_EQ(out.lines[0], "INFO tile_13_4483_2832=missing");
  EXPECT_EQ(out.lines[1], "INFO tile_13_4484_2832=ok");
  EXPECT_EQ(out.lines[2], "OK");
}

// ---------------------------------------------------------------- missing

namespace {

// Stands in for MapActivity's MissingTilesStore adapter. A vector rather than
// the real store, which needs ArduinoJson and an SD card.
class FakeMissingTiles final : public IMissingTilesSource {
 public:
  // The real source sorts by fetch priority here (MissingTilePriority.h has
  // its own suite for the policy). This one only counts the calls, which is
  // what this suite is about: *when* the console asks for an ordering. It
  // leaves `tiles` alone so the paging tests below can still name entries by
  // the order they were inserted in.
  void orderForFetch() override { ++orderCalls; }
  size_t missingTileCount() const override { return tiles.size(); }
  MapMissingTile missingTileAt(size_t index) const override { return tiles[index]; }
  std::vector<MapMissingTile> tiles;
  int orderCalls = 0;
};

// n entries, each distinguishable by its column so a page boundary is visible.
FakeMissingTiles fakeList(size_t n) {
  FakeMissingTiles list;
  list.tiles.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    list.tiles.push_back(MapMissingTile{13, static_cast<uint32_t>(4000 + i), 2832, static_cast<uint32_t>(i + 1)});
  }
  return list;
}

}  // namespace

TEST(MapCommandParser, MissingBareAndWithOffset) {
  const MapCommand bare = parseMapCommand("missing");
  ASSERT_EQ(bare.type, MapCommandType::Missing);
  EXPECT_EQ(bare.missingOffset, 0);

  const MapCommand paged = parseMapCommand("missing 40");
  ASSERT_EQ(paged.type, MapCommandType::Missing);
  EXPECT_EQ(paged.missingOffset, 40);
}

TEST(MapCommandParser, MissingRejectsJunk) {
  EXPECT_EQ(errorOf("missing 0 0"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("missing x"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("missing 65536"), MapCommandError::OutOfRange);
}

TEST(MapCommandConsole, MissingWithNoSourceWiredSaysUnavailable) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  EXPECT_FALSE(feedLine(console, out, "missing"));
  ASSERT_EQ(out.lines.size(), 2u);
  // Not "total=0": a build that never wired the store must not read as a
  // device that needs no tiles.
  EXPECT_EQ(out.lines[0], "INFO missing=unavailable");
  EXPECT_EQ(out.lines[1], "OK");
  EXPECT_EQ(console.state().seq(), 0u);  // missing never redraws
}

TEST(MapCommandConsole, MissingEmptyListIsTotalZeroAndDone) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  FakeMissingTiles list;
  state.setMissingTilesSource(&list);

  EXPECT_FALSE(feedLine(console, out, "missing"));
  ASSERT_EQ(out.lines.size(), 4u);
  EXPECT_EQ(out.lines[0], "INFO missing_total=0");
  EXPECT_EQ(out.lines[1], "INFO missing_offset=0");
  EXPECT_EQ(out.lines[2], "INFO missing_next=done");
  EXPECT_EQ(out.lines[3], "OK");
}

TEST(MapCommandConsole, MissingPrintsEveryFieldOfAnEntry) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  FakeMissingTiles list;
  list.tiles.push_back(MapMissingTile{13, 4483, 2832, 7});
  state.setMissingTilesSource(&list);

  EXPECT_FALSE(feedLine(console, out, "missing"));
  ASSERT_EQ(out.lines.size(), 5u);
  EXPECT_EQ(out.lines[0], "INFO missing_total=1");
  EXPECT_EQ(out.lines[2], "INFO missing_13_4483_2832=7");
  EXPECT_EQ(out.lines[3], "INFO missing_next=done");
}

TEST(MapCommandConsole, MissingWidestPossibleEntryIsNotTruncated) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  FakeMissingTiles list;
  list.tiles.push_back(MapMissingTile{255, 4294967295u, 4294967295u, 4294967295u});
  state.setMissingTilesSource(&list);

  feedLine(console, out, "missing");
  // The whole point of kReplyBuf being 64: a clipped coordinate would send
  // the laptop tool after a tile the device never asked for.
  EXPECT_EQ(out.lines[2], "INFO missing_255_4294967295_4294967295=4294967295");
}

TEST(MapCommandConsole, MissingPagesAtTwentyAndSaysWhereToResume) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  FakeMissingTiles list = fakeList(25);
  state.setMissingTilesSource(&list);

  feedLine(console, out, "missing");
  // total, offset, 20 entries, next, OK
  ASSERT_EQ(out.lines.size(), 24u);
  EXPECT_EQ(out.lines[0], "INFO missing_total=25");
  EXPECT_EQ(out.lines[1], "INFO missing_offset=0");
  EXPECT_EQ(out.lines[2], "INFO missing_13_4000_2832=1");
  EXPECT_EQ(out.lines[21], "INFO missing_13_4019_2832=20");
  EXPECT_EQ(out.lines[22], "INFO missing_next=20");

  out.lines.clear();
  feedLine(console, out, "missing 20");
  // total, offset, 5 entries, next, OK
  ASSERT_EQ(out.lines.size(), 9u);
  EXPECT_EQ(out.lines[1], "INFO missing_offset=20");
  EXPECT_EQ(out.lines[2], "INFO missing_13_4020_2832=21");
  EXPECT_EQ(out.lines[6], "INFO missing_13_4024_2832=25");
  EXPECT_EQ(out.lines[7], "INFO missing_next=done");
}

TEST(MapCommandConsole, MissingOrdersTheListOnPageZeroOnly) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  FakeMissingTiles list = fakeList(25);
  state.setMissingTilesSource(&list);

  feedLine(console, out, "missing");
  EXPECT_EQ(list.orderCalls, 1);

  // A re-sort here would move entries across the page boundary page 0 already
  // reported, and the reader would miss some and see others twice.
  feedLine(console, out, "missing 20");
  EXPECT_EQ(list.orderCalls, 1);

  // A fresh listing is a fresh ordering -- counts may have moved since.
  feedLine(console, out, "missing");
  EXPECT_EQ(list.orderCalls, 2);
}

TEST(MapCommandConsole, MissingOffsetPastTheEndIsAnEmptyPageNotAnError) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  FakeMissingTiles list = fakeList(3);
  state.setMissingTilesSource(&list);

  // The last request of a paging loop can legitimately land here.
  EXPECT_FALSE(feedLine(console, out, "missing 900"));
  ASSERT_EQ(out.lines.size(), 4u);
  EXPECT_EQ(out.lines[0], "INFO missing_total=3");
  EXPECT_EQ(out.lines[1], "INFO missing_offset=900");
  EXPECT_EQ(out.lines[2], "INFO missing_next=done");
  EXPECT_EQ(out.lines[3], "OK");
}

// ------------------------------------------------------------------- skip

TEST(MapCommandParser, SkipTakesATileAndAnOptionalReason) {
  const MapCommand bare = parseMapCommand("skip 12 2199 1416");
  ASSERT_EQ(bare.type, MapCommandType::Skip);
  EXPECT_EQ(bare.skipZ, 12);
  EXPECT_EQ(bare.skipCol, 2199u);
  EXPECT_EQ(bare.skipRow, 1416u);
  EXPECT_STREQ(bare.skipReason, "");

  const MapCommand withReason = parseMapCommand("skip 12 2199 1416 nocdn");
  ASSERT_EQ(withReason.type, MapCommandType::Skip);
  EXPECT_STREQ(withReason.skipReason, "nocdn");
}

TEST(MapCommandParser, SkipReasonIsTruncatedNotRejected) {
  // A long reason word is still a legitimate skip -- the tile is genuinely
  // unavailable either way, and the device shows a count, not the word.
  const MapCommand cmd = parseMapCommand("skip 12 2199 1416 aaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  ASSERT_EQ(cmd.type, MapCommandType::Skip);
  EXPECT_EQ(strlen(cmd.skipReason), MapCommand::kSkipReasonBytes - 1);
}

TEST(MapCommandParser, SkipRejectsJunk) {
  EXPECT_EQ(errorOf("skip"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("skip 12 2199"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("skip 12 2199 1416 why extra"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("skip 12 x 1416"), MapCommandError::BadNumber);
  // z lives in a uint8_t everywhere else (MissingTileHit, MapTileCoord).
  EXPECT_EQ(errorOf("skip 256 1 1"), MapCommandError::OutOfRange);
}

TEST(MapCommandConsole, SkipCountsAndNeverRedraws) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  EXPECT_FALSE(feedLine(console, out, "skip 12 2199 1416 nocdn"));
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "INFO skipped=1");
  EXPECT_EQ(out.lines[1], "OK");
  // A tile the phone cannot supply changes nothing on the panel -- refreshing
  // e-ink for it would cost two seconds to display the same map.
  EXPECT_EQ(console.state().seq(), 0u);

  EXPECT_EQ(state.skips().count, 1u);
  EXPECT_EQ(state.skips().z, 12);
  EXPECT_EQ(state.skips().col, 2199u);
  EXPECT_EQ(state.skips().row, 1416u);
  EXPECT_STREQ(state.skips().reason, "nocdn");

  out.lines.clear();
  feedLine(console, out, "skip 11 1099 708");
  EXPECT_EQ(out.lines[0], "INFO skipped=2");
  EXPECT_EQ(state.skips().count, 2u);
  // The tally holds the latest, not a list -- the screen shows a number.
  EXPECT_EQ(state.skips().z, 11);
  EXPECT_STREQ(state.skips().reason, "");
}

TEST(MapCommandConsole, SkipObserverHearsEveryTileNotJustTheLast) {
  // The tile sync screen shows a row per tile, so it needs each skip, not a
  // "last one plus a counter" snapshot -- two skips between two polls would
  // otherwise leave a row stuck on "waiting" forever.
  struct Recorder final : IMapSkipObserver {
    std::vector<std::string> seen;
    void onTileSkipped(uint8_t z, uint32_t col, uint32_t row) override {
      seen.push_back(std::to_string(z) + "/" + std::to_string(col) + "/" + std::to_string(row));
    }
  } recorder;

  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  state.setSkipObserver(&recorder);

  feedLine(console, out, "skip 12 2199 1416 nosource");
  feedLine(console, out, "skip 11 1099 708 fmt3");
  feedLine(console, out, "skip 13 4482 2789");

  ASSERT_EQ(recorder.seen.size(), 3u);
  EXPECT_EQ(recorder.seen[0], "12/2199/1416");
  EXPECT_EQ(recorder.seen[1], "11/1099/708");
  EXPECT_EQ(recorder.seen[2], "13/4482/2789");
  // The tally still counts, for the summary line.
  EXPECT_EQ(state.skips().count, 3u);
}

TEST(MapCommandConsole, SkipWithNoObserverStillCounts) {
  // The map screen wires no observer -- it has no rows to mark.
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  feedLine(console, out, "skip 12 1 1");
  EXPECT_EQ(state.skips().count, 1u);
}

TEST(MapCommandConsole, SkipTallyIsClearedForANewFetch) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  feedLine(console, out, "skip 12 2199 1416");
  feedLine(console, out, "skip 12 2200 1416");
  ASSERT_EQ(state.skips().count, 2u);

  // What MapActivity::startFetch() does, so the count on screen belongs to
  // this fetch and not to the last one.
  state.clearSkips();
  EXPECT_EQ(state.skips().count, 0u);
  EXPECT_STREQ(state.skips().reason, "");
}

TEST(MapCommandConsole, SkipDoesNotTouchTheMissingList) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  FakeMissingTiles list = fakeList(3);
  state.setMissingTilesSource(&list);

  feedLine(console, out, "skip 13 4000 2832");
  // The tile is still absent, so it stays on the list. Only an arrival clears
  // an entry (MissingTilesStore::forget()).
  out.lines.clear();
  feedLine(console, out, "missing");
  EXPECT_EQ(out.lines[0], "INFO missing_total=3");
}

TEST(MapCommandConsole, InfoReportsRealZoomLodMppAndTileStats) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  console.state().setZoomInfo(/*zoomStep=*/0, /*lod=*/13, /*mpp=*/3.0);
  console.state().setRenderStats(/*tilesOk=*/3, /*tilesMissing=*/1, /*ways=*/2065, /*bytesRead=*/61234,
                                 /*waysFiltered=*/412);

  EXPECT_FALSE(feedLine(console, out, "info"));
  const std::vector<std::string> expected = {
      "INFO zoom=0",          "INFO lod=13",    "INFO mpp=3.0",     "INFO tiles_ok=3",
      "INFO tiles_missing=1", "INFO ways=2065", "INFO bytes=61234", "INFO ways_filtered=412",
  };
  for (const std::string& want : expected) {
    EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), want), out.lines.end()) << want;
  }
  // marker and mode were the last two unimplemented values; same keys,
  // real values now.
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), "INFO marker=0"), out.lines.end());
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), "INFO mode=ride"), out.lines.end());
  EXPECT_EQ(std::find(out.lines.begin(), out.lines.end(), "INFO marker=unimplemented"), out.lines.end());
  EXPECT_EQ(std::find(out.lines.begin(), out.lines.end(), "INFO mode=unimplemented"), out.lines.end());
  // zoom/lod/mpp must not still claim to be unimplemented.
  EXPECT_EQ(std::find(out.lines.begin(), out.lines.end(), "INFO zoom=unimplemented"), out.lines.end());
  EXPECT_EQ(std::find(out.lines.begin(), out.lines.end(), "INFO lod=unimplemented"), out.lines.end());
  EXPECT_EQ(std::find(out.lines.begin(), out.lines.end(), "INFO mpp=unimplemented"), out.lines.end());
}

TEST(MapCommandConsole, RedrawBumpsSeqWithoutMoving) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  feedLine(console, out, "pos 1 2");
  const int32_t lat = console.state().latE7();
  EXPECT_TRUE(feedLine(console, out, "redraw"));
  EXPECT_EQ(console.state().latE7(), lat);
  EXPECT_EQ(console.state().seq(), 2u);
}

TEST(MapCommandConsole, HeadingOnlyKeepsPosition) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  feedLine(console, out, "pos 48.4372 17.0186");
  EXPECT_TRUE(feedLine(console, out, "heading 9"));
  EXPECT_EQ(console.state().heading(), 9);
  EXPECT_EQ(console.state().latE7(), 484372000);
}

TEST(MapCommandConsole, InfoReportsStateAndEndsWithOk) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  feedLine(console, out, "pos -0.5 17.0186 heading 3 speed 42");
  out.lines.clear();

  EXPECT_FALSE(feedLine(console, out, "info"));
  ASSERT_FALSE(out.lines.empty());
  EXPECT_EQ(out.lines.back(), "OK");

  const std::vector<std::string> expected = {
      "INFO pos=1",   "INFO lat=-0.5000000", "INFO lon=17.0186000", "INFO heading=3",
      "INFO speed_kmh=42", "INFO alt_m=unset", "INFO seq=1",
  };
  for (const std::string& want : expected) {
    EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), want), out.lines.end()) << want;
  }
  // No heap provider set natively, so no heap line.
  for (const std::string& line : out.lines) EXPECT_EQ(line.rfind("INFO heap=", 0), std::string::npos);
}

TEST(MapCommandConsole, InfoReportsAltitudeWhenSet) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  feedLine(console, out, "pos -0.5 17.0186 alt -14");
  out.lines.clear();

  feedLine(console, out, "info");
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), "INFO alt_m=-14"), out.lines.end());
}

TEST(MapCommandConsole, InfoReportsTheTileFormatVersionOnlyWhenPushed) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  // Nothing pushed: the line is omitted rather than claiming version 0, which
  // is not a version any tile was ever built to.
  feedLine(console, out, "info");
  EXPECT_EQ(std::count_if(out.lines.begin(), out.lines.end(),
                          [](const std::string& l) { return l.rfind("INFO tile_fmt=", 0) == 0; }),
            0);

  out.lines.clear();
  state.setTileFormatVersion(2);
  feedLine(console, out, "info");
  // A supplier of tiles reads this before it pushes anything: a tile built to
  // another version passes CRC and is then refused on open.
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), "INFO tile_fmt=2"), out.lines.end());
}

TEST(MapCommandConsole, InfoUsesTheHeapProviderWhenSet) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  console.state().setFreeHeapProvider([]() -> uint32_t { return 123456; });
  feedLine(console, out, "info");
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), std::string("INFO heap=123456")), out.lines.end());
}

// The observer is what puts a log line between a command and its reply on
// the device. If it fired after the reply the log would land where a sender
// has already stopped reading, and the marker filter would never be
// exercised -- so the ordering is the entire reason the observer exists,
// and it is what this asserts. Interleaving both into one vector is what
// makes the order observable at all; two containers would pass either way.
TEST(MapCommandConsole, LineObserverFiresBeforeTheReply) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  g_events.clear();
  console.setLineObserver([](std::string_view line) { g_events.emplace_back("observe:" + std::string(line)); });

  feedLine(console, out, "pos 48.4372 17.0186");
  ASSERT_EQ(g_events.size(), 2u);
  EXPECT_EQ(g_events[0], "observe:pos 48.4372 17.0186");
  EXPECT_EQ(g_events[1], "reply:OK");

  // Bad lines are observed too -- the device should log what it was sent,
  // not only what it understood -- and in the same order.
  feedLine(console, out, "fly");
  ASSERT_EQ(g_events.size(), 4u);
  EXPECT_EQ(g_events[2], "observe:fly");
  EXPECT_EQ(g_events[3], "reply:ERR unknown_command");
}

TEST(MapCommandConsole, TwoCommandsInOneBurst) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  const std::string burst = "pos 48.4372 17.0186\r\nheading 8\n";
  for (const char c : burst) console.feed(c, out);
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "OK");
  EXPECT_EQ(out.lines[1], "OK");
  EXPECT_EQ(console.state().heading(), 8);
  EXPECT_EQ(console.state().seq(), 2u);
}
