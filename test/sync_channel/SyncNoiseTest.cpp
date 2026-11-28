// SyncNoise.h against the published Noise test vectors.
//
// The vectors are cacophony's (github.com/haskell-cryptography/cacophony,
// vectors/cacophony.txt, public domain), the only two entries in that file for
// the protocol names the sync channel uses. Cacophony is a Haskell
// implementation with no code in common with this header or with OpenSSL, so a
// byte-exact match on every message and on the handshake hash is the transcript
// check the T-2098 plan asks for.
//
// Each vector runs both roles: our initiator must write the published bytes and
// our responder must read them (and the other way round for message 2), then
// four transport messages alternate directions after the split.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "SyncCryptoHost.h"
#include "SyncNoise.h"

namespace {

using syncnoise::CipherState;
using syncnoise::Handshake;
using syncnoise::Pattern;
using syncnoise::Role;

std::vector<uint8_t> hex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
  return out;
}

struct Vector {
  Pattern pattern;
  const char* prologue;
  const char* psk;  // nullptr for IK
  const char* initStatic;
  const char* initEphemeral;
  const char* respStatic;
  const char* respEphemeral;
  const char* handshakeHash;
  // payload, ciphertext; 0 and 1 are the handshake, the rest transport,
  // alternating initiator -> responder first.
  const char* messages[6][2];
};

// Copied verbatim from cacophony.txt, fetched 2026-11-28.
const Vector kIK = {
    Pattern::IK,
    "4a6f686e2047616c74",
    nullptr,
    "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1",
    "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a",
    "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893",
    "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b",
    "669c8640d9e42a3cda2f232f78597ceefb01daa6e3df81181ccce6fc6b5026bf",
    {
        {"4c756477696720766f6e204d69736573",
         "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79444e417bc55c7a8166c993356c1be41ef67818a292426f3"
         "0"
         "1556c7f26b21d25ddb097153891a9a956cff47b83e63ad8d701c1342c209cff1ca5ecd43402762ac249e3bd3a4c0a145fe07cb5dae28e"
         "a"
         "13a3"},
        {"4d757272617920526f746862617264",
         "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843af2ccf9972e22afc67aeafcd25162f7f98c363b7762e3"
         "e"
         "4cb7d272e39f27a5"},
        {"462e20412e20486179656b", "66acfc92e3197de166809e6d4d5d003dcc819a84bc3522ca53c9d9"},
        {"4361726c204d656e676572", "71f89aa6533a6de70b0826864dd75f60806ee40170c16290189eb3"},
        {"4a65616e2d426170746973746520536179", "4795a3423550c8bf00386bd496a3e2c76c10669d2a75ab8f79b5094c5412a25705"},
        {"457567656e2042f6686d20766f6e2042617765726b",
         "aa0bb39097555c918e40be82abc2b909eb79d9eb87adb07e268fc37323a6cf904fd01fb391"},
    },
};

const Vector kIKpsk2 = {
    Pattern::IKpsk2,
    "4a6f686e2047616c74",
    "54686973206973206d7920417573747269616e20706572737065637469766521",
    "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1",
    "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a",
    "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893",
    "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b",
    "48c3f1eb397c797af6b139032c6074cb41282ff033ee25d5aee4a4badd3a2c3f",
    {
        {"4c756477696720766f6e204d69736573",
         "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79445a1baf339186d7867062b0dd31f5de322d370165d2179"
         "3"
         "9b2b7ed9e2bae7840458ea65c2ce1554e0e8077fe7334c4bd514c658fd04d97b86d216f58f59714a7e24144155045c8f36bed85a2cb0f"
         "d"
         "9af7"},
        {"4d757272617920526f746862617264",
         "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088432fe8d675e949799d2a968211a66cc7ba2b0f4cad8eda3"
         "c"
         "2bd37eae737bbf88"},
        {"462e20412e20486179656b", "38cbf3835c78b7802df0dd0d9640ff8575debd0cb61cfb52590da1"},
        {"4361726c204d656e676572", "ffc365ff6955fc66c5d809b31ac995f812c5770bd29f0df12f0ffd"},
        {"4a65616e2d426170746973746520536179", "a7f2828174c4ead2491ffeb77c41c890fd1229ee36f23ccb9066a795daff1cad6b"},
        {"457567656e2042f6686d20766f6e2042617765726b",
         "3b86963d5c0e6e3b4153d77153836606393eefc4e5e6dae97796552d1265839ee3825cb80c"},
    },
};

void runVector(const Vector& v) {
  const auto& c = synchost::crypto();
  const auto prologue = hex(v.prologue);
  const auto psk = v.psk ? hex(v.psk) : std::vector<uint8_t>(32, 0);
  const auto is = hex(v.initStatic), ie = hex(v.initEphemeral), rsPriv = hex(v.respStatic), re = hex(v.respEphemeral);
  uint8_t rsPub[32];
  ASSERT_TRUE(c.pubFromPriv(rsPriv.data(), rsPub));

  Handshake init, resp;
  ASSERT_TRUE(init.initialize(&c, v.pattern, Role::Initiator, prologue.data(), prologue.size(), is.data(), rsPub,
                              psk.data(), ie.data()));
  ASSERT_TRUE(resp.initialize(&c, v.pattern, Role::Responder, prologue.data(), prologue.size(), rsPriv.data(), nullptr,
                              psk.data(), re.data()));

  uint8_t buf[512], pl[512];
  size_t n = 0, pn = 0;

  // Message 1: initiator writes the published bytes, responder reads them.
  auto p0 = hex(v.messages[0][0]);
  ASSERT_TRUE(init.writeMessage1(p0.data(), p0.size(), buf, sizeof(buf), n));
  EXPECT_EQ(std::vector<uint8_t>(buf, buf + n), hex(v.messages[0][1]));
  ASSERT_TRUE(resp.readMessage1(buf, n, pl, sizeof(pl), pn));
  EXPECT_EQ(std::vector<uint8_t>(pl, pl + pn), p0);
  uint8_t isPub[32];
  ASSERT_TRUE(c.pubFromPriv(is.data(), isPub));
  EXPECT_EQ(0, memcmp(resp.remoteStatic(), isPub, 32));

  // Message 2.
  auto p1 = hex(v.messages[1][0]);
  ASSERT_TRUE(resp.writeMessage2(p1.data(), p1.size(), buf, sizeof(buf), n));
  EXPECT_EQ(std::vector<uint8_t>(buf, buf + n), hex(v.messages[1][1]));
  ASSERT_TRUE(init.readMessage2(buf, n, pl, sizeof(pl), pn));
  EXPECT_EQ(std::vector<uint8_t>(pl, pl + pn), p1);

  CipherState iSend, iRecv, rSend, rRecv;
  ASSERT_TRUE(init.split(iSend, iRecv));
  ASSERT_TRUE(resp.split(rSend, rRecv));
  EXPECT_EQ(std::vector<uint8_t>(init.handshakeHash(), init.handshakeHash() + 32), hex(v.handshakeHash));
  EXPECT_EQ(std::vector<uint8_t>(resp.handshakeHash(), resp.handshakeHash() + 32), hex(v.handshakeHash));

  // Transport: 2 and 4 initiator -> responder, 3 and 5 back.
  for (int i = 2; i < 6; ++i) {
    const bool fromInit = (i % 2) == 0;
    CipherState& tx = fromInit ? iSend : rSend;
    CipherState& rx = fromInit ? rRecv : iRecv;
    auto p = hex(v.messages[i][0]);
    ASSERT_TRUE(tx.encryptWithAd(c, nullptr, 0, p.data(), p.size(), buf));
    EXPECT_EQ(std::vector<uint8_t>(buf, buf + p.size() + 16), hex(v.messages[i][1])) << "message " << i;
    ASSERT_TRUE(rx.decryptWithAd(c, nullptr, 0, buf, p.size() + 16, pl));
    EXPECT_EQ(std::vector<uint8_t>(pl, pl + p.size()), p);
  }
}

TEST(SyncNoiseVectors, IK) { runVector(kIK); }
TEST(SyncNoiseVectors, IKpsk2) { runVector(kIKpsk2); }

// --- Properties the channel leans on, beyond the vectors -------------------

struct Pair {
  uint8_t devPriv[32], devPub[32], phonePriv[32];
};

Pair makeKeys() {
  const auto& c = synchost::crypto();
  Pair p{};
  c.random(p.devPriv, 32);
  c.random(p.phonePriv, 32);
  c.pubFromPriv(p.devPriv, p.devPub);
  return p;
}

// A wrong psk is not caught by the responder at message 1 -- the psk enters
// only at message 2 -- but the initiator cannot read message 2 and, had it
// gone on, neither side would share a transport key. This is why the device
// stores a pairing only on the first transport message it can open
// (SyncChannel.h), never on message 1.
TEST(SyncNoiseProperties, WrongPskFailsAtMessageTwo) {
  const auto& c = synchost::crypto();
  Pair k = makeKeys();
  uint8_t pskA[32], pskB[32];
  c.random(pskA, 32);
  c.random(pskB, 32);
  Handshake init, resp;
  ASSERT_TRUE(init.initialize(&c, Pattern::IKpsk2, Role::Initiator, nullptr, 0, k.phonePriv, k.devPub, pskA));
  ASSERT_TRUE(resp.initialize(&c, Pattern::IKpsk2, Role::Responder, nullptr, 0, k.devPriv, nullptr, pskB));
  uint8_t buf[256], pl[256];
  size_t n = 0, pn = 0;
  ASSERT_TRUE(init.writeMessage1(nullptr, 0, buf, sizeof(buf), n));
  ASSERT_TRUE(resp.readMessage1(buf, n, pl, sizeof(pl), pn));
  ASSERT_TRUE(resp.writeMessage2(nullptr, 0, buf, sizeof(buf), n));
  EXPECT_FALSE(init.readMessage2(buf, n, pl, sizeof(pl), pn));
}

// A message 1 for another device's key does not open.
TEST(SyncNoiseProperties, WrongResponderKeyFailsAtMessageOne) {
  const auto& c = synchost::crypto();
  Pair k = makeKeys();
  Pair other = makeKeys();
  Handshake init, resp;
  ASSERT_TRUE(init.initialize(&c, Pattern::IK, Role::Initiator, nullptr, 0, k.phonePriv, other.devPub, nullptr));
  ASSERT_TRUE(resp.initialize(&c, Pattern::IK, Role::Responder, nullptr, 0, k.devPriv, nullptr, nullptr));
  uint8_t buf[256], pl[256];
  size_t n = 0, pn = 0;
  ASSERT_TRUE(init.writeMessage1(nullptr, 0, buf, sizeof(buf), n));
  EXPECT_FALSE(resp.readMessage1(buf, n, pl, sizeof(pl), pn));
}

// Every flipped bit of message 1 is refused.
TEST(SyncNoiseProperties, TamperedMessageOneRefusedAtEveryByte) {
  const auto& c = synchost::crypto();
  Pair k = makeKeys();
  Handshake init;
  ASSERT_TRUE(init.initialize(&c, Pattern::IK, Role::Initiator, nullptr, 0, k.phonePriv, k.devPub, nullptr));
  uint8_t msg[256], pl[256];
  size_t n = 0, pn = 0;
  const uint8_t payload[4] = {1, 2, 3, 4};
  ASSERT_TRUE(init.writeMessage1(payload, 4, msg, sizeof(msg), n));
  for (size_t i = 0; i < n; ++i) {
    uint8_t bad[256];
    memcpy(bad, msg, n);
    bad[i] ^= 0x01;
    Handshake resp;
    ASSERT_TRUE(resp.initialize(&c, Pattern::IK, Role::Responder, nullptr, 0, k.devPriv, nullptr, nullptr));
    EXPECT_FALSE(resp.readMessage1(bad, n, pl, sizeof(pl), pn)) << "byte " << i;
  }
}

// A low-order public key makes X25519 output zero; dh() must refuse it rather
// than let a handshake continue on a key an attacker knows.
TEST(SyncNoiseProperties, ZeroPublicKeyRefused) {
  const auto& c = synchost::crypto();
  Pair k = makeKeys();
  uint8_t zero[32] = {};
  uint8_t out[32];
  EXPECT_FALSE(c.dh(k.devPriv, zero, out));
}

// Out of order: each method only in its own step, and a failed step kills the
// handshake for good.
TEST(SyncNoiseProperties, StepsRunOnlyInOrder) {
  const auto& c = synchost::crypto();
  Pair k = makeKeys();
  Handshake resp;
  ASSERT_TRUE(resp.initialize(&c, Pattern::IK, Role::Responder, nullptr, 0, k.devPriv, nullptr, nullptr));
  uint8_t buf[256];
  size_t n = 0;
  EXPECT_FALSE(resp.writeMessage2(nullptr, 0, buf, sizeof(buf), n));
  CipherState a, b;
  EXPECT_FALSE(resp.split(a, b));
  uint8_t pl[8];
  size_t pn = 0;
  EXPECT_FALSE(resp.readMessage1(buf, 10, pl, sizeof(pl), pn));  // dead now
}

}  // namespace
