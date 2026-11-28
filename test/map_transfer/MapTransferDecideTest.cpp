// The BLE transfer receiver's decisions, proved without a board.
//
// Written as a **characterisation** suite: every case below was read off
// `MapTransferReceiver.cpp` as it stood at T-2096's merge (`c9f4bdba`) and
// written down *before* the decisions were lifted into MapTransferDecide.h, so
// the suite is the contract the move had to preserve rather than a description
// of whatever came out of it. Nothing here is an improvement on the receiver's
// behaviour; a test that disagreed with the old code is a bug in this file.
//
// Why it exists at all: T-2096 took five review rounds and found eleven real
// defects, and every one of them was in that file -- the one file in this
// subsystem with no host test on it, because it pulls in Arduino, NimBLE,
// HalStorage and the renderer. T-2106.
//
// Mirrors test/card_root_migration/, which does the same thing for the card
// root rename: a pure decision function tested without a card, the I/O that
// acts on it left to the board.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "MapTransferDecide.h"
#include "MapTransferPath.h"

namespace {

using maptransfer::BeginAction;
using maptransfer::BeginDecision;
using maptransfer::BeginGate;
using maptransfer::BeginGateFacts;
using maptransfer::BeginHeader;
using maptransfer::BeginTarget;
using maptransfer::ChunkAction;
using maptransfer::ChunkDecision;
using maptransfer::ChunkFacts;
using maptransfer::ChunkGate;
using maptransfer::FrameAction;
using maptransfer::WalletGate;
using maptransfer::WalletGateFacts;

// A begin frame body (everything past the opcode), built the way the phone
// builds it: u32 total, u32 crc, u8 pathLen, path bytes.
std::vector<uint8_t> beginBody(const uint32_t total, const uint32_t crc, const std::string& path,
                               const int pathLenByteOverride = -1) {
  std::vector<uint8_t> body;
  for (int i = 0; i < 4; ++i) body.push_back(static_cast<uint8_t>(total >> (8 * i)));
  for (int i = 0; i < 4; ++i) body.push_back(static_cast<uint8_t>(crc >> (8 * i)));
  body.push_back(static_cast<uint8_t>(pathLenByteOverride >= 0 ? pathLenByteOverride : path.size()));
  for (const char c : path) body.push_back(static_cast<uint8_t>(c));
  return body;
}

const char* parseBegin(const std::vector<uint8_t>& body, BeginHeader& out) {
  return maptransfer::parseBeginHeader(body.data(), body.size(), out);
}

// The facts of a begin that is allowed through: a safe path, somebody
// listening, nothing in flight, an ordinary tile.
BeginGateFacts goodGates() {
  BeginGateFacts f;
  f.pathSafe = true;
  f.statusSubscribed = true;
  f.transferActive = false;
  f.msSinceLastFrame = 0;
  f.staleAfterMs = 30000;
  f.target = BeginTarget::Plain;
  return f;
}

WalletGateFacts walletFacts(const BeginTarget target) {
  WalletGateFacts f;
  f.target = target;
  f.walletUnlocked = true;
  f.havePanel = true;
  f.declaredTotal = 1024;
  f.manifestCapBytes = 32000;
  return f;
}

ChunkFacts chunkFacts(const uint32_t offset, const uint32_t received, const uint32_t total, const size_t payload) {
  ChunkFacts f;
  f.transferActive = true;
  f.bodyLen = maptransfer::kChunkHeaderBytes + payload;
  f.offset = offset;
  f.received = received;
  f.declaredTotal = total;
  return f;
}

}  // namespace

// --- The frame -------------------------------------------------------------

TEST(TransferFrame, NothingHappensBeforeAttach) {
  // Answered with silence and before the frame guard is taken, so this is not
  // the same "nothing" as an abort with no transfer behind it: that one still
  // stamps lastFrameMs_ and publishes.
  const uint8_t frame[] = {maptransfer::kOpBegin};
  EXPECT_EQ(maptransfer::decideFrame(false, frame, sizeof(frame)), FrameAction::Ignore);
}

TEST(TransferFrame, AnEmptyOrAbsentFrameIsIgnored) {
  const uint8_t frame[] = {maptransfer::kOpBegin};
  EXPECT_EQ(maptransfer::decideFrame(true, nullptr, 4), FrameAction::Ignore);
  EXPECT_EQ(maptransfer::decideFrame(true, frame, 0), FrameAction::Ignore);
}

TEST(TransferFrame, TheThreeOpcodesDispatch) {
  const uint8_t begin[] = {0x01, 0xff};
  const uint8_t chunk[] = {0x02, 0xff};
  const uint8_t abort[] = {0x03};
  EXPECT_EQ(maptransfer::decideFrame(true, begin, sizeof(begin)), FrameAction::Begin);
  EXPECT_EQ(maptransfer::decideFrame(true, chunk, sizeof(chunk)), FrameAction::Chunk);
  EXPECT_EQ(maptransfer::decideFrame(true, abort, sizeof(abort)), FrameAction::Abort);
}

TEST(TransferFrame, EveryOtherOpcodeIsRefused) {
  for (int op = 0; op < 256; ++op) {
    if (op == 0x01 || op == 0x02 || op == 0x03) continue;
    const uint8_t frame[] = {static_cast<uint8_t>(op), 0x00};
    EXPECT_EQ(maptransfer::decideFrame(true, frame, sizeof(frame)), FrameAction::BadOpcode) << "opcode " << op;
  }
}

// --- The begin frame's header ----------------------------------------------

TEST(TransferBeginHeader, AFrameTooShortToHoldTheFixedPartIsShortBegin) {
  // Nine bytes is the fixed part exactly, and a begin frame also needs at least
  // one path byte -- so the boundary is at ten, not nine.
  const std::vector<uint8_t> full = beginBody(100, 0xdeadbeef, "a");
  ASSERT_EQ(full.size(), maptransfer::kBeginFixedBytes + 1);
  for (size_t len = 0; len < full.size(); ++len) {
    BeginHeader h;
    EXPECT_STREQ(maptransfer::parseBeginHeader(full.data(), len, h), "short begin") << "len " << len;
  }
  BeginHeader h;
  EXPECT_EQ(maptransfer::parseBeginHeader(full.data(), full.size(), h), nullptr);
}

TEST(TransferBeginHeader, ANullBodyIsShortBegin) {
  BeginHeader h;
  EXPECT_STREQ(maptransfer::parseBeginHeader(nullptr, 64, h), "short begin");
}

TEST(TransferBeginHeader, AZeroLengthPathIsRefused) {
  BeginHeader h;
  // pathLen 0 with a byte of path behind it: the length byte is what is read,
  // and zero is never a path.
  const std::vector<uint8_t> body = beginBody(100, 0, "a", 0);
  EXPECT_STREQ(parseBegin(body, h), "bad path length");
}

TEST(TransferBeginHeader, APathLongerThanTheProtocolAllowsIsRefused) {
  BeginHeader h;
  const std::string longest(maptransfer::kMaxRelPathBytes, 'a');
  EXPECT_EQ(parseBegin(beginBody(100, 0, longest), h), nullptr);
  const std::string tooLong(maptransfer::kMaxRelPathBytes + 1, 'a');
  EXPECT_STREQ(parseBegin(beginBody(100, 0, tooLong), h), "bad path length");
}

TEST(TransferBeginHeader, TheLengthMustBeExactNotAtLeast) {
  // Trailing bytes in a begin frame mean the sender and this parser disagree
  // about the format, and guessing which one is right is how a wire format
  // rots. Both directions are refused.
  BeginHeader h;
  std::vector<uint8_t> trailing = beginBody(100, 0, "a/b.tib");
  trailing.push_back(0x00);
  EXPECT_STREQ(parseBegin(trailing, h), "bad path length");

  // The declared path length is longer than the bytes actually sent.
  const std::vector<uint8_t> truncated = beginBody(100, 0, "a/b.tib", 9);
  EXPECT_STREQ(parseBegin(truncated, h), "bad path length");
}

TEST(TransferBeginHeader, ADeclaredLengthOfZeroOrOverTheCeilingIsRefused) {
  BeginHeader h;
  EXPECT_STREQ(parseBegin(beginBody(0, 0, "a.tib"), h), "bad length");
  EXPECT_STREQ(parseBegin(beginBody(maptransfer::kMaxFileBytes + 1, 0, "a.tib"), h), "bad length");
  // The ceiling itself is accepted -- it is a sanity bound, not an off-by-one.
  EXPECT_EQ(parseBegin(beginBody(maptransfer::kMaxFileBytes, 0, "a.tib"), h), nullptr);
  EXPECT_EQ(parseBegin(beginBody(1, 0, "a.tib"), h), nullptr);
}

TEST(TransferBeginHeader, ThePathLengthIsCheckedBeforeTheDeclaredLength) {
  // A frame that is wrong in both ways answers for the path, not the length.
  // The order is what the receiver has always run and what a sender's own
  // tests will have recorded.
  BeginHeader h;
  EXPECT_STREQ(parseBegin(beginBody(0, 0, "a", 0), h), "bad path length");
}

TEST(TransferBeginHeader, TheFieldsComeOutLittleEndianAndThePathIsNotCopied) {
  BeginHeader h;
  // A total with four distinct bytes that is still under the 8 MB ceiling --
  // the obvious 0x01020304 is 16 MB and the parser refuses it, which is the
  // first thing this suite caught.
  const std::vector<uint8_t> body = beginBody(0x007f3c01, 0xdeadbeef, "base/13/4482/2789.tib");
  ASSERT_EQ(parseBegin(body, h), nullptr);
  EXPECT_EQ(h.total, 0x007f3c01u);
  EXPECT_EQ(h.crc, 0xdeadbeefu);
  EXPECT_EQ(h.pathLen, std::strlen("base/13/4482/2789.tib"));
  // Points into the caller's frame buffer, nul terminator not included: the
  // receiver copies it into a bounded local and terminates it there.
  ASSERT_EQ(h.path, body.data() + maptransfer::kBeginFixedBytes);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(h.path), h.pathLen), "base/13/4482/2789.tib");
}

// --- The begin ladder ------------------------------------------------------

TEST(TransferBeginGates, AnOrdinaryTilePushIsLetThrough) {
  const BeginDecision d = maptransfer::decideBeginGates(goodGates());
  EXPECT_EQ(d.action, BeginAction::Proceed);
  EXPECT_EQ(d.reason, nullptr);
}

TEST(TransferBeginGates, AnUnsafePathIsRefused) {
  BeginGateFacts f = goodGates();
  f.pathSafe = false;
  const BeginDecision d = maptransfer::decideBeginGates(f);
  EXPECT_EQ(d.action, BeginAction::Refuse);
  EXPECT_STREQ(d.reason, "bad path");
}

TEST(TransferBeginGates, NobodyListeningMeansNoTransfer) {
  // Starting a transfer with nobody subscribed means writing a file and never
  // being able to say whether it is good, which is worse than not starting.
  BeginGateFacts f = goodGates();
  f.statusSubscribed = false;
  const BeginDecision d = maptransfer::decideBeginGates(f);
  EXPECT_EQ(d.action, BeginAction::Refuse);
  EXPECT_STREQ(d.reason, "status not subscribed");
}

TEST(TransferBeginGates, ASecondBeginDuringALiveTransferIsBusy) {
  BeginGateFacts f = goodGates();
  f.transferActive = true;
  f.msSinceLastFrame = 0;
  const BeginDecision d = maptransfer::decideBeginGates(f);
  EXPECT_EQ(d.action, BeginAction::Refuse);
  EXPECT_STREQ(d.reason, "busy");
}

TEST(TransferBeginGates, TheStaleBoundaryIsExact) {
  BeginGateFacts f = goodGates();
  f.transferActive = true;
  f.staleAfterMs = 30000;

  f.msSinceLastFrame = 29999;
  EXPECT_STREQ(maptransfer::decideBeginGates(f).reason, "busy");

  // At the bound it is reclaimed, not defended: a sender that died without
  // disconnecting would otherwise leave this stuck busy for ever.
  f.msSinceLastFrame = 30000;
  EXPECT_EQ(maptransfer::decideBeginGates(f).action, BeginAction::ReclaimStaleThenProceed);
  f.msSinceLastFrame = 60000;
  EXPECT_EQ(maptransfer::decideBeginGates(f).action, BeginAction::ReclaimStaleThenProceed);
}

TEST(TransferBeginGates, AReclaimStillRunsTheGatesBelowIt) {
  // The reclaim is not an early exit. A stale transfer plus a wallet path this
  // device does not write is still `ERR not a wallet file`, and the stale
  // transfer is left alone -- refuse() does not touch a transfer in flight.
  BeginGateFacts f = goodGates();
  f.transferActive = true;
  f.staleAfterMs = 30000;
  f.msSinceLastFrame = 45000;
  f.target = BeginTarget::WalletForeign;
  const BeginDecision d = maptransfer::decideBeginGates(f);
  EXPECT_EQ(d.action, BeginAction::Refuse);
  EXPECT_STREQ(d.reason, "not a wallet file");
}

TEST(TransferBeginGates, AWalletPathThisDeviceDoesNotWriteIsRefused) {
  // Before anything touches the card, including the directory creation, which
  // creates every parent component and cannot be undone: `wallet/manifest.bak/x`
  // used to make a *directory* called `manifest.bak`, and Storage.remove()
  // cannot delete one.
  BeginGateFacts f = goodGates();
  f.target = BeginTarget::WalletForeign;
  const BeginDecision d = maptransfer::decideBeginGates(f);
  EXPECT_EQ(d.action, BeginAction::Refuse);
  EXPECT_STREQ(d.reason, "not a wallet file");
}

TEST(TransferBeginGates, TheThreeRealWalletTargetsPassThisLadder) {
  for (const BeginTarget t : {BeginTarget::Plain, BeginTarget::WalletAsset, BeginTarget::WalletManifest}) {
    BeginGateFacts f = goodGates();
    f.target = t;
    EXPECT_EQ(maptransfer::decideBeginGates(f).action, BeginAction::Proceed) << "target " << static_cast<int>(t);
  }
}

TEST(TransferBeginGates, TheLadderOrderIsPathThenSubscriptionThenBusyThenWallet) {
  // Every gate failing at once. Each step answers for itself and the one after
  // it never runs -- this is the order the phone's own error handling was
  // written against, and it is the order a reviewer reads the file in.
  BeginGateFacts f;
  f.pathSafe = false;
  f.statusSubscribed = false;
  f.transferActive = true;
  f.msSinceLastFrame = 0;
  f.staleAfterMs = 30000;
  f.target = BeginTarget::WalletForeign;
  EXPECT_STREQ(maptransfer::decideBeginGates(f).reason, "bad path");

  f.pathSafe = true;
  EXPECT_STREQ(maptransfer::decideBeginGates(f).reason, "status not subscribed");

  f.statusSubscribed = true;
  EXPECT_STREQ(maptransfer::decideBeginGates(f).reason, "busy");

  f.transferActive = false;
  EXPECT_STREQ(maptransfer::decideBeginGates(f).reason, "not a wallet file");

  f.target = BeginTarget::Plain;
  EXPECT_EQ(maptransfer::decideBeginGates(f).action, BeginAction::Proceed);
}

// --- The wallet's gates ----------------------------------------------------

TEST(TransferWalletGates, APlainPushIsNoneOfTheWalletsBusiness) {
  WalletGateFacts f = walletFacts(BeginTarget::Plain);
  f.walletUnlocked = false;
  f.havePanel = false;
  f.declaredTotal = 8u * 1024u * 1024u;
  EXPECT_EQ(maptransfer::decideWalletGates(f).gate, WalletGate::None);
}

TEST(TransferWalletGates, ALockedWalletRefusesAnAssetRatherThanStoringPlaintext) {
  // Storing the plaintext "for now" is the one outcome this rung exists to
  // prevent.
  WalletGateFacts f = walletFacts(BeginTarget::WalletAsset);
  f.walletUnlocked = false;
  EXPECT_STREQ(maptransfer::decideWalletGates(f).reason, "wallet locked");
}

TEST(TransferWalletGates, ALockedWalletRefusesTheManifestToo) {
  WalletGateFacts f = walletFacts(BeginTarget::WalletManifest);
  f.walletUnlocked = false;
  EXPECT_STREQ(maptransfer::decideWalletGates(f).reason, "wallet locked");
}

TEST(TransferWalletGates, AnAssetNeedsALivePanelForItsGeometryCheck) {
  WalletGateFacts f = walletFacts(BeginTarget::WalletAsset);
  f.havePanel = false;
  EXPECT_STREQ(maptransfer::decideWalletGates(f).reason, "no panel");
}

TEST(TransferWalletGates, TheManifestNeedsNoPanel) {
  // It is a list, not a bitmap -- nothing about it is checked against the
  // screen, so a screen with no renderer may still take one.
  WalletGateFacts f = walletFacts(BeginTarget::WalletManifest);
  f.havePanel = false;
  EXPECT_EQ(maptransfer::decideWalletGates(f).gate, WalletGate::None);
}

TEST(TransferWalletGates, TheManifestCapIsCheckedBeforeAByteIsTaken) {
  // The begin frame carries the length, so an oversized manifest costs nothing
  // on the wire.
  WalletGateFacts f = walletFacts(BeginTarget::WalletManifest);
  f.manifestCapBytes = 32000;

  f.declaredTotal = 32000;
  EXPECT_EQ(maptransfer::decideWalletGates(f).gate, WalletGate::None);
  f.declaredTotal = 32001;
  EXPECT_STREQ(maptransfer::decideWalletGates(f).reason, "manifest too big");
}

TEST(TransferWalletGates, AnAssetHasNoManifestCap) {
  WalletGateFacts f = walletFacts(BeginTarget::WalletAsset);
  f.manifestCapBytes = 32000;
  f.declaredTotal = 2u * 1024u * 1024u;
  EXPECT_EQ(maptransfer::decideWalletGates(f).gate, WalletGate::None);
}

TEST(TransferWalletGates, LockBeatsEveryOtherWalletRefusal) {
  // A locked wallet answers "wallet locked" whatever else is wrong, on both
  // paths. The rider's fix is the same either way: unlock.
  WalletGateFacts asset = walletFacts(BeginTarget::WalletAsset);
  asset.walletUnlocked = false;
  asset.havePanel = false;
  EXPECT_STREQ(maptransfer::decideWalletGates(asset).reason, "wallet locked");

  WalletGateFacts manifest = walletFacts(BeginTarget::WalletManifest);
  manifest.walletUnlocked = false;
  manifest.declaredTotal = 1u << 20;
  manifest.manifestCapBytes = 32000;
  EXPECT_STREQ(maptransfer::decideWalletGates(manifest).reason, "wallet locked");
}

// --- The chunk state machine -----------------------------------------------

TEST(TransferChunk, AChunkWithNoTransferBehindItIsRefusedNotAbandoned) {
  // Refused, not abandoned: there is nothing in flight to tear down, and
  // abandon() would send a second verdict for a transfer that already ended.
  ChunkFacts f = chunkFacts(0, 0, 1024, 32);
  f.transferActive = false;
  const ChunkDecision d = maptransfer::decideChunk(f);
  EXPECT_EQ(d.action, ChunkAction::Refuse);
  EXPECT_STREQ(d.reason, "no transfer");
}

TEST(TransferChunk, AFrameWithNoPayloadEndsTheTransfer) {
  // Four bytes is the offset and nothing else. `<=`, not `<`.
  for (size_t bodyLen = 0; bodyLen <= maptransfer::kChunkHeaderBytes; ++bodyLen) {
    ChunkFacts f = chunkFacts(0, 0, 1024, 0);
    f.bodyLen = bodyLen;
    const ChunkDecision d = maptransfer::decideChunk(f);
    EXPECT_EQ(d.action, ChunkAction::Abandon) << "bodyLen " << bodyLen;
    EXPECT_STREQ(d.reason, "short chunk");
  }
  const ChunkDecision ok = maptransfer::decideChunk(chunkFacts(0, 0, 1024, 1));
  EXPECT_EQ(ok.action, ChunkAction::Accept);
  EXPECT_EQ(ok.payloadLen, 1u);
}

TEST(TransferChunk, ARepeatedChunkIsAbandoned) {
  // The offset is checked against bytes actually written, so one comparison
  // catches a repeat, a gap and a reorder.
  const ChunkDecision d = maptransfer::decideChunk(chunkFacts(0, 64, 1024, 64));
  EXPECT_EQ(d.action, ChunkAction::Abandon);
  EXPECT_STREQ(d.reason, "offset");
}

TEST(TransferChunk, AGapIsAbandoned) {
  const ChunkDecision d = maptransfer::decideChunk(chunkFacts(128, 64, 1024, 64));
  EXPECT_EQ(d.action, ChunkAction::Abandon);
  EXPECT_STREQ(d.reason, "offset");
}

TEST(TransferChunk, MoreBytesThanWereDeclaredIsAbandoned) {
  const ChunkDecision d = maptransfer::decideChunk(chunkFacts(900, 900, 1024, 200));
  EXPECT_EQ(d.action, ChunkAction::Abandon);
  EXPECT_STREQ(d.reason, "overrun");
}

TEST(TransferChunk, TheOffsetIsCheckedBeforeTheOverrun) {
  // A chunk that is both out of order and too big answers for the offset. The
  // transfer is already wrong by then and the second answer would say less.
  ChunkFacts f = chunkFacts(0, 900, 1024, 500);
  EXPECT_STREQ(maptransfer::decideChunk(f).reason, "offset");
}

TEST(TransferChunk, TheTransferBeingDeadBeatsEveryOtherChunkRefusal) {
  ChunkFacts f = chunkFacts(999, 0, 1024, 0);
  f.transferActive = false;
  f.bodyLen = 0;
  EXPECT_STREQ(maptransfer::decideChunk(f).reason, "no transfer");
}

TEST(TransferChunk, AnOrdinaryChunkIsAcceptedAndCarriesItsPayloadLength) {
  const ChunkDecision d = maptransfer::decideChunk(chunkFacts(64, 64, 1024, 180));
  EXPECT_EQ(d.action, ChunkAction::Accept);
  EXPECT_EQ(d.reason, nullptr);
  EXPECT_EQ(d.payloadLen, 180u);
  EXPECT_FALSE(d.isFirst);
  EXPECT_FALSE(d.completesTransfer);
}

TEST(TransferChunk, OnlyOffsetZeroIsTheFirstChunk) {
  EXPECT_TRUE(maptransfer::decideChunk(chunkFacts(0, 0, 1024, 180)).isFirst);
  EXPECT_FALSE(maptransfer::decideChunk(chunkFacts(180, 180, 1024, 180)).isFirst);
}

TEST(TransferChunk, TheLastChunkIsTheOneThatFitsExactly) {
  const ChunkDecision d = maptransfer::decideChunk(chunkFacts(900, 900, 1024, 124));
  EXPECT_EQ(d.action, ChunkAction::Accept);
  EXPECT_TRUE(d.completesTransfer);
  // One byte short is not the last chunk, and one byte long is an overrun --
  // the completion test has no slack on either side.
  EXPECT_FALSE(maptransfer::decideChunk(chunkFacts(900, 900, 1024, 123)).completesTransfer);
  EXPECT_EQ(maptransfer::decideChunk(chunkFacts(900, 900, 1024, 125)).action, ChunkAction::Abandon);
}

TEST(TransferChunk, AWholeFileInOneChunkIsBothFirstAndLast) {
  const ChunkDecision d = maptransfer::decideChunk(chunkFacts(0, 0, 200, 200));
  EXPECT_EQ(d.action, ChunkAction::Accept);
  EXPECT_TRUE(d.isFirst);
  EXPECT_TRUE(d.completesTransfer);
}

TEST(TransferChunk, AWholeTransferWalksToCompletionOneChunkAtATime) {
  // The state machine driven end to end the way the phone drives it: 23-byte
  // payloads (the default MTU's worth) against a 1,024-byte file. Nothing but
  // the final chunk completes, and the offsets never need a sequence number.
  constexpr uint32_t kTotal = 1024;
  constexpr size_t kPayload = 23;
  uint32_t received = 0;
  int chunks = 0;
  while (received < kTotal) {
    const size_t payload = std::min<size_t>(kPayload, kTotal - received);
    const ChunkDecision d = maptransfer::decideChunk(chunkFacts(received, received, kTotal, payload));
    ASSERT_EQ(d.action, ChunkAction::Accept) << "at " << received;
    EXPECT_EQ(d.isFirst, received == 0);
    received += static_cast<uint32_t>(d.payloadLen);
    EXPECT_EQ(d.completesTransfer, received == kTotal) << "at " << received;
    ++chunks;
  }
  EXPECT_EQ(received, kTotal);
  EXPECT_EQ(chunks, 45);  // 44 full payloads plus a 12-byte tail
}

TEST(TransferChunk, AChunkArrivingAfterTheFileIsCompleteIsAnOverrun) {
  // received == declaredTotal and the transfer is somehow still active: every
  // further chunk is refused by arithmetic rather than by a flag.
  const ChunkDecision d = maptransfer::decideChunk(chunkFacts(1024, 1024, 1024, 1));
  EXPECT_EQ(d.action, ChunkAction::Abandon);
  EXPECT_STREQ(d.reason, "overrun");
}

// --- The contract itself ---------------------------------------------------

TEST(TransferDecisions, EveryReasonThisLayerCanGiveIsOneOfTheReceiversOwnStrings) {
  // These fifteen strings are what a rider reads on the phone: the companion
  // strips `ERR ` and shows the rest (TransferFrames.kt). So they are
  // user-visible text and not a parsed contract -- which is exactly why a move
  // may not quietly reword one. Byte for byte against
  // MapTransferReceiver.cpp at c9f4bdba.
  //
  // The ones the decision layer does NOT own stay at the call site, because
  // their answer is an allocation, a card operation or the sender's own bytes:
  // `oom`, `mkdir failed`, `open failed`, `write failed`, `readback failed`,
  // `crc mismatch`, `replace failed`, `rename failed`, `aborted`, and the
  // wallet ingest and install reasons.
  const std::vector<std::string> owned = {
      "bad opcode",  "short begin",   "bad path length",  "bad length", "bad path",          "status not subscribed",
      "busy",        "wallet locked", "manifest too big", "no panel",   "not a wallet file", "no transfer",
      "short chunk", "offset",        "overrun",
  };
  std::vector<std::string> seen;

  const uint8_t badOp[] = {0x7f};
  if (maptransfer::decideFrame(true, badOp, 1) == FrameAction::BadOpcode) seen.emplace_back("bad opcode");

  BeginHeader h;
  const uint8_t empty[] = {0};
  seen.emplace_back(maptransfer::parseBeginHeader(empty, 1, h));
  seen.emplace_back(parseBegin(beginBody(100, 0, "a", 0), h));
  seen.emplace_back(parseBegin(beginBody(0, 0, "a.tib"), h));

  BeginGateFacts g = goodGates();
  g.pathSafe = false;
  seen.emplace_back(maptransfer::decideBeginGates(g).reason);
  g = goodGates();
  g.statusSubscribed = false;
  seen.emplace_back(maptransfer::decideBeginGates(g).reason);
  g = goodGates();
  g.transferActive = true;
  seen.emplace_back(maptransfer::decideBeginGates(g).reason);
  g = goodGates();
  g.target = BeginTarget::WalletForeign;
  seen.emplace_back(maptransfer::decideBeginGates(g).reason);

  WalletGateFacts w = walletFacts(BeginTarget::WalletAsset);
  w.walletUnlocked = false;
  seen.emplace_back(maptransfer::decideWalletGates(w).reason);
  w = walletFacts(BeginTarget::WalletAsset);
  w.havePanel = false;
  seen.emplace_back(maptransfer::decideWalletGates(w).reason);
  w = walletFacts(BeginTarget::WalletManifest);
  w.declaredTotal = w.manifestCapBytes + 1;
  seen.emplace_back(maptransfer::decideWalletGates(w).reason);

  ChunkFacts c = chunkFacts(0, 0, 1024, 32);
  c.transferActive = false;
  seen.emplace_back(maptransfer::decideChunk(c).reason);
  c = chunkFacts(0, 0, 1024, 0);
  seen.emplace_back(maptransfer::decideChunk(c).reason);
  seen.emplace_back(maptransfer::decideChunk(chunkFacts(8, 0, 1024, 32)).reason);
  seen.emplace_back(maptransfer::decideChunk(chunkFacts(0, 0, 16, 32)).reason);

  for (const std::string& reason : seen) {
    EXPECT_NE(std::find(owned.begin(), owned.end(), reason), owned.end()) << "unexpected reason: " << reason;
  }
  // Every string in the list is reachable, so the list cannot rot into a
  // superset of what the code can actually say.
  for (const std::string& want : owned) {
    EXPECT_NE(std::find(seen.begin(), seen.end(), want), seen.end()) << "unreachable reason: " << want;
  }
}

TEST(TransferDecisions, TheGateAndTheReasonAlwaysAgree) {
  // The receiver switches on the gate, not on the string, because two begin
  // refusals also carry the offending path into the log and `manifest too big`
  // is logged with both numbers -- things a reason string cannot say. That only
  // holds while the two stay in step, so this is the test that keeps them there.
  BeginGateFacts g = goodGates();
  EXPECT_EQ(maptransfer::decideBeginGates(g).gate, BeginGate::None);
  g.pathSafe = false;
  EXPECT_EQ(maptransfer::decideBeginGates(g).gate, BeginGate::UnsafePath);
  g = goodGates();
  g.statusSubscribed = false;
  EXPECT_EQ(maptransfer::decideBeginGates(g).gate, BeginGate::NotSubscribed);
  g = goodGates();
  g.transferActive = true;
  EXPECT_EQ(maptransfer::decideBeginGates(g).gate, BeginGate::Busy);
  g = goodGates();
  g.target = BeginTarget::WalletForeign;
  EXPECT_EQ(maptransfer::decideBeginGates(g).gate, BeginGate::ForeignWalletPath);

  // A reclaim is not a refusal, so it names no gate.
  g = goodGates();
  g.transferActive = true;
  g.msSinceLastFrame = g.staleAfterMs;
  EXPECT_EQ(maptransfer::decideBeginGates(g).action, BeginAction::ReclaimStaleThenProceed);
  EXPECT_EQ(maptransfer::decideBeginGates(g).gate, BeginGate::None);

  WalletGateFacts w = walletFacts(BeginTarget::WalletAsset);
  w.walletUnlocked = false;
  EXPECT_EQ(maptransfer::decideWalletGates(w).gate, WalletGate::Locked);
  w = walletFacts(BeginTarget::WalletAsset);
  w.havePanel = false;
  EXPECT_EQ(maptransfer::decideWalletGates(w).gate, WalletGate::NoPanel);
  w = walletFacts(BeginTarget::WalletManifest);
  w.declaredTotal = w.manifestCapBytes + 1;
  EXPECT_EQ(maptransfer::decideWalletGates(w).gate, WalletGate::ManifestTooBig);

  ChunkFacts c = chunkFacts(0, 0, 1024, 32);
  EXPECT_EQ(maptransfer::decideChunk(c).gate, ChunkGate::None);
  c.transferActive = false;
  EXPECT_EQ(maptransfer::decideChunk(c).gate, ChunkGate::NoTransfer);
  c = chunkFacts(0, 0, 1024, 0);
  EXPECT_EQ(maptransfer::decideChunk(c).gate, ChunkGate::ShortChunk);
  EXPECT_EQ(maptransfer::decideChunk(chunkFacts(8, 0, 1024, 32)).gate, ChunkGate::BadOffset);
  EXPECT_EQ(maptransfer::decideChunk(chunkFacts(0, 0, 16, 32)).gate, ChunkGate::Overrun);
}

TEST(TransferChunk, PayloadLengthAndFirstDescribeTheFrameNotTheVerdict) {
  // The receiver's first-chunk diagnostic prints the payload size and has
  // always run after the offset check and *before* the overrun check, so an
  // overrunning first chunk still logs its real size on the way to being
  // abandoned. Keeping that exact meant filling these two as soon as they are
  // known rather than only on Accept.
  const ChunkDecision overrunFirst = maptransfer::decideChunk(chunkFacts(0, 0, 16, 32));
  EXPECT_EQ(overrunFirst.action, ChunkAction::Abandon);
  EXPECT_EQ(overrunFirst.gate, ChunkGate::Overrun);
  EXPECT_EQ(overrunFirst.payloadLen, 32u);
  EXPECT_TRUE(overrunFirst.isFirst);
  EXPECT_FALSE(overrunFirst.completesTransfer);

  // A frame refused before the offset lines up is not a first chunk, whatever
  // offset it claims -- so it logs nothing.
  const ChunkDecision badOffset = maptransfer::decideChunk(chunkFacts(0, 64, 1024, 32));
  EXPECT_EQ(badOffset.gate, ChunkGate::BadOffset);
  EXPECT_FALSE(badOffset.isFirst);
  ChunkFacts shortFrame = chunkFacts(0, 0, 1024, 0);
  EXPECT_FALSE(maptransfer::decideChunk(shortFrame).isFirst);
  EXPECT_EQ(maptransfer::decideChunk(shortFrame).payloadLen, 0u);
}
