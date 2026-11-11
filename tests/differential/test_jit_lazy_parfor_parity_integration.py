#!/usr/bin/env python3
"""Hermetic tests for parity fixture composition and strict equivalence."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from jit_lazy_parfor_parity_integration import (
    SUMMARY, VOLATILE_GROUPS, compare_parity, merge_projects, parse_failures,
)


def create_zip(path: Path, project: dict, assets: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(project))
        for name, content in assets.items():
            archive.writestr(name, content)


def summary(passed: int = 2656, failed_groups: dict | None = None) -> dict:
    return {"total": 2656, "passed": passed, "failure_groups": failed_groups or {}}


class ParityIntegrationTests(unittest.TestCase):
    def test_merge_preserves_existing_project_and_assets(self) -> None:
        base = {
            "targets": [{
                "name": "Stage", "isStage": True, "layerOrder": 0,
                "variables": {"v": ["existing", 1]}, "lists": {}, "blocks": {},
            }, {
                "name": "Existing", "isStage": False, "layerOrder": 2,
                "blocks": {"b0001": {"opcode": "looks_say", "topLevel": False}},
            }],
            "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"},
        }
        original = copy.deepcopy(base)
        probe = {
            "targets": [
                {"name": "Stage", "isStage": True},
                {
                    "name": "Bench", "isStage": False, "layerOrder": 1,
                    "variables": {"v": ["counter", 0]}, "lists": {"l": ["items", []]},
                    "blocks": {
                        "b0001": {
                            "opcode": "event_whenflagclicked", "topLevel": True,
                            "next": "b0002", "parent": None,
                        },
                        "b0002": {
                            "opcode": "data_setvariableto", "topLevel": False,
                            "next": None, "parent": "b0001",
                            "fields": {"VARIABLE": ["counter", "v"]},
                        },
                    },
                },
            ],
        }
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            src, extra, dest = (root / name for name in ("base.sb3", "probe.sb3", "out.sb3"))
            assets = {"characters.svg": b"<svg></svg>", "audio.wav": b"hello"}
            create_zip(src, base, assets)
            create_zip(extra, probe, {})
            info = merge_projects(src, extra, dest)
            self.assertEqual(info["targets"], 3)
            with zipfile.ZipFile(dest) as archive:
                merged = json.loads(archive.read("project.json"))
                for name, content in assets.items():
                    self.assertEqual(archive.read(name), content)
            self.assertEqual(merged["targets"][:2], original["targets"])
            sprite = merged["targets"][2]
            self.assertEqual(sprite["name"], "__parfor_lazy_parity_probe__")
            self.assertEqual(sprite["layerOrder"], 3)
            self.assertEqual(sprite["blocks"]["parfor_integration_b0001"]["next"],
                             "parfor_integration_b0002")
            self.assertEqual(sprite["blocks"]["parfor_integration_b0002"]["fields"]["VARIABLE"][1],
                             "parfor_integration_v")
            self.assertEqual(set(sprite["variables"]), {"parfor_integration_v"})
            self.assertEqual(set(sprite["lists"]), {"parfor_integration_l"})

    def test_short_and_verbose_failed_case_formats(self) -> None:
        old = (
            "[FAILED] Looks: Size bounds: 10/12 (83%)\n"
            "┣ Test 8 Failed\n"
            "┗ Test 9 Failed\n"
            "[FAILED] Some Tests Were Not Successful: 2121/2123 (99%)\n"
        )
        groups, cases = parse_failures(old)
        self.assertEqual(groups["Looks: Size bounds"], (10, 12))
        self.assertEqual(cases["Looks: Size bounds"], [(8, ""), (9, "")])
        new = (
            "[FAILED] Operators: Math: 1/2 (50%)\n"
            "┗ Test 7 Failed: value should match | got 'bad' (expected 'ok')\n"
            "[FAILED] Some Tests Were Not Successful: 1/2 (50%)\n"
        )
        _, cases = parse_failures(new)
        self.assertEqual(cases["Operators: Math"], [(7, "value should match")])

    def test_incomplete_failed_case_output_rejected(self) -> None:
        with self.assertRaisesRegex(AssertionError, "incomplete"):
            parse_failures(
                "[FAILED] Math: Test: 1/2 (50%)\n"
                "[FAILED] Some Tests Were Not Successful: 1/2 (50%)"
            )

    def test_matching_parity(self) -> None:
        same = summary(2623, {"Operators: Foo": (8, 10)})
        self.assertFalse(compare_parity(same, copy.deepcopy(same))["clock_only_difference"])

    def test_same_failure_count_different_case_is_rejected(self) -> None:
        baseline = summary(2623, {"Operators: Foo": (8, 10)})
        candidate = copy.deepcopy(baseline)
        baseline["failure_cases"] = {"Operators: Foo": [(1, "same test")]}
        candidate["failure_cases"] = {"Operators: Foo": [(2, "other test")]}
        with self.assertRaisesRegex(AssertionError, "individual failed parity cases"):
            compare_parity(baseline, candidate)

    def test_clock_only_variation(self) -> None:
        baseline = summary(2623, {"Operators: Foo": (8, 10)})
        candidate = summary(2622, {
            "Operators: Foo": (8, 10), "Sensing: Date And Time": (17, 18)
        })
        self.assertIn("Sensing: Date And Time", VOLATILE_GROUPS)
        self.assertTrue(compare_parity(baseline, candidate)["clock_only_difference"])

    def test_other_failure_cannot_be_masked_by_clock(self) -> None:
        baseline = summary(2623, {"Operators: Foo": (8, 10)})
        candidate = summary(2622, {
            "Operators: Foo": (7, 10), "Sensing: Date And Time": (17, 18)
        })
        with self.assertRaisesRegex(AssertionError, "non-clock parity"):
            compare_parity(baseline, candidate)

    def test_unexplained_failure_count_is_rejected(self) -> None:
        with self.assertRaisesRegex(AssertionError, "unexplained parity"):
            compare_parity(summary(2623), summary(2622))

    def test_missing_checks_are_rejected(self) -> None:
        with self.assertRaisesRegex(AssertionError, "changed/shrank"):
            compare_parity(summary(), {"total": 2000, "passed": 2000, "failure_groups": {}})

    def test_pass_summary_parsing(self) -> None:
        self.assertEqual(SUMMARY.search("[PASSED] All Tests Were Successful: 2123/2123").groups(),
                         ("2123", "2123"))
        self.assertEqual(SUMMARY.search(
            "[FAILED] Some Tests Were Not Successful: 2623/2656 (98%)").groups(),
            ("2623", "2656"))


if __name__ == "__main__":
    unittest.main()
