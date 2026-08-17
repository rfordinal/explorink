#include "PinLogScanner.h"

void PinLogScanner::emit() {
  buf_[len_] = '\0';
  if (fn_ != nullptr) fn_(ctx_, std::string_view(buf_, len_));
  len_ = 0;
}

void PinLogScanner::feed(const char* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (c == '\n' || c == '\r') {
      if (discarding_) {
        discarding_ = false;
        len_ = 0;
        ++droppedLong_;
        continue;
      }
      emit();
      continue;
    }
    if (discarding_) continue;
    if (len_ >= kPinLineMax) {
      // Too long to be a record. Drop what is buffered and swallow the rest of
      // this line: a truncated record is worse than no record, because it can
      // still parse.
      discarding_ = true;
      len_ = 0;
      continue;
    }
    buf_[len_++] = c;
  }
}

void PinLogScanner::finish() {
  if (discarding_) {
    discarding_ = false;
    len_ = 0;
    ++droppedLong_;
    return;
  }
  if (len_ > 0) {
    droppedTail_ = true;
    len_ = 0;
  }
}

void pinReplayLine(std::string_view line, PinStore& store, PinReplayStats& stats) {
  if (line.empty()) return;  // a blank line is not a record and is not damage

  PinRecord rec;
  if (!decodePinRecord(line, rec)) {
    ++stats.skipped;
    return;
  }
  if (!store.apply(rec)) {
    ++stats.skipped;
    return;
  }
  ++stats.applied;
}

void PinLogReplayer::onLine(void* ctx, std::string_view line) {
  auto* self = static_cast<PinLogReplayer*>(ctx);
  pinReplayLine(line, self->store_, self->stats_);
}

void PinLogReplayer::finish() {
  scanner_.finish();
  // The tail and the over-long lines are damage the caller should see in one
  // number, so they land in the same counter as an unparseable record.
  stats_.skipped += scanner_.droppedLong();
  if (scanner_.droppedTail()) ++stats_.skipped;
}
