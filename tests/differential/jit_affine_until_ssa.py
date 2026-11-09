#!/usr/bin/env python3
"""Pure integer affine parity/division until: interpreter/JIT parity."""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_ifelse, S_set, S_until,
    add, div, eq, mod, mul, num, text, var, make_project, write_sb3,
)

def fixture(path, warp):
    cases=[
        (1,num(0)),(2,num(0)),(3,num(0)),(4,num(0)),
        (7,num(0)),(8,num(0)),(19,num(0)),(27,num(0)),
        (97,num(0)),(129,num(0)),(871,num(0)),
        (8,num(0.5)),(8,num(-2)),(8,text("5")),
        (8,div(num(0),num(-1))),
        (27,text("12")),(97,div(num(0),num(0))),
        (2**40,num(0)), (2**40+2,num(0)),
        (2**48,num(0)), (4,num(2**40+1)),
    ]
    outputs=[f"case_{i}_{suffix}" for i in range(len(cases))
             for suffix in ("n","acc","inverse_n","inverse_acc")]
    g=Gen(["n","acc","done",*outputs],[])
    stmts=[]
    for i,(n,acc) in enumerate(cases):
        stmts += [S_set("n",n if not isinstance(n,int) else num(n)),
                  S_set("acc",acc),
                  S_until(eq(var("n"),num(1)),[
                      S_ifelse(
                          eq(mod(var("n"),num(2)),num(0)),
                          [S_set("n",div(var("n"),num(2)))],
                          [S_set("n",add(mul(var("n"),num(3)),num(1)))],
                      ),
                      S_change("acc",num(1)),
                  ]),
                  S_set(f"case_{i}_n",var("n")),
                  S_set(f"case_{i}_acc",var("acc")),
                  S_set(f"case_{i}_inverse_n",div(num(1),var("n"))),
                  S_set(f"case_{i}_inverse_acc",div(num(1),var("acc"))),
                  ]
    stmts.append(S_set("done",num(1)))
    g.define("run",[],stmts,warp=warp)
    g.script([S_call("run")])
    write_sb3(path,make_project(g.vars.keys(),g.lists.keys(),g))
    return outputs+["done"]

def run(binary,project,keys,jit,enabled):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS":"json","XYO_GPU":"0","XYO_PAR":"0",
        "XYO_PARFOR":"0",
        "XYO_JIT_AFFINE_UNTIL":str(int(enabled)),
        "XYO_JIT_NUMERIC_UNTIL_SSA":"0"
    })
    p=subprocess.run(
        [str(binary),"run",str(project),"--jit",jit,"--frames","40000"],
        env=env,cwd=ROOT,capture_output=True,text=True,timeout=120)
    if p.returncode:
        raise RuntimeError(f"{project} {jit} affine={enabled}: "+
                           p.stderr[-1500:])
    results=json.loads(p.stdout[p.stdout.index("{"):
                                p.stdout.rindex("}")+1])["Bench"]
    return {k:results[k] for k in keys}

def main():
    bin=Path(sys.argv[1] if len(sys.argv)>1 else ROOT/"build/xyo").resolve()
    out=ROOT/"build/math004-affine-differential";out.mkdir(parents=True,exist_ok=True)
    for warp in (True,False):
        p=out/("warp.sb3" if warp else "nonwarp.sb3")
        keys=fixture(p,warp)
        reference=run(bin,p,keys,"off",False)
        assert reference["done"]=="1"
        for jit,enabled in [
            ("eager",False),("eager",True),("hotspot",True)
        ]:
            actual=run(bin,p,keys,jit,enabled)
            mismatched={k:(reference[k],actual[k]) for k in keys
                        if reference[k]!=actual[k]}
            if mismatched:
                raise AssertionError((warp,jit,enabled,mismatched))
            print("PASS",warp,jit,enabled,len(keys),flush=True)

if __name__=="__main__":
    main()
