#pragma once

#include <cstddef>

// Native-only heap instrument. Links global operator new/delete overrides
// that track live bytes and a high-water mark, so "peak RAM during a render"
// is a measured number rather than a constant an accessor happens to return.
// Never built into the firmware -- this file lives under test/.
//
// What it counts: every C++ operator new/delete in the process, including
// std::vector and std::string. That is exactly where the old
// MapViewState-materialising pipeline spent its 40.7 KB and 1218 allocations
// on one dense z12 tile.
//
// What it does NOT count: raw libc malloc. StdioFileSource's fopen buys a
// FILE buffer that way (a few KB, one at a time, freed on close). Say so
// when quoting a number from here rather than implying it is the whole
// process footprint.
namespace HeapProbe {

// Zeroes live/peak/alloc counters. Call immediately before the region being
// measured -- allocations made before the reset are forgotten, so their
// later frees can drive `live` negative; the counter saturates at 0 instead
// of wrapping.
void reset();

size_t liveBytes();
size_t peakBytes();
size_t allocCount();

}  // namespace HeapProbe
