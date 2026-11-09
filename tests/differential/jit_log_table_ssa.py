#!/usr/bin/env python3
"""Exact jsLog positive-integer table JIT parity: edges, ULP neighbors, tags."""
import json
import math
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set,
    div, mul, num, op, text, var,
    make_project, write_sb3,
)

def log(x):
    return op("operator_mathop", {"OPERATOR":"ln"}, NUM=x)

def fixture(path, warp):
    integers = set(range(1, 35))
    integers |= {41, 53, 63, 64, 65, 127, 128, 129, 255, 256, 257,
                 511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049,
                 4095, 4096, 4097, 8190, 8191, 8192, 8193, 8194,
                 10000, 100000}
    cases = [(f"int_{n}", num(n)) for n in sorted(integers)]
    for n in (1, 2, 3, 53, 127, 256, 511, 1024, 2048, 8191, 8192):
        cases.extend((
            (f"prev_{n}",num(math.nextafter(float(n),-math.inf))),
            (f"next_{n}",num(math.nextafter(float(n),math.inf))),
        ))
    cases.extend([
        ("zero",num(0)),
        ("negativezero",div(num(0),num(-1))),
        ("negative",num(-1)),
        ("negativefraction",num(-0.05)),
        ("fraction",num(20.25)),
        ("infinity",div(num(1),num(0))),
        ("negativeinf",div(num(-1),num(0))),
        ("nan",div(num(0),num(0))),
        ("numericstring",text("2048")),
        ("nonnumericstring",text("abc")),
        ("huge",num(1e308)),
        ("tiny",num(1e-300)),
    ])
    outputs = [f"case_{label}_{field}" for label,_ in cases
               for field in ("last","acc","reciprocal")]
    gen=Gen(["x","v","total","done",*outputs],[])
    statements=[]
    for label,initial in cases:
        statements.extend([
            S_set("x",initial),S_set("total",num(0)),
            S_repeat(num(65), [
                S_set("v",log(var("x"))),
                S_change("total",div(var("v"),num(64))),
            ]),
            S_set(f"case_{label}_last",var("v")),
            S_set(f"case_{label}_acc",var("total")),
            S_set(f"case_{label}_reciprocal",div(num(1),var("v"))),
        ])
    statements.append(S_set("done",num(1)))
    gen.define("run",[],statements,warp=warp)
    gen.script([S_call("run")])
    write_sb3(path,make_project(gen.vars.keys(),gen.lists.keys(),gen))
    return outputs+["done"]

def run(binary,project,keys,jit,table):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_DUMPVARS="json",XYO_GPU="0",XYO_PAR="0",XYO_PARFOR="0",
        XYO_JIT_NUMERIC_REPEAT_FLOAT="1",
        XYO_JIT_LOG_TABLE="1" if table else "0",
        XYO_JIT_LOG_SHARED_TABLE=os.environ.get("XYO_JIT_LOG_SHARED_TABLE", "0"),
    )
    proc=subprocess.run([str(binary),"run",str(project),"--jit",jit,
                         "--frames","30000"],cwd=ROOT,env=env,
                        text=True,capture_output=True,timeout=120)
    if proc.returncode:
        raise RuntimeError(f"jit={jit} table={table}: "
                           +proc.stderr[-1500:])
    obj=json.loads(proc.stdout[
        proc.stdout.index("{"):proc.stdout.rindex("}")+1])["Bench"]
    return {k:obj[k] for k in keys}

def main():
    binary=(Path(sys.argv[1]).resolve() if len(sys.argv)>1 else
            ROOT/"build/math008-log-xyo")
    path=ROOT/"build/math008-log-differential"
    path.mkdir(parents=True,exist_ok=True)
    for warp in (True,False):
        project=path/("warp.sb3" if warp else "nonwarp.sb3")
        keys=fixture(project,warp)
        baseline=run(binary,project,keys,"off",False)
        assert baseline["done"]=="1"
        for jit,table in (("eager",False),("eager",True),
                          ("hotspot",True)):
            actual=run(binary,project,keys,jit,table)
            mismatch={k:(baseline[k],actual[k]) for k in keys
                      if baseline[k]!=actual[k]}
            assert not mismatch,(warp,jit,table,mismatch)
            print("PASS",warp,jit,table,len(keys),flush=True)

if __name__=="__main__":
    main()
