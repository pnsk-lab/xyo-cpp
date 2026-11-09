#!/usr/bin/env python3
"""Real Scratch nested warp stack snapshot: GC-rooted arguments/source path.

The snapshot is passive; never allow the test to treat a captured source
cursor as an executable LLVM stack continuation.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_set, S_change, S_ifelse, S_repeat,
    num, text, arg, eq, make_project, write_sb3,
)


def create(path: Path):
    gen = Gen(["counter"], [])
    gen.define(
        "inner %s", ["msg"],
        [S_ifelse(
            eq(num(1), num(1)),
            [S_repeat(num(1_000_000_000),
                      [S_change("counter", num(1))])],
            [],
        )],
        warp=True,
    )
    gen.define(
        "outer %s", ["msg"], [S_call("inner %s", [arg("msg")])],
        warp=True,
    )
    gen.script([
        S_set("counter", num(0)),
        S_call("outer %s", [text("GC文字列 🧪")]),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    source = create(args.out / "interp-deep-warp-gc.sb3")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": "off",
        "XYO_EXIT_FRAMES": "4",
        "XYO_PAR": "0",
        "XYO_INTERP_OSR_SNAPSHOT": "1",
    })
    completed = subprocess.run(
        [str(args.native.resolve()), "play", str(source.resolve())],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=30,
    )
    if completed.returncode:
        raise AssertionError(
            f"native exit={completed.returncode}\n"
            + completed.stderr[-1800:]
        )
    regex = re.compile(
        r"\[interp-osr-snapshot\] frame=(\d+) script=(\S+) "
        r"depth=(\d+) roots=(\d+) counted_loop=(\d+) valid=(\d+) lexical=(\d+)"
    )
    snapshots = [tuple(map(int, (m[1], m[3], m[4], m[5], m[6], m[7])))
                 for m in regex.finditer(completed.stderr)]
    if len(snapshots) < 3:
        raise AssertionError(
            "too few nested warp snapshots: " + completed.stderr[-1500:]
        )
    for frame, depth, roots, counted, valid, lexical in snapshots:
        assert depth >= 4 and roots == 2
        assert counted == 1 and valid == 1 and lexical == 1, (
            frame, depth, roots, counted, valid, lexical
        )
    print(
        f"PASS real nested warp snapshot: {len(snapshots)} safe points, "
        "GC arguments=2, valid source ancestry and counted loop"
    )


if __name__ == "__main__":
    main()
