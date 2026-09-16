#pragma once

namespace DeviceIdentity {

// A short id for the board this binary is running on right now: "X4", "X3",
// "X4Pro", "T5S3Pro". Empty for anything outside ExplorInk's target device
// line (CLAUDE.md, "Target devices"). Used to build a per-device WiFi
// hostname/SSID so several ExplorInk devices on one LAN are distinguishable --
// see docs/webserver-endpoints.md.
//
// Lives in its own lib, not lib/hal/: the simulator build replaces lib/hal/
// wholesale (lib_ignore = hal in platformio.ini's [env:simulator]) but still
// builds the network activities that need this.
const char* activeBoardId();

}  // namespace DeviceIdentity
