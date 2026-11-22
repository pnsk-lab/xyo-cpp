#!/usr/bin/env python3
"""Run the original ScratchBench sched_pingpong with its original receivers."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[3]
BIN = ROOT / "build-scheduler-rewrite-20261111/xyo"

def rewrite(source, budget, fixed_hits=None, post_change=False, post_procedure=False):
    with zipfile.ZipFile(source) as z:
        obj=json.loads(z.read("project.json"))
    stage=next(t for t in obj["targets"] if t["isStage"])
    blocks=stage["blocks"]
    hats=[key for key,b in blocks.items()
          if b["opcode"]=="event_whenflagclicked" and b.get("topLevel")]
    assert len(hats)==1,hats
    root=hats[0]
    blocks[root]["next"]="test-pingpong-call"
    calls=[copy.deepcopy(node) for node in blocks.values()
           if node["opcode"]=="procedures_call"
           and node.get("mutation",{}).get("proccode")=="sched_pingpong"]
    assert calls
    blocks["test-pingpong-call"]=calls[0]
    blocks["test-pingpong-call"]["next"]=None
    blocks["test-pingpong-call"]["parent"]=root
    if post_change:
        # One exact, source-certified caller return suffix. This is the
        # supported narrow boundary for integrating into a larger caller;
        # an arbitrary following procedure chain is *not* supported.
        stage["variables"]["after_pingpong"]=["after_pingpong",0]
        blocks["test-pingpong-call"]["next"]="after-pingpong-change"
        blocks["after-pingpong-change"]={
            "opcode":"data_changevariableby","next":None,
            "parent":"test-pingpong-call",
            "inputs":{"VALUE":[1,[4,"17"]]},
            "fields":{"VARIABLE":["after_pingpong","after_pingpong"]},
        }
    if post_procedure:
        # Model the heterogeneous next call in suite_sched: it must be
        # executed by Fiber rather than silently dropped by a Mega tail.
        stage["variables"]["after_pingpong"]=["after_pingpong",0]
        blocks["test-pingpong-call"]["next"]="after-pingpong-call"
        blocks["after-pingpong-call"]={
            "opcode":"procedures_call","next":None,
            "parent":"test-pingpong-call","inputs":{},"fields":{},
            "mutation":{"tagName":"mutation","proccode":"after_pingpong",
                        "argumentids":"[]","warp":"false"},
        }
        blocks["after-pingpong-def"]={
            "opcode":"procedures_definition","next":"after-pingpong-write",
            "parent":None,"topLevel":True,
            "inputs":{"custom_block":[1,"after-pingpong-proto"]},
            "fields":{},
        }
        blocks["after-pingpong-proto"]={
            "opcode":"procedures_prototype","next":None,
            "parent":"after-pingpong-def","shadow":True,
            "inputs":{},"fields":{},
            "mutation":{"tagName":"mutation","proccode":"after_pingpong",
                        "argumentids":"[]","argumentnames":"[]",
                        "argumentdefaults":"[]","warp":"false"},
        }
        blocks["after-pingpong-write"]={
            "opcode":"data_changevariableby","next":None,
            "parent":"after-pingpong-def",
            "inputs":{"VALUE":[1,[4,"17"]]},
            "fields":{"VARIABLE":["after_pingpong","after_pingpong"]},
        }
    stage["variables"]["b_budget"]=["b_budget",budget]
    if fixed_hits is not None:
        # The only workload mutation: change the original time reporter
        # into a pure live counter read. Keep all original receiver scripts.
        # sc_hits is reset once per outer round and incremented once per
        # sc_ping broadcast, making exactly fixed_hits sends per round.
        cond=blocks["~TY"]
        assert cond["opcode"]=="operator_lt"
        cond["inputs"]["OPERAND1"]=[3,[12,"sc_hits","sc_hits"],[10,""]]
        stage["variables"]["b_budget"]=["b_budget",fixed_hits]
    stage["lists"]["output"]=["output",[]]
    return obj

def run(project,directory,enabled):
    file=directory / ("native.sb3" if enabled else "fiber.sb3")
    with zipfile.ZipFile(file,"w",zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json",json.dumps(project))
    env={k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE":"eager", "XYO_PAR":"0", "XYO_YIELD_QMODE":"off",
        "XYO_JIT_MEGA_BATCH_YIELD":"1", "XYO_JIT_MEGA_SINGLE_SENDER":"1",
        "XYO_JIT_MEGA_UNTIL_BCAST":"1", "XYO_JIT_MEGA_UNTIL_OUTER":"1",
        "XYO_JIT_MEGA_UNTIL_POST_SET":"1",
        "XYO_JIT_MEGA_UNTIL_OUTER_POST_SET":"1",
        "XYO_JIT_MEGA_UNTIL_BENCH_ROUND":"1",
        "XYO_JIT_MEGA_UNTIL_BENCH_BEGIN":"1",
        "XYO_JIT_MEGA_UNTIL_BENCH_END":"1" if enabled else "0",
        "XYO_JIT_MEGA_UNTIL_VAR_PAIR":"1" if enabled else "0",
        "XYO_JIT_MEGA_FAST_VAR_PAIR_JOIN": "1" if enabled and
            os.getenv("XYO_TEST_MEGA_FAST_PAIR") == "1" else "0",
        "XYO_JIT_MEGA_NORMALIZE_YIELD":"1",
        "XYO_DUMPVARS":"json", "XYO_DUMPLISTS":"json",
        "XYO_JIT_MEGA_BATCH_TRACE":"1",
    })
    res=subprocess.run([str(BIN),"run",str(file),"--fps","0"],env=env,
        cwd=directory,text=True,capture_output=True,timeout=50)
    assert res.returncode==0,(enabled,res.returncode,res.stderr[-2000:])
    match=re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}',res.stdout,re.S)
    assert match,res.stdout[-1600:]
    stage=json.loads(match.group())["Stage"]
    return stage,"[mega-yield]" in res.stderr,res.stderr

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--source",type=Path,required=True)
    ap.add_argument("--budget",type=float,default=-1)
    ap.add_argument("--fixed-hits",type=int,default=None)
    ap.add_argument("--post-change",action="store_true")
    ap.add_argument("--post-procedure",action="store_true")
    args=ap.parse_args()
    if args.fixed_hits is not None and not 0 <= args.fixed_hits <= 100000:
        ap.error("--fixed-hits must be in [0,100000]")
    if args.post_change and args.post_procedure:
        ap.error("--post-change and --post-procedure are mutually exclusive")
    p=rewrite(args.source,args.budget,args.fixed_hits,
              args.post_change,args.post_procedure)
    with tempfile.TemporaryDirectory(prefix="real-pingpong-") as tmp:
        directory=Path(tmp)
        fiber,_,fiber_err=run(p,directory,False)
        mega,installed,mega_err=run(p,directory,True)
        assert installed == (not args.post_procedure),mega_err[-2000:]
        if args.post_procedure:
            assert fiber["after_pingpong"]==mega["after_pingpong"]=="17",(
                fiber.get("after_pingpong"),mega.get("after_pingpong"))
            print("Heterogeneous following procedure: safe Fiber fallback, "
                  "effect executed exactly once")
        if args.post_change:
            assert fiber["after_pingpong"]==mega["after_pingpong"]=="17",(
                fiber.get("after_pingpong"),mega.get("after_pingpong"))
            print("Source-certified caller return suffix executed exactly once")
        if args.fixed_hits is not None:
            for name in ("b_chk","b_round","b_chunks","sc_hits"):
                assert fiber.get(name)==mega.get(name),(
                    name,fiber.get(name),mega.get(name))
            assert int(float(fiber["b_chk"])) == args.fixed_hits
            assert fiber["b_round"] == "5"
            assert int(float(fiber["b_chunks"])) == args.fixed_hits * 4
            # b_total, b_ms and the formatted rates are real-clock
            # measurements even though the broadcast work is fixed.
            # Compare only the invariant fields of the output record.
            assert len(fiber["output"]) == len(mega["output"]) == 1
            f_fields=fiber["output"][0].split(" | ")
            m_fields=mega["output"][0].split(" | ")
            assert len(f_fields)==len(m_fields)==6
            for index in (0,2,5):
                assert f_fields[index]==m_fields[index],(
                    index,f_fields,m_fields)
            print(f"Fixed work {args.fixed_hits} broadcasts/round: "
                  "Fiber/Mega invariant output parity")
        elif args.budget < 0:
            for name in ("output","b_chk","b_round","b_total","b_chunks",
                         "b_bestrate","sc_hits"):
                if name in fiber or name in mega:
                    assert fiber.get(name)==mega.get(name),(
                        name,fiber.get(name),mega.get(name))
            print("Real sched_pingpong source: Fiber/Mega outputs agree")
        else:
            # Wall-clock-bound repeat: number of broadcast/join turns and
            # measured durations legitimately vary across separate runs.
            assert len(fiber["output"]) == len(mega["output"]) == 1
            assert fiber["b_round"] == mega["b_round"] == "5"
            assert float(fiber["b_chk"]) >= 0 and float(mega["b_chk"]) >= 0
            print("Real sched_pingpong timed runs complete; not an exact "
                  "wall-clock output parity claim")
        print("Mega installed:",installed)
        print("Output entries:",fiber.get("output"),mega.get("output"))
        if not installed:
            print("NOTE: native call was not installed; receiver eligibility remains")
            print("Mega trace:",mega_err[-1200:])

if __name__=="__main__":
    main()
