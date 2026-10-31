#include "OtaVersionCompare.h"

#include <cstdio>
#include <cstring>

namespace ota_version {

bool parseVersion(const char* tagName, int* major, int* minor, int* patch) {
  if (!tagName || !major || !minor || !patch) return false;

  const char* versionStart = nullptr;
  if (tagName[0] >= '0' && tagName[0] <= '9') {
    versionStart = tagName;
  } else if (tagName[0] == 'v' && tagName[1] >= '0' && tagName[1] <= '9') {
    // A bare "v0.2.1-alpha" tag, no "explorink-" prefix (seen on the
    // explorink-android repo's own releases, e.g. "v0.2.0-alpha").
    versionStart = tagName + 1;
  } else {
    // Look for the "-v<digit>" marker that precedes the version in every tag
    // shape this project publishes, regardless of what device prefix (if any)
    // comes before it.
    for (const char* p = tagName; p[0] != '\0' && p[1] != '\0' && p[2] != '\0'; ++p) {
      if (p[0] == '-' && p[1] == 'v' && p[2] >= '0' && p[2] <= '9') {
        versionStart = p + 2;
        break;
      }
    }
  }
  if (!versionStart) return false;

  int m = 0, n = 0, p = 0;
  if (sscanf(versionStart, "%d.%d.%d", &m, &n, &p) != 3) return false;

  *major = m;
  *minor = n;
  *patch = p;
  return true;
}

bool isVersionNewer(const char* currentVersion, const char* latestTag) {
  int curMajor, curMinor, curPatch;
  int latMajor, latMinor, latPatch;
  if (!parseVersion(currentVersion, &curMajor, &curMinor, &curPatch)) return false;
  if (!parseVersion(latestTag, &latMajor, &latMinor, &latPatch)) return false;

  if (latMajor != curMajor) return latMajor > curMajor;
  if (latMinor != curMinor) return latMinor > curMinor;
  if (latPatch != curPatch) return latPatch > curPatch;

  // Equal numeric version: a running RC build is superseded by any release
  // carrying that same version number (the RC's own final cut included).
  if (strstr(currentVersion, "-rc") != nullptr) return true;

  return false;
}

}  // namespace ota_version
