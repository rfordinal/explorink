// Pins phase 1: the record format, the active store, the log reader's damage
// rules and the `pin` console commands -- all natively, with no card and no
// device (../../docs/pins-plan.md, phase 1).
//
// Everything the device does to a pin except drawing it goes through the four
// pure files under test here. PinLog itself is the only part not covered: it is
// HalStorage and nothing else, and the parsing it would exercise is exercised
// directly below.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "MapCommandConsole.h"
#include "MapCommandParser.h"
#include "PinCatalog.h"
#include "PinLogScanner.h"
#include "PinRecord.h"
#include "PinStore.h"

namespace {

std::string encodeLine(const PinRecord& rec) {
  char buf[kPinLineMax + 1] = {};
  const size_t len = encodePinRecord(rec, buf, sizeof(buf));
  EXPECT_GT(len, 0u);
  return std::string(buf, len);
}

PinRecord makeRecord(uint32_t seq, PinOp op, const char* key, uint32_t id, int32_t latE7, int32_t lonE7,
                     uint32_t utc = 0, uint32_t uptimeMs = 0) {
  PinRecord rec;
  rec.seq = seq;
  rec.op = op;
  rec.id = id;
  rec.utc = utc;
  rec.uptimeMs = uptimeMs;
  if (op != PinOp::Delete) {
    rec.latE7 = latE7;
    rec.lonE7 = lonE7;
    rec.hasPos = true;
  }
  EXPECT_TRUE(setPinRecordKey(rec, key));
  return rec;
}

// Flips one character of a line so its CRC no longer matches, without changing
// the field count -- the damage a card actually produces.
std::string corrupt(std::string line) {
  const size_t at = line.find("48");
  EXPECT_NE(at, std::string::npos);
  line[at + 1] = '9';
  return line;
}

PinReplayStats replayText(const std::string& text, PinStore& store) {
  PinLogReplayer replayer(store);
  // Fed in three-byte bites on purpose: the device reads the card in chunks and
  // a record lands split across two of them almost every time.
  for (size_t i = 0; i < text.size(); i += 3) {
    const size_t len = std::min<size_t>(3, text.size() - i);
    replayer.feed(text.data() + i, len);
  }
  replayer.finish();
  return replayer.stats();
}

}  // namespace

// ------------------------------------------------------------- the catalogue

TEST(PinCatalog, KeysAreUniqueAndStorable) {
  for (size_t i = 0; i < kPinSlotCount; ++i) {
    EXPECT_TRUE(isValidPinKey(kPinCatalog[i].key)) << kPinCatalog[i].key;
    EXPECT_EQ(pinCatalogIndex(kPinCatalog[i].key), i) << kPinCatalog[i].key;
    for (size_t j = i + 1; j < kPinSlotCount; ++j) {
      EXPECT_STRNE(kPinCatalog[i].key, kPinCatalog[j].key);
    }
  }
}

TEST(PinCatalog, UnknownKeyIsReported) {
  EXPECT_EQ(pinCatalogIndex("nope"), kPinIndexUnknown);
  EXPECT_EQ(pinCatalogIndex(""), kPinIndexUnknown);
  EXPECT_EQ(pinCatalogIndex("Base"), kPinIndexUnknown);  // case sensitive
}

TEST(PinCatalog, KeysThatWouldBreakTheFormatAreRefused) {
  EXPECT_FALSE(isValidPinKey(""));
  EXPECT_FALSE(isValidPinKey("a|b"));
  EXPECT_FALSE(isValidPinKey("a b"));
  EXPECT_FALSE(isValidPinKey("a,b"));
  EXPECT_FALSE(isValidPinKey("a\nb"));
  EXPECT_FALSE(isValidPinKey("waaaaaaaaaytoolong"));
  EXPECT_TRUE(isValidPinKey("future_x"));  // a key a later firmware might write
}

// ----------------------------------------------------------------- the record

TEST(PinRecord, RoundTripsEveryOp) {
  const PinOp ops[] = {PinOp::Add, PinOp::Replace, PinOp::Delete, PinOp::Restore};
  for (const PinOp op : ops) {
    const PinRecord rec = makeRecord(7, op, "camp", 3, 484372000, 170186000, 1755400000, 12345);
    PinRecord back;
    ASSERT_TRUE(decodePinRecord(encodeLine(rec), back)) << pinOpText(op);
    EXPECT_EQ(back.seq, 7u);
    EXPECT_EQ(back.op, op);
    EXPECT_EQ(back.id, 3u);
    EXPECT_EQ(back.utc, 1755400000u);
    EXPECT_EQ(back.uptimeMs, 12345u);
    EXPECT_STREQ(back.key, "camp");
    EXPECT_EQ(back.hasPos, op != PinOp::Delete);
    if (back.hasPos) {
      EXPECT_EQ(back.latE7, 484372000);
      EXPECT_EQ(back.lonE7, 170186000);
    }
  }
}

TEST(PinRecord, RoundTripsNegativeCoordinatesAndAnUnknownClock) {
  const PinRecord rec = makeRecord(1, PinOp::Add, "base", 1, -338688000, -1712095000, 0, 900);
  PinRecord back;
  ASSERT_TRUE(decodePinRecord(encodeLine(rec), back));
  EXPECT_EQ(back.latE7, -338688000);
  EXPECT_EQ(back.lonE7, -1712095000);
  EXPECT_EQ(back.utc, 0u);  // 0 stays 0: no clock is never a fabricated time
  EXPECT_EQ(back.uptimeMs, 900u);
}

TEST(PinRecord, EncodedLineHasTheDocumentedShape) {
  const PinRecord rec = makeRecord(4, PinOp::Replace, "parking", 2, 484372000, 170186000, 1755400000, 60000);
  const std::string line = encodeLine(rec);
  // The whole line, checksum included. The expected CRC comes from Python's
  // zlib.crc32 over the bytes before the final separator, which is what pins the
  // format to the one every other map file on the card uses (MapCrc32.h) -- and
  // what makes the example in docs/pins.md checkable.
  EXPECT_EQ(line, "v1|4|1755400000|60000|rep|parking|2|484372000|170186000||0afa87b9");
}

TEST(PinRecord, BadCrcIsRefused) {
  const std::string line = encodeLine(makeRecord(1, PinOp::Add, "camp", 1, 484372000, 170186000));
  PinRecord back;
  EXPECT_FALSE(decodePinRecord(corrupt(line), back));
}

TEST(PinRecord, UnknownVersionIsRefused) {
  std::string line = encodeLine(makeRecord(1, PinOp::Add, "camp", 1, 484372000, 170186000));
  line[1] = '2';  // v2, whatever that will mean
  PinRecord back;
  EXPECT_FALSE(decodePinRecord(line, back));
}

TEST(PinRecord, MalformedLinesAreRefused) {
  PinRecord back;
  EXPECT_FALSE(decodePinRecord("", back));
  EXPECT_FALSE(decodePinRecord("v1|1|0|0|add|camp|1|484372000|170186000|", back));  // no crc field
  EXPECT_FALSE(decodePinRecord("v1|1|0|0|add|camp|1|484372000|170186000||zzzzzzzz", back));
  // An op this build does not know, with an otherwise valid CRC over it.
  PinRecord rec = makeRecord(1, PinOp::Add, "camp", 1, 484372000, 170186000);
  std::string line = encodeLine(rec);
  const size_t at = line.find("|add|");
  ASSERT_NE(at, std::string::npos);
  line.replace(at, 5, "|zap|");
  EXPECT_FALSE(decodePinRecord(line, back));
}

TEST(PinRecord, APlacementWithoutACoordinateIsRefused) {
  // Hand-built, because encodePinRecord() cannot produce it: an add whose
  // coordinate fields are empty places nothing and must not load as if it did.
  PinRecord rec = makeRecord(1, PinOp::Delete, "camp", 1, 0, 0);
  std::string line = encodeLine(rec);
  const size_t at = line.find("|del|");
  ASSERT_NE(at, std::string::npos);
  line.replace(at, 5, "|add|");
  PinRecord back;
  EXPECT_FALSE(decodePinRecord(line, back));  // CRC also breaks; both reasons are refusals
}

TEST(PinRecord, AKeyWithASeparatorNeverEncodes) {
  PinRecord rec;
  rec.hasPos = true;
  // setPinRecordKey refuses it, so force the bytes in to prove encode also does.
  rec.key[0] = 'a';
  rec.key[1] = '|';
  rec.key[2] = '\0';
  char buf[kPinLineMax + 1] = {};
  EXPECT_EQ(encodePinRecord(rec, buf, sizeof(buf)), 0u);
}

// ------------------------------------------------------------------ the store

TEST(PinStore, AddThenFind) {
  PinStore store;
  PinRecord rec;
  ASSERT_TRUE(store.makeSetRecord("camp", 484372000, 170186000, 0, 100, rec));
  EXPECT_EQ(rec.op, PinOp::Add);
  EXPECT_EQ(rec.seq, 1u);
  EXPECT_EQ(rec.id, 1u);
  ASSERT_TRUE(store.apply(rec));

  const PinEntry* entry = store.find("camp");
  ASSERT_NE(entry, nullptr);
  EXPECT_TRUE(entry->present);
  EXPECT_EQ(entry->latE7, 484372000);
  EXPECT_EQ(entry->catalogIndex, pinCatalogIndex("camp"));
  EXPECT_EQ(store.presentCount(), 1u);
}

TEST(PinStore, ReplaceKeepsTheIdAndMovesThePin) {
  PinStore store;
  PinRecord add;
  ASSERT_TRUE(store.makeSetRecord("parking", 100000000, 200000000, 0, 1, add));
  ASSERT_TRUE(store.apply(add));

  PinRecord rep;
  ASSERT_TRUE(store.makeSetRecord("parking", 110000000, 210000000, 0, 2, rep));
  EXPECT_EQ(rep.op, PinOp::Replace);
  EXPECT_EQ(rep.id, add.id) << "a replace is the same pin, moved";
  EXPECT_EQ(rep.seq, add.seq + 1);
  ASSERT_TRUE(store.apply(rep));
  EXPECT_EQ(store.find("parking")->latE7, 110000000);
  EXPECT_EQ(store.presentCount(), 1u);
}

TEST(PinStore, DeleteThenCreateGivesANewIdUnderTheSameKey) {
  PinStore store;
  PinRecord add;
  ASSERT_TRUE(store.makeSetRecord("camp", 100000000, 200000000, 0, 1, add));
  ASSERT_TRUE(store.apply(add));

  PinRecord del;
  ASSERT_TRUE(store.makeDeleteRecord("camp", 0, 2, del));
  EXPECT_EQ(del.op, PinOp::Delete);
  EXPECT_FALSE(del.hasPos);
  EXPECT_EQ(del.id, add.id);
  ASSERT_TRUE(store.apply(del));
  EXPECT_EQ(store.find("camp"), nullptr);
  EXPECT_EQ(store.presentCount(), 0u);

  PinRecord again;
  ASSERT_TRUE(store.makeSetRecord("camp", 300000000, 400000000, 0, 3, again));
  EXPECT_EQ(again.op, PinOp::Add);
  EXPECT_NE(again.id, add.id) << "a remade camp is a second pin to whoever reads the log";
  EXPECT_EQ(again.id, add.id + 1);
}

TEST(PinStore, DeletingAnEmptySlotProducesNoRecord) {
  PinStore store;
  PinRecord del;
  EXPECT_FALSE(store.makeDeleteRecord("camp", 0, 1, del));
}

TEST(PinStore, EveryCatalogueSlotFits) {
  PinStore store;
  for (size_t i = 0; i < kPinSlotCount; ++i) {
    PinRecord rec;
    ASSERT_TRUE(store.makeSetRecord(kPinCatalog[i].key, 100000000, 200000000, 0, 0, rec)) << kPinCatalog[i].key;
    ASSERT_TRUE(store.apply(rec));
  }
  EXPECT_EQ(store.presentCount(), kPinSlotCount);
}

TEST(PinStore, AForeignKeyLoadsAndStaysDeletable) {
  PinStore store;
  // What a newer firmware wrote and this build has never heard of.
  const PinRecord rec = makeRecord(5, PinOp::Add, "summit", 9, 484372000, 170186000);
  ASSERT_TRUE(store.apply(rec));

  const PinEntry* entry = store.find("summit");
  ASSERT_NE(entry, nullptr) << "an update must not eat a pin it does not understand";
  EXPECT_EQ(entry->catalogIndex, kPinIndexUnknown);
  EXPECT_STREQ(entry->key, "summit");

  PinRecord del;
  ASSERT_TRUE(store.makeDeleteRecord("summit", 0, 1, del));
  ASSERT_TRUE(store.apply(del));
  EXPECT_EQ(store.find("summit"), nullptr);
}

TEST(PinStore, ForeignKeysBeyondTheHeldSlotsAreRefusedNotMisfiled) {
  PinStore store;
  for (size_t i = 0; i < kPinUnknownSlots; ++i) {
    const std::string key = "far" + std::to_string(i);
    ASSERT_TRUE(store.apply(makeRecord(static_cast<uint32_t>(i + 1), PinOp::Add, key.c_str(),
                                       static_cast<uint32_t>(i + 1), 100000000, 200000000)));
  }
  const PinRecord overflow = makeRecord(99, PinOp::Add, "onemore", 99, 100000000, 200000000);
  EXPECT_FALSE(store.apply(overflow));
  // The counters still moved: the event happened even though it could not be held.
  EXPECT_EQ(store.nextSeq(), 100u);
  EXPECT_EQ(store.nextId(), 100u);
}

// ------------------------------------------------------------- the log reader

TEST(PinLogReplay, RebuildsTheActivePinsInOrder) {
  std::string text;
  text += encodeLine(makeRecord(1, PinOp::Add, "base", 1, 100000000, 200000000)) + "\n";
  text += encodeLine(makeRecord(2, PinOp::Add, "camp", 2, 300000000, 400000000)) + "\n";
  text += encodeLine(makeRecord(3, PinOp::Replace, "camp", 2, 310000000, 410000000)) + "\n";
  text += encodeLine(makeRecord(4, PinOp::Delete, "base", 1, 0, 0)) + "\n";

  PinStore store;
  const PinReplayStats stats = replayText(text, store);
  EXPECT_EQ(stats.applied, 4u);
  EXPECT_EQ(stats.skipped, 0u);
  EXPECT_EQ(store.presentCount(), 1u);
  EXPECT_EQ(store.find("base"), nullptr);
  ASSERT_NE(store.find("camp"), nullptr);
  EXPECT_EQ(store.find("camp")->latE7, 310000000);
}

TEST(PinLogReplay, ADamagedRecordNeverInvalidatesTheOthers) {
  std::string text;
  text += encodeLine(makeRecord(1, PinOp::Add, "base", 1, 484372000, 170186000)) + "\n";
  text += corrupt(encodeLine(makeRecord(2, PinOp::Add, "camp", 2, 484372000, 170186000))) + "\n";
  std::string bogusVersion = encodeLine(makeRecord(3, PinOp::Add, "meet", 3, 484372000, 170186000));
  bogusVersion[1] = '9';
  text += bogusVersion + "\n";
  text += "garbage with no separators at all\n";
  text += encodeLine(makeRecord(4, PinOp::Add, "dest", 4, 484372000, 170186000)) + "\n";

  PinStore store;
  const PinReplayStats stats = replayText(text, store);
  EXPECT_EQ(stats.applied, 2u);
  EXPECT_EQ(stats.skipped, 3u);
  EXPECT_NE(store.find("base"), nullptr);
  EXPECT_NE(store.find("dest"), nullptr);
  EXPECT_EQ(store.find("camp"), nullptr);
  EXPECT_EQ(store.find("meet"), nullptr);
  EXPECT_EQ(store.nextSeq(), 5u) << "seq continues past the records this build refused";
}

TEST(PinLogReplay, ATornFinalLineIsDiscardedAndTheRestSurvives) {
  const std::string whole = encodeLine(makeRecord(1, PinOp::Add, "base", 1, 484372000, 170186000));
  const std::string torn = encodeLine(makeRecord(2, PinOp::Add, "camp", 2, 484372000, 170186000));
  const std::string text = whole + "\n" + torn.substr(0, torn.size() / 2);

  PinStore store;
  const PinReplayStats stats = replayText(text, store);
  EXPECT_EQ(stats.applied, 1u);
  EXPECT_EQ(stats.skipped, 1u);
  EXPECT_NE(store.find("base"), nullptr);
  EXPECT_EQ(store.find("camp"), nullptr);
}

TEST(PinLogReplay, AnOverlongLineIsSwallowedToItsOwnEnd) {
  std::string text(kPinLineMax + 40, 'x');
  text += "\n";
  text += encodeLine(makeRecord(1, PinOp::Add, "base", 1, 484372000, 170186000)) + "\n";

  PinStore store;
  const PinReplayStats stats = replayText(text, store);
  EXPECT_EQ(stats.applied, 1u) << "the record after an over-long line still lands";
  EXPECT_EQ(stats.skipped, 1u);
}

TEST(PinLogReplay, BlankLinesAreNeitherAppliedNorCountedAsDamage) {
  std::string text = "\n\n";
  text += encodeLine(makeRecord(1, PinOp::Add, "base", 1, 484372000, 170186000)) + "\n\n";

  PinStore store;
  const PinReplayStats stats = replayText(text, store);
  EXPECT_EQ(stats.applied, 1u);
  EXPECT_EQ(stats.skipped, 0u);
}

TEST(PinLogReplay, SeqAndIdContinueAfterAReboot) {
  std::string text;
  text += encodeLine(makeRecord(1, PinOp::Add, "base", 1, 100000000, 200000000)) + "\n";
  text += encodeLine(makeRecord(2, PinOp::Add, "camp", 2, 300000000, 400000000)) + "\n";

  PinStore store;
  ASSERT_EQ(replayText(text, store).applied, 2u);
  EXPECT_EQ(store.nextSeq(), 3u);
  EXPECT_EQ(store.nextId(), 3u);

  PinRecord next;
  ASSERT_TRUE(store.makeSetRecord("meet", 500000000, 600000000, 0, 5, next));
  EXPECT_EQ(next.seq, 3u) << "a reboot must not reuse a seq";
  EXPECT_EQ(next.id, 3u);
}

TEST(PinLogReplay, RecordsWithNoClockKeepTheirUptimeOrdering) {
  // Nothing here has a utc, so uptime is the only thing that orders two records
  // inside one run -- it has to survive the round trip.
  std::string text;
  text += encodeLine(makeRecord(1, PinOp::Add, "camp", 1, 100000000, 200000000, 0, 1000)) + "\n";
  text += encodeLine(makeRecord(2, PinOp::Replace, "camp", 1, 110000000, 210000000, 0, 90000)) + "\n";

  PinStore store;
  ASSERT_EQ(replayText(text, store).applied, 2u);
  ASSERT_NE(store.find("camp"), nullptr);
  EXPECT_EQ(store.find("camp")->utc, 0u);
  EXPECT_EQ(store.find("camp")->latE7, 110000000) << "the later uptime is the later record";
}

// -------------------------------------------------------------- the grammar

TEST(PinCommand, SetParses) {
  const MapCommand cmd = parseMapCommand("pin set camp 48.4372 17.0186");
  ASSERT_EQ(cmd.type, MapCommandType::Pin);
  EXPECT_EQ(cmd.pinVerb, MapPinVerb::Set);
  EXPECT_STREQ(cmd.pinKey, "camp");
  EXPECT_EQ(cmd.latE7, 484372000);
  EXPECT_EQ(cmd.lonE7, 170186000);
  EXPECT_EQ(cmd.pinUtc, 0u);

  const MapCommand stamped = parseMapCommand("pin set base -33.8688 151.2093 1755400000");
  ASSERT_EQ(stamped.type, MapCommandType::Pin);
  EXPECT_EQ(stamped.latE7, -338688000);
  EXPECT_EQ(stamped.pinUtc, 1755400000u);
}

TEST(PinCommand, DelListAndLogParse) {
  const MapCommand del = parseMapCommand("pin del parking");
  ASSERT_EQ(del.type, MapCommandType::Pin);
  EXPECT_EQ(del.pinVerb, MapPinVerb::Del);
  EXPECT_STREQ(del.pinKey, "parking");

  EXPECT_EQ(parseMapCommand("pin list").pinVerb, MapPinVerb::List);

  const MapCommand log = parseMapCommand("pin log");
  EXPECT_EQ(log.pinVerb, MapPinVerb::Log);
  EXPECT_EQ(log.pinLogOffset, 0u);
  EXPECT_EQ(parseMapCommand("pin log 24").pinLogOffset, 24u);
}

TEST(PinCommand, BadFormsAreRejectedWithTheRightReason) {
  auto errorOf = [](const char* line) {
    const MapCommand cmd = parseMapCommand(line);
    EXPECT_EQ(cmd.type, MapCommandType::Error) << line;
    return cmd.error;
  };

  EXPECT_EQ(errorOf("pin"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pin list extra"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pin del"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pin set camp 48.4"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pin set camp 48.4 17.0 1 2"), MapCommandError::BadArity);
  EXPECT_EQ(errorOf("pin wat"), MapCommandError::UnknownCommand);
  // A typo must not occupy a slot, and must not be mistaken for a foreign key
  // written by a newer firmware.
  EXPECT_EQ(errorOf("pin del kamp"), MapCommandError::UnknownPin);
  EXPECT_EQ(errorOf("pin set kamp 48.4 17.0"), MapCommandError::UnknownPin);
  EXPECT_EQ(errorOf("pin set camp 48.4 abc"), MapCommandError::BadNumber);
  EXPECT_EQ(errorOf("pin set camp 91 17.0"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("pin set camp 48.4 181"), MapCommandError::OutOfRange);
  EXPECT_EQ(errorOf("pin log 70000"), MapCommandError::OutOfRange);
  EXPECT_STREQ(mapCommandErrorText(MapCommandError::UnknownPin), "unknown_pin");
}

// -------------------------------------------------------- the console replies

namespace {

class CollectingWriter final : public IMapReplyWriter {
 public:
  void reply(const char* line) override { lines.emplace_back(line); }
  bool has(const std::string& line) const {
    for (const std::string& l : lines) {
      if (l == line) return true;
    }
    return false;
  }
  size_t countPrefixed(const std::string& prefix) const {
    size_t n = 0;
    for (const std::string& l : lines) {
      if (l.rfind(prefix, 0) == 0) ++n;
    }
    return n;
  }
  std::vector<std::string> lines;
};

// PinStore plus an in-memory log: the same order of operations the device uses
// (append first, apply only if that worked), with a vector standing in for the
// card. `failWrites` is how a full or missing card is exercised.
class FakePinsSource final : public IMapPinsSource {
 public:
  bool pinSet(std::string_view key, int32_t latE7, int32_t lonE7, uint32_t utc) override {
    PinRecord rec;
    if (!store.makeSetRecord(key, latE7, lonE7, utc, ++uptimeMs, rec)) return false;
    if (failWrites) return false;
    log.emplace_back(encodeLine(rec));
    return store.apply(rec);
  }

  bool pinDelete(std::string_view key) override {
    PinRecord rec;
    if (!store.makeDeleteRecord(key, 0, ++uptimeMs, rec)) return false;
    if (failWrites) return false;
    log.emplace_back(encodeLine(rec));
    return store.apply(rec);
  }

  size_t pinCount() const override { return store.presentCount(); }

  PinEntry pinAt(size_t index) const override {
    size_t seen = 0;
    for (size_t slot = 0; slot < PinStore::kSlotCount; ++slot) {
      if (!store.at(slot).present) continue;
      if (seen == index) return store.at(slot);
      ++seen;
    }
    return PinEntry{};
  }

  uint32_t pinLogPage(uint32_t offset, uint32_t maxCount, IPinLogVisitor& visitor) override {
    const uint32_t total = static_cast<uint32_t>(log.size());
    if (offset >= total) return total;
    uint32_t emitted = 0;
    for (uint32_t i = total - offset; i > 0 && emitted < maxCount; --i, ++emitted) {
      PinRecord rec;
      if (decodePinRecord(log[i - 1], rec)) visitor.onPinLogRecord(rec);
    }
    return total;
  }

  PinStore store;
  std::vector<std::string> log;
  uint32_t uptimeMs = 0;
  bool failWrites = false;
};

}  // namespace

TEST(PinConsole, WithNoSourceEveryPinCommandSaysUnavailable) {
  MapConsoleState state;
  CollectingWriter out;
  EXPECT_FALSE(state.execute(parseMapCommand("pin list"), out));
  EXPECT_TRUE(out.has("INFO pins=unavailable")) << "must never read as 'the rider saved nothing'";
  EXPECT_TRUE(out.has("OK"));
}

TEST(PinConsole, SetListAndDelete) {
  MapConsoleState state;
  FakePinsSource pins;
  state.setPinsSource(&pins);

  CollectingWriter set;
  EXPECT_TRUE(state.execute(parseMapCommand("pin set camp 48.4372 17.0186"), set))
      << "a new pin has to be drawn, so the command asks for a redraw";
  EXPECT_TRUE(set.has("INFO pin_set=camp"));
  EXPECT_TRUE(set.has("OK"));

  CollectingWriter list;
  state.execute(parseMapCommand("pin list"), list);
  EXPECT_TRUE(list.has("INFO pins_total=1"));
  EXPECT_TRUE(list.has("INFO pin_camp=48.4372000,17.0186000,0,1")) << list.lines.back();

  CollectingWriter del;
  EXPECT_TRUE(state.execute(parseMapCommand("pin del camp"), del));
  EXPECT_TRUE(del.has("INFO pin_del=camp"));

  CollectingWriter empty;
  state.execute(parseMapCommand("pin list"), empty);
  EXPECT_TRUE(empty.has("INFO pins_total=0"));
  EXPECT_EQ(empty.countPrefixed("INFO pin_"), 0u);
}

TEST(PinConsole, ARefusedWriteAnswersErrAndChangesNothing) {
  MapConsoleState state;
  FakePinsSource pins;
  state.setPinsSource(&pins);
  pins.failWrites = true;

  CollectingWriter out;
  EXPECT_FALSE(state.execute(parseMapCommand("pin set camp 48.4372 17.0186"), out));
  EXPECT_TRUE(out.has("ERR pin_write"));
  EXPECT_FALSE(out.has("OK")) << "ERR is the terminator; a sender must not see both";
  EXPECT_EQ(pins.store.presentCount(), 0u);
}

TEST(PinConsole, DeletingAPinThatIsNotThereAnswersErr) {
  MapConsoleState state;
  FakePinsSource pins;
  state.setPinsSource(&pins);

  CollectingWriter out;
  EXPECT_FALSE(state.execute(parseMapCommand("pin del camp"), out));
  EXPECT_TRUE(out.has("ERR pin_write"));
}

TEST(PinConsole, LogPagesNewestFirst) {
  MapConsoleState state;
  FakePinsSource pins;
  state.setPinsSource(&pins);

  // Ten records: five slots filled, then all five moved.
  const char* keys[] = {"base", "parking", "dest", "meet", "camp"};
  for (const char* key : keys) {
    CollectingWriter out;
    std::string cmd = std::string("pin set ") + key + " 48.4 17.0";
    ASSERT_TRUE(state.execute(parseMapCommand(cmd), out)) << cmd;
  }
  for (const char* key : keys) {
    CollectingWriter out;
    std::string cmd = std::string("pin set ") + key + " 48.5 17.1";
    ASSERT_TRUE(state.execute(parseMapCommand(cmd), out)) << cmd;
  }

  CollectingWriter page0;
  state.execute(parseMapCommand("pin log"), page0);
  EXPECT_TRUE(page0.has("INFO pinlog_total=10"));
  EXPECT_TRUE(page0.has("INFO pinlog_offset=0"));
  EXPECT_EQ(page0.countPrefixed("INFO pinlog_1="), 0u) << "seq 1 is the oldest and not on the newest page";
  EXPECT_EQ(page0.countPrefixed("INFO pinlog_10="), 1u);
  EXPECT_EQ(page0.countPrefixed("INFO pinlog_"), 8u + 3u) << "eight records plus total, offset and next";
  EXPECT_TRUE(page0.has("INFO pinlog_next=8"));
  // The newest record is the last replace, and it prints its op and its key.
  EXPECT_EQ(page0.countPrefixed("INFO pinlog_10=rep,camp,48.5000000,17.1000000,0"), 1u);

  CollectingWriter page1;
  state.execute(parseMapCommand("pin log 8"), page1);
  EXPECT_TRUE(page1.has("INFO pinlog_offset=8"));
  EXPECT_TRUE(page1.has("INFO pinlog_next=done"));
  EXPECT_EQ(page1.countPrefixed("INFO pinlog_2="), 1u);
  EXPECT_EQ(page1.countPrefixed("INFO pinlog_1="), 1u);

  CollectingWriter pastEnd;
  state.execute(parseMapCommand("pin log 400"), pastEnd);
  EXPECT_TRUE(pastEnd.has("INFO pinlog_total=10"));
  EXPECT_TRUE(pastEnd.has("INFO pinlog_next=done"));
  EXPECT_EQ(pastEnd.countPrefixed("INFO pinlog_2="), 0u) << "an offset past the end is an empty page, not an error";
}

TEST(PinConsole, ADeleteRecordPrintsWithNoCoordinate) {
  MapConsoleState state;
  FakePinsSource pins;
  state.setPinsSource(&pins);

  CollectingWriter scratch;
  state.execute(parseMapCommand("pin set camp 48.4 17.0"), scratch);
  state.execute(parseMapCommand("pin del camp"), scratch);

  CollectingWriter out;
  state.execute(parseMapCommand("pin log"), out);
  EXPECT_EQ(out.countPrefixed("INFO pinlog_2=del,camp,,,0"), 1u) << "a delete has no position to print";
}
