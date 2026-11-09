#!/usr/bin/env python3
"""Scratch native-f64 SSA constant expression tree parity.

Exercise nested literal arithmetic feeding ln, e^x and 10^x, including
NaN-producing cases where the original runtime path must be preserved.
Use independently generated warp and non-warp projects and compare all
observable values with the pure interpreter.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen,S_call,S_change,S_repeat,S_set,
    add,sub,mul,div,num,op,var,make_project,write_sb3,
)


def mathop(name, argument):
    return op("operator_mathop", {"OPERATOR":name}, NUM=argument)


def expressions(index):
    c=var(f"counter{index}")
    return [
        mathop("ln",add(num(1),num(2))),
        mathop("ln",mul(num(2),num(4))),
        mathop("ln",div(num(9),num(3))),
        mathop("ln",sub(num(5),num(4))),
        mathop("ln",sub(num(2),num(3))),
        mathop("ln",div(num(0),num(0))),
        mathop("e ^",add(num(1),num(0.5))),
        mathop("e ^",sub(num(2),num(0.3))),
        mathop("e ^",div(num(3),num(2))),
        mathop("e ^",mul(num(-1),num(50))),
        mathop("e ^",div(num(1),num(0))),
        mathop("10 ^",div(num(1),num(20))),
        mathop("10 ^",div(num(51),num(20))),
        mathop("10 ^",sub(num(0),num(0.05))),
        mathop("10 ^",mul(num(-1),num(324))),
        mathop("ln",add(num(-0.0),num(1))),
        mathop("ln",mathop("e ^",add(num(1),num(0.7)))),
        mathop("e ^",mathop("ln",num(2))),
        mathop("ln",add(num(1e16),num(1))),
        mathop("ln",add(c,num(2))),
        mathop("e ^",div(c,num(7))),
        mathop("10 ^",div(add(c,num(2)),num(20))),
        mathop("e ^",add(num(1e308),num(1e308))),
        mathop("ln",sub(num(0),num(0.0))),
    ]


def fixture(path:Path, warp:bool):
    count=len(expressions(0))
    variables=[f"{key}{i}" for i in range(count)
               for key in ("counter","value","acc","last")]
    done=[f"done{i}" for i in range(count)]
    g=Gen(variables+done,[])
    for i, expr in enumerate(expressions(0)):
        # The dynamic cases must be constructed using this procedure's
        # own variable, rather than shared or nonexistent variables.
        expr=expressions(i)[i]
        statements=[
            S_set(f"counter{i}",num(0)),
            S_set(f"acc{i}",num(0)),
            S_repeat(num(65),[
                S_change(f"counter{i}",num(1)),
                S_set(f"value{i}",expr),
                S_change(f"acc{i}",div(var(f"counter{i}"),num(64))),
            ]),
            S_set(f"last{i}",var(f"value{i}")),
            S_set(f"done{i}",num(1)),
        ]
        g.define(f"run{i}",[],statements,warp=warp)
        g.script([S_call(f"run{i}")],x=i*30,y=i*20)
    write_sb3(path,make_project(g.vars.keys(),g.lists.keys(),g))
    return variables+done


def run(binary,project,keys,jit,tree):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_DUMPVARS="json",XYO_GPU="0",XYO_PAR="0",XYO_PARFOR="0",
        XYO_JIT_NUMERIC_REPEAT_FLOAT="1",
        XYO_JIT_MATH_CONST_TREE=str(int(tree)),
    )
    proc=subprocess.run(
        [str(binary),"run",str(project),"--jit",jit,"--frames","20000"],
        cwd=ROOT,env=env,text=True,capture_output=True,timeout=90)
    if proc.returncode:
        raise RuntimeError(f"{jit=} {tree=} {project=} failed: "+
                           proc.stderr[-2500:])
    result=json.loads(proc.stdout[
        proc.stdout.index("{"):proc.stdout.rindex("}")+1])["Bench"]
    assert all(result[k]=="1" for k in keys if k.startswith("done")),\
        "unfinished procedure"
    return {k:result[k] for k in keys}


def main():
    binary=(Path(sys.argv[1]).resolve() if len(sys.argv)>1 else
            ROOT/"build/math012-tree-xyo")
    folder=ROOT/"build/math012-tree-differential"
    folder.mkdir(parents=True,exist_ok=True)
    for warp in (True,False):
        project=folder/("warp.sb3" if warp else "nonwarp.sb3")
        keys=fixture(project,warp)
        baseline=run(binary,project,keys,"off",False)
        for jit,tree in (("eager",False),("eager",True),
                         ("hotspot",True)):
            actual=run(binary,project,keys,jit,tree)
            mismatch={k:(baseline[k],actual[k]) for k in keys
                      if baseline[k]!=actual[k]}
            assert not mismatch,(warp,jit,tree,mismatch)
            print("PASS",warp,jit,tree,len(keys),flush=True)

if __name__=="__main__":
    main()
