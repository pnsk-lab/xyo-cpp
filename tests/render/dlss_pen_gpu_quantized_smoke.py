#!/usr/bin/env python3
"""Compare CPU and 8-bit-quantized GPU Pen over 32 NGX temporal frames."""
from __future__ import annotations
from pathlib import Path
import json
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

from dlss_pen_primitive_smoke import make_case
from dlss_pen_gpu_precision_smoke import error, read_input, run
from dlss_pen_gpu_transparency_smoke import make_transparent_project


def check(binary: str, root: Path, name: str,
          transparency: int | None, diameter: int,
          compare_scissor: bool = False) -> bool:
    scene = root / (name + ".sb3")
    if transparency is None:
        make_case(scene, "replay")
    else:
        make_transparent_project(scene)
    # Use actual Scratch blocks rather than calling the renderer directly:
    # cover Scratch's odd-diameter origin correction and varying opacity.
    with ZipFile(scene) as archive:
        content = {filename: archive.read(filename)
                   for filename in archive.namelist()}
    project = json.loads(content["project.json"])
    sprite = next(t for t in project["targets"] if not t["isStage"])
    blocks = sprite["blocks"]
    blocks["size"]["inputs"]["SIZE"] = [1, [4, str(diameter)]]
    if transparency is not None:
        blocks["alpha"]["inputs"]["VALUE"] = [1, [4, str(transparency)]]
    content["project.json"] = json.dumps(project).encode()
    with ZipFile(scene, "w", compression=ZIP_DEFLATED) as archive:
        for filename, data in content.items():
            archive.writestr(filename, data)
    cpu, _ = run(binary, scene, root, name + "-cpu", False, False)
    baseline, log = run(binary, scene, root, name + "-fp64", True, True)
    if "GPU Pen coverage shader: FP64 per-pixel" not in log:
        return False
    quantized, log = run(binary, scene, root, name + "-quantized",
                         True, True, quantized=True)
    if "GPU Pen coverage shader: FP64 CPU-quantized" not in log:
        return False
    tiled, tiled_log = run(binary, scene, root, name + "-tiled",
                           True, True, quantized=True, tiled=True)
    if "GPU Pen coverage shader: FP64 CPU-quantized" not in tiled_log:
        raise RuntimeError(name + ": tiled shader unavailable")
    old_delta = new_delta = tiled_delta = 0
    for frame in range(1, 33):
        ref = read_input(root, name + "-cpu", frame)
        old_delta += error(ref, read_input(root, name + "-fp64", frame))[1]
        new_delta += error(ref, read_input(root, name + "-quantized", frame))[1]
        tiled_delta += error(ref, read_input(root, name + "-tiled", frame))[1]
    old_final = error(cpu, baseline)[1]
    new_final = error(cpu, quantized)[1]
    tiled_final = error(cpu, tiled)[1]
    print(f"{name}: input absRGB FP64={old_delta}, quantized={new_delta}, "
          f"tiled={tiled_delta}; final FP64={old_final}, quantized={new_final}, "
          f"tiled={tiled_final}", flush=True)
    if old_delta <= 0:
        raise RuntimeError(name + ": baseline did not expose rounding differences")
    if new_delta != 0 or new_final != 0 or tiled_delta != 0 or tiled_final != 0:
        raise RuntimeError(name + ": CPU/GPU tiled exact parity regression")
    if compare_scissor:
        full, log = run(binary, scene, root, name + "-no-scissor",
                        True, True, quantized=True, scissor=False)
        if "GPU Pen coverage shader: FP64 CPU-quantized" not in log:
            raise RuntimeError(name + ": no-scissor GPU shader fallback")
        for frame in range(1, 33):
            cropped = read_input(root, name + "-quantized", frame)
            uncropped = read_input(root, name + "-no-scissor", frame)
            if cropped != uncropped:
                raise RuntimeError(name + ": scissor changed input pixels")
        if full != quantized:
            raise RuntimeError(name + ": scissor changed NGX output")
        print(name + ": scissor and fullscreen outputs match", flush=True)
    return True


def main() -> int:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_pen_gpu_quantized_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-pen-quantized-") as directory:
        root = Path(directory)
        scenarios = (
            ("opaque3", None, 3, False),
            ("alpha50_size3", 50, 3, True),
            ("alpha25_size1", 25, 1, False),
            ("alpha75_size5", 75, 5, False),
        )
        for name, transparency, diameter, compare_scissor in scenarios:
            if not check(sys.argv[1], root, name,
                         transparency, diameter, compare_scissor):
                print("SKIP: no GL4.3 CPU-quantized Pen shader")
                return 77
    print("PASS: 4 scenes x 32 frames: tiled/untiled exactly match CPU Pen replay")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print("FAIL:", exc, file=sys.stderr)
        sys.exit(1)
