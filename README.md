<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/images/trailink-lockup-dark.svg">
    <img src="./docs/images/trailink-lockup.svg" alt="TrailInk — tools for the trail. Ready when you are." width="440">
  </picture>
</p>

# TrailInk

An outdoor trip companion for e-ink readers: offline maps and navigation for
motorcycle rides and hiking. A fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader),
which is where nearly everything that makes this device work came from — see
[Credits](#credits).

> **Heavy development. Not usable yet.**
> The map screen draws real OSM data off the SD card, zoom and marker height
> respond to the hardware buttons, and a phone can drive it all over BLE. The
> renderer now follows most of the map style: per-class road widths and casings,
> hatched or dithered buildings, forest, built-up and water areas. No place names
> and no route yet — those are the next two pieces. Formats and the BLE protocol
> change without notice. If you want a working device today, use
> [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) — this
> fork trades its features away for a different purpose.

<p align="center">
  <img src="./docs/images/map-landuse-full.png" alt="480x800 one-bit map of Kostolište: hatched forest, stippled built-up area, dithered buildings with outlines, a dark pond, thin roads and the position marker" width="300">
  <img src="./docs/images/map-landuse-calm.png" alt="The same map with the built-up wash dropped and the forest as a light stipple, so the village body comes from the buildings themselves" width="300">
</p>

<p align="center">
  <sub><b>What the renderer draws.</b> Kostolište at 3 m/px, both frames 480x800
  one-bit, straight out of this firmware's own <code>MapRenderer</code> — the
  same source files the device compiles, built for a laptop so a style change
  takes seconds instead of a flash (<code>pio run -t map-preview</code>). Areas
  are dithered or hatched, never filled solid: a black building swallows the
  roads around it on a one-bit panel. <b>Left</b> has every layer on; <b>right</b>
  drops the built-up wash and lightens the forest, so the village reads through
  its buildings. Not photographed off the panel — these are the renderer's
  pixels, and the panel has not been checked against them yet.</sub>
</p>

<p align="center">
  <img src="./docs/images/map-preview.png" alt="480x800 one-bit map: a thick route through Kostolište with junction dots, a place label and the position puck" width="300">
  <img src="./docs/images/device-map-z13.png" alt="480x800 one-bit framebuffer dumped from the device: the road network around Malacky at 3 metres per pixel, every road the same width, a debug readout in the top left corner" width="300">
</p>

<p align="center">
  <sub><b>Left: where it is going.</b> The map tooling's own sketch of the render
  spec, at the device's exact resolution — place labels, a thick route with
  junction dots and the position puck, none of which the firmware draws yet.
  <b>Right: a real framebuffer off the hardware</b>, dumped with the POWER+DOWN
  screenshot combo, at 3 m/px around Malacky. It predates the style work above:
  every road is one width there because nothing of the style reached the renderer
  when it was taken.</sub>
</p>

## Why a separate fork

A phone is a bad navigation display on a motorbike. The screen washes out in
direct sun, it overheats in a handlebar mount, and the battery does not last a
day of riding. E-ink has the opposite properties: readable in full sunlight,
free to hold an image on screen, and tens of hours of runtime because power is
only spent when the picture changes.

What it gives up is refresh rate, colour and grey. That rules out the moving map
a phone gives you and forces a different design: a still image showing where you
are, where the route goes and what is around you, redrawn rarely and readable at
a glance at speed. No rerouting, no voice, no live traffic — the route is
planned before you leave.

That is a different product from an e-reader rather than an extra feature on
one, and it falls outside CrossPoint's [scope](./SCOPE.md), so it is a fork and
not a pull request.

## Status

| | |
|---|---|
| Map activity and screen | exists |
| BLE position receiver (phone sends GPS) | exists |
| Loading a real map from the SD card | exists — this is the current state |
| Buttons on the map screen (zoom, marker height) | exists, per mode, saved across power cycles |
| Command console (serial and BLE), zoom/mode filter | exists — same grammar over USB and BLE |
| Renderer following the map style spec | mostly — per-class road widths and casings, hidden classes, buildings, forest, built-up and water areas with dither tones or hatch, place dots, marker anchor. Labels, route and junction dots: **not implemented** |
| Companion phone app | **not started** |

`data/mapstyle.json` holds the styling the renderer is meant to follow — road
widths and casings, junction dots, place labels, the position marker. It is
written by the mapbuilder webapp rather than edited by hand. It is a
build-time input, not a runtime one: `scripts/gen_mode_masks.py` compiles its
`modes` block into the ride/hike/cycle class masks and `scripts/gen_mapstyle.py`
compiles the drawing numbers into a `MapStyle` constant, both as `pre:` build
steps; the device reads no style file at runtime. See
[`docs/map-style.md`](./docs/map-style.md).

The same renderer builds as a host binary — `pio run -t map-preview` renders real
tiles to a PPM with no ESP32 toolchain and no flashing, which is how a style
change gets checked in seconds.

The map tooling itself — OpenStreetMap fetch, projection, routing along real
roads, and a browser tuner with a pixel-exact 480x800 preview — lives in a
companion workspace that is not published, together with the render
specification this firmware is meant to implement.

## Inherited and new

Everything that makes an ESP32-C3 e-ink device work is CrossPoint's: the display
driver, the partial refresh path, the activity system, settings, i18n, the web
server and file manager, the build and test setup. This fork adds a map
activity, a BLE position server and the map style file, and over time strips out
the e-reader stack — EPUB, OPDS, dictionary, the font library — that a
navigation device does not need.

Upstream fixes therefore still matter here. The intent is to keep tracking
CrossPoint rather than drift away from it.

## Hardware

ESP32-C3 based Xteink [X4](https://www.xteink.com/products/xteink-x4) and
[X3](https://www.xteink.com/products/xteink-x3) — the same devices CrossPoint
supports. Development and testing happen on an X4.

## Development quick start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/rfordinal/TrailInk
cd TrailInk

# if cloned without --recursive:
git submodule update --init --recursive
```

### Nix/NixOS

Enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your
NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev
rules.

### Build / flash / monitor

```bash
pio run --target upload
pio device monitor
```

### Pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Native map preview

The map renderer and tile reader have no hardware dependency, so they build
and run on a laptop without the ESP toolchain. `map_preview` reads real
`.tib` tiles (mapbuilder/build_tiles.py output) around a coordinate:

```bash
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
cmake --build build/test --target map_preview
./build/test/map_preview/map_preview \
  --tiles test/map_tile_reader/fixtures/tiny-sd \
  --lat 48.531158410819025 --lon 17.072751469276742 \
  --heading 0 --zoom 0 --out out.ppm
```

It writes a 480x800 one-bit image. Note that `PpmCanvas` rasterizes separately
from the real `GfxRenderer`, so the result is layout-accurate, not
pixel-accurate against the device.

## Documentation

The docs under [`docs/`](./docs) are inherited and still accurate for the parts
this fork has not touched — firmware internals, the activity manager, file
formats, i18n and the contributing guide all came from CrossPoint.

## Credits

**This project exists because of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)** —
open-source e-reader firmware for Xteink devices, community-built and fully
hackable, MIT licensed, © 2025 Dave Allie and contributors. The e-ink driver,
refresh handling, UI framework and toolchain in this repository are their work,
not ours. TrailInk replaces only what it must for a different purpose.

If CrossPoint is useful to you, support the people who maintain it:
[fund contributors](https://app.royalty.dev/crosspoint-reader/crosspoint-reader),
or buy an X3/X4 Developer Edition through
[crosspointreader.com](https://crosspointreader.com), which sends them a share
of each sale.

Built on [freeink-sdk](https://github.com/Free-Ink/freeink-sdk) by FreeInk, MIT
licensed, © 2026 FreeInk.

CrossPoint in turn credits
[diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader) as
its inspiration.

Map data © [OpenStreetMap](https://www.openstreetmap.org/copyright)
contributors, ODbL.

## Licence

MIT, inherited from CrossPoint — see [LICENSE](./LICENSE).

---

TrailInk is **not affiliated with Xteink, CrossPoint, or any device
manufacturer**.
