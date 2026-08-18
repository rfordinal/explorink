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
//                  [--asset <16 hex>] [--win-x N --win-y N] --out PREFIX
//
// When the level carries a design-B `pageImage`, the window at (--win-x, --win-y)
// is blitted out of it row by row -- the same row maths PageReader uses on the
// device, so an off-by-one stride or a mis-clamped origin shows up as a visibly
// wrong picture. Origins default to the manifest's focal point and are clamped
// exactly as the device clamps them.
//
// Writes PREFIX.png (the panel's own 800x480 landscape frame) and
// PREFIX-portrait.png (the same bits read the way a rider holds the device).

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "WalletAsset.h"
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
bool feedManifest(const std::string& tree, wallet::ManifestParser& parser) {
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

void usage() {
  std::fprintf(stderr,
               "usage: wallet_preview --tree DIR --out PREFIX [--panel x4|x3]\n"
               "                     [--asset <16 hex> | --item N --page N\n"
               "                      --level fit|detail|one_to_one --col N --row N]\n");
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

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool hasNext = i + 1 < argc;
    if (arg == "--tree" && hasNext) tree = argv[++i];
    else if (arg == "--out" && hasNext) out = argv[++i];
    else if (arg == "--panel" && hasNext) panelName = argv[++i];
    else if (arg == "--asset" && hasNext) assetId = argv[++i];
    else if (arg == "--level" && hasNext) levelName = argv[++i];
    else if (arg == "--item" && hasNext) item = std::atoi(argv[++i]);
    else if (arg == "--page" && hasNext) page = std::atoi(argv[++i]);
    else if (arg == "--col" && hasNext) col = std::atoi(argv[++i]);
    else if (arg == "--row" && hasNext) row = std::atoi(argv[++i]);
    else if (arg == "--win-x" && hasNext) winX = std::atol(argv[++i]);
    else if (arg == "--win-y" && hasNext) winY = std::atol(argv[++i]);
    else {
      usage();
      return 2;
    }
  }
  if (tree.empty() || out.empty()) {
    usage();
    return 2;
  }

  wallet::PanelGeometry panel = wallet::kPanelX4;
  if (panelName == "x3") panel = wallet::kPanelX3;
  else if (panelName != "x4") {
    std::fprintf(stderr, "unknown panel %s\n", panelName.c_str());
    return 2;
  }

  wallet::Level level = wallet::Level::Fit;
  if (!wallet::levelFromKey(levelName.c_str(), level)) {
    std::fprintf(stderr, "unknown level %s\n", levelName.c_str());
    return 2;
  }

  // Resolve the asset through the manifest unless one was named outright.
  wallet::DeclaredPanel declared;
  wallet::PageImageSpec pageImage;
  if (assetId.empty()) {
    wallet::ManifestParser parser;
    parser.beginLookup(item, page, level, static_cast<uint8_t>(col), static_cast<uint8_t>(row));
    if (!feedManifest(tree, parser)) return 1;
    declared = parser.panel();
    const wallet::PageLookup& found = parser.lookup();
    std::printf("manifest    : formatVersion %u, walletVersion %u\n", parser.formatVersion(),
                parser.walletVersion());
    if (declared.present) {
      std::printf("declares    : panel %s %ux%u, %u B/row, %u B/asset -- %s\n", declared.name, declared.width,
                  declared.height, declared.rowBytes, declared.assetBytes,
                  wallet::panelMatches(declared, panel) ? "matches" : "MISMATCH");
    } else {
      std::printf("declares    : no panel object (pre-panel manifest)\n");
    }
    if (!found.itemFound || !found.pageFound || !found.assetFound) {
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
  const wallet::AssetCheck check = pageImage.present
                                       ? wallet::checkPageImage(file.data(), file.size(), pageImage, panel, header)
                                       : wallet::checkAssetForPanel(file.data(), file.size(), panel, header);
  std::printf("header      : type %u, bitDepth %u, tile %u,%u, %ux%u, rawLen %u, version %u, flags %u, "
              "presentation %u\n",
              static_cast<unsigned>(header.assetType), header.bitDepth, header.tileCol, header.tileRow, header.width,
              header.height, header.rawLen, header.version, header.flags, header.presentation);
  if (check != wallet::AssetCheck::Ok) {
    static const char* names[] = {"Ok", "Malformed", "Encrypted", "BitDepth", "WrongPanel", "PageImageMismatch"};
    std::fprintf(stderr, "refused: %s (panel %ux%u, %u B/row, %u B/asset)\n",
                 names[static_cast<unsigned>(check)], panel.width, panel.height, panel.rowBytes, panel.bufferBytes);
    return 1;
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
    const uint32_t x = wallet::clampWindowOrigin(winX < 0 ? static_cast<int32_t>(pageImage.focalX)
                                                         : static_cast<int32_t>(winX),
                                                 limitX, 0);
    const uint32_t y = wallet::clampWindowOrigin(winY < 0 ? static_cast<int32_t>(pageImage.focalY)
                                                         : static_cast<int32_t>(winY),
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
    }
  } else {
    fb.assign(file.begin() + wallet::kAssetHeaderBytes,
              file.begin() + wallet::kAssetHeaderBytes + header.rawLen);
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
