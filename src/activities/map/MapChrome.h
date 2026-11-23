#pragma once

#include <cstddef>
#include <cstdint>

// Which rectangles of the map screen are owned by something other than the map,
// so that anything placed *into* the map can refuse to draw under them.
//
// **Why this exists.** Screen furniture -- the four button boxes, the two side
// boxes, the compass, the scale bar, the header band, the debug window -- is
// drawn at the end of the frame, on top of everything. That is right for a road:
// a line running under the button row still reads as a road that continues off
// the panel. It is wrong for anything that has to be *found*: a pin under the
// scale bar is a pin the rider never sees, and a place name with half its
// letters painted over reads as a different name.
//
// Before this, each placer worked that out for itself. `MapActivity::
// pinEdgeArea()` asked the theme for both hint bands and then shrank a single
// rectangle until it cleared them, which costs the whole top band to clear a
// compass that only owns the top-right corner. Nothing else asked at all -- and
// on the X3 the scale bar and the place label draw on the same pixels
// (docs/TODO.md T-296).
//
// **A flat list, not an occupancy grid.** MapLabels already carries a
// MapOccupancyGrid for label-against-label placement, and the first draft of
// this reused it. It is the wrong instrument here: the furniture is about a
// dozen axis-aligned rectangles that never move within a frame, so a linear
// intersection test is both cheaper (no 1,250-byte grid to clear per frame) and
// exact at the pixel rather than to the nearest 8 px cell. The grid earns its
// keep when hundreds of boxes are added and queried; this is ten.
//
// **Every rectangle comes from whoever draws it.** No constant is copied here.
// A number restated in two files is a number free to drift, and the drift is
// invisible until it lands on the glass of a panel nobody has on the desk --
// which is exactly how T-296 happened.
enum class MapChromeTag : uint8_t {
  Header,       // the status band across the top, down to mapContentTop()
  Compass,      // the north indicator and its white halo, top right
  ScaleBar,     // the bar, its ticks and its numbers, bottom left
  DebugWindow,  // the diagnostic readout, off by default
  Button,       // one front button box, registered per box and not as a band
  SideButton,   // one side button box (zoom in Follow, pan in Observe)
  Notice,       // a transient overlay that owns its pixels for a while
};

class MapChromeRegister {
 public:
  // Ten pieces of furniture exist today: header, compass, scale bar, debug
  // window, four front boxes, two side boxes. Sixteen leaves room for the
  // notices without being worth a heap allocation -- the whole register is
  // 16 * 10 bytes and lives inside MapActivity.
  static constexpr size_t kMaxEntries = 16;

  struct Entry {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    MapChromeTag tag = MapChromeTag::Notice;
  };

  void clear() {
    count_ = 0;
    dropped_ = 0;
  }

  // Adds one rectangle. An empty one is ignored rather than stored: a theme
  // answers "this box is not drawn" with a zero-size rect (BaseTheme::
  // frontHintBox returns false and leaves the Rect untouched), and a register
  // holding zero-area entries would make every query walk them for nothing.
  //
  // Returns false when the register is full, and counts that. A silently
  // dropped entry is furniture that placers believe is not there, which is the
  // failure this class exists to prevent -- so the caller logs the count rather
  // than letting it pass.
  bool add(const int x, const int y, const int width, const int height, const MapChromeTag tag) {
    if (width <= 0 || height <= 0) return true;
    if (count_ >= kMaxEntries) {
      ++dropped_;
      return false;
    }
    Entry& entry = entries_[count_++];
    entry.x = static_cast<int16_t>(x);
    entry.y = static_cast<int16_t>(y);
    entry.w = static_cast<int16_t>(width);
    entry.h = static_cast<int16_t>(height);
    entry.tag = tag;
    return true;
  }

  // True when any registered rectangle shares a pixel with this one. Half-open
  // on both axes, the same convention Rect is drawn with, so a box that ends
  // exactly where a button box starts does not count as touching it.
  bool hits(const int x, const int y, const int width, const int height) const {
    if (width <= 0 || height <= 0) return false;
    for (size_t i = 0; i < count_; ++i) {
      const Entry& entry = entries_[i];
      if (x >= entry.x + entry.w || entry.x >= x + width) continue;
      if (y >= entry.y + entry.h || entry.y >= y + height) continue;
      return true;
    }
    return false;
  }

  size_t count() const { return count_; }
  size_t dropped() const { return dropped_; }
  const Entry& at(const size_t index) const { return entries_[index]; }

 private:
  Entry entries_[kMaxEntries];
  size_t count_ = 0;
  size_t dropped_ = 0;
};
