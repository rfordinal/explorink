#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "WalletCrypto.h"

// Where the wallet key lives while the wallet is unlocked, and the one way out of
// it: a copy, taken under the same lock that clear() takes. T-2104, item 1.
//
// **Why a copy and not a pointer.** The accessor this replaces, `Session::key()`,
// handed out a pointer into the holder's own array, and `clear()` wipes that array
// in place. On a dual-core board (`env:x4pro`, `env:t5s3pro`: NimBLE on core 0,
// the Arduino task on core 1, `sdkconfig.x4pro` leaves CONFIG_FREERTOS_UNICORE
// unset) a sleep or `CMD:WALLETLOCK` on one core can wipe the bytes while the
// other core is inside `mbedtls_aes_setkey_enc()` with that pointer. A null check
// before the call does not close it: the wipe lands after the check. The asset
// sealer then enciphers the rest of the file under a half-zeroed key, both
// end-of-transfer checks pass because they are over the plaintext, and the file
// is renamed over the previous good one -- a rider's document, gone, with `OK` on
// the wire.
//
// With a copy, a reader holds either the whole key or nothing. A clear() that
// lands after the copy is taken does not touch the copy; the next chunk asks
// again and is refused, so the transfer ends instead of finishing wrong.
//
// **Why no lock across the AES.** The lock is a portMUX critical section, and
// the AES call cannot run inside one: on both the C3 and the S3 the ESP-IDF AES
// driver is the DMA one, and it takes a blocking lock for the peripheral
// (framework-espidf 5.5.2, components/mbedtls/port/aes/dma/esp_aes.c, AES_LOCK()
// -> esp_crypto_sha_aes_lock_acquire() -> _lock_acquire(), esp_crypto_lock.c).
// Blocking with interrupts masked is not allowed. So the lock covers the 32-byte
// memcpy and nothing else, and the AES runs on the copy.
//
// **One accessor, on purpose.** There is `has()` -- a cheap "is there a key now",
// for gates that only decide whether to start -- and `copyKey()`. A second way to
// reach the bytes is how the pointer survives. Callers take a `KeyCopy` below
// rather than calling copyKey() with a buffer of their own, so the wipe is in a
// destructor and not in every caller's memory.
//
// Header-only and templated on the lock so the host tests can drive it with a
// std::mutex and two real threads (test/wallet/WalletKeySlotTest.cpp). The
// device binds a portMUX spinlock (WalletCryptoDevice.h).
namespace wallet {

template <class Lock>
class KeySlot {
 public:
  // A hint, read without the lock: true can be stale by the time the caller acts
  // on it. Fine for "should this start at all"; never a licence to use the key --
  // that is copyKey(), which answers again under the lock.
  bool has() const { return has_.load(std::memory_order_acquire); }

  void set(const uint8_t key[kWalletKeyLen]) {
    if (key == nullptr) return;
    lock_.lock();
    std::memcpy(key_, key, kWalletKeyLen);
    ++generation_;
    has_.store(true, std::memory_order_release);
    lock_.unlock();
  }

  // True when there was a key to wipe. The wipe goes through a volatile pointer
  // (secureWipe) so it cannot be optimised away.
  bool clear() {
    lock_.lock();
    const bool had = has_.load(std::memory_order_relaxed);
    secureWipe(key_, sizeof(key_));
    ++generation_;
    has_.store(false, std::memory_order_release);
    lock_.unlock();
    return had;
  }

  // The key into `out`, or false and `out` zeroed. Never a partial copy: the copy
  // and the wipe hold the same lock, so one runs entirely before the other.
  //
  // `generation`, when asked for, is which key this was: it moves on every set()
  // and every clear(). A caller that borrows across many calls -- the asset
  // sealer, once per chunk -- compares it with the one it started under, because
  // "there is a key" is not "it is the same key". Locked, re-provisioned and
  // unlocked between two chunks, the next borrow would otherwise succeed under a
  // different key and seal the rest of the file under it (review, 2026-11-28).
  bool copyKey(uint8_t out[kWalletKeyLen], uint32_t* generation = nullptr) const {
    if (out == nullptr) return false;
    lock_.lock();
    const bool ok = has_.load(std::memory_order_relaxed);
    if (ok) std::memcpy(out, key_, kWalletKeyLen);
    const uint32_t gen = generation_;
    lock_.unlock();
    if (!ok) secureWipe(out, kWalletKeyLen);
    if (generation != nullptr) *generation = gen;
    return ok;
  }

 private:
  mutable Lock lock_;
  uint8_t key_[kWalletKeyLen] = {0};
  // Lock only. Wraps after 2^32 lock/unlock cycles, which no device reaches.
  uint32_t generation_ = 0;
  // Atomic so has() may be read off the lock from either core. Written only with
  // the lock held, so the lock orders it against key_.
  std::atomic<bool> has_{false};
};

// A borrowed copy of the key, wiped when it goes out of scope.
//
// The rule it carries: **the copy lives for one unit of work** -- one chunk of a
// transfer, one manifest seal, one page decrypt -- and the next unit borrows
// again. Holding one across a transfer would bring back the bug this exists to
// remove, one level up: a key that survives a lock.
//
// 32 bytes on the caller's stack. The receiver's callers run on the NimBLE host
// task's 4 KB stack; 32 bytes there is the same order as the IV next to it.
class KeyCopy {
 public:
  template <class Source>
  explicit KeyCopy(const Source& source) : ok_(source.copyKey(bytes_, &generation_)) {}
  ~KeyCopy() { secureWipe(bytes_, sizeof(bytes_)); }

  KeyCopy(const KeyCopy&) = delete;
  KeyCopy& operator=(const KeyCopy&) = delete;

  explicit operator bool() const { return ok_; }
  // nullptr when there was no key, so a caller that forgets to test still cannot
  // run a cipher over 32 zero bytes: the crypto primitives refuse a null key
  // (WalletCryptoDevice.cpp). readPlaneWindow() does NOT -- there null means
  // "cleartext asset" -- so a read-path caller must test before passing it.
  const uint8_t* bytes() const { return ok_ ? bytes_ : nullptr; }
  // Which key this was (KeySlot::copyKey). Compare, never interpret.
  uint32_t generation() const { return generation_; }

 private:
  uint8_t bytes_[kWalletKeyLen];
  uint32_t generation_ = 0;
  const bool ok_;
};

}  // namespace wallet
