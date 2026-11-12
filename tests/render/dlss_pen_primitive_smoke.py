#!/usr/bin/env python3
"""Run real NGX Scratch pen rerasterization and test conservative fallback."""
from __future__ import annotations
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

from dlss_pen_jitter_smoke import project_with_pen

def make_case(path: Path, case: str) -> None:
    project_with_pen(path)
    with ZipFile(path) as z:
        content={name:z.read(name) for name in z.namelist()}
    project=json.loads(content["project.json"])
    sprite=next(t for t in project["targets"] if not t["isStage"])
    blocks=sprite["blocks"]
    if case=="stamp":
        blocks["clear"]["next"]="stamp"
        blocks["stamp"]={
            "opcode":"pen_stamp","next":"size","parent":"clear",
            "fields":{},"inputs":{},"topLevel":False,"shadow":False,
        }
        blocks["size"]["parent"]="stamp"
    elif case=="uncleared":
        blocks["root"]["next"]="size"
        blocks["size"]["parent"]="root"
        del blocks["clear"]
    elif case!="replay":
        raise ValueError(case)
    content["project.json"]=json.dumps(project).encode()
    with ZipFile(path,"w",compression=ZIP_DEFLATED) as z:
        for name,data in content.items():z.writestr(name,data)

def run(binary: str, sb3: Path, image: Path, enabled: bool) -> tuple[bytes,str]:
    env=os.environ.copy()
    env.update({
        "XYO_EXIT_FRAMES":"16",
        "XYO_DLSS_REAL":"1",
        "XYO_DLSS_OPTICAL_FLOW":"1",
        "XYO_DLSS_TEMPORAL":"1",
        "XYO_DLSS_RASTER_JITTER":"1",
        "XYO_DLSS_PEN_PRIMITIVE_REPLAY":"1" if enabled else "0",
        "XYO_DLSS_JITTER_PEN_FILTER":"0",
        "XYO_DLSS_REAL_DEBUG":"1",
        "XYO_RENDER_SCALE":"0.5",
        "XYO_PEN_FSR":"0",
        "XYO_DLSS_RASTER_CAPTURE_FRAME":"5",
        "XYO_DLSS_RASTER_INPUT_CAPTURE":str(image),
    })
    result=subprocess.run(
        [binary,"play",str(sb3),"--scale","2"],
        env=env,capture_output=True,text=True,timeout=35,check=False)
    log=result.stdout+result.stderr
    if result.returncode or "NVIDIA NGX initialized" not in log:
        raise RuntimeError(f"NGX playback failed:\n{log}")
    if not image.exists():raise RuntimeError("pre-NGX input image missing")
    return image.read_bytes(),log

def main() -> None:
    if len(sys.argv)!=2:raise ValueError("usage: dlss_pen_primitive_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-pen-primitive-") as temp:
        root=Path(temp)
        for case in ("replay","stamp","uncleared"):
            sb3=root/f"{case}.sb3"
            make_case(sb3,case)
            original,baselineLog=run(sys.argv[1],sb3,root/f"{case}-normal.ppm",False)
            replayed,replayLog=run(sys.argv[1],sb3,root/f"{case}-replay.ppm",True)
            logged="pen primitive rerasterization active" in replayLog
            changed=sum(x!=y for x,y in zip(original,replayed))
            print(f"{case}: active={logged}, differing RGB bytes={changed}")
            if case=="replay":
                if not logged or changed < 50:
                    raise RuntimeError("eligible pen strokes did not rerasterize")
            elif logged or changed!=0:
                raise RuntimeError(f"unsafe pen replay used on {case}")
    print("PASS: pen stroke replay is active only with complete safe history")

if __name__=="__main__":
    try:main()
    except (RuntimeError,ValueError,OSError,subprocess.TimeoutExpired) as e:
        print("FAIL:",e,file=sys.stderr);sys.exit(1)
