#!/usr/bin/env python3
"""MATH-015: exact change-by finite literal guard elision differential.

Compare interpreter, eager and HotSpot results with change guard ON/OFF
across nonfinite accumulators, signed zero and adverse expression paths.
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
    add, div, mul, num, op, var, make_project, write_sb3
)

def mathop(kind, value):
    return op("operator_mathop", {"OPERATOR":kind}, NUM=value)

def seeds():
    return [
        ("zero", lambda:num(0.0)),
        ("negative_zero", lambda:num(-0.0)),
        ("positive_one", lambda:num(1.0)),
        ("negative_one", lambda:num(-1.0)),
        ("very_large", lambda:num(1e308)),
        ("very_negative", lambda:num(-1e308)),
        ("tiny",lambda:num(1e-300)),
        ("infinity",lambda:div(num(1),num(0))),
        ("negative_infinity",lambda:div(num(-1),num(0))),
        ("nan",lambda:div(num(0),num(0))),
    ]

def fixture(path:Path,warp:bool):
    changes=[-1e308,-1.0,-0.0,0.0,0.1,1.0,1e308]
    cases=[(label,seed,change) for label,seed in seeds()
           for change in changes]
    names=[f"{prefix}{i}" for i in range(len(cases))
           for prefix in ("x","z","acc","inv","done")]
    gen=Gen(names,[])
    for i,(_label,seed,change) in enumerate(cases):
        x,z,acc,inv,done=(f"{prefix}{i}" for prefix in
                          ("x","z","acc","inv","done"))
        body=[
            S_change(x,num(change)),
            S_set(z,mathop("abs",var(x))),
            # Nonliteral delta is deliberately left guarded. A change
            # to the global rule must not erase this NaN check.
            S_change(acc,var(z)),
        ]
        program=[
            S_set(x,seed()), S_set(z,num(0)), S_set(acc,num(0)),
            S_repeat(num(65),body),
            S_set(inv,div(num(1),var(x))),
            S_set(done,num(1)),
        ]
        gen.define(f"run{i}",[],program,warp=warp)
        gen.script([S_call(f"run{i}")],x=i*20,y=i*20)
    write_sb3(path,make_project(gen.vars.keys(),gen.lists.keys(),gen))
    return names

def run(binary,project,keys,jit,change_facts):
    env={key:value for key,value in os.environ.items()
         if not key.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS":"json", "XYO_GPU":"0", "XYO_PAR":"0",
        "XYO_PARFOR":"0", "XYO_JIT_NUMERIC_REPEAT_FLOAT":"1",
        "XYO_JIT_FLOAT_CONST_CHANGE_FACTS":"1" if change_facts else "0"
    })
    result=subprocess.run(
        [str(binary),"run",str(project),"--jit",jit,"--frames","20000"],
        cwd=ROOT,text=True,capture_output=True,env=env,timeout=180)
    if result.returncode:
        raise RuntimeError(f"jit={jit}, facts={change_facts}: "
                           f"exit {result.returncode}\n{result.stderr[-1600:]}")
    output=result.stdout
    state=json.loads(output[output.index("{"):output.rindex("}")+1])["Bench"]
    assert all(state[k]=="1" for k in keys if k.startswith("done"))
    return {k:state[k] for k in keys}

def main():
    binary=(Path(sys.argv[1]).resolve() if len(sys.argv)>1
            else ROOT/"build/math015-change-xyo")
    out=ROOT/"build/math015-change-differential"
    out.mkdir(parents=True,exist_ok=True)
    for warp in (True,False):
        project=out/("warp.sb3" if warp else "nonwarp.sb3")
        keys=fixture(project,warp)
        baseline=run(binary,project,keys,"off",False)
        for jit,facts in (("eager",False),("eager",True),
                          ("hotspot",True)):
            state=run(binary,project,keys,jit,facts)
            mismatches={key:(baseline[key],state[key])
                        for key in keys if baseline[key]!=state[key]}
            assert not mismatches,(warp,jit,facts,list(mismatches.items())[:30])
            print("PASS",warp,jit,facts,len(keys),flush=True)

if __name__=="__main__":
    main()
