#include <gtest/gtest.h>

#include "DownloadResumePolicy.h"

// The decision a resumable download takes after each attempt. Every case here
// is one a real download will not reproduce on demand -- a short range, a
// barren reconnect, an expired signature -- which is the reason this logic was
// lifted out of the transfer loop in the first place. T-2064.

using download_resume::Attempt;
using download_resume::decide;
using download_resume::State;
using download_resume::Step;

namespace {

constexpr size_t WHOLE = 4'105'888;

Attempt fresh(int status, bool complete, size_t downloaded, size_t total = WHOLE) {
  Attempt a;
  a.status = status;
  a.bodyComplete = complete;
  a.resumedFrom = 0;
  a.downloaded = downloaded;
  a.total = total;
  a.onStartUrl = true;
  return a;
}

Attempt resumed(int status, bool complete, size_t from, size_t downloaded, bool onStartUrl = false) {
  Attempt a;
  a.status = status;
  a.bodyComplete = complete;
  a.resumedFrom = from;
  a.downloaded = downloaded;
  a.total = WHOLE;
  a.onStartUrl = onStartUrl;
  return a;
}

}  // namespace

TEST(DownloadResume, WholeBodyOnOneConnectionIsDone) {
  State state;
  EXPECT_EQ(decide(fresh(200, true, WHOLE), state), Step::Done);
  EXPECT_EQ(state.resumes, 0);
}

TEST(DownloadResume, CompleteRangeThatCoversTheRestIsDone) {
  State state;
  EXPECT_EQ(decide(resumed(206, true, 3'000'000, WHOLE), state), Step::Done);
}

// The defect this test exists for: before the downloaded-vs-total check, a 206
// whose announced range was shorter than the remainder reported success and the
// caller kept a truncated file. RFC 7233 permits exactly that answer.
TEST(DownloadResume, ShortButCompleteRangeIsNotDone) {
  State state;
  EXPECT_EQ(decide(resumed(206, true, 3'000'000, 3'001'000), state), Step::Retry);
  EXPECT_EQ(state.resumes, 1);
}

TEST(DownloadResume, TransportDeathPartWayThroughRetries) {
  State state;
  EXPECT_EQ(decide(fresh(-1, false, 61'810), state), Step::Retry);
  EXPECT_EQ(state.resumes, 1);
  EXPECT_EQ(state.barren, 0);
}

// Measured shape of a real C3 install: progress, progress, nothing, progress.
// Giving up on the first barren attempt stopped a transfer at 3,661,739 bytes.
TEST(DownloadResume, OneBarrenAttemptBetweenProgressDoesNotEndIt) {
  State state;
  EXPECT_EQ(decide(fresh(-1, false, 100'000), state), Step::Retry);
  EXPECT_EQ(decide(resumed(206, false, 100'000, 100'000), state), Step::Retry);
  EXPECT_EQ(state.barren, 1);
  EXPECT_EQ(decide(resumed(206, false, 100'000, 900'000), state), Step::Retry);
  EXPECT_EQ(state.barren, 0);
}

TEST(DownloadResume, ThreeBarrenAttemptsInARowGiveUp) {
  State state;
  EXPECT_EQ(decide(fresh(-1, false, 100'000), state), Step::Retry);
  EXPECT_EQ(decide(resumed(206, false, 100'000, 100'000), state), Step::Retry);
  EXPECT_EQ(decide(resumed(206, false, 100'000, 100'000), state), Step::Retry);
  EXPECT_EQ(decide(resumed(206, false, 100'000, 100'000), state), Step::Fail);
  EXPECT_EQ(state.barren, download_resume::MAX_BARREN_RESUMES);
}

TEST(DownloadResume, ResumesAreCapped) {
  State state;
  size_t have = 0;
  for (int i = 0; i < download_resume::MAX_RESUMES; ++i) {
    have += 10'000;
    ASSERT_EQ(decide(resumed(206, false, have - 10'000, have), state), Step::Retry) << "at resume " << i;
  }
  EXPECT_EQ(decide(resumed(206, false, have, have + 10'000), state), Step::Fail);
}

// A server that ignores Range restarts the body at zero. No sink on this path
// can rewind, so appending that would corrupt what is already written.
TEST(DownloadResume, TwoHundredInAnswerToARangeRequestIsFatal) {
  State state;
  EXPECT_EQ(decide(resumed(200, true, 3'000'000, 3'000'000), state), Step::Fail);
  EXPECT_EQ(state.resumes, 0);
}

TEST(DownloadResume, ExpiredSignatureOnAResumedCdnUrlReResolves) {
  State state;
  EXPECT_EQ(decide(resumed(403, false, 2'000'000, 2'000'000), state), Step::ReResolve);
  EXPECT_EQ(state.resumes, 1);
}

TEST(DownloadResume, ForbiddenOnTheStartUrlIsNotAnExpiredSignature) {
  State state;
  EXPECT_EQ(decide(resumed(403, false, 2'000'000, 2'000'000, /*onStartUrl=*/true), state), Step::Fail);
  EXPECT_EQ(decide(fresh(403, false, 0), state), Step::Fail);
}

// A permanently 403ing CDN must not burn the whole resume budget: the
// re-resolve path is barren by definition, so the barren counter bounds it.
TEST(DownloadResume, RepeatedExpiryGivesUpOnTheBarrenBudget) {
  State state;
  EXPECT_EQ(decide(resumed(403, false, 2'000'000, 2'000'000), state), Step::ReResolve);
  EXPECT_EQ(decide(resumed(403, false, 2'000'000, 2'000'000), state), Step::ReResolve);
  EXPECT_EQ(decide(resumed(403, false, 2'000'000, 2'000'000), state), Step::Fail);
  EXPECT_LT(state.resumes, download_resume::MAX_RESUMES);
}

TEST(DownloadResume, AnyOtherStatusIsFatal) {
  for (const int status : {400, 401, 416, 500, 503}) {
    State state;
    EXPECT_EQ(decide(fresh(status, false, 0), state), Step::Fail) << "status " << status;
  }
}

// No Content-Length means no offset to resume from and no way to tell a short
// body from a whole one, so a chunked response is never resumed. It still
// completes normally.
TEST(DownloadResume, ChunkedResponseCompletesButIsNeverResumed) {
  State done;
  EXPECT_EQ(decide(fresh(200, true, 12'345, /*total=*/0), done), Step::Done);
  State cut;
  EXPECT_EQ(decide(fresh(-1, false, 12'345, /*total=*/0), cut), Step::Fail);
}

// The connection dying after the headers and before the first body byte is the
// attempt the whole mechanism exists for; it must still retry.
TEST(DownloadResume, DeathBeforeTheFirstBodyByteRetriesOnceTheSizeIsKnown) {
  State state;
  EXPECT_EQ(decide(fresh(-1, false, 0), state), Step::Retry);
  EXPECT_EQ(state.barren, 1);
}
