# Desktop simulator

The firmware compiles as a native host binary and draws the e-ink panel into an
SDL2 window. No device, no flash, no USB port to fight another session over.

Upstream built it: `crosspoint-reader/crosspoint-simulator`, MIT. We run a fork,
`rfordinal/explorink-simulator`, branch `explorink` — the simulator *replaces*
`lib/hal/` rather than extending it, so it is pinned to one firmware's HAL and
ours has diverged. The fork's own `EXPLORINK.md` lists every divergence and
which half of it belongs upstream.

## Build and run

```bash
pio run -e simulator                    # build
pio run -e simulator -t run_simulator   # build and launch
./.pio/build/simulator/program          # launch a built binary directly
```

Host packages (Debian/Ubuntu): `libsdl2-dev`, `libssl-dev`, `curl`.

`[simdep]` in `platformio.ini` names the simulator source. Point it at a local
checkout in `platformio.local.ini` (gitignored) while working on the simulator
itself:

```ini
[simdep]
source = symlink://../../firmware/explorink-simulator
```

**The number of `..` depends on where you are, and getting it wrong looks like a
missing dependency rather than a bad path.** `../../` is right from the main
firmware checkout and from a worktree under the legacy `trailink-worktrees/`
root, because both sit two levels below the parent repo. The current convention
is `.worktrees/firmware/<topic>` (parent `CLAUDE.md`), which is three levels
down and needs `../../../`:

| working from | source |
|---|---|
| `firmware/explorink` (main checkout) | `symlink://../../firmware/explorink-simulator` |
| `trailink-worktrees/<topic>` (legacy) | `symlink://../../firmware/explorink-simulator` |
| `.worktrees/firmware/<topic>` | `symlink://../../../firmware/explorink-simulator` |

Verified 2026-08-23 by resolving all three. An absolute path in
`platformio.local.ini` avoids the question entirely -- that file is gitignored
and personal, so there is no reason for it to be relative.

Only the symlink form has been exercised so far -- the committed git-URL default
is unverified.

## The simulated SD card

Everything the firmware reads from the card lives under `./fs_/` next to the
binary. `/trailink/base/13/4482/2789.tib` on the device is
`./fs_/trailink/base/13/4482/2789.tib` here. Symlink the local CDN mirror in
rather than copying tens of MB:

```bash
mkdir -p fs_/trailink
ln -sfn <repo>/mapbuilder/cdn/base   fs_/trailink/base
ln -sfn <repo>/mapbuilder/cdn/points fs_/trailink/points
```

Settings are a plain JSON file at `./fs_/.crosspoint/settings.json`
(`CrossPointSettings::getFilePath()`, src/CrossPointSettings.h:468).

## Getting a position on screen

Two ways, and the fast one needs no client at all.

**Seed the persisted fix.** Still the quickest way to a map on screen: write
`./fs_/.crosspoint/settings.json` and start the binary.

```json
{
  "mapHasLastFix": true,
  "mapLastLatE7": 483770000,
  "mapLastLonE7": 175880000,
  "mapLastHeading": 0
}
```

`MapActivity::onEnter` renders that immediately
(`src/activities/map/MapActivity.cpp`, "rendering persisted fix"), so the map
draws real tiles on the first frame. No socket, no client, no timing to get
right.

**Write the packet over BLE.** Since 2026-08-23 the real `BlePositionServer`
runs here, so `tools/blepos.py --sim <host>:<port>` writes the same 21-byte
packet a phone writes, and the whole ingest path runs -- length check, field
copies, the redraw gate, the viewport re-anchor. Use this one when the packet
itself is the thing under test: a seeded fix cannot exercise `seq`, `heading`,
`utc`, the altitude flag or a re-anchor. See "BLE" below.

Serial input is still absent, so `CMD:` and the map console are not reachable
that way -- but the map console's commands are reachable over the BLE command
characteristic, which is what `tools/mapcmd.py --sim` uses.

## Scripted runs and screenshots

No desktop-control needed. Times are milliseconds from process start.

```bash
mkdir -p qa-artifacts
CROSSPOINT_SIM_INPUT_SCRIPT='1200:ENTER;12000:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS='6000:./qa-artifacts/map.bmp' \
  ./.pio/build/simulator/program
```

Keys: `BACK`, `ENTER`, `LEFT`, `RIGHT`, `UP`, `DOWN`, `POWER`, `SLEEP`, `HOME`,
`QUIT`. Screenshots are BMP at the host's drawable resolution. Upstream's
`README.md` has the touch actions, the sleep/wake pair and the heap overrides.

## Device profiles

One env per device and panel controller, extending `[env:simulator]`: the base
env is X4 (800x480 SSD1677 framebuffer, drawn portrait). Upstream's samples add X3, X4 Pro,
Sticky, PaperMono and the UC8179/UC8279 controller revisions. Only the X4 env is
wired here so far.

## What it is and is not

**Is** the firmware's own code: the activity system, `GfxRenderer`,
`MapRenderer`, the `.tib` reader, settings, i18n. A map drawn here is drawn by
the same code the device runs.

**Is not** the panel. No refresh timing, no LUT waveforms, no ghosting, no
grayscale second pass, no memory pressure — heap reads a flat 1 MiB. So it
answers layout, geometry, labels and logic questions, and cannot answer a
dither, tone, or "does this fit in 380 KB" question. Those still need the
device (`docs/eink-grayscale.md`).

Also missing: serial input, and wolfSSL (`SecureNet` is in `lib_ignore`). BLE
used to be on that list and is not any more -- see "BLE" below for what the
shim does and does not reproduce.

## BLE

The real `BlePositionServer` compiles and runs here. The simulator ships a
NimBLE **shim**: a header-compatible fake that declares the same C++ API as
NimBLE-Arduino and implements it over a TCP socket, so a python client plays the
part of the phone. No NimBLE source is compiled. The shim's own wire protocol,
threading model and limits are the simulator fork's `docs/ble-shim.md`; the
FreeRTOS primitives it needed are that fork's `docs/freertos-shim.md`.

### Build wiring

Two things turn it on, and only one of them is a build flag.

`[env:simulator]` sets `-DFREEINK_CAP_BLE_PERIPHERAL=1` (platformio.ini:319-322).
Without it `BlePositionServer.cpp:1028` links stub bodies and `begin()` returns
false, which is what put "Bluetooth failed to start" on the map screen
(`src/activities/map/MapActivity.cpp:4506`).

`BLE` **stays** in that env's `lib_ignore` (platformio.ini:338). That entry
ignores NimBLE-Arduino. Removing it would compile the real library, which is the
opposite of the point: the shim supplies `NimBLEDevice.h`, `NimBLECharacteristic.h`,
`NimBLEConnInfo.h`, `NimBLEAttValue.h` and `host/ble_gap.h` instead.

### The one firmware source change it needed

`BlePositionServer.cpp` used `portMUX_TYPE`, `portENTER_CRITICAL`, `vTaskDelay`,
`pdMS_TO_TICKS` and the semaphore API without including a single FreeRTOS
header. On the device NimBLE-Arduino drags all of them in through `Arduino.h`,
so the file built. The simulator's `NimBLEDevice.h` does not, so it did not.
Every error the build printed was a `was not declared in this scope` on a
FreeRTOS name, and nothing else.

Fixed by naming them (`lib/BlePositionServer/src/BlePositionServer.cpp:26-28`):

```cpp
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
```

Correct on both targets and no conditional: ESP-IDF owns those exact three
paths, and it is what every other FreeRTOS user in this tree already does
(`src/activities/ActivityManager.h:3`, `lib/Xtc/Xtc.cpp:13`,
`lib/PngToBmpConverter/PngToBmpConverter.cpp:7`). Nothing else in the firmware
changed.

Checked on both targets, 2026-08-23: `pio run -e default` (ESP32-C3, the real
NimBLE-Arduino) still compiles, links and produces an image, with no new
warning in that file. Not flashed -- there is no device.

**The general shape, because there will be more of these.** The header of that
library is NimBLE-free by design and stayed that way; the `.cpp` was
free-riding on NimBLE-Arduino's include graph without anyone noticing, because
on device it compiles either way. Any firmware file that includes a third-party
library and then uses ESP-IDF or FreeRTOS names it never included is in the same
position, and swapping that library for a header-compatible fake is what
surfaces it. The symptom is unmistakable once seen: every error is a
`was not declared in this scope` on a FreeRTOS or ESP-IDF name, and none is on a
name belonging to the library that was replaced. Name the headers; do not guard
the include.

### Point `[simdep]` at a local simulator checkout with an absolute path

The depth table above is real, and an absolute path in `platformio.local.ini`
sidesteps it:

```ini
[simdep]
source = symlink:///home/<user>/.../.worktrees/simulator/<topic>
```

That file is gitignored (`.gitignore:21`, pattern `*.local*`).

### Turning the radio on at runtime

```
CROSSPOINT_SIM_BLE_PORT=8765     # absent, empty or 0 = no socket
```

Off by default, and the firmware does not care: `NimBLEDevice::init()` returns
true either way, so a run with no client behaves exactly as it did before the
shim existed. Verified by running, 2026-08-23: with the variable absent nothing
is listening on 8765 and a connect is refused; with it set,
`ss -ltnp` shows `LISTEN 127.0.0.1:8765` owned by `program`. Loopback only,
never a LAN nic.

### What a client sees

Verified by running, 2026-08-23. A python client that connects, sends
`{"op":"connect","mtu":517}` and subscribes to the command characteristic drove
real firmware callbacks:

```
[BLEPOS] connected: interval 24 units (30 ms), latency 0, timeout 400
[BLEPOS] MTU now 517, file payload 509 bytes per chunk
[BLEPOS] command channel subscribed
[BLEPOS] conn params: requested idle set (24-40 units, latency 4) after 5011 ms quiet
```

and the client read back the GATT table, both advertising transitions, the
firmware's own indication (`444941475f4d20353539372e370a` is `DIAG_M 5597.7\n`
on the command channel) and the `connparams_request`. Exiting the map emitted
`{"ev":"stack","state":"down"}`; re-entering it built the table again.

**`stack up` is not observable from a client.** The listener only exists after
`NimBLEDevice::init()` has started it, and the shim drops an event when no
client fd is accepted yet (simulator `src/SimBleLink.cpp:424`). A client racing
the accept sometimes wins `gatt` and sometimes does not. Read off the code plus
observed twice. Not a blocker for anything: every later event arrives.

### Fixed: the shim's self-test used to own `main`

**Was a blocker, resolved in the fork the same day.** The shim's GATT self-test
and its `SimBleLink` stub both defined their own `main()`/`SimBleLink` and both
sat in the library's `src/`, so PlatformIO compiled them into the library
archive next to `simulator_main.o`. The self-test's object sorted first, the
linker satisfied `main` from it, and the simulator never ran: the binary printed
nothing (stdout still buffered) and aborted in
`ActivityManager::~ActivityManager()` (`src/activities/ActivityManager.h:93`)
because `main` returned instead of calling `_exit(0)`.

Both files carried header comments saying they must not be linked. Nothing
enforced it. The fork moved them to a `tests/` directory the library does not
compile, which is enforcement rather than a request. **Keep them there.** The
library has no `srcFilter`, so anything under `src/` is in the archive, and a
comment cannot stop a linker.

If an older simulator checkout is pinned, the symptom is exactly the one above
and the workaround is to delete the two objects from the archive and re-link:

```bash
ar d .pio/build/simulator/lib*/libble-integration.a \
      SimBleGattSelfTest.o SimBleGattSelfTestStub.o
# then re-run the g++ link line from `pio run -e simulator -v`
```

### What the shim can and cannot answer

The shim's own limits are in the simulator fork's `docs/ble-shim.md`. Four of
them decide whether a number from a `--sim` run may be quoted at all, so they
are repeated here:

- **Throughput is meaningless.** A `write` returns when TCP took the bytes.
  There is no ATT write response and no connection interval. Measured: a 52 KB
  tile push clocked 3377 KB/s and 4196 KB/s through the socket, against
  **2.6 KB/s** the same tool measures over a real radio at MTU 256
  (`map-memory.md`, "Measured: a real transfer costs no heap" -- and note that
  `optimization/03-ble-link.md`'s ~6.2 kB/s for that MTU is an estimate marked
  open, not a measurement). Never quote a `--sim` transfer time.
- **Nothing negotiates the MTU, and nothing rejects an over-MTU write.** The
  client declares the MTU. A 485-byte frame was accepted on a link the firmware
  believed had a 15 byte payload budget. `MapTransferReceiver` never compares an
  arriving frame against `BlePositionServer::transferPayloadBytes()`, so on
  hardware the only thing bounding a frame is the ATT bearer -- and that cannot
  be tested here.
- **A link cannot die without notice.** `--sim` disconnects politely, so a run
  exercises the device's own disconnect cleanup and never a supervision
  timeout.
- **Heap says nothing.** `esp_get_free_heap_size()` is a flat 1000000 here, and
  the real NimBLE host plus BT controller are the biggest single RAM consumer on
  the device. A BLE feature that fits here can still not fit on hardware.

One more, because it is easy to misread the other way: the shim is far faster
than a radio. A race the code prices as theoretical can fire on the first try
here purely because a whole file transfer took milliseconds. That is evidence
the race exists, not evidence it happens on device.

### What was found by driving it, 2026-08-23

Four sessions drove the position path, the command channel, the transfer path
and the missing-tile fetch loop through the shim. **No hardware was involved in
any of it** -- there is no device. Every finding is therefore
measured-in-the-simulator or read-off-the-code, never measured-on-hardware, and
each one below says what a hardware pass would still have to check. They live in
the topic docs, not here:

| what | where |
|---|---|
| A position packet with no field validation: `lat = +/-90` wedges the map screen, a stale `seq` is drawn, `heading` is not masked | [`map-follow.md`](map-follow.md), "Nothing validates a position packet" |
| `kMaxBlocksPerPoll` does not bound the freeze it exists to bound: 69.1 s measured, and a truncated reply block silently merges into the next line | [`tile-freshness.md`](tile-freshness.md), the two "console flush" and "reply channel" sections |
| A corrupt tile is re-fetched forever; the transfer path's refusals all hold, and what they leave behind | [`missing-tiles.md`](missing-tiles.md), "A corrupt arrival is never refused" |
| An over-long command write is dropped in silence, on an unauthenticated channel | [`ble-advertising.md`](ble-advertising.md), "What the open channel now demonstrably allows" |

**A caveat that must travel with all of it.** When those runs were made the
laptop tools could not complete a handshake with the simulator: the `gatt`
event's `props` field arrives as an integer bitmask and the client assumed a
list, so every `--sim` tool died in `connect()`. Each session patched its own
local copy of the client to get past it. That makes every result above real
evidence about **this firmware** -- the firmware binary was unmodified and the
bytes on the characteristic were real -- and **no** evidence that the tooling
worked. Two separate claims; do not merge them. The client was fixed later the
same day and the type is now pinned in the wire contract, so a rerun needs no
patch.

## Verified 2026-08-23

Ubuntu, SDL2 2.30, OpenSSL 3.0.13. Boot, Home and the map screen all render;
the map loaded 4 z12 tiles, 2874 ways, 18 places in 20 ms and drew Trnava with
place labels, scale bar and compass. Screenshots in the branch's
`qa-artifacts/` (gitignored).

Re-run the same day with `-DFREEINK_CAP_BLE_PERIPHERAL=1`, same numbers: 4
tiles, 2874 ways, 18 places, a 480x800 screenshot at 11.88 percent dark pixels
and two grey levels, 12.12 s wall clock. Build stayed at 4 unique compiler
warnings (5 occurrences), the same 4 as before the flag -- none of them in a BLE
file.

The map screen no longer says "Bluetooth failed to start". With the persisted
fix cleared so `renderWaiting()` draws, the screenshot reads **"Waiting for BLE
position..."** (`STR_MAP_WAITING_BLE`), and `[MAP] BlePositionServer.begin()
failed` never appears in the log -- so `bleStartFailed_` is false
(`src/activities/map/MapActivity.cpp:1829-1832`).

**Open, seen in that run:** the map's zoom `+`/`-` buttons are clipped by the
right edge of the 480 px-wide frame. Unclear whether that is the simulator's
orientation transform or a real style-anchoring bug the device also has — a
device screenshot of the same screen would settle it.
