#include "TeamRoster.h"

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <StreamingJsonParser.h>

#include "TeamRecord.h"

#include <cstdio>
#include <cstring>

namespace {

// False when the value does not fit. A truncated field is not a smaller version
// of itself: "TOOLONG" cut to "TOO" is a valid acronym that nobody wrote, and an
// id cut short addresses a different radio.
bool copyField(char* dst, size_t dstBytes, std::string_view src) {
  if (src.size() >= dstBytes) return false;
  memcpy(dst, src.data(), src.size());
  dst[src.size()] = '\0';
  return true;
}

// One member being assembled out of the document, plus everything the callbacks
// need to know where they are. A struct rather than captured state because
// StreamingJsonParser takes free functions and a void* context.
struct ParseCtx {
  TeamRoster* roster = nullptr;
  size_t skipped = 0;
  bool sawVersion = false;
  bool versionOk = false;
  bool inMembers = false;
  bool inMember = false;
  // Which array we are inside. `ids` nests inside a member, so the end of *that*
  // array must not be read as the end of the members list.
  int arrayDepth = 0;
  int membersDepth = -1;
  char key[16] = {};
  char ids[kTeamIdsPerMember][kTeamIdBytes] = {};
  size_t idCount = 0;
  char acr[kTeamAcrBytes] = {};
  char name[kTeamNameBytes] = {};
  bool enabled = true;
  // Cleared by a field that did not fit, so the row is counted as skipped rather
  // than stored as a truncated stranger.
  bool rowOk = true;
};

void onKey(void* ctx, const char* key, size_t len) {
  auto* c = static_cast<ParseCtx*>(ctx);
  // A key longer than any this build knows cannot match one either way, so it is
  // emptied rather than truncated into a false match.
  if (!copyField(c->key, sizeof(c->key), std::string_view(key, len))) c->key[0] = '\0';
}

void onString(void* ctx, const char* value, size_t len) {
  auto* c = static_cast<ParseCtx*>(ctx);
  if (!c->inMember) return;
  const std::string_view v(value, len);
  // `id` is the single-identifier form older files use; `ids` is the array. Both
  // land in the same list, because one radio is the common case and several is
  // the one that matters.
  if (strcmp(c->key, "id") == 0 || strcmp(c->key, "ids") == 0) {
    if (c->idCount >= kTeamIdsPerMember) {
      c->rowOk = false;
    } else {
      c->rowOk &= copyField(c->ids[c->idCount], kTeamIdBytes, v);
      ++c->idCount;
    }
  } else if (strcmp(c->key, "acr") == 0) {
    c->rowOk &= copyField(c->acr, sizeof(c->acr), v);
  } else if (strcmp(c->key, "name") == 0) {
    c->rowOk &= copyField(c->name, sizeof(c->name), v);
  }
}

void onNumber(void* ctx, const char* value, size_t len) {
  auto* c = static_cast<ParseCtx*>(ctx);
  if (c->inMember || strcmp(c->key, "v") != 0) return;
  c->sawVersion = true;
  c->versionOk = (len == 1 && value[0] == '0' + TeamRoster::kVersion);
}

void onBool(void* ctx, bool value) {
  auto* c = static_cast<ParseCtx*>(ctx);
  if (c->inMember && strcmp(c->key, "on") == 0) c->enabled = value;
}

void onNull(void*) {}

void onObjectStart(void* ctx) {
  auto* c = static_cast<ParseCtx*>(ctx);
  if (!c->inMembers || c->inMember) return;
  c->inMember = true;
  c->idCount = 0;
  c->acr[0] = '\0';
  c->name[0] = '\0';
  c->enabled = true;
  c->rowOk = true;
  c->key[0] = '\0';
}

void onObjectEnd(void* ctx) {
  auto* c = static_cast<ParseCtx*>(ctx);
  if (!c->inMember) return;
  c->inMember = false;
  // A row the rules refuse is counted, never silently dropped: a member who is
  // missing from the allowlist is a rider whose marker never appears, and the
  // count is what tells the console the file needs fixing.
  if (!c->rowOk || c->idCount == 0) {
    ++c->skipped;
    return;
  }
  // Every identifier of the row, one call each: the first creates the member and
  // the rest attach to it.
  bool ok = true;
  for (size_t i = 0; i < c->idCount; ++i) {
    ok &= c->roster->set(c->ids[i], c->acr, c->name, c->enabled);
  }
  if (!ok) ++c->skipped;
}

void onArrayStart(void* ctx) {
  auto* c = static_cast<ParseCtx*>(ctx);
  ++c->arrayDepth;
  if (strcmp(c->key, "members") == 0) {
    c->inMembers = true;
    c->membersDepth = c->arrayDepth;
  }
}

void onArrayEnd(void* ctx) {
  auto* c = static_cast<ParseCtx*>(ctx);
  if (c->arrayDepth == c->membersDepth) {
    c->inMembers = false;
    c->membersDepth = -1;
  }
  --c->arrayDepth;
}

}  // namespace

void TeamRoster::clear() {
  for (auto& slot : slots_) slot = TeamMember{};
}

size_t TeamRoster::count() const {
  size_t n = 0;
  for (const auto& slot : slots_) {
    if (slot.present) ++n;
  }
  return n;
}

size_t TeamRoster::findId(std::string_view id) const {
  if (id.empty()) return kSlotCount;
  // Every identifier the member is known by, not just the first: the same person
  // arrives under a MeshCore key on one radio and under something else entirely
  // when a phone relays them (TeamMembers.h).
  for (size_t i = 0; i < kSlotCount; ++i) {
    if (!slots_[i].present) continue;
    for (size_t n = 0; n < slots_[i].idCount; ++n) {
      if (id == std::string_view(slots_[i].ids[n])) return i;
    }
  }
  return kSlotCount;
}

size_t TeamRoster::findAcr(std::string_view acr) const {
  char norm[kTeamAcrBytes];
  if (!teamNormaliseAcr(acr, norm)) return kSlotCount;
  for (size_t i = 0; i < kSlotCount; ++i) {
    if (slots_[i].present && strcmp(slots_[i].acr, norm) == 0) return i;
  }
  return kSlotCount;
}

size_t TeamRoster::find(std::string_view idOrAcr) const {
  const size_t byId = findId(idOrAcr);
  if (byId < kSlotCount) return byId;
  return findAcr(idOrAcr);
}

size_t TeamRoster::firstFreeSlot() const {
  for (size_t i = 0; i < kSlotCount; ++i) {
    if (!slots_[i].present) return i;
  }
  return kSlotCount;
}

bool TeamRoster::set(std::string_view id, std::string_view acr, std::string_view name, bool enabled) {
  char norm[kTeamAcrBytes];
  if (!isValidTeamId(id) || !teamNormaliseAcr(acr, norm) || !isValidTeamName(name)) return false;
  // `ME` is the rider's own row in the black box (TeamRecord.h). A member under
  // that acronym would put two people on one name in the one file somebody reads
  // when looking for them.
  if (std::string_view(norm) == kTeamSelfWho) return false;

  const size_t byId = findId(id);
  const size_t byAcr = findAcr(norm);
  // **An identifier belongs to one person, and so does an acronym.** A known id
  // arriving under an acronym that belongs to somebody *else* would merge two
  // riders into one marker, and a search reading the card afterwards could not
  // untangle them. A free acronym is a different thing: that is a rename, and a
  // rider relabelling their group between rides is ordinary.
  if (byId < kSlotCount && byAcr < kSlotCount && byId != byAcr) return false;

  size_t slot = byId < kSlotCount ? byId : byAcr;
  if (slot >= kSlotCount) slot = firstFreeSlot();
  if (slot >= kSlotCount) return false;

  TeamMember& member = slots_[slot];
  const bool isNew = !member.present;
  member.present = true;
  member.enabled = enabled;
  memcpy(member.acr, norm, sizeof(norm));
  copyField(member.name, sizeof(member.name), name);

  if (isNew) member.idCount = 0;
  // A known member named by a radio that has not named them before gains that
  // identifier rather than replacing the one they had: the LoRa key and the
  // relayed BLE name are both true at once.
  if (byId >= kSlotCount) {
    if (member.idCount >= kTeamIdsPerMember) return false;
    copyField(member.ids[member.idCount], kTeamIdBytes, id);
    ++member.idCount;
  }
  return true;
}

bool TeamRoster::remove(std::string_view idOrAcr) {
  const size_t slot = find(idOrAcr);
  if (slot >= kSlotCount) return false;
  slots_[slot] = TeamMember{};
  return true;
}

bool TeamRoster::parseJson(std::string_view json, size_t& skipped) {
  skipped = 0;
  clear();

  ParseCtx ctx;
  ctx.roster = this;
  const JsonCallbacks callbacks = {&ctx,          onKey,         onString,     onNumber,   onBool,
                                   onNull,        onObjectStart, onObjectEnd,  onArrayStart, onArrayEnd};
  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());

  // A version this build does not know is refused whole. Guessing at it could
  // drop a member without saying so, and the rider would only find out when
  // somebody's marker never showed up.
  if (parser.hasError() || !ctx.sawVersion || !ctx.versionOk) {
    clear();
    return false;
  }
  skipped = ctx.skipped;
  return true;
}

size_t TeamRoster::writeJson(char* buf, size_t bufLen) const {
  if (buf == nullptr || bufLen == 0) return 0;
  int written = snprintf(buf, bufLen, "{\"v\":%u,\"members\":[", static_cast<unsigned>(kVersion));
  if (written < 0 || static_cast<size_t>(written) >= bufLen) return 0;
  size_t len = static_cast<size_t>(written);

  bool first = true;
  for (const auto& member : slots_) {
    if (!member.present) continue;
    written = snprintf(buf + len, bufLen - len, "%s{\"acr\":\"%s\",\"name\":\"%s\",\"on\":%s,\"ids\":[",
                       first ? "" : ",", member.acr, member.name, member.enabled ? "true" : "false");
    if (written < 0 || static_cast<size_t>(written) >= bufLen - len) return 0;
    len += static_cast<size_t>(written);
    for (size_t i = 0; i < member.idCount; ++i) {
      written = snprintf(buf + len, bufLen - len, "%s\"%s\"", i == 0 ? "" : ",", member.ids[i]);
      if (written < 0 || static_cast<size_t>(written) >= bufLen - len) return 0;
      len += static_cast<size_t>(written);
    }
    written = snprintf(buf + len, bufLen - len, "]}");
    if (written < 0 || static_cast<size_t>(written) >= bufLen - len) return 0;
    len += static_cast<size_t>(written);
    first = false;
  }

  written = snprintf(buf + len, bufLen - len, "]}\n");
  if (written < 0 || static_cast<size_t>(written) >= bufLen - len) return 0;
  return len + static_cast<size_t>(written);
}
#endif  // ENABLE_TEAM_MARKERS
