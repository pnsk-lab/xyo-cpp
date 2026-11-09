#!/usr/bin/env python3
"""Differential and optimized-IR coverage for duplicate pure scalar operands.

Only arithmetic operations with two *direct* reporters for the same resolved
scalar may share a boxed value and its Scratch number coercion. No conclusion
about the scalar's global type is made.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_change, S_repeat, S_set, add, div, mul, num,
    op, sub, text, var, make_project, write_sb3,
)

VALUES = (" 12.5 ", "NaN", " ", "Infinity", "-Infinity",
          "-0", "0x10", "0", "abc", "1e3", "", "4.5")


def fixture(path: Path) -> list[str]:
    outputs = [f"out_{i}" for i in range(len(VALUES))]
    g = Gen(["x", "sum", "temp", "stamp", "done", *outputs], [])
    script = []
    # Timer reporter prevents loop promotion; this explicitly checks the
    # non-promoted, generic xyo_to_num path for dynamically tagged values.
    body = [
        S_set("stamp", op("sensing_timer")),
        S_set("temp", add(var("x"), var("x"))),
        S_change("sum", var("temp")),
        S_change("sum", mul(var("x"), var("x"))),
        S_change("sum", sub(var("x"), var("x"))),
        S_change("sum", div(var("x"), var("x"))),
        S_change("x", var("x")),  # direct self-delta, numeric after write
    ]
    for i, literal in enumerate(VALUES):
        script.extend([
            S_set("x", text(literal)),
            S_set("sum", num(0)),
            S_repeat(num(70), body),
            S_set(outputs[i], var("sum")),
        ])
    # Also cover the native numeric payload, not just numeric strings.
    script.extend([
        S_set("x", num(17)),
        S_set("temp", add(var("x"), var("x"))),
        S_change("sum", var("temp")),
        S_set("done", num(1)),
    ])
    g.script(script)
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return ["x", "sum", "temp", "done", *outputs]


def execute(binary: Path, path: Path, keys: list[str], mode: str,
            tier: str, optimized_ir: bool = False) -> tuple[dict, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_PAR="0", XYO_GPU="0", XYO_DUMPVARS="json",
        XYO_JIT_DUPLICATE_SCALAR_NUM=mode,
        XYO_JIT_DUPLICATE_SCALAR_STATS="1",
        XYO_JIT_DUMP2="1" if optimized_ir else "0",
    )
    if not optimized_ir:
        del env["XYO_JIT_DUMP2"]
    result = subprocess.run(
        [str(binary), "run", str(path), "--jit", tier, "--frames", "800"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=110,
    )
    if result.returncode:
        raise RuntimeError((tier, mode, result.returncode,
                            result.stderr[-1600:]))
    stdout = result.stdout
    var_map = json.loads(stdout[stdout.index("{"):stdout.rindex("}") + 1])
    values = var_map["Bench"]
    return {key: values[key] for key in keys}, result.stderr


def main() -> None:
    binary = (Path(sys.argv[1]) if len(sys.argv) > 1
              else ROOT / "build/xyo").resolve()
    target = ROOT / "build/duplicate-scalar"
    target.mkdir(parents=True, exist_ok=True)
    path = target / "mixed.sb3"
    keys = fixture(path)
    expected, _ = execute(binary, path, keys, "0", "off")
    assert expected["done"] == "1"
    ir_calls = {}
    for tier, mode, dump in [
        ("eager", "0", True),
        ("eager", "1", True),
        ("hotspot", "0", False),
        ("hotspot", "1", False),
        ("eager", "1", False),
    ]:
        actual, log = execute(binary, path, keys, mode, tier, dump)
        if actual != expected:
            differences = {k: (expected[k], actual[k]) for k in keys
                           if actual[k] != expected[k]}
            raise AssertionError((tier, mode, differences))
        if tier == "eager" and dump:
            counts = [
                int(x) for x in
                re.findall(r"^\[jit-duplicate-scalar\] \S+ pairs=(\d+) self_changes=\d+$",
                           log, re.MULTILINE)
            ]
            if mode == "1" and (not counts or sum(counts) < 4):
                raise AssertionError(("expected deduplicated pairs", counts))
            if mode == "1" and not any(
                int(value) > 0 for value in re.findall(
                    r"^\[jit-duplicate-scalar\] \S+ pairs=\d+ self_changes=(\d+)$",
                    log, re.MULTILINE)
            ):
                raise AssertionError("no self-change deduplication observed")
            ir_calls[mode] = len(
                re.findall(r"\bcall double @xyo_to_num\(", log))
        print("PASS", tier, mode,
              f"optimized-IR-calls={ir_calls.get(mode)}" if dump else "",
              flush=True)
    if not ir_calls["1"] < ir_calls["0"]:
        raise AssertionError(
            ("optimized IR did not reduce Scratch number-conversion call sites",
             ir_calls))
    print("Optimized IR conversion call sites", ir_calls, flush=True)


if __name__ == "__main__":
    main()
