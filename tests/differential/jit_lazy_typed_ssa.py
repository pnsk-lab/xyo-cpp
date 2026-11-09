#!/usr/bin/env python3
"""Differential test for general, lazy type-guarded SSA promotion.

The test mixes boxed string writes, numeric reads, control-flow merges and
scheduler yields. No particular loop shape is used to trigger a rewrite:
the ordinary Scratch stack-block lowerer emits every instruction.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_ifelse, S_repeat, S_set,
    add, eq, gt, lt, num, text, var, make_project, write_sb3,
)

def fixture(path: Path, warp: bool):
    counts = (0, 1, 2, 63, 64, 65, 127, 129, 257, 600)
    strings = (
        "17", " 12.5 ", "NaN", "Infinity", "-Infinity", "-0",
        "2e3", "0x10", "not a number", " ", "", "1.25",
    )
    outputs = [f"out_{i}" for i in range(len(counts))]
    g = Gen(["value", "sum", "temporary", "done", *outputs], [])
    statements = []
    for i, n in enumerate(counts):
        literal = text(strings[i % len(strings)])
        body = [
            S_set("value", literal),
            S_change("sum", var("value")),
            S_change("sum", var("value")),  # numeric cache must be reusable
            S_ifelse(gt(var("sum"), num(1000)),
                [S_set("value", text("1.5"))],
                [S_if(lt(var("sum"), num(-1000)),
                    [S_set("value", text("-2.25"))])]),
            S_change("sum", add(var("value"), num(1))),
            S_change("value", num(1)),
            S_change("sum", var("value")),
            S_set("value", add(var("sum"), num(1))),
            S_change("sum", var("value")),
            S_set("temporary", var("value")),
            # Two control-flow predecessors assign different storage types.
            # The join must merge boxed SSA values without coercion.
            S_ifelse(lt(var("sum"), num(1000)),
                [S_set("temporary", text(" 12.5 "))],
                [S_set("temporary", num(3))]),
            S_change("sum", var("temporary")),
            S_change("sum", var("temporary")),
            # Both branches produce verified numeric SSA values even though
            # this same scalar has received string writes elsewhere.
            S_ifelse(gt(var("sum"), num(0)),
                [S_set("temporary", add(var("sum"), num(1)))],
                [S_set("temporary", add(var("sum"), num(-4)))]),
            S_change("sum", var("temporary")),
            S_if(eq(var("temporary"), literal),
                [S_set("value", num(3))]),
            S_change("sum", var("value")),
        ]
        statements.extend([
            S_set("value", num(0)),
            S_set("sum", num(0)),
            S_repeat(num(n), body),
            S_set(outputs[i], var("sum")),
        ])
    statements.append(S_set("done", num(1)))
    g.define("mixed", [], statements, warp=warp)
    g.script([S_call("mixed")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return outputs + ["done", "sum", "value"]

def run(binary, path, keys, tier, enabled, extra=None):
    env = {k: v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_JIT_LAZY_TYPED_SSA="1" if enabled else "0",
               XYO_JIT_CONDITIONAL_SSA="0",
               XYO_JIT_INLINE_EMPTY_DIRECT="0",
               XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0")
    env.update(extra or {})
    p = subprocess.run([str(binary), "run", str(path), "--jit", tier,
                        "--frames", "1000"],
                       cwd=ROOT, env=env, capture_output=True, text=True,
                       timeout=110)
    if p.returncode:
        raise RuntimeError((path.name,tier,enabled,p.returncode,p.stderr[-1200:]))
    obj = json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}")+1])["Bench"]
    return {key:obj[key] for key in keys}

def main():
    binary = Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/"build/xyo"
    out = ROOT/"build/typed-ssa-fixtures"
    out.mkdir(parents=True, exist_ok=True)
    for warp in (True, False):
        path = out/("mixed-warp.sb3" if warp else "mixed-nonwarp.sb3")
        keys = fixture(path,warp)
        expected = run(binary,path,keys,"off",False)
        assert expected["done"] == "1"
        for tier,enabled,extra in [
            ("eager",False,{}),("eager",True,{}),
            ("hotspot",True,{}),("eager",True,{"XYO_JIT_O2":"1"}),
            # Whole-loop type-demand analysis on/off must preserve values.
            ("eager",False,{"XYO_JIT_TYPE_FLOW":"0"}),
            ("eager",False,{"XYO_JIT_TYPE_FLOW":"1"}),
            ("hotspot",False,{"XYO_JIT_TYPE_FLOW":"1"}),
            ("eager",True,{"XYO_JIT_TYPE_PROFILE":"0"}),
        ]:
            actual = run(binary,path,keys,tier,enabled,extra)
            mismatch={k:(expected[k],actual[k]) for k in keys
                      if expected[k] != actual[k]}
            if mismatch:
                raise AssertionError((warp,tier,enabled,mismatch))
            print("PASS",warp,tier,enabled,extra,flush=True)

if __name__ == "__main__":
    main()
