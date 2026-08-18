#include "WalletKeyStore.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>

#include "WalletCryptoDevice.h"

namespace wallet {

namespace {

constexpr const char* kLogTag = "WALLETKEY";
// Same shape as the hardware namespace in lib/hal/HalGPIO.cpp: a short name, and
// one Preferences handle opened and closed per access rather than held.
constexpr const char* kNamespace = "cpwallet";
constexpr const char* kKeySecret = "dsecret";
constexpr const char* kKeySalt = "salt";
constexpr const char* kKeyIters = "iters";
constexpr const char* kKeyWrap = "wrap";
constexpr const char* kKeyFails = "fails";

// Reads the device secret, making one the first time anybody asks. Generated on
// the device and never sent anywhere: it is what stops a leaked provisioning file
// from being a wrap.
bool loadOrCreateDeviceSecret(uint8_t out[kDeviceSecretLen]) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    LOG_ERR(kLogTag, "NVS namespace %s would not open", kNamespace);
    return false;
  }
  size_t got = prefs.getBytesLength(kKeySecret);
  if (got == kDeviceSecretLen && prefs.getBytes(kKeySecret, out, kDeviceSecretLen) == kDeviceSecretLen) {
    prefs.end();
    return true;
  }
  randomBytes(out, kDeviceSecretLen);
  const size_t wrote = prefs.putBytes(kKeySecret, out, kDeviceSecretLen);
  prefs.end();
  if (wrote != kDeviceSecretLen) {
    LOG_ERR(kLogTag, "could not store the device secret");
    secureWipe(out, kDeviceSecretLen);
    return false;
  }
  LOG_INF(kLogTag, "generated this device's wallet secret (%u bytes)", static_cast<unsigned>(kDeviceSecretLen));
  return true;
}

}  // namespace

bool KeyStore::isProvisioned() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return false;
  const size_t len = prefs.getBytesLength(kKeyWrap);
  prefs.end();
  return len == kWrappedKeyLen;
}

uint32_t KeyStore::iterations() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return 0;
  const uint32_t iters = prefs.getUInt(kKeyIters, 0);
  prefs.end();
  return iters;
}

bool KeyStore::provision(const uint8_t key[kWalletKeyLen], const char* pin, const size_t pinLen, const uint8_t* salt,
                         const size_t saltLen, const uint32_t iterations, const bool force) {
  if (key == nullptr || pin == nullptr || salt == nullptr) return false;
  if (pinLen < kPinMinLen || pinLen > kPinMaxLen) return false;
  if (saltLen != kPbkdf2SaltLen || iterations == 0) return false;

  if (isProvisioned() && !force) {
    // Overwriting a wrap makes every asset already on the card unreadable for
    // ever. That needs to be asked for explicitly, not defaulted into.
    LOG_ERR(kLogTag, "already provisioned; refusing to overwrite the wrap");
    return false;
  }

  uint8_t secret[kDeviceSecretLen];
  if (!loadOrCreateDeviceSecret(secret)) return false;

  uint8_t kek[kWalletKeyLen];
  const bool derived = deriveKek(pin, pinLen, secret, sizeof(secret), salt, saltLen, iterations, kek);
  secureWipe(secret, sizeof(secret));
  if (!derived) return false;

  uint8_t wrapped[kWrappedKeyLen];
  const bool wrapOk = gcmWrapKey(kek, key, wrapped, sizeof(wrapped));
  secureWipe(kek, sizeof(kek));
  if (!wrapOk) {
    LOG_ERR(kLogTag, "wrapping the wallet key failed");
    secureWipe(wrapped, sizeof(wrapped));
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    secureWipe(wrapped, sizeof(wrapped));
    return false;
  }
  const bool stored = prefs.putBytes(kKeySalt, salt, saltLen) == saltLen && prefs.putUInt(kKeyIters, iterations) > 0 &&
                      prefs.putBytes(kKeyWrap, wrapped, sizeof(wrapped)) == sizeof(wrapped) &&
                      prefs.putUChar(kKeyFails, 0) > 0;
  prefs.end();
  secureWipe(wrapped, sizeof(wrapped));
  if (!stored) {
    LOG_ERR(kLogTag, "storing the wrap failed");
    return false;
  }
  LOG_INF(kLogTag, "provisioned: %u PBKDF2 iterations, %u-byte salt, wrap stored", static_cast<unsigned>(iterations),
          static_cast<unsigned>(saltLen));
  return true;
}

bool KeyStore::unwrap(const char* pin, const size_t pinLen, uint8_t out[kWalletKeyLen]) {
  if (pin == nullptr || out == nullptr || pinLen < kPinMinLen || pinLen > kPinMaxLen) return false;

  uint8_t secret[kDeviceSecretLen];
  uint8_t salt[kPbkdf2SaltLen];
  uint8_t wrapped[kWrappedKeyLen];
  uint32_t iters = 0;

  {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false;
    const bool haveAll = prefs.getBytes(kKeySecret, secret, sizeof(secret)) == sizeof(secret) &&
                         prefs.getBytes(kKeySalt, salt, sizeof(salt)) == sizeof(salt) &&
                         prefs.getBytes(kKeyWrap, wrapped, sizeof(wrapped)) == sizeof(wrapped);
    iters = prefs.getUInt(kKeyIters, 0);
    prefs.end();
    if (!haveAll || iters == 0) {
      LOG_ERR(kLogTag, "not provisioned: no secret, salt, wrap or iteration count");
      secureWipe(secret, sizeof(secret));
      return false;
    }
  }

  uint8_t kek[kWalletKeyLen];
  const bool derived = deriveKek(pin, pinLen, secret, sizeof(secret), salt, sizeof(salt), iters, kek);
  secureWipe(secret, sizeof(secret));
  if (!derived) {
    secureWipe(wrapped, sizeof(wrapped));
    return false;
  }

  const bool ok = gcmUnwrapKey(kek, wrapped, sizeof(wrapped), out);
  secureWipe(kek, sizeof(kek));
  secureWipe(wrapped, sizeof(wrapped));
  // A failure is a wrong PIN or an altered wrap, and GCM cannot say which. The
  // caller counts it against the rate limiter either way.
  return ok;
}

UnlockResult KeyStore::tryUnlock(const char* pinText, uint32_t& unwrapMicros, uint8_t& failures, uint32_t& waitMs) {
  unwrapMicros = 0;
  waitMs = 0;
  failures = KeyStore::failures();

  char pin[kPinBufBytes];
  size_t pinLen = 0;
  if (!normalisePin(pinText, pin, sizeof(pin), pinLen)) {
    // Not a PIN at all. Refused without touching the counter: a typo must not spend
    // one of ten attempts.
    secureWipe(pin, sizeof(pin));
    return UnlockResult::Malformed;
  }
  if (!isProvisioned()) {
    secureWipe(pin, sizeof(pin));
    return UnlockResult::NotProvisioned;
  }
  if (pinIsLockedOut(failures)) {
    secureWipe(pin, sizeof(pin));
    return UnlockResult::LockedOut;
  }
  // The delay is re-armed from the persisted count every time an unlock path starts,
  // so a reboot cannot skip it, and the gate is shared with the PIN screen.
  Session& session = Session::instance();
  session.armRetryDelay(failures);
  waitMs = session.retryWaitMs();
  if (waitMs > 0) {
    secureWipe(pin, sizeof(pin));
    return UnlockResult::Waiting;
  }

  uint8_t key[kWalletKeyLen];
  const uint32_t startedUs = micros();
  const bool ok = unwrap(pin, pinLen, key);
  unwrapMicros = micros() - startedUs;
  secureWipe(pin, sizeof(pin));

  if (!ok) {
    secureWipe(key, sizeof(key));
    recordFailure();
    failures = KeyStore::failures();
    session.armRetryDelay(failures);
    waitMs = session.retryWaitMs();
    return pinIsLockedOut(failures) ? UnlockResult::LockedOut : UnlockResult::BadPin;
  }

  session.setKey(key);
  secureWipe(key, sizeof(key));
  clearFailures();
  failures = 0;
  return UnlockResult::Ok;
}

uint8_t KeyStore::failures() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return 0;
  const uint8_t fails = prefs.getUChar(kKeyFails, 0);
  prefs.end();
  return fails;
}

void KeyStore::recordFailure() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  const uint8_t fails = prefs.getUChar(kKeyFails, 0);
  // Written before the next attempt is allowed, so pulling the power mid-guess
  // does not buy a free retry.
  if (fails < 0xFF) prefs.putUChar(kKeyFails, static_cast<uint8_t>(fails + 1));
  prefs.end();
  LOG_ERR(kLogTag, "wrong PIN: %u consecutive failures", static_cast<unsigned>(fails + 1));
}

void KeyStore::clearFailures() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.putUChar(kKeyFails, 0);
  prefs.end();
}

void KeyStore::forgetWrap() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.remove(kKeyWrap);
  prefs.remove(kKeySalt);
  prefs.remove(kKeyIters);
  prefs.putUChar(kKeyFails, 0);
  prefs.end();
  LOG_INF(kLogTag, "wrap forgotten; the device secret stays");
}

}  // namespace wallet
