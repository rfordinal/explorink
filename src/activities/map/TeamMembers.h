#pragma once

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstddef>
#include <cstdint>
#include <string_view>

// Who the device is willing to hear about, and the two identifiers a member
// carries. Pure: no Arduino, no HAL, no SD (test/team/).
//
// **Two identifiers, because they answer different questions.** The `id` is
// whatever the transport calls a sender -- a MeshCore public key prefix, a BLE
// address, a Meshtastic node number printed as text. The `acr` is the two or
// three letters the panel draws inside the marker, chosen by the rider, and it
// is what a human says out loud. Deriving one from the other was rejected: an
// id is not readable and an acronym is not unique across the world.
//
// The roster is an **allowlist**. Anything arriving over a radio that is not in
// it is dropped and counted, never drawn and never logged
// (../../../docs/team-markers.md, "A stranger is not a member").

// Twelve, fixed array, no heap: a riding group that needs a thirteenth marker on
// a 480x800 panel has a bigger problem than storage (same reasoning as
// PinCatalog.h's slot count).
inline constexpr size_t kTeamMaxMembers = 12;

// Two or three characters plus the terminator. Longer does not fit the pin's
// head at a size a thumb can read on the panel, which is the constraint that
// picked the number.
inline constexpr size_t kTeamAcrBytes = 4;

// The transport's own name for a sender, as text. 23 characters holds a 16-hex
// MeshCore key prefix or a `!a4c1380c`-style node id with room left.
inline constexpr size_t kTeamIdBytes = 24;

// Optional, for the list screen only. Never on the map: the marker has room for
// the acronym and nothing else.
inline constexpr size_t kTeamNameBytes = 20;

struct TeamMember {
  bool present = false;
  // Muted rather than deleted: a member who is not riding today keeps their
  // acronym and their history instead of being forgotten and re-added, which
  // would split their trace across two identities in the black box.
  bool enabled = true;
  char id[kTeamIdBytes] = {};
  char acr[kTeamAcrBytes] = {};
  char name[kTeamNameBytes] = {};
};

// Uppercase A-Z and digits, two or three of them. Case is normalised on the way
// in (teamNormaliseAcr), so `rf` and `RF` are the same member and cannot both be
// added -- two markers a rider reads as one person is the failure this prevents.
bool isValidTeamAcr(std::string_view acr);

// Copies `acr` uppercased into `out` (kTeamAcrBytes). False leaves `out`
// untouched and means the acronym is not storable.
bool teamNormaliseAcr(std::string_view acr, char* out);

// Printable ASCII, no space and none of the separators the black box's CSV or
// the console's replies use. An id is machine text from a radio, so it is
// checked rather than trusted.
bool isValidTeamId(std::string_view id);

// Anything printable except the CSV and console separators; empty is legal (the
// name is optional). Truncated, not rejected, by whoever copies it in.
bool isValidTeamName(std::string_view name);
#endif  // ENABLE_TEAM_MARKERS
