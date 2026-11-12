#!/usr/bin/env python3
"""FP64 NVIDIA Pen replay safety on a partially transparent Scratch stroke."""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

from dlss_pen_primitive_smoke import make_case
from dlss_pen_gpu_precision_smoke import error, read_input, run


def make_transparent_project(path: Path) -> None:
    make_case(path, "replay")
    with ZipFile(path) as z:
        content = {name: z.read(name) for name in z.namelist()}
    project = json.loads(content["project.json"])
    sprite = next(t for t in project["targets"] if not t["isStage"])
    blocks = sprite["blocks"]
    blocks["clear"]["next"] = "alpha"
    blocks["alpha"] = {
        "opcode": "pen_setPenColorParamTo",
        "next": "size", "parent": "clear",
        "fields": {},
        "inputs": {"COLOR_PARAM": [1, "menu"], "VALUE": [1, [4, "50"]]},
        "topLevel": False, "shadow": False,
    }
    blocks["menu"] = {
        "opcode": "pen_menu_colorParam",
        "next": None, "parent": "alpha",
        "fields": {"colorParam": ["transparency", None]},
        "inputs": {},
        "topLevel": False, "shadow": True,
    }
    blocks["size"]["parent"] = "alpha"
    content["project.json"] = json.dumps(project).encode()
    with ZipFile(path, "w", compression=ZIP_DEFLATED) as archive:
        for name, data in content.items():
            archive.writestr(name, data)


def main() -> int:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_pen_gpu_transparency_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-pen-alpha-fp64-") as directory:
        root = Path(directory)
        sb3 = root / "translucent-pen.sb3"
        make_transparent_project(sb3)
        cpu, _ = run(sys.argv[1], sb3, root, "cpu", False, False)
        normal, log1 = run(sys.argv[1], sb3, root, "gpu", True, False)
        precise, log2 = run(sys.argv[1], sb3, root, "precise", True, True)
        if "GPU Pen coverage shader: regular interpolated" not in log1:
            raise RuntimeError("Normal GPU program did not execute")
        if "GPU Pen coverage shader: FP64 per-pixel" not in log2:
            print("SKIP: GPU FP64 unsupported on this GL renderer")
            return 77
        totals = {}
        for mode in ("gpu", "precise"):
            total = 0
            for frame in range(1, 33):
                reference = read_input(root, "cpu", frame)
                candidate = read_input(root, mode, frame)
                _, absolute_error = error(reference, candidate)
                total += absolute_error
            totals[mode] = total
        base_final = error(cpu, normal)[1]
        precise_final = error(cpu, precise)[1]
        print(f"50% transparent Pen, 32 frames: input sumabs "
              f"GPU={totals['gpu']}, FP64={totals['precise']}; "
              f"final sumabs GPU={base_final}, FP64={precise_final}")
        if totals["precise"] > totals["gpu"] or precise_final > base_final:
            raise RuntimeError("FP64 regressed translucent Pen fidelity")
    print("PASS: FP64 GPU Pen preserves alpha and improves cumulative parity")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print("FAIL:", error, file=sys.stderr)
        sys.exit(1)
