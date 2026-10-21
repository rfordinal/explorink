#include "PresenceMesh.h"

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <HalStorage.h>
#include <Logging.h>
#include <helpers/AdvertDataHelpers.h>

#include <cstring>

#include "IdentityLoadRule.h"

namespace {

constexpr const char* kLog = "LORAMESH";

// 64 bytes of private key followed by 32 of public, which is what
// LocalIdentity::writeTo() lays down and readFrom() expects back
// (meshcore/src/Identity.cpp). Written as a raw blob rather than through
// MeshCore's own IdentityStore because that one is built on `fs::FS` and every
// file in this firmware goes through HalStorage instead -- SdFat is not thread
// safe and HalStorage is the mutex that makes it so (CLAUDE.md, "Hardware
// Abstraction Layer").
constexpr size_t kIdentityBytes = PRV_KEY_SIZE + PUB_KEY_SIZE;

// Wipes a stack buffer that held private key bytes.
//
// **Not for RAM pressure -- 96 bytes is nothing -- but for what a coredump
// carries.** This firmware writes coredumps to flash and archives them
// (../../docs/crashes/README.md), and the pinned ESP-IDF captures task stacks
// while `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` stays unset
// (sdkconfig.defaults:1868), so `.bss` -- where the loaded key lives -- is
// never in one. A buffer on the stack is the exception, and this is that
// buffer.
//
// Written through a volatile pointer because a plain memset() on a local that
// is dead afterwards is exactly what a compiler is allowed to delete.
void scrub(uint8_t* p, size_t n) {
  volatile uint8_t* v = p;
  while (n-- > 0) *v++ = 0;
}

}  // namespace

bool PresenceMesh::beginIdentity() {
  // **Once per boot, not once per map screen.** The screen gate of 2026-10-21
  // calls meshStart() every time the rider opens the map, so this used to run
  // on every entry -- ten map openings were ten chances for a transient card
  // failure to land on the path below. The key is in RAM after the first
  // success and nothing re-reads the card for it.
  if (identityLoaded_) return true;

  uint8_t blob[kIdentityBytes];

  // **"Cannot read it" and "it is not there" are opposites, and the first
  // version of this treated them as the same thing.** A failed open fell
  // straight through to generating a new key and overwriting the file, so one
  // busy moment on a card that is also streaming map tiles would have
  // destroyed the rider's identity and handed the group a stranger. Existence
  // is therefore asked separately, and a file that exists is never replaced
  // by this function.
  const bool exists = Storage.exists(PresenceMesh::kIdentityPath);
  bool opened = false;
  int n = -1;
  HalFile in;
  if (exists) {
    opened = Storage.openFileForRead(kLog, PresenceMesh::kIdentityPath, in);
    if (opened) {
      // The size is checked before the read, because a read can only ever
      // return at most sizeof(blob) and would therefore accept the first 96
      // bytes of a larger file as an identity. A file of the wrong size is a
      // different format, and using part of it is a guess.
      const size_t fileBytes = in.size();
      n = fileBytes == sizeof(blob) ? in.read(blob, sizeof(blob)) : static_cast<int>(fileBytes);
    }
  }

  switch (identityLoadAction(exists, opened, n, sizeof(blob))) {
    case IdentityLoadAction::UseExisting:
      self_id.readFrom(blob, sizeof(blob));
      identityLoaded_ = true;
      identityPersisted_ = true;
      scrub(blob, sizeof(blob));
      LOG_INF(kLog, "identity loaded from %s", PresenceMesh::kIdentityPath);
      return true;

    case IdentityLoadAction::Refuse:
      // Either the file would not open or it read short, and from here those
      // are indistinguishable -- a truncated write and a busy bus look the
      // same. Refuse both: a wrong guess costs the rider a key that cannot be
      // recreated, and the recovery is one a human makes deliberately once
      // they know (delete the file).
      scrub(blob, sizeof(blob));
      LOG_ERR(kLog, "%s is there and did not read whole (opened=%d, %d of %u bytes) -- refusing to replace it",
              PresenceMesh::kIdentityPath, opened ? 1 : 0, n, static_cast<unsigned>(sizeof(blob)));
      return false;

    case IdentityLoadAction::Create:
      break;  // the only path that makes a key; below
  }

  // Genuinely absent. This is the only path that ever creates a key, and it
  // runs once in a device's life.
  self_id = mesh::LocalIdentity(getRNG());
  identityLoaded_ = true;

  Storage.mkdir(PresenceMesh::kIdentityDir);
  const size_t len = self_id.writeTo(blob, sizeof(blob));
  bool written = len == sizeof(blob);
  if (written) {
    HalFile out;
    written = Storage.openFileForWrite(kLog, PresenceMesh::kIdentityPath, out) && out.write(blob, len) == len;
  }

  scrub(blob, sizeof(blob));
  if (!written) {
    // **Leave no half-written file behind.** The read path above now refuses a
    // short one forever, so a failed first write that left 40 bytes on the
    // card would brick this feature on that device until somebody deleted the
    // file by hand.
    Storage.remove(PresenceMesh::kIdentityPath);
    LOG_ERR(kLog, "could not write %s -- running on a key that dies at reboot", PresenceMesh::kIdentityPath);
    return true;  // the node still works today; identityPersisted() says it will not tomorrow
  }

  identityPersisted_ = true;
  scrub(blob, sizeof(blob));
  LOG_INF(kLog, "new identity written to %s", PresenceMesh::kIdentityPath);
  return true;
}

bool PresenceMesh::sendSelfAdvert(const char* name, bool hasLoc, int32_t latE6, int32_t lonE6) {
  uint8_t appData[MAX_ADVERT_DATA_SIZE];
  uint8_t len;

  // ADV_TYPE_CHAT, not ADV_TYPE_SENSOR or ADV_TYPE_REPEATER: to the rest of a
  // MeshCore mesh this device is an ordinary node a person carries. The type
  // is what other clients use to decide whether to list us at all, and the two
  // alternatives would both be a lie about what this is.
  if (hasLoc) {
    AdvertDataBuilder builder(ADV_TYPE_CHAT, name, static_cast<double>(latE6) / 1e6, static_cast<double>(lonE6) / 1e6);
    len = builder.encodeTo(appData);
  } else {
    AdvertDataBuilder builder(ADV_TYPE_CHAT, name);
    len = builder.encodeTo(appData);
  }

  mesh::Packet* pkt = createAdvert(self_id, appData, len);
  if (pkt == nullptr) {
    LOG_ERR(kLog, "advert not sent: packet pool empty");
    return false;
  }
  sendZeroHop(pkt);
  return true;
}

void PresenceMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
  lastRxRssiDbm_ = static_cast<int16_t>(rssi);
}

void PresenceMesh::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                                const uint8_t* appData, size_t appDataLen) {
  AdvertDataParser parser(appData, static_cast<uint8_t>(appDataLen));
  if (!parser.isValid()) {
    ++advertsRejected_;
    return;
  }

  // The SNR is the packet's own, carried in its header by the Dispatcher. The
  // RSSI is not: MeshCore's Packet has no field for it, so it comes from the
  // last frame the radio reported. For an advert the Dispatcher processed
  // straight away -- which is every advert when the air is quiet -- those are
  // the same frame. Under traffic a scored packet can be held back, and then
  // this number belongs to something that landed afterwards. Said here rather
  // than hidden, because signal strength is the number a range test reads.
  const int8_t snrDb = static_cast<int8_t>(packet->getSNR());

  const bool isNew = peers_.heard(id.pub_key, parser.hasName() ? parser.getName() : nullptr, parser.hasLatLon(),
                                  parser.getIntLat(), parser.getIntLon(), timestamp, lastRxRssiDbm_, snrDb, millis());

  if (isNew) {
    LOG_INF(kLog, "new node %02x%02x%02x%02x '%s' rssi=%d snr=%d", id.pub_key[0], id.pub_key[1], id.pub_key[2],
            id.pub_key[3], parser.hasName() ? parser.getName() : "", static_cast<int>(lastRxRssiDbm_),
            static_cast<int>(snrDb));
  }
}

#endif  // ENABLE_LORA_MESH
