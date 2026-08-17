#pragma once

#include <I18n.h>

#include "PinCatalog.h"
#include "PinStore.h"

// The user-facing name of a pin type. Device side only: this is the half that
// needs the generated I18nKeys.h, which the host tests do not build -- keeping it
// out of PinCatalog.h is what lets the catalogue be tested natively.
//
// `#1`-`#5` carry no StrId. They are not language-dependent, the same call as the
// map's "+"/"--" side hints and the direction arrows, so they come straight off
// the catalogue's ASCII name.

inline const char* pinTypeLabel(size_t catalogIndex) {
  switch (catalogIndex) {
    case 0:
      return tr(STR_PIN_BASE);
    case 1:
      return tr(STR_PIN_PARKING);
    case 2:
      return tr(STR_PIN_DEST);
    case 3:
      return tr(STR_PIN_MEET);
    case 4:
      return tr(STR_PIN_CAMP);
    default:
      break;
  }
  // #1-#5, and anything a later catalogue row adds before it gets a string.
  return catalogIndex < kPinSlotCount ? kPinCatalog[catalogIndex].name : "?";
}

// The label for a stored pin. A pin whose key this build does not know shows the
// raw key rather than a blank row -- it has to stay recognisable enough to
// delete (PinCatalog.h, the unknown-key rule).
inline const char* pinEntryLabel(const PinEntry& entry) {
  if (entry.catalogIndex < kPinSlotCount) return pinTypeLabel(entry.catalogIndex);
  return entry.key;
}
