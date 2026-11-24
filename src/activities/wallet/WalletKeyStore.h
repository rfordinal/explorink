#pragma once

#include <cstddef>
#include <cstdint>

#include "WalletCrypto.h"

// What NVS holds for the wallet, and nothing more.
//
//   dsecret  32 random bytes, made once on this device, never leaves it
//   salt     16 bytes, from provisioning
//   iters    PBKDF2 iteration count, from provisioning
//   pstart   the cell the stored pattern starts on, 0..15
//   wrap     nonce || AES-256-GCM(K under the KEK) || tag  -- 60 bytes
//   fails    consecutive wrong patterns, persisted so a power cycle does not reset it
//
// **K is never in NVS unwrapped and never on the card.** The wrap needs the KEK,
// the KEK needs the pattern and the device secret, and the pattern is only ever in
// RAM for the length of one unwrap.
//
// `pstart` is the one entry here that is not a secret and still has to be stored.
// The gate draws its cursor on the start cell before the first press, so a
// shoulder has it for free -- but the wrap says nothing about the path that made
// it, and a gate opened on the wrong cell is a wallet nobody can ever open
// (`components/lock/PatternLock.h`, isWellFormed).
//
// NVS is plaintext flash. Somebody with the device and a cable can read all of the
// above, and closing that needs flash encryption plus an eFuse burn -- out of
// scope, see ../../../docs/wallet-crypto.md, "The threat boundary". Nothing in the
// product text may claim otherwise.
namespace wallet {

struct KeyStore {
  // True when a wrap exists, i.e. the device has been provisioned for a wallet.
  static bool isProvisioned();

  // Generates the device secret if this device has none, derives the KEK from
  // `pattern`, wraps `key` and stores everything. Refuses an existing provisioning
  // unless `force`, because overwriting the wrap makes every asset on the card
  // unreadable for ever.
  //
  // It also refuses a `pattern` the gate could not produce. That check lives here
  // and nowhere else: this is the moment the device decides what it will later
  // demand, and a wrap made from an unwalkable path is a wallet that opens for
  // nobody with no way back.
  static bool provision(const uint8_t key[kWalletKeyLen], const char* pattern, size_t patternLen, const uint8_t* salt,
                        size_t saltLen, uint32_t iterations, bool force);

  // Derives the KEK from `pattern` and unwraps K into `out`.
  //
  // Failure is either a wrong pattern or a wrap that does not authenticate, and
  // GCM cannot tell those apart -- which is correct. The caller counts the failure.
  static bool unwrap(const char* pattern, size_t patternLen, uint8_t out[kWalletKeyLen]);

  // One unlock attempt, whole: normalise the pattern, check the lockout and the
  // rate limiter, unwrap, and on success put K in the session and clear the
  // failure count. On failure it records the failure and arms the delay.
  //
  // **The only unlock path there is.** The gate screen's verifier calls it and so
  // does CMD:WALLETUNLOCK, which is what makes the serial command a test *driver*
  // rather than a test *bypass*: a command that installed K directly would verify
  // nothing about the thing it was meant to verify.
  //
  // `unwrapMicros` is how long the unwrap took (PBKDF2 dominates it), `failures` the
  // count after the attempt, `waitMs` the delay still to serve when the result is
  // Waiting.
  static UnlockResult tryUnlock(const char* patternText, uint32_t& unwrapMicros, uint8_t& failures, uint32_t& waitMs);

  // The cell the stored pattern starts on, or `kPatternNoStart` when this device
  // has none. The gate needs it before it can draw a single frame.
  static uint8_t patternStart();

  // The rate limiter's state. Persisted, so pulling the power does not buy a fresh
  // set of guesses.
  static uint8_t failures();
  static void recordFailure();
  static void clearFailures();

  // Forgets everything: the wrap, the salt, the iteration count, the start cell
  // and the failure count. The device secret **stays**, because it is this device's identity and a
  // new provisioning should not silently change it.
  static void forgetWrap();

  static uint32_t iterations();
};

}  // namespace wallet
