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
  char nodeName[21];
  // millis() at the moment the snapshot was taken, so an age computed against
  // a peer's heardUptimeMs is measured from the same clock that stamped it.
  uint32_t nowUptimeMs;
};

LoraPresenceStatus loraPresenceStatus();

// Copies at most `max` peers, newest heard first, and returns how many were
// written. Costs one memcpy of about 1.2 kB at sixteen peers, under the chip
// mutex -- which is why a screen calls it once per repaint and not once per
// row.
size_t loraPresenceSnapshot(LoraPeer* out, size_t max);

#endif  // ENABLE_LORA_MESH
