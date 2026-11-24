#pragma once

// Grid pattern lock, the arithmetic half: a cursor walking a 4x4 grid in right
// angles, and the path it leaves behind.
//
// ## Why this is a header with no I/O in it
//
// No Arduino, no renderer, no stored secret. `PatternLockActivity` draws it and
// the caller verifies the path it produces. The split is what lets a host test
// walk every legal path in a millisecond, and it keeps the stored secret out of a
// component that every screen can include.
//
// ## What a pattern is
//
// A start cell plus right-angle steps, one cell per press. Diagonals do not
// exist: the hardware has four direction buttons. A cell may be stepped on twice
// -- right-then-left comes back, and forbidding that would both surprise a rider
// and cut the space further.
//
// **How many points is the caller's choice**, `Config::minPoints` /
// `Config::maxPoints`, because the two callers want different things from the
// same screen. A screen-lock wants a short gesture; the wallet's gate derives a
// key from the path and wants the bits (see the table). `kMaxPoints` here is only
// the buffer bound.
//
// ## How large the space actually is (measured, not estimated)
//
// Distinct paths on a 4x4 grid, counted by walk enumeration
// (`scripts/pattern_space.py`). Each extra point multiplies the space by about
// 3.24 -- four directions less the walls -- so it is worth **+1.7 bits**:
//
//   points   corner      edge        interior
//   4 to 8    2,810      4,545         7,350   (12.8 bits)
//   4 to 12 308,834    499,701       808,526   (19.6 bits)
//   8 to 12 307,970    498,305       806,270   (19.6 bits)
//
// So the default start is an interior cell: the first step branches four ways
// there instead of two, and that alone is 2.6x the space of a corner start for
// free. **The pattern is still not the strength.** What makes a wrap hard to
// attack is the device secret it is mixed with, and what makes guessing
// impractical is the caller's rate limiter
// (../../../docs/pattern-lock.md, "How large the space is").
//
// The floor matters more than the total, because an attacker searches the
// shortest patterns first, and a 4-point path is 46 walks -- 5.5 bits. That is
// why the wallet asks for 8 to 12 (12.3 bits at its own floor) rather than
// accepting this file's absolute minimum: it replaced a 6-to-10-symbol dot PIN
// of 12.0 to 20.0 bits and must not quietly lose six of them
// (../../../docs/wallet-crypto.md, "What the pattern is for").

#include <cstddef>
#include <cstdint>

namespace patternlock {

// Zeroes a buffer in a way a release build may not drop. A plain loop over a
// buffer that is dead on the next statement is a dead store, and the optimiser is
// entitled to remove it -- which is exactly how a secret survives its own
// zeroing. The wallet's crypto header carries the same primitive for the same
// reason; this one is local so a gate pulls in no wallet code.
template <typename Byte>
inline void secureWipe(Byte* buffer, const size_t length) {
  volatile Byte* p = buffer;
  for (size_t i = 0; i < length; ++i) p[i] = 0;
}

inline constexpr uint8_t kCols = 4;
inline constexpr uint8_t kRows = 4;
inline constexpr uint8_t kCells = kCols * kRows;

// The absolute bounds any caller may ask for. `kMaxPoints` sizes every path
// buffer in the tree, so raising it costs RAM in `Entry` and in every caller that
// declares a `char[kPathBufBytes]`; a caller that wants fewer says so in its
// Config rather than by editing this.
inline constexpr size_t kMinPoints = 4;
inline constexpr size_t kMaxPoints = 12;

// One char per point plus the terminator.
inline constexpr size_t kPathBufBytes = kMaxPoints + 1;

// Row 1, column 1 of the 4x4. Interior, for the 2.6x above.
inline constexpr uint8_t kDefaultStart = kCols + 1;

enum class Step : uint8_t { Left, Right, Up, Down };

// Why a fixed start at all: it is the one point no button press produces. Leave it
// to chance -- the cell the cursor happened to rest on last time -- and the same
// four presses mean two different patterns on two days.
class Entry {
 public:
  // `minPoints` / `maxPoints` are the caller's range, clamped into this file's
  // absolute bounds. They are runtime rather than template parameters because the
  // path buffer is sized by kMaxPoints either way, and a second instantiation of
  // the whole class to save four bytes would be a poor trade on a device with one
  // gate screen on the stack at a time.
  explicit Entry(const uint8_t startCell = kDefaultStart, const size_t minPoints = kMinPoints,
                 const size_t maxPoints = kMaxPoints)
      : start_(startCell < kCells ? startCell : kDefaultStart),
        min_(minPoints < kMinPoints ? kMinPoints : (minPoints > kMaxPoints ? kMaxPoints : minPoints)),
        max_(maxPoints > kMaxPoints ? kMaxPoints : (maxPoints < kMinPoints ? kMinPoints : maxPoints)) {
    // A caller that swaps the two would otherwise get a gate whose CONFIRM is
    // never armed: longEnough() could not be reached before full() refuses the
    // next step. Silently ordering them is the one repair that cannot lock
    // anybody out.
    if (min_ > max_) min_ = max_;
    wipe();
  }

  // The only way back to an empty entry, and it goes through secureWipe -- there
  // is no non-secure reset, so no call site can pick the wrong one. The first cut
  // had a `clear()` and a `wipe()` with identical bodies and different comments,
  // which told a reader nothing about which one mattered.
  void wipe() {
    secureWipe(path_, kMaxPoints);
    path_[0] = start_;
    count_ = 1;
  }

  // Appends the cell the cursor steps onto. False, and nothing changed, when the
  // step would leave the grid or the path is already full. A wall and a full path
  // are both "nothing happened", so the screen redraws for neither.
  bool step(const Step s) {
    if (count_ >= max_) return false;
    const uint8_t from = path_[count_ - 1];
    const uint8_t row = from / kCols;
    const uint8_t col = from % kCols;
    switch (s) {
      case Step::Left:
        if (col == 0) return false;
        path_[count_++] = static_cast<uint8_t>(from - 1);
        return true;
      case Step::Right:
        if (col + 1 >= kCols) return false;
        path_[count_++] = static_cast<uint8_t>(from + 1);
        return true;
      case Step::Up:
        if (row == 0) return false;
        path_[count_++] = static_cast<uint8_t>(from - kCols);
        return true;
      case Step::Down:
        if (row + 1 >= kRows) return false;
        path_[count_++] = static_cast<uint8_t>(from + kCols);
        return true;
    }
    return false;
  }

  // Drops the last point. False when only the start is left -- which is BACK's cue
  // to leave the screen instead of eating the press.
  bool undo() {
    if (count_ <= 1) return false;
    path_[--count_] = 0;
    return true;
  }

  uint8_t cursor() const { return path_[count_ - 1]; }
  uint8_t cursorRow() const { return static_cast<uint8_t>(cursor() / kCols); }
  uint8_t cursorCol() const { return static_cast<uint8_t>(cursor() % kCols); }
  size_t points() const { return count_; }
  bool longEnough() const { return count_ >= min_; }
  bool full() const { return count_ >= max_; }
  uint8_t startCell() const { return start_; }
  size_t minPoints() const { return min_; }
  size_t maxPoints() const { return max_; }

  // '0'..'9' then 'a'..'f' for cells 0..15, NUL terminated. One char per point, in
  // order, start included. This is the only thing that leaves the class, and it is
  // what a verifier compares -- so a stored pattern is this string, hashed or
  // wrapped by whoever stores it, never by this component.
  void encode(char out[kPathBufBytes]) const {
    for (size_t i = 0; i < count_; ++i) {
      const uint8_t c = path_[i];
      out[i] = c < 10 ? static_cast<char>('0' + c) : static_cast<char>('a' + (c - 10));
    }
    out[count_] = '\0';
  }

 private:
  uint8_t start_;
  size_t min_;
  size_t max_;
  uint8_t path_[kMaxPoints] = {0};
  size_t count_ = 1;
};

// Decodes one encoded char back to a cell index, or kCells when the char is not
// one. Needed by a caller that has to draw or validate a stored pattern.
inline uint8_t cellFromChar(const char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
  return kCells;
}

// The cell a stored pattern starts on, or kCells when the string is not one. The
// start is the one point no button produces, so a caller that stores a pattern
// has to configure the screen with this -- `Config::startCell`.
inline uint8_t startCellOf(const char* path) {
  if (path == nullptr || path[0] == '\0') return kCells;
  return cellFromChar(path[0]);
}

// True when `path` is a pattern this screen could actually have produced **from
// `startCell`, within `minPoints`..`maxPoints`**: right length, legal cells,
// starts where the cursor starts, every consecutive pair one orthogonal step
// apart.
//
// The start cell is a parameter and not optional on purpose. Without it this
// function certified patterns nobody could ever enter -- store "0145", open the
// gate on the default start of 5, and every path a rider can produce begins '5'
// while the stored one begins '0'. That is a permanent lockout, and it is the
// exact failure this check exists to prevent (found in review, 2026-11-23).
//
// The length range is a parameter for the same reason, one range later: a caller
// that opens its gate with `Config::minPoints = 8` and validates a stored pattern
// against this file's floor of 4 accepts a 5-point pattern it can then never
// enter. The two numbers must be the same numbers, so they are passed, not
// assumed.
inline bool isWellFormed(const char* path, const uint8_t startCell, const size_t minPoints = kMinPoints,
                         const size_t maxPoints = kMaxPoints) {
  if (path == nullptr) return false;
  const size_t lo = minPoints < kMinPoints ? kMinPoints : minPoints;
  const size_t hi = maxPoints > kMaxPoints ? kMaxPoints : maxPoints;
  if (lo > hi) return false;
  size_t n = 0;
  while (path[n] != '\0') {
    if (++n > hi) return false;
  }
  if (n < lo) return false;
  for (size_t i = 0; i < n; ++i) {
    if (cellFromChar(path[i]) >= kCells) return false;
  }
  if (cellFromChar(path[0]) != startCell) return false;
  for (size_t i = 1; i < n; ++i) {
    const uint8_t a = cellFromChar(path[i - 1]);
    const uint8_t b = cellFromChar(path[i]);
    const int dr = static_cast<int>(a / kCols) - static_cast<int>(b / kCols);
    const int dc = static_cast<int>(a % kCols) - static_cast<int>(b % kCols);
    if (dr * dr + dc * dc != 1) return false;
  }
  return true;
}

}  // namespace patternlock
