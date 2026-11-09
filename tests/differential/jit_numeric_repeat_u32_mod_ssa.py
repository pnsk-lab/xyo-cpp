#!/usr/bin/env python3
"""MATH-016: guarded i32 modulus vs original i64 numeric SSA.

Includes 64-tick scheduler boundaries, integer precision limits, input
types (numbers, strings, NaN, ±Infinity) and expressions whose interval
bound exceeds uint32. The slow path and 64-bit fast path must agree.
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
    add, div, mul, num, text, var, op, make_project, write_sb3,
)

def cases():
    entries=[
        ("0", num(0)),
        ("1", num(1)),
        ("3", num(3)),
        ("max_entry", num(2097152)),
        ("over_entry", num(2097153)),
        ("negative", num(-1)),
        ("negative_zero", num(-0.0)),
        ("fraction", num(1.5)),
        ("large_finite", num(1e308)),
        ("pos_inf", div(num(1),num(0))),
        ("neg_inf", div(num(-1),num(0))),
        ("nan", div(num(0),num(0))),
        ("string_number", text("42")),
        ("string_bad", text("xyz")),
    ]
    options=[
        ("normal", 31, 1000003),
        ("mod1", 31, 1),
        ("large_factor", 3000, 1000003),
        # With entry bound 2^21, factors 1000 and 1100 straddle
        # INT32_MAX. Factors 2000 and 2050 straddle UINT32_MAX.
        # The compiler must choose signed-i32, unsigned-i32, or
        # original i64 without truncation based on the full bound.
        ("near_signed32", 1000, 1000003),
        ("over_signed32", 1100, 1000003),
        ("near_unsigned32", 2000, 1000003),
        ("over_unsigned32", 2050, 1000003),
        ("near1e9_divisor", 31, 999999937),
        ("large_divisor", 31, 2000000000),
        ("zero_divisor", 31, 0),
        ("negative_divisor", 31, -7),
    ]
    return [
        (f"{name}_{opt}", value, factor, divisor)
        for name,value in entries
        for opt,factor,divisor in options
    ]

def fixture(path,warp):
    configurations=cases()
    keys=[f"{key}{i}" for i in range(len(configurations))
          for key in ("i","a","b","after","done")]
    g=Gen(keys,[])
    for n,(_name,initial,factor,divisor) in enumerate(configurations):
        idx,a,b,after,done=[f"{v}{n}" for v in ("i","a","b","after","done")]
        remainder=op("operator_mod", {}, NUM1=add(mul(var(a),num(factor)),var(idx)),
                     NUM2=num(divisor))
        script=[
            S_set(idx,num(0)),
            S_set(a,initial),
            S_set(b,num(0)),
            S_repeat(num(65),[
                S_change(idx,num(1)),
                S_set(a,remainder),
                S_set(b,add(var(b),div(var(a),num(4)))),
            ]),
            S_set(after,add(var(a),num(0))),
            S_set(done,num(1))
        ]
        g.define(f"run{n}",[],script,warp=warp)
        g.script([S_call(f"run{n}")],x=n*20,y=n*13)
    write_sb3(path,make_project(g.vars.keys(),g.lists.keys(),g))
    return keys

def run(binary,project,keys,jit,narrow,signed=False):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_DUMPVARS="json",XYO_GPU="0",XYO_PAR="0",XYO_PARFOR="0",
        XYO_JIT_NUMERIC_REPEAT_INT="1",
        XYO_JIT_NUMERIC_REPEAT_U32_MOD="1" if narrow else "0",
        XYO_JIT_NUMERIC_REPEAT_SIGNED_MOD="1" if signed else "0",
    )
    process=subprocess.run(
        [str(binary),"run",str(project),"--jit",jit,"--frames","30000"],
        cwd=ROOT,env=env,text=True,capture_output=True,timeout=180)
    if process.returncode:
        raise RuntimeError(
            f"{jit} narrow={narrow} exited {process.returncode}:\n"
            f"{process.stderr[-1700:]}")
    s=process.stdout
    result=json.loads(s[s.index("{"):s.rindex("}")+1])["Bench"]
    assert all(result[k]=="1" for k in keys if k.startswith("done"))
    return {key:result[key] for key in keys}

def main():
    binary=(Path(sys.argv[1]).resolve() if len(sys.argv)>1
            else ROOT/"build/math016-u32-xyo")
    folder=ROOT/"build/math016-u32-differential"
    folder.mkdir(parents=True,exist_ok=True)
    for warp in (True,False):
        project=folder/("warp.sb3" if warp else "nonwarp.sb3")
        keys=fixture(project,warp)
        baseline=run(binary,project,keys,"off",False)
        for jit,narrow,signed in (
            ("eager",False,False),("eager",True,False),
            ("eager",True,True),("hotspot",True,False),
            ("hotspot",True,True),
        ):
            actual=run(binary,project,keys,jit,narrow,signed)
            bad={key:(baseline[key],actual[key]) for key in keys
                 if baseline[key]!=actual[key]}
            assert not bad,(warp,jit,narrow,signed,list(bad.items())[:30])
            print("PASS",warp,jit,narrow,signed,len(keys),flush=True)

if __name__=="__main__":
    main()
