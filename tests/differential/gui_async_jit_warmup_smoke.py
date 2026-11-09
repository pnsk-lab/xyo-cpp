#!/usr/bin/env python3
"""Bounded async JIT warmup: a single long warp procedure, no replay.

The opt-in scheduler may defer the *start* of a script while an LLVM
worker finishes. It must not drop Scratch state, run a prefix twice, or
deadlock when the pending job is later published.
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
    Gen, S_add, S_call, S_change, S_repeat, S_set, num, var,
    make_project, write_sb3,
)


def fixture(path: Path) -> None:
    g = Gen(["sum", "prefix", "done"], ["output"])
    g.define("large warp", [], [
        S_repeat(num(150000), [S_change("sum", num(1))]),
    ], warp=True)
    g.script([
        S_set("prefix", num(5)),
        S_set("sum", num(0)),
        S_call("large warp"),
        S_add("output", var("sum")),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))


def run(binary: Path, sb3: Path, tier: str, warmup: int):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_JIT_MODE": tier,
        "XYO_JIT_ASYNC": "1",
        "XYO_JIT_ASYNC_WARMUP_MS": str(warmup),
        "XYO_JIT_ASYNC_WARMUP_LOG": "1",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0",
    })
    result = subprocess.run(
        [str(binary.resolve()), "play", str(sb3.resolve())],
        env=env, cwd=ROOT, capture_output=True, text=True, timeout=120,
    )
    if result.returncode:
        raise AssertionError(
            f"{tier}/{warmup}: exit {result.returncode}, "
            f"stderr={result.stderr[-1200:]}"
        )
    pos = result.stdout.find("{")
    if pos < 0:
        raise AssertionError("missing final stage state: " + result.stdout[-800:])
    data, _ = json.JSONDecoder().raw_decode(result.stdout[pos:])
    return data["Bench"], result.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--out", type=Path, default=ROOT / "build/async-jit/warmup-gui")
    args = parser.parse_args()
    sb3 = args.out / "warmup-warp-procedure.sb3"
    fixture(sb3)
    base, _ = run(args.native, sb3, "off", 0)
    assert base["sum"] == "150000", base
    assert base["prefix"] == "5" and base["done"] == "1", base
    assert base["output"] == ["150000"], base
    for tier, warmup in (("hotspot", 0), ("hotspot", 2000), ("eager", 0)):
        value, log = run(args.native, sb3, tier, warmup)
        assert value == base, (tier, warmup, value, base)
        if tier == "hotspot" and warmup and "[jit-warmup] deferred" not in log:
            raise AssertionError("warmup was not exercised: " + log[-800:])
        print(f"PASS {tier}, warmup={warmup}: final Scratch state equal")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
