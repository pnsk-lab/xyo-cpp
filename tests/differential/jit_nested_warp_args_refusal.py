#!/usr/bin/env python3
"""Argument-carrying warp procedures must never use zero-argument LLVM OSR.

The interpreter can retain arbitrary GC-managed argument Values, but the
native one-loop continuation currently has no argument/SSA reconstruction.
Check fail-closed eligibility and final observable state.
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
    Gen, S_call, S_change, S_repeat, S_set, arg, make_project,
    num, text, write_sb3,
)

MESSAGE = "引数文字列 🌠"
ITERATIONS = 250_000


def fixture(path: Path) -> Path:
    g = Gen(["sum", "seen", "prefix", "suffix"], [])
    g.define(
        "work %s %s", ["message", "delta"],
        [
            S_change("prefix", num(13)),
            S_repeat(num(ITERATIONS), [
                S_change("sum", arg("delta")),
            ]),
            S_set("seen", arg("message")),
            S_change("suffix", num(17)),
        ],
        warp=True,
    )
    g.script([
        S_set("prefix", num(0)),
        S_set("suffix", num(0)),
        S_set("sum", num(0)),
        S_set("seen", text("")),
        S_call("work %s %s", [text(MESSAGE), num(2)]),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))
    return path


def execute(binary: Path, source: Path, mode: str, nested: bool):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": mode,
        "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1",
        "XYO_JIT_OSR_NESTED": "1" if nested else "0",
        "XYO_JIT_OSR_TIMING": "1",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_DUMPVARS": "json",
    })
    finished = subprocess.run(
        [str(binary.resolve()), "play", str(source.resolve())],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90,
    )
    if finished.returncode:
        raise AssertionError(
            f"mode={mode} nested={nested}: exit {finished.returncode}\n"
            + finished.stderr[-1200:]
        )
    start = finished.stdout.find("{")
    if start == -1:
        raise AssertionError("no Scratch state: " + finished.stdout[-800:])
    state, _ = json.JSONDecoder().raw_decode(finished.stdout[start:])
    return state["Bench"], finished.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source = fixture(args.out / "arg-carrying-warp-no-osr.sb3")

    expected, _ = execute(args.native, source, "off", False)
    assert expected["sum"] == str(ITERATIONS * 2), expected
    assert expected["prefix"] == "13" and expected["suffix"] == "17"
    assert expected["seen"] == MESSAGE, expected

    for mode, nested in (
        ("hotspot", False), ("hotspot", True), ("eager", True)
    ):
        actual, diagnostic = execute(args.native, source, mode, nested)
        assert actual == expected, (mode, nested, actual, expected)
        assert "[jit-osr-nested] promoted " not in diagnostic, (
            mode, nested, diagnostic[-1200:]
        )
        print(f"PASS {mode} nested={nested}: argument GC/SSA fallback, exact once")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
