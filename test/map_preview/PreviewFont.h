#pragma once

#include <cstdint>

// Host-side text for the map preview: the firmware's own built-in Noto Sans
// faces, measured with the firmware's own EpdFont and rasterized here.
//
// Why not reuse GfxRenderer's text path: it is bound to the framebuffer, the
// HAL and Arduino (lib/GfxRenderer/GfxRenderer.cpp, FontDecompressor's
// <Arduino.h>), none of which exists in this build. Why not a stand-in font:
// the whole point of the preview is deciding whether a label fits, and a label
// fits or does not fit in the real font's widths. So the metrics come from the
// same flash tables the device reads (EpdFont::getTextDimensions), and only the
// glyph decompression differs -- zlib here, uzlib on the device.
//
// Consequence, same as the rest of PpmCanvas: close to the device, not
// provably identical. Kerning, ligatures and advances are the device's own;
// combining marks are not implemented (Slovak place names arrive precomposed).
// A label placed here is placed at the same pixel on the panel; the ink inside
// it can differ by a pixel of antialiasing threshold.
namespace PreviewFont {

// Snaps a style's requested pixel size to a face the firmware carries, exactly
// as GfxRendererCanvas::fontIdForSize does -- nearest below, never above, so
// preview and device measure the same string with the same face.
// Returns nullptr only if the size maps to no face, which cannot happen today.
const void* pick(int sizePx, bool bold);

// Text width in pixels, and the line height the device would reserve for it
// (EpdFontData::advanceY, matching GfxRenderer::getLineHeight).
bool measure(const void* face, const char* utf8, int& outWidth, int& outHeight);

// Draws `utf8` with its top-left ink box at (x, y) -- the IMapCanvas contract --
// calling `plot(px, py, ctx)` for every pixel that takes ink. Threshold matches
// the device's BW mode: any glyph value darker than white gets a pixel
// (lib/GfxRenderer/GfxRenderer.cpp renderCharImpl, `bmpVal < 3`).
void draw(const void* face, const char* utf8, int x, int y, void (*plot)(int, int, void*), void* ctx);

}  // namespace PreviewFont
