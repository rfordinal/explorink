#pragma once

#ifdef ENABLE_GNSS_CMD

#include <Gnss.h>

#include <cstdint>

// A synthetic sky, so the satellite wait screen can be judged without weather.
//
// ## Why this exists
//
// The screen's whole subject is satellites on a sky, and after five hardware
// passes on 2026-09-10 **not one mark had ever been drawn**: indoors the
// receiver heard one or two and located none, and the day it was taken outside
// the weather gave nothing stable either. So the plot's placement, its mark
// sizes, the halo over the ridge and whether a dozen marks read at all were all
// unverifiable by waiting.
//
// This is the same answer the tile grid got for the same problem
// (`MapActivity`'s `fake <missing> <held>` console command, and
// `tools/tile_grid_shot.py` in the parent repo): a bench instrument that puts a
// populated screen on the panel deterministically, so the drawing can be looked
// at and two runs can be compared.
//
// ## What it is not
//
// **Not a fake fix.** It feeds the sky plot and the readout's counts only; it
// never produces a position, never touches `Gnss`, and the map still gets a
// position from a real receiver or a real phone. So a screenshot taken with
// this on shows exactly what the wait screen draws and nothing about the map.
//
// **Not a simulator of the receiver.** The numbers are chosen to exercise the
// drawing -- one satellite per bucket, one behind the ridge, one heard but not
// located -- not to look like any particular sky.
//
// ## Devel only
//
// Behind ENABLE_GNSS_CMD, which is set in env:t5s3pro and in no release env
// (`GnssAccess.h`). It draws a screen that claims satellites the device cannot
// see, which is a lie a shipped build must not be able to tell -- and the same
// gate already covers `CMD:GNSS`, whose reply is the rider's own position.
class GnssFakeSky {
 public:
  static GnssFakeSky& getInstance() {
    static GnssFakeSky instance;
    return instance;
  }

  static constexpr uint8_t kMaxSatellites = 16;

  bool enabled() const { return enabled_; }

  // Fills the sky with `count` satellites, capped at kMaxSatellites. Zero
  // disables, so `CMD:GNSS SKY 0` and `CMD:GNSS SKY OFF` are the same thing.
  void enable(uint8_t count) {
    if (count == 0) {
      disable();
      return;
    }
    count_ = count > kMaxSatellites ? kMaxSatellites : count;
    build();
    enabled_ = true;
  }

  void disable() {
    enabled_ = false;
    count_ = 0;
  }

  uint8_t count() const { return count_; }
  const GnssSatellite& satellite(uint8_t index) const { return satellites_[index]; }

  uint8_t satsInView() const { return count_; }
  uint8_t satsWithSignal() const {
    uint8_t heard = 0;
    for (uint8_t i = 0; i < count_; ++i) {
      if (satellites_[i].snr > 0) ++heard;
    }
    return heard;
  }
  uint8_t bestSnr() const {
    uint8_t best = 0;
    for (uint8_t i = 0; i < count_; ++i) {
      if (satellites_[i].snr > best) best = satellites_[i].snr;
    }
    return best;
  }

 private:
  GnssFakeSky() = default;

  // Deterministic, and every entry is there to make the drawing show something
  // it otherwise could not:
  //
  //   - the elevations run from 3 to 80 degrees, so the plot's whole vertical
  //     range is used and the low ones land inside the ridge -- which is where
  //     the white halo either works or does not;
  //   - the C/N0 values cross all four calibrated rungs and include zeros, so
  //     every mark size appears next to an outline (GnssSkyView::snrBucket);
  //   - two satellites carry no position at all, which is the state that puts
  //     "N not located yet" on the readout and leaves a count with no mark.
  //
  // Azimuths are spread evenly and offset, so nothing lands exactly on a
  // cardinal tick -- a mark hiding under the N label would be the one case a
  // screenshot could not tell from a missing mark.
  void build() {
    static constexpr uint8_t kElevation[kMaxSatellites] = {70, 15, 45, 5, 60, 25, 80, 8, 35, 50, 12, 65, 20, 40, 3, 55};
    static constexpr uint8_t kSnr[kMaxSatellites] = {41, 0, 33, 22, 37, 0, 45, 18, 28, 31, 12, 39, 24, 26, 0, 35};
    static constexpr char kTalkers[4][2] = {{'G', 'P'}, {'G', 'L'}, {'G', 'B'}, {'G', 'A'}};

    for (uint8_t i = 0; i < count_; ++i) {
      GnssSatellite& sat = satellites_[i];
      sat.talker[0] = kTalkers[i % 4][0];
      sat.talker[1] = kTalkers[i % 4][1];
      sat.prn = static_cast<uint8_t>(i + 1);
      sat.snr = kSnr[i];
      // Two of them heard and unplaced. `hasPosition` false means the receiver
      // has not located it, so elevation and azimuth must stay zero -- a caller
      // that trusted them would draw due north on the horizon.
      const bool located = !(i == 3 || i == 10);
      sat.hasPosition = located;
      sat.elevation = located ? kElevation[i] : 0;
      sat.azimuth = located ? static_cast<uint16_t>((i * 360 / count_ + 17) % 360) : 0;
    }
  }

  GnssSatellite satellites_[kMaxSatellites];
  uint8_t count_ = 0;
  bool enabled_ = false;
};

#define FAKE_SKY GnssFakeSky::getInstance()

#endif  // ENABLE_GNSS_CMD
