# Style is per travel mode and per zoom rung

**Built 2026-08-25. Unverified on hardware** — there is no device to test on
(the X4 was lost 2026-08-22). Host tests pass, the ESP32 build is clean, and the
host renderer draws it; nothing here has been looked at on a panel.

## The problem

Every length in `data/mapstyle.json` is a device pixel. A pixel is not a fixed
amount of ground: it is 1 m at the closest zoom rung and 45 m at the coarsest.
So one width per road class means an 11 px motorway that covers 11 m of ground
at rung 0 and **495 m at rung 6**.

That was measured, not guessed: roads were two thirds of all ink at both
overview rungs, 9.1 % of 13.9 % at rung 3 and 9.4 % of 14.4 % at rung 4
(`../../docs/PROGRESS.md`, 2026-08-08). The note there names the fix and says it
was not started: "a per-rung width multiplier, or a width set per rung".

A multiplier is the wrong shape. A motorway must stay visible as it thins, a
residential street should vanish rather than thin, and a railway's tick rhythm
is not a width at all. Three different decisions per class is a table.

The same argument applies to travel mode, where the filter already existed but
the *style* did not: a hiker's footpath and a rider's motorway are not the same
mark, and a mode that only decides "drawn or not" cannot say so.

## The grammar

Any object in `data/mapstyle.json` may carry a `when` list:

```json
{
  "match": { "class": ["primary"] },
  "width": 9, "casing_px": 2, "major": true,
  "when": [
    { "steps": [2],          "width": 7, "casing_px": 2 },
    { "steps": [3, 4],       "width": 5, "casing_px": 1 },
    { "steps": [5],          "width": 4, "casing_px": 1 },
    { "steps": [6],          "width": 3, "casing_px": 0 },
    { "modes": ["hike"], "steps": [6], "hidden": true }
  ]
}
```

- **`modes` omitted means every mode. `steps` omitted means every rung.** An
  entry with neither always applies, which is how a value is overridden without
  repeating the outer field.
- **Entries apply in order, and the last matching one wins.** That is the whole
  precedence rule. There is no specificity scoring to reason about, and the file
  reads top to bottom.
- A patch is a shallow merge; a dict value deep-merges. `match` and a nested
  `when` cannot be patched.
- **`when` is banned under `device`.** The marker anchor becomes a `constexpr`
  the whole viewport arithmetic is built on (`MapViewport.h`, `kAnchorScreenX`),
  so a per-rung anchor would be a per-rung viewport. That is a different
  feature, and the resolver, the generator and a host test each refuse it
  separately.

Everything with a `when` today: `layers.roads.rules` (width and casing per
rung), `layers.buildings` (`enabled`, rung 0 only), the built-up
`layers.landuse` rule (`hidden` at rung 0), and `layers.places`
(`max_labels`, 3 names at rung 0 up to 14 at rung 6).

## The device never sees a rule

This is the part worth being clear about, because "a config with nested
conditions" usually means an interpreter somewhere.

`scripts/mapstyle_variants.py` resolves the rules **at build time**.
`scripts/gen_mapstyle.py` walks the 3 modes x 7 rungs, resolves each, and emits
one `MapStyle` per *distinct* result plus a `[3][7]` index of `uint8_t`. The
firmware does one array lookup:

```cpp
const MapStyle& style = mapStyleFor(mode_, view.zoomStep);   // MapStyleTable.h
```

No parsing, no branching, no JSON, no card read. Identical variants are
deduplicated, so a style file with no `when` in it compiles to exactly one
struct and a 21-byte index.

Measured on this branch's style, which has 7 distinct variants (one per rung;
no mode differs yet): **`sizeof(MapStyle)` is 238 bytes, and the variants plus
the index are 1,687 bytes of flash.** The whole ESP32 build is 3,944,467 bytes,
60.2 % of the partition.

## The filter is per rung too, and it agrees with the style by construction

`modes.<mode>.classes` is the mode's **vocabulary**: which classes it cares
about at all. What `MapTileSource` actually filters with is that intersected
with the classes that rung's style draws:

```
mask[mode][rung] = modes[mode].classes  AND  { classes with width > 0 at that rung }
```

Two things fall out. A class the style hides at a rung is no longer read past
its record header there, instead of being read, bbox-tested, projected and then
drawn at width 0 — the whole cost and none of the picture. And the filter and
the style can no longer disagree, which was a bug that would have looked like a
missing road and read like a corrupt tile.

`gen_mode_masks.py` prints a note when a mode lists a class its style hides at
every rung, because `modes` reads like the list of what a mode draws and is only
half of it. It says this today for `living_street` and `track` in all three
modes, and `steps` in hike — all three are `hidden: true` in the style.

## What moved out of MapViewport::ZoomStep

Three fields lived in the zoom ladder and are now `when` blocks: `buildings`,
`builtUp` and `maxLabels`. They were drawing decisions in a table beside the
style rather than in it — two places to look, two places to edit, and the same
numbers free to drift. `MapViewState` lost `drawBuildings`, `drawBuiltUp` and
`maxLabels` with them and gained one `zoomStep`, which the caller uses to pick
the style and the renderer never reads.

`markerScale8` and `minMovePx` stayed. They are refresh policy — how big the
saved patch box is, and how far the marker must move before a waveform is worth
it — not appearance, and the maintainer's call was to keep them out of the
style.

## Where each piece lives

| file | what |
|---|---|
| `data/mapstyle.json` | the style, `when` lists and all. Written by the webapp, never by hand. |
| `scripts/mapstyle_variants.py` | the resolver. The only implementation; the laptop tooling loads *this* file rather than copying it. |
| `scripts/gen_mapstyle.py` | resolves 3 x 7, deduplicates, emits `MapStyleDefaults.h`. |
| `scripts/gen_mode_masks.py` | resolves 3 x 7, intersects with the drawn classes, emits `MapModeMaskDefaults.h`. |
| `src/activities/map/MapStyleTable.h` | `mapStyleFor(mode, rung)`. The entire runtime cost. |
| `scripts/test_mapstyle_variants.py` | the resolver's tests, run by `ctest` as `MapstyleVariants`. |
| `test/map_style_table/` | what the generators produced, and the invariants the firmware depends on. |

## What a hardware pass has to check

Nothing here has been on a panel. In order of what would hurt most:

1. **The coarse rungs, 3 to 6.** Do the thinner main roads still read as a
   hierarchy, or did they collapse into "all roads look the same"? The numbers
   in the file are a first proposal and were chosen by eye on a host render of
   Pezinok/Modra (`../../docs/device-preview-shots/per-rung-roads-2026-08-25.png`).
   Ink went 10.1 % to 9.5 % at rung 2, 10.3 % to 9.6 % at rung 4, 13.8 % to
   13.0 % at rung 6 — small, so the numbers probably want to go further.
2. **Rung 0 and 1 are unchanged by design.** If they look different, something
   resolved wrong.
3. **The mode switch.** No mode differs from another yet, so ride, hike and
   cycle must still look exactly as they did.
4. **Buildings at rung 0 and the wash from rung 1 out**, and the label counts
   per rung. Those numbers did not change; only where they are written did. A
   difference there is a migration bug, not a style decision.
