#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include "LoraEventLine.h"
#include "LoraMeshRadio.h"
#include "LoraPeerTable.h"

// A MeshCore node that does one thing: say who and where we are, and keep a
// list of everyone who said the same back.
//
// **Why mesh::Mesh and not BaseChatMesh.** MeshCore's own clients subclass
// BaseChatMesh, which brings contacts, channels, text messages, ACKs and
// connections -- a messaging client, because MeshCore is messaging-first. None
// of that is this product: ExplorInk answers where am I and what is around me,
// and group text is a separate decision with its own reason (parent
// docs/lora.md, "What we will not build"). Subclassing one layer lower costs a
// forty-line override and keeps the chat machinery, and its contact array, out
// of the binary entirely.
//
// **An advert is the whole feature.** MeshCore's advert carries an Ed25519
// public key, a timestamp, a signature, and optionally a name and a coordinate
// at 1e6 degrees. It is signed, which is the property that made this protocol
// the choice over Meshtastic, where a group message carries no sender
// signature at all (docs/prior-art-meshcore.md, "The security reading"). So
// "who is out there" is answered by a packet type that already exists, already
// has a parser, and already cannot be forged by a stranger who merely holds the
// channel key.
//
// What this class deliberately does not do:
//
// - **It never forwards.** allowPacketForward() returns false. That is
//   MeshCore's own companion behaviour, and it is also the choice the one other
//   project on this board made deliberately: relaying puts every packet it
//   hears back on the air, and airtime and battery on a bike are not free
//   (parent docs/lora.md, "And their non-relay is a choice").
// - **It never transmits unasked.** sendSelfAdvert() is called by whoever owns
//   the cadence, and nothing here starts a timer. An advert is a plaintext
//   broadcast of this device's name and position to anyone in range, so the
//   beacon is off by default and the rider turns it on (parent docs/lora.md,
//   "Security of the mesh channel").
class PresenceMesh : public mesh::Mesh {
 public:
  PresenceMesh(LoraMeshRadio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
               mesh::PacketManager& mgr, mesh::MeshTables& tables)
      : mesh::Mesh(radio, ms, rng, rtc, mgr, tables), radio_(&radio) {}

  // Loads this device's keypair from the card, or makes one and saves it.
  //
  // **The identity has to outlive a reboot or the list is worthless.** A node
  // that draws a fresh key at every boot appears as a new stranger to everyone
  // who already knows it, and the group's rosters fill with ghosts of the same
  // rider. Returns false when the card could not be read or written -- the node
  // then runs on a RAM-only identity, which is better than refusing to run and
  // is logged as what it is.
  bool beginIdentity();

  // True when beginIdentity() got its key off the card, or put one there.
  // False means the node is running on a key that dies at the next boot, which
  // is a thing the rider has to be told rather than a reason to refuse.
  bool identityPersisted() const { return identityPersisted_; }

  // Broadcasts one advert at zero hops: to whoever can hear us directly, with
  // no node asked to pass it on. Zero hop rather than flood because a position
  // is only interesting to the people near it, and a flooded advert is paid for
  // in everybody's airtime (parent docs/lora.md, "What we take from FieldMesh",
  // item 3).
  //
  // `name` may be null or empty, and the coordinate is omitted unless hasLoc.
  // Returns false when the packet pool is empty.
  // Why an advert did not go out, or that it did. **One value per cause, on
  // purpose**: until 2026-10-30 this returned a bool and every caller printed
  // "mesh off, or packet pool empty" for all of them. On a board whose pool
  // was full and whose mesh was running that string sent an afternoon after a
  // defect that did not exist, and put a wrong finding in the task list
  // (../../docs/lora-mesh.md, "One cause per refusal").
  enum class AdvertResult : uint8_t {
    Sent,
    PoolEmpty,  // no free packet in the 16-slot static pool
    TooLong,    // the encoded advert exceeds MAX_ADVERT_DATA_SIZE
  };

  AdvertResult sendSelfAdvert(const char* name, bool hasLoc, int32_t latE6, int32_t lonE6);

  // Where a heard position goes next, when somebody wants one.
  //
  // **A plain function pointer, and the hook is the whole point.** This class
  // must not know the group layer exists: `MapGroup` lives in the map's own
  // directory, writes the black box to the SD card and is edited by the UI
  // task, and none of that belongs inside a radio receive path. main.cpp owns
  // both and joins them.
  //
  // **Called on the radio task**, so the hook may not touch the card, the
  // roster or anything the UI writes. The one in main.cpp queues and returns;
  // the work happens on loop().
  using PositionHook = void (*)(const uint8_t pubKey[32], int32_t latE6, int32_t lonE6, uint32_t advertUtc);
  void setPositionHook(PositionHook hook) { positionHook_ = hook; }

  // Everything heard, whether or not it carried a position and whether or not
  // it parsed at all.
  //
  // **A separate hook from the one above, because they answer different
  // questions.** The position hook feeds the map and therefore fires only for
  // an advert with a coordinate from a node somebody approved. This one feeds
  // the file somebody reads after a walk that found nothing, so it has to fire
  // for the advert that had no position and for the frame that did not parse
  // -- those are the rows that say why the screen is empty
  // (`LoraEventLine.h`).
  //
  // **Called on the radio task**, same contract as the position hook: copy and
  // return, touch no card and nothing the UI writes.
  using EventHook = void (*)(const LoraLogEvent& ev);
  void setEventHook(EventHook hook) { eventHook_ = hook; }

  const LoraPeerTable& peers() const { return peers_; }
  LoraPeerTable& peers() { return peers_; }

  // Adverts heard whose signature or payload did not parse, since boot. Shown
  // rather than swallowed: on a shared band this is the number that says
  // somebody is transmitting something we are not meant to read.
  uint32_t advertsRejected() const { return advertsRejected_; }

  // Where the keypair lives. Under the same `/trailink` root as the rest of the
  // device's state -- the path is the SD card's legacy spelling and renaming it
  // is a card-compatibility change, not a text change (CLAUDE.md, "Naming").
  static constexpr const char* kIdentityDir = "/trailink/lora";
  static constexpr const char* kIdentityPath = "/trailink/lora/identity.bin";

 protected:
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* appData,
                    size_t appDataLen) override;

  bool allowPacketForward(const mesh::Packet* packet) override { return false; }

  // The Dispatcher's own hook, called the moment a frame comes off the radio.
  // It is the only place RSSI exists: MeshCore's Packet carries SNR and no
  // RSSI field, so without this the strength of a signal -- the number a range
  // test is actually made of -- would never reach the list.
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;

 private:
  LoraPeerTable peers_;
  uint32_t advertsRejected_ = 0;
  bool identityPersisted_ = false;
  // The key is read from the card once per boot and kept. See beginIdentity().
  bool identityLoaded_ = false;
  int16_t lastRxRssiDbm_ = 0;
  PositionHook positionHook_ = nullptr;
  EventHook eventHook_ = nullptr;
  // Kept for its noise, which seeds the keypair. See beginIdentity().
  LoraMeshRadio* radio_ = nullptr;
};

#endif  // ENABLE_LORA_MESH
