#include "MapGroup.h"

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS

#include <Arduino.h>
#include <BlePositionServer.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "GnssAccess.h"
#include "PinGeo.h"

namespace {
constexpr const char* kLogTag = "GROUP";
}

bool MapGroup::begin() {
  size_t skipped = 0;
  const bool ok = GroupRosterFile::load(roster_, skipped);
  loaded_ = ok;
  if (!ok) {
    LOG_ERR(kLogTag, "no roster read -- every incoming position will be refused");
    return false;
  }
  if (skipped > 0) LOG_ERR(kLogTag, "%u roster row(s) refused by the rules", static_cast<unsigned>(skipped));

  // Rotate before the replay, so a day that is past keeping is not read back
  // into RAM on its way to being deleted.
  GroupBlackBox::rotate(utcNowOrZero(), SETTINGS.mapGroupKeepDays);
  GroupBlackBox::replayLast(roster_, store_, bootId());
  return true;
}

uint32_t MapGroup::bootId() {
  // Once per boot, and never zero: zero is what a row written before this column
  // existed carries, and it has to keep meaning "not this run".
  //
  // Built from the microsecond counter at first use rather than from a hardware
  // RNG, so the simulator and the device take the same path -- and first use is
  // a human opening the map, which lands anywhere inside a wide window. A
  // collision would date a previous run's row as if it were this run's, so the
  // low bits of micros() are mixed up into the high ones instead of being used
  // raw: two boots that entered the map within a millisecond of each other would
  // otherwise share an id.
  static uint32_t id = 0;
  if (id == 0) {
    const uint32_t seed = static_cast<uint32_t>(micros());
    id = seed * 2654435761u;  // Knuth's multiplicative hash: spreads the low bits
    if (id == 0) id = 1;
  }
  return id;
}

uint32_t MapGroup::utcNowOrZero() {
  // **Both radios carry a clock, so ask both.** The phone's position packet
  // anchors one (BlePositionServer::utcNow) and every GNSS receiver sends UTC
  // with its date sentence, well before it has a position fix (Gnss.h). The map
  // header already reads them this way; a position's time must come from the
  // same place, or a row on the card would be undated on exactly the device that
  // did know the time.
  uint32_t utc = 0;
  if (freeink::BlePositionServer::getInstance().utcNow(utc)) return utc;
#ifdef ENABLE_GNSS_CMD
  if (gnss.running()) {
    const uint32_t gnssUtc = gnss.fix().utc;
    if (gnssUtc != 0) return gnssUtc;
  }
#endif
  return 0;
}

size_t MapGroup::slotForIndex(size_t index) const {
  size_t seen = 0;
  for (size_t slot = 0; slot < GroupRoster::kSlotCount; ++slot) {
    if (!roster_.at(slot).present) continue;
    if (seen == index) return slot;
    ++seen;
  }
  return GroupRoster::kSlotCount;
}

IMapGroupSource::Ingest MapGroup::groupPosition(std::string_view idOrAcr, const GroupFix& fix) {
  const size_t slot = roster_.find(idOrAcr);
  if (slot >= GroupRoster::kSlotCount) {
    // The allowlist doing its job. Counted and said out loud, because on a radio
    // this is a device from another group -- or somebody probing ours -- and a
    // silent drop would make that invisible (../../../docs/group-markers.md).
    ++strangersRefused_;
    LOG_INF(kLogTag, "refused a position from '%.*s': not in the roster", static_cast<int>(idOrAcr.size()),
            idOrAcr.data());
    return Ingest::UnknownMember;
  }
  const GroupMember& member = roster_.at(slot);
  if (!member.enabled) return Ingest::Muted;

  GroupFix stored = fix;
  stored.present = true;
  stored.fromLog = false;
  stored.recvUptimeMs = millis();
  // **When we heard it, by our own clock.** The sender's `utc` is theirs and is
  // often absent; this one is ours, it is what the black box is read by, and it
  // is what dates the position after a reboot or a re-entry into the map.
  stored.recvUtc = utcNowOrZero();

  // The card first, always -- see the class comment. Skipped entirely when the
  // rider has turned peer logging off, and the marker still moves: what is on
  // the panel and what is kept on the card are two decisions, and only the
  // second one is a privacy question.
  if (SETTINGS.mapGroupLogPeers != 0) {
    GroupRecord rec;
    rec.utc = stored.utc;
    rec.recvUtc = stored.recvUtc;
    rec.uptimeMs = stored.recvUptimeMs;
    rec.boot = bootId();
    memcpy(rec.who, member.acr, sizeof(rec.who));
    // The identifier this position actually arrived under, not the member's
    // first one: a rider heard over two radios has two, and the row should say
    // which one spoke. Falls back to the member's own when the caller addressed
    // them by acronym, which only the console does.
    const bool addressedById = roster_.findId(idOrAcr) < GroupRoster::kSlotCount;
    if (addressedById) {
      snprintf(rec.id, sizeof(rec.id), "%.*s", static_cast<int>(idOrAcr.size()), idOrAcr.data());
    } else if (member.idCount > 0) {
      snprintf(rec.id, sizeof(rec.id), "%s", member.ids[0]);
    }
    rec.latE7 = stored.latE7;
    rec.lonE7 = stored.lonE7;
    rec.heading = stored.heading;
    rec.hasHeading = stored.hasHeading;
    rec.speedKmh = stored.speedKmh;
    rec.hasSpeed = stored.hasSpeed;
    rec.source = stored.source;
    if (!GroupBlackBox::append(rec)) return Ingest::WriteFailed;
  }

  store_.set(slot, stored);
  LOG_DBG(kLogTag, "%s at %ld,%ld (%s)", member.acr, static_cast<long>(stored.latE7), static_cast<long>(stored.lonE7),
          groupSourceText(stored.source));
  return Ingest::Accepted;
}

IMapGroupSource::Edit MapGroup::groupAdd(std::string_view acr, std::string_view id, std::string_view name) {
  if (!loaded_) {
    // Saving now would write a roster built from nothing over the one on the
    // card, which is how a rider loses eleven members to one add.
    LOG_ERR(kLogTag, "roster not loaded -- refusing to save");
    return Edit::WriteFailed;
  }
  if (roster_.find(id) >= GroupRoster::kSlotCount && roster_.findAcr(acr) >= GroupRoster::kSlotCount &&
      roster_.count() >= GroupRoster::kSlotCount) {
    return Edit::Full;
  }

  GroupRoster candidate = roster_;
  if (!candidate.set(id, acr, name, true)) return Edit::Invalid;
  if (!GroupRosterFile::save(candidate)) return Edit::WriteFailed;
  roster_ = candidate;
  LOG_INF(kLogTag, "added %.*s (%.*s)", static_cast<int>(acr.size()), acr.data(), static_cast<int>(id.size()),
          id.data());
  return Edit::Ok;
}

IMapGroupSource::Edit MapGroup::groupRemove(std::string_view idOrAcr) {
  if (!loaded_) return Edit::WriteFailed;
  const size_t slot = roster_.find(idOrAcr);
  if (slot >= GroupRoster::kSlotCount) return Edit::UnknownMember;

  GroupRoster candidate = roster_;
  if (!candidate.remove(idOrAcr)) return Edit::UnknownMember;
  if (!GroupRosterFile::save(candidate)) return Edit::WriteFailed;
  roster_ = candidate;
  // The marker goes with the member. Their rows stay in the black box -- it is
  // evidence and is never rewritten -- they are simply no longer drawn.
  store_.clearSlot(slot);
  return Edit::Ok;
}

bool MapGroup::groupReload(size_t& skipped) {
  GroupRoster candidate;
  if (!GroupRosterFile::load(candidate, skipped)) return false;
  roster_ = candidate;
  // Positions are keyed by slot and a reload can renumber the slots, so the
  // store is rebuilt from the card rather than reindexed: a marker under the
  // wrong acronym is worse than a marker that takes a moment to come back.
  GroupBlackBox::replayLast(roster_, store_, bootId());
  loaded_ = true;
  return true;
}

size_t MapGroup::groupCount() const { return roster_.count(); }

GroupMember MapGroup::groupMemberAt(size_t index) const {
  const size_t slot = slotForIndex(index);
  return slot < GroupRoster::kSlotCount ? roster_.at(slot) : GroupMember{};
}

GroupFix MapGroup::groupFixAt(size_t index) const {
  const size_t slot = slotForIndex(index);
  return slot < GroupRoster::kSlotCount ? store_.at(slot) : GroupFix{};
}

uint32_t MapGroup::groupLogPage(uint32_t offset, uint32_t maxCount, IGroupLogVisitor& visitor) {
  return GroupBlackBox::page(offset, maxCount, visitor);
}

void MapGroup::logSelf(int32_t latE7, int32_t lonE7, uint32_t utc, uint8_t heading, bool hasHeading, uint16_t speedKmh,
                       bool hasSpeed, GroupFixSource source, uint32_t nowUptimeMs) {
  if (SETTINGS.mapGroupLogSelf == 0) return;

  if (selfLogged_) {
    const bool soon = nowUptimeMs >= selfLastUptimeMs_ && nowUptimeMs - selfLastUptimeMs_ < kSelfLogMinIntervalMs;
    const uint32_t moved = PinGeo::distanceM(selfLastLatE7_, selfLastLonE7_, latE7, lonE7);
    // Either one is enough to earn a row: a rider standing still for an hour
    // still gets one every interval (which is what says they were there), and a
    // rider moving fast gets one every kSelfLogMinMetres rather than waiting out
    // the clock.
    if (soon && moved < kSelfLogMinMetres) return;
  }

  GroupRecord rec;
  rec.utc = utc;
  rec.recvUtc = utc;  // our own position: the two times are the same reading
  rec.uptimeMs = nowUptimeMs;
  rec.boot = bootId();
  snprintf(rec.who, sizeof(rec.who), "%s", kGroupSelfWho);
  rec.latE7 = latE7;
  rec.lonE7 = lonE7;
  rec.heading = heading;
  rec.hasHeading = hasHeading;
  rec.speedKmh = speedKmh;
  rec.hasSpeed = hasSpeed;
  rec.source = source;
  if (!GroupBlackBox::append(rec)) return;

  selfLogged_ = true;
  selfLastUptimeMs_ = nowUptimeMs;
  selfLastLatE7_ = latE7;
  selfLastLonE7_ = lonE7;
}

#endif  // ENABLE_GROUP_MARKERS
