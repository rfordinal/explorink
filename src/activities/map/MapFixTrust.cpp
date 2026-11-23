#include "MapFixTrust.h"

namespace MapFixTrust {

Pos posTrustFor(uint16_t accuracyM, State& state) {
  if (accuracyM == kAccuracyUnstated) {
    // Does not touch the latch. A source that reports accuracy on some fixes
    // and omits it on others would otherwise flip the ring between broken and
    // whole on the gaps alone, which is a repaint per fix and says nothing
    // true.
    return Pos::Unstated;
  }
  if (accuracyM >= kLooseAtOrAboveM) {
    state.pos = Pos::Loose;
  } else if (accuracyM <= kTrustedAtOrBelowM) {
    state.pos = Pos::Trusted;
  } else if (state.pos == Pos::Unstated) {
    // First stated fix landing inside the dead band has no previous side to
    // keep. Trusted, not Loose: the band's whole width is under the marker at
    // the finest rung, so the ring is not lying, and starting Loose would make
    // the alarm the default state.
    state.pos = Pos::Trusted;
  }
  // Inside the band with a side already chosen, keep it -- that is the
  // hysteresis.
  return state.pos;
}

Pos posTrustForHdop(float hdop, uint8_t satsUsed, State& state) {
  // `!(hdop > 0)` rather than `hdop <= 0`, so a NaN lands here too instead of
  // falling through every comparison below and leaving the latch untouched by
  // accident rather than on purpose.
  if (!(hdop > 0.0f)) {
    return Pos::Unstated;
  }
  if (satsUsed < kMinSatsForTrust) {
    // Latches, unlike Unstated: a 2D fix is a stated bad fix, not a missing
    // figure.
    state.pos = Pos::Loose;
    return state.pos;
  }
  if (hdop >= kLooseAtOrAboveHdop) {
    state.pos = Pos::Loose;
  } else if (hdop <= kTrustedAtOrBelowHdop) {
    state.pos = Pos::Trusted;
  } else if (state.pos == Pos::Unstated) {
    // Same call as the metre side makes, for the same reason: the first stated
    // fix inside the dead band has no previous side to keep, and starting Loose
    // would make the alarm the default.
    state.pos = Pos::Trusted;
  }
  return state.pos;
}

Dir dirTrustFromWireCode(uint8_t code) {
  switch (code & 0x03) {
    case 1:
      return Dir::Good;
    case 2:
      return Dir::Coarse;
    case 3:
      return Dir::Unknown;
    default:
      return Dir::Unstated;
  }
}

uint8_t wireCodeForDirTrust(Dir dir) {
  switch (dir) {
    case Dir::Good:
      return 1;
    case Dir::Coarse:
      return 2;
    case Dir::Unknown:
      return 3;
    case Dir::Unstated:
    default:
      return 0;
  }
}

MarkerStyle styleFor(Trust trust) {
  MarkerStyle style{};
  // Unstated draws whole, same as Trusted. The two are different facts and the
  // same picture on purpose: "I believe this" and "nobody told me" both mean
  // the marker has no reason to raise an alarm, and an alarm nobody can act on
  // is worse than none.
  style.ringBroken = trust.pos == Pos::Loose;
  switch (trust.dir) {
    case Dir::Coarse:
      style.head = MarkerStyle::Head::Wedge;
      break;
    case Dir::Unknown:
      style.head = MarkerStyle::Head::None;
      break;
    case Dir::Good:
    case Dir::Unstated:
    default:
      style.head = MarkerStyle::Head::Glyph;
      break;
  }
  return style;
}

}  // namespace MapFixTrust
