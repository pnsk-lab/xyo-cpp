#!/usr/bin/env python3
"""Real NGX GPU integration for Scratch rotation and scaling motion vectors."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

from dlss_rigid_project_smoke import build_project

MATRIX = re.compile(
    r"affine motion override active: (\d+) opaque quads; "
    r"current->previous rows \[\(([-\d.]+), ([-\d.]+), ([-\d.]+)\), "
    r"\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)\]"
)

def generate(path: Path, kind: str) -> None:
    build_project(path)
    with ZipFile(path) as archive:
        contents = {name: archive.read(name) for name in archive.namelist()}
    project = json.loads(contents["project.json"])
    sprite = next(t for t in project["targets"] if not t["isStage"])
    moving = sprite["blocks"]["move"]
    if kind == "rotation":
        moving["opcode"] = "motion_turnright"
        moving["inputs"] = {"DEGREES": [1, [4, "15"]]}
    elif kind == "scale":
        moving["opcode"] = "looks_changesizeby"
        moving["inputs"] = {"CHANGE": [1, [4, "2"]]}
    else:
        raise ValueError(kind)
    contents["project.json"] = json.dumps(project).encode()
    with ZipFile(path, "w", compression=ZIP_DEFLATED) as archive:
        for name, data in contents.items():
            archive.writestr(name, data)

def check(binary: str, project: Path, kind: str, enabled: bool) -> None:
    env = os.environ.copy()
    env.update({
        "XYO_EXIT_FRAMES": "20",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_AFFINE_MOTION": "1" if enabled else "0",
        "XYO_DLSS_RIGID_MOTION": "0",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_PEN_FSR": "0",
    })
    result = subprocess.run(
        [binary, "play", str(project), "--scale", "2"],
        capture_output=True, text=True, env=env, timeout=35, check=False,
    )
    output = result.stdout + result.stderr
    match = MATRIX.search(output)
    ngx = "NVIDIA NGX initialized" in output
    if result.returncode or not ngx or bool(match) != enabled:
        raise RuntimeError(
            f"{kind} enabled={enabled}, rc={result.returncode}, "
            f"ngx={ngx}, affine={bool(match)}\n{output}"
        )
    if enabled:
        count = int(match.group(1))
        a, b, _, c, d, _ = map(float, match.groups()[1:])
        if count != 1:
            raise RuntimeError(f"{kind}: expected one affine sprite; got {count}")
        if kind == "rotation":
            if abs(b) < 0.05 or abs(c) < 0.05:
                raise RuntimeError(f"{kind}: missing rotation, matrix={match.groups()}")
        if kind == "scale":
            if abs(a-1) < 0.005 or abs(d-1) < 0.005:
                raise RuntimeError(f"{kind}: missing scale, matrix={match.groups()}")
        print(f"{kind} affine: first M=({a:.4f},{b:.4f};{c:.4f},{d:.4f})")
    else:
        print(f"{kind} affine disabled: no override, native NGX initialized")

def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("expected xyo-cli binary")
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-affine-") as directory:
        for kind in ("rotation", "scale"):
            project = Path(directory) / f"{kind}.sb3"
            generate(project, kind)
            check(sys.argv[1], project, kind, enabled=True)
            check(sys.argv[1], project, kind, enabled=False)
    print("PASS: Scratch runtime rotation/scale reaches NGX as affine geometry")

if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError,
            subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
