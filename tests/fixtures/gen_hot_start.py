#!/usr/bin/env python3
"""A deterministic Scratch 3 project for frame-boundary hot-start."""
import json
import sys
import zipfile

def node(op, parent=None, next_=None, inputs=None, fields=None, top=False):
    return {"opcode": op, "next": next_, "parent": parent,
            "inputs": inputs or {}, "fields": fields or {},
            "shadow": False, "topLevel": top}

stage = {
    "isStage": True, "name": "Stage", "variables": {"counter": ["counter", 0],
                             "message": ["message", "こんにちはホットスタート"]},
    "lists": {"history": ["history", []]}, "broadcasts": {},
    "blocks": {}, "costumes": [], "sounds": [],
    "currentCostume": 0, "layerOrder": 0,
}
blocks = {
    "hat": node("event_whenflagclicked", next_="forever", top=True),
    "forever": node("control_forever", parent="hat", inputs={"SUBSTACK": [2, "change"]}),
    "change": node("data_changevariableby", parent="forever", next_="append",
                   fields={"VARIABLE": ["counter", "counter"]},
                   inputs={"VALUE": [1, [4, "1"]]}),
     "append": node("data_addtolist", parent="change", next_="random-append",
                   fields={"LIST": ["history", "history"]},
                   inputs={"ITEM": [2, "read"]}),
    "read": node("data_variable", parent="append",
                 fields={"VARIABLE": ["counter", "counter"]}),
    "random-append": node("data_addtolist", parent="append", next_="wait",
                   fields={"LIST": ["history", "history"]},
                   inputs={"ITEM": [2, "random"]}),
    "random": node("operator_random", parent="random-append",
                   inputs={"FROM": [1, [4, "1"]], "TO": [1, [4, "100"]]}),
    "wait": node("control_wait", parent="random-append",
                 inputs={"DURATION": [1, [5, "0"]]}),
}
sprite = {
    "isStage": False, "name": "Worker", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": blocks, "costumes": [], "sounds": [],
    "currentCostume": 0, "layerOrder": 1,
    "x": 0, "y": 0, "size": 100, "direction": 90,
    "rotationStyle": "all around", "visible": True, "volume": 100
}
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("project.json", json.dumps({"targets": [stage, sprite],
        "monitors": [], "extensions": [],
        "meta": {"semver": "3.0.0"}}, ensure_ascii=False))
