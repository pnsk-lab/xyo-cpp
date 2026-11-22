#!/usr/bin/env python3
"""Generate nested non-recursive custom-block Scratch project for mega-goto."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

def node(opcode, nxt=None, parent=None, inputs=None, fields=None, mutation=None, hat=False):
    v = {"opcode": opcode, "next": nxt, "parent": parent,
         "inputs": inputs or {}, "fields": fields or {}, "shadow": False,
         "topLevel": hat}
    if mutation is not None:
        v["mutation"] = mutation
    if hat:
        v.update(x=0, y=0)
    return v

def mutation(name, warp):
    return {"tagName": "mutation", "children": [], "proccode": name,
            "argumentids": "[]", "argumentnames": "[]",
            "argumentdefaults": "[]", "warp": "true" if warp else "false"}

def generate(repetitions, warp, body_size=1, recursive=False):
    if body_size < 1 or body_size > 400:
        raise ValueError("body_size must be between 1 and 400")
    p = mutation("p", warp)
    q = mutation("q", warp)
    blocks = {
        "hat": node("event_whenflagclicked", "repeat", hat=True),
        "repeat": node("control_repeat", inputs={
            "TIMES": [1, [4, str(repetitions)]],
            "SUBSTACK": [2, "callP"],
        }, parent="hat"),
        "callP": node("procedures_call", parent="repeat", mutation=p),
        "defP": node("procedures_definition", "callQ1", hat=True,
                     inputs={"custom_block": [1, "protoP"]}),
        "protoP": node("procedures_prototype", parent="defP", mutation=p),
        "callQ1": node("procedures_call", "callQ2", "defP", mutation=q),
        "callQ2": node("procedures_call", parent="callQ1", mutation=q),
        "defQ": node("procedures_definition", "inc0", hat=True,
                     inputs={"custom_block": [1, "protoQ"]}),
        "protoQ": node("procedures_prototype", parent="defQ", mutation=q),
    }
    for index in range(body_size):
        current = f"inc{index}"
        nxt = f"inc{index + 1}" if index + 1 < body_size else (
            "recurseGuard" if recursive else None)
        blocks[current] = node(
            "data_changevariableby", nxt,
            "defQ" if index == 0 else f"inc{index - 1}",
            inputs={"VALUE": [1, [4, "1"]]},
            fields={"VARIABLE": ["s", "s"]})
    if recursive:
        # A finite recursive procedure graph: P calls Q twice, Q conditionally
        # calls P again until Stage s reaches 3. The mega pass must skip it.
        blocks["recurseGuard"] = node(
            "control_if", parent=f"inc{body_size - 1}",
            inputs={"CONDITION": [2, "recurseLt"],
                    "SUBSTACK": [2, "callPRecursive"]})
        blocks["recurseLt"] = node(
            "operator_lt", parent="recurseGuard",
            inputs={"OPERAND1": [2, "recurseVar"],
                    "OPERAND2": [1, [4, "3"]]})
        blocks["recurseVar"] = node(
            "data_variable", parent="recurseLt",
            fields={"VARIABLE": ["s", "s"]})
        blocks["callPRecursive"] = node(
            "procedures_call", parent="recurseGuard", mutation=p)
    return {
        "targets": [{"isStage": True, "name": "Stage",
                     "variables": {"s": ["s", 0]}, "lists": {},
                     "broadcasts": {}, "blocks": blocks, "costumes": [],
                     "sounds": [], "currentCostume": 0, "layerOrder": 0,
                     "volume": 100}],
        "monitors": [], "extensions": [],
        "meta": {"semver": "3.0.0"}
    }

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--count", type=int, default=20000)
    ap.add_argument("--warp", action="store_true")
    ap.add_argument("--body-size", type=int, default=1)
    ap.add_argument("--recursive", action="store_true")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(
            generate(args.count, args.warp, args.body_size, args.recursive)))
    print("project", args.out, "sha256", hashlib.sha256(args.out.read_bytes()).hexdigest(),
          "expected", 2 * args.count * args.body_size if not args.recursive else "dynamic")
