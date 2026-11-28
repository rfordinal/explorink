// sync_channel_host: the device half of the sync channel on stdin/stdout, so a
// phone written in another language can drive it (T-2098, plan step 2: "driven
// from a host script"). scripts/sync_channel_phone.py is that phone, built on
// Python `noiseprotocol` -- an implementation that shares no code with
// SyncNoise.h, OpenSSL or mbedtls.
//
// One command per line in, one answer per line out:
//
//   code <nowMs>            -> QR <text> | NOCODE
//   frame <nowMs> <hex>     -> LINE <status line> | DELIVER <hex> | NONE
//   seal <text...>          -> LINE <NXS ...> | NOSESSION
//   disconnect              -> OK
//   table                   -> TABLE <count>
//
// The same Channel object the device runs, the OpenSSL primitives the host
// suite runs. Not a ctest: it needs a peer. Run the script.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "SyncChannel.h"
#include "SyncCryptoHost.h"

namespace {

int stored = 0;
bool persist(void*, const syncchannel::PairedPhones&) {
  ++stored;
  return true;
}

std::vector<uint8_t> fromHex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
  return out;
}

std::string toHex(const uint8_t* p, size_t n) {
  static const char* k = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += k[p[i] >> 4];
    s += k[p[i] & 15];
  }
  return s;
}

}  // namespace

int main() {
  const auto& c = synchost::crypto();
  uint8_t devPriv[32];
  c.random(devPriv, 32);
  syncchannel::Channel ch;
  if (!ch.begin(&c, devPriv, syncchannel::PairedPhones{}, syncchannel::PairingSink{nullptr, &persist})) return 1;

  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "code") {
      uint32_t now = 0;
      in >> now;
      char qr[syncchannel::kQrTextLen + 1];
      if (ch.issueCode(now) && ch.qrText(now, qr, sizeof(qr))) {
        std::cout << "QR " << qr << "\n";
      } else {
        std::cout << "NOCODE\n";
      }
    } else if (cmd == "frame") {
      uint32_t now = 0;
      std::string hex;
      in >> now >> hex;
      const auto f = fromHex(hex);
      switch (ch.onFrame(f.data(), f.size(), now)) {
        case syncchannel::Outcome::Reply:
          std::cout << "LINE " << ch.line() << "\n";
          break;
        case syncchannel::Outcome::DeliverInner:
          std::cout << "DELIVER " << toHex(ch.inner(), ch.innerLen()) << "\n";
          break;
        case syncchannel::Outcome::None:
          std::cout << "NONE\n";
          break;
      }
    } else if (cmd == "seal") {
      std::string text;
      std::getline(in >> std::ws, text);
      if (ch.sealLine(text.c_str())) {
        std::cout << "LINE " << ch.line() << "\n";
      } else {
        std::cout << "NOSESSION\n";
      }
    } else if (cmd == "disconnect") {
      ch.onDisconnect();
      std::cout << "OK\n";
    } else if (cmd == "table") {
      std::cout << "TABLE " << static_cast<int>(ch.table().count) << " stored=" << stored << "\n";
    } else {
      std::cout << "ERR unknown\n";
    }
    std::cout.flush();
  }
  return 0;
}
