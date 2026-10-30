#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <cstddef>
#include <cstdint>

// One row of the radio's event log: what the row holds, and how it is
// spelled. The file that collects them is `LoraEventLog.h`.
//
// **Split exactly like `GroupRecord` is split from `GroupFiles`**, and for the
// same reason: the format is what rots and the format is what tests can
// reach. Nothing here touches storage, so `test/lora_log/` proves every
// column with no card attached.
//
// What the radio did, as a file somebody can read on a laptop afterwards.
//
// **Not the black box, and the difference is the whole reason this exists.**
// `GroupBlackBox` records positions that were *accepted* -- it is the answer to
// "where was everyone" (`group-markers.md`). This file answers the question a
// range walk actually asks, which is "why is there nothing on my screen": an
// advert that did not parse, a neighbour who is not in the roster, a beacon
// that had nothing new to say, a packet pool that was empty. None of those
// produce a position, so none of them reach the black box, and every one of
// them is a different diagnosis.
//
// ## What is an event and what is not
//
// **State is logged when it changes; decisions are logged when they are
// taken.** `meshBeaconTick()` runs on every loop tick and declines to transmit
// on nearly all of them -- a line per decline would be thousands of rows an
// hour saying "the cadence timer has not elapsed", which is not a finding, it
// is the timer working. So:
//
// - the radio coming up and going down are two lines, carrying the cadence and
//   the name, and they are what makes every later line readable;
// - a decline is logged only once the **cadence floor has already elapsed**,
//   because at that point the device was willing to transmit and chose not to,
//   which is a decision with a reason;
// - everything heard is logged, because the radio hearing something is rare
//   enough to be worth a row every time.
//
// ## Privacy, said out loud
//
// A row carries another person's node name, their key prefix and, when their
// advert had one, their exact position -- on a removable card, in plain text,
// with no passphrase. It is the same exposure as the black box
// (`../activities/map/GroupFiles.h`) widened to **everyone in radio range**
// rather than the roster, because on a public MeshCore channel most of what
// this hears belongs to strangers who never agreed to be written down. Sixteen
// of them filled the node table in an hour in Bratislava
// (`../../docs/lora-mesh.md`). The file is dev-only today because the whole
// radio is; it is not fit to ship as it stands, and T-2039 is the decision
// about what ships.

enum class LoraLogKind : uint8_t {
  RadioUp,     // the radio started; detail carries the cadence
  RadioDown,   // the radio stopped
  AdvertRx,    // a signed advert parsed; key, name, signal, position if any
  AdvertBad,   // a frame arrived and did not parse; signal only, no identity
  PosRefused,  // a valid advert from a node the roster does not know
  AdvertTx,    // we transmitted
  TxDeclined,  // the cadence was due and we chose not to; detail says why
};

// Why a transmit that was due did not happen. Spelled as an enum rather than a
// string at the call site so the set is closed and greppable -- a reader of the
// file can be told what the whole vocabulary is.
enum class LoraTxDecline : uint8_t {
  None,
  NoNews,     // fix unchanged, under the movement threshold, not silent too long
  PoolEmpty,  // MeshCore had no packet to give out
  // No wall clock, from BLE or GNSS, so every advert we could build would
  // carry MeshCore's arbitrary epoch and be hidden on arrival. See
  // meshWallClockUtc() in main.cpp for why this is a refusal and not a
  // best-effort send.
  NoClock,
};

// Why a transmit that did happen happened.
//
// **Its own enum rather than BeaconPolicy's, and that is deliberate.** This
// file is the file format: it owns its vocabulary so a reader of `lora.csv`
// can be told the whole closed set, and so the log does not move every time a
// rule is renamed. main.cpp maps one to the other in one place. Same reason
// LoraTxDecline is spelled out here rather than taken from a caller.
//
// `Manual` is the one that is not a rule at all: three callers transmit --
// the beacon, the screen's "announce me now" and the console -- and until
// this existed the file could not say which. A row that cannot be attributed
// to a mechanism cannot answer "is the cadence working".
enum class LoraTxReason : uint8_t {
  None,  // not a transmit row
  First,
  FixChanged,
  Moved,
  Settled,
  Moving,
  Keepalive,
  Manual,
};

// One row, as it leaves whichever task noticed it.
//
// **Eight key bytes, not thirty-two.** That is the same prefix `loraGroupId()`
// derives the roster id from (`LoraGroupId.h`), so a line here joins a line in
// the black box by eye. The full key is what verifies a signature and it has
// already done that job by the time an event exists.
struct LoraLogEvent {
  LoraLogKind kind;
  LoraTxDecline decline;
  // Why we transmitted. LoraTxReason::None on every row that is not an
  // AdvertTx, which is what keeps the detail column empty where it always was.
  LoraTxReason txReason;
  uint8_t key[8];
  bool hasKey;
  char name[21];
  int16_t rssiDbm;
  int8_t snrDb;
  bool hasLoc;
  int32_t latE6;
  int32_t lonE6;
  // Our own millis() when it happened. Stamped by the task that noticed, not
  // by the writer: the writer runs on the loop task and may be a queue's worth
  // of events behind the radio.
  uint32_t uptimeMs;
  // Seconds, for RadioUp. Zero elsewhere.
  uint32_t cadenceSec;
};

inline constexpr size_t kLoraLogLineMax = 160;
inline constexpr const char* kLoraLogCsvHeader = "utc,uptime_ms,boot,kind,key,name,rssi,snr,lat,lon,detail";

// Writes one CSV row, no newline, NUL-terminated. Returns the length, or 0
// when the row would not fit -- in which case nothing is written and the
// caller logs rather than truncating. Pure: no clock, no card, no globals, so
// `test/lora_log/` proves the format with no device attached.
//
// `utc` 0 means the device has no clock and the field is left empty, which is
// the same thing `GroupRecord` does and for the same reason: a zero there would
// read as 1970 rather than as "unknown".
size_t encodeLoraLogEvent(const LoraLogEvent& ev, uint32_t utc, uint32_t bootId, char* buf, size_t bufLen);

// The same word the `tx` row carries, for a console line that has to agree
// with the file. Empty string for None. Never null.
const char* loraTxReasonText(LoraTxReason why);

#endif  // ENABLE_LORA_MESH
