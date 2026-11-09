#!/usr/bin/env python3
"""Native LLVM JIT ln cache comparison for independently triggered Scratch scripts."""
import argparse, hashlib, json, os, statistics, subprocess, sys, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen,S_call,S_change,S_repeat,S_set,num,op,var,make_project,write_sb3
)

def generate(path,count):
    names=[f"{p}{i}" for i in range(count) for p in ("i","acc","done")]
    g=Gen(names,[])
    for i in range(count):
        index=f"i{i}"; acc=f"acc{i}"; done=f"done{i}"
        g.define(f"run{i}",[],[
            S_set(index,num(i+1)),S_set(acc,num(0)),
            S_repeat(num(65),[
                S_change(index,num(1)),
                S_change(acc,op("operator_mathop",{"OPERATOR":"ln"},
                                NUM=var(index)))
            ]),S_set(done,num(1))],warp=True)
        g.script([S_call(f"run{i}")],x=i*45,y=i*25)
    write_sb3(path,make_project(g.vars.keys(),g.lists.keys(),g))
    return names

def run(binary,project,keys,shared,path):
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS":"json","XYO_GPU":"0","XYO_PAR":"0","XYO_PARFOR":"0",
        "XYO_JIT_LOG_TABLE":"1",
        "XYO_JIT_LOG_SHARED_TABLE":"1" if shared else "0",
        "XYO_JIT_NUMERIC_REPEAT_FLOAT":"1",
        "XYO_JIT_EXP_TABLE":"0","XYO_JIT_POW10_TABLE":"0",
    })
    with path.open("wb") as out,path.with_suffix(".err").open("wb") as err:
        start=time.perf_counter()
        p=subprocess.Popen(
            [str(binary),"run",str(project),"--jit","eager","--frames","5000"],
            cwd=ROOT,env=env,stdout=out,stderr=err)
        pid,status,usage=os.wait4(p.pid,0)
        elapsed=time.perf_counter()-start
    assert pid==p.pid
    code=os.waitstatus_to_exitcode(status)
    if code: raise RuntimeError(f"{code}: "+path.with_suffix(".err").read_text()[-1200:])
    data=path.read_bytes()
    result=json.loads(data[data.index(b"{"):data.rindex(b"}")+1])["Bench"]
    assert all(result[k]=="1" for k in keys if k.startswith("done")), "unfinished"
    observed={k:result[k] for k in keys}
    return {
        "shared":shared,
        "cpu_s":usage.ru_utime+usage.ru_stime,
        "wall_s":elapsed,"rss_mib":usage.ru_maxrss/1024,
        "digest":hashlib.sha256(json.dumps(observed,sort_keys=True).encode()).hexdigest(),
    }

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--binary",type=Path,default=ROOT/"build/math010-ab-xyo")
    p.add_argument("--scripts",type=int,default=32)
    p.add_argument("--groups",type=int,default=4)
    p.add_argument("--output",type=Path,default=ROOT/"build/math011-multiscript-cost.json")
    a=p.parse_args()
    assert 2<=a.scripts<=100 and a.groups>=1
    a.output.parent.mkdir(parents=True,exist_ok=True)
    project=a.output.with_suffix(".sb3")
    keys=generate(project,a.scripts)
    rows=[]
    for group in range(a.groups):
        pattern=(False,True,True,False) if group%2==0 else (True,False,False,True)
        for slot,shared in enumerate(pattern):
            filename=a.output.parent/f"math011-multi-{group}-{slot}.log"
            sample=run(a.binary.resolve(),project,keys,shared,filename)
            rows.append({"group":group,"slot":slot,**sample})
            a.output.write_text(json.dumps(rows,indent=2)+"\n")
            print(group,slot,"SHARED" if shared else "MODULE",
                  "CPU",round(sample["cpu_s"],4),
                  "RSS",round(sample["rss_mib"],2),
                  "WALL",round(sample["wall_s"],4),flush=True)
    assert len({x["digest"] for x in rows})==1
    ratios=[];rss=[]
    for group in range(a.groups):
        samples=[r for r in rows if r["group"]==group]
        old=[r for r in samples if not r["shared"]]
        new=[r for r in samples if r["shared"]]
        ratio=statistics.mean(r["cpu_s"] for r in new)/statistics.mean(r["cpu_s"] for r in old)
        diff=statistics.mean(r["rss_mib"] for r in new)-statistics.mean(r["rss_mib"] for r in old)
        ratios.append(ratio);rss.append(diff)
        print("GROUP",group,"CPU ratio",round(ratio,4),"RSS delta",round(diff,2))
    print("RESULT","groups",a.groups,"scripts",a.scripts,
          "CPU ratio median",statistics.median(ratios),
          "wins",sum(x<1 for x in ratios),
          "RSS delta median",statistics.median(rss),"digest",rows[0]["digest"])

if __name__=="__main__":
    main()
