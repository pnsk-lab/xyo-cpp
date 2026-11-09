#!/usr/bin/env python3
"""Verify and benchmark per-module pow10 table reuse vs per-reporter tables."""
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
    Gen, S_call, S_change, S_repeat, S_set,
    add, div, mul, num, op, var, make_project, write_sb3,
)


def generate(path, count):
    names = ["counter", "sum", "done"] + [f"v{i}" for i in range(count)]
    g = Gen(names, [])
    body = [S_change("counter", num(1))]
    for i in range(count):
        reporter = op("operator_mathop", {"OPERATOR":"10 ^"},
                      NUM=div(add(var("counter"),num(i)),num(20)))
        body.append(S_set(f"v{i}",reporter))
    statements = [S_set("counter",num(0)),S_set("sum",num(0))]
    statements += [S_set(f"v{i}",num(0)) for i in range(count)]
    statements += [S_repeat(num(65),body)]
    statements += [S_change("sum",mul(var(f"v{i}"),num(0.01)))
                   for i in range(count)]
    statements += [S_set("done",num(1))]
    g.define("run",[],statements,warp=True)
    g.script([S_call("run")])
    write_sb3(path,make_project(g.vars.keys(),g.lists.keys(),g))
    return names


def sample(binary, path, keys, duplicate, log):
    env = {k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0",
               XYO_PARFOR="0", XYO_JIT_POW10_TABLE="1",
               XYO_JIT_NUMERIC_REPEAT_FLOAT="1",
               XYO_JIT_POW10_DUPLICATE_TABLE=str(int(duplicate)))
    with log.open("wb") as out, log.with_suffix(".err").open("wb") as err:
        start=time.perf_counter()
        p=subprocess.Popen(
            [str(binary),"run",str(path),"--jit","eager",
             "--frames","5000"],env=env,cwd=ROOT,stdout=out,stderr=err)
        pid,status,usage=os.wait4(p.pid,0)
        wall=time.perf_counter()-start
        p.returncode=os.waitstatus_to_exitcode(status)
    assert pid==p.pid
    if p.returncode:
        raise AssertionError(log.with_suffix(".err").read_text()[-1800:])
    content=log.read_bytes()
    state=json.loads(content[content.index(b"{"):
                             content.rindex(b"}")+1])["Bench"]
    assert state["done"]=="1"
    observed={k:state[k] for k in keys}
    digest=hashlib.sha256(json.dumps(observed,sort_keys=True).encode()).hexdigest()
    return dict(duplicate=duplicate,wall_s=wall,
                cpu_s=usage.ru_utime+usage.ru_stime,
                rss_mib=usage.ru_maxrss/1024,digest=digest)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--binary",type=Path,
                        default=ROOT/"build/math010-ab-xyo")
    parser.add_argument("--groups",type=int,default=4)
    parser.add_argument("--reporters",type=int,default=16)
    parser.add_argument("--output",type=Path,
                        default=ROOT/"build/math010-dedup-cost.json")
    args=parser.parse_args()
    if not 2<=args.reporters<=19: parser.error("reporters must be 2..19")
    path=args.output.with_suffix(".sb3")
    names=generate(path,args.reporters)
    rows=[]
    for group in range(args.groups):
        pattern=(True,False,False,True) if group%2==0 else (
            False,True,True,False)
        for slot,duplicate in enumerate(pattern):
            file=args.output.parent/f"math010-dedup-{group}-{slot}.log"
            result=sample(args.binary.resolve(),path,names,duplicate,file)
            rows.append({"group":group,"slot":slot,**result})
            args.output.write_text(json.dumps(rows,indent=2)+"\n")
            print(group,slot,"OLD" if duplicate else "DEDUP",
                  "cpu_s",round(result["cpu_s"],3),
                  "rss_mib",round(result["rss_mib"],2),flush=True)
    assert len({r["digest"] for r in rows})==1
    for group in range(args.groups):
        r=[x for x in rows if x["group"]==group]
        old=[x for x in r if x["duplicate"]]
        new=[x for x in r if not x["duplicate"]]
        oldcpu=statistics.mean(x["cpu_s"] for x in old)
        newcpu=statistics.mean(x["cpu_s"] for x in new)
        rssdelta=statistics.mean(x["rss_mib"] for x in new)-statistics.mean(
            x["rss_mib"] for x in old)
        print(f"GROUP {group}: new/old CPU {newcpu/oldcpu:.4f}"
              f" RSS delta {rssdelta:.2f} MiB")
    print("OUTPUT_DIGEST",rows[0]["digest"])

if __name__=="__main__":
    main()
