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

### A heard position reaches the map, since 2026-10-22

`MapTeam::teamPosition()` is the transport-independent ingest that a LoRa frame
was always going to arrive through (`team-markers.md`, "The layer does not know
which radio spoke"), and it is wired now: an advert that carries a coordinate
and belongs to a roster member moves that member's marker.

**It is wired through a queue, and which task does what is the reason.**
`onAdvertRecv()` runs on the radio task; `teamPosition()` writes a black box
row to the SD card and reads a roster the UI task edits. A card write inside
the radio service loop costs packets -- an SX126x holds exactly one -- and
touching the roster from two tasks is a race. So the radio task copies four
numbers into a queue and returns, and `loop()` does the roster lookup, builds
the `TeamFix` and calls the ingest. Same split the console's receive path
already uses.

**Membership is checked on the loop task, before the ingest is called.** Not
only to keep roster reads on one task: on a public channel most adverts are
strangers, and letting `teamPosition()` refuse each one would bury
`strangersRefused()` -- the number that is supposed to say somebody is talking
*at* this group -- under every passing node from Graz.

The advert carries 1e6 degrees and `TeamFix` stores 1e7, so the last digit is
always zero. That is a unit conversion, not precision.

**The layer stays transport-neutral and this must not erode it.** A position
reaches `MapTeam` from this radio, from a phone relay over BLE, or from the
console, and a member holds one id per transport that has ever named them --
which is why the id written here is `lora-`-prefixed rather than bare. The one
place that slipped and was corrected the same day: the node list's third
section says **"not on LoRa"** and not "not heard", because this screen can
only see one transport and must not report the others as silent.

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

## The clock has to be ticked, or only the first advert is ever heard

**`VolatileRTCClock::tick()` is not boilerplate.** MeshCore's examples call it
from `loop()` next to `the_mesh.loop()`, it looks like housekeeping, and
leaving it out produces a failure that points at entirely the wrong end of the
link.

The chain: an advert carries the clock's value as its timestamp; a frozen
clock makes every advert byte-identical to the last; a receiver discards a
packet whose hash it has already seen (`MeshTables::wasSeen`, `Mesh.cpp`). So
the **first** advert is received normally and every one after it is dropped
silently at the far end. The sender sees a successful send, the receiver logs
nothing, and no counter anywhere moves.

**Measured 2026-10-20, and it cost an hour.** Board A was beaconing every
120 s; board B reported `LORA_PEER:0 key=3e8f8e75 name=RF-alfa ... age=206s
adv=1`. The search went through the sender first -- the beacon timer, the
`loop()` call chain, MeshCore's airtime budget (which starts at 1.8 million ms
and was never the limit) -- before the receiver's duplicate table was
suspected at all. The symptom "my radio does not transmit" was the wrong
sentence: it transmitted every time.

Two consequences, both in `loraDrain()` and `meshStart()`:

- The clock is ticked once per `loop()` iteration.
- It is seeded from the GNSS receiver at mesh start when the receiver already
  knows the time, which it does before it has a fix (`gnss.md`). Not required
  for the mesh to work -- required for the timestamp to mean anything to
  anybody else.

**And `LORA_STATE` now prints the beacon's own state** -- `mesh=`, `beacon=`,
`due_in=` -- because "it never transmits" has three possible causes that look
identical from outside: the mesh is off, the cadence is 0, or the next-due
stamp is in the future. Guessing between them is what the hour went on.

## The radio runs on two screens and nowhere else

**Maintainer's rule, 2026-10-21, and it corrected an assumption this feature
was built on.** The first version ran the node from boot, on every screen. The
argument for that was that a group has to find you when you are not looking at
the device -- which sounds right and misreads this device: **an open map is
not somebody staring at a screen, it is the state the device lives in during a
ride, pocket included.** Nobody closes the map to put it away.

So the radio is up while the map or the LoRa list is open, and everywhere else
-- Home, settings, the browser, asleep -- it **neither transmits nor
listens**.

**The second half is the one that pays.** A listening SX1262 costs 5.4 mA
continuously; a 30-second beacon adds about 3 mA on top
(`../../docs/lora.md`, "What a beacon cadence costs"). Gating receive is
almost all of the battery saving and gating transmit is almost none of it --
transmit is gated for airtime and privacy, which are different reasons and
worth keeping separate.

`loraMeshOn` therefore means **the radio may be used**, not the radio is
running. `loraPresenceHoldRadio()` decides when, called from
`MapActivity::onEnter/onExit` and the LoRa list's own.

### Two holders, and they are not equal

The radio runs while any holder wants it and is parked the moment none do.
There are two, and the difference is deliberate:

| holder | set by | obeys `loraMeshOn`? |
|---|---|---|
| screen | the map and the LoRa list, on enter and exit | **yes** |
| console | `CMD:LORA MESH ON` / `OFF` | **no** |

The console holder is a bench override: it forces the radio up on any screen
and ignores the rider's switch, because bring-up on a fresh card has to be
possible before anything has been enabled through a UI. It also does **not**
write `loraMeshOn`, so a console session cannot quietly leave a device enabled
for its owner.

**This asymmetry is written down rather than left to be found.** It was found
the hard way on 2026-10-21: the gate had just been built and documented, and
one `CMD:LORA MESH ON` typed on the Home screen brought the radio straight up
-- the console was going around the rule the doc had just finished asserting.
A devel console doing what the product cannot is correct; a doc that does not
say so is not.

Two consequences a reader should expect rather than report as bugs:

- **The node list is much emptier than a continuously listening one.**
  Strangers are only heard during a map session.
- **Two riders hear each other only when both have the map open** inside the
  same window. On a ride that is the normal state; sitting at a table with the
  device on Home, both are silent, by design.

## When a position goes out

One mechanism, three states, all of them inside the gate above. **The cadence
row is a floor, not a period** -- no faster than this, not exactly this often.

| state | what goes out | how often |
|---|---|---|
| moving, fix | the position | every 100 m, never faster than the floor |
| parked, fix | the same position again | once an hour |
| no fix | the name, no coordinate | once an hour |

The parked case is the one that is easy to get wrong: **an hour in a pub on a
hike still has to tell the group where the pub is**, so silence is capped
rather than left to the movement test. And a fix appearing or disappearing
sends immediately, because "I can see the sky again" is news that should not
wait out the cap.

A node with no fix sends its nameplate and never 0,0 -- the same distinction
the peer table keeps and the panel prints as `loc=-`.

**The numbers are OpenTrailPaper's, not ours**: 100 m, and an hourly heartbeat
when parked, from a project that has run groups on this band for years
(`../../docs/lora.md`, "And their cadence is the precedent our own guess was
missing"). Ours start there, and moving them has to say what it buys.

**The schedule survives the screen gate.** `meshStart()` deliberately does not
rearm anything: the floor is measured from the last advert actually sent, not
from the last time a screen opened. Without that, flipping between Home and
the map would be a transmitter.

## There is no certificate, and what the identity actually rests on

Worth stating plainly because the word invites the wrong model: **there is no
certificate, no authority and no PKI here.** Nothing is signed on this
device's behalf by anybody. The identity is an Ed25519 keypair and **the
public key is the name that matters** -- a signature proves possession of the
private key and says nothing about who that is.

### What makes the key unique

`PresenceMesh::beginIdentity()` builds it with `mesh::LocalIdentity(getRNG())`,
and the RNG is MeshCore's `StdRNG`, which is `random(0, 256)` per byte.

**We never call `gMeshRng.begin()`, and that is deliberate rather than an
omission.** Read in the pinned Arduino core (`cores/esp32/WMath.cpp:36-61`):
`random()` uses `esp_random()`, the hardware generator, *until* somebody calls
`randomSeed()` -- which sets `s_useRandomHW = false` and drops the whole
process to `srand()/rand()`. MeshCore's own example does exactly that
(`fast_rng.begin(radio_driver.getRngSeed())`), so **following their example on
ESP32 would make our keys weaker, not stronger.**

So today the key comes from the hardware RNG. Measured consequence: two boards
flashed from one image produced `3e8f8e75` and `c6c28a10`.

### The entropy caveat, and it is open

The pinned ESP-IDF 5.5.2.260206 says it in its own header
(`components/esp_hw_support/include/esp_random.h`): *"If Wi-Fi or Bluetooth
are enabled, this function returns true random numbers. In other situations,
if true random numbers are required then consult the ESP-IDF Programming
Guide."*

The identity is created at `meshStart()`, which the screen gate means happens
on one of two screens:

- **from the map** -- `BlePositionServer.begin()` has run, BLE is up, the
  guarantee holds;
- **from the LoRa list** -- BLE is very likely down, and the guarantee does
  not hold.

A key is generated **once per device, ever**, so the one path that can be
weak is the one that matters most. MeshCore already ships the right tool for
exactly this job: `RadioNoiseListener`, whose own comment says it is very slow
and to use it *only* for creating a `LocalIdentity`. T-2035.

### Keeping the same key across reboots, and across the tenth map screen

Persistence is the easy half and it is measured: board A reloaded
`key=3e8f8e75` across two reflashes and several reboots, board B kept
`c6c28a10` (2026-10-21).

**The hard half is what the screen gate changed.** `meshStart()` now runs
every time the rider opens the map, so `beginIdentity()` used to run on every
entry -- ten map openings were ten chances for a bad moment on the card. Three
rules close that, all in `PresenceMesh::beginIdentity()`:

1. **Read the card once per boot.** The key lives in RAM afterwards and the
   tenth map screen touches no file at all.
2. **"Cannot read it" and "it is not there" are opposites.** The first version
   treated them as the same thing: a failed open fell straight through to
   generating a new key and overwriting the file. One busy moment on a card
   that is also streaming map tiles -- which is exactly what the map does --
   would have destroyed the rider's identity and handed the group a stranger.
   Existence is now asked separately, and **a file that exists is never
   replaced by this function**. If it exists and will not read, or reads
   short, the mesh stays down and says so; deleting the file is a decision a
   human makes once they know.
3. **Leave no half-written file.** Because of rule 2 a short file is now
   refused forever, so a failed first write deletes what it managed to put
   down rather than bricking the feature on that device.

A fourth rule fell out of writing the test: **a file of the wrong size is
refused in both directions.** A read can only ever return at most 96 bytes,
so an oversize file would have had its first 96 taken as an identity -- a
guess about a format that is not this one. The size is checked before the
read.

The branch table lives in `src/lora/IdentityLoadRule.h` as a pure function and
is proved by `test/lora_identity/`, separately from the card. The I/O around
it is ordinary; the branch table is the part where a mistake is
unrecoverable, so that is the part with a test.

The one case that still runs without a card is the one that cannot be
confused with anything: the file is genuinely absent and the write fails too.
The node works for this session and `identityPersisted()` is false, which the
`MESH ON` reply reports as `stored=0` and the screen shows as an error line.

### The key in RAM, and why it stays there

96 bytes -- 64 private, 32 public -- so this is not a memory question and
sizing it as one would miss what matters. The question worth asking is what
else can read those bytes.

**A coredump cannot.** This firmware writes coredumps to flash and archives
them into the repository (`../../docs/crashes/README.md`), which would be a
fine way to publish a private key. It does not happen: the pinned ESP-IDF
captures task stacks, and `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` -- *"Include
whole .bss and .data sections and heap data into core dump file"* -- is unset
in `sdkconfig.defaults:1868`. The loaded key lives in `.bss`, inside the
static `PresenceMesh`, so it is not in a dump. **If anybody ever turns that
option on, this paragraph stops being true**, which is the reason it names the
line.

What a dump *does* carry is stacks, so the 96-byte buffer that holds the key
while it is read from or written to the card is scrubbed through a volatile
pointer before those functions return. A plain `memset()` on a local that is
dead afterwards is exactly what a compiler may delete.

**The key is deliberately not wiped when the radio stops.** It would mean a
card read on every map entry, on a bus that is streaming tiles at that
moment, and it buys very little: `.bss` is not in a dump, and anyone who can
read this device's RAM can also take the card, where the same bytes sit in
plaintext. Recorded as a decision so the next reader does not redo the
analysis.

### Where it lives, and who else can be you

`/trailink/lora/identity.bin`, 96 bytes: 64 of private key then 32 of public,
in that order, raw. **Plaintext, on a removable SD card.** No passphrase, no
secure element, no flash encryption.

So the threat `CLAUDE.md` asks about has a blunt answer: **anyone who takes
the card out can copy the file and become that rider.** They can sign adverts
under that key and put a false position on every panel in the group, and the
signature will verify, because a signature only ever proved possession.
Nothing in this design detects a copied key. T-2036.

### The name is not an identity and is not unique

Two things are easy to conflate here. The default name is
`EI-%02x%02x` of the first two key bytes -- 65,536 values, so two devices in a
group of a few hundred collide by birthday, and a rider can type whatever they
like over it with nothing checking.

That is not a defect as long as nobody treats it as identity: `LoraPeerTable`
matches on the **whole 32-byte key** and never on the name, so two nodes
called the same thing are still two rows. What it does mean is that **a name
on the panel is a label, not proof**, and no UI may ever imply otherwise.

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

## A busy channel fills the table with strangers

**Measured 2026-10-21 in Bratislava, and it is the first thing real traffic
taught this feature.** Within about an hour of listening on MeshCore's default
channel the sixteen-slot table was **full, all sixteen of them strangers** --
`SK-BA-Pribisova`, `AT-ST-GRAZ-Geidorf`, `Core-PlavStvrtok`, `AT-WL-RS01` and
twelve more, between -92 and -103 dBm, several of them over 100 km away.

The public mesh here is busy, which is good news for the radio and bad news
for the table: **eviction is by age heard, so a group member who has been
quiet for ten minutes is dropped to make room for a stranger who shouted once
from Graz.** For a feature whose whole job is finding the people you came
with, that is backwards.

Nothing is broken today, because nothing feeds the roster yet. But the fix
belongs with that wiring and not after it: a roster member must outrank a
stranger for a slot, whatever the ages say. T-2034.

A second, smaller thing the same screenshot showed: ages run to `4009s ago`,
which nobody reads as an hour. Seconds are right for a bench and wrong for a
list somebody uses.

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

## The bench pass for the gate and the send rules

Everything above the range walk, re-verified on both boards against build
`06eed316` on 2026-10-21 -- because the screen gate and the send rules
replaced the whole transmit path after the first pass, and the first pass no
longer describes this firmware.

**The gate, through real button presses.** `CMD:BUTTON` walked the UI rather
than the console driving the radio directly. On the LoRa list:
`mesh=1 hold=0x1`. One Back: `mesh=0 hold=0x0`. The map holds it the same
way -- board B sat on the map for the whole session at `hold=0x1` and heard
six strangers while it was there, so the gate bounds the listening window
without making the node deaf inside it.

**An advert still crosses.** `ADVERT` on A, and B went from six peers to
seven with `key=3e8f8e75 name=RF-alfa rssi=-12 snr=11 adv=1 loc=-`. The
`loc=-` is the no-fix nameplate path, indoors, doing what it is supposed to
instead of claiming 0,0.

**The hourly cap, observed rather than argued.** Both boards sat on holding
screens with `fix=0` and did not transmit for **36 minutes**
(`sent_ago=2150s`). The build before this one would have sent about twenty
adverts in that time. One advert on entry, then silence until something is
worth saying.

**Three screen entries are not three transmissions.** Into the list, out to
Home, three times: `hold=0x1` each time, and `sent_ago` grew 2073 -> 2098 ->
2123 -> 2150 instead of resetting. B's row for `RF-alfa` read `adv=1` before
and `adv=1` after. The schedule survived three radio stop/starts, which is
the property `meshStart()` deliberately does not reset.

### The group rows, driven through the panel

Walked end to end on 2026-10-22 with `CMD:BUTTON` and `CMD:SCREENSHOT`, build
`51f7c62b`, no hands:

- board B announced itself and board A listed it as `RF-bravo`,
  `-38 dBm  SNR 11  35s ago  x2` -- the coarse age format in place of the old
  raw seconds;
- Select on that row opened the keyboard headed *Short name on the map*,
  pre-filled with **RFB**, derived from `RF-bravo` and editable;
- accepting marked the row `[RFB] RF-bravo`;
- **a reboot of A proved the write**: the heard table was empty and the member
  came back in the third section, value column `Not on LoRa`.

**And the walk found a defect of its own, which is why it was worth doing.** A
stranger was heard while the screen was open, the heard section grew by one
row, and a Select aimed at the member opened the *add* dialog for the node
that had just arrived -- keyboard up, pre-filled with somebody else's
initials. The periodic check created that: before it the list only changed
when the rider pressed something, so a row index was as good as an identity.
The cursor is anchored to the item now (`51f7c62b`).

### The position ingest, end to end

**Done 2026-10-22, and only because the boards sit a metre from a window.**
The receiver on one of them reached a 3D fix in 272 ms from warm ephemeris
(`q=1 used=6 inview=12`), which is what made the last untested link testable
at a desk.

The chain, each step observed:

1. `LORA_STATE` reported `fix=1`, and the advert that followed carried a
   coordinate instead of a bare nameplate.
2. The far board listed the sender with that coordinate in the value column of
   its node list -- the position reached the panel.
3. Adding the sender there wrote the roster entry
   `lora-3e8f8e75a6b20bd0`, the eight-byte prefix form.
4. The next advert went through `teamPosition()`, and the far board's map
   console answered:

   ```
   INFO team_total=1
   INFO team_RFA=lora-3e8f8e75a6b20bd0,on,<lat>,<lon>,<utc>,lora
   ```

   Stored at 1e7, dated, and tagged `lora` -- `TeamFixSource::Lora`, the value
   the group layer reserved for this transport before the radio existed.

**What this does not cover** is the trigger logic around it. The advert above
was forced by hand; the 100 m movement rule and the cap expiring still need a
rider who moves and an hour that passes.

**And a note on what a test like this leaves behind.** Running it put a real
position on the air from a device with a beacon armed, on MeshCore's public
channel where strangers hundreds of kilometres away are listening, and wrote
that position into the other board's black box on its card. Both were turned
off afterwards. Worth saying out loud because the feature works: a position
test is a position disclosure, and the beacon's default-off is not a
formality.

### What the bench cannot answer

Honest list, because the four above are easy to read as "the send rules are
verified" and they are not, as a set:

- **The 100 m movement trigger** and **the fix-appears trigger** both need a
  GNSS fix, and there is none indoors. Untested on hardware.
- **The hourly cap expiring.** 36 minutes of silence were observed; the
  advert at the far end of the hour was not.
- **The `Refuse` path** for an identity file that exists and will not read.
  Host-tested (`test/lora_identity/`), never provoked on a card -- doing so
  means damaging a real identity file, which is the rider's data.
- **Range.** Step 6 below, and nothing on a desk substitutes for it.

## The hardware pass

In this order. Each step can kill the next. Build `638e7b27`, archived in the
parent repo as `docs/firmware-builds/t5s3pro-{a,b}-lora-presence-638e7b27-link-verified.*`
-- two archives, because `-t upload` relinks and the two boards therefore carry
different bytes of the same commit.

1. **One board.** `CMD:LORA ON`, `CMD:LORA MESH ON`. The reply must carry a key
   and `stored=1`. Reboot, `MESH ON` again: the same key, or the identity file
   is not surviving.
   **Done 2026-10-20, in full.** Both boards came up clean:
   `LORA_OK:on chip=SX1261 V2D 2D02. tcxo=1.8V xosc=ok errors=0x0000`, then
   `LORA_OK:mesh=1 name=EI-3e8f key=3e8f8e75 stored=1` on board A and
   `name=EI-c6c2 key=c6c28a10 stored=1` on board B -- two distinct keys, both
   written to the card, the default name derived from the key as intended.
   **And the key survives.** Board A was reset and reflashed twice afterwards
   and came back as `identity loaded from /trailink/lora/identity.bin` with
   the same `key=3e8f8e75` each time -- so the file outlives a power cycle and
   a firmware replacement, which is the part that matters.
2. **The card is still safe.** With the mesh running, open the map and pan.
   `lora-bringup.md`'s SD-bus question is about a radio *listening* while tiles
   stream, and the mesh listens continuously.
   **Done 2026-10-21.** `CMD:GOTO_MAP`, then six pans over 55 s with the mesh
   up and the beacon armed: every redraw reported `tiles ok, 0 missing`, about
   5.2 MB read off the card in total, no `[SD]` error, no CRC failure and no
   corrupt tile. **Stronger than the 2026-08-31 run, and still not a proof.**
   That run could not have failed because the card reads finished before the
   panel bus ran; here tile loading (about 2.5 s in the card per frame) and
   panel refreshes interleave repeatedly with the radio armed, so the window is
   far more likely to have opened -- but nothing instruments whether a refresh
   ever actually overlapped a transfer in flight, so this is evidence and not a
   closed question.
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
   **Done 2026-10-21**, and with no eyes on the panel: `tools/press.py` walked
   Home to the row and `CMD:SCREENSHOT` read the framebuffer back. The header
   showed `LoRa nodes` / `RF-alfa beacon 70s bad 0`, the four control rows
   carried their values, and eight heard nodes sat below them.

   **The grab needs its own decoder on this board.** `tools/screenshot_gate.py`
   reports success and writes a garbled BMP, because it assumes the X4's
   800x480 framebuffer while this panel is 960x540 and returns 64,800 bytes.
   The byte count in `SCREENSHOT_START:` is right; only the geometry the tool
   then imposes is wrong.
5. **The beacon.** `BEACON 70` on one, leave both. Rows must keep arriving and
   the age must keep resetting; `LORA_PEERS` `rejected=` must stay 0.
   **Done 2026-10-20, on build `3b656d2a`, and it failed first on `5f7f7d40`**
   -- that failure is the frozen-clock section above and is the reason this
   step exists separately from step 3. After the fix: board A beaconing at
   70 s, board B went from `adv=1` to `adv=3` across 160 s with `rejected=0`,
   which is the two adverts that interval owes. The persisted switch was
   exercised at the same time: A came up from its settings file after the
   reflash with `mesh=1 beacon=70s due_in=58053ms`, no console command given.

   **Both boards beacon since 2026-10-21**, 70 s each, which is what the range
   walk needs -- one-way tells you nothing about the return path. A hears B at
   -8 dBm and B hears A at -9 dBm on the desk.

   **The cadence was measured rather than assumed**, because the cumulative
   counter looked wrong at a glance: B reported `adv=547` for A, which is
   10.6 h of 70-second adverts and read as far too many. Two readings 140 s
   apart gave **+2**, exactly the interval, and B's own uptime of 11.10 h
   reconciles the total. A counter that spans a run is not a rate; the delta
   is.
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
