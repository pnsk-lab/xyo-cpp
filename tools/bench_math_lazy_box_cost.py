#!/usr/bin/env python3
"""MATH-017: paired LLVM JIT compilation/startup cost of lazy boxed shadows.

Generates independent Scratch warp procedures with all-integer numeric SSA
and measures process CPU time and peak RSS for the SAME binary, with an
ABBA/BAAB on/off sequence. All process outputs must match exactly.
"""
from __future__ import annotations

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
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, add, mul,
    num, op, var, make_project, write_sb3,
)

def generate(project: Path, scripts: int, width: int) -> list[str]:
    variables = [
        name for i in range(scripts)
        for name in [
            f"counter{i}", f"done{i}",
            *[f"value{i}_{j}" for j in range(width)],
        ]
    ]
    gen = Gen(variables, [])
    for i in range(scripts):
        counter = f"counter{i}"
        values = [f"value{i}_{j}" for j in range(width)]
        body = [S_change(counter, num(1))]
        for j, target in enumerate(values):
            other = values[(j + 1) % width]
            result = op(
                "operator_mod", {},
                NUM1=add(var(target), var(other)),
                NUM2=num(1000003),
            )
            body.append(S_set(target, result))
        program = [
            S_set(counter, num(0)),
            *[S_set(name, num(j+1)) for j, name in enumerate(values)],
            S_repeat(num(65), body),
            S_set(f"done{i}", num(1)),
        ]
        gen.define(f"run{i}", [], program, warp=True)
        gen.script([S_call(f"run{i}")], x=i*20, y=i*18)
    write_sb3(project, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return variables


def run(binary: Path, project: Path, keys: list[str],
        lazy: bool, filename: Path) -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_NUMERIC_REPEAT_INT": "1",
        "XYO_JIT_NUMERIC_REPEAT_U32_MOD": "1",
        "XYO_JIT_NUMERIC_REPEAT_LAZY_BOX": "1" if lazy else "0",
    })
    with filename.open("wb") as out, filename.with_suffix(".err").open("wb") as err:
        started=time.perf_counter()
        child=subprocess.Popen(
            [str(binary), "run", str(project), "--jit", "eager",
             "--frames", "5000"],
            env=env, cwd=ROOT, stdout=out, stderr=err,
        )
        pid, status, usage=os.wait4(child.pid,0)
        wall=time.perf_counter()-started
    assert pid == child.pid
    code=os.waitstatus_to_exitcode(status)
    if code:
        raise RuntimeError(
            f"lazy={lazy} exit={code}: "
            f"{filename.with_suffix('.err').read_text(errors='replace')[-1200:]}"
        )
    raw=filename.read_bytes()
    result=json.loads(raw[raw.index(b"{"):raw.rindex(b"}")+1])["Bench"]
    assert all(result[k]=="1" for k in keys if k.startswith("done"))
    digest=hashlib.sha256(json.dumps(
        {k:result[k] for k in keys},sort_keys=True).encode()).hexdigest()
    return {
        "enabled": lazy,
        "wall_s": wall,
        "cpu_s": usage.ru_utime + usage.ru_stime,
        "rss_mib": usage.ru_maxrss / 1024,
        "digest": digest,
    }


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--binary",type=Path,
                    default=ROOT/"build/math017-lazy-xyo")
    ap.add_argument("--scripts",type=int,default=24)
    ap.add_argument("--width",type=int,default=8)
    ap.add_argument("--groups",type=int,default=8)
    ap.add_argument("--out",type=Path,
                    default=ROOT/"build/math017-lazy-cost.json")
    a=ap.parse_args()
    assert 1 <= a.scripts <= 64 and 2 <= a.width <= 12
    assert a.groups >= 1
    a.out.parent.mkdir(parents=True,exist_ok=True)
    project=a.out.with_suffix(".sb3")
    keys=generate(project,a.scripts,a.width)
    rows=[]
    for group in range(a.groups):
        order=(False,True,True,False) if group%2==0 else (
            True,False,False,True)
        for slot,lazy in enumerate(order):
            log=a.out.parent/f"math017-cost-{group}-{slot}.log"
            data=run(a.binary.resolve(),project,keys,lazy,log)
            rows.append({"group":group,"slot":slot,**data})
            a.out.write_text(json.dumps(rows,indent=2)+"\n")
            print("RUN",group,slot,"LAZY" if lazy else "EAGER",
                  "CPU",round(data["cpu_s"],4),
                  "RSS",round(data["rss_mib"],3),
                  "WALL",round(data["wall_s"],4),flush=True)
    assert len({r["digest"] for r in rows})==1
    ratios=[];rss=[]
    for g in range(a.groups):
        samples=[r for r in rows if r["group"]==g]
        old=[r for r in samples if not r["enabled"]]
        new=[r for r in samples if r["enabled"]]
        cpu_ratio=(statistics.mean(r["cpu_s"] for r in new)/
                   statistics.mean(r["cpu_s"] for r in old))
        rss_diff=(statistics.mean(r["rss_mib"] for r in new)-
                  statistics.mean(r["rss_mib"] for r in old))
        ratios.append(cpu_ratio);rss.append(rss_diff)
        print("GROUP",g,"CPU_RATIO",round(cpu_ratio,5),
              "RSS_DELTA",round(rss_diff,3),flush=True)
    print("RESULT",a.scripts,"SCRIPTS",a.width,"VARS",
          "CPU_RATIO_MEDIAN",statistics.median(ratios),
          "WINS",sum(v<1 for v in ratios),"/",a.groups,
          "RSS_DELTA_MEDIAN",statistics.median(rss),
          "DIGEST",rows[0]["digest"])

if __name__=="__main__":
    main()
