#!/usr/bin/env python3
"""A copied integer SSA chunk must not normalize an unchanged -0 variable."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (Gen, S_call, S_change, S_if, S_repeat,
                           S_set, div, eq, num, var, make_project, write_sb3)


def fixture(path):
    g = Gen(["x", "acc", "reciprocal", "done"], [])
    g.define("run", [], [
        S_set("x", div(num(0), num(-1))),
        S_set("acc", num(0)),
        S_repeat(num(65), [
            S_if(eq(var("x"), num(0)), [S_change("acc", num(1))])
        ]),
        S_set("reciprocal", div(num(1), var("x"))),
        S_set("done", num(1))
    ], warp=True)
    g.script([S_call("run")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))


def run(binary, path, mode):
    env = {k: v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0", XYO_PARFOR="0")
    if mode == "disabled":
        env["XYO_JIT_CONDITIONAL_SSA"] = "0"
    elif mode == "signed":
        env["XYO_JIT_CONDITIONAL_SSA"] = "1"
        env["XYO_JIT_CONDITIONAL_UREM"] = "0"
    elif mode == "enabled":
        env["XYO_JIT_CONDITIONAL_SSA"] = "1"
        env["XYO_JIT_CONDITIONAL_UREM"] = "1"
    jit = "off" if mode == "interpreter" else "eager"
    p = subprocess.run([str(binary), "run", str(path), "--jit", jit,
                        "--frames", "1000"], env=env, cwd=ROOT,
                       capture_output=True, text=True, timeout=60)
    if p.returncode:
        raise AssertionError(f"{mode}: {p.stderr[-1200:]}")
    return json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}")+1])["Bench"]


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/xyo").resolve()
    path = ROOT / "build/math002-negative-zero.sb3"
    fixture(path)
    readings = {mode: run(binary, path, mode)
                for mode in ["interpreter", "disabled", "signed", "enabled"]}
    expected = readings["interpreter"]
    assert expected["done"] == "1", expected
    assert expected["acc"] == "65", expected
    # If -0 was lost at the SSA checkpoint, 1/x would be +Infinity instead.
    assert expected["reciprocal"] == "-Infinity", expected
    assert all(v == expected for v in readings.values()), readings
    print("PASS signed-zero guard: interpreter, baseline, signed and unsigned SSA")


if __name__ == "__main__":
    main()
