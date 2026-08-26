#!/usr/bin/env python3
"""Tests for the `when` resolver -- scripts/mapstyle_variants.py.

Run: python3 scripts/test_mapstyle_variants.py

Python rather than C++ because this is where the rules are actually evaluated.
By the time the firmware sees a style, every `when` is gone: the device gets a
table and an index (MapStyleTable.h). So a mistake here is a mistake that
compiles cleanly and draws the wrong map, and no C++ test can reach it.
"""
import copy
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapstyle_variants as mv  # noqa: E402


def _when_keys(node, path=""):
    """Every path at which a `when` key survived. Should always be empty."""
    found = []
    if isinstance(node, dict):
        if "when" in node:
            found.append(path)
        for key, value in node.items():
            found += _when_keys(value, f"{path}.{key}")
    elif isinstance(node, list):
        for index, item in enumerate(node):
            found += _when_keys(item, f"{path}[{index}]")
    return found


def road(class_name, **fields):
    rule = {"match": {"class": [class_name]}}
    rule.update(fields)
    return {"layers": {"roads": {"rules": [rule]}}}


def width(style, index=0):
    return style["layers"]["roads"]["rules"][index]["width"]


class Selectors(unittest.TestCase):
    def test_a_step_list_applies_only_to_those_rungs(self):
        s = road("primary", width=9, when=[{"steps": [4, 5, 6], "width": 4}])
        self.assertEqual(width(mv.resolve(s, "ride", 3)), 9)
        self.assertEqual(width(mv.resolve(s, "ride", 4)), 4)
        self.assertEqual(width(mv.resolve(s, "ride", 6)), 4)

    def test_a_mode_list_applies_only_to_those_modes(self):
        s = road("footway", width=1, when=[{"modes": ["hike"], "width": 3}])
        self.assertEqual(width(mv.resolve(s, "ride", 0)), 1)
        self.assertEqual(width(mv.resolve(s, "hike", 0)), 3)

    def test_both_lists_must_match(self):
        s = road("path", width=1, when=[{"modes": ["hike"], "steps": [0], "width": 5}])
        self.assertEqual(width(mv.resolve(s, "hike", 0)), 5)
        self.assertEqual(width(mv.resolve(s, "hike", 1)), 1)
        self.assertEqual(width(mv.resolve(s, "cycle", 0)), 1)

    def test_neither_list_means_always(self):
        s = road("track", width=1, when=[{"width": 2}])
        self.assertEqual(width(mv.resolve(s, "ride", 0)), 2)
        self.assertEqual(width(mv.resolve(s, "cycle", 6)), 2)


class Precedence(unittest.TestCase):
    def test_the_last_matching_entry_wins(self):
        # The entire precedence rule. No specificity scoring, no mode-beats-rung
        # question to remember -- the file reads top to bottom.
        s = road("primary", width=9, when=[
            {"steps": [4], "width": 6},
            {"modes": ["hike"], "width": 2},
        ])
        self.assertEqual(width(mv.resolve(s, "hike", 4)), 2, "the later entry wins")
        s["layers"]["roads"]["rules"][0]["when"].reverse()
        self.assertEqual(width(mv.resolve(s, "hike", 4)), 6, "and reversing them reverses that")

    def test_a_patch_leaves_the_fields_it_does_not_name(self):
        s = road("primary", width=9, casing_px=2, major=True,
                 when=[{"steps": [5], "width": 4}])
        out = mv.resolve(s, "ride", 5)["layers"]["roads"]["rules"][0]
        self.assertEqual(out["width"], 4)
        self.assertEqual(out["casing_px"], 2)
        self.assertTrue(out["major"])

    def test_a_dict_value_deep_merges(self):
        s = {"layers": {"places": {"a": {"x": 1, "y": 2}, "when": [{"steps": [0], "a": {"y": 9}}]}}}
        out = mv.resolve(s, "ride", 0)["layers"]["places"]["a"]
        self.assertEqual(out, {"x": 1, "y": 9})


class BaseAndStripping(unittest.TestCase):
    def test_base_applies_nothing_and_strips_every_when(self):
        s = road("primary", width=9, when=[{"width": 4}])
        out = mv.base(s)["layers"]["roads"]["rules"][0]
        self.assertEqual(out["width"], 9)
        self.assertNotIn("when", out)

    def test_resolve_strips_every_when(self):
        s = road("primary", width=9, when=[{"steps": [1], "width": 4}])
        self.assertNotIn("when", mv.resolve(s, "ride", 1)["layers"]["roads"]["rules"][0])

    def test_the_input_is_not_mutated(self):
        s = road("primary", width=9, when=[{"steps": [1], "width": 4}])
        before = copy.deepcopy(s)
        mv.resolve(s, "ride", 1)
        self.assertEqual(s, before)

    def test_has_variants(self):
        self.assertFalse(mv.has_variants(road("primary", width=9)))
        self.assertTrue(mv.has_variants(road("primary", width=9, when=[{"width": 4}])))


class Rejections(unittest.TestCase):
    """Every one of these is a build failure, not a warning.

    A generated file is a file that ships. A warning in a build log is a
    warning nobody reads (docs/map-data-spec.md, "Mode is a render-time
    filter": warning-and-emit is the same bug wearing a log line).
    """

    def bad(self, style, needle, mode="ride", step=0):
        with self.assertRaises(mv.VariantError) as caught:
            mv.resolve(style, mode, step)
        self.assertIn(needle, str(caught.exception))

    def test_unknown_mode_name(self):
        self.bad(road("primary", width=9, when=[{"modes": ["moto"], "width": 1}]), "unknown mode")

    def test_rung_out_of_range(self):
        self.bad(road("primary", width=9, when=[{"steps": [7], "width": 1}]), "not in 0..6")

    def test_a_when_that_patches_nothing(self):
        self.bad(road("primary", width=9, when=[{"steps": [0]}]), "patches nothing")

    def test_match_cannot_be_patched(self):
        self.bad(road("primary", width=9, when=[{"match": {"class": ["motorway"]}}]), "cannot be patched")

    def test_a_nested_when_is_refused(self):
        self.bad(road("primary", width=9, when=[{"when": []}]), "cannot be patched")

    def test_when_is_banned_under_device(self):
        # A per-rung marker anchor would be a per-rung viewport: MapViewport
        # turns the anchor into a constexpr and the tile arithmetic is built
        # on it (MapViewport.h, kAnchorScreenX).
        self.bad({"device": {"marker_x_px": 230, "when": [{"steps": [0], "marker_x_px": 100}]}},
                 "not allowed here")

    def test_when_must_be_a_list(self):
        self.bad(road("primary", width=9, when={"width": 1}), "must be a list")


class TheRealStyleFile(unittest.TestCase):
    def test_every_mode_and_rung_resolves(self):
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "data", "mapstyle.json")
        with open(path) as handle:
            style = json.load(handle)
        for mode in mv.MODE_ORDER:
            for step in range(mv.ZOOM_STEPS):
                resolved = mv.resolve(style, mode, step)
                self.assertIn("roads", resolved["layers"])
                # Structurally, not by grepping the text: the `_comment` fields
                # are prose and the word "when" appears in several of them.
                self.assertEqual(_when_keys(resolved), [], f"{mode}@{step}")


if __name__ == "__main__":
    unittest.main()
