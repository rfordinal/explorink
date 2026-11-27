#pragma once

#include <cstddef>
#include <cstring>

// Is this relative path one spelling of one file, and does it stay inside the
// root?
//
// **Pulled out of MapTransferReceiver so it can be host-tested**, after it had
// held a hole twice: this is the function that decides what a path *means*
// before any gate compares it, and until 2026-11-27 nothing exercised it at
// all. Every aliasing spelling the wallet gates were later patched for --
// `.DAT` against a case-folding filesystem, FAT's `~1` short name, a leading
// space SdFat silently strips -- is really a question about this function,
// because a gate that compares strings can only be as sound as the guarantee
// that one file has one spelling.
//
// Pure: no Storage, no globals, nothing from Arduino. The test for it lives in
// test/wallet, because the wallet is what an alias gets you.

namespace maptransfer {

// The longest relative path a begin frame may carry. It lives here with the
// check that enforces it, and MapTransferReceiver asserts that its own path
// buffers are big enough for it -- a root longer than `/explorink` would
// otherwise truncate the very string the gates inspect, silently.
inline constexpr size_t kMaxRelPathBytes = 80;

inline bool relPathIsSafe(const char* path, size_t len) {
  if (len == 0 || len > kMaxRelPathBytes) return false;
  // Relative to /explorink, always. An absolute path would land wherever the
  // sender fancied.
  if (path[0] == '/' || path[len - 1] == '/') return false;

  size_t componentLen = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = path[i];
    // Printable ASCII only, and no backslash: SdFat takes '\\' as an ordinary
    // filename character, so allowing it just makes paths that look like
    // directories but are not.
    if (c < 0x20 || c > 0x7e || c == '\\') return false;
    if (c != '/') {
      ++componentLen;
      continue;
    }
    // An empty component is "//" -- harmless but a sign the sender built the
    // path wrong, and cheaper to reject than to normalise.
    if (componentLen == 0) return false;
    componentLen = 0;
  }

  // `..` anywhere is the one that matters: it is how a relative path escapes
  // the root. Checked as a whole component so a file legitimately called
  // `a..b.tib` is still allowed.
  //
  // **`.` and FAT's trailing-character trimming are rejected for a different
  // reason**: not escape, but *aliasing*. One file must have one spelling here,
  // because a second spelling is a second answer from every gate that reads the
  // path -- `wallet::pathIsWalletAsset()` decides whether a document gets
  // sealed, and FAT resolves `wallet/./ab/x.dat` and `wallet/ab/x.dat.` to the
  // same directory entry the gate just said was not a wallet asset. Added
  // 2026-11-27 with the case-folding fix next to it; the escape check alone let
  // all of these through.
  const char* cursor = path;
  while (cursor != nullptr) {
    const char* slash = strchr(cursor, '/');
    const size_t partLen = slash != nullptr ? static_cast<size_t>(slash - cursor) : strlen(cursor);
    if (partLen == 2 && cursor[0] == '.' && cursor[1] == '.') return false;
    if (partLen == 1 && cursor[0] == '.') return false;
    // FAT trims trailing dots and spaces from a name, so `x.dat.` and `x.dat `
    // open `x.dat`. An honest sender never writes either.
    if (partLen > 0 && (cursor[partLen - 1] == '.' || cursor[partLen - 1] == ' ')) return false;
    // **And it skips LEADING spaces, which is the same aliasing in the other
    // direction and was the hole this check missed.** SdFat's
    // `FatFile::parsePathName()` opens with `while (*path == ' ') path++;` for
    // every component, so ` wallet/ab/<id>.dat` and `wallet/ab/<id>.dat` are
    // one directory entry -- while the wallet gates, which compare the string,
    // see two different paths. A leading space therefore walked straight past
    // `pathTouchesWalletDir()` and landed a plaintext document on top of a
    // sealed one. Found in review, 2026-11-27, after two earlier rounds had
    // closed the case-fold and the 8.3-alias spellings of the same idea.
    if (partLen > 0 && cursor[0] == ' ') return false;
    cursor = slash != nullptr ? slash + 1 : nullptr;
  }

  return true;
}

}  // namespace maptransfer
