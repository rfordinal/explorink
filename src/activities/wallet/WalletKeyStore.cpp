#include "WalletKeyStore.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>

#include "WalletCryptoDevice.h"
#include "components/lock/PatternLock.h"

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
// The cell the stored pattern starts on. Not a secret -- the gate draws the
// cursor on it before the first press -- but the wrap says nothing about the path
// that produced it, so without this the gate opens on the wrong cell and no path
// a rider can walk will ever match (PatternLock.h, isWellFormed).
constexpr const char* kKeyPatternStart = "pstart";

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

uint8_t KeyStore::patternStart() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return kPatternNoStart;
  // 0xFF rather than the grid's default cell: a caller must be able to tell "this
  // device has no stored start" from "it starts at 5". Defaulting to a real cell
  // would hand the gate a plausible wrong answer on an unprovisioned device, and
  // a plausible wrong answer here reads on the panel as a gate that refuses a
  // pattern the rider is entering correctly.
  const uint8_t start = prefs.getUChar(kKeyPatternStart, kPatternNoStart);
  prefs.end();
  return start < patternlock::kCells ? start : kPatternNoStart;
}

bool KeyStore::provision(const uint8_t key[kWalletKeyLen], const char* pattern, const size_t patternLen,
                         const uint8_t* salt, const size_t saltLen, const uint32_t iterations, const bool force) {
  if (key == nullptr || pattern == nullptr || salt == nullptr) return false;
  if (patternLen < kPatternMinPoints || patternLen > kPatternMaxPoints) return false;
  if (saltLen != kPbkdf2SaltLen || iterations == 0) return false;
  // Geometry, here and nowhere else. This is the one moment the device chooses
  // what it will later demand, so it is the only moment a pattern nobody can walk
  // can be refused -- a wrap made from an illegal path is a wallet that opens for
  // no one, and no later check can undo it. The start cell is read off the
  // pattern itself, so the stored cell and the stored wrap cannot disagree.
  const uint8_t startCell = patternlock::startCellOf(pattern);
  if (!patternlock::isWellFormed(pattern, startCell, kPatternMinPoints, kPatternMaxPoints)) {
    LOG_ERR(kLogTag, "refusing a pattern this device's gate cannot produce");
    return false;
  }

  if (isProvisioned() && !force) {
    // Overwriting a wrap makes every asset already on the card unreadable for
    // ever. That needs to be asked for explicitly, not defaulted into.
    LOG_ERR(kLogTag, "already provisioned; refusing to overwrite the wrap");
    return false;
  }

  uint8_t secret[kDeviceSecretLen];
  if (!loadOrCreateDeviceSecret(secret)) return false;

  uint8_t kek[kWalletKeyLen];
  const bool derived = deriveKek(pattern, patternLen, secret, sizeof(secret), salt, saltLen, iterations, kek);
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
  // **The wrap is written last, and everything else before it.** `isProvisioned()`
  // reads the wrap alone, so a failure or a power cut anywhere above leaves a
  // device that still reports unprovisioned and can be set up again. Any other
  // order leaves the opposite: a stored wrap and a function that returned false.
  //
  // That used to be the case for the failure counter, which sat after the wrap.
  // It mattered only once this screen could retry: a false return sends the
  // rider back to pick a new pattern, the next `provision()` sees a wrap and
  // refuses without `force`, and `goToWallet()` then routes past the setup offer
  // for ever -- a device provisioned under the one pattern it had just told the
  // rider had failed, with no way to unlock and no way to re-enroll. Found in
  // review, 2026-11-26.
  const bool stored = prefs.putBytes(kKeySalt, salt, saltLen) == saltLen && prefs.putUInt(kKeyIters, iterations) > 0 &&
                      prefs.putUChar(kKeyPatternStart, startCell) > 0 && prefs.putUChar(kKeyFails, 0) > 0 &&
                      prefs.putBytes(kKeyWrap, wrapped, sizeof(wrapped)) == sizeof(wrapped);
  prefs.end();
  secureWipe(wrapped, sizeof(wrapped));
  if (!stored) {
    LOG_ERR(kLogTag, "storing the wrap failed");
    return false;
  }
  LOG_INF(kLogTag, "provisioned: %u PBKDF2 iterations, %u-byte salt, start cell %u, wrap stored",
          static_cast<unsigned>(iterations), static_cast<unsigned>(saltLen), static_cast<unsigned>(startCell));
  return true;
}

bool KeyStore::unwrap(const char* pattern, const size_t patternLen, uint8_t out[kWalletKeyLen]) {
  if (pattern == nullptr || out == nullptr || patternLen < kPatternMinPoints || patternLen > kPatternMaxPoints)
    return false;

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
  const bool derived = deriveKek(pattern, patternLen, secret, sizeof(secret), salt, sizeof(salt), iters, kek);
  secureWipe(secret, sizeof(secret));
  if (!derived) {
    secureWipe(wrapped, sizeof(wrapped));
    return false;
  }

  const bool ok = gcmUnwrapKey(kek, wrapped, sizeof(wrapped), out);
  secureWipe(kek, sizeof(kek));
  secureWipe(wrapped, sizeof(wrapped));
  // A failure is a wrong pattern or an altered wrap, and GCM cannot say which. The
  // caller counts it against the rate limiter either way.
  return ok;
}

UnlockResult KeyStore::tryUnlock(const char* patternText, uint32_t& unwrapMicros, uint8_t& failures, uint32_t& waitMs) {
  unwrapMicros = 0;
  waitMs = 0;
  failures = KeyStore::failures();

  char pattern[kPatternBufBytes];
  size_t patternLen = 0;
  if (!normalisePattern(patternText, pattern, sizeof(pattern), patternLen)) {
    // Not a pattern at all. Refused without touching the counter: a typo must not
    // spend one of ten attempts.
    secureWipe(pattern, sizeof(pattern));
    return UnlockResult::Malformed;
  }
  if (!isProvisioned()) {
    secureWipe(pattern, sizeof(pattern));
    return UnlockResult::NotProvisioned;
  }
  if (isLockedOut(failures)) {
    secureWipe(pattern, sizeof(pattern));
    return UnlockResult::LockedOut;
  }
  // The delay is re-armed from the persisted count every time an unlock path starts,
  // so a reboot cannot skip it, and the gate is shared with the pattern screen.
  Session& session = Session::instance();
  session.armRetryDelay(failures);
  waitMs = session.retryWaitMs();
  if (waitMs > 0) {
    secureWipe(pattern, sizeof(pattern));
    return UnlockResult::Waiting;
  }

  uint8_t key[kWalletKeyLen];
  const uint32_t startedUs = micros();
  const bool ok = unwrap(pattern, patternLen, key);
  unwrapMicros = micros() - startedUs;
  secureWipe(pattern, sizeof(pattern));

  if (!ok) {
    secureWipe(key, sizeof(key));
    recordFailure();
    failures = KeyStore::failures();
    session.armRetryDelay(failures);
    waitMs = session.retryWaitMs();
    return isLockedOut(failures) ? UnlockResult::LockedOut : UnlockResult::BadPattern;
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
  // The start cell goes with the wrap it belongs to. Leaving it behind would make
  // the next gate open on the old pattern's cell while the new wrap expects
  // another, which is the same permanent lockout provisioning refuses up front.
  prefs.remove(kKeyPatternStart);
  prefs.putUChar(kKeyFails, 0);
  prefs.end();
  LOG_INF(kLogTag, "wrap forgotten; the device secret stays");
}

}  // namespace wallet
