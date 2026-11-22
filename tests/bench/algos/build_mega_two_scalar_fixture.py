#!/usr/bin/env python3
"""Emit eight Scratch counted-repeat scripts with TWO ordered scalar writes.

Each green-flag script repeats N times:
    change v{i} by i+1
    change w{i} by 2*(i+1)
Neither write is reducible to a single variable update.
"""
import argparse
import json
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED


def source(rounds: int, scripts: int = 8) -> dict:
    blocks, variables = {}, {}
    for i in range(scripts):
        idx = str(i)
        variables["v" + idx] = ["v" + idx, 0]
        variables["w" + idx] = ["w" + idx, 0]
        blocks["hat" + idx] = {
            "opcode": "event_whenflagclicked", "topLevel": True,
            "next": "repeat" + idx, "inputs": {}, "fields": {},
            "parent": None, "shadow": False
        }
        blocks["repeat" + idx] = {
            "opcode": "control_repeat", "topLevel": False,
            "next": None, "parent": "hat" + idx, "shadow": False,
            "fields": {}, "inputs": {
                "TIMES": [1, [4, str(rounds)]],
                "SUBSTACK": [2, "change" + idx]
            }
        }
        blocks["change" + idx] = {
            "opcode": "data_changevariableby", "topLevel": False,
            "next": "changeSecond" + idx, "parent": "repeat" + idx,
            "shadow": False,
            "fields": {"VARIABLE": ["v" + idx, "v" + idx]},
            "inputs": {"VALUE": [1, [4, str(i + 1)]]}
        }
        blocks["changeSecond" + idx] = {
            "opcode": "data_changevariableby", "topLevel": False,
            "next": None, "parent": "change" + idx,
            "shadow": False,
            "fields": {"VARIABLE": ["w" + idx, "w" + idx]},
            "inputs": {"VALUE": [1, [4, str(2 * (i + 1))]]}
        }
    return {"targets": [{
        "name": "Stage", "isStage": True, "variables": variables,
        "lists": {}, "broadcasts": {}, "blocks": blocks,
        "costumes": [], "sounds": [], "currentCostume": 0,
        "layerOrder": 0, "volume": 100
    }], "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--scripts", type=int, default=8)
    args = parser.parse_args()
    if args.rounds < 0 or not 2 <= args.scripts <= 8:
        parser.error("rounds must be nonnegative and scripts must be 2..8")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.out, "w", ZIP_DEFLATED) as archive:
        archive.writestr("project.json",
                         json.dumps(source(args.rounds, args.scripts)))
    print("expected", {
        **{f"v{i}": args.rounds*(i+1) for i in range(args.scripts)},
        **{f"w{i}": args.rounds*2*(i+1) for i in range(args.scripts)}
    })
