#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BrandSplash.h"
#include "fontIds.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  drawBrandSplash(renderer, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, TRAILINK_VERSION);
  renderer.displayBuffer();
}
