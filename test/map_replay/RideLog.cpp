#include "RideLog.h"

#include <cstdlib>
#include <fstream>

#include "StreamingJsonParser.h"

namespace RideLog {
namespace {

// One line's worth of the four fields that matter. The firmware's own
// StreamingJsonParser does the parsing rather than a hand-rolled scanner --
// same reason this harness links MapFollow.cpp instead of re-implementing it:
// a second parser is a second thing that can be wrong on its own.
struct LineCtx {
  int depth = 0;
  std::string key;
  bool isPacket = false;
  bool ok = false;
  bool haveLat = false;
  bool haveLon = false;
  bool haveHeading = false;
  double lat = 0.0;
  double lon = 0.0;
  long heading = 0;
};

void onKey(void* ctx, const char* key, size_t len) {
  auto* c = static_cast<LineCtx*>(ctx);
  if (c->depth == 1) c->key.assign(key, len);
}

void onString(void* ctx, const char* value, size_t len) {
  auto* c = static_cast<LineCtx*>(ctx);
  if (c->depth != 1) return;
  if (c->key == "type") c->isPacket = (std::string(value, len) == "packet");
}

void onNumber(void* ctx, const char* value, size_t len) {
  auto* c = static_cast<LineCtx*>(ctx);
  if (c->depth != 1) return;
  const std::string text(value, len);
  if (c->key == "lat") {
    c->lat = std::strtod(text.c_str(), nullptr);
    c->haveLat = true;
  } else if (c->key == "lon") {
    c->lon = std::strtod(text.c_str(), nullptr);
    c->haveLon = true;
  } else if (c->key == "heading") {
    c->heading = std::strtol(text.c_str(), nullptr, 10);
    c->haveHeading = true;
  }
}

void onBool(void* ctx, bool value) {
  auto* c = static_cast<LineCtx*>(ctx);
  if (c->depth == 1 && c->key == "ok") c->ok = value;
}

void onNull(void*) {}
void onObjectStart(void* ctx) { ++static_cast<LineCtx*>(ctx)->depth; }
void onObjectEnd(void* ctx) { --static_cast<LineCtx*>(ctx)->depth; }
void onArrayStart(void*) {}
void onArrayEnd(void*) {}

std::string stemOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  return dot == std::string::npos ? base : base.substr(0, dot);
}

}  // namespace

bool read(const std::string& path, Ride& out, int& skippedLines) {
  std::ifstream file(path);
  if (!file) return false;

  out.name = stemOf(path);
  out.packets.clear();
  skippedLines = 0;

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;

    LineCtx ctx;
    JsonCallbacks callbacks{};
    callbacks.ctx = &ctx;
    callbacks.onKey = onKey;
    callbacks.onString = onString;
    callbacks.onNumber = onNumber;
    callbacks.onBool = onBool;
    callbacks.onNull = onNull;
    callbacks.onObjectStart = onObjectStart;
    callbacks.onObjectEnd = onObjectEnd;
    callbacks.onArrayStart = onArrayStart;
    callbacks.onArrayEnd = onArrayEnd;

    StreamingJsonParser parser(callbacks);
    parser.feed(line.data(), line.size());
    if (parser.hasError()) {
      ++skippedLines;
      continue;
    }
    if (!ctx.isPacket) continue;
    if (!ctx.ok) continue;
    if (!ctx.haveLat || !ctx.haveLon || !ctx.haveHeading) {
      ++skippedLines;
      continue;
    }

    Packet packet;
    packet.lat = ctx.lat;
    packet.lon = ctx.lon;
    packet.headingStep = static_cast<uint8_t>(ctx.heading & 0x0F);
    out.packets.push_back(packet);
  }
  return true;
}

}  // namespace RideLog
