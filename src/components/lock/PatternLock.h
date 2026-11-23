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
// A start cell plus 3 to 7 right-angle steps, so 4 to 8 points counting the start.
// Diagonals do not exist: the hardware has four direction buttons and each press
// moves one cell. A cell may be stepped on twice -- right-then-left comes back,
// and forbidding that would both surprise a rider and cut the space further.
//
// ## How large the space actually is (measured, not estimated)
//
// Distinct paths of 4 to 8 points on a 4x4 grid, counted by walk enumeration:
//
//   start in a corner        2,810   (~11.5 bits)
//   start on an edge         4,545
//   start in the middle four 7,350   (~12.8 bits)
//
// So the default start is an interior cell: the first step branches four ways
// there instead of two, and that alone is 2.6x the space of a corner start for
// free. **The pattern is still not the strength** -- it is comparable to a
// 4-digit decimal PIN. What makes a wrap hard to attack is the device secret it
// is mixed with, and what makes guessing impractical is the caller's rate
// limiter (../../../docs/pattern-lock.md, "How large the space is").
//
// For contrast, the wallet's dot PIN (WalletUnlockActivity) is 4^6 to 4^10
// because every press is a free choice there -- no grid wall ever refuses one.
// A grid is easier to remember and smaller. That trade is the whole point of this
// screen, and it is a trade.

#include <cstddef>
#include <cstdint>

namespace patternlock {

// Zeroes a buffer in a way a release build may not drop. A plain loop over a
// buffer that is dead on the next statement is a dead store, and the optimiser is
// entitled to remove it -- which is exactly how a secret survives its own
// zeroing. The wallet's crypto header carries the same primitive for the same
// reason; this one is local so a gate pulls in no wallet code.
inline void secureWipe(char* buffer, const size_t length) {
  volatile char* p = buffer;
  for (size_t i = 0; i < length; ++i) p[i] = 0;
}

inline constexpr uint8_t kCols = 4;
inline constexpr uint8_t kRows = 4;
inline constexpr uint8_t kCells = kCols * kRows;

inline constexpr size_t kMinPoints = 4;
inline constexpr size_t kMaxPoints = 8;

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
  explicit Entry(const uint8_t startCell = kDefaultStart) : start_(startCell < kCells ? startCell : kDefaultStart) {
    wipe();
  }

  // The only way back to an empty entry, and it goes through secureWipe -- there
  // is no non-secure reset, so no call site can pick the wrong one. The first cut
  // had a `clear()` and a `wipe()` with identical bodies and different comments,
  // which told a reader nothing about which one mattered.
  void wipe() {
    volatile uint8_t* p = path_;
    for (size_t i = 0; i < kMaxPoints; ++i) p[i] = 0;
    path_[0] = start_;
    count_ = 1;
  }

  // Appends the cell the cursor steps onto. False, and nothing changed, when the
  // step would leave the grid or the path is already full. A wall and a full path
  // are both "nothing happened", so the screen redraws for neither.
  bool step(const Step s) {
    if (count_ >= kMaxPoints) return false;
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
  bool longEnough() const { return count_ >= kMinPoints; }
  bool full() const { return count_ >= kMaxPoints; }
  uint8_t startCell() const { return start_; }

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
// `startCell`**: right length, legal cells, starts where the cursor starts, every
// consecutive pair one orthogonal step apart.
//
// The start cell is a parameter and not optional on purpose. Without it this
// function certified patterns nobody could ever enter -- store "0145", open the
// gate on the default start of 5, and every path a rider can produce begins '5'
// while the stored one begins '0'. That is a permanent lockout, and it is the
// exact failure this check exists to prevent (found in review, 2026-11-23).
inline bool isWellFormed(const char* path, const uint8_t startCell) {
  if (path == nullptr) return false;
  size_t n = 0;
  while (path[n] != '\0') {
    if (++n > kMaxPoints) return false;
  }
  if (n < kMinPoints) return false;
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
