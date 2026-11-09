#!/usr/bin/env python3
"""Build a small, reproducible GPU-effect stress case from an SB3 costume.

Each visible sprite changes one or two Scratch effects once per frame.
No assets or script changes are made to the source SB3.
"""
import argparse
import json
import zipfile

EFFECTS = (
    ("COLOR", "5"),
    ("BRIGHTNESS", "3"),
    ("FISHEYE", "4"),
    ("WHIRL", "7"),
    ("PIXELATE", "2"),
    ("MOSAIC", "9"),
    ("COLOR", "3"),
    ("BRIGHTNESS", "-3"),
)


def blocks(effect, step, has_ghost):
    result = {
        "hat": {
            "opcode": "event_whenflagclicked", "topLevel": True,
            "next": "setup" if has_ghost else "forever",
            "fields": {}, "inputs": {},
        },
        "forever": {
            "opcode": "control_forever", "parent": "setup" if has_ghost else "hat",
            "next": None, "fields": {}, "inputs": {"SUBSTACK": [2, "change"]},
        },
        "change": {
            "opcode": "looks_changeeffectby", "parent": "forever", "next": None,
            "fields": {"EFFECT": [effect, None]},
            "inputs": {"CHANGE": [1, [4, step]]},
        },
    }
    if has_ghost:
        result["setup"] = {
            "opcode": "looks_seteffectto", "parent": "hat", "next": "forever",
            "fields": {"EFFECT": ["GHOST", None]},
            "inputs": {"VALUE": [1, [4, "35"]]},
        }
    return result


def generate(source, destination, count, asset_name):
    with zipfile.ZipFile(source) as original:
        document = json.loads(original.read("project.json"))
        costume = next(
            c for t in document["targets"] for c in t["costumes"]
            if c.get("md5ext") == asset_name
        )
        asset_bytes = original.read(asset_name)

    stage = {
        "isStage": True, "name": "Stage", "variables": {}, "lists": {},
        "broadcasts": {}, "blocks": {}, "costumes": [], "sounds": [],
        "currentCostume": 0, "layerOrder": 0, "volume": 100,
    }
    targets = [stage]
    for index in range(count):
        effect, amount = EFFECTS[index % len(EFFECTS)]
        col, row = index % 4, (index // 4) % 2
        targets.append({
            "isStage": False, "name": f"effect-{index}-{effect}",
            "variables": {}, "lists": {}, "broadcasts": {},
            "blocks": blocks(effect, amount, has_ghost=index % 2 == 1),
            "costumes": [costume], "sounds": [],
            "currentCostume": 0, "layerOrder": index + 1, "volume": 100,
            "x": -174 + col * 116, "y": 80 - row * 160,
            "size": 11, "direction": 90, "rotationStyle": "all around",
            "visible": True, "draggable": False,
        })

    project = {
        "targets": targets, "monitors": [], "extensions": [],
        "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": "fx-stress"},
    }
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr("project.json", json.dumps(project))
        out.writestr(asset_name, asset_bytes)
    print(f"wrote {destination}: {count} active effects, source={asset_name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("destination")
    parser.add_argument("--count", type=int, default=8)
    parser.add_argument(
        "--costume", default="c94e9de64e46787c2f59be8a06676360.png",
        help="An existing PNG costume asset name in the input SB3",
    )
    args = parser.parse_args()
    generate(args.source, args.destination, args.count, args.costume)
