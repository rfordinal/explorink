#include "LoraPeersActivity.h"

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "lora/LoraPresence.h"

void LoraPeersActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  refresh();
  requestUpdate();
}

void LoraPeersActivity::refresh() {
  const LoraPresenceStatus status = loraPresenceStatus();
  meshRunning_ = status.meshRunning;
  beaconSec_ = status.beaconSec;
  rejected_ = status.advertsRejected;
  snapshotMs_ = status.nowUptimeMs;
  memcpy(nodeName_, status.nodeName, sizeof(nodeName_));
  nodeName_[sizeof(nodeName_) - 1] = '\0';

  peerCount_ = loraPresenceSnapshot(peers_, LoraPeerTable::kMaxPeers);
  if (selectedIndex >= static_cast<int>(peerCount_)) selectedIndex = 0;
}

void LoraPeersActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Select is a reload, not an open: there is nothing behind a row. A node
  // heard here is not a group member -- making it one is `team add` with its
  // key, deliberately, because an allowlist that can be filled by tapping
  // whatever shouted loudest is not an allowlist (docs/team-markers.md).
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    refresh();
    requestUpdate();
    return;
  }

  const int itemCount = static_cast<int>(peerCount_);
  if (itemCount > 0) {
    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void LoraPeersActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // The subtitle is this node's own state, because every row below it is
  // meaningless without it: an empty list means "nobody is out there" only if
  // the radio was actually listening, and "the mesh is off" otherwise.
  char subtitle[64];
  if (!meshRunning_) {
    snprintf(subtitle, sizeof(subtitle), "%s", tr(STR_LORA_PEERS_MESH_OFF));
  } else if (beaconSec_ == 0) {
    snprintf(subtitle, sizeof(subtitle), tr(STR_LORA_PEERS_HEADER_OFF_FORMAT), nodeName_,
             static_cast<unsigned long>(rejected_));
  } else {
    snprintf(subtitle, sizeof(subtitle), tr(STR_LORA_PEERS_HEADER_FORMAT), nodeName_,
             static_cast<unsigned long>(beaconSec_), static_cast<unsigned long>(rejected_));
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LORA_PEERS), subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  if (peerCount_ == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LORA_PEERS_NONE));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(peerCount_), selectedIndex,
        [this](int index) -> std::string {
          const LoraPeer& peer = peers_[index];
          if (peer.name[0] != '\0') return std::string(peer.name);
          // No name in the advert, so the key is the only thing this node ever
          // told us. Four bytes of it: enough to tell two boards apart on a
          // bench, and it is a prefix on the screen only -- the table itself
          // matches on the whole key (src/lora/LoraPeerTable.h).
          char hex[16];
          snprintf(hex, sizeof(hex), "%02x%02x%02x%02x", peer.pubKey[0], peer.pubKey[1], peer.pubKey[2],
                   peer.pubKey[3]);
          return std::string(hex);
        },
        [this](int index) -> std::string {
          const LoraPeer& peer = peers_[index];
          char sub[64];
          snprintf(sub, sizeof(sub), tr(STR_LORA_PEERS_SUB_FORMAT), static_cast<int>(peer.rssiDbm),
                   static_cast<int>(peer.snrDb), static_cast<unsigned long>((snapshotMs_ - peer.heardUptimeMs) / 1000),
                   static_cast<unsigned>(peer.adverts));
          return std::string(sub);
        },
        nullptr,
        [this](int index) -> std::string {
          // The value column is the coordinate, and an empty one means they
          // have not told us where they are. Never 0,0 -- that is a place in
          // the Atlantic and a different statement entirely.
          const LoraPeer& peer = peers_[index];
          if (!peer.hasLoc) return std::string("");
          char loc[32];
          snprintf(loc, sizeof(loc), "%.4f %.4f", static_cast<double>(peer.latE6) / 1e6,
                   static_cast<double>(peer.lonE6) / 1e6);
          return std::string(loc);
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_LORA_PEERS_REFRESH), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // ENABLE_LORA_MESH
