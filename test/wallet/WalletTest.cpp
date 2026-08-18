#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "WalletAsset.h"
#include "WalletManifestParser.h"

// The two pure halves of the wallet viewer: the 32-byte asset header, and the
// manifest reader. Both run here with no Storage, no renderer and no panel --
// the device-only glue lives in WalletStore.cpp (docs/wallet-viewer.md).

namespace {

using namespace wallet;

// A well-formed header. Field offsets are the format contract; if this builder
// and parseAssetHeader() ever disagree, one of them is wrong.
std::vector<uint8_t> makeHeader(uint8_t type = 1, uint8_t bitDepth = 1, uint8_t col = 0, uint8_t row = 0,
                                uint16_t width = 800, uint16_t height = 480, uint32_t rawLen = 48000,
                                uint32_t version = 7, uint8_t flags = 0, uint8_t presentation = 0) {
  std::vector<uint8_t> h(kAssetHeaderBytes, 0);
  h[0] = 'E';
  h[1] = 'W';
  h[2] = 'A';
  h[3] = '1';
  h[4] = type;
  h[5] = bitDepth;
  h[6] = col;
  h[7] = row;
  h[8] = static_cast<uint8_t>(width & 0xFF);
  h[9] = static_cast<uint8_t>(width >> 8);
  h[10] = static_cast<uint8_t>(height & 0xFF);
  h[11] = static_cast<uint8_t>(height >> 8);
  h[12] = static_cast<uint8_t>(rawLen & 0xFF);
  h[13] = static_cast<uint8_t>((rawLen >> 8) & 0xFF);
  h[14] = static_cast<uint8_t>((rawLen >> 16) & 0xFF);
  h[15] = static_cast<uint8_t>((rawLen >> 24) & 0xFF);
  h[16] = static_cast<uint8_t>(version & 0xFF);
  h[17] = static_cast<uint8_t>((version >> 8) & 0xFF);
  h[18] = static_cast<uint8_t>((version >> 16) & 0xFF);
  h[19] = static_cast<uint8_t>((version >> 24) & 0xFF);
  h[20] = flags;
  h[21] = presentation;
  for (int i = 0; i < 8; ++i) h[24 + i] = static_cast<uint8_t>(0xA0 + i);
  return h;
}

// A two-item manifest. Item 0 has two pages and a 2x2 detail grid plus a 4x4
// one-to-one grid; item 1 has one page and fit only.
const char* kManifest = R"JSON({
  "formatVersion": 1,
  "walletVersion": 7,
  "items": [
    {"id": "item-a", "title": "Passport", "sortOrder": 0,
     "pages": [
       {"id": "p0", "paper": "a4",
        "levels": {
          "fit": {"cols": 1, "rows": 1, "assets": [
            {"assetId": "0123456789abcdef", "type": "FIT", "col": 0, "row": 0, "rawLen": 48000, "sha256": "aa", "rleLen": 12}]},
          "detail": {"cols": 2, "rows": 2, "assets": [
            {"assetId": "1111111111111111", "type": "DETAIL_TILE", "col": 0, "row": 0, "rawLen": 48000},
            {"assetId": "2222222222222222", "type": "DETAIL_TILE", "col": 1, "row": 0, "rawLen": 48000},
            {"assetId": "3333333333333333", "type": "DETAIL_TILE", "col": 0, "row": 1, "rawLen": 48000},
            {"assetId": "4444444444444444", "type": "DETAIL_TILE", "col": 1, "row": 1, "rawLen": 48000}]},
          "one_to_one": {"cols": 4, "rows": 4, "assets": [
            {"assetId": "5555555555555555", "type": "ONE_TO_ONE_TILE", "col": 3, "row": 2, "rawLen": 48000}]}},
        "codes": []},
       {"id": "p1", "paper": "a4",
        "levels": {
          "fit": {"cols": 1, "rows": 1, "assets": [
            {"assetId": "abcdefabcdef0000", "type": "FIT", "col": 0, "row": 0, "rawLen": 48000}]}},
        "codes": []}]},
    {"id": "item-b", "title": "Insurance card", "sortOrder": 1,
     "pages": [
       {"id": "q0", "paper": "id1",
        "levels": {
          "fit": {"cols": 1, "rows": 1, "assets": [
            {"assetId": "9999999999999999", "type": "FIT", "col": 0, "row": 0, "rawLen": 48000}]}},
        "codes": [{"kind": "qr", "value": "x"}]}]}]})JSON";

// Feeds in `chunk`-byte bites, so a test can prove the parse does not depend on
// seeing the document in one piece.
void feedChunked(ManifestParser& parser, const char* json, size_t chunk) {
  const size_t len = std::strlen(json);
  for (size_t at = 0; at < len; at += chunk) {
    const size_t take = (at + chunk <= len) ? chunk : len - at;
    parser.feed(json + at, take);
  }
}

}  // namespace

TEST(WalletAssetHeader, ParsesEveryField) {
  const auto bytes = makeHeader(/*type=*/2, /*bitDepth=*/1, /*col=*/3, /*row=*/1, /*width=*/800, /*height=*/480,
                                /*rawLen=*/48000, /*version=*/9, /*flags=*/0, /*presentation=*/1);
  AssetHeader h;
  ASSERT_TRUE(parseAssetHeader(bytes.data(), bytes.size(), h));
  EXPECT_EQ(h.assetType, AssetType::DetailTile);
  EXPECT_EQ(h.bitDepth, 1);
  EXPECT_EQ(h.tileCol, 3);
  EXPECT_EQ(h.tileRow, 1);
  EXPECT_EQ(h.width, 800);
  EXPECT_EQ(h.height, 480);
  EXPECT_EQ(h.rawLen, 48000u);
  EXPECT_EQ(h.version, 9u);
  EXPECT_EQ(h.flags, 0);
  EXPECT_EQ(h.presentation, 1);
  // Carried, never checked in P1.
  EXPECT_EQ(h.sha256Prefix[0], 0xA0);
  EXPECT_EQ(h.sha256Prefix[7], 0xA7);
}

TEST(WalletAssetHeader, RejectsWrongMagic) {
  auto bytes = makeHeader();
  bytes[3] = '2';
  AssetHeader h;
  EXPECT_FALSE(parseAssetHeader(bytes.data(), bytes.size(), h));
}

TEST(WalletAssetHeader, RejectsShortHeader) {
  const auto bytes = makeHeader();
  AssetHeader h;
  EXPECT_FALSE(parseAssetHeader(bytes.data(), kAssetHeaderBytes - 1, h));
  EXPECT_FALSE(parseAssetHeader(nullptr, kAssetHeaderBytes, h));
}

TEST(WalletAssetHeader, EncryptedFlagIsVisible) {
  const auto bytes = makeHeader(1, 1, 0, 0, 800, 480, 48000, 7, /*flags=*/kFlagEncrypted);
  AssetHeader h;
  ASSERT_TRUE(parseAssetHeader(bytes.data(), bytes.size(), h));
  EXPECT_NE(h.flags & kFlagEncrypted, 0);
}

TEST(WalletAssetPath, MapsAssetIdToTwoLevelPath) {
  char path[kAssetPathBufBytes];
  ASSERT_TRUE(buildAssetPath("0123456789abcdef", path, sizeof(path)));
  EXPECT_STREQ(path, "/trailink/wallet/01/0123456789abcdef.dat");
}

TEST(WalletAssetPath, RejectsAnythingThatIsNotSixteenHex) {
  char path[kAssetPathBufBytes];
  EXPECT_FALSE(buildAssetPath("0123456789abcde", path, sizeof(path)));   // 15
  EXPECT_FALSE(buildAssetPath("0123456789abcdef0", path, sizeof(path))); // 17
  EXPECT_FALSE(buildAssetPath("0123456789abcdeg", path, sizeof(path))); // 'g'
  EXPECT_FALSE(buildAssetPath(nullptr, path, sizeof(path)));
  // The reason the check exists: a manifest must not be able to name a file
  // outside the wallet directory.
  EXPECT_FALSE(buildAssetPath("../../../etc/pw", path, sizeof(path)));
  EXPECT_FALSE(buildAssetPath("0123456789ab/../", path, sizeof(path)));
}

TEST(WalletAssetPath, RejectsABufferThatWouldNotHoldThePath) {
  char tiny[20];
  EXPECT_FALSE(buildAssetPath("0123456789abcdef", tiny, sizeof(tiny)));
}

TEST(WalletManifest, ListsItemsWithPageCounts) {
  ItemEntry items[8];
  ManifestParser parser;
  parser.beginList(items, 8);
  feedChunked(parser, kManifest, 7);

  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(parser.formatVersion(), 1u);
  EXPECT_EQ(parser.walletVersion(), 7u);
  ASSERT_EQ(parser.itemsStored(), 2);
  EXPECT_EQ(parser.itemsSeen(), 2u);
  EXPECT_STREQ(items[0].title, "Passport");
  EXPECT_EQ(items[0].pageCount, 2);
  EXPECT_STREQ(items[1].title, "Insurance card");
  EXPECT_EQ(items[1].pageCount, 1);
}

TEST(WalletManifest, CountsItemsItCouldNotStore) {
  ItemEntry items[1];
  ManifestParser parser;
  parser.beginList(items, 1);
  feedChunked(parser, kManifest, 256);

  EXPECT_EQ(parser.itemsStored(), 1);
  // Seen, not stored: the browse screen says "showing 1 of 2" rather than
  // hiding the second document.
  EXPECT_EQ(parser.itemsSeen(), 2u);
}

TEST(WalletManifest, LookupReturnsGridsForEveryLevel) {
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Fit, 0, 0);
  feedChunked(parser, kManifest, 13);

  const PageLookup& out = parser.lookup();
  ASSERT_TRUE(out.itemFound);
  ASSERT_TRUE(out.pageFound);
  ASSERT_TRUE(out.assetFound);
  EXPECT_STREQ(out.assetId, "0123456789abcdef");
  EXPECT_STREQ(out.title, "Passport");
  EXPECT_EQ(out.pageCount, 2);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Fit)].cols, 1);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Detail)].cols, 2);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Detail)].rows, 2);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::OneToOne)].cols, 4);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::OneToOne)].rows, 4);
}

TEST(WalletManifest, LookupPicksTheRightTile) {
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Detail, 1, 1);
  feedChunked(parser, kManifest, 64);
  EXPECT_TRUE(parser.lookup().assetFound);
  EXPECT_STREQ(parser.lookup().assetId, "4444444444444444");

  ManifestParser other;
  other.beginLookup(0, 0, Level::OneToOne, 3, 2);
  feedChunked(other, kManifest, 64);
  EXPECT_STREQ(other.lookup().assetId, "5555555555555555");
}

TEST(WalletManifest, LookupFindsASecondPageAndASecondItem) {
  ManifestParser parser;
  parser.beginLookup(0, 1, Level::Fit, 0, 0);
  feedChunked(parser, kManifest, 64);
  EXPECT_STREQ(parser.lookup().assetId, "abcdefabcdef0000");

  ManifestParser other;
  other.beginLookup(1, 0, Level::Fit, 0, 0);
  feedChunked(other, kManifest, 64);
  EXPECT_STREQ(other.lookup().assetId, "9999999999999999");
  EXPECT_STREQ(other.lookup().title, "Insurance card");
  EXPECT_EQ(other.lookup().pageCount, 1);
}

TEST(WalletManifest, LookupMissesAreReportedNotGuessed) {
  // A tile the level does not carry.
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::OneToOne, 0, 0);
  feedChunked(parser, kManifest, 64);
  EXPECT_TRUE(parser.lookup().pageFound);
  EXPECT_FALSE(parser.lookup().assetFound);

  // A page the item does not have.
  ManifestParser page;
  page.beginLookup(1, 5, Level::Fit, 0, 0);
  feedChunked(page, kManifest, 64);
  EXPECT_TRUE(page.lookup().itemFound);
  EXPECT_FALSE(page.lookup().pageFound);

  // An item the manifest does not have.
  ManifestParser item;
  item.beginLookup(9, 0, Level::Fit, 0, 0);
  feedChunked(item, kManifest, 64);
  EXPECT_FALSE(item.lookup().itemFound);
}

TEST(WalletManifest, EmptyItemsArrayIsNotAnError) {
  ItemEntry items[4];
  ManifestParser parser;
  parser.beginList(items, 4);
  const char* json = R"({"formatVersion":1,"walletVersion":1,"items":[]})";
  parser.feed(json, std::strlen(json));
  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(parser.formatVersion(), 1u);
  EXPECT_EQ(parser.itemsStored(), 0);
}

TEST(WalletManifest, UnknownLevelIsSkippedNotFatal) {
  const char* json = R"({"formatVersion":1,"items":[{"title":"T","pages":[{"levels":{
      "quarter":{"cols":9,"rows":9,"assets":[{"assetId":"deadbeefdeadbeef","col":0,"row":0}]},
      "fit":{"cols":1,"rows":1,"assets":[{"assetId":"aaaaaaaaaaaaaaaa","col":0,"row":0}]}}}]}]})";
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Fit, 0, 0);
  parser.feed(json, std::strlen(json));
  EXPECT_FALSE(parser.hasError());
  EXPECT_STREQ(parser.lookup().assetId, "aaaaaaaaaaaaaaaa");
}

TEST(WalletManifest, AssetIdThatIsNotSixteenHexIsRefused) {
  const char* json = R"({"formatVersion":1,"items":[{"title":"T","pages":[{"levels":{
      "fit":{"cols":1,"rows":1,"assets":[{"assetId":"../../secret","col":0,"row":0}]}}}]}]})";
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Fit, 0, 0);
  parser.feed(json, std::strlen(json));
  EXPECT_TRUE(parser.lookup().pageFound);
  EXPECT_FALSE(parser.lookup().assetFound);
}

TEST(WalletManifest, TitleTruncationLandsOnACodepointBoundary) {
  // 47 bytes fit; a multi-byte character straddling the cut is dropped whole,
  // not left as half a sequence.
  std::string title;
  for (int i = 0; i < 30; ++i) title += "\xc3\xa1";  // 'a' with acute, 2 bytes each
  const std::string json = std::string(R"({"formatVersion":1,"items":[{"title":")") + title +
                           R"(","pages":[]}]})";
  ItemEntry items[2];
  ManifestParser parser;
  parser.beginList(items, 2);
  parser.feed(json.c_str(), json.size());

  ASSERT_EQ(parser.itemsStored(), 1);
  const size_t len = std::strlen(items[0].title);
  EXPECT_LE(len, kTitleBufBytes - 1);
  EXPECT_EQ(len % 2, 0u) << "cut inside a 2-byte sequence";
}

TEST(WalletLevels, ConfirmCyclesFitDetailOneToOneFit) {
  EXPECT_EQ(nextLevel(Level::Fit), Level::Detail);
  EXPECT_EQ(nextLevel(Level::Detail), Level::OneToOne);
  EXPECT_EQ(nextLevel(Level::OneToOne), Level::Fit);
}
