// Host-side wallet preview: read a generated wallet tree through the device's own
// reader code and write PNGs of the resulting framebuffer.
//
// Why this exists. The asset format was implemented twice from one written
// contract -- once in the generator (tools/walletgen.py, parent repo) and once
// here -- and the two had never met. A unit test on hand-authored bytes cannot
// catch that: it agrees with whichever side wrote it. This tool reads bytes a
// real generator produced, through parseAssetHeader() / checkAssetForPanel() /
// buildAssetPathIn() / ManifestParser -- the same functions the firmware calls --
// and expands the result to an image *exactly* as the panel does. A wrong byte
// order or a flipped polarity comes out as a visibly wrong picture.
//
// Not a gtest suite: a plain executable, same arrangement as test/map_preview
// (see scripts/register_wallet_preview_target.py). Run it with
// `pio run -t wallet-preview`, or directly:
//
//   wallet_preview --tree DIR [--panel x4|x3] [--item 0 --page 0
//                  --level fit|detail|one_to_one --col 0 --row 0]
//                  [--asset <16 hex>] [--win-x N --win-y N] [--code N]
//                  [--key <64 hex>] --out PREFIX
//
// `--key` renders an ENCRYPTED tree (P3): it reads `manifest.enc` instead of
// `manifest.json`, verifies the GCM tag before parsing a byte of it, and decrypts
// each asset with AES-256-CTR. Windows are decrypted row by row at each row's own
// payload offset -- the same arithmetic PageReader::readWindow() runs on the device,
// so a wrong offset comes out as a visibly wrong picture here rather than as noise
// on the panel.
//
// `--code N` renders the item's Nth machine-readable code instead of a document
// level (P2). It runs the device's own code path: ManifestParser's code lookup,
// checkCodeAsset(), checkPayloadHash() against the manifest's sha256, and the
// blank-band test the symbology label uses to find space. It also measures the
// drawn code's bounding box off the pixels and compares it with the manifest's
// codeWidthPx / codeHeightPx -- so "is it centred, is the quiet zone intact, is it
// as large as the panel allows" comes out as numbers next to the picture.
//
// When the level carries a design-B `pageImage`, the window at (--win-x, --win-y)
// is blitted out of it row by row -- the same row maths PageReader uses on the
// device, so an off-by-one stride or a mis-clamped origin shows up as a visibly
// wrong picture. Origins default to the manifest's focal point and are clamped
// exactly as the device clamps them.
//
// `--grey` renders the level's GREY asset instead of its 1bpp one (P2b): four
// levels, out of the baked `greyPlanes` asset the device streams to the panel, or
// out of the 2bpp `greyPageImage` where the level carries only that. When it carries
// both, the two are decoded independently and compared pixel by pixel -- which is
// the check that matters, because the device never looks at a grey pixel and so
// cannot notice a wrong bit order itself (../../docs/wallet-grey.md).
//
// `--synth-grey DIR` writes a synthetic grey tree (one page, one level, a test
// pattern) and exits, so `--grey` has something to render before the generator's own
// grey output exists. The layout it writes comes from the firmware's encoding table,
// not from a copy of it (test/wallet_preview/WalletGreySynth.h).
//
// Writes PREFIX.png (the panel's own 800x480 landscape frame) and
// PREFIX-portrait.png (the same bits read the way a rider holds the device).

#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "WalletAsset.h"
#include "WalletCrypto.h"
#include "WalletCryptoHost.h"
#include "WalletGreySynth.h"
#include "WalletManifestParser.h"
#include "WalletSidecar.h"

namespace {

// --- PNG out (8-bit greyscale, filter 0). zlib is already a test dependency. --

void be32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  be32(out, static_cast<uint32_t>(data.size()));
  const size_t crcStart = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  const uLong crc = crc32(crc32(0L, Z_NULL, 0), out.data() + crcStart, static_cast<uInt>(out.size() - crcStart));
  be32(out, static_cast<uint32_t>(crc));
}

bool writeGreyPng(const std::string& path, const std::vector<uint8_t>& grey, int width, int height) {
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // filter: none
    raw.insert(raw.end(), grey.begin() + static_cast<size_t>(y) * width,
               grey.begin() + static_cast<size_t>(y + 1) * width);
  }
  uLongf compLen = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> comp(compLen);
  if (compress2(comp.data(), &compLen, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK) return false;
  comp.resize(compLen);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  be32(ihdr, static_cast<uint32_t>(width));
  be32(ihdr, static_cast<uint32_t>(height));
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(0);  // colour type: greyscale
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", comp);
  chunk(png, "IEND", {});

  FILE* fh = std::fopen(path.c_str(), "wb");
  if (fh == nullptr) return false;
  const bool ok = std::fwrite(png.data(), 1, png.size(), fh) == png.size();
  std::fclose(fh);
  return ok;
}

// --- framebuffer -> pixels, the way the panel reads it ----------------------

// Row-major, `rowBytes` per physical row, MSB first inside a byte, bit 1 =
// white. Read straight off the renderer's own pixel write
// (lib/GfxRenderer/GfxRenderer.cpp:517-524): it clears the bit to ink a pixel
// and sets it to leave it white.
inline bool isWhite(const std::vector<uint8_t>& fb, int rowBytes, int phyX, int phyY) {
  const size_t index = static_cast<size_t>(phyY) * static_cast<size_t>(rowBytes) + static_cast<size_t>(phyX / 8);
  const uint8_t bit = static_cast<uint8_t>(7 - (phyX % 8));
  return ((fb[index] >> bit) & 1u) != 0;
}

std::vector<uint8_t> panelImage(const std::vector<uint8_t>& fb, const wallet::PanelGeometry& panel) {
  std::vector<uint8_t> grey(static_cast<size_t>(panel.width) * panel.height, 0);
  for (int y = 0; y < panel.height; ++y) {
    for (int x = 0; x < panel.width; ++x) {
      grey[static_cast<size_t>(y) * panel.width + x] = isWhite(fb, panel.rowBytes, x, y) ? 255 : 0;
    }
  }
  return grey;
}

// The portrait view: logical (x,y) -> physical (y, panelHeight - 1 - x), which is
// exactly what GfxRenderer::rotateCoordinates() does for Portrait
// (GfxRenderer.cpp:216-223). If the generator's own rotation is not the inverse
// of this, the portrait PNG comes out sideways or mirrored.
std::vector<uint8_t> portraitImage(const std::vector<uint8_t>& fb, const wallet::PanelGeometry& panel, int& outW,
                                   int& outH) {
  outW = panel.height;  // 480
  outH = panel.width;   // 800
  std::vector<uint8_t> grey(static_cast<size_t>(outW) * outH, 0);
  for (int y = 0; y < outH; ++y) {
    for (int x = 0; x < outW; ++x) {
      const int phyX = y;
      const int phyY = panel.height - 1 - x;
      grey[static_cast<size_t>(y) * outW + x] = isWhite(fb, panel.rowBytes, phyX, phyY) ? 255 : 0;
    }
  }
  return grey;
}

// --- four levels, as pixels -------------------------------------------------

// 0 = black, 3 = white, evenly spaced. Not what the panel does with them -- the
// panel's two greys are wherever the waveform puts them, which is the thing only a
// photograph can answer -- but even spacing is what makes a wrong level obvious.
inline uint8_t greyByte(const uint8_t level) { return static_cast<uint8_t>(level * 85); }

std::vector<uint8_t> levelPanelImage(const std::vector<uint8_t>& levels, const wallet::PanelGeometry& panel) {
  std::vector<uint8_t> grey(static_cast<size_t>(panel.width) * panel.height, 0);
  for (size_t i = 0; i < grey.size() && i < levels.size(); ++i) grey[i] = greyByte(levels[i]);
  return grey;
}

std::vector<uint8_t> levelPortraitImage(const std::vector<uint8_t>& levels, const wallet::PanelGeometry& panel,
                                        int& outW, int& outH) {
  outW = panel.height;
  outH = panel.width;
  std::vector<uint8_t> grey(static_cast<size_t>(outW) * outH, 0);
  for (int y = 0; y < outH; ++y) {
    for (int x = 0; x < outW; ++x) {
      const int phyX = y;
      const int phyY = panel.height - 1 - x;
      grey[static_cast<size_t>(y) * outW + x] = greyByte(levels[static_cast<size_t>(phyY) * panel.width + phyX]);
    }
  }
  return grey;
}

// --- tree reads -------------------------------------------------------------

bool readWholeFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* fh = std::fopen(path.c_str(), "rb");
  if (fh == nullptr) return false;
  std::fseek(fh, 0, SEEK_END);
  const long size = std::ftell(fh);
  std::fseek(fh, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(fh);
    return false;
  }
  out.resize(static_cast<size_t>(size));
  const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), fh) == out.size();
  std::fclose(fh);
  return ok;
}

// Feeds the manifest to the real ManifestParser in 256-byte bites, the same
// bite WalletStore uses on the card.
//
// With a key, `manifest.enc` wins and the tag is verified over the whole file
// before one byte reaches the parser -- the same order the device enforces, and the
// reason there is no streaming path for an encrypted manifest.
bool feedManifest(const std::string& tree, wallet::ManifestParser& parser, const uint8_t* key) {
  if (key != nullptr) {
    std::vector<uint8_t> blob;
    const std::string encPath = tree + "/manifest.enc";
    if (!readWholeFile(encPath, blob)) {
      std::fprintf(stderr, "no encrypted manifest at %s\n", encPath.c_str());
      return false;
    }
    wallet::ManifestEnvelope envelope;
    if (!wallet::parseManifestEnvelope(blob.data(), blob.size(), envelope)) {
      std::fprintf(stderr, "%s is not an EWM1 envelope, or its lengths disagree\n", encPath.c_str());
      return false;
    }
    std::vector<uint8_t> plain(envelope.ciphertextLen);
    if (!wallet::host::gcmDecrypt(key, envelope.nonce, wallet::kGcmNonceLen, blob.data() + envelope.tagOffset,
                                  wallet::kGcmTagLen, blob.data() + envelope.ciphertextOffset, envelope.ciphertextLen,
                                  plain.data())) {
      std::fprintf(stderr, "manifest does not authenticate: wrong key, or the file was altered\n");
      return false;
    }
    std::printf("manifest.enc: %zu bytes, tag VERIFIED, %u bytes of JSON\n", blob.size(), envelope.plaintextLen);
    for (size_t at = 0; at < plain.size(); at += 256) {
      const size_t take = std::min<size_t>(256, plain.size() - at);
      parser.feed(reinterpret_cast<const char*>(plain.data() + at), take);
    }
    if (parser.hasError()) {
      std::fprintf(stderr, "decrypted manifest did not parse\n");
      return false;
    }
    return true;
  }

  const std::string path = tree + "/manifest.json";
  FILE* fh = std::fopen(path.c_str(), "rb");
  if (fh == nullptr) {
    std::fprintf(stderr, "no manifest at %s\n", path.c_str());
    return false;
  }
  char buf[256];
  for (;;) {
    const size_t got = std::fread(buf, 1, sizeof(buf), fh);
    if (got == 0) break;
    parser.feed(buf, got);
  }
  std::fclose(fh);
  if (parser.hasError()) {
    std::fprintf(stderr, "manifest did not parse\n");
    return false;
  }
  return true;
}

// --- grey: four levels, out of whichever grey form the level carries ---------

// One window of a grey asset, decoded to one level per pixel in panel order.
//
// Row by row at each row's own payload offset, which is the arithmetic
// PageReader/GreyPageReader run on the device -- and for an encrypted tree it is
// also where the CTR offset has to be right, so an off-by-32 comes out as a visibly
// wrong picture here instead of as noise on the panel.
bool decodeGreyWindow(const std::vector<uint8_t>& file, const wallet::PageImageSpec& spec,
                      const wallet::PanelGeometry& panel, bool fromPlanes, uint32_t x, uint32_t y, bool encrypted,
                      const uint8_t* key, const uint8_t* iv, std::vector<uint8_t>& levels) {
  levels.assign(static_cast<size_t>(panel.width) * panel.height, 0);
  const uint32_t planeBytes = wallet::greyPlaneBytes(spec);
  // A 2bpp row needs twice the bytes of a plane row for the same pixels.
  const uint32_t takeBytes = fromPlanes ? panel.rowBytes : static_cast<uint32_t>(panel.rowBytes) * 2u;
  const uint32_t xByte = fromPlanes ? x / 8u : x / 4u;
  std::vector<std::vector<uint8_t>> rows(fromPlanes ? wallet::kGreyPlaneCount : 1);

  for (uint32_t r = 0; r < panel.height; ++r) {
    for (size_t plane = 0; plane < rows.size(); ++plane) {
      const uint32_t payloadOffset =
          static_cast<uint32_t>(plane) * (fromPlanes ? planeBytes : 0u) + (y + r) * spec.rowBytes + xByte;
      const size_t off = wallet::kAssetHeaderBytes + payloadOffset;
      if (off + takeBytes > file.size()) {
        std::fprintf(stderr, "grey row %u plane %zu runs past the file\n", r, plane);
        return false;
      }
      rows[plane].assign(file.begin() + static_cast<long>(off), file.begin() + static_cast<long>(off + takeBytes));
      if (encrypted) wallet::host::ctrXor(key, iv, payloadOffset, rows[plane].data(), rows[plane].size());
    }
    for (uint32_t px = 0; px < panel.width; ++px) {
      const uint8_t level = fromPlanes
                                ? wallet::host::levelFromPlaneRows(rows[0].data(), rows[1].data(), rows[2].data(), px)
                                : wallet::host::twoBppValueAt(rows[0].data(), px);
      levels[static_cast<size_t>(r) * panel.width + px] = level;
    }
  }
  return true;
}

// Reads a grey asset, gates it the way the device gates it, and hands back the
// header and the file.
bool loadGreyAsset(const std::string& tree, const wallet::PageImageSpec& spec, const wallet::PanelGeometry& panel,
                   bool fromPlanes, bool haveKey, std::vector<uint8_t>& file, wallet::AssetHeader& header) {
  char path[512];
  if (!wallet::buildAssetPathIn(tree.c_str(), spec.assetId, path, sizeof(path))) {
    std::fprintf(stderr, "grey assetId %s is not 16 hex characters\n", spec.assetId);
    return false;
  }
  if (!readWholeFile(path, file)) {
    std::fprintf(stderr, "cannot read %s\n", path);
    return false;
  }
  const wallet::AssetCheck check = fromPlanes
                                       ? wallet::checkGreyPlanes(file.data(), file.size(), spec, panel, header, haveKey)
                                       : wallet::checkGreyImage(file.data(), file.size(), spec, panel, header, haveKey);
  std::printf("%s : %s -- type %u, bitDepth %u, %ux%u, rawLen %u, flags %u, %s\n",
              fromPlanes ? "greyPlanes " : "greyImage  ", path, static_cast<unsigned>(header.assetType),
              header.bitDepth, header.width, header.height, header.rawLen, header.flags,
              check == wallet::AssetCheck::Ok ? "accepted" : "REFUSED");
  if (check != wallet::AssetCheck::Ok) {
    static const char* names[] = {"Ok",         "Malformed",         "Encrypted", "BitDepth",
                                  "WrongPanel", "PageImageMismatch", "NotACode"};
    std::fprintf(stderr, "refused: %s\n", names[static_cast<unsigned>(check)]);
    return false;
  }
  return true;
}

// `--grey`. Decodes the baked planes, decodes the 2bpp image, compares them where
// the level carries both, and writes a four-level PNG of the window.
int renderGrey(const std::string& tree, const wallet::PageImageSpec& planes, const wallet::PageImageSpec& image,
               const wallet::PanelGeometry& panel, long winX, long winY, const uint8_t* key, const std::string& out) {
  const bool havePlanes = planes.present;
  const bool haveImage = image.present;
  const wallet::PageImageSpec& geometry = havePlanes ? planes : image;

  // The x limit is a byte count times eight in BOTH forms: what bounds it is the
  // 1bpp plane row a panel row is read out of, not the 2bpp stride.
  const uint32_t planeRowBytes = wallet::greyPlaneRowBytes(geometry.nativeWidth);
  const uint32_t limitX = planeRowBytes > panel.rowBytes ? (planeRowBytes - panel.rowBytes) * 8u : 0u;
  const uint32_t x = wallet::clampWindowOrigin(
      winX < 0 ? static_cast<int32_t>(geometry.focalX) : static_cast<int32_t>(winX), limitX, 0);
  const uint32_t y =
      wallet::clampWindowOrigin(winY < 0 ? static_cast<int32_t>(geometry.focalY) : static_cast<int32_t>(winY),
                                geometry.nativeHeight, panel.height);
  if ((x % 8) != 0) {
    std::fprintf(stderr, "window x=%u is not 8-aligned\n", x);
    return 1;
  }
  std::printf("grey window : %u,%u of %ux%u (x limit %u)\n", x, y, geometry.nativeWidth, geometry.nativeHeight, limitX);

  std::vector<uint8_t> fromPlanes;
  std::vector<uint8_t> fromImage;
  const bool haveKey = key != nullptr;

  if (havePlanes) {
    std::vector<uint8_t> file;
    wallet::AssetHeader header;
    if (!loadGreyAsset(tree, planes, panel, true, haveKey, file, header)) return 1;
    const bool encrypted = (header.flags & wallet::kFlagEncrypted) != 0;
    uint8_t iv[wallet::kAssetIvLen] = {0};
    if (encrypted) {
      if (!haveKey) {
        std::fprintf(stderr, "the grey planes asset is encrypted; pass --key\n");
        return 1;
      }
      if (!wallet::buildAssetIv(planes.assetId, header.version, iv)) return 1;
    }
    if (!decodeGreyWindow(file, planes, panel, true, x, y, encrypted, key, iv, fromPlanes)) return 1;
  }
  if (haveImage) {
    std::vector<uint8_t> file;
    wallet::AssetHeader header;
    if (!loadGreyAsset(tree, image, panel, false, haveKey, file, header)) return 1;
    const bool encrypted = (header.flags & wallet::kFlagEncrypted) != 0;
    uint8_t iv[wallet::kAssetIvLen] = {0};
    if (encrypted) {
      if (!haveKey) {
        std::fprintf(stderr, "the grey image asset is encrypted; pass --key\n");
        return 1;
      }
      if (!wallet::buildAssetIv(image.assetId, header.version, iv)) return 1;
    }
    if (!decodeGreyWindow(file, image, panel, false, x, y, encrypted, key, iv, fromImage)) return 1;
  }

  // The check that matters. The device streams the planes without looking at a
  // pixel, so nothing on the device can notice a wrong bit order; the 2bpp form is
  // the only independent statement of the same picture.
  if (havePlanes && haveImage) {
    size_t differ = 0;
    size_t firstAt = 0;
    for (size_t i = 0; i < fromPlanes.size(); ++i) {
      if (fromPlanes[i] == fromImage[i]) continue;
      if (differ == 0) firstAt = i;
      ++differ;
    }
    if (differ == 0) {
      std::printf("cross-check  : the baked planes and the 2bpp image agree on all %zu pixels\n", fromPlanes.size());
    } else {
      std::printf("cross-check  : DISAGREE on %zu of %zu pixels, first at panel %zu,%zu (planes %u, image %u)\n",
                  differ, fromPlanes.size(), firstAt % panel.width, firstAt / panel.width, fromPlanes[firstAt],
                  fromImage[firstAt]);
    }
  }

  const std::vector<uint8_t>& levels = havePlanes ? fromPlanes : fromImage;
  size_t histogram[wallet::kGreyValues] = {0, 0, 0, 0};
  for (const uint8_t level : levels) histogram[level & 0x3]++;
  const double total = static_cast<double>(levels.size());
  std::printf("levels       : black %.2f %%, dark grey %.2f %%, light grey %.2f %%, white %.2f %% (from the %s)\n",
              100.0 * histogram[0] / total, 100.0 * histogram[1] / total, 100.0 * histogram[2] / total,
              100.0 * histogram[3] / total, havePlanes ? "baked planes" : "2bpp image");
  if (histogram[1] == 0 || histogram[2] == 0) {
    // Not an error -- a page can legitimately have no mid tones -- but for the test
    // pattern it means the plane encoding collapsed, which is the bug this tool is
    // for.
    std::printf("levels       : WARNING one of the two greys is absent\n");
  }

  const std::vector<uint8_t> landscape = levelPanelImage(levels, panel);
  if (!writeGreyPng(out + ".png", landscape, panel.width, panel.height)) {
    std::fprintf(stderr, "cannot write %s.png\n", out.c_str());
    return 1;
  }
  int pw = 0;
  int ph = 0;
  const std::vector<uint8_t> portrait = levelPortraitImage(levels, panel, pw, ph);
  if (!writeGreyPng(out + "-portrait.png", portrait, pw, ph)) {
    std::fprintf(stderr, "cannot write %s-portrait.png\n", out.c_str());
    return 1;
  }
  std::printf("wrote        : %s.png (%dx%d panel), %s-portrait.png (%dx%d as held), 0/85/170/255 per level\n",
              out.c_str(), panel.width, panel.height, out.c_str(), pw, ph);
  return 0;
}

// `--synth-grey DIR`. A tree with one page and one level, carrying the 1bpp page
// image and both grey forms of the same test pattern.
//
// The point is not to imitate the generator. It is that the grey byte layout can be
// checked, and looked at, before the generator's own grey output exists -- and that
// when it does exist, this tree is what a disagreement is diffed against.
int writeSynthGreyTree(const std::string& dir, const wallet::PanelGeometry& panel) {
  const uint16_t width = static_cast<uint16_t>(panel.width * 2);
  const uint16_t height = static_cast<uint16_t>(panel.height * 2);
  const wallet::host::GreyPage page = wallet::host::makeGreyPage(width, height);
  const std::vector<uint8_t> oneBpp = wallet::host::makeOneBppPage(page);

  static const char* kOneBppId = "a0a0a0a0a0a0a001";
  static const char* kPlanesId = "b0b0b0b0b0b0b002";
  static const char* kImageId = "c0c0c0c0c0c0c003";

  ::mkdir(dir.c_str(), 0775);
  struct Asset {
    const char* id;
    std::vector<uint8_t> file;
  };
  const Asset assets[] = {
      {kOneBppId,
       wallet::host::buildAssetFile(wallet::AssetType::PageImage, wallet::kBitDepth1, width, height, oneBpp)},
      {kPlanesId,
       wallet::host::buildAssetFile(wallet::AssetType::GreyPlanes, wallet::kBitDepth2, width, height, page.planes)},
      {kImageId,
       wallet::host::buildAssetFile(wallet::AssetType::PageImageGrey, wallet::kBitDepth2, width, height, page.twoBpp)},
  };
  for (const Asset& asset : assets) {
    const std::string shard = dir + "/" + std::string(asset.id).substr(0, 2);
    ::mkdir(shard.c_str(), 0775);
    char path[512];
    if (!wallet::buildAssetPathIn(dir.c_str(), asset.id, path, sizeof(path))) return 1;
    FILE* fh = std::fopen(path, "wb");
    if (fh == nullptr) {
      std::fprintf(stderr, "cannot write %s\n", path);
      return 1;
    }
    const bool ok = std::fwrite(asset.file.data(), 1, asset.file.size(), fh) == asset.file.size();
    std::fclose(fh);
    if (!ok) return 1;
    std::printf("wrote asset : %s (%zu bytes)\n", path, asset.file.size());
  }

  const std::string manifest =
      wallet::host::greyManifestJson(page, kOneBppId, kPlanesId, kImageId, panel.width, panel.height, panel.rowBytes);
  const std::string manifestPath = dir + "/manifest.json";
  FILE* fh = std::fopen(manifestPath.c_str(), "wb");
  if (fh == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", manifestPath.c_str());
    return 1;
  }
  const bool ok = std::fwrite(manifest.data(), 1, manifest.size(), fh) == manifest.size();
  std::fclose(fh);
  if (!ok) return 1;
  std::printf("wrote        : %s -- page %ux%u, %u B/plane row, %u B/2bpp row\n", manifestPath.c_str(), width, height,
              page.planeRowBytes, page.greyRowBytes);
  return 0;
}

void usage() {
  std::fprintf(stderr,
               "usage: wallet_preview --tree DIR --out PREFIX [--panel x4|x3]\n"
               "                     [--asset <16 hex> | --item N --page N\n"
               "                      --level fit|detail|one_to_one --col N --row N]\n"
               "                     [--code N] [--grey] [--key <64 hex>]\n"
               "       wallet_preview --synth-grey DIR [--panel x4|x3]\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string tree;
  std::string out;
  std::string panelName = "x4";
  std::string assetId;
  std::string levelName = "fit";
  int item = 0;
  int page = 0;
  int col = 0;
  int row = 0;
  long winX = -1;
  long winY = -1;
  int codeIndex = -1;
  bool grey = false;
  std::string synthDir;
  std::string keyHex;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool hasNext = i + 1 < argc;
    if (arg == "--tree" && hasNext)
      tree = argv[++i];
    else if (arg == "--out" && hasNext)
      out = argv[++i];
    else if (arg == "--panel" && hasNext)
      panelName = argv[++i];
    else if (arg == "--asset" && hasNext)
      assetId = argv[++i];
    else if (arg == "--level" && hasNext)
      levelName = argv[++i];
    else if (arg == "--item" && hasNext)
      item = std::atoi(argv[++i]);
    else if (arg == "--page" && hasNext)
      page = std::atoi(argv[++i]);
    else if (arg == "--col" && hasNext)
      col = std::atoi(argv[++i]);
    else if (arg == "--row" && hasNext)
      row = std::atoi(argv[++i]);
    else if (arg == "--win-x" && hasNext)
      winX = std::atol(argv[++i]);
    else if (arg == "--win-y" && hasNext)
      winY = std::atol(argv[++i]);
    else if (arg == "--code" && hasNext)
      codeIndex = std::atoi(argv[++i]);
    else if (arg == "--grey")
      grey = true;
    else if (arg == "--synth-grey" && hasNext)
      synthDir = argv[++i];
    else if (arg == "--key" && hasNext)
      keyHex = argv[++i];
    else {
      usage();
      return 2;
    }
  }
  if (synthDir.empty() && (tree.empty() || out.empty())) {
    usage();
    return 2;
  }
  if (grey && !assetId.empty()) {
    std::fprintf(stderr, "--grey resolves the level's grey asset through the manifest; drop --asset\n");
    return 2;
  }

  uint8_t key[wallet::kWalletKeyLen] = {0};
  bool haveKey = false;
  if (!keyHex.empty()) {
    if (!wallet::hexToBytes(keyHex.c_str(), key, sizeof(key))) {
      std::fprintf(stderr, "--key wants exactly %zu hex characters\n", wallet::kWalletKeyLen * 2);
      return 2;
    }
    haveKey = true;
  }

  wallet::PanelGeometry panel = wallet::kPanelX4;
  if (panelName == "x3")
    panel = wallet::kPanelX3;
  else if (panelName != "x4") {
    std::fprintf(stderr, "unknown panel %s\n", panelName.c_str());
    return 2;
  }

  if (!synthDir.empty()) return writeSynthGreyTree(synthDir, panel);

  wallet::Level level = wallet::Level::Fit;
  if (!wallet::levelFromKey(levelName.c_str(), level)) {
    std::fprintf(stderr, "unknown level %s\n", levelName.c_str());
    return 2;
  }

  // Resolve the asset through the manifest unless one was named outright.
  wallet::DeclaredPanel declared;
  wallet::PageImageSpec pageImage;
  wallet::CodeEntry code;
  if (codeIndex >= 0) {
    wallet::ManifestParser parser;
    parser.beginCodeLookup(item, codeIndex);
    if (!feedManifest(tree, parser, haveKey ? key : nullptr)) return 1;
    const wallet::CodeLookup& found = parser.codes();
    std::printf("manifest    : formatVersion %u, walletVersion %u\n", parser.formatVersion(), parser.walletVersion());
    if (!found.itemFound) {
      std::fprintf(stderr, "item %d is not in the manifest\n", item);
      return 1;
    }
    std::printf("item        : \"%s\", %u code(s)\n", found.title, found.codeCount);
    if (!found.code.present) {
      std::fprintf(stderr, "item %d has no code at index %d\n", item, codeIndex);
      return 1;
    }
    code = found.code;
    std::printf("code        : %s %s, %s, module %u px, quiet zone %u modules, %ux%u px drawn\n", code.id,
                code.symbology, code.verified ? "VERIFIED" : "NOT VERIFIED", code.moduleSize, code.quietZone,
                code.codeWidthPx, code.codeHeightPx);
    std::printf("manifest sha: %s\n",
                code.sha256[0] != '\0' ? code.sha256 : "(none -- header prefix is the authority)");
    assetId = code.assetId;
  } else if (assetId.empty()) {
    wallet::ManifestParser parser;
    parser.beginLookup(item, page, level, static_cast<uint8_t>(col), static_cast<uint8_t>(row));
    if (!feedManifest(tree, parser, haveKey ? key : nullptr)) return 1;
    declared = parser.panel();
    const wallet::PageLookup& found = parser.lookup();
    std::printf("manifest    : formatVersion %u, walletVersion %u\n", parser.formatVersion(), parser.walletVersion());
    if (declared.present) {
      std::printf("declares    : panel %s %ux%u, %u B/row, %u B/asset -- %s\n", declared.name, declared.width,
                  declared.height, declared.rowBytes, declared.assetBytes,
                  wallet::panelMatches(declared, panel) ? "matches" : "MISMATCH");
    } else {
      std::printf("declares    : no panel object (pre-panel manifest)\n");
    }
    if (!found.itemFound || !found.pageFound) {
      std::fprintf(stderr, "item %d page %d is not in the manifest\n", item, page);
      return 1;
    }
    if (grey) {
      // Grey is resolved and rendered on its own path: the level may carry a grey
      // asset and no tile assets at all, and the four-level decode has nothing to do
      // with the 1bpp framebuffer expansion below.
      std::printf("item        : \"%s\", %u page(s)\n", found.title, found.pageCount);
      if (!found.greyPlanes.present && !found.greyImage.present) {
        std::fprintf(stderr, "item %d page %d %s carries no grey asset (greyPlanes / greyPageImage)\n", item, page,
                     levelName.c_str());
        return 1;
      }
      if (found.greyPlanes.present) {
        std::printf("greyPlanes  : %s %ux%u, %u B/row, rawLen %u, step %u,%u, focal %u,%u\n", found.greyPlanes.assetId,
                    found.greyPlanes.nativeWidth, found.greyPlanes.nativeHeight, found.greyPlanes.rowBytes,
                    found.greyPlanes.rawLen, found.greyPlanes.windowStepX, found.greyPlanes.windowStepY,
                    found.greyPlanes.focalX, found.greyPlanes.focalY);
      }
      if (found.greyImage.present) {
        std::printf("greyImage   : %s %ux%u, %u B/row, rawLen %u\n", found.greyImage.assetId,
                    found.greyImage.nativeWidth, found.greyImage.nativeHeight, found.greyImage.rowBytes,
                    found.greyImage.rawLen);
      }
      return renderGrey(tree, found.greyPlanes, found.greyImage, panel, winX, winY, haveKey ? key : nullptr, out);
    }
    // A tile assetId is needed only when there is no page image to blit out of.
    // A level may legitimately carry a page image and no tiles at all (design B),
    // and this tool used to refuse such a tree even though the device draws it.
    if (!found.assetFound && !found.pageImage.present) {
      std::fprintf(stderr, "item %d page %d %s tile %d,%d not in the manifest\n", item, page, levelName.c_str(), col,
                   row);
      return 1;
    }
    std::printf("item        : \"%s\", %u page(s)\n", found.title, found.pageCount);
    for (uint8_t i = 0; i < wallet::kLevelCount; ++i) {
      std::printf("grid %-11s: %ux%u\n", wallet::levelName(static_cast<wallet::Level>(i)), found.grid[i].cols,
                  found.grid[i].rows);
    }
    pageImage = found.pageImage;
    if (pageImage.present) {
      std::printf("page image  : %s %ux%u, %u B/row, rawLen %u, step %u,%u, focal %u,%u\n", pageImage.assetId,
                  pageImage.nativeWidth, pageImage.nativeHeight, pageImage.rowBytes, pageImage.rawLen,
                  pageImage.windowStepX, pageImage.windowStepY, pageImage.focalX, pageImage.focalY);
      assetId = pageImage.assetId;
    } else {
      assetId = found.assetId;
    }
  }

  char path[512];
  if (!wallet::buildAssetPathIn(tree.c_str(), assetId.c_str(), path, sizeof(path))) {
    std::fprintf(stderr, "assetId %s is not 16 hex characters, or the path does not fit\n", assetId.c_str());
    return 1;
  }
  std::printf("asset       : %s\n", path);

  // The .dat is what the device reads. A tree that carries only the sidecars
  // (the committed fixture does, because it is a third of the size) is read
  // through the sidecar instead and rebuilt into the same bytes.
  std::vector<uint8_t> file;
  if (!readWholeFile(path, file)) {
    std::string rle(path);
    rle.replace(rle.size() - 4, 4, ".rle");
    std::vector<uint8_t> blob;
    if (!readWholeFile(rle, blob)) {
      std::fprintf(stderr, "cannot read %s (nor %s)\n", path, rle.c_str());
      return 1;
    }
    std::vector<uint8_t> payload;
    if (!wallet::host::decodeSidecarPayload(blob, payload)) {
      std::fprintf(stderr, "cannot decode sidecar %s\n", rle.c_str());
      return 1;
    }
    file.assign(blob.begin(), blob.begin() + wallet::kAssetHeaderBytes);
    file.insert(file.end(), payload.begin(), payload.end());
    std::printf("source      : %s (sidecar, rebuilt to %zu bytes)\n", rle.c_str(), file.size());
  }

  wallet::AssetHeader header;
  const wallet::AssetCheck check =
      codeIndex >= 0
          ? wallet::checkCodeAsset(file.data(), file.size(), panel, header, haveKey)
          : (pageImage.present ? wallet::checkPageImage(file.data(), file.size(), pageImage, panel, header, haveKey)
                               : wallet::checkAssetForPanel(file.data(), file.size(), panel, header, haveKey));
  std::printf(
      "header      : type %u, bitDepth %u, tile %u,%u, %ux%u, rawLen %u, version %u, flags %u, "
      "presentation %u\n",
      static_cast<unsigned>(header.assetType), header.bitDepth, header.tileCol, header.tileRow, header.width,
      header.height, header.rawLen, header.version, header.flags, header.presentation);
  if (check != wallet::AssetCheck::Ok) {
    static const char* names[] = {"Ok",         "Malformed",         "Encrypted", "BitDepth",
                                  "WrongPanel", "PageImageMismatch", "NotACode"};
    std::fprintf(stderr, "refused: %s (panel %ux%u, %u B/row, %u B/asset)\n", names[static_cast<unsigned>(check)],
                 panel.width, panel.height, panel.rowBytes, panel.bufferBytes);
    return 1;
  }

  const bool encrypted = (header.flags & wallet::kFlagEncrypted) != 0;
  uint8_t iv[wallet::kAssetIvLen] = {0};
  if (encrypted) {
    if (!haveKey) {
      std::fprintf(stderr, "%s is encrypted; pass --key\n", path);
      return 1;
    }
    if (!wallet::buildAssetIv(assetId.c_str(), header.version, iv)) {
      std::fprintf(stderr, "cannot build an IV for %s\n", assetId.c_str());
      return 1;
    }
    char ivHex[wallet::kAssetIvLen * 2 + 1];
    for (size_t i = 0; i < wallet::kAssetIvLen; ++i) std::snprintf(ivHex + i * 2, 3, "%02x", iv[i]);
    std::printf("crypto      : AES-256-CTR, IV %s (assetId || version %u || 0)\n", ivHex, header.version);
  }

  if (!pageImage.present && file.size() < wallet::kAssetHeaderBytes + header.rawLen) {
    std::fprintf(stderr, "short payload: %zu bytes of file, header wants %u after 32\n", file.size(), header.rawLen);
    return 1;
  }
  // The device reads straight into the framebuffer; here the vector *is* the
  // framebuffer. Same bytes, same offset, no transform.
  std::vector<uint8_t> fb;
  if (pageImage.present) {
    // Design B. Clamp the origin the way the device does -- x against a byte
    // count times eight so it cannot come out unaligned -- then lift the window
    // out row by row, which is PageReader::readWindow()'s arithmetic with a
    // memcpy where the device has a seek and a read.
    const uint32_t limitX = wallet::maxWindowX(pageImage, panel.rowBytes);
    const uint32_t x = wallet::clampWindowOrigin(
        winX < 0 ? static_cast<int32_t>(pageImage.focalX) : static_cast<int32_t>(winX), limitX, 0);
    const uint32_t y =
        wallet::clampWindowOrigin(winY < 0 ? static_cast<int32_t>(pageImage.focalY) : static_cast<int32_t>(winY),
                                  pageImage.nativeHeight, panel.height);
    if ((x % 8) != 0) {
      std::fprintf(stderr, "window x=%u is not 8-aligned\n", x);
      return 1;
    }
    std::printf("window      : %u,%u of %ux%u (x limit %u)\n", x, y, pageImage.nativeWidth, pageImage.nativeHeight,
                limitX);
    fb.resize(panel.bufferBytes);
    for (uint32_t r = 0; r < panel.height; ++r) {
      const size_t off = wallet::kAssetHeaderBytes + static_cast<size_t>(y + r) * pageImage.rowBytes + x / 8;
      if (off + panel.rowBytes > file.size()) {
        std::fprintf(stderr, "row %u runs past the file\n", r);
        return 1;
      }
      std::memcpy(fb.data() + static_cast<size_t>(r) * panel.rowBytes, file.data() + off, panel.rowBytes);
      if (encrypted) {
        // Per row, at that row's own payload offset -- the file offset minus the
        // 32-byte cleartext header. This is PageReader::readWindow()'s arithmetic, and
        // the reason the format uses CTR at all.
        const uint32_t payloadOffset = static_cast<uint32_t>(off - wallet::kAssetHeaderBytes);
        wallet::host::ctrXor(key, iv, payloadOffset, fb.data() + static_cast<size_t>(r) * panel.rowBytes,
                             panel.rowBytes);
      }
    }
  } else {
    fb.assign(file.begin() + wallet::kAssetHeaderBytes, file.begin() + wallet::kAssetHeaderBytes + header.rawLen);
    if (encrypted) {
      wallet::host::ctrXor(key, iv, 0, fb.data(), fb.size());
      // The header's prefix covers the PLAINTEXT, so this is where it can be checked --
      // and on an encrypted asset it is also the wrong-key detector.
      const wallet::HashResult hash = wallet::checkPayloadHash(fb.data(), fb.size(), nullptr, header.sha256Prefix);
      std::printf("plaintext   : sha256 prefix %s\n", hash.ok ? "MATCHES the header" : "DOES NOT MATCH -- wrong key?");
      if (!hash.ok) return 1;
    }
  }

  size_t inked = 0;
  for (int y = 0; y < panel.height; ++y) {
    for (int x = 0; x < panel.width; ++x) {
      if (!isWhite(fb, panel.rowBytes, x, y)) ++inked;
    }
  }
  std::printf("ink         : %.2f %% of %d pixels\n",
              100.0 * static_cast<double>(inked) / (static_cast<double>(panel.width) * panel.height),
              panel.width * panel.height);

  if (codeIndex >= 0) {
    // The hash the device checks before it draws anything, run here on the same
    // bytes with the same function.
    const wallet::HashResult hash =
        wallet::checkPayloadHash(fb.data(), header.rawLen, code.sha256, header.sha256Prefix);
    std::printf("hash        : %s against the %s\n", hash.ok ? "MATCH" : "MISMATCH",
                hash.authority == wallet::HashAuthority::Full ? "manifest's sha256" : "header's 8-byte prefix");
    if (!hash.ok) {
      std::fprintf(stderr, "the payload does not hash to what the manifest promised\n");
      return 1;
    }

    // The drawn code's bounding box, measured off the pixels in LOGICAL portrait
    // coordinates -- what a rider sees. Nothing here trusts the manifest; the
    // manifest is printed beside it so the two can be compared.
    const int logicalW = panel.height;
    const int logicalH = panel.width;
    int minX = logicalW, minY = logicalH, maxX = -1, maxY = -1;
    for (int y = 0; y < logicalH; ++y) {
      for (int x = 0; x < logicalW; ++x) {
        const int phyX = y;
        const int phyY = panel.height - 1 - x;
        if (isWhite(fb, panel.rowBytes, phyX, phyY)) continue;
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
      }
    }
    if (maxX < 0) {
      std::fprintf(stderr, "the code asset has no ink at all\n");
      return 1;
    }
    const int drawnW = maxX - minX + 1;
    const int drawnH = maxY - minY + 1;
    const int qzPx = static_cast<int>(code.quietZone) * static_cast<int>(code.moduleSize);
    std::printf("modules     : %dx%d px of dark modules at %u px per module\n", drawnW, drawnH, code.moduleSize);
    std::printf("with quiet  : %dx%d px, manifest says %ux%u\n", drawnW + 2 * qzPx, drawnH + 2 * qzPx, code.codeWidthPx,
                code.codeHeightPx);
    std::printf("margins     : left %d, right %d, top %d, bottom %d px (quiet zone needs %d)\n", minX,
                logicalW - 1 - maxX, minY, logicalH - 1 - maxY, qzPx);
    std::printf("centred     : x off by %d px, y off by %d px\n", minX - (logicalW - 1 - maxX),
                minY - (logicalH - 1 - maxY));
    std::printf("headroom    : %.1f %% of the logical %dx%d screen used by the code plus its quiet zone\n",
                100.0 * static_cast<double>(drawnW + 2 * qzPx) / logicalW, logicalW, logicalH);

    // The band the symbology label would use, tested exactly as the device tests
    // it. `lineHeight` is not known here, so a 14 px line plus the device's 4 px
    // pad and 10 px bottom margin stands in for it (WalletCodeActivity.cpp).
    const int bandBottom = logicalH - 10;
    const int bandTop = bandBottom - 14 - 4;
    std::printf("label band  : logical y %d..%d is %s\n", bandTop, bandBottom + 4,
                wallet::logicalBandIsBlank(fb.data(), panel, bandTop, bandBottom + 4) ? "blank -- label fits"
                                                                                      : "INKED -- no label");
  }

  const std::vector<uint8_t> landscape = panelImage(fb, panel);
  if (!writeGreyPng(out + ".png", landscape, panel.width, panel.height)) {
    std::fprintf(stderr, "cannot write %s.png\n", out.c_str());
    return 1;
  }
  int pw = 0;
  int ph = 0;
  const std::vector<uint8_t> portrait = portraitImage(fb, panel, pw, ph);
  if (!writeGreyPng(out + "-portrait.png", portrait, pw, ph)) {
    std::fprintf(stderr, "cannot write %s-portrait.png\n", out.c_str());
    return 1;
  }
  std::printf("wrote       : %s.png (%dx%d panel), %s-portrait.png (%dx%d as held)\n", out.c_str(), panel.width,
              panel.height, out.c_str(), pw, ph);
  return 0;
}
