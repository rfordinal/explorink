#pragma once

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "TeamFix.h"
#include "TeamMembers.h"

// One line of the black box, and the day file it belongs in.
//
// Pure: no Arduino, no HAL, no SD. TeamBlackBox does the card, this does the
// bytes, so every format rule is host-testable (test/team/).
//
// CSV, one row per accepted position, header written when a file is created:
//
//   utc,recv_utc,uptime_ms,boot,who,id,lat,lon,heading,speed_kmh,src
//   1789430400,1789430402,81234,3149271044,RF,a4c1380c,48.1486000,17.1077000,4,62,lora
//
// **The acronym is a label, the id is the identity.** `RF` is what the rider
// typed and can retype; a roster edit between rides makes an old row's acronym
// point at somebody else. The id is what the transport said, so it survives
// that, and a replay matches on it first.
//
// **It is not a MAC and must never be one.** A constant broadcast identifier is
// a tracking device and modern phones flag it as one, so our own advertised id
// has to rotate from a group secret (../../../docs/safety-concept.md,
// "Anti-stalking") -- and a peer's BLE address is usually a resolvable random
// address that rotates anyway, which is not something to store. What goes here
// is the transport's *durable* identity: for MeshCore the Ed25519 public key,
// which is also what makes a forged position detectable there.
//
// **`recv_utc` is the column a search reads.** The sender's `utc` is theirs and
// is often absent -- a peer with no clock, a console line, an early frame. Ours
// is there whenever the device knew the time at all, from the phone's packet or
// from the GNSS receiver (MapTeam::utcNowOrZero), and it says when this device
// heard the position. It is also what dates a position across a reboot.
//
// **`boot` is what makes `uptime_ms` mean something after a reload.** A device
// with no clock dates a position by its own uptime, and an uptime from a
// *previous* run is a number with no relation to this one -- so without this
// column every replayed position had to be treated as undateable, and re-opening
// the map turned every age into `?` even for a fix heard a minute earlier
// (seen on an X4 Pro, 2026-09-16). With it, a row from this run is as good as
// live and one from an older run is honestly unknown.
//
// CSV rather than the pins log's `v1|...|crc32` framing, deliberately. This file
// is read by a person -- on a laptop, possibly by somebody looking for a rider
// -- and /trailink/power.csv and /trailink/gnss.csv already set that precedent.
// There is no CRC for the same reason: a torn last line is visibly torn, and a
// checksum that makes a rescuer's spreadsheet refuse a row helps nobody.
//
// | field     | notes                                                        |
// |-----------|--------------------------------------------------------------|
// | utc       | unix seconds, 0 = the sender had no clock                    |
// | uptime_ms | our uptime at receipt; orders rows inside a run with no clock |
// | who       | member acronym, or `ME` for the rider's own position          |
// | lat, lon  | decimal degrees, 7 places, integer-formatted (no FPU)         |
// | heading   | 0-15 sixteenths of a turn, empty when not sent                |
// | speed_kmh | empty when not sent                                           |
// | src       | cmd / ble / lora / gnss / unknown -- how it reached us        |

// `ME` is the rider's own row, so it can never be a member's acronym -- two
// people under one name in a search log is the failure that reserves it.
inline constexpr const char* kTeamSelfWho = "ME";

struct TeamRecord {
  uint32_t utc = 0;      // the sender's clock
  uint32_t recvUtc = 0;  // ours, when we heard it
  uint32_t uptimeMs = 0;
  // Identifies the run of *this device* that wrote the row, so its uptime can be
  // compared against the current one. 0 when the row predates the column.
  uint32_t boot = 0;
  char who[kTeamAcrBytes] = {};
  char id[kTeamIdBytes] = {};
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  uint8_t heading = 0;
  bool hasHeading = false;
  uint16_t speedKmh = 0;
  bool hasSpeed = false;
  TeamFixSource source = TeamFixSource::Unknown;
};

// Every field at its type's widest plus the separators, rounded up: four
// ten-digit numbers, a three-character acronym, a 23-character id, two signed
// coordinates, a heading, a speed and a source word. Sized for the type, not for
// today's data (same rule as kPinLineMax).
inline constexpr size_t kTeamLineMax = 160;

inline constexpr const char* kTeamCsvHeader = "utc,recv_utc,uptime_ms,boot,who,id,lat,lon,heading,speed_kmh,src";

const char* teamSourceText(TeamFixSource source);
bool teamSourceFromText(std::string_view text, TeamFixSource& out);

// Writes the row into `buf`, no trailing newline (TeamBlackBox adds it). Returns
// the length written, or 0 when the buffer is too small or `who` is unstorable.
size_t encodeTeamRecord(const TeamRecord& rec, char* buf, size_t bufLen);

// Parses one row, terminator already stripped, header line included (it is
// refused like any other malformed row). False means "skip this line and keep
// reading" -- never "the file is broken".
//
// Takes eight to eleven fields: a card written by an older build is still
// somebody's last known position, and refusing it would throw away the evidence
// over columns that only help dating. Eight is the original row, nine adds
// `boot`, ten adds `recv_utc`, eleven adds `id` after the acronym.
bool decodeTeamRecord(std::string_view line, TeamRecord& out);

// The file a fix belongs in. Rotation is by day, so the name carries the date:
// `bb-20260916.csv`. A device with no clock writes `bb-noclock.csv`, because a
// day it cannot name is still a day whose rows must not be lost.
bool teamDayFileName(uint32_t utc, char* out, size_t outBytes);

// Days since 1970-01-01 UTC, which is what rotation compares. 0 for utc 0.
uint32_t teamDayNumber(uint32_t utc);

#endif  // ENABLE_TEAM_MARKERS
