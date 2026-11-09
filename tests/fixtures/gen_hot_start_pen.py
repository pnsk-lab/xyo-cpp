#!/usr/bin/env python3
"""Pen bitmap and sprite location fixture for xyo hot-start rendering."""
import json
import sys
import zipfile

def b(op, nxt=None, parent=None, inputs=None, fields=None):
    return {
        "opcode": op, "parent": parent, "next": nxt,
        "inputs": inputs or {}, "fields": fields or {},
        "topLevel": parent is None, "shadow": False,
    }

blocks = {
    "hat": b("event_whenflagclicked", nxt="setx"),
    "setx": b("motion_setx", parent="hat", nxt="sety",
              inputs={"X": [1, [4, "-160"]]}),
    "sety": b("motion_sety", parent="setx", nxt="down",
              inputs={"Y": [1, [4, "-100"]]}),
    "down": b("pen_penDown", parent="sety", nxt="forever"),
    "forever": b("control_forever", parent="down",
                 inputs={"SUBSTACK": [2, "moveX"]}),
    "moveX": b("motion_changexby", parent="forever", nxt="moveY",
               inputs={"DX": [1, [4, "13"]]}),
    "moveY": b("motion_changeyby", parent="moveX", nxt="wait",
               inputs={"DY": [1, [4, "5"]]}),
    "wait": b("control_wait", parent="moveY",
              inputs={"DURATION": [1, [5, "0"]]}),
}
stage = {
    "isStage": True, "name": "Stage", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": {}, "costumes": [], "sounds": [],
    "currentCostume": 0, "layerOrder": 0,
}
sprite = {
    "isStage": False, "name": "Pen", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": blocks, "costumes": [], "sounds": [],
    "currentCostume": 0, "layerOrder": 1, "x": 0, "y": 0, "size": 100,
    "direction": 90, "rotationStyle": "all around",
    "visible": True, "volume": 100,
}
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as archive:
    archive.writestr("project.json", json.dumps({
        "targets": [stage, sprite], "monitors": [], "extensions": [],
        "meta": {"semver": "3.0.0"},
    }))
