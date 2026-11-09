#!/usr/bin/env python3
"""MATH-014 parity: repeated-variable products and safe divisions.

The positive cases are x*x*x, (x*x)*(x*x), and 5000/(x*x*x).
Negative controls are x*0, 0/(x*x), and x/x: these MAY produce NaN.
Covers signed zero, infinity, NaN, overflow and underflow, and verifies
both ordinary and warp procedures against the interpreter.
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
    add, div, mul, num, op, var, make_project, write_sb3,
)

def mathop(name, value):
    return op("operator_mathop", {"OPERATOR": name}, NUM=value)

def seeds():
    return [
        ("zero", lambda: num(0.0)),
        ("negative_zero", lambda: num(-0.0)),
        ("positive", lambda: num(1.0)),
        ("negative", lambda: num(-1.0)),
        ("large", lambda: num(1e308)),
        ("negative_large", lambda: num(-1e308)),
        ("overflow", lambda: mul(num(1e308),num(1e308))),
        ("tiny", lambda: num(1e-300)),
        ("negative_tiny", lambda: num(-1e-300)),
        ("subnormal", lambda: num(1e-320)),
        ("infinity", lambda: div(num(1),num(0))),
        ("negative_infinity", lambda: div(num(-1),num(0))),
        ("nan", lambda: div(num(0),num(0))),
        ("three", lambda: num(3)),
        ("negative_three", lambda: num(-3)),
        ("sqrt2", lambda: mathop("sqrt",num(2))),
    ]

def expressions(name):
    x=var(name)
    def square():
        return mul(var(name),var(name))
    if name.endswith("_placeholder"):
        raise AssertionError("invalid variable")
    return [
        ("cube", lambda: mul(square(),var(name))),
        ("fourth", lambda: mul(square(),square())),
        ("division", lambda: div(num(5000),mul(square(),var(name)))),
        ("zero_multiply", lambda: mul(var(name),num(0))),
        ("zero_divide", lambda: div(num(0),square())),
        ("self_divide", lambda: div(var(name),var(name))),
    ]

def fixture(path:Path,warp:bool):
    definitions=[]
    for seed_name, factory in seeds():
        for expr_name,_ in expressions("x"):
            definitions.append((seed_name, factory, expr_name))
    variables=[]
    for i in range(len(definitions)):
        variables += [f"{field}{i}" for field in ("x","z","acc","done")]
    gen=Gen(variables,[])
    for i,(_seed_name,seed_value,expr_name) in enumerate(definitions):
        x,z,acc,done=(f"{field}{i}" for field in ("x","z","acc","done"))
        expression=dict(expressions(x))[expr_name]()
        # "abs" is a side-effect-free math reporter. Its presence
        # activates the same fast numeric SSA path used in N-body.
        gen.define(f"check{i}",[],[
            S_set(x,seed_value()),
            S_set(z,num(0)),S_set(acc,num(0)),
            S_repeat(num(65),[
                S_set(z,mathop("abs",expression)),
                S_change(acc,div(var(z),num(2))),
                S_set(x,add(var(x),num(0))),
            ]),
            S_set(done,num(1))
        ],warp=warp)
        gen.script([S_call(f"check{i}")],x=i*26,y=i*13)
    write_sb3(path,make_project(gen.vars.keys(),gen.lists.keys(),gen))
    return variables

def run(binary:Path,project:Path,keys:list[str],jit:str,extended:bool):
    env={key:value for key,value in os.environ.items()
         if not key.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS":"json",
        "XYO_GPU":"0","XYO_PAR":"0","XYO_PARFOR":"0",
        "XYO_JIT_NUMERIC_REPEAT_FLOAT":"1",
        "XYO_JIT_FLOAT_NAN_FACTS":"1",
        "XYO_JIT_FLOAT_NAN_POWER_FACTS":"1" if extended else "0",
    })
    completed=subprocess.run(
        [str(binary),"run",str(project),"--jit",jit,"--frames","20000"],
        cwd=ROOT,env=env,capture_output=True,text=True,timeout=180)
    if completed.returncode:
        raise RuntimeError(f"{jit=} {extended=} exit={completed.returncode}: "+
                           completed.stderr[-1500:])
    data=completed.stdout
    state=json.loads(data[data.index("{"):data.rindex("}")+1])["Bench"]
    assert all(state[k]=="1" for k in keys if k.startswith("done")), "unfinished"
    return {key:state[key] for key in keys}

def main():
    binary=(Path(sys.argv[1]).resolve() if len(sys.argv)>1 else
            ROOT/"build/math014-power-xyo")
    out=ROOT/"build/math014-differential"
    out.mkdir(parents=True,exist_ok=True)
    for warp in (True,False):
        project=out/("warp.sb3" if warp else "nonwarp.sb3")
        keys=fixture(project,warp)
        baseline=run(binary,project,keys,"off",False)
        for jit,extended in (("eager",False),("eager",True),
                             ("hotspot",True)):
            actual=run(binary,project,keys,jit,extended)
            bad={key:(baseline[key],actual[key]) for key in keys
                 if baseline[key]!=actual[key]}
            assert not bad,(warp,jit,extended,list(bad.items())[:30])
            print("PASS",warp,jit,extended,len(keys),flush=True)

if __name__=="__main__":
    main()
