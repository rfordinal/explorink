#pragma once

#include <cstdint>
#include <memory>

#include "WalletStore.h"
#include "activities/Activity.h"

// Browse: one row per document in the wallet manifest.
//
// UP/DOWN move the selection, CONFIRM opens the document in WalletViewActivity,
// LEFT/RIGHT open its machine-readable codes in WalletCodeActivity, BACK goes
// home. A missing, unreadable or empty manifest gets a row of text
// saying which of those it is -- never a blank screen and never a crash
// (../../../docs/wallet-viewer.md, "Failure states").
//
// Read-only. This screen has no delete, no rename, no reorder: the card is the
// only offline copy of the rider's documents.
//
// Modelled on RouteSelectActivity: same list geometry, same "rows come out of a
// file's own metadata, not its name", same heap-in-onEnter / free-in-onExit
// bracket for the row snapshot.
class WalletActivity final : public Activity {
 public:
  // `openItem` / `openCode` are what `CMD:GOTO_WALLET` asks for: -1 for "just the
  // list", an item index to open that document, and both to land straight on one
  // of its codes. Acted on once, in onEnter(), because that is the first moment
  // the manifest is known -- and refused with a message on screen if the index is
  // not in the wallet, never clamped (../../../docs/wallet-viewer.md,
  // "CMD:GOTO_WALLET").
  WalletActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int openItem = -1, int openCode = -1);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  // 24 rows is 1.2 KB. A wallet is a passport, a licence, an insurance card and
  // a few papers -- not a library. A manifest with more says so on screen
  // instead of silently hiding the rest.
  static constexpr uint16_t kMaxItems = 24;

  void renderScreen(HalDisplay::RefreshMode mode);
  void drawList();
  void drawRow(int index, int y, int rowHeight);
  void listRect(int& x, int& y, int& w, int& h) const;
  int visibleRowCount() const;
  int firstVisibleRow() const;
  int rowCount() const { return static_cast<int>(stored_); }
  void openSelected();
  // Steps the code ring from the browse list: `delta` +1 opens the selected item's
  // first code, -1 its last. wallet::walkCodeIndex() decides where that lands.
  void openCodeStep(int delta);
  // Opens one code of the selected item by index. False when the index is not in
  // the item's ring.
  bool openCodeAt(int codeIndex, uint16_t codeCount);
  // Acts on the CMD:GOTO_WALLET request, if there was one. Called from onEnter()
  // with the list already read. True when it started a child activity, so the
  // caller can skip painting a list nobody will see -- that would cost a whole
  // 1.7 s HALF refresh and flash the list on the way to the code.
  bool applyGotoTarget();

  std::unique_ptr<wallet::ItemEntry[]> entries_;
  // What the manifest says it was built for. Only read when the wallet is
  // refused for being built for another panel, where the message names both.
  wallet::DeclaredPanel declared_;
  uint16_t stored_ = 0;
  uint32_t seen_ = 0;
  wallet::Error error_ = wallet::Error::None;
  int selected_ = 0;
  // The CMD:GOTO_WALLET request, consumed once by applyGotoTarget().
  int openItem_ = -1;
  int openCode_ = -1;
  // Filled when a requested index was not in the wallet, so the status line can
  // say which one -- a host driving this over serial reads the panel, not a
  // return code.
  char gotoError_[64] = {0};
};
