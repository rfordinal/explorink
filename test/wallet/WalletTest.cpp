#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "WalletAsset.h"
#include "WalletCrypto.h"
#include "WalletCryptoHost.h"
#include "WalletGreySynth.h"
#include "WalletManifestParser.h"
#include "WalletSha256.h"
#include "WalletSidecar.h"
#include "builtinFonts/ubuntu_10_regular.h"

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
  EXPECT_FALSE(buildAssetPath("0123456789abcde", path, sizeof(path)));    // 15
  EXPECT_FALSE(buildAssetPath("0123456789abcdef0", path, sizeof(path)));  // 17
  EXPECT_FALSE(buildAssetPath("0123456789abcdeg", path, sizeof(path)));   // 'g'
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

TEST(WalletManifest, GreyIsAPropertyOfTheDocument) {
  // Grey costs 2,604 ms a frame against ~570 ms for 1bpp (measured on the panel,
  // docs/wallet-grey.md), so it is per document and not a card-wide mode: a scan
  // wants it, a page of text the rider pans does not. The device follows the
  // manifest; CMD:WALLETGREY is a lab override on top.
  const char* greyDoc = R"({"formatVersion":1,"items":[{"title":"Scan","grey":true,"pages":[{"levels":{
      "fit":{"cols":1,"rows":1,"assets":[{"assetId":"0123456789abcdef","type":1,"col":0,"row":0}]}}}]}]})";
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Fit, 0, 0);
  feedChunked(parser, greyDoc, 11);
  EXPECT_TRUE(parser.lookup().wantsGrey);
}

TEST(WalletManifest, ADocumentWithoutTheFlagAsksForNoGrey) {
  // Absent means false. A card written before grey existed must not start
  // rendering grey frames because a later firmware learned how.
  const char* plainDoc = R"({"formatVersion":1,"items":[{"title":"Text","pages":[{"levels":{
      "fit":{"cols":1,"rows":1,"assets":[{"assetId":"0123456789abcdef","type":1,"col":0,"row":0}]}}}]}]})";
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Fit, 0, 0);
  feedChunked(parser, plainDoc, 11);
  EXPECT_FALSE(parser.lookup().wantsGrey);
}

TEST(WalletManifest, TheGreyFlagIsReadFromTheWantedItemOnly) {
  // Two documents, only the second is grey. Asking for item 0 must not inherit
  // item 1's answer -- the parser walks one stream and the flag is per item.
  const char* twoDocs = R"({"formatVersion":1,"items":[
      {"title":"Text","pages":[{"levels":{"fit":{"cols":1,"rows":1,"assets":[
        {"assetId":"0123456789abcdef","type":1,"col":0,"row":0}]}}}]},
      {"title":"Scan","grey":true,"pages":[{"levels":{"fit":{"cols":1,"rows":1,"assets":[
        {"assetId":"fedcba9876543210","type":1,"col":0,"row":0}]}}}]}]})";
  ManifestParser first;
  first.beginLookup(0, 0, Level::Fit, 0, 0);
  feedChunked(first, twoDocs, 17);
  EXPECT_FALSE(first.lookup().wantsGrey) << "item 0 is not the grey one";

  ManifestParser second;
  second.beginLookup(1, 0, Level::Fit, 0, 0);
  feedChunked(second, twoDocs, 17);
  EXPECT_TRUE(second.lookup().wantsGrey) << "item 1 is";
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
  const std::string json = std::string(R"({"formatVersion":1,"items":[{"title":")") + title + R"(","pages":[]}]})";
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

// ---------------------------------------------------------------------------
// The panel a wallet was built for
// ---------------------------------------------------------------------------

TEST(WalletPanel, AbsentPanelObjectMatchesAnything) {
  // Trees generated before the field existed. Treated as the live panel, with
  // the per-asset header check as the real gate.
  const DeclaredPanel none;
  EXPECT_TRUE(panelMatches(none, kPanelX4));
  EXPECT_TRUE(panelMatches(none, kPanelX3));
}

TEST(WalletPanel, DeclaredPanelMustMatchEveryFieldItStates) {
  DeclaredPanel x4;
  x4.present = true;
  std::strcpy(x4.name, "x4");
  x4.width = 800;
  x4.height = 480;
  x4.rowBytes = 100;
  x4.assetBytes = 48000;
  EXPECT_TRUE(panelMatches(x4, kPanelX4));
  EXPECT_FALSE(panelMatches(x4, kPanelX3));

  // A field left at 0 declared nothing and is not compared.
  DeclaredPanel partial;
  partial.present = true;
  partial.assetBytes = 52272;
  EXPECT_FALSE(panelMatches(partial, kPanelX4));
  EXPECT_TRUE(panelMatches(partial, kPanelX3));
}

TEST(WalletPanel, ParsedOutOfTheManifest) {
  const char* json = R"({"formatVersion":1,"walletVersion":3,
    "panel":{"name":"x3","width":792,"height":528,"rowBytes":99,"assetBytes":52272},
    "items":[]})";
  ManifestParser parser;
  ItemEntry items[1];
  parser.beginList(items, 1);
  parser.feed(json, std::strlen(json));

  ASSERT_TRUE(parser.panel().present);
  EXPECT_STREQ(parser.panel().name, "x3");
  EXPECT_EQ(parser.panel().width, 792);
  EXPECT_EQ(parser.panel().height, 528);
  EXPECT_EQ(parser.panel().rowBytes, 99);
  EXPECT_EQ(parser.panel().assetBytes, 52272u);
  EXPECT_FALSE(panelMatches(parser.panel(), kPanelX4));
  EXPECT_TRUE(panelMatches(parser.panel(), kPanelX3));
}

TEST(WalletAssetGate, AcceptsItsOwnPanelAndRefusesTheOther) {
  const auto x4Asset = makeHeader(1, 1, 0, 0, 800, 480, 48000);
  AssetHeader h;
  EXPECT_EQ(checkAssetForPanel(x4Asset.data(), x4Asset.size(), kPanelX4, h), AssetCheck::Ok);
  EXPECT_EQ(checkAssetForPanel(x4Asset.data(), x4Asset.size(), kPanelX3, h), AssetCheck::WrongPanel);

  const auto x3Asset = makeHeader(1, 1, 0, 0, 792, 528, 52272);
  EXPECT_EQ(checkAssetForPanel(x3Asset.data(), x3Asset.size(), kPanelX3, h), AssetCheck::Ok);
  EXPECT_EQ(checkAssetForPanel(x3Asset.data(), x3Asset.size(), kPanelX4, h), AssetCheck::WrongPanel);
}

TEST(WalletAssetGate, NamesTheReasonItRefused) {
  AssetHeader h;
  auto bad = makeHeader();
  bad[0] = 'X';
  EXPECT_EQ(checkAssetForPanel(bad.data(), bad.size(), kPanelX4, h), AssetCheck::Malformed);

  const auto enc = makeHeader(1, 1, 0, 0, 800, 480, 48000, 7, kFlagEncrypted);
  EXPECT_EQ(checkAssetForPanel(enc.data(), enc.size(), kPanelX4, h), AssetCheck::Encrypted);

  const auto grey = makeHeader(1, /*bitDepth=*/2);
  EXPECT_EQ(checkAssetForPanel(grey.data(), grey.size(), kPanelX4, h), AssetCheck::BitDepth);
}

TEST(WalletAssetPath, RootIsAParameterSoTheHostToolWalksTheSameMapping) {
  char path[128];
  ASSERT_TRUE(buildAssetPathIn("/tmp/tree", "3acec0373d3c4ba0", path, sizeof(path)));
  EXPECT_STREQ(path, "/tmp/tree/3a/3acec0373d3c4ba0.dat");
  // And the card root is the same function with one argument bound.
  char card[kAssetPathBufBytes];
  ASSERT_TRUE(buildAssetPath("3acec0373d3c4ba0", card, sizeof(card)));
  EXPECT_STREQ(card, "/trailink/wallet/3a/3acec0373d3c4ba0.dat");
}

// ---------------------------------------------------------------------------
// Against bytes a real generator wrote
//
// fixtures/manifest.json and fixtures/3a/3acec0373d3c4ba0.rle are verbatim
// output of `tools/walletgen.py --demo --paper a4` (parent repo). Nothing here
// is hand-authored, which is the point: the format was implemented twice from
// one written contract -- once in that generator, once in this firmware -- and
// these cases are where the two meet. See docs/wallet-viewer.md, "Read against
// real generator output".
//
// The sidecar is committed instead of the 48 KB .dat because it is 17 KB and
// carries the asset's 32-byte header verbatim as a prefix. The firmware does
// not decode EWRL in P1, so the decoder below is test scaffolding only.
// ---------------------------------------------------------------------------

namespace {

std::string fixture(const char* name) { return std::string(WALLET_FIXTURES_DIR) + "/" + name; }

std::vector<uint8_t> readFixture(const char* name) {
  const std::string path = fixture(name);
  FILE* fh = std::fopen(path.c_str(), "rb");
  if (fh == nullptr) return {};
  std::vector<uint8_t> out;
  uint8_t buf[4096];
  for (;;) {
    const size_t got = std::fread(buf, 1, sizeof(buf), fh);
    if (got == 0) break;
    out.insert(out.end(), buf, buf + got);
  }
  std::fclose(fh);
  return out;
}

// Panel-native read, exactly as GfxRenderer writes a pixel
// (lib/GfxRenderer/GfxRenderer.cpp:517-524): MSB first, bit 1 = white.
bool physWhite(const std::vector<uint8_t>& fb, int rowBytes, int phyX, int phyY) {
  return ((fb[static_cast<size_t>(phyY) * rowBytes + phyX / 8] >> (7 - (phyX % 8))) & 1u) != 0;
}

// Logical portrait read: logical (x,y) -> physical (y, panelHeight - 1 - x).
// GfxRenderer::rotateCoordinates(), Portrait (GfxRenderer.cpp:216-223).
bool logicalWhite(const std::vector<uint8_t>& fb, const PanelGeometry& panel, int x, int y) {
  return physWhite(fb, panel.rowBytes, y, panel.height - 1 - x);
}

int logicalBandInk(const std::vector<uint8_t>& fb, const PanelGeometry& panel, int x0, int x1, int y0, int y1) {
  int inked = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      if (!logicalWhite(fb, panel, x, y)) ++inked;
    }
  }
  return inked;
}

}  // namespace

TEST(WalletGeneratedTree, ManifestListsTheDemoItem) {
  const std::vector<uint8_t> raw = readFixture("manifest.json");
  ASSERT_FALSE(raw.empty()) << "fixtures/manifest.json missing";

  ItemEntry items[4];
  ManifestParser parser;
  parser.beginList(items, 4);
  parser.feed(reinterpret_cast<const char*>(raw.data()), raw.size());

  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(parser.formatVersion(), 1u);
  EXPECT_EQ(parser.walletVersion(), 1u);
  ASSERT_EQ(parser.itemsStored(), 1);
  EXPECT_STREQ(items[0].title, "Demo A4 Page");
  EXPECT_EQ(items[0].pageCount, 1);
  // This tree predates the panel field. Nothing refuses it for that.
  EXPECT_FALSE(parser.panel().present);
  EXPECT_TRUE(panelMatches(parser.panel(), kPanelX4));
}

TEST(WalletGeneratedTree, LookupFindsTheRealAssetIdsAndGrids) {
  const std::vector<uint8_t> raw = readFixture("manifest.json");
  ASSERT_FALSE(raw.empty());

  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Fit, 0, 0);
  parser.feed(reinterpret_cast<const char*>(raw.data()), raw.size());

  const PageLookup& out = parser.lookup();
  ASSERT_TRUE(out.assetFound);
  // The id the generator derived for this demo page. Committed with the tree, so
  // a change in either id derivation shows up here.
  EXPECT_STREQ(out.assetId, "3acec0373d3c4ba0");
  // A4 at 217 PPI: one FIT screen, a 2x2 DETAIL grid, a 4x4 1:1 grid.
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Fit)].cols, 1);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Fit)].rows, 1);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Detail)].cols, 2);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Detail)].rows, 2);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::OneToOne)].cols, 4);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::OneToOne)].rows, 4);
}

TEST(WalletGeneratedTree, TheAssetIdMapsToTheFileTheGeneratorWrote) {
  char path[512];
  ASSERT_TRUE(buildAssetPathIn(WALLET_FIXTURES_DIR, "3acec0373d3c4ba0", path, sizeof(path)));
  // The .dat is not committed (48 KB); the sidecar sits beside it under the same
  // shard and stem, which is what this asserts.
  const std::string dat(path);
  ASSERT_GT(dat.size(), 4u);
  const std::string rle = dat.substr(0, dat.size() - 4) + ".rle";
  FILE* fh = std::fopen(rle.c_str(), "rb");
  EXPECT_NE(fh, nullptr) << rle;
  if (fh != nullptr) std::fclose(fh);
}

TEST(WalletGeneratedTree, HeaderTheGeneratorWroteParsesFieldForField) {
  const std::vector<uint8_t> blob = readFixture("3a/3acec0373d3c4ba0.rle");
  ASSERT_GE(blob.size(), kAssetHeaderBytes);

  AssetHeader h;
  ASSERT_TRUE(parseAssetHeader(blob.data(), blob.size(), h));
  EXPECT_EQ(h.assetType, AssetType::Fit);
  EXPECT_EQ(h.bitDepth, 1);
  EXPECT_EQ(h.tileCol, 0);
  EXPECT_EQ(h.tileRow, 0);
  EXPECT_EQ(h.width, 800);
  EXPECT_EQ(h.height, 480);
  EXPECT_EQ(h.rawLen, 48000u);
  EXPECT_EQ(h.flags, 0);
  // The generator lays the document out upright for a device held in portrait.
  EXPECT_EQ(h.presentation, 1);

  EXPECT_EQ(checkAssetForPanel(blob.data(), blob.size(), kPanelX4, h), AssetCheck::Ok);
  EXPECT_EQ(checkAssetForPanel(blob.data(), blob.size(), kPanelX3, h), AssetCheck::WrongPanel);
}

TEST(WalletGeneratedTree, PayloadDrawsAnUprightPageWithInkAsBlack) {
  const std::vector<uint8_t> blob = readFixture("3a/3acec0373d3c4ba0.rle");
  ASSERT_GE(blob.size(), kAssetHeaderBytes);
  std::vector<uint8_t> fb;
  ASSERT_TRUE(wallet::host::decodeSidecarPayload(blob, fb));
  ASSERT_EQ(fb.size(), 48000u);

  const PanelGeometry panel = kPanelX4;
  const int total = panel.width * panel.height;
  const int inked = logicalBandInk(fb, panel, 0, panel.height, 0, panel.width);

  // 1. Polarity. A page of text inks a few per cent. If bit 1 meant ink instead
  //    of white, this would be the complement -- over 90 %.
  EXPECT_GT(inked, total / 100) << "blank: nothing decoded as ink";
  EXPECT_LT(inked, total / 5) << "mostly black: polarity is inverted";

  // 2. Margins. The top and bottom 10 % of the page carry no ink at all. Read
  //    with a wrong byte order or a transposed row stride, ink lands here.
  EXPECT_EQ(logicalBandInk(fb, panel, 0, 480, 0, 80), 0) << "ink in the top margin";
  EXPECT_EQ(logicalBandInk(fb, panel, 0, 480, 720, 800), 0) << "ink in the bottom margin";

  // 3. Which way up. The title band is dense; the footer band near the bottom is
  //    a single small line. A 180-degree flip swaps them.
  const int titleBand = logicalBandInk(fb, panel, 0, 480, 80, 140);
  const int footerBand = logicalBandInk(fb, panel, 0, 480, 600, 720);
  EXPECT_GT(titleBand, 500);
  EXPECT_LT(footerBand, 200);
  EXPECT_GT(titleBand, footerBand * 5) << "page is upside down";

  // 4. Which way round. The left column band carries the text block's start; the
  //    right margin is nearly empty. A mirror swaps them.
  const int leftBand = logicalBandInk(fb, panel, 0, 60, 0, 800);
  const int rightBand = logicalBandInk(fb, panel, 420, 480, 0, 800);
  EXPECT_GT(leftBand, rightBand * 5) << "page is mirrored left to right";

  // 5. Bit order inside a byte. None of the checks above sees it: reversing the
  //    bits of every byte keeps the margins white and the bands dense, it only
  //    scrambles pixels within each group of eight. What does see it is
  //    correlation. Adjacent pixels agree far more often than pixels seven apart,
  //    so compare the transition rate across a byte boundary (bit 7 next to the
  //    next byte's bit 0) with the rate inside bytes. Equal-ish when the order is
  //    right; the boundary rate jumps when it is reversed, because the pixels
  //    that end up adjacent were seven apart.
  //
  //    Measured on this fixture: 0.887 correct, 1.254 with every byte reversed.
  //    The 1.05 threshold sits between them with room on both sides. A heuristic,
  //    and fixture-specific -- recheck the two numbers if the fixture changes.
  long innerPairs = 0;
  long innerFlips = 0;
  long edgePairs = 0;
  long edgeFlips = 0;
  for (int y = 0; y < panel.height; ++y) {
    for (int x = 0; x + 1 < panel.width; ++x) {
      const bool changed = physWhite(fb, panel.rowBytes, x, y) != physWhite(fb, panel.rowBytes, x + 1, y);
      if ((x % 8) == 7) {
        ++edgePairs;
        edgeFlips += changed ? 1 : 0;
      } else {
        ++innerPairs;
        innerFlips += changed ? 1 : 0;
      }
    }
  }
  ASSERT_GT(innerPairs, 0);
  ASSERT_GT(edgePairs, 0);
  const double innerRate = static_cast<double>(innerFlips) / static_cast<double>(innerPairs);
  const double edgeRate = static_cast<double>(edgeFlips) / static_cast<double>(edgePairs);
  ASSERT_GT(innerRate, 0.0);
  EXPECT_LT(edgeRate / innerRate, 1.05) << "bit order inside a byte looks reversed: inner " << innerRate << ", edge "
                                        << edgeRate;
}

TEST(WalletGeneratedTree, FocalTileHintComesOutOfTheManifest) {
  const std::vector<uint8_t> raw = readFixture("manifest.json");
  ASSERT_FALSE(raw.empty());

  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Fit, 0, 0);
  parser.feed(reinterpret_cast<const char*>(raw.data()), raw.size());
  const PageLookup& out = parser.lookup();

  // The generator's centre-biased-top-left rule: 1x1 -> 0,0; 2x2 -> 0,0;
  // 4x4 -> 1,1, the upper-left of the middle four, where a page's text starts.
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Fit)].defaultCol, 0);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Fit)].defaultRow, 0);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Detail)].defaultCol, 0);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::Detail)].defaultRow, 0);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::OneToOne)].defaultCol, 1);
  EXPECT_EQ(out.grid[static_cast<uint8_t>(Level::OneToOne)].defaultRow, 1);
}

// ---------------------------------------------------------------------------
// Design B: one whole-page image per level, an arbitrary window blitted out
// ---------------------------------------------------------------------------

namespace {

// The measured shape: an A4 1:1 page image, 2576x1819 native, 322 bytes a row.
PageImageSpec a4OneToOne() {
  PageImageSpec page;
  page.present = true;
  std::strcpy(page.assetId, "abcdef0123456789");
  page.nativeWidth = 2576;
  page.nativeHeight = 1819;
  page.rowBytes = 322;
  page.rawLen = 322u * 1819u;
  page.windowStepX = 400;
  page.windowStepY = 240;
  page.focalX = 888;
  page.focalY = 480;
  return page;
}

}  // namespace

TEST(WalletWindow, ClampsAndNeverWraps) {
  // Mid-range moves are untouched.
  EXPECT_EQ(clampWindowOrigin(400, 2576, 800), 400u);
  // Past the right edge stops at span - window, it does not come back round.
  EXPECT_EQ(clampWindowOrigin(2400, 2576, 800), 1776u);
  EXPECT_EQ(clampWindowOrigin(999999, 2576, 800), 1776u);
  // Before the left edge stops at zero, it does not go negative or wrap.
  EXPECT_EQ(clampWindowOrigin(-1, 2576, 800), 0u);
  EXPECT_EQ(clampWindowOrigin(-999999, 2576, 800), 0u);
  // An image no bigger than the window is pinned.
  EXPECT_EQ(clampWindowOrigin(0, 800, 800), 0u);
  EXPECT_EQ(clampWindowOrigin(100, 800, 800), 0u);
  EXPECT_EQ(clampWindowOrigin(100, 480, 800), 0u);
}

TEST(WalletWindow, TheXLimitIsAByteCountSoItIsAlways8Aligned) {
  const PageImageSpec page = a4OneToOne();
  // 322 - 100 = 222 bytes of travel = 1776 px, which is what the pixel maths
  // gives here too (2576 - 800).
  EXPECT_EQ(maxWindowX(page, 100), 1776u);
  EXPECT_EQ(maxWindowX(page, 100) % 8, 0u);

  // The point of deriving it from bytes: an image whose nativeWidth is not a
  // multiple of 8 still cannot produce an unaligned limit.
  PageImageSpec odd = page;
  odd.nativeWidth = 2570;
  EXPECT_EQ(maxWindowX(odd, 100) % 8, 0u);
  // And a page narrower than the panel cannot pan at all.
  PageImageSpec narrow = page;
  narrow.rowBytes = 100;
  EXPECT_EQ(maxWindowX(narrow, 100), 0u);
}

TEST(WalletWindow, EveryClampedStepStays8Aligned) {
  const PageImageSpec page = a4OneToOne();
  const uint32_t limit = maxWindowX(page, 100);
  uint32_t x = page.focalX;
  ASSERT_EQ(x % 8, 0u) << "the generator must hand over an 8-aligned focal point";
  for (int i = 0; i < 12; ++i) {
    x = clampWindowOrigin(static_cast<int32_t>(x) + page.windowStepX, limit, 0);
    EXPECT_EQ(x % 8, 0u) << "step " << i;
  }
  EXPECT_EQ(x, limit) << "walking right must end at the edge, not past it";
  for (int i = 0; i < 20; ++i) {
    x = clampWindowOrigin(static_cast<int32_t>(x) - page.windowStepX, limit, 0);
    EXPECT_EQ(x % 8, 0u) << "back-step " << i;
  }
  EXPECT_EQ(x, 0u);
}

TEST(WalletPageImageGate, AcceptsAPageImageThatIsNotPanelShaped) {
  const PageImageSpec page = a4OneToOne();
  const auto bytes =
      makeHeader(/*type=*/5, /*bitDepth=*/1, /*col=*/0, /*row=*/0, page.nativeWidth, page.nativeHeight, page.rawLen);
  AssetHeader h;
  // The thing the tile gate would refuse: rawLen is 585,718, not the panel's
  // 48,000. A page image is checked against its own geometry instead.
  EXPECT_EQ(checkAssetForPanel(bytes.data(), bytes.size(), kPanelX4, h), AssetCheck::WrongPanel);
  EXPECT_EQ(checkPageImage(bytes.data(), bytes.size(), page, kPanelX4, h), AssetCheck::Ok);
  // And byte-identical on the other panel, because nothing about it is cut to a
  // screen. This is the reason design B needs no per-panel asset set at 1:1.
  EXPECT_EQ(checkPageImage(bytes.data(), bytes.size(), page, kPanelX3, h), AssetCheck::Ok);
}

TEST(WalletPageImageGate, RefusesAHeaderThatDisagreesWithTheManifest) {
  const PageImageSpec page = a4OneToOne();
  AssetHeader h;

  // Right shape, wrong type: a tile claiming to be a page image, or the reverse.
  const auto tile = makeHeader(/*type=*/3, 1, 0, 0, page.nativeWidth, page.nativeHeight, page.rawLen);
  EXPECT_EQ(checkPageImage(tile.data(), tile.size(), page, kPanelX4, h), AssetCheck::PageImageMismatch);

  // Header says one size, the manifest promised another.
  const auto wrongSize = makeHeader(5, 1, 0, 0, 2000, page.nativeHeight, page.rawLen);
  EXPECT_EQ(checkPageImage(wrongSize.data(), wrongSize.size(), page, kPanelX4, h), AssetCheck::PageImageMismatch);

  // rowBytes * nativeHeight has to be rawLen. This is the structural check that
  // replaces the panel check.
  PageImageSpec badMath = page;
  badMath.rawLen = page.rawLen + 1;
  const auto matching = makeHeader(5, 1, 0, 0, page.nativeWidth, page.nativeHeight, badMath.rawLen);
  EXPECT_EQ(checkPageImage(matching.data(), matching.size(), badMath, kPanelX4, h), AssetCheck::PageImageMismatch);

  // Encrypted and grey are refused before any of that, same as a tile.
  const auto enc = makeHeader(5, 1, 0, 0, page.nativeWidth, page.nativeHeight, page.rawLen, 7, kFlagEncrypted);
  EXPECT_EQ(checkPageImage(enc.data(), enc.size(), page, kPanelX4, h), AssetCheck::Encrypted);
  const auto grey = makeHeader(5, 2, 0, 0, page.nativeWidth, page.nativeHeight, page.rawLen);
  EXPECT_EQ(checkPageImage(grey.data(), grey.size(), page, kPanelX4, h), AssetCheck::BitDepth);
}

TEST(WalletPageImageGate, RefusesAPageSmallerThanThePanel) {
  // No window can fill the screen out of this, which is a wrong-device problem
  // rather than a corrupt file -- so WrongPanel, not PageImageMismatch.
  PageImageSpec small;
  small.present = true;
  std::strcpy(small.assetId, "abcdef0123456789");
  small.nativeWidth = 640;
  small.nativeHeight = 400;
  small.rowBytes = 80;
  small.rawLen = 80u * 400u;
  const auto bytes = makeHeader(5, 1, 0, 0, small.nativeWidth, small.nativeHeight, small.rawLen);
  AssetHeader h;
  EXPECT_EQ(checkPageImage(bytes.data(), bytes.size(), small, kPanelX4, h), AssetCheck::WrongPanel);
}

TEST(WalletManifest, PageImageIsReadForTheRequestedLevelOnly) {
  const char* json = R"({"formatVersion":1,"walletVersion":9,"items":[{"title":"Licence","pages":[{
      "levels":{
        "fit":{"cols":1,"rows":1,"assets":[{"assetId":"1111111111111111","col":0,"row":0}],
               "pageImage":{"assetId":"aaaaaaaaaaaaaaaa","nativeWidth":800,"nativeHeight":480,
                            "rowBytes":100,"rawLen":48000,"sha256":"ff","windowStepX":0,"windowStepY":0,
                            "focalX":0,"focalY":0}},
        "one_to_one":{"cols":4,"rows":4,"assets":[],
               "pageImage":{"assetId":"bbbbbbbbbbbbbbbb","nativeWidth":2576,"nativeHeight":1819,
                            "rowBytes":322,"rawLen":585718,"sha256":"ee","windowStepX":400,"windowStepY":240,
                            "focalX":888,"focalY":480}}}}]}]})";

  ManifestParser oneToOne;
  oneToOne.beginLookup(0, 0, Level::OneToOne, 0, 0);
  oneToOne.feed(json, std::strlen(json));
  const PageLookup& big = oneToOne.lookup();
  ASSERT_TRUE(big.pageFound);
  ASSERT_TRUE(big.pageImage.present);
  EXPECT_STREQ(big.pageImage.assetId, "bbbbbbbbbbbbbbbb");
  EXPECT_EQ(big.pageImage.nativeWidth, 2576);
  EXPECT_EQ(big.pageImage.nativeHeight, 1819);
  EXPECT_EQ(big.pageImage.rowBytes, 322);
  EXPECT_EQ(big.pageImage.rawLen, 585718u);
  EXPECT_EQ(big.pageImage.windowStepX, 400);
  EXPECT_EQ(big.pageImage.windowStepY, 240);
  EXPECT_EQ(big.pageImage.focalX, 888);
  EXPECT_EQ(big.pageImage.focalY, 480);
  // A level whose assets array is empty still opens, because the page image is a
  // source in its own right.
  EXPECT_FALSE(big.assetFound);

  // The FIT lookup gets FIT's page image, not the 1:1 one.
  ManifestParser fit;
  fit.beginLookup(0, 0, Level::Fit, 0, 0);
  fit.feed(json, std::strlen(json));
  ASSERT_TRUE(fit.lookup().pageImage.present);
  EXPECT_STREQ(fit.lookup().pageImage.assetId, "aaaaaaaaaaaaaaaa");
  EXPECT_TRUE(fit.lookup().assetFound) << "and the tile is still there beside it";
  EXPECT_STREQ(fit.lookup().assetId, "1111111111111111");

  // DETAIL has neither in this manifest.
  ManifestParser detail;
  detail.beginLookup(0, 0, Level::Detail, 0, 0);
  detail.feed(json, std::strlen(json));
  EXPECT_FALSE(detail.lookup().pageImage.present);
  EXPECT_FALSE(detail.lookup().assetFound);
}

TEST(WalletManifest, PageImageWithABadAssetIdIsNoPageImage) {
  // The id becomes a path, so the same 16-hex rule applies here as everywhere.
  const char* json = R"({"formatVersion":1,"items":[{"title":"T","pages":[{"levels":{
      "fit":{"cols":1,"rows":1,"assets":[],
             "pageImage":{"assetId":"../../etc","nativeWidth":800,"nativeHeight":480,
                          "rowBytes":100,"rawLen":48000}}}}]}]})";
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Fit, 0, 0);
  parser.feed(json, std::strlen(json));
  EXPECT_TRUE(parser.lookup().pageFound);
  EXPECT_FALSE(parser.lookup().pageImage.present);
}

TEST(WalletManifest, ATreeWithNoPageImagesStillReadsAsTiles) {
  // The committed generator fixture predates design B. The tile path must not
  // have moved under it.
  const std::vector<uint8_t> raw = readFixture("manifest.json");
  ASSERT_FALSE(raw.empty());
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Detail, 1, 1);
  parser.feed(reinterpret_cast<const char*>(raw.data()), raw.size());
  EXPECT_FALSE(parser.lookup().pageImage.present);
  EXPECT_TRUE(parser.lookup().assetFound);
  EXPECT_STREQ(parser.lookup().assetId, "806dfe66748d208f");
}

TEST(WalletWindow, NativeXRunsDownThePageAndNativeYRunsAcrossItInverted) {
  // The invariant the viewer's button wiring depends on, stated as an assertion
  // rather than left in a comment. GfxRenderer maps logical portrait (x, y) to
  // panel (y, panelHeight - 1 - x), so in a stored page image native x is the
  // document's vertical axis -- and it is the byte-offset axis, which is why it is
  // the one that must stay 8-aligned -- while native y is its horizontal axis,
  // inverted: the largest native y is the document's LEFT edge.
  //
  // Verified visually too: the host preview renders native (0, max) of a real
  // generated 1:1 page image as the top-left of the page, title and left margin
  // (docs/wallet-viewer.md, "Two sources per level").
  const PanelGeometry panel = kPanelX4;

  // A framebuffer with exactly one pixel of ink, at panel (0, height - 1).
  std::vector<uint8_t> fb(panel.bufferBytes, 0xFF);
  const size_t byteIndex = static_cast<size_t>(panel.height - 1) * panel.rowBytes + 0;
  fb[byteIndex] &= static_cast<uint8_t>(~0x80);  // clear the MSB: bit 0 = ink

  // Read logically, that pixel is the document's top-left corner.
  EXPECT_FALSE(logicalWhite(fb, panel, 0, 0)) << "native (0, height-1) must be logical (0,0)";
  EXPECT_TRUE(logicalWhite(fb, panel, 1, 0));
  EXPECT_TRUE(logicalWhite(fb, panel, 0, 1));
  // And native (0, 0) -- the other end of the y axis -- is the document's top
  // RIGHT, which is why panning left means increasing native y.
  EXPECT_TRUE(logicalWhite(fb, panel, panel.height - 1, 0));
}

// ---------------------------------------------------------------------------
// Machine-readable codes (P2)
//
// fixtures/codes/ is verbatim output of
//
//   tools/walletgen.py --demo --paper a4 --panel x4 --title "Boarding pass"
//       --code "qr:M1DOE/JOHN       EABC123 BTSFRAAF 0123 250Y012C0045 100"
//       --code "pdf417:M1DOE/JOHN EABC123 BTSFRAAF"
//
// -- the manifest plus the two code assets' sidecars. Both codes came back
// `verified` from the generator's own decode round trip, so these are bytes a
// scanner has already read once, on the laptop.
//
// The one thing none of this covers: nothing has been scanned off the panel.
// See docs/wallet-viewer.md.
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> codeFixturePayload(const char* name) {
  const std::vector<uint8_t> blob = readFixture(name);
  std::vector<uint8_t> payload;
  if (!wallet::host::decodeSidecarPayload(blob, payload)) return {};
  return payload;
}

std::vector<uint8_t> codeFixtureHeader(const char* name) {
  const std::vector<uint8_t> blob = readFixture(name);
  if (blob.size() < kAssetHeaderBytes) return {};
  return std::vector<uint8_t>(blob.begin(), blob.begin() + kAssetHeaderBytes);
}

CodeLookup codeLookupIn(const char* manifestFixture, int item, int index) {
  const std::vector<uint8_t> raw = readFixture(manifestFixture);
  ManifestParser parser;
  parser.beginCodeLookup(item, index);
  parser.feed(reinterpret_cast<const char*>(raw.data()), raw.size());
  return parser.codes();
}

std::string toHex(const uint8_t* bytes, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < len; ++i) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0x0F]);
  }
  return out;
}

}  // namespace

TEST(WalletSha256, MatchesTheStandardVectors) {
  // FIPS 180-4 / NIST examples. This is a second sha256 in the tree (mbedtls is
  // the first, src/network/FirmwareFlasher.cpp) and the only reason it exists is
  // that the host tests cannot link mbedtls -- so it has to be pinned to
  // something outside itself, or it only agrees with whoever wrote it.
  uint8_t digest[kSha256Bytes];

  Sha256 empty;
  empty.finish(digest);
  EXPECT_EQ(toHex(digest, sizeof(digest)), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  Sha256 abc;
  abc.update(reinterpret_cast<const uint8_t*>("abc"), 3);
  abc.finish(digest);
  EXPECT_EQ(toHex(digest, sizeof(digest)), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  Sha256 twoBlocks;
  const char* text = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  twoBlocks.update(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  twoBlocks.finish(digest);
  EXPECT_EQ(toHex(digest, sizeof(digest)), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // Fed one byte at a time it must land in the same place: the wallet hashes a
  // 48,000-byte framebuffer in one call, but the sidecar tests and any later
  // chunked reader do not.
  Sha256 dribbled;
  for (const char* p = text; *p != '\0'; ++p) dribbled.update(reinterpret_cast<const uint8_t*>(p), 1);
  dribbled.finish(digest);
  EXPECT_EQ(toHex(digest, sizeof(digest)), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // A length that lands exactly on the padding boundary -- 55 and 56 bytes are
  // where a hand-written finish() goes wrong.
  const std::string fiftyFive(55, 'a');
  Sha256 s55;
  s55.update(reinterpret_cast<const uint8_t*>(fiftyFive.data()), fiftyFive.size());
  s55.finish(digest);
  EXPECT_EQ(toHex(digest, sizeof(digest)), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  const std::string fiftySix(56, 'a');
  Sha256 s56;
  s56.update(reinterpret_cast<const uint8_t*>(fiftySix.data()), fiftySix.size());
  s56.finish(digest);
  EXPECT_EQ(toHex(digest, sizeof(digest)), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
}

TEST(WalletSha256, HexParseRefusesAnythingThatIsNotSixtyFourHex) {
  uint8_t out[kSha256Bytes];
  EXPECT_TRUE(sha256FromHex("4e748db4a304c0859a31e25706e5e22af8af768301f65c5a84d1f9762ae85f9d", out));
  EXPECT_FALSE(sha256FromHex(nullptr, out));
  EXPECT_FALSE(sha256FromHex("", out));
  // 63 characters. A truncated hash silently accepted would turn the whole verify
  // step into a formality.
  EXPECT_FALSE(sha256FromHex("4e748db4a304c0859a31e25706e5e22af8af768301f65c5a84d1f9762ae85f9", out));
  // 65.
  EXPECT_FALSE(sha256FromHex("4e748db4a304c0859a31e25706e5e22af8af768301f65c5a84d1f9762ae85f9dd", out));
  EXPECT_FALSE(sha256FromHex("4e748db4a304c0859a31e25706e5e22af8af768301f65c5a84d1f9762ae85fzz", out));
}

TEST(WalletCodeManifest, ReadsEveryFieldOfEveryCodeTheGeneratorWrote) {
  const CodeLookup first = codeLookupIn("codes/manifest.json", 0, 0);
  ASSERT_TRUE(first.itemFound);
  EXPECT_STREQ(first.title, "Boarding pass");
  EXPECT_EQ(first.codeCount, 2);
  ASSERT_TRUE(first.code.present);
  EXPECT_STREQ(first.code.id, "c001");
  EXPECT_STREQ(first.code.symbology, "qr");
  EXPECT_STREQ(first.code.assetId, "f3e7250a407c088b");
  EXPECT_TRUE(first.code.verified);
  EXPECT_EQ(first.code.moduleSize, 12);
  EXPECT_EQ(first.code.quietZone, 4);
  EXPECT_EQ(first.code.codeWidthPx, 444);
  EXPECT_EQ(first.code.codeHeightPx, 444);
  EXPECT_STREQ(first.code.sha256, "4e748db4a304c0859a31e25706e5e22af8af768301f65c5a84d1f9762ae85f9d");
}

TEST(WalletCodeManifest, TheWalkIsManifestOrder) {
  // RIGHT from the browse list opens index 0 and steps up; LEFT opens the last.
  // Both have to land on the code the manifest lists there.
  const CodeLookup second = codeLookupIn("codes/manifest.json", 0, 1);
  ASSERT_TRUE(second.code.present);
  EXPECT_EQ(second.codeCount, 2);
  EXPECT_STREQ(second.code.id, "c002");
  EXPECT_STREQ(second.code.symbology, "pdf417");
  EXPECT_STREQ(second.code.assetId, "0e706e8d6bc5dd91");
  EXPECT_EQ(second.code.moduleSize, 3);

  // Past the end: the count still comes out, so the caller can clamp or cycle
  // rather than guess. The count is the whole reason an out-of-range index is not
  // an error here.
  const CodeLookup past = codeLookupIn("codes/manifest.json", 0, 2);
  EXPECT_TRUE(past.itemFound);
  EXPECT_EQ(past.codeCount, 2);
  EXPECT_FALSE(past.code.present);

  // A negative index cannot match a cursor, so it finds nothing and still counts.
  const CodeLookup negative = codeLookupIn("codes/manifest.json", 0, -1);
  EXPECT_EQ(negative.codeCount, 2);
  EXPECT_FALSE(negative.code.present);

  // No such item.
  const CodeLookup noItem = codeLookupIn("codes/manifest.json", 4, 0);
  EXPECT_FALSE(noItem.itemFound);
  EXPECT_EQ(noItem.codeCount, 0);
}

TEST(WalletCodeManifest, TheBrowseListCountsCodesPerItemInTheSamePass) {
  // What makes LEFT/RIGHT discoverable: the row says how many codes there are, so
  // a dead button and a document with no code look different on screen.
  const std::vector<uint8_t> raw = readFixture("codes/manifest.json");
  ASSERT_FALSE(raw.empty());
  ItemEntry items[4];
  ManifestParser parser;
  parser.beginList(items, 4);
  parser.feed(reinterpret_cast<const char*>(raw.data()), raw.size());
  ASSERT_EQ(parser.itemsStored(), 1);
  EXPECT_STREQ(items[0].title, "Boarding pass");
  EXPECT_EQ(items[0].pageCount, 1);
  EXPECT_EQ(items[0].codeCount, 2);

  // And the pre-P2 fixture, which has no codes anywhere: zero, not garbage.
  const std::vector<uint8_t> old = readFixture("manifest.json");
  ASSERT_FALSE(old.empty());
  ItemEntry plain[4];
  ManifestParser second;
  second.beginList(plain, 4);
  second.feed(reinterpret_cast<const char*>(old.data()), old.size());
  ASSERT_GE(second.itemsStored(), 1);
  EXPECT_EQ(plain[0].codeCount, 0);
}

TEST(WalletCodeManifest, UnverifiedAndMissingVerifiedBothReadAsUnverified) {
  const char* json = R"({"formatVersion":1,"items":[{"title":"Ticket","pages":[
      {"levels":{},"codes":[
        {"id":"c001","symbology":"qr","verified":false,"assetId":"1111111111111111","sha256":"ab"},
        {"id":"c002","symbology":"aztec","assetId":"2222222222222222"},
        {"id":"c003","symbology":"qr","verified":true,"assetId":"3333333333333333"}]}]}]})";

  ManifestParser a;
  a.beginCodeLookup(0, 0);
  a.feed(json, std::strlen(json));
  ASSERT_TRUE(a.codes().code.present);
  EXPECT_FALSE(a.codes().code.verified) << "verified:false must read as false";
  EXPECT_EQ(a.codes().codeCount, 3);

  ManifestParser b;
  b.beginCodeLookup(0, 1);
  b.feed(json, std::strlen(json));
  ASSERT_TRUE(b.codes().code.present);
  // Silence is not consent. A manifest that says nothing has verified nothing.
  EXPECT_FALSE(b.codes().code.verified) << "a missing `verified` must read as false";

  ManifestParser c;
  c.beginCodeLookup(0, 2);
  c.feed(json, std::strlen(json));
  ASSERT_TRUE(c.codes().code.present);
  EXPECT_TRUE(c.codes().code.verified);
}

TEST(WalletCodeManifest, ACodeWhoseAssetIdIsNotSixteenHexIsNoCode) {
  // The id becomes a path. Refused here, before it can become one -- and it still
  // counts, because the count is what the manifest claims the document has.
  const char* json = R"({"formatVersion":1,"items":[{"title":"T","pages":[
      {"levels":{},"codes":[{"id":"c001","symbology":"qr","verified":true,"assetId":"../../etc/passwd"}]}]}]})";
  ManifestParser parser;
  parser.beginCodeLookup(0, 0);
  parser.feed(json, std::strlen(json));
  EXPECT_TRUE(parser.codes().itemFound);
  EXPECT_EQ(parser.codes().codeCount, 1);
  EXPECT_FALSE(parser.codes().code.present);
}

TEST(WalletCodeManifest, CodesOfEveryPageAreOneWalk) {
  // Codes belong to a page in the manifest; a rider walks the codes of a
  // document. So page 1's code follows page 0's in one ring.
  const char* json = R"({"formatVersion":1,"items":[{"title":"Two pages","pages":[
      {"levels":{},"codes":[{"id":"c001","symbology":"qr","verified":true,"assetId":"1111111111111111"}]},
      {"levels":{},"codes":[{"id":"c002","symbology":"ean13","verified":true,"assetId":"2222222222222222"}]}]}]})";
  ManifestParser first;
  first.beginCodeLookup(0, 0);
  first.feed(json, std::strlen(json));
  EXPECT_EQ(first.codes().codeCount, 2);
  EXPECT_STREQ(first.codes().code.id, "c001");

  ManifestParser second;
  second.beginCodeLookup(0, 1);
  second.feed(json, std::strlen(json));
  EXPECT_STREQ(second.codes().code.id, "c002");
  EXPECT_STREQ(second.codes().code.symbology, "ean13");
}

TEST(WalletCodeManifest, AnOverlongPayloadBreaksNothing) {
  // A boarding-pass payload runs to hundreds of characters and can exceed the
  // parser's 512-byte token buffer, where the value is dropped without a callback
  // (StreamingJsonParser.cpp:226-249). The device reads no payload, so this must
  // cost nothing -- and above all must not lose the fields after it.
  std::string json = R"({"formatVersion":1,"items":[{"title":"Big","pages":[{"levels":{},"codes":[
      {"id":"c001","symbology":"pdf417","payload":")";
  json += std::string(900, 'M');
  json += R"(","verified":true,"assetId":"abcdef0123456789","moduleSize":3}]}]}]})";

  ManifestParser parser;
  parser.beginCodeLookup(0, 0);
  parser.feed(json.c_str(), json.size());
  EXPECT_FALSE(parser.hasError());
  ASSERT_TRUE(parser.codes().code.present);
  EXPECT_STREQ(parser.codes().code.assetId, "abcdef0123456789");
  EXPECT_TRUE(parser.codes().code.verified);
  EXPECT_EQ(parser.codes().code.moduleSize, 3);
}

TEST(WalletCodeGate, AcceptsAMachineCodeAndRefusesADocumentTile) {
  const std::vector<uint8_t> header = codeFixtureHeader("codes/f3/f3e7250a407c088b.rle");
  ASSERT_EQ(header.size(), kAssetHeaderBytes);
  AssetHeader parsed;
  // The generator's own bytes: assetType 4, one panel frame.
  EXPECT_EQ(checkCodeAsset(header.data(), header.size(), kPanelX4, parsed), AssetCheck::Ok);
  EXPECT_EQ(parsed.assetType, AssetType::MachineCode);
  EXPECT_EQ(parsed.presentation, 1);

  // A FIT tile is the right shape and the wrong thing. Drawing page three of an
  // insurance policy where a boarding pass belongs is worse than drawing nothing.
  const auto fit = makeHeader(/*type=*/1);
  EXPECT_EQ(checkCodeAsset(fit.data(), fit.size(), kPanelX4, parsed), AssetCheck::NotACode);
  // And the panel gate still runs first: wrong screen beats wrong type.
  EXPECT_EQ(checkCodeAsset(header.data(), header.size(), kPanelX3, parsed), AssetCheck::WrongPanel);
}

TEST(WalletCodeHash, RealGeneratorBytesMatchTheManifestHash) {
  const CodeLookup found = codeLookupIn("codes/manifest.json", 0, 0);
  ASSERT_TRUE(found.code.present);
  const std::vector<uint8_t> payload = codeFixturePayload("codes/f3/f3e7250a407c088b.rle");
  ASSERT_EQ(payload.size(), 48000u);
  const std::vector<uint8_t> header = codeFixtureHeader("codes/f3/f3e7250a407c088b.rle");
  AssetHeader parsed;
  ASSERT_TRUE(parseAssetHeader(header.data(), header.size(), parsed));

  const HashResult full = checkPayloadHash(payload.data(), payload.size(), found.code.sha256, parsed.sha256Prefix);
  EXPECT_TRUE(full.ok);
  EXPECT_EQ(full.authority, HashAuthority::Full);

  // The header's 8-byte prefix is the same hash, and agrees. Two implementations
  // of the format meeting again: the generator wrote both fields, this side hashes
  // the payload and gets both back.
  const HashResult prefixOnly = checkPayloadHash(payload.data(), payload.size(), "", parsed.sha256Prefix);
  EXPECT_TRUE(prefixOnly.ok);
  EXPECT_EQ(prefixOnly.authority, HashAuthority::Prefix);
}

TEST(WalletCodeHash, OneCorruptedByteRefusesTheCodeAndTheVerdictSaysSo) {
  // The path that matters, proved to fire. A garbage document tile is cosmetic; a
  // garbage barcode is a rider at a gate with a pass that will not scan, so this
  // check is the one thing P2 adds that must never be skipped.
  const CodeLookup found = codeLookupIn("codes/manifest.json", 0, 0);
  ASSERT_TRUE(found.code.present);
  std::vector<uint8_t> payload = codeFixturePayload("codes/f3/f3e7250a407c088b.rle");
  ASSERT_EQ(payload.size(), 48000u);
  const std::vector<uint8_t> header = codeFixtureHeader("codes/f3/f3e7250a407c088b.rle");
  AssetHeader parsed;
  ASSERT_TRUE(parseAssetHeader(header.data(), header.size(), parsed));

  // One bit, in the middle of the code, is all it takes to turn a module the wrong
  // colour -- and all it takes to fail the hash.
  const size_t middle = payload.size() / 2;
  payload[middle] = static_cast<uint8_t>(payload[middle] ^ 0x01);

  const HashResult full = checkPayloadHash(payload.data(), payload.size(), found.code.sha256, parsed.sha256Prefix);
  EXPECT_FALSE(full.ok) << "a flipped bit must not pass the manifest hash";
  const HashResult prefix = checkPayloadHash(payload.data(), payload.size(), "", parsed.sha256Prefix);
  EXPECT_FALSE(prefix.ok) << "nor the header prefix";

  // And the verdict that selects the failure screen. WalletCodeActivity has
  // exactly one call to codeVerdict() and exactly one path for RefuseAsset, which
  // is drawFailure() -- so this is the screen, as close as a host test gets to it.
  EXPECT_EQ(codeVerdict(/*loadedAndHashed=*/false, found.code.verified, /*markerPlaced=*/true),
            CodeVerdict::RefuseAsset);
  EXPECT_EQ(codeVerdict(/*loadedAndHashed=*/false, false, false), CodeVerdict::RefuseAsset);

  // A truncated payload is a different fault and must also fail, not hash the
  // short buffer and pass.
  payload.resize(payload.size() - 100);
  EXPECT_FALSE(checkPayloadHash(payload.data(), payload.size(), found.code.sha256, parsed.sha256Prefix).ok);
  EXPECT_FALSE(checkPayloadHash(nullptr, 0, found.code.sha256, parsed.sha256Prefix).ok);
}

TEST(WalletCodeVerdict, AnUnverifiedCodeIsShownOnlyMarked) {
  // The choice, as an assertion. An unverified code is not hidden -- hiding it
  // makes a dead button and a document with no code look the same -- but it is
  // never shown unmarked, because that is the lie this feature must not tell.
  EXPECT_EQ(codeVerdict(true, /*verified=*/true, /*marked=*/true), CodeVerdict::Draw);
  EXPECT_EQ(codeVerdict(true, /*verified=*/true, /*marked=*/false), CodeVerdict::Draw)
      << "a verified code needs no marker, so a full-panel code still draws";
  EXPECT_EQ(codeVerdict(true, /*verified=*/false, /*marked=*/true), CodeVerdict::Draw);
  EXPECT_EQ(codeVerdict(true, /*verified=*/false, /*marked=*/false), CodeVerdict::RefuseUnmarked);
}

TEST(WalletCodeLabel, TheBandIsCheckedInPanelColumnsNotRows) {
  const PanelGeometry panel = kPanelX4;
  std::vector<uint8_t> fb(panel.bufferBytes, 0xFF);
  // A blank framebuffer: every band is blank.
  EXPECT_TRUE(logicalBandIsBlank(fb.data(), panel, 770, 794));

  // One pixel of ink at logical (300, 780) -- inside the band. A band checked as a
  // range of panel *rows* instead of columns would miss it entirely, which is the
  // bug that would let a label land on a code.
  const int logicalX = 300;
  const int logicalY = 780;
  const int phyX = logicalY;
  const int phyY = panel.height - 1 - logicalX;
  fb[static_cast<size_t>(phyY) * panel.rowBytes + phyX / 8] &= static_cast<uint8_t>(~(0x80u >> (phyX % 8)));
  EXPECT_FALSE(logicalBandIsBlank(fb.data(), panel, 770, 794)) << "ink inside the band must block the label";
  // A band above it is still clear.
  EXPECT_TRUE(logicalBandIsBlank(fb.data(), panel, 700, 760));

  // Degenerate asks are refused rather than answered "blank".
  EXPECT_FALSE(logicalBandIsBlank(nullptr, panel, 700, 760));
  EXPECT_FALSE(logicalBandIsBlank(fb.data(), panel, 760, 760));
  EXPECT_FALSE(logicalBandIsBlank(fb.data(), panel, 900, 950)) << "past the logical bottom is not blank space";
}

TEST(WalletCodeLabel, TheRealQrLeavesTheLabelBandBlank) {
  // The QR fixture is 444x444 px of code plus quiet zone on a 480x800 logical
  // screen, so there is white below it. Measured off the bytes, not assumed from
  // the manifest.
  const std::vector<uint8_t> payload = codeFixturePayload("codes/f3/f3e7250a407c088b.rle");
  ASSERT_EQ(payload.size(), 48000u);
  const PanelGeometry panel = kPanelX4;
  EXPECT_TRUE(logicalBandIsBlank(payload.data(), panel, 772, 794));
  // And the code itself is where the manifest says: the middle band is not blank.
  EXPECT_FALSE(logicalBandIsBlank(payload.data(), panel, 380, 420));
}

TEST(WalletCodeLabel, SymbologyIsUpperCasedNotTranslated) {
  char out[kSymbologyBufBytes];
  symbologyLabel("qr", out, sizeof(out));
  EXPECT_STREQ(out, "QR");
  symbologyLabel("pdf417", out, sizeof(out));
  EXPECT_STREQ(out, "PDF417");
  symbologyLabel("datamatrix", out, sizeof(out));
  EXPECT_STREQ(out, "DATAMATRIX") << "the longest symbology the generator emits must survive whole";
  // A manifest that left the field out still gets a legible label.
  symbologyLabel("", out, sizeof(out));
  EXPECT_STREQ(out, "CODE");
  symbologyLabel(nullptr, out, sizeof(out));
  EXPECT_STREQ(out, "CODE");
  // And a longer one is cut, never overrun.
  char small[4];
  symbologyLabel("datamatrix", small, sizeof(small));
  EXPECT_STREQ(small, "DAT");
}

// ---------------------------------------------------------------------------
// The code walk's ends, and CMD:GOTO_WALLET's arguments
// ---------------------------------------------------------------------------

TEST(WalletCodeWalk, WrapsAtBothEndsAndTheBrowseEntryIsTheSameRing) {
  // The stated rule: the code walk WRAPS where the document pan CLAMPS. Two
  // different behaviours on the same buttons, so the difference is asserted here
  // rather than left to a comment (docs/wallet-viewer.md, "The walk wraps").
  EXPECT_EQ(walkCodeIndex(0, +1, 3), 1);
  EXPECT_EQ(walkCodeIndex(1, +1, 3), 2);
  EXPECT_EQ(walkCodeIndex(2, +1, 3), 0) << "RIGHT off the last code comes back to the first";
  EXPECT_EQ(walkCodeIndex(0, -1, 3), 2) << "LEFT off the first code goes to the last";

  // The browse screen's entry points are steps on the same ring: RIGHT steps on
  // from before the start, LEFT steps back off the beginning.
  EXPECT_EQ(walkCodeIndex(-1, +1, 3), 0);
  EXPECT_EQ(walkCodeIndex(0, -1, 3), 2);

  // A ring of one returns where it started, which is the caller's cue to spend no
  // refresh. A ring of none returns -1, which is its cue to do nothing at all.
  EXPECT_EQ(walkCodeIndex(0, +1, 1), 0);
  EXPECT_EQ(walkCodeIndex(0, -1, 1), 0);
  EXPECT_EQ(walkCodeIndex(-1, +1, 1), 0) << "browse RIGHT still opens the only code";
  EXPECT_EQ(walkCodeIndex(0, -1, 1), 0) << "and browse LEFT opens the same one";
  EXPECT_EQ(walkCodeIndex(0, +1, 0), -1);
  EXPECT_EQ(walkCodeIndex(-1, +1, 0), -1);

  // An index left over from a shorter manifest cannot walk out of range: this is
  // how the code screen recovers when the card changed under it.
  EXPECT_EQ(walkCodeIndex(9, +1, 2), 0);
  EXPECT_EQ(walkCodeIndex(-9, +1, 2), 0);
}

TEST(WalletGotoArgs, NothingAnItemOrAnItemAndACode) {
  int item = 99;
  int code = 99;

  // No arguments: the browse list.
  ASSERT_TRUE(parseGotoWalletArgs("", item, code));
  EXPECT_EQ(item, -1);
  EXPECT_EQ(code, -1);
  ASSERT_TRUE(parseGotoWalletArgs(nullptr, item, code));
  EXPECT_EQ(item, -1);
  // Whitespace only is still no arguments -- String::trim() should have removed it,
  // and this must not depend on that.
  ASSERT_TRUE(parseGotoWalletArgs("   ", item, code));
  EXPECT_EQ(item, -1);
  EXPECT_EQ(code, -1);

  ASSERT_TRUE(parseGotoWalletArgs("0", item, code));
  EXPECT_EQ(item, 0);
  EXPECT_EQ(code, -1);

  ASSERT_TRUE(parseGotoWalletArgs("2 3", item, code));
  EXPECT_EQ(item, 2);
  EXPECT_EQ(code, 3);
  // Extra spacing between the two, as a host script's printf may well produce.
  ASSERT_TRUE(parseGotoWalletArgs("  12   7 ", item, code));
  EXPECT_EQ(item, 12);
  EXPECT_EQ(code, 7);
}

TEST(WalletGotoArgs, RefusesEverythingItCannotMean) {
  int item = 0;
  int code = 0;
  // A mistyped index must be told, not coerced into document 0.
  EXPECT_FALSE(parseGotoWalletArgs("x", item, code));
  EXPECT_FALSE(parseGotoWalletArgs("0x2", item, code));
  EXPECT_FALSE(parseGotoWalletArgs("-1", item, code)) << "no negative index: -1 is the internal 'no target'";
  EXPECT_FALSE(parseGotoWalletArgs("1 -1", item, code));
  EXPECT_FALSE(parseGotoWalletArgs("1 2 3", item, code)) << "a third argument is a typo, not a feature";
  EXPECT_FALSE(parseGotoWalletArgs("1.5", item, code));
  EXPECT_FALSE(parseGotoWalletArgs("99999", item, code)) << "no wallet has ten thousand documents";
  // And a refusal leaves the outputs at 'no target', so a caller that ignores the
  // return value still cannot open the wrong document.
  EXPECT_EQ(item, -1);
  EXPECT_EQ(code, -1);
}

// ---------------------------------------------------------------------------
// The hint labels have to fit the hint box
//
// This is measured with the firmware's OWN font table -- ubuntu_10, the face
// UI_10_FONT_ID maps to (src/main.cpp:109) -- through the firmware's own
// EpdFont::getTextDimensions(), which is what GfxRenderer::getTextWidth() calls on
// the device. So a label that overflows the box fails here rather than on the
// panel.
//
// It reads the label text out of every translation file, not out of a copy in this
// test: the risk is a translator writing something longer, and a test carrying its
// own copy of the string cannot see that.
//
// The box is 106 px wide and the text is centred in it
// (src/components/themes/BaseTheme.cpp:165, :182-186), so anything wider spills
// over the border on both sides.
// ---------------------------------------------------------------------------

namespace {

constexpr int kHintBoxWidth = 106;

// The value of `key` in a yaml translation file, or "" when the file does not
// carry it -- most languages fall back to English and state nothing.
std::string yamlValue(const std::string& path, const std::string& key) {
  FILE* fh = std::fopen(path.c_str(), "rb");
  if (fh == nullptr) return "";
  std::string out;
  char line[512];
  const std::string prefix = key + ":";
  while (std::fgets(line, sizeof(line), fh) != nullptr) {
    std::string text(line);
    if (text.compare(0, prefix.size(), prefix) != 0) continue;
    const size_t open = text.find('"');
    const size_t close = text.rfind('"');
    if (open != std::string::npos && close > open) out = text.substr(open + 1, close - open - 1);
    break;
  }
  std::fclose(fh);
  return out;
}

int uiTenWidth(const std::string& text) {
  int width = 0;
  int height = 0;
  EpdFont(&ubuntu_10_regular).getTextDimensions(text.c_str(), &width, &height);
  return width;
}

}  // namespace

TEST(WalletCodeHints, TheTwoFrontHintsAreDirectionalAndDistinguishable) {
  const std::string dir = std::string(WALLET_I18N_DIR);
  const std::string prev = yamlValue(dir + "/english.yaml", "STR_WALLET_CODE_PREV");
  const std::string next = yamlValue(dir + "/english.yaml", "STR_WALLET_CODE_NEXT");
  ASSERT_FALSE(prev.empty()) << "STR_WALLET_CODE_PREV missing from english.yaml";
  ASSERT_FALSE(next.empty()) << "STR_WALLET_CODE_NEXT missing from english.yaml";

  // The defect being fixed: two hint boxes with the same text. The maintainer
  // could not tell which button went which way, pressed one, landed on the last
  // code and concluded the code was drawn wrong.
  EXPECT_NE(prev, next) << "the two hints must not read the same";
  // And distinguishable at a glance means more than a different string: each has to
  // carry a direction the other does not.
  EXPECT_TRUE(prev.find('<') != std::string::npos || next.find('>') != std::string::npos)
      << "neither label carries a direction";
}

TEST(WalletCodeHints, EveryTranslationOfThemFitsTheHintBox) {
  // Measured against the real face, for every language that states them. Today
  // that is English alone -- the rest fall back -- so this test grows cover on its
  // own as translations arrive.
  static const char* kLanguages[] = {
      "arabic",     "belarusian", "bosnian", "catalan",       "czech",         "danish",    "dutch",     "english",
      "finnish",    "french",     "german",  "hebrew",        "hungarian",     "indonesia", "italian",   "kazakh",
      "lithuanian", "norwegian",  "polish",  "portuguese-BR", "portuguese-PT", "romanian",  "russian",   "slovak",
      "slovenian",  "spanish",    "swedish", "turkish",       "ukrainian",     "valencian", "vietnamese"};
  const std::string dir = std::string(WALLET_I18N_DIR);

  int stated = 0;
  for (const char* language : kLanguages) {
    const std::string path = dir + "/" + language + ".yaml";
    for (const char* key : {"STR_WALLET_CODE_PREV", "STR_WALLET_CODE_NEXT"}) {
      const std::string label = yamlValue(path, key);
      if (label.empty()) continue;
      ++stated;
      const int width = uiTenWidth(label);
      EXPECT_GT(width, 0) << language << " " << key << " measured as nothing: a glyph the face lacks";
      EXPECT_LE(width, kHintBoxWidth) << language << " " << key << " is " << width << " px, box is " << kHintBoxWidth;
    }
  }
  EXPECT_EQ(stated, 2) << "English states both and nobody else does yet; update this count with the translations";

  // The rejected alternative, kept as a number: "Prev code" / "Next code" measure
  // 92 and 95 px, which fits English and leaves ten pixels for every other
  // language. That is why the labels are the short arrow form.
  EXPECT_GT(uiTenWidth("Prev code"), kHintBoxWidth - 20);
  EXPECT_GT(uiTenWidth("Next code"), kHintBoxWidth - 20);
}

// ---------------------------------------------------------------------------
// Wallet crypto v1 (P3)
//
// fixtures/enc/ is verbatim output of
//
//   tools/walletgen.py --demo --paper a4 --panel x4 --title "Boarding pass"
//       --code "qr:M1DOE/JOHN ..." --key <the test key below> --pin UDLRUD
//
// -- `manifest.enc` and the encrypted QR asset. The key is
// 000102...1f, a deliberately non-secret test key committed on purpose; a real
// wallet key never leaves a 0600 file on the laptop.
//
// The asset ids are panel-and-content scoped, so the encrypted QR asset has the
// SAME id as the cleartext one in fixtures/codes -- which means its plaintext must
// hash to the same value the cleartext tree's manifest states. That is the
// cross-tree check below, and it needs no expected-plaintext fixture at all.
// ---------------------------------------------------------------------------

namespace {

// The committed test key, and the wallet it was built for.
const uint8_t kTestKey[kWalletKeyLen] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                         0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                         0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
constexpr const char* kEncCodeAssetId = "f3e7250a407c088b";
// The plaintext's sha256, as the CLEARTEXT tree's manifest states it. Nothing in
// the encrypted fixture carries this value.
constexpr const char* kEncCodePlainSha = "4e748db4a304c0859a31e25706e5e22af8af768301f65c5a84d1f9762ae85f9d";

std::vector<uint8_t> encFixture(const char* name) { return readFixture(name); }

}  // namespace

TEST(WalletCryptoIv, AssetIvIsTheIdThenTheVersionThenZero) {
  uint8_t iv[kAssetIvLen];
  ASSERT_TRUE(buildAssetIv(kEncCodeAssetId, 1, iv));
  const uint8_t want[kAssetIvLen] = {0xf3, 0xe7, 0x25, 0x0a, 0x40, 0x7c, 0x08, 0x8b,
                                     0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(std::memcmp(iv, want, sizeof(iv)), 0);

  // The version is little endian, and it is the only thing that changes between two
  // versions of the same asset -- which is what stops one keystream covering two
  // plaintexts.
  ASSERT_TRUE(buildAssetIv(kEncCodeAssetId, 0x01020304u, iv));
  EXPECT_EQ(iv[8], 0x04);
  EXPECT_EQ(iv[9], 0x03);
  EXPECT_EQ(iv[10], 0x02);
  EXPECT_EQ(iv[11], 0x01);
  // The trailing word is the block counter and starts at zero.
  for (size_t i = 12; i < kAssetIvLen; ++i) EXPECT_EQ(iv[i], 0);

  // An id that is not 16 hex characters never becomes an IV, same rule as the path.
  EXPECT_FALSE(buildAssetIv("../../etc/passwd", 1, iv));
  EXPECT_FALSE(buildAssetIv("f3e7250a407c088", 1, iv));
  EXPECT_FALSE(buildAssetIv(nullptr, 1, iv));
}

TEST(WalletCryptoCtr, OffsetSplitsIntoABlockAndARemainder) {
  // The whole reason the format uses CTR: a windowed read reaches an arbitrary
  // payload offset by starting the counter at offset / 16 and dropping offset % 16
  // bytes of keystream.
  EXPECT_EQ(ctrStartForOffset(0).block, 0u);
  EXPECT_EQ(ctrStartForOffset(0).skip, 0);
  EXPECT_EQ(ctrStartForOffset(1).block, 0u);
  EXPECT_EQ(ctrStartForOffset(1).skip, 1);
  EXPECT_EQ(ctrStartForOffset(15).block, 0u);
  EXPECT_EQ(ctrStartForOffset(15).skip, 15);
  EXPECT_EQ(ctrStartForOffset(16).block, 1u);
  EXPECT_EQ(ctrStartForOffset(16).skip, 0);
  EXPECT_EQ(ctrStartForOffset(17).block, 1u);
  EXPECT_EQ(ctrStartForOffset(17).skip, 1);
  // A real row offset from a design-B window: row 1 of a 322-byte-stride page image.
  EXPECT_EQ(ctrStartForOffset(322).block, 20u);
  EXPECT_EQ(ctrStartForOffset(322).skip, 2);
}

TEST(WalletCryptoCtr, CounterAdvancesBigEndianAcrossAByteBoundary) {
  uint8_t iv[kAssetIvLen] = {0};
  uint8_t out[kAesBlockLen];

  advanceCounter(iv, 0, out);
  for (size_t i = 0; i < kAesBlockLen; ++i) EXPECT_EQ(out[i], 0) << i;

  advanceCounter(iv, 1, out);
  EXPECT_EQ(out[15], 1);
  advanceCounter(iv, 255, out);
  EXPECT_EQ(out[15], 255);
  EXPECT_EQ(out[14], 0);
  // 256 has to carry into the next byte up, not wrap in place.
  advanceCounter(iv, 256, out);
  EXPECT_EQ(out[15], 0);
  EXPECT_EQ(out[14], 1);
  advanceCounter(iv, 65535, out);
  EXPECT_EQ(out[15], 255);
  EXPECT_EQ(out[14], 255);
  EXPECT_EQ(out[13], 0);

  // And it carries out of the low word into the version word, which is what the
  // format's "the whole 128-bit block increments" sentence means. An asset large
  // enough to need this does not exist today, but the arithmetic must not be the
  // reason.
  uint8_t ff[kAssetIvLen] = {0};
  ff[12] = ff[13] = ff[14] = ff[15] = 0xFF;
  advanceCounter(ff, 1, out);
  EXPECT_EQ(out[15], 0);
  EXPECT_EQ(out[12], 0);
  EXPECT_EQ(out[11], 1) << "the carry has to reach the version word";
}

TEST(WalletCryptoCtr, DecryptsRealGeneratorCiphertextToTheRightPlaintext) {
  const std::vector<uint8_t> file = encFixture("enc/f3/f3e7250a407c088b.dat");
  ASSERT_EQ(file.size(), 48032u) << "fixtures/enc asset missing";

  AssetHeader header;
  ASSERT_TRUE(parseAssetHeader(file.data(), file.size(), header));
  EXPECT_EQ(header.assetType, AssetType::MachineCode);
  EXPECT_NE(header.flags & kFlagEncrypted, 0) << "the generator must mark it encrypted";
  EXPECT_EQ(header.rawLen, 48000u);

  // The tile gate refuses it without a key and accepts it with one. That flag is the
  // whole difference between "noise on the panel" and "a document".
  AssetHeader gated;
  EXPECT_EQ(checkAssetForPanel(file.data(), file.size(), kPanelX4, gated), AssetCheck::Encrypted);
  EXPECT_EQ(checkAssetForPanel(file.data(), file.size(), kPanelX4, gated, /*allowEncrypted=*/true), AssetCheck::Ok);
  EXPECT_EQ(checkCodeAsset(file.data(), file.size(), kPanelX4, gated, true), AssetCheck::Ok);

  uint8_t iv[kAssetIvLen];
  ASSERT_TRUE(buildAssetIv(kEncCodeAssetId, header.version, iv));

  // Whole payload at offset 0, in place, exactly as the device does it.
  std::vector<uint8_t> plain(file.begin() + kAssetHeaderBytes, file.end());
  wallet::host::ctrXor(kTestKey, iv, 0, plain.data(), plain.size());

  // Cross-tree: this is the same asset the CLEARTEXT fixture carries, so the
  // decrypted bytes must hash to what that tree's manifest states.
  const HashResult full = checkPayloadHash(plain.data(), plain.size(), kEncCodePlainSha, header.sha256Prefix);
  EXPECT_TRUE(full.ok) << "decrypted plaintext does not match the cleartext tree's sha256";
  EXPECT_EQ(full.authority, HashAuthority::Full);
  // And the header's 8-byte prefix covers the PLAINTEXT, not the ciphertext.
  EXPECT_TRUE(checkPayloadHash(plain.data(), plain.size(), "", header.sha256Prefix).ok);
  EXPECT_FALSE(checkPayloadHash(file.data() + kAssetHeaderBytes, plain.size(), "", header.sha256Prefix).ok)
      << "the prefix must not match the ciphertext";

  // The picture is a real QR again: ink between 1 % and 20 %, and the label band
  // blank, exactly as the cleartext one measured.
  const int inked = logicalBandInk(plain, kPanelX4, 0, kPanelX4.height, 0, kPanelX4.width);
  EXPECT_GT(inked, kPanelX4.width * kPanelX4.height / 100);
  EXPECT_LT(inked, kPanelX4.width * kPanelX4.height / 5);
  EXPECT_TRUE(logicalBandIsBlank(plain.data(), kPanelX4, 772, 794));
}

TEST(WalletCryptoCtr, EveryRowOffsetADesignBWindowUsesDecryptsIndependently) {
  // The property the windowed read depends on, against real ciphertext: decrypting
  // a slice starting at an arbitrary offset must give the same bytes as decrypting
  // the whole payload and taking that slice. If this failed, design B would need a
  // full decrypt per row -- 480 of them per frame.
  const std::vector<uint8_t> file = encFixture("enc/f3/f3e7250a407c088b.dat");
  ASSERT_EQ(file.size(), 48032u);
  AssetHeader header;
  ASSERT_TRUE(parseAssetHeader(file.data(), file.size(), header));
  uint8_t iv[kAssetIvLen];
  ASSERT_TRUE(buildAssetIv(kEncCodeAssetId, header.version, iv));

  std::vector<uint8_t> whole(file.begin() + kAssetHeaderBytes, file.end());
  wallet::host::ctrXor(kTestKey, iv, 0, whole.data(), whole.size());

  // 0 and 1 and 15 and 16 and 17 are the block-boundary cases; 100 is one panel row;
  // 322 and 644 are rows of a real 1:1 page image's stride; the last one is the tail.
  const size_t offsets[] = {0, 1, 15, 16, 17, 99, 100, 322, 644, 4800, 47900, 47999};
  for (const size_t offset : offsets) {
    const size_t len = std::min<size_t>(100, whole.size() - offset);
    std::vector<uint8_t> slice(file.begin() + kAssetHeaderBytes + offset,
                               file.begin() + kAssetHeaderBytes + offset + len);
    wallet::host::ctrXor(kTestKey, iv, static_cast<uint32_t>(offset), slice.data(), slice.size());
    EXPECT_EQ(std::memcmp(slice.data(), whole.data() + offset, len), 0) << "offset " << offset;
  }
}

TEST(WalletCryptoCtr, TheWrongKeyIsCaughtByThePlaintextHashAndNotDrawn) {
  // A wrong key turns 48 KB into noise that looks exactly like a hardware fault on
  // the panel. The header's plaintext prefix is what catches it -- the one place the
  // hash earns its keep beyond corruption.
  const std::vector<uint8_t> file = encFixture("enc/f3/f3e7250a407c088b.dat");
  ASSERT_EQ(file.size(), 48032u);
  AssetHeader header;
  ASSERT_TRUE(parseAssetHeader(file.data(), file.size(), header));
  uint8_t iv[kAssetIvLen];
  ASSERT_TRUE(buildAssetIv(kEncCodeAssetId, header.version, iv));

  uint8_t wrongKey[kWalletKeyLen];
  std::memcpy(wrongKey, kTestKey, sizeof(wrongKey));
  wrongKey[31] ^= 0x01;  // one bit

  std::vector<uint8_t> plain(file.begin() + kAssetHeaderBytes, file.end());
  wallet::host::ctrXor(wrongKey, iv, 0, plain.data(), plain.size());
  EXPECT_FALSE(checkPayloadHash(plain.data(), plain.size(), kEncCodePlainSha, header.sha256Prefix).ok)
      << "a one-bit-wrong key must not pass the plaintext hash";
  EXPECT_FALSE(checkPayloadHash(plain.data(), plain.size(), "", header.sha256Prefix).ok);

  // The right IV with the wrong version is the same failure: a version bump changes
  // the keystream, which is what keeps one keystream to one plaintext.
  uint8_t otherIv[kAssetIvLen];
  ASSERT_TRUE(buildAssetIv(kEncCodeAssetId, header.version + 1, otherIv));
  std::vector<uint8_t> again(file.begin() + kAssetHeaderBytes, file.end());
  wallet::host::ctrXor(kTestKey, otherIv, 0, again.data(), again.size());
  EXPECT_FALSE(checkPayloadHash(again.data(), again.size(), "", header.sha256Prefix).ok);
}

TEST(WalletCryptoManifest, EnvelopeParsesAndTheTagVerifies) {
  const std::vector<uint8_t> blob = encFixture("enc/manifest.enc");
  ASSERT_FALSE(blob.empty()) << "fixtures/enc/manifest.enc missing";

  ManifestEnvelope envelope;
  ASSERT_TRUE(parseManifestEnvelope(blob.data(), blob.size(), envelope));
  EXPECT_TRUE(envelope.valid);
  // The committed fixture is a **v1** envelope, written before the cleartext
  // walletVersion field existed. It stays v1 on purpose: a card in the field must
  // not become unreadable when the container grows.
  EXPECT_EQ(envelope.ciphertextOffset, kManifestEnvelopeLenV1);
  EXPECT_FALSE(envelope.hasWalletVersion);
  EXPECT_EQ(envelope.walletVersion, 0u);
  EXPECT_EQ(envelope.plaintextLen, envelope.ciphertextLen);
  EXPECT_EQ(envelope.tagOffset, kManifestEnvelopeLenV1 + envelope.ciphertextLen);
  EXPECT_LE(blob.size(), kMaxEncryptedManifestBytes) << "the fixture must be under the cap";

  std::vector<uint8_t> plain(envelope.ciphertextLen);
  ASSERT_TRUE(wallet::host::gcmDecrypt(kTestKey, envelope.nonce, kGcmNonceLen, blob.data() + envelope.tagOffset,
                                       kGcmTagLen, blob.data() + envelope.ciphertextOffset, envelope.ciphertextLen,
                                       plain.data()));

  // And it is a manifest the firmware's own parser reads, encryption or not.
  ManifestParser parser;
  ItemEntry items[4];
  parser.beginList(items, 4);
  parser.feed(reinterpret_cast<const char*>(plain.data()), plain.size());
  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(parser.formatVersion(), 1u);
  ASSERT_EQ(parser.itemsStored(), 1);
  EXPECT_STREQ(items[0].title, "Boarding pass") << "the title only exists after the tag verified";
  EXPECT_EQ(items[0].codeCount, 1);
}

// A v2 envelope built from the v1 fixture: same sealed bytes, header grown by the
// cleartext u32 walletVersion. Synthesised rather than committed as a second
// fixture so the two cannot drift apart.
static std::vector<uint8_t> v2FromV1(const std::vector<uint8_t>& v1, uint32_t walletVersion) {
  std::vector<uint8_t> out;
  out.reserve(v1.size() + 4);
  out.insert(out.end(), v1.begin(), v1.begin() + kManifestEnvelopeLenV1);
  out[4] = kManifestEncVersion;
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((walletVersion >> (8 * i)) & 0xFF));
  out.insert(out.end(), v1.begin() + kManifestEnvelopeLenV1, v1.end());
  return out;
}

TEST(WalletCryptoManifest, V2CarriesAWalletVersionReadableWithoutTheKey) {
  const std::vector<uint8_t> v1 = encFixture("enc/manifest.enc");
  ASSERT_FALSE(v1.empty());
  const std::vector<uint8_t> v2 = v2FromV1(v1, 147);

  ManifestEnvelope envelope;
  ASSERT_TRUE(parseManifestEnvelope(v2.data(), v2.size(), envelope));
  EXPECT_TRUE(envelope.hasWalletVersion);
  EXPECT_EQ(envelope.walletVersion, 147u);
  EXPECT_EQ(envelope.ciphertextOffset, kManifestEnvelopeLen);
  EXPECT_EQ(envelope.plaintextLen, envelope.ciphertextLen);

  // No key was involved in reading it -- that is the whole point. The phone must
  // be able to compute pending work against a locked wallet (brief 23-26, 54).
  std::vector<uint8_t> plain(envelope.ciphertextLen);
  ASSERT_TRUE(wallet::host::gcmDecrypt(kTestKey, envelope.nonce, kGcmNonceLen, v2.data() + envelope.tagOffset,
                                       kGcmTagLen, v2.data() + envelope.ciphertextOffset, envelope.ciphertextLen,
                                       plain.data()));
}

TEST(WalletCryptoManifest, TheWalletVersionIsNotAuthenticated) {
  // It sits outside the GCM tag, so tampering with it cannot be detected. The
  // manifest must still decrypt -- the field is a hint, every asset is verified by
  // its own hash -- and nothing may treat it as trustworthy.
  const std::vector<uint8_t> v1 = encFixture("enc/manifest.enc");
  ASSERT_FALSE(v1.empty());
  std::vector<uint8_t> v2 = v2FromV1(v1, 5);
  v2[kManifestEnvelopeLenV1] ^= 0xFF;  // first byte of walletVersion

  ManifestEnvelope envelope;
  ASSERT_TRUE(parseManifestEnvelope(v2.data(), v2.size(), envelope));
  EXPECT_NE(envelope.walletVersion, 5u) << "the tamper must be visible in the value";
  std::vector<uint8_t> plain(envelope.ciphertextLen);
  EXPECT_TRUE(wallet::host::gcmDecrypt(kTestKey, envelope.nonce, kGcmNonceLen, v2.data() + envelope.tagOffset,
                                       kGcmTagLen, v2.data() + envelope.ciphertextOffset, envelope.ciphertextLen,
                                       plain.data()))
      << "a tampered version must not break the manifest";
}

TEST(WalletCryptoManifest, AnUnknownContainerVersionIsRefused) {
  const std::vector<uint8_t> v1 = encFixture("enc/manifest.enc");
  ASSERT_FALSE(v1.empty());
  std::vector<uint8_t> bad = v1;
  bad[4] = 9;
  ManifestEnvelope envelope;
  EXPECT_FALSE(parseManifestEnvelope(bad.data(), bad.size(), envelope));
}

TEST(WalletCryptoManifest, OneFlippedBitAnywhereFailsTheTag) {
  const std::vector<uint8_t> blob = encFixture("enc/manifest.enc");
  ASSERT_FALSE(blob.empty());
  ManifestEnvelope envelope;
  ASSERT_TRUE(parseManifestEnvelope(blob.data(), blob.size(), envelope));

  // Three places, three ways to be wrong, and all three must be refused BEFORE any
  // byte is parsed: a flipped ciphertext bit, a flipped tag bit, and the wrong key.
  const size_t targets[] = {envelope.ciphertextOffset, envelope.ciphertextOffset + envelope.ciphertextLen / 2,
                            envelope.tagOffset, envelope.tagOffset + kGcmTagLen - 1};
  for (const size_t at : targets) {
    std::vector<uint8_t> broken = blob;
    broken[at] ^= 0x01;
    std::vector<uint8_t> plain(envelope.ciphertextLen);
    EXPECT_FALSE(wallet::host::gcmDecrypt(kTestKey, envelope.nonce, kGcmNonceLen, broken.data() + envelope.tagOffset,
                                          kGcmTagLen, broken.data() + envelope.ciphertextOffset, envelope.ciphertextLen,
                                          plain.data()))
        << "flipped bit at " << at << " must fail the tag";
  }

  uint8_t wrongKey[kWalletKeyLen];
  std::memcpy(wrongKey, kTestKey, sizeof(wrongKey));
  wrongKey[0] ^= 0x80;
  std::vector<uint8_t> plain(envelope.ciphertextLen);
  EXPECT_FALSE(wallet::host::gcmDecrypt(wrongKey, envelope.nonce, kGcmNonceLen, blob.data() + envelope.tagOffset,
                                        kGcmTagLen, blob.data() + envelope.ciphertextOffset, envelope.ciphertextLen,
                                        plain.data()))
      << "the wrong key must fail the tag, not produce garbage JSON";
}

TEST(WalletCryptoManifest, EnvelopeRefusesWhatItCannotBe) {
  const std::vector<uint8_t> blob = encFixture("enc/manifest.enc");
  ASSERT_FALSE(blob.empty());
  ManifestEnvelope envelope;

  EXPECT_FALSE(parseManifestEnvelope(nullptr, blob.size(), envelope));
  // Too short to hold an envelope and a tag.
  EXPECT_FALSE(parseManifestEnvelope(blob.data(), kManifestEnvelopeLen + kGcmTagLen - 1, envelope));

  std::vector<uint8_t> bad = blob;
  bad[0] = 'X';
  EXPECT_FALSE(parseManifestEnvelope(bad.data(), bad.size(), envelope)) << "wrong magic";

  bad = blob;
  bad[4] = 2;
  EXPECT_FALSE(parseManifestEnvelope(bad.data(), bad.size(), envelope)) << "a version this firmware does not know";

  // plaintextLen has to agree with the file's shape. It is OUTSIDE the tag -- the
  // generator seals with no associated data -- so this check is the only thing that
  // catches a spliced length.
  bad = blob;
  bad[18] = static_cast<uint8_t>(bad[18] + 1);
  EXPECT_FALSE(parseManifestEnvelope(bad.data(), bad.size(), envelope)) << "a length that disagrees with the file";

  // A cap above the fixture, and a real refusal above it. The cap exists so an
  // oversized manifest gets a message instead of an allocation failure.
  EXPECT_GT(kMaxEncryptedManifestBytes, blob.size());
  EXPECT_EQ(kMaxEncryptedManifestBytes, 32u * 1024u);
}

TEST(WalletCryptoKek, ThePasswordIsPinThenSecretAndTheWrapRoundTrips) {
  // The formula, end to end, with OpenSSL standing in for mbedtls: the KEK is
  // PBKDF2(PIN bytes || deviceSecret), and it wraps K with AES-256-GCM.
  const char* pin = "UDLRUD";
  uint8_t secret[kDeviceSecretLen];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = static_cast<uint8_t>(0xA0 + i);
  const uint8_t salt[kPbkdf2SaltLen] = {0x78, 0xa3, 0x20, 0x45, 0xfc, 0x9d, 0x47, 0x94,
                                        0x87, 0x73, 0x18, 0xcd, 0x4d, 0x2a, 0xce, 0xc3};

  uint8_t password[kKekPasswordBufBytes];
  size_t passwordLen = 0;
  ASSERT_TRUE(buildKekPassword(pin, std::strlen(pin), secret, sizeof(secret), password, sizeof(password), passwordLen));
  EXPECT_EQ(passwordLen, std::strlen(pin) + sizeof(secret));
  EXPECT_EQ(std::memcmp(password, "UDLRUD", 6), 0) << "the PIN's ASCII comes first";
  EXPECT_EQ(std::memcmp(password + 6, secret, sizeof(secret)), 0) << "then the device secret, nothing between";

  // A short PIN, a wrong-sized secret and a small buffer are all refused rather
  // than padded into something that half works.
  size_t ignored = 0;
  EXPECT_FALSE(buildKekPassword("UDL", 3, secret, sizeof(secret), password, sizeof(password), ignored));
  EXPECT_FALSE(buildKekPassword(pin, std::strlen(pin), secret, 31, password, sizeof(password), ignored));
  EXPECT_FALSE(buildKekPassword(pin, std::strlen(pin), secret, sizeof(secret), password, 8, ignored));

  // Two iteration counts, one of them small enough to keep the test fast; the real
  // count is a device measurement (docs/wallet-crypto.md).
  uint8_t kek[kWalletKeyLen];
  ASSERT_TRUE(wallet::host::pbkdf2(password, passwordLen, salt, sizeof(salt), 1000, kek));

  // Wrap K, unwrap it, get K back.
  uint8_t wrapped[kWrappedKeyLen];
  for (size_t i = 0; i < kGcmNonceLen; ++i) wrapped[i] = static_cast<uint8_t>(i + 1);
  ASSERT_TRUE(wallet::host::gcmEncrypt(kek, wrapped, kGcmNonceLen, kTestKey, kWalletKeyLen, wrapped + kGcmNonceLen,
                                       wrapped + kGcmNonceLen + kWalletKeyLen, kGcmTagLen));
  uint8_t unwrapped[kWalletKeyLen];
  ASSERT_TRUE(wallet::host::gcmDecrypt(kek, wrapped, kGcmNonceLen, wrapped + kGcmNonceLen + kWalletKeyLen, kGcmTagLen,
                                       wrapped + kGcmNonceLen, kWalletKeyLen, unwrapped));
  EXPECT_EQ(std::memcmp(unwrapped, kTestKey, kWalletKeyLen), 0);

  // One wrong symbol in the PIN and the unwrap fails. Not "gives a wrong key" --
  // fails, because the wrap is authenticated.
  uint8_t wrongPassword[kKekPasswordBufBytes];
  size_t wrongLen = 0;
  ASSERT_TRUE(buildKekPassword("UDLRUU", 6, secret, sizeof(secret), wrongPassword, sizeof(wrongPassword), wrongLen));
  uint8_t wrongKek[kWalletKeyLen];
  ASSERT_TRUE(wallet::host::pbkdf2(wrongPassword, wrongLen, salt, sizeof(salt), 1000, wrongKek));
  EXPECT_NE(std::memcmp(wrongKek, kek, sizeof(kek)), 0);
  EXPECT_FALSE(wallet::host::gcmDecrypt(wrongKek, wrapped, kGcmNonceLen, wrapped + kGcmNonceLen + kWalletKeyLen,
                                        kGcmTagLen, wrapped + kGcmNonceLen, kWalletKeyLen, unwrapped))
      << "a wrong PIN must fail the unwrap, not return a plausible key";

  // And the device secret matters as much as the PIN: same PIN, another device, no
  // unwrap. That is what makes a leaked provisioning file not a wrap.
  uint8_t otherSecret[kDeviceSecretLen];
  std::memcpy(otherSecret, secret, sizeof(otherSecret));
  otherSecret[0] ^= 0x01;
  uint8_t otherPassword[kKekPasswordBufBytes];
  size_t otherLen = 0;
  ASSERT_TRUE(buildKekPassword(pin, std::strlen(pin), otherSecret, sizeof(otherSecret), otherPassword,
                               sizeof(otherPassword), otherLen));
  uint8_t otherKek[kWalletKeyLen];
  ASSERT_TRUE(wallet::host::pbkdf2(otherPassword, otherLen, salt, sizeof(salt), 1000, otherKek));
  EXPECT_FALSE(wallet::host::gcmDecrypt(otherKek, wrapped, kGcmNonceLen, wrapped + kGcmNonceLen + kWalletKeyLen,
                                        kGcmTagLen, wrapped + kGcmNonceLen, kWalletKeyLen, unwrapped));
}

TEST(WalletCryptoPin, NormalisesAndRefusesWhatCannotBeAPin) {
  char out[kPinBufBytes];
  size_t len = 0;

  ASSERT_TRUE(normalisePin("UDLRUD", out, sizeof(out), len));
  EXPECT_EQ(len, 6u);
  EXPECT_STREQ(out, "UDLRUD");
  // Lower case is the same PIN: the symbols are directions, not letters a rider
  // types with a shift key.
  ASSERT_TRUE(normalisePin("udlrud", out, sizeof(out), len));
  EXPECT_STREQ(out, "UDLRUD");
  ASSERT_TRUE(normalisePin("UUUUUUUUUU", out, sizeof(out), len));
  EXPECT_EQ(len, kPinMaxLen);

  // Too short, too long, and not a direction. Refused here so a typo is a message
  // rather than an attempt spent against the rate limiter.
  EXPECT_FALSE(normalisePin("UDLR", out, sizeof(out), len));
  EXPECT_FALSE(normalisePin("UUUUUUUUUUU", out, sizeof(out), len));
  EXPECT_FALSE(normalisePin("UDLR1D", out, sizeof(out), len));
  EXPECT_FALSE(normalisePin("UDLR D", out, sizeof(out), len));
  EXPECT_FALSE(normalisePin("", out, sizeof(out), len));
  EXPECT_FALSE(normalisePin(nullptr, out, sizeof(out), len));
  EXPECT_EQ(len, 0u) << "a refusal must not leave a length behind";

  // The four symbols round-trip through their ASCII, which is what the KDF password
  // is built from.
  for (const char c : std::string("UDLR")) {
    PinSymbol symbol = PinSymbol::Up;
    ASSERT_TRUE(pinSymbolFromChar(c, symbol));
    EXPECT_EQ(pinSymbolChar(symbol), c);
  }
}

TEST(WalletCryptoRateLimit, FreeTriesThenDoublingThenAHardStop) {
  // The state machine, as stated in the brief: a few free failures, exponential
  // delay after them, and a hard lockout at ten.
  EXPECT_EQ(pinFailureDelayMs(0), 0u);
  EXPECT_EQ(pinFailureDelayMs(3), 0u) << "three fat-fingered presses cost nothing";
  EXPECT_EQ(pinFailureDelayMs(4), 1000u);
  EXPECT_EQ(pinFailureDelayMs(5), 2000u);
  EXPECT_EQ(pinFailureDelayMs(6), 4000u);
  EXPECT_EQ(pinFailureDelayMs(7), 8000u);
  EXPECT_EQ(pinFailureDelayMs(8), 16000u);
  // Capped, so the screen never looks hung.
  EXPECT_EQ(pinFailureDelayMs(9), 30000u);
  EXPECT_EQ(pinFailureDelayMs(200), 30000u);

  EXPECT_FALSE(pinIsLockedOut(9));
  EXPECT_TRUE(pinIsLockedOut(10));
  EXPECT_TRUE(pinIsLockedOut(255));
  EXPECT_EQ(kMaxPinFailures, 10);
}

TEST(WalletCryptoHex, ExactLengthOrNothing) {
  uint8_t key[kWalletKeyLen];
  EXPECT_TRUE(hexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, sizeof(key)));
  EXPECT_EQ(std::memcmp(key, kTestKey, sizeof(key)), 0);
  // Upper case is the same key.
  EXPECT_TRUE(hexToBytes("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", key, sizeof(key)));
  EXPECT_EQ(std::memcmp(key, kTestKey, sizeof(key)), 0);

  // One character short, one long, one wrong: all refused. A key that is
  // zero-padded into place would half work, which is worse than not working.
  EXPECT_FALSE(hexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1", key, sizeof(key)));
  EXPECT_FALSE(hexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f00", key, sizeof(key)));
  EXPECT_FALSE(hexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1ezz", key, sizeof(key)));
  EXPECT_FALSE(hexToBytes("", key, sizeof(key)));
  EXPECT_FALSE(hexToBytes(nullptr, key, sizeof(key)));

  uint8_t salt[kPbkdf2SaltLen];
  EXPECT_TRUE(hexToBytes("78a32045fc9d4794877318cd4d2acec3", salt, sizeof(salt)));
  EXPECT_EQ(salt[0], 0x78);
  EXPECT_EQ(salt[15], 0xc3);
}

TEST(WalletCryptoWipe, SecureWipeClearsEveryByte) {
  uint8_t key[kWalletKeyLen];
  std::memcpy(key, kTestKey, sizeof(key));
  secureWipe(key, sizeof(key));
  for (size_t i = 0; i < sizeof(key); ++i) EXPECT_EQ(key[i], 0) << i;
  // A zero length is a no-op, not a crash.
  secureWipe(key, 0);
}

TEST(WalletUnlockOutcomes, EveryResultHasItsOwnStableToken) {
  // These tokens are the serial contract: a host script greps
  // `WALLETUNLOCK_ERR wrong_pin` and friends, so each result needs its own word and
  // renaming one breaks a caller. Asserted rather than trusted, because a switch
  // that falls through to "?" would look fine on screen and silently break a script.
  const UnlockResult all[] = {UnlockResult::Ok,      UnlockResult::NotProvisioned, UnlockResult::Malformed,
                              UnlockResult::Waiting, UnlockResult::LockedOut,      UnlockResult::BadPin};
  std::vector<std::string> seen;
  for (const UnlockResult result : all) {
    const std::string name = unlockResultName(result);
    EXPECT_FALSE(name.empty());
    EXPECT_STRNE(name.c_str(), "?") << "an unmapped result reaches the wire as '?'";
    EXPECT_EQ(name.find(' '), std::string::npos) << "'" << name << "' has a space: the reply is space-separated";
    EXPECT_EQ(std::count(seen.begin(), seen.end(), name), 0) << "'" << name << "' is used twice";
    seen.push_back(name);
  }
  EXPECT_EQ(seen.size(), 6u);
  // The two a host script keys on most, spelled out so a rename fails here.
  EXPECT_STREQ(unlockResultName(UnlockResult::BadPin), "wrong_pin");
  EXPECT_STREQ(unlockResultName(UnlockResult::LockedOut), "locked_out");
  EXPECT_STREQ(unlockResultName(UnlockResult::Waiting), "rate_limited");
  EXPECT_STREQ(unlockResultName(UnlockResult::NotProvisioned), "not_provisioned");
}

TEST(WalletUnlockOutcomes, TheDelayAHostWillWatchIsTheOneTheScreenEnforces) {
  // CMD:WALLETUNLOCK reports wait_ms straight out of pinFailureDelayMs() via the
  // session's gate, and the PIN screen reads the same gate. So the sequence a host
  // sees walking the limiter is exactly this, and it is worth pinning as a sequence
  // rather than as six separate cases.
  const uint32_t expected[] = {0, 0, 0, 0, 1000, 2000, 4000, 8000, 16000, 30000};
  for (uint8_t failures = 0; failures < 10; ++failures) {
    EXPECT_EQ(pinFailureDelayMs(failures), expected[failures]) << "after " << static_cast<int>(failures) << " failures";
  }
  // And the tenth failure is not a delay at all: it is the end.
  EXPECT_TRUE(pinIsLockedOut(kMaxPinFailures));
  EXPECT_FALSE(pinIsLockedOut(kMaxPinFailures - 1));
}

// ---------------------------------------------------------------------------
// P2b: grey. The two grey asset forms, the plane encoding they share, and the
// gates. docs/wallet-grey.md is the layout; docs/eink-grayscale.md is why the
// planes look the way they do.
//
// The device never looks at a grey pixel -- it streams baked planes straight to
// controller RAM -- so nothing on the device can notice a wrong bit order. These
// tests and the host preview are the only place it can be noticed at all.
// ---------------------------------------------------------------------------

namespace {

// The synthetic page, at panel size so the in-memory payloads stay small:
// 3 x 48,000 for the planes, 96,000 for the 2bpp form.
const wallet::host::GreyPage& greyPage() {
  static const wallet::host::GreyPage page = wallet::host::makeGreyPage(800, 480);
  return page;
}

// One a little larger than the panel, so a window has somewhere to pan to. Plane
// row 102 bytes, so the x limit is 16 px.
const wallet::host::GreyPage& greyPagePannable() {
  static const wallet::host::GreyPage page = wallet::host::makeGreyPage(816, 488);
  return page;
}

PageImageSpec greyPlanesSpec(const wallet::host::GreyPage& page, const char* id = "1234567890abcdef") {
  PageImageSpec spec;
  spec.present = true;
  std::strcpy(spec.assetId, id);
  spec.nativeWidth = page.width;
  spec.nativeHeight = page.height;
  spec.rowBytes = static_cast<uint16_t>(page.planeRowBytes);
  spec.rawLen = static_cast<uint32_t>(page.planes.size());
  spec.windowStepX = 8;
  spec.windowStepY = 8;
  return spec;
}

PageImageSpec greyImageSpec(const wallet::host::GreyPage& page, const char* id = "fedcba0987654321") {
  PageImageSpec spec;
  spec.present = true;
  std::strcpy(spec.assetId, id);
  spec.nativeWidth = page.width;
  spec.nativeHeight = page.height;
  spec.rowBytes = static_cast<uint16_t>(page.greyRowBytes);
  spec.rawLen = static_cast<uint32_t>(page.twoBpp.size());
  return spec;
}

uint8_t levelFromPlanesAt(const wallet::host::GreyPage& page, uint32_t x, uint32_t y) {
  const size_t planeBytes = static_cast<size_t>(page.planeRowBytes) * page.height;
  const uint8_t* base = page.planes.data() + static_cast<size_t>(y) * page.planeRowBytes;
  const uint8_t* lsb = base + planeBytes;
  const uint8_t* msb = base + planeBytes * 2;
  return wallet::host::levelFromPlaneRows(base, lsb, msb, x);
}

}  // namespace

TEST(WalletGreyEncoding, PlaneBitsAreTheLutTable) {
  // docs/eink-grayscale.md, "A grey pixel is a black pixel that got nudged
  // lighter": black/white 00 (no drive), light grey 10, dark grey 11. Plus the
  // base frame's own convention, where bit 1 is white and BOTH greys are ink.
  struct Row {
    uint8_t value;
    bool base;  // framebuffer bit: 1 = white
    bool lsb;   // controller bit: 1 = nudge
    bool msb;
  };
  const Row rows[] = {
      {kGreyValueBlack, false, false, false},
      {kGreyValueDarkGray, false, true, true},
      {kGreyValueLightGray, false, false, true},
      {kGreyValueWhite, true, false, false},
  };
  for (const Row& row : rows) {
    const GrayShade shade = greyShadeFromValue(row.value);
    EXPECT_EQ(greyPlaneBit(shade, GreyPlane::Base), row.base) << "base, value " << int(row.value);
    EXPECT_EQ(greyPlaneBit(shade, GreyPlane::Lsb), row.lsb) << "lsb, value " << int(row.value);
    EXPECT_EQ(greyPlaneBit(shade, GreyPlane::Msb), row.msb) << "msb, value " << int(row.value);
    // And the level is recoverable from the three bits, or the host preview draws
    // the wrong picture while the panel draws the right one.
    EXPECT_EQ(greyValueFromPlaneBits(row.base, row.lsb, row.msb), row.value);
    EXPECT_EQ(greyValueFromShade(shade), row.value);
  }
}

TEST(WalletGreyEncoding, LosingThePlanesReadsBlackAndNotWhite) {
  // The consequence that bites (docs/eink-grayscale.md): with both plane bits
  // clear, a grey pixel is whatever the base frame says -- and the base frame inks
  // both greys. So a dropped plane is a black page, not a washed-out one.
  for (const uint8_t value : {kGreyValueBlack, kGreyValueDarkGray, kGreyValueLightGray}) {
    const GrayShade shade = greyShadeFromValue(value);
    EXPECT_FALSE(greyPlaneBit(shade, GreyPlane::Base)) << "value " << int(value) << " must be ink in the base frame";
    EXPECT_EQ(greyValueFromPlaneBits(greyPlaneBit(shade, GreyPlane::Base), false, false), kGreyValueBlack);
  }
}

TEST(WalletGreyEncoding, TwoBppIsFourPixelsPerByteMsbFirst) {
  std::vector<uint8_t> row(1, 0);
  wallet::host::setTwoBppValue(row, 1, 0, 0, kGreyValueBlack);
  wallet::host::setTwoBppValue(row, 1, 1, 0, kGreyValueDarkGray);
  wallet::host::setTwoBppValue(row, 1, 2, 0, kGreyValueLightGray);
  wallet::host::setTwoBppValue(row, 1, 3, 0, kGreyValueWhite);
  // 00 01 10 11, pixel 0 in the TOP two bits.
  EXPECT_EQ(row[0], 0x1Bu);
  for (uint32_t x = 0; x < 4; ++x) EXPECT_EQ(wallet::host::twoBppValueAt(row.data(), x), x);
}

TEST(WalletGreyEncoding, StridesAreTheDocumentedFormulas) {
  EXPECT_EQ(greyPlaneRowBytes(800), 100u);
  EXPECT_EQ(greyRowBytes2bpp(800), 200u);
  // X3: 792 px.
  EXPECT_EQ(greyPlaneRowBytes(792), 99u);
  EXPECT_EQ(greyRowBytes2bpp(792), 198u);
  // A width that is not a whole number of bytes rounds UP in both forms, or the
  // last pixels of every row fall off.
  EXPECT_EQ(greyPlaneRowBytes(801), 101u);
  EXPECT_EQ(greyRowBytes2bpp(801), 201u);
  EXPECT_EQ(greyRowBytes2bpp(802), 201u);
  EXPECT_EQ(greyRowBytes2bpp(803), 201u);
  EXPECT_EQ(greyRowBytes2bpp(804), 201u);
}

TEST(WalletGreyEncoding, TheTwoFormsOfOnePageAgreeOnEveryPixel) {
  const wallet::host::GreyPage& page = greyPage();
  ASSERT_EQ(page.planes.size(), static_cast<size_t>(page.planeRowBytes) * page.height * kGreyPlaneCount);
  ASSERT_EQ(page.twoBpp.size(), static_cast<size_t>(page.greyRowBytes) * page.height);

  size_t counts[kGreyValues] = {0, 0, 0, 0};
  for (uint32_t y = 0; y < page.height; ++y) {
    const uint8_t* twoBppRow = page.twoBpp.data() + static_cast<size_t>(y) * page.greyRowBytes;
    for (uint32_t x = 0; x < page.width; ++x) {
      const uint8_t want = page.levels[static_cast<size_t>(y) * page.width + x];
      ASSERT_EQ(levelFromPlanesAt(page, x, y), want) << "planes at " << x << "," << y;
      ASSERT_EQ(wallet::host::twoBppValueAt(twoBppRow, x), want) << "2bpp at " << x << "," << y;
      counts[want]++;
    }
  }
  // All four levels have to be in the test pattern, or this test proves nothing
  // about the two mid tones -- which are the only ones the encoding can get wrong.
  for (uint8_t value = 0; value < kGreyValues; ++value) EXPECT_GT(counts[value], 0u) << "level " << int(value);
}

TEST(WalletGreyOffsets, EachPlaneStartsWhereTheLastOneEnded) {
  const PageImageSpec spec = greyPlanesSpec(greyPage());
  const uint32_t planeBytes = greyPlaneBytes(spec);
  EXPECT_EQ(planeBytes, 48000u);
  EXPECT_EQ(greyPlaneOffset(spec, GreyPlane::Base), 0u);
  EXPECT_EQ(greyPlaneOffset(spec, GreyPlane::Lsb), planeBytes);
  EXPECT_EQ(greyPlaneOffset(spec, GreyPlane::Msb), planeBytes * 2u);
  EXPECT_EQ(greyPlaneOffset(spec, GreyPlane::Msb) + planeBytes, spec.rawLen);
}

TEST(WalletGreyGate, AcceptsTheBakedPlanesAndThe2bppImage) {
  const wallet::host::GreyPage& page = greyPage();
  const PageImageSpec planes = greyPlanesSpec(page);
  const PageImageSpec image = greyImageSpec(page);
  AssetHeader h;

  const auto planesFile =
      wallet::host::buildAssetFile(AssetType::GreyPlanes, kBitDepth2, page.width, page.height, page.planes);
  EXPECT_EQ(checkGreyPlanes(planesFile.data(), planesFile.size(), planes, kPanelX4, h), AssetCheck::Ok);
  EXPECT_EQ(h.bitDepth, kBitDepth2);

  const auto imageFile =
      wallet::host::buildAssetFile(AssetType::PageImageGrey, kBitDepth2, page.width, page.height, page.twoBpp);
  EXPECT_EQ(checkGreyImage(imageFile.data(), imageFile.size(), image, kPanelX4, h), AssetCheck::Ok);

  // The X3's panel is wider than this page, so no window can fill it. Refused as a
  // wrong-device asset, not as a corrupt one.
  EXPECT_EQ(checkGreyPlanes(planesFile.data(), planesFile.size(), planes, kPanelX3, h), AssetCheck::WrongPanel);
}

TEST(WalletGreyGate, RefusesTheOtherFormInEitherSlot) {
  const wallet::host::GreyPage& page = greyPage();
  AssetHeader h;
  const auto planesFile =
      wallet::host::buildAssetFile(AssetType::GreyPlanes, kBitDepth2, page.width, page.height, page.planes);
  const auto imageFile =
      wallet::host::buildAssetFile(AssetType::PageImageGrey, kBitDepth2, page.width, page.height, page.twoBpp);
  // Swapped: the manifest's greyPlanes pointing at the 2bpp asset would have the
  // device stream a third of an image into the controller as plane bits.
  EXPECT_EQ(checkGreyPlanes(imageFile.data(), imageFile.size(), greyImageSpec(page), kPanelX4, h),
            AssetCheck::PageImageMismatch);
  EXPECT_EQ(checkGreyImage(planesFile.data(), planesFile.size(), greyPlanesSpec(page), kPanelX4, h),
            AssetCheck::PageImageMismatch);
  // And a 1bpp page image is not a grey asset either, whatever slot names it.
  const auto oneBpp = wallet::host::buildAssetFile(AssetType::PageImage, kBitDepth1, page.width, page.height,
                                                   wallet::host::makeOneBppPage(page));
  EXPECT_EQ(checkGreyPlanes(oneBpp.data(), oneBpp.size(), greyPlanesSpec(page), kPanelX4, h), AssetCheck::BitDepth);
}

TEST(WalletGreyGate, RefusesBitDepthThatIsNotTwo) {
  const wallet::host::GreyPage& page = greyPage();
  AssetHeader h;
  // Right type, right lengths, wrong depth. Both grey forms say 2, because the
  // field describes the picture and not the stride of one plane.
  const auto planesFile =
      wallet::host::buildAssetFile(AssetType::GreyPlanes, kBitDepth1, page.width, page.height, page.planes);
  EXPECT_EQ(checkGreyPlanes(planesFile.data(), planesFile.size(), greyPlanesSpec(page), kPanelX4, h),
            AssetCheck::BitDepth);
  // And the 1bpp gates keep refusing a 2bpp asset, which is what stops the old
  // reader from drawing a grey page at half width.
  const auto greyFile =
      wallet::host::buildAssetFile(AssetType::GreyPlanes, kBitDepth2, page.width, page.height, page.planes);
  EXPECT_EQ(checkAssetForPanel(greyFile.data(), greyFile.size(), kPanelX4, h), AssetCheck::BitDepth);
  EXPECT_EQ(checkPageImage(greyFile.data(), greyFile.size(), greyPlanesSpec(page), kPanelX4, h), AssetCheck::BitDepth);
}

TEST(WalletGreyGate, RefusesARawLenThatIsNotThreeWholePlanes) {
  const wallet::host::GreyPage& page = greyPage();
  AssetHeader h;
  const auto file =
      wallet::host::buildAssetFile(AssetType::GreyPlanes, kBitDepth2, page.width, page.height, page.planes);

  // A manifest that describes one plane where the file holds three: the classic
  // way a reader ends up streaming the base frame as a plane.
  PageImageSpec onePlane = greyPlanesSpec(page);
  onePlane.rawLen = greyPlaneBytes(onePlane);
  EXPECT_EQ(checkGreyPlanes(file.data(), file.size(), onePlane, kPanelX4, h), AssetCheck::PageImageMismatch);

  // A header whose rawLen disagrees with the manifest's, both otherwise valid.
  const auto shortFile =
      wallet::host::buildAssetFile(AssetType::GreyPlanes, kBitDepth2, page.width, page.height,
                                   std::vector<uint8_t>(page.planes.begin(), page.planes.end() - page.planeRowBytes));
  EXPECT_EQ(checkGreyPlanes(shortFile.data(), shortFile.size(), greyPlanesSpec(page), kPanelX4, h),
            AssetCheck::PageImageMismatch);
}

TEST(WalletGreyGate, RefusesAStrideTooNarrowForTheWidth) {
  const wallet::host::GreyPage& page = greyPage();
  AssetHeader h;
  const auto file =
      wallet::host::buildAssetFile(AssetType::GreyPlanes, kBitDepth2, page.width, page.height, page.planes);
  PageImageSpec narrow = greyPlanesSpec(page);
  narrow.rowBytes = 99;  // 792 px of stride for an 800 px page
  narrow.rawLen = static_cast<uint32_t>(narrow.rowBytes) * narrow.nativeHeight * kGreyPlaneCount;
  // Caught as a mismatch rather than drawn: a stride one byte short skews every
  // row a little further than the last, which reads as a bent document instead of
  // as a broken file.
  EXPECT_EQ(checkGreyPlanes(file.data(), file.size(), narrow, kPanelX4, h), AssetCheck::PageImageMismatch);
}

TEST(WalletGreyGate, EncryptedNeedsAKeyExactlyLikeEveryOtherAsset) {
  const wallet::host::GreyPage& page = greyPage();
  AssetHeader h;
  const auto file = wallet::host::buildAssetFile(AssetType::GreyPlanes, kBitDepth2, page.width, page.height,
                                                 page.planes, /*version=*/1, /*encryptedFlag=*/true);
  const PageImageSpec spec = greyPlanesSpec(page);
  EXPECT_EQ(checkGreyPlanes(file.data(), file.size(), spec, kPanelX4, h, /*allowEncrypted=*/false),
            AssetCheck::Encrypted);
  EXPECT_EQ(checkGreyPlanes(file.data(), file.size(), spec, kPanelX4, h, /*allowEncrypted=*/true), AssetCheck::Ok);
  EXPECT_NE(h.flags & kFlagEncrypted, 0);
}

TEST(WalletGreyCrypto, EveryPlaneRowOfAWindowDecryptsAtItsOwnPayloadOffset) {
  // The device reads a grey window as 3 x 480 rows, each at an arbitrary payload
  // offset, and decrypts each one where it lies -- readPlaneWindow() in
  // WalletStore.cpp, called once per plane with that plane's base offset. The
  // offsets in the second and third plane are the ones a naive implementation gets
  // wrong, because they are past the first 48,000 bytes.
  const wallet::host::GreyPage& page = greyPagePannable();
  const PageImageSpec spec = greyPlanesSpec(page);
  const uint32_t planeBytes = greyPlaneBytes(spec);

  uint8_t key[kWalletKeyLen];
  for (size_t i = 0; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(0x40 + i);
  uint8_t iv[kAssetIvLen];
  ASSERT_TRUE(buildAssetIv(spec.assetId, /*version=*/1, iv));

  // One CTR stream over the whole payload, exactly as the generator writes it.
  std::vector<uint8_t> cipher = page.planes;
  wallet::host::ctrXor(key, iv, 0, cipher.data(), cipher.size());
  ASSERT_NE(cipher, page.planes);

  const uint32_t panelRowBytes = kPanelX4.rowBytes;
  const uint32_t x = 8;  // 8-aligned and not zero: xByte = 1
  const uint32_t y = 7;  // not a band boundary either
  ASSERT_LE(x + kPanelX4.width, page.width);
  ASSERT_LE(y + kPanelX4.height, page.height);

  std::vector<uint8_t> rows[kGreyPlaneCount];
  for (uint32_t r = 0; r < kPanelX4.height; ++r) {
    for (uint8_t plane = 0; plane < kGreyPlaneCount; ++plane) {
      const uint32_t payloadOffset = plane * planeBytes + (y + r) * spec.rowBytes + x / 8u;
      rows[plane].assign(cipher.begin() + payloadOffset, cipher.begin() + payloadOffset + panelRowBytes);
      wallet::host::ctrXor(key, iv, payloadOffset, rows[plane].data(), rows[plane].size());
    }
    for (uint32_t px = 0; px < kPanelX4.width; ++px) {
      const uint8_t got = wallet::host::levelFromPlaneRows(rows[0].data(), rows[1].data(), rows[2].data(), px);
      const uint8_t want = page.levels[static_cast<size_t>(y + r) * page.width + x + px];
      ASSERT_EQ(got, want) << "row " << r << " px " << px;
    }
  }
}

TEST(WalletGreyCrypto, TheWrongKeyDoesNotDecodeToThePicture) {
  const wallet::host::GreyPage& page = greyPage();
  const PageImageSpec spec = greyPlanesSpec(page);
  uint8_t key[kWalletKeyLen];
  uint8_t wrong[kWalletKeyLen];
  for (size_t i = 0; i < sizeof(key); ++i) {
    key[i] = static_cast<uint8_t>(0x40 + i);
    wrong[i] = static_cast<uint8_t>(0x41 + i);
  }
  uint8_t iv[kAssetIvLen];
  ASSERT_TRUE(buildAssetIv(spec.assetId, 1, iv));

  std::vector<uint8_t> cipher = page.planes;
  wallet::host::ctrXor(key, iv, 0, cipher.data(), cipher.size());
  std::vector<uint8_t> bad = cipher;
  wallet::host::ctrXor(wrong, iv, 0, bad.data(), bad.size());
  EXPECT_NE(bad, page.planes);
  // A window carries no hash to check itself against -- it is a fraction of the
  // payload -- so a wrong key on a grey page shows as a wrong picture, exactly as
  // it does on a 1bpp page image. What catches a wrong key is the manifest's GCM
  // tag, which fails before any assetId is even read (docs/wallet-crypto.md).
  const uint8_t* base = bad.data();
  size_t differing = 0;
  for (uint32_t x = 0; x < page.width; ++x) {
    const uint8_t want = page.levels[x];
    const uint8_t got =
        wallet::host::levelFromPlaneRows(base, base + greyPlaneBytes(spec), base + greyPlaneBytes(spec) * 2, x);
    if (got != want) ++differing;
  }
  EXPECT_GT(differing, page.width / 4) << "a wrong key must not produce the picture";
}

TEST(WalletGreyManifest, ReadsBothGreyFormsForTheRequestedLevelOnly) {
  // The keys the generator has to write, in the shape the level object already
  // uses for `pageImage`.
  const char* json = R"JSON({
  "formatVersion": 1,
  "items": [{
    "title": "Grey both ways",
    "pages": [{
      "levels": {
        "fit": {
          "cols": 1, "rows": 1,
          "pageImage": {"assetId": "aaaaaaaaaaaaaaaa", "nativeWidth": 800, "nativeHeight": 480,
                        "rowBytes": 100, "rawLen": 48000, "windowStepX": 240, "windowStepY": 160,
                        "focalX": 8, "focalY": 16},
          "greyPlanes": {"assetId": "bbbbbbbbbbbbbbbb", "nativeWidth": 800, "nativeHeight": 480,
                         "rowBytes": 100, "rawLen": 144000, "windowStepX": 240, "windowStepY": 160,
                         "focalX": 8, "focalY": 16},
          "greyPageImage": {"assetId": "cccccccccccccccc", "nativeWidth": 800, "nativeHeight": 480,
                            "rowBytes": 200, "rawLen": 96000}
        },
        "detail": {
          "cols": 1, "rows": 1,
          "greyPlanes": {"assetId": "dddddddddddddddd", "nativeWidth": 1600, "nativeHeight": 960,
                         "rowBytes": 200, "rawLen": 576000}
        }
      }
    }]
  }]
})JSON";

  ManifestParser fit;
  fit.beginLookup(0, 0, Level::Fit, 0, 0);
  fit.feed(json, std::strlen(json));
  ASSERT_FALSE(fit.hasError());
  const PageLookup& f = fit.lookup();
  ASSERT_TRUE(f.pageImage.present);
  ASSERT_TRUE(f.greyPlanes.present);
  ASSERT_TRUE(f.greyImage.present);
  EXPECT_STREQ(f.pageImage.assetId, "aaaaaaaaaaaaaaaa");
  EXPECT_STREQ(f.greyPlanes.assetId, "bbbbbbbbbbbbbbbb");
  EXPECT_STREQ(f.greyImage.assetId, "cccccccccccccccc");
  // Three objects, three slots: a field of one must never land in another.
  EXPECT_EQ(f.greyPlanes.rawLen, 144000u);
  EXPECT_EQ(f.greyPlanes.rowBytes, 100u);
  EXPECT_EQ(f.greyImage.rawLen, 96000u);
  EXPECT_EQ(f.greyImage.rowBytes, 200u);
  EXPECT_EQ(f.pageImage.rawLen, 48000u);
  EXPECT_EQ(f.greyPlanes.focalX, 8u);
  EXPECT_EQ(f.greyPlanes.windowStepY, 160u);

  // The other level's grey asset does not leak into this answer.
  ManifestParser detail;
  detail.beginLookup(0, 0, Level::Detail, 0, 0);
  detail.feed(json, std::strlen(json));
  ASSERT_FALSE(detail.hasError());
  const PageLookup& d = detail.lookup();
  EXPECT_FALSE(d.pageImage.present);
  EXPECT_FALSE(d.greyImage.present);
  ASSERT_TRUE(d.greyPlanes.present);
  EXPECT_STREQ(d.greyPlanes.assetId, "dddddddddddddddd");
  EXPECT_EQ(d.greyPlanes.nativeWidth, 1600u);
}

TEST(WalletGreyManifest, AGreyAssetIdThatIsNotSixteenHexIsNoGreyAsset) {
  const char* json = R"JSON({
  "formatVersion": 1,
  "items": [{"title": "Bad ids", "pages": [{"levels": {"fit": {
    "greyPlanes": {"assetId": "../../etc/passwd", "nativeWidth": 800, "nativeHeight": 480,
                   "rowBytes": 100, "rawLen": 144000},
    "greyPageImage": {"assetId": "nope", "nativeWidth": 800, "nativeHeight": 480,
                      "rowBytes": 200, "rawLen": 96000}
  }}}]}]
})JSON";
  ManifestParser parser;
  parser.beginLookup(0, 0, Level::Fit, 0, 0);
  parser.feed(json, std::strlen(json));
  ASSERT_FALSE(parser.hasError());
  // Same rule as every other assetId: the id becomes a path, so anything that is
  // not 16 hex characters is not an asset at all rather than one that fails to open.
  EXPECT_FALSE(parser.lookup().greyPlanes.present);
  EXPECT_FALSE(parser.lookup().greyImage.present);
}

TEST(WalletGreyManifest, ACardWithNoGreyAssetsReadsExactlyAsItDidBefore) {
  // The regression that matters: P2b must be invisible to a tree generated before
  // it. kManifest is the pre-grey fixture used by every test above.
  for (const Level level : {Level::Fit, Level::Detail, Level::OneToOne}) {
    ManifestParser parser;
    parser.beginLookup(0, 0, level, 0, 0);
    parser.feed(kManifest, std::strlen(kManifest));
    ASSERT_FALSE(parser.hasError());
    EXPECT_TRUE(parser.lookup().itemFound);
    EXPECT_FALSE(parser.lookup().greyPlanes.present) << levelName(level);
    EXPECT_FALSE(parser.lookup().greyImage.present) << levelName(level);
  }
}
