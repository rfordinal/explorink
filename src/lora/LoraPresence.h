#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include "LoraPeerTable.h"

// What a screen may ask the mesh node, and the only way it may ask.
//
// **The table is written by the radio task and read by the UI task, so it is
// never handed out by reference.** `PresenceMesh::onAdvertRecv()` runs inside
// `gMesh.loop()` on the LoRa service task, under the chip mutex; a list screen
// walking the same array from the render task would be reading records while
// they are half-rewritten -- a name truncated mid-copy, a coordinate from one
// node with the key of another. So the snapshot below copies under that lock
// and the screen draws from its own copy.
//
// Declared here and defined in main.cpp, because that is where the node and
// the lock live. Everything in this header is a function, deliberately: a
// global the screen could reach into would be the race this exists to close.

struct LoraPresenceStatus {
  bool meshRunning;
  uint32_t beaconSec;        // 0 when the beacon is off, which is the default
  uint32_t advertsRejected;  // adverts heard that did not parse, since boot
  size_t peerCount;
  bool identityPersisted;  // false means this node's key dies at the next boot
  // Whether the device has a real wall clock, from BLE or GNSS. **False means
  // the node cannot transmit at all** -- every advert would carry MeshCore's
  // arbitrary epoch and be hidden on arrival (main.cpp, meshWallClockUtc()).
  // Surfaced because a beacon that is armed and silent otherwise looks like a
  // broken radio.
  bool hasWallClock;
  char nodeName[21];
  // millis() at the moment the snapshot was taken, so an age computed against
  // a peer's heardUptimeMs is measured from the same clock that stamped it.
  uint32_t nowUptimeMs;
};

LoraPresenceStatus loraPresenceStatus();

// The two bits the map header needs, read **without taking the chip mutex**.
//
// The header's structural-change test runs on every loop tick, and
// loraPresenceStatus() takes the radio's lock -- which would put the UI task
// behind whatever the radio task is doing, every tick, to decide whether an
// icon changed. These two are a plain bool and a plain integer written by the
// loop and UI tasks; reading them torn is not possible on this architecture
// and would cost one stale repaint if it were.
struct LoraPresenceIndicator {
  bool running;
  bool beaconing;
};
LoraPresenceIndicator loraPresenceIndicator();

// Copies at most `max` peers, newest heard first, and returns how many were
// written. Costs one memcpy of about 1.2 kB at sixteen peers, under the chip
// mutex -- which is why a screen calls it once per repaint and not once per
// row.
size_t loraPresenceSnapshot(LoraPeer* out, size_t max);

// The four controls, so the node can be driven from the panel and not only
// from a USB cable. Each one takes the chip mutex, writes the matching
// CrossPointSettings field and saves it, so the screen and the `CMD:LORA`
// console are two faces of one state and cannot disagree about it -- the same
// rule the map menu and the Settings screen already follow for the fields they
// share.
//
// A rider holding the device is the whole reason these exist: before them the
// radio could only be switched on over the serial console, which is not a
// thing a device on a handlebar has.

// Called by a screen that may use the radio, on the way in and on the way
// out. **The radio runs only between those two calls**: an open map is the
// state this device rides in, pocket included, and everywhere else the radio
// neither transmits nor listens. Gating receive is where the battery saving
// is -- listening costs 5.4 mA continuously and a beacon adds about 3 mA.
// See the definition in main.cpp for the rest of the reasoning.
void loraPresenceHoldRadio(bool wanted);

// Returns false when the radio would not come up at all (rail, reset, TCXO).
// `persisted` says whether the identity reached the card.
bool loraPresenceSetMesh(bool on, bool& persisted);

// 0 turns the beacon off. Any other value below the 70 s floor is refused and
// returns false, leaving the cadence untouched -- a clamp would report success
// for a number the device did not use.
bool loraPresenceSetBeacon(uint32_t seconds);

// Empty restores the key-derived default. Takes effect on the next advert.
void loraPresenceSetName(const char* name);

// One advert now. False when the mesh is off or the packet pool is empty.
// Why an advert did not go out, carried all the way to whoever asked.
//
// **Every value here was once the same sentence.** `advert not sent (mesh off,
// or packet pool empty)` was printed for refusals that were neither, and the
// one that actually fires most often -- no wall clock -- was not even in it
// (../../docs/lora-mesh.md, "One cause per refusal").
enum class LoraAdvertResult : uint8_t {
  Sent,
  MeshOff,    // the radio is not running; no screen is holding it up
  NoClock,    // no wall clock, so a timestamp would be MeshCore's epoch
  PoolEmpty,  // the 16-slot packet pool had nothing free
  TooLong,    // the encoded advert exceeds MAX_ADVERT_DATA_SIZE
};

// One short line per cause, for a console reply or a log. Never null.
const char* loraAdvertResultText(LoraAdvertResult result);

LoraAdvertResult loraPresenceAdvertNow();

// The cadences the screen cycles through, lowest first after "off". Here
// rather than in the screen because the floor they respect is a radio fact,
// not a UI one.
inline constexpr uint32_t kLoraBeaconChoices[] = {0, 70, 120, 300, 900};

#endif  // ENABLE_LORA_MESH
