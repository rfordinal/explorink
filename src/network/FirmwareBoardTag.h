#pragma once

#include <cstddef>
#include <cstdint>

// Board-identity tag embedded in every ExplorInk image, plus a streaming
// scanner the firmware update paths use to reject an image built for a
// different board before it can boot and drive another device's pins. The
// x4pro, t5s3pro and sticky envs all target ESP32-S3 and share a chip_id, so
// the existing esp_image_header chip check alone cannot tell them apart
// (T-2008).
//
// The tag is "EXPLORINK-BOARD-V1:<board>;" stored once in .rodata -- the
// scanner's needle references the same array, so a build contains exactly
// one occurrence. Images without a tag (other projects, forks, a build from
// before this existed) are allowed through: the guard only rejects a tag
// naming a DIFFERENT board, matching validateImageFile()'s existing
// structure-only checks, which also accept anything that merely looks like a
// valid ESP32 image.
//
// Design follows upstream CrossPoint's FirmwareBoardTag (read at
// upstream/develop, not our stale pin) -- same mechanism, reimplemented
// against our own board list and branding rather than ported verbatim.

namespace board_tag {

// Full tag: magic prefix + board name + ';'.
extern const char TAG[];

// Board name of the running firmware (pointer into TAG; not null-terminated
// at the name boundary -- always pair with boardNameLen()).
const char* boardName();
size_t boardNameLen();

// Incremental scanner: feed every byte of a candidate image in stream order,
// then check mismatch(). State persists across feed() calls, so chunk
// boundaries splitting the tag are handled.
class Scanner {
 public:
  void feed(const uint8_t* data, size_t len);
  // True once a tag naming a different board has been seen. Valid mid-stream:
  // callers may abort a download as soon as this turns true.
  bool mismatch() const { return mismatchFound; }
  // Board name from the offending tag, for logging (empty until mismatch()).
  const char* foundName() const { return mismatchFound ? captured : ""; }
  // True once ANY complete tag was seen, matching or not. An untagged image
  // (every build before this existed) is indistinguishable from a wrong-board
  // image on a chip multiple boards share -- callers on such a chip should
  // treat "no tag at all" as unproven, not as "safe by default". See
  // FirmwareFlasher::validateImageFile(), the S3-family check.
  bool sawAnyTag() const { return tagSeen; }

 private:
  static constexpr size_t MAX_NAME = 23;
  char captured[MAX_NAME + 1] = {0};
  size_t nameLen = 0;
  size_t magicMatched = 0;
  bool capturing = false;
  bool mismatchFound = false;
  bool tagSeen = false;
};

}  // namespace board_tag
