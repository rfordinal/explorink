#include "LoraPeersActivity.h"

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "lora/LoraPresence.h"

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
#include "activities/map/MapTeam.h"
#endif

namespace {

// The next cadence in the ring, so Confirm walks off -> 70 s -> 2 min -> 5 min
// -> 15 min -> off. A cycling row rather than a picker because there are five
// values and a second screen for five values is a screen nobody wants.
uint32_t nextBeaconChoice(uint32_t current) {
  constexpr size_t n = sizeof(kLoraBeaconChoices) / sizeof(kLoraBeaconChoices[0]);
  for (size_t i = 0; i < n; ++i) {
    if (kLoraBeaconChoices[i] == current) return kLoraBeaconChoices[(i + 1) % n];
  }
  // A cadence set over the console that is not one of the five. Next press
  // lands on "off", which is the safe end of the ring.
  return 0;
}

// How old, in the words the row actually prints.
//
// **Coarse on purpose, and it is what makes the periodic check affordable.**
// A row that prints raw seconds changes every second, so a repaint-when-it
// changed rule would repaint every tick. Seconds while a node is fresh --
// which is when the rider wants to watch it -- then whole minutes, then fives,
// the same quantisation the map applies to team marker ages and for the same
// reason. It also fixes a readability complaint of its own: `4009s ago` is an
// hour and nobody reads it as one.
void formatAge(uint32_t seconds, char* buf, size_t bufLen) {
  if (seconds < 60) {
    snprintf(buf, bufLen, "%lus", static_cast<unsigned long>(seconds));
  } else if (seconds < 600) {
    snprintf(buf, bufLen, "%lum", static_cast<unsigned long>(seconds / 60));
  } else if (seconds < 3600) {
    snprintf(buf, bufLen, "%lum", static_cast<unsigned long>((seconds / 300) * 5));
  } else {
    snprintf(buf, bufLen, "%luh", static_cast<unsigned long>(seconds / 3600));
  }
}

}  // namespace

void LoraPeersActivity::onEnter() {
  Activity::onEnter();

  // The other screen the radio may run under. Without this the list would be
  // the one place you cannot watch the radio work, which is the opposite of
  // what it is for.
  loraPresenceHoldRadio(true);

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
  // Necessary here, not only on the map: this screen can be the first thing to
  // touch the roster in a session that never opened the map.
  MapTeam::getInstance().begin();
#endif

  selectedIndex = 0;
  lastCheckMs_ = millis();
  refresh();
  drawnSignature_ = listSignature();
  requestUpdate();
}

void LoraPeersActivity::onExit() {
  loraPresenceHoldRadio(false);
  Activity::onExit();
}

int LoraPeersActivity::itemCount() const {
  int n = kControlRowCount + static_cast<int>(peerCount_);
#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
  n += static_cast<int>(rosterOnlyCount_);
#endif
  return n;
}

LoraPeersActivity::RowKind LoraPeersActivity::rowKind(int index) const {
  if (index < kControlRowCount) return RowKind::Control;
  if (index < rosterBase()) return RowKind::HeardPeer;
  return RowKind::RosterOnly;
}

void LoraPeersActivity::refresh() {
  const LoraPresenceStatus status = loraPresenceStatus();
  meshRunning_ = status.meshRunning;
  beaconSec_ = status.beaconSec;
  rejected_ = status.advertsRejected;
  identityPersisted_ = status.identityPersisted;
  snapshotMs_ = status.nowUptimeMs;
  memcpy(nodeName_, status.nodeName, sizeof(nodeName_));
  nodeName_[sizeof(nodeName_) - 1] = '\0';

  peerCount_ = loraPresenceSnapshot(peers_, LoraPeerTable::kMaxPeers);

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
  // Which heard nodes are already members, and which members nobody is
  // hearing. Both answers are computed once here rather than per drawn row:
  // the roster walk is twelve entries and the draw runs on every repaint.
  MapTeam& team = MapTeam::getInstance();
  bool memberHeard[kTeamMaxMembers] = {};
  const size_t members = team.teamCount();

  for (size_t i = 0; i < peerCount_; ++i) {
    peerAcr_[i][0] = '\0';
    char id[kLoraTeamIdBytes];
    loraTeamId(peers_[i].pubKey, id, sizeof(id));
    const size_t slot = team.roster().findId(id);
    if (slot >= TeamRoster::kSlotCount) continue;
    snprintf(peerAcr_[i], sizeof(peerAcr_[i]), "%s", team.roster().at(slot).acr);
    for (size_t k = 0; k < members && k < kTeamMaxMembers; ++k) {
      if (team.slotForIndex(k) == slot) memberHeard[k] = true;
    }
  }

  rosterOnlyCount_ = 0;
  for (size_t k = 0; k < members && k < kTeamMaxMembers; ++k) {
    if (memberHeard[k]) continue;
    rosterOnly_[rosterOnlyCount_++] = static_cast<uint8_t>(k);
  }
#endif

  if (selectedIndex >= itemCount()) selectedIndex = 0;
}

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
void LoraPeersActivity::addHeardPeer(size_t peerIndex) {
  const LoraPeer& peer = peers_[peerIndex];

  char id[kLoraTeamIdBytes];
  loraTeamId(peer.pubKey, id, sizeof(id));
  char guess[kTeamAcrBytes];
  loraTeamAcrGuess(peer.name[0] != '\0' ? peer.name : nullptr, peer.pubKey, guess, sizeof(guess));

  // **Derived, then offered.** The acronym sits on a map balloon for as long
  // as that person is in the group, and it is guessed from a string they chose
  // for something else -- so the guess saves the common case and the keyboard
  // keeps the uncommon one from being wrong forever (maintainer's call,
  // 2026-10-22: derive it, but allow changing it).
  const std::string name(peer.name);
  const std::string idStr(id);
  auto handler = [this, idStr, name](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto& kb = std::get<KeyboardResult>(result.data);
      if (MapTeam::getInstance().teamAdd(kb.text, idStr, name) != IMapTeamSource::Edit::Ok) {
        // The roster refuses a duplicate acronym, a crossing id pair and a
        // full table, and from here they mean one thing to the rider: pick
        // another short name, or make room.
        lastError_ = StrId::STR_LORA_TEAM_ADD_FAILED;
        hasError_ = true;
      }
    }
    refresh();
    requestUpdate();
  };

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LORA_TEAM_ADD), std::string(guess),
                                              kTeamAcrBytes - 1, InputType::Text),
      handler);
}

void LoraPeersActivity::removeMember(const std::string& idOrAcr, const std::string& label) {
  // **Confirmed, because this is the destructive one.** Removing a member
  // drops the allowlist entry their positions arrive through, so the next
  // thing they send is refused as a stranger. That is the point of the row,
  // and it is not something a mis-aimed Select should do.
  auto handler = [this, idOrAcr](const ActivityResult& result) {
    if (!result.isCancelled) {
      if (MapTeam::getInstance().teamRemove(idOrAcr) != IMapTeamSource::Edit::Ok) {
        lastError_ = StrId::STR_LORA_TEAM_REMOVE_FAILED;
        hasError_ = true;
      }
    }
    refresh();
    requestUpdate();
  };
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_LORA_TEAM_REMOVE), label),
                         handler);
}
#endif  // ENABLE_TEAM_MARKERS

void LoraPeersActivity::activate(int index) {
  hasError_ = false;

  const RowKind kind = rowKind(index);
#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
  if (kind == RowKind::HeardPeer) {
    const size_t i = static_cast<size_t>(index - kControlRowCount);
    if (peerAcr_[i][0] == '\0') {
      addHeardPeer(i);
    } else {
      removeMember(std::string(peerAcr_[i]), rowTitle(index));
    }
    return;  // the sub-activity's handler refreshes
  }
  if (kind == RowKind::RosterOnly) {
    const size_t k = rosterOnly_[static_cast<size_t>(index - rosterBase())];
    const TeamMember m = MapTeam::getInstance().teamMemberAt(k);
    removeMember(std::string(m.acr), rowTitle(index));
    return;
  }
#else
  // Without the group layer a peer row has nothing behind it.
  if (kind != RowKind::Control) return;
#endif

  switch (index) {
    case kRowMesh: {
      bool persisted = false;
      if (!loraPresenceSetMesh(!meshRunning_, persisted)) {
        // The radio refused: rail, reset, or the TCXO never started. Named on
        // the panel rather than logged, because the person pressing this has
        // no serial console in their hand.
        lastError_ = StrId::STR_LORA_RADIO_FAILED;
        hasError_ = true;
      } else if (!meshRunning_ && !persisted) {
        // It started, and the key is RAM-only. Not a failure -- it works
        // today -- but every reboot then makes this a new node to everyone
        // who heard it, which the rider has to know.
        lastError_ = StrId::STR_LORA_KEY_NOT_STORED;
        hasError_ = true;
      }
      break;
    }
    case kRowBeacon:
      // The floor is enforced in LoraPresence, and the ring never offers a
      // value under it, so this cannot be refused from here.
      loraPresenceSetBeacon(nextBeaconChoice(beaconSec_));
      break;
    case kRowName: {
      auto handler = [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& kb = std::get<KeyboardResult>(result.data);
          loraPresenceSetName(kb.text.c_str());
        }
        refresh();
        requestUpdate();
      };
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(
                                 renderer, mappedInput, tr(STR_LORA_NAME_ROW), std::string(SETTINGS.loraNodeName),
                                 sizeof(SETTINGS.loraNodeName) - 1, InputType::Text),
                             handler);
      return;  // refresh happens in the handler, after the keyboard closes
    }
    case kRowAdvert:
      if (!loraPresenceAdvertNow()) {
        lastError_ = StrId::STR_LORA_PEERS_MESH_OFF;
        hasError_ = true;
      }
      break;
    default:
      break;
  }

  refresh();
  requestUpdate();
}

void LoraPeersActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activate(selectedIndex);
    return;
  }

  // The periodic check. Re-reads the radio and repaints only when the drawn
  // text would differ -- a node arriving while the rider watches is the whole
  // point, and an age ticking over is not worth a panel refresh.
  const uint32_t now = millis();
  if (now - lastCheckMs_ >= kCheckIntervalMs) {
    lastCheckMs_ = now;
    refresh();
    const uint32_t sig = listSignature();
    if (sig != drawnSignature_) {
      drawnSignature_ = sig;
      requestUpdate();
      return;
    }
  }

  const int count = itemCount();
  buttonNavigator.onNext([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });
}

std::string LoraPeersActivity::rowTitle(int index) const {
  const RowKind kind = rowKind(index);

  if (kind == RowKind::Control) {
    switch (index) {
      case kRowMesh:
        return std::string(tr(STR_LORA_MESH_ROW));
      case kRowBeacon:
        return std::string(tr(STR_LORA_BEACON_ROW));
      case kRowName:
        return std::string(tr(STR_LORA_NAME_ROW));
      default:
        return std::string(tr(STR_LORA_ADVERT_ROW));
    }
  }

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
  if (kind == RowKind::RosterOnly) {
    const size_t k = rosterOnly_[static_cast<size_t>(index - rosterBase())];
    const TeamMember m = MapTeam::getInstance().teamMemberAt(k);
    char buf[48];
    snprintf(buf, sizeof(buf), tr(STR_LORA_TEAM_MEMBER_FORMAT), m.acr, m.name[0] != '\0' ? m.name : m.ids[0]);
    return std::string(buf);
  }
#endif

  const size_t i = static_cast<size_t>(index - kControlRowCount);
  const LoraPeer& peer = peers_[i];
  char who[32];
  if (peer.name[0] != '\0') {
    snprintf(who, sizeof(who), "%s", peer.name);
  } else {
    // No name in the advert, so the key is the only thing this node ever told
    // us. Four bytes of it: enough to tell two boards apart on a bench, and it
    // is a prefix on the screen only -- the table matches on the whole key
    // (src/lora/LoraPeerTable.h).
    snprintf(who, sizeof(who), "%02x%02x%02x%02x", peer.pubKey[0], peer.pubKey[1], peer.pubKey[2], peer.pubKey[3]);
  }
#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
  if (peerAcr_[i][0] != '\0') {
    char buf[48];
    snprintf(buf, sizeof(buf), tr(STR_LORA_TEAM_MEMBER_FORMAT), peerAcr_[i], who);
    return std::string(buf);
  }
#endif
  return std::string(who);
}

std::string LoraPeersActivity::rowValue(int index) const {
  const RowKind kind = rowKind(index);
  char buf[32];

  if (kind == RowKind::Control) {
    switch (index) {
      case kRowMesh:
        // "Listening", not "On": it says what the radio is actually doing, and
        // it is the honest word -- the node hears everybody whether or not the
        // beacon below is telling anybody about us. `tr()` takes a bare key
        // name and cannot take an expression, so this goes through I18n.
        return std::string(
            I18n::getInstance().get(meshRunning_ ? StrId::STR_LORA_RADIO_ON : StrId::STR_LORA_RADIO_OFF));
      case kRowBeacon:
        if (beaconSec_ == 0) return std::string(tr(STR_LORA_BEACON_OFF));
        if (beaconSec_ % 60 == 0) {
          snprintf(buf, sizeof(buf), tr(STR_LORA_BEACON_EVERY_MIN_FORMAT), static_cast<unsigned long>(beaconSec_ / 60));
        } else {
          snprintf(buf, sizeof(buf), tr(STR_LORA_BEACON_EVERY_FORMAT), static_cast<unsigned long>(beaconSec_));
        }
        return std::string(buf);
      case kRowName:
        return std::string(nodeName_);
      default:
        return std::string();
    }
  }

  if (kind == RowKind::RosterOnly) return std::string(tr(STR_LORA_TEAM_NOT_HEARD));

  // A peer's coordinate, and an empty column means they have not told us where
  // they are. Never 0,0 -- that is a place in the Atlantic and a different
  // statement entirely.
  const LoraPeer& peer = peers_[static_cast<size_t>(index - kControlRowCount)];
  if (!peer.hasLoc) return std::string();
  snprintf(buf, sizeof(buf), "%.4f %.4f", static_cast<double>(peer.latE6) / 1e6, static_cast<double>(peer.lonE6) / 1e6);
  return std::string(buf);
}

std::string LoraPeersActivity::rowSubtitle(int index) const {
  if (rowKind(index) != RowKind::HeardPeer) return std::string();

  const LoraPeer& peer = peers_[static_cast<size_t>(index - kControlRowCount)];
  char age[16];
  formatAge((snapshotMs_ - peer.heardUptimeMs) / 1000, age, sizeof(age));
  char sub[64];
  snprintf(sub, sizeof(sub), tr(STR_LORA_PEERS_SUB_FORMAT), static_cast<int>(peer.rssiDbm),
           static_cast<int>(peer.snrDb), age, static_cast<unsigned>(peer.adverts));
  return std::string(sub);
}

uint32_t LoraPeersActivity::listSignature() const {
  // FNV-1a over what is drawn, not over what is stored: the question this
  // answers is "would the panel look different", and anything the rider cannot
  // see must not be allowed to cost a refresh.
  uint32_t h = 2166136261u;
  auto mix = [&h](uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      h ^= (v >> (i * 8)) & 0xFF;
      h *= 16777619u;
    }
  };
  mix(static_cast<uint32_t>(peerCount_));
  mix(meshRunning_ ? 1u : 0u);
  mix(beaconSec_);
  mix(rejected_);
  for (size_t i = 0; i < peerCount_; ++i) {
    const LoraPeer& p = peers_[i];
    mix(static_cast<uint32_t>(p.pubKey[0]) | (static_cast<uint32_t>(p.pubKey[1]) << 8) |
        (static_cast<uint32_t>(p.pubKey[2]) << 16) | (static_cast<uint32_t>(p.pubKey[3]) << 24));
    mix(p.adverts);
    mix(static_cast<uint32_t>(p.rssiDbm));
    char age[16];
    formatAge((snapshotMs_ - p.heardUptimeMs) / 1000, age, sizeof(age));
    for (const char* c = age; *c != '\0'; ++c) mix(static_cast<uint32_t>(*c));
#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
    for (const char* c = peerAcr_[i]; *c != '\0'; ++c) mix(static_cast<uint32_t>(*c));
#endif
  }
#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS
  mix(static_cast<uint32_t>(rosterOnlyCount_));
  for (size_t i = 0; i < rosterOnlyCount_; ++i) mix(rosterOnly_[i]);
#endif
  return h;
}

void LoraPeersActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // The subtitle is this node's own state, because every row below it is
  // meaningless without it: an empty list means "nobody is out there" only if
  // the radio was actually listening, and "the mesh is off" otherwise. An
  // error from the last action wins the line, because a control that did
  // nothing has to say so.
  char subtitle[72];
  if (hasError_) {
    snprintf(subtitle, sizeof(subtitle), "%s", I18n::getInstance().get(lastError_));
  } else if (!meshRunning_) {
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

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount(), selectedIndex,
      [this](int index) { return rowTitle(index); }, [this](int index) { return rowSubtitle(index); }, nullptr,
      [this](int index) { return rowValue(index); }, false,
      [this](int index) {
        // "Announce me now" is unreachable with the radio off, and a row that
        // looks pressable and is not is worse than a dimmed one.
        return index == kRowAdvert && !meshRunning_;
      });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // ENABLE_LORA_MESH
