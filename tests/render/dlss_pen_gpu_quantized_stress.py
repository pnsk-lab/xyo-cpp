#!/usr/bin/env python3
"""Validate GL4.3 quantized Pen limits and CPU analytic fallback on real NGX."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

from dlss_pen_primitive_smoke import make_case
from dlss_pen_gpu_precision_smoke import error, read_input, run


def make_strokes(path: Path, moves: int, wide: bool) -> None:
    make_case(path, "replay")
    with ZipFile(path) as z:
        content = {name: z.read(name) for name in z.namelist()}
    project = json.loads(content["project.json"])
    sprite = next(target for target in project["targets"] if not target["isStage"])
    blocks = sprite["blocks"]
    if wide:
        sprite["x"], sprite["y"] = -220, -150
    blocks["down"]["next"] = "multi_0"
    for i in range(moves):
        if wide:
            axis = "DX" if i % 2 == 0 else "DY"
            opcode = ("motion_changexby" if axis == "DX"
                      else "motion_changeyby")
            amount = 440 if i % 4 == 0 else (
                300 if i % 4 == 1 else -440 if i % 4 == 2 else -300
            )
        else:
            axis, opcode, amount = "DX", "motion_changexby", 2
        blocks[f"multi_{i}"] = {
            "opcode": opcode,
            "next": f"multi_{i + 1}" if i + 1 < moves else None,
            "parent": "down" if i == 0 else f"multi_{i - 1}",
            "fields": {}, "inputs": {axis: [1, [4, str(amount)]]},
            "topLevel": False, "shadow": False,
        }
    del blocks["loop"]
    del blocks["move"]
    content["project.json"] = json.dumps(project).encode()
    with ZipFile(path, "w", compression=ZIP_DEFLATED) as z:
        for name, data in content.items():
            z.writestr(name, data)


def make_sparse_strokes(path: Path, dots: int = 30) -> None:
    """Use pen-up jumps to spread short, disjoint strokes over the canvas."""
    make_case(path, "replay")
    with ZipFile(path) as z:
        content = {name: z.read(name) for name in z.namelist()}
    project = json.loads(content["project.json"])
    sprite = next(target for target in project["targets"]
                  if not target["isStage"])
    blocks = sprite["blocks"]
    blocks["down"]["next"] = "up_0"
    prev = "down"
    for i in range(dots):
        x = -210 + (i % 10) * 46
        y = -145 + (i // 10) * 145
        for name, opcode, inputs in (
            (f"up_{i}", "pen_penUp", {}),
            (f"jump_{i}", "motion_gotoxy", {
                "X": [1, [4, str(x)]], "Y": [1, [4, str(y)]],
            }),
            (f"down_{i}", "pen_penDown", {}),
            (f"draw_{i}", "motion_changexby", {
                "DX": [1, [4, "2"]],
            }),
        ):
            blocks[name] = {
                "opcode": opcode, "next": None, "parent": prev,
                "fields": {}, "inputs": inputs,
                "topLevel": False, "shadow": False,
            }
            blocks[prev]["next"] = name
            prev = name
    del blocks["loop"]
    del blocks["move"]
    content["project.json"] = json.dumps(project).encode()
    with ZipFile(path, "w", compression=ZIP_DEFLATED) as z:
        for name, data in content.items():
            z.writestr(name, data)


def check(binary: str, root: Path, name: str, moves: int, wide: bool,
          gpu_expected: bool, tiled: bool = False,
          sparse: bool = False) -> None:
    path = root / (name + ".sb3")
    if sparse:
        make_sparse_strokes(path)
    else:
        make_strokes(path, moves, wide)
    cpu, _ = run(binary, path, root, name + "-cpu", False, False)
    gpu, log = run(binary, path, root, name + "-gpu",
                   True, True, quantized=True, tiled=tiled)
    # The fixture starts with one Pen segment before the generated moves.
    strokes = (1 + 2 * 30) if sparse else moves + 1
    gpu_active = "GPU Pen coverage shader: FP64 CPU-quantized" in log
    if tiled and gpu_expected and "GPU Pen quantized tiled16 active" not in log:
        raise RuntimeError(f"{name}: did not select tiled16 shader")
    cpu_fallback = "pen primitive rerasterization active" in log and (
        "GPU pen primitive rerasterization active" not in log
    )
    if gpu_expected and not gpu_active:
        raise RuntimeError(f"{name}: quantized GPU not used for {strokes} strokes")
    if not gpu_expected and not cpu_fallback:
        raise RuntimeError(f"{name}: expected CPU fallback for {strokes} strokes")
    if wide and "GPU Pen quantized work budget exceeded" not in log:
        raise RuntimeError(f"{name}: did not exercise work budget")
    input_sum = 0
    for f in range(1, 33):
        input_sum += error(read_input(root, name + "-cpu", f),
                           read_input(root, name + "-gpu", f))[1]
    final_sum = error(cpu, gpu)[1]
    print(f"{name}: strokes={strokes}, quantized_gpu={gpu_expected}, "
          f"tiled={tiled}, pre-NGX abs RGB={input_sum}, final abs RGB={final_sum}",
          flush=True)
    if input_sum != 0 or final_sum != 0:
        raise RuntimeError(f"{name}: CPU-quantized parity regression")


def main() -> int:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_pen_gpu_quantized_stress.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-pen-quantized-stress-") as work:
        root = Path(work)
        for name, moves, wide, gpu_expected, tiled, sparse in (
            ("63-moves-gpu64", 63, False, True, False, False),
            ("63-moves-tiled64", 63, False, True, True, False),
            ("sparse-untiled61", 0, False, False, False, True),
            ("sparse-tiled61", 0, False, True, True, True),
            ("64-moves-cpu65", 64, False, False, False, False),
            ("wide-cpu-budget", 30, True, False, False, False),
        ):
            check(sys.argv[1], root, name, moves, wide, gpu_expected,
                  tiled, sparse)
    print("PASS: 64-stroke tiled GPU, 65-stroke CPU fallback, 4M-work CPU fallback")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print("FAIL:", error, file=sys.stderr)
        sys.exit(1)
