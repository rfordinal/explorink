#include "SyncPairingStore.h"

#include <Logging.h>
#include <Preferences.h>

#include "SyncCryptoDevice.h"

namespace syncstore {

namespace {

constexpr const char* kLogTag = "SYNC";
constexpr const char* kNamespace = "cpsync";
constexpr const char* kKeyStatic = "dev_sk";
constexpr const char* kKeyPhones = "phones";

bool persist(void*, const syncchannel::PairedPhones& table) { return saveTable(table); }

}  // namespace

bool loadOrCreateStaticKey(uint8_t out[syncnoise::kDhLen]) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    LOG_ERR(kLogTag, "NVS namespace %s would not open", kNamespace);
    return false;
  }
  if (prefs.getBytesLength(kKeyStatic) == syncnoise::kDhLen &&
      prefs.getBytes(kKeyStatic, out, syncnoise::kDhLen) == syncnoise::kDhLen) {
    prefs.end();
    return true;
  }
  if (!syncdevice::crypto().random(out, syncnoise::kDhLen)) {
    prefs.end();
    return false;
  }
  const size_t wrote = prefs.putBytes(kKeyStatic, out, syncnoise::kDhLen);
  prefs.end();
  if (wrote != syncnoise::kDhLen) {
    LOG_ERR(kLogTag, "could not store the sync static key");
    syncnoise::wipe(out, syncnoise::kDhLen);
    return false;
  }
  LOG_INF(kLogTag, "generated this device's sync key");
  return true;
}

void loadTable(syncchannel::PairedPhones& out) {
  out.clear();
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return;  // read-only open fails when the namespace does not exist yet
  uint8_t blob[syncchannel::kTableBlobLen];
  const size_t len = prefs.getBytesLength(kKeyPhones);
  const size_t got = len <= sizeof(blob) ? prefs.getBytes(kKeyPhones, blob, len) : 0;
  prefs.end();
  if (len == 0) return;
  if (got != len || !out.parse(blob, len)) {
    LOG_ERR(kLogTag, "paired-phone table unreadable (%u bytes); treating as none", static_cast<unsigned>(len));
  }
}

bool saveTable(const syncchannel::PairedPhones& table) {
  uint8_t blob[syncchannel::kTableBlobLen];
  const size_t len = table.serialize(blob);
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    LOG_ERR(kLogTag, "NVS namespace %s would not open", kNamespace);
    return false;
  }
  const size_t wrote = prefs.putBytes(kKeyPhones, blob, len);
  prefs.end();
  if (wrote != len) {
    LOG_ERR(kLogTag, "could not store the paired-phone table");
    return false;
  }
  LOG_INF(kLogTag, "paired phones: %u", static_cast<unsigned>(table.count));
  return true;
}

syncchannel::PairingSink sink() { return syncchannel::PairingSink{nullptr, &persist}; }

void forgetAll() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.remove(kKeyPhones);
  prefs.remove(kKeyStatic);
  prefs.end();
  LOG_INF(kLogTag, "pairings and the sync key forgotten");
}

}  // namespace syncstore
