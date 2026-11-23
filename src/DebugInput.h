#pragma once

#include <cstdint>

// Synthetic hardware-button presses, fed from the USB serial console
// (`CMD:BUTTON`, src/main.cpp; docs/serial-button-injection.md).
//
// Why this exists: a host can already put two screens on the panel
// (CMD:GOTO_MAP, CMD:GOTO_TILESYNC) and read the panel back
// (CMD:SCREENSHOT), but everything else -- home, the file browser, the
// reader, settings, every confirm prompt -- is reachable only by a thumb on
// the device. So a screen nobody can walk to from the laptop gets looked at
// once and never again, which is the same gap CMD:GOTO_TILESYNC was added to
// close (docs/tile-freshness.md, "The check queue is dots"). It also means a
// bug report that starts "press back twice in the reader" cannot be
// reproduced without the hardware in hand, and the simulator has no serial
// thumb either.
//
// Devel builds only, gated on ENABLE_BUTTON_CMD (platformio.ini: default,
// sticky, simulator -- not gh_release, gh_release_rc, slim). A press injector
// in a shipped build is a thumb for whoever picks the device up: with a cable
// they could walk the menus, open the rider's books, read their pins. And it
// stays on the serial channel only, never on the BLE command grammar
// (MapCommandParser.h): BLE advertises with no pairing and no bonding
// (docs/ble-advertising.md), so a BLE version would hand that thumb to anyone
// in radio range instead of to someone holding the cable.
//
// The injection lands in MappedInputManager, not in HalGPIO: the same choke
// point the touch hint boxes already use (MappedInputManager::rawButton()),
// which keeps it in src/ where the simulator compiles it too and keeps
// lib/hal/ and the simulator's own HalGPIO fork untouched. The cost of that
// choice is that the three places reading HalGPIO directly do not see
// injected presses -- the power-hold deep sleep, the POWER+DOWN screenshot
// combo (main.cpp) and the reader's own POWER+DOWN check
// (EpubReaderActivity.cpp). Not being able to sleep the device from a debug
// command is the good half of that: a host script cannot drop the port it is
// talking through.
//
// Timing model. One press at a time, the rest queued, each press advanced one
// step per input frame (gpio.update() in loop()):
//
//   frame N     wasPressed + isPressed, held time starts at 0
//   frame N+k   isPressed, held time = now - down
//   frame N+m   wasReleased once, held time = the final total (>= holdMs)
//   frame N+m+1 idle, next queued press may start
//
// The frame gap after a release is deliberate: two presses in a row must look
// like two presses, not one long one. loop() runs at ~10 ms, so a `CMD:BUTTON
// back` with no hold reads as a ~10 ms tap and `CMD:BUTTON back 1500` holds
// long enough for the long-press paths (ReaderUtils::GO_HOME_MS and friends,
// which all read MappedInputManager::getHeldTime()).
namespace DebugInput {

// Which of MappedInputManager's three raw button reads is being answered.
enum class Query : uint8_t { Pressed, Released, Down };

// HalGPIO::BTN_* has no "none" value, and this layer needs one.
inline constexpr uint8_t kNoButton = 0xFF;

#ifdef ENABLE_BUTTON_CMD

// Names the console accepts, in HalGPIO button-index order.
inline constexpr const char* kButtonNames[] = {"back", "confirm", "left", "right", "up", "down", "power"};
inline constexpr uint8_t kButtonCount = 7;

// Queue one press. holdMs 0 is a tap. False when the queue is full -- the host
// is pressing faster than the device can act on, and dropping the press
// silently would make a script's screenshots line up with the wrong screen.
bool queue(uint8_t hwButton, uint32_t holdMs);

// Advance the state machine. Called once per input frame from loop(); frameSeq
// is HalGPIO::updateSequence(), so a second call in the same frame is a no-op
// and the callers below can be read in any order. nowMs is passed in rather
// than read here so the host test can drive time (test/debug_input).
void pump(uint32_t frameSeq, unsigned long nowMs);

bool button(uint8_t hwButton, Query query);
bool anyEdge(Query query);

// True while an injected press owns the frame: down, or released this frame.
// getHeldTime() has to answer the injected held time for exactly that long.
bool active();
unsigned long heldMs();

// kNoButton when the name is not one of kButtonNames. Case insensitive.
uint8_t buttonFromName(const char* name);

// Drop the current press and the queue. For tests.
void reset();

#else

// Release builds: every call site below compiles to a constant and the
// optimizer drops it, so there is nothing to reach and nothing to strip.
inline bool button(uint8_t, Query) { return false; }
inline bool anyEdge(Query) { return false; }
inline bool active() { return false; }
inline unsigned long heldMs() { return 0; }
inline void pump(uint32_t, unsigned long) {}

#endif  // ENABLE_BUTTON_CMD

}  // namespace DebugInput
