#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "lib/JsonParser/ReleaseJsonParser.h"

namespace {

// Suffix used by most tests below -- equivalent to the old exact-match
// "firmware.bin" behavior when the asset name has nothing before it, and the
// behavior this parser actually needs: ReleaseJsonParser now matches assets
// by a caller-supplied SUFFIX (see setFirmwareAssetSuffix()), because a
// published asset's name embeds the device/chip, not just "firmware.bin".
constexpr const char* kAnyFirmwareSuffix = "firmware.bin";

const char* kRealisticPretty = R"({
  "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345",
  "assets_url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/assets",
  "upload_url": "https://uploads.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/assets{?name,label}",
  "html_url": "https://github.com/crosspoint-reader/crosspoint-reader/releases/tag/v2.4.1",
  "id": 12345,
  "author": {
    "login": "releasebot",
    "id": 99887766,
    "node_id": "MDQ6VXNlcjk5ODg3NzY2",
    "avatar_url": "https://avatars.githubusercontent.com/u/99887766?v=4",
    "url": "https://api.github.com/users/releasebot",
    "type": "User",
    "site_admin": false
  },
  "node_id": "RE_kwDOAbCdEf4AADBN",
  "tag_name": "v2.4.1",
  "target_commitish": "main",
  "name": "CrossPoint Reader v2.4.1",
  "draft": false,
  "prerelease": false,
  "created_at": "2026-04-28T10:00:00Z",
  "published_at": "2026-04-28T10:30:00Z",
  "assets": [
    {
      "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100001",
      "id": 100001,
      "node_id": "RA_kwDOAbCdEf4AAGHR",
      "name": "crosspoint-reader-v2.4.1-source.zip",
      "label": null,
      "uploader": {
        "login": "releasebot",
        "id": 99887766,
        "node_id": "MDQ6VXNlcjk5ODg3NzY2",
        "type": "User"
      },
      "content_type": "application/zip",
      "state": "uploaded",
      "size": 2048576,
      "download_count": 42,
      "created_at": "2026-04-28T10:15:00Z",
      "updated_at": "2026-04-28T10:15:30Z",
      "browser_download_url": "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/crosspoint-reader-v2.4.1-source.zip"
    },
    {
      "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100002",
      "id": 100002,
      "node_id": "RA_kwDOAbCdEf4AAGHS",
      "name": "firmware.bin",
      "label": "ESP32-C3 Firmware",
      "uploader": {
        "login": "releasebot",
        "id": 99887766,
        "node_id": "MDQ6VXNlcjk5ODg3NzY2",
        "type": "User"
      },
      "content_type": "application/octet-stream",
      "state": "uploaded",
      "size": 1572864,
      "download_count": 187,
      "created_at": "2026-04-28T10:16:00Z",
      "updated_at": "2026-04-28T10:16:45Z",
      "browser_download_url": "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin"
    },
    {
      "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100003",
      "id": 100003,
      "node_id": "RA_kwDOAbCdEf4AAGHR",
      "name": "checksums.sha256",
      "label": null,
      "uploader": {
        "login": "releasebot",
        "id": 99887766,
        "node_id": "MDQ6VXNlcjk5ODg3NzY2",
        "type": "User"
      },
      "content_type": "text/plain",
      "state": "uploaded",
      "size": 192,
      "download_count": 15,
      "created_at": "2026-04-28T10:17:00Z",
      "updated_at": "2026-04-28T10:17:10Z",
      "browser_download_url": "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/checksums.sha256"
    }
  ],
  "tarball_url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/tarball/v2.4.1",
  "zipball_url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/zipball/v2.4.1",
  "body": "## What's Changed\n\n* Fixed orientation crash (#123)\n* Improved EPUB rendering performance\n* Added Serbian translation\n\n**Full Changelog**: https://github.com/crosspoint-reader/crosspoint-reader/compare/v2.4.0...v2.4.1",
  "reactions": {
    "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/reactions",
    "total_count": 5,
    "+1": 3,
    "-1": 0,
    "laugh": 1,
    "hooray": 1,
    "confused": 0,
    "heart": 0,
    "rocket": 0,
    "eyes": 0
  }
})";

const char* kRealisticMinified =
    R"({"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345","assets_url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/assets","id":12345,"author":{"login":"releasebot","id":99887766,"node_id":"MDQ6VXNlcjk5ODg3NzY2","type":"User","site_admin":false},"tag_name":"v2.4.1","target_commitish":"main","name":"CrossPoint Reader v2.4.1","draft":false,"prerelease":false,"assets":[{"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100001","id":100001,"name":"crosspoint-reader-v2.4.1-source.zip","uploader":{"login":"releasebot","id":99887766},"content_type":"application/zip","state":"uploaded","size":2048576,"download_count":42,"browser_download_url":"https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/crosspoint-reader-v2.4.1-source.zip"},{"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100002","id":100002,"name":"firmware.bin","uploader":{"login":"releasebot","id":99887766},"content_type":"application/octet-stream","state":"uploaded","size":1572864,"download_count":187,"browser_download_url":"https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin"},{"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100003","id":100003,"name":"checksums.sha256","uploader":{"login":"releasebot","id":99887766},"content_type":"text/plain","state":"uploaded","size":192,"download_count":15,"browser_download_url":"https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/checksums.sha256"}],"body":"## What's Changed\n\n* Fixed orientation crash","reactions":{"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/reactions","total_count":5,"+1":3}})";

void feedChunked(ReleaseJsonParser& p, const char* json, size_t chunkSize) {
  size_t len = strlen(json);
  for (size_t off = 0; off < len; off += chunkSize) {
    size_t n = len - off < chunkSize ? len - off : chunkSize;
    p.feed(json + off, n);
  }
}

}  // namespace

TEST(ReleaseJsonParser, RealisticPrettyPrinted) {
  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(kRealisticPretty, strlen(kRealisticPretty));

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v2.4.1");
  EXPECT_STREQ(p.getFirmwareUrl(),
               "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin");
  EXPECT_EQ(p.getFirmwareSize(), 1572864u);
}

TEST(ReleaseJsonParser, RealisticMinified) {
  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(kRealisticMinified, strlen(kRealisticMinified));

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v2.4.1");
  EXPECT_STREQ(p.getFirmwareUrl(),
               "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin");
  EXPECT_EQ(p.getFirmwareSize(), 1572864u);
}

TEST(ReleaseJsonParser, PrettyAndMinifiedAgree) {
  ReleaseJsonParser pretty;
  pretty.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  pretty.feed(kRealisticPretty, strlen(kRealisticPretty));

  ReleaseJsonParser minified;
  minified.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  minified.feed(kRealisticMinified, strlen(kRealisticMinified));

  ASSERT_TRUE(pretty.foundTag());
  ASSERT_TRUE(pretty.foundFirmware());
  ASSERT_TRUE(minified.foundTag());
  ASSERT_TRUE(minified.foundFirmware());

  EXPECT_STREQ(pretty.getTagName(), minified.getTagName());
  EXPECT_STREQ(pretty.getFirmwareUrl(), minified.getFirmwareUrl());
  EXPECT_EQ(pretty.getFirmwareSize(), minified.getFirmwareSize());
}

TEST(ReleaseJsonParser, FirmwareNotFirstAsset) {
  const char* json = R"({
      "tag_name": "v1.0.0",
      "assets": [
        {"name": "source.tar.gz", "browser_download_url": "https://example.com/src.tar.gz", "size": 500000},
        {"name": "docs.pdf", "browser_download_url": "https://example.com/docs.pdf", "size": 120000},
        {"name": "firmware.bin", "browser_download_url": "https://example.com/firmware.bin", "size": 987654},
        {"name": "checksums.txt", "browser_download_url": "https://example.com/checksums.txt", "size": 256}
      ]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v1.0.0");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/firmware.bin");
  EXPECT_EQ(p.getFirmwareSize(), 987654u);
}

TEST(ReleaseJsonParser, FieldOrderUrlBeforeName) {
  const char* json = R"({
      "tag_name": "v3.0",
      "assets": [{
        "browser_download_url": "https://example.com/fw.bin",
        "name": "firmware.bin",
        "size": 2222
      }]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/fw.bin");
  EXPECT_EQ(p.getFirmwareSize(), 2222u);
}

TEST(ReleaseJsonParser, FieldOrderSizeBeforeUrl) {
  const char* json = R"({
      "tag_name": "v3.1",
      "assets": [{
        "size": 3333,
        "browser_download_url": "https://example.com/fw2.bin",
        "name": "firmware.bin"
      }]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/fw2.bin");
  EXPECT_EQ(p.getFirmwareSize(), 3333u);
}

TEST(ReleaseJsonParser, FieldOrderNameFirst) {
  const char* json = R"({
      "tag_name": "v3.2",
      "assets": [{
        "name": "firmware.bin",
        "size": 4444,
        "browser_download_url": "https://example.com/fw3.bin"
      }]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/fw3.bin");
  EXPECT_EQ(p.getFirmwareSize(), 4444u);
}

TEST(ReleaseJsonParser, AssetsBeforeTagName) {
  // tag_name appears after assets in the JSON
  const char* json = R"({
      "name": "Release",
      "assets": [{
        "name": "firmware.bin",
        "browser_download_url": "https://example.com/fw.bin",
        "size": 5555
      }],
      "tag_name": "v4.0"
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v4.0");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/fw.bin");
  EXPECT_EQ(p.getFirmwareSize(), 5555u);
}

TEST(ReleaseJsonParser, ChunkedFeedingSmallChunks) {
  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  feedChunked(p, kRealisticPretty, 64);

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v2.4.1");
  EXPECT_STREQ(p.getFirmwareUrl(),
               "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin");
  EXPECT_EQ(p.getFirmwareSize(), 1572864u);
}

TEST(ReleaseJsonParser, ChunkedFeedingByteByByte) {
  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  feedChunked(p, kRealisticMinified, 1);

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v2.4.1");
  EXPECT_EQ(p.getFirmwareSize(), 1572864u);
}

TEST(ReleaseJsonParser, ChunkedFeedingVariousChunkSizes) {
  for (size_t chunkSize : {3u, 7u, 13u, 31u, 97u, 128u, 256u, 512u, 1024u}) {
    ReleaseJsonParser p;
    p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
    feedChunked(p, kRealisticPretty, chunkSize);

    EXPECT_TRUE(p.foundTag()) << "chunkSize=" << chunkSize;
    EXPECT_TRUE(p.foundFirmware()) << "chunkSize=" << chunkSize;
    EXPECT_STREQ(p.getTagName(), "v2.4.1") << "chunkSize=" << chunkSize;
    EXPECT_STREQ(p.getFirmwareUrl(),
                 "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin")
        << "chunkSize=" << chunkSize;
    EXPECT_EQ(p.getFirmwareSize(), 1572864u) << "chunkSize=" << chunkSize;
  }
}

TEST(ReleaseJsonParser, MissingTagName) {
  const char* json = R"({
      "name": "Some Release",
      "draft": false,
      "assets": [{
        "name": "firmware.bin",
        "browser_download_url": "https://example.com/fw.bin",
        "size": 1000
      }]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_FALSE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "");
}

TEST(ReleaseJsonParser, MissingFirmwareBinAsset) {
  const char* json = R"({
      "tag_name": "v1.0.0",
      "assets": [
        {"name": "source.zip", "browser_download_url": "https://example.com/src.zip", "size": 1000},
        {"name": "docs.tar.gz", "browser_download_url": "https://example.com/docs.tar.gz", "size": 2000}
      ]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_FALSE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v1.0.0");
  EXPECT_STREQ(p.getFirmwareUrl(), "");
  EXPECT_EQ(p.getFirmwareSize(), 0u);
}

TEST(ReleaseJsonParser, EmptyAssetsArray) {
  const char* json = R"({"tag_name": "v1.0.0", "assets": []})";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_FALSE(p.foundFirmware());
}

TEST(ReleaseJsonParser, NoAssetsKey) {
  const char* json = R"({"tag_name": "v1.0.0", "name": "Release"})";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_FALSE(p.foundFirmware());
}

TEST(ReleaseJsonParser, TruncatedBeforeTagValue) {
  const char* json = R"({"tag_name": )";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_FALSE(p.foundTag());
  EXPECT_FALSE(p.foundFirmware());
}

TEST(ReleaseJsonParser, TruncatedInsideTagValue) {
  const char* json = R"({"tag_name": "v2.4)";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_FALSE(p.foundTag());
}

TEST(ReleaseJsonParser, TruncatedInsideAssetsArray) {
  const char* json = R"({"tag_name": "v2.4.1", "assets": [{"name": "firm)";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_STREQ(p.getTagName(), "v2.4.1");
  EXPECT_FALSE(p.foundFirmware());
}

TEST(ReleaseJsonParser, TruncatedAfterFirmwareName) {
  // Found the name but connection dropped before URL/size
  const char* json = R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_dow)";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_FALSE(p.foundFirmware());
}

TEST(ReleaseJsonParser, TruncatedRealisticJson) {
  std::string full(kRealisticPretty);
  for (size_t cutPoint : {10u, 50u, 100u, 200u, 500u, 1000u, 1500u, 2000u}) {
    if (cutPoint >= full.size()) continue;

    ReleaseJsonParser p;
    p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
    p.feed(full.c_str(), cutPoint);
    (void)p.foundTag();
    (void)p.foundFirmware();
  }
  SUCCEED();
}

TEST(ReleaseJsonParser, NestedObjectsInAsset) {
  // Asset with deeply nested "uploader" object -- should not confuse depth tracking
  const char* json = R"({
      "tag_name": "v5.0",
      "assets": [{
        "name": "firmware.bin",
        "uploader": {
          "login": "bot",
          "id": 42,
          "permissions": {"admin": false, "push": true, "pull": true}
        },
        "browser_download_url": "https://example.com/fw5.bin",
        "size": 8888
      }]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/fw5.bin");
  EXPECT_EQ(p.getFirmwareSize(), 8888u);
}

TEST(ReleaseJsonParser, NestedObjectsAtTopLevel) {
  // Multiple nested objects at the top level before/after tag_name and assets
  const char* json = R"({
      "author": {"login": "dev", "id": 1, "nested": {"deep": true}},
      "tag_name": "v6.0",
      "reactions": {"url": "https://reactions", "total_count": 0, "+1": 0},
      "assets": [{"name": "firmware.bin", "browser_download_url": "https://fw6", "size": 1111}],
      "mentions_count": 3
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v6.0");
  EXPECT_EQ(p.getFirmwareSize(), 1111u);
}

TEST(ReleaseJsonParser, ArraysAtTopLevel) {
  // A non-assets array at the top level should not interfere
  const char* json = R"({
      "tag_name": "v7.0",
      "labels": ["release", "stable"],
      "assets": [{"name": "firmware.bin", "browser_download_url": "https://fw7", "size": 7070}]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v7.0");
  EXPECT_EQ(p.getFirmwareSize(), 7070u);
}

TEST(ReleaseJsonParser, ResetAndReuse) {
  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);

  const char* json1 =
      R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_download_url":"https://a","size":1}]})";
  p.feed(json1, strlen(json1));
  EXPECT_TRUE(p.foundTag());
  EXPECT_STREQ(p.getTagName(), "v1.0");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://a");
  EXPECT_EQ(p.getFirmwareSize(), 1u);

  p.reset();
  // reset() clears the expected-suffix too -- a fresh check must set it again,
  // same as a fresh GitHub API round trip would.
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);

  const char* json2 =
      R"({"tag_name":"v2.0","assets":[{"name":"firmware.bin","browser_download_url":"https://b","size":2}]})";
  p.feed(json2, strlen(json2));
  EXPECT_TRUE(p.foundTag());
  EXPECT_STREQ(p.getTagName(), "v2.0");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://b");
  EXPECT_EQ(p.getFirmwareSize(), 2u);
}

TEST(ReleaseJsonParser, ResetClearsState) {
  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);

  const char* json =
      R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_download_url":"https://a","size":100}]})";
  p.feed(json, strlen(json));
  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());

  p.reset();

  EXPECT_FALSE(p.foundTag());
  EXPECT_FALSE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "");
  EXPECT_STREQ(p.getFirmwareUrl(), "");
  EXPECT_EQ(p.getFirmwareSize(), 0u);

  // reset() also clears the expected suffix -- feeding the same JSON again
  // without re-setting it must not find anything.
  p.feed(json, strlen(json));
  EXPECT_FALSE(p.foundFirmware());
}

TEST(ReleaseJsonParser, NoSuffixSetMatchesNothing) {
  // The default (reset) state has no expected suffix. A board whose
  // FirmwareBoardTag maps to no published asset suffix (e.g. an env with no
  // release yet) must see NO_UPDATE, not an accidental match on any asset.
  const char* json =
      R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_download_url":"https://a","size":1}]})";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_FALSE(p.foundFirmware());
}

TEST(ReleaseJsonParser, SuffixSelectsMatchingDeviceAmongSeveral) {
  // A release carries one asset per device (today's publish convention,
  // docs/public-release.md). A board must pick its own and ignore the rest --
  // this is the actual bug this parser was fixed for (T-2008 investigation).
  const char* json = R"({
      "tag_name": "explorink-v0.2.1-alpha",
      "assets": [
        {"name": "explorink-0.2.1-alpha-esp32c3-bootloader.bin", "browser_download_url": "https://c3-boot", "size": 100},
        {"name": "explorink-0.2.1-alpha-esp32c3-firmware.bin", "browser_download_url": "https://c3-fw", "size": 1000000},
        {"name": "explorink-0.2.1-alpha-x4pro-esp32s3-firmware.bin", "browser_download_url": "https://x4pro-fw", "size": 2000000},
        {"name": "explorink-0.2.1-alpha-t5s3pro-esp32s3-firmware.bin", "browser_download_url": "https://t5s3pro-fw", "size": 3000000}
      ]
    })";

  {
    ReleaseJsonParser p;
    p.setFirmwareAssetSuffix("-x4pro-esp32s3-firmware.bin");
    p.feed(json, strlen(json));
    EXPECT_TRUE(p.foundFirmware());
    EXPECT_STREQ(p.getFirmwareUrl(), "https://x4pro-fw");
    EXPECT_EQ(p.getFirmwareSize(), 2000000u);
  }
  {
    ReleaseJsonParser p;
    p.setFirmwareAssetSuffix("-t5s3pro-esp32s3-firmware.bin");
    p.feed(json, strlen(json));
    EXPECT_TRUE(p.foundFirmware());
    EXPECT_STREQ(p.getFirmwareUrl(), "https://t5s3pro-fw");
    EXPECT_EQ(p.getFirmwareSize(), 3000000u);
  }
  {
    ReleaseJsonParser p;
    p.setFirmwareAssetSuffix("-esp32c3-firmware.bin");
    p.feed(json, strlen(json));
    EXPECT_TRUE(p.foundFirmware());
    EXPECT_STREQ(p.getFirmwareUrl(), "https://c3-fw");
    EXPECT_EQ(p.getFirmwareSize(), 1000000u);
  }
  {
    // A board whose suffix isn't in this release at all (e.g. sticky, not
    // published per T-2003) must see no match, never fall back to someone
    // else's binary.
    ReleaseJsonParser p;
    p.setFirmwareAssetSuffix("-sticky-esp32s3-firmware.bin");
    p.feed(json, strlen(json));
    EXPECT_FALSE(p.foundFirmware());
  }
}

TEST(ReleaseJsonParser, PartialAssetNameMatch) {
  // "firmware.bin.bak"/".old" do not END with "firmware.bin" and must not match.
  const char* json = R"({
      "tag_name": "v1.0",
      "assets": [
        {"name": "firmware.bin.bak", "browser_download_url": "https://bak", "size": 100},
        {"name": "firmware.bin.old", "browser_download_url": "https://old", "size": 200}
      ]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_FALSE(p.foundFirmware());
}

TEST(ReleaseJsonParser, FirmwareBinSuffixMatch) {
  // Case-sensitive; "firmware.bin2" does not end with "firmware.bin".
  const char* json = R"({
      "tag_name": "v1.0",
      "assets": [
        {"name": "FIRMWARE.BIN", "browser_download_url": "https://upper", "size": 100},
        {"name": "firmware.bin", "browser_download_url": "https://exact", "size": 200},
        {"name": "firmware.bin2", "browser_download_url": "https://suffix", "size": 300}
      ]
    })";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getFirmwareUrl(), "https://exact");
  EXPECT_EQ(p.getFirmwareSize(), 200u);
}

TEST(ReleaseJsonParser, LargeSize) {
  // 16MB firmware (maximum flash size)
  const char* json =
      R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_download_url":"https://fw","size":16777216}]})";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_EQ(p.getFirmwareSize(), 16777216u);
}

TEST(ReleaseJsonParser, SizeZero) {
  const char* json =
      R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_download_url":"https://fw","size":0}]})";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundFirmware());
  EXPECT_EQ(p.getFirmwareSize(), 0u);
}

TEST(ReleaseJsonParser, MinimalValidJson) {
  const char* json = R"({"tag_name":"v0","assets":[{"name":"firmware.bin","browser_download_url":"u","size":1}]})";

  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v0");
  EXPECT_STREQ(p.getFirmwareUrl(), "u");
  EXPECT_EQ(p.getFirmwareSize(), 1u);
}

TEST(ReleaseJsonParser, ChunkedRealisticEveryBoundary) {
  // Two-chunk split at every byte boundary on a compact JSON
  const char* json =
      R"({"tag_name":"v2.0","assets":[{"name":"firmware.bin","browser_download_url":"https://example.com/fw","size":9999}]})";
  size_t len = strlen(json);

  for (size_t split = 0; split <= len; ++split) {
    ReleaseJsonParser p;
    p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
    if (split > 0) p.feed(json, split);
    if (split < len) p.feed(json + split, len - split);

    EXPECT_TRUE(p.foundTag()) << "split=" << split;
    EXPECT_TRUE(p.foundFirmware()) << "split=" << split;
    EXPECT_STREQ(p.getTagName(), "v2.0") << "split=" << split;
    EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/fw") << "split=" << split;
    EXPECT_EQ(p.getFirmwareSize(), 9999u) << "split=" << split;
  }
}

// --- List mode: GET /releases instead of /releases/latest (T-2053) ---------
//
// Shape taken from the live API on 2026-11-03: the top level is an ARRAY of
// release objects, newest first, with every board's releases interleaved.
// `/releases/latest` could not be used because it skips prereleases, and every
// ExplorInk release but the first is flagged that way.

namespace {

// Three releases, newest first, exactly as GitHub orders them: x4pro 0.2.2,
// then the C3 0.2.2, then x4pro 0.2.1. A board looking for an x4pro asset must
// land on the FIRST entry; a C3 board must skip past it to the second.
const char* kReleaseList = R"([
  {
    "id": 3,
    "tag_name": "explorink-x4pro-v0.2.2",
    "prerelease": true,
    "assets": [
      {"name": "explorink-0.2.2-x4pro-esp32s3-bootloader.bin", "size": 100,
       "browser_download_url": "https://example.com/x4pro-022-boot"},
      {"name": "explorink-0.2.2-x4pro-esp32s3-firmware.bin", "size": 1500000,
       "browser_download_url": "https://example.com/x4pro-022-fw"}
    ],
    "body": "x4pro notes"
  },
  {
    "id": 2,
    "tag_name": "explorink-v0.2.2",
    "prerelease": true,
    "assets": [
      {"name": "explorink-0.2.2-esp32c3-firmware.bin", "size": 900000,
       "browser_download_url": "https://example.com/c3-022-fw"}
    ],
    "body": "c3 notes"
  },
  {
    "id": 1,
    "tag_name": "explorink-x4pro-v0.2.1-alpha",
    "prerelease": true,
    "assets": [
      {"name": "explorink-0.2.1-alpha-x4pro-esp32s3-firmware.bin", "size": 1400000,
       "browser_download_url": "https://example.com/x4pro-021-fw"}
    ],
    "body": "older x4pro notes"
  }
])";

constexpr const char* kX4ProSuffix = "-x4pro-esp32s3-firmware.bin";
constexpr const char* kC3Suffix = "-esp32c3-firmware.bin";
constexpr const char* kT5S3ProSuffix = "-t5s3pro-esp32s3-firmware.bin";

}  // namespace

TEST(ReleaseJsonParserList, PicksNewestReleaseForThisBoard) {
  ReleaseJsonParser p;
  p.setListMode(true);
  p.setFirmwareAssetSuffix(kX4ProSuffix);
  p.feed(kReleaseList, strlen(kReleaseList));

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "explorink-x4pro-v0.2.2");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/x4pro-022-fw");
  EXPECT_EQ(p.getFirmwareSize(), 1500000u);
}

TEST(ReleaseJsonParserList, SkipsReleasesBelongingToOtherBoards) {
  // The C3 board's release is SECOND in the list. Landing on the first entry's
  // tag with the second entry's asset is the bug this guards: the tag has to
  // be rolled back when a release carries nothing for us.
  ReleaseJsonParser p;
  p.setListMode(true);
  p.setFirmwareAssetSuffix(kC3Suffix);
  p.feed(kReleaseList, strlen(kReleaseList));

  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "explorink-v0.2.2");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/c3-022-fw");
  EXPECT_EQ(p.getFirmwareSize(), 900000u);
}

TEST(ReleaseJsonParserList, OlderReleaseForSameBoardDoesNotOverwrite) {
  // Both the first and the third entry carry an x4pro asset. The newest one
  // wins and the older one must not touch tag, url or size.
  ReleaseJsonParser p;
  p.setListMode(true);
  p.setFirmwareAssetSuffix(kX4ProSuffix);
  p.feed(kReleaseList, strlen(kReleaseList));

  EXPECT_STREQ(p.getTagName(), "explorink-x4pro-v0.2.2");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/x4pro-022-fw");
  EXPECT_EQ(p.getFirmwareSize(), 1500000u);
}

TEST(ReleaseJsonParserList, NoReleaseForThisBoardLeavesNothingBehind) {
  // A board with no published asset anywhere in the list. foundTag() must be
  // false too: a leftover tag from the last release scanned would read as
  // "found a release, it just had no asset", which is a different diagnosis.
  ReleaseJsonParser p;
  p.setListMode(true);
  p.setFirmwareAssetSuffix(kT5S3ProSuffix);
  p.feed(kReleaseList, strlen(kReleaseList));

  EXPECT_FALSE(p.foundFirmware());
  EXPECT_FALSE(p.foundTag());
  EXPECT_STREQ(p.getTagName(), "");
  EXPECT_FALSE(p.isComplete());
}

TEST(ReleaseJsonParserList, IsCompleteGoesTrueAtTheMatchingAsset) {
  // The caller aborts the HTTP transfer the moment this flips, so it must not
  // flip before the url and size of the matching asset are both in hand.
  ReleaseJsonParser p;
  p.setListMode(true);
  p.setFirmwareAssetSuffix(kC3Suffix);

  const size_t len = strlen(kReleaseList);
  size_t consumed = 0;
  for (; consumed < len; ++consumed) {
    p.feed(kReleaseList + consumed, 1);
    if (p.isComplete()) break;
  }

  ASSERT_TRUE(p.isComplete());
  EXPECT_STREQ(p.getTagName(), "explorink-v0.2.2");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/c3-022-fw");
  EXPECT_EQ(p.getFirmwareSize(), 900000u);
  // And it stopped before the end -- the third release was never read.
  EXPECT_LT(consumed, len - 1);
}

TEST(ReleaseJsonParserList, ChunkedAtEveryBoundary) {
  const size_t len = strlen(kReleaseList);
  for (size_t split = 0; split <= len; ++split) {
    ReleaseJsonParser p;
    p.setListMode(true);
    p.setFirmwareAssetSuffix(kC3Suffix);
    if (split > 0) p.feed(kReleaseList, split);
    if (split < len) p.feed(kReleaseList + split, len - split);

    EXPECT_TRUE(p.foundFirmware()) << "split=" << split;
    EXPECT_STREQ(p.getTagName(), "explorink-v0.2.2") << "split=" << split;
    EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/c3-022-fw") << "split=" << split;
  }
}

TEST(ReleaseJsonParserList, TagAfterAssetsInTheMatchingRelease) {
  // GitHub puts tag_name before assets today, but key order is not part of the
  // contract. The release that supplied the firmware must still give up its
  // tag when the key arrives late.
  const char* json = R"([
    {"assets": [{"name": "a-esp32c3-firmware.bin", "size": 7,
                 "browser_download_url": "https://example.com/late"}],
     "tag_name": "explorink-v9.9.9"},
    {"tag_name": "explorink-v0.0.1",
     "assets": [{"name": "b-esp32c3-firmware.bin", "size": 1,
                 "browser_download_url": "https://example.com/old"}]}
  ])";

  ReleaseJsonParser p;
  p.setListMode(true);
  p.setFirmwareAssetSuffix(kC3Suffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "explorink-v9.9.9");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/late");
  EXPECT_EQ(p.getFirmwareSize(), 7u);
}

TEST(ReleaseJsonParserList, EmptyList) {
  ReleaseJsonParser p;
  p.setListMode(true);
  p.setFirmwareAssetSuffix(kC3Suffix);
  const char* json = "[]";
  p.feed(json, strlen(json));

  EXPECT_FALSE(p.foundTag());
  EXPECT_FALSE(p.foundFirmware());
}

TEST(ReleaseJsonParserList, TruncatedMidList) {
  // A dropped connection halfway through the second release. Nothing matched
  // yet, so nothing is reported -- not a half-filled result.
  const char* full = kReleaseList;
  const std::string truncated(full, strlen(full) / 2);

  ReleaseJsonParser p;
  p.setListMode(true);
  p.setFirmwareAssetSuffix(kT5S3ProSuffix);
  p.feed(truncated.c_str(), truncated.size());

  EXPECT_FALSE(p.foundFirmware());
  EXPECT_FALSE(p.isComplete());
}

TEST(ReleaseJsonParserList, NestedObjectsInsideListedReleases) {
  // `author` and `uploader` sub-objects sit at the depths the list shifts by
  // one; their own "name" keys must not be mistaken for an asset name.
  const char* json = R"([
    {"author": {"login": "bot", "name": "a-esp32c3-firmware.bin"},
     "tag_name": "explorink-v1.0.0",
     "assets": [
       {"name": "notes.txt", "size": 1, "browser_download_url": "https://example.com/txt",
        "uploader": {"login": "bot", "name": "fake-esp32c3-firmware.bin"}},
       {"name": "real-esp32c3-firmware.bin", "size": 42,
        "browser_download_url": "https://example.com/real"}
     ]}
  ])";

  ReleaseJsonParser p;
  p.setListMode(true);
  p.setFirmwareAssetSuffix(kC3Suffix);
  p.feed(json, strlen(json));

  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "explorink-v1.0.0");
  EXPECT_STREQ(p.getFirmwareUrl(), "https://example.com/real");
  EXPECT_EQ(p.getFirmwareSize(), 42u);
}

TEST(ReleaseJsonParserList, ListModeOffStillParsesASingleRelease) {
  // The single-object path must be untouched by the new mode.
  ReleaseJsonParser p;
  p.setFirmwareAssetSuffix(kAnyFirmwareSuffix);
  p.feed(kRealisticMinified, strlen(kRealisticMinified));

  EXPECT_TRUE(p.foundTag());
  EXPECT_TRUE(p.foundFirmware());
  EXPECT_STREQ(p.getTagName(), "v2.4.1");
  EXPECT_TRUE(p.isComplete());
}
