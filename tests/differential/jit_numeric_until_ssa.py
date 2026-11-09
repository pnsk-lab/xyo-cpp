#!/usr/bin/env python3
"""Pure numeric repeat-until differential: interpreter vs baseline vs SSA.

Validates mixed f64 exactness, NaN rollback, signed zero, and 64-tick yield
boundaries. Cases are generated independently from ScratchBench.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_set, S_until, add, div, lt, gt, mul, num,
    op, sub, text, var, make_project, write_sb3,
)

def ge(a, b):
    return op("operator_not", OPERAND=lt(a, b))

def either(a, b):
    return op("operator_or", OPERAND1=a, OPERAND2=b)

def body():
    return [
        S_set("z", add(sub(mul(var("x"), var("x")),
                            mul(var("y"), var("y"))), var("e"))),
        S_set("y", add(mul(mul(num(2), var("x")), var("y")),
                       var("f"))),
        S_set("x", var("z")),
        S_change("n", num(1)),
    ]

def fixture(path, warp):
    inf = div(num(1), num(0))
    nan = div(num(0), num(0))
    nz = div(num(0), num(-1))
    cases = [
        (0, num(0), num(0), num(0), num(0)),
        (1, num(0), num(0), num(0), num(0)),
        (2, num(0), num(0), num(0), num(0)),
        (63, num(0), num(0), num(0), num(0)),
        (64, num(0), num(0), num(0), num(0)),
        (65, num(0), num(0), num(0), num(0)),
        (129, num(0), num(0), num(0), num(0)),
        (64, num(0), num(0), nz, num(0)),
        (65, nz, num(0), num(0), num(0)),
        (20, num(-0.01), num(0.1), num(-0.8), num(0.156)),
        (20, num(0), num(0), num(-0.5), num(0.45)),
        (20, num(0), num(0), num(1.01), num(1.01)),
        (20, num(0), num(0), num(3), num(0)),
        (20, num(0), num(0), inf, num(0)),
        (20, num(0), num(0), nan, num(0)),
        (20, num(0), num(0), num(0), inf),
        (20, num(0), num(0), num(0), nan),
        (20, text("0"), num(0), num(0), num(0)),
        (20, text("abc"), num(0), num(0), num(0)),
        (20, num(0), text("false"), num(0), num(0)),
        (20, num(0), num(0), text("1"), num(0)),
        (20, num(0), num(0), num(1e308), num(1e308)),
        (20, num(0), num(0), num(-1e308), num(1e308)),
        (20, num(0), num(0), num(0.01), num(0.02)),
    ]
    output_names = [
        f"result_{i}_{key}"
        for i in range(len(cases))
        for key in ("x", "y", "z", "n", "rx", "ry")
    ]
    g = Gen(["x", "y", "z", "n", "e", "f", "done", *output_names], [])
    statements = []
    for i, (limit, x, y, e, f) in enumerate(cases):
        statements.extend([
            S_set("x", x), S_set("y", y), S_set("z", num(0)),
            S_set("e", e), S_set("f", f), S_set("n", num(0)),
            S_until(either(ge(var("n"), num(limit)),
                           gt(add(mul(var("x"),var("x")),
                                  mul(var("y"),var("y"))), num(4))),
                    body()),
        ])
        statements.extend(
            S_set(f"result_{i}_{name}", var(name))
            for name in ("x", "y", "z", "n")
        )
        statements.extend([
            S_set(f"result_{i}_rx", div(num(1), var("x"))),
            S_set(f"result_{i}_ry", div(num(1), var("y"))),
        ])
    statements.append(S_set("done", num(1)))
    g.define("run", [], statements, warp=warp)
    g.script([S_call("run")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return output_names+["done"]

def run(binary, project, keys, mode, until, fast):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_JIT_NUMERIC_UNTIL_SSA": str(int(until)),
        "XYO_JIT_NUMERIC_UNTIL_FAST": str(int(fast)),
    })
    p=subprocess.run(
        [str(binary), "run", str(project), "--jit", mode,
         "--frames","30000"],env=env,cwd=ROOT,capture_output=True,
        text=True,timeout=90,
    )
    if p.returncode:
        raise RuntimeError(f"{project} {mode} until={until} fast={fast}: "
                           f"{p.stderr[-1500:]}")
    result=json.loads(p.stdout[p.stdout.index("{"):
                              p.stdout.rindex("}")+1])["Bench"]
    return {key:result[key] for key in keys}

def main():
    binary=Path(sys.argv[1] if len(sys.argv)>1 else ROOT/"build/xyo").resolve()
    out=ROOT/"build/math004-differential"
    out.mkdir(parents=True,exist_ok=True)
    for warp in (True,False):
        project=out/("warp.sb3" if warp else "nonwarp.sb3")
        keys=fixture(project,warp)
        reference=run(binary,project,keys,"off",False,False)
        assert reference["done"]=="1"
        for mode,until,fast in [
            ("eager",False,False), ("eager",True,False),
            ("eager",True,True), ("hotspot",True,True),
        ]:
            candidate=run(binary,project,keys,mode,until,fast)
            mismatch={k:(reference[k],candidate[k]) for k in keys
                      if reference[k]!=candidate[k]}
            if mismatch:
                raise AssertionError(
                    f"warp={warp} mode={mode} until={until} fast={fast}:"
                    f"{mismatch}")
            print("PASS",warp,mode,until,fast,len(keys),flush=True)

if __name__=="__main__":
    main()
