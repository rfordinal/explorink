#pragma once

// Pure semver parsing/compare logic pulled out of OtaUpdater so it is
// reachable from a host test without dragging in esp_ota_ops.h / esp_wifi.h
// (T-2010 -- this logic previously had zero coverage, and shipped with an
// uninitialized-int read: sscanf("%d.%d.%d", ...) on a GitHub release
// tag_name like "explorink-v0.2.1-alpha" never matches, because the string
// doesn't start with a digit, and the old code never checked sscanf's return
// value).

namespace ota_version {

// Finds a semantic version inside `tagName` and parses it into major/minor/patch.
// Accepts two shapes, both seen in this project's own release tags
// (docs/public-release.md): a bare version ("0.2.1-alpha", what
// OtaUpdater::installUpdate's own TRAILINK_VERSION looks like), or a GitHub
// tag_name carrying a prefix before a "-v<digit>" marker
// ("explorink-v0.2.1-alpha", "explorink-x4pro-v0.2.0-alpha",
// "explorink-t5s3pro-v0.2.0-alpha"). Returns false (leaving the outputs
// untouched) if no such marker is found or the three numeric components
// don't all parse.
bool parseVersion(const char* tagName, int* major, int* minor, int* patch);

// True if `latestTag` names a strictly newer version than `currentVersion`.
// Equal numeric versions are "newer" only when `currentVersion` is itself an
// RC build ("-rc" substring) -- an RC is always superseded by the final cut
// of the same version number. Returns false if either string fails to parse.
bool isVersionNewer(const char* currentVersion, const char* latestTag);

}  // namespace ota_version
