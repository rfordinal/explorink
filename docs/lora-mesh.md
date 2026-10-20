# LoRa mesh: MeshCore, adverts, and the node list

The first thing on this radio that is a protocol rather than a byte stream.
`lora-bringup.md` is the layer under it -- the SX1262, the console, the service
task -- and it stays the instrument that answers "is the radio alive at all".

Built 2026-10-20 on `feat/lora-presence` and **run on two T5 S3 Pro boards the
same day: the link works**. Steps 1 and 3 of the hardware pass at the end are
done and marked there; the rest is still read off the code or measured by a
host build.

## What it does

An **advert** is MeshCore's packet for "who and where I am": a 32-byte Ed25519
public key, a unix timestamp, a 64-byte signature over both, and optionally a
name and a coordinate at 1e6 degrees. This firmware sends one when asked, and
keeps a table of every node whose advert it heard.

That table is the feature. Two boards on the same channel see each other in a
list, with signal strength, and that is what a range test needs.

Three parts:

1. **`PresenceMesh`** (`src/lora/PresenceMesh.{h,cpp}`) -- a `mesh::Mesh`
   subclass. Loads an identity, sends adverts, receives them.
2. **`LoraPeerTable`** (`src/lora/LoraPeerTable.{h,cpp}`) -- sixteen heard
   nodes, newest first. Plain C++, proved by `test/lora_peers/`.
3. **`LoraPeersActivity`** (`src/activities/lora/`) -- Home > LoRa nodes, one
   row per node. A bench screen, gated like the grayscale bench.

## It is not the group roster, and that is deliberate

`MapTeam` is an allowlist: a position from an id that is not in it is refused,
counted and never drawn (`team-markers.md`, "A stranger is not a member"). This
table is the opposite question -- everyone the antenna heard, approved or not --
because a board that is not in the roster yet is exactly the board a range test
is trying to hear.

Nothing on the node list reaches the map. Making a heard node a group member is
`team add <acr> <key>` with its key, typed on purpose.

**The seam between the two already exists and is not wired yet.**
`MapTeam::teamPosition()` is the transport-independent ingest that a LoRa frame
was always going to arrive through (`team-markers.md`, "The layer does not know
which radio spoke"). Feeding it from `onAdvertRecv()` for roster members is the
next pass, not this one: it needs the roster to hold MeshCore keys as
identifiers, and it needs a hardware pass behind it first.

## Why `mesh::Mesh` and not `BaseChatMesh`

MeshCore's own clients subclass `BaseChatMesh`, which brings contacts,
channels, text messages, ACKs and connections. MeshCore is messaging-first, so
that is the natural layer there and the wrong one here: ExplorInk answers where
am I and what is around me, and group text is a separate decision with its own
reason (parent `docs/lora.md`, "What we will not build").

Subclassing one layer lower costs two overrides -- `onAdvertRecv()` and
`logRxRaw()` -- and keeps the chat machinery out of the image.

## The radio adapter, and why it is not MeshCore's

MeshCore ships `RadioLibWrapper` and a `CustomSX1262` under it. We do not use
them. `LoraMeshRadio` (`src/lora/LoraMeshRadio.{h,cpp}`) implements
`mesh::Radio` over this repo's own `lib/LoraRadio` instead.

The reason is the board, not taste. On the T5 S3 Pro the radio's chip select is
also the SD card's bus mate and the panel's `pin_pwr`, and everything that
keeps those three apart lives in `LoraRadio` plus the service task in
`main.cpp`: one mutex over every chip call, NRESET held low while the radio is
off so the card gets its MISO back, and a DIO1 interrupt that services receive
off the rendering path (`lora-bringup.md`). A second driver would have to
reproduce all of that or quietly break it.

The translation is thin. Seven methods, and only three of them have any
thinking in them:

| `mesh::Radio` | answered with | note |
|---|---|---|
| `begin()` | `startListening()` | nothing else puts the chip in receive |
| `loop()` | re-arm if not listening and not transmitting | the Dispatcher only *notices* a radio that stopped listening; upstream's wrapper repairs it in `loop()` and so does this |
| `recvRaw()` | `poll()` | a CRC failure answers 0: the Dispatcher's contract has no third value |
| `getEstAirtimeFor()` | `timeOnAirMs()` | |
| `startSendRaw()` | `startTransmit()` | never the blocking `transmit()` -- it spins on `yield()` and starves core 0's idle task into a watchdog reboot |
| `isSendComplete()` | latched `finishTransmit()` | the chip is released by the call that reports success, and the Dispatcher polls until true -- asking twice would strand the packet |
| `packetScore()` | reimplemented | see below |

**`packetScore()` has to match upstream's numbers, not merely be plausible.**
It feeds `Dispatcher::calcRxDelay()`, which is how two nodes that heard the same
packet avoid answering on top of each other: a higher score waits less. The SNR
floor table is reimplemented from `RadioLibWrappers.cpp`'s `snr_threshold`
because that one is a file-static inside a class we do not link. Score on a
different curve than the rest of the mesh and this device becomes the one that
always talks first.

## Mesh mode and the bring-up console are exclusive

An SX126x holds exactly one received packet. Two owners polling it is two
readers of a one-slot buffer, so `CMD:LORA MESH ON` hands the chip to the
Dispatcher for the duration and `loraServiceStep()` returns early:

```
if (gMeshRunning) { gMesh.loop(); return false; }
```

The console's PING/PONG path, its re-arm watchdog and its transmit timeout all
sit after that line and do not run. The Dispatcher has its own versions of all
three, plus an airtime budget the console never had.

`MESH OFF` stops listening explicitly. Without that the chip stays in receive
with nobody reading it, costing 5.4 mA continuously (`lora-bringup.md`) with
nothing on screen to say why.

## The identity lives on the card

`/trailink/lora/identity.bin`, 96 bytes: 64 of private key then 32 of public,
exactly what `LocalIdentity::writeTo()` lays down.

**A node that draws a fresh key at every boot is useless for a list.** It
appears as a new stranger to everyone who already knows it, and a group's
rosters fill with ghosts of the same rider. When the card cannot be read or
written the node runs on a RAM-only identity and says so in the log and in the
`MESH ON` reply (`stored=0`), rather than refusing to start.

Written through `HalStorage`, not through MeshCore's `IdentityStore`: that one
is built on `fs::FS` and every file in this firmware goes through HalStorage,
which is the mutex that makes SdFat thread-safe.

## The beacon is off by default

An advert is a plaintext broadcast of this device's name and position to anyone
in range, readable by any MeshCore node. A lost or stolen device that beacons
is the rider's track handed to whoever holds it (parent `docs/lora.md`,
"Security of the mesh channel"). So nothing transmits until somebody asks.

`CMD:LORA BEACON <seconds>` sets the cadence, 0 turns it off, and **below 70
seconds it is refused rather than clamped**. 70 is not a round number: a
~100-byte MeshCore packet at this channel's settings (SF8, BW 62.5 kHz) is 615
to 714 ms of airtime depending on the preamble, so one packet a minute is 1.1 %
duty cycle and one every 70 seconds is the first cadence that fits under a 1 %
sub-band. Which sub-band 869.618 MHz actually sits in is still open -- Meshtastic's
own region table puts EU_868 at 10 %, which would make this very
conservative, and no primary ETSI reading has been done (parent
`docs/TODO.md`, T-285). The floor follows the stricter candidate until somebody
reads the standard.

A clamp was rejected on purpose: it answers OK to a number the device did not
use, and the next person reads the cadence off the command they typed.

Adverts go out **zero hop** -- to whoever can hear us directly, with nobody
asked to pass it on -- and `allowPacketForward()` returns false. That is
MeshCore's own companion behaviour and also the choice the one other project on
this board made deliberately: relaying puts every packet it hears back on the
air, and airtime and battery on a bike are not free.

## The console

Devel builds only, on top of the commands `lora-bringup.md` documents:

```
CMD:LORA MESH ON|OFF   ->  LORA_OK:mesh=1 name=<n> key=<8 hex> stored=0|1
CMD:LORA ADVERT        ->  LORA_OK:advert started
CMD:LORA BEACON <sec>  ->  LORA_OK:beacon=<sec>s     (0 = off, minimum 70)
CMD:LORA NAME <text>   ->  LORA_OK:name=<text>
CMD:LORA PEERS         ->  LORA_PEERS: then one LORA_PEER: per node
```

`PEERS` prints our own uptime-based age beside the sender's timestamp and
believes the sender about nothing: a node with no clock stamps a number that
means nothing at all. A node that has sent no coordinate prints `loc=-`, never
`0,0` -- that is a place in the Atlantic and a different statement entirely.

The two-board test is `MESH ON` on both, then `ADVERT` on one and `PEERS` on
the other.

## The screen, and the four controls on it

**The first version of this screen listed peers and nothing else**, and that
was the defect: `CMD:LORA MESH ON` over a USB cable was then the only way to
start the node. A device on a handlebar has no serial console, so the feature
was unreachable on the thing it was built for. Written down because it is a
shape worth recognising -- a bench instrument built console-first reads as
complete from the diff and is unusable in the hand.

Four rows sit above the heard nodes:

| row | value | Confirm does |
|---|---|---|
| Radio | Listening / Off | starts or stops the node |
| Broadcast my position | Never / Every 70 s / 2 min / 5 min / 15 min | cycles the ring |
| This device's name | the current name | opens the keyboard |
| Announce me now | -- | sends one advert; dimmed with the radio off |

"Listening", not "On", because that is what the radio is doing and it is the
honest word: the node hears everybody whether or not the beacon is telling
anybody about us.

**Each row writes the same `CrossPointSettings` field the console writes**, so
the screen and `CMD:LORA` are two faces of one state and cannot disagree --
the pattern the map menu and the Settings screen already follow for the fields
they share. `loraMeshOn`, `loraBeaconSec` and `loraNodeName` are not in
`SettingsList`: this is a bench feature gated out of every release build, and
its controls belong on its own screen rather than in a rider's Settings list
(same call `team-markers.md` made for its three numbers).

**The radio choice is honoured at boot**, immediately after the settings are
read. Not a default -- `loraMeshOn` is 0 out of the box, because a listening
SX1262 costs 5.4 mA continuously and drags the shared GNSS rail up with it --
but once a rider has turned it on it stays on, because re-arming a group radio
at every power cycle is how a group feature stops being trusted. The beacon
keeps its own switch and its own default of off, because listening and
transmitting are different consent.

**A control that failed says so in the header.** A switch that silently does
nothing is the worst thing a switch can do: a radio that refuses to start looks
exactly like a radio nobody has pressed. Two messages, and the second is not an
error -- a key that did not reach the card still works today, and the rider has
to know that every reboot will make this a new node to everyone who heard it.

## The list

Home > **LoRa nodes**, one row per heard node: name (or four bytes of the key
when the advert carried none), and under it RSSI, SNR, age and how many adverts
have arrived. The value column is the coordinate when there is one.

The header subtitle is this node's own state, because every row below it is
meaningless without it: an empty list means "nobody is out there" only if the
radio was listening, and "the mesh is off" otherwise.

**Select is a reload, not an open.** There is nothing behind a row, and there
is no auto-refresh timer either: on e-ink a screen that repaints itself every
few seconds is a screen that is unreadable while it does, and a range test is
walked between deliberate readings.

The screen never touches the live table. `PresenceMesh::onAdvertRecv()` runs on
the radio task; the screen takes a snapshot under the chip mutex once per
repaint (`src/lora/LoraPresence.h`). Walking the live array from the render
task would read records while they are half-rewritten -- a name truncated
mid-copy, a coordinate from one node under the key of another.

## Four traps in linking MeshCore, and what each cost

MeshCore is a `library.json` PlatformIO library, so a `lib_deps` entry is the
obvious way in. It is the wrong way in on this firmware, and it took four
failures to establish that. Written down because each one presents as an
unrelated error.

1. **`ed_25519.h: No such file or directory`.** Its Ed25519 is bundled in
   `meshcore/lib/`, which PlatformIO treats as *that project's* private library
   directory and does not export to a consumer.
2. **`MissingPackageManifestError`** when that directory is named in
   `lib_deps`. It carries no `library.json`, and the error's own suggestion
   ("check our library registry") is a dead end -- the code is bundled, not
   published.
3. **`lib_extra_dirs` does not fix it either.** MeshCore's manifest *declares*
   its dependencies, and a declared list makes PlatformIO skip scanning that
   library's own includes. `ed_25519.h` is never looked for at all.
4. **`SPI.h: No such file or directory`, from Adafruit BusIO.** Taking the
   manifest's dependencies drags in RTClib, Adafruit BusIO, Melopero RV3028 and
   CayenneLPP, and compiles `ESP32Board.cpp` and `CommonRadioPrefs.cpp`, which
   demand a board `target.h` this firmware has no business supplying. All of it
   for an advert.

**The answer is to compile the protocol core file by file**, which is what
`env:t5s3pro`'s `build_src_filter` does: five sources from `src/` plus
`AdvertDataHelpers` and `StaticPoolPacketManager`. Nothing in that set touches a
board, a display, a sensor or an RTC, because those are ours. The cost is one
line when an upstream bump adds a core file; the cost of not doing it is four
libraries and a stub header.

### The fifth trap is a real symbol collision

Eighteen symbols -- `fe_0`, `fe_mul`, `fe_tobytes` and fifteen more -- are
defined by both MeshCore's bundled Ed25519 and `Arduino-wolfSSL`, which this
firmware links for TLS. Both carry a descendant of the reference `ref10`
implementation and both kept its names. Measured with `nm` against the two
archives, 2026-10-20: every collision is `fe_*`; `ge_*`, `sc_*`, `sha512*` and
the public `ed25519_*` entry points are MeshCore's alone.

`src/lora/ed25519/mc_ed25519_prefix.h` renames them, and ten one-line wrappers
include the upstream `.c` files behind it. The submodule is never edited:
`meshcore` is pinned upstream and reviewed per batch (`CLAUDE.md`, "Upstream is
two tracks"), so the whole workaround stays on our side of the line and a
version bump cannot silently drop it.

**`-Wl,--allow-multiple-definition` was rejected.** It links, and that is the
problem: the linker keeps whichever definition it met first and binds
MeshCore's `ge.c` to wolfSSL's field arithmetic. Two `ref10` descendants are not
guaranteed to share an internal representation, and a signature scheme that is
subtly wrong does not fail loudly -- it produces adverts nobody can verify, or
verifies ones it should not.

### And the LDF breaks the C3 builds unless told not to

PlatformIO's LDF runs in `chain` mode, which does not evaluate preprocessor
conditionals. So `#include <LoraRadio.h>` inside `main.cpp`'s
`#ifdef ENABLE_LORA_CMD` is seen by *every* environment, and every C3 build
fails on `RadioLib.h: No such file or directory`. `base.lib_ignore` now carries
`LoraRadio` and `env:t5s3pro` drops it again -- rather than putting RadioLib in
`base.lib_deps`, which would compile a radio library into three boards that
have no radio.

## What it costs

`env:t5s3pro`, same tree, the only difference being `-DENABLE_LORA_MESH=1`.
Measured 2026-10-20 on this laptop, build 55.03.37 / RadioLib 7.7.1 /
MeshCore `companion-v1.18.0`:

| | without | with | cost |
|---|---|---|---|
| Flash | 4,014,699 B | 4,099,291 B | **84.6 kB** |
| RAM (static) | 73,080 B | 76,104 B | **3.0 kB** |

The RAM is the sixteen-packet pool, the sixteen-node table (about 1.2 kB) and
the mesh tables. Both numbers are before the node list screen, which added
2.8 kB of flash and no static RAM.

**Heap at runtime is not measured.** Nothing here allocates -- the packet pool,
the peer table and the mesh tables are all static arrays, by design, because
the thing filling them is a radio anyone in range can talk to. That is read off
the code and not off a device.

## What is open

- **Everything on hardware.** No board has run this. See below.
- **The RSSI a peer row shows** is the radio's last reported frame, not the
  packet's own: MeshCore's `Packet` carries SNR and has no RSSI field. For an
  advert the Dispatcher processed straight away -- which is every advert on
  quiet air -- they are the same frame. Under traffic a scored packet can be
  held back, and then the number belongs to something that landed afterwards.
- **Our own adverts carry a timestamp nobody can date.** The mesh runs on
  `VolatileRTCClock`, which counts from an arbitrary epoch. The board has a
  PCF85063 and a GNSS receiver that both know better and neither is wired in.
- **No roster ingest**, no map markers. The seam is named above.
- **Nothing verifies an advert against a roster entry** beyond MeshCore's own
  signature check, which proves the sender holds the key and says nothing about
  whether we ever agreed to listen to them.
- **The 70-second beacon floor rests on an unread standard** (T-285).

## The hardware pass

In this order. Each step can kill the next. Build `638e7b27`, archived in the
parent repo as `docs/firmware-builds/t5s3pro-{a,b}-lora-presence-638e7b27-link-verified.*`
-- two archives, because `-t upload` relinks and the two boards therefore carry
different bytes of the same commit.

1. **One board.** `CMD:LORA ON`, `CMD:LORA MESH ON`. The reply must carry a key
   and `stored=1`. Reboot, `MESH ON` again: the same key, or the identity file
   is not surviving.
   **Done 2026-10-20, half.** Both boards came up clean:
   `LORA_OK:on chip=SX1261 V2D 2D02. tcxo=1.8V xosc=ok errors=0x0000`, then
   `LORA_OK:mesh=1 name=EI-3e8f key=3e8f8e75 stored=1` on board A and
   `name=EI-c6c2 key=c6c28a10 stored=1` on board B -- two distinct keys, both
   written to the card, the default name derived from the key as intended.
   **The reboot half is not done**: nothing has yet confirmed the same key
   comes back after a power cycle, which is the part that matters.
2. **The card is still safe.** With the mesh running, open the map and pan.
   `lora-bringup.md`'s SD-bus question is about a radio *listening* while tiles
   stream, and the mesh listens continuously. **Not done.**
3. **Two boards.** `MESH ON` on both, `ADVERT` on one, `PEERS` on the other.
   One row, the right key, a plausible RSSI.
   **Done 2026-10-20, both directions.** A to B:
   `LORA_PEER:0 key=3e8f8e75 name=EI-3e8f rssi=-13 snr=12 age=7s adv=1 loc=-`.
   B to A: the same shape with `key=c6c28a10` and `snr=13`. `rejected=0` on
   both. Desk distance, so -13 dBm says nothing about range -- what it says is
   that a signed advert was built, sent, received, verified and parsed, and
   that the table keyed it correctly. `loc=-` is the no-fix path printing
   nothing rather than 0,0, which is the rule it was written for.
4. **The screen.** Home > LoRa nodes on the listening board, same row.
   **Not done** -- it needs eyes on the panel.
5. **The beacon.** `BEACON 70` on one, leave both. Rows must keep arriving and
   the age must keep resetting; `LORA_PEERS` `rejected=` must stay 0.
   **Not done.**
6. **Then, and only then, range.** Two boards, a walk, `PEERS` on each at
   intervals. This is bring-up step 4 in parent `docs/lora.md` and the thing
   every protocol decision above it is waiting on. **Not done.**

### Flashing these two boards needs `/dev/serial/by-id/`, not `/dev/ttyACM*`

Both boards were flashed on 2026-10-20 and **the first two uploads hit the same
board**, because `/dev/ttyACM0` and `/dev/ttyACM1` swapped between the
`udevadm` check and esptool opening the port. The check passed, the flash
verified its hash, and the wrong board took both images. It was caught only by
`esp_app_desc`: the board that should have been updated still reported
`project_name: t5s3-sync-develop`, linked two days earlier.

`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00` carries
the MAC in the path and follows the device across renumbering, so the race
cannot happen. And esptool prints the MAC it actually talked to -- read that
line after every flash, because it is the only statement about which board
received the bytes.
