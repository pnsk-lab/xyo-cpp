#!/usr/bin/env python3
"""Deterministic visuals for GPU vs CPU penStamp comparison."""
import json
import sys
import zipfile


def make_script(prefix):
    return {
        prefix + "a": {
            "opcode": "event_whenflagclicked", "topLevel": True,
            "next": prefix + "b", "inputs": {}, "fields": {},
        },
        prefix + "b": {
            "opcode": "pen_stamp", "next": prefix + "c",
            "parent": prefix + "a", "inputs": {}, "fields": {},
        },
        prefix + "c": {
            "opcode": "looks_hide", "next": None,
            "parent": prefix + "b", "inputs": {}, "fields": {},
        },
    }


def generate(destination):
    stage = {
        "isStage": True, "name": "Stage",
        "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
        "costumes": [], "sounds": [], "currentCostume": 0, "layerOrder": 0,
    }
    sprites = []
    for idx, (x, y, rotation, style, ghost) in enumerate((
        (-120, 60, 90, "all around", 0),
        (70, 60, 25, "all around", 0),
        (-120, -70, -40, "all around", 35),
        (70, -70, -90, "left-right", 50),
    )):
        script = make_script(f"{idx}-")
        if ghost:
            script[f"{idx}-a"]["next"] = f"{idx}-effect"
            script[f"{idx}-effect"] = {
                "opcode": "looks_seteffectto", "parent": f"{idx}-a",
                "next": f"{idx}-b", "fields": {"EFFECT": ["GHOST", None]},
                "inputs": {"VALUE": [1, [4, str(ghost)]]},
            }
        sprites.append({
            "isStage": False, "name": f"stamp-{idx}",
            "variables": {}, "lists": {}, "broadcasts": {}, "blocks": script,
            "costumes": [{
                "name": "gradient", "assetId": "stamp",
                "md5ext": "stamp.svg", "dataFormat": "svg",
                "rotationCenterX": 48, "rotationCenterY": 48,
                "bitmapResolution": 1,
            }],
            "sounds": [], "currentCostume": 0, "layerOrder": idx + 1,
            "x": x, "y": y, "size": 150, "direction": rotation,
            "rotationStyle": style, "visible": True, "volume": 100,
        })
    picture = """<svg xmlns="http://www.w3.org/2000/svg" width="96"
height="96" viewBox="0 0 96 96">
<rect width="96" height="48" fill="#e63946" />
<rect x="0" y="48" width="96" height="48" fill="#457b9d" />
<circle cx="18" cy="22" r="11" fill="#f1faee" />
<rect x="48" y="15" width="25" height="25" fill="#a8dadc" opacity=".65" />
</svg>"""
    j = {
        "targets": [stage, *sprites], "monitors": [], "extensions": ["pen"],
        "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": "stamp-check"},
    }
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(j))
        z.writestr("stamp.svg", picture)
    print(destination)


if __name__ == "__main__":
    generate(sys.argv[1])
