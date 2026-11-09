#!/usr/bin/env python3
"""Pure numeric repeat: interpreter, normal JIT, f64 SSA and mixed i64/f64 SSA."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (Gen, S_call, S_change, S_repeat, S_set,
                            add, div, mod, mul, num, op, sub, text, var,
                            make_project, write_sb3)


def sqrt(x):
    return op("operator_mathop", {"OPERATOR": "sqrt"}, NUM=x)


def arith_body():
    return [
        S_change("i", num(1)),
        S_set("a", mod(add(mul(var("a"), num(31)), var("i")),
                       num(1000003))),
        S_set("b", sub(add(var("b"), div(var("a"), num(4))),
                       mul(var("i"), num(0.5)))),
    ]


def nbody_body():
    return [
        S_set("x", sub(num(0), var("a"))),
        S_set("y", sub(num(0), var("b"))),
        S_set("r", sqrt(add(mul(var("x"), var("x")),
                            mul(var("y"), var("y"))))),
        S_set("c", div(mul(var("x"), num(2)), add(var("r"), num(1)))),
        S_set("d", div(mul(var("y"), num(3)), add(var("r"), num(1)))),
        S_change("a", mul(var("c"), num(0.125))),
        S_change("b", mul(var("d"), num(0.125))),
    ]


def fixture(path, warp):
    negzero = div(num(0), num(-1))
    nan = div(num(0), num(0))
    inf = div(num(1), num(0))
    arith_cases = [
        (0, num(0), num(1), num(0)),
        (1, num(0), num(1), num(0)),
        (63, num(0), num(1), num(0)),
        (64, num(0), num(1), num(0)),
        (65, num(0), num(1), num(0)),
        (129, num(0), num(1), num(0)),
        (257, num(0), num(1), num(0)),
        (129, num(999), num(1000003), num(10)),
        (65, num(0), num(2097152), num(100)),
        (65, num(0), num(2097153), num(100)),
        (65, num(0), num(-1), num(0)),
        (65, num(0), num(1.25), num(0)),
        (65, num(0), negzero, num(0)),
        (65, num(0), text("15"), num(0)),
        (65, num(0), text("abc"), num(0)),
        (65, num(0), nan, num(0)),
        (65, num(0), inf, num(0)),
        (65, num(0), num(3), inf),
        (65, num(0), num(3), nan),
        (65, num(0), num(3), text("99")),
        (65, num(2000000), num(1), num(0)),
        (65, num(2097153), num(1), num(0)),
        (65, num(-1), num(1), num(0)),
        (65, negzero, num(1), num(0)),
    ]
    nbody_cases = [
        (0, num(2), num(-1)),
        (1, num(2), num(-1)),
        (63, num(2), num(-1)),
        (65, num(2), num(-1)),
        (129, num(2), num(-1)),
        (65, num(0), num(0)),
        (65, negzero, num(0)),
        (65, text("2"), num(0)),
        (65, text("abc"), num(0)),
        (65, nan, num(0)),
        (65, inf, num(0)),
        (65, num(1e300), num(-1e300)),
        (65, num(-10.25), num(20.75)),
    ]
    outputs = [f"out_{i}_{key}" for i in range(len(arith_cases))
               for key in ("i", "a", "b", "inv")]
    outputs += [f"body_{i}_{key}" for i in range(len(nbody_cases))
                for key in ("a", "b", "c", "d", "r", "x")]
    outputs += ["negative_zero_constant", "generated_nan", "after_generated_nan"]
    g = Gen(["i", "a", "b", "c", "d", "r", "x", "y", "done",
             *outputs], [])
    statements = []
    for j, (count, i, a, b) in enumerate(arith_cases):
        statements += [
            S_set("i", i), S_set("a", a), S_set("b", b),
            S_repeat(num(count), arith_body()),
            S_set(f"out_{j}_i", var("i")),
            S_set(f"out_{j}_a", var("a")),
            S_set(f"out_{j}_b", var("b")),
            S_set(f"out_{j}_inv", div(num(1), var("a"))),
        ]
    for j, (count, a, b) in enumerate(nbody_cases):
        statements += [
            S_set("a", a), S_set("b", b),
            S_set("c", num(0)), S_set("d", num(0)),
            S_set("r", num(0)), S_set("x", num(0)), S_set("y", num(0)),
            S_repeat(num(count), nbody_body()),
        ]
        statements += [
            S_set(f"body_{j}_{key}", var(key))
            for key in ("a", "b", "c", "d", "r", "x")
        ]
    # A negative-zero operand must not be converted to a positive integer
    # literal by a supposed exact integer SSA recurrence.
    statements += [
        S_set("i", num(0)), S_set("a", num(1)),
        S_repeat(num(65), [
            S_change("i", num(1)),
            S_set("a", mod(mul(var("a"), num(-0.0)), num(1000003))),
        ]),
        S_set("negative_zero_constant", div(num(1), var("a"))),
    ]
    # NaN generated *inside* an eligible numeric loop must remain boxed
    # NaN for storage, yet convert back to zero in subsequent arithmetic.
    statements += [
        S_set("a", num(1)), S_set("b", num(0)),
        S_repeat(num(65), [
            S_set("b", sub(div(num(1), num(0)), div(num(1), num(0)))),
            S_change("a", var("b")),
        ]),
        S_set("generated_nan", var("b")),
        S_set("after_generated_nan", var("a")),
    ]
    statements.append(S_set("done", num(1)))
    g.define("run", [], statements, warp=warp)
    g.script([S_call("run")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return outputs + ["done"]


def run(binary, project, keys, jit, ssa, integers):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0", XYO_PARFOR="0",
        XYO_JIT_NUMERIC_REPEAT_SSA=str(int(ssa)),
        XYO_JIT_NUMERIC_REPEAT_INT=str(int(integers)),
    )
    proc = subprocess.run(
        [str(binary), "run", str(project), "--jit", jit, "--frames", "2000"],
        env=env, cwd=ROOT, capture_output=True, text=True, timeout=100)
    if proc.returncode:
        raise AssertionError(f"{project.name} {jit} {ssa} {integers}: "
                             + proc.stderr[-1200:])
    content = json.loads(proc.stdout[proc.stdout.index("{"):
                                     proc.stdout.rindex("}") + 1])
    return {k: content["Bench"][k] for k in keys}


def main():
    binary = (Path(sys.argv[1]).resolve() if len(sys.argv)>1
              else (ROOT/"build/xyo"))
    out = ROOT/"build/math003-differential"
    out.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        project = out/("warp.sb3" if warp else "nonwarp.sb3")
        keys = fixture(project, warp)
        expected = run(binary, project, keys, "off", False, False)
        assert expected["done"] == "1"
        for jit, ssa, integers in [
            ("eager", False, False),
            ("eager", True, False),
            ("eager", True, True),
            ("hotspot", True, True),
        ]:
            actual = run(binary, project, keys, jit, ssa, integers)
            mismatches = {key: (expected[key], actual[key])
                          for key in keys if expected[key] != actual[key]}
            if mismatches:
                raise AssertionError(
                    f"{warp} {jit} {ssa} {integers}: {mismatches}")
            print(f"PASS warp={warp} jit={jit} ssa={ssa} "
                  f"integers={integers} outputs={len(keys)}", flush=True)


if __name__ == "__main__":
    main()
