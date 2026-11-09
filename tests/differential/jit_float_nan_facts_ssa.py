#!/usr/bin/env python3
"""Guarded f64 repeat: prove redundant NaN guards safe with hostile inputs."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set,
    add, sub, mul, div, num, op, var, make_project, write_sb3,
)

def sqrt(v):
    return op("operator_mathop", {"OPERATOR": "sqrt"}, NUM=v)

def fixture(path,warp):
    initial = [
        (num(0),num(0)),
        (num(-0.0),num(0)),
        (num(1),num(0)),
        (num(3),num(4)),
        (num(-3),num(4)),
        (num(1e308),num(1e308)),
        (num(1e-300),num(0)),
        (num(1e150),num(-1e150)),
        (div(num(1),num(0)),num(0)),
        (div(num(-1),num(0)),div(num(1),num(0))),
        (div(num(0),num(0)),num(1)),
        (num(1),div(num(0),num(0))),
    ]
    cases = [(i,j,mode) for i,j in initial for mode in ("signflip","divide","nan")]
    names=[f"{name}{i}" for i in range(len(cases))
           for name in ("x","y","z","acc","done")]
    g=Gen(names,[])
    for i,(start_x,start_y,mode) in enumerate(cases):
        x,y,z,acc=(f"{name}{i}" for name in ("x","y","z","acc"))
        body = [
            S_set(z,sqrt(add(mul(var(x),var(x)),
                             mul(var(y),var(y))))),
            S_change(acc,mul(var(z),num(0.01))),
        ]
        if mode=="signflip":
            body.extend([
                S_set(x,sub(num(0),var(x))),
                S_set(y,sub(num(0),var(y))),
            ])
        elif mode=="divide":
            body.extend([
                S_set(x,div(num(1),sub(var(z),num(1)))),
                S_set(y,div(num(-1),sub(var(z),num(1)))),
            ])
        else:
            body.extend([
                S_set(x,sub(var(z),var(z))),
                S_set(y,sub(num(0),var(y))),
            ])
        program = [
            S_set(x,start_x),S_set(y,start_y),S_set(z,num(0)),
            S_set(acc,num(0)),
            S_repeat(num(65),body),
            S_set(f"done{i}",num(1))
        ]
        g.define(f"run{i}",[],program,warp=warp)
        g.script([S_call(f"run{i}")],x=i*18,y=i*13)
    write_sb3(path,make_project(g.vars.keys(),g.lists.keys(),g))
    return names

def run(binary,project,keys,jit,facts):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_DUMPVARS="json",XYO_GPU="0",XYO_PAR="0",XYO_PARFOR="0",
        XYO_JIT_NUMERIC_REPEAT_FLOAT="1",
        XYO_JIT_FLOAT_NAN_FACTS=str(int(facts)),
    )
    p=subprocess.run([
        str(binary),"run",str(project),"--jit",jit,"--frames","30000"
    ],env=env,cwd=ROOT,text=True,capture_output=True,timeout=120)
    if p.returncode:
        raise RuntimeError(f"{jit=},{facts=} exit={p.returncode}: "+
                           p.stderr[-1600:])
    state=json.loads(p.stdout[p.stdout.index("{"):
                                p.stdout.rindex("}")+1])["Bench"]
    assert all(state[k]=="1" for k in keys if k.startswith("done"))
    return {k:state[k] for k in keys}

def main():
    binary=(Path(sys.argv[1]).resolve() if len(sys.argv)>1 else
            ROOT/"build/math013-facts-xyo")
    directory=ROOT/"build/math013-nan-facts-differential"
    directory.mkdir(parents=True,exist_ok=True)
    for warp in (True,False):
        project=directory/("warp.sb3" if warp else "nonwarp.sb3")
        keys=fixture(project,warp)
        reference=run(binary,project,keys,"off",False)
        for jit,facts in (("eager",False),("eager",True),("hotspot",True)):
            actual=run(binary,project,keys,jit,facts)
            bad={k:(reference[k],actual[k]) for k in keys
                 if reference[k]!=actual[k]}
            assert not bad,(warp,jit,facts,bad)
            print("PASS",warp,jit,facts,len(keys),flush=True)

if __name__=="__main__":
    main()
