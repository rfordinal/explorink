#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <string>

#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"
#include "OtaVersionCompare.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/rfordinal/explorink/releases/latest";

// Suffix of the published asset name this running build should download --
// see docs/public-release.md for the naming convention, and
// FirmwareBoardTag.cpp for where the board name itself comes from. Returns
// nullptr for a board with no published asset (sticky, T-2003), so OTA
// correctly reports NO_UPDATE instead of matching someone else's binary.
//
// NOTE: `/releases/latest` is the single most-recently-published release
// across the WHOLE repo, not the most recent release FOR THIS BOARD -- each
// board ships on its own tag (docs/public-release.md), so this can still miss
// a real update if a different board's release happened to land more
// recently. Tracked as a follow-up, not fixed here: this suffix match is what
// makes OTA pick the right ASSET once it has a release JSON in hand, not what
// decides WHICH release to fetch.
const char* expectedAssetSuffix() {
  const char* name = board_tag::boardName();
  const size_t len = board_tag::boardNameLen();
  auto is = [&](const char* s) { return len == strlen(s) && memcmp(name, s, len) == 0; };
  if (is("x3x4")) return "-esp32c3-firmware.bin";
  if (is("x4pro")) return "-x4pro-esp32s3-firmware.bin";
  if (is("t5s3pro")) return "-t5s3pro-esp32s3-firmware.bin";
  return nullptr;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", TRAILINK_VERSION);

  const char* assetSuffix = expectedAssetSuffix();
  if (!assetSuffix) {
    LOG_INF("OTA", "No published asset for board '%.*s'", static_cast<int>(board_tag::boardNameLen()),
            board_tag::boardName());
    return NO_UPDATE;
  }

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  releaseParser.setFirmwareAssetSuffix(assetSuffix);
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_INF("OTA", "No asset ending '%s' in latest release (tag=%s)", assetSuffix, releaseParser.getTagName());
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) return false;
  return ota_version::isVersionNewer(TRAILINK_VERSION, latestVersion.c_str());
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  // The image streams in chunks; only the first bytes carry the header.
  // Buffer them so we can read chip_id (esp_image_header_t offset 12) and
  // reject a wrong-MCU image before any of it reaches the OTA partition.
  uint8_t hdr[14];
  size_t hdrLen = 0;
  bool wrongDevice = false;
  // All our S3 boards (x4pro, t5s3pro, sticky) share one chip_id, so also
  // scan the stream for the embedded board tag (FirmwareBoardTag.h) -- same
  // defense-in-depth as FirmwareFlasher::validateImageFile() applies to the
  // SD-card path (T-2008). An untagged image passes; a tag naming a
  // different board aborts the download. Bytes may partially land in the
  // inactive OTA slot before the abort, but esp_ota_abort() below means it
  // never becomes the boot target.
  board_tag::Scanner tagScanner;
  const bool fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    if (hdrLen < sizeof(hdr)) {
      const size_t take = std::min(len, sizeof(hdr) - hdrLen);
      std::memcpy(hdr + hdrLen, data, take);
      hdrLen += take;
      if (hdrLen == sizeof(hdr)) {
        uint16_t imageChip;
        std::memcpy(&imageChip, hdr + 12, sizeof(imageChip));
        const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
        if (deviceChip != 0xFFFF && imageChip != deviceChip) {
          LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
          wrongDevice = true;
          return false;  // abort the transfer
        }
      }
    }
    tagScanner.feed(data, len);
    if (tagScanner.mismatch()) {
      LOG_ERR("OTA", "wrong board: image=%s device=%.*s", tagScanner.foundName(),
              static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
      wrongDevice = true;
      return false;  // abort the transfer
    }
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (wrongDevice) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
