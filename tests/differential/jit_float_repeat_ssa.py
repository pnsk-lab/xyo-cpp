#!/usr/bin/env python3
"""Pure floating N-body SSA vs Scratch interpreter and unmodified JIT.

The real ScratchBench recurrence is generated independently from Scratch
source, including singularities, negative zero, NaN/infinity and 64-tick
scheduler boundaries. Fast and slow paths must agree on every output.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set,
    add, div, mul, num, op, sub, text, var,
    make_project, write_sb3,
)


def sqrt(x):
    return op("operator_mathop", {"OPERATOR": "sqrt"}, NUM=x)


def body():
    return [
        S_set("x", sub(num(0), var("a"))),
        S_set("y", sub(num(0), var("b"))),
        S_set("z", sqrt(add(mul(var("x"), var("x")),
                            mul(var("y"), var("y"))))),
        S_set("acc", div(num(5000),
                         mul(mul(var("z"), var("z")), var("z")))),
        S_change("c", mul(mul(var("x"), var("acc")), num(0.01))),
        S_change("d", mul(mul(var("y"), var("acc")), num(0.01))),
        S_change("a", mul(var("c"), num(0.01))),
        S_change("b", mul(var("d"), num(0.01))),
    ]


def project(path, warp):
    negative_zero = div(num(0), num(-1))
    nan = div(num(0), num(0))
    inf = div(num(1), num(0))
    cases = [
        (0, num(100), num(0), num(0), num(1.2)),
        (1, num(100), num(0), num(0), num(1.2)),
        (2, num(100), num(0), num(0), num(1.2)),
        (63, num(100), num(0), num(0), num(1.2)),
        (64, num(100), num(0), num(0), num(1.2)),
        (65, num(100), num(0), num(0), num(1.2)),
        (127, num(100), num(0), num(0), num(1.2)),
        (128, num(100), num(0), num(0), num(1.2)),
        (129, num(100), num(0), num(0), num(1.2)),
        (255, num(100), num(0), num(0), num(1.2)),
        (512, num(100), num(0), num(0), num(1.2)),
        (2000, num(100), num(0), num(0), num(1.2)),
        (65, num(0), num(0), num(0), num(0)),  # 0/0 and 0*inf
        (65, negative_zero, num(0), num(0), num(0)),
        (65, num(-0.001), num(0.1), num(0), num(1.2)),
        (65, num(1e308), num(1e308), num(0), num(0)),
        (65, num(1e-300), num(0), num(0), num(1)),
        (65, nan, num(2), num(0), num(1)),
        (65, inf, num(2), num(0), num(1)),
        (65, text("100"), num(0), num(0), num(1.2)),
        (65, text("abc"), num(0), num(0), num(1.2)),
        (65, num(100), text("-0"), num(0), num(1.2)),
        (65, num(100), num(0), text("1"), num(1.2)),
    ]
    fields = ("a", "b", "c", "d", "x", "y", "z", "acc")
    outputs = [
        f"case_{i}_{v}" for i in range(len(cases)) for v in fields
    ]
    outputs += [f"case_{i}_inverse" for i in range(len(cases))]
    gen = Gen([*fields, "done", *outputs], [])
    statements = []
    for i, (count, a, b, c, d) in enumerate(cases):
        statements.extend([
            S_set("a", a), S_set("b", b), S_set("c", c), S_set("d", d),
            S_set("x", num(0)), S_set("y", num(0)),
            S_set("z", num(0)), S_set("acc", num(0)),
            S_repeat(num(count), body()),
        ])
        statements.extend(
            S_set(f"case_{i}_{field}", var(field)) for field in fields
        )
        statements.append(
            S_set(f"case_{i}_inverse", div(num(1), var("a")))
        )
    statements.append(S_set("done", num(1)))
    gen.define("run", [], statements, warp=warp)
    gen.script([S_call("run")])
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return outputs + ["done"]


def rollback_project(path, warp):
    # The first 65 iterations have valid f64 math; the next sqrt returns NaN.
    # The optimized loop must preserve 65 prior updates and execute the
    # rejected iteration only once via the ordinary JIT body.
    values = ["i", "x", "a", "b", "c", "inv_x", "done"]
    gen = Gen(values, [])
    statements = [
        S_set("i", num(0)), S_set("x", num(0)),
        S_set("a", num(0)), S_set("b", num(0)),
        S_set("c", num(0)),
        S_repeat(num(130), [
            S_set("x", sqrt(sub(num(64), var("i")))),
            S_change("i", num(1)),
            S_set("a", add(var("x"), var("i"))),
            S_change("b", var("a")),
            S_set("c", mul(var("x"), num(0.5))),
        ]),
        S_set("inv_x", div(num(1), var("x"))),
        S_set("done", num(1)),
    ]
    gen.define("run", [], statements, warp=warp)
    gen.script([S_call("run")])
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return values


def execute(binary, sb3, keys, jit, float_ssa):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0",
        XYO_PARFOR="0",
        XYO_JIT_NUMERIC_REPEAT_FLOAT=str(int(float_ssa)),
    )
    proc = subprocess.run(
        [str(binary), "run", str(sb3), "--jit", jit, "--frames", "40000"],
        env=env, cwd=ROOT, capture_output=True, text=True, timeout=120,
    )
    if proc.returncode:
        raise AssertionError(f"{sb3.name} {jit} {float_ssa}: "
                             + proc.stderr[-2000:])
    values = json.loads(
        proc.stdout[proc.stdout.index("{"):proc.stdout.rindex("}")+1]
    )["Bench"]
    return {key:values[key] for key in keys}


def main():
    binary = (Path(sys.argv[1]).resolve() if len(sys.argv) > 1
              else (ROOT / "build/xyo").resolve())
    out = ROOT / "build/math005-differential"
    out.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        sb3 = out / ("warp.sb3" if warp else "nonwarp.sb3")
        keys = project(sb3, warp)
        expected = execute(binary, sb3, keys, "off", False)
        assert expected["done"] == "1"
        for jit, on in [
            ("eager", False), ("eager", True), ("hotspot", True),
        ]:
            got = execute(binary, sb3, keys, jit, on)
            differences = {
                k: (expected[k], got[k]) for k in keys
                if expected[k] != got[k]
            }
            if differences:
                raise AssertionError(
                    f"warp={warp}, jit={jit}, float_ssa={on}: {differences}"
                )
            print(
                f"PASS warp={warp} jit={jit} float_ssa={on} "
                f"observed={len(keys)}", flush=True,
            )

        rollback = out / ("rollback_warp.sb3" if warp
                          else "rollback_nonwarp.sb3")
        rollback_keys = rollback_project(rollback, warp)
        reference = execute(binary, rollback, rollback_keys, "off", False)
        for jit in ("eager", "hotspot"):
            optimized = execute(binary, rollback, rollback_keys, jit, True)
            assert optimized == reference, (warp, jit, reference, optimized)
            print(f"PASS rollback warp={warp} jit={jit} "
                  f"observed={len(rollback_keys)}", flush=True)


if __name__ == "__main__":
    main()
