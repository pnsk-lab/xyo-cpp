#!/usr/bin/env python3
"""Round, trig, exponential and sqrt repeat SSA differential tests.

Interpreter is the reference. Every result is compared against both the
ordinary JIT and the optimized JIT, with warp/non-warp scheduler boundaries.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set,
    add, div, mod, mul, num, op, sub, text, var,
    make_project, write_sb3
)

def mathop(name, number):
    return op("operator_mathop", {"OPERATOR":name}, NUM=number)

def round_(number):
    return op("operator_round", NUM=number)

def math_body(kind):
    i=var("i")
    if kind=="round":
        return [
            S_change("i",num(1)),
            S_change("acc",add(add(
                mathop("floor",div(i,num(3))),
                round_(div(i,num(7)))),
                mathop("abs",sub(num(0),i)))),
        ]
    if kind=="round_ties":
        # Explicit positive/negative half-integers exercise Scratch's
        # ties-toward-positive-infinity rule, including signed zero.
        return [
            S_change("i",num(1)),
            S_change("acc",add(add(
                mathop("floor",div(i,num(2))),
                round_(div(i,num(2)))),
                mathop("ceiling",div(i,num(3))))),
        ]
    if kind=="trig":
        return [
            S_change("i",num(1)),
            S_change("acc",add(
                add(mathop("sin",i),mathop("cos",i)),
                mathop("atan",div(i,num(100))))),
        ]
    if kind=="expsqrt":
        return [
            S_change("i",num(1)),
            S_set("x",add(mod(i,num(50)),num(1))),
            S_change("acc",add(
                add(mathop("sqrt",i),mathop("ln",i)),
                add(mathop("e ^",div(var("x"),num(10))),
                    mathop("10 ^",div(var("x"),num(20)))))),
        ]
    raise ValueError(kind)

def fixture(path,kind,warp):
    negzero=div(num(0),num(-1))
    nan=div(num(0),num(0))
    inf=div(num(1),num(0))
    cases=[
        (0,num(0),num(0)),(1,num(0),num(0)),
        (63,num(0),num(0)),(64,num(0),num(0)),
        (65,num(0),num(0)),(127,num(0),num(0)),
        (129,num(0),num(0)),(255,num(0),num(0)),
        (65,num(-6.5),num(0)),(65,num(-1),num(0)),
        (65,negzero,num(0)),(65,num(1.25),num(0)),
        (65,text("5"),num(0)),(65,text("oops"),num(0)),
        (65,inf,num(0)),(65,nan,num(0)),
        (65,num(0),inf),(65,num(0),nan),
        (65,num(0),text("3")),(65,num(0),negzero),
        (65,num(1e308),num(0)),(65,num(-1e308),num(0)),
        (65,num(-7),num(0)),(65,num(-6),num(0)),
        (65,num(-4),num(0)),(65,num(-2),num(0)),
        (65,num(89),num(0)),(65,num(-91),num(0)),
        (65,num(269),num(0)),(65,num(-271),num(0)),
        (65,num(-0.5),num(0)),(65,num(3),num(0)),
    ]
    outputs=[f"out_{i}_{name}" for i in range(len(cases))
             for name in ("i","acc","x","reciprocal")]
    vars=["i","acc","x","done",*outputs]
    g=Gen(vars,[])
    statements=[]
    for i,(count,initial_i,initial_acc) in enumerate(cases):
        statements.extend([
            S_set("i",initial_i),
            S_set("acc",initial_acc),
            S_set("x",num(0)),
            S_repeat(num(count),math_body(kind)),
            S_set(f"out_{i}_i",var("i")),
            S_set(f"out_{i}_acc",var("acc")),
            S_set(f"out_{i}_x",var("x")),
            S_set(f"out_{i}_reciprocal",div(num(1),var("acc"))),
        ])
    statements.append(S_set("done",num(1)))
    g.define("run",[],statements,warp=warp)
    g.script([S_call("run")])
    write_sb3(path,make_project(g.vars.keys(),g.lists.keys(),g))
    return outputs+["done"]

def execute(binary,project,keys,jit,enabled):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS":"json",
        "XYO_GPU":"0","XYO_PAR":"0","XYO_PARFOR":"0",
        "XYO_JIT_NUMERIC_REPEAT_FLOAT":"1" if enabled else "0",
    })
    p=subprocess.run([str(binary),"run",str(project),"--jit",jit,
        "--frames","10000"],cwd=ROOT,env=env,text=True,capture_output=True,
        timeout=120)
    if p.returncode:
        raise RuntimeError(f"{project} jit={jit} enabled={enabled}: "
                           f"{p.stderr[-1500:]}")
    output=json.loads(p.stdout[p.stdout.index("{"):
                            p.stdout.rindex("}")+1])["Bench"]
    return {key:output[key] for key in keys}

def main():
    binary=(Path(sys.argv[1]).resolve()
        if len(sys.argv)>1 else ROOT/"build/xyo")
    out=ROOT/"build/math006-differential"
    out.mkdir(parents=True,exist_ok=True)
    for kind in ("round","round_ties","trig","expsqrt"):
        for warp in (True,False):
            project=out/f"{kind}-{'warp' if warp else 'nonwarp'}.sb3"
            keys=fixture(project,kind,warp)
            reference=execute(binary,project,keys,"off",False)
            assert reference["done"]=="1"
            for jit,enabled in [
                ("eager",False),("eager",True),("hotspot",True)
            ]:
                observed=execute(binary,project,keys,jit,enabled)
                failures={key:(reference[key],observed[key])
                    for key in keys if observed[key]!=reference[key]}
                if failures:
                    raise AssertionError((kind,warp,jit,enabled,failures))
                print("PASS",kind,"warp",warp,jit,enabled,
                      len(keys),"values",flush=True)

if __name__=="__main__":
    main()
