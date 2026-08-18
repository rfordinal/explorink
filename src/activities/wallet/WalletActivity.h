#pragma once

#include <cstdint>
#include <memory>

#include "WalletStore.h"
#include "activities/Activity.h"

// Browse: one row per document in the wallet manifest.
//
// UP/DOWN move the selection, CONFIRM opens the document in WalletViewActivity,
// BACK goes home. A missing, unreadable or empty manifest gets a row of text
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
  WalletActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

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

  std::unique_ptr<wallet::ItemEntry[]> entries_;
  // What the manifest says it was built for. Only read when the wallet is
  // refused for being built for another panel, where the message names both.
  wallet::DeclaredPanel declared_;
  uint16_t stored_ = 0;
  uint32_t seen_ = 0;
  wallet::Error error_ = wallet::Error::None;
  int selected_ = 0;
};
