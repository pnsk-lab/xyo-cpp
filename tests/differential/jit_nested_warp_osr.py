#!/usr/bin/env python3
"""Opt-in native JIT OSR of one loop inside a zero-arg warp procedure.

The procedure's prefix and suffix must run exactly once.  The interpreter
owns both source frames while the native fiber executes only the remaining
iterations, then resumes the original procedure's next statement.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_set, S_change, S_repeat, S_call, S_add,
    make_project, num, write_sb3,
)


def fixture(path: Path, iterations: int) -> None:
    g = Gen(["prefix", "sum", "suffix", "done"], ["events"])
    g.define("work", [], [
        S_change("prefix", num(13)),
        S_add("events", num(5)),
        S_repeat(num(iterations), [S_change("sum", num(1))]),
        S_change("suffix", num(17)),
        S_add("events", num(9)),
    ], warp=True)
    g.script([
        S_set("prefix", num(0)),
        S_set("sum", num(0)),
        S_set("suffix", num(0)),
        S_call("work"),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))


def execute(binary: Path, path: Path, tier: str, enabled: bool):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": tier,
        "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1",
        "XYO_JIT_OSR_NESTED": "1" if enabled else "0",
        "XYO_JIT_OSR_TIMING": "1",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_GPU": "0",
    })
    proc = subprocess.run(
        [str(binary.resolve()), "play", str(path.resolve())],
        env=env, cwd=ROOT, capture_output=True, text=True, timeout=110,
    )
    if proc.returncode:
        raise AssertionError(
            f"{tier} nested={enabled} rc={proc.returncode}\n"
            + proc.stderr[-1800:]
        )
    pos = proc.stdout.find("{")
    if pos < 0:
        raise AssertionError("no state dump: " + proc.stdout[-500:])
    data, _ = json.JSONDecoder().raw_decode(proc.stdout[pos:])
    return data["Bench"], proc.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument(
        "--out", type=Path, default=ROOT / "build/osr-eval/nested-warp"
    )
    args = parser.parse_args()
    source = args.out / "nested-warp-region.sb3"
    iterations = 5_000_000
    fixture(source, iterations)
    expected = {
        "prefix": "13", "sum": str(iterations),
        "suffix": "17", "done": "1", "events": ["5", "9"],
    }
    baseline, _ = execute(args.native, source, "off", False)
    assert all(baseline[k] == v for k, v in expected.items()), baseline
    for tier, enabled in (
        ("hotspot", False),
        ("hotspot", True),
        ("eager", False),
    ):
        actual, diagnostics = execute(args.native, source, tier, enabled)
        assert actual == baseline, (
            tier, enabled, actual, baseline, diagnostics[-1500:]
        )
        promoted = "[jit-osr-nested] promoted " in diagnostics
        print(
            f"PASS {tier} nested={enabled} promoted={promoted}: "
            "procedure prefix/loop/suffix exact once"
        )
        if tier == "hotspot" and enabled and not promoted:
            raise AssertionError("native nested OSR did not trigger")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
