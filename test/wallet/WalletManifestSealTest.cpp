// The device sealing the wallet's list of documents as it arrives -- T-2096.
//
// What these tests are for: **the size bound and the order of the checks.** The
// sealer buffers a whole manifest, and the one failure that cannot be recovered
// from is writing a file this same device will later refuse to read. So the cap
// is asserted from both ends here -- the largest accepted manifest must seal to
// exactly the read path's own limit, and one byte more must be refused before a
// byte is taken.
//
// The second subject is that nothing reaches the card half-decided: a CRC
// mismatch, a body that is not a manifest, or an overrun must leave no sealed
// bytes at all.
//
// What they cannot reach is mbedtls, the RNG and the SD card -- the seal is
// bound to OpenSSL here and to mbedtls on the device, which is the same
// two-independent-primitives standard the rest of test/wallet holds
// (WalletCryptoHost.h). The swap itself (write, verify, two renames) is
// Storage-bound and is covered by the hardware pass.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "WalletCryptoHost.h"
#include "WalletManifestIngest.h"
#include "WalletManifestParser.h"

namespace {

// CRC-32, zlib polynomial, bitwise on purpose -- the test's own instrument,
// pinned to the standard check value below so it cannot drift into agreeing
// with the code under test for the wrong reason.
uint32_t crc32Bitwise(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
  }
  return ~crc;
}

const uint8_t kKey[wallet::kWalletKeyLen] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
                                             0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
                                             0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};

// A counter, not a PRNG: a test must not depend on randomness, and what the
// sealer owes its caller is a nonce that *changes*, which a counter proves just
// as well as entropy would.
struct NonceSource {
  uint8_t next = 1;
};

void countingRandom(void* ctx, uint8_t* out, size_t len) {
  auto* source = static_cast<NonceSource*>(ctx);
  std::memset(out, 0, len);
  out[0] = source->next++;
}

bool hostSeal(void* ctx, const uint8_t* nonce, uint8_t* data, size_t len, uint8_t* tag) {
  return wallet::host::gcmEncrypt(static_cast<const uint8_t*>(ctx), nonce, wallet::kGcmNonceLen, data, len, data, tag,
                                  wallet::kGcmTagLen);
}

// The device's own inspection, bound to the same parser the device binds it to.
// Deliberately not a stub: "is this a manifest" is answered by the code that
// will later have to open it, and a stub here would hide a disagreement between
// the two.
bool hostInspect(void*, const char* json, size_t len, uint32_t& walletVersion) {
  wallet::ManifestParser parser;
  parser.beginList(nullptr, 0);
  parser.feed(json, len);
  if (parser.hasError()) return false;
  if (parser.formatVersion() != 1u) return false;
  walletVersion = parser.walletVersion();
  return true;
}

// A small but real manifest: one item, one page, one asset, and a
// `walletVersion` the envelope has to carry out in the clear.
const char* kManifest =
    R"JSON({"formatVersion":1,"walletVersion":42,"items":[{"id":"a","title":"Passport","pages":[{"id":"p0",)JSON"
    R"JSON("levels":{"fit":{"cols":1,"rows":1,"assets":[{"assetId":"0123456789abcdef","type":"FIT","col":0,)JSON"
    R"JSON("row":0,"rawLen":48000}]}},"codes":[]}]}]})JSON";

struct SealRun {
  wallet::ManifestIngestStatus began = wallet::ManifestIngestStatus::NotActive;
  wallet::ManifestIngestStatus last = wallet::ManifestIngestStatus::NotActive;
  std::vector<uint8_t> sealed;
  uint32_t walletVersion = 0;
  uint32_t plaintextCrc = 0;
};

// Pushes `json` through the sealer in `chunkBytes` bites, exactly as the BLE
// transfer does. `declaredCrc` is what the sender claimed.
SealRun sealManifest(const std::string& json, size_t chunkBytes, uint32_t declaredCrc,
                     wallet::ManifestInspect inspect = hostInspect) {
  SealRun run;
  NonceSource nonces;
  wallet::ManifestIngest ingest;
  wallet::ManifestIngest::Callbacks cb;
  cb.crc32 = crc32Bitwise;
  cb.inspect = inspect;
  cb.random = countingRandom;
  cb.randomCtx = &nonces;
  cb.seal = hostSeal;
  cb.sealCtx = const_cast<uint8_t*>(kKey);

  const size_t want = wallet::sealedManifestLen(json.size());
  auto buffer = std::unique_ptr<uint8_t[]>(new uint8_t[want]);
  run.began = ingest.begin(static_cast<uint32_t>(json.size()), declaredCrc, std::move(buffer), want, cb);
  if (run.began != wallet::ManifestIngestStatus::Ok) return run;

  const auto* bytes = reinterpret_cast<const uint8_t*>(json.data());
  for (size_t at = 0; at < json.size(); at += chunkBytes) {
    const size_t take = (at + chunkBytes <= json.size()) ? chunkBytes : json.size() - at;
    run.last = ingest.feed(bytes + at, take);
    if (run.last != wallet::ManifestIngestStatus::Ok && run.last != wallet::ManifestIngestStatus::Complete) break;
  }
  run.plaintextCrc = ingest.plaintextCrc();
  if (run.last == wallet::ManifestIngestStatus::Complete) {
    run.sealed.assign(ingest.sealed(), ingest.sealed() + ingest.sealedLen());
    run.walletVersion = ingest.walletVersion();
  }
  return run;
}

// Opens a sealed manifest the way the device's read path does: structural parse
// of the envelope, then GCM with the tag verified. Returns the plaintext, or an
// empty string when anything refused.
std::string openSealed(const std::vector<uint8_t>& sealed) {
  wallet::ManifestEnvelope envelope;
  if (!wallet::parseManifestEnvelope(sealed.data(), sealed.size(), envelope)) return {};
  std::vector<uint8_t> plain(envelope.ciphertextLen, 0);
  if (!wallet::host::gcmDecrypt(kKey, envelope.nonce, wallet::kGcmNonceLen, sealed.data() + envelope.tagOffset,
                                wallet::kGcmTagLen, sealed.data() + envelope.ciphertextOffset, envelope.ciphertextLen,
                                plain.data())) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(plain.data()), plain.size());
}

// A manifest padded to exactly `bytes` with whitespace inside the object, so a
// size can be chosen without making the JSON invalid.
std::string manifestOfSize(size_t bytes) {
  std::string json = kManifest;
  const std::string head = R"({"formatVersion":1,"walletVersion":42,)";
  if (bytes <= json.size()) return json;
  json.insert(head.size(), std::string(bytes - json.size(), ' '));
  return json;
}

uint32_t crcOf(const std::string& text) {
  return crc32Bitwise(0, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

TEST(WalletManifestSeal, Crc32KnownAnswer) {
  const char* kCheck = "123456789";
  EXPECT_EQ(crc32Bitwise(0, reinterpret_cast<const uint8_t*>(kCheck), 9), 0xCBF43926u);
}

// The round trip: what the device seals, the read path opens, byte for byte.
TEST(WalletManifestSeal, SealedManifestOpensWithTheReadPath) {
  const std::string json = kManifest;
  const SealRun run = sealManifest(json, 180, crcOf(json));
  ASSERT_EQ(run.began, wallet::ManifestIngestStatus::Ok);
  ASSERT_EQ(run.last, wallet::ManifestIngestStatus::Complete);
  EXPECT_EQ(run.sealed.size(), wallet::sealedManifestLen(json.size()));
  EXPECT_EQ(openSealed(run.sealed), json);
  // Carried in the clear, outside the tag, so a locked card can be diffed.
  EXPECT_EQ(run.walletVersion, 42u);
  wallet::ManifestEnvelope envelope;
  ASSERT_TRUE(wallet::parseManifestEnvelope(run.sealed.data(), run.sealed.size(), envelope));
  EXPECT_TRUE(envelope.hasWalletVersion);
  EXPECT_EQ(envelope.walletVersion, 42u);
  EXPECT_EQ(envelope.plaintextLen, json.size());
}

// The chunking is the sender's, never the sealer's: a 23-byte MTU splits this
// document into dozens of pieces and must produce the identical file.
TEST(WalletManifestSeal, ChunkingDoesNotChangeTheResult) {
  const std::string json = kManifest;
  const SealRun big = sealManifest(json, 4096, crcOf(json));
  const SealRun small = sealManifest(json, 7, crcOf(json));
  ASSERT_EQ(big.last, wallet::ManifestIngestStatus::Complete);
  ASSERT_EQ(small.last, wallet::ManifestIngestStatus::Complete);
  // Same nonce (the counter starts at 1 for each run), so the bytes themselves
  // must match -- a stronger statement than "both decrypt to the same thing".
  EXPECT_EQ(big.sealed, small.sealed);
  EXPECT_EQ(openSealed(small.sealed), json);
}

// A flipped byte anywhere inside the sealed region must fail the tag. The
// envelope is NOT covered by it -- that is the format, and parseManifestEnvelope
// documents it -- so this flips the ciphertext and the tag, which are.
TEST(WalletManifestSeal, FlippedCiphertextOrTagFailsTheTag) {
  const std::string json = kManifest;
  const SealRun run = sealManifest(json, 256, crcOf(json));
  ASSERT_EQ(run.last, wallet::ManifestIngestStatus::Complete);

  std::vector<uint8_t> badCipher = run.sealed;
  badCipher[wallet::kManifestEnvelopeLen + 3] ^= 0x01;
  EXPECT_TRUE(openSealed(badCipher).empty());

  std::vector<uint8_t> badTag = run.sealed;
  badTag[badTag.size() - 1] ^= 0x01;
  EXPECT_TRUE(openSealed(badTag).empty());
}

// Each write gets its own nonce, so two seals of identical content differ. One
// key and one nonce over two different plaintexts breaks GCM outright, and this
// file is rewritten on every sync.
TEST(WalletManifestSeal, EachWriteGetsItsOwnNonce) {
  const std::string json = kManifest;
  NonceSource nonces;
  std::vector<std::vector<uint8_t>> files;
  for (int round = 0; round < 2; ++round) {
    wallet::ManifestIngest ingest;
    wallet::ManifestIngest::Callbacks cb;
    cb.crc32 = crc32Bitwise;
    cb.inspect = hostInspect;
    cb.random = countingRandom;
    cb.randomCtx = &nonces;  // shared across both rounds, as the device's RNG is
    cb.seal = hostSeal;
    cb.sealCtx = const_cast<uint8_t*>(kKey);
    const size_t want = wallet::sealedManifestLen(json.size());
    ASSERT_EQ(ingest.begin(static_cast<uint32_t>(json.size()), crcOf(json),
                           std::unique_ptr<uint8_t[]>(new uint8_t[want]), want, cb),
              wallet::ManifestIngestStatus::Ok);
    ASSERT_EQ(ingest.feed(reinterpret_cast<const uint8_t*>(json.data()), json.size()),
              wallet::ManifestIngestStatus::Complete);
    files.emplace_back(ingest.sealed(), ingest.sealed() + ingest.sealedLen());
  }
  EXPECT_NE(files[0], files[1]);
  EXPECT_EQ(openSealed(files[0]), json);
  EXPECT_EQ(openSealed(files[1]), json);
}

// **The size trap, from both ends.** The largest manifest this device accepts
// has to seal to exactly the cap the read path measures off the card -- not one
// byte over, which would be a wallet written successfully and never openable
// again.
TEST(WalletManifestSeal, TheLargestAcceptedManifestFitsTheReadPathsCap) {
  EXPECT_EQ(wallet::sealedManifestLen(wallet::kMaxManifestPlaintextBytes), wallet::kMaxEncryptedManifestBytes);

  const std::string atCap = manifestOfSize(wallet::kMaxManifestPlaintextBytes);
  ASSERT_EQ(atCap.size(), wallet::kMaxManifestPlaintextBytes);
  const SealRun run = sealManifest(atCap, 509, crcOf(atCap));
  ASSERT_EQ(run.began, wallet::ManifestIngestStatus::Ok);
  ASSERT_EQ(run.last, wallet::ManifestIngestStatus::Complete);
  EXPECT_EQ(run.sealed.size(), wallet::kMaxEncryptedManifestBytes);
  EXPECT_EQ(openSealed(run.sealed), atCap);
}

// One byte more is refused at begin -- before a byte is taken, because the begin
// frame already carries the length.
TEST(WalletManifestSeal, OneByteOverTheCapIsRefusedAtBegin) {
  wallet::ManifestIngest ingest;
  wallet::ManifestIngest::Callbacks cb;
  cb.crc32 = crc32Bitwise;
  cb.inspect = hostInspect;
  cb.random = countingRandom;
  cb.seal = hostSeal;
  cb.sealCtx = const_cast<uint8_t*>(kKey);
  NonceSource nonces;
  cb.randomCtx = &nonces;

  const uint32_t tooBig = static_cast<uint32_t>(wallet::kMaxManifestPlaintextBytes) + 1;
  EXPECT_EQ(ingest.begin(tooBig, 0, std::unique_ptr<uint8_t[]>(new uint8_t[64]), 64, cb),
            wallet::ManifestIngestStatus::TooBig);
  EXPECT_FALSE(ingest.active());
  // And a refused begin leaves nothing a later feed could walk into.
  EXPECT_EQ(ingest.feed(reinterpret_cast<const uint8_t*>("{"), 1), wallet::ManifestIngestStatus::NotActive);
  EXPECT_EQ(ingest.sealed(), nullptr);
}

TEST(WalletManifestSeal, ZeroLengthAndShortBufferAreRefused) {
  wallet::ManifestIngest ingest;
  wallet::ManifestIngest::Callbacks cb;
  cb.crc32 = crc32Bitwise;
  cb.inspect = hostInspect;
  cb.random = countingRandom;
  cb.seal = hostSeal;
  cb.sealCtx = const_cast<uint8_t*>(kKey);
  NonceSource nonces;
  cb.randomCtx = &nonces;

  EXPECT_EQ(ingest.begin(0, 0, std::unique_ptr<uint8_t[]>(new uint8_t[64]), 64, cb),
            wallet::ManifestIngestStatus::BadLength);
  // A buffer one byte short of the sealed size: the envelope and the tag are
  // part of the file, and a caller that sized for the plaintext alone would
  // otherwise write past it.
  const size_t justShort = wallet::sealedManifestLen(100) - 1;
  EXPECT_EQ(ingest.begin(100, 0, std::unique_ptr<uint8_t[]>(new uint8_t[justShort]), justShort, cb),
            wallet::ManifestIngestStatus::NoBuffer);
}

// The bytes the sender declared, before anything is sealed.
TEST(WalletManifestSeal, CrcMismatchSealsNothing) {
  const std::string json = kManifest;
  const SealRun run = sealManifest(json, 128, crcOf(json) ^ 0x1u);
  EXPECT_EQ(run.last, wallet::ManifestIngestStatus::CrcMismatch);
  EXPECT_TRUE(run.sealed.empty());
}

// A body that is not a manifest is refused on its first chunk, not after 30 KB
// of it has been sent.
TEST(WalletManifestSeal, NonJsonIsRefusedOnTheFirstChunk) {
  const std::string notJson = "MTIL\x01\x02 this is a tile, not a list";
  const SealRun run = sealManifest(notJson, 8, crcOf(notJson));
  ASSERT_EQ(run.began, wallet::ManifestIngestStatus::Ok);
  EXPECT_EQ(run.last, wallet::ManifestIngestStatus::NotJson);
  EXPECT_TRUE(run.sealed.empty());
}

// Leading whitespace is not a verdict either way: the decision waits for the
// first byte that means something.
TEST(WalletManifestSeal, LeadingWhitespaceIsAllowed) {
  const std::string json = std::string("\n\t  ") + kManifest;
  const SealRun run = sealManifest(json, 2, crcOf(json));
  ASSERT_EQ(run.last, wallet::ManifestIngestStatus::Complete);
  EXPECT_EQ(openSealed(run.sealed), json);
}

// JSON that parses but is not a wallet manifest this firmware can open: the
// inspection is the gate, and it is the read path's own parser.
TEST(WalletManifestSeal, AWrongFormatVersionSealsNothing) {
  const std::string json = R"JSON({"formatVersion":99,"items":[]})JSON";
  const SealRun run = sealManifest(json, 64, crcOf(json));
  EXPECT_EQ(run.last, wallet::ManifestIngestStatus::NotJson);
  EXPECT_TRUE(run.sealed.empty());
}

// An empty wallet is a legitimate sync -- the rider deleted their last document
// -- and must not be refused, or the device keeps showing a list the phone no
// longer has.
TEST(WalletManifestSeal, AnEmptyManifestIsAccepted) {
  const std::string json = R"JSON({"formatVersion":1,"walletVersion":9,"items":[]})JSON";
  const SealRun run = sealManifest(json, 16, crcOf(json));
  ASSERT_EQ(run.last, wallet::ManifestIngestStatus::Complete);
  EXPECT_EQ(openSealed(run.sealed), json);
  EXPECT_EQ(run.walletVersion, 9u);
}

// A manifest with no walletVersion seals with 0, which the format reads as
// "this card does not say". The cost is a resend, never a wrong sync.
TEST(WalletManifestSeal, AMissingWalletVersionSealsAsZero) {
  const std::string json = R"JSON({"formatVersion":1,"items":[]})JSON";
  const SealRun run = sealManifest(json, 16, crcOf(json));
  ASSERT_EQ(run.last, wallet::ManifestIngestStatus::Complete);
  EXPECT_EQ(run.walletVersion, 0u);
  wallet::ManifestEnvelope envelope;
  ASSERT_TRUE(wallet::parseManifestEnvelope(run.sealed.data(), run.sealed.size(), envelope));
  EXPECT_EQ(envelope.walletVersion, 0u);
}

TEST(WalletManifestSeal, MoreBytesThanDeclaredIsRefused) {
  const std::string json = kManifest;
  wallet::ManifestIngest ingest;
  wallet::ManifestIngest::Callbacks cb;
  cb.crc32 = crc32Bitwise;
  cb.inspect = hostInspect;
  cb.random = countingRandom;
  cb.seal = hostSeal;
  cb.sealCtx = const_cast<uint8_t*>(kKey);
  NonceSource nonces;
  cb.randomCtx = &nonces;

  // Declares ten bytes fewer than it sends.
  const uint32_t declaredLen = static_cast<uint32_t>(json.size()) - 10;
  const size_t want = wallet::sealedManifestLen(declaredLen);
  ASSERT_EQ(ingest.begin(declaredLen, crcOf(json), std::unique_ptr<uint8_t[]>(new uint8_t[want]), want, cb),
            wallet::ManifestIngestStatus::Ok);
  EXPECT_EQ(ingest.feed(reinterpret_cast<const uint8_t*>(json.data()), json.size()),
            wallet::ManifestIngestStatus::Overrun);
  EXPECT_EQ(ingest.sealed(), nullptr);
}

// The envelope builder on its own: the layout is shared with the generator and
// the phone, so the byte offsets are pinned here rather than only exercised
// through a round trip.
TEST(WalletManifestSeal, EnvelopeLayoutIsPinned) {
  uint8_t nonce[wallet::kGcmNonceLen];
  for (size_t i = 0; i < sizeof(nonce); ++i) nonce[i] = static_cast<uint8_t>(0x30 + i);
  uint8_t out[wallet::kManifestEnvelopeLen] = {0};
  ASSERT_TRUE(wallet::buildManifestEnvelope(nonce, 0x11223344u, 0x55667788u, out, sizeof(out)));

  EXPECT_EQ(std::memcmp(out, "EWM1", 4), 0);
  EXPECT_EQ(out[4], wallet::kManifestEncVersion);
  EXPECT_EQ(out[5], 0u);  // flags: none defined
  EXPECT_EQ(std::memcmp(out + 6, nonce, sizeof(nonce)), 0);
  EXPECT_EQ(wallet::readLe32(out + 18), 0x11223344u);
  EXPECT_EQ(wallet::readLe32(out + 22), 0x55667788u);

  // A zero-length manifest cannot be sealed: parseManifestEnvelope() refuses to
  // open one, so writing it would be writing an unreadable file.
  EXPECT_FALSE(wallet::buildManifestEnvelope(nonce, 0, 0, out, sizeof(out)));
  EXPECT_FALSE(wallet::buildManifestEnvelope(nonce, 10, 0, out, sizeof(out) - 1));
}

// The gate that decides a push is the list at all. Case-folded, because SdFat
// compares names folded -- the same hole the asset gate had in review.
TEST(WalletManifestSeal, ManifestPathGateFoldsCase) {
  EXPECT_TRUE(wallet::pathIsWalletManifest("/explorink/wallet/manifest.json"));
  EXPECT_TRUE(wallet::pathIsWalletManifest("/explorink/wallet/MANIFEST.JSON"));
  EXPECT_TRUE(wallet::pathIsWalletManifest("/EXPLORINK/WALLET/Manifest.Json"));

  EXPECT_FALSE(wallet::pathIsWalletManifest("/explorink/wallet/manifest.enc"));
  EXPECT_FALSE(wallet::pathIsWalletManifest("/explorink/wallet/manifest.json.part"));
  EXPECT_FALSE(wallet::pathIsWalletManifest("/explorink/wallet/ab/manifest.json"));
  EXPECT_FALSE(wallet::pathIsWalletManifest("/explorink/base/manifest.json"));
  EXPECT_FALSE(wallet::pathIsWalletManifest("/explorink/wallet/ab/0123456789abcdef.dat"));
  EXPECT_FALSE(wallet::pathIsWalletManifest(nullptr));

  // And the two gates must never both claim one path.
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet/manifest.json"));
  EXPECT_TRUE(wallet::pathIsWalletAsset("/explorink/wallet/ab/0123456789abcdef.dat"));
}

// **Everything under the wallet directory is sealed or refused, never stored as
// it arrived.** The two name gates cannot be the whole defence, because FAT
// gives one file more than one name: SdFat writes an 8.3 alias for any name
// that is not 8.3 and its open() matches a query spelled with that alias
// against the same directory entry, so `MANIFE~1.JSO` IS `manifest.json` on the
// card and is not `manifest.json` to a name gate.
TEST(WalletManifestSeal, EverythingElseUnderTheWalletDirIsRefused) {
  // What the receiver computes: in the wallet directory, and neither gate
  // claims it.
  const auto refused = [](const char* path) {
    return wallet::pathIsUnderWalletDir(path) && !wallet::pathIsWalletAsset(path) &&
           !wallet::pathIsWalletManifest(path);
  };

  // FAT's own alias for manifest.json. Writing here deletes and replaces the
  // real list, with no key and nothing but BLE range.
  EXPECT_TRUE(refused("/explorink/wallet/MANIFE~1.JSO"));
  // The device's own outputs. `manifest.bak` is the half-swap survivor, so junk
  // written over it destroys the only list a half-installed card has; a
  // `manifest.bak` on a CLEARTEXT card also flips treeIsEncrypted() and sends
  // the rider to an unlock screen for a wallet that has no wrap.
  EXPECT_TRUE(refused("/explorink/wallet/manifest.bak"));
  EXPECT_TRUE(refused("/explorink/wallet/manifest.enc"));
  EXPECT_TRUE(refused("/explorink/wallet/manifest.enc.part"));
  EXPECT_TRUE(refused("/explorink/wallet/MANIFEST.BAK"));
  // A sidecar: the format writes these for cleartext trees only and this
  // firmware never reads one, so a device that seals its own tree has no use
  // for them either.
  EXPECT_TRUE(refused("/explorink/wallet/ab/0123456789abcdef.rle"));
  EXPECT_TRUE(refused("/explorink/wallet/anything"));

  // And the two legitimate shapes are not refused.
  EXPECT_FALSE(refused("/explorink/wallet/manifest.json"));
  EXPECT_FALSE(refused("/explorink/wallet/MANIFEST.JSON"));
  EXPECT_FALSE(refused("/explorink/wallet/ab/0123456789abcdef.dat"));
  // Nor is anything outside the directory -- a tile must still take the plain
  // path.
  EXPECT_FALSE(refused("/explorink/base/13/4482/2789.tib"));
  EXPECT_FALSE(refused("/explorink/walletish/manifest.bak"));
  EXPECT_FALSE(wallet::pathIsUnderWalletDir("/explorink/wallet"));
  EXPECT_FALSE(wallet::pathIsUnderWalletDir(nullptr));
}

// A callback the sealer cannot work without is a programming error, and a phone
// told "oom" retries a condition retrying cannot fix.
TEST(WalletManifestSeal, MissingCallbacksAreNotReportedAsOom) {
  wallet::ManifestIngest ingest;
  wallet::ManifestIngest::Callbacks cb;  // all null
  EXPECT_EQ(ingest.begin(100, 0, std::unique_ptr<uint8_t[]>(new uint8_t[512]), 512, cb),
            wallet::ManifestIngestStatus::NotWired);
  EXPECT_STREQ(wallet::manifestIngestReason(wallet::ManifestIngestStatus::NotWired), "sealer not wired");
  EXPECT_STRNE(wallet::manifestIngestReason(wallet::ManifestIngestStatus::NotWired), "oom");
}

// The install gives the card its memory back before it reads the file off the
// card, and the object has to survive that -- the transfer is not over until
// its caller says so, and until then another task may still call reset() on it.
TEST(WalletManifestSeal, ReleaseBufferKeepsTheObjectUsable) {
  const std::string json = kManifest;
  NonceSource nonces;
  wallet::ManifestIngest ingest;
  wallet::ManifestIngest::Callbacks cb;
  cb.crc32 = crc32Bitwise;
  cb.inspect = hostInspect;
  cb.random = countingRandom;
  cb.randomCtx = &nonces;
  cb.seal = hostSeal;
  cb.sealCtx = const_cast<uint8_t*>(kKey);
  const size_t want = wallet::sealedManifestLen(json.size());
  ASSERT_EQ(ingest.begin(static_cast<uint32_t>(json.size()), crcOf(json), std::unique_ptr<uint8_t[]>(new uint8_t[want]),
                         want, cb),
            wallet::ManifestIngestStatus::Ok);
  ASSERT_EQ(ingest.feed(reinterpret_cast<const uint8_t*>(json.data()), json.size()),
            wallet::ManifestIngestStatus::Complete);
  ASSERT_NE(ingest.sealed(), nullptr);

  ingest.releaseBuffer();
  EXPECT_EQ(ingest.sealed(), nullptr);
  EXPECT_EQ(ingest.sealedLen(), 0u);
  EXPECT_FALSE(ingest.active());
  // The calls another task can still make on it, after the memory is gone.
  ingest.reset();
  EXPECT_EQ(ingest.feed(reinterpret_cast<const uint8_t*>("{"), 1), wallet::ManifestIngestStatus::NotActive);
}

// A reused instance must not inherit the previous transfer's state: the `{`
// refusal would be skipped, and the previous manifest's titles would still be
// in RAM.
TEST(WalletManifestSeal, BeginDropsThePreviousRunsBufferAndFlags) {
  NonceSource nonces;
  wallet::ManifestIngest ingest;
  wallet::ManifestIngest::Callbacks cb;
  cb.crc32 = crc32Bitwise;
  cb.inspect = hostInspect;
  cb.random = countingRandom;
  cb.randomCtx = &nonces;
  cb.seal = hostSeal;
  cb.sealCtx = const_cast<uint8_t*>(kKey);

  const std::string json = kManifest;
  const size_t want = wallet::sealedManifestLen(json.size());
  ASSERT_EQ(ingest.begin(static_cast<uint32_t>(json.size()), crcOf(json), std::unique_ptr<uint8_t[]>(new uint8_t[want]),
                         want, cb),
            wallet::ManifestIngestStatus::Ok);
  // One chunk in -- `openingChecked_` is now set and the buffer holds titles --
  // and then the transfer dies without a terminal feed().
  ASSERT_EQ(ingest.feed(reinterpret_cast<const uint8_t*>(json.data()), 40), wallet::ManifestIngestStatus::Ok);

  // The next transfer is not JSON. It has to be refused on its first chunk,
  // which only happens if begin() cleared the flag.
  const std::string notJson = "MTIL not a list at all";
  const size_t want2 = wallet::sealedManifestLen(notJson.size());
  ASSERT_EQ(ingest.begin(static_cast<uint32_t>(notJson.size()), crcOf(notJson),
                         std::unique_ptr<uint8_t[]>(new uint8_t[want2]), want2, cb),
            wallet::ManifestIngestStatus::Ok);
  EXPECT_EQ(ingest.feed(reinterpret_cast<const uint8_t*>(notJson.data()), notJson.size()),
            wallet::ManifestIngestStatus::NotJson);
}

}  // namespace
