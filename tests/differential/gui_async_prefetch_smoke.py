#!/usr/bin/env python3
"""Warp-list broadcast prefetch must never execute or replay Scratch effects.

The receiver is a nested warp procedure using a list-backed RAM image. Its
script can be compiled before launch, but the broadcast sender must still
observe exactly one receiver execution with the same output ordering.
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
    Gen, S_add, S_call, S_set, S_delall, S_repeat, length,
    make_project, num, write_sb3,
)


def create_fixture(path: Path):
    g = Gen(["sum", "done", "start"], ["RAM"])
    g.define("reset RAM", [], [
        S_repeat(num(2000), [S_add("RAM", num(7))]),
    ], warp=True)

    sender = g.script([
        S_set("sum", num(0)),
        S_set("done", num(0)),
        S_delall("RAM"),
    ], x=0)
    cursor = sender
    while g.blocks[cursor]["next"]:
        cursor = g.blocks[cursor]["next"]
    broadcast = g.new("event_broadcastandwait", parent=cursor)
    g.blocks[broadcast]["inputs"]["BROADCAST_INPUT"] = [
        1, [11, "start", "start-id"]
    ]
    g.blocks[cursor]["next"] = broadcast
    final = g.emit_stmt(S_set("done", num(2)), broadcast)
    g.blocks[broadcast]["next"] = final

    receiver = g.script([
        S_call("reset RAM"),
        S_set("sum", length("RAM")),
        S_set("start", num(1)),
    ], x=300)
    g.blocks[receiver]["opcode"] = "event_whenbroadcastreceived"
    g.blocks[receiver]["fields"]["BROADCAST_OPTION"] = [
        "start", "start-id"
    ]
    project = make_project(g.vars.keys(), g.lists.keys(), g)
    project["targets"][0]["broadcasts"]["start-id"] = "start"
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), project)
    return path


def execute(binary: Path, source: Path, jit: str, mode: str):
    env = {
        k: v for k, v in os.environ.items() if not k.startswith("XYO_")
    }
    env.update(
        SDL_VIDEODRIVER="offscreen",
        SDL_AUDIODRIVER="dummy",
        XYO_JIT_MODE=jit,
        XYO_JIT_ASYNC="1",
        XYO_JIT_ASYNC_PREFETCH=mode,
        XYO_JIT_ASYNC_PREFETCH_LOG="1",
        XYO_EXIT_ON_DONE="1",
        XYO_DUMPVARS="json",
        XYO_PAR="0",
    )
    completed = subprocess.run(
        [str(binary.resolve()), "play", str(source.resolve())],
        env=env, cwd=ROOT, capture_output=True, text=True, timeout=90,
    )
    if completed.returncode:
        raise AssertionError(
            f"jit={jit} prefetch={mode}: exit={completed.returncode}\n"
            + completed.stderr[-1800:]
        )
    start = completed.stdout.find("{")
    if start < 0:
        raise AssertionError(
            "missing state: " + completed.stdout[-800:]
        )
    state, _ = json.JSONDecoder().raw_decode(completed.stdout[start:])
    return state["Bench"], completed.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument(
        "--out", type=Path, default=ROOT / "build/async-jit/prefetch-gui"
    )
    args = parser.parse_args()
    source = create_fixture(args.out / "prefetch-warp-list.sb3")
    baseline, _ = execute(args.native, source, "off", "0")
    assert baseline["sum"] == "2000", baseline["sum"]
    assert baseline["done"] == "2", baseline["done"]
    assert baseline["start"] == "1", baseline["start"]
    assert baseline["RAM"] == ["7"] * 2000, baseline["RAM"][:20]
    for mode in ("0", "1", "2"):
        value, log = execute(args.native, source, "hotspot", mode)
        assert value == baseline, (
            mode, value["sum"], value["done"], value["start"]
        )
        if mode != "0" and "[jit-prefetch] " not in log:
            raise AssertionError(
                "prefetch was not selected: " + log[-1300:]
            )
        print(f"PASS async prefetch mode={mode}: exactly-once warp RAM and suffix")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
