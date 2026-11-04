#pragma once
#include <cstddef>

// What to do after one HTTP attempt of a resumable download.
//
// This is the whole decision, lifted out of HttpDownloader's transfer loop so
// it can be exercised on a host. The alternative is a four-megabyte download
// on a C3 per transition, and the transitions that matter -- a short range, a
// barren reconnect, an expired signature -- are the ones a real download will
// not reproduce on demand. Everything here is arithmetic over the outcome of
// an attempt; nothing touches the network, the heap or a sink.
namespace download_resume {

// Reconnect attempts allowed after a body is cut short. Each costs a TLS
// handshake, so this bounds how long a download may limp, not how long it may
// stall -- a peer that accepts the connection and then goes quiet is bounded
// by the socket timeout instead. Measured on an X3, 2026-11-04: five
// consecutive OTA installs needed 9, 3, 3, 10 and 5 reconnects, so a cap in
// single digits would have failed two of them.
constexpr int MAX_RESUMES = 24;
// A reconnect that delivers nothing is still worth retrying: late in a
// download the connection often dies on the first record after the handshake
// and the next attempt gets further. Giving up on the first barren one stopped
// a transfer at 3,661,739 of 4,105,888 bytes (X3, 2026-11-04).
constexpr int MAX_BARREN_RESUMES = 3;

enum class Step {
  Done,        // the whole body is in; report success
  Retry,       // reconnect to the same URL and ask for the rest
  ReResolve,   // the signed URL expired; go back to the start URL, then retry
  Fail,        // give up
};

// One attempt's outcome, as the transfer loop sees it.
struct Attempt {
  int status = 0;             // HTTP status, or negative when the transport died
  bool bodyComplete = false;  // the server delivered the length it announced
  size_t resumedFrom = 0;     // byte offset this attempt asked for; 0 = a fresh GET
  size_t downloaded = 0;      // total bytes handed to the sink so far
  size_t total = 0;           // whole-body size, 0 while still unknown
  bool onStartUrl = true;     // false once a redirect has moved us off it
};

// Carried across attempts.
struct State {
  int resumes = 0;
  int barren = 0;
};

inline bool transportDied(const Attempt& a) { return a.status < 0; }

// A 403 or 404 is an ordinary failure except in one case: a signed CDN URL
// whose signature expired while the download was being resumed. Then the
// redirect that minted it can simply be replayed.
inline bool signatureExpired(const Attempt& a) {
  return (a.status == 403 || a.status == 404) && a.resumedFrom > 0 && !a.onStartUrl;
}

inline Step decide(const Attempt& a, State& state) {
  // A server that ignores Range answers 200 and restarts the body at zero.
  // Appending that to what the sink already holds would corrupt it, and no
  // sink on this path can rewind, so this is fatal rather than another
  // attempt.
  if (a.resumedFrom > 0 && a.status == 200) return Step::Fail;

  const bool died = transportDied(a);
  const bool expired = signatureExpired(a);
  if (!died && !expired && a.status != 200 && a.status != 206) return Step::Fail;

  // Complete only when the bytes are all here. A 206's "body complete" means
  // the server delivered the range it announced, and RFC 7233 lets it announce
  // less than the remainder, so on its own it no longer implies a whole body.
  // Without this a short range reports success and the caller keeps a
  // truncated file.
  if (!died && !expired && a.bodyComplete && (a.total == 0 || a.downloaded >= a.total)) return Step::Done;

  // Everything left is an attempt that ended early. Retry while attempts keep
  // making progress; a run of barren ones means the server or the heap cannot
  // serve this at all, and reconnecting 24 times against that is only a slower
  // way to fail.
  state.barren = a.downloaded > a.resumedFrom ? 0 : state.barren + 1;
  // Without a known size there is no offset to resume from and no way to tell
  // a short body from a whole one, so a chunked response is never resumed.
  if (a.total == 0 || state.resumes >= MAX_RESUMES || state.barren >= MAX_BARREN_RESUMES) return Step::Fail;

  ++state.resumes;
  return expired ? Step::ReResolve : Step::Retry;
}

}  // namespace download_resume
