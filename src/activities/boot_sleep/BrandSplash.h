#pragma once

class GfxRenderer;

// Logo + wordmark + one line of subtitle, centred -- the block BootActivity
// and SleepActivity's text-title screens share. Draws only that block; the
// caller still owns clearScreen()/displayBuffer() and anything else on the
// page (BootActivity's version string, SleepActivity's invertScreen()).
void drawBrandSplash(GfxRenderer& renderer, const char* subtitle);
