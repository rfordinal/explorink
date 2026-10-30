#include "GroupActivity.h"

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

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
#include "activities/map/MapGroup.h"
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
// the same quantisation the map applies to group marker ages and for the same
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

void GroupActivity::onEnter() {
  Activity::onEnter();

  // The other screen the radio may run under. Without this the list would be
  // the one place you cannot watch the radio work, which is the opposite of
  // what it is for.
  loraPresenceHoldRadio(true);

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
  // Necessary here, not only on the map: this screen can be the first thing to
  // touch the roster in a session that never opened the map.
  MapGroup::getInstance().begin();
#endif

  selectedIndex = controlBase();
  anchor_ = Anchor{};
  lastCheckMs_ = millis();
  refresh();
  drawnSignature_ = listSignature();
  requestUpdate();
}

void GroupActivity::onExit() {
  loraPresenceHoldRadio(false);
  Activity::onExit();
}

bool GroupActivity::hasRosterSection() const {
#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
  return rosterOnlyCount_ > 0;
#else
  return false;
#endif
}

int GroupActivity::itemCount() const {
  int n = rosterBase();
#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
  n += static_cast<int>(rosterOnlyCount_);
#endif
  return n;
}

GroupActivity::RowKind GroupActivity::rowKind(int index) const {
  if (index == kSettingsHeaderIndex) return RowKind::Header;
  if (index < heardHeaderIndex()) return RowKind::Control;
  if (index == heardHeaderIndex()) return RowKind::Header;
  if (hasHeardPlaceholder() && index == peerBase()) return RowKind::Placeholder;
  if (index < rosterHeaderIndex()) return RowKind::HeardPeer;
  if (hasRosterSection() && index == rosterHeaderIndex()) return RowKind::Header;
  return RowKind::RosterOnly;
}

int GroupActivity::stepIndex(int from, int step) const {
  const int count = itemCount();
  int i = from;
  for (int guard = 0; guard < count; ++guard) {
    i = step > 0 ? ButtonNavigator::nextIndex(i, count) : ButtonNavigator::previousIndex(i, count);
    const RowKind k = rowKind(i);
    if (k != RowKind::Header && k != RowKind::Placeholder) return i;
  }
  return from;
}

void GroupActivity::refresh() {
  const LoraPresenceStatus status = loraPresenceStatus();
  meshRunning_ = status.meshRunning;
  beaconSec_ = status.beaconSec;
  rejected_ = status.advertsRejected;
  identityPersisted_ = status.identityPersisted;
  snapshotMs_ = status.nowUptimeMs;
  memcpy(nodeName_, status.nodeName, sizeof(nodeName_));
  nodeName_[sizeof(nodeName_) - 1] = '\0';

  peerCount_ = loraPresenceSnapshot(peers_, LoraPeerTable::kMaxPeers);

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
  // Which heard nodes are already members, and which members nobody is
  // hearing. Both answers are computed once here rather than per drawn row:
  // the roster walk is twelve entries and the draw runs on every repaint.
  MapGroup& group = MapGroup::getInstance();
  bool memberHeard[kGroupMaxMembers] = {};
  const size_t members = group.groupCount();

  for (size_t i = 0; i < peerCount_; ++i) {
    peerAcr_[i][0] = '\0';
    char id[kLoraGroupIdBytes];
    loraGroupId(peers_[i].pubKey, id, sizeof(id));
    const size_t slot = group.roster().findId(id);
    if (slot >= GroupRoster::kSlotCount) continue;
    snprintf(peerAcr_[i], sizeof(peerAcr_[i]), "%s", group.roster().at(slot).acr);
    for (size_t k = 0; k < members && k < kGroupMaxMembers; ++k) {
      if (group.slotForIndex(k) == slot) memberHeard[k] = true;
    }
  }

  rosterOnlyCount_ = 0;
  for (size_t k = 0; k < members && k < kGroupMaxMembers; ++k) {
    if (memberHeard[k]) continue;
    rosterOnly_[rosterOnlyCount_++] = static_cast<uint8_t>(k);
  }
#endif

  restoreAnchor();
}

void GroupActivity::captureAnchor() {
  anchor_ = Anchor{};
  anchor_.kind = rowKind(selectedIndex);
  switch (anchor_.kind) {
    case RowKind::Placeholder:
    case RowKind::Header:
      // The cursor never rests on one, but refresh() can run while a
      // sub-activity owns the screen. Anchoring to the first control is the
      // same harmless place restoreAnchor() falls back to.
      anchor_.kind = RowKind::Control;
      anchor_.controlRow = 0;
      break;
    case RowKind::Control:
      anchor_.controlRow = controlRowOf(selectedIndex);
      break;
    case RowKind::HeardPeer:
      memcpy(anchor_.key, peers_[static_cast<size_t>(selectedIndex - peerBase())].pubKey, sizeof(anchor_.key));
      break;
    case RowKind::RosterOnly:
#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
      snprintf(
          anchor_.acr, sizeof(anchor_.acr), "%s",
          MapGroup::getInstance().groupMemberAt(rosterOnly_[static_cast<size_t>(selectedIndex - rosterBase())]).acr);
#endif
      break;
  }
}

void GroupActivity::restoreAnchor() {
  switch (anchor_.kind) {
    case RowKind::Placeholder:
    case RowKind::Header:
    case RowKind::Control:
      selectedIndex = controlBase() + anchor_.controlRow;
      return;

    case RowKind::HeardPeer:
      for (size_t i = 0; i < peerCount_; ++i) {
        if (memcmp(peers_[i].pubKey, anchor_.key, sizeof(anchor_.key)) == 0) {
          selectedIndex = peerBase() + static_cast<int>(i);
          return;
        }
      }
      // Adding a node does not move it out of the heard section -- it stays
      // where it is and gains an acronym -- so a miss here means it genuinely
      // stopped being heard, and the fallback below is the honest answer.
      break;

    case RowKind::RosterOnly:
#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
      for (size_t i = 0; i < rosterOnlyCount_; ++i) {
        const GroupMember m = MapGroup::getInstance().groupMemberAt(rosterOnly_[i]);
        if (strncmp(m.acr, anchor_.acr, sizeof(anchor_.acr)) == 0) {
          selectedIndex = rosterBase() + static_cast<int>(i);
          return;
        }
      }
#endif
      break;
  }

  // The anchored item is gone -- removed, or stopped being heard and is not a
  // member either. The cursor goes somewhere harmless rather than to whatever
  // slid into that row number.
  selectedIndex = controlBase();
  anchor_.kind = RowKind::Control;
  anchor_.controlRow = 0;
}

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
void GroupActivity::addHeardPeer(size_t peerIndex) {
  const LoraPeer& peer = peers_[peerIndex];

  char id[kLoraGroupIdBytes];
  loraGroupId(peer.pubKey, id, sizeof(id));
  char guess[kGroupAcrBytes];
  loraGroupAcrGuess(peer.name[0] != '\0' ? peer.name : nullptr, peer.pubKey, guess, sizeof(guess));

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
      if (MapGroup::getInstance().groupAdd(kb.text, idStr, name) != IMapGroupSource::Edit::Ok) {
        // The roster refuses a duplicate acronym, a crossing id pair and a
        // full table, and from here they mean one thing to the rider: pick
        // another short name, or make room.
        lastError_ = StrId::STR_LORA_GROUP_ADD_FAILED;
        hasError_ = true;
      }
    }
    refresh();
    requestUpdate();
  };

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LORA_GROUP_ADD), std::string(guess),
                                              kGroupAcrBytes - 1, InputType::Text),
      handler);
}

void GroupActivity::removeMember(const std::string& idOrAcr, const std::string& label) {
  // **Confirmed, because this is the destructive one.** Removing a member
  // drops the allowlist entry their positions arrive through, so the next
  // thing they send is refused as a stranger. That is the point of the row,
  // and it is not something a mis-aimed Select should do.
  auto handler = [this, idOrAcr](const ActivityResult& result) {
    if (!result.isCancelled) {
      if (MapGroup::getInstance().groupRemove(idOrAcr) != IMapGroupSource::Edit::Ok) {
        lastError_ = StrId::STR_LORA_GROUP_REMOVE_FAILED;
        hasError_ = true;
      }
    }
    refresh();
    requestUpdate();
  };
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_LORA_GROUP_REMOVE), label), handler);
}
#endif  // ENABLE_GROUP_MARKERS

void GroupActivity::activate(int index) {
  hasError_ = false;

  const RowKind kind = rowKind(index);
  if (kind == RowKind::Header || kind == RowKind::Placeholder) return;
#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
  if (kind == RowKind::HeardPeer) {
    const size_t i = static_cast<size_t>(index - peerBase());
    if (peerAcr_[i][0] == '\0') {
      addHeardPeer(i);
    } else {
      removeMember(std::string(peerAcr_[i]), rowTitle(index));
    }
    return;  // the sub-activity's handler refreshes
  }
  if (kind == RowKind::RosterOnly) {
    const size_t k = rosterOnly_[static_cast<size_t>(index - rosterBase())];
    const GroupMember m = MapGroup::getInstance().groupMemberAt(k);
    removeMember(std::string(m.acr), rowTitle(index));
    return;
  }
#else
  // Without the group layer a peer row has nothing behind it.
  if (kind != RowKind::Control) return;
#endif

  switch (controlRowOf(index)) {
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

void GroupActivity::loop() {
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

  buttonNavigator.onNext([this] {
    selectedIndex = stepIndex(selectedIndex, 1);
    captureAnchor();
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = stepIndex(selectedIndex, -1);
    captureAnchor();
    requestUpdate();
  });
}

std::string GroupActivity::rowTitle(int index) const {
  const RowKind kind = rowKind(index);

  if (kind == RowKind::Header) {
    if (index == kSettingsHeaderIndex) return std::string(tr(STR_GROUP_SECTION_SETTINGS));
    if (index == heardHeaderIndex()) return std::string(tr(STR_GROUP_SECTION_HEARD));
    return std::string(tr(STR_GROUP_SECTION_OFFLINE));
  }

  if (kind == RowKind::Placeholder) return std::string(tr(STR_LORA_PEERS_NONE));

  if (kind == RowKind::Control) {
    switch (controlRowOf(index)) {
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

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
  if (kind == RowKind::RosterOnly) {
    const size_t k = rosterOnly_[static_cast<size_t>(index - rosterBase())];
    const GroupMember m = MapGroup::getInstance().groupMemberAt(k);
    char buf[48];
    snprintf(buf, sizeof(buf), tr(STR_LORA_GROUP_MEMBER_FORMAT), m.acr, m.name[0] != '\0' ? m.name : m.ids[0]);
    return std::string(buf);
  }
#endif

  const size_t i = static_cast<size_t>(index - peerBase());
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
#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
  if (peerAcr_[i][0] != '\0') {
    char buf[48];
    snprintf(buf, sizeof(buf), tr(STR_LORA_GROUP_MEMBER_FORMAT), peerAcr_[i], who);
    return std::string(buf);
  }
#endif
  return std::string(who);
}

std::string GroupActivity::rowValue(int index) const {
  const RowKind kind = rowKind(index);
  char buf[32];

  if (kind == RowKind::Header || kind == RowKind::Placeholder) return std::string();

  if (kind == RowKind::Control) {
    switch (controlRowOf(index)) {
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

  // No value: the section heading above these rows already says "Not on
  // LoRa", and repeating it once per row was the whole reason the screen
  // needed headings.
  if (kind == RowKind::RosterOnly) return std::string();

  // A peer's coordinate, and an empty column means they have not told us where
  // they are. Never 0,0 -- that is a place in the Atlantic and a different
  // statement entirely.
  const LoraPeer& peer = peers_[static_cast<size_t>(index - peerBase())];
  if (!peer.hasLoc) return std::string();
  snprintf(buf, sizeof(buf), "%.4f %.4f", static_cast<double>(peer.latE6) / 1e6, static_cast<double>(peer.lonE6) / 1e6);
  return std::string(buf);
}

std::string GroupActivity::rowSubtitle(int index) const {
  if (rowKind(index) != RowKind::HeardPeer) return std::string();

  const LoraPeer& peer = peers_[static_cast<size_t>(index - peerBase())];
  char age[16];
  formatAge((snapshotMs_ - peer.heardUptimeMs) / 1000, age, sizeof(age));
  char sub[64];
  snprintf(sub, sizeof(sub), tr(STR_LORA_PEERS_SUB_FORMAT), static_cast<int>(peer.rssiDbm),
           static_cast<int>(peer.snrDb), age, static_cast<unsigned>(peer.adverts));
  return std::string(sub);
}

uint32_t GroupActivity::listSignature() const {
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
#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
    for (const char* c = peerAcr_[i]; *c != '\0'; ++c) mix(static_cast<uint32_t>(*c));
#endif
  }
#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS
  mix(static_cast<uint32_t>(rosterOnlyCount_));
  for (size_t i = 0; i < rosterOnlyCount_; ++i) mix(rosterOnly_[i]);
#endif
  return h;
}

void GroupActivity::render(RenderLock&&) {
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GROUP_TITLE), subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount(), selectedIndex,
      [this](int index) { return rowTitle(index); }, [this](int index) { return rowSubtitle(index); }, nullptr,
      [this](int index) { return rowValue(index); }, false,
      [this](int index) {
        // "Announce me now" is unreachable with the radio off, and a row that
        // looks pressable and is not is worse than a dimmed one.
        const RowKind k = rowKind(index);
        if (k == RowKind::Header || k == RowKind::Placeholder) return true;
        return k == RowKind::Control && controlRowOf(index) == kRowAdvert && !meshRunning_;
      });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // ENABLE_LORA_MESH
