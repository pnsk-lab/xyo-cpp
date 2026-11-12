#!/usr/bin/env python3
"""NGX SDL pen-layer fractional sampling test on a real Scratch pen project."""
from __future__ import annotations
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

from dlss_rigid_project_smoke import build_project

def project_with_pen(path: Path) -> None:
    build_project(path)
    with ZipFile(path) as archive:
        content = {name: archive.read(name) for name in archive.namelist()}
    project=json.loads(content["project.json"])
    sprite=next(t for t in project["targets"] if not t["isStage"])
    sprite["x"]=-120
    sprite["y"]=-40
    sprite["blocks"]={
        "root":{
            "opcode":"event_whenflagclicked","next":"clear","parent":None,
            "fields":{},"inputs":{},"topLevel":True,"shadow":False,"x":0,"y":0
        },
        "clear":{
            "opcode":"pen_clear","next":"size","parent":"root",
            "fields":{},"inputs":{},"topLevel":False,"shadow":False
        },
        "size":{
            "opcode":"pen_setPenSizeTo","next":"down","parent":"clear",
            "fields":{},"inputs":{"SIZE":[1,[4,"3"]]},
            "topLevel":False,"shadow":False
        },
        "down":{
            "opcode":"pen_penDown","next":"loop","parent":"size",
            "fields":{},"inputs":{},"topLevel":False,"shadow":False
        },
        "loop":{
            "opcode":"control_forever","next":None,"parent":"down",
            "fields":{},"inputs":{"SUBSTACK":[2,"move"]},
            "topLevel":False,"shadow":False
        },
        "move":{
            "opcode":"motion_changexby","next":None,"parent":"loop",
            "fields":{},"inputs":{"DX":[1,[4,"7"]]},
            "topLevel":False,"shadow":False
        },
    }
    project["extensions"]=["pen"]
    content["project.json"]=json.dumps(project).encode()
    with ZipFile(path,"w",compression=ZIP_DEFLATED) as out:
        for name,data in content.items():
            out.writestr(name,data)

def run(binary: str, sb3: Path, path: Path, mode: str) -> tuple[bytes,str]:
    env=os.environ.copy()
    env.update({
        "XYO_EXIT_FRAMES":"14",
        "XYO_RENDER_SCALE":"0.5",
        "XYO_DLSS_REAL":"1",
        "XYO_DLSS_OPTICAL_FLOW":"1",
        "XYO_DLSS_TEMPORAL":"1",
        "XYO_DLSS_RASTER_JITTER":"1",
        "XYO_DLSS_JITTER_PEN_FILTER":"1" if mode=="linear" else "0",
        "XYO_DLSS_REAL_DEBUG":"1",
        "XYO_PEN_FSR":"0",
        "XYO_DLSS_RASTER_CAPTURE_FRAME":"5",
        "XYO_DLSS_RASTER_INPUT_CAPTURE":str(path),
    })
    p=subprocess.run([binary,"play",str(sb3),"--scale","2"],
        capture_output=True,text=True,check=False,timeout=35,env=env)
    output=p.stdout+p.stderr
    if p.returncode or "NVIDIA NGX initialized" not in output:
        raise RuntimeError(f"NGX pen playback failed:\n{output}")
    if not path.exists():
        raise RuntimeError("pen raster input missing")
    return path.read_bytes(),output

def main() -> None:
    if len(sys.argv)!=2: raise ValueError("usage: dlss_pen_jitter_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-pen-jitter-") as d:
        root=Path(d)
        sb3=root/"pen-line.sb3"
        project_with_pen(sb3)
        nearest,_=run(sys.argv[1],sb3,root/"nearest.ppm","nearest")
        filtered,log=run(sys.argv[1],sb3,root/"linear.ppm","linear")
        if "pen subpixel sampler" not in log:
            raise RuntimeError("pen texture filtering was never reached")
        changed=sum(a!=b for a,b in zip(nearest,filtered))
        print(f"jittered pen-layer linear vs default: changed RGB bytes={changed}")
        print(next(x for x in log.splitlines() if "pen subpixel sampler" in x))
        # Shader sampling may already be linear on the active SDL driver.
        # The expected requirement is an executed, controlled filter path;
        # a zero difference is only acceptable when already linear.
        if changed==0 and "already active" not in log:
            raise RuntimeError("linear pen sampler changed no input pixels")
    print("PASS: jittered Scratch pen uses controlled fractional sampling")
if __name__=="__main__":
    try:main()
    except (RuntimeError,ValueError,OSError,subprocess.TimeoutExpired) as e:
        print("FAIL:",e,file=sys.stderr);sys.exit(1)
