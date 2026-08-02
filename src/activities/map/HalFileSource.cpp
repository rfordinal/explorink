#include "HalFileSource.h"

HalFileSource::~HalFileSource() { close(); }

bool HalFileSource::open(const char* path) {
  close();
  if (path == nullptr) return false;

  // Storage.open rather than Storage.openFileForRead: a tile that is not on
  // the card is an ordinary outcome here -- it draws as hatch -- and
  // openFileForRead LOG_ERRs on every miss, which would turn a normal
  // viewport at the edge of the mapped area into a wall of error lines.
  file_ = Storage.open(path, O_RDONLY);
  open_ = file_.isOpen();
  return open_;
}

int HalFileSource::read(void* buf, size_t len) {
  if (!open_) return -1;
  return file_.read(buf, len);
}

bool HalFileSource::seek(uint32_t offset) {
  if (!open_) return false;
  return file_.seekSet(static_cast<size_t>(offset));
}

void HalFileSource::close() {
  // Guarded on open_, not on file_ alone: HalFile's methods assert on a
  // default-constructed handle (impl == nullptr), so close() must never run
  // against one.
  if (!open_) return;
  file_.close();
  open_ = false;
}
