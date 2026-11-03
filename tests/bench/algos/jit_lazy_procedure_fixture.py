#!/usr/bin/env python3
"""Generate independent Scratch projects to check on-demand procedure materialization."""
import json
import sys
import zipfile
from pathlib import Path

def make_project(count: int, body_steps: int, gate: int) -> dict:
    blocks = {}

    def add(uid, opcode, next_id=None, inputs=None, fields=None, mutation=None, top=False, shadow=False):
        b = {
            "opcode": opcode, "next": next_id, "parent": None,
            "inputs": inputs or {}, "fields": fields or {},
            "shadow": shadow, "topLevel": top,
        }
        if mutation is not None:
            b["mutation"] = mutation
        if top:
            b["x"], b["y"] = 0, 0
        blocks[uid] = b

    def mutation(name, prototype=False):
        m = {"tagName": "mutation", "children": [],
             "proccode": name, "argumentids": "[]", "warp": "true"}
        if prototype:
            m.update({"argumentnames": "[]", "argumentdefaults": "[]"})
        return m

    def call(uid, name, nxt):
        add(uid, "procedures_call", nxt, mutation=mutation(name))

    def define(name, steps):
        definition = f"{name}_definition"
        prototype = f"{name}_prototype"
        first = f"{name}_step0"
        add(definition, "procedures_definition", first,
            {"custom_block": [1, prototype]}, top=True)
        add(prototype, "procedures_prototype", mutation=mutation(name, True),
            shadow=True)
        for i in range(steps):
            nxt = f"{name}_step{i+1}" if i + 1 < steps else None
            add(f"{name}_step{i}", "data_changevariableby", nxt,
                {"VALUE": [2, f"{name}_sum{i}"]},
                {"VARIABLE": ["value", "value"]})
            add(f"{name}_sum{i}", "operator_add",
                inputs={"NUM1": [2, f"{name}_mod{i}"],
                        "NUM2": [1, [4, "1"]]})
            add(f"{name}_mod{i}", "operator_mod",
                inputs={"NUM1": [2, f"{name}_read{i}"],
                        "NUM2": [1, [4, "7"]]})
            add(f"{name}_read{i}", "data_variable",
                fields={"VARIABLE": ["value", "value"]})

    add("hat", "event_whenflagclicked", "branch", top=True)
    add("branch", "control_if", "hot_call",
        {"CONDITION": [2, "gate_read"],
         "SUBSTACK": [2, "cold_call0"]})
    add("gate_read", "data_variable",
        fields={"VARIABLE": ["gate", "gate"]})
    for i in range(count):
        call(f"cold_call{i}", f"cold{i}",
             f"cold_call{i+1}" if i+1 < count else None)
        define(f"cold{i}", body_steps)
    define("hot", 1)
    call("hot_call", "hot", "output")
    add("output", "data_addtolist", None,
        {"ITEM": [2, "value_read"]}, {"LIST": ["output", "out"]})
    add("value_read", "data_variable",
        fields={"VARIABLE": ["value", "value"]})
    return {"targets": [{
        "isStage": True, "name": "Stage",
        "variables": {"gate": ["gate", gate], "value": ["value", 0]},
        "lists": {"out": ["output", []]},
        "blocks": blocks, "costumes": [], "sounds": []
    }], "monitors": [], "extensions": [], "meta": {}}

if __name__ == "__main__":
    count, steps = map(int, sys.argv[1:3])
    for gate in (0, 1):
        path = Path(f"build/jit_lazy_{count}procs_gate{gate}.sb3")
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr("project.json", json.dumps(make_project(count, steps, gate)))
        print(path)
