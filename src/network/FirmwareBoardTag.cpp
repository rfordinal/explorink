#include "FirmwareBoardTag.h"

#include <cstring>

// The board name derives from the FREEINK_DEVICE_* build flags (platformio.ini)
// so every env is tagged automatically and a new env fails to compile until it
// is named here -- the same discipline BoardConfig.h's own device-coherence
// check already applies one level down. The combined X3/X4 ESP32-C3 binary is
// one compatibility class (same pinout, selected at runtime -- see
// BoardConfig.h, "X3 and X4 are two profiles in one ESP32-C3 binary"), tagged
// "x3x4"; a wrong-chip X3/X4 image is already caught by BAD_CHIP, so this tag
// mainly guards the S3 family, where x4pro/t5s3pro/sticky all share one
// chip_id and the chip check alone cannot tell them apart.
#if FREEINK_DEVICE_X4PRO
#define EXPLORINK_BOARD_NAME "x4pro"
#elif FREEINK_DEVICE_LILYGO
#define EXPLORINK_BOARD_NAME "t5s3pro"
#elif FREEINK_DEVICE_STICKY
#define EXPLORINK_BOARD_NAME "sticky"
#elif FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
#define EXPLORINK_BOARD_NAME "x3x4"
#else
#error "FirmwareBoardTag: no known FREEINK_DEVICE_* flag set; add this env's board name here"
#endif

namespace board_tag {

namespace {
// The magic's first character must not recur inside it -- the scanner restarts
// a failed match with a single-byte lookback, which is only exact then.
constexpr size_t MAGIC_LEN = sizeof("EXPLORINK-BOARD-V1:") - 1;
}  // namespace

const char TAG[] = "EXPLORINK-BOARD-V1:" EXPLORINK_BOARD_NAME ";";

const char* boardName() { return TAG + MAGIC_LEN; }
size_t boardNameLen() { return sizeof(TAG) - 1 - MAGIC_LEN - 1; }  // strip magic and ';'

void Scanner::feed(const uint8_t* data, size_t len) {
  if (mismatchFound) return;
  for (size_t i = 0; i < len; i++) {
    const char c = static_cast<char>(data[i]);
    if (capturing) {
      if (c == ';') {
        capturing = false;
        captured[nameLen] = '\0';
        tagSeen = true;
        if (nameLen != boardNameLen() || memcmp(captured, boardName(), nameLen) != 0) {
          mismatchFound = true;
          return;
        }
        continue;
      }
      if (nameLen < MAX_NAME && c > 0x20 && c < 0x7F) {
        captured[nameLen++] = c;
        continue;
      }
      // Overlong or non-printable: a chance byte-collision, not a real tag.
      // Fall through to the magic check below instead of `continue` -- this
      // same byte can still be the start of a real tag elsewhere in the
      // image, and skipping it here used to lose that resync point.
      capturing = false;
    }
    if (c == TAG[magicMatched]) {
      if (++magicMatched == MAGIC_LEN) {
        magicMatched = 0;
        capturing = true;
        nameLen = 0;
      }
    } else {
      magicMatched = (c == TAG[0]) ? 1 : 0;
    }
  }
}

}  // namespace board_tag
