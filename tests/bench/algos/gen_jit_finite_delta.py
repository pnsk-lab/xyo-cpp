#!/usr/bin/env python3
"""Generate an ordinary Scratch project exercising finite SSA fact propagation.

The workload uses general operator_mod and data_changevariableby blocks. It is
not altered in the JIT; integer/fraction/negative-zero edge cases belong in
unit tests. This benchmark isolates finite numeric delta propagation.
"""
from pathlib import Path
from gen_algo_bench import Gen, S_call, S_change, S_repeat, S_set, arg, driver, make_project, mod, num, var, add, mul, write_sb3

def generate(n: int, kind: str = "direct"):
    g = Gen(["i", "sum", "t0", "t1", "check"], ["times"])
    delta = mod(var("i"), num(7))
    if kind == "affine":
        delta = add(mul(delta, num(31)), num(1))
    elif kind != "direct":
        raise ValueError(kind)
    g.define("finite delta %s", ["n"], [
        S_set("i", num(0)),
        S_set("sum", num(0)),
        S_repeat(arg("n"), [
            S_change("i", num(1)),
            S_change("sum", delta),
        ])
    ], warp=True)
    g.define("verify finite", [], [S_set("check", var("sum"))])
    g.script(driver(
        [], [S_call("finite delta %s", [num(11)])],
        S_call("finite delta %s", [num(n)]),
        S_call("verify finite"),
    ))
    expected = (n // 7) * 21 + (n % 7) * (n % 7 + 1) // 2
    if kind == "affine":
        expected = expected * 31 + n
    return make_project(g.vars.keys(), g.lists.keys(), g), expected

if __name__ == "__main__":
    import argparse
    p=argparse.ArgumentParser()
    p.add_argument("--out",type=Path,default=Path("build/v1-jit-finite-delta-project.sb3"))
    p.add_argument("--iterations",type=int,default=300000)
    p.add_argument("--kind",choices=["direct","affine"],default="direct")
    a=p.parse_args()
    project, expected=generate(a.iterations,a.kind)
    write_sb3(str(a.out),project)
    print("project:",a.out,"iterations:",a.iterations,"expected:",expected)
