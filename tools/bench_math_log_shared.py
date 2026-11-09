#!/usr/bin/env python3
"""Compare JIT ln module-global table vs one process-lifetime shared table.

Build a real warp repeat with <=16 distinct variables (SSA eligible). Compare
the same native binary in ABBA groups and require identical output digests.
Measure process CPU time, peak RSS, and wall time separately.
"""
import argparse
import hashlib
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/"tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, add, num, op, var,
    make_project, write_sb3,
)


def generate(project: Path, count: int) -> list[str]:
    names = ["i", "total", "done", *[f"v{k}" for k in range(count)]]
    g = Gen(names, [])
    body = [S_change("i", num(1))]
    for k in range(count):
        log = op("operator_mathop", {"OPERATOR":"ln"},
                 NUM=add(var("i"),num(k+1)))
        body.append(S_set(f"v{k}", log))
    statements = [S_set("i",num(0)),S_set("total",num(0))]
    statements += [S_set(f"v{k}",num(0)) for k in range(count)]
    statements += [S_repeat(num(65),body)]
    statements += [S_change("total",var(f"v{k}")) for k in range(count)]
    statements += [S_set("done",num(1))]
    g.define("run", [], statements, warp=True)
    g.script([S_call("run")])
    write_sb3(project, make_project(g.vars.keys(),g.lists.keys(),g))
    return names


def measure(binary: Path, project: Path, names, shared: bool,
            output: Path, jit: str) -> dict:
    env = {key:value for key,value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS":"json", "XYO_GPU":"0", "XYO_PAR":"0",
        "XYO_PARFOR":"0", "XYO_JIT_LOG_TABLE":"1",
        "XYO_JIT_LOG_SHARED_TABLE":"1" if shared else "0",
        "XYO_JIT_NUMERIC_REPEAT_FLOAT":"1",
        "XYO_JIT_EXP_TABLE":"0",
        "XYO_JIT_POW10_TABLE":"0",
    })
    with output.open("wb") as out, output.with_suffix(".err").open("wb") as err:
        start=time.perf_counter()
        p=subprocess.Popen(
            [str(binary),"run",str(project),"--jit",jit,"--frames","5000"],
            stdout=out,stderr=err,env=env,cwd=ROOT)
        pid,status,resource=os.wait4(p.pid,0)
        elapsed=time.perf_counter()-start
    assert pid==p.pid
    exitcode=os.waitstatus_to_exitcode(status)
    if exitcode:
        raise RuntimeError(
            f"process exit {exitcode}: "
            f"{output.with_suffix('.err').read_text(errors='replace')[-2000:]}")
    content=output.read_bytes()
    state=json.loads(
        content[content.index(b"{"):content.rindex(b"}")+1])["Bench"]
    assert state["done"]=="1"
    observed={name:state[name] for name in names}
    digest=hashlib.sha256(json.dumps(observed,sort_keys=True).encode()).hexdigest()
    return {
        "shared":shared,"wall_s":elapsed,
        "cpu_s":resource.ru_utime+resource.ru_stime,
        "rss_mib":resource.ru_maxrss/1024,
        "digest":digest,
    }


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--binary",type=Path,
                        default=ROOT/"build/math010-ab-xyo")
    parser.add_argument("--groups",type=int,default=8)
    parser.add_argument("--reporters",type=int,default=12)
    parser.add_argument("--jit",choices=("eager","hotspot"),default="eager")
    parser.add_argument("--output",type=Path,
                        default=ROOT/"build/math011-shared-log-cost.json")
    args=parser.parse_args()
    if not 2 <= args.reporters <= 13:
        parser.error("require 2<=reporters<=13 so SSA variable cap is respected")
    args.output.parent.mkdir(parents=True,exist_ok=True)
    path=args.output.with_suffix(".sb3")
    names=generate(path,args.reporters)
    rows=[]
    for group in range(args.groups):
        order=(False,True,True,False) if group%2==0 else (
            True,False,False,True)
        for slot,shared in enumerate(order):
            logfile=args.output.parent / f"math011-shared-{group}-{slot}.log"
            result=measure(args.binary.resolve(),path,names,shared,logfile,args.jit)
            rows.append({"group":group,"slot":slot,**result})
            args.output.write_text(json.dumps(rows,indent=2)+"\n")
            print(group,slot,"SHARED" if shared else "MODULE",
                  "cpu",round(result["cpu_s"],4),
                  "rssMiB",round(result["rss_mib"],2),
                  "wall",round(result["wall_s"],3),flush=True)
    assert len({r["digest"] for r in rows})==1
    ratios=[];mem=[]
    for group in range(args.groups):
        r=[item for item in rows if item["group"]==group]
        old=[x for x in r if not x["shared"]]
        new=[x for x in r if x["shared"]]
        ratio=statistics.mean(x["cpu_s"] for x in new)/statistics.mean(
            x["cpu_s"] for x in old)
        delta=statistics.mean(x["rss_mib"] for x in new)-statistics.mean(
            x["rss_mib"] for x in old)
        ratios.append(ratio);mem.append(delta)
        print(f"GROUP {group}: CPU ratio {ratio:.4f}, RSS delta {delta:.2f} MiB")
    print("RESULT", "CPU ratio median",statistics.median(ratios),
          "wins",sum(v<1 for v in ratios),"/",len(ratios),
          "RSS MiB delta median",statistics.median(mem),
          "digest",rows[0]["digest"])

if __name__=="__main__":
    main()
