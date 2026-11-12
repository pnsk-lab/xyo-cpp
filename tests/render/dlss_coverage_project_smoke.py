#!/usr/bin/env python3
"""End-to-end PNG alpha-coverage validation in real NVIDIA NGX playback."""
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

from dlss_affine_project_smoke import generate

AFFINE = re.compile(r"\[real DLSS\] affine motion override active: 1 opaque quads")

def chunk(name: bytes, content: bytes) -> bytes:
    return (struct.pack(">I", len(content)) + name + content +
            struct.pack(">I", zlib.crc32(name + content) & 0xffffffff))

def build_donut(path: Path) -> None:
    generate(path, "rotation")
    with ZipFile(path) as archive:
        items = {key: archive.read(key) for key in archive.namelist()}
    project = json.loads(items["project.json"])
    sprite = next(t for t in project["targets"] if not t["isStage"])
    width = height = 64
    image = bytearray()
    for y in range(height):
        image.append(0)
        for x in range(width):
            outer = 8 <= x < 56 and 8 <= y < 56
            hole = 24 <= x < 40 and 24 <= y < 40
            a = 255 if outer and not hole else 0
            image.extend((225, 65, 40, a))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR",struct.pack(">IIBBBBB",width,height,8,6,0,0,0)) +
           chunk(b"IDAT",zlib.compress(bytes(image))) +
           chunk(b"IEND", b""))
    digest = hashlib.md5(png).hexdigest()
    md5ext = digest + ".png"
    c = sprite["costumes"][0]
    c.update(assetId=digest, md5ext=md5ext,
             bitmapResolution=1, rotationCenterX=32, rotationCenterY=32)
    sprite["costumes"] = [c]
    project["targets"][1] = sprite
    items["project.json"] = json.dumps(project).encode()
    for path_in_zip in list(items):
        if path_in_zip.endswith(".png"):
            del items[path_in_zip]
    items[md5ext] = png
    with ZipFile(path,"w",compression=ZIP_DEFLATED) as out:
        for name, data in items.items():
            out.writestr(name, data)

def verify(binary: str, sb3: Path, coverage: bool) -> None:
    env = os.environ.copy()
    env.update({
        "XYO_EXIT_FRAMES":"20",
        "XYO_DLSS_REAL":"1",
        "XYO_DLSS_OPTICAL_FLOW":"1",
        "XYO_DLSS_TEMPORAL":"1",
        "XYO_DLSS_RIGID_MOTION":"0",
        "XYO_DLSS_AFFINE_MOTION":"1",
        "XYO_DLSS_OPAQUE_COVERAGE":"1" if coverage else "0",
        "XYO_DLSS_REAL_DEBUG":"1",
        "XYO_RENDER_SCALE":"0.5",
        "XYO_PEN_FSR":"0",
    })
    result=subprocess.run(
        [binary,"play",str(sb3),"--scale","2"],env=env,
        capture_output=True,text=True,check=False,timeout=35,
    )
    output=result.stdout+result.stderr
    found=AFFINE.search(output) is not None
    ngx="NVIDIA NGX initialized" in output
    print(f"coverage={coverage}: NGX={ngx}, affine override={found}, rc={result.returncode}")
    if result.returncode or not ngx or found!=coverage:
        raise RuntimeError(f"Expected coverage={coverage}, got {found}\n{output}")

def main() -> None:
    if len(sys.argv)!=2:
        raise ValueError("usage: dlss_coverage_project_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-coverage-") as folder:
        sb3=Path(folder)/"transparent-donut-rotating.sb3"
        build_donut(sb3)
        verify(sys.argv[1],sb3,True)
        verify(sys.argv[1],sb3,False)
    print("PASS: transparent-hole costume enables motion only with opaque coverage")

if __name__=="__main__":
    try: main()
    except (RuntimeError,ValueError,OSError,subprocess.TimeoutExpired) as error:
        print("FAIL:",error,file=sys.stderr)
        sys.exit(1)
