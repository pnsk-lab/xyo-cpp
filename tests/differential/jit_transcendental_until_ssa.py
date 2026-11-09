#!/usr/bin/env python3
"""Regression for the shared numeric-until body parser with new MathOps."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen,S_call,S_change,S_set,S_until,add,div,gt,num,op,text,var,
    make_project,write_sb3
)

def mathop(name, value):
    return op("operator_mathop",{"OPERATOR":name},NUM=value)

def round_(value):
    return op("operator_round",NUM=value)

def fixture(path,warp):
    cases=[(0,0),(0,63),(0,64),(0,65),(0,129),
           (-10,0),(-10,65),(2,63),("bad",63)]
    outputs=[f"out_{i}_{name}" for i in range(len(cases))
             for name in ("i","acc","tmp","inverse")]
    g=Gen(["i","acc","tmp","done",*outputs],[])
    program=[]
    for j,(start,limit) in enumerate(cases):
        initial=text(start) if isinstance(start,str) else num(start)
        program.extend([
            S_set("i",initial),S_set("acc",num(0)),S_set("tmp",num(0)),
            S_until(gt(var("i"),num(limit)),[
                S_change("i",num(1)),
                S_set("tmp",round_(div(var("i"),num(7)))),
                S_change("acc",add(
                    mathop("sin",var("i")),
                    mathop("sqrt",add(var("i"),num(1))))),
            ]),
            S_set(f"out_{j}_i",var("i")),
            S_set(f"out_{j}_acc",var("acc")),
            S_set(f"out_{j}_tmp",var("tmp")),
            S_set(f"out_{j}_inverse",div(num(1),var("acc"))),
        ])
    program.append(S_set("done",num(1)))
    g.define("run",[],program,warp=warp)
    g.script([S_call("run")])
    write_sb3(path,make_project(g.vars.keys(),g.lists.keys(),g))
    return outputs+["done"]

def run(binary,path,keys,jit,ssa):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_DUMPVARS="json",XYO_GPU="0",XYO_PAR="0",XYO_PARFOR="0",
        XYO_JIT_NUMERIC_UNTIL_SSA=str(int(ssa)),
        XYO_JIT_NUMERIC_UNTIL_FAST=str(int(ssa)),
    )
    p=subprocess.run([str(binary),"run",str(path),"--jit",jit,
                     "--frames","10000"],cwd=ROOT,env=env,
                     capture_output=True,text=True,timeout=90)
    if p.returncode:
        raise RuntimeError(f"{jit} {ssa}: {p.stderr[-1800:]}")
    o=json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}")+1])["Bench"]
    return {k:o[k] for k in keys}

def main():
    binary=Path(sys.argv[1] if len(sys.argv)>1 else ROOT/"build/xyo").resolve()
    folder=ROOT/"build/math006-until-differential"
    folder.mkdir(parents=True,exist_ok=True)
    for warp in (True,False):
        path=folder/("warp.sb3" if warp else "nonwarp.sb3")
        keys=fixture(path,warp)
        reference=run(binary,path,keys,"off",False)
        assert reference["done"]=="1"
        for jit,ssa in [("eager",False),("eager",True),("hotspot",True)]:
            actual=run(binary,path,keys,jit,ssa)
            mismatch={k:(reference[k],actual[k]) for k in keys
                      if reference[k]!=actual[k]}
            assert not mismatch,(warp,jit,ssa,mismatch)
            print("PASS until",warp,jit,ssa,len(keys),flush=True)

if __name__=="__main__":
    main()
