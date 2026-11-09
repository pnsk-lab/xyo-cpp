#!/usr/bin/env python3
"""Paired Math until-loop microbench, using the unmodified ScratchBench suite."""
import argparse
import json
import random
import statistics
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tests/bench/algos"))
import scratchbench_probe as probe
CASES={
    "mandel": ("math_mandel",1957,None),
    "collatz": ("math_collatz",3142,None),
    "nbody": ("math_nbody",2000,None),
}
def main():
    p=argparse.ArgumentParser()
    p.add_argument("--binary",type=Path,default=ROOT/"build/xyo")
    p.add_argument("--cases",default="mandel,collatz")
    p.add_argument("--pairs",type=int,default=4)
    p.add_argument("--toggle",choices=["ssa","fast","affine"],default="fast")
    p.add_argument("--output",type=Path,default=ROOT/"build/math004-paired.json")
    args=p.parse_args()
    names=args.cases.split(",")
    for name in names:
        probe.CASES[name]=CASES[name]
        probe.build(name)
    probe.XYO=args.binary.resolve()
    randomizer=random.Random(20261108)
    results=[]
    for pair in range(args.pairs):
        jobs=[(name,enable) for name in names for enable in (False,True)]
        randomizer.shuffle(jobs)
        for name,enable in jobs:
            flags={
              "XYO_JIT_NUMERIC_UNTIL_SSA":
                  ("1" if args.toggle != "ssa" else ("1" if enable else "0")),
              "XYO_JIT_NUMERIC_UNTIL_FAST":
                  ("1" if enable else "0") if args.toggle == "fast" else "1",
              "XYO_JIT_AFFINE_UNTIL":
                  ("1" if enable else "0") if args.toggle == "affine" else "0",
              "XYO_GPU":"0","XYO_PAR":"0","XYO_PARFOR":"0"
            }
            r=probe.run_case(name,"xyo",flags)
            if r["exit"]!=0 or not r["done"] or r["rate"] is None:
                raise RuntimeError(f"{pair} {name} {enable} {r}")
            results.append({"pair":pair,"case":name,"enabled":enable,**r})
            args.output.parent.mkdir(parents=True,exist_ok=True)
            args.output.write_text(json.dumps(results,indent=2)+"\n")
            print(pair,name, "ON" if enable else "OFF",
                  f"{r['rate']:.3f}",r["checksum"],flush=True)
    for name in names:
        rows=[r for r in results if r["case"]==name]
        checks={r["checksum"] for r in rows}
        assert len(checks)==1,(name,checks)
        on={r["pair"]:r["rate"] for r in rows if r["enabled"]}
        off={r["pair"]:r["rate"] for r in rows if not r["enabled"]}
        ratios=[on[i]/off[i] for i in range(args.pairs)]
        print("RESULT",name,statistics.median(ratios),ratios,"checksum",checks.pop(),flush=True)
if __name__=="__main__":
    main()
