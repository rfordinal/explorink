#pragma once

#include <Icon.h>

#include "PinCatalog.h"
#include "components/icons/pins16.h"

// Which glyph a pin type is drawn with.
//
// Lucide, through the existing pipeline (parent CLAUDE.md, Icons): the SVGs are
// in freeink-sdk/libs/assets/Icons/lucide/icons, the manifest is
// src/components/icons/pins.icons.txt, and pins16.h is generated -- never edited
// by hand.
//
// `#1`-`#5` share one glyph (a hash). Numbering them in the bitmap would mean
// five near-identical assets and a sixth the day the cap moves; the list is where
// a rider reads which number it is.
//
// A key this build does not know gets the same glyph, so a pin written by a later
// firmware is still visible on the map rather than silently absent.
inline const freeink::Icon& pinIcon(size_t catalogIndex) {
  switch (catalogIndex) {
    case 0:
      return icon_pin_base_16;
    case 1:
      return icon_pin_parking_16;
    case 2:
      return icon_pin_dest_16;
    case 3:
      return icon_pin_meet_16;
    case 4:
      return icon_pin_camp_16;
    default:
      return icon_pin_custom_16;
  }
}
