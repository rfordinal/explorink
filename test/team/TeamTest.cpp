// The group-markers layer's pure half: the roster and its JSON, the acronym
// rules, the black box line, the age rules, and the `team` grammar.
//
// Everything here runs with no card and no Arduino, which is the point of
// keeping TeamRoster/TeamRecord/TeamFix free of both (../../src/activities/map/
// TeamFiles.h is the device half and is not covered here).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "MapCommandConsole.h"
#include "MapCommandParser.h"
#include "TeamFix.h"
#include "TeamMembers.h"
#include "TeamRecord.h"
#include "TeamRoster.h"
#include "TeamStore.h"

namespace {

TEST(TeamAcronym, TwoOrThreeAlphanumerics) {
  EXPECT_TRUE(isValidTeamAcr("RF"));
  EXPECT_TRUE(isValidTeamAcr("rf"));
  EXPECT_TRUE(isValidTeamAcr("M3K"));
  EXPECT_FALSE(isValidTeamAcr("R"));
  EXPECT_FALSE(isValidTeamAcr("ROMA"));
  EXPECT_FALSE(isValidTeamAcr("R F"));
  EXPECT_FALSE(isValidTeamAcr("R,F"));
  EXPECT_FALSE(isValidTeamAcr(""));
}

TEST(TeamAcronym, NormalisesToUpper) {
  char out[kTeamAcrBytes] = {};
  ASSERT_TRUE(teamNormaliseAcr("rf", out));
  EXPECT_STREQ(out, "RF");
  EXPECT_FALSE(teamNormaliseAcr("toolong", out));
}

TEST(TeamId, RefusesSeparatorsAndSpaces) {
  EXPECT_TRUE(isValidTeamId("a4c1380c"));
  EXPECT_TRUE(isValidTeamId("!a4c1380c"));
  EXPECT_FALSE(isValidTeamId("a4c 1380c"));
  EXPECT_FALSE(isValidTeamId("a4c,1380c"));
  EXPECT_FALSE(isValidTeamId(""));
}

TEST(TeamRosterRules, OneAcronymPerId) {
  TeamRoster roster;
  ASSERT_TRUE(roster.set("id-1", "RF", "Roman", true));
  // Same id, new acronym: the member moved, not a second member.
  ASSERT_TRUE(roster.set("id-1", "RO", "Roman", true));
  EXPECT_EQ(roster.count(), 1u);
  EXPECT_LT(roster.findAcr("ro"), TeamRoster::kSlotCount);

  // A crossing pair is refused: acronym on one radio, the same acronym on
  // another, is two markers a rider reads as one person.
  ASSERT_TRUE(roster.set("id-2", "MK", "Marek", true));
  EXPECT_FALSE(roster.set("id-2", "RO", "Marek", true));
}

TEST(TeamRosterRules, MeIsReserved) {
  TeamRoster roster;
  EXPECT_FALSE(roster.set("id-1", "ME", "", true));
  EXPECT_FALSE(roster.set("id-1", "me", "", true));
}

TEST(TeamRosterRules, FullRosterRefusesTheNextMember) {
  TeamRoster roster;
  for (size_t i = 0; i < kTeamMaxMembers; ++i) {
    const std::string id = "id-" + std::to_string(i);
    char acr[4] = {'A', static_cast<char>('0' + i % 10), static_cast<char>('a' + i), '\0'};
    ASSERT_TRUE(roster.set(id, acr, "", true)) << id;
  }
  EXPECT_EQ(roster.count(), kTeamMaxMembers);
  EXPECT_FALSE(roster.set("id-extra", "ZZ", "", true));
}

TEST(TeamRosterJson, RoundTrips) {
  TeamRoster roster;
  ASSERT_TRUE(roster.set("a4c1380c", "RF", "Roman", true));
  ASSERT_TRUE(roster.set("b7e2", "MK", "Marek", false));

  char buf[TeamRoster::kJsonBytes];
  const size_t len = roster.writeJson(buf, sizeof(buf));
  ASSERT_GT(len, 0u);

  TeamRoster loaded;
  size_t skipped = 0;
  ASSERT_TRUE(loaded.parseJson(std::string_view(buf, len), skipped));
  EXPECT_EQ(skipped, 0u);
  ASSERT_EQ(loaded.count(), 2u);
  const size_t slot = loaded.findAcr("MK");
  ASSERT_LT(slot, TeamRoster::kSlotCount);
  EXPECT_STREQ(loaded.at(slot).id, "b7e2");
  EXPECT_STREQ(loaded.at(slot).name, "Marek");
  EXPECT_FALSE(loaded.at(slot).enabled);
}

TEST(TeamRosterJson, UnknownVersionIsRefusedWhole) {
  TeamRoster roster;
  size_t skipped = 0;
  EXPECT_FALSE(roster.parseJson(R"({"v":2,"members":[{"id":"x","acr":"RF"}]})", skipped));
  EXPECT_EQ(roster.count(), 0u);
  // No version at all is the same answer: a document this build cannot place is
  // not an allowlist it may act on.
  EXPECT_FALSE(roster.parseJson(R"({"members":[{"id":"x","acr":"RF"}]})", skipped));
}

TEST(TeamRosterJson, ABadRowIsSkippedAndCounted) {
  TeamRoster roster;
  size_t skipped = 0;
  ASSERT_TRUE(roster.parseJson(
      R"({"v":1,"members":[{"id":"x","acr":"RF"},{"id":"y","acr":"TOOLONG"},{"id":"z","acr":"MK"}]})", skipped));
  EXPECT_EQ(roster.count(), 2u);
  EXPECT_EQ(skipped, 1u);
}

TEST(TeamRecordCsv, RoundTrips) {
  TeamRecord rec;
  rec.utc = 1789430400u;
  rec.uptimeMs = 81234u;
  snprintf(rec.who, sizeof(rec.who), "RF");
  rec.latE7 = 481486000;
  rec.lonE7 = 171077000;
  rec.heading = 4;
  rec.hasHeading = true;
  rec.speedKmh = 62;
  rec.hasSpeed = true;
  rec.source = TeamFixSource::Lora;

  char line[kTeamLineMax + 1];
  const size_t len = encodeTeamRecord(rec, line, sizeof(line));
  ASSERT_GT(len, 0u);
  EXPECT_STREQ(line, "1789430400,81234,RF,48.1486000,17.1077000,4,62,lora");

  TeamRecord back;
  ASSERT_TRUE(decodeTeamRecord(std::string_view(line, len), back));
  EXPECT_EQ(back.utc, rec.utc);
  EXPECT_EQ(back.latE7, rec.latE7);
  EXPECT_EQ(back.lonE7, rec.lonE7);
  EXPECT_EQ(back.heading, 4);
  EXPECT_TRUE(back.hasHeading);
  EXPECT_EQ(back.speedKmh, 62);
  EXPECT_EQ(back.source, TeamFixSource::Lora);
  EXPECT_STREQ(back.who, "RF");
}

TEST(TeamRecordCsv, EmptyHeadingAndSpeedStayEmpty) {
  TeamRecord rec;
  snprintf(rec.who, sizeof(rec.who), "%s", kTeamSelfWho);
  rec.latE7 = -481486000;
  rec.lonE7 = 5;
  rec.source = TeamFixSource::Gnss;

  char line[kTeamLineMax + 1];
  ASSERT_GT(encodeTeamRecord(rec, line, sizeof(line)), 0u);
  EXPECT_STREQ(line, "0,0,ME,-48.1486000,0.0000005,,,gnss");

  TeamRecord back;
  ASSERT_TRUE(decodeTeamRecord(line, back));
  EXPECT_FALSE(back.hasHeading);
  EXPECT_FALSE(back.hasSpeed);
  EXPECT_EQ(back.latE7, rec.latE7);
}

TEST(TeamRecordCsv, MalformedRowsAreRefused) {
  TeamRecord out;
  EXPECT_FALSE(decodeTeamRecord(kTeamCsvHeader, out));          // the header line
  EXPECT_FALSE(decodeTeamRecord("1,2,RF,48.1,17.1,,,", out));   // no source word
  EXPECT_FALSE(decodeTeamRecord("1,2,RF,48.1,17.1,,,lora,x", out));  // one field too many
  EXPECT_FALSE(decodeTeamRecord("1,2,TOOLONG,48.1,17.1,,,lora", out));
  EXPECT_FALSE(decodeTeamRecord("1,2,RF,48.1,17.1,16,,lora", out));  // heading out of range
}

TEST(TeamDayFile, NamesTheDayAndSaysWhenItCannot) {
  char name[20] = {};
  ASSERT_TRUE(teamDayFileName(1789430400u, name, sizeof(name)));  // 2026-09-15 00:00 UTC
  EXPECT_STREQ(name, "bb-20260915.csv");
  ASSERT_TRUE(teamDayFileName(1789430400u + 86399u, name, sizeof(name)));  // same day, one second before midnight
  EXPECT_STREQ(name, "bb-20260915.csv");
  ASSERT_TRUE(teamDayFileName(1789430400u + 86400u, name, sizeof(name)));
  EXPECT_STREQ(name, "bb-20260916.csv");
  ASSERT_TRUE(teamDayFileName(0, name, sizeof(name)));
  EXPECT_STREQ(name, "bb-noclock.csv");
}

TEST(TeamAge, PrefersTheSendersClockAndFallsBackToReceipt) {
  TeamFix fix;
  fix.present = true;
  fix.utc = 1000;
  fix.recvUptimeMs = 5000;

  const TeamFixAge byUtc = teamFixAge(fix, 1300, 60000);
  EXPECT_TRUE(byUtc.known);
  EXPECT_EQ(byUtc.seconds, 300u);

  // No clock on either side: our own receipt uptime dates it.
  fix.utc = 0;
  const TeamFixAge byUptime = teamFixAge(fix, 0, 65000);
  EXPECT_TRUE(byUptime.known);
  EXPECT_EQ(byUptime.seconds, 60u);
}

TEST(TeamAge, AReplayedFixWithNoClockHasNoKnownAge) {
  TeamFix fix;
  fix.present = true;
  fix.fromLog = true;
  fix.recvUptimeMs = 5000;  // a previous run's clock, meaningless now
  const TeamFixAge age = teamFixAge(fix, 0, 600000);
  EXPECT_FALSE(age.known);
  // And it is still drawn -- as stale, never hidden. A last known position with
  // no date is the best lead there is.
  EXPECT_EQ(teamFixVisibility(age, 300, 1800), TeamVisibility::Stale);
}

TEST(TeamAge, FreshStaleHidden) {
  TeamFixAge age;
  age.known = true;
  age.seconds = 10;
  EXPECT_EQ(teamFixVisibility(age, 300, 1800), TeamVisibility::Fresh);
  age.seconds = 400;
  EXPECT_EQ(teamFixVisibility(age, 300, 1800), TeamVisibility::Stale);
  age.seconds = 3600;
  EXPECT_EQ(teamFixVisibility(age, 300, 1800), TeamVisibility::Hidden);
  // Hiding off: an old position keeps being drawn as stale.
  EXPECT_EQ(teamFixVisibility(age, 300, 0), TeamVisibility::Stale);
}

TEST(TeamGrammar, PosTakesEitherHandleAndAKeywordTail) {
  const MapCommand byAcr = parseMapCommand("team pos RF 48.1486 17.1077 utc 1789430400 heading 4 speed 62 src lora");
  ASSERT_EQ(byAcr.type, MapCommandType::Team);
  EXPECT_EQ(byAcr.teamVerb, MapTeamVerb::Pos);
  EXPECT_STREQ(byAcr.teamAcr, "RF");
  EXPECT_EQ(byAcr.latE7, 481486000);
  EXPECT_EQ(byAcr.teamUtc, 1789430400u);
  EXPECT_EQ(byAcr.heading, 4);
  EXPECT_TRUE(byAcr.hasSpeed);
  EXPECT_EQ(byAcr.teamSource, TeamFixSource::Lora);

  const MapCommand byId = parseMapCommand("team pos !a4c1380c 48.1 17.1");
  ASSERT_EQ(byId.type, MapCommandType::Team);
  EXPECT_STREQ(byId.teamId, "!a4c1380c");
  EXPECT_STREQ(byId.teamAcr, "");
}

TEST(TeamGrammar, ABareTailIsRefused) {
  // Unlike `pos`, where a bare number fills heading then speed: after a
  // coordinate here a bare number is as likely a unix time as a heading.
  const MapCommand cmd = parseMapCommand("team pos RF 48.1 17.1 4");
  EXPECT_EQ(cmd.type, MapCommandType::Error);
  EXPECT_EQ(cmd.error, MapCommandError::BadArity);
}

TEST(TeamGrammar, AddTakesANameWithSpaces) {
  const MapCommand cmd = parseMapCommand("team add MK !b7e2 Marek Novak");
  ASSERT_EQ(cmd.type, MapCommandType::Team);
  EXPECT_EQ(cmd.teamVerb, MapTeamVerb::Add);
  EXPECT_STREQ(cmd.teamAcr, "MK");
  EXPECT_STREQ(cmd.teamId, "!b7e2");
  EXPECT_STREQ(cmd.teamName, "Marek Novak");
}

TEST(TeamGrammar, BadMemberIsItsOwnError) {
  EXPECT_EQ(parseMapCommand("team add TOOLONG id").error, MapCommandError::BadMember);
  // A single letter is not an acronym but is a legal id, so it resolves as one
  // and the roster -- not the grammar -- decides whether it is known.
  EXPECT_EQ(parseMapCommand("team pos R 48.1 17.1").type, MapCommandType::Team);
  EXPECT_EQ(parseMapCommand("team pos R,F 48.1 17.1").error, MapCommandError::BadMember);
  EXPECT_STREQ(mapCommandErrorText(MapCommandError::BadMember), "bad_member");
}

TEST(TeamGrammar, ListReloadAndLog) {
  EXPECT_EQ(parseMapCommand("team list").teamVerb, MapTeamVerb::List);
  EXPECT_EQ(parseMapCommand("team reload").teamVerb, MapTeamVerb::Reload);
  EXPECT_EQ(parseMapCommand("team log 8").teamLogOffset, 8);
  EXPECT_EQ(parseMapCommand("team list extra").type, MapCommandType::Error);
}

TEST(TeamLabel, FourShapes) {
  TeamFixAge fresh;
  fresh.known = true;
  fresh.seconds = 30;
  TeamFixAge old;
  old.known = true;
  old.seconds = 12 * 60;

  char buf[kTeamLabelBytes];
  // Nothing: close in, and current.
  EXPECT_EQ(teamMarkerLabel(false, 1200, false, fresh, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "");

  // Distance alone: the rung is wide enough that the eye cannot judge it.
  ASSERT_GT(teamMarkerLabel(true, 1200, false, fresh, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "1.2 km");

  // Age alone: close enough to see, old enough to doubt.
  ASSERT_GT(teamMarkerLabel(false, 1200, true, old, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "12m");

  // Both.
  ASSERT_GT(teamMarkerLabel(true, 1200, true, old, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "1.2 km/12m");
}

TEST(TeamLabel, HoursAndTheUnknownAge) {
  TeamFixAge hours;
  hours.known = true;
  hours.seconds = 3 * 3600 + 900;
  char buf[kTeamLabelBytes];
  ASSERT_GT(teamMarkerLabel(false, 0, true, hours, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "3h");

  // A replayed fix on a device with no clock: `?`, never a number nobody can
  // stand behind.
  const TeamFixAge unknown;
  ASSERT_GT(teamMarkerLabel(true, 450, true, unknown, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "450 m/?");
}

TEST(TeamLabel, NothingIsSaidAboutTheLastMinute) {
  TeamFixAge seconds;
  seconds.known = true;
  seconds.seconds = 40;
  char buf[kTeamLabelBytes];
  // Stale by a threshold of zero, but `0m` would read as information and carry
  // none -- the hollow head is the whole message there.
  EXPECT_EQ(teamMarkerLabel(false, 450, true, seconds, buf, sizeof(buf)), 0u);
  ASSERT_GT(teamMarkerLabel(true, 450, true, seconds, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "450 m");
}

TEST(TeamLabel, DistanceSteps) {
  TeamFixAge age;
  char buf[kTeamLabelBytes];
  ASSERT_GT(teamMarkerLabel(true, 940, false, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "940 m");
  ASSERT_GT(teamMarkerLabel(true, 9400, false, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "9.4 km");
  // Rounded, not truncated, once the decimal is gone.
  ASSERT_GT(teamMarkerLabel(true, 12600, false, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "13 km");
}

// --- the console over a fake source -----------------------------------------

class FakeTeamSource : public IMapTeamSource {
 public:
  Ingest teamPosition(std::string_view idOrAcr, const TeamFix& fix) override {
    lastHandle = std::string(idOrAcr);
    if (roster.find(idOrAcr) >= TeamRoster::kSlotCount) return Ingest::UnknownMember;
    const size_t slot = roster.find(idOrAcr);
    if (!roster.at(slot).enabled) return Ingest::Muted;
    TeamFix stored = fix;
    stored.present = true;
    store.set(slot, stored);
    return Ingest::Accepted;
  }
  Edit teamAdd(std::string_view acr, std::string_view id, std::string_view name) override {
    return roster.set(id, acr, name, true) ? Edit::Ok : Edit::Invalid;
  }
  Edit teamRemove(std::string_view idOrAcr) override {
    return roster.remove(idOrAcr) ? Edit::Ok : Edit::UnknownMember;
  }
  bool teamReload(size_t& skipped) override {
    skipped = 0;
    return true;
  }
  size_t teamCount() const override { return roster.count(); }
  TeamMember teamMemberAt(size_t index) const override { return roster.at(slotFor(index)); }
  TeamFix teamFixAt(size_t index) const override { return store.at(slotFor(index)); }
  uint32_t teamLogPage(uint32_t, uint32_t, ITeamLogVisitor&) override { return 0; }

  TeamRoster roster;
  TeamStore store;
  std::string lastHandle;

 private:
  size_t slotFor(size_t index) const {
    size_t seen = 0;
    for (size_t slot = 0; slot < TeamRoster::kSlotCount; ++slot) {
      if (!roster.at(slot).present) continue;
      if (seen == index) return slot;
      ++seen;
    }
    return 0;
  }
};

class RecordingWriter : public IMapReplyWriter {
 public:
  void reply(const char* line) override { lines.emplace_back(line); }
  bool has(const std::string& needle) const {
    for (const auto& line : lines) {
      if (line.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  std::vector<std::string> lines;
};

TEST(TeamConsole, WithoutASourceItSaysUnavailable) {
  MapConsoleState state;
  RecordingWriter out;
  state.execute(parseMapCommand("team list"), out);
  // Not "no members": a build that never wired the layer must not read as a
  // rider whose group is empty.
  EXPECT_TRUE(out.has("INFO team=unavailable"));
}

TEST(TeamConsole, AStrangerIsRefusedAndSaidOutLoud) {
  FakeTeamSource source;
  MapConsoleState state;
  state.setTeamSource(&source);
  RecordingWriter out;
  const bool redraw = state.execute(parseMapCommand("team pos ZZ 48.1 17.1"), out);
  EXPECT_FALSE(redraw);
  EXPECT_TRUE(out.has("ERR unknown_member"));
}

TEST(TeamConsole, AnAcceptedPositionRedraws) {
  FakeTeamSource source;
  ASSERT_TRUE(source.roster.set("id-1", "RF", "Roman", true));
  MapConsoleState state;
  state.setTeamSource(&source);
  RecordingWriter out;
  EXPECT_TRUE(state.execute(parseMapCommand("team pos RF 48.1486 17.1077"), out));
  EXPECT_TRUE(out.has("INFO team_pos=RF"));
  EXPECT_TRUE(out.has("OK"));
}

TEST(TeamConsole, ListSaysHasNotToldUsWithEmptyFieldsNotZero) {
  FakeTeamSource source;
  ASSERT_TRUE(source.roster.set("id-1", "RF", "Roman", true));
  MapConsoleState state;
  state.setTeamSource(&source);

  RecordingWriter before;
  state.execute(parseMapCommand("team list"), before);
  EXPECT_TRUE(before.has("INFO team_total=1"));
  // Empty coordinate fields, never 0,0 -- which is a place in the Atlantic.
  EXPECT_TRUE(before.has("INFO team_RF=id-1,on,,,,"));

  RecordingWriter after;
  state.execute(parseMapCommand("team pos RF 48.1486 17.1077 utc 1789430400 src ble"), after);
  RecordingWriter listed;
  state.execute(parseMapCommand("team list"), listed);
  EXPECT_TRUE(listed.has("INFO team_RF=id-1,on,48.1486000,17.1077000,1789430400,ble"));
}

}  // namespace
