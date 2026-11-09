#!/usr/bin/env python3
"""Radix String->Number differential test across JIT tiers and warp modes.

Exercises direct 0x/0b/0o strings with short/exact and long/rounded
representations, plus invalid prefixes. Every value is reallocated via
operator_join to cover cold string parsing.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))

from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, join, mod, num, text,
    var, make_project, write_sb3,
)
from jit_to_num_fastpath import execute


def make_fixture(path: Path, warp: bool, iterations: int = 420) -> None:
    g = Gen(["i", "value", "sum", "done"], [])
    variants = (
        (text("0b"), num(2)),                  # exact small bit
        (text("0o"), num(8)),                  # exact small octal
        (text("0x"), num(100)),                # small valid hex
        (text("0XABCD"), num(10)),             # alphabetic hex
        (text("0b111111111111111111111"), num(2)),
        (text("0o7777777777777777"), num(8)),
        (text("0xFFFFFFFFFFFF"), num(10)),     # near 2^52
        (text("0x2000000000000"), num(10)),    # >= 2^53
        (text("0xFFFFFFFFFFFFFFFF"), num(10)), # long FP fallback
        (text("0b00000000000000000000000000000000000000"), num(2)),
        (text("-0x"), num(10)),                # invalid signed prefix
        (text("0b101"), num(4)),               # invalid suffixes too
    )
    body = [S_change("i", num(1))]
    for prefix, modulus in variants:
        body += [
            S_set("value", join(prefix, mod(var("i"), modulus))),
            S_change("sum", var("value")),
        ]
    g.define(
        "radix", [],
        [S_set("i", num(0)), S_set("sum", num(0)),
         S_repeat(num(iterations), body), S_set("done", num(1))],
        warp=warp,
    )
    g.script([S_call("radix")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))


def main() -> None:
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build-to-num-cold/xyo"
    output = ROOT / "build-to-num-cold/radix-parity-fixtures"
    output.mkdir(parents=True, exist_ok=True)
    for warp in (False, True):
        project = output / ("warp.sb3" if warp else "nonwarp.sb3")
        make_fixture(project, warp)
        expected, _ = execute(binary, project, "off")
        assert expected["done"] == "1", (warp, expected)
        for tier in ("eager", "hotspot"):
            for cold_helper in (False, True):
                actual, _ = execute(binary, project, tier, cold_helper)
                if actual != expected:
                    raise AssertionError((warp, tier, cold_helper, expected, actual))
                print("PASS", "warp" if warp else "nonwarp",
                      tier, "cold", cold_helper, flush=True)


if __name__ == "__main__":
    main()
