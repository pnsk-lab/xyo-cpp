#!/usr/bin/env python3
"""End-to-end: Scratch runtime position->GPU rigid motion->NGX (NVIDIA GPU)."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED
import zlib

ROOT = Path(__file__).resolve().parents[2]
MATCH = re.compile(
    r"rigid motion override active: 1 opaque rectangles, "
    r"first current->previous=\((-6\.000), (0\.000)\)"
)


def chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload +
            struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff))


def build_project(path: Path) -> None:
    pixels = (b"\x00" + bytes((64, 160, 240, 255)) * 48) * 48
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", 48, 48, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))
    digest = hashlib.md5(png).hexdigest()
    filename = digest + ".png"
    with ZipFile(ROOT / "examples/fx_demo.sb3") as template:
        project = json.loads(template.read("project.json"))
        stage = next(t for t in project["targets"] if t["isStage"])
        sprite = next(t for t in project["targets"]
                      if t.get("name") == "fx-ghost")
        sprite.update(name="opaque-moving-square", x=0, y=0,
                      visible=True, size=100, currentCostume=0)
        costume = sprite["costumes"][0]
        costume.update(assetId=digest, md5ext=filename,
                       dataFormat="png", bitmapResolution=1,
                       rotationCenterX=24, rotationCenterY=24)
        sprite["costumes"] = [costume]
        # Forever loops yield at the same Scratch frame boundary used by
        # the renderer; each observed frame translates the sprite +6px.
        sprite["blocks"] = {
            "root": {
                "opcode": "event_whenflagclicked", "next": "loop",
                "parent": None, "inputs": {}, "fields": {},
                "shadow": False, "topLevel": True, "x": 0, "y": 0
            },
            "loop": {
                "opcode": "control_forever", "next": None,
                "parent": "root", "inputs": {"SUBSTACK": [2, "move"]},
                "fields": {}, "shadow": False, "topLevel": False
            },
            "move": {
                "opcode": "motion_changexby", "next": None,
                "parent": "loop", "inputs": {"DX": [1, [4, "6"]]},
                "fields": {}, "shadow": False, "topLevel": False
            }
        }
        project["targets"] = [stage, sprite]
        project["monitors"] = []
        with ZipFile(path, "w", compression=ZIP_DEFLATED) as archive:
            archive.writestr("project.json", json.dumps(project))
            for c in stage["costumes"]:
                archive.writestr(c["md5ext"], template.read(c["md5ext"]))
            archive.writestr(filename, png)


def check(binary: str, project: Path, rigid: bool) -> None:
    env = os.environ.copy()
    env.update({
        "XYO_EXIT_FRAMES": "28",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_RIGID_MOTION": "1" if rigid else "0",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_PEN_FSR": "0",
    })
    p = subprocess.run(
        [binary, "play", str(project), "--scale", "2"],
        env=env, text=True, capture_output=True,
        timeout=35, check=False,
    )
    output = p.stdout + p.stderr
    actual = MATCH.search(output) is not None
    ngx = "NVIDIA NGX initialized" in output
    if p.returncode or not ngx or actual != rigid:
        raise RuntimeError(
            f"rigid={rigid}, rc={p.returncode}, NGX={ngx}, "
            f"motion={actual}\n{output}"
        )
    print(f"rigid={rigid}: actual NVIDIA NGX, "
          f"exact (-6,0) override={'yes' if actual else 'no'}")


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("expected path to xyo CLI")
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-rigid-") as folder:
        sb3 = Path(folder) / "known-motion.sb3"
        build_project(sb3)
        check(sys.argv[1], sb3, True)
        check(sys.argv[1], sb3, False)
    print("PASS: Scratch opaque sprite moves 6px with opt-in NGX rigid motion")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError,
            subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
