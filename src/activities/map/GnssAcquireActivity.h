#pragma once

#ifdef ENABLE_GNSS_CMD

#include <Gnss.h>
#include <I18n.h>

#include <cstdint>

#include "GnssSkyView.h"
#include "MapRouteSource.h"
#include "activities/Activity.h"

// The screen the rider waits on while the receiver finds the sky, on the way
// into the map.
//
// ## Why a screen and not a banner on the map
//
// The map already has a waiting state -- `STR_MAP_WAITING_GNSS` over an empty
// panel -- and it is the wrong answer to the only question the rider has while
// it is up: **is this going to work, and should I move?** A banner says nothing
// about whether the receiver is hearing anything, so a wait that will never
// finish looks exactly like one that is nearly done. On this board that is not
// hypothetical: a 15-minute walk on 2026-09-04 got no fix at all while the
// receiver tracked one satellite the whole time (../docs/gnss.md), and a ride
// took over ten minutes to first fix (../docs/gnss-to-map-plan.md, step 5).
//
// So the wait gets its own screen, and the screen shows the sky: how many
// satellites are up there, how many the antenna can actually hear, how strong
// the best one is, and how long this has been going on. A rider can read
// "fourteen in view, none heard" and go and stand somewhere else, which is the
// one useful action available to them.
//
// Same shape as the two screens next to it: preparation is a separate screen,
// riding is the map (RouteSelectActivity.h, TileSyncActivity.h).
//
// ## It is skipped whenever it has nothing to add
//
// `ActivityManager::goToGnssAcquire()` goes straight to the map unless the GNSS
// setting is on AND the receiver has no usable fix yet. So a second entry into
// the map while the receiver is still running and tracking never shows this at
// all, and `CMD:GOTO_MAP` and the wake-into-map path do not route through it --
// the screenshot tooling and the resume path see exactly what they saw before.
//
// ## Two ways out, and the difference is which radio the map session runs
//
// One position source per map session (MapActivity's `bleInUse_`), so the two
// action rows are not "wait" versus "do not wait", they are a choice of source:
//
// - **Open the map now** hands the running receiver over to the map, which
//   finishes the acquisition behind a map frame drawn from the persisted last
//   fix. Nothing is lost by pressing it: the receiver keeps searching and the
//   header glyph goes from Seeking to Fixed when it lands.
// - **Take position from the phone** gives up on the sky for this session. The
//   rail goes down here and the map runs BLE instead, which is also the only
//   way to reach tile sync and the command channel while riding.
//
// Back goes home, same as the trip picker.
//
// ## What the receiver's ownership means here
//
// This screen starts the receiver (`gnssStart()`, which also seeds it with the
// persisted position -- main.cpp), and whoever started it must be the one to
// stop it. So the handover into the map passes `adoptRunningGnss`: without it
// the map would find a receiver already running, decline to own it, and leave
// the rail powered after the rider left the map (MapActivity's constructor). A
// receiver a host `CMD:GNSS ON` already owned is never adopted and never
// stopped here.
class GnssAcquireActivity final : public Activity {
 public:
  // `routePath` is carried through untouched to the map -- the trip picker runs
  // before this screen, so a rider who chose a trip must not lose it by waiting
  // for satellites.
  GnssAcquireActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* routePath = nullptr);

  void onEnter() override;
  void onExit() override;
  void loop() override;

  // The receiver must survive the wait. Deep sleep is a full chip reset, so a
  // device that sleeps here cold-starts the receiver and throws away everything
  // the wait bought -- the same defect that made a GNSS map session sleep out
  // from under a rider on 2026-09-04 (MapActivity::preventAutoSleep()).
  bool preventAutoSleep() override { return true; }
  // But the clock may throttle, exactly as it may on the map screen: this
  // screen is idle by definition and the UART is a 9600 baud trickle that
  // main.cpp's loop drains.
  bool preventThrottle() override { return false; }

 private:
  // Which row is highlighted. Open-the-map is first because it is the default
  // path: a rider who pressed Explore wants the map, and the phone row is the
  // detour.
  enum class Action : uint8_t { OpenMap = 0, UsePhone = 1 };
  static constexpr int kActionCount = 2;

  void renderScreen();
  void drawClock();
  void drawSky();
  void drawReadout();
  void drawActions();
  // The asset is wider than the panel and seated so its bottom runs past the
  // horizon, and GfxRenderer's blit LOG_ERRs rather than clips. So this one
  // clips: see its comment in the .cpp.
  void drawRidgeClipped(int x, int y, int clipTop, int clipBottom);
  // The sky the screen draws: the receiver's, or the bench's synthetic one when
  // it is switched on (GnssFakeSky.h). Every read goes through these five, so
  // the drawing cannot tell which sky it has.
  uint8_t skyCount() const;
  const GnssSatellite& skySatellite(uint8_t index) const;
  uint8_t skyInView() const;
  uint8_t skyHeard() const;
  uint8_t skyBestSnr() const;
  // 0 when the rider chose no limit (CrossPointSettings::mapGnssWaitLimit).
  uint32_t waitLimitMs() const;
  int clockTop() const;
  int clockHeight() const;
  // The two bands that change while the screen is up. Kept apart because a
  // selection move must not spend a refresh on the sky, and a satellite arriving
  // must not spend one on the rows.
  void skyBand(int& x, int& y, int& w, int& h) const;
  void actionsBand(int& x, int& y, int& w, int& h) const;
  GnssSkyView::Box skyBox() const;
  void actionRect(int index, int& x, int& y, int& w, int& h) const;
  int readoutTop() const;
  int readoutHeight() const;

  // Refreshes only the band that changed, and falls back to a whole panel when
  // the driver refuses the window (it does on this board today --
  // ../docs/t5s3-partial-refresh.md).
  void refreshBand(int x, int y, int w, int h);

  void activate(Action action);
  // Both exits from the wait. `usePhone` decides the map session's source and
  // therefore who owns the receiver on the way out.
  void openMap(bool usePhone);

  // What the panel is currently showing, so a redraw only happens when the
  // picture would actually differ. Every field is drawn text or a drawn dot --
  // nothing here is state the screen owns.
  struct Drawn {
    uint8_t inView = 0xFF;
    uint8_t heard = 0xFF;
    uint8_t bestSnr = 0xFF;
    uint8_t satellites = 0xFF;
    // Whole 5-second steps, matching the redraw floor. The clock is deliberately
    // coarse: a per-second timer on this panel would spend a 1.08 s refresh
    // every second and read as a device that is busy rather than waiting.
    uint16_t waitedSteps = 0xFFFF;
    bool receiverUp = false;
  };
  bool skyChanged() const;
  Drawn currentDrawn() const;

  char routePath_[MapRouteSource::kMaxPathLen] = {};
  // True when this screen powered the receiver and therefore owns it.
  bool gnssStartedHere_ = false;
  // The rail did not come up (the I2C expander behind it did not answer). The
  // screen stays and says so: the two ways out are still the rider's to pick,
  // and a black panel with no explanation is the state this replaces.
  bool startFailed_ = false;
  uint32_t enteredMs_ = 0;
  uint32_t lastRedrawMs_ = 0;
  Drawn drawn_;
  Action selected_ = Action::OpenMap;
};

#endif  // ENABLE_GNSS_CMD
