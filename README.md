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
> The map screen draws mock data. Nothing loads a real map from storage, and the
> renderer does not yet follow the map style the tooling produces. Formats and
> the BLE protocol change without notice. If you want a working device today,
> use [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) —
> this fork trades its features away for a different purpose.

<p align="center">
  <img src="./docs/images/map-preview.png" alt="480x800 one-bit map: a thick route through Kostolište with junction dots, a place label and the position puck" width="300">
</p>

<p align="center">
  <sub><b>What it should look like.</b> Produced by the map tooling on a laptop at
  the device's exact 480x800 one-bit resolution — <b>not</b> a photo of the device,
  which does not draw this yet.</sub>
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
| Map drawn from mock data | yes — this is the current state |
| Loading a real map from storage | **not implemented** |
| Renderer following the map style spec | **not implemented** |
| Companion phone app | **not started** |

`data/mapstyle.json` holds the styling the renderer is meant to follow — road
widths and casings, junction dots, place labels, the position marker. It is
written by the mapbuilder webapp rather than edited by hand. It is a
build-time input, not a runtime one: `scripts/gen_mode_masks.py` compiles its
`modes` block into the ride/hike/cycle class masks as a `pre:` build step; the
device reads no style file at runtime.

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

The map renderer has no hardware dependency, so it builds and runs on a laptop
without the ESP toolchain:

```bash
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
cmake --build build/test --target map_preview
./build/test/map_preview/map_preview out.ppm
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
