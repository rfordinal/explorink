#include "PresenceMesh.h"

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <HalStorage.h>
#include <Logging.h>
#include <helpers/AdvertDataHelpers.h>

#include <cstring>

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

}  // namespace

bool PresenceMesh::beginIdentity() {
  uint8_t blob[kIdentityBytes];

  HalFile in;
  if (Storage.openFileForRead(kLog, PresenceMesh::kIdentityPath, in)) {
    const int n = in.read(blob, sizeof(blob));
    if (n == static_cast<int>(sizeof(blob))) {
      self_id.readFrom(blob, sizeof(blob));
      identityPersisted_ = true;
      LOG_INF(kLog, "identity loaded from %s", PresenceMesh::kIdentityPath);
      return true;
    }
    // A short file is a truncated write from a card pulled mid-save, not a
    // different format. Replacing it is right; silently running on a random
    // key while the file stays broken is not.
    LOG_ERR(kLog, "identity file is %d bytes, expected %u -- making a new one", n, static_cast<unsigned>(sizeof(blob)));
  }

  self_id = mesh::LocalIdentity(getRNG());

  Storage.mkdir(PresenceMesh::kIdentityDir);
  HalFile out;
  if (!Storage.openFileForWrite(kLog, PresenceMesh::kIdentityPath, out)) {
    LOG_ERR(kLog, "no card for %s -- this node's key dies at reboot", PresenceMesh::kIdentityPath);
    return false;
  }
  const size_t len = self_id.writeTo(blob, sizeof(blob));
  if (len != sizeof(blob) || out.write(blob, len) != len) {
    LOG_ERR(kLog, "identity write failed -- this node's key dies at reboot");
    return false;
  }
  identityPersisted_ = true;
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
