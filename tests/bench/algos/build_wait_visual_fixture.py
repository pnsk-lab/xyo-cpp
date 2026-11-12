#!/usr/bin/env python3
"""Augment an existing rendered Scratch SB3 with a deterministic wait-until fanout.

Keeps the original Scratch art, scripts and assets unchanged, and adds one
visible overlay sprite and a Stage variable gate controlled by a broadcast.
The extra 24 independent wait scripts all complete after a native JIT write.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import zipfile

from PIL import Image


def block(opcode: str, parent: str | None, next_id: str | None,
          inputs: dict | None = None, fields: dict | None = None,
          top_level: bool = False, xy: tuple[int, int] = (0, 0)) -> dict:
    result = {
        "opcode": opcode, "next": next_id, "parent": parent,
        "inputs": inputs or {}, "fields": fields or {},
        "shadow": False, "topLevel": top_level,
    }
    if top_level:
        result["x"], result["y"] = xy
    return result


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--waiters", type=int, default=24)
    args = ap.parse_args()
    if not (1 <= args.waiters <= 256):
        ap.error("--waiters must be between 1 and 256")
    with zipfile.ZipFile(args.source) as archive:
        payloads = {entry.filename: archive.read(entry.filename)
                    for entry in archive.infolist()}
    project = json.loads(payloads["project.json"])
    stage = next(target for target in project["targets"] if target["isStage"])
    original = next(target for target in project["targets"] if not target["isStage"])

    gate_id = "xyo_wait_ready_gate"
    done_id = "xyo_wait_ready_done"
    broadcast_id = "xyo_wait_ready_unlock"
    stage["variables"][gate_id] = ["wait_ready_gate", 0]
    stage["variables"][done_id] = ["wait_ready_done", 0]
    stage.setdefault("broadcasts", {})[broadcast_id] = "UNLOCK"

    png = io.BytesIO()
    Image.new("RGBA", (96, 96), (245, 40, 64, 255)).save(png, "PNG")
    png_bytes = png.getvalue()
    asset_id = hashlib.md5(png_bytes).hexdigest()
    asset_file = asset_id + ".png"

    overlay = copy.deepcopy(original)
    overlay.update({
        "name": "WakeOverlay", "variables": {}, "lists": {},
        "broadcasts": {}, "blocks": {}, "comments": {},
        "costumes": [{
            "name": "overlay", "assetId": asset_id,
            "md5ext": asset_file, "dataFormat": "png",
            "rotationCenterX": 48, "rotationCenterY": 48
        }],
        "sounds": [], "visible": True, "x": 0, "y": 0,
        "size": 100, "direction": 90, "draggable": False,
        "layerOrder": max(t.get("layerOrder", 0) for t in project["targets"]) + 1,
        "currentCostume": 0,
    })
    blocks = overlay["blocks"]
    for i in range(args.waiters):
        p = f"xyo_{i}_"
        hat_id, wait_id = p + "hat", p + "wait"
        gt_id, var_id = p + "gt", p + "value"
        done_id_block, hide_id = p + "done", p + "hide"
        blocks[hat_id] = block("event_whenflagclicked", None, wait_id,
                               top_level=True, xy=(40 + (i % 6) * 250,
                                                   40 + (i // 6) * 220))
        blocks[wait_id] = block(
            "control_wait_until", hat_id, done_id_block,
            inputs={"CONDITION": [2, gt_id]})
        blocks[gt_id] = block(
            "operator_gt", wait_id, None,
            inputs={"OPERAND1": [2, var_id],
                    "OPERAND2": [1, [4, "0"]]})
        blocks[var_id] = block(
            "data_variable", gt_id, None,
            fields={"VARIABLE": ["wait_ready_gate", gate_id]})
        blocks[done_id_block] = block(
            "data_changevariableby", wait_id, hide_id if i == 0 else None,
            inputs={"VALUE": [1, [4, "1"]]},
            fields={"VARIABLE": ["wait_ready_done", done_id]})
        if i == 0:
            blocks[hide_id] = block("looks_hide", done_id_block, None)
    blocks["xyo_unlock_hat"] = block(
        "event_whenbroadcastreceived", None, "xyo_unlock_set",
        fields={"BROADCAST_OPTION": ["UNLOCK", broadcast_id]},
        top_level=True, xy=(0, 1100))
    blocks["xyo_unlock_set"] = block(
        "data_setvariableto", "xyo_unlock_hat", None,
        inputs={"VALUE": [1, [4, "1"]]},
        fields={"VARIABLE": ["wait_ready_gate", gate_id]})

    project["targets"].append(overlay)
    payloads["project.json"] = json.dumps(
        project, separators=(",", ":"), ensure_ascii=False).encode()
    payloads[asset_file] = png_bytes
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in payloads.items():
            archive.writestr(name, data)
    print(json.dumps({
        "source": str(args.source), "out": str(args.out),
        "waiters": args.waiters, "target_count": len(project["targets"]),
        "binary_sha256": hashlib.sha256(args.out.read_bytes()).hexdigest(),
        "gate_id": gate_id, "done_id": done_id,
    }, indent=2))


if __name__ == "__main__":
    main()
