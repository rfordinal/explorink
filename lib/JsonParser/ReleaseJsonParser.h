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

  char tagName[32];
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
};
