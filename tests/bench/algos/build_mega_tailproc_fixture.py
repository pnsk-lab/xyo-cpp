#!/usr/bin/env python3
"""Wrap each supported green-flag body in a single argument-free tail procedure.

The resulting Scratch project tests a real procedures_call / definition /
prototype boundary, while keeping the original repeat/broadcast body intact.
"""
import argparse
import json
from pathlib import Path
import zipfile


def transform(project):
    for target in project["targets"]:
        blocks = target["blocks"]
        hats = [(key, value) for key, value in blocks.items()
                if isinstance(value, dict) and
                value.get("opcode") == "event_whenflagclicked" and
                value.get("next")]
        for index, (hat_id, hat) in enumerate(hats):
            original_next = hat["next"]
            procedure = f"mega_tail_proc_{index}"
            call_id = f"mega_call_{index}"
            definition_id = f"mega_definition_{index}"
            proto_id = f"mega_prototype_{index}"
            for key in (call_id, definition_id, proto_id):
                if key in blocks:
                    raise ValueError(f"generated id conflict: {key}")
            mutation = {
                "tagName": "mutation", "children": [], "proccode": procedure,
                "argumentids": "[]", "argumentnames": "[]",
                "argumentdefaults": "[]", "warp": "false",
            }
            blocks[call_id] = {
                "opcode": "procedures_call", "parent": hat_id, "next": None,
                "topLevel": False, "shadow": False, "inputs": {},
                "fields": {}, "mutation": mutation,
            }
            blocks[definition_id] = {
                "opcode": "procedures_definition", "parent": None,
                "next": original_next, "topLevel": True, "shadow": False,
                "inputs": {"custom_block": [1, proto_id]}, "fields": {},
                "x": 240, "y": index*240,
            }
            blocks[proto_id] = {
                "opcode": "procedures_prototype", "parent": definition_id,
                "next": None, "topLevel": False, "shadow": True,
                "inputs": {}, "fields": {}, "mutation": mutation,
            }
            blocks[original_next]["parent"] = definition_id
            hat["next"] = call_id
    return project


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    with zipfile.ZipFile(args.source) as archive:
        project = json.loads(archive.read("project.json"))
        extra = [(item.filename, archive.read(item.filename))
                 for item in archive.infolist()
                 if item.filename != "project.json"]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(transform(project)))
        for name, blob in extra:
            archive.writestr(name, blob)
    print(args.out)


if __name__ == "__main__":
    main()
