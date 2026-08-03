#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

#include "IFileSource.h"

// IFileSource over the SD card. The device counterpart to
// test/map_preview/StdioFileSource, and the only file-touching code the map
// path adds to the firmware.
//
// Everything goes through HalFile, never SdFat / SdSpiCard / FsFile /
// SDCardManager. Those bypass HalStorage's storageMutex, and two tasks in
// SdFat at once can leave SdSpiCard's unsynchronised m_spiActive confused
// enough that one task ends a transaction against a lock the other holds --
// which trips FreeRTOS's xTaskPriorityDisinherit assert and panics the
// board. See the firmware CLAUDE.md, "SdFat is not thread-safe".
//
// `file_` is a member, so DESTRUCTOR_CLOSES_FILE=1 does not release it at
// the point the tile reader needs it released -- close() is explicit, and is
// called before every reopen and from the destructor.
//
// No multi-byte field is decoded here. This hands raw bytes to
// MapTileReader, which memcpy's every field out of them. Keep it that way:
// a cast-and-dereference of a wider type out of this buffer works on x86 and
// faults on the ESP32-C3, with a trace that points somewhere else.
class HalFileSource : public IFileSource {
 public:
  ~HalFileSource() override;

  bool open(const char* path) override;
  int read(void* buf, size_t len) override;
  bool seek(uint32_t offset) override;
  void close() override;

  bool isOpen() const { return open_; }

 private:
  HalFile file_;
  bool open_ = false;
};
