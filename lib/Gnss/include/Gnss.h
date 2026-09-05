#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>

#include <atomic>
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
  // 1.2 s of caller inattention.
  //
  // A caller that blocks for seconds has to raise this, and can: measured on a
  // LilyGo T5 S3 Pro 2026-09-01, a map redrawing real data blocked for up to
  // 5.76 s, and 8 KB covered every zoom rung with no sentence lost. Size it as
  // worst-block x byte-rate -- the firmware's platformio.ini shows the
  // arithmetic. The default stays modest because a board with 380 KB of RAM
  // cannot spend 8 KB here and does not have to; only the board that blocks
  // pays.
  //
  // Measure the worst block against a REAL workload. The same measurement over
  // a viewport with no data said 481 ms where the loaded one said 2687 ms, and
  // sizing against the empty number would have been wrong by four times.
  //
  // A ring cannot cover an UNBOUNDED block, so a caller that cannot state its
  // worst case wants the UART on its own task instead. Gnss::ringOverflows() is
  // what says which caller you are.
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

  // Seed the receiver with where it is, and when, so it does not have to read
  // that off the sky. Call right after begin(); returns false if not running.
  //
  // `haveTime` false seeds position only, which is the normal case on this
  // firmware: a GNSS map session runs no BLE (MapActivity's bleInUse_), so
  // there is often no clock to pass. Position alone is still most of the win --
  // it tells the receiver which satellites should be overhead instead of making
  // it search all of them.
  //
  // Accuracies are hints, not promises: the defaults say "within 50 km" and
  // "within 30 s", which is what a persisted last fix and a phone clock are
  // actually worth. Claiming better than the truth makes the receiver reject
  // measurements that would have helped.
  bool injectAidIni(double latitude, double longitude, bool haveTime = false, uint32_t utcUnixSeconds = 0,
                    float posAccMeters = 50000.0f, float timeAccSeconds = 30.0f);

  // Send one NMEA sentence to the receiver. `body` is everything between the
  // '$' and the '*', e.g. "PCAS06,L"; the checksum and the CRLF are added here.
  // Returns false if not running or the write was short.
  //
  // Deliberately generic: framing a sentence is NMEA, which this library owns,
  // while knowing that `PCAS06,L` asks a CASIC receiver how many ephemerides it
  // holds is board and vendor knowledge, which it does not. The caller supplies
  // the meaning.
  //
  // Replies arrive as ordinary sentences. This parser ignores talkers it does
  // not know, so a `$PCAS...` answer reaches the raw sink (setRawSink) and
  // nowhere else -- turn that on before asking, or the answer goes nowhere.
  bool sendNmeaSentence(const char* body);

  // Consume every byte the UART has buffered and parse what completes. Returns
  // true if this call changed anything in fix() -- position, quality, speed,
  // course or the clock, not position alone. Cheap to call every loop.
  //
  // It must actually BE called every loop, and the RX ring is what forgives a
  // caller that is not. The driver's default is 256 bytes
  // (framework-arduinoespressif32 3.3.7, HardwareSerial.cpp:148) against a
  // receiver sending about 800 bytes a second, measured, so at the default
  // anything that blocks for more than ~0.3 s loses sentences: a 6.07 s
  // blocking screen render cost 85 of them on real hardware.
  //
  // GnssConfig::rxBufferBytes is the dial, and ringOverflows() is the check --
  // a caller who blocks and does not want to think about the size is choosing
  // to lose sentences it cannot see.
  //
  // Losing them is not always wrong. A caller that only wants the current
  // position wants the NEWEST sentence, and the ring holds the oldest: when it
  // fills, the driver refuses incoming bytes rather than evicting old ones
  // (ESP-IDF 5.5.2, esp_driver_uart/src/uart.c:1302). So a block longer than
  // the ring costs roughly one extra second of fix age once the caller returns,
  // and the fix still only ever moves forward in time. A caller logging a track
  // is the one that needs every sentence.
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
  // A non-zero value here means the numbers below are ALMOST CERTAINLY an
  // undercount -- not certainly. The counter fires when the buffer is nearly
  // full, which is a proxy: a stall just long enough to reach the threshold and
  // no longer loses nothing. It reports proximity to loss, and the driver is
  // the only thing that knows about loss itself.
  uint32_t rxNearlyFullEvents() const { return rxNearlyFull_; }

  // The RX ring the driver actually granted, in bytes, or 0 before begin().
  // Not GnssConfig::rxBufferBytes: setRxBufferSize() returns what it allocated,
  // which can be less than asked for, and the near-full test above measures
  // against the granted size. A caller sizing a buffer against a blocking
  // render is choosing this number, so it has to be able to read it back
  // instead of trusting that its request landed.
  size_t rxBufferSize() const { return rxBufferBytes_; }

  // Overflows the UART DRIVER reported, as opposed to rxNearlyFullEvents()
  // above, which this class infers. Both exist because they answer different
  // questions and neither answers the other's.
  //
  // ringOverflows() counts UART_BUFFER_FULL: the ISR tried to push a batch of
  // FIFO bytes into the RX ring, the ring refused, and the driver switched the
  // RX interrupts off until somebody drains (ESP-IDF 5.5.2,
  // esp_driver_uart/src/uart.c:1302). The refused batch is stashed and
  // re-delivered, so this on its own is "the ring is full", not yet lost data.
  //
  // fifoOverflows() counts UART_FIFO_OVF, which is the loss: with the ring full
  // and the interrupts off, the hardware FIFO fills next and the bytes past it
  // are gone.
  //
  // Why these are worth their cost: rxNearlyFullEvents() is a proxy that reads
  // 0 both when nothing was lost and when nothing was measured, so "rxfull=0"
  // is a check that cannot fail. These two come from the driver, and a caller
  // sizing a buffer against a blocking render is choosing a number that only
  // these can prove wrong.
  //
  // Both UNDERCOUNT. The driver's event queue is 20 entries deep
  // (framework-arduinoespressif32 3.3.7, esp32-hal-uart.c:793) and the ISR
  // drops an event it cannot enqueue, so a long enough stall reports fewer
  // events than it caused. Non-zero means it happened; zero across a window
  // that also matched the sentence-rate baseline means it did not.
  uint32_t ringOverflows() const { return ringOverflows_.load(std::memory_order_relaxed); }
  uint32_t fifoOverflows() const { return fifoOverflows_.load(std::memory_order_relaxed); }

  uint32_t sentencesParsed() const { return sentences_; }
  // Sentences whose checksum did not match: the baud-rate and line-quality
  // signal. Distinct from framingErrors(), which counts input that had no
  // usable "*hh" at all -- a garbage burst on a cold UART, or a line lost to
  // buffer overflow.
  //
  // Splitting them was necessary and, on its own, was NOT sufficient: the torn
  // first line after begin() lands in whichever counter the tear position
  // dictates, which measurement confirmed (2026-08-31, one mid-open in each).
  // What makes checksumErrors() trustworthy is poll() discarding everything
  // before the session's first '$'.
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
  // False until the first '$' after begin(). Everything before it is discarded
  // unparsed: see poll(), and the counters' comments below.
  bool seenStart_ = false;

  GnssFix fix_;
  TalkerState talkers_[kMaxTalkers];

  uint32_t beginMs_ = 0;
  uint32_t lastFixMs_ = 0;
  uint32_t ttffMs_ = 0;

  uint32_t sentences_ = 0;
  uint32_t checksumErrors_ = 0;
  uint32_t framingErrors_ = 0;
  uint32_t rxNearlyFull_ = 0;
  // Written by the UART event task, read by the caller's. Atomic rather than a
  // lock because there is one writer and the only question asked of these is
  // "did it ever happen", which a reader one increment behind still answers.
  // Relaxed ordering for the same reason: nothing else is published with them.
  std::atomic<uint32_t> ringOverflows_{0};
  std::atomic<uint32_t> fifoOverflows_{0};
  size_t rxBufferBytes_ = 0;
  bool fixDirty_ = false;
  uint32_t bytesRead_ = 0;

  GnssRawSink rawSink_ = nullptr;
};
