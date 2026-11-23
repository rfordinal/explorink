#include "PreviewFont.h"

#include <zlib.h>

#include <cstring>
#include <map>
#include <vector>

#include "EpdFont.h"
#include "Utf8.h"
#include "builtinFonts/notosans_8_regular.h"
#include "builtinFonts/notosans_12_bold.h"
#include "builtinFonts/notosans_12_regular.h"
#include "builtinFonts/notosans_14_bold.h"
#include "builtinFonts/notosans_14_regular.h"
#include "builtinFonts/notosans_16_bold.h"
#include "builtinFonts/notosans_16_regular.h"
#include "builtinFonts/notosans_18_bold.h"
#include "builtinFonts/notosans_18_regular.h"
#include "builtinFonts/ubuntu_5_regular.h"
#include "builtinFonts/ubuntu_10_bold.h"
#include "builtinFonts/ubuntu_10_regular.h"
#include "builtinFonts/ubuntu_12_bold.h"
#include "builtinFonts/ubuntu_12_regular.h"

namespace {

const EpdFontData* asFont(const void* face) { return static_cast<const EpdFontData*>(face); }

// Which group holds a glyph. `glyphToGroup` is present when the generator could
// not keep groups contiguous; otherwise the groups themselves say where they
// start (EpdFontData.h, EpdFontGroup::firstGlyphIndex).
int groupFor(const EpdFontData* font, const uint32_t glyphIndex) {
  if (font->glyphToGroup != nullptr) return font->glyphToGroup[glyphIndex];
  for (int group = font->groupCount - 1; group >= 0; --group) {
    if (glyphIndex >= font->groups[group].firstGlyphIndex) return group;
  }
  return -1;
}

// Decompressed glyph blocks, kept for the life of the process. A preview run
// draws a handful of labels out of at most two faces, so this is a few tens of
// KB on a host with gigabytes -- the device's careful one-group-at-a-time cache
// (FontDecompressor) exists for a 380 KB budget this build does not have.
std::vector<uint8_t>* groupBitmap(const EpdFontData* font, const int group) {
  static std::map<std::pair<const EpdFontData*, int>, std::vector<uint8_t>> cache;
  const auto key = std::make_pair(font, group);
  const auto found = cache.find(key);
  if (found != cache.end()) return &found->second;

  const EpdFontGroup& block = font->groups[group];
  std::vector<uint8_t> out(block.uncompressedSize);

  // Raw DEFLATE, no zlib header -- windowBits negative is how zlib is told
  // that. Same stream the device hands uzlib (FontDecompressor.cpp).
  z_stream stream{};
  if (inflateInit2(&stream, -15) != Z_OK) return nullptr;
  stream.next_in = const_cast<uint8_t*>(&font->bitmap[block.compressedOffset]);
  stream.avail_in = block.compressedSize;
  stream.next_out = out.data();
  stream.avail_out = static_cast<unsigned>(out.size());
  const int status = inflate(&stream, Z_FINISH);
  inflateEnd(&stream);
  if (status != Z_STREAM_END) return nullptr;

  return &cache.emplace(key, std::move(out)).first->second;
}

// Where a glyph's rows start inside its decompressed group, and how many bytes
// one row takes.
//
// This is the trap in the format: inside a group the rows are **byte-aligned**
// -- ceil(width/4) bytes per row for a 2-bit font -- and a glyph's position is
// the running sum of the aligned sizes of the glyphs before it in that group.
// `EpdGlyph::dataOffset` indexes the *packed* form and is not that offset. The
// device pays the same price differently: it decompresses the aligned block and
// compacts one glyph into packed form before drawing
// (FontDecompressor::getAlignedOffset, compactSingleGlyph). Here the aligned
// rows are read in place, which is the same pixels with no copy.
struct GlyphBits {
  const uint8_t* data = nullptr;
  int rowStride = 0;
};

GlyphBits bitmapFor(const EpdFontData* font, const EpdGlyph* glyph) {
  const int bitsPerPixel = font->is2Bit ? 2 : 1;
  const int pixelsPerByte = 8 / bitsPerPixel;

  if (font->groups == nullptr || font->groupCount == 0) {
    // Uncompressed font: one packed run per glyph, rows not aligned. Row stride
    // is meaningless there, so the caller is handed a stride of 0 and walks the
    // run continuously, exactly as GfxRenderer does.
    return GlyphBits{&font->bitmap[glyph->dataOffset], 0};
  }

  const auto glyphIndex = static_cast<uint32_t>(glyph - font->glyph);
  const int group = groupFor(font, glyphIndex);
  if (group < 0) return GlyphBits{};
  const std::vector<uint8_t>* block = groupBitmap(font, group);
  if (block == nullptr) return GlyphBits{};

  const auto alignedSize = [&](const EpdGlyph& g) -> uint32_t {
    if (g.width == 0 || g.height == 0) return 0;
    return ((g.width + pixelsPerByte - 1) / pixelsPerByte) * g.height;
  };
  uint32_t offset = 0;
  if (font->glyphToGroup != nullptr) {
    for (uint32_t i = 0; i < glyphIndex; ++i) {
      if (font->glyphToGroup[i] == group) offset += alignedSize(font->glyph[i]);
    }
  } else {
    for (uint32_t i = font->groups[group].firstGlyphIndex; i < glyphIndex; ++i) offset += alignedSize(font->glyph[i]);
  }

  const int rowStride = (glyph->width + pixelsPerByte - 1) / pixelsPerByte;
  if (offset + static_cast<uint32_t>(rowStride) * glyph->height > block->size()) return GlyphBits{};
  return GlyphBits{block->data() + offset, rowStride};
}

}  // namespace

namespace {

// The same faces GfxRendererCanvas::kLabelFontIds offers, in the same order,
// with the same rule for choosing between them: largest line height that fits
// the style's request, smallest face when nothing fits. Both sides must agree or
// a label measured here would be a different width on the panel.
//
// notosans_8 has no bold cut in the firmware (src/main.cpp: smallFontFamily is
// built from the regular alone), so a bold request at that size gets the regular
// -- here as on the device.
struct Face {
  const EpdFontData* regular;
  const EpdFontData* bold;
};
const Face kFaces[] = {
    // A small Ubuntu cut, regular only, added 2026-08-26 for the contour height
    // numbers: a 12 px line against the 23 px floor everything else had.
    // Regular only is deliberate -- a number has no second tier.
    {&ubuntu_5_regular, &ubuntu_5_regular},
    {&notosans_8_regular, &notosans_8_regular}, {&ubuntu_10_regular, &ubuntu_10_bold},
    {&ubuntu_12_regular, &ubuntu_12_bold},      {&notosans_12_regular, &notosans_12_bold},
    {&notosans_14_regular, &notosans_14_bold},  {&notosans_16_regular, &notosans_16_bold},
    {&notosans_18_regular, &notosans_18_bold},
};

}  // namespace

const void* PreviewFont::pick(const int sizePx, const bool bold) {
  const Face* best = &kFaces[0];
  int bestHeight = 0;
  for (const Face& face : kFaces) {
    const int height = face.regular->advanceY;
    if (height > sizePx) continue;
    if (height > bestHeight) {
      bestHeight = height;
      best = &face;
    }
  }
  return bold ? static_cast<const void*>(best->bold) : static_cast<const void*>(best->regular);
}

bool PreviewFont::measure(const void* face, const char* utf8, int& outWidth, int& outHeight) {
  if (face == nullptr) return false;
  const EpdFontData* font = asFont(face);
  if (utf8 == nullptr || *utf8 == '\0') {
    outWidth = 0;
    outHeight = 0;
    return true;
  }
  int width = 0, height = 0;
  EpdFont(font).getTextDimensions(utf8, &width, &height);
  outWidth = width;
  outHeight = font->advanceY;
  return outWidth > 0 && outHeight > 0;
}

void PreviewFont::draw(const void* face, const char* utf8, const int x, const int y, void (*plot)(int, int, void*),
                       void* ctx) {
  if (face == nullptr || utf8 == nullptr || *utf8 == '\0' || plot == nullptr) return;
  const EpdFontData* font = asFont(face);
  const EpdFont metrics(font);

  // Mirrors GfxRenderer::drawText: the caller's y is the top of the ascender
  // box, and the cursor walks in 12.4 fixed-point with differential rounding so
  // the same character pair always steps by the same number of pixels.
  const int baselineY = y + font->ascender;
  int cursorX = x;
  int32_t prevAdvanceFP = 0;
  uint32_t prevCp = 0;

  const char* text = utf8;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    cp = metrics.applyLigatures(cp, text);
    if (prevCp != 0) cursorX += fp4::toPixel(prevAdvanceFP + metrics.getKerning(prevCp, cp));

    const EpdGlyph* glyph = metrics.getGlyph(cp);
    if (glyph == nullptr) {
      cursorX += fp4::toPixel(prevAdvanceFP);
      prevCp = 0;
      prevAdvanceFP = 0;
      continue;
    }
    prevAdvanceFP = glyph->advanceX;
    prevCp = cp;

    const GlyphBits bits = bitmapFor(font, glyph);
    if (bits.data == nullptr) continue;

    const int inkX = cursorX + glyph->left;
    const int inkY = baselineY - glyph->top;
    for (int glyphY = 0; glyphY < glyph->height; ++glyphY) {
      for (int glyphX = 0; glyphX < glyph->width; ++glyphX) {
        // Aligned rows (compressed fonts) index per row; a packed run
        // (uncompressed) counts pixels straight through.
        const int pixel = bits.rowStride > 0 ? glyphX : glyphY * glyph->width + glyphX;
        const uint8_t* row = bits.rowStride > 0 ? bits.data + glyphY * bits.rowStride : bits.data;
        bool ink;
        if (font->is2Bit) {
          // 4 pixels per byte, MSB first; the stored value is 0=white..3=black,
          // and the device's BW mode inks anything that is not pure white
          // (GfxRenderer.cpp renderCharImpl, `bmpVal < 3`).
          const uint8_t raw = (row[pixel >> 2] >> ((3 - (pixel & 3)) * 2)) & 0x3;
          ink = raw > 0;
        } else {
          ink = (row[pixel >> 3] >> (7 - (pixel & 7))) & 1;
        }
        if (ink) plot(inkX + glyphX, inkY + glyphY, ctx);
      }
    }
  }
}
