#!/usr/bin/env python3
"""A real two-script Scratch test for switch(pc) mega-batch execution."""
import argparse
import json
from pathlib import Path
import zipfile

def script(i, n):
    return {
        f"hat{i}": {"opcode": "event_whenflagclicked", "next": f"set{i}",
                    "inputs": {}, "fields": {}, "topLevel": True,
                    "parent": None, "shadow": False, "x": 0, "y": i * 100},
        f"set{i}": {"opcode": "data_setvariableto", "next": None,
                   "parent": f"hat{i}", "inputs": {
                       "VALUE": [1, [4, str(n)]]
                   }, "fields": {"VARIABLE": [f"v{i}", f"v{i}"]},
                   "topLevel": False, "shadow": False},
    }

def generate(count):
    blocks = {}
    variables = {}
    for i in range(count):
        blocks.update(script(i, i + 5))
        variables[f"v{i}"] = [f"v{i}", 0]
    return {
        "targets": [{
            "name": "Stage", "isStage": True,
            "variables": variables, "lists": {},
            "broadcasts": {}, "blocks": blocks, "costumes": [],
            "sounds": [], "currentCostume": 0, "layerOrder": 0,
            "volume": 100
        }],
        "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
    }

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--scripts", type=int, default=3)
    a = ap.parse_args()
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(a.out, "w", compression=zipfile.ZIP_DEFLATED) as f:
        f.writestr("project.json", json.dumps(generate(a.scripts)))
    print(f"created {a.scripts} independent greenflag Scratch scripts")
