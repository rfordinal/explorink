#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>

#include <cstddef>
#include <cstdint>

// FreeInk GNSS: an NMEA 0183 receiver on a UART.
//
// Board-agnostic on purpose. The pins, the baud rate and the receiver's power
// rail are all injected through GnssConfig, because no two boards agree on any
// of them. The LilyGo T5 S3 Pro, for one, gates its L76K behind a PCA9535
// expander pin shared with the LoRa radio; that is board support and not
// something this library can know about.
//
// The library never logs, and poll() never blocks -- it consumes whatever the
// UART already has and returns immediately, so the caller decides what to
// print, when to ask, and how to report it. begin() is the one exception: it
// blocks for GnssConfig::powerSettleMs while the receiver's regulator comes up.
// Nothing here allocates after begin().

// One position solution, as last reported by the receiver.
struct GnssFix {
  // False until the receiver has reported a usable solution at least once.
  // Once true it stays true and the fields keep the last good values, so a
  // caller can tell "never had a fix" from "had one, lost it" by also reading
  // Gnss::fixAgeMs().
  bool valid = false;
  // GGA field 6: 0 = no fix, 1 = GNSS, 2 = differential, 4/5 = RTK,
  // 6 = estimated. Note that 6 is dead reckoning, with no satellites behind it,
  // and `valid` is set for it like any other non-zero quality -- a caller that
  // must not act on a dead-reckoned position has to check this field. Untested:
  // this receiver has not been seen to emit 6.
  uint8_t quality = 0;
  // Satellites used in the solution (GGA field 7), not satellites in view.
  uint8_t satsUsed = 0;
  double latitude = 0.0;   // decimal degrees, north positive
  double longitude = 0.0;  // decimal degrees, east positive
  float altitudeMeters = 0.0f;
  float hdop = 0.0f;
  float speedKmh = 0.0f;
  float courseDegrees = 0.0f;
  // Unix seconds, zero until known. Taken only from sentences that carry date
  // and time TOGETHER (RMC, and ZDA where the receiver sends it), never
  // stitched across two sentences -- see parseGga's comment for the midnight
  // bug that cost.
  uint32_t utc = 0;
};

// Raw sentence sink: every checksum-valid sentence, with its "*hh" checksum and
// without the leading '$' or the trailing CRLF. Used for bring-up passthrough.
// Called from poll(), on the caller's task.
//
// `sentence[length]` is guaranteed to be '\0', so a sink may treat it as a
// C string and ignore `length`. Stated because a caller already relies on it.
using GnssRawSink = void (*)(const char* sentence, size_t length);

struct GnssConfig {
  // Not owned. Must outlive the Gnss instance.
  HardwareSerial* serial = nullptr;
  // MCU-side pins: rxPin is where this MCU receives, so it goes to the
  // receiver's TX. The two are trivially easy to swap and the symptom is
  // identical to a dead receiver, so a bring-up should try both.
  int8_t rxPin = -1;
  int8_t txPin = -1;
  uint32_t baud = 9600;
  // Optional. Called with true from begin() and false from end(). Return false
  // and begin() fails without touching the UART.
  bool (*powerEnable)(bool on) = nullptr;
  // Settling time between powerEnable(true) and the first UART read. A cold
  // receiver needs its regulator up before it says anything.
  uint16_t powerSettleMs = 100;
};

class Gnss {
 public:
  // Calling begin() while already running is a no-op that returns true, and in
  // particular does NOT adopt the new config or reset the counters. That is
  // deliberate -- a second begin() must not drop the rail and throw away a fix
  // the caller already has -- but it means a caller checking uptimeMs() or
  // timeToFirstFixMs() after one may be reading the first session's numbers.
  bool begin(const GnssConfig& config);
  void end();
  bool running() const { return running_; }

  // Consume every byte the UART has buffered and parse what completes. Returns
  // true if this call changed anything in fix() -- position, quality, speed,
  // course or the clock, not position alone. Cheap to call every loop.
  //
  // It must actually BE called every loop: the driver's RX buffer is 256 bytes
  // by default (framework-arduinoespressif32 HardwareSerial.cpp:148) and this
  // receiver sends about 816 bytes a second, measured, so anything that blocks
  // the caller for more than ~0.3 s loses sentences. A 6.07 s blocking screen
  // render cost 85 of them on real hardware.
  bool poll();

  const GnssFix& fix() const { return fix_; }
  // Milliseconds since the fix last changed, or 0 if there has never been one.
  uint32_t fixAgeMs() const;
  // Milliseconds from begin() to the first valid fix. Zero while there is none.
  uint32_t timeToFirstFixMs() const { return ttffMs_; }
  uint32_t uptimeMs() const;

  // Satellites the receiver can see, summed across constellations (GSV field
  // 3 per talker). Always at least satsUsed, usually more.
  uint8_t satsInView() const;
  // Satellites reporting a non-zero carrier-to-noise ratio, and the best one,
  // both in dB-Hz. This is the pair that separates "sees sky" from "sees
  // ceiling": indoors the count collapses and the best value sits in the teens.
  uint8_t satsWithSignal() const;
  uint8_t bestSnr() const;

  uint32_t sentencesParsed() const { return sentences_; }
  // Sentences whose checksum did not match: the baud-rate and line-quality
  // signal. Distinct from framingErrors(), which counts input that had no
  // usable "*hh" at all -- a garbage burst on a cold UART, or a line lost to
  // buffer overflow. Mixing the two makes the first useless as a diagnosis.
  uint32_t checksumErrors() const { return checksumErrors_; }
  uint32_t framingErrors() const { return framingErrors_; }
  uint32_t bytesRead() const { return bytesRead_; }

  // Pass nullptr to stop.
  void setRawSink(GnssRawSink sink) { rawSink_ = sink; }

 private:
  // NMEA caps a sentence at 82 characters including the delimiters; the extra
  // room absorbs a receiver that ignores that and keeps the parser from
  // silently truncating mid-field.
  static constexpr size_t kLineMax = 120;
  // GPS, GLONASS, Galileo, BeiDou, QZSS, plus a combined talker.
  static constexpr uint8_t kMaxTalkers = 6;

  struct TalkerState {
    char id[2] = {0, 0};
    uint8_t inView = 0;        // committed GSV field 3
    uint8_t snrCount = 0;      // committed count of sats with snr > 0
    uint8_t snrBest = 0;       // committed best snr
    uint8_t pendingCount = 0;  // accumulating over the current GSV cycle
    uint8_t pendingBest = 0;
    uint8_t expectedNext = 1;  // next message number this cycle should carry
    bool cycleIntact = true;   // false once a message of this cycle went missing
  };

  void handleSentence(char* line, size_t length);
  void parseGga(const char* body);
  void parseRmc(const char* body);
  void parseGsv(const char* talker, const char* body);
  void parseZda(const char* body);
  TalkerState* talkerFor(const char* id);

  GnssConfig config_;
  bool running_ = false;

  char line_[kLineMax] = {0};
  size_t lineLength_ = 0;
  bool lineOverflowed_ = false;

  GnssFix fix_;
  TalkerState talkers_[kMaxTalkers];

  uint32_t beginMs_ = 0;
  uint32_t lastFixMs_ = 0;
  uint32_t ttffMs_ = 0;

  uint32_t sentences_ = 0;
  uint32_t checksumErrors_ = 0;
  uint32_t framingErrors_ = 0;
  bool fixDirty_ = false;
  uint32_t bytesRead_ = 0;

  GnssRawSink rawSink_ = nullptr;
};
