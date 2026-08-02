#pragma once

#include <cstdio>

#include "IFileSource.h"

// Native-only IFileSource implementation over plain stdio -- used by
// test/map_preview and the map tile reader tests. The device implementation
// (HalFileSource, over HalFile) lands in P4; this file never ships to the
// firmware build.
class StdioFileSource : public IFileSource {
 public:
  ~StdioFileSource() override { close(); }

  bool open(const char* path) override {
    close();
    file_ = std::fopen(path, "rb");
    return file_ != nullptr;
  }

  int read(void* buf, size_t len) override {
    if (!file_) return -1;
    const size_t n = std::fread(buf, 1, len, file_);
    if (n < len && std::ferror(file_)) return -1;
    return static_cast<int>(n);
  }

  bool seek(uint32_t offset) override {
    if (!file_) return false;
    return std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0;
  }

  void close() override {
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

 private:
  std::FILE* file_ = nullptr;
};
