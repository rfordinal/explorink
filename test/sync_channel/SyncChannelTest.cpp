// SyncChannel.h: every refusal the device half of T-2098 makes, driven from a
// phone played by SyncNoise.h's initiator. The initiator itself is checked
// against the cacophony vectors in SyncNoiseTest.cpp, and against an independent
// implementation by scripts/sync_channel_phone.py.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "SyncChannel.h"
#include "SyncCryptoHost.h"

namespace {

using namespace syncchannel;
using syncnoise::CipherState;
using syncnoise::Handshake;
using syncnoise::Pattern;
using syncnoise::Role;

std::vector<uint8_t> unb64(const std::string& s) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<uint8_t> out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    const int v = val(c);
    if (v < 0) break;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>(acc >> bits));
    }
  }
  return out;
}

std::vector<uint8_t> fromHex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
  return out;
}

struct Store {
  int calls = 0;
  bool accept = true;
  PairedPhones last;
  static bool persist(void* ctx, const PairedPhones& t) {
    auto* s = static_cast<Store*>(ctx);
    ++s->calls;
    if (s->accept) s->last = t;
    return s->accept;
  }
};

// The phone, as T-174 will write it.
struct Phone {
  uint8_t priv[32];
  uint8_t pub[32];
  uint8_t devicePub[32];
  uint8_t psk[32];
  Handshake hs;
  CipherState send, recv;

  Phone() {
    const auto& c = synchost::crypto();
    c.random(priv, 32);
    c.pubFromPriv(priv, pub);
  }

  bool scan(const std::string& qr) {
    const std::string prefix = kQrPrefix;
    if (qr.rfind(prefix, 0) != 0 || qr.size() != prefix.size() + 128) return false;
    auto pubBytes = fromHex(qr.substr(prefix.size(), 64));
    auto pskBytes = fromHex(qr.substr(prefix.size() + 64, 64));
    memcpy(devicePub, pubBytes.data(), 32);
    memcpy(psk, pskBytes.data(), 32);
    return true;
  }

  std::vector<uint8_t> hello(Pattern p) {
    hs.initialize(&synchost::crypto(), p, Role::Initiator, reinterpret_cast<const uint8_t*>(kPrologue),
                  sizeof(kPrologue) - 1, priv, devicePub, psk);
    uint8_t buf[256];
    size_t n = 0;
    EXPECT_TRUE(hs.writeMessage1(nullptr, 0, buf, sizeof(buf), n));
    std::vector<uint8_t> f{p == Pattern::IKpsk2 ? kOpPair : kOpOpen, kWireVersion};
    f.insert(f.end(), buf, buf + n);
    return f;
  }

  bool finish(const std::string& line) {
    if (line.rfind("NX2 ", 0) != 0) return false;
    auto msg = unb64(line.substr(4));
    uint8_t pl[8];
    size_t pn = 0;
    return hs.readMessage2(msg.data(), msg.size(), pl, sizeof(pl), pn) && hs.split(send, recv);
  }

  std::vector<uint8_t> seal(const std::vector<uint8_t>& inner) {
    std::vector<uint8_t> f(1 + inner.size() + 16);
    f[0] = kOpSealed;
    EXPECT_TRUE(send.encryptWithAd(synchost::crypto(), nullptr, 0, inner.data(), inner.size(), f.data() + 1));
    return f;
  }

  std::string open(const std::string& line) {
    if (line.rfind("NXS ", 0) != 0) return "<not sealed: " + line + ">";
    auto ct = unb64(line.substr(4));
    std::vector<uint8_t> pt(ct.size());
    if (!recv.decryptWithAd(synchost::crypto(), nullptr, 0, ct.data(), ct.size(), pt.data())) return "<tag failed>";
    return std::string(pt.begin(), pt.begin() + (ct.size() - 16));
  }
};

struct Rig {
  Store store;
  Channel ch;
  uint8_t devPriv[32];
  uint32_t now = 1000;

  Rig() {
    synchost::crypto().random(devPriv, 32);
    EXPECT_TRUE(ch.begin(&synchost::crypto(), devPriv, PairedPhones{}, PairingSink{&store, &Store::persist}));
  }

  Outcome send(const std::vector<uint8_t>& f) { return ch.onFrame(f.data(), f.size(), now); }
  std::string line() const { return ch.line(); }

  std::string qr() {
    char buf[kQrTextLen + 1];
    EXPECT_TRUE(ch.qrText(now, buf, sizeof(buf)));
    return buf;
  }

  // The whole pairing, as the rider does it.
  void pair(Phone& p) {
    ASSERT_TRUE(ch.issueCode(now));
    ASSERT_TRUE(p.scan(qr()));
    ASSERT_EQ(send(p.hello(Pattern::IKpsk2)), Outcome::Reply);
    ASSERT_TRUE(p.finish(line())) << line();
    ASSERT_EQ(send(p.seal({kInnerPaired})), Outcome::Reply);
    ASSERT_EQ(p.open(line()), "OK paired");
  }

  void openSession(Phone& p) {
    ASSERT_EQ(send(p.hello(Pattern::IK)), Outcome::Reply);
    ASSERT_TRUE(p.finish(line())) << line();
  }
};

const std::vector<uint8_t> kBeginFrame = {0x01, 0x05, 0, 0, 0, 1, 2, 3, 4, 3, 'a', '/', 'b'};

// --- Pairing ---------------------------------------------------------------

TEST(SyncChannelPairing, ScanThenPairStoresThePhone) {
  Rig r;
  Phone p;
  r.pair(p);
  EXPECT_EQ(r.store.calls, 1);
  EXPECT_EQ(r.store.last.count, 1);
  EXPECT_EQ(memcmp(r.store.last.keys[0], p.pub, 32), 0);
  EXPECT_EQ(r.ch.table().count, 1);
  // The pairing session is closed: it cannot carry a transfer afterwards.
  EXPECT_EQ(r.send(p.seal(kBeginFrame)), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR no session");
}

TEST(SyncChannelPairing, NoCodeNoPairing) {
  Rig r;
  Phone p;
  ASSERT_TRUE(r.ch.issueCode(r.now));
  ASSERT_TRUE(p.scan(r.qr()));
  r.ch.killCode();  // the screen closed
  EXPECT_EQ(r.send(p.hello(Pattern::IKpsk2)), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR no pairing code");
  EXPECT_EQ(r.store.calls, 0);
}

TEST(SyncChannelPairing, OneAttemptPerCodeEvenAMalformedOne) {
  Rig r;
  Phone p;
  ASSERT_TRUE(r.ch.issueCode(r.now));
  ASSERT_TRUE(p.scan(r.qr()));
  const std::vector<uint8_t> junk = {kOpPair, kWireVersion, 1, 2, 3};
  EXPECT_EQ(r.send(junk), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR short frame");
  EXPECT_FALSE(r.ch.codeAlive(r.now));
  EXPECT_EQ(r.send(p.hello(Pattern::IKpsk2)), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR no pairing code");
}

TEST(SyncChannelPairing, WrongVersionSpendsTheCodeAndSaysVersion) {
  Rig r;
  Phone p;
  ASSERT_TRUE(r.ch.issueCode(r.now));
  ASSERT_TRUE(p.scan(r.qr()));
  auto f = p.hello(Pattern::IKpsk2);
  f[1] = kWireVersion + 1;
  r.send(f);
  EXPECT_EQ(r.line(), "ERR channel version");
  EXPECT_FALSE(r.ch.codeAlive(r.now));
}

TEST(SyncChannelPairing, CodeDiesOnTheClock) {
  Rig r;
  ASSERT_TRUE(r.ch.issueCode(r.now));
  EXPECT_TRUE(r.ch.codeAlive(r.now + kCodeLifetimeMs - 1));
  EXPECT_FALSE(r.ch.codeAlive(r.now + kCodeLifetimeMs));
  char buf[kQrTextLen + 1];
  EXPECT_FALSE(r.ch.qrText(r.now + kCodeLifetimeMs, buf, sizeof(buf)));
}

TEST(SyncChannelPairing, CodeSurvivesMillisWrap) {
  Rig r;
  r.now = 0xFFFFFFFFu - 10;
  ASSERT_TRUE(r.ch.issueCode(r.now));
  EXPECT_TRUE(r.ch.codeAlive(r.now + 100));  // wrapped past zero
  EXPECT_FALSE(r.ch.codeAlive(r.now + kCodeLifetimeMs));
}

TEST(SyncChannelPairing, EveryCodeIsANewSecret) {
  Rig r;
  ASSERT_TRUE(r.ch.issueCode(r.now));
  const std::string a = r.qr();
  ASSERT_TRUE(r.ch.issueCode(r.now));
  const std::string b = r.qr();
  EXPECT_EQ(a.substr(0, sizeof(kQrPrefix) - 1 + 64), b.substr(0, sizeof(kQrPrefix) - 1 + 64));  // same device key
  EXPECT_NE(a, b);                                                                              // different psk
}

// A stranger who reads the device's public key (it is not secret) but not the
// code: message 1 is valid, so the device answers message 2 -- it cannot know
// yet -- but nothing the stranger can send afterwards opens, and nothing is
// stored. This is the reason the store happens on the first sealed frame.
TEST(SyncChannelPairing, APairingWithoutTheCodeIsNeverStored) {
  Rig r;
  Phone honest;
  ASSERT_TRUE(r.ch.issueCode(r.now));
  ASSERT_TRUE(honest.scan(r.qr()));
  Phone stranger;
  memcpy(stranger.devicePub, honest.devicePub, 32);
  synchost::crypto().random(stranger.psk, 32);
  EXPECT_EQ(r.send(stranger.hello(Pattern::IKpsk2)), Outcome::Reply);
  ASSERT_EQ(r.line().rfind("NX2 ", 0), 0u);
  EXPECT_FALSE(stranger.finish(r.line()));
  // The stranger's best guess at a PAIRED frame: keys it derived without the psk.
  std::vector<uint8_t> forged(1 + 1 + 16, 0xAA);
  forged[0] = kOpSealed;
  EXPECT_EQ(r.send(forged), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR bad sealed frame");
  EXPECT_EQ(r.store.calls, 0);
  EXPECT_EQ(r.ch.table().count, 0);
}

TEST(SyncChannelPairing, APairingSessionCarriesNoTransfer) {
  Rig r;
  Phone p;
  ASSERT_TRUE(r.ch.issueCode(r.now));
  ASSERT_TRUE(p.scan(r.qr()));
  r.send(p.hello(Pattern::IKpsk2));
  ASSERT_TRUE(p.finish(r.line()));
  EXPECT_EQ(r.send(p.seal(kBeginFrame)), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR pairing only");
  EXPECT_EQ(r.store.calls, 0);
}

TEST(SyncChannelPairing, StoreFailureRefusesThePairing) {
  Rig r;
  r.store.accept = false;
  Phone p;
  ASSERT_TRUE(r.ch.issueCode(r.now));
  ASSERT_TRUE(p.scan(r.qr()));
  r.send(p.hello(Pattern::IKpsk2));
  ASSERT_TRUE(p.finish(r.line()));
  r.send(p.seal({kInnerPaired}));
  EXPECT_EQ(r.line(), "ERR not stored");
  EXPECT_EQ(r.ch.table().count, 0);
}

TEST(SyncChannelPairing, FullTableIssuesNoCode) {
  Rig r;
  Phone phones[kMaxPhones];
  for (auto& p : phones) r.pair(p);
  EXPECT_EQ(r.ch.table().count, kMaxPhones);
  EXPECT_FALSE(r.ch.issueCode(r.now));
}

TEST(SyncChannelPairing, PairingTwiceIsOneEntry) {
  Rig r;
  Phone p;
  r.pair(p);
  r.pair(p);
  EXPECT_EQ(r.ch.table().count, 1);
}

// --- Sync sessions ---------------------------------------------------------

TEST(SyncChannelSession, PairedPhoneDeliversTransferFramesAndReadsSealedStatus) {
  Rig r;
  Phone p;
  r.pair(p);
  r.openSession(p);
  EXPECT_TRUE(r.ch.sessionOpen());
  ASSERT_EQ(r.send(p.seal(kBeginFrame)), Outcome::DeliverInner);
  EXPECT_EQ(std::vector<uint8_t>(r.ch.inner(), r.ch.inner() + r.ch.innerLen()), kBeginFrame);
  ASSERT_TRUE(r.ch.sealLine("RDY 0 a/b"));
  EXPECT_EQ(p.open(r.line()), "RDY 0 a/b");
  // The longest status line the receiver can send still fits the status buffer.
  const std::string longest(kMaxStatusLineLen, 'x');
  ASSERT_TRUE(r.ch.sealLine(longest.c_str()));
  EXPECT_LE(r.line().size(), kMaxOutLineLen);
  EXPECT_EQ(p.open(r.line()), longest);
}

TEST(SyncChannelSession, UnknownPhoneGetsNoSession) {
  Rig r;
  Phone paired;
  r.pair(paired);
  Phone stranger;
  memcpy(stranger.devicePub, paired.devicePub, 32);
  EXPECT_EQ(r.send(stranger.hello(Pattern::IK)), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR not paired");
  EXPECT_FALSE(r.ch.sessionOpen());
  EXPECT_EQ(r.send(stranger.seal(kBeginFrame)), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR no session");
}

TEST(SyncChannelSession, SealedFrameWithoutSessionRefused) {
  Rig r;
  std::vector<uint8_t> f(1 + kBeginFrame.size() + 16, 0);
  f[0] = kOpSealed;
  r.send(f);
  EXPECT_EQ(r.line(), "ERR no session");
}

// A recorded sync, replayed whole into a fresh connection: message 1 is
// accepted again (it is valid), but the device's ephemeral is new, so the
// recorded sealed frame does not open under the new keys.
TEST(SyncChannelSession, RecordedSyncCannotBeReplayed) {
  Rig r;
  Phone p;
  r.pair(p);
  const auto hello = p.hello(Pattern::IK);
  r.send(hello);
  ASSERT_TRUE(p.finish(r.line()));
  const auto sealed = p.seal(kBeginFrame);
  ASSERT_EQ(r.send(sealed), Outcome::DeliverInner);
  r.ch.onDisconnect();

  EXPECT_EQ(r.send(hello), Outcome::Reply);  // the attacker replays message 1
  ASSERT_EQ(r.line().rfind("NX2 ", 0), 0u);
  EXPECT_EQ(r.send(sealed), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR bad sealed frame");
  EXPECT_FALSE(r.ch.sessionOpen());
}

TEST(SyncChannelSession, RepeatedFrameInsideASessionEndsIt) {
  Rig r;
  Phone p;
  r.pair(p);
  r.openSession(p);
  const auto sealed = p.seal(kBeginFrame);
  ASSERT_EQ(r.send(sealed), Outcome::DeliverInner);
  EXPECT_EQ(r.send(sealed), Outcome::Reply);
  EXPECT_EQ(r.line(), "ERR bad sealed frame");
  EXPECT_FALSE(r.ch.sessionOpen());
}

TEST(SyncChannelSession, TamperedSealedFrameEndsTheSession) {
  Rig r;
  Phone p;
  r.pair(p);
  r.openSession(p);
  auto sealed = p.seal(kBeginFrame);
  sealed[3] ^= 0x40;
  r.send(sealed);
  EXPECT_EQ(r.line(), "ERR bad sealed frame");
  EXPECT_FALSE(r.ch.sessionOpen());
}

TEST(SyncChannelSession, DisconnectEndsTheSession) {
  Rig r;
  Phone p;
  r.pair(p);
  r.openSession(p);
  r.ch.onDisconnect();
  EXPECT_FALSE(r.ch.sessionOpen());
  EXPECT_FALSE(r.ch.sealLine("OK"));
}

TEST(SyncChannelSession, ForgottenPhoneIsRefusedAgain) {
  Rig r;
  Phone p;
  r.pair(p);
  r.openSession(p);
  ASSERT_TRUE(r.ch.forgetPhone(0));
  EXPECT_FALSE(r.ch.sessionOpen());  // forgetting ends a live session too
  r.send(p.hello(Pattern::IK));
  EXPECT_EQ(r.line(), "ERR not paired");
}

TEST(SyncChannelSession, ShutdownWipesEverything) {
  Rig r;
  Phone p;
  r.pair(p);
  ASSERT_TRUE(r.ch.issueCode(r.now));
  r.ch.shutdown();
  char buf[kQrTextLen + 1];
  EXPECT_FALSE(r.ch.qrText(r.now, buf, sizeof(buf)));
  EXPECT_EQ(r.send(p.hello(Pattern::IK)), Outcome::None);
  EXPECT_EQ(r.ch.table().count, 0);
  const uint8_t zero[32] = {};
  EXPECT_EQ(memcmp(r.ch.staticPub(), zero, 32), 0);
}

TEST(SyncChannelSession, PlainTransferOpcodesAreNotTheChannels) {
  Rig r;
  r.send(kBeginFrame);
  EXPECT_EQ(r.line(), "ERR bad channel opcode");
}

// --- Formats ---------------------------------------------------------------

TEST(SyncChannelFormat, TableRoundTripAndRefusals) {
  PairedPhones t;
  uint8_t k[32];
  for (int i = 0; i < 3; ++i) {
    memset(k, i + 1, 32);
    ASSERT_TRUE(t.add(k));
  }
  uint8_t blob[kTableBlobLen];
  const size_t n = t.serialize(blob);
  EXPECT_EQ(n, 2u + 3 * 32);
  PairedPhones u;
  ASSERT_TRUE(u.parse(blob, n));
  EXPECT_EQ(u.count, 3);
  EXPECT_EQ(memcmp(u.keys, t.keys, 3 * 32), 0);
  EXPECT_FALSE(u.parse(blob, n - 1));
  EXPECT_EQ(u.count, 0);  // a failed parse leaves no phones, never half a table
  blob[0] = kTableVersion + 1;
  EXPECT_FALSE(u.parse(blob, n));
  blob[0] = kTableVersion;
  blob[1] = kMaxPhones + 1;
  EXPECT_FALSE(u.parse(blob, 2 + (kMaxPhones + 1) * 32));
  ASSERT_TRUE(t.remove(1));
  memset(k, 3, 32);
  EXPECT_EQ(t.find(k), 1);  // order kept
}

TEST(SyncChannelFormat, QrTextIsAlphanumericModeAndFixedLength) {
  uint8_t pub[32], psk[32];
  for (int i = 0; i < 32; ++i) pub[i] = static_cast<uint8_t>(i * 7), psk[i] = static_cast<uint8_t>(255 - i);
  char buf[kQrTextLen + 1];
  ASSERT_TRUE(buildQrText(pub, psk, buf, sizeof(buf)));
  EXPECT_EQ(strlen(buf), kQrTextLen);
  for (const char* c = buf; *c; ++c) EXPECT_TRUE(strchr("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:", *c)) << *c;
  EXPECT_FALSE(buildQrText(pub, psk, buf, kQrTextLen));
}

TEST(SyncChannelFormat, Base64Rfc4648Vectors) {
  const char* in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
  const char* out[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
  for (int i = 0; i < 7; ++i) {
    char buf[16];
    ASSERT_TRUE(base64Encode(reinterpret_cast<const uint8_t*>(in[i]), strlen(in[i]), buf, sizeof(buf)));
    EXPECT_STREQ(buf, out[i]);
  }
}

}  // namespace
