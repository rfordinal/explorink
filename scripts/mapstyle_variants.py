#!/usr/bin/env python3
"""
Resolve mapstyle.json's `when` blocks into one plain style per (mode, rung).

The whole point of this module: **the rules are evaluated here, at build
time.** The firmware never sees a rule, a condition or a list. It gets a
finished table and indexes it -- exactly as much work as reading one style
was before (docs/map-data-spec.md, "Style is per mode and per rung").

Shared by scripts/gen_mapstyle.py and scripts/gen_mode_masks.py so the two
cannot disagree about what a variant resolves to. The laptop-side tooling has
its own copy of the same rules in mapbuilder/style.py, for the same reason
the class enum is duplicated: nothing outside this repo may be read to build
this repo.

The grammar, and it is deliberately small:

    { "match": {"class": ["primary"]},
      "width": 9, "casing_px": 2,
      "when": [
        {"steps": [3, 4],                "width": 6, "casing_px": 1},
        {"steps": [5, 6],                "width": 4, "casing_px": 0},
        {"modes": ["hike"], "steps": [6], "hidden": true}
      ] }

- A `when` list may sit on **any object** in the style tree. One walk handles
  roads rules, water rules, landuse rules, the scalar layer blocks and the
  `modes` lists, and it keeps working for a block added later.
- `modes` omitted means every mode; `steps` omitted means every rung. An entry
  with neither is a patch that always applies, which is legal and is how a
  value is overridden without repeating the outer field.
- Entries apply **in order, last matching one wins.** That is the whole
  precedence rule -- there is no specificity scoring to reason about, and the
  file reads top to bottom like the panel is drawn.
- A patch is a shallow merge, except that a dict value deep-merges, so
  `{"match": ...}` can never be reached and a nested block can be partly
  overridden.
"""
import copy

# mapstyle.json's `modes` keys, in MapRideMode order (Ride=0, Hike=1, Cycle=2).
MODE_ORDER = ["ride", "hike", "cycle"]

# MapRideMode.h's kMapZoomStepCount. Duplicated rather than imported for the
# same reason the class enum is: this repo builds from its own checkout alone.
ZOOM_STEPS = 7

# `when` is banned under these top-level keys. `device` carries the marker
# anchor, which MapViewport turns into a constexpr the tile arithmetic is
# built on (MapViewport.h, kAnchorScreenX) -- a per-rung anchor would mean a
# per-rung viewport, which is a different feature and not this one.
_NO_WHEN_UNDER = {"device"}

_SELECTOR_KEYS = {"modes", "steps"}
# Never patchable: `match` is what identifies the rule a `when` belongs to, and
# a nested `when` would be a rule that rewrites its own conditions.
_UNPATCHABLE = {"match", "when"}


class VariantError(Exception):
    """Raised for a malformed `when`. Callers turn it into a build failure."""


def _check_entry(entry, path):
    if not isinstance(entry, dict):
        raise VariantError(f"{path}: every `when` entry must be an object, got {entry!r}")
    modes = entry.get("modes")
    if modes is not None:
        if not isinstance(modes, list):
            raise VariantError(f"{path}: `modes` must be a list of mode names")
        for name in modes:
            if name not in MODE_ORDER:
                raise VariantError(f"{path}: unknown mode '{name}' (one of {MODE_ORDER})")
    steps = entry.get("steps")
    if steps is not None:
        if not isinstance(steps, list):
            raise VariantError(f"{path}: `steps` must be a list of rung numbers")
        for step in steps:
            if not isinstance(step, int) or isinstance(step, bool) or not 0 <= step < ZOOM_STEPS:
                raise VariantError(f"{path}: rung {step!r} is not in 0..{ZOOM_STEPS - 1}")
    # **A singular selector is the dangerous typo, so it is refused by name.**
    # `mode` and `step` are not selectors, so they were taken as *patch fields*:
    # the entry then had no selector at all, which means "every mode and every
    # rung", so a block written to change hike changed ride and cycle too, and the
    # stray key rode into the resolved rule where the layer parser ignored it.
    # Found 2026-08-27 in a live style -- `{"mode": ["hike"], "pattern": "dashed"}`
    # had overridden every one of the 15 variants.
    for singular, plural in (("mode", "modes"), ("step", "steps")):
        if singular in entry:
            raise VariantError(
                f"{path}: `{singular}` is not a selector -- did you mean `{plural}`? "
                f"As written it is treated as a field to patch, and an entry with no "
                f"selector matches every mode and every rung")
    for key in entry:
        if key in _UNPATCHABLE:
            raise VariantError(f"{path}: `{key}` cannot be patched by a `when` entry")
    if not set(entry) - _SELECTOR_KEYS:
        raise VariantError(f"{path}: `when` entry selects but patches nothing")


def _matches(entry, mode, step):
    modes = entry.get("modes")
    if modes is not None and mode not in modes:
        return False
    steps = entry.get("steps")
    if steps is not None and step not in steps:
        return False
    return True


def _merge(target, patch):
    for key, value in patch.items():
        if key in _SELECTOR_KEYS:
            continue
        if isinstance(value, dict) and isinstance(target.get(key), dict):
            _merge(target[key], value)
        else:
            target[key] = copy.deepcopy(value)


def _walk(node, mode, step, path, when_allowed):
    if isinstance(node, list):
        return [_walk(item, mode, step, f"{path}[{index}]", when_allowed)
                for index, item in enumerate(node)]
    if not isinstance(node, dict):
        return node

    out = {}
    for key, value in node.items():
        if key == "when":
            continue
        child_allowed = when_allowed and key not in _NO_WHEN_UNDER
        out[key] = _walk(value, mode, step, f"{path}.{key}" if path else key, child_allowed)

    when = node.get("when")
    if when is None:
        return out
    if not when_allowed:
        raise VariantError(f"{path}: `when` is not allowed here -- "
                           f"{sorted(_NO_WHEN_UNDER)} must be the same at every mode and rung")
    if not isinstance(when, list):
        raise VariantError(f"{path}.when: must be a list of entries")
    for index, entry in enumerate(when):
        _check_entry(entry, f"{path}.when[{index}]")
        # mode None is base(): every entry is still validated -- a malformed
        # `when` must fail the build whether or not it happens to match -- but
        # none is applied.
        if mode is not None and _matches(entry, mode, step):
            _merge(out, entry)
    return out


def resolve(style, mode, step):
    """The style as it applies to one travel mode at one zoom rung.

    Returns a new dict with every `when` applied and removed, so every reader
    downstream sees a plain style file and needs to know nothing about
    variants.
    """
    if mode not in MODE_ORDER:
        raise VariantError(f"unknown mode '{mode}' (one of {MODE_ORDER})")
    if not 0 <= step < ZOOM_STEPS:
        raise VariantError(f"rung {step} is not in 0..{ZOOM_STEPS - 1}")
    return _walk(style, mode, step, "", True)


def base(style):
    """The style with every `when` stripped and none applied.

    This is what a field means before any rung or mode has its say, and it is
    what the invariants are read off (the marker anchor, and the `max_labels`
    ceiling a rung can only lower).
    """
    return _walk(style, None, None, "", True)


def has_variants(style):
    """True when any `when` block exists at all.

    A style with none resolves to one variant for all 21 combinations, and the
    generated table costs 21 bytes rather than 21 structs.
    """
    found = [False]

    def scan(node):
        if isinstance(node, list):
            for item in node:
                scan(item)
        elif isinstance(node, dict):
            if "when" in node:
                found[0] = True
            for key, value in node.items():
                if key != "when":
                    scan(value)

    scan(style)
    return found[0]
