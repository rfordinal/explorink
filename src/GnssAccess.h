#pragma once

// The one seam between the firmware's GNSS receiver and anything that wants a
// position from it. There is no interface and no base class here on purpose:
// MapActivity::applyFix() is already the transport-agnostic funnel every
// position source ends in (BLE, the serial console, and now this), so a source
// only needs to be able to reach the driver and to turn it on.
//
// Gated on ENABLE_GNSS_CMD, which is a build flag whose NAME is now narrower
// than its meaning. It was added for the CMD:GNSS bring-up console and today it
// says "this build has a receiver": it is set in env:t5s3pro and in no other
// env, because no other board here has one. Splitting it into a capability flag
// plus a console flag is a rename job for the day a release env wants GNSS --
// until then two flags would both be set in exactly one place, which buys
// nothing.

#ifdef ENABLE_GNSS_CMD

#include <Gnss.h>

// The single receiver instance, defined in main.cpp. main.cpp's loop() is what
// calls poll() on it, every iteration; a reader here only ever reads fix().
extern Gnss gnss;

// Powers the receiver's rail and opens the UART with this board's pins, baud
// and RX ring. Returns false when the rail or the I2C expander behind it did
// not answer, and true when the receiver is already running -- Gnss::begin()
// treats a second call as a no-op, deliberately, so a caller that cannot know
// who else started it can just ask.
//
// The rail is shared with the LoRa radio and the enable behind it is a PCA9535
// pin; both of those live in main.cpp's gnssPowerEnable(), which is where the
// board knowledge belongs. Nothing above this line has to know about either.
bool gnssStart();

#endif  // ENABLE_GNSS_CMD
