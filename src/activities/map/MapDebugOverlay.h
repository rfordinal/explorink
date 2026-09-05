#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

class GfxRenderer;

// The map screen's debug readout, as one managed window instead of a set of
// hardcoded line numbers.
//
// What it replaces: MapActivity::drawDebugLine(int y, char*) plus a `line1Y`,
// `line2Y`, `line3Y`, `line4Y` ladder recomputed at each call site. Two frame
// paths owned that ladder independently, both started from a compile-time
// `kTextTopY` that did not know the header had grown a second row in Hike
// mode, and a third caller (the GNSS fix log) had nowhere to put a line at
// all without picking a number and hoping nobody else had picked it.
//
// So: a feature that wants to say something here reserves a slot once, at map
// setup, and writes into that slot from its own loop. The window decides
// where the slot lands on the panel, keeps it inside the space it reserved,
// and spends the panel on it.
//
// Three things it owns and its callers therefore do not:
//
//  - **Geometry.** setLayout() is handed the map's own boundaries every frame,
//    so the window follows the header down when Hike mode adds its second row
//    rather than sitting on top of it, and stops short of the compass halo
//    rather than painting over it.
//  - **Staying inside.** A line too wide is trimmed to the box's inner width
//    (GfxRenderer::drawText does not clip, and GfxRenderer::drawPixel answers
//    every off-panel pixel with a LOG_ERR -- one overlong line is several
//    hundred error lines over USB CDC). A slot with no room left below is
//    dropped, once, with a LOG_ERR naming its owner -- never drawn over the
//    scale bar or the button hints.
//  - **The panel.** repaint() is a polled, windowed update in the same shape
//    as MapActivity::updateHeaderStatus(): it compares what each slot holds
//    against what was last drawn and spends a waveform pass only on a real
//    change.
//
// Not thread-safe. Every call is on the activity task, same as the renderer.
class MapDebugOverlay {
 public:
  static constexpr uint8_t kInvalidSlot = 0xFF;
  // Eight is well past the six the map itself registers today, and the whole
  // table is a fixed member of MapActivity -- no allocation on a screen that
  // already fights for heap during a viewport reset (docs/map-memory.md).
  static constexpr uint8_t kMaxSlots = 8;
  // 64 including the terminator. The widest the box can ever be is the left
  // margin to the compass halo (~371 px at 480 wide), which is about 53
  // UI_10 characters -- anything past that is trimmed at draw time anyway, so
  // a bigger buffer would only cost RAM.
  static constexpr size_t kMaxTextLen = 64;

  // Where the window may live on the panel this frame. Called before every
  // draw() and every repaint(), never cached across frames: `top` moves with
  // the mode (Hike's header is one row taller) and the renderer's own font
  // metrics decide the row pitch.
  //
  //  - `fontId`      the font every row is drawn in.
  //  - `textLeft`    x of the first glyph. The box's own left edge sits one
  //                  pad left of it, so this stays the same number the old
  //                  readout's kTextX was.
  //  - `top`         y of the box's top edge. Below the header bar, not below
  //                  the screen's top -- that is the whole fix for Hike mode.
  //  - `rightLimit`  first x the box may NOT touch. The compass halo's left
  //                  edge, so the box never paints over the compass.
  //  - `bottomLimit` first y the box may NOT touch. The scale bar's clearance
  //                  line, so a slot that would land under it is dropped.
  void setLayout(int fontId, int textLeft, int top, int rightLimit, int bottomLimit);

  // Reserve one row. Call once per feature, at map setup, and keep the id.
  // `owner` is a short tag -- it is what a dropped-slot LOG_ERR names, so
  // make it the feature ("gnss", "transfer"), not the content.
  //
  // Rows are drawn in reservation order, which is why reservation happens in
  // one place at setup rather than lazily on first use: the order on the
  // panel is then a property of that list, not of which feature happened to
  // produce a line first.
  //
  // Returns kInvalidSlot when the table is full; set() on that id is a no-op,
  // so a caller that ignores the return value degrades to silence rather than
  // to a corrupted neighbour.
  uint8_t reserve(const char* owner);

  // Write this slot's current line. Cheap: a vsnprintf into the slot's own
  // buffer, no drawing and no panel. Safe to call every loop with the same
  // text -- repaint() compares against what was drawn and does nothing.
  void set(uint8_t slot, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

  // Empty this slot. An empty slot takes no row: the rows below it move up
  // and the box gets shorter. This is how a frame path says "this line is not
  // mine" -- see MapActivity's overview frame, which clears the follow
  // frame's slots and fills its own.
  void clear(uint8_t slot);
  void clearAll();

  // Paint the window into the framebuffer as part of a full frame. Does not
  // touch the panel -- the frame's own refresh carries it.
  //
  // Also resets the high-water mark repaint() keeps (see the .cpp): a full
  // frame redraws the map underneath, so this is the one moment the box is
  // allowed to get shorter.
  void draw(GfxRenderer& renderer);

  // The polled update. Repaints and pushes a window to the panel only when a
  // slot's text actually differs from what was last drawn. Returns the rect
  // it refreshed through the out params, and true when it refreshed at all,
  // so the caller can account for the panel time it just spent.
  bool repaint(GfxRenderer& renderer, int& x, int& y, int& w, int& h);

  // True when a slot holds something other than what is on the panel. Split
  // out of repaint() so a caller can apply its own rate cap before paying for
  // the paint.
  bool dirty() const;

  // Nothing reserved, or every reserved slot empty: the caller can skip the
  // whole path. Not the same as !dirty() -- a window that just went empty is
  // empty and dirty at once.
  bool empty() const;

 private:
  struct Slot {
    char owner[12];
    char text[kMaxTextLen];
    char drawn[kMaxTextLen];
    bool used;
    // One LOG_ERR per slot, not one per frame: a window too short for its
    // slots is short every frame, and at ~1 Hz that is a log nobody can read
    // past.
    bool dropReported;
  };

  // Rows that fit between `top_` and `bottomLimit_`, before the high-water
  // mark is applied.
  int capacityRows(GfxRenderer& renderer) const;
  int rowStride(GfxRenderer& renderer) const;
  // Slots holding text right now, in reservation order.
  int visibleRows() const;
  void paint(GfxRenderer& renderer, int rows);
  void boxRect(GfxRenderer& renderer, int rows, int& x, int& y, int& w, int& h) const;

  Slot slots_[kMaxSlots] = {};
  uint8_t count_ = 0;

  int fontId_ = 0;
  int textLeft_ = 0;
  int top_ = 0;
  int rightLimit_ = 0;
  int bottomLimit_ = 0;
  bool layoutSet_ = false;

  // Tallest the box has been since the last full frame. repaint() may not
  // shrink the box: the map pixels a taller box covered were filled white and
  // are not coming back without a re-render, so a shorter box would leave a
  // white scar. Reset by draw().
  int highWaterRows_ = 0;
};
