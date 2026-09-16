#include "MapTeam.h"

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <Arduino.h>
#include <BlePositionServer.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "PinGeo.h"

namespace {
constexpr const char* kLogTag = "TEAM";
}

bool MapTeam::begin() {
  size_t skipped = 0;
  const bool ok = TeamRosterFile::load(roster_, skipped);
  loaded_ = ok;
  if (!ok) {
    LOG_ERR(kLogTag, "no roster read -- every incoming position will be refused");
    return false;
  }
  if (skipped > 0) LOG_ERR(kLogTag, "%u roster row(s) refused by the rules", static_cast<unsigned>(skipped));

  // Rotate before the replay, so a day that is past keeping is not read back
  // into RAM on its way to being deleted.
  TeamBlackBox::rotate(utcNowOrZero(), SETTINGS.mapTeamKeepDays);
  TeamBlackBox::replayLast(roster_, store_);
  return true;
}

uint32_t MapTeam::utcNowOrZero() {
  uint32_t utc = 0;
  if (!freeink::BlePositionServer::getInstance().utcNow(utc)) return 0;
  return utc;
}

size_t MapTeam::slotForIndex(size_t index) const {
  size_t seen = 0;
  for (size_t slot = 0; slot < TeamRoster::kSlotCount; ++slot) {
    if (!roster_.at(slot).present) continue;
    if (seen == index) return slot;
    ++seen;
  }
  return TeamRoster::kSlotCount;
}

IMapTeamSource::Ingest MapTeam::teamPosition(std::string_view idOrAcr, const TeamFix& fix) {
  const size_t slot = roster_.find(idOrAcr);
  if (slot >= TeamRoster::kSlotCount) {
    // The allowlist doing its job. Counted and said out loud, because on a radio
    // this is a device from another group -- or somebody probing ours -- and a
    // silent drop would make that invisible (../../../docs/team-markers.md).
    ++strangersRefused_;
    LOG_INF(kLogTag, "refused a position from '%.*s': not in the roster", static_cast<int>(idOrAcr.size()),
            idOrAcr.data());
    return Ingest::UnknownMember;
  }
  const TeamMember& member = roster_.at(slot);
  if (!member.enabled) return Ingest::Muted;

  TeamFix stored = fix;
  stored.present = true;
  stored.fromLog = false;
  stored.recvUptimeMs = millis();

  // The card first, always -- see the class comment. Skipped entirely when the
  // rider has turned peer logging off, and the marker still moves: what is on
  // the panel and what is kept on the card are two decisions, and only the
  // second one is a privacy question.
  if (SETTINGS.mapTeamLogPeers != 0) {
    TeamRecord rec;
    rec.utc = stored.utc;
    rec.uptimeMs = stored.recvUptimeMs;
    memcpy(rec.who, member.acr, sizeof(rec.who));
    rec.latE7 = stored.latE7;
    rec.lonE7 = stored.lonE7;
    rec.heading = stored.heading;
    rec.hasHeading = stored.hasHeading;
    rec.speedKmh = stored.speedKmh;
    rec.hasSpeed = stored.hasSpeed;
    rec.source = stored.source;
    if (!TeamBlackBox::append(rec)) return Ingest::WriteFailed;
  }

  store_.set(slot, stored);
  LOG_DBG(kLogTag, "%s at %ld,%ld (%s)", member.acr, static_cast<long>(stored.latE7), static_cast<long>(stored.lonE7),
          teamSourceText(stored.source));
  return Ingest::Accepted;
}

IMapTeamSource::Edit MapTeam::teamAdd(std::string_view acr, std::string_view id, std::string_view name) {
  if (!loaded_) {
    // Saving now would write a roster built from nothing over the one on the
    // card, which is how a rider loses eleven members to one add.
    LOG_ERR(kLogTag, "roster not loaded -- refusing to save");
    return Edit::WriteFailed;
  }
  if (roster_.find(id) >= TeamRoster::kSlotCount && roster_.findAcr(acr) >= TeamRoster::kSlotCount &&
      roster_.count() >= TeamRoster::kSlotCount) {
    return Edit::Full;
  }

  TeamRoster candidate = roster_;
  if (!candidate.set(id, acr, name, true)) return Edit::Invalid;
  if (!TeamRosterFile::save(candidate)) return Edit::WriteFailed;
  roster_ = candidate;
  LOG_INF(kLogTag, "added %.*s (%.*s)", static_cast<int>(acr.size()), acr.data(), static_cast<int>(id.size()),
          id.data());
  return Edit::Ok;
}

IMapTeamSource::Edit MapTeam::teamRemove(std::string_view idOrAcr) {
  if (!loaded_) return Edit::WriteFailed;
  const size_t slot = roster_.find(idOrAcr);
  if (slot >= TeamRoster::kSlotCount) return Edit::UnknownMember;

  TeamRoster candidate = roster_;
  if (!candidate.remove(idOrAcr)) return Edit::UnknownMember;
  if (!TeamRosterFile::save(candidate)) return Edit::WriteFailed;
  roster_ = candidate;
  // The marker goes with the member. Their rows stay in the black box -- it is
  // evidence and is never rewritten -- they are simply no longer drawn.
  store_.clearSlot(slot);
  return Edit::Ok;
}

bool MapTeam::teamReload(size_t& skipped) {
  TeamRoster candidate;
  if (!TeamRosterFile::load(candidate, skipped)) return false;
  roster_ = candidate;
  // Positions are keyed by slot and a reload can renumber the slots, so the
  // store is rebuilt from the card rather than reindexed: a marker under the
  // wrong acronym is worse than a marker that takes a moment to come back.
  TeamBlackBox::replayLast(roster_, store_);
  loaded_ = true;
  return true;
}

size_t MapTeam::teamCount() const { return roster_.count(); }

TeamMember MapTeam::teamMemberAt(size_t index) const {
  const size_t slot = slotForIndex(index);
  return slot < TeamRoster::kSlotCount ? roster_.at(slot) : TeamMember{};
}

TeamFix MapTeam::teamFixAt(size_t index) const {
  const size_t slot = slotForIndex(index);
  return slot < TeamRoster::kSlotCount ? store_.at(slot) : TeamFix{};
}

uint32_t MapTeam::teamLogPage(uint32_t offset, uint32_t maxCount, ITeamLogVisitor& visitor) {
  return TeamBlackBox::page(offset, maxCount, visitor);
}

void MapTeam::logSelf(int32_t latE7, int32_t lonE7, uint32_t utc, uint8_t heading, bool hasHeading, uint16_t speedKmh,
                      bool hasSpeed, TeamFixSource source, uint32_t nowUptimeMs) {
  if (SETTINGS.mapTeamLogSelf == 0) return;

  if (selfLogged_) {
    const bool soon = nowUptimeMs >= selfLastUptimeMs_ && nowUptimeMs - selfLastUptimeMs_ < kSelfLogMinIntervalMs;
    const uint32_t moved = PinGeo::distanceM(selfLastLatE7_, selfLastLonE7_, latE7, lonE7);
    // Either one is enough to earn a row: a rider standing still for an hour
    // still gets one every interval (which is what says they were there), and a
    // rider moving fast gets one every kSelfLogMinMetres rather than waiting out
    // the clock.
    if (soon && moved < kSelfLogMinMetres) return;
  }

  TeamRecord rec;
  rec.utc = utc;
  rec.uptimeMs = nowUptimeMs;
  snprintf(rec.who, sizeof(rec.who), "%s", kTeamSelfWho);
  rec.latE7 = latE7;
  rec.lonE7 = lonE7;
  rec.heading = heading;
  rec.hasHeading = hasHeading;
  rec.speedKmh = speedKmh;
  rec.hasSpeed = hasSpeed;
  rec.source = source;
  if (!TeamBlackBox::append(rec)) return;

  selfLogged_ = true;
  selfLastUptimeMs_ = nowUptimeMs;
  selfLastLatE7_ = latE7;
  selfLastLonE7_ = lonE7;
}

#endif  // ENABLE_TEAM_MARKERS
