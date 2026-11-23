#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "DayFile.h"
#include "LoraEventLine.h"

namespace {

std::string encode(const LoraLogEvent& ev, uint32_t utc = 1761200000u, uint32_t boot = 7u) {
  char buf[kLoraLogLineMax + 1];
  const size_t len = encodeLoraLogEvent(ev, utc, boot, buf, sizeof(buf));
  EXPECT_GT(len, 0u);
  return std::string(buf, len);
}

// Field by index, so a test names the column it cares about instead of
// matching a whole line -- a new column then breaks one assertion rather than
// every one of them.
std::string field(const std::string& line, size_t index) {
  size_t start = 0;
  for (size_t i = 0; i < index; ++i) {
    const size_t comma = line.find(',', start);
    if (comma == std::string::npos) return "";
    start = comma + 1;
  }
  const size_t comma = line.find(',', start);
  return line.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
}

constexpr size_t kUtc = 0, kUptime = 1, kBoot = 2, kKind = 3, kKey = 4, kName = 5;
constexpr size_t kRssi = 6, kSnr = 7, kLat = 8, kLon = 9, kDetail = 10;

LoraLogEvent rxEvent() {
  LoraLogEvent ev{};
  ev.kind = LoraLogKind::AdvertRx;
  const uint8_t key[8] = {0x3e, 0x8f, 0x8e, 0x75, 0xa6, 0xb2, 0x0b, 0xd0};
  memcpy(ev.key, key, sizeof(key));
  ev.hasKey = true;
  snprintf(ev.name, sizeof(ev.name), "RF-alfa");
  ev.rssiDbm = -92;
  ev.snrDb = -11;
  ev.uptimeMs = 123456;
  return ev;
}

TEST(LoraEventLine, ATransmitRowSaysWhichRuleSentIt) {
  // Four rules put the same position on the air. Without the detail column a
  // hardware pass cannot tell "the rider stopped" from "the rider moved
  // 100 m", which is the question the pass exists to answer.
  LoraLogEvent ev{};
  ev.kind = LoraLogKind::AdvertTx;
  ev.txReason = LoraTxReason::Settled;
  snprintf(ev.name, sizeof(ev.name), "RF-alfa");
  ev.hasLoc = true;
  ev.latE6 = 48377062;
  ev.lonE6 = 17590000;
  ev.uptimeMs = 1000;

  char buf[kLoraLogLineMax] = {};
  ASSERT_GT(encodeLoraLogEvent(ev, 1792751919, 7, buf, sizeof(buf)), 0u);
  EXPECT_NE(std::string(buf).find(",settled"), std::string::npos) << buf;
}

TEST(LoraEventLine, AManualAdvertIsAttributedToTheButtonAndNotToARule) {
  // Three callers transmit. A row that cannot be attributed to a mechanism
  // cannot answer "is the cadence working".
  LoraLogEvent ev{};
  ev.kind = LoraLogKind::AdvertTx;
  ev.txReason = LoraTxReason::Manual;
  ev.uptimeMs = 1000;

  char buf[kLoraLogLineMax] = {};
  ASSERT_GT(encodeLoraLogEvent(ev, 0, 7, buf, sizeof(buf)), 0u);
  EXPECT_NE(std::string(buf).find(",manual"), std::string::npos) << buf;
}

TEST(LoraEventLine, EveryOtherRowKeepsAnEmptyDetailColumn) {
  // The detail column is shared, so a reason leaking onto a receive row would
  // be a format change nobody asked for.
  LoraLogEvent ev{};
  ev.kind = LoraLogKind::AdvertRx;
  ev.txReason = LoraTxReason::Moved;  // ignored: not a transmit row
  ev.uptimeMs = 1000;

  char buf[kLoraLogLineMax] = {};
  ASSERT_GT(encodeLoraLogEvent(ev, 0, 7, buf, sizeof(buf)), 0u);
  const std::string line(buf);
  EXPECT_EQ(line.back(), ',') << line;
}

TEST(LoraEventLine, TheConsoleWordAndTheFileWordAreTheSame) {
  // LORA_STATE prints why= from this, and a console reading that disagreed
  // with the file would be two answers to one question.
  EXPECT_STREQ(loraTxReasonText(LoraTxReason::Settled), "settled");
  EXPECT_STREQ(loraTxReasonText(LoraTxReason::Keepalive), "keepalive");
  EXPECT_STREQ(loraTxReasonText(LoraTxReason::None), "");
}

}  // namespace

TEST(LoraEventLine, HeaderNamesEveryColumnTheEncoderWrites) {
  const LoraLogEvent ev = rxEvent();
  const std::string line = encode(ev);
  const auto count = [](const std::string& s) { return std::count(s.begin(), s.end(), ',') + 1; };
  EXPECT_EQ(count(line), count(kLoraLogCsvHeader));
}

TEST(LoraEventLine, AReceivedAdvertCarriesKeySignalAndName) {
  const std::string line = encode(rxEvent());
  EXPECT_EQ(field(line, kKind), "rx");
  EXPECT_EQ(field(line, kKey), "3e8f8e75a6b20bd0");
  EXPECT_EQ(field(line, kName), "RF-alfa");
  EXPECT_EQ(field(line, kRssi), "-92");
  EXPECT_EQ(field(line, kSnr), "-11");
  EXPECT_EQ(field(line, kUptime), "123456");
  EXPECT_EQ(field(line, kBoot), "7");
}

// The id a roster entry is matched on is `lora-` plus these same eight bytes,
// so a row here joins a row in the black box by eye. If the width ever
// changes, the two stop lining up silently.
TEST(LoraEventLine, TheKeyIsTheSameEightBytesTheRosterIdUses) { EXPECT_EQ(field(encode(rxEvent()), kKey).size(), 16u); }

TEST(LoraEventLine, APositionlessAdvertLeavesTheCoordinateColumnsEmpty) {
  const std::string line = encode(rxEvent());
  EXPECT_EQ(field(line, kLat), "");
  EXPECT_EQ(field(line, kLon), "");
}

TEST(LoraEventLine, APositionIsWrittenAtSixDecimals) {
  LoraLogEvent ev = rxEvent();
  ev.hasLoc = true;
  ev.latE6 = 48123456;
  ev.lonE6 = -17654321;
  const std::string line = encode(ev);
  EXPECT_EQ(field(line, kLat), "48.123456");
  EXPECT_EQ(field(line, kLon), "-17.654321");
}

// Zero would read as a node and as a perfect link. Empty reads as "this row
// has nothing to say here", which is what it means.
TEST(LoraEventLine, AFrameThatDidNotParseHasNoKeyAndNoName) {
  LoraLogEvent ev{};
  ev.kind = LoraLogKind::AdvertBad;
  ev.rssiDbm = -103;
  ev.snrDb = -14;
  const std::string line = encode(ev);
  EXPECT_EQ(field(line, kKind), "rxbad");
  EXPECT_EQ(field(line, kKey), "");
  EXPECT_EQ(field(line, kName), "");
  EXPECT_EQ(field(line, kRssi), "-103");
}

TEST(LoraEventLine, ATransmitHasNoSignalColumns) {
  LoraLogEvent ev{};
  ev.kind = LoraLogKind::AdvertTx;
  snprintf(ev.name, sizeof(ev.name), "RF-alfa");
  ev.rssiDbm = -1;  // must not reach the row: we did not receive anything
  ev.snrDb = -1;
  const std::string line = encode(ev);
  EXPECT_EQ(field(line, kKind), "tx");
  EXPECT_EQ(field(line, kRssi), "");
  EXPECT_EQ(field(line, kSnr), "");
}

TEST(LoraEventLine, ADeclinedTransmitNamesItsReason) {
  LoraLogEvent ev{};
  ev.kind = LoraLogKind::TxDeclined;
  ev.decline = LoraTxDecline::NoNews;
  EXPECT_EQ(field(encode(ev), kDetail), "no_news");
  ev.decline = LoraTxDecline::PoolEmpty;
  EXPECT_EQ(field(encode(ev), kDetail), "pool_empty");
}

TEST(LoraEventLine, TheRadioComingUpCarriesTheCadence) {
  LoraLogEvent ev{};
  ev.kind = LoraLogKind::RadioUp;
  ev.cadenceSec = 70;
  EXPECT_EQ(field(encode(ev), kDetail), "bcn70s");
  ev.cadenceSec = 0;
  EXPECT_EQ(field(encode(ev), kDetail), "rx_only");
}

TEST(LoraEventLine, ARefusedPositionKeepsTheKeySoItCanBeAddedToTheRoster) {
  LoraLogEvent ev{};
  ev.kind = LoraLogKind::PosRefused;
  const uint8_t key[8] = {0xc6, 0xc2, 0x8a, 0x10, 0x00, 0x11, 0x22, 0x33};
  memcpy(ev.key, key, sizeof(key));
  ev.hasKey = true;
  ev.hasLoc = true;
  ev.latE6 = 1000000;
  ev.lonE6 = 2000000;
  const std::string line = encode(ev);
  EXPECT_EQ(field(line, kKind), "refused");
  EXPECT_EQ(field(line, kKey), "c6c28a1000112233");
  EXPECT_EQ(field(line, kLat), "1.000000");
}

// A zero there would read as 1970 rather than as "this device has no clock",
// which is the same rule GroupRecord lives under.
TEST(LoraEventLine, NoClockLeavesTheUtcColumnEmptyRatherThanZero) {
  EXPECT_EQ(field(encode(rxEvent(), 0u), kUtc), "");
  EXPECT_EQ(field(encode(rxEvent(), 1761200000u), kUtc), "1761200000");
}

// The name arrives over the air from a stranger and nobody validated it. A
// comma would turn one event into two rows; a newline would turn it into two
// lines, and every column after it would be read as the wrong field.
TEST(LoraEventLine, AHostileNameCannotBreakTheRow) {
  LoraLogEvent ev = rxEvent();
  snprintf(ev.name, sizeof(ev.name), "a,b\nc");
  const std::string line = encode(ev);
  EXPECT_EQ(field(line, kName), "a_b_c");
  EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST(LoraEventLine, ATooSmallBufferWritesNothingRatherThanTruncating) {
  char buf[8];
  EXPECT_EQ(encodeLoraLogEvent(rxEvent(), 1761200000u, 7u, buf, sizeof(buf)), 0u);
}

TEST(DayFileNames, ADatedDayAndAClocklessOneAreDifferentFiles) {
  char name[20];
  ASSERT_TRUE(dayFileName("ev", 1761200000u, name, sizeof(name)));
  EXPECT_STREQ(name, "ev-20251023.csv");
  ASSERT_TRUE(dayFileName("ev", 0u, name, sizeof(name)));
  EXPECT_STREQ(name, "ev-noclock.csv");
}

// The black box and the event log must keep naming the same day the same way,
// or two files from one walk sort into different days on a laptop.
TEST(DayFileNames, BothLogsNameTheSameDay) {
  char ev[20];
  char bb[20];
  ASSERT_TRUE(dayFileName("ev", 1761200000u, ev, sizeof(ev)));
  ASSERT_TRUE(dayFileName("bb", 1761200000u, bb, sizeof(bb)));
  EXPECT_EQ(std::string(ev).substr(2), std::string(bb).substr(2));
}

TEST(DayFileNames, ABufferTooSmallIsRefusedRatherThanTruncated) {
  char name[8];
  EXPECT_FALSE(dayFileName("ev", 1761200000u, name, sizeof(name)));
}
