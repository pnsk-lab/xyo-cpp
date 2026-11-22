#!/usr/bin/env python3
"""Source-certified original bench_begin + bench_round Mega integration."""
import copy
import json
from pathlib import Path
import tempfile
import zipfile

from bench_mega_outer_until import block, graft_real_bench_round
from test_mega_outer_final_set import project, run as base_run

def graft_begin(project_json, original):
    with zipfile.ZipFile(original) as z:
        src = json.loads(z.read("project.json"))
    original_blocks = next(t for t in src["targets"] if t["isStage"])["blocks"]
    definition = None
    for key, node in original_blocks.items():
        if node["opcode"] != "procedures_definition":
            continue
        custom = node["inputs"]["custom_block"][1]
        if original_blocks[custom].get("mutation", {}).get("proccode") == "bench_begin":
            definition = key
            break
    assert definition
    keys = [definition]
    imported = {}
    while keys:
        key = keys.pop()
        if key in imported:
            continue
        node = original_blocks[key]
        imported[key] = copy.deepcopy(node)
        if isinstance(node.get("next"), str):
            keys.append(node["next"])
        for value in node.get("inputs", {}).values():
            if isinstance(value[1], str):
                keys.append(value[1])
    target = project_json["targets"][0]
    assert not set(imported) & set(target["blocks"])
    target["blocks"].update(imported)
    target["blocks"]["hat"]["next"] = "beginCall"
    target["blocks"]["beginCall"] = block("procedures_call", "outer")
    proto = original_blocks[original_blocks[definition]["inputs"]["custom_block"][1]]
    target["blocks"]["beginCall"]["mutation"] = copy.deepcopy(
        next(node["mutation"] for node in original_blocks.values()
             if node["opcode"] == "procedures_call" and
             node.get("mutation", {}).get("proccode") == "bench_begin"))
    for name in ["b_round", "b_total", "b_chunks", "b_bestrate", "b_chk"]:
        target["variables"][name] = [name, 27]
    return definition

def run_begin(p, enabled, label, directory):
    # The base test uses the outer-post-set flag to toggle Mega. Keep
    # it enabled for both branches and toggle only the bench-begin flag.
    import os, subprocess, re
    path = directory / (label + ".sb3")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(p))
    env = {k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE":"eager", "XYO_PAR":"0", "XYO_YIELD_QMODE":"off",
        "XYO_JIT_MEGA_BATCH_YIELD":"1", "XYO_JIT_MEGA_SINGLE_SENDER":"1",
        "XYO_JIT_MEGA_UNTIL_BCAST":"1", "XYO_JIT_MEGA_UNTIL_OUTER":"1",
        "XYO_JIT_MEGA_UNTIL_POST_SET":"1",
        "XYO_JIT_MEGA_UNTIL_OUTER_POST_SET":"1",
        "XYO_JIT_MEGA_UNTIL_BENCH_ROUND":"1",
        "XYO_JIT_MEGA_UNTIL_BENCH_BEGIN":"1" if enabled else "0",
        "XYO_JIT_MEGA_NORMALIZE_YIELD":"1",
        "XYO_DUMPVARS":"json", "XYO_JIT_MEGA_BATCH_TRACE":"1",
    })
    binary = Path(__file__).resolve().parents[3]/"build-scheduler-rewrite-20261111/xyo"
    r = subprocess.run([str(binary),"run",str(path),"--fps","0"],
                       cwd=directory,env=env,text=True,capture_output=True,timeout=30)
    assert r.returncode == 0, (label,r.stderr[-2000:])
    match = re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}',r.stdout,re.S)
    assert match, (label,r.stdout)
    return json.loads(match.group())["Stage"], "[mega-yield]" in r.stderr

def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--source",type=Path,required=True)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mega-begin-") as temp:
        root = Path(temp)
        for repeats in (0,1,3):
            p = project(repeats)
            graft_real_bench_round(p,args.source)
            definition = graft_begin(p,args.source)
            outputs=[]
            for enabled in (False,True):
                data,installed=run_begin(p,enabled,f"begin-{repeats}-{enabled}",root)
                assert installed==enabled,(repeats,enabled,installed)
                assert float(data["b_round"])==repeats,(repeats,enabled,data)
                assert float(data["b_chk"])==(-1 if repeats==0 else 0),data
                assert float(data["b_total"])==0,(repeats,enabled,data)
                assert float(data["b_chunks"])==0,(repeats,enabled,data)
                outputs.append(data)
            stable = ("b_round", "b_chk", "b_total", "b_chunks",
                      "b_bestrate", "sum", "total", "hits")
            assert all(outputs[0][key] == outputs[1][key] for key in stable), (
                repeats, outputs)
            print(f"PASS original bench_begin+bench_round, repeat={repeats}",flush=True)
        p=project(0)
        graft_real_bench_round(p,args.source)
        definition=graft_begin(p,args.source)
        nodes=p["targets"][0]["blocks"]
        first=nodes[definition]["next"]
        nodes[first]["inputs"]["VALUE"]=[1,[4,"1"]]
        data,installed=run_begin(p,True,"altered-constant",root)
        assert not installed and float(data["b_round"])==1,(installed,data)
        print("PASS altered bench_begin constant rejected",flush=True)
        p=project(0)
        graft_real_bench_round(p,args.source)
        definition=graft_begin(p,args.source)
        nodes=p["targets"][0]["blocks"]
        first=nodes[definition]["next"]
        nodes[first]["fields"]["VARIABLE"]=["b_total","b_total"]
        data,installed=run_begin(p,True,"altered-order",root)
        assert not installed
        print("PASS altered bench_begin write order rejected",flush=True)
        p=project(0)
        graft_real_bench_round(p,args.source)
        definition=graft_begin(p,args.source)
        nodes=p["targets"][0]["blocks"]
        current=definition
        while nodes[current]["next"]:
            current=nodes[current]["next"]
        nodes[current]["next"]="extra-begin"
        nodes["extra-begin"]=block(
            "data_changevariableby",
            inputs={"VALUE":[1,[4,"100"]]},
            fields={"VARIABLE":["sum","sum"]})
        data,installed=run_begin(p,True,"extra-begin-effect",root)
        assert not installed and float(data["sum"])==100,(installed,data)
        print("PASS extra bench_begin side effect falls back to Fiber",flush=True)

if __name__=="__main__":
    main()
