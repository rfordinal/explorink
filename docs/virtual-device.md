# Host-side device simulation

The X4 was lost 2026-08-22. Map and UI work needs a device. This doc says what we
run instead, and what no host build can answer.

**Wokwi is dropped, 2026-08-23.** The plan below it was to run the real firmware
binary on an emulated ESP32-C3 as a tenth `BoardProfile` (`FREEINK_DEVICE_VX4`).
The reason it was worth paying for was that it looked like the only way to get
the whole firmware UI on a screen without a device. It is not: upstream's own
simulator does that natively, MIT and maintained (see "Prior art"). What is left
of Wokwi's exclusive ground is narrow, and against it stand a closed core, a beta
chips API, a paid plan and an SSD1677 model we would write ourselves.

The Wokwi and QEMU research below is **kept deliberately, not as a plan.** Every
finding in it cost a fetch or a measurement and would otherwise be re-derived by
whoever asks this question next.

## What we run

**`map_window`, built 2026-08-22.** The real map code in an SDL2 window on the
laptop: a replayed ride or a held position, the real `MapFollow` decisions and
the real `MapRenderer`, a menu for the rungs, modes and layers, and the map
module's own log. It joins two host tools that already existed. Details in
[`map-follow.md`](map-follow.md), "`map_window`".

**Upstream's simulator, working since 2026-08-23.** Runs the *whole* firmware UI
-- menus, every activity, settings, the web server. We run a fork,
`rfordinal/explorink-simulator`, because the simulator replaces `lib/hal/` rather
than extending it and ours has diverged. `pio run -e simulator -t run_simulator`.
**[`simulator.md`](simulator.md) is the doc**; "Prior art" below is only how it
was found.

| | `map_window` | upstream simulator | needs a device |
|---|---|---|---|
| loop speed | native, 64x the ride clock | native | real time |
| UI pixels | faithful, map screen only | faithful, whole UI | the panel itself |
| refresh counts, dirty rects | **yes** -- firmware decisions | presumably | yes |
| activities outside the map | no -- links map code, not the UI stack | **yes**, running | yes |
| heap ceiling, stack limits | no -- x86 | no -- x86 | **yes** |
| real RISC-V execution | no | no | **yes** |
| SSD1677 command stream | no | no | **yes** |
| waveform timing, ghosting | no | no | **yes** |

**Read the last column as the cost of the lost device, not as a gap some tool
fills.** Dropping Wokwi means those four rows have no substitute at all until
replacement hardware arrives. That is the honest trade: Wokwi could have reached
three of them (waveform timing only as a model we wrote), for money and for
weeks of work on a platform we cannot fix.

## `map_window` was cheaper than any estimate in this doc

`map_window` cost one refactor and one new tool because both halves were already
in the tree and nobody had joined them live:

- `test/map_replay/` had the ride, the packets and the real
  `MapFollow::decide()`, gated against three real X4 rides
  (`hardware-baseline.txt`).
- `test/map_preview/` had the real `MapRenderer`, `MapProjection`,
  `MapTileReader` and the firmware's own font tables, drawing into a 480x800
  canvas.

`tools/render_ride_video.py` (parent repo) had already joined them for **video**,
but with a scope cut: "only ReAnchor frames are rendered", so partial refreshes
-- the thing worth tuning -- were invisible.

What layer 1 measures, and at what confidence:

| quantity | confidence |
|---|---|
| refresh counts (full / partial / skip) | **measured** -- real `MapFollow::decide()`, agrees with `map_replay` exactly |
| dirty rectangle | **measured** -- the real patch box, `MapMarkerMetrics.h` |
| the decision log line | **the firmware's own bytes** -- `MapFollow::formatDecisionLog()`, the same call `MapActivity` hands to `LOG_DBG` |
| panel time | **measured constant** -- 500 ms per refresh, X4 2026-08-05 |
| device render time | **not modelled.** Shows the laptop's own, labelled |

The log line row is the one that pushed a change back into the firmware rather
than only reading it. Getting the map module's real messages into the window
meant `decide()` reporting which check fired instead of the reason being guessed
afterwards, and the line text living in one place instead of two. Both are in
`map-follow.md`, "`decide()` says why now" and "One copy of the log line" -- and
the second one found a log line that had been printing the wrong number.

That last row is the honest limit of a host build, and it is why the device
still matters.

**It paid for itself on the first run.** Replaying one ride at ride mode's own
default rung instead of the rung the hardware runs used showed 30 re-anchors and
264 marker moves against 14 and 113 -- 2.3x the panel cost, on the setting the
device actually ships with. `map-follow.md`, "The default ride rung costs more
than twice what was measured".

## Prior art: upstream ships a simulator, and it is better than this doc assumed

**Found 2026-08-23, after `map_window` was already built and merged.**
`crosspoint-reader/crosspoint-simulator` is upstream CrossPoint's own desktop
simulator. **MIT licensed, actively maintained** (created 2026-03-17, pushed
2026-08-20), 51 stars, 31 forks.

It is not a separate program. It is a PlatformIO library: the firmware adds it as
a `lib_dep`, declares an `[env:simulator]`, and `lib_ignore = hal` drops the
firmware's whole `lib/hal/` so the simulator supplies `HalDisplay`,
`HalStorage`, `HalGPIO` and the rest in its place. Then the **entire firmware**
compiles natively and renders into an SDL2 window -- menus, every activity,
settings, the web server and the WebDAV routes, not just one screen.

It already carries device profiles for X4, X3, X4 Pro, Seeed Sticky and M5Stack
PaperMono, and controller variants for SSD1677, UC8179 and UC8279. Needs SDL2
plus `libssl-dev`; macOS and Linux/WSL only, no native Windows.

`FORKING.md` answers our exact case: a fork whose HAL has diverged forks the
simulator too and repoints `lib_deps` at it. That is called "the supported path,
not a workaround", with a split of what belongs upstream (platform emulation
gaps, rendering behaviour) against what stays in a fork (HAL signatures, device
profiles for hardware upstream does not target).

**What this corrects in this doc.** The layer table above says layer 1 cannot
reach activities outside the map, and that only layer 2 can. That was true of
`map_window`, which links the map sources rather than the UI stack -- it is not
true of the approach. A host build *can* run the whole firmware UI, and upstream
has been doing it since March. Layer 2's remaining exclusive ground is narrower
than this doc claimed: the heap ceiling, real RISC-V execution, the SSD1677
command stream and waveform timing.

**Done, the same day this was written.** The HAL divergence was real -- the fork
`rfordinal/explorink-simulator` exists for exactly that, and its `EXPLORINK.md`
lists every difference and which half belongs upstream. The firmware builds and
runs under it: [`simulator.md`](simulator.md). This section is kept as the record
of how a maintained MIT simulator went unnoticed while a narrower tool was built
against it; the claims in it were **read** off the repo and are now superseded by
having run the thing.

`jonmooreai/Crosspoint-Emulator`, which an earlier version of this section
presented as the prior art, is one of the 31 forks. Its "no licence, one author,
untouched since 2026-02-11" is true of that fork and was wrong to read as the
state of the art. `uxjulia/CrossInk` is another fork.

# The dropped Wokwi plan, kept for its findings

Everything below this line is the VX4 plan as it stood on 2026-08-22, before
upstream's simulator was found. **It is not a plan any more.** Read it as a
record, and read the section titles as past tense.

Four findings in it are the reason it is not deleted, because each cost a fetch
or a measurement:

- **QEMU's ESP32-C3 cannot drive this device.** No GP SPI, no GPIO matrix/IOMUX,
  no SD/MMC -- which is the panel, the card and the buttons. "Why Wokwi and not
  QEMU".
- **Nobody outside Espressif can emulate BLE**, and the reason is a closed blob
  against undocumented radio registers. "No BLE, and that is fine".
- **Wokwi references every virtual ADC to 5 V regardless of MCU**, so a button
  model has to target the raw count rather than reproduce the ladder. "Buttons
  target the raw ADC count".
- **Wokwi's SPI API has no CS**, so any model on a shared bus must watch the pin
  itself or eat the other device's traffic. "The panel model must watch its own
  CS".

The pricing, the spike order and the platform gate are historical. If this
question is ever reopened, the licensing numbers were read on 2026-08-22 and
should be re-read.

## The invariant

**VX4 is not a new hardware target.**

VX4 is electrically and geometrically identical to the X4 everywhere that
property reaches firmware behaviour:

| property | VX4 |
|---|---|
| MCU family | ESP32-C3, same as X4 |
| screen geometry | 800x480, same as X4 (`BoardConfig.h:685`) |
| display controller | SSD1677, same as X4 |
| display pins | sclk 8, mosi 10, cs 21, dc 4, rst 5, busy 6 (`BoardConfig.h:691`) |
| display SPI clock | 20 MHz (`FREEINK_X4_DISPLAY_SPI_HZ`, `BoardConfig.h:683`) |
| SD pins | miso 7, cs 12, bus shared with panel (`BoardConfig.h:693`) |
| input | ADC ladder on GPIO1 / GPIO2 (`InputManager.h:67-68`) |
| flash | 16 MB, same partition table |
| RAM | whatever the emulated C3 has. Not a number we pick. |

Only the **simulated surroundings** differ:

| real X4 | VX4 |
|---|---|
| BLE radio + phone | scripted packet source (`FREEINK_CAP_BLE_FAKE`) |
| GPS fix over BLE | scripted, from a ride log |
| battery / fuel gauge | absent or modelled |
| e-ink panel | SSD1677 chip model |
| physical buttons | ADC source model |
| microSD card | Wokwi microSD part |

Break this table and the pixel-fidelity claim dies with it. A VX4 screen that is
not the X4 screen debugs a device nobody owns.

## Why Wokwi and not QEMU

Espressif's QEMU fork does support ESP32-C3, and it is GPL so we could extend it.
It still loses, on its own support table (`espressif/esp-toolchain-docs`,
`qemu/README.md`, fetched 2026-08-22):

- **GP SPI — not supported.** The panel and the SD card both live on SPI
  (`BoardConfig.h:691`, `:693`).
- **GPIO matrix / IOMUX — not supported.**
- **SD/MMC — not supported.**
- ADC is not in the table at all.

So QEMU today cannot drive the panel, the card or the buttons. Adding them means
writing the C3's own SPI2 and IOMUX peripherals first. That work is honest — those
registers are in the C3 TRM, unlike the radio — but it is weeks before the first
pixel.

Wokwi's ESP32-C3 supports **SPI, ADC, GPIO and timers** (docs.wokwi.com,
`guides/esp32`), plus a microSD part over SPI and a custom-chip API with a
framebuffer. That is the whole X4 minus the radio.

**Wokwi's cost: the core is closed.** A gap in their C3 emulation cannot be fixed
by us, only reported. QEMU stays the fallback: the SSD1677 state machine we write
is portable, only its shell changes.

## No BLE, and that is fine

Neither platform emulates BLE. Wokwi says Bluetooth is "not implemented" for
every ESP32 variant; Espressif's QEMU marks Wi-Fi and Bluetooth unsupported.

Nobody can fix that from outside. The BLE controller is a closed blob shipped
with the pinned ESP-IDF (`5.5.2.260206`):
`components/bt/controller/lib_esp32c3_family/esp32c3/libbtdm_app.a`, plus
`components/esp_phy/lib/esp32c3/libphy.a` and `libbtbb.a`. The C3 TRM does not
document the radio registers that blob writes. Emulating BLE means reverse
engineering a radio MAC from a binary. That is why Espressif's own QEMU has no
BT.

We do not need it. The firmware never touches the controller — it goes through
NimBLE, and BLE is **already** a build capability with stub bodies written:
`lib/BlePositionServer/src/BlePositionServer.cpp:1020`, `#else //
!FREEINK_CAP_BLE_PERIPHERAL -- stub bodies, no BLE code linked.` A no-BLE build
links today.

`FREEINK_CAP_BLE_FAKE` replaces those stubs with a scripted packet source. The
shape exists: `test/map_replay/RideLog.cpp` already replays recorded packets, and
`toE7()` converts into the same int32 1e7 fixed point a BLE packet carries
(`MapCommandParser.h:47-50`).

## Two mechanics that decide whether the spike works

### Buttons target the raw ADC count, not the real voltage

Wokwi: *"the reference voltage for all the virtual ADCs is 5 volts (regardless of
the MCU)"* and *"Wokwi is a digital simulator with basic analog support, so there
is currently very limited analog simulation"* (`chips-api/analog`).

The X4 reads the ladder with `analogSetAttenuation(ADC_11db)`
(`InputManager.cpp:77`), so full scale is ~3.1 V at 4095, and the thresholds are
raw counts: `ADC_RANGES_1[] = {ADC_NO_BUTTON, 3100, 2090, 750, ...}`,
`ADC_RANGES_2[] = {ADC_NO_BUTTON, 1120, INT32_MIN}` (`InputManager.cpp:29-31`).

Drive the real ladder voltage into a 5 V-referenced virtual ADC and every
threshold misses. So the chip model does **not** reproduce the resistor network.
It reproduces the network's result, and calibrates the DAC voltage to the count:

```
v_wokwi = raw_target / 4095.0 * 5.0
```

Threshold 3100 needs ~3.79 V, not the ladder's ~2.35 V. Set with
`pin_dac_write()`.

Firmware sees a normal `analogRead` and never learns the input is virtual, so
this stays inside the platform gate below.

### The panel model must watch its own CS

Wokwi's SPI device API: *"The API does not support a CS/SS pin: it is up to the
user to select/deselect the SPI interface by calling `spi_start()` and
`spi_stop()`"* (`chips-api/spi`). It is slave mode; `pin_watch` on the CS pin is
the documented pattern.

The X4 shares one SPI bus between panel (cs 21) and SD card (cs 12). A model that
consumes bytes while its CS is high eats the card's traffic and both break. CS
gating is not a refinement here, it is correctness.

At 20 MHz, use a multi-kilobyte buffer — Wokwi recommends that for display-sized
DMA transfers.

## The panel model is a diagnostic instrument

Not just SPI to bitmap. It records what the firmware costs the panel:

| quantity | confidence |
|---|---|
| `fullRefreshCount` | **measured** — derived from the real SPI stream |
| `partialRefreshCount` | **measured** |
| refresh type | **measured** |
| dirty rectangle (x, y, w, h) | **measured** |
| `spiBytesWritten` | **measured** |
| `pixelsChanged`, black->white, white->black | **measured** |
| `busyDuration` | **modelled** |
| refresh duration | **modelled** |
| ghosting | **modelled**, and not before the rest works |
| energy estimate | **modelled** |

**The modelled rows cannot be calibrated right now.** They will only ever say
what we wrote into the model from `Ssd1677Luts.h`. The X4 that could check them
is gone. When a replacement arrives, calibrate and re-mark them; until then a
number like "312 ms partial refresh" is our own assumption quoted back at us, not
a measurement.

The counts are the point anyway. "This menu costs 6 full refreshes" is a finding
the real device makes expensive to see.

## The platform gate

The spike passes **only if no production runtime logic changed to accommodate
Wokwi.**

Allowed:

- `FREEINK_DEVICE_VX4` in `BoardConfig.h`, alongside the nine existing profiles
- a `[env:vx4]` build environment
- `diagram.json`, the SSD1677 chip, the button/ADC chip, the virtual SD
- `FREEINK_CAP_BLE_FAKE`

Forbidden, anywhere in the renderer, `EpdBus`, the input manager, the SD layer,
navigation, map code or UI:

```cpp
#ifdef WOKWI
if (BoardConfig::ACTIVE.board == Board::VX4) { /* skip real behaviour */ }
```

VX4 may have different hardware behind the HAL. The application must not behave
differently because of it. A spike that passes only with an `#ifdef` has not
validated the platform, it has bypassed it.

### Why a separate device flag, not X4-plus-a-capability

`[env:default]` compiles **X4 and X3 into one binary**
(`platformio.ini:223-224`) and picks at runtime: `XteinkDetect` probes the
X3-only I2C parts on SDA=20 / SCL=0 — BQ27220 (0x55), DS3231 (0x68), QMI8658
(0x6B/0x6A) — two passes, >= 2 hits each means X3, anything else is treated as
X4 (`XteinkDetect.h`).

With its own device flag the probe goes inert by design:

> "In builds without an Xteink profile (neither FREEINK_DEVICE_X4 nor
> FREEINK_DEVICE_X3) both functions compile to no-ops returning false and never
> touch a pin." (`XteinkDetect.h`)

Reuse `-DFREEINK_DEVICE_X4` instead and the probe runs against a bus Wokwi may
answer differently, so the verdict — `X4Confirmed` vs `Inconclusive`, which
differ in whether the answer is persisted — becomes platform-dependent. The
separate flag is a requirement, not tidiness.

## Spike order

Stop at 7 and judge before writing the real panel model.

1. `FREEINK_DEVICE_VX4` profile — exact X4 clone, X3/X4 detection inert
2. minimal `diagram.json`, ESP32-C3, `flashSize` 16
3. boot the existing firmware binary, confirm the USB CDC console
4. dummy SSD1677 — real `EpdBus.cpp` over real SPI, print the command/data stream
5. button ADC source — press maps to the expected raw count in `MappedInputManager`
6. Wokwi microSD — `SdFat` reads one real `.tib`
7. **platform gate** — zero Wokwi-specific runtime workarounds
8. SSD1677 command and state implementation
9. 1bpp framebuffer, 480x800 visualisation
10. diagnostics: refresh counts, dirty regions, SPI traffic, pixel transitions
11. modelled BUSY / LUT timing
12. `FREEINK_CAP_BLE_FAKE` + `RideLog` playback
13. automated navigation scenarios
14. ghosting and power modelling, last

BLE playback sits at 12 deliberately. Proving a virtual X4 needs boot, input, SD
and draw — not a position feed.

## Sizing

- Wokwi microSD: SPI, FAT16, up to 8 MB (`parts/wokwi-microsd-card`).
- Tiles measured in `mapbuilder/cdn` on 2026-08-22: 1291 `.tib` files, median
  22 kB, max 721 kB. A build is 21 tiles across three LOD levels, so roughly
  0.5 MB per area. `data/` is 24 kB.
- So ~10 areas fit on the virtual card. Enough for UI work, not a mirror.
- Flash: `partitions.csv` ends at `0xFF0000 + 0x10000` = exactly 16 MB, so
  `flashSize` must accept 16. **Reported as a valid value ("2", "4", "8", "16",
  "32"); our own fetch of the ESP32 guide did not reproduce that list.** Step 2
  settles it in one run. Fallback if it caps lower: a VX4-only partition table
  with a single app slot — the binary is far smaller than the 6.5 MB reserved.

## Licensing: the free tier is not enough

Read from wokwi.com/pricing and the VS Code docs, 2026-08-22. **An earlier claim
in this project that Wokwi is "free for open source" was wrong and is
withdrawn.**

| plan | price | what it gives us |
|---|---|---|
| Community | €0 | "For open source projects". Unlimited simulations, **public projects only**, no custom libraries, **no VS Code** |
| Hobby | €5.6/mo | unlisted projects, custom libraries. Still no VS Code |
| Hobby+ | €8.1/mo | **Wokwi for VS Code** |
| Pro | €20/seat/mo | VS Code **offline** plug-in, 2000 CI minutes, **commercial use permitted** |

### Getting our own binary in does not need a paid plan

We do not use their build service and do not need VS Code. A browser project takes
a locally built binary: in the editor, F1 -> "Upload Firmware and Start
Simulation…", then any `.bin`, `.elf` or `.uf2`. Wokwi infers the flash offset,
defaulting to `0x10000` — exactly our `app0` (`partitions.csv`). And "you can
specify a custom partition table by adding a `partitions.csv` file to your
project", so the 16 MB table travels with the project.

So the loop is: PlatformIO builds locally, we upload the `.bin`.

**Sourcing, weaker than the rest of this section.** The upload flow came out of
web searches summarising the Wokwi docs, not a fetch of the page documenting it
-- `docs.wokwi.com/guides/esp32` was fetched twice and surfaced it neither time.
Two searches also gave two different menu labels ("Upload Firmware and Start
Simulation…" and "Load HEX File and Start Simulation…"), so the capability is
confirmed twice and the exact wording is not. It is the claim the whole "VS Code
is irrelevant" conclusion rests on. One minute in a browser project settles it.

Unrelated, and worth separating because it gets conflated: Meshtastic's
browser-based firmware tool is a **Web Flasher** -- Web Serial to a real device
over USB -- not a Wokwi simulation. Browser flashing and browser simulating are
different mechanisms.

`wokwi-cli` is the CI path, not the dev loop. It reads a local `wokwi.toml`
(`version`, `firmware`, `elf`) plus `diagram.json`, but **runs against Wokwi's
servers** and needs `WOKWI_CLI_TOKEN` from the CI dashboard. Metered.

### Where the simulation actually executes — `[open]`

This matters: if the browser engine is server-side, the daily UI loop depends on
Wokwi's availability and on network latency per button press. If it is
client-side, we are independent.

What is documented, and about what:

- **VS Code extension.** The engine is "cached on your machine after being
  downloaded during an online session", and Pro can simulate with no internet at
  all — but "the simulation does not run fully locally". These lines are from
  `vscode/offline-mode` and say nothing about the browser.
- **wokwi-cli.** Explicitly remote: it "runs simulations against Wokwi's remote
  servers".
- **Browser.** No page found that says either way.

Weak indirect hint: Community offers "unlimited simulations" while CI minutes are
metered, which fits browser = our CPU, CI = theirs. Not evidence.

Measured 2026-08-22, headless Chrome with a full net log on a public Wokwi ESP32
project page: 1211 requests, all to `wokwi.com`, no separate simulation host, no
WebSocket, and the only WASM asset is `assets/libastyle.wasm` — astyle, the code
formatter, not a simulator. **Inconclusive: the run never pressed Play, so no
engine was ever fetched.** A negative from a page that did not start the thing is
not a negative.

Two ways to settle it:

- Open a project, press Play, then cut the network. Still running means
  client-side.
- Drive Chrome over the DevTools Protocol to click Play, re-capture the net log,
  and look for a large `.wasm` fetch or a WebSocket.

An earlier draft of this doc asserted the browser runs client-side by carrying the
VS Code offline wording over to it. That inference had no support and is
withdrawn.

### What the free tier does block

- **Step 6.** The microSD part docs: "Paying users can upload custom binary files
  through an SD Card tab." No real `.tib` on the virtual card without paying. Do
  not work around it by baking a tile into flash — that is a firmware change, so
  it fails the platform gate.
- **Everything is public on Community.** The binary and the diagram are fine — the
  firmware repo is public already. **A ride log is not.** It is the maintainer's
  GPS trace, same class as a device screenshot showing home. Real traces never go
  into a public Wokwi project. Synthesise ride logs for anything public.
- **Commercial use.** See below.

**Commercial use is forbidden on Community, Hobby and Hobby+.** Only Pro permits
it. Whether ExplorInk counts as commercial (`docs/business.md` exists in the
parent repo, so probably) is the maintainer's call, and it decides €8.1 vs €20
per month.

Offline is not a free escape either: "the simulation does not run fully locally"
— the engine is downloaded and cached, and offline mode needs Pro. The core is
closed source, so there is no self-hosted option at any price.

**This does not change the platform decision.** The alternative to paying is not
a free emulator, it is weeks of writing the C3's SPI2 and IOMUX into QEMU before
the first pixel. €8.1/mo is cheap against that, and trivial against a
replacement device.

**Suggested sequencing:** run spike steps 1-5 on Community in the browser — public
project, no real tile, no real trace. That proves boot, console, SPI and buttons.
Pay only if it passes, then do step 6.

## Open items

- CI minutes contradict across pages: `wokwi-ci/getting-started` says "Free
  users: 50 minutes", the pricing page lists no CI minutes for Community.
  `[open]`
- **Custom Chips API is beta**: "The Chips API is currently in beta." Expect
  churn.
- **Deep sleep** does not appear in Wokwi's C3 feature table. Irrelevant to UI
  work, fatal to power work. `[open]`
- Whether a 480x800 RGBA framebuffer (1.5 MB, no documented size cap) performs
  acceptably. `[open]`

## What no simulation tests

Ghosting on real pigment, power draw, deep sleep, ADC tolerances, SD timing, BLE
against a real phone, and whether a mount holds at speed. Wokwi would not have
reached these either; the table at the top of this doc is the shorter list of
what it *would* have added over a host build.

**The merge rule is unchanged.** Nothing reaches `develop` or `master` on
simulated evidence alone. A simulator moves where bugs are found earlier; it does
not move where they are confirmed.

## Automation, later

`wokwi-cli` supports `--scenario`, `--expect-text`, `--fail-text`,
`--screenshot-part`, `--screenshot-time`, `--timeout` and `--interactive`
(`wokwi-ci/cli-usage`). That is enough for a ride-scenario suite: inject a route,
replay a GPS sequence, press a button, screenshot the panel, compare. Blocked on
the licensing item above, not on the tooling.
