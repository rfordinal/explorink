#include "MapDebugOverlay.h"

#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "GfxRenderer.h"

namespace {

constexpr const char* kLogTag = "MAPDBG";

// Breathing room between the box's edge and the text inside it. Carried over
// from the old readout's kDebugPad, which was tuned on the panel: less and the
// glyphs read as touching the backing's edge.
constexpr int kPad = 3;

}  // namespace

void MapDebugOverlay::setLayout(int fontId, int textLeft, int top, int rightLimit, int bottomLimit) {
  fontId_ = fontId;
  textLeft_ = textLeft;
  top_ = top;
  rightLimit_ = rightLimit;
  bottomLimit_ = bottomLimit;
  layoutSet_ = true;
}

uint8_t MapDebugOverlay::reserve(const char* owner) {
  if (count_ >= kMaxSlots) {
    LOG_ERR(kLogTag, "no slot left for '%s' -- %u already reserved", owner != nullptr ? owner : "?",
            static_cast<unsigned>(count_));
    return kInvalidSlot;
  }
  const uint8_t slot = count_++;
  Slot& s = slots_[slot];
  s.used = true;
  s.dropReported = false;
  s.text[0] = '\0';
  s.drawn[0] = '\0';
  snprintf(s.owner, sizeof(s.owner), "%s", owner != nullptr ? owner : "?");
  return slot;
}

void MapDebugOverlay::set(uint8_t slot, const char* fmt, ...) {
  if (slot >= count_) return;
  va_list args;
  va_start(args, fmt);
  vsnprintf(slots_[slot].text, sizeof(slots_[slot].text), fmt, args);
  va_end(args);
}

void MapDebugOverlay::clear(uint8_t slot) {
  if (slot >= count_) return;
  slots_[slot].text[0] = '\0';
}

void MapDebugOverlay::clearAll() {
  for (uint8_t i = 0; i < count_; ++i) slots_[i].text[0] = '\0';
}

int MapDebugOverlay::rowStride(GfxRenderer& renderer) const {
  // getLineHeight() (the font's full advanceY), not getTextHeight() (the
  // ascender alone): ascender-only spacing put each row's descenders -- the
  // "g" in a route name, the "y" in a place -- into the row below it.
  return renderer.getLineHeight(fontId_) + kPad;
}

int MapDebugOverlay::capacityRows(GfxRenderer& renderer) const {
  // The box is kPad + rows * rowStride tall (see boxRect()), so this inverts
  // that against the space between the header and whatever the caller named
  // as the bottom limit.
  const int available = bottomLimit_ - top_ - kPad;
  if (available <= 0) return 0;
  return available / rowStride(renderer);
}

int MapDebugOverlay::visibleRows() const {
  int rows = 0;
  for (uint8_t i = 0; i < count_; ++i) {
    if (slots_[i].text[0] != '\0') ++rows;
  }
  return rows;
}

bool MapDebugOverlay::empty() const { return visibleRows() == 0; }

bool MapDebugOverlay::dirty() const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (strcmp(slots_[i].text, slots_[i].drawn) != 0) return true;
  }
  return false;
}

void MapDebugOverlay::boxRect(GfxRenderer& renderer, int rows, int& x, int& y, int& w, int& h) const {
  x = textLeft_ - kPad;
  y = top_;
  w = rightLimit_ - x;
  h = kPad + rows * rowStride(renderer);
}

void MapDebugOverlay::paint(GfxRenderer& renderer, int rows) {
  int x = 0, y = 0, w = 0, h = 0;
  boxRect(renderer, rows, x, y, w, h);

  // One filled box for the whole window, not a backing per line. The readout
  // sits over live map lines, so text drawn straight onto a hatch or a road is
  // unreadable and something white has to go under it either way. A box wide
  // enough for the widest line it could hold, rather than tight to the text
  // actually there, is what makes the windowed repaint below honest: a
  // line that gets shorter leaves no stale ink outside a backing that moved.
  //
  // Nothing this covers needs to survive: the window is placed below the
  // header bar (setLayout()'s `top`) and stops at the compass halo
  // (`rightLimit`), so the only thing under it is map.
  renderer.fillRect(x, y, w, h, false);
  // A 1px frame, so the reserved space is visible as a boundary rather than
  // as a white smudge over the map -- and so a line that got trimmed reads as
  // trimmed by the window rather than as truncated data.
  renderer.drawRect(x, y, w, h, true);

  const int innerWidth = w - kPad * 2;
  const int stride = rowStride(renderer);
  int row = 0;
  for (uint8_t i = 0; i < count_; ++i) {
    Slot& s = slots_[i];
    // Recorded whether the row is drawn or dropped: dirty() compares against
    // this, and a slot whose text cannot be shown must not keep asking for a
    // repaint that will drop it again.
    memcpy(s.drawn, s.text, sizeof(s.drawn));
    if (s.text[0] == '\0') continue;
    if (row >= rows) {
      if (!s.dropReported) {
        s.dropReported = true;
        LOG_ERR(kLogTag, "slot '%s' dropped: window holds %d rows between y=%d and y=%d", s.owner, rows, top_,
                bottomLimit_);
      }
      continue;
    }

    // Trim to what fits. GfxRenderer::drawText does not clip and
    // GfxRenderer::drawPixel answers every off-panel pixel with a LOG_ERR, so
    // one overlong line is several hundred error lines over USB CDC -- and a
    // line running past `rightLimit_` would land on the compass.
    char line[kMaxTextLen];
    memcpy(line, s.text, sizeof(line));
    for (size_t len = strlen(line); len > 0 && renderer.getTextWidth(fontId_, line) > innerWidth; --len) {
      line[len - 1] = '\0';
    }
    renderer.drawText(fontId_, textLeft_, y + kPad + row * stride, line, true);
    ++row;
  }
}

void MapDebugOverlay::draw(GfxRenderer& renderer) {
  if (!layoutSet_) {
    LOG_ERR(kLogTag, "draw() with no layout -- call setLayout() each frame");
    return;
  }
  const int rows = std::min(visibleRows(), capacityRows(renderer));
  // The one moment the box may get shorter: the frame around it is being
  // redrawn, so the map under a row that went away comes back with it.
  highWaterRows_ = rows;
  if (rows <= 0) {
    // Still records what each slot holds, so the next repaint() does not read
    // an empty window as a change and spend a waveform pass on nothing.
    for (uint8_t i = 0; i < count_; ++i) memcpy(slots_[i].drawn, slots_[i].text, sizeof(slots_[i].drawn));
    return;
  }
  paint(renderer, rows);
}

bool MapDebugOverlay::repaint(GfxRenderer& renderer, int& x, int& y, int& w, int& h) {
  if (!layoutSet_ || !dirty()) return false;

  const int wanted = std::min(visibleRows(), capacityRows(renderer));
  // Never shorter than the box already on the panel -- see highWaterRows_.
  // A row that emptied leaves an empty row inside the box until the next full
  // frame, which is honest about what the panel holds and costs nothing.
  const int rows = std::max(wanted, highWaterRows_);
  if (rows <= 0) {
    for (uint8_t i = 0; i < count_; ++i) memcpy(slots_[i].drawn, slots_[i].text, sizeof(slots_[i].drawn));
    return false;
  }
  highWaterRows_ = rows;
  paint(renderer, rows);
  boxRect(renderer, rows, x, y, w, h);
  return true;
}
