#!/usr/bin/env python3
"""Exact V8 fdlibm exp(k/10) JIT-table parity, all 51 entries and edges.

Check interpreter, regular libm JIT, table JIT, Eager and HotSpot, as well
as warp and non-warp. Values one ULP away must take the libm fallback.
"""
import json
import math
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, add, div, mul,
    num, op, text, var, make_project, write_sb3
)


def exp_op(x):
    return op("operator_mathop", {"OPERATOR":"e ^"}, NUM=x)


def fixture(path: Path, warp: bool):
    cases = [(f"table_{k}", num(k / 10.0)) for k in range(51)]
    for k in (0, 1, 2, 3, 10, 20, 29, 49, 50):
        base = k / 10.0
        cases.extend([
            (f"low_{k}", num(math.nextafter(base, -math.inf))),
            (f"high_{k}", num(math.nextafter(base, math.inf))),
        ])
    cases.extend([
        ("negative", num(-0.05)),
        ("above", num(5.05)),
        ("other", num(0.07)),
        ("negative_zero", div(num(0), num(-1))),
        ("positive_inf", div(num(1), num(0))),
        ("negative_inf", div(num(-1), num(0))),
        ("nan", div(num(0), num(0))),
        ("numeric_string", text("0.05")),
        ("nonnumeric_string", text("hello")),
        ("large", num(1e308)),
        ("underflow", num(-1e308)),
    ])
    variables = ["x", "y", "sum", "done"]
    outputs = []
    for label, _ in cases:
        outputs.extend([f"result_{label}", f"acc_{label}",
                        f"product_{label}"])
    gen = Gen([*variables, *outputs], [])
    statements = []
    for label, initial in cases:
        statements.extend([
            S_set("x", initial),
            S_set("sum", num(0)),
            # >=2 scalar statements with a MathOp: eligible for float SSA.
            S_repeat(num(65), [
                S_set("y", exp_op(var("x"))),
                S_change("sum", div(var("y"), num(64))),
            ]),
            S_set(f"result_{label}", var("y")),
            S_set(f"acc_{label}", var("sum")),
            S_set(f"product_{label}", mul(var("y"), num(1e10))),
        ])
    statements.append(S_set("done", num(1)))
    gen.define("run", [], statements, warp=warp)
    gen.script([S_call("run")])
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return outputs + ["done"]


def run(binary: Path, project: Path, keys, mode: str, table: bool):
    env = {k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS":"json",
        "XYO_GPU":"0", "XYO_PAR":"0", "XYO_PARFOR":"0",
        "XYO_JIT_NUMERIC_REPEAT_FLOAT":"1",
        "XYO_JIT_EXP_TABLE":"1" if table else "0",
    })
    proc = subprocess.run(
        [str(binary), "run", str(project), "--jit", mode,
         "--frames","30000"], cwd=ROOT, env=env,
        capture_output=True, text=True, timeout=120,
    )
    if proc.returncode:
        raise RuntimeError(f"mode={mode},table={table},project={project}: "
                           f"{proc.stderr[-1500:]}")
    result = json.loads(proc.stdout[
        proc.stdout.index("{"):proc.stdout.rindex("}")+1])["Bench"]
    return {k:result[k] for k in keys}


def main():
    binary = (Path(sys.argv[1]).resolve() if len(sys.argv)>1 else
              (ROOT/"build/math008-exp-xyo").resolve())
    out = ROOT/"build/math008-exp-differential"
    out.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        project = out/("warp.sb3" if warp else "nonwarp.sb3")
        keys = fixture(project, warp)
        expected = run(binary, project, keys, "off", False)
        assert expected["done"]=="1"
        for mode, enabled in [
            ("eager",False),("eager",True),("hotspot",True)
        ]:
            candidate = run(binary, project, keys, mode, enabled)
            differences = {
                key:(expected[key],candidate[key]) for key in keys
                if expected[key] != candidate[key]
            }
            if differences:
                raise AssertionError(
                    f"warp={warp} mode={mode} table={enabled}: "
                    f"{differences}")
            print(f"PASS warp={warp} mode={mode} table={enabled}"
                  f" outputs={len(keys)}", flush=True)


if __name__=="__main__":
    main()
