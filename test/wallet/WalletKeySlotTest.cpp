// The wallet key's holder and the one way out of it -- T-2104, item 1.
//
// What these tests are for: the **contract** a caller relies on. A copy is the
// whole key or nothing, a copy after clear() fails, and a KeyCopy wipes what it
// held. The two sealer tests that prove a failing copy ends a transfer rather
// than finishing it wrong sit next to the sealers they drive
// (WalletIngestTest.cpp, WalletManifestSealTest.cpp).
//
// The host binds the lock to std::mutex; the device binds a portMUX spinlock
// (WalletCryptoDevice.h). So the race test below proves KeySlot takes its lock
// around every touch of the bytes. It cannot prove that portENTER_CRITICAL is
// exclusive across an S3's two cores -- that is ESP-IDF's claim, not ours.

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include "WalletKeySlot.h"

namespace {

using HostSlot = wallet::KeySlot<std::mutex>;

void fillKey(uint8_t out[wallet::kWalletKeyLen], uint8_t seed) {
  // No zero byte anywhere, so a partial wipe always shows up as a zero.
  for (size_t i = 0; i < wallet::kWalletKeyLen; ++i) out[i] = static_cast<uint8_t>(seed + i) | 0x01u;
}

bool allZero(const uint8_t* p, size_t len) {
  for (size_t i = 0; i < len; ++i)
    if (p[i] != 0) return false;
  return true;
}

}  // namespace

TEST(WalletKeySlot, AnEmptySlotRefusesAndZeroesTheOutput) {
  HostSlot slot;
  uint8_t out[wallet::kWalletKeyLen];
  std::memset(out, 0xAA, sizeof(out));
  EXPECT_FALSE(slot.has());
  EXPECT_FALSE(slot.copyKey(out));
  // A caller that ignored the return would hold zeros, not stale bytes from a
  // buffer it reused.
  EXPECT_TRUE(allZero(out, sizeof(out)));
}

TEST(WalletKeySlot, CopyReturnsTheKeyThatWasSet) {
  HostSlot slot;
  uint8_t key[wallet::kWalletKeyLen];
  fillKey(key, 7);
  slot.set(key);
  uint8_t out[wallet::kWalletKeyLen] = {0};
  EXPECT_TRUE(slot.has());
  ASSERT_TRUE(slot.copyKey(out));
  EXPECT_EQ(std::memcmp(out, key, sizeof(key)), 0);
}

TEST(WalletKeySlot, CopyFailsAfterClear) {
  // The plan's done test, in its plainest form.
  HostSlot slot;
  uint8_t key[wallet::kWalletKeyLen];
  fillKey(key, 7);
  slot.set(key);
  EXPECT_TRUE(slot.clear());
  EXPECT_FALSE(slot.has());
  uint8_t out[wallet::kWalletKeyLen];
  std::memset(out, 0xAA, sizeof(out));
  EXPECT_FALSE(slot.copyKey(out));
  EXPECT_TRUE(allZero(out, sizeof(out)));
  // A second clear has nothing to wipe, which is what Session::clear() logs on.
  EXPECT_FALSE(slot.clear());
}

TEST(WalletKeySlot, ACopyTakenBeforeClearSurvivesIt) {
  // The point of a copy over a pointer: clear() wipes the slot, not what a
  // caller already borrowed for the chunk it is enciphering.
  HostSlot slot;
  uint8_t key[wallet::kWalletKeyLen];
  fillKey(key, 7);
  slot.set(key);
  const wallet::KeyCopy borrowed(slot);
  ASSERT_TRUE(borrowed);
  slot.clear();
  EXPECT_EQ(std::memcmp(borrowed.bytes(), key, sizeof(key)), 0);
  // And the next borrow is refused.
  const wallet::KeyCopy next(slot);
  EXPECT_FALSE(next);
  EXPECT_EQ(next.bytes(), nullptr);
}

TEST(WalletKeySlot, AKeyCopyWipesItselfOnScopeExit) {
  // Read through placement storage, so the bytes are inspected after the
  // destructor without touching an object whose lifetime ended.
  HostSlot slot;
  uint8_t key[wallet::kWalletKeyLen];
  fillKey(key, 7);
  slot.set(key);
  alignas(wallet::KeyCopy) unsigned char storage[sizeof(wallet::KeyCopy)];
  auto* copy = new (storage) wallet::KeyCopy(slot);
  ASSERT_TRUE(*copy);
  const auto* bytes = reinterpret_cast<const uint8_t*>(storage);
  EXPECT_FALSE(allZero(bytes, wallet::kWalletKeyLen));
  copy->~KeyCopy();
  EXPECT_TRUE(allZero(bytes, wallet::kWalletKeyLen));
}

TEST(WalletKeySlot, ACopyIsNeverTornByAConcurrentClear) {
  // The race from the review: one thread locks and unlocks the wallet as fast as
  // it can, the other borrows the key as fast as it can. Every borrow must be
  // either the whole key or a refusal -- never the half-zeroed key that the old
  // pointer accessor could hand mbedtls on a dual-core board.
  HostSlot slot;
  uint8_t key[wallet::kWalletKeyLen];
  fillKey(key, 7);
  std::atomic<bool> stop{false};
  std::thread locker([&] {
    while (!stop.load()) {
      slot.set(key);
      slot.clear();
    }
  });

  size_t whole = 0;
  size_t refused = 0;
  size_t torn = 0;
  // At least 200,000 borrows, and on until both outcomes were seen -- a host
  // that schedules the locker late must not turn this into a vacuous pass.
  for (long i = 0; i < 20000000 && (i < 200000 || whole == 0 || refused == 0); ++i) {
    const wallet::KeyCopy borrowed(slot);
    if (!borrowed) {
      ++refused;
    } else if (std::memcmp(borrowed.bytes(), key, sizeof(key)) == 0) {
      ++whole;
    } else {
      ++torn;
    }
  }
  stop.store(true);
  locker.join();

  EXPECT_EQ(torn, 0u);
  // Both outcomes have to have actually happened, or the test proved nothing
  // about the interleaving it claims to cover.
  EXPECT_GT(whole, 0u);
  EXPECT_GT(refused, 0u);
}
