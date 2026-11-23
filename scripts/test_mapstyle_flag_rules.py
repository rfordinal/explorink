#!/usr/bin/env python3
"""Tests for the flag/roughness rule grammar -- gen_mapstyle.py's road_flag_rules.

Run: python3 scripts/test_mapstyle_flag_rules.py

Python rather than C++ for the same reason the `when` resolver's tests are: the
grammar is evaluated here, at build time. The firmware never sees a rule, only
four resolved MapRoadFlagRule structs, so a mistake in the parsing compiles
cleanly and draws the wrong map. The C++ half -- that the renderer actually
reads those structs -- is test/map_flag_rules/.
"""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_mapstyle as gm  # noqa: E402

EMPTY = (0, 0, 0, 0, 0, 0, "Solid", False)


def style(*rules):
    return {"layers": {"roads": {"rules": list(rules)}}}


def flag_rule(flag=None, **fields):
    match = {}
    if flag is not None:
        match["flag"] = flag
    if "roughness_min" in fields:
        match["roughness_min"] = fields.pop("roughness_min")
    rule = {"match": match}
    rule.update(fields)
    return rule


class Slots(unittest.TestCase):
    def test_a_style_with_no_flag_rule_is_four_empty_slots(self):
        rules = gm.road_flag_rules(style({"match": {"class": ["path"]}, "width": 1}))
        self.assertEqual(rules, [EMPTY] * gm._FLAG_RULE_SLOTS)

    def test_slots_are_padded_and_ordered_as_written(self):
        rules = gm.road_flag_rules(style(
            flag_rule("no_foot", width=3),
            flag_rule("bridge", width=5),
        ))
        self.assertEqual(len(rules), gm._FLAG_RULE_SLOTS)
        self.assertEqual(rules[0][0], 1 << 7)
        self.assertEqual(rules[1][0], 1 << 1)
        self.assertEqual(rules[2], EMPTY)
        self.assertEqual(rules[3], EMPTY)

    def test_more_rules_than_slots_is_a_build_failure(self):
        many = [flag_rule("no_foot", width=1)] * (gm._FLAG_RULE_SLOTS + 1)
        with self.assertRaises(SystemExit) as caught:
            gm.road_flag_rules(style(*many))
        self.assertIn("kMapRoadFlagRuleSlots", str(caught.exception))


class Masks(unittest.TestCase):
    def test_a_single_flag_name_is_accepted_as_a_bare_string(self):
        rules = gm.road_flag_rules(style(flag_rule("unpaved", width=2)))
        self.assertEqual(rules[0][0], 1 << 4)

    def test_a_list_of_flags_ors_their_bits(self):
        rules = gm.road_flag_rules(style(flag_rule(["no_motor", "no_bicycle", "no_foot"], width=2)))
        self.assertEqual(rules[0][0], (1 << 5) | (1 << 6) | (1 << 7))

    def test_every_named_bit_matches_the_spec(self):
        # docs/map-data-spec.md, "Flag bits". A drift here is silent: the rule
        # would compile and match the wrong attribute.
        expected = {"link": 0, "bridge": 1, "tunnel": 2, "oneway": 3, "unpaved": 4,
                    "no_motor": 5, "no_bicycle": 6, "no_foot": 7, "seasonal": 14, "permit": 15}
        self.assertEqual(gm._FLAG_BIT, expected)

    def test_the_waymark_bits_have_no_names(self):
        # 8-13 are one 6-bit symbol id, not six flags. A name for a single bit
        # of it would invite a rule that cannot be right.
        for name in ("waymark", "waymark8", "wm8"):
            with self.assertRaises(SystemExit):
                gm.road_flag_rules(style(flag_rule(name, width=1)))

    def test_an_unknown_flag_name_is_a_build_failure(self):
        with self.assertRaises(SystemExit) as caught:
            gm.road_flag_rules(style(flag_rule("access_no", width=1)))
        self.assertIn("unknown flag", str(caught.exception))


class TheStroke(unittest.TestCase):
    def test_width_is_required_because_a_rule_replaces_rather_than_patches(self):
        with self.assertRaises(SystemExit) as caught:
            gm.road_flag_rules(style(flag_rule("no_foot")))
        self.assertIn("must state its own `width`", str(caught.exception))

    def test_hidden_needs_no_width_and_zeroes_the_stroke(self):
        rules = gm.road_flag_rules(style(flag_rule("no_foot", hidden=True)))
        self.assertEqual(rules[0], (1 << 7, 0, 0, 0, 0, 0, "Solid", True))

    def test_a_pattern_becomes_a_dash_and_a_gap(self):
        rules = gm.road_flag_rules(style(flag_rule("no_foot", width=1, pattern="dotted")))
        mask, roughness_min, width, casing, dash, gap, pattern, hidden = rules[0]
        self.assertEqual((width, dash, gap, pattern), (1, 2, 3, "Dashed"))

    def test_a_casing_with_no_white_left_falls_back_to_solid(self):
        rules = gm.road_flag_rules(style(flag_rule("no_foot", width=2, casing_px=1)))
        self.assertEqual(rules[0][3], 0)

    def test_a_width_that_rounds_to_zero_is_floored_at_one(self):
        rules = gm.road_flag_rules(style(flag_rule("no_foot", width=0.2)))
        self.assertEqual(rules[0][2], 1)

    def test_fill_and_tone_are_not_part_of_this_grammar(self):
        for field in ({"fill": "tone"}, {"tone": "light"}, {"major": True}):
            with self.assertRaises(SystemExit) as caught:
                gm.road_flag_rules(style(flag_rule("no_foot", width=4, **field)))
            self.assertIn("no meaning on a flag rule", str(caught.exception))


class Roughness(unittest.TestCase):
    def test_a_roughness_floor_needs_no_flag(self):
        rules = gm.road_flag_rules(style(flag_rule(roughness_min=5, width=3)))
        self.assertEqual(rules[0][0], 0)
        self.assertEqual(rules[0][1], 5)

    def test_a_floor_outside_the_three_bits_is_a_build_failure(self):
        for bad in (-1, 8, 249):
            with self.assertRaises(SystemExit) as caught:
                gm.road_flag_rules(style(flag_rule(roughness_min=bad, width=3)))
            self.assertIn("roughness_min", str(caught.exception))

    def test_a_floor_of_zero_with_no_flag_matches_everything_and_is_refused(self):
        with self.assertRaises(SystemExit) as caught:
            gm.road_flag_rules(style(flag_rule(roughness_min=0, width=3)))
        self.assertIn("matches every way", str(caught.exception))


class MixingWithClassRules(unittest.TestCase):
    def test_a_rule_cannot_match_both_a_class_and_a_flag(self):
        rule = flag_rule("no_foot", width=3)
        rule["match"]["class"] = ["path"]
        with self.assertRaises(SystemExit) as caught:
            gm.road_flag_rules(style(rule))
        self.assertIn("cannot match both", str(caught.exception))

    def test_a_rule_matching_nothing_at_all_is_a_build_failure(self):
        # Silently ignored before this grammar existed: road_widths() looped over
        # an empty class list and moved on, so a typo'd `match` compiled clean and
        # drew nothing.
        with self.assertRaises(SystemExit) as caught:
            gm.road_flag_rules(style({"match": {"classes": ["path"]}, "width": 3}))
        self.assertIn("can never apply", str(caught.exception))

    def test_a_disabled_roads_layer_yields_empty_slots(self):
        disabled = style(flag_rule("no_foot", width=3))
        disabled["layers"]["roads"]["enabled"] = False
        self.assertEqual(gm.road_flag_rules(disabled), [EMPTY] * gm._FLAG_RULE_SLOTS)


class TheRealStyleFile(unittest.TestCase):
    def test_the_shipped_style_carries_no_flag_rule(self):
        """The default must draw exactly what it drew before this grammar existed.

        A mark on the map is judged on the panel, and there is no device to judge
        one on. So the shipped file stays empty here until the maintainer turns
        the knob on -- and this test going red is how that decision announces
        itself.
        """
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "data", "mapstyle.json")
        with open(path) as handle:
            loaded = json.load(handle)
        self.assertEqual(gm.road_flag_rules(loaded), [EMPTY] * gm._FLAG_RULE_SLOTS)


if __name__ == "__main__":
    unittest.main()
