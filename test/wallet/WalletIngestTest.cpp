// The device sealing an asset as it arrives -- T-2095.
//
// What these tests are for: the **offset arithmetic**. The sealer runs one pass
// over a stream it does not control the chunking of, it keeps two offsets (file
// and payload) that differ by the 32-byte header, and the header itself can
// arrive split across three chunks at a 23-byte MTU. That is the part that gets
// a windowed CTR write wrong, and it is pure arithmetic, so it is testable here
// against OpenSSL instead of mbedtls -- the same two-independent-primitives
// standard the rest of test/wallet holds (WalletCryptoHost.h).
//
// What they cannot reach is mbedtls's own calls and the SD card. The hardware
// pass is what covers those (firmware docs/wallet-crypto.md).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "WalletAssetIngest.h"
#include "WalletCryptoHost.h"
#include "WalletKeySlot.h"

namespace {

// CRC-32, the zlib/esp_rom_crc32_le polynomial. Bitwise and slow on purpose:
// this is the test's instrument, and a table copied from the thing under test
// would agree with it for the wrong reason. Pinned to the standard check value
// in Crc32KnownAnswer below.
uint32_t crc32Bitwise(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
  }
  return ~crc;
}

uint32_t crc32Of(const std::vector<uint8_t>& bytes) { return crc32Bitwise(0, bytes.data(), bytes.size()); }

// The 32-byte key the session would hold. Fixed, so a failure is reproducible.
const uint8_t kKey[wallet::kWalletKeyLen] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
                                             0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
                                             0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};

// The callbacks the device binds to mbedtls and the ROM, bound here to OpenSSL
// and to the bitwise CRC above.
bool hostCipher(void* ctx, const uint8_t* iv, uint32_t payloadOffset, uint8_t* data, size_t len) {
  wallet::host::ctrXor(static_cast<const uint8_t*>(ctx), iv, payloadOffset, data, len);
  return true;
}

// A cipher that refuses once it has enciphered `limit` bytes, standing in for
// the session key dying part-way through a transfer -- which is what the device
// binding does now that it re-reads the key per chunk.
struct FlakyCipher {
  size_t done = 0;
  size_t limit = 0;
};

bool flakyCipher(void* ctx, const uint8_t* iv, uint32_t payloadOffset, uint8_t* data, size_t len) {
  auto* c = static_cast<FlakyCipher*>(ctx);
  if (c->done + len > c->limit) return false;
  c->done += len;
  wallet::host::ctrXor(kKey, iv, payloadOffset, data, len);
  return true;
}

bool hostSink(void* ctx, const uint8_t* data, size_t len) {
  auto* out = static_cast<std::vector<uint8_t>*>(ctx);
  out->insert(out->end(), data, data + len);
  return true;
}

// A sink that fails once it has taken `limit` bytes, for the short-write path.
struct LimitedSink {
  std::vector<uint8_t> bytes;
  size_t limit = 0;
};

bool limitedSink(void* ctx, const uint8_t* data, size_t len) {
  auto* sink = static_cast<LimitedSink*>(ctx);
  if (sink->bytes.size() + len > sink->limit) return false;
  sink->bytes.insert(sink->bytes.end(), data, data + len);
  return true;
}

void writeLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void writeLe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// Deterministic bytes that are not compressible and not all the same, so a
// keystream reuse or an off-by-one offset shows as a diff rather than hiding in
// a run of zeros.
std::vector<uint8_t> fakePayload(size_t len, uint32_t seed = 1) {
  std::vector<uint8_t> out(len);
  uint32_t state = seed * 2654435761u + 1u;
  for (size_t i = 0; i < len; ++i) {
    state = state * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(state >> 24);
  }
  return out;
}

// One asset exactly as a cleartext generator writes it: 32-byte header, flags
// clear, sha256_prefix over the plaintext payload (tools/walletgen.py,
// build_asset_header).
std::vector<uint8_t> buildPlainAsset(const std::vector<uint8_t>& payload, wallet::AssetType type, uint8_t bitDepth,
                                     uint16_t width, uint16_t height, uint32_t version) {
  std::vector<uint8_t> file(wallet::kAssetHeaderBytes, 0);
  std::memcpy(file.data(), wallet::kAssetMagic, sizeof(wallet::kAssetMagic));
  file[4] = static_cast<uint8_t>(type);
  file[5] = bitDepth;
  file[6] = 0;  // tileCol
  file[7] = 0;  // tileRow
  writeLe16(file.data() + 8, width);
  writeLe16(file.data() + 10, height);
  writeLe32(file.data() + 12, static_cast<uint32_t>(payload.size()));
  writeLe32(file.data() + 16, version);
  file[20] = 0;  // flags: a cleartext tree sets none
  file[21] = 0;  // presentation
  wallet::Sha256 sha;
  sha.update(payload.data(), payload.size());
  uint8_t digest[wallet::kSha256Bytes];
  sha.finish(digest);
  std::memcpy(file.data() + 24, digest, wallet::kSha256PrefixBytes);
  file.insert(file.end(), payload.begin(), payload.end());
  return file;
}

// A full-screen X4 tile, which is the common case.
std::vector<uint8_t> buildX4Tile(uint32_t version = 7, uint32_t seed = 1) {
  const std::vector<uint8_t> payload = fakePayload(wallet::kPanelX4.bufferBytes, seed);
  return buildPlainAsset(payload, wallet::AssetType::Fit, wallet::kBitDepth1, wallet::kPanelX4.width,
                         wallet::kPanelX4.height, version);
}

// Runs a whole file through the sealer in fixed-size chunks, the way the BLE
// transfer does. `declaredCrc` is what the sender said; pass crc32Of(file) for
// the honest case.
struct SealRun {
  wallet::IngestStatus status = wallet::IngestStatus::NotActive;
  std::vector<uint8_t> sealed;
  uint32_t plaintextCrc = 0;
  bool began = false;
};

SealRun seal(const std::vector<uint8_t>& file, size_t chunkBytes, uint32_t declaredCrc,
             const wallet::PanelGeometry& panel = wallet::kPanelX4) {
  SealRun run;
  wallet::AssetIngest ingest;
  wallet::AssetIngest::Callbacks cb;
  cb.cipher = hostCipher;
  cb.cipherCtx = const_cast<uint8_t*>(kKey);
  cb.crc32 = crc32Bitwise;
  cb.write = hostSink;
  cb.writeCtx = &run.sealed;
  run.began = ingest.begin(static_cast<uint32_t>(file.size()), declaredCrc, panel, cb);
  if (!run.began) return run;

  for (size_t at = 0; at < file.size(); at += chunkBytes) {
    const size_t take = std::min(chunkBytes, file.size() - at);
    run.status = ingest.feed(file.data() + at, take);
    if (run.status != wallet::IngestStatus::Ok) break;
  }
  run.plaintextCrc = ingest.plaintextCrc();
  return run;
}

// --- the instrument itself -------------------------------------------------

TEST(WalletIngest, Crc32KnownAnswer) {
  // The standard CRC-32 check value. Without this the CRC tests below would
  // prove only that the sealer agrees with the test's own mistake.
  const char* kCheck = "123456789";
  EXPECT_EQ(crc32Bitwise(0, reinterpret_cast<const uint8_t*>(kCheck), 9), 0xCBF43926u);
}

// --- the round trip --------------------------------------------------------

TEST(WalletIngest, SealsAnAssetThatDecryptsBackToThePlaintext) {
  const std::vector<uint8_t> file = buildX4Tile();
  const SealRun run = seal(file, 180, crc32Of(file));

  ASSERT_TRUE(run.began);
  ASSERT_EQ(run.status, wallet::IngestStatus::Complete);
  // Same number of bytes sealed as in the clear: the format does not grow.
  ASSERT_EQ(run.sealed.size(), file.size());

  // The header is the sender's, with the two crypto flags set and nothing else
  // touched -- geometry, version, presentation and the hash prefix all survive.
  EXPECT_EQ(run.sealed[20], wallet::kFlagEncrypted | wallet::kFlagContentIv);
  std::vector<uint8_t> headerWithoutFlags(run.sealed.begin(), run.sealed.begin() + wallet::kAssetHeaderBytes);
  headerWithoutFlags[20] = 0;
  EXPECT_EQ(0, std::memcmp(headerWithoutFlags.data(), file.data(), wallet::kAssetHeaderBytes));

  // And the payload decrypts with the IV the read path would rebuild from the
  // flags it just read -- which is the only thing that proves the IV was right,
  // because a wrong IV produces noise and no error.
  wallet::AssetHeader header;
  ASSERT_TRUE(wallet::parseAssetHeader(run.sealed.data(), wallet::kAssetHeaderBytes, header));
  uint8_t iv[wallet::kAssetIvLen];
  ASSERT_TRUE(wallet::buildAssetIvForAsset(header.flags, "0123456789abcdef", header.sha256Prefix,
                                           sizeof(header.sha256Prefix), header.version, iv));
  std::vector<uint8_t> payload(run.sealed.begin() + wallet::kAssetHeaderBytes, run.sealed.end());
  wallet::host::ctrXor(kKey, iv, 0, payload.data(), payload.size());
  EXPECT_EQ(0, std::memcmp(payload.data(), file.data() + wallet::kAssetHeaderBytes, payload.size()));

  // The reply the phone compares against is the CRC of what it sent, not of
  // what is on the card.
  EXPECT_EQ(run.plaintextCrc, crc32Of(file));
  EXPECT_NE(run.plaintextCrc, crc32Of(run.sealed));
}

TEST(WalletIngest, ChunkSizeDoesNotChangeTheCiphertext) {
  // The CTR counter crosses chunk boundaries and chunks are not 16-byte
  // aligned, so a sealer that restarted the keystream per chunk -- or per
  // scratch slice -- would still produce a plausible file. This is the test
  // that catches it: 15 bytes is a 23-byte MTU, 509 is the largest BLE payload,
  // 4096 is bigger than one scratch slice, and all three must agree.
  const std::vector<uint8_t> file = buildX4Tile();
  const uint32_t declared = crc32Of(file);
  const SealRun reference = seal(file, 180, declared);
  ASSERT_EQ(reference.status, wallet::IngestStatus::Complete);

  for (const size_t chunk : {size_t{1}, size_t{15}, size_t{31}, size_t{32}, size_t{33}, size_t{509}, size_t{4096}}) {
    const SealRun run = seal(file, chunk, declared);
    EXPECT_EQ(run.status, wallet::IngestStatus::Complete) << "chunk " << chunk;
    EXPECT_EQ(run.sealed, reference.sealed) << "chunk " << chunk;
    EXPECT_EQ(run.plaintextCrc, declared) << "chunk " << chunk;
  }
}

TEST(WalletIngest, NothingIsDecidedUntilTheHeaderIsWhole) {
  // At a 23-byte MTU the 32-byte header arrives in three pieces, so nothing can
  // be decided until the third one lands.
  //
  // **Asserted as "still Ok at byte 31, Malformed at byte 32"**, because the
  // obvious version of this test cannot fail: feeding a bad header in 15-byte
  // chunks answers Malformed whether the sealer waits for all 32 bytes or
  // wrongly parses the first 15, since parseAssetHeader() refuses a short
  // buffer too. The byte-by-byte form is the one that distinguishes them.
  std::vector<uint8_t> file = buildX4Tile();
  file[5] = wallet::kBitDepth2;  // a 1bpp tile claiming grey: refused once read

  wallet::AssetIngest ingest;
  std::vector<uint8_t> sink;
  wallet::AssetIngest::Callbacks cb;
  cb.cipher = hostCipher;
  cb.cipherCtx = const_cast<uint8_t*>(kKey);
  cb.crc32 = crc32Bitwise;
  cb.write = hostSink;
  cb.writeCtx = &sink;
  ASSERT_TRUE(ingest.begin(static_cast<uint32_t>(file.size()), crc32Of(file), wallet::kPanelX4, cb));

  for (size_t i = 0; i < wallet::kAssetHeaderBytes - 1; ++i) {
    ASSERT_EQ(ingest.feed(file.data() + i, 1), wallet::IngestStatus::Ok) << "byte " << i;
  }
  // Nothing has been written yet either: the header the device owns is not
  // known until its last byte lands.
  EXPECT_TRUE(sink.empty());
  EXPECT_EQ(ingest.feed(file.data() + wallet::kAssetHeaderBytes - 1, 1), wallet::IngestStatus::Malformed);
}

// --- the checks that keep a bad asset off the card -------------------------

TEST(WalletIngest, FlippedPayloadByteFiresTheHashCheck) {
  // The plan's own done-test. The prefix is trusted when the keystream is built
  // and verified when the last byte lands; without this check a corrupted
  // transfer would be sealed under a keystream nobody can reproduce and would
  // be noise on the panel for ever, with every structural check passing.
  std::vector<uint8_t> file = buildX4Tile();
  file[wallet::kAssetHeaderBytes + 1234] ^= 0x40;
  // The sender's CRC is recomputed over the corrupted file, so the CRC agrees
  // and only the hash can catch it -- which is the case worth testing.
  const SealRun run = seal(file, 180, crc32Of(file));
  EXPECT_EQ(run.status, wallet::IngestStatus::HashMismatch);
}

TEST(WalletIngest, CorruptedTransferFiresTheCrcCheck) {
  const std::vector<uint8_t> file = buildX4Tile();
  const SealRun run = seal(file, 180, crc32Of(file) ^ 1u);
  EXPECT_EQ(run.status, wallet::IngestStatus::CrcMismatch);
}

TEST(WalletIngest, AlreadyEncryptedIsRefused) {
  // A sender that encrypted it used a key this board does not have. Sealing it
  // again would bury the payload under two keystreams.
  std::vector<uint8_t> file = buildX4Tile();
  file[20] = wallet::kFlagEncrypted | wallet::kFlagContentIv;
  const SealRun run = seal(file, 180, crc32Of(file));
  EXPECT_EQ(run.status, wallet::IngestStatus::Encrypted);
}

TEST(WalletIngest, BadMagicIsRefused) {
  std::vector<uint8_t> file = buildX4Tile();
  file[3] = '2';
  const SealRun run = seal(file, 180, crc32Of(file));
  EXPECT_EQ(run.status, wallet::IngestStatus::Malformed);
}

TEST(WalletIngest, AnotherPanelsTileIsRefused) {
  // One asset set per wallet, and the X3 is 792x528 at 99 B/row. Refused on the
  // way in rather than stored and refused later by the viewer.
  const std::vector<uint8_t> payload = fakePayload(wallet::kPanelX3.bufferBytes);
  const std::vector<uint8_t> file = buildPlainAsset(payload, wallet::AssetType::Fit, wallet::kBitDepth1,
                                                    wallet::kPanelX3.width, wallet::kPanelX3.height, 7);
  const SealRun run = seal(file, 180, crc32Of(file), wallet::kPanelX4);
  EXPECT_EQ(run.status, wallet::IngestStatus::WrongPanel);
}

TEST(WalletIngest, HeaderLengthMustAgreeWithTheTransfer) {
  // Two independent statements about the same file. Disagreeing means one is
  // corrupt, and the asset would read short or long for ever.
  std::vector<uint8_t> file = buildX4Tile();
  writeLe32(file.data() + 12, wallet::kPanelX4.bufferBytes - 1);
  const SealRun run = seal(file, 180, crc32Of(file));
  EXPECT_EQ(run.status, wallet::IngestStatus::BadLength);
}

TEST(WalletIngest, UnknownAssetTypeIsRefused) {
  // It would be sealed under a key only this board holds and then never drawn.
  const std::vector<uint8_t> payload = fakePayload(wallet::kPanelX4.bufferBytes);
  const std::vector<uint8_t> file = buildPlainAsset(payload, static_cast<wallet::AssetType>(99), wallet::kBitDepth1,
                                                    wallet::kPanelX4.width, wallet::kPanelX4.height, 7);
  const SealRun run = seal(file, 180, crc32Of(file));
  EXPECT_EQ(run.status, wallet::IngestStatus::Malformed);
}

TEST(WalletIngest, TooShortToBeAnAssetIsRefusedAtBegin) {
  // Nothing is opened for write before this answers, so a refusal here leaves
  // the card exactly as it was.
  wallet::AssetIngest ingest;
  std::vector<uint8_t> sink;
  wallet::AssetIngest::Callbacks cb;
  cb.cipher = hostCipher;
  cb.cipherCtx = const_cast<uint8_t*>(kKey);
  cb.crc32 = crc32Bitwise;
  cb.write = hostSink;
  cb.writeCtx = &sink;
  EXPECT_FALSE(ingest.begin(wallet::kAssetHeaderBytes, 0, wallet::kPanelX4, cb));
  EXPECT_FALSE(ingest.begin(0, 0, wallet::kPanelX4, cb));
  // And a sealer that was never wired to its primitives must refuse too, rather
  // than call a null pointer on the first chunk.
  cb.cipher = nullptr;
  EXPECT_FALSE(ingest.begin(1024, 0, wallet::kPanelX4, cb));
}

TEST(WalletIngest, OverrunIsRefused) {
  const std::vector<uint8_t> file = buildX4Tile();
  wallet::AssetIngest ingest;
  std::vector<uint8_t> sink;
  wallet::AssetIngest::Callbacks cb;
  cb.cipher = hostCipher;
  cb.cipherCtx = const_cast<uint8_t*>(kKey);
  cb.crc32 = crc32Bitwise;
  cb.write = hostSink;
  cb.writeCtx = &sink;
  // The declared length is honest -- a dishonest one is BadLength, which the
  // header gate catches before a byte of payload moves. This is the other case:
  // a sender that keeps sending past what it declared. One extra byte must end
  // the transfer rather than run off the end of the file the receiver sized.
  std::vector<uint8_t> padded = file;
  padded.push_back(0);
  ASSERT_TRUE(ingest.begin(static_cast<uint32_t>(file.size()), crc32Of(file), wallet::kPanelX4, cb));
  size_t at = 0;
  wallet::IngestStatus status = wallet::IngestStatus::Ok;
  while (at < padded.size() && status == wallet::IngestStatus::Ok) {
    const size_t take = std::min<size_t>(180, padded.size() - at);
    status = ingest.feed(padded.data() + at, take);
    at += take;
  }
  EXPECT_EQ(status, wallet::IngestStatus::Overrun);
}

TEST(WalletIngest, ShortWriteEndsTheTransfer) {
  // A full card. The transfer must end rather than silently keep hashing.
  const std::vector<uint8_t> file = buildX4Tile();
  LimitedSink sink;
  sink.limit = 4096;
  wallet::AssetIngest ingest;
  wallet::AssetIngest::Callbacks cb;
  cb.cipher = hostCipher;
  cb.cipherCtx = const_cast<uint8_t*>(kKey);
  cb.crc32 = crc32Bitwise;
  cb.write = limitedSink;
  cb.writeCtx = &sink;
  ASSERT_TRUE(ingest.begin(static_cast<uint32_t>(file.size()), crc32Of(file), wallet::kPanelX4, cb));
  size_t at = 0;
  wallet::IngestStatus status = wallet::IngestStatus::Ok;
  while (at < file.size() && status == wallet::IngestStatus::Ok) {
    const size_t take = std::min<size_t>(180, file.size() - at);
    status = ingest.feed(file.data() + at, take);
    at += take;
  }
  EXPECT_EQ(status, wallet::IngestStatus::WriteFailed);
}

TEST(WalletIngest, FeedWithoutBeginIsRefused) {
  wallet::AssetIngest ingest;
  const uint8_t byte = 0;
  EXPECT_EQ(ingest.feed(&byte, 1), wallet::IngestStatus::NotActive);
}

// --- a page image, whose stride the manifest normally states ---------------

TEST(WalletIngest, PageImageIsCheckedAgainstItsOwnStride) {
  // Ingest has no manifest -- that is T-2096 -- so a page image's stride comes
  // from rawLen / height, whatever padding the generator chose. A page that can
  // hold the panel window is taken; one narrower than the panel is not.
  const uint16_t nativeWidth = 1200;
  const uint16_t nativeHeight = 1600;
  const uint32_t stride = nativeWidth / 8u;
  const std::vector<uint8_t> payload = fakePayload(stride * nativeHeight);
  const std::vector<uint8_t> file =
      buildPlainAsset(payload, wallet::AssetType::PageImage, wallet::kBitDepth1, nativeWidth, nativeHeight, 3);
  const SealRun run = seal(file, 509, crc32Of(file));
  EXPECT_EQ(run.status, wallet::IngestStatus::Complete);

  const uint16_t narrowWidth = 400;
  const uint32_t narrowStride = narrowWidth / 8u;
  const std::vector<uint8_t> narrowPayload = fakePayload(narrowStride * nativeHeight);
  const std::vector<uint8_t> narrow =
      buildPlainAsset(narrowPayload, wallet::AssetType::PageImage, wallet::kBitDepth1, narrowWidth, nativeHeight, 3);
  EXPECT_EQ(seal(narrow, 509, crc32Of(narrow)).status, wallet::IngestStatus::WrongPanel);
}

TEST(WalletIngest, GreyPlanesCarryThreePlanes) {
  // assetType 7 is three concatenated 1bpp planes, so its rawLen is three times
  // a 1bpp page's.
  //
  // **This test is written so that deleting the `3u` factor breaks it**, which
  // the first version was not: with the old "stride is at least the panel's"
  // rule, dividing by one plane merely produced a stride three times too big
  // and the asset was taken either way, so both assertions held with the factor
  // removed. The stride upper bound is what makes the two answers differ now --
  // 3 x the row is wider than the row can possibly be.
  const uint16_t width = 1200;
  const uint16_t height = 1600;
  const uint32_t planeBytes = (width / 8u) * height;
  const std::vector<uint8_t> payload = fakePayload(planeBytes * 3u);
  const std::vector<uint8_t> file =
      buildPlainAsset(payload, wallet::AssetType::GreyPlanes, wallet::kBitDepth2, width, height, 3);
  EXPECT_EQ(seal(file, 509, crc32Of(file)).status, wallet::IngestStatus::Complete);

  // Proof that the factor is load-bearing, stated as the gate rather than a
  // whole transfer: the same header read as one plane is a 450-byte stride for
  // a row that needs 150, which the stride bound refuses.
  wallet::AssetHeader asOnePlane;
  ASSERT_TRUE(wallet::parseAssetHeader(file.data(), wallet::kAssetHeaderBytes, asOnePlane));
  asOnePlane.assetType = wallet::AssetType::PageImageGrey;  // one image, not three planes
  EXPECT_EQ(wallet::checkAssetForIngest(asOnePlane, wallet::kPanelX4, planeBytes * 3u),
            wallet::IngestStatus::Malformed);
}

TEST(WalletIngest, AStrideWiderThanTheRowIsRefused) {
  // The skew case, and the reason the stride has an upper bound at all. 800
  // pixels need 100 bytes a row; this header declares 200 and every other check
  // here passes. Taken, it would be sealed and then refused for ever by the read
  // path, which compares the stride against the manifest.
  const uint16_t width = 800;
  const uint16_t height = 1600;
  const uint32_t rawLen = 200u * height;
  const std::vector<uint8_t> file =
      buildPlainAsset(fakePayload(rawLen), wallet::AssetType::PageImage, wallet::kBitDepth1, width, height, 3);
  EXPECT_EQ(seal(file, 509, crc32Of(file)).status, wallet::IngestStatus::Malformed);

  // And the honest stride for the same page is taken.
  const uint32_t honest = 100u * height;
  const std::vector<uint8_t> ok =
      buildPlainAsset(fakePayload(honest), wallet::AssetType::PageImage, wallet::kBitDepth1, width, height, 3);
  EXPECT_EQ(seal(ok, 509, crc32Of(ok)).status, wallet::IngestStatus::Complete);
}

TEST(WalletIngest, ADegeneratePanelTakesNothing) {
  // A renderer answering zero must not make every page image pass. The tile
  // branch already guarded its own geometry; this is the page branch's.
  const std::vector<uint8_t> file = buildX4Tile();
  wallet::AssetHeader header;
  ASSERT_TRUE(wallet::parseAssetHeader(file.data(), wallet::kAssetHeaderBytes, header));
  header.assetType = wallet::AssetType::PageImage;
  const wallet::PanelGeometry dead = {};
  EXPECT_EQ(wallet::checkAssetForIngest(header, dead, wallet::kPanelX4.bufferBytes), wallet::IngestStatus::WrongPanel);
}

TEST(WalletIngest, ACipherThatFailsMidTransferEndsIt) {
  // The device re-reads the session key on every chunk, so a wallet locked
  // part-way through -- the rider pressing power, CMD:WALLETLOCK -- makes the
  // cipher refuse. That must end the transfer. It cannot be caught any other
  // way: the hash and the CRC are both over the plaintext, so both would still
  // pass on a half-enciphered file.
  const std::vector<uint8_t> file = buildX4Tile();
  FlakyCipher cipher;
  cipher.limit = 10000;
  std::vector<uint8_t> sink;
  wallet::AssetIngest ingest;
  wallet::AssetIngest::Callbacks cb;
  cb.cipher = flakyCipher;
  cb.cipherCtx = &cipher;
  cb.crc32 = crc32Bitwise;
  cb.write = hostSink;
  cb.writeCtx = &sink;
  ASSERT_TRUE(ingest.begin(static_cast<uint32_t>(file.size()), crc32Of(file), wallet::kPanelX4, cb));
  size_t at = 0;
  wallet::IngestStatus status = wallet::IngestStatus::Ok;
  while (at < file.size() && status == wallet::IngestStatus::Ok) {
    const size_t take = std::min<size_t>(180, file.size() - at);
    status = ingest.feed(file.data() + at, take);
    at += take;
  }
  EXPECT_EQ(status, wallet::IngestStatus::CipherFailed);
}

TEST(WalletIngest, AWalletLockedMidTransferAbandonsInsteadOfSealingWrong) {
  // T-2104, item 1, with the device's own shape: the cipher borrows the key
  // from a KeySlot per chunk, exactly as walletCipher() in
  // MapTransferReceiver.cpp does, and the slot is cleared part-way through.
  // The transfer must end with CipherFailed -- which the receiver turns into
  // abandon(), deleting the part file -- and every byte written before that
  // must have been enciphered under the whole key, never a partly wiped one.
  struct SlotCipher {
    wallet::KeySlot<std::mutex>* slot = nullptr;
    size_t chunks = 0;
    size_t clearBefore = 0;
  };
  auto slotCipher = [](void* ctx, const uint8_t* iv, uint32_t payloadOffset, uint8_t* data, size_t len) {
    auto* c = static_cast<SlotCipher*>(ctx);
    if (c->chunks++ == c->clearBefore) c->slot->clear();
    const wallet::KeyCopy key(*c->slot);
    if (!key) return false;
    wallet::host::ctrXor(key.bytes(), iv, payloadOffset, data, len);
    return true;
  };

  const std::vector<uint8_t> file = buildX4Tile();
  wallet::KeySlot<std::mutex> slot;
  slot.set(kKey);
  SlotCipher cipher;
  cipher.slot = &slot;
  cipher.clearBefore = 20;
  std::vector<uint8_t> sink;
  wallet::AssetIngest ingest;
  wallet::AssetIngest::Callbacks cb;
  cb.cipher = slotCipher;
  cb.cipherCtx = &cipher;
  cb.crc32 = crc32Bitwise;
  cb.write = hostSink;
  cb.writeCtx = &sink;
  ASSERT_TRUE(ingest.begin(static_cast<uint32_t>(file.size()), crc32Of(file), wallet::kPanelX4, cb));
  size_t at = 0;
  wallet::IngestStatus status = wallet::IngestStatus::Ok;
  while (at < file.size() && status == wallet::IngestStatus::Ok) {
    const size_t take = std::min<size_t>(180, file.size() - at);
    status = ingest.feed(file.data() + at, take);
    at += take;
  }
  EXPECT_EQ(status, wallet::IngestStatus::CipherFailed);
  // It got far enough for the lock to land mid-payload, not at begin.
  EXPECT_EQ(cipher.chunks, cipher.clearBefore + 1);
  ASSERT_GT(sink.size(), wallet::kAssetHeaderBytes);
  ASSERT_LT(sink.size(), file.size());

  // What did reach the sink decrypts back to the sender's bytes under the real
  // key: nothing was enciphered under a wiped one before the refusal.
  uint8_t iv[wallet::kAssetIvLen];
  wallet::AssetHeader header;
  ASSERT_TRUE(wallet::parseAssetHeader(sink.data(), sink.size(), header));
  ASSERT_TRUE(wallet::buildAssetIvFromHash(header.sha256Prefix, sizeof(header.sha256Prefix), header.version, iv));
  std::vector<uint8_t> payload(sink.begin() + wallet::kAssetHeaderBytes, sink.end());
  wallet::host::ctrXor(kKey, iv, 0, payload.data(), payload.size());
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), file.begin() + wallet::kAssetHeaderBytes));
}

// --- which files get sealed at all -----------------------------------------

TEST(WalletIngest, OnlyWalletDatFilesAreSealed) {
  // The path is the signal, not a flag on the wire: a sender cannot ask for a
  // document to be stored in the clear, and cannot turn a map tile into one.
  EXPECT_TRUE(wallet::pathIsWalletAsset("/explorink/wallet/ab/abcdef0123456789.dat"));
  EXPECT_TRUE(wallet::pathIsWalletAsset("/explorink/wallet/00/0000000000000000.dat"));

  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/base/13/4482/2789.tib"));
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet/manifest.enc"));
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet/ab/abcdef0123456789.rle"));
  // A directory that merely starts with the same letters is not the wallet.
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/walletish/ab/abcdef0123456789.dat"));
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet"));
  EXPECT_FALSE(wallet::pathIsWalletAsset(nullptr));

  // **The shape is checked whole, not as a prefix and a suffix** (review,
  // 2026-11-27). A directory component the sender chose is how
  // `wallet/manifest.bak/x.dat` got itself treated as an asset, and the
  // directory creation that follows made `manifest.bak` undeletable.
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet/manifest.bak/x.dat"));
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet/ab/cd/abcdef0123456789.dat"));
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet/abcdef0123456789.dat"));
  // The shard is the first two characters of the id, so a name that disagrees
  // with its own directory was never written by this device.
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet/zz/abcdef0123456789.dat"));
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet/ab/gbcdef0123456789.dat"));
}

TEST(WalletIngest, TheGateFoldsCaseBecauseTheFilesystemDoes) {
  // SdFat matches directory entries case-insensitively, so an exact-case gate
  // and the filesystem under it disagreed about what one name means. Pushing
  // `…abcdef.DAT` skipped the sealer -- no key needed, wallet locked -- and then
  // replaced the sealed `…abcdef.dat`, which the viewer drew as a genuine
  // document because the encrypted flag was clear. Found in review 2026-11-27.
  EXPECT_TRUE(wallet::pathIsWalletAsset("/explorink/wallet/ab/abcdef0123456789.DAT"));
  EXPECT_TRUE(wallet::pathIsWalletAsset("/explorink/wallet/AB/ABCDEF0123456789.Dat"));
  EXPECT_TRUE(wallet::pathIsWalletAsset("/EXPLORINK/WALLET/ab/abcdef0123456789.dat"));
  // Folding must not widen it past the wallet.
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/WALLETISH/ab/abcdef0123456789.dat"));
  EXPECT_FALSE(wallet::pathIsWalletAsset("/explorink/wallet/ab/abcdef0123456789.datx"));
}

// --- the version is part of the keystream ----------------------------------

TEST(WalletIngest, VersionChangesTheKeystream) {
  // The IV is sha256_prefix || version, so two versions of the same bytes do
  // not share a keystream. Same plaintext, same key, different version: the
  // ciphertext must differ (WalletCrypto.h, buildAssetIvFromHash).
  const std::vector<uint8_t> v1 = buildX4Tile(1);
  const std::vector<uint8_t> v2 = buildX4Tile(2);
  const SealRun a = seal(v1, 180, crc32Of(v1));
  const SealRun b = seal(v2, 180, crc32Of(v2));
  ASSERT_EQ(a.status, wallet::IngestStatus::Complete);
  ASSERT_EQ(b.status, wallet::IngestStatus::Complete);
  const std::vector<uint8_t> aBody(a.sealed.begin() + wallet::kAssetHeaderBytes, a.sealed.end());
  const std::vector<uint8_t> bBody(b.sealed.begin() + wallet::kAssetHeaderBytes, b.sealed.end());
  EXPECT_NE(aBody, bBody);
}

}  // namespace
