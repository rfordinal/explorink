#pragma once

#include <cstddef>
#include <cstdint>

// Minimal file abstraction MapTileReader needs -- open/read/seek/close,
// nothing else. Nothing in the tile reader may touch a file except through
// this interface. Native implementation is StdioFileSource
// (test/map_preview/StdioFileSource.h), over plain stdio. The device
// implementation (HalFileSource, over HalFile) lands in P4 -- see
// docs/prototype-plan.md. No HalStorage dependency belongs here yet.
class IFileSource {
 public:
  virtual ~IFileSource() = default;

  virtual bool open(const char* path) = 0;

  // Reads up to len bytes. Returns the number of bytes actually read, 0 at
  // EOF, or a negative value on error.
  virtual int read(void* buf, size_t len) = 0;

  // Absolute seek from the start of the file.
  virtual bool seek(uint32_t offset) = 0;

  virtual void close() = 0;
};
