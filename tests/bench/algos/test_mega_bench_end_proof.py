#!/usr/bin/env python3
"""Audit the original bench_end AST in the Mega source gate, without lowering."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

from bench_mega_outer_until import graft_real_bench_round
from test_mega_outer_final_set import project
from test_mega_bench_begin import graft_begin
from test_mega_real_bench_end import extract

ROOT = Path(__file__).resolve().parents[3]
BIN = ROOT / "build-scheduler-rewrite-20261111/xyo"

def create(source: Path, repeats: int = 0) -> dict:
    p = project(repeats)
    graft_real_bench_round(p, source)
    graft_begin(p, source)
    stage = p["targets"][0]
    original_end = extract(source)["targets"][0]
    blocks = stage["blocks"]
    for key, value in original_end["blocks"].items():
        if key in ("hat", "benchCall"):
            continue
        assert key not in blocks, key
        blocks[key] = copy.deepcopy(value)
    blocks["final"]["next"] = "postBenchEnd"
    blocks["postBenchEnd"] = copy.deepcopy(original_end["blocks"]["benchCall"])
    blocks["postBenchEnd"]["parent"] = "final"
    stage["lists"]["output"] = ["output", []]
    return p

def run(p, label, folder, native=False, request_native=None):
    path = folder / (label + ".sb3")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(p))
    if request_native is None:
        request_native = native
    env = {k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE":"eager", "XYO_PAR":"0", "XYO_YIELD_QMODE":"off",
        "XYO_JIT_MEGA_BATCH_YIELD":"1", "XYO_JIT_MEGA_SINGLE_SENDER":"1",
        "XYO_JIT_MEGA_UNTIL_BCAST":"1", "XYO_JIT_MEGA_UNTIL_OUTER":"1",
        "XYO_JIT_MEGA_UNTIL_POST_SET":"1",
        "XYO_JIT_MEGA_UNTIL_OUTER_POST_SET":"1",
        "XYO_JIT_MEGA_UNTIL_BENCH_ROUND":"1",
        "XYO_JIT_MEGA_UNTIL_BENCH_BEGIN":"1",
        "XYO_JIT_MEGA_BENCH_END_AUDIT":"1",
        "XYO_JIT_MEGA_UNTIL_BENCH_END":"1" if request_native else "0",
        "XYO_JIT_MEGA_NORMALIZE_YIELD":"1",
        "XYO_DUMPVARS":"json", "XYO_DUMPLISTS":"json",
        "XYO_JIT_MEGA_BATCH_TRACE":"1",
    })
    result = subprocess.run([str(BIN),"run",str(path),"--fps","0"],
        env=env,cwd=folder,capture_output=True,text=True,timeout=30)
    assert result.returncode == 0, (label,result.stderr[-1500:])
    installed = "[mega-yield]" in result.stderr
    if installed != native:
        raise AssertionError((label,native,installed,result.stderr))
    return result.stdout, result.stderr

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, required=True)
    args=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="mega-end-proof-") as tmp:
        root=Path(tmp)
        orig=create(args.source)
        stdout,stderr=run(orig, "real", root)
        assert "[mega-bench-end-proof] certified=1 fallback=1" in stderr,stderr
        assert '"output": [' in stdout,stdout
        native_stdout,native_stderr=run(orig,"real-native",root,native=True)
        assert '"output": [' in native_stdout,native_stdout
        assert "[mega-bench-end-proof] certified=1 fallback=0" in native_stderr, native_stderr
        import re
        def parse(out):
            match = re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}',out,re.S)
            assert match,out
            return json.loads(match.group())["Stage"]
        fiber_data,native_data=parse(stdout),parse(native_stdout)
        assert fiber_data["output"]==native_data["output"],(
            fiber_data["output"],native_data["output"])
        for key in ("b_chk","b_round","b_total","b_chunks","b_bestrate"):
            assert fiber_data[key]==native_data[key],(key,fiber_data,native_data)
        print("PASS real bench_end: native Mega and Fiber output list parity",flush=True)
        for repeats in (1, 3):
            candidate=create(args.source,repeats)
            baseline,_=run(candidate,f"repeat-{repeats}-fiber",root)
            mega,_=run(candidate,f"repeat-{repeats}-mega",root,native=True)
            baseline_data,mega_data=parse(baseline),parse(mega)
            assert baseline_data["output"]==mega_data["output"],(
                repeats,baseline_data["output"],mega_data["output"])
            assert baseline_data["b_round"]==mega_data["b_round"],(
                repeats,baseline_data["b_round"],mega_data["b_round"])
            print(f"PASS bench_end repeat={repeats}: list parity",flush=True)
        for values in (
            {"b_total":12.34567,"b_chunks":7,"b_bestrate":2.33333,
             "b_n":"xyz"},
            {"b_total":2.5,"b_chunks":3,"b_bestrate":-1.55555,
             "b_n":-42},
            {"b_total":0,"b_chunks":9,"b_bestrate":2,
             "b_n":"hello"},
        ):
            candidate=create(args.source)
            stage=candidate["targets"][0]
            # Start directly at the outer repeat, without bench_begin,
            # so the initial nonzero scalar values reach bench_end.
            stage["blocks"]["hat"]["next"]="outer"
            for key,value in values.items():
                stage["variables"][key]=[key,value]
            label=str(values["b_n"])
            baseline,_=run(candidate,"rounding-"+label+"-fiber",root)
            native_out,_=run(candidate,"rounding-"+label+"-mega",root,native=True)
            fiber_data,native_data=parse(baseline),parse(native_out)
            assert fiber_data["output"]==native_data["output"],(
                values,fiber_data["output"],native_data["output"])
            print(f"PASS nonzero arithmetic and rounding {values}",flush=True)

        for value in (0, 1, 7, 42, "hello", "0042", -13.5):
            candidate=copy.deepcopy(orig)
            candidate["targets"][0]["variables"]["b_n"]=["b_n",value]
            baseline,_=run(candidate,f"value-{str(value)}-fiber",root)
            native_run,_=run(candidate,f"value-{str(value)}-mega",root,native=True)
            baseline_data,native_data=parse(baseline),parse(native_run)
            assert baseline_data["output"]==native_data["output"],(
                value,baseline_data["output"],native_data["output"])
            print(f"PASS bench_end dynamic b_n={value!r}: list parity",flush=True)

        cases = {}
        mutated=copy.deepcopy(orig)
        mutated["targets"][0]["blocks"]["~MU"]["opcode"]="operator_multiply"
        cases["unexpected-opcode"]=mutated

        mutated=copy.deepcopy(orig)
        mutated["targets"][0]["blocks"]["~Md"]["inputs"]["STRING2"]=[1,[10," changed "]]
        cases["changed-separator"]=mutated

        mutated=copy.deepcopy(orig)
        mutated["targets"][0]["blocks"]["~M1"]["fields"]["LIST"]=["different","different"]
        cases["list-target-change"]=mutated

        mutated=copy.deepcopy(orig)
        mutated["targets"][0]["blocks"]["~Mi"]["inputs"]["NUM2"]=[1,[4,"2"]]
        cases["replaced-argument"]=mutated

        mutated=copy.deepcopy(orig)
        # Alter the inline Scratch primitive [12, name, id] while
        # keeping its human-readable variable name unchanged.
        mutated["targets"][0]["blocks"]["~Mh"]["inputs"]["NUM1"][1][2]="b_chunks"
        cases["variable-id-alias"]=mutated

        mutated=copy.deepcopy(orig)
        mutated["targets"][0]["blocks"]["~M1"]["fields"]["LIST"][1]="other_list"
        cases["list-id-alias"]=mutated

        mutated=copy.deepcopy(orig)
        blocks=mutated["targets"][0]["blocks"]
        blocks["custom-external-log-proto"]={
            "opcode":"procedures_prototype","next":None,
            "parent":"custom-external-log-def","inputs":{},
            "fields":{},"shadow":True,"topLevel":False,
            "mutation":{"tagName":"mutation",
                "proccode":"​​log​​ %s","argumentids":"[]",
                "argumentnames":"[]","argumentdefaults":"[]",
                "warp":"false"},
        }
        blocks["custom-external-log-def"]={
            "opcode":"procedures_definition","next":None,
            "parent":None,"topLevel":True,
            "inputs":{"custom_block":[1,"custom-external-log-proto"]},
            "fields":{},
        }
        cases["new-external-definition"]=mutated

        mutated=copy.deepcopy(orig)
        mutated["targets"][0]["blocks"]["~MT"]["next"]="extraEffect"
        mutated["targets"][0]["blocks"]["extraEffect"]={
            "opcode":"data_changevariableby","next":None,"parent":"~MT",
            "inputs":{"VALUE":[1,[4,"1"]]},
            "fields":{"VARIABLE":["b_chk","b_chk"]},
        }
        cases["extra-log-statement"]=mutated

        absent=copy.deepcopy(orig)
        del absent["targets"][0]["lists"]["output"]
        _,stderr=run(absent,"missing-output-list",root,
                     native=False,request_native=True)
        assert "[mega-yield]" not in stderr
        print("PASS missing output list rejects Mega installation",flush=True)

        for label, mutated in cases.items():
            _,stderr=run(mutated,label,root,native=False,request_native=True)
            assert "[mega-bench-end-proof] certified=0 fallback=1" in stderr,(
                label,stderr)
            print(f"PASS rejected altered source: {label}",flush=True)

if __name__=="__main__":
    main()
