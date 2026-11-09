#!/usr/bin/env python3
"""Same-basic-block lazy scalar read forwarding, compared with interpreter.

A mixed-type scalar occurs twice within the same arithmetic operator
(x + x). The second use must reuse the first boxed load and its guarded
numeric result, without assuming that the scalar stays numeric elsewhere.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_ifelse, S_repeat, S_set,
    add, div, eq, gt, mul, num, sub, text, var,
    make_project, write_sb3,
)

STRINGS = (" 12.5 ", "NaN", " ", "Infinity", "-Infinity", "-0",
           "0x10", "0", "hello", "1e3")


def fixture(path: Path, warp: bool) -> list[str]:
    g = Gen(["x", "sum", "temp", "check", "done"], [])
    body = []
    for literal in STRINGS:
        body.extend([
            S_set("x", text(literal)),
            S_set("temp", add(var("x"), var("x"))),
            S_change("sum", var("temp")),
            S_set("temp", mul(var("x"), var("x"))),
            S_change("sum", var("temp")),
            S_set("temp", sub(var("x"), var("x"))),
            S_change("sum", var("temp")),
            S_set("temp", div(var("x"), var("x"))),
            S_change("sum", var("temp")),
            # Both paths can write the same variable, but no stale fact
            # must cross the corresponding branch / scheduler boundary.
            S_ifelse(gt(var("sum"), num(100)),
                     [S_set("x", add(var("sum"), num(-3)))],
                     [S_set("x", text(" 2.5 "))]),
            S_change("sum", add(var("x"), var("x"))),
            S_if(eq(var("x"), var("x")),
                 [S_change("check", num(1))]),
        ])
    statements = [
        S_set("sum", num(0)),
        S_repeat(num(65), body),
        S_set("done", num(1)),
    ]
    g.define("repeated", [], statements, warp=warp)
    g.script([S_call("repeated")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return ["x", "sum", "temp", "check", "done"]


def run(binary: Path, path: Path, keys: list[str], tier: str,
        read_forward: bool, dump_ir: bool = False) -> tuple[dict, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update(
        XYO_PAR="0", XYO_GPU="0", XYO_DUMPVARS="json",
        XYO_JIT_LAZY_TYPED_SSA="1", XYO_JIT_TYPE_FLOW="1",
        XYO_JIT_LOCAL_READ_FORWARD="1" if read_forward else "0",
        XYO_JIT_LOCAL_READ_STATS="1",
        XYO_JIT_DUMP="1" if dump_ir else "0",
    )
    if not dump_ir:
        env.pop("XYO_JIT_DUMP")
    p = subprocess.run(
        [str(binary), "run", str(path), "--jit", tier, "--frames", "1000"],
        env=env, cwd=ROOT, capture_output=True, text=True, timeout=110,
    )
    if p.returncode:
        raise RuntimeError((path.name, tier, read_forward, p.returncode,
                            p.stderr[-1100:]))
    output = p.stdout
    obj = json.loads(output[output.index("{"):output.rindex("}") + 1])["Bench"]
    return {key: obj[key] for key in keys}, p.stderr


def main() -> None:
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    out = ROOT / "build/local-read-forward"
    out.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        path = out / ("warp.sb3" if warp else "nonwarp.sb3")
        keys = fixture(path, warp)
        expected, _ = run(binary, path, keys, "off", False)
        assert expected["done"] == "1", expected
        unoptimized_loads: dict[bool, int] = {}
        for tier, enabled, dump in (
            ("eager", False, True),
            ("eager", True, True),
            ("hotspot", False, False),
            ("hotspot", True, False),
            ("eager", True, False),
        ):
            actual, log = run(binary, path, keys, tier, enabled, dump)
            if actual != expected:
                changes = {key: (expected[key], actual[key]) for key in keys
                           if actual[key] != expected[key]}
                raise AssertionError((warp, tier, enabled, changes))
            if tier == "eager" and dump:
                unoptimized_loads[enabled] = sum(
                    line.lstrip().startswith("%") and " = load i64" in line
                    for line in log.splitlines()
                )
            if enabled and tier == "eager" and dump:
                stats = [line for line in log.splitlines()
                         if line.startswith("[jit-local-read] ")]
                if not stats or not any(
                    int(line.split("reuses=")[-1]) > 0 for line in stats
                ):
                    raise AssertionError("no repeated load was reused")
            print("PASS", warp, tier, enabled, "dump" if dump else "",
                  flush=True)
        if not unoptimized_loads[True] < unoptimized_loads[False]:
            raise AssertionError(
                ("no reduction in raw LLVM i64 loads", warp,
                 unoptimized_loads))
        print("raw load i64 count", warp, unoptimized_loads, flush=True)


if __name__ == "__main__":
    main()
