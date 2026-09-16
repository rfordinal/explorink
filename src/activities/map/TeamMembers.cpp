#include "TeamMembers.h"

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

namespace {

bool isPrintableAscii(char c) {
  const auto u = static_cast<unsigned char>(c);
  return u >= 0x20 && u <= 0x7E;
}

// The bytes that would break something downstream: the black box is CSV, the
// console's replies are `key=value` lines, and the roster is JSON. A byte that
// ends a field in any of the three is refused at the door rather than escaped in
// three places.
bool isSeparator(char c) {
  return c == ',' || c == '|' || c == '"' || c == '\\' || c == '=' || c == '\n' || c == '\r' || c == '\t';
}

char toUpper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

}  // namespace

bool isValidTeamAcr(std::string_view acr) {
  if (acr.size() < 2 || acr.size() > kTeamAcrBytes - 1) return false;
  for (const char c : acr) {
    const char u = toUpper(c);
    const bool letter = u >= 'A' && u <= 'Z';
    const bool digit = u >= '0' && u <= '9';
    if (!letter && !digit) return false;
  }
  return true;
}

bool teamNormaliseAcr(std::string_view acr, char* out) {
  if (out == nullptr || !isValidTeamAcr(acr)) return false;
  size_t i = 0;
  for (; i < acr.size(); ++i) out[i] = toUpper(acr[i]);
  out[i] = '\0';
  return true;
}

bool isValidTeamId(std::string_view id) {
  if (id.empty() || id.size() > kTeamIdBytes - 1) return false;
  for (const char c : id) {
    if (!isPrintableAscii(c) || c == ' ' || isSeparator(c)) return false;
  }
  return true;
}

bool isValidTeamName(std::string_view name) {
  if (name.size() > kTeamNameBytes - 1) return false;
  for (const char c : name) {
    if (!isPrintableAscii(c) || isSeparator(c)) return false;
  }
  return true;
}
#endif  // ENABLE_TEAM_MARKERS
