#include "LoraEventLine.h"

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <cstdio>
#include <cstring>

namespace {

const char* kindText(LoraLogKind kind) {
  switch (kind) {
    case LoraLogKind::RadioUp:
      return "up";
    case LoraLogKind::RadioDown:
      return "down";
    case LoraLogKind::AdvertRx:
      return "rx";
    case LoraLogKind::AdvertBad:
      return "rxbad";
    case LoraLogKind::PosRefused:
      return "refused";
    case LoraLogKind::AdvertTx:
      return "tx";
    case LoraLogKind::TxDeclined:
      return "txskip";
  }
  return "?";
}

const char* declineText(LoraTxDecline why) {
  switch (why) {
    case LoraTxDecline::None:
      return "";
    case LoraTxDecline::NoNews:
      return "no_news";
    case LoraTxDecline::PoolEmpty:
      return "pool_empty";
    case LoraTxDecline::NoClock:
      return "no_clock";
  }
  return "";
}

const char* txReasonTextImpl(LoraTxReason why) {
  switch (why) {
    case LoraTxReason::None:
      return "";
    case LoraTxReason::First:
      return "first";
    case LoraTxReason::FixChanged:
      return "fix";
    case LoraTxReason::Moved:
      return "moved";
    case LoraTxReason::Settled:
      return "settled";
    case LoraTxReason::Moving:
      return "moving";
    case LoraTxReason::Keepalive:
      return "keepalive";
    case LoraTxReason::Manual:
      return "manual";
  }
  return "";
}

// int32 scaled by 1e6 back to plain decimal degrees. Integer only, same reason
// as `GroupRecord`'s 1e7 version: no FPU, and a soft-float `printf("%f")` is
// both slow and flash nothing else here needs. Not shared with that one
// because the scale differs -- MeshCore's advert carries 1e6 and our own
// records carry 1e7, and a single helper taking a scale would hide which is
// which at the call site.
void formatE6(int32_t value, char* buf, size_t bufLen) {
  const bool negative = value < 0;
  const uint32_t magnitude =
      negative ? static_cast<uint32_t>(-static_cast<int64_t>(value)) : static_cast<uint32_t>(value);
  snprintf(buf, bufLen, "%s%lu.%06lu", negative ? "-" : "", static_cast<unsigned long>(magnitude / 1000000u),
           static_cast<unsigned long>(magnitude % 1000000u));
}

// A name goes in the file as the sender wrote it, minus anything that would
// make the row stop being one row of CSV. **The sender chose that string and
// nobody validated it**: it arrives over the air from a stranger, and a comma
// or a newline in it would silently turn one event into two rows or shift
// every later column. Replaced rather than rejected, because the name is not
// what the row is for.
void sanitiseName(const char* in, char* out, size_t outBytes) {
  size_t i = 0;
  for (; in != nullptr && in[i] != '\0' && i + 1 < outBytes; ++i) {
    const char c = in[i];
    out[i] = (c == ',' || c == '\n' || c == '\r' || c < 0x20) ? '_' : c;
  }
  out[i] = '\0';
}

}  // namespace

const char* loraTxReasonText(LoraTxReason why) { return txReasonTextImpl(why); }

size_t encodeLoraLogEvent(const LoraLogEvent& ev, uint32_t utc, uint32_t bootId, char* buf, size_t bufLen) {
  if (buf == nullptr || bufLen == 0) return 0;

  // An empty field means "this row has nothing to say here", never zero. A
  // literal 0 in the key column would read as a node, and a 0 in the signal
  // columns as a perfect link.
  char key[17] = {};
  if (ev.hasKey) {
    for (size_t i = 0; i < sizeof(ev.key); ++i) snprintf(key + i * 2, 3, "%02x", ev.key[i]);
  }

  char name[21];
  sanitiseName(ev.name, name, sizeof(name));

  char rssi[8] = {};
  char snr[8] = {};
  const bool hasSignal = ev.kind == LoraLogKind::AdvertRx || ev.kind == LoraLogKind::AdvertBad;
  if (hasSignal) {
    snprintf(rssi, sizeof(rssi), "%d", static_cast<int>(ev.rssiDbm));
    snprintf(snr, sizeof(snr), "%d", static_cast<int>(ev.snrDb));
  }

  char lat[16] = {};
  char lon[16] = {};
  if (ev.hasLoc) {
    formatE6(ev.latE6, lat, sizeof(lat));
    formatE6(ev.lonE6, lon, sizeof(lon));
  }

  // The detail column is a decline reason for `txskip`, a send reason for
  // `tx`, and the cadence for `up`. One column rather than three, because no
  // row ever needs two of them and an always-empty column is a column nobody
  // reads.
  char detail[16] = {};
  if (ev.kind == LoraLogKind::TxDeclined) {
    snprintf(detail, sizeof(detail), "%s", declineText(ev.decline));
  } else if (ev.kind == LoraLogKind::AdvertTx) {
    // **Without this a hardware pass cannot read its own result.** Four rules
    // can put a position on the air and the row looked identical for all of
    // them, so "did it send because the rider stopped" was unanswerable from
    // the file the test produces.
    snprintf(detail, sizeof(detail), "%s", txReasonTextImpl(ev.txReason));
  } else if (ev.kind == LoraLogKind::RadioUp) {
    if (ev.cadenceSec == 0) {
      snprintf(detail, sizeof(detail), "rx_only");
    } else {
      snprintf(detail, sizeof(detail), "bcn%lus", static_cast<unsigned long>(ev.cadenceSec));
    }
  }

  char utcField[12] = {};
  if (utc != 0) snprintf(utcField, sizeof(utcField), "%lu", static_cast<unsigned long>(utc));

  const int written =
      snprintf(buf, bufLen, "%s,%lu,%lu,%s,%s,%s,%s,%s,%s,%s,%s", utcField, static_cast<unsigned long>(ev.uptimeMs),
               static_cast<unsigned long>(bootId), kindText(ev.kind), key, name, rssi, snr, lat, lon, detail);
  if (written < 0 || static_cast<size_t>(written) >= bufLen) return 0;
  return static_cast<size_t>(written);
}

#endif  // ENABLE_LORA_MESH
