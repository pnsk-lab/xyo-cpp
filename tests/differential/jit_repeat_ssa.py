#!/usr/bin/env python3
"""Parity for guarded integer SSA repeat loops and warp polling."""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, add, sub, make_project, num,
    text, var, write_sb3,
)


def fixture(path: Path, warp: bool):
    cases = [
        (num(0), num(5), 1),
        (num(1), num(0), 1),
        (num(2), num(1), 1),
        (num(63), num(0), 1),
        (num(64), num(0), 1),
        (num(65), num(0), 1),
        (num(129), num(-5), -2),
        (num(500), text("8"), 3),
        (num(128), text("abc"), 2),
        (num(130), text("1.25"), -1),
        (num(64), text(" "), 1),
        (num(64), num(281474976710655), 32767),
        (num(64), num(-281474976710654), -32767),
        (num(130), num(10), 65536),
        (num(129), num(0), 65537),
        (num(125), num(0), 0),
        (num(2.5), num(2), 1),
        (num(-2), num(3), 1),
        (num(20000), num(0), 1),
        (num(128), text("NaN"), 1),
        (num(128), text("Infinity"), 1),
    ]
    names = [f"case_{i}" for i in range(len(cases))]
    g = Gen(["acc", "dynamic", "done", *names], [])
    body = []
    for i, (times, initial, step) in enumerate(cases):
        update = (S_set("acc", add(var("acc"), num(step))) if i % 4 == 0
                  else S_set("acc", sub(var("acc"), num(-step))) if i % 4 == 1
                  else S_set("acc", add(num(step), var("acc"))) if i % 4 == 2
                  else S_change("acc", num(step)))
        body.extend([
            S_set("acc", initial),
            S_repeat(times, [update]),
            S_set(names[i], var("acc")),
        ])
    # Dynamic trip count and non-literal body are intentionally not specialized.
    body.extend([
        S_set("acc", num(7)), S_set("dynamic", num(130)),
        S_repeat(var("dynamic"), [S_change("acc", num(2))]),
        S_set("done", num(1)),
    ])
    g.define("run", [], body, warp=warp)
    g.script([S_call("run")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return [*names, "acc", "done"]


def run(binary: Path, path: Path, mode: str, enabled: bool, fields: list[str],
        extra_env: dict[str, str] | None = None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({"XYO_DUMPVARS": "json", "XYO_PAR": "0", "XYO_GPU": "0",
                "XYO_JIT_REPEAT_SSA": "1" if enabled else "0"})
    env.update(extra_env or {})
    proc = subprocess.run([str(binary), "run", str(path), "--jit", mode,
                           "--frames", "1000"], cwd=ROOT, env=env,
                          text=True, capture_output=True, timeout=100)
    if proc.returncode:
        raise RuntimeError((mode, enabled, proc.returncode, proc.stderr[-1000:]))
    output = proc.stdout
    data = json.loads(output[output.index("{"):output.rindex("}") + 1])["Bench"]
    return {key: data[key] for key in fields}


def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/xyo"
    out = ROOT / "build/llvm-scev-loop"
    out.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        project = out / ("repeat-ssa-warp.sb3" if warp else "repeat-ssa-nonwarp.sb3")
        keys = fixture(project, warp)
        baseline = run(binary, project, "off", False, keys)
        assert baseline["done"] == "1", baseline
        for mode, enabled, extra in [
            ("eager", True, {}), ("eager", False, {}),
            ("hotspot", True, {}), ("hotspot", False, {}),
            ("eager", True, {"XYO_JIT_OPT_LEVEL": "2"}),
        ]:
            result = run(binary, project, mode, enabled, keys, extra)
            if result != baseline:
                diff = {key: (baseline[key], result[key]) for key in keys
                        if result[key] != baseline[key]}
                raise AssertionError((warp, mode, enabled, diff))
            print("PASS", "warp" if warp else "nonwarp", mode,
                  "ssa" if enabled else "fallback", extra, flush=True)


if __name__ == "__main__":
    main()
