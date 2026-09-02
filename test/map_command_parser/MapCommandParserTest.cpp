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
  // Both bounds come off their own ladder, and the two ladders are different
  // lengths: seven zoom rungs, five marker rungs (MapRideMode.h).
  EXPECT_EQ(errorOf("zoom 7"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("marker 5"), MapCommandError::OutOfRange);
  EXPECT_EQ(parseMapCommand("zoom 6").type, MapCommandType::Zoom);
  EXPECT_EQ(parseMapCommand("zoom 6").zoom, 6u);
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
      "INFO pos=1",        "INFO lat=-0.5000000", "INFO lon=17.0186000", "INFO heading=3",
      "INFO speed_kmh=42", "INFO alt_m=unset",    "INFO seq=1",
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

// Nothing behind the command is the normal state on a host test and on any
// screen that has not wired a provider. The reply says so instead of printing
// a page of zeroes -- a zeroed battery reads as a flat one, and a power log
// with that in it is worse than one with a gap.
TEST(MapCommandConsole, StatsSaysUnavailableWithNoProvider) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  feedLine(console, out, "stats");
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), std::string("INFO stats=unavailable")), out.lines.end());
  EXPECT_EQ(out.lines.back(), "OK");
}

// A provider that refuses is the same case as no provider at all.
TEST(MapCommandConsole, StatsSaysUnavailableWhenTheProviderRefuses) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  state.setPowerStatsProvider([](MapPowerStats&) -> bool { return false; });

  feedLine(console, out, "stats");
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), std::string("INFO stats=unavailable")), out.lines.end());
}

TEST(MapCommandConsole, StatsReportsTheProvidersNumbers) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  state.setPowerStatsProvider([](MapPowerStats& s) -> bool {
    s.batteryMv = 3812;
    s.batteryPct = 64;
    s.uptimeS = 7200;
    s.cpuMhz = 160;
    s.fullClockMs = 7'000'000;
    s.throttledMs = 200'000;
    s.loopIters = 640'000;
    s.loopBusyMs = 90'000;
    s.loopMaxMs = 1900;
    s.refreshFull = 3;
    s.refreshHalf = 11;
    s.refreshFast = 42;
    s.refreshWindow = 128;
    s.panelBusyMs = 61'000;
    s.freeHeap = 51'200;
    s.minFreeHeap = 40'960;
    s.rssiDbm = -57;
    return true;
  });

  feedLine(console, out, "stats");
  const auto has = [&out](const char* line) {
    return std::find(out.lines.begin(), out.lines.end(), std::string(line)) != out.lines.end();
  };
  EXPECT_TRUE(has("INFO batt_mv=3812"));
  EXPECT_TRUE(has("INFO batt_pct=64"));
  EXPECT_TRUE(has("INFO uptime_s=7200"));
  EXPECT_TRUE(has("INFO cpu_mhz=160"));
  EXPECT_TRUE(has("INFO full_clock_ms=7000000"));
  EXPECT_TRUE(has("INFO throttled_ms=200000"));
  EXPECT_TRUE(has("INFO loops=640000"));
  EXPECT_TRUE(has("INFO loop_busy_ms=90000"));
  EXPECT_TRUE(has("INFO loop_max_ms=1900"));
  EXPECT_TRUE(has("INFO ref_full=3"));
  EXPECT_TRUE(has("INFO ref_half=11"));
  EXPECT_TRUE(has("INFO ref_fast=42"));
  EXPECT_TRUE(has("INFO ref_window=128"));
  EXPECT_TRUE(has("INFO panel_busy_ms=61000"));
  EXPECT_TRUE(has("INFO heap=51200"));
  EXPECT_TRUE(has("INFO min_heap=40960"));
  EXPECT_TRUE(has("INFO rssi_dbm=-57"));
  EXPECT_EQ(out.lines.back(), "OK");
}

// 0 dBm is not a reading any BLE link produces, so it is the sentinel for
// "nothing connected" -- and an omitted line is how every other optional value
// in this console reports that (the MTU in `info`).
TEST(MapCommandConsole, StatsOmitsRssiWhenNothingIsConnected) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  state.setPowerStatsProvider([](MapPowerStats& s) -> bool {
    s.rssiDbm = 0;
    return true;
  });

  feedLine(console, out, "stats");
  EXPECT_EQ(std::count_if(out.lines.begin(), out.lines.end(),
                          [](const std::string& l) { return l.rfind("INFO rssi_dbm=", 0) == 0; }),
            0);
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

// ---------------------------------------------------------------------------
// The freshness exchange: have / stale / checked (docs/tile-freshness.md)
// ---------------------------------------------------------------------------

namespace {

class RecordingStaleObserver final : public IMapStaleObserver {
 public:
  struct Tile {
    uint8_t z;
    uint32_t col;
    uint32_t row;
  };
  std::vector<Tile> stale;
  bool finished = false;
  bool known = false;
  uint16_t count = 0;

  void onTileStale(uint8_t z, uint32_t col, uint32_t row) override { stale.push_back(Tile{z, col, row}); }
  void onCheckFinished(bool k, uint16_t n) override {
    finished = true;
    known = k;
    count = n;
  }
};

struct HeldTileSeed {
  uint8_t z;
  uint32_t col;
  uint32_t row;
  uint32_t contentId;
};

void seedHeld(HeldTilesStore& store, std::initializer_list<HeldTileSeed> entries) {
  for (const auto& e : entries) store.record(e.z, e.col, e.row, e.contentId);
}

}  // namespace

// Re-recording the same bytes must not undo a check, or the list never drains
// -- every viewport reset walks tiles the last one already had.
TEST(HeldTilesStore, ReRecordingTheSameContentIdKeepsItSettled) {
  HeldTilesStore store;
  store.record(13, 4482, 2839, 0xABB60454u);
  store.beginListing();
  store.markAskedChecked();
  ASSERT_EQ(store.pendingCount(), 0u);

  store.record(13, 4482, 2839, 0xABB60454u);
  EXPECT_EQ(store.pendingCount(), 0u);
  EXPECT_EQ(store.size(), 1u);
}

// A moved content_id is what a replaced tile looks like from here. It re-arms,
// which is how the loop closes after a stale fetch: the new copy is checked
// once, agrees, and settles.
TEST(HeldTilesStore, AMovedContentIdRearmsTheEntry) {
  HeldTilesStore store;
  store.record(13, 4482, 2839, 0xABB60454u);
  store.beginListing();
  store.markAskedChecked();
  ASSERT_EQ(store.pendingCount(), 0u);

  store.record(13, 4482, 2839, 0xC0F79EE0u);
  EXPECT_EQ(store.pendingCount(), 1u);
  EXPECT_EQ(store.size(), 1u);
  EXPECT_EQ(store.at(0).contentId, 0xC0F79EE0u);
}

// A tile that did not open has no content to vouch for. Recording it at
// content 0 would have the phone report it stale forever.
TEST(HeldTilesStore, ContentIdZeroIsNotRecorded) {
  HeldTilesStore store;
  store.record(13, 4482, 2839, 0u);
  EXPECT_EQ(store.size(), 0u);
  EXPECT_FALSE(store.valid());
}

// Full store: a settled entry is the cheapest thing to lose, because the map
// re-records it the next time it draws that ground.
TEST(HeldTilesStore, EvictionPrefersASettledEntry) {
  HeldTilesStore store;
  for (uint32_t i = 0; i < HeldTilesStore::kMaxEntries; ++i) store.record(13, 4482, i, 0x1000u + i);
  ASSERT_EQ(store.size(), HeldTilesStore::kMaxEntries);

  // Settle the first one only, then overflow by one.
  store.beginListing();
  store.markAskedChecked();
  store.record(13, 4482, 7, 0x2000u);  // moves one entry back to pending
  store.record(13, 9999, 1, 0x3000u);  // the overflow

  EXPECT_EQ(store.size(), HeldTilesStore::kMaxEntries);
  // The newcomer is on the list and row 0 -- a settled entry -- is the one that
  // made room for it.
  bool hasNewcomer = false;
  bool hasRowZero = false;
  for (size_t i = 0; i < store.size(); ++i) {
    if (store.at(i).col == 9999) hasNewcomer = true;
    if (store.at(i).col == 4482 && store.at(i).row == 0) hasRowZero = true;
  }
  EXPECT_TRUE(hasNewcomer);
  EXPECT_FALSE(hasRowZero);
}

// Nothing settled and the store is full: the oldest goes, because the tiles
// worth checking are the ones near where the rider now is.
TEST(HeldTilesStore, AFullUnsettledStoreDropsTheOldest) {
  HeldTilesStore store;
  for (uint32_t i = 0; i < HeldTilesStore::kMaxEntries; ++i) store.record(13, 4482, i, 0x1000u + i);
  store.record(13, 9999, 1, 0x3000u);

  EXPECT_EQ(store.size(), HeldTilesStore::kMaxEntries);
  EXPECT_EQ(store.at(0).row, 1u);
  EXPECT_EQ(store.at(store.size() - 1).col, 9999u);
}

// One listing is bounded by the console's per-poll() block budget, not by the
// store. Without this a full store emits ~10 indications back to back, each
// waiting on the peer's confirm, and the buttons die for the duration.
TEST(HeldTilesStore, AListingIsCappedAndTheRestKeeps) {
  HeldTilesStore store;
  for (uint32_t i = 0; i < HeldTilesStore::kMaxEntries; ++i) store.record(13, 4482, i, 0x1000u + i);

  EXPECT_EQ(store.beginListing(), HeldTilesStore::kMaxPerListing);
  store.markAskedChecked();
  EXPECT_EQ(store.pendingCount(), HeldTilesStore::kMaxEntries - HeldTilesStore::kMaxPerListing);

  // Rounds strictly shrink the backlog, so a caller that re-asks terminates.
  size_t rounds = 0;
  while (store.pendingCount() > 0) {
    const size_t asked = store.beginListing();
    ASSERT_GT(asked, 0u);
    store.markAskedChecked();
    ASSERT_LT(++rounds, 100u);
  }
  EXPECT_EQ(store.pendingCount(), 0u);
}

// `have` is what the phone reads to know which tiles to look up in the CDN's
// index. Lowercase hex with no 0x, the same way mapbuilder prints a content_id.
TEST(MapCommandConsole, HaveReportsTheHeldContentIds) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  HeldTilesStore held;
  seedHeld(held, {{13, 4482, 2839, 0xABB60454u}, {13, 4482, 2840, 0x00000001u}});
  state.setHeldTilesStore(&held);

  feedLine(console, out, "have");
  ASSERT_EQ(out.lines.size(), 4u);
  EXPECT_EQ(out.lines[0], "INFO have_total=2");
  EXPECT_EQ(out.lines[1], "INFO have_13_4482_2839=abb60454");
  EXPECT_EQ(out.lines[2], "INFO have_13_4482_2840=00000001");
  EXPECT_EQ(out.lines[3], "OK");
}

// The drain. A tile the phone has already answered about is not listed again,
// which is what lets Live mode work a backlog down instead of re-sending the
// same screenful every ten minutes.
TEST(MapCommandConsole, HaveDropsTilesAlreadyChecked) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  HeldTilesStore held;
  seedHeld(held, {{13, 4482, 2839, 0xABB60454u}, {13, 4482, 2840, 0x00000001u}});
  state.setHeldTilesStore(&held);

  feedLine(console, out, "have");
  out.lines.clear();
  feedLine(console, out, "checked 0");
  out.lines.clear();

  // Everything settled, so the listing is empty -- but the store is still
  // valid, so this is "nothing left to check", not "cannot answer".
  feedLine(console, out, "have");
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "INFO have_total=0");
  EXPECT_EQ(out.lines[1], "OK");
}

// `checked unknown` means the phone could not read the index. Settling the
// listing on it would bury exactly the bug the distinction exists to catch.
TEST(MapCommandConsole, CheckedUnknownLeavesTilesPending) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  HeldTilesStore held;
  seedHeld(held, {{13, 4482, 2839, 0xABB60454u}});
  state.setHeldTilesStore(&held);

  feedLine(console, out, "have");
  out.lines.clear();
  feedLine(console, out, "checked unknown");
  out.lines.clear();

  feedLine(console, out, "have");
  ASSERT_EQ(out.lines.size(), 3u);
  EXPECT_EQ(out.lines[0], "INFO have_total=1");
  EXPECT_EQ(out.lines[1], "INFO have_13_4482_2839=abb60454");
}

// A tile recorded while a listing is on the wire was never sent, so the answer
// that comes back must not settle it.
TEST(MapCommandConsole, CheckedOnlySettlesWhatWasListed) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  HeldTilesStore held;
  seedHeld(held, {{13, 4482, 2839, 0xABB60454u}});
  state.setHeldTilesStore(&held);

  feedLine(console, out, "have");
  out.lines.clear();
  // The map drew another tile between the listing and the phone's answer.
  held.record(13, 4482, 2840, 0x00000001u);
  feedLine(console, out, "checked 0");
  out.lines.clear();

  feedLine(console, out, "have");
  ASSERT_EQ(out.lines.size(), 3u);
  EXPECT_EQ(out.lines[0], "INFO have_total=1");
  EXPECT_EQ(out.lines[1], "INFO have_13_4482_2840=00000001");
}

// "Cannot answer" must never read as "everything is current" -- the same
// distinction `tiles=none` and `missing=unavailable` already make.
TEST(MapCommandConsole, HaveSaysNoneBeforeAnythingIsDrawn) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  feedLine(console, out, "have");
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "INFO have=none");
  EXPECT_EQ(out.lines[1], "OK");
}

namespace {

// Records what `fake` asked for, and reports back fewer than asked so the
// reply's honesty can be checked.
struct RecordingFakeSink : IMapFakeSink {
  bool called = false;
  uint16_t askedMissing = 0;
  uint16_t askedHeld = 0;
  uint16_t giveMissing = 0;
  uint16_t giveHeld = 0;

  void seedFakeTiles(uint16_t missing, uint16_t held, uint16_t& seededMissing, uint16_t& seededHeld) override {
    called = true;
    askedMissing = missing;
    askedHeld = held;
    seededMissing = giveMissing;
    seededHeld = giveHeld;
  }
};

}  // namespace

TEST(MapCommandParser, FakeTakesTwoCounts) {
  const MapCommand cmd = parseMapCommand("fake 6 20");
  ASSERT_EQ(cmd.type, MapCommandType::Fake);
  EXPECT_EQ(cmd.fakeMissing, 6u);
  EXPECT_EQ(cmd.fakeHeld, 20u);
}

TEST(MapCommandParser, FakeRejectsWrongArityAndBadNumbers) {
  EXPECT_EQ(parseMapCommand("fake 6").error, MapCommandError::BadArity);
  EXPECT_EQ(parseMapCommand("fake 6 20 3").error, MapCommandError::BadArity);
  EXPECT_EQ(parseMapCommand("fake six 20").error, MapCommandError::BadNumber);
}

// The reply states what actually landed, not what was asked for: the receiving
// stores have their own caps, and a console used to catch lies must not tell
// one.
TEST(MapCommandConsole, FakeReportsWhatTheSinkTook) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  RecordingFakeSink sink;
  sink.giveMissing = 4;
  sink.giveHeld = 12;
  state.setFakeSink(&sink);

  feedLine(console, out, "fake 6 20");
  EXPECT_TRUE(sink.called);
  EXPECT_EQ(sink.askedMissing, 6u);
  EXPECT_EQ(sink.askedHeld, 20u);
  ASSERT_EQ(out.lines.size(), 3u);
  EXPECT_EQ(out.lines[0], "INFO fake_missing=4");
  EXPECT_EQ(out.lines[1], "INFO fake_held=12");
  EXPECT_EQ(out.lines[2], "OK");
}

// No sink means the screen cannot seed, which must not read as "seeded zero" --
// same distinction `missing=unavailable` makes.
TEST(MapCommandConsole, FakeSaysUnavailableWithNoSink) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  feedLine(console, out, "fake 6 20");
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "INFO fake=unavailable");
  EXPECT_EQ(out.lines[1], "OK");
}

TEST(MapCommandConsole, StaleAndCheckedReachTheObserver) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  RecordingStaleObserver observer;
  state.setStaleObserver(&observer);

  feedLine(console, out, "stale 13 4482 2839");
  feedLine(console, out, "checked 1");

  ASSERT_EQ(observer.stale.size(), 1u);
  EXPECT_EQ(observer.stale[0].z, 13);
  EXPECT_EQ(observer.stale[0].col, 4482u);
  EXPECT_EQ(observer.stale[0].row, 2839u);
  EXPECT_TRUE(observer.finished);
  EXPECT_TRUE(observer.known);
  EXPECT_EQ(observer.count, 1);
}

// The whole point of the word. A phone with no signal cannot tell a current
// tile from a stale one, and `checked unknown` has to arrive as "I do not
// know" rather than as zero.
TEST(MapCommandConsole, CheckedUnknownIsNotZero) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  RecordingStaleObserver observer;
  state.setStaleObserver(&observer);

  feedLine(console, out, "checked unknown");
  EXPECT_TRUE(observer.finished);
  EXPECT_FALSE(observer.known);
}

// A stale tile is on the card and opens fine, so reporting it as `missing`
// would be a lie -- and the supplier fetches the two differently.
TEST(MapCommandConsole, TilesFlagsAStaleTileApartFromAMissingOne) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  StaleTilesList stale;
  stale.add(13, 4482, 2840);
  state.setStaleTiles(&stale);

  MapTileRangeSnapshot range;
  range.valid = true;
  range.z = 13;
  range.col0 = 4482;
  range.col1 = 4482;
  range.row0 = 2839;
  range.row1 = 2841;
  range.unavailableMask = 1u << 0;  // the first tile is genuinely absent
  state.setTileRange(range);

  feedLine(console, out, "tiles");
  ASSERT_EQ(out.lines.size(), 4u);
  EXPECT_EQ(out.lines[0], "INFO tile_13_4482_2839=missing");
  EXPECT_EQ(out.lines[1], "INFO tile_13_4482_2840=stale");
  EXPECT_EQ(out.lines[2], "INFO tile_13_4482_2841=ok");
}

TEST(MapCommandParser, RejectsMalformedFreshnessCommands) {
  EXPECT_EQ(parseMapCommand("stale 13 4482").error, MapCommandError::BadArity);
  EXPECT_EQ(parseMapCommand("stale 13 4482 x").error, MapCommandError::BadNumber);
  EXPECT_EQ(parseMapCommand("stale 999 1 1").error, MapCommandError::OutOfRange);
  EXPECT_EQ(parseMapCommand("checked").error, MapCommandError::BadArity);
  EXPECT_EQ(parseMapCommand("checked maybe").error, MapCommandError::BadNumber);
  EXPECT_EQ(parseMapCommand("have 1").error, MapCommandError::BadArity);
}

// ---------------------------------------------------------------------------
// StaleTilesList: the two traps the design exists to avoid
// ---------------------------------------------------------------------------

TEST(StaleTilesList, AddsOnceAndForgetsOnArrival) {
  StaleTilesList list;
  EXPECT_TRUE(list.add(13, 1, 1));
  EXPECT_FALSE(list.add(13, 1, 1));  // duplicate
  EXPECT_EQ(list.count(), 1u);
  list.onArrived(13, 1, 1);
  EXPECT_TRUE(list.empty());
}

// The ping-pong guard. A tile reported stale again after it was fetched for
// staleness was not fixed by that fetch -- a cache served the old copy, or the
// index is ahead of the tiles. Asking once more would ask forever.
TEST(StaleTilesList, GivesUpOnATileAFetchDidNotFix) {
  StaleTilesList list;
  ASSERT_TRUE(list.add(13, 1, 1));
  list.onArrived(13, 1, 1);

  EXPECT_FALSE(list.add(13, 1, 1));
  EXPECT_TRUE(list.hasGivenUp(13, 1, 1));
  EXPECT_TRUE(list.empty());

  // And it stays given up on, whatever else happens.
  list.clear();
  EXPECT_FALSE(list.add(13, 1, 1));
}

// clear() drops the round's findings and keeps the memory that stops the loop.
TEST(StaleTilesList, ClearKeepsWhatTheGuardNeeds) {
  StaleTilesList list;
  list.add(13, 1, 1);
  list.giveUp(13, 2, 2);
  list.clear();
  EXPECT_TRUE(list.empty());
  EXPECT_TRUE(list.hasGivenUp(13, 2, 2));
}

TEST(StaleTilesList, StaysBoundedAndAllocatesNothing) {
  StaleTilesList list;
  for (uint32_t i = 0; i < StaleTilesList::kMaxEntries + 10; ++i) list.add(13, i, 0);
  EXPECT_EQ(list.count(), StaleTilesList::kMaxEntries);
  // The oldest are kept: what is on screen now was added first this round.
  EXPECT_TRUE(list.contains(13, 0, 0));
  EXPECT_FALSE(list.contains(13, StaleTilesList::kMaxEntries + 5, 0));
}

// ---------------------------------------------------------------- push

namespace {

struct RecordingPushObserver final : IMapPushObserver {
  std::vector<uint16_t> announced;
  void onPushAnnounced(uint16_t count) override {
    announced.push_back(count);
    g_events.emplace_back("push:" + std::to_string(count));
  }
};

}  // namespace

TEST(MapCommandParser, PushTakesOneCount) {
  const MapCommand cmd = parseMapCommand("push 55");
  ASSERT_EQ(cmd.type, MapCommandType::Push);
  EXPECT_EQ(cmd.pushCount, 55u);
}

// Zero is a phone-side bug, not a no-op: answering OK to it would leave a
// screen showing a run that can never move.
TEST(MapCommandParser, PushRejectsZeroWrongArityAndGarbage) {
  EXPECT_EQ(errorOf("push 0"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("push"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("push 12 13"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("push fifty"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("push -3"), MapCommandError::BadNumber);
}

// The cap is an order of magnitude past any real batch (a 40 km box around
// Barcelona is 77 tiles), so it exists to keep a garbage number off the panel.
TEST(MapCommandParser, PushIsBounded) {
  EXPECT_EQ(parseMapCommand("push 4096").type, MapCommandType::Push);
  EXPECT_EQ(errorOf("push 4097"), MapCommandError::OutOfRange);
}

// The observer is called before the terminating OK: the phone sends its first
// begin frame off the back of that OK, so the screen has to have the batch on
// its books by then.
TEST(MapCommandConsole, PushReachesTheObserverBeforeTheOk) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;
  RecordingPushObserver observer;
  state.setPushObserver(&observer);

  g_events.clear();
  feedLine(console, out, "push 55");

  ASSERT_EQ(observer.announced.size(), 1u);
  EXPECT_EQ(observer.announced[0], 55u);
  ASSERT_EQ(out.lines.size(), 1u);
  EXPECT_EQ(out.lines[0], "OK");
  ASSERT_EQ(g_events.size(), 2u);
  EXPECT_EQ(g_events[0], "push:55");
  EXPECT_EQ(g_events[1], "reply:OK");
}

// A screen that cannot count a batch must not answer like one that counted it.
// The map screen is exactly this case, and the phone is about to spend tens of
// minutes of radio on the answer.
TEST(MapCommandConsole, PushSaysUnavailableWithNoObserver) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  feedLine(console, out, "push 55");
  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_EQ(out.lines[0], "INFO push=unavailable");
  EXPECT_EQ(out.lines[1], "OK");
}

// Which screen is up. Omitted when nothing set it, so a phone reads a missing
// line as "this firmware cannot say" -- which an older build genuinely is --
// and never as either screen.
TEST(MapCommandConsole, InfoReportsTheScreenOnlyWhenSet) {
  MapConsoleState state;
  MapCommandConsole console(state);
  CollectingWriter out;

  feedLine(console, out, "info");
  EXPECT_EQ(std::count_if(out.lines.begin(), out.lines.end(),
                          [](const std::string& l) { return l.rfind("INFO screen=", 0) == 0; }),
            0);

  out.lines.clear();
  state.setScreenName("sync");
  feedLine(console, out, "info");
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), "INFO screen=sync"), out.lines.end());

  out.lines.clear();
  state.setScreenName("map");
  feedLine(console, out, "info");
  EXPECT_NE(std::find(out.lines.begin(), out.lines.end(), "INFO screen=map"), out.lines.end());
}
