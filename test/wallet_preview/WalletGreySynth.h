#pragma once

// A synthetic grey wallet page, in both grey forms, built out of the DEVICE's own
// encoding table.
//
// Why this exists, and why it is host-only. The grey asset format was specified
// before either side wrote it: the generator bakes `PAGE_IMAGE_GREY` (2bpp) and
// `GREY_PLANES` (base + LSB + MSB), and the firmware streams the planes straight to
// the controller without ever looking at a pixel. A firmware that never looks
// cannot notice a wrong bit order -- the panel just shows a wrong picture, and only
// after a flash.
//
// So the layout is built here from `greyPlaneBit()` / `greyValueFromPlaneBits()`
// (`src/activities/wallet/WalletAsset.h`), which are themselves written in terms of
// `GrayShade.h`'s constexpr encoding -- the same table `GrayscaleFrame` draws its
// plane passes with. One encoding, three consumers: the firmware's own grey
// renderer, this synth, and the tests that read it back.
//
// Two users:
//
//   * `test/wallet/WalletTest.cpp` -- round-trips the two forms against each other
//     and against the gates, in memory, with no files involved;
//   * `wallet_preview --synth-grey DIR` -- writes a tree so `--grey` has something
//     to render into a four-level PNG before the real generator's output exists.
//
// It is NOT a second generator. It writes no RLE sidecar, no codes, one page, one
// level, and its picture is a test pattern rather than a document.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "WalletAsset.h"
#include "WalletSha256.h"

namespace wallet {
namespace host {

struct GreyPage {
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t planeRowBytes = 0;  // one row of one 1bpp plane
  uint32_t greyRowBytes = 0;   // one 2bpp row
  // The picture itself: one level per pixel, 0 = black .. 3 = white, in NATIVE
  // (panel) coordinates -- the same order both payloads are packed in.
  std::vector<uint8_t> levels;
  std::vector<uint8_t> planes;  // assetType 7 payload: base || lsb || msb
  std::vector<uint8_t> twoBpp;  // assetType 6 payload
};

// --- packing and unpacking, MSB-first in both forms -------------------------

inline void setPlaneBit(std::vector<uint8_t>& plane, const uint32_t rowBytes, const uint32_t x, const uint32_t y,
                        const bool bit) {
  const size_t index = static_cast<size_t>(y) * rowBytes + x / 8u;
  const uint8_t mask = static_cast<uint8_t>(1u << (7u - (x % 8u)));
  if (bit) {
    plane[index] = static_cast<uint8_t>(plane[index] | mask);
  } else {
    plane[index] = static_cast<uint8_t>(plane[index] & ~mask);
  }
}

inline bool planeBitAt(const uint8_t* row, const uint32_t x) { return ((row[x / 8u] >> (7u - (x % 8u))) & 1u) != 0u; }

// 4 pixels to a byte, pixel 0 in the TOP two bits.
inline void setTwoBppValue(std::vector<uint8_t>& image, const uint32_t rowBytes, const uint32_t x, const uint32_t y,
                           const uint8_t value) {
  const size_t index = static_cast<size_t>(y) * rowBytes + x / 4u;
  const uint8_t shift = static_cast<uint8_t>(6u - 2u * (x % 4u));
  image[index] = static_cast<uint8_t>((image[index] & ~(0x3u << shift)) | ((value & 0x3u) << shift));
}

inline uint8_t twoBppValueAt(const uint8_t* row, const uint32_t x) {
  const uint8_t shift = static_cast<uint8_t>(6u - 2u * (x % 4u));
  return static_cast<uint8_t>((row[x / 4u] >> shift) & 0x3u);
}

// The level three plane rows encode at x. The inverse of the bake below, and the
// one thing the preview needs in order to draw four levels.
inline uint8_t levelFromPlaneRows(const uint8_t* baseRow, const uint8_t* lsbRow, const uint8_t* msbRow,
                                  const uint32_t x) {
  return greyValueFromPlaneBits(planeBitAt(baseRow, x), planeBitAt(lsbRow, x), planeBitAt(msbRow, x));
}

// --- the picture -----------------------------------------------------------

// A 4x4 grid of the four levels, arranged so every level sits next to every other
// (level = (row + col) % 4), a 2 px black frame around the whole page, and a band of
// 1/2/3 px lines on white inside each cell.
//
// What each part is for:
//   * the grid answers "are four levels distinguishable, and next to which";
//   * the frame catches a stride error -- a wrong row stride skews it into a
//     diagonal instead of leaving it square;
//   * the thin lines catch it too, and are the stroke-width question the map's own
//     grey bench asks (docs/eink-grayscale.md, "Grey scale" page);
//   * the solid black block in the first cell is an orientation marker: it must
//     land in one corner, not in the middle.
inline GreyPage makeGreyPage(const uint16_t width, const uint16_t height) {
  GreyPage page;
  page.width = width;
  page.height = height;
  page.planeRowBytes = greyPlaneRowBytes(width);
  page.greyRowBytes = greyRowBytes2bpp(width);
  page.levels.assign(static_cast<size_t>(width) * height, kGreyValueWhite);

  const uint32_t cellW = width / 4u;
  const uint32_t cellH = height / 4u;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t rowCell = cellH > 0 ? (y / cellH) : 0;
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t colCell = cellW > 0 ? (x / cellW) : 0;
      uint8_t value = static_cast<uint8_t>((rowCell + colCell) % 4u);
      // The bottom third of every cell is white with thin lines of the cell's own
      // level drawn into it.
      const uint32_t inCellY = cellH > 0 ? (y % cellH) : y;
      if (cellH > 0 && inCellY > (cellH * 2u) / 3u) {
        value = kGreyValueWhite;
        const uint32_t lineY = inCellY - (cellH * 2u) / 3u;
        // 1 px at 4, 2 px at 10-11, 3 px at 18-20.
        const bool onLine = lineY == 4u || (lineY >= 10u && lineY <= 11u) || (lineY >= 18u && lineY <= 20u);
        if (onLine) value = static_cast<uint8_t>((rowCell + colCell) % 4u);
      }
      page.levels[static_cast<size_t>(y) * width + x] = value;
    }
  }
  // Orientation marker: one solid black block in the first cell.
  for (uint32_t y = 8; y < 8u + 48u && y < height; ++y) {
    for (uint32_t x = 8; x < 8u + 48u && x < width; ++x)
      page.levels[static_cast<size_t>(y) * width + x] = kGreyValueBlack;
  }
  // 2 px black frame.
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      if (y < 2 || y + 2 >= height || x < 2 || x + 2 >= width) {
        page.levels[static_cast<size_t>(y) * width + x] = kGreyValueBlack;
      }
    }
  }

  // --- bake. Both payloads come out of the same `levels`, through the device's own
  // encoding table, so a disagreement between them is impossible by construction --
  // which is exactly what makes a disagreement *found by the tests* proof that one
  // of the two readers is wrong rather than one of the two writers.
  const size_t planeBytes = static_cast<size_t>(page.planeRowBytes) * height;
  page.planes.assign(planeBytes * kGreyPlaneCount, 0);
  page.twoBpp.assign(static_cast<size_t>(page.greyRowBytes) * height, 0);

  std::vector<uint8_t> base(planeBytes, 0);
  std::vector<uint8_t> lsb(planeBytes, 0);
  std::vector<uint8_t> msb(planeBytes, 0);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t value = page.levels[static_cast<size_t>(y) * width + x];
      const GrayShade shade = greyShadeFromValue(value);
      setPlaneBit(base, page.planeRowBytes, x, y, greyPlaneBit(shade, GreyPlane::Base));
      setPlaneBit(lsb, page.planeRowBytes, x, y, greyPlaneBit(shade, GreyPlane::Lsb));
      setPlaneBit(msb, page.planeRowBytes, x, y, greyPlaneBit(shade, GreyPlane::Msb));
      setTwoBppValue(page.twoBpp, page.greyRowBytes, x, y, value);
    }
  }
  std::memcpy(page.planes.data(), base.data(), planeBytes);
  std::memcpy(page.planes.data() + planeBytes, lsb.data(), planeBytes);
  std::memcpy(page.planes.data() + planeBytes * 2, msb.data(), planeBytes);
  return page;
}

// The 1bpp page image of the same picture, for the A/B: every grey collapses the way
// a 2x2 dither would not -- light grey to white, dark grey to black. Crude on
// purpose. The real generator dithers; this only has to be a legitimate 1bpp asset
// of the same geometry so the toggle has something to toggle to.
inline std::vector<uint8_t> makeOneBppPage(const GreyPage& page) {
  const size_t planeBytes = static_cast<size_t>(page.planeRowBytes) * page.height;
  std::vector<uint8_t> out(planeBytes, 0);
  for (uint32_t y = 0; y < page.height; ++y) {
    for (uint32_t x = 0; x < page.width; ++x) {
      const uint8_t value = page.levels[static_cast<size_t>(y) * page.width + x];
      const bool white = value >= kGreyValueLightGray;
      setPlaneBit(out, page.planeRowBytes, x, y, white);
    }
  }
  return out;
}

// --- asset files -----------------------------------------------------------

inline void putLe16(std::vector<uint8_t>& out, const size_t at, const uint16_t v) {
  out[at] = static_cast<uint8_t>(v & 0xFF);
  out[at + 1] = static_cast<uint8_t>(v >> 8);
}

inline void putLe32(std::vector<uint8_t>& out, const size_t at, const uint32_t v) {
  out[at] = static_cast<uint8_t>(v & 0xFF);
  out[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  out[at + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  out[at + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// The 32-byte header the format doc specifies, at the absolute offsets it
// specifies (parent repo docs/wallet-format.md, section 5), plus the payload.
// `sha256Prefix` always covers the PLAINTEXT, so it is computed before any
// encryption the caller applies afterwards.
inline std::vector<uint8_t> buildAssetFile(const AssetType type, const uint8_t bitDepth, const uint16_t width,
                                           const uint16_t height, const std::vector<uint8_t>& payload,
                                           const uint32_t version = 1, const bool encryptedFlag = false) {
  std::vector<uint8_t> file(kAssetHeaderBytes, 0);
  file[0] = 'E';
  file[1] = 'W';
  file[2] = 'A';
  file[3] = '1';
  file[4] = static_cast<uint8_t>(type);
  file[5] = bitDepth;
  file[6] = 0;
  file[7] = 0;
  putLe16(file, 8, width);
  putLe16(file, 10, height);
  putLe32(file, 12, static_cast<uint32_t>(payload.size()));
  putLe32(file, 16, version);
  file[20] = encryptedFlag ? kFlagEncrypted : 0;
  file[21] = 1;  // presentation: upright with the device held portrait
  Sha256 sha;
  sha.update(payload.data(), payload.size());
  uint8_t digest[kSha256Bytes];
  sha.finish(digest);
  std::memcpy(file.data() + 24, digest, kSha256PrefixBytes);
  file.insert(file.end(), payload.begin(), payload.end());
  return file;
}

// One page, one level, three assets: the 1bpp page image and both grey forms. The
// manifest keys are the contract the device parses (`ManifestParser`, Ctx::PageImage)
// and the ones the generator has to write.
inline std::string greyManifestJson(const GreyPage& page, const char* oneBppId, const char* greyPlanesId,
                                    const char* greyImageId, const uint16_t panelWidth, const uint16_t panelHeight,
                                    const uint16_t panelRowBytes) {
  const uint32_t planeBytes = static_cast<uint32_t>(page.planeRowBytes) * page.height;
  char buf[2048];
  std::snprintf(
      buf, sizeof(buf),
      "{\n"
      "  \"formatVersion\": 1,\n"
      "  \"walletVersion\": 1,\n"
      "  \"panel\": {\"name\": \"synth\", \"width\": %u, \"height\": %u, \"rowBytes\": %u, "
      "\"assetBytes\": %u},\n"
      "  \"items\": [{\n"
      "    \"id\": \"1111111111111111\", \"title\": \"Grey test pattern\",\n"
      "    \"pages\": [{\n"
      "      \"id\": \"p001\",\n"
      "      \"levels\": {\n"
      "        \"fit\": {\n"
      "          \"cols\": 1, \"rows\": 1, \"defaultTileX\": 0, \"defaultTileY\": 0,\n"
      "          \"pageImage\": {\"assetId\": \"%s\", \"nativeWidth\": %u, \"nativeHeight\": %u,"
      " \"rowBytes\": %u, \"rawLen\": %u, \"windowStepX\": 240, \"windowStepY\": 160,"
      " \"focalX\": 0, \"focalY\": 0},\n"
      "          \"greyPlanes\": {\"assetId\": \"%s\", \"nativeWidth\": %u, \"nativeHeight\": %u,"
      " \"rowBytes\": %u, \"rawLen\": %u, \"windowStepX\": 240, \"windowStepY\": 160,"
      " \"focalX\": 0, \"focalY\": 0},\n"
      "          \"greyPageImage\": {\"assetId\": \"%s\", \"nativeWidth\": %u, \"nativeHeight\": %u,"
      " \"rowBytes\": %u, \"rawLen\": %u, \"windowStepX\": 240, \"windowStepY\": 160,"
      " \"focalX\": 0, \"focalY\": 0}\n"
      "        }\n"
      "      }\n"
      "    }]\n"
      "  }]\n"
      "}\n",
      static_cast<unsigned>(panelWidth), static_cast<unsigned>(panelHeight), static_cast<unsigned>(panelRowBytes),
      static_cast<unsigned>(static_cast<uint32_t>(panelRowBytes) * panelHeight), oneBppId,
      static_cast<unsigned>(page.width), static_cast<unsigned>(page.height), static_cast<unsigned>(page.planeRowBytes),
      static_cast<unsigned>(planeBytes), greyPlanesId, static_cast<unsigned>(page.width),
      static_cast<unsigned>(page.height), static_cast<unsigned>(page.planeRowBytes),
      static_cast<unsigned>(planeBytes * kGreyPlaneCount), greyImageId, static_cast<unsigned>(page.width),
      static_cast<unsigned>(page.height), static_cast<unsigned>(page.greyRowBytes),
      static_cast<unsigned>(static_cast<uint32_t>(page.greyRowBytes) * page.height));
  return std::string(buf);
}

}  // namespace host
}  // namespace wallet
