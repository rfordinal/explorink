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

## Getting a position without BLE

The simulator has no BLE and no serial input, so neither the phone nor
`CMD:`/map-console commands can deliver a fix. Seed the persisted one instead:

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
draws real tiles on the first frame.

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

Also missing: BLE (the position server, tile push, wallet transfer), serial
input, and wolfSSL (`SecureNet` is in `lib_ignore`).

## Verified 2026-08-23

Ubuntu, SDL2 2.30, OpenSSL 3.0.13. Boot, Home and the map screen all render;
the map loaded 4 z12 tiles, 2874 ways, 18 places in 20 ms and drew Trnava with
place labels, scale bar and compass. Screenshots in the branch's
`qa-artifacts/` (gitignored).

**Open, seen in that run:** the map's zoom `+`/`-` buttons are clipped by the
right edge of the 480 px-wide frame. Unclear whether that is the simulator's
orientation transform or a real style-anchoring bug the device also has — a
device screenshot of the same screen would settle it.

## On a phone

[`simulator-android.md`](./simulator-android.md) tracks building this as an
Android app. The compile probe passed; nothing past the compiler is built.
