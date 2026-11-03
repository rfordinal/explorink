#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

class ReleaseJsonParser {
 public:
  ReleaseJsonParser();

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  // Asset names in the "assets" array are matched by SUFFIX against this
  // (e.g. "-x4pro-esp32s3-firmware.bin"), not by exact equality -- the
  // version-number prefix in a published filename doesn't necessarily match
  // the release's own tag_name string (see OtaUpdater::expectedAssetSuffix()),
  // so matching on the device-and-chip-specific tail is what's reliable. Call
  // before feed(); an empty/null suffix means no asset ever matches.
  void setFirmwareAssetSuffix(const char* suffix);

  // Parse a RELEASE LIST (`GET /repos/<o>/<r>/releases`) instead of a single
  // release object (`GET .../releases/latest`). The two differ only by one
  // array level, so the same state machine covers both: every depth test
  // shifts by one and a release object that carried no matching asset is
  // rolled back at its closing brace, leaving the NEWEST release that does
  // carry one. GitHub returns the list newest-first, so first match wins.
  //
  // Why this mode exists at all: `/releases/latest` skips every release
  // flagged "prerelease", and every ExplorInk release but the first is
  // flagged that way -- see OtaUpdater::checkForUpdate() and T-2053.
  // Call before feed(), like setFirmwareAssetSuffix().
  void setListMode(bool enabled);

  // True once a tag AND a matching firmware asset have both been seen. In
  // list mode the caller can stop the download here: GitHub orders a release
  // object's keys with `tag_name` and `assets` before the long `body`, so
  // everything needed is already in hand and the rest of the response -- the
  // release notes, plus every older release -- is dead weight.
  bool isComplete() const;

  bool foundTag() const;
  bool foundFirmware() const;
  const char* getTagName() const;
  const char* getFirmwareUrl() const;
  size_t getFirmwareSize() const;

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    ASSETS,
    ASSET_NAME,
    ASSET_URL,
    ASSET_SIZE,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitAsset();

  StreamingJsonParser parser;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;

  // 48, not 32: the longest real tag today ("explorink-t5s3pro-v0.2.1-alpha",
  // 30 chars) leaves only 1 byte of headroom in 32 -- the same silent-
  // truncation risk currentAssetName was just bumped from 32 to 96 for.
  char tagName[48];
  char expectedAssetSuffix[48];
  char firmwareUrl[512];
  size_t firmwareSize;
  bool tagFound;
  bool firmwareFound;

  // 32 bytes was enough for the old literal "firmware.bin" exact match, but
  // this project's real published assets are much longer
  // ("explorink-0.2.1-alpha-x4pro-esp32s3-firmware.bin", docs/public-release.md)
  // and get matched by SUFFIX -- a silently truncated name (safeCopy keeps the
  // front, drops the tail) can never end with the right suffix again. Found by
  // a host test using a realistic filename, not by inspection.
  char currentAssetName[96];
  char currentAssetUrl[512];
  size_t currentAssetSize;

  bool listMode;

  // Set at the closing brace of the release that supplied the firmware. From
  // then on no further tag_name is taken -- see sOnString().
  bool releaseLocked;

  // Depth at which a release object's own keys sit: 1 for a bare release
  // object, 2 once the top level is the list's array.
  uint8_t releaseDepth() const { return listMode ? 2 : 1; }
};
