#!/usr/bin/env python3
"""Native counted repeat must resume saved counter, not re-evaluate TIMES.

The TIMES reporter reads a live procedure parameter. Interpreter has already
evaluated that reporter before native handoff. The generated continuation
must use the captured remaining count; prefix/suffix still run exactly once.
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
    Gen, S_call, S_change, S_repeat, S_set, arg,
    make_project, num, text, write_sb3,
)

COUNT = 20_000_000
MESSAGE = "動的TIMESから復帰 🌙"


def fixture(path: Path):
    gen = Gen(["sum", "prefix", "suffix", "message", "done"], [])
    gen.define(
        "dynamic %s %s", ["times", "msg"], [
            S_change("prefix", num(11)),
            S_repeat(arg("times"), [S_change("sum", num(1))]),
            S_set("message", arg("msg")),
            S_change("suffix", num(13)),
        ], warp=True,
    )
    gen.script([
        S_set("sum", num(0)),
        S_set("prefix", num(0)),
        S_set("suffix", num(0)),
        S_set("message", text("")),
        S_set("done", num(0)),
        S_call("dynamic %s %s", [num(COUNT), text(MESSAGE)]),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(gen.vars.keys(), gen.lists.keys(), gen))


def run(binary: Path, source: Path, tier: str, arg_reads: bool):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": tier,
        "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1",
        "XYO_JIT_OSR_NESTED": "1",
        "XYO_JIT_OSR_NESTED_ARG_READS": "1" if arg_reads else "0",
        "XYO_JIT_OSR_TIMING": "1",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
    })
    result = subprocess.run(
        [str(binary.resolve()), "play", str(source.resolve())],
        env=env, cwd=ROOT, capture_output=True, text=True, timeout=120,
    )
    if result.returncode:
        raise AssertionError(
            f"tier={tier} arg_reads={arg_reads}: "
            f"exit={result.returncode} {result.stderr[-1200:]}"
        )
    pos = result.stdout.find("{")
    if pos < 0:
        raise AssertionError("no variable dump")
    state, _ = json.JSONDecoder().raw_decode(result.stdout[pos:])
    return state["Bench"], result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    source = args.out / "arg-times-warp.sb3"
    fixture(source)
    baseline, _ = run(args.native, source, "off", False)
    assert baseline == {
        "sum": str(COUNT),
        "prefix": "11",
        "suffix": "13",
        "message": MESSAGE,
        "done": "1",
    }, baseline

    for reads in (False, True):
        result, log = run(args.native, source, "hotspot", reads)
        assert result == baseline, (reads, result, baseline)
        promoted = "[jit-osr-nested] promoted " in log
        assert promoted == reads, (reads, promoted, log[-1000:])
        if reads:
            assert "[jit-osr-nested] restored caller" in log
        print(
            f"PASS dynamic-times arg_reads={reads} promoted={promoted}: "
            "original remaining count and suffix preserved", flush=True
        )


if __name__ == "__main__":
    main()
