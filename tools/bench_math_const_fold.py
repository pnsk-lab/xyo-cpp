#!/usr/bin/env python3
"""JIT pure-math constant folding: ABBA startup/RSS + exact parity checks.

All values are pure Scratch numeric literals, and each warp repeat has
multiple variable writes, remaining below numeric SSA's 16-variable cap.
"""
import argparse, hashlib, json, os, statistics, subprocess, sys, time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen,S_call,S_change,S_repeat,S_set,
    num,op,var,make_project,write_sb3,
)

def mathop(fn,argument):
    return op("operator_mathop",{"OPERATOR":fn},NUM=num(argument))

def generate(path,count:int,edge:bool=False,warp:bool=True):
    names=[]
    gen=Gen([],[])
    finite_inputs=[
        ("ln",1.0),("ln",2.0),("ln",3.0),("ln",50.0),
        ("e ^",0.1),("e ^",1.5),("e ^",2.1),("e ^",4.7),
        ("10 ^",0.0),("10 ^",0.05),("10 ^",1.7),("10 ^",2.5),
    ]
    edge_inputs=[
        ("ln",0.0),("ln",-1.0),("ln",-0.0),("ln",1e-308),
        ("e ^",-745.0),("e ^",709.0),("e ^",1e308),("e ^",-1e308),
        ("10 ^",-324.0),("10 ^",308.0),("10 ^",-0.0),("10 ^",1e308),
    ]
    inputs=edge_inputs if edge else finite_inputs
    for i in range(count):
        index=f"counter{i}";done=f"done{i}"
        variables=[f"value{i}_{j}" for j in range(len(inputs))]
        names.extend([index,done,*variables])
        gen.vars.update({k:"v_"+k for k in [index,done,*variables]})
        body=[S_change(index,num(1))]
        for j,(fn,value) in enumerate(inputs):
            # Keep every reporter static; varying scripts exercise separate
            # compiled modules, without a per-benchmark runtime special case.
            body.append(S_set(variables[j],mathop(fn,value)))
        gen.define(f"run{i}",[],[
            S_set(index,num(0)),
            *[S_set(name,num(0)) for name in variables],
            S_repeat(num(65),body),
            S_set(done,num(1)),
        ],warp=warp)
        gen.script([S_call(f"run{i}")],x=i*60,y=i*24)
    write_sb3(path,make_project(gen.vars.keys(),gen.lists.keys(),gen))
    return names

def measure(binary,path,keys,fold,log,jit):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS":"json","XYO_GPU":"0","XYO_PAR":"0","XYO_PARFOR":"0",
        "XYO_JIT_NUMERIC_REPEAT_FLOAT":"1",
        "XYO_JIT_MATH_CONST_FOLD":"1" if fold else "0",
        "XYO_JIT_LOG_TABLE":"1","XYO_JIT_EXP_TABLE":"1",
        "XYO_JIT_POW10_TABLE":"1","XYO_JIT_LOG_SHARED_TABLE":"0",
    })
    with log.open("wb") as out,log.with_suffix(".err").open("wb") as err:
        start=time.perf_counter()
        p=subprocess.Popen(
            [str(binary),"run",str(path),"--jit",jit,"--frames","5000"],
            env=env,cwd=ROOT,stdout=out,stderr=err)
        pid,status,usage=os.wait4(p.pid,0)
        wall=time.perf_counter()-start
    assert pid==p.pid
    code=os.waitstatus_to_exitcode(status)
    if code:
        raise RuntimeError(f"{jit}, fold={fold}, exit {code}: "
                           f"{log.with_suffix('.err').read_text(errors='replace')[-1400:]}")
    data=log.read_bytes()
    obj=json.loads(data[data.index(b"{"):data.rindex(b"}")+1])["Bench"]
    assert all(obj[k]=="1" for k in keys if k.startswith("done"))
    digest=hashlib.sha256(
        json.dumps({k:obj[k] for k in keys},sort_keys=True).encode()).hexdigest()
    return {
        "fold":fold,"jit":jit,"cpu_s":usage.ru_utime+usage.ru_stime,
        "rss_mib":usage.ru_maxrss/1024,"wall_s":wall,"digest":digest
    }

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--binary",type=Path,default=ROOT/"build/math011-fold-final-xyo")
    p.add_argument("--scripts",type=int,default=12)
    p.add_argument("--groups",type=int,default=6)
    p.add_argument("--jit",choices=("eager","hotspot","off"),default="eager")
    p.add_argument("--edge",action="store_true")
    p.add_argument("--nonwarp",action="store_true")
    p.add_argument("--output",type=Path,default=ROOT/"build/math011-constfold-abba.json")
    a=p.parse_args()
    assert a.groups>=1 and 1<=a.scripts<=64
    a.output.parent.mkdir(parents=True,exist_ok=True)
    path=a.output.with_suffix(".sb3")
    keys=generate(path,a.scripts,a.edge,not a.nonwarp)
    rows=[]
    for group in range(a.groups):
        pattern=(False,True,True,False) if group%2==0 else (True,False,False,True)
        for slot,fold in enumerate(pattern):
            logfile=a.output.parent/f"math011-constfold-{group}-{slot}.log"
            r=measure(a.binary.resolve(),path,keys,fold,logfile,a.jit)
            rows.append({"group":group,"slot":slot,**r})
            a.output.write_text(json.dumps(rows,indent=2)+"\n")
            print(group,slot,"FOLD" if fold else "BASELINE",
                  "cpu_s",round(r["cpu_s"],4),
                  "rss_mib",round(r["rss_mib"],2),
                  "wall_s",round(r["wall_s"],4),flush=True)
    assert len({r["digest"] for r in rows})==1
    cpus=[];rss=[]
    for group in range(a.groups):
        v=[r for r in rows if r["group"]==group]
        old=[r for r in v if not r["fold"]]
        new=[r for r in v if r["fold"]]
        ratio=statistics.mean(r["cpu_s"] for r in new)/statistics.mean(
            r["cpu_s"] for r in old)
        mem=statistics.mean(r["rss_mib"] for r in new)-statistics.mean(
            r["rss_mib"] for r in old)
        cpus.append(ratio);rss.append(mem)
        print("GROUP",group,"cpu_ratio",round(ratio,4),
              "rss_delta",round(mem,2),flush=True)
    print("RESULT",a.scripts,"scripts", "median_cpu_ratio",statistics.median(cpus),
          "wins",sum(x<1 for x in cpus),"median_rss_delta",statistics.median(rss),
          "digest",rows[0]["digest"])

if __name__=="__main__":
    main()
