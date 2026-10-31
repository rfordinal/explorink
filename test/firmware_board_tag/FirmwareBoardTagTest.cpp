#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "FirmwareBoardTag.h"

using board_tag::Scanner;

namespace {

// This test binary is built with FREEINK_DEVICE_X4PRO=1 (CMakeLists.txt), so
// board_tag::TAG is "EXPLORINK-BOARD-V1:x4pro;" -- every test below is
// written against that fixed string, not against the macro.
constexpr const char* kOwnTag = "EXPLORINK-BOARD-V1:x4pro;";
constexpr const char* kOtherBoardTag = "EXPLORINK-BOARD-V1:t5s3pro;";

void feed(Scanner& s, const std::string& data) { s.feed(reinterpret_cast<const uint8_t*>(data.data()), data.size()); }

void feedChunked(Scanner& s, const std::string& data, size_t chunkSize) {
  for (size_t off = 0; off < data.size(); off += chunkSize) {
    const size_t n = std::min(chunkSize, data.size() - off);
    s.feed(reinterpret_cast<const uint8_t*>(data.data() + off), n);
  }
}

}  // namespace

TEST(FirmwareBoardTag, TagMatchesItsOwnBoardName) {
  EXPECT_STREQ(board_tag::TAG, kOwnTag);
  ASSERT_EQ(board_tag::boardNameLen(), strlen("x4pro"));
  EXPECT_EQ(memcmp(board_tag::boardName(), "x4pro", 5), 0);
}

TEST(FirmwareBoardTag, NoTagAtAllIsNotAMismatch) {
  // Plenty of random-ish surrounding bytes, never the magic.
  Scanner s;
  feed(s, "\xE9\x03\x02\x4Fsome esp32 image bytes here, no tag anywhere in this stream at all");
  EXPECT_FALSE(s.mismatch());
  EXPECT_FALSE(s.sawAnyTag());
}

TEST(FirmwareBoardTag, MatchingTagIsNotAMismatchButIsSeen) {
  Scanner s;
  feed(s, std::string("leading junk ") + kOwnTag + " trailing junk");
  EXPECT_FALSE(s.mismatch());
  EXPECT_TRUE(s.sawAnyTag());
}

TEST(FirmwareBoardTag, MismatchedTagIsDetected) {
  Scanner s;
  feed(s, std::string("leading junk ") + kOtherBoardTag + " trailing junk");
  EXPECT_TRUE(s.mismatch());
  EXPECT_TRUE(s.sawAnyTag());
  EXPECT_STREQ(s.foundName(), "t5s3pro");
}

TEST(FirmwareBoardTag, MismatchIsStickyOnceFound) {
  Scanner s;
  feed(s, kOtherBoardTag);
  ASSERT_TRUE(s.mismatch());
  // Further feed() calls are no-ops once mismatch is latched (the caller is
  // expected to stop/abort as soon as mismatch() turns true, but the scanner
  // itself must not un-latch if fed more bytes first).
  feed(s, kOwnTag);
  EXPECT_TRUE(s.mismatch());
  EXPECT_STREQ(s.foundName(), "t5s3pro");
}

TEST(FirmwareBoardTag, MatchingTagThenMismatchedTagStillRejects) {
  // Two tags in one stream: the embedded-tag mechanism only promises "exactly
  // one occurrence" for a well-formed build, but the scanner's own behavior
  // if that's ever violated (or an attacker appends one) should still be to
  // reject, not to let an earlier match wave through a later mismatch.
  Scanner s;
  feed(s, std::string(kOwnTag) + " padding " + kOtherBoardTag);
  EXPECT_TRUE(s.mismatch());
}

TEST(FirmwareBoardTag, ChunkBoundarySplitsTheMagic) {
  const std::string stream = std::string("junk") + kOwnTag + "junk";
  for (size_t chunkSize : {1u, 2u, 3u, 5u, 7u, 11u, 19u, 23u}) {
    Scanner s;
    feedChunked(s, stream, chunkSize);
    EXPECT_FALSE(s.mismatch()) << "chunkSize=" << chunkSize;
    EXPECT_TRUE(s.sawAnyTag()) << "chunkSize=" << chunkSize;
  }
}

TEST(FirmwareBoardTag, ChunkBoundarySplitsAMismatch) {
  const std::string stream = std::string("junk") + kOtherBoardTag + "junk";
  for (size_t chunkSize : {1u, 2u, 3u, 5u, 7u, 11u, 19u, 23u}) {
    Scanner s;
    feedChunked(s, stream, chunkSize);
    EXPECT_TRUE(s.mismatch()) << "chunkSize=" << chunkSize;
  }
}

TEST(FirmwareBoardTag, SelfOverlapInMagicDoesNotFalsePositive) {
  // The magic's first byte ('E') does not recur inside "EXPLORINK-BOARD-V1:",
  // so a partial match followed by a stray 'E' should restart cleanly at 1,
  // not corrupt the match. Construct a stream with a decoy partial prefix.
  Scanner s;
  feed(s, std::string("EXPLOR") + "E" + kOwnTag);  // decoy "EXPLORE" then the real tag
  EXPECT_FALSE(s.mismatch());
  EXPECT_TRUE(s.sawAnyTag());
}

TEST(FirmwareBoardTag, OverlongNameAbortsCaptureAndResyncs) {
  // A name longer than MAX_NAME (23) aborts the capture as "not a real tag"
  // (chance byte-collision on the magic) -- but the byte that triggered the
  // abort must still be tested as a possible start of a REAL tag right after
  // it, not silently dropped. Regression test for that exact bug.
  const std::string fakeOverlongName(30, 'z');  // > MAX_NAME, all printable
  const std::string stream = "EXPLORINK-BOARD-V1:" + fakeOverlongName + ";" + kOwnTag;
  Scanner s;
  feed(s, stream);
  EXPECT_FALSE(s.mismatch());
  EXPECT_TRUE(s.sawAnyTag());
}

TEST(FirmwareBoardTag, OverlongNameAbortsAndTheAbortingByteCanStartARealMismatch) {
  // Same shape as above, but arrange for the byte that aborts the capture to
  // itself be the first byte of a DIFFERENT board's real tag -- this only
  // passes if that byte is re-tested against the magic instead of consumed.
  const std::string fakeOverlongName(30, 'z');
  const std::string stream = "EXPLORINK-BOARD-V1:" + fakeOverlongName + kOtherBoardTag;
  Scanner s;
  feed(s, stream);
  EXPECT_TRUE(s.mismatch());
  EXPECT_STREQ(s.foundName(), "t5s3pro");
}

TEST(FirmwareBoardTag, NonPrintableByteAbortsCaptureAndResyncs) {
  std::string stream = "EXPLORINK-BOARD-V1:abc";
  stream.push_back('\x01');  // non-printable: aborts the in-progress capture
  stream += kOwnTag;
  Scanner s;
  feed(s, stream);
  EXPECT_FALSE(s.mismatch());
  EXPECT_TRUE(s.sawAnyTag());
}

TEST(FirmwareBoardTag, NameExactlyAtMaxLengthIsHandled) {
  // MAX_NAME is 23; a name of exactly that length must not overflow
  // `captured` (24 bytes) nor be treated as overlong.
  const std::string name23(23, 'q');
  const std::string stream = "EXPLORINK-BOARD-V1:" + name23 + ";";
  Scanner s;
  feed(s, stream);
  EXPECT_TRUE(s.mismatch());  // doesn't match "x4pro", but must not crash/corrupt
  EXPECT_STREQ(s.foundName(), name23.c_str());
}

TEST(FirmwareBoardTag, EmptyFeedIsHarmless) {
  Scanner s;
  s.feed(nullptr, 0);
  EXPECT_FALSE(s.mismatch());
  EXPECT_FALSE(s.sawAnyTag());
  feed(s, kOwnTag);
  EXPECT_FALSE(s.mismatch());
  EXPECT_TRUE(s.sawAnyTag());
}
