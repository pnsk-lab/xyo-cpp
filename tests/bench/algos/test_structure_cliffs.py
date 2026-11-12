#!/usr/bin/env python3
"""Fast harness tests; does not need a built runtime."""
import os
import unittest
from unittest.mock import patch

from bench_structure_cliffs import KINDS, clean_env, oracle, project, summarize, validate


class StructureCliffsTest(unittest.TestCase):
    def test_oracle_known_values(self):
        self.assertEqual(oracle(0), [1, 0, 1])
        self.assertEqual(oracle(1), [40, 40, 40])
        self.assertEqual(oracle(2), [703, 743, 703])

    def test_fixture_graph_references(self):
        for kind in KINDS:
            for count in (0, 1, 7, 8, 65):
                p = project(count, kind, 1)
                blocks = p["targets"][1]["blocks"]
                for block in blocks.values():
                    for key in ("next", "parent"):
                        if block[key] is not None:
                            self.assertIn(block[key], blocks)
                    for value in block["inputs"].values():
                        for member in value[1:]:
                            if isinstance(member, str):
                                self.assertIn(member, blocks)
                self.assertEqual(sum(b["opcode"] == "event_whenflagclicked"
                                     for b in blocks.values()), 1)

    def test_environment_isolation(self):
        with patch.dict(os.environ, {"XYO_PROF": "1", "XYO_OPT_ALL": "1",
                                     "XYO_JIT_NUMERIC_REPEAT_COPY": "1",
                                     "XYO_JIT_NUMERIC_REPEAT_READONLY_STRING": "1",
                                     "XYO_JIT_NUMERIC_REPEAT_INLINE": "1"}):
            env = clean_env("default")
            self.assertNotIn("XYO_PROF", env)
            self.assertNotIn("XYO_OPT_ALL", env)
            self.assertNotIn("XYO_JIT_NUMERIC_REPEAT_COPY", env)
            self.assertEqual(clean_env("scalar_copy")["XYO_JIT_NUMERIC_REPEAT_COPY"], "1")
            self.assertNotIn("XYO_JIT_NUMERIC_REPEAT_READONLY_STRING", env)
            self.assertNotIn("XYO_JIT_NUMERIC_REPEAT_INLINE", env)
            self.assertEqual(clean_env("composition_all")["XYO_JIT_NUMERIC_REPEAT_READONLY_STRING"], "1")
            self.assertEqual(clean_env("composition_all")["XYO_JIT_NUMERIC_REPEAT_INLINE"], "1")

    def test_validation_rejects_wrong_or_incomplete_results(self):
        validate({"checks": [1, 0, 1], "timings_ms": [0]}, oracle(0), 1)
        for result in ({"checks": [1, 0, 1], "timings_ms": [0]},
                       {"checks": [1, 0, 2], "timings_ms": [1]},
                       {"checks": [1, 0, 1], "timings_ms": []}):
            with self.assertRaises(RuntimeError):
                validate(result, oracle(0), 1, timed=True)

    def test_summary_pairs_by_block_not_row_order(self):
        rows = []
        for block in (0, 1):
            for profile, scale in (("default", 2), ("repeat_ssa_off", 4),
                                   ("scalar_copy", 1), ("scalar_copy_off", 4)):
                for kind, factor in (("inline", 1), ("unrelated_write", 3)):
                    rows.append({"block": block, "profile": profile, "kind": kind,
                                 "timings_ms": [scale * factor * (block + 1)]})
        rows.reverse()
        result = summarize(rows, ["inline", "unrelated_write"],
                           ["default", "repeat_ssa_off", "scalar_copy", "scalar_copy_off"])
        self.assertEqual(result["default/unrelated_write"]["median_slowdown_vs_inline"], 3)
        self.assertEqual(result["repeat_ssa_speedup/inline"]["median"], 2)
        self.assertEqual(result["scalar_copy_speedup/unrelated_write"]["median"], 2)
        self.assertEqual(result["scalar_copy_default_speedup/unrelated_write"]["median"], 2)


if __name__ == "__main__":
    unittest.main()
