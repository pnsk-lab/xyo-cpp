#!/usr/bin/env python3
"""Clone lifetime and identity fixture for portable hot-start."""
import json
import sys
import zipfile

def b(op, parent=None, nxt=None, inputs=None, fields=None, top=False):
    return {"opcode": op, "parent": parent, "next": nxt,
            "inputs": inputs or {}, "fields": fields or {},
            "topLevel": top, "shadow": False}

stage = {
    "isStage": True, "name": "Stage", "variables": {},
    "lists": {"events": ["events", []]}, "broadcasts": {}, "blocks": {},
    "costumes": [], "sounds": [], "currentCostume": 0, "layerOrder": 0,
}
blocks = {
    "flag": b("event_whenflagclicked", nxt="repeat", top=True),
    "repeat": b("control_repeat", parent="flag", nxt="pause-original",
                inputs={"TIMES": [1, [6, "3"]], "SUBSTACK": [2, "create"]}),
    "create": b("control_create_clone_of", parent="repeat",
                inputs={"CLONE_OPTION": [1, "clone-menu"]}),
    "clone-menu": b("control_create_clone_of_menu", parent="create",
                    fields={"CLONE_OPTION": ["_myself_", None]}),
    "pause-original": b("control_wait", parent="repeat", nxt="last-clone",
                        inputs={"DURATION": [1, [5, "0.23"]]}),
    "last-clone": b("control_create_clone_of", parent="pause-original",
                    inputs={"CLONE_OPTION": [1, "clone-menu-2"]}),
    "clone-menu-2": b("control_create_clone_of_menu", parent="last-clone",
                      fields={"CLONE_OPTION": ["_myself_", None]}),

    "clone-flag": b("control_start_as_clone", nxt="spawned", top=True),
    "spawned": b("data_addtolist", parent="clone-flag", nxt="pause-clone",
                 fields={"LIST": ["events", "events"]},
                 inputs={"ITEM": [1, [10, "spawned"]]}),
    "pause-clone": b("control_wait", parent="spawned", nxt="died",
                     inputs={"DURATION": [1, [5, "0.10"]]}),
    "died": b("data_addtolist", parent="pause-clone", nxt="delete-clone",
              fields={"LIST": ["events", "events"]},
              inputs={"ITEM": [1, [10, "died"]]}),
    "delete-clone": b("control_delete_this_clone", parent="died"),
}
sprite = {
    "isStage": False, "name": "Cloner", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": blocks, "costumes": [], "sounds": [],
    "currentCostume": 0, "layerOrder": 1, "x": 0, "y": 0,
    "size": 100, "direction": 90, "rotationStyle": "all around",
    "visible": True, "volume": 100,
}
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("project.json", json.dumps({
        "targets": [stage, sprite], "monitors": [], "extensions": [],
        "meta": {"semver": "3.0.0"},
    }))
