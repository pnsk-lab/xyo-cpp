#!/usr/bin/env python3
"""GC pressure across a native loop that reads a GC-managed procedure argument.

A pure join allocates a new Scratch string each iteration. The parameter
string consumed by LLVM and a separate UTF-16 string used only by the
interpreter procedure suffix must remain valid through GC collections.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, arg, join,
    make_project, num, text, write_sb3,
)

ITERATIONS = 5_000_000
MESSAGE = "元の手続き引数がGCを越えて生存 🍀"


def fixture(path: Path):
    g = Gen(["message", "count", "scratch", "done"], [])
    g.define(
        "work %s %s", ["item", "message"],
        [
            S_repeat(num(ITERATIONS), [
                S_set("scratch", join(arg("item"), text("!"))),
                S_change("count", num(1)),
            ]),
            S_set("message", arg("message")),
        ],
        warp=True,
    )
    g.script([
        S_set("count", num(0)),
        S_set("scratch", text("")),
        S_set("done", num(0)),
        S_call("work %s %s", [text("x"), text(MESSAGE)]),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(g.vars.keys(), g.lists.keys(), g))


def execute(
    binary: Path, source: Path, tier: str, argreads: bool,
    *, direct_args: bool = False,
):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": tier,
        "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1",
        "XYO_JIT_OSR_NESTED": "1",
        "XYO_JIT_OSR_NESTED_ARG_READS": "1" if argreads else "0",
        "XYO_JIT_OSR_NESTED_DIRECT_ARGS": "1" if direct_args else "0",
        "XYO_JIT_OSR_TIMING": "1",
        "XYO_GC_THRESHOLD": "1",
        "XYO_GC_DEBUG": "1",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_GPU": "0",
    })
    p = subprocess.run(
        [str(binary.resolve()), "play", str(source.resolve())],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=120,
    )
    if p.returncode:
        raise AssertionError(
            f"tier={tier} arg_reads={argreads} rc={p.returncode}\n"
            + p.stderr[-1500:]
        )
    begin = p.stdout.find("{")
    if begin < 0:
        raise AssertionError("Missing stage dump " + p.stdout[-500:])
    state, _ = json.JSONDecoder().raw_decode(p.stdout[begin:])
    return state["Bench"], p.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source = args.out / "argroots-gc-string-join.sb3"
    fixture(source)
    baseline, _ = execute(args.native, source, "off", False)
    assert baseline == {
        "message": MESSAGE,
        "count": str(ITERATIONS),
        "scratch": "x!",
        "done": "1",
    }, baseline

    for enabled, direct_args in (
        (False, False), (True, False), (True, True)
    ):
        state, log = execute(
            args.native, source, "hotspot", enabled,
            direct_args=direct_args,
        )
        assert state == baseline, (state, baseline)
        promoted = "[jit-osr-nested] promoted " in log
        collections = len(re.findall(r"\[gc\] collect #", log))
        if enabled:
            assert promoted, ("native OSR never promoted", log[-1500:])
            assert "[jit-osr-nested] restored caller" in log
            assert collections > 0, (
                "no GC observed while testing argument roots", log[-1500:]
            )
        else:
            assert not promoted
        print(
            f"PASS GC argument reads={enabled}, direct={direct_args}, "
            f"promoted={promoted}, "
            f"collections={collections}",
            flush=True,
        )


if __name__ == "__main__":
    main()
