#!/usr/bin/env python3
"""Paired fixed-work Mega/Fiber benchmark using real ScratchBench receivers.

The sched_pingpong body and receivers are unmodified. The time-limited
until reporter is replaced by sc_hits < b_budget, yielding a fixed count
of synchronized sc_ping broadcasts per round. The wall time includes
process startup, SB3 loading and LLVM compilation: not kernel-only timing.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time
import zipfile

from test_mega_real_pingpong_full import ROOT, BIN, rewrite

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--hits", type=int, default=200)
    parser.add_argument("--pairs", type=int, default=12)
    parser.add_argument("--cpu", type=int, default=None)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--fast-pair", action="store_true")
    parser.add_argument("--compare-fast-pair", action="store_true")
    parser.add_argument("--single-receiver", action="store_true")
    parser.add_argument("--restart-reuse", action="store_true")
    parser.add_argument("--profile-broadcast", action="store_true")
    parser.add_argument("--profile-push", action="store_true")
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args()
    assert args.hits >= 1 and args.pairs >= 1
    p = rewrite(args.source, budget=args.hits, fixed_hits=args.hits)
    with tempfile.TemporaryDirectory(prefix="mega-fixed-work-") as tmp:
        folder=Path(tmp)
        path=folder/"fixed-work.sb3"
        with zipfile.ZipFile(path,"w",zipfile.ZIP_DEFLATED) as z:
            z.writestr("project.json",json.dumps(p))
        cmd=[str(BIN),"run",str(path),"--fps","0"]
        if args.cpu is not None:
            cmd=["taskset","-c",str(args.cpu),*cmd]
        env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
        env.update({
            "XYO_JIT_MODE":"eager",
            "XYO_PAR":"0", "XYO_YIELD_QMODE":"off",
            "XYO_JIT_MEGA_BATCH_YIELD":"1",
            "XYO_JIT_MEGA_SINGLE_SENDER":"1",
            "XYO_JIT_MEGA_UNTIL_BCAST":"1",
            "XYO_JIT_MEGA_UNTIL_OUTER":"1",
            "XYO_JIT_MEGA_UNTIL_POST_SET":"1",
            "XYO_JIT_MEGA_UNTIL_OUTER_POST_SET":"1",
            "XYO_JIT_MEGA_UNTIL_BENCH_ROUND":"1",
            "XYO_JIT_MEGA_UNTIL_BENCH_BEGIN":"1",
            "XYO_JIT_MEGA_UNTIL_VAR_PAIR":"1",
            "XYO_JIT_MEGA_NORMALIZE_YIELD":"1",
            "XYO_DUMPVARS":"json", "XYO_DUMPLISTS":"json",
            "XYO_JIT_MEGA_BATCH_TRACE":"1",
            "XYO_JIT_MEGA_JOIN_PROFILE":"1" if args.profile else "0",
            "XYO_JIT_MEGA_SINGLE_RECEIVER":"1" if args.single_receiver else "0",
            "XYO_RESTART_THREAD_REUSE":"1" if args.restart_reuse else "0",
            "XYO_BCAST_WAIT_PROFILE":"1" if args.profile_broadcast else "0",
            "XYO_PUSH_THREAD_PHASE_PROFILE":"1" if args.profile_push else "0",
        })
        def run(native, fast_pair=None):
            if fast_pair is None:
                fast_pair=args.fast_pair
            env["XYO_JIT_MEGA_UNTIL_BENCH_END"]="1" if native else "0"
            env["XYO_JIT_MEGA_FAST_VAR_PAIR_JOIN"]="1" if native and fast_pair else "0"
            start=time.perf_counter_ns()
            res=subprocess.run(cmd,cwd=folder,env=env,text=True,
                capture_output=True,timeout=120)
            elapsed=(time.perf_counter_ns()-start)*1e-9
            assert res.returncode==0,(native,res.returncode,res.stderr[-800:])
            installed="[mega-yield]" in res.stderr
            assert installed==native,(native,installed,res.stderr[-800:])
            match=re.search(r'"Stage"\s*:\s*',res.stdout)
            assert match,res.stdout[-800:]
            values,_=json.JSONDecoder().raw_decode(res.stdout[match.end():])
            assert int(float(values["b_chk"]))==args.hits,values
            assert int(float(values["b_chunks"]))==args.hits*4,values
            assert values["b_round"]=="5",values
            fields=values["output"][0].split(" | ")
            assert fields[0]=="sched_pingpong" and fields[2]==str(args.hits*4) and fields[5]==str(args.hits),fields
            if args.profile and native:
                lines=[line for line in res.stderr.splitlines()
                       if line.startswith("[mega-join-prof]")]
                assert len(lines)==1,(native,lines,res.stderr[-500:])
                print(lines[0],flush=True)
            if args.profile_broadcast and native:
                lines=[line for line in res.stderr.splitlines()
                       if line.startswith("[bcast-wait-prof]")]
                assert len(lines)==1,(native,lines,res.stderr[-500:])
                print(lines[0],flush=True)
            if args.profile_push and native:
                lines=[line for line in res.stderr.splitlines()
                       if line.startswith("[push-phase-prof]")]
                assert len(lines)==1,(native,lines,res.stderr[-500:])
                print(lines[0],flush=True)
            return elapsed
        # Independent warmups trigger JIT compilation and catch any
        # cross-mode configuration or output regression before timing.
        direct=args.compare_fast_pair
        warmup=(
            {"baseline":run(True,False),"fast":run(True,True)}
            if direct else
            {"fiber":run(False),"mega":run(True)}
        )
        data=[]
        for i in range(args.pairs):
            order=[True,False] if i%2==0 else [False,True]
            sample={}
            for mode in order:
                if args.compare_fast_pair:
                    sample["fast" if mode else "baseline"]=run(True,mode)
                else:
                    sample["mega" if mode else "fiber"]=run(mode)
            sample["pair"]=i+1
            if direct:
                sample["speedup"]=sample["baseline"]/sample["fast"]
                print("pair=%d baseline=%.6fs fast=%.6fs speedup=%.3fx"%(
                    i+1,sample["baseline"],sample["fast"],sample["speedup"]),
                    flush=True)
            else:
                sample["speedup"]=sample["fiber"]/sample["mega"]
                print("pair=%d fiber=%.6fs mega=%.6fs speedup=%.3fx"%(
                    i+1,sample["fiber"],sample["mega"],sample["speedup"]),
                    flush=True)
            data.append(sample)
        fiber=[s["baseline" if direct else "fiber"]
               for s in data]
        mega=[s["fast" if direct else "mega"]
              for s in data]
        # Geometric mean of paired ratios (not a ratio of separate means).
        gm=math.exp(statistics.mean(math.log(s["speedup"]) for s in data))
        report={
            "source":str(args.source),"hits_per_round":args.hits,
            "rounds":5,"pair_count":args.pairs,
            "cpu":args.cpu,"scope":"whole process including JIT compile",
            "fast_pair":args.fast_pair,"profiling":args.profile,
            "single_receiver":args.single_receiver,
            "restart_reuse":args.restart_reuse,
            "comparison":"mega fast-pair ON / OFF"
                if args.compare_fast_pair else "mega / fiber",
            "warmup_sec":warmup,
            "baseline_median_sec":statistics.median(fiber),
            "experimental_median_sec":statistics.median(mega),
            "paired_geomean_speedup":gm,"pairs":data,
        }
        print("SUMMARY",json.dumps({k:v for k,v in report.items() if k not in ("pairs","warmup_sec")}),flush=True)
        if args.out:
            args.out.parent.mkdir(parents=True,exist_ok=True)
            args.out.write_text(json.dumps(report,indent=2)+"\n")
if __name__=="__main__":
    main()
