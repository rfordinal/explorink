#include "HeapProbe.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace {

// Every block carries {size, epoch} in front of the pointer handed out.
// The epoch is what makes reset() safe: a block allocated before the reset
// frees after it, and without the epoch its size would be subtracted from a
// counter that never counted it.
struct BlockHeader {
  size_t size;
  size_t epoch;
};

constexpr size_t kMinHeader = 16;  // >= alignof(max_align_t), keeps payload aligned
static_assert(sizeof(BlockHeader) <= kMinHeader, "header must fit the alignment padding");

size_t g_epoch = 1;
size_t g_live = 0;
size_t g_peak = 0;
size_t g_allocs = 0;

void* allocate(size_t bytes, size_t headerLen) {
  // headerLen is also the payload's offset, so when it equals the requested
  // over-alignment an aligned_alloc'd base keeps the payload aligned too.
  // malloc's own 16-byte guarantee covers the ordinary kMinHeader case.
  void* raw;
  if (headerLen > kMinHeader) {
    const size_t total = bytes + headerLen;
    raw = std::aligned_alloc(headerLen, ((total + headerLen - 1) / headerLen) * headerLen);
  } else {
    raw = std::malloc(bytes + headerLen);
  }
  if (!raw) return nullptr;
  BlockHeader header{bytes, g_epoch};
  std::memcpy(static_cast<char*>(raw) + headerLen - sizeof(BlockHeader), &header, sizeof(header));

  g_live += bytes;
  ++g_allocs;
  if (g_live > g_peak) g_peak = g_live;
  return static_cast<char*>(raw) + headerLen;
}

void release(void* ptr, size_t headerLen) {
  if (!ptr) return;
  char* raw = static_cast<char*>(ptr) - headerLen;
  BlockHeader header{};
  std::memcpy(&header, raw + headerLen - sizeof(BlockHeader), sizeof(header));
  if (header.epoch == g_epoch) {
    g_live = header.size <= g_live ? g_live - header.size : 0;
  }
  std::free(raw);
}

size_t headerFor(std::align_val_t alignment) {
  const size_t align = static_cast<size_t>(alignment);
  return align > kMinHeader ? align : kMinHeader;
}

}  // namespace

void* operator new(size_t bytes) {
  void* p = allocate(bytes, kMinHeader);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void* operator new(size_t bytes, const std::nothrow_t&) noexcept { return allocate(bytes, kMinHeader); }
void* operator new[](size_t bytes, const std::nothrow_t&) noexcept { return allocate(bytes, kMinHeader); }

void* operator new(size_t bytes, std::align_val_t alignment) {
  void* p = allocate(bytes, headerFor(alignment));
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t bytes, std::align_val_t alignment) { return ::operator new(bytes, alignment); }

void operator delete(void* ptr) noexcept { release(ptr, kMinHeader); }
void operator delete[](void* ptr) noexcept { release(ptr, kMinHeader); }
void operator delete(void* ptr, size_t) noexcept { release(ptr, kMinHeader); }
void operator delete[](void* ptr, size_t) noexcept { release(ptr, kMinHeader); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { release(ptr, kMinHeader); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { release(ptr, kMinHeader); }

void operator delete(void* ptr, std::align_val_t alignment) noexcept { release(ptr, headerFor(alignment)); }
void operator delete[](void* ptr, std::align_val_t alignment) noexcept { release(ptr, headerFor(alignment)); }
void operator delete(void* ptr, size_t, std::align_val_t alignment) noexcept { release(ptr, headerFor(alignment)); }
void operator delete[](void* ptr, size_t, std::align_val_t alignment) noexcept { release(ptr, headerFor(alignment)); }

namespace HeapProbe {

void reset() {
  ++g_epoch;
  g_live = 0;
  g_peak = 0;
  g_allocs = 0;
}

size_t liveBytes() { return g_live; }
size_t peakBytes() { return g_peak; }
size_t allocCount() { return g_allocs; }

}  // namespace HeapProbe
