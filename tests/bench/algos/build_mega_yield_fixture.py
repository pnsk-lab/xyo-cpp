#!/usr/bin/env python3
"""Real Scratch SB3 for 2–8 independent repeat-yielding hats.

Each script:
    when green flag clicked
    repeat N
       change v_i by delta_i

The mega-yield backend must execute one body per scheduler selection and
save program counter + remaining iterations inside the owning Thread.
"""
import argparse
import json
from pathlib import Path
import zipfile

def generate(counts, increments):
    blocks = {}
    variables = {}
    for i, (n, delta) in enumerate(zip(counts, increments)):
        hat = f"hat{i}"
        loop = f"repeat{i}"
        change = f"change{i}"
        variable = f"v{i}"
        variables[variable] = [variable, 0]
        blocks[hat] = {
            "opcode": "event_whenflagclicked", "topLevel": True,
            "next": loop, "parent": None, "inputs": {}, "fields": {},
            "shadow": False, "x": 0, "y": 180 * i,
        }
        blocks[loop] = {
            "opcode": "control_repeat", "topLevel": False,
            "next": None, "parent": hat, "shadow": False,
            "fields": {}, "inputs": {
                "TIMES": [1, [4, str(n)]],
                "SUBSTACK": [2, change]
            }
        }
        blocks[change] = {
            "opcode": "data_changevariableby", "topLevel": False,
            "next": None, "parent": loop, "shadow": False,
            "fields": {"VARIABLE": [variable, variable]},
            "inputs": {"VALUE": [1, [4, str(delta)]]}
        }
    return {
        "targets": [{
            "name": "Stage", "isStage": True, "variables": variables,
            "lists": {}, "broadcasts": {}, "blocks": blocks,
            "costumes": [], "sounds": [], "currentCostume": 0,
            "layerOrder": 0, "volume": 100
        }],
        "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
    }

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--counts", default="3,2,0")
    ap.add_argument("--increments", default="1,2,4")
    args = ap.parse_args()
    counts = [int(v) for v in args.counts.split(",")]
    increments = [float(v) for v in args.increments.split(",")]
    if len(counts) != len(increments) or not (2 <= len(counts) <= 8):
        ap.error("supply 2–8 matched counts/increments")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(generate(counts, increments)))
    print("expected", [c*d for c, d in zip(counts, increments)])
