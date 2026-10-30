// The group-markers layer's pure half: the roster and its JSON, the acronym
// rules, the black box line, the age rules, and the `group` grammar.
//
// Everything here runs with no card and no Arduino, which is the point of
// keeping GroupRoster/GroupRecord/GroupFix free of both (../../src/activities/map/
// GroupFiles.h is the device half and is not covered here).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "GroupFix.h"
#include "GroupMembers.h"
#include "GroupRecord.h"
#include "GroupRoster.h"
#include "GroupStore.h"
#include "MapCommandConsole.h"
#include "MapCommandParser.h"

namespace {

TEST(GroupAcronym, TwoOrThreeAlphanumerics) {
  EXPECT_TRUE(isValidGroupAcr("RF"));
  EXPECT_TRUE(isValidGroupAcr("rf"));
  EXPECT_TRUE(isValidGroupAcr("M3K"));
  EXPECT_FALSE(isValidGroupAcr("R"));
  EXPECT_FALSE(isValidGroupAcr("ROMA"));
  EXPECT_FALSE(isValidGroupAcr("R F"));
  EXPECT_FALSE(isValidGroupAcr("R,F"));
  EXPECT_FALSE(isValidGroupAcr(""));
}

TEST(GroupAcronym, NormalisesToUpper) {
  char out[kGroupAcrBytes] = {};
  ASSERT_TRUE(groupNormaliseAcr("rf", out));
  EXPECT_STREQ(out, "RF");
  EXPECT_FALSE(groupNormaliseAcr("toolong", out));
}

TEST(GroupId, RefusesSeparatorsAndSpaces) {
  EXPECT_TRUE(isValidGroupId("a4c1380c"));
  EXPECT_TRUE(isValidGroupId("!a4c1380c"));
  EXPECT_FALSE(isValidGroupId("a4c 1380c"));
  EXPECT_FALSE(isValidGroupId("a4c,1380c"));
  EXPECT_FALSE(isValidGroupId(""));
}

TEST(GroupRosterRules, OneAcronymPerId) {
  GroupRoster roster;
  ASSERT_TRUE(roster.set("id-1", "RF", "Roman", true));
  // Same id, new acronym: the member moved, not a second member.
  ASSERT_TRUE(roster.set("id-1", "RO", "Roman", true));
  EXPECT_EQ(roster.count(), 1u);
  EXPECT_LT(roster.findAcr("ro"), GroupRoster::kSlotCount);

  // A crossing pair is refused: acronym on one radio, the same acronym on
  // another, is two markers a rider reads as one person.
  ASSERT_TRUE(roster.set("id-2", "MK", "Marek", true));
  EXPECT_FALSE(roster.set("id-2", "RO", "Marek", true));
}

TEST(GroupRosterRules, OnePersonSeveralRadios) {
  GroupRoster roster;
  ASSERT_TRUE(roster.set("lora-8f3a", "RF", "Roman", true));
  // The same rider, relayed by a phone under a name that radio chose. The
  // roster is the only thing that can know these are one person.
  ASSERT_TRUE(roster.set("ble-7f", "RF", "Roman", true));
  EXPECT_EQ(roster.count(), 1u);
  const size_t slot = roster.findAcr("RF");
  ASSERT_LT(slot, GroupRoster::kSlotCount);
  EXPECT_EQ(roster.at(slot).idCount, 2u);
  EXPECT_EQ(roster.findId("lora-8f3a"), slot);
  EXPECT_EQ(roster.findId("ble-7f"), slot);

  // An identifier belongs to one person: claiming it for somebody who is already
  // in the roster would merge two riders into one marker.
  ASSERT_TRUE(roster.set("mk-1", "MK", "Marek", true));
  EXPECT_FALSE(roster.set("ble-7f", "MK", "Marek", true));

  // And the list is bounded.
  ASSERT_TRUE(roster.set("third", "RF", "Roman", true));
  EXPECT_FALSE(roster.set("fourth", "RF", "Roman", true));
}

TEST(GroupRosterJson, SeveralIdsRoundTrip) {
  GroupRoster roster;
  ASSERT_TRUE(roster.set("lora-8f3a", "RF", "Roman", true));
  ASSERT_TRUE(roster.set("ble-7f", "RF", "Roman", true));

  char buf[GroupRoster::kJsonBytes];
  const size_t len = roster.writeJson(buf, sizeof(buf));
  ASSERT_GT(len, 0u);

  GroupRoster loaded;
  size_t skipped = 0;
  ASSERT_TRUE(loaded.parseJson(std::string_view(buf, len), skipped));
  EXPECT_EQ(skipped, 0u);
  ASSERT_EQ(loaded.count(), 1u);
  EXPECT_EQ(loaded.findId("ble-7f"), loaded.findAcr("RF"));
}

TEST(GroupRosterJson, TheOldSingleIdSpellingStillLoads) {
  GroupRoster roster;
  size_t skipped = 0;
  ASSERT_TRUE(
      roster.parseJson(R"({"v":1,"members":[{"id":"a4c1380c","acr":"RF","name":"Roman","on":true}]})", skipped));
  EXPECT_EQ(skipped, 0u);
  ASSERT_EQ(roster.count(), 1u);
  EXPECT_LT(roster.findId("a4c1380c"), GroupRoster::kSlotCount);
}

TEST(GroupRosterRules, MeIsReserved) {
  GroupRoster roster;
  EXPECT_FALSE(roster.set("id-1", "ME", "", true));
  EXPECT_FALSE(roster.set("id-1", "me", "", true));
}

TEST(GroupRosterRules, FullRosterRefusesTheNextMember) {
  GroupRoster roster;
  for (size_t i = 0; i < kGroupMaxMembers; ++i) {
    const std::string id = "id-" + std::to_string(i);
    char acr[4] = {'A', static_cast<char>('0' + i % 10), static_cast<char>('a' + i), '\0'};
    ASSERT_TRUE(roster.set(id, acr, "", true)) << id;
  }
  EXPECT_EQ(roster.count(), kGroupMaxMembers);
  EXPECT_FALSE(roster.set("id-extra", "ZZ", "", true));
}

TEST(GroupRosterJson, RoundTrips) {
  GroupRoster roster;
  ASSERT_TRUE(roster.set("a4c1380c", "RF", "Roman", true));
  ASSERT_TRUE(roster.set("b7e2", "MK", "Marek", false));

  char buf[GroupRoster::kJsonBytes];
  const size_t len = roster.writeJson(buf, sizeof(buf));
  ASSERT_GT(len, 0u);

  GroupRoster loaded;
  size_t skipped = 0;
  ASSERT_TRUE(loaded.parseJson(std::string_view(buf, len), skipped));
  EXPECT_EQ(skipped, 0u);
  ASSERT_EQ(loaded.count(), 2u);
  const size_t slot = loaded.findAcr("MK");
  ASSERT_LT(slot, GroupRoster::kSlotCount);
  ASSERT_EQ(loaded.at(slot).idCount, 1u);
  EXPECT_STREQ(loaded.at(slot).ids[0], "b7e2");
  EXPECT_STREQ(loaded.at(slot).name, "Marek");
  EXPECT_FALSE(loaded.at(slot).enabled);
}

TEST(GroupRosterJson, UnknownVersionIsRefusedWhole) {
  GroupRoster roster;
  size_t skipped = 0;
  EXPECT_FALSE(roster.parseJson(R"({"v":2,"members":[{"id":"x","acr":"RF"}]})", skipped));
  EXPECT_EQ(roster.count(), 0u);
  // No version at all is the same answer: a document this build cannot place is
  // not an allowlist it may act on.
  EXPECT_FALSE(roster.parseJson(R"({"members":[{"id":"x","acr":"RF"}]})", skipped));
}

TEST(GroupRosterJson, ABadRowIsSkippedAndCounted) {
  GroupRoster roster;
  size_t skipped = 0;
  ASSERT_TRUE(roster.parseJson(
      R"({"v":1,"members":[{"id":"x","acr":"RF"},{"id":"y","acr":"TOOLONG"},{"id":"z","acr":"MK"}]})", skipped));
  EXPECT_EQ(roster.count(), 2u);
  EXPECT_EQ(skipped, 1u);
}

TEST(GroupRecordCsv, OlderRowsStillRead) {
  GroupRecord out;
  // The original eight-field row: still somebody's last known position, and the
  // two dating columns simply read as 0.
  ASSERT_TRUE(decodeGroupRecord("1789430400,81234,RF,48.1486000,17.1077000,4,62,lora", out));
  EXPECT_EQ(out.recvUtc, 0u);
  EXPECT_EQ(out.boot, 0u);
  EXPECT_STREQ(out.who, "RF");
  EXPECT_EQ(out.latE7, 481486000);

  // Nine fields: `boot` but no `recv_utc`.
  ASSERT_TRUE(decodeGroupRecord("1789430400,81234,3149271044,RF,48.1486000,17.1077000,4,62,lora", out));
  EXPECT_EQ(out.boot, 3149271044u);
  EXPECT_EQ(out.recvUtc, 0u);

  // Ten: `recv_utc` but no id.
  ASSERT_TRUE(decodeGroupRecord("1789430400,1789430402,81234,3149271044,RF,48.1486000,17.1077000,4,62,lora", out));
  EXPECT_EQ(out.recvUtc, 1789430402u);
  EXPECT_EQ(out.boot, 3149271044u);
  EXPECT_STREQ(out.id, "");

  // Eleven, which is what this build writes.
  ASSERT_TRUE(
      decodeGroupRecord("1789430400,1789430402,81234,3149271044,RF,a4c1380c,48.1486000,17.1077000,4,62,lora", out));
  EXPECT_STREQ(out.id, "a4c1380c");
  EXPECT_EQ(out.latE7, 481486000);

  EXPECT_FALSE(decodeGroupRecord("1,2,3,4,RF,x,48.1,17.1,,,lora,extra", out));  // twelve
}

TEST(GroupRecordCsv, RoundTrips) {
  GroupRecord rec;
  rec.utc = 1789430400u;
  rec.uptimeMs = 81234u;
  snprintf(rec.who, sizeof(rec.who), "RF");
  rec.latE7 = 481486000;
  rec.lonE7 = 171077000;
  rec.heading = 4;
  rec.hasHeading = true;
  rec.speedKmh = 62;
  rec.hasSpeed = true;
  rec.source = GroupFixSource::Lora;
  rec.boot = 3149271044u;
  rec.recvUtc = 1789430402u;
  snprintf(rec.id, sizeof(rec.id), "a4c1380c");

  char line[kGroupLineMax + 1];
  const size_t len = encodeGroupRecord(rec, line, sizeof(line));
  ASSERT_GT(len, 0u);
  EXPECT_STREQ(line, "1789430400,1789430402,81234,3149271044,RF,a4c1380c,48.1486000,17.1077000,4,62,lora");

  GroupRecord back;
  ASSERT_TRUE(decodeGroupRecord(std::string_view(line, len), back));
  EXPECT_EQ(back.utc, rec.utc);
  EXPECT_EQ(back.latE7, rec.latE7);
  EXPECT_EQ(back.lonE7, rec.lonE7);
  EXPECT_EQ(back.heading, 4);
  EXPECT_TRUE(back.hasHeading);
  EXPECT_EQ(back.speedKmh, 62);
  EXPECT_EQ(back.source, GroupFixSource::Lora);
  EXPECT_EQ(back.boot, rec.boot);
  EXPECT_EQ(back.recvUtc, rec.recvUtc);
  EXPECT_STREQ(back.id, "a4c1380c");
  EXPECT_STREQ(back.who, "RF");
}

TEST(GroupRecordCsv, EmptyHeadingAndSpeedStayEmpty) {
  GroupRecord rec;
  snprintf(rec.who, sizeof(rec.who), "%s", kGroupSelfWho);
  rec.latE7 = -481486000;
  rec.lonE7 = 5;
  rec.source = GroupFixSource::Gnss;

  char line[kGroupLineMax + 1];
  ASSERT_GT(encodeGroupRecord(rec, line, sizeof(line)), 0u);
  // `ME` has no transport identity: the id column is empty, not invented.
  EXPECT_STREQ(line, "0,0,0,0,ME,,-48.1486000,0.0000005,,,gnss");

  GroupRecord back;
  ASSERT_TRUE(decodeGroupRecord(line, back));
  EXPECT_FALSE(back.hasHeading);
  EXPECT_FALSE(back.hasSpeed);
  EXPECT_EQ(back.latE7, rec.latE7);
}

TEST(GroupRecordCsv, MalformedRowsAreRefused) {
  GroupRecord out;
  EXPECT_FALSE(decodeGroupRecord(kGroupCsvHeader, out));                     // the header line
  EXPECT_FALSE(decodeGroupRecord("1,2,RF,48.1,17.1,,,", out));               // no source word
  EXPECT_FALSE(decodeGroupRecord("1,2,3,4,RF,id,48.1,17.1,,,lora,x", out));  // one field too many
  EXPECT_FALSE(decodeGroupRecord("1,2,TOOLONG,48.1,17.1,,,lora", out));
  EXPECT_FALSE(decodeGroupRecord("1,2,RF,48.1,17.1,16,,lora", out));  // heading out of range
}

TEST(GroupDayFile, NamesTheDayAndSaysWhenItCannot) {
  char name[20] = {};
  ASSERT_TRUE(groupDayFileName(1789430400u, name, sizeof(name)));  // 2026-09-15 00:00 UTC
  EXPECT_STREQ(name, "bb-20260915.csv");
  ASSERT_TRUE(groupDayFileName(1789430400u + 86399u, name, sizeof(name)));  // same day, one second before midnight
  EXPECT_STREQ(name, "bb-20260915.csv");
  ASSERT_TRUE(groupDayFileName(1789430400u + 86400u, name, sizeof(name)));
  EXPECT_STREQ(name, "bb-20260916.csv");
  ASSERT_TRUE(groupDayFileName(0, name, sizeof(name)));
  EXPECT_STREQ(name, "bb-noclock.csv");
}

TEST(GroupAge, PrefersTheSendersClockAndFallsBackToReceipt) {
  GroupFix fix;
  fix.present = true;
  fix.utc = 1000;
  fix.recvUptimeMs = 5000;

  const GroupFixAge byUtc = groupFixAge(fix, 1300, 60000);
  EXPECT_TRUE(byUtc.known);
  EXPECT_EQ(byUtc.seconds, 300u);

  // No clock on either side: our own receipt uptime dates it.
  fix.utc = 0;
  const GroupFixAge byUptime = groupFixAge(fix, 0, 65000);
  EXPECT_TRUE(byUptime.known);
  EXPECT_EQ(byUptime.seconds, 60u);
}

TEST(GroupAge, OurOwnReceiptClockDatesAReplayedFix) {
  // The sender had no clock, we did. That is the common case: a peer's beacon
  // may carry nothing, and our own time comes from the phone or the receiver.
  GroupFix fix;
  fix.present = true;
  fix.fromLog = true;  // read back off the card, so its uptime means nothing
  fix.recvUtc = 1789430400u;
  const GroupFixAge age = groupFixAge(fix, 1789430400u + 600u, 1234);
  EXPECT_TRUE(age.known);
  EXPECT_EQ(age.seconds, 600u);
}

TEST(GroupAge, AReplayedFixWithNoClockHasNoKnownAge) {
  GroupFix fix;
  fix.present = true;
  fix.fromLog = true;
  fix.recvUptimeMs = 5000;  // a previous run's clock, meaningless now
  const GroupFixAge age = groupFixAge(fix, 0, 600000);
  EXPECT_FALSE(age.known);
  // And it is still drawn -- as stale, never hidden. A last known position with
  // no date is the best lead there is.
  EXPECT_EQ(groupFixVisibility(age, 300, 1800), GroupVisibility::Stale);
}

TEST(GroupAge, FreshStaleHidden) {
  GroupFixAge age;
  age.known = true;
  age.seconds = 10;
  EXPECT_EQ(groupFixVisibility(age, 300, 1800), GroupVisibility::Fresh);
  age.seconds = 400;
  EXPECT_EQ(groupFixVisibility(age, 300, 1800), GroupVisibility::Stale);
  age.seconds = 3600;
  EXPECT_EQ(groupFixVisibility(age, 300, 1800), GroupVisibility::Hidden);
  // Hiding off: an old position keeps being drawn as stale.
  EXPECT_EQ(groupFixVisibility(age, 300, 0), GroupVisibility::Stale);
}

TEST(GroupGrammar, PosTakesEitherHandleAndAKeywordTail) {
  const MapCommand byAcr = parseMapCommand("group pos RF 48.1486 17.1077 utc 1789430400 heading 4 speed 62 src lora");
  ASSERT_EQ(byAcr.type, MapCommandType::Group);
  EXPECT_EQ(byAcr.groupVerb, MapGroupVerb::Pos);
  EXPECT_STREQ(byAcr.groupAcr, "RF");
  EXPECT_EQ(byAcr.latE7, 481486000);
  EXPECT_EQ(byAcr.groupUtc, 1789430400u);
  EXPECT_EQ(byAcr.heading, 4);
  EXPECT_TRUE(byAcr.hasSpeed);
  EXPECT_EQ(byAcr.groupSource, GroupFixSource::Lora);

  const MapCommand byId = parseMapCommand("group pos !a4c1380c 48.1 17.1");
  ASSERT_EQ(byId.type, MapCommandType::Group);
  EXPECT_STREQ(byId.groupId, "!a4c1380c");
  EXPECT_STREQ(byId.groupAcr, "");
}

TEST(GroupGrammar, ABareTailIsRefused) {
  // Unlike `pos`, where a bare number fills heading then speed: after a
  // coordinate here a bare number is as likely a unix time as a heading.
  const MapCommand cmd = parseMapCommand("group pos RF 48.1 17.1 4");
  EXPECT_EQ(cmd.type, MapCommandType::Error);
  EXPECT_EQ(cmd.error, MapCommandError::BadArity);
}

TEST(GroupGrammar, AddTakesANameWithSpaces) {
  const MapCommand cmd = parseMapCommand("group add MK !b7e2 Marek Novak");
  ASSERT_EQ(cmd.type, MapCommandType::Group);
  EXPECT_EQ(cmd.groupVerb, MapGroupVerb::Add);
  EXPECT_STREQ(cmd.groupAcr, "MK");
  EXPECT_STREQ(cmd.groupId, "!b7e2");
  EXPECT_STREQ(cmd.groupName, "Marek Novak");
}

TEST(GroupGrammar, BadMemberIsItsOwnError) {
  EXPECT_EQ(parseMapCommand("group add TOOLONG id").error, MapCommandError::BadMember);
  // A single letter is not an acronym but is a legal id, so it resolves as one
  // and the roster -- not the grammar -- decides whether it is known.
  EXPECT_EQ(parseMapCommand("group pos R 48.1 17.1").type, MapCommandType::Group);
  EXPECT_EQ(parseMapCommand("group pos R,F 48.1 17.1").error, MapCommandError::BadMember);
  EXPECT_STREQ(mapCommandErrorText(MapCommandError::BadMember), "bad_member");
}

TEST(GroupGrammar, ListReloadAndLog) {
  EXPECT_EQ(parseMapCommand("group list").groupVerb, MapGroupVerb::List);
  EXPECT_EQ(parseMapCommand("group reload").groupVerb, MapGroupVerb::Reload);
  EXPECT_EQ(parseMapCommand("group log 8").groupLogOffset, 8);
  EXPECT_EQ(parseMapCommand("group list extra").type, MapCommandType::Error);
}

TEST(GroupLabel, FourShapes) {
  GroupFixAge fresh;
  fresh.known = true;
  fresh.seconds = 30;
  GroupFixAge old;
  old.known = true;
  old.seconds = 12 * 60;

  char buf[kGroupLabelBytes];
  // Nothing: close in, and current.
  EXPECT_EQ(groupMarkerLabel(false, 1200, false, fresh, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "");

  // Distance alone: the rung is wide enough that the eye cannot judge it.
  ASSERT_GT(groupMarkerLabel(true, 1200, false, fresh, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "1.2 km");

  // Age alone: close enough to see, old enough to doubt.
  ASSERT_GT(groupMarkerLabel(false, 1200, true, old, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "15m");

  // Both.
  ASSERT_GT(groupMarkerLabel(true, 1200, true, old, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "1.2 km/15m");
}

TEST(GroupLabel, RoundedUpToFiveMinutes) {
  char buf[kGroupLabelBytes];
  GroupFixAge age;
  age.known = true;
  // Rounded up, never down: a position is never claimed fresher than it is, and
  // the step matches how often the panel revisits it.
  age.seconds = 61;
  ASSERT_GT(groupMarkerLabel(false, 0, true, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "5m");
  age.seconds = 5 * 60;
  ASSERT_GT(groupMarkerLabel(false, 0, true, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "5m");
  age.seconds = 5 * 60 + 1;
  ASSERT_GT(groupMarkerLabel(false, 0, true, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "10m");
  age.seconds = 47 * 60;
  ASSERT_GT(groupMarkerLabel(false, 0, true, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "50m");
  age.seconds = 58 * 60;
  ASSERT_GT(groupMarkerLabel(false, 0, true, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "1h");
}

TEST(GroupLabel, HoursAndTheUnknownAge) {
  GroupFixAge hours;
  hours.known = true;
  hours.seconds = 3 * 3600 + 900;
  char buf[kGroupLabelBytes];
  ASSERT_GT(groupMarkerLabel(false, 0, true, hours, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "4h");

  // A replayed fix on a device with no clock: `?`, never a number nobody can
  // stand behind.
  const GroupFixAge unknown;
  ASSERT_GT(groupMarkerLabel(true, 450, true, unknown, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "500 m/?");
}

TEST(GroupLabel, NothingIsSaidAboutTheLastMinute) {
  GroupFixAge seconds;
  seconds.known = true;
  seconds.seconds = 40;
  char buf[kGroupLabelBytes];
  // Stale by a threshold of zero, but `0m` would read as information and carry
  // none -- the hollow head is the whole message there.
  EXPECT_EQ(groupMarkerLabel(false, 450, true, seconds, buf, sizeof(buf)), 0u);
  ASSERT_GT(groupMarkerLabel(true, 450, true, seconds, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "500 m");
}

TEST(GroupLabel, DistanceSteps) {
  GroupFixAge age;
  char buf[kGroupLabelBytes];
  // 100 m steps under a kilometre: the fix behind the number is good to tens of
  // metres and arrived minutes ago, so a metre is noise dressed as precision.
  ASSERT_GT(groupMarkerLabel(true, 940, false, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "900 m");
  // Nearest hundred, floored at one: `0 m` would claim they are on top of you.
  ASSERT_GT(groupMarkerLabel(true, 40, false, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "100 m");
  ASSERT_GT(groupMarkerLabel(true, 960, false, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "1.0 km");
  ASSERT_GT(groupMarkerLabel(true, 9400, false, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "9.4 km");
  // Rounded, not truncated, once the decimal is gone.
  ASSERT_GT(groupMarkerLabel(true, 12600, false, age, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "13 km");
}

// --- the console over a fake source -----------------------------------------

class FakeGroupSource : public IMapGroupSource {
 public:
  Ingest groupPosition(std::string_view idOrAcr, const GroupFix& fix) override {
    lastHandle = std::string(idOrAcr);
    if (roster.find(idOrAcr) >= GroupRoster::kSlotCount) return Ingest::UnknownMember;
    const size_t slot = roster.find(idOrAcr);
    if (!roster.at(slot).enabled) return Ingest::Muted;
    GroupFix stored = fix;
    stored.present = true;
    store.set(slot, stored);
    return Ingest::Accepted;
  }
  Edit groupAdd(std::string_view acr, std::string_view id, std::string_view name) override {
    return roster.set(id, acr, name, true) ? Edit::Ok : Edit::Invalid;
  }
  Edit groupRemove(std::string_view idOrAcr) override {
    return roster.remove(idOrAcr) ? Edit::Ok : Edit::UnknownMember;
  }
  bool groupReload(size_t& skipped) override {
    skipped = 0;
    return true;
  }
  size_t groupCount() const override { return roster.count(); }
  GroupMember groupMemberAt(size_t index) const override { return roster.at(slotFor(index)); }
  GroupFix groupFixAt(size_t index) const override { return store.at(slotFor(index)); }
  uint32_t groupLogPage(uint32_t, uint32_t, IGroupLogVisitor&) override { return 0; }

  GroupRoster roster;
  GroupStore store;
  std::string lastHandle;

 private:
  size_t slotFor(size_t index) const {
    size_t seen = 0;
    for (size_t slot = 0; slot < GroupRoster::kSlotCount; ++slot) {
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

TEST(GroupConsole, WithoutASourceItSaysUnavailable) {
  MapConsoleState state;
  RecordingWriter out;
  state.execute(parseMapCommand("group list"), out);
  // Not "no members": a build that never wired the layer must not read as a
  // rider whose group is empty.
  EXPECT_TRUE(out.has("INFO group=unavailable"));
}

TEST(GroupConsole, AStrangerIsRefusedAndSaidOutLoud) {
  FakeGroupSource source;
  MapConsoleState state;
  state.setGroupSource(&source);
  RecordingWriter out;
  const bool redraw = state.execute(parseMapCommand("group pos ZZ 48.1 17.1"), out);
  EXPECT_FALSE(redraw);
  EXPECT_TRUE(out.has("ERR unknown_member"));
}

TEST(GroupConsole, AnAcceptedPositionRedraws) {
  FakeGroupSource source;
  ASSERT_TRUE(source.roster.set("id-1", "RF", "Roman", true));
  MapConsoleState state;
  state.setGroupSource(&source);
  RecordingWriter out;
  EXPECT_TRUE(state.execute(parseMapCommand("group pos RF 48.1486 17.1077"), out));
  EXPECT_TRUE(out.has("INFO group_pos=RF"));
  EXPECT_TRUE(out.has("OK"));
}

TEST(GroupConsole, ListSaysHasNotToldUsWithEmptyFieldsNotZero) {
  FakeGroupSource source;
  ASSERT_TRUE(source.roster.set("id-1", "RF", "Roman", true));
  MapConsoleState state;
  state.setGroupSource(&source);

  RecordingWriter before;
  state.execute(parseMapCommand("group list"), before);
  EXPECT_TRUE(before.has("INFO group_total=1"));
  // Empty coordinate fields, never 0,0 -- which is a place in the Atlantic.
  EXPECT_TRUE(before.has("INFO group_RF=id-1,on,,,,"));

  RecordingWriter after;
  state.execute(parseMapCommand("group pos RF 48.1486 17.1077 utc 1789430400 src ble"), after);
  RecordingWriter listed;
  state.execute(parseMapCommand("group list"), listed);
  EXPECT_TRUE(listed.has("INFO group_RF=id-1,on,48.1486000,17.1077000,1789430400,ble"));
}

}  // namespace
