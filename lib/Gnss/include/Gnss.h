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
  //
  // On a board where the receiver shares a power rail with something else, this
  // hook is where that gets handled, and it can be the most delicate part of
  // the whole integration -- see the LilyGo T5 S3 Pro case in the firmware's
  // docs/gnss.md, where the same rail powers a LoRa radio whose chip select the
  // panel bus drives. The library deliberately knows none of that.
  bool (*powerEnable)(bool on) = nullptr;
  // Settling time between powerEnable(true) and the first UART read. A cold
  // receiver needs its regulator up before it says anything.
  uint16_t powerSettleMs = 100;
  // RX ring buffer, requested before the UART opens. The Arduino default is 256
  // bytes, which a receiver sending ~800 B/s fills in a third of a second --
  // less than one blocking screen refresh on an e-ink device. 1 KB buys about
  // 1.2 s of caller inattention. It does not fix a multi-second block; nothing
  // sized in kilobytes does, and that wants the UART on its own task.
  uint16_t rxBufferBytes = 1024;
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
  //
  // READ THE NEXT PARAGRAPH BEFORE USING THIS NUMBER. It is not a
  // time-to-first-fix in the sense the name suggests, and two independent code
  // reviewers read it wrongly in the same way (2026-08-31), as did the author.
  //
  // The zero point is the UART open, after powerEnable() and the settle delay.
  // If the receiver was ALREADY POWERED AND TRACKING when begin() ran -- which
  // is the normal case on a board that powers it from a shared rail, or after
  // any second begin() -- then the first GGA to arrive already carries a fix,
  // and this measures nothing but the phase between our UART open and the next
  // sentence in the receiver's 1 Hz cycle. That is a uniform draw over roughly
  // 0 to 1000 ms.
  //
  // So: any value under about 1.2 s means "the receiver was already tracking"
  // and NOTHING about acquisition. It is not a cold start, it is not a warm
  // start, and it does not say the receiver retained ephemeris. A real
  // acquisition figure needs the receiver verifiably unpowered first, and then
  // it lands in the tens of seconds.
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

  // Times poll() found the RX buffer nearly full on entry, meaning the caller
  // very likely lost bytes before this call. It exists because sentence loss is
  // otherwise INVISIBLE: whole sentences vanish inside the driver, upstream of
  // anything this class can see, and none of the counters below move. Measured
  // on hardware 2026-08-31 -- a 6.07 s blocking render lost 85 sentences and
  // moved checksumErrors() by exactly one, so 84 disappeared without a trace.
  // A non-zero value here means the numbers below are an undercount.
  uint32_t rxNearlyFullEvents() const { return rxNearlyFull_; }

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
  uint32_t rxNearlyFull_ = 0;
  size_t rxBufferBytes_ = 0;
  bool fixDirty_ = false;
  uint32_t bytesRead_ = 0;

  GnssRawSink rawSink_ = nullptr;
};
