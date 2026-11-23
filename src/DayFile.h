#pragma once

#include <cstddef>
#include <cstdint>

// Naming a log file after the day it covers, for every log that rotates by
// day rather than by size.
//
// **Extracted 2026-10-23, when the LoRa event log became the second caller.**
// The calendar arithmetic lived inside `GroupRecord.cpp`'s anonymous namespace
// and was reachable only from the group layer, which is behind its own build
// flag -- so the radio's log could not have used it without either compiling
// the group layer or keeping a second copy of a date algorithm. Two copies of
// a calendar is exactly the shape that drifts.
//
// Integer only and no `<ctime>`: `gmtime_r` on this toolchain pulls in locale
// code, and these have to stay usable from a pure translation unit the host
// tests compile on their own.

// Days since the unix epoch.
uint32_t dayNumberFromUtc(uint32_t utc);

// Civil date from days since the epoch (Howard Hinnant's algorithm).
void civilFromDays(uint32_t days, uint32_t& year, uint32_t& month, uint32_t& day);

// `<prefix>-YYYYMMDD.csv`, or `<prefix>-noclock.csv` when `utc` is 0.
//
// **A device with no clock still gets a file**, and it gets one that rotation
// will never delete: a day that cannot be named cannot be judged old. Every
// Xteink board is in that state until a phone hands it a time.
//
// False when the buffer is too small for the longest name a prefix can make.
bool dayFileName(const char* prefix, uint32_t utc, char* out, size_t outBytes);
