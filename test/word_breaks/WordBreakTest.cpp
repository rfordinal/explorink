// What Hyphenator still does after the Liang pattern tries were deleted
// (2026-11-11, xteink docs/firmware-cleanup-plan.md step 3).
//
// The suite that used to cover this file scored ten language tries against
// pyphen and went with them. Nothing then covered the part that survived, which
// is the part still compiled into every build -- so this pins it instead:
// breaks on marks already present in the text, and the blind fallback.
//
// The tests are written as "what a caller gets", not as "what the private
// helpers do", so a later rewrite of the internals is free as long as the
// answers hold.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Epub/hyphenation/Hyphenator.h"

namespace {

constexpr bool kWithFallback = true;
constexpr bool kNoFallback = false;

std::vector<size_t> offsets(const std::string& word, const bool includeFallback) {
  std::vector<size_t> out;
  for (const auto& info : Hyphenator::breakOffsets(word, includeFallback)) {
    out.push_back(info.byteOffset);
  }
  return out;
}

// Renders every break the hyphenator offers as a visible string, so a failure
// message says what the split would look like instead of printing offsets.
std::string rendered(const std::string& word, const bool includeFallback) {
  std::string out;
  size_t prev = 0;
  for (const auto& info : Hyphenator::breakOffsets(word, includeFallback)) {
    out += word.substr(prev, info.byteOffset - prev);
    out += info.requiresInsertedHyphen ? "|-" : "|";
    prev = info.byteOffset;
  }
  out += word.substr(prev);
  return out;
}

// --- marks already in the text -------------------------------------------

TEST(WordBreaks, HardHyphenBreaksWithoutInsertingASecondOne) {
  const auto breaks = Hyphenator::breakOffsets("US-Satellitensystems", kNoFallback);
  ASSERT_EQ(breaks.size(), 1u);
  // After the '-', so the rendered line keeps the hyphen it already has.
  EXPECT_EQ(breaks[0].byteOffset, 3u);
  EXPECT_FALSE(breaks[0].requiresInsertedHyphen);
}

TEST(WordBreaks, HardHyphenIsTheOnlyBreakNowThatPatternsAreGone) {
  // While the tries were here this word also broke inside "Satellitensystems"
  // (Sa|tel|li|ten|sys|tems). It does not any more, and that is the deliberate
  // cost of the strip rather than a regression to chase.
  EXPECT_EQ(rendered("US-Satellitensystems", kNoFallback), "US-|Satellitensystems");
}

TEST(WordBreaks, SoftHyphenBreaksAndAsksForAVisibleHyphen) {
  // U+00AD between the two 'l's: invisible until the break is taken.
  const auto breaks = Hyphenator::breakOffsets("Satel­liten", kNoFallback);
  ASSERT_EQ(breaks.size(), 1u);
  EXPECT_TRUE(breaks[0].requiresInsertedHyphen);
}

TEST(WordBreaks, AHyphenThatIsNotBetweenTwoLettersIsNotABreak) {
  // A trailing dash, a leading dash and a dash next to a digit are punctuation,
  // not a place to wrap a line.
  EXPECT_TRUE(offsets("-word", kNoFallback).empty());
  EXPECT_TRUE(offsets("word-", kNoFallback).empty());
  EXPECT_TRUE(offsets("1-2", kNoFallback).empty());
}

TEST(WordBreaks, ApostropheContractionBreaksWhenBothSidesAreLongEnough) {
  // all'improvviso: 3 letters left, well over 3 right.
  const auto breaks = Hyphenator::breakOffsets("all'improvviso", kNoFallback);
  ASSERT_EQ(breaks.size(), 1u);
  EXPECT_FALSE(breaks[0].requiresInsertedHyphen);
  EXPECT_EQ(rendered("all'improvviso", kNoFallback), "all'|improvviso");
}

TEST(WordBreaks, ShortCliticsAndContractionTailsAreNotStranded) {
  // "l'" would leave one letter on the line; "'ve" would start one with two.
  EXPECT_TRUE(offsets("l'improvviso", kNoFallback).empty());
  EXPECT_TRUE(offsets("they've", kNoFallback).empty());
}

// --- the blind fallback ---------------------------------------------------

TEST(WordBreaks, AnOrdinaryWordOffersNoBreakUnlessTheCallerAsksForFallback) {
  // This is the whole behaviour change: with the tries gone there is no
  // language knowledge left, so a plain word is unsplittable until the caller
  // says it would rather split badly than overflow the line.
  EXPECT_TRUE(offsets("Quadratkilometer", kNoFallback).empty());
  EXPECT_FALSE(offsets("Quadratkilometer", kWithFallback).empty());
}

TEST(WordBreaks, FallbackKeepsTwoCodepointsOnEachSide) {
  // TeX's 2/2 default. "abcdef" -> breaks before c, d and e, never before b
  // (one letter left behind) and never before f (one letter carried over).
  EXPECT_EQ(rendered("abcdef", kWithFallback), "ab|-c|-d|-ef");
}

TEST(WordBreaks, FallbackCannotSplitAWordWithNoRoomForIt) {
  // Four codepoints is the shortest word the 2/2 rule can split at all.
  EXPECT_TRUE(offsets("abc", kWithFallback).empty());
  EXPECT_EQ(offsets("abcd", kWithFallback).size(), 1u);
}

TEST(WordBreaks, FallbackCountsCodepointsRatherThanBytes) {
  // Each of these is two bytes in UTF-8. A byte-counting fallback would offer
  // a break in the middle of a character and corrupt the render.
  const std::string word = "ťažkosti";  // 8 codepoints, 10 bytes
  for (const size_t offset : offsets(word, kWithFallback)) {
    // A UTF-8 continuation byte is 10xxxxxx; no break may land on one.
    ASSERT_LT(offset, word.size());
    EXPECT_NE(static_cast<unsigned char>(word[offset]) & 0xC0, 0x80) << "break at byte " << offset << " splits a character";
  }
}

TEST(WordBreaks, EmptyAndSingleCharacterWordsAreSafe) {
  EXPECT_TRUE(offsets("", kWithFallback).empty());
  EXPECT_TRUE(offsets("a", kWithFallback).empty());
  EXPECT_TRUE(offsets("", kNoFallback).empty());
}

TEST(WordBreaks, BreaksComeBackSortedAndWithoutDuplicates) {
  // The line-breaker walks them in order and takes the widest prefix that fits,
  // so an unsorted or doubled list silently picks the wrong split.
  const auto breaks = Hyphenator::breakOffsets("well-known'ish", kWithFallback);
  for (size_t i = 1; i < breaks.size(); ++i) {
    EXPECT_LT(breaks[i - 1].byteOffset, breaks[i].byteOffset);
  }
}

TEST(WordBreaks, EveryOffsetIsInsideTheWord) {
  // A break at 0 or at the end is not a break; the caller would produce an
  // empty line or an empty remainder from it.
  for (const char* word : {"Quadratkilometer", "all'improvviso", "US-Satellitensystems", "ťažkosti"}) {
    for (const bool fallback : {kNoFallback, kWithFallback}) {
      const std::string w{word};
      for (const size_t offset : offsets(w, fallback)) {
        EXPECT_GT(offset, 0u) << w;
        EXPECT_LT(offset, w.size()) << w;
      }
    }
  }
}

}  // namespace
