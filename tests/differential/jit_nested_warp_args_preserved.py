#!/usr/bin/env python3
"""GC-rooted interpreter argument frames across a native nested warp OSR.

Only a loop that NEVER evaluates a procedure-argument reporter may transfer
to the standalone native loop. Arguments before/after the loop are executed
exactly once by the original interpreter caller, even after the LLVM fiber.
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

MESSAGE = "引数のGCルート 🍀"


def fixture(
    path: Path, *, use_argument_in_loop: bool,
    iterations_override: int | None = None,
) -> tuple[int, dict]:
    iterations = (iterations_override if iterations_override is not None
                  else 250_000 if use_argument_in_loop else 20_000_000)
    g = Gen(["prefix", "sum", "suffix", "message", "done"], [])
    g.define(
        "worker %s %s", ["text", "delta"],
        [
            S_change("prefix", arg("delta")),
            S_repeat(num(iterations), [
                S_change(
                    "sum",
                    arg("delta") if use_argument_in_loop else num(1),
                ),
            ]),
            S_set("message", arg("text")),
            S_change("suffix", arg("delta")),
        ],
        warp=True,
    )
    g.script([
        S_set("prefix", num(0)),
        S_set("sum", num(0)),
        S_set("suffix", num(0)),
        S_set("message", text("")),
        S_set("done", num(0)),
        S_call("worker %s %s", [text(MESSAGE), num(2)]),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))
    expected = {
        "prefix": "2",
        "sum": str(iterations * (2 if use_argument_in_loop else 1)),
        "suffix": "2",
        "message": MESSAGE,
        "done": "1",
    }
    return iterations, expected


def execute(
    binary: Path, source: Path, tier: str, enabled: bool,
    *, arg_reads: bool = False, direct_args: bool = False,
):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": tier,
        "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1",
        "XYO_JIT_OSR_NESTED": "1" if enabled else "0",
        "XYO_JIT_OSR_NESTED_ARG_READS": "1" if arg_reads else "0",
        "XYO_JIT_OSR_NESTED_DIRECT_ARGS": "1" if direct_args else "0",
        "XYO_JIT_OSR_TIMING": "1",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_GPU": "0",
    })
    proc = subprocess.run(
        [str(binary.resolve()), "play", str(source.resolve())],
        cwd=ROOT, capture_output=True, text=True, timeout=120, env=env,
    )
    if proc.returncode:
        raise AssertionError(
            f"mode={tier} nested={enabled} exit={proc.returncode}\n"
            + proc.stderr[-2000:]
        )
    pos = proc.stdout.find("{")
    if pos < 0:
        raise AssertionError("No final Scratch state: " + proc.stdout[-700:])
    state, _ = json.JSONDecoder().raw_decode(proc.stdout[pos:])
    return state["Bench"], proc.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    for argument_in_loop in (False, True):
        source = args.out / (
            "args-referenced-loop.sb3" if argument_in_loop
            else "args-preserved-native-loop.sb3"
        )
        _, expected = fixture(source, use_argument_in_loop=argument_in_loop)
        baseline, _ = execute(args.native, source, "off", False)
        assert all(baseline[k] == v for k, v in expected.items()), (
            source.name, baseline, expected
        )
        for tier, enabled in (
            ("hotspot", False), ("hotspot", True), ("eager", False)
        ):
            actual, diagnostics = execute(args.native, source, tier, enabled)
            assert actual == baseline, (
                source.name, tier, enabled, actual, baseline, diagnostics[-1300:]
            )
            promoted = "[jit-osr-nested] promoted " in diagnostics
            if tier == "hotspot" and enabled and not argument_in_loop:
                assert promoted, (
                    "Argument-preserving region never promoted",
                    diagnostics[-1500:],
                )
                assert "[jit-osr-nested] restored caller" in diagnostics, (
                    "Lost interpreter continuation",
                    diagnostics[-1500:],
                )
            else:
                assert not promoted, (
                    "Unwanted promotion of argument-dependent region",
                    tier, enabled, diagnostics[-1000:],
                )
            print(
                f"PASS region={source.name} {tier} nested={enabled} "
                f"promoted={promoted}: args GC/prefix/suffix preserved",
                flush=True,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
